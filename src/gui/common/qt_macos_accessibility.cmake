# Shared by the unified build and both standalone GUI builds.
if(APPLE AND NOT CMAKE_OBJCXX_COMPILER_LOADED)
    enable_language(OBJCXX)
endif()
set(LINCUPL_ACCESSIBILITY_DIR "${CMAKE_CURRENT_LIST_DIR}")
function(lincupl_mac_accessibility target)
    if(APPLE)
        target_sources(${target} PRIVATE
            "${LINCUPL_ACCESSIBILITY_DIR}/qt_macos_accessibility.mm")
        set_source_files_properties(
            "${LINCUPL_ACCESSIBILITY_DIR}/qt_macos_accessibility.mm"
            PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
        set_target_properties(${target} PROPERTIES
            OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED ON)
        target_link_libraries(${target} PRIVATE "-framework AppKit")
    endif()
endfunction()
