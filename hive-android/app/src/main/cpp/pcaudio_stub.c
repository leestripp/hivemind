/* Stub libpcaudio.so (pcaudiolib API): eSpeak-ng's DT_NEEDED pulls this in
 * and it imports exactly these audio_object_* symbols — but ONLY for audio
 * playback, which audio.cpp's Kokoro phonemizer never performs. No-op
 * implementations satisfy the dynamic linker without Termux's PulseAudio
 * dependency chain. Signatures mirror pcaudiolib's audio_object.h. */
#include <stddef.h>

void *audio_object_open(const char *device, const char *application_name,
                        const char *description) {
    (void)device; (void)application_name; (void)description;
    return NULL;
}
void audio_object_close(void *object) { (void)object; }
void audio_object_destroy(void *object) { (void)object; }
int audio_object_write(void *object, const void *data, size_t bytes) {
    (void)object; (void)data; (void)bytes;
    return 0;
}
int audio_object_drain(void *object) { (void)object; return 0; }
int audio_object_flush(void *object) { (void)object; return 0; }
const char *audio_object_strerror(void *object, int error) {
    (void)object; (void)error;
    return "pcaudio stub (no playback on Android)";
}

/* pcaudiolib's device factory (playback only — never called by the
 * phonemizer). */
void *create_audio_device_object(const char *device,
                                 const char *application_name,
                                 const char *description) {
    (void)device; (void)application_name; (void)description;
    return NULL;
}
