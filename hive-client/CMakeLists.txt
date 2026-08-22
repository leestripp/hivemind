# ============================================================================
# hive-client — HiveMind 2.0 native Linux client (GTK4 / gtkmm4)
# Chat-first daily driver: agent roster, threads, approvals, live streaming.
# Target distro: Ubuntu 26.04 LTS (libgtkmm-4.0-dev).
# ============================================================================

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)

add_executable(hive-client
    src/main.cpp
    src/HiveApplication.cpp
    src/HiveWindow.cpp
)

target_include_directories(hive-client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hive-client PRIVATE PkgConfig::GTKMM)

install(TARGETS hive-client DESTINATION "bin")
install(FILES data/hivemind.desktop DESTINATION "share/applications")
