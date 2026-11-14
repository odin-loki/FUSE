// FUSE Remaster W1.7 parity harness (docs/plans/FUSE_REMASTER_PLAN.md §4.5, §7 Wave 1): a headless run of a converted
// Torque level with its game logic, written as an event log and compared with the expected log.
//
//   fuse_remaster_parity <fixture dir> [--scenario <file>] [--expected <file>] [--out <log>] [--work <dir>]
//
// Fixture: level.mis (the mission), logic.tscript (datablocks + their callbacks), scenario.json (the scripted,
// headless player input), expected.log. Exit 0 when the log equals the expected one, 1 on a mismatch (the first
// differing line is printed), 2 on a setup error.
//
// What runs, all existing FUSE code:
//   * world: world_converter turns level.mis into a .fuselevel v3 and World3D loads it and ticks it every frame
//     (fixed dt). Positions of the mission objects come from World3D's mirrored scene nodes (Z-up -> Y-up done by
//     the converter); the harness checks them against the mission's own fields.
//   * mission / logic analysis: fuse_remaster_tscript (W1.6) parses level.mis and logic.tscript: object classes,
//     names and fields (Trigger scale / polyhedron / dataBlock, Item dataBlock), datablock fields (pickupRadius,
//     tickPeriodMS) and the callback definitions (`<Datablock>::onEnterTrigger` and friends).
//   * triggers: fuse::mechanics::TriggerZoneComponent (the T3D Trigger port: enter / leave callbacks) per Trigger.
//   * callbacks run in the quarantined compat TorqueScript VM (fuse::compat, Dialect::T3d). That VM has no
//     namespaces, objects or datablocks, so the harness feeds it only the callback functions (sliced from the
//     W1.6 AST, `A::b` mangled to `A__b`) plus one call per event; the script talks back to the engine through
//     echo lines starting with '@' ("@open <door>", "@close <door>", "@delete <item>", "@heal <obj> <n>"), and
//     its other echo lines are logged as script output. Item pickup is the T3D rule reduced to a radius test
//     (ItemData.pickupRadius, default 1 m) against the player point.
// Log line: "<tick, 4 digits> <event> <args>"; positions are FUSE metres with two decimals. The run is
// deterministic (fixed dt, sorted iteration, no time stamps).
#include <fuse/compat/ts.hpp>
#include <fuse/core/init.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/mechanics/trigger_zone.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/remaster/json.hpp>
#include <fuse/remaster/tscript/analysis.hpp>
#include <fuse/remaster/tscript/parser.hpp>
#include <fuse/world3d/scene_object_3d.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace ts = fuse::remaster::tscript;
namespace rj = fuse::remaster::json;
using fuse::u32;

struct V3 {
    float x = 0, y = 0, z = 0;
};

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string unquote(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

std::vector<float> floats(const std::string& s) {
    std::vector<float> out;
    std::istringstream in(unquote(s));
    float f;
    while (in >> f) {
        out.push_back(f);
    }
    return out;
}

/// Torque Z-up (x, y, z) -> FUSE Y-up (x, z, -y), as world_converter maps positions.
V3 toFuse(float x, float y, float z) { return {x, z, -y}; }

std::string fmt3(const V3& v) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%.2f %.2f %.2f", v.x + 0.f, v.y + 0.f, v.z + 0.f);
    return buf;
}

fuse::SceneObject3D* findObject(fuse::world3d::World3D& world, const std::string& name) {
    std::vector<fuse::Object*> stack(world.root()->children().begin(), world.root()->children().end());
    while (!stack.empty()) {
        fuse::Object* node = stack.back();
        stack.pop_back();
        if (node->name() == name) {
            return fuse::asSceneObject3D(node);
        }
        stack.insert(stack.end(), node->children().begin(), node->children().end());
    }
    return nullptr;
}

struct MissionObject {
    std::string cls, name, dataBlock;
    std::map<std::string, std::string> fields; // lower-cased name -> unquoted value
};

std::string field(const std::map<std::string, std::string>& f, const std::string& key, const std::string& fallback = {}) {
    const auto it = f.find(ts::to_lower(key));
    return it == f.end() ? fallback : it->second;
}

class Harness {
public:
    std::vector<std::string> log;
    std::vector<std::string> errors;

    bool run(const fs::path& fixture, const fs::path& scenarioFile, const fs::path& work) {
        // ---- the mission and the logic through the W1.6 analysis ----
        const ts::ParseResult mis = ts::parse_file(fixture / "level.mis", "level.mis");
        const ts::ParseResult logic = ts::parse_file(fixture / "logic.tscript", "logic.tscript");
        for (const auto* p : {&mis, &logic}) {
            for (const auto& d : p->diags) {
                errors.push_back(d.file + ":" + std::to_string(d.line) + ":" + std::to_string(d.col) + ": " + d.message);
            }
        }
        if (!errors.empty()) {
            return false;
        }
        const ts::FileAnalysis misA = ts::analyze(mis);
        const ts::FileAnalysis logicA = ts::analyze(logic);
        for (const ts::ObjectDef& o : misA.objects) {
            if (o.kind != "new" || o.name.empty()) {
                continue;
            }
            MissionObject m;
            m.cls = o.cls;
            m.name = o.name;
            for (const ts::ObjectField& f : o.fields) {
                m.fields[ts::to_lower(f.name)] = unquote(f.value);
            }
            m.dataBlock = field(m.fields, "dataBlock");
            objects_.push_back(m);
        }
        for (const ts::ObjectDef& o : logicA.objects) {
            if (o.kind == "datablock") {
                auto& fields = datablocks_[o.name];
                for (const ts::ObjectField& f : o.fields) {
                    fields[ts::to_lower(f.name)] = unquote(f.value);
                }
            }
        }
        // Callbacks: the function declarations, sliced from the AST, `::` mangled for the compat VM.
        for (const auto& stmt : logic.root->kids) {
            if (stmt->is(ts::Kind::FunctionDecl)) {
                std::string text(logic.slice(*stmt));
                std::string mangled;
                for (std::size_t i = 0; i < text.size(); ++i) {
                    if (text.compare(i, 2, "::") == 0) {
                        mangled += "__";
                        ++i;
                    } else {
                        mangled.push_back(text[i]);
                    }
                }
                callbacks_.insert(ts::to_lower(stmt->text));
                callbackSource_ += mangled + "\n";
            }
        }

        // ---- the scenario ----
        const auto scenario = rj::parse(readText(scenarioFile));
        if (!scenario || !scenario->get("schema") || scenario->get("schema")->asString() != "fuse.remaster.parity-scenario/1") {
            errors.push_back(scenarioFile.string() + ": not a fuse.remaster.parity-scenario/1 document");
            return false;
        }
        const double dt = scenario->get("dt") ? scenario->get("dt")->asDouble() : 0.05;
        const double speed = scenario->get("speed") ? scenario->get("speed")->asDouble() : 4.0;
        const std::string player = scenario->get("player") ? scenario->get("player")->asString() : "Player";
        const u32 maxTicks = scenario->get("max_ticks") ? u32(scenario->get("max_ticks")->asInt()) : 1000u;
        std::vector<V3> path;
        if (const rj::Value* w = scenario->get("waypoints_torque")) {
            for (const rj::Value& p : w->asArray()) {
                const auto& a = p.asArray();
                if (a.size() == 3) {
                    path.push_back(toFuse(float(a[0].asDouble()), float(a[1].asDouble()), float(a[2].asDouble())));
                }
            }
        }
        if (path.empty()) {
            errors.push_back("scenario: no waypoints");
            return false;
        }

        // ---- the world: convert, load, mirror ----
        std::error_code ec;
        fs::create_directories(work, ec);
        const std::string level = (work / "level.fuselevel").generic_string();
        const fuse::project::ConvertResult converted = fuse::project::convertT3DMissionToFuselevel((fixture / "level.mis").string(), level);
        if (converted.status != fuse::project::ConvertStatus::Ok) {
            errors.push_back("world_converter: " + converted.note);
            return false;
        }
        fuse::world3d::World3D world;
        const auto loaded = world.loadWorldFromFuselevel(level);
        if (!loaded.ok) {
            errors.push_back("World3D: " + loaded.note);
            return false;
        }
        emit(0, "level.load entities=" + std::to_string(loaded.sceneEntities) + " objects=" + std::to_string(loaded.objects) +
                    " unhandled=" + std::to_string(converted.components.unhandled));
        float spawn[3] = {0, 0, 0};
        if (!world.findSpawnPoint(spawn)) {
            errors.push_back("the level has no spawn point");
            return false;
        }
        V3 pos{spawn[0], spawn[1], spawn[2]};
        emit(0, "spawn " + player + " at " + fmt3(pos));

        // Mission objects: positions from World3D (checked against the mission fields).
        auto worldPos = [&](const MissionObject& m, V3& out) {
            fuse::SceneObject3D* node = findObject(world, m.name);
            const std::vector<float> p = floats(field(m.fields, "position", "0 0 0"));
            const V3 expect = p.size() == 3 ? toFuse(p[0], p[1], p[2]) : V3{};
            if (!node) {
                errors.push_back("World3D did not mirror '" + m.name + "' (" + m.cls + ")");
                return false;
            }
            const auto wt = node->worldTransform3D();
            out = {wt.x, wt.y, wt.z};
            if (std::fabs(out.x - expect.x) > 1e-4f || std::fabs(out.y - expect.y) > 1e-4f || std::fabs(out.z - expect.z) > 1e-4f) {
                errors.push_back("'" + m.name + "': World3D position " + fmt3(out) + " != mission " + fmt3(expect));
                return false;
            }
            return true;
        };
        std::vector<std::unique_ptr<fuse::mechanics::TriggerZoneComponent>> triggers;
        std::vector<std::string> triggerNames;
        struct ItemState {
            std::string name, dataBlock;
            V3 pos;
            float radius = 1.f;
            bool deleted = false;
        };
        std::vector<ItemState> items;
        constexpr u32 kPlayerId = 1;
        u32 tick = 0;
        for (const MissionObject& m : objects_) {
            V3 at;
            if (m.cls == "Trigger") {
                if (!worldPos(m, at)) {
                    return false;
                }
                // Default T3D polyhedron "-0.5 0.5 0 | 1 0 0 | 0 -1 0 | 0 0 1": x and y centred, z from the base.
                const std::vector<float> s = floats(field(m.fields, "scale", "1 1 1"));
                const std::vector<float> poly = floats(field(m.fields, "polyhedron", "-0.5 0.5 0 1 0 0 0 -1 0 0 0 1"));
                if (s.size() != 3 || poly.size() != 12 || poly[4] != 0 || poly[5] != 0 || poly[6] != 0 || poly[8] != 0 || poly[9] != 0 ||
                    poly[10] != 0) {
                    errors.push_back("trigger '" + m.name + "': only axis-aligned polyhedra are supported");
                    return false;
                }
                const float x0 = poly[0] * s[0], x1 = (poly[0] + poly[3]) * s[0];
                const float y0 = poly[1] * s[1], y1 = (poly[1] + poly[7]) * s[1];
                const float z0 = poly[2] * s[2], z1 = (poly[2] + poly[11]) * s[2];
                const V3 a = toFuse(std::min(x0, x1), std::min(y0, y1), std::min(z0, z1));
                const V3 b = toFuse(std::max(x0, x1), std::max(y0, y1), std::max(z0, z1));
                fuse::mechanics::AxisAlignedBox box{at.x + std::min(a.x, b.x), at.y + std::min(a.y, b.y), at.z + std::min(a.z, b.z),
                                                    at.x + std::max(a.x, b.x), at.y + std::max(a.y, b.y), at.z + std::max(a.z, b.z)};
                auto t = std::make_unique<fuse::mechanics::TriggerZoneComponent>(m.name, box);
                const std::string trig = m.name, db = m.dataBlock;
                t->setOnEnter([this, &tick, trig, db, player](u32) {
                    emit(tick, "trigger.enter " + trig + " " + player);
                    callback(tick, db, "onEnterTrigger", {db, trig, player});
                });
                t->setOnLeave([this, &tick, trig, db, player](u32) {
                    emit(tick, "trigger.leave " + trig + " " + player);
                    callback(tick, db, "onLeaveTrigger", {db, trig, player});
                });
                emit(0, "trigger " + m.name + " box " + fmt3({box.minX, box.minY, box.minZ}) + " .. " + fmt3({box.maxX, box.maxY, box.maxZ}));
                triggers.push_back(std::move(t));
                triggerNames.push_back(m.name);
            } else if (m.cls == "Item") {
                if (!worldPos(m, at)) {
                    return false;
                }
                const auto dbIt = datablocks_.find(m.dataBlock);
                const float r = dbIt == datablocks_.end() ? 1.f
                                                         : float(std::atof(field(dbIt->second, "pickupRadius", "1").c_str()));
                items.push_back({m.name, m.dataBlock, at, r, false});
                emit(0, "item " + m.name + " at " + fmt3(at));
            } else if (m.cls == "StaticShape") {
                if (!worldPos(m, at)) {
                    return false;
                }
                doors_[m.name] = false;
                emit(0, "door " + m.name + " closed at " + fmt3(at));
            }
        }

        // ---- the headless run ----
        fuse::frame::FrameCtx ctx;
        std::size_t next = 0;
        u32 idle = 0;
        const u32 settle = scenario->get("settle_ticks") ? u32(scenario->get("settle_ticks")->asInt()) : 5u;
        for (tick = 1; tick <= maxTicks; ++tick) {
            ctx.dt = float(dt);
            ctx.frameIndex = tick;
            world.tick(ctx);
            // Move the player towards the current waypoint at `speed`.
            float step = float(speed * dt);
            while (next < path.size() && step > 0.f) {
                const V3 d{path[next].x - pos.x, path[next].y - pos.y, path[next].z - pos.z};
                const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                if (len <= step) {
                    pos = path[next];
                    step -= len;
                    ++next;
                    emit(tick, "waypoint " + std::to_string(next - 1) + " " + fmt3(pos));
                } else {
                    pos = {pos.x + d.x / len * step, pos.y + d.y / len * step, pos.z + d.z / len * step};
                    step = 0.f;
                }
            }
            for (auto& t : triggers) {
                t->testObject(kPlayerId, pos.x, pos.y, pos.z);
            }
            for (ItemState& it : items) {
                if (it.deleted) {
                    continue;
                }
                const float dx = it.pos.x - pos.x, dy = it.pos.y - pos.y, dz = it.pos.z - pos.z;
                if (std::sqrt(dx * dx + dy * dy + dz * dz) <= it.radius) {
                    emit(tick, "item.pickup " + it.name + " " + player);
                    callback(tick, it.dataBlock, "onPickup", {it.dataBlock, it.name, player});
                    it.deleted = true; // T3D: a picked-up item is gone (the script decides what it gives)
                    if (!deleted_.count(it.name)) {
                        emit(tick, "item.consumed " + it.name);
                    }
                }
            }
            if (next >= path.size() && ++idle >= settle) {
                break;
            }
            if (!errors.empty()) {
                return false;
            }
        }
        std::string doors;
        for (const auto& [name, open] : doors_) {
            doors += " " + name + "=" + (open ? "open" : "closed");
        }
        emit(tick, "end health=" + std::to_string(health_) + doors);
        fs::remove_all(work, ec);
        return errors.empty();
    }

private:
    std::vector<MissionObject> objects_;
    std::map<std::string, std::map<std::string, std::string>> datablocks_;
    std::set<std::string> callbacks_;
    std::string callbackSource_;
    std::map<std::string, bool> doors_;
    std::set<std::string> deleted_;
    int health_ = 100;

    void emit(u32 tick, const std::string& line) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%04u ", tick);
        log.push_back(buf + line);
    }

    void callback(u32 tick, const std::string& ns, const std::string& fn, const std::vector<std::string>& args) {
        const std::string key = ns + "::" + fn;
        if (!callbacks_.count(ts::to_lower(key))) {
            return; // Torque: an undefined callback is a no-op
        }
        std::string call = ns + "__" + fn + "(";
        std::string shown;
        for (std::size_t i = 0; i < args.size(); ++i) {
            call += (i ? ", \"" : "\"") + args[i] + "\"";
            if (i) {
                shown += (i > 1 ? ", " : "") + args[i];
            }
        }
        call += ");\n";
        emit(tick, "script.call " + key + "(" + shown + ")");
        const fuse::compat::ExecResult r = fuse::compat::eval(fuse::compat::Dialect::T3d, callbackSource_ + call, "logic.tscript");
        if (!r.ok) {
            errors.push_back("script " + key + ": " + r.error);
            return;
        }
        std::istringstream lines(r.output);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty()) {
                continue;
            }
            if (line.front() != '@') {
                emit(tick, "script.echo " + line);
                continue;
            }
            std::istringstream cmd(line.substr(1));
            std::string op, a, b;
            cmd >> op >> a >> b;
            if (op == "open" || op == "close") {
                if (!doors_.count(a)) {
                    errors.push_back("script " + key + ": no door '" + a + "'");
                    return;
                }
                const bool want = op == "open";
                emit(tick, "door." + op + " " + a + (doors_[a] == want ? " (already)" : ""));
                doors_[a] = want;
            } else if (op == "delete") {
                deleted_.insert(a);
                emit(tick, "item.delete " + a);
            } else if (op == "heal") {
                health_ = std::min(200, health_ + std::atoi(b.c_str()));
                emit(tick, "player.health " + a + " " + std::to_string(health_));
            } else {
                errors.push_back("script " + key + ": unknown engine command '@" + op + "'");
                return;
            }
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fuse_remaster_parity <fixture dir> [--scenario f] [--expected f] [--out log] [--work dir]\n");
        return 2;
    }
    const fs::path fixture = argv[1];
    fs::path scenario = fixture / "scenario.json", expected = fixture / "expected.log", out,
             work = fs::temp_directory_path() / "fuse_remaster_parity";
    for (int i = 2; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scenario") scenario = argv[++i];
        else if (a == "--expected") expected = argv[++i];
        else if (a == "--out") out = argv[++i];
        else if (a == "--work") work = argv[++i];
    }
    fuse::core::initialize();
    Harness h;
    const bool ok = h.run(fixture, scenario, work);
    fuse::core::shutdown();
    for (const auto& e : h.errors) {
        std::fprintf(stderr, "error: %s\n", e.c_str());
    }
    std::string text;
    for (const auto& l : h.log) {
        text += l + "\n";
    }
    if (!out.empty()) {
        std::ofstream(out, std::ios::binary) << text;
    }
    std::fputs(text.c_str(), stdout);
    if (!ok) {
        return 2;
    }
    std::string want = readText(expected);
    want.erase(std::remove(want.begin(), want.end(), '\r'), want.end());
    if (want == text) {
        std::printf("parity: %zu events match %s\n", h.log.size(), expected.filename().string().c_str());
        return 0;
    }
    std::istringstream a(want), b(text);
    std::string la, lb;
    for (u32 n = 1;; ++n) {
        const bool ha = static_cast<bool>(std::getline(a, la)), hb = static_cast<bool>(std::getline(b, lb));
        if (!ha && !hb) {
            break;
        }
        if (!ha || !hb || la != lb) {
            std::printf("parity MISMATCH at line %u:\n  expected: %s\n  actual:   %s\n", n, ha ? la.c_str() : "<end>", hb ? lb.c_str() : "<end>");
            break;
        }
    }
    return 1;
}
