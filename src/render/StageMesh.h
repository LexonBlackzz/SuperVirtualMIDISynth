#pragma once

// A room imported from an OBJ file: the triangles, grouped the way the file
// groups them (g, o or usemtl names), plus a guess at each group's material
// from its name. Everything shifts so the model's lowest corner is the origin,
// which keeps the stage's coordinates (x across, y deep, z up) valid for the
// listener, the channels and the plan views.

#include "render/StageTracer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace svms {
namespace stage {

constexpr int kMaxMeshGroups = 24;          // further groups share the last slot
constexpr size_t kMaxMeshTriangles = 12000; // the tracer tests every triangle on the CPU

struct StageMesh {
    struct Tri {
        uint32_t a = 0, b = 0, c = 0;
        uint8_t group = 0;
    };
    std::vector<Vec3> vertices;
    std::vector<Tri> tris;
    std::vector<std::string> groups;
    std::vector<uint8_t> guess;      // Material per group, from its name
    float width = 0.0f, depth = 0.0f, height = 0.0f;
    size_t fileTriangles = 0;        // before the cap
};

inline Material GuessMaterial(const std::string& name) {
    std::string l;
    for (char c : name) l.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    auto has = [&](const char* w) { return l.find(w) != std::string::npos; };
    if (has("carpet") || has("rug")) return Material::Carpet;
    if (has("foam") || has("absor")) return Material::Foam;
    if (has("wood") || has("floor") || has("parquet")) return Material::Wood;
    if (has("glass") || has("window")) return Material::Glass;
    if (has("concrete") || has("stone") || has("brick")) return Material::Concrete;
    if (has("curtain") || has("drape")) return Material::Curtain;
    return Material::Plaster;
}

// Reads v and f lines. `yUp` turns a Y-up file into the stage's Z-up axes.
inline bool ParseObj(const std::string& text, bool yUp, float scale, StageMesh& out) {
    out = StageMesh{};
    std::vector<Vec3> raw;
    std::vector<std::string> groups{"default"};
    int cur = 0;
    std::vector<StageMesh::Tri> tris;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const char* p = text.c_str() + pos;
        const char* e = text.c_str() + end;
        pos = end + 1;
        while (p < e && (*p == ' ' || *p == '\t')) ++p;
        if (p >= e) continue;
        if (p[0] == 'v' && (p[1] == ' ' || p[1] == '\t')) {
            char* q = nullptr;
            const float x = std::strtof(p + 1, &q), y = std::strtof(q, &q), z = std::strtof(q, &q);
            raw.push_back(yUp ? Vec3{x * scale, z * scale, y * scale} : Vec3{x * scale, y * scale, z * scale});
        } else if ((p[0] == 'g' || p[0] == 'o') && (p[1] == ' ' || p[1] == '\t')) {
            // group
            std::string name(p + 2, e);
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            if (name.empty()) name = "default";
            auto it = std::find(groups.begin(), groups.end(), name);
            cur = static_cast<int>(it - groups.begin());
            if (it == groups.end()) groups.push_back(name);
        } else if (std::strncmp(p, "usemtl", 6) == 0) {
            std::string name(p + 6, e);
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(name.begin());
            while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
            if (name.empty()) name = "default";
            auto it = std::find(groups.begin(), groups.end(), name);
            cur = static_cast<int>(it - groups.begin());
            if (it == groups.end()) groups.push_back(name);
        } else if (p[0] == 'f' && (p[1] == ' ' || p[1] == '\t')) {
            std::vector<long> idx;
            const char* q = p + 1;
            while (q < e) {
                while (q < e && (*q == ' ' || *q == '\t')) ++q;
                if (q >= e || *q == '\r') break;
                char* nx = nullptr;
                const long k = std::strtol(q, &nx, 10);
                if (nx == q) break;
                idx.push_back(k < 0 ? static_cast<long>(raw.size()) + k : k - 1);
                q = nx;
                while (q < e && *q != ' ' && *q != '\t') ++q;   // skip /vt/vn
            }
            for (size_t i = 1; i + 1 < idx.size(); ++i) {
                if (idx[0] < 0 || idx[i] < 0 || idx[i + 1] < 0 || idx[0] >= static_cast<long>(raw.size()) ||
                    idx[i] >= static_cast<long>(raw.size()) || idx[i + 1] >= static_cast<long>(raw.size()))
                    continue;
                StageMesh::Tri t;
                t.a = static_cast<uint32_t>(idx[0]);
                t.b = static_cast<uint32_t>(idx[i]);
                t.c = static_cast<uint32_t>(idx[i + 1]);
                t.group = static_cast<uint8_t>((std::min)(cur, kMaxMeshGroups - 1));
                tris.push_back(t);
            }
        }
    }
    if (tris.empty()) return false;
    out.fileTriangles = tris.size();
    if (tris.size() > kMaxMeshTriangles) tris.resize(kMaxMeshTriangles);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const Vec3& v : raw) {
        lo[0] = (std::min)(lo[0], v.x); hi[0] = (std::max)(hi[0], v.x);
        lo[1] = (std::min)(lo[1], v.y); hi[1] = (std::max)(hi[1], v.y);
        lo[2] = (std::min)(lo[2], v.z); hi[2] = (std::max)(hi[2], v.z);
    }
    for (Vec3& v : raw) { v.x -= lo[0]; v.y -= lo[1]; v.z -= lo[2]; }
    out.vertices = std::move(raw);
    out.tris = std::move(tris);
    out.width = hi[0] - lo[0];
    out.depth = hi[1] - lo[1];
    out.height = hi[2] - lo[2];
    const size_t n = (std::min)(groups.size(), static_cast<size_t>(kMaxMeshGroups));
    out.groups.assign(groups.begin(), groups.begin() + static_cast<long>(n));
    if (groups.size() > n) out.groups.back() = "other groups";
    for (const std::string& g : out.groups) out.guess.push_back(static_cast<uint8_t>(GuessMaterial(g)));
    return true;
}

// The mesh as tracer triangles; `materials` holds one Material per group (null = the guesses).
inline TraceScene MeshTraceScene(const StageMesh& m, const uint8_t* materials = nullptr) {
    TraceScene s;
    s.tris.reserve(m.tris.size());
    for (const StageMesh::Tri& t : m.tris) {
        Triangle x;
        const Material mat = static_cast<Material>(
            materials ? materials[t.group] : m.guess[(std::min)(static_cast<size_t>(t.group), m.guess.size() - 1)]);
        for (int b = 0; b < kBands; ++b) x.absorption[b] = Absorption(mat)[b];
        x.scatter = Scatter(mat);
        x.a = m.vertices[t.a];
        x.b = m.vertices[t.b];
        x.c = m.vertices[t.c];
        s.tris.push_back(x);
    }
    return s;
}


// Loads a model file when its path, up axis or scale changed and keeps the tracer scene built
// with the chosen materials (255 = the guess from the group name). Control thread only.
class MeshProvider {
public:
    std::shared_ptr<const StageMesh> mesh;      // null: no model (or it could not be read)
    std::shared_ptr<const TraceScene> scene;
    float volume = 0.0f;

    void Resolve(const std::string& path, bool upY, float scale, const uint8_t* materials) {
        if (path.empty()) {
            mesh.reset();
            scene.reset();
            key_.clear();
            return;
        }
        char tail[32];
        std::snprintf(tail, sizeof(tail), "|%d|%.4f", upY ? 1 : 0, scale);
        const std::string key = path + tail;
        if (key != key_) {
            key_ = key;
            mesh.reset();
            scene.reset();
            std::ifstream in(path, std::ios::binary);
            if (!in) return;
            std::ostringstream text;
            text << in.rdbuf();
            auto m = std::make_shared<StageMesh>();
            if (!ParseObj(text.str(), upY, scale, *m)) return;
            volume = SceneVolume(MeshTraceScene(*m));
            mesh = std::move(m);
        }
        if (!mesh) return;
        uint8_t mats[kMaxMeshGroups];
        for (int i = 0; i < kMaxMeshGroups; ++i) {
            const size_t g = (std::min)(static_cast<size_t>(i), mesh->guess.size() - 1);
            mats[i] = materials[i] < static_cast<uint8_t>(Material::Count) ? materials[i] : mesh->guess[g];
        }
        if (!scene || std::memcmp(mats, mats_, sizeof(mats)) != 0) {
            std::memcpy(mats_, mats, sizeof(mats));
            scene = std::make_shared<const TraceScene>(MeshTraceScene(*mesh, mats));
        }
    }

private:
    std::string key_;
    uint8_t mats_[kMaxMeshGroups] = {};
};

} // namespace stage
} // namespace svms
