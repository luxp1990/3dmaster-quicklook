import os
import sys
import time
import subprocess

if sys.platform == "win32":
    import win32file
    import win32pipe
    import pywintypes

PIPE_NAME = r"\\.\pipe\3dmaster_quicklook"
EXE_PATH = r"D:\seer\3dmaster\3dmaster-quicklook\3dmaster-preview\build\Release\3dmaster-preview.exe"
TEST_MODEL = r"D:\seer\3dmaster\test_models\box.glb"
TEST_STEP = r"D:\seer\3dmaster\test_models\main_assembly.stp"

def get_env():
    env = os.environ.copy()
    qt_bin = r"D:\seer\3dmaster\qt6\6.8.2\msvc2022_64\bin"
    occt_bin = r"D:\seer\3dmaster\occt\win64\vc14\bin"
    orig_dir = r"D:\seer\3dmaster\3dmaster"
    env["PATH"] = f"{qt_bin};{occt_bin};{orig_dir};" + env.get("PATH", "")
    return env

def connect_pipe(timeout_sec=5.0):
    start = time.time()
    while time.time() - start < timeout_sec:
        try:
            handle = win32file.CreateFile(
                PIPE_NAME,
                win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                0,
                None,
                win32file.OPEN_EXISTING,
                0,
                None
            )
            return handle
        except Exception:
            time.sleep(0.1)
    return None

def send_cmd(handle, cmd_str):
    print(f"[IPC >>] {cmd_str.strip()}")
    win32file.WriteFile(handle, (cmd_str.strip() + "\n").encode("utf-8"))

def recv_line(handle, timeout_sec=5.0):
    start = time.time()
    buf = b""
    while time.time() - start < timeout_sec:
        try:
            hr, data = win32file.ReadFile(handle, 1024)
            if data:
                buf += data
                if b"\n" in buf:
                    line = buf.split(b"\n")[0].decode("utf-8").strip()
                    print(f"[IPC <<] {line}")
                    return line
        except Exception:
            break
        time.sleep(0.05)
    return None

def test_protocol_and_cleared():
    print("\n--- TEST 1: Protocol, CLEARED handshake, and Cancellation ---")
    proc = subprocess.Popen([EXE_PATH, "--pipe", "3dmaster_quicklook"],
                            cwd=os.path.dirname(EXE_PATH), env=get_env())
    handle = connect_pipe(4.0)
    assert handle is not None, "Failed to connect to pipe"

    try:
        send_cmd(handle, "PING")
        assert recv_line(handle, 3.0) == "PONG"
        print("[PASS] PING/PONG")

        send_cmd(handle, "ATTACH 0")
        assert recv_line(handle, 3.0) == "ATTACHED"
        print("[PASS] ATTACH")

        send_cmd(handle, f"LOAD 101 {TEST_MODEL}")
        loaded = recv_line(handle, 5.0)
        assert loaded and "LOADED 101 OK" in loaded
        print("[PASS] LOAD")

        # Test CLEAR -> CLEARED handshake
        send_cmd(handle, "CLEAR")
        cleared = recv_line(handle, 3.0)
        assert cleared == "CLEARED", f"Expected CLEARED, got {cleared}"
        print("[PASS] CLEAR -> CLEARED handshake verified!")

        send_cmd(handle, "QUIT")
        proc.wait(timeout=3.0)
        assert proc.returncode == 0
        print("[PASS] QUIT clean exit (code 0)")
        return True
    finally:
        try: win32file.CloseHandle(handle)
        except: pass
        if proc.poll() is None: proc.kill()

def test_idle_timeout():
    print("\n--- TEST 2: Daemon Idle Auto-Exit (Step 4) ---")
    # Launch with 3 seconds idle timeout
    proc = subprocess.Popen([EXE_PATH, "--pipe", "3dmaster_quicklook", "--idle-timeout", "3"],
                            cwd=os.path.dirname(EXE_PATH), env=get_env())
    handle = connect_pipe(4.0)
    assert handle is not None, "Failed to connect to pipe"

    try:
        send_cmd(handle, "PING")
        assert recv_line(handle, 3.0) == "PONG"
        print("[PASS] Pipe alive")

        # No ATTACH, or send CLEAR to ensure unattached
        send_cmd(handle, "CLEAR")
        assert recv_line(handle, 3.0) == "CLEARED"

        # Disconnect client handle
        win32file.CloseHandle(handle)
        handle = None
        print("[TEST] Client disconnected. Waiting 5s for idle auto-exit (timeout set to 3s)...")

        # Wait for daemon to auto-quit
        proc.wait(timeout=8.0)
        print(f"[PASS] Daemon automatically exited on idle! Returncode: {proc.returncode}")
        return True
    finally:
        if handle:
            try: win32file.CloseHandle(handle)
            except: pass
        if proc.poll() is None:
            proc.kill()

def main():
    t1 = test_protocol_and_cleared()
    t2 = test_idle_timeout()

    if t1 and t2:
        print("\n==============================================")
        print("ALL HARDENING TESTS PASSED SUCCESSFULLY! (100%)")
        print("==============================================")
        sys.exit(0)
    else:
        print("\nSOME TESTS FAILED!")
        sys.exit(1)

if __name__ == "__main__":
    main()
