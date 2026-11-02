include(ShinkouFetchDependency)

# Source revisions and archive checksums are centralized in dependencies.json.
shinkou_fetch_dependency(ufbx ufbx_SOURCE_DIR)
shinkou_fetch_dependency(pugixml pugixml_SOURCE_DIR)
shinkou_fetch_dependency(miniz miniz_SOURCE_DIR)
set(SHINKOU_MINIZ_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/shinkou_dependency_include")
file(MAKE_DIRECTORY "${SHINKOU_MINIZ_INCLUDE_DIR}")
file(WRITE "${SHINKOU_MINIZ_INCLUDE_DIR}/miniz_export.h" "#pragma once\n#define MINIZ_EXPORT\n")
add_library(shinkou_asset_readers STATIC
    "${ufbx_SOURCE_DIR}/ufbx.c"
    "${pugixml_SOURCE_DIR}/src/pugixml.cpp"
    "${miniz_SOURCE_DIR}/miniz.c"
    "${miniz_SOURCE_DIR}/miniz_zip.c"
    "${miniz_SOURCE_DIR}/miniz_tinfl.c"
    "${miniz_SOURCE_DIR}/miniz_tdef.c")
target_include_directories(shinkou_asset_readers SYSTEM PUBLIC
    "${ufbx_SOURCE_DIR}" "${pugixml_SOURCE_DIR}/src"
    "${miniz_SOURCE_DIR}" "${SHINKOU_MINIZ_INCLUDE_DIR}")
target_compile_features(shinkou_asset_readers PRIVATE c_std_99 cxx_std_17)
foreach(reader IN ITEMS ufbx pugixml miniz)
    shinkou_install_dependency_licenses(${reader} "${${reader}_SOURCE_DIR}")
endforeach()

set(SHINKOU_WITH_SDL3 OFF)
set(SHINKOU_WITH_VULKAN OFF)
set(SHINKOU_WITH_PHYSX OFF)
set(SHINKOU_WITH_PHYSX_COOKING OFF)
set(SHINKOU_WITH_PHYSX_EXTENSIONS OFF)
set(SHINKOU_WITH_IMGUI OFF)
set(SHINKOU_WITH_MINIAUDIO OFF)
set(SHINKOU_WITH_LUA OFF)
set(SHINKOU_WITH_PYTHON OFF)
set(SHINKOU_WITH_CSHARP OFF)
set(SHINKOU_WITH_SPDLOG OFF)

shinkou_fetch_dependency(spdlog spdlog_SOURCE_DIR)
find_package(Threads REQUIRED)
add_library(shinkou_spdlog_header_only INTERFACE)
target_include_directories(shinkou_spdlog_header_only SYSTEM INTERFACE "${spdlog_SOURCE_DIR}/include")
target_link_libraries(shinkou_spdlog_header_only INTERFACE Threads::Threads)
set(SHINKOU_SPDLOG_TARGET shinkou_spdlog_header_only)
set(SHINKOU_WITH_SPDLOG ON)
shinkou_install_dependency_licenses(spdlog "${spdlog_SOURCE_DIR}")

find_package(Vulkan QUIET)
if(TARGET Vulkan::Vulkan)
    set(SHINKOU_WITH_VULKAN ON)
endif()

if(SHINKOU_ENABLE_PHYSX)
    # PhysX does not ship a stable CMake package across SDK generations. Keep
    # discovery explicit and cacheable so projects can point at either a
    # source SDK or a prebuilt binary package without hard-coding a machine
    # path into the engine.
    set(SHINKOU_PHYSX_HINTS)
    if(SHINKOU_PHYSX_ROOT)
        list(APPEND SHINKOU_PHYSX_HINTS
            "${SHINKOU_PHYSX_ROOT}"
            "${SHINKOU_PHYSX_ROOT}/include"
            "${SHINKOU_PHYSX_ROOT}/Include")
    endif()
    if(DEFINED ENV{PHYSX_ROOT})
        list(APPEND SHINKOU_PHYSX_HINTS "$ENV{PHYSX_ROOT}" "$ENV{PHYSX_ROOT}/include" "$ENV{PHYSX_ROOT}/Include")
    endif()

    find_path(PHYSX_INCLUDE_DIR PxPhysicsAPI.h PATHS ${SHINKOU_PHYSX_HINTS}
        PATH_SUFFIXES include Include physx physx/include physx/Include)

    set(SHINKOU_PHYSX_LIBRARY_HINTS)
    if(SHINKOU_PHYSX_LIBRARY_DIR)
        list(APPEND SHINKOU_PHYSX_LIBRARY_HINTS "${SHINKOU_PHYSX_LIBRARY_DIR}")
    endif()
    if(SHINKOU_PHYSX_ROOT)
        list(APPEND SHINKOU_PHYSX_LIBRARY_HINTS
            "${SHINKOU_PHYSX_ROOT}/lib"
            "${SHINKOU_PHYSX_ROOT}/Lib"
            "${SHINKOU_PHYSX_ROOT}/bin")
    endif()
    if(PHYSX_INCLUDE_DIR)
        get_filename_component(SHINKOU_PHYSX_INCLUDE_PARENT "${PHYSX_INCLUDE_DIR}" DIRECTORY)
        list(APPEND SHINKOU_PHYSX_LIBRARY_HINTS
            "${SHINKOU_PHYSX_INCLUDE_PARENT}/lib"
            "${SHINKOU_PHYSX_INCLUDE_PARENT}/Lib"
            "${PHYSX_INCLUDE_DIR}/../lib"
            "${PHYSX_INCLUDE_DIR}/../Lib")
    endif()

    # The official GitHub SDK is generated into configuration-specific
    # directories such as bin/win.x86_64.vc143.mt/checked or a Linux
    # equivalent. Discover those directories without assuming a compiler,
    # runtime, or debug/release spelling so the same integration works on
    # Windows, Linux, and other supported PhysX targets.
    if(SHINKOU_PHYSX_ROOT)
        file(GLOB_RECURSE SHINKOU_PHYSX_LIBRARY_FILES LIST_DIRECTORIES false
            "${SHINKOU_PHYSX_ROOT}/bin/*.lib"
            "${SHINKOU_PHYSX_ROOT}/bin/*.a"
            "${SHINKOU_PHYSX_ROOT}/bin/*.so"
            "${SHINKOU_PHYSX_ROOT}/lib/*.lib"
            "${SHINKOU_PHYSX_ROOT}/lib/*.a"
            "${SHINKOU_PHYSX_ROOT}/lib/*.so")
        foreach(SHINKOU_PHYSX_LIBRARY_FILE IN LISTS SHINKOU_PHYSX_LIBRARY_FILES)
            get_filename_component(SHINKOU_PHYSX_LIBRARY_PARENT
                "${SHINKOU_PHYSX_LIBRARY_FILE}" DIRECTORY)
            list(APPEND SHINKOU_PHYSX_LIBRARY_HINTS "${SHINKOU_PHYSX_LIBRARY_PARENT}")
        endforeach()
        list(REMOVE_DUPLICATES SHINKOU_PHYSX_LIBRARY_HINTS)
    endif()

    find_library(PHYSX_CORE_LIBRARY NAMES PhysX PhysX_64 PhysX_static_64 PhysX_64_DEBUG
        PATHS ${SHINKOU_PHYSX_LIBRARY_HINTS})
    find_library(PHYSX_COMMON_LIBRARY NAMES PhysXCommon PhysXCommon_64 PhysXCommon_static_64 PhysXCommon_64_DEBUG
        PATHS ${SHINKOU_PHYSX_LIBRARY_HINTS})
    find_library(PHYSX_FOUNDATION_LIBRARY NAMES PhysXFoundation PhysXFoundation_64 PhysXFoundation_static_64 PhysXFoundation_64_DEBUG
        PATHS ${SHINKOU_PHYSX_LIBRARY_HINTS})

    if(PHYSX_INCLUDE_DIR AND PHYSX_CORE_LIBRARY AND PHYSX_COMMON_LIBRARY AND PHYSX_FOUNDATION_LIBRARY)
        set(PHYSX_LIBRARIES ${PHYSX_CORE_LIBRARY} ${PHYSX_COMMON_LIBRARY} ${PHYSX_FOUNDATION_LIBRARY})
        set(SHINKOU_WITH_PHYSX ON)

        find_library(PHYSX_EXTENSIONS_LIBRARY NAMES PhysXExtensions PhysXExtensions_64 PhysXExtensions_static_64 PhysXExtensions_64_DEBUG
            PATHS ${SHINKOU_PHYSX_LIBRARY_HINTS})
        if(PHYSX_EXTENSIONS_LIBRARY)
            list(APPEND PHYSX_LIBRARIES ${PHYSX_EXTENSIONS_LIBRARY})
            set(SHINKOU_WITH_PHYSX_EXTENSIONS ON)
        endif()

        if(EXISTS "${PHYSX_INCLUDE_DIR}/cooking/PxCooking.h")
            find_library(PHYSX_COOKING_LIBRARY NAMES PhysXCooking PhysXCooking_64 PhysXCooking_static_64 PhysXCooking_64_DEBUG
                PATHS ${SHINKOU_PHYSX_LIBRARY_HINTS})
            if(PHYSX_COOKING_LIBRARY)
                list(APPEND PHYSX_LIBRARIES ${PHYSX_COOKING_LIBRARY})
                set(SHINKOU_WITH_PHYSX_COOKING ON)
            endif()
        endif()
    elseif(SHINKOU_ENABLE_PHYSX)
        message(STATUS "PhysX SDK not found; the engine will use its deterministic fallback")
    endif()
endif()

if(SHINKOU_ENABLE_IMGUI)
    shinkou_fetch_dependency(imgui imgui_SOURCE_DIR)
    shinkou_fetch_dependency(hello_imgui hello_imgui_SOURCE_DIR)
    shinkou_fetch_dependency(implot implot_SOURCE_DIR)
    shinkou_fetch_dependency(imguizmo imguizmo_SOURCE_DIR)
    shinkou_fetch_dependency(codicons codicons_SOURCE_DIR)
    set(IMGUI_INCLUDE_DIR "${imgui_SOURCE_DIR}")
    add_library(shinkou_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui_demo.cpp"
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
    target_include_directories(shinkou_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}")
    target_include_directories(shinkou_imgui PRIVATE "${imgui_SOURCE_DIR}/backends")
    if(WIN32)
        target_sources(shinkou_imgui PRIVATE
            "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp")
        target_compile_definitions(shinkou_imgui PRIVATE VK_USE_PLATFORM_WIN32_KHR=1)
        target_link_libraries(shinkou_imgui PUBLIC d3d11 dxgi dwmapi)
    endif()
    if(TARGET Vulkan::Vulkan)
        target_sources(shinkou_imgui PRIVATE "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
        target_link_libraries(shinkou_imgui PUBLIC Vulkan::Vulkan)
    endif()
    set(IMGUI_LIBRARY shinkou_imgui)
    set(SHINKOU_WITH_IMGUI ON)
    add_library(shinkou_editor_widgets STATIC
        "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp"
        "${hello_imgui_SOURCE_DIR}/src/hello_imgui/impl/imgui_theme.cpp"
        "${implot_SOURCE_DIR}/implot.cpp"
        "${implot_SOURCE_DIR}/implot_items.cpp")
    target_include_directories(shinkou_editor_widgets SYSTEM PUBLIC
        "${imguizmo_SOURCE_DIR}/src" "${hello_imgui_SOURCE_DIR}/src" "${implot_SOURCE_DIR}")
    target_link_libraries(shinkou_editor_widgets PUBLIC shinkou_imgui)
    foreach(widget IN ITEMS imgui hello_imgui implot imguizmo codicons)
        shinkou_install_dependency_licenses(${widget} "${${widget}_SOURCE_DIR}")
    endforeach()
endif()

if(SHINKOU_ENABLE_MINIAUDIO)
    shinkou_fetch_dependency(miniaudio miniaudio_SOURCE_DIR)
    set(MINIAUDIO_INCLUDE_DIR "${miniaudio_SOURCE_DIR}")
    set(SHINKOU_WITH_MINIAUDIO ON)
    shinkou_install_dependency_licenses(miniaudio "${miniaudio_SOURCE_DIR}")
endif()

if(SHINKOU_ENABLE_LUA)
    find_package(Lua QUIET)
    if(LUA_FOUND)
        set(SHINKOU_WITH_LUA ON)
    endif()
endif()

if(SHINKOU_ENABLE_PYTHON)
    find_package(Python3 COMPONENTS Development QUIET)
    if(Python3_Development_FOUND)
        set(SHINKOU_WITH_PYTHON ON)
    endif()
endif()

if(SHINKOU_ENABLE_CSHARP)
    find_program(SHINKOU_DOTNET_EXECUTABLE dotnet)
    if(SHINKOU_DOTNET_EXECUTABLE)
        set(SHINKOU_WITH_CSHARP ON)
    endif()
endif()
