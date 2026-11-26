# Compile to a temporary file so a failed build preserves the last library.
get_filename_component(_out_dir "${OUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")
file(REMOVE "${OUT}.tmp")
execute_process(
    COMMAND "${PYTHON}" "${COMPILER}" "${SOURCE}" -o "${OUT}.tmp"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _log ERROR_VARIABLE _log
)
if(NOT _result EQUAL 0)
    file(REMOVE "${OUT}.tmp")
    message(FATAL_ERROR "Device compilation failed:\n${_log}")
endif()
file(RENAME "${OUT}.tmp" "${OUT}")
