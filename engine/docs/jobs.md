# Job system (`shinkou::jobs`)

Engine-wide task system. Public header: `engine/include/shinkou/jobs/JobSystem.h`,
implementation: `engine/src/jobs/JobSystem.cpp`, tests: `engine/tests/JobsTests.cpp`
(`ctest -R shinkou_jobs_tests`).

## Why it exists

Before this module the engine had no shared execution layer: `MathParallel`,
`NetworkSystem`, `PhysXPhysicsWorld`, `RendererDiagnosticsService` and
`RenderGraphScheduler` each created their own threads. That is N thread pools per
process, N sets of wakeup policies, and every one of them competes for the same
cores during a frame. This is the single place a scheduling decision should be
made.

## API

```cpp
shinkou::jobs::JobSystem jobs;              // 0 == hardware_concurrency()
jobs.submit([]{ ... });                     // fire and forget
jobs.submit([]{ ... }, &counter);           // counted
jobs.submit_batch(bodies, count, &counter); // many jobs, one queue transaction
jobs.parallel_for(count, grain, [](std::size_t i){ ... });
jobs.wait(counter);                         // helps while it waits
jobs.run_pending(64);                       // frame-budgeted drain
jobs.wait_idle();
```

`parallel_for` is the fast path. It submits at most `worker_count()` tasks and
distributes indices through an atomic cursor, so partitioning adapts to run-time
cost instead of relying on a static split.

`submit_batch` is the fast path for many *independent* jobs. It reserves the
counter once per batch and pushes up to 64 tasks under a single lock acquisition;
see the performance section for why that is a 10x difference rather than a micro
optimisation. It has two forms: a range of distinct callables, and a single
callable queued `count` times (what `parallel_for` uses for its helper tasks, so
it never has to materialise an array of N identical `std::function`s).

## Guarantees

* **Waiting helps.** A thread blocked in `wait()` executes pending jobs instead of
  sleeping, so nested waits cannot deadlock. A one-worker system still finishes
  work submitted from inside a job.
* **Exceptions never vanish.** `parallel_for` rethrows the first failure after
  every helper task finished. Jobs submitted with `submit`/`submit_batch` have no
  caller to rethrow to, so they are parked and returned by `take_exception()`; a
  long-lived system should poll it.
* **No lost work.** The destructor joins the workers and then runs whatever is
  still queued, so no counter can be left stranded.
* **Cancellation is advisory.** A cancelled counter skips bodies that have not
  started yet; running bodies are never interrupted.

## Contracts

Violating these is undefined behaviour:

* A `Counter` must outlive every job attached to it.
* Job bodies must be safe on any thread, in any order.
* No thread may submit while a `JobSystem` is being destroyed.

For deterministic behaviour use a grain that covers the whole range: `parallel_for`
then runs serially on the caller, and exceptions propagate directly.

A `Counter` counts *pending completions*: `submit`/`submit_batch` reserve one unit
per job, so do not pre-`add()` for a job that is also submitted with that counter.
`add()`/`complete()` are the manual form, for work that is not one submitted job.

## Architecture

| Piece | Purpose |
|---|---|
| `WorkDeque` per worker | Owner pushes/pops at the bottom (LIFO, keeps the hottest task in cache); thieves take from the top (FIFO). Bounded ring, one spin lock. |
| Overflow queue | Single intrusive list with a spin lock. Used when a worker deque is full and by non-worker producers that cannot be placed. |
| Task pool | Chunked slab (`kChunkTasks` per chunk). Chunk memory is owned by the pool for its whole lifetime, so a task pointer can never dangle. |
| Worker magazine | Up to `kLocalFreeTarget` recycled tasks per worker. Keeps the pool lock out of the hot path. |
| Producer magazine | Same, per thread, for producer threads that are not workers. Keyed by a process-unique system id, not by address. |
| Idle protocol | Long pause-spin while work flows, then a condition variable with a 1 ms timeout, so a lost wakeup is a latency blip and never a hang. Wakeups are coalesced through `notifyPending_`. |
| Steal batching | A thief moves up to `kStealBatch` tasks per lock acquisition, taking at most half of the victim's queue so it can never strip a worker bare. Extras park in a per-worker buffer; `buffered_` keeps `wait_idle()` from returning while one is non-empty. |

### Two decisions worth remembering

**The deque uses a spin lock, not a lock-free ring.** A Chase-Lev style lock-free
ring was implemented first and withdrawn. With a *bounded* ring a thief that has
claimed index `t` can be overtaken by the owner wrapping around onto `t & mask`;
the result is a double-take: one task, two consumers, a counter that completes
while work is still queued. That showed up as `parallel_for` returning early and
corrupting the *next* call through the reused stack counter. Making it correct
needs hazard pointers or a never-recycled buffer. An uncontended spin lock costs
a few nanoseconds; a double-take is a silent correctness bug.

**The caller's own share of `parallel_for` is exception-guarded.** The calling
thread also processes chunks. If its body throws, the helper tasks are still
holding references to the caller's `fn`, `next` and `counter`; unwinding before
waiting for them writes through a dead stack frame. The catch records the
exception, waits, and only then rethrows.

## Performance

Measured on an idle 32-thread Windows laptop (MinGW 15.2, `-O2`, 8 P-cores +
16 E-cores), so the speedup ceiling is the hardware's, not the scheduler's:

| Pattern | Result |
|---|---|
| `parallel_for` dispatch, 4,000,000 items, empty body | **0.14-0.21 ns/item** |
| `parallel_for` small call, 256 items / 4 chunks | **0.28 us/call** |
| `parallel_for` small call, 2048 items / 32 chunks | **1.96 us/call** |
| `parallel_for` with an atomic bump per item (contended body) | 14-18 ns/item, the atomic dominates, not the framework |
| CPU-bound (register-only xorshift) speedup, 64 chunks | 5-7.6x on this 8P+16E part (run-to-run variance is large) |
| `submit_batch`, 200,000 jobs, 128 per call | **60-137 ns/job** |
| `submit`, workers parked (producer side alone) | **71-79 ns/job** |
| `submit`, 32 workers actively draining | 670-840 ns/job |

The last row is the honest weak spot, and the reason `submit_batch` exists.
Profiling the two ends:

* With the workers parked, a submission costs **~75 ns**: that is the producer's
  own floor (task acquire from the magazine, counter reserve, one queue push).
* With 32 workers draining, the same submission costs **~750 ns**. The extra 675 ns
  is cache-line ownership: every push touches the deque that a pool of thieves is
  concurrently try-locking, and every counter reserve shares a line with 32
  concurrent decrements. No amount of tuning removes that while one job is one
  transaction.
* `submit_batch` collapses N jobs into one transaction per batch and lands back at
  the producer floor (**60-137 ns/job**, ~10x better) with no safety cost.

So: fan out with `parallel_for`; for large numbers of unrelated jobs use
`submit_batch`; a `submit()` loop is only appropriate for a handful of jobs per
frame.

One measured detail inside `parallel_for`: it used to build a `kMaxWorkers`-sized
array of `std::function` on every call just to hand the same helper lambda over N
times. Queuing one callable N times removes it, worth about 9% on calls that fan
out to 31 helpers (26.0 us to 23.5 us for `parallel_for(2048, 64)`); below that it
is inside the noise. Small, but free.

The bigger fix for small calls is **inline first, escalate later**. `parallel_for`
now starts on the caller and only fans out once it has spent `kEscalateAfterNs`
(20 us) of real work inline. A hand-off costs 1-2 us, so for ranges that finish
faster than that, submitting helper tasks is pure loss:

| Call | Before | After |
|---|---|---|
| `parallel_for(256, 64)` | 1.65 us | **0.28 us** |
| `parallel_for(1024, 64)` | 7.86 us | **1.00 us** |
| `parallel_for(2048, 64)` | 22.7 us | **1.96 us** |
| `parallel_for(8192, 8)` | 46.8 us | **14.7 us** |

Large workloads pay for it only in the sense that the first 20 us run on one
thread. An A/B against the always-fan-out build on the CPU-bound benchmark shows no
regression (5.0-6.1x before, 6.5-7.6x after, both noise-dominated). The clock is sampled once after the first chunk
and then every fourth, so a tiny range finishes before it ever asks for the time.

Steal batching is a smaller effect measured the same way: `submit_batch` at 128
per call went from 77.5 to 68.7 ns/job (median of six interleaved runs, ~11%),
while `parallel_for` at every measured size was unchanged. The halving rule is what
keeps it safe for load balance: a thief can never take more than half of a victim's
queue, so heavy-task workloads still spread across workers.

## Cross-platform notes

Only `std::thread`, `std::mutex`, `std::condition_variable` and `std::atomic` are
used. The only platform-specific code is the CPU pause hint:

* MSVC: `_mm_pause` / `__yield` (`<intrin.h>`)
* GCC/Clang x86: `__builtin_ia32_pause`
* GCC/Clang ARM: `yield`
* anything else: `std::this_thread::yield`

No Windows or POSIX API is called, no thread affinity is set, and no thread names
are assigned (worth adding later for debugger ergonomics).

## Tests

`shinkou_jobs_tests` is built from two translation units only (`JobsTests.cpp` +
`JobSystem.cpp`) and links nothing but `Threads::Threads`: no engine archive, no
third-party library. That is deliberate. The module is a leaf, so a stray engine
include should break this target immediately instead of quietly working. It
compiles and runs in about three seconds total, which makes it usable as a fast
inner loop while tuning the pool.

```
ctest -R shinkou_jobs_tests                    # 122 checks, ~1 s
shinkou_jobs_tests --stress 50                 # same binary, 50 adversarial rounds
```

The `--stress` mode is the same binary on purpose: proving the subsystem must not
need the engine. One round mixes 200k coverage items (each index must run exactly
once), 4 concurrent producers sharing one counter, nested fan-out, a throwing
round every third iteration, and a cancelled batch, then verifies every count.

`engine/tests/JobsTests.cpp` covers: inline escalation (slow-body coverage still runs each index exactly once), worker-count clamping, submit/wait, manual
counter completion, nested submit, parallel-for coverage (each index exactly once,
including uneven per-index cost), serial fallback, nested parallel-for, exception
propagation from helpers and from the calling thread, cancellation, single-worker
determinism, multi-producer submission, task-storage release via
`shared_ptr::use_count`, system restart (no state reuse across identically
addressed systems), `submit_batch` (single batch, multi-batch, larger than one
worker ring, called from inside a job, carrying an exception), destructor drain,
`run_pending`, re-entrant wait, and an informational throughput probe.

## Follow-ups

1. Migrate `MathParallel`, `RenderGraphScheduler`, `PhysXPhysicsWorld` and
   `RendererDiagnosticsService` onto this pool, one at a time, with the existing
   tests as the safety net.
2. Optional thread names for profilers/debuggers.
3. Per-worker `queued_` counters to keep the shared line out of the submit path.
