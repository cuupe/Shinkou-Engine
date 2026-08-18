# ShinkouEngine logging

`shinkou::log` is the runtime logging service. It is independent of the render
backend and is initialized from `EngineConfig::logging`.

The default configuration writes to `Saved/Logs/Engine.log` and the console,
uses an asynchronous queue of 8192 messages, rotates at 16 MiB, keeps five
files, flushes errors immediately, and flushes the queue once per second.
The default overflow policy is non-blocking (`OverrunOldest`) so a slow disk
cannot stall the frame loop. Use `OverflowPolicy::Block` only for diagnostic
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
filtered before formatting and the asynchronous logger keeps sink I/O off the
calling thread. `shinkou::log::stats()` exposes queue overruns and the active
file path for diagnostics.

The `Tools/EngineDevMCP` server reads the same `Saved/Logs/Engine.log` file
through `engine_get_log` and supports `lines`, `contains`, and `level` filters;
`engine_log_stats` returns file metadata. Set `ENGINE_EDITOR_LOG` when the
runtime uses a different log path.
