// Unit test of plugin/src/replay_logic.hpp: the Replay Outfits tab's row cycling, colour cycling, file round trip.
// Build and run: build_replay_logic_test.bat (prints "all ok" and exits 0 when every check passes).
#define _CRT_SECURE_NO_WARNINGS
#include "../src/replay_logic.hpp"

#include <cmath>
#include <cstdio>

using namespace slots::replay;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-72s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

static Fighter ryu() {
    Fighter f;
    f.fid = 1;
    f.name = "Ryu";
    f.outfits = {
        { 9, "abc123", "zeta Ryu", true, { 0, 1, 2 } },
        { 1, "", "Outfit 2", false, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 } },
        { 0, "", "Outfit 1", false, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 200, 201 } },
        { 5, "def456", "Aloha Ryu", true, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 } },
        { 7, "nocol", "No colours", true, {} },
        { 6, "", "Beta (no mod id)", true, { 0, 1 } },
    };
    sort_outfits(f.outfits);
    return f;
}

int main() {
    const Fighter f = ryu();
    check(f.outfits.size() == 5, "an outfit without colours is dropped");
    check(f.outfits[0].costume == 0 && f.outfits[1].costume == 1, "game outfits first, by costume number");
    check(f.outfits[2].name == "Aloha Ryu" && f.outfits[3].name == "Beta (no mod id)" && f.outfits[4].name == "zeta Ryu",
          "then extra outfits by name, case-insensitive");

    // outfit cycling
    Choice c;
    check(row_text(f, c) == "Default", "no entry: Default");
    c = cycle_outfit(f, c, +1);
    check(c.forced && c.costume == 0 && c.mod_id.empty() && c.color == 0, "Right from Default: Outfit 1, colour 0");
    check(row_text(f, c) == "Outfit 1 - Color 01", "row text of Outfit 1 colour 0");
    c.color = 200;
    check(row_text(f, c) == "Outfit 1 - Color DX 1", "colour 200 shows as the game's DX 1");
    check(color_label(9) == "10" && color_label(100) == "EX 1" && color_label(101) == "EX 2" && color_label(105) == "EX 6" &&
          color_label(207) == "DX 8", "colour names as the game's grid: 10, EX 1 (100), EX 2, EX 6, DX 8");
    c = cycle_outfit(f, c, +1);
    check(c.costume == 1 && c.color == 0, "Outfit 2 has no colour 200: its first colour");
    c.color = 5;
    c = cycle_outfit(f, c, +1);
    check(c.costume == 5 && c.mod_id == "def456" && c.color == 5, "Aloha Ryu keeps colour 5");
    c = cycle_outfit(f, c, +1);
    check(c.costume == 6 && c.mod_id.empty() && c.color == 0, "an extra outfit without a mod id: by its number, no colour 5");
    c = cycle_outfit(f, c, +1);
    check(c.costume == 9 && c.mod_id == "abc123" && c.color == 0, "zeta Ryu has no colour 5: colour 0");
    c = cycle_outfit(f, c, +1);
    check(!c.forced && row_text(f, c) == "Default", "Right from the last outfit: back to Default");
    c = cycle_outfit(f, c, -1);
    check(c.forced && c.costume == 9 && c.mod_id == "abc123", "Left from Default: the last outfit");
    c = cycle_outfit(f, c, -1);
    check(c.costume == 6, "Left again: the previous outfit");
    Fighter empty;
    empty.fid = 99;
    check(!cycle_outfit(empty, Choice{}, +1).forced, "a fighter without outfits stays Default");
    Choice k = cycle_outfit(f, Choice{}, +1);              // Outfit 1
    k.color = 5;
    Choice k2 = cycle_outfit(f, k, +1, false);
    check(k2.costume == 1 && k2.color == 0, "a costume change on the page: the colour goes back to 01");
    k2 = cycle_outfit(f, cycle_outfit(f, k2, +1, false), +1, false);   // Aloha Ryu (colours from 0)
    check(k2.color == 0, "the colour reset again on the next costume");

    // colour cycling
    Choice d;
    check(cycle_color(f, d, +1) == d, "colour key on Default: nothing");
    Choice z = cycle_outfit(f, Choice{}, -1);          // zeta Ryu, colours 0 1 2
    z = cycle_color(f, z, +1);
    check(z.color == 1, "colour key: next colour");
    z = cycle_color(f, z, +1);
    z = cycle_color(f, z, +1);
    check(z.color == 0, "colour key wraps to the first colour");
    z = cycle_color(f, z, -1);
    check(z.color == 2, "colour key backwards wraps to the last colour");
    Choice o1 = cycle_outfit(f, Choice{}, +1);
    o1.color = 9;
    o1 = cycle_color(f, o1, +1);
    check(o1.color == 200, "Outfit 1: after colour 9 comes the special colour 200");
    Choice gone;
    gone.forced = true; gone.costume = 42; gone.mod_id = "removed";
    check(row_text(f, gone) == "Default" && outfit_index(f, gone) == -1, "an outfit no longer listed shows Default");
    check(cycle_outfit(f, gone, +1).costume == 0, "Right from a vanished outfit: the first outfit");

    // the Color Mod's entries (RW / MC): after the game colours, each one on colour 1, with its own label
    Fighter g;
    g.fid = 1;
    Outfit vag;
    vag.costume = 8; vag.mod_id = "8cd9"; vag.extra = true; vag.name = "Vagrant Ryu"; vag.colors = { 0, 1, 2 };
    vag.cm = { { "..\\color_mods\\Ryu\\Pack\\esf001_009_cmd_003.user.2", "RW 1" },
               { "..\\color_mods\\Ryu\\MANUALCOLORS_01\\esf001_009_cmd_001.user.2", "MC 1" } };
    g.outfits = { vag };
    Choice v = cycle_outfit(g, Choice{}, +1);
    v.color = 2;
    v = cycle_color(g, v, +1);
    check(v.color == 1 && v.colormod == vag.cm[0].id && row_text(g, v) == "Vagrant Ryu - Color RW 1",
          "after the last game colour: the first Color Mod entry, on colour 1, with its label");
    v = cycle_color(g, v, +1);
    check(v.colormod == vag.cm[1].id && choice_color_label(g.outfits[0], v) == "MC 1", "then the next entry (MC 1)");
    v = cycle_color(g, v, +1);
    check(v.color == 0 && v.colormod.empty(), "then back to the first game colour");
    v = cycle_color(g, v, -1);
    check(v.colormod == vag.cm[1].id, "backwards from the first game colour: the last entry");
    Choice v1 = v; v1.colormod.clear();
    check(!(v1 == v), "an entry is another choice than the game colour 02");
    Choice gone_cm = v; gone_cm.colormod = "removed";
    check(choice_color_label(g.outfits[0], gone_cm) == "02", "an entry the Color Mod no longer lists: shown as worn, 02");
    check(cycle_outfit(g, v, +1, false).colormod.empty(), "a costume change: back to 01, no entry");
    Fighter h = g;
    h.outfits[0].colors = { 0, 2 };
    Choice w = cycle_outfit(h, Choice{}, +1);
    w = cycle_color(h, cycle_color(h, w, +1), +1);
    check(w.color == 0 && w.colormod.empty(), "an outfit without colour 1 offers no entry (it is their carrier)");
    Choices cmf;
    cmf[1] = v;
    Choices cmb;
    const std::string cmt = to_json(cmf);
    check(from_json(cmt, cmb) && cmb[1] == v && cmb[1].color == 1, "a Color Mod entry survives the file (its id, colour 1)");
    check(cmt.find("\"colormod\": \"..\\\\color_mods\\\\Ryu\\\\MANUALCOLORS_01\\\\esf001_009_cmd_001.user.2\"") != std::string::npos,
          "the entry is written by its file path (escaped)");

    // file round trip
    Choices cs;
    cs[1] = cycle_outfit(f, Choice{}, +1);
    Choice m = cycle_outfit(f, cycle_outfit(f, cycle_outfit(f, Choice{}, +1), +1), +1);   // Aloha Ryu
    m.color = 7;
    cs[30] = m;
    cs[2] = Choice{};                                   // Default: never written
    const std::string text = to_json(cs);
    Choices back;
    check(from_json(text, back), "written file parses");
    check(back.size() == 2 && back.count(1) && back.count(30) && !back.count(2), "two forced fighters, Default left out");
    check(back[1] == cs[1] && back[30] == cs[30], "values survive the round trip");
    check(back[1].mod_id.empty() && back[30].mod_id == "def456" && back[30].color == 7, "mod_id only for the extra outfit");
    check(text.find("\"mod_id\"") == text.rfind("\"mod_id\""), "one mod_id key in the file");
    Choices none;
    Choices back2;
    check(from_json(to_json(none), back2) && back2.empty(), "no forced fighter: an empty fighters object");
    check(!from_json("{\"version\":2,\"fighters\":{}}", back2), "another version is refused");
    check(!from_json("not json", back2), "garbage is refused");
    check(from_json("{\"version\":1,\"fighters\":{\"7\":{\"mode\":\"auto\",\"costume\":1},\"x\":{\"mode\":\"forced\",\"costume\":1},"
                    "\"8\":{\"mode\":\"forced\",\"costume\":2,\"color\":3}}}", back2) &&
          back2.size() == 1 && back2[8].costume == 2 && back2[8].color == 3,
          "entries not forced, or with a bad id, are skipped");
    // the stage
    StageChoice st;
    st.forced = true; st.id = 40000;
    const std::string with_stage = to_json(cs, st);
    StageChoice st_back;
    Choices back3;
    check(from_json(with_stage, back3, &st_back) && st_back == st && back3.size() == 2, "a forced stage survives the round trip");
    check(with_stage.find("\"stage\": {\"mode\": \"forced\", \"id\": 40000}") != std::string::npos, "the stage entry as written");
    check(to_json(cs) == to_json(cs, StageChoice{}) && to_json(cs).find("stage") == std::string::npos, "Default stage: no entry");
    check(from_json(to_json(none, st), back3, &st_back) && back3.empty() && st_back.forced && st_back.id == 40000,
          "a forced stage with no forced fighter");
    check(from_json("{\"version\":1,\"fighters\":{},\"stage\":{\"mode\":\"auto\",\"id\":3}}", back3, &st_back) && !st_back.forced,
          "a stage entry that is not forced: Default");
    check(st_back.entry.empty() && from_json(with_stage, back3, &st_back) && st_back.entry.empty(),
          "an older file (no \"entry\"): the stage's own look");
    StageChoice tile;
    tile.forced = true; tile.id = 0; tile.entry = "suzaku_castle";
    const std::string with_tile = to_json(cs, tile);
    check(from_json(with_tile, back3, &st_back) && st_back == tile && st_back.entry == "suzaku_castle",
          "a stage slot (tile / variation) survives the round trip, by its key");
    check(with_tile.find("\"stage\": {\"mode\": \"forced\", \"id\": 0, \"entry\": \"suzaku_castle\"}") != std::string::npos,
          "the stage slot as written");
    StageChoice tile2 = tile; tile2.entry.clear();
    check(!(tile2 == tile), "a stage slot is another choice than the vanilla look of its stage");
    check(stage_image_path(40000) == "Product/GUI/data/Area_Image/ess/ess0400_00/tex_StageImage_ess0400_00_IM.tex" &&
          stage_image_path(400000) == "Product/GUI/data/Area_Image/ess/ess4000_00/tex_StageImage_ess4000_00_IM.tex" &&
          stage_image_path(0) == "Product/GUI/data/Area_Image/ess/ess0000_00/tex_StageImage_ess0000_00_IM.tex",
          "stage picture paths (the game's Stage Settings')");

    // cursor and window
    check(move_cursor(0, -1, 32) == 31 && move_cursor(31, 1, 32) == 0 && move_cursor(4, 1, 32) == 5, "Up / Down wrap");
    check(window_top(0, 0, 32, 14) == 0 && window_top(0, 14, 32, 14) == 1 && window_top(10, 3, 32, 14) == 3, "window follows the cursor");
    check(window_top(0, 31, 32, 14) == 18 && window_top(5, 2, 10, 14) == 0, "window clamped at the end, none when all fit");

    // fighter order and names
    std::vector<Fighter> fs(3);
    fs[0].fid = 99; fs[1].fid = 21; fs[2].fid = 1;
    order_fighters(fs);
    check(fs[0].fid == 21 && fs[1].fid == 1 && fs[2].fid == 99, "grid order (Jamie before Ryu), unknown ids last");
    check(face_path(1) == "Product/GUI/data/Character_Image/001/caracterselect/tex_Esf001_face_L_s_IAM.tex" &&
          face_path(30) == "Product/GUI/data/Character_Image/030/characterselect/tex_Esf030_face_L_s_IAM.tex",
          "portrait paths, both folder spellings");
    check(grid_caption("C. Viper") == "C. VIPER", "grid caption in capitals");
    // 32 cells, 4 columns
    check(grid_move(0, -1, 0, 32, 4) == 3 && grid_move(3, 1, 0, 32, 4) == 0, "Left / Right wrap inside the row");
    check(grid_move(0, 0, -1, 32, 4) == 28 && grid_move(29, 0, 1, 32, 4) == 1, "Up / Down wrap inside the column");
    check(grid_move(1, 0, -1, 30, 4) == 29 && grid_move(29, 0, 1, 30, 4) == 1 && grid_move(3, 0, -1, 30, 4) == 27 &&
          grid_move(27, 0, 1, 30, 4) == 3 && grid_move(28, 1, 0, 30, 4) == 29 &&
          grid_move(29, 1, 0, 30, 4) == 28, "a short last row: wraps to the cells that exist");
    check(std::string(fighter_name(30)) == "C. Viper" && fighter_name(23) == nullptr, "fighter names");
    check(game_outfit_name(0) == "Outfit 1" && game_outfit_name(3) == "Outfit 4" && game_outfit_name(4) == "DriveTech",
          "game outfit names");

    // the summary on the right: one line per character set to On, the panel's layout
    {
        Fighter luke;
        luke.fid = 2; luke.name = "Luke";
        luke.outfits = { { 3, "", "Outfit 4", false, { 0, 1, 2 } } };
        Fighter vr = g;                                   // Ryu with Vagrant Ryu and its RW / MC entries
        vr.name = "Ryu";
        std::vector<Fighter> cat = { luke, vr };
        Choices sc;
        check(summary_lines(cat, sc, 20) == std::vector<std::string>{ "Default" }, "summary: nobody On, Default");
        Choice lc; lc.forced = true; lc.costume = 3; lc.color = 0;
        Choice rc = cycle_outfit(vr, Choice{}, +1);
        rc.color = 1; rc.colormod = vag.cm[1].id;
        sc[1] = rc; sc[2] = lc;
        const std::vector<std::string> two = summary_lines(cat, sc, 20);
        check(two.size() == 2 && two[0] == "Luke: Outfit 4, 01" && two[1] == "Ryu: Vagrant Ryu, MC 1",
              "summary: the catalog's order, outfit and colour as the page names them");
        Choice lost; lost.forced = true; lost.costume = 99;
        check(summary_line(luke, lost) == "Luke: Default", "summary: an outfit no longer listed shows Default");
        std::vector<Fighter> many;
        Choices all;
        for (int i = 0; i < 25; ++i) {
            Fighter one = luke; one.fid = 100 + i; one.name = "F" + std::to_string(i);
            many.push_back(one);
            all[one.fid] = lc;
        }
        const std::vector<std::string> capped = summary_lines(many, all, 20);
        check(capped.size() == 20 && capped[18] == "F18: Outfit 4, 01" && capped[19] == "+ 6 more",
              "summary: 25 On, 19 lines and \"+ 6 more\"");
        check(cut_utf8("Short", 32) == "Short" && cut_utf8("abcdefghij", 8) == "abcde...", "summary: long lines cut with ...");
        check(cut_utf8("ab\xC3\xA9" "cdefgh", 6) == "ab...", "summary: never cut inside a UTF-8 sequence");
        // Battle Settings > Other's "Game Settings", measured on screen (y = 189 + 0.81 x layout): Stage Settings (its picture),
        // Battle BGM, Commentary Settings (6 lines), Side Settings, Battle HUD Design
        using K = SumKind;
        const std::vector<float> ys = summary_layout({ K::Name, K::Picture, K::Name, K::Line, K::Name, K::Line, K::Line, K::Line,
                                                       K::Line, K::Line, K::Line, K::Name, K::Line, K::Name, K::Line });
        const float screen[] = { 240, 256, 338, 362, 401, 424, 449, 475, 499, 525, 550, 589, 613, 652, 676 };
        bool near = ys.size() == 15;
        for (size_t i = 0; near && i < ys.size(); ++i) near = std::fabs(189.0f + 0.81f * ys[i] - screen[i]) <= 2.0f;
        check(near, "summary layout: Battle Settings > Other's panel within 2 px");
        const std::vector<float> page = summary_layout({ K::Name, K::Line, K::Name, K::Line, K::Name, K::Line });
        check(page == std::vector<float>{ 63, 93, 141, 171, 219, 249 }, "summary layout: the character page's three settings");
        const std::vector<float> sl = summary_layout({ K::Name, K::Line, K::Name, K::Picture, K::Line });
        check(sl[3] == sl[2] + kSumPicGap && sl[4] == sl[3] + kSumPicH + kSumLineAfterPic, "summary layout: the stage's name under its picture");
    }

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
