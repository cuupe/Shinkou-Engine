cmake_minimum_required(VERSION 3.20)
find_package(Git REQUIRED QUIET)
get_filename_component(source_parent "${SOURCE_DIRECTORY}" DIRECTORY)
# Do not discover the engine's Git index when sources live inside its ignored
# build tree. This also works with a FetchContent cache outside the repository.
set(apply_command "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${source_parent}"
    "${GIT_EXECUTABLE}" apply --ignore-space-change --whitespace=nowarn)
execute_process(COMMAND ${apply_command} --reverse --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIRECTORY}" RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET)
if(already_applied EQUAL 0)
    return()
endif()
execute_process(COMMAND ${apply_command} --check "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIRECTORY}" RESULT_VARIABLE applicable
    ERROR_VARIABLE detail)
if(NOT applicable EQUAL 0)
    message(FATAL_ERROR "Integration patch does not match pinned source: ${detail}")
endif()
execute_process(COMMAND ${apply_command} "${PATCH_FILE}"
    WORKING_DIRECTORY "${SOURCE_DIRECTORY}" RESULT_VARIABLE applied
    ERROR_VARIABLE detail)
if(NOT applied EQUAL 0)
    message(FATAL_ERROR "Integration patch failed: ${detail}")
endif()
