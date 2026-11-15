#include "Avatars.h"

#include <gdkmm/general.h>
#include <giomm/memoryinputstream.h>
#include <glibmm/bytes.h>

#include <cstring>

extern "C" {
GResource* voice_resources_get_resource(void);
#include <gio/gio.h>
}

namespace hv {

namespace {

std::string resourceText(const std::string& path) {
    // C API with nullptr GError: NULL on a missing resource, NEVER throws.
    GBytes* bytes =
        g_resources_lookup_data(path.c_str(), G_RESOURCE_LOOKUP_FLAGS_NONE,
                                nullptr);
    if (!bytes) return {};
    gsize size = 0;
    const char* data =
        static_cast<const char*>(g_bytes_get_data(bytes, &size));
    std::string out(data, size);
    g_bytes_unref(bytes);
    return out;
}

bool resourceExists(const std::string& name) {
    return !resourceText("/org/hivemind/voice/avatars/" + name + ".svg")
                .empty();
}

}  // namespace

const std::vector<std::string>& avatarNames() {
    static const std::vector<std::string> names = {
        "queen-bee",   "worker-bee", "drone-bee",   "honey-bee",
        "bumblebee",   "ladybug",    "butterfly",   "blue-butterfly",
        "dragonfly",   "firefly",    "caterpillar", "snail",
        "ant",         "beetle",     "spider",      "moth",
        "grasshopper", "worm",       "mantis",      "cricket",
    };
    return names;
}

const std::string& autoAvatar(const std::string& name) {
    return avatarNames()[std::hash<std::string>{}(name) % avatarNames().size()];
}

std::string resolveAvatar(const std::string& name, const std::string& field) {
    std::string res = field.empty() ? autoAvatar(name) : field;
    if (!resourceExists(res)) res = autoAvatar(name);
    if (!resourceExists(res)) res = "user";
    return res;
}

Glib::RefPtr<Gdk::Pixbuf> avatarPixbuf(const std::string& resolved, int size) {
    static const char* kBlank =
        "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 1 1'/>";
    std::string svg =
        resourceText("/org/hivemind/voice/avatars/" + resolved + ".svg");
    if (svg.empty()) svg = kBlank;
    auto stream = Gio::MemoryInputStream::create();
    stream->add_bytes(Glib::Bytes::create(svg.data(), svg.size()));
    try {
        return Gdk::Pixbuf::create_from_stream_at_scale(stream, size, size,
                                                        true);
    } catch (const Glib::Error&) {
        auto retry = Gio::MemoryInputStream::create();
        retry->add_bytes(Glib::Bytes::create(kBlank, std::strlen(kBlank)));
        try {
            return Gdk::Pixbuf::create_from_stream_at_scale(retry, size, size,
                                                            true);
        } catch (const Glib::Error&) {
            return Gdk::Pixbuf::create(Gdk::Colorspace::RGB, true, 8, 1, 1);
        }
    }
}

}  // namespace hv
