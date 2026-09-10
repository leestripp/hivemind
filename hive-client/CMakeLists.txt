# ============================================================================
# hive-client — HiveMind 2.0 native Linux client (GTK4 / gtkmm4)
# Chat-first daily driver: agent roster, threads, approvals, live streaming.
# Target distro: Ubuntu 26.04 LTS (libgtkmm-4.0-dev).
# ============================================================================

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)

# Optional: WebKitGTK 6.0 — embeds the noVNC screen view (agent context
# menu → Computer). Without it the menu item falls back to the system
# browser.
pkg_check_modules(WEBKIT IMPORTED_TARGET webkitgtk-6.0)
if(WEBKIT_FOUND)
    message(STATUS "WebKitGTK ${WEBKIT_VERSION} found — embedded Computer view enabled")
endif()
find_package(nlohmann_json REQUIRED)

# Bundle the SVG icon/avatar set into the binary so the client is
# self-contained (no runtime data-dir lookup). CurrentColor icons are
# re-tinted at load time to match the active dark/light theme.
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
set(GRESOURCE_XML ${CMAKE_CURRENT_SOURCE_DIR}/data/hivemind.gresource.xml)
set(GRESOURCE_C   ${CMAKE_CURRENT_BINARY_DIR}/hivemind-resources.c)
file(GLOB GRESOURCE_DEPS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/data/icons/*.svg
     ${CMAKE_CURRENT_SOURCE_DIR}/data/avatars/*.svg)
add_custom_command(
    OUTPUT ${GRESOURCE_C}
    COMMAND ${GLIB_COMPILE_RESOURCES} ${GRESOURCE_XML}
            --target=${GRESOURCE_C} --generate-source --c-name hivemind
    DEPENDS ${GRESOURCE_XML} ${GRESOURCE_DEPS}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data
    COMMENT "Bundling icons/avatars into GResource")

add_executable(hive-client
    src/main.cpp
    src/HiveApplication.cpp
    src/HiveWindow.cpp
    src/ApiClient.cpp
    src/UiApi.cpp
    ${GRESOURCE_C}
)

target_include_directories(hive-client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hive-client PRIVATE
    PkgConfig::GTKMM
    nlohmann_json::nlohmann_json
    OmniNet
)

if(WEBKIT_FOUND)
    target_compile_definitions(hive-client PRIVATE HIVEMIND_WITH_WEBKIT=1)
    target_link_libraries(hive-client PRIVATE PkgConfig::WEBKIT)
endif()

include(GNUInstallDirs)

install(TARGETS hive-client DESTINATION "${CMAKE_INSTALL_BINDIR}")

# Desktop file is configured so Exec= carries the absolute install path;
# desktop sessions do not reliably have ~/.local/bin in PATH.
configure_file(data/org.hivemind.Client.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/org.hivemind.Client.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.hivemind.Client.desktop
        DESTINATION "share/applications")
install(FILES data/icons/org.hivemind.Client.svg
        DESTINATION "share/icons/hicolor/scalable/apps")

# Unit tests — ./tests/ (scaffold; no test targets yet).
add_subdirectory(tests)
