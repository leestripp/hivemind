// Avatars — the shared hive-garden creature set (SVGs bundled from
// hive-client/data/avatars). Resolution mirrors Avatars.kt /
// hive-client make_avatar 1:1: stored field wins, "" or stale → stable
// name-hash into the set, final fallback "user". Never renders nothing.
#pragma once

#include <gdkmm/pixbuf.h>
#include <glibmm/refptr.h>

#include <string>
#include <vector>

namespace hv {

// The 20-creature set (bees first — brand), same order as Avatars.kt.
const std::vector<std::string>& avatarNames();

// Stable MurmurHash64A of `name` into the set — identical to the
// Android/desktop clients for the same name (std::hash on libstdc++ is
// MurmurHash64A, the constant Avatars.kt was verified against).
const std::string& autoAvatar(const std::string& name);

// Full resolution chain for an agent (field may be "").
std::string resolveAvatar(const std::string& name, const std::string& field);

// Render an avatar SVG from the gresource at `size` px; returns a blank
// transparent pixbuf if the resource is missing or unparseable.
Glib::RefPtr<Gdk::Pixbuf> avatarPixbuf(const std::string& resolved, int size);

}  // namespace hv
