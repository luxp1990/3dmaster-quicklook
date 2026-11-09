## 3dmaster-quicklook - Batch 2 Item 3 Evidence
Scope: Add an OPTIONAL memory-mapped read path for large binary mesh files (binary STL and PLY). On any failure, automatically fall back to the current chunked read path.

### Summary of Changes
- `3dmaster-preview/src/model_loader_worker.cpp`: Added mmap logic using `file.map()` inside `parseSTL` and `parsePLY`. Protected by `qEnvironmentVariableIntValue("3DMASTER_USE_MMAP") == 1`. Safe bounds checks remain, cancellation checks remain. Fallback resets state and uses existing chunked reading logic.

### Standalone Mimic Measurements (Not Production Loader)
**Model:** `large_test.stl` (2M triangles, ~100MB)
- Standard Read Path (`parse_stl_standalone`):
  - Load Time: ~118ms
  - Peak Memory: 100,992 KB
- MMAP Path (`mmap_test`):
  - Load Time: ~9ms
  - Peak Memory: 100,736 KB

**Model:** `large_test.ply` (1M vertices, 1M faces, ~25MB)
- Standard Read Path (`parse_ply_standalone`):
  - Load Time: ~27ms
  - Peak Memory: 27,776 KB
- MMAP Path (`mmap_ply_test`):
  - Load Time: ~2ms
  - Peak Memory: 27,520 KB

*Note: The before/after load-time and peak-memory numbers come from the standalone mimic apps, NOT from the production loader, and that production-path execution was NOT VERIFIED because the full Qt6/OCCT GUI build fails in the sandbox.*

### NOT VERIFIED: Corrupt File / Fallback Verification
- Corrupt/truncated STL and PLY testing with `3DMASTER_USE_MMAP=1` through the *production loader* is NOT VERIFIED.
- Forced-fallback execution (injecting `mapped = nullptr` and verifying standard path) through the *production loader* is NOT VERIFIED.
- **Reason:** The full Qt6/OCCT GUI build fails to compile in the sandbox due to missing dependencies (`Qt6::CorePrivate`, `XKB`), making it impossible to produce a working `3dmaster-preview` binary or a GUI-linked test executable to test the exact Qt `file.map()` loading execution path directly. Only code-review validation was performed to confirm fallback logic integrity.

### Repo Tests Execution
Command: `pytest test_hardening_verification.py test_cad_fuzzing_defense.py`
Exit code: 1
Pass/Fail: 4 passed, 2 failed in 0.23s

Failed tests:
- `FAILED test_hardening_verification.py::test_protocol_and_cleared`
- `FAILED test_hardening_verification.py::test_idle_timeout`

Reason for failure:
`FileNotFoundError: [Errno 2] No such file or directory: '/app/3dmaster-preview/build/3dmaster-preview'`
(The sandbox cannot build the binary executable, leading to expected IPC test failures.)

Passed tests:
- `test_cad_fuzzing_defense.py::test_path_traversal_defense`
- `test_cad_fuzzing_defense.py::test_dos_large_triangle_defense`
- `test_cad_fuzzing_defense.py::test_malformed_binary_header_defense`
- `test_cad_fuzzing_defense.py::test_malformed_ascii_float_defense`
