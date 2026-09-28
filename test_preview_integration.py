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

def run_test():
    print("[TEST] 1. Launching 3dmaster-preview.exe ...")
    env = os.environ.copy()
    qt_bin = r"D:\seer\3dmaster\qt6\6.8.2\msvc2022_64\bin"
    occt_bin = r"D:\seer\3dmaster\occt\win64\vc14\bin"
    orig_dir = r"D:\seer\3dmaster\3dmaster"
    env["PATH"] = f"{qt_bin};{occt_bin};{orig_dir};" + env.get("PATH", "")

    proc = subprocess.Popen([EXE_PATH, "--pipe", "3dmaster_quicklook"],
                            cwd=os.path.dirname(EXE_PATH),
                            env=env)
    
    # Wait for pipe to be ready
    connected = False
    handle = None
    for attempt in range(20):
        time.sleep(0.2)
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
            connected = True
            print(f"[TEST] Connected to pipe on attempt {attempt+1}!")
            break
        except Exception as e:
            pass

    if not connected or not handle:
        print("[FAIL] Could not connect to named pipe within 4 seconds!")
        proc.kill()
        return False

    def send_cmd(cmd_str):
        print(f"[IPC >>] {cmd_str.strip()}")
        win32file.WriteFile(handle, (cmd_str.strip() + "\n").encode("utf-8"))

    def recv_line(timeout_sec=5.0):
        start = time.time()
        buf = b""
        while time.time() - start < timeout_sec:
            hr, data = win32file.ReadFile(handle, 1024)
            if data:
                buf += data
                if b"\n" in buf:
                    line = buf.split(b"\n")[0].decode("utf-8").strip()
                    print(f"[IPC <<] {line}")
                    return line
            time.sleep(0.05)
        return None

    try:
        # Test PING
        send_cmd("PING")
        pong = recv_line(3.0)
        assert pong == "PONG", f"Expected PONG, got {pong}"
        print("[PASS] PING/PONG test passed!")

        # Test ATTACH
        send_cmd("ATTACH 0")
        att = recv_line(3.0)
        assert att == "ATTACHED", f"Expected ATTACHED, got {att}"
        print("[PASS] ATTACH test passed!")

        # Test LOAD GLB
        send_cmd(f"LOAD 101 {TEST_MODEL}")
        loaded = recv_line(5.0)
        assert loaded and "LOADED 101 OK" in loaded, f"Expected LOADED 101 OK, got {loaded}"
        print("[PASS] Model Load test passed!")

        # Test Rapid Switching / Cancellation: Load heavy STEP then immediately override with GLB
        print("[TEST] Rapid file switching (Generation cancellation test) ...")
        send_cmd(f"LOAD 201 {TEST_STEP}")
        time.sleep(0.05) # Rapid switch while STEP is parsing
        send_cmd(f"LOAD 202 {TEST_MODEL}")
        loaded = recv_line(8.0)
        assert loaded and "LOADED 202 OK" in loaded, f"Expected LOADED 202 OK, got {loaded}"
        print("[PASS] Rapid generation cancellation verified successfully!")

        # Test CLEAR
        send_cmd("CLEAR")
        print("[PASS] CLEAR sent!")

        # Test QUIT
        send_cmd("QUIT")
        proc.wait(timeout=3.0)
        print("[PASS] Clean QUIT test passed! Process exited code:", proc.returncode)

        print("\nALL INTEGRATION TESTS PASSED (100% SUCCESS)!")
        return True

    except Exception as e:
        print(f"[ERROR] Test failed with exception: {e}")
        proc.kill()
        return False
    finally:
        try:
            win32file.CloseHandle(handle)
        except:
            pass
        if proc.poll() is None:
            proc.kill()

if __name__ == "__main__":
    success = run_test()
    sys.exit(0 if success else 1)
