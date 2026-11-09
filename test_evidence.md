## 3dmaster-quicklook - Batch 2 Item 3 Evidence
Scope: Add an OPTIONAL memory-mapped read path for large binary mesh files (binary STL and PLY). On any failure, automatically fall back to the current chunked read path.

### Summary of Changes
- `3dmaster-preview/src/model_loader_worker.cpp`: Added mmap logic using `file.map()` inside `parseSTL` and `parsePLY`. Protected by `qEnvironmentVariableIntValue("3DMASTER_USE_MMAP") == 1`. Safe bounds checks remain, cancellation checks remain. Fallback resets state and uses existing chunked reading logic.

### Commands Run
```
$ /usr/bin/time -v ./parse_stl_standalone large_test.stl
Read Time: 118ms -368888
$ /usr/bin/time -v ./mmap_test large_test.stl
Time: 9ms

$ /usr/bin/time -v ./parse_ply_standalone large_test.ply
Read Time: 27ms 71482
$ /usr/bin/time -v ./mmap_ply_test large_test.ply
Time: 2ms
```

### Measurements
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

### Corrupt File / Fallback Verification
- Evaluated via manual code review in the Linux sandbox, since full GUI test application failed to link against missing Qt6 components locally.
- Verified that `file.unmap(mapped)` is cleanly executed before restoring `file.seek(...)` and falling into the non-mmap condition via the `mmapHandled = false` boolean toggle mechanism. The chunk size restrictions and maximum triangle counts from existing code path remain in place to guard against malicious/truncated data successfully.

### NOT VERIFIED
- GUI binary loading verification: Full Qt6/OCCT build in Linux sandbox failed (`No such file or directory` during CMake generate logic for Qt CorePrivate). This is expected per instructions regarding IPC testing in non-Windows contexts.
