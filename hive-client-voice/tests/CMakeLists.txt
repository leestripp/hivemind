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

add_executable(test_announcer test_announcer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/AgentStateAnnouncer.cpp)
target_link_libraries(test_announcer PRIVATE Catch2::Catch2WithMain)

add_executable(test_prefs test_prefs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Prefs.cpp)
target_link_libraries(test_prefs PRIVATE Catch2::Catch2WithMain
    nlohmann_json::nlohmann_json)

foreach(t test_speech_text test_announcer test_prefs)
    if(TARGET compile_options)
        target_link_libraries(${t} PRIVATE compile_options)
    endif()
    add_test(NAME ${t} COMMAND ${t})
endforeach()
