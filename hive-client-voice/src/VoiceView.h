#pragma once

#include "AgentStateAnnouncer.h"
#include "ApiClient.h"
#include "Prefs.h"
#include "SphereArea.h"

#include <gtkmm.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hv {

class AudioCapture;
class AudioPlayback;
class SoundFx;
class SttEngine;
class TtsEngine;

// Voice session with one agent — the 1:1 port of VoiceActivity.kt.
//
// Big round button: tap once to talk, tap again to send; while the
// agent speaks it becomes an interrupt (cut off → straight back to
// recording). Small side button: cancel / do not send.
//
// IDLE ─tap─▶ LISTENING ─tap─▶ THINKING ─first sentence─▶ STREAMING
//   ─speech done─▶ IDLE; ✕ cancel anywhere → IDLE.
// A page (Gtk::Box, builder root voiceRoot), not a window: MainWindow
// slides it over the roster and destroys it on < Back.
class VoiceView : public Gtk::Box {
 public:
    VoiceView(BaseObjectType* cobject,
                const Glib::RefPtr<Gtk::Builder>& builder, Prefs& prefs,
                AgentInfo agent);
    ~VoiceView() override;

    // < Back pressed — MainWindow pops to the roster and destroys us.
    sigc::signal<void()>& signal_back() { return signal_back_; }

    // DebugShell hooks.
    std::string debugAgentName() const { return agentName_; }
    std::string debugPhaseName() const;
    std::string debugStatusText() const;
    void debugSay(const std::string& text);  // as if STT produced it

    // What the TALK button does — same debounce + phase switch. Used by
    // the button and the XDG global shortcut (portal Activated signal).
    void toggleTalk();
    void debugSetSphere(SphereState state, float level);
    void debugSetSphereLevel(float level);
    std::string debugSphereStats() const;
    void debugSphereShot(const std::string& path);

 private:
    enum class Phase { Setup, Idle, Listening, Thinking, Streaming };

    struct PendingNote {
        std::string senderId;
        std::string text;
    };

    // -- io plumbing -------------------------------------------------
    void post(std::function<void()> work);  // onto the io thread
    void toUi(std::function<void()> fn);    // back onto the UI thread
    void ioLoop();

    // -- setup / phases (UI thread) -----------------------------------
    void openThread();
    void toIdle(const std::string& message);
    void maybeStartListening();
    void sendTranscript();
    void sendAndEnterWait(const std::string& text);
    bool reresolveIfThreadGone(const std::exception& e);
    void startIdleNarration();
    void speakNotice(const std::string& text);
    void speakPending();
    void enterStreaming();
    void onEngineStopped(const std::string& msg);
    void interruptAndTalk();
    void cancelCurrent();
    void confirmStopThread();
    void stopAgentAndConfirm();
    void issueCancelAndResync();
    std::string voiceNameFor(const std::string& senderId) const;

    // -- ticks ---------------------------------------------------------
    void scheduleInboxWatch(int ms);
    void schedulePollTick(int ms);
    void inboxWatchTick();
    void pollTick();
    void handleTick(bool busy, const std::string& error,
                    const std::string& partial, bool cancelling, int gen,
                    bool grew);

    // -- speech ---------------------------------------------------------
    void drainSpeech(bool flush);
    void speakFinalTail();
    void queueSpeech(const std::string& text);
    void queueSpeechChunk(const std::string& text);
    void startSpeakAnimator();
    void stopSpeakAnimator();
    float ttsLevelNow() const;

    // -- widgets ---------------------------------------------------------
    Prefs& prefs_;
    AgentInfo agent_;
    std::string agentName_;

    Gtk::Image* avatar_ = nullptr;
    Gtk::Label* name_ = nullptr;
    Gtk::Label* status_ = nullptr;
    Gtk::Button* talk_ = nullptr;
    Gtk::Button* cancel_ = nullptr;
    Gtk::Button* back_ = nullptr;
    sigc::signal<void()> signal_back_;
    SphereArea* sphere_ = nullptr;

    // -- engines ---------------------------------------------------------
    std::unique_ptr<AudioCapture> capture_;
    std::shared_ptr<std::atomic<bool>> recording_flag_{
        std::make_shared<std::atomic<bool>>(false)};
    std::unique_ptr<AudioPlayback> playback_;
    std::unique_ptr<SttEngine> stt_;
    std::unique_ptr<TtsEngine> tts_;
    std::unique_ptr<SoundFx> fx_;
    std::unique_ptr<ApiClient> api_;
    std::unique_ptr<AgentStateAnnouncer> announcer_;

    // -- state ------------------------------------------------------------
    Phase phase_ = Phase::Setup;
    std::string thread_id_;
    bool showingAgentBusy_ = false;
    std::atomic<bool> stopping_{false};
    bool queuedBehindStop_ = false;
    std::string lastMessageId_;
    std::string finalText_;
    std::size_t finalCredit_ = 0;
    std::map<std::string, std::string> agentNames_;
    std::deque<PendingNote> pendingSpeech_;
    std::string streamText_;
    std::size_t spokenLen_ = 0;
    bool turnDone_ = false;
    long lastProgressMs_ = 0;
    std::size_t lastPartialLen_ = 0;
    std::size_t narrationGiveUpLen_ = 0;
    std::string narratedLive_;  // text already queued from the live partial feed
    std::atomic<long> netFailSince_{0};
    int turnGen_ = 0;
    long lastStatusUpdate_ = 0;
    long lastTalkClick_ = 0;
    sigc::connection speakAnim_;
    sigc::connection inboxConn_;
    sigc::connection pollConn_;

    // io worker: a queue of closures drained by one thread; results come
    // back through a Glib::Dispatcher.
    std::mutex ioMutex_;
    std::deque<std::function<void()>> ioQueue_;
    std::condition_variable ioCv_;
    std::atomic<bool> ioRunning_{false};
    std::thread ioThread_;
    Glib::Dispatcher uiDispatch_;
    std::mutex uiMutex_;
    std::deque<std::function<void()>> uiQueue_;

    // TTS speaking-envelope origin.
    std::chrono::steady_clock::time_point speakStart_;
};

}  // namespace hv
