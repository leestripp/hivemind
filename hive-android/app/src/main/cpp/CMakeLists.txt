cmake_minimum_required(VERSION 3.22.1)
project(hive_sphere CXX)

add_library(sphere SHARED sphere_renderer.cpp)

set_target_properties(sphere PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

find_library(lib_gles GLESv2)
find_library(lib_egl EGL)
find_library(lib_log log)

target_link_libraries(sphere PRIVATE ${lib_gles} ${lib_egl} ${lib_log} m)
