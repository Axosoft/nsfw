cmake_minimum_required (VERSION 3.8 FATAL_ERROR)
set (PROJECT_NAME "NSFW")
project (${PROJECT_NAME})

message (STATUS "Running CMake version ${CMAKE_VERSION}")

set (NSFW_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/include")

add_subdirectory (src)
