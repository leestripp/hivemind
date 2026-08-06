cmake_minimum_required(VERSION 3.28)

# Add multiarch cmake paths so OmniNet's find_package(OpenSSL) finds OpenSSLConfig.cmake
# on Ubuntu/Debian where it lives in /usr/lib/x86_64-linux-gnu/cmake/
list(PREPEND CMAKE_PREFIX_PATH /usr/lib/x86_64-linux-gnu/cmake /usr/lib/cmake)

project(hivemind LANGUAGES CXX C)

# Automatic version from git tags
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT GIT_VERSION)
    set(GIT_VERSION "v0.0.0")
endif()

string(REGEX MATCHALL "[0-9]+" VERSION_PARTS "${GIT_VERSION}")
list(LENGTH VERSION_PARTS PARTS_LEN)
if(PARTS_LEN GREATER_EQUAL 3)
    list(GET VERSION_PARTS 0 VERSION_MAJOR)
    list(GET VERSION_PARTS 1 VERSION_MINOR)
    list(GET VERSION_PARTS 2 VERSION_PATCH)
    string(REGEX MATCH "^[vV]?[0-9]+\\.[0-9]+\\.[0-9]+" VERSION_TAG "${GIT_VERSION}")
    string(LENGTH "${VERSION_TAG}" TAG_LEN)
    string(SUBSTRING "${GIT_VERSION}" ${TAG_LEN} -1 SUFFIX)
    if(SUFFIX AND NOT SUFFIX STREQUAL "")
        set(VERSION_STRING "${VERSION_TAG}${SUFFIX}")
    else()
        set(VERSION_STRING "${VERSION_TAG}")
    endif()
else()
    set(VERSION_MAJOR 0)
    set(VERSION_MINOR 0)
    set(VERSION_PATCH 0)
    set(VERSION_STRING "${GIT_VERSION}")
endif()

message(STATUS "Parsed version: ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
message(STATUS "Version string: ${VERSION_STRING}")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# ── Clang support ─────────────────────────────────────────────────────────────
# To build with clang instead of GCC, use either:
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/clang.cmake
# Or:
#   CC=clang CXX=clang++ cmake -S . -B build
#
# The CMAKE_BUILD_TYPE values work with clang:
#   Release  = -O3 -DNDEBUG   (fastest builds)
#   Debug    = -g -O0         (debugger-friendly)
#   RelWithDebInfo = -O2 -g   (optimised + debug info)
#
# To run clang-tidy over the project (requires compile_commands.json):
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
#       -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#   cmake --build build --target lint
# Or directly:
#   clang-tidy src/*.cpp include/hivemind/**/*.cpp
#
# To auto-fix:
#   cmake --build build --target lint-fix

set(_CLANG_TIDY_FLAG
    $<$<BOOL:$<TARGET_PROPERTY:hivemind,INTERFACE_INCLUDE_DIRECTORIES>>:
        -config-file=${CMAKE_SOURCE_DIR}/.clang-tidy>
)

# Per-target clang-tidy is set in the target properties below so that
# only our first-party sources are checked (not _deps).
# Set CMAKE_CXX_CLANG_TIDY globally here for any test targets that opt in.
if(NOT DEFINED CMAKE_CXX_CLANG_TIDY AND CMAKE_CXX_CLANG_TIDY)
  message(STATUS "clang-tidy: ${CMAKE_CXX_CLANG_TIDY}")
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Suppress third-party / FetchContent warnings on clang.
# nlohmann/json and OmniNet compile with their own flags; we don't want
# -Wconversion / -Wshadow noise from them.  Done by checking the compiler
# ID after project() has run (can't use generator expressions reliably here).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-command-line-argument)
endif()
set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local")

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLite3 REQUIRED sqlite3)

# FetchContent is needed by both the Daedalus and OmniNet FetchContent blocks
# below — declare it here so the Daedalus block can fetch before the OmniNet
# block runs.
include(FetchContent)

# ============================================================================
# Daedalus — Agent engine (successor to hive-agent)
# ============================================================================
# Daedalus is an autonomous agent library for C++23 (gitlab.com/leestripp/daedalus)
# that exposes a public API as the static library `daedalus`. The library
# replaced our in-tree `hive-agent/` copy of codemax. It fetches its own copy
# of OmniNet internally; we keep our own copy as well.
#
# Required system packages (Ubuntu):
#   sudo apt install -y libssl-dev libpng-dev libpoppler-cpp-dev
#
# CMake options:
#   -DHIVEMIND_USE_DAEDALUS=OFF         skip daedalus entirely
#   -DDAEDALUS_TAG=0.9.6               pin a different tag / branch / SHA
#   -DDAEDALUS_LOCAL=/path/to/daedalus use a local checkout instead of fetching
# ============================================================================
option(HIVEMIND_USE_DAEDALUS  "Use daedalus as the Agent engine (replaces hive-agent)" ON)

if(HIVEMIND_USE_DAEDALUS)
    # Daedalus PUBLIC-links OpenSSL + nlohmann_json and finds libpng / poppler-cpp
    # via pkg-config. Do the finds at the top level so missing system packages
    # fail with a clear error before FetchContent tries to clone anything.
    find_package(OpenSSL REQUIRED)
    pkg_check_modules(PNG     REQUIRED libpng)
    pkg_check_modules(POPPLER REQUIRED poppler-cpp)

    set(DAEDALUS_TAG "0.9.6" CACHE STRING
        "Git tag / branch / SHA for daedalus (default: latest stable 0.9.6)")

    if(DEFINED DAEDALUS_LOCAL AND EXISTS "${DAEDALUS_LOCAL}/CMakeLists.txt")
        message(STATUS "[hivemind] Using local daedalus at ${DAEDALUS_LOCAL}")
        add_subdirectory("${DAEDALUS_LOCAL}" daedalus-build EXCLUDE_FROM_ALL)
    else()
        message(STATUS "[hivemind] Fetching daedalus ${DAEDALUS_TAG} from gitlab.com/leestripp/daedalus")
        FetchContent_Declare(
            daedalus
            GIT_REPOSITORY https://gitlab.com/leestripp/daedalus.git
            GIT_TAG        ${DAEDALUS_TAG}
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(daedalus)
    endif()

    # Sanity check — daedalus must expose its public include directory.
    get_target_property(_daedalus_inc daedalus INCLUDE_DIRECTORIES)
    if(NOT _daedalus_inc)
        message(FATAL_ERROR
            "[hivemind] daedalus target has no include directories. "
            "Check that leestripp/daedalus exports a `daedalus` CMake target.")
    endif()
endif()

# OmniNet via FetchContent (same pattern as codemax)

FetchContent_Declare(
    nlohmann_json
    FIND_PACKAGE_ARGS
)
FetchContent_MakeAvailable(nlohmann_json)

set(OMNINET_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(OMNINET_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(OMNINET_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(OMNINET_INSTALL OFF CACHE BOOL "" FORCE)

if(DEFINED OMNINET_LOCAL AND EXISTS "${OMNINET_LOCAL}/CMakeLists.txt")
    message(STATUS "[hivemind] Using local OmniNet at ${OMNINET_LOCAL}")
    add_subdirectory("${OMNINET_LOCAL}" omninet-build EXCLUDE_FROM_ALL)
else()
    message(STATUS "[hivemind] Fetching OmniNet from gitlab.com/leestripp/omninet")
    FetchContent_Declare(
        OmniNet
        GIT_REPOSITORY https://gitlab.com/leestripp/omninet.git
        GIT_TAG        main
    )
    FetchContent_MakeAvailable(OmniNet)
endif()

get_target_property(_omninet_inc OmniNet INCLUDE_DIRECTORIES)
if(NOT _omninet_inc)
    message(FATAL_ERROR "[hivemind] OmniNet fetched but `OmniNet` target has no include directories.")
endif()

enable_testing()

# ============================================================================
# Source files
# ============================================================================

set(CORE_SOURCES
    src/Graph.cpp
    src/GraphStore.cpp
    src/NodeExecutor.cpp
    src/ExecutorRegistry.cpp
    src/LoopEngine.cpp
    src/GraphRunner.cpp
    src/Settings.cpp
)

set(EXECUTOR_SOURCES
    src/executors/PromptExecutor.cpp
    src/executors/AgentExecutor.cpp
    src/executors/LoopStartExecutor.cpp
    src/executors/LoopEndExecutor.cpp
    src/executors/InputExecutor.cpp
    src/executors/OutputExecutor.cpp
    src/executors/LogExecutor.cpp
    src/executors/MergeExecutor.cpp
)

set(ROUTE_SOURCES
    include/hivemind/routes/GraphRoutes.cpp
    include/hivemind/routes/RunRoutes.cpp
    include/hivemind/routes/StaticRoutes.cpp
    include/hivemind/routes/ModelRoutes.cpp
    include/hivemind/routes/ProjectRoutes.cpp
    include/hivemind/routes/AgentRoutes.cpp
)

set(ALL_SOURCES
    src/main.cpp
    ${CORE_SOURCES}
    ${EXECUTOR_SOURCES}
    ${ROUTE_SOURCES}
)

add_executable(${PROJECT_NAME}
    ${ALL_SOURCES}
)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${SQLite3_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/_deps/nlohmann_json-src/include
)

# Fix library search paths for Ubuntu/Debian multiarch layout
# /usr/lib/x86_64-linux-gnu is where the .so files actually live on this system
link_directories(/usr/lib/x86_64-linux-gnu)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        OmniNet
        CURL::libcurl
        nlohmann_json::nlohmann_json
        ${SQLite3_LIBRARIES}
        pthread
)

if(HIVEMIND_USE_DAEDALUS)
    target_link_libraries(${PROJECT_NAME} PRIVATE daedalus)
endif()

# ============================================================================
# Tests
# ============================================================================
if(EXISTS "${CMAKE_SOURCE_DIR}/tests")
    add_subdirectory(tests)
endif()

# ============================================================================
# Install
# ============================================================================
install(TARGETS ${PROJECT_NAME} DESTINATION "bin")

# ============================================================================
# Clang-Tidy — requires compile_commands.json
# ============================================================================
# Build with: cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug \
#                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
# Then run:   cmake --build build --target lint

# List of all first-party source files (excluding _deps)
set(_LINT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/Graph.cpp
    ${CMAKE_SOURCE_DIR}/src/GraphStore.cpp
    ${CMAKE_SOURCE_DIR}/src/NodeExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/ExecutorRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/LoopEngine.cpp
    ${CMAKE_SOURCE_DIR}/src/GraphRunner.cpp
    ${CMAKE_SOURCE_DIR}/src/Settings.cpp
    ${CMAKE_SOURCE_DIR}/src/executors/PromptExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/executors/AgentExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/executors/LoopStartExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/executors/LoopEndExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/executors/InputExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/executors/OutputExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/executors/LogExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/executors/MergeExecutor.cpp
    ${CMAKE_SOURCE_DIR}/include/hivemind/routes/GraphRoutes.cpp
    ${CMAKE_SOURCE_DIR}/include/hivemind/routes/RunRoutes.cpp
    ${CMAKE_SOURCE_DIR}/include/hivemind/routes/StaticRoutes.cpp
    ${CMAKE_SOURCE_DIR}/include/hivemind/routes/ModelRoutes.cpp
    ${CMAKE_SOURCE_DIR}/include/hivemind/routes/ProjectRoutes.cpp
    ${CMAKE_SOURCE_DIR}/include/hivemind/routes/AgentRoutes.cpp
)

# Verify compile_commands.json exists before offering the lint target
find_program(_CLANG_TIDY clang-tidy)
if(_CLANG_TIDY)
    message(STATUS "clang-tidy: found — 'cmake --build build --target lint' is available")
else()
    message(WARNING "clang-tidy not found — install it with:  apt install clang-tidy")
endif()

# ── lint ────────────────────────────────────────────────────────────────────
# Run clang-tidy in dry-run mode.
# Usage: cmake --build build --target lint
add_custom_target(lint
    COMMAND clang-tidy
        --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
        -p ${CMAKE_BINARY_DIR}
        --export-fixes=lint-results.yaml
        ${_LINT_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-tidy (dry run)..."
    VERBATIM
)

# ── lint-fix ────────────────────────────────────────────────────────────────
# Apply auto-fixes. Review changes with:  git diff
add_custom_target(lint-fix
    COMMAND clang-tidy
        --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
        -p ${CMAKE_BINARY_DIR}
        --fix
        --fix-errors
        ${_LINT_SOURCES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-tidy with auto-fix..."
    VERBATIM
)
