// fuse_app D-Bus access for the Unix shell services (notifications, StatusNotifierItem tray icons): the libdbus-1
// entry points used, loaded at runtime (no build dependency), plus small append / read helpers. Shared with the tray
// gate (it plays watcher, notification daemon and tray host on a private session bus).
#pragma once

#if !defined(_WIN32)
#include <dlfcn.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fuse::app::dbus {

struct Connection;
struct Message;
/// DBusMessageIter is caller-allocated; 128 bytes covers its 14-field layout on every ABI with room to spare.
struct Iter { alignas(8) unsigned char opaque[128]; };
struct Error { const char* name; const char* message; unsigned dummy; void* padding1; };
constexpr int kBusSession = 0;
enum : int { MethodCall = 1, MethodReturn = 2, ErrorReply = 3, Signal = 4 };
enum : int {
    TInvalid = 0, TByte = 'y', TBool = 'b', TInt32 = 'i', TUint32 = 'u', TString = 's', TPath = 'o', TArray = 'a',
    TVariant = 'v', TStruct = 'r', TDictEntry = 'e'
};
constexpr unsigned kNameFlagDoNotQueue = 4;

struct Lib {
    void* so = nullptr;
    int (*threadsInitDefault)() = nullptr;
    void (*errorInit)(Error*) = nullptr;
    void (*errorFree)(Error*) = nullptr;
    unsigned (*errorIsSet)(const Error*) = nullptr;
    Connection* (*busGet)(int, Error*) = nullptr;
    Connection* (*busGetPrivate)(int, Error*) = nullptr;
    const char* (*busUniqueName)(Connection*) = nullptr;
    int (*busRequestName)(Connection*, const char*, unsigned, Error*) = nullptr;
    int (*busReleaseName)(Connection*, const char*, Error*) = nullptr;
    unsigned (*busNameHasOwner)(Connection*, const char*, Error*) = nullptr;
    void (*busAddMatch)(Connection*, const char*, Error*) = nullptr;
    void (*connClose)(Connection*) = nullptr;
    void (*connUnref)(Connection*) = nullptr;
    void (*connFlush)(Connection*) = nullptr;
    unsigned (*connSend)(Connection*, Message*, std::uint32_t*) = nullptr;
    Message* (*sendBlocking)(Connection*, Message*, int, Error*) = nullptr;
    unsigned (*readWrite)(Connection*, int) = nullptr;
    Message* (*popMessage)(Connection*) = nullptr;
    unsigned (*unixFd)(Connection*, int*) = nullptr;
    void (*exitOnDisconnect)(Connection*, unsigned) = nullptr;
    Message* (*newCall)(const char*, const char*, const char*, const char*) = nullptr;
    Message* (*newReturn)(Message*) = nullptr;
    Message* (*newSignal)(const char*, const char*, const char*) = nullptr;
    Message* (*newError)(Message*, const char*, const char*) = nullptr;
    void (*unref)(Message*) = nullptr;
    int (*msgType)(Message*) = nullptr;
    const char* (*msgInterface)(Message*) = nullptr;
    const char* (*msgMember)(Message*) = nullptr;
    const char* (*msgPath)(Message*) = nullptr;
    const char* (*msgSender)(Message*) = nullptr;
    const char* (*msgErrorName)(Message*) = nullptr;
    std::uint32_t (*msgReplySerial)(Message*) = nullptr;
    unsigned (*iterInit)(Message*, Iter*) = nullptr;
    void (*iterInitAppend)(Message*, Iter*) = nullptr;
    unsigned (*appendBasic)(Iter*, int, const void*) = nullptr;
    unsigned (*appendFixedArray)(Iter*, int, const void*, int) = nullptr;
    unsigned (*openContainer)(Iter*, int, const char*, Iter*) = nullptr;
    unsigned (*closeContainer)(Iter*, Iter*) = nullptr;
    int (*argType)(Iter*) = nullptr;
    void (*getBasic)(Iter*, void*) = nullptr;
    unsigned (*iterNext)(Iter*) = nullptr;
    void (*recurse)(Iter*, Iter*) = nullptr;

    template <class F>
    bool sym(F& f, const char* name) {
        f = reinterpret_cast<F>(dlsym(so, name));
        return f != nullptr;
    }
    bool load() {
        if (so) return true;
        so = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
        if (!so) return false;
        const bool ok =
            sym(threadsInitDefault, "dbus_threads_init_default") && sym(errorInit, "dbus_error_init") && sym(errorFree, "dbus_error_free") &&
            sym(errorIsSet, "dbus_error_is_set") && sym(busGet, "dbus_bus_get") && sym(busGetPrivate, "dbus_bus_get_private") &&
            sym(busUniqueName, "dbus_bus_get_unique_name") && sym(busRequestName, "dbus_bus_request_name") &&
            sym(busReleaseName, "dbus_bus_release_name") && sym(busNameHasOwner, "dbus_bus_name_has_owner") &&
            sym(busAddMatch, "dbus_bus_add_match") && sym(connClose, "dbus_connection_close") && sym(connUnref, "dbus_connection_unref") &&
            sym(connFlush, "dbus_connection_flush") && sym(connSend, "dbus_connection_send") &&
            sym(sendBlocking, "dbus_connection_send_with_reply_and_block") && sym(readWrite, "dbus_connection_read_write") &&
            sym(popMessage, "dbus_connection_pop_message") && sym(unixFd, "dbus_connection_get_unix_fd") &&
            sym(exitOnDisconnect, "dbus_connection_set_exit_on_disconnect") && sym(newCall, "dbus_message_new_method_call") &&
            sym(newReturn, "dbus_message_new_method_return") && sym(newSignal, "dbus_message_new_signal") &&
            sym(newError, "dbus_message_new_error") && sym(unref, "dbus_message_unref") && sym(msgType, "dbus_message_get_type") &&
            sym(msgInterface, "dbus_message_get_interface") && sym(msgMember, "dbus_message_get_member") &&
            sym(msgPath, "dbus_message_get_path") && sym(msgSender, "dbus_message_get_sender") &&
            sym(msgErrorName, "dbus_message_get_error_name") && sym(msgReplySerial, "dbus_message_get_reply_serial") &&
            sym(iterInit, "dbus_message_iter_init") && sym(iterInitAppend, "dbus_message_iter_init_append") &&
            sym(appendBasic, "dbus_message_iter_append_basic") && sym(appendFixedArray, "dbus_message_iter_append_fixed_array") &&
            sym(openContainer, "dbus_message_iter_open_container") && sym(closeContainer, "dbus_message_iter_close_container") &&
            sym(argType, "dbus_message_iter_get_arg_type") && sym(getBasic, "dbus_message_iter_get_basic") &&
            sym(iterNext, "dbus_message_iter_next") && sym(recurse, "dbus_message_iter_recurse");
        if (!ok) {
            dlclose(so);
            so = nullptr;
            return false;
        }
        threadsInitDefault(); // connections are used from more than one thread (gates, notify())
        return true;
    }
};
inline Lib& lib() {
    static Lib l;
    return l;
}

/// Appends arguments to a message (or into a container).
struct Writer {
    Lib& d;
    Iter it{};
    Writer(Lib& l, Message* m) : d(l) { d.iterInitAppend(m, &it); }
    Writer(Lib& l) : d(l) {}
    Writer& str(std::string_view s) {
        const std::string c(s);
        const char* p = c.c_str();
        d.appendBasic(&it, TString, &p);
        return *this;
    }
    Writer& path(const char* p) {
        d.appendBasic(&it, TPath, &p);
        return *this;
    }
    Writer& u32(std::uint32_t v) {
        d.appendBasic(&it, TUint32, &v);
        return *this;
    }
    Writer& i32(std::int32_t v) {
        d.appendBasic(&it, TInt32, &v);
        return *this;
    }
    Writer& boolean(bool b) {
        const std::uint32_t v = b ? 1u : 0u;
        d.appendBasic(&it, TBool, &v);
        return *this;
    }
    Writer& bytes(const std::vector<unsigned char>& v) {
        return container(TArray, "y", [&](Writer& a) {
            static const unsigned char none = 0;
            const unsigned char* p = v.empty() ? &none : v.data();
            a.d.appendFixedArray(&a.it, TByte, &p, int(v.size()));
        });
    }
    /// `sig`: the contained signature (arrays, variants); null for structs / dict entries.
    template <class F>
    Writer& container(int type, const char* sig, F&& fill) {
        Writer sub(d);
        d.openContainer(&it, type, sig, &sub.it);
        fill(sub);
        d.closeContainer(&it, &sub.it);
        return *this;
    }
    template <class F>
    Writer& variant(const char* sig, F&& fill) { return container(TVariant, sig, fill); }
    template <class F>
    Writer& entry(std::string_view key, const char* sig, F&& fill) { // a{sv} entry
        return container(TDictEntry, nullptr, [&](Writer& e) { e.str(key).variant(sig, fill); });
    }
};

/// Reads arguments of a message (or of a container).
struct Reader {
    Lib& d;
    Iter it{};
    bool ok = false;
    Reader(Lib& l, Message* m) : d(l) { ok = d.iterInit(m, &it) != 0; }
    Reader(Lib& l) : d(l) {}
    int type() { return ok ? d.argType(&it) : TInvalid; }
    bool next() { return ok && d.iterNext(&it); }
    std::string str() {
        if (type() != TString && type() != TPath) return {};
        const char* p = nullptr;
        d.getBasic(&it, &p);
        return p ? p : "";
    }
    std::int64_t integer() { // i / u / b / y
        const int t = type();
        if (t == TInt32) { std::int32_t v = 0; d.getBasic(&it, &v); return v; }
        if (t == TUint32 || t == TBool) { std::uint32_t v = 0; d.getBasic(&it, &v); return v; }
        if (t == TByte) { unsigned char v = 0; d.getBasic(&it, &v); return v; }
        return 0;
    }
    Reader sub() {
        Reader r(d);
        if (type() == TArray || type() == TVariant || type() == TStruct || type() == TDictEntry) {
            d.recurse(&it, &r.it);
            r.ok = true;
        }
        return r;
    }
};

} // namespace fuse::app::dbus
#endif
