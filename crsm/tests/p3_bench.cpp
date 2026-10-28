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
#include "model/pack.hpp"
#include "model/replay.hpp"
#include "model/setup.hpp"
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
        out->active = s.active;
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
    bool set_active(int, int k0, bool on) override { g_->slots[k0].active = on; return true; }
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

// ---- P3a: the pack's `active` flags and training block (docs/PACK_FORMAT.md) -----------------------------------------------
// The game's reversal rows and replay settings behind the port, as the DLL's GameSetup gives them (pack terms).
class FakeSetup final : public model::files::SetupPort {
public:
    model::setup::Setup now;
    int reads = 0, writes = 0;
    bool read(int, model::setup::Setup* out) override { ++reads; *out = now; return now.playback || now.reversal || now.dummy; }
    bool write(int, const model::setup::Setup& s, std::string* report) override {
        ++writes;
        if (s.playback) now.playback = s.playback;
        if (s.dummy) {                                          // field by field: an absent field keeps the game's
            if (!now.dummy) now.dummy = model::setup::Dummy{};
            for (size_t i = 0; i < s.dummy->v.size(); ++i) if (s.dummy->v[i]) now.dummy->v[i] = s.dummy->v[i];
        }
        if (s.reversal) {                                       // the layer writes all 30 rows: the missing ones as default
            model::setup::Rows r = *s.reversal;
            for (auto& c : r) c.resize(model::setup::kRowsTotal);
            now.reversal = r;
        }
        if (report) *report = "fake";
        return true;
    }
};

class FakeAlloc final : public model::files::AllocEnv {
public:
    explicit FakeAlloc(FakeGame* g) : g_(g) {}
    bool start_recording(int slot) override {                   // the recording gives the slot its buffer
        FakeSlot& s = g_->slots[slot];
        s.buff.assign(3600, 0); s.has_buff = true; s.no_alloc = false;
        return true;
    }
    void stop_phase1() override {}
    void stop_phase2() override {}
private:
    FakeGame* g_;
};

model::setup::Rows rows_of(std::initializer_list<std::pair<int, model::store::RdRow>> down, int len = 30) {
    model::setup::Rows r;
    for (auto& c : r) c.resize((size_t)len);
    for (const auto& [k, row] : down) r[0][(size_t)k] = row;
    return r;
}

model::store::RdRow rec_row(int64_t slot0, bool active) {
    model::store::RdRow r;
    r.Type = model::setup::kRecording; r.SkillIndex = slot0; r.IsValid = true; r.IsActive = active;
    return r;
}

bool same_row(const model::store::RdRow& a, const model::store::RdRow& b) {
    return model::store::rd_to_json(a) == model::store::rd_to_json(b);
}

bool same_rows(const model::setup::Rows& a, const model::setup::Rows& b) {
    for (size_t rt = 0; rt < 3; ++rt) {
        const size_t n = std::max(a[rt].size(), b[rt].size());
        for (size_t k = 0; k < n; ++k) {
            const model::store::RdRow x = k < a[rt].size() ? a[rt][k] : model::store::RdRow{};
            const model::store::RdRow y = k < b[rt].size() ? b[rt][k] : model::store::RdRow{};
            if (!same_row(x, y)) return false;
        }
    }
    return true;
}

void select_pack(model::files::Engine& e, const std::string& file) {   // by file name (Save As names the display "Drill")
    for (size_t i = 2; i <= e.dropdown().size(); ++i) {
        e.set_dropdown_index(json((int)i));
        if (e.selected_file() == file) return;
    }
}

void set_vactive(model::files::Engine& e, int n, bool on) {
    model::vslots::VSlots::Fields f;
    f.active = &on;
    e.vslots().set_virtual(n, f);
    e.vslots_save();
}

void p3a_setup_model() {
    begin("P3a pack: training block model");
    namespace st = model::setup;
    st::Setup s;
    s.playback = st::Playback{ 1, 2, 0 };
    model::store::RdRow lvl;
    lvl.Type = 3; lvl.SkillIndex = 1; lvl.Level = 3; lvl.Count = 2; lvl.Delay = 4; lvl.IsValid = true; lvl.IsFrame = true; lvl.Frame = 29;
    s.reversal = rows_of({ { 0, rec_row(0, true) }, { 3, rec_row(14, false) }, { 12, lvl } });
    const json b = st::to_json(s);
    std::string why;
    const auto back = st::from_json(b, &why);
    check(back && back->playback && back->playback->PlayType == 1 && back->playback->LoopType == 2 && back->playback->InfoType == 0,
          "round trip: playback", [&] { return b.dump(); });
    check(back && back->reversal && same_rows(*back->reversal, *s.reversal), "round trip: 3 x 30 rows", [&] { return b.dump().substr(0, 300); });
    check(b["version"] == 1 && b["reversal"]["down"].size() == 30 && b["reversal"]["down"][0]["SkillIndex"] == 0, "the block's shape",
          [&] { return b.dump().substr(0, 200); });
    json b2 = b; b2["version"] = 2;
    check(!st::from_json(b2, &why) && !why.empty(), "a block of another version is ignored", [&] { return why; });
    json b3 = b; b3.erase("version");
    check(!st::from_json(b3, &why), "a block without a version is ignored", nullptr);
    json b4 = json::object(); b4["version"] = 1; b4["reversal"] = json::object();
    b4["reversal"]["guard"] = json::array({ model::store::rd_to_json(rec_row(2, true)) });
    const auto s4 = st::from_json(b4, &why);
    check(s4 && s4->reversal && (*s4->reversal)[1].size() == 10 && (*s4->reversal)[0].size() == 10 && same_row((*s4->reversal)[1][0], rec_row(2, true))
          && !s4->playback, "short categories padded to 10 default rows, no playback part", nullptr);
    json b5 = b4; b5["reversal"]["down"] = json::array();
    for (int i = 0; i < 40; ++i) b5["reversal"]["down"].push_back(json::object({ { "Count", 1 } }));
    const auto s5 = st::from_json(b5, &why);
    check(s5 && (*s5->reversal)[0].size() == 30, "rows past 30 ignored", nullptr);

    // pack terms <-> the game + map
    st::GameRow g = st::to_game(rec_row(14, true));
    check(g.virt == 15 && g.row.SkillIndex == st::kWork && g.row.Type == st::kRecording && g.row.IsActive, "virtual slot 15: SkillIndex 7 + map 15", nullptr);
    g = st::to_game(rec_row(3, false));
    check(g.virt == 0 && g.row.SkillIndex == 3, "live slot 4: as is, no map", nullptr);
    g = st::to_game(rec_row(30, true));
    check(st::is_default(g.row) && g.virt == 0, "a RECORDING row past slot 30: a default row", nullptr);
    g = st::to_game(rec_row(-1, true));
    check(st::is_default(g.row), "a RECORDING row without a slot: a default row", nullptr);
    model::store::RdRow bad; bad.Type = 9; bad.IsValid = true;
    check(st::is_default(st::to_game(bad).row), "an unknown Type: a default row", nullptr);
    check(st::to_game(lvl).row.SkillIndex == 1 && st::to_game(lvl).virt == 0, "a move row: as is", nullptr);
    check(st::from_game(rec_row(7, true), 15).SkillIndex == 14, "from the game: RECORDING / 7 mapped to 15 -> 14", nullptr);
    check(st::from_game(rec_row(7, true), std::nullopt).SkillIndex == 7, "from the game: unmapped slot 8 stays 7", nullptr);
    check(st::from_game(lvl, 15).SkillIndex == 1, "from the game: a stale map on a move row is ignored", nullptr);

    st::Rows t = rows_of({ { 0, rec_row(0, true) } });
    st::trim(t);
    check(t[0].size() == 10 && t[2].size() == 10, "trim: rows 11-30 all default -> 10 rows", nullptr);
    t = rows_of({ { 25, rec_row(0, true) } });
    st::trim(t);
    check(t[0].size() == 30 && t[1].size() == 30, "trim: a row 11-30 in use -> 30 rows", nullptr);
    check(!st::rows_in_use(rows_of({})) && st::rows_in_use(rows_of({ { 4, rec_row(1, false) } })), "rows_in_use: a valid row", nullptr);

    // the block on a pack: entry 1 only, old readers' rule unchanged
    json pack = json::array();
    for (int id = 1; id <= 30; ++id) {
        json e = json::object(); e["id"] = id; e["empty"] = true; e["timeline"] = json::array(); e["weight"] = 1;
        pack.push_back(e);
    }
    std::swap(pack[0], pack[4]);                                // entry 1 found by its id, not its place
    check(st::stamp(pack, s), "stamp: entry 1 found", nullptr);
    int holders = 0;
    for (const json& e : pack) if (e.contains(st::kKey)) ++holders;
    check(holders == 1 && pack[4].contains(st::kKey) && pack[4]["id"] == 1, "the block rides on entry 1 alone", nullptr);
    std::string verr;
    check(model::pack::validate(lj::decode(pack), &verr), "a pack with the block passes the old validate rule (2.1.x, Lua)", [&] { return verr; });
    const auto sp = model::pack::split(pack);
    check(sp.live.size() == 8 && sp.virt.size() == 22, "split: the block stays with the live entries", nullptr);
    check(st::of_pack(sp.live, &why).has_value(), "the live part alone still carries the block (the ALLOC re-run)", [&] { return why; });
    json noone = json::array({ pack[1] });
    check(!st::stamp(noone, s) && !st::of_pack(noone, &why), "no entry 1: no block", nullptr);
    std::array<bool, 30> on{};
    on[0] = true; on[1] = true;
    pack[1]["empty"] = false; pack[1]["timeline"] = json::array({ "3f : 5" });
    st::stamp_active(pack, on);
    check(pack[1]["active"] == true && pack[4]["active"] == false && pack[0]["active"] == false, "stamp_active: an empty slot is never active", nullptr);
    check(st::active_of(pack[1]) == std::optional<bool>(true) && !st::active_of(json::object({ { "active", 1 } })), "active_of: booleans only", nullptr);
}

void p3a_setup_engine(const fs::path& work) {
    begin("P3a pack: active flags and training block on the files engine");
    const fs::path root = work / "p3a_setup";
    const fs::path ken = root / "SF6_RecordingSlotManager_data" / "Ken";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(ken, ec);
    FakeGame g; g.fid = 10; initial_slots(g, 0);
    FakeLive live(&g);
    FakeClock clock;
    clock.when.tm_year = 126; clock.when.tm_mon = 9; clock.when.tm_mday = 23;
    FakeSetup setup;
    setup.now.playback = model::setup::Playback{ 0, 2, 2 };
    setup.now.reversal = rows_of({ { 0, rec_row(0, true) }, { 1, rec_row(14, true) } });
    model::files::Engine e(root, &live, &clock);
    e.set_setup(&setup);
    e.load_settings();
    e.ensure(10);
    e.vslot_put(15, entry_from(15, 30, 40, "V15"), false);
    e.vslot_put(16, entry_from(16, 30, 50, ""), false);
    set_vactive(e, 15, true);

    // export
    std::string tk;
    const std::string res = e.export_as(json("Drill"), &tk);
    check(res.rfind("Custom Saved", 0) == 0, "export written", [&] { return res; });
    const json pack = lj::load_file(ken / "Ken_Drill.json");
    std::string verr;
    check(pack.is_array() && pack.size() == 30 && model::pack::validate(pack, &verr), "30 entries, valid for the old rule", [&] { return verr; });
    bool flags_ok = pack.is_array() && pack.size() == 30;
    for (int k0 = 0; flags_ok && k0 < 8; ++k0) flags_ok = pack[(size_t)k0]["active"] == json(g.slots[k0].active && g.slots[k0].valid);
    check(flags_ok, "slots 1-8: active = IsActive of a recorded slot", [&] { return pack.dump().substr(0, 300); });
    check(flags_ok && pack[14]["active"] == true && pack[15]["active"] == false && pack[20]["active"] == false, "slots 9-30: active from _virtual_slots.json", nullptr);
    std::string why;
    const auto blk = model::setup::of_pack(pack, &why);
    check(blk && blk->playback && blk->playback->LoopType == 2 && blk->reversal && (*blk->reversal)[0].size() == 10 &&
          same_rows(*blk->reversal, *setup.now.reversal), "entry 1 carries the playback and the 10 game rows (11-30 unused)", [&] { return why; });
    check(flags_ok && !pack[1].contains(model::setup::kKey) && !pack[14].contains(model::setup::kKey), "no other entry carries the block", nullptr);
    if (!flags_ok) return;

    // an unused reversal setup is not exported
    const model::setup::Setup keep = setup.now;
    setup.now.reversal = rows_of({});
    e.export_as(json("NoRev"), &tk);
    const auto blk2 = model::setup::of_pack(lj::load_file(ken / "Ken_NoRev.json"), &why);
    check(blk2 && blk2->playback && !blk2->reversal, "no valid reversal row: the block carries the playback only", nullptr);
    setup.now = keep;

    // import of the drill over another state
    for (FakeSlot& sl : g.slots) sl.active = !sl.active;
    set_vactive(e, 15, false);
    set_vactive(e, 16, true);
    setup.now.playback = model::setup::Playback{ 1, 0, 0 };
    setup.now.reversal = rows_of({ { 5, rec_row(3, false) }, { 15, rec_row(20, true) } });
    const model::setup::Setup before = setup.now;
    std::array<bool, 8> act_before{};
    for (int k0 = 0; k0 < 8; ++k0) act_before[(size_t)k0] = g.slots[k0].active && g.slots[k0].valid;
    select_pack(e, "Ken_Drill.json");
    check(e.selected_file() == "Ken_Drill.json", "Ken_Drill selected", [&] { return e.selected_file(); });
    clock.t += 10;
    const std::string ir = e.import_selected();
    check(ir.rfind("Loaded:", 0) == 0, "import", [&] { return ir; });
    flags_ok = true;
    for (int k0 = 0; k0 < 8; ++k0) flags_ok &= g.slots[k0].active == pack[(size_t)k0]["active"].get<bool>();
    check(flags_ok, "slots 1-8 active as the pack says", nullptr);
    check(e.vslots().get(15) && (*e.vslots().get(15))["active"] == true && e.vslots().get(16) && (*e.vslots().get(16))["active"] == false,
          "slots 9-30 active as the pack says (not the user's flags)", nullptr);
    check(setup.writes == 1 && setup.now.playback->PlayType == 0 && setup.now.playback->LoopType == 2 && setup.now.playback->InfoType == 2 &&
          same_rows(*setup.now.reversal, *keep.reversal), "the training block replaced the game's reversal rows and replay settings",
          [&] { return e.setup_report(); });

    // the backup holds what the import replaced
    json backup;
    for (const auto& f : fs::directory_iterator(ken / "Backups", ec)) backup = lj::load_file(f.path());
    const auto bblk = model::setup::of_pack(backup, &why);
    check(bblk && bblk->reversal && (*bblk->reversal)[0].size() == 30 && same_rows(*bblk->reversal, *before.reversal) &&
          bblk->playback && bblk->playback->PlayType == 1, "the pre-import backup carries the replaced setup (30 rows: row 16 was in use)", [&] { return why; });
    flags_ok = backup.is_array() && backup.size() == 8;
    for (int k0 = 0; flags_ok && k0 < 8; ++k0) flags_ok = backup[(size_t)k0]["active"] == json(act_before[(size_t)k0]);
    check(flags_ok, "the backup carries the replaced active flags", [&] { return backup.dump().substr(0, 200); });

    // a pack without the flags and the block: today's behaviour
    json old = pack;
    for (json& x : old) { x.erase("active"); x.erase(model::setup::kKey); }
    std::string derr;
    lj::dump_file(ken / "Ken_Old.json", old, &derr);
    e.invalidate();
    e.ensure(10);
    set_vactive(e, 16, true);
    for (FakeSlot& sl : g.slots) sl.active = true;
    const int w0 = setup.writes;
    select_pack(e, "Ken_Old.json");
    clock.t += 10;
    const std::string olr = e.import_selected();
    flags_ok = olr.rfind("Loaded:", 0) == 0;
    for (int k0 = 0; k0 < 8; ++k0) flags_ok &= !g.slots[k0].active;
    check(flags_ok, "an old pack: slots 1-8 written inactive, as before", [&] { return olr; });
    check(setup.writes == w0 && e.setup_report().empty(), "an old pack: the training setup untouched", nullptr);
    check((*e.vslots().get(16))["active"] == true && (*e.vslots().get(15))["active"] == true, "an old pack: slots 9-30 keep the user's flags", nullptr);

    // activate_on_load gives way to the pack's flags; import_training_setup = false skips the block
    lj::dump_file(root / model::store::config_path(), json::object({ { "activate_on_load", true }, { "import_training_setup", false } }), &derr);
    model::files::Engine e2(root, &live, &clock);
    e2.set_setup(&setup);
    e2.load_settings();
    e2.ensure(10);
    select_pack(e2, "Ken_Drill.json");
    clock.t += 10;
    const std::string r2 = e2.import_selected();
    flags_ok = r2.rfind("Loaded:", 0) == 0;
    for (int k0 = 0; k0 < 8; ++k0) flags_ok &= g.slots[k0].active == pack[(size_t)k0]["active"].get<bool>();
    check(flags_ok, "activate_on_load on: the pack's flags still win", [&] { return r2; });
    check(setup.writes == w0 && e2.setup_report().find("import_training_setup") != std::string::npos,
          "import_training_setup false: the block is skipped", [&] { return e2.setup_report(); });
    fs::remove(root / model::store::config_path(), ec);

    // the ALLOC fallback: the block is applied once, by the re-run that writes the slots
    FakeGame g3; g3.fid = 10; initial_slots(g3, 0);
    g3.slots[0].has_buff = false; g3.slots[0].buff.clear(); g3.slots[0].no_alloc = true;   // slot 1 (in the pack) has no memory
    FakeLive live3(&g3);
    FakeSetup setup3;
    model::files::Engine e3(root, &live3, &clock);
    e3.set_setup(&setup3);
    e3.load_settings();
    e3.ensure(10);
    select_pack(e3, "Ken_Drill.json");
    clock.t += 10;
    const std::string r3 = e3.import_selected();
    check(r3.rfind("Auto-Allocating", 0) == 0 && setup3.writes == 0, "ALLOC queued: nothing applied yet", [&] { return r3; });
    FakeAlloc alloc(&g3);
    for (int i = 0; i < 100 && e3.alloc_pending(); ++i) e3.alloc_tick(false, alloc);
    check(!e3.alloc_pending() && e3.status().rfind("Loaded:", 0) == 0 && setup3.writes == 1 && g3.slots[0].active == pack[0]["active"].get<bool>(),
          "WRITE_DATA: the slots, the flags and the block", [&] { return e3.status(); });
}

// ---- P3a: the dummy's settings in the training block, `info` kept by Save / Save As, the list read at each page entry ------
int dummy_index(const char* name) {
    for (int i = 0; i < model::setup::kDummyCount; ++i) if (std::string(model::setup::kDummyFields[i].name) == name) return i;
    return -1;
}

model::setup::Dummy dummy_of(std::initializer_list<std::pair<const char*, int64_t>> kv) {
    model::setup::Dummy d;
    for (const auto& [k, v] : kv) d.v[(size_t)dummy_index(k)] = v;
    return d;
}

bool same_dummy(const std::optional<model::setup::Dummy>& a, const model::setup::Dummy& b) { return a && a->v == b.v; }

void p3a_dummy_model() {
    begin("P3a pack: the dummy's settings (training.dummy) model");
    namespace st = model::setup;
    check(dummy_index("GuardType") >= 0 && dummy_index("GD_Type") >= 0 && dummy_index("IsGuardSwitching") >= 0, "field table", nullptr);
    st::Setup s;
    s.dummy = dummy_of({ { "DummyActionType", 1 }, { "GuardType", 2 }, { "IsGuardSwitching", 1 }, { "GuardCount", 29 },
                         { "GD_Type", 1 }, { "QS_Type", 1 }, { "DR_Type", 2 }, { "DR_DelayFrame", 3 },
                         { "DR_No_Weight", 1 }, { "DR_Guard_Weight", 0 }, { "DR_Getup_Weight", 2 }, { "NC_TYPE", 0 }, { "PC_TYPE", 1 } });
    const json b = st::to_json(s);
    std::string why;
    const auto back = st::from_json(b, &why);
    check(back && same_dummy(back->dummy, *s.dummy) && !back->reversal && !back->playback, "round trip: the fields given, nothing else",
          [&] { return b.dump(); });
    check(b["dummy"]["guard"]["IsGuardSwitching"] == true && b["dummy"]["defense"]["GD_Type"] == 1 && b["dummy"]["status"]["DummyActionType"] == 1 &&
          b["dummy"]["counter"]["PC_TYPE"] == 1 && !b["dummy"]["status"].contains("JumpType"), "the shape: page objects, game names, a JSON boolean",
          [&] { return b.dump(); });
    check(!st::to_json(st::Setup{}).contains("dummy"), "no dummy part when nothing is carried", nullptr);

    json bad = b;
    bad["dummy"]["guard"]["GuardType"] = 9;                     // outside TM_GS_GUARD_TYPE
    bad["dummy"]["guard"]["GuardWeight"] = 150;                 // a percentage
    bad["dummy"]["guard"]["IsGuardSwitching"] = 1;              // not a boolean
    bad["dummy"]["defense"].erase("DR_Getup_Weight");           // an incomplete weight group
    bad["dummy"]["counter"]["NH_Weight"] = 0; bad["dummy"]["counter"]["NC_Weight"] = 0; bad["dummy"]["counter"]["PC_Weight"] = 0;
    bad["dummy"]["status"] = "x";                               // a page that is not an object
    auto bb = st::from_json(bad, &why);
    check(bb && bb->dummy, "a block with bad values still read", [&] { return why; });
    if (!bb || !bb->dummy) return;
    const auto& v = bb->dummy->v;
    check(bb && bb->dummy && !v[(size_t)dummy_index("GuardType")] && !v[(size_t)dummy_index("GuardWeight")] &&
          !v[(size_t)dummy_index("IsGuardSwitching")] && v[(size_t)dummy_index("GuardCount")] == 29, "out of range / not a boolean: not taken",
          nullptr);
    check(bb && !v[(size_t)dummy_index("DR_No_Weight")] && !v[(size_t)dummy_index("DR_Guard_Weight")] && v[(size_t)dummy_index("DR_Type")] == 2,
          "an incomplete weight group: none of it taken", nullptr);
    check(bb && !v[(size_t)dummy_index("NH_Weight")] && !v[(size_t)dummy_index("PC_Weight")] && v[(size_t)dummy_index("PC_TYPE")] == 1,
          "weights all zero: not taken", nullptr);
    check(bb && !v[(size_t)dummy_index("DummyActionType")], "a page that is not an object: nothing from it", nullptr);
    json none = json::object({ { "version", 1 }, { "dummy", json::object({ { "guard", json::object({ { "GuardType", -1 } }) } }) } });
    const auto sn = st::from_json(none, &why);
    check(sn && !sn->dummy, "nothing valid: no dummy part", nullptr);

    // the block with a dummy part on a pack: the old rule still passes, the other parts unchanged
    json pack = json::array();
    for (int id = 1; id <= 30; ++id) pack.push_back(json::object({ { "id", id }, { "empty", true }, { "timeline", json::array() }, { "weight", 1 } }));
    s.playback = st::Playback{ 1, 2, 0 };
    st::stamp(pack, s);
    std::string verr;
    check(model::pack::validate(lj::decode(pack), &verr), "a pack with training.dummy passes the old validate rule", [&] { return verr; });
    const auto op = st::of_pack(pack, &why);
    check(op && op->playback && op->playback->LoopType == 2 && same_dummy(op->dummy, *s.dummy), "of_pack: playback and dummy", nullptr);
}

void p3a_dummy_info_engine(const fs::path& work) {
    begin("P3a pack: dummy settings through export / import / backup; info kept by Save and Save As");
    const fs::path root = work / "p3a_dummy";
    const fs::path ken = root / "SF6_RecordingSlotManager_data" / "Ken";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(ken, ec);
    FakeGame g; g.fid = 10; initial_slots(g, 0);
    FakeLive live(&g);
    FakeClock clock;
    FakeSetup setup;
    const model::setup::Dummy author = dummy_of({ { "DummyActionType", 0 }, { "GuardType", 3 }, { "GD_Type", 1 }, { "IsGuardSwitching", 1 },
                                                  { "QS_Type", 1 }, { "NC_TYPE", 0 }, { "PC_TYPE", 0 } });
    setup.now.playback = model::setup::Playback{ 0, 2, 2 };
    setup.now.dummy = author;
    model::files::Engine e(root, &live, &clock);
    e.set_setup(&setup);
    e.load_settings();
    e.ensure(10);
    std::string tk, why;
    e.export_as(json("Drill"), &tk);
    const json pack = lj::load_file(ken / "Ken_Drill.json");
    const auto blk = model::setup::of_pack(pack, &why);
    check(blk && same_dummy(blk->dummy, author) && blk->playback && !blk->reversal, "export: the dummy's settings on entry 1", [&] { return why; });

    // the importer's own settings: the pack's fields replace them, the others stay
    const model::setup::Dummy mine = dummy_of({ { "DummyActionType", 1 }, { "GuardType", 0 }, { "GD_Type", 0 }, { "IsGuardSwitching", 0 },
                                                { "QS_Type", 0 }, { "NC_TYPE", 1 }, { "PC_TYPE", 1 }, { "DR_Type", 3 } });
    setup.now.dummy = mine;
    select_pack(e, "Ken_Drill.json");
    clock.t += 10;
    const std::string ir = e.import_selected();
    model::setup::Dummy want = author;
    want.v[(size_t)dummy_index("DR_Type")] = 3;
    check(ir.rfind("Loaded:", 0) == 0 && same_dummy(setup.now.dummy, want), "import: the pack's fields written, the rest kept",
          [&] { return ir + " / " + e.setup_report(); });
    json backup;
    for (const auto& f : fs::directory_iterator(ken / "Backups", ec)) backup = lj::load_file(f.path());
    const auto bblk = model::setup::of_pack(backup, &why);
    check(bblk && same_dummy(bblk->dummy, mine), "the pre-import backup carries the replaced dummy settings", [&] { return why; });

    // import_training_setup = false: the dummy settings untouched too
    std::string derr;
    lj::dump_file(root / model::store::config_path(), json::object({ { "import_training_setup", false } }), &derr);
    model::files::Engine e2(root, &live, &clock);
    e2.set_setup(&setup);
    e2.load_settings();
    e2.ensure(10);
    setup.now.dummy = mine;
    select_pack(e2, "Ken_Drill.json");
    clock.t += 10;
    e2.import_selected();
    check(same_dummy(setup.now.dummy, mine), "import_training_setup false: the dummy settings skipped", [&] { return e2.setup_report(); });
    fs::remove(root / model::store::config_path(), ec);

    // a downloaded pack with `info`: Save over it keeps it, Save As copies it, a pack without info gives none
    json drill = pack;
    const json info = json::object({ { "author", "Chris_F" }, { "description", "Throw him.\nHe breaks it." }, { "source", "drillcodes.com" } });
    drill[0]["info"] = info;
    lj::dump_file(ken / "Ken_Dl.json", drill, &derr);
    e.invalidate();
    e.ensure(10);
    select_pack(e, "Ken_Dl.json");
    clock.t += 10;
    check(e.import_selected().rfind("Loaded:", 0) == 0, "the downloaded pack opened", nullptr);
    const std::string sr = e.export_selected(&tk);
    const json saved = lj::load_file(ken / "Ken_Dl.json");
    check(sr.rfind("Custom Saved", 0) == 0 && saved.is_array() && saved[0]["info"] == info, "Save over the loaded pack keeps its info",
          [&] { return sr + " " + saved.dump().substr(0, 200); });
    e.export_as(json("Copy"), &tk);
    const json copy = lj::load_file(ken / "Ken_Copy.json");
    check(copy.is_array() && copy[0]["info"] == info && !copy[1].contains("info"), "Save As copies the loaded pack's info (entry 1 only)",
          [&] { return copy.dump().substr(0, 200); });
    e.export_as(json("Copy2"), &tk);                            // the loaded pack is now Ken_Copy: its info goes on
    check(lj::load_file(ken / "Ken_Copy2.json")[0]["info"] == info, "a second Save As still carries it", nullptr);
    select_pack(e, "Ken_Drill.json");
    clock.t += 10;
    e.import_selected();
    e.export_as(json("Plain"), &tk);
    check(!lj::load_file(ken / "Ken_Plain.json")[0].contains("info"), "a loaded pack without info: none written", nullptr);
    model::files::Engine e3(root, &live, &clock);              // nothing loaded in this session
    e3.set_setup(&setup);
    e3.load_settings();
    e3.ensure(10);
    e3.export_as(json("Fresh"), &tk);
    check(!lj::load_file(ken / "Ken_Fresh.json")[0].contains("info"), "nothing loaded: no info", nullptr);
}

void p3a_list_reglob(const fs::path& work) {
    begin("P3a files: the drill list read again at each Manager page entry (ensure)");
    const fs::path root = work / "p3a_reglob";
    const fs::path ken = root / "SF6_RecordingSlotManager_data" / "Ken";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(ken, ec);
    FakeGame g; g.fid = 10; initial_slots(g, 0);
    FakeLive live(&g);
    FakeClock clock;
    model::files::Engine e(root, &live, &clock);
    std::string tk, derr;
    e.ensure(10);
    e.export_as(json("A"), &tk);
    e.export_as(json("B"), &tk);
    select_pack(e, "Ken_A.json");
    const uint64_t v0 = e.list_version();
    const size_t n0 = e.dropdown().size();
    check(!e.ensure(10) && e.list_version() == v0, "nothing changed in the folder: no change, same list version", nullptr);
    lj::dump_file(ken / "Ken_Hand.json", lj::load_file(ken / "Ken_B.json"), &derr);   // copied in by hand
    check(e.ensure(10) && e.dropdown().size() == n0 + 1 && e.list_version() != v0, "a pack copied in by hand is listed at the next entry",
          [&] { return std::to_string(e.dropdown().size()); });
    check(e.selected_file() == "Ken_A.json", "the selection kept (its file is still there)", [&] { return e.selected_file(); });
    bool listed = false;
    for (size_t i = 2; i <= e.dropdown().size(); ++i) { e.set_dropdown_index(json((int)i)); if (e.selected_file() == "Ken_Hand.json") listed = true; }
    check(listed, "the new pack can be selected", nullptr);
    select_pack(e, "Ken_A.json");
    fs::remove(ken / "Ken_A.json", ec);                          // removed by hand
    check(e.ensure(10) && e.dropdown().size() == n0 && e.selected_file().empty(), "a pack removed by hand leaves the list and the selection",
          [&] { return e.selected_file(); });
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

void p3a_unknown_folders(const fs::path& work) {
    begin("P3a files: Unknown(<id>) folder merged into the fighter's name");
    const fs::path root = work / "p3a_unknown";
    const fs::path data = root / "SF6_RecordingSlotManager_data";
    std::error_code ec;
    fs::remove_all(root, ec);
    auto put = [](const fs::path& p, const std::string& text) {
        std::error_code e; fs::create_directories(p.parent_path(), e);
        std::ofstream(p, std::ios::binary) << text;
    };
    auto read = [](const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
    FakeGame g; g.fid = 34; initial_slots(g, 0);
    FakeLive live(&g);
    FakeClock clock;
    model::files::Engine e(root, &live, &clock);

    // no named folder yet: renamed as a whole
    put(data / "Unknown(34)" / "_virtual_slots.json", "old_vs");
    put(data / "Unknown(34)" / "Backups" / "b.json", "bk");
    auto r = e.migrate_unknown_folders();
    check(r.size() == 1 && !fs::exists(data / "Unknown(34)") && read(data / "Arjun" / "_virtual_slots.json") == "old_vs"
          && read(data / "Arjun" / "Backups" / "b.json") == "bk", "Unknown(34) renamed to Arjun", [&] { return r.empty() ? "" : r[0]; });

    // both exist: missing files moved, a file Arjun already has stays in Unknown(34)
    put(data / "Unknown(34)" / "_virtual_slots.json", "older_vs");
    put(data / "Unknown(34)" / "Pack.json", "pack");
    r = e.migrate_unknown_folders();
    check(read(data / "Arjun" / "Pack.json") == "pack" && read(data / "Arjun" / "_virtual_slots.json") == "old_vs"
          && read(data / "Unknown(34)" / "_virtual_slots.json") == "older_vs", "merge: missing files moved, existing ones kept",
          [&] { return r.empty() ? "" : r[0]; });
    fs::remove(data / "Unknown(34)" / "_virtual_slots.json", ec);
    r = e.migrate_unknown_folders();
    check(!fs::exists(data / "Unknown(34)"), "an empty Unknown(34) left after the merge is removed", nullptr);
    check(e.migrate_unknown_folders().empty(), "nothing to do: no line", nullptr);
    e.ensure(34);
    check(e.char_name() && *e.char_name() == "Arjun", "fighter 34 is Arjun", nullptr);
}

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
    p3a_unknown_folders(work);
    p3a_setup_model();
    p3a_setup_engine(work);
    p3a_dummy_model();
    p3a_dummy_info_engine(work);
    p3a_list_reglob(work);
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
