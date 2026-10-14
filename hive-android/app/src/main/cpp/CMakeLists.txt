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

# ── audio.cpp pocket-TTS (vendored by fetch-deps.sh → audio-cpp/) ────
# Same engine + voices as desktop hive-client-voice (ChimpFlow). Pinned
# to the ChimpFlow commit; CPU-only, pocket_tts family only (custom model
# set skips the other 110+ families' objects). Measured on the edge 30:
# RTF 0.78 (~1.3x faster than real-time), 490MB peak RSS.
# audio.cpp builds OUTSIDE this project (fetch-deps.sh → jniLibs/
# libaudiocpp.so): both whisper.cpp and audio.cpp vendor ggml, and
# add_subdirectory'ing both collides on the ggml target names
# (CMP0002). The prebuilt also pins its own configure flags (C++20,
# pocket_tts-only model set, -llog) independent of AGP's -std=c++17.
add_library(audiocpp SHARED IMPORTED)
set_target_properties(audiocpp PROPERTIES
    IMPORTED_LOCATION
        ${CMAKE_SOURCE_DIR}/../jniLibs/${ANDROID_ABI}/libaudiocpp.so
    INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/audio-cpp/include)

add_library(pocket_tts_jni SHARED pocket_tts_jni.cpp)
set_target_properties(pocket_tts_jni PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
target_link_libraries(pocket_tts_jni PRIVATE audiocpp ${lib_log})
