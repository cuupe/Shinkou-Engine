cmake_minimum_required(VERSION 3.20)
project(ShinkouRender VERSION 0.1.0 LANGUAGES CXX)

option(SHINKOU_RENDER_BUILD_TESTS "Build ShinkouRender display tests" ON)
option(SHINKOU_RENDER_BUILD_EXAMPLE "Build the standalone display preview" ON)
option(SHINKOU_RENDER_BUILD_NATIVE_API "Build the C ABI shared library for managed tools" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(shinkou_render STATIC
    src/DisplayList.cpp
    src/Renderer.cpp
    src/SoftwareDisplay.cpp
)
target_include_directories(shinkou_render PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(shinkou_render PUBLIC cxx_std_17)
target_compile_definitions(shinkou_render PUBLIC SHINKOU_RENDER_VERSION="0.1.0")
if(MSVC)
    target_compile_options(shinkou_render PRIVATE /W4 /permissive-)
else()
    target_compile_options(shinkou_render PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SHINKOU_RENDER_BUILD_EXAMPLE)
    add_executable(shinkou_render_preview examples/DisplayPreview.cpp)
    target_link_libraries(shinkou_render_preview PRIVATE shinkou_render)
endif()

if(SHINKOU_RENDER_BUILD_TESTS)
    enable_testing()
    add_executable(shinkou_render_display_tests tests/DisplayTests.cpp)
    target_link_libraries(shinkou_render_display_tests PRIVATE shinkou_render)
    add_test(NAME shinkou_render_display_tests COMMAND shinkou_render_display_tests)
endif()

if(SHINKOU_RENDER_BUILD_NATIVE_API)
    add_library(shinkou_render_native SHARED
        src/CAPI.cpp
        src/DisplayList.cpp
        src/Renderer.cpp
        src/SoftwareDisplay.cpp
    )
    target_include_directories(shinkou_render_native PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_features(shinkou_render_native PUBLIC cxx_std_17)
    target_compile_definitions(shinkou_render_native PRIVATE SHINKOU_RENDER_BUILD_SHARED=1)
    set_target_properties(shinkou_render_native PROPERTIES PREFIX "" OUTPUT_NAME "shinkou_render_native")
    if(MSVC)
        target_compile_options(shinkou_render_native PRIVATE /W4 /permissive-)
    else()
        target_compile_options(shinkou_render_native PRIVATE -Wall -Wextra -Wpedantic)
        target_link_options(shinkou_render_native PRIVATE -static-libgcc -static-libstdc++)
    endif()
    if(SHINKOU_RENDER_BUILD_TESTS)
        add_executable(shinkou_render_capi_tests tests/CapiTests.cpp)
        target_link_libraries(shinkou_render_capi_tests PRIVATE shinkou_render_native)
        add_test(NAME shinkou_render_capi_tests COMMAND shinkou_render_capi_tests)
    endif()
endif()
