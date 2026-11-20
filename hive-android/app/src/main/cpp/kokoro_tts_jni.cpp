/* JNI bridge to audio.cpp's C API for Kokoro 82M on-device synthesis.
 * One process-wide registry + model + session (the model is 203MB BF16 —
 * never reload per utterance). All calls serialized on g_mu: the C API
 * session carries per-request preparation state and is not thread-safe.
 * Synthesis is offline per text chunk (VoiceActivity already splits
 * replies into sentences), returning 24 kHz mono float PCM.
 *
 * All 54 Kokoro voice packs live INSIDE the GGUF — no embeddings dir.
 * The text frontend language must match the voice prefix (af/am=en-us,
 * bf/bm=en-gb), so it is set per request from the voice id. */

#include <jni.h>
#include <android/log.h>
#include <audiocpp.h>

#include "loudness.h"

#include <mutex>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>

#define LOG_TAG "KokoroTts"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

namespace {

std::mutex           g_mu;
audiocpp_registry*   g_registry = nullptr;
audiocpp_model*      g_model    = nullptr;
audiocpp_session*    g_session  = nullptr;
int                  g_rate     = 24000;

const char* status_or_ok(audiocpp_status st) {
    return st == AUDIOCPP_OK ? "ok" : audiocpp_status_string(st);
}

} // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_hivemind_voice_KokoroTts_nativeInit(JNIEnv* env, jobject /*thiz*/,
                                             jstring modelPath, jint threads) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_session) return JNI_TRUE;   // idempotent

    const char* path = env->GetStringUTFChars(modelPath, nullptr);

    // Point the engine at the bundled eSpeak data explicitly. Its env
    // override (AUDIOCPP_ESPEAK_DATA) is checked FIRST, ahead of the
    // install-location search — the Termux-built library otherwise falls
    // back to its own nonexistent build-time data path on Android.
    {
        std::string data_dir = path;
        const auto slash = data_dir.find_last_of('/');
        if (slash != std::string::npos) data_dir.resize(slash);
        data_dir += "/espeak-ng-data";
        setenv("AUDIOCPP_ESPEAK_DATA", data_dir.c_str(), 1);
    }

    audiocpp_status st = audiocpp_registry_create(nullptr, &g_registry);
    if (st != AUDIOCPP_OK) {
        LOGE("registry_create: %s", status_or_ok(st));
        env->ReleaseStringUTFChars(modelPath, path);
        return JNI_FALSE;
    }

    audiocpp_options* load_opts = audiocpp_options_create();
    audiocpp_options_set(load_opts, "language", "en-us");
    audiocpp_model_config cfg{};
    cfg.family_hint = "kokoro_tts";
    st = audiocpp_model_load(g_registry, path, &cfg, load_opts, &g_model);
    audiocpp_options_free(load_opts);
    env->ReleaseStringUTFChars(modelPath, path);
    if (st != AUDIOCPP_OK) {
        LOGE("model_load: %s (%s)", status_or_ok(st), audiocpp_last_error());
        return JNI_FALSE;
    }

    audiocpp_backend_config backend{};
    backend.backend = "cpu";
    backend.device  = 0;
    backend.threads = threads > 0 ? threads : 4;
    audiocpp_options* sess_opts = audiocpp_options_create();
    audiocpp_options_set(sess_opts, "language", "en-us");
    st = audiocpp_session_create(g_model, "tts", "offline", &backend,
                                 sess_opts, &g_session);
    audiocpp_options_free(sess_opts);
    if (st != AUDIOCPP_OK) {
        LOGE("session_create: %s (%s)", status_or_ok(st), audiocpp_last_error());
        return JNI_FALSE;
    }
    LOGI("ready: %s (%s)", audiocpp_build_version(),
         audiocpp_model_description(g_model));
    return JNI_TRUE;
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_hivemind_voice_KokoroTts_nativeSynthesize(JNIEnv* env, jobject /*thiz*/,
                                                   jstring text, jstring voiceId) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_session) { LOGE("synthesize before init"); return nullptr; }

    const char* t = env->GetStringUTFChars(text, nullptr);
    const char* v = voiceId ? env->GetStringUTFChars(voiceId, nullptr) : nullptr;

    // Kokoro's text frontend language must match the voice prefix.
    const char* lang = "en-us";
    if (v && (strncmp(v, "bf_", 3) == 0 || strncmp(v, "bm_", 3) == 0))
        lang = "en-gb";

    audiocpp_request* req = audiocpp_request_create();
    audiocpp_request_set_text(req, t, lang);
    if (v && *v) audiocpp_request_set_voice_id(req, v);
    env->ReleaseStringUTFChars(text, t);
    if (v) env->ReleaseStringUTFChars(voiceId, v);

    audiocpp_result* result = nullptr;
    audiocpp_status st = audiocpp_session_run(g_session, req, &result);
    audiocpp_request_free(req);
    if (st != AUDIOCPP_OK || !result) {
        LOGE("session_run: %s (%s)", status_or_ok(st), audiocpp_last_error());
        if (result) audiocpp_result_free(result);
        return nullptr;
    }

    const float* samples = nullptr;
    size_t frames = 0;
    int rate = 0, channels = 0;
    st = audiocpp_result_audio(result, &samples, &frames, &rate, &channels);
    if (st != AUDIOCPP_OK || !samples || frames == 0) {
        LOGE("result_audio: %s", status_or_ok(st));
        audiocpp_result_free(result);
        return nullptr;
    }
    g_rate = rate > 0 ? rate : 24000;

    // Normalize a private copy: the result API exposes const samples.
    const size_t n = frames * channels;
    std::vector<float> normalized(samples, samples + n);
    const auto loudness = kokorotts::normalizeLoudness(normalized.data(),
                                                        normalized.size());
    LOGD("loudness raw_rms=%.5f gain=%.3f out_rms=%.5f peak=%.3f",
         loudness.inputRms, loudness.gain, loudness.outputRms,
         loudness.outputPeak);

    jfloatArray out = env->NewFloatArray(static_cast<jsize>(frames * channels));
    if (out) {
        env->SetFloatArrayRegion(out, 0, static_cast<jsize>(frames * channels),
                                 normalized.data());
    }
    audiocpp_result_free(result);
    return out;   // nullptr on OOM — Kotlin falls back to system TTS
}

extern "C" JNIEXPORT jint JNICALL
Java_com_hivemind_voice_KokoroTts_nativeSampleRate(JNIEnv*, jobject) {
    return g_rate;
}

extern "C" JNIEXPORT void JNICALL
Java_com_hivemind_voice_KokoroTts_nativeShutdown(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_session)  { audiocpp_session_free(g_session);   g_session = nullptr; }
    if (g_model)    { audiocpp_model_free(g_model);       g_model = nullptr; }
    if (g_registry) { audiocpp_registry_free(g_registry); g_registry = nullptr; }
}
