# libtvm.a — core TVM library (no main)
add_library(tvm_static STATIC
    rmem.c
    ulinker.c
    tvm.c
)
set_target_properties(tvm_static PROPERTIES OUTPUT_NAME tvm)
target_include_directories(tvm_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(tvm_static PRIVATE -g -fPIC)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_compile_options(tvm_static PRIVATE -Wno-unavailable-declarations)
endif()
target_link_libraries(tvm_static PRIVATE unicorn)

# tvm executable (not for iOS)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_executable(tvm_exe main.c)
    set_target_properties(tvm_exe PROPERTIES OUTPUT_NAME tvm)
    target_link_libraries(tvm_exe PRIVATE tvm_static)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(tvm_exe PRIVATE unicorn z pthread util m ${CMAKE_DL_LIBS} crypt ncurses)
        target_link_options(tvm_exe PRIVATE -rdynamic -Wl,--no-as-needed -pie)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        target_link_libraries(tvm_exe PRIVATE unicorn z pthread m ncurses)
    endif()
endif()

# libtvm.dylib — TVM runtime for Flutter app (Apple platforms)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_library(tvm_shared SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/../flutter/native/tvm_main_apple.c
    )
    set_target_properties(tvm_shared PROPERTIES
        OUTPUT_NAME tvm
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
        INSTALL_NAME_DIR "@rpath"
        C_VISIBILITY_PRESET default
    )
    target_include_directories(tvm_shared PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../flutter/native
    )
    set(TVM_SHARED_LIBS tvm_static unicorn z pthread m)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        list(APPEND TVM_SHARED_LIBS ncurses)
    endif()
    target_link_libraries(tvm_shared PRIVATE ${TVM_SHARED_LIBS})
    set_target_properties(tvm_shared PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# tests (not for iOS)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
add_executable(test_tvm tests/test_tvm.c)
target_compile_options(test_tvm PRIVATE -g)
target_link_libraries(test_tvm PRIVATE pthread)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(test_tvm PRIVATE util)
endif()
set_target_properties(test_tvm PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_executable(test_rmem rmem.c tests/test_rmem.c)
target_include_directories(test_rmem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_rmem PRIVATE -g)
target_link_libraries(test_rmem PRIVATE pthread)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(test_rmem PRIVATE -rdynamic)
endif()
set_target_properties(test_rmem PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_custom_target(tests DEPENDS test_tvm test_rmem)
endif() # NOT iOS
