#include "rsz.hpp"

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace engine::rsz {
namespace {

using K = Kind;

// SF6_ColorSpinExtra.lua RSZ (fqn -> class, fields) + CRC_BY_CLASS, same order and kinds.
const std::vector<ClassSpec> kSpecs = {
    { 1736823766u, 1032534745u, "app.CostumeMaterialData", { { "Parts", K::Arr } } },
    { 2654138868u, 1724411514u, "app.CostumeMaterialData.MaterialData", { { "Type", K::S32 }, { "Clusters", K::Arr } } },
    { 3491580948u, 3938626124u, "app.CostumeMaterialData.ClusterData", { { "Name", K::Str }, { "CustomizeColors", K::Arr }, { "Emissive", K::Obj }, { "Body", K::Obj } } },
    { 305794566u, 3741444672u, "app.CostumeMaterialData.CustomizeColorData", { { "Enable", K::Bool }, { "Color", K::U32 }, { "Option", K::Obj } } },
    { 2888208841u, 557063721u, "app.CostumeMaterialData.CustomizeColorData.ColorOption", { { "BlendRate", K::Obj }, { "Roughness", K::Obj }, { "Metalness", K::Obj } } },
    { 3083848685u, 3454441739u, "app.CostumeMaterialData.EmissiveData", { { "Enable", K::Bool }, { "Value", K::F32 } } },
    { 4080363160u, 1865018371u, "app.CostumeMaterialData.FloatData", { { "Enable", K::Bool }, { "Value", K::F32 } } },
    { 23355824u, 1371660610u, "app.CostumeMaterialData.FreeFloatData", { { "Enable", K::Bool }, { "Value", K::F32 } } },
    { 743782621u, 3178020597u, "app.CostumeMaterialData.MetalnessData", { { "Enable", K::Bool }, { "Value", K::F32 } } },
    { 2553265928u, 2356056382u, "app.CostumeMaterialData.RoughnessData", { { "Enable", K::Bool }, { "Value", K::F32 } } },
    { 4109569515u, 475126048u, "app.CostumeMaterialData.VelvetData", { { "Enable", K::Bool }, { "Value", K::F32 } } },
    { 2635879760u, 3743521194u, "app.CostumeMaterialData.Float4Data_Offset", { { "Enable", K::Bool }, { "ValueX", K::F32 }, { "ValueY", K::F32 }, { "ValueZ", K::F32 }, { "ValueW", K::F32 } } },
    { 2002222968u, 3273561332u, "app.CostumeMaterialData.ColorData", { { "Enable", K::Bool }, { "Value", K::U32 } } },
    { 3870692347u, 1349132724u, "app.CostumeMaterialData.BodyData", { { "Cloth", K::Obj }, { "Stitch", K::Obj }, { "Hair", K::Obj } } },
    { 2674337419u, 2236564619u, "app.CostumeMaterialData.Cloth", { { "Cloth_DetailA_AOColor", K::Obj }, { "ClothAniso_PrimalySpecularColor", K::Obj }, { "FakeCloth_Color", K::Obj }, { "ClothFur_Color", K::Obj } } },
    { 1939510082u, 4153165424u, "app.CostumeMaterialData.ShellFurColor", { { "Fur_ColorFront", K::Obj }, { "Fur_ColorDeep", K::Obj }, { "Velvet_RimLightPow", K::Obj }, { "Velvet_ShadingIntensity", K::Obj }, { "Velvet_RimLightIntensity", K::Obj } } },
    { 2277254637u, 682865439u, "app.CostumeMaterialData.Stitch", { { "Stitch_A_AOColor", K::Obj }, { "Stitch_B_AOColor", K::Obj } } },
    { 2142608660u, 4034787141u, "app.CostumeMaterialData.Hair", { { "OcclutionColor", K::Obj }, { "PrimalySpecularColor", K::Obj }, { "SecondarySpecularColor", K::Obj }, { "PrimalyColorOffset", K::Obj }, { "SecondaryColorOffset", K::Obj }, { "RimLight_Color", K::Obj }, { "SpecularIntensity", K::Obj }, { "PrimalySpec_Sharpness", K::Obj }, { "SecondSpec_Sharpness", K::Obj } } },
};

struct Reader {
    std::span<const uint8_t> b;
    bool ok = true;
    bool has(size_t at, size_t n) const { return at <= b.size() && n <= b.size() - at; }
    uint32_t u32(size_t at) {
        if (!has(at, 4)) { ok = false; return 0; }
        uint32_t v; std::memcpy(&v, b.data() + at, 4); return v;
    }
    uint64_t u64(size_t at) {
        if (!has(at, 8)) { ok = false; return 0; }
        uint64_t v; std::memcpy(&v, b.data() + at, 8); return v;
    }
    float f32(size_t at) {
        if (!has(at, 4)) { ok = false; return 0.0f; }
        float v; std::memcpy(&v, b.data() + at, 4); return v;
    }
};

std::string fmt(const char* f, long long a, long long c) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), f, a, c);
    return buf;
}

struct Writer {
    std::vector<uint8_t> out;
    void put(const void* p, size_t n) { auto* c = static_cast<const uint8_t*>(p); out.insert(out.end(), c, c + n); }
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void u64(uint64_t v) { put(&v, 8); }
    void f32(float v) { put(&v, 4); }
    void zeros(size_t n) { out.insert(out.end(), n, 0); }
};

}  // namespace

const std::vector<ClassSpec>& all_specs() { return kSpecs; }

const ClassSpec* spec_by_fqn(uint32_t fqn) {
    for (auto& s : kSpecs) if (s.fqn == fqn) return &s;
    return nullptr;
}
const ClassSpec* spec_by_name(std::string_view name) {
    for (auto& s : kSpecs) if (name == s.name) return &s;
    return nullptr;
}

Value* Node::field(std::string_view name) {
    for (size_t i = 0; i < cls->fields.size(); ++i) if (name == cls->fields[i].name) return &fields[i];
    return nullptr;
}
const Value* Node::field(std::string_view name) const {
    for (size_t i = 0; i < cls->fields.size(); ++i) if (name == cls->fields[i].name) return &fields[i];
    return nullptr;
}

NodeRef make_node(const ClassSpec* cls) {
    auto n = std::make_shared<Node>();
    n->cls = cls;
    n->fields.resize(cls->fields.size());
    return n;
}

ParseResult parse(std::span<const uint8_t> bytes) {
    ParseResult r;
    if (bytes.size() < 4 || std::memcmp(bytes.data(), "USR\0", 4) != 0) { r.error = "not a .user file"; return r; }
    Reader rd{ bytes };
    const size_t rsz = (size_t)rd.u64(0x20);                       // USR dataOffset
    if (!rd.ok || !rd.has(rsz, 4) || std::memcmp(bytes.data() + rsz, "RSZ\0", 4) != 0) { r.error = "no RSZ block"; return r; }
    const uint32_t instc = rd.u32(rsz + 12);
    const size_t inst_off = (size_t)rd.u64(rsz + 24);
    const size_t data_off = (size_t)rd.u64(rsz + 32);
    const uint32_t root_index = rd.u32(rsz + 0x30);
    if (!rd.ok) { r.error = "truncated header"; return r; }

    std::vector<uint32_t> infos(instc);
    for (uint32_t i = 0; i < instc; ++i) infos[i] = rd.u32(rsz + inst_off + 8ull * i);
    if (!rd.ok) { r.error = "truncated instance table"; return r; }

    size_t p = rsz + data_off;
    auto align = [&](size_t n) { p = (p + n - 1) / n * n; };
    std::vector<NodeRef> inst(instc > 0 ? instc : 1);              // inst[0] = null instance
    auto at = [&](uint32_t v, uint32_t idx) -> NodeRef { return (v != 0 && v < idx) ? inst[v] : nullptr; };

    for (uint32_t idx = 1; idx < instc; ++idx) {
        const ClassSpec* spec = spec_by_fqn(infos[idx]);
        if (!spec) { r.error = fmt("unknown type id %lld at instance %lld", infos[idx], idx); return r; }
        NodeRef node = make_node(spec);
        for (size_t fi = 0; fi < spec->fields.size(); ++fi) {
            Value& v = node->fields[fi];
            switch (spec->fields[fi].kind) {
            case K::Bool:
                if (!rd.has(p, 1)) { rd.ok = false; break; }
                v.b = bytes[p] != 0; p += 1;
                break;
            case K::U32: case K::S32:
                align(4); v.u = rd.u32(p); p += 4;
                break;
            case K::Obj: {
                align(4); uint32_t ref = rd.u32(p); p += 4;
                v.obj = at(ref, idx);
                break;
            }
            case K::F32:
                align(4); v.f = rd.f32(p); p += 4;
                break;
            case K::Str: {
                align(4); uint32_t n = rd.u32(p); p += 4;
                // n UTF-16 units; the Lua reader keeps the low byte of each unit up to the first 0
                const size_t want = 2ull * n;
                const size_t avail = p < bytes.size() ? std::min(want, bytes.size() - p) : 0;
                for (size_t i = 0; i + 1 < avail; i += 2) {
                    uint8_t c = bytes[p + i];
                    if (c == 0) break;
                    v.s.push_back((char)c);
                }
                p += want;
                break;
            }
            case K::Arr: {
                align(4); uint32_t n = rd.u32(p); p += 4;
                if (!rd.ok || !rd.has(p, 4ull * n)) { rd.ok = false; break; }
                v.arr.reserve(n);
                for (uint32_t i = 0; i < n; ++i) { uint32_t ref = rd.u32(p); p += 4; v.arr.push_back(at(ref, idx)); }
                break;
            }
            }
            if (!rd.ok) { r.error = "truncated data block"; return r; }
        }
        inst[idx] = node;
    }
    if (p != bytes.size()) { r.error = fmt("size mismatch: consumed %lld of %lld", (long long)p, (long long)bytes.size()); return r; }
    r.root = (root_index < inst.size()) ? inst[root_index] : nullptr;
    if (!r.root) r.error = "no root instance";
    return r;
}

std::vector<NodeRef> emit_order(const NodeRef& root) {
    // instances children-first, in spec order; a node reached twice is emitted once (Lua: node._idx)
    std::vector<NodeRef> order;
    std::unordered_map<const Node*, uint32_t> seen;
    auto emit = [&](auto&& self, const NodeRef& node) -> void {
        if (!node || seen.count(node.get())) return;
        for (size_t fi = 0; fi < node->cls->fields.size(); ++fi) {
            const Kind k = node->cls->fields[fi].kind;
            if (k == K::Obj) self(self, node->fields[fi].obj);
            else if (k == K::Arr) for (auto& e : node->fields[fi].arr) self(self, e);
        }
        order.push_back(node);
        seen[node.get()] = (uint32_t)order.size();
    };
    emit(emit, root);
    return order;
}

std::vector<uint8_t> serialize(const NodeRef& root) {
    std::vector<const Node*> order;
    std::unordered_map<const Node*, uint32_t> idx_of;
    for (auto& n : emit_order(root)) { order.push_back(n.get()); idx_of[n.get()] = (uint32_t)order.size(); }

    const uint32_t n = (uint32_t)order.size() + 1;               // + the null instance 0
    const size_t rsz = 0x30, inst_off = 0x34;
    size_t data_start = rsz + inst_off + 8ull * n;
    data_start = (data_start + 15) / 16 * 16;

    Writer data;
    size_t pos = data_start;                                      // absolute position, for the alignment
    auto align = [&](size_t a) { size_t pad = (a - pos % a) % a; data.zeros(pad); pos += pad; };
    auto ref_of = [&](const NodeRef& e) -> uint32_t { if (!e) return 0; auto it = idx_of.find(e.get()); return it == idx_of.end() ? 0 : it->second; };
    for (const Node* node : order) {
        for (size_t fi = 0; fi < node->cls->fields.size(); ++fi) {
            const Value& v = node->fields[fi];
            switch (node->cls->fields[fi].kind) {
            case K::Bool: data.u8(v.b ? 1 : 0); pos += 1; break;
            case K::U32: case K::S32: align(4); data.u32(v.u); pos += 4; break;
            case K::F32: align(4); data.f32(v.f); pos += 4; break;
            case K::Obj: align(4); data.u32(ref_of(v.obj)); pos += 4; break;
            case K::Arr:
                align(4); data.u32((uint32_t)v.arr.size()); pos += 4;
                for (auto& e : v.arr) { data.u32(ref_of(e)); pos += 4; }
                break;
            case K::Str:
                align(4); data.u32((uint32_t)v.s.size() + 1); pos += 4;
                for (unsigned char c : v.s) { data.u16(c); pos += 2; }
                data.u16(0); pos += 2;
                break;
            }
        }
    }

    Writer w;
    w.put("USR\0", 4); w.u32(0); w.u32(0); w.u32(0); w.u64(rsz); w.u64(rsz); w.u64(rsz); w.zeros(8);
    const uint64_t rel_data = data_start - rsz;
    w.put("RSZ\0", 4); w.u32(16); w.u32(1); w.u32(n); w.u32(0); w.u32(0); w.u64(inst_off); w.u64(rel_data); w.u64(rel_data);
    w.u32((uint32_t)order.size());                                // object table: the root (last emitted)
    w.u32(0); w.u32(0);                                           // null instance
    for (const Node* node : order) { w.u32(node->cls->fqn); w.u32(node->cls->crc); }
    if (w.out.size() < data_start) w.zeros(data_start - w.out.size());
    w.out.insert(w.out.end(), data.out.begin(), data.out.end());
    return w.out;
}

}  // namespace engine::rsz
