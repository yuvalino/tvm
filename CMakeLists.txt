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
# python (macOS / iOS only): its framework, and the standard library that
# scripts/embed_frameworks.sh copies into the bundle
set(FLUTTER_PYTHON_DEPS "")
if(APPLE)
    set(FLUTTER_PYTHON_DEPS python_shared python_stdlib)
endif()
# clang (macOS / iOS only): its framework, and the toolchain bundle that
# scripts/embed_frameworks.sh copies into the bundle
set(FLUTTER_LLVM_DEPS "")
if(APPLE)
    set(FLUTTER_LLVM_DEPS llvm_shared llvm_bundle)
endif()
# Claude Code, when TVM_CLAUDE_CODE names its binary (third_party/CMakeLists.txt)
if(APPLE AND TVM_CLAUDE_CODE)
    list(APPEND FLUTTER_LLVM_DEPS claude_shared)
endif()
# ripgrep, when TVM_RIPGREP names its binary (third_party/CMakeLists.txt)
if(APPLE AND TVM_RIPGREP)
    list(APPEND FLUTTER_LLVM_DEPS rg_shared)
endif()
# opencode, when TVM_OPENCODE names its binary (third_party/CMakeLists.txt)
if(APPLE AND TVM_OPENCODE)
    list(APPEND FLUTTER_LLVM_DEPS opencode_shared)
endif()
# Node.js, when TVM_NODE names a distribution (third_party/CMakeLists.txt)
if(APPLE AND TVM_NODE)
    list(APPEND FLUTTER_LLVM_DEPS node_shared node_modules)
endif()
add_custom_target(flutter
    COMMAND ${FLUTTER_ENV} sh ${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_flutter.sh
        ${CMAKE_CURRENT_SOURCE_DIR}/flutter
    ${FLUTTER_IPA}
    DEPENDS tvm_shared dropbear_shared sftpserver_shared bash_shared toybox_shared vim_shared curl_shared less_shared nano_shared sqlite_shared git_shared git-remote-http_shared git-http-fetch_shared ${FLUTTER_PYTHON_DEPS} ${FLUTTER_LLVM_DEPS} initrd_flutter
    COMMENT "Building Flutter app"
)
set_target_properties(flutter PROPERTIES EXCLUDE_FROM_ALL TRUE)
