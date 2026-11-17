find_package(Git REQUIRED)
get_filename_component(source_parent "${SOURCE_DIR}" DIRECTORY)
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${source_parent}" "${GIT_EXECUTABLE}" -C
        "${SOURCE_DIR}" apply --reverse --check "${PATCH_FILE}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET
    ERROR_QUIET
)
if(already_applied EQUAL 0)
    return()
endif()
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${source_parent}" "${GIT_EXECUTABLE}" -C
        "${SOURCE_DIR}" apply --check "${PATCH_FILE}"
    COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
    COMMAND
        "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${source_parent}" "${GIT_EXECUTABLE}" -C
        "${SOURCE_DIR}" apply "${PATCH_FILE}"
    COMMAND_ERROR_IS_FATAL ANY
)
