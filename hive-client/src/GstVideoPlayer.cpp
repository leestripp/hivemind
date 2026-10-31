#include "GstVideoPlayer.h"

#include <glibmm.h>

#include <cstdio>
#include <utility>

namespace hive {

GstVideoPlayer::GstVideoPlayer() {
    GError* init_error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &init_error)) {
        last_error_ = init_error ? init_error->message : "GStreamer init failed";
        if (init_error) g_error_free(init_error);
        return;
    }

    playbin_ = gst_element_factory_make("playbin", "hive-video-playbin");
    sink_ = gst_element_factory_make("gtk4paintablesink", "hive-video-sink");
    if (!playbin_ || !sink_) {
        last_error_ = "playbin or gtk4paintablesink is unavailable";
        return;
    }

    g_object_set(playbin_, "video-sink", sink_, nullptr);
    GdkPaintable* raw_paintable = nullptr;
    g_object_get(sink_, "paintable", &raw_paintable, nullptr);
    if (!raw_paintable) {
        last_error_ = "gtk4paintablesink provided no paintable";
        return;
    }
    // g_object_get transferred one reference; Glib::wrap adopts it.
    paintable_ = Glib::wrap(raw_paintable, false);

    bus_ = gst_element_get_bus(playbin_);
    if (!bus_) {
        last_error_ = "playbin provided no message bus";
        return;
    }
    bus_watch_id_ = gst_bus_add_watch(bus_, &GstVideoPlayer::on_bus, this);
}

GstVideoPlayer::~GstVideoPlayer() {
    playing_ = false;
    if (bus_watch_id_ != 0) g_source_remove(bus_watch_id_);
    if (playbin_) gst_element_set_state(playbin_, GST_STATE_NULL);
    if (bus_) gst_object_unref(bus_);
    if (playbin_) gst_object_unref(playbin_);
    if (sink_) gst_object_unref(sink_);
    paintable_.reset();
}

void GstVideoPlayer::play(const std::string& file) {
    if (!available()) return;
    last_error_.clear();
    // Official playbin lifecycle: READY before replacing the URI. Keeping
    // the same playbin/sink (rather than constructing a new one) preserves
    // the GL context and prevents resource growth across track changes.
    gst_element_set_state(playbin_, GST_STATE_READY);
    const std::string uri = Glib::filename_to_uri(file);
    g_object_set(playbin_, "uri", uri.c_str(), nullptr);
    playing_ = gst_element_set_state(playbin_, GST_STATE_PLAYING) !=
               GST_STATE_CHANGE_FAILURE;
}

void GstVideoPlayer::toggle_pause() {
    if (!available()) return;
    playing_ = !playing_;
    gst_element_set_state(playbin_, playing_ ? GST_STATE_PLAYING
                                             : GST_STATE_PAUSED);
}

void GstVideoPlayer::stop() {
    playing_ = false;
    if (playbin_) gst_element_set_state(playbin_, GST_STATE_READY);
}

gboolean GstVideoPlayer::on_bus(GstBus*, GstMessage* message, gpointer data) {
    auto& self = *static_cast<GstVideoPlayer*>(data);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
        self.playing_ = false;
        if (self.on_eos_) self.on_eos_();
        break;
    case GST_MESSAGE_ERROR: {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        self.last_error_ = error ? error->message : "unknown video error";
        std::fprintf(stderr, "[hive-client] video: %s (%s)\n",
                     self.last_error_.c_str(), debug ? debug : "no debug");
        if (error) g_error_free(error);
        if (debug) g_free(debug);
        self.playing_ = false;
        if (self.on_error_) self.on_error_(self.last_error_);
        break;
    }
    default:
        break;
    }
    return G_SOURCE_CONTINUE;
}

} // namespace hive
