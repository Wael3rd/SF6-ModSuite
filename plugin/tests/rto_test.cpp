// Unit test of plugin/src/rto_data.cpp: the options manifest, the choices file, what a model loads for a
// set of options, the menu rows, the rules between options, the former variant slots.
// Build and run: build_rto_test.bat [costume_options.json] (prints "all ok" and exits 0 when every check
// passes; with a manifest written by the loader, it is parsed and its rows listed too).
#include "../src/rto_data.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace slots::rto;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-78s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

// One modular mod of fighter 28, one slot (folder 006): a root group (base + 3 toggles: chain, colour
// only, texture), a skin group (materials), a dress group (textures), two body groups shipping the
// same mesh (the first one also a texture), 2 variants, 1 former slot.
static const char* kManifest = R"JSON({
  "format": "sf6-costume-options-1", "generated": "2026-10-19 21:45", "loader": "test", "pak": "re_chunk_000.pak.patch_001.pak",
  "layout": "one",
  "mods": [
    {"key": "esf028/test dress", "name": "Test Dress", "source": "x", "defaults_from": "loader", "groups": [
      {"name": "(Test Dress)", "options": [
        {"id": "base0001", "name": "1) Main", "folder": "a", "mode": "base", "apply": "relaunch"},
        {"id": "jigl0001", "name": "more jiggle", "folder": "b", "mode": "toggle", "apply": "live"},
        {"id": "tan00001", "name": "tan", "folder": "c", "mode": "toggle", "apply": "colour"},
        {"id": "pony0001", "name": "hide ponytail", "folder": "d", "mode": "toggle", "apply": "live"}]},
      {"name": "2! Skin", "options": [
        {"id": "skin0001", "name": "damaged", "folder": "e", "mode": "pick", "apply": "live"},
        {"id": "skin0002", "name": "oily", "folder": "f", "mode": "pick", "apply": "relaunch"}]},
      {"name": "3! Dress", "options": [
        {"id": "drss0001", "name": "opaque", "folder": "g", "mode": "pick", "apply": "live"},
        {"id": "drss0002", "name": "fishnet", "folder": "h", "mode": "pick", "apply": "live"}]},
      {"name": "4! Body A", "options": [
        {"id": "bdya0001", "name": "A regular", "folder": "i", "mode": "pick", "apply": "live"},
        {"id": "bdya0002", "name": "A thick", "folder": "j", "mode": "pick", "apply": "live"}]},
      {"name": "4! Body B", "options": [
        {"id": "bdyb0001", "name": "B regular", "folder": "k", "mode": "pick", "apply": "live"},
        {"id": "bdyb0002", "name": "B thick", "folder": "l", "mode": "pick", "apply": "live"}]}
     ], "variants": [
      {"label": "Test Dress - regular", "options": ["base0001", "skin0001", "drss0001", "bdya0001", "bdyb0001"]},
      {"label": "Test Dress - thick", "options": ["base0001", "skin0001", "drss0001", "bdya0001", "bdyb0002"]}]}],
  "slots": [
    {"fighter": 28, "fighter_dir": "esf028", "costume_no": 5, "folder": "006", "name": "Outfit I", "label": "Test Dress",
     "mod": "esf028/test dress",
     "defaults": ["base0001", "skin0001", "drss0001", "bdya0001", "bdyb0001"],
     "options": {
       "jigl0001": [{"kind": "chain", "target": "product/model/esf/esf028/006/esf028_001_01_chain.chain", "path": "product/model/esf/esf028/006/rto/jigl0001/esf028_001_01_chain.chain"}],
       "tan00001": [{"kind": "colour", "target": "product/model/esf/esf028/006/esf028_001_cmd_001.user", "path": "product/model/esf/esf028/006/rto/tan00001/esf028_001_cmd_001.user"}],
       "pony0001": [{"kind": "texture", "target": "product/model/esf/esf028/006/02/tail_atos.tex", "path": "product/model/esf/esf028/006/rto/pony0001/02/tail_atos.tex"}],
       "skin0001": [{"kind": "material", "target": "product/model/esf/esf028/006/01/esf028_006_01_v00.mdf2", "path": "product/model/esf/esf028/006/rto/skin0001/01/esf028_006_01_v00.mdf2"}],
       "skin0002": [{"kind": "material", "target": "product/model/esf/esf028/006/01/esf028_006_01_v00.mdf2", "path": "product/model/esf/esf028/006/rto/skin0002/01/esf028_006_01_v00.mdf2"},
                    {"kind": "other", "target": "product/model/esf/esf028/006/01/esf028_006_01.clsp", "path": "product/model/esf/esf028/006/rto/skin0002/01/esf028_006_01.clsp"}],
       "drss0001": [{"kind": "texture", "target": "product/model/esf/esf028/mini/mini1_atos.tex", "path": "product/model/esf/esf028/006/rto/drss0001/_/mini/mini1_atos.tex"}],
       "drss0002": [{"kind": "texture", "target": "product/model/esf/esf028/mini/mini1_atos.tex", "path": "product/model/esf/esf028/006/rto/drss0002/_/mini/mini1_atos.tex"}],
       "bdya0001": [{"kind": "mesh", "target": "product/model/esf/esf028/006/01/esf028_006_01.mesh", "path": "product/model/esf/esf028/006/rto/bdya0001/01/esf028_006_01.mesh"},
                    {"kind": "texture", "target": "product/model/esf/esf028/mini/royal_atos.tex", "path": "product/model/esf/esf028/006/rto/bdya0001/_/mini/royal_atos.tex"}],
       "bdya0002": [{"kind": "mesh", "target": "product/model/esf/esf028/006/01/esf028_006_01.mesh", "path": "product/model/esf/esf028/006/rto/bdya0002/01/esf028_006_01.mesh"}],
       "bdyb0001": [{"kind": "mesh", "target": "product/model/esf/esf028/006/01/esf028_006_01.mesh", "path": "product/model/esf/esf028/006/rto/bdyb0001/01/esf028_006_01.mesh"}],
       "bdyb0002": [{"kind": "mesh", "target": "Product/Model/esf/esf028/006/01/esf028_006_01.mesh", "path": "natives/stm/product/model/esf/esf028/006/rto/bdyb0002/01/esf028_006_01.mesh"}]
     },
     "base": [{"kind": "mesh", "target": "product/model/esf/esf028/006/01/esf028_006_01.mesh", "path": "product/model/esf/esf028/006/rto/base/01/esf028_006_01.mesh"},
              {"kind": "material", "target": "product/model/esf/esf028/006/01/esf028_006_01_v00.mdf2", "path": "product/model/esf/esf028/006/rto/base/01/esf028_006_01_v00.mdf2"},
              {"kind": "texture", "target": "product/model/esf/esf028/mini/mini1_atos.tex", "path": "product/model/esf/esf028/006/rto/base/_/mini/mini1_atos.tex"},
              {"kind": "texture", "target": "product/model/esf/esf028/mini/royal_atos.tex", "path": "product/model/esf/esf028/006/rto/base/_/mini/royal_atos.tex"}],
     "former": [{"costume_no": 6, "folder": "007", "label": "Test Dress - thick", "options": ["base0001", "skin0001", "drss0001", "bdya0001", "bdyb0002"]}]}
  ]
})JSON";

static const char* T_MESH = "product/model/esf/esf028/006/01/esf028_006_01.mesh";
static const char* T_MDF2 = "product/model/esf/esf028/006/01/esf028_006_01_v00.mdf2";
static const char* T_DRESS = "product/model/esf/esf028/mini/mini1_atos.tex";
static const char* T_ROYAL = "product/model/esf/esf028/mini/royal_atos.tex";
static const char* T_TAIL = "product/model/esf/esf028/006/02/tail_atos.tex";
static const char* T_CHAIN = "product/model/esf/esf028/006/esf028_001_01_chain.chain";

static bool has(const std::vector<std::string>& v, const char* id) {
    for (auto& s : v) if (s == id) return true;
    return false;
}
static bool is_orig(const std::map<std::string, Want>& d, const char* t) { auto it = d.find(t); return it != d.end() && it->second.orig; }
static std::string path_of(const std::map<std::string, Want>& d, const char* t) {
    auto it = d.find(t);
    return (it == d.end() || it->second.orig) ? std::string() : it->second.path;
}

static int real_manifest(const char* file) {
    std::ifstream f(file, std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    Manifest m; std::string err;
    if (!parse_manifest(ss.str(), m, err)) { std::printf("%s: %s\n", file, err.c_str()); return 1; }
    std::printf("%s: layout %s, %zu mods, %zu slots\n", file, m.layout.c_str(), m.mods.size(), m.slots.size());
    for (auto& s : m.slots) {
        auto d = desired(m, s, s.defaults);
        size_t changed = 0;
        for (auto& [t, w] : d) if (!w.orig) ++changed;
        std::printf("  slot %s/%s (costume %d) %s: %zu targets, %zu not as built with its defaults, %zu former\n",
                    s.fighter_dir.c_str(), s.folder.c_str(), s.costume_no, s.label.c_str(), d.size(), changed, s.former.size());
        for (auto& r : rows(m, s, s.defaults))
            std::printf("    %-6s %-48s %zu labels, value %d%s\n", r.kind == Row::Pick ? "pick" : "toggle", r.title.c_str(),
                        r.labels.size(), r.value, r.partial ? " (partial)" : "");
    }
    return 0;
}

int main(int argc, char** argv) {
    Manifest m; std::string err;
    check(parse_manifest(kManifest, m, err), "manifest parsed");
    check(m.mods.size() == 1 && m.slots.size() == 1, "1 mod, 1 slot");
    const Slot& s = m.slots[0];
    const Mod& mod = *m.mod_of(s);
    check(s.prefix == "product/model/esf/esf028/006/", "slot folder prefix");
    check(m.slot_of_path("product/model/esf/esf028/006/01/esf028_006_01.mesh") == &s, "slot found from a model path");
    check(m.slot_of_path("product/model/esf/esf028/007/01/esf028_007_01.mesh") == nullptr, "another folder: no slot");
    check(m.slot_of(28, 5) == &s && m.slot_of(28, 6) == nullptr, "slot by fighter and costume number");
    check(s.rev.count("product/model/esf/esf028/006/rto/bdyb0002/01/esf028_006_01.mesh") == 1
          && s.rev.at("product/model/esf/esf028/006/rto/bdyb0002/01/esf028_006_01.mesh") == T_MESH,
          "paths normalised (case, natives/stm/), stored path -> target");
    check(mod.options[static_cast<size_t>(mod.find("bdyb0002"))].rank > mod.options[static_cast<size_t>(mod.find("bdya0001"))].rank,
          "layering rank follows the groups");
    check(norm_path("Natives\\STM\\Product/Model/A.mesh") == "product/model/a.mesh", "norm_path");

    {
        auto d = desired(m, s, s.defaults);
        bool all = true;
        for (auto& [t, w] : d) all = all && w.orig;
        check(all && d.size() == 6, "defaults: every target is the slot's own file (6 live targets)");
        check(d.count("product/model/esf/esf028/006/esf028_001_cmd_001.user") == 0, "colour files are not model targets");
    }
    {
        auto c = s.defaults;
        choose(m, s, c, 2, "drss0002");
        check(!has(c, "drss0001") && has(c, "drss0002"), "pick: the group's other option goes");
        auto d = desired(m, s, c);
        check(path_of(d, T_DRESS) == "product/model/esf/esf028/006/rto/drss0002/_/mini/mini1_atos.tex", "dress texture -> the option's copy");
        check(is_orig(d, T_MESH) && is_orig(d, T_MDF2), "the rest unchanged");
        choose(m, s, c, 2, "");
        d = desired(m, s, c);
        check(path_of(d, T_DRESS) == "product/model/esf/esf028/006/rto/base/_/mini/mini1_atos.tex", "dress None -> base copy");
    }
    {
        auto c = s.defaults;
        choose(m, s, c, 4, "bdyb0002");                     // the later body group
        check(has(c, "bdya0001") && has(c, "bdyb0002") && !has(c, "bdyb0001"), "later group picked: the earlier one stays");
        auto d = desired(m, s, c);
        check(path_of(d, T_MESH) == "product/model/esf/esf028/006/rto/bdyb0002/01/esf028_006_01.mesh", "its mesh wins");
        check(is_orig(d, T_ROYAL), "the earlier option's texture stays as built");
        check(preset_of(mod, c) == 2, "this set is the preset 'thick'");
        choose(m, s, c, 3, "bdya0002");                     // the earlier body group
        check(!has(c, "bdyb0002") && has(c, "bdya0002"), "earlier group picked: the later option shipping its mesh goes");
        d = desired(m, s, c);
        check(path_of(d, T_MESH) == "product/model/esf/esf028/006/rto/bdya0002/01/esf028_006_01.mesh", "its mesh shows");
        check(path_of(d, T_ROYAL) == "product/model/esf/esf028/006/rto/base/_/mini/royal_atos.tex", "texture of the option left -> base copy");
        check(preset_of(mod, c) == 0, "no preset matches");
    }
    {
        auto c = s.defaults;
        toggle(m, s, c, "jigl0001", true);
        toggle(m, s, c, "pony0001", true);
        auto d = desired(m, s, c);
        check(path_of(d, T_CHAIN) == "product/model/esf/esf028/006/rto/jigl0001/esf028_001_01_chain.chain", "toggle on: chain");
        check(path_of(d, T_TAIL) == "product/model/esf/esf028/006/rto/pony0001/02/tail_atos.tex", "toggle on: texture");
        toggle(m, s, c, "pony0001", false);
        std::vector<std::string> notes;
        d = desired(m, s, c, &notes);
        check(is_orig(d, T_TAIL), "toggle off (not a default): the slot's own file");
    }
    {
        auto c = s.defaults;
        auto r = rows(m, s, c);
        // root: jiggle, ponytail (tan is colour only); skin; dress; body A; body B
        check(r.size() == 6, "6 rows (the colour-only toggle is not offered)");
        check(r.size() == 6 && r[0].kind == Row::Toggle && r[0].title == "more jiggle" && r[0].value == 1, "toggle row, off");
        check(r.size() == 6 && r[2].kind == Row::Pick && r[2].labels.size() == 3 && r[2].value == 2, "skin row: None + 2, 'damaged' chosen");
        check(r.size() == 6 && r[2].labels[2] == "oily *" && r[2].partial, "an option with files the model cannot swap is marked");
        check(!offered(s, "tan00001") && !offered(s, "base0001"), "colour-only and base options not offered");
        apply_row(m, s, c, r[2], 3);
        check(has(c, "skin0002") && !has(c, "skin0001"), "row value -> pick");
        apply_row(m, s, c, r[0], 2);
        check(has(c, "jigl0001"), "row value -> toggle on");
        apply_row(m, s, c, r[2], 1);
        check(!has(c, "skin0001") && !has(c, "skin0002"), "row None -> no option of the group");
        check(has(c, "base0001"), "base option kept");
    }
    {   // a group of 20 options is split in rows of at most 15 labels
        Manifest big; std::string e2;
        std::string j = R"({"format": "sf6-costume-options-1", "layout": "one", "mods": [{"key": "k", "name": "N", "groups": [{"name": "G", "options": [)";
        std::string files;
        for (int i = 0; i < 20; ++i) {
            char id[16]; std::snprintf(id, sizeof id, "o%07d", i);
            j += std::string(i ? "," : "") + "{\"id\": \"" + id + "\", \"name\": \"n" + std::to_string(i) + "\", \"mode\": \"pick\"}";
            files += std::string(i ? "," : "") + "\"" + id + "\": [{\"kind\": \"texture\", \"target\": \"product/model/esf/esf001/005/t.tex\", \"path\": \"product/model/esf/esf001/005/rto/" + id + "/t.tex\"}]";
        }
        j += R"(]}], "variants": []}], "slots": [{"fighter": 1, "fighter_dir": "esf001", "costume_no": 4, "folder": "005", "mod": "k", "defaults": ["o0000017"], "options": {)" + files + "}}]}";
        check(parse_manifest(j, big, e2), "manifest with a 20-option group");
        auto r = rows(big, big.slots[0], big.slots[0].defaults);
        check(r.size() == 2 && r[0].labels.size() == 15 && r[1].labels.size() == 7, "split in 2 rows (15 + 7 labels)");
        check(r.size() == 2 && r[0].value == 1 && r[1].value == 5, "the choice shows on the second row only");
        auto c = big.slots[0].defaults;
        apply_row(big, big.slots[0], c, r[0], 1);
        check(has(c, "o0000017"), "None on the other row keeps the choice");
        apply_row(big, big.slots[0], c, r[1], 1);
        check(c.empty(), "None on its own row clears it");
    }
    {   // a required group (the loader found no base copy for a file of its options): no "None"
        std::string j = kManifest;
        const std::string at = "\"former\": [";
        j.replace(j.find(at), at.size(), "\"required\": [1], " + at);
        Manifest q; std::string e;
        check(parse_manifest(j, q, e) && q.slots[0].required == std::vector<int>{ 1 }, "required groups parsed");
        const Slot& t = q.slots[0];
        check(required(t, 1) && !required(t, 2), "skin required, dress not");
        auto c = t.defaults;
        auto r = rows(q, t, c);
        check(r.size() == 6 && !r[2].none && r[2].labels.size() == 2 && r[2].labels[0] == "damaged" && r[2].value == 1,
              "required skin row: its 2 options, no None, 'damaged' chosen");
        check(r.size() == 6 && r[3].none && r[3].labels[0] == "None", "dress row keeps None");
        apply_row(q, t, c, r[2], 2);
        check(has(c, "skin0002") && !has(c, "skin0001"), "required row value -> pick");
        Choices ch;
        auto& sd = ch.mods["esf028/test dress"][0];
        sd.changed = true;
        sd.ids = { "base0001", "drss0002" };                // saved with no skin (None before the group was required)
        auto p1 = chosen_of(q, t, ch, 0);
        check(has(p1, "skin0001") && has(p1, "drss0002"), "a saved None of a required group -> its default option");
        Slot u = t;
        u.defaults = { "base0001", "drss0001" };
        check(default_pick(q, u, 1) == "skin0001" && has(chosen_of(q, u, Choices{}, 0), "skin0001"),
              "no default in the group: its first offered option");
        auto d = desired(q, t, p1);
        check(is_orig(d, T_MDF2), "the default skin: the slot's own material file");
    }
    {   // a required group split in rows: the first label says the choice is on another row
        Manifest big; std::string e2;
        std::string j = R"({"format": "sf6-costume-options-1", "layout": "one", "mods": [{"key": "k", "name": "N", "groups": [{"name": "G", "options": [)";
        std::string files;
        for (int i = 0; i < 20; ++i) {
            char id[16]; std::snprintf(id, sizeof id, "o%07d", i);
            j += std::string(i ? "," : "") + "{\"id\": \"" + id + "\", \"name\": \"n" + std::to_string(i) + "\", \"mode\": \"pick\"}";
            files += std::string(i ? "," : "") + "\"" + id + "\": [{\"kind\": \"texture\", \"target\": \"product/model/esf/esf001/005/t.tex\", \"path\": \"product/model/esf/esf001/005/rto/" + id + "/t.tex\"}]";
        }
        j += R"(]}], "variants": []}], "slots": [{"fighter": 1, "fighter_dir": "esf001", "costume_no": 4, "folder": "005", "mod": "k", "required": [0], "defaults": ["o0000017"], "options": {)" + files + "}}]}";
        check(parse_manifest(j, big, e2), "manifest with a required 20-option group");
        const Slot& t = big.slots[0];
        auto r = rows(big, t, t.defaults);
        check(r.size() == 2 && r[0].labels[0] == "(other row)" && r[0].labels.size() == 15 && r[1].labels.size() == 7,
              "split in 2 rows, first label '(other row)'");
        auto c = t.defaults;
        apply_row(big, t, c, r[0], 4);
        check(has(c, "o0000002") && c.size() == 1, "pick on the first row");
        apply_row(big, t, c, r[0], 1);
        check(has(c, "o0000017") && c.size() == 1, "'(other row)' on the row holding the choice -> the default option");
    }
    {
        check(preset_label(mod, 0) == "regular" && preset_label(mod, 1) == "thick", "preset labels without the mod's name");
        check(preset_of(mod, s.defaults) == 1, "defaults = preset 'regular'");
    }
    {
        Choices c;
        auto& sd = c.mods["esf028/test dress"][1];
        sd.changed = true;
        sd.ids = { "base0001", "skin0002", "gone0001", "xxxxxxxx" };
        sd.names = { "1) Main", "oily", "gone", "fishnet" };
        c.former_done = "2026-10-19 21:45 re_chunk_000.pak.patch_001.pak";
        std::vector<std::string> notes;
        sanitize(c, m, notes);
        check(sd.ids.size() == 3 && sd.ids[2] == "drss0002", "sanitize: unknown id found by its name, the other dropped");
        check(notes.size() == 2, "sanitize: one note each");
        std::string text = write_choices(c, m);
        Choices back;
        check(parse_choices(text, back), "choices written then read");
        check(back.former_done == c.former_done, "former_done kept");
        check(!back.mods["esf028/test dress"][0].changed && back.mods["esf028/test dress"][1].changed, "P1 untouched, P2 changed");
        check(back.mods["esf028/test dress"][1].ids == sd.ids && back.mods["esf028/test dress"][1].names[1] == "oily", "ids and names");
        check(same_set(chosen_of(m, s, back, 0), s.defaults), "P1 wears the defaults");
        auto p2 = chosen_of(m, s, back, 1);
        check(has(p2, "skin0002") && has(p2, "drss0002") && has(p2, "base0001") && p2.size() == 3, "P2 wears its choice");
        back.mods["esf028/test dress"][1].ids = { "skin0002" };
        check(has(chosen_of(m, s, back, 1), "base0001"), "the base option is always worn");
        Choices bad;
        check(!parse_choices("{\"format\": \"other\"}", bad), "another format is refused");
    }
    {
        const Former* f = nullptr;
        const Slot* k = former_of(m, 28, 6, &f);
        check(k == &s && f && f->folder == "007", "former slot 6 -> kept slot 5");
        check(f && same_set(f->ids, m.mods[0].variants[1].ids), "with the options of its variant");
        check(former_of(m, 28, 5, &f) == nullptr && former_of(m, 1, 6, &f) == nullptr, "no former for the kept slot or another fighter");
        Manifest v = m; v.layout = "variants";
        check(former_of(v, 28, 6, &f) == nullptr, "layout 'variants': no migration");
    }
    {
        Manifest x; std::string e3;
        check(!parse_manifest("{\"format\": \"sf6-costume-options-0\"}", x, e3), "another manifest format is refused");
        check(!parse_manifest("", x, e3), "no manifest");
    }
    {   // an outfit with add-ons, as the loader writes it (Leon Requiem On Luke, 20/10): the outfit is the base,
        // "More muscles" a toggle, two hairs replacing the same files one pick group with None; the hairs target
        // the part the slot's scene loads (the game's hair in 000/02/), one required group from the author
        const char* j = R"JSON({"format": "sf6-costume-options-1", "layout": "one", "mods": [
          {"key": "esf002/leon as luke", "name": "Leon as Luke", "groups": [
            {"name": "Leon as Luke", "options": [{"id": "c47d9bac", "name": "Leon as Luke", "mode": "base"}]},
            {"name": "Add-ons", "options": [{"id": "cf9f838f", "name": "Optional - More muscles", "mode": "toggle"}]},
            {"name": "Hair", "options": [{"id": "84fa84c5", "name": "Optional - Leon Hair", "mode": "pick"},
                                         {"id": "87a803a0", "name": "Optional - Trimmed Hair", "mode": "pick"}]},
            {"name": "2! Eyes", "options": [{"id": "e0000001", "name": "blue", "mode": "pick"},
                                            {"id": "e0000002", "name": "grey", "mode": "pick"}]}],
           "variants": [{"label": "Leon as Luke", "options": ["c47d9bac"]},
                        {"label": "Leon as Luke + Optional - Leon Hair", "options": ["c47d9bac", "84fa84c5"]}]}],
          "slots": [{"fighter": 2, "fighter_dir": "esf002", "costume_no": 5, "folder": "006", "mod": "esf002/leon as luke",
            "defaults": ["c47d9bac", "e0000001"],
            "options": {
              "cf9f838f": [{"kind": "mesh", "target": "product/model/esf/esf002/006/01/esf002_006_01.mesh", "path": "product/model/esf/esf002/006/rto/cf9f838f/01/esf002_006_01.mesh"}],
              "84fa84c5": [{"kind": "mesh", "target": "product/model/esf/esf002/000/02/esf002_000_02.mesh", "path": "product/model/esf/esf002/006/rto/84fa84c5/_/000/02/esf002_000_02.mesh"},
                           {"kind": "material", "target": "product/model/esf/esf002/000/02/esf002_000_02.mdf2", "path": "product/model/esf/esf002/006/rto/84fa84c5/_/000/02/esf002_000_02.mdf2"}],
              "87a803a0": [{"kind": "mesh", "target": "product/model/esf/esf002/000/02/esf002_000_02.mesh", "path": "product/model/esf/esf002/006/rto/87a803a0/_/000/02/esf002_000_02.mesh"},
                           {"kind": "material", "target": "product/model/esf/esf002/000/02/esf002_000_02.mdf2", "path": "product/model/esf/esf002/006/rto/87a803a0/_/000/02/esf002_000_02.mdf2"}],
              "e0000001": [{"kind": "texture", "target": "product/model/esf/esf002/006/00/eye.tex", "path": "product/model/esf/esf002/006/rto/e0000001/00/eye.tex"}],
              "e0000002": [{"kind": "texture", "target": "product/model/esf/esf002/006/00/eye.tex", "path": "product/model/esf/esf002/006/rto/e0000002/00/eye.tex"}]},
            "base": [], "required": [3], "former": [{"costume_no": 7, "folder": "008", "label": "Leon as Luke + Optional - Leon Hair", "options": ["c47d9bac", "84fa84c5"]}]}]})JSON";
        Manifest q; std::string e4;
        check(parse_manifest(j, q, e4), "add-on manifest parsed");
        const Slot& t = q.slots[0];
        auto c = t.defaults;
        complete(q, t, c);
        check(same_set(c, t.defaults), "add-ons: the outfit alone by default (the required group has its default)");
        auto d = desired(q, t, c);
        bool all = true;
        for (auto& [k, w] : d) all = all && w.orig;
        check(all, "add-ons: defaults change nothing on the model");
        choose(q, t, c, 2, "84fa84c5");
        toggle(q, t, c, "cf9f838f", true);
        d = desired(q, t, c);
        check(path_of(d, "product/model/esf/esf002/000/02/esf002_000_02.mesh") == "product/model/esf/esf002/006/rto/84fa84c5/_/000/02/esf002_000_02.mesh"
              && path_of(d, "product/model/esf/esf002/006/01/esf002_006_01.mesh") == "product/model/esf/esf002/006/rto/cf9f838f/01/esf002_006_01.mesh",
              "add-ons: a hair on the game's hair part, the muscles on the body");
        choose(q, t, c, 2, "87a803a0");
        check(!has(c, "84fa84c5") && has(c, "87a803a0") && has(c, "cf9f838f"), "add-ons: the other hair replaces it, the toggle stays");
        choose(q, t, c, 2, "");
        d = desired(q, t, c);
        check(is_orig(d, "product/model/esf/esf002/000/02/esf002_000_02.mesh"), "add-ons: hair None -> the slot's own hair");
        auto r = rows(q, t, t.defaults);
        check(r.size() == 3 && r[0].kind == Row::Toggle && r[0].title == "Optional - More muscles", "add-ons: a toggle row");
        check(r.size() == 3 && r[1].kind == Row::Pick && r[1].none && r[1].labels.size() == 3 && r[1].value == 1, "add-ons: hair row None + 2, None chosen");
        check(r.size() == 3 && r[2].kind == Row::Pick && !r[2].none && r[2].labels.size() == 2 && r[2].value == 1, "author's 'exactly one' group: no None");
        Choices ch;
        ch.mods["esf002/leon as luke"][0] = Side{true, {"c47d9bac"}, {}};   // saved without any eyes option
        auto cc = chosen_of(q, t, ch, 0);
        check(has(cc, "e0000001"), "a required group gets its default back");
        const Former* fo = nullptr;
        check(former_of(q, 2, 7, &fo) == &t && fo && fo->ids.size() == 2, "the former add-on outfit's slot moves to the outfit");
    }

    {   // part units, as the loader writes them (20/10): Ken SFV Hair V2 = the hair's mesh, material and chain worn by
        // default, the game's hair as one unit (base copies with "unit"); a second toggle without any base copy
        const char* j = R"JSON({"format": "sf6-costume-options-1", "layout": "one", "mods": [
          {"key": "esf010/kensfv", "name": "KenSFV", "groups": [
            {"name": "KenSFV", "options": [{"id": "aa404dd9", "name": "KenSFV", "mode": "base"}]},
            {"name": "Add-ons", "options": [{"id": "86fddfb3", "name": "Ken SFV Hair V2", "mode": "toggle"},
                                            {"id": "0000beef", "name": "Prop", "mode": "toggle"}]}]}],
          "slots": [{"fighter": 10, "fighter_dir": "esf010", "costume_no": 9, "folder": "010", "mod": "esf010/kensfv",
            "defaults": ["aa404dd9", "86fddfb3", "0000beef"],
            "options": {
              "86fddfb3": [
                {"kind": "mesh", "target": "product/model/esf/esf010/010/02/esf010_010_02.mesh", "path": "product/model/esf/esf010/010/rto/86fddfb3/02/esf010_010_02.mesh", "unit": "product/model/esf/esf010/010/02/esf010_002_02_chain.chain", "joints": 62},
                {"kind": "chain", "target": "product/model/esf/esf010/010/02/esf010_002_02_chain.chain", "path": "product/model/esf/esf010/010/rto/86fddfb3/02/esf010_002_02_chain.chain", "unit": "product/model/esf/esf010/010/02/esf010_002_02_chain.chain"},
                {"kind": "material", "target": "product/model/esf/esf010/010/02/esf010_010_02_v00.mdf2", "path": "product/model/esf/esf010/010/rto/86fddfb3/02/esf010_010_02_v00.mdf2", "unit": "product/model/esf/esf010/010/02/esf010_002_02_chain.chain"},
                {"kind": "texture", "target": "product/model/esf/esf010/010/02/esf010_010_02_hai2_albd.tex", "path": "product/model/esf/esf010/010/rto/86fddfb3/02/esf010_010_02_hai2_albd.tex"}],
              "0000beef": [
                {"kind": "mesh", "target": "product/model/esf/esf010/010/30/esf010_010_30.mesh", "path": "product/model/esf/esf010/010/rto/0000beef/30/esf010_010_30.mesh"}]},
            "base": [
              {"kind": "mesh", "target": "product/model/esf/esf010/010/02/esf010_010_02.mesh", "path": "product/model/esf/esf010/010/rto/base/02/esf010_010_02.mesh", "unit": "product/model/esf/esf010/010/02/esf010_002_02_chain.chain", "joints": 131, "own_joints": 62},
              {"kind": "chain", "target": "product/model/esf/esf010/010/02/esf010_002_02_chain.chain", "path": "product/model/esf/esf010/010/rto/base/02/esf010_002_02_chain.chain", "unit": "product/model/esf/esf010/010/02/esf010_002_02_chain.chain"},
              {"kind": "material", "target": "product/model/esf/esf010/010/02/esf010_010_02_v00.mdf2", "path": "product/model/esf/esf010/010/rto/base/02/esf010_010_02_v00.mdf2", "unit": "product/model/esf/esf010/010/02/esf010_002_02_chain.chain"}],
            "required": [], "former": []}]})JSON";
        Manifest q; std::string e5;
        check(parse_manifest(j, q, e5), "part units: manifest parsed");
        const Slot& t = q.slots[0];
        const std::string U = "product/model/esf/esf010/010/02/esf010_002_02_chain.chain";
        check(t.base.size() == 3 && t.base[0].unit == U && t.base[0].joints == 131, "part units: unit and joints parsed");
        auto c = t.defaults;
        toggle(q, t, c, "86fddfb3", false);
        auto d = desired(q, t, c);
        check(path_of(d, "product/model/esf/esf010/010/02/esf010_010_02.mesh") == "product/model/esf/esf010/010/rto/base/02/esf010_010_02.mesh"
              && path_of(d, U.c_str()) == "product/model/esf/esf010/010/rto/base/02/esf010_002_02_chain.chain"
              && path_of(d, "product/model/esf/esf010/010/02/esf010_010_02_v00.mdf2") == "product/model/esf/esf010/010/rto/base/02/esf010_010_02_v00.mdf2",
              "part units: Off -> the game's mesh, material and chain");
        check(d["product/model/esf/esf010/010/02/esf010_010_02.mesh"].unit == U && d[U].unit == U, "part units: the wanted files say their unit");
        check(is_orig(d, "product/model/esf/esf010/010/02/esf010_010_02_hai2_albd.tex"), "part units: the add-on's own texture is left (nothing names it)");
        check(removable(q, t, "86fddfb3"), "part units: the hair can be turned off");
        check(!removable(q, t, "0000beef"), "a default toggle without any base copy cannot be turned off");
        auto r = rows(q, t, t.defaults);
        check(r.size() == 1 && r[0].kind == Row::Toggle && r[0].title == "Ken SFV Hair V2" && r[0].value == 2,
              "only the hair row (the toggle Off cannot change is not offered)");
        check(joints_at(q, t, "product/model/esf/esf010/010/rto/base/02/esf010_010_02.mesh") == 131
              && joints_at(q, t, "product/model/esf/esf010/010/02/esf010_010_02.mesh") == 62
              && joints_at(q, t, "product/model/esf/esf010/010/02/other.mesh") == -1, "joints of a copy, of the slot's own mesh");
    }

    {   // a game outfit given a separate add-on's options ("game": true): no base copy, nothing worn by default; a mod
        // slot made from that outfit that keeps a part in the game's folder is still identified as the mod slot
        const char* j = R"JSON({"format": "sf6-costume-options-1", "generated": "g", "loader": "t", "pak": "p", "layout": "one",
          "mods": [
            {"key": "esf028/game outfit 2", "name": "Outfit 2", "source": "Mai/face.zip", "defaults_from": "game", "groups": [
              {"name": "!C2 Mai face accessories", "options": [
                {"id": "c2main01", "name": "1) Main files - C2", "folder": "a", "mode": "pick", "apply": "live"},
                {"id": "c2glas01", "name": "2) Glasses only", "folder": "b", "mode": "pick", "apply": "live"}]},
              {"name": "Mai face accessories", "options": [
                {"id": "wrnk0001", "name": "3) addon - remove wrinkles", "folder": "c", "mode": "toggle", "apply": "live"}]}],
             "variants": []},
            {"key": "esf028/mod", "name": "Mod", "source": "x", "defaults_from": "loader", "groups": [
              {"name": "Mod", "options": [{"id": "modbase1", "name": "Mod", "folder": "m", "mode": "base", "apply": "live"}]},
              {"name": "Add-ons", "options": [{"id": "modadd01", "name": "Add", "folder": "n", "mode": "toggle", "apply": "live"}]}],
             "variants": []}],
          "slots": [
            {"fighter": 28, "fighter_dir": "esf028", "costume_no": 1, "folder": "002", "name": "Outfit 2", "label": "Outfit 2",
             "mod": "esf028/game outfit 2", "game": true, "defaults": [],
             "options": {
               "c2main01": [{"kind": "texture", "target": "product/model/esf/esf028/002/00/esf028_002_00_head_albd.tex", "path": "product/model/esf/esf028/002/rto/c2main01/00/esf028_002_00_head_albd.tex"}],
               "c2glas01": [{"kind": "mesh", "target": "product/model/esf/esf028/002/00/esf028_002_00.mesh", "path": "product/model/esf/esf028/002/rto/c2glas01/00/esf028_002_00.mesh"},
                            {"kind": "texture", "target": "product/model/esf/esf028/002/00/esf028_002_00_head_albd.tex", "path": "product/model/esf/esf028/002/rto/c2glas01/00/esf028_002_00_head_albd.tex"}],
               "wrnk0001": [{"kind": "texture", "target": "product/model/esf/esf028/000/00/esf028_000_00_facialblend.tex", "path": "product/model/esf/esf028/002/rto/wrnk0001/_/000/00/esf028_000_00_facialblend.tex"}]},
             "base": [], "required": [], "former": []},
            {"fighter": 28, "fighter_dir": "esf028", "costume_no": 6, "folder": "007", "name": "Outfit II", "label": "Mod",
             "mod": "esf028/mod", "defaults": ["modbase1"],
             "options": {"modadd01": [{"kind": "mesh", "target": "product/model/esf/esf028/007/30/esf028_007_30.mesh", "path": "product/model/esf/esf028/007/rto/modadd01/30/esf028_007_30.mesh"}]},
             "base": [], "required": [], "former": []}]})JSON";
        Manifest q; std::string e6;
        check(parse_manifest(j, q, e6) && q.slots.size() == 2, "game outfit: manifest parsed");
        const Slot& t = q.slots[0];
        check(t.game && !q.slots[1].game, "game outfit: \"game\" read");
        check(q.slot_of_paths({ "product/model/esf/esf028/002/01/esf028_002_01.mesh", "product/model/esf/esf028/007/01/esf028_007_01.mesh" }) == &q.slots[1],
              "a model with a part in the game's folder and its body in ours wears our slot");
        check(q.slot_of_paths({ "product/model/esf/esf028/002/00/esf028_002_00.mesh", "product/model/esf/esf028/002/01/esf028_002_01.mesh" }) == &t,
              "a model loading the game outfit's files wears the game outfit");
        check(q.slot_of_paths({ "product/model/esf/esf028/002/rto/c2glas01/00/esf028_002_00.mesh", "product/model/esf/esf028/002/01/esf028_002_01.mesh" }) == &t,
              "a model wearing an option's copy still wears the game outfit");
        check(q.slot_of_paths({ "product/model/esf/esf028/000/00/esf028_000_00.mesh", "product/model/esf/esf028/002/01/esf028_002_01.mesh" }) == nullptr,
              "another game outfit sharing the folder (not the mesh the options replace): none");
        check(q.slot_of_paths({ "product/model/esf/esf028/001/01/esf028_001_01.mesh" }) == nullptr, "another game outfit: none");
        Choices none;
        auto c = chosen_of(q, t, none, 0);
        check(c.empty(), "game outfit: nothing worn by default");
        auto d = desired(q, t, c);
        check(is_orig(d, "product/model/esf/esf028/002/00/esf028_002_00.mesh") && is_orig(d, "product/model/esf/esf028/000/00/esf028_000_00_facialblend.tex"),
              "game outfit: without options, the game's own files");
        choose(q, t, c, 0, "c2glas01");
        toggle(q, t, c, "wrnk0001", true);
        d = desired(q, t, c);
        check(path_of(d, "product/model/esf/esf028/002/00/esf028_002_00.mesh") == "product/model/esf/esf028/002/rto/c2glas01/00/esf028_002_00.mesh"
              && path_of(d, "product/model/esf/esf028/002/00/esf028_002_00_head_albd.tex") == "product/model/esf/esf028/002/rto/c2glas01/00/esf028_002_00_head_albd.tex"
              && path_of(d, "product/model/esf/esf028/000/00/esf028_000_00_facialblend.tex") == "product/model/esf/esf028/002/rto/wrnk0001/_/000/00/esf028_000_00_facialblend.tex",
              "game outfit: the glasses (with the series' main files) and the wrinkles");
        choose(q, t, c, 0, "");
        d = desired(q, t, c);
        check(is_orig(d, "product/model/esf/esf028/002/00/esf028_002_00.mesh"), "game outfit: None gives back the game's head");
        auto r = rows(q, t, c);
        check(r.size() == 2 && r[0].kind == Row::Pick && r[0].none && r[0].labels.size() == 3 && r[1].kind == Row::Toggle,
              "game outfit: a choice with None (main files / glasses) and an On / Off row");
    }

    {   // the menu title: a long outfit name is cut, UTF-8 aware
        check(clip_name("Ingrid - Fly In A Bucket - Drive Tech", kTitleNameChars) == "Ingrid - Fly In A Bucket - Drive Tech",
              "title: a name of 40 characters or less is kept");
        const std::string longer = "Ingrid - Fly In A Bucket - Drive Tech + Ingrid - True... meme book";
        const std::string cut = clip_name(longer, kTitleNameChars);
        check(cut == "Ingrid - Fly In A Bucket - Drive Tech..." && cut.size() == 40, "title: a longer name ends with ...");
        const std::string u8 = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";   // 6 characters, 12 bytes
        check(clip_name(u8, 6) == u8 && clip_name(u8, 5) == "\xC3\xA9\xC3\xA9...", "title: characters, not bytes (UTF-8)");
    }

    {   // names shown in the menu: no sort-order prefixes
        check(display_name("3) addon - remove mais facial wrinkles") == "Remove mais facial wrinkles", "name: number and addon prefix");
        check(display_name("!C2 Mai face accessories") == "C2 Mai face accessories", "name: leading !");
        check(display_name("2) glasses only") == "Glasses only", "name: number, first letter upper case");
        check(display_name("Ken SFV Hair V2") == "Ken SFV Hair V2", "name: a plain name is kept");
        check(display_name("2B hair") == "2B hair" && display_name("1)") == "1)", "name: a number that is the name stays");
    }

    int rc = 0;
    if (argc > 1) rc = real_manifest(argv[1]);
    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("all ok\n");
    return rc;
}
