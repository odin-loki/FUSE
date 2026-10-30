// libdbus-1 loaded at runtime through the os seam (fuse::ui::os::loadLibrary -> FORGE).
#include "fuse/ui/extras/dbus.hpp"

#include <cstring>
#include <mutex>

#include "fuse/ui/os.hpp"

namespace fuse::ui::dbus {
namespace {
struct DBusError { const char* name; const char* message; unsigned d1 : 1, d2 : 1, d3 : 1, d4 : 1, d5 : 1; void* pad; };
using HandlerFn = int (*)(void*, void*, void*);
struct VTable { void (*unreg)(void*, void*); HandlerFn msg; void (*p1)(void*); void (*p2)(void*); void (*p3)(void*); void (*p4)(void*); };
constexpr int kBusSession = 0;
constexpr int kNameFlagDoNotQueue = 4, kPrimaryOwner = 1;
constexpr int kHandled = 0, kNotYetHandled = 1;
constexpr int kTypeMethodCall = 1, kTypeError = 3, kTypeSignal = 4;

struct Api {
    std::unique_ptr<os::Library> lib;
    int (*threads_init_default)();
    void (*error_init)(DBusError*);
    void (*error_free)(DBusError*);
    unsigned (*error_is_set)(const DBusError*);
    void* (*bus_get_private)(int, DBusError*);
    void* (*connection_open_private)(const char*, DBusError*);
    unsigned (*bus_register)(void*, DBusError*);
    int (*bus_request_name)(void*, const char*, unsigned, DBusError*);
    unsigned (*bus_name_has_owner)(void*, const char*, DBusError*);
    const char* (*bus_get_unique_name)(void*);
    void (*bus_add_match)(void*, const char*, DBusError*);
    void (*connection_close)(void*);
    void (*connection_unref)(void*);
    void (*connection_flush)(void*);
    unsigned (*connection_send)(void*, void*, u32*);
    void* (*connection_send_with_reply_and_block)(void*, void*, int, DBusError*);
    unsigned (*connection_read_write_dispatch)(void*, int);
    unsigned (*connection_read_write)(void*, int);
    int (*connection_dispatch)(void*);
    unsigned (*connection_try_register_object_path)(void*, const char*, const VTable*, void*, DBusError*);
    unsigned (*connection_try_register_fallback)(void*, const char*, const VTable*, void*, DBusError*);
    unsigned (*connection_unregister_object_path)(void*, const char*);
    unsigned (*connection_add_filter)(void*, HandlerFn, void*, void (*)(void*));
    void (*connection_set_exit_on_disconnect)(void*, unsigned);
    void* (*message_new_method_call)(const char*, const char*, const char*, const char*);
    void* (*message_new_signal)(const char*, const char*, const char*);
    void* (*message_new_method_return)(void*);
    void* (*message_new_error)(void*, const char*, const char*);
    void* (*message_ref)(void*);
    void (*message_unref)(void*);
    int (*message_get_type)(void*);
    const char* (*message_get_interface)(void*);
    const char* (*message_get_member)(void*);
    const char* (*message_get_path)(void*);
    const char* (*message_get_sender)(void*);
    const char* (*message_get_signature)(void*);
    const char* (*message_get_error_name)(void*);
    void (*message_iter_init_append)(void*, void*);
    unsigned (*message_iter_init)(void*, void*);
    unsigned (*message_iter_append_basic)(void*, int, const void*);
    unsigned (*message_iter_append_fixed_array)(void*, int, const void*, int);
    unsigned (*message_iter_open_container)(void*, int, const char*, void*);
    unsigned (*message_iter_close_container)(void*, void*);
    int (*message_iter_get_arg_type)(void*);
    void (*message_iter_get_basic)(void*, void*);
    void (*message_iter_recurse)(void*, void*);
    unsigned (*message_iter_next)(void*);
};
Api* api() {
    static Api a{};
    static bool ok = [] {
        a.lib = os::loadLibrary({"libdbus-1.so.3", "libdbus-1.so", "libdbus-1.3.dylib", "libdbus-1.dylib"});
        if (!a.lib) return false;
        bool all = true;
        auto L = [&](auto& f, const char* n) { f = a.lib->get<std::remove_reference_t<decltype(f)>>(n); all = all && f; };
#define S(n) L(a.n, "dbus_" #n)
        S(threads_init_default); S(error_init); S(error_free); S(error_is_set); S(bus_get_private); S(connection_open_private);
        S(bus_register); S(bus_request_name); S(bus_name_has_owner); S(bus_get_unique_name); S(bus_add_match);
        S(connection_close); S(connection_unref); S(connection_flush); S(connection_send); S(connection_send_with_reply_and_block);
        S(connection_read_write_dispatch); S(connection_read_write); S(connection_dispatch); S(connection_try_register_object_path);
        S(connection_try_register_fallback); S(connection_unregister_object_path); S(connection_add_filter);
        S(connection_set_exit_on_disconnect); S(message_new_method_call); S(message_new_signal); S(message_new_method_return);
        S(message_new_error); S(message_ref); S(message_unref); S(message_get_type); S(message_get_interface); S(message_get_member);
        S(message_get_path); S(message_get_sender); S(message_get_signature); S(message_get_error_name); S(message_iter_init_append);
        S(message_iter_init); S(message_iter_append_basic); S(message_iter_append_fixed_array); S(message_iter_open_container);
        S(message_iter_close_container); S(message_iter_get_arg_type); S(message_iter_get_basic); S(message_iter_recurse);
        S(message_iter_next);
#undef S
        if (all) a.threads_init_default();
        return all;
    }();
    return ok ? &a : nullptr;
}
std::string cs(const char* s) { return s ? s : ""; }
} // namespace

bool available() { return api() != nullptr; }

// ---- Writer
Writer::Writer(void* msg) {
    m_stack.emplace_back();
    if (api() && msg) api()->message_iter_init_append(msg, m_stack.back().raw);
}
Writer& Writer::s(std::string_view v) {
    const std::string t(v);
    const char* p = t.c_str();
    if (api()) api()->message_iter_append_basic(m_stack.back().raw, 's', &p);
    return *this;
}
Writer& Writer::o(std::string_view v) {
    const std::string t(v);
    const char* p = t.c_str();
    if (api()) api()->message_iter_append_basic(m_stack.back().raw, 'o', &p);
    return *this;
}
Writer& Writer::i(i32 v) { if (api()) api()->message_iter_append_basic(m_stack.back().raw, 'i', &v); return *this; }
Writer& Writer::u(u32 v) { if (api()) api()->message_iter_append_basic(m_stack.back().raw, 'u', &v); return *this; }
Writer& Writer::b(bool v) { const u32 x = v; if (api()) api()->message_iter_append_basic(m_stack.back().raw, 'b', &x); return *this; }
Writer& Writer::y(u8 v) { if (api()) api()->message_iter_append_basic(m_stack.back().raw, 'y', &v); return *this; }
Writer& Writer::bytes(const void* data, std::size_t n) {
    open('a', "y");
    if (api()) api()->message_iter_append_fixed_array(m_stack.back().raw, 'y', &data, int(n));
    return close();
}
Writer& Writer::open(char type, const char* sig) {
    m_stack.emplace_back();
    if (api()) api()->message_iter_open_container(m_stack[m_stack.size() - 2].raw, type, sig, m_stack.back().raw);
    return *this;
}
Writer& Writer::close() {
    if (m_stack.size() < 2) return *this;
    if (api()) api()->message_iter_close_container(m_stack[m_stack.size() - 2].raw, m_stack.back().raw);
    m_stack.pop_back();
    return *this;
}
Writer& Writer::entry(std::string_view key, char sig, const void* value) {
    open('e').s(key);
    const char sg[2] = {sig, 0};
    open('v', sg);
    if (api()) api()->message_iter_append_basic(m_stack.back().raw, sig, value);
    return close().close();
}
Writer& Writer::entryS(std::string_view key, std::string_view v) {
    const std::string t(v);
    const char* p = t.c_str();
    return entry(key, 's', &p);
}
Writer& Writer::entryI(std::string_view key, i32 v) { return entry(key, 'i', &v); }
Writer& Writer::entryB(std::string_view key, bool v) { const u32 x = v; return entry(key, 'b', &x); }

// ---- Reader
Reader::Reader(void* msg) { m_valid = api() && msg && api()->message_iter_init(msg, m_it.raw); }
int Reader::type() const { return m_valid ? api()->message_iter_get_arg_type(const_cast<unsigned char*>(m_it.raw)) : 0; }
bool Reader::next() { return m_valid && api()->message_iter_next(m_it.raw); }
std::string Reader::str() const {
    const int t = type();
    if (t != 's' && t != 'o' && t != 'g') return {};
    const char* p = nullptr;
    api()->message_iter_get_basic(const_cast<unsigned char*>(m_it.raw), &p);
    return cs(p);
}
i32 Reader::i32v() const {
    if (type() != 'i') return 0;
    i32 v = 0;
    api()->message_iter_get_basic(const_cast<unsigned char*>(m_it.raw), &v);
    return v;
}
u32 Reader::u32v() const {
    if (type() != 'u') return 0;
    u32 v = 0;
    api()->message_iter_get_basic(const_cast<unsigned char*>(m_it.raw), &v);
    return v;
}
bool Reader::boolean() const {
    if (type() != 'b') return false;
    u32 v = 0;
    api()->message_iter_get_basic(const_cast<unsigned char*>(m_it.raw), &v);
    return v != 0;
}
std::string Reader::bytes() const {
    std::string out;
    if (type() != 'a') return out;
    Reader r = recurse();
    while (r.type() == 'y') {
        u8 v = 0;
        api()->message_iter_get_basic(r.m_it.raw, &v);
        out += char(v);
        r.next();
    }
    return out;
}
Reader Reader::recurse() const {
    Reader r;
    const int t = type();
    if (t == 'a' || t == 'v' || t == 'r' || t == 'e') {
        api()->message_iter_recurse(const_cast<unsigned char*>(m_it.raw), r.m_it.raw);
        r.m_valid = true;
    }
    return r;
}

// ---- Message
Message::Message(void* m, bool adopt) : m_m(m) { if (m && !adopt && api()) api()->message_ref(m); }
Message& Message::operator=(Message&& o) noexcept {
    if (this != &o) { if (m_m && api()) api()->message_unref(m_m); m_m = o.m_m; o.m_m = nullptr; }
    return *this;
}
Message::~Message() { if (m_m && api()) api()->message_unref(m_m); }
std::string Message::interface() const { return m_m ? cs(api()->message_get_interface(m_m)) : ""; }
std::string Message::member() const { return m_m ? cs(api()->message_get_member(m_m)) : ""; }
std::string Message::path() const { return m_m ? cs(api()->message_get_path(m_m)) : ""; }
std::string Message::sender() const { return m_m ? cs(api()->message_get_sender(m_m)) : ""; }
std::string Message::signature() const { return m_m ? cs(api()->message_get_signature(m_m)) : ""; }
std::string Message::errorName() const { return m_m ? cs(api()->message_get_error_name(m_m)) : ""; }
bool Message::isCall(std::string_view i, std::string_view mb) const {
    return m_m && api()->message_get_type(m_m) == kTypeMethodCall && (i.empty() || interface() == i) && member() == mb;
}
bool Message::isSignal(std::string_view i, std::string_view mb) const {
    return m_m && api()->message_get_type(m_m) == kTypeSignal && interface() == i && member() == mb;
}
bool Message::isError() const { return m_m && api()->message_get_type(m_m) == kTypeError; }
Message Message::reply() const { return Message(m_m ? api()->message_new_method_return(m_m) : nullptr); }
Message Message::error(const char* name, const char* text) const { return Message(m_m ? api()->message_new_error(m_m, name, text) : nullptr); }
Message Message::call(std::string_view dest, std::string_view path, std::string_view iface, std::string_view member) {
    if (!api()) return {};
    const std::string d(dest), p(path), i(iface), m(member);
    return Message(api()->message_new_method_call(d.empty() ? nullptr : d.c_str(), p.c_str(), i.empty() ? nullptr : i.c_str(), m.c_str()));
}
Message Message::signal(std::string_view path, std::string_view iface, std::string_view member) {
    if (!api()) return {};
    const std::string p(path), i(iface), m(member);
    return Message(api()->message_new_signal(p.c_str(), i.c_str(), m.c_str()));
}

// ---- Connection
struct ConnectionAccess {
    static int objectFn(void*, void* msg, void* user) {
        auto* e = static_cast<Connection::Exported*>(user);
        const Message m(msg, false);
        return e->h && e->h(*e->owner, m) ? kHandled : kNotYetHandled;
    }
    static int filterFn(void*, void* msg, void* user) {
        auto* c = static_cast<Connection*>(user);
        if (c->onMessage) {
            const Message m(msg, false);
            if (api()->message_get_type(msg) != kTypeMethodCall) c->onMessage(*c, m);
        }
        return kNotYetHandled;
    }
};
namespace {
void setup(void* raw) { api()->connection_set_exit_on_disconnect(raw, 0); }
} // namespace
std::unique_ptr<Connection> Connection::session() {
    Api* a = api();
    if (!a) return nullptr;
    DBusError e;
    a->error_init(&e);
    void* raw = a->bus_get_private(kBusSession, &e);
    if (a->error_is_set(&e)) a->error_free(&e);
    if (!raw) return nullptr;
    std::unique_ptr<Connection> c(new Connection());
    c->m_c = raw;
    setup(raw);
    a->connection_add_filter(raw, ConnectionAccess::filterFn, c.get(), nullptr);
    return c;
}
std::unique_ptr<Connection> Connection::open(std::string_view address) {
    Api* a = api();
    if (!a) return nullptr;
    DBusError e;
    a->error_init(&e);
    const std::string ad(address);
    void* raw = a->connection_open_private(ad.c_str(), &e);
    if (raw && !a->bus_register(raw, &e)) { a->connection_close(raw); a->connection_unref(raw); raw = nullptr; }
    if (a->error_is_set(&e)) a->error_free(&e);
    if (!raw) return nullptr;
    std::unique_ptr<Connection> c(new Connection());
    c->m_c = raw;
    setup(raw);
    a->connection_add_filter(raw, ConnectionAccess::filterFn, c.get(), nullptr);
    return c;
}
Connection::~Connection() {
    if (!m_c) return;
    for (auto& o : m_objects) api()->connection_unregister_object_path(m_c, o.path.c_str());
    api()->connection_flush(m_c);
    api()->connection_close(m_c);
    api()->connection_unref(m_c);
}
std::string Connection::uniqueName() const { return cs(api()->bus_get_unique_name(m_c)); }
bool Connection::requestName(std::string_view name) {
    DBusError e;
    api()->error_init(&e);
    const std::string n(name);
    const int r = api()->bus_request_name(m_c, n.c_str(), kNameFlagDoNotQueue, &e);
    if (api()->error_is_set(&e)) api()->error_free(&e);
    return r == kPrimaryOwner;
}
bool Connection::nameHasOwner(std::string_view name) {
    DBusError e;
    api()->error_init(&e);
    const std::string n(name);
    const bool r = api()->bus_name_has_owner(m_c, n.c_str(), &e);
    if (api()->error_is_set(&e)) api()->error_free(&e);
    return r;
}
bool Connection::exportObject(std::string_view path, Handler h, bool fallback) {
    m_objects.push_back({std::string(path), std::move(h), this});
    static const VTable vt{nullptr, ConnectionAccess::objectFn, nullptr, nullptr, nullptr, nullptr};
    DBusError e;
    api()->error_init(&e);
    Exported& x = m_objects.back();
    const bool ok = fallback ? api()->connection_try_register_fallback(m_c, x.path.c_str(), &vt, &x, &e)
                             : api()->connection_try_register_object_path(m_c, x.path.c_str(), &vt, &x, &e);
    if (api()->error_is_set(&e)) api()->error_free(&e);
    if (!ok) m_objects.pop_back();
    return ok;
}
void Connection::unexportObject(std::string_view path) {
    for (auto& o : m_objects)
        if (o.path == path) { api()->connection_unregister_object_path(m_c, o.path.c_str()); o.h = nullptr; }
}
void Connection::addMatch(std::string_view rule) {
    DBusError e;
    api()->error_init(&e);
    const std::string r(rule);
    api()->bus_add_match(m_c, r.c_str(), &e);
    if (api()->error_is_set(&e)) api()->error_free(&e);
}
bool Connection::send(Message& m) { return m && api()->connection_send(m_c, m.raw(), nullptr); }
Message Connection::callBlocking(Message& m, int timeoutMs) {
    if (!m) return {};
    DBusError e;
    api()->error_init(&e);
    void* r = api()->connection_send_with_reply_and_block(m_c, m.raw(), timeoutMs, &e);
    if (api()->error_is_set(&e)) {
        Message err = m.error(e.name ? e.name : "org.freedesktop.DBus.Error.Failed", e.message ? e.message : "");
        api()->error_free(&e);
        return err;
    }
    return Message(r);
}
void Connection::poll(int timeoutMs) {
    api()->connection_read_write(m_c, timeoutMs);
    while (api()->connection_dispatch(m_c) == 0 /* DATA_REMAINS */) {}
}
void Connection::flush() { api()->connection_flush(m_c); }

} // namespace fuse::ui::dbus
