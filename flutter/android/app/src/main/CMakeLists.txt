cmake_minimum_required(VERSION 3.22)
project(tvm_bridge C)

set(TVM_ROOT "${CMAKE_SOURCE_DIR}/../../../../tvm")
set(NATIVE_ROOT "${CMAKE_SOURCE_DIR}/../../../../native")

add_library(tvm_bridge SHARED
    ${NATIVE_ROOT}/tvm_bridge.c
)

target_include_directories(tvm_bridge PRIVATE
    ${NATIVE_ROOT}
    ${TVM_ROOT}/include
)

# Link pre-built TVM static libraries for the current ABI.
# Place .a files under tvm/lib/android/<abi>/.
set(TVM_LIB_DIR "${TVM_ROOT}/lib/android/${ANDROID_ABI}")

# Uncomment once TVM libraries are built for Android.
# target_link_libraries(tvm_bridge
#     ${TVM_LIB_DIR}/libtvm.a
#     ${TVM_LIB_DIR}/libdropbear.a
#     ${TVM_LIB_DIR}/libmksh.a
#     ${TVM_LIB_DIR}/libtoybox.a
#     ${TVM_LIB_DIR}/libminivim.a
#     ${TVM_LIB_DIR}/libunicorn.a
#     z
#     log
# )
target_link_libraries(tvm_bridge log)
