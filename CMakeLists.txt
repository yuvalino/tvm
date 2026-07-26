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
set(FLUTTER_ENV "")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(FLUTTER_ENV ${CMAKE_COMMAND} -E env TVM_IOS_CROSS=1
        DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer)
endif()
add_custom_target(flutter
    COMMAND ${FLUTTER_ENV} sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_flutter.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/flutter
        ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}
    DEPENDS tvm_shared dropbear_shared mksh_shared toybox_shared vim_shared initrd_flutter
    COMMENT "Building Flutter app"
)
set_target_properties(flutter PROPERTIES EXCLUDE_FROM_ALL TRUE)
