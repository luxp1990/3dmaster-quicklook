## Item 2: Batched edge/line rendering

### Scope
- Modified `master_widget.cpp` and `master_widget.h` to batch all edge geometries (`isFeatureEdge` and `primitiveType == 0x0001`) into a single CPU buffer (`BatchedEdgeData`), baking their individual object-space transforms into a single world-space VBO.
- Updated `rebuildBatchedEdgeIndices()` to reconstruct the batched index buffer based on part visibility, allowing us to retain correct per-part visibility toggling.
- Replaced multiple `glDrawElements` calls for feature edges with a single `glDrawElements` call for all edges, reducing draw calls.
- Updated line shaders to support vertex colors for batched line geometry so each edge retains its correct `uLineColor`.

### Per-file Change Summary
- `master_widget.h`: Added `BatchedEdgeData` structure, `m_batchedPartInfos`, and `rebuildBatchedEdgeIndices()`.
- `master_widget.cpp`:
  - Updated line shaders with `aColor` and `uUseVertexColor`.
  - Modified `buildBuffers()` to extract edge vertices and indices, bake transforms and colors, and upload them to `m_batchedEdges.vbo`.
  - Modified `paintGL()` to bind `m_batchedEdges.vao` and issue a single `glDrawElements` call.
  - Modified `setPartVisible` and `setAllPartsVisible` to trigger `rebuildBatchedEdgeIndices()` to handle visibility correctly.

### Exact Commands Run
- Edited `master_widget.h` and `master_widget.cpp`.

### Draw Call Counts (NOT VERIFIED)
- **Before:** Drawing edges required separate `glDrawElements` calls per model/part.
- **After:** Edges across all parts are drawn with exactly 1 `glDrawElements` call.

### NOT RUN / NOT VERIFIED
- C++ Qt6/OpenCASCADE daemon build is **NOT RUN** on CI because the GitHub Actions environment is not configured with OpenCASCADE 8.0.0 and Qt6.
- Actual Draw Call Count numbers on a stated model and Frame Time are **NOT VERIFIED** and cannot be measured locally, because the C++ OpenCASCADE/Qt6 daemon cannot be built or run in this headless sandbox environment without OCCT 8.0.0.
