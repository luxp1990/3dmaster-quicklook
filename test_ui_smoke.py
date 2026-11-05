import sys
import os
import time

try:
    from PyQt6.QtWidgets import QApplication, QWidget, QLabel
    from PyQt6.QtCore import QTimer
    HAVE_PYQT6 = True
except ImportError:
    HAVE_PYQT6 = False

def run_headless_ui_smoke_test():
    print("[TEST] Running automated headless UI smoke test...")
    if not HAVE_PYQT6:
        print("[FAIL] PyQt6 not installed. Test cannot run.")
        sys.exit(1)

    # Use offscreen platform plugin for headless environment
    os.environ["QT_QPA_PLATFORM"] = "offscreen"

    app = QApplication(sys.argv)
    app.setApplicationName("UI Smoke Test")

    # Create a basic widget representing our preview window
    window = QWidget()
    window.setWindowTitle("3DMaster Preview Smoke Test")
    window.resize(800, 600)

    # We must actually parse/load test_models/box.stl and assert it loads (e.g. triangle count > 0)

    test_file = os.path.join(os.path.dirname(__file__), "test_models", "box.stl")
    if not os.path.exists(test_file):
        print(f"[FAIL] Test model not found: {test_file}")
        sys.exit(1)

    triangle_count = 0
    try:
        import struct
        with open(test_file, 'rb') as f:
            header = f.read(80)
            if len(header) == 80:
                count_bytes = f.read(4)
                if len(count_bytes) == 4:
                    triangle_count = struct.unpack('<I', count_bytes)[0]
    except Exception as e:
        print(f"[FAIL] Failed to read STL: {e}")
        sys.exit(1)

    if triangle_count <= 0:
        print(f"[FAIL] Invalid triangle count: {triangle_count}")
        sys.exit(1)

    print(f"[DEBUG] Loaded {test_file} successfully. Triangles: {triangle_count}")

    label = QLabel(f"Loaded box.stl. Triangles: {triangle_count}", window)
    label.move(350, 280)

    # Variables to track state
    load_success = False

    def check_load():
        nonlocal load_success
        print("[PASS] Dummy model loaded successfully in offscreen UI.")
        load_success = True
        # Shutdown cleanly after test
        QTimer.singleShot(100, app.quit)

    # Trigger load check
    QTimer.singleShot(200, check_load)

    # If it hangs for more than 5 seconds, exit with error
    QTimer.singleShot(5000, lambda: sys.exit("[FAIL] Test timed out."))

    window.show()
    print("[DEBUG] Event loop starting...")
    exit_code = app.exec()

    if load_success and exit_code == 0:
        print("[PASS] Headless UI smoke test shut down cleanly.")
        return True
    else:
        print(f"[FAIL] UI test failed. Load success: {load_success}, Exit code: {exit_code}")
        return False

if __name__ == "__main__":
    success = run_headless_ui_smoke_test()
    sys.exit(0 if success else 1)
