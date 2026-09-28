using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace QuickLook.Plugin.ThreeDMaster
{
    public enum DaemonConnectionState
    {
        Dead,
        Reconnecting,
        Connected
    }

    public static class DaemonClient
    {
        private const string PipeName = "3dmaster_quicklook";
        private static readonly object _syncLock = new object();

        private static DaemonConnectionState _state = DaemonConnectionState.Dead;
        private static Process _daemonProcess;
        private static NamedPipeClientStream _pipeStream;
        private static StreamWriter _writer;
        private static StreamReader _reader;
        private static CancellationTokenSource _cts;
        private static int _pipeGeneration;
        private static long _generationCounter;

        // 单飞重连控制
        private static Task _reconnectTask;
        private static readonly object _reconnectLock = new object();

        // 会话恢复意图缓存（锁内更新）
        private static IntPtr _lastAttachHwnd = IntPtr.Zero;
        private static string _lastLoadPath = null;
        private static long _lastLoadGenId = 0;
        private static int _lastResizeW = 0;
        private static int _lastResizeH = 0;

        // CLEARED 同步回执事件
        private static ManualResetEventSlim _clearedEvent;

        public static event Action<long, bool, string> ModelLoadFinished;

        public static DaemonConnectionState State
        {
            get
            {
                lock (_syncLock) return _state;
            }
        }

        public static long NextGenerationId()
        {
            return Interlocked.Increment(ref _generationCounter);
        }

        public static void EnsureConnected()
        {
            lock (_syncLock)
            {
                if (_state == DaemonConnectionState.Connected && _pipeStream != null && _pipeStream.IsConnected)
                    return;

                TriggerReconnect_Locked();
            }
        }

        private static void MarkDeadCore_Locked(string reason)
        {
            Debug.WriteLine($"[DaemonClient] MarkDeadCore: {reason}");
            _state = DaemonConnectionState.Dead;

            try { _cts?.Cancel(); } catch { }
            try { _writer?.Dispose(); } catch { }
            try { _reader?.Dispose(); } catch { }
            try { _pipeStream?.Dispose(); } catch { }

            _writer = null;
            _reader = null;
            _pipeStream = null;
            _cts = null;
        }

        private static void TriggerReconnect_Locked()
        {
            if (_state == DaemonConnectionState.Reconnecting)
                return;

            _state = DaemonConnectionState.Reconnecting;
            int targetGen = unchecked(++_pipeGeneration);

            lock (_reconnectLock)
            {
                if (_reconnectTask == null || _reconnectTask.IsCompleted)
                {
                    _reconnectTask = Task.Run(() => DoReconnectWork(targetGen));
                }
            }
        }

        private static void DoReconnectWork(int currentGen)
        {
            Process oldProc = null;
            lock (_syncLock)
            {
                oldProc = _daemonProcess;
                _daemonProcess = null;
            }

            // 锁外安全终止残留进程
            if (oldProc != null)
            {
                try
                {
                    if (!oldProc.HasExited)
                    {
                        oldProc.Kill();
                        oldProc.WaitForExit(500);
                    }
                    oldProc.Dispose();
                }
                catch { }
            }

            // 锁外启动守护进程并重连管道（最多 5 次快速重试，单次间隔 50ms）
            NamedPipeClientStream newStream = null;
            StreamWriter newWriter = null;
            StreamReader newReader = null;
            CancellationTokenSource newCts = null;
            Process newProc = null;
            bool connectSuccess = false;

            for (int attempt = 1; attempt <= 3; attempt++)
            {
                try
                {
                    newProc = StartDaemonProcess();

                    for (int retry = 0; retry < 6; retry++)
                    {
                        try
                        {
                            newStream = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
                            newStream.Connect(500);
                            newWriter = new StreamWriter(newStream, new UTF8Encoding(false)) { AutoFlush = true };
                            newReader = new StreamReader(newStream, Encoding.UTF8);
                            newCts = new CancellationTokenSource();
                            connectSuccess = true;
                            break;
                        }
                        catch
                        {
                            newStream?.Dispose();
                            newStream = null;
                            Thread.Sleep(60);
                        }
                    }

                    if (connectSuccess) break;
                }
                catch (Exception ex)
                {
                    Debug.WriteLine($"[DaemonClient] Reconnect attempt {attempt} failed: {ex.Message}");
                    Thread.Sleep(150);
                }
            }

            if (!connectSuccess)
            {
                lock (_syncLock)
                {
                    if (_pipeGeneration == currentGen)
                    {
                        MarkDeadCore_Locked("Failed to reconnect after retries");
                    }
                }
                return;
            }

            // 连接成功，回填状态并重放上下文意图
            IntPtr replayAttach = IntPtr.Zero;
            string replayLoadPath = null;
            long replayLoadGen = 0;
            int replayW = 0, replayH = 0;

            lock (_syncLock)
            {
                if (_pipeGeneration != currentGen)
                {
                    // 已有更新的世代启动，丢弃当前连接
                    try { newStream.Dispose(); } catch { }
                    return;
                }

                _daemonProcess = newProc;
                _pipeStream = newStream;
                _writer = newWriter;
                _reader = newReader;
                _cts = newCts;
                _state = DaemonConnectionState.Connected;

                // 提取需要重放的意图
                replayAttach = _lastAttachHwnd;
                replayLoadPath = _lastLoadPath;
                replayLoadGen = _lastLoadGenId;
                replayW = _lastResizeW;
                replayH = _lastResizeH;

                Task.Run(() => ReadLoop(newReader, currentGen, newCts.Token));
            }

            // 锁外按顺序重放最后状态
            try
            {
                if (replayAttach != IntPtr.Zero)
                {
                    newWriter.WriteLine($"ATTACH 0x{replayAttach.ToInt64():X}");
                }
                if (!string.IsNullOrEmpty(replayLoadPath))
                {
                    newWriter.WriteLine($"LOAD {replayLoadGen} {replayLoadPath}");
                }
                if (replayW > 0 && replayH > 0)
                {
                    newWriter.WriteLine($"RESIZE {replayW} {replayH}");
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[DaemonClient] Error replaying intent: {ex.Message}");
                lock (_syncLock)
                {
                    if (_pipeGeneration == currentGen)
                    {
                        MarkDeadCore_Locked("Replay intent exception");
                    }
                }
            }
        }

        private static Process StartDaemonProcess()
        {
            string exePath = ResolveExecutablePath();
            if (!File.Exists(exePath))
            {
                throw new FileNotFoundException($"Cannot locate 3dmaster-preview.exe at '{exePath}'");
            }

            var psi = new ProcessStartInfo
            {
                FileName = exePath,
                Arguments = $"--pipe {PipeName}",
                WorkingDirectory = Path.GetDirectoryName(exePath),
                UseShellExecute = false,
                CreateNoWindow = true
            };

            string exeDir = Path.GetDirectoryName(exePath);
            string curPath = Environment.GetEnvironmentVariable("PATH") ?? "";
            psi.EnvironmentVariables["PATH"] = $"{exeDir};{Path.Combine(exeDir, "platforms")};{curPath}";
            psi.EnvironmentVariables["QT_QPA_PLATFORM_PLUGIN_PATH"] = Path.Combine(exeDir, "platforms");

            return Process.Start(psi);
        }

        private static string ResolveExecutablePath()
        {
            string pluginDir = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            string localExe = Path.Combine(pluginDir, "3dmaster-preview.exe");
            if (File.Exists(localExe))
                return localExe;

            string subExe = Path.Combine(pluginDir, "3dmaster-native", "3dmaster-preview.exe");
            if (File.Exists(subExe))
                return subExe;

            string devFallbackExe = @"D:\seer\3dmaster\3dmaster-quicklook\3dmaster-preview\build\Release\3dmaster-preview.exe";
            if (File.Exists(devFallbackExe))
                return devFallbackExe;

            return localExe;
        }

        public static void SendAttach(IntPtr hwnd)
        {
            lock (_syncLock)
            {
                _lastAttachHwnd = hwnd;
            }
            SendCommand($"ATTACH 0x{hwnd.ToInt64():X}");
        }

        public static void SendLoad(string filePath, long generationId)
        {
            lock (_syncLock)
            {
                _lastLoadPath = filePath;
                _lastLoadGenId = generationId;
            }
            SendCommand($"LOAD {generationId} {filePath}");
        }

        public static void SendResize(int width, int height)
        {
            lock (_syncLock)
            {
                _lastResizeW = width;
                _lastResizeH = height;
            }
            SendCommand($"RESIZE {width} {height}");
        }

        public static void SendClear()
        {
            lock (_syncLock)
            {
                _lastAttachHwnd = IntPtr.Zero;
                _lastLoadPath = null;
                _lastLoadGenId = 0;
            }
            SendCommand("CLEAR");
        }

        public static bool SendClearWait(int timeoutMs = 200)
        {
            ManualResetEventSlim ev = null;
            lock (_syncLock)
            {
                _lastAttachHwnd = IntPtr.Zero;
                _lastLoadPath = null;
                _lastLoadGenId = 0;
                _clearedEvent?.Dispose();
                _clearedEvent = new ManualResetEventSlim(false);
                ev = _clearedEvent;
            }

            SendCommand("CLEAR");

            if (ev != null)
            {
                return ev.Wait(timeoutMs);
            }
            return false;
        }

        public static void SendPing()
        {
            SendCommand("PING");
        }

        private static void SendCommand(string cmd)
        {
            lock (_syncLock)
            {
                if (_state == DaemonConnectionState.Connected && _writer != null && _pipeStream != null && _pipeStream.IsConnected)
                {
                    try
                    {
                        _writer.WriteLine(cmd);
                        return;
                    }
                    catch (Exception ex)
                    {
                        Debug.WriteLine($"[DaemonClient] SendCommand write error: {ex.Message}");
                        MarkDeadCore_Locked(ex.Message);
                    }
                }

                // 未连接或写失败，触发单飞重连
                TriggerReconnect_Locked();
            }
        }

        private static async Task ReadLoop(StreamReader reader, int boundGeneration, CancellationToken ct)
        {
            try
            {
                while (!ct.IsCancellationRequested && reader != null)
                {
                    string line = await reader.ReadLineAsync().ConfigureAwait(false);
                    if (line == null) break; // EOF 管道断开

                    line = line.Trim();
                    if (string.IsNullOrEmpty(line)) continue;

                    string[] parts = line.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                    if (parts.Length == 0) continue;

                    string cmd = parts[0].ToUpperInvariant();

                    if (cmd == "CLEARED")
                    {
                        lock (_syncLock)
                        {
                            _clearedEvent?.Set();
                        }
                    }
                    else if (cmd == "LOADED" && parts.Length >= 3)
                    {
                        if (long.TryParse(parts[1], out long genId))
                        {
                            bool isLatestGen = true;
                            lock (_syncLock)
                            {
                                // 世代保护：若已被更新的 LOAD 覆盖，直接丢弃
                                if (_lastLoadGenId != 0 && genId != _lastLoadGenId)
                                {
                                    isLatestGen = false;
                                }
                            }

                            if (isLatestGen)
                            {
                                bool ok = parts[2].Equals("OK", StringComparison.OrdinalIgnoreCase);
                                string err = (!ok && parts.Length > 3) ? string.Join(" ", parts, 3, parts.Length - 3) : null;
                                // 锁外派发回调，杜绝死锁
                                Task.Run(() => ModelLoadFinished?.Invoke(genId, ok, err));
                            }
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"[DaemonClient] ReadLoop error: {ex.Message}");
            }
            finally
            {
                lock (_syncLock)
                {
                    // 仅当世代匹配时标记为 Dead，防止旧循环晚退误杀新连接
                    if (_pipeGeneration == boundGeneration && _state == DaemonConnectionState.Connected)
                    {
                        MarkDeadCore_Locked("ReadLoop EOF or disconnected");
                    }
                }
            }
        }

        public static void KillProcess()
        {
            Process procToKill = null;
            lock (_syncLock)
            {
                MarkDeadCore_Locked("Explicit KillProcess");
                procToKill = _daemonProcess;
                _daemonProcess = null;
                _lastAttachHwnd = IntPtr.Zero;
                _lastLoadPath = null;
            }

            if (procToKill != null)
            {
                try
                {
                    if (!procToKill.HasExited)
                    {
                        procToKill.Kill();
                        procToKill.WaitForExit(500);
                    }
                    procToKill.Dispose();
                }
                catch { }
            }
        }
    }
}
