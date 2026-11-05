// TorqueScript static analysis (plan §1.2 Track B, W1.6): call graph + datablock tree. See analysis.hpp.
#include "fuse/remaster/tscript/analysis.hpp"

#include <algorithm>
#include <set>
#include <tuple>

namespace fuse::remaster::tscript {

std::string_view call_kind_name(CallKind k) noexcept {
    switch (k) {
    case CallKind::Plain: return "plain";
    case CallKind::Namespace: return "namespace";
    case CallKind::Parent: return "parent";
    case CallKind::Method: return "method";
    }
    return "?";
}

std::string to_lower(std::string_view s) {
    std::string r(s);
    for (auto& c : r) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return r;
}

namespace {

struct Context {
    std::string function; // key of the enclosing function, empty at file scope
    std::string ns;       // its namespace ("Ns" of "Ns::fn"), empty if none
    std::string name;     // its unqualified name
    std::string package;
    std::string container; // enclosing object's name
};

class Analyzer {
public:
    explicit Analyzer(const ParseResult& p) : p_(p) { out_.file = p.file; }

    FileAnalysis run() {
        walk(*p_.root, Context{});
        return std::move(out_);
    }

private:
    // Object/class/parent names: bare words and literal strings as their text, anything else as source text.
    std::string name_text(const Node& n) const {
        switch (n.kind) {
        case Kind::None: return {};
        case Kind::Ident:
        case Kind::String:
        case Kind::TaggedString: return n.text;
        default: return std::string(p_.slice(n));
        }
    }

    void walk_kids(const Node& n, const Context& ctx) {
        for (const auto& k : n.kids) walk(*k, ctx);
    }

    void walk(const Node& n, const Context& ctx) {
        switch (n.kind) {
        case Kind::PackageDecl: {
            out_.packages.push_back(n.text);
            Context c = ctx;
            c.package = n.text;
            walk_kids(n, c);
            return;
        }
        case Kind::FunctionDecl: {
            FunctionDef d;
            d.key = n.text;
            d.package = ctx.package;
            for (const auto& p : n.kid(0).kids) d.params.push_back(p->text);
            d.file = p_.file;
            d.line = n.line;
            d.col = n.col;
            out_.functions.push_back(std::move(d));
            Context c = ctx;
            c.function = n.text;
            if (const auto sep = n.text.rfind("::"); sep != std::string::npos) {
                c.ns = n.text.substr(0, sep);
                c.name = n.text.substr(sep + 2);
            } else {
                c.ns.clear();
                c.name = n.text;
            }
            c.container.clear();
            walk_kids(n, c);
            return;
        }
        case Kind::Call: {
            CallSite cs = site(n, ctx);
            cs.callee = n.text;
            const auto lower = to_lower(n.text);
            if (lower.starts_with("parent::")) {
                cs.kind = CallKind::Parent;
                const std::string fn = n.text.substr(8);
                if (!ctx.package.empty() && !ctx.function.empty()) {
                    // Inside a package, Parent:: reaches the definition the package overrides.
                    cs.callee = ctx.ns.empty() ? fn : ctx.ns + "::" + fn;
                } else {
                    cs.callee = "parent::" + fn;
                    cs.receiver = ctx.ns;
                    cs.resolved = false;
                }
            } else if (n.text.find("::") != std::string::npos) {
                cs.kind = CallKind::Namespace;
            } else {
                cs.kind = CallKind::Plain;
            }
            out_.calls.push_back(std::move(cs));
            walk_kids(n, ctx);
            return;
        }
        case Kind::MethodCall: {
            CallSite cs = site(n, ctx);
            cs.kind = CallKind::Method;
            cs.callee = "." + n.text;
            if (n.kid(0).kind == Kind::Ident) cs.receiver = n.kid(0).text;
            out_.calls.push_back(std::move(cs));
            walk_kids(n, ctx);
            return;
        }
        case Kind::ObjectDecl: {
            ObjectDef o;
            o.kind = n.text;
            o.cls = name_text(n.kid(0));
            o.name = name_text(n.kid(1));
            o.parent = name_text(n.kid(2));
            o.function = ctx.function;
            o.container = ctx.container;
            o.file = p_.file;
            o.line = n.line;
            o.col = n.col;
            const Node& body = n.kid(4);
            if (body.kind == Kind::List) {
                for (const auto& item : body.kids) {
                    if (item->kind != Kind::FieldAssign) continue;
                    ObjectField f;
                    f.name = item->text;
                    const Node& idx = item->kid(0);
                    if (!idx.kids.empty()) {
                        Node span;
                        span.begin = idx.kids.front()->begin;
                        span.end = idx.kids.back()->end;
                        f.index = std::string(p_.slice(span));
                    }
                    f.value = std::string(p_.slice(item->kid(1)));
                    o.fields.push_back(std::move(f));
                }
            }
            const bool keep = o.kind != "new" || (!o.name.empty() && o.file_scope());
            Context c = ctx;
            c.container = o.name;
            if (keep) out_.objects.push_back(std::move(o));
            walk_kids(n, c);
            return;
        }
        default: walk_kids(n, ctx); return;
        }
    }

    CallSite site(const Node& n, const Context& ctx) const {
        CallSite cs;
        cs.caller = ctx.function.empty() ? "<file>" : ctx.function;
        cs.file = p_.file;
        cs.line = n.line;
        cs.col = n.col;
        return cs;
    }

    const ParseResult& p_;
    FileAnalysis out_;
};

auto site_order(const CallSite& c) {
    return std::tie(c.file, c.line, c.col, c.caller, c.callee, c.receiver);
}

} // namespace

FileAnalysis analyze(const ParseResult& parsed) { return Analyzer(parsed).run(); }

CallGraph build_call_graph(const std::vector<FileAnalysis>& files) {
    CallGraph g;
    for (const auto& f : files) {
        for (const auto& d : f.functions) {
            g.definitions[to_lower(d.key)].push_back(d);
            if (!d.package.empty()) g.packages[d.package].push_back(d.key);
        }
        for (const auto& p : f.packages) g.packages.try_emplace(p);
        g.edges.insert(g.edges.end(), f.calls.begin(), f.calls.end());
    }
    for (auto& [key, defs] : g.definitions) {
        std::sort(defs.begin(), defs.end(), [](const FunctionDef& a, const FunctionDef& b) {
            return std::tie(a.file, a.line, a.col, a.package) < std::tie(b.file, b.line, b.col, b.package);
        });
    }
    for (auto& [pkg, fns] : g.packages) {
        std::sort(fns.begin(), fns.end());
        fns.erase(std::unique(fns.begin(), fns.end()), fns.end());
    }
    std::sort(g.edges.begin(), g.edges.end(),
              [](const CallSite& a, const CallSite& b) { return site_order(a) < site_order(b); });
    for (const auto& e : g.edges) {
        if (e.kind == CallKind::Parent && !e.resolved) ++g.unresolved_parent_calls;
        if (e.kind != CallKind::Plain && e.kind != CallKind::Namespace) continue;
        const auto key = to_lower(e.callee);
        if (!g.definitions.contains(key)) ++g.unresolved[key];
    }
    return g;
}

DatablockTree build_datablock_tree(const std::vector<FileAnalysis>& files) {
    DatablockTree t;
    for (const auto& f : files) {
        for (const auto& o : f.objects) t.nodes.push_back(DatablockNode{o, -1, {}});
    }
    std::sort(t.nodes.begin(), t.nodes.end(), [](const DatablockNode& a, const DatablockNode& b) {
        return std::tie(a.def.file, a.def.line, a.def.col, a.def.name) <
               std::tie(b.def.file, b.def.line, b.def.col, b.def.name);
    });
    std::map<std::string, int> by_name;
    std::set<std::string> dup;
    for (int i = 0; i < static_cast<int>(t.nodes.size()); ++i) {
        const auto& d = t.nodes[static_cast<std::size_t>(i)].def;
        if (d.name.empty()) continue;
        const auto [it, inserted] = by_name.try_emplace(to_lower(d.name), i);
        if (!inserted) dup.insert(it->first);
    }
    t.duplicate_names = static_cast<std::uint32_t>(dup.size());
    for (int i = 0; i < static_cast<int>(t.nodes.size()); ++i) {
        auto& n = t.nodes[static_cast<std::size_t>(i)];
        if (n.def.parent.empty()) {
            t.roots.push_back(i);
            continue;
        }
        const auto it = by_name.find(to_lower(n.def.parent));
        if (it == by_name.end() || it->second == i) {
            t.missing_parents.push_back(MissingParent{n.def.name, n.def.parent, n.def.file, n.def.line});
            t.roots.push_back(i);
            continue;
        }
        n.parent_index = it->second;
        t.nodes[static_cast<std::size_t>(it->second)].children.push_back(i);
    }
    return t;
}

} // namespace fuse::remaster::tscript
