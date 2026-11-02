include_guard(GLOBAL)
include(FetchContent)
option(SHINKOU_FETCH_DEPENDENCIES "Download pinned dependencies when not cached" ON)

# Source-only dependencies: our targets select the required files; upstream
# examples, tests, installers and transitive downloads never run at configure.
function(shinkou_fetch_dependency name output_directory)
    set(registry "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/dependencies.json")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${registry}")
    file(READ "${registry}" manifest)
    string(JSON entry GET "${manifest}" libraries "${name}")
    string(JSON version GET "${entry}" version)
    string(JSON url GET "${entry}" url)
    string(JSON sha256 GET "${entry}" sha256)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    # Always use the declared archive, even if an unrelated package is installed.
    set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
    set(content_name "shinkou_${name}")
    string(TOUPPER "${content_name}" content_upper)
    if(NOT SHINKOU_FETCH_DEPENDENCIES)
        set(FETCHCONTENT_FULLY_DISCONNECTED TRUE)
    endif()
    FetchContent_Declare(${content_name}
        URL "${url}"
        URL_HASH "SHA256=${sha256}"
        TLS_VERIFY TRUE
        TIMEOUT 120
        INACTIVITY_TIMEOUT 30
        SOURCE_SUBDIR __shinkou_source_only__)
    FetchContent_MakeAvailable(${content_name})
    FetchContent_GetProperties(${content_name} SOURCE_DIR source_directory)
    string(JSON first_license GET "${entry}" licenses 0)
    if(NOT EXISTS "${source_directory}/${first_license}")
        message(FATAL_ERROR "Missing cached ${name} ${version}. Enable SHINKOU_FETCH_DEPENDENCIES for the first configure, or supply FETCHCONTENT_SOURCE_DIR_${content_upper}.")
    endif()

    string(JSON patch ERROR_VARIABLE patch_error GET "${entry}" patch)
    if(NOT patch_error)
        set(patch_file "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/${patch}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch_file}")
        execute_process(COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_DIRECTORY=${source_directory}" "-DPATCH_FILE=${patch_file}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ShinkouApplyDependencyPatch.cmake"
            RESULT_VARIABLE patch_result)
        if(NOT patch_result EQUAL 0)
            message(FATAL_ERROR "Cannot apply Shinkou integration patch for ${name} ${version}")
        endif()
    endif()
    set(${output_directory} "${source_directory}" PARENT_SCOPE)
    message(STATUS "Shinkou dependency ${name}: ${version} (pinned archive)")
endfunction()

function(shinkou_install_dependency_licenses name source_directory)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../third_party/dependencies.json" manifest)
    string(JSON licenses GET "${manifest}" libraries "${name}" licenses)
    string(JSON count LENGTH "${licenses}")
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON relative_path GET "${licenses}" ${index})
        install(FILES "${source_directory}/${relative_path}"
            DESTINATION "share/shinkou/licenses/${name}")
    endforeach()
endfunction()
