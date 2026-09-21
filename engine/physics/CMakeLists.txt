if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(ShinkouPhysics VERSION 0.1.0 LANGUAGES CXX)
    set(SHINKOU_PHYSICS_STANDALONE_PROJECT ON)
    set(SHINKOU_ENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    option(SHINKOU_ENABLE_PHYSX "Enable PhysX integration when available" ON)
    set(SHINKOU_PHYSX_ROOT "" CACHE PATH "PhysX SDK root containing include and lib directories")
    set(SHINKOU_PHYSX_LIBRARY_DIR "" CACHE PATH "PhysX SDK library directory override")
    option(SHINKOU_BUILD_TESTS "Build standalone physics tests" ON)
    list(APPEND CMAKE_MODULE_PATH "${SHINKOU_ENGINE_ROOT}/../cmake")
    include(ShinkouDependencies)
    if(SHINKOU_BUILD_TESTS)
        include(CTest)
    endif()
endif()

if(NOT DEFINED SHINKOU_ENGINE_ROOT)
    get_filename_component(SHINKOU_ENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()
if(NOT DEFINED SHINKOU_BUILD_PHYSICS_EXAMPLE)
    option(SHINKOU_BUILD_PHYSICS_EXAMPLE "Build the standalone physics library example" ON)
endif()
if(NOT DEFINED SHINKOU_BUILD_PHYSICS_PERFORMANCE_TEST)
    option(SHINKOU_BUILD_PHYSICS_PERFORMANCE_TEST "Build the PhysX dense performance test" ON)
endif()

# This target intentionally depends only on the public math headers and the
# optional PhysX SDK. The main engine links it as a normal library consumer.
set(SHINKOU_PHYSICS_SOURCES
    "${SHINKOU_ENGINE_ROOT}/src/physics/PhysicsWorld.cpp"
    "${SHINKOU_ENGINE_ROOT}/src/physics/SimplePhysicsWorld.cpp"
    "${SHINKOU_ENGINE_ROOT}/src/physics/PhysXPhysicsWorld.cpp")

add_library(shinkou_physics STATIC ${SHINKOU_PHYSICS_SOURCES})
set_target_properties(shinkou_physics PROPERTIES OUTPUT_NAME shinkou_physics EXPORT_NAME Physics)
add_library(ShinkouEngine::Physics ALIAS shinkou_physics)

target_include_directories(shinkou_physics
    PUBLIC "${SHINKOU_ENGINE_ROOT}/include")
target_compile_features(shinkou_physics PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(shinkou_physics PRIVATE /W4 /permissive-)
else()
    target_compile_options(shinkou_physics PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SHINKOU_WITH_PHYSX)
    target_include_directories(shinkou_physics PRIVATE "${PHYSX_INCLUDE_DIR}")
    target_link_libraries(shinkou_physics PRIVATE ${PHYSX_LIBRARIES})
    target_compile_definitions(shinkou_physics PRIVATE SHINKOU_WITH_PHYSX=1)
    # PhysX headers require exactly one of _DEBUG/NDEBUG. MSVC provides this
    # through its configuration, while MinGW does not consistently do so for
    # a Debug single-config build.
    target_compile_definitions(shinkou_physics PRIVATE
        "$<$<CONFIG:Debug>:_DEBUG=1>"
        "$<$<NOT:$<CONFIG:Debug>>:NDEBUG=1>")
    if(SHINKOU_WITH_PHYSX_EXTENSIONS)
        target_compile_definitions(shinkou_physics PRIVATE SHINKOU_WITH_PHYSX_EXTENSIONS=1)
    endif()
    if(SHINKOU_WITH_PHYSX_COOKING)
        target_compile_definitions(shinkou_physics PRIVATE SHINKOU_WITH_PHYSX_COOKING=1)
    endif()
endif()

if(SHINKOU_BUILD_PHYSICS_EXAMPLE)
    add_executable(shinkou_physics_example examples/PhysicsStandaloneExample.cpp)
    target_link_libraries(shinkou_physics_example PRIVATE shinkou_physics)
    set_target_properties(shinkou_physics_example PROPERTIES OUTPUT_NAME shinkou_physics_example)
    if(WIN32 AND SHINKOU_WITH_PHYSX AND SHINKOU_PHYSX_ROOT)
        file(GLOB_RECURSE SHINKOU_PHYSX_RUNTIME_DLLS LIST_DIRECTORIES false
            "${SHINKOU_PHYSX_ROOT}/bin/*.dll"
            "${SHINKOU_PHYSX_ROOT}/lib/*.dll")
        foreach(SHINKOU_PHYSX_RUNTIME_DLL IN LISTS SHINKOU_PHYSX_RUNTIME_DLLS)
            get_filename_component(SHINKOU_PHYSX_RUNTIME_NAME
                "${SHINKOU_PHYSX_RUNTIME_DLL}" NAME)
            add_custom_command(TARGET shinkou_physics_example POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SHINKOU_PHYSX_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:shinkou_physics_example>/${SHINKOU_PHYSX_RUNTIME_NAME}"
                VERBATIM)
        endforeach()
    endif()
    if(BUILD_TESTING)
        add_test(NAME shinkou_physics_example COMMAND shinkou_physics_example)
    endif()
endif()

if(SHINKOU_PHYSICS_STANDALONE_PROJECT OR BUILD_TESTING)
    add_executable(shinkou_physics_real_scene_test examples/PhysicsRealSceneTest.cpp)
    target_link_libraries(shinkou_physics_real_scene_test PRIVATE shinkou_physics)
    if(WIN32 AND SHINKOU_WITH_PHYSX AND SHINKOU_PHYSX_ROOT)
        file(GLOB_RECURSE SHINKOU_PHYSX_RUNTIME_DLLS LIST_DIRECTORIES false
            "${SHINKOU_PHYSX_ROOT}/bin/*.dll"
            "${SHINKOU_PHYSX_ROOT}/lib/*.dll")
        foreach(SHINKOU_PHYSX_RUNTIME_DLL IN LISTS SHINKOU_PHYSX_RUNTIME_DLLS)
            get_filename_component(SHINKOU_PHYSX_RUNTIME_NAME
                "${SHINKOU_PHYSX_RUNTIME_DLL}" NAME)
            add_custom_command(TARGET shinkou_physics_real_scene_test POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SHINKOU_PHYSX_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:shinkou_physics_real_scene_test>/${SHINKOU_PHYSX_RUNTIME_NAME}"
                VERBATIM)
        endforeach()
    endif()
    if(BUILD_TESTING)
        add_test(NAME shinkou_physics_real_scene_test COMMAND shinkou_physics_real_scene_test)
    endif()
endif()

if(SHINKOU_BUILD_PHYSICS_PERFORMANCE_TEST)
    add_executable(shinkou_physics_dense_performance examples/PhysicsDensePerformanceTest.cpp)
    target_link_libraries(shinkou_physics_dense_performance PRIVATE shinkou_physics)
    if(WIN32 AND SHINKOU_WITH_PHYSX AND SHINKOU_PHYSX_ROOT)
        file(GLOB_RECURSE SHINKOU_PHYSX_RUNTIME_DLLS LIST_DIRECTORIES false
            "${SHINKOU_PHYSX_ROOT}/bin/*.dll"
            "${SHINKOU_PHYSX_ROOT}/lib/*.dll")
        foreach(SHINKOU_PHYSX_RUNTIME_DLL IN LISTS SHINKOU_PHYSX_RUNTIME_DLLS)
            get_filename_component(SHINKOU_PHYSX_RUNTIME_NAME
                "${SHINKOU_PHYSX_RUNTIME_DLL}" NAME)
            add_custom_command(TARGET shinkou_physics_dense_performance POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SHINKOU_PHYSX_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:shinkou_physics_dense_performance>/${SHINKOU_PHYSX_RUNTIME_NAME}"
                VERBATIM)
        endforeach()
    endif()
endif()

if(SHINKOU_PHYSICS_STANDALONE_PROJECT AND BUILD_TESTING)
    add_executable(shinkou_physics_world_tests "${SHINKOU_ENGINE_ROOT}/tests/PhysicsWorldTests.cpp")
    target_link_libraries(shinkou_physics_world_tests PRIVATE shinkou_physics)
    if(WIN32 AND SHINKOU_WITH_PHYSX AND SHINKOU_PHYSX_ROOT)
        file(GLOB_RECURSE SHINKOU_PHYSX_RUNTIME_DLLS LIST_DIRECTORIES false
            "${SHINKOU_PHYSX_ROOT}/bin/*.dll"
            "${SHINKOU_PHYSX_ROOT}/lib/*.dll")
        foreach(SHINKOU_PHYSX_RUNTIME_DLL IN LISTS SHINKOU_PHYSX_RUNTIME_DLLS)
            get_filename_component(SHINKOU_PHYSX_RUNTIME_NAME
                "${SHINKOU_PHYSX_RUNTIME_DLL}" NAME)
            add_custom_command(TARGET shinkou_physics_world_tests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SHINKOU_PHYSX_RUNTIME_DLL}"
                    "$<TARGET_FILE_DIR:shinkou_physics_world_tests>/${SHINKOU_PHYSX_RUNTIME_NAME}"
                VERBATIM)
        endforeach()
    endif()
    add_test(NAME shinkou_physics_world_tests COMMAND shinkou_physics_world_tests)
endif()

if(SHINKOU_PHYSICS_STANDALONE_PROJECT)
    install(TARGETS shinkou_physics
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin)
    install(DIRECTORY "${SHINKOU_ENGINE_ROOT}/include/" DESTINATION include)
endif()
