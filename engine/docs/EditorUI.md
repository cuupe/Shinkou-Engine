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
- light, dark and high-contrast themes;
- panel visibility and workspace settings;
- custom panel registration through `EditorLayer::register_panel`.

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
