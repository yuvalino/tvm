cmake_minimum_required(VERSION 3.16)
project(tvm C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
# iOS: the app in flutter/build/ios/iphoneos is also zipped into an IPA
set(FLUTTER_IPA "")
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(IPA_STAGE ${CMAKE_CURRENT_BINARY_DIR}/ipa)
    set(FLUTTER_IPA
        COMMAND ${CMAKE_COMMAND} -E remove_directory ${IPA_STAGE}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/flutter/build/ios/iphoneos/Runner.app
            ${IPA_STAGE}/Payload/Runner.app
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
        COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tvm.ipa
        COMMAND ${CMAKE_COMMAND} -E chdir ${IPA_STAGE}
            zip -qry ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tvm.ipa Payload)
endif()
add_custom_target(flutter
    COMMAND ${FLUTTER_ENV} sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_flutter.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/flutter
    ${FLUTTER_IPA}
    DEPENDS tvm_shared dropbear_shared sftpserver_shared bash_shared toybox_shared vim_shared curl_shared less_shared nano_shared sqlite_shared git_shared git-remote-http_shared git-http-fetch_shared initrd_flutter
    COMMENT "Building Flutter app"
)
set_target_properties(flutter PROPERTIES EXCLUDE_FROM_ALL TRUE)
