using System;
using System.IO;
using System.Linq;
using System.Windows;
using QuickLook.Common.Plugin;

namespace QuickLook.Plugin.ThreeDMaster
{
    public class Plugin : IViewer
    {
        private static readonly string[] SupportedExtensions = new[]
        {
            ".step", ".stp",
            ".iges", ".igs",
            ".gltf", ".glb",
            ".3mf",
            ".stl",
            ".obj",
            ".ply",
            ".off",
            ".prt"
        };

        private Model3DViewerHost _host;
        private Action<long, bool, string> _loadCallback;

        public int Priority => 10;

        public void Init()
        {
            // Background pre-warm daemon connection
            System.Threading.Tasks.Task.Run(() =>
            {
                try
                {
                    DaemonClient.EnsureConnected();
                }
                catch { }
            });
        }

        public bool CanHandle(string path)
        {
            if (Directory.Exists(path))
                return false;

            string ext = Path.GetExtension(path)?.ToLowerInvariant();
            if (string.IsNullOrEmpty(ext))
                return false;

            return SupportedExtensions.Contains(ext);
        }

        public void Prepare(string path, ContextObject context)
        {
            // 0ms 纯数学屏幕自适应计算，严禁发起阻塞式 IPC 查询
            context.SetPreferredSizeFit(new Size { Width = 1200, Height = 800 }, 0.85d);
        }

        public void View(string path, ContextObject context)
        {
            try
            {
                DaemonClient.EnsureConnected();

                _host = new Model3DViewerHost
                {
                    HorizontalAlignment = HorizontalAlignment.Stretch,
                    VerticalAlignment = VerticalAlignment.Stretch
                };
                context.ViewerContent = _host;
                context.Title = Path.GetFileName(path);

                long genId = DaemonClient.NextGenerationId();
                bool callbackFired = false;

                _loadCallback = (finishedGenId, ok, errMsg) =>
                {
                    if (finishedGenId != genId) return;
                    callbackFired = true;

                    var disp = _host?.Dispatcher ?? Application.Current?.Dispatcher;
                    if (disp != null && !disp.HasShutdownStarted)
                    {
                        disp.BeginInvoke(new Action(() =>
                        {
                            context.IsBusy = false;
                            if (!ok && !string.IsNullOrEmpty(errMsg))
                            {
                                context.Title = $"{Path.GetFileName(path)} ({errMsg})";
                            }
                        }));
                    }
                    else
                    {
                        context.IsBusy = false;
                    }
                };

                DaemonClient.ModelLoadFinished += _loadCallback;
                DaemonClient.SendLoad(path, genId);

                // 5秒守护进程状态看门狗：防止后台异常退出导致前台无限转圈
                System.Threading.Tasks.Task.Delay(5000).ContinueWith(t =>
                {
                    if (!callbackFired && DaemonClient.State == DaemonConnectionState.Dead)
                    {
                        var disp = _host?.Dispatcher ?? Application.Current?.Dispatcher;
                        disp?.BeginInvoke(new Action(() =>
                        {
                            context.IsBusy = false;
                            context.Title = $"{Path.GetFileName(path)} (3dmaster 预览引擎启动失败，请检查运行库)";
                        }));
                    }
                });
            }
            catch (Exception ex)
            {
                context.ViewerContent = new System.Windows.Controls.Label
                {
                    Content = $"3dmaster 启动异常: {ex.Message}",
                    HorizontalAlignment = HorizontalAlignment.Center,
                    VerticalAlignment = VerticalAlignment.Center
                };
                context.IsBusy = false;
            }
        }

        public void Cleanup()
        {
            if (_loadCallback != null)
            {
                DaemonClient.ModelLoadFinished -= _loadCallback;
                _loadCallback = null;
            }

            _host?.Dispose();
            _host = null;

            DaemonClient.SendClear();
        }
    }
}
