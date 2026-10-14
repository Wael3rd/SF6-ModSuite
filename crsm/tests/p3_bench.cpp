// p3_bench — offline bench of phase P3 (the layers files, replay, reversal, record), no game, no user data.
//
//   p3_bench --work <dir>
//
// P3a (files): the name cut on a character boundary, on model::files::Engine with a fake game in a scratch folder.
// P3b-P3d: the pure parts of replay, reversal and record (p3_tests.cpp), then the record and replay layers sharing slot 8
// through the real files engine. (The comparison of the files layer with the v1.6 Lua on copies of user data is not part
// of this tree.)
// Exit code 0 only when every check passed.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "model/chars.hpp"
#include "model/files.hpp"
#include "model/luajson.hpp"
#include "model/lutil.hpp"
#include "model/record.hpp"
#include "model/replay.hpp"
#include "model/store.hpp"
#include "model/timeline.hpp"
#include "model/transfer.hpp"
#include "model/vslots.hpp"
#include "p3_tests.hpp"

namespace fs = std::filesystem;
namespace lj = model::luajson;
using json = nlohmann::json;

namespace p3 {

// ---- reporting ------------------------------------------------------------------------------------------------------------
struct Group { int checks = 0; int fails = 0; };
std::map<std::string, Group> g_groups;
std::vector<std::string> g_order;
std::string g_group;
int g_shown = 0;

void begin(const std::string& name) {
    g_group = name;
    g_shown = 0;
    if (!g_groups.count(name)) g_order.push_back(name);
}

void check(bool ok, const std::string& what, const std::function<std::string()>& detail) {
    Group& g = g_groups[g_group];
    ++g.checks;
    if (ok) return;
    ++g.fails;
    if (g_shown++ < 10) {
        std::printf("  FAIL [%s] %s\n", g_group.c_str(), what.c_str());
        if (detail) std::printf("       %s\n", detail().c_str());
    }
}

}  // namespace p3

namespace {

using p3::begin;
using p3::check;

// ---- the fake game (the game objects as the v1.6 Lua sees them) -------------------------------------------------------------
struct FakeSlot {
    bool active = false, valid = false;
    int64_t weight = 1, frame = 0;
    bool input = true;                 // InputData present
    bool has_buff = false;
    std::vector<uint16_t> buff;
    int64_t num = 0, cursor = 0;
    bool no_alloc = false;             // the direct allocation is refused (the Lua's InputData refuses `buff`)
};

struct FakeGame {
    int fid = -1, select_id = -1;
    FakeSlot slots[8];
    std::vector<std::string> log;
    int rec_slot = -1;
};

class FakeLive final : public model::files::LiveSlots {
public:
    explicit FakeLive(FakeGame* g) : g_(g) {}
    std::string access(int fid) override {
        if (fid == -1) return "Unknown Character ID";
        if (fid != g_->fid) return "Data not found for ID " + std::to_string(fid);
        return "";
    }
    int read(int, int k0, model::files::LiveRead* out) override {
        const FakeSlot& s = g_->slots[k0];
        out->valid = s.valid;
        out->frames = s.frame;
        out->weight = s.weight;
        out->values.clear();
        if (s.valid && s.frame > 0) {
            if (!s.input) return 1;
            if (!s.has_buff || s.frame > (int64_t)s.buff.size()) return 2;
            out->values.assign(s.buff.begin(), s.buff.begin() + (ptrdiff_t)s.frame);
        }
        return 0;
    }
    int64_t capacity(int, int k0) override {
        const FakeSlot& s = g_->slots[k0];
        if (!s.input) return -1;
        return s.has_buff ? (int64_t)s.buff.size() : 0;
    }
    bool allocate(int, int k0, int64_t needed) override {
        FakeSlot& s = g_->slots[k0];
        if (!s.input || s.no_alloc) return false;
        s.buff.assign((size_t)std::max<int64_t>(model::files::kRecordBufferLen, needed), 0);
        s.has_buff = true;
        s.cursor = 0;
        return (int64_t)s.buff.size() >= needed;
    }
    bool set_weight(int, int k0, const json& w) override {
        if (!w.is_number()) return false;
        g_->slots[k0].weight = (int64_t)w.get<double>();
        return true;
    }
    bool write_empty(int, int k0) override {
        FakeSlot& s = g_->slots[k0];
        s.valid = false; s.frame = 0; s.active = false;
        if (!s.input) return false;
        s.num = 0;
        return true;
    }
    bool write_values(int, int k0, const uint16_t* v, size_t n) override {
        FakeSlot& s = g_->slots[k0];
        if (!s.input || !s.has_buff) return false;
        const size_t m = std::min(n, s.buff.size());
        std::copy(v, v + m, s.buff.begin());
        return n <= s.buff.size();
    }
    bool finish(int, int k0, int64_t needed) override {
        FakeSlot& s = g_->slots[k0];
        s.valid = true; s.frame = needed; s.active = false; s.num = needed;
        return true;
    }
    bool activate_valid(int) override {
        for (FakeSlot& s : g_->slots) if (s.valid) s.active = true;
        return true;
    }
private:
    FakeGame* g_;
};

class FakeClock final : public model::files::Clock {
public:
    double t = 0;
    std::tm when{};
    double seconds() override { return t; }
    std::tm local_now() override { return when; }
};

// ---- the cases ----------------------------------------------------------------------------------------------------------------
void initial_slots(FakeGame& g, int variant) {
    for (int k0 = 0; k0 < 8; ++k0) {
        FakeSlot& s = g.slots[k0];
        s = FakeSlot{};
        s.valid = (k0 % 3) != 2;
        s.active = (k0 % 2) == 0;
        s.weight = 1 + k0 % 3;
        s.frame = s.valid ? 20 + 7 * k0 : 0;
        s.input = true;
        s.has_buff = true;
        s.buff.assign(3600, 0);
        for (int64_t f = 0; f < s.frame; ++f) s.buff[(size_t)f] = (uint16_t)((k0 * 5 + (f / 9)) & 0x3FF);
        s.num = s.frame;
    }
    if (variant == 1) {                         // the ALLOC path: slot 3 has no buffer and refuses the direct allocation
        FakeSlot& s = g.slots[2];
        s.has_buff = false; s.buff.clear(); s.no_alloc = true;
    }
    if (variant == 2) {                         // a slot without InputData: the Lua raises on a non-empty entry
        FakeSlot& s = g.slots[4];
        s.input = false; s.has_buff = false; s.buff.clear(); s.valid = false; s.frame = 0;
    }
}

json entry_from(int id, int frames, uint16_t base, const std::string& name) {
    std::vector<uint16_t> v;
    for (int f = 0; f < frames; ++f) v.push_back((uint16_t)((base + f / 5) & 0x3FF));
    json e;
    e["id"] = id;
    e["empty"] = false;
    json tl = json::array();
    for (auto& s : model::timeline::compress(v.data(), v.size())) tl.push_back(s);
    e["timeline"] = tl;
    e["weight"] = 2;
    if (!name.empty()) e["name"] = name;
    return e;
}

// The name cut in the engine: a name cut inside a UTF-8 character is cut on its boundary and the files stay valid.
void p3a_fix(const fs::path& work) {
    begin("P3a files: name cut on a character boundary");
    const fs::path root = work / "p3a_fix";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SF6_RecordingSlotManager_data" / "Ryu", ec);
    FakeGame g; g.fid = 1; initial_slots(g, 0);
    FakeLive live(&g);
    FakeClock clock;
    model::files::Engine e(root, &live, &clock);
    e.ensure(1);
    std::string name = "a";
    for (int i = 0; i < 30; ++i) name += "\xE5\x90\x8D";                  // 1 + 90 bytes: the Lua's sub(1, 64) splits a character
    e.name_slot(2, name);
    e.name_slot(17, name);
    const json n2 = e.slot_name(1);
    check(n2.is_string() && n2.get<std::string>().size() == 64 && json::accept(json(n2).dump()), "slot 2's name: 64 bytes, valid UTF-8", [&] { return n2.dump(); });
    const std::string v = e.vslots().name(17);
    check(v.size() == 64, "slot 17's name: 64 bytes", [&] { return std::to_string(v.size()); });
    std::string t;
    const std::string res = e.export_as(json("Cut"), &t);
    check(res.rfind("Custom Saved", 0) == 0, "the pack with the cut name is written", [&] { return res; });
    const json pack = lj::load_file(root / "SF6_RecordingSlotManager_data" / "Ryu" / "Ryu_Cut.json");
    check(pack.is_array() && pack.size() == 30 && pack[1]["name"] == n2, "the pack holds the cut name", [&] { return pack.dump().substr(0, 200); });
    const json vs = lj::load_file(root / "SF6_RecordingSlotManager_data" / "Ryu" / "_virtual_slots.json");
    check(vs.is_object() && vs["17"]["name"].get<std::string>() == v, "_virtual_slots.json holds the cut name", nullptr);
}

// ---- P3d on the real engines: the record and replay layers share slot 8 through the files engine ----------------------------
// Wael 14/10: "a slot > 8 I record takes the name and the random selection rate of another slot > 8". The calls in the
// DLL's order (ui/windows.cpp, mech/layer_record.cpp, mech/layer_replay.cpp): the F on the Record row (replay unload, arm)
// with the menu open, the menu close (the longest checked virtual into slot 8, named after it by MAIN's apply), the game
// recording slot 8, the finalize (menu opened while recording, or the record state's end).
class ReplayOnFiles final : public model::replay::Port {
public:
    ReplayOnFiles(FakeGame* g, model::files::Engine* e) : g_(g), e_(e) {}
    bool live(int k0, model::replay::Live* out) override {
        const FakeSlot& s = g_->slots[k0];
        out->present = true; out->active = s.active; out->valid = s.valid; out->weight = s.weight;
        return true;
    }
    void set_active(int k0, bool on) override { g_->slots[k0].active = on; }
    std::optional<json> read8() override { std::string err; return e_->read_slot(model::replay::kWorkSlot, &err); }
    void write8(const json& x) override { e_->write_slot(x); }
    bool install() override { on_ = true; return true; }
    bool installed() override { return on_; }
    bool entry(int n, bool* a, int64_t* w) override {           // the 22 entries as the files hold them (no Replay page edit)
        const json* x = e_->vslots().get(n);
        if (!on_ || !x) return false;
        *a = x->contains("active") && (*x)["active"] == json(true) && e_->vslots().frames(n) > 0;
        *w = (int64_t)e_->vslots().weight(n);
        return true;
    }
    void entry_set_active(int, bool) override {}
    void uninstall() override { on_ = false; }
    void sync(int) override {}
private:
    FakeGame* g_;
    model::files::Engine* e_;
    bool on_ = false;
};

class RecordOnFiles final : public model::record::Port {
public:
    RecordOnFiles(model::files::Engine* e, model::replay::Engine* rp) : e_(e), rp_(rp) {}
    std::optional<json> read8() override { std::string err; return e_->read_slot(model::record::kWorkSlot, &err); }
    void write8(const json& x) override { e_->write_slot(x); }
    const json* vslot(int n) override { return e_->vslots().get(n); }
    void store(int n, const json& x) override { e_->vslot_put(n, x, true); }
    int64_t frames(const json& x) override { model::vslots::VSlots v; v.put(9, x); return v.frames(9); }
    void own8_back() override { rp_->own8_back(); }            // mech::record -> mech::replay::own8_back
private:
    model::files::Engine* e_;
    model::replay::Engine* rp_;
};

void p3d_record_meta(const fs::path& work) {
    begin("P3d record on the files / replay engines: only the recording moves into n (14/10)");
    const fs::path root = work / "p3d_meta";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SF6_RecordingSlotManager_data" / "Ryu", ec);
    FakeGame g; g.fid = 1; initial_slots(g, 0);                 // slot 8: 69 frames, weight 2; slots 1, 3, 5, 7 active
    FakeLive live(&g);
    FakeClock clock;
    model::files::Engine e(root, &live, &clock);
    e.ensure(1);
    e.name_slot(8, json("Own eight"));
    json m = entry_from(12, 60, 300, "Combo M"); m["weight"] = 3; m["active"] = true;
    e.vslot_put(12, m, true);                                   // the checked virtual the menu close puts into slot 8
    json u = entry_from(21, 10, 50, ""); u["weight"] = 5;
    e.vslot_put(21, u, true);                                   // recorded before, never named
    ReplayOnFiles rpp(&g, &e);
    model::replay::Engine rp;
    RecordOnFiles rcp(&e, &rp);
    model::record::Engine rc;
    uint64_t t = 1000;
    auto vs = [&]() -> const model::vslots::VSlots& { return e.vslots(); };
    auto patch = [&](const json& x) { e.vslots_active_weight(x); };
    const json own8 = *e.read_slot(7, nullptr);
    rp.menu_close(rpp, vs, patch);                              // in the fight: virtual 12 rides slot 8
    check(rp.loaded() == 12 && e.slot_name(7) == json("Combo M"), "before: virtual 12 rides slot 8 under its name", [&] { return e.slot_name(7).dump(); });
    // one recording into n; returns the timeline the game recorded
    auto record_into = [&](int n, bool stop_in_menu, uint16_t base) {
        rc.menu_open(rcp); rp.menu_open(rpp, e.vslots());       // the player pauses (CreateViewData pre)
        rp.unload_if_loaded(rpp); rc.arm(rcp, n, t);            // F on the Record row showing n
        rp.menu_close(rpp, vs, patch);                          // the menu closes: the countdown
        rc.tick(rcp, t += model::record::kTickEvery, 0, false);
        FakeSlot& s = g.slots[7];                               // the game records slot 8
        s.valid = true; s.frame = 40; s.num = 40;
        std::vector<uint16_t> v;
        for (int f = 0; f < 40; ++f) { v.push_back((uint16_t)((base + f / 6) & 0x3FF)); s.buff[(size_t)f] = v.back(); }
        rc.tick(rcp, t += model::record::kTickEvery, 3, false);
        if (stop_in_menu) { rc.menu_open(rcp); rp.menu_open(rpp, e.vslots()); rp.menu_close(rpp, vs, patch); }
        else rc.tick(rcp, t += model::record::kTickEvery, 0, false);
        json tl = json::array();
        for (auto& x : model::timeline::compress(v.data(), v.size())) tl.push_back(x);
        return tl;
    };
    const json m_before = *e.vslots().get(12);
    auto own8_kept = [&](const std::string& when) {             // slot 8's own: in place, or kept aside while a virtual rides it
        const json own = rp.loaded() ? rp.snapshot() : *e.read_slot(7, nullptr);
        check(own["timeline"] == own8["timeline"] && own["name"] == "Own eight" && g.slots[7].weight == 2,
              "slot 8: its own recording, name and weight (" + when + ")", [&] { return own.dump().substr(0, 200) + " w " + std::to_string(g.slots[7].weight); });
    };
    auto others_kept = [&](const char* when) {
        const json* x = e.vslots().get(12);
        check(x && *x == m_before, std::string("virtual 12 untouched (") + when + ")", [&] { return x ? x->dump() : "nil"; });
        own8_kept(when);
    };

    const json tl20 = record_into(20, true, 700);               // a new slot, the menu opened to stop
    const json* s20 = e.vslots().get(20);
    check(s20 && (*s20)["timeline"] == tl20 && (*s20)["id"] == 20 && (*s20)["empty"] == false, "slot 20 holds the recording", [&] { return s20 ? s20->dump() : "nil"; });
    check(s20 && !s20->contains("name"), "slot 20: no name (not the name of virtual 12 riding slot 8)", [&] { return s20 ? s20->dump() : "nil"; });
    check(s20 && (*s20)["weight"] == 1 && (!s20->contains("active") || (*s20)["active"] == false), "slot 20: a new slot's weight 1, not checked (not slot 8's weight 2)",
          [&] { return s20 ? s20->dump() : "nil"; });
    const json file = lj::load_file(root / "SF6_RecordingSlotManager_data" / "Ryu" / "_virtual_slots.json");
    check(file.is_object() && file.contains("20") && !file["20"].contains("name") && file["20"]["weight"] == 1, "_virtual_slots.json: slot 20 unnamed, weight 1",
          [&] { return file.dump().substr(0, 240); });
    others_kept("after 20");

    const json tl21 = record_into(21, false, 800);              // an unnamed slot, the recording stopped in the fight
    const json* s21 = e.vslots().get(21);
    check(s21 && (*s21)["timeline"] == tl21 && !s21->contains("name") && (*s21)["weight"] == 5, "slot 21: the recording, still unnamed, its own weight 5",
          [&] { return s21 ? s21->dump() : "nil"; });
    others_kept("after 21");

    e.name_slot(22, json("Mine"));                              // named, never recorded (an empty entry)
    const json tl22 = record_into(22, true, 900);
    const json* s22 = e.vslots().get(22);
    check(s22 && (*s22)["timeline"] == tl22 && (*s22)["name"] == "Mine" && (*s22)["weight"] == 1 && (*s22)["empty"] == false, "slot 22: the recording under its own name",
          [&] { return s22 ? s22->dump() : "nil"; });
    others_kept("after 22");

    const json tl12 = record_into(12, true, 950);               // the virtual riding slot 8 recorded again
    const json* s12 = e.vslots().get(12);
    check(s12 && (*s12)["timeline"] == tl12 && (*s12)["name"] == "Combo M" && (*s12)["weight"] == 3 && (*s12)["active"] == true,
          "slot 12 recorded again: its name, weight and On kept", [&] { return s12 ? s12->dump() : "nil"; });
    own8_kept("after 12");
}

// Wael 14/10 (second report): a virtual recording that ends on its own (the menu not opened) puts slot 8's own content back
// while the replay layer still held "virtual 12 in slot 8" (loaded at the menu close after the F): a draw of 12 then played
// slot 8's own recording, the HUD saying "Slot 12".
void p3d_record_end_slot8(const fs::path& work) {
    begin("P3d record ended on its own: the replay layer's slot 8 follows (14/10)");
    const fs::path root = work / "p3d_end8";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SF6_RecordingSlotManager_data" / "Ryu", ec);
    FakeGame g; g.fid = 1; initial_slots(g, 0);
    FakeLive live(&g);
    FakeClock clock;
    model::files::Engine e(root, &live, &clock);
    e.ensure(1);
    json m = entry_from(12, 60, 300, "Combo M"); m["active"] = true;
    e.vslot_put(12, m, true);                                   // the one ticked virtual
    model::replay::Engine rp;
    ReplayOnFiles rpp(&g, &e);
    RecordOnFiles rcp(&e, &rp);
    model::record::Engine rc;
    auto vs = [&]() -> const model::vslots::VSlots& { return e.vslots(); };
    auto patch = [&](const json& x) { e.vslots_active_weight(x); };
    uint64_t t = 1000;
    rp.menu_close(rpp, vs, patch);
    rc.menu_open(rcp); rp.menu_open(rpp, e.vslots());           // pause
    rp.unload_if_loaded(rpp); rc.arm(rcp, 20, t);               // F on the Record row showing 20
    rp.menu_close(rpp, vs, patch);                              // unpause: virtual 12 into slot 8, the countdown
    check(rp.loaded() == 12, "the menu close put virtual 12 into slot 8 during the countdown");
    rc.tick(rcp, t += model::record::kTickEvery, 0, false);
    FakeSlot& s8 = g.slots[7];                                  // the game records slot 8
    s8.valid = true; s8.frame = 40; s8.num = 40;
    for (int f = 0; f < 40; ++f) s8.buff[(size_t)f] = (uint16_t)((600 + f / 6) & 0x3FF);
    rc.tick(rcp, t += model::record::kTickEvery, 3, false);
    const bool fin = rc.tick(rcp, t += model::record::kTickEvery, 0, false);   // the recording ends on its own
    const json own = *e.read_slot(7, nullptr);
    check(fin && e.vslots().get(20) && own["timeline"] != m["timeline"], "finalized: 20 stored, slot 8 has its own recording back");
    check(rp.label(true, 7) == 0, "HUD while slot 8 replays now: Slot 8 (its own recording is in it)", [&] { return std::to_string(rp.label(true, 7)); });
    for (int k0 = 0; k0 < 7; ++k0) g.slots[k0].active = false;  // only virtual 12 to draw
    model::draw::MtRng rng(7);
    model::replay::Draw d;
    const bool drew = rp.pre_draw(rpp, e.vslots(), rng, &d);
    const json now8 = *e.read_slot(7, nullptr);
    check(drew && d.n == 12 && now8["timeline"] == m["timeline"], "a draw of 12 plays 12's recording (slot 8 loaded again)",
          [&] { return std::to_string(d.n) + " " + now8["timeline"].dump().substr(0, 120); });
    check(rp.label(true, 7) == 12, "HUD: Slot 12 while 12's recording is in slot 8", [&] { return std::to_string(rp.label(true, 7)); });
    rp.post_draw(rpp, d);
    rc.menu_open(rcp); rp.menu_open(rpp, e.vslots());           // the next pause: slot 8's own recording back
    const json back = *e.read_slot(7, nullptr);
    check(rp.loaded() == 0 && back["timeline"] == own["timeline"], "the next menu open: slot 8 holds its own recording");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fs::path work;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--work" && i + 1 < argc) work = argv[++i];
    }
    if (work.empty()) {
        std::fprintf(stderr, "usage: p3_bench --work <dir>\n");
        return 2;
    }
    work = fs::absolute(work).lexically_normal().make_preferred();
    fs::create_directories(work);
    std::printf("[P3a] files\n");
    p3a_fix(work);
    p3::run_pure_tests();
    p3d_record_meta(work);
    p3d_record_end_slot8(work);

    int checks = 0, fails = 0;
    std::printf("\n%-60s %8s %8s\n", "group", "checks", "failed");
    for (const std::string& n : p3::g_order) {
        const p3::Group& g = p3::g_groups[n];
        std::printf("%-60s %8d %8d\n", n.c_str(), g.checks, g.fails);
        checks += g.checks; fails += g.fails;
    }
    std::printf("%-60s %8d %8d\n", "TOTAL", checks, fails);
    std::printf("\n%s\n", fails == 0 ? "ALL GREEN" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
