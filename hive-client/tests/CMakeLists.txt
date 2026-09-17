cmake_minimum_required(VERSION 3.28)

# hive-client unit tests — convention: one binary per area, assert()-based,
# -UNDEBUG so assertions stay live in Release builds (same as hive-agent,
# hive-server and hive-worker tests).
#
# Test targets link hive_client_core (../CMakeLists.txt) — pure helpers
# factored out of HiveWindow/ApiClient. GUI behaviour is covered through
# the UiApi hooks (127.0.0.1:8481) instead of widget-level tests.

macro(hive_client_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE hive_client_core)
    target_compile_options(${name} PRIVATE -UNDEBUG)
    add_test(NAME ${name} COMMAND ${name})
endmacro()

hive_client_test(test_text_utils)

# Upload wire-format regression (2026-09-17 locale/Content-Length bug) —
# needs ApiClient + an in-process OmniNet server, so not the pure-helper
# macro.
add_executable(test_upload_wire_format test_upload_wire_format.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/../src/ApiClient.cpp)
target_include_directories(test_upload_wire_format PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_upload_wire_format PRIVATE
    hive_client_core OmniNet nlohmann_json::nlohmann_json)
target_compile_options(test_upload_wire_format PRIVATE -UNDEBUG)
add_test(NAME test_upload_wire_format COMMAND test_upload_wire_format)
