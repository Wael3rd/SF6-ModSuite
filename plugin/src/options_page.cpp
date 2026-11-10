// SF6 Slots -- what the pages drawn over the game's Options screen share (options_page.hpp): taken out of title_preview.cpp as it
// was (08/11), for ui_packs.cpp's page and editor.
#include "options_page.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "user32.lib")   // GetForegroundWindow / GetAsyncKeyState

namespace slots::optpage {
namespace {

constexpr const char* kAgent = "OptionSettingMenu";
constexpr const char* kGuideAgent = "InputGuide";
constexpr const char* kGuideList = "p_InputGuideList_0_fit";
constexpr const char* kSubList = "p_GroupScroll_sub_v";
constexpr const char* kSpin = "p_Spin_spintext_h";

struct BtnDef { int id; const char* pad[2]; int vk; };
const BtnDef kDefs[kKeyCount] = {
    { 64, { "RRight", nullptr }, VK_ESCAPE },          // UICancel
    { 63, { "RDown", nullptr }, VK_RETURN },           // UIDecide
    { 73, { "LTrigTop", nullptr }, 0 },                // UITabL
    { 74, { "RTrigTop", nullptr }, 0 },                // UITabR
    { 75, { "LTrigBottom", nullptr }, 0 },             // UIChildTabL
    { 76, { "RTrigBottom", nullptr }, 0 },             // UIChildTabR
    { 88, { "RLeft", nullptr }, 0 },                   // UIRemove (X)
    { 306, { "RUp", nullptr }, 0 },                    // UIOptionExtraButton (Y)
    { 86, { "LStickPush", nullptr }, 0 },              // UISort (L3)
    { 87, { "RStickPush", nullptr }, 0 },              // UIToggle (R3)
    { 65, { "LUp", "EmuLup" }, VK_UP },                // UISelectU
    { 66, { "LDown", "EmuLdown" }, VK_DOWN },          // UISelectD
    { 67, { "LLeft", "EmuLleft" }, VK_LEFT },          // UISelectL
    { 68, { "LRight", "EmuLright" }, VK_RIGHT },       // UISelectR
};
uint32_t g_pad_mask[kKeyCount] = {};
std::vector<int> g_vks[kKeyCount];
bool g_pad_init = false;
uint32_t g_vk_at = 0;
Method* m_pad_merged = nullptr;
Method* m_pad_button = nullptr;
Method* m_symbol = nullptr;
float g_gx = 0, g_gy = 0;                   // the copies' parent's place on the screen (taken off every place)
std::map<std::string, void*> g_pics;        // resource -> TextureResourceHolder (one reference held; nullptr: not loadable)

// The keys of a keyboard symbol: a letter or digit is the key that types it in this layout; the named ones by name
std::vector<int> vks_of(const std::string& sym) {
    static const struct { const char* name; int vk; } named[] = {
        { "Esc", VK_ESCAPE }, { "Enter", VK_RETURN }, { "NumEnter", VK_RETURN }, { "Tab", VK_TAB }, { "BS", VK_BACK }, { "Space", VK_SPACE },
        { "ArrowU", VK_UP }, { "ArrowD", VK_DOWN }, { "ArrowL", VK_LEFT }, { "ArrowR", VK_RIGHT }, { "Shift", VK_SHIFT }, { "Ctrl", VK_CONTROL },
        { "Alt", VK_MENU }, { "Del", VK_DELETE }, { "Ins", VK_INSERT }, { "Home", VK_HOME }, { "End", VK_END }, { "PgUp", VK_PRIOR }, { "PgDn", VK_NEXT },
    };
    std::vector<int> out;
    const std::string tag = "<ICON Key";
    for (size_t at = sym.find(tag); at != std::string::npos; at = sym.find(tag, at + 1)) {
        const size_t end = sym.find('>', at);
        if (end == std::string::npos) break;
        const std::string k = sym.substr(at + tag.size(), end - at - tag.size());
        if (k.size() == 1 && isalnum(static_cast<unsigned char>(k[0]))) out.push_back(toupper(static_cast<unsigned char>(k[0])));
        else for (const auto& n : named) if (k == n.name) out.push_back(n.vk);
    }
    return out;
}

} // namespace

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

// ---- keys ----
int menu_id(Key k) { return k >= 0 && k < kKeyCount ? kDefs[k].id : 0; }

bool game_in_front() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
std::string symbol_of(int id, int mode) {
    auto* gm = api().get_managed_singleton("app.InputGuideManager");
    if (!gm) return {};
    if (!m_symbol) m_symbol = method_of(gm->get_type_definition(), "GetSymbolText(app.InputAssign.Digital.Id, app.InputGuideMode, System.Int32, app.EConfigInputType)", 4);
    return m_symbol ? str(call_obj(m_symbol, gm, { arg_i32(id), arg_i32(mode), arg_i32(0), arg_i32(0) })) : std::string();
}
int guide_mode_now() {
    int mode = 2;
    auto* gm = api().get_managed_singleton("app.InputGuideManager");
    if (Obj* modes = gm ? get_obj(gm, "_Modes") : nullptr) {
        const Ret r = call(modes, "get_Item", { arg_i32(0) });
        if (!r.exception_thrown && static_cast<int32_t>(r.dword) > 0) mode = static_cast<int32_t>(r.dword);
    }
    return mode;
}
void Keys::read() {
    if (!g_pad_init) {
        g_pad_init = true;
        if (TD* t = api().tdb()->find_type("via.hid.GamePad")) m_pad_merged = method_of(t, "get_MergedDevice", 0);
        TD* b = api().tdb()->find_type("via.hid.GamePadButton");
        for (int i = 0; i < kKeyCount; ++i)
            for (const char* f : kDefs[i].pad)
                if (f && b) if (auto* fld = b->find_field(f)) g_pad_mask[i] |= static_cast<uint32_t>(fld->get_data<int32_t>(nullptr));
    }
    if (!g_vk_at || g_frame - g_vk_at >= 300) {                   // the game's bindings can change while it runs
        g_vk_at = g_frame ? g_frame : 1;
        for (int i = 0; i < kKeyCount; ++i) {
            g_vks[i] = vks_of(symbol_of(kDefs[i].id, 2));
            if (kDefs[i].vk) g_vks[i].push_back(kDefs[i].vk);
        }
    }
    uint32_t pad = 0;
    void* s = api().get_native_singleton("via.hid.GamePad");
    if (Obj* dev = (s && m_pad_merged) ? call_obj(m_pad_merged, reinterpret_cast<Obj*>(s)) : nullptr) {
        if (!m_pad_button) m_pad_button = method_of(dev->get_type_definition(), "get_Button", 0);
        const Ret r = m_pad_button ? call(m_pad_button, dev) : Ret{};
        if (m_pad_button && !r.exception_thrown) pad = r.dword;
    }
    const bool front = game_in_front();
    for (int i = 0; i < kKeyCount; ++i) {
        bool d = (pad & g_pad_mask[i]) != 0;
        if (front) for (int vk : g_vks[i]) d = d || (GetAsyncKeyState(vk) & 0x8000) != 0;
        held_[i] = d ? held_[i] + 1 : 0;
        down_[i] = d;
    }
}
bool Keys::any_down() const { for (bool d : down_) if (d) return true; return false; }

std::string icons(int id, int mode) {
    static std::map<std::pair<int, int>, std::string> cache;
    static uint32_t at = 0;
    if (g_frame - at >= 60) { cache.clear(); at = g_frame; }
    const auto k = std::make_pair(mode, id);
    auto it = cache.find(k);
    return it != cache.end() ? it->second : (cache[k] = symbol_of(id, mode));
}

// ---- the game's controls ----
bool floats_of(Obj* o, const char* m, float* out, size_t n) {
    const Ret r = call(o, m);
    if (r.exception_thrown) return false;
    std::memcpy(out, r.bytes.data(), sizeof(float) * n);
    return true;
}
bool shown(void* c) {
    bool ok = false;
    const bool v = c && call_bool(static_cast<Obj*>(c), "get_ActualVisible", {}, &ok);
    return ok && v;
}
bool drawn(Obj* agent) {
    Obj* go = call_obj(agent, "get_GameObject");
    bool ok = false;
    const bool d = go && call_bool(go, "get_DrawSelf", {}, &ok);
    return ok && d;
}
std::string message(void* text) { return text ? str(call_obj(static_cast<Obj*>(text), "get_Message")) : std::string(); }
std::string name_of(void* c) { return c ? str(call_obj(static_cast<Obj*>(c), "get_Name")) : std::string(); }
Obj* options_agent() {
    void* buf[1] = {};
    return A()->ui_agents(kAgent, buf, 1) ? static_cast<Obj*>(buf[0]) : nullptr;
}

int64_t g_cp_center = 5, g_cp_lefttop = 0;  // via.gui.ControlPoint / PageAlignment CenterCenter, LeftTop (read from the game)
int64_t g_pa_right = 6;                     // via.gui.PageAlignment RightCenter (read from the game)
int64_t g_cp_leftcenter = 3;                // via.gui.ControlPoint / PageAlignment LeftCenter (read from the game)
void read_enums() {
    static bool done = false;
    if (done) return;
    done = true;
    if (TD* cp = api().tdb()->find_type("via.gui.ControlPoint")) {
        if (auto* f = cp->find_field("CenterCenter")) g_cp_center = f->get_data<int32_t>(nullptr);
        if (auto* f = cp->find_field("LeftTop")) g_cp_lefttop = f->get_data<int32_t>(nullptr);
        if (auto* f = cp->find_field("LeftCenter")) g_cp_leftcenter = f->get_data<int32_t>(nullptr);
    }
    if (TD* pa = api().tdb()->find_type("via.gui.PageAlignment")) {
        if (auto* f = pa->find_field("RightCenter")) g_pa_right = f->get_data<int32_t>(nullptr);
    }
}

// ---- the copies ----
void set_enum(void* c, const char* sig, int64_t v) {
    SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = v;
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void set_size_value(void* c, const char* sig, float w, float h) {
    float v[2] = { w, h };
    SF6CoreArg a{}; a.type = SF6CORE_ARG_PTR; a.v.p = v;          // a value type goes by address
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void s9_look(void* c, int64_t seq, float l, float t, float r, float b) {
    if (!c) return;
    for (int64_t pos = 0; pos < 9; ++pos) {
        SF6CoreArg args[2] = {};
        args[0].type = SF6CORE_ARG_I64; args[0].v.i = pos;
        args[1].type = SF6CORE_ARG_I64; args[1].v.i = seq;
        A()->obj_invoke(C(), c, "setSequenceNo(via.gui.GridPos, System.UInt32)", args, 2, nullptr);
    }
    float rect[4] = { l, t, r, b };
    SF6CoreArg a{}; a.type = SF6CORE_ARG_PTR; a.v.p = rect;
    A()->obj_invoke(C(), c, "set_BorderRect(via.Rect)", &a, 1, nullptr);
}
void show(void* c, bool on) {
    if (!c) return;
    if (on) A()->ui_set_force_invisible(C(), c, 0);
    A()->ui_set_visible(C(), c, on ? 1 : 0);
    if (!on) A()->ui_set_force_invisible(C(), c, 1);
}
void set_origin(void* parent) {
    float g[3] = {};
    if (!parent || !floats_of(static_cast<Obj*>(parent), "get_GlobalPosition", g, 2)) g[0] = g[1] = 0;
    g_gx = g[0]; g_gy = g[1];
}
void place(void* c, float x, float y) { A()->ui_set_position(C(), c, x - g_gx, y - g_gy, 0); }
float origin_x() { return g_gx; }
float origin_y() { return g_gy; }
void style_text(void* t, float font, float w, float h, int64_t prio) {
    set_enum(t, "set_ControlPoint(via.gui.ControlPoint)", g_cp_center);
    set_enum(t, "set_PageAlignment(via.gui.PageAlignment)", g_cp_center);
    set_enum(t, "set_AutoRegionFit(via.gui.RegionFitType)", 0);   // never squeezed: a long line slides (name_scroll.cpp)
    set_enum(t, "set_AutoWrap(System.Boolean)", 0);
    A()->ui_set_region_size(C(), t, w, h);
    if (font > 0) A()->ui_set_font_size(C(), t, font, font);
    set_enum(t, "set_Priority(System.UInt16)", prio);
}
void style_text_left(void* t, float font, float w, float h, int64_t prio) {
    set_enum(t, "set_ControlPoint(via.gui.ControlPoint)", g_cp_leftcenter);
    set_enum(t, "set_PageAlignment(via.gui.PageAlignment)", g_cp_leftcenter);
    set_enum(t, "set_AutoRegionFit(via.gui.RegionFitType)", 0);
    set_enum(t, "set_AutoWrap(System.Boolean)", 0);
    A()->ui_set_region_size(C(), t, w, h);
    if (font > 0) A()->ui_set_font_size(C(), t, font, font);
    set_enum(t, "set_Priority(System.UInt16)", prio);
}
void style_rect(void* r, float x, float y, float w, float h, uint32_t col, int64_t prio, bool centre) {
    set_enum(r, "set_ControlPoint(via.gui.ControlPoint)", centre ? g_cp_center : g_cp_lefttop);
    place(r, x, y);
    A()->ui_set_size(C(), r, w, h);
    A()->ui_set_color(C(), r, col);
    set_enum(r, "set_Priority(System.UInt16)", prio);
}

void* Templates::for_copy(int want, int no_checks) const {
    static const int kFallback[kTmplCount] = { 0, 1, 2, 3, 1, 0, 0, 1, 1, 0, 0, 3, 2, 5 };
    int k = want;
    if (!t[k]) k = kFallback[k];
    if (k == kTBox || k == kTTick) k = game_checks ? k : no_checks;
    return t[k];
}
Templates templates(void* cm, const std::string& unit_hint) {
    const SF6CoreAPI* a = A();
    Templates out;
    void* sub = cm ? a->ui_find_cached(cm, kSubList) : nullptr;
    // the sub list's first pooled row (always there, whatever section is shown)
    void* unit = sub ? a->ui_find_child(sub, "p_OptionUnit_0_switch_show", 3) : nullptr;
    if (!unit && sub && !unit_hint.empty()) unit = a->ui_find_child(sub, unit_hint.c_str(), 3);
    void* spin = unit ? a->ui_find_child(unit, kSpin, 1) : nullptr;
    void* shell = unit ? a->ui_find_child(unit, "c_bg_s", 6) : nullptr;
    void* radio = cm ? a->ui_find_child(cm, "p_Group_radiobtn_switch", 5) : nullptr;
    void* bar = sub ? a->ui_find_child(sub, "c_ScrollBar", 3) : nullptr;
    void* header = cm ? a->ui_find_child(cm, "c_header", 1) : nullptr;
    void* htitle = header ? a->ui_find_child(header, "c_title", 3) : nullptr;
    void* t[kTmplCount] = {
        spin ? a->ui_find_child(spin, "e_rect_bar", 4) : nullptr, unit ? a->ui_find_child(unit, "e_flag", 5) : nullptr,
        spin ? a->ui_find_child(spin, "e_text_title", 1) : nullptr, spin ? a->ui_find_child(spin, "e_text_0", 2) : nullptr,
        shell, shell ? a->ui_find_child(shell, "e_scale9grid_mask", 2) : nullptr, shell ? a->ui_find_child(shell, "e_scale9grid_default", 1) : nullptr,
        radio ? a->ui_find_child(radio, "e_texture_box", 5) : nullptr, radio ? a->ui_find_child(radio, "e_texture_check", 5) : nullptr,
        bar ? a->ui_find_child(bar, "BG", 1) : nullptr, bar ? a->ui_find_child(bar, "Bar", 1) : nullptr, guide_key_text(),
        htitle ? a->ui_find_child(htitle, "e_text_title", 0) : nullptr, unit ? a->ui_find_child(unit, "e_darken", 3) : nullptr };
    for (int i = 0; i < kTmplCount; ++i) out.t[i] = t[i];
    out.game_checks = t[kTBox] && t[kTTick];
    return out;
}
void find_all(void* cm, const std::vector<std::string>& names, std::vector<void*>& out) {
    const SF6CoreAPI* a = A();
    out.assign(names.size(), nullptr);
    if (SF6CORE_HAS(a, ui_find_many)) {
        for (size_t at = 0; at < names.size(); at += 32) {
            const char* nm[32] = {};
            void* found[32] = {};
            const size_t n = std::min<size_t>(32, names.size() - at);
            for (size_t i = 0; i < n; ++i) nm[i] = names[at + i].c_str();
            a->ui_find_many(cm, nm, static_cast<uint32_t>(n), found);
            for (size_t i = 0; i < n; ++i) out[at + i] = found[i];
        }
    } else {
        for (size_t i = 0; i < names.size(); ++i) out[i] = a->ui_find_child(cm, names[i].c_str(), 0);
    }
}

void make_panel_parts(void* panel, const Templates& t, const char* mask, const char* bg, const char* tint) {
    const SF6CoreAPI* a = A();
    void* cbg = panel ? a->ui_find_child(panel, "c_bg", 1) : nullptr;
    const std::pair<const char*, int> parts[] = { { tint, kTMask }, { bg, kTMask }, { mask, kTRect } };
    for (const auto& [nm, k] : parts) {
        if (!cbg || a->ui_find_child(cbg, nm, 0) || !t.t[k]) continue;
        void* x = nullptr;
        if (a->ui_clone_control(C(), t.t[k], cbg, nm, &x) == SF6CORE_OK && x) a->ui_set_visible(C(), x, 0);
    }
}
void lay_out_panel(void* panel, void* mask, void* bg, void* tint, float x, float y, float w, float h, uint32_t bg_col, uint32_t tint_col, int64_t prio) {
    const SF6CoreAPI* a = A();
    SF6CoreClient* cl = C();
    if (!panel) return;
    set_enum(panel, "set_Priority(System.UInt16)", prio);
    place(panel, x, y);
    for (const char* nm : { "e_scale9grid_shadow", "e_scale9grid_default" })
        if (void* p = a->ui_find_child(panel, nm, 1)) a->ui_set_force_invisible(cl, p, 1);
    if (void* cbg = a->ui_find_child(panel, "c_bg", 1)) {
        a->ui_set_position(cl, cbg, 0, 0, 0);
        for (const char* nm : { "e_scale9grid_mask", "e_mat_bg", "e_rect_bar", "e_focus_bar", "e_rect_bg", "e_tex_point_bg" })
            if (void* p = a->ui_find_child(cbg, nm, 0)) { set_enum(p, "set_MaskType(via.gui.MaskType)", 0); a->ui_set_force_invisible(cl, p, 1); }
    }
    if (void* m = mask) {                                        // the fading mask: white, its right side (top right) clear
        set_enum(m, "set_ControlPoint(via.gui.ControlPoint)", g_cp_lefttop);
        a->ui_set_position(cl, m, 0, 0, 0);
        a->ui_set_size(cl, m, w, h);
        a->ui_set_color(cl, m, 0xFFFFFFFFu);
        set_enum(m, "set_MaskType(via.gui.MaskType)", 2);
        set_enum(m, "set_ColorType(via.gui.ColorType)", 2);
        const std::pair<const char*, uint32_t> corners[] = {
            { "set_ColorLeftTop(via.Color)", 0xFFFFFFFFu }, { "set_ColorTop(via.Color)", 0xFFFFFFFFu }, { "set_ColorRightTop(via.Color)", 0x00FFFFFFu },
            { "set_ColorLeft(via.Color)", 0xFFFFFFFFu }, { "set_ColorRight(via.Color)", 0x00FFFFFFu },
            { "set_ColorLeftBottom(via.Color)", 0xFFFFFFFFu }, { "set_ColorBottom(via.Color)", 0xFFFFFFFFu }, { "set_ColorRightBottom(via.Color)", 0xFFFFFFFFu } };
        for (const auto& [sig, col] : corners) {
            uint32_t v = col;
            SF6CoreArg arg{}; arg.type = SF6CORE_ARG_PTR; arg.v.p = &v;
            a->obj_invoke(cl, m, sig, &arg, 1, nullptr);
        }
        show(m, true);
    }
    for (void* p : { bg, tint }) {
        if (!p) continue;
        set_enum(p, "set_ControlPoint(via.gui.ControlPoint)", g_cp_lefttop);
        set_enum(p, "set_MaskType(via.gui.MaskType)", 0);
        a->ui_set_position(cl, p, 0, 0, 0);
        a->ui_set_size(cl, p, w, h);
        a->ui_set_color(cl, p, p == bg ? bg_col : tint_col);
        s9_look(p, 10, 12.5f, 12.5f, 12.5f, 12.5f);              // e_scale9grid_bg / _focus
    }
    show(bg, true);
}
void lay_out_shell(void* cell, float x, float y, float w, float h, uint32_t outline, int64_t prio) {
    const SF6CoreAPI* a = A();
    SF6CoreClient* cl = C();
    if (!cell) return;
    set_enum(cell, "set_Priority(System.UInt16)", prio);
    place(cell, x, y);
    if (void* sh = a->ui_find_child(cell, "e_scale9grid_shadow", 1)) {   // the stage cell's: (-8, -6), 19 larger
        a->ui_set_position(cl, sh, -8, -6, 0);
        a->ui_set_size(cl, sh, w + 19, h + 19);
    }
    void* bg = a->ui_find_child(cell, "c_bg", 1);
    if (void* m = bg ? a->ui_find_child(bg, "e_scale9grid_mask", 0) : nullptr) { a->ui_set_position(cl, m, 0, 0, 0); a->ui_set_size(cl, m, w, h); }
    if (void* d = a->ui_find_child(cell, "e_scale9grid_default", 1)) {
        a->ui_set_position(cl, d, 0, 0, 0);
        a->ui_set_size(cl, d, w, h);
        a->ui_set_color(cl, d, outline);
    }
    for (const char* other : { "e_mat_bg", "e_rect_bar", "e_focus_bar", "e_rect_bg", "e_tex_point_bg" })
        if (void* o = bg ? a->ui_find_child(bg, other, 0) : nullptr) a->ui_set_force_invisible(cl, o, 1);
}
void style_left_line(void* line, float x, float y, float w, float h, uint32_t col, int64_t prio) {
    set_enum(line, "set_MaskType(via.gui.MaskType)", 0);
    style_rect(line, x, y, w, h, col, prio);
    s9_look(line, 8, 7.0f, 0.0f, 7.0f, 0.0f);                       // c_tab / e_scale9grid_line_00, turned 90 degrees
    float rot[3] = { 0.0f, 0.0f, 90.0f };
    SF6CoreArg r{}; r.type = SF6CORE_ARG_PTR; r.v.p = rot;
    A()->obj_invoke(C(), line, "set_Rotation(via.vec3)", &r, 1, nullptr);
}
void style_banner_name(void* name, float font, float w, float h, float x, float y, uint32_t col, int64_t font_slot, int64_t prio) {
    const SF6CoreAPI* a = A();
    SF6CoreClient* cl = C();
    style_text(name, font, w, h, prio);
    set_enum(name, "set_PageAlignment(via.gui.PageAlignment)", g_pa_right);   // the whole banner's width, ending at its right as the game's
    set_enum(name, "set_FontSlot(via.gui.FontSlot)", font_slot);
    SF6CoreArg on{}; on.type = SF6CORE_ARG_BOOL; on.v.i = 1;
    a->obj_invoke(cl, name, "set_GlowEnable(System.Boolean)", &on, 1, nullptr);
    uint32_t black = 0xFF000000u;
    SF6CoreArg c{}; c.type = SF6CORE_ARG_PTR; c.v.p = &black;
    a->obj_invoke(cl, name, "set_GlowColor(via.Color)", &c, 1, nullptr);
    set_size_value(name, "set_GlowBlurSize(via.Size)", 20, 20);
    SF6CoreArg st{}; st.type = SF6CORE_ARG_F64; st.v.f = 3.0;
    a->obj_invoke(cl, name, "set_GlowStrength(System.Single)", &st, 1, nullptr);
    place(name, x, y);
    a->ui_set_color(cl, name, col);
}
void style_band(void* const band[5], float w, float h, uint32_t col, int64_t prio) {
    for (int i = 0; i < 5; ++i) style_rect(band[i], 0, 0, w + 6 - 2 * i, h + 6 - 2 * i, col, prio, true);
}
void place_band(void* const band[5], float cx, float cy) { for (int i = 0; i < 5; ++i) place(band[i], cx, cy); }

// ---- the guide bar ----
void Guides::hide(bool on) {
    const SF6CoreAPI* a = A();
    void* buf[4] = {};
    const uint32_t n = a->ui_agents(kGuideAgent, buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        const uintptr_t id = reinterpret_cast<uintptr_t>(buf[i]);
        Obj* cm = call_obj(static_cast<Obj*>(buf[i]), "get_ControlMain");
        void* list = cm ? a->ui_find_child(cm, kGuideList, 3) : nullptr;
        if (!list) continue;
        if (on && !hidden_.count(id) && shown(list)) { a->ui_set_force_invisible(C(), list, 1); hidden_.insert(id); }
        else if (!on && hidden_.count(id)) a->ui_set_force_invisible(C(), list, 0);
    }
    if (!on) hidden_.clear();
}
void* guide_key_text() {
    const SF6CoreAPI* a = A();
    void* buf[4] = {};
    const uint32_t n = a->ui_agents(kGuideAgent, buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* gcm = call_obj(static_cast<Obj*>(buf[i]), "get_ControlMain");
        void* btn = gcm ? a->ui_find_child(gcm, "c_guideBtn", 8) : nullptr;
        for (Obj* t = btn ? call_obj(static_cast<Obj*>(btn), "get_Child") : nullptr; t; t = call_obj(t, "get_Next"))
            if (name_of(t).rfind("e_text_0", 0) == 0) return t;
    }
    return nullptr;
}
void guide_place(float& keys_x, float& keys_y, float& desc_x, float& desc_y) {
    if (void* t = guide_key_text()) {
        float g[3] = {};
        if (floats_of(static_cast<Obj*>(t), "get_GlobalPosition", g, 2)) { keys_x = g[0]; keys_y = g[1]; }
    }
    void* buf[4] = {};
    const uint32_t n = A()->ui_agents(kGuideAgent, buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* gcm = call_obj(static_cast<Obj*>(buf[i]), "get_ControlMain");
        void* sc = gcm ? A()->ui_find_child(gcm, "p_ScrollText_0", 8) : nullptr;
        void* t = sc ? A()->ui_find_child(sc, "e_text", 3) : nullptr;
        float g[3] = {};
        if (t && shown(t) && floats_of(static_cast<Obj*>(t), "get_GlobalPosition", g, 2)) { desc_x = g[0]; desc_y = g[1]; break; }
    }
}

// ---- the pictures ----
void* picture(const std::string& res, const char* who) {
    if (res.empty()) return nullptr;
    auto it = g_pics.find(res);
    if (it != g_pics.end()) return it->second;
    void* h = nullptr;
    void* got = A()->ui_texture_load(C(), res.c_str(), &h) == SF6CORE_OK ? h : nullptr;
    g_pics[res] = got;
    if (!got) logf("%s: picture %s not loadable", who ? who : "options page", res.c_str());
    return got;
}
void* loaded_picture(const std::string& res) {
    auto it = g_pics.find(res);
    return it != g_pics.end() ? it->second : nullptr;
}
void Painter::show_if(void* c, bool on) {
    auto it = vis.find(c);
    if (it != vis.end() && it->second == on) return;
    show(c, on);
    vis[c] = on;
}
bool Painter::put_tex(void* ctrl, const std::string& res, float v0, float v1) {
    if (!ctrl || res.empty()) return false;
    if (auto t = tex_on.find(ctrl); t != tex_on.end() && t->second == res) return true;
    void* h = picture(res, who);
    if (!h || A()->ui_texture_ready(h) != 1) return false;
    auto& on = tex_on[ctrl];
    if (on == res) return true;
    if (A()->ui_set_texture_rect(C(), ctrl, h, 0.0f, v0, 1.0f, v1 - v0) != SF6CORE_OK) return false;
    on = res;
    return true;
}

// ---- the game's message box ----
bool GameBox::open(const std::string& text, const std::string& title) {
    TD* td = api().tdb()->find_type("app.UIFlowDialog.MessageBox");
    Method* m = td ? method_of(td, "Start", 7) : nullptr;
    if (!m) return false;
    Obj* box = call_obj(m, nullptr, { arg_obj(new_string(text)), arg_obj(new_string(title)), arg_i32(1), arg_i32(1), arg_i32(4), arg_i32(-1), arg_i32(1) });
    if (!box) return false;
    box->add_ref();
    box_ = box;
    up.store(true, std::memory_order_release);
    return true;
}
int GameBox::poll() {
    if (!box_) return 0;
    bool ok = false;
    const bool ended = call_bool(box_, "get_IsEnd", {}, &ok);
    if (ok && !ended) return -1;
    int answer = 0;
    if (ok) {
        TD* td = api().tdb()->find_type("app.UIFlowDialog.MessageBox");
        Method* g = td ? method_of(td, "GetSelectValue") : nullptr;
        const Ret v = g ? call(g, nullptr, { arg_obj(box_) }) : Ret{};
        answer = g && !v.exception_thrown && static_cast<int32_t>(v.dword) == 0 ? 1 : 0;
    }
    box_->release();
    box_ = nullptr;
    up.store(false, std::memory_order_release);
    return answer;
}
void GameBox::drop() {
    if (box_) { box_->release(); box_ = nullptr; }
    up.store(false, std::memory_order_release);
}

}  // namespace slots::optpage
