# ShinkouEngine editor UI

The editor layer is intentionally isolated from `engine/src/render`. `Engine::tick()`
passes the current renderer, world, frame index and delta time to
`editor::EditorLayer`; the editor only reads renderer diagnostics and world
objects/components. RenderGraph and backend ownership remain unchanged.

## Included surface

The editor now has a backend-neutral UI foundation in addition to the ImGui
view adapter:

- `shinkou::ui::UiRuntime` provides retained widgets, flex/overlay layout,
  dirty-subtree rebuilding, hit testing, focus and pointer capture;
- `shinkou::ui::ThemeRegistry` provides semantic tokens, built-in themes,
  contrast checks and versioned JSON customization;
- `shinkou::editor::DockWorkspace` provides split/tab/floating workspace trees,
  minimum-size rebalance, splitter hit testing and versioned JSON persistence.

The existing ImGui view uses a real DockSpace host when the docking-capable
ImGui build is available. The same panel registry and dock model remain usable
in headless builds.

`Engine::tick()` polls the configured `InputSystem`, forwards pointer, key and
text events through `EditorLayer::process_input()` into `UiRuntime`, and keeps
the UI hit-test geometry current. Window client-size changes are propagated to
the renderer and editor before input dispatch, so resizing does not require a
separate editor integration layer.

- Unity-style top menu model: File, Edit, Assets, GameObject, Component,
  Window and Help;
- Scene/Game workspaces plus Hierarchy, Inspector, Project, Console, Profiler,
  Render Graph and Project Settings pages;
- a pure C++ `EditorUiModel` that owns page definitions, menu commands, object
  tree snapshots, filtering, selection and play/pause state;
- toolbar with play/pause/step/save affordances;
- hierarchy tree with filtering, selection and object creation;
- component inspector using the existing `PropertyDescriptor` API;
- renderer-independent viewport surface ready for a future render-target adapter;
- asset browser, console, profiler and RenderGraph diagnostics panels;
- retained Build panel with toolchain availability, task state, bounded output and diagnostics;
- versioned `.shinkou/build-profile.json` persistence with a bounded multi-profile set, retained Name/Build Directory editing, plus direct Windows launch plans for Visual Studio, Rider, VS Code and clangd; launcher/profile errors remain explicit in the Build snapshot;
- Build diagnostics provide All/Error/Warning/Note filters, total/severity counts, bounded live incremental parsing, row selection and checked file/line/column IDE opening; Windows-launched IDE PIDs are polled at a bounded cadence for Running/Exited/NotFound/QueryFailed presentation state;
- project integration discovers bounded CMake/Visual Studio/.NET/Meson/Cargo files and compile databases, recommends a profile project file, and explicitly generates a validated project-root `.clangd` through the safe filesystem service;
- live bounded stdout/stderr snapshots from the Windows build runner, including partial-line status;
- project-root-safe asynchronous text resource preview in the Inspector, with bounded lines and truncation status;
- safe `compile_commands.json` import/export with normalized project-root paths and arguments-array output;
- retained Media Preview state with AudioSystem-backed Play/Pause/Stop/Loop/Volume commands, bounded duration/waveform metadata, decoder-backed cursor/seek transport when the backend supports it, and explicit unavailable status;
- Windows Media Foundation video previews with bounded metadata and an immutable first-frame snapshot, with explicit decoder failure status;
- video MediaSeek requests can asynchronously decode a bounded time-point frame with cancellation/coalescing; continuous playback and A/V sync remain separate capabilities;
- bounded OBJ model previews with immutable structured vertex/index geometry, bounds and vertex/triangle statistics in the Inspector; when AssetSystem is connected, the OBJ worker consumes its typed model payload before falling back to the project-safe file provider;
- bounded GLTF/GLB model previews for project-local buffers and basic TRIANGLES/POSITION/index accessors, using the same immutable snapshot seam;
- GLTF/GLB model snapshots expose bounded material, sampler, texture and image metadata plus immutable encoded image artifacts; Inspector can select material/texture references, send the selected artifact through the existing WIC memory decoder, and report the selected Base Color/Normal/Metallic-Roughness role to the D3D11 preview seam;
- selected resources are bridged to `AssetSystem` through a non-blocking typed source-artifact contract; the Inspector exposes loading/error/ready, canonical format, descriptor schema/size and source hash without passing source payloads into paint;
- editor initialization and explicit asset refresh also expose an asynchronous `AssetSystem` manifest snapshot with resource count/status; scanning is off the paint and input paths;
- manifest entries carry stable `AssetId` values and reuse bounded source fingerprints between scans; cache hit/miss statistics remain outside paint and are available to the asset-system audit seam;
- when present, `.shinkou/manifest.json` is validated and used only to seed the AssetSystem cache before a fresh mount scan; the Inspector status reports validated-cache versus readback-fallback paths;
- an isolated renderer-neutral model preview scene with independent orbit/zoom/reset camera state; viewing a model never mutates the active `World`;
- Project assets can be dragged from a retained asset row into the visible Scene viewport; the editor validates the project-relative path and supported kind, resolves a connected AssetSystem manifest-backed `AssetId` when available, and creates an undoable `AssetReferenceComponent` at a bounded XY placement. This is a scene-safe reference bridge, not yet runtime model/audio/video instantiation;
- retained Inspector image previews with bounded asynchronous WIC snapshots and a D3D11 Direct2D bitmap cache; connected AssetSystem texture payloads are decoded through a bounded WIC memory stream before falling back to the project file provider;
- light, dark and high-contrast themes;
- panel visibility and workspace settings;
- custom panel registration through `EditorLayer::register_panel`.

Model preview deliberately has two seams: `EditorModelPreviewSnapshot` is the
immutable provider result, while `EditorModelPreviewScene` owns only camera
state and a bounded retained projection. The Inspector renders wireframe
geometry and can show a selected decoded texture artifact. When an initialized
D3D11 backend exposes the editor offscreen-target capability,
`EditorModelPreviewRenderer` uploads the immutable mesh into persistent
renderer resources and appends a model pass into a bounded BGRA8 offscreen
target, followed by an explicit fullscreen composite pass inside the editor
viewport seam; the selected material base color is applied by an HLSL constant
buffer. The renderer no longer depends on the scene color attachment for this
preview. Null, Vulkan, D3D12 and missing-capability paths report an explicit
fallback and keep the retained/WIC preview. A selected WIC BGRA8 image snapshot
now uploads to the D3D11 base-color texture binding when available; the glTF provider carries
`TEXCOORD_0` and `NORMAL` into the D3D11 vertex buffer, while OBJ/no-UV
assets use bounded position/normal fallbacks. The preview shader applies a
small fixed diffuse light and the selected glTF metallic/roughness factors.
Base Color and Normal roles now use separate D3D11 texture/sampler bindings;
Normal uses a bounded derivative-based tangent basis for preview, while a
flat 1x1 normal fallback keeps the shader contract valid when no role payload
is ready. Metallic/Roughness uses a separate binding and samples the glTF
blue/green channels to modulate the material factors. Complete tangent accessor/
UV transform/PBR semantics remain later stages; a bounded synchronous D3D11
readback exists for QA, while an automated pixel-threshold oracle,
animation, nodes and lighting remain later stages before this is described as
a full Unity-class 3D preview.

The AssetSystem bridge is also deliberately narrow. `EditorLayer` requests the
selected project-relative resource asynchronously and publishes only the
result metadata to `EditorUiModel`; text/image/audio/video/model providers keep
their own bounded decoding and immutable snapshot lifetimes. Built-in typed
source processors preserve source bytes with a canonical format, while an
application may register structured processors/loaders for later importer and
derived-cache stages. OBJ, GLTF/GLB and image now consume AssetSystem source
payloads directly; GLTF additionally publishes bounded immutable image artifacts
that flow into the regular WIC memory decoder. Audio and video still use their
dedicated bounded provider seams. This is not yet a formal persisted decoded
artifact or derived thumbnail cache.

Build/IDE integration follows the same boundary: `EditorBuildSystem` owns
toolchain discovery, build planning and bounded process output, while
`EditorToolIntegration` serializes project-relative profile sets and produces an
inspectable executable/argument/working-directory plan. The Build panel exposes
profile selection and bounded field editing; diagnostic rows can pass a checked
file/line/column location to the selected IDE. Windows launch uses
`CreateProcessW` directly and never falls back to shell syntax. The editor
retains only the returned PID and uses limited-information queries, so the
Build snapshot can report the launched process's current state without
claiming compiler completion or taking ownership of the IDE lifecycle. Live
diagnostics are parsed from complete output lines into bounded presentation
state; total and severity counts remain separate from the 256-item display
cap.

Project integration is a separate read/plan/write seam. Discovery only reads
bounded project metadata and canonicalizes candidates beneath the project root;
profile association changes memory until the user saves the profile. A clangd
plan validates `compile_commands.json` through the bounded parser and writes
only the root `.clangd` file through `FileSystemService`, so paint and input do
not perform project scans, parsing, or arbitrary file writes.

ImGui is optional. With `SHINKOU_ENABLE_IMGUI=OFF`, the engine still builds and
the editor state, menu model and object-tree serialization API remain available
without linking UI code. ImGui is only one view adapter for the model; another
native desktop or remote UI can consume the same `EditorUiModel` data.
With ImGui enabled, the layer creates its own context, builds a frame, and
produces draw data. A platform/renderer adapter can consume that draw data when
the host window backend is ready; this boundary deliberately does not alter
Shinkou's renderer.

## Persistence

`Saved/Editor/Layouts/Default.json` stores the engine-owned layout state using
the existing reflection JSON serializer. ImGui window geometry, when enabled,
is stored separately as `Default.json.imgui.ini`, so the two formats never
overwrite each other. The backend-neutral dock tree is stored separately as
`Default.json.dock.json`. All three files are independent and written with a
temporary-file/rename commit. Call `set_layout_path` for per-project or
per-user workspaces.

## Build and run

```powershell
cmake -S . -B out/build/editor -G "MinGW Makefiles" `
  -DSHINKOU_FETCH_DEPENDENCIES=ON -DSHINKOU_ENABLE_IMGUI=ON `
  -DSHINKOU_BUILD_SAMPLE=ON
cmake --build out/build/editor --target shinkou_engine_sample
out/build/editor/shinkou_engine_sample.exe --editor --frames 3
```

The sample's render callback remains the existing renderer smoke-test path; the
`--editor` switch only enables the editor layer.

## EngineDevMCP

`Tools/EngineDevMCP` exposes build, launch, log and shutdown tools. The local
`.codex/config.toml` now uses the workspace-relative `out/build/editor` path and
falls back to `cmake --build` when a legacy `BuildEditor.bat` is absent.
