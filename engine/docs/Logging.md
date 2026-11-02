# ShinkouEngine logging

`shinkou::log` is the runtime logging service. It is independent of the render
backend and is initialized from `EngineConfig::logging`.

CMake downloads the pinned spdlog 1.16.0 archive and verifies its SHA-256.
Headers (including bundled fmt) live in the ignored build cache, not in the
source repository. The first configuration needs network access; later builds
can use `SHINKOU_FETCH_DEPENDENCIES=OFF` with the populated cache. A missing
required dependency is a configuration error. Version pins and offline source
overrides are documented in [third-party dependencies](../../third_party/README.md).

The default configuration writes to `Saved/Logs/Engine.log` and the console,
uses an asynchronous queue of 8192 messages, rotates at 16 MiB, keeps five
files, requests a sink flush when an error is processed, and requests a flush
once per second. Sessions append to the existing log by default;
`rotateOnOpen=true` explicitly rotates on initialization.
The default overflow policy is non-blocking (`OverrunOldest`) so a slow disk
does not make producers wait for free queue space. Queue locks and OS scheduling
still have a cost. Use `OverflowPolicy::Block` only for diagnostic
runs where every message must be retained.

```cpp
shinkou::EngineConfig config;
config.logging.level = shinkou::log::Level::Debug;
config.logging.directory = "Saved/Logs";
config.logging.fileName = "Engine.log";
config.logging.maxFileSize = 32u * 1024u * 1024u;
config.logging.maxFiles = 8;
shinkou::Engine engine(config);
```

Use `SHINKOU_LOG_TRACE`, `SHINKOU_LOG_DEBUG`, `SHINKOU_LOG_INFO`, `SHINKOU_LOG_WARN`,
`SHINKOU_LOG_ERROR`, and `SHINKOU_LOG_CRITICAL` in engine code. Disabled levels are
filtered before evaluating arguments or formatting, and the asynchronous logger
keeps sink I/O off the calling thread. Required side effects must not be placed
inside log arguments. `shinkou::log::stats()` exposes actual queue capacity/depth,
worker count, overruns, errors, reader-slot storage, the active file path and the
last error. The last session's overrun/error information remains available after
shutdown. A failed initialization is recoverable by fixing its configuration and
calling initialize again.

## Ownership and concurrency

Each engine logging session owns its spdlog thread pool, logger and periodic
flush worker. It does not replace spdlog's process-wide default logger, named
registry, error handler, thread pool or global flush timer. Direct `spdlog::info`
calls therefore follow the caller's separate spdlog configuration. Use the
`SHINKOU_LOG_*` macros for the engine log.

Logging, level changes and flush requests may race with shutdown/reinitialization.
Readers publish a per-thread protection record, validate the current logger, and
hold a lease until the operation finishes. Shutdown first unpublishes the logger,
waits for pre-existing leases, stops periodic flushes, joins and drains its own
pool, then flushes sinks directly. It does not insert a flush record into a full
overwriting queue during teardown. Thread exit releases its protection record;
new producers reuse free records, so storage tracks peak live producers rather
than the number of threads ever created. Nested logging on one thread shares the
outer protection; lease destruction avoids a second emulated-TLS lookup on MinGW.
The publication/validation design follows the protection principle in
[WG21 P2530R3](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2530r3.pdf),
implemented here with C++17 atomics rather than the C++26 library interface.

`flush()` is a request, not an asynchronous completion barrier. `shutdown()`
waits for accepted messages that survived the selected overflow policy, flushes
C library buffers and closes the session. This is not an OS fsync guarantee.
The default one worker preserves queue processing order; additional workers can
complete messages out of order. A blocked producer or slow sink can make shutdown
wait. Do not call logging lifecycle functions from a formatter/sink that is itself
inside a logging operation. Join application producer threads before process-wide
static teardown. The internal `detail` lease API is thread-bound and is not a
public extension point.

## Validation and performance

`shinkou_logging` is a standalone C++17 library; its tests and benchmarks no longer
link the full engine. `shinkou_engine` links it transitively. Tests cover lazy
arguments, nested leases during concurrent shutdown, 8000 accepted messages from
four producers/four workers, deterministic queue overflow, last-record retention,
rotation and append behavior, foreign spdlog ownership, 40 concurrent lifecycle
cycles, reader-record reuse, sink errors and recovery after invalid configuration.

```powershell
cmake --build out/build/runtime-release --target shinkou_logging_tests shinkou_logging_benchmarks --parallel 4
ctest --test-dir out/build/runtime-release -R '^shinkou_logging_tests$' --output-on-failure
./out/build/runtime-release/shinkou_logging_benchmarks.exe
```

See [the measured optimization report](RuntimeOptimization.md) for improvements
and the enabled-log cost of safe concurrent reclamation.

The `Tools/EngineDevMCP` server reads the same `Saved/Logs/Engine.log` file
through `engine_get_log` and supports `lines`, `contains`, and `level` filters;
`engine_log_stats` returns file metadata. Set `ENGINE_EDITOR_LOG` when the
runtime uses a different log path.
