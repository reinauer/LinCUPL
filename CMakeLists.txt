cmake_minimum_required(VERSION 3.16)

project(lincupl
    VERSION 1.0.0
    DESCRIPTION "Native CUPL compiler, simulator and ATF150x fitters"
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
option(LINCUPL_REQUIRE_TEST_DATA "Require reference device data for comparison tests" OFF)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    add_compile_options(/utf-8)
endif()

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
    src/sim/jed150x.c
    src/sim/truth.c
    src/sim/seq.c
    src/sim/seqlegacy.c
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
    src/fit150x/dev_tables.c
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

add_executable(jedinspect src/tools/jedinspect.c)
target_link_libraries(jedinspect PRIVATE lincupl_shared)
target_compile_options(jedinspect PRIVATE ${_cli_warn_flags})

add_executable(jedcompare src/tools/jedcompare.c src/tools/jedcompare_seq.c)
target_link_libraries(jedcompare PRIVATE cupl_phases)
target_compile_options(jedcompare PRIVATE ${_cli_warn_flags})

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
list(REMOVE_ITEM LINCUPL_FIT150X_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/fit150x/dev_tables.c")
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
    target_link_libraries(${_fit} PRIVATE lincupl_shared)
    if(NOT WIN32)
        target_link_libraries(${_fit} PRIVATE m)
    endif()
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
            WIN32_EXECUTABLE "${WIN32}"
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
            WIN32_EXECUTABLE "${WIN32}"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        )
    endif()
endif()

# ------------------------------------------------------------------------------
# Data Files Staging (${CMAKE_BINARY_DIR}/share/lincupl/) & Installation
# ------------------------------------------------------------------------------
set(LINCUPL_SOURCE_SHARE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/share/lincupl")
set(LINCUPL_BUILD_SHARE_DIR "${CMAKE_BINARY_DIR}/share/lincupl")

# Optional examples, macros and reference libraries are staged separately.
# The default library always comes from the checked-in device descriptions.
file(MAKE_DIRECTORY "${LINCUPL_BUILD_SHARE_DIR}")
add_custom_target(lincupl_data ALL
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE=${LINCUPL_SOURCE_SHARE_DIR}"
            "-DDESTINATION=${LINCUPL_BUILD_SHARE_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_data.cmake"
    COMMENT "Staging optional support data"
    VERBATIM
)

set(LINCUPL_MERGED_DL "${LINCUPL_BUILD_SHARE_DIR}/lincupl.dl")
file(GLOB_RECURSE _device_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/devices/*.dev"
    "${CMAKE_CURRENT_SOURCE_DIR}/devices/manifest.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/devices/*/manifest.json")
file(GLOB _device_compiler CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/devlang/*.py")
add_custom_command(
    OUTPUT "${LINCUPL_MERGED_DL}"
    COMMAND "${CMAKE_COMMAND}" "-DPYTHON=${Python3_EXECUTABLE}"
            "-DCOMPILER=${CMAKE_CURRENT_SOURCE_DIR}/tools/devcompile.py"
            "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/devices"
            "-DOUT=${LINCUPL_MERGED_DL}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compile_devices.cmake"
    DEPENDS ${_device_sources} ${_device_compiler}
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/devcompile.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/compile_devices.cmake"
    COMMENT "Compiling lincupl.dl from device descriptions"
    VERBATIM
)
add_custom_target(lincupl_dl ALL DEPENDS "${LINCUPL_MERGED_DL}")
add_dependencies(lincupl_data lincupl_dl)
foreach(_tgt IN ITEMS cupl csim cbld backpin cfitr jedcompare ${LINCUPL_FIT150X_PROGRAMS} lincupl linsim)
    if(TARGET ${_tgt})
        add_dependencies(${_tgt} lincupl_data)
    endif()
endforeach()
install(FILES "${LINCUPL_MERGED_DL}" DESTINATION "${CMAKE_INSTALL_DATADIR}/lincupl")

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

# ------------------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------------------
# Native regressions use the generated library. Additional catalog
# comparisons use optional reference libraries staged with support data.
enable_testing()
add_executable(devlib_normalize_test tests/devlib_normalize.c)
target_link_libraries(devlib_normalize_test PRIVATE lincupl_shared)
target_compile_options(devlib_normalize_test PRIVATE ${_cli_warn_flags})
add_test(NAME devlib_normalize COMMAND devlib_normalize_test)
add_executable(devlib_read_test tests/devlib_read.c)
target_link_libraries(devlib_read_test PRIVATE lincupl_shared)
target_compile_options(devlib_read_test PRIVATE ${_cli_warn_flags})
if(Python3_EXECUTABLE)
    add_test(NAME devcompile
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devcompile.py")
    add_test(NAME devcatalog
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devcatalog.py")
    add_test(NAME devcatalog_sources
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devcatalog_sources.py")
    add_test(NAME devdefault
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devdefault.py"
            "$<TARGET_FILE_DIR:csim>")
    set_tests_properties(devdefault PROPERTIES TIMEOUT 120)
    add_test(NAME devcatalog_libraries
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devcatalog_libraries.py"
            "$<TARGET_FILE_DIR:csim>")
    add_test(NAME devcompile_libraries
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devcompile_libraries.py"
            "$<TARGET_FILE_DIR:csim>")
    add_test(NAME devpackages
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devpackages.py"
            "$<TARGET_FILE_DIR:csim>")
    add_test(NAME devgal22v10
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devgal22v10.py"
            "$<TARGET_FILE_DIR:csim>")
    add_test(NAME devatf2500
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devatf2500.py"
            "$<TARGET_FILE_DIR:csim>")
    set_tests_properties(devcompile PROPERTIES TIMEOUT 30)
    set_tests_properties(devcatalog PROPERTIES TIMEOUT 30)
    set_tests_properties(devcatalog_sources PROPERTIES TIMEOUT 120)
    add_test(NAME devsequence
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/devsequence.py" "$<TARGET_FILE_DIR:cupl>")
    set_tests_properties(devsequence PROPERTIES TIMEOUT 120)
    set_tests_properties(devcatalog_libraries PROPERTIES TIMEOUT 300)
    set_tests_properties(devcompile_libraries PROPERTIES TIMEOUT 600)
    set_tests_properties(devpackages devgal22v10 devatf2500 PROPERTIES TIMEOUT 600)
    if(NOT LINCUPL_REQUIRE_TEST_DATA)
        set_tests_properties(devcatalog_libraries PROPERTIES SKIP_RETURN_CODE 77)
        set_tests_properties(devcompile_libraries PROPERTIES SKIP_RETURN_CODE 77)
        set_tests_properties(devpackages devgal22v10 devatf2500 PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_executable(runfit_test tests/runfit.c)
    target_link_libraries(runfit_test PRIVATE lincupl_shared)
    add_test(NAME runfit
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/runfit.py"
            "$<TARGET_FILE:runfit_test>")
    set_tests_properties(runfit PROPERTIES TIMEOUT 60)
    add_test(NAME backpin
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/backpin.py"
            "$<TARGET_FILE:backpin>")
    set_tests_properties(backpin PROPERTIES TIMEOUT 60)
    add_test(NAME fit150x_builtin
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fit150x_builtin.py"
            "$<TARGET_FILE_DIR:cupl>")
    set_tests_properties(fit150x_builtin PROPERTIES TIMEOUT 600)
    add_test(NAME fit150x_edif
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fit150x_edif.py"
            "$<TARGET_FILE_DIR:cupl>")
    add_test(NAME fit150x_eqn
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fit150x_eqn.py"
            "$<TARGET_FILE_DIR:cupl>")
    add_test(NAME fit150x_eqn_extended
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fit150x_eqn.py"
            "$<TARGET_FILE_DIR:cupl>" extended)
    set_tests_properties(fit150x_eqn fit150x_eqn_extended PROPERTIES TIMEOUT 120)
    add_test(NAME fit150x_yosys
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fit150x_yosys.py"
            "$<TARGET_FILE_DIR:cupl>")
    add_test(NAME fit150x_edif_extended
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fit150x_edif.py"
            "$<TARGET_FILE_DIR:cupl>" extended)
    add_test(NAME fit150x_yosys_extended
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fit150x_yosys.py"
            "$<TARGET_FILE_DIR:cupl>" extended)
    set_tests_properties(fit150x_edif fit150x_yosys fit150x_edif_extended
        fit150x_yosys_extended PROPERTIES TIMEOUT 300)
    set_tests_properties(fit150x_yosys fit150x_yosys_extended PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME jedinspect
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/jedinspect.py" "$<TARGET_FILE:jedinspect>")
    add_test(NAME jedcompare
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/jedcompare.py" "$<TARGET_FILE_DIR:jedcompare>")
    set_tests_properties(jedinspect jedcompare PROPERTIES TIMEOUT 120)
    add_test(NAME jedcompare_seq
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/jedcompare_seq.py" "$<TARGET_FILE_DIR:jedcompare>")
    set_tests_properties(jedcompare_seq PROPERTIES TIMEOUT 180)
    add_test(NAME jedcompare_generic
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/jedcompare_generic.py" "$<TARGET_FILE_DIR:jedcompare>")
    set_tests_properties(jedcompare_generic PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    add_test(NAME fit1508_cbga132
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fit1508_cbga132.py" "$<TARGET_FILE_DIR:jedcompare>")
    set_tests_properties(fit1508_cbga132 PROPERTIES TIMEOUT 180)
    add_test(NAME csim_150x_be
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/csim_150x_be.py" "$<TARGET_FILE_DIR:jedcompare>")
    set_tests_properties(csim_150x_be PROPERTIES TIMEOUT 180)
    add_test(NAME fit150x_be
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fit150x_be.py" "$<TARGET_FILE_DIR:jedcompare>")
    set_tests_properties(fit150x_be PROPERTIES TIMEOUT 180)
    if(NOT LINCUPL_REQUIRE_TEST_DATA)
        set_tests_properties(jedcompare PROPERTIES SKIP_RETURN_CODE 77)
        set_tests_properties(jedcompare_seq PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    add_test(
        NAME csim_jed
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/csim_jed.py" --bin-dir "$<TARGET_FILE_DIR:csim>"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(NAME csim_atf_jed
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/csim_atf_jed.py"
            "$<TARGET_FILE_DIR:csim>")
    set_tests_properties(csim_atf_jed PROPERTIES TIMEOUT 600)

    add_test(NAME csim_atf750_alias
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/csim_atf750_alias.py"
                "$<TARGET_FILE_DIR:cupl>")
    set_tests_properties(csim_atf750_alias PROPERTIES TIMEOUT 180)
    foreach(_mode IN ITEMS legacy extended)
        add_test(NAME csim_150x_jed_${_mode}
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/csim_150x_jed.py"
                "$<TARGET_FILE_DIR:csim>" ${_mode})
        set_tests_properties(csim_150x_jed_${_mode} PROPERTIES TIMEOUT 300)
    endforeach()
    add_test(NAME atf750c_mapping
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/atf750c_mapping.py"
            "$<TARGET_FILE_DIR:csim>")
    set_tests_properties(atf750c_mapping PROPERTIES TIMEOUT 600)
    if(NOT LINCUPL_REQUIRE_TEST_DATA)
        set_tests_properties(csim_jed csim_atf_jed atf750c_mapping PROPERTIES SKIP_RETURN_CODE 77)
    endif()
    file(GLOB _defect_cases CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/defects/*.json")
    foreach(_case IN LISTS _defect_cases)
        get_filename_component(_name "${_case}" NAME_WE)
        add_test(NAME "defect_${_name}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/defects.py"
                "${_case}" --bin-dir "$<TARGET_FILE_DIR:cupl>")
        set_tests_properties("defect_${_name}" PROPERTIES TIMEOUT 120)
        if(NOT LINCUPL_REQUIRE_TEST_DATA)
            set_tests_properties("defect_${_name}" PROPERTIES SKIP_RETURN_CODE 77)
        endif()
    endforeach()
endif()

# Install rules
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    include(InstallRequiredSystemLibraries)
endif()
set(_install_targets cupl csim cbld backpin cfitr jedinspect jedcompare ${LINCUPL_FIT150X_PROGRAMS})
if(TARGET lincupl)
    list(APPEND _install_targets lincupl)
endif()
if(TARGET linsim)
    list(APPEND _install_targets linsim)
endif()

install(TARGETS ${_install_targets}
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(FILES doc/jed-tools.md doc/device-language.md doc/device-implementation.md
    DESTINATION "${CMAKE_INSTALL_DOCDIR}")

if(EXISTS "${LINCUPL_SOURCE_SHARE_DIR}")
    install(DIRECTORY "${LINCUPL_SOURCE_SHARE_DIR}/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/lincupl"
        PATTERN "lincupl.dl" EXCLUDE
    )
endif()
