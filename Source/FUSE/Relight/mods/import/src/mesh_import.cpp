// FUSE Relight RL-3.2: UsdGeomMesh and transform import (see mesh_import.hpp).
#include <fuse/relight/mods/import/mesh_import.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace fuse::relight::mods::import {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// The attribute's default value, else its earliest time sample (`sampled` set); nullptr when neither.
const usd::Value* attrValue(const usd::Prim& p, std::string_view name, bool* sampled = nullptr) {
    const usd::Attribute* a = p.attribute(name);
    if (!a) {
        return nullptr;
    }
    if (a->hasDefault && !a->defaultValue->isNone()) {
        return &a->defaultValue.get();
    }
    if (!a->timeSamples.empty() && !a->timeSamples.front().second->isNone()) {
        if (sampled) {
            *sampled = true;
        }
        return &a->timeSamples.front().second.get();
    }
    return nullptr;
}

bool flatten(const usd::Value& v, std::vector<double>& out) {
    if (v.kind == usd::Value::Kind::Number) {
        out.push_back(v.number);
        return true;
    }
    if (v.kind == usd::Value::Kind::Bool) {
        out.push_back(v.boolean ? 1.0 : 0.0);
        return true;
    }
    if (v.kind == usd::Value::Kind::Tuple || v.kind == usd::Value::Kind::Array) {
        for (const usd::Value& i : v.items) {
            if (!flatten(i, out)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

std::string metaString(const usd::Attribute& a, std::string_view key) {
    for (const auto& [k, v] : a.metadata) {
        if (k == key) {
            return v.asString().value_or("");
        }
    }
    return {};
}

double metaNumber(const usd::Attribute& a, std::string_view key, double fallback) {
    for (const auto& [k, v] : a.metadata) {
        if (k == key) {
            return v.asNumber().value_or(fallback);
        }
    }
    return fallback;
}

// ---- matrices ----------------------------------------------------------------------------------------------

Mat4d translate(double x, double y, double z) {
    Mat4d m = identityMatrix();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

Mat4d scale(double x, double y, double z) {
    Mat4d m = identityMatrix();
    m[0] = x;
    m[5] = y;
    m[10] = z;
    return m;
}

/// Row-vector rotation about one axis (GfMatrix4d::SetRotate convention), degrees.
Mat4d rotateAxis(int axis, double degrees) {
    const double r = degrees * kPi / 180.0;
    const double c = std::cos(r), s = std::sin(r);
    Mat4d m = identityMatrix();
    if (axis == 0) {
        m[5] = c;
        m[6] = s;
        m[9] = -s;
        m[10] = c;
    } else if (axis == 1) {
        m[0] = c;
        m[2] = -s;
        m[8] = s;
        m[10] = c;
    } else {
        m[0] = c;
        m[1] = s;
        m[4] = -s;
        m[5] = c;
    }
    return m;
}

Mat4d quaternion(double w, double x, double y, double z) {
    const double n = std::sqrt(w * w + x * x + y * y + z * z);
    if (n == 0.0) {
        return identityMatrix();
    }
    w /= n;
    x /= n;
    y /= n;
    z /= n;
    Mat4d m = identityMatrix();
    // Transpose of the column-vector rotation matrix.
    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y + w * z);
    m[2] = 2 * (x * z - w * y);
    m[4] = 2 * (x * y - w * z);
    m[5] = 1 - 2 * (x * x + z * z);
    m[6] = 2 * (y * z + w * x);
    m[8] = 2 * (x * z + w * y);
    m[9] = 2 * (y * z - w * x);
    m[10] = 1 - 2 * (x * x + y * y);
    return m;
}

Mat4d invert(const Mat4d& m) {
    // Gauss-Jordan on a copy (general 4x4).
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[static_cast<std::size_t>(r * 4 + c)];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; ++c) {
        int piv = c;
        for (int r = c + 1; r < 4; ++r) {
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) {
                piv = r;
            }
        }
        if (a[piv][c] == 0.0) {
            return identityMatrix();
        }
        if (piv != c) {
            for (int k = 0; k < 8; ++k) {
                std::swap(a[c][k], a[piv][k]);
            }
        }
        const double d = a[c][c];
        for (int k = 0; k < 8; ++k) {
            a[c][k] /= d;
        }
        for (int r = 0; r < 4; ++r) {
            if (r != c && a[r][c] != 0.0) {
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k) {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    Mat4d out{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[static_cast<std::size_t>(r * 4 + c)] = a[r][c + 4];
        }
    }
    return out;
}

std::optional<Mat4d> opMatrix(const usd::Prim& prim, const std::string& opName, std::vector<TransformIssue>* issues) {
    auto issue = [&](const std::string& m) {
        if (issues) {
            issues->push_back({prim.path + ": " + m});
        }
    };
    const usd::Value* v = attrValue(prim, opName);
    if (!v) {
        issue("xformOp " + opName + " has no value");
        return std::nullopt;
    }
    std::vector<double> n;
    if (!flatten(*v, n)) {
        issue("xformOp " + opName + " is not numeric");
        return std::nullopt;
    }
    std::string type = opName.substr(8); // after "xformOp:"
    if (const std::size_t colon = type.find(':'); colon != std::string::npos) {
        type.resize(colon);
    }
    auto need = [&](std::size_t k) {
        if (n.size() != k) {
            issue("xformOp " + opName + " expects " + std::to_string(k) + " values");
            return false;
        }
        return true;
    };
    if (type == "translate") {
        return need(3) ? std::optional(translate(n[0], n[1], n[2])) : std::nullopt;
    }
    if (type == "scale") {
        return need(3) ? std::optional(scale(n[0], n[1], n[2])) : std::nullopt;
    }
    if (type == "rotateX" || type == "rotateY" || type == "rotateZ") {
        return need(1) ? std::optional(rotateAxis(type[6] - 'X', n[0])) : std::nullopt;
    }
    if (type.size() == 9 && type.compare(0, 6, "rotate") == 0) {
        if (!need(3)) {
            return std::nullopt;
        }
        // rotateABC: rotation about A first, then B, then C (row vectors: RA * RB * RC).
        Mat4d m = identityMatrix();
        for (int i = 0; i < 3; ++i) {
            const int axis = type[static_cast<std::size_t>(6 + i)] - 'X';
            if (axis < 0 || axis > 2) {
                issue("unknown xformOp " + opName);
                return std::nullopt;
            }
            m = multiplyMatrix(m, rotateAxis(axis, n[static_cast<std::size_t>(axis)]));
        }
        return m;
    }
    if (type == "orient") {
        return need(4) ? std::optional(quaternion(n[0], n[1], n[2], n[3])) : std::nullopt;
    }
    if (type == "transform") {
        if (!need(16)) {
            return std::nullopt;
        }
        Mat4d m{};
        std::copy(n.begin(), n.end(), m.begin());
        return m;
    }
    issue("unsupported xformOp " + opName);
    return std::nullopt;
}

// ---- primvars ----------------------------------------------------------------------------------------------

enum class Interp { Constant, Uniform, Vertex, FaceVarying };

struct Primvar {
    std::string name;
    std::vector<double> data;
    std::size_t comps = 1;
    std::size_t elementSize = 1;
    Interp interp = Interp::Constant;
    std::vector<std::size_t> indices;
    bool indexed = false;

    std::size_t count() const { return data.size() / (comps * elementSize); }
    /// First value of element `e` (after the index mapping).
    const double* element(std::size_t e) const {
        const std::size_t i = indexed ? indices[e] : e;
        return data.data() + i * comps * elementSize;
    }
};

struct Topology {
    std::size_t points = 0, faces = 0, corners = 0;
};

std::optional<Primvar> readPrimvar(const usd::Prim& prim, const std::string& name, std::size_t comps, Interp fallback,
                                   const Topology& topo, std::vector<MeshIssue>* issues) {
    const usd::Attribute* a = prim.attribute(name);
    if (!a) {
        return std::nullopt;
    }
    auto warn = [&](const std::string& m) {
        if (issues) {
            issues->push_back({false, name + ": " + m});
        }
    };
    bool sampled = false;
    const usd::Value* v = attrValue(prim, name, &sampled);
    if (!v) {
        return std::nullopt;
    }
    if (sampled) {
        warn("time-sampled; the earliest sample is imported");
    }
    Primvar pv;
    pv.name = name;
    pv.comps = comps;
    if (!flatten(*v, pv.data)) {
        warn("not numeric; ignored");
        return std::nullopt;
    }
    pv.elementSize = static_cast<std::size_t>(std::max(1.0, metaNumber(*a, "elementSize", 1.0)));
    const std::string interp = metaString(*a, "interpolation");
    if (interp.empty()) {
        pv.interp = fallback;
    } else if (interp == "constant") {
        pv.interp = Interp::Constant;
    } else if (interp == "uniform") {
        pv.interp = Interp::Uniform;
    } else if (interp == "vertex" || interp == "varying") {
        pv.interp = Interp::Vertex;
    } else if (interp == "faceVarying") {
        pv.interp = Interp::FaceVarying;
    } else {
        warn("unknown interpolation '" + interp + "'; ignored");
        return std::nullopt;
    }
    if (pv.data.size() % (comps * pv.elementSize) != 0) {
        warn("value count is not a multiple of the element size; ignored");
        return std::nullopt;
    }
    if (const usd::Value* iv = attrValue(prim, name + ":indices")) {
        std::vector<double> idx;
        if (flatten(*iv, idx)) {
            pv.indexed = true;
            for (double d : idx) {
                if (d < 0 || d >= double(pv.count()) || d != std::floor(d)) {
                    warn("index out of range; ignored");
                    return std::nullopt;
                }
                pv.indices.push_back(static_cast<std::size_t>(d));
            }
        }
    }
    const std::size_t have = pv.indexed ? pv.indices.size() : pv.count();
    std::size_t want = 1;
    switch (pv.interp) {
    case Interp::Constant: want = 1; break;
    case Interp::Uniform: want = topo.faces; break;
    case Interp::Vertex: want = topo.points; break;
    case Interp::FaceVarying: want = topo.corners; break;
    }
    if (pv.interp == Interp::Constant ? have < 1 : have != want) {
        warn("has " + std::to_string(have) + " elements, the interpolation needs " + std::to_string(want) + "; ignored");
        return std::nullopt;
    }
    return pv;
}

std::size_t elementIndex(const Primvar& pv, std::size_t face, std::size_t corner, std::size_t vertex) {
    switch (pv.interp) {
    case Interp::Constant: return 0;
    case Interp::Uniform: return face;
    case Interp::Vertex: return vertex;
    case Interp::FaceVarying: return corner;
    }
    return 0;
}

template <typename T>
void appendLE(std::vector<std::uint8_t>& out, T v) {
    std::uint8_t b[sizeof(T)];
    std::memcpy(b, &v, sizeof(T));
    out.insert(out.end(), b, b + sizeof(T));
}

} // namespace

Mat4d identityMatrix() {
    Mat4d m{};
    m[0] = m[5] = m[10] = m[15] = 1.0;
    return m;
}

Mat4d multiplyMatrix(const Mat4d& a, const Mat4d& b) {
    Mat4d r{};
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            double s = 0.0;
            for (std::size_t k = 0; k < 4; ++k) {
                s += a[i * 4 + k] * b[k * 4 + j];
            }
            r[i * 4 + j] = s;
        }
    }
    return r;
}

Mat4d localTransform(const usd::Prim& prim, bool* resetsStack, std::vector<TransformIssue>* issues) {
    if (resetsStack) {
        *resetsStack = false;
    }
    const usd::Value* order = attrValue(prim, "xformOpOrder");
    Mat4d m = identityMatrix();
    if (!order) {
        return m;
    }
    // Ops listed first are applied last: M = op[n-1] * ... * op[0] in row-vector order.
    for (const usd::Value& item : order->items) {
        const std::string op = item.asString().value_or("");
        if (op == "!resetXformStack!") {
            if (resetsStack) {
                *resetsStack = true;
            }
            m = identityMatrix();
            continue;
        }
        const bool inv = op.compare(0, 8, "!invert!") == 0;
        const std::string name = inv ? op.substr(8) : op;
        if (name.compare(0, 8, "xformOp:") != 0) {
            if (issues) {
                issues->push_back({prim.path + ": xformOpOrder entry '" + op + "' is not an xformOp"});
            }
            continue;
        }
        if (auto om = opMatrix(prim, name, issues)) {
            m = multiplyMatrix(inv ? invert(*om) : *om, m);
        }
    }
    return m;
}

Mat4d relativeTransform(const usd::ComposedStage& stage, const std::string& path, const std::string& ancestor,
                        std::vector<TransformIssue>* issues) {
    Mat4d m = identityMatrix();
    std::string cur = path;
    while (!cur.empty() && cur != "/") {
        const usd::Prim* p = stage.find(cur);
        if (!p) {
            break;
        }
        bool reset = false;
        m = multiplyMatrix(m, localTransform(*p, &reset, issues));
        if (reset || cur == ancestor) {
            break;
        }
        const std::size_t slash = cur.rfind('/');
        cur = slash == 0 || slash == std::string::npos ? std::string() : cur.substr(0, slash);
    }
    return m;
}

std::string boundMaterial(const usd::ComposedStage& stage, const std::string& path, const std::string& root) {
    std::string cur = path;
    while (!cur.empty() && cur != "/") {
        if (const usd::Prim* p = stage.find(cur)) {
            if (const usd::Relationship* r = p->relationship("material:binding"); r && !r->targets.empty()) {
                return r->targets.front();
            }
        }
        if (cur == root) {
            break;
        }
        const std::size_t slash = cur.rfind('/');
        cur = slash == 0 || slash == std::string::npos ? std::string() : cur.substr(0, slash);
    }
    return {};
}

std::optional<ImportedMesh> importMesh(const usd::ComposedStage& stage, const usd::Prim& prim, const std::string& bindingRoot,
                                       std::vector<MeshIssue>* issues) {
    auto warn = [&](const std::string& m) {
        if (issues) {
            issues->push_back({false, m});
        }
    };
    auto fail = [&](const std::string& m) -> std::optional<ImportedMesh> {
        if (issues) {
            issues->push_back({true, m});
        }
        return std::nullopt;
    };
    ImportedMesh out;
    bool sampled = false;
    std::vector<double> pts, counts, fvi, holes;
    const usd::Value* pv = attrValue(prim, "points", &sampled);
    const usd::Value* cv = attrValue(prim, "faceVertexCounts");
    const usd::Value* iv = attrValue(prim, "faceVertexIndices");
    if (!pv || !cv || !iv || !flatten(*pv, pts) || !flatten(*cv, counts) || !flatten(*iv, fvi) || pts.size() % 3 != 0) {
        return fail("no points / faceVertexCounts / faceVertexIndices");
    }
    if (sampled) {
        warn("points are time-sampled; the earliest sample is imported");
    }
    if (const usd::Value* hv = attrValue(prim, "holeIndices")) {
        flatten(*hv, holes);
    }
    Topology topo;
    topo.points = pts.size() / 3;
    topo.faces = counts.size();
    topo.corners = fvi.size();
    std::vector<std::size_t> faceStart(counts.size() + 1, 0);
    for (std::size_t f = 0; f < counts.size(); ++f) {
        if (counts[f] < 0) {
            return fail("negative face vertex count");
        }
        faceStart[f + 1] = faceStart[f] + static_cast<std::size_t>(counts[f]);
    }
    if (faceStart.back() != fvi.size()) {
        return fail("faceVertexCounts do not add up to faceVertexIndices");
    }
    for (double i : fvi) {
        if (i < 0 || i >= double(topo.points)) {
            return fail("faceVertexIndices out of range");
        }
    }
    std::set<std::size_t> holeSet;
    for (double h : holes) {
        if (h >= 0) {
            holeSet.insert(static_cast<std::size_t>(h));
        }
    }
    if (const usd::Value* o = attrValue(prim, "orientation")) {
        out.leftHanded = o->asString().value_or("") == "leftHanded";
    }
    if (const usd::Value* d = attrValue(prim, "doubleSided")) {
        out.doubleSided = d->asBool().value_or(false);
    }

    // Primvars.
    std::optional<Primvar> normals = readPrimvar(prim, "primvars:normals", 3, Interp::Constant, topo, issues);
    if (!normals) {
        normals = readPrimvar(prim, "normals", 3, Interp::Vertex, topo, issues);
    }
    std::string uvName = "primvars:st";
    if (!prim.attribute(uvName)) {
        uvName.clear();
        for (const auto& [name, a] : prim.attributes) { // sorted by name: deterministic
            if (name.compare(0, 9, "primvars:") == 0 && name.find(':', 9) == std::string::npos &&
                a.typeName.compare(0, 10, "texCoord2f") == 0) {
                uvName = name;
                break;
            }
        }
    }
    std::optional<Primvar> uv = uvName.empty() ? std::nullopt : readPrimvar(prim, uvName, 2, Interp::Constant, topo, issues);
    if (uv) {
        out.uvPrimvar = uvName;
    }
    std::optional<Primvar> color = readPrimvar(prim, "primvars:displayColor", 3, Interp::Constant, topo, issues);
    std::optional<Primvar> opacity = readPrimvar(prim, "primvars:displayOpacity", 1, Interp::Constant, topo, issues);
    std::optional<Primvar> joints = readPrimvar(prim, "primvars:skel:jointIndices", 1, Interp::Constant, topo, issues);
    std::optional<Primvar> weights = readPrimvar(prim, "primvars:skel:jointWeights", 1, Interp::Constant, topo, issues);
    if (joints && weights) {
        const bool perVertex = (joints->interp == Interp::Vertex || joints->interp == Interp::Constant) &&
                               (weights->interp == Interp::Vertex || weights->interp == Interp::Constant);
        if (!perVertex || joints->elementSize != weights->elementSize) {
            warn("skinning primvars are not per-vertex with matching elementSize; skinning ignored");
            joints.reset();
            weights.reset();
        } else {
            out.influences = static_cast<std::uint32_t>(joints->elementSize);
        }
    } else if (joints || weights) {
        warn("only one of skel:jointIndices / skel:jointWeights is authored; skinning ignored");
        joints.reset();
        weights.reset();
    }
    auto isPerCorner = [](const std::optional<Primvar>& p) {
        return p && (p->interp == Interp::Uniform || p->interp == Interp::FaceVarying);
    };
    out.expanded = isPerCorner(normals) || isPerCorner(uv) || isPerCorner(color) || isPerCorner(opacity);

    struct Vtx {
        std::array<float, 3> p{}, n{};
        std::array<float, 2> t{};
        std::array<float, 4> c{};
    };
    auto makeVertex = [&](std::size_t face, std::size_t corner, std::size_t v) {
        Vtx x;
        for (std::size_t k = 0; k < 3; ++k) {
            x.p[k] = static_cast<float>(pts[v * 3 + k]);
        }
        if (normals) {
            const double* e = normals->element(elementIndex(*normals, face, corner, v));
            x.n = {static_cast<float>(e[0]), static_cast<float>(e[1]), static_cast<float>(e[2])};
        }
        if (uv) {
            const double* e = uv->element(elementIndex(*uv, face, corner, v));
            x.t = {static_cast<float>(e[0]), static_cast<float>(e[1])};
        }
        x.c = {1.f, 1.f, 1.f, 1.f};
        if (color) {
            const double* e = color->element(elementIndex(*color, face, corner, v));
            x.c[0] = static_cast<float>(e[0]);
            x.c[1] = static_cast<float>(e[1]);
            x.c[2] = static_cast<float>(e[2]);
        }
        if (opacity) {
            x.c[3] = static_cast<float>(opacity->element(elementIndex(*opacity, face, corner, v))[0]);
        }
        return x;
    };
    auto pushVertex = [&](const Vtx& x, std::size_t v) {
        out.points.push_back(x.p);
        if (normals) {
            out.normals.push_back(x.n);
        }
        if (uv) {
            out.uv0.push_back(x.t);
        }
        if (color || opacity) {
            out.colors.push_back(x.c);
        }
        if (joints) {
            const std::size_t es = joints->elementSize;
            const double* j = joints->element(joints->interp == Interp::Constant ? 0 : v);
            const double* w = weights->element(weights->interp == Interp::Constant ? 0 : v);
            for (std::size_t k = 0; k < es; ++k) {
                out.jointIndices.push_back(static_cast<std::int32_t>(j[k]));
                out.jointWeights.push_back(static_cast<float>(w[k]));
            }
        }
    };

    // Corner -> output vertex.
    std::vector<std::uint32_t> cornerVertex(fvi.size(), 0);
    if (!out.expanded) {
        for (std::size_t v = 0; v < topo.points; ++v) {
            pushVertex(makeVertex(0, 0, v), v);
        }
        for (std::size_t c = 0; c < fvi.size(); ++c) {
            cornerVertex[c] = static_cast<std::uint32_t>(fvi[c]);
        }
    } else {
        std::map<std::string, std::uint32_t> weld;
        for (std::size_t f = 0; f < topo.faces; ++f) {
            if (holeSet.count(f)) {
                continue;
            }
            for (std::size_t c = faceStart[f]; c < faceStart[f + 1]; ++c) {
                const auto v = static_cast<std::size_t>(fvi[c]);
                const Vtx x = makeVertex(f, c, v);
                std::string key(reinterpret_cast<const char*>(&x), sizeof(x));
                if (joints) {
                    key.append(reinterpret_cast<const char*>(&v), sizeof(v)); // skin data follows the source vertex
                }
                const auto [it, fresh] = weld.emplace(std::move(key), static_cast<std::uint32_t>(out.points.size()));
                if (fresh) {
                    pushVertex(x, v);
                }
                cornerVertex[c] = it->second;
            }
        }
    }

    // Subsets.
    std::vector<std::pair<std::string, std::vector<std::size_t>>> groups;
    std::vector<bool> claimed(topo.faces, false);
    for (const std::string& child : prim.children) {
        const usd::Prim* sp = stage.find(prim.path + "/" + child);
        if (!sp || !sp->active || sp->typeName != "GeomSubset") {
            continue;
        }
        if (const usd::Value* et = attrValue(*sp, "elementType"); et && et->asString().value_or("face") != "face") {
            warn("GeomSubset " + sp->path + " is not a face subset; ignored");
            continue;
        }
        std::vector<double> idx;
        if (const usd::Value* iv2 = attrValue(*sp, "indices")) {
            flatten(*iv2, idx);
        }
        std::vector<std::size_t> faces;
        for (double d : idx) {
            if (d < 0 || d >= double(topo.faces)) {
                warn("GeomSubset " + sp->path + " has a face index out of range");
                continue;
            }
            const auto fi = static_cast<std::size_t>(d);
            if (!claimed[fi]) {
                claimed[fi] = true;
                faces.push_back(fi);
            }
        }
        std::sort(faces.begin(), faces.end());
        groups.emplace_back(sp->path, std::move(faces));
    }
    {
        std::vector<std::size_t> rest;
        for (std::size_t f = 0; f < topo.faces; ++f) {
            if (!claimed[f]) {
                rest.push_back(f);
            }
        }
        groups.emplace_back(std::string(), std::move(rest));
    }
    const std::string meshMaterial = boundMaterial(stage, prim.path, bindingRoot);
    bool degenerate = false;
    for (const auto& [subset, faces] : groups) {
        ImportedSubmesh sm;
        sm.indexOffset = static_cast<std::uint32_t>(out.indices.size());
        sm.subsetPath = subset;
        sm.materialPath = subset.empty() ? meshMaterial : boundMaterial(stage, subset, prim.path);
        if (!subset.empty() && sm.materialPath.empty()) {
            sm.materialPath = meshMaterial;
        }
        for (const std::size_t f : faces) {
            if (holeSet.count(f)) {
                continue;
            }
            const std::size_t n = faceStart[f + 1] - faceStart[f];
            if (n < 3) {
                degenerate = true;
                continue;
            }
            const std::size_t c0 = faceStart[f];
            for (std::size_t i = 1; i + 1 < n; ++i) {
                const std::uint32_t a = cornerVertex[c0], b = cornerVertex[c0 + i], c = cornerVertex[c0 + i + 1];
                out.indices.push_back(a);
                if (out.leftHanded) {
                    out.indices.push_back(c);
                    out.indices.push_back(b);
                } else {
                    out.indices.push_back(b);
                    out.indices.push_back(c);
                }
            }
        }
        sm.indexCount = static_cast<std::uint32_t>(out.indices.size()) - sm.indexOffset;
        if (sm.indexCount > 0 || (!subset.empty())) {
            out.submeshes.push_back(std::move(sm));
        }
    }
    if (degenerate) {
        warn("faces with fewer than 3 vertices were skipped");
    }
    if (out.indices.empty()) {
        warn("no triangles");
    }
    if (!out.points.empty()) {
        out.bounds = {out.points[0][0], out.points[0][1], out.points[0][2], out.points[0][0], out.points[0][1], out.points[0][2]};
        for (const auto& p : out.points) {
            for (std::size_t k = 0; k < 3; ++k) {
                out.bounds[k] = std::min(out.bounds[k], p[k]);
                out.bounds[k + 3] = std::max(out.bounds[k + 3], p[k]);
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> positionStream(const ImportedMesh& m) {
    std::vector<std::uint8_t> b;
    for (const auto& v : m.points) {
        appendLE(b, v[0]);
        appendLE(b, v[1]);
        appendLE(b, v[2]);
    }
    return b;
}

std::vector<std::uint8_t> normalStream(const ImportedMesh& m) {
    std::vector<std::uint8_t> b;
    for (const auto& v : m.normals) {
        appendLE(b, v[0]);
        appendLE(b, v[1]);
        appendLE(b, v[2]);
    }
    return b;
}

std::vector<std::uint8_t> uv0Stream(const ImportedMesh& m) {
    std::vector<std::uint8_t> b;
    for (const auto& v : m.uv0) {
        appendLE(b, v[0]);
        appendLE(b, v[1]);
    }
    return b;
}

std::vector<std::uint8_t> color0Stream(const ImportedMesh& m) {
    std::vector<std::uint8_t> b;
    for (const auto& v : m.colors) {
        for (int k = 0; k < 4; ++k) {
            b.push_back(static_cast<std::uint8_t>(std::lround(std::clamp(v[static_cast<std::size_t>(k)], 0.f, 1.f) * 255.f)));
        }
    }
    return b;
}

std::vector<std::uint8_t> joints0Stream(const ImportedMesh& m) {
    std::vector<std::uint8_t> b;
    const std::size_t bpv = m.influences;
    for (std::size_t i = 0; i < m.points.size(); ++i) {
        for (std::size_t k = 0; k < 4; ++k) {
            const bool in = k < bpv && i * bpv + k < m.jointIndices.size();
            appendLE(b, static_cast<std::uint16_t>(in ? m.jointIndices[i * bpv + k] : 0));
        }
    }
    return b;
}

std::vector<std::uint8_t> weights0Stream(const ImportedMesh& m) {
    std::vector<std::uint8_t> b;
    const std::size_t bpv = m.influences;
    for (std::size_t i = 0; i < m.points.size(); ++i) {
        for (std::size_t k = 0; k < 4; ++k) {
            const bool in = k < bpv && i * bpv + k < m.jointWeights.size();
            appendLE(b, in ? m.jointWeights[i * bpv + k] : 0.f);
        }
    }
    return b;
}

std::vector<std::uint8_t> indices32Stream(const ImportedMesh& m) {
    std::vector<std::uint8_t> b;
    for (const std::uint32_t i : m.indices) {
        appendLE(b, i);
    }
    return b;
}

} // namespace fuse::relight::mods::import
