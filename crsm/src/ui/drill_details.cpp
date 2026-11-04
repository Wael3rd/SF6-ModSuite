// ui/drill_details — the drill details dialog (Wael 04/11): Confirm (keyboard F, pad A / Cross) on the Manager page's
// Drill row opens the game's own app.training.UITrainingImageDialog (GUI / UIAgent ui11281, the page of "Simple
// Training Settings > The Frame Meter and You") filled with the selected drill: its name as the title, "<Character> -
// <Source>" in the top-left box, its saved slots ten a page in the list (the game's tab keys: the keys at Q / E of a
// QWERTY keyboard, A / E on AZERTY, pad L1 / R1), its whole description wrapped by the game and scrolled 18 lines at a
// time (Up / Down, W / Z / S, pad d-pad and left stick), with a scroll bar, and the buttons "Open drill" / "Cancel". T
// (pad Y / Triangle) edits the description in the game's text dialog, drawn over this one (in game 04/11), the new text
// shown in place after. The key guide at the bottom shows the dialog's own entries while it is up. The pure parts are
// ui/details_logic; the Confirm comes from the core (on_confirm, core 0.9.5) through the Manager page, which gathers the
// drill, runs "Open drill" and the edit (ui/manager_page.cpp).
//
// Proven in game with Lua probes (04/11): the dialog opens from any training menu page as a new instance + .ctor +
// StartDialog(0, title guid, detail guid, Action[2] of nulls) on the game thread; null actions are safe (Esc and Confirm
// both close it, nothing leaks to the page behind). Every text is written with set_Message (the core's ui_set_text)
// with AutoRegionFit 0 (1 squeezed a whole text into its region: "the other font"); the description is wrapped by the
// game (AutoWrap + ForceAutoWrap, 930 wide, PageAlignment LeftTop).
//
// The button (in game 04/11): UIFlowDecideWait.OnEnter and Param.OnEnd share their machine code with 52 and 21 other
// methods of the game (R18: never hooked). So the button is read from the inputs themselves, as the game takes them
// once the dialog waits for input (its UIFlowInputWait.OnEnter seen): a decide press (keyboard F, pad south A / Cross,
// a left click) records the list's selected button, a back press (Esc, pad east B / Circle, a right click) records a
// cancel; the last of them before the end decides, and "Open drill" runs only for a decide on the left button less
// than kDecideToEnd frames before the end. Esc is a cancel: it never opens the drill. Nothing seen (a remapped key):
// no drill opened.
//
// Threads and rules: request() (any thread) is run by the UI frame callback (game thread, ui::lock held). The fill runs
// in the dialog's own flow, before its first draw: a post-hook of its Param's BeforeShowObject, applied again after
// ShowedObject and UIFlowInputWait.OnEnter (every write is compared first by the core: what the game's own show steps
// may have put back is written again, nothing else), under ui::lock; the frame callback fills it when none of them
// ran. The scroll bar's two Rects are copied there too (R19: duplicate() only in the owning flow's update; the core's
// ui_clone_control, the copies found again by name, R35). Everything else (the lines, the scrolling, the slot pages,
// the inputs, the end) runs on the frame callback. No GUI element is kept across frames: each use finds it again from
// the dialog's agent. The dialog object is held (one reference) while it is up and released once it has ended; it is
// closed (CloseDialog) when the menu closes, training ends or the UI stops.
//
// The end (in game 04/11, a crash of the Lua demo): ui11281's GameObject and View stay in the scene after the dialog
// closes (pooled) while its elements are released, and they stay reachable from the View: a set_Message on one of them
// after the close was an access violation in the game. So "ui11281 still there" is never an alive test: the GUI is
// written only while g_live says our dialog is up, set at StartDialog and cleared FIRST at the first sign of the end (a
// decide or back press, get_IsEnd checked before anything else each frame, our own CloseDialog), and found only
// through our own Param (its agent) or, once one of our own show hooks has run, the core's discovery. The scroll bar's
// copies are hidden at a decide or back press (the dialog still drawn); otherwise they go with the released elements,
// their parent's. Nothing of the dialog is touched once it ended (the tab keys and Up / Down are then the menu's). A
// press the dialog did not take (it is still up kRearm frames later) gives the inputs back.
#include "drill_details.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

#include "common.hpp"
#include "details_logic.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "hookstack.hpp"
#include "internal.hpp"
#include "logic.hpp"
#include "perf.hpp"

namespace ui::details {
namespace {

namespace gm = game::gm;
using gx::MO;
using gx::TD;
using Method = reframework::API::Method;

constexpr const char* kDialogType = "app.training.UITrainingImageDialog";
constexpr const char* kParamType = "app.training.UITrainingImageDialog.Param";
constexpr const char* kAgentName = "ui11281";
// The game's own call (TrainingMenuFunc.Function, the Frame Meter page): its title and text, all rewritten here
constexpr const char* kTitleGuid = "7bc776ce-2aa0-4e96-a88d-4609a265bb80";
constexpr const char* kDetailGuid = "4300956a-4292-412f-bcce-89850ad3ad71";
constexpr const char* kTrackName = "e_rect_crsm_track";   // the scroll bar's copies (never "p_...": not a UI part, R20)
constexpr const char* kThumbName = "e_rect_crsm_thumb";
constexpr uint32_t kTrackColor = 0xCC463C3Cu;   // via.Color as stored (0xAABBGGRR): a dark grey at 80 %
constexpr uint32_t kThumbColor = 0xFFCC24CEu;   // the menu's magenta accent (its focused row's glow, windows.cpp)
constexpr float kBoxMargin = 16.0f;             // the list's texts inside their frame line
constexpr float kMinFitWidth = 120.0f;          // a region narrower than this is not trusted for the "…" cut
constexpr uint64_t kFillWait = 10;              // frames the dialog's own hooks get to fill it before the frame does
constexpr uint64_t kMeasureAfter = 2;           // the wrap read 2 frames after the text was set (the demo's frame 1 -> 3)
constexpr int kMeasureTries = 10;
constexpr uint64_t kClosingWait = 120;          // frames a dialog we closed gets to end before it is let go
constexpr uint64_t kRearm = 60;                 // a decide / back press the dialog did not take: the inputs back after this
                                                // (its close takes ~9 frames, 04/11)
constexpr uint64_t kDecideToEnd = 90;           // "Open drill": a decide on the left button at most this long before the end
constexpr int kMenuGoneFrames = 3;              // the menu seen closed that many frames in a row: the dialog is closed
constexpr int kCloneTries = 3;
constexpr uint64_t kAfterEnd = 10;              // frames is_open() still answers after the end: the press that closed it
                                                // (F on a button) is never a Confirm on the Drill row that opens it again
constexpr int kKeyUp = 0x26, kKeyDown = 0x28, kKeyW = 0x57, kKeyZ = 0x5A, kKeyS = 0x53;   // the demo's scroll keys
constexpr int kKeyF = 0x46, kKeyEsc = 0x1B, kKeyT = 0x54;                                // decide, back, edit
constexpr int kMouseL = 0x01, kMouseR = 0x02;                                            // VK_LBUTTON / VK_RBUTTON
constexpr int kScanTabLeft = 0x10, kScanTabRight = 0x12;   // the game's tab keys by position: Q / E (QWERTY), A / E (AZERTY)

enum class Phase { None, Opening, Filled, Ready, Ending, Closing };
const char* phase_name(Phase p) {
    switch (p) {
        case Phase::Opening: return "opening";
        case Phase::Filled: return "filled";
        case Phase::Ready: return "up";
        case Phase::Ending: return "ending";
        case Phase::Closing: return "closing";
        default: return "closed";
    }
}

struct Req { Drill d; std::function<void()> on_open, on_edit; };
std::mutex g_req_mx;                             // the request (any thread)
std::optional<Req> g_req;
std::atomic<bool> g_open{false};                 // asked, up or closing (is_open)
std::atomic<void*> g_param{nullptr};             // our dialog's Param: identity only (the hooks compare it)
std::atomic<bool> g_live{false};                 // our dialog is up: its GUI may be written (cleared first at its end)
std::atomic<uint64_t> g_ended_at{0};             // the frame of the last end (+1: 0 = none yet)

enum class Press { None, Decide, Back };
struct LastPress { Press kind = Press::None; int sel = -1; uint64_t at = 0; const char* by = ""; };

struct State {                                   // under ui::lock
    Phase phase = Phase::None;
    Phase before_end = Phase::Ready;             // the phase an Ending came from (a press not taken: back to it)
    MO* dlg = nullptr;                           // held while up
    Drill d;
    std::function<void()> on_open, on_edit;
    std::vector<model::pack::SlotSummary> saved;
    std::string text;                            // the whole description box text
    std::vector<size_t> starts{ 0 };             // its lines' byte offsets
    int total = 1, top = 0, page = 0, pages = 1;
    uint64_t opened_at = 0, filled_at = 0, closing_at = 0, ending_at = 0;
    int measure_tries = 0, menu_gone = 0, clone_tries = 0;
    bool measured_visible = false, bar = false, clones = false;
    float bar_x = 0, bar_y = 0, bar_z = 0, thumb_z = 0;
    double row_em = 0, header_em = 0;
    const char* filled_by = "";
    bool gui_seen = false;                       // one of our show hooks ran for our Param: the GUI holds our elements
    bool input_seen = false;                     // its UIFlowInputWait.OnEnter ran: the dialog takes the inputs
    bool closed_by_us = false;                   // CloseDialog was ours (the menu closed): no "Open drill"
    bool editing = false;                        // the text dialog of an edit is up over it: every key its, none ours
    uint64_t quiet_until = 0;                    // after the edit: the presses that closed the text dialog are not ours
    // the key guide at the bottom (InputGuide), written while the dialog is up, put back at its end
    std::string guide_host, guide_orig, guide_written, help_orig, help_written, tab_icons;
    std::vector<std::string> guide_hidden;       // the bar's items hidden by us (by name): shown again at the end
    float guide_orig_w = 0;
    bool guide_saved = false, help_saved = false, guide_missing_logged = false;
    uint64_t guide_next = 0;
    LastPress last;                              // the last decide / back press seen while it took the inputs
    uint32_t presses_f = 0, presses_esc = 0, presses_ml = 0, presses_mr = 0, presses_t = 0;   // the keys' press counts seen
    HoldRepeat scroll, paging;
    logic::KeyEdge pad_south, pad_east, pad_y;
};
State g_st;

// the game's objects made once and kept for the session
bool g_guids = false;
uint8_t g_title_guid[16] = {}, g_detail_guid[16] = {};
MO* g_actions = nullptr;                         // System.Action[2] of nulls (gm::new_array: held)
uint64_t g_opens = 0, g_refused = 0, g_drills_opened = 0, g_decides = 0, g_backs = 0, g_rearms = 0, g_edits = 0;
bool ok_before = false, ok_showed = false, ok_input = false;

std::string tr(const std::string& key, const std::vector<std::string>& args) { return ui::T(key, args); }

// ---- the dialog's GUI (read through REFramework, written through the core) ---------------------------------------------
MO* kid(MO* parent, const char* name) {          // a direct child by name
    const SF6CoreAPI* a = gx::api();
    return (parent && a) ? static_cast<MO*>(perf::counted(a)->ui_find_child(parent, name, 0)) : nullptr;
}

MO* param_now() { return g_st.dlg ? gx::ref(g_st.dlg, "_Param") : nullptr; }

// The dialog's agent: our Param's own (when it names ui11281), else, only when `discovery` (one of our own show hooks
// ran for our Param and the dialog is up: the pooled GUI then holds our elements), the core's discovery of ui11281
MO* dialog_agent(MO* param, bool discovery) {
    if (MO* ag = param ? gx::ref(param, "<Agent>k__BackingField") : nullptr)
        if (gx::name(gx::call_obj(ag, "get_GameObject")) == kAgentName) return ag;
    const SF6CoreAPI* a = gx::api();
    if (!discovery || !a || !SF6CORE_HAS(a, ui_agents)) return nullptr;
    void* out[4] = {};
    return perf::counted(a)->ui_agents(kAgentName, out, 4) ? static_cast<MO*>(out[0]) : nullptr;
}

// ui11281 under View: c_main / c_detail / { c_contents { c_scale, c_frame { e_s9g_line, c_meter, e_txt_frame },
// e_txt_detail }, e_txt_title, p_SimpleList_0_h } (the wiki's tree, 04/11)
struct Parts { MO* contents = nullptr, *title = nullptr, *list = nullptr, *frame = nullptr, *frame_text = nullptr,
               *meter = nullptr, *scale = nullptr, *text = nullptr; };
bool find_parts(MO* param, bool discovery, Parts& p) {
    MO* agent = dialog_agent(param, discovery);
    MO* cm = agent ? gx::call_obj(agent, "get_ControlMain") : nullptr;
    if (!cm) return false;
    MO* main = cm;
    if (gx::name(cm) != "c_main") {
        const SF6CoreAPI* a = gx::api();
        main = a ? static_cast<MO*>(perf::counted(a)->ui_find_child(cm, "c_main", 2)) : nullptr;
    }
    MO* detail = kid(main, "c_detail");
    p.contents = kid(detail, "c_contents");
    p.title = kid(detail, "e_txt_title");
    p.list = kid(detail, "p_SimpleList_0_h");
    p.frame = kid(p.contents, "c_frame");
    p.frame_text = kid(p.frame, "e_txt_frame");
    p.meter = kid(p.frame, "c_meter");
    p.scale = kid(p.contents, "c_scale");
    p.text = kid(p.contents, "e_txt_detail");
    return p.contents && p.scale && p.text;
}

// A one-argument setter by its full signature ("set_X(<parameter type>)"), resolved once per type: the core's
// obj_invoke finds a full signature exactly (a bare name is looked for among a type's first methods only, gx.cpp)
std::mutex g_sig_mx;
std::map<std::pair<TD*, std::string>, std::string> g_sigs;
bool set_prop(MO* o, const char* name, SF6CoreArg v) {
    TD* td = o ? o->get_type_definition() : nullptr;
    if (!td) return false;
    std::string sig;
    {
        std::lock_guard lk(g_sig_mx);
        const auto key = std::make_pair(td, std::string(name));
        auto it = g_sigs.find(key);
        if (it == g_sigs.end()) {
            std::string s;
            if (Method* m = gm::method_n(td, name, 1)) {
                const auto params = m->get_params();
                if (TD* pt = params.size() == 1 ? reinterpret_cast<TD*>(params[0].t) : nullptr) s = std::string(name) + "(" + pt->get_full_name() + ")";
            }
            it = g_sigs.emplace(key, s).first;
        }
        sig = it->second;
    }
    return !sig.empty() && gx::invoke(o, sig.c_str(), { v });
}

void no_squash(MO* t) { if (t) set_prop(t, "set_AutoRegionFit", gx::I(0)); }

bool gpos(MO* o, float* x, float* y) {
    float v[3] = {};
    if (!o || !gx::vec3_of(o, "get_GlobalPosition", v)) return false;
    *x = v[0];
    *y = v[1];
    return true;
}
float hfrac(int32_t cp) { const int h = cp & 3; return h == 1 ? 0.5f : (h == 2 ? 1.0f : 0.0f); }        // via.gui.ControlPoint:
float vfrac(int32_t cp) { const int v = (cp >> 2) & 3; return v == 1 ? 0.5f : (v == 2 ? 1.0f : 0.0f); } // h = value & 3, v = value >> 2

// A frame line (via.gui.Scale9Grid): its left edge and width, global units; false when unreadable
bool box_of(MO* line, float* left, float* w) {
    float x = 0, y = 0, sw = 0, sh = 0;
    int32_t cp = 0;
    if (!line || !gpos(line, &x, &y) || !gx::size_of(line, "get_Size", &sw, &sh) || !gx::call_i32(line, "get_ControlPoint", &cp) || sw < 50) return false;
    *left = x - sw * hfrac(cp);
    *w = sw;
    return true;
}

// A text's place: its ControlPoint's global position, its region's left / top edges, its region and font width
struct TextGeo { float gx = 0, gy = 0, left = 0, top = 0, w = 0, h = 0, fw = 0; int32_t cp = 0; bool ok = false; };
TextGeo text_geo(MO* t) {
    TextGeo g;
    float fh = 0;
    g.ok = t && gpos(t, &g.gx, &g.gy) && gx::size_of(t, "get_RegionSize", &g.w, &g.h) && gx::size_of(t, "get_FontSize", &g.fw, &fh)
           && gx::call_i32(t, "get_ControlPoint", &g.cp);
    if (g.ok) { g.left = g.gx - g.w * hfrac(g.cp); g.top = g.gy - g.h * vfrac(g.cp); }
    return g;
}

// The text's region made `width` wide with its left edge at `left` (global): its position moved by what its ControlPoint
// moves (the GUI's scale taken as 1)
void place_region(MO* t, const TextGeo& g, float left, float width) {
    float p[3] = {};
    if (!g.ok || width < 1 || !gx::vec3_of(t, "get_Position", p)) return;
    const float anchor = left + width * hfrac(g.cp);
    if (std::fabs(anchor - g.gx) > 0.5f) gx::set_pos(t, p[0] + anchor - g.gx, p[1], p[2]);
    if (std::fabs(width - g.w) > 0.5f) gx::set_region(t, width, g.h);
}

// A text cut with "…" to what its region holds (a region too narrow to be trusted: as it is)
std::string fit(const std::string& s, float width, float font_w) {
    return width >= kMinFitWidth ? logic::cut_em(s, em_for(width, font_w)) : s;
}

void write_fitted(MO* t, const std::string& s) {
    if (!t) return;
    no_squash(t);
    const TextGeo g = text_geo(t);
    gx::set_text(t, g.ok ? fit(s, g.w, g.fw) : s);
}

// getCharBaseLine(g) of a text (System.Single: the invoke wrapper hands it back as a double)
float baseline(MO* t, uint32_t g) {
    static Method* m = nullptr;
    static bool looked = false, real = true;
    if (!looked) {
        looked = true;
        if (TD* td = gm::type("via.gui.Text")) m = gm::method_n(td, "getCharBaseLine", 1);
        if (TD* rt = m ? m->get_return_type() : nullptr) { const std::string n = rt->get_full_name(); real = n == "System.Single" || n == "System.Double"; }
        cmn::journal("ui: drill details: via.gui.Text.getCharBaseLine %s", m ? (real ? "found" : "found (an integer)") : "NOT found: the description is not scrolled");
    }
    reframework::InvokeRet r{};
    if (!m || !gm::call(t, m, { gm::arg_i32((int32_t)g) }, &r)) return 0.0f;
    return real ? (float)r.d : (float)(int32_t)r.dword;
}

// ---- the texts ----------------------------------------------------------------------------------------------------------
// The list: no colour squares, the header centred across the list's box, the rows as wide as the box (learnt here, kept
// as numbers for the page changes)
void layout_list(const Parts& p, bool first) {
    float bl = 0, bw = 0;
    const bool box = box_of(kid(p.scale, "e_s9g_line"), &bl, &bw);
    for (int i = 0; i <= kSlotsPerPage; ++i) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "c_item%d", i);
        MO* it = kid(p.scale, nm);
        if (!it) continue;
        if (MO* c = kid(it, "e_tex_icon")) gx::set_visible(c, false);
        if (MO* c = kid(it, "e_rect_frame")) gx::set_visible(c, false);
        MO* t = kid(it, "e_txt_title");
        if (!t) continue;
        no_squash(t);
        const TextGeo g = text_geo(t);
        if (i == 0) {
            int32_t pa = 0;
            if (gx::call_i32(t, "get_PageAlignment", &pa)) set_prop(t, "set_PageAlignment", gx::I((pa & ~3) | 1));   // centred, its vertical kept
            if (box) place_region(t, g, bl + kBoxMargin, bw - 2 * kBoxMargin);
            g_st.header_em = box ? em_for(bw - 2 * kBoxMargin, g.fw) : (g.ok && g.w >= kMinFitWidth ? em_for(g.w, g.fw) : 0);
            continue;
        }
        float w = g.ok ? g.w : 0;
        if (box && g.ok && bl + bw - kBoxMargin - g.left > kMinFitWidth) {
            w = bl + bw - kBoxMargin - g.left;
            place_region(t, g, g.left, w);
        }
        if (i == 1) g_st.row_em = g.ok && w >= kMinFitWidth ? em_for(w, g.fw) : 0;
    }
    if (!first) return;
    char b[160];
    std::snprintf(b, sizeof b, "list box %s (left %.0f, width %.0f), rows hold %.1f em, header %.1f em", box ? "found" : "NOT found",
                  bl, bw, g_st.row_em, g_st.header_em);
    cmn::journal("ui: drill details: %s", b);
}

// The header and the rows of the page shown: the saved slots page * 10 .. + 9, the rows past them hidden
void write_rows(const Parts& p) {
    State& st = g_st;
    const int saved = (int)st.saved.size();
    const Range r = page_range(saved, st.page);
    for (int i = 0; i <= kSlotsPerPage; ++i) {
        char nm[16];
        std::snprintf(nm, sizeof nm, "c_item%d", i);
        MO* it = kid(p.scale, nm);
        MO* t = it ? kid(it, "e_txt_title") : nullptr;
        if (!t) continue;
        if (i == 0) {
            const std::string h = header_text(tr, saved, st.page, st.pages);
            gx::set_text(t, st.header_em > 0 ? logic::cut_em(h, st.header_em) : h);
            gx::set_visible(it, true);
            continue;
        }
        const int k = r.first + i - 1;
        if (k < r.last) {
            const model::pack::SlotSummary& s = st.saved[(size_t)k];
            gx::set_text(t, row_text(tr("details_slot", { std::to_string(s.id) }), s.name, s.frames, st.row_em));
            gx::set_visible(it, true);
        } else {
            gx::set_visible(it, false);
        }
    }
}

void show_window(const Parts& p);

// The description box's text: the whole text until its lines are known (hidden; the lines are read kMeasureAfter frames
// after the last time it had to be written), then the window shown
void write_detail(MO* t) {
    State& st = g_st;
    if (st.phase == Phase::Ready) return;                                  // (the window is show_window's)
    if (!st.measured_visible) gx::set_force_invisible(t, true);
    if (gx::msg(t) != st.text) {
        gx::set_text(t, st.text);
        st.filled_at = ui::tick();
    }
}

// Everything the dialog shows, in its own flow before its first draw, and again after the game's later show steps (the
// frame callback when no hook ran). Each write is compared first (the core): a second pass writes only what changed.
void apply(const Parts& p, const char* where) {
    State& st = g_st;
    const bool first = st.phase == Phase::Opening;
    write_fitted(p.title, clean_line(st.d.title));                       // the header: the drill's name
    if (p.meter) gx::set_visible(p.meter, false);                         // the top-left box: no frame meter, the source
    if (p.frame_text) {
        no_squash(p.frame_text);
        const TextGeo g = text_geo(p.frame_text);
        float bl = 0, bw = 0, w = g.ok ? g.w : 0;
        if (g.ok && box_of(kid(p.frame, "e_s9g_line"), &bl, &bw) && bl + bw - kBoxMargin - g.left > kMinFitWidth) {
            w = bl + bw - kBoxMargin - g.left;
            place_region(p.frame_text, g, g.left, w);
        }
        const std::string s = frame_text(st.d.character, source_label(tr, st.d.source));
        gx::set_text(p.frame_text, g.ok ? fit(s, w, g.fw) : s);
    }
    layout_list(p, first);
    write_rows(p);
    // the description: wrapped by the game in its own font, from the top
    MO* t = p.text;
    set_prop(t, "set_AutoWrap", gx::B(true));
    set_prop(t, "set_ForceAutoWrap", gx::B(true));
    no_squash(t);
    set_prop(t, "set_PageAlignment", gx::I(0));                           // LeftTop (the game's: 4, LeftCenter)
    float rw = 0, rh = 0;
    if (gx::size_of(t, "get_RegionSize", &rw, &rh) && std::fabs(rw - kDetailWidth) > 0.5f) gx::set_region(t, kDetailWidth, rh);
    if (st.phase == Phase::Ready) show_window(p);
    else write_detail(t);
    write_fitted(kid(kid(p.list, "c_item_0"), "e_text"), tr("open", {}));     // "Open drill" (the Manager's own label)
    write_fitted(kid(kid(p.list, "c_item_1"), "e_text"), tr("cancel", {}));
    if (!first) return;
    st.phase = Phase::Filled;
    st.filled_at = ui::tick();
    st.filled_by = where;
    cmn::journal("ui: drill details filled from %s (%zu slots saved, %d page(s), %zu bytes of text)", where, st.saved.size(), st.pages,
                 st.text.size());
}

// The scroll bar's two Rects: copies of the frame meter's base and first bar (the meter itself is hidden), added to
// c_contents, hidden until the lines are known. Only from the dialog's own flow (R19); at most kCloneTries asks.
void make_bar(const Parts& p) {
    State& st = g_st;
    const SF6CoreAPI* a = gx::api();
    if (st.clones || st.clone_tries >= kCloneTries || !a || !SF6CORE_HAS(a, ui_clone_control)) return;
    MO* base = kid(p.meter, "e_rect_base");
    MO* bar0 = kid(p.meter, "e_rect_0");
    if (!base || !bar0) return;
    ++st.clone_tries;
    const std::pair<MO*, const char*> made[2] = { { base, kTrackName }, { bar0, kThumbName } };
    for (const auto& [tmpl, name] : made) {
        if (kid(p.contents, name)) continue;
        void* c = nullptr;
        const SF6CoreStatus s = perf::counted(a)->ui_clone_control(gx::cl(), tmpl, p.contents, name, &c);
        if (s == SF6CORE_OK && c) perf::counted(a)->ui_set_visible(gx::cl(), c, 0);
    }
    st.clones = kid(p.contents, kTrackName) && kid(p.contents, kThumbName);
    cmn::journal("ui: drill details: scroll bar copies %s (try %d)", st.clones ? "made" : "refused", st.clone_tries);
}

void move_thumb(MO* th) {
    State& st = g_st;
    const Thumb t = thumb(kWindowLines * kLineHeight, st.total, kWindowLines, st.top);
    gx::set_size(th, kBarWidth, t.h);
    gx::set_pos(th, st.bar_x, st.bar_y + t.y, st.thumb_z);
}

// The bar placed next to the description (x = its region's left + 930 + 30, from its top, 18 lines high), in c_contents'
// units, shown only when there are more lines than the window holds
void place_bar(const Parts& p) {
    State& st = g_st;
    MO* track = kid(p.contents, kTrackName);
    MO* th = kid(p.contents, kThumbName);
    st.bar = false;
    if (st.total <= kWindowLines) {
        if (track) gx::set_visible(track, false);
        if (th) gx::set_visible(th, false);
        return;
    }
    const TextGeo g = text_geo(p.text);
    float cx = 0, cy = 0, z[3] = {};
    if (!track || !th || !g.ok || !gpos(p.contents, &cx, &cy)) {
        cmn::journal("ui: drill details: no scroll bar (%s)", track && th ? "the description's place unreadable" : "no copies");
        return;
    }
    st.bar_x = g.left + kDetailWidth + kBarGap - cx;
    st.bar_y = g.top + kBarTopOffset - cy;
    if (gx::vec3_of(track, "get_Position", z)) st.bar_z = z[2];
    if (gx::vec3_of(th, "get_Position", z)) st.thumb_z = z[2];
    const SF6CoreAPI* a = gx::api();
    for (MO* r : { track, th }) set_prop(r, "set_ControlPoint", gx::I(0));   // LeftTop
    if (a) {
        perf::counted(a)->ui_set_color(gx::cl(), track, kTrackColor);
        perf::counted(a)->ui_set_color(gx::cl(), th, kThumbColor);
    }
    gx::set_size(track, kBarWidth, kWindowLines * kLineHeight);
    gx::set_pos(track, st.bar_x, st.bar_y, st.bar_z);
    move_thumb(th);
    gx::set_visible(track, true);
    gx::set_visible(th, true);
    st.bar = true;
}

// The GUI may be written now (frame callback): our dialog up, our elements shown
bool writable() {
    const Phase ph = g_st.phase;
    return g_live.load() && g_st.gui_seen && (ph == Phase::Opening || ph == Phase::Filled || ph == Phase::Ready);
}

void hide_bar(const Parts& p) {                  // (the GUI alive: before the dialog ends)
    if (MO* c = kid(p.contents, kTrackName)) gx::set_visible(c, false);
    if (MO* c = kid(p.contents, kThumbName)) gx::set_visible(c, false);
}

// Lines [top, top + 18) of the description, and the thumb
void show_window(const Parts& p) {
    State& st = g_st;
    // (its colour spans opened / closed in it; compared whole here: the core's compare reads 511 bytes, and the same long
    // message written again is R7's crash)
    const std::string want = window_text(st.text, st.starts, st.top, kWindowLines);
    if (gx::msg(p.text) != want) gx::set_text(p.text, want);
    if (st.bar) if (MO* th = kid(p.contents, kThumbName)) move_thumb(th);
}

// kMeasureAfter frames after the fill: the lines of the whole text from the game's baselines (hidden first; a hidden
// text that the game did not lay out is measured again shown), then the window and the bar
void measure() {
    State& st = g_st;
    Parts p;
    if (!find_parts(param_now(), true, p)) {
        if (++st.measure_tries >= kMeasureTries) { st.phase = Phase::Ready; cmn::journal("ui: drill details: the description not found: not scrolled"); }
        else st.filled_at = ui::tick();
        return;
    }
    if (gx::msg(p.text) != st.text) { write_detail(p.text); return; }   // the game put its own text back: again, later
    const Lines ls = line_starts(st.text, [&](uint32_t g) { return baseline(p.text, g); });
    if (!ls.laid_out) {
        if (!st.measured_visible) {
            st.measured_visible = true;
            gx::set_force_invisible(p.text, false);
            st.filled_at = ui::tick();
            cmn::journal("ui: drill details: the hidden description was not laid out: read again shown");
            return;
        }
        if (++st.measure_tries < kMeasureTries) { st.filled_at = ui::tick(); return; }
        cmn::journal("ui: drill details: no baseline read: the whole description shown, not scrolled");
    }
    st.starts = ls.starts;
    st.total = (int)st.starts.size();
    st.top = 0;
    place_bar(p);
    show_window(p);
    gx::set_force_invisible(p.text, false);
    st.phase = Phase::Ready;
    cmn::journal("ui: drill details: %d line(s), window %d, %s", st.total, kWindowLines,
                 st.total > kWindowLines ? (st.bar ? "scrolled, with the bar" : "scrolled, no bar") : "all shown");
}

// ---- the key guide at the bottom (in game 04/11) ----------------------------------------------------------------------------
// The bar is the game's GUI "InputGuide" (two exist: the live one's UIAgent has _IsEnableInput), View / c_main /
// p_InputGuideList_0_fit / c_guideBtn / e_text_0_* (one text an entry, its glyphs inline) and p_ScrollText_0 / e_text
// (the help line). The menu writes it for its own dialogs only: for ours it keeps the Drill row's guide ("Go to Basic
// Settings" and Back; BlockInput on the menu's agent and InputGuideManager.RequestUpdate change nothing). So while our
// dialog is up (and no text dialog over it) the Drill row's "Go to Basic Settings" entry (an e_text_0_g_* item) shows
// ours instead: Confirm, Show More Slots on the game's own tab glyphs (copied from that entry: device and layout as the
// game draws them), Edit description; every other item but Back is hidden (the Drill row's own hints: "Edit Drill
// Name"), the items laid out again by the game's rule; the help line says how to scroll. Written from the frame (R1), every item found again by name each time, each write compared first (the core).
// Put back at the end (the entry's text and width, the help line), only where the bar still shows what we wrote.
constexpr uint64_t kGuideEvery = 5;              // the bar checked again every 5 frames (the game may write it back)

struct GuideParts { MO* btn = nullptr; MO* help = nullptr; };
GuideParts guide_parts() {
    GuideParts g;
    MO* root = gx::input_guide_main();
    const SF6CoreAPI* a = gx::api();
    if (!root || !a) return g;
    MO* list = static_cast<MO*>(perf::counted(a)->ui_find_child(root, "p_InputGuideList_0_fit", 4));
    g.btn = list ? static_cast<MO*>(perf::counted(a)->ui_find_child(list, "c_guideBtn", 2)) : nullptr;
    g.help = gx::input_guide_help();
    return g;
}

bool shown(MO* c) { return c && gx::visible(c) && !gx::force_invisible(c); }

// The bar's shown items, left to right, laid out again by the game's rule from the first one's x (after a width change)
void guide_relayout(MO* btn) {
    struct It { MO* t; float x, y, z, w, fw; };
    std::vector<It> v;
    for (MO* c = gx::first_child(btn); c; c = gx::next(c)) {
        if (gx::name(c).rfind("e_text_0_", 0) != 0 || !gx::is_type(c, "via.gui.Text") || !shown(c)) continue;
        float p[3] = {}, w = 0, h = 0, fw = 0, fh = 0;
        if (gx::vec3_of(c, "get_Position", p) && gx::size_of(c, "get_RegionSize", &w, &h) && gx::size_of(c, "get_FontSize", &fw, &fh))
            v.push_back({ c, p[0], p[1], p[2], w, fw });
    }
    std::stable_sort(v.begin(), v.end(), [](const It& a, const It& b) { return a.x < b.x; });
    std::vector<float> xs, ws, fws;
    for (const It& i : v) { xs.push_back(i.x); ws.push_back(i.w); fws.push_back(i.fw); }
    const std::vector<float> nx = guide_layout(xs, ws, fws);
    for (size_t i = 0; i < v.size(); ++i)
        if (std::fabs(nx[i] - v[i].x) > 0.5f) gx::set_pos(v[i].t, nx[i], v[i].y, v[i].z);
}

std::string tab_icons_fallback(bool pad) {       // no game entry to copy them from: the default tab keys by position
    if (pad) return "<ICON BtnL1><ICON +><ICON BtnR1>";
    const UINT vk = MapVirtualKeyW(kScanTabLeft, MAPVK_VSC_TO_VK);
    const char left = (vk >= 'A' && vk <= 'Z') ? (char)vk : 'Q';
    return std::string("<ICON Key") + left + "><ICON +><ICON KeyE>";
}

void guide_apply() {
    State& st = g_st;
    const GuideParts g = guide_parts();
    if (g.btn) {
        MO* host = st.guide_host.empty() ? nullptr : kid(g.btn, st.guide_host.c_str());
        if (!shown(host)) {                      // the Drill row's "Go to Basic Settings" (a guide-id item), first one shown
            host = nullptr;
            for (MO* c = gx::first_child(g.btn); c && !host; c = gx::next(c))
                if (gx::name(c).rfind("e_text_0_g_", 0) == 0 && gx::is_type(c, "via.gui.Text") && shown(c)) host = c;
        }
        if (host) {
            const std::string cur = gx::msg(host);
            float w = 0, h = 0, fw = 0, fh = 0;
            if (cur != st.guide_written && gx::size_of(host, "get_RegionSize", &w, &h)) {   // the game's own text: kept to put back
                st.guide_host = gx::name(host);
                st.guide_orig = cur;
                st.guide_orig_w = w;
                st.guide_saved = true;
                if (const std::string icons = leading_combo_icons(cur); !icons.empty()) st.tab_icons = icons;
            }
            const bool pad = ui::com::guide_mode_pad().value_or(false);
            const std::string tabs = st.tab_icons.empty() ? tab_icons_fallback(pad) : st.tab_icons;
            const std::string text = guide_entries(tr, pad, tabs, st.pages > 1);
            if (cur != text) {
                no_squash(host);
                gx::set_text(host, text);
                if (gx::size_of(host, "get_RegionSize", &w, &h) && gx::size_of(host, "get_FontSize", &fw, &fh)) {
                    const float want = (float)logic::guide_text_w(text, fw) * 1.15f + 4.0f;
                    if (std::fabs(w - want) > 0.5f) gx::set_region(host, want, h);
                }
                guide_relayout(g.btn);
                if (st.guide_written.empty())
                    cmn::journal("ui: drill details: the key guide's %s shows the dialog's entries (%s)", st.guide_host.c_str(),
                                 st.tab_icons.empty() ? "default tab glyphs" : "the game's tab glyphs");
            }
            st.guide_written = text;
            // the bar's other items, but Back (e_text_0_d_100, Esc / right click), hidden: the row's own hints are not ours
            bool hid = false;
            for (MO* c = gx::first_child(g.btn); c; c = gx::next(c)) {
                const std::string nm = gx::name(c);
                if (c == host || nm.rfind("e_text_0_", 0) != 0 || !gx::is_type(c, "via.gui.Text") || !shown(c)) continue;
                if (nm == "e_text_0_d_100" || gx::msg(c).find("<ICON KeyEsc>") != std::string::npos) continue;
                gx::set_visible(c, false);
                if (std::find(st.guide_hidden.begin(), st.guide_hidden.end(), nm) == st.guide_hidden.end()) st.guide_hidden.push_back(nm);
                hid = true;
            }
            if (hid) guide_relayout(g.btn);
        } else if (!st.guide_missing_logged) {
            st.guide_missing_logged = true;
            cmn::journal("ui: drill details: no guide-id item in the key guide: its entries left as they are");
        }
    }
    if (g.help) {
        const std::string want = tr("details_help", {});
        const std::string cur = gx::msg(g.help);
        if (cur != want) {
            if (cur != st.help_written || !st.help_saved) { st.help_orig = cur; st.help_saved = true; }
            gx::set_text(g.help, want);
        }
        st.help_written = want;
    }
}

// At the end: the entry and the help line put back where they still show what we wrote (the game may have written them)
void guide_restore() {
    State& st = g_st;
    if (!st.guide_saved && !st.help_saved && st.guide_hidden.empty()) return;
    const GuideParts g = guide_parts();
    if (g.btn) {
        bool moved = false;
        MO* host = st.guide_saved ? kid(g.btn, st.guide_host.c_str()) : nullptr;
        if (host && gx::msg(host) == st.guide_written) {
            gx::set_text(host, st.guide_orig);
            float w = 0, h = 0;
            if (gx::size_of(host, "get_RegionSize", &w, &h) && st.guide_orig_w > 1 && std::fabs(w - st.guide_orig_w) > 0.5f)
                gx::set_region(host, st.guide_orig_w, h);
            moved = true;
        }
        for (const std::string& nm : st.guide_hidden)  // (those the game has not made again meanwhile)
            if (MO* c = kid(g.btn, nm.c_str()); c && !gx::visible(c)) { gx::set_visible(c, true); moved = true; }
        if (moved) guide_relayout(g.btn);
    }
    st.guide_hidden.clear();
    if (st.help_saved && g.help && gx::msg(g.help) == st.help_written) gx::set_text(g.help, st.help_orig);
    st.guide_saved = st.help_saved = false;
    cmn::journal("ui: drill details: the key guide put back");
}

// ---- the inputs (raw, read on the frame) -------------------------------------------------------------------------------------
// A key pressed since the last read (its press count from the window's messages moved: a tap shorter than a frame
// counts too)
bool pressed(int vk, uint32_t* seen) {
    const uint32_t n = cmn::key_presses(vk);
    const bool p = n != *seen;
    *seen = n;
    return p;
}

void count_presses(State& st, const windows::PadHeld& pad) {   // (at the open: what was pressed before is no press)
    st.presses_f = cmn::key_presses(kKeyF);
    st.presses_esc = cmn::key_presses(kKeyEsc);
    st.presses_ml = cmn::key_presses(kMouseL);
    st.presses_mr = cmn::key_presses(kMouseR);
    st.presses_t = cmn::key_presses(kKeyT);
    st.pad_south.press(pad.south);
    st.pad_east.press(pad.east);
    st.pad_y.press(pad.y);
}

// A decide or back press while the dialog takes the inputs: the last one decides at the end. A key or pad press begins
// the end (the dialog closes at once on either button and on Back): the GUI written no more, the bar hidden first; a
// left click (on a button, or anywhere) is only recorded.
void end_presses(State& st, const windows::PadHeld& pad) {
    const bool f = pressed(kKeyF, &st.presses_f), esc = pressed(kKeyEsc, &st.presses_esc);
    const bool ml = pressed(kMouseL, &st.presses_ml), mr = pressed(kMouseR, &st.presses_mr);
    const bool ps = st.pad_south.press(pad.south), pe = st.pad_east.press(pad.east);
    if (st.editing || ui::tick() < st.quiet_until) return;   // (the text dialog's own F / Esc: never a press of ours)
    if (!st.input_seen || (st.phase != Phase::Filled && st.phase != Phase::Ready && st.phase != Phase::Ending)) return;
    const bool back = esc || mr || pe;
    const bool decide = !back && (f || ml || ps);
    if (!back && !decide) return;
    LastPress l;
    l.kind = back ? Press::Back : Press::Decide;
    l.at = ui::tick();
    l.by = back ? (esc ? "Esc" : mr ? "a right click" : "pad B / Circle") : (f ? "F" : ps ? "pad A / Cross" : "a left click");
    if (decide) if (MO* pm = gx::ref(st.dlg, "_Param")) gx::call_i32(gx::ref(pm, "_SelectList"), "get_SelectedIndex", &l.sel);
    st.last = l;
    if (back) ++g_backs; else ++g_decides;
    if (decide && !f && !ps) return;             // a click: the dialog ends only when it hit a button
    if (st.phase == Phase::Ending) { st.ending_at = ui::tick(); return; }   // (already ending: the last press kept)
    Parts p;                                     // the end begins: the bar hidden (the dialog still drawn), then no write
    if (writable() && find_parts(gx::ref(st.dlg, "_Param"), true, p)) hide_bar(p);
    g_live.store(false);
    st.before_end = st.phase;
    st.phase = Phase::Ending;
    st.ending_at = ui::tick();
}

// The keys of the dialog (raw): scroll Up / Down, W / Z (up) and S (down), the pad's d-pad and left stick, as the
// validated demo; slot pages on the game's tab keys (by position: QWERTY Q / E, AZERTY A / E) and the pad's L1 / R1;
// T / pad Y edits the description (the game's text dialog over this one: in game 04/11 it draws over it, Esc closes only
// it, this dialog keeps its state). One step at the press, then one every 3 frames after 12 frames held.
void keys(const windows::PadHeld& pad, bool t, bool py) {
    State& st = g_st;
    if (!writable()) return;                     // (ended: the keys are the menu's again)
    if ((t || py) && st.on_edit) {
        st.editing = true;
        ++g_edits;
        cmn::journal("ui: drill details: %s: the description edited in the game's text dialog", t ? "T" : "pad Y");
        st.on_edit();
        return;
    }
    const bool up = cmn::key_down(kKeyUp) || cmn::key_down(kKeyW) || cmn::key_down(kKeyZ) || pad.up;
    const bool down = cmn::key_down(kKeyDown) || cmn::key_down(kKeyS) || pad.down;
    const bool prev = cmn::scan_down(kScanTabLeft) || pad.l1;
    const bool next = cmn::scan_down(kScanTabRight) || pad.r1;
    int top = st.top, page = st.page;
    if (st.scroll.step(up || down)) top = std::clamp(st.top + (down ? 1 : -1), 0, max_top(st.total, kWindowLines));
    if (st.paging.step(prev || next)) page = std::clamp(st.page + (next ? 1 : -1), 0, st.pages - 1);
    if (top == st.top && page == st.page) return;
    Parts p;
    if (!find_parts(param_now(), true, p)) return;
    if (top != st.top) { st.top = top; show_window(p); }
    if (page != st.page) { st.page = page; write_rows(p); }
}

// ---- open, end ------------------------------------------------------------------------------------------------------------
bool guids() {                                   // System.Guid.Parse of the two guids, once
    if (g_guids) return true;
    Method* parse = gm::method(gm::type("System.Guid"), "Parse(System.String)");
    if (!parse) return false;
    auto one = [&](const char* s, uint8_t* out) {
        MO* str = reframework::API::get()->create_managed_string_normal(s);
        reframework::InvokeRet r{};
        if (!str || !gm::call(nullptr, parse, { str }, &r)) return false;
        std::memcpy(out, r.bytes.data(), 16);
        return true;
    };
    g_guids = one(kTitleGuid, g_title_guid) && one(kDetailGuid, g_detail_guid);
    return g_guids;
}

void start(Req r, bool training, bool paused) {
    auto refuse = [](const char* why) {
        ++g_refused;
        g_open.store(false);
        cmn::journal("ui: drill details not opened (%s)", why);
    };
    if (g_st.phase != Phase::None) { refuse("one is up"); return; }
    if (!training || !paused) { refuse("the menu is not up"); return; }
    TD* td = gm::type(kDialogType);
    Method* ctor = td ? gm::method_n(td, ".ctor", 0) : nullptr;
    Method* start_dialog = td ? gm::method_n(td, "StartDialog", 4) : nullptr;
    if (!td || !start_dialog) { refuse("no UITrainingImageDialog.StartDialog"); return; }
    if (!guids()) { refuse("System.Guid.Parse failed"); return; }
    if (!g_actions) g_actions = gm::new_array(gm::type("System.Action"), 2);
    if (!g_actions) { refuse("no System.Action[2]"); return; }
    MO* dlg = td->create_instance();
    if (!dlg) { refuse("create_instance failed"); return; }
    dlg->add_ref();
    if (ctor) gm::call(dlg, ctor, {});
    MO* param0 = gx::ref(dlg, "_Param");         // (the Param may come with the instance: our identity before StartDialog)
    bool can = true;
    if (gm::call_bool(dlg, "get_IsCanOpen", &can) && !can) { dlg->release(); refuse("the game cannot open it now"); return; }
    State st;
    st.phase = Phase::Opening;                   // (before StartDialog: its Param's hooks may run inside it)
    st.dlg = dlg;
    st.d = std::move(r.d);
    st.on_open = std::move(r.on_open);
    st.on_edit = std::move(r.on_edit);
    for (const model::pack::SlotSummary& s : st.d.slots) if (!s.empty) st.saved.push_back(s);
    st.pages = page_count((int)st.saved.size());
    st.text = detail_text(tr, st.d.description, st.d.author, source_label(tr, st.d.source));
    st.opened_at = ui::tick();
    count_presses(st, windows::pad_held());      // the Confirm that opened it, still held, is no press
    g_st = std::move(st);
    g_param.store(param0);
    g_live.store(true);
    reframework::InvokeRet ret{};          // (ui::lock is held: a Param hook inside StartDialog on this thread fills at once)
    if (!gm::call(dlg, start_dialog, { gm::arg_i32(0), g_title_guid, g_detail_guid, g_actions }, &ret)) {
        g_live.store(false);
        g_param.store(nullptr);
        dlg->release();
        g_st = State{};
        refuse("StartDialog threw");
        return;
    }
    if (MO* pm = param_now()) g_param.store(pm);
    ++g_opens;
    cmn::journal("ui: drill details opened: %zu slot(s) saved of %zu, %zu bytes of text%s", g_st.saved.size(), g_st.d.slots.size(),
                 g_st.text.size(), g_st.phase == Phase::Filled ? " (filled inside StartDialog)" : "");
}

// The dialog ended (or was let go): the reference released; "Open drill" chosen = the Manager's Open, an edit = the
// Manager's edit, now. Nothing of its GUI is touched here: its elements may already be released (the pooled GUI stays).
void finish(const char* why) {
    State& st = g_st;
    g_live.store(false);
    const LastPress l = st.last;
    const bool open = !st.closed_by_us && l.kind == Press::Decide && l.sel == 0 && ui::tick() - l.at <= kDecideToEnd;
    if (!cmn::closing()) guide_restore();        // (the key guide is the game's own GUI, not the dialog's: still there)
    if (st.dlg) st.dlg->release();
    std::function<void()> on_open = std::move(st.on_open);
    const char* what = open ? "Open drill" : l.kind == Press::Back ? "Back" : (l.kind == Press::Decide && l.sel == 1) ? "Cancel" : "no button";
    cmn::journal("ui: drill details closed (%s): %s (last press: %s%s)", why, what, l.kind == Press::None ? "none" : l.by,
                 l.kind == Press::Decide ? (l.sel == 0 ? " on Open drill" : l.sel == 1 ? " on Cancel" : " on ?") : "");
    g_st = State{};
    g_param.store(nullptr);
    g_ended_at.store(ui::tick() + 1);
    g_open.store(false);
    if (open && on_open) { ++g_drills_opened; on_open(); }
}

void close_now(const char* why) {                // (no write: the menu or training is going, and its GUI with it)
    State& st = g_st;
    g_live.store(false);
    st.closed_by_us = true;
    if (!cmn::closing()) guide_restore();
    if (st.dlg && !cmn::closing()) gm::call(st.dlg, "CloseDialog", {});
    st.phase = Phase::Closing;
    st.closing_at = ui::tick();
    cmn::journal("ui: drill details closed by CRSM (%s)", why);
}

void tick(bool training, bool paused) {
    State& st = g_st;
    bool ended = false;
    if (!gm::call_bool(st.dlg, "get_IsEnd", &ended)) ended = true;   // (unreadable: gone)
    if (ended) { g_live.store(false); finish(st.phase == Phase::Closing ? "ended after its close" : "ended"); return; }
    if (st.phase == Phase::Closing) {
        if (ui::tick() - st.closing_at > kClosingWait) finish("closed, its end not seen");
        return;
    }
    st.menu_gone = (training && paused) ? 0 : st.menu_gone + 1;
    if (!training) { close_now("training ended"); return; }
    if (st.menu_gone >= kMenuGoneFrames) { close_now("the menu closed"); return; }
    const windows::PadHeld pad = windows::pad_held(st.phase == Phase::Ready);
    if (st.phase == Phase::Ending) {             // a decide / back press: the dialog closes (get_IsEnd above)
        end_presses(st, pad);                    // (a later press still counts: the last one decides)
        if (ui::tick() - st.ending_at > kRearm) {   // not taken: the dialog is still up, the inputs back
            st.phase = st.before_end;
            st.last = LastPress{};
            g_live.store(true);
            ++g_rearms;
            cmn::journal("ui: drill details: the press did not close the dialog: its inputs again");
        }
        return;
    }
    if (!g_param.load()) if (MO* pm = param_now()) g_param.store(pm);
    end_presses(st, pad);
    const bool t = pressed(kKeyT, &st.presses_t), py = st.pad_y.press(pad.y);   // (read every frame: an early press is spent)
    if (st.phase == Phase::Ending || st.editing) return;   // (the text dialog over it: its own guide, its keys)
    if (writable() && (st.phase == Phase::Filled || st.phase == Phase::Ready) && ui::tick() >= st.guide_next) {
        st.guide_next = ui::tick() + kGuideEvery;
        try { guide_apply(); } catch (...) { ui::fault("drill details key guide"); }
    }
    switch (st.phase) {
        case Phase::Opening: {                   // our show seen but its hook found no part: filled from here (no copies:
            Parts p;                             //  duplicate() from the frame comes back unfinished, R19)
            if (writable() && ui::tick() - st.opened_at >= kFillWait && find_parts(param_now(), true, p)) apply(p, "the frame");
            if (!st.gui_seen && ui::tick() - st.opened_at == 120)
                cmn::journal("ui: drill details: none of the dialog's show hooks ran for it: its texts are the game's");
            break;
        }
        case Phase::Filled:
            if (writable() && ui::tick() >= st.filled_at + kMeasureAfter) { PERF_SUB(pl, "UI:details_lines"); measure(); }
            break;
        case Phase::Ready: keys(pad, t, py); break;
        default: break;
    }
}

// ---- hooks (the dialog's own flow, the game's UI job threads) ---------------------------------------------------------------
// Our dialog's Param, for a write: the one our instance holds, or (not known yet: inside StartDialog) a Param of this type
// while ours opens. The game's own image dialog ("The Frame Meter and You") is never written.
bool ours(void* param) {
    void* p = g_param.load();
    if (p) return param == p;
    return param && g_st.phase == Phase::Opening && gx::is_type(static_cast<MO*>(param), kParamType);
}

MO* flow_param(MO* flow) {                       // a nested flow's Param (the Lua probe read either)
    MO* p = gx::call_obj(flow, "get_Param");
    return p ? p : gx::ref(flow, "_Param");
}

void show_body(void* param, const char* where, bool input_wait) {
    std::lock_guard lk(ui::lock());
    try {
        State& st = g_st;
        if (!g_live.load() || st.phase == Phase::None || st.phase == Phase::Closing || st.phase == Phase::Ending || !ours(param)) return;
        st.gui_seen = true;                      // our dialog's show: the pooled GUI holds our elements now
        if (input_wait) st.input_seen = true;    // it takes the inputs from now: the presses count
        Parts p;
        if (!find_parts(static_cast<MO*>(param), true, p)) return;
        apply(p, where);
        const bool had = st.clones;
        make_bar(p);
        if (st.phase == Phase::Ready && st.clones && !had) place_bar(p);
    } catch (...) { ui::fault("drill details fill"); }
}

struct ShowCall { void* self = nullptr; };
thread_local HookStack<ShowCall> s_before, s_showed, s_input;

int before_pre(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    ShowCall* s = s_before.push();
    if (s && g_open.load(std::memory_order_relaxed)) s->self = argv[1];
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void before_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const ShowCall c = s_before.pop();
    if (!c.self || cmn::closing() || !ui::active()) return;
    __try { show_body(c.self, "BeforeShowObject", false); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("drill details BeforeShowObject post (SEH)"); }
}

int showed_pre(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    ShowCall* s = s_showed.push();
    if (s && g_open.load(std::memory_order_relaxed)) s->self = argv[1];
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void showed_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const ShowCall c = s_showed.pop();
    if (!c.self || cmn::closing() || !ui::active()) return;
    __try { show_body(c.self, "ShowedObject", false); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("drill details ShowedObject post (SEH)"); }
}

int input_pre(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    ShowCall* s = s_input.push();
    if (s && g_open.load(std::memory_order_relaxed)) s->self = argv[1];
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void input_body(void* flow) { if (MO* p = flow_param(static_cast<MO*>(flow))) show_body(p, "UIFlowInputWait.OnEnter", true); }
void input_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const ShowCall c = s_input.pop();
    if (!c.self || cmn::closing() || !ui::active()) return;
    __try { input_body(c.self); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("drill details UIFlowInputWait.OnEnter post (SEH)"); }
}

}  // namespace

bool request(Drill drill, std::function<void()> on_open, std::function<void()> on_edit) {
    if (!ui::active() || cmn::closing() || g_open.exchange(true)) return false;
    std::lock_guard lk(g_req_mx);
    g_req = Req{ std::move(drill), std::move(on_open), std::move(on_edit) };
    return true;
}

bool is_open() {
    if (g_open.load()) return true;
    const uint64_t e = g_ended_at.load();
    return e && ui::tick() + 1 < e + kAfterEnd;
}

void edit_done(bool changed, const std::string& description) {
    std::lock_guard lk(ui::lock());
    State& st = g_st;
    if (!st.editing) return;                     // (the dialog ended meanwhile)
    st.editing = false;
    count_presses(st, windows::pad_held());      // the F / Esc that closed the text dialog: already counted, never ours
    st.quiet_until = ui::tick() + kAfterEnd;
    st.guide_next = 0;                           // the guide the text dialog wrote: ours again at once
    if (!changed || !writable()) return;
    st.d.description = description;              // the new text in place: whole, hidden, its lines read again, from the top
    st.text = detail_text(tr, st.d.description, st.d.author, source_label(tr, st.d.source));
    st.phase = Phase::Filled;
    st.measured_visible = false;
    st.measure_tries = 0;
    st.top = 0;
    Parts p;
    if (find_parts(param_now(), true, p)) write_detail(p.text);
    st.filled_at = ui::tick();
    cmn::journal("ui: drill details: the new description shown (%zu bytes)", st.text.size());
}

void frame(bool training, bool paused) {
    std::optional<Req> r;
    {
        std::lock_guard lk(g_req_mx);
        r.swap(g_req);
    }
    if (r) {
        try { start(std::move(*r), training, paused); } catch (...) { g_open.store(false); ui::fault("drill details open"); }
    }
    if (g_st.phase != Phase::None) tick(training, paused);
}

void specs(std::vector<mech::hooks::Spec>& out) {
    ui::spec(out, kParamType, "BeforeShowObject", before_pre, before_post,
             "the drill details are written at the dialog's ShowedObject / UIFlowInputWait (a frame of the game's texts may show)", &ok_before);
    ui::spec(out, kParamType, "ShowedObject", showed_pre, showed_post, "the drill details are written at BeforeShowObject / UIFlowInputWait or from the frame", &ok_showed);
    ui::spec(out, "app.training.UITrainingImageDialog.UIFlowInputWait", "OnEnter", input_pre, input_post,
             "the drill details' scroll bar is copied at BeforeShowObject / ShowedObject only, and no decide / back press is read (no Open drill)", &ok_input);
}

void hand_back() {
    g_live.store(false);                         // (first: nothing of the dialog written from here)
    {
        std::lock_guard lk(g_req_mx);
        g_req.reset();
    }
    if (!cmn::closing()) { try { guide_restore(); } catch (...) {} }
    if (g_st.dlg && !cmn::closing()) {
        gm::call(g_st.dlg, "CloseDialog", {});
        g_st.dlg->release();
    }
    g_st = State{};
    g_param.store(nullptr);
    g_open.store(false);
}

std::string status() {
    char b[320];
    std::snprintf(b, sizeof b, "%s, opened %llu, refused %llu, decide presses %llu, back presses %llu, presses not taken %llu, drills "
                  "opened %llu, edits %llu; hooks before-show %d, showed %d, input-wait %d", phase_name(g_st.phase),
                  (unsigned long long)g_opens, (unsigned long long)g_refused, (unsigned long long)g_decides, (unsigned long long)g_backs,
                  (unsigned long long)g_rearms, (unsigned long long)g_drills_opened, (unsigned long long)g_edits, ok_before, ok_showed, ok_input);
    return b;
}

}  // namespace ui::details
