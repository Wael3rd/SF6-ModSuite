// report_e2e_test - a whole report through the real client code, to a server given on the command line (a local stand-in, never a live one):
//
//   report_e2e_test --api-base http://127.0.0.1:<port> [--api-key <a key, as an old client's config>] --work <scratch folder> [--after-fixed]
//   Without --api-key: what ships, no key at all, against a server that still has one for the Recording Manager's other routes.
// --after-fixed: the second half, once the admin has marked the mod's report fixed: the menu's TO TEST, and confirming it.
//
// What the game's part would do after the key: a real JPEG (WIC), gather() on a fake game folder, make_command(), then community::Client over
// WinHTTP with a fake Steam ticket. No game, no Steam. It refuses any api-base that is not on this machine.
#include <windows.h>
#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "community/auth.hpp"
#include "community/client.hpp"
#include "community/http.hpp"
#include "model/luajson.hpp"
#include "report/capture.hpp"
#include "report/gather.hpp"
#include "report/report_logic.hpp"

#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;
namespace rp = report;
using json = nlohmann::json;

static int g_checks = 0, g_failed = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    printf("%s: %s\n", ok ? "ok" : "FAILED", what.c_str());
    if (!ok) ++g_failed;
}
static void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << text;
}

class FakeTickets final : public community::TicketSource {
public:
    Ticket get(int) override { Ticket t; t.ok = true; t.handle = 7; t.bytes.assign(40, 0xAB); return t; }
    void release(uint32_t) override {}
};

struct Answer { bool done = false, ok = false; json data; };
static Answer post(community::Client& c, json cmd) {
    Answer a;
    c.send(std::move(cmd), [&](bool ok, const json& d) { a.done = true; a.ok = ok; a.data = d; }, community::kReportTimeoutTicks);
    for (int i = 0; i < 6000 && !a.done; ++i) { c.poll(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    return a;
}

static json get_cmd(const std::string& key) { return json{ { "cmd", "report_get" }, { "costume_key", key }, { "files", "" } }; }
static json status_cmd(const std::string& key, const std::string& action) { return json{ { "cmd", "report_status" }, { "costume_key", key }, { "action", action } }; }

// The admin marked the mod's report fixed (verify_standin): TO TEST, then OK confirms it, the row is OK again and the admin still sees it
static int second_half(const std::string& base, const std::string& key, const fs::path& work) {
    const std::string costume = "mod:402b1ac9c4dd7fb1:f30:c0";
    const fs::path cfg = work / "config.json";
    put(cfg, key.empty() ? json({ {"api_base", base} }).dump() : json({ {"api_base", base}, {"api_key", key} }).dump());   // the shipped config: no key
    FakeTickets tickets;
    community::Client c;
    community::set_client_version("e2e");
    std::string cfg_path = cfg.string();
    for (char& ch : cfg_path) if (ch == '\\') ch = '/';
    c.configure(cfg_path);
    c.worker().set_tickets(&tickets);
    c.start();
    Answer a = post(c, get_cmd(costume));
    check(a.done && a.ok && a.data["exists"] == true && a.data["status"] == "fixed", "marked fixed: the player reads it: " + a.data.dump().substr(0, 120));
    rp::Session m;
    m.begin(); m.shot_settled();
    m.server_answer(rp::server_from(true, a.data["status"].get<std::string>()), a.data["text"].get<std::string>());
    check(m.server() == rp::Server::Fixed && m.option_index() == 0 && m.options()[0] == "report_st_to_test" && m.options()[1] == "report_st_ok", "the menu: TO TEST");
    check(m.choose(1) && m.rows().size() == 2 && m.rows()[1] == rp::Row::MarkFixed && m.start_status(), "OK, then its row MARK AS FIXED: confirm goes");
    a = post(c, status_cmd(costume, rp::action_confirm()));
    check(a.done && a.ok && a.data["ok"] == true, "confirmed: " + a.data.dump());
    m.status_answered(true, rp::Server::Confirmed, "");
    check(m.option_index() == 0 && m.options()[0] == "report_st_ok", "the row is OK again");
    a = post(c, get_cmd(costume));
    check(a.done && a.data["status"] == "confirmed", "and stays on the server, confirmed: " + a.data.dump().substr(0, 120));
    c.stop();
    printf("report_e2e_test --after-fixed: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

int main(int argc, char** argv) {
    std::string base, key;
    fs::path work = fs::temp_directory_path() / "report_e2e";
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--api-base")) base = argv[i + 1];
        else if (!strcmp(argv[i], "--api-key")) key = argv[i + 1];
        else if (!strcmp(argv[i], "--work")) work = argv[i + 1];
    }
    bool after_fixed = false;
    for (int i = 1; i < argc; ++i) if (!strcmp(argv[i], "--after-fixed")) after_fixed = true;
    if (base.rfind("http://127.0.0.1:", 0) != 0 && base.rfind("http://localhost:", 0) != 0) {
        printf("report_e2e_test: only a server on this machine; got '%s'\n", base.c_str());
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::error_code ec;
    if (after_fixed) return second_half(base, key, work);
    fs::remove_all(work, ec);
    const fs::path root = work / "game";

    // a picture with something in it, as the capture makes one
    const int w = 1280, h = 720;
    std::vector<uint8_t> bgr((size_t)w * h * 3);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        uint8_t* p = bgr.data() + ((size_t)y * w + x) * 3;
        p[0] = (uint8_t)(x * 255 / w); p[1] = (uint8_t)(y * 255 / h); p[2] = (uint8_t)(((x / 80 + y / 80) & 1) ? 230 : 40);
    }
    rp::Snapshot snap;
    std::string why;
    const bool encoded = report::capture::encode_jpeg(bgr.data(), w, h, (size_t)w * 3, rp::kJpegMaxBytes, &snap.jpeg, &why);
    check(encoded, "a real JPEG: " + std::to_string(snap.jpeg.size()) + " bytes " + why);

    put(root / "reframework/data/SF6_Costumes_Data/outfits.json", R"({"loader":"e2e-loader","outfits":[
      {"fighter":30,"fighter_dir":"esf030","costume_no":5,"name":"Outfit I","based_on":0,"label":"Swimsuit <b>evil</b>","kind":"archive","source":"C. Viper/Swimsuit 1.0.zip"}]})");
    put(root / "reframework/data/SF6_Costumes_Data/registry.json", R"({"slots":[{"mod_id":"402b1ac9c4dd7fb1","fighter":30,"original_costume_no":0,"new_costume_no":5}]})");
    put(root / "SF6_CostumeLoader.log", "[start] read C:\\Users\\SomeOne\\AppData\\x\nsteam 76561198012345678 <script>alert(1)</script>\n");
    put(root / "reframework/data/SF6_CostumeSlots_data/native_log.txt", std::string(300 * 1024, 'p') + "\nlast line\n");
    put(root / "hook_trace.txt", "MUST_NOT_LEAVE");

    snap.fighter = 30; snap.character = "C. Viper"; snap.outfit_row = "Outfit I"; snap.colour_row = "Color 3";
    snap.text = "Le col d\xC3\xA9passe <img src=x onerror=alert(1)> \xE6\xBC\xA2\xE5\xAD\x97\nsecond line";
    snap.reporter_version = "e2e"; snap.core_version = "0.9.6";
    rp::Scrub scrub;
    scrub.names = { "SomeOne" };
    scrub.game_root = root.string();
    const rp::Report r = rp::gather(snap, root, scrub);
    check(r.costume_key == "mod:402b1ac9c4dd7fb1:f30:c0", "the costume key: " + r.costume_key);

    const fs::path cfg = work / "config.json";
    put(cfg, key.empty() ? json({ {"api_base", base} }).dump() : json({ {"api_base", base}, {"api_key", key} }).dump());   // the shipped config: no key
    FakeTickets tickets;
    community::Client c;
    community::set_client_version("e2e");
    std::string cfg_path = cfg.string();
    for (char& ch : cfg_path) if (ch == '\\') ch = '/';
    c.configure(cfg_path);
    c.worker().set_tickets(&tickets);
    c.start();
    // The menu opens: its one request, the key alone, before anything was ever sent
    Answer a = post(c, get_cmd(r.costume_key));
    check(a.done && a.ok && a.data["ok"] == true && a.data["exists"] == false && !a.data.contains("text"), "no report on this costume yet: " + a.data.dump());
    rp::Session menu;
    menu.begin(); menu.shot_settled();
    menu.server_answer(rp::server_from(a.data["exists"] == true, ""), "");
    check(menu.server() == rp::Server::None && menu.pick() == rp::Pick::Ok, "the menu: OK");
    a = post(c, rp::make_command(r));
    check(a.done && a.ok && a.data.contains("ok") && a.data["ok"] == true && a.data["replaced"] == false, "first report: accepted, new: " + a.data.dump());
    check(fs::exists(community::auth::token_path(cfg.string())), "the token came from the (fake) Steam ticket and was kept");
    rp::Report again = r;
    again.text = "the second one replaces the first <img src=x onerror=\"document.title='XSS'\"> \xC3\xA9\xE6\xBC\xA2\nits second line";
    a = post(c, rp::make_command(again));
    check(a.done && a.ok && a.data["replaced"] == true, "same costume: replaced: " + a.data.dump());
    rp::Report nopic = r;
    nopic.costume_key = "game:30:0123456789abcdef";
    nopic.outfit = "Outfit 2";
    nopic.jpeg.clear();
    nopic.notes.push_back("no picture: the copy of the frame faulted");
    a = post(c, rp::make_command(nopic));
    check(a.done && a.ok && a.data["replaced"] == false, "a game outfit, no picture: a new report: " + a.data.dump());
    json bad = rp::make_command(r);
    bad["image"] = "PGh0bWw+PHNjcmlwdD5hbGVydCgxKTwvc2NyaXB0PjwvaHRtbD4=";
    a = post(c, bad);
    check(a.done && community::auth::error_code(a.data) == "bad_image", "a picture that is no JPEG: refused: " + a.data.dump());
    // The next opening: the text comes back, Update; marking it fixed, and what the player may not do
    a = post(c, get_cmd(r.costume_key));
    check(a.done && a.ok && a.data["exists"] == true && a.data["status"] == "waiting" && a.data["text"].get<std::string>() == again.text, "the report comes back, its text whole: " + a.data.dump().substr(0, 120));
    rp::Session back;
    back.begin(); back.shot_settled();
    back.server_answer(rp::server_from(true, a.data["status"].get<std::string>()), a.data["text"].get<std::string>());
    check(back.server() == rp::Server::Waiting && back.has_report() && back.text() == again.text, "the menu: WAITING FOR FIX, Update");
    check(back.update_blocked() && !back.send(), "the menu: Update does nothing until the text differs");
    a = post(c, status_cmd(nopic.costume_key, rp::action_confirm()));
    check(a.done && community::auth::error_code(a.data) == "bad_transition", "WAITING FOR FIX: the player cannot move it: " + a.data.dump());
    a = post(c, get_cmd(nopic.costume_key));
    check(a.done && a.data["status"] == "waiting", "it still waits: " + a.data.dump().substr(0, 120));
    a = post(c, status_cmd(nopic.costume_key, "fixed"));
    check(a.done && community::auth::error_code(a.data) == "bad_action", "the player can never set fixed: " + a.data.dump());
    c.stop();
    printf("report_e2e_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
