# ============================================================================
# hive-client — HiveMind 2.0 native Linux client (GTK4 / gtkmm4)
# Chat-first daily driver: agent roster, threads, approvals, live streaming.
# Target distro: Ubuntu 26.04 LTS (libgtkmm-4.0-dev).
# ============================================================================

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
find_package(CURL REQUIRED)
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
    CURL::libcurl
    nlohmann_json::nlohmann_json
    OmniNet
)

install(TARGETS hive-client DESTINATION "bin")
install(FILES data/hivemind.desktop DESTINATION "share/applications")
