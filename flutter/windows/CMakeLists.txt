cmake_minimum_required(VERSION 3.14)
project(tvm LANGUAGES CXX C)

set(BINARY_NAME "tvm")

cmake_policy(SET CMP0063 NEW)

set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")

# Configure build options based on CMAKE_BUILD_TYPE.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Flutter build mode" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Profile" "Release")
endif()

# TVM native bridge.
set(TVM_ROOT "${CMAKE_SOURCE_DIR}/../tvm")
set(NATIVE_ROOT "${CMAKE_SOURCE_DIR}/../native")

add_library(tvm_bridge SHARED
  ${NATIVE_ROOT}/tvm_bridge.c
)
target_include_directories(tvm_bridge PRIVATE
  ${NATIVE_ROOT}
  ${TVM_ROOT}/include
)
set(TVM_LIB_DIR "${TVM_ROOT}/lib/windows/x64")
target_link_libraries(tvm_bridge
  ${TVM_LIB_DIR}/tvm.lib
  ${TVM_LIB_DIR}/dropbear.lib
  ${TVM_LIB_DIR}/bash.lib
  ${TVM_LIB_DIR}/toybox.lib
  ${TVM_LIB_DIR}/minivim.lib
  ${TVM_LIB_DIR}/unicorn.lib
)

# Define preprocessor definitions for the build.
add_definitions(-D_HAS_EXCEPTIONS=0)
add_definitions(-DUNICODE -D_UNICODE)

# Compilation settings that should be applied to most targets.
function(APPLY_STANDARD_SETTINGS TARGET)
  target_compile_features(${TARGET} PUBLIC cxx_std_17)
  target_compile_options(${TARGET} PRIVATE /W4 /WX /wd"4100")
  target_compile_options(${TARGET} PRIVATE /EHsc)
  target_compile_definitions(${TARGET} PRIVATE "_HAS_EXCEPTIONS=0")
  target_compile_definitions(${TARGET} PRIVATE "$<$<CONFIG:Debug>:_DEBUG>")
endfunction()

set(FLUTTER_MANAGED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/flutter")

add_subdirectory(${FLUTTER_MANAGED_DIR})

add_subdirectory("runner")

# Generated plugin build rules, which manage building the plugins and adding
# them to the application.
include(flutter/generated_plugins.cmake)

# === Installation ===
set(BUILD_BUNDLE_DIR "$<TARGET_FILE_DIR:${BINARY_NAME}>")
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${BUILD_BUNDLE_DIR}" CACHE PATH "..." FORCE)
endif()

set(INSTALL_BUNDLE_DATA_DIR "${CMAKE_INSTALL_PREFIX}/data")
set(INSTALL_BUNDLE_LIB_DIR "${CMAKE_INSTALL_PREFIX}")

install(TARGETS ${BINARY_NAME} RUNTIME DESTINATION "${CMAKE_INSTALL_PREFIX}"
  COMPONENT Runtime)

install(FILES "${FLUTTER_ICU_DATA_FILE}" DESTINATION "${INSTALL_BUNDLE_DATA_DIR}"
  COMPONENT Runtime)

install(FILES "${FLUTTER_LIBRARY}" DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
  COMPONENT Runtime)

if(PLUGIN_BUNDLED_LIBRARIES)
  install(FILES ${PLUGIN_BUNDLED_LIBRARIES} DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
    COMPONENT Runtime)
endif()

install(TARGETS tvm_bridge RUNTIME DESTINATION "${INSTALL_BUNDLE_LIB_DIR}"
  COMPONENT Runtime)

install(DIRECTORY "${PROJECT_BUILD_DIR}/flutter_assets" DESTINATION "${INSTALL_BUNDLE_DATA_DIR}"
  COMPONENT Runtime)

if(FLUTTER_LIBRARY_HEADERS_ONLY)
  install(CODE "
    file(REMOVE \"${INSTALL_BUNDLE_LIB_DIR}/flutter_windows.dll\")
  " COMPONENT Runtime)
endif()
