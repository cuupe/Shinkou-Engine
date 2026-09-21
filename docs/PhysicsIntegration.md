# Shinkou Physics Integration

The engine exposes physics through `shinkou::physics::IPhysicsWorld`. The
public contract is independent of PhysX, so gameplay and the future editor can
inspect and control bodies without including SDK headers.

## Standalone library

Physics is built as the independent `shinkou_physics` static library. The
engine links it as `ShinkouEngine::Physics`, while the physics tests,
benchmarks, and example link the physics library directly. This keeps PhysX,
the deterministic fallback, and the backend factory out of the editor/runtime
library's source compilation boundary.

The standalone case can be built and run with:

```text
cmake --build out/build/mingw-debug --target shinkou_physics_example
out/build/mingw-debug/shinkou_physics/shinkou_physics_example.exe
```

It creates a static ground and dynamic box, advances 60 frames, performs a
static-only raycast, drains contact events, and prints the selected backend.
For a deterministic end-to-end PhysX assertion, build and run
`shinkou_physics_real_scene_test`; it stacks four rigid bodies, waits for real
contacts, raycasts the ground, and verifies that a velocity change moves the
top body.

## Rendered rigid-body visualizer

`shinkou_physics_visualizer` is a standalone runtime sample, separate from the
editor UI. It uses the engine's backend-neutral `RenderScene`,
`ForwardRenderer`, and `RenderGraph`; Windows defaults to D3D11 and other
platforms default to Vulkan, with `--dx11`, `--dx12`, `--vulkan`, and `--null`
available for explicit selection. The rigid-body state comes from the public
`IPhysicsWorld` interface. The scene contains a real static platform, a
tilted static collider, ten dynamic boxes with initial linear/angular
velocity, and an in-world status panel whose bars are driven by live
height/contact statistics. The window consumes the engine's SDL3 input stream:
A/D or arrows apply horizontal force, W/S or arrows apply vertical force, Q/E
applies depth force, Space launches the selected body, N spawns a new body,
Tab selects a body, and R resets the dynamic scene.

Build and launch it with:

```text
cmake --build out/build/mingw-debug --target shinkou_physics_visualizer
out/build/mingw-debug/shinkou_physics_visualizer.exe
```

The PhysX-enabled Windows validation build is:

```text
cmake -S . -B out/build/shinkou-msvc-physx -G Ninja \
  -DSHINKOU_PHYSX_ROOT=<path-to-PhysX/physx> \
  -DSHINKOU_ENABLE_SDL3_INPUT=ON
cmake --build out/build/shinkou-msvc-physx --target shinkou_physics_visualizer
out/build/shinkou-msvc-physx/shinkou_physics_visualizer.exe
```

The executable output directory receives the PhysX runtime DLLs and SDL3
input DLL automatically. A separate `--null --frames 240` run is useful for
headless integration checks.

Use `--frames 1` or another finite frame count for a headless-style renderer
smoke test. A normal launch keeps the window open until it is closed. The
visualizer automatically selects PhysX when it was found at configure time;
the current fallback remains visible in the console as `Simple`.

## Backend selection

`EngineConfig::physics.backend` accepts `Auto`, `Simple`, or `PhysX`.

- `Auto` selects PhysX when the SDK was found at configure time and otherwise
  uses the deterministic fallback.
- `Simple` is intended for bootstrap, headless tests, and environments where
  distributing PhysX is not possible.
- `PhysX` keeps the requested backend visible through `backend()` and falls
  back internally if initialization fails; `is_available()` and `last_error()`
  expose the reason.

PhysX is not downloaded automatically. It can be obtained from the official
NVIDIA GitHub repository, but the source tree must be built for the target
platform before Shinkou links it:

```text
git clone --recurse-submodules https://github.com/NVIDIA-Omniverse/PhysX.git external/PhysX
cd external/PhysX/physx
generate_projects.bat       # Windows; select a supported Visual Studio preset
generate_projects.sh        # Linux; select the matching Linux preset
```

Build the checked or release configuration for the target platform, then
configure Shinkou with `-DSHINKOU_PHYSX_ROOT=<path-to-PhysX/physx>` or set
`PHYSX_ROOT`. The detector accepts both a conventional `include/lib` layout
and the official configuration-specific `bin/<platform>/<configuration>`
layout. It also accepts the source tree root when `physx/include` is present.
Each platform must use its own PhysX artifacts; Windows `.lib/.dll` files are
never reused on Linux, macOS, or another target.

PhysXExtensions is detected separately and enables shape-derived mass/inertia
calculation. PhysXCooking is also detected separately; primitive shapes still
work when the optional Cooking library is absent. If PhysX is not available on
a target, the same public interface remains usable through the deterministic
cross-platform `Simple` backend.

## Simulation contract

`BodyDesc` supports static, dynamic, and kinematic rigid actors. A body can
contain Box, Sphere, Capsule, Plane, ConvexMesh, or TriangleMesh shapes, each
with a material and collision layer/mask. Mesh vertices and indices are
cooked during body creation and are not retained by the engine-facing
descriptor after the call.

The world provides fixed-step accumulation by default (`1/60`, at most eight
substeps), explicit transforms and velocities, force/torque application,
kinematic targets, filtered raycasts, reusable body-id enumeration, and
contact events (`Begin`, `Persist`, `End`). `drain_contact_events()` is the
allocation boundary for gameplay/editor inspection; a listener can be used
when immediate notification is preferred.

The PhysX adapter owns Foundation, Physics, CPU Dispatcher, Scene, actor and
callback lifetime in a private implementation. PhysX 5.x immediate cooking
functions are used for runtime convex/triangle mesh creation when the
Cooking library is present. PhysX types do not cross the public ABI. Scene
stepping remains synchronous at the engine boundary, while PhysX performs
broadphase/narrowphase/solver work on its configured dispatcher threads.

## Current boundary

The adapter currently covers rigid bodies and scene queries. Character
controllers, articulations, vehicles, cloth, particles, GPU dynamics, and
custom PhysX extensions should be added behind new narrow interfaces rather
than exposing `Px*` types through `IPhysicsWorld`.
