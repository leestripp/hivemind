cmake_minimum_required(VERSION 3.28)

# ============================================================================
# HiveMind 2.0 — superbuild
#   hive-server/  C++23 + OmniNet server (API, agents, graph runner, scheduler)
#   hive-client/  GTK4/gtkmm4 native Linux client
#   tests/        server test suite
# Deps (first-party, fetched): OmniNet + daedalus (gitlab.com/leestripp/*)
# ============================================================================

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
message(STATUS "HiveMind version: ${GIT_VERSION}")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unused-command-line-argument)
endif()
set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local")

find_package(CURL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLite3 REQUIRED sqlite3)

include(FetchContent)

# ============================================================================
# daedalus — Agent engine (gitlab.com/leestripp/daedalus)
#   -DHIVEMIND_USE_DAEDALUS=OFF         skip daedalus entirely
#   -DDAEDALUS_TAG=0.9.6               pin a different tag / branch / SHA
#   -DDAEDALUS_LOCAL=/path/to/daedalus use a local checkout instead of fetching
# Required system packages: libssl-dev libpng-dev libpoppler-cpp-dev
# ============================================================================
option(HIVEMIND_USE_DAEDALUS "Use daedalus as the Agent engine" ON)

if(HIVEMIND_USE_DAEDALUS)
    find_package(OpenSSL REQUIRED)
    pkg_check_modules(PNG     REQUIRED libpng)
    pkg_check_modules(POPPLER REQUIRED poppler-cpp)

    set(DAEDALUS_TAG "0.9.6" CACHE STRING "Git tag / branch / SHA for daedalus")

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

    get_target_property(_daedalus_inc daedalus INCLUDE_DIRECTORIES)
    if(NOT _daedalus_inc)
        message(FATAL_ERROR
            "[hivemind] daedalus target has no include directories. "
            "Check that leestripp/daedalus exports a `daedalus` CMake target.")
    endif()
endif()

# ============================================================================
# OmniNet — HTTP/WebSocket server (gitlab.com/leestripp/omninet)
#   -DOMNINET_LOCAL=/path/to/omninet use a local checkout instead of fetching
# ============================================================================
FetchContent_Declare(nlohmann_json FIND_PACKAGE_ARGS)
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
# Subprojects
# ============================================================================
option(HIVEMIND_BUILD_SERVER "Build the hive-server daemon" ON)
if(HIVEMIND_BUILD_SERVER)
    add_subdirectory(hive-server)
    add_subdirectory(hive-worker)
endif()

option(HIVEMIND_BUILD_CLIENT "Build the GTK4/gtkmm4 native client" ON)
if(HIVEMIND_BUILD_CLIENT)
    pkg_check_modules(GTKMM gtkmm-4.0)
    if(GTKMM_FOUND)
        add_subdirectory(hive-client)
    else()
        message(WARNING
            "gtkmm-4.0 not found — skipping hive-client. "
            "Ubuntu 26.04: sudo apt install libgtkmm-4.0-dev "
            "(or configure with -DHIVEMIND_BUILD_CLIENT=OFF to silence this)")
    endif()
endif()

# Tests cover the server side only — skip in client-only builds
# (e.g. the Flatpak manifest configures with HIVEMIND_BUILD_SERVER=OFF).
if(HIVEMIND_BUILD_SERVER AND EXISTS "${CMAKE_SOURCE_DIR}/tests")
    add_subdirectory(tests)
endif()

# ============================================================================
# Clang-Tidy — requires compile_commands.json
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
#   cmake --build build --target lint | lint-fix
# ============================================================================
file(GLOB_RECURSE _LINT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/hive-server/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/hive-server/include/hivemind/*.cpp"
    "${CMAKE_SOURCE_DIR}/hive-client/src/*.cpp"
)

find_program(_CLANG_TIDY clang-tidy)
if(_CLANG_TIDY)
    add_custom_target(lint
        COMMAND clang-tidy --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
            -p ${CMAKE_BINARY_DIR} --export-fixes=lint-results.yaml
            ${_LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy (dry run)..." VERBATIM)
    add_custom_target(lint-fix
        COMMAND clang-tidy --config-file=${CMAKE_SOURCE_DIR}/.clang-tidy
            -p ${CMAKE_BINARY_DIR} --fix --fix-errors
            ${_LINT_SOURCES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy with auto-fix..." VERBATIM)
else()
    message(WARNING "clang-tidy not found — install it with: apt install clang-tidy")
endif()
