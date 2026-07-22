# Stub implementations for platforms with no ncurses/termcap available
# (iOS, and android — bionic ships no terminfo/termcap library at all).
# TVM intercepts tputs at runtime, so vim needs only these link-time stubs.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR ANDROID)
    add_library(termcap_stubs STATIC termcap_stubs.c)
    target_include_directories(termcap_stubs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(termcap_stubs PROPERTIES
        EXCLUDE_FROM_ALL TRUE
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
    )
endif()
