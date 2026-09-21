# Rendering interface boundary

The engine has one upper-level rendering contract:

`Renderer -> RenderGraph -> IRenderBackend -> native backend implementation`

Gameplay, editor features, samples, and tools use `Renderer`, `RenderGraph`,
resource descriptors, capabilities, and the common draw/dispatch commands.
DX11, DX12, and Vulkan objects remain implementation details of
`engine/src/render/Backends.cpp`. `Renderer` intentionally does not expose a
backend pointer; use `Renderer::execute_graph()` for an explicitly supplied
graph and `Renderer::submit()` for the owned frame graph.

Backend differences that affect feature selection must be reported as
capabilities, not tested through `BackendApi` in feature code. For example,
editor model rendering uses `supportsEditorModelRendering`. `BackendApi` is
still valid for backend selection, diagnostics, and shader-asset compilation
inside the rendering implementation.

## Common line geometry

`shinkou/render/RenderGeometry.h` provides the backend-neutral line primitive
and indexed line-list builder. `append_line_segment()` is the allocation-free
hot-path after the caller reserves storage; it rejects non-finite,
scale-degenerate segments and protects the 32-bit index range. The resulting
positions use tightly packed `Vec3` values and can be uploaded through the
normal `BufferDesc`/`MeshDraw` path with a line-topology pipeline.
