// dialogs/whole_colour: the colour editor's whole colour paste, matched by the structure's names (same outfit: every
// part; another outfit or character: the parts both have; nothing copied: nothing). Build: build_whole_colour_test.bat
#include <cstdio>
#include <string>
#include <vector>

#include "../src/dialogs/whole_colour.hpp"

using namespace engine::rsz;
namespace whole = dialogs::editor::whole;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-90s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

// ---- trees as the game's colour files hold them (app.CostumeMaterialData)
static NodeRef node(const char* cls) { return make_node(spec_by_name(cls)); }
static NodeRef value(const char* cls, float v) { NodeRef n = node(cls); n->field("Enable")->b = true; n->field("Value")->f = v; return n; }
static NodeRef colour(uint32_t rgba, bool on, float blend) {
    NodeRef cc = node("app.CostumeMaterialData.CustomizeColorData");
    cc->field("Enable")->b = on;
    cc->field("Color")->u = rgba;
    NodeRef opt = node("app.CostumeMaterialData.CustomizeColorData.ColorOption");
    opt->field("BlendRate")->obj = value("app.CostumeMaterialData.FloatData", blend);
    opt->field("Roughness")->obj = value("app.CostumeMaterialData.RoughnessData", blend / 2);
    opt->field("Metalness")->obj = value("app.CostumeMaterialData.MetalnessData", blend / 4);
    cc->field("Option")->obj = opt;
    return cc;
}
// a cluster of n colours from `seed`, with its Emissive and a hair specular colour in its Body
static NodeRef cluster(const std::string& name, int n, uint32_t seed, bool body = true) {
    NodeRef cl = node("app.CostumeMaterialData.ClusterData");
    cl->field("Name")->s = name;
    for (int k = 0; k < n; ++k) cl->field("CustomizeColors")->arr.push_back(colour(0xFF000000u + seed * 0x10101u + (uint32_t)k, k % 2 == 0, (float)((seed + k) % 10) / 10));
    cl->field("Emissive")->obj = value("app.CostumeMaterialData.EmissiveData", (float)(seed % 7));
    if (body) {
        NodeRef b = node("app.CostumeMaterialData.BodyData"), hair = node("app.CostumeMaterialData.Hair"), spec = node("app.CostumeMaterialData.ColorData");
        spec->field("Enable")->b = true; spec->field("Value")->u = 0xFF000000u + seed;
        hair->field("PrimalySpecularColor")->obj = spec;
        b->field("Hair")->obj = hair;
        cl->field("Body")->obj = b;
    }
    return cl;
}
static NodeRef part(int type, std::vector<NodeRef> cls) {
    NodeRef p = node("app.CostumeMaterialData.MaterialData");
    p->field("Type")->u = (uint32_t)type;
    p->field("Clusters")->arr = std::move(cls);
    return p;
}
static NodeRef tree(std::vector<NodeRef> parts) { NodeRef t = node("app.CostumeMaterialData"); t->field("Parts")->arr = std::move(parts); return t; }
static NodeRef cl_at(const NodeRef& t, size_t pi, size_t ci) { return t->field("Parts")->arr[pi]->field("Clusters")->arr[ci]; }
static NodeRef cc_at(const NodeRef& t, size_t pi, size_t ci, size_t k) { return cl_at(t, pi, ci)->field("CustomizeColors")->arr[k]; }
static uint32_t col(const NodeRef& cc) { return cc->field("Color")->u; }
static bool same(const NodeRef& a, const NodeRef& b) { NodeRef ta = tree({ part(0, { a }) }), tb = tree({ part(0, { b }) }); return serialize(ta) == serialize(tb); }
// a cluster's values without its name (what a paste copies)
static bool same_values(const NodeRef& a, const NodeRef& b) {
    const std::string na = a->field("Name")->s;
    a->field("Name")->s = b->field("Name")->s;
    const bool r = same(a, b);
    a->field("Name")->s = na;
    return r;
}

// one outfit's colour: body (two clusters), face (with a hair cluster of its own), hair
static NodeRef outfit(uint32_t seed) {
    return tree({ part(0, { cluster("esf_Body00", 2, seed + 1), cluster("esf_Cloth00", 3, seed + 2) }),
                  part(1, { cluster("esf_Head00", 2, seed + 3), cluster("esf_Hair00", 1, seed + 4) }),
                  part(2, { cluster("esf_Hair00", 2, seed + 5) }) });
}

int main() {
    std::printf("same structure (another colour of the same outfit)\n");
    {
        NodeRef from = outfit(10), to = outfit(50);
        const auto r = whole::paste(to, from);
        check(r.pasted == 5 && r.total == 5, "every part pasted: 5 of 5");
        check(serialize(to) == serialize(from), "the target is the copied colour, byte for byte");
        check(r.colours.size() == 10, "its 10 colours listed (the sliders' memory is cleared for them)");
        check(col(cc_at(from, 0, 0, 0)) != col(cc_at(outfit(50), 0, 0, 0)), "(the two colours did differ)");
    }
    std::printf("same names, another order\n");
    {
        NodeRef from = outfit(10);
        NodeRef to = tree({ part(0, { cluster("esf_Cloth00", 3, 60), cluster("esf_Body00", 2, 61) }),
                            part(2, { cluster("esf_Hair00", 2, 62) }),
                            part(1, { cluster("esf_Hair00", 1, 63), cluster("esf_Head00", 2, 64) }) });
        const auto r = whole::paste(to, from);
        check(r.pasted == 5 && r.total == 5, "every part pasted: 5 of 5");
        check(same_values(cl_at(to, 0, 0), cl_at(from, 0, 1)) && same_values(cl_at(to, 0, 1), cl_at(from, 0, 0)), "body clusters by name, not by place");
        check(same_values(cl_at(to, 1, 0), cl_at(from, 2, 0)), "hair part's Hair00 = the copied hair part's");
        check(same_values(cl_at(to, 2, 0), cl_at(from, 1, 1)), "face part's Hair00 = the copied face part's");
        check(cl_at(to, 0, 0)->field("Name")->s == "esf_Cloth00", "names kept");
    }
    std::printf("another structure (another outfit or character): the parts both have\n");
    {
        NodeRef from = outfit(10);
        NodeRef to = tree({ part(0, { cluster("esf_body00", 2, 70), cluster("esf_Pants", 2, 71) }),
                            part(2, { cluster("esf_Hair00", 3, 72, false) }) });
        const NodeRef pants0 = cluster("esf_Pants", 2, 71);
        const uint32_t hair3 = col(cc_at(to, 1, 0, 2));
        const auto r = whole::paste(to, from);
        check(r.pasted == 2 && r.total == 5, "2 of 5 pasted: the body and the hair part's Hair00");
        check(same_values(cl_at(to, 0, 0), cl_at(from, 0, 0)), "esf_body00 took esf_Body00 (names in any case)");
        check(cl_at(to, 0, 0)->field("Name")->s == "esf_body00", "its name kept");
        check(same(cl_at(to, 0, 1), pants0), "esf_Pants (not in the copy) untouched");
        check(col(cc_at(to, 1, 0, 0)) == col(cc_at(from, 2, 0, 0)) && col(cc_at(to, 1, 0, 1)) == col(cc_at(from, 2, 0, 1)),
              "hair part's Hair00 took the copied HAIR part's colours, not the face part's");
        check(col(cc_at(to, 1, 0, 2)) == hair3, "its third colour (the copy has two) untouched");
        check(cl_at(to, 1, 0)->field("Body")->obj == nullptr, "no Body in the target: none added (its structure never changes)");
        check(r.colours.size() == 4, "4 colours written");
    }
    std::printf("names the game repeats, accessory parts numbered differently\n");
    {
        NodeRef from = tree({ part(0, { cluster("esf_ClothA_Top", 1, 80), cluster("esf_ClothA_Top", 1, 81) }),
                              part(4, { cluster("esf_Tume00", 1, 82) }), part(4, { cluster("esf_Tume00", 1, 83) }), part(-1, { cluster("esf_Maracas00", 1, 84) }) });
        NodeRef to = tree({ part(0, { cluster("esf_ClothA_Top", 1, 90), cluster("esf_ClothA_Top", 1, 91) }),
                            part(4, { cluster("esf_Tume00", 1, 92) }), part(5, { cluster("esf_Tume00", 1, 93) }), part(6, { cluster("esf_Maracas00", 1, 94) }) });
        const auto r = whole::paste(to, from);
        check(r.pasted == 5 && r.total == 5, "every part pasted: 5 of 5");
        check(col(cc_at(to, 0, 0, 0)) == col(cc_at(from, 0, 0, 0)) && col(cc_at(to, 0, 1, 0)) == col(cc_at(from, 0, 1, 0)), "repeated name: first to first, second to second");
        check(col(cc_at(to, 2, 0, 0)) == col(cc_at(from, 2, 0, 0)), "accessory 4 / 4 = accessory 4 / 5, in order");
        check(col(cc_at(to, 3, 0, 0)) == col(cc_at(from, 3, 0, 0)), "accessory -1 = accessory 6");
        check(to->field("Parts")->arr[2]->field("Type")->u == 5u && to->field("Parts")->arr[3]->field("Type")->u == 6u, "part types kept");
    }
    std::printf("nothing copied\n");
    {
        NodeRef to = outfit(50);
        const auto before = serialize(to);
        const auto r = whole::paste(to, nullptr);
        check(r.pasted == 0 && r.total == 0 && r.colours.empty(), "0 of 0");
        check(serialize(to) == before, "the target untouched");
        const auto r2 = whole::paste(nullptr, outfit(10));
        check(r2.pasted == 0 && r2.total == 5, "no target: 0 of 5, no crash");
        check(whole::clusters(nullptr).empty() && whole::clusters(tree({})).empty(), "no tree, no parts: no clusters");
    }
    std::printf("the names\n");
    {
        const auto c = whole::clusters(outfit(10));
        std::string all;
        for (auto& [name, cl] : c) all += name + " ";
        check(all == "0/esf_body00#1 0/esf_cloth00#1 1/esf_head00#1 1/esf_hair00#1 2/esf_hair00#1 ", all.c_str());
    }
    if (g_fail) std::printf("%d FAILED\n", g_fail); else std::printf("all ok\n");
    return g_fail ? 1 : 0;
}
