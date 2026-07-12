cmake_minimum_required(VERSION 3.16)
project(tvm C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(stubs)
add_subdirectory(third_party)
add_subdirectory(tvm)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(examples)

    # initrd.cpio — rootfs archive with real binaries (CLI builds)
    add_custom_target(initrd
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mkinitrd.sh
            ${CMAKE_CURRENT_BINARY_DIR}/.rootfs
            ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/third_party
        DEPENDS dropbear_exe mksh_exe toybox_exe vim_exe
        COMMENT "Assembling initrd.cpio"
    )
    set_target_properties(initrd PROPERTIES EXCLUDE_FROM_ALL TRUE)
endif()

# initrd_flutter — rootfs with launcher stubs instead of real binaries
# On non-iOS, depends on toybox_exe for the applet list.
# On iOS, toybox must be pre-built for the host (it can't run cross-compiled).
set(INITRD_FLUTTER_DEPS dropbear_launcher mksh_launcher toybox_launcher vim_launcher)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    list(APPEND INITRD_FLUTTER_DEPS toybox_exe)
endif()
add_custom_target(initrd_flutter
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mkinitrd_flutter.sh
        ${CMAKE_CURRENT_BINARY_DIR}/.rootfs_flutter
        ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        ${CMAKE_CURRENT_BINARY_DIR}/third_party/toybox/toybox
    DEPENDS ${INITRD_FLUTTER_DEPS}
    COMMENT "Assembling initrd.cpio (flutter)"
)
set_target_properties(initrd_flutter PROPERTIES EXCLUDE_FROM_ALL TRUE)

# flutter app
add_custom_target(flutter
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_flutter.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/flutter
        ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
    DEPENDS tvm_shared dropbear_shared mksh_shared toybox_shared vim_shared initrd_flutter
    COMMENT "Building Flutter app"
)
set_target_properties(flutter PROPERTIES EXCLUDE_FROM_ALL TRUE)
