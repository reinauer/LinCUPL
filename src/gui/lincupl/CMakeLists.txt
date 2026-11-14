cmake_minimum_required(VERSION 3.16)

include("${CMAKE_CURRENT_LIST_DIR}/../common/lincupl_version.cmake")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(lincupl VERSION ${LINCUPL_VERSION} LANGUAGES CXX)
    if(APPLE)
        foreach(_qt_prefix "/opt/homebrew/opt/qt" "/usr/local/opt/qt")
            if(EXISTS "${_qt_prefix}" AND NOT "${_qt_prefix}" IN_LIST CMAKE_PREFIX_PATH)
                list(APPEND CMAKE_PREFIX_PATH "${_qt_prefix}")
            endif()
        endforeach()
    endif()
    find_package(Qt6 COMPONENTS Widgets REQUIRED)
    find_package(Qt6 COMPONENTS PrintSupport QUIET)
else()
    if(NOT TARGET Qt6::Widgets)
        find_package(Qt6 COMPONENTS Widgets REQUIRED)
    endif()
    if(NOT TARGET Qt6::PrintSupport)
        find_package(Qt6 COMPONENTS PrintSupport QUIET)
    endif()
endif()

get_filename_component(LINCUPL_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(LINCUPL_SOURCES
    main.cpp
    tool_paths.h
    tool_paths.cpp
    device_library.h
    device_library.cpp
    error_catalog.h
    error_catalog.cpp
    ../common/cupl_highlighter.h
    ../common/cupl_highlighter.cpp
    code_editor.h
    code_editor.cpp
    compiler_options.h
    compiler_options.cpp
    dialogs.h
    dialogs.cpp
    font_settings.h
    font_settings.cpp
    user_tools.h
    user_tools.cpp
    mainwindow.h
    mainwindow.cpp
    mainwindow_fonts.cpp
    mainwindow_project.cpp
    mainwindow_tools.cpp
    mainwindow_pins.cpp
    selftest.h
    selftest.cpp
)

add_executable(lincupl ${LINCUPL_SOURCES})
include(../common/app_icon.cmake)
lincupl_app_icon(lincupl lincupl)
lincupl_action_icons(lincupl)
include(../common/qt_macos_accessibility.cmake)
lincupl_mac_accessibility(lincupl)
include(../common/help.cmake)
lincupl_help_window(lincupl)

set_target_properties(lincupl PROPERTIES
    AUTOMOC ON
    MACOSX_BUNDLE FALSE
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(lincupl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(lincupl PRIVATE
    LINCUPL_SOURCE_ROOT="${LINCUPL_REPO_ROOT}"
    LINCUPL_VERSION="${LINCUPL_VERSION}"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(lincupl PRIVATE -Wall -Wextra)
endif()

target_link_libraries(lincupl PRIVATE Qt6::Widgets)

# The decoded view of the .abs file (src/shared/absdump.c) comes from the
# shared library of the command-line programs; built on its own, the GUI
# shows the .abs file as hex only.
if(TARGET lincupl_shared)
    target_link_libraries(lincupl PRIVATE lincupl_shared)
    target_compile_definitions(lincupl PRIVATE LINCUPL_HAS_ABSDUMP=1)
endif()

if(TARGET Qt6::PrintSupport)
    target_compile_definitions(lincupl PRIVATE LINCUPL_HAS_PRINTSUPPORT=1)
    target_link_libraries(lincupl PRIVATE Qt6::PrintSupport)
endif()
