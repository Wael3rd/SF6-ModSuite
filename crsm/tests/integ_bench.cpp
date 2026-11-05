// integ_bench — offline tests of the P4 + P5 integration: the pure
// decisions that join the DLL's screens (ui/logic, P4) and the Manager page (ui/manager_logic, P5), and the owner rule.
// No game, no user data.
//
//   integ_bench --repo <crsm folder>
//
// Groups: the owner rule (owners.hpp: every layer lua unless asked native, ui only with the five others; a missing
// settings.json: all native without a CRSM Lua script, all lua with one, and the standalone claim follows), the name keys (one
// reader: T / pad Y renames the loadout on the Manager page, names a slot on the Record page, nothing under a dialog;
// G / R / pad X), T on the Manager page through P5's queue and Runner, the asynchronous tickers (exactly one consumer:
// the Lua's frame_poll or the DLL's strip), the refresh after a Community download (the Manager page re-reads the list
// once, or the files layer at once without the page). P6: the standalone claim (standalone.hpp: the setting, the CRSM Lua
// scan, the decision, whole sessions frame by frame) and the settings.json the package ships (package/settings.json).
// Exit code 0 only when every check passed.
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "model/locale.hpp"
#include "model/luajson.hpp"
#include "owners.hpp"
#include "standalone.hpp"
#include "ui/logic.hpp"
#include "ui/manager_logic.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
namespace lg = ui::logic;
namespace mgr = ui::mgr;

namespace {

// ---- reporting --------------------------------------------------------------------------------------------------------------
struct Group { int checks = 0; int fails = 0; };
std::map<std::string, Group> g_groups;
std::vector<std::string> g_order;
std::string g_group;

void begin(const std::string& name) {
    g_group = name;
    if (!g_groups.count(name)) g_order.push_back(name);
}

void check(bool ok, const std::string& what, const std::string& detail = "") {
    Group& g = g_groups[g_group];
    ++g.checks;
    if (ok) return;
    ++g.fails;
    std::printf("  FAIL [%s] %s%s%s\n", g_group.c_str(), what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " | ") + x;
    return s;
}

model::locale::Locale g_loc;
mgr::Tr translator() {
    return [](const std::string& key, const std::vector<std::string>& args) { return g_loc.T(key, args); };
}

// ---- fakes of P5's two ports (only what these tests read) ---------------------------------------------------------------------
struct FakeLoadouts final : mgr::Loadouts {
    std::vector<std::string> calls;
    std::string sel = "Old", chr = "Ryu";
    std::string selected_name() override { return sel; }
    std::string char_name() override { return chr; }
    std::string loaded_name() override { return ""; }
    std::vector<std::string> dropdown() override { return { mgr::kNoFile }; }
    int dropdown_index() override { return 1; }
    void set_dropdown_index(int i) override { calls.push_back("set_dropdown_index(" + std::to_string(i) + ")"); }
    void set_native_action(bool) override {}
    std::string import_selected() override { calls.push_back("import_selected"); return "Loaded: x"; }
    std::string reopen_loaded() override { calls.push_back("reopen_loaded"); return "Loaded: x"; }
    std::string export_selected(std::string*) override { calls.push_back("export_selected"); return "Saved: x"; }
    std::string export_as(const std::string& n, std::string*) override { calls.push_back("export_as(" + n + ")"); return "Saved: x"; }
    std::string erase_selected() override { calls.push_back("erase_selected"); return "Erased: x"; }
    std::string rename_selected(const std::string& n, std::string*) override { calls.push_back("rename_selected(" + n + ")"); return "Renamed: x"; }
    void invalidate() override { calls.push_back("invalidate"); }
    bool ensure() override { calls.push_back("ensure"); return true; }
};

struct FakeScreen final : mgr::Screen {
    std::vector<std::string> calls, tickers;
    std::string input_title, input_initial, input_text = "New";
    void ticker(const std::string& t) override { tickers.push_back(t); calls.push_back("ticker"); }
    void confirm(const std::string&, const std::string& title, std::function<void(bool)> done) override {
        calls.push_back("confirm(" + title + ")");
        done(true);
    }
    void text_input(const std::string& title, const std::string& initial, size_t, std::function<void(bool, const std::string&)> done) override {
        calls.push_back("text_input");
        input_title = title; input_initial = initial;
        done(true, input_text);
    }
    void community_open() override { calls.push_back("community_open"); }
    void community_share() override { calls.push_back("community_share"); }
    void check_slots() override { calls.push_back("check_slots"); }
    void rows_dirty() override { calls.push_back("rows_dirty"); }
    void list_changed() override { calls.push_back("list_changed"); }
    size_t count(const std::string& c) const { return (size_t)std::count(calls.begin(), calls.end(), c); }
};

// ---- the owner rule -------------------------------------------------------------------------------------------------------------
void test_owners() {
    begin("owners: lua by default, ui only with the five others");
    using owners::kCount;
    const std::array<bool, kCount> all{ true, true, true, true, true, true };
    const std::array<bool, kCount> none{};
    {
        const owners::Result r = owners::resolve(none, all);   // a missing / all-lua settings.json
        check(std::none_of(r.owns.begin(), r.owns.end(), [](bool b) { return b; }) && r.notes.empty(), "all lua: nothing native, no note");
    }
    {
        const owners::Result r = owners::resolve(all, all);
        check(std::all_of(r.owns.begin(), r.owns.end(), [](bool b) { return b; }) && r.notes.empty(), "all six native: all the DLL's");
    }
    {
        std::array<bool, kCount> req{};
        req[owners::kUi] = true;
        const owners::Result r = owners::resolve(req, all);
        check(!r.owns[owners::kUi], "ui alone: stays lua");
        check(r.notes.size() == 1 && r.notes[0].find("ui=native without community=native") != std::string::npos, "ui alone: one note",
              join(r.notes));
    }
    for (size_t miss = 0; miss < owners::kUi; ++miss) {       // ui with one of the five left lua
        std::array<bool, kCount> req = all;
        req[miss] = false;
        const owners::Result r = owners::resolve(req, all);
        const std::string name = owners::kNames[miss];
        check(!r.owns[owners::kUi], "ui without " + name + ": stays lua");
        bool named = false;
        for (const auto& n : r.notes) if (n.find("ui=native without " + name + "=native") != std::string::npos) named = true;
        check(named, "ui without " + name + ": the note names it", join(r.notes));   // (without files: replay / reversal / record noted too)
        for (size_t i = 0; i < owners::kUi; ++i) {
            const bool want = req[i] && !(miss == owners::kFiles && owners::needs_files(i));
            check(r.owns[i] == want, "ui without " + name + ": " + owners::kNames[i] + (want ? " native" : " lua"));
        }
    }
    {
        std::array<bool, kCount> req = all;
        req[owners::kUi] = false;
        const owners::Result r = owners::resolve(req, all);
        check(!r.owns[owners::kUi] && r.owns[owners::kFiles] && r.owns[owners::kRecord] && r.notes.empty(), "the five without ui: the P3 layers, the Lua UI");
    }
    {
        std::array<bool, kCount> req{};
        req[owners::kReplay] = true;
        const owners::Result r = owners::resolve(req, all);
        check(!r.owns[owners::kReplay] && r.notes.size() == 1, "replay without files: stays lua, noted");
    }
    {
        std::array<bool, kCount> impl = all;
        impl[owners::kUi] = false;
        const owners::Result r = owners::resolve(all, impl);
        check(!r.owns[owners::kUi] && r.owns[owners::kRecord], "ui not implemented by the build: lua, the others native");
    }
}

// ---- P6: the standalone claim (standalone.hpp) -----------------------------------------------------------------------------------
// One frame of the game as ui.cpp's standalone_tick sees it.
struct Frame { bool training = false; bool menu_up = false; bool lua_called = false; };
struct SimEnd { standalone::Decision d = standalone::Decision::Wait; uint64_t frame = 0; };   // frame 0 = never terminal

// Runs ui.cpp's loop over the frames (frame numbers from 1, as cmn::frame()) until a terminal decision.
SimEnd simulate(const std::vector<Frame>& frames, standalone::Mode mode, bool lua_files, bool ui_owned = true) {
    uint64_t run = 0;
    bool called = false;
    SimEnd end;
    for (size_t i = 0; i < frames.size(); ++i) {
        run = standalone::next_training_run(run, frames[i].training);
        called = called || frames[i].lua_called;
        standalone::Inputs in;
        in.mode = mode;
        in.ui_owned = ui_owned;
        in.lua_files = lua_files;
        in.lua_called = called;
        in.frames = i + 1;
        in.training_run = run;
        in.menu_up = frames[i].menu_up;
        end.d = standalone::decide(in);
        if (standalone::terminal(end.d)) { end.frame = i + 1; return end; }
    }
    end.frame = 0;
    return end;
}

std::vector<Frame> outside(size_t n) { return std::vector<Frame>(n); }
std::vector<Frame> training(size_t n, bool menu_up = false) { return std::vector<Frame>(n, Frame{ true, menu_up, false }); }
std::vector<Frame> cat(std::initializer_list<std::vector<Frame>> parts) {
    std::vector<Frame> v;
    for (const auto& p : parts) v.insert(v.end(), p.begin(), p.end());
    return v;
}

void test_standalone() {
    using standalone::Decision;
    using standalone::Mode;
    const uint64_t G = standalone::kGraceFrames, T = standalone::kTrainingFrames;

    begin("standalone: the setting");
    {
        std::string note;
        check(standalone::parse_mode(std::nullopt, &note) == Mode::Auto && note.empty(), "missing: auto");
        check(standalone::parse_mode(std::string("auto"), &note) == Mode::Auto && note.empty(), "\"auto\": auto");
        check(standalone::parse_mode(std::string("off"), &note) == Mode::Off && note.empty(), "\"off\": off");
        check(standalone::parse_mode(std::string("native"), &note) == Mode::Off && note.find("not auto / off") != std::string::npos,
              "anything else: off, with a note", note);
        note.clear();
        check(standalone::parse_mode(std::string("Auto"), &note) == Mode::Off && !note.empty(), "the value is case-sensitive: off, noted");
    }

    begin("standalone: a CRSM Lua script in reframework/autorun");
    {
        check(standalone::find_lua([](const std::string&) { return false; }).empty(), "none: \"\"");
        for (const char* n : standalone::kLuaEntries) {
            const std::string want = n;
            check(standalone::find_lua([&](const std::string& x) { return x == want; }) == want, "found: " + want);
        }
        check(standalone::find_lua([](const std::string&) { return true; }) == "SF6_RecordingManager.lua", "several: the loader first");
        check(standalone::find_lua([](const std::string& x) { return x == "crsm/CrsmNative.lua" || x == "func/RecordingManagerTab.lua"; }).empty(),
              "modules in sub-folders do not count (REFramework runs the top level only)");
    }

    begin("standalone: the decision (pure)");
    {
        standalone::Inputs in;
        in.ui_owned = true;
        in.frames = 1;
        check(standalone::decide(in) == Decision::Claim, "no Lua, menu closed: claim at once (no grace, no training run)");
        in.menu_up = true;
        check(standalone::decide(in) == Decision::Wait, "the same with the menu up: wait");
        in.menu_up = false;
        in.lua_files = true;
        in.frames = G;
        in.training_run = T - 1;
        check(standalone::decide(in) == Decision::WaitLua, "a CRSM Lua installed, one frame short of the training run: wait for it");
        in.training_run = T;
        in.frames = G - 1;
        check(standalone::decide(in) == Decision::WaitLua, "a CRSM Lua installed, one frame short of the grace: wait for it");
        in.lua_files = false;
        in.frames = G;
        in.lua_called = true;
        check(standalone::decide(in) == Decision::LuaCalled, "a Lua called the DLL: never the DLL's claim");
        in.lua_called = false;
        in.lua_files = true;
        check(standalone::decide(in) == Decision::Foreign, "a CRSM Lua script installed and silent when the claim is due: foreign");
        in.training_run = 0;
        check(standalone::decide(in) == Decision::WaitLua, "the same before the delay: wait for its loader");
        in.lua_called = true;
        check(standalone::decide(in) == Decision::LuaCalled, "a CRSM Lua script installed that calls the DLL: its loader claims");
        in = standalone::Inputs{};
        in.ui_owned = true;
        in.frames = G;
        in.training_run = T;
        in.mode = Mode::Off;
        check(standalone::decide(in) == Decision::Off, "off: never, even when due");
        in.lua_files = true;
        check(standalone::decide(in) == Decision::Off, "off with a CRSM Lua: off (no foreign hand-back: 0.2.0 behaviour)");
        in.mode = Mode::Auto;
        in.lua_files = false;
        in.ui_owned = false;
        check(standalone::decide(in) == Decision::NotNative, "ui not the DLL's (a layer lua, no pm service, faults): nothing");
        in.ui_owned = true;
        in.claimed = true;
        check(standalone::decide(in) == Decision::Claimed, "already claimed (the loader first): nothing more");
        check(!standalone::terminal(Decision::Wait) && !standalone::terminal(Decision::WaitLua), "wait / wait lua: not terminal");
        for (Decision d : { Decision::Off, Decision::NotNative, Decision::Claimed, Decision::LuaCalled, Decision::Claim, Decision::Foreign })
            check(standalone::terminal(d), std::string("terminal: ") + standalone::decision_name(d));
        check(standalone::next_training_run(5, true) == 6 && standalone::next_training_run(5, false) == 0, "training run: +1 in, 0 out");
    }

    begin("standalone: the menu read only when it matters (decide_lazy)");
    {
        // Every combination of the inputs: decide_lazy gives decide's answer with the real menu, and reads the menu (a game
        // read in the DLL) only when the claim or the foreign verdict would be due with the menu closed.
        int combos = 0, reads_total = 0, mismatches = 0, needless = 0;
        for (int mode = 0; mode < 2; ++mode)
        for (int owned = 0; owned < 2; ++owned)
        for (int claimed = 0; claimed < 2; ++claimed)
        for (int files = 0; files < 2; ++files)
        for (int called = 0; called < 2; ++called)
        for (uint64_t frames : { G - 1, G, G + 5000 })
        for (uint64_t run : { (uint64_t)0, T - 1, T, T + 100 })
        for (int menu = 0; menu < 2; ++menu) {
            standalone::Inputs in;
            in.mode = mode ? Mode::Off : Mode::Auto;
            in.ui_owned = owned != 0;
            in.claimed = claimed != 0;
            in.lua_files = files != 0;
            in.lua_called = called != 0;
            in.frames = frames;
            in.training_run = run;
            in.menu_up = menu != 0;
            int reads = 0;
            const Decision lazy = standalone::decide_lazy(in, [&] { ++reads; return menu != 0; });
            ++combos;
            reads_total += reads;
            if (lazy != standalone::decide(in)) ++mismatches;
            standalone::Inputs closed = in;
            closed.menu_up = false;
            const Decision c = standalone::decide(closed);
            if (reads && c != Decision::Claim && c != Decision::Foreign) ++needless;
            if (!reads && (c == Decision::Claim || c == Decision::Foreign)) ++needless;
        }
        check(mismatches == 0, "the same decision as decide() with the real menu, every combination", std::to_string(mismatches) + " of " + std::to_string(combos));
        check(needless == 0, "the menu read exactly when the closed-menu answer is claim / foreign", std::to_string(needless));
        check(reads_total > 0 && reads_total < combos, "some combinations read the menu, most do not", std::to_string(reads_total));
        // A CRSM Lua installed, waiting in training before the deadline (2 s): no menu read on any frame.
        standalone::Inputs w;
        w.ui_owned = true;
        w.lua_files = true;
        w.frames = G;
        int reads = 0;
        for (uint64_t r = 0; r < T; ++r) {
            w.training_run = r;
            if (standalone::decide_lazy(w, [&] { ++reads; return false; }) != Decision::WaitLua) { reads = -1000; break; }
        }
        check(reads == 0, "a CRSM Lua installed, the training run before the deadline: wait, no menu read", std::to_string(reads));
    }

    begin("standalone: sessions frame by frame");
    {
        // The usual start without a CRSM Lua: claimed at the first frame (title screen), long before any training.
        SimEnd e = simulate(cat({ outside(2000), training(400) }), Mode::Auto, false);
        check(e.d == Decision::Claim && e.frame == 1, "no CRSM Lua: claimed at the first frame", std::to_string(e.frame));
        e = simulate(outside(100000), Mode::Auto, false);
        check(e.d == Decision::Claim && e.frame == 1, "no CRSM Lua, never in training: claimed all the same");
        // A menu up at the first frames (an option window): the claim waits for it to close (never inside a menu).
        e = simulate(cat({ training(50, true), training(10) }), Mode::Auto, false);
        check(e.d == Decision::Claim && e.frame == 51, "menu up: claimed on the first closed frame", std::to_string(e.frame));
        // A CRSM Lua installed: its loader calls at its load (frame 3): the DLL never claims.
        std::vector<Frame> f = cat({ outside(G), training(1000) });
        f[2].lua_called = true;
        e = simulate(f, Mode::Auto, true);
        check(e.d == Decision::LuaCalled && e.frame == 3, "the loader called at its load: no standalone claim, ever");
        // A loader that calls late (a script reset in training) still wins if it comes before the deadline.
        f = cat({ outside(G), training(1000) });
        f[G + T - 2].lua_called = true;
        e = simulate(f, Mode::Auto, true);
        check(e.d == Decision::LuaCalled, "a late Lua call before the deadline: still the Lua's");
        // A CRSM Lua installed: leaving training before the delay starts the count again.
        e = simulate(cat({ outside(G), training(T - 1), outside(30), training(T) }), Mode::Auto, true);
        check(e.d == Decision::Foreign && e.frame == G + (T - 1) + 30 + T, "CRSM Lua installed, training left before T frames: T frames again",
              std::to_string(e.frame));
        // An installed CRSM Lua that never calls (v1.x, the Training Suite): no claim; foreign at the deadline.
        e = simulate(cat({ outside(2000), training(400) }), Mode::Auto, true);
        check(e.d == Decision::Foreign && e.frame == 2000 + T, "CRSM Lua installed, silent: foreign at the deadline, never a claim",
              std::to_string(e.frame));
        e = simulate(cat({ outside(2000), training(400) }), Mode::Off, false);
        check(e.d == Decision::Off && e.frame == 1, "off: decided at the first frame, nothing else");
        e = simulate(cat({ outside(2000), training(400) }), Mode::Auto, false, false);
        check(e.d == Decision::NotNative && e.frame == 1, "ui lua: decided at the first frame, nothing else");
    }
}

// ---- a missing settings.json (owners::default_settings): the mod is never left inert -----------------------------------------
// common.cpp load_settings: no file -> find_lua over reframework/autorun -> default_settings(lua found) written and
// default_requested(lua found) used at once. Without a CRSM Lua the DLL alone must run the mod (as the package ships it).
void test_settings_default() {
    using standalone::Decision;
    using standalone::Mode;
    begin("owners: a missing settings.json");
    const std::array<bool, owners::kCount> all{ true, true, true, true, true, true };
    auto read_back = [](const json& j, std::array<bool, owners::kCount>* req, std::optional<std::string>* sa) {
        for (size_t i = 0; i < owners::kCount; ++i) {           // as common.cpp load_settings reads a file
            auto it = j["owners"].find(owners::kNames[i]);
            (*req)[i] = it != j["owners"].end() && it->is_string() && it->get<std::string>() == "native";
        }
        if (j.contains("standalone") && j["standalone"].is_string()) *sa = j["standalone"].get<std::string>();
    };
    const std::string no_lua = standalone::find_lua([](const std::string&) { return false; });
    check(no_lua.empty(), "no CRSM Lua script: find_lua says none");
    {
        const std::array<bool, owners::kCount> req = owners::default_requested(!no_lua.empty());
        check(req == all, "no CRSM Lua: the six owners asked native");
        const owners::Result r = owners::resolve(req, all);
        check(r.owns == all && r.notes.empty(), "no CRSM Lua: all six layers the DLL's, ui included, no note", join(r.notes));
        const json j = owners::default_settings(false);
        std::array<bool, owners::kCount> back{};
        std::optional<std::string> sa;
        read_back(j, &back, &sa);
        check(back == req, "no CRSM Lua: the file written reads back as the owners used");
        std::string note;
        check(sa && standalone::parse_mode(sa, &note) == Mode::Auto && note.empty(), "no CRSM Lua: standalone auto written out");
        bool all_native = j["owners"].size() == owners::kCount;
        for (const auto& [k, v] : j["owners"].items()) all_native = all_native && v == "native";
        check(all_native && j.size() == 2, "no CRSM Lua: owners (six, native) and standalone only", j.dump());
        // a fresh install: the defaults hand the UI to the DLL, which claims it alone at the first frame
        const SimEnd e = simulate(cat({ outside(standalone::kGraceFrames), training(standalone::kTrainingFrames + 10) }), Mode::Auto,
                                  false, r.owns[owners::kUi]);
        check(e.d == Decision::Claim, "no CRSM Lua: the standalone claim follows (the mod is not inert)", standalone::decision_name(e.d));
    }
    for (const char* entry : standalone::kLuaEntries) {
        const std::string want = entry;
        const std::string lua = standalone::find_lua([&](const std::string& n) { return n == want; });
        const std::array<bool, owners::kCount> req = owners::default_requested(!lua.empty());
        const owners::Result r = owners::resolve(req, all);
        check(std::none_of(req.begin(), req.end(), [](bool b) { return b; }) &&
              std::none_of(r.owns.begin(), r.owns.end(), [](bool b) { return b; }) && r.notes.empty(),
              want + " installed: the six owners lua (the Lua drives)");
        const json j = owners::default_settings(!lua.empty());
        std::array<bool, owners::kCount> back = all;
        std::optional<std::string> sa;
        read_back(j, &back, &sa);
        check(back == req && sa && *sa == "auto", want + " installed: the file written reads back as the owners used");
        const SimEnd e = simulate(cat({ outside(standalone::kGraceFrames), training(standalone::kTrainingFrames + 10) }), Mode::Auto,
                                  true, r.owns[owners::kUi]);
        check(e.d == Decision::NotNative, want + " installed: no standalone claim (ui lua)", standalone::decision_name(e.d));
    }
}

// ---- the settings.json of the package (package/): what the owner rule and the standalone setting make of it ------------------
void test_package_settings(const fs::path& repo) {
    begin("package: the settings.json it ships");
    const std::array<bool, owners::kCount> all{ true, true, true, true, true, true };
    auto read = [&](std::array<bool, owners::kCount>* req, std::optional<std::string>* sa) {
        const fs::path p = repo / "package" / "settings.json";
        std::ifstream f(p, std::ios::binary);
        const json j = json::parse(f, nullptr, false);
        if (j.is_discarded() || !j.contains("owners")) return false;
        for (size_t i = 0; i < owners::kCount; ++i) {
            auto it = j["owners"].find(owners::kNames[i]);
            (*req)[i] = it != j["owners"].end() && it->is_string() && it->get<std::string>() == "native";
        }
        if (j.contains("standalone") && j["standalone"].is_string()) *sa = j["standalone"].get<std::string>();
        return true;
    };
    std::array<bool, owners::kCount> req{};
    std::optional<std::string> sa;
    const bool ok = read(&req, &sa);
    check(ok, "package/settings.json readable");
    if (ok) {
        const owners::Result r = owners::resolve(req, all);
        check(std::all_of(r.owns.begin(), r.owns.end(), [](bool b) { return b; }) && r.notes.empty(), "Native: all six layers the DLL's");
        std::string note;
        check(sa && standalone::parse_mode(sa, &note) == standalone::Mode::Auto && note.empty(), "Native: standalone auto, written out");
    }
    // the file a missing settings.json is replaced with (no CRSM Lua) is the package's, byte for byte
    std::ifstream f(repo / "package" / "settings.json", std::ios::binary);
    const std::string shipped((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const std::string written = owners::default_settings(false).dump(4);
    check(written == shipped, "default_settings(no CRSM Lua) = package/settings.json byte for byte", written);
}

// ---- the name keys: one reader, one decision ----------------------------------------------------------------------------------
void test_name_keys() {
    begin("name keys: T / pad Y and G / R / pad X routed once");
    using lg::NameKey;
    check(lg::route_name_key(true, false, false, true, true) == NameKey::RenameLoadout, "T on the Manager page: rename the drill (P5)");
    check(lg::route_name_key(true, false, false, true, false) == NameKey::NameSlot, "T elsewhere (a Record row): the slot name dialog (P4)");
    check(lg::route_name_key(false, true, false, true, false) == NameKey::DeleteSlotName, "G / R / X on a Record row: the name deleted");
    check(lg::route_name_key(false, true, false, true, true) == NameKey::None, "G / R / X on the Manager page: nothing");
    check(lg::route_name_key(true, true, false, true, false) == NameKey::NameSlot, "T and X together: T wins (RMT's if / elseif)");
    check(lg::route_name_key(true, true, false, true, true) == NameKey::RenameLoadout, "T and X on the Manager: rename");
    check(lg::route_name_key(false, false, false, true, true) == NameKey::None, "no edge: nothing");
    for (int m = 0; m < 2; ++m)
        for (int t = 0; t < 2; ++t)
            for (int d = 0; d < 2; ++d) {
                check(lg::route_name_key(t, d, true, true, m) == NameKey::None, "a dialog / box / Community up: nothing (RMT:1762)");
                check(lg::route_name_key(t, d, false, false, m) == NameKey::None, "pages not installed: nothing (RMT:1762)");
            }
    // a held key gives one press (the reader's edge), released then pressed again gives a second one
    lg::KeyEdge e;
    int presses = 0;
    for (bool down : { true, true, true, false, true, true }) if (e.press(down)) ++presses;
    check(presses == 2, "held T: one press per push", std::to_string(presses));
}

// ---- T on the Manager page through P5's queue and Runner -----------------------------------------------------------------------
void test_rename_key() {
    begin("T on the Manager page: P5's rename, once");
    g_loc.set_lang("en");
    FakeLoadouts f; FakeScreen s; mgr::Runner r(f, s, translator());
    mgr::Queue q;
    // P4's reader routes, P5's rename_key() queues, P5's frame drains and runs
    if (lg::route_name_key(true, false, false, true, true) == lg::NameKey::RenameLoadout) q.push(mgr::Action{ mgr::Act::Rename, 0 });
    if (lg::route_name_key(true, false, false, true, false) == lg::NameKey::RenameLoadout) q.push(mgr::Action{ mgr::Act::Rename, 0 });
    const auto acts = q.drain();
    check(acts.size() == 1 && acts[0].act == mgr::Act::Rename, "one rename queued (the Record page's T queues none)");
    for (const auto& a : acts) r.run(a);
    check(s.count("text_input") == 1 && s.input_title == "Rename drill" && s.input_initial == "Old", "the rename dialog with the selected name",
          s.input_title + " / " + s.input_initial);
    check(std::find(f.calls.begin(), f.calls.end(), "rename_selected(New)") != f.calls.end(), "renamed to the typed name", join(f.calls));
    check(!s.tickers.empty() && s.tickers.back() == "Drill renamed: New", "the page's ticker", join(s.tickers));
    check(s.count("rows_dirty") == 0 && s.count("list_changed") == 1, "a rename resyncs the spin, not the slot rows (RMT:1620-1625)");
    f.sel.clear();
    r.run(mgr::Action{ mgr::Act::Rename, 0 });
    check(s.count("text_input") == 1, "nothing selected: T does nothing");
}

// ---- the asynchronous tickers ---------------------------------------------------------------------------------------------------
void test_tickers() {
    begin("asynchronous tickers: exactly one consumer");
    using lg::TickerSink;
    check(lg::ticker_sink(false, true) == TickerSink::Lua && lg::ticker_sink(false, false) == TickerSink::Lua, "ui not running: the Lua's frame_poll");
    check(lg::ticker_sink(true, true) == TickerSink::Show, "ui running: the DLL's ticker strip");
    check(lg::ticker_sink(true, false) == TickerSink::Drop, "ui running without its GetMessage hook: dropped (no Lua UI to show them)");
    for (int ui = 0; ui < 2; ++ui)
        for (int hook = 0; hook < 2; ++hook) {
            const bool lua_takes = lg::ticker_sink(ui, true) == TickerSink::Lua;   // mech.cpp lua_frame_poll's test
            const bool dll_takes = ui && lg::ticker_sink(ui, hook) != TickerSink::Lua;   // ticker.cpp frame (runs only with ui claimed)
            check((lua_takes ? 1 : 0) + (dll_takes ? 1 : 0) == 1,
                  "ui " + std::to_string(ui) + ", hook " + std::to_string(hook) + ": one consumer");
        }
}

// ---- the refresh after a Community download --------------------------------------------------------------------------------------
void test_after_download() {
    begin("after a Community download: the list re-read once");
    {
        std::vector<std::string> calls;
        lg::after_download([&] { calls.push_back("refresh_list"); return true; }, [&] { calls.push_back("invalidate"); },
                           [&] { calls.push_back("ensure"); });
        check(calls == std::vector<std::string>{ "refresh_list" }, "Manager page running: its refresh only (no second glob)", join(calls));
    }
    {
        std::vector<std::string> calls;
        lg::after_download([&] { calls.push_back("refresh_list"); return false; }, [&] { calls.push_back("invalidate"); },
                           [&] { calls.push_back("ensure"); });
        check(calls == std::vector<std::string>{ "refresh_list", "invalidate", "ensure" }, "no Manager page: the files layer at once", join(calls));
    }
    {   // the Manager's side: refresh_list() queues Act::Refresh, the frame runs it: invalidate, ensure, the spin synced (RMT:276-278)
        FakeLoadouts f; FakeScreen s; mgr::Runner r(f, s, translator());
        mgr::Queue q;
        lg::after_download([&] { q.push(mgr::Action{ mgr::Act::Refresh, 0 }); return true; }, [&] { f.invalidate(); }, [&] { f.ensure(); });
        check(f.calls.empty(), "nothing read on the Community's side");
        for (const auto& a : q.drain()) r.run(a);
        check(f.calls == std::vector<std::string>{ "invalidate", "ensure" }, "the frame: invalidate, ensure (one glob)", join(f.calls));
        check(s.calls == std::vector<std::string>{ "list_changed" }, "then the spin synced (manager_dirty)", join(s.calls));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fs::path repo;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--repo" && i + 1 < argc) repo = argv[++i];
    }
    if (repo.empty()) {
        std::fprintf(stderr, "usage: integ_bench --repo <crsm folder>\n");
        return 2;
    }
    const json file = model::luajson::load_file(repo / "data" / "SF6_CRSM_data" / "locale.json");
    if (file.is_null()) { std::fprintf(stderr, "locale.json not found under %s\n", repo.string().c_str()); return 2; }
    g_loc.load(file);
    std::printf("[P4 + P5] integration\n");
    test_owners();
    test_standalone();
    test_settings_default();
    test_package_settings(repo);
    test_name_keys();
    test_rename_key();
    test_tickers();
    test_after_download();

    int checks = 0, fails = 0;
    std::printf("\n%-60s %8s %8s\n", "group", "checks", "failed");
    for (const std::string& n : g_order) {
        const Group& g = g_groups[n];
        std::printf("%-60s %8d %8d\n", n.c_str(), g.checks, g.fails);
        checks += g.checks; fails += g.fails;
    }
    std::printf("%-60s %8d %8d\n", "TOTAL", checks, fails);
    std::printf("\n%s\n", fails == 0 ? "ALL GREEN" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
