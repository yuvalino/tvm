cmake_minimum_required(VERSION 3.16)
project(tvm C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(third_party)
add_subdirectory(tvm)
add_subdirectory(examples)
