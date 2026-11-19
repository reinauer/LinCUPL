cmake_minimum_required(VERSION 3.16)

project(lincupl
    VERSION 6.0
    DESCRIPTION "Native Linux/macOS CUPL compiler, simulator and ATF150x fitters"
    LANGUAGES C
)

include(GNUInstallDirs)

# Default to Release for single-config generators when unspecified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

option(LINCUPL_BUILD_GUI "Build Qt6 GUI applications" ON)
option(LINCUPL_USE_QTHELP "Show the GUI help with Qt Help (QtHelp module and qhelpgenerator) when both are found; otherwise a plain page viewer is used" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(UNIX AND NOT APPLE)
    add_compile_definitions(_DEFAULT_SOURCE)
endif()

# Shared headers (cupl_phases.h, banner.h) for every CLI tool
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/include")
# Version shown in every banner and file header (include/banner.h)
set(LINCUPL_VERSION "${PROJECT_VERSION}")
add_compile_definitions(LINCUPL_VERSION="${LINCUPL_VERSION}")
# Installed data directory (device libraries); searched for the default
# library and named in the "no library" message (src/shared/locate.c)
add_compile_definitions(LINCUPL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/lincupl")

# Place all CLI and GUI executables together in ${CMAKE_BINARY_DIR}/bin
# so GUI tools immediately discover sibling CLI binaries and ../share/lincupl.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# ------------------------------------------------------------------------------
# CLI Phase Library & Tools
# ------------------------------------------------------------------------------
# Compiler phases in pipeline order (cuplk drives cuplx..cuplc), then the
# simulator (csimk drives csima or vsima, which share the sim_* sources).
set(LINCUPL_PHASE_SOURCES
    src/cupl/driver.c
    src/cupl/preprocess.c
    src/cupl/parse.c
    src/cupl/parse_tokens.c
    src/cupl/parse_symtab.c
    src/cupl/parse_lex.c
    src/cupl/parse_grammar.c
    src/cupl/parse_actions.c
    src/cupl/parse_lower.c
    src/cupl/parse_sequence.c
    src/cupl/parse_output.c
    src/cupl/expand.c
    src/cupl/expand_input.c
    src/cupl/expand_device.c
    src/cupl/expand_tree.c
    src/cupl/expand_frame.c
    src/cupl/minimize.c
    src/cupl/minimize_io.c
    src/cupl/minimize_cube.c
    src/cupl/minimize_cover.c
    src/cupl/minimize_levels.c
    src/cupl/minimize_espresso.c
    src/cupl/fusemap.c
    src/cupl/abs.c
    src/cupl/atv5000.c
    src/cupl/prom.c
    src/cupl/doc.c
    src/cupl/jedec.c
    src/cupl/netlist.c
    src/cupl/pla.c
    src/sim/driver.c
    src/sim/sim.c
    src/sim/load.c
    src/sim/jedload.c
    src/sim/parse.c
    src/sim/core.c
    src/sim/bind.c
    src/sim/eval.c
    src/sim/out.c
    src/sim/jedec.c
)

# Berkeley Espresso 2.5.1 for minimize level 4 (-m4), see
# third_party/espresso/README.lincupl.md.  lincupl_embed.c includes the
# unchanged upstream sources as one translation unit; it is third-party code
# and is not built with the warning flags of src/.
set(LINCUPL_ESPRESSO_SOURCES
    third_party/espresso/lincupl_embed.c
)

add_library(cupl_phases STATIC ${LINCUPL_PHASE_SOURCES} ${LINCUPL_ESPRESSO_SOURCES})

target_include_directories(cupl_phases PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set_source_files_properties(${LINCUPL_PHASE_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter;-fno-strict-aliasing"
    )
    # -fwrapv: the set macros of Espresso shift a signed 1 into the sign bit
    set_source_files_properties(${LINCUPL_ESPRESSO_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-fno-strict-aliasing;-fwrapv"
    )
endif()

set(_cli_warn_flags "")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    set(_cli_warn_flags -Wall -Wextra -Wno-unused-parameter)
endif()

# Code used by several programs (src/shared): file name and data file
# lookup, string helpers, starting an external fitter, JEDEC checksums,
# reading JEDEC fuse maps and device libraries, the files the compiler
# phases hand on, the
# absolute file as text (for the GUI), the runtime of the compiler phases
# and the simulator
set(LINCUPL_SHARED_SOURCES
    src/shared/absdump.c
    src/shared/cfile.c
    src/shared/devlib.c
    src/shared/jedread.c
    src/shared/jedsum.c
    src/shared/locate.c
    src/shared/phase.c
    src/shared/runfit.c
    src/shared/setsort.c
    src/shared/strutil.c
)
add_library(lincupl_shared STATIC ${LINCUPL_SHARED_SOURCES})
target_include_directories(lincupl_shared PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/shared")
target_compile_options(lincupl_shared PRIVATE ${_cli_warn_flags})
target_link_libraries(cupl_phases PUBLIC lincupl_shared)

add_executable(cupl src/cupl/main.c)
target_link_libraries(cupl PRIVATE cupl_phases)
target_compile_options(cupl PRIVATE ${_cli_warn_flags})

add_executable(csim src/sim/main.c)
target_link_libraries(csim PRIVATE cupl_phases)
target_compile_options(csim PRIVATE ${_cli_warn_flags})

add_executable(cbld src/tools/cbld.c)
target_link_libraries(cbld PRIVATE lincupl_shared)
target_compile_options(cbld PRIVATE ${_cli_warn_flags})

add_executable(backpin src/tools/backpin.c)
target_link_libraries(backpin PRIVATE lincupl_shared)
target_compile_options(backpin PRIVATE ${_cli_warn_flags})

add_executable(cfitr src/tools/cfitr.c)
target_link_libraries(cfitr PRIVATE lincupl_shared)
target_compile_options(cfitr PRIVATE ${_cli_warn_flags})

# ------------------------------------------------------------------------------
# ATF1502 / ATF1504 / ATF1508 fitter
# ------------------------------------------------------------------------------
# One program for the three devices; the device is taken from the name it is
# run under.  cupl starts it for these devices when it sits next to cupl.
file(GLOB LINCUPL_FIT150X_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/fit150x/*.c")
add_library(fit150x_objects OBJECT ${LINCUPL_FIT150X_SOURCES})
target_include_directories(fit150x_objects PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/fit150x"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_options(fit150x_objects PRIVATE ${_cli_warn_flags})
target_link_libraries(fit150x_objects PRIVATE lincupl_shared)
set(LINCUPL_FIT150X_PROGRAMS fit1502 fit1504 fit1508)
foreach(_fit IN LISTS LINCUPL_FIT150X_PROGRAMS)
    add_executable(${_fit} $<TARGET_OBJECTS:fit150x_objects>)
    target_link_libraries(${_fit} PRIVATE lincupl_shared m)
endforeach()

# ------------------------------------------------------------------------------
# Qt6 GUI Discovery & Conditional Targets (lincupl, linsim)
# ------------------------------------------------------------------------------
if(LINCUPL_BUILD_GUI)
    include(CheckLanguage)
    check_language(CXX)
    if(CMAKE_CXX_COMPILER)
        enable_language(CXX)
        set(CMAKE_CXX_STANDARD 17)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)

        if(APPLE)
            enable_language(OBJCXX)
            set(_lincupl_prefix_search "${CMAKE_PREFIX_PATH};$ENV{CMAKE_PREFIX_PATH}")
            if(NOT Qt6_DIR AND NOT _lincupl_prefix_search MATCHES "([Qq]t)")
                foreach(_qt_prefix IN ITEMS "/opt/homebrew/opt/qt" "/usr/local/opt/qt")
                    if(EXISTS "${_qt_prefix}" AND NOT "${_qt_prefix}" IN_LIST CMAKE_PREFIX_PATH)
                        list(APPEND CMAKE_PREFIX_PATH "${_qt_prefix}")
                    endif()
                endforeach()
            endif()
        endif()

        find_package(Qt6 COMPONENTS Widgets PrintSupport QUIET)
        if(NOT Qt6Widgets_FOUND)
            find_package(Qt6 COMPONENTS Widgets QUIET)
        endif()

        if(Qt6Widgets_FOUND)
            message(STATUS "Found Qt6Widgets (${Qt6Widgets_VERSION}) - GUI targets enabled")

            # Qt Help is packaged separately on most Linux distributions, so
            # the help window falls back to a plain page viewer without it.
            set(LINCUPL_HAVE_QTHELP FALSE)
            if(LINCUPL_USE_QTHELP)
                find_package(Qt6 COMPONENTS Help QUIET)
                if(NOT TARGET Qt6::qhelpgenerator)
                    find_package(Qt6 COMPONENTS ToolsTools QUIET)
                endif()
                if(TARGET Qt6::qhelpgenerator)
                    set(LINCUPL_QHELPGENERATOR "$<TARGET_FILE:Qt6::qhelpgenerator>")
                else()
                    find_program(LINCUPL_QHELPGENERATOR_EXE qhelpgenerator
                        HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBEXECS}"
                              "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
                    set(LINCUPL_QHELPGENERATOR "${LINCUPL_QHELPGENERATOR_EXE}")
                endif()
                if(TARGET Qt6::Help AND LINCUPL_QHELPGENERATOR)
                    set(LINCUPL_HAVE_QTHELP TRUE)
                endif()
            endif()
            if(LINCUPL_HAVE_QTHELP)
                message(STATUS "Found Qt Help and qhelpgenerator - help window uses Qt Help")
            else()
                message(STATUS "Qt Help or qhelpgenerator not used - help window shows the plain pages")
            endif()
        else()
            message(STATUS "Qt6Widgets not found - skipping Qt6 GUI targets (CLI tools will still build)")
        endif()
    else()
        message(STATUS "No C++ compiler found - skipping Qt6 GUI targets")
    endif()
endif()

if(LINCUPL_BUILD_GUI AND Qt6Widgets_FOUND
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/lincupl/CMakeLists.txt"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/lincupl/main.cpp")
    add_subdirectory(src/gui/lincupl)
    if(TARGET lincupl)
        set_target_properties(lincupl PROPERTIES
            MACOSX_BUNDLE FALSE
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
    endif()
endif()

if(LINCUPL_BUILD_GUI AND Qt6Widgets_FOUND
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/linsim/CMakeLists.txt"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/gui/linsim/main.cpp")
    add_subdirectory(src/gui/linsim)
    if(TARGET linsim)
        set_target_properties(linsim PROPERTIES
            MACOSX_BUNDLE FALSE
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
    endif()
endif()

# ------------------------------------------------------------------------------
# Data Files Staging (${CMAKE_BINARY_DIR}/share/lincupl/) & Installation
# ------------------------------------------------------------------------------
set(LINCUPL_SOURCE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/share/lincupl")
set(LINCUPL_BUILD_SHARE_DIR "${CMAKE_BINARY_DIR}/share/lincupl")

if(NOT EXISTS "${LINCUPL_SOURCE_SHARE_DIR}/Atmel.dl")
    message(STATUS "WinCUPL device libraries (Atmel.dl) not found in ${LINCUPL_SOURCE_SHARE_DIR} — run ./wincupl-extract-data before using device-dependent tools.")
endif()

# Stage share/lincupl (including Macro/, Examples/Atmel/, and Fitters/) when extracted
file(MAKE_DIRECTORY "${LINCUPL_BUILD_SHARE_DIR}")
if(EXISTS "${LINCUPL_SOURCE_SHARE_DIR}" AND NOT LINCUPL_SOURCE_SHARE_DIR STREQUAL LINCUPL_BUILD_SHARE_DIR)
    file(COPY "${LINCUPL_SOURCE_SHARE_DIR}/" DESTINATION "${LINCUPL_BUILD_SHARE_DIR}")
endif()

set(_stage_cmds
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LINCUPL_BUILD_SHARE_DIR}"
)
if(EXISTS "${LINCUPL_SOURCE_SHARE_DIR}" AND NOT LINCUPL_SOURCE_SHARE_DIR STREQUAL LINCUPL_BUILD_SHARE_DIR)
    list(APPEND _stage_cmds
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${LINCUPL_SOURCE_SHARE_DIR}" "${LINCUPL_BUILD_SHARE_DIR}"
    )
endif()

add_custom_target(lincupl_data ALL
    ${_stage_cmds}
    COMMENT "Staging WinCUPL device libraries and support data into ${LINCUPL_BUILD_SHARE_DIR}"
    VERBATIM
)

foreach(_tgt IN ITEMS cupl csim cbld backpin cfitr ${LINCUPL_FIT150X_PROGRAMS} lincupl linsim)
    if(TARGET ${_tgt})
        add_dependencies(${_tgt} lincupl_data)
    endif()
endforeach()

# lincupl.dl, the default library of the tools: the devices of Atmel.dl, then
# those that only cupl.dl has, copied by our cbld (cmake/make_lincupl_dl.cmake).
# Atmel.dl and cupl.dl stay as they are, next to it.
set(LINCUPL_MERGED_DL "${LINCUPL_BUILD_SHARE_DIR}/lincupl.dl")
if(EXISTS "${LINCUPL_SOURCE_SHARE_DIR}/Atmel.dl" AND EXISTS "${LINCUPL_SOURCE_SHARE_DIR}/cupl.dl")
    if(CMAKE_CROSSCOMPILING)
        message(STATUS "Cross-compiling: lincupl.dl is not built (it needs cbld to run on the build machine)")
    else()
        add_custom_command(
            OUTPUT "${LINCUPL_MERGED_DL}"
            COMMAND "${CMAKE_COMMAND}" -DCBLD=$<TARGET_FILE:cbld>
                    "-DATMEL=${LINCUPL_SOURCE_SHARE_DIR}/Atmel.dl"
                    "-DCUPL=${LINCUPL_SOURCE_SHARE_DIR}/cupl.dl"
                    "-DOUT=${LINCUPL_MERGED_DL}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/make_lincupl_dl.cmake"
            DEPENDS cbld
                    "${LINCUPL_SOURCE_SHARE_DIR}/Atmel.dl"
                    "${LINCUPL_SOURCE_SHARE_DIR}/cupl.dl"
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/make_lincupl_dl.cmake"
            COMMENT "Building lincupl.dl from Atmel.dl and cupl.dl"
            VERBATIM
        )
        add_custom_target(lincupl_dl ALL DEPENDS "${LINCUPL_MERGED_DL}")
        add_dependencies(lincupl_dl lincupl_data)
        foreach(_tgt IN ITEMS lincupl linsim)
            if(TARGET ${_tgt})
                add_dependencies(${_tgt} lincupl_dl)
            endif()
        endforeach()
        install(FILES "${LINCUPL_MERGED_DL}" DESTINATION "${CMAKE_INSTALL_DATADIR}/lincupl")
    endif()
endif()

# Help pages of the GUI programs (doc/help): staged into
# ${LINCUPL_BUILD_SHARE_DIR}/help, with the Qt Help files made from them
# when Qt Help is used, and installed to share/lincupl/help.
if(TARGET lincupl OR TARGET linsim)
    add_subdirectory(doc/help)
    foreach(_tgt IN ITEMS lincupl linsim)
        if(TARGET ${_tgt})
            add_dependencies(${_tgt} lincupl_help)
        endif()
    endforeach()
endif()

# Install rules
set(_install_targets cupl csim cbld backpin cfitr ${LINCUPL_FIT150X_PROGRAMS})
if(TARGET lincupl)
    list(APPEND _install_targets lincupl)
endif()
if(TARGET linsim)
    list(APPEND _install_targets linsim)
endif()

install(TARGETS ${_install_targets}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if(EXISTS "${LINCUPL_SOURCE_SHARE_DIR}")
    install(DIRECTORY "${LINCUPL_SOURCE_SHARE_DIR}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/lincupl"
    )
endif()
