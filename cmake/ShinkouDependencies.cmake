include(FetchContent)

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

find_package(spdlog CONFIG QUIET)
if(TARGET spdlog::spdlog_header_only)
    set(SHINKOU_SPDLOG_TARGET spdlog::spdlog_header_only)
    set(SHINKOU_WITH_SPDLOG ON)
elseif(TARGET spdlog::spdlog)
    set(SHINKOU_SPDLOG_TARGET spdlog::spdlog)
    set(SHINKOU_WITH_SPDLOG ON)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/engine/include/spdlog/spdlog.h")
    add_library(shinkou_spdlog_header_only INTERFACE)
    target_include_directories(shinkou_spdlog_header_only INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/include")
    set(SHINKOU_SPDLOG_TARGET shinkou_spdlog_header_only)
    set(SHINKOU_WITH_SPDLOG ON)
elseif(SHINKOU_FETCH_DEPENDENCIES)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.16.0)
    FetchContent_MakeAvailable(spdlog)
    set(SHINKOU_SPDLOG_TARGET spdlog::spdlog_header_only)
    set(SHINKOU_WITH_SPDLOG ON)
endif()

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
    find_path(IMGUI_INCLUDE_DIR imgui.h PATH_SUFFIXES imgui)
    if(IMGUI_INCLUDE_DIR)
        find_library(IMGUI_LIBRARY NAMES imgui)
        if(IMGUI_LIBRARY)
            set(SHINKOU_WITH_IMGUI ON)
        endif()
    elseif(SHINKOU_FETCH_DEPENDENCIES)
        FetchContent_Declare(imgui
            GIT_REPOSITORY https://github.com/ocornut/imgui.git
            GIT_TAG v1.91.9b
        )
        FetchContent_MakeAvailable(imgui)
        set(IMGUI_INCLUDE_DIR "${imgui_SOURCE_DIR}")
        add_library(shinkou_imgui STATIC
            "${imgui_SOURCE_DIR}/imgui.cpp"
            "${imgui_SOURCE_DIR}/imgui_draw.cpp"
            "${imgui_SOURCE_DIR}/imgui_tables.cpp"
            "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp"
            "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
        )
        target_include_directories(shinkou_imgui PUBLIC "${imgui_SOURCE_DIR}")
        target_include_directories(shinkou_imgui PRIVATE "${imgui_SOURCE_DIR}/backends")
        if(WIN32)
            target_compile_definitions(shinkou_imgui PRIVATE VK_USE_PLATFORM_WIN32_KHR=1)
            target_link_libraries(shinkou_imgui PUBLIC d3d11 dxgi dwmapi)
        endif()
        if(TARGET Vulkan::Vulkan)
            target_link_libraries(shinkou_imgui PUBLIC Vulkan::Vulkan)
        endif()
        set(IMGUI_LIBRARY shinkou_imgui)
        set(SHINKOU_WITH_IMGUI ON)
    endif()
endif()

if(SHINKOU_ENABLE_MINIAUDIO)
    find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h
        PATHS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio"
        NO_DEFAULT_PATH)
    if(NOT MINIAUDIO_INCLUDE_DIR)
        find_path(MINIAUDIO_INCLUDE_DIR miniaudio.h)
    endif()
    if(MINIAUDIO_INCLUDE_DIR)
        set(SHINKOU_WITH_MINIAUDIO ON)
    elseif(SHINKOU_FETCH_DEPENDENCIES)
        FetchContent_Declare(miniaudio
            GIT_REPOSITORY https://github.com/mackron/miniaudio.git
            GIT_TAG 0.11.21
        )
        FetchContent_MakeAvailable(miniaudio)
        set(MINIAUDIO_INCLUDE_DIR "${miniaudio_SOURCE_DIR}")
        set(SHINKOU_WITH_MINIAUDIO ON)
    endif()
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
