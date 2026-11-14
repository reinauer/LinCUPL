# Sets LINCUPL_VERSION to the project version in the top-level CMakeLists.txt,
# so the GUIs report the same version as the command line programs also when
# they are configured on their own.
if(NOT DEFINED LINCUPL_VERSION)
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../../../CMakeLists.txt" _lincupl_top)
    string(REGEX MATCH "project\\(lincupl[ \t\r\n]+VERSION[ \t\r\n]+([0-9.]+)" _lincupl_match "${_lincupl_top}")
    if(NOT CMAKE_MATCH_1)
        message(FATAL_ERROR "no project(lincupl VERSION ...) in the top-level CMakeLists.txt")
    endif()
    set(LINCUPL_VERSION "${CMAKE_MATCH_1}")
endif()
