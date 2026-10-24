// ui/hud — the REC / replay HUD says "Slot n" for a virtual slot riding live slot 8 (phase P4, owner "ui"),
// RecordingManagerTab.lua (RMT) 566-596. The game labels app.training.UIWidget_TMRecording from one function of the HUD's
// update (code 0x143958E90: its four SetText(Guid, Object[]) calls, widgets 1 and 2 = TMRecording, 0 = the countdown);
// the Lua's hook on SetText(Guid, Object[]) is refused here (R18: its code is shared with TMRecordCountDown.SetReadyText).
//
// Route 1A: a post-hook on app.training.tf_RecordSetting.UpdateRecord(), the labelling function, when its code is its own
// (R18, the hooks' code scan). It was first found by its code address (0x143958E90 in the build of 11/10: the scan named
// UpdateRecord, the only method entering it; its caller 0x1439C6A70 = tf_RecordSetting.bUpdate); the game update of
// 13/10 moved the code, the address no longer named a method and the HUD fell back to route 1B ("Slot 8" kept): the
// method is now found by its name. Right after the game's own labels, in the same call chain (no fight), the TMRecording
// widgets showing the game's label of the work slot (8) get "Slot n" (mech::hud_label: a recording armed by the record
// layer, or the replay layer's virtual in slot 8). The widgets are found without a hook, from
// TrainingManager._ViewUIWigetDict (as NativeHud.lua did), on the game thread, when a label starts (0 -> n, n -> m) and
// after each mech event (flow change, training edge, menu close), checked by type then and held (add_ref) with their label
// texts until the label goes back to 0: no object check per frame. The text written is their label field (via.gui.Text at
// +0x40) through the core's ui_set_text, compared first (R7): per frame, one read of each held text (the game writes its
// label every frame), a write only where it shows the work slot. The countdown widget (another type) is never touched;
// SlotID is never written (recording goes through slot 8).
// Route 1B (that method shared or not found): the same write from the frame callback, once per game label; if the game
// puts its label back within the next frame three times running, it writes after the frame callback: the HUD keeps
// "Slot 8" (one log line, no per-frame fight).
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "common.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "internal.hpp"
#include "mech/hooks.hpp"
#include "mech/mech.hpp"

namespace ui::hud {
namespace {

namespace gm = game::gm;
using gx::MO;
using API = reframework::API;

constexpr const char* kLabelsType = "app.training.tf_RecordSetting";   // the HUD update's labelling function (its 4 SetText
constexpr const char* kLabelsSig = "UpdateRecord()";                    // calls), called by tf_RecordSetting.bUpdate
constexpr const char* kWidgetType = "app.training.UIWidget_TMRecording";
constexpr uint32_t kLabelOffset = 0x40;         // the label text of the widget (the survey notes)
constexpr int kRevertsToStop = 3;               // route 1B: the game's label back 3 frames running = it writes after us

bool ok_labels = false;                         // route 1A in place

std::mutex g_mx;                                // the widget cache (the hook's thread / the frame)
gm::TD* g_td = nullptr;                         // app.training.UIWidget_TMRecording
const char* g_field = nullptr;                  // the label field's name (+0x40, via.gui.Text), read once from the type
bool g_field_read = false;
MO* g_w[4]{};                                   // the TMRecording widgets of the HUD (held while a label runs)
MO* g_t[4]{};                                   // their label texts (held likewise)
int g_wn = 0;
bool g_cache = false;                           // g_w / g_t were resolved for this label and these mech events
uint64_t g_cache_events = 0;
reframework::API::Method* g_get_message = nullptr;   // via.gui.Text.get_Message (found once)
constexpr int kStaleEvery = 10;                 // route 1A's check from the frame: one frame in 10
std::atomic<uint64_t> g_writes{0}, g_resolves{0}, g_skips_same{0};
// route 1B
MO* g_last_t = nullptr;                         // the text written last frame
int g_reverts = 0;
bool g_gave_up = false;                         // until the label goes back to 0
int g_last_label = 0;
int g_resolve_wait = 0;                         // frames before the widgets are looked for again (none found)
int g_stale = 0;                                // route 1A: checks in a row where the game's slot-8 label stayed (the post-hook did not label)
int g_stale_tick = 0;
bool g_1a_off = false;                          // ... past 30 frames (3 checks): route 1B for this recording / replay
constexpr int kStaleTo1B = 30;

// The label field: the via.gui.Text field at +0x40 of the widget type, else its first via.gui.Text field.
void read_field() {
    if (g_field_read) return;
    g_field_read = true;
    g_td = gm::type(kWidgetType);
    if (!g_td) { cmn::journal("ui: HUD label: type %s not found", kWidgetType); return; }
    const char* first = nullptr;
    uint32_t first_off = 0;
    for (API::TypeDefinition* t = g_td; t && !g_field; t = t->get_parent_type()) {
        for (API::Field* f : t->get_fields()) {
            if (!f || f->is_static()) continue;
            API::TypeDefinition* ft = f->get_type();
            if (!ft || std::strcmp(ft->get_full_name().c_str(), "via.gui.Text") != 0) continue;
            if (f->get_offset_from_base() == kLabelOffset) { g_field = f->get_name(); break; }
            if (!first) { first = f->get_name(); first_off = f->get_offset_from_base(); }
        }
    }
    if (!g_field && first) {
        g_field = first;
        cmn::journal("ui: HUD label: no via.gui.Text at +0x%X of %s, its first one used: %s (+0x%X)", kLabelOffset, kWidgetType,
                     first, first_off);
    } else if (g_field) {
        cmn::journal("ui: HUD label: %s.%s (+0x%X, via.gui.Text)", kWidgetType, g_field, kLabelOffset);
    } else {
        cmn::journal("ui: HUD label: %s has no via.gui.Text field", kWidgetType);
    }
}

bool alive(MO* w) { return w && gm::is_managed(w) && w->get_type_definition() == g_td; }

// A held label text's message (its get_Message, found once): one game call.
std::string text_of(MO* t) {
    if (!t) return "nil";
    if (!g_get_message) g_get_message = gm::method(t->get_type_definition(), "get_Message");
    reframework::InvokeRet r{};
    if (!g_get_message || !gm::call(t, g_get_message, {}, &r) || !r.ptr) return "nil";
    return gx::str(r.ptr);
}

void cache_drop() {                                               // under g_mx
    for (int i = 0; i < g_wn; ++i) {
        if (g_t[i]) g_t[i]->release();
        if (g_w[i]) g_w[i]->release();
        g_w[i] = g_t[i] = nullptr;
    }
    g_wn = 0;
    g_cache = false;
    g_last_t = nullptr;                                           // (one of the texts let go)
}

bool cache_ok() { return g_cache && g_wn > 0 && g_cache_events == mech::events(); }

// TrainingManager._ViewUIWigetDict._entries[i].value: the HUD's widget lists (NativeHud.lua resolve_panel). The entries
// are value types: GetValue boxes one; its "value" sits after the boxed object's header. Game thread only.
void resolve() {
    cache_drop();
    ++g_resolves;
    g_cache = true;
    g_cache_events = mech::events();
    MO* dict = gm::ref(gm::singleton("app.training.TrainingManager"), "_ViewUIWigetDict");
    MO* entries = gm::ref(dict, "_entries");
    const int32_t n = gm::array_len(entries);
    for (int32_t i = 0; i < n && g_wn < 4; ++i) {
        MO* box = gm::array_get(entries, i);
        if (!box || !gm::is_managed(box)) continue;
        API::TypeDefinition* et = box->get_type_definition();
        API::Field* fv = et ? et->find_field("value") : nullptr;
        if (!fv || fv->is_static()) continue;
        MO* list = *reinterpret_cast<MO**>(reinterpret_cast<uint8_t*>(box) + 0x10 + fv->get_offset_from_fieldptr());
        if (!list || !gm::is_managed(list)) continue;
        const int32_t m = gm::list_count(list);
        for (int32_t j = 0; j < m && g_wn < 4; ++j) {
            MO* w = gm::list_item(list, j);
            if (!alive(w)) continue;
            MO* t = g_field ? gm::ref(w, g_field) : nullptr;
            if (!t || !gm::is_managed(t)) continue;
            w->add_ref();                                         // held until the label goes back to 0 (or the next resolve)
            t->add_ref();
            g_w[g_wn] = w;
            g_t[g_wn] = t;
            ++g_wn;
        }
    }
}

// The game's label of the work slot: its last number is 8 (the widget shows SlotID + 1).
bool shows_work_slot(const std::string& m) {
    size_t e = m.find_last_of("0123456789");
    if (e == std::string::npos) return false;
    size_t b = e;
    while (b > 0 && m[b - 1] >= '0' && m[b - 1] <= '9') --b;
    return m.compare(b, e - b + 1, "8") == 0;
}

// The cached widgets showing the game's slot-8 label get "Slot n". Returns the text written (or null).
MO* relabel(int n) {
    if (!g_field) return nullptr;
    const std::string want = "Slot " + std::to_string(n);       // the Lua's own wording here (RMT:592)
    MO* wrote = nullptr;
    for (int i = 0; i < g_wn; ++i) {
        MO* t = g_t[i];
        if (!t) continue;
        const std::string cur = text_of(t);
        if (cur == want) { ++g_skips_same; continue; }
        if (!shows_work_slot(cur)) continue;
        gx::set_text(t, want);                                    // the core's ui_set_text: compared first as well (R7)
        ++g_writes;
        wrote = t;
    }
    return wrote;
}

// ---- route 1A: post-hook on the labelling function ----------------------------------------------------------------------
void post_body() {
    const int n = mech::hud_label();
    if (n == 0) return;
    std::lock_guard lk(g_mx);
    if (!cache_ok()) return;                                      // resolved on the frame callback (game thread)
    relabel(n);
}
bool post_seh() {
    __try { post_body(); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}
void post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (cmn::closing() || !ui::active()) return;
    if (!post_seh()) ui::fault("HUD labels post (SEH)");
}

}  // namespace

void specs(std::vector<mech::hooks::Spec>& out) {
    mech::hooks::Spec s{ cmn::Feature::Ui, kLabelsType, kLabelsSig, nullptr, post,
                         "the REC / replay HUD label follows from the frame callback (route 1B)", false };
    s.installed = &ok_labels;
    out.push_back(s);
}

// Frame callback (game thread, ui::lock held): the widget cache (both routes) and route 1B's write.
void frame() {
    const int n = mech::hud_label();
    std::lock_guard lk(g_mx);
    if (n != g_last_label) {
        if (n != 0) cmn::journal("ui: HUD label: virtual slot %d riding slot 8 (route %s)", n, ok_labels ? "1A, post-hook" : "1B, frame");
        g_last_label = n;
        g_gave_up = false;
        g_reverts = 0;
        g_last_t = nullptr;
        g_resolve_wait = 0;
        g_stale = 0;
        g_stale_tick = 0;
        g_1a_off = false;
        g_cache = false;                                          // the widgets looked for again for this label
    }
    if (n == 0) { if (g_wn) cache_drop(); return; }
    read_field();
    if (!g_field) return;
    if (!cache_ok()) {
        if (g_resolve_wait > 0) { --g_resolve_wait; return; }    // not found: looked for again once a second
        resolve();
        if (g_wn == 0) g_resolve_wait = 60;
        static int logged = -1;                                   // once per count seen (0 = not found)
        if (logged != g_wn) {
            logged = g_wn;
            cmn::journal("ui: HUD label: %d %s widget(s) found in TrainingManager._ViewUIWigetDict", g_wn, kWidgetType);
        }
        if (g_wn == 0) return;
    }
    if (g_gave_up) return;
    if (ok_labels && !g_1a_off) {                                 // route 1A writes from its post-hook: checked here, 1 frame in 10
        if (++g_stale_tick < kStaleEvery) return;
        g_stale_tick = 0;
        bool stale = false;
        for (int i = 0; i < g_wn && !stale; ++i) stale = g_t[i] && shows_work_slot(text_of(g_t[i]));
        g_stale = stale ? g_stale + 1 : 0;
        if (g_stale * kStaleEvery <= kStaleTo1B) return;
        g_1a_off = true;
        cmn::log_info("[CRSMNative] ui: HUD label: the post-hook did not label the widget within %d frames: route 1B for this "
                      "recording / replay", kStaleTo1B);
    }
    if (g_last_t && shows_work_slot(text_of(g_last_t))) {        // the game's label is back (g_last_t: a held text)
        if (++g_reverts >= kRevertsToStop) {
            g_gave_up = true;
            cmn::log_info("[CRSMNative] ui: HUD label (route 1B): the game writes its label after the frame callback: "
                          "\"Slot 8\" kept for this %s (no per-frame fight)", "recording / replay");
            return;
        }
    } else if (g_last_t) {
        g_reverts = 0;
    }
    MO* t = relabel(n);
    if (t) g_last_t = t;
}

void hand_back() {
    std::lock_guard lk(g_mx);
    cache_drop();
}

std::string status() {
    char b[256];
    std::snprintf(b, sizeof(b), "route %s, field %s, widgets %d, writes %llu, resolves %llu%s",
                  ok_labels ? (g_1a_off ? "1A installed, 1B now (the post did not label)" : "1A (post-hook)") : "1B (frame)",
                  g_field ? g_field : "-", g_wn, (unsigned long long)g_writes.load(), (unsigned long long)g_resolves.load(),
                  g_gave_up ? ", 1B gave up (the game writes after the frame)" : "");
    return b;
}

}  // namespace ui::hud
