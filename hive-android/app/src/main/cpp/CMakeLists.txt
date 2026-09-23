cmake_minimum_required(VERSION 3.22.1)
project(hive_native CXX)

# ── OpenGL sphere renderer (existing) ────────────────────────────────
add_library(sphere SHARED sphere_renderer.cpp)

set_target_properties(sphere PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)

find_library(lib_gles GLESv2)
find_library(lib_egl EGL)
find_library(lib_log log)

target_link_libraries(sphere PRIVATE ${lib_gles} ${lib_egl} ${lib_log} m)

# ── whisper.cpp STT (vendored by fetch-deps.sh → whisper-cpp/) ───────
# Static ggml+whisper, no examples/tests; GGML_NATIVE OFF — its arch
# autodetect targets the build host, not the Android ABI.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
# OMP's spin-wait thread pool thrashes big.LITTLE; whisper's own pool suffices
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
# The 778G+ (Cortex-A78) has ARMv8.2 dotprod — ggml's feature probe fails
# to add it under the NDK, and without it q5_1 matmuls fall back to scalar
# (measured: final decode went from ~4s fp16 to 60s+ scalar q5_1!).
if (ANDROID_ABI STREQUAL "arm64-v8a")
    add_compile_options(-march=armv8.2-a+dotprod)
endif()
# AGP's Debug native build compiles with -O0 — whisper at -O0 is 10-30x
# slower (measured: 17.7s for a 5s tiny partial!). Appended AFTER the
# toolchain's -O0, so it wins.
add_compile_options($<$<CONFIG:Debug>:-O2>)
add_subdirectory(whisper-cpp)

add_library(whisper_jni SHARED whisper_jni.cpp)
set_target_properties(whisper_jni PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
target_compile_options(whisper_jni PRIVATE -O2)
target_link_libraries(whisper_jni PRIVATE whisper ${lib_log})
