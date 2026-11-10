// Unit test of plugin/src/replay_logic.hpp: the Replay Outfits tab's row cycling, colour cycling, file round trip.
// Build and run: build_replay_logic_test.bat (prints "all ok" and exits 0 when every check passes).
#define _CRT_SECURE_NO_WARNINGS
#include "../src/replay_logic.hpp"
#include "../src/words.hpp"

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
    {   // the Random outfit: the last value of the Costume row, its one colour reading Random
        Fighter r = ryu();
        Outfit rnd;
        rnd.costume = 104;
        rnd.extra = true;
        rnd.random = true;
        rnd.name = "Random";
        rnd.colors = { 0 };
        r.outfits.insert(r.outfits.begin(), rnd);
        sort_outfits(r.outfits);
        check(r.outfits.size() == 6 && r.outfits.back().random, "the Random outfit last, whatever its name");
        Choice rc;
        for (int i = 0; i < 6; ++i) rc = cycle_outfit(r, rc, +1);
        check(rc.forced && rc.costume == 104 && rc.color == 0, "Right to the last outfit: Random, its one colour");
        check(choice_color_label(r.outfits.back(), rc) == slots::words::game(slots::words::G::Random), "its colour reads Random");
        check(cycle_color(r, rc, +1) == rc, "its Color row has that one value");
        const std::string text = to_json({ { 1, rc } });
        Choices back;
        check(from_json(text, back) && back[1] == rc, "saved and read back by its number");
    }

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
    check(z.random_color && z.color == 0 && z.colormod.empty() && choice_color_label(f.outfits[4], z) == slots::words::game(slots::words::G::Random),
          "after the last colour: Random (colour 0 for the preview), named with the game's word");
    z = cycle_color(f, z, +1);
    check(z.color == 0 && !z.random_color, "colour key wraps to the first colour");
    z = cycle_color(f, z, -1);
    check(z.random_color, "colour key backwards from the first: Random, the last value");
    z = cycle_color(f, z, -1);
    check(z.color == 2 && !z.random_color, "then the last colour");
    {   // Random as a colour: kept by an outfit change that keeps the colour, never by a costume change on the page
        Choice rz = z;
        rz.random_color = true; rz.color = 0;
        check(cycle_outfit(f, rz, -1, true).random_color, "an outfit change keeping the colour keeps Random");
        check(!cycle_outfit(f, rz, -1, false).random_color, "a costume change: back to 01");
        Choice rr = z; rr.random_color = true; rr.color = 0;
        Choice r2 = rr; r2.random_color = false;
        check(!(rr == r2), "Random is another choice than colour 01");
        Choices rc;
        rc[1] = rr;
        const std::string rt = to_json(rc);
        Choices rb;
        check(rt.find("\"color_random\": true") != std::string::npos && from_json(rt, rb) && rb[1] == rr, "Random survives the file");
        check(from_json("{\"version\":1,\"fighters\":{\"1\":{\"mode\":\"forced\",\"costume\":0,\"color\":3}}}", rb) && !rb[1].random_color && rb[1].color == 3,
              "an older file (no color_random): its colour as written");
    }
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
    check(v.random_color && v.colormod.empty(), "after the entries: Random, the last value");
    v = cycle_color(g, v, +1);
    check(v.color == 0 && v.colormod.empty() && !v.random_color, "then back to the first game colour");
    v = cycle_color(g, cycle_color(g, v, -1), -1);
    check(v.colormod == vag.cm[1].id && !v.random_color, "backwards from the first game colour: Random, then the last entry");
    Choice v1 = v; v1.colormod.clear();
    check(!(v1 == v), "an entry is another choice than the game colour 02");
    Choice gone_cm = v; gone_cm.colormod = "removed";
    check(choice_color_label(g.outfits[0], gone_cm) == "02", "an entry the Color Mod no longer lists: shown as worn, 02");
    check(cycle_outfit(g, v, +1, false).colormod.empty(), "a costume change: back to 01, no entry");
    Fighter h = g;
    h.outfits[0].colors = { 0, 2 };
    Choice w = cycle_outfit(h, Choice{}, +1);
    w = cycle_color(h, cycle_color(h, w, +1), +1);
    check(w.random_color && w.colormod.empty(), "an outfit without colour 1 offers no entry (it is their carrier): 02 then Random");
    w = cycle_color(h, w, +1);
    check(w.color == 0 && !w.random_color, "and back to 01");
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
    check(slots::words::fighter_name(30) == "C. Viper" && slots::words::fighter_name(23).empty() && slots::words::fighter_caption(30) == "C. VIPER", "fighter names");
    check(game_outfit_name(0) == "Outfit 1" && game_outfit_name(3) == "Outfit 4" && game_outfit_name(4) == "DriveTech Wear",
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
        {   // a line's two parts, in the languages' own patterns
            const LineParts en = line_parts("%s: %s, %s", { "Luke", "Outfit 4", "01" });
            check(en.fixed == "Luke: " && en.moving == "Outfit 4, 01" && en.whole() == "Luke: Outfit 4, 01", "parts: the name and its colon stay, the rest moves");
            const LineParts fr = line_parts("%s : %s, %s", { "Luke", "Type 1", "01" });
            check(fr.fixed == "Luke : " && fr.moving == "Type 1, 01", "parts: French, a space before the colon");
            const LineParts ja = line_parts("%s\xEF\xBC\x9A%s\xE3\x80\x81%s", { "L", "A", "01" });
            check(ja.fixed == "L\xEF\xBC\x9A" && ja.moving == "A\xE3\x80\x81" "01", "parts: Japanese, its own colon and comma");
            const LineParts one_value = line_parts("%s: %s", { "Luke", "Default" });
            check(one_value.fixed == "Luke: " && one_value.moving == "Default", "parts: a name and one value");
            const LineParts pct = line_parts("%s%%: %s", { "A", "b" });
            check(pct.fixed == "A%: " && pct.moving == "b", "parts: a percent sign of the pattern");
            const LineParts no_value = line_parts("nothing", {});
            check(no_value.fixed == "nothing" && no_value.moving.empty(), "parts: no value at all");
        }
        const std::vector<std::string> capped = summary_lines(many, all, 20);
        check(capped.size() == 20 && capped[18] == "F18: Outfit 4, 01" && capped[19] == "+ 6 more",
              "summary: 25 On, 19 lines and \"+ 6 more\"");
        const std::vector<LineParts> capped_parts = summary_parts(many, all, 20);
        check(capped_parts[18].fixed == "F18: " && capped_parts[19].fixed.empty(), "summary: a character's line has a fixed part, \"+ 6 more\" none");
        // Battle Settings > Other's "Game Settings" (read 05/11, its own units): Stage Settings (its picture), Battle BGM,
        // Commentary Settings (6 lines), Side Settings, Battle HUD Design; and as measured on screen (y = 189 + 0.81 x layout)
        using K = SumKind;
        const std::vector<float> ys = summary_layout({ K::Name, K::Picture, K::Name, K::Line, K::Name, K::Line, K::Line, K::Line,
                                                       K::Line, K::Line, K::Line, K::Name, K::Line, K::Name, K::Line });
        const float game[] = { 63, 82, 184, 214, 261, 291, 322, 353, 384, 415, 446, 493, 523, 572, 602 };
        const float screen[] = { 240, 256, 338, 362, 401, 424, 449, 475, 499, 525, 550, 589, 613, 652, 676 };
        bool exact = ys.size() == 15, near = ys.size() == 15;
        for (size_t i = 0; exact && i < ys.size(); ++i) exact = std::fabs(ys[i] - game[i]) <= 2.0f;
        for (size_t i = 0; near && i < ys.size(); ++i) near = std::fabs(189.0f + 0.81f * ys[i] - screen[i]) <= 2.0f;
        check(exact, "summary layout: Battle Settings > Other's panel, its units within 2");
        check(near, "summary layout: the same on screen within 2 px");
        const std::vector<float> page = summary_layout({ K::Name, K::Line, K::Name, K::Line, K::Name, K::Line });
        check(page == std::vector<float>{ 63, 93, 140, 170, 217, 247 }, "summary layout: the character page's three settings");
        const std::vector<float> sl = summary_layout({ K::Name, K::Line, K::Name, K::Picture, K::Line });
        check(sl[3] == sl[2] + kSumPicGap && sl[4] == sl[3] + kSumPicH + kSumLineAfterPic, "summary layout: the stage's name under its picture");
    }

    // the costume options saved per fighter and outfit (replay_options.hpp)
    {
        Choices fc;
        Choice bison; bison.forced = true; bison.costume = 0; bison.color = 2;
        fc[26] = bison;
        OptionStore op;
        op[26]["esf026/game outfit 1"] = { "wesker01" };
        op[26]["esf026/other outfit"] = { "a1", "b2" };
        op[30]["esf030/game outfit 1"] = { "body0001" };       // C. Viper set to Off: its options stay
        const std::string t = to_json(fc, StageChoice{}, op);
        Choices fb; OptionStore ob; StageChoice sb;
        check(from_json(t, fb, &sb, &ob), "options: the file parses");
        check(fb.size() == 1 && fb[26] == bison, "options: only the forced fighter is forced");
        check(ob == op, "options: both outfits of a fighter and an Off fighter's survive the file");
        check(t.find("\"30\": {\"mode\": \"off\", \"options\"") != std::string::npos, "options: an Off fighter is written \"off\"");
        Choices fo; OptionStore oo;
        check(from_json("{\"version\":1,\"fighters\":{\"2\":{\"mode\":\"forced\",\"costume\":1,\"color\":0}}}", fo, nullptr, &oo) &&
              fo.size() == 1 && oo.empty(), "options: a file from before them still loads, none saved");
        Choices fn;
        check(from_json(t, fn) && fn.size() == 1, "options: a caller not asking for them reads the same choices");
        check(to_json(fc) == to_json(fc, StageChoice{}, OptionStore{}), "options: none saved, the file is as before");
        check(to_json(fc).find("options") == std::string::npos, "options: none saved, no key");
        check(from_json("{\"version\":1,\"fighters\":{\"7\":{\"mode\":\"off\",\"options\":{\"m\":[\"a\",3,\"\"],\"n\":\"x\"}}}}", fo, nullptr, &oo) &&
              fo.empty() && oo.size() == 1 && oo[7].size() == 1 && oo[7]["m"] == std::vector<std::string>{ "a" },
              "options: what is not a list of ids is left out");
        // an outfit change on the page: the choice changes, what is saved for the outfits does not depend on it
        Fighter two; two.fid = 26; two.name = "M. Bison";
        two.outfits = { { 0, "", "Outfit 1", false, { 0, 1 } }, { 1, "", "Outfit 2", false, { 0, 1 } } };
        fc[26] = cycle_outfit(two, fc[26], +1, false);
        check(fc[26].costume == 1 && to_json(fc, StageChoice{}, op).find("wesker01") != std::string::npos,
              "options: another outfit chosen, the first one's kept");
        fc.erase(26);
        Choices fd; OptionStore od;
        check(from_json(to_json(fc, StageChoice{}, op), fd, nullptr, &od) && fd.empty() && od == op, "options: the fighter set to Off keeps them");
    }

    // ---- the Battle BGM row (replay_bgm.hpp) ----
    {
        // the game's order: its spin's number + 1 is app.SelectBgm (Self, Opponent, Stage, Random)
        check(static_cast<int>(BgmMode::Character) + 1 == 1 && static_cast<int>(BgmMode::Opponent) + 1 == 2 &&
              static_cast<int>(BgmMode::Stage) + 1 == 3 && static_cast<int>(BgmMode::Random) + 1 == 4, "bgm: the values in the game's order");
        check(BgmChoice{}.mode == BgmMode::Random, "bgm: Random by default");
        check(bgm_cycle(BgmMode::Character, 1) == BgmMode::Opponent && bgm_cycle(BgmMode::Random, 1) == BgmMode::Custom &&
              bgm_cycle(BgmMode::Custom, 1) == BgmMode::Character && bgm_cycle(BgmMode::Character, -1) == BgmMode::Custom,
              "bgm: Left / Right wrap, Custom after the game's values");
        BgmChoice bb;
        check(bgm_set(bb) == -1, "bgm: Random leaves the replay's description alone");
        bb.mode = BgmMode::Character; check(bgm_set(bb) == 1, "bgm: Character is P1 (EBgmSet.Player1)");
        bb.mode = BgmMode::Opponent;  check(bgm_set(bb) == 2, "bgm: Opponent is P2 (EBgmSet.Player2)");
        bb.mode = BgmMode::Stage;     check(bgm_set(bb) == 0, "bgm: Stage (EBgmSet.Stage)");
        bb.mode = BgmMode::Custom;    check(bgm_set(bb) == -1, "bgm: Custom with no list behaves as Random");
        bb.list = { 70034001 };       check(bgm_set(bb) == 1, "bgm: Custom with a list is Player1, whose list the load gets replaced");
        bb.mode = BgmMode::Random;    check(bgm_set(bb) == -1, "bgm: a list kept under another value changes nothing");
        bb.mode = BgmMode::Stage;     check(bgm_set(bb) == 0, "bgm: ...nor under Stage");

        // the list in the file
        {
            const Choices ncw;
            BgmChoice cw; cw.mode = BgmMode::Custom; cw.list = { 70034001, 70034002 };
            const std::string ct = to_json(ncw, StageChoice{}, OptionStore{}, cw);
            check(ct.find("\"bgm\": {\"mode\": \"custom\", \"list\": [70034001, 70034002]}") != std::string::npos, "bgm: the list as the file holds it");
            Choices cc; BgmChoice cb;
            check(from_json(ct, cc, nullptr, nullptr, &cb) && cb == cw, "bgm: the list survives the file");
            cw.mode = BgmMode::Random;
            check(from_json(to_json(ncw, StageChoice{}, OptionStore{}, cw), cc, nullptr, nullptr, &cb) && cb == cw, "bgm: left Custom, the list is kept (Random)");
            cw.mode = BgmMode::Stage;
            check(from_json(to_json(ncw, StageChoice{}, OptionStore{}, cw), cc, nullptr, nullptr, &cb) && cb == cw, "bgm: left Custom, the list is kept (Stage)");
            BgmChoice ce; ce.mode = BgmMode::Custom;
            check(to_json(ncw, StageChoice{}, OptionStore{}, ce).find("list") == std::string::npos, "bgm: an empty list: no key");
            check(from_json("{\"version\":1,\"fighters\":{},\"bgm\":{\"mode\":\"custom\"}}", cc, nullptr, nullptr, &cb) && cb.mode == BgmMode::Custom && cb.list.empty(),
                  "bgm: Custom with no list loads");
            check(from_json("{\"version\":1,\"fighters\":{},\"bgm\":{\"mode\":\"custom\",\"list\":[5,5,\"x\",-1,0,7,4294967296]}}", cc, nullptr, nullptr, &cb) &&
                  cb.list == std::vector<uint32_t>{ 5, 7 }, "bgm: in the list, each track once, only what can be a track");
            std::string many = "{\"version\":1,\"fighters\":{},\"bgm\":{\"mode\":\"custom\",\"list\":[1";
            for (int i = 2; i <= 40; ++i) many += "," + std::to_string(i);
            check(from_json(many + "]}}", cc, nullptr, nullptr, &cb) && cb.list.size() == kBgmListMax, "bgm: at most what the dialog takes");
        }
        // the tracks owned
        check(bgm_keep_owned({ 1, 2, 3 }, { 3, 1, 9 }) == std::vector<uint32_t>{ 1, 3 }, "bgm: a track no longer owned is left out, the order kept");
        check(bgm_keep_owned({ 1, 2 }, { 8 }).empty(), "bgm: none owned: an empty list, which plays as Random");
        check(bgm_keep_owned({ 1, 2 }, {}) == std::vector<uint32_t>{ 1, 2 }, "bgm: what is owned not read: the list as it is");
        check(bgm_keep_owned({ 4, 4 }, { 4 }) == std::vector<uint32_t>{ 4 }, "bgm: each track once");
        // the swap: only on the getter's own copy
        {
            int lists[2] = { 0, 0 };          // two lists at two addresses
            check(bgm_swap_allowed(&lists[0], &lists[1]), "bgm: the swap on the getter's copy");
            check(!bgm_swap_allowed(&lists[1], &lists[1]), "bgm: never on the save's own list");
            check(!bgm_swap_allowed(nullptr, &lists[1]), "bgm: no list, no swap");
            check(bgm_swap_allowed(&lists[0], nullptr), "bgm: a fighter with nothing in the save: its copy");
        }
        check(bgm_custom_line("Custom", 3) == "Custom (3)" && bgm_custom_line("Custom", 0) == "Custom (0)", "bgm: the summary's line");

        const Choices no_c;
        check(to_json(no_c).find("bgm") == std::string::npos && to_json(no_c) == to_json(no_c, StageChoice{}, OptionStore{}, BgmChoice{}),
              "bgm: Random writes no key: a file never touched stays as it was");
        for (const BgmMode bm : { BgmMode::Character, BgmMode::Opponent, BgmMode::Stage, BgmMode::Custom }) {
            BgmChoice bw; bw.mode = bm;
            const std::string bt = to_json(no_c, StageChoice{}, OptionStore{}, bw);
            Choices brc; BgmChoice brb;
            check(from_json(bt, brc, nullptr, nullptr, &brb) && brb == bw, "bgm: the value survives the file");
        }
        Choices brc; BgmChoice brb; brb.mode = BgmMode::Stage;
        check(from_json("{\"version\":1,\"fighters\":{}}", brc, nullptr, nullptr, &brb) && brb.mode == BgmMode::Random, "bgm: an old file loads, Random");
        brb.mode = BgmMode::Stage;
        check(from_json("{\"version\":1,\"fighters\":{},\"bgm\":{\"mode\":\"future\"}}", brc, nullptr, nullptr, &brb) && brb.mode == BgmMode::Random,
              "bgm: a value not known: Random");
        brb.mode = BgmMode::Stage;
        check(from_json("{\"version\":1,\"fighters\":{},\"bgm\":3}", brc, nullptr, nullptr, &brb) && brb.mode == BgmMode::Random, "bgm: not an object: Random");
        BgmChoice bsb; bsb.mode = BgmMode::Opponent;
        StageChoice bst; bst.forced = true; bst.id = 40000;
        Choices bfc; StageChoice brs;
        check(from_json(to_json(no_c, bst, OptionStore{}, bsb), bfc, &brs, nullptr, &brb) && brs == bst && brb == bsb, "bgm: with a stage in the file too");
        check(from_json(to_json(no_c, bst, OptionStore{}, bsb), bfc) && bfc.empty(), "bgm: a caller not asking for it reads the same");

        // a System.Guid's bytes in memory
        uint8_t bg[16] = {};
        const uint8_t want[16] = { 0x31, 0x7b, 0x1a, 0x27, 0xf2, 0xb6, 0x21, 0x43, 0xbd, 0x51, 0xc2, 0xe7, 0xb6, 0xc8, 0x62, 0xf6 };
        check(parse_guid("271a7b31-b6f2-4321-bd51-c2e7b6c862f6", bg) && std::equal(bg, bg + 16, want), "bgm: a GUID's text to its bytes in memory");
        check(bgm_value_guid(BgmMode::Custom) == nullptr && !parse_guid(bgm_value_guid(BgmMode::Custom), bg) && std::string(bgm_value_english(BgmMode::Custom)) == "Custom",
              "bgm: Custom's word is ours, no message");
        check(!parse_guid("271a7b31-b6f2-4321-bd51", bg) && !parse_guid("271a7b31xb6f2-4321-bd51-c2e7b6c862f6", bg) && !parse_guid(nullptr, bg) &&
              !parse_guid("271a7b31-b6f2-4321-bd51-c2e7b6c862f6a", bg), "bgm: not a GUID: refused");
        for (const BgmMode bm : { BgmMode::Character, BgmMode::Opponent, BgmMode::Stage, BgmMode::Random })
            check(parse_guid(bgm_value_guid(bm), bg) && bgm_value_english(bm)[0] != 0, "bgm: each value has its words");
    }

    std::printf(fails ? "%d FAILED\n" : "all ok\n", fails);
    return fails ? 1 : 0;
}
