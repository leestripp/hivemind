#include "VoiceView.h"

#include "DebugShell.h"

#include "AudioCapture.h"
#include "Avatars.h"
#include "AudioPlayback.h"
#include "SoundFx.h"
#include "SpeechText.h"
#include "TranscriptText.h"
#include "VoiceTurnLogic.h"
#include "SttEngine.h"
#include "TtsEngine.h"

#include <algorithm>
#include <chrono>

namespace hv {

namespace {
constexpr int kPollMs = 700;
constexpr int kIdlePollMs = 2500;
constexpr long kStallTimeoutMs = 240000;
constexpr long kNetFailTimeoutMs = 45000;
constexpr long kStopConfirmMs = 12000;
constexpr std::size_t kMaxUtterChars = 2000;

std::string trimStr(std::string_view s) {
    const auto b = s.find_first_not_of(" \t\n\r");
    if (b == std::string_view::npos) return "";
    const auto e = s.find_last_not_of(" \t\n\r");
    return std::string(s.substr(b, e - b + 1));
}

long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace

VoiceView::VoiceView(BaseObjectType* cobject,
                         const Glib::RefPtr<Gtk::Builder>& builder,
                         Prefs& prefs, AgentInfo agent)
    : Gtk::Box(cobject), prefs_(prefs), agent_(std::move(agent)) {
    DebugShell::registerVoice(this);
    agentName_ = agent_.name.empty() ? "Agent" : agent_.name;

    avatar_ = builder->get_widget<Gtk::Image>("agentAvatar");
    name_ = builder->get_widget<Gtk::Label>("agentName");
    status_ = builder->get_widget<Gtk::Label>("statusText");
    talk_ = builder->get_widget<Gtk::Button>("talkButton");
    cancel_ = builder->get_widget<Gtk::Button>("cancelButton");
    back_ = builder->get_widget<Gtk::Button>("backButton");
    if (back_)
        back_->signal_clicked().connect(
            [this] { signal_back_.emit(); });
    sphere_ = Gtk::Builder::get_widget_derived<SphereArea>(builder, "sphere");
    if (name_) name_->set_label(agentName_);
    if (avatar_) {
        avatar_->set(avatarPixbuf(resolveAvatar(agentName_, agent_.avatar),
                                  80));  // 40 logical px, 2x for HiDPI
        avatar_->set_pixel_size(40);
    }

    // io worker + UI dispatch.
    ioRunning_ = true;
    ioThread_ = std::thread([this] { ioLoop(); });
    uiDispatch_.connect([this] {
        std::deque<std::function<void()>> q;
        {
            std::lock_guard<std::mutex> lk(uiMutex_);
            q.swap(uiQueue_);
        }
        for (auto& fn : q) fn();
    });

    api_ = std::make_unique<ApiClient>(prefs_.base(), prefs_.apiKey());
    playback_ = std::make_unique<AudioPlayback>();
    playback_->start();
    tts_ = std::make_unique<TtsEngine>(*playback_, prefs_.chimpflowUrl(),
                                       prefs_.chimpflowKey());
    tts_->onQueueEmpty = [this] {
        toUi([this] {
            if (phase_ == Phase::Streaming && turnDone_) toIdle("");
        });
    };
    // PTT confirmation beeps live on their own playback queue so they
    // never interleave with (or delay) TTS utterances.
    fx_ = std::make_unique<SoundFx>();
    capture_ = std::make_unique<AudioCapture>();
    stt_ = std::make_unique<SttEngine>(
        *capture_, prefs_.chimpflowUrl(), prefs_.chimpflowKey(),
        [this](float level) {
            toUi([this, level] {
                if (phase_ == Phase::Listening && sphere_) sphere_->setLevel(level);
            });
        },
        [this](const std::string& msg) { toUi([this, msg] { onEngineStopped(msg); }); });
    // Push-to-talk playback hold (2026-10-14): agents must not talk
    // over the user's recording. The recording listener drives a
    // shared flag; the playback hold probe reads it. Both lambdas
    // hold a shared_ptr copy, so the flag outlives whichever engine
    // dies first — no set/forget, no dangling.
    stt_->setRecordingListener(
        [flag = recording_flag_](bool rec) { flag->store(rec); });
    playback_->setHoldProbe(
        [flag = recording_flag_] { return flag->load(); });
    // Warm the /healthz readiness probe in the background so a slow
    // post-reboot gateway is picked up without a dead first TALK press.
    stt_->prime();
    announcer_ = std::make_unique<AgentStateAnnouncer>();
    tts_->applyVoice(voiceNameFor(agent_.id));

    if (talk_) {
        talk_->set_sensitive(false);
        talk_->signal_clicked().connect([this] { toggleTalk(); });
    }
    if (cancel_) {
        cancel_->set_visible(false);
        cancel_->signal_clicked().connect([this] { cancelCurrent(); });
    }
    if (status_) status_->set_label("Connecting…");
    if (sphere_) sphere_->setState(SphereState::Idle);

    openThread();
}

VoiceView::~VoiceView() {
    DebugShell::registerVoice(nullptr);
    stopSpeakAnimator();
    inboxConn_.disconnect();
    pollConn_.disconnect();
    ioRunning_ = false;
    {
        std::lock_guard<std::mutex> lk(ioMutex_);
        ioCv_.notify_all();
    }
    if (ioThread_.joinable()) ioThread_.join();
    stt_.reset();
    tts_.reset();
    playback_.reset();
    capture_.reset();
}

// -- io plumbing ---------------------------------------------------------

void VoiceView::post(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lk(ioMutex_);
        ioQueue_.push_back(std::move(work));
    }
    ioCv_.notify_one();
}

void VoiceView::toUi(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(uiMutex_);
        uiQueue_.push_back(std::move(fn));
    }
    uiDispatch_.emit();
}

void VoiceView::ioLoop() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lk(ioMutex_);
            ioCv_.wait(lk, [&] { return !ioRunning_ || !ioQueue_.empty(); });
            if (!ioRunning_) return;
            work = std::move(ioQueue_.front());
            ioQueue_.pop_front();
        }
        try {
            work();
        } catch (...) {
            // Per-task containment: a throwing task must never kill the
            // io thread (mirrors Kotlin runCatching at the call sites).
        }
    }
}

// -- setup -----------------------------------------------------------------

void VoiceView::openThread() {
    post([this] {
        try {
            thread_id_ = api_->openDm(agent_.id);
            lastMessageId_ = api_->latestMessageId(thread_id_);
            try { api_->markRead(thread_id_); } catch (...) {}
            try {
                for (const auto& a : api_->listAgents())
                    agentNames_[a.id] = a.name;
            } catch (...) {}
            toUi([this] {
                toIdle("Tap TALK and speak");
                scheduleInboxWatch(kIdlePollMs);
            });
        } catch (const std::exception& e) {
            toUi([this, msg = std::string{e.what()}] {
                if (status_)
                    status_->set_label("Couldn't open conversation:\n" + msg +
                                       "\n(tap to retry)");
                // Retry automatically while still in Setup.
                if (phase_ == Phase::Setup) {
                    Glib::signal_timeout().connect_once(
                        [this] {
                            if (phase_ == Phase::Setup) {
                                if (status_) status_->set_label("Connecting…");
                                openThread();
                            }
                        },
                        3000);
                }
            });
        }
    });
}

// -- phases -----------------------------------------------------------------

void VoiceView::toIdle(const std::string& message) {
    ++turnGen_;
    stopping_ = false;
    queuedBehindStop_ = false;
    phase_ = Phase::Idle;
    finalText_.clear();
    finalCredit_ = 0;
    narratedLive_.clear();
    streamText_.clear();
    spokenLen_ = 0;
    turnDone_ = false;
    if (sphere_) {
        sphere_->setState(SphereState::Idle);
        sphere_->setLevel(0.0f);
    }
    stopSpeakAnimator();
    if (talk_) {
        talk_->remove_css_class("talk-active");
        talk_->set_label("TALK");
        talk_->set_sensitive(true);
    }
    if (cancel_) cancel_->set_visible(false);
    if (!message.empty() && status_) status_->set_label(message);
    // Notes that arrived mid-turn wait for exactly this moment.
    if (!pendingSpeech_.empty()) speakPending();
}

void VoiceView::maybeStartListening() {
    phase_ = Phase::Listening;
    if (sphere_) sphere_->setState(SphereState::Listening);
    if (talk_) {
        talk_->set_label("SEND");
        talk_->add_css_class("talk-active");
    }
    if (cancel_) cancel_->set_visible(true);
    if (status_)
        status_->set_label(stt_->engineReady() ? "Listening…"
                                               : "Starting voice service…");
    if (fx_) fx_->recordingStarted();
    stt_->start();
}

void VoiceView::sendTranscript() {
    if (sphere_) sphere_->setLevel(0.0f);
    if (fx_) fx_->recordingStopped();
    phase_ = Phase::Thinking;
    ++turnGen_;
    // New turn: old narration state must not credit or trim new messages.
    narratedLive_.clear();
    finalText_.clear();
    finalCredit_ = 0;
    tts_->applyVoice(voiceNameFor(agent_.id));
    if (sphere_) sphere_->setState(SphereState::Processing);
    if (talk_) {
        talk_->set_label("···");
        talk_->set_sensitive(false);
        talk_->remove_css_class("talk-active");
    }
    if (cancel_) cancel_->set_visible(true);
    if (status_) status_->set_label("Transcribing…");
    lastProgressMs_ = nowMs();
    lastPartialLen_ = 0;
    netFailSince_ = 0;
    stt_->stop([this] {
        toUi([this] {
            if (phase_ != Phase::Thinking) return;  // cancelled meanwhile
            const std::string text = trimStr(stt_->transcript());
            if (text.empty()) {
                toIdle("Didn't catch that — tap TALK to retry");
                return;
            }
            if (status_) status_->set_label("Sending…");
            post([this, text] { postTranscript(text); });
        });
    });
}

void VoiceView::sendAndEnterWait(const std::string& text) {
    ThreadActivity preAct;
    bool havePre = false;
    try {
        preAct = api_->activity(thread_id_);
        havePre = true;
    } catch (...) {}
    bool agentBusy = false;
    try {
        for (const auto& a : api_->listAgents())
            if (a.id == agent_.id) agentBusy = a.busy;
    } catch (...) {}
    const bool queuedBehindStop = havePre && preAct.cancelling;
    const bool queued = queuedBehindStop || agentBusy;
    api_->postMessage(thread_id_, text, "voice");
    toUi([this, queued, queuedBehindStop] {
        if (phase_ != Phase::Thinking) return;
        // Only a cancelling DM needs the special dead-zone wait below.
        // Ordinary agent-wide busy is usually an agent-to-agent turn in
        // ANOTHER thread; tying this flag to that busy state can leave the
        // voice UI waiting forever after the relay chain goes idle.
        queuedBehindStop_ = queuedBehindStop;
        if (sphere_) sphere_->setState(SphereState::Thinking);
        if (status_)
            status_->set_label(queued
                ? agentName_ + " is busy — your message is queued…"
                : "Thinking…");
        schedulePollTick(kPollMs);
    });
}

void VoiceView::postTranscript(const std::string& text) {
    // STT input is ONE user message. Only outbound TTS is sentence-split.
    const std::string cleaned = sanitizeSttTranscript(text);
    if (cleaned.empty()) {
        toUi([this] { toIdle("Didn't catch that — tap TALK to retry"); });
        return;
    }
    try {
        sendAndEnterWait(cleaned);
    } catch (const std::exception& e) {
        if (reresolveIfThreadGone(e)) {
            try {
                sendAndEnterWait(cleaned);
                return;
            } catch (const std::exception&) {
                // fall through to the visible failure
            }
        }
        std::string msg = e.what();
        toUi([this, msg = std::move(msg)] {
            toIdle("Send failed: " + msg);
            speakNotice("Your message didn't send. Check the "
                        "connection and try again.");
        });
    }
}

bool VoiceView::reresolveIfThreadGone(const std::exception& e) {
    const auto* apiEx = dynamic_cast<const ApiException*>(&e);
    if (!apiEx || apiEx->code() != 404) return false;
    try {
        thread_id_ = api_->openDm(agent_.id);
        lastMessageId_ = api_->latestMessageId(thread_id_);
        try { api_->markRead(thread_id_); } catch (...) {}
        narratedLive_.clear();
        finalText_.clear();
        finalCredit_ = 0;
        return true;
    } catch (...) {
        return false;
    }
}

void VoiceView::startIdleNarration() {
    if (phase_ != Phase::Idle) return;
    ++turnGen_;
    narratedLive_.clear();
    finalText_.clear();
    finalCredit_ = 0;
    streamText_.clear();
    spokenLen_ = 0;
    lastPartialLen_ = 0;
    lastProgressMs_ = nowMs();
    netFailSince_ = 0;
    phase_ = Phase::Thinking;
    tts_->applyVoice(voiceNameFor(agent_.id));
    if (sphere_) sphere_->setState(SphereState::Thinking);
    if (talk_) {
        talk_->set_label("···");
        talk_->set_sensitive(false);
        talk_->remove_css_class("talk-active");
    }
    if (cancel_) cancel_->set_visible(true);
    if (status_) status_->set_label("Replying…");
    schedulePollTick(kPollMs);
}

void VoiceView::speakNotice(const std::string& text) {
    tts_->applyVoice(voiceNameFor(agent_.id));
    turnDone_ = true;
    streamText_ = text;
    spokenLen_ = 0;
    drainSpeech(true);
}

void VoiceView::speakPending() {
    if (phase_ != Phase::Idle || pendingSpeech_.empty()) return;
    PendingNote note = std::move(pendingSpeech_.front());
    pendingSpeech_.pop_front();
    tts_->applyVoice(voiceNameFor(note.senderId));
    turnDone_ = true;
    streamText_ = note.text;
    spokenLen_ = 0;
    drainSpeech(true);
}

void VoiceView::enterStreaming() {
    phase_ = Phase::Streaming;
    if (sphere_) sphere_->setState(SphereState::Speaking);
    if (talk_) {
        talk_->remove_css_class("talk-active");
        talk_->set_label("TALK");  // interrupt-into-recording
        talk_->set_sensitive(true);
    }
    if (cancel_) cancel_->set_visible(true);
    if (status_) status_->set_label("");
    startSpeakAnimator();
}

void VoiceView::onEngineStopped(const std::string& msg) {
    if (phase_ != Phase::Listening) {
        if (status_) status_->set_label(msg);
    } else if (!stt_->transcript().empty()) {
        if (status_)
            status_->set_label("⚠ " + msg +
                               " — SEND keeps what you have, TALK restarts");
    } else {
        toIdle(msg);
    }
}

void VoiceView::toggleTalk() {
    const long now = nowMs();
    if (now - lastTalkClick_ < 500) return;  // debounce
    lastTalkClick_ = now;
    switch (phase_) {
        case Phase::Idle:
            if (!stopping_) maybeStartListening();
            break;
        case Phase::Listening: sendTranscript(); break;
        case Phase::Streaming: interruptAndTalk(); break;
        default: break;  // Setup/Thinking: insensitive
    }
}

void VoiceView::interruptAndTalk() {
    tts_->stop();
    issueCancelAndResync();
    toIdle("");
    maybeStartListening();
}

void VoiceView::cancelCurrent() {
    // Stop means STOP: queued inbox notes must not start playing the
    // moment the cancel lands.
    pendingSpeech_.clear();
    switch (phase_) {
        case Phase::Listening:
            if (fx_) fx_->recordingStopped();
            stt_->discard();
            toIdle("Cancelled");
            break;
        case Phase::Thinking:
        case Phase::Streaming:
            tts_->stop();
            confirmStopThread();
            break;
        case Phase::Idle:
            if (showingAgentBusy_) {
                post([this] { stopAgentAndConfirm(); });
            } else {
                toIdle("Cancelled");
            }
            break;
        default:
            break;
    }
}

void VoiceView::confirmStopThread() {
    post([this] {
        bool free_ = false;
        try {
            api_->cancelThread(thread_id_);
            const long deadline = nowMs() + kStopConfirmMs;
            while (nowMs() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                ThreadActivity act;
                try {
                    act = api_->activity(thread_id_);
                } catch (...) {
                    break;
                }
                if (!act.busy) {
                    free_ = true;
                    break;
                }
            }
            if (free_) {
                try {
                    const std::string id = api_->latestMessageId(thread_id_);
                    if (!id.empty()) lastMessageId_ = id;
                } catch (...) {}
            }
        } catch (const std::exception&) {
            toUi([this] { toIdle("Stop failed — try again"); });
            return;
        }
        toUi([this, free_] {
            if (free_) {
                toIdle("Cancelled");
            } else if (phase_ == Phase::Thinking ||
                       phase_ == Phase::Streaming) {
                stopping_ = true;
                if (talk_) {
                    talk_->set_label("···");
                    talk_->set_sensitive(false);
                }
                if (status_)
                    status_->set_label("Still busy — the current command "
                                       "must finish first…");
            }
        });
    });
}

void VoiceView::stopAgentAndConfirm() {
    bool free_ = false;
    try {
        api_->cancelAgent(agent_.id);
        const long deadline = nowMs() + kStopConfirmMs;
        while (nowMs() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            try {
                for (const auto& a : api_->listAgents()) {
                    if (a.id == agent_.id) {
                        free_ = !a.busy;
                        break;
                    }
                }
            } catch (...) {
            }
            if (free_) break;
        }
    } catch (const std::exception&) {
        toUi([this] { toIdle("Stop failed — try again"); });
        return;
    }
    toUi([this, free_] {
        if (free_) {
            showingAgentBusy_ = false;
            toIdle("Stopped");
        } else if (phase_ == Phase::Idle) {
            stopping_ = true;
            if (talk_) talk_->set_sensitive(false);
            if (status_)
                status_->set_label("Still busy — the current command "
                                   "must finish first…");
        }
    });
}

void VoiceView::issueCancelAndResync() {
    post([this] {
        api_->cancelThread(thread_id_);
        const long deadline = nowMs() + 15000;
        while (nowMs() < deadline) {
            ThreadActivity act;
            try {
                act = api_->activity(thread_id_);
            } catch (...) {
                break;
            }
            if (!act.busy) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        try {
            const std::string id = api_->latestMessageId(thread_id_);
            if (!id.empty()) lastMessageId_ = id;
        } catch (...) {}
    });
}

std::string VoiceView::voiceNameFor(const std::string& senderId) const {
    const std::string v = prefs_.voiceFor(senderId);
    if (!v.empty()) return v;
    if (senderId != agent_.id) return prefs_.voiceFor(agent_.id);
    return "";
}

// -- ticks -------------------------------------------------------------------

void VoiceView::scheduleInboxWatch(int ms) {
    inboxConn_.disconnect();
    inboxConn_ = Glib::signal_timeout().connect(
        [this] {
            inboxWatchTick();
            return false;
        },
        ms);
}

void VoiceView::schedulePollTick(int ms) {
    pollConn_.disconnect();
    pollConn_ = Glib::signal_timeout().connect(
        [this] {
            pollTick();
            return false;
        },
        ms);
}

void VoiceView::inboxWatchTick() {
    if (phase_ != Phase::Idle) {
        scheduleInboxWatch(kIdlePollMs);
        return;
    }
    post([this] {
        std::vector<PendingNote> fresh;
        std::string meBusy;
        bool haveMe = false;
        ThreadActivity act;
        bool haveAct = false;
        bool dmConsumed = false;
        try {
            for (const auto& m : api_->messagesAfter(thread_id_, lastMessageId_)) {
                lastMessageId_ = m.id;
                dmConsumed = true;
                if (m.senderKind == "agent" && !m.body.empty()) {
                    // Accumulate into the turn's final text and speak only
                    // the unheard tail (credited against the live narration).
                    // Per-message credit — never re-reads earlier messages.
                    if (!finalText_.empty()) finalText_ += ' ';
                    finalText_ += trimStr(m.body);
                    toUi([this] { speakFinalTail(); });
                }
            }
            try {
                act = api_->activity(thread_id_);
                haveAct = true;
            } catch (...) {}
            // Deliberately no You/inbox narration: completion notes there
            // duplicate what the agent's DM narration already spoke. The
            // inbox is left unread for the text UIs. (lee, 2026-10-25)
            std::vector<AgentSnapshot> roster;
            try {
                for (const auto& a : api_->listAgents()) {
                    agentNames_[a.id] = a.name;
                    roster.push_back({a.id, a.name, a.online, a.busy});
                    if (a.id == agent_.id) {
                        meBusy = a.busy ? "1" : "0";
                        haveMe = true;
                    }
                }
            } catch (...) {}
            if (!roster.empty() && prefs_.stateAnnounce()) {
                for (const auto& ann : announcer_->diff(roster))
                    fresh.push_back({ann.agentId, ann.text});
            }
            try { if (dmConsumed) api_->markRead(thread_id_); } catch (...) {}
        } catch (const std::exception& e) {
            if (!reresolveIfThreadGone(e)) {
                // poll blip — reschedule below
            }
        }
        toUi([this, fresh = std::move(fresh), meBusy, haveMe,
              act = std::move(act), haveAct]() mutable {
            // Stop confirmation (agent level).
            if (stopping_ && haveMe && meBusy == "0") {
                stopping_ = false;
                showingAgentBusy_ = false;
                toIdle("Stopped");
            }
            if (haveAct) {
                if (!act.busy) narrationGiveUpLen_ = 0;
                else if (narrationGiveUpLen_ == 0 ||
                         act.partial.size() > narrationGiveUpLen_)
                    startIdleNarration();
            }
            if (!fresh.empty()) {
                for (auto& n : fresh) pendingSpeech_.push_back(std::move(n));
                speakPending();
            }
            // Working chrome, edge-triggered on the roster busy flag.
            if (haveMe && phase_ == Phase::Idle &&
                ((meBusy == "1") != showingAgentBusy_)) {
                showingAgentBusy_ = (meBusy == "1");
                if (showingAgentBusy_) {
                    if (sphere_) sphere_->setState(SphereState::Thinking);
                    if (status_) status_->set_label(agentName_ + " is working…");
                    if (cancel_) cancel_->set_visible(true);
                } else {
                    if (sphere_) sphere_->setState(SphereState::Idle);
                    if (cancel_) cancel_->set_visible(false);
                    if (status_) {
                        const Glib::ustring was = agentName_ + " is working…";
                        if (status_->get_label() == was ||
                            status_->get_label() == "Stopping…")
                            status_->set_label("");
                    }
                }
            }
            scheduleInboxWatch(kIdlePollMs);
        });
    });
}

void VoiceView::pollTick() {
    if (phase_ != Phase::Thinking && phase_ != Phase::Streaming) return;
    const long now = nowMs();
    if (netFailSince_ != 0 && now - netFailSince_ > kNetFailTimeoutMs) {
        tts_->stop();
        narratedLive_ = streamText_.substr(0, spokenLen_);
        narrationGiveUpLen_ = lastPartialLen_;
        toIdle("Connection lost");
        speakNotice("I lost the connection to the hub. Check your network, "
                    "then ask again.");
        return;
    }
    if (now - lastProgressMs_ > kStallTimeoutMs) {
        tts_->stop();
        narratedLive_ = streamText_.substr(0, spokenLen_);
        narrationGiveUpLen_ = lastPartialLen_;
        toIdle("Agent stopped responding");
        speakNotice("The agent stopped responding partway through. I'll "
                    "read the full reply out when it arrives.");
        return;
    }
    const int gen = turnGen_;
    post([this, gen] {
        try {
            ThreadActivity act = api_->activity(thread_id_);
            const bool agentBusy = api_->agentBusy(agent_.id);
            bool appended = false;
            for (const auto& m : api_->messagesAfter(thread_id_, lastMessageId_)) {
                lastMessageId_ = m.id;
                if (m.senderKind == "agent" && !m.body.empty()) {
                    if (!finalText_.empty()) finalText_ += ' ';
                    finalText_ += trimStr(m.body);
                    appended = true;
                }
            }
            netFailSince_ = 0;
            const bool grew = appended || act.partial.size() > lastPartialLen_;
            if (!act.partial.empty()) lastPartialLen_ = act.partial.size();
            toUi([this, act, agentBusy, gen, grew] {
                handleTick(act.busy, agentBusy, act.error, act.partial,
                           act.cancelling, gen, grew);
            });
        } catch (const std::exception& e) {
            fprintf(stderr, "hive-poll: tick threw: %s\n", e.what());
            if (reresolveIfThreadGone(e)) {
                netFailSince_ = 0;
            } else if (netFailSince_ == 0) {
                netFailSince_ = nowMs();
            }
            toUi([this] { schedulePollTick(kPollMs); });
        }
    });
}

void VoiceView::handleTick(bool threadBusy, bool agentBusy,
                           const std::string& error,
                           const std::string& partial, bool cancelling,
                           int gen, bool grew) {
    if (gen != turnGen_) return;  // stale tick from a cancelled turn
    if (phase_ != Phase::Thinking && phase_ != Phase::Streaming) return;
    if (grew) lastProgressMs_ = nowMs();
    const bool busy = threadBusy;
    const hv::voice_turn::ActivityScope scope{threadBusy, agentBusy};
    if (!error.empty()) {
        tts_->stop();
        toIdle("Agent error: " + error);
        speakNotice("Sorry, the agent hit an error and stopped. Please "
                    "try again.");
        return;
    }
    // Stop confirmation: only stand down once the turn actually died.
    if (!busy && stopping_) {
        toIdle("Stopped");
        return;
    }
    // A cancelling turn's partial is the OLD reply on its way out.
    if (busy && cancelling) {
        lastProgressMs_ = nowMs();
        schedulePollTick(kPollMs);
        return;
    }
    if (busy && queuedBehindStop_) queuedBehindStop_ = false;
    if (!busy && queuedBehindStop_) {
        schedulePollTick(kPollMs);
        return;
    }
    if (hv::voice_turn::workingOffThread(scope)) {
        // The DM is idle, but the selected agent is still working in an
        // agent-to-agent thread. Speak any new DM messages now, then keep
        // waiting for the AGENT-wide idle edge instead of prematurely
        // completing (or wedging behind) this thread's activity feed.
        if (!finalText_.empty()) speakFinalTail();
        schedulePollTick(kPollMs);
        return;
    }
    if (!busy && !finalText_.empty()) {
        // Turn over with posted messages: the final text is the source of
        // truth. Speak only what neither the live feed nor an earlier
        // final-tail pass already covered.
        if (spokenLen_ > 0)
            narratedLive_ = streamText_.substr(0, spokenLen_);
        speakFinalTail();
    } else {
        const std::string next = partial;
        if (busy && next.empty()) {
            schedulePollTick(kPollMs);
            return;
        }
        if (next.compare(0, streamText_.size(), streamText_) != 0 ||
            next.size() < streamText_.size()) {
            // Source diverged: re-speak only from the first differing char.
            const std::size_t lcp = speech::commonPrefixLen(next, streamText_);
            spokenLen_ = std::min(spokenLen_, lcp);
            streamText_ = next;
        } else {
            streamText_ = next;
        }
        drainSpeech(!hv::voice_turn::inFlight(scope));
        // Remember exactly what the live feed already queued so the final
        // message handler can trim down to the unheard tail instead of
        // re-reading the whole reply.
        if (spokenLen_ > 0) narratedLive_ = streamText_.substr(0, spokenLen_);
    }
    if (hv::voice_turn::inFlight(scope)) {
        schedulePollTick(kPollMs);
    } else {
        turnDone_ = true;
        if (tts_->outstanding() == 0) toIdle("");
    }
}

// -- speech -------------------------------------------------------------------

void VoiceView::speakFinalTail() {
    if (finalText_.empty()) return;
    std::size_t credit = speech::commonPrefixLen(finalText_, narratedLive_);
    if (credit < finalCredit_) credit = finalCredit_;  // append-only: never regress
    if (credit < finalText_.size()) {
        const std::string tail = trimStr(finalText_.substr(credit));
        if (!tail.empty()) queueSpeech(tail);
    }
    finalCredit_ = finalText_.size();
}

void VoiceView::drainSpeech(bool flush) {
    if (flush) {
        const std::string tail =
            trimStr(streamText_.substr(spokenLen_));
        if (!tail.empty()) queueSpeech(tail);
        spokenLen_ = streamText_.size();
        return;
    }
    std::size_t end = std::string::npos;
    for (std::size_t i = streamText_.size(); i > spokenLen_ + 1; --i) {
        const char ch = streamText_[i - 1];
        if ((ch == '.' || ch == '!' || ch == '?' || ch == ':' || ch == ';' ||
             ch == '\n') &&
            std::isspace(static_cast<unsigned char>(streamText_[i]))) {
            end = i;
            break;
        }
    }
    if (end != std::string::npos && end > spokenLen_) {
        const std::string chunk = trimStr(
            streamText_.substr(spokenLen_, end - spokenLen_));
        if (!chunk.empty()) queueSpeech(chunk);
        spokenLen_ = end;
    }
}

void VoiceView::queueSpeech(const std::string& text) {
    const std::string clean = speech::forSpeech(text);
    if (clean.empty()) return;
    std::string rest = clean;
    while (rest.size() > kMaxUtterChars) {
        std::size_t cut = rest.rfind(' ', kMaxUtterChars);
        if (cut < kMaxUtterChars / 2) cut = kMaxUtterChars;
        queueSpeechChunk(rest.substr(0, cut));
        rest = trimStr(rest.substr(cut));
    }
    queueSpeechChunk(rest);
}

void VoiceView::queueSpeechChunk(const std::string& text) {
    if (text.empty()) return;
    fprintf(stderr, "hive-voice: tts << %s\n", text.c_str());
    tts_->speakQueued(text);
    if (phase_ == Phase::Thinking || phase_ == Phase::Idle) enterStreaming();
}

// -- sphere level while speaking ------------------------------------------------

void VoiceView::startSpeakAnimator() {
    stopSpeakAnimator();
    speakStart_ = std::chrono::steady_clock::now();
    speakAnim_ = Glib::signal_timeout().connect(
        [this] {
            if (phase_ != Phase::Streaming) return false;
            const float t = std::chrono::duration<float>(
                                std::chrono::steady_clock::now() - speakStart_)
                                .count();
            if (sphere_) sphere_->setLevel(tts_->level(t));
            return true;
        },
        50);
}

void VoiceView::stopSpeakAnimator() { speakAnim_.disconnect(); }

// -- DebugShell hooks -----------------------------------------------------

std::string VoiceView::debugPhaseName() const {
    switch (phase_) {
        case Phase::Setup: return "setup";
        case Phase::Idle: return "idle";
        case Phase::Listening: return "listening";
        case Phase::Thinking: return "thinking";
        case Phase::Streaming: return "streaming";
    }
    return "?";
}

std::string VoiceView::debugStatusText() const {
    return status_ ? std::string(status_->get_text()) : std::string();
}

void VoiceView::debugSay(const std::string& text) {
    // Inject as if STT had produced it — same path as sendTranscript's
    // post-STT branch, minus the microphone.
    if (text.empty() || phase_ == Phase::Setup || phase_ == Phase::Thinking)
        return;
    if (sphere_) sphere_->setLevel(0.0f);
    phase_ = Phase::Thinking;
    ++turnGen_;
    narratedLive_.clear();
    finalText_.clear();
    finalCredit_ = 0;
    tts_->applyVoice(voiceNameFor(agent_.id));
    if (sphere_) sphere_->setState(SphereState::Processing);
    if (talk_) {
        talk_->set_label("···");
        talk_->set_sensitive(false);
        talk_->remove_css_class("talk-active");
    }
    if (cancel_) cancel_->set_visible(true);
    if (status_) status_->set_label("Sending…");
    lastProgressMs_ = nowMs();
    lastPartialLen_ = 0;
    netFailSince_ = 0;
    post([this, text] { postTranscript(text); });
}

void VoiceView::debugSetSphere(SphereState state, float level) {
    if (sphere_) {
        sphere_->setState(state);
        sphere_->setLevel(level);
    }
}

void VoiceView::debugSetSphereLevel(float level) {
    if (sphere_) sphere_->setLevel(level);
}

std::string VoiceView::debugSphereStats() const {
    return sphere_ ? sphere_->debugStats() : std::string("no sphere widget");
}

void VoiceView::debugSphereShot(const std::string& path) {
    if (sphere_) sphere_->queueShot(path);
}

}  // namespace hv
