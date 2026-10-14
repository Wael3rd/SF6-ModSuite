// mech/record — the "record" layer (P3d) in the game: model::record::Engine (a recording into a virtual slot through
// live slot 8). Armed by the Lua UI (the game's own F on a Record row showing n > 8: RMT's TrainingMenuFunc.Function
// pre-hook calls rec_arm), watched on the frame callback (every 10 frames in training, also under the pause:
// RecordFunc._gData.State), finalized there or at the menu open (RMT's CreateViewData pre-hook calls rec_menu_open).
// No game hook of its own. The ALLOC / WRITE_DATA queue of the imports runs with the files layer (layer_files.cpp).
//
// Uncertain without the game (not probed yet): starting a recording without the menu's F.
// Kept off behind settings.json { "experimental": { "record_start_without_menu": true } } (rec_start: arm, then at the
// unpause RecordFunc.ChangeRecordSlot(7) + SetRecordShortCut()). The fallback is on: rec_capture8(n) stores slot 8
// (recorded with the game's own Record page) into n. Neither is called by today's Lua UI.
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <reframework/API.hpp>

#include "common.hpp"
#include "game/gm.hpp"
#include "mech/mech.hpp"
#include "model/files.hpp"
#include "model/record.hpp"
#include "model/vslots.hpp"

namespace mech::record {
namespace {

namespace gm = game::gm;
using F = cmn::Feature;
using model::record::kWorkSlot;

class GamePort final : public model::record::Port {
public:
    std::optional<json> read8() override {
        std::string err;
        return files::engine().read_slot(kWorkSlot, &err);
    }
    void write8(const json& e) override { files::engine().write_slot(e); }
    const json* vslot(int n) override { return files::engine().vslots().get(n); }
    void store(int n, const json& e) override { files::engine().vslot_put(n, e, true); }
    void own8_back() override { mech::replay::own8_back(); }
    int64_t frames(const json& e) override {
        model::vslots::VSlots v;
        v.put(9, e);
        return v.frames(9);
    }
};

GamePort g_port;
model::record::Engine g_eng;
bool g_start_enabled = false;                    // settings.json experimental.record_start_without_menu
bool g_start_pending = false;
uint64_t g_arms = 0, g_finals = 0, g_captures = 0;

int64_t record_state() {                         // RMT:526-529: 0 when unreadable
    gm::MO* rf = gm::call_obj(gm::singleton("app.training.TrainingManager"), "get_RecordFunc");
    int64_t v = 0;
    if (!rf || !gm::get_int(gm::ref(rf, "_gData"), "State", &v)) return 0;
    return v;
}

void arm_now(int n) {
    mech::replay::unload_for_record();            // slot 8 = its own content before the snapshot (RMT:514)
    g_eng.arm(g_port, n, cmn::frame());
    ++g_arms;
    cmn::journal("record: virtual recording %d through slot 8", n);
}

#define REC_FN(name) int32_t SF6CORE_CALL name(void*, const SF6CoreValue* a, uint32_t n, SF6CoreValue* r, uint32_t max)
#define REC_TRY std::lock_guard<std::recursive_mutex> lk_(mech::lock()); br::Out o{ r, max }; (void)a; (void)n; try {
#define REC_END(what) } catch (...) { cmn::fault(F::Record, what); return o.done(); } return o.done();

// rec_state(): armed, slot, RECORD_NOW seen (the Lua's `vrec` view for its F handler)
REC_FN(f_state) { REC_TRY o.boolean(g_eng.armed()); o.i64(g_eng.slot()); o.boolean(g_eng.seen_now()); REC_END("state") }
REC_FN(f_arm) { REC_TRY arm_now((int)br::i64(a, n, 0, 0)); o.boolean(true); REC_END("arm") }
REC_FN(f_retarget) {
    REC_TRY
    g_eng.retarget((int)br::i64(a, n, 0, 0));
    cmn::journal("record: pending recording re-targeted to slot %d", g_eng.slot());
    REC_END("retarget")
}
REC_FN(f_drop) { REC_TRY g_eng.drop(); cmn::journal("record: pending virtual recording dropped"); REC_END("drop") }
REC_FN(f_finalize) {
    REC_TRY
    const int s = g_eng.slot();
    g_eng.finalize(g_port, br::boolean(a, n, 0));
    ++g_finals;
    cmn::journal("record: finalised: virtual %d", s);
    REC_END("finalize")
}
REC_FN(f_menu_open) {
    REC_TRY
    if (g_eng.armed() && g_eng.seen_now()) { g_eng.menu_open(g_port); ++g_finals; cmn::journal("record: menu opened while recording: stored, slot 8 restored"); }
    REC_END("menu_open")
}
REC_FN(f_capture8) {
    REC_TRY
    const bool ok = g_eng.capture8(g_port, (int)br::i64(a, n, 0, 0));
    if (ok) ++g_captures;
    o.boolean(ok);
    REC_END("capture8")
}
// rec_start(n): the uncertain path, off unless settings.json enables it
REC_FN(f_start) {
    REC_TRY
    if (!g_start_enabled) { o.str("disabled"); return o.done(); }
    arm_now((int)br::i64(a, n, 0, 0));
    g_start_pending = true;
    o.str("armed");
    REC_END("start")
}

int label() { return cmn::owns(F::Record) ? g_eng.label() : 0; }

void frame(uint64_t fr) {
    if (!cmn::owns(F::Record)) return;
    std::lock_guard lk(mech::lock());
    try {
        const State st = mech::state();
        if (!st.training) return;
        if (g_eng.armed() && fr % model::record::kTickEvery == 0) {     // the Lua's gate: the watch runs while armed
            const int s = g_eng.slot();
            const bool paused = mech::pause_now().bit;                   // (read on the watch's frames only)
            if (g_eng.tick(g_port, fr, record_state(), paused)) {
                ++g_finals;
                cmn::journal("record: finalised: virtual %d stored, slot 8 restored (paused=%d)", s, paused ? 1 : 0);
            }
        }
        if (g_start_pending && !mech::pause_now().bit) {               // experimental: the game's shortcut record start
            g_start_pending = false;
            gm::MO* rf = gm::call_obj(gm::singleton("app.training.TrainingManager"), "get_RecordFunc");
            if (rf) {
                gm::TD* t = rf->get_type_definition();
                gm::call(rf, gm::method_n(t, "ChangeRecordSlot", 1), { gm::arg_i32(kWorkSlot) });
                gm::call(rf, gm::method_n(t, "SetRecordShortCut", 0), {});
                cmn::journal("record: experimental start (ChangeRecordSlot(7) + SetRecordShortCut)");
            }
        }
    } catch (...) {
        cmn::fault(F::Record, "frame");
    }
}

void hand_back() {
    std::lock_guard lk(mech::lock());
    try { g_eng.hand_back(g_port); } catch (...) {}
    cmn::log_warn("[CRSMNative] record: back to the Lua (a pending recording dropped, slot 8 restored)");
}

std::string status() {
    std::lock_guard lk(mech::lock());
    char b[240];
    std::snprintf(b, sizeof(b), "%s, armed %d (slot %d), arms %llu, finalised %llu, captures %llu, start without menu %s",
                  cmn::owns(F::Record) ? "native" : "lua", g_eng.armed() ? 1 : 0, g_eng.slot(), (unsigned long long)g_arms,
                  (unsigned long long)g_finals, (unsigned long long)g_captures, g_start_enabled ? "ENABLED (experimental)" : "off");
    return b;
}

}  // namespace

View view() {
    std::lock_guard lk(mech::lock());
    return View{ g_eng.armed(), g_eng.slot(), g_eng.seen_now() };
}
void arm(int n) {
    std::lock_guard lk(mech::lock());
    try { arm_now(n); } catch (...) { cmn::fault(F::Record, "arm (ui)"); }
}
void retarget(int n) {
    std::lock_guard lk(mech::lock());
    g_eng.retarget(n);
    cmn::journal("record: pending recording re-targeted to slot %d", g_eng.slot());
}
void drop() {
    std::lock_guard lk(mech::lock());
    g_eng.drop();
    cmn::journal("record: pending virtual recording dropped");
}
void finalize(bool paused) {
    std::lock_guard lk(mech::lock());
    try {
        const int s = g_eng.slot();
        g_eng.finalize(g_port, paused);
        ++g_finals;
        cmn::journal("record: finalised: virtual %d", s);
    } catch (...) {
        cmn::fault(F::Record, "finalize (ui)");
    }
}
void menu_open() {
    std::lock_guard lk(mech::lock());
    try {
        if (g_eng.armed() && g_eng.seen_now()) { g_eng.menu_open(g_port); ++g_finals; cmn::journal("record: menu opened while recording: stored, slot 8 restored"); }
    } catch (...) {
        cmn::fault(F::Record, "menu_open (ui)");
    }
}

void init() {
    cmn::set_implemented(F::Record);
    cmn::on_hand_back(F::Record, hand_back);
    cmn::add_status("record", status);
    const struct { const char* name; SF6CoreLuaFn fn; } fns[] = {
        { "rec_state", f_state }, { "rec_arm", f_arm }, { "rec_retarget", f_retarget }, { "rec_drop", f_drop },
        { "rec_finalize", f_finalize }, { "rec_menu_open", f_menu_open }, { "rec_capture8", f_capture8 }, { "rec_start", f_start },
    };
    for (const auto& f : fns) cmn::bridge_fn(f.name, f.fn);
    cmn::add_frame_fn(frame, "layer_record");
    mech::set_label_source(0, label);
}

void start() {
    if (!cmn::owns(F::Record)) return;
    try {
        std::ifstream f("reframework/data/SF6_CRSMNative_data/settings.json", std::ios::binary);
        const json j = json::parse(f, nullptr, false);
        if (!j.is_discarded() && j.contains("experimental") && j["experimental"].is_object()) {
            const auto it = j["experimental"].find("record_start_without_menu");
            g_start_enabled = it != j["experimental"].end() && it->is_boolean() && it->get<bool>();
        }
    } catch (...) {}
    cmn::log_info("[CRSMNative] record: native (virtual recording through slot 8, finalize, slot 8 restored)%s",
                  g_start_enabled ? "; EXPERIMENTAL start without the menu enabled" : "");
}

}  // namespace mech::record
