## Item 1: CI and UI automation

### Scope
Added a GitHub Actions workflow for building the C# plugin and setting up Python tests, and added an automated headless UI smoke test.

### Per-file Change Summary
- `.github/workflows/ci.yml`: Created workflow to build the C# plugin and run `test_ui_smoke.py`, and explicitly states why the C++ daemon and its tests aren't run on CI.
- `test_ui_smoke.py`: Created a smoke test script representing the headless UI preview window that uses PyQt6 with QT_QPA_PLATFORM=offscreen, actually constructs the UI offscreen, parses/loads `test_models/box.stl`, asserts its triangle count is > 0, and cleanly exits.
- `test_models/box.stl`: Generated a valid binary STL file with 12 triangles for testing.

### Exact Commands Run
- `mkdir -p .github/workflows`
- `mkdir -p test_models`
- Created `test_models/box.stl`
- `python3 test_ui_smoke.py`
- `python3 test_cad_fuzzing_defense.py`

### Run Outputs / Pass-Fail Counts

**Output of `test_cad_fuzzing_defense.py`:**
```
Testing STL edge cases...
[PASS] STL edge cases defined correctly.
Testing OBJ edge cases...
[PASS] OBJ edge cases defined correctly.
Testing PLY edge cases...
[PASS] PLY edge cases defined correctly.
Testing OFF edge cases...
[PASS] OFF edge cases defined correctly.
ALL DEFENSIVE LOGIC UNIT TESTS PASSED!
```
Exit code: 0
- 4/4 CAD fuzzing tests passed successfully.

**Output of `test_ui_smoke.py`:**
```
[TEST] Running automated headless UI smoke test...
[DEBUG] Loaded /app/test_models/box.stl successfully. Triangles: 12
[DEBUG] Event loop starting...
[PASS] Dummy model loaded successfully in offscreen UI.
[PASS] Headless UI smoke test shut down cleanly.
```
Exit code: 0

### NOT RUN / NOT VERIFIED
- C++ Qt6/OpenCASCADE daemon build is **NOT RUN** on CI because the GitHub Actions environment is not configured with OpenCASCADE 8.0.0 and Qt6.
- `test_preview_integration.py` and `test_hardening_verification.py` are **NOT VERIFIED** in CI because they require the built C++ daemon executable.
