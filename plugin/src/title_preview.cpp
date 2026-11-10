// SF6 Slots -- the "Title screen" row of Options > Interface > Other Settings (nsfw_option.cpp): its value slides inside its place when
// it is too long (name_scroll.cpp, as a Costume row), and Confirm on the row opens a preview over the whole screen where the OVERDRIVE
// EDITION line of each background is set (Wael, 08/11, after the game's own Battle HUD window: the picture over the screen, the
// settings on it).
//
// Where: the Options screen's GUI (GameObject "OptionSettingMenu", its agent's ControlMain). The row is the unit of the sub list
// (p_GroupScroll_sub_v) whose spin title (p_Spin_spintext_h / e_text_title) reads the row's name: found by that name, then again
// each frame by the unit's own name (R35), its title checked every time. Its focus: its p_Spin_spintext_h plays FOCUS, then SELECT;
// DEFAULT without it (read 08/11).
//
// The preview, open (Confirm on the focused row): the Options screen's input is blocked (the core's ui_set_block_input on its agent)
// and every key is read here, natively: the keyboard keys the game binds to its menu ids (app.InputAssign.Digital.Id, from their
// symbols), the pad's buttons, the arrows. The game's description line and guide bar (GUI "InputGuide", p_InputGuideList_0_fit) are
// hidden (ForceInvisible, put back on closing) and replaced by ours: the line's place and size, and the keys with the game's icons.
//   B (UICancel)               back to the row (the input given back once the key is up)
//   A (UIDecide)               nothing (the confirmation box: yes)
//   LB / RB (UITabL / R)       the previous / next background: the row's value too (core 0.9.8's opt_set_value)
//   X (UIRemove)               the OVERDRIVE EDITION line shown or not on this background
//   directions                 move the line, while it is shown (a press: 0.05 %; held: faster)
//   L3 / R3 (UISort / Toggle)  the line centred horizontally / vertically
//   LT / RT (UIChildTabL / R)  the line smaller / larger
//   Y (UIOptionExtraButton)    this background's setting for every background, after a confirmation box
// "Random" is not a background: nothing to set there (each background drawn keeps its own setting). Its picture is drawn live: the
// backgrounds it draws from, each its thumbnail, laid out as the loader's mosaic (same rule). R / pad X (UIRemove) on it opens its
// list (Wael 08/11), as Battle Settings' stage grid: the mosaic darkened, a panel with the focused background's picture and name,
// a 3-column grid of the backgrounds with their check box (the loader's check_off / check_on textures) and a scroll bar. A (Confirm)
// ticks or unticks, Y ticks every one (all ticked: only the focused one stays), B / Esc goes back to the mosaic; at least one stays
// ticked. Every change: settings.json's title_random_pool and the loader at once (nsfwopt::set_random_pool): the next draw, at a
// return to the title or the next cycle on it, takes from it. A background never set has no
// line (Wael 08/11), except the game's own: shown at kGameDefault until set; X gives it the line at its default place (the middle of the screen's width, 75.19 % of its height, 7.04 %).
// The settings: reframework/data/SF6_TitleSlots_Data/overdrive.json, per background key ("game": the game's own title, a pack's key):
// show, x, y (the line's middle, % of the screen), size (its font height, % of the screen's height). Written when the background
// changes, on "apply to all" and on closing; read by the core's title.cpp on the title screen.
//
// The pictures: one per background in the stage pak (the loader's add_title_previews, 1920x1080, the registry's "preview"; Random's
// is the mosaic of them), loaded with ui_texture_load once the row is seen, kept for the session.
//
// The copies (R19: made inside the guide list's update, fs_name.cpp's hook, from this row's own controls): under ControlMain, its last
// children, drawn over the whole menu by their priority: a black backdrop, the picture, the OVERDRIVE EDITION line (styled as the
// core's: its font slot, colour, raster), a dark band at the top with the background's name, a dark band at the bottom with the line's
// numbers and the keys, the note when there is no picture, the confirmation box. Hidden and ForceInvisible until shown, found again by
// name. Every write from LateUpdateBehavior, through the core (R1).
//
// The machinery its page shares with Interface Packs' (ui_packs.cpp): keys, icons, guide bar, the parts copied, their style, the
// pictures and the game's box are options_page.cpp's.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "options_page.hpp"
#include "name_scroll.hpp"
#include "codecheck_rt.hpp"
#include "words.hpp"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace slots::titleprev {
namespace {

using optpage::A; using optpage::C;
using optpage::floats_of; using optpage::shown; using optpage::drawn; using optpage::message; using optpage::name_of; using optpage::options_agent;
using optpage::g_cp_center; using optpage::g_cp_lefttop; using optpage::g_cp_leftcenter; using optpage::g_pa_right; using optpage::read_enums;
using optpage::set_enum; using optpage::set_size_value; using optpage::s9_look; using optpage::show; using optpage::place;
using optpage::style_text; using optpage::style_rect; using optpage::guide_key_text; using optpage::guide_mode_now;
using optpage::kScreenW; using optpage::kScreenH; using optpage::kPX;
using optpage::kLPanelX; using optpage::kLPanelY; using optpage::kLPanelW; using optpage::kLPanelH;
using optpage::kLBanCX; using optpage::kLBanCY; using optpage::kLBanW; using optpage::kLBanH;
using optpage::kLNameCX; using optpage::kLNameCY; using optpage::kLNameW; using optpage::kLNameH; using optpage::kLNameFont;
using optpage::kLLineX; using optpage::kLLineY; using optpage::kLLineW; using optpage::kLLineH; using optpage::kLStripV0; using optpage::kLStripV1;
using optpage::kColBg; using optpage::kColBand; using optpage::kColDlg; using optpage::kColText; using optpage::kColNote;
using optpage::kColLDim; using optpage::kColLPanelBg; using optpage::kColLPanelTint; using optpage::kColLOutline;
using optpage::kColLFocus; using optpage::kColLGlow; using optpage::kColLCellDim; using optpage::kColLTrack; using optpage::kColLName; using optpage::kColWhite;
using optpage::kColLLine;

constexpr const char* kSubList = "p_GroupScroll_sub_v";
constexpr const char* kSpin = "p_Spin_spintext_h";
constexpr const char* kCodecheckCache = "reframework/data/SF6_CostumeSlots_data/codecheck_title.txt";
constexpr const char* kSettingsFile = "reframework/data/SF6_TitleSlots_Data/overdrive.json";
// the copies (not "p_...": never taken for a UI part, R20)
constexpr const char* kBg = "e_slots_title_bg";
constexpr const char* kPic = "e_slots_title_pic";
constexpr const char* kLine = "e_slots_title_line";
constexpr const char* kPress = "e_slots_title_press";
constexpr const char* kBand = "e_slots_title_band";
constexpr const char* kName = "e_slots_title_name";
constexpr const char* kNote = "e_slots_title_note";
constexpr const char* kBand2 = "e_slots_title_band2";
constexpr const char* kInfo = "e_slots_title_info";
constexpr const char* kKeys = "e_slots_title_keys";
constexpr const char* kDlgBg = "e_slots_title_dlgbg";
constexpr const char* kDlgText = "e_slots_title_dlgtext";
constexpr const char* kDlgKeys = "e_slots_title_dlgkeys";
// Random's list and live mosaic
constexpr const char* kLDim = "e_slots_title_ldim";
constexpr const char* kLPanel = "e_slots_title_lpanel";       // the page's panel: a cell shell holding the three below
constexpr const char* kLPMask = "e_slots_title_lpmask";       // in its c_bg: the fading mask (the game's c_bgContents/e_rect_mask)
constexpr const char* kLPBg = "e_slots_title_lpbg";           //   e_scale9grid_bg
constexpr const char* kLPTint = "e_slots_title_lptint";       //   e_scale9grid_focus (the page has the input)
constexpr const char* kLRandQ = "e_slots_title_lrq";          // Random's cell: its "?"
constexpr const char* kLBanQ = "e_slots_title_lbq";           // Random's banner: its "?"
constexpr const char* kPKeys = "e_slots_title_pkeys";         // the page's keys, in the guide bar's style
constexpr const char* kPDesc = "e_slots_title_pdesc";         // the page's description line, in the guide bar's style
constexpr const char* kGlobe = "Product/GUI/data/Area_Image/random/tex_random_IM.tex";   // the stage grid's Random picture
constexpr const char* kLBanner = "e_slots_title_lban";
constexpr const char* kLBanName = "e_slots_title_lname";
constexpr const char* kLHalo = "e_slots_title_lout2";   // the outline's second line, one pixel out: thicker (Wael 08/11)
constexpr const char* kLOutline = "e_slots_title_lout";
constexpr const char* kLTrack = "e_slots_title_ltrack";
constexpr const char* kLThumb = "e_slots_title_lthumb";
constexpr const char* kLGlow = "e_slots_title_lglow";         // the focus: the stage grid's glow (c_outline / c_inner / e_scale9grid_glow)
constexpr const char* kLOut3 = "e_slots_title_lout3";         // the focus band's lines 3 - 5 (with kLOutline and kLHalo)
constexpr const char* kLOut4 = "e_slots_title_lout4";
constexpr const char* kLOut5 = "e_slots_title_lout5";
constexpr const char* kLLine = "e_slots_title_lline";
constexpr int kMosaicMax = 24;              // the live mosaic's cells (more backgrounds: the loader's mosaic picture)
constexpr int kLCols = 3, kLRows = 6, kLSlots = kLCols * kLRows;
constexpr uint32_t kIdleEvery = 15;         // frames between looks while the row is not on screen
constexpr uint32_t kFindEvery = 30;         // frames between searches of the row while it is not found
constexpr uint32_t kCopyAskEvery = 120;     // frames between asks for the copies
constexpr int kCopyRefusedMax = 3;
// The layout, in screen units (1920 x 1080)
constexpr float kBandH = 90, kNameCY = 45, kNameW = 1700, kTextH = 60, kNameFont = 34, kNoteFont = 30;
constexpr float kBand2Y = 958, kInfoCY = 985, kKeysCY = 1040, kInfoFont = 28, kKeysFont = 26, kBottomW = 1840;
constexpr float kDlgW = 1100, kDlgH = 240, kDlgTextCY = 505, kDlgKeysCY = 580, kDlgTextW = 1040;
// Random's list, laid out as Battle Settings > Other > Stage Settings (GUI MatchingSetting, c_StageSelect / p_StageSelectSimple_0_h,
// read 08/11): the panel, the preview banner (776 x 194) with the name at its bottom right, 3 columns of 252 x 63 cells (pitch 262 x 73,
// 6 rows shown), the scroll bar on the right. A cell: its shadow (-8, -6, 271 x 82), the picture under the rounded mask, the thin
// outline, the check box (box 30 at +17.5, tick 42 at +10.5, centres). Focused: the outline gives way to the blue glow.
// On the Options screen the page sits in the right side's area (c_bg_right/e_bg: 912, 145, 910 x 833): the stage page's places
// moved by kPX = 912 - 734 (its page starts at 734 on Battle Settings): options_page.hpp's panel, banner, name (the game's 361
// wide at 1356: widened) and left bar (the line turned 90 degrees), and here:
constexpr float kLCellX = 760 + kPX, kLCellY = 404, kLCellW = 252, kLCellH = 63, kLPitchX = 262, kLPitchY = 73;
constexpr float kLBoxD = 17.5f, kLBox = 30, kLTickD = 10.5f, kLTick = 42;
constexpr float kLBarX = 1546 + kPX, kLBarY = 404, kLBarW = 6, kLBarH = 430;
constexpr float kMosaicGap = 3;            // the loader's TITLE_MOSAIC_GAP
// The OVERDRIVE EDITION line, as the core's title.cpp draws it
constexpr const char* kLineText = "OVERDRIVE EDITION";
constexpr int64_t kLineFontSlot = 6;
constexpr uint32_t kLineRgba = 0xFFEBEBECu;
constexpr float kLineRegionW = 1600, kLineRegionPerFont = 130.0f / 76.0f, kLineRasterPerFont = 2.0f;
constexpr float kDefX = 50.0f, kDefY = 812.0f / 1080.0f * 100.0f, kDefSize = 76.0f / 1080.0f * 100.0f;   // title.cpp's defaults
constexpr float kSizeMin = 8.0f / 1080.0f * 100.0f, kSizeMax = 150.0f / 1080.0f * 100.0f;              // its font limits
// PRESS ANY BUTTON as the title screen shows it (GUI "Title", c_main/e_text_botton, read 08/11): shown with the line, so that a line
// over it shows here. Its text: the game's message, read from the title screen each session (the game shows it at every launch).
constexpr float kPressX = 960, kPressY = 930, kPressFont = 40, kPressRaster = 80, kPressW = 1000, kPressH = 64;
constexpr int64_t kPressFontSlot = 2;
constexpr uint32_t kPressRgba = 0xB4FFFFFFu;
constexpr float kMoveStep = 0.05f, kMoveMax = 0.6f, kSizeStep = 0.1f;
// The colours: options_page.hpp's (via.Color, 0xAABBGGRR: replay_tab.cpp's blue bar 0xFFD76407)
// Over every sibling (theirs: 0), each layer over the one before: siblings of one priority are drawn in their order, and the copies
// come in the reverse of their making (read 08/11)
constexpr int64_t kPrioBg = 60000, kPrioPic = 60010, kPrioMosaic = 60011, kPrioLine = 60012, kPrioBand = 60015, kPrioText = 60020,
                  kPrioLDim = 60022, kPrioLPanel = 60023, kPrioLCursor = 60024, kPrioLCell = 60025, kPrioLOver = 60026, kPrioLTop = 60027,
                  kPrioDlg = 60030, kPrioDlgText = 60040;

uint32_t g_frame = 0;
bool g_seen = false;                        // the row was on screen at the last look
bool g_value_driven = false;                // the row's value text is in the scroller's hands (name_scroll.cpp)
std::string g_unit;                         // the row's unit, as last found
uint32_t g_find_at = 0;
// copy -> the resource it shows (set once each: a copy keeps the last texture given); copy -> shown, for show_if (cleared whenever
// show() may have been called on them). The pictures: options_page.cpp's, loaded once for the session.
optpage::Painter g_paint{ "title preview" };
std::atomic<bool> g_open{ false };          // the preview is up (read by the cancel hook)
std::atomic<bool> g_cancel{ false };        // the cancel hook took a cancel: close
optpage::GameBox g_box;                     // the game's Yes / No box (its `up` read by the cancel hook)
bool g_ok_held = true;                      // Confirm was down at the last look (true at start: a key held acts once released)
bool g_unblock = false;                     // the Options screen's input to give back once no key is down
optpage::Guides g_guides;                   // the guide bars we hid
uint8_t g_press_guid[16] = {};              // PRESS ANY BUTTON's message, read on the title screen
bool g_press_known = false;
uint32_t g_press_look = 0;
std::atomic<int> g_copy_status{ 1 };        // the last copy job: SF6CORE_OK, an error, 1 none yet
uint32_t g_copy_asked = 0;
int g_copy_refused = 0;
bool g_copy_logged = false;

// ---- the settings per background ----
struct Line { bool show = false; float x = kDefX, y = kDefY, size = kDefSize; };
// The game's own title ("game") never set (a first install): the line shown at Wael's place (08/11); the core's title.cpp the same
const Line kGameDefault = { true, 50.00f, 62.49f, 11.44f };
std::map<std::string, Line> g_lines;        // overdrive.json, read when the preview opens
bool g_dirty = false;
int g_refresh_row = 0;                      // the row's value just set by LB / RB (1-based): its display to redraw this frame
bool g_dialog = false;                      // the confirmation box of "apply to all" is up
uint32_t g_toast_until = 0;                 // "applied to every background" shown until this frame
// Random's list
// The page (Options > Interface > Title Screen Selection)
bool g_page = false;                        // the page has the input (the core's section held)
bool g_ticks = false;                       // its random settings (ticks) instead of the choice
bool g_unhold = false;                      // the section's input to give back once no key is down
int g_lcur = 0;                             // the focused entry (0: the game's own, then the packs, then Random in the choice)
int g_ltop = 0;                             // the first row of the grid shown
int g_edit = 0;                             // the editor's background (0-based, the row's values)
float g_keys_x = 60, g_keys_y = 1040;       // the guide bar's first key (our keys line starts there)
float g_desc_x = 60, g_desc_y = 1003;       // the guide's description line (ours there)
std::set<std::string> g_sel;                // the backgrounds ticked (keys)
uint32_t g_one_until = 0;                   // "at least one stays" shown until this frame

// The backgrounds of the list: the row's values but Random (the last)
int bg_count(const nsfwopt::TitleRow& row) { return row.keys.size() > 1 ? static_cast<int>(row.keys.size()) - 1 : 0; }
// settings.json's selection; never set: every pack (not the game's own), as the loader's random_selection
void load_selection(const nsfwopt::TitleRow& row) {
    g_sel.clear();
    std::vector<std::string> keys;
    const bool has = costumes::random_pool(keys);
    const int n = bg_count(row);
    for (int i = 0; i < n; ++i) {
        const std::string& k = row.keys[static_cast<size_t>(i)];
        const bool on = has ? std::find(keys.begin(), keys.end(), k) != keys.end() : k != "game";
        if (on) g_sel.insert(k);
    }
    if (g_sel.empty()) for (int i = 0; i < n; ++i) if (row.keys[static_cast<size_t>(i)] != "game") g_sel.insert(row.keys[static_cast<size_t>(i)]);
    if (g_sel.empty() && n > 0) g_sel.insert(row.keys[0]);
}
void save_selection(const nsfwopt::TitleRow& row) {
    std::vector<std::string> keys;
    for (int i = 0, n = bg_count(row); i < n; ++i) if (g_sel.count(row.keys[static_cast<size_t>(i)])) keys.push_back(row.keys[static_cast<size_t>(i)]);
    nsfwopt::set_random_pool(keys);
}

void load_lines() {
    g_lines.clear();
    g_lines["game"] = kGameDefault;                          // replaced below when overdrive.json has its own
    std::string text = read_file(kSettingsFile);
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    Json j;
    if (text.empty() || !json_parse(text, j)) return;
    const Json* bgs = j.get("backgrounds");
    if (!bgs || bgs->kind != Json::Object) return;
    for (const auto& [key, v] : bgs->obj) {
        if (v.kind != Json::Object) continue;
        Line l;
        const Json* s = v.get("show");
        l.show = s && (s->kind == Json::Bool ? s->b : s->kind == Json::Number && s->num != 0);
        if (const Json* x = v.get("x"); x && x->kind == Json::Number) l.x = static_cast<float>(x->num);
        if (const Json* y = v.get("y"); y && y->kind == Json::Number) l.y = static_cast<float>(y->num);
        if (const Json* z = v.get("size"); z && z->kind == Json::Number) l.size = static_cast<float>(z->num);
        g_lines[key] = l;
    }
}
void save_lines(const char* why) {
    if (!g_dirty) return;
    g_dirty = false;
    std::string out = "{\n  \"version\": 1,\n  \"backgrounds\": {";
    bool first = true;
    for (const auto& [key, l] : g_lines) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "{ \"show\": %s, \"x\": %.2f, \"y\": %.2f, \"size\": %.2f }", l.show ? "true" : "false", l.x, l.y, l.size);
        out += (first ? "\n    \"" : ",\n    \"") + json_escape(key) + "\": " + buf;
        first = false;
    }
    out += first ? "}\n}\n" : "\n  }\n}\n";
    CreateDirectoryA("reframework/data/SF6_TitleSlots_Data", nullptr);
    if (write_file(kSettingsFile, out)) logf("title preview: overdrive.json written (%zu background(s), %s)", g_lines.size(), why);
    else logf("title preview: overdrive.json NOT written (%s)", why);
}
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
float round2(float v) { return std::round(v * 100.0f) / 100.0f; }

// ---- keys: the game's own bindings (options_page.cpp's reader, this page's own held counts) ----
// The game's menu buttons as this page uses them (the order of optpage::Key)
enum Act { kBack, kDecide, kBgPrev, kBgNext, kSizeDown, kSizeUp, kTitle, kAll, kCenterH, kCenterV, kUp, kDown, kLeft, kRight, kActs };
static_assert(+kActs == +optpage::kKeyCount && +kBgPrev == +optpage::kTabL && +kTitle == +optpage::kRemove && +kAll == +optpage::kExtra &&
              +kCenterH == +optpage::kSort && +kUp == +optpage::kUp && +kRight == +optpage::kRight, "the page's keys are optpage::Key's");
optpage::Keys g_keys;
void read_buttons() { g_keys.read(); }
bool pressed(Act a) { return g_keys.pressed(static_cast<optpage::Key>(a)); }
bool any_down() { return g_keys.any_down(); }
// A press, then again while held: after `wait` frames, every `every`
bool repeat(Act a, uint32_t wait, uint32_t every) { return g_keys.repeat(static_cast<optpage::Key>(a), wait, every); }
// How far a held direction moves this frame (in %): a step at the press, faster the longer it is held
float move_of(Act a) {
    const uint32_t h = g_keys.held(static_cast<optpage::Key>(a));
    if (h == 0) return 0;
    if (h == 1) return kMoveStep;
    if (h < 12) return 0;
    return std::fmin(kMoveStep + 0.02f * static_cast<float>(h - 12), kMoveMax);
}
// The icons of a menu id for the device in use, asked to the game once a second at most
std::string icons(int id, int mode) { return optpage::icons(id, mode); }

bool confirm_down() {                                              // opening: the game's Confirm (keys or pad A), the input not blocked yet
    read_buttons();
    return g_keys.down(optpage::kDecide);
}

// ---- the game's controls (options_page.cpp's) ----
std::string play_state(void* c) { return c ? str(call_obj(static_cast<Obj*>(c), "get_PlayState")) : std::string(); }

// The row's unit under the screen's main control: by its last name (checked), else searched among the sub list's units every kFindEvery
// frames. `title`: the row's name.
void* find_unit(void* cm, const std::string& title) {
    const SF6CoreAPI* a = A();
    void* sub = a->ui_find_cached(cm, kSubList);
    if (!sub) return nullptr;
    auto is_ours = [&](void* unit) {
        void* spin = unit ? a->ui_find_child(unit, kSpin, 1) : nullptr;
        void* t = spin ? a->ui_find_child(spin, "e_text_title", 1) : nullptr;
        return t && message(t) == title;
    };
    if (!g_unit.empty()) {
        void* u = a->ui_find_child(sub, g_unit.c_str(), 3);
        if (is_ours(u)) return u;
    }
    if (g_frame - g_find_at < kFindEvery && g_find_at) return nullptr;
    g_find_at = g_frame;
    void* scroll = a->ui_find_child(sub, "c_scroll", 1);
    int guard = 0;
    for (Obj* u = scroll ? call_obj(static_cast<Obj*>(scroll), "get_Child") : nullptr; u && guard < 80; u = call_obj(u, "get_Next"), ++guard) {
        if (!is_ours(u)) continue;
        g_unit = name_of(u);
        logf("title preview: the Title Screen row is %s", g_unit.c_str());
        return u;
    }
    return nullptr;
}

// The row's part (app.UIPartsOptionUnit) on the Options screen: the one whose control is `unit` (found this frame), among the parts of
// the screen's parts manager and their groups' children (read 08/11: _List[1] / UIPartsGroupScroll / UIPartsOptionUnit)
Obj* find_part(Obj* part, void* unit, int depth) {
    if (!part || depth > 4) return nullptr;
    const std::string tn = type_name(part);
    if (tn == "app.UIPartsOptionUnit") return call_obj(part, "get_Control") == unit ? part : nullptr;
    if (tn.find("Group") == std::string::npos) return nullptr;      // only groups hold rows
    const Ret n = call(part, "GetChildCount");
    if (n.exception_thrown) return nullptr;
    for (int i = 0; i < static_cast<int32_t>(n.dword) && i < 200; ++i)
        if (Obj* f = find_part(call_obj(part, "GetChild", { arg_i32(i) }), unit, depth + 1)) return f;
    return nullptr;
}
// The value set from here (core opt_set_value) is the unit's; the row shows it once its text list is moved and the row redrawn
void refresh_row(Obj* agent, void* unit, int value) {
    Obj* pm = agent ? get_obj(agent, "_PartsManager") : nullptr;
    Obj* list = pm ? get_obj(pm, "_List") : nullptr;
    Obj* part = nullptr;
    for (int i = 0, n = list_count(list); i < n && !part; ++i) part = find_part(list_item(list, i), unit, 0);
    if (!part) { logf("title preview: the row's part not found: its value is saved, the row shows it next time"); return; }
    if (Obj* tl = get_obj(part, "SpinTextList")) {
        SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = value - 1;
        A()->obj_invoke(C(), tl, "SetFocusIndex(System.Int32)", &a, 1, nullptr);
    }
    A()->obj_invoke(C(), part, "UpdateDisp", nullptr, 0, nullptr);
}

// The game's description line and guide bar: hidden while the preview is up, put back after
void hide_guides(bool hide) { g_guides.hide(hide); }

struct CopyDef { std::string name; int tmpl; };                  // tmpl: 0 a Rect, 1 a Texture, 2 a Text, 3 a Text with icons
std::vector<CopyDef> g_defs;                // every copy, in making order
std::map<std::string, size_t> g_def_at;
size_t g_m0 = 0, g_c0 = 0, g_b0 = 0, g_k0 = 0;   // the first mosaic cell, list cell, check box, tick
// The templates (make_copies): optpage::Tmpl's numbers, 0 a Rect, 1 a Texture, 2 a Text, 3 a Text with icons (the row's own); 4 the
// row's cell shell (c_bg_s: shadow, rounded mask, outline), 5 its rounded mask, 6 its outline (scale9, the game's hub parts); 7 / 8 the
// Options radio list's check box and tick (e_texture_box / e_texture_check: the game's uvs_checkbutton); 9 / 10 the sub list's scroll
// bar (BG, Bar). One missing: its fallback (options_page.cpp): the list then draws with plainer parts.
// 11 the guide bar's own key text (InputGuide, e_text_0_d_*: the page's keys line in its style)
bool g_game_checks = false;                 // the check boxes are the game's (else the loader's check_off / check_on pictures)
std::string numbered(const char* base, int i) { char b[64]; std::snprintf(b, sizeof b, "%s%02d", base, i); return b; }
const std::vector<CopyDef>& defs() {
    if (!g_defs.empty()) return g_defs;
    const CopyDef base[] = { { kBg, 0 }, { kPic, 1 }, { kLine, 2 }, { kPress, 2 }, { kBand, 0 }, { kName, 2 }, { kNote, 2 }, { kBand2, 0 }, { kInfo, 2 },
                             { kKeys, 3 }, { kDlgBg, 0 }, { kDlgText, 2 }, { kDlgKeys, 3 }, { kLPanel, 4 },
                             { kLBanner, 1 }, { kLBanName, 2 }, { kLHalo, 6 }, { kLOutline, 6 }, { kLTrack, 9 }, { kLThumb, 10 }, { kLLine, 5 },
                             { kLRandQ, 2 }, { kLBanQ, 2 }, { kPKeys, 11 }, { kPDesc, 11 }, { kLGlow, 5 }, { kLOut3, 6 }, { kLOut4, 6 }, { kLOut5, 6 } };
    for (const CopyDef& d : base) g_defs.push_back(d);
    g_m0 = g_defs.size();
    for (int i = 0; i < kMosaicMax; ++i) g_defs.push_back({ numbered("e_slots_title_m", i), 1 });
    g_c0 = g_defs.size();
    for (int i = 0; i < kLSlots; ++i) g_defs.push_back({ numbered("e_slots_title_c", i), 4 });
    g_b0 = g_defs.size();
    for (int i = 0; i < kLSlots; ++i) g_defs.push_back({ numbered("e_slots_title_b", i), 7 });
    g_k0 = g_defs.size();
    for (int i = 0; i < kLSlots; ++i) g_defs.push_back({ numbered("e_slots_title_k", i), 8 });
    for (size_t i = 0; i < g_defs.size(); ++i) g_def_at[g_defs[i].name] = i;
    return g_defs;
}

// Inside the guide list's update (R19): the copies under the screen's main control, from the Options screen's own controls
void make_copies() {
    const SF6CoreAPI* a = A();
    SF6CoreClient* cl = C();
    int status = -3000;
    try {
        Obj* agent = options_agent();
        Obj* cm = agent ? call_obj(agent, "get_ControlMain") : nullptr;
        const optpage::Templates tm = optpage::templates(cm, g_unit);
        if (cm && tm.basic()) {
            status = SF6CORE_OK;
            g_game_checks = tm.game_checks;
            static bool logged = false;
            if (!logged) {
                logged = true;
                logf("title preview: the list's game parts: cell shell %s, check box %s, scroll bar %s", tm.t[optpage::kTShell] ? "yes" : "NO (plain pictures)",
                     g_game_checks ? "yes" : "NO (the loader's pictures)", tm.t[optpage::kTBarBg] && tm.t[optpage::kTBar] ? "yes" : "NO (plain rectangles)");
            }
            for (const CopyDef& w : defs()) {
                void* c = a->ui_find_child(cm, w.name.c_str(), 0);
                if (!c) {
                    const SF6CoreStatus st = a->ui_clone_control(cl, tm.for_copy(w.tmpl, optpage::kTTexture), cm, w.name.c_str(), &c);
                    if (st != SF6CORE_OK || !c) { status = st != SF6CORE_OK ? st : -3001; break; }
                    a->ui_set_force_invisible(cl, c, 1);
                    a->ui_set_visible(cl, c, 0);
                }
                // the page's panel: in its shell's c_bg, the fading mask over the panel's two scale9 (made last: first among them)
                if (w.name == kLPanel && c) {
                    optpage::make_panel_parts(c, tm, kLPMask, kLPBg, kLPTint);
                    continue;
                }
                // a cell shell gets its picture inside its rounded mask (c_bg), as the game's cell (c_bg / e_texture_thumb)
                if (w.tmpl == 4 && c) {
                    void* inner_parent = a->ui_find_child(c, "c_bg", 1);
                    const std::string pic = w.name + "_pic";
                    if (inner_parent && !a->ui_find_child(inner_parent, pic.c_str(), 0)) {
                        void* t = nullptr;
                        if (a->ui_clone_control(cl, tm.t[optpage::kTTexture], inner_parent, pic.c_str(), &t) == SF6CORE_OK && t) a->ui_set_visible(cl, t, 0);
                    }
                }
            }
        }
    } catch (...) { status = -2000; }
    g_copy_status.store(status, std::memory_order_release);
}

struct Copies {
    std::vector<void*> c;
    void* get(const char* name) const { auto it = g_def_at.find(name); return it != g_def_at.end() && it->second < c.size() ? c[it->second] : nullptr; }
    void* mosaic(int i) const { return g_m0 + i < c.size() ? c[g_m0 + i] : nullptr; }
    void* cell(int i) const { return g_c0 + i < c.size() ? c[g_c0 + i] : nullptr; }
    void* box(int i) const { return g_b0 + i < c.size() ? c[g_b0 + i] : nullptr; }
    void* check(int i) const { return g_k0 + i < c.size() ? c[g_k0 + i] : nullptr; }
    std::vector<void*> pic;                 // per list cell: its picture (inside a cell shell), or the cell itself (a plain picture)
    void* pmask = nullptr; void* pbg = nullptr; void* ptint = nullptr;   // the page panel's parts (in its c_bg)
    bool all() const { if (c.size() != g_defs.size()) return false; for (void* x : c) if (!x) return false; return true; }
};
// The copies, found once per screen (75 of them: one walk per 32 names, core ui_find_many), kept while the screen's main control and
// the first copy are the ones found (one cached look a frame)
Copies g_cache;
void* g_cache_cm = nullptr;
uint32_t g_cache_at = 0;
Copies copies(void* cm) {
    const SF6CoreAPI* a = A();
    const auto& d = defs();
    if (cm == g_cache_cm && g_cache.all() && a->ui_find_cached(cm, kBg) == g_cache.c[0]) return g_cache;
    if (cm == g_cache_cm && !g_cache.c.empty() && !g_cache.all() && g_frame - g_cache_at < 30) return g_cache;   // not all made yet: looked again soon
    if (cm != g_cache_cm) g_paint.forget();
    g_cache_cm = cm;
    g_cache_at = g_frame;
    static std::vector<std::string> names;
    if (names.size() != d.size()) { names.clear(); for (const CopyDef& w : d) names.push_back(w.name); }
    optpage::find_all(cm, names, g_cache.c);
    g_cache.pic.assign(kLSlots, nullptr);
    for (int i = 0; i < kLSlots; ++i) {
        void* cell = g_cache.cell(i);
        void* bg = cell ? a->ui_find_child(cell, "c_bg", 1) : nullptr;
        g_cache.pic[static_cast<size_t>(i)] = bg ? a->ui_find_child(bg, (d[g_c0 + i].name + "_pic").c_str(), 0) : cell;
    }
    void* panel = g_cache.get(kLPanel);
    void* pbg = panel ? a->ui_find_child(panel, "c_bg", 1) : nullptr;
    g_cache.pmask = pbg ? a->ui_find_child(pbg, kLPMask, 0) : nullptr;
    g_cache.pbg = pbg ? a->ui_find_child(pbg, kLPBg, 0) : nullptr;
    g_cache.ptint = pbg ? a->ui_find_child(pbg, kLPTint, 0) : nullptr;
    return g_cache;
}
// Once per opening: every copy's style and place
void lay_out(void* cm, const Copies& c) {
    optpage::set_origin(cm);
    const SF6CoreAPI* a = A();
    SF6CoreClient* cl = C();
    style_rect(c.get(kBg), -20, -20, kScreenW + 40, kScreenH + 40, kColBg, kPrioBg);
    void* pic = c.get(kPic);
    set_enum(pic, "set_ControlPoint(via.gui.ControlPoint)", g_cp_center);
    place(pic, kScreenW / 2, kScreenH / 2);
    a->ui_set_size(cl, pic, kScreenW, kScreenH);
    a->ui_set_color(cl, pic, 0xFFFFFFFFu);
    set_enum(pic, "set_Priority(System.UInt16)", kPrioPic);
    void* line = c.get(kLine);
    set_enum(line, "set_FontSlot(via.gui.FontSlot)", kLineFontSlot);
    set_enum(line, "set_ControlPoint(via.gui.ControlPoint)", g_cp_center);
    set_enum(line, "set_PageAlignment(via.gui.PageAlignment)", g_cp_center);
    set_enum(line, "set_AutoRegionFit(via.gui.RegionFitType)", 0);
    set_enum(line, "set_AutoWrap(System.Boolean)", 0);
    set_enum(line, "set_Priority(System.UInt16)", kPrioLine);
    a->ui_set_text(cl, line, kLineText);
    a->ui_set_color(cl, line, kLineRgba);
    void* press = c.get(kPress);
    set_enum(press, "set_FontSlot(via.gui.FontSlot)", kPressFontSlot);
    set_enum(press, "set_ControlPoint(via.gui.ControlPoint)", g_cp_center);
    set_enum(press, "set_PageAlignment(via.gui.PageAlignment)", g_cp_center);
    set_enum(press, "set_AutoRegionFit(via.gui.RegionFitType)", 0);
    set_enum(press, "set_AutoWrap(System.Boolean)", 0);
    set_enum(press, "set_Priority(System.UInt16)", kPrioLine);
    set_size_value(press, "set_RasterSize(via.Size)", kPressRaster, kPressRaster);
    a->ui_set_region_size(cl, press, kPressW, kPressH);
    a->ui_set_font_size(cl, press, kPressFont, kPressFont);
    a->ui_set_color(cl, press, kPressRgba);
    place(press, kPressX, kPressY);
    if (g_press_known && SF6CORE_HAS(a, ui_set_message_id)) a->ui_set_message_id(cl, press, g_press_guid);
    else a->ui_set_text(cl, press, "PRESS ANY BUTTON");
    style_rect(c.get(kBand), -20, -20, kScreenW + 40, kBandH + 20, kColBand, kPrioBand);
    style_rect(c.get(kBand2), -20, kBand2Y, kScreenW + 40, kScreenH - kBand2Y + 20, kColBand, kPrioBand);
    style_text(c.get(kName), kNameFont, kNameW, kTextH, kPrioText);
    place(c.get(kName), kScreenW / 2, kNameCY);
    style_text(c.get(kNote), kNoteFont, kNameW, kTextH, kPrioText);
    place(c.get(kNote), kScreenW / 2, kScreenH / 2);
    style_text(c.get(kInfo), kInfoFont, kBottomW, 50, kPrioText);
    place(c.get(kInfo), kScreenW / 2, kInfoCY);
    style_text(c.get(kKeys), kKeysFont, kBottomW, 50, kPrioText);
    place(c.get(kKeys), kScreenW / 2, kKeysCY);
    style_rect(c.get(kDlgBg), kScreenW / 2, kScreenH / 2, kDlgW, kDlgH, kColDlg, kPrioDlg, true);
    style_text(c.get(kDlgText), kInfoFont + 2, kDlgTextW, kTextH, kPrioDlgText);
    place(c.get(kDlgText), kScreenW / 2, kDlgTextCY);
    style_text(c.get(kDlgKeys), kKeysFont, kDlgTextW, 50, kPrioDlgText);
    place(c.get(kDlgKeys), kScreenW / 2, kDlgKeysCY);
    a->ui_set_color(cl, c.get(kName), kColText);
    a->ui_set_color(cl, c.get(kNote), kColNote);
    a->ui_set_color(cl, c.get(kInfo), kColText);
    a->ui_set_color(cl, c.get(kKeys), kColText);
    a->ui_set_color(cl, c.get(kDlgText), kColText);
    a->ui_set_color(cl, c.get(kDlgKeys), kColText);
    a->ui_set_text(cl, c.get(kNote), words::tr(words::W::TitleNoPicture));
    // The page: the stage page's places (moved to the Options screen's right side)
    // a cell shell: its own parts hidden, ours in its c_bg
    optpage::lay_out_panel(c.get(kLPanel), c.pmask, c.pbg, c.ptint, kLPanelX, kLPanelY, kLPanelW, kLPanelH, kColLPanelBg, kColLPanelTint, kPrioLPanel);
    for (const auto& [nm, font, x, y] : { std::tuple<const char*, float, float, float>{ kLRandQ, 50.0f, 0.0f, 0.0f },
                                          std::tuple<const char*, float, float, float>{ kLBanQ, 120.0f, kLBanCX, kLBanCY } }) {
        void* q = c.get(nm);                                         // Random's "?" (e_text_random: slot 6, its colour)
        style_text(q, font, font * 2, font * 1.6f, kPrioLTop);
        set_enum(q, "set_FontSlot(via.gui.FontSlot)", kLineFontSlot);
        a->ui_set_color(cl, q, kColLName);
        a->ui_set_text(cl, q, "?");
        place(q, x, y);
    }
    for (void* k : { c.get(kPKeys), c.get(kPDesc) }) {               // the keys and description lines: the guide bar's text, left aligned
        if (!k) continue;
        set_enum(k, "set_ControlPoint(via.gui.ControlPoint)", g_cp_leftcenter);
        set_enum(k, "set_PageAlignment(via.gui.PageAlignment)", g_cp_leftcenter);
        set_enum(k, "set_AutoRegionFit(via.gui.RegionFitType)", 0);
        set_enum(k, "set_AutoWrap(System.Boolean)", 0);
        set_enum(k, "set_Priority(System.UInt16)", kPrioLTop);
        set_enum(k, "set_MaskType(via.gui.MaskType)", 1);
        A()->ui_set_region_size(cl, k, kBottomW, 60);
    }
    style_rect(c.get(kLBanner), kLBanCX, kLBanCY, kLBanW, kLBanH, kColWhite, kPrioLCell, true);
    // the stage name's (e_text_stage: slot 6, glow 20 x 20, strength 3), the whole banner's width, ending at its right as the game's
    optpage::style_banner_name(c.get(kLBanName), kLNameFont, kLNameW, kLNameH, kLNameCX, kLNameCY, kColLName, kLineFontSlot, kPrioLTop);
    set_enum(c.get(kLGlow), "set_MaskType(via.gui.MaskType)", 0);
    style_rect(c.get(kLGlow), 0, 0, 277, 89, kColLGlow, kPrioLOver, true);
    s9_look(c.get(kLGlow), 20, 23.0f, 23.0f, 23.5f, 23.5f);
    {
        void* const band[5] = { c.get(kLOutline), c.get(kLHalo), c.get(kLOut3), c.get(kLOut4), c.get(kLOut5) };   // outer edges at -3, -2, -1, 0, +1 from the cell's
        optpage::style_band(band, kLCellW, kLCellH, kColLFocus, kPrioLOver);
    }
    optpage::style_left_line(c.get(kLLine), kLLineX, kLLineY, kLLineW, kLLineH, kColLLine, kPrioLCell);
    style_rect(c.get(kLTrack), kLBarX, kLBarY, kLBarW, kLBarH, kColLTrack, kPrioLCell);
    style_rect(c.get(kLThumb), kLBarX, kLBarY, kLBarW, kLBarH, kColWhite, kPrioLOver);
    for (int i = 0; i < kLSlots; ++i) {
        const float x = kLCellX + (i % kLCols) * kLPitchX, y = kLCellY + (i / kLCols) * kLPitchY;
        void* cell = c.cell(i);
        void* cpic = i < static_cast<int>(c.pic.size()) ? c.pic[static_cast<size_t>(i)] : nullptr;
        if (cell && cpic && cpic != cell) {                           // the game's cell shell: its parts resized to the stage cell's
            optpage::lay_out_shell(cell, x, y, kLCellW, kLCellH, kColLOutline, kPrioLCell);
            set_enum(cpic, "set_ControlPoint(via.gui.ControlPoint)", g_cp_lefttop);
            a->ui_set_position(cl, cpic, 0, 0, 0);
            a->ui_set_size(cl, cpic, kLCellW, kLCellH);
            a->ui_set_color(cl, cpic, kColWhite);
            show(cpic, true);
        } else if (cell) {
            style_rect(cell, x, y, kLCellW, kLCellH, kColWhite, kPrioLCell);
        }
        if (g_game_checks) {
            set_enum(c.box(i), "set_Priority(System.UInt16)", kPrioLTop);
            set_enum(c.check(i), "set_Priority(System.UInt16)", kPrioLTop);
            place(c.box(i), x + kLBoxD, y + kLBoxD);
            place(c.check(i), x + kLTickD, y + kLTickD);
            a->ui_set_size(cl, c.box(i), kLBox, kLBox);
            a->ui_set_size(cl, c.check(i), kLTick, kLTick);
        } else {
            style_rect(c.box(i), x + kLBoxD, y + kLBoxD, kLBox, kLBox, kColWhite, kPrioLTop, true);
            style_rect(c.check(i), x + kLBoxD, y + kLBoxD, kLBox, kLBox, kColWhite, kPrioLTop, true);
        }
    }
    for (int i = 0; i < kMosaicMax; ++i) style_rect(c.mosaic(i), 0, 0, 16, 9, kColWhite, kPrioMosaic);
}



void load_pictures(const nsfwopt::TitleRow& row) {
    for (const std::string& p : row.pictures) optpage::picture(p, "title preview");
}

// What the copies show now: written only when it changed
struct Shown {
    int value = -1;
    bool pic_ready = false;
    std::string name, info, keys, dlg_keys;
    bool line_on = false;
    float lx = -1, ly = -1, lsize = -1;
    bool dialog = false;
    int mode = 0;
} g_shown;

std::string number(float v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    const std::string dec = words::tr(words::W::TitleDecimal);
    std::string s = buf;
    if (!dec.empty() && dec[0] != '.') for (char& ch : s) if (ch == '.') ch = dec[0];
    return s;
}
std::string key_item(const std::string& icons_text, words::W what) { return icons_text + " " + words::tr(what); }

void set_text_if(void* t, std::string& last, const std::string& now, bool slide, float width) {
    if (now != last) {
        if (slide) namescroll::release(t);
        A()->ui_set_text(C(), t, now.c_str());
        last = now;
    }
    if (slide) namescroll::drive(t, width);
}

// ---- textures on the copies: set once each (a copy keeps the last texture given): options_page.cpp's Painter ----
void show_if(void* c, bool on) { g_paint.show_if(c, on); }
// The copy shows `res` (true) once loaded; "" or not loadable: false (the caller hides it)
bool put_tex(void* ctrl, const std::string& res, float v0 = 0.0f, float v1 = 1.0f) { return g_paint.put_tex(ctrl, res, v0, v1); }

// The loader's mosaic layout (preview_tex.cpp title_mosaic_layout): cols x rows places of 16:9, the largest that fit the screen;
// between equals the fewest empty places, then the most columns. The block centred, a last row with fewer centred too.
struct Mosaic { int cols = 0, rows = 0, place_w = 0, place_h = 0; };
Mosaic mosaic_layout(int count) {
    Mosaic best;
    for (int cols = 1; cols <= count; ++cols) {
        const int rows = (count + cols - 1) / cols;
        if (cols > 1 && (cols - 1) * rows >= count) continue;
        int w = std::min(1920 / cols, 1080 / rows * 16 / 9);
        w -= w % 16;
        if (w < 32) continue;
        const int empty = cols * rows - count;
        const bool better = w > best.place_w || (w == best.place_w && (empty < best.cols * best.rows - count ||
                                                                     (empty == best.cols * best.rows - count && cols > best.cols)));
        if (!best.cols || better) { best.cols = cols; best.rows = rows; best.place_w = w; best.place_h = w * 9 / 16; }
    }
    return best;
}

// Random's picture, live: the backgrounds ticked, each its thumbnail (its full picture when the places are large). False: not drawn
// here (too many, no copies): the loader's mosaic picture instead.
struct MosaicShown { std::string sig; bool drawn = false; } g_mosaic;
bool paint_mosaic(const Copies& c, const nsfwopt::TitleRow& row, bool on) {
    std::vector<int> idx;
    if (on) for (int i = 0, n = bg_count(row); i < n; ++i) if (g_sel.count(row.keys[static_cast<size_t>(i)])) idx.push_back(i);
    if (!on || idx.empty() || static_cast<int>(idx.size()) > kMosaicMax) {
        if (g_mosaic.drawn) for (int i = 0; i < kMosaicMax; ++i) show_if(c.mosaic(i), false);
        g_mosaic = MosaicShown{};
        return false;
    }
    const Mosaic lay = mosaic_layout(static_cast<int>(idx.size()));
    const bool full = lay.place_w > 960;                                 // larger than the thumbnail: one picture alone
    std::string sig;
    for (int i : idx) sig += row.keys[static_cast<size_t>(i)] + ",";
    const bool moved = sig != g_mosaic.sig;
    const int n = static_cast<int>(idx.size());
    const int rows = (n + lay.cols - 1) / lay.cols;
    const float top = (1080.0f - rows * lay.place_h) / 2;
    bool all_ready = true;
    for (int k = 0; k < kMosaicMax; ++k) {
        void* cell = c.mosaic(k);
        if (k >= n) { show_if(cell, false); continue; }
        const size_t i = static_cast<size_t>(idx[static_cast<size_t>(k)]);
        const std::string& res = full && !row.pictures[i].empty() ? row.pictures[i] : (!row.thumbs[i].empty() ? row.thumbs[i] : row.pictures[i]);
        if (moved) {
            const int r = k / lay.cols, col = k % lay.cols;
            const int in_row = std::min(lay.cols, n - r * lay.cols);
            const float left = (1920.0f - in_row * lay.place_w) / 2;
            place(cell, left + col * lay.place_w + kMosaicGap, top + r * lay.place_h + kMosaicGap);
            A()->ui_set_size(C(), cell, lay.place_w - 2 * kMosaicGap, lay.place_h - 2 * kMosaicGap);
        }
        const bool ready = put_tex(cell, res);
        all_ready = all_ready && ready;
        show_if(cell, ready);
    }
    if (moved) logf("title preview: Random's picture drawn live, %d background(s), %d x %d", n, lay.cols, lay.rows);
    g_mosaic.sig = sig;
    g_mosaic.drawn = true;
    (void)all_ready;
    return true;
}

// The page: one frame of drawing (written when it changed). `up`: shown (the cursor on the section, or the page has the input);
// `held`: the page has the input (its cursor, its tint, its keys).
struct ListShown { int cur = -1, top = -1, n = -1, value = -1; std::string sel, name, keys, desc; bool up = false, held = false, ticks = false; } g_lshown;
void page_hide(const Copies& c) {
    if (!g_lshown.up) return;
    for (const char* nm : { kLPanel, kLLine, kLBanner, kLBanName, kLBanQ, kLRandQ, kLHalo, kLOutline, kLGlow, kLOut3, kLOut4, kLOut5, kLTrack, kLThumb, kPKeys, kPDesc })
        show_if(c.get(nm), false);
    show_if(c.ptint, false);
    for (int i = 0; i < kLSlots; ++i) { show_if(c.cell(i), false); show_if(c.box(i), false); show_if(c.check(i), false); }
    namescroll::release(c.get(kLBanName));
    namescroll::release(c.get(kPKeys));
    namescroll::release(c.get(kPDesc));
    g_lshown = ListShown{};
}
void paint_page(const Copies& c, const nsfwopt::TitleRow& row, int mode, bool up, bool held) {
    using words::W;
    const SF6CoreAPI* a = A();
    if (!up) { page_hide(c); return; }
    const int n_all = static_cast<int>(row.keys.size());          // the choice: every value, Random last
    const int n = g_ticks ? bg_count(row) : n_all;                  // the random settings: the backgrounds alone
    const int random_at = n_all - 1;
    const int chosen = row.value - 1;
    if (!g_lshown.up) {
        for (const char* nm : { kLPanel, kLLine, kLBanName }) show_if(c.get(nm), true);
        g_lshown.up = true;
    }
    std::string sel;
    for (int i = 0; i < bg_count(row); ++i) sel += g_sel.count(row.keys[static_cast<size_t>(i)]) ? '1' : '0';
    const bool redo = g_lshown.cur != g_lcur || g_lshown.top != g_ltop || g_lshown.n != n || g_lshown.sel != sel || g_lshown.value != row.value ||
                      g_lshown.held != held || g_lshown.ticks != g_ticks;
    const int rows = (n + kLCols - 1) / kLCols;
    show_if(c.ptint, held);
    bool random_shown = false;
    for (int s = 0; s < kLSlots; ++s) {
        const int i = g_ltop * kLCols + s;
        void* cell = c.cell(s);
        void* pic = s < static_cast<int>(c.pic.size()) ? c.pic[static_cast<size_t>(s)] : nullptr;
        if (i >= n) { show_if(cell, false); show_if(c.box(s), false); show_if(c.check(s), false); continue; }
        const size_t k = static_cast<size_t>(i);
        const bool is_random = !g_ticks && i == random_at;
        bool ready;
        if (is_random) ready = put_tex(pic, kGlobe);                 // the stage grid's own Random picture, whole (4:1)
        else ready = put_tex(pic, !row.pictures[k].empty() ? row.pictures[k] : row.thumbs[k], kLStripV0, kLStripV1);
        show_if(cell, pic != cell || ready);
        if (pic != cell) show_if(pic, ready);
        if (is_random) {
            random_shown = true;
            if (redo) place(c.get(kLRandQ), kLCellX + (s % kLCols) * kLPitchX + 125, kLCellY + (s / kLCols) * kLPitchY + 32);
        }
        if (redo && pic) a->ui_set_color(C(), pic, held && i != g_lcur ? kColLCellDim : kColWhite);
        if (redo && pic != cell) {                                    // the focused cell: its outline gives way to the focus
            if (void* d = a->ui_find_child(cell, "e_scale9grid_default", 1)) show(d, !(held && i == g_lcur));
        }
        if (g_ticks) {                                                // random settings: the check box, empty or ticked
            const bool on = g_sel.count(row.keys[k]) != 0;
            if (g_game_checks) {
                if (redo) set_enum(c.box(s), "set_UVPatternNo(System.UInt32)", on ? 0 : 1);
                show_if(c.box(s), true);
            } else {
                show_if(c.box(s), put_tex(c.box(s), on ? row.check_on : row.check_off));
            }
            show_if(c.check(s), false);
        } else {                                                      // the choice: the chosen one's yellow corner
            show_if(c.box(s), false);
            if (g_game_checks) {
                if (redo) set_enum(c.check(s), "set_UVPatternNo(System.UInt32)", 0);
                show_if(c.check(s), i == chosen);
            } else {
                show_if(c.check(s), false);
            }
        }
    }
    show_if(c.get(kLRandQ), random_shown);
    if (redo) {
        const int s = g_lcur - g_ltop * kLCols;
        const float cx = kLCellX + (s % kLCols) * kLPitchX + kLCellW / 2, cy = kLCellY + (s / kLCols) * kLPitchY + kLCellH / 2;
        for (const char* nm : { kLGlow, kLOutline, kLHalo, kLOut3, kLOut4, kLOut5 }) place(c.get(nm), cx, cy);
        const bool bar = rows > kLRows;
        show_if(c.get(kLTrack), bar);
        show_if(c.get(kLThumb), bar);
        if (bar) {
            const float h = kLBarH * kLRows / rows, y = kLBarY + (kLBarH - h) * g_ltop / (rows - kLRows);
            place(c.get(kLThumb), kLBarX, y);
            a->ui_set_size(C(), c.get(kLThumb), kLBarW, h);
        }
    }
    for (const char* nm : { kLOutline, kLHalo, kLOut3, kLOut4, kLOut5 }) show_if(c.get(nm), held);
    show_if(c.get(kLGlow), false);                                   // the stage grid's glow: not wanted (Wael 08/11)
    // the banner: the focused entry (the chosen one while the page has not the input)
    const int f = held ? g_lcur : chosen;
    if (f >= 0 && f < n_all) {
        const size_t k = static_cast<size_t>(f);
        const bool is_random = f == random_at;
        bool ok;
        if (is_random) ok = put_tex(c.get(kLBanner), kGlobe);
        else ok = put_tex(c.get(kLBanner), !row.pictures[k].empty() ? row.pictures[k] : row.thumbs[k], kLStripV0, kLStripV1);
        show_if(c.get(kLBanner), ok);
        show_if(c.get(kLBanQ), is_random);
        set_text_if(c.get(kLBanName), g_lshown.name, is_random ? std::string(words::tr(W::TitleRandomShort)) : (k < row.labels.size() ? row.labels[k] : std::string()),
                    true, kLNameW);
    }
    // the keys, in the guide bar's place and style (the game's own keys line hidden while the page has the input)
    if (held) {
        std::string keys;
        if (g_ticks) {
            const bool all = static_cast<int>(g_sel.size()) >= n;
            keys = key_item(icons(63, mode), W::TitleKeyCheck) + "     " + key_item(icons(64, mode), W::TitleKeyBack) + "     " +
                   key_item(icons(306, mode), all ? W::TitleKeyUncheckAll : W::TitleKeyCheckAll) + "     " + key_item(icons(88, mode), W::TitleKeyEdit);
            if (g_one_until && g_frame < g_one_until) keys += "     " + std::string(words::tr(W::TitleAtLeastOne));
        } else {
            keys = key_item(icons(63, mode), W::TitleKeySelect) + "     " + key_item(icons(64, mode), W::TitleKeyBack) + "     " +
                   key_item(icons(306, mode), W::TitleKeyRandomSettings) + "     " + key_item(icons(88, mode), W::TitleKeyEdit);
        }
        if (!g_lshown.held) { place(c.get(kPKeys), g_keys_x, g_keys_y); place(c.get(kPDesc), g_desc_x, g_desc_y); }
        show_if(c.get(kPKeys), true);
        show_if(c.get(kPDesc), true);
        set_text_if(c.get(kPKeys), g_lshown.keys, keys, true, kBottomW);
        set_text_if(c.get(kPDesc), g_lshown.desc, words::tr(W::TitleSectionDesc), true, kBottomW);
    } else {
        show_if(c.get(kPKeys), false);
        show_if(c.get(kPDesc), false);
        g_lshown.keys.clear();
        g_lshown.desc.clear();
    }
    g_lshown.cur = g_lcur;
    g_lshown.top = g_ltop;
    g_lshown.n = n;
    g_lshown.sel = sel;
    g_lshown.value = row.value;
    g_lshown.held = held;
    g_lshown.ticks = g_ticks;
}

void paint(const Copies& c, const nsfwopt::TitleRow& row) {
    using words::W;
    const int v = row.value;
    const size_t i = static_cast<size_t>(v - 1);
    const std::string& res = i < row.pictures.size() ? row.pictures[i] : std::string();
    const std::string key = i < row.keys.size() ? row.keys[i] : std::string();
    void* h = res.empty() ? nullptr : optpage::loaded_picture(res);
    const bool ready = h && A()->ui_texture_ready(h) == 1;
    if (v != g_shown.value) { g_shown.value = v; g_shown.pic_ready = false; }
    if (ready && !g_shown.pic_ready) g_shown.pic_ready = A()->ui_set_texture_rect(C(), c.get(kPic), h, 0.0f, 0.0f, 1.0f, 1.0f) == SF6CORE_OK;
    const bool live = paint_mosaic(c, row, key.empty());                  // Random: its picture drawn live
    show(c.get(kPic), !live && ready && g_shown.pic_ready);
    show(c.get(kNote), !live && !h);
    const int mode = guide_mode_now();
    set_text_if(c.get(kName), g_shown.name, i < row.labels.size() ? row.labels[i] : std::string(), true, kNameW);

    // the line
    auto ln = g_lines.find(key);
    const bool line_on = !key.empty() && ln != g_lines.end() && ln->second.show;
    void* line = c.get(kLine);
    if (line_on) {
        const Line& l = ln->second;
        if (l.size != g_shown.lsize) {
            const float font = l.size * kScreenH / 100.0f;
            set_size_value(line, "set_RasterSize(via.Size)", font * kLineRasterPerFont, font * kLineRasterPerFont);
            A()->ui_set_region_size(C(), line, kLineRegionW, font * kLineRegionPerFont);
            A()->ui_set_font_size(C(), line, font, font);
            g_shown.lsize = l.size;
        }
        if (l.x != g_shown.lx || l.y != g_shown.ly) {
            place(line, l.x * kScreenW / 100.0f, l.y * kScreenH / 100.0f);
            g_shown.lx = l.x; g_shown.ly = l.y;
        }
    }
    if (line_on != g_shown.line_on) { show(line, line_on); show(c.get(kPress), line_on); g_shown.line_on = line_on; }

    // the bottom: the numbers (or why there are none) and the keys, with the game's icons for the device in use
    std::string info;
    if (g_toast_until && g_frame < g_toast_until) info = words::tr(W::TitleAppliedAll);
    else if (key.empty()) info = words::tr(W::TitleInfoRandom);
    else if (!line_on) info = words::tr(W::TitleInfoHidden);
    else info = words::trf(W::TitleInfo, { number(ln->second.x), number(ln->second.y), number(ln->second.size) });
    set_text_if(c.get(kInfo), g_shown.info, info, true, kBottomW);
    std::string keys = key_item(icons(73, mode) + icons(74, mode), W::TitleKeyBackground);
    if (!key.empty()) {
        keys += "     " + key_item(icons(88, mode), line_on ? W::TitleKeyHide : W::TitleKeyShow);
        if (line_on) {
            keys += "     " + key_item(icons(65, mode) + icons(66, mode) + icons(67, mode) + icons(68, mode), W::TitleKeyMove);
            keys += "     " + key_item(icons(86, mode), W::TitleKeyCenterH);
            keys += "     " + key_item(icons(87, mode), W::TitleKeyCenterV);
            keys += "     " + key_item(icons(75, mode) + icons(76, mode), W::TitleKeySize);
        }
        keys += "     " + key_item(icons(306, mode), W::TitleKeyApplyAll);
    }
    keys += "     " + key_item(icons(64, mode), W::TitleKeyBack);
    set_text_if(c.get(kKeys), g_shown.keys, keys, true, kBottomW);

    // the confirmation box
    if (g_dialog != g_shown.dialog) {
        A()->ui_set_text(C(), c.get(kDlgText), words::tr(W::TitleConfirmAll));
        for (const char* n : { kDlgBg, kDlgText, kDlgKeys }) show(c.get(n), g_dialog);
        g_shown.dialog = g_dialog;
        g_shown.dlg_keys.clear();
    }
    if (g_dialog)
        set_text_if(c.get(kDlgKeys), g_shown.dlg_keys, key_item(icons(63, mode), W::TitleYes) + "     " + key_item(icons(64, mode), W::TitleNo), false, 0);
}

// ---- the game's message box: options_page.cpp's GameBox ----
bool box_open(const std::string& text, const std::string& title) { return g_box.open(text, title); }
int box_poll() { return g_box.poll(); }                        // -1 still up, 1 Yes, 0 No (or lost)

void close(void* cm, const char* why) {
    if (!g_open.exchange(false)) return;
    g_cancel.store(false);
    g_dialog = false;
    g_box.drop();
    save_lines("closed");
    if (cm) {
        const Copies c = copies(cm);
        for (const char* n : { kName, kInfo, kKeys, kLBanName }) if (void* t = c.get(n)) namescroll::release(t);
        for (void* x : c.c) show(x, false);
    }
    g_lshown = ListShown{};
    g_mosaic = MosaicShown{};
    g_paint.vis.clear();
    g_keys.swallow();                                              // the key that closed acts on nothing on the page
    g_shown = Shown{};
    logf("title preview: closed (%s)", why);
}

// One frame of the open preview: the keys
void edit(void* cm, const nsfwopt::TitleRow& row) {
    const size_t i = static_cast<size_t>(row.value - 1);
    const std::string key = i < row.keys.size() ? row.keys[i] : std::string();
    if (g_box.is_open()) {                                         // the game's Yes / No box: its answer
        const int r = box_poll();
        if (r < 0) return;
        g_cancel.store(false);
        g_keys.swallow();                                          // the key that answered acts on nothing here
        if (r == 1) {
            const auto it = g_lines.find(key);
            const Line src = it != g_lines.end() ? it->second : Line{};
            for (const std::string& k : row.keys) if (!k.empty()) g_lines[k] = src;
            g_dirty = true;
            save_lines("applied to every background");
            g_toast_until = g_frame + 120;
            logf("title preview: \"%s\"'s line applied to every background (the game's box)", key.c_str());
        }
        return;
    }
    if (g_dialog) {
        if (pressed(kDecide)) {
            auto it = g_lines.find(key);
            {
                const Line src = it != g_lines.end() ? it->second : Line{};   // a background never set: no line, everywhere
                for (const std::string& k : row.keys) if (!k.empty()) g_lines[k] = src;
                g_dirty = true;
                save_lines("applied to every background");
                g_toast_until = g_frame + 120;
                logf("title preview: \"%s\"'s line applied to every background (%s, %.2f %%, %.2f %%, size %.2f %%)", key.c_str(),
                     src.show ? "shown" : "hidden", src.x, src.y, src.size);
            }
            g_dialog = false;
        } else if (pressed(kBack)) {
            g_dialog = false;
        }
        return;
    }
    if (pressed(kBack) || g_cancel.exchange(false)) { close(cm, "Back"); return; }
    const int n = static_cast<int>(row.labels.size());
    if (n > 0 && (repeat(kBgPrev, 20, 8) || repeat(kBgNext, 20, 8))) {
        save_lines("the background changed");
        const int step = g_keys.held(optpage::kTabR) ? 1 : -1;
        g_edit = ((row.value - 1 + step) % n + n) % n;              // the edited one only: the choice stays (Wael 08/11)
        return;
    }
    if (key.empty()) return;                                       // Random: nothing to set (its picture: the backgrounds drawn)
    if (pressed(kTitle)) {
        Line& l = g_lines[key];                                    // a background never set: the default place
        l.show = !l.show;
        g_dirty = true;
        return;
    }
    if (pressed(kAll)) {                                           // the game's Yes / No box; ours when it cannot open
        if (!box_open(words::tr(words::W::TitleConfirmAll), words::tr(words::W::TitleScreen))) g_dialog = true;
        return;
    }
    auto it = g_lines.find(key);
    if (it == g_lines.end() || !it->second.show) return;
    Line& l = it->second;
    const Line before = l;
    if (pressed(kCenterH)) l.x = 50.0f;
    if (pressed(kCenterV)) l.y = 50.0f;
    l.x = clampf(l.x + move_of(kRight) - move_of(kLeft), 0.0f, 100.0f);
    l.y = clampf(l.y + move_of(kDown) - move_of(kUp), 0.0f, 100.0f);
    if (repeat(kSizeUp, 15, 3)) l.size += kSizeStep;
    if (repeat(kSizeDown, 15, 3)) l.size -= kSizeStep;
    l.size = clampf(l.size, kSizeMin, kSizeMax);
    l.x = round2(l.x); l.y = round2(l.y); l.size = round2(l.size);
    if (l.x != before.x || l.y != before.y || l.size != before.size) g_dirty = true;
}

// ---- the page's keys (the section held: the Options screen's input is blocked, every key is ours) ----
// The editor on an entry: the full-screen preview, its background `idx` (the choice is not changed)
void editor_open(void* cm, const Copies& c, const nsfwopt::TitleRow& row, int idx) {
    g_edit = idx;
    g_open.store(true, std::memory_order_release);
    g_cancel.store(false);
    g_dialog = false;
    g_shown = Shown{};
    g_mosaic = MosaicShown{};
    page_hide(c);
    load_lines();
    g_keys.swallow();
    lay_out(cm, c);
    for (const char* nm : { kBg, kBand, kBand2, kName, kInfo, kKeys }) show(c.get(nm), true);
    logf("title preview: editor opened on \"%s\"", static_cast<size_t>(idx) < row.labels.size() ? row.labels[static_cast<size_t>(idx)].c_str() : "?");
}
void page_keys(void* cm, const Copies& c, const nsfwopt::TitleRow& row, int64_t sec) {
    const int n_all = static_cast<int>(row.keys.size());
    const int n = g_ticks ? bg_count(row) : n_all;
    if (pressed(kBack)) {
        if (g_ticks) {                                             // the random settings: back to the choice
            g_ticks = false;
            logf("title preview: random settings closed (%zu ticked)", g_sel.size());
            return;
        }
        g_page = false;                                            // the page: back to the left list once every key is up
        g_unhold = true;
        hide_guides(false);
        (void)sec;
        logf("title preview: page left");
        return;
    }
    if (n <= 0) return;
    int cur = std::min(g_lcur, n - 1);
    if (repeat(kLeft, 20, 6)) cur = std::max(0, cur - 1);
    if (repeat(kRight, 20, 6)) cur = std::min(n - 1, cur + 1);
    if (repeat(kUp, 20, 6) && cur - kLCols >= 0) cur -= kLCols;
    if (repeat(kDown, 20, 6)) cur = cur + kLCols < n ? cur + kLCols : ((n - 1) / kLCols > cur / kLCols ? n - 1 : cur);
    g_lcur = cur;
    const int rows = (n + kLCols - 1) / kLCols, r = g_lcur / kLCols;
    if (r < g_ltop) g_ltop = r;
    if (r >= g_ltop + kLRows) g_ltop = r - kLRows + 1;
    g_ltop = std::max(0, std::min(g_ltop, std::max(0, rows - kLRows)));
    if (pressed(kTitle)) { editor_open(cm, c, row, g_lcur); return; }   // X: the title editor of that background (Random: its picture)
    if (!g_ticks) {
        if (pressed(kDecide)) nsfwopt::set_title_value(g_lcur + 1);     // A: the choice (the yellow corner moves there)
        else if (pressed(kAll) && bg_count(row) > 0) {                   // Y: the random settings
            g_ticks = true;
            g_lcur = 0;                                             // at the top left (Wael 08/11)
            g_ltop = 0;
            logf("title preview: random settings opened (%zu of %d ticked)", g_sel.size(), bg_count(row));
        }
        return;
    }
    const std::string& k = row.keys[static_cast<size_t>(g_lcur)];
    if (pressed(kDecide)) {
        if (g_sel.count(k) && g_sel.size() <= 1) g_one_until = g_frame + 150;
        else {
            if (g_sel.count(k)) g_sel.erase(k); else g_sel.insert(k);
            save_selection(row);
        }
    } else if (pressed(kAll)) {
        if (static_cast<int>(g_sel.size()) >= n) { g_sel.clear(); g_sel.insert(k); }
        else for (int j = 0; j < n; ++j) g_sel.insert(row.keys[static_cast<size_t>(j)]);
        save_selection(row);
    }
}
// Where the guide bar's first key sits (our keys line starts there)
void read_guide_place() { optpage::guide_place(g_keys_x, g_keys_y, g_desc_x, g_desc_y); }

// ---- the cancel hook (the input is blocked while the preview is up: a safety, should the block not hold) ----
codecheck::Batch g_check;
bool g_check_started = false, g_hooks_done = false, g_hooked = false;
int pre_cancel(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (g_closing.load(std::memory_order_relaxed) || !g_open.load(std::memory_order_acquire)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    if (g_box.up.load(std::memory_order_acquire)) return REFRAMEWORK_HOOK_SKIP_ORIGINAL;   // the game's box is up: its Back is its own
    g_cancel.store(true, std::memory_order_release);
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;           // the preview closes, the list stays
}
void hooks_step() {
    if (!g_check_started) {
        g_check_started = true;
        TD* t = api().tdb()->find_type("app.UIOptionSettingMenu.Flow_SubList.InputDelegates");
        g_check.add(t ? method_of(t, "EventCancel", 0) : nullptr, "app.UIOptionSettingMenu.Flow_SubList.InputDelegates.EventCancel()");
        g_check.start(kCodecheckCache, "title preview");
        return;
    }
    if (g_hooks_done || !g_check.done()) return;
    g_hooks_done = true;
    g_check.log_summary();
    Method* m = g_check.method(0);
    const auto& e = g_check.entry(0);
    if (g_check.failed() || !m) { logf("hook %s: not found or not checked", e.label.c_str()); return; }
    if (!g_check.unique(0)) {
        logf("hook %s: REFUSED (R18): %d other method(s) enter its code, e.g. %s", e.label.c_str(), e.shared, g_check.other_label(0).c_str());
        return;
    }
    g_hooked = m->add_hook(pre_cancel, hook_post_pass, false) != 0;
    logf("hook %s: %s", e.label.c_str(), g_hooked ? "installed (its own code)" : "add_hook refused");
}

} // namespace

void frame() {
    ++g_frame;
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, ui_clone_control) || !SF6CORE_HAS(a, ui_set_texture_rect) || !SF6CORE_HAS(a, ui_agents) ||
        !SF6CORE_HAS(a, ui_find_cached))
        return;
    try {
        // The title screen left for 2 s: on "random", the loader draws the next visit's background (SF6TitleSlots_Next; it does
        // nothing on any other choice). Never during a visit: the game asks for title_start and title_loop again while it stays on
        // the title (about every 30 s), and a switch then crashed it; between two cycles it would not, but title_end (the transition
        // after a key, each pack its own) is loaded once per visit: it would come from another pack than the loop (Wael 08/11: no).
        static bool s_title_up = false;
        static uint32_t s_left_at = 0;
        auto loader_next = [](const char* why) {
            using NextFn = void (*)();
            static NextFn next = nullptr;
            static bool looked = false;
            if (!looked) {
                looked = true;
                if (HMODULE h = GetModuleHandleW(L"amd_ags_x64.dll")) next = reinterpret_cast<NextFn>(GetProcAddress(h, "SF6TitleSlots_Next"));
            }
            if (next) { next(); logf("title preview: next background asked (%s)", why); }
        };
        if (g_frame % 30 == 0) {
            void* tb[1] = {};
            Obj* tag = a->ui_agents("Title", tb, 1) ? static_cast<Obj*>(tb[0]) : nullptr;
            Obj* tcm = tag ? call_obj(tag, "get_ControlMain") : nullptr;
            if (tcm && drawn(tag) && shown(tcm)) {
                s_title_up = true;
                s_left_at = 0;
            } else if (s_title_up) {
                if (!s_left_at) s_left_at = g_frame;
                else if (g_frame - s_left_at >= 120) {
                    s_title_up = false;
                    s_left_at = 0;
                    loader_next("the title screen left");
                }
            }
        }
        if (!g_press_known && g_frame - g_press_look >= 30 && SF6CORE_HAS(a, ui_get_message_id)) {   // the title screen's PRESS ANY BUTTON
            g_press_look = g_frame;
            void* buf[1] = {};
            Obj* cm = a->ui_agents("Title", buf, 1) ? call_obj(static_cast<Obj*>(buf[0]), "get_ControlMain") : nullptr;
            void* t = cm ? a->ui_find_child(cm, "e_text_botton", 2) : nullptr;
            uint8_t g[16] = {};
            if (t && a->ui_get_message_id(t, g) == SF6CORE_OK) {
                bool any = false;
                for (uint8_t b : g) any = any || b;
                if (any) {
                    std::memcpy(g_press_guid, g, 16);
                    g_press_known = true;
                    logf("title preview: PRESS ANY BUTTON's message read on the title screen");
                }
            }
        }
        if (g_unblock) {                                           // the Options screen's input back, once every key is up
            read_buttons();
            if (!any_down()) {
                g_unblock = false;
                if (Obj* agent = options_agent()) a->ui_set_block_input(C(), agent, 0);
            }
        }
        // The page: Options > Interface > Title Screen Selection (the core's section). Shown while the cursor is on the section or the
        // page has the input; Confirm on the section gives the page the input (the core holds the screen's input), B gives it back.
        const int64_t sec = nsfwopt::title_section();
        if (g_unhold) {
            read_buttons();
            if (!any_down()) {
                g_unhold = false;
                if (sec >= 0 && SF6CORE_HAS(a, opt_section_hold)) a->opt_section_hold(C(), sec, 0);
            }
        }
        const bool open = g_open.load(std::memory_order_relaxed);
        if (sec < 0 || !SF6CORE_HAS(a, opt_section_state)) return;
        if (!open && !g_page && !g_lshown.up && (g_frame % 3) != 0) return;   // idle: a look every 3 frames
        nsfwopt::TitleRow row;
        SF6CoreOptSectionState st{};
        st.struct_size = sizeof st;
        const bool have = nsfwopt::title_row(row) && a->opt_section_state(C(), sec, &st) == SF6CORE_OK && st.shown && st.control_main;
        {   // the game's "Restore Default Settings" confirmed on the section (core 0.9.10's SF6CoreOptSectionState.resets): the game's
            // own title again (the OVERDRIVE EDITION lines and Random's selection are kept)
            static int64_t s_sec = -1;
            static uint32_t s_seen = 0;
            if (have) {
                if (s_sec == sec && st.resets != s_seen && row.value != 1) {
                    nsfwopt::set_title_value(1);
                    logf("title preview: the game's Restore Default Settings: the game's own title");
                }
                s_sec = sec;
                s_seen = st.resets;
            }
        }
        void* cm = have ? st.control_main : nullptr;
        if (!have || (!st.focused && !g_page && !open)) {
            if (cm && (g_lshown.up || open)) {
                const Copies c = copies(cm);
                if (open) close(cm, "the section left");
                page_hide(c);
            } else if (!cm) {
                g_lshown = ListShown{};
                g_open.store(false);
            }
            if (g_page) { g_page = false; g_ticks = false; hide_guides(false); if (have) g_unhold = true; }
            g_ok_held = true;
            return;
        }
        read_enums();
        fsname::need_guide_hook();
        load_pictures(row);
        Copies c = copies(cm);
        if (!c.all()) {
            const int st_copy = g_copy_status.load(std::memory_order_acquire);
            if (st_copy != 1 && !g_copy_logged) {
                g_copy_logged = true;
                logf("title preview: copies %s (status %d)", st_copy == SF6CORE_OK ? "asked" : "not made", st_copy);
            }
            if (st_copy != 1 && st_copy != SF6CORE_OK) { ++g_copy_refused; g_copy_status.store(1); g_copy_logged = false; }
            if (g_copy_refused < kCopyRefusedMax && (g_copy_asked == 0 || g_frame - g_copy_asked >= kCopyAskEvery)) {
                g_copy_asked = g_frame;
                fsname::run_in_guide_update(make_copies);
            }
            return;
        }
        static void* s_laid = nullptr;
        if (s_laid != cm) { lay_out(cm, c); s_laid = cm; load_selection(row); }
        if (open) {                                                // the editor (full screen) over the page
            read_buttons();
            nsfwopt::TitleRow ed = row;
            ed.value = std::max(1, std::min(g_edit + 1, static_cast<int>(row.keys.size())));
            edit(cm, ed);
            if (!g_open.load()) return;                            // closed by its keys: the page comes back next frame
            ed.value = std::max(1, std::min(g_edit + 1, static_cast<int>(row.keys.size())));
            hide_guides(true);
            paint(c, ed);
            return;
        }
        if (g_page) {
            read_buttons();
            hide_guides(true);
            page_keys(cm, c, row, sec);
            if (g_open.load()) return;                             // X opened the editor
        } else {
            const bool ok = !g_unhold && confirm_down();
            const bool press = ok && !g_ok_held;
            g_ok_held = ok;
            if (press && st.focused && SF6CORE_HAS(a, opt_section_hold) && a->opt_section_hold(C(), sec, 1) == SF6CORE_OK) {
                g_page = true;
                g_ticks = false;
                g_lcur = std::max(0, std::min(row.value - 1, static_cast<int>(row.keys.size()) - 1));
                g_ltop = std::max(0, g_lcur / kLCols - kLRows + 1);
                load_selection(row);
                read_guide_place();
                g_keys.swallow();                                  // the Confirm that opened acts once released
                hide_guides(true);
                logf("title preview: page opened (\"%s\" chosen)", static_cast<size_t>(row.value - 1) < row.labels.size() ? row.labels[row.value - 1].c_str() : "?");
            }
        }
        paint_page(c, row, guide_mode_now(), true, g_page);
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logf("title preview: EXCEPTION in a frame"); }
    }
}

} // namespace slots::titleprev
