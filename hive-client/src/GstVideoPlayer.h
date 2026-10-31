#pragma once

#include <gdkmm/paintable.h>
#include <glibmm/refptr.h>
#include <gst/gst.h>

#include <functional>
#include <string>

namespace hive {

/// One persistent playbin + gtk4paintablesink for all inline video cards.
/// Reusing the pipeline avoids GTK MediaFile's per-instance FD/GL-thread
/// growth. All methods and bus callbacks run on the GTK main context.
class GstVideoPlayer {
public:
    GstVideoPlayer();
    ~GstVideoPlayer();

    GstVideoPlayer(const GstVideoPlayer&) = delete;
    GstVideoPlayer& operator=(const GstVideoPlayer&) = delete;

    bool available() const {
        return playbin_ && sink_ && paintable_ && bus_ && bus_watch_id_ != 0;
    }
    bool playing() const { return playing_; }
    const std::string& last_error() const { return last_error_; }
    Glib::RefPtr<Gdk::Paintable> paintable() const { return paintable_; }

    void play(const std::string& file);
    void toggle_pause();
    void stop();

    void set_on_eos(std::function<void()> cb) { on_eos_ = std::move(cb); }
    void set_on_error(std::function<void(std::string)> cb) {
        on_error_ = std::move(cb);
    }

private:
    static gboolean on_bus(GstBus* bus, GstMessage* message, gpointer data);

    GstElement* playbin_ = nullptr;
    GstElement* sink_ = nullptr;
    GstBus* bus_ = nullptr;
    guint bus_watch_id_ = 0;
    Glib::RefPtr<Gdk::Paintable> paintable_;
    bool playing_ = false;
    std::string last_error_;
    std::function<void()> on_eos_;
    std::function<void(std::string)> on_error_;
};

} // namespace hive
