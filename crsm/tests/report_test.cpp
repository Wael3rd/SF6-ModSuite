// report_test - the pure parts of "Report a bug": what the guards promise. No game, no network; its files only in --work.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "report/gather.hpp"
#include "report/report_logic.hpp"
#include "report/pixels.hpp"
#include "ui/logic.hpp"

namespace fs = std::filesystem;
namespace rp = report;
using json = nlohmann::json;

static int g_checks = 0, g_failed = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { ++g_failed; printf("FAILED: %s\n", what.c_str()); }
}
static bool has(const std::string& s, const std::string& what) { return s.find(what) != std::string::npos; }
static void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

static void test_clean() {
    rp::Scrub s;
    s.names = { "Wael", "DESKTOP-XY12" };
    s.game_root = "D:\\Games\\Steam\\Street Fighter 6";
    const auto c = [&](const std::string& t) { return rp::clean_log(t, s); };
    // any path under a user's folder, whoever's
    check(c("read C:\\Users\\Someone Else\\AppData\\x.txt ok") == "read <user>\\AppData\\x.txt ok", "a Windows path: " + c("read C:\\Users\\Someone Else\\AppData\\x.txt ok"));
    check(c("C:/Users/bob/Documents/a.log") == "<user>/Documents/a.log", "forward slashes: " + c("C:/Users/bob/Documents/a.log"));
    check(c("\"C:\\\\Users\\\\bob\\\\a.json\"") == "\"<user>\\\\a.json\"", "JSON's doubled backslashes: " + c("\"C:\\\\Users\\\\bob\\\\a.json\""));
    check(c("\\\\?\\C:\\Users\\bob\\a") == "<user>\\a", "a long path prefix: " + c("\\\\?\\C:\\Users\\bob\\a"));
    check(c("c:\\USERS\\Bob\\x") == "<user>\\x", "any case: " + c("c:\\USERS\\Bob\\x"));
    check(c("C:\\Documents and Settings\\bob\\x") == "<user>\\x", "the old name: " + c("C:\\Documents and Settings\\bob\\x"));
    check(!has(c("path \"C:\\Users\\bob\""), "bob"), "a path that ends at the name: " + c("path \"C:\\Users\\bob\""));
    check(!has(c("D:\\Users\\bob\\x"), "bob"), "another drive");
    // the Windows user's name and the PC's, anywhere
    check(c("hello wael from WAEL3rd") == "hello <user> from <user>3rd", "the user's name, any case: " + c("hello wael from WAEL3rd"));
    check(!has(c("host desktop-xy12 up"), "xy12"), "the PC's name");
    rp::Scrub shortname;
    shortname.names = { "Al" };
    check(rp::clean_log("Also Alex", shortname) == "Also Alex", "a name of 2 letters is no pattern");
    // the game folder
    check(c("D:\\Games\\Steam\\Street Fighter 6\\reframework\\data") == "<game>\\reframework\\data", "the game folder: " + c("D:\\Games\\Steam\\Street Fighter 6\\reframework\\data"));
    check(c("d:/games/steam/street fighter 6/x") == "<game>/x", "the game folder, other way round: " + c("d:/games/steam/street fighter 6/x"));
    // platform ids
    check(c("id 76561198012345678 here") == "id <id> here", "a SteamID64");
    check(c("steam76561198012345678x") == "steam<id>x", "a SteamID64 inside a word: " + c("steam76561198012345678x"));
    check(c("[U:1:12345678] and [G:1:9]") == "<id> and <id>", "Steam3: " + c("[U:1:12345678] and [G:1:9]"));
    check(c("STEAM_0:1:1234567 steam_1:0:9") == "<id> <id>", "Steam2: " + c("STEAM_0:1:1234567 steam_1:0:9"));
    check(c("https://steamcommunity.com/id/some_name/ x") == "https://<id>/ x", "a vanity profile: " + c("https://steamcommunity.com/id/some_name/ x"));
    check(c("steamcommunity.com/profiles/76561198012345678") == "<id>", "a profile: " + c("steamcommunity.com/profiles/76561198012345678"));
    check(c("cfn 3512345678 psn 1234567890123456789") == "cfn <id> psn <id>", "any lone long number");
    // what must stay
    check(c("mod_id: 402b1ac9c4dd7fb1, addr 0x7ff612345678, f123456 12:30:05 2026-11-07") == "mod_id: 402b1ac9c4dd7fb1, addr 0x7ff612345678, f123456 12:30:05 2026-11-07", "hex, frames, dates: " + c("mod_id: 402b1ac9c4dd7fb1, addr 0x7ff612345678, f123456 12:30:05 2026-11-07"));
    check(c("Outfit VII, 12345678 bytes") == "Outfit VII, 12345678 bytes", "a short number");
    check(rp::clean_paths("Name-1591-1-0-1709931165.zip C:\\Users\\bob\\x", s) == "Name-1591-1-0-1709931165.zip <user>\\x", "clean_paths: only paths and names");
}

static void test_caps() {
    std::string big;
    for (int i = 0; big.size() < 500 * 1024; ++i) big += "line " + std::to_string(i) + " \xC3\xA9\xE6\xBC\xA2 text\n";
    const std::string cut = rp::cap_log(big);
    check(cut.size() <= rp::kLogMaxBytes, "a log within its cap: " + std::to_string(cut.size()));
    check(has(cut, "line 0 "), "its start stays");
    check(has(cut, "bytes left out"), "it says so");
    const std::string last_line = big.substr(big.rfind('\n', big.size() - 2) + 1);
    check(cut.size() >= last_line.size() && cut.compare(cut.size() - last_line.size(), last_line.size(), last_line) == 0, "its end stays");
    check(json::parse(json(cut).dump(), nullptr, false).is_string(), "still valid UTF-8 for JSON");
    check(rp::cap_log("short") == "short", "a short log is left alone");
    check(rp::cut_utf8("\xE6\xBC\xA2\xE6\xBC\xA2", 4) == "\xE6\xBC\xA2", "a cut on a character");
    check(rp::base64(reinterpret_cast<const uint8_t*>("Man"), 3) == "TWFu" && rp::base64(reinterpret_cast<const uint8_t*>("Ma"), 2) == "TWE=" && rp::base64(reinterpret_cast<const uint8_t*>("M"), 1) == "TQ==", "base64");
    check(rp::sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256");
    const std::string a64(64, 'a');
    check(rp::sha256_hex(a64.data(), 64) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb", "SHA-256 on a whole block: " + rp::sha256_hex(a64.data(), 64));
    // only our logs, by exact path: never hook_trace.txt, never perf, never anything else
    for (const rp::LogFile& l : rp::kLogs) {
        const std::string p = l.path;
        check(!has(p, "hook_trace") && !has(p, "perf") && !has(p, "..") && !has(p, ":"), std::string("the log list: ") + l.path);
    }
    check(std::size(rp::kLogs) == 2, "two logs: the loader's, the plugin's");
}

static void test_key() {
    const json outfits = json::parse(R"({"outfits":[
      {"fighter":30,"costume_no":5,"name":"Outfit I","based_on":0,"label":"Swimsuit C. Viper C1","source":"C. Viper/Swimsuit 1.0.zip"},
      {"fighter":30,"costume_no":16,"name":"Outfit VI","based_on":1,"label":"Lace <lingerie>","source":"C. Viper/Lace 1.1.zip"},
      {"fighter":28,"costume_no":15,"name":"Outfit I","based_on":0,"label":"Mini Dress","source":"Mai/Mini.zip"}]})");
    const json registry = json::parse(R"({"slots":[{"mod_id":"402B1ac9c4dd7fb1","fighter":30,"original_costume_no":0,"new_costume_no":5},
      {"mod_id":"aaaa","fighter":28,"original_costume_no":0,"new_costume_no":15}]})");
    const json* a = rp::find_outfit(outfits, 30, "Outfit I");
    check(a && (*a)["costume_no"] == 5, "by the row's Outfit I, on that fighter");
    check(rp::find_outfit(outfits, 28, "Outfit I") && (*rp::find_outfit(outfits, 28, "Outfit I"))["label"] == "Mini Dress", "the same word on another fighter");
    check(rp::find_outfit(outfits, 30, "Swimsuit C. Viper C1") == a, "by the label names-scroll's loader shows");
    check(rp::find_outfit(outfits, 30, "Lace lingerie") != nullptr, "the row without what the game cannot draw");
    check(!rp::find_outfit(outfits, 30, "Outfit 2") && !rp::find_outfit(outfits, -1, "Outfit I") && !rp::find_outfit(json(nullptr), 30, "Outfit I"), "a game outfit, no fighter, no file");
    const json* sl = rp::find_slot(registry, 30, 5);
    check(sl && !rp::find_slot(registry, 30, 6), "the slot by its number");

    check(rp::row_name("Outfit VII  <ICON KeyF> options") == "Outfit VII", "a row's icon suffix");
    const std::string k1 = rp::costume_key(30, "Outfit I", a, sl, true);
    check(k1 == "mod:402b1ac9c4dd7fb1:f30:c0", "a mod's id: " + k1);
    check(rp::costume_key_ok(k1), "well formed");
    const json updated = json::parse(R"({"mod_id":"5555555555555555","fighter":30,"original_costume_no":0,"new_costume_no":5})");
    check(rp::costume_key(30, "Outfit I", a, &updated, true) != k1, "a mod's update: a new key");
    check(rp::costume_key(30, "Outfit I", a, nullptr, true) != k1 && rp::costume_key_ok(rp::costume_key(30, "Outfit I", a, nullptr, true)), "no registry: from its source");
    check(rp::costume_key(30, "Outfit 2", nullptr, nullptr, true, 1) == "game:30:c1" && rp::costume_key(30, "Tenue 2", nullptr, nullptr, true, 1) == "game:30:c1", "a game outfit by its number: the same in every language");
    check(rp::find_outfit_number(outfits, 30, 16) && (*rp::find_outfit_number(outfits, 30, 16))["name"] == "Outfit VI" && !rp::find_outfit_number(outfits, 30, 1) && !rp::find_outfit_number(outfits, 30, -1), "by its number");
    const std::string g = rp::costume_key(30, "Outfit 2", nullptr, nullptr, true);
    check(g.rfind("game:30:", 0) == 0 && rp::costume_key_ok(g), "a game outfit: " + g);
    check(g != rp::costume_key(31, "Outfit 2", nullptr, nullptr, true) && g != rp::costume_key(30, "Outfit 3", nullptr, nullptr, true), "character + outfit");
    check(g == rp::costume_key(30, "OUTFIT 2", nullptr, nullptr, true), "the same outfit whatever the case");
    check(rp::costume_key(30, "Outfit 2", nullptr, nullptr, false).rfind("unk:30:", 0) == 0, "no data: says so");
    check(rp::costume_key_ok(rp::costume_key(-1, "", nullptr, nullptr, false)), "nothing at all: still a key");
    for (const char* bad : { "", "mod:", "x:1", "mod:A", "mod:a b", "mod:<", "game" }) check(!rp::costume_key_ok(bad), std::string("refused: ") + bad);
    check(!rp::costume_key_ok("mod:" + std::string(73, 'a')) && rp::costume_key_ok("mod:" + std::string(72, 'a')), "its length");

    check(rp::qwerty_scan("<ICON KeyR>") == 0x13 && rp::qwerty_scan("<ICON KeyQ> x") == 0x10 && rp::qwerty_scan("<ICON BtnL>") == 0 && rp::qwerty_scan("") == 0, "the key's place");
}

// Asleep means asleep: nothing here has a Training flag or a claim to look at; what the module may depend on, its sources are held to
static void test_wake() {
    rp::LeftWatch w;
    check(!w.seen(), "a new waking");
    check(!w.left(100, 1000) && w.seen(), "its first frame counts as a call");
    check(!w.left(101, 1001), "the update runs: awake");
    check(!w.left(101, 1001 + rp::kLeftFrames), "a long pause in its calls is not yet a leaving");
    check(w.left(101, 1002 + rp::kLeftFrames), "no call for that long: the screen is left");
    w.reset();
    check(!w.seen() && !w.left(101, 5000), "the next opening starts again, whatever the count");
    check(!w.left(UINT32_MAX, 5001) && !w.left(0, 5002), "the count may wrap");
    w.reset();
    w.beat(5, 10);
    check(!w.left(5, 10 + rp::kLeftFrames), "a report under way counts as beats, then the same rule");
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
// "Report a bug" before any Training and from Training's Multi Menu: both were lost in game, 07/11, by gates of the UI's frame
static void test_sources(const fs::path& crsm) {
    const std::string report = slurp(crsm / "src/ui/bug_report.cpp"), ui = slurp(crsm / "src/ui/ui.cpp"), dialogs = slurp(crsm / "src/ui/dialogs.cpp"), windows = slurp(crsm / "src/ui/windows.cpp");
    check(!report.empty() && !ui.empty() && !dialogs.empty() && !windows.empty(), "the sources are there");
    check(!has(ui, "bug_report::frame"), "not in the UI's frame: it returns before the claim");
    check(!has(report, "ui::active()") && !has(report, "ui::training()") && !has(report, "ui::paused()") && !has(report, "mech::state()"), "no gate on the claim, Training or the pause");
    check(has(report, "set_frame_callback(g_opt_client, on_core_frame, nullptr)") && has(report, "set_frame_callback(g_opt_client, nullptr, nullptr)"), "its own frame callback, on and off");
    check(has(report, "dlg::frame(false)") && has(report, "ui::lang::frame("), "the text dialog and the language run for it until the claim");
    check(!has(report, "add_hook") && !has(report, "on_present("), "no hook of its own beside the screen's: nothing per frame anywhere asleep");
    check(has(report, "app.UIFlowMatchingFighterSetting.Main\", \"OnUpdate"), "woken by the screen's own update, through the code check (mech::hooks)");
    check(!has(report, "OnEnter") || !has(report, "\"OnEnter"), "no state OnEnter");
    check(has(dialogs, "ui::dialogs_on()") && has(dialogs, "bool text_available()"), "the text dialog's hooks and requests follow dialogs_on");
    check(has(windows, "com::open() || bug_report::awake()"), "under the screen in Training, the Record page's R and T act on nothing");
    // The Steam ID: read once, for g_values, never journaled, never in a command or a snapshot
    const std::string gather_src = slurp(crsm / "src/report/gather.cpp"), logic_src = slurp(crsm / "src/report/report_logic.cpp"), snap_src = slurp(crsm / "src/report/gather.hpp");
    check(has(report, "g_values.steam_id = rp::steam_id_text(community::steam::local_steam_id())"), "the menu's Steam ID comes from the Steam client");
    size_t uses = 0;
    for (size_t p = report.find("steam_id"); p != std::string::npos; p = report.find("steam_id", p + 1)) ++uses;
    check(uses == 3 && !has(report, "journal(\"bug report: steam"), "and goes nowhere else in the game side (that one line: " + std::to_string(uses) + ")");
    check(!has(gather_src, "steam_id") && !has(snap_src, "steam_id") && !has(gather_src, "local_steam_id"), "not in what is gathered");
    const size_t mk = logic_src.find("make_command");
    const size_t mk_end = mk == std::string::npos ? mk : logic_src.find("\n}", mk);
    check(mk != std::string::npos && !has(logic_src.substr(mk, mk_end - mk), "steam"), "not in what is sent");
    // A status request carries the key alone
    const size_t ask = report.find("void ask_status()"), ask_end = report.find("community::client().send", ask);
    const std::string ask_src = report.substr(ask, ask_end - ask);
    check(has(ask_src, "\"report_get\"") && has(ask_src, "\"costume_key\"") && !has(ask_src, "jpeg") && !has(ask_src, "logs") && !has(ask_src, "make_command"), "a status request is not a report");
    check(has(report, "d.desc = \"\";") && has(report, "cur == g_summary") && has(report, "g_wrote == g_summary && cur == g_read_back"), "R7: the long text has one writer, the whole text compared first");
    check(!has(report, "\"set_AutoRegionFit\"") && has(report, "set_AutoRegionFit(System.Boolean)"), "the fit is really turned off: the setter's full signature");
    check(has(report, "kDockX = -430.0f") && has(report, "kFrame = { 1058, 892, 1000, 834, 990, 824, -282, -352, 262 }"), "on the side, as the Color Mod's Edit Drive Colors");
}

static void test_screen() {
    rp::ScreenRead s;
    s.agent_drawn = true; s.tab = 0; s.costume_row = true;
    check(rp::entry_allowed(s), "the Character tab");
    auto no = [&](auto change, const char* what) { rp::ScreenRead t = s; change(t); check(!rp::entry_allowed(t), what); };
    no([](rp::ScreenRead& t) { t.tab = 1; }, "the Titles tab");
    no([](rp::ScreenRead& t) { t.tab = 2; }, "the Profile tab");
    no([](rp::ScreenRead& t) { t.tab = -1; }, "a tab not read proves nothing");
    no([](rp::ScreenRead& t) { t.costume_row = false; }, "the costume row hidden");
    no([](rp::ScreenRead& t) { t.agent_drawn = false; }, "the agent not drawn");
    no([](rp::ScreenRead& t) { t.input_on = false; }, "a game's dialog over it");
    no([](rp::ScreenRead& t) { t.window_open = true; }, "an option window");
    no([](rp::ScreenRead& t) { t.dialog_up = true; }, "CRSM's dialogs");
    no([](rp::ScreenRead& t) { t.key_taken = true; }, "the game's own use of the key");
    no([](rp::ScreenRead& t) { t.can_report = false; }, "no Community client");
}

// Open with the server's answer given
static rp::Session opened(rp::Server sv, const std::string& text = "") {
    rp::Session s;
    s.begin();
    s.shot_settled();
    if (sv == rp::Server::Unknown) s.server_unreachable();
    else if (sv != rp::Server::Checking) s.server_answer(sv, text);
    return s;
}
static bool rows_are(const rp::Session& s, std::vector<rp::Row> want) { want.insert(want.begin(), rp::Row::Status); return s.rows() == want; }
static bool opts_are(const rp::Session& s, const std::vector<std::string>& want) { return s.options() == want; }

static void test_session() {
    using S = rp::Stage;
    using V = rp::Server;
    using R = rp::Row;
    rp::Session s;
    check(s.stage() == S::Idle && !s.send(), "nothing up: nothing to send");
    check(s.begin() && s.stage() == S::Shooting && !s.begin(), "the key: one report at a time");
    check(!s.send(), "not before the menu is open");
    s.shot_settled();
    check(s.stage() == S::Open, "the menu");
    // Checking: nothing else shows, nothing can be done
    check(s.server() == V::Checking && opts_are(s, { "report_st_checking" }) && rows_are(s, {}), "checking: the one row");
    check(!s.choose(1) && !s.confirm_due(), "checking: STATUS does nothing");
    // No report: OK by default, nothing else; BUG the form
    s.server_answer(V::None, "");
    check(opts_are(s, { "report_st_ok", "report_st_bug" }) && s.option_index() == 0 && s.pick() == rp::Pick::Ok && rows_are(s, {}), "no report: OK, and nothing else");
    check(s.choose(1) && s.pick() == rp::Pick::Bug && rows_are(s, { R::Text, R::Send }), "BUG: the two rows");
    check(!s.choose(1), "the same choice again changes nothing");
    check(!s.choose(7) && !s.choose(-1), "an option that does not exist");
    check(s.needs_text() && !s.send(), "no text while the field works: not sent");
    s.set_text_field_ok(false);
    check(!s.needs_text(), "a field that cannot open does not stop a report");
    s.set_text_field_ok(true);
    check(s.choose(0) && rows_are(s, {}), "back to OK: nothing shows again");
    s.choose(1);
    // Back sends nothing, keeps nothing
    s.set_text("secret");
    check(s.closed() && s.stage() == S::Idle && s.text().empty(), "Back: gone, text dropped, nothing on its way");
    check(s.begin() && s.text().empty() && s.server() == V::Checking, "the next report starts empty, and asks again");
    s.shot_settled();
    s.server_answer(V::None, "");
    s.choose(1);
    s.set_text("hair clips");
    check(s.send() && s.busy() == rp::Busy::Sending, "Send");
    check(!s.send() && !s.start_status() && !s.choose(0), "never twice, nothing else meanwhile");
    s.answered(false, "curl exit 28");
    check(s.busy() == rp::Busy::None && s.outcome() == rp::Session::Outcome::Failed && s.text() == "hair clips" && s.server() == V::None, "a failure: the text stays, the row can try again");
    check(s.send(), "try again");
    s.answered(true, "");
    check(s.outcome() == rp::Session::Outcome::Sent && s.server() == V::Waiting && s.pick() == rp::Pick::Held, "sent: WAITING FOR FIX");
    check(opts_are(s, { "report_st_waiting", "report_st_ok" }) && s.option_index() == 0 && rows_are(s, { R::Text, R::Send }) && s.text() == "hair clips", "with the same two rows, its text kept");
    check(s.has_report(), "and Send is now Update");
    check(s.closed() && s.stage() == S::Idle && s.text().empty(), "the window closes: nothing kept");
    // closed while on its way
    s = opened(V::None);
    s.choose(1); s.set_text("x"); s.send();
    check(!s.closed() && s.window_gone() && s.busy() == rp::Busy::Sending, "closed while sending: its answer still comes");
    s.answered(true, "");
    check(s.stage() == S::Idle && s.text().empty(), "then nothing is left");

    // A report on the server: its text comes back, Update, and OK withdraws, only by its own row
    s = opened(V::Waiting, "from the server");
    check(s.text() == "from the server" && s.pick() == rp::Pick::Held && s.option_index() == 0 && rows_are(s, { R::Text, R::Send }), "waiting: the server's text, the form");
    check(s.choose(1) && s.pick() == rp::Pick::Ok && rows_are(s, { R::Withdraw }) && !s.confirm_due(), "OK on a waiting report: only the row that withdraws: nothing left yet");
    check(s.choose(0) && rows_are(s, { R::Text, R::Send }), "changed his mind");
    s.choose(1);
    check(s.start_status() && !s.start_status() && !s.send(), "withdraw: one request");
    s.status_answered(false, rp::Server::Withdrawn, "network");
    check(s.server() == V::Waiting && s.pick() == rp::Pick::Held && s.outcome() == rp::Session::Outcome::Failed, "a failure: the report still waits, the row says so");
    s.choose(1);
    s.start_status();
    s.status_answered(true, V::Withdrawn, "");
    check(s.server() == V::Withdrawn && s.pick() == rp::Pick::Ok && s.option_index() == 0 && opts_are(s, { "report_st_ok", "report_st_bug" }) && rows_are(s, {}), "withdrawn: OK");
    check(s.choose(1) && rows_are(s, { R::Text, R::Send }) && s.has_report(), "BUG after that: Update, the server's row is replaced");
    s.set_text("again");
    check(s.send(), "and it goes");
    s.answered(true, "");
    check(s.server() == V::Waiting, "waiting again");

    // TO TEST: nothing but STATUS; OK confirms at once, BUG is the form
    s = opened(V::Fixed, "old text");
    check(opts_are(s, { "report_st_to_test", "report_st_ok", "report_st_bug" }) && s.option_index() == 0 && rows_are(s, {}) && !s.confirm_due(), "to test: STATUS alone");
    check(s.choose(2) && rows_are(s, { R::Text, R::Send }) && s.text() == "old text" && s.has_report(), "BUG: still broken: Change and Update");
    s.send();
    s.answered(true, "");
    check(s.server() == V::Waiting && s.option_index() == 0, "WAITING FOR FIX once sent");
    s = opened(V::Fixed);
    check(s.choose(1) && s.confirm_due(), "OK: confirm is due");
    check(s.start_status() && !s.confirm_due(), "once");
    s.status_answered(true, V::Confirmed, "");
    check(s.server() == V::Confirmed && !s.confirm_due() && s.pick() == rp::Pick::Ok && rows_are(s, {}) && opts_are(s, { "report_st_ok", "report_st_bug" }), "confirmed: OK again for the player");
    s = opened(V::Fixed);
    s.choose(1); s.start_status();
    s.status_answered(false, V::Confirmed, "network");
    check(s.server() == V::Fixed && s.pick() == rp::Pick::Held && !s.confirm_due() && s.outcome() == rp::Session::Outcome::Failed, "a confirm that failed: TO TEST again");
    // Confirmed: as no report, but a new report replaces the row
    s = opened(V::Confirmed, "t");
    check(s.pick() == rp::Pick::Ok && rows_are(s, {}) && s.has_report(), "confirmed: OK");
    // The server cannot be reached: OK and BUG still work, the send fails cleanly
    s = opened(V::Unknown);
    check(s.server() == V::Unknown && opts_are(s, { "report_st_ok", "report_st_bug" }) && !s.has_report(), "not reached: OK and BUG");
    s.choose(1); s.set_text("t");
    check(s.send(), "and Send may try");
    s.answered(false, "");
    check(s.outcome() == rp::Session::Outcome::Failed && s.text() == "t", "its failure keeps the text");
    // What he typed while it was being asked stays
    s = rp::Session{};
    s.begin(); s.shot_settled();
    s.set_text("mine");
    s.server_answer(V::Waiting, "theirs");
    check(s.text() == "mine", "the answer never overwrites what he typed");
    s.server_answer(V::Fixed, "later");
    check(s.server() == V::Waiting, "one answer only");
    check(rp::server_from(false, "fixed") == V::None && rp::server_from(true, "waiting") == V::Waiting && rp::server_from(true, "fixed") == V::Fixed && rp::server_from(true, "confirmed") == V::Confirmed &&
          rp::server_from(true, "withdrawn") == V::Withdrawn && rp::server_from(true, "?") == V::Waiting, "the server's words");

    check(rp::error_key("banned") == "report_err_banned" && rp::error_key("verification_unavailable") == "report_err_steam" && rp::error_key("account_full") == "report_err_account" &&
          rp::error_key("storage_full") == "report_err_full" && rp::error_key("bad_transition") == "report_err_status" && rp::error_key("") == "report_err_network", "the errors' own texts");
    check(rp::steam_id_text(76561198000000001ULL) == "76561198000000001" && rp::steam_id_text(0).empty() && rp::steam_id_text(12345).empty(), "a SteamID shows, anything else does not");
    const std::vector<uint8_t> j = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 0, 0, 0xFF, 0xC0, 0, 11, 8, 0x04, 0x38, 0x07, 0x80, 3, 0, 0, 0, 0, 0 };
    int w = 0, h = 0;
    check(rp::jpeg_size(j, &w, &h) && w == 1920 && h == 1080, "a JPEG's size from its header");
    check(!rp::jpeg_size({ 1, 2, 3 }, &w, &h) && !rp::jpeg_size({}, &w, &h), "not a JPEG");
}

static std::string joined(const std::vector<rp::SummaryLine>& l) { std::string s; for (const auto& x : l) s += x.text + "\n"; return s; }
// What the menu shows: real values, in form states only
static void test_summary() {
    using V = rp::Server;
    const rp::Tr tr = [](const std::string& k, const std::vector<std::string>& a) { std::string s = k; for (const auto& x : a) s += "(" + x + ")"; return s; };
    rp::MenuValues v;
    v.steam_id = "76561198000000001";
    v.character = "Ryu"; v.outfit = "Outfit <VII>"; v.colour = "Color [3]";
    v.shot_w = 1920; v.shot_h = 1080; v.shot_bytes = 300 * 1024;
    v.versions = { { "Recording Manager", "2.2.8" }, { "Core", "0.9.6" } };
    v.log_loader_bytes = 4096; v.log_plugin_bytes = 2048; v.mods = 37; v.loaded = true;
    rp::Session s = opened(V::None);
    check(rp::summary_lines(s, v, tr).empty(), "OK: no text at all");
    s.choose(1);
    std::string t = joined(rp::summary_lines(s, v, tr));
    check(has(t, "76561198000000001"), "the actual Steam ID");
    check(has(t, "Ryu") && has(t, "Outfit (VII)") && has(t, "Color (3)") && !has(t, "<") && !has(t, "["), "the character, outfit and colour, with no markup");
    check(has(t, "(1920)(1080)(300)"), "the picture's real size");
    check(has(t, "Recording Manager 2.2.8") && has(t, "Core 0.9.6"), "the real versions");
    check(has(t, "(37)") && has(t, "report_sum_logs(4)(2)") && has(t, "report_sum_keep"), "the logs' real sizes, the mods, what Back does");
    check(!has(t, "report_sum_replace") && !has(t, "report_sum_text"), "no warning without a report, no text before one is written");
    s.set_text("line one\nline two <x>");
    t = joined(rp::summary_lines(s, v, tr));
    check(has(t, "line one line two (x)"), "the text on one line, no markup");
    check(rp::row_title(s, rp::Row::Text, tr) == "report_row_change" && rp::row_title(s, rp::Row::Send, tr) == "report_row_send", "Change, Send");
    rp::Session none = opened(V::None);
    none.choose(1);
    check(rp::row_title(none, rp::Row::Text, tr) == "report_row_describe" && rp::row_title(none, rp::Row::Send, tr) == "report_row_need_text", "Describe, and Send says what is missing");
    // no picture, no Steam ID, the files not read yet
    rp::MenuValues b = v;
    b.shot_bytes = 0; b.shot_failed = true; b.steam_id.clear();
    t = joined(rp::summary_lines(none, b, tr));
    check(has(t, "report_sum_shot_none") && has(t, "report_sum_steam_none") && !has(t, "1920"), "says when there is no picture, and still says the Steam ID goes");
    b.loaded = false;
    t = joined(rp::summary_lines(none, b, tr));
    check(!has(t, "report_sum_logs") && !has(t, "report_sum_versions") && has(t, "report_sum_shot_none"), "a moment after the key: what is known");
    // the overwrite warning, in the text and on the row
    rp::Session w = opened(V::Waiting, "theirs");
    t = joined(rp::summary_lines(w, v, tr));
    check(has(t, "report_sum_replace") && has(t, "theirs") && rp::row_title(w, rp::Row::Send, tr) == "report_row_update" && rp::row_title(w, rp::Row::Text, tr) == "report_row_change", "a report exists: replaced, said twice");
    w.send();
    check(rp::row_title(w, rp::Row::Send, tr) == "report_sending", "sending");
    w.answered(true, "");
    t = joined(rp::summary_lines(w, v, tr));
    check(has(t, "report_sum_sent") && !has(t, "report_sum_replace"), "sent");
    w.set_text("new");
    t = joined(rp::summary_lines(w, v, tr));
    check(!has(t, "report_sum_sent") && has(t, "report_sum_replace"), "changed after sending: the warning is back");
    w.send();
    w.answered(false, "banned");
    check(has(rp::row_title(w, rp::Row::Send, tr), "report_failed(report_err_banned)"), "a failure on the row");
    // the states with no form
    w = opened(V::Waiting, "theirs");
    w.choose(1);
    t = joined(rp::summary_lines(w, v, tr));
    check(has(t, "report_sum_withdraw") && !has(t, "76561198000000001") && rp::row_title(w, rp::Row::Withdraw, tr) == "report_row_withdraw", "OK on a waiting report: the warning alone");
    rp::Session f = opened(V::Fixed, "x");
    t = joined(rp::summary_lines(f, v, tr));
    check(has(t, "report_sum_to_test") && !has(t, "76561198000000001"), "to test: what to do, alone");
    f.choose(1); f.start_status(); f.status_answered(false, V::Confirmed, "network");
    t = joined(rp::summary_lines(f, v, tr));
    check(has(t, "report_failed(report_err_network)") && has(t, "report_sum_to_test"), "its failure is said");
    rp::Session u = opened(V::Unknown);
    check(has(joined(rp::summary_lines(u, v, tr)), "report_sum_unreachable"), "the server not reached: said at once");
    // the room: kSummaryLines at most, whatever the wrap gives
    rp::Session big = opened(V::Waiting, "x");
    const std::string all = rp::summary_text(big, v, tr, [](const std::string& line, int max_lines) { return std::vector<std::string>((size_t)max_lines, line.substr(0, 10)); });
    check(std::count(all.begin(), all.end(), '\n') + 1 == rp::kSummaryLines, "never past the room");
    // The command never holds the Steam ID the menu shows
    rp::Report r;
    r.costume_key = "game:1:c1";
    r.text = "t";
    const std::string cmd = rp::make_command(r).dump();
    check(!has(cmd, "steam") && !has(cmd, "76561198"), "the command has no identity");
}

// Every language's real texts with the longest values, wrapped by the UI's own measure: the promises fit the window's room, nothing is lost by the cut
static void test_summary_fits(const fs::path& crsm) {
    const json all = json::parse(slurp(crsm / "data/SF6_CRSM_data/locale.json"), nullptr, false);
    check(all.is_object(), "the texts");
    if (!all.is_object()) return;
    int langs = 0;
    int worst = 0;
    std::string worst_lang;
    for (const auto& [lang, block] : all.items()) {
        if (lang.empty() || lang[0] == '_' || !block.is_object()) continue;
        ++langs;
        const rp::Tr tr = [&block](const std::string& k, const std::vector<std::string>& a) {
            std::string s = block.value("rsm." + k, std::string());
            for (const std::string& x : a) { const size_t p = s.find("%s"); if (p == std::string::npos) break; s.replace(p, 2, x); }
            return s;
        };
        rp::MenuValues v;
        v.steam_id = "76561198000000001";
        v.character = "Chun-Li"; v.outfit = "VEGETA MAJIN Black Hair Long Name Outfit"; v.colour = "Color 10";
        v.shot_w = 1920; v.shot_h = 1080; v.shot_bytes = 449 * 1024;
        v.versions = { { "Recording Manager", "2.2.8" }, { "Core", "0.9.6" }, { "Color Mod", "0.24.2" }, { "Loader", "1.20.0" } };
        v.log_loader_bytes = 64 * 1024; v.log_plugin_bytes = 64 * 1024; v.mods = 137; v.loaded = true;
        rp::Session s = opened(rp::Server::Waiting, std::string(900, 'x'));
        int lines = 0;
        for (const auto& l : rp::summary_lines(s, v, tr)) lines += (int)ui::logic::desc_lines(l.text, 45.0, l.max_lines).size();
        if (lines > worst) { worst = lines; worst_lang = lang; }
        check(lines <= rp::kSummaryLines, lang + ": fits the window, " + std::to_string(lines) + " lines");
        for (const auto& l : rp::summary_lines(s, v, tr)) check(!l.text.empty() && !has(l.text, "%s"), lang + ": a line with its values");
    }
    check(langs == 14, "the 14 languages: " + std::to_string(langs));
    printf("  the summary's longest: %d lines of %d (%s)\n", worst, rp::kSummaryLines, worst_lang.c_str());
}

static void test_pixels() {
    namespace px = report::pixels;
    int w = 0, h = 0;
    px::scaled_size(1920, 1080, &w, &h); check(w == 1920 && h == 1080, "1080p stays");
    px::scaled_size(1280, 720, &w, &h); check(w == 1280 && h == 720, "never up");
    px::scaled_size(3840, 2160, &w, &h); check(w == 1920 && h == 1080, "4K down");
    px::scaled_size(3440, 1440, &w, &h); check(w == 1920 && h == 804, "ultrawide keeps its shape");
    px::scaled_size(1200, 1600, &w, &h); check(w == 810 && h == 1080, "tall too");
    std::vector<uint8_t> out(3 * 4);
    const uint8_t rgba[8] = { 10, 20, 30, 255, 40, 50, 60, 255 };
    check(px::to_bgr(rgba, 8, 2, 1, px::Format::RGBA8, 2, 1, out.data(), 12) && out[0] == 30 && out[1] == 20 && out[2] == 10 && out[3] == 60, "RGBA: channels swapped");
    check(px::to_bgr(rgba, 8, 2, 1, px::Format::BGRA8, 2, 1, out.data(), 12) && out[0] == 10 && out[2] == 30, "BGRA: as it is");
    check(px::to_bgr(rgba, 8, 2, 1, px::Format::BGRA8, 1, 1, out.data(), 12) && out[0] == 25 && out[1] == 35 && out[2] == 45, "downscale: the average");
    check(!px::to_bgr(rgba, 8, 2, 1, px::Format::Unknown, 2, 1, out.data(), 12), "an unknown format is refused");
    const uint32_t sdr10 = (1023u) | (512u << 10) | (0u << 20);
    check(px::to_bgr(reinterpret_cast<const uint8_t*>(&sdr10), 4, 1, 1, px::Format::RGB10A2, 1, 1, out.data(), 12) && out[2] == 255 && out[1] == 128 && out[0] == 0, "10 bits SDR");
    // HDR10: paper white (203 nits, PQ code ~ 592 of 1023) is bright, not white-clipped; black stays black; 10000 nits white; neutral stays neutral
    const uint32_t code = 592;
    const uint32_t paper = code | (code << 10) | (code << 20);
    check(px::to_bgr(reinterpret_cast<const uint8_t*>(&paper), 4, 1, 1, px::Format::RGB10A2_PQ, 1, 1, out.data(), 12) && out[0] == out[1] && out[1] == out[2] && out[0] > 170 && out[0] < 215, "PQ paper white: " + std::to_string(out[0]));
    const uint32_t black = 0;
    check(px::to_bgr(reinterpret_cast<const uint8_t*>(&black), 4, 1, 1, px::Format::RGB10A2_PQ, 1, 1, out.data(), 12) && out[0] == 0 && out[2] == 0, "PQ black");
    const uint32_t peak = 1023 | (1023u << 10) | (1023u << 20);
    check(px::to_bgr(reinterpret_cast<const uint8_t*>(&peak), 4, 1, 1, px::Format::RGB10A2_PQ, 1, 1, out.data(), 12) && out[0] == 255 && out[1] == 255 && out[2] == 255, "PQ peak: white");
    check(px::half_to_float(0x3C00) == 1.0f && px::half_to_float(0x3800) == 0.5f && px::half_to_float(0) == 0.0f && px::half_to_float(0xC000) == -2.0f, "half floats");
}

static void test_gather(const fs::path& work) {
    const fs::path root = work / "game";
    std::error_code ec;
    fs::remove_all(root, ec);
    rp::Scrub scrub;
    scrub.names = { "Wael" };
    scrub.game_root = "C:\\Steam\\Street Fighter 6";
    rp::Snapshot snap;
    snap.fighter = 30; snap.character = "C. Viper"; snap.outfit_row = "Outfit I  <ICON KeyF>"; snap.colour_row = "Color 3";
    snap.text = "the hair"; snap.crsm_version = "2.2.8"; snap.core_version = "0.9.6";
    snap.jpeg = { 0xFF, 0xD8, 0xFF, 0xD9 };

    // nothing of the Slots Loader at all: the report still goes, saying so
    fs::create_directories(root);
    rp::Report bare = rp::gather(snap, root, scrub);
    check(bare.costume_key.rfind("unk:30:", 0) == 0 && rp::costume_key_ok(bare.costume_key), "no data: a key all the same: " + bare.costume_key);
    check(bare.outfit == "Outfit I", "the row's word");
    const std::string notes = bare.notes.dump();
    check(has(notes, "outfits.json") && has(notes, "registry.json") && has(notes, "loader log") && has(notes, "plugin log"), "notes say what could not be read: " + notes);
    check(bare.jpeg.size() == 4 && !has(notes, "no picture"), "the picture is in");
    check(bare.versions["recording_manager"] == "2.2.8" && bare.versions["native_core"] == "0.9.6", "versions");

    // a whole install
    put(root / "reframework/data/SF6_Costumes_Data/outfits.json", R"({"loader":"2026-11-07-x","outfits":[
       {"fighter":30,"fighter_dir":"esf030","costume_no":5,"name":"Outfit I","based_on":0,"label":"Swimsuit","kind":"archive","source":"C. Viper/Swimsuit 1.0.zip"},
       {"fighter":30,"costume_no":6,"name":"Outfit II","based_on":0,"label":"Swimsuit","source":"C. Viper/Swimsuit 1.0.zip"}]})");
    put(root / "reframework/data/SF6_Costumes_Data/registry.json", R"({"slots":[{"mod_id":"402b1ac9c4dd7fb1","fighter":30,"original_costume_no":0,"new_costume_no":5,"scene_name":"esf030v05"}],"possession":[{"swatches":[[1,2]]}]})");
    put(root / "reframework/data/SF6_Costumes_Data/variants.json", R"({"contract":2,"runtime_options":"off"})");
    put(root / "reframework/data/SF6_ColorModNative_data/status.json", R"({"version":"0.24.1"})");
    put(root / "SF6_CostumeLoader.log", "started by C:\\Users\\Wael\\x\nsteam 76561198012345678\n=== at C:\\Steam\\Street Fighter 6\\reframework\n");
    std::string longlog;
    for (int i = 0; longlog.size() < 900 * 1024; ++i) longlog += "f" + std::to_string(i) + " a line of the plugin C:\\Users\\Other Name\\z\n";
    put(root / "reframework/data/SF6_CostumeSlots_data/native_log.txt", longlog);
    put(root / "hook_trace.txt", "SECRET_HOOK_TRACE");
    put(root / "reframework/data/SF6_CostumeSlots_data/hook_trace.txt", "SECRET_HOOK_TRACE");
    put(root / "reframework/data/SF6_CostumeSlots_data/perf_live.log", "SECRET_PERF");
    put(root / "reframework/plugins/SF6_CRSMNative.dll", "abc");
    put(root / "reframework/plugins/other_mod_of_someone.dll", "SECRET_DLL");
    put(root / "amd_ags_x64.dll", "x");
    put(root / "reframework/stage_mods/Ring/Ring.zip", "x");
    put(root / "reframework/color_mods/Ryu/Blue.zip", "x");
    put(root / "modinfo.ini", "name=SF6 Mod Suite\r\nversion=1.22.0\r\nauthor=x\r\n");
    rp::Report r = rp::gather(snap, root, scrub);
    check(r.costume_key == "mod:402b1ac9c4dd7fb1:f30:c0", "the mod's key: " + r.costume_key);
    check(r.outfit == "Swimsuit" && r.outfit_row == "Outfit I" && r.costume_no == 5 && r.colour == "Color 3", "the outfit");
    check(r.record["outfit"]["source"] == "C. Viper/Swimsuit 1.0.zip" && r.record["slot"]["scene_name"] == "esf030v05", "the record");
    check(!r.record.dump().empty() && !has(r.record.dump(), "swatches"), "the slot only, not the whole registry");
    const std::string logs = r.logs.dump(-1, ' ', false, json::error_handler_t::replace);
    check(!has(logs, "Wael") && !has(logs, "Other Name") && !has(logs, "76561198012345678") && !has(logs, "C:\\\\Steam"), "the logs are clean");
    check(has(logs, "<user>") && has(logs, "<id>") && has(logs, "<game>"), "and say what was cut");
    check(r.logs["plugin"].get<std::string>().size() <= rp::kLogMaxBytes, "a long log within its cap");
    check(has(r.logs["plugin"].get<std::string>(), "f0 a line"), "its start");
    const json cmd = rp::make_command(r);
    const std::string all = cmd.dump(-1, ' ', false, json::error_handler_t::replace);
    check(!has(all, "SECRET"), "nothing but our two logs, our plugins");
    check(!has(all, "\"steamid\"") && !has(all, "\"cfn\"") && !has(all, "\"player\""), "no identity declared: the server takes it from the token");
    check(cmd["cmd"] == "report" && cmd["image"] == "/9j/2Q==", "the command, its picture in base64: " + cmd["image"].get<std::string>());
    check(all.size() < 1800 * 1024, "under the Worker's size");
    check(r.versions["color_mod"] == "0.24.1" && r.versions["slots_loader_pak"] == "2026-11-07-x" && r.versions["manager_contract"] == 2 && r.versions["runtime_options"] == "off", "the versions: " + r.versions.dump());
    check(r.versions["modinfo"] == "SF6 Mod Suite 1.22.0", "the package");
    check(r.versions["files"]["SF6_CRSMNative.dll"]["sha256"] == "ba7816bf8f01" && r.versions["files"]["SF6_CRSMNative.dll"]["bytes"] == 3, "a build's name: " + r.versions["files"].dump());
    const std::string mods = r.mods.dump();
    check(has(mods, "C. Viper/Swimsuit 1.0.zip") && has(mods, "Ring/Ring.zip") && has(mods, "Ryu/Blue.zip"), "the mods' list: " + mods);
    check(r.mods.size() == 3, "one line for a mod with two outfits of the same label");

    // Manager mode: one row per variant, each its own mod_id, each its own key
    put(root / "reframework/data/SF6_Costumes_Data/outfits.json", R"({"outfits":[
       {"fighter":30,"costume_no":5,"name":"Outfit I","based_on":0,"label":"Main","source":"C. Viper/M.zip"},
       {"fighter":30,"costume_no":6,"name":"Outfit II","based_on":0,"label":"Main + Addon","source":"C. Viper/M.zip"}]})");
    put(root / "reframework/data/SF6_Costumes_Data/registry.json", R"({"slots":[{"mod_id":"aaaa000000000001","fighter":30,"original_costume_no":0,"new_costume_no":5},
       {"mod_id":"aaaa000000000002","fighter":30,"original_costume_no":0,"new_costume_no":6}]})");
    rp::Snapshot v = snap;
    v.outfit_row = "Outfit II";
    const std::string k2 = rp::gather(v, root, scrub).costume_key;
    v.outfit_row = "Outfit I";
    check(k2 == "mod:aaaa000000000002:f30:c0" && rp::gather(v, root, scrub).costume_key == "mod:aaaa000000000001:f30:c0", "Manager mode: a key per variant: " + k2);
    v.outfit_row = "Main + Addon";
    check(rp::gather(v, root, scrub).costume_key == k2, "the same by its label");

    // the outfit's number, when the screen's part gives it, beats the row's text; a game outfit gets its number
    rp::Snapshot num = snap;
    num.outfit_row = "Some text the row shows";
    num.costume_no = 6;
    check(rp::gather(num, root, scrub).costume_key == "mod:aaaa000000000002:f30:c0", "by the number, whatever the row says");
    num.costume_no = 2;
    const rp::Report gm_r = rp::gather(num, root, scrub);
    check(gm_r.costume_key == "game:30:c2" && gm_r.costume_no == 2, "a game outfit by its number: " + gm_r.costume_key);

    // broken files do not stop a report
    put(root / "reframework/data/SF6_Costumes_Data/outfits.json", "{ not json");
    rp::Report broken = rp::gather(snap, root, scrub);
    check(rp::costume_key_ok(broken.costume_key) && has(broken.notes.dump(), "outfits.json"), "a broken file: said, and it goes");
    // no character read, no picture
    rp::Snapshot none = snap;
    none.fighter = -1; none.jpeg.clear(); none.jpeg_note = "the copy of the frame faulted";
    rp::Report nr = rp::gather(none, root, scrub);
    check(has(nr.notes.dump(), "no picture: the copy of the frame faulted") && has(nr.notes.dump(), "character could not be read"), "a report without a picture says why");
    const json nc = rp::make_command(nr);
    check(!nc.contains("image") && !nc.contains("fighter_id"), "and carries none");
    fs::remove_all(root, ec);
}

int main(int argc, char** argv) {
    fs::path work = fs::temp_directory_path() / "report_test", crsm;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--work")) work = argv[i + 1];
        else if (!strcmp(argv[i], "--repo")) crsm = argv[i + 1];
    }
    test_clean();
    test_caps();
    test_key();
    test_wake();
    if (!crsm.empty()) test_sources(crsm);
    test_screen();
    test_session();
    test_summary();
    if (!crsm.empty()) test_summary_fits(crsm);
    test_pixels();
    test_gather(work);
    printf("report_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
