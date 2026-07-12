# Stub implementations for iOS (no ncurses/termcap available)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_library(termcap_stubs STATIC termcap_stubs.c)
    target_include_directories(termcap_stubs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(termcap_stubs PROPERTIES
        EXCLUDE_FROM_ALL TRUE
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
    )
endif()
