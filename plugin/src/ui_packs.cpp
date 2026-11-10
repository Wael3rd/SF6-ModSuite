// SF6 Slots -- the interface packs in Options > Interface (Wael 10/11: every Fluffy option of an interface mod chosen inside the
// game, "never make an interface: use what the game has"). The mods of reframework\ui_mods, as the loader lists them
// (reframework/data/SF6_UISlots_Data/registry.json; the rules: ui_packs_logic.hpp).
//
// One section per pack in the left list, just above Other Settings, in the registry's order (as "Battle HUD"): its name the pack's,
// its description "by <author>" (else the pack's own description, else ours). A pack of more rows than the game's right list has
// parts (25) is cut into sections "<name> 1", "<name> 2"..., between its top-level headings (ui_packs_logic.hpp's split_rows). Its right side is the game's own rows, made and run
// by the Native Core (core 0.9.10's opt_section_place_rows): the game draws them, moves the focus, plays the sounds, shows the
// description line and cycles the values with its arrows; a long title or value slides in its place (name_scroll.cpp). A one-of
// group is one row (its values: the game's own files, then its options), a check box option one row (the game's Off / On), a
// heading that holds other rows a header row when the core has them. A row's title is its own name; two of a section with the same:
// the nearest heading that tells them apart in front, else their archives', else what their options share (unique_titles).
//
// A row changed (the core's on_value, game thread): settings.json's "ui_choices" written (costumes.cpp's writer: every other key
// kept, the other packs' choices as the file has them) and the loader told at once (amd_ags_x64.dll's SF6UISlots_Apply with the
// whole ui_choices object; then ui_retexture for the textures the game keeps in memory). An older loader without the export: the
// choice from the next launch (logged once). settings.json edited by hand: the rows follow it (opt_set_value).
//
// The picture of the focused row's value (a box's option whatever its value; the game's own files: none), else of the pack, in
// the right panel under the game's rows, the panel's width, when there is room (none: no picture): one texture copy of the
// Options screen's own parts (R19: made inside the guide list's update, fs_name.cpp's hook), under its main control, found again
// by name (R35); every write from LateUpdateBehavior, through the core (R1). The focused row: the core's section state
// (focused_entry, core 0.9.10: each section gives its own, 0 when the cursor is on another's), else the row changed last.
// Without opt_section_place_rows (an older core): no section, one log line; the choices in settings.json still apply at launch.
//
// A pack of portraits per character (a one-of group per character: ui_packs_logic.hpp's grid_of, Wael 10/11) is no rows: one section
// whose page is the grid of characters (ui_packs_grid.cpp), each character cycling its own portrait; the pack's other options, if
// any, stay rows in a section of their own ("<pack>: Other Options").
#include "ui_packs_grid.hpp"
#include "ui_retexture.hpp"
#include "slots.hpp"
#include "slots_ui.hpp"
#include "options_page.hpp"
#include "ui_packs_logic.hpp"
#include "words.hpp"
#include "json.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdio>

namespace slots::uipacks {
namespace {

using namespace optpage;
using words::G;
using words::W;
using words::game;
using words::tr;
using words::trf;

constexpr const char* kRegistry = "reframework/data/SF6_UISlots_Data/registry.json";
constexpr const char* kSettings = "reframework/data/SF6_CostumeSlots_data/settings.json";
constexpr int32_t kTabInterface = 1;        // app.Option.TabType
constexpr int32_t kSectionOthers = 290;     // app.Option.GroupType OthersSettings_Interface ("Other Settings")
constexpr uint32_t kFilesEvery = 120;       // frames between looks at registry.json and settings.json while no pack is on screen
constexpr uint32_t kLayoutEvery = 10;       // frames between looks at where the game's rows end, while a pack is on screen
constexpr uint32_t kCopyAskEvery = 120;
constexpr int kCopyRefusedMax = 3;
constexpr const char* kPic = "e_slots_ui_pic";      // the picture (not "p_...": never taken for a UI part, R20)
constexpr float kPicMargin = 20, kPicMinH = 150, kRowPitch = 70;
constexpr size_t kRowsMax = 25;             // the game's right list's parts (read 08/11): a section shows no more rows
constexpr int64_t kPrioPic = 60010;         // over every sibling (theirs: 0)

// ---- the core's increment 14 (core 0.9.10), until sf6core_api.h declares it ----
// Assumed (the core's author's signature): opt_section_place_rows is the API table's next member after opt_section_hold; the section
// state gets `int64_t focused_entry` as its next field (0 = none, entry ids start at 1); a header row is entry kind 3 (refused, -1,
// by a core without it). Once the header declares them, the header's own are used (the requires-clauses below).
using PlaceRowsFn = SF6CoreStatus (SF6CORE_CALL*)(SF6CoreClient*, int64_t, int32_t, int32_t);
template <class Api> PlaceRowsFn place_rows_of(const Api* a) {
    if constexpr (requires { a->opt_section_place_rows; }) {                // SF6CORE_HAS, on the template's own type
        return a->struct_size >= offsetof(Api, opt_section_place_rows) + sizeof(a->opt_section_place_rows) ? a->opt_section_place_rows : nullptr;
    } else {
        struct V14 { Api base; PlaceRowsFn opt_section_place_rows; };
        if (a->struct_size < offsetof(V14, opt_section_place_rows) + sizeof(PlaceRowsFn)) return nullptr;
        return reinterpret_cast<const V14*>(a)->opt_section_place_rows;
    }
}
constexpr int64_t kNoField = INT64_MIN;     // focused_entry not given (an older core)
template <class St> bool section_state(const SF6CoreAPI* a, int64_t group, St& out, int64_t& focused_entry) {
    if constexpr (requires { out.focused_entry; }) {
        out = St{};
        out.struct_size = sizeof out;
        out.focused_entry = kNoField;
        const bool ok = a->opt_section_state(C(), group, &out) == SF6CORE_OK;
        focused_entry = out.focused_entry;
        return ok;
    } else {
        struct V14 { St base; int64_t focused_entry; } v{};
        v.base.struct_size = sizeof v;          // a core that knows less fills only its own fields
        v.focused_entry = kNoField;
        const bool ok = a->opt_section_state(C(), group, &v.base) == SF6CORE_OK;
        out = v.base;
        focused_entry = v.focused_entry;
        return ok;
    }
}
constexpr int32_t kOptHeader = 3;           // SF6CORE_OPT_HEADER, as assumed

// ---- the packs ----
struct Sec {
    int pack = -1;                          // its pack in g_reg (a pack of more than kRowsMax rows has several sections)
    int64_t group = -1;
    std::vector<Row> rows;
    std::vector<int64_t> entries;           // per row (-1: not made)
    std::vector<int> values;                // per row, what the core was given / told (1-based)
};
// A pack shown as a grid of characters: the core's section with a page of ours (ui_packs_grid.cpp)
struct GridSec {
    int pack = -1;
    int64_t group = -1;
    Grid grid;
};
uint32_t g_frame = 0;
std::vector<GridSec> g_grids;               // the packs' grids, in order (their addresses stay until the next drop_sections)
bool g_setup = false, g_off = false;        // setup ran; no section possible (an older core)
PlaceRowsFn g_place_rows = nullptr;
bool g_headers = false;                     // the game has no title-only row (core 0.9.10, increment 14): headings fold into titles
Registry g_reg;
uint64_t g_reg_stamp = 0, g_set_stamp = 0;
bool g_reg_read = false;
uint32_t g_files_at = 0;
uint32_t g_words = 0;                       // the language the sections were written in (words::version)
std::vector<Sec> g_secs;                    // the packs' sections, in order
Choices g_choices;                          // settings.json's ui_choices, as last read
int g_last_sec = -1, g_last_row = -1;       // the row changed last
int g_active = -1;                          // the section whose rows are on the right side (-1 none)
std::string g_pic_shown = "-";              // what the picture shows now ("" nothing), for the log
// the picture
std::atomic<int> g_copy_status{ 1 };
uint32_t g_copy_asked = 0, g_layout_at = 0;
int g_copy_refused = 0;
float g_pic_x = 0, g_pic_y = 0, g_pic_w = 0, g_pic_h = 0;   // its place (centre), size; 0 wide: no room
void* g_laid_cm = nullptr;
Painter g_paint{ "ui packs" };

uint64_t stamp_of(const char* path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime) ^ fa.nFileSizeLow;
}
Choices read_choices() {
    Json root;
    root.kind = Json::Object;
    root.obj.emplace_back("ui_choices", costumes::ui_choices());
    return parse_choices(root);
}
Ticks ticks_of(const Pack& p) {
    auto it = g_choices.find(p.key);
    return it == g_choices.end() ? Ticks{} : sanitize(p, it->second);
}
// registry.json read again when its stamp changed. True when the packs changed.
bool read_registry() {
    const uint64_t st = stamp_of(kRegistry);
    if (g_reg_read && st == g_reg_stamp) return false;
    g_reg_read = true;
    g_reg_stamp = st;
    Registry r;
    if (st && !parse_registry(read_file(kRegistry), r)) logf("ui packs: registry.json unreadable: no pack");
    if (r.version > kRegistryVersion) logf("ui packs: registry.json version %d (this plugin knows %d): what it knows is read", r.version, kRegistryVersion);
    bool same = r.packs.size() == g_reg.packs.size();
    for (size_t i = 0; same && i < r.packs.size(); ++i) {
        const Pack &a = r.packs[i], &b = g_reg.packs[i];
        same = a.key == b.key && a.name == b.name && a.author == b.author && a.picture == b.picture && a.nodes.size() == b.nodes.size();
        for (size_t k = 0; same && k < a.nodes.size(); ++k) {
            const Node &x = a.nodes[k], &y = b.nodes[k];
            same = x.id == y.id && x.parent == y.parent && x.name == y.name && x.description == y.description && x.title == y.title &&
                   x.one == y.one && x.group == y.group && x.preview == y.preview && x.depth == y.depth;
        }
    }
    g_reg = std::move(r);
    return !same;
}

// ---- the loader ----
// amd_ags_x64.dll's SF6UISlots_Apply (proxy/slots_proxy.cpp): the files it serves recomputed from the whole ui_choices; the real
// files now served, -1 when its UI slots are not running. An older loader without it: nullptr.
using ApplyFn = int (*)(const char*);
ApplyFn loader_apply() {
    static ApplyFn fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE h = GetModuleHandleW(L"amd_ags_x64.dll")) fn = reinterpret_cast<ApplyFn>(GetProcAddress(h, "SF6UISlots_Apply"));
        logf("ui packs: the loader's SF6UISlots_Apply %s", fn ? "found: a choice is served at once"
                                                            : "not found (older loader): a choice waits for the next launch");
    }
    return fn;
}
// Right after each SF6UISlots_Apply call (whatever it answered): what follows the loader's new map: the textures the game
// keeps in memory (loaded at boot, never asked again) are swapped on the screen's elements by ui_retexture.
void on_ui_choices_applied() {
    ui_retexture::refresh_from_loader();   // the textures the game keeps in memory follow the new choices (ui_retexture.cpp)
}
// One pack's ticks changed: settings.json (the file's other packs as they are) and the loader
void save_and_apply(const Pack& p, const Ticks& t, const char* why) {
    Choices c = read_choices();
    set_pack(c, p.key, t);
    g_choices = c;
    const Json j = choices_json(c);
    costumes::set_ui_choices(j);
    g_set_stamp = stamp_of(kSettings);
    logf("ui packs: \"%s\": %zu option(s) ticked (%s)", p.name.c_str(), t.size(), why);
    if (ApplyFn fn = loader_apply()) {
        const int n = fn(json_write(j).c_str());
        static bool not_running_logged = false;
        if (n < 0 && !not_running_logged) { not_running_logged = true; logf("ui packs: the loader's UI slots are not running at this launch: the choice from the next one"); }
        else if (n >= 0) logf("ui packs: the loader serves %d file(s) now", n);
        on_ui_choices_applied();
    }
}

// ---- the sections ----
void drop_sections() {
    const SF6CoreAPI* a = A();
    gridpage::drop();
    for (Sec& s : g_secs) {
        if (s.group < 0) continue;
        if (g_place_rows) g_place_rows(C(), s.group, -1, -1);
        a->opt_remove_group(C(), s.group);
    }
    g_secs.clear();
    for (GridSec& gs : g_grids) {
        if (gs.group < 0) continue;
        if (SF6CORE_HAS(a, opt_section_place)) a->opt_section_place(C(), gs.group, -1, -1);
        a->opt_remove_group(C(), gs.group);
    }
    g_grids.clear();
    g_active = -1;
}
std::string section_desc(const Pack& p) {
    if (!p.author.empty()) return trf(W::UiBy, { p.author });
    for (const Node& n : p.nodes) if (n.depth == min_depth(p) && !n.description.empty()) return one_line(n.description);
    return tr(W::UiSectionDesc);
}
std::string row_desc(const Pack& p, const Row& r) {
    const std::string d = r.head >= 0 ? one_line(p.nodes[static_cast<size_t>(r.head)].description) : std::string();
    if (!d.empty()) return d;
    if (r.kind == Row::Group) return tr(W::UiDescGroup);
    if (r.kind == Row::Box) return trf(W::UiDescBox, { game(G::On) });
    return std::string();
}
// Every pack's sections and rows made again (the old ones first). False: a header row was refused (made again without them).
bool make_sections() {
    const SF6CoreAPI* a = A();
    drop_sections();
    g_choices = read_choices();
    g_words = words::version();
    const std::string own = tr(W::TitleGameOwn), off = game(G::Off), on = game(G::On);
    const char* fold = tr(W::FmtNameValue);
    for (size_t pi = 0; pi < g_reg.packs.size(); ++pi) {
        const Pack& p = g_reg.packs[pi];
        const std::string desc = section_desc(p);
        GridSec gs;
        gs.pack = static_cast<int>(pi);
        gs.grid = SF6CORE_HAS(a, opt_section_place) ? grid_of(p, g_reg.game_textures, own) : Grid{};
        if (!gs.grid.empty()) {                                    // a pack of portraits per character: its grid's section
            const std::string key = "slots.ui." + p.key + ".grid";
            gs.group = a->opt_group(C(), key.c_str(), p.name.c_str(), desc.c_str());
            if (gs.group < 0) logf("ui packs: \"%s\": opt_group refused (its grid)", p.name.c_str());
            else if (a->opt_section_place(C(), gs.group, kTabInterface, kSectionOthers) != SF6CORE_OK)
                logf("ui packs: \"%s\": its grid's section refused by the core", p.name.c_str());
            else logf("ui packs: \"%s\": a grid of %zu characters (%zu option(s) in it, %zu split per character)", p.name.c_str(),
                      gs.grid.fighters.size(), gs.grid.taken.size(), gs.grid.split.size());
            g_grids.push_back(std::move(gs));
        }
        const bool grid = !g_grids.empty() && g_grids.back().pack == static_cast<int>(pi);
        std::vector<Row> all = grid ? rows_left(rows_of(p, g_headers), g_grids.back().grid) : rows_of(p, g_headers);
        const std::vector<std::vector<int>> parts = split_rows(p, all, kRowsMax);
        for (const std::vector<int>& part : parts) unique_titles(p, all, part, fold);
        const Ticks t = ticks_of(p);
        const std::string base = grid ? fold2(fold, p.name, tr(W::UiMoreOptions)) : p.name;
        for (size_t si = 0; si < parts.size(); ++si) {
            Sec s;
            s.pack = static_cast<int>(pi);
            for (int r : parts[si]) s.rows.push_back(all[static_cast<size_t>(r)]);
            const std::string name = parts.size() > 1 ? base + " " + std::to_string(si + 1) : base;
            const std::string key = "slots.ui." + p.key + (grid ? ".more" : "") + (si ? "." + std::to_string(si + 1) : std::string());
            s.group = a->opt_group(C(), key.c_str(), name.c_str(), desc.c_str());
            if (s.group < 0) { logf("ui packs: \"%s\": opt_group refused", name.c_str()); g_secs.push_back(std::move(s)); continue; }
            for (const Row& r : s.rows) {
                const std::string rd = row_desc(p, r);
                const std::vector<std::string> labels = labels_of(p, r, own, off, on);
                std::vector<const char*> ptrs;
                for (const std::string& l : labels) ptrs.push_back(l.c_str());
                SF6CoreOptEntryDesc d{};
                d.struct_size = sizeof d;
                d.kind = r.kind == Row::Header ? kOptHeader : SF6CORE_OPT_CHOICE;
                const std::string ek = p.key + "." + (r.nodes.empty() ? std::string() : p.nodes[static_cast<size_t>(r.nodes[0])].id);
                d.key = ek.c_str();
                d.title = r.title.c_str();
                d.desc = rd.c_str();
                d.options = ptrs.empty() ? nullptr : ptrs.data();
                d.n_options = static_cast<uint32_t>(ptrs.size());
                d.value = std::max(1, value_of(p, r, t));
                const int64_t id = a->opt_entry(C(), s.group, &d);
                if (id < 0 && r.kind == Row::Header) {              // this core has no header rows: the sections without them
                    logf("ui packs: the core has no header rows: none");
                    g_headers = false;
                    g_secs.push_back(std::move(s));
                    return false;
                }
                if (id < 0) logf("ui packs: \"%s\": the row \"%s\" refused by the core", name.c_str(), r.title.c_str());
                s.entries.push_back(id);
                s.values.push_back(value_of(p, r, t));
            }
            if (g_place_rows(C(), s.group, kTabInterface, kSectionOthers) != SF6CORE_OK)
                logf("ui packs: \"%s\": its section refused by the core", name.c_str());
            g_secs.push_back(std::move(s));
        }
        if (parts.size() > 1) logf("ui packs: \"%s\": %zu rows, in %zu sections", p.name.c_str(), all.size(), parts.size());
    }
    a->opt_rebuild(C());
    a->opt_ensure_built(C());
    size_t rows = 0;
    for (const Sec& s : g_secs) rows += s.rows.size();
    logf("ui packs: %zu pack(s), %zu section(s) of rows and %zu grid(s) in Options > Interface, above Other Settings (%zu row(s)%s)",
         g_reg.packs.size(), g_secs.size(), g_grids.size(), rows, g_headers ? "" : ", no header rows");
    return true;
}
void build() {
    if (!make_sections()) make_sections();                   // again without header rows
}
// settings.json changed elsewhere: the rows show it
void follow_settings() {
    const SF6CoreAPI* a = A();
    g_choices = read_choices();
    for (size_t i = 0; i < g_secs.size(); ++i) {
        Sec& s = g_secs[i];
        if (s.pack < 0 || s.pack >= static_cast<int>(g_reg.packs.size())) continue;
        const Pack& p = g_reg.packs[static_cast<size_t>(s.pack)];
        const Ticks t = ticks_of(p);
        for (size_t r = 0; r < s.rows.size() && r < s.entries.size(); ++r) {
            const int v = value_of(p, s.rows[r], t);
            if (s.entries[r] < 0 || s.rows[r].kind == Row::Header || v == s.values[r]) continue;
            if (SF6CORE_HAS(a, opt_set_value) && a->opt_set_value(C(), s.entries[r], v) == SF6CORE_OK) s.values[r] = v;
        }
    }
}
// The section and row of an entry (false: not ours)
bool find_entry(int64_t entry, int& sec, int& row) {
    if (entry <= 0) return false;
    for (size_t i = 0; i < g_secs.size(); ++i)
        for (size_t r = 0; r < g_secs[i].entries.size(); ++r)
            if (g_secs[i].entries[r] == entry) { sec = static_cast<int>(i); row = static_cast<int>(r); return true; }
    return false;
}
const Pack* pack_of(const Sec& s) { return s.pack >= 0 && s.pack < static_cast<int>(g_reg.packs.size()) ? &g_reg.packs[static_cast<size_t>(s.pack)] : nullptr; }

// ---- the picture under the rows ----
void make_copy() {
    const SF6CoreAPI* a = A();
    int status = -3000;
    try {
        Obj* agent = options_agent();
        Obj* cm = agent ? call_obj(agent, "get_ControlMain") : nullptr;
        const Templates tm = templates(cm, std::string());
        if (cm && tm.t[kTTexture]) {
            status = SF6CORE_OK;
            if (!a->ui_find_child(cm, kPic, 0)) {
                void* c = nullptr;
                const SF6CoreStatus st = a->ui_clone_control(C(), tm.t[kTTexture], cm, kPic, &c);
                if (st != SF6CORE_OK || !c) status = st != SF6CORE_OK ? st : -3001;
                else { a->ui_set_force_invisible(C(), c, 1); a->ui_set_visible(C(), c, 0); }
            }
        }
    } catch (...) { status = -2000; }
    g_copy_status.store(status, std::memory_order_release);
}
// Where the game's rows end in the right panel: the last row shown, one row further (their units' places: the right list's units
// p_GroupScroll_sub_v/c_scroll/*). The picture fills the rest of the panel at 16:9, its width at most; too little: none (w 0).
void lay_out_pic(void* cm, const float area[4]) {
    const SF6CoreAPI* a = A();
    void* sub = a->ui_find_cached(cm, "p_GroupScroll_sub_v");
    void* scroll = sub ? a->ui_find_child(sub, "c_scroll", 1) : nullptr;
    float last = -1, prev = -1, pitch = 0;
    int guard = 0;
    for (Obj* u = scroll ? call_obj(static_cast<Obj*>(scroll), "get_Child") : nullptr; u && guard < 80; u = call_obj(u, "get_Next"), ++guard) {
        if (!shown(u)) continue;
        float g[3] = {};
        if (!floats_of(u, "get_GlobalPosition", g, 2)) continue;
        if (prev >= 0 && g[1] > prev && (pitch <= 0 || g[1] - prev < pitch)) pitch = g[1] - prev;
        prev = g[1];
        last = std::max(last, g[1]);
    }
    const float top = (last >= 0 ? last + (pitch > 0 ? pitch : kRowPitch) : area[1]) + kPicMargin;
    const float bottom = area[1] + area[3] - kPicMargin;
    float w = area[2] - 2 * kPicMargin, h = bottom - top;
    if (w * 9 / 16 > h) w = h * 16 / 9;
    h = w * 9 / 16;
    if (h < kPicMinH || w <= 0) { g_pic_w = g_pic_h = 0; return; }
    g_pic_w = w; g_pic_h = h;
    g_pic_x = area[0] + area[2] / 2;
    g_pic_y = top + h / 2;
}
void hide_pic(void* cm) {
    if (!cm) return;
    if (void* pic = A()->ui_find_cached(cm, kPic)) g_paint.show_if(pic, false);
    if (!g_pic_shown.empty() && g_pic_shown != "-") logf("ui packs: picture: hidden (no pack's rows on screen)");
    g_pic_shown = "";
}
// The picture of the active section's focused (else last changed) row's value, else its pack's. `what`: which, for the log.
std::string pic_res(int64_t focused_entry, std::string& what) {
    if (g_active < 0 || g_active >= static_cast<int>(g_secs.size())) return std::string();
    const Sec& s = g_secs[static_cast<size_t>(g_active)];
    const Pack* p = pack_of(s);
    if (!p) return std::string();
    int sec = -1, row = -1;
    if (focused_entry != kNoField) {
        if (!find_entry(focused_entry, sec, row) || sec != g_active) row = -1;   // the cursor on the section, or on no row of ours
    } else if (g_last_sec == g_active) {
        row = g_last_row;
    }
    if (row < 0 || row >= static_cast<int>(s.rows.size()) || s.rows[static_cast<size_t>(row)].kind == Row::Header) {
        what = "the pack \"" + p->name + "\"";
        return p->picture;
    }
    const Row& r = s.rows[static_cast<size_t>(row)];
    const int v = value_of(*p, r, ticks_of(*p));
    what = "\"" + r.title + "\" = " + (r.kind == Row::Box ? (v == 2 ? "On" : "Off") : v >= 2 ? p->nodes[static_cast<size_t>(r.nodes[static_cast<size_t>(v - 2)])].name : "the game's own");
    return picture_of(*p, r, v);
}
void paint_pic(void* cm, const SF6CoreOptSectionState& st, int64_t focused_entry) {
    const SF6CoreAPI* a = A();
    void* pic = a->ui_find_cached(cm, kPic);
    if (!pic) {
        const int s = g_copy_status.load(std::memory_order_acquire);
        if (s != 1 && s != SF6CORE_OK) { ++g_copy_refused; g_copy_status.store(1); logf("ui packs: the picture's copy not made (status %d)", s); }
        if (g_copy_refused < kCopyRefusedMax && (!g_copy_asked || g_frame - g_copy_asked >= kCopyAskEvery)) {
            g_copy_asked = g_frame;
            fsname::need_guide_hook();
            fsname::run_in_guide_update(make_copy);
        }
        return;
    }
    if (g_laid_cm != cm) {
        g_laid_cm = cm;
        g_paint.forget();
        read_enums();
        set_origin(cm);
        set_enum(pic, "set_ControlPoint(via.gui.ControlPoint)", g_cp_center);
        set_enum(pic, "set_Priority(System.UInt16)", kPrioPic);
        a->ui_set_color(C(), pic, kColWhite);
        g_layout_at = 0;
    }
    if (!g_layout_at || g_frame - g_layout_at >= kLayoutEvery) {
        g_layout_at = g_frame ? g_frame : 1;
        const float w0 = g_pic_w, h0 = g_pic_h, x0 = g_pic_x, y0 = g_pic_y;
        lay_out_pic(cm, st.area);
        if (g_pic_w > 0 && (g_pic_w != w0 || g_pic_h != h0 || g_pic_x != x0 || g_pic_y != y0)) {
            set_origin(cm);
            place(pic, g_pic_x, g_pic_y);
            a->ui_set_size(C(), pic, g_pic_w, g_pic_h);
        }
    }
    std::string what;
    const std::string res = g_pic_w > 0 ? pic_res(focused_entry, what) : std::string();
    const bool ready = !res.empty() && g_paint.put_tex(pic, res);
    g_paint.show_if(pic, ready);
    const std::string now = ready ? res : std::string();
    if (now != g_pic_shown && (ready || res.empty())) {          // (a picture still loading: logged once shown)
        g_pic_shown = now;
        if (now.empty()) logf("ui packs: picture: none (%s)", g_pic_w > 0 ? (what.empty() ? "no pack" : what.c_str()) : "no room under the rows");
        else logf("ui packs: picture: %s (%s)", now.c_str(), what.c_str());
    }
}

} // namespace

bool on_value(int64_t entry, int64_t value) {
    int sec = -1, row = -1;
    if (!find_entry(entry, sec, row)) return false;
    Sec& s = g_secs[static_cast<size_t>(sec)];
    const Pack* p = pack_of(s);
    if (!p) return true;
    const Row& r = s.rows[static_cast<size_t>(row)];
    s.values[static_cast<size_t>(row)] = static_cast<int>(value);
    g_last_sec = sec;
    g_last_row = row;
    g_choices = read_choices();
    Ticks t = ticks_of(*p);
    if (set_value(*p, r, static_cast<int>(value), t)) save_and_apply(*p, t, r.title.c_str());
    return true;
}

void setup() {
    if (g_setup) return;
    g_setup = true;
    const SF6CoreAPI* a = A();
    g_place_rows = a && C() ? place_rows_of(a) : nullptr;
    read_registry();
    g_files_at = g_frame;
    g_set_stamp = stamp_of(kSettings);
    if (!a || !C() || !g_place_rows || !SF6CORE_HAS(a, opt_section_state) || !SF6CORE_HAS(a, opt_entry) || !SF6CORE_HAS(a, opt_rebuild)) {
        g_off = true;
        if (!g_reg.packs.empty())
            logf("ui packs: this core has no rows in an Options section (opt_section_place_rows, core 0.9.10): no Interface pack section"
                 " (the choices in settings.json still apply at launch)");
        return;
    }
    if (!g_reg.packs.empty()) build();
}

void frame() {
    ++g_frame;
    if (!g_setup || g_off) return;
    const SF6CoreAPI* a = A();
    if (!a || !C()) return;
    try {
        {                                                           // the grids' page: every frame
            std::vector<gridpage::Section> gsecs;
            for (const GridSec& gs : g_grids)
                if (gs.group >= 0 && gs.pack >= 0 && gs.pack < static_cast<int>(g_reg.packs.size()))
                    gsecs.push_back({ gs.group, &g_reg.packs[static_cast<size_t>(gs.pack)], &gs.grid });
            gridpage::frame(gsecs, [](const Pack& p) { return ticks_of(p); },
                            [](const Pack& p, const Ticks& t, const char* why) { save_and_apply(p, t, why); });
        }
        if (g_secs.empty() && g_grids.empty() && (g_frame % 15) != 0) return;   // no pack: the files looked at every 120 frames below
        // the screen's state, once (every section reads the same screen); this pack's section focused on the left
        SF6CoreOptSectionState st{};
        int64_t focused_entry = kNoField;           // the entry under the right list's cursor: each section gives its own (0: not its rows)
        bool any = false, shown = false, menu = false, grid_shown = false;
        int focused = -1;
        for (size_t i = 0; i < g_secs.size(); ++i) {
            if (g_secs[i].group < 0) continue;
            SF6CoreOptSectionState si{};
            int64_t fe = kNoField;
            if (!section_state(a, g_secs[i].group, si, fe)) continue;
            if (!any) { st = si; any = true; }
            if (si.shown) { shown = true; if (!st.shown) st = si; }
            if (fe != kNoField && (focused_entry == kNoField || focused_entry <= 0)) focused_entry = fe;
            if (si.focused) focused = static_cast<int>(i);
        }
        for (const GridSec& gs : g_grids) {                         // a grid on screen: no rebuild meanwhile (its page holds the pack)
            if (gs.group < 0) continue;
            SF6CoreOptSectionState si{};
            si.struct_size = sizeof si;
            if (a->opt_section_state(C(), gs.group, &si) == SF6CORE_OK && si.shown) grid_shown = true;
        }
        if (shown) menu = st.menu_state == 0 || st.menu_state == 1;   // MAIN, SUB: no dialog of the screen's over it
        // the pack on the right side: the one focused on the left list; kept while its rows have the cursor (the sub list); none once
        // the left list's cursor is on another section
        int fe_sec = -1, fe_row = -1;
        if (focused >= 0) g_active = focused;
        else if (focused_entry != kNoField && find_entry(focused_entry, fe_sec, fe_row)) g_active = fe_sec;
        else if (!shown || st.list_state == 0) g_active = -1;
        void* cm = shown ? st.control_main : nullptr;
        if (!shown || g_active < 0 || g_active >= static_cast<int>(g_secs.size()) || !menu) {
            if (st.control_main) hide_pic(st.control_main);
            if (!shown && !grid_shown && (!g_files_at || g_frame - g_files_at >= kFilesEvery)) {   // the files, while no pack is on screen
                g_files_at = g_frame;
                const bool packs = read_registry();
                if (packs || (g_words != words::version() && !g_reg.packs.empty())) build();
                else if (stamp_of(kSettings) != g_set_stamp) { g_set_stamp = stamp_of(kSettings); follow_settings(); }
            }
            return;
        }
        if (const Pack* p = pack_of(g_secs[static_cast<size_t>(g_active)])) {
            static const Pack* s_asked = nullptr;                   // the pack's pictures asked once it is on screen
            if (s_asked != p) {
                s_asked = p;
                picture(p->picture, "ui packs");
                for (const Node& n : p->nodes) picture(n.preview, "ui packs");
            }
        }
        if (cm) paint_pic(cm, st, focused_entry);
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logf("ui packs: EXCEPTION in a frame"); }
    }
}

} // namespace slots::uipacks
