// SF6 Slots -- an interface pack of portraits per character as a grid of characters in Options > Interface (Wael 10/11: "instead of
// rows, the portraits system of the Replays' Outfit Settings; on the cell, the choice as on Stage Settings; R / pad X and we SEE the
// portrait change"). Which packs, which values: ui_packs_logic.hpp (grid_of); the sections: ui_packs.cpp.
//
// The section: the core's section with a page of ours (opt_section_place, core 0.9.9), its name the pack's, its description the pack's
// author. Its page, in the right side of the Options screen, is Battle Settings > Other > Opponent Outfit Settings' grid as the
// Replays' Settings tab draws it (replay_tab.cpp, read 05/11), moved to the Options screen's right side (options_page.hpp's kPX):
// the panel and the left line of the stage page (title_preview.cpp's), 4 columns of 160 x 72 cells 168 x 80 apart, 8 rows shown
// (the scroll bar of the stage page past them), the characters in the game's grid order. A cell is the game's own (game_cell.hpp,
// Wael 10/11: "much more faithful to the game's page"): the Options rows' cell shell at the game's cell size, the portrait inside it
// fading in from its left, the character's name and the game's "On" (when the character has a portrait of the pack) in the title
// font, the game's darken while the left list has the cursor, the focused cell light. The portrait: the one the character has now
// (the game's own: the game's texture, as the stage pak holds a copy of it), its pak copy loaded once for the session
// (options_page.cpp's pictures).
//
// While the cursor of the left list is on the section: the grid dimmed, no cell focused. Confirm there gives the page the input
// (opt_section_hold; the screen's input blocked, every key read here: optpage::Keys, the game's own bindings):
//   directions        move (wrapping, as the game's grid); the shown portrait goes back to the chosen one
//   R / pad X         the focused character's next portrait shown on its cell (wrapping; back on the chosen one: nothing pending),
//                     as Stage Settings' "Stage Variation"
//   Confirm (A)       the portrait shown is chosen: settings.json and the loader at once (ui_packs.cpp's apply), "On" follows
//   Y                 the portrait shown given to every character that has one of that name (the game's own: every character back to
//                     the game's), after the game's Yes / No box
//   Back (B / Esc)    back to the left list (the input given back once every key is up)
// The game's "Restore Default Settings" on the section (core 0.9.10's SF6CoreOptSectionState.resets): every character back to the
// game's own portrait.
// The game's guide bar and description line give way to ours at their places (the keys with the game's icons; "<character>: <the
// portrait shown>"), put back when the page lets go.
//
// The copies (R19: made inside the guide list's update, fs_name.cpp's hook, from the Options screen's own parts: optpage templates),
// under the screen's main control, named e_slots_uig_*, found again by name (R35), hidden and ForceInvisible until shown. Every write
// from LateUpdateBehavior, through the core (R1).
#include "ui_packs_grid.hpp"
#include "game_cell.hpp"
#include "slots.hpp"
#include "slots_ui.hpp"
#include "options_page.hpp"
#include "replay_logic.hpp"
#include "words.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace slots::uipacks::gridpage {
namespace {

using namespace optpage;
using words::G;
using words::W;
using words::tr;
using words::trf;

// the copies (not "p_...": never taken for a UI part, R20); their order in defs(): these six; the cells' (game_cell.hpp) after them,
// named kCellPrefix + "g?NN"
constexpr const char* kCellPrefix = "e_slots_uig_";
constexpr const char* kPanel = "e_slots_uig_panel";        // the page's panel: a cell shell holding the three below
constexpr const char* kPMask = "e_slots_uig_pmask";        // in its c_bg: the fading mask
constexpr const char* kPBg = "e_slots_uig_pbg";            //   its background
constexpr const char* kPTint = "e_slots_uig_ptint";        //   its tint while the page has the input
constexpr const char* kLine = "e_slots_uig_line";          // the left bar
constexpr const char* kTrack = "e_slots_uig_track";        // the scroll bar
constexpr const char* kThumb = "e_slots_uig_thumb";
constexpr const char* kKeysLine = "e_slots_uig_keys";      // the keys, at the guide bar's place
constexpr const char* kDescLine = "e_slots_uig_desc";      // the description, at the game's description line's place
enum { kIPanel, kILine, kITrack, kIThumb, kIKeys, kIDesc };
constexpr int kCols = 4, kVisRows = 8, kSlots = kCols * kVisRows;
// the grid (replay_tab.cpp's: Opponent Outfit Settings' places, read 05/11), moved to the Options screen's right side
constexpr float kGX = 760 + kPX, kGY = 184, kCH = 72, kSX = 168, kSY = 80;
constexpr float kBarX = kGX + kCols * kSX + 2, kBarY = kGY, kBarW = 6, kBarH = kVisRows * kSY - (kSY - kCH);
constexpr float kBottomW = 1840;
// the cells: their shell at kPrioCell, the darken one over, the texts two over (game_cell.hpp)
constexpr int64_t kPrioPanel = 60023, kPrioCell = 60024, kPrioPic = 60025, kPrioTop = 60027;
constexpr uint32_t kCopyAskEvery = 120;
constexpr int kCopyRefusedMax = 3;
constexpr int kMenuDecide = 63, kMenuCancel = 64, kMenuRemove = 88, kMenuExtra = 306;   // app.InputAssign.Digital.Id

uint32_t g_frame = 0;
// the copies
struct CopyDef { std::string name; int tmpl; };
std::vector<CopyDef> g_defs;
const std::vector<CopyDef>& defs() {
    if (!g_defs.empty()) return g_defs;
    g_defs = { { kPanel, kTShell }, { kLine, kTMask }, { kTrack, kTBarBg }, { kThumb, kTBar }, { kKeysLine, kTGuideText }, { kDescLine, kTGuideText } };
    return g_defs;
}
std::atomic<int> g_copy_status{ 1 };        // the last copy job: SF6CORE_OK, an error, 1 none yet
uint32_t g_copy_asked = 0;
int g_copy_refused = 0;
// Inside the guide list's update (R19): the copies under the screen's main control, from the Options screen's own parts
void make_copies() {
    const SF6CoreAPI* a = A();
    int status = -3000;
    try {
        Obj* agent = options_agent();
        Obj* cm = agent ? call_obj(agent, "get_ControlMain") : nullptr;
        const Templates tm = templates(cm, std::string());
        if (cm && tm.basic()) {
            status = SF6CORE_OK;
            for (const CopyDef& w : defs()) {
                void* c = a->ui_find_child(cm, w.name.c_str(), 0);
                if (!c) {
                    const SF6CoreStatus st = a->ui_clone_control(C(), tm.for_copy(w.tmpl, kTTexture), cm, w.name.c_str(), &c);
                    if (st != SF6CORE_OK || !c) { status = st != SF6CORE_OK ? st : -3001; break; }
                    a->ui_set_force_invisible(C(), c, 1);
                    a->ui_set_visible(C(), c, 0);
                }
                if (w.name == kPanel && c) make_panel_parts(c, tm, kPMask, kPBg, kPTint);
            }
            // the cells: the game's (game_cell.hpp) from the screen's own parts: the rows' cell shell, the header's title text, a
            // plain texture, a row's darken (missing: the rounded mask, same shape)
            gamecell::Parts parts;
            parts.shell = tm.t[kTShell];
            parts.text = tm.for_copy(kTTitle, kTText);
            parts.tex = tm.t[kTTexture];
            parts.darken = tm.for_copy(kTDarken, kTMask);
            if (status == SF6CORE_OK && !parts.ok()) status = -3002;
            for (int s = 0; status == SF6CORE_OK && s < kSlots; ++s) status = gamecell::make(cm, parts, kCellPrefix, s);
        }
    } catch (...) { status = -2000; }
    g_copy_status.store(status, std::memory_order_release);
}
struct Copies {
    std::vector<void*> c;
    std::vector<gamecell::Cell> cells;           // kSlots
    void* pmask = nullptr; void* pbg = nullptr; void* ptint = nullptr;
    void* get(size_t i) const { return i < c.size() ? c[i] : nullptr; }
    const gamecell::Cell& cell(int s) const { return cells[static_cast<size_t>(s)]; }
    bool all() const {
        if (c.size() != g_defs.size() || c.empty() || cells.size() != static_cast<size_t>(kSlots)) return false;
        for (void* x : c) if (!x) return false;
        for (const gamecell::Cell& x : cells) if (!x.all()) return false;
        return true;
    }
};
// The copies, found once per screen (one walk per 32 names, core ui_find_many), kept while the main control and the panel are the ones
// found (one cached look a frame)
Copies g_cache;
void* g_cache_cm = nullptr;
uint32_t g_cache_at = 0;
Painter g_paint{ "ui grid" };
Copies copies(void* cm) {
    const SF6CoreAPI* a = A();
    const auto& d = defs();
    if (cm == g_cache_cm && g_cache.all() && a->ui_find_cached(cm, kPanel) == g_cache.c[kIPanel]) return g_cache;
    if (cm == g_cache_cm && !g_cache.c.empty() && !g_cache.all() && g_frame - g_cache_at < 30) return g_cache;
    if (cm != g_cache_cm) g_paint.forget();
    g_cache_cm = cm;
    g_cache_at = g_frame;
    static std::vector<std::string> names;
    if (names.size() != d.size()) { names.clear(); for (const CopyDef& w : d) names.push_back(w.name); }
    find_all(cm, names, g_cache.c);
    g_cache.cells.resize(kSlots);
    for (int s = 0; s < kSlots; ++s) g_cache.cells[static_cast<size_t>(s)] = gamecell::find(cm, kCellPrefix, s);
    void* panel = g_cache.get(kIPanel);
    void* pbg = panel ? a->ui_find_child(panel, "c_bg", 1) : nullptr;
    g_cache.pmask = pbg ? a->ui_find_child(pbg, kPMask, 0) : nullptr;
    g_cache.pbg = pbg ? a->ui_find_child(pbg, kPBg, 0) : nullptr;
    g_cache.ptint = pbg ? a->ui_find_child(pbg, kPTint, 0) : nullptr;
    return g_cache;
}
float cell_x(int s) { return kGX + static_cast<float>(s % kCols) * kSX; }
float cell_y(int s) { return kGY + static_cast<float>(s / kCols) * kSY; }
// Once per screen: every copy's style and place
void lay_out(void* cm, const Copies& c) {
    const SF6CoreAPI* a = A();
    read_enums();
    set_origin(cm);
    lay_out_panel(c.get(kIPanel), c.pmask, c.pbg, c.ptint, kLPanelX, kLPanelY, kLPanelW, kLPanelH, kColLPanelBg, kColLPanelTint, kPrioPanel);
    style_left_line(c.get(kILine), kLLineX, kLLineY, kLLineW, kLLineH, kColLLine, kPrioCell);
    style_rect(c.get(kITrack), kBarX, kBarY, kBarW, kBarH, kColLTrack, kPrioCell);
    style_rect(c.get(kIThumb), kBarX, kBarY, kBarW, kBarH, kColWhite, kPrioPic);
    for (void* k : { c.get(kIKeys), c.get(kIDesc) }) {               // the guide bar's text, left aligned
        if (!k) continue;
        set_enum(k, "set_ControlPoint(via.gui.ControlPoint)", g_cp_leftcenter);
        set_enum(k, "set_PageAlignment(via.gui.PageAlignment)", g_cp_leftcenter);
        set_enum(k, "set_AutoRegionFit(via.gui.RegionFitType)", 0);
        set_enum(k, "set_AutoWrap(System.Boolean)", 0);
        set_enum(k, "set_Priority(System.UInt16)", kPrioTop);
        set_enum(k, "set_MaskType(via.gui.MaskType)", 1);
        a->ui_set_region_size(C(), k, kBottomW, 60);
        a->ui_set_color(C(), k, kColText);
    }
    const std::string on = words::game(G::On);
    for (int s = 0; s < kSlots; ++s) gamecell::lay_out(c.cell(s), cell_x(s), cell_y(s), origin_x(), origin_y(), kPrioCell, on.c_str());
}

// ---- the game's description line (InputGuide > p_ScrollText_0): hidden while the page has the input, ours at its place ----
std::set<uintptr_t> g_desc_hidden;
void game_desc(bool hide) {
    const SF6CoreAPI* a = A();
    void* buf[4] = {};
    const uint32_t n = a->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        const uintptr_t id = reinterpret_cast<uintptr_t>(buf[i]);
        Obj* gcm = call_obj(static_cast<Obj*>(buf[i]), "get_ControlMain");
        void* sc = gcm ? a->ui_find_child(gcm, "p_ScrollText_0", 8) : nullptr;
        if (!sc) continue;
        if (hide && !g_desc_hidden.count(id)) { a->ui_set_force_invisible(C(), sc, 1); g_desc_hidden.insert(id); }
        else if (!hide && g_desc_hidden.count(id)) a->ui_set_force_invisible(C(), sc, 0);
    }
    if (!hide) g_desc_hidden.clear();
}

// ---- the page's state ----
Keys g_keys;
Guides g_guides;
GameBox g_box;
int64_t g_sec = -1;                          // the section the page shows (-1 none)
const Grid* g_grid = nullptr;                // its grid, as last given (the order below is its)
std::vector<int> g_order;                    // the grid's characters in the game's grid order (indexes in grid->fighters)
bool g_up = false;                           // the page is shown
bool g_held = false;                         // the page has the input
bool g_unhold = false;                       // the input to give back once every key is up
bool g_ok_held = true;                       // Confirm was down at the last look (a key held acts once released)
int g_cur = 0, g_top = 0;                    // the focused character (in g_order), the first row shown
int g_preview = -1;                          // the value shown on the focused cell (-1: its chosen one)
int g_all_value = -1;                        // Y: the value given to everyone once the box says Yes
std::string g_all_name;
float g_keys_x = 60, g_keys_y = 1040, g_desc_x = 60, g_desc_y = 1004;
bool g_need_hide = false;                    // drop(): the copies hidden at the next frame that has the screen
std::map<int64_t, uint32_t> g_resets_seen;   // section -> the game's reset count last seen (core SF6CoreOptSectionState.resets)
struct SlotShown { int f = -2; int look = -1; std::string pic; };
SlotShown g_slot[kSlots];
std::string g_keys_text, g_desc_text;
int g_bar_top = -1, g_bar_rows = -1;

void forget_shown() {
    for (SlotShown& s : g_slot) s = SlotShown{};
    g_keys_text.clear();
    g_desc_text.clear();
    g_bar_top = g_bar_rows = -1;
}
// A character folder the game has no name for (no FighterNameMessage, read 10/11): its name as players know it
std::string unnamed(int fid) { return fid == 103 ? "Dark Ingrid" : std::string(); }
std::string caption_of(const std::string& fid) {
    const int n = fid_number(fid);
    std::string c = words::fighter_caption(n);
    if (c.empty()) c = replay::grid_caption(words::fighter_name(n));
    if (c.empty()) c = replay::grid_caption(unnamed(n));
    return c.empty() ? "#" + fid : c;
}
std::string name_of_fighter(const std::string& fid) {
    std::string c = words::fighter_name(fid_number(fid));
    if (c.empty()) c = unnamed(fid_number(fid));
    return c.empty() ? "#" + fid : c;
}
void make_order(const Grid& g) {
    const auto& go = replay::grid_order();
    auto rank = [&](const std::string& fid) {
        const int n = fid_number(fid);
        for (size_t i = 0; i < go.size(); ++i) if (go[i] == n) return static_cast<int>(i);
        return 1000 + n;
    };
    g_order.clear();
    for (size_t i = 0; i < g.fighters.size(); ++i) g_order.push_back(static_cast<int>(i));
    std::stable_sort(g_order.begin(), g_order.end(), [&](int a, int b) { return rank(g.fighters[static_cast<size_t>(a)].fid) < rank(g.fighters[static_cast<size_t>(b)].fid); });
    g_grid = &g;
}
const GridFighter* fighter_at(int i) {
    if (!g_grid || i < 0 || i >= static_cast<int>(g_order.size())) return nullptr;
    return &g_grid->fighters[static_cast<size_t>(g_order[static_cast<size_t>(i)])];
}
void preload(const GridFighter& f) { for (const GridValue& v : f.values) if (!v.picture.empty()) picture(v.picture, "ui grid"); }

// A cell's parts the page shows and hides (its two pictures inside its shell: shown with it)
void show_cell(const gamecell::Cell& cell, bool on) {
    for (gamecell::What w : { gamecell::kShell, gamecell::kName, gamecell::kDarken }) g_paint.show_if(cell.get(w), on);
    if (!on) g_paint.show_if(cell.get(gamecell::kOn), false);
}
void page_hide(const Copies& c) {
    for (size_t i = 0; i < c.c.size(); ++i) g_paint.show_if(c.c[i], false);
    for (const gamecell::Cell& cell : c.cells) show_cell(cell, false);
    g_paint.show_if(c.ptint, false);
    forget_shown();
    g_up = false;
}
void let_go(const char* why) {               // the input given back (once the keys are up), the game's guide and description back
    if (g_held) logf("ui grid: page left (%s)", why);
    if (g_held) g_unhold = true;
    g_held = false;
    g_preview = -1;
    g_guides.hide(false);
    game_desc(false);
}

// One frame of drawing (written only when it changed)
void paint(const Copies& c, const Ticks& t, int mode) {
    const SF6CoreAPI* a = A();
    const int n = static_cast<int>(g_order.size());
    const int rows = (n + kCols - 1) / kCols;
    if (!g_up) {
        for (size_t i : { size_t(kIPanel), size_t(kILine) }) g_paint.show_if(c.get(i), true);
        g_up = true;
    }
    g_paint.show_if(c.ptint, g_held);
    if (g_held) g_top = replay::window_top(g_top, g_cur / kCols, rows, kVisRows);
    for (int sl = 0; sl < kSlots; ++sl) {
        SlotShown& sh = g_slot[sl];
        const int idx = g_top * kCols + sl;
        const GridFighter* f = idx < n ? fighter_at(idx) : nullptr;
        const gamecell::Cell& cell = c.cell(sl);
        if (!f) {
            if (sh.f != -1) {
                show_cell(cell, false);
                sh = SlotShown{};
                sh.f = -1;
            }
            continue;
        }
        const int chosen = grid_value(*f, t);
        const bool foc = g_held && idx == g_cur;
        const int shown = foc && g_preview >= 0 && g_preview < static_cast<int>(f->values.size()) ? g_preview : chosen;
        const int look = foc ? 2 : g_held ? 1 : 0;
        if (sh.f != g_order[static_cast<size_t>(idx)]) {
            sh = SlotShown{};
            sh.f = g_order[static_cast<size_t>(idx)];
            a->ui_set_text(C(), cell.get(gamecell::kName), caption_of(f->fid).c_str());
            show_cell(cell, true);
        }
        gamecell::look(cell, look == 2 ? gamecell::Look::Focus : look == 1 ? gamecell::Look::Plain : gamecell::Look::Dim);   // every frame
        sh.look = look;
        // the portrait: the one wanted once loaded; until then the one it showed (same character), so a change never blinks
        const std::string& res = f->values[static_cast<size_t>(shown)].picture;
        void* h = res.empty() ? nullptr : picture(res, "ui grid");
        if (h && a->ui_texture_ready(h) == 1) {
            gamecell::picture(cell, h, gamecell::crop_for(res));
            sh.pic = res;
        } else if (res.empty() || sh.pic.empty()) {
            gamecell::no_picture(cell);
        }
        g_paint.show_if(cell.get(gamecell::kOn), chosen != 0);
    }
    const bool bar = rows > kVisRows;
    g_paint.show_if(c.get(kITrack), bar);
    g_paint.show_if(c.get(kIThumb), bar);
    if (bar && (g_bar_top != g_top || g_bar_rows != rows)) {
        const float h = kBarH * kVisRows / static_cast<float>(rows), y = kBarY + (kBarH - h) * static_cast<float>(g_top) / static_cast<float>(rows - kVisRows);
        place(c.get(kIThumb), kBarX, y);
        a->ui_set_size(C(), c.get(kIThumb), kBarW, h);
        g_bar_top = g_top;
        g_bar_rows = rows;
    }
    // the keys and the description, at the guide bar's places, while the page has the input
    if (g_held) {
        std::string keys = key_item(icons(kMenuDecide, mode), tr(W::TitleKeySelect)) + "     " + key_item(icons(kMenuCancel, mode), tr(W::TitleKeyBack)) +
                           "     " + key_item(icons(kMenuRemove, mode), tr(W::UiKeyPortrait)) + "     " + key_item(icons(kMenuExtra, mode), tr(W::UiKeyAll));
        std::string desc;
        if (const GridFighter* f = fighter_at(g_cur)) {
            const int chosen = grid_value(*f, t);
            const int shown = g_preview >= 0 && g_preview < static_cast<int>(f->values.size()) ? g_preview : chosen;
            desc = fold2(tr(W::FmtNameValue), name_of_fighter(f->fid), f->values[static_cast<size_t>(shown)].name);
        }
        if (g_keys_text.empty()) { place(c.get(kIKeys), g_keys_x, g_keys_y); place(c.get(kIDesc), g_desc_x, g_desc_y); }
        if (keys != g_keys_text) { a->ui_set_text(C(), c.get(kIKeys), keys.c_str()); g_keys_text = keys; }
        if (desc != g_desc_text) { a->ui_set_text(C(), c.get(kIDesc), desc.c_str()); g_desc_text = desc; }
        g_paint.show_if(c.get(kIKeys), true);
        g_paint.show_if(c.get(kIDesc), true);
    } else {
        g_paint.show_if(c.get(kIKeys), false);
        g_paint.show_if(c.get(kIDesc), false);
        g_keys_text.clear();
        g_desc_text.clear();
    }
}

// Y's answer: every character given the value of that name (the game's own: index 0)
void apply_all(const Section& s, TicksFn ticks, ApplyFn apply) {
    Ticks t = ticks(*s.pack);
    int changed = 0;
    for (const GridFighter& f : s.grid->fighters) {
        int v = -1;
        if (g_all_value == 0) v = 0;
        else for (size_t k = 1; k < f.values.size() && v < 0; ++k) if (f.values[k].name == g_all_name) v = static_cast<int>(k);
        if (v >= 0 && grid_set(*s.pack, *s.grid, f.fid, v, t)) ++changed;
    }
    logf("ui grid: \"%s\" given to every character that has it (%d changed)", g_all_name.c_str(), changed);
    if (changed) apply(*s.pack, t, "every character");
}

// The page's keys (the section held: the screen's input is blocked, every key is ours)
void page_keys(const Section& s, TicksFn ticks, ApplyFn apply) {
    if (g_box.is_open()) {                                          // the game's box: its keys are its own
        const int r = g_box.poll();
        if (r < 0) return;
        g_box.drop();
        if (r == 1) apply_all(s, ticks, apply);
        g_preview = -1;
        g_all_value = -1;
        g_keys.swallow();
        return;
    }
    if (g_keys.pressed(kBack)) { let_go("Back"); return; }
    const int n = static_cast<int>(g_order.size());
    if (n <= 0) return;
    int dx = 0, dy = 0;
    if (g_keys.repeat(kLeft, 20, 6)) dx = -1;
    if (g_keys.repeat(kRight, 20, 6)) dx = 1;
    if (g_keys.repeat(kUp, 20, 6)) dy = -1;
    if (g_keys.repeat(kDown, 20, 6)) dy = 1;
    if (dx || dy) {
        const int cur = replay::grid_move(std::min(g_cur, n - 1), dx, dy, n, kCols);
        if (cur != g_cur) {
            g_cur = cur;
            g_preview = -1;
            if (const GridFighter* f = fighter_at(g_cur)) preload(*f);
        }
    }
    const GridFighter* f = fighter_at(g_cur);
    if (!f || f->values.empty()) return;
    const Ticks t = ticks(*s.pack);
    const int chosen = grid_value(*f, t);
    const int shown = g_preview >= 0 && g_preview < static_cast<int>(f->values.size()) ? g_preview : chosen;
    if (g_keys.pressed(kRemove)) {                                  // R / pad X: the next portrait, on the cell
        const int next = (shown + 1) % static_cast<int>(f->values.size());
        g_preview = next == chosen ? -1 : next;
        return;
    }
    if (g_keys.pressed(kDecide)) {                                  // Confirm: the portrait shown chosen
        if (g_preview < 0) return;
        Ticks next = t;
        if (grid_set(*s.pack, *s.grid, f->fid, g_preview, next)) {
            logf("ui grid: %s: \"%s\" chosen", name_of_fighter(f->fid).c_str(), f->values[static_cast<size_t>(g_preview)].name.c_str());
            apply(*s.pack, next, "grid");
        }
        g_preview = -1;
        return;
    }
    if (g_keys.pressed(kExtra)) {                                   // Y: that portrait for every character, after the game's box
        g_all_value = shown;
        g_all_name = f->values[static_cast<size_t>(shown)].name;
        if (!g_box.open(trf(W::UiConfirmAll, { g_all_name }), s.pack->name)) {
            logf("ui grid: the game's Yes / No box did not open: nothing given to every character");
            g_all_value = -1;
        }
        g_keys.swallow();
    }
}

}  // namespace

void drop() {
    if (g_held && g_sec >= 0 && SF6CORE_HAS(A(), opt_section_hold)) A()->opt_section_hold(C(), g_sec, 0);
    if (g_held) logf("ui grid: page left (the sections are made again)");
    g_held = false;
    g_unhold = false;
    g_preview = -1;
    if (g_box.is_open()) g_box.drop();
    g_guides.hide(false);
    game_desc(false);
    g_sec = -1;
    g_grid = nullptr;
    g_order.clear();
    g_resets_seen.clear();
    g_need_hide = true;
}

void frame(const std::vector<Section>& secs, TicksFn ticks, ApplyFn apply) {
    ++g_frame;
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, opt_section_state) || !SF6CORE_HAS(a, opt_section_hold) || !SF6CORE_HAS(a, ui_clone_control) ||
        !SF6CORE_HAS(a, ui_set_texture_rect) || !SF6CORE_HAS(a, ui_find_cached))
        return;
    if (secs.empty() && !g_need_hide && !g_up && !g_held && !g_unhold) return;
    try {
        if (g_unhold) {                                             // the screen's input back, once every key is up
            g_keys.read();
            if (!g_keys.any_down()) {
                g_unhold = false;
                if (g_sec >= 0) a->opt_section_hold(C(), g_sec, 0);
            }
        }
        // the game's "Restore Default Settings" confirmed on a grid's section (core: SF6CoreOptSectionState.resets): every character
        // back to the game's own portrait
        for (const Section& x : secs) {
            if (x.group < 0 || !x.pack || !x.grid) continue;
            SF6CoreOptSectionState si{};
            si.struct_size = sizeof si;
            if (a->opt_section_state(C(), x.group, &si) != SF6CORE_OK) continue;
            auto seen = g_resets_seen.find(x.group);
            if (seen != g_resets_seen.end() && si.resets != seen->second) {
                Ticks t = ticks(*x.pack);
                int changed = 0;
                for (const GridFighter& f : x.grid->fighters) if (grid_set(*x.pack, *x.grid, f.fid, 0, t)) ++changed;
                logf("ui grid: \"%s\": the game's Restore Default Settings: every character back to the game's portrait (%d changed)",
                     x.pack->name.c_str(), changed);
                if (changed) apply(*x.pack, t, "Restore Default Settings");
                g_preview = -1;
            }
            g_resets_seen[x.group] = si.resets;
        }
        // the section on screen: the one the left list's cursor is on, or the one the page holds
        const Section* s = nullptr;
        SF6CoreOptSectionState st{};
        for (const Section& x : secs) {
            if (x.group < 0 || !x.pack || !x.grid) continue;
            SF6CoreOptSectionState si{};
            si.struct_size = sizeof si;
            if (a->opt_section_state(C(), x.group, &si) != SF6CORE_OK || !si.shown || !si.control_main) continue;
            if (si.focused || (g_held && x.group == g_sec) || (g_unhold && x.group == g_sec)) { s = &x; st = si; break; }
            if (!st.control_main) st = si;                          // the screen, for hiding
        }
        void* cm = st.control_main;
        if (!s) {
            if (g_held) let_go("the section left");
            if (cm && (g_up || g_need_hide)) { const Copies c = copies(cm); if (c.all()) page_hide(c); g_need_hide = false; }
            else if (!cm) { g_up = false; forget_shown(); }
            g_ok_held = true;
            return;
        }
        if (s->group != g_sec || s->grid != g_grid) {               // another section (or the same made again)
            if (g_held && g_sec >= 0) a->opt_section_hold(C(), g_sec, 0);
            g_held = false;
            g_sec = s->group;
            make_order(*s->grid);
            g_cur = 0;
            g_top = 0;
            g_preview = -1;
            forget_shown();
        }
        static uint32_t s_preloaded_for = 0;
        if (!g_up && s_preloaded_for != static_cast<uint32_t>(s->group) + 1) {   // the chosen portraits, asked once the page shows
            s_preloaded_for = static_cast<uint32_t>(s->group) + 1;
            const Ticks t = ticks(*s->pack);
            for (const GridFighter& f : s->grid->fighters) {
                const int v = grid_value(f, t);
                if (!f.values[static_cast<size_t>(v)].picture.empty()) picture(f.values[static_cast<size_t>(v)].picture, "ui grid");
            }
        }
        fsname::need_guide_hook();
        Copies c = copies(cm);
        if (!c.all()) {
            const int stc = g_copy_status.load(std::memory_order_acquire);
            if (stc != 1 && stc != SF6CORE_OK) { ++g_copy_refused; g_copy_status.store(1); logf("ui grid: copies not made (status %d)", stc); }
            if (g_copy_refused < kCopyRefusedMax && (g_copy_asked == 0 || g_frame - g_copy_asked >= kCopyAskEvery)) {
                g_copy_asked = g_frame;
                fsname::run_in_guide_update(make_copies);
            }
            return;
        }
        static void* s_laid = nullptr;
        if (s_laid != cm) { lay_out(cm, c); s_laid = cm; forget_shown(); g_up = false; }
        if (g_need_hide) { page_hide(c); g_need_hide = false; }
        if (g_held) {
            g_keys.read();
            g_guides.hide(true);
            game_desc(true);
            page_keys(*s, ticks, apply);
        } else if (!g_unhold) {
            g_keys.read();
            const bool ok = g_keys.down(kDecide);
            const bool press = ok && !g_ok_held;
            g_ok_held = ok;
            if (press && st.focused && a->opt_section_hold(C(), s->group, 1) == SF6CORE_OK) {
                g_held = true;
                g_preview = -1;
                guide_place(g_keys_x, g_keys_y, g_desc_x, g_desc_y);
                g_keys.swallow();                                   // the Confirm that opened acts once released
                g_guides.hide(true);
                game_desc(true);
                if (const GridFighter* f = fighter_at(g_cur)) preload(*f);
                logf("ui grid: page opened on \"%s\" (%zu characters)", s->pack->name.c_str(), g_order.size());
            }
        }
        paint(c, ticks(*s->pack), guide_mode_now());
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logf("ui grid: EXCEPTION in a frame"); }
    }
}

}  // namespace slots::uipacks::gridpage
