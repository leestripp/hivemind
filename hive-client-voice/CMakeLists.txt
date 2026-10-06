# hive-client-voice — GTK4 voice client (1:1 port of the Android voice app).
#
# Target: native C++23 / gtkmm-4.0. Mirrors hive-client's CMake shape.
# Audio capture/playback is PipeWire (16 kHz mono, matching the Android
# AudioRecord/AudioTrack contract); TTS and STT are both served by the
# local ChimpFlow gateway (audio.cpp + whisper.cpp over localhost HTTP,
# see ChimpFlowClient) — no local models, no subprocesses.
cmake_minimum_required(VERSION 3.28)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
# epoxy loads GL function pointers for the sphere shaders (GLSL 330).
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
# PipeWire is the audio backend (16 kHz mono capture + playback). The
# whole app still builds without the dev package — voice I/O compiles to
# a null backend and the app says so on startup — but install
# libpipewire-0.3-dev for a functional client.
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)

set(VOICE_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/hive-voice-resources.c)
add_custom_command(
    OUTPUT ${VOICE_RESOURCES}
    COMMAND glib-compile-resources --target=${VOICE_RESOURCES} --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data --generate-source ${CMAKE_CURRENT_SOURCE_DIR}/data/hive-voice.gresource.xml
    DEPENDS data/hive-voice.gresource.xml data/voice.ui data/style.css
    VERBATIM)

add_executable(hive-client-voice
    ${VOICE_RESOURCES}
    src/main.cpp
    src/Avatars.cpp
    src/DebugShell.cpp
    src/GlobalShortcuts.cpp
    src/HiveVoiceApp.cpp
    src/Prefs.cpp
    src/ApiClient.cpp
    src/SpeechText.cpp
    src/AgentStateAnnouncer.cpp
    src/AudioCommon.cpp
    src/AudioCapture.cpp
    src/AudioPlayback.cpp
    src/ChimpFlowClient.cpp
    src/SttEngine.cpp
    src/TtsEngine.cpp
    src/SphereArea.cpp
    src/MainWindow.cpp
    src/RosterView.cpp
    src/VoiceView.cpp
    src/SettingsWindow.cpp
    src/VoicePickerWindow.cpp
)

target_compile_features(hive-client-voice PRIVATE cxx_std_23)
set_target_properties(hive-client-voice PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(hive-client-voice PRIVATE src)

target_link_libraries(hive-client-voice PRIVATE
    PkgConfig::GTKMM
    PkgConfig::EPOXY
    nlohmann_json::nlohmann_json
    OmniNet
)

if(PIPEWIRE_FOUND)
    target_link_libraries(hive-client-voice PRIVATE PkgConfig::PIPEWIRE)
    target_compile_definitions(hive-client-voice PRIVATE HIVEMIND_VOICE_PIPEWIRE=1)
else()
    message(WARNING
        "libpipewire-0.3 not found — building hive-client-voice with a null "
        "audio backend (no mic/speaker). Install libpipewire-0.3-dev for "
        "full voice I/O.")
endif()

if(TARGET compile_options)
    target_link_libraries(hive-client-voice PRIVATE compile_options)
endif()

set_target_properties(hive-client-voice PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib")
install(TARGETS hive-client-voice RUNTIME DESTINATION bin)

# Data: desktop entry + icon.
configure_file(data/org.hivemind.Voice.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/org.hivemind.Voice.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.hivemind.Voice.desktop
    DESTINATION share/applications)
install(FILES data/org.hivemind.Voice.svg
    DESTINATION share/icons/hicolor/scalable/apps)

add_subdirectory(tests)
