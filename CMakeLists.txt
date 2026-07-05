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
    DEPENDS dropbear mksh toybox vim
    COMMENT "Assembling initrd.cpio"
)
set_target_properties(initrd PROPERTIES EXCLUDE_FROM_ALL TRUE)

# flutter app (calls build_flutter.sh after cmake targets are built)
add_custom_target(flutter
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_flutter.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/flutter
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/..
    DEPENDS tvm_objects initrd
    COMMENT "Building Flutter app"
)
set_target_properties(flutter PROPERTIES EXCLUDE_FROM_ALL TRUE)
