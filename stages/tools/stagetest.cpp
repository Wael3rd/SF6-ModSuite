// stagetest: runs the stage loader pass on a folder laid out like the game's
// (reframework\stage_mods, reframework\data\SF6_StageSlots_Data\loader\stage_paths.txt),
// without the game. Usage: stagetest <fake game dir>
//        stagetest --selftest   unit tests of the tile / variation guess, stage_slots.ini and the menu picture conversion
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include "../loader/preview_tex.hpp"
#include "../loader/stage_classify.hpp"
#include "../loader/stage_loader.hpp"
#include "../loader/stage_log.hpp"

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

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--selftest") == 0) {
        test_classify();
        test_overrides();
        test_pictures();
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
