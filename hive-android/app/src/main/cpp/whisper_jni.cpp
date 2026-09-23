// whisper_jni.cpp — JNI bridge to whisper.cpp for SttEngine.
// TWO contexts: a tiny model for cheap live partials and a base model
// for the accuracy-critical final pass. Inference holds a global mutex
// (one decode at a time); no_context=true so windowed partials stay
// independent.
#include <jni.h>
#include <android/log.h>
#include <mutex>
#include <string>
#include <vector>

#include "whisper.h"

#define TAG "HiveWhisper"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

whisper_context* g_ctx[2] = {nullptr, nullptr};   // 0=partial, 1=final
std::string g_path[2];
std::mutex g_mu;

whisper_context* load(const std::string& path) {
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;   // CPU+NEON+dotprod; ggml GPU not built here
    return whisper_init_from_file_with_params(path.c_str(), cparams);
}

} // namespace

// which: 0 = partial model, 1 = final model
extern "C" JNIEXPORT jboolean JNICALL
Java_com_hivemind_voice_WhisperNative_nativeInit(JNIEnv* env, jclass,
                                                 jstring path, jint which) {
    const char* p = env->GetStringUTFChars(path, nullptr);
    std::string new_path(p ? p : "");
    if (p) env->ReleaseStringUTFChars(path, p);
    if (which < 0 || which > 1) return JNI_FALSE;

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_ctx[which] && new_path == g_path[which]) return JNI_TRUE;
    if (g_ctx[which]) { whisper_free(g_ctx[which]); g_ctx[which] = nullptr; }
    g_ctx[which] = load(new_path);
    if (!g_ctx[which]) {
        LOGE("whisper_init failed (%d): %s", which, new_path.c_str());
        return JNI_FALSE;
    }
    g_path[which] = new_path;
    LOGI("model %d loaded: %s", which, new_path.c_str());
    LOGI("system: %s", whisper_print_system_info());
    return JNI_TRUE;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_hivemind_voice_WhisperNative_nativeTranscribe(
        JNIEnv* env, jclass, jshortArray pcm, jint nSamples, jint nThreads,
        jboolean finalPass) {
    std::lock_guard<std::mutex> lk(g_mu);
    whisper_context* ctx = g_ctx[finalPass ? 1 : 0];
    if (!ctx) ctx = g_ctx[finalPass ? 0 : 1];   // fall back to the other
    if (!ctx) {
        LOGE("transcribe before init");
        return env->NewStringUTF("");
    }
    const jint n = std::min(nSamples, env->GetArrayLength(pcm));
    if (n <= 0) return env->NewStringUTF("");

    std::vector<short> tmp(n);
    env->GetShortArrayRegion(pcm, 0, n, tmp.data());
    std::vector<float> samples(n);
    for (jint i = 0; i < n; ++i) samples[i] = tmp[i] / 32768.0f;

    auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.print_realtime   = false;
    params.print_progress   = false;
    params.print_timestamps = false;
    params.print_special    = false;
    params.translate        = false;
    params.language         = "en";
    params.no_context       = true;
    params.single_segment   = !finalPass;
    params.n_threads        = nThreads;

    if (whisper_full(ctx, params, samples.data(), n) != 0) {
        LOGE("whisper_full failed");
        return env->NewStringUTF("");
    }
    std::string out;
    const int nseg = whisper_full_n_segments(ctx);
    for (int i = 0; i < nseg; ++i) {
        const char* t = whisper_full_get_segment_text(ctx, i);
        if (t) { if (!out.empty()) out += ' '; out += t; }
    }
    const auto b = out.find_first_not_of(' ');
    return env->NewStringUTF(b == std::string::npos ? "" : out.c_str() + b);
}

extern "C" JNIEXPORT void JNICALL
Java_com_hivemind_voice_WhisperNative_nativeFree(JNIEnv*, jclass) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int i = 0; i < 2; ++i) {
        if (g_ctx[i]) { whisper_free(g_ctx[i]); g_ctx[i] = nullptr; }
        g_path[i].clear();
    }
}
