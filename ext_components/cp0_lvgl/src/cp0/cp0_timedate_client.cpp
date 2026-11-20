#include "cp0_timedate_client.hpp"
#include <gio/gio.h>

namespace {
constexpr const char *bus = "org.freedesktop.timedate1";
constexpr const char *path = "/org/freedesktop/timedate1";
constexpr const char *iface = "org.freedesktop.timedate1";
GDBusConnection *connection(GError **e) { return g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, e); }
GVariant *get_property(GDBusConnection *c, const char *name, GError **e) {
    return g_dbus_connection_call_sync(c, bus, path, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", iface, name), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 10000, nullptr, e);
}
}
int cp0_timedate_get_ntp(int *enabled) {
    if (!enabled) return -22; GError *e = nullptr; auto *c = connection(&e); if (!c) { g_clear_error(&e); return -1; }
    auto *r = get_property(c, "NTP", &e); GVariant *v = nullptr;
    if (r) g_variant_get(r, "(v)", &v);
    const bool ok = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN);
    if (ok) *enabled = g_variant_get_boolean(v);
    if (v) g_variant_unref(v); if (r) g_variant_unref(r); g_clear_error(&e); g_object_unref(c); return ok ? 0 : -1;
}
