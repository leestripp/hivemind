#include "GlobalShortcuts.h"

#include <gio/gio.h>

#include <cstdio>
#include <optional>
#include <random>

namespace hv {

namespace {
constexpr auto kPortalDest = "org.freedesktop.portal.Desktop";
constexpr auto kPortalPath = "/org/freedesktop/portal/desktop";
constexpr auto kIface = "org.freedesktop.portal.GlobalShortcuts";
constexpr auto kAppId = "org.hivemind.Client";
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

// Open an independent session-bus connection so Registry.Register controls
// this client's portal identity instead of inheriting GTK's shared-bus identity.
Glib::RefPtr<Gio::DBus::Connection> openPrivateSessionBus() {
    GError* error = nullptr;
    char* address =
        g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!address) throw Glib::Error(error);

    constexpr auto flags = static_cast<GDBusConnectionFlags>(
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION);
    GDBusConnection* connection = g_dbus_connection_new_for_address_sync(
        address, flags, nullptr, nullptr, &error);
    g_free(address);
    if (!connection) throw Glib::Error(error);
    // Glib::wrap(..., take_copy=false) adopts the returned full reference.
    return Glib::wrap(connection);
}

// Bind Response results["shortcuts"] is the authoritative subset that the
// portal actually bound. GNOME supplies trigger_description for each entry.
// nullopt means our shortcut is absent; an engaged empty string means it is
// present but supplied no human-readable trigger description.
std::optional<std::string> boundTriggerDescription(
    const Glib::VariantContainerBase& response) {
    auto results = Glib::VariantBase::cast_dynamic<Glib::Variant<
        std::map<std::string, Glib::VariantBase>>>(response.get_child(1));
    if (!results) return std::nullopt;
    const auto m = results.get();
    const auto it = m.find("shortcuts");
    if (it == m.end()) return std::nullopt;
    const auto shortcuts =
        Glib::VariantBase::cast_dynamic<Glib::VariantContainerBase>(
            it->second);
    if (!shortcuts) return std::nullopt;
    for (gsize i = 0; i < shortcuts.get_n_children(); ++i) {
        const auto entry =
            Glib::VariantBase::cast_dynamic<Glib::VariantContainerBase>(
                shortcuts.get_child(i));
        if (!entry || getChild<std::string>(entry, 0) != kShortcutId)
            continue;
        return mapGetString(entry.get_child(1), "trigger_description");
    }
    return std::nullopt;
}
}  // namespace

GlobalShortcuts::GlobalShortcuts(std::function<void()> onActivated)
    : onActivated_(std::move(onActivated)) {
    try {
        // A dedicated connection is required: Registry.Register is
        // connection-scoped, and GTK may already have associated the shared
        // g_bus_get_sync() connection with a different portal app identity.
        bus_ = openPrivateSessionBus();
    } catch (const Glib::Error& e) {
        std::fprintf(stderr, "hive-voice: global shortcuts: no session bus: "
                             "%s\n",
                     e.what());
    }
}

GlobalShortcuts::~GlobalShortcuts() {
    close();
}

void GlobalShortcuts::close() {
    if (closed_) return;
    closed_ = true;
    bound_ = false;
    if (!bus_) return;
    for (const guint id : subscriptions_) bus_->signal_unsubscribe(id);
    subscriptions_.clear();
    if (sessionHandle_.empty()) return;
    try {
        const auto params = Glib::VariantContainerBase(
            g_variant_new_tuple(nullptr, 0), false);
        bus_->call_sync(sessionHandle_, "org.freedesktop.portal.Session",
                        "Close", params, kPortalDest);
        std::fprintf(stderr, "hive-voice: global shortcut closed\n");
    } catch (const Glib::Error& e) {
        std::fprintf(stderr, "hive-voice: closing global shortcut failed: %s\n",
                     e.what());
    }
}

void GlobalShortcuts::bind(const std::string& preferredTrigger) {
    if (!bus_ || closed_) return;
    bound_ = false;
    pendingBindTrigger_ = preferredTrigger;
    createSession();
}

void GlobalShortcuts::createSession() {
    // xdg-desktop-portal 1.20+ supports explicit host-app registration and
    // current GNOME requires a desktop-file-backed reverse-DNS app id. The
    // legacy systemd-scope identity can otherwise select a different app's
    // saved binding when the client is launched by another host application.
    // Keep failure non-fatal for older portals without the Registry API.
    try {
        GVariantBuilder registryOptions;
        g_variant_builder_init(&registryOptions, G_VARIANT_TYPE_VARDICT);
        const auto params = Glib::VariantContainerBase(
            g_variant_new("(sa{sv})", kAppId, &registryOptions));
        bus_->call_sync(kPortalPath, "org.freedesktop.host.portal.Registry",
                        "Register", params, kPortalDest);
        std::fprintf(stderr,
                     "hive-voice: portal host app registered: %s\n", kAppId);
    } catch (const std::exception& e) {
        std::fprintf(stderr,
                     "hive-voice: portal host app registration unavailable; "
                     "using launch identity: %s\n",
                     e.what());
    }

    // The token identifies only this live D-Bus session. GNOME persists the
    // approved binding by registered app id, so a fresh live session restores
    // it without another permission prompt.
    sessionToken_ = newToken();

    // Raw C API: glibmm's variant-type deduction mangles nested a{sv}
    // maps, so build parameters where every type is explicit.
    // Both tokens are required: handle_token names the CreateSession Request;
    // session_handle_token names the resulting Session. xdg-desktop-portal
    // 1.21.1 asserts if session_handle_token is absent. These tokens are not
    // substitutes for GNOME's per-app-id persisted shortcut grant.
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
    subscriptions_.push_back(bus_->signal_subscribe(
        [this](const Glib::RefPtr<Gio::DBus::Connection>&,
               const Glib::ustring&, const Glib::ustring&,
               const Glib::ustring&, const Glib::ustring&,
               const Glib::VariantContainerBase& p) {
            if (closed_) return;
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
        kPortalDest, kRequestIface, "Response", requestPath));
}

void GlobalShortcuts::bindShortcuts() {
    if (closed_ || binding_ || sessionHandle_.empty()) return;
    binding_ = true;

    // WARNING: this is the XDG wire format, NOT a GTK accelerator.
    // shortcuts is a(sa{sv}) — id + {description, preferred_trigger}.
    // preferred_trigger uses freedesktop XKB syntax (LOGO+ALT+BackSpace),
    // not GTK syntax (<Super><Alt>BackSpace). The old "trigger" key and GTK
    // string were silently discarded by the portal, so a successful Bind
    // response did not necessarily mean any physical key had been grabbed.
    GVariantBuilder desc;
    g_variant_builder_init(&desc, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&desc, "{sv}", "description",
                          g_variant_new_string("HiveMind Voice: talk"));
    g_variant_builder_add(&desc, "{sv}", "preferred_trigger",
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
    subscriptions_.push_back(bus_->signal_subscribe(
        [this](const Glib::RefPtr<Gio::DBus::Connection>&,
               const Glib::ustring&, const Glib::ustring&,
               const Glib::ustring&, const Glib::ustring&,
               const Glib::VariantContainerBase& p) {
            if (closed_) return;
            const guint r = getChild<guint>(p, 0);
            if (r == 0) {
                const auto description = boundTriggerDescription(p);
                if (!description) {
                    std::fprintf(stderr,
                                 "hive-voice: BindShortcuts succeeded but "
                                 "returned no push-to-talk binding\n");
                    binding_ = false;
                    return;
                }
                bound_ = true;
                if (description->empty()) {
                    std::fprintf(stderr,
                                 "hive-voice: push-to-talk bound (portal "
                                 "returned no trigger description)\n");
                } else {
                    std::fprintf(stderr,
                                 "hive-voice: global shortcut bound: %s\n",
                                 description->c_str());
                }
            } else {
                std::fprintf(stderr, "hive-voice: BindShortcuts denied "
                                     "(%u)\n",
                             r);
            }
            binding_ = false;
        },
        kPortalDest, kRequestIface, "Response", requestPath));

    // Keypress: Activated(o session, s shortcut_id, t timestamp, a{sv}).
    subscriptions_.push_back(bus_->signal_subscribe(
        [this](
            const Glib::RefPtr<Gio::DBus::Connection>& /*connection*/,
            const Glib::ustring& /*senderName*/,
            const Glib::ustring& objectPath,
            const Glib::ustring& /*interfaceName*/,
            const Glib::ustring& /*signalName*/,
            const Glib::VariantContainerBase& p) {
            // WARNING: giomm SlotSignal order is connection, sender_name,
            // object_path, interface_name, signal_name, parameters. The old
            // code treated interface_name as the path and rejected every key.
            // Keep the parameter names above in sync with this order.
            // Activated is on the portal ROOT object, not the Session object;
            // parameters[0], not the signal's object path, names the Session.
            const auto activatedSession = getChild<std::string>(p, 0);
            const auto activatedId = getChild<std::string>(p, 1);
            std::fprintf(stderr,
                         "hive-voice: Activated callback path=%s session=%s "
                         "expected=%s id=%s\n",
                         objectPath.c_str(), activatedSession.c_str(),
                         sessionHandle_.c_str(), activatedId.c_str());
            if (closed_ || objectPath != kPortalPath) return;
            if (activatedSession != sessionHandle_) return;
            if (activatedId != kShortcutId) return;
            std::fprintf(stderr, "hive-voice: global shortcut activated\n");
            // Subscription was made on the GTK thread. Defer widget work to
            // an idle callback; HiveWindow's weak/exact-identity guard still
            // protects against a queued callback after voice teardown.
            Glib::signal_idle().connect_once(
                [cb = onActivated_] { if (cb) cb(); });
        },
        kPortalDest, kIface, "Activated", kPortalPath));
}

}  // namespace hv
