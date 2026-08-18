cmake_minimum_required(VERSION 3.25)

project(ShinkouEngine VERSION 0.1.0 LANGUAGES CXX)

set(SHINKOU_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/engine")
set(SHINKOU_SAMPLE_ROOT "${SHINKOU_ENGINE_ROOT}/samples")
set(SHINKOU_TEST_ROOT "${SHINKOU_ENGINE_ROOT}/tests")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SHINKOU_BUILD_SAMPLE "Build the engine sample" ON)
option(SHINKOU_BUILD_SHADER_SCENE_TEST "Build the standalone reference multi-pass shader scene test" ON)
option(SHINKOU_FETCH_DEPENDENCIES "Fetch optional third-party dependencies" OFF)
option(SHINKOU_ENABLE_SDL3_INPUT "Enable SDL3 event input" ON)
option(SHINKOU_INPUT_FETCH_SDL3 "Fetch SDL3 source for the private ShinkouInput DLL" ON)
set(SHINKOU_INPUT_SDL3_SOURCE_DIR "" CACHE PATH "Optional local SDL3 source directory for ShinkouInput")
option(SHINKOU_ENABLE_MINIAUDIO "Enable miniaudio integration when available" ON)
option(SHINKOU_ENABLE_PHYSX "Enable PhysX integration when available" ON)
option(SHINKOU_ENABLE_IMGUI "Enable ImGui editor integration when available" ON)
option(SHINKOU_ENABLE_LUA "Enable Lua scripting integration when available" ON)
option(SHINKOU_ENABLE_PYTHON "Enable Python scripting integration when available" ON)
option(SHINKOU_ENABLE_CSHARP "Enable .NET/C# capability detection" ON)
option(SHINKOU_BUILD_TESTS "Build engine tests" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ShinkouDependencies)

if(SHINKOU_ENABLE_SDL3_INPUT)
    add_subdirectory("${SHINKOU_ENGINE_ROOT}/input" "${CMAKE_CURRENT_BINARY_DIR}/shinkou_input" EXCLUDE_FROM_ALL)
endif()

file(GLOB_RECURSE SHINKOU_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${SHINKOU_ENGINE_ROOT}/src/*.cpp")

add_library(shinkou_engine STATIC ${SHINKOU_ENGINE_SOURCES})
set_target_properties(shinkou_engine PROPERTIES OUTPUT_NAME shinkou_engine EXPORT_NAME Engine)
add_library(ShinkouEngine::Engine ALIAS shinkou_engine)
target_include_directories(shinkou_engine PUBLIC
    "${SHINKOU_ENGINE_ROOT}/include"
)
target_include_directories(shinkou_engine PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/entt"
)
target_compile_features(shinkou_engine PUBLIC cxx_std_17)

if(SHINKOU_WITH_SPDLOG)
    target_link_libraries(shinkou_engine PUBLIC ${SHINKOU_SPDLOG_TARGET})
    target_compile_definitions(shinkou_engine PUBLIC SHINKOU_WITH_SPDLOG=1)
else()
    target_compile_definitions(shinkou_engine PUBLIC SHINKOU_WITH_SPDLOG=0)
endif()

if(MSVC)
    target_compile_options(shinkou_engine PRIVATE /W4 /permissive-)
else()
    target_compile_options(shinkou_engine PRIVATE -Wall -Wextra -Wpedantic)
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

if(SHINKOU_WITH_PHYSX)
    target_include_directories(shinkou_engine PRIVATE "${PHYSX_INCLUDE_DIR}")
    target_link_libraries(shinkou_engine PRIVATE ${PHYSX_LIBRARIES})
    target_compile_definitions(shinkou_engine PRIVATE SHINKOU_WITH_PHYSX=1)
endif()

if(SHINKOU_WITH_IMGUI)
    target_include_directories(shinkou_engine PRIVATE "${IMGUI_INCLUDE_DIR}")
    if(IMGUI_LIBRARY)
        target_link_libraries(shinkou_engine PRIVATE "${IMGUI_LIBRARY}")
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

if(WIN32)
    target_link_libraries(shinkou_engine PUBLIC d3d11 d3d12 dxgi d3dcompiler dxguid ws2_32)
    target_compile_definitions(shinkou_engine PRIVATE SHINKOU_PLATFORM_WINDOWS=1)
endif()

if(SHINKOU_BUILD_SAMPLE)
    find_program(SHINKOU_DXC_EXECUTABLE NAMES dxc dxc.exe)
    find_program(SHINKOU_GLSLC_EXECUTABLE NAMES glslc glslc.exe)
    set(SHINKOU_SAMPLE_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shinkou_sample_shaders")
    set(SHINKOU_SAMPLE_SHADER_OUTPUTS)
    if(SHINKOU_DXC_EXECUTABLE)
        add_custom_command(
            OUTPUT "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.dxil" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.dxil"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E VSMain -T vs_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/sample_mesh.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E PSMain -T ps_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.hlsl"
            COMMAND "${SHINKOU_DXC_EXECUTABLE}" -E CSMain -T cs_6_0 -Fo "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.dxil" "${SHINKOU_SAMPLE_ROOT}/shaders/gpu_culling.hlsl"
            DEPENDS "${SHINKOU_SAMPLE_ROOT}/shaders/sample.hlsl" "${SHINKOU_SAMPLE_ROOT}/shaders/sample_mesh.hlsl" "${SHINKOU_SAMPLE_ROOT}/shaders/gpu_culling.hlsl"
            VERBATIM
        )
        list(APPEND SHINKOU_SAMPLE_SHADER_OUTPUTS
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.dxil"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.dxil"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.dxil")
            list(APPEND SHINKOU_SAMPLE_SHADER_OUTPUTS "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.dxil")
    endif()
    if(SHINKOU_GLSLC_EXECUTABLE)
        add_custom_command(
            OUTPUT "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.spv" "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.spv"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=vert -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.vert"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=vert -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/sample_mesh.vert"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=frag -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.frag"
            COMMAND "${SHINKOU_GLSLC_EXECUTABLE}" -fshader-stage=comp -o "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/gpu_culling.spv" "${SHINKOU_SAMPLE_ROOT}/shaders/gpu_culling.comp"
            DEPENDS "${SHINKOU_SAMPLE_ROOT}/shaders/sample.vert" "${SHINKOU_SAMPLE_ROOT}/shaders/sample_mesh.vert" "${SHINKOU_SAMPLE_ROOT}/shaders/sample.frag" "${SHINKOU_SAMPLE_ROOT}/shaders/gpu_culling.comp"
            VERBATIM
        )
        list(APPEND SHINKOU_SAMPLE_SHADER_OUTPUTS
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_vs.spv"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_mesh_vs.spv"
            "${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}/sample_ps.spv")
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
    target_compile_definitions(shinkou_engine_sample PRIVATE SHINKOU_SAMPLE_SHADER_DIR="${SHINKOU_SAMPLE_SHADER_OUTPUT_DIR}")
    if(TARGET shinkou_sample_shaders)
        add_dependencies(shinkou_engine_sample shinkou_sample_shaders)
    endif()
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

if(SHINKOU_BUILD_TESTS)
    include(CTest)
    if(BUILD_TESTING)
        add_executable(shinkou_render_graph_tests "${SHINKOU_TEST_ROOT}/RenderGraphTests.cpp")
        target_link_libraries(shinkou_render_graph_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_render_graph_tests COMMAND shinkou_render_graph_tests)

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

        add_executable(shinkou_asset_system_tests "${SHINKOU_TEST_ROOT}/AssetSystemTests.cpp")
        target_link_libraries(shinkou_asset_system_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_asset_system_tests COMMAND shinkou_asset_system_tests)

        add_executable(shinkou_input_tests "${SHINKOU_TEST_ROOT}/InputSystemTests.cpp")
        target_link_libraries(shinkou_input_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_input_tests COMMAND shinkou_input_tests)

        add_executable(shinkou_game_object_tests "${SHINKOU_TEST_ROOT}/GameObjectTests.cpp")
        target_link_libraries(shinkou_game_object_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_game_object_tests COMMAND shinkou_game_object_tests)

        add_executable(shinkou_reflection_tests "${SHINKOU_TEST_ROOT}/ReflectionTests.cpp")
        target_link_libraries(shinkou_reflection_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_reflection_tests COMMAND shinkou_reflection_tests)

        add_executable(shinkou_animation_tests "${SHINKOU_TEST_ROOT}/AnimationTests.cpp")
        target_link_libraries(shinkou_animation_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_animation_tests COMMAND shinkou_animation_tests)

        add_executable(shinkou_math_tests "${SHINKOU_TEST_ROOT}/MathTests.cpp")
        target_link_libraries(shinkou_math_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_math_tests COMMAND shinkou_math_tests)

        add_executable(shinkou_editor_ui_model_tests "${SHINKOU_TEST_ROOT}/EditorUiModelTests.cpp")
        target_link_libraries(shinkou_editor_ui_model_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_editor_ui_model_tests COMMAND shinkou_editor_ui_model_tests)

        add_executable(shinkou_math_parallel_tests "${SHINKOU_TEST_ROOT}/MathParallelTests.cpp")
        target_link_libraries(shinkou_math_parallel_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_math_parallel_tests COMMAND shinkou_math_parallel_tests)

        add_executable(shinkou_logging_tests "${SHINKOU_TEST_ROOT}/LoggingTests.cpp")
        target_link_libraries(shinkou_logging_tests PRIVATE shinkou_engine)
        add_test(NAME shinkou_logging_tests COMMAND shinkou_logging_tests)

        add_executable(shinkou_network_tests "${SHINKOU_TEST_ROOT}/NetworkSystemTests.cpp")
        target_link_libraries(shinkou_network_tests PRIVATE shinkou_engine)
        if(WIN32)
            target_link_libraries(shinkou_network_tests PRIVATE ws2_32)
        endif()
        add_test(NAME shinkou_network_tests COMMAND shinkou_network_tests)

        set(SHINKOU_TEST_TARGETS
            shinkou_render_graph_tests shinkou_gpu_memory_allocator_tests shinkou_texture_model_tests shinkou_texture_backend_mapping_tests shinkou_pbr_render_pipeline_tests shinkou_lighting_pipeline_tests shinkou_shader_pipeline_library_tests shinkou_render_capture_budget_tests shinkou_advanced_render_features_tests shinkou_render_feature_graph_builder_tests shinkou_renderer_diagnostics_service_tests shinkou_render_graph_scheduler_tests shinkou_queue_sync_tests
            shinkou_render_feature_graph_builder_tests shinkou_descriptor_allocator_tests
            shinkou_audio_tests shinkou_asset_system_tests shinkou_input_tests
            shinkou_game_object_tests shinkou_reflection_tests shinkou_animation_tests shinkou_math_tests
            shinkou_editor_ui_model_tests
            shinkou_math_parallel_tests shinkou_logging_tests shinkou_network_tests)
        foreach(shinkou_test_target IN LISTS SHINKOU_TEST_TARGETS)
            string(REPLACE "shinkou_" "shinkou_" shinkou_test_output "${shinkou_test_target}")
            set_target_properties(${shinkou_test_target} PROPERTIES OUTPUT_NAME "${shinkou_test_output}")
        endforeach()
        if(MSVC)
            foreach(shinkou_test_target IN LISTS SHINKOU_TEST_TARGETS)
                target_compile_options(${shinkou_test_target} PRIVATE /UNDEBUG)
            endforeach()
        else()
            foreach(shinkou_test_target IN LISTS SHINKOU_TEST_TARGETS)
                target_compile_options(${shinkou_test_target} PRIVATE -UNDEBUG)
            endforeach()
        endif()
    endif()
endif()

install(TARGETS shinkou_engine
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
install(DIRECTORY "${SHINKOU_ENGINE_ROOT}/include/" DESTINATION include)

message(STATUS "ShinkouEngine optional capabilities:")
message(STATUS "  SDL3 input DLL: ${SHINKOU_ENABLE_SDL3_INPUT}")
message(STATUS "  spdlog:     ${SHINKOU_WITH_SPDLOG}")
message(STATUS "  Vulkan:     ${SHINKOU_WITH_VULKAN}")
message(STATUS "  PhysX:      ${SHINKOU_WITH_PHYSX}")
message(STATUS "  ImGui:      ${SHINKOU_WITH_IMGUI}")
message(STATUS "  miniaudio:  ${SHINKOU_WITH_MINIAUDIO}")
message(STATUS "  Lua:        ${SHINKOU_WITH_LUA}")
message(STATUS "  Python:     ${SHINKOU_WITH_PYTHON}")
message(STATUS "  C#:         ${SHINKOU_WITH_CSHARP}")
