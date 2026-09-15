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
