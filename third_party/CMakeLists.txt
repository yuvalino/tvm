# unicorn (CPU emulator)
set(UNICORN_INTERPRETER ON CACHE BOOL "" FORCE)
set(UNICORN_ARCH "x86;aarch64" CACHE STRING "" FORCE)
set(UNICORN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(unicorn EXCLUDE_FROM_ALL)

set(SCRIPTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../scripts)

# dropbear (SSH)
add_custom_target(dropbear
    COMMAND sh ${SCRIPTS_DIR}/build_dropbear.sh ${CMAKE_CURRENT_SOURCE_DIR}/dropbear
    COMMENT "Building dropbear"
)
set_target_properties(dropbear PROPERTIES EXCLUDE_FROM_ALL TRUE)

# mksh (shell)
add_custom_target(mksh
    COMMAND sh ${SCRIPTS_DIR}/build_mksh.sh ${CMAKE_CURRENT_SOURCE_DIR}/mksh
    COMMENT "Building mksh"
)
set_target_properties(mksh PROPERTIES EXCLUDE_FROM_ALL TRUE)

# toybox (unix utilities)
add_custom_target(toybox
    COMMAND sh ${SCRIPTS_DIR}/build_toybox.sh ${CMAKE_CURRENT_SOURCE_DIR}/toybox
    COMMENT "Building toybox"
)
set_target_properties(toybox PROPERTIES EXCLUDE_FROM_ALL TRUE)

# vim (text editor)
add_custom_target(vim_prog
    COMMAND sh ${SCRIPTS_DIR}/build_vim.sh ${CMAKE_CURRENT_SOURCE_DIR}/vim
    COMMENT "Building vim"
)
set_target_properties(vim_prog PROPERTIES EXCLUDE_FROM_ALL TRUE)
