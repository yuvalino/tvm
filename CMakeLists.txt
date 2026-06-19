cmake_minimum_required(VERSION 3.16)
project(tvm C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(third_party)
add_subdirectory(tvm)
add_subdirectory(examples)

# initrd.cpio — rootfs archive with dropbear, mksh, toybox, vim
add_custom_target(initrd
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mkinitrd.sh
        ${CMAKE_CURRENT_BINARY_DIR}/.rootfs
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/..
        ${CMAKE_CURRENT_SOURCE_DIR}
    DEPENDS dropbear mksh toybox vim_prog
    COMMENT "Assembling initrd.cpio"
)
set_target_properties(initrd PROPERTIES EXCLUDE_FROM_ALL TRUE)
