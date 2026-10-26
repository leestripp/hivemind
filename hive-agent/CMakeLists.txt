# hive-agent — the HiveMind agent brain. ONE self-contained binary: the
# agent engine (merged in-tree 2026-09-07 — no longer an
# external dependency) plus the stdio NDJSON host loop that talks to
# hive-worker (task/delta/message events). No HTTP server.
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PNG REQUIRED libpng)
pkg_check_modules(JPEG REQUIRED libjpeg)
pkg_check_modules(POPPLER REQUIRED poppler-cpp)

file(GLOB_RECURSE HIVE_AGENT_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM HIVE_AGENT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

configure_file(src/core/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/core/version.h @ONLY)

# Engine as a static lib so unit tests link the same objects the binary
# ships (tests live in hive-agent/tests/). Includes/deps are PUBLIC so
# test targets inherit them.
add_library(hive_agent_core STATIC ${HIVE_AGENT_SOURCES})
target_compile_definitions(hive_agent_core PUBLIC GIT_VERSION="${GIT_VERSION}")
target_include_directories(hive_agent_core PUBLIC
    src ${CMAKE_CURRENT_BINARY_DIR} ${SQLite3_INCLUDE_DIRS}
    ${PNG_INCLUDE_DIRS} ${JPEG_INCLUDE_DIRS} ${POPPLER_INCLUDE_DIRS})
target_link_libraries(hive_agent_core PUBLIC
    OmniNet
    nlohmann_json::nlohmann_json
    OpenSSL::SSL OpenSSL::Crypto
    ${SQLite3_LIBRARIES}
    ${PNG_LIBRARIES} ${JPEG_LIBRARIES} ${POPPLER_LIBRARIES})

add_executable(hive-agent src/main.cpp)
target_link_libraries(hive-agent PRIVATE hive_agent_core)
install(TARGETS hive-agent DESTINATION "bin")

add_subdirectory(tests)
