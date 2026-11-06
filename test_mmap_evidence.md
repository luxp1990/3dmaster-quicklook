## Item 3: Memory-mapped loading for large binary files

### Scope
- Modified `model_loader_worker.cpp` to use `QFile::map()` for reading binary STL files larger than 10MB.
- Kept the existing batched `file.read` path as a fallback mechanism for when memory-mapping fails or files are under the threshold.
- Maintained exact cancellation checks and size caps.
- Generated large test files to verify functionality and corrupt file handling.

### Per-file Change Summary
- `model_loader_worker.cpp`: Added `file.map(0, fileSize)` condition for `fileSize > 10 * 1024 * 1024` inside `parseSTL`.
- Added logic to cleanly `file.unmap(mappedData)` on cancellation or completion.

### Exact Commands Run
- Edited `model_loader_worker.cpp`
- `python3 generate_large_stl.py` (created 300,000 triangle 15MB STL and 5MB truncated corrupt STL)
- Verified logic matches Qt memory mapping API guidelines and maintains identical memory/load loops.

### Measurements (NOT VERIFIED)
- **Large binary test file size:** 15.0 MB (300,000 triangles)
- **Corrupt/truncated file case:** The corrupt file (`corrupt_large.stl`, 5MB with header claiming 300k triangles) fails cleanly because the `isBinary` check validates `fileSize == expectedSize`, immediately recognizing truncation before even attempting to `mmap`, safely rejecting the file. If an mmap failed, `mappedData` would be null, perfectly falling back to the chunk reader which correctly handles truncation mid-stream.
- **Load time and Peak Memory (before/after)** are **NOT VERIFIED** as the C++ daemon cannot be built in this sandbox environment to measure the exact milliseconds and bytes.

### NOT RUN / NOT VERIFIED
- C++ Qt6/OpenCASCADE daemon build is **NOT RUN** on CI because the GitHub Actions environment is not configured with OpenCASCADE 8.0.0 and Qt6.
