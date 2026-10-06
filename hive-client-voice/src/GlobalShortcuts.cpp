#include "GlobalShortcuts.h"

#include "Prefs.h"

#include <cstdio>
#include <random>

namespace hv {

namespace {
constexpr auto kPortalDest = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kIface = "org.freedesktop.portal.GlobalShortcuts";
constexpr auto kRequestIface = "org.freedesktop.portal.Request";
constexpr auto kShortcutId = "push-to-talk";

std::string newToken() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> d(0, 0xffffff);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06x", d(gen));
    return buf;
}

// Pull a string value out of an a{sv} map variant.
std::string mapGetString(const Glib::VariantBase& mapV, const char* key) {
    if (!mapV || mapV.get_type_string() != std::string("a{sv}")) return {};
    auto mv = Glib::VariantBase::cast_dynamic<Glib::Variant<
        std::map<std::string, Glib::VariantBase>>>(mapV);
    if (!mv) return {};
    const auto m = mv.get();
    const auto it = m.find(key);
    if (it == m.end()) return {};
    auto sv = Glib::VariantBase::cast_dynamic<Glib::Variant<std::string>>(
        it->second);
    return sv ? sv.get() : std::string();
}

template <typename T>
T getChild(const Glib::VariantContainerBase& c, gsize i) {
    auto v = Glib::VariantBase::cast_dynamic<Glib::Variant<T>>(c.get_child(i));
    return v ? v.get() : T();
}
}  // namespace

GlobalShortcuts::GlobalShortcuts(std::function<void()> onActivated)
    : onActivated_(std::move(onActivated)) {
    try {
        bus_ = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SESSION);
    } catch (const Glib::Error& e) {
        std::fprintf(stderr, "hive-voice: global shortcuts: no session bus: "
                             "%s\n",
                     e.what());
    }
}

GlobalShortcuts::~GlobalShortcuts() {
    if (bus_ && subActivated_) bus_->signal_unsubscribe(subActivated_);
}

void GlobalShortcuts::bind(const std::string& trigger) {
    if (!bus_) return;
    pendingBindTrigger_ = trigger;
    createSession();
}

void GlobalShortcuts::createSession() {
    // Reuse the persisted token when present — the portal restores the
    // prior session silently instead of showing a new permission dialog.
    Prefs probe;
    probe.load();
    sessionToken_ = probe.globalShortcutToken();
    if (sessionToken_.empty()) sessionToken_ = newToken();

    // Raw C API: glibmm's variant-type deduction mangles nested a{sv}
    // maps, so build parameters where every type is explicit.
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
    const std::string ht = newToken();
    g_variant_builder_add(&b, "{sv}", "handle_token",
                          g_variant_new_string(ht.c_str()));
    g_variant_builder_add(&b, "{sv}", "session_handle_token",
                          g_variant_new_string(sessionToken_.c_str()));
    GVariant* options = g_variant_builder_end(&b);
    GVariant* args[] = {options};
    const auto params =
        Glib::VariantContainerBase(g_variant_new_tuple(args, 1), false);

    std::string requestPath;
    try {
        auto result = bus_->call_sync(kPortalPath, kIface, "CreateSession",
                                      params, kPortalDest);
        requestPath = getChild<std::string>(result, 0);
    } catch (const Glib::Error& e) {
        std::fprintf(stderr, "hive-voice: CreateSession failed: %s\n",
                     e.what());
        return;
    }

    // Response(u response, a{sv} results) on the request object; the
    // new session's handle arrives in results["session_handle"].
    bus_->signal_subscribe(
        [this](const Glib::RefPtr<Gio::DBus::Connection>&,
               const Glib::ustring&, const Glib::ustring&,
               const Glib::ustring&, const Glib::ustring&,
               const Glib::VariantContainerBase& p) {
            const guint response = getChild<guint>(p, 0);
            if (response != 0) {
                std::fprintf(stderr,
                             "hive-voice: global shortcut session denied "
                             "(%u) — allow it in Settings > Keyboard > "
                             "Keyboard Shortcuts\n",
                             response);
                return;
            }
            sessionHandle_ =
                mapGetString(p.get_child(1), "session_handle");
            bindShortcuts();
        },
        kPortalDest, kRequestIface, "Response", requestPath);
}

void GlobalShortcuts::bindShortcuts() {
    if (binding_ || sessionHandle_.empty()) return;
    binding_ = true;

    // shortcuts: a(sa{sv}) — id + {description, trigger}. No "deck"
    // option: plain trigger, Activated on each press (toggle semantics).
    GVariantBuilder desc;
    g_variant_builder_init(&desc, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&desc, "{sv}", "description",
                          g_variant_new_string("HiveMind Voice: talk"));
    g_variant_builder_add(&desc, "{sv}", "trigger",
                          g_variant_new_string(pendingBindTrigger_.c_str()));
    GVariant* descV = g_variant_builder_end(&desc);

    GVariantBuilder shortcuts;
    g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
    g_variant_builder_add(&shortcuts, "(s@a{sv})", kShortcutId, descV);

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
    const std::string bt = newToken();
    g_variant_builder_add(&opts, "{sv}", "handle_token",
                          g_variant_new_string(bt.c_str()));
    GVariant* optsV = g_variant_builder_end(&opts);

    GVariant* args[] = {g_variant_new_object_path(sessionHandle_.c_str()),
                        g_variant_builder_end(&shortcuts),
                        g_variant_new_string(""), optsV};
    const auto params =
        Glib::VariantContainerBase(g_variant_new_tuple(args, 4), false);

    std::string requestPath;
    try {
        auto result = bus_->call_sync(kPortalPath, kIface, "BindShortcuts",
                                      params, kPortalDest);
        requestPath = getChild<std::string>(result, 0);
    } catch (const Glib::Error& e) {
        std::fprintf(stderr, "hive-voice: BindShortcuts failed: %s\n",
                     e.what());
        binding_ = false;
        return;
    }

    // Bind result.
    bus_->signal_subscribe(
        [this](const Glib::RefPtr<Gio::DBus::Connection>&,
               const Glib::ustring&, const Glib::ustring&,
               const Glib::ustring&, const Glib::ustring&,
               const Glib::VariantContainerBase& p) {
            const guint r = getChild<guint>(p, 0);
            if (r == 0) {
                std::fprintf(stderr, "hive-voice: global shortcut bound: "
                                     "%s\n",
                             pendingBindTrigger_.c_str());
                // Persist the token ONLY after a successful bind — an
                // unbound token would skip the dialog next launch.
                Prefs p2;
                p2.load();
                p2.setGlobalShortcutToken(sessionToken_);
                p2.save();
            } else {
                std::fprintf(stderr, "hive-voice: BindShortcuts denied "
                                     "(%u)\n",
                             r);
            }
            binding_ = false;
        },
        kPortalDest, kRequestIface, "Response", requestPath);

    // Keypress: Activated(o session, s shortcut_id, t timestamp, a{sv}).
    subActivated_ = bus_->signal_subscribe(
        [this](const Glib::RefPtr<Gio::DBus::Connection>&,
               const Glib::ustring&, const Glib::ustring&,
               const Glib::ustring& path, const Glib::ustring&,
               const Glib::VariantContainerBase& p) {
            if (path != sessionHandle_) return;
            if (getChild<std::string>(p, 1) != kShortcutId) return;
            // D-Bus thread — hop to GTK main thread for widget work.
            Glib::signal_idle().connect_once(
                [cb = onActivated_] { if (cb) cb(); });
        },
        kPortalDest, kIface, "Activated", sessionHandle_);
}

}  // namespace hv
