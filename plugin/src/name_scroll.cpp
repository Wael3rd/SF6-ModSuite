// SF6 Slots -- long names scroll inside their place instead of being squeezed or cut (scroll_logic.hpp).
//
// Where: the Costume rows (Fighter Settings, the character select's panels, Training's popup, Opponent Outfit Settings) and the Opponent
// Outfit summary's lines, from here; Replays > Settings' summary lines and its banner's name (replay_tab.cpp); the banner's name on Battle
// Settings' and Training's stage grids (stages.cpp).
//
// How, as the game's own scrolling line (app.UIPartsScrollText): the text's Position moves and its ScissorRect, local to the text, moves the
// other way. A text that fits is left as the game has it. A text wider than its place: its AutoRegionFit and AutoWrap off (the game's squeezes
// it), placed and aligned by its left point on its row, then only Position and ScissorRect are written, and only when the whole offset
// changes. Its region stays the game's, as in the game's own scroller: the costume options and the Color Mod's DI mode save and restore it, and
// measure their rows on it. No text is ever written here: the game's message stays what it is, so nothing mixes a message id and a plain text.
// All of it is put back when the text changes, hides or the row's owner takes it.
//
// To measure, the text's fit is turned off and its region widened at the same anchor for two frames, which does not move a text that fits. A
// width once measured is kept for that control and text, so the next time needs no such frames.
//
// R1: every write from LateUpdateBehavior, through the core. R35: the controls are found by name each frame; an address is kept for
// comparison only, never used to reach an object. A row whose panel is claimed (SF6Slots_SelectPanelClaim: the costume options or the Color
// Mod's DI mode borrow the rows) is left to its owner.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "name_scroll.hpp"
#include "rto.hpp"
#include "scroll_logic.hpp"

#include <cstring>
#include <map>
#include <unordered_map>

namespace slots::namescroll {
namespace {

constexpr uint32_t kPollEvery = 15, kLayoutFrames = 2, kMeasureMax = 30, kForgetFrames = 600;
constexpr uint32_t kGapFrames = 5;          // not looked at for longer: the control at this address may be another one
constexpr float kMeasureRegionW = 4000;     // wider than any name
constexpr size_t kWidthsKept = 64;
constexpr int kSummaryItems = 40;           // Opponent Outfit Settings' summary: its lines, at most
constexpr const char* kFitSig = "set_AutoRegionFit(via.gui.RegionFitType)";

struct Found {                              // the text as it was found
    bool have = false;
    float x = 0, y = 0, z = 0, region_w = 0, region_h = 0;
    float rect[4] = { 0, 0, -1, -1 };
    int64_t cp = 0, align = 0, fit = 0;
    bool wrap = false;
};
struct State {
    std::string msg;
    Found found;
    std::map<std::string, float> widths;    // what this control measured
    uint32_t changed_at = 0, born = 0, seen = 0, checked = 0;
    bool looked = false;                    // msg is what the control holds
    bool measuring = false, sliding = false, clipped = false;
    float width = 0, natural = 0, left = 0, last_x = 0;
    bool rtl = false;
    uint64_t t0 = 0;
    int last = -1;
};
std::unordered_map<uintptr_t, State> g_states;
uint32_t g_frame = 0;
bool g_any = false;

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

bool floats_of(Obj* o, const char* m, float* out, size_t n) {
    const Ret r = call(o, m);
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), sizeof(float) * n);
    return true;
}
void set_i64(void* c, const char* sig, int64_t v) {
    SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = v;
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void set_rect(void* c, const float rect[4]) {
    float r[4] = { rect[0], rect[1], rect[2], rect[3] };
    SF6CoreArg a{}; a.type = SF6CORE_ARG_PTR; a.v.p = r;      // via.Rect goes by address, as every value type
    A()->obj_invoke(C(), c, "set_ScissorRect(via.Rect)", &a, 1, nullptr);
}
void no_fit(Obj* t, const Found& f) {
    set_i64(t, kFitSig, 0);
    if (f.wrap) set_i64(t, "set_AutoWrap(System.Boolean)", 0);
}
bool capture(Obj* t, Found& f) {
    float p[3], r[2];
    if (!floats_of(t, "get_Position", p, 3) || !floats_of(t, "get_RegionSize", r, 2)) return false;
    const Ret cp = call(t, "get_ControlPoint"), al = call(t, "get_PageAlignment"), fit = call(t, "get_AutoRegionFit");
    if (cp.exception_thrown || al.exception_thrown || fit.exception_thrown) return false;
    f.x = p[0]; f.y = p[1]; f.z = p[2]; f.region_w = r[0]; f.region_h = r[1];
    f.cp = static_cast<int32_t>(cp.dword); f.align = static_cast<int32_t>(al.dword); f.fit = static_cast<int32_t>(fit.dword);
    if (!floats_of(t, "get_ScissorRect", f.rect, 4)) { f.rect[0] = f.rect[1] = 0; f.rect[2] = f.rect[3] = -1; }
    f.wrap = call_bool(t, "get_AutoWrap");
    f.have = true;
    return true;
}
void put_back(Obj* t, State& s) {
    if (s.found.have) {
        const Found& f = s.found;
        if (s.clipped) set_rect(t, f.rect);
        if (s.sliding) {
            set_i64(t, "set_ControlPoint(via.gui.ControlPoint)", f.cp);
            set_i64(t, "set_PageAlignment(via.gui.PageAlignment)", f.align);
            A()->ui_set_position(C(), t, f.x, f.y, f.z);
        }
        if (s.measuring) A()->ui_set_region_size(C(), t, f.region_w, f.region_h);
        set_i64(t, kFitSig, f.fit);
        if (f.wrap) set_i64(t, "set_AutoWrap(System.Boolean)", 1);
    }
    s.found = Found{};
    s.measuring = s.sliding = s.clipped = false;
    s.last = -1;
}
// Its fit off, its region wide, at the same anchor: a text that fits does not move. A text placed by its left point is clipped at once: what
// doesn't fit never shows, not even those frames.
bool open_for_measure(Obj* t, State& s, float width) {
    if (!s.found.have && !capture(t, s.found)) return false;
    no_fit(t, s.found);
    A()->ui_set_region_size(C(), t, kMeasureRegionW, s.found.region_h);
    s.measuring = true;
    const float place = width > 0 ? width : s.found.region_w;
    if (s.found.cp % 4 == 0 && s.found.align % 4 == 0 && place > 0) {
        const scroll::Window w = scroll::window_at(s.found.x, place, 0);
        const float rect[4] = { w.clip_left, w.clip_top, w.clip_right, w.clip_bottom };
        set_rect(t, rect);
        s.clipped = true;
    }
    return true;
}
void start_slide(Obj* t, State& s, float natural, float width) {
    if (!s.found.have && !capture(t, s.found)) return;
    const Found& f = s.found;
    s.width = width > 0 ? width : f.region_w;
    s.natural = natural;
    s.left = scroll::left_of(f.x, static_cast<int>(f.cp), s.width);
    s.rtl = scroll::text_is_rtl(s.msg);
    no_fit(t, f);
    set_i64(t, "set_ControlPoint(via.gui.ControlPoint)", (f.cp / 4) * 4);
    set_i64(t, "set_PageAlignment(via.gui.PageAlignment)", (f.align / 4) * 4);
    if (s.measuring) A()->ui_set_region_size(C(), t, f.region_w, f.region_h);
    s.measuring = false;
    s.sliding = s.clipped = true;
    s.t0 = GetTickCount64();
    s.last = -1;
}
void slide_step(Obj* t, State& s) {
    const double secs = static_cast<double>(GetTickCount64() - s.t0) / 1000.0;
    const int off = scroll::offset_at(s.natural - s.width, secs, s.rtl);
    if (off == s.last) return;
    s.last = off;
    const scroll::Window w = scroll::window_at(s.left, s.width, off);
    const float rect[4] = { w.clip_left, w.clip_top, w.clip_right, w.clip_bottom };
    set_rect(t, rect);
    A()->ui_set_position(C(), t, w.x, s.found.y, s.found.z);
    s.last_x = w.x;
}
// Not looked at for a while: still where it was slid, the same control. Otherwise nothing of this state is its own.
bool still_ours(Obj* t, const State& s) {
    float p[3] = {};
    return floats_of(t, "get_Position", p, 3) && p[0] == s.last_x;
}
void sweep() {
    static uint32_t at = 0;
    if (g_frame - at < kForgetFrames) return;
    at = g_frame;
    for (auto it = g_states.begin(); it != g_states.end();)
        it = g_frame - it->second.seen > kForgetFrames ? g_states.erase(it) : std::next(it);
}

bool drawn(Obj* agent) {
    Obj* go = call_obj(agent, "get_GameObject");
    bool ok = false;
    const bool d = go && call_bool(go, "get_DrawSelf", {}, &ok);
    return ok && d;
}
bool shown(Obj* c) {
    bool ok = false;
    const bool v = c && call_bool(c, "get_ActualVisible", {}, &ok);
    return ok && v;
}
struct Screen { const char* agent; int side; bool summary; };   // side: the panel's claim (0: none, 3: either side, Training's popup)
const Screen kScreens[] = {
    { "MatchingFighterSetting", 0, false },
    { "OtherMatchingFighterSetting", 0, true },
    { "ui10505", 1, false },
    { "ui10507", 2, false },
    { "GenericCharacterDetailSetting_FG", 3, false },
};
bool claimed(int side) {
    if (side == 3) return rto::select::claim(1, 0, -1) != 0 || rto::select::claim(2, 0, -1) != 0;
    return side != 0 && rto::select::claim(side, 0, -1) != 0;
}

}  // namespace

void drive(void* text, float width, uint32_t poll, bool allowed) {
    if (!text || !A() || !C()) return;
    Obj* t = static_cast<Obj*>(text);
    State& s = g_states[reinterpret_cast<uintptr_t>(text)];
    if (s.seen && g_frame - s.seen > kGapFrames && s.sliding && !still_ours(t, s)) s = State{};
    s.seen = g_frame;
    if (s.looked && !s.sliding && !s.measuring && poll > 1 && g_frame - s.checked < poll) return;
    s.checked = g_frame;
    if (!allowed || !shown(t)) {
        if (s.found.have) put_back(t, s);
        s.looked = false;
        return;
    }
    const std::string msg = str(call_obj(t, "get_Message"));
    if (!s.looked || msg != s.msg) {
        if (s.found.have) put_back(t, s);
        s.msg = msg;
        s.looked = true;
        s.changed_at = s.born = g_frame;
        if (msg.empty()) return;
        auto known = s.widths.find(msg);
        if (known != s.widths.end()) {
            float w = width;
            if (w <= 0) { float r[2] = {}; floats_of(t, "get_RegionSize", r, 2); w = r[0]; }
            if (scroll::overflows(known->second, w)) start_slide(t, s, known->second, width);
        } else if (!open_for_measure(t, s, width)) return;
    }
    if (s.measuring) {
        if (g_frame < s.changed_at + kLayoutFrames) return;
        float d[2] = {};
        const bool got = floats_of(t, "get_DisplaySize", d, 2) && std::isfinite(d[0]) && d[0] > 0;
        if (!got && g_frame - s.born < kMeasureMax) return;
        const float place = width > 0 ? width : s.found.region_w;
        if (got) {
            if (s.widths.size() >= kWidthsKept) s.widths.clear();
            s.widths[msg] = d[0];
        }
        if (got && scroll::overflows(d[0], place)) start_slide(t, s, d[0], width);
        else put_back(t, s);
    }
    if (s.sliding) slide_step(t, s);
    sweep();
}

void release(void* text) {
    if (!text || !A() || !C()) return;
    auto it = g_states.find(reinterpret_cast<uintptr_t>(text));
    if (it == g_states.end()) return;
    if (it->second.found.have) put_back(static_cast<Obj*>(text), it->second);
    it->second.looked = false;
}

void release_unless(void* text, const std::string& message) {
    auto it = g_states.find(reinterpret_cast<uintptr_t>(text));
    if (it != g_states.end() && it->second.looked && it->second.msg != message) release(text);
}

bool busy(void* text) {
    auto it = g_states.find(reinterpret_cast<uintptr_t>(text));
    return it != g_states.end() && (it->second.sliding || it->second.measuring);
}

bool busy(void* text, float x, float y) {
    auto it = g_states.find(reinterpret_cast<uintptr_t>(text));
    if (it == g_states.end() || (!it->second.sliding && !it->second.measuring)) return false;
    State& s = it->second;
    if (s.found.have && (s.found.x != x || s.found.y != y)) {
        s.found.x = x; s.found.y = y;
        if (s.sliding) { s.left = scroll::left_of(x, static_cast<int>(s.found.cp), s.width); s.last = -1; }
        else A()->ui_set_position(C(), text, x, y, s.found.z);
    }
    return true;
}

void frame() {
    ++g_frame;
    if (!g_any && (g_frame % kPollEvery) != 0) return;
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, ui_agents) || !SF6CORE_HAS(a, ui_find_cached) || !SF6CORE_HAS(a, ui_find_child)) return;
    try {
        g_any = false;
        for (const Screen& sc : kScreens) {
            void* buf[2] = {};
            const uint32_t n = a->ui_agents(sc.agent, buf, 2);
            for (uint32_t i = 0; i < n && i < 2; ++i) {
                Obj* agent = static_cast<Obj*>(buf[i]);
                Obj* cm = call_obj(agent, "get_ControlMain");
                if (!cm) continue;
                const bool on = drawn(agent) && shown(cm);
                g_any = g_any || on;
                void* spin = a->ui_find_cached(cm, "p_Spin_costume");
                if (void* text = spin ? a->ui_find_cached(spin, "e_text") : nullptr) drive(text, 0, 1, on && !claimed(sc.side));
                if (!sc.summary) continue;
                // Opponent Outfit Settings' summary: its lines, "<outfit>: <colours on>"
                Obj* list = static_cast<Obj*>(a->ui_find_cached(cm, "p_ScrollList_texts_v"));
                int guard = 0;
                for (Obj* item = list ? call_obj(list, "get_Child") : nullptr; item && guard < kSummaryItems; item = call_obj(item, "get_Next"), ++guard)
                    if (void* line = a->ui_find_child(item, "e_text_costume", 3)) drive(line, 0, 1, on);
            }
        }
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logf("name scroll: EXCEPTION in a frame"); }
    }
}

}  // namespace slots::namescroll
