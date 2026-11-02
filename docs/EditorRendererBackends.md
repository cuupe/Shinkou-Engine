# Native editor rendering backends

The native Dear ImGui editor uses the same renderer-owned world and model
preview paths on DX11, DX12 and Vulkan. Capability flags are backed by native
offscreen color/depth targets, texture sampling and GPU readback.

| Contract | DX11 | DX12 | Vulkan |
| --- | --- | --- | --- |
| Physical-pixel viewport/scissor | Yes | Yes | Yes |
| World color/depth, PBR material, sky and grid | Yes | Yes | Yes |
| Model preview, texture roles and supported node animation | Yes | Yes | Yes |
| Docking and independent Win32 panels | Yes | Yes | Yes |
| RGBA8/BGRA8/RGBA16F/R32F region/mip/layer readback | Yes | Yes | Yes |
| GPU capture of main and secondary windows | Yes | Yes | Yes, surface transfer-source support required |

## Coordinates

Engine world/view math is **left-handed**: +X right, +Y up, +Z forward.
Matrices are column-major and multiply column vectors. Clip depth is [0,1].
Camera movement, rays, selection and ImGuizmo use the same view/projection.
Middle dragging translates along camera right/up with zero forward displacement;
right dragging rotates around the eye without translating it.

glTF/GLB right-handed input is converted once at the provider boundary with
S=diag(1,1,-1): positions/normals flip Z, triangle winding reverses, node
matrices become S*M*S, quaternion X/Y change sign and translation keys flip Z.
Scale is preserved. FBX asks ufbx for left-handed Y-up output, including winding.
OBJ has no reliable axis metadata and is interpreted in engine coordinates.
SPIR-V compilation inverts clip Y to match the DirectX top-left presentation;
Vulkan must therefore preserve the DirectX framebuffer front-face convention.

## Frame and window lifetime

The ViewportsEnable option is set before GPU adapter initialization. This is
required for the official DX12 backend to install its native-window callbacks.
The application updates platform windows after ImGui::Render even if a graph
fails; secondary GPU rendering happens after the world submission.

DX12 fences order world rendering, upstream's separate child queues, and final
resource retirement. Vulkan child windows use the engine graphics queue, with
a completion fence covering child reads before uploads and retirement. Resizing
releases every queue's command pool after idle. An out-of-date present never
resubmits an already-used fence or acquired image.
Presentation wait semaphores belong to acquired swapchain images, independently
of render-graph batch signals; command-completion fences alone do not authorize
retiring presentation semaphores.

Vulkan descriptor overrides allocate a fresh set when an object uniform changes;
earlier draws retain their descriptor snapshot. Compatible render passes reuse
pipelines across load/clear changes; incompatible variants retire after fences.
Depth contents are stored across model, sky and grid passes, with attachment
read/write dependencies between render passes.

## Shader compilation and diagnostics

Built-in engine Vulkan shaders use the configured GLSLC build step. Inline
editor/custom HLSL uses that configured compiler directly, without a shell or
console window, in an isolated temporary directory with bounded wait/cleanup.
Compiled bytecode/reflection is cached (cache format 9). Development/runtime
inline compilation requires that GLSLC executable and a writable temp directory.

`EngineConfig::enableRenderValidation` and sample `--render-validation` enable
available Vulkan validation. DirectX debug layers retain their existing
`SHINKOU_D3D11_DEBUG`/`SHINKOU_D3D12_DEBUG` environment switches. Vulkan diagnostics are retained in
RenderStats and printed as they arrive, so a driver failure cannot hide them.
`supportsTextureReadback` means a submitted frame can be read. The separate
`supportsInPassTextureReadback` capability describes immediate-context pass
observation; explicit command-buffer backends keep it false. Pipeline tests
check final GPU pixels on every backend and observe intermediate passes only
when that additional capability is present.

## Verification

`Tools/UiCapture/RunInspectorInteraction.ps1 -Backend dx11|dx12|vulkan` exercises
typed axes, undo/cancel, native detachment, edits in the child window, close/reopen,
redocking, 3840x2160 and 1024x768. `RunViewportInteraction.ps1` accepts the same
backend argument and checks selection, eye rotation, camera-plane translation,
native framebuffer dimensions and UI scaling. Add `-RenderValidation` when desired.
`RunViewportInteraction.ps1 -Detached` selects `DetachedViewportInteraction.txt`
and checks a world image and input in a secondary window before returning it to
the main dock.

`shinkou_editor_model_preview_renderer_tests dx12 --require-native` (also dx11
or vulkan) requires a real device and checks mesh/material/texture execution,
colored GPU pixels, uploaded bytes, subregions and readback limits. Required mode
also rejects Vulkan validation errors. glTF tests use asymmetric Z coordinates,
non-uniform scale and animated quaternions to check the basis conversion.

Desktop QA uses posted Win32 input with owning-HWND coordinates and actual GPU
readback. It does not establish physical SendInput or mixed-DPI monitor behavior.
This matrix describes editor integration; it is not a claim that every advanced
render feature or asset-provider semantic has been implemented.

Recorded Windows GPU evidence (2026-10-24/25, system DPI 144):

- `out/qa/inspector-dx12-fixed-20261024`, `out/qa/inspector-vulkan-final-20261025`:
  88 inspector actions per backend.
- `out/qa/viewport-dx12-fixed-20261024`, `out/qa/viewport-vulkan-20261024`:
  51 viewport, picking, camera and DPI actions per backend.
- `out/qa/detached-world-dx12-20261025`, `out/qa/detached-world-vulkan-final-20261025`: 27 actions per backend,
  including GPU readback of the child world window.
- `out/model-gpu-final-{dx11,dx12,vulkan}-20261025.log`: required-device model checks.
- `out/editor-hdr-vulkan-validation-20261025.log`: PBR, shadows, HDR effects,
  temporal history, HDR sky/background motion and histogram exposure on Vulkan,
  with synchronization validation and no validation errors.
- `out/editor-final-all-tests-20261025.log`: 71/71 regression checks passed.

Consolidated PNGs and machine-readable HWND/action-count evidence live under
`out/deliverables/render-backends-20261025`.
