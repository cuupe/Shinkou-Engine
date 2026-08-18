cmake_minimum_required(VERSION 3.25)

project(ShinkouInput VERSION 1.0.0 LANGUAGES C CXX)

option(SHINKOU_INPUT_FETCH_SDL3 "Fetch SDL3 source when no source directory is supplied" ON)
set(SHINKOU_INPUT_SDL3_SOURCE_DIR "" CACHE PATH "Optional local SDL3 source directory")

if(SHINKOU_INPUT_SDL3_SOURCE_DIR)
    if(NOT EXISTS "${SHINKOU_INPUT_SDL3_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "SHINKOU_INPUT_SDL3_SOURCE_DIR does not contain an SDL3 CMake project for ShinkouInput")
    endif()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    add_subdirectory("${SHINKOU_INPUT_SDL3_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/shinkou_input_sdl3" EXCLUDE_FROM_ALL)
elseif(SHINKOU_INPUT_FETCH_SDL3)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
else()
    message(FATAL_ERROR "ShinkouInput requires SDL3 source: set SHINKOU_INPUT_SDL3_SOURCE_DIR or enable SHINKOU_INPUT_FETCH_SDL3")
endif()

if(NOT TARGET SDL3::SDL3-static)
    message(FATAL_ERROR "SDL3 static target was not created; ShinkouInput must not link SDL3 shared")
endif()

add_library(shinkou_input SHARED
    src/ShinkouInput.cpp
)
set_target_properties(shinkou_input PROPERTIES OUTPUT_NAME shinkou_input)
add_library(ShinkouInput::ShinkouInput ALIAS shinkou_input)

target_include_directories(shinkou_input
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(shinkou_input PRIVATE cxx_std_17)
target_link_libraries(shinkou_input PRIVATE SDL3::SDL3-static)
target_compile_definitions(shinkou_input PRIVATE SHINKOU_INPUT_BUILDING_DLL=1)

if(MSVC)
    target_compile_options(shinkou_input PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(shinkou_input PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(shinkou_input PROPERTIES
    OUTPUT_NAME "ShinkouInput"
    PREFIX ""
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
