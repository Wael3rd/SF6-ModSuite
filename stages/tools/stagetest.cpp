// stagetest: runs the stage loader pass on a folder laid out like the game's
// (reframework\stage_mods, reframework\data\SF6_StageSlots_Data\loader\stage_paths.txt),
// without the game. Usage: stagetest <fake game dir>
//        stagetest --selftest   unit tests of the tile / variation guess, stage_slots.ini, the menu picture conversion
//                               and the title packs' choice (title_slots.hpp)
#include <windows.h>
#include <stdio.h>
#include <algorithm>
#include <string>
#include <vector>
#include "../loader/preview_tex.hpp"
#include "../loader/stage_classify.hpp"
#include "../loader/stage_loader.hpp"
#include "../loader/stage_log.hpp"
#include "../loader/title_slots.hpp"

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    printf("  %-84s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fails;
}

std::string P(const char* rel) { return std::string("natives/stm/product/") + rel; }

// The file lists of real mods (PC3, 2026-11-05), shortened to what decides.
void test_classify() {
    printf("tile / variation guess\n");
    // NULSPACE (pak): scenes only, its root prop scene among them
    StageKind k = classify_stage_variant("ess0000_00", {
        P("camera/scene/ess/ess0000_00_camera.scn.20"), P("environment/stage/scene/ess/ess0000_00/ess0000_00_env.scn.20"),
        P("level/scene/ess/ess0000_00/ess0000_00_level.scn.20"), P("light/scene/ess/ess0000_00/ess0000_00_light.scn.20") }, 0);
    check(k.tile && k.reason.find("root prop scene") != std::string::npos, "NULSPACE: tile (root prop scene)");
    // Suzaku Castle over the Training Room: arena mesh and 188 new textures
    k = classify_stage_variant("ess0000_00", { P("environment/stage/resource/ess/ess0000_00/ess0000_00.mdf2.31"),
        P("environment/stage/resource/ess/ess0000_00/ess0000_00.mesh.230110883"), P("light/scene/ess/ess0000_00/ess0000_00_light_00.scn.20") }, 188);
    check(k.tile && k.reason.find("arena mesh") != std::string::npos && k.reason.find("188 files") != std::string::npos,
          "Suzaku Castle: tile (arena mesh, 188 new files)");
    // Training Void: the _00 mesh and collision
    k = classify_stage_variant("ess0000_00", { P("environment/stage/resource/ess/ess0000_00/ess0000_00_00.mesh.230110883"),
        P("environment/stage/resource/ess/ess0000_00/ess0000_00e.mcol.20021") }, 2);
    check(k.tile && k.reason.find("collision") != std::string::npos, "Training Void: tile (arena mesh, collision)");
    // Stage Lighting Overhaul, Training Room Red: camera, light, material, albedo
    k = classify_stage_variant("ess0000_00", { P("camera/scene/ess/ess0000_00_camera.scn.20"),
        P("environment/stage/resource/ess/ess0000_00/ess0000_00.mdf2.31"), P("environment/stage/resource/ess/ess0000_00/ess0000_00_albd.tex.241101895"),
        P("light/scene/ess/ess0000_00/ess0000_00_light_00.scn.20") }, 0);
    check(!k.tile, "Training Room Red (lighting): variation");
    // Stage Lighting Overhaul, Suval'hal Arena: a level scene and env_props_dynamic are not a new stage
    k = classify_stage_variant("ess0101_00", { P("environment/stage/scene/ess/ess0101_00/ess0101_00_env_props_dynamic.scn.20"),
        P("level/scene/ess/ess0101_00/ess0101_00_level.scn.20"), P("light/scene/ess/ess0101_00/ess0101_00_light_00.scn.20") }, 0);
    check(!k.tile, "Suval'hal Arena (lighting, level scene): variation");
    // Dhalsimer Temple With Banner: a sub-mesh and 12 new textures
    k = classify_stage_variant("ess0700_00", { P("environment/stage/resource/ess/ess0700_00/ess0700_00_03.mdf2.31"),
        P("environment/stage/resource/ess/ess0700_00/ess0700_00_03.mesh.230110883") }, 12);
    check(!k.tile, "Dhalsimer Temple With Banner (sub-mesh, 12 new files): variation");
    // Genbu Temple Night Time: a light scene and a mesh at a path of its own
    k = classify_stage_variant("ess0200_00", { P("environment/stage/resource/ess/ess0200_00/ess0000_00.mesh.230110883"),
        P("light/scene/ess/ess0200_00/ess0200_00_light_00.scn.20") }, 1);
    check(!k.tile, "Genbu Temple Night Time (light, mesh of another name): variation");
    // case and natives prefix do not matter
    k = classify_stage_variant("ESS0000_00", { "natives/STM/Product/Environment/Stage/Resource/ESS/ess0000_00/ESS0000_00.MESH.230110883" }, 0);
    check(k.tile, "upper-case path: still the arena mesh");
    k = classify_stage_variant("ess0000_00", {}, 25);
    check(k.tile, "a pak with 25 unknown files: tile");
}

void test_overrides() {
    printf("stage_slots.ini\n");
    std::vector<std::string> bad;
    auto list = parse_stage_overrides("\xEF\xBB\xBF; comment\n[SIRN Training Room]\nkind = Variation\n\n[Stage Lighting Overhaul-3258-1-0-1774283942.rar]\n"
                                      "kind=tile\n[Training Room Red]\nkind=variation\nname = Red Room\nfoo=bar\nkind=maybe\n# x\n[]\nloose\n", &bad);
    check(list.size() == 3, "three sections (an empty section name is refused)");
    check(list[0].kind == "variation" && list[0].name.empty(), "kind is case-insensitive, spaces trimmed");
    check(list[2].name == "Red Room" && list[2].kind == "variation", "name= kept as written");
    check(bad.size() == 4, "unknown key, bad kind value, empty section, a line outside a section: reported");
    const StageOverride* o = find_stage_override(list, "scbacd1e2e2cf", "SIRN Training Room", "SIRN Training Room.rar/SIRN Training Room");
    check(o == &list[0], "found by the mod's name");
    o = find_stage_override(list, "sd6f5bdab19f0", "Training Room Red", "Stage Lighting Overhaul-3258-1-0-1774283942.rar/Training Room Red");
    check(o == &list[2], "the last matching section wins (a pack, then one of its options)");
    o = find_stage_override(list, "s1ec9cc1c9515", "Training Room Green", "Stage Lighting Overhaul-3258-1-0-1774283942.rar/Training Room Green");
    check(o == &list[1], "found by the archive's name");
    auto l2 = parse_stage_overrides("[Stage Lighting Overhaul-3258-1-0-1774283942]\nkind=tile\n[s800c479d6d25]\nname=Null\n");
    check(find_stage_override(l2, "x", "y", "Stage Lighting Overhaul-3258-1-0-1774283942.rar/Fete Foraine") == &l2[0],
          "found by the archive's name without its extension");
    check(find_stage_override(l2, "s800c479d6d25", "NULSPACE", "NULSPACE-3564-1-0-1781025053.zip/NULSPACE") == &l2[1], "found by the key");
    check(find_stage_override(l2, "s1", "Other", "Other.zip/Other") == nullptr, "no section: the guess stays");
    check(parse_stage_overrides(kStageOverridesTemplate).empty(), "the template is all comments");
}

// A game texture of a solid colour (RGBA8, or BC1 built by our own encoder) converted to our preview and thumbnail.
void test_pictures() {
    printf("menu pictures\n");
    auto rgba_tex = [](int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        std::vector<uint8_t> t(56 + (size_t)w * h * 4, 0);
        memcpy(t.data(), "TEX\0", 4);
        int32_t ver = 241101895, fmt = 28, pitch = w * 4, size = w * h * 4;
        int64_t off = 56;
        memcpy(t.data() + 4, &ver, 4);
        uint16_t W = (uint16_t)w, H = (uint16_t)h;
        memcpy(t.data() + 8, &W, 2);
        memcpy(t.data() + 10, &H, 2);
        t[14] = 1; t[15] = 16;
        memcpy(t.data() + 16, &fmt, 4);
        memcpy(t.data() + 40, &off, 8);
        memcpy(t.data() + 48, &pitch, 4);
        memcpy(t.data() + 52, &size, 4);
        for (size_t i = 0; i < (size_t)w * h; ++i) { t[56 + i * 4] = r; t[57 + i * 4] = g; t[58 + i * 4] = b; t[59 + i * 4] = 255; }
        return t;
    };
    auto dims = [](const std::vector<uint8_t>& t, int& w, int& h, int& fmt) {
        w = h = fmt = 0;
        if (t.size() < 56) return;
        uint16_t W, H;
        memcpy(&W, t.data() + 8, 2); memcpy(&H, t.data() + 10, 2); memcpy(&fmt, t.data() + 16, 4);
        w = W; h = H;
    };
    std::vector<uint8_t> out, rgba;
    std::string err;
    int w, h, fmt;
    check(make_stage_tex_from_game_tex(rgba_tex(1024, 256, 200, 40, 40), false, out, err), "RGBA8 4:1 picture -> preview");
    dims(out, w, h, fmt);
    check(w == 2048 && h == 512 && fmt == 72 && out.size() == 56 + 524288, "preview: 2048x512 BC1 sRGB, one level (the vanilla layout)");
    check(decode_game_tex_rgba(out, rgba, w, h, err) && w == 2048 && h == 512 && rgba[0] > 150 && rgba[1] < 90,
          "our BC1 decodes back to the colour");
    std::vector<uint8_t> bc1;
    check(make_stage_tex_from_game_tex(rgba_tex(128, 256, 10, 200, 10), true, bc1, err), "RGBA8 1:2 picture -> thumbnail");
    dims(bc1, w, h, fmt);
    check(w == 128 && h == 256 && fmt == 72, "thumbnail: 128x256 BC1");
    check(make_stage_tex_from_game_tex(rgba_tex(1920, 1080, 1, 2, 3), true, bc1, err), "16:9 picture -> thumbnail (cut like a screenshot)");
    std::vector<uint8_t> broken = rgba_tex(64, 64, 1, 1, 1);
    int64_t beyond = 1 << 20;
    memcpy(broken.data() + 40, &beyond, 8);
    check(!make_stage_tex_from_game_tex(broken, false, out, err) && err.find("not in the file") != std::string::npos,
          "a streaming base file (mip 0 outside the file) is refused");
    std::vector<uint8_t> odd = rgba_tex(64, 64, 1, 1, 1);
    int32_t bc6 = 95;
    memcpy(odd.data() + 16, &bc6, 4);
    check(!make_stage_tex_from_game_tex(odd, false, out, err) && err.find("unsupported") != std::string::npos, "BC6H is refused");
    check(!make_stage_tex_from_game_tex(std::vector<uint8_t>(10, 0), false, out, err), "garbage is refused");
}

// Title packs: the split of a pack's files, settings.json, the keys, the order and the choice.
void test_titles() {
    printf("title packs\n");
    using namespace title_slots;
    const char* part = "natives/stm/streaming/product/gui/gm/ui10000/part/";
    check(is_title_file(std::string(part) + "title_start.mov.1") && is_title_file(std::string(part) + "title_loop.mov.1"),
          "title_start / title_loop movies: title files");
    check(is_title_file("NATIVES/STM/Streaming/Product/GUI/GM/UI10000/Part/Title_End.mov.1"), "any case: still a title file");
    check(!is_title_file("natives/stm/product/gui/gm/ui10000/ui10001.gui.530035") &&
          !is_title_file("natives/stm/product/gui/gm/ui10000/part/re logo animation.mov.1.x64") &&
          !is_title_file("natives/stm/product/gui/gm/ui10000/part/tex_title_logo.tex.241101895") &&
          !is_title_file("natives/stm/product/sound/wwise/re_logo.sbnk.1.x64"),
          "boot screens, logo textures (even tex_title_*), re_logo.sbnk: startup files");

    Settings st = parse_settings("\xEF\xBB\xBF{\n    \"display_nsfw\": false,\n    \"online_outfit\": \"Outfit 1\",\n"
                                 "    \"nested\": { \"title_pack\": \"no\", \"a\": [1, {\"b\": \"}\"}] },\n"
                                 "    \"esc\": \"a \\\"quoted\\\" }, value\",\n    \"title_pack\": \"t0123456789ab\",\n"
                                 "    \"skip_intro\": true,\n    \"n\": -1.5e3\n}\n");
    check(st.pack == "t0123456789ab" && st.skip_intro, "settings.json: both keys read past other keys, nested values, escapes");
    st = parse_settings("{\"select_options_key\": \"C\"}");
    check(st.pack.empty() && !st.skip_intro, "keys missing: the game's own title, intro kept");
    st = parse_settings("");
    check(st.pack.empty() && !st.skip_intro, "empty file: the defaults");
    st = parse_settings("not json {\"title_pack\": \"random\"}");
    check(st.pack.empty(), "not an object: the defaults");
    st = parse_settings("{\"title_pack\": 3, \"skip_intro\": \"yes\"}");
    check(st.pack.empty() && !st.skip_intro, "keys of another type: their defaults");
    st = parse_settings("{\"title_pack\": \"random\", \"skip_intro\": tr");
    check(st.pack == "random" && !st.skip_intro, "a text cut short keeps what came before");
    st = parse_settings("{\"skip_intro\":true,\"title_pack\":\"\\u0074abc\"}");
    check(st.pack == "tabc" && st.skip_intro, "no spaces, \\u escape");

    check(pack_key("a") == "taf63dc4c8601", "key: \"t\" + 12 hex digits of FNV-1a 64 (test vector of \"a\")");
    check(pack_key("Ryu Title.zip") == pack_key("RYU TITLE.ZIP") && pack_key("Ryu Title.zip") != pack_key("Ken Title.zip"),
          "key: case-insensitive, one per source");
    check(pack_key("Pack.zip/Option A") != pack_key("Pack.zip/Option B"), "key: one per option of a bundle");

    auto mk = [](const char* name, size_t title, size_t startup) {
        Pack p;
        p.name = name;
        p.source = std::string(name) + ".zip";
        p.key = pack_key(p.source);
        p.title_files = title;
        p.startup_files = startup;
        return p;
    };
    std::vector<Pack> packs = { mk("beta", 3, 0), mk("Alpha", 3, 6), mk("alpha 2", 2, 0), mk("Logos only", 0, 6) };
    std::stable_sort(packs.begin(), packs.end(), pack_before);
    check(packs[0].name == "Alpha" && packs[1].name == "alpha 2" && packs[2].name == "beta" && packs[3].name == "Logos only",
          "registry order: by name, case-insensitive");
    Settings s;
    Choice c = resolve_choice(packs, s, 0);
    check(c.title < 0 && c.startup < 0 && !c.random && !c.unknown, "nothing set: the game's own title, nothing written");
    s.skip_intro = true;
    c = resolve_choice(packs, s, 0);
    check(c.title < 0 && c.startup == 0, "own title + skip_intro: the first pack's startup files");
    s.pack = packs[2].key;
    c = resolve_choice(packs, s, 0);
    check(c.title == 2 && c.startup == 0, "a pack without startup files + skip_intro: the first pack's that has some");
    s.pack = packs[1].key;
    s.skip_intro = false;
    c = resolve_choice(packs, s, 0);
    check(c.title == 1 && c.startup < 0, "a pack's key: its title files only");
    s.pack = " " + std::string("T") + packs[0].key.substr(1) + " ";
    s.skip_intro = true;
    c = resolve_choice(packs, s, 0);
    check(c.title == 0 && c.startup == 0, "a key in upper case, with spaces: found; skip_intro: its own startup files");
    s.pack = "t000000000000";
    c = resolve_choice(packs, s, 0);
    check(c.title < 0 && c.unknown && c.startup == 0, "an unknown key: the game's own title (reported)");
    s.pack = "Random";
    s.skip_intro = false;
    bool drawn[4] = {};
    for (uint64_t r = 0; r < 12; ++r) {
        c = resolve_choice(packs, s, r);
        if (c.title >= 0) drawn[c.title] = true;
        if (!c.random || c.title < 0) drawn[3] = true;
    }
    check(drawn[0] && drawn[1] && drawn[2] && !drawn[3], "random: every pack with title files can be drawn, never one without");
    c = resolve_choice({}, s, 7);
    check(c.random && c.title < 0 && c.startup < 0, "random without packs: the game's own title");

    // Random's mosaic: the layout with the largest 16:9 places, then the fewest empty ones, then the most columns
    auto lay_is = [](int n, int cols, int rows) { const MosaicLayout l = title_mosaic_layout(n); return l.cols == cols && l.rows == rows; };
    check(lay_is(1, 1, 1) && title_mosaic_layout(1).place_w == 1920, "mosaic: 1 picture, the whole screen");
    check(lay_is(2, 2, 1) && lay_is(3, 2, 2) && lay_is(4, 2, 2), "mosaic: 2 side by side, 3 and 4 on 2 x 2");
    check(lay_is(5, 3, 2) && lay_is(6, 3, 2), "mosaic: 5 and 6 on 3 columns x 2 rows");
    check(lay_is(7, 3, 3) && lay_is(8, 3, 3) && lay_is(9, 3, 3) && lay_is(10, 4, 3), "mosaic: 7 to 9 on 3 x 3, 10 on 4 x 3");
    bool fits = true;
    for (int n = 1; n <= 40; ++n) {
        const MosaicLayout l = title_mosaic_layout(n);
        fits = fits && l.cols * l.rows >= n && l.cols * l.place_w <= 1920 && l.rows * l.place_h <= 1080 && l.place_h * 16 == l.place_w * 9 &&
               l.cell_w == l.place_w - 2 * TITLE_MOSAIC_GAP;
    }
    check(fits, "mosaic: 1 to 40 pictures, every place 16:9, all of them on the screen");

    // Random's bag: every key once per round, never twice in a row across rounds, dropped keys gone, kept through its text
    const std::vector<std::string> keys = { "ta", "tb", "tc", "td", "te" };
    Bag bag;
    uint32_t seed = 12345;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    bool rounds_ok = true, no_repeat = true;
    std::string prev;
    for (int round = 0; round < 6; ++round) {
        std::vector<std::string> seen;
        for (size_t i = 0; i < keys.size(); ++i) {
            const std::string k = bag_next(bag, keys, rnd);
            no_repeat = no_repeat && k != prev;
            prev = k;
            seen.push_back(k);
            if (round == 3 && i == 1) bag = parse_bag(bag_text(bag));   // a new launch in the middle of a round
        }
        std::sort(seen.begin(), seen.end());
        rounds_ok = rounds_ok && seen == keys;
    }
    check(rounds_ok && no_repeat, "random bag: each round shows every pack once, never the same twice in a row, through a relaunch");
    Bag b2;
    b2.left = { "ta", "tz", "tb" };
    const std::vector<std::string> pool2 = { "ta", "tb" };
    const std::string k1 = bag_next(b2, pool2, rnd), k2 = bag_next(b2, pool2, rnd);
    check(k1 == "ta" && k2 == "tb" && b2.left.empty(), "random bag: a pack removed from title_mods leaves the bag");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--selftest") == 0) {
        test_classify();
        test_overrides();
        test_pictures();
        test_titles();
        printf(g_fails ? "%d FAILED\n" : "all ok\n", g_fails);
        return g_fails ? 1 : 0;
    }
    if (argc < 2) { fprintf(stderr, "usage: stagetest <fake game dir> | stagetest --selftest\n"); return 2; }
    slog_open(argv[1]);
    std::vector<StageVariant> vars;
    stage_loader_run(argv[1], vars);
    for (auto& v : vars)
        printf("%s  stage %u (%s)  \"%s\"  %zu files%s%s  %s: %s\n", v.key.c_str(), v.stage_id, v.ess.c_str(), v.name.c_str(),
               v.redirects.size(), v.has_preview ? "  preview" : "", v.banner.empty() ? "" : (" (" + v.banner + ")").c_str(),
               v.tile ? "TILE" : "variation", v.kind_reason.c_str());
    return 0;
}
