// dialogs::editor::whole - see whole_colour.hpp.
#include "whole_colour.hpp"

#include <map>
#include <unordered_map>

namespace dialogs::editor::whole {
namespace {

using rsz::Kind;
using rsz::Node;
using rsz::NodeRef;

std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }

// dst takes src's values (same class): switches and numbers, then its sub-objects and its arrays' elements by their
// place, where dst has the same thing. Names stay (they are what matched) and dst's structure never changes: what it
// lacks (a null, another class, fewer elements) is skipped.
void take_values(Node& dst, const Node& src) {
    if (dst.cls != src.cls || !dst.cls) return;
    for (size_t fi = 0; fi < dst.cls->fields.size() && fi < dst.fields.size() && fi < src.fields.size(); ++fi) {
        rsz::Value& d = dst.fields[fi];
        const rsz::Value& s = src.fields[fi];
        switch (dst.cls->fields[fi].kind) {
        case Kind::Bool: d.b = s.b; break;
        case Kind::U32: case Kind::S32: d.u = s.u; break;
        case Kind::F32: d.f = s.f; break;
        case Kind::Str: break;
        case Kind::Obj:
            if (d.obj && s.obj) take_values(*d.obj, *s.obj);
            break;
        case Kind::Arr:
            for (size_t i = 0; i < d.arr.size() && i < s.arr.size(); ++i)
                if (d.arr[i] && s.arr[i]) take_values(*d.arr[i], *s.arr[i]);
            break;
        }
    }
}

}  // namespace

std::vector<std::pair<std::string, NodeRef>> clusters(const NodeRef& tree) {
    std::vector<std::pair<std::string, NodeRef>> out;
    const rsz::Value* parts = tree ? tree->field("Parts") : nullptr;
    if (!parts) return out;
    std::map<std::string, int> seen;                            // "<part>/<cluster>" -> clusters so far
    for (const NodeRef& part : parts->arr) {
        const rsz::Value* cls = part ? part->field("Clusters") : nullptr;
        if (!cls) continue;
        const rsz::Value* type = part->field("Type");
        const int t = type ? (int)(int32_t)type->u : 0;
        const std::string kind = (t >= 0 && t <= 3) ? std::to_string(t) : std::string("acc");
        for (const NodeRef& cl : cls->arr) {
            if (!cl) continue;
            const rsz::Value* name = cl->field("Name");
            const std::string base = kind + "/" + lower(name ? name->s : std::string());
            out.push_back({ base + "#" + std::to_string(++seen[base]), cl });
        }
    }
    return out;
}

Pasted paste(const NodeRef& to, const NodeRef& from) {
    Pasted r;
    const auto copied = clusters(from);
    r.total = (int)copied.size();
    if (!to || copied.empty()) return r;
    std::unordered_map<std::string, NodeRef> target;
    for (auto& [name, cl] : clusters(to)) target.emplace(name, cl);
    for (auto& [name, src] : copied) {
        auto it = target.find(name);
        if (it == target.end() || it->second->cls != src->cls) continue;
        const NodeRef& dst = it->second;
        take_values(*dst, *src);
        ++r.pasted;
        const rsz::Value* dc = dst->field("CustomizeColors");
        const rsz::Value* sc = src->field("CustomizeColors");
        if (dc && sc)
            for (size_t k = 0; k < dc->arr.size() && k < sc->arr.size(); ++k)
                if (dc->arr[k] && sc->arr[k] && dc->arr[k]->cls == sc->arr[k]->cls) r.colours.push_back(dc->arr[k].get());
    }
    return r;
}

}  // namespace dialogs::editor::whole
