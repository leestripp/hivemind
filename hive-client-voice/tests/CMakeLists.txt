# Pure-logic unit tests — Catch2 v3 via FetchContent, mirroring the other
# apps' test suites. Audio/GL/GTK layers stay integration-tested by hand;
# everything speech-shaped that CAN be pure IS pure and tested here.
cmake_minimum_required(VERSION 3.28)

FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE)
if(NOT Catch2_POPULATED)
    FetchContent_MakeAvailable(Catch2)
endif()

add_executable(test_speech_text test_speech_text.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/SpeechText.cpp)
target_link_libraries(test_speech_text PRIVATE Catch2::Catch2WithMain)

add_executable(test_transcript_text test_transcript_text.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/TranscriptText.cpp)
target_link_libraries(test_transcript_text PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_transcript_text PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(test_announcer test_announcer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/AgentStateAnnouncer.cpp)
target_link_libraries(test_announcer PRIVATE Catch2::Catch2WithMain)

# DM/agent-wide activity scope — regression coverage for agent-to-agent
# relay chains outliving the selected agent's direct-message turn.
add_executable(test_voice_turn_logic test_voice_turn_logic.cpp)
target_link_libraries(test_voice_turn_logic PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_voice_turn_logic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(test_tts_tag_ledger test_tts_tag_ledger.cpp)
target_link_libraries(test_tts_tag_ledger PRIVATE Catch2::Catch2WithMain)
target_include_directories(test_tts_tag_ledger PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(test_prefs test_prefs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Prefs.cpp)
target_link_libraries(test_prefs PRIVATE Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json)

add_executable(test_avatars test_avatars.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Avatars.cpp)
target_link_libraries(test_avatars PRIVATE Catch2::Catch2WithMain
    PkgConfig::GTKMM)

# WavCodec is header-only pure logic — the WAV parse/resample/encode
# path the ChimpFlow client depends on.
add_executable(test_wav_codec test_wav_codec.cpp)
target_link_libraries(test_wav_codec PRIVATE Catch2::Catch2WithMain)

# Loudness normalization — the shared-volume DSP every TTS utterance
# gets before playback.
add_executable(test_audio_common test_audio_common.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/AudioCommon.cpp)
target_link_libraries(test_audio_common PRIVATE Catch2::Catch2WithMain)

foreach(t test_speech_text test_transcript_text test_announcer test_voice_turn_logic test_tts_tag_ledger test_prefs test_avatars test_wav_codec test_audio_common)
    if(TARGET compile_options)
        target_link_libraries(${t} PRIVATE compile_options)
    endif()
    add_test(NAME ${t} COMMAND ${t})
endforeach()
