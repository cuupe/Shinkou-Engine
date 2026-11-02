# Editor interaction capture

`RunHierarchyPerformance.ps1` uses the optional `shinkou_editor_ui_benchmarks`
target to open a real 20,001-object editor, select the last row and filter it.
Build with `SHINKOU_BUILD_BENCHMARKS=ON`. It supports DX11/DX12/Vulkan and the
three editor themes. QA snapshots include cached/submitted hierarchy rows,
rebuild count and region count; normal editor sessions leave QA recording off.

The native editor now uses Dear ImGui Docking. Run
`Tools/UiCapture/RunImGuiInteraction.ps1 -Theme dark` from the repository root
after building `out/build/mingw-debug`. The same runner supports `light` and
`high-contrast`; each run creates a separate fixture project and evidence folder.
It checks exact object names, Ctrl+A/Z/Y, play/pause/step, asset filtering,
panel activation, profiler display, selection changes during an uncommitted edit,
and resizing to 1024x768 and back.

This runner attaches only to its child editor's keyboard queue and supplies
queued Win32 key events with matching Ctrl/Shift state (`--queued-keyboard`).
The official Win32 backend reads that state; posting key messages alone cannot
exercise modifier chords reliably. Mouse actions remain targeted Win32 messages.
The log identifies this mode as `AttachedEditorKeyboardQueue`; this is synthetic
window-input evidence, not physical hardware input. The queue is detached and
cleared on exit. `--native-keyboard` remains available for foreground `SendInput`
tests and refuses to type if the editor does not own foreground focus.
The `object-name` assertion checks the complete selected name, including Unicode.

The retained editor runner and its legacy interaction/splitter scripts have been removed. Use the ImGui, Inspector, AssetInspection and Viewport runners for the active editor. All functional actions travel through the official Win32 ImGui backend. Environment-gated snapshot/capture messages only observe state or request GPU readback.

`RunViewportInteraction.ps1` verifies the native world framebuffer at multiple resolutions, triangle picking, right-button look, middle-button pan, and font rebuild on user scale changes. It supports DX11, DX12 and Vulkan; `-Detached` exercises an independent native world window.

Outputs:

- `interactive.bmp.steps.jsonl`: actions and assertions; any failure exits nonzero.
- `*.state.txt`: UI regions, selection, status, camera and inspected property values.
- `*.bmp`: complete 32-bit GPU client-surface readbacks, captured after each phase.
- `run.log`: backend capability, client/surface size and actual window DPI.

Script grammar is one command per line. Quoted strings support paths with spaces.
`@PROJECT@` is replaced only by the fixture runner. Supported commands are
`click/double/right/hover/sweep "region"`, `drag "region" fraction`, `orbit`,
`key/down/up virtualKey`, `text "UTF-8"`, `wheel signedNotches`, `resize WxH`,
`wait milliseconds`, `capture name`, `exists/missing "path"`, `state "substring"`,
`scroll-min/scroll-max number`, `remember-camera`, and `camera-changed`.

Window resizing does not change system DPI. The log's `dpi` is the evidence;
UI scale is a user preference and must not be presented as another system DPI.
GPU capture and profiler collection have overhead, especially on capture and
resize frames. Small sample counts do not establish a stable p95.

# Asset inspector regression

`RunAssetInspection.ps1` creates generated TXT/script, DOCX, WAV/MP3, MP4 and FBX
fixtures, then runs `AssetInspection.txt` against the native D3D11 editor. Python
and ffmpeg are needed only for fixture generation. The run checks saved per-file
preferences, audio transport, frame stepping and model previews, with GPU images,
state snapshots and a step log under a new `out/qa/asset-inspection-*` directory.
Use `-Scenario narrow -Theme high-contrast` for the binary FBX/1024×768 case.
Queued tests enable persistent targeted hover only in their child QA window;
native desktop leave notifications are ignored in that explicit test mode.

# Typed inspector and detached windows

`RunInspectorInteraction.ps1 -Theme dark` runs the typed inspector and native
window regression. `light` and `high-contrast` are also supported. Each run uses
a new project and preserves its step log, snapshots and GPU captures.

The script checks independent XYZ inputs, Euler degrees, scale, text editing,
undo/cancel, native detachment, edits inside the detached window, closing and
reopening that panel, redocking, layout reset/save, 3840×2160 and 1024×768.
`capture-window "region" name` reads that region's owning native GPU render target;
secondary RGBA swapchains are converted to BMP channel order during readback.

Additional script commands are `typed "component:label" "value"`,
`axis-drag/detach "region" dx dy`, `redock "source" "target"`,
`docked/detached "region"`, and `close-panel "region"`.
Region snapshots include the owning HWND and physical client rectangle.
Dragging a moving native window interpolates desktop coordinates, then converts
to that window's current client coordinates for each message.

Unattended queued mode uses posted Win32 events, not SendInput. The opt-in QA
adapter ignores hardware leave notifications across the editor's HWNDs and
uses ImGui's geometry fallback for hovered viewports. Production retains the
official Win32 cursor/window detection. Pointer records include requested and
observed hardware coordinates, because remote/background desktops may reject
cursor warps. These runs establish the message route and actual GPU output;
they do not establish physical mouse or multi-monitor DPI transitions.

`RunInspectorInteraction.ps1` and `RunViewportInteraction.ps1` accept
`-Backend dx11|dx12|vulkan` and optional `-RenderValidation`. Vulkan inline HLSL
uses the GLSLC path recorded at CMake configuration; `TEMP`/`TMP` must be writable.
`RunViewportInteraction.ps1 -Detached -Backend vulkan` selects
`DetachedViewportInteraction.txt` and additionally verifies the world image,
selection, right-button look and camera-plane pan in a distinct HWND, then
redocks that viewport. It uses the same capture helper and `--require-gpu` flag.
See [backend contracts](../../docs/EditorRendererBackends.md) for synchronization
and coordinate conventions.
