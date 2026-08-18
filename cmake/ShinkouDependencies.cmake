include(FetchContent)

set(SHINKOU_WITH_SDL3 OFF)
set(SHINKOU_WITH_VULKAN OFF)
set(SHINKOU_WITH_PHYSX OFF)
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
    find_path(PHYSX_INCLUDE_DIR PxPhysicsAPI.h)
    find_library(PHYSX_CORE_LIBRARY NAMES PhysX PhysX_64)
    find_library(PHYSX_COMMON_LIBRARY NAMES PhysXCommon PhysXCommon_64)
    if(PHYSX_INCLUDE_DIR AND PHYSX_CORE_LIBRARY)
        set(PHYSX_LIBRARIES ${PHYSX_CORE_LIBRARY} ${PHYSX_COMMON_LIBRARY})
        set(SHINKOU_WITH_PHYSX ON)
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
