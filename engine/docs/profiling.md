# Profiling and tracing (`shinkou::profile`)

CPU timeline capture for the engine. Header: `engine/include/shinkou/profile/Profiler.h`,
implementation: `engine/src/profile/Profiler.cpp`, tests: `engine/tests/ProfileTests.cpp`
(`ctest -R shinkou_profile_tests`).

The C++ analogue of Unity's Profiler CPU module: mark scopes in the code, run, and
open the exported trace in `chrome://tracing` or Perfetto. There is no GUI in the
library — the viewer is the browser's.

## Quick start

```cpp
#include "shinkou/profile/Profiler.h"

void Engine::tick(Seconds dt) {
    auto& profiler = shinkou::profile::Profiler::instance();
    profiler.begin_frame();
    {
        SHINKOU_PROFILE_SCOPE("cull");
        jobs.parallel_for(...);
    }
    {
        SHINKOU_PROFILE_SCOPE("render");
        renderer.submit(world);
    }
    SHINKOU_PROFILE_COUNTER("draw calls", 220);
    profiler.end_frame();
}

// anywhere, later:
profiler.write_chrome_trace("frame.trace.json");
```

Open the file in Chrome (`chrome://tracing` -> Load) or <https://ui.perfetto.dev>.
Both render the same thing: threads as lanes, spans as blocks, counters as tracks.

## Data model

Three shapes, one pipeline:

| Kind | Macro | Trace form | Use |
|---|---|---|---|
| Span | `SHINKOU_PROFILE_SCOPE("name")` | `"ph":"X"` block with `dur` | any timed region |
| Counter | `SHINKOU_PROFILE_COUNTER("name", value)` | `"ph":"C"` sample | draw calls, entities, queued jobs |
| Instant | `SHINKOU_PROFILE_INSTANT("name")` | `"ph":"i"` marker | asset loaded, level ended |
| Frame | `begin_frame()` / `end_frame()` | a "frame" span plus a `frame_ms` counter | the CPU-usage curve |

Counters are what make the profiler more than a CPU view: any subsystem that can
report a number gets a track in the same timeline.

## Architecture

| Piece | Purpose |
|---|---|
| Zone registry | Names are interned once; the macros cache the id in a function-local static, so a marker costs no string work after the first call. |
| Per-thread ring | Single producer (the owning thread) / single consumer (the exporter). Fixed capacity, no allocation, no locks: a full ring drops the sample and counts it. |
| Archives | `drain()` moves ring contents into per-thread vectors. `end_frame()` drains for you. |
| Export | Text dump of the archives in Chrome Trace Event format. Metadata events name the process and every thread. |
| Retention cap | 2 M samples (~64 MB). Past that, recording stops appending and `stats().truncated` is set; export and `reset()` a session that needs more. |

Recording never blocks and never allocates. The only syscall in the whole path is
whatever `steady_clock::now()` costs on the platform.

## Overhead

Measured on this machine (MinGW 15.2, `-O2`), 2 M marker pairs:

| | per span |
|---|---|
| profiling on | **62 ns** |
| profiling off (runtime switch) | ~0 ns (the loop optimises away) |
| compiled out (`SHINKOU_PROFILE=0`) | 0 |

Two thirds of that is the two `steady_clock::now()` calls (~22 ns each on Windows);
the ring store is the rest. At 1000 spans per frame that is ~62 us, or 0.4% of a
16.7 ms frame.

## What it does and does not cover

Covered: CPU time on any thread, thread lanes, span nesting, counters.

Not covered (deliberately, each is its own project):

* **Memory**: needs a global `operator new/delete` hook and allocation call sites.
* **GPU**: needs timestamp queries in each render backend (DX11/DX12/Vulkan).
* **Rendering stats**: counters are ready, but the renderer has to report them.

The library is the pipe; each subsystem still has to say what it did.

## Wiring it in (suggested order)

1. `Engine::tick` — one span per stage (input, physics, audio, scripts, render).
2. `JobSystem` — a span in `execute` and counters for queue depth.
3. Subsystems: physics bodies, audio voices, network bytes, loaded assets.
4. Renderer: draw calls and pass time (that one belongs to the render work, not to
   this library).

Keep the markers out of loops: one span per frame per stage is the intended
granularity, not one per entity.

## Tests

`ctest -R shinkou_profile_tests` — 52 checks: zone interning, span/counter/instant/
frame recording, nesting, multi-threaded capture, ring overflow accounting,
runtime disable, macro expansion, thread names, JSON well-formedness (including
escaping), file writing, and reset semantics.

The target compiles only `ProfileTests.cpp` + `Profiler.cpp` and links nothing but
`Threads::Threads`, so the module stays a leaf: an accidental include of anything
from the engine breaks the build immediately.

## Viewing a sample

```
shinkou_profile_tests --demo out/profile-demo.trace.json
```

writes a four-frame trace with nested spans, counters and two worker threads.
