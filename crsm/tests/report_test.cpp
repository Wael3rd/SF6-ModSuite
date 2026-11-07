// report_test - the pure parts of "Report a bug": what the guards promise. No game, no network; its files only in --work.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "report/gather.hpp"
#include "report/report_logic.hpp"
#include "report/pixels.hpp"

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

static void test_session() {
    using S = rp::Stage;
    rp::Session s;
    check(s.stage() == S::Idle && !s.send(), "nothing up: nothing to send");
    check(s.begin() && s.stage() == S::Shooting && !s.begin(), "the key: one report at a time");
    check(!s.send(), "not before the menu is open");
    s.shot_settled();
    check(s.stage() == S::Open, "the menu");
    check(s.needs_text() && !s.send(), "no text while the field works: not sent");
    s.set_text_field_ok(false);
    check(!s.needs_text(), "a field that cannot open does not stop a report");
    s.set_text_field_ok(true);
    // Back sends nothing, keeps nothing
    s.set_text("secret");
    check(s.closed() && s.stage() == S::Idle && s.text().empty(), "Back: gone, text dropped, nothing on its way");
    check(s.begin() && s.text().empty(), "the next report starts empty");
    s.shot_settled();
    s.set_text("hair clips");
    check(s.send() && s.stage() == S::Sending, "Send");
    check(!s.send(), "never twice");
    s.answered(false, "curl exit 28");
    check(s.stage() == S::Open && s.outcome() == rp::Session::Outcome::Failed && s.text() == "hair clips", "a failure: the text stays, the row can try again");
    check(s.send(), "try again");
    s.answered(true, "");
    check(s.outcome() == rp::Session::Outcome::Sent && s.text().empty() && !s.send(), "sent: the text gone, not sent twice");
    check(s.closed() && s.stage() == S::Idle, "and the window closes");
    // closed while on its way
    s.begin(); s.shot_settled(); s.set_text("x"); s.send();
    check(!s.closed() && s.window_gone() && s.stage() == S::Sending, "closed while sending: its answer still comes");
    s.answered(true, "");
    check(s.stage() == S::Idle && s.text().empty(), "then nothing is left");

    // the rows: the list of what goes out, Steam ID included
    rp::Session m;
    m.begin(); m.shot_settled();
    rp::MenuInput in;
    in.session = &m;
    in.what = "Ryu, Outfit <VII>, Color [3]";
    const rp::Tr tr = [](const std::string& k, const std::vector<std::string>& a) { return a.empty() ? k : k + "(" + a[0] + ")"; };
    auto rows = rp::menu_rows(in, tr);
    check(rows.size() == (size_t)rp::Row::Count, "one row for each thing");
    bool steam = false, send = false;
    for (const auto& r : rows) { steam = steam || r.row == rp::Row::Steam; send = send || r.row == rp::Row::Send; check(!r.desc.empty(), r.title + ": says what it means"); }
    check(steam && send, "the Steam ID on the list, and Send");
    check(rows[(size_t)rp::Row::Outfit].title.find('<') == std::string::npos && rows[(size_t)rp::Row::Outfit].title.find('[') == std::string::npos, "no markup from a mod's name in a game text");
    check(has(rows[(size_t)rp::Row::Send].title, "report_need_text"), "Send says what is missing");
    m.set_text("line one\nline two <x>");
    rows = rp::menu_rows(in, tr);
    check(has(rows[0].title, "line one line two (x)"), "the text on one line, no markup: " + rows[0].title);
    check(has(rows[(size_t)rp::Row::Send].title, "report_row_send"), "Send, now");
    in.shot_ok = false;
    check(has(rp::menu_rows(in, tr)[1].title, "report_row_shot_none"), "the list says when there is no picture");
    in.cut = [](const std::string& t, double) { return t.substr(0, 5) + "..."; };
    check(rp::menu_rows(in, tr)[0].title == "repor...", "a long row is cut, never squeezed");
    check(rp::error_key("banned") == "report_err_banned" && rp::error_key("verification_unavailable") == "report_err_steam" && rp::error_key("curl exit 28") == "report_err_network" && rp::error_key("") == "report_err_network", "the errors' words");
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
    fs::path work = fs::temp_directory_path() / "report_test";
    for (int i = 1; i + 1 < argc; ++i) if (!strcmp(argv[i], "--work")) work = argv[i + 1];
    test_clean();
    test_caps();
    test_key();
    test_screen();
    test_session();
    test_pixels();
    test_gather(work);
    printf("report_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
