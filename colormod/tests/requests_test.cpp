// engine/requests.hpp: other mods' requests (scopes, the replay's battle, the battle sides, housekeeping). Build: build_requests_test.bat
#include <cstdio>

#include "../src/engine/requests.hpp"

using namespace engine::requests;

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-90s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

int main() {
    Table t;
    const uintptr_t replay = 0x1000, other = 0x2000;
    // refusals
    check(!t.set(0, 1, 8, "x", 0, 0) && !t.set(5, 1, 8, "x", 0, 0), "unknown scopes refused");
    check(!t.set(kReplay, 1, 8, "x", 0, 0), "a replay request without its battle refused");
    check(!t.set(kPreview, -1, 8, "x", 0, 0) && !t.set(kPreview, 1, -1, "x", 0, 0), "bad fighter / costume refused");
    // the preview
    check(t.set(kPreview, 1, 8, "..\\color_mods\\Ryu\\MANUALCOLORS_01\\esf001_009_cmd_001.user.2", 0, 10), "a preview request");
    const Request* p = t.find(kPreview, 1, 8, 0);
    check(p && p->id.find("esf001_009_cmd_001") != std::string::npos, "the preview request applies, outside any battle");
    check(!t.find(kPreview, 1, 7, 0) && !t.find(kPreview, 2, 8, 0), "only to that fighter / costume");
    check(t.set(kPreview, 1, 8, "", 0, 11) && t.find(kPreview, 1, 8, 0)->id.empty(), "a new request replaces it: the game's own colours");
    t.clear(kPreview);
    check(!t.any(kPreview) && !t.find(kPreview, 1, 8, 0), "cleared");
    // the replay
    check(t.set(kReplay, 1, 8, "mc1", replay, 100) && t.set(kReplay, 21, 0, "", replay, 100), "two replay requests (both sides)");
    check(!t.find(kReplay, 1, 8, 0) && !t.find(kReplay, 1, 8, other), "not applied while another flow map (or none) runs");
    check(t.find(kReplay, 1, 8, replay) && t.find(kReplay, 21, 0, replay)->id.empty(), "applied while its replay runs");
    t.tick(replay, 200, 600);
    check(t.size(kReplay) == 2, "kept while its replay runs");
    t.tick(other, 300, 600);
    check(t.size(kReplay) == 0, "the replay over (another flow map runs): dropped");
    check(t.set(kReplay, 1, 8, "mc1", replay, 1000), "a request for a replay not started yet");
    t.tick(0, 1500, 600);
    check(t.size(kReplay) == 1, "kept while its replay has not started (within the timeout)");
    t.tick(0, 1700, 600);
    check(t.size(kReplay) == 0, "dropped when its replay never ran within the timeout");
    check(t.set(kReplay, 1, 8, "a", replay, 2000) && t.set(kReplay, 2, 0, "b", other, 2000) && t.size(kReplay) == 1 &&
          t.find(kReplay, 2, 0, other), "a request for another battle drops the previous battle's requests");
    check(!t.any(kPreview), "scopes are separate");
    // the battle sides (the Slots Loader's Random colour): no battle pointer, kept until replaced or cleared
    check(t.set(kBattle1, 1, 8, "rw2", 0, 3000) && t.set(kBattle2, 1, 8, "mc1", 0, 3000), "a request per battle side, no battle needed");
    check(t.find(kBattle1, 1, 8, 0)->id == "rw2" && t.find(kBattle2, 1, 8, 0)->id == "mc1", "each side its own (a mirror match: two draws)");
    check(!t.find(kBattle1, 1, 7, 0) && !t.find(kBattle1, 2, 8, 0), "only to that fighter / costume");
    t.tick(other, 9000, 600);
    check(t.size(kBattle1) == 1 && t.size(kBattle2) == 1, "the replay housekeeping leaves them alone");
    check(t.set(kBattle1, 1, 9, "rw1", 0, 9100) && t.size(kBattle1) == 2, "another outfit of that side: kept next to it");
    t.clear(kBattle1);
    check(!t.any(kBattle1) && t.any(kBattle2), "cleared side by side");
    std::printf(g_fail ? "%d FAILED\n" : "all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
