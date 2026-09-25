// FUSE Relight test-app kit (RL-0.4): minimal JSON value tree with a deterministic writer.
// Keys keep insertion order; floats print as %.9g (round-trips a float exactly); arrays of
// scalars print on one line, everything else one item per line.
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string>
#include <utility>
#include <vector>

namespace rl {

class JV {
public:
    enum Type { Null, Bool, Num, Str, Arr, Obj };

    JV() : m_t(Null) {}
    static JV null() { return JV(); }
    static JV boolean(bool b)
    {
        JV v;
        v.m_t = Bool;
        v.m_s = b ? "true" : "false";
        return v;
    }
    static JV num(double d)
    {
        char buf[64];
        snprintf(buf, sizeof buf, "%.9g", d);
        JV v;
        v.m_t = Num;
        v.m_s = buf;
        return v;
    }
    static JV integer(long long i)
    {
        JV v;
        v.m_t = Num;
        v.m_s = std::to_string(i);
        return v;
    }
    static JV uinteger(unsigned long long i)
    {
        JV v;
        v.m_t = Num;
        v.m_s = std::to_string(i);
        return v;
    }
    static JV str(const std::string& s)
    {
        JV v;
        v.m_t = Str;
        v.m_s = s;
        return v;
    }
    static JV arr()
    {
        JV v;
        v.m_t = Arr;
        return v;
    }
    static JV obj()
    {
        JV v;
        v.m_t = Obj;
        return v;
    }
    static JV floats(const float* f, size_t n)
    {
        JV v = arr();
        for (size_t i = 0; i < n; ++i)
            v.push(num(f[i]));
        return v;
    }

    Type type() const { return m_t; }
    JV& push(JV v)
    {
        m_items.emplace_back(std::string(), std::move(v));
        return m_items.back().second;
    }
    JV& set(const std::string& k, JV v)
    {
        for (auto& kv : m_items)
            if (kv.first == k) {
                kv.second = std::move(v);
                return kv.second;
            }
        m_items.emplace_back(k, std::move(v));
        return m_items.back().second;
    }
    size_t size() const { return m_items.size(); }

    std::string dump(bool pretty = true) const
    {
        std::string out;
        write(out, pretty, 0);
        if (pretty)
            out += "\n";
        return out;
    }

private:
    static void escape(std::string& out, const std::string& s)
    {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else
                    out += char(c);
            }
        }
        out += '"';
    }
    bool scalarOnly() const
    {
        for (auto& kv : m_items)
            if (kv.second.m_t == Arr || kv.second.m_t == Obj)
                return false;
        return true;
    }
    static void indent(std::string& out, int depth)
    {
        out += '\n';
        out.append(size_t(depth), ' ');
    }
    void write(std::string& out, bool pretty, int depth) const
    {
        switch (m_t) {
        case Null: out += "null"; return;
        case Bool:
        case Num: out += m_s; return;
        case Str: escape(out, m_s); return;
        case Arr:
        case Obj: {
            bool obj = m_t == Obj;
            out += obj ? '{' : '[';
            if (m_items.empty()) {
                out += obj ? '}' : ']';
                return;
            }
            bool inl = !pretty || (scalarOnly() && (!obj || m_items.size() <= 8));
            bool first = true;
            for (auto& kv : m_items) {
                if (!first)
                    out += inl && pretty ? ", " : ",";
                first = false;
                if (!inl)
                    indent(out, depth + 1);
                if (obj) {
                    escape(out, kv.first);
                    out += pretty ? ": " : ":";
                }
                kv.second.write(out, pretty, depth + 1);
            }
            if (!inl)
                indent(out, depth);
            out += obj ? '}' : ']';
            return;
        }
        }
    }

    Type m_t;
    std::string m_s;
    std::vector<std::pair<std::string, JV>> m_items;
};

} // namespace rl
