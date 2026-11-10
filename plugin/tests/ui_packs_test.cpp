// Unit test of plugin/src/ui_packs_logic.hpp (the interface packs: the loader's registry, a pack as the game's Options rows - a one-of
// group one row, a check box one row Off / On, headings as header rows or in front of their rows' titles -, the rows' values and
// pictures, the modinfo descriptions on one line, settings.json's "ui_choices") and json.cpp's json_with_key (settings.json written
// with one key, the others kept).
// Build and run: build_ui_packs_test.bat (prints "all ok" and exits 0 when every check passes).
// ui_packs_test.exe <registry.json>: the sections and rows that registry gives (read only), no test.
#define _CRT_SECURE_NO_WARNINGS
#include "../src/ui_packs_logic.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace slots;
using namespace slots::uipacks;

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-80s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

// A registry as the contract writes it (10/11, v1): a pack with a heading, a one-of group, boxes, a preset and a sub group under a
// box (the "Comic FG" add-ons); a pack of gathered roots (the Loading Animations' archives: one group at the pack's level); a pack
// whose headings hold rows of several kinds (the Round Announcement's components); a pack of headings alone (left out); a pack key
// seen before (left out); a node without an id and one seen before (left out).
static const char* kRegistry =
    "\xEF\xBB\xBF{ \"version\": 1,\n"
    "  \"packs\": [\n"
    "    { \"key\": \"u0123456789ab\", \"name\": \"Comic Art Bundle Mod\", \"author\": \"BrewedVFX\", \"picture\": \"_uislots/u0123456789ab/preview.tex\",\n"
    "      \"nodes\": [\n"
    "        { \"id\": \"nA\", \"parent\": \"\", \"name\": \"Comic Art\", \"description\": \"The bundle\", \"kind\": \"title\", \"choice\": \"\", \"group\": \"\", \"files\": 0, \"preview\": \"\", \"depth\": 1 },\n"
    "        { \"id\": \"nB\", \"parent\": \"nA\", \"name\": \"Portraits A\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"nA\", \"files\": 12, \"preview\": \"_uislots/u0123456789ab/nB/preview.tex\", \"depth\": 2 },\n"
    "        { \"id\": \"nC\", \"parent\": \"nA\", \"name\": \"Portraits B\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"nA\", \"files\": 12, \"preview\": \"\", \"depth\": 2 },\n"
    "        { \"id\": \"nD\", \"parent\": \"nA\", \"name\": \"Remove Purchase Cart on Character Select and Crown Avatar Arcade\", \"description\": \"Replaces Arcade Artwork Dark Reign \\\\n\\\\nArt By: Someone\", \"kind\": \"option\", \"choice\": \"check\", \"group\": \"\", \"files\": 3, \"preview\": \"_uislots/u0123456789ab/nD/preview.tex\", \"depth\": 2 },\n"
    "        { \"id\": \"nE\", \"parent\": \"nD\", \"name\": \"Comic FG\", \"kind\": \"title\", \"depth\": 3, \"future_field\": [1, 2] },\n"
    "        { \"id\": \"nF\", \"parent\": \"nE\", \"name\": \"FG Red\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"nE\", \"files\": 4, \"preview\": \"_uislots/u0123456789ab/nF/preview.tex\", \"depth\": 4 },\n"
    "        { \"id\": \"nG\", \"parent\": \"nE\", \"name\": \"FG Blue\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"nE\", \"files\": 4, \"preview\": \"_uislots/u0123456789ab/nG/preview.tex\", \"depth\": 4 },\n"
    "        { \"id\": \"nH\", \"parent\": \"nA\", \"name\": \"Default Set\", \"kind\": \"option\", \"choice\": \"check\", \"files\": 30, \"preview\": \"\" },\n"
    "        { \"id\": \"\", \"name\": \"no id\", \"kind\": \"option\" },\n"
    "        { \"id\": \"nB\", \"name\": \"seen before\", \"kind\": \"option\" }\n"
    "      ] },\n"
    "    { \"key\": \"uffffffffffff\", \"name\": \"\", \"author\": \"\", \"picture\": \"\",\n"
    "      \"nodes\": [\n"
    "        { \"id\": \"nL1\", \"parent\": \"\", \"name\": \"Loading 1\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"\", \"files\": 2, \"preview\": \"_uislots/uffffffffffff/nL1/preview.tex\", \"depth\": 1 },\n"
    "        { \"id\": \"nL2\", \"parent\": \"\", \"name\": \"Loading 2\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"\", \"files\": 2, \"preview\": \"\", \"depth\": 1 },\n"
    "        { \"id\": \"nL3\", \"parent\": \"\", \"name\": \"Loading 3\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"\", \"files\": 2, \"preview\": \"\", \"depth\": 1 }\n"
    "      ] },\n"
    "    { \"key\": \"u333333333333\", \"name\": \"Round Announcement UI\", \"author\": \"\",\n"
    "      \"nodes\": [\n"
    "        { \"id\": \"r0\", \"parent\": \"\", \"name\": \"Round UI\", \"kind\": \"title\", \"depth\": 1 },\n"
    "        { \"id\": \"r1\", \"parent\": \"r0\", \"name\": \"Announcer\", \"description\": \"Who says it\", \"kind\": \"title\", \"depth\": 2 },\n"
    "        { \"id\": \"r2\", \"parent\": \"r1\", \"name\": \"Voice A\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"r1\", \"preview\": \"_uislots/u333333333333/r2/preview.tex\", \"depth\": 3 },\n"
    "        { \"id\": \"r3\", \"parent\": \"r1\", \"name\": \"Voice B\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"r1\", \"depth\": 3 },\n"
    "        { \"id\": \"r4\", \"parent\": \"r1\", \"name\": \"Big letters\", \"kind\": \"option\", \"choice\": \"check\", \"depth\": 3 },\n"
    "        { \"id\": \"r5\", \"parent\": \"r0\", \"name\": \"Colours\", \"kind\": \"title\", \"depth\": 2 },\n"
    "        { \"id\": \"r6\", \"parent\": \"r5\", \"name\": \"Gold\", \"kind\": \"option\", \"choice\": \"check\", \"depth\": 3 }\n"
    "      ] },\n"
    "    { \"key\": \"u222222222222\", \"name\": \"Headings only\", \"nodes\": [ { \"id\": \"nT\", \"kind\": \"title\" } ] },\n"
    "    { \"key\": \"u0123456789ab\", \"name\": \"the same key again\", \"nodes\": [ { \"id\": \"nX\", \"kind\": \"option\" } ] }\n"
    "  ],\n"
    "  \"applied\": { \"u0123456789ab\": [\"nB\", \"nD\"], \"ugone\": [] } }\n";

static Ticks T(std::initializer_list<const char*> ids) { Ticks t; for (const char* s : ids) t.push_back(s); return t; }
static bool nodes_are(const Row& r, std::initializer_list<int> want) { return r.nodes == std::vector<int>(want); }

static void test_registry(Registry& r) {
    std::printf("registry.json\n");
    check(parse_registry(kRegistry, r), "the contract's registry parses (BOM in front)");
    check(r.version == 1, "its version");
    check(r.packs.size() == 3, "three packs: one of headings only and a key seen before left out");
    if (r.packs.size() != 3) return;
    const Pack& a = r.packs[0];
    check(a.key == "u0123456789ab" && a.name == "Comic Art Bundle Mod" && a.author == "BrewedVFX" && a.picture == "_uislots/u0123456789ab/preview.tex",
          "the pack's key, name, author and picture");
    check(a.nodes.size() == 8, "8 nodes: one without an id and one seen before left out");
    check(a.nodes[0].title && !a.nodes[0].one && a.nodes[0].depth == 1, "a heading");
    check(a.nodes[1].one && a.nodes[1].group == "nA" && a.nodes[1].files == 12 && a.nodes[1].depth == 2, "a radio: its group, files, depth");
    check(!a.nodes[3].one && !a.nodes[3].title, "a check box");
    check(a.nodes[4].title && a.nodes[4].depth == 3, "a heading under a box (unknown fields skipped)");
    check(a.nodes[7].depth == 2, "a node without a depth: its parent's + 1");
    check(r.packs[1].name == "uffffffffffff", "a pack without a name: its key");
    check(group_of(r.packs[1].nodes[0]) == "" && group_of(r.packs[1].nodes[2]) == "", "gathered roots: one group at the pack's level");
    check(min_depth(a) == 1 && min_depth(r.packs[1]) == 1, "the pack's first level");
    check(r.applied.size() == 2 && r.applied["u0123456789ab"] == T({ "nB", "nD" }) && r.applied["ugone"].empty(), "what this launch serves");
    Registry bad;
    check(!parse_registry("", bad) && bad.packs.empty(), "no file: no pack");
    check(!parse_registry("[1, 2]", bad) && bad.packs.empty(), "not an object: no pack");
    check(parse_registry("{ \"version\": 1 }", bad) && bad.packs.empty(), "no packs: no pack");
    std::printf("sanitize\n");
    check(sanitize(a, T({ "nH", "zz", "nA", "nC", "nB", "nD" })) == T({ "nB", "nD", "nH" }),
          "unknown ids and headings dropped, one per group (the first in display order), display order");
    check(sanitize(r.packs[1], T({ "nL2", "nL1" })) == T({ "nL1" }), "gathered roots: one kept");
    check(sanitize(a, {}).empty(), "nothing ticked");
}

static void test_rows(const Registry& r) {
    std::printf("a pack as the game's rows\n");
    const Pack& a = r.packs[0];
    for (bool headers : { true, false }) {
        const std::vector<Row> rows = rows_of(a, headers);
        check(rows.size() == 4, headers ? "Comic Art (headers): 4 rows, no header (its root and a heading of its group alone)" : "Comic Art (no headers): 4 rows");
        if (rows.size() != 4) continue;
        check(rows[0].kind == Row::Group && rows[0].title == "Comic Art" && nodes_are(rows[0], { 1, 2 }) && rows[0].head == 0,
              "  its root's group: one row, the root's name, its two options");
        check(rows[1].kind == Row::Box && rows[1].title == a.nodes[3].name && nodes_are(rows[1], { 3 }), "  a box: one row, its whole name");
        check(rows[2].kind == Row::Group && rows[2].title == "Comic FG" && nodes_are(rows[2], { 5, 6 }) && rows[2].head == 4,
              "  the add-ons' group under the box: one row, its heading's name");
        check(rows[3].kind == Row::Box && rows[3].title == "Default Set", "  the preset: a box row");
    }
    const std::vector<Row> loading = rows_of(r.packs[1], true);
    check(loading.size() == 1 && loading[0].kind == Row::Group && loading[0].title == "uffffffffffff" && nodes_are(loading[0], { 0, 1, 2 }) &&
          loading[0].head == -1, "gathered roots: one row named as the pack, its three options");
    const Pack& u = r.packs[2];
    const std::vector<Row> h = rows_of(u, true);
    check(h.size() == 5 && h[0].kind == Row::Header && h[0].title == "Announcer" && h[1].kind == Row::Group && h[1].title == "Announcer" &&
          nodes_are(h[1], { 2, 3 }) && h[2].kind == Row::Box && h[2].title == "Big letters" && h[3].kind == Row::Header && h[3].title == "Colours" &&
          h[4].kind == Row::Box && h[4].title == "Gold", "headings holding other rows: header rows (the root never one)");
    const std::vector<Row> f = rows_of(u, false);
    check(f.size() == 3 && f[0].kind == Row::Group && f[0].title == "Announcer" && f[1].title == "Big letters" && f[2].title == "Gold",
          "no header rows: each row its own name");
    check(header_ids(a).empty() && header_ids(u) == std::set<std::string>({ "r1", "r5" }), "which headings would be headers");

    std::printf("values\n");
    const std::vector<Row> rows = rows_of(a, true);
    const std::vector<std::string> gl = labels_of(a, rows[0], "Game's own", "Off", "On");
    check(gl == std::vector<std::string>({ "Game's own", "Portraits A", "Portraits B" }), "a group's values: the game's own first, then its options in order");
    check(labels_of(a, rows[1], "Game's own", "Off", "On") == std::vector<std::string>({ "Off", "On" }), "a box's values: the game's Off / On");
    check(labels_of(u, h[0], "x", "Off", "On").empty() && value_of(u, h[0], {}) == 0, "a header: no value");
    Ticks t;
    check(value_of(a, rows[0], t) == 1 && value_of(a, rows[1], t) == 1, "nothing ticked: the game's own, Off");
    check(set_value(a, rows[0], 3, t) && t == T({ "nC" }) && value_of(a, rows[0], t) == 3, "a group's third value: its second option");
    check(set_value(a, rows[0], 2, t) && t == T({ "nB" }) && value_of(a, rows[0], t) == 2, "its second: the tick moves");
    check(!set_value(a, rows[0], 2, t) && t == T({ "nB" }), "the same value again: nothing changes");
    check(set_value(a, rows[1], 2, t) && set_value(a, rows[3], 2, t) && t == T({ "nB", "nD", "nH" }), "two boxes On with the group's option");
    check(set_value(a, rows[2], 3, t) && t == T({ "nB", "nD", "nG", "nH" }) && value_of(a, rows[0], t) == 2, "another group: the first keeps its value");
    check(set_value(a, rows[0], 1, t) && t == T({ "nD", "nG", "nH" }) && value_of(a, rows[0], t) == 1, "back to the game's own: none of its group");
    check(set_value(a, rows[1], 1, t) && t == T({ "nG", "nH" }), "a box Off: the others stay");
    check(!set_value(a, rows[0], 4, t) && !set_value(a, rows[1], 3, t) && !set_value(a, rows[0], 0, t) && !set_value(u, h[0], 1, t) && t == T({ "nG", "nH" }),
          "out of range, or a header: nothing changes");
    Ticks l;
    check(set_value(r.packs[1], loading[0], 4, l) && l == T({ "nL3" }) && set_value(r.packs[1], loading[0], 2, l) && l == T({ "nL1" }),
          "gathered roots: one of them at a time");

    std::printf("pictures\n");
    check(picture_of(a, rows[0], 2) == a.nodes[1].preview, "a group's value: its option's picture");
    check(picture_of(a, rows[0], 3).empty() && picture_of(a, rows[0], 1).empty(), "an option without one, the game's own: none");
    check(picture_of(a, rows[1], 1) == a.nodes[3].preview && picture_of(a, rows[1], 2) == a.nodes[3].preview, "a box: its option's, On or Off");
    check(picture_of(a, rows[0], 9).empty() && picture_of(u, h[0], 0).empty(), "out of range, a header: none");

    std::printf("descriptions\n");
    check(one_line(a.nodes[3].description) == "Replaces Arcade Artwork Dark Reign Art By: Someone", "a modinfo's literal \\n: one space, runs of them one");
    check(one_line("a\r\nb\tc  \n") == "a b c" && one_line("  x  ") == "x" && one_line("").empty() && one_line("\\n\\n").empty(),
          "real line breaks and tabs too, none at the ends");
    check(one_line("C:\\\\mods\\\\new") == "C:\\\\mods\\\\new" && one_line("a\\\\b") == "a\\\\b", "other backslashes kept");
}

// A pack made here: n nodes as (id, parent, name, kind 't' title / 'o' one / 'c' check, group, depth, source)
struct N { const char* id; const char* parent; const char* name; char kind; const char* group; int depth; const char* source; };
static Pack make(const char* name, std::initializer_list<N> nodes) {
    Pack p;
    p.key = "u" + std::string(name);
    p.name = name;
    for (const N& n : nodes) {
        Node d;
        d.id = n.id; d.parent = n.parent; d.name = n.name; d.group = n.group; d.depth = n.depth; d.source = n.source;
        d.title = n.kind == 't';
        d.one = n.kind == 'o';
        p.nodes.push_back(d);
    }
    return p;
}
static std::vector<std::string> titles(const Pack& p, std::vector<Row>& rows) {
    std::vector<std::string> out;
    const auto parts = split_rows(p, rows, 25);
    for (const auto& part : parts) unique_titles(p, rows, part, "%s: %s");
    for (const Row& r : rows) out.push_back(r.title);
    return out;
}

static void test_titles() {
    std::printf("titles (unique within a section)\n");
    // Battlehud Portraits Colour's shape: under each "Customise" a fighter heading, its P1 options one group (the heading's id), its
    // P2 options another (the first P2 option's id: one of several groups under one parent)
    const Pack bh = make("Battlehud", {
        { "b0", "", "Battlehud", 't', "", 1, "Base.rar/x/100) Battlehud" }, { "b1", "b0", "C1 Full", 'c', "", 2, "Base.rar/x/C1 Full" },
        { "b2", "b0", "Customise", 't', "", 2, "AKI.rar/a/101) Roster" }, { "b3", "b2", "AKI C1-C4", 't', "", 3, "AKI.rar/a/213) AKI" },
        { "b4", "b3", "AKI C1 Alt (P1)", 'o', "b3", 4, "AKI.rar/a/c1" }, { "b5", "b3", "AKI C1 Alt (P2)", 'o', "b5", 4, "AKI.rar/a/c1p2" },
        { "b6", "b3", "AKI C2 (P1)", 'o', "b3", 4, "AKI.rar/a/c2" }, { "b7", "b3", "AKI C2 (P2)", 'o', "b5", 4, "AKI.rar/a/c2p2" },
        { "b8", "b0", "Customise", 't', "", 2, "Ryu.rar/r/101) Roster" }, { "b9", "b8", "Ryu C1-C3", 't', "", 3, "Ryu.rar/r/ryu" },
        { "b10", "b9", "Ryu C1 Alt P1", 'o', "b9", 4, "Ryu.rar/r/1" }, { "b11", "b9", "Ryu C1 Alt P2", 'o', "b11", 4, "Ryu.rar/r/2" },
        { "b12", "b9", "Ryu C2 P1", 'o', "b9", 4, "Ryu.rar/r/3" }, { "b13", "b9", "Ryu C2 P2", 'o', "b11", 4, "Ryu.rar/r/4" } });
    std::vector<Row> rows = rows_of(bh, false);
    check(rows.size() == 5 && rows[1].nodes == std::vector<int>({ 4, 6 }) && rows[2].nodes == std::vector<int>({ 5, 7 }) && rows[2].head == 3,
          "a group named by one of its options: its parent heading names it");
    check(titles(bh, rows) == std::vector<std::string>({ "C1 Full", "AKI C1-C4 (P1)", "AKI C1-C4 (P2)", "Ryu C1-C3 (P1)", "Ryu C1-C3 (P2)" }),
          "same heading, same archive: what each group's options share");
    // FG Artwork's shape: every group under "FG Optional", each its own name: no heading in front
    const Pack fg = make("FG", {
        { "f0", "", "FG Artwork", 't', "", 1, "FG.zip/x/0" }, { "f1", "f0", "Default Set", 'c', "", 2, "FG.zip/x/1" },
        { "f2", "f0", "FG Optional", 't', "", 2, "FG.zip/x/2" }, { "f3", "f2", "1) Arcade", 't', "", 3, "FG.zip/x/3" },
        { "f4", "f3", "Dark Reign", 'o', "f3", 4, "FG.zip/x/4" }, { "f5", "f3", "Luke", 'o', "f3", 4, "FG.zip/x/5" },
        { "f6", "f2", "2) Practice", 't', "", 3, "FG.zip/x/6" }, { "f7", "f6", "Peace Out", 'o', "f6", 4, "FG.zip/x/7" } });
    rows = rows_of(fg, false);
    check(titles(fg, rows) == std::vector<std::string>({ "Default Set", "1) Arcade", "2) Practice" }), "unique titles: their own names alone");
    // two boxes of the same name under different headings: the nearest heading in front; under same-named headings of two archives:
    // the archives'; the same everywhere: a number
    const Pack rd = make("Round", {
        { "r0", "", "Round UI", 't', "", 1, "R.zip/x/0" }, { "r1", "r0", "1) Fight", 't', "", 2, "R.zip/x/1" },
        { "r2", "r1", "Remove", 'c', "", 3, "R.zip/x/2" }, { "r3", "r0", "2) Round", 't', "", 2, "R.zip/x/3" },
        { "r4", "r3", "Remove", 'c', "", 3, "R.zip/x/4" }, { "r5", "r0", "Customise", 't', "", 2, "A.zip/a/5" },
        { "r6", "r5", "Same", 'c', "", 3, "A.zip/a/6" }, { "r7", "r0", "Customise", 't', "", 2, "B.zip/b/7" },
        { "r8", "r7", "Same", 'c', "", 3, "B.zip/b/8" }, { "r9", "r0", "Dup", 'c', "", 2, "R.zip/x/9" }, { "r10", "r0", "Dup", 'c', "", 2, "R.zip/x/10" } });
    rows = rows_of(rd, false);
    check(titles(rd, rows) == std::vector<std::string>({ "1) Fight: Remove", "2) Round: Remove", "A: Same", "B: Same", "Dup 1", "Dup 2" }),
          "the nearest heading that tells them apart, else the archives', else a number");
    // Round Announcement's "9) Combo Counters": three groups under one heading; "XX) Other": two, one sharing nothing
    const Pack cc = make("Combo", {
        { "c0", "", "Round Announcement UI", 't', "", 1, "R.zip/x/0" }, { "c1", "c0", "9) Combo Counters & Win Streaks", 't', "", 2, "R.zip/x/1" },
        { "c2", "c1", "3a) Fire (Combo Counter and SA)", 'o', "c1", 3, "R.zip/x/2" }, { "c3", "c1", "3b) Fire (Win Streak)", 'o', "c3", 3, "R.zip/x/3" },
        { "c4", "c1", "4a) Neon (Combo Counter and SA)", 'o', "c1", 3, "R.zip/x/4" }, { "c5", "c1", "4b) Neon (Win Streak)", 'o', "c3", 3, "R.zip/x/5" },
        { "c6", "c0", "XX) Other", 't', "", 2, "R.zip/x/6" }, { "c7", "c6", "Chun-Li Loading", 'o', "c6", 3, "R.zip/x/7" },
        { "c8", "c6", "Dhalsim Loading (Fire Theme Bonus)", 'o', "c6", 3, "R.zip/x/8" },
        { "c9", "c6", "Remove Operation Type (Classic)", 'o', "c9", 3, "R.zip/x/9" },
        { "c10", "c6", "Remove Operation Type ALL (Classic/Modern/Dynamic)", 'o', "c9", 3, "R.zip/x/10" } });
    rows = rows_of(cc, false);
    check(titles(cc, rows) == std::vector<std::string>({ "9) Combo Counters & Win Streaks (Combo Counter and SA)", "9) Combo Counters & Win Streaks (Win Streak)",
                                                          "XX) Other", "XX) Other (Remove Operation Type)" }),
          "what each group's options share, at their end or their start; one sharing nothing keeps the name");
    check(common_part(cc, rows[2]).empty() && common_part(cc, rows[3]) == "Remove Operation Type" && common_part(bh, rows_of(bh, false)[1]) == "P1",
          "  what they share: whole words, without brackets");
    std::printf("values\n");
    const Pack ld = make("Loading", {
        { "l1", "", "Chun-Li Token Loading", 'o', "uLoading", 1, "Chun Li Loading.zip/x" }, { "l2", "", "Ingrid Loading", 'o', "uLoading", 1, "Ingrid Loading 8 Frames.zip/x" },
        { "l3", "", "Ingrid Loading", 'o', "uLoading", 1, "Ingrid Loading.zip/x" } });
    rows = rows_of(ld, false);
    check(rows.size() == 1 && rows[0].title == "Loading" && labels_of(ld, rows[0], "G", "Off", "On") ==
          std::vector<std::string>({ "G", "Chun-Li Token Loading", "Ingrid Loading (Ingrid Loading 8 Frames)", "Ingrid Loading (Ingrid Loading)" }),
          "gathered roots (the pack's key as group): the pack's name; two values of one name: their archives'");
    Pack ld2 = ld;
    ld2.nodes[2].source = ld2.nodes[1].source;
    check(labels_of(ld2, rows[0], "G", "Off", "On")[2] == "Ingrid Loading 1" && labels_of(ld2, rows[0], "G", "Off", "On")[3] == "Ingrid Loading 2",
          "  the same archive: a number");
}

static void test_split() {
    std::printf("sections of at most 25 rows\n");
    std::vector<N> nodes = { { "s0", "", "Big", 't', "", 1, "B.zip/x" } };
    static std::vector<std::string> ids;                    // the names the nodes point at (room for all: never moved)
    ids.clear();
    ids.reserve(1024);
    for (int h = 0; h < 6; ++h) {
        ids.push_back("h" + std::to_string(h));
        nodes.push_back({ ids.back().c_str(), "s0", "Heading", 't', "", 2, "B.zip/x" });
        const char* parent = ids.back().c_str();
        for (int k = 0; k < 5; ++k) { ids.push_back("h" + std::to_string(h) + "b" + std::to_string(k)); nodes.push_back({ ids.back().c_str(), parent, "Box", 'c', "", 3, "B.zip/x" }); }
    }
    Pack big;
    big.key = "uBig"; big.name = "Big";
    for (const N& n : nodes) { Node d; d.id = n.id; d.parent = n.parent; d.name = n.name; d.depth = n.depth; d.title = n.kind == 't'; d.source = n.source; big.nodes.push_back(d); }
    std::vector<Row> rows = rows_of(big, false);
    auto parts = split_rows(big, rows, 25);
    check(rows.size() == 30 && parts.size() == 2 && parts[0].size() == 25 && parts[1].size() == 5, "30 rows in blocks of 5: 25 + 5, no block cut");
    parts = split_rows(big, rows, 12);
    check(parts.size() == 3 && parts[0].size() == 10 && parts[1].size() == 10 && parts[2].size() == 10, "at most 12: two blocks a section");
    parts = split_rows(big, rows, 4);
    check(parts.size() == 12 && parts[0].size() == 4 && parts[1].size() == 1, "a block larger than a section: in pieces");
    check(split_rows(big, rows, 30).size() == 1 && split_rows(big, {}, 25).empty(), "all of it fits: one section; no row: none");
    // the real Battlehud: a box at the top, then 19 blocks of two rows: 25 + 14
    std::vector<N> bh = { { "r", "", "Battlehud", 't', "", 1, "B.rar/x" }, { "c1", "r", "C1 Full", 'c', "", 2, "B.rar/x" } };
    for (int f = 0; f < 19; ++f) {
        ids.push_back("cu" + std::to_string(f)); const char* cu = ids.back().c_str();
        bh.push_back({ cu, "r", "Customise", 't', "", 2, "B.rar/x" });
        ids.push_back("fi" + std::to_string(f)); const char* fi = ids.back().c_str();
        bh.push_back({ fi, cu, "Fighter", 't', "", 3, "B.rar/x" });
        ids.push_back("p1" + std::to_string(f)); bh.push_back({ ids.back().c_str(), fi, "X (P1)", 'o', fi, 4, "B.rar/x" });
        ids.push_back("p2" + std::to_string(f)); const char* p2 = ids.back().c_str(); bh.push_back({ p2, fi, "X (P2)", 'o', p2, 4, "B.rar/x" });
    }
    Pack b;
    b.key = "uB"; b.name = "Battlehud";
    for (const N& n : bh) { Node d; d.id = n.id; d.parent = n.parent; d.name = n.name; d.group = n.group; d.depth = n.depth; d.title = n.kind == 't'; d.one = n.kind == 'o'; d.source = n.source; b.nodes.push_back(d); }
    rows = rows_of(b, false);
    parts = split_rows(b, rows, 25);
    check(rows.size() == 39 && parts.size() == 2 && parts[0].size() == 25 && parts[1].size() == 14, "Battlehud's 39 rows: 25 + 14, a fighter's two rows together");
}

// The registry's sections and rows, as the game gets them (no header rows)
static int dump(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    Registry r;
    if (!parse_registry(ss.str(), r)) { std::printf("unreadable: %s\n", path); return 1; }
    for (const Pack& p : r.packs) {
        std::vector<Row> rows = rows_of(p, false);
        const auto parts = split_rows(p, rows, 25);
        for (const auto& part : parts) unique_titles(p, rows, part, "%s: %s");
        for (size_t s = 0; s < parts.size(); ++s) {
            std::printf("SECTION \"%s%s\" (%zu rows)\n", p.name.c_str(), parts.size() > 1 ? (" " + std::to_string(s + 1)).c_str() : "", parts[s].size());
            for (int i : parts[s]) {
                const Row& row = rows[static_cast<size_t>(i)];
                const auto labels = labels_of(p, row, "Game's own", "Off", "On");
                std::string vals;
                for (size_t k = 0; k < labels.size() && k < 3; ++k) vals += (k ? " | " : "") + labels[k];
                if (labels.size() > 3) vals += " | ... " + std::to_string(labels.size()) + " values";
                std::printf("   %s %s  < %s >\n", row.kind == Row::Box ? "box  " : "group", row.title.c_str(), vals.c_str());
            }
        }
    }
    return 0;
}

static void test_choices(const Registry& r) {
    std::printf("settings.json's ui_choices\n");
    const std::string text = "\xEF\xBB\xBF{\n    \"display_nsfw\": true,\n    \"title_random_pool\": [\"game\", \"t1\"],\n"
                             "    \"ui_choices\": { \"u0123456789ab\": [\"nB\", \"nB\", 3, \"nD\"], \"ugone\": [\"nZ\"], \"uempty\": [], \"ubad\": 1 },\n"
                             "    \"select_options_key\": \"C\"\n}\n";
    Json j;
    check(json_parse(strip_bom(text), j), "a settings.json with every kind of key parses");
    Choices c = parse_choices(j);
    check(c.size() == 2 && c["u0123456789ab"] == T({ "nB", "nD" }) && c["ugone"] == T({ "nZ" }),
          "its packs: strings only, once each; empty and wrong ones left out");
    Ticks t = sanitize(r.packs[0], c["u0123456789ab"]);
    const std::vector<Row> rows = rows_of(r.packs[0], true);
    check(set_value(r.packs[0], rows[0], 3, t), "a row changed in the game");
    set_pack(c, "u0123456789ab", t);
    check(c["u0123456789ab"] == T({ "nC", "nD" }) && c.count("ugone"), "the pack changed set, a pack the registry no longer lists kept");
    const Json cj = choices_json(c);
    const std::string merged = json_with_key(text, "ui_choices", cj, "{}");
    Json back;
    check(json_parse(merged, back) && back.kind == Json::Object, "settings.json written back parses");
    check(back.obj.size() == 4 && back.obj[0].first == "display_nsfw" && back.obj[1].first == "title_random_pool" &&
          back.obj[2].first == "ui_choices" && back.obj[3].first == "select_options_key", "every other key kept, in its place");
    const Json* pool = back.get("title_random_pool");
    check(back.get("display_nsfw") && back.get("display_nsfw")->b && pool && pool->arr.size() == 2 && pool->arr[1].s == "t1",
          "their values kept");
    Choices again = parse_choices(back);
    check(again == c, "ui_choices read back as written");
    set_pack(again, "u0123456789ab", {});
    set_pack(again, "ugone", {});
    const std::string none = json_with_key(merged, "ui_choices", choices_json(again), "{}");
    check(json_parse(none, back) && back.get("ui_choices") && back.get("ui_choices")->kind == Json::Object && back.get("ui_choices")->obj.empty(),
          "nothing ticked anywhere: an empty object");
    const std::string fresh = json_with_key("", "ui_choices", cj, "{\n    \"display_nsfw\": false\n}\n");
    check(json_parse(fresh, back) && back.obj.size() == 2 && back.obj[0].first == "ui_choices" && back.get("display_nsfw"),
          "no file: the defaults with the key first");
    const std::string broken = json_with_key("{ \"display_nsfw\": tru", "ui_choices", cj, "{}");
    check(json_parse(broken, back) && back.obj.size() == 1 && back.get("ui_choices"), "an unreadable file: the fallback with the key");
    Json cleared;
    check(json_parse(json_write(choices_json(Choices{})), cleared) && cleared.kind == Json::Object && cleared.obj.empty(),
          "no choice: {} (what SF6UISlots_Apply gets)");
}

// A portraits pack as the loader writes it (10/11, the fighters fields): a box over two fighters (001 and 013, 1p and 2p each), the
// fighter 013's P1 and P2 groups of two options each (one with a stray file of another name in 013's folder), a box of no fighter.
static const char* kGridRegistry =
    "{ \"version\": 1, \"packs\": [ { \"key\": \"u58e2c904cd20\", \"name\": \"Battlehud Portraits Colour\", \"author\": \"brewedVFX\", \"nodes\": [\n"
    "  { \"id\": \"n0\", \"parent\": \"\", \"name\": \"Battlehud Portraits Colour\", \"kind\": \"title\", \"depth\": 0 },\n"
    "  { \"id\": \"nbase\", \"parent\": \"n0\", \"name\": \"C1 Battlehud Colour (Full)\", \"kind\": \"option\", \"choice\": \"check\", \"depth\": 1, \"fighters_only\": true,\n"
    "    \"fighters\": { \"001\": [\"product/gui/data/character_image/001/battlehud/tex_battlehud_001_1p_iam.tex\", \"product/gui/data/character_image/001/battlehud/tex_battlehud_001_2p_iam.tex\"],\n"
    "                    \"013\": [\"product/gui/data/character_image/013/battlehud/tex_battlehud_013_1p_iam.tex\", \"product/gui/data/character_image/013/battlehud/tex_battlehud_013_2p_iam.tex\"] } },\n"
    "  { \"id\": \"nt\", \"parent\": \"n0\", \"name\": \"AKI C1-C4\", \"kind\": \"title\", \"depth\": 1 },\n"
    "  { \"id\": \"na1\", \"parent\": \"nt\", \"name\": \"AKI C1 Alt (P1)\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"nt\", \"depth\": 2, \"fighters_only\": true,\n"
    "    \"fighters\": { \"013\": [\"product/gui/data/character_image/013/battlehud/tex_battlehud_013_1p_iam.tex\"] } },\n"
    "  { \"id\": \"nb1\", \"parent\": \"nt\", \"name\": \"AKI C1 Alt (P2)\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"nb\", \"depth\": 2, \"fighters_only\": true,\n"
    "    \"fighters\": { \"013\": [\"product/gui/data/character_image/013/battlehud/tex_battlehud_013_2p_iam.tex\"] } },\n"
    "  { \"id\": \"na3\", \"parent\": \"nt\", \"name\": \"AKI C3 (P1)\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"nt\", \"depth\": 2, \"fighters_only\": true,\n"
    "    \"fighters\": { \"013\": [\"product/gui/data/character_image/013/battlehud/tex_battlehud_012_1p_iam.tex\", \"product/gui/data/character_image/013/battlehud/tex_battlehud_013_1p_iam.tex\"] } },\n"
    "  { \"id\": \"nb3\", \"parent\": \"nt\", \"name\": \"AKI C3 (P2)\", \"kind\": \"option\", \"choice\": \"one\", \"group\": \"nb\", \"depth\": 2, \"fighters_only\": true,\n"
    "    \"fighters\": { \"013\": [\"product/gui/data/character_image/013/battlehud/tex_battlehud_013_2p_iam.tex\"] } },\n"
    "  { \"id\": \"nx\", \"parent\": \"n0\", \"name\": \"Big round letters\", \"kind\": \"option\", \"choice\": \"check\", \"depth\": 1 }\n"
    "  ] } ],\n"
    "  \"applied\": {},\n"
    "  \"game_textures\": { \"product/gui/data/character_image/013/battlehud/tex_battlehud_013_1p_iam.tex\": \"_uislots/game/product/gui/data/character_image/013/battlehud/tex_battlehud_013_1p_iam.tex\" } }\n";

static void test_grid() {
    std::printf("grid\n");
    Registry r;
    check(parse_registry(kGridRegistry, r) && r.packs.size() == 1, "a portraits pack read");
    if (r.packs.size() != 1) return;
    const Pack& p = r.packs[0];
    check(p.nodes[1].fighters.size() == 2 && p.nodes[1].fighters_only && p.nodes[1].fighters.at("013").size() == 2, "a node's fighters and fighters_only read");
    check(r.game_textures.size() == 1, "game_textures read");
    const Grid g = grid_of(p, r.game_textures, "Game's");
    check(g.fighters.size() == 2 && g.fighters[0].fid == "001" && g.fighters[1].fid == "013", "two characters, by number");
    check(g.split.size() == 1 && p.nodes[static_cast<size_t>(g.split[0])].id == "nbase", "the box over every fighter is split per fighter");
    check(g.taken.size() == 5 && !g.taken.count(7), "the grid takes the box and both groups, not the box of no fighter");
    if (g.fighters.size() != 2) return;
    const GridFighter& ryu = g.fighters[0];
    const GridFighter& aki = g.fighters[1];
    check(ryu.values.size() == 2 && ryu.values[1].name == "C1 Battlehud Colour (Full)" && ryu.values[1].ids == Ticks{ "nbase@001" },
          "a fighter of the box alone: the game's, the box for it");
    check(ryu.values[0].name == "Game's" && ryu.values[0].picture.empty(), "no game copy of its texture: no picture for the game's");
    check(aki.values.size() == 4, "A.K.I.: the game's, the box, then her two pairs");
    if (aki.values.size() == 4) {
        check(aki.values[2].name == "C1 Alt" && aki.values[3].name == "C3", "P1 + P2 named by what they share, the name every pair has taken off");
        check((aki.values[2].ids == Ticks{ "na1", "nb1" }), "a pair ticks both sides");
        check(aki.values[0].picture == "_uislots/game/product/gui/data/character_image/013/battlehud/tex_battlehud_013_1p_iam.tex",
              "the game's: the game's copy of the main texture");
        check(aki.values[1].picture == "_uislots/u58e2c904cd20/nbase/product/gui/data/character_image/013/battlehud/tex_battlehud_013_1p_iam.tex",
              "the box: its copy of the main texture");
        check(aki.values[3].picture == "_uislots/u58e2c904cd20/na3/product/gui/data/character_image/013/battlehud/tex_battlehud_013_1p_iam.tex",
              "a stray file in the folder: the main texture still shown");
    }
    const std::vector<Row> left = rows_left(rows_of(p, false), g);
    check(left.size() == 1 && left[0].title == "Big round letters", "the rows left: the box of no fighter");

    Ticks t;
    check(grid_set(p, g, "013", 2, t) && (t == Ticks{ "na1", "nb1" }) && grid_value(aki, t) == 2, "A.K.I. set to C1 Alt: both sides ticked");
    check(grid_set(p, g, "013", 3, t) && (t == Ticks{ "na3", "nb3" }), "C3: the other pair off");
    check(grid_value(ryu, t) == 0, "Ryu: the game's");
    t = { "nbase" };
    check(grid_value(ryu, t) == 1 && grid_value(aki, t) == 1, "the box ticked whole: every fighter on it");
    check(grid_set(p, g, "013", 2, t) && (t == Ticks{ "nbase@001", "na1", "nb1" }), "one fighter off the box: the box kept for the other");
    check(grid_value(ryu, t) == 1 && grid_value(aki, t) == 2, "each its own value");
    check(grid_set(p, g, "013", 1, t) && (t == Ticks{ "nbase" }), "every fighter back on the box: the box itself");
    check(!grid_set(p, g, "013", 1, t), "the same value: no change");
    check(grid_set(p, g, "001", 0, t) && (t == Ticks{ "nbase@013" }), "Ryu to the game's: the box for A.K.I. alone");
    check(grid_set(p, g, "013", 0, t) && t.empty(), "everyone the game's: nothing ticked");
    check(!grid_set(p, g, "013", 9, t) && !grid_set(p, g, "999", 0, t), "out of range, unknown fighter: nothing");
    check((sanitize(p, { "nbase@013", "nbase@999", "nx@013", "nbase", "na1@013" }) == Ticks{ "nbase", "na1@013" }),
          "sanitize: a fighter's tick kept when its option has that fighter and is not ticked whole");
    Registry plain;
    parse_registry(kRegistry, plain);
    bool none = true;
    for (const Pack& q : plain.packs) none = none && grid_of(q, plain.game_textures, "x").empty();
    check(none, "packs without fighters: no grid");
}

int main(int argc, char** argv) {
    if (argc > 1) return dump(argv[1]);
    Registry r;
    test_registry(r);
    if (r.packs.size() == 3) {
        test_rows(r);
        test_choices(r);
        test_titles();
        test_split();
    }
    test_grid();
    if (fails) { std::printf("%d FAILED\n", fails); return 1; }
    std::printf("all ok\n");
    return 0;
}
