// SF6 Slots -- long names scroll inside their place instead of being squeezed or cut (scroll_logic.hpp).
//
// Where: the Costume rows (Fighter Settings, the character select's panels, Training's popup, Opponent Outfit Settings), the Opponent
// Outfit summary's lines and every row of the option window (the game's OptionDialog: the core's option menus - costume options, Report a
// bug, the Interface sections - and the game's own: a value between its arrows, a title in its region; Wael 09/11, "keep the whole name,
// let it scroll") and every row of the Options screen's right list (its own and the interface packs' sections, 10/11), from here; Replays > Settings' summary lines and its banner's name (replay_tab.cpp); the banner's name on Battle
// Settings' and Training's stage grids (stages.cpp).
//
// A Costume row's place runs between its two arrows, read on that row (Wael, 07/11: the text's region reaches past the arrows on the character
// select and stops well inside them on Opponent Outfit Settings). There a name:
// - that fits the place and its region is left as the game has it;
// - that fits the place but not its region stays where it is, at full size: only its AutoRegionFit and AutoWrap off;
// - that is wider than the place slides inside it.
//
// How it slides, as the game's own scrolling line (app.UIPartsScrollText): the text's Position moves and its ScissorRect, local to the text,
// moves the other way. Its AutoRegionFit and AutoWrap off (the game's squeezes it), placed and aligned by its left point on its row, then only
// Position and ScissorRect are written, and only when the whole offset changes. Its region stays the game's, as in the game's own scroller: the
// costume options and the Color Mod's DI mode save and restore it, and measure their rows on it. No text is ever written here: the game's
// message stays what it is, so nothing mixes a message id and a plain text. All of it is put back when the text changes, hides or the row's
// owner takes it.
//
// On the character select's panels and Training's popup the row's focus animation (states FOCUS / SELECT, UNFOCUS / DEFAULT) writes the text's
// Position back to the game's, and its font size: 443 wide with the focus, 407 without on the same name (read in game, 07/11). A state's first write
// comes inside LateUpdateBehavior, the next ones after the frame is drawn. So while a name slides its place is read before that phase and after
// it (post_frame), and put back when moved: the frame drawn has it right, and the name doesn't wait for its next offset, which comes only after a
// rest. Its width is read too, and followed once it holds still: the slide then goes on from where it is, to a new end.
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

#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace slots::namescroll {
namespace {

constexpr uint32_t kPollEvery = 15, kLayoutFrames = 2, kMeasureMax = 30, kForgetFrames = 600;
constexpr uint32_t kGapFrames = 5;          // not looked at for longer: the control at this address may be another one
constexpr uint32_t kSteadyFrames = 12;      // a sliding text's width the same for so long: the row's focus animation is over
constexpr float kMeasureRegionW = 4000;     // wider than any name
constexpr float kArrowHalf = 6;             // a row's arrow: 12 wide, on every row
constexpr float kArrowMargin = 8;           // a name never touches it
constexpr size_t kWidthsKept = 64;
constexpr int kSummaryItems = 40;           // Opponent Outfit Settings' summary: its lines, at most
constexpr int kNotYet = -1000000;           // no offset written yet
constexpr const char* kFitSig = "set_AutoRegionFit(via.gui.RegionFitType)";
constexpr const char* kWrapSig = "set_AutoWrap(System.Boolean)";

struct Found {                              // the text as it was found
    bool have = false;
    float x = 0, y = 0, z = 0, region_w = 0, region_h = 0;
    float rect[4] = { 0, 0, -1, -1 };
    int64_t cp = 0, align = 0, fit = 0;
    bool wrap = false;
    scroll::Span span;                      // a row's place, from x
};
struct State {
    std::string msg;
    Found found;
    std::map<std::string, float> widths;    // what this control measured
    uint32_t changed_at = 0, born = 0, seen = 0, checked = 0;
    bool looked = false;                    // msg is what the control holds
    bool settled = false;                   // what msg needs is decided, for settled_w
    float settled_w = 0;
    bool measuring = false, sliding = false, clipped = false, fit_off = false;
    float width = 0, natural = 0, left = 0, last_x = 0, seen_w = 0;
    uint32_t seen_at = 0;
    bool rtl = false;
    uint64_t t0 = 0;
    int last = kNotYet;
    bool centred = false;                   // by its centre as found: one that fits sits in the middle of its place
    bool logged = false;
};
std::unordered_map<uintptr_t, State> g_states;
uint32_t g_frame = 0;
bool g_any = false;
bool g_row_sliding = false;                 // a Costume row's name is sliding: post_frame has something to look at

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
void no_fit(Obj* t, State& s) {
    if (s.fit_off) return;
    s.fit_off = true;
    set_i64(t, kFitSig, 0);
    if (s.found.wrap) set_i64(t, kWrapSig, 0);
}
// A Costume row's place: from its left arrow's inner edge to its right arrow's, read where they are on this row, in the text's parent's units.
// An arrow's panel sits on the arrow's middle on every row, and the right one is the left one mirrored (scale -1, read 07/11); what draws it differs
// (e_texture_arrow, or none of that name on the character select's panels), so its width is a constant. The two panels' places in the row and on
// the screen give the units. Nothing valid: the region is the place, as for any other text.
scroll::Span row_span(Obj* t, void* row) {
    scroll::Span none;
    void* l = A()->ui_find_child(row, "p_MouseDecideOperable_L", 4);
    void* r = A()->ui_find_child(row, "p_MouseDecideOperable_R", 4);
    if (!l || !r) return none;
    float lp[3] = {}, rp[3] = {}, lg[3] = {}, rg[3] = {}, tg[3] = {};
    if (!floats_of(static_cast<Obj*>(l), "get_Position", lp, 2) || !floats_of(static_cast<Obj*>(r), "get_Position", rp, 2)) return none;
    if (!floats_of(static_cast<Obj*>(l), "get_GlobalPosition", lg, 2) || !floats_of(static_cast<Obj*>(r), "get_GlobalPosition", rg, 2)) return none;
    if (!floats_of(t, "get_GlobalPosition", tg, 2)) return none;
    const float apart = rg[0] - lg[0];
    if (!(apart > 1)) return none;
    const float per_unit = (rp[0] - lp[0]) / apart;
    if (!(per_unit > 0.01f) || !std::isfinite(per_unit)) return none;
    const float half = kArrowHalf / per_unit;
    return scroll::span_between(tg[0], lg[0] + half, rg[0] - half, per_unit, kArrowMargin);
}
bool capture(Obj* t, Found& f, void* row) {
    float p[3], r[2];
    if (!floats_of(t, "get_Position", p, 3) || !floats_of(t, "get_RegionSize", r, 2)) return false;
    const Ret cp = call(t, "get_ControlPoint"), al = call(t, "get_PageAlignment"), fit = call(t, "get_AutoRegionFit");
    if (cp.exception_thrown || al.exception_thrown || fit.exception_thrown) return false;
    f.x = p[0]; f.y = p[1]; f.z = p[2]; f.region_w = r[0]; f.region_h = r[1];
    f.cp = static_cast<int32_t>(cp.dword); f.align = static_cast<int32_t>(al.dword); f.fit = static_cast<int32_t>(fit.dword);
    if (!floats_of(t, "get_ScissorRect", f.rect, 4)) { f.rect[0] = f.rect[1] = 0; f.rect[2] = f.rect[3] = -1; }
    f.wrap = call_bool(t, "get_AutoWrap");
    f.span = row ? row_span(t, row) : scroll::Span{};
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
        if (s.fit_off) {
            set_i64(t, kFitSig, f.fit);
            if (f.wrap) set_i64(t, kWrapSig, 1);
        }
    }
    s.found = Found{};
    s.measuring = s.sliding = s.clipped = s.fit_off = false;
    s.last = kNotYet;
}
// Its fit off, its region wide, at the same anchor: a text that fits does not move. A text placed by its left point is clipped at once: what
// doesn't fit never shows, not even those frames.
bool open_for_measure(Obj* t, State& s, float width, void* row) {
    if (!s.found.have && !capture(t, s.found, row)) return false;
    no_fit(t, s);
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
// The measure is over: the region and the clip as they were
void close_measure(Obj* t, State& s) {
    if (!s.measuring) return;
    A()->ui_set_region_size(C(), t, s.found.region_w, s.found.region_h);
    if (s.clipped) set_rect(t, s.found.rect);
    s.measuring = s.clipped = false;
}
void start_slide(Obj* t, State& s, float natural, float place, float left) {
    const Found& f = s.found;
    s.width = place;
    s.natural = natural;
    s.left = left;
    s.rtl = scroll::text_is_rtl(s.msg);
    no_fit(t, s);
    set_i64(t, "set_ControlPoint(via.gui.ControlPoint)", (f.cp / 4) * 4);
    set_i64(t, "set_PageAlignment(via.gui.PageAlignment)", (f.align / 4) * 4);
    s.centred = f.cp % 4 == 1;
    s.sliding = s.clipped = true;
    s.t0 = GetTickCount64();
    s.last = kNotYet;
}
// What a text of this width needs in its place
void settle(Obj* t, State& s, float natural, float width, void* row) {
    if (!s.found.have && !capture(t, s.found, row)) return;
    const Found& f = s.found;
    const bool spanned = f.span.ok;
    const int column = static_cast<int>(f.cp % 4);
    const float place = spanned ? f.span.r - f.span.l : width > 0 ? width : f.region_w;
    const bool fits = spanned ? scroll::fits_span(natural, f.span, column) : !scroll::overflows(natural, place);
    if (row && !s.logged) {
        s.logged = true;
        logf("name scroll: a spin row's place: %s, %.0f wide (region %.0f, from its point %.0f to %.0f)", spanned ? "between its arrows" : "its region (arrows not read)",
             place, f.region_w, spanned ? f.span.l : 0.0f, spanned ? f.span.r : 0.0f);
    }
    if (!fits) {
        const float left = spanned ? f.x + f.span.l : scroll::left_of(f.x, column, place);
        if (s.sliding) { s.width = place; s.left = left; s.last = kNotYet; }     // its place changed
        else start_slide(t, s, natural, place, left);
        return;
    }
    const float region = f.region_w;
    if (s.sliding) put_back(t, s);
    if (natural > region + 0.5f && (s.found.have || capture(t, s.found, row))) {
        no_fit(t, s);                               // wider than its region, inside its place: where it is, at full size
        return;
    }
    if (s.found.have) put_back(t, s);
}
void slide_step(Obj* t, State& s) {
    float d[2] = {}, p[3] = {};
    const uint64_t now = GetTickCount64();
    if (floats_of(t, "get_DisplaySize", d, 2) && std::isfinite(d[0]) && d[0] > 0) {      // its fit is off: its real width, in this focus state
        if (std::fabs(d[0] - s.seen_w) > 0.5f) { s.seen_w = d[0]; s.seen_at = g_frame; }
        else if (g_frame - s.seen_at >= kSteadyFrames && std::fabs(d[0] - s.natural) > 0.5f) {
            s.natural = d[0];
            s.t0 = now - static_cast<uint64_t>(scroll::time_at(s.natural - s.width, s.last, s.rtl) * 1000.0);
        }
    }
    const bool moved = s.last != kNotYet && floats_of(t, "get_Position", p, 3) && std::fabs(p[0] - s.last_x) > 0.25f;
    const double secs = static_cast<double>(now - s.t0) / 1000.0;
    // a name that fits only in the smaller font of a row without the focus: in the middle of its place, as any name that fits
    const int off = s.centred && s.natural < s.width ? -static_cast<int>(std::lround((s.width - s.natural) * 0.5f))
                                                     : scroll::offset_at(s.natural - s.width, secs, s.rtl);
    if (off == s.last && !moved) return;
    s.last = off;
    const scroll::Window w = scroll::window_at(s.left, s.width, off);
    const float rect[4] = { w.clip_left, w.clip_top, w.clip_right, w.clip_bottom };
    set_rect(t, rect);
    A()->ui_set_position(C(), t, w.x, s.found.y, s.found.z);
    s.last_x = w.x;
}
// Not looked at for a while: still clipped as only this code clips, the same control. Otherwise nothing of this state is its own.
bool still_ours(Obj* t) {
    float r[4] = {};
    return floats_of(t, "get_ScissorRect", r, 4) && r[1] == scroll::kClipTop && r[3] == scroll::kClipBottom;
}
void sweep() {
    static uint32_t at = 0;
    if (g_frame - at < kForgetFrames) return;
    at = g_frame;
    for (auto it = g_states.begin(); it != g_states.end();)
        it = g_frame - it->second.seen > kForgetFrames ? g_states.erase(it) : std::next(it);
}

void step(void* text, float width, void* row, uint32_t poll, bool allowed) {
    if (!text || !A() || !C()) return;
    Obj* t = static_cast<Obj*>(text);
    State& s = g_states[reinterpret_cast<uintptr_t>(text)];
    if (s.seen && g_frame - s.seen > kGapFrames && s.sliding && !still_ours(t)) s = State{};
    s.seen = g_frame;
    if (s.looked && !s.sliding && !s.measuring && poll > 1 && g_frame - s.checked < poll) return;
    s.checked = g_frame;
    bool ok = false;
    const bool visible = call_bool(t, "get_ActualVisible", {}, &ok);
    if (!allowed || !ok || !visible) {
        if (s.found.have) put_back(t, s);
        s.looked = false;
        return;
    }
    const std::string msg = str(call_obj(t, "get_Message"));
    if (!s.looked || msg != s.msg) {
        if (s.found.have) put_back(t, s);
        s.msg = msg;
        s.looked = true;
        s.settled = false;
        s.changed_at = s.born = g_frame;
        if (msg.empty()) return;
        if (!s.widths.count(msg) && !open_for_measure(t, s, width, row)) return;
    }
    if (msg.empty()) return;
    if (s.measuring) {
        if (g_frame < s.changed_at + kLayoutFrames) return;
        float d[2] = {};
        const bool got = floats_of(t, "get_DisplaySize", d, 2) && std::isfinite(d[0]) && d[0] > 0;
        if (!got && g_frame - s.born < kMeasureMax) return;
        if (s.widths.size() >= kWidthsKept) s.widths.clear();
        s.widths[msg] = got ? d[0] : 0;             // 0: no width to go on, left as it is
        close_measure(t, s);
    }
    if (row && s.settled && !s.sliding) {           // the focus changes a row's font: wider than it was measured, what it needs again
        float d[2] = {};
        auto known = s.widths.find(msg);
        if (known != s.widths.end() && floats_of(t, "get_DisplaySize", d, 2) && std::isfinite(d[0]) && d[0] > known->second + 1) {
            known->second = d[0];
            s.settled = false;
        }
    }
    if (!s.settled || s.settled_w != width) {
        auto known = s.widths.find(msg);
        if (known == s.widths.end()) return;
        settle(t, s, known->second, width, row);
        s.settled = true;
        s.settled_w = width;
    }
    if (s.sliding) slide_step(t, s);
    sweep();
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

// The option window's rows (read in game, 09/11): OptionDialog > c_main/c_dialog/p_Group_main_v_kc > p_OptionUnit_* > a row panel that
// holds the arrows p_MouseDecideOperable_L / _R, its title e_text_title and its value p_TextList_0_h/e_text (AutoRegionFit on: the game
// squeezes a long value). Each row: f(value text or null, row, title text or null). R35: found by name each time.
// The Options screen's right list (OptionSettingMenu > p_GroupScroll_sub_v/c_scroll > p_OptionUnit_N_switch_show, the game's own
// rows and the Native Core's in its sections, Wael 10/11: "as the Costume row"): the same, its row panel p_Spin_spintext_h, its value
// e_text_0 (or a p_TextList_0_h/e_text as the window's). By the rows' parts, never by their texts.
constexpr int kOptionUnits = 48;
// `spin`: the Options screen's rows (their parts one level deeper, the value e_text_0 when there is no text list)
template <class F> void unit_rows(const SF6CoreAPI* a, Obj* units, bool spin, F f) {
    const int32_t depth = spin ? 3 : 2;
    int guard = 0;
    for (Obj* unit = units ? call_obj(units, "get_Child") : nullptr; unit && guard < kOptionUnits; unit = call_obj(unit, "get_Next"), ++guard) {
        int g2 = 0;
        for (Obj* row = call_obj(unit, "get_Child"); row && g2 < 8; row = call_obj(row, "get_Next"), ++g2) {
            if (!a->ui_find_child(row, "p_MouseDecideOperable_L", depth)) continue;        // not a row with arrows
            void* list = a->ui_find_child(row, "p_TextList_0_h", depth);
            void* value = list ? a->ui_find_child(list, "e_text", 2) : spin ? a->ui_find_child(row, "e_text_0", depth) : nullptr;
            f(value, static_cast<void*>(row), a->ui_find_child(row, "e_text_title", 2));
        }
    }
}
template <class F> void option_rows(const SF6CoreAPI* a, Obj* cm, F f) { unit_rows(a, static_cast<Obj*>(a->ui_find_cached(cm, "p_Group_main_v_kc")), false, f); }
template <class F> void settings_rows(const SF6CoreAPI* a, Obj* cm, F f) {
    void* sub = a->ui_find_cached(cm, "p_GroupScroll_sub_v");
    unit_rows(a, static_cast<Obj*>(sub ? a->ui_find_child(sub, "c_scroll", 1) : nullptr), true, f);
}
// The option window (the core's menus, the game's own) and the Options screen: f(agent's main control, shown)
template <class F> void option_screens(const SF6CoreAPI* a, F f) {
    for (const char* agent_name : { "OptionDialog", "OptionSettingMenu" }) {
        void* ob[2] = {};
        const uint32_t n = a->ui_agents(agent_name, ob, 2);
        const bool window = agent_name[6] == 'D';
        for (uint32_t i = 0; i < n && i < 2; ++i) {
            Obj* agent = static_cast<Obj*>(ob[i]);
            Obj* cm = call_obj(agent, "get_ControlMain");
            if (cm) f(cm, drawn(agent) && shown(cm), window);
        }
    }
}

}  // namespace

void drive(void* text, float width, uint32_t poll, bool allowed) { step(text, width, nullptr, poll, allowed); }

void drive_row(void* text, void* row, bool allowed) { step(text, 0, row, 1, allowed); }

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
        if (s.sliding) { s.left = x; s.last = kNotYet; }   // these are texts placed by their left point
        else A()->ui_set_position(C(), text, x, y, s.found.z);
    }
    return true;
}

float known_width(void* text, const std::string& message) {
    auto it = g_states.find(reinterpret_cast<uintptr_t>(text));
    if (it == g_states.end()) return -1;
    auto w = it->second.widths.find(message);
    return w == it->second.widths.end() ? -1 : w->second;
}

void frame() {
    ++g_frame;
    if (!g_any && (g_frame % kPollEvery) != 0) return;
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, ui_agents) || !SF6CORE_HAS(a, ui_find_cached) || !SF6CORE_HAS(a, ui_find_child)) return;
    try {
        g_any = false;
        g_row_sliding = false;
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
                if (void* text = spin ? a->ui_find_cached(spin, "e_text") : nullptr) {
                    step(text, 0, spin, 1, on && !claimed(sc.side));
                    if (busy(text)) g_row_sliding = true;
                }
                if (!sc.summary) continue;
                // Opponent Outfit Settings' summary: its lines, "<outfit>: <colours on>"
                Obj* list = static_cast<Obj*>(a->ui_find_cached(cm, "p_ScrollList_texts_v"));
                int guard = 0;
                for (Obj* item = list ? call_obj(list, "get_Child") : nullptr; item && guard < kSummaryItems; item = call_obj(item, "get_Next"), ++guard)
                    if (void* line = a->ui_find_child(item, "e_text_costume", 3)) step(line, 0, nullptr, 1, on);
            }
        }
        // the option window and the Options screen: every row's value and title
        option_screens(a, [&](Obj* cm, bool on, bool window) {
            g_any = g_any || on;
            auto each = [&](void* value, void* row, void* title) {
                // (the game's own Options page has up to ~50 rows: a still text is looked at every 5 / 15 frames, a sliding one every frame)
                if (value) { step(value, 0, row, 5, on); if (busy(value)) g_row_sliding = true; }
                if (title) step(title, 0, nullptr, 15, on);
            };
            if (window) option_rows(a, cm, each);
            else settings_rows(a, cm, each);
        });
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logf("name scroll: EXCEPTION in a frame"); }
    }
}

// A sliding text the row's focus animation moved this frame: back where it was slid
void hold_place(void* text) {
    if (!text) return;
    auto it = g_states.find(reinterpret_cast<uintptr_t>(text));
    if (it == g_states.end() || !it->second.sliding || it->second.last == kNotYet) return;
    const State& s = it->second;
    float p[3] = {};
    if (floats_of(static_cast<Obj*>(text), "get_Position", p, 3) && std::fabs(p[0] - s.last_x) > 0.25f)
        A()->ui_set_position(C(), text, s.last_x, s.found.y, s.found.z);
}

void post_frame() {
    if (!g_row_sliding) return;
    const SF6CoreAPI* a = A();
    if (!a || !C()) return;
    option_screens(a, [&](Obj* cm, bool, bool window) {
        auto each = [&](void* value, void*, void*) { hold_place(value); };
        if (window) option_rows(a, cm, each);
        else settings_rows(a, cm, each);
    });
    for (const Screen& sc : kScreens) {
        void* buf[2] = {};
        const uint32_t n = a->ui_agents(sc.agent, buf, 2);
        for (uint32_t i = 0; i < n && i < 2; ++i) {
            Obj* cm = call_obj(static_cast<Obj*>(buf[i]), "get_ControlMain");
            void* spin = cm ? a->ui_find_cached(cm, "p_Spin_costume") : nullptr;
            void* text = spin ? a->ui_find_cached(spin, "e_text") : nullptr;
            if (!text) continue;
            auto it = g_states.find(reinterpret_cast<uintptr_t>(text));
            if (it == g_states.end() || !it->second.sliding || it->second.last == kNotYet) continue;
            const State& s = it->second;
            float p[3] = {};
            if (floats_of(static_cast<Obj*>(text), "get_Position", p, 3) && std::fabs(p[0] - s.last_x) > 0.25f)
                a->ui_set_position(C(), text, s.last_x, s.found.y, s.found.z);
        }
    }
}

}  // namespace slots::namescroll
