add_library(tvm_objects OBJECT
    rmem.c
    ulinker.c
    tvm.c
    main.c
)
target_include_directories(tvm_objects PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(tvm_objects PRIVATE -g)
target_link_libraries(tvm_objects PRIVATE unicorn)

# tvm executable
add_executable(tvm $<TARGET_OBJECTS:tvm_objects>)
target_link_libraries(tvm PRIVATE unicorn z pthread util m ${CMAKE_DL_LIBS} crypt ncurses)
target_link_options(tvm PRIVATE -rdynamic -Wl,--no-as-needed)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(tvm PRIVATE -pie)
endif()

# libtvm.a
add_library(tvm_static STATIC $<TARGET_OBJECTS:tvm_objects>)
set_target_properties(tvm_static PROPERTIES OUTPUT_NAME tvm)
target_link_libraries(tvm_static PRIVATE unicorn)
set_target_properties(tvm_static PROPERTIES EXCLUDE_FROM_ALL TRUE)

# tests
add_executable(test_tvm tests/test_tvm.c)
target_compile_options(test_tvm PRIVATE -g)
target_link_libraries(test_tvm PRIVATE pthread util)
set_target_properties(test_tvm PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_executable(test_rmem rmem.c tests/test_rmem.c)
target_include_directories(test_rmem PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_rmem PRIVATE -g)
target_link_libraries(test_rmem PRIVATE pthread)
target_link_options(test_rmem PRIVATE -rdynamic)
set_target_properties(test_rmem PROPERTIES EXCLUDE_FROM_ALL TRUE)

add_custom_target(tests DEPENDS test_tvm test_rmem)
