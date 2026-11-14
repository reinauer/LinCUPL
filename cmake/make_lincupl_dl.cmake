# Builds lincupl.dl, the default device library of the LinCUPL tools: the
# devices of Atmel.dl, then those that only cupl.dl has, copied with cbld -b.
# Atmel.dl's devices keep their directory slots and image offsets, so a design
# for one of them gives the same output as with Atmel.dl itself.
#
#   cmake -DCBLD=<cbld> -DATMEL=<Atmel.dl> -DCUPL=<cupl.dl> -DOUT=<lincupl.dl>
#         -P make_lincupl_dl.cmake

foreach(_v CBLD ATMEL CUPL OUT)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "make_lincupl_dl.cmake: ${_v} is not set")
    endif()
    get_filename_component(${_v} "${${_v}}" ABSOLUTE)
endforeach()

# The device names of a library, in directory order, from cbld -t (which,
# like the original, prints on stderr): the names follow the line that names
# the library and its revision.
function(_dl_device_names lib out_var)
    execute_process(COMMAND "${CBLD}" -t -u "${lib}"
                    OUTPUT_VARIABLE _text ERROR_VARIABLE _text RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "cbld -t -u ${lib} failed (${_rc}):\n${_text}")
    endif()
    string(FIND "${_text}" "rev:" _pos)
    if(_pos LESS 0)
        message(FATAL_ERROR "cbld -t -u ${lib}: no device listing:\n${_text}")
    endif()
    string(SUBSTRING "${_text}" ${_pos} -1 _text)
    string(FIND "${_text}" "\n" _pos)
    math(EXPR _pos "${_pos} + 1")
    string(SUBSTRING "${_text}" ${_pos} -1 _text)
    string(REGEX MATCHALL "[^ \t\r\n]+" _names "${_text}")
    if(NOT _names)
        message(FATAL_ERROR "cbld -t -u ${lib}: no devices listed:\n${_text}")
    endif()
    set(${out_var} "${_names}" PARENT_SCOPE)
endfunction()

_dl_device_names("${ATMEL}" _atmel)
_dl_device_names("${CUPL}" _cupl)

# cupl.dl's devices that Atmel.dl lacks, each once (cupl.dl lists p16l8 twice;
# a lookup takes the first entry)
set(_extra)
foreach(_n IN LISTS _cupl)
    list(FIND _atmel "${_n}" _in_atmel)
    list(FIND _extra "${_n}" _seen)
    if(_in_atmel LESS 0 AND _seen LESS 0)
        list(APPEND _extra "${_n}")
    endif()
endforeach()

get_filename_component(_out_dir "${OUT}" DIRECTORY)
get_filename_component(_out_name "${OUT}" NAME_WE)
set(_work "${_out_dir}/.${_out_name}.build")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}")

string(REPLACE ";" "\n" _extra_lines "${_extra}")
file(WRITE "${_work}/${_out_name}.bld"
     "target ${_out_name}\nsource ${ATMEL}\n*\nsource ${CUPL}\n${_extra_lines}\n")

execute_process(COMMAND "${CBLD}" -b "${_out_name}.bld"
                WORKING_DIRECTORY "${_work}"
                OUTPUT_VARIABLE _text ERROR_VARIABLE _text RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0 OR NOT EXISTS "${_work}/${_out_name}.dl")
    message(FATAL_ERROR "cbld -b ${_work}/${_out_name}.bld failed (${_rc}):\n${_text}")
endif()

# the result must list every device of both libraries
_dl_device_names("${_work}/${_out_name}.dl" _merged)
list(LENGTH _atmel _n_atmel)
list(LENGTH _extra _n_extra)
list(LENGTH _merged _n_merged)
math(EXPR _n_want "${_n_atmel} + ${_n_extra}")
if(NOT _n_merged EQUAL _n_want)
    message(FATAL_ERROR "${_out_name}.dl lists ${_n_merged} devices, expected ${_n_want}")
endif()

file(RENAME "${_work}/${_out_name}.dl" "${OUT}")
file(REMOVE_RECURSE "${_work}")
message(STATUS "${OUT}: ${_n_atmel} devices of Atmel.dl, ${_n_extra} of cupl.dl")
