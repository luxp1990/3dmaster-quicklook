import os
import sys
import time
import subprocess
import struct

if sys.platform == "win32":
    import win32file
    import win32pipe
    import pywintypes

PIPE_NAME = r"\\.\pipe\3dmaster_quicklook"
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
if sys.platform == "win32":
    EXE_PATH = os.path.join(BASE_DIR, '3dmaster-preview', 'build', 'Release', '3dmaster-preview.exe')
else:
    EXE_PATH = os.path.join(BASE_DIR, '3dmaster-preview', 'build', '3dmaster-preview')

def verify_test_model(test_model):
    print(f"[TEST] Verifying test model: {test_model}")
    size = os.path.getsize(test_model)
    if size < 84:
        print(f"[FAIL] STL file too small: {size} bytes")
        return False

    with open(test_model, 'rb') as f:
        f.read(80) # Skip header
        num_triangles_bytes = f.read(4)
        if len(num_triangles_bytes) != 4:
            print("[FAIL] Could not read triangle count")
            return False

        num_triangles = struct.unpack('<I', num_triangles_bytes)[0]
        expected_size = 84 + (num_triangles * 50)

        if size != expected_size:
            print(f"[FAIL] STL size mismatch. Size: {size}, Expected: {expected_size}, Triangles: {num_triangles}")
            return False

        if num_triangles == 0:
            print(f"[FAIL] STL has 0 triangles")
            return False

        print(f"[PASS] Model verified. Size: {size} bytes, Triangles: {num_triangles}")
        return True

def run_smoke_test():
    test_model = os.path.abspath("test_model.stl")
    if not os.path.exists(test_model):
        print("FAIL: test_model.stl not found")
        sys.exit(1)

    if not verify_test_model(test_model):
        sys.exit(1)

    print(f"Running UI smoke test with model: {test_model}")

    if not os.path.exists(EXE_PATH):
        print(f"NOT RUN: executable not found at {EXE_PATH}. Cannot run headless preview.")
        sys.exit(0)

    env = os.environ.copy()

    # Launch preview
    print("Launching preview...")
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
        sys.exit(1)

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
        # Load the real test model
        send_cmd(f"LOAD 1 {test_model}")
        loaded = recv_line(5.0)

        if not loaded or "LOADED 1 OK" not in loaded:
            print(f"FAIL: Expected LOADED 1 OK, got {loaded}")
            sys.exit(1)

        print("PASS: Model loaded successfully.")

        # Test QUIT
        send_cmd("QUIT")
        proc.wait(timeout=3.0)
        print("PASS: Clean QUIT test passed! Process exited code:", proc.returncode)

    except Exception as e:
        print(f"FAIL: Exception: {e}")
        proc.kill()
        sys.exit(1)
    finally:
        try:
            win32file.CloseHandle(handle)
        except:
            pass
        if proc.poll() is None:
            proc.kill()

    print("PASS: Smoke test completed successfully")
    sys.exit(0)

if __name__ == "__main__":
    run_smoke_test()
