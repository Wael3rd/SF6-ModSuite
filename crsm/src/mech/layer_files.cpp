// mech/files — the "files" layer (P3a) in the game: model::files::Engine on the game's live slots, its ALLOC fallback
// on the frame callback, and the bridge functions the Lua shims call (MAIN's _rsm_api, RMT's virtual-slot storage,
// RSL's reversal files). Every bridge call runs in the Lua caller's context, as the Lua function it replaces did: the
// Manager page's decides (menu chain), RMT's row hooks, the reversal trigger. Game writes there = the Lua's own moment.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <reframework/API.hpp>

#include "common.hpp"
#include "game/gm.hpp"
#include "mech/mech.hpp"
#include "model/chars.hpp"
#include "model/files.hpp"
#include "model/luajson.hpp"
#include "model/lutil.hpp"
#include "model/store.hpp"
#include "model/timeline.hpp"
#include "model/transfer.hpp"
#include "ui/ui.hpp"

namespace mech::files {
namespace {

namespace gm = game::gm;
namespace fsys = std::filesystem;
using model::files::LiveRead;
using F = cmn::Feature;

constexpr const char* kRoot = "reframework/data";          // REFramework's json / fs root (the game's working directory)

// ---- MAIN's get_slots_access (path A) and the slot writes, on the game ------------------------------------------------
class GameLive final : public model::files::LiveSlots {
public:
    std::string access(int fid) override {
        slots_ = nullptr;
        if (fid == -1) return "Unknown Character ID";
        gm::MO* tm = gm::singleton("app.training.TrainingManager");
        if (!tm) return "No TrainingManager";
        gm::MO* rf = gm::call_obj(tm, "get_RecordFunc");
        if (!rf) return "No RecFunc";
        gm::MO* list = gm::ref(gm::ref(gm::ref(rf, "_tData"), "RecordSetting"), "FighterDataList");
        if (!list) return "No Fighter List";
        gm::MO* fd = (fid >= 0 && fid < gm::list_count(list)) ? gm::list_item(list, fid) : nullptr;
        if (!fd) return "Data not found for ID " + std::to_string(fid);
        slots_ = gm::ref(fd, "RecordSlots");
        if (!slots_) return "OK";                                // the Lua returns (nil, "OK") there: "Error: OK"
        return "";
    }
    int read(int, int k0, LiveRead* out) override {
        gm::MO* s = slot(k0);
        if (!s) return 1;
        int64_t frames = 0, weight = 0;
        bool valid = false;
        if (!gm::get_int(s, "Frame", &frames) || !gm::get_int(s, "Weight", &weight) || !gm::get_bool(s, "IsValid", &valid)) return 1;
        out->frames = frames;
        out->weight = weight;
        out->valid = valid;
        out->active = false;
        gm::get_bool(s, "IsActive", &out->active);
        out->values.clear();
        if (valid && frames > 0) {
            gm::MO* rd = gm::ref(s, "InputData");
            if (!rd) return 1;                                   // the Lua indexes a nil InputData
            int32_t len = 0;
            const uint16_t* d = gm::u16_data(gm::ref(rd, "buff"), &len);
            if (!d || frames > len) return 2;                    // nil buffer / GetValue past the end: the Lua raises
            out->values.assign(d, d + frames);
        }
        return 0;
    }
    int64_t capacity(int, int k0) override {
        gm::MO* rd = gm::ref(slot(k0), "InputData");
        if (!rd) return -1;
        gm::MO* buf = gm::ref(rd, "buff");
        if (!buf) return 0;
        const int32_t n = gm::array_len(buf);
        return n < 0 ? 0 : n;
    }
    bool allocate(int, int k0, int64_t needed) override {
        gm::MO* rd = gm::ref(slot(k0), "InputData");
        if (!rd) return false;
        const int64_t n = needed > model::files::kRecordBufferLen ? needed : model::files::kRecordBufferLen;
        gm::MO* arr = gm::new_array(gm::type("System.UInt16"), (uint32_t)n);   // add_ref'd, held (as the Lua's arr:add_ref())
        if (!arr) return false;
        if (!gm::set_ref(rd, "buff", arr)) return false;
        gm::set_int(rd, "Cursor", 0);
        gm::MO* chk = gm::ref(rd, "buff");
        return chk && gm::array_len(chk) >= needed;
    }
    bool set_weight(int, int k0, const model::files::json& w) override {
        if (!w.is_number()) return false;
        return gm::set_int(slot(k0), "Weight", (int64_t)w.get<double>());
    }
    bool write_empty(int, int k0) override {
        gm::MO* s = slot(k0);
        gm::set_bool(s, "IsValid", false);
        gm::set_int(s, "Frame", 0);
        gm::set_bool(s, "IsActive", false);
        gm::MO* rd = gm::ref(s, "InputData");
        if (!rd) return false;
        gm::set_int(rd, "Num", 0);
        return true;
    }
    bool write_values(int, int k0, const uint16_t* v, size_t n) override {
        int32_t len = 0;
        uint16_t* d = gm::u16_data(gm::ref(gm::ref(slot(k0), "InputData"), "buff"), &len);
        if (!d) return false;
        const size_t m = n < (size_t)len ? n : (size_t)len;
        std::copy(v, v + m, d);
        return n <= (size_t)len;
    }
    bool finish(int, int k0, int64_t needed) override {
        gm::MO* s = slot(k0);
        gm::set_bool(s, "IsValid", true);
        gm::set_int(s, "Frame", needed);
        gm::set_bool(s, "IsActive", false);
        return gm::set_int(gm::ref(s, "InputData"), "Num", needed);
    }
    bool activate_valid(int) override {
        for (int i = 0; i < 8; ++i) {
            gm::MO* s = slot(i);
            bool v = false;
            if (s && gm::get_bool(s, "IsValid", &v) && v) gm::set_bool(s, "IsActive", true);
        }
        return true;
    }
    bool set_active(int, int k0, bool on) override { return gm::set_bool(slot(k0), "IsActive", on); }
private:
    gm::MO* slot(int k0) { return (slots_ && k0 >= 0 && k0 < 8) ? gm::array_get(slots_, k0) : nullptr; }
    gm::MO* slots_ = nullptr;
};

// A pack's training block (model::setup): the replay settings on TM_RecordSetting (TrainingManager._tData.RecordSetting,
// the object MAIN's path A reaches too), the reversal rows through the reversal layer, the dummy's settings on the
// DummyData of _tData.DummyStatus / GuardSetting / DefenseSystem / CounterSetting. Runs inside the import / export call,
// under lock(), at the moment the Lua wrote the slots (the menu open). The dummy's fields are written as the game's own
// pages leave them, a plain store into the menu's data with no setter and no Apply call: the training functions take the
// menu's values when the menu closes, as after a change made on the page.
class GameSetup final : public model::files::SetupPort {
public:
    bool read(int fid, model::setup::Setup* out) override {
        if (gm::MO* rs = record_setting()) {
            model::setup::Playback p;
            int64_t v = 0;
            if (gm::get_int(rs, "PlayType", &v)) p.PlayType = v;
            if (gm::get_int(rs, "LoopType", &v)) p.LoopType = v;
            if (gm::get_int(rs, "InfoType", &v)) p.InfoType = v;
            if (p.PlayType || p.LoopType || p.InfoType) out->playback = p;
        }
        model::setup::Rows rows;
        std::string why;
        if (mech::reversal::setup_read(fid, &rows, &why)) out->reversal = std::move(rows);
        else cmn::journal("files: pack: no reversal rows (%s)", why.c_str());
        if (gm::MO* td = tdata()) {
            model::setup::Dummy d;
            for (int i = 0; i < model::setup::kDummyCount; ++i) {
                gm::MO* o = dummy_data(td, model::setup::kDummyFields[i].object);
                int64_t v = 0;
                if (o && gm::get_int(o, model::setup::kDummyFields[i].name, &v)) d.v[(size_t)i] = v;
            }
            model::setup::sanitize(d);
            if (d.any()) out->dummy = d;
            else cmn::journal("files: pack: no dummy settings read");
        }
        return out->playback || out->reversal || out->dummy;
    }
    bool write(int fid, const model::setup::Setup& s, std::string* report) override {
        std::string r;
        bool ok = true;
        if (s.playback) {
            gm::MO* rs = record_setting();
            int n = 0;
            if (rs && s.playback->PlayType && gm::set_int(rs, "PlayType", *s.playback->PlayType)) ++n;
            if (rs && s.playback->LoopType && gm::set_int(rs, "LoopType", *s.playback->LoopType)) ++n;
            if (rs && s.playback->InfoType && gm::set_int(rs, "InfoType", *s.playback->InfoType)) ++n;
            r += "replay settings: " + std::to_string(n) + " written";
            if (!rs) { r += " (no RecordSetting)"; ok = false; }
        }
        if (s.reversal) {
            std::string rr;
            if (!mech::reversal::setup_write(fid, *s.reversal, &rr)) ok = false;
            r += (r.empty() ? "" : "; ") + rr;
        }
        if (s.dummy) {
            gm::MO* td = tdata();
            int n = 0, failed = 0;
            for (int i = 0; i < model::setup::kDummyCount; ++i) {
                const auto& v = s.dummy->v[(size_t)i];
                if (!v) continue;
                const model::setup::DummyField& f = model::setup::kDummyFields[i];
                gm::MO* o = td ? dummy_data(td, f.object) : nullptr;
                const bool w = o && (f.is_bool ? gm::set_bool(o, f.name, *v != 0) : gm::set_int(o, f.name, *v));
                if (w) ++n; else ++failed;
            }
            r += (r.empty() ? "" : "; ") + std::string("dummy settings: ") + std::to_string(n) + " written";
            if (failed) { r += ", " + std::to_string(failed) + " not reachable"; ok = false; }
        }
        cmn::journal("files: pack training block: %s", r.c_str());
        if (report) *report = r;
        return ok;
    }
private:
    static gm::MO* tdata() { return gm::ref(gm::singleton("app.training.TrainingManager"), "_tData"); }
    static gm::MO* record_setting() { return gm::ref(tdata(), "RecordSetting"); }
    // _tData.<object>.DummyData, checked live (the menu's own object, never cached)
    static gm::MO* dummy_data(gm::MO* td, const char* object) {
        gm::MO* o = gm::ref(gm::ref(td, object), "DummyData");
        return (o && gm::is_managed(o)) ? o : nullptr;
    }
};

class SteadyClock final : public model::files::Clock {
public:
    double seconds() override {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    std::tm local_now() override {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        return tm;
    }
};

// MAIN:871-926: the recording-based allocation (the game's own record path on the empty slot, 10 frames)
class GameAlloc final : public model::files::AllocEnv {
public:
    bool start_recording(int slot) override {
        gm::MO* tm = gm::singleton("app.training.TrainingManager");
        gm::MO* rf = tm ? gm::call_obj(tm, "get_RecordFunc") : nullptr;
        if (!rf) return false;
        gm::TD* rt = rf->get_type_definition();
        gm::call(rf, gm::method_n(rt, "ChangeRecordStartSetting", 1), { gm::arg_i32(0) });
        gm::call(rf, gm::method_n(rt, "SetStartRecord", 2), { gm::arg_i32(16), gm::arg_i32(slot) });
        gm::call(rf, gm::method_n(rt, "ReleaseDummyData", 0), {});
        gm::call(rf, gm::method_n(rt, "CopyDummyData", 0), {});
        gm::call(tm, gm::method_n(tm->get_type_definition(), "ChangeState", 1), { gm::arg_i32(3) });
        gm::call(rf, gm::method_n(rt, "ForceApply", 0), {});
        return true;
    }
    void stop_phase1() override {
        if (gm::MO* tm = gm::singleton("app.training.TrainingManager"))
            gm::call(tm, gm::method_n(tm->get_type_definition(), "ChangeState", 1), { gm::arg_i32(4) });
    }
    void stop_phase2() override {
        gm::MO* tm = gm::singleton("app.training.TrainingManager");
        gm::MO* rf = tm ? gm::call_obj(tm, "get_RecordFunc") : nullptr;
        if (!rf) return;
        gm::TD* rt = rf->get_type_definition();
        gm::call(rf, gm::method_n(rt, "StopRecord", 0), {});
        gm::call(rf, gm::method_n(rt, "ChangeRecordStartSetting", 1), { gm::arg_i32(1) });
        gm::call(rf, gm::method_n(rt, "ForceApply", 0), {});
    }
};

GameLive g_live;
GameSetup g_setup;
SteadyClock g_clock;
GameAlloc g_alloc;
model::files::Engine g_engine(kRoot, &g_live, &g_clock);
void (*g_import_listener)() = nullptr;
uint64_t g_imports_seen = 0;
// the replay layer follows an import at once (the installed array's entries refreshed), as RMT:2101-2102
void after_files_call() {
    if (g_import_listener && g_engine.vslots_imports() != g_imports_seen) {
        g_imports_seen = g_engine.vslots_imports();
        g_import_listener();
    }
}
bool g_started = false;
bool g_checked = false;                     // the UInt16[] layout check ran (first frame with the layer native)
uint64_t g_ops = 0;
// The dummy follow (below): its reads after each mech event (flow change: the training character select is flow 6 between
// two training flows; training edge; menu close), one every 30 frames, 10 of them (5 s).
constexpr int kFollowReads = 10;
uint64_t g_follow_events = UINT64_MAX;
int g_follow_reads = 0;

// The id the Manager page reads when it is entered (MAIN:757-762): TrainingManager._tData.SelectMenu.PlayerDatas[1].FighterID
int read_select_menu_id() {
    gm::MO* tm = gm::singleton("app.training.TrainingManager");
    gm::MO* pd = gm::ref(gm::ref(gm::ref(tm, "_tData"), "SelectMenu"), "PlayerDatas");
    gm::MO* p2 = pd ? gm::array_get(pd, 1) : nullptr;
    int64_t id = -1;
    if (!p2 || !gm::get_int(p2, "FighterID", &id)) return -1;
    return (int)id;
}

// ---- bridge functions ------------------------------------------------------------------------------------------------------
// Each: under lock(); any exception or access violation (catch(...) with /EHa) = one fault of the layer and a neutral
// answer (the Lua shim then shows what the Lua shows for a failed action).
#define FILES_FN(name) int32_t SF6CORE_CALL name(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max)
#define FILES_TRY std::lock_guard<std::recursive_mutex> lk_(mech::lock()); br::Out o{ r, max }; (void)a; (void)n; try {
#define FILES_END(what) } catch (...) { cmn::fault(F::Files, what); return o.done(); } return o.done();

FILES_FN(f_fighter_changed) { FILES_TRY g_engine.fighter_changed((int)br::i64(a, n, 0, -1)); FILES_END("fighter_changed") }
FILES_FN(f_invalidate) { FILES_TRY g_engine.invalidate(); FILES_END("invalidate") }
FILES_FN(f_ensure) { FILES_TRY o.boolean(g_engine.ensure(read_select_menu_id())); FILES_END("ensure") }
FILES_FN(f_list_version) { FILES_TRY o.i64((int64_t)g_engine.list_version()); FILES_END("list_version") }
FILES_FN(f_list) {
    FILES_TRY
    json l = json::array();
    for (const std::string& s : g_engine.dropdown()) l.push_back(s);
    o.i64((int64_t)g_engine.list_version());
    o.str(br::to_lua(l));
    FILES_END("list")
}
FILES_FN(f_get_index) { FILES_TRY o.i64(g_engine.dropdown_index()); FILES_END("get_index") }
FILES_FN(f_set_index) { FILES_TRY g_engine.set_dropdown_index(br::value(a, n, 0)); FILES_END("set_index") }
FILES_FN(f_selected_name) { FILES_TRY o.str(g_engine.selected_name()); FILES_END("selected_name") }
FILES_FN(f_selected_file) { FILES_TRY o.str(g_engine.selected_file()); FILES_END("selected_file") }
FILES_FN(f_loaded_name) { FILES_TRY o.str(g_engine.loaded_name()); FILES_END("loaded_name") }
FILES_FN(f_import_selected) {
    FILES_TRY ++g_ops;
    const std::string res = g_engine.import_selected();
    after_files_call();
    o.str(res);
    cmn::journal("files: import_selected -> %s", res.c_str());
    FILES_END("import_selected")
}
FILES_FN(f_export_as) {
    FILES_TRY ++g_ops;
    std::string t;
    const std::string res = g_engine.export_as(br::value(a, n, 0), &t);
    o.str(res);
    if (t.empty()) o.nil(); else o.str(t);
    cmn::journal("files: export_as -> %s", res.c_str());
    FILES_END("export_as")
}
FILES_FN(f_export_selected) {
    FILES_TRY ++g_ops;
    std::string t;
    const std::string res = g_engine.export_selected(&t);
    o.str(res);
    if (t.empty()) o.nil(); else o.str(t);
    cmn::journal("files: export_selected -> %s", res.c_str());
    FILES_END("export_selected")
}
FILES_FN(f_erase_selected) {
    FILES_TRY ++g_ops;
    const std::string res = g_engine.erase_selected();
    o.str(res);
    cmn::journal("files: erase_selected -> %s", res.c_str());
    FILES_END("erase_selected")
}
FILES_FN(f_rename_selected) {
    FILES_TRY ++g_ops;
    std::string t;
    const std::string res = g_engine.rename_selected(br::value(a, n, 0), &t);
    o.str(res);
    if (t.empty()) o.nil(); else o.str(t);
    cmn::journal("files: rename_selected -> %s", res.c_str());
    FILES_END("rename_selected")
}
FILES_FN(f_reopen_loaded) {
    FILES_TRY ++g_ops;
    const std::string res = g_engine.reopen_loaded();
    after_files_call();
    o.str(res);
    cmn::journal("files: reopen_loaded -> %s", res.c_str());
    FILES_END("reopen_loaded")
}
FILES_FN(f_set_native_action) { FILES_TRY g_engine.set_native_action(br::boolean(a, n, 0)); FILES_END("set_native_action") }
FILES_FN(f_slot_name) { FILES_TRY o.val(g_engine.slot_name((int)br::i64(a, n, 0, -1))); FILES_END("slot_name") }
FILES_FN(f_set_slot_name) { FILES_TRY g_engine.set_slot_name((int)br::i64(a, n, 0, -1), br::value(a, n, 1)); FILES_END("set_slot_name") }
FILES_FN(f_fighter_id) { FILES_TRY o.i64(g_engine.fighter()); FILES_END("fighter_id") }
FILES_FN(f_char_name) {
    FILES_TRY
    const auto c = g_engine.char_name();
    if (c) o.str(*c); else o.nil();
    FILES_END("char_name")
}
FILES_FN(f_read_slot) {
    FILES_TRY
    std::string err;
    const auto e = g_engine.read_slot((int)br::i64(a, n, 0, -1), &err);
    if (e) o.str(br::to_lua(*e)); else o.nil();
    FILES_END("read_slot")
}
FILES_FN(f_write_slot) {
    FILES_TRY ++g_ops;
    const json e = br::from_lua(br::str(a, n, 0));
    o.str(e.is_discarded() ? std::string("Error: bad entry") : g_engine.write_slot(e));
    FILES_END("write_slot")
}
// fill_record(RecordData address, entry) (MAIN:1025-1044): the entry's timeline into that RecordData (buffer allocated
// when missing or too short), Num = frames, Cursor = 0. Returns the frames.
FILES_FN(f_fill_record) {
    FILES_TRY
    gm::MO* rd = reinterpret_cast<gm::MO*>((uintptr_t)br::i64(a, n, 0, 0));
    const json e = br::from_lua(br::str(a, n, 1));
    o.i64((rd && gm::is_managed(rd) && !e.is_discarded()) ? mech::files::fill_record(rd, e) : 0);
    FILES_END("fill_record")
}
FILES_FN(f_name_slot) { FILES_TRY ++g_ops; g_engine.name_slot((int)br::i64(a, n, 0, 0), br::value(a, n, 1)); FILES_END("name_slot") }
FILES_FN(f_vslots_version) {
    FILES_TRY
    g_engine.vslots();
    o.i64((int64_t)g_engine.vslots_version());
    o.i64((int64_t)g_engine.vslots_imports());
    FILES_END("vslots_version")
}
FILES_FN(f_vslots_get) {
    FILES_TRY
    model::vslots::VSlots& v = g_engine.vslots();
    o.str(br::to_lua(v.to_file()));
    o.i64((int64_t)g_engine.vslots_version());
    o.i64((int64_t)g_engine.vslots_imports());
    FILES_END("vslots_get")
}
FILES_FN(f_vslot_delete) { FILES_TRY ++g_ops; g_engine.vslot_delete((int)br::i64(a, n, 0, 0)); FILES_END("vslot_delete") }
FILES_FN(f_vslot_put) {
    FILES_TRY ++g_ops;
    const json e = br::from_lua(br::str(a, n, 1));
    if (!e.is_discarded()) g_engine.vslot_put((int)br::i64(a, n, 0, 0), e, true);
    FILES_END("vslot_put")
}
FILES_FN(f_vslots_aw) {
    FILES_TRY
    const json p = json::parse(br::str(a, n, 0), nullptr, false);
    if (!p.is_discarded()) g_engine.vslots_active_weight(p);
    FILES_END("vslots_aw")
}

// RSL's two files while it keeps the reversal layer: read as the file is, written as json.dump_file writes the table.
std::string doc_path(const std::string& kind, const std::string& char_name) {
    bool known = false;
    for (const auto& c : model::chars::all()) if (char_name == c.name) known = true;
    if (!known) return "";
    if (kind == "revmap") return model::store::reversal_virtual_path(char_name);
    if (kind == "revrows") return model::store::reversal_rows_path(char_name);
    return "";
}
FILES_FN(f_doc_get) {
    FILES_TRY
    const std::string rel = doc_path(br::str(a, n, 0), br::str(a, n, 1));
    std::ifstream f(g_engine.abs(rel), std::ios::binary);
    if (rel.empty() || !f) { o.nil(); return o.done(); }
    o.str(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
    FILES_END("doc_get")
}
FILES_FN(f_doc_put) {
    FILES_TRY ++g_ops;
    const std::string rel = doc_path(br::str(a, n, 0), br::str(a, n, 1));
    if (rel.empty()) { o.boolean(false); return o.done(); }
    std::string text, err;
    bool ok = false;
    if (model::transfer::file_text(br::str(a, n, 2), &text)) ok = model::luajson::write_text(g_engine.abs(rel), text, &err);
    if (!ok) {                                                  // json.dump_file truncates the file when the dump throws
        std::error_code ec;
        fsys::create_directories(g_engine.abs(rel).parent_path(), ec);
        std::ofstream(g_engine.abs(rel), std::ios::binary | std::ios::trunc);
    }
    o.boolean(ok);
    FILES_END("doc_put")
}

void hand_back() {
    std::lock_guard lk(mech::lock());
    cmn::log_warn("[CRSMNative] files: back to the Lua (its own lists, selection and slot names 1-8 from now on)");
}

std::string status() {
    std::lock_guard lk(mech::lock());
    char b[256];
    const auto c = g_engine.char_name();
    std::snprintf(b, sizeof(b), "%s, fighter %d (%s), %zu loadouts, alloc queue %zu, %llu actions, u16 %s",
                  cmn::owns(F::Files) ? "native" : "lua", g_engine.fighter(), c ? c->c_str() : "-", g_engine.dropdown().size() - 1,
                  g_engine.alloc_queue().size(), (unsigned long long)g_ops, g_checked ? gm::u16_report().c_str() : "not checked");
    return b;
}

}  // namespace

model::files::Engine& engine() { return g_engine; }
int fighter() { return g_engine.fighter(); }
void set_import_listener(void (*fn)()) { g_import_listener = fn; }
bool ensure() {
    std::lock_guard lk(mech::lock());
    try { return g_engine.ensure(read_select_menu_id()); } catch (...) { cmn::fault(F::Files, "ensure (ui)"); return false; }
}
void after_ui_call() {
    std::lock_guard lk(mech::lock());
    try { after_files_call(); } catch (...) { cmn::fault(F::Files, "after_ui_call"); }
}
void after_call() { after_files_call(); }
int select_menu_id() { return read_select_menu_id(); }

int64_t fill_record(void* record_data, const json& e) {
    gm::MO* rd = static_cast<gm::MO*>(record_data);
    if (!rd || !e.is_object() || !e.contains("timeline") || !(e["timeline"].is_array() || e["timeline"].is_object())) return 0;
    const auto lines = model::files::timeline_lines(e["timeline"]);
    const int64_t needed = model::timeline::entry_frames(lines);
    gm::MO* buf = gm::ref(rd, "buff");
    if (!buf || gm::array_len(buf) < needed) {
        const int64_t len = needed > model::files::kRecordBufferLen ? needed : model::files::kRecordBufferLen;
        buf = gm::new_array(gm::type("System.UInt16"), (uint32_t)len);     // add_ref'd (held): the Lua's buffer:add_ref()
        if (!buf) return 0;
        gm::set_ref(rd, "buff", buf);
    }
    int32_t cap = 0;
    uint16_t* d = gm::u16_data(buf, &cap);
    if (!d) { cmn::fault(F::Files, "fill_record: buffer"); return 0; }
    const std::vector<uint16_t> v = model::timeline::expand_anchored(lines);
    const size_t m = v.size() < (size_t)cap ? v.size() : (size_t)cap;
    std::copy(v.begin(), v.begin() + (ptrdiff_t)m, d);
    gm::set_int(rd, "Num", needed);
    gm::set_int(rd, "Cursor", 0);
    return needed;
}

void init() {
    cmn::set_implemented(F::Files);
    cmn::on_hand_back(F::Files, hand_back);
    cmn::add_status("files", status);
    struct Reg { const char* name; SF6CoreLuaFn fn; };
    const Reg fns[] = {
        { "files_fighter_changed", f_fighter_changed }, { "files_invalidate", f_invalidate }, { "files_ensure", f_ensure },
        { "files_list_version", f_list_version }, { "files_list", f_list }, { "files_get_index", f_get_index },
        { "files_set_index", f_set_index }, { "files_selected_name", f_selected_name }, { "files_selected_file", f_selected_file },
        { "files_loaded_name", f_loaded_name }, { "files_import_selected", f_import_selected }, { "files_export_as", f_export_as },
        { "files_export_selected", f_export_selected }, { "files_erase_selected", f_erase_selected },
        { "files_rename_selected", f_rename_selected }, { "files_reopen_loaded", f_reopen_loaded },
        { "files_set_native_action", f_set_native_action }, { "files_slot_name", f_slot_name },
        { "files_set_slot_name", f_set_slot_name }, { "files_fighter_id", f_fighter_id }, { "files_char_name", f_char_name },
        { "files_read_slot", f_read_slot }, { "files_write_slot", f_write_slot }, { "files_fill_record", f_fill_record },
        { "files_name_slot", f_name_slot }, { "files_vslots_version", f_vslots_version }, { "files_vslots_get", f_vslots_get },
        { "files_vslot_delete", f_vslot_delete }, { "files_vslot_put", f_vslot_put }, { "files_vslots_aw", f_vslots_aw },
        { "files_doc_get", f_doc_get }, { "files_doc_put", f_doc_put },
    };
    for (const Reg& f : fns) cmn::bridge_fn(f.name, f.fn);
    g_engine.set_setup(&g_setup);
    cmn::add_frame_fn(frame, "layer_files");
}

void start() {
    if (!cmn::owns(F::Files)) return;
    std::lock_guard lk(mech::lock());
    try { g_engine.load_settings(); } catch (...) {}
    try {
        for (const std::string& m : g_engine.migrate_unknown_folders()) cmn::log_info("[CRSMNative] files: %s", m.c_str());
    } catch (...) {}
    g_started = true;
    cmn::log_info("[CRSMNative] files: native (loadouts, slot names, virtual slots, reversal files); no hook");
}

// Frame callback (game thread): the UInt16[] check once, the dummy's id while unknown, the ALLOC / WRITE_DATA queue.
void frame(uint64_t fr) {
    if (!cmn::owns(F::Files)) return;
    std::lock_guard lk(mech::lock());
    try {
        if (!g_checked) {
            g_checked = true;
            if (!gm::u16_ok()) {
                cmn::log_warn("[CRSMNative] files: UInt16[] layout check failed (%s): the layer goes back to the Lua", gm::u16_report().c_str());
                for (int i = 0; i < 5; ++i) cmn::fault(F::Files, "UInt16[] layout");
                return;
            }
            cmn::journal("files: UInt16[] layout %s", gm::u16_report().c_str());
            cmn::refresh_status();
        }
        const State st = mech::state();
        // MAIN's UpdateGameInfo hook forwards the dummy (files_fighter_changed); this covers a missed one. Standalone (P6: no
        // CRSM Lua, so no MAIN): the dummy is followed here, with the menu closed, from the training menu's own record of it
        // (the id MAIN reads at the Manager page's entry, MAIN:757-762), with MAIN's reset (names, selection, list). No fight
        // hook: R38. The dummy changes only through the character select (another flow): read every 30 frames for 5 s after
        // each mech event, not every 30 frames for the whole session.
        if (const uint64_t ev = mech::events(); ev != g_follow_events) { g_follow_events = ev; g_follow_reads = kFollowReads; }
        const bool follow = g_engine.fighter() == -1 || ui::standalone();
        if (follow && st.training && g_follow_reads > 0 && fr % 30 == 11 &&
            (g_engine.fighter() == -1 || !mech::pause_now().bit)) {      // (the standalone follow waits for the menu to close)
            --g_follow_reads;
            const int id = read_select_menu_id();
            if (id != -1 && id != g_engine.fighter()) {
                if (g_engine.fighter() != -1) cmn::journal("files: standalone: the dummy is now fighter %d (was %d)", id, g_engine.fighter());
                g_engine.fighter_changed(id);
            }
        }
        if (g_engine.alloc_pending()) {
            bool crashed = false;
            const std::string t = g_engine.alloc_tick(mech::pause_now().bit, g_alloc, &crashed);
            after_files_call();
            if (crashed) cmn::fault(F::Files, ("WRITE_DATA: " + g_engine.status()).c_str());
            mech::push_ticker(t);
            if (!g_engine.alloc_pending()) cmn::journal("files: alloc queue done: %s", g_engine.status().c_str());
        }
    } catch (...) {
        cmn::fault(F::Files, "frame");
    }
}

}  // namespace mech::files
