# Integrated ChimpFlow voice module for hive-client.
#
# C++23 / gtkmm-4.0. Audio capture/playback uses PipeWire (16 kHz mono);
# STT/TTS are served by the local ChimpFlow gateway. There is no standalone
# executable or desktop entry: VoiceView is embedded in the main client.

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTKMM REQUIRED IMPORTED_TARGET gtkmm-4.0)
# epoxy loads GL function pointers for the sphere shaders (GLSL 330).
pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
# PipeWire is optional at build time; without its dev package a null audio
# backend keeps text chat usable and reports voice I/O unavailable.
pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)

set(VOICE_RESOURCES ${CMAKE_CURRENT_BINARY_DIR}/hive-voice-resources.c)
file(GLOB VOICE_AVATAR_RESOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../data/avatars/*.svg)
add_custom_command(
    OUTPUT ${VOICE_RESOURCES}
    COMMAND glib-compile-resources
            --target=${VOICE_RESOURCES}
            --sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/data
            --generate-source
            ${CMAKE_CURRENT_SOURCE_DIR}/data/hive-voice.gresource.xml
    DEPENDS data/hive-voice.gresource.xml data/voice.ui data/embedded.css
            ${VOICE_AVATAR_RESOURCES}
    VERBATIM)

set(HIVE_VOICE_UI_SOURCES
    ${VOICE_RESOURCES}
    src/Avatars.cpp
    src/GlobalShortcuts.cpp
    src/Prefs.cpp
    src/ApiClient.cpp
    src/SpeechText.cpp
    src/TranscriptText.cpp
    src/AgentStateAnnouncer.cpp
    src/AudioCommon.cpp
    src/AudioCapture.cpp
    src/AudioPlayback.cpp
    src/ChimpFlowClient.cpp
    src/SoundFx.cpp
    src/SttEngine.cpp
    src/TtsEngine.cpp
    src/SphereArea.cpp
    src/VoiceView.cpp
)

add_library(hive_voice_ui STATIC ${HIVE_VOICE_UI_SOURCES})
target_compile_features(hive_voice_ui PUBLIC cxx_std_23)
set_target_properties(hive_voice_ui PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(hive_voice_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hive_voice_ui PUBLIC
    PkgConfig::GTKMM
    PkgConfig::EPOXY
    nlohmann_json::nlohmann_json
    OmniNet
)

if(PIPEWIRE_FOUND)
    target_link_libraries(hive_voice_ui PUBLIC PkgConfig::PIPEWIRE)
    target_compile_definitions(hive_voice_ui PRIVATE HIVEMIND_VOICE_PIPEWIRE=1)
else()
    message(WARNING
        "libpipewire-0.3 not found — building integrated voice with a null "
        "audio backend (no mic/speaker). Install libpipewire-0.3-dev for "
        "full voice I/O.")
endif()

if(TARGET compile_options)
    target_link_libraries(hive_voice_ui PRIVATE compile_options)
endif()

add_subdirectory(tests)
