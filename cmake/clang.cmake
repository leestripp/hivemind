# =============================================================================
# cmake/clang.cmake — Clang toolchain for HiveMind
#
# Usage:
#   cmake -S . -B build-clang -DCMAKE_TOOLCHAIN_FILE=cmake/clang.cmake
#
# Or set CXX environment variables before cmake:
#   CXX=clang++ CC=clang cmake -S . -B build-clang
# =============================================================================

set(CMAKE_C_COMPILER   clang   CACHE FILEPATH "C compiler")
set(CMAKE_CXX_COMPILER clang++ CACHE FILEPATH "C++ compiler")

# Clang-specific flags that are safe across versions
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}"
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
)

# Less noisy for generated/third-party headers in _deps
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}"
    -Wno-unused-command-line-argument
)

# Position-independent code
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Colour diagnostics (clang emits these automatically on a TTY, but cmake
# suppresses the TTY flag in its own output — force it on)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" -fdiagnostics-color=always)

# Search paths for multiarch systems
set(CMAKE_PREFIX_PATH
    /usr/lib/x86_64-linux-gnu/cmake
    /usr/lib/cmake
    ${CMAKE_PREFIX_PATH}
)
