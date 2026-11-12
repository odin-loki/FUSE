// Deterministic JSON dumps for the TorqueScript AST and analyses (plan §1.2 Track B, W1.6). See json.hpp.
#include "fuse/remaster/tscript/json.hpp"

#include <cstdio>

namespace fuse::remaster::tscript {
namespace {

// Minimal pretty-printing JSON writer: fixed key order (call order), 2-space indent.
class Writer {
public:
    void begin_object() { open('{'); }
    void end_object() { close('}'); }
    void begin_array() { open('['); }
    void end_array() { close(']'); }

    void key(std::string_view k) {
        separate();
        string(k);
        out_ += ": ";
        after_key_ = true;
    }
    void value(std::string_view s) {
        separate();
        string(s);
    }
    void value(const char* s) { value(std::string_view(s)); }
    void value(std::int64_t v) {
        separate();
        out_ += std::to_string(v);
    }
    void value(std::uint32_t v) { value(static_cast<std::int64_t>(v)); }
    void value(int v) { value(static_cast<std::int64_t>(v)); }
    void value(std::size_t v) { value(static_cast<std::int64_t>(v)); }
    void value(bool b) {
        separate();
        out_ += b ? "true" : "false";
    }

    template <class T> void field(std::string_view k, const T& v) {
        key(k);
        value(v);
    }

    std::string take() {
        out_ += '\n';
        return std::move(out_);
    }

private:
    void separate() {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (!first_.empty()) {
            if (!first_.back()) out_ += ',';
            first_.back() = false;
            newline();
        }
    }
    void open(char c) {
        separate();
        out_ += c;
        first_.push_back(true);
    }
    void close(char c) {
        const bool empty = first_.back();
        first_.pop_back();
        if (!empty) newline();
        out_ += c;
    }
    void newline() {
        out_ += '\n';
        out_.append(first_.size() * 2, ' ');
    }
    void string(std::string_view s) {
        out_ += '"';
        for (const char ch : s) {
            const auto c = static_cast<unsigned char>(ch);
            switch (ch) {
            case '"': out_ += "\\\""; break;
            case '\\': out_ += "\\\\"; break;
            case '\n': out_ += "\\n"; break;
            case '\r': out_ += "\\r"; break;
            case '\t': out_ += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out_ += buf;
                } else {
                    out_ += ch; // UTF-8 passes through; invalid bytes are the source's own
                }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> first_;
    bool after_key_ = false;
};

void write_node(Writer& w, const Node& n) {
    w.begin_object();
    w.field("kind", kind_name(n.kind));
    if (!n.text.empty()) w.field("text", n.text);
    w.field("line", n.line);
    w.field("col", n.col);
    if (!n.kids.empty()) {
        w.key("kids");
        w.begin_array();
        for (const auto& k : n.kids) write_node(w, *k);
        w.end_array();
    }
    w.end_object();
}

void write_def(Writer& w, const FunctionDef& d) {
    w.begin_object();
    w.field("key", d.key);
    if (!d.package.empty()) w.field("package", d.package);
    w.key("params");
    w.begin_array();
    for (const auto& p : d.params) w.value(p);
    w.end_array();
    w.field("file", d.file);
    w.field("line", d.line);
    w.end_object();
}

void write_call(Writer& w, const CallSite& c) {
    w.begin_object();
    w.field("caller", c.caller);
    w.field("callee", c.callee);
    w.field("kind", call_kind_name(c.kind));
    if (!c.resolved) w.field("resolved", false);
    if (!c.receiver.empty()) w.field("receiver", c.receiver);
    w.field("file", c.file);
    w.field("line", c.line);
    w.field("col", c.col);
    w.end_object();
}

void write_object(Writer& w, const ObjectDef& o) {
    w.field("kind", o.kind);
    w.field("class", o.cls);
    w.field("name", o.name);
    if (!o.parent.empty()) w.field("parent", o.parent);
    if (!o.function.empty()) w.field("function", o.function);
    if (!o.container.empty()) w.field("container", o.container);
    w.field("file", o.file);
    w.field("line", o.line);
    w.key("fields");
    w.begin_array();
    for (const auto& f : o.fields) {
        w.begin_object();
        w.field("name", f.name);
        if (!f.index.empty()) w.field("index", f.index);
        w.field("value", f.value);
        w.end_object();
    }
    w.end_array();
}

} // namespace

std::string ast_to_json(const Node& root) {
    Writer w;
    write_node(w, root);
    return w.take();
}

std::string diagnostics_to_json(const std::vector<Diagnostic>& diags) {
    Writer w;
    w.begin_array();
    for (const auto& d : diags) {
        w.begin_object();
        w.field("file", d.file);
        w.field("line", d.line);
        w.field("col", d.col);
        w.field("message", d.message);
        w.end_object();
    }
    w.end_array();
    return w.take();
}

std::string analysis_to_json(const FileAnalysis& fa) {
    Writer w;
    w.begin_object();
    w.field("file", fa.file);
    w.key("packages");
    w.begin_array();
    for (const auto& p : fa.packages) w.value(p);
    w.end_array();
    w.key("functions");
    w.begin_array();
    for (const auto& d : fa.functions) write_def(w, d);
    w.end_array();
    w.key("calls");
    w.begin_array();
    for (const auto& c : fa.calls) write_call(w, c);
    w.end_array();
    w.key("objects");
    w.begin_array();
    for (const auto& o : fa.objects) {
        w.begin_object();
        write_object(w, o);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    return w.take();
}

std::string call_graph_to_json(const CallGraph& g) {
    Writer w;
    w.begin_object();
    w.key("definitions");
    w.begin_object();
    for (const auto& [key, defs] : g.definitions) {
        w.key(key);
        w.begin_array();
        for (const auto& d : defs) write_def(w, d);
        w.end_array();
    }
    w.end_object();
    w.key("packages");
    w.begin_object();
    for (const auto& [pkg, fns] : g.packages) {
        w.key(pkg);
        w.begin_array();
        for (const auto& f : fns) w.value(f);
        w.end_array();
    }
    w.end_object();
    w.key("edges");
    w.begin_array();
    for (const auto& e : g.edges) write_call(w, e);
    w.end_array();
    w.key("unresolved");
    w.begin_object();
    for (const auto& [callee, count] : g.unresolved) w.field(callee, count);
    w.end_object();
    w.field("unresolved_parent_calls", g.unresolved_parent_calls);
    w.end_object();
    return w.take();
}

std::string datablock_tree_to_json(const DatablockTree& t) {
    Writer w;
    w.begin_object();
    w.key("nodes");
    w.begin_array();
    for (std::size_t i = 0; i < t.nodes.size(); ++i) {
        const auto& n = t.nodes[i];
        w.begin_object();
        w.field("id", i);
        write_object(w, n.def);
        if (n.parent_index >= 0) w.field("parent_id", n.parent_index);
        if (!n.children.empty()) {
            w.key("children");
            w.begin_array();
            for (const int c : n.children) w.value(c);
            w.end_array();
        }
        w.end_object();
    }
    w.end_array();
    w.key("roots");
    w.begin_array();
    for (const int r : t.roots) w.value(r);
    w.end_array();
    w.key("missing_parents");
    w.begin_array();
    for (const auto& m : t.missing_parents) {
        w.begin_object();
        w.field("child", m.child);
        w.field("parent", m.parent);
        w.field("file", m.file);
        w.field("line", m.line);
        w.end_object();
    }
    w.end_array();
    w.field("duplicate_names", t.duplicate_names);
    w.end_object();
    return w.take();
}

} // namespace fuse::remaster::tscript
