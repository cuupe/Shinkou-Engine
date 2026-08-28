# Shinkou UI system

The UI stack is split into four layers so the engine is not coupled to ImGui or
to a particular desktop backend:

1. **UI runtime** owns the retained widget tree, measurement/arrangement,
   pointer hit testing, focus/capture and dirty-state accounting.
2. **Style system** owns semantic colors, metrics and typography. Built-in
   dark, light and high-contrast themes are data, not renderer code, and can be
   overridden or loaded from JSON.
3. **Workspace system** owns editor panels and their dock tree. A workspace is
   a split/tab/floating tree with minimum-size constraints, active tabs and
   visibility. Its JSON is versioned and independent from ImGui's ini format.
4. **Adapters** translate the runtime draw commands and input events to a
   renderer/backend. The current editor keeps the existing ImGui adapter for
   compatibility; native and remote adapters can consume the same models.

## Delivery plan and ownership

| Workstream | Ownership | Output | Integration gate |
| --- | --- | --- | --- |
| Runtime | UI runtime worker | `shinkou/ui/Ui.*` and runtime tests | deterministic layout, hit-test and event tests |
| Theme | Style worker | `shinkou/ui/Theme.*` and theme tests | JSON round-trip and contrast checks |
| Workspace | Docking worker | `shinkou/editor/DockLayout.*` and workspace tests | malformed-data safety and min-size normalization |
| Editor integration | engine maintainer | `EditorLayer`, CMake and docs | existing editor model/tests stay green |
| Verification | engine maintainer | build/CTest and sample smoke test | headless build plus optional ImGui build |

The first milestone intentionally provides deterministic data models and
headless tests. Rendering adapters can then be added without changing layout,
theme or persistence formats. Each persisted format carries a schema/version
field and unknown fields are ignored for forward compatibility.

## Performance rules

- Layout is recomputed only for dirty subtrees; unchanged subtrees reuse their
  measured size and arranged rectangles.
- Hit testing visits children in reverse paint order and stops at the first
  captured or consumed target.
- Workspace operations mutate the dock tree and normalize once at the end,
  avoiding per-frame allocations.
- Persistence happens on explicit save/shutdown, never on every frame.

## Persistence locations

Engine-owned editor workspaces live under `Saved/Editor/Layouts/`. The engine
workspace JSON and backend-specific ini/config files remain separate so a
backend can be replaced without invalidating editor state.
