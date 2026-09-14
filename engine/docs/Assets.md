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

The default editor-facing extension map also provides typed source processors and
loaders for text, texture, model, audio, video, font, shader, material, and scene
assets. These artifacts preserve source bytes with a canonical typed format; they
are not a claim that decoding or GPU materialization has happened. Applications may
replace any type with a structured processor/loader during setup.

Typed artifacts may additionally publish a bounded descriptor through
`AssetArtifact::metadataFormat` and `AssetArtifact::metadata`. The built-in
processor currently emits versioned source descriptors for text, PNG/JPEG
dimensions, WAV format fields, and OBJ vertex/face counts. `AssetData` exposes
that descriptor through an immutable string; the descriptor is cacheable metadata,
not a decoded texture, audio stream, mesh, or renderer resource.

The editor's OBJ and image previews consume typed payloads: when an initialized
AssetSystem is attached, the model worker requests the `model` artifact and the
image worker requests the `texture` artifact. They parse/decode only their
immutable bytes under their existing bounded provider rules. Other preview
providers remain separate until their structured artifacts are defined.

## Caching and live editing

Cooked artifacts use a versioned `.wac` binary format keyed by asset id, source
hash, and processor version. `poll()` marks changed loaded records stale and emits
an invalidation event; the next request reloads them. `trim()` evicts the least
recently used unpinned data until the configured memory budget is satisfied.
`scan_sources()` and `write_manifest()` expose build/editor tooling without adding
editor dependencies to the runtime library. Each manifest entry also contains the
stable `AssetId` derived from its normalized virtual key, so editor caches do not
need to use a physical path or scan order as identity. `read_manifest()` provides
a bounded validation-only readback path; callers must still revalidate sources
through the current mounts before using the entries as a cache seed.
