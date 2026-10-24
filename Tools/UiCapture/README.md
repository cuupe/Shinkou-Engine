# Editor interaction capture

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

The older retained-UI runner below remains available for compatibility tests.

`RunViewportInteraction.ps1` verifies the native world framebuffer at 1440x900,
3840x2160, 1024x768, and 2560x1440; actual world triangle picking; right-button
look without camera translation; middle-button motion in the camera plane; and
font-atlas rebuild on user scale changes. It restores the initial client size
before exit and preserves GPU captures for every resolution and scaling stage.

Build with `cmake --build out/build/ui-current -j 2` from the repository root.
Run `Tools/UiCapture/RunEditorInteraction.ps1` to create an isolated fixture project,
launch the real D3D11 editor, execute the checked-in script and save evidence.
Use `-Theme light` or `-Theme high-contrast` for the other palettes. Every run
requires a new output directory; the runner preserves existing evidence.

The runner uses targeted Win32 window messages through SDL and InputSystem.
It moves the cursor to keep SDL's native coordinates consistent, then restores
the previous cursor position. This is automated native-message evidence, not
a recording of physical keyboard/mouse hardware or a foreground SendInput test.
The editor's environment-gated snapshot and capture messages only read state
and request GPU readback; functional actions use normal window input events.

Outputs:

- `interactive.bmp.steps.jsonl`: actions and assertions; any failure exits nonzero.
- `*.state.txt`: UI regions, selection, status, camera and inspected property values.
- `*.bmp`: complete 32-bit GPU client-surface readbacks, captured after each phase.
- `interactive.bmp.timings.csv`: bounded per-workload CPU stage percentiles,
  upload bytes and per-frame surface cache hits; GPU execution time is unavailable.
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

`SplitterInteraction.txt` additionally drags the default center/bottom divider.
Pass it with `shinkou_ui_capture --script <absolute-script-path>` and compare the
`viewport.surface` rectangle in `before-splitter.state.txt` and
`after-splitter.state.txt`. Use a fixture project because workspace persistence
is intentionally exercised on shutdown.
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
`capture-window "region" name` reads that region's owning DX11 render target;
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
