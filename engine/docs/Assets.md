# Shinkou asset system

The asset system is intentionally independent from `shinkou/render`. Runtime code can load bytes or typed runtime data without creating a graphics device, and render code may consume the resulting data later through an application-specific adapter.

## Runtime setup

```cpp
shinkou::EngineConfig config;
config.assets.projectRoot = "Content";
config.assets.cacheRoot = "Saved/AssetCache";
config.assets.workerCount = 4;

shinkou::Engine engine(config);
engine.assets().add_mount("textures", "Content/Textures");
engine.assets().register_extension("png", "texture");
```

Asset URIs are normalized to forward-slash form. A request such as
`{"textures://ui/logo.png", "texture"}` is deduplicated, processed on a worker,
and returns a `shared_future<AssetLoadResult>`. The future's `AssetData::bytes`
is immutable and remains valid after the system trims its own cache.

## Extensibility

Register an `IAssetProcessor` for source import/cooking and an `IAssetLoader` for
runtime materialization. Processors can publish hard or soft dependencies in
`AssetArtifact`; dependency requests are automatically scheduled after the parent
becomes ready. Registration should happen during application setup, before worker
loads are started.

The built-in `raw` pair copies source bytes and is useful for prototypes, scripts,
custom formats, and tests. Production projects can add texture, mesh, material,
animation, shader, localization, or platform-specific processors without changing
the engine or renderer.

## Caching and live editing

Cooked artifacts use a versioned `.wac` binary format keyed by asset id, source
hash, and processor version. `poll()` marks changed loaded records stale and emits
an invalidation event; the next request reloads them. `trim()` evicts the least
recently used unpinned data until the configured memory budget is satisfied.
`scan_sources()` and `write_manifest()` expose build/editor tooling without adding
editor dependencies to the runtime library.
