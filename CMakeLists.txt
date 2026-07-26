cmake_minimum_required(VERSION 3.16)
project(tvm C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(stubs)
add_subdirectory(third_party)
add_subdirectory(tvm)
add_subdirectory(scripts)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    add_subdirectory(examples)
endif()

# flutter app
#
# The iOS app is pinned to a specific Xcode so the toolchain matches what the
# device build was validated against. Fall back to whatever xcode-select points
# at when that exact version is not installed — otherwise every iOS build on a
# machine with a differently-named Xcode dies in xcrun with "missing
# DEVELOPER_DIR path" after the whole tree has already been compiled.
set(FLUTTER_ENV "")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(TVM_XCODE_DIR /Applications/Xcode-26.3.app/Contents/Developer)
    if(NOT IS_DIRECTORY ${TVM_XCODE_DIR})
        execute_process(COMMAND xcode-select -p
            OUTPUT_VARIABLE TVM_XCODE_DIR OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        message(STATUS "flutter: pinned Xcode not found, using ${TVM_XCODE_DIR}")
    endif()
    set(FLUTTER_ENV ${CMAKE_COMMAND} -E env TVM_IOS_CROSS=1
        DEVELOPER_DIR=${TVM_XCODE_DIR})
endif()
add_custom_target(flutter
    COMMAND ${FLUTTER_ENV} sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_flutter.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/flutter
        ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
    DEPENDS tvm_shared dropbear_shared mksh_shared toybox_shared vim_shared initrd_flutter
    COMMENT "Building Flutter app"
)
set_target_properties(flutter PROPERTIES EXCLUDE_FROM_ALL TRUE)
