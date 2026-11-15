#include "AudioCommon.h"
#include "AudioPlayback.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

using namespace std::chrono_literals;

TEST_CASE("mute does not stop playback or completion tags", "[audio]") {
    hv::setMasterVolume(1.0f);
    hv::setMasterMuted(true);

    hv::AudioPlayback playback;
    playback.start();
    std::mutex mutex;
    std::condition_variable done;
    std::uint64_t completed = 0;
    playback.setOnTagDone([&](std::uint64_t tag) {
        {
            std::lock_guard lock{mutex};
            completed = tag;
        }
        done.notify_one();
    });

    playback.submit(41, hv::Pcm16(1600, 1000), 16000);
    {
        std::unique_lock lock{mutex};
        REQUIRE(done.wait_for(lock, 2s, [&] { return completed == 41; }));
    }
    REQUIRE(hv::masterMuted());
    REQUIRE(hv::masterGain() == 0.0f);

    // Unmute is equally independent of playback lifecycle: the same queue
    // accepts and completes the next utterance without restart or resume.
    hv::setMasterMuted(false);
    playback.submit(42, hv::Pcm16(1600, 1000), 16000);
    {
        std::unique_lock lock{mutex};
        REQUIRE(done.wait_for(lock, 2s, [&] { return completed == 42; }));
    }
    playback.stop();
}
