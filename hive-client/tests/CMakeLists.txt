cmake_minimum_required(VERSION 3.28)

# hive-client unit tests — convention: one binary per area, assert()-based,
# -UNDEBUG so assertions stay live in Release builds (same as hive-agent,
# hive-server and hive-worker tests).
#
# The client is a GTK4/gtkmm4 GUI app and currently has no unit tests.
# When a non-GUI unit needs coverage, add a test_<name>.cpp here and:
#
#     hive_client_test(test_<name>)
#
# If the test needs to link client code, first factor that code into a
# hive_client_core static lib in ../CMakeLists.txt (the hive-agent /
# hive-worker pattern) and link it here.

# macro(hive_client_test name)
#     add_executable(${name} ${name}.cpp)
#     target_compile_options(${name} PRIVATE -UNDEBUG)
#     add_test(NAME ${name} COMMAND ${name})
# endmacro()
