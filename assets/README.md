# Shinkou Assets

`shinkou_assets` is the engine-independent resource library. It has no dependency on the renderer, window system, UI, audio backend, ECS, or platform SDKs.

## Build it alone

```powershell
cmake -S . -B out-assets -DSHINKOU_BUILD_TESTS=ON -DSHINKOU_ASSETS_BUILD_TESTS=ON
cmake --build out-assets --target shinkou_assets_tests
ctest --test-dir out-assets -R shinkou_assets_tests --output-on-failure
```

The root project exposes both `shinkou_assets` and the namespaced alias `ShinkouEngine::Assets`. The main engine links this target only as a consumer; resource implementation files are not part of the engine source glob.

## Data structures

- `AssetKey -> slot` hash index gives stable lookup without storing owning pointers in the hot path.
- A slot table stores immutable published data, generation, source fingerprint and dependency metadata.
- Dependency edges use compact slot vectors with duplicate suppression; invalidation walks the reverse graph iteratively.
- A priority queue schedules loads, so scene-critical requests can outrank background preloads.
- An intrusive LRU list makes unpinned eviction O(1) per asset instead of rescanning every record.
- A bounded event queue moves callbacks to `poll()` and prevents worker threads from entering UI/gameplay code.

The default watcher performs filesystem checks on a background thread. Cache validation uses timestamp/size first and can be made content-strict through `AssetSystemConfig::verifyCacheByContentHash`.
