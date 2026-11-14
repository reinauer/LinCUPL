# Help window of a GUI program (help_window.h).
#
# lincupl_help_window(<target>) adds the help window sources to the target
# and, when the top-level build found Qt Help and qhelpgenerator
# (LINCUPL_HAVE_QTHELP), links Qt6::Help and defines LINCUPL_HAS_QTHELP.
# Otherwise the window shows the plain HTML pages.

set(_lincupl_help_common_dir "${CMAKE_CURRENT_LIST_DIR}")

function(lincupl_help_window target)
    target_sources(${target} PRIVATE
        "${_lincupl_help_common_dir}/help_window.h"
        "${_lincupl_help_common_dir}/help_window.cpp"
    )
    if(LINCUPL_HAVE_QTHELP AND TARGET Qt6::Help)
        target_compile_definitions(${target} PRIVATE LINCUPL_HAS_QTHELP=1)
        target_link_libraries(${target} PRIVATE Qt6::Help)
    endif()
endfunction()
