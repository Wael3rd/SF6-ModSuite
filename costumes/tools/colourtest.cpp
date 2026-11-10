// colourtest: the Color Mod's colours following their outfit (costumes/loader/colour_follow.hpp), without the game.
// Usage: colourtest --selftest            the pure functions: which outfit went where (decide), the moves (make_plan, swaps,
//                                         a taken place, the journal), the file names, the Color Mod's three files
//        colourtest --launch <game dir> <outfits.tsv>   one launch's pass on a folder laid out like the game's: the slots of the
//                                         last launch are <game dir>\reframework\data\SF6_Costumes_Data\colourtest_registry.tsv
//                                         (registry.json's, none at the first), this launch's are <outfits.tsv>, one per line:
//                                         fighter<TAB>number<TAB>mod_id<TAB>based on<TAB>label. Then, as the loader does, the
//                                         registry and outfits.json are written for the next launch.
// Built by build.bat (loader step) into build\colourtest.exe.
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../loader/colour_follow.hpp"

using namespace colour_follow;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    printf("  %-96s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fails;
}

Outfit O(int f, int n, const char* id, int c, const char* name) { return Outfit{ f, n, id, c, name }; }

const Step* find_step(const std::vector<Step>& v, Why w, int f, int from, int to) {
    for (auto& s : v) if (s.why == w && s.fighter == f && s.from == from && s.to == to) return &s;
    return nullptr;
}

// ---- names ------------------------------------------------------------------------------------------------------------
void test_names() {
    printf("file names and paths\n");
    int f = 0, n = 0;
    std::string rest;
    check(parse_manual_name("esf001_006_cmd_003.user.2", f, n) && f == 1 && n == 5, "MC file: esf001_006 = fighter 1, number 5");
    check(parse_manual_name("ESF001_006_CMD_003.USER.2", f, n) && n == 5, "MC file: letters of any case");
    check(!parse_manual_name("esf001_000_cmd_003.user.2", f, n) && !parse_manual_name("esf001_06_cmd_003.user.2", f, n)
          && !parse_manual_name(".colfollow_esf001_006_cmd_003.user.2", f, n), "MC file: folder 000, short, temporary name refused");
    check(renumber_manual_name("esf001_006_cmd_003.user.2", 9) == "esf001_010_cmd_003.user.2", "MC file renumbered 5 -> 9: esf001_010");
    check(parse_data_name("1_5_game_101.user.2", f, n, rest) && f == 1 && n == 5 && rest == "game_101", "data file: 1_5_game_101");
    check(!parse_data_name("_5_game.user.2", f, n, rest) && !parse_data_name("1_5_.user.2", f, n, rest)
          && !parse_data_name(".colfollow_1_5_game_1.user.2", f, n, rest), "data file: malformed names refused");
    const std::string mc_edit = "1_5_file____color_mods_Ryu_MANUALCOLORS_01_esf001_006_cmd_003_user_2.user.2";
    const std::string rw_edit = "1_5_file____color_mods_Ryu_Pack_natives_stm_esf001_006_cmd_003_user_2.user.2";
    const std::string cache_edit = "1_5_file____color_mods__cache_MANUALCOLORS_pack_esf001_006_cmd_003_user_2.user.2";
    parse_data_name(mc_edit, f, n, rest);
    check(override_follows(rest), "override of an MC colour follows the outfit");
    parse_data_name(rw_edit, f, n, rest);
    check(!override_follows(rest), "override of a colour pack's colour (RW) stays with its number");
    parse_data_name(cache_edit, f, n, rest);
    check(!override_follows(rest), "override of an archive's colour (.cache) stays");
    check(override_follows("game_3"), "edited game colour follows");
    check(renumber_data_name(mc_edit, 9) == "1_9_file____color_mods_Ryu_MANUALCOLORS_01_esf001_010_cmd_003_user_2.user.2",
          "MC override renumbered: prefix and its colour's file name");
    check(renumber_data_name("1_5_game_101.user.2", 12) == "1_12_game_101.user.2", "game colour copy renumbered 5 -> 12");
    const std::string p = "..\\color_mods\\Ryu\\MANUALCOLORS_01\\esf001_006_cmd_003.user.2";
    check(parse_manual_path(p, f, n) && f == 1 && n == 5, "MC path of state.json / chip_slots.json");
    check(parse_manual_path("../color_mods/Ryu/MANUALCOLORS_01/esf001_006_cmd_003.user.2", f, n), "MC path with forward slashes");
    check(!parse_manual_path("..\\color_mods\\Ryu\\Pack\\esf001_006_cmd_003.user.2", f, n), "a pack's path (RW): not MC");
    check(!parse_manual_path("..\\color_mods\\.cache\\MANUALCOLORS_x_1234abcd\\esf001_006_cmd_003.user.2", f, n), "an archive's .cache: not MC");
    check(!parse_manual_path("..\\color_mods\\esf001_006_cmd_003.user.2", f, n), "no folder: not MC");
    check(renumber_manual_path(p, 9) == "..\\color_mods\\Ryu\\MANUALCOLORS_01\\esf001_010_cmd_003.user.2", "MC path renumbered");
    const Outfit a = O(1, 5, "0123456789abcdef", 0, "Jin");
    check(identity_key(a) == "0123456789abcdef_f001_c00", "identity key: mod id, fighter, game costume");
    check(is_identity_key(identity_key(a)) && is_identity_key("0123456789abcdef_f001_c00_n7"), "identity keys recognised");
    check(!is_identity_key("20261110_1432") && !is_identity_key("_f001_c00") && !is_identity_key("abc_f01_c00"),
          "a date folder (1.27.1) is no identity");
    check(normal_name("  Tekken 8   JIN ") == "tekken 8 jin", "names: case and spaces left out");
}

// ---- decide -----------------------------------------------------------------------------------------------------------
void test_decide() {
    printf("which outfit went where\n");
    const std::string T = "20261110_1200";
    auto s = decide({ O(1, 5, "aaaa", 0, "Jin") }, { O(1, 5, "aaaa", 0, "Jin") }, {}, T);
    check(s.empty(), "same outfit, same number: nothing");
    s = decide({ O(1, 5, "aaaa", 0, "Jin") }, { O(1, 7, "aaaa", 0, "Jin") }, {}, T);
    check(s.size() == 2 && find_step(s, Why::Same, 1, 5, 7) && find_step(s, Why::Unknown, 1, 7, -1),
          "renumbered: 5 -> 7 (and 7, known to nobody, cleared first)");
    s = decide({ O(1, 5, "aaaa", 0, "A"), O(1, 6, "bbbb", 0, "B") }, { O(1, 6, "aaaa", 0, "A"), O(1, 5, "bbbb", 0, "B") }, {}, T);
    check(s.size() == 2 && find_step(s, Why::Same, 1, 5, 6) && find_step(s, Why::Same, 1, 6, 5), "swap: 5 -> 6 and 6 -> 5");
    s = decide({ O(1, 5, "aaaa", 0, "Tekken 8 Jin") }, { O(1, 7, "cccc", 0, "tekken  8 jin ") }, {}, T);
    check(find_step(s, Why::Updated, 1, 5, 7) && s.size() == 2, "updated mod (new id, same fighter and name): follows 5 -> 7");
    s = decide({ O(1, 5, "aaaa", 0, "Jin") }, { O(1, 5, "cccc", 0, "Jin") }, {}, T);
    check(s.empty(), "updated mod given the same number: its colours stay (not set aside)");
    s = decide({ O(1, 5, "aaaa", 0, "Jin") }, { O(2, 7, "cccc", 0, "Jin") }, {}, T);
    check(find_step(s, Why::Gone, 1, 5, -1) && !find_step(s, Why::Updated, 2, 5, 7), "same name, another fighter: not the same outfit");
    s = decide({ O(1, 5, "aaaa", 0, "Pack"), O(1, 6, "aaaa", 1, "Pack") }, { O(1, 7, "cccc", 0, "Pack"), O(1, 8, "cccc", 1, "Pack") }, {}, T);
    check(find_step(s, Why::Updated, 1, 5, 7) && find_step(s, Why::Updated, 1, 6, 8), "a mod's two outfits updated: each by its game costume");
    s = decide({ O(1, 5, "aaaa", 0, "Pack"), O(1, 6, "bbbb", 1, "Pack") }, { O(1, 7, "cccc", 2, "Pack"), O(1, 8, "dddd", 3, "Pack") }, {}, T);
    check(find_step(s, Why::Gone, 1, 5, -1) && find_step(s, Why::Gone, 1, 6, -1) && find_step(s, Why::Unknown, 1, 7, -1)
          && find_step(s, Why::Unknown, 1, 8, -1) && s.size() == 4, "two outfits of one name went, two came: ambiguous, nothing follows");
    s = decide({ O(1, 5, "aaaa", 0, "Jin") }, { O(1, 5, "eeee", 0, "Vagrant") }, {}, T);
    check(s.size() == 1 && find_step(s, Why::Gone, 1, 5, -1) && s[0].key == "aaaa_f001_c00",
          "number reused by another mod: the former outfit's colours set aside under its identity");
    s = decide({}, { O(1, 9, "aaaa", 0, "Jin") }, { "aaaa_f001_c00" }, T);
    check(find_step(s, Why::Back, 1, -1, 9) && find_step(s, Why::Unknown, 1, 9, -1), "installed again: put back at 9 (9 cleared first)");
    s = decide({ O(1, 7, "bbbb", 0, "Jin") }, { O(1, 9, "aaaa", 0, "Jin") }, { "aaaa_f001_c00" }, T);
    check(find_step(s, Why::Back, 1, -1, 9) && find_step(s, Why::Gone, 1, 7, -1) && !find_step(s, Why::Updated, 1, 7, 9),
          "its own set-aside colours come before a name's");
    s = decide({ O(1, 5, "aaaa", 0, "A"), O(2, 5, "aaaa", 0, "A") }, { O(1, 5, "aaaa", 0, "A"), O(2, 6, "aaaa", 0, "A") }, {}, T);
    check(find_step(s, Why::Same, 2, 5, 6) && !find_step(s, Why::Same, 1, 5, 5), "one mod, two fighters: each its own");
    s = decide({ O(1, 5, "aaaa", 0, "A"), O(1, 6, "aaaa", 0, "A") }, { O(1, 5, "aaaa", 0, "A") }, {}, T);
    check(s.size() == 1 && s[0].why == Why::Gone && s[0].key == "aaaa_f001_c00_n6", "an identity two outfits had: kept apart (_n6)");
    s = decide({ O(1, 5, "aaaa", 0, "A") }, { O(1, 5, "aaaa", 0, "A"), O(1, 8, "aaaa", 0, "A") }, { "aaaa_f001_c00" }, T);
    check(s.size() == 1 && find_step(s, Why::Unknown, 1, 8, -1), "two outfits of one identity now: the second gets nothing back");
    s = decide({ O(1, 5, "aaaa", 0, "") }, { O(1, 7, "cccc", 0, "") }, {}, T);
    check(find_step(s, Why::Gone, 1, 5, -1) && !find_step(s, Why::Updated, 1, 5, 7), "no name (a Fluffy pak): never by name");
}

// ---- a pretend file system to run plans on ------------------------------------------------------------------------------
struct Vfs {
    std::map<std::string, std::string> files;   // lower case path -> content
    std::map<std::string, std::string> names;   // lower case path -> path as written
    std::set<std::string> locked;               // lower case paths that cannot move
    static std::string low(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c + 32); return s; }
    bool has(const std::string& p) const { return files.count(low(p)) != 0; }
    void put(const std::string& p, const std::string& c) { files[low(p)] = c; names[low(p)] = p; }
    std::string get(const std::string& p) const { auto it = files.find(low(p)); return it == files.end() ? "<none>" : it->second; }
    bool mv(const std::string& a, const std::string& b, bool replace) {
        if (!has(a) || locked.count(low(a)) || (!replace && has(b))) return false;
        const std::string c = get(a);
        files.erase(low(a)); names.erase(low(a));
        put(b, c);
        return true;
    }
    void op(const Op& o) {   // colour_follow.cpp do_op's rules
        if (!has(o.src)) return;
        if (o.replace) { mv(o.src, o.dst, true); return; }
        if (!has(o.dst)) { if (mv(o.src, o.dst, false)) return; if (!has(o.dst)) return; }
        if (o.fallback.empty()) return;
        std::string fb = o.fallback;
        const size_t ext = fb.size() - 7;
        for (int k = 2; has(fb); ++k) fb = o.fallback.substr(0, ext) + " (" + std::to_string(k) + ")" + o.fallback.substr(ext);
        mv(o.src, fb, false);
    }
    void run(const Plan& p) {
        for (auto& w : p.writes) put(w.first + ".colfollow_new", w.second);
        for (auto& o : p.first) op(o);
        for (auto& o : p.second) op(o);
    }
};

const Place kPlace{ "G\\reframework\\color_mods", "G\\reframework\\data\\SF6_ColorSpinExtra_data" };
ColourFile mc(int f, int n, int k, const std::string& key = "") {
    char b[64];
    snprintf(b, sizeof b, "esf%03d_%03d_cmd_%03d.user.2", f, n + 1, k);
    return { ColourFile::Manual, kPlace.color_mods + "\\Ryu", "MANUALCOLORS_01", b, key, f, n };
}
ColourFile data(ColourFile::Kind kind, int f, int n, const std::string& rest, const std::string& key = "") {
    return { kind, kPlace.data + (kind == ColourFile::Override ? "\\overrides" : "\\pristine"), "",
             std::to_string(f) + "_" + std::to_string(n) + "_" + rest + ".user.2", key, f, n };
}
void add(Vfs& v, Listing& l, const ColourFile& f, const std::string& content) {
    l.files.push_back(f);
    v.put(f.key.empty() ? live_path(f) : aside_path(f, f.key), content);
}

void test_plan() {
    printf("the moves\n");
    const std::string T = "20261110_1200";
    auto exists_in = [](const Vfs& v) { return [&v](const std::string& p) { return v.has(p); }; };
    {   // swap of two outfits, with MC colours, an MC override, a game override, a captured colour, a pack's override
        Vfs v; Listing l;
        add(v, l, mc(1, 5, 1), "A-mc1"); add(v, l, mc(1, 5, 2), "A-mc2"); add(v, l, mc(1, 6, 1), "B-mc1");
        add(v, l, data(ColourFile::Override, 1, 5, "game_3"), "A-game3");
        add(v, l, data(ColourFile::Override, 1, 6, "file____color_mods_Ryu_MANUALCOLORS_01_esf001_007_cmd_001_user_2"), "B-mc1-edit");
        add(v, l, data(ColourFile::Pristine, 1, 6, "game_0"), "B-pristine0");
        v.put(kPlace.data + "\\overrides\\1_5_file____color_mods_Ryu_Pack_esf001_006_cmd_001_user_2.user.2", "pack-edit");   // not listed: stays
        auto steps = decide({ O(1, 5, "aaaa", 0, "A"), O(1, 6, "bbbb", 0, "B") }, { O(1, 6, "aaaa", 0, "A"), O(1, 5, "bbbb", 0, "B") }, {}, T);
        Plan p = make_plan(steps, l, kPlace, exists_in(v));
        bool temps = !p.first.empty();
        for (auto& o : p.first) temps = temps && o.dst.find("\\.colfollow_") != std::string::npos;
        check(temps && p.files == 6, "every file leaves its name first, for a temporary name beside it");
        v.run(p);
        const std::string M = kPlace.color_mods + "\\Ryu\\MANUALCOLORS_01\\";
        check(v.get(M + "esf001_007_cmd_001.user.2") == "A-mc1" && v.get(M + "esf001_007_cmd_002.user.2") == "A-mc2"
              && v.get(M + "esf001_006_cmd_001.user.2") == "B-mc1" && !v.has(M + "esf001_006_cmd_002.user.2"), "swap: MC colours exchanged");
        check(v.get(kPlace.data + "\\overrides\\1_6_game_3.user.2") == "A-game3"
              && v.get(kPlace.data + "\\overrides\\1_5_file____color_mods_Ryu_MANUALCOLORS_01_esf001_006_cmd_001_user_2.user.2") == "B-mc1-edit"
              && v.get(kPlace.data + "\\pristine\\1_5_game_0.user.2") == "B-pristine0", "swap: overrides and captured colours follow");
        check(v.get(kPlace.data + "\\overrides\\1_5_file____color_mods_Ryu_Pack_esf001_006_cmd_001_user_2.user.2") == "pack-edit",
              "a colour pack's edited copy stays with its number");
        bool no_temp = true;
        for (auto& [k, c] : v.files) if (k.find(".colfollow") != std::string::npos) no_temp = false;
        check(no_temp, "no temporary name left");
        // the journal: written whole, read back the same; cut short after phase 1 and finished: the same files
        Plan back;
        int phase = 0;
        check(parse_journal(journal_text(p, 1), back, phase) && phase == 1 && back.first.size() == p.first.size()
              && back.second.size() == p.second.size() && back.second.back().replace == p.second.back().replace, "journal read back");
        check(!parse_journal(journal_text(p, 1).substr(0, journal_text(p, 1).size() - 4), back, phase), "a journal cut short is refused");
    }
    {   // cut short: in phase 2, then phase 2 again (the journal says 2): same as a whole run; a chain 5 -> 6 -> 7
        auto make = [&](Vfs& v, Listing& l) {
            add(v, l, mc(1, 5, 1), "A"); add(v, l, mc(1, 6, 1), "B"); add(v, l, mc(1, 7, 1), "C");
            return decide({ O(1, 5, "aaaa", 0, "A"), O(1, 6, "bbbb", 0, "B"), O(1, 7, "cccc", 0, "C") },
                          { O(1, 6, "aaaa", 0, "A"), O(1, 7, "bbbb", 0, "B"), O(1, 8, "cccc", 0, "C") }, {}, T);
        };
        Vfs whole, cut; Listing l1, l2;
        auto s1 = make(whole, l1); auto s2 = make(cut, l2);
        Plan p1 = make_plan(s1, l1, kPlace, exists_in(whole)), p2 = make_plan(s2, l2, kPlace, exists_in(cut));
        whole.run(p1);
        for (auto& o : p2.first) cut.op(o);
        for (size_t i = 0; i < p2.second.size() / 2; ++i) cut.op(p2.second[i]);
        for (auto& o : p2.second) cut.op(o);   // the next launch: phase 2 again
        check(whole.files == cut.files, "a pass cut short in phase 2 and finished: the same files");
        const std::string M = kPlace.color_mods + "\\Ryu\\MANUALCOLORS_01\\";
        check(whole.get(M + "esf001_007_cmd_001.user.2") == "A" && whole.get(M + "esf001_008_cmd_001.user.2") == "B"
              && whole.get(M + "esf001_009_cmd_001.user.2") == "C" && !whole.has(M + "esf001_006_cmd_001.user.2"), "chain 5 -> 6 -> 7 -> 8");
    }
    {   // a place taken when the move is made (a file that could not move): never overwritten, set aside for the outfit
        Vfs v; Listing l;
        add(v, l, mc(1, 5, 1), "A"); add(v, l, mc(1, 6, 1), "B");
        auto steps = decide({ O(1, 5, "aaaa", 0, "A"), O(1, 6, "bbbb", 0, "B") }, { O(1, 6, "aaaa", 0, "A") }, {}, T);
        Plan p = make_plan(steps, l, kPlace, exists_in(v));
        v.locked.insert(Vfs::low(live_path(mc(1, 6, 1))));
        v.run(p);
        check(v.get(live_path(mc(1, 6, 1))) == "B" && v.get(aside_path(mc(1, 6, 1), "aaaa_f001_c00")) == "A",
              "taken: the arriving colour goes to its outfit's .former_outfits, the file there stays");
    }
    {   // gone, then installed again at another number: back, renamed
        Vfs v; Listing l;
        add(v, l, mc(1, 5, 1), "A"); add(v, l, data(ColourFile::Pristine, 1, 5, "game_2"), "A-p2");
        v.put(aside_path(mc(1, 5, 1), "aaaa_f001_c00"), "older");   // set aside before: kept, the new one beside it
        Plan p = make_plan(decide({ O(1, 5, "aaaa", 0, "A") }, {}, {}, T), l, kPlace, exists_in(v));
        v.run(p);
        const std::string first = aside_path(mc(1, 5, 1), "aaaa_f001_c00");
        const std::string second = first.substr(0, first.size() - 7) + " (2).user.2";
        check(v.get(first) == "older" && v.get(second) == "A" && !v.has(live_path(mc(1, 5, 1))),
              "gone: set aside under its identity, never over a file there");
        Listing l2;
        l2.files.push_back(mc(1, 5, 1, "aaaa_f001_c00"));
        l2.files.push_back(data(ColourFile::Pristine, 1, 5, "game_2", "aaaa_f001_c00"));
        v.put(live_path(mc(1, 9, 1)), "stale");   // at the new number: of no known outfit
        l2.files.push_back(mc(1, 9, 1));
        auto steps = decide({}, { O(1, 9, "aaaa", 0, "A") }, aside_keys(l2), T);
        Plan p2 = make_plan(steps, l2, kPlace, exists_in(v));
        v.run(p2);
        check(v.get(live_path(mc(1, 9, 1))) == "older" && v.get(kPlace.data + "\\pristine\\1_9_game_2.user.2") == "A-p2",
              "back at 9: renamed to its new number");
        check(v.get(aside_path(mc(1, 9, 1), T)) == "stale", "what was at 9 (no known outfit) set aside by date");
    }
}

// ---- the Color Mod's files ----------------------------------------------------------------------------------------------
void test_files() {
    printf("the Color Mod's files\n");
    const std::string state =
        "{\r\n    \"fs\": {\r\n        \"costume\": 9,\r\n        \"fighter\": 1,\r\n        \"rw\": 0\r\n    },\r\n"
        "    \"p1\": {\r\n        \"costume\": 5,\r\n        \"fighter\": 1,\r\n"
        "        \"path\": \"..\\\\color_mods\\\\Ryu\\\\MANUALCOLORS_01\\\\esf001_006_cmd_002.user.2\",\r\n        \"rw\": 3\r\n    },\r\n"
        "    \"p2\": {\r\n        \"costume\": 5,\r\n        \"fighter\": 1,\r\n"
        "        \"path\": \"..\\\\color_mods\\\\Ryu\\\\Pack\\\\esf001_006_cmd_002.user.2\",\r\n        \"rw\": 1\r\n    }\r\n}";
    JVal j;
    std::string tail;
    check(json_parse(state, j, &tail) && json_dump(j, "\r\n") + tail == state, "state.json read and written byte for byte");
    Renumber r;
    r.move[{ 1, 5 }] = { 7, "aaaa_f001_c00" };
    Rewrite w = rewrite_state(state, r);
    check(w.changed == 1 && w.text.find("\"costume\": 7,\r\n        \"fighter\": 1,\r\n        \"path\": \"..\\\\color_mods\\\\Ryu\\\\MANUALCOLORS_01\\\\esf001_008_cmd_002.user.2\"")
          != std::string::npos, "state.json: a choice of the outfit's MC colour follows it (costume, path)");
    check(w.text.find("Pack\\\\esf001_006_cmd_002") != std::string::npos && w.text.find("\"costume\": 9") != std::string::npos,
          "state.json: a pack's colour (RW) and another outfit's choice unchanged");
    const std::string pend = "{\"p1\":{\"costume\":5,\"fighter\":1,\"rw\":0,\"path_pending\":{\"costume\":5,\"fighter\":1,"
                             "\"path\":\"..\\\\color_mods\\\\Ryu\\\\MANUALCOLORS_01\\\\esf001_006_cmd_002.user.2\"}}}";
    w = rewrite_state(pend, r);
    check(w.changed == 1 && w.text.find("{\"costume\":7,\"fighter\":1,\"rw\":0,\"path_pending\":{\"costume\":7,") == 6
          && w.text.find("esf001_008_cmd_002") != std::string::npos, "state.json: a parked choice follows too, with its choice");

    const std::string chips =
        "{\r\n    \"..\\\\color_mods\\\\Ryu\\\\MANUALCOLORS_01\\\\esf001_006_cmd_001.user.2\": [\r\n        1,\r\n        2\r\n    ],\r\n"
        "    \"..\\\\color_mods\\\\Ryu\\\\MANUALCOLORS_01\\\\esf001_007_cmd_001.user.2\": [\r\n        3,\r\n        4\r\n    ],\r\n"
        "    \"1_5\": [\r\n        \"esf_A/1\",\r\n        \"esf_B/1\"\r\n    ],\r\n    \"1_6\": [\r\n        \"esf_C/1\",\r\n        \"esf_D/1\"\r\n    ],\r\n"
        "    \"2_5\": [\r\n        \"esf_E/1\",\r\n        \"esf_F/1\"\r\n    ]\r\n}";
    check(json_parse(chips, j, &tail) && json_dump(j, "\r\n") + tail == chips, "chip_slots.json read and written byte for byte");
    Renumber sw;
    sw.move[{ 1, 5 }] = { 6, "aaaa_f001_c00" };
    sw.move[{ 1, 6 }] = { 5, "bbbb_f001_c00" };
    w = rewrite_chips(chips, sw, {});
    check(w.changed == 4 && w.text.find("\"1_5\": [\r\n        \"esf_C/1\"") != std::string::npos
          && w.text.find("\"1_6\": [\r\n        \"esf_A/1\"") != std::string::npos
          && w.text.find("esf001_007_cmd_001.user.2\": [\r\n        1,") != std::string::npos
          && w.text.find("esf001_006_cmd_001.user.2\": [\r\n        3,") != std::string::npos
          && w.text.find("\"2_5\": [\r\n        \"esf_E/1\"") != std::string::npos, "chip_slots.json: a swap exchanges the squares, Ken's stay");
    Renumber out;
    out.out[{ 1, 5 }] = "aaaa_f001_c00";
    w = rewrite_chips(chips, out, {});
    check(w.changed == 2 && w.text.find("\"1_5\"") == std::string::npos && w.text.find("esf001_006") == std::string::npos
          && w.add["aaaa_f001_c00"].chips.size() == 2, "chip_slots.json: set aside, the outfit's entries taken out into its record");
    Record rec = w.add["aaaa_f001_c00"];
    check(parse_record(record_text(rec)).chips.size() == 2 && parse_record(record_text(rec)).chips[1].value == rec.chips[1].value,
          "record read back");
    Renumber bk;
    bk.back[{ 1, 9 }] = "aaaa_f001_c00";
    Rewrite w2 = rewrite_chips(w.text, bk, { { "aaaa_f001_c00", rec } });
    check(w2.changed == 2 && w2.text.find("\"1_9\": [\r\n        \"esf_A/1\"") != std::string::npos
          && w2.text.find("esf001_010_cmd_001.user.2\": [\r\n        1,") != std::string::npos && w2.left.empty(),
          "chip_slots.json: back at 9, put in again renamed");
    Renumber taken;
    taken.move[{ 1, 5 }] = { 6, "aaaa_f001_c00" };
    w = rewrite_chips(chips, taken, {});
    check(w.text.find("\"1_6\": [\r\n        \"esf_C/1\"") != std::string::npos && w.add["aaaa_f001_c00"].chips.size() == 2
          && w.add["aaaa_f001_c00"].chips[1].key == "\"1_6\"" && w.text.find("esf001_007_cmd_001.user.2\": [\r\n        3,") != std::string::npos,
          "chip_slots.json: a place taken keeps its entry, the arriving ones kept aside");

    const std::string ticks = "SF6 Color Mod opponent colours 1\n"
                              "1\t5\t..\\color_mods\\Ryu\\MANUALCOLORS_01\\esf001_006_cmd_001.user.2\n"
                              "1\t5\t..\\color_mods\\Ryu\\Pack\\esf001_006_cmd_004.user.2\n"
                              "9\t0\t..\\color_mods\\Cammy\\MANUALCOLORS_01\\esf009_001_cmd_001.user.2\n";
    w = rewrite_ticks(ticks, r, {});
    check(w.changed == 1 && w.text == "SF6 Color Mod opponent colours 1\n1\t7\t..\\color_mods\\Ryu\\MANUALCOLORS_01\\esf001_008_cmd_001.user.2\n"
                                      "1\t5\t..\\color_mods\\Ryu\\Pack\\esf001_006_cmd_004.user.2\n9\t0\t..\\color_mods\\Cammy\\MANUALCOLORS_01\\esf009_001_cmd_001.user.2\n",
          "opponent_colours.txt: an MC colour's tick follows, a pack's stays");
    w = rewrite_ticks(ticks, out, {});
    check(w.changed == 1 && w.add["aaaa_f001_c00"].ticks.size() == 1 && w.text.find("MANUALCOLORS_01\\esf001_006") == std::string::npos,
          "opponent_colours.txt: set aside with the outfit");
    w2 = rewrite_ticks(w.text, bk, { { "aaaa_f001_c00", w.add["aaaa_f001_c00"] } });
    check(w2.changed == 1 && w2.text.find("1\t9\t..\\color_mods\\Ryu\\MANUALCOLORS_01\\esf001_010_cmd_001.user.2\n") != std::string::npos,
          "opponent_colours.txt: back at 9");
    check(rewrite_ticks("another format\n1\t5\tx\n", r, {}).text.empty(), "opponent_colours.txt of another format: untouched");
    check(rewrite_chips("{ not json", r, {}).text.empty() && rewrite_state("[1,", r).text.empty(), "unreadable JSON: untouched");
}

// ---- owners -----------------------------------------------------------------------------------------------------------
void test_owners() {
    printf("who the colours belong to\n");
    const std::vector<Outfit> now{ O(1, 5, "aaaa", 0, "Jin"), O(1, 6, "bbbb", 1, "Tab\there") };
    Owners own;
    check(parse_owners(owners_text(now, { "d1", "d2" }), own) && own.outfits.size() == 2 && own.outfits[1].name == "Tab here"
          && own.registries.count("d2"), "colour_owners.txt read back");
    std::vector<Outfit> reg{ O(1, 5, "aaaa", 0, ""), O(1, 6, "bbbb", 1, "") };
    check(registry_digest(reg) == registry_digest({ reg[1], reg[0] }) && registry_digest(reg) != registry_digest({ reg[0] }),
          "registry digest: order left out, slots counted");
    std::string src;
    Owners o2;
    parse_owners(owners_text({ O(1, 7, "aaaa", 0, "Jin") }, { registry_digest(reg) }), o2);
    auto f = choose_former(o2, reg, {}, src);
    check(src == "colour_owners.txt" && f.size() == 1 && f[0].number == 7, "registry it goes with: colour_owners.txt trusted");
    f = choose_former(o2, {}, {}, src);
    check(src == "colour_owners.txt" && f.size() == 1, "no registry (no mods at the last launch, SlotsClean): trusted");
    f = choose_former(o2, { O(1, 9, "zzzz", 0, "") }, { { { 1, 9 }, "Other" } }, src);
    check(src.find("another loader") != std::string::npos && f.size() == 1 && f[0].name == "Other",
          "a registry it does not know (another loader version): registry and outfits.json labels");
    f = choose_former(Owners(), reg, { { { 1, 5 }, "Jin" } }, src);
    check(src.find("first launch") != std::string::npos && f.size() == 2 && f[0].name == "Jin" && f[1].name.empty(),
          "first launch of this loader: the registry's numbers are the truth");
    f = choose_former(Owners(), {}, {}, src);
    check(src.empty() && f.empty(), "first install: nothing known");
    const std::string oj = "{\n  \"pak\": \"x\",\n  \"outfits\": [\n    {\"fighter\": 1, \"fighter_dir\": \"esf001\", \"costume_no\": 9, "
                           "\"name\": \"Outfit I\", \"based_on\": 0, \"label\": \"Tekken 8 \\\"Jin\\\"\", \"kind\": \"archive\", \"source\": \"Ryu/a.zip\"},\n"
                           "    {\"fighter\": 1, \"fighter_dir\": \"esf001\", \"costume_no\": 10, \"name\": \"Outfit II\", \"based_on\": 0, "
                           "\"label\": \"re_chunk_000.pak.patch_002.pak\", \"kind\": \"fluffy pak\", \"source\": \"x\"}\n  ],\n  \"left_out\": []\n}\n";
    auto lab = outfit_labels(oj);
    check(lab.size() == 2 && lab[{ 1, 9 }] == "Tekken 8 \"Jin\"" && lab[{ 1, 10 }].empty(), "outfits.json labels (a Fluffy pak's: none)");
}

// ---- --launch -----------------------------------------------------------------------------------------------------------
std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::vector<Outfit> read_tsv(const std::string& text) {
    std::vector<Outfit> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> p;
        size_t a = 0;
        for (;;) { size_t b = line.find('\t', a); p.push_back(line.substr(a, b == std::string::npos ? b : b - a)); if (b == std::string::npos) break; a = b + 1; }
        if (p.size() < 4) continue;
        out.push_back({ atoi(p[0].c_str()), atoi(p[1].c_str()), p[2], atoi(p[3].c_str()), p.size() > 4 ? p[4] : "" });
    }
    return out;
}

int launch(const std::string& game, const std::string& tsv) {
    const std::string reg = game + "\\reframework\\data\\SF6_Costumes_Data";
    RunInput in;
    in.game_dir = game;
    in.registry_dir = reg;
    in.registry = read_tsv(slurp(reg + "\\colourtest_registry.tsv"));
    for (auto& o : in.registry) o.name.clear();   // registry.json has no names
    in.now = read_tsv(slurp(tsv));
    in.log = [](const std::string& s) { printf("%s", s.c_str()); };
    run(in);
    // as the loader goes on: the registry, then outfits.json
    std::ofstream r(reg + "\\colourtest_registry.tsv", std::ios::binary);
    std::ofstream j(reg + "\\outfits.json", std::ios::binary);
    j << "{\n  \"outfits\": [";
    for (size_t i = 0; i < in.now.size(); ++i) {
        auto& o = in.now[i];
        r << o.fighter << "\t" << o.number << "\t" << o.mod_id << "\t" << o.based_on << "\n";
        j << (i ? "," : "") << "\n    {\"fighter\": " << o.fighter << ", \"costume_no\": " << o.number << ", \"based_on\": " << o.based_on
          << ", \"label\": " << json_quote(o.name) << ", \"kind\": \"folder\"}";
    }
    j << "\n  ]\n}\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0) {
        test_names();
        test_decide();
        test_plan();
        test_files();
        test_owners();
        printf(g_fails ? "\nFAIL: %d check(s)\n" : "\nPASS\n", g_fails);
        return g_fails ? 1 : 0;
    }
    if (argc == 4 && strcmp(argv[1], "--launch") == 0) return launch(argv[2], argv[3]);
    fprintf(stderr, "usage: colourtest --selftest | colourtest --launch <game dir> <outfits.tsv>\n");
    return 2;
}
