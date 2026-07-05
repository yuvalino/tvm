# libtvm.a — core TVM library (no main)
add_library(tvm_static STATIC
    rmem.c
    ulinker.c
    tvm.c
)
set_target_properties(tvm_static PROPERTIES OUTPUT_NAME tvm)
target_include_directories(tvm_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(tvm_static PRIVATE -g -fPIC)
target_link_libraries(tvm_static PRIVATE unicorn)

# tvm executable
add_executable(tvm_exe main.c)
set_target_properties(tvm_exe PROPERTIES OUTPUT_NAME tvm)
target_link_libraries(tvm_exe PRIVATE tvm_static)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(tvm_exe PRIVATE unicorn z pthread util m ${CMAKE_DL_LIBS} crypt ncurses)
    target_link_options(tvm_exe PRIVATE -rdynamic -Wl,--no-as-needed -pie)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_link_libraries(tvm_exe PRIVATE unicorn z pthread m ncurses)
endif()

# libtvm.dylib — TVM runtime for Flutter app (macOS only)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(INITRD_CPIO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/../initrd.cpio")

    add_library(tvm_shared SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/../flutter/native/devtool_main_macos.c
    )
    set_target_properties(tvm_shared PROPERTIES
        OUTPUT_NAME tvm
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/.."
        INSTALL_NAME_DIR "@rpath"
        C_VISIBILITY_PRESET default
    )
    target_include_directories(tvm_shared PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../flutter/native
    )
    target_link_libraries(tvm_shared PRIVATE tvm_static unicorn z pthread m ncurses)
    target_link_options(tvm_shared PRIVATE
        -Wl,-sectcreate,__DATA,__initrd,${INITRD_CPIO}
    )
    set_target_properties(tvm_shared PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# tests
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
