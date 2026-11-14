# Help pages of LinCUPL and LinSim.
#
# The pages, their stylesheet and the Qt Help project (lincupl.qhp: table of
# contents, index keywords, file list) are copied to
# ${LINCUPL_BUILD_SHARE_DIR}/help, where the programs find them when run
# from the build tree.  With Qt Help, qhelpgenerator then compiles the
# collection project there into lincupl.qch (the pages) and lincupl.qhc (the
# collection that registers it).  Without it the programs show the copied
# pages and read the contents and index from lincupl.qhp themselves.
#
# Everything ends up in share/lincupl/help when installed.  The examples/
# directory holds the complete designs the pages quote and is not installed.

file(GLOB _help_pages CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.css"
)
set(_help_sources ${_help_pages}
    "${CMAKE_CURRENT_SOURCE_DIR}/lincupl.qhp"
    "${CMAKE_CURRENT_SOURCE_DIR}/lincupl.qhcp"
)

set(_help_stage "${LINCUPL_BUILD_SHARE_DIR}/help")
set(_help_staged)
foreach(_src IN LISTS _help_sources)
    get_filename_component(_name "${_src}" NAME)
    add_custom_command(
        OUTPUT "${_help_stage}/${_name}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_help_stage}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_src}" "${_help_stage}/${_name}"
        DEPENDS "${_src}"
        VERBATIM
    )
    list(APPEND _help_staged "${_help_stage}/${_name}")
endforeach()

set(_help_outputs ${_help_staged})
if(LINCUPL_HAVE_QTHELP)
    add_custom_command(
        OUTPUT "${_help_stage}/lincupl.qch" "${_help_stage}/lincupl.qhc"
        COMMAND "${CMAKE_COMMAND}" -E remove -f "${_help_stage}/lincupl.qch" "${_help_stage}/lincupl.qhc"
        COMMAND "${LINCUPL_QHELPGENERATOR}" -s lincupl.qhcp -o lincupl.qhc
        WORKING_DIRECTORY "${_help_stage}"
        DEPENDS ${_help_staged}
        COMMENT "Compiling the help pages with qhelpgenerator"
        VERBATIM
    )
    list(APPEND _help_outputs "${_help_stage}/lincupl.qch" "${_help_stage}/lincupl.qhc")
endif()

add_custom_target(lincupl_help ALL DEPENDS ${_help_outputs})

install(FILES ${_help_outputs} DESTINATION "${CMAKE_INSTALL_DATADIR}/lincupl/help")
