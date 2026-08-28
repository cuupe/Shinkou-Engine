cmake_minimum_required(VERSION 3.20)
project(ShinkouUI VERSION 0.1.0 LANGUAGES CXX)

option(SHINKOU_UI_BUILD_TESTS "Build ShinkouUI tests" ON)
option(SHINKOU_UI_BUILD_EXAMPLE "Build the backend-neutral preview example" ON)
option(SHINKOU_UI_BUILD_STUDIO "Build the Windows-only visual style studio" ON)
option(SHINKOU_UI_BUILD_NATIVE_API "Build the C ABI editor bridge" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(shinkou_ui STATIC
    src/Animation.cpp
    src/AssetBrowser.cpp
    src/Editor.cpp
    src/EditorWorkspace.cpp
    src/Factory.cpp
    src/Render.cpp
    src/SceneModel.cpp
    src/Style.cpp
    src/Ui.cpp
    src/Widgets.cpp
    src/Xml.cpp
    src/Win32Platform.cpp
)

target_include_directories(shinkou_ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(shinkou_ui PUBLIC cxx_std_17)
target_compile_definitions(shinkou_ui PUBLIC SHINKOU_UI_VERSION="0.1.0")
if(WIN32)
    target_compile_definitions(shinkou_ui PRIVATE SHINKOU_UI_PLATFORM_WINDOWS=1 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(MSVC)
    target_compile_options(shinkou_ui PRIVATE /W4 /permissive-)
else()
    target_compile_options(shinkou_ui PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(SHINKOU_UI_BUILD_EXAMPLE)
    add_executable(shinkou_ui_preview examples/Preview.cpp)
    target_link_libraries(shinkou_ui_preview PRIVATE shinkou_ui)
endif()

if(SHINKOU_UI_BUILD_NATIVE_API)
    add_library(shinkou_ui_native SHARED src/CAPI.cpp)
    target_link_libraries(shinkou_ui_native PRIVATE shinkou_ui)
    target_include_directories(shinkou_ui_native PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    )
    target_compile_features(shinkou_ui_native PUBLIC cxx_std_17)
    target_compile_definitions(shinkou_ui_native PRIVATE SHINKOU_UI_BUILD_SHARED=1)
    set_target_properties(shinkou_ui_native PROPERTIES PREFIX "" OUTPUT_NAME "shinkou_ui_native")
    if(NOT MSVC)
        target_link_options(shinkou_ui_native PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

if(WIN32 AND SHINKOU_UI_BUILD_STUDIO)
    add_executable(shinkou_ui_studio WIN32 studio/StyleStudio.cpp)
    target_link_libraries(shinkou_ui_studio PRIVATE shinkou_ui user32 gdi32 comctl32 comdlg32)
    target_compile_definitions(shinkou_ui_studio PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(SHINKOU_UI_BUILD_TESTS)
    enable_testing()
    function(shinkou_ui_test name source)
        add_executable(${name} tests/${source})
        target_link_libraries(${name} PRIVATE shinkou_ui)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()
    shinkou_ui_test(shinkou_ui_xml_tests XmlTests.cpp)
    shinkou_ui_test(shinkou_ui_asset_browser_tests AssetBrowserTests.cpp)
    shinkou_ui_test(shinkou_ui_factory_tests FactoryTests.cpp)
    shinkou_ui_test(shinkou_ui_style_tests StyleTests.cpp)
    shinkou_ui_test(shinkou_ui_animation_tests AnimationTests.cpp)
    shinkou_ui_test(shinkou_ui_widget_tests WidgetTests.cpp)
    shinkou_ui_test(shinkou_ui_render_tests RenderTests.cpp)
    shinkou_ui_test(shinkou_ui_editor_tests EditorTests.cpp)
    shinkou_ui_test(shinkou_ui_scene_model_tests SceneModelTests.cpp)
    shinkou_ui_test(shinkou_ui_workspace_tests WorkspaceTests.cpp)
    if(SHINKOU_UI_BUILD_NATIVE_API)
        add_executable(shinkou_ui_native_api_tests tests/NativeApiTests.cpp)
        target_link_libraries(shinkou_ui_native_api_tests PRIVATE shinkou_ui_native)
        add_test(NAME shinkou_ui_native_api_tests COMMAND shinkou_ui_native_api_tests)
    endif()
endif()
