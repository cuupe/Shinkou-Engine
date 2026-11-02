# Shinkou Core

Independent C++17 primitives and structured interchange for Shinkou modules.

```cmake
target_link_libraries(app PRIVATE ShinkouEngine::Core)
```

The public API lives in `include/shinkou/core` and `include/shinkou/data`.
It supplies Error/Result, generational and packed handles, stable hash primitives,
UTF-8 filesystem boundaries, a compact Value tree, a bounded JSON codec, schema
headers, and checked field readers. The nlohmann/json implementation is private
to one compiled source file. No engine/editor/render/platform-backend dependency
is required.

```powershell
cmake -S core -B out/build/core-release -DCMAKE_BUILD_TYPE=Release
cmake --build out/build/core-release
ctest --test-dir out/build/core-release --output-on-failure
```

See [the architecture and migration contract](../docs/CoreArchitecture.md).

CMake obtains nlohmann/json 3.12.0 using the shared version/checksum registry.
The first configure downloads into `_deps/`; populated caches can be reused with
`SHINKOU_FETCH_DEPENDENCIES=OFF`. See [dependency management](../third_party/README.md).
