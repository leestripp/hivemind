# hive-client-voice — GTK4 voice client (1:1 port of the Android voice app).
#
# Target: native C++23 / gtkmm-4.0. Mirrors hive-client's CMake shape.
# Audio capture/playback is PipeWire (16 kHz mono float, matching the
# Android AudioRecord/AudioTrack contract); TTS is a piper subprocess fed
# one sentence per line on stdin, raw s16le PCM framed on stdout (see
# TtsEngine); STT is whisper.cpp vendored from the Android app.
cmake_minimum_required(VERSION 3.28)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

add_executable(hive-client-voice
    src/main.cpp
    src/HiveVoiceApp.cpp
    src/Prefs.cpp
    src/ApiClient.cpp
    src/SpeechText.cpp
    src/AgentStateAnnouncer.cpp
    src/AudioCommon.cpp
    src/AudioCapture.cpp
    src/AudioPlayback.cpp
    src/SttEngine.cpp
    src/TtsEngine.cpp
    src/SphereArea.cpp
    src/RosterWindow.cpp
    src/VoiceWindow.cpp
    src/SettingsWindow.cpp
    src/VoicePickerWindow.cpp
)

target_compile_features(hive-client-voice PRIVATE cxx_std_23)
set_target_properties(hive-client-voice PROPERTIES CXX_EXTENSIONS OFF)

target_include_directories(hive-client-voice PRIVATE src)

target_link_libraries(hive-client-voice PRIVATE
    PkgConfig::GTKMM
    PkgConfig::PIPEWIRE
    nlohmann_json::nlohmann_json
    OmniNet::OmniNet
    whisper
)

if(TARGET compile_options)
    target_link_libraries(hive-client-voice PRIVATE compile_options)
endif()

# whisper.cpp is vendored by the Android app; reuse that exact subtree so
# both apps track one STT engine. OpenMP parallelises the encoder.
if(NOT TARGET whisper)
    set(WHISPER_SDL2 OFF CACHE BOOL "" FORCE)
    add_subdirectory(
        ${CMAKE_SOURCE_DIR}/hive-android/app/src/main/cpp/whisper-cpp
        ${CMAKE_BINARY_DIR}/whisper-cpp)
endif()

install(TARGETS hive-client-voice RUNTIME DESTINATION bin)

# Data: desktop entry + icon.
configure_file(data/org.hivemind.Voice.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/org.hivemind.Voice.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.hivemind.Voice.desktop
    DESTINATION share/applications)
install(FILES data/org.hivemind.Voice.svg
    DESTINATION share/icons/hicolor/scalable/apps)

add_subdirectory(tests)
