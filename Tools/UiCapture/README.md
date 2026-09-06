# Editor interaction capture

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
