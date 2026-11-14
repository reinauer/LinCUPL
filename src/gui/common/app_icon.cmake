# Application icon of a GUI program.
#
# lincupl_app_icon(<target> <name>) compiles icons/png/<name>-<size>.png into
# the target as Qt resources (:/icons/<name>-<size>.png, read by
# app_icon.h) and, outside macOS, installs the icon into the hicolor theme
# together with <name>.desktop, so desktop environments show it in their
# launchers and task bars.  On macOS the running program hands the icon to
# the Dock itself.

set(LINCUPL_ICON_SIZES 16 24 32 48 64 128 256 512)
set(_lincupl_icon_dir "${CMAKE_CURRENT_LIST_DIR}/icons")
set(_lincupl_common_dir "${CMAKE_CURRENT_LIST_DIR}")

function(lincupl_app_icon target name)
    set(_files)
    foreach(_size IN LISTS LINCUPL_ICON_SIZES)
        list(APPEND _files "${_lincupl_icon_dir}/png/${name}-${_size}.png")
    endforeach()
    qt_add_resources(${target} "${name}_icon"
        PREFIX "/icons"
        BASE "${_lincupl_icon_dir}/png"
        FILES ${_files}
    )

    if(NOT APPLE)
        include(GNUInstallDirs)
        foreach(_size IN LISTS LINCUPL_ICON_SIZES)
            install(FILES "${_lincupl_icon_dir}/png/${name}-${_size}.png"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps"
                RENAME "${name}.png")
        endforeach()
        install(FILES "${_lincupl_icon_dir}/${name}.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
        install(FILES "${_lincupl_common_dir}/${name}.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    endif()
endfunction()

# Toolbar and menu icons: every icons/actions/png/<name>-<size>.png as
# :/actions/<name>-<size>.png, read by action_icons.h.
function(lincupl_action_icons target)
    file(GLOB _files "${_lincupl_icon_dir}/actions/png/*.png")
    qt_add_resources(${target} "${target}_action_icons"
        PREFIX "/actions"
        BASE "${_lincupl_icon_dir}/actions/png"
        FILES ${_files}
    )
endfunction()
