## Item 2 - Batched Edge/Line Rendering

### Scope
Changed the preview renderer so edges for a model are accumulated into batched geometry (a single merged line-segments buffer per model/part) and drawn with as few draw calls as practical (1 call), without changing how the result looks. If part visibility changes, the batched buffer is updated dynamically.

### Per-File Summary
- `3dmaster-preview/include/master_widget.h`
  - Added variables for tracking the batched edge buffer (`m_batchedEdgeVao`, `m_batchedEdgeVbo`, `m_batchedEdgeIbo`, `m_batchedEdgeIndexCount`, `m_batchedEdgeBufferDirty`).
  - Added `updateBatchedEdgeBuffer` method.
  - Added `srcMesh` pointer to `GLMesh` so original vertices are accessible when batching.

- `3dmaster-preview/src/master_widget.cpp`
  - `buildBuffers`: Skips individual VBO/IBO creation for edges (`primitiveType == 0x0001` or `isFeatureEdge`). Marks the batched edge buffer as dirty.
  - `updateBatchedEdgeBuffer`: Iterates all visible feature edge meshes, transforms their vertices by their individual model matrices into a single contiguous vertex buffer, and assigns the correct color to the vertices.
  - `setPartVisible` / `setAllPartsVisible`: Marks the batched buffer as dirty when visibility state changes.
  - `initShaders`: Modified `m_lineProgram` (both vertex and fragment shaders) to accept vertex color (`aCol`) and blend it with `uLineColor`.
  - `paintGL`: Replaced the loop that drew each edge mesh separately with a single `glDrawElements` call of the batched buffer. Non-batched lines (axis, bounding box) were updated to handle the new `aCol` attribute by providing a default white color.

### Exact Commands Run
1. `cat 3dmaster-preview/src/master_widget.cpp | grep -i draw -B 15 -A 40` (to inspect existing draw calls)
2. `cat 3dmaster-preview/src/model_loader_worker.cpp | grep -n -A 60 "edgeMesh.name"` (to verify how feature edges are identified/created)
3. Python script created to test batched vertex logic over STL structure.
4. `mkdir -p 3dmaster-preview/build && cd 3dmaster-preview/build && cmake .. && make -j$(nproc)` (Sandbox compilation - NOT VERIFIED, see below)

### Pass/Fail Counts, Exit Codes
- C++ Compilation in Sandbox: FAILED (Exit Code 2).
- Reason: The sandbox runs OCCT 7.6.3 and lacks newer modules (`TKDESTEP`, `TKDEIGES`, `TKDEGLTF`, etc.) introduced in OCCT 8.0.0. The codebase uses these modern modules in production. As per strict instructions, the production code was **not** adapted/downgraded for the sandbox environment.
- Python logic tests for batching implementation logic: PASSED (Exit Code 0).

### Measured Before/After Draw-Call Numbers
NOT VERIFIED (Sandbox)
- **Reason**: The executable could not be built and run in the sandbox due to the aforementioned OCCT 8.0.0 dependency mismatches, rendering direct instrumentation via `apitrace` impossible in this environment.
- **Model Assessed for Structure**: `pr2_head_pan.stl` (from three.js examples repo, 49KB, 1000 triangles).
- **Structural Code Analysis Before**: `MasterWidget::paintGL` iterates `m_glMeshes`. For a model with $N$ parts, up to $N$ feature edge `GLMesh` instances are drawn via $N$ separate `glDrawElements` calls.
- **Structural Code Analysis After**: `MasterWidget::paintGL` tests `m_batchedEdgeBufferDirty`, rebuilds once if needed, and issues exactly **1** `glDrawElements` call for all accumulated feature edges across the entire model.

Real world performance measurements (frame times, CPU overhead reduction) are deferred to a Windows + OCCT 8.0.0 build.
