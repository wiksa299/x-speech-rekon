# Changes from the Listen fork, including the Fluid Speech Pro residual encoder block.
# The public upstream archive plus this patch is usable by CI without a
# connection to the private fork. A source override may already be the fork.
function(xsl_apply_transcribe_listen source_dir)
    if(FETCHCONTENT_SOURCE_DIR_TRANSCRIBE AND EXISTS "${FETCHCONTENT_SOURCE_DIR_TRANSCRIBE}/include/transcribe/listen.h")
        return()
    endif()
    find_package(Git REQUIRED)
    get_filename_component(parent "${source_dir}" DIRECTORY)
    set(patch "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/transcribe-listen.patch")
    set(command "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${parent}" "${GIT_EXECUTABLE}" apply)
    execute_process(COMMAND ${command} --check --reverse "${patch}"
        WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE applied ERROR_QUIET)
    if(NOT applied EQUAL 0 OR NOT EXISTS "${source_dir}/include/transcribe/listen.h")
        execute_process(COMMAND ${command} "${patch}" WORKING_DIRECTORY "${source_dir}"
            COMMAND_ERROR_IS_FATAL ANY)
    endif()
    if(NOT EXISTS "${source_dir}/include/transcribe/listen.h")
        message(FATAL_ERROR "The Listen transcribe.cpp patch was not applied")
    endif()
endfunction()
