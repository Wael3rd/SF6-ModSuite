// Costume colour editor, native core (phase 4d). See editor_core.hpp. Every block names the Lua it ports
// (SF6_ColorSpinExtra.lua at commit 4dab476); the order of the calls on the option engine is the Lua's, statement by
// statement: tools/editor_harness compares the two traces line by line.
#include "editor_core.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "nlohmann/json.hpp"

#include "../fighters.hpp"
#include "../engine/catalog.hpp"
#include "../engine/chips.hpp"

namespace dialogs::editor::core {

// The avatar editor's colour grid (AvatarCreateColorPopUp, read live 16/10), line by line, left to right
const uint32_t kPalette[kPaletteLines][kPaletteCols] = {
    { 0xFF0000, 0xFF7700, 0xFAFF00, 0x33FF00, 0x00FFE7, 0x009AFF, 0x000EFF, 0x9D00FF, 0xFF00DD, 0xFFFFFF },
    { 0xCC0000, 0xCC5F00, 0xC8CC00, 0x28CC00, 0x00CCB9, 0x007DCC, 0x000DCC, 0x7D00CC, 0xCC00B0, 0xBABABA },
    { 0x990000, 0x994700, 0x969900, 0x1E9900, 0x00998A, 0x005E99, 0x000899, 0x5E0099, 0x990084, 0x777777 },
    { 0x660000, 0x662F00, 0x646600, 0x146600, 0x00665C, 0x003E66, 0x000666, 0x3E0066, 0x660058, 0x3A3A3A },
    { 0x330000, 0x331800, 0x323300, 0x0A3300, 0x00332E, 0x001F33, 0x000333, 0x1F0033, 0x33002C, 0x000000 },
};

namespace {

using rsz::Node;
using rsz::NodeRef;
using TargetsRef = std::shared_ptr<std::vector<NodeRef>>;      // a row's colours; the Lua compares these lists by identity

Backend be;
std::string g_data_dir = "reframework/data";
engine::Catalog g_catalog;
bool g_catalog_built = false;

const char* const DATA_DIR = "SF6_ColorSpinExtra_data/";
const char* const MANUAL_DIR = "MANUALCOLORS";
const char* const ROOT = "__coloredit_root";
const char* const SQUARE = "\xE2\x96\xA0\xE2\x96\xA0";          // the two swatch glyphs
constexpr int ROWS = 7;                                         // the dialog page is a fixed pool of 8 rows, the 8th sits outside the frame
struct Pad { uint32_t BTN = 64, A = 32, Y = 16, B = 128, L1 = 256, R1 = 1024, R3 = 8192, L3 = 4096; } PAD;

// ---------------------------------------------------------------- small Lua-isms
std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap; va_start(ap, f);
    const int n = std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    if (n < 0) return {};
    if ((size_t)n < sizeof(buf)) return std::string(buf, (size_t)n);
    std::string big((size_t)n + 1, '\0');
    va_start(ap, f); std::vsnprintf(big.data(), big.size(), f, ap); va_end(ap);
    big.resize((size_t)n);
    return big;
}
double lua_mod(double a, double b) {                            // Lua's float a % b
    double m = std::fmod(a, b);
    if (m != 0.0 && (m < 0.0) != (b < 0.0)) m += b;
    return m;
}
double lfloor(double x) { return std::floor(x); }
std::string L(const std::string& key) { return be.L ? be.L(key, {}) : key; }
std::string L(const std::string& key, const std::vector<std::string>& args) { return be.L ? be.L(key, args) : key; }
std::string upper(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 32); return s; }
std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c + 32); return s; }
bool starts(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool ends(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
void strip_prefix(std::string& s, const std::string& p) { if (starts(s, p)) s.erase(0, p.size()); }
void strip_suffix(std::string& s, const std::string& p) { if (ends(s, p)) s.erase(s.size() - p.size()); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
void strip_esf(std::string& s) {                                // gsub("^esf%d*_", "")
    if (!starts(s, "esf")) return;
    size_t i = 3;
    while (i < s.size() && is_digit(s[i])) ++i;
    if (i < s.size() && s[i] == '_') s.erase(0, i + 1);
}
std::string backslashes(std::string s) { for (auto& c : s) if (c == '/') c = '\\'; return s; }
void journal(const std::string& s) { if (be.journal) be.journal(s); }
int64_t frame_now() { return be.frame ? be.frame() : 0; }

// ---------------------------------------------------------------- the tree (app.CostumeMaterialData as parse_user gives it)
std::vector<NodeRef>* arr_of(const NodeRef& n, const char* field) { auto* v = n ? n->field(field) : nullptr; return v ? &v->arr : nullptr; }
NodeRef obj_of(const NodeRef& n, const char* field) { auto* v = n ? n->field(field) : nullptr; return v ? v->obj : nullptr; }
uint32_t cc_color(const NodeRef& cc) { auto* v = cc ? cc->field("Color") : nullptr; return v ? v->u : 0; }
void cc_set_color(const NodeRef& cc, uint32_t c) { if (auto* v = cc ? cc->field("Color") : nullptr) v->u = c; }
bool node_enable(const NodeRef& n) { auto* v = n ? n->field("Enable") : nullptr; return v && v->b; }
void node_set_enable(const NodeRef& n, bool on) { if (auto* v = n ? n->field("Enable") : nullptr) v->b = on; }
bool node_has_value(const NodeRef& n) { return n && n->field("Value") != nullptr; }
double node_value(const NodeRef& n) { auto* v = n ? n->field("Value") : nullptr; return v ? (double)v->f : 0.0; }
void node_set_value(const NodeRef& n, double x) { if (auto* v = n ? n->field("Value") : nullptr) v->f = (float)x; }
std::string cl_name(const NodeRef& cl) { auto* v = cl ? cl->field("Name") : nullptr; return v ? v->s : std::string(); }
int part_type(const NodeRef& p) { auto* v = p ? p->field("Type") : nullptr; return v ? (int)(int32_t)v->u : 0; }

NodeRef copy_tree(const NodeRef& t) {                           // :2088 (shared nodes are not kept shared, as in the Lua)
    if (!t) return nullptr;
    auto c = std::make_shared<Node>();
    c->cls = t->cls;
    c->fields = t->fields;
    for (auto& v : c->fields) {
        if (v.obj) v.obj = copy_tree(v.obj);
        for (auto& e : v.arr) if (e) e = copy_tree(e);
    }
    return c;
}
// for pi, part / for ci, cl / for k, cc: the Lua's three nested ipairs, with their stops (a false / nil element ends ipairs
// only when nil; false entries are skipped by the `if cc` tests)
template <class F> void each_cc(const NodeRef& tree, F fn) {    // fn(pi, ci, k, part, cl, cc) 1-based
    auto* parts = arr_of(tree, "Parts");
    if (!parts) return;
    for (size_t pi = 0; pi < parts->size(); ++pi) {
        const NodeRef& part = (*parts)[pi];
        auto* cls = arr_of(part, "Clusters");
        if (!cls) continue;
        for (size_t ci = 0; ci < cls->size(); ++ci) {
            const NodeRef& cl = (*cls)[ci];
            auto* ccs = arr_of(cl, "CustomizeColors");
            if (!ccs) continue;
            for (size_t k = 0; k < ccs->size(); ++k) fn((int)pi + 1, (int)ci + 1, (int)k + 1, part, cl, (*ccs)[k]);
        }
    }
}
NodeRef cc_at(const NodeRef& tree, int pi, int ci, int k) {     // tree.Parts[pi].Clusters[ci].CustomizeColors[k] or nil
    auto* parts = arr_of(tree, "Parts");
    if (!parts || pi < 1 || (size_t)pi > parts->size()) return nullptr;
    auto* cls = arr_of((*parts)[pi - 1], "Clusters");
    if (!cls || ci < 1 || (size_t)ci > cls->size()) return nullptr;
    auto* ccs = arr_of((*cls)[ci - 1], "CustomizeColors");
    if (!ccs || k < 1 || (size_t)k > ccs->size()) return nullptr;
    return (*ccs)[k - 1];
}
NodeRef opt_node(const NodeRef& cc, const char* name) { return obj_of(obj_of(cc, "Option"), name); }

// ---------------------------------------------------------------- colour maths (:2523-2625)
std::string rgb_hex(uint32_t col) { return fmt("%02X%02X%02X", col % 0x100, (col / 0x100) % 0x100, (col / 0x10000) % 0x100); }
void rgb_of(uint32_t col, int& r, int& g, int& b, int& a) { r = col % 0x100; g = (col / 0x100) % 0x100; b = (col / 0x10000) % 0x100; a = (col / 0x1000000) % 0x100; }
void rgb_to_hsv(int ri, int gi, int bi, double& h, double& s, double& v) {
    const double r = ri / 255.0, g = gi / 255.0, b = bi / 255.0;
    const double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b));
    const double d = mx - mn;
    h = 0;
    if (d > 0) {
        if (mx == r) h = lua_mod((g - b) / d, 6.0); else if (mx == g) h = (b - r) / d + 2; else h = (r - g) / d + 4;
        h = h * 60;
    }
    s = (mx > 0) ? (d / mx) : 0;
    v = mx;
}
void hsv_to_rgb(double h, double s, double v, int& ro, int& go, int& bo) {
    h = lua_mod(h, 360.0);
    const double c = v * s;
    const double x = c * (1 - std::fabs(lua_mod(h / 60, 2.0) - 1));
    const double m = v - c;
    double r, g, b;
    if (h < 60) { r = c; g = x; b = 0; } else if (h < 120) { r = x; g = c; b = 0; } else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; } else if (h < 300) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
    ro = (int)lfloor((r + m) * 255 + 0.5); go = (int)lfloor((g + m) * 255 + 0.5); bo = (int)lfloor((b + m) * 255 + 0.5);
}
uint32_t pack_rgba(int r, int g, int b, int a) { return (uint32_t)a * 0x1000000u + (uint32_t)b * 0x10000u + (uint32_t)g * 0x100u + (uint32_t)r; }

// HSV view of a packed colour, every component on 0-255; the last set hue / sat survive greys (the memo)
struct MemoKey {
    const Node* node = nullptr; std::string name;
    bool operator<(const MemoKey& o) const { return node != o.node ? node < o.node : name < o.name; }
};
struct Memo { int h = 0, s = 0; uint32_t col = 0; };
std::map<MemoKey, Memo> g_memo;
std::vector<NodeRef> g_memo_keep;                               // the Lua's memo keys keep their tables alive
struct HSV3 { int H = 0, S = 0, V = 0; int get(char c) const { return c == 'H' ? H : c == 'S' ? S : V; } };
HSV3 hsv_get(uint32_t col, const MemoKey* key) {
    int r, g, b, a; rgb_of(col, r, g, b, a);
    double h, sat, v; rgb_to_hsv(r, g, b, h, sat, v);
    if (key) {
        auto it = g_memo.find(*key);
        if (it != g_memo.end() && it->second.col == col) return { it->second.h, it->second.s, (int)lfloor(v * 255 + 0.5) };
    }
    return { (int)lua_mod(lfloor(h / 360 * 255 + 0.5), 256.0), (int)lfloor(sat * 255 + 0.5), (int)lfloor(v * 255 + 0.5) };
}
HSV3 hsv_raw(uint32_t col) { return hsv_get(col, nullptr); }
uint32_t hsv_set(uint32_t col, char comp, int val, const MemoKey* key) {
    HSV3 c = hsv_get(col, key);
    int h = c.H, sat = c.S, v = c.V;
    if (comp == 'H') h = val; else if (comp == 'S') sat = val; else v = val;
    int r0, g0, b0, a; rgb_of(col, r0, g0, b0, a);
    const double hd = std::min(h / 255.0 * 360, 359.999);       // 255 stays 255 (360 would read back as 0)
    int r, g, b; hsv_to_rgb(hd, sat / 255.0, v / 255.0, r, g, b);
    const uint32_t out = pack_rgba(r, g, b, a);
    if (key) g_memo[*key] = { h, sat, out };
    return out;
}
MemoKey key_of(const NodeRef& cc) { return { cc.get(), {} }; }
MemoKey key_of(const std::string& name) { return { nullptr, name }; }

// ---------------------------------------------------------------- pages as the editor holds them
struct Marks {                                                  // edit.marks_fn(): col[cc] / mat[cc], counts, saved_of
    std::unordered_set<const Node*> col, mat;
    int ncol = 0, nmat = 0;
    std::unordered_map<const Node*, NodeRef> saved_of;
};
struct Palette;
struct Entry {
    int id = -1;
    std::string key;
    bool slider = false, choice = false;
    std::string title, desc;
    int mn = 0, mx = 0;
    bool has_value = false; int value = 0;
    bool has_verb = false; std::string verb;
    bool nav = false; int nav_gen = 0;                          // edit.nav_fns[cb]: the table is new at every build, so only this build's rows are in it
    std::function<ButtonResult()> cb_button;
    std::function<void(int)> cb_value;
    std::function<void(int)> apply;
    std::function<void()> restore;
    std::function<int()> reset_value;
    TargetsRef targets;
    std::function<bool(const Marks&)> mark_fn;
    int look = 0;                                               // RowLook::kind: 1 hue slider (the rainbow, 0.20.0), 2 palette row (0.21.0)
    int line = 0;                                               // palette row: its line of the grid (0-based)
    std::shared_ptr<Palette> pal;                               // palette row: the colour it sets
    bool has_echo = false; int echo = 0;                        // a value set in place: the engine's read-back is not a move
};
using EntryRef = std::shared_ptr<Entry>;
void note_entry(const EntryRef& e);
struct Group {
    int id = -1;
    std::string key, title, desc;
    bool has_parent = false; std::string parent_key;
    bool slider_page = false;
    std::vector<EntryRef> entries;
    EntryRef button(const std::string& key_, const std::string& title_, const std::string& desc_, std::function<ButtonResult()> cb) {
        auto e = std::make_shared<Entry>();
        e->key = key_; e->title = title_; e->desc = desc_; e->cb_button = std::move(cb);
        e->id = be.opt_button ? be.opt_button(id, key_, title_, desc_) : -1;
        entries.push_back(e);
        note_entry(e);
        return e;
    }
    EntryRef slider(const std::string& key_, const std::string& title_, const std::string& desc_, int mn_, int mx_, int init) {
        auto e = std::make_shared<Entry>();
        e->key = key_; e->slider = true; e->title = title_; e->desc = desc_; e->mn = mn_; e->mx = mx_; e->has_value = true; e->value = init;
        e->id = be.opt_slider ? be.opt_slider(id, key_, title_, desc_, mn_, mx_, init) : -1;
        entries.push_back(e);
        note_entry(e);
        return e;
    }
    EntryRef choice_row(const std::string& key_, const std::string& title_, const std::string& desc_, const std::vector<std::string>& options, int init) {
        auto e = std::make_shared<Entry>();
        e->key = key_; e->choice = true; e->title = title_; e->desc = desc_; e->has_value = true; e->value = init;
        e->id = be.opt_choice ? be.opt_choice(id, key_, title_, desc_, options, init) : -1;
        entries.push_back(e);
        note_entry(e);
        return e;
    }
};
using GroupRef = std::shared_ptr<Group>;

struct Status { std::string text; int64_t until = 0; bool close = false, restore = false; };
using Chip = std::shared_ptr<std::array<uint32_t, 2>>;          // a shared table: the squares page aliases edit.chip, as the Lua does

struct DiRootRows { EntryRef c1, c2, reset; };
struct DiSaved { std::optional<uint32_t> c1, c2; };
struct EditState {
    bool active = false;
    bool di = false;
    // the DI colour menu (edit.di_*)
    int di_fighter = 0, di_costume = 0, di_side = 1;
    bool di_dirty = false, drv_dirty = false, di_at_select = false;
    bool has_di_saved = false; DiSaved di_saved;
    std::shared_ptr<DiRootRows> di_root_rows;
    std::function<std::string(int)> di_color_title;
    bool has_di_blk_of = false; std::map<std::string, int> di_blk_of;
    EntryRef di_preset_entry;
    bool has_di_show_at = false; int64_t di_show_at = 0;
    bool has_di_refresh_at = false; int64_t di_refresh_at = 0;
    bool di_rebuild = false;
    NodeRef tree, original, saved;
    int fighter = 0, costume = 0, color_id = 0;
    bool has_entry = false; engine::Entry entry;
    Chip chip, saved_chip;
    bool dirty = false;
    std::vector<GroupRef> groups;
    std::map<std::string, int> focus_of;
    bool has_page = false; std::string page;
    bool opened = false;
    bool built_pad = false;
    std::map<std::string, std::pair<std::string, std::string>> pages;   // key -> { prev, next }
    std::map<std::string, std::unordered_set<std::string>> slot_union;  // "<fighter>_<costume>" -> { "pi_ci_k" }
    bool marks_on = false;
    // titles, status
    bool has_title_override = false; std::string title_override;
    bool has_title_hold = false; int64_t title_hold = 0;
    bool has_status = false; std::vector<Status> status;
    // the dialog
    Obj cm_new = nullptr;
    bool has_frame_due = false; int64_t frame_due = 0;
    Obj rows_cm = nullptr; int rows_n = 0;
    bool di_simple = false;
    // slider pages
    bool page_backup = false;
    bool has_entry_values = false; std::map<std::string, std::optional<int>> entry_values;
    bool selected = false; bool has_selected_key = false; std::string selected_key; TargetsRef selected_targets;
    bool has_sel_index = false; int sel_index = 0;
    bool lr_was_down = false; int64_t lr_since = 0;               // the palette cursor's LEFT / RIGHT (0.21.0)
    bool sliders_armed = false, validate_was_down = false, reset_was_down = false, copy_was_down = false, copy_color_was_down = false, paste_was_down = false;
    bool has_clip = false; int clip = 0;
    bool has_clip_kind = false; std::string clip_kind;
    bool has_clip_color = false; HSV3 clip_color;
    std::weak_ptr<Entry> pal_focus;                             // the row under the cursor last frame (palette rows, 0.21.0)
    // pulse
    bool hl_on = false; TargetsRef hl_targets;
    bool has_hl_t = false; double hl_t = 0;
    bool has_hl_k = false; double hl_k = 0;
    // guide
    bool has_guide_wanted = false; std::string guide_wanted;
    bool has_guide_desc = false; std::string guide_desc;
    bool hints_shown = false;
    // Esc, page turn, close
    bool cancel_was_down = false, prev_was_down = false, next_was_down = false;
    bool has_esc_nav_frame = false; int64_t esc_nav_frame = 0;
    bool has_closed_at = false; int64_t closed_at = 0;
    bool has_reopen_deadline = false; int64_t reopen_deadline = 0;
    bool empty_purged = false;
};
EditState edit;
bool g_reopen_now = false;                                      // fsm.reopen_now
bool g_grid_page = false;                                       // the page on top holds the palette rows (0.21.0)
bool ll_entry() { return false; }                               // edit.entry.ll: the LL entries are disabled (entries_for)

// Every entry made while the window is up stays reachable by its id: a page under the current one still shows the
// rows of an earlier build, and the engine names them (the wrapper keeps its `dead` entries until the window closes).
std::unordered_map<int, EntryRef> g_by_id;
int g_nav_gen = 0;                                              // edit.nav_fns = {} at every editor_build_pages
bool is_nav(const EntryRef& e) { return e && e->nav && e->nav_gen == g_nav_gen; }
EntryRef entry_by_id(int id) {
    if (id < 0) return nullptr;
    auto it = g_by_id.find(id);
    return it == g_by_id.end() ? nullptr : it->second;
}
void note_entry(const EntryRef& e) { if (e->id >= 0) g_by_id[e->id] = e; }
EntryRef focused_entry() { return entry_by_id(be.opt_focused ? be.opt_focused() : -1); }
bool window_open() { return be.opt_window_open && be.opt_window_open(); }

// ---------------------------------------------------------------- files, catalogue, squares
std::string override_path(const std::string& key) { return std::string(DATA_DIR) + "overrides/" + key + ".user.2"; }
NodeRef read_tree(const std::string& rel) {                     // :346
    std::vector<uint8_t> bytes;
    if (!be.file_read || !be.file_read(rel, bytes) || bytes.empty()) return nullptr;
    auto r = rsz::parse(bytes);
    if (!r.root) journal("parse error " + rel + ": " + r.error);
    return r.root;
}
NodeRef override_tree(const std::string& key) { return key.empty() ? nullptr : read_tree(override_path(key)); }   // :355 (no cache here: the file is the cache)
bool delete_file(const std::string& rel) { return be.file_delete && be.file_delete(rel); }                         // dbg.delete_file
bool set_override(const std::string& key, const NodeRef& tree) {                                                   // :372
    if (key.empty()) return false;
    const std::string path = override_path(key);
    std::vector<uint8_t> bytes;
    if (tree) bytes = rsz::serialize(tree);
    if (!be.file_write || !be.file_write(path, bytes)) return false;
    if (!tree) delete_file(path);
    return true;
}
NodeRef entry_tree(const engine::Entry& e, bool raw) {          // :391
    if (!raw && !e.okey.empty()) if (auto ov = override_tree(e.okey)) return ov;
    return read_tree(e.path);
}
bool write_file(const std::string& rel, const NodeRef& tree) {  // :2251
    return be.file_write && be.file_write(rel, rsz::serialize(tree));
}
void build_catalog() {                                          // :274 (the DLL's own list; the Lua's follows through catalog_changed)
    g_catalog.build(g_data_dir);
    g_catalog_built = true;
    if (be.catalog_changed) be.catalog_changed();
}
void invalidate_caches() { if (be.caches_invalidated) be.caches_invalidated(); }   // :2247 (the Lua's e.tree / e.chip / overrides)
const std::vector<engine::Entry>* entries_for(int fighter, int costume) {
    if (!g_catalog_built) { g_catalog.build(g_data_dir); g_catalog_built = true; }
    return g_catalog.entries(fighter, costume);
}
const char* const CHIP_FILE = "SF6_ColorSpinExtra_data/chip_slots.json";
nlohmann::json chip_cfg_load() {                                // the Lua's chip_cfg table = that file
    std::vector<uint8_t> bytes;
    if (be.file_read && be.file_read(CHIP_FILE, bytes)) {
        auto j = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
        if (j.is_object()) { j.erase("seen"); return j; }
    }
    return nlohmann::json::object();
}
void chip_cfg_save(const nlohmann::json& j) {
    const std::string text = j.dump(4);
    if (be.file_write) be.file_write(CHIP_FILE, std::vector<uint8_t>(text.begin(), text.end()));
}
std::string edit_okey() {                                       // :2212
    if (edit.has_entry) return edit.entry.okey;
    return fmt("%d_%d_game_%d", edit.fighter, edit.costume, edit.color_id);
}
std::string pristine_path(int fighter, int costume, int id) { return fmt("%spristine/%d_%d_game_%d.user.2", DATA_DIR, fighter, costume, id); }
// the fixed name, else the game's (fighters.hpp); "" when unknown
std::string fighter_name(int id) { return fighters::name_or_empty(id); }
// next free MANUALCOLORS_NN/esfXXX_YYY_cmd_NNN.user.2 for a fighter / costume (:2153)
bool manual_path(int fighter, int costume, std::string& rel) {
    const std::string nm = fighter_name(fighter);
    const std::string folder = fmt("%scolors/%s_esf%03d/", DATA_DIR, (nm.empty() ? fmt("Fighter%d", fighter) : nm).c_str(), fighter);
    std::unordered_set<std::string> existing;
    if (auto* list = entries_for(fighter, costume)) for (auto& e : *list) existing.insert(backslashes(e.path));
    for (int pack = 1; pack <= 99; ++pack) {
        for (int n = 1; n <= 10; ++n) {
            rel = fmt("%s%s_%02d/esf%03d_%03d_cmd_%03d.user.2", folder.c_str(), MANUAL_DIR, pack, fighter, costume + 1, n);
            if (!existing.count(backslashes(rel))) return true;
        }
    }
    return false;
}

void editor_refresh() { if (be.refresh) be.refresh(); }         // :2182
void editor_build_pages();

// ---------------------------------------------------------------- marks (:2777-2819)
int mval(const NodeRef& cc, const char* name) {
    NodeRef n = opt_node(cc, name);
    return n ? (int)lfloor(node_value(n) * 1000 + 0.5) : -1;
}
HSV3 hsv3(uint32_t c, const MemoKey& key) { return hsv_get(c, &key); }
bool col_diff(const NodeRef& cc, const NodeRef& scc) {
    if (ll_entry()) return false;
    if (!scc) return true;
    if (cc_color(cc) != cc_color(scc)) return true;
    const HSV3 a = hsv3(cc_color(cc), key_of(cc)), b = hsv_raw(cc_color(scc));   // a hue set on a grey: same RGB, different slider
    return a.H != b.H || a.S != b.S || a.V != b.V;
}
bool mat_diff(const NodeRef& cc, const NodeRef& scc, const char* name) { return !ll_entry() && (!scc || mval(cc, name) != mval(scc, name)); }
Marks marks() {                                                 // edit.marks_fn()
    Marks m;
    if (!edit.saved) return m;
    each_cc(edit.tree, [&](int pi, int ci, int k, const NodeRef&, const NodeRef&, const NodeRef& cc) {
        if (!cc) return;
        NodeRef scc = cc_at(edit.saved, pi, ci, k);
        m.saved_of[cc.get()] = scc;
        if (node_enable(cc)) {
            if (col_diff(cc, scc)) { m.col.insert(cc.get()); ++m.ncol; }
            if (mat_diff(cc, scc, "BlendRate") || mat_diff(cc, scc, "Roughness") || mat_diff(cc, scc, "Metalness")) { m.mat.insert(cc.get()); ++m.nmat; }
        }
    });
    return m;
}
bool chip_changed() {
    const Chip& c = edit.chip; const Chip& sc = edit.saved_chip;
    if ((c == nullptr) != (sc == nullptr)) return true;
    return c && sc && ((*c)[0] != (*sc)[0] || (*c)[1] != (*sc)[1]);
}

// find_name (:717): the control itself, its children, then its next siblings
Obj find_name(Obj ctrl, const std::string& name, int depth) {
    if (!ctrl || depth > 8) return nullptr;
    if (be.ui_name && be.ui_name(ctrl) == name) return ctrl;
    Obj c = be.ui_child ? be.ui_child(ctrl) : nullptr;
    if (Obj f = c ? find_name(c, name, depth + 1) : nullptr) return f;
    Obj nx = be.ui_next ? be.ui_next(ctrl) : nullptr;
    return nx ? find_name(nx, name, depth) : nullptr;
}

// ---------------------------------------------------------------- the actions (:2386-2522)
std::string focus_root_key() { return ROOT; }
int focus_of_or0(const std::string& key) { auto it = edit.focus_of.find(key); return it == edit.focus_of.end() ? 0 : it->second; }
void mark_saved() {                                             // edit.saved = copy_tree(edit.tree); edit.saved_chip = ...; edit.dirty = false
    edit.saved = copy_tree(edit.tree);
    edit.saved_chip = edit.chip ? std::make_shared<std::array<uint32_t, 2>>(*edit.chip) : nullptr;
    edit.dirty = false;
}
void save_chip(const std::string& key_path) {                   // :2386
    const std::string key = backslashes(key_path);
    if (!edit.chip && !edit.has_entry) {                        // game colour saved as MC: its own pair travels with it
        uint32_t g0 = 0, g1 = 0;
        if (be.game_chips && be.game_chips(edit.fighter, edit.costume, edit.color_id, g0, g1)) edit.chip = std::make_shared<std::array<uint32_t, 2>>(std::array<uint32_t, 2>{ g0, g1 });
    }
    auto cfg = chip_cfg_load();
    if (edit.chip) cfg[key] = { (*edit.chip)[0], (*edit.chip)[1] }; else cfg.erase(key);
    chip_cfg_save(cfg);
}
void refresh_list(bool has_path, const std::string& path) { if (be.screen_refresh_list) be.screen_refresh_list(has_path, has_path ? backslashes(path) : std::string()); }   // :2395
void back_to_root_page() {                                      // edit.page = ROOT; title_hold; build; navigate (root replaces the confirm page + the stale root under it)
    edit.has_page = true; edit.page = ROOT; edit.has_title_hold = true; edit.title_hold = frame_now() + 15;
    editor_build_pages();
    if (be.opt_navigate) be.opt_navigate(ROOT, kTrue, focus_of_or0(ROOT), kFalse, kFalse, kTrue);
}
void set_status(const std::string& text, int frames, bool close, bool restore) {
    edit.has_title_override = true; edit.title_override = text;
    edit.has_status = true; edit.status.clear(); edit.status.push_back({ text, frame_now() + frames, close, restore });
}
ButtonResult editor_save_as() {                                 // :2400
    std::string rel;
    if (!manual_path(edit.fighter, edit.costume, rel) || !write_file(rel, edit.tree)) { ButtonResult r; r.kind = ButtonResult::Close; return r; }
    save_chip(rel);
    build_catalog(); invalidate_caches();
    refresh_list(true, rel);
    std::string label; bool has_label = false;
    const std::string want = backslashes(rel);
    if (auto* list = g_catalog.entries(edit.fighter, edit.costume)) for (auto& e : *list) if (backslashes(e.path) == want) { label = e.label; has_label = true; }
    // title: "SAVED AS MC n" for 1.5 s, then the window closes (the list reload needs it closed) and reopens on the new colour
    mark_saved();
    set_status(L("status.saved_as", { has_label ? label : std::string("MC") }), 90, true, false);
    g_reopen_now = true;
    back_to_root_page();
    return {};
}
ButtonResult editor_save() {                                    // :2419
    if (edit.has_entry && edit.entry.manual) {                  // MC: overwrite the file itself
        if (write_file(edit.entry.path, edit.tree)) save_chip(edit.entry.path);
    } else {                                                    // RW pack file or game colour: override
        set_override(edit_okey(), edit.tree);
        if (edit.has_entry) save_chip(edit.entry.path);
    }
    invalidate_caches();
    // window stays open: title "SAVING..." then "SAVE OK!", back to the root page
    mark_saved();
    set_status(L("status.save_ok"), 120, false, true);
    back_to_root_page();
    return {};
}
ButtonResult editor_reset() {                                   // :2437 drop the override, back to the original
    g_memo.clear(); g_memo_keep.clear();                        // the page shows the true values of the restored colours
    const bool manual = edit.has_entry && edit.entry.manual;
    if (!manual) set_override(edit_okey(), nullptr);
    edit.tree = copy_tree(edit.original);
    if (!edit.has_entry && be.apply_original) be.apply_original(edit.color_id, edit.original);   // game colour: the live data itself goes back to pristine
    if (manual) {                                               // MC: the squares saved with the file (last SAVE)
        edit.chip = edit.saved_chip ? std::make_shared<std::array<uint32_t, 2>>(*edit.saved_chip) : nullptr;
    } else {                                                    // RW / game colour: the original squares
        edit.chip = nullptr;
        if (edit.has_entry && !edit.entry.path.empty()) { auto cfg = chip_cfg_load(); cfg.erase(backslashes(edit.entry.path)); chip_cfg_save(cfg); }
    }
    invalidate_caches();
    editor_refresh();
    // window stays open: "RESETTING..." then "RESET OK!", back to the root page (rebuilt: fresh swatches)
    mark_saved();
    set_status(L("status.reset_ok"), 120, false, true);
    back_to_root_page();
    return {};
}
ButtonResult editor_erase() {                                   // :2462 MC: the file goes (emptied, then deleted); others: reset
    if (edit.has_entry && edit.entry.manual) {
        const engine::Entry e = edit.entry;
        if (be.file_write) be.file_write(e.path, {});
        delete_file(e.path);
        { auto cfg = chip_cfg_load(); cfg.erase(backslashes(e.path)); chip_cfg_save(cfg); }
        set_override(e.okey, nullptr);
        build_catalog(); invalidate_caches();
        refresh_list(false, {});
        // "ERASED MC n" for 1.5 s, then the window closes (the list reload needs it closed)
        set_status(L("status.erased", { e.label.empty() ? std::string("MC") : e.label }), 90, true, false);
        back_to_root_page();
        return {};
    }
    return editor_reset();
}
// every editable slot = enabled in this colour OR in any game colour of the costume (:2488)
template <class F> void each_editable(F fn) {
    static const std::unordered_set<std::string> none;
    auto it = edit.slot_union.find(fmt("%d_%d", edit.fighter, edit.costume));
    const auto& uni = it == edit.slot_union.end() ? none : it->second;
    each_cc(edit.tree, [&](int pi, int ci, int k, const NodeRef&, const NodeRef&, const NodeRef& cc) {
        if (cc && (node_enable(cc) || uni.count(fmt("%d_%d_%d", pi, ci, k)))) fn(cc);
    });
}
ButtonResult open_result(const char* key) { ButtonResult r; r.kind = ButtonResult::Open; r.open = key; r.rebuild = kTrue; return r; }
ButtonResult editor_random() {                                  // :2498
    each_editable([](const NodeRef& cc) {
        node_set_enable(cc, true);
        const uint32_t b = (uint32_t)be.rand_int(0, 255), g = (uint32_t)be.rand_int(0, 255), r = (uint32_t)be.rand_int(0, 255);
        cc_set_color(cc, 0xFF000000u + b * 0x10000u + g * 0x100u + r);
    });
    editor_refresh();
    editor_build_pages();                                       // definitions; the engine units are rebuilt after End()
    edit.has_page = true; edit.page = "__coloredit_col";
    return open_result("__coloredit_col");
}
ButtonResult editor_random_materials() {                        // :2506
    each_editable([](const NodeRef& cc) {
        if (!obj_of(cc, "Option")) return;
        node_set_enable(cc, true);
        for (const char* nm : { "BlendRate", "Roughness", "Metalness" }) {
            NodeRef node = opt_node(cc, nm);
            if (node) { node_set_enable(node, true); node_set_value(node, be.rand_float()); }
        }
    });
    editor_refresh();
    editor_build_pages();
    edit.has_page = true; edit.page = "__coloredit_mat";
    return open_result("__coloredit_mat");
}

// ---------------------------------------------------------------- editor_build_pages (:2749-3211)
struct Cluster {
    NodeRef cl; std::vector<int> enabled; std::string id; int pi = 0, ci = 0, ptype = 0; std::string label;
};
std::string swatch(uint32_t col) { return fmt("<color %s>%s</color>", rgb_hex(col).c_str(), SQUARE); }
// one square: a row that edits ONE colour (its H / S / V rows, the Drive page's Couleur 1 / Couleur 2)
std::string swatch_one(uint32_t col) { return fmt("<color %s>\xE2\x96\xA0</color>", rgb_hex(col).c_str()); }
std::string base_name(std::string n) {                          // quick edit: the cluster name without its decorations, lower case
    n = lower(n);
    strip_prefix(n, "unused_"); strip_esf(n);
    strip_suffix(n, "_pdo"); strip_suffix(n, "_noalpha"); strip_suffix(n, "_notwoside"); strip_suffix(n, "_twoside");
    return n;
}
std::string human(std::string n) {                              // "esf_ClothA_Jacket_chest" -> "JACKET CHEST"
    strip_prefix(n, "UnUsed_"); strip_esf(n);
    if (starts(n, "Cloth") || starts(n, "cloth")) {             // gsub("^[Cc]loth[AB]?_", "")
        size_t i = 5;
        if (i < n.size() && (n[i] == 'A' || n[i] == 'B')) ++i;
        if (i < n.size() && n[i] == '_') n.erase(0, i + 1);
    }
    strip_prefix(n, "Misc_");
    strip_suffix(n, "_PDO"); strip_suffix(n, "_NoAlpha"); strip_suffix(n, "_NoTwoSide"); strip_suffix(n, "_TwoSide");
    if ((starts(n, "Costume") || starts(n, "costume")) && n.size() > 7) {   // ^[Cc]ostume(%d+)$
        bool digits = true;
        for (size_t i = 7; i < n.size(); ++i) if (!is_digit(n[i])) digits = false;
        if (digits) return "OUTFIT " + std::to_string(std::stol(n.substr(7)) + 1);
    }
    std::string s;                                              // gsub("(%l)(%u)", "%1 %2")
    for (size_t i = 0; i < n.size(); ++i) {
        s += n[i];
        if (i + 1 < n.size() && n[i] >= 'a' && n[i] <= 'z' && n[i + 1] >= 'A' && n[i + 1] <= 'Z') s += ' ';
    }
    for (auto& c : s) if (c == '_') c = ' ';
    while (!s.empty() && is_digit(s.back())) s.pop_back();      // gsub("%d+$", "")
    s = upper(s);
    size_t a = 0; while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size(); while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
bool has_any(const std::string& n, std::initializer_list<const char*> list) { for (auto* k : list) if (n.find(k) != std::string::npos) return true; return false; }

GroupRef new_group(const std::string& title, const std::string& desc, const std::string& key, const char* parent) {
    auto g = std::make_shared<Group>();
    g->key = key; g->title = title; g->desc = desc;
    g->id = be.opt_group ? be.opt_group(key, title, desc) : -1;
    if (parent) { g->has_parent = true; g->parent_key = parent; }
    edit.groups.push_back(g);
    return g;
}
// remember which row took us down, then open the page on top of this one (:2762)
std::function<ButtonResult()> nav(const std::string& key) {
    return [key]() {
        if (EntryRef e = focused_entry()) {
            for (auto& g : edit.groups) for (size_t i = 0; i < g->entries.size(); ++i) if (g->entries[i] == e) edit.focus_of[g->key] = (int)i;
        }
        edit.has_page = true; edit.page = key;
        editor_build_pages();                                   // titles (swatches) from the current tree
        ButtonResult r; r.kind = ButtonResult::Open; r.open = key; r.rebuild = kTrue; r.push = kTrue;   // the parent page stays alive underneath
        return r;
    };
}
EntryRef nav_button(const GroupRef& g, const std::string& key, const std::string& title, const std::string& desc, const std::string& to) {
    EntryRef e = g->button(key, title, desc, nav(to));
    e->nav = true; e->nav_gen = g_nav_gen;
    return e;
}
// root / cluster rows: "something below changed" (:2811)
void marked(const EntryRef& e, const char* kind, TargetsRef targets) {
    const std::string k = kind;
    e->mark_fn = [k, targets](const Marks& m) {
        if (targets) {
            for (auto& cc : *targets) if ((k == "col" && m.col.count(cc.get())) || (k == "mat" && m.mat.count(cc.get()))) return true;
            return false;
        }
        if (k == "col") return m.ncol > 0;
        if (k == "mat") return m.nmat > 0;
        return chip_changed();
    };
}
void set_selected(const EntryRef& e) { edit.selected = true; edit.has_selected_key = true; edit.selected_key = e->key; edit.selected_targets = e->targets; }
void retitle_row(const EntryRef& e, const std::string& want) {  // the row's own title text, when it differs
    Obj ctrl = be.opt_row_control ? be.opt_row_control(e->id, nullptr) : nullptr;
    Obj t = ctrl ? find_name(ctrl, "e_text_title", 0) : nullptr;
    if (t && be.ui_message(t) != want) be.ui_set_text(t, want);
}
struct Row { std::weak_ptr<Entry> entry; char comp = 0; };

// The palette rows of one colour (0.21.0): five choice rows of ten values under its three sliders, holders of the
// navigation only (the game side hides them and draws the grid). The column under the cursor is shared by the five
// rows, so that UP / DOWN keeps it: the game's own row navigation is the grid's second axis (no 2D input).
struct Palette {
    MemoKey mk; int col = 0;
    std::shared_ptr<std::vector<Row>> rows;                     // the colour's H / S / V rows
    std::function<uint32_t()> get_col;                          // the colour edited (a costume colour, a Drive colour slot)
    std::function<void(uint32_t)> set_col;                      // set it, the way its sliders do (live preview included)
    std::function<void(const EntryRef&, char, uint32_t)> retitle;   // one of its sliders' titles, for that colour
};
uint32_t rgb_of_col(uint32_t col) { int r, g, b, a; rgb_of(col, r, g, b, a); return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
std::pair<int, int> palette_hit(uint32_t col) {                 // where the colour is in the palette (RGB, alpha aside); -1 none
    const uint32_t rgb = rgb_of_col(col);
    for (int l = 0; l < kPaletteLines; ++l) for (int c = 0; c < kPaletteCols; ++c) if (kPalette[l][c] == rgb) return { l, c };
    return { -1, -1 };
}
void set_row_value(const EntryRef& e, int v) {                  // a row's value in place (choice 1-based); its read-back is not a move
    if (!e || (e->has_value && e->value == v)) return;
    e->value = v; e->has_value = true;
    if (be.opt_set_value && be.opt_set_value(e->id, e->choice ? v - 1 : v)) { e->has_echo = true; e->echo = v; }
}
void pick_swatch(const std::shared_ptr<Palette>& p, int line, int col, const EntryRef& self) {   // the cell becomes the colour, live
    if (!p || line < 0 || line >= kPaletteLines || col < 0 || col >= kPaletteCols) return;
    if (self) set_selected(self);
    const uint32_t rgb = kPalette[line][col];
    int r, g, b, a; rgb_of(p->get_col(), r, g, b, a);
    const uint32_t c = pack_rgba((int)(rgb >> 16), (int)((rgb >> 8) & 0xFF), (int)(rgb & 0xFF), a);
    g_memo.erase(p->mk);                                        // the sliders show the cell's true values
    p->set_col(c);
    const HSV3 hv = hsv_raw(c);
    for (auto& row : *p->rows) if (auto re = row.entry.lock()) { set_row_value(re, hv.get(row.comp)); p->retitle(re, row.comp, c); }
}
// the five palette rows of one colour page, after its three sliders (one colour per page: 8 rows); restore = Backspace
void palette_rows(const GroupRef& g, const std::string& prefix, const std::shared_ptr<Palette>& pal, std::function<void()> restore, const TargetsRef& targets) {
    const auto hit = palette_hit(pal->get_col());
    pal->col = hit.second >= 0 ? hit.second : 0;
    for (int line = 0; line < kPaletteLines; ++line) {
        // ONE value: the row never moves on LEFT / RIGHT (the editor moves the cursor); its text is the line's colours,
        // shown only without the grid
        std::string line_text;
        for (int c = 0; c < kPaletteCols; ++c) line_text += fmt("<color %06X>\xE2\x96\xA0</color>", kPalette[line][c]);
        EntryRef e = g->choice_row(fmt("%s_p%d", prefix.c_str(), line + 1), std::string(), "", { line_text }, 1);
        e->look = 2; e->line = line; e->pal = pal;
        e->restore = restore;
        e->targets = targets;
    }
}

// colour sliders (H / S / V) of one colour, with the live swatch in the titles (:2824)
void color_rows(const GroupRef& g, int k, const NodeRef& cc, const NodeRef& occ, const Marks& m0) {
    auto rows = std::make_shared<std::vector<Row>>();
    auto sit = m0.saved_of.find(cc.get());
    NodeRef scc = sit == m0.saved_of.end() ? nullptr : sit->second;
    auto title_of = [k, cc](char comp, uint32_t col) {
        const std::string name = comp == 'H' ? L("slider.hue") : comp == 'S' ? L("slider.saturation") : L("slider.brightness");
        return L("slider.color_row", { std::to_string(k), name, swatch_one(col) }) + (node_enable(cc) ? std::string() : ("  " + L("slider.texture")));
    };
    auto set_col = [cc, occ](uint32_t v) {                      // a texture slot (Enable false) becomes ours when coloured
        cc_set_color(cc, v);
        node_set_enable(cc, !(occ && !node_enable(occ) && v == cc_color(occ)));
    };
    const MemoKey mk = key_of(cc);
    g_memo_keep.push_back(cc);
    const HSV3 init = hsv_get(cc_color(cc), &mk);
    for (char comp : { 'H', 'S', 'V' }) {
        EntryRef e = g->slider(fmt("c%d_%c", k, comp), title_of(comp, cc_color(cc)), "", 0, 255, init.get(comp));
        if (comp == 'H') e->look = 1;                           // the rainbow under it (dialogs/row_look)
        std::weak_ptr<Entry> we = e;
        e->cb_value = [we, cc, comp, mk, set_col, title_of, rows](int v) {
            if (auto self = we.lock()) set_selected(self);
            set_col(hsv_set(cc_color(cc), comp, v, &mk));
            editor_refresh();
            for (auto& row : *rows) if (auto re = row.entry.lock()) retitle_row(re, title_of(row.comp, cc_color(cc)));
        };
        e->apply = [cc, comp, mk, set_col, scc](int v) {        // data only (Backspace / paste)
            set_col(hsv_set(cc_color(cc), comp, v, &mk));
            if (scc && cc_color(cc) == cc_color(scc)) g_memo.erase(mk);   // back to the saved colour: true values
        };
        e->reset_value = [cc, comp, scc]() { return hsv_raw(scc ? cc_color(scc) : cc_color(cc)).get(comp); };   // this component of the SAVED colour
        e->targets = std::make_shared<std::vector<NodeRef>>(std::vector<NodeRef>{ cc });
        e->mark_fn = [cc, comp, mk, scc](const Marks&) { return !ll_entry() && (!scc || hsv3(cc_color(cc), mk).get(comp) != hsv_raw(cc_color(scc)).get(comp)); };
        rows->push_back({ e, comp });
    }
    // the palette (0.21.0): five rows of ten values under the three sliders (one colour per page: 8 rows)
    auto pal = std::make_shared<Palette>();
    pal->mk = mk; pal->rows = rows;
    pal->get_col = [cc]() { return cc_color(cc); };
    pal->set_col = [set_col](uint32_t c) { set_col(c); editor_refresh(); };
    pal->retitle = [title_of](const EntryRef& re, char comp, uint32_t c) { retitle_row(re, title_of(comp, c)); };
    palette_rows(g, fmt("c%d", k), pal, [pal, scc]() { if (scc) { g_memo.erase(pal->mk); pal->set_col(cc_color(scc)); } },   // Backspace: the SAVED colour
                 std::make_shared<std::vector<NodeRef>>(std::vector<NodeRef>{ cc }));
}
// material sliders (BlendRate / Roughness / Metalness, 0-1000 = 0.0-1.0) of one colour (:2859)
void material_rows(const GroupRef& g, int k, const NodeRef& cc, const NodeRef& /*occ*/, const Marks& m0) {
    if (!obj_of(cc, "Option")) return;
    auto sit = m0.saved_of.find(cc.get());
    NodeRef scc = sit == m0.saved_of.end() ? nullptr : sit->second;
    struct M { const char* loc; const char* name; };
    for (const M& m : { M{ "slider.blend", "BlendRate" }, M{ "slider.rough", "Roughness" }, M{ "slider.metal", "Metalness" } }) {
        NodeRef node = opt_node(cc, m.name);
        if (!node) continue;
        const char* name = m.name;
        EntryRef e = g->slider(fmt("m%d_%s", k, name), fmt("Color %d  %s", k, L(m.loc).c_str()), "", 0, 1000, (int)lfloor(node_value(node) * 1000 + 0.5));
        std::weak_ptr<Entry> we = e;
        e->cb_value = [we, node, cc](int v) {
            if (auto self = we.lock()) set_selected(self);
            node_set_value(node, v / 1000.0); node_set_enable(node, true); node_set_enable(cc, true);
            editor_refresh();
        };
        e->mark_fn = [cc, scc, name](const Marks&) { return mat_diff(cc, scc, name); };
        e->apply = [node, cc](int v) { node_set_value(node, v / 1000.0); node_set_enable(node, true); node_set_enable(cc, true); };   // data only (Backspace / Y)
        e->reset_value = [node, scc, name]() { int sv = mval(scc, name); if (sv < 0) sv = (int)lfloor(node_value(node) * 1000 + 0.5); return sv; };
        e->targets = std::make_shared<std::vector<NodeRef>>(std::vector<NodeRef>{ cc });
    }
}
using RowsFn = void (*)(const GroupRef&, int, const NodeRef&, const NodeRef&, const Marks&);

// The keyboard keys that turn pages: the game's own, so the layout in use counts (the physical key left of W / Z is
// A on an AZERTY keyboard, Q on a QWERTY one, where A moves left in the game's menus). Unknown: A / E as before.
struct PageKey { std::string symbol; int vk; };
PageKey page_key(bool next) {
    PageKey k{ next ? "<ICON KeyE>" : "<ICON KeyA>", next ? 0x45 : 0x41 };
    std::string s;
    int vk = 0;
    if (be.page_key && be.page_key(next, s, vk) && !s.empty() && vk > 0) k = { s, vk };
    return k;
}

std::string page_title(const std::string& title, int pg, int n) {
    if (n <= 1) return title;
    const bool pad = be.screen_pad && be.screen_pad();
    const std::string prev = pad ? "<ICON BtnL1>" : page_key(false).symbol, next = pad ? "<ICON BtnR1>" : page_key(true).symbol;
    return fmt("%s   %s   %d/%d   %s", prev.c_str(), title.c_str(), pg, n, next.c_str());
}
void link_pages(const std::vector<std::string>& keys) {
    const size_t n = keys.size();
    if (n <= 1) return;
    for (size_t i = 0; i < n; ++i) edit.pages[keys[i]] = { keys[i == 0 ? n - 1 : i - 1], keys[i == n - 1 ? 0 : i + 1] };
}
// level 2 (cluster list, paginated; extra rows on page 1) + level 3 (per_page colours per page) (:2932)
std::string build_mode(const std::vector<Cluster>& clusters, const std::string& mode, const std::string& list_title, const std::string& desc, RowsFn rows_fn,
                       const std::function<void(const GroupRef&)>& extra_first_page, int per_page, const Marks& m0) {
    const std::string first = "__coloredit_" + mode;
    const int nclusters = (int)clusters.size();
    const int budget1 = ROWS - (extra_first_page ? 1 : 0);
    const int total = std::max(1, (nclusters <= budget1) ? 1 : (1 + (int)std::ceil((nclusters - budget1) / (double)ROWS)));
    std::vector<std::string> list_keys;
    int i = 1;
    for (int page_no = 1; page_no <= total; ++page_no) {
        const std::string key = page_no == 1 ? first : (first + "_p" + std::to_string(page_no));
        list_keys.push_back(key);
        GroupRef g = new_group(page_title(list_title, page_no, total), desc, key, ROOT);
        if (page_no == 1 && extra_first_page) extra_first_page(g);
        const int take = std::min(nclusters - i + 1, page_no == 1 ? budget1 : ROWS);
        for (int j = i; j <= i + take - 1; ++j) {
            const Cluster& c = clusters[(size_t)j - 1];
            const std::string ckey = first + "_" + c.id;
            auto targets = std::make_shared<std::vector<NodeRef>>();   // the cluster's colours pulse while focused
            auto* ccs = arr_of(c.cl, "CustomizeColors");
            for (int k : c.enabled) targets->push_back((*ccs)[(size_t)k - 1]);
            EntryRef cb_entry = nav_button(g, "open_" + ckey, L("list.cluster_count", { c.label, std::to_string(c.enabled.size()) }), "", ckey);
            marked(cb_entry, mode.c_str(), targets);
            cb_entry->targets = targets;
            const int n_pages = (int)std::ceil(c.enabled.size() / (double)per_page);
            std::vector<std::string> keys;
            for (int pg = 1; pg <= n_pages; ++pg) {
                const std::string pkey = pg == 1 ? ckey : (ckey + "_" + std::to_string(pg));
                keys.push_back(pkey);
                GroupRef pgrp = new_group(page_title(c.label, pg, n_pages), "", pkey, key.c_str());
                pgrp->slider_page = true;                       // Esc / B cancels, Enter / A validates (the loop)
                for (int idx = (pg - 1) * per_page + 1; idx <= std::min(pg * per_page, (int)c.enabled.size()); ++idx) {
                    const int k = c.enabled[(size_t)idx - 1];
                    rows_fn(pgrp, k, (*ccs)[(size_t)k - 1], cc_at(edit.original, c.pi, c.ci, k), m0);
                }
            }
            link_pages(keys);
        }
        i += take;
    }
    link_pages(list_keys);
    return first;
}

// QUICK EDIT (:2979-3111): up to 7 colour families from the game's own structure, one ABSOLUTE hue slider per family
struct QItem { NodeRef cc; double h = 0, s = 0, v = 0; int a = 0; uint32_t orig = 0; bool orig_enable = false; };
struct QCluster { const Cluster* c = nullptr; std::vector<QItem> items; int n = 0; };
struct QGroup { std::string label; std::vector<std::shared_ptr<QCluster>> cls; bool fixed = false; std::vector<QItem> items; const QItem* rep = nullptr; double center = 0; };
const char* bucket_of(const Cluster& c) {
    static const std::initializer_list<const char*> ACC = { "belt", "obi", "ring", "watch", "bag", "cap", "hat", "glass", "sanglass", "dogtag", "whistle", "chain", "band",
        "strap", "neck", "earing", "earring", "kazari", "tassel", "ribbon", "zipper", "badge", "sign", "threads", "metal", "stone", "gadget", "display", "lens", "camera",
        "kinzoku", "tume", "sensu", "stand", "hankachi" };
    static const std::initializer_list<const char*> BOTTOM = { "pants", "pant", "skirt", "short", "sock", "shoe", "shose", "boot", "sandal", "setta", "waraji", "kyahan",
        "tabi", "leg", "trausers", "trouser", "hakama", "bottom" };
    const std::string n = base_name(cl_name(c.cl));
    const int t = c.ptype;
    if (t == 1) {
        if (n.find("head") != std::string::npos) return "SKIN";
        if (has_any(n, { "hair", "eyebrow", "eyelash", "beard" })) return "HAIR";
        return "FACE";
    } else if (t == 2) return "HAIR";
    else if (t >= 4) return "ACC";
    if (has_any(n, { "body", "head", "skin" })) return "SKIN";
    if (has_any(n, { "hair", "eyebrow", "beard" })) return "HAIR";
    if (has_any(n, { "eye", "mouth", "teeth", "tear" })) return "FACE";
    if (starts(n, "clotha")) return "A";
    if (starts(n, "clothb")) return "B";
    if (has_any(n, ACC)) return "ACC";
    if (has_any(n, BOTTOM)) return "B";
    return "A";
}
void build_quick(const std::vector<Cluster>& clusters, const Marks& m0) {
    std::map<std::string, std::vector<std::shared_ptr<QCluster>>> buckets;
    for (const char* b : { "SKIN", "HAIR", "FACE", "A", "B", "ACC" }) buckets[b];
    for (auto& c : clusters) {
        auto qc = std::make_shared<QCluster>();
        qc->c = &c; qc->n = (int)c.enabled.size();
        auto* ccs = arr_of(c.cl, "CustomizeColors");
        for (int k : c.enabled) {                               // one item per enabled colour
            NodeRef cc = (*ccs)[(size_t)k - 1];
            int r, g, b, a; rgb_of(cc_color(cc), r, g, b, a);
            QItem it; it.cc = cc; rgb_to_hsv(r, g, b, it.h, it.s, it.v); it.a = a; it.orig = cc_color(cc); it.orig_enable = node_enable(cc);
            qc->items.push_back(it);
        }
        buckets[bucket_of(c)].push_back(qc);
    }
    auto count = [](const std::vector<std::shared_ptr<QCluster>>& list) { int n = 0; for (auto& x : list) n += x->n; return n; };
    auto biggest = [](const std::vector<std::shared_ptr<QCluster>>& list) { std::shared_ptr<QCluster> bg; for (auto& x : list) if (!bg || x->n > bg->n) bg = x; return bg; };
    std::vector<std::shared_ptr<QGroup>> groups;
    for (const char* key : { "SKIN", "HAIR", "FACE" }) {
        if (count(buckets[key]) > 0) { auto g = std::make_shared<QGroup>(); g->label = key; g->cls = buckets[key]; g->fixed = true; groups.push_back(g); }
    }
    for (const char* key : { "A", "B", "ACC" }) {
        if (count(buckets[key]) > 0) {
            auto g = std::make_shared<QGroup>();
            g->label = std::string(key) == "ACC" ? "ACCESSORIES" : human(cl_name(biggest(buckets[key])->c->cl));
            g->cls = buckets[key];
            groups.push_back(g);
        }
    }
    int guard = 0;                                              // fill up to 7 rows: biggest cluster -> own row
    while (groups.size() < 7 && guard < 8) {
        ++guard;
        std::shared_ptr<QGroup> pick;
        for (auto& g : groups) if (!g->fixed && g->cls.size() >= 2 && (!pick || g->cls.size() > pick->cls.size())) pick = g;
        if (!pick) break;
        auto big = biggest(pick->cls);
        for (size_t i = 0; i < pick->cls.size(); ++i) if (pick->cls[i] == big) { pick->cls.erase(pick->cls.begin() + (ptrdiff_t)i); break; }
        size_t pos = 0;
        for (size_t i = 0; i < groups.size(); ++i) if (groups[i] == pick) pos = i;
        auto ng = std::make_shared<QGroup>(); ng->label = human(cl_name(big->c->cl)); ng->cls = { big };
        groups.insert(groups.begin() + (ptrdiff_t)pos, ng);
        if (pick->label != "ACCESSORIES") pick->label = human(cl_name(biggest(pick->cls)->c->cl));
    }
    const double PI = 3.14159265358979323846;
    for (auto& g : groups) {                                    // flatten, representative colour, hue centre
        for (auto& x : g->cls) for (auto& it : x->items) g->items.push_back(it);
        const QItem* rep = nullptr;
        if (g->label == "SKIN") {                               // skin tone, not the lips: warm hue, mid value
            for (auto& it : g->items) {
                const bool warm = it.h >= 10 && it.h <= 50 && it.v > 0.35 && it.s < 0.7;
                if (warm && (!rep || it.v > rep->v)) rep = &it;
            }
        }
        if (!rep) for (auto& it : g->items) if (!rep || it.s * it.v > rep->s * rep->v) rep = &it;
        g->rep = rep;
        double sx = 0, sy = 0;
        for (auto& it : g->items) if (it.s > 0.15) { sx = sx + std::cos(it.h * (PI / 180.0)) * it.s; sy = sy + std::sin(it.h * (PI / 180.0)) * it.s; }
        g->center = (sx == 0 && sy == 0) ? (rep ? lfloor(rep->h + 0.5) : 0) : lua_mod(std::atan2(sy, sx) * (180.0 / PI), 360.0);
    }
    GroupRef qe = new_group(L("quick.title"), "", "__coloredit_quick", ROOT);
    qe->slider_page = true;
    int gi = 0;
    for (auto& grp : groups) {
        ++gi;
        if (grp->items.empty()) continue;                       // (a family always has a colour: its clusters have enabled ones)
        const QItem rep = grp->rep ? *grp->rep : grp->items[0];
        const int center0 = (int)lfloor(grp->center + 0.5);
        auto saved_of = std::make_shared<std::unordered_map<const Node*, NodeRef>>(m0.saved_of);
        auto restore = [grp, saved_of]() {                      // the family's SAVED colours
            for (auto& it : grp->items) {
                auto s = saved_of->find(it.cc.get());
                NodeRef sc = s == saved_of->end() ? nullptr : s->second;
                cc_set_color(it.cc, sc ? cc_color(sc) : it.orig);
                node_set_enable(it.cc, (sc && node_enable(sc)) ? true : it.orig_enable);
            }
        };
        auto shift = [grp, center0, restore](int v) {
            if (v == center0) { restore(); return; }            // entry value = the saved colours
            for (auto& it : grp->items) {
                const bool chroma = it.s > 0.15;
                const double h = chroma ? (v + (it.h - grp->center)) : (double)v;   // coloured members keep their offsets
                const double sat = chroma ? std::max(it.s, 0.45) : 0.55;
                const double val = 0.25 + 0.75 * it.v;          // black -> dark coloured, white -> pastel
                int r, g, b; hsv_to_rgb(h, sat, val, r, g, b);
                cc_set_color(it.cc, pack_rgba(r, g, b, it.a)); node_set_enable(it.cc, true);
            }
        };
        const std::string label = grp->label;
        const NodeRef rep_cc = rep.cc;
        auto title_of = [label, rep_cc]() {
            const char* ql = label == "SKIN" ? "quick.skin" : label == "HAIR" ? "quick.hair" : label == "FACE" ? "quick.face" : label == "ACCESSORIES" ? "quick.accessories" : nullptr;
            return fmt("%s   %s", (ql ? L(ql) : label).c_str(), swatch(cc_color(rep_cc)).c_str());
        };
        std::string members;                                    // what this family changes (guide line)
        for (auto& x : grp->cls) {
            if (!members.empty()) members += ", ";
            members += fmt("%s (%d)", human(cl_name(x->c->cl)).c_str(), (int)x->items.size());
        }
        EntryRef e = qe->slider(fmt("qe%d", gi), title_of(), L("quick.changes", { members }), 0, 360, center0);
        e->look = 1;                                            // an absolute hue: the rainbow under it
        std::weak_ptr<Entry> we = e;
        e->cb_value = [we, shift, title_of](int v) {
            auto self = we.lock();
            if (self) set_selected(self);
            shift(v);
            editor_refresh();
            if (self) retitle_row(self, title_of());
        };
        e->apply = shift;
        e->restore = restore;                                   // Backspace: the family's SAVED colours
        e->targets = std::make_shared<std::vector<NodeRef>>();
        for (auto& it : grp->items) e->targets->push_back(it.cc);
        e->mark_fn = [grp](const Marks& m) { for (auto& it : grp->items) if (m.col.count(it.cc.get())) return true; return false; };
    }
}

// NO replaces the confirm page AND the root under it, cursor back on the row it came from (:3173)
std::function<ButtonResult()> back(const std::string& key) {
    return [key]() {
        edit.has_page = true; edit.page = key; edit.has_title_hold = true; edit.title_hold = frame_now() + 15;
        editor_build_pages();
        if (be.opt_navigate) be.opt_navigate(key, kTrue, focus_of_or0(key), kFalse, kFalse, kTrue);
        return ButtonResult{};
    };
}
std::string confirm_page(const std::string& key, const std::string& title, const std::string& question, ButtonResult (*action)()) {
    GroupRef g = new_group(title, question, key, ROOT);
    EntryRef y = g->button("yes", L("confirm.yes"), "", action); y->has_verb = true; y->verb = "Validate";
    EntryRef n = g->button("no", L("confirm.no"), "", back(ROOT)); n->has_verb = true; n->verb = "Validate";
    return key;
}

const char* const DI_ROOT = "__dicolor_root";
struct Mech { const char* key; const char* mech; bool hue; const char* tkey; };
const Mech MECS[5] = {
    { "__dicolor_root", "impact", false, "vfx.impact" },
    { "__dicolor_rev", "reversal", false, "vfx.reversal" },
    { "__dicolor_parry", "parry", true, "vfx.parry" },
    { "__dicolor_rush", "rush", true, "vfx.rush" },
    { "__dicolor_spec", "special", true, "vfx.special" },       // special moves (SpecialColors)
};
constexpr int WINDOW = 15;                                      // one below the game's own maximum of entries in a spin

uint32_t col_or0(const std::optional<uint32_t>& c) { return c ? *c : 0; }
std::string swatch1(const std::optional<uint32_t>& col) { return fmt("<color %s>%s</color>", rgb_hex(col_or0(col)).c_str(), "\xE2\x96\xA0"); }   // one square

// the title text of a row's VISIBLE prefab (:21)
Obj di_row_title(const EntryRef& entry) {
    Obj ctrl = be.opt_row_control ? be.opt_row_control(entry->id, nullptr) : nullptr;
    if (!ctrl) return nullptr;
    Obj c = be.ui_child(ctrl);
    while (c) {
        const std::string nm = be.ui_name(c);
        if (nm == "p_Slider_0_s" || nm == "p_Spin_spintext_h" || nm == "p_GroupItem_button") {
            bool av = false;
            if (be.ui_actual_visible(c, av) && av) { ctrl = c; break; }
        }
        c = be.ui_next(c);
    }
    return find_name(ctrl, "e_text_title", 0);
}
// the row's VALUE text, inside the visible spin prefab (:39)
Obj di_row_value(const EntryRef& entry) {
    Obj ctrl = be.opt_row_control ? be.opt_row_control(entry->id, nullptr) : nullptr;
    if (!ctrl) return nullptr;
    Obj c = be.ui_child(ctrl);
    while (c) {
        if (be.ui_name(c) == "p_Spin_spintext_h") {
            bool av = false;
            if (be.ui_actual_visible(c, av) && av) { ctrl = c; break; }
        }
        c = be.ui_next(c);
    }
    Obj list = find_name(ctrl, "p_TextList_0_h", 0);
    return list ? find_name(list, "e_text", 0) : nullptr;
}
void di_value_shown(const EntryRef& entry, bool on) {           // a hidden element STAYS hidden across a rebuild: put back explicitly
    Obj t = entry ? di_row_value(entry) : nullptr;
    if (t) be.ui_set_visible(t, on);
}
void di_write_title(const EntryRef& entry, const std::string& want) {
    Obj t = entry ? di_row_title(entry) : nullptr;
    if (t && be.ui_message(t) != want) be.ui_set_text(t, want);
}
// "<prefix> n", n after the biggest already taken (:63)
std::string next_di_name(const std::vector<DiPreset>& presets, const std::string& prefix) {
    long n = 0;
    for (auto& p : presets) {
        const std::string nm = p.name ? *p.name : std::string();
        if (nm.size() > prefix.size() + 1 && starts(nm, prefix + " ")) {
            bool digits = true;
            for (size_t i = prefix.size() + 1; i < nm.size(); ++i) if (!is_digit(nm[i])) digits = false;
            if (digits) n = std::max(n, std::stol(nm.substr(prefix.size() + 1)));
        }
    }
    return prefix + " " + std::to_string(n + 1);
}
int sel_index(const DiCfg& cfg, size_t npresets) {              // :72
    long s = cfg.selected ? (long)*cfg.selected : 1;
    if (s < 1 || (size_t)s > npresets) s = 1;
    return (int)s;
}
struct DiItem { int index = 0; std::string name; std::optional<uint32_t> c1, c2; };   // the spin's items: presets only
std::string item_key(const DiItem& it) { return "p:" + std::to_string(it.index); }
std::string item_label(const DiItem& it) { return fmt("%s  %s%s", it.name.c_str(), swatch1(it.c1).c_str(), swatch1(it.c2).c_str()); }

void di_set(int mech, const char* field, Tri v) { if (be.di_cfg_set) be.di_cfg_set(mech, field, v == kNil ? 0 : 1, v == kTrue, 0, {}); }
void di_set_num(int mech, const char* field, const std::optional<double>& v) { if (be.di_cfg_set) be.di_cfg_set(mech, field, v ? 2 : 0, false, v ? *v : 0, {}); }
void di_set_col(int mech, const char* field, const std::optional<uint32_t>& v) { if (be.di_cfg_set) be.di_cfg_set(mech, field, v ? 2 : 0, false, v ? (double)*v : 0, {}); }
void di_set_str(int mech, const char* field, const std::optional<std::string>& v) { if (be.di_cfg_set) be.di_cfg_set(mech, field, v ? 3 : 0, false, 0, v ? *v : std::string()); }

struct DiRef { std::optional<uint32_t> c1, c2; };
using CfgRef = std::shared_ptr<DiCfg>;

void di_build_page(int mi, int fighter, int side, const std::string& who, const std::string& cur_page) {
    const Mech& desc = MECS[mi];
    const bool is_hue = desc.hue;                               // parry / rush / special
    const bool is_sp = std::string(desc.mech) == "special";
    int list = kPresetsDI;
    auto plist = std::make_shared<std::vector<DiPreset>>();
    if (be.di_presets) be.di_presets(kPresetsDI, *plist);
    if (is_hue) {
        std::vector<DiPreset> l;
        // pcall(is_sp and A.presets_sp or A.presets_dr): the special list when it is there, else the Drive one
        if (is_sp && be.di_presets && be.di_presets(kPresetsSP, l)) { *plist = std::move(l); list = kPresetsSP; }
        else if (be.di_presets && be.di_presets(kPresetsDR, l)) { *plist = std::move(l); list = kPresetsDR; }
    }
    const std::string prefix = is_sp ? "SP" : (is_hue ? "DR" : "DI");
    auto cfg = std::make_shared<DiCfg>();
    const bool has_cfg = be.di_cfg && be.di_cfg(mi, fighter, side, *cfg);
    const std::string KEY = desc.key;
    const bool is_root = KEY == DI_ROOT;
    // two modules, two setting files: the close saves the one that moved
    auto touch = [is_hue]() { if (is_hue) edit.drv_dirty = true; else edit.di_dirty = true; };
    auto page_title = [who](const std::string& label) { return fmt("%s - %s   %s", L("vfx.title").c_str(), who.c_str(), label.c_str()); };
    GroupRef root = new_group((is_root && edit.has_title_override) ? edit.title_override : page_title(L(desc.tkey)), "", KEY, nullptr);
    if (!has_cfg) { nav_button(root, "none", L(desc.tkey), "", KEY); return; }

    auto ref = std::make_shared<DiRef>();
    if (is_root && edit.has_di_saved) { ref->c1 = edit.di_saved.c1; ref->c2 = edit.di_saved.c2; }
    auto slot_of = [](const CfgRef& c, int k) -> std::optional<uint32_t>& { return k == 1 ? c->c1 : c->c2; };
    const std::string mech = desc.mech;
    auto col_changed = [cfg, ref, mech](int k) {
        const std::optional<uint32_t>& cv = k == 1 ? cfg->c1 : cfg->c2;
        const std::optional<uint32_t>& rv = k == 1 ? ref->c1 : ref->c2;
        if (col_or0(cv) != col_or0(rv)) return true;
        const MemoKey mk = key_of("di_" + mech + (k == 1 ? "_c1" : "_c2"));
        const HSV3 a = hsv_get(col_or0(cv), &mk), b = hsv_raw(col_or0(rv));
        return a.H != b.H || a.S != b.S || a.V != b.V;
    };

    // Color 1 / Color 2: Hue / Saturation / Brightness (the file keeps RGBA integers)
    auto color_page = [&](int k) {
        const std::string key = "__dicolor_" + mech + "_c" + std::to_string(k);
        const char* slot = k == 1 ? "c1" : "c2";
        const MemoKey mk = key_of("di_" + mech + "_" + slot);
        GroupRef pg = new_group(L("di.color_page", { std::to_string(k) }), "", key, desc.key);
        pg->slider_page = true;
        auto rows = std::make_shared<std::vector<Row>>();
        auto title_of = [k](char comp, uint32_t col) {
            const std::string name = comp == 'H' ? L("slider.hue") : comp == 'S' ? L("slider.saturation") : L("slider.brightness");
            return L("slider.color_row", { std::to_string(k), name, swatch_one(col) });
        };
        const HSV3 init = hsv_get(col_or0(slot_of(cfg, k)), &mk);
        for (char comp : { 'H', 'S', 'V' }) {
            EntryRef e = pg->slider(fmt("di%d_%c", k, comp), title_of(comp, col_or0(slot_of(cfg, k))), "", 0, 255, init.get(comp));
            if (comp == 'H') e->look = 1;
            auto set = [cfg, k, comp, mk, mi, slot, touch](int v) {
                std::optional<uint32_t>& c = k == 1 ? cfg->c1 : cfg->c2;
                c = hsv_set(col_or0(c), comp, v, &mk);
                di_set_col(mi, slot, c);
                touch();
            };
            e->cb_value = [set, cfg, k, rows, title_of](int v) {
                set(v);
                const uint32_t col = col_or0(k == 1 ? cfg->c1 : cfg->c2);
                for (auto& row : *rows) if (auto re = row.entry.lock()) di_write_title(re, title_of(row.comp, col));
            };
            e->apply = set;
            e->mark_fn = [cfg, ref, k, comp, mk](const Marks&) {
                return hsv_get(col_or0(k == 1 ? cfg->c1 : cfg->c2), &mk).get(comp) != hsv_raw(col_or0(k == 1 ? ref->c1 : ref->c2)).get(comp);
            };
            rows->push_back({ e, comp });
        }
        // the palette (0.21.0), as on the costume colour pages: the cell goes into this slot as its sliders' values do
        auto pal = std::make_shared<Palette>();
        pal->mk = mk; pal->rows = rows;
        pal->get_col = [cfg, k]() { return col_or0(k == 1 ? cfg->c1 : cfg->c2); };
        pal->set_col = [cfg, k, mi, slot, touch](uint32_t c) {
            std::optional<uint32_t>& cur = k == 1 ? cfg->c1 : cfg->c2;
            cur = c;
            di_set_col(mi, slot, cur);
            touch();
        };
        pal->retitle = [title_of](const EntryRef& re, char comp, uint32_t c) { di_write_title(re, title_of(comp, c)); };
        palette_rows(pg, fmt("di%d", k), pal, [pal, ref, k, mk]() {     // Backspace: the colour the page started from
            const std::optional<uint32_t>& rv = k == 1 ? ref->c1 : ref->c2;
            if (rv) { g_memo.erase(mk); pal->set_col(*rv); }
        }, nullptr);
        return key;
    };
    const std::string k_c1 = color_page(1), k_c2 = color_page(2);

    auto back_to_page = [KEY](const std::string& status_key, const std::optional<std::string>& arg) {
        edit.has_title_override = true; edit.title_override = arg ? L(status_key, { *arg }) : L(status_key);
        edit.has_status = true; edit.status.clear(); edit.status.push_back({ edit.title_override, frame_now() + 120, false, true });
        edit.has_page = true; edit.page = KEY; edit.has_title_hold = true; edit.title_hold = frame_now() + 15;
        editor_build_pages();
        if (be.opt_navigate) be.opt_navigate(KEY, kTrue, focus_of_or0(KEY), kFalse, kFalse, kTrue);
        return ButtonResult{};
    };
    // the question is carried by BOTH buttons: the bottom banner shows the description of the row under the cursor
    auto confirm_page = [&](const std::string& key, const std::string& title, const std::string& question, std::function<ButtonResult()> action) {
        GroupRef g = new_group(title, question, key, desc.key);
        EntryRef y = g->button("yes", L("confirm.yes"), question, std::move(action)); y->has_verb = true; y->verb = "Validate";
        EntryRef n = g->button("no", L("confirm.no"), question, back(KEY)); n->has_verb = true; n->verb = "Validate";
        return key;
    };

    std::vector<DiItem> items;                                  // :91 the presets (no costume colours, no LL any more)
    for (size_t i = 0; i < plist->size(); ++i) {
        const DiPreset& p = (*plist)[i];
        items.push_back({ (int)i + 1, p.name ? *p.name : ("DI " + std::to_string(i + 1)), p.c1, p.c2 });
    }
    int spin_sel = 1;                                           // :101
    {
        const std::string want = cfg->sel ? *cfg->sel : ("p:" + std::to_string(sel_index(*cfg, plist->size())));
        for (size_t i = 0; i < items.size(); ++i) if (item_key(items[i]) == want) { spin_sel = (int)i + 1; break; }
    }
    const bool has_cur = spin_sel >= 1 && (size_t)spin_sel <= items.size();
    if (has_cur) { ref->c1 = items[(size_t)spin_sel - 1].c1; ref->c2 = items[(size_t)spin_sel - 1].c2; }
    const bool on_preset = has_cur;
    const int sel = on_preset ? items[(size_t)spin_sel - 1].index : sel_index(*cfg, plist->size());
    const std::string sel_name = ((size_t)sel >= 1 && (size_t)sel <= plist->size() && (*plist)[(size_t)sel - 1].name) ? *(*plist)[(size_t)sel - 1].name
                                 : (((size_t)sel >= 1 && (size_t)sel <= plist->size()) ? std::string("nil") : std::string("-"));
    const std::string pfx = "__dicolor_" + mech;
    const std::string save_target = on_preset ? sel_name : next_di_name(*plist, prefix);
    const std::string k_save = confirm_page(pfx + "_c_save", L("di.confirm.save.title") + "   " + save_target,
        on_preset ? L("di.confirm.save.desc", { sel_name }) : L("di.confirm.save.desc_new", { save_target }),
        [=]() {
            if (on_preset) {
                if ((size_t)sel >= 1 && (size_t)sel <= plist->size()) {
                    (*plist)[(size_t)sel - 1].c1 = cfg->c1; (*plist)[(size_t)sel - 1].c2 = cfg->c2;
                    if (be.di_preset_set) be.di_preset_set(list, sel, cfg->c1, cfg->c2);
                    if (be.di_save_presets) be.di_save_presets();
                }
                cfg->selected = sel; di_set_num(mi, "selected", cfg->selected);
                cfg->sel = "p:" + std::to_string(sel); di_set_str(mi, "sel", cfg->sel);
            } else {
                const std::string nm = next_di_name(*plist, prefix);
                plist->push_back({ nm, cfg->c1, cfg->c2 });
                if (be.di_preset_add) be.di_preset_add(list, nm, cfg->c1, cfg->c2);
                cfg->selected = (double)plist->size(); di_set_num(mi, "selected", cfg->selected);
                cfg->sel = "p:" + std::to_string(plist->size()); di_set_str(mi, "sel", cfg->sel);
                cfg->ll = kFalse; di_set(mi, "ll", kFalse);
                if (be.di_save_presets) be.di_save_presets();
            }
            touch();
            return back_to_page("status.save_ok", std::nullopt);
        });
    const std::string saveas_name = next_di_name(*plist, prefix);
    // the target name is in the TITLE, not only in the bottom banner
    const std::string k_saveas = confirm_page(pfx + "_c_saveas", L("di.confirm.saveas.title") + "   " + saveas_name, L("di.confirm.saveas.desc", { saveas_name }),
        [=]() {
            plist->push_back({ saveas_name, cfg->c1, cfg->c2 });
            if (be.di_preset_add) be.di_preset_add(list, saveas_name, cfg->c1, cfg->c2);
            cfg->selected = (double)plist->size(); di_set_num(mi, "selected", cfg->selected);
            cfg->sel = "p:" + std::to_string(plist->size()); di_set_str(mi, "sel", cfg->sel);
            cfg->ll = kFalse; di_set(mi, "ll", kFalse);
            if (be.di_save_presets) be.di_save_presets();
            touch();
            return back_to_page("status.saved_as", saveas_name);
        });
    // RESET and DELETE: two rows, two pages (the 8th row of the pool; frame enlarged by fix_frame)
    const std::string k_delete = pfx + "_c_reset";
    {
        GroupRef g = new_group(k_delete, "", k_delete, desc.key);
        g->title = L("di.confirm.reset.title");
        g->desc = on_preset ? L("di.confirm.reset.desc_preset_only", { sel_name }) : L("di.confirm.reset.desc");
        if (be.opt_group_def) be.opt_group_def(g->id, g->title, g->desc);
        EntryRef y = g->button("yes", L("confirm.yes"), g->desc, [=]() {
            cfg->c1 = ref->c1; di_set_col(mi, "c1", cfg->c1);
            cfg->c2 = ref->c2; di_set_col(mi, "c2", cfg->c2);
            g_memo.clear(); g_memo_keep.clear();
            touch();
            return back_to_page("status.reset_ok", std::nullopt);
        });
        y->has_verb = true; y->verb = "Validate";
        EntryRef n = g->button("no", L("confirm.no"), g->desc, back(KEY)); n->has_verb = true; n->verb = "Validate";
    }
    const std::string k_del2 = pfx + "_c_delete";
    {
        GroupRef g = new_group(k_del2, "", k_del2, desc.key);
        g->title = on_preset ? (L("di.confirm.delete.title") + "   " + sel_name) : L("di.confirm.delete.title");
        g->desc = on_preset ? L("di.confirm.delete.desc", { sel_name }) : L("di.confirm.delete.none");
        if (be.opt_group_def) be.opt_group_def(g->id, g->title, g->desc);
        if (on_preset) {
            EntryRef d = g->button("delete", L("di.delete"), g->desc, [=]() {
                const std::string gone = ((size_t)sel >= 1 && (size_t)sel <= plist->size() && (*plist)[(size_t)sel - 1].name) ? *(*plist)[(size_t)sel - 1].name : std::string("-");
                if (plist->size() > 1) {
                    plist->erase(plist->begin() + (sel - 1));
                    if (be.di_preset_remove) be.di_preset_remove(list, sel);
                    cfg->selected = 1.0; di_set_num(mi, "selected", cfg->selected);
                    cfg->sel = std::string("p:1"); di_set_str(mi, "sel", cfg->sel);
                    if (be.di_save_presets) be.di_save_presets();
                    touch();
                }
                return back_to_page("status.erased", gone);
            });
            d->has_verb = true; d->verb = "Validate";
        }
        EntryRef n = g->button("no", L("confirm.no"), g->desc, back(KEY)); n->has_verb = true; n->verb = "Validate";
    }

    const int mode0 = (cfg->enabled == kTrue && cfg->follow == kTrue) ? 3 : (cfg->enabled == kTrue ? 2 : 1);
    auto rows_shown = [cfg]() { return cfg->enabled == kTrue && cfg->follow != kTrue; };
    EntryRef en = root->choice_row("enabled", L("di.enabled"), L("di.enabled_desc"), { L("toggle.off"), L("toggle.on"), L("di.follow") }, mode0);
    en->cb_value = [cfg, mi, touch, rows_shown, KEY](int v) {
        const bool before = rows_shown();
        cfg->enabled = (v != 1) ? kTrue : kFalse; di_set(mi, "enabled", cfg->enabled);
        cfg->follow = (v == 3) ? kTrue : kFalse; di_set(mi, "follow", cfg->follow);
        touch();
        if (cfg->follow == kTrue && cfg->ll == kTrue) { cfg->ll = kFalse; di_set(mi, "ll", kFalse); cfg->sel.reset(); di_set_str(mi, "sel", cfg->sel); }
        if (before != rows_shown()) { edit.focus_of[KEY] = 0; edit.has_di_refresh_at = true; edit.di_refresh_at = frame_now() + 20; edit.di_rebuild = true; }
    };
    if (!rows_shown()) return;
    auto color_title = [cfg](int k) { return L("di.color_row", { std::to_string(k), swatch_one(col_or0(k == 1 ? cfg->c1 : cfg->c2)) }); };
    auto root_rows = std::make_shared<DiRootRows>();
    // nav() rebuilds ALL the pages with edit.page already on the sub-page being opened: the sub-pages of this
    // mechanic are accepted too, or the swatch would not follow on the way back
    const std::string sub = "__dicolor_" + mech + "_";
    if (KEY == cur_page || starts(cur_page, sub)) { edit.di_root_rows = root_rows; edit.di_color_title = color_title; }

    // long lists: the spin holds about 16 options, so the list is cut into blocks of WINDOW; the window slides
    const int total = (int)items.size();
    const int nblocks = std::max(1, (int)std::ceil(total / (double)WINDOW));
    edit.has_di_blk_of = true;
    int blk = edit.di_blk_of.count(mech) ? edit.di_blk_of[mech] : 0;
    if (blk < 0 || blk >= nblocks || spin_sel <= blk * WINDOW || spin_sel > std::min(total, (blk + 1) * WINDOW)) blk = (spin_sel - 1) / WINDOW;
    edit.di_blk_of[mech] = blk;
    const int base = std::min(blk * WINDOW, std::max(0, total - WINDOW));
    const int size = std::min(WINDOW, total);
    std::vector<std::string> labels;
    for (int i = 1; i <= size; ++i) labels.push_back(item_label(items[(size_t)(base + i) - 1]));
    struct Spin { int abs, slot; };
    auto spin = std::make_shared<Spin>(Spin{ spin_sel, spin_sel - base });
    auto all_items = std::make_shared<std::vector<DiItem>>(items);
    EntryRef preset_entry = root->choice_row("preset", L("di.preset"), L("di.preset_desc"), labels, spin->slot);
    std::weak_ptr<Entry> wpreset = preset_entry;
    preset_entry->cb_value = [=](int v) {
        int step = v - spin->slot;
        if (step == 0) return;
        if (size > 1) { if (step == -(size - 1)) step = 1; else if (step == size - 1) step = -1; }
        const int nabs = (int)lua_mod((double)(spin->abs + step - 1), (double)total) + 1;
        if (nabs < 1 || (size_t)nabs > all_items->size()) return;
        const DiItem& it = (*all_items)[(size_t)nabs - 1];
        spin->abs = nabs; spin->slot = spin->abs - base;
        cfg->sel = item_key(it); di_set_str(mi, "sel", cfg->sel);
        cfg->ll = kFalse; di_set(mi, "ll", kFalse);
        cfg->c1 = it.c1; di_set_col(mi, "c1", cfg->c1);
        cfg->c2 = it.c2; di_set_col(mi, "c2", cfg->c2);
        ref->c1 = it.c1; ref->c2 = it.c2;
        cfg->selected = (double)it.index; di_set_num(mi, "selected", cfg->selected);
        touch();
        edit.focus_of[KEY] = 1;
        const bool dehors = (spin->abs <= base) || (spin->abs > base + size);   // left the window on screen
        if (dehors) {
            di_value_shown(wpreset.lock(), false);
            edit.has_di_show_at = true; edit.di_show_at = frame_now() + 12;
            edit.di_blk_of[mech] = (spin->abs - 1) / WINDOW;
            edit.di_rebuild = true;
            edit.has_di_refresh_at = true; edit.di_refresh_at = frame_now();
            return;
        }
        di_write_title(root_rows->c1, color_title(1)); di_write_title(root_rows->c2, color_title(2));
    };
    if (KEY == cur_page) {
        edit.di_preset_entry = preset_entry;
        if (!edit.has_di_show_at) di_value_shown(preset_entry, true);
    }
    if (edit.di_simple) return;                                 // training popup: Enabled + Preset only
    root_rows->c1 = nav_button(root, "c1", color_title(1), L("di.color1_desc"), k_c1); root_rows->c1->mark_fn = [col_changed](const Marks&) { return col_changed(1); };
    root_rows->c2 = nav_button(root, "c2", color_title(2), L("di.color2_desc"), k_c2); root_rows->c2->mark_fn = [col_changed](const Marks&) { return col_changed(2); };
    auto verb = [](const EntryRef& e) { e->has_verb = true; e->verb = "Validate"; return e; };
    verb(nav_button(root, "save", L("root.save"), L("di.save_desc"), k_save));
    verb(nav_button(root, "saveas", L("root.saveas"), L("di.saveas_desc"), k_saveas));
    root_rows->reset = verb(nav_button(root, "reset", L("di.reset"), L("di.reset_desc_only"), k_delete));
    verb(nav_button(root, "delete", L("di.delete"), L("di.delete_desc"), k_del2));
}

void di_build_pages() {                                         // M.build_pages (:163)
    if (!(be.di_available && be.di_available())) return;
    const int fighter = edit.di_fighter, side = edit.di_side;
    for (auto& g : edit.groups) if (be.opt_remove_group) be.opt_remove_group(g->id);
    edit.groups.clear();
    if (!window_open()) g_by_id.clear();
    edit.pages.clear();
    ++g_nav_gen;                                                // edit.nav_fns = {}
    edit.marks_on = true;                                       // edit.marks_fn: nothing marked by colour here
    std::string name = be.di_char_name ? be.di_char_name(fighter) : std::string();
    if (name.empty()) name = std::to_string(fighter);
    const std::string who = upper(name) + " - P" + std::to_string(side);
    // M.open builds the pages BEFORE setting edit.page: without this fallback the first page would not get its row references
    const std::string cur_page = edit.has_page ? edit.page : std::string(DI_ROOT);
    for (int i = 0; i < 5; ++i) {
        // Special Attacks off: no page
        if (std::string(MECS[i].mech) == "special" && !(be.di_special_enabled && be.di_special_enabled())) continue;
        di_build_page(i, fighter, side, who, cur_page);
    }
}

// root page rebuilt (swatches) after a preset change, once the spin is idle; cursor kept on the Preset row (:453)
void di_tick() {
    if (edit.has_di_show_at && frame_now() >= edit.di_show_at) {   // the new page is up: show the value again
        edit.has_di_show_at = false;
        di_value_shown(edit.di_preset_entry, true);
    }
    if (!edit.di || !edit.has_di_refresh_at) return;
    // nav() puts edit.page on the SUB-page: walk up the parents to a mechanic page
    bool ours = false; int guard = 0;
    bool has_page = edit.has_page; std::string page = edit.page;
    while (has_page && guard < 8) {
        for (auto& d : MECS) if (page == d.key) ours = true;
        if (ours) break;
        has_page = false;
        for (auto& g : edit.groups) if (g->key == page && g->has_parent) { page = g->parent_key; has_page = true; break; }
        ++guard;
    }
    if (!window_open() || !ours) { edit.has_di_refresh_at = false; return; }
    if (frame_now() < edit.di_refresh_at) return;
    edit.has_di_refresh_at = false;
    if (edit.di_rebuild) {                                      // Enabled switched to / from Follow: the rows change
        edit.di_rebuild = false;
        const std::string k = edit.page;
        editor_build_pages();
        if (be.opt_navigate) be.opt_navigate(k, kTrue, focus_of_or0(k), kNil, kNil, kNil);   // back on the row we were on
        return;
    }
    if (edit.di_root_rows && edit.di_color_title) {             // back from Color 1 / 2: the swatches follow, no rebuild
        di_write_title(edit.di_root_rows->c1, edit.di_color_title(1)); di_write_title(edit.di_root_rows->c2, edit.di_color_title(2));
    }
}

// called by editor_teardown: the character file is written once, the battle applier re-reads it (:508)
void di_on_close() {
    if (edit.di_fighter > 0 && edit.di_dirty && be.di_save_cfg) be.di_save_cfg(edit.di_fighter);
    if (be.di_mark_dirty) be.di_mark_dirty();
    if (edit.di_fighter > 0 && edit.drv_dirty && be.drv_save_cfg) be.drv_save_cfg(edit.di_fighter);   // parry / rush: their own file
    edit.drv_dirty = false; edit.has_di_blk_of = false; edit.di_blk_of.clear();
    edit.di = false; edit.di_fighter = 0; edit.di_dirty = false; edit.di_simple = false;
    edit.di_root_rows = nullptr; edit.di_color_title = nullptr; edit.has_di_refresh_at = false; edit.di_rebuild = false;   // never touch rows of a closed dialog on the next open
}

void editor_build_pages() {
    if (edit.di) { di_build_pages(); return; }
    for (auto& g : edit.groups) if (be.opt_remove_group) be.opt_remove_group(g->id);
    edit.groups.clear();
    if (!window_open()) g_by_id.clear();                        // window closed: no page shows an earlier build
    ++g_nav_gen;
    edit.marks_on = true;
    const Marks m0 = marks();
    // customizable slots = enabled in this colour OR in any game colour of the costume; editing one enables it (:2876)
    const std::string ukey = fmt("%d_%d", edit.fighter, edit.costume);
    if (!edit.slot_union.count(ukey)) {
        std::unordered_set<std::string> uni;
        if (be.slot_union) for (auto& s : be.slot_union(edit.fighter, edit.costume)) uni.insert(s);
        edit.slot_union[ukey] = std::move(uni);
    }
    const auto& uni = edit.slot_union[ukey];
    std::vector<Cluster> clusters;
    if (auto* parts = arr_of(edit.tree, "Parts")) {
        for (size_t pi = 0; pi < parts->size(); ++pi) {
            const NodeRef& part = (*parts)[pi];
            auto* cls = arr_of(part, "Clusters");
            if (!cls) continue;
            for (size_t ci = 0; ci < cls->size(); ++ci) {
                const NodeRef& cl = (*cls)[ci];
                std::vector<int> enabled;
                if (auto* ccs = arr_of(cl, "CustomizeColors")) {
                    for (size_t k = 0; k < ccs->size(); ++k) {
                        const NodeRef& cc = (*ccs)[k];
                        if (cc && (node_enable(cc) || uni.count(fmt("%d_%d_%d", (int)pi + 1, (int)ci + 1, (int)k + 1)))) enabled.push_back((int)k + 1);
                    }
                }
                if (!enabled.empty()) {
                    Cluster c; c.cl = cl; c.enabled = enabled; c.id = fmt("%d_%d", (int)pi + 1, (int)ci + 1); c.pi = (int)pi + 1; c.ci = (int)ci + 1; c.ptype = part_type(part);
                    std::string lbl = cl_name(cl);
                    strip_prefix(lbl, "esf_");
                    for (auto& ch : lbl) if (ch == '_') ch = ' ';
                    c.label = upper(lbl);
                    clusters.push_back(std::move(c));
                }
            }
        }
    }
    edit.pages.clear();
    build_quick(clusters, m0);
    const std::string colors_key = build_mode(clusters, "col", L("root.colors"), L("list.pick_cluster"), color_rows, [](const GroupRef& g) {
        EntryRef e = g->button("random", L("list.random"), L("list.random_colors_desc"), editor_random); e->has_verb = true; e->verb = "Generate";
    }, 1, m0);                                                  // one colour per page: its sliders and its palette (0.21.0)
    const std::string mats_key = build_mode(clusters, "mat", L("root.materials"), L("list.pick_cluster"), material_rows, [](const GroupRef& g) {
        EntryRef e = g->button("random", L("list.random"), L("list.random_materials_desc"), editor_random_materials); e->has_verb = true; e->verb = "Generate";
    }, 1, m0);                                                  // one colour per page
    // EDIT SQUARES: the two chip colours (manual) (:3121)
    GroupRef sq = new_group(L("squares.title"), L("squares.desc"), "__coloredit_sq", ROOT);
    sq->slider_page = true;
    {
        engine::chips::Config cfg;
        std::vector<uint8_t> bytes;
        if (be.file_read && be.file_read(CHIP_FILE, bytes)) cfg.load_text(std::string(bytes.begin(), bytes.end()));
        const engine::chips::GameChips game = [](int f, int c, uint32_t num) -> std::optional<engine::chips::Pair> {
            uint32_t a = 0, b = 0;
            if (be.game_chips && be.game_chips(f, c, (int)num, a, b)) return engine::chips::Pair{ a, b };
            return std::nullopt;
        };
        const engine::chips::Pair cp = engine::chips::chip_for(edit.tree.get(), edit.fighter, edit.costume, nullptr, (uint32_t)edit.color_id, cfg, game);
        Chip pair = edit.chip ? edit.chip : std::make_shared<std::array<uint32_t, 2>>(std::array<uint32_t, 2>{ cp.first, cp.second });
        for (int which = 1; which <= 2; ++which) {
            auto rows = std::make_shared<std::vector<Row>>();
            const std::string memo_name = "sq" + std::to_string(which);
            const MemoKey mk = key_of(memo_name);
            auto title_of = [which](char comp, uint32_t col) {
                const std::string name = comp == 'H' ? L("slider.hue") : comp == 'S' ? L("slider.saturation") : L("slider.brightness");
                return L("slider.square_row", { std::to_string(which), name, swatch(col) });
            };
            const size_t w = (size_t)which - 1;
            const HSV3 init = hsv_get((*pair)[w], &mk);
            auto take_chip = [pair]() { if (!edit.chip) edit.chip = std::make_shared<std::array<uint32_t, 2>>(*pair); };
            for (char comp : { 'H', 'S', 'V' }) {
                EntryRef e = sq->slider(fmt("sq%d_%c", which, comp), title_of(comp, (*pair)[w]), "", 0, 255, init.get(comp));
                if (comp == 'H') e->look = 1;
                e->cb_value = [take_chip, w, comp, mk, rows, title_of](int v) {
                    take_chip();
                    (*edit.chip)[w] = hsv_set((*edit.chip)[w], comp, v, &mk);
                    for (auto& row : *rows) if (auto re = row.entry.lock()) retitle_row(re, title_of(row.comp, (*edit.chip)[w]));
                };
                e->apply = [take_chip, pair, w, comp, mk](int v) {
                    take_chip(); (*edit.chip)[w] = hsv_set((*edit.chip)[w], comp, v, &mk);
                    const uint32_t sv = edit.saved_chip ? (*edit.saved_chip)[w] : (*pair)[w];
                    if ((*edit.chip)[w] == sv) g_memo.erase(mk);
                };
                e->reset_value = [pair, w, comp]() { return hsv_raw(edit.saved_chip ? (*edit.saved_chip)[w] : (*pair)[w]).get(comp); };
                e->mark_fn = [pair, w, comp, mk](const Marks&) {
                    const uint32_t cur = edit.chip ? (*edit.chip)[w] : (*pair)[w];
                    return hsv3(cur, mk).get(comp) != hsv_raw(edit.saved_chip ? (*edit.saved_chip)[w] : (*pair)[w]).get(comp);
                };
                rows->push_back({ e, comp });
            }
        }
    }
    // confirmations: YES runs the action, NO goes back to the root (:3184)
    const std::string what = edit.has_entry ? edit.entry.label : L("root.color_n", { std::to_string(edit.color_id + 1) });
    const bool is_mc = edit.has_entry && edit.entry.manual;
    const std::string k_saveas = confirm_page("__coloredit_c_saveas", L("confirm.saveas.title"), L("confirm.saveas.desc", { what }), editor_save_as);
    const std::string k_save = confirm_page("__coloredit_c_save", L("confirm.save.title"), is_mc ? L("confirm.save.desc_mc", { what }) : L("confirm.save.desc", { what }), editor_save);
    const std::string k_reset = confirm_page("__coloredit_c_reset", L("confirm.reset.title"), is_mc ? L("confirm.reset.desc_mc", { what }) : L("confirm.reset.desc", { what }), editor_reset);
    const std::string k_erase = is_mc ? confirm_page("__coloredit_c_erase", L("confirm.erase.title"), L("confirm.erase.desc", { what }), editor_erase) : std::string();
    // level 1
    GroupRef root = new_group(edit.has_title_override ? edit.title_override : L("root.title", { upper(what) }), L("root.desc"), ROOT, nullptr);
    marked(nav_button(root, "quick", L("root.quick"), L("root.quick_desc"), "__coloredit_quick"), "col", nullptr);
    marked(nav_button(root, "colors", L("root.colors"), L("root.colors_desc"), colors_key), "col", nullptr);
    marked(nav_button(root, "materials", L("root.materials"), L("root.materials_desc"), mats_key), "mat", nullptr);
    marked(nav_button(root, "squares", L("root.squares"), L("root.squares_desc"), "__coloredit_sq"), "sq", nullptr);
    auto verb = [](const EntryRef& e) { e->has_verb = true; e->verb = "Validate"; };
    verb(nav_button(root, "save", L("root.save"), L("root.save_desc"), k_save));
    verb(nav_button(root, "saveas", L("root.saveas"), L("root.saveas_desc"), k_saveas));
    verb(nav_button(root, "reset", L("root.reset"), L("root.reset_desc"), k_reset));
    if (!k_erase.empty()) verb(nav_button(root, "erase", L("root.erase"), L("root.erase_desc"), k_erase));
}

// ---------------------------------------------------------------- open, teardown (:2187, :3214)
void fill_effective(const NodeRef& tree, const NodeRef& live) { // :2068 disabled material options: the effective value of the model
    if (!tree || !live) return;
    each_cc(tree, [&](int pi, int ci, int k, const NodeRef&, const NodeRef&, const NodeRef& cc) {
        NodeRef lcc = cc_at(live, pi, ci, k);
        if (!cc || !lcc || !obj_of(cc, "Option") || !obj_of(lcc, "Option")) return;
        for (const char* nm : { "BlendRate", "Roughness", "Metalness" }) {
            NodeRef n = opt_node(cc, nm), ln = opt_node(lcc, nm);
            if (n && ln && !node_enable(n) && node_has_value(ln)) node_set_value(n, node_value(ln));
        }
    });
}
void editor_teardown() {
    if (be.camera_close) be.camera_close();                     // belt and suspenders, also called on the hints_on -> false edge
    for (auto& g : edit.groups) if (be.opt_remove_group) be.opt_remove_group(g->id);
    edit.groups.clear();
    g_by_id.clear();
    if (edit.di) di_on_close();
    if (be.teardown_restore) be.teardown_restore();             // blocked agents, the training menu's input
    edit.has_status = false; edit.status.clear(); edit.has_title_override = false; edit.title_override.clear();
    edit.active = false; edit.tree = nullptr; edit.cm_new = nullptr;
    if (be.opt_rebuild) be.opt_rebuild();
    if (be.opt_ensure_built) be.opt_ensure_built();
}

}  // namespace

void set_backend(const Backend& b) { be = b; }
void set_data_dir(const std::string& dir) { g_data_dir = dir; g_catalog_built = false; }
bool active() { return edit.active; }
bool di_active() { return edit.di; }
bool closed_at(int64_t& frame) { frame = edit.closed_at; return edit.has_closed_at; }
bool preview(rsz::NodeRef& tree, bool& hl_on, double& hl_k, std::vector<const rsz::Node*>& hl_targets) {
    if (!edit.active || edit.di || !edit.tree) return false;
    tree = edit.tree;
    hl_on = edit.hl_on && edit.hl_targets != nullptr;
    hl_k = edit.has_hl_k ? edit.hl_k : 0.5;
    hl_targets.clear();
    if (hl_on) for (auto& cc : *edit.hl_targets) hl_targets.push_back(cc.get());
    return true;
}
void set_pad_bits(uint32_t btn, uint32_t a, uint32_t y, uint32_t b, uint32_t l1, uint32_t r1, uint32_t r3, uint32_t l3) {
    PAD.BTN = btn; PAD.A = a; PAD.Y = y; PAD.B = b; PAD.L1 = l1; PAD.R1 = r1; PAD.R3 = r3; PAD.L3 = l3;
}
bool reopen_now() { return g_reopen_now; }
void reset() {
    edit = EditState{};
    g_by_id.clear();
    g_memo.clear(); g_memo_keep.clear();
    g_reopen_now = false; g_catalog_built = false;
}

void open() {
    journal(fmt("editor_open: active=%s window=%s", edit.active ? "true" : "false", window_open() ? "true" : "false"));
    if (edit.active || window_open()) return;
    if (!(be.has_preview && be.has_preview())) { journal("editor_open: no preview controller"); return; }
    int k = 0;
    if (!(be.open_args && be.open_args(edit.fighter, edit.costume, edit.color_id, k))) { journal("editor_open: no screen"); return; }
    const auto* entries = entries_for(edit.fighter, edit.costume);
    edit.has_entry = (k > 0 && entries && (size_t)k <= entries->size());
    if (edit.has_entry) edit.entry = (*entries)[(size_t)k - 1];
    NodeRef original, current;
    if (edit.has_entry) {
        original = entry_tree(edit.entry, true); current = entry_tree(edit.entry, false);
        NodeRef live = be.live_tree ? be.live_tree(edit.color_id) : nullptr;   // the model wears this entry: its `_Value`s are the effective ones
        if (live) { original = copy_tree(original); current = copy_tree(current); fill_effective(original, live); fill_effective(current, live); }
    } else {
        if (be.snapshot_pristine) be.snapshot_pristine(edit.fighter, edit.costume);
        NodeRef live = be.live_tree ? be.live_tree(edit.color_id) : nullptr;
        original = read_tree(pristine_path(edit.fighter, edit.costume, edit.color_id));
        if (!original) original = live;
        current = override_tree(edit_okey());
        if (!current) current = live ? live : original;
    }
    if (!current) { journal("editor_open: no tree"); return; }
    edit.original = copy_tree(original ? original : current);
    edit.tree = copy_tree(current);
    edit.chip = nullptr;
    if (edit.has_entry && !edit.entry.path.empty()) {
        const auto cfg = chip_cfg_load();
        auto it = cfg.find(edit.entry.path);
        if (it != cfg.end() && it->is_array() && it->size() >= 2 && (*it)[0].is_number() && (*it)[1].is_number())
            edit.chip = std::make_shared<std::array<uint32_t, 2>>(std::array<uint32_t, 2>{ (*it)[0].get<uint32_t>(), (*it)[1].get<uint32_t>() });
    }
    mark_saved();                                               // unsaved-changes marker
    g_memo.clear(); g_memo_keep.clear();                        // slider memory belongs to one editing session
    edit.active = true; edit.di_simple = false;                 // full-size frame (the DI popup menu is the small one)
    if (!edit.empty_purged) { edit.empty_purged = true; if (be.purge_empty_files) be.purge_empty_files(); }   // once: the files emptied before the DLL could delete
    edit.built_pad = be.screen_pad && be.screen_pad();
    editor_build_pages();
    if (be.opt_rebuild) be.opt_rebuild();
    const bool okb = be.opt_ensure_built && be.opt_ensure_built();
    journal(fmt("editor_open: pages=%d built=%s", (int)edit.groups.size(), okb ? "true" : "false"));
    edit.has_page = true; edit.page = ROOT;
    if (be.opt_open_menu) be.opt_open_menu(ROOT, kNoFocus);
    edit.opened = false;
    editor_refresh();
}

void open_di(int fighter, int costume, bool simple, int side, const std::string& mech, bool at_select) {   // M.open (:487)
    if (edit.active || window_open()) return;
    if (!(be.di_available && be.di_available())) { journal("DI menu: SF6_DIRecolor.lua not loaded"); return; }
    if (fighter <= 0) { journal("DI menu: no fighter"); return; }
    edit.di_at_select = at_select;
    edit.di = true; edit.di_fighter = fighter; edit.di_costume = costume; edit.di_side = (side == 2) ? 2 : 1; edit.di_simple = simple; edit.di_dirty = false;
    g_memo.clear(); g_memo_keep.clear();
    DiCfg c0;
    edit.has_di_saved = true; edit.di_saved = {};
    if (be.di_cfg && be.di_cfg(0, fighter, edit.di_side, c0)) { edit.di_saved.c1 = c0.c1; edit.di_saved.c2 = c0.c2; }   // unsaved-change dots
    edit.tree = nullptr; edit.has_entry = false; edit.chip = nullptr;
    edit.active = true;
    edit.built_pad = be.screen_pad && be.screen_pad();
    // mech = the mechanic page to land on (the native Drive rows open the editor straight on their own mechanic)
    std::string first = DI_ROOT;
    for (auto& d : MECS) if (!mech.empty() && mech == d.mech) first = d.key;
    edit.has_page = true; edit.page = first;
    editor_build_pages();
    if (be.opt_rebuild) be.opt_rebuild();
    if (be.opt_ensure_built) be.opt_ensure_built();
    edit.page = first;
    if (be.opt_open_menu) be.opt_open_menu(first, kNoFocus);
    edit.opened = false;
}

// ---------------------------------------------------------------- layout, the dialog's callbacks (:3269-3409)
namespace {
struct Frame { double shadow_w, shadow_h, bg_w, bg_h, line_w, line_h, main_y, title_y, guide_y; };
const Frame kFrameBig = { 1058, 892, 1000, 834, 990, 824, -282, -352, 262 };     // 8-row page: +64 px, centred
const Frame kFrameSmall = { 1058, 498, 1000, 440, 990, 430, -85, -155, 120 };     // 2-row menu (DI menu on the training popup)

void apply_layout(Obj cm) {                                     // re-asserted: stacked dialogs get cascaded by the game
    if (!cm) return;
    double lx = (be.screen_visible && be.screen_visible()) ? -430 : 0, ly = 0, scale = 1.0;   // docked left on Edit Character, centred elsewhere
    if (edit.di && edit.di_at_select) { lx = (edit.di_side == 2) ? 620 : -620; ly = -60; scale = 0.66; }   // character select: the opener's third of the screen
    double x = 0, y = 0, z = 0;
    if (!be.ui_position(cm, x, y, z) || std::fabs(x - lx) > 0.5 || std::fabs(y - ly) > 0.5) be.ui_set_position(cm, lx, ly, 0.0);
    double sx = 0;
    if (!be.ui_scale(cm, sx) || std::fabs(sx - scale) > 0.001) be.ui_set_scale(cm, scale, scale, 1.0);
}
void fix_frame(Obj cm) {                                        // one frame for every page, the 8-row one (:3299)
    if (!cm) return;
    const Frame& f = edit.di_simple ? kFrameSmall : kFrameBig;
    Obj dlg = find_name(cm, "c_dialog", 0);
    Obj c = dlg ? be.ui_child(dlg) : nullptr;
    while (c) {
        const std::string nm = be.ui_name(c);
        double w = 0, h = 0; bool has_size = true;
        if (nm == "e_scale9grid_shadow") { w = f.shadow_w; h = f.shadow_h; }
        else if (nm == "e_bg" || nm == "e_bg_cfn" || nm == "e_texture_dot") { w = f.bg_w; h = f.bg_h; }
        else if (nm == "e_scale9grid_line") { w = f.line_w; h = f.line_h; }
        else has_size = false;
        if (has_size) {
            double cw = 0, ch = 0;
            if (!be.ui_size(c, cw, ch) || std::fabs(ch - h) > 0.5) be.ui_set_size(c, w, h);
        }
        double y = 0; bool has_y = true;
        if (nm == "p_Group_main_v_kc") y = f.main_y; else if (nm == "e_title") y = f.title_y; else if (nm == "p_InputGuideList_0") y = f.guide_y; else has_y = false;
        if (has_y) {
            double px = 0, py = 0, pz = 0;
            if (be.ui_position(c, px, py, pz) && std::fabs(py - y) > 0.5) be.ui_set_position(c, px, y, pz);
        }
        c = be.ui_next(c);
    }
}
void editor_layout() {                                          // safety net (polled) (:3348)
    if (edit.cm_new) {                                          // newest dialog (page change: several coexist)
        Obj cm = edit.cm_new;
        apply_layout(cm);
        if (edit.has_frame_due && frame_now() >= edit.frame_due) fix_frame(cm);
        // The 8th row of the pool is placed by the game but left invisible: this dialog shows 7. On a page of 8
        // entries it is shown, and re-asserted - by the row count of the dialog itself.
        if (frame_now() % 3 == 0) {
            const int rows = (edit.rows_cm == cm) ? edit.rows_n : 0;
            if (rows >= 8) {
                if (Obj u7 = be.ui_find ? be.ui_find(cm, "p_OptionUnit_7_switch_show") : nullptr) {
                    if (!g_grid_page && be.ui_force_invisible(u7)) be.ui_set_force_invisible(u7, false);   // a palette row's lock stays
                    if (!be.ui_visible(u7)) be.ui_set_visible(u7, true);
                }
            }
        }
    }
    if (be.dialog_control_mains) for (Obj cm : be.dialog_control_mains()) if (cm) apply_layout(cm);   // every stacked dialog stays docked
}
}  // namespace

void on_dialog_created(Obj param) {
    if (!edit.active) return;
    edit.has_title_hold = true; edit.title_hold = frame_now() + 15;
    Obj cm = be.param_control_main ? be.param_control_main(param) : nullptr;
    apply_layout(cm);
    fix_frame(cm);                                              // frame pinned before the first draw (and again in on_page_setup)
    edit.cm_new = cm;                                           // the newest dialog (the old one may still be up)
    edit.has_frame_due = true; edit.frame_due = frame_now() + 1;
}
void on_page_popped(Obj /*param*/) {                            // the game closed the top page: the parent is current again
    edit.cm_new = nullptr;                                      // never touch the ended dialog's controls
    edit.has_frame_due = false;
    edit.has_title_hold = true; edit.title_hold = frame_now() + 15;
    if (edit.di) { edit.has_di_refresh_at = true; edit.di_refresh_at = frame_now() + 15; }   // DI menu: root swatches refreshed once the pop has settled
}
void on_page_setup(Obj param, int rows) {                       // page rows built: pin the frame again (before draw)
    if (!edit.active) return;
    Obj cm = be.param_control_main ? be.param_control_main(param) : nullptr;
    if (cm) fix_frame(cm);
    edit.rows_cm = cm; edit.rows_n = rows;                      // rows of THIS dialog (the 8th pool row, editor_layout)
}

ButtonResult on_button(int eid) {
    EntryRef e = entry_by_id(eid);
    if (!e || !e->cb_button) return {};
    auto cb = e->cb_button;                                     // the callback rebuilds the pages: it must outlive its entry
    return cb();
}
void on_value(int eid, int value) {
    EntryRef e = entry_by_id(eid);
    if (!e) return;
    if (e->has_echo) {                                          // the read-back of a value set in place: not a move
        e->has_echo = false;
        if (value == e->echo) { e->value = value; e->has_value = true; return; }
    }
    e->value = value; e->has_value = true;
    if (e->cb_value) { auto cb = e->cb_value; cb(value); }
}

// ---------------------------------------------------------------- the loop (:3898-4253)
namespace {
bool same_targets(const TargetsRef& a, const TargetsRef& b) {  // :3659
    if (a == b) return true;
    if (!a || !b || a->size() != b->size()) return false;
    for (size_t i = 0; i < a->size(); ++i) if ((*a)[i] != (*b)[i]) return false;
    return true;
}
GroupRef group_by_key(const std::string& key) { for (auto& g : edit.groups) if (g->key == key) return g; return nullptr; }
bool key_down(int vk) { return be.key_down && be.key_down(vk); }
uint32_t pad_mask() { return be.pad_mask ? be.pad_mask() : 0; }
int index_in(const GroupRef& g, const EntryRef& e) { int idx = 0; for (size_t i = 0; i < g->entries.size(); ++i) if (g->entries[i] == e) idx = (int)i; return idx; }
// the page is rebuilt from the tree (fresh values / titles) and reopened on the same row
void rebuild_same_page(int idx) {
    const bool keep_backup = edit.page_backup; const bool keep_has = edit.has_entry_values; auto keep_values = edit.entry_values;
    const bool keep_has_page = edit.has_page; const std::string keep_page = edit.page;
    editor_build_pages();                                       // fresh definitions (values / titles from the tree)
    edit.page_backup = keep_backup; edit.has_entry_values = keep_has; edit.entry_values = keep_values; edit.has_page = keep_has_page; edit.page = keep_page;
    if (be.opt_navigate) be.opt_navigate(edit.page, kTrue, idx, kNil, kNil, kNil);   // End() -> rebuild the engine units -> Start(), cursor kept
}
// the three H / S / V sliders of the FOCUSED colour, by their keys (c3_H / di1_H / sq1_H)
bool color_triple(const GroupRef& g, const EntryRef& e, EntryRef out[3]) {
    if (e && e->pal) {                                          // a palette row: its colour's three sliders
        EntryRef hsv[3];
        for (auto& row : *e->pal->rows) if (auto re = row.entry.lock()) hsv[row.comp == 'H' ? 0 : row.comp == 'S' ? 1 : 2] = re;
        if (!(hsv[0] && hsv[1] && hsv[2])) return false;
        for (int i = 0; i < 3; ++i) out[i] = hsv[i];
        return true;
    }
    if (!e || !e->slider) return false;
    const std::string& k = e->key;
    if (k.size() < 2 || k[k.size() - 2] != '_' || (k.back() != 'H' && k.back() != 'S' && k.back() != 'V')) return false;
    const std::string pre = k.substr(0, k.size() - 2);
    EntryRef h, s, v;
    for (auto& x : g->entries) {
        if (!x->slider) continue;
        if (x->key.size() == pre.size() + 2 && starts(x->key, pre + "_")) {
            const char comp = x->key.back();
            if (comp == 'H') h = x; else if (comp == 'S') s = x; else if (comp == 'V') v = x;
        }
    }
    if (!(h && s && v)) return false;
    out[0] = h; out[1] = s; out[2] = v;
    return true;
}
void dots_update(const GroupRef& group, Obj param) {            // dots of one page's rows (param = that page's dialog) (:4094)
    if (!group || !param || !edit.marks_on) return;
    const Marks m = edit.di ? Marks{} : marks();
    const std::string glyph = "<color FF3A3A>\xE2\x97\x8F</color>";
    for (size_t i = 0; i < group->entries.size(); ++i) {
        if ((int)i + 1 > ROWS) break;
        const EntryRef& e = group->entries[i];
        const bool want = e->mark_fn && e->mark_fn(m);
        Obj ctrl = be.opt_row_control ? be.opt_row_control(e->id, param) : nullptr;
        if (ctrl) {                                             // the unit panel holds 3 prefabs (slider / spin / button): the visible one
            Obj c = be.ui_child(ctrl);
            while (c) {
                const std::string nm = be.ui_name(c);
                if (nm == "p_Slider_0_s" || nm == "p_Spin_spintext_h" || nm == "p_GroupItem_button") {
                    bool av = false;
                    if (be.ui_actual_visible(c, av) && av) { ctrl = c; break; }
                }
                c = be.ui_next(c);
            }
        }
        Obj pnl = ctrl ? find_name(ctrl, "p_InputGuide_0_dUIOptionExtraButton", 0) : nullptr;
        Obj tx = pnl ? find_name(pnl, "e_text_0", 0) : nullptr;
        if (pnl && tx) {
            const bool vis = be.ui_visible(pnl);
            if (want) {
                if (be.ui_message(tx) != glyph) be.ui_set_text(tx, glyph);
                double fh = 0;
                if (!be.ui_font_size(tx, fh) || std::fabs(fh - 16) > 0.5) be.ui_set_font_size(tx, 16, 16);
                double x = 0, y = 0, z = 0;
                if (!be.ui_position(pnl, x, y, z) || std::fabs(x - (-4)) > 0.5 || std::fabs(y - (-12)) > 0.5) be.ui_set_position(pnl, -4, -12, 0);
                if (be.ui_force_invisible(pnl)) be.ui_set_force_invisible(pnl, false);   // the game's lock on the extra-button hint
                if (!vis) be.ui_set_visible(pnl, true);
            } else if (vis || !be.ui_force_invisible(pnl)) {
                be.ui_set_visible(pnl, false);
                be.ui_set_force_invisible(pnl, true);           // the game's lock back: its focus logic re-shows the panel otherwise
            }
        }
    }
}
}  // namespace

void frame() {
    di_tick();
    const int64_t now_f = frame_now();
    // slider pages: Backspace targets are taken when one is entered
    GroupRef cur_group;
    if (edit.active && edit.has_page) cur_group = group_by_key(edit.page);
    const bool on_sliders = cur_group && cur_group->slider_page;
    if (on_sliders && !edit.page_backup) {
        edit.page_backup = true;                                // page entered: Backspace targets below
        edit.has_entry_values = true; edit.entry_values.clear();   // Backspace / Y target, by group / entry key
        for (auto& g : edit.groups) for (auto& e : g->entries) edit.entry_values[g->key + "/" + e->key] = e->has_value ? std::optional<int>(e->value) : std::nullopt;
        edit.selected = false; edit.has_selected_key = false; edit.selected_targets = nullptr;
    }
    if (!on_sliders && edit.page_backup) { edit.page_backup = false; edit.has_entry_values = false; edit.entry_values.clear(); edit.selected = false; edit.has_selected_key = false; edit.selected_targets = nullptr; }
    // the focused row's colours pulse on the model - except the rows of the colour being edited
    const bool want_hl = edit.active && window_open();
    bool hl_changed = false;
    if (want_hl) {
        if (now_f % 5 == 0) {
            EntryRef fe = focused_entry();
            TargetsRef t = fe ? fe->targets : nullptr;
            if (t && edit.selected && edit.selected_targets && same_targets(t, edit.selected_targets)) t = nullptr;
            if (t != edit.hl_targets) { edit.hl_targets = t; hl_changed = true; }
        }
        // 5 steps per second on the clock: each step re-patches the whole preview
        const double now = be.clock ? be.clock() : 0.0;
        if (now - (edit.has_hl_t ? edit.hl_t : 0.0) >= 0.2) {
            edit.has_hl_t = true; edit.hl_t = now;
            const double t = lua_mod(now, 1.2) / 1.2 * 2 * 3.14159265358979323846;   // 1.2 s per pulse
            double k = 0.15 + 0.85 * std::pow(0.5 - 0.5 * std::cos(t), 0.6);        // steeper rise: longer near pure white
            k = lfloor(k * 100 + 0.5) / 100;
            // a pulse step only matters when something pulses
            if (!edit.has_hl_k || k != edit.hl_k) { edit.has_hl_k = true; edit.hl_k = k; if (edit.hl_targets && !edit.hl_targets->empty()) hl_changed = true; }
        }
        if (!edit.hl_on) { edit.hl_on = true; hl_changed = true; }
    } else if (edit.hl_on) {
        edit.hl_on = false; edit.hl_targets = nullptr; hl_changed = true;
    }
    if (hl_changed && edit.active) editor_refresh();
    if (on_sliders && window_open()) {
        if (now_f % 5 == 0) {
            EntryRef fe = focused_entry();
            if (fe && cur_group) for (size_t i = 0; i < cur_group->entries.size(); ++i) if (cur_group->entries[i] == fe) { edit.has_sel_index = true; edit.sel_index = (int)i; }
        }
        const uint32_t mask = pad_mask();
        const bool validate = key_down(0x46) || (mask & PAD.A) != 0;      // F / A (the game's decide)
        const bool reset_k = key_down(0x08) || (mask & PAD.R3) != 0;      // Backspace / R3
        const bool l3 = (mask & PAD.L3) != 0;
        const bool copy_k = key_down(0x43) || ((mask & PAD.BTN) != 0 && !l3);          // C / Square = the line
        const bool copy_color_k = key_down(0x58) || ((mask & PAD.BTN) != 0 && l3);     // X / L3 + Square = the colour
        const bool paste_k = key_down(0x56) || (mask & PAD.Y) != 0;       // V / Y (Triangle)
        if (edit.sliders_armed) {
            if (reset_k && !edit.reset_was_down) {
                // the tree node and the slider definition take the entry value, then the page is rebuilt from scratch
                EntryRef e = focused_entry();
                std::optional<int> v;
                if (e && edit.has_entry_values) { auto it = edit.entry_values.find(cur_group->key + "/" + e->key); if (it != edit.entry_values.end()) v = it->second; }
                if (e && e->reset_value) v = e->reset_value();              // the saved value
                if (e && (e->restore || (v && e->apply))) {
                    const int idx = index_in(cur_group, e);
                    if (e->restore) e->restore(); else e->apply(*v);
                    set_selected(e);                                        // the row just changed stops pulsing: the result shows
                    editor_refresh();
                    rebuild_same_page(idx);
                }
            }
            if (validate && !edit.validate_was_down) {                      // F / A on a palette row: the cell under the cursor is applied
                EntryRef e = focused_entry();
                if (e && e->pal) pick_swatch(e->pal, e->line, e->pal->col, e);
            }
        }
        if (edit.sliders_armed) {
            if (copy_k && !edit.copy_was_down) {                            // the row's number
                EntryRef e = focused_entry();
                if (e && e->slider) { edit.has_clip = e->has_value; edit.clip = e->value; edit.has_clip_kind = true; edit.clip_kind = "line"; }
            }
            if (copy_color_k && !edit.copy_color_was_down) {                // the focused colour: its H / S / V
                EntryRef t[3];
                if (color_triple(cur_group, focused_entry(), t)) {
                    edit.has_clip_color = true; edit.clip_color = { t[0]->value, t[1]->value, t[2]->value };
                    edit.has_clip_kind = true; edit.clip_kind = "color";
                }
            }
            edit.copy_color_was_down = copy_color_k;
            if (paste_k && !edit.paste_was_down && edit.has_clip_kind && edit.clip_kind == "color" && edit.has_clip_color) {
                EntryRef e = focused_entry();
                EntryRef t[3];
                const bool has_t = color_triple(cur_group, e, t);
                int idx = 0; bool changed = false;
                if (e) idx = index_in(cur_group, e);
                if (has_t) {
                    const int vals[3] = { edit.clip_color.H, edit.clip_color.S, edit.clip_color.V };
                    for (int i = 0; i < 3; ++i) {
                        const EntryRef& x = t[i];
                        if (x->apply) { const int v = std::max(x->mn, std::min(x->mx, vals[i])); x->apply(v); x->value = v; x->has_value = true; changed = true; }
                    }
                }
                if (changed) {
                    set_selected(e);
                    editor_refresh();
                    rebuild_same_page(idx);
                }
            } else if (paste_k && !edit.paste_was_down && edit.has_clip) {
                EntryRef e = focused_entry();
                if (e && e->slider && e->apply) {
                    const int v = std::max(e->mn, std::min(e->mx, edit.clip));
                    e->apply(v); e->value = v; e->has_value = true;
                    set_selected(e);
                    const int idx = index_in(cur_group, e);
                    editor_refresh();
                    rebuild_same_page(idx);
                }
            }
        }
        edit.copy_was_down = copy_k; edit.paste_was_down = paste_k;
        edit.sliders_armed = true;                              // the press that opened the page (A / decide) must be released first
        edit.validate_was_down = validate; edit.reset_was_down = reset_k;
    } else {
        edit.sliders_armed = false;
        edit.validate_was_down = false; edit.reset_was_down = false;
    }
    // the dialog's guide bar: our hints on slider pages, the verb of the focused row elsewhere
    if (edit.active && window_open()) {
        const bool pad = be.screen_pad && be.screen_pad();
        EntryRef fe = focused_entry();
        if (on_sliders) {
            edit.has_guide_desc = true; edit.guide_desc = fe ? fe->desc : std::string();     // quick edit: the family's clusters
            if (fe && fe->pal) edit.guide_desc = L("slider.palette_desc", { pad ? "<ICON BtnD>" : "<ICON KeyF>" });   // the decide key's own icon
            edit.guide_wanted = pad
                ? ("<ICON BtnR> " + L("verb.back") + "   <ICON BtnR3> " + L("verb.reset_value") + "   <ICON BtnL> " + L("verb.copy_line") + "   <ICON BtnL3><ICON BtnL> " + L("verb.copy_color") + "   <ICON BtnU> " + L("verb.paste"))
                : ("<ICON KeyEsc> " + L("verb.back") + "   <ICON KeyBS> " + L("verb.reset_value") + "   <ICON KeyC> " + L("verb.copy_line") + "   <ICON KeyX> " + L("verb.copy_color") + "   <ICON KeyV> " + L("verb.paste"));
        } else {                                                // hints; the focused row's description goes below
            edit.has_guide_desc = true; edit.guide_desc = fe ? fe->desc : std::string();
            std::string verb = (fe && fe->has_verb) ? fe->verb : (is_nav(fe) ? "Enter" : "Select");
            verb = L("verb." + lower(verb));
            edit.guide_wanted = pad ? ("<ICON BtnD> " + verb + "    <ICON BtnR> " + L("verb.back")) : ("<ICON KeyF> " + verb + "    <ICON KeyEsc> " + L("verb.back"));
        }
        edit.has_guide_wanted = true;
    } else {
        edit.has_guide_wanted = false; edit.guide_wanted.clear();
    }
    if (edit.has_guide_wanted && now_f % 5 == 0 && be.guide_bar)
        be.guide_bar(edit.guide_wanted, edit.has_guide_desc ? edit.guide_desc : std::string(), be.opt_current_param ? be.opt_current_param() : nullptr);
    // status messages (SAVE OK! / SAVED AS MC n), then the page title again
    if (edit.active && edit.has_status) {
        if (!edit.status.empty() && now_f >= edit.status[0].until) {
            const Status st = edit.status[0];
            edit.status.erase(edit.status.begin());
            if (st.close) { edit.has_title_override = false; edit.title_override.clear(); if (be.opt_close) be.opt_close(); }
            if (st.restore) {
                edit.has_title_override = false; edit.title_override.clear();
                const std::string root_key = edit.di ? std::string(DI_ROOT) : std::string(ROOT);
                if (window_open() && edit.has_page && edit.page == root_key) {
                    editor_build_pages();
                    if (be.opt_navigate) be.opt_navigate(root_key, kTrue, focus_of_or0(root_key), kNil, kNil, kNil);   // normal title back (replace)
                    edit.has_title_hold = true; edit.title_hold = frame_now() + 15;
                }
            }
            if (edit.status.empty()) edit.has_status = false;
        }
    }
    // unsaved-change dots: never within 15 frames of a page change
    if (edit.active && window_open() && now_f % 5 == 0 && now_f > (edit.has_title_hold ? edit.title_hold : 0) && cur_group && edit.marks_on)
        dots_update(cur_group, be.opt_current_param ? be.opt_current_param() : nullptr);
    const bool visible = be.screen_visible && be.screen_visible();
    if (g_reopen_now && !edit.active && !window_open() && visible && !(be.screen_busy && be.screen_busy())) {
        g_reopen_now = false;                                   // SAVE AS: list reloaded, cursor on the new colour
        open();
    }
    // the camera gate (owner "dialogs_camera"): the gate state every 5 frames while it holds
    const bool hints_on = edit.active && window_open() && visible;
    if (hints_on) {
        if (now_f % 5 == 0 && be.camera_open) be.camera_open(be.screen_pad && be.screen_pad(), L("hint.move"), L("hint.zoom"));
        edit.hints_shown = true;
    } else if (edit.hints_shown) {
        edit.hints_shown = false; if (be.camera_close) be.camera_close();
    } else if (visible && now_f % 30 == 0) {                    // the screen can re-show its hint blocks: hide only, no camera restore
        if (be.camera_hide_hints) be.camera_hide_hints();
    }
    // Esc / B on a sub-page: the game closes the current one, the parent is already up
    if (edit.active && window_open() && cur_group && cur_group->has_parent) {
        const uint32_t mask = pad_mask();
        const bool cancel = key_down(0x1B) || (mask & PAD.B) != 0;
        if (cancel && !edit.cancel_was_down && (now_f - (edit.has_esc_nav_frame ? edit.esc_nav_frame : -1000)) > 20) {
            edit.has_esc_nav_frame = true; edit.esc_nav_frame = now_f; edit.has_title_hold = true; edit.title_hold = now_f + 15;
            dots_update(group_by_key(cur_group->parent_key), be.opt_parent_param ? be.opt_parent_param() : nullptr);   // the parent's dots follow before it comes back up
            edit.has_page = true; edit.page = cur_group->parent_key;
        }
        edit.cancel_was_down = cancel;
    } else {
        edit.cancel_was_down = false;
    }
    // page turning inside the editor: the game's previous / next keys (A / E on an AZERTY keyboard, Q / E on a
    // QWERTY one) or L1 / R1, wrapping
    if (edit.active && window_open()) {
        const uint32_t mask = pad_mask();
        bool prev_down = key_down(page_key(false).vk) || (mask & PAD.L1) != 0;
        bool next_down = key_down(page_key(true).vk) || (mask & PAD.R1) != 0;
        auto lit = edit.has_page ? edit.pages.find(edit.page) : edit.pages.end();
        bool has_link = lit != edit.pages.end();
        std::pair<std::string, std::string> link = has_link ? lit->second : std::pair<std::string, std::string>{};
        // mouse: a click on the title row, left or right of the title (where the A / E glyphs sit)
        double mx = 0, my = 0, dw = 0, dh = 0;
        if (has_link && be.mouse && be.mouse(mx, my, dw, dh)) {
            const double sx = dw / 1920, sy = dh / 1080;
            if (my > 190 * sy && my < 252 * sy && mx > 40 * sx && mx < 1020 * sx && (mx < 470 * sx || mx > 590 * sx)) {
                if (key_down(0x01)) { if (mx < 470 * sx) prev_down = true; else next_down = true; }
            }
        }
        const bool turn = has_link && ((prev_down && !edit.prev_was_down) || (next_down && !edit.next_was_down && !prev_down));
        if (turn) {
            const bool pad = be.screen_pad && be.screen_pad();
            bool rebuild_tree = false;
            if (pad != edit.built_pad) {                        // device changed: titles carry the other glyphs
                edit.built_pad = pad;
                editor_build_pages();                           // definitions only; the engine tree is rebuilt after End()
                auto l2 = edit.pages.find(edit.page);
                if (l2 != edit.pages.end()) link = l2->second;
                rebuild_tree = true;
            }
            const std::string target = (prev_down && !edit.prev_was_down) ? link.first : link.second;
            if (!rebuild_tree) editor_build_pages();
            edit.has_page = true; edit.page = target;
            if (be.opt_navigate) be.opt_navigate(target, kTrue, kNoFocus, kNil, kNil, kNil);
        }
        edit.prev_was_down = prev_down; edit.next_was_down = next_down;
    } else {
        edit.prev_was_down = false; edit.next_was_down = false;
    }
    // the palette rows (0.21.0): the cursor keeps its column from one palette row to another; moving never changes the
    // colour, F applies the cell under the cursor (above)
    g_grid_page = false;
    if (edit.active && window_open() && cur_group) for (auto& e : cur_group->entries) if (e->pal) { g_grid_page = true; break; }
    // ONE cursor owned by the editor: its line is the palette row the game focuses (UP / DOWN = the game's own row
    // navigation), its column moves on LEFT / RIGHT read here (pressed: one step, held: one step every 4 frames after
    // 15), clamped at the ends like the avatar grid
    EntryRef pal_fe;
    if (g_grid_page) {
        pal_fe = focused_entry();
        edit.pal_focus = pal_fe;
        bool left = false, right = false;
        if (pal_fe && pal_fe->pal && be.select_lr) be.select_lr(left, right);
        const bool lr = left != right;
        if (lr) {
            const bool step = !edit.lr_was_down || (now_f - edit.lr_since >= 15 && (now_f - edit.lr_since) % 4 == 3);
            if (!edit.lr_was_down) edit.lr_since = now_f;
            if (step) pal_fe->pal->col = std::max(0, std::min(kPaletteCols - 1, pal_fe->pal->col + (right ? 1 : -1)));
        }
        edit.lr_was_down = lr;
    } else {
        if (!edit.pal_focus.expired()) edit.pal_focus.reset();
        edit.lr_was_down = false;
    }
    // the rows' own look (0.20.0, dialogs/row_look): the rainbow under the hue sliders, the palette's cells (0.21.0)
    if (edit.active && window_open() && cur_group && be.row_looks) {
        std::vector<RowLook> looks;
        for (auto& e : cur_group->entries) {
            if (!e->slider && !e->pal) continue;
            RowLook r; r.eid = e->id; r.kind = e->look;
            if (e->pal) {
                r.line = e->line;
                r.cursor = e->pal->col;
                const auto hit = palette_hit(e->pal->get_col());
                r.checked = hit.first == e->line ? hit.second : -1;
                r.focused = pal_fe == e;
            }
            looks.push_back(r);
        }
        be.row_looks(be.opt_current_param ? be.opt_current_param() : nullptr, looks);
    }
    // window closed (Esc or SAVE): drop the working copy, the preview goes back to the real selection
    if (edit.active && !window_open() && !edit.opened && edit.has_reopen_deadline && frame_now() > edit.reopen_deadline) {
        edit.has_reopen_deadline = false;
        edit.has_closed_at = true; edit.closed_at = frame_now();
        editor_teardown();
        editor_refresh();
    }
    if (edit.active) {
        if (window_open()) { edit.opened = true; edit.has_reopen_deadline = false; editor_layout(); }   // every frame: the game cascades stacked dialogs
        else if (edit.opened) {
            bool has_back = false; std::string back_key;
            for (auto& g : edit.groups) if (edit.has_page && g->key == edit.page) { has_back = g->has_parent; back_key = g->parent_key; }
            if (has_back && (frame_now() - (edit.has_esc_nav_frame ? edit.esc_nav_frame : -1000)) < 40) {
                edit.opened = false;                            // popped natively; if nothing is up soon, close for good
                edit.has_reopen_deadline = true; edit.reopen_deadline = frame_now() + 20;
            } else if (has_back) {                              // Esc = one level up (fallback)
                edit.has_page = true; edit.page = back_key; edit.opened = false;   // changes are kept; Backspace resets a row
                editor_build_pages();
                if (be.opt_rebuild) be.opt_rebuild();
                if (be.opt_ensure_built) be.opt_ensure_built(); // fresh titles (swatches); dialog closed = safe
                edit.page = back_key;
                auto fo = edit.focus_of.find(back_key);
                if (be.opt_open_menu) be.opt_open_menu(back_key, fo == edit.focus_of.end() ? kNoFocus : fo->second);
            } else {
                edit.has_closed_at = true; edit.closed_at = frame_now();
                editor_teardown();
                editor_refresh();
            }
        }
    }
}

// ---------------------------------------------------------------- the harness' trace (tools/editor_harness/ref.lua's text)
namespace {
std::string q(bool b) { return b ? "true" : "false"; }
std::string qi(int64_t v) { return std::to_string(v); }
std::string qd(double v) {
    if (v == std::floor(v) && std::fabs(v) < 1e15) return fmt("%lld", (long long)v);
    return fmt("%.9g", v);
}
std::string qs(const std::string& s) { return "[" + s + "]"; }
std::string opt_s(bool has, const std::string& s) { return has ? qs(s) : "nil"; }
std::string opt_i(bool has, int64_t v) { return has ? qi(v) : "nil"; }
std::string sum(const std::vector<uint8_t>& bytes) {
    uint32_t a = 1, b = 0;
    for (uint8_t c : bytes) { a = (a + c) % 65521; b = (b + a) % 65521; }
    return fmt("%d:%04X%04X", (int)bytes.size(), b, a);
}
std::string sum_tree(const NodeRef& t) { return t ? sum(rsz::serialize(t)) : "nil"; }
std::string ids(const TargetsRef& list) {                       // colours by their place in the working tree: "p.c.k"
    if (!list) return "nil";
    std::unordered_map<const Node*, std::string> where;
    each_cc(edit.tree, [&](int pi, int ci, int k, const NodeRef&, const NodeRef&, const NodeRef& cc) { if (cc) where[cc.get()] = fmt("%d.%d.%d", pi, ci, k); });
    std::string out = "[";
    for (size_t i = 0; i < list->size(); ++i) {
        if (i) out += " ";
        auto it = where.find((*list)[i].get());
        out += it == where.end() ? "?" : it->second;
    }
    return out + "]";
}
std::string pair_s(const Chip& p) { return p ? (qi((*p)[0]) + "," + qi((*p)[1])) : "nil"; }
}  // namespace

std::string refresh_signature() {
    if (!edit.tree) return "tree=nil";
    return fmt("tree=%s hl=%s k=%s targets=%s", sum_tree(edit.tree).c_str(), q(edit.hl_on && edit.hl_targets != nullptr).c_str(),
               edit.has_hl_k ? qd(edit.hl_k).c_str() : "nil", ids(edit.hl_on ? edit.hl_targets : nullptr).c_str());
}
std::vector<std::string> debug_state() {
    std::vector<std::string> out;
    out.push_back(fmt("STATE active=%s di=%s page=%s opened=%s", q(edit.active).c_str(), q(edit.di).c_str(), opt_s(edit.has_page, edit.page).c_str(), q(edit.opened).c_str()));
    std::string st;
    for (size_t i = 0; i < edit.status.size(); ++i) {
        if (i) st += ";";
        st += qs(edit.status[i].text) + "@" + qi(edit.status[i].until) + (edit.status[i].close ? " close" : "") + (edit.status[i].restore ? " restore" : "");
    }
    out.push_back(fmt("STATE title_override=%s title_hold=%s status=%s", opt_s(edit.has_title_override, edit.title_override).c_str(), opt_i(edit.has_title_hold, edit.title_hold).c_str(), st.c_str()));
    out.push_back(fmt("STATE chip=%s saved_chip=%s", pair_s(edit.chip).c_str(), pair_s(edit.saved_chip).c_str()));
    out.push_back(fmt("STATE selected=%s sel_key=%s sel_index=%s armed=%s hl_on=%s hl_k=%s hl_targets=%s", q(edit.selected).c_str(), opt_s(edit.has_selected_key, edit.selected_key).c_str(),
        opt_i(edit.has_sel_index, edit.sel_index).c_str(), q(edit.sliders_armed).c_str(), q(edit.hl_on).c_str(), edit.has_hl_k ? qd(edit.hl_k).c_str() : "nil", ids(edit.hl_targets).c_str()));
    out.push_back(fmt("STATE clip=%s kind=%s color=%s", opt_i(edit.has_clip, edit.clip).c_str(), opt_s(edit.has_clip_kind, edit.clip_kind).c_str(),
        edit.has_clip_color ? (qi(edit.clip_color.H) + "," + qi(edit.clip_color.S) + "," + qi(edit.clip_color.V)).c_str() : "nil"));
    out.push_back(fmt("STATE guide=%s | %s", opt_s(edit.has_guide_wanted, edit.guide_wanted).c_str(), opt_s(edit.has_guide_desc, edit.guide_desc).c_str()));
    out.push_back(fmt("STATE closed_at=%s reopen_deadline=%s esc_nav_frame=%s page_backup=%s", opt_i(edit.has_closed_at, edit.closed_at).c_str(),
        opt_i(edit.has_reopen_deadline, edit.reopen_deadline).c_str(), opt_i(edit.has_esc_nav_frame, edit.esc_nav_frame).c_str(), q(edit.page_backup).c_str()));
    std::string fo;
    for (auto& [k, v] : edit.focus_of) { if (!fo.empty()) fo += " "; fo += k + "=" + qi(v); }
    out.push_back("STATE focus_of=" + fo);
    out.push_back(fmt("STATE tree=%s saved=%s original=%s", sum_tree(edit.tree).c_str(), sum_tree(edit.saved).c_str(), sum_tree(edit.original).c_str()));
    std::string blk;
    for (auto& [k, v] : edit.di_blk_of) { if (!blk.empty()) blk += " "; blk += k + "=" + qi(v); }
    out.push_back(fmt("STATE di fighter=%s simple=%s dirty=%s drv_dirty=%s show_at=%s refresh_at=%s rebuild=%s blk=%s", edit.di_fighter > 0 ? qi(edit.di_fighter).c_str() : "nil",
        q(edit.di_simple).c_str(), q(edit.di_dirty).c_str(), q(edit.drv_dirty).c_str(), opt_i(edit.has_di_show_at, edit.di_show_at).c_str(),
        opt_i(edit.has_di_refresh_at, edit.di_refresh_at).c_str(), q(edit.di_rebuild).c_str(), blk.c_str()));
    return out;
}
std::vector<std::string> debug_groups() {
    std::vector<std::string> out;
    for (auto& [k, v] : edit.pages) out.push_back(fmt("PAGES %s prev=%s next=%s", k.c_str(), v.first.c_str(), v.second.c_str()));
    Marks m;
    if (edit.marks_on) { if (!edit.di) m = marks(); out.push_back(fmt("MARKS ncol=%d nmat=%d", m.ncol, m.nmat)); }
    else out.push_back("MARKS ncol=nil nmat=nil");
    for (auto& g : edit.groups) {
        out.push_back(fmt("GROUP %s parent=%s slider_page=%s title=%s desc=%s", g->key.c_str(), opt_s(g->has_parent, g->parent_key).c_str(), q(g->slider_page).c_str(), qs(g->title).c_str(), qs(g->desc).c_str()));
        for (auto& x : g->entries) {
            const std::string mark = (x->mark_fn && edit.marks_on) ? q(x->mark_fn(m)) : "nil";
            const std::string rv = x->reset_value ? qi(x->reset_value()) : "nil";
            out.push_back(fmt("  ENTRY %s %s title=%s desc=%s min=%s max=%s value=%s verb=%s apply=%s restore=%s reset=%s targets=%s mark=%s nav=%s",
                x->key.c_str(), x->slider ? "slider" : x->choice ? "choice" : "button", qs(x->title).c_str(), qs(x->desc).c_str(), x->slider ? qi(x->mn).c_str() : "nil", x->slider ? qi(x->mx).c_str() : "nil",
                x->has_value ? qi(x->value).c_str() : "nil", opt_s(x->has_verb, x->verb).c_str(), q((bool)x->apply).c_str(), q((bool)x->restore).c_str(), rv.c_str(),
                ids(x->targets).c_str(), mark.c_str(), q(is_nav(x)).c_str()));
        }
    }
    return out;
}
std::string status() {
    return fmt("active=%d di=%d groups=%d page=%s", (int)edit.active, (int)edit.di, (int)edit.groups.size(), edit.has_page ? edit.page.c_str() : "-");
}

}  // namespace dialogs::editor::core
