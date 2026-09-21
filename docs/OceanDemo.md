# Shinkou Ocean Demo

`shinkou_ocean_demo` is one renderer sample that runs the same animated ocean
and sky scene on DirectX 11, DirectX 12, and Vulkan. The scene is generated in
the fragment shader: the sky contains a moving atmospheric sun/cloud signal,
and the ocean combines five directional swell bands with reference-inspired
multi-octave procedural wave detail, analytic slope normals, Schlick Fresnel,
GGX water highlights, roughness-controlled sky reflection, refractive
underwater absorption, crest foam and distance fog that reaches 99% at 1 km.

The engine loop is configured with `targetFrameRate = 60.0` and renderer
vsync enabled. When ImGui is enabled, the live window exposes controls for
wind direction/speed, wave height/length, wave choppiness,
sun direction/intensity, water roughness, crest foam and fog distance. Hold the
right mouse button and drag to orbit the camera; dragging
over the control panel is captured by ImGui and does not rotate the camera.

The multi-octave noise, reflection/refraction and crest-foam approach is a
re-authored adaptation of the user-provided ocean reference under CC BY-NC-SA
3.0; its flat placeholder sky was not used, and Shinkou retains its own
animated sun/atmosphere implementation.

The ImGui-enabled configuration fetches Dear ImGui and builds the DX11, DX12
and Vulkan renderer adapters:

```powershell
cmake -S . -B out/build/mingw-imgui -G "MinGW Makefiles" `
  -DSHINKOU_FETCH_DEPENDENCIES=ON -DSHINKOU_ENABLE_IMGUI=ON `
  -DSHINKOU_BUILD_TESTS=OFF -DSHINKOU_BUILD_PHYSICS_VISUALIZER=OFF
cmake --build out/build/mingw-imgui --target shinkou_ocean_demo -j2
```

Use `--frames N` for a bounded smoke run; omit it for the live window.

```powershell
cmake --build out/build/mingw-debug --target shinkou_ocean_demo -j2

& .\out\build\mingw-debug\shinkou_ocean_demo.exe --dx11
& .\out\build\mingw-debug\shinkou_ocean_demo.exe --dx12
& .\out\build\mingw-debug\shinkou_ocean_demo.exe --vulkan
```

For the ImGui build, replace `mingw-debug` with `mingw-imgui` in the commands
above.

For automated backend smoke tests:

```powershell
& .\out\build\mingw-debug\shinkou_ocean_demo.exe --dx11 --frames 3
& .\out\build\mingw-debug\shinkou_ocean_demo.exe --dx12 --frames 3
& .\out\build\mingw-debug\shinkou_ocean_demo.exe --vulkan --frames 3
```

The DX11 path compiles `ocean.hlsl` at runtime through the existing shader
compiler. DX12 consumes `ocean_vs.dxil`/`ocean_ps.dxil`, while Vulkan consumes
`ocean_vs.spv`/`ocean_ps.spv`; CMake generates those artifacts from the same
sample shader sources.
