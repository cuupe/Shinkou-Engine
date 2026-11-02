cmake_minimum_required(VERSION 3.25)

project(ShinkouEngine VERSION 0.1.0 LANGUAGES C CXX)

set(SHINKOU_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/engine")
set(SHINKOU_SAMPLE_ROOT "${SHINKOU_ENGINE_ROOT}/samples")
set(SHINKOU_TEST_ROOT "${SHINKOU_ENGINE_ROOT}/tests")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SHINKOU_BUILD_SAMPLE "Build the engine sample" ON)
option(SHINKOU_BUILD_PHYSICS_VISUALIZER "Build the standalone rendered rigid-body visualizer" ON)
option(SHINKOU_BUILD_SHADER_SCENE_TEST "Build the standalone reference multi-pass shader scene test" ON)
option(SHINKOU_FETCH_DEPENDENCIES "Download pinned dependencies when not cached" ON)
option(SHINKOU_ENABLE_SDL3_INPUT "Enable SDL3 event input" ON)
option(SHINKOU_INPUT_FETCH_SDL3 "Fetch SDL3 source for the private ShinkouInput DLL" ON)
set(SHINKOU_INPUT_SDL3_SOURCE_DIR "" CACHE PATH "Optional local SDL3 source directory for ShinkouInput")
option(SHINKOU_ENABLE_MINIAUDIO "Enable miniaudio integration when available" ON)
option(SHINKOU_ENABLE_PHYSX "Enable PhysX integration when available" ON)
set(SHINKOU_PHYSX_ROOT "" CACHE PATH "PhysX SDK root containing include and lib directories")
set(SHINKOU_PHYSX_LIBRARY_DIR "" CACHE PATH "PhysX SDK library directory override")
option(SHINKOU_ENABLE_IMGUI "Enable ImGui editor integration when available" ON)
option(SHINKOU_ENABLE_LUA "Enable Lua scripting integration when available" ON)
option(SHINKOU_ENABLE_PYTHON "Enable Python scripting integration when available" ON)
option(SHINKOU_ENABLE_CSHARP "Enable .NET/C# capability detection" ON)
option(SHINKOU_BUILD_TESTS "Build engine tests" ON)
option(SHINKOU_BUILD_EDITOR "Build the dedicated native editor application" ON)
option(SHINKOU_BUILD_BENCHMARKS "Build engine microbenchmarks" OFF)
option(SHINKOU_BUILD_PHYSICS_EXAMPLE "Build the standalone physics library example" ON)
option(SHINKOU_BUILD_PHYSICS_PERFORMANCE_TEST "Build the PhysX dense performance test" ON)
option(SHINKOU_BUILD_AUDIO_MIXER_TEST "Build the native Win32 audio mixer test utility" ON)
option(SHINKOU_ENABLE_AVX2 "Enable AVX2 math kernels for supported targets" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ShinkouDependencies)
if(SHINKOU_BUILD_TESTS)
    include(CTest)
endif()
add_subdirectory(core)

# Assets are a standalone library so they can be tested and reused without
# pulling in the renderer, window system, or the rest of the engine.
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/assets" "${CMAKE_CURRENT_BINARY_DIR}/shinkou_assets")

if(SHINKOU_ENABLE_SDL3_INPUT)
    add_subdirectory("${SHINKOU_ENGINE_ROOT}/input" "${CMAKE_CURRENT_BINARY_DIR}/shinkou_input" EXCLUDE_FROM_ALL)
endif()

add_subdirectory("${SHINKOU_ENGINE_ROOT}/physics" "${CMAKE_CURRENT_BINARY_DIR}/shinkou_physics")

# PhysX is a shared runtime on the supported native builds. Keep executable
# output directories self-contained so examples and tools run directly after
# a build, without requiring the user to edit PATH manually.
function(shinkou_copy_physx_runtime target_name)
    if(NOT WIN32 OR NOT SHINKOU_WITH_PHYSX OR NOT SHINKOU_PHYSX_ROOT OR NOT TARGET ${target_name})
        return()
    endif()
    file(GLOB_RECURSE SHINKOU_PHYSX_RUNTIME_DLLS LIST_DIRECTORIES false
        "${SHINKOU_PHYSX_ROOT}/bin/*.dll"
        "${SHINKOU_PHYSX_ROOT}/lib/*.dll")
    foreach(SHINKOU_PHYSX_RUNTIME_DLL IN LISTS SHINKOU_PHYSX_RUNTIME_DLLS)
        get_filename_component(SHINKOU_PHYSX_RUNTIME_NAME
            "${SHINKOU_PHYSX_RUNTIME_DLL}" NAME)
        add_custom_command(TARGET ${target_name} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SHINKOU_PHYSX_RUNTIME_DLL}"
                "$<TARGET_FILE_DIR:${target_name}>/${SHINKOU_PHYSX_RUNTIME_NAME}"
            VERBATIM)
    endforeach()
endfunction()

file(GLOB_RECURSE SHINKOU_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${SHINKOU_ENGINE_ROOT}/src/*.cpp")
list(FILTER SHINKOU_ENGINE_SOURCES EXCLUDE REGEX "/engine/src/physics/")
list(FILTER SHINKOU_ENGINE_SOURCES EXCLUDE REGEX "/engine/src/coroutines/")
list(FILTER SHINKOU_ENGINE_SOURCES EXCLUDE REGEX "/engine/src/log/")
# The unified platform/system layer builds as its own small library so it can be
# tested (and reused) without the renderer, editor or window system.
list(FILTER SHINKOU_ENGINE_SOURCES EXCLUDE REGEX "/engine/src/platform/system/")
# Keep the standalone visualizer build independent from an editor translation
# unit that is being completed in another task, while still linking its small
# file-history data implementation below.
list(FILTER SHINKOU_ENGINE_SOURCES EXCLUDE REGEX "FileHistory\\.cpp$")

add_library(shinkou_engine STATIC ${SHINKOU_ENGINE_SOURCES})
add_library(shinkou_logging STATIC "${SHINKOU_ENGINE_ROOT}/src/log/Log.cpp")
target_include_directories(shinkou_logging PUBLIC "${SHINKOU_ENGINE_ROOT}/include")
target_compile_features(shinkou_logging PUBLIC cxx_std_17)
target_link_libraries(shinkou_engine PUBLIC shinkou_logging ShinkouEngine::Core)
# Keep C++20 coroutine machinery behind a C++17 scheduler facade, so existing
# engine clients do not need a language-standard migration.
find_package(Threads REQUIRED)

file(GLOB_RECURSE SHINKOU_PLATFORM_SYSTEM_SOURCES CONFIGURE_DEPENDS
    "${SHINKOU_ENGINE_ROOT}/src/platform/system/*.cpp")
# The native backends live in platform-specific directories; CMake selects the
# one that matches the host so the same glob works on every target.
if(WIN32)
    list(FILTER SHINKOU_PLATFORM_SYSTEM_SOURCES EXCLUDE REGEX "/platform/system/posix/")
elseif(UNIX AND NOT ANDROID AND NOT EMSCRIPTEN)
    list(FILTER SHINKOU_PLATFORM_SYSTEM_SOURCES EXCLUDE REGEX "/platform/system/windows/")
else()
    list(FILTER SHINKOU_PLATFORM_SYSTEM_SOURCES EXCLUDE REGEX "/platform/system/(posix|windows)/")
endif()
add_library(shinkou_platform_system STATIC ${SHINKOU_PLATFORM_SYSTEM_SOURCES})
add_library(ShinkouEngine::PlatformSystem ALIAS shinkou_platform_system)
target_include_directories(shinkou_platform_system PUBLIC "${SHINKOU_ENGINE_ROOT}/include")
target_compile_features(shinkou_platform_system PUBLIC cxx_std_17)
target_link_libraries(shinkou_platform_system PUBLIC Threads::Threads ShinkouEngine::Core)
if(UNIX AND NOT ANDROID AND NOT EMSCRIPTEN)
    # The POSIX backend uses dlopen and the portable fallback uses it when no
    # native backend exists.
    target_link_libraries(shinkou_platform_system PUBLIC ${CMAKE_DL_LIBS})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # shm_open lives in librt on glibc before 2.34; the stub library is
        # harmless on newer glibc.
        find_library(SHINKOU_PLATFORM_RT_LIBRARY rt)
        if(SHINKOU_PLATFORM_RT_LIBRARY)
            target_link_libraries(shinkou_platform_system PUBLIC ${SHINKOU_PLATFORM_RT_LIBRARY})
        endif()
    endif()
    if(APPLE)
        target_compile_definitions(shinkou_platform_system PRIVATE _DARWIN_C_SOURCE=1)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # pthread_setname_np / pthread_setaffinity_np and ucontext need the GNU
        # interface on Linux.
        target_compile_definitions(shinkou_platform_system PRIVATE _GNU_SOURCE=1)
    else()
        target_compile_definitions(shinkou_platform_system PRIVATE _XOPEN_SOURCE=700)
    endif()
endif()
if(MSVC)
    target_compile_options(shinkou_platform_system PRIVATE /W4 /permissive-)
else()
    target_compile_options(shinkou_platform_system PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(shinkou_coroutine_runtime STATIC "${SHINKOU_ENGINE_ROOT}/src/coroutines/Scheduler.cpp")
target_include_directories(shinkou_coroutine_runtime PUBLIC "${SHINKOU_ENGINE_ROOT}/include")
target_compile_features(shinkou_coroutine_runtime PRIVATE cxx_std_20 PUBLIC cxx_std_17)
target_link_libraries(shinkou_coroutine_runtime PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(shinkou_coroutine_runtime PRIVATE /W4 /permissive-)
else()
    target_compile_options(shinkou_coroutine_runtime PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_library(shinkou_coroutines INTERFACE)
add_library(ShinkouEngine::Coroutines ALIAS shinkou_coroutines)
target_compile_features(shinkou_coroutines INTERFACE cxx_std_20)
target_link_libraries(shinkou_coroutines INTERFACE shinkou_engine)
include(ShinkouBuiltinShaders)
# The parallel physics worktree temporarily filters this editor data unit from
# the source glob; keep the engine target link-complete in that configuration.
target_sources(shinkou_engine PRIVATE
    "${SHINKOU_ENGINE_ROOT}/src/editor/FileHistory.cpp")
set_target_properties(shinkou_engine PROPERTIES OUTPUT_NAME shinkou_engine EXPORT_NAME Engine)
add_library(ShinkouEngine::Engine ALIAS shinkou_engine)
target_link_libraries(shinkou_engine PUBLIC shinkou_assets shinkou_physics shinkou_coroutine_runtime shinkou_platform_system PRIVATE shinkou_asset_readers)
if(WIN32)
    target_link_libraries(shinkou_engine PRIVATE comdlg32 shell32)
endif()
target_include_directories(shinkou_engine PUBLIC
    "${SHINKOU_ENGINE_ROOT}/include"
)
target_compile_features(shinkou_engine PUBLIC cxx_std_17)
target_compile_definitions(shinkou_engine PRIVATE
    SHINKOU_EDITOR_ICON_FONT_SOURCE="${codicons_SOURCE_DIR}/dist/codicon.ttf"
    SHINKOU_EDITOR_ICON_FONT_INSTALL="${CMAKE_INSTALL_PREFIX}/share/shinkou/editor/fonts/codicon.ttf")

if(SHINKOU_ENABLE_AVX2)
    if(MSVC)
        target_compile_options(shinkou_engine PRIVATE /arch:AVX2)
    else()
        # MinGW's Win64 ABI guarantees 16-byte stack alignment; AVX2 register
        # spills may require 32-byte alignment, so realign functions that use
        # the optional backend.
        target_compile_options(shinkou_engine PRIVATE -mavx2 -mstackrealign)
    endif()
endif()


if(SHINKOU_WITH_SPDLOG)
    target_link_libraries(shinkou_logging PUBLIC ${SHINKOU_SPDLOG_TARGET})
    target_compile_definitions(shinkou_logging PUBLIC SHINKOU_WITH_SPDLOG=1)
else()
    target_compile_definitions(shinkou_logging PUBLIC SHINKOU_WITH_SPDLOG=0)
endif()

if(MSVC)
    target_compile_options(shinkou_engine PRIVATE /W4 /permissive-)
    target_compile_options(shinkou_logging PRIVATE /W4 /permissive-)
else()
    target_compile_options(shinkou_engine PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(shinkou_logging PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SHINKOU_ENABLE_SDL3_INPUT)
    target_link_libraries(shinkou_engine PRIVATE ShinkouInput::ShinkouInput)
    target_compile_definitions(shinkou_engine PRIVATE SHINKOU_WITH_INPUT=1)
endif()

if(SHINKOU_WITH_VULKAN)
    target_link_libraries(shinkou_engine PUBLIC Vulkan::Vulkan)
    target_compile_definitions(shinkou_engine PUBLIC SHINKOU_WITH_VULKAN=1)
    if(WIN32)
        target_compile_definitions(shinkou_engine PRIVATE VK_USE_PLATFORM_WIN32_KHR=1)
    endif()
endif()

if(SHINKOU_WITH_IMGUI)
    target_include_directories(shinkou_engine PRIVATE "${IMGUI_INCLUDE_DIR}")
    if(IMGUI_LIBRARY)
        target_link_libraries(shinkou_engine PRIVATE "${IMGUI_LIBRARY}")
    endif()
    if(TARGET shinkou_editor_widgets)
        target_link_libraries(shinkou_engine PRIVATE shinkou_editor_widgets)
    endif()
    target_compile_definitions(shinkou_engine PRIVATE SHINKOU_WITH_IMGUI=1)
endif()

if(SHINKOU_WITH_MINIAUDIO)
    target_include_directories(shinkou_engine PRIVATE "${MINIAUDIO_INCLUDE_DIR}")
    target_compile_definitions(shinkou_engine PRIVATE SHINKOU_WITH_MINIAUDIO=1)
endif()

if(SHINKOU_WITH_LUA)
    target_include_directories(shinkou_engine PRIVATE "${LUA_INCLUDE_DIR}")
    target_link_libraries(shinkou_engine PRIVATE ${LUA_LIBRARIES})
    target_compile_definitions(shinkou_engine PRIVATE SHINKOU_WITH_LUA=1)
endif()

if(SHINKOU_WITH_PYTHON)
    target_include_directories(shinkou_engine PRIVATE ${Python3_INCLUDE_DIRS})
    target_link_libraries(shinkou_engine PRIVATE Python3::Python)
    target_compile_definitions(shinkou_engine PRIVATE SHINKOU_WITH_PYTHON=1)
endif()

if(SHINKOU_WITH_CSHARP)
    target_compile_definitions(shinkou_engine PRIVATE SHINKOU_WITH_CSHARP=1)
endif()

if(SHINKOU_WITH_CSHARP)
    set(SHINKOU_CSHARP_SOLUTION "${SHINKOU_ENGINE_ROOT}/scripting/Shinkou.Scripting.sln")
    add_custom_target(shinkou_csharp
        COMMAND "${SHINKOU_DOTNET_EXECUTABLE}" build "${SHINKOU_CSHARP_SOLUTION}" --configuration Release --nologo
        WORKING_DIRECTORY "${SHINKOU_ENGINE_ROOT}/scripting"
        COMMENT "Building Shinkou C# scripting runtime and compiler"
        VERBATIM
    )
endif()

if(SHINKOU_BUILD_PHYSICS_VISUALIZER)
    set(SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT
        "${SHINKOU_ENGINE_ROOT}/physics/examples/shaders")
    set(SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/shinkou_physics_visualizer_shaders")
    set(SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUTS)
    find_program(SHINKOU_PHYSICS_VISUALIZER_DXC_EXECUTABLE NAMES dxc dxc.exe)
    find_program(SHINKOU_PHYSICS_VISUALIZER_GLSLC_EXECUTABLE NAMES glslc glslc.exe)
    if(SHINKOU_PHYSICS_VISUALIZER_DXC_EXECUTABLE)
        add_custom_command(
            OUTPUT
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_vs.dxil"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_ps.dxil"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_vs.dxil"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_ps.dxil"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}"
            COMMAND "${SHINKOU_PHYSICS_VISUALIZER_DXC_EXECUTABLE}" -E MeshVS -T vs_6_0
                -Fo "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_vs.dxil"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer.hlsl"
            COMMAND "${SHINKOU_PHYSICS_VISUALIZER_DXC_EXECUTABLE}" -E MeshPS -T ps_6_0
                -Fo "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_ps.dxil"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer.hlsl"
            COMMAND "${SHINKOU_PHYSICS_VISUALIZER_DXC_EXECUTABLE}" -E PresentVS -T vs_6_0
                -Fo "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_vs.dxil"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer.hlsl"
            COMMAND "${SHINKOU_PHYSICS_VISUALIZER_DXC_EXECUTABLE}" -E PresentPS -T ps_6_0
                -Fo "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_ps.dxil"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer.hlsl"
            DEPENDS "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer.hlsl"
            VERBATIM)
        list(APPEND SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUTS
            "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_vs.dxil"
            "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_ps.dxil"
            "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_vs.dxil"
            "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_ps.dxil")
    endif()
    if(SHINKOU_PHYSICS_VISUALIZER_GLSLC_EXECUTABLE)
        add_custom_command(
            OUTPUT
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_vs.spv"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_ps.spv"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_vs.spv"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_ps.spv"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}"
            COMMAND "${SHINKOU_PHYSICS_VISUALIZER_GLSLC_EXECUTABLE}" -fshader-stage=vert
                -o "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_vs.spv"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer_mesh.vert"
            COMMAND "${SHINKOU_PHYSICS_VISUALIZER_GLSLC_EXECUTABLE}" -fshader-stage=frag
                -o "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_ps.spv"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer_mesh.frag"
            COMMAND "${SHINKOU_PHYSICS_VISUALIZER_GLSLC_EXECUTABLE}" -fshader-stage=vert
                -o "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_vs.spv"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer_present.vert"
            COMMAND "${SHINKOU_PHYSICS_VISUALIZER_GLSLC_EXECUTABLE}" -fshader-stage=frag
                -o "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_ps.spv"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer_present.frag"
            DEPENDS
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer_mesh.vert"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer_mesh.frag"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer_present.vert"
                "${SHINKOU_PHYSICS_VISUALIZER_SHADER_ROOT}/physics_visualizer_present.frag"
            VERBATIM)
        list(APPEND SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUTS
            "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_vs.spv"
            "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_mesh_ps.spv"
            "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_vs.spv"
            "${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}/physics_visualizer_present_ps.spv")
    endif()
    add_executable(shinkou_physics_visualizer
        "${SHINKOU_ENGINE_ROOT}/physics/examples/PhysicsVisualizer.cpp")
    target_sources(shinkou_physics_visualizer PRIVATE
        "${SHINKOU_ENGINE_ROOT}/src/editor/FileHistory.cpp")
    set_target_properties(shinkou_physics_visualizer PROPERTIES OUTPUT_NAME shinkou_physics_visualizer)
    target_link_libraries(shinkou_physics_visualizer PRIVATE shinkou_engine)
    target_compile_definitions(shinkou_physics_visualizer PRIVATE
        SHINKOU_PHYSICS_SHADER_DIR="${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUT_DIR}")
    if(SHINKOU_WITH_IMGUI)
        # The standalone visualizer owns its ImGui context and input bridge;
        # keep this opt-in so the physics library remains UI-independent.
        target_include_directories(shinkou_physics_visualizer PRIVATE "${IMGUI_INCLUDE_DIR}")
        target_compile_definitions(shinkou_physics_visualizer PRIVATE SHINKOU_PHYSICS_VISUALIZER_WITH_IMGUI=1)
        if(TARGET shinkou_imgui)
            target_link_libraries(shinkou_physics_visualizer PRIVATE shinkou_imgui)
        elseif(IMGUI_LIBRARY)
            target_link_libraries(shinkou_physics_visualizer PRIVATE "${IMGUI_LIBRARY}")
        endif()
    endif()
    if(SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUTS)
        add_custom_target(shinkou_physics_visualizer_shaders
            DEPENDS ${SHINKOU_PHYSICS_VISUALIZER_SHADER_OUTPUTS})
        add_dependencies(shinkou_physics_visualizer shinkou_physics_visualizer_shaders)
    endif()
    if(WIN32)
        target_link_libraries(shinkou_physics_visualizer PRIVATE ws2_32)
    endif()
    if(MINGW)
        target_link_options(shinkou_physics_visualizer PRIVATE
            -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
        get_filename_component(SHINKOU_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(SHINKOU_RUNTIME_DLL libwinpthread-1.dll)
            if(EXISTS "${SHINKOU_MINGW_BIN}/${SHINKOU_RUNTIME_DLL}")
                add_custom_command(TARGET shinkou_physics_visualizer POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${SHINKOU_MINGW_BIN}/${SHINKOU_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:shinkou_physics_visualizer>/${SHINKOU_RUNTIME_DLL}"
                    VERBATIM)
            endif()
        endforeach()
    endif()
    shinkou_copy_physx_runtime(shinkou_physics_visualizer)
endif()

if(WIN32)
    target_link_libraries(shinkou_engine PUBLIC d3d11 d3d12 dxgi d3dcompiler dxguid windowscodecs mfplat mfreadwrite mfuuid propsys gdi32 ws2_32)
    # Keep the Windows SDK from defining min/max macros in the engine's
    # portable C++ sources. This is especially important for the editor
    # translation units, which are also compiled into non-editor tools such as
    # the physics visualizer.
    target_compile_definitions(shinkou_engine PRIVATE
        SHINKOU_PLATFORM_WINDOWS=1
        NOMINMAX=1
        WIN32_LEAN_AND_MEAN=1)

    if(SHINKOU_BUILD_AUDIO_MIXER_TEST)
        # This executable owns a normal Win32 window and uses only the
        # AudioSystem API. It intentionally does not create an Engine window,
        # renderer, swap chain, or any DX11/DX12/Vulkan presentation surface.
        add_executable(shinkou_audio_mixer_test WIN32
            "${CMAKE_CURRENT_SOURCE_DIR}/Tools/AudioMixerTest/AudioMixerTest.cpp")
        target_compile_features(shinkou_audio_mixer_test PRIVATE cxx_std_17)
        target_compile_definitions(shinkou_audio_mixer_test PRIVATE
            NOMINMAX=1
            WIN32_LEAN_AND_MEAN=1)
        target_link_libraries(shinkou_audio_mixer_test PRIVATE
            shinkou_engine comctl32 comdlg32 gdi32 user32 ole32 avrt winmm)
        set_target_properties(shinkou_audio_mixer_test PROPERTIES
            OUTPUT_NAME shinkou_audio_mixer_test)
    endif()
endif()

if(SHINKOU_BUILD_SAMPLE)
    find_program(SHINKOU_DXC_EXECUTABLE NAMES dxc dxc.exe)
    find_program(SHINKOU_GLSLC_EXECUTABLE NAMES glslc glslc.exe)
    set(SHINKOU_SAMPLE_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shinkou_sample_shaders")
    set(SHINKOU_SAMPLE_SHADER_OUTPUTS)
    if(SHINKOU_DXC_EXECUTABLE)
        add_custom_command(
            OUTPUT "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_vs.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_ps.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_vs.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_ps.dxil"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/sample_mesh.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E CSMain -T cs_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/gpu_culling.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_vs.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/ocean.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_ps.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/ocean.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_vs.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/buoy.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_ps.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/buoy.hlsl"
            DEPENDS "${SHINKOU_SAMPLE_ROOT}/shaders/sample.hlsl" "${SHINKOU_SAMPLE_ROOT}/shaders/sample_mesh.hlsl" "${SHINKOU_SAMPLE_ROOT}/shaders/gpu_culling.hlsl" "${SHINKOU_SAMPLE_ROOT}/shaders/ocean.hlsl" "${SHINKOU_SAMPLE_ROOT}/shaders/buoy.hlsl"
            VERBATIM
        )
        list(APPEND SHINKOU_SAMPLE_SHADER_OUTPUTS
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.dxil"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.dxil"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.dxil"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_vs.dxil"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_ps.dxil"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_vs.dxil"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_ps.dxil")
            list(APPEND SHINKOU_SAMPLE_SHADER_OUTPUTS "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.dxil")
    endif()
    if(SHINKOU_GLSLC_EXECUTABLE)
        add_custom_command(
            OUTPUT "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_vs.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_ps.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_vs.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_ps.spv"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=vert -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.vert"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=vert -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/sample_mesh.vert"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=frag -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.frag"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=comp -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/gpu_culling.comp"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=vert -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_vs.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/ocean.vert"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=frag -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_ps.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/ocean.frag"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=vert -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_vs.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/buoy.vert"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=frag -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_ps.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/buoy.frag"
            DEPENDS "${SHINKOU_SAMPLE_ROOT}/shaders/sample.vert" "${SHINKOU_SAMPLE_ROOT}/shaders/sample_mesh.vert" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.frag" "${SHINKOU_SAMPLE_ROOT}/shaders/gpu_culling.comp" "${SHINKOU_SAMPLE_ROOT}/shaders/ocean.vert" "${SHINKOU_SAMPLE_ROOT}/shaders/ocean.frag" "${SHINKOU_SAMPLE_ROOT}/shaders/buoy.vert" "${SHINKOU_SAMPLE_ROOT}/shaders/buoy.frag"
            VERBATIM
        )
        list(APPEND SHINKOU_SAMPLE_SHADER_OUTPUTS
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.spv"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.spv"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.spv"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_vs.spv"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/ocean_ps.spv"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_vs.spv"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/buoy_ps.spv")
            list(APPEND SHINKOU_SAMPLE_SHADER_OUTPUTS "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.spv")
    endif()
    if(SHINKOU_SAMPLE_SHADER_OUTPUTS)
        add_custom_target(shinkou_sample_shaders DEPENDS ${SHINKOU_SAMPLE_SHADER_OUTPUTS})
    endif()
    add_executable(shinkou_engine_sample "${SHINKOU_SAMPLE_ROOT}/EngineSample.cpp")
    set_target_properties(shinkou_engine_sample PROPERTIES OUTPUT_NAME shinkou_engine_sample)
    target_link_libraries(shinkou_engine_sample PRIVATE shinkou_engine)
    if(WIN32)
        target_link_libraries(shinkou_engine_sample PRIVATE ws2_32)
    endif()
    target_compile_definitions(shinkou_engine_sample PRIVATE
        SHINKOU_SAMPLE_SHADER_DIR="${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}"
        SHINKOU_SAMPLE_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
    add_executable(shinkou_ocean_demo "${SHINKOU_SAMPLE_ROOT}/OceanSample.cpp")
    set_target_properties(shinkou_ocean_demo PROPERTIES OUTPUT_NAME shinkou_ocean_demo)
    target_link_libraries(shinkou_ocean_demo PRIVATE shinkou_engine)
    if(WIN32)
        target_link_libraries(shinkou_ocean_demo PRIVATE ws2_32)
    endif()
    target_compile_definitions(shinkou_ocean_demo PRIVATE
        SHINKOU_SAMPLE_SHADER_DIR="${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}"
        SHINKOU_SAMPLE_SOURCE_DIR="${SHINKOU_SAMPLE_ROOT}")
    if(SHINKOU_WITH_IMGUI)
        target_include_directories(shinkou_ocean_demo PRIVATE "${IMGUI_INCLUDE_DIR}")
        target_compile_definitions(shinkou_ocean_demo PRIVATE SHINKOU_OCEAN_WITH_IMGUI=1)
        if(TARGET shinkou_imgui)
            target_link_libraries(shinkou_ocean_demo PRIVATE shinkou_imgui)
        elseif(IMGUI_LIBRARY)
            target_link_libraries(shinkou_ocean_demo PRIVATE "${IMGUI_LIBRARY}")
        endif()
    endif()
    if(MINGW)
        target_link_options(shinkou_engine_sample PRIVATE
            -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
        target_link_options(shinkou_ocean_demo PRIVATE
            -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
        get_filename_component(SHINKOU_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
        foreach(SHINKOU_RUNTIME_DLL libwinpthread-1.dll)
            if(EXISTS "${SHINKOU_MINGW_BIN}/${SHINKOU_RUNTIME_DLL}")
                add_custom_command(TARGET shinkou_engine_sample POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${SHINKOU_MINGW_BIN}/${SHINKOU_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:shinkou_engine_sample>/${SHINKOU_RUNTIME_DLL}"
                    VERBATIM)
                add_custom_command(TARGET shinkou_ocean_demo POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${SHINKOU_MINGW_BIN}/${SHINKOU_RUNTIME_DLL}"
                        "$<TARGET_FILE_DIR:shinkou_ocean_demo>/${SHINKOU_RUNTIME_DLL}"
                    VERBATIM)
            endif()
        endforeach()
    endif()
    if(TARGET shinkou_sample_shaders)
        add_dependencies(shinkou_engine_sample shinkou_sample_shaders)
        add_dependencies(shinkou_ocean_demo shinkou_sample_shaders)
    endif()
endif()

if(WIN32)
    # Native capture is part of the UI QA workflow. It intentionally has no
    # engine dependency so it can diagnose a broken editor window as well as
    # validate a working one.
    add_executable(shinkou_ui_capture "${CMAKE_CURRENT_SOURCE_DIR}/Tools/UiCapture/WindowCapture.cpp")
    target_compile_features(shinkou_ui_capture PRIVATE cxx_std_17)
    target_link_libraries(shinkou_ui_capture PRIVATE user32 gdi32 dwmapi shell32)
    set_target_properties(shinkou_ui_capture PROPERTIES OUTPUT_NAME shinkou_ui_capture)
endif()

if(SHINKOU_BUILD_SHADER_SCENE_TEST)
    find_program(SHINKOU_SHADER_SCENE_DXC_EXECUTABLE NAMES dxc dxc.exe)
    find_program(SHINKOU_SHADER_SCENE_GLSLC_EXECUTABLE NAMES glslc glslc.exe)
    set(SHINKOU_SHADER_SCENE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shinkou_shader_scene_shaders")
    set(SHINKOU_SHADER_SCENE_OUTPUTS)
    if(SHINKOU_SHADER_SCENE_DXC_EXECUTABLE)
        add_custom_command(
            OUTPUT
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_vs.dxil"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_ps.dxil"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_post_b.dxil"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_h.dxil"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_v.dxil"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_image_pass.dxil"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}"
            COMMAND "${SHINKOU_SHADER_SCENE_DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -Fo "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_vs.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/shader_scene.hlsl"
            COMMAND "${SHINKOU_SHADER_SCENE_DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_ps.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/shader_scene.hlsl"
            COMMAND "${SHINKOU_SHADER_SCENE_DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_post_b.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/reference_post_b.hlsl"
            COMMAND "${SHINKOU_SHADER_SCENE_DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_h.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_h.hlsl"
            COMMAND "${SHINKOU_SHADER_SCENE_DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_v.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_v.hlsl"
            COMMAND "${SHINKOU_SHADER_SCENE_DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_image_pass.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/reference_image_pass.hlsl"
            DEPENDS
                "${SHINKOU_SAMPLE_ROOT}/shaders/shader_scene.hlsl"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_post_b.hlsl"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_h.hlsl"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_v.hlsl"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_image_pass.hlsl"
            VERBATIM
        )
        list(APPEND SHINKOU_SHADER_SCENE_OUTPUTS
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_vs.dxil"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_ps.dxil"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_post_b.dxil"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_h.dxil"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_v.dxil"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_image_pass.dxil")
    endif()
    if(SHINKOU_SHADER_SCENE_GLSLC_EXECUTABLE)
        add_custom_command(
            OUTPUT
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_vs.spv"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_ps.spv"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_post_b.spv"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_h.spv"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_v.spv"
                "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_image_pass.spv"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}"
            COMMAND "${SHINKOU_SHADER_SCENE_GLSLC_EXECUTABLE}" -fshader-stage=vert -o "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_vs.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/shader_scene.vert"
            COMMAND "${SHINKOU_SHADER_SCENE_GLSLC_EXECUTABLE}" -fshader-stage=frag -o "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_ps.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blackhole.frag"
            COMMAND "${SHINKOU_SHADER_SCENE_GLSLC_EXECUTABLE}" -fshader-stage=frag -o "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_post_b.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/reference_post_b.frag"
            COMMAND "${SHINKOU_SHADER_SCENE_GLSLC_EXECUTABLE}" -fshader-stage=frag -o "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_h.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_h.frag"
            COMMAND "${SHINKOU_SHADER_SCENE_GLSLC_EXECUTABLE}" -fshader-stage=frag -o "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_v.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_v.frag"
            COMMAND "${SHINKOU_SHADER_SCENE_GLSLC_EXECUTABLE}" -fshader-stage=frag -o "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_image_pass.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/reference_image_pass.frag"
            DEPENDS
                "${SHINKOU_SAMPLE_ROOT}/shaders/shader_scene.vert"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blackhole.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_post_b.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_h.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_v.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_image_pass.frag"
            VERBATIM
        )
        list(APPEND SHINKOU_SHADER_SCENE_OUTPUTS
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_vs.spv"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/shader_scene_ps.spv"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_post_b.spv"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_h.spv"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_blur_v.spv"
            "${SHINKOU_SHADER_SCENE_OUTPUT_DIR}/reference_image_pass.spv")
    endif()
    if(SHINKOU_SHADER_SCENE_OUTPUTS)
        add_custom_target(shinkou_shader_scene_shaders
            DEPENDS ${SHINKOU_SHADER_SCENE_OUTPUTS}
            SOURCES
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blackhole.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_buffer_b.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_buffer_c.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_buffer_d.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_image.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_post_b.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_h.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_v.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_image_pass.frag"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_post_b.hlsl"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_h.hlsl"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_blur_v.hlsl"
                "${SHINKOU_SAMPLE_ROOT}/shaders/reference_image_pass.hlsl")
    endif()
    add_executable(shinkou_shader_scene_test "${SHINKOU_TEST_ROOT}/ShaderSceneTest.cpp")
    set_target_properties(shinkou_shader_scene_test PROPERTIES OUTPUT_NAME shinkou_shader_scene_test)
    target_link_libraries(shinkou_shader_scene_test PRIVATE shinkou_engine)
    target_compile_definitions(shinkou_shader_scene_test PRIVATE
        SHINKOU_SHADER_SCENE_ASSET_DIR="${SHINKOU_SHADER_SCENE_OUTPUT_DIR}"
        SHINKOU_SHADER_SCENE_SOURCE_DIR="${SHINKOU_SAMPLE_ROOT}/shaders")
    if(TARGET shinkou_shader_scene_shaders)
        add_dependencies(shinkou_shader_scene_test shinkou_shader_scene_shaders)
    endif()
endif()

if(SHINKOU_BUILD_EDITOR AND SHINKOU_WITH_IMGUI)
    add_executable(shinkou_editor WIN32 "${CMAKE_CURRENT_SOURCE_DIR}/engine/editor/EditorMain.cpp")
    target_link_libraries(shinkou_editor PRIVATE shinkou_engine)
    target_compile_definitions(shinkou_editor PRIVATE SHINKOU_EDITOR_DEFAULT_PROJECT="${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(shinkou_editor PRIVATE shell32)
    endif()
    if(MINGW)
        target_link_options(shinkou_editor PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    endif()
endif()

if(SHINKOU_BUILD_TESTS)
    if(BUILD_TESTING)
        add_executable(shinkou_coroutine_tests
            "${SHINKOU_TEST_ROOT}/CoroutineTests.cpp"
            "${SHINKOU_ENGINE_ROOT}/src/jobs/JobSystem.cpp")
        target_compile_features(shinkou_coroutine_tests PRIVATE cxx_std_20)
        target_link_libraries(shinkou_coroutine_tests PRIVATE shinkou_coroutine_runtime)
        add_test(NAME shinkou_coroutine_tests COMMAND shinkou_coroutine_tests)
        set_tests_properties(shinkou_coroutine_tests PROPERTIES TIMEOUT 60)
        add_executable(shinkou_engine_coroutine_tests "${SHINKOU_TEST_ROOT}/EngineCoroutineTests.cpp")
        target_link_libraries(shinkou_engine_coroutine_tests PRIVATE ShinkouEngine::Coroutines)
        add_test(NAME shinkou_engine_coroutine_tests COMMAND shinkou_engine_coroutine_tests)
        set_tests_properties(shinkou_engine_coroutine_tests PROPERTIES TIMEOUT 30)
        add_executable(shinkou_render_graph_tests "${SHINKOU_TEST_ROOT}/RenderGraphTests.cpp")
        target_link_libraries(shinkou_render_graph_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_render_graph_tests COMMAND shinkou_render_graph_tests)

        add_executable(shinkou_render_geometry_tests "${SHINKOU_TEST_ROOT}/RenderGeometryTests.cpp")
        target_link_libraries(shinkou_render_geometry_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_render_geometry_tests COMMAND shinkou_render_geometry_tests)

        add_executable(shinkou_gpu_memory_allocator_tests "${SHINKOU_TEST_ROOT}/GpuMemoryAllocatorTests.cpp")
        target_link_libraries(shinkou_gpu_memory_allocator_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_gpu_memory_allocator_tests COMMAND shinkou_gpu_memory_allocator_tests)

        add_executable(shinkou_texture_model_tests "${SHINKOU_TEST_ROOT}/TextureModelTests.cpp")
        target_link_libraries(shinkou_texture_model_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_texture_model_tests COMMAND shinkou_texture_model_tests)

        add_executable(shinkou_texture_backend_mapping_tests "${SHINKOU_TEST_ROOT}/TextureBackendMappingTests.cpp")
        target_link_libraries(shinkou_texture_backend_mapping_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_texture_backend_mapping_tests COMMAND shinkou_texture_backend_mapping_tests)

        add_executable(shinkou_pbr_render_pipeline_tests "${SHINKOU_TEST_ROOT}/PbrRenderPipelineTests.cpp")
        target_link_libraries(shinkou_pbr_render_pipeline_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_pbr_render_pipeline_tests COMMAND shinkou_pbr_render_pipeline_tests)

        add_executable(shinkou_lighting_pipeline_tests "${SHINKOU_TEST_ROOT}/LightingPipelineTests.cpp")
        target_link_libraries(shinkou_lighting_pipeline_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_lighting_pipeline_tests COMMAND shinkou_lighting_pipeline_tests)

        add_executable(shinkou_shader_pipeline_library_tests "${SHINKOU_TEST_ROOT}/ShaderPipelineLibraryTests.cpp")
        target_link_libraries(shinkou_shader_pipeline_library_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_shader_pipeline_library_tests COMMAND shinkou_shader_pipeline_library_tests)

        add_executable(shinkou_render_capture_budget_tests "${SHINKOU_TEST_ROOT}/RenderCaptureBudgetTests.cpp")
        target_link_libraries(shinkou_render_capture_budget_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_render_capture_budget_tests COMMAND shinkou_render_capture_budget_tests)

        add_executable(shinkou_advanced_render_features_tests "${SHINKOU_TEST_ROOT}/AdvancedRenderFeaturesTests.cpp")
        target_link_libraries(shinkou_advanced_render_features_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_advanced_render_features_tests COMMAND shinkou_advanced_render_features_tests)

        add_executable(shinkou_render_feature_graph_builder_tests "${SHINKOU_TEST_ROOT}/RenderFeatureGraphBuilderTests.cpp")
        target_link_libraries(shinkou_render_feature_graph_builder_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_render_feature_graph_builder_tests COMMAND shinkou_render_feature_graph_builder_tests)

        add_executable(shinkou_render_pipeline_customization_tests "${SHINKOU_TEST_ROOT}/RenderPipelineCustomizationTests.cpp")
        target_link_libraries(shinkou_render_pipeline_customization_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_render_pipeline_customization_tests COMMAND shinkou_render_pipeline_customization_tests)

        add_executable(shinkou_render_pipeline_execution_tests "${SHINKOU_TEST_ROOT}/RenderPipelineExecutionTests.cpp")
        target_link_libraries(shinkou_render_pipeline_execution_tests PRIVATE shinkou_engine)
        target_compile_definitions(shinkou_render_pipeline_execution_tests PRIVATE
            SHINKOU_PIPELINE_TEST_SHADER="${SHINKOU_TEST_ROOT}/shaders/PipelineTransform.hlsl"
            SHINKOU_PIPELINE_TEST_SPIRV="${CMAKE_CURRENT_BINARY_DIR}/pipeline_transform.spv")
        if(SHINKOU_WITH_VULKAN AND SHINKOU_BUILTIN_GLSLC)
            add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/pipeline_transform.spv"
                COMMAND "${SHINKOU_BUILTIN_GLSLC}" -x hlsl -fshader-stage=frag
                    -fentry-point=main -DSHINKOU_SPIRV=1
                    -o "${CMAKE_CURRENT_BINARY_DIR}/pipeline_transform.spv"
                    "${SHINKOU_TEST_ROOT}/shaders/PipelineTransform.hlsl"
                DEPENDS "${SHINKOU_TEST_ROOT}/shaders/PipelineTransform.hlsl" VERBATIM)
            add_custom_target(shinkou_pipeline_test_shaders DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/pipeline_transform.spv")
            add_dependencies(shinkou_render_pipeline_execution_tests shinkou_pipeline_test_shaders)
        endif()
        add_test(NAME shinkou_render_pipeline_execution_tests COMMAND shinkou_render_pipeline_execution_tests)
        set_tests_properties(shinkou_render_pipeline_execution_tests PROPERTIES SKIP_RETURN_CODE 77)
        add_test(NAME shinkou_render_pipeline_dx12_tests COMMAND shinkou_render_pipeline_execution_tests dx12)
        add_test(NAME shinkou_render_pipeline_vulkan_tests COMMAND shinkou_render_pipeline_execution_tests vulkan)
        set_tests_properties(shinkou_render_pipeline_dx12_tests shinkou_render_pipeline_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77)

        add_executable(shinkou_environment_exposure_tests "${SHINKOU_TEST_ROOT}/EnvironmentExposureTests.cpp")
        target_link_libraries(shinkou_environment_exposure_tests PRIVATE shinkou_engine)
        foreach(render_api IN ITEMS dx11 dx12 vulkan)
            add_test(NAME shinkou_environment_exposure_${render_api}_tests COMMAND shinkou_environment_exposure_tests ${render_api})
            set_tests_properties(shinkou_environment_exposure_${render_api}_tests PROPERTIES SKIP_RETURN_CODE 77)
        endforeach()

        add_executable(shinkou_renderer_diagnostics_service_tests "${SHINKOU_TEST_ROOT}/RendererDiagnosticsServiceTests.cpp")
        target_link_libraries(shinkou_renderer_diagnostics_service_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_renderer_diagnostics_service_tests COMMAND shinkou_renderer_diagnostics_service_tests)

        add_executable(shinkou_render_graph_scheduler_tests "${SHINKOU_TEST_ROOT}/RenderGraphSchedulerTests.cpp")
        target_link_libraries(shinkou_render_graph_scheduler_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_render_graph_scheduler_tests COMMAND shinkou_render_graph_scheduler_tests)

        add_executable(shinkou_queue_sync_tests "${SHINKOU_TEST_ROOT}/QueueSyncTests.cpp")
        target_link_libraries(shinkou_queue_sync_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_queue_sync_tests COMMAND shinkou_queue_sync_tests)

        add_executable(shinkou_descriptor_allocator_tests "${SHINKOU_TEST_ROOT}/DescriptorAllocatorTests.cpp")
        target_link_libraries(shinkou_descriptor_allocator_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_descriptor_allocator_tests COMMAND shinkou_descriptor_allocator_tests)

        add_executable(shinkou_audio_tests "${SHINKOU_TEST_ROOT}/AudioSystemTests.cpp")
        target_link_libraries(shinkou_audio_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_audio_tests COMMAND shinkou_audio_tests)

        add_executable(shinkou_audio_scene_system_tests "${SHINKOU_TEST_ROOT}/AudioSceneSystemTests.cpp")
        target_link_libraries(shinkou_audio_scene_system_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_audio_scene_system_tests COMMAND shinkou_audio_scene_system_tests)

        add_executable(shinkou_input_tests "${SHINKOU_TEST_ROOT}/InputSystemTests.cpp")
        target_link_libraries(shinkou_input_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_input_tests COMMAND shinkou_input_tests)

        add_executable(shinkou_game_object_tests "${SHINKOU_TEST_ROOT}/GameObjectTests.cpp")
        target_link_libraries(shinkou_game_object_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_game_object_tests COMMAND shinkou_game_object_tests)

        add_executable(shinkou_ecs_store_tests "${SHINKOU_TEST_ROOT}/EcsStoreTests.cpp")
        target_link_libraries(shinkou_ecs_store_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_ecs_store_tests COMMAND shinkou_ecs_store_tests)

        add_executable(shinkou_ecs_runtime_tests "${SHINKOU_TEST_ROOT}/EcsRuntimeTests.cpp")
        target_link_libraries(shinkou_ecs_runtime_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_ecs_runtime_tests COMMAND shinkou_ecs_runtime_tests)

        add_executable(shinkou_world_transform_tests "${SHINKOU_TEST_ROOT}/WorldTransformTests.cpp")
        target_link_libraries(shinkou_world_transform_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_world_transform_tests COMMAND shinkou_world_transform_tests)

        add_executable(shinkou_reflection_tests "${SHINKOU_TEST_ROOT}/ReflectionTests.cpp")
        target_link_libraries(shinkou_reflection_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_reflection_tests COMMAND shinkou_reflection_tests)

        add_executable(shinkou_animation_tests "${SHINKOU_TEST_ROOT}/AnimationTests.cpp")
        target_link_libraries(shinkou_animation_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_animation_tests COMMAND shinkou_animation_tests)

        add_executable(shinkou_math_tests "${SHINKOU_TEST_ROOT}/MathTests.cpp")
        target_link_libraries(shinkou_math_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_math_tests COMMAND shinkou_math_tests)

        add_executable(shinkou_physics_world_tests "${SHINKOU_TEST_ROOT}/PhysicsWorldTests.cpp")
        target_link_libraries(shinkou_physics_world_tests PRIVATE shinkou_physics)
        add_test(NAME shinkou_physics_world_tests COMMAND shinkou_physics_world_tests)

        add_executable(shinkou_editor_ui_model_tests "${SHINKOU_TEST_ROOT}/EditorUiModelTests.cpp")
        target_link_libraries(shinkou_editor_ui_model_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_ui_model_tests COMMAND shinkou_editor_ui_model_tests)
        add_executable(shinkou_transform_inspector_tests "${SHINKOU_TEST_ROOT}/TransformInspectorTests.cpp")
        target_link_libraries(shinkou_transform_inspector_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_transform_inspector_tests COMMAND shinkou_transform_inspector_tests)

        add_executable(shinkou_editor_localization_tests "${SHINKOU_TEST_ROOT}/EditorLocalizationTests.cpp")
        target_link_libraries(shinkou_editor_localization_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_localization_tests COMMAND shinkou_editor_localization_tests)

        add_executable(shinkou_file_system_tests "${SHINKOU_TEST_ROOT}/FileSystemTests.cpp")
        target_link_libraries(shinkou_file_system_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_file_system_tests COMMAND shinkou_file_system_tests)

        # The platform contract is tested against both backends: the Windows
        # native kernel-object backend and the portable standard-library one.
        # Linking the isolated platform library keeps this target independent of
        # renderer/editor translation units.
        add_executable(shinkou_platform_system_tests "${SHINKOU_TEST_ROOT}/PlatformSystemTests.cpp")
        target_link_libraries(shinkou_platform_system_tests PRIVATE shinkou_platform_system)
        add_test(NAME shinkou_platform_system_tests COMMAND shinkou_platform_system_tests)
        set_tests_properties(shinkou_platform_system_tests PROPERTIES TIMEOUT 120)

        add_executable(shinkou_window_file_drop_tests "${SHINKOU_TEST_ROOT}/WindowFileDropTests.cpp")
        target_link_libraries(shinkou_window_file_drop_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_window_file_drop_tests COMMAND shinkou_window_file_drop_tests)

        add_executable(shinkou_asset_preview_tests "${SHINKOU_TEST_ROOT}/AssetPreviewTests.cpp")
        target_link_libraries(shinkou_asset_preview_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_asset_preview_tests COMMAND shinkou_asset_preview_tests)

        add_executable(shinkou_editor_asset_index_tests "${SHINKOU_TEST_ROOT}/EditorAssetIndexTests.cpp")
        target_link_libraries(shinkou_editor_asset_index_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_asset_index_tests COMMAND shinkou_editor_asset_index_tests)

        add_executable(shinkou_editor_audio_preview_tests "${SHINKOU_TEST_ROOT}/EditorAudioPreviewTests.cpp")
        add_executable(shinkou_editor_asset_inspection_tests "${SHINKOU_TEST_ROOT}/EditorAssetInspectionTests.cpp")
        target_compile_definitions(shinkou_editor_asset_inspection_tests PRIVATE
            SHINKOU_EDITOR_TEST_FIXTURE_DIR="${SHINKOU_TEST_ROOT}/fixtures")
        target_link_libraries(shinkou_editor_asset_inspection_tests PRIVATE shinkou_engine shinkou_asset_readers)
        add_test(NAME shinkou_editor_asset_inspection_tests COMMAND shinkou_editor_asset_inspection_tests)
        target_link_libraries(shinkou_editor_audio_preview_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_audio_preview_tests COMMAND shinkou_editor_audio_preview_tests)

        add_executable(shinkou_editor_image_preview_tests "${SHINKOU_TEST_ROOT}/EditorImagePreviewTests.cpp")
        target_link_libraries(shinkou_editor_image_preview_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_image_preview_tests COMMAND shinkou_editor_image_preview_tests)

        add_executable(shinkou_editor_video_preview_tests "${SHINKOU_TEST_ROOT}/EditorVideoPreviewTests.cpp")
        target_compile_definitions(shinkou_editor_video_preview_tests PRIVATE
            SHINKOU_EDITOR_TEST_FIXTURE_DIR="${SHINKOU_TEST_ROOT}/fixtures")
        target_link_libraries(shinkou_editor_video_preview_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_video_preview_tests COMMAND shinkou_editor_video_preview_tests)

        add_executable(shinkou_editor_model_preview_tests "${SHINKOU_TEST_ROOT}/EditorModelPreviewTests.cpp")
        target_link_libraries(shinkou_editor_model_preview_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_model_preview_tests COMMAND shinkou_editor_model_preview_tests)

        add_executable(shinkou_editor_model_preview_scene_tests "${SHINKOU_TEST_ROOT}/EditorModelPreviewSceneTests.cpp")
        target_link_libraries(shinkou_editor_model_preview_scene_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_model_preview_scene_tests COMMAND shinkou_editor_model_preview_scene_tests)

        add_executable(shinkou_editor_model_preview_renderer_tests "${SHINKOU_TEST_ROOT}/EditorModelPreviewRendererTests.cpp")
        target_link_libraries(shinkou_editor_model_preview_renderer_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_model_preview_renderer_tests COMMAND shinkou_editor_model_preview_renderer_tests)
        add_executable(shinkou_editor_world_viewport_tests "${SHINKOU_TEST_ROOT}/EditorWorldViewportTests.cpp")
        add_executable(shinkou_editor_controls_tests "${SHINKOU_TEST_ROOT}/EditorControlsTests.cpp")
        target_link_libraries(shinkou_editor_controls_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_controls_tests COMMAND shinkou_editor_controls_tests)
        target_link_libraries(shinkou_editor_world_viewport_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_world_viewport_tests COMMAND shinkou_editor_world_viewport_tests)

        add_executable(shinkou_editor_gltf_preview_tests "${SHINKOU_TEST_ROOT}/EditorGltfPreviewTests.cpp")
        target_link_libraries(shinkou_editor_gltf_preview_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_gltf_preview_tests COMMAND shinkou_editor_gltf_preview_tests)

        add_executable(shinkou_editor_command_bus_tests "${SHINKOU_TEST_ROOT}/EditorCommandBusTests.cpp")
        target_link_libraries(shinkou_editor_command_bus_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_command_bus_tests COMMAND shinkou_editor_command_bus_tests)

        add_executable(shinkou_editor_build_system_tests "${SHINKOU_TEST_ROOT}/EditorBuildSystemTests.cpp")
        target_link_libraries(shinkou_editor_build_system_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_build_system_tests COMMAND shinkou_editor_build_system_tests)

        add_executable(shinkou_editor_compile_commands_tests "${SHINKOU_TEST_ROOT}/EditorCompileCommandsTests.cpp")
        target_link_libraries(shinkou_editor_compile_commands_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_compile_commands_tests COMMAND shinkou_editor_compile_commands_tests)

        add_executable(shinkou_editor_project_integration_tests "${SHINKOU_TEST_ROOT}/EditorProjectIntegrationTests.cpp")
        target_link_libraries(shinkou_editor_project_integration_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_project_integration_tests COMMAND shinkou_editor_project_integration_tests)

        add_executable(shinkou_editor_selection_tests "${SHINKOU_TEST_ROOT}/EditorSelectionTests.cpp")
        target_link_libraries(shinkou_editor_selection_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_selection_tests COMMAND shinkou_editor_selection_tests)

        add_executable(shinkou_render_view_tests "${SHINKOU_TEST_ROOT}/RenderViewTests.cpp")
        target_link_libraries(shinkou_render_view_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_render_view_tests COMMAND shinkou_render_view_tests)

        add_executable(shinkou_ui_runtime_tests "${SHINKOU_TEST_ROOT}/UiRuntimeTests.cpp")
        target_link_libraries(shinkou_ui_runtime_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_ui_runtime_tests COMMAND shinkou_ui_runtime_tests)

        add_executable(shinkou_input_bridge_tests "${SHINKOU_TEST_ROOT}/InputBridgeTests.cpp")
        target_link_libraries(shinkou_input_bridge_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_input_bridge_tests COMMAND shinkou_input_bridge_tests)

        add_executable(shinkou_theme_tests "${SHINKOU_TEST_ROOT}/ThemeTests.cpp")
        target_link_libraries(shinkou_theme_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_theme_tests COMMAND shinkou_theme_tests)

        add_executable(shinkou_dock_layout_tests "${SHINKOU_TEST_ROOT}/DockLayoutTests.cpp")
        target_link_libraries(shinkou_dock_layout_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_dock_layout_tests COMMAND shinkou_dock_layout_tests)

        add_executable(shinkou_ui_render_tests "${SHINKOU_TEST_ROOT}/UiRenderTests.cpp")
        target_link_libraries(shinkou_ui_render_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_ui_render_tests COMMAND shinkou_ui_render_tests)

        add_executable(shinkou_editor_interaction_tests "${SHINKOU_TEST_ROOT}/EditorInteractionTests.cpp")
        target_link_libraries(shinkou_editor_interaction_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_interaction_tests COMMAND shinkou_editor_interaction_tests)
        add_executable(shinkou_editor_asset_browser_tests "${SHINKOU_TEST_ROOT}/EditorAssetBrowserTests.cpp")
        target_link_libraries(shinkou_editor_asset_browser_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_asset_browser_tests COMMAND shinkou_editor_asset_browser_tests)
        add_executable(shinkou_editor_hierarchy_tests "${SHINKOU_TEST_ROOT}/EditorHierarchyTests.cpp")
        target_link_libraries(shinkou_editor_hierarchy_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_hierarchy_tests COMMAND shinkou_editor_hierarchy_tests)
        if(SHINKOU_WITH_IMGUI)
            add_executable(shinkou_editor_imgui_presentation_tests "${SHINKOU_TEST_ROOT}/EditorImGuiPresentationTests.cpp")
            target_link_libraries(shinkou_editor_imgui_presentation_tests PRIVATE shinkou_engine)
            add_test(NAME shinkou_editor_imgui_presentation_tests COMMAND shinkou_editor_imgui_presentation_tests)
            add_executable(shinkou_editor_ui_performance_tests "${SHINKOU_TEST_ROOT}/EditorUiPerformanceTests.cpp")
            target_link_libraries(shinkou_editor_ui_performance_tests PRIVATE shinkou_engine)
            add_test(NAME shinkou_editor_ui_performance_tests COMMAND shinkou_editor_ui_performance_tests)
        else()
            add_executable(shinkou_editor_no_imgui_tests "${SHINKOU_TEST_ROOT}/EditorNoImGuiTests.cpp")
            target_link_libraries(shinkou_editor_no_imgui_tests PRIVATE shinkou_engine)
            add_test(NAME shinkou_editor_no_imgui_tests COMMAND shinkou_editor_no_imgui_tests)
        endif()

        add_executable(shinkou_style_tests "${SHINKOU_TEST_ROOT}/StyleTests.cpp")
        target_link_libraries(shinkou_style_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_style_tests COMMAND shinkou_style_tests)

        add_executable(shinkou_media_tests "${SHINKOU_TEST_ROOT}/MediaTests.cpp")
        target_link_libraries(shinkou_media_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_media_tests COMMAND shinkou_media_tests)

        add_executable(shinkou_imgui_adapter_tests "${SHINKOU_TEST_ROOT}/ImGuiAdapterTests.cpp")
        target_link_libraries(shinkou_imgui_adapter_tests PRIVATE shinkou_engine)
        if(SHINKOU_WITH_IMGUI)
            target_compile_definitions(shinkou_imgui_adapter_tests PRIVATE SHINKOU_WITH_IMGUI=1)
            target_include_directories(shinkou_imgui_adapter_tests PRIVATE "${IMGUI_INCLUDE_DIR}")
        endif()
        add_test(NAME shinkou_imgui_adapter_tests COMMAND shinkou_imgui_adapter_tests)

        add_executable(shinkou_components_tests "${SHINKOU_TEST_ROOT}/ComponentsTests.cpp")
        target_link_libraries(shinkou_components_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_components_tests COMMAND shinkou_components_tests)

        # The job system is a leaf: it includes nothing from the engine, so this
        # target compiles its translation unit directly instead of linking the
        # multi-hundred-megabyte engine archive. That keeps the dependency honest
        # (a stray engine include breaks the build immediately) and turns a
        # several-minute link into a couple of seconds.
        find_package(Threads REQUIRED)
        add_executable(shinkou_jobs_tests
            "${SHINKOU_TEST_ROOT}/JobsTests.cpp"
            "${SHINKOU_ENGINE_ROOT}/src/jobs/JobSystem.cpp")
        target_include_directories(shinkou_jobs_tests PRIVATE "${SHINKOU_ENGINE_ROOT}/include")
        target_link_libraries(shinkou_jobs_tests PRIVATE Threads::Threads)
        add_test(NAME shinkou_jobs_tests COMMAND shinkou_jobs_tests)

        # The profiler is a leaf too: same reasoning as the job system above. Its
        # test target deliberately links nothing from the engine.
        add_executable(shinkou_profile_tests
            "${SHINKOU_TEST_ROOT}/ProfileTests.cpp"
            "${SHINKOU_ENGINE_ROOT}/src/profile/Profiler.cpp")
        target_include_directories(shinkou_profile_tests PRIVATE "${SHINKOU_ENGINE_ROOT}/include")
        target_link_libraries(shinkou_profile_tests PRIVATE Threads::Threads ShinkouEngine::Core)
        add_test(NAME shinkou_profile_tests COMMAND shinkou_profile_tests)

        add_executable(shinkou_math_parallel_tests "${SHINKOU_TEST_ROOT}/MathParallelTests.cpp")
        target_link_libraries(shinkou_math_parallel_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_math_parallel_tests COMMAND shinkou_math_parallel_tests)

        add_executable(shinkou_math_advanced_tests "${SHINKOU_TEST_ROOT}/MathAdvancedTests.cpp")
        target_link_libraries(shinkou_math_advanced_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_math_advanced_tests COMMAND shinkou_math_advanced_tests)

        if(SHINKOU_WITH_SPDLOG)
            add_executable(shinkou_logging_tests "${SHINKOU_TEST_ROOT}/LoggingTests.cpp")
            target_link_libraries(shinkou_logging_tests PRIVATE shinkou_logging)
            add_test(NAME shinkou_logging_tests COMMAND shinkou_logging_tests)
            set_tests_properties(shinkou_logging_tests PROPERTIES TIMEOUT 60)
        endif()

        add_executable(shinkou_network_tests "${SHINKOU_TEST_ROOT}/NetworkSystemTests.cpp")
        target_link_libraries(shinkou_network_tests PRIVATE shinkou_engine)
        if(WIN32)
            target_link_libraries(shinkou_network_tests PRIVATE ws2_32)
        endif()
        add_test(NAME shinkou_network_tests COMMAND shinkou_network_tests)

    endif()
endif()

if(SHINKOU_BUILD_BENCHMARKS)
    add_executable(shinkou_logging_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/LoggingBenchmarks.cpp")
    target_link_libraries(shinkou_logging_benchmarks PRIVATE shinkou_logging)
    add_executable(shinkou_coroutine_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/CoroutineBenchmarks.cpp")
    target_compile_features(shinkou_coroutine_benchmarks PRIVATE cxx_std_20)
    target_link_libraries(shinkou_coroutine_benchmarks PRIVATE shinkou_coroutine_runtime)
    if(SHINKOU_WITH_IMGUI)
        add_executable(shinkou_editor_ui_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/EditorUiBenchmarks.cpp")
        target_link_libraries(shinkou_editor_ui_benchmarks PRIVATE shinkou_engine)
    endif()
    add_executable(shinkou_math_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/MathBenchmarks.cpp")
    target_link_libraries(shinkou_math_benchmarks PRIVATE shinkou_engine)
    add_executable(shinkou_render_geometry_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/RenderGeometryBenchmarks.cpp")
    target_link_libraries(shinkou_render_geometry_benchmarks PRIVATE shinkou_engine)
    add_executable(shinkou_physics_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/PhysicsBenchmarks.cpp")
    target_link_libraries(shinkou_physics_benchmarks PRIVATE shinkou_physics)
    add_executable(shinkou_object_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/ObjectBenchmarks.cpp")
    target_link_libraries(shinkou_object_benchmarks PRIVATE shinkou_engine)
    shinkou_fetch_dependency(entt entt_SOURCE_DIR)
    shinkou_install_dependency_licenses(entt "${entt_SOURCE_DIR}")
    add_executable(shinkou_ecs_chunk_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/EcsChunkBenchmarks.cpp")
    target_link_libraries(shinkou_ecs_chunk_benchmarks PRIVATE shinkou_engine)
    target_include_directories(shinkou_ecs_chunk_benchmarks SYSTEM PRIVATE "${entt_SOURCE_DIR}/single_include")
    add_executable(shinkou_ecs_runtime_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/EcsRuntimeBenchmarks.cpp")
    target_link_libraries(shinkou_ecs_runtime_benchmarks PRIVATE shinkou_engine)
    add_executable(shinkou_ecs_bulk_benchmarks "${SHINKOU_ENGINE_ROOT}/benchmarks/EcsBulkBenchmarks.cpp")
    target_link_libraries(shinkou_ecs_bulk_benchmarks PRIVATE shinkou_engine)
    if(SHINKOU_ENABLE_AVX2)
        if(MSVC)
            target_compile_options(shinkou_math_benchmarks PRIVATE /arch:AVX2)
        elseif(MINGW)
            # Keep the MinGW benchmark executable on the stable SSE2 path;
            # the production engine still compiles its AVX2-capable objects.
            target_compile_options(shinkou_math_benchmarks PRIVATE -mno-avx2 -mno-avx)
        else()
            target_compile_options(shinkou_math_benchmarks PRIVATE -mavx2 -mstackrealign)
        endif()
    endif()
    if(MSVC)
        target_compile_options(shinkou_math_benchmarks PRIVATE /O2)
        target_compile_options(shinkou_render_geometry_benchmarks PRIVATE /O2)
        target_compile_options(shinkou_object_benchmarks PRIVATE /O2)
        target_compile_options(shinkou_ecs_chunk_benchmarks PRIVATE /O2)
        target_compile_options(shinkou_ecs_runtime_benchmarks PRIVATE /O2)
        target_compile_options(shinkou_ecs_bulk_benchmarks PRIVATE /O2)
        target_compile_options(shinkou_physics_benchmarks PRIVATE /O2)
    else()
        target_compile_options(shinkou_math_benchmarks PRIVATE -O3)
        target_compile_options(shinkou_render_geometry_benchmarks PRIVATE -O3)
        target_compile_options(shinkou_object_benchmarks PRIVATE -O3)
        target_compile_options(shinkou_ecs_chunk_benchmarks PRIVATE -O3)
        target_compile_options(shinkou_ecs_runtime_benchmarks PRIVATE -O3)
        target_compile_options(shinkou_ecs_bulk_benchmarks PRIVATE -O3)
        target_compile_options(shinkou_physics_benchmarks PRIVATE -O3)
    endif()
endif()

install(TARGETS shinkou_physics shinkou_engine shinkou_assets shinkou_asset_readers shinkou_coroutine_runtime shinkou_logging
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY "${SHINKOU_ENGINE_ROOT}/include/" DESTINATION include)
install(DIRECTORY "${spdlog_SOURCE_DIR}/include/" DESTINATION include)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/include/" DESTINATION include)
if(SHINKOU_WITH_IMGUI)
    install(FILES "${codicons_SOURCE_DIR}/dist/codicon.ttf" DESTINATION share/shinkou/editor/fonts)
endif()

message(STATUS "ShinkouEngine optional capabilities:")
message(STATUS "  SDL3 input DLL: ${SHINKOU_ENABLE_SDL3_INPUT}")
message(STATUS "  spdlog:     ${SHINKOU_WITH_SPDLOG}")
message(STATUS "  Vulkan:     ${SHINKOU_WITH_VULKAN}")
message(STATUS "  Physics library: ON")
message(STATUS "  Physics example: ${SHINKOU_BUILD_PHYSICS_EXAMPLE}")
message(STATUS "  PhysX:      ${SHINKOU_WITH_PHYSX}")
message(STATUS "  PhysX Extensions: ${SHINKOU_WITH_PHYSX_EXTENSIONS}")
message(STATUS "  PhysX Cooking: ${SHINKOU_WITH_PHYSX_COOKING}")
message(STATUS "  ImGui:      ${SHINKOU_WITH_IMGUI}")
message(STATUS "  miniaudio:  ${SHINKOU_WITH_MINIAUDIO}")
message(STATUS "  Lua:        ${SHINKOU_WITH_LUA}")
message(STATUS "  Python:     ${SHINKOU_WITH_PYTHON}")
message(STATUS "  C#:         ${SHINKOU_WITH_CSHARP}")

# Build all regression executables without relinking a running sample/editor.
if(SHINKOU_BUILD_TESTS AND BUILD_TESTING)
    add_custom_target(shinkou_editor_checks)
    if(TARGET shinkou_core_tests)
        add_dependencies(shinkou_editor_checks shinkou_core_tests)
    endif()
    get_property(SHINKOU_CHECK_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(SHINKOU_CHECK_TARGET IN LISTS SHINKOU_CHECK_TARGETS)
        if(SHINKOU_CHECK_TARGET MATCHES "^shinkou_.*_tests$")
            add_dependencies(shinkou_editor_checks ${SHINKOU_CHECK_TARGET})
            # Test setup and checks often use assert(). Apply this to every test
            # target automatically so Release never compiles those calls away.
            if(MSVC)
                target_compile_options(${SHINKOU_CHECK_TARGET} PRIVATE /UNDEBUG)
            else()
                target_compile_options(${SHINKOU_CHECK_TARGET} PRIVATE -UNDEBUG)
                if(MINGW AND SHINKOU_ENABLE_AVX2)
                    # Keep the existing MinGW test-stack compatibility policy;
                    # production targets and benchmarks retain their AVX2 path.
                    target_compile_options(${SHINKOU_CHECK_TARGET} PRIVATE -mno-avx2 -mno-avx)
                endif()
            endif()
        endif()
    endforeach()
endif()
