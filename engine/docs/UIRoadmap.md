# Shinkou UI architecture roadmap

This roadmap keeps the editor UI usable while the renderer and engine systems continue to evolve.

## Foundation

- `UiRuntime` owns widget lifetime, layout, dirty propagation, hit testing, pointer capture and focus.
- `UiRenderList` is a backend-neutral retained command list. Toolkits are adapters, not the UI model.
- `visible_range` provides bounded work for large trees and asset lists.
- `ThemeRegistry` owns semantic colors, metrics, typography and contrast checks.
- `UiStyleConfig` persists theme, scale, fonts and backgrounds in JSON or XML.

## Editor shell

- Dock layout is a versioned tree with minimum sizes, split-bar hit testing, tabs and floating nodes.
- Layout, dock topology and style are separate files so a theme can change without destroying workspace topology.
- The shell provides menus, transport controls, workspace visibility, settings tabs, asset search and diagnostics.
- UI scale is clamped and applied incrementally; window size is treated as runtime state, not a fixed 1280x720 assumption.

## Engine boundary

- `InputBridge` converts engine events into backend-neutral UI events.
- `FileSystemService` scopes reads/writes to the project root, writes configuration atomically and reports changes.
- Editor file changes are polled at frame boundaries; style changes are hot-reloaded without rebuilding the engine.
- Renderer, world, asset and diagnostics data are exposed to panels through `EditorPanelContext`.

## Next slices

1. Replace remaining ImGui-only controls with semantic widgets backed by `UiRuntime`.
2. Add an editor command registry with undo/redo transactions and shortcut serialization.
3. Add asynchronous asset indexing and thumbnail jobs; keep the UI thread on immutable snapshots.
4. Add font atlas families, DPI buckets and a GPU draw-list adapter with clip batching.
5. Add scene viewport picking/gizmos and a file-backed document model for scenes and prefabs.

The current implementation intentionally keeps these boundaries public so each slice can be tested independently and can later replace ImGui without rewriting editor features.
