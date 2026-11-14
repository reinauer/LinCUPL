cmake_minimum_required(VERSION 3.16)

include("${CMAKE_CURRENT_LIST_DIR}/../common/lincupl_version.cmake")

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(linsim VERSION ${LINCUPL_VERSION} LANGUAGES CXX)
    include(GNUInstallDirs)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    endif()

    if(APPLE)
        foreach(_qt_prefix IN ITEMS "/opt/homebrew/opt/qt" "/usr/local/opt/qt")
            if(EXISTS "${_qt_prefix}" AND NOT "${_qt_prefix}" IN_LIST CMAKE_PREFIX_PATH)
                list(APPEND CMAKE_PREFIX_PATH "${_qt_prefix}")
            endif()
        endforeach()
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT Qt6Widgets_FOUND)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
endif()
if(NOT Qt6PrintSupport_FOUND)
    find_package(Qt6 COMPONENTS PrintSupport QUIET)
endif()

get_filename_component(_LINSIM_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(LINSIM_SOURCES
    main.cpp
    SimDocument.h
    SimDocument.cpp
    ColorTheme.h
    WaveformView.h
    WaveformView.cpp
    Dialogs.h
    Dialogs.cpp
    MainWindow.h
    MainWindow.cpp
    ../common/cupl_highlighter.h
    ../common/cupl_highlighter.cpp
    SelfTest.h
    SelfTest.cpp
)

add_executable(linsim ${LINSIM_SOURCES})
include(../common/qt_macos_accessibility.cmake)
lincupl_mac_accessibility(linsim)
include(../common/app_icon.cmake)
lincupl_app_icon(linsim linsim)
lincupl_action_icons(linsim)
include(../common/help.cmake)
lincupl_help_window(linsim)

target_include_directories(linsim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_compile_definitions(linsim PRIVATE
    LINSIM_SOURCE_ROOT="${_LINSIM_REPO_ROOT}"
    LINCUPL_VERSION="${LINCUPL_VERSION}"
)

target_link_libraries(linsim PRIVATE Qt6::Widgets)

if(TARGET Qt6::PrintSupport)
    target_link_libraries(linsim PRIVATE Qt6::PrintSupport)
    target_compile_definitions(linsim PRIVATE LINSIM_HAS_PRINTSUPPORT=1)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(linsim PRIVATE -Wall -Wextra)
endif()

set_target_properties(linsim PROPERTIES
    MACOSX_BUNDLE FALSE
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    install(TARGETS linsim RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
