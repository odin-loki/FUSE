// Minimal D-Bus client over runtime-loaded libdbus-1 (no link-time dependency; absent library -> session() == null).
// Used by the StatusNotifierItem tray, DBusMenu export and the AppMenu global menu bar on Linux / BSD.
#pragma once
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fuse/ui/types.hpp"

namespace fuse::ui::dbus {

bool available(); ///< libdbus-1 could be loaded

/// Appends arguments to a message. Containers: open('a', "s") ... close(); open('r') struct; open('e') dict entry;
/// open('v', "s") variant.
class Writer {
public:
    explicit Writer(void* msg);
    Writer& s(std::string_view v);
    Writer& o(std::string_view v);
    Writer& i(i32 v);
    Writer& u(u32 v);
    Writer& b(bool v);
    Writer& y(u8 v);
    Writer& bytes(const void* data, std::size_t n); ///< "ay"
    Writer& open(char type, const char* sig = nullptr);
    Writer& close();
    /// Dict entry {s v} with a basic value; sig is the value's signature ("s", "i", "u", "b", "o").
    Writer& entry(std::string_view key, char sig, const void* value);
    Writer& entryS(std::string_view key, std::string_view v);
    Writer& entryI(std::string_view key, i32 v);
    Writer& entryB(std::string_view key, bool v);

private:
    struct alignas(8) Iter { unsigned char raw[128]; };
    std::deque<Iter> m_stack;
};

class Reader {
public:
    Reader() = default;
    explicit Reader(void* msg);
    int type() const;          ///< D-Bus type code at the cursor ('s', 'i', 'a', 'v', ...); 0 at the end
    bool next();
    std::string str() const;   ///< s / o / g
    i32 i32v() const;
    u32 u32v() const;
    bool boolean() const;
    std::string bytes() const; ///< "ay" at the cursor
    Reader recurse() const;    ///< into a container / variant
    bool atEnd() const { return type() == 0; }

private:
    struct alignas(8) Iter { unsigned char raw[128]; };
    Iter m_it{};
    bool m_valid = false;
};

class Message {
public:
    Message() = default;
    explicit Message(void* m, bool adopt = true);
    Message(Message&& o) noexcept : m_m(o.m_m) { o.m_m = nullptr; }
    Message& operator=(Message&& o) noexcept;
    Message(const Message&) = delete;
    ~Message();
    explicit operator bool() const { return m_m != nullptr; }
    void* raw() const { return m_m; }
    std::string interface() const, member() const, path() const, sender() const, signature() const, errorName() const;
    bool isCall(std::string_view iface, std::string_view member) const;
    bool isSignal(std::string_view iface, std::string_view member) const;
    bool isError() const;
    Writer args() { return Writer(m_m); }
    Reader read() const { return Reader(m_m); }
    Message reply() const;
    Message error(const char* name, const char* text) const;
    static Message call(std::string_view dest, std::string_view path, std::string_view iface, std::string_view member);
    static Message signal(std::string_view path, std::string_view iface, std::string_view member);

private:
    void* m_m = nullptr;
};

class Connection {
public:
    /// Private connection to the session bus ($DBUS_SESSION_BUS_ADDRESS); null when libdbus or the bus is missing.
    static std::unique_ptr<Connection> session();
    static std::unique_ptr<Connection> open(std::string_view address);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    std::string uniqueName() const;
    bool requestName(std::string_view name);   ///< true when we became primary owner
    bool nameHasOwner(std::string_view name);
    /// Handler gets every method call on `path` (and below when fallback); return true when handled (a reply was sent).
    using Handler = std::function<bool(Connection&, const Message&)>;
    bool exportObject(std::string_view path, Handler h, bool fallback = false);
    void unexportObject(std::string_view path);
    void addMatch(std::string_view rule);
    /// Signals and unhandled messages.
    std::function<void(Connection&, const Message&)> onMessage;
    bool send(Message& m);
    /// Blocking round trip (pumps only this connection). Error replies are returned as messages (isError()).
    Message callBlocking(Message& m, int timeoutMs = 2000);
    /// Non-blocking dispatch of everything queued (timeoutMs > 0 waits for I/O at most that long).
    void poll(int timeoutMs = 0);
    void flush();

private:
    Connection() = default;
    struct Exported { std::string path; Handler h; Connection* owner = nullptr; };
    void* m_c = nullptr;
    std::deque<Exported> m_objects;
    friend struct ConnectionAccess;
};

} // namespace fuse::ui::dbus
