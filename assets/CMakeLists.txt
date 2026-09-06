cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ShinkouAssets VERSION 0.1.0 LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    option(SHINKOU_BUILD_TESTS "Build tests" ON)
    include(CTest)
endif()

add_library(shinkou_assets STATIC
    src/AssetSystem.cpp
)
set_target_properties(shinkou_assets PROPERTIES OUTPUT_NAME shinkou_assets)
add_library(ShinkouEngine::Assets ALIAS shinkou_assets)
target_include_directories(shinkou_assets PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(shinkou_assets PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(shinkou_assets PRIVATE /W4 /permissive-)
else()
    target_compile_options(shinkou_assets PRIVATE -Wall -Wextra -Wpedantic)
endif()

option(SHINKOU_ASSETS_BUILD_TESTS "Build standalone asset library tests" ON)
if(SHINKOU_ASSETS_BUILD_TESTS AND SHINKOU_BUILD_TESTS AND BUILD_TESTING)
    add_executable(shinkou_assets_tests tests/AssetSystemTests.cpp)
    target_link_libraries(shinkou_assets_tests PRIVATE shinkou_assets)
    if(MSVC)
        target_compile_options(shinkou_assets_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(shinkou_assets_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME shinkou_assets_tests COMMAND shinkou_assets_tests)

    add_executable(shinkou_assets_benchmarks tests/AssetSystemBenchmarks.cpp)
    target_link_libraries(shinkou_assets_benchmarks PRIVATE shinkou_assets)
endif()
