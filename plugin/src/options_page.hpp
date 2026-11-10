// SF6 Slots -- what the pages drawn over the game's Options screen share (options_page.cpp): title_preview.cpp (Title Screen
// Selection and the title editor) and ui_packs.cpp (Interface Packs and its editor). The game's own keys, read natively, and their
// icons; the game's guide bar hidden and its place; the Options screen's parts the pages copy and the copies' style and place; the
// pictures of the stage pak, loaded once for the session; the game's own Yes / No box.
// Every write from LateUpdateBehavior, through the core (R1); a copy is found again by name, its address kept for comparison only
// (R35).
#pragma once

#include "slots.hpp"
#include "slots_ui.hpp"

#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace slots::optpage {

// ---- keys: the game's own bindings ----
// The game's menu buttons, in the order of options_page.cpp's table: their menu ids (app.InputAssign.Digital.Id, read 08/11),
// the pad's buttons (via.hid.GamePadButton), a keyboard key always taken (Esc, Enter, the arrows)
enum Key {
    kBack,          // UICancel (64): B, Esc
    kDecide,        // UIDecide (63): A, Enter
    kTabL,          // UITabL (73): LB
    kTabR,          // UITabR (74): RB
    kChildTabL,     // UIChildTabL (75): LT
    kChildTabR,     // UIChildTabR (76): RT
    kRemove,        // UIRemove (88): X
    kExtra,         // UIOptionExtraButton (306): Y
    kSort,          // UISort (86): L3
    kToggle,        // UIToggle (87): R3
    kUp, kDown, kLeft, kRight,   // UISelectU / D / L / R (65 - 68): the left stick, the d-pad, the arrows
    kKeyCount
};
int menu_id(Key k);
// One reader per page, each with its own held counts: read() once in each frame the page reads its keys. Only while the game's
// window is in front for the keyboard.
class Keys {
public:
    void read();
    bool pressed(Key k) const { return held_[k] == 1; }
    bool down(Key k) const { return down_[k]; }
    uint32_t held(Key k) const { return held_[k]; }
    bool any_down() const;
    // A press, then again while held: after `wait` frames, every `every`
    bool repeat(Key k, uint32_t wait, uint32_t every) const { const uint32_t h = held_[k]; return h == 1 || (h > wait && (h - wait) % every == 0); }
    // Every key down now acts on nothing: the one that opened or closed something acts once released
    void swallow() { for (int i = 0; i < kKeyCount; ++i) held_[i] = down_[i] ? 2 : 0; }
private:
    bool down_[kKeyCount] = {};
    uint32_t held_[kKeyCount] = {};
};
bool game_in_front();
// The game's symbol of a menu id: "<ICON KeyF>", "<ICON BtnD>"... (mode 1 the pad's, 2 the keyboard's)
std::string symbol_of(int id, int mode);
// The device the game's guide shows (app.InputGuideManager's mode)
int guide_mode_now();
// The icons of a menu id for the device in use, asked to the game once a second at most
std::string icons(int id, int mode);
// "<icons> <word>": one key of a guide line
inline std::string key_item(const std::string& icons_text, const char* word) { return icons_text + " " + word; }

// ---- the game's controls ----
const SF6CoreAPI* A();
SF6CoreClient* C();
bool floats_of(Obj* o, const char* m, float* out, size_t n);
bool shown(void* c);                        // get_ActualVisible
bool drawn(Obj* agent);                     // its GameObject's DrawSelf
std::string message(void* text);
std::string name_of(void* c);
Obj* options_agent();                       // the Options screen's agent (GameObject "OptionSettingMenu")
// via.gui.ControlPoint / PageAlignment values (read from the game once, read_enums())
extern int64_t g_cp_center, g_cp_lefttop, g_cp_leftcenter, g_pa_right;
void read_enums();

// ---- the copies ----
void set_enum(void* c, const char* sig, int64_t v);
void set_size_value(void* c, const char* sig, float w, float h);
// A scale9 copy shows another of its UV sequence's looks: the sequence for its nine places (their patterns stay 0 - 8), its border
void s9_look(void* c, int64_t seq, float l, float t, float r, float b);
void show(void* c, bool on);                // shown, or hidden and ForceInvisible
// Places are screen units (1920 x 1080): the copies' parent's own place, taken at each lay-out (set_origin), is taken off
void set_origin(void* parent);
void place(void* c, float x, float y);
float origin_x();                           // the copies' parent's place on the screen (set_origin)
float origin_y();
// A text centred on its point, never squeezed (a long line slides: name_scroll.cpp); font 0: the template's own size
void style_text(void* t, float font, float w, float h, int64_t prio);
// A text by its left point, the same otherwise (a list's line, the guide's keys)
void style_text_left(void* t, float font, float w, float h, int64_t prio);
void style_rect(void* r, float x, float y, float w, float h, uint32_t col, int64_t prio, bool centre = false);

// The Options screen's parts the pages copy, found in its main control `cm` (the sub list's first pooled row: always there,
// whatever section is shown; `unit_hint`: another row's unit name, when that one is not found). One missing: its fallback.
enum Tmpl {
    kTRect,         // a Rect: the row's spin bar (e_rect_bar)
    kTTexture,      // a Texture: the row's flag (e_flag)
    kTText,         // a Text: the row's title (e_text_title)
    kTIconText,     // a Text with icons: the row's value (e_text_0)
    kTShell,        // the row's cell shell (c_bg_s: shadow, rounded mask, outline)
    kTMask,         // its rounded mask (scale9, the game's hub parts)
    kTOutline,      // its outline (scale9)
    kTBox,          // the Options radio list's check box (e_texture_box: the game's uvs_checkbutton)
    kTTick,         // and its tick (e_texture_check)
    kTBarBg,        // the sub list's scroll bar: its track (BG)
    kTBar,          // and its thumb (Bar)
    kTGuideText,    // the guide bar's own key text (InputGuide, e_text_0_d_*)
    kTTitle,        // the screen's header title (c_header > c_title > e_text_title: the title font)
    kTDarken,       // a row's darken (e_darken: the hub's cell shape)
    kTmplCount
};
struct Templates {
    void* t[kTmplCount] = {};
    bool game_checks = false;               // the check boxes are the game's
    bool basic() const { return t[kTRect] && t[kTTexture] && t[kTText] && t[kTIconText]; }
    // The template a copy of `want` is made from: its own, else its fallback (a check box without the game's: `no_checks`)
    void* for_copy(int want, int no_checks) const;
};
Templates templates(void* cm, const std::string& unit_hint);
// Every name's copy under cm (nullptr: not there), one walk per 32 names (core ui_find_many)
void find_all(void* cm, const std::vector<std::string>& names, std::vector<void*>& out);

// A page's panel: a cell shell whose own parts are hidden, ours in its c_bg (made last: first among them): the fading mask (the
// game's c_bgContents/e_rect_mask), the panel's two scale9 (e_scale9grid_bg, e_scale9grid_focus: the page has the input)
void make_panel_parts(void* panel, const Templates& t, const char* mask, const char* bg, const char* tint);
void lay_out_panel(void* panel, void* mask, void* bg, void* tint, float x, float y, float w, float h, uint32_t bg_col, uint32_t tint_col, int64_t prio);
// A cell shell at x, y (its top left), w x h: its shadow, rounded mask and outline (colour `outline`) resized, its other parts hidden
void lay_out_shell(void* cell, float x, float y, float w, float h, uint32_t outline, int64_t prio);
// The page's left bar: the header's line (e_scale9grid_line_00, from a rounded mask copy) turned 90 degrees
void style_left_line(void* line, float x, float y, float w, float h, uint32_t col, int64_t prio);
// A banner's name as the stage grid's (e_text_stage: slot 6, glow 20 x 20, strength 3), ending at its right
void style_banner_name(void* name, float font, float w, float h, float x, float y, uint32_t col, int64_t font_slot, int64_t prio);
// The focus band: five outlines one unit apart around a w x h cell (outer edges -3 to +1 from the cell's), centred on its point
void style_band(void* const band[5], float w, float h, uint32_t col, int64_t prio);
void place_band(void* const band[5], float cx, float cy);

// ---- the guide bar (InputGuide > p_InputGuideList_0_fit): hidden while a page has the input, put back after ----
class Guides {
public:
    void hide(bool on);
private:
    std::set<uintptr_t> hidden_;            // the guide bars hidden (agents' addresses, for comparison only)
};
void* guide_key_text();                     // the guide bar's first key text: a page's keys line's template
// Where the guide bar's first key and its description line sit (each left as it is when not found)
void guide_place(float& keys_x, float& keys_y, float& desc_x, float& desc_y);

// ---- the pictures: resources of the stage pak, loaded once for the session ----
void* picture(const std::string& res, const char* who);     // the holder (one reference held; nullptr: not loadable)
void* loaded_picture(const std::string& res);                // the same, only when asked before
// What a page's copies show: textures set once each (a copy keeps the last texture given), shown / hidden written when it changes
struct Painter {
    const char* who;
    std::map<void*, std::string> tex_on;
    std::map<void*, bool> vis;
    void show_if(void* c, bool on);
    // The copy shows `res` (true) once loaded; "" or not loadable: false (the caller hides it). v0, v1: the band of it shown
    bool put_tex(void* ctrl, const std::string& res, float v0 = 0.0f, float v1 = 1.0f);
    void forget() { tex_on.clear(); vis.clear(); }
};

// ---- the game's message box (crsm/src/ui/dialogs.cpp's confirm: app.UIFlowDialog.MessageBox.Start(text, title, 1, 1, YesNo 4,
// no icon -1, FightingGround 1); polled with get_IsEnd, the answer GetSelectValue(box) == 0 for Yes) ----
class GameBox {
public:
    bool open(const std::string& text, const std::string& title);
    int poll();                             // -1 still up, 1 Yes, 0 No (or lost)
    void drop();
    bool is_open() const { return box_ != nullptr; }
    std::atomic<bool> up{ false };          // read by a hook (any thread)
private:
    Obj* box_ = nullptr;
};

// ---- the layout and look the pages share (screen units; via.Color is 0xAABBGGRR) ----
constexpr float kScreenW = 1920, kScreenH = 1080;
// Battle Settings > Other > Stage Settings' places (read 08/11), moved to the Options screen's right side (c_bg_right/e_bg: 912,
// 145, 910 x 833; the stage page starts at 734 on Battle Settings)
constexpr float kPX = 178;
constexpr float kLPanelX = 731 + kPX, kLPanelY = 150, kLPanelW = 960, kLPanelH = 710;
constexpr float kLBanCX = 1148 + kPX, kLBanCY = 281, kLBanW = 776, kLBanH = 194;
constexpr float kLNameCX = 1146 + kPX, kLNameCY = 359, kLNameW = 740, kLNameH = 46, kLNameFont = 30;
constexpr float kLLineX = 739 + kPX, kLLineY = 150, kLLineW = 706, kLLineH = 5;
constexpr float kLStripV0 = 0.27778f, kLStripV1 = 0.72222f;   // a 16:9 picture's middle 4:1 band (the game's cells and banner are 4:1)
constexpr int64_t kNameFontSlot = 6;
constexpr uint32_t kColBg = 0xFF000000u;        // AARRGGBB as the game's own (its e_darken 99000000)
constexpr uint32_t kColBand = 0xB4000000u;
constexpr uint32_t kColDlg = 0xF0101014u;
constexpr uint32_t kColText = 0xFFF3F3F3u;      // the rows' text
constexpr uint32_t kColNote = 0xFFA0A0A0u;
// the stage grid's own (read 08/11): its panel 0x66000000 + 0x1CE98A31, the cells' outline 0x33FFFFFF, the scroll bar 0x0DCCCCCC +
// white, the name 0xFFFFF3FA; the focus: the replay tab's stage focus blue
constexpr uint32_t kColLDim = 0xB4000000u, kColLPanelBg = 0x66000000u, kColLPanelTint = 0x1CE98A31u, kColLOutline = 0x33FFFFFFu;
constexpr uint32_t kColLFocus = 0xFFFF8D00u, kColLGlow = 0xFF811900u, kColLCellDim = 0xFF737373u, kColLTrack = 0x0DCCCCCCu, kColLName = 0xFFFFF3FAu, kColWhite = 0xFFFFFFFFu;
constexpr uint32_t kColLLine = 0x80FFFFFFu;

}  // namespace slots::optpage
