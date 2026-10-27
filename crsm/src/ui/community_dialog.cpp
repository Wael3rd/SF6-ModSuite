// ui/community_dialog — the Community dialog ("Download Loadouts") on the core's option service (phase P4, owner "ui"),
// RecordingManagerTab.lua (RMT) 149-431 and 1564-1586 in C++. The page's state and actions are community::Page (P2: list,
// sort, pages, votes, downloads, com_state.json, tickers); the dialog's rules are ui::logic::ComDialog (pure); here: the
// option window (opt_group / opt_entry / opt_navigate / opt_open_menu, the Color Mod's pattern for per-page texts: the
// group removed and made again with the same key, then a rebuild in place), the raw keys A / E / T / R (a pad's L1 / R1 / Y / X
// for A / E / T / R since 2.2.2, its glyphs when the game's guide shows a pad's), the guide line
// ("Like / Sort") on our dialog only, the prompt close detection, the Manager's "Share loadout".
// Since 2.2 (Wael 24/10): every row label fits its row (lg::com_row_fit: the date, then the name, then the author cut),
// and the dialog's description area (GuideMessageText: the game writes the focused unit's description there, CRSM's
// "Like / Sort" line replaces it, so a unit's own description never shows) holds the focused loadout's full name when its
// row cut it and its description, above the "Like / Sort" line. Since 27/10 (Wael): "Like / Sort" are one more
// entry of the dialog's own guide row, after "F Confirm  Esc Back" (a copy of the game's last item: row_hints), the description area grows upward into the empty
// space under the list with a larger font (desc_box), longer descriptions in pages shown in turn.
//
// The one intended difference from the Lua (Wael's decision): after the list is refreshed (an answer arrived: open, sort,
// after a download) the cursor stays on its row; the Lua sent it back to row 1 (com_focus_idx written as a global).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>

#include "common.hpp"
#include "community/community.hpp"
#include "community/page.hpp"
#include "dialogs.hpp"
#include "dump.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "hookstack.hpp"
#include "internal.hpp"
#include "lang.hpp"
#include "logic.hpp"
#include "manager_page.hpp"
#include "mech/mech.hpp"
#include "model/files.hpp"
#include "model/labels.hpp"
#include "model/luajson.hpp"
#include "model/lutil.hpp"
#include "community/protocol.hpp"
#include "perf.hpp"

namespace ui {
namespace com_api { void open(); void share(); }
}

namespace ui::com {
namespace {

namespace gm = game::gm;
namespace lg = ui::logic;
using gx::MO;

constexpr const char* kKey = "Community";       // _com.KEY (RMT:151)
constexpr const char* kDesc = "Community loadouts.";   // the group's description (RMT:297)
constexpr int kKeyA = 0x41, kKeyE = 0x45, kKeyT = 0x54, kKeyR = 0x52;   // RMT:329-332
constexpr uint64_t kSetupWait = 60;              // frames a rebuilt dialog may take to show its page

bool g_pad_glyphs = false;                       // the game's guide shows a pad's glyphs (guide_pad), read on the frame
std::unique_ptr<community::Page> g_page;
std::atomic<bool> g_open_request{false}, g_share_request{false};   // the Manager's buttons (any thread): run on the frame
std::atomic<int> g_pressed_row{0};               // a row's F (the core's callback): run on the frame, as NO's button queue did
int64_t g_group = -1;
std::vector<int64_t> g_entries;                  // the 7 buttons of the group now (row i = g_entries[i - 1])
bool g_cb_set = false;
std::atomic<bool> g_open{false};
MO* g_param = nullptr;                           // our dialog's flow Param (on_page_setup with our rows), nullptr = unknown
uint64_t g_param_wait_until = 0;                 // after an open / rebuild: the new param is awaited, the window flag only
bool g_window_seen = false;                      // the core opens the window at its next frame: "no window" counts after that
uint64_t g_window_deadline = 0;
constexpr uint64_t kWindowWait = 120;            // frames for the core to open the window (it takes one)
uint64_t g_builds = 0, g_navs = 0, g_closes = 0;
bool ok_guide = false;
constexpr const char* kRowEmFile = "reframework/data/SF6_CRSMNative_data/com_row_em.txt";
constexpr const char* kDumpOut = "reframework/data/SF6_CRSMNative_data/dump.txt";
constexpr const char* kDescLinesFile = "reframework/data/SF6_CRSMNative_data/com_desc_lines.txt";
std::atomic<int> g_desc_lines{lg::kComDescLines};   // a page of the description area (com_desc_lines.txt replaces it at each open)
std::vector<std::vector<std::string>> g_extras;  // per row shown (row i = [i - 1]): all its lines above "Like / Sort"
int g_desc_row = 0;                              // the row whose description shows (ui::lock): its pages follow from g_desc_since
uint64_t g_desc_since = 0;
std::string g_guide_last;                        // the description area's row / line count journaled last (journal only)
std::atomic<double> g_row_em{lg::kComRowEm};     // what a row holds (com_row_em.txt replaces it at each open)
int64_t g_probed = -1;                           // the dump's last probed build (dump_on.txt only)

const SF6CoreAPI* api() { const SF6CoreAPI* a = gx::api(); return (a && SF6CORE_HAS(a, opt_status)) ? a : nullptr; }

// the CFN read from the game's own banner (CommunityBridge.lua 127-149), through the core's discovery
std::string banner_cfn() {
    const SF6CoreAPI* a = gx::api();
    if (!a || !SF6CORE_HAS(a, ui_find_cached)) return "";
    void* agents[4] = {};
    const uint32_t n = perf::counted(a)->ui_agents("OnlineBannerUI", agents, 4);
    for (uint32_t i = 0; i < n; ++i) {
        MO* ctrl = gx::call_obj(static_cast<MO*>(agents[i]), "get_ControlMain");
        if (!ctrl) continue;
        if (MO* t = static_cast<MO*>(perf::counted(a)->ui_find_cached(ctrl, "e_text_fid_name"))) { const std::string m = gx::msg(t); return m == "nil" ? std::string() : m; }
    }
    return "";
}

community::Page& page() {
    if (!g_page) {
        community::Page::Env env;
        env.root = "reframework/data/";
        env.current_char = [] { std::lock_guard lk(mech::lock()); const auto c = mech::files::engine().char_name(); return c ? *c : std::string(); };
        env.ticker = [](const std::string& s) { ui::dialogs::ticker(s); };
        env.packs_changed = [] {                 // invalidate_files + ensure_files + manager_dirty (RMT:276-278)
            lg::after_download(
                [] { return ui::manager::refresh_list(); },   // the Manager page (P5) re-reads the list and syncs its spin
                [] { std::lock_guard lk(mech::lock()); mech::files::engine().invalidate(); },   // (no page: at once)
                [] { mech::files::ensure(); });
        };
        env.identity = [] { return community::identity(banner_cfn()); };
        env.log = [](const std::string& s) { cmn::journal("ui: %s", s.c_str()); };
        env.tr = [](const std::string& key, const std::vector<std::string>& args) { return ui::lang::T(key, args); };   // locale.json rsm.com_*
        env.fit_label = [](const nlohmann::json& shown, const nlohmann::json& dv, const std::string& label) {
            return lg::com_row_fit(shown, dv, label, g_row_em.load());
        };
        g_page = std::make_unique<community::Page>(community::client(), std::move(env));
        g_page->load_state();                    // com_state.json (the Lua read it at load, RMT:156)
    }
    return *g_page;
}

void SF6CORE_CALL cb_button(void*, int64_t entry, SF6CoreOptButtonResult* out);
void SF6CORE_CALL cb_page(void*, void* param, uint32_t rows);
void SF6CORE_CALL cb_closed(void*);

bool ensure_callbacks() {
    if (g_cb_set) return true;
    const SF6CoreAPI* a = api();
    if (!a) { cmn::log_warn("[CRSMNative] ui: the core has no option service: no Community dialog"); return false; }
    SF6CoreOptCallbacks cb{};
    cb.struct_size = sizeof cb;
    cb.on_button = cb_button;
    cb.on_page_setup = cb_page;
    cb.on_window_closed = cb_closed;
    g_cb_set = perf::counted(a)->opt_set_callbacks(gx::cl(), &cb) == SF6CORE_OK;
    if (!g_cb_set) cmn::log_warn("[CRSMNative] ui: the core's option service refused the callbacks: no Community dialog");
    return g_cb_set;
}

// the dialog's port: the page and the option window
class Port final : public lg::ComPort {
public:
    void page_open() override { page().open(); }
    std::string title(bool pad) override { return page().title(pad); }
    bool loading() override { return page().loading(); }
    int row_count() override { return (int)page().rows().size(); }
    std::string row_label(int row) override { return page().rows()[(size_t)row - 1].label; }
    std::string row_desc(int row) override {     // RMT:305-310 (created_at, the author: author_cfn since the Worker v4)
        const auto& item = page().rows()[(size_t)row - 1].item;
        const auto cr = model::luajson::index(item, "created_at");
        std::string uploader = community::item_author(item);
        if (community::item_verified(item)) uploader += "  " + page().verified_mark() + " " + page().tr_or("com_verified", "Steam-verified");
        const std::string created = cr.is_string() ? cr.get<std::string>().substr(0, 10) : std::string();
        return "F: Download  |  " + uploader + "  " + created;
    }
    // the lines of a row above "Like / Sort": its full name when the row cut it, then its description, wrapped at the
    // area's width (shown a page of g_desc_lines at a time: guide_text)
    std::vector<std::string> row_extra(int row) {
        std::vector<std::string> out;
        const auto& r = page().rows()[(size_t)row - 1];
        const double em = lg::kComDescEm;
        const auto dn = model::luajson::index(r.item, "display_name");
        const std::string name = dn.is_string() ? model::lutil::strip_json_ext(dn.get<std::string>()) : std::string();
        if (!name.empty() && r.label.find(name) == std::string::npos) {
            const auto l = lg::desc_lines(name, em, 1);
            if (!l.empty()) out.push_back(l[0]);
        }
        const auto ds = model::luajson::index(r.item, "description");
        if (ds.is_string())
            for (const std::string& l : lg::desc_lines(ds.get<std::string>(), em, lg::kComDescMaxLines - (int)out.size())) out.push_back(l);
        return out;
    }
    bool data_changed() override { return page().data_changed(); }
    void mark_built() override { page().mark_built(); }
    int64_t version() override { return page().version(); }
    bool prev_page() override { return page().prev_page(); }
    bool next_page() override { return page().next_page(); }
    void cycle_sort() override { page().cycle_sort(); }
    void vote(int row) override { page().vote(row); }
    bool download_prompt(int row, std::string* message) override { return page().download_prompt(row, message); }
    void download_confirmed() override { page().download_confirmed(); }

    void show(const std::string& title, const std::vector<std::string>& labels, const std::vector<std::string>& descs) override {
        const SF6CoreAPI* a = api();
        if (!a || !ensure_callbacks()) return;
        if (g_group >= 0) perf::counted(a)->opt_remove_group(gx::cl(), g_group);   // the same key gives the same TypeIds (the Color Mod's pattern)
        g_entries.clear();
        g_extras.assign(labels.size(), {});
        const int built = std::min((int)labels.size(), row_count());
        for (int i = 1; i <= built; ++i) if (labels[(size_t)i - 1] != "Loading...") g_extras[(size_t)i - 1] = row_extra(i);
        g_group = perf::counted(a)->opt_group(gx::cl(), kKey, title.c_str(), kDesc);
        if (g_group < 0) { cmn::journal("ui: Community: opt_group failed"); return; }
        for (size_t i = 0; i < labels.size(); ++i) {
            const std::string key = "com_r" + std::to_string(i + 1);
            SF6CoreOptEntryDesc d{};
            d.struct_size = sizeof d;
            d.kind = SF6CORE_OPT_BUTTON;
            d.key = key.c_str();
            d.title = labels[i].c_str();
            d.desc = i < descs.size() ? descs[i].c_str() : "";
            g_entries.push_back(perf::counted(a)->opt_entry(gx::cl(), g_group, &d));
        }
        ++g_builds;
    }
    void open_menu() override {
        const SF6CoreAPI* a = api();
        if (!a || g_group < 0) return;
        g_param = nullptr;
        g_param_wait_until = tick() + kSetupWait;
        g_window_seen = false;                   // (NO set its window flag in its own tick, before RMT's: the same here)
        g_window_deadline = tick() + kWindowWait;
        // show() made the group again: its page has no unit until a build, and the core opens the window on the units
        // built when its open delay ends (the Color Mod's and the Slots Loader's open: opt_rebuild + opt_ensure_built
        // first). Without it the dialog only came with the list answer's rebuild, and an answer slower than ~300 ms
        // found the empty window already closed by the core ("Download Loadouts" did nothing). Never under another
        // client's open window.
        if (!perf::counted(a)->opt_window_open()) {
            perf::counted(a)->opt_rebuild(gx::cl());
            perf::counted(a)->opt_ensure_built(gx::cl());
        }
        perf::counted(a)->opt_open_menu(gx::cl(), kKey, -1);
    }
    void navigate(int focus0) override {         // NO.navigate(KEY, rebuild, focus, now) (RMT:327, 344, 349, 357, 388)
        const SF6CoreAPI* a = api();
        if (!a || g_group < 0 || !perf::counted(a)->opt_window_open()) return;
        g_param = nullptr;                       // the rebuilt dialog brings a new param (and ends the old one)
        g_param_wait_until = tick() + kSetupWait;
        perf::counted(a)->opt_navigate(gx::cl(), kKey, SF6CORE_OPT_NAV_REBUILD | SF6CORE_OPT_NAV_NOW, focus0 < 0 ? 0 : focus0);
        ++g_navs;
        cmn::journal("ui: Community rebuilt, cursor on row %d", focus0 + 1);
    }
    int focused_row() override {                 // the focused unit's entry -> its row (RMT:362-368)
        const SF6CoreAPI* a = api();
        if (!a) return 0;
        const int64_t e = perf::counted(a)->opt_focused_entry();
        for (size_t i = 0; i < g_entries.size(); ++i) if (g_entries[i] >= 0 && g_entries[i] == e) return (int)i + 1;
        return 0;
    }
    void confirm(const std::string& message, std::function<void(bool)> done) override {
        ui::dialogs::confirm(message, "Community", std::move(done));   // the box title (RMT:256)
    }
};

Port g_port;
lg::ComDialog g_dlg(g_port);

void closed_now(const char* why) {
    if (!g_dlg.is_open()) return;
    g_dlg.closed();
    g_open.store(false);
    g_param = nullptr;
    ++g_closes;
    cmn::journal("ui: Community dialog closed (%s)", why);
}

// our dialog still up? The Lua dropped the dialog as soon as its top handle ended, not
// when the window flag fell up to 10 ticks later: the flow handle of our param, from UIFlowManager._Handles
bool handle_alive(MO* param) {
    MO* hs = gx::ref(gx::singleton("app.UIFlowManager"), "_Handles");
    const int32_t n = gm::list_count(hs);
    for (int32_t i = 0; i < n; ++i) {
        MO* h = gm::list_item(hs, i);
        if (!h || gx::ref(h, "<Param>k__BackingField") != param) continue;
        bool ended = false;
        return !(gm::call_bool(h, "get_IsEnd", &ended) && ended);
    }
    return false;
}

void SF6CORE_CALL cb_button(void*, int64_t entry, SF6CoreOptButtonResult* out) {
    if (out) out->kind = 0;                      // nothing more (the Lua's button had refresh = false)
    if (!ui::active()) return;
    std::lock_guard lk(ui::lock());
    for (size_t i = 0; i < g_entries.size(); ++i)
        if (g_entries[i] == entry) { g_pressed_row.store((int)i + 1); return; }
}

void SF6CORE_CALL cb_page(void*, void* param, uint32_t rows) {
    if (!ui::active() || rows == 0 || !param) return;
    std::lock_guard lk(ui::lock());
    if (g_dlg.is_open()) g_param = static_cast<MO*>(param);   // a page showing our entries: our dialog
}

void SF6CORE_CALL cb_closed(void*) {
    if (!ui::active()) return;
    std::lock_guard lk(ui::lock());
    closed_now("window closed");
}

bool key_down(int vk) { return cmn::key_down(vk); }   // reframework:is_key_down: the game window's keys only

// The glyph set of the game's own guide, as the Color Mod reads it: app.InputGuideManager.GetMode(player 0), static
// (1 GamePad, 2 KeyboardAndMouse, 0 nothing to follow = nullopt: the keyboard's glyphs stay)
std::optional<bool> guide_pad() {
    static reframework::API::Method* m_get = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (gx::TD* td = gm::type("app.InputGuideManager")) m_get = gm::method(td, "GetMode(System.Int32)");
        cmn::journal("ui: Community glyphs: the game's guide mode %s", m_get ? "read from InputGuideManager.GetMode" : "not found (keyboard glyphs)");
    }
    if (!m_get || !gm::singleton("app.InputGuideManager")) return std::nullopt;
    reframework::InvokeRet r{};
    if (!gm::call(nullptr, m_get, { gm::arg_i32(0) }, &r)) return std::nullopt;
    if (r.dword == 1) return true;
    if (r.dword == 2) return false;
    return std::nullopt;
}

// -- UIFlowOptionBGDialog.Param.UpdateInputGuide post: the controls in the description area, our dialog only (RMT:1564-1586)
struct GuideCall { MO* param = nullptr; };
thread_local HookStack<GuideCall> s_guide;
int guide_pre(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    GuideCall* s = s_guide.push();
    if (s && ui::active() && g_open.load()) s->param = static_cast<MO*>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
// ---- the description area grown upward, the hints in the dialog's guide row (Wael 27/10) ---------------------------------
// Each dialog has its own tree (the Color Mod resizes the same dialog's frame and never restores it): nothing to put back.
// GuideMessageText as the game made it, learnt once per text, and the ControlPoint y that keeps its bottom edge.
struct DescBox { MO* text = nullptr; bool ok = false; float pos[3] = {}; float w = 0, h = 0, fw = 0, fh = 0; int32_t cp = -1; float y_new = 0; };
DescBox g_box;
std::string g_layout_last;                       // journal only

bool near_f(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

// The description area kComDescAreaH high (its bottom kept: it grows into the empty space under the list), its font
// kComDescScale larger: every write compared first. false (the box left as the game made it) when its geometry could
// not be read, or looked already changed (its height not under kComDescAreaH).
bool desc_box(MO* gmt) {
    if (g_box.text != gmt) {
        g_box = DescBox{};
        g_box.text = gmt;
        g_box.ok = gx::vec3_of(gmt, "get_Position", g_box.pos) && gx::size_of(gmt, "get_RegionSize", &g_box.w, &g_box.h)
                   && gx::size_of(gmt, "get_FontSize", &g_box.fw, &g_box.fh) && gx::call_i32(gmt, "get_ControlPoint", &g_box.cp)
                   && g_box.h > 1.0f && g_box.h < lg::kComDescAreaH - 1.0f && g_box.w > 100.0f && g_box.fh > 1.0f
                   && lg::keep_bottom_y(g_box.cp, g_box.pos[1], g_box.h, lg::kComDescAreaH, &g_box.y_new);
        char b[200];
        std::snprintf(b, sizeof b, "ControlPoint %d, y %.0f, %.0f x %.0f, font %.1f", (int)g_box.cp, g_box.pos[1], g_box.w, g_box.h, g_box.fh);
        cmn::journal("ui: Community description area (%s): %s", b, g_box.ok ? "grown upward, larger font" : "left as the game made it");
    }
    if (!g_box.ok) return false;
    const float fw = g_box.fw * lg::kComDescScale, fh = g_box.fh * lg::kComDescScale;
    float w = 0, h = 0, p[3] = {};
    if (!gx::size_of(gmt, "get_FontSize", &w, &h) || !near_f(w, fw, 0.05f) || !near_f(h, fh, 0.05f)) gx::set_font(gmt, fw, fh);
    if (!gx::size_of(gmt, "get_RegionSize", &w, &h) || !near_f(w, g_box.w, 0.5f) || !near_f(h, lg::kComDescAreaH, 0.5f))
        gx::set_region(gmt, g_box.w, lg::kComDescAreaH);
    if (!gx::vec3_of(gmt, "get_Position", p) || !near_f(p[0], g_box.pos[0], 0.5f) || !near_f(p[1], g_box.y_new, 0.5f))
        gx::set_pos(gmt, g_box.pos[0], g_box.y_new, g_box.pos[2]);
    return true;
}

// The first via.gui.Text with a message under e (depth <= 3), e itself first
MO* text_in(MO* e, int depth) {
    if (!e) return nullptr;
    if (gx::is_type(e, "via.gui.Text")) { const std::string m = gx::msg(e); return (m.empty() || m == "nil") ? nullptr : e; }
    if (depth >= 3) return nullptr;
    for (MO* c = gx::first_child(e); c; c = gx::next(c)) if (MO* t = text_in(c, depth + 1)) return t;
    return nullptr;
}

// The dialog's guide row ("F Confirm  Esc Back"): <Agent> -> ControlMain -> p_InputGuideList_0 -> c_guideBtn, its items
// e_text_0_* and its line e_text_guide, as the Color Mod's editor finds it. nullptr = not found.
MO* guide_btn(MO* param) {
    const SF6CoreAPI* a = api();
    MO* agent = gx::ref(param, "<Agent>k__BackingField");
    MO* cm = agent ? gx::call_obj(agent, "get_ControlMain") : nullptr;
    MO* lst = (a && cm) ? static_cast<MO*>(perf::counted(a)->ui_find_child(cm, "p_InputGuideList_0", 6)) : nullptr;
    return lst ? static_cast<MO*>(perf::counted(a)->ui_find_child(lst, "c_guideBtn", 4)) : nullptr;
}

// "T Like  R Sort" in the game's language (rsm.com_like / rsm.com_sort of CRSM's locale.json, English otherwise), the
// glyphs of the device in use
std::string row_hint_text() {
    return std::string(g_pad_glyphs ? "<ICON BtnU> " : "<ICON KeyT> ") + page().tr_or("com_like", "Like") + "   "
         + (g_pad_glyphs ? "<ICON BtnL> " : "<ICON KeyR> ") + page().tr_or("com_sort", "Sort");
}

// The hints as one more entry of the dialog's guide row (dump 27/10: c_guideBtn holds e_text_guide = GuideMessageText, the
// template e_text_0 and the game's items e_text_0_d_<n>, each ONE via.gui.Text "<ICON KeyF> Confirm" with its glyphs
// inline, ControlPoint LeftCenter, RegionSize fitted by the game to its text in the game's language, placed at
// next.x = x + RegionSize.x + FontSize.w). CRSM's entry is a copy of the game's last item (same parent, same style),
// made by the core's ui_clone_control inside app.UIFlowOptionBGDialog.Main.OnUpdate's post (R19: duplicate() only in
// the owning flow's update; the Color Mod's pattern), found again by its name, and placed by the same rule from the
// last item as it is NOW (the game re-fits its items per language). Never the template e_text_0, never the copy itself.
// A copy is not a managed object (R35): only found by name and written.
// While the game makes its row again (each cursor move: new items, not laid out yet for a few frames; 27/10 the copy was
// drawn ~550 px to the right then) the copy is hidden, and placed and shown again once the row is laid out: every item
// fitted to its text (guide_w_fits) and each one where the game's rule puts it after the one before. Tried at each
// UpdateInputGuide post, at each UIPartsInputGuideList.OnLateUpdate post while a placement waits (the list's own layout
// pass), and every 5 frames.
constexpr const char* kHintName = "e_text_crsm_hints";
constexpr int kMaxClones = 8;                    // copies asked for one dialog (the game may drop them at a rebuild)
struct RowState { MO* param = nullptr; uint64_t since = 0; bool done = false; int clones = 0; };
RowState g_row;
std::atomic<bool> g_clone_due{false};            // a copy is asked: made by the next OnUpdate post of our dialog
std::atomic<bool> g_place_due{false};            // the copy hidden, waiting for the row's layout
bool ok_update = false, ok_late = false;

// The game's items now (the visible e_text_0_d_*, left to right): the last one's x, y, width and font width, and
// whether the row is laid out (settled)
struct Item { MO* t; float x, y, w, fw; std::string msg; };
struct Row { MO* last = nullptr; float x = 0, y = 0, w = 0, fw = 0; bool settled = false; size_t n = 0; };
Row read_row(MO* btn) {
    std::vector<Item> v;
    for (MO* c = btn ? gx::first_child(btn) : nullptr; c; c = gx::next(c)) {
        if (gx::name(c).rfind("e_text_0_d_", 0) != 0 || !gx::is_type(c, "via.gui.Text") || !gx::visible(c) || gx::force_invisible(c)) continue;
        float p[3] = {}, rw = 0, rh = 0, f = 0, fh = 0;
        if (!gx::vec3_of(c, "get_Position", p) || !gx::size_of(c, "get_RegionSize", &rw, &rh) || !gx::size_of(c, "get_FontSize", &f, &fh)) continue;
        v.push_back({ c, p[0], p[1], rw, f, gx::msg(c) });
    }
    Row r;
    r.n = v.size();
    if (v.empty()) return r;
    std::stable_sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return a.x < b.x; });
    const Item& l = v.back();
    r.last = l.t; r.x = l.x; r.y = l.y; r.w = l.w; r.fw = l.fw;
    r.settled = true;
    for (size_t i = 0; i < v.size() && r.settled; ++i) {
        if (!lg::guide_w_fits(v[i].msg, v[i].w, v[i].fw)) r.settled = false;            // not fitted to its text yet
        else if (i > 0 && (!near_f(v[i].x, v[i - 1].x + v[i - 1].w + v[i - 1].fw, 1.5f) || !near_f(v[i].y, v[0].y, 0.5f)))
            r.settled = false;                                                            // not where the row puts it yet
    }
    return r;
}

// Main.OnUpdate post: the copy asked, made from the game's last item of OUR dialog (found again here: its items are made
// again at each guide update), hidden until write_guide places it
void make_hint_clone() {
    const SF6CoreAPI* a = api();
    if (!a || !SF6CORE_HAS(a, ui_clone_control) || !g_dlg.is_open() || !g_param || !handle_alive(g_param)) return;
    MO* btn = guide_btn(g_param);
    MO* tmpl = btn ? read_row(btn).last : nullptr;
    if (!tmpl || perf::counted(a)->ui_find_child(btn, kHintName, 0)) return;
    g_clone_due.store(false);
    ++g_row.clones;
    void* c = nullptr;
    const SF6CoreStatus st = perf::counted(a)->ui_clone_control(gx::cl(), tmpl, btn, kHintName, &c);
    if (st == SF6CORE_OK && c) perf::counted(a)->ui_set_visible(gx::cl(), c, 0);
    cmn::journal("ui: Community guide row entry copied from %s: %s (%d)", gx::name(tmpl).c_str(), st == SF6CORE_OK && c ? "made" : "refused", (int)st);
}
int update_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void update_post_body() {
    std::lock_guard lk(ui::lock());
    try { make_hint_clone(); } catch (...) { ui::fault("OptionBGDialog OnUpdate post"); }
}
void update_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_clone_due.load() || cmn::closing() || !ui::active()) return;   // nothing asked: one load per update
    __try { update_post_body(); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("OptionBGDialog OnUpdate post (SEH)"); }
}

// The hints entry placed after the game's last item and shown once the row is laid out, hidden while it is not: true
// when the entry belongs to the row (shown, or hidden for a rebuild: the hints never go under the description then)
bool row_hints(MO* param) {
    if (g_row.param != param) { g_row = RowState{}; g_row.param = param; g_row.since = tick(); }
    const SF6CoreAPI* a = api();
    MO* btn = a ? guide_btn(param) : nullptr;
    if (!btn) return false;
    const Row r = read_row(btn);
    MO* h = static_cast<MO*>(perf::counted(a)->ui_find_child(btn, kHintName, 0));
    if (!h) {                                    // not made yet, or dropped by the game's rebuild: asked again
        if (r.last && ok_update && g_row.clones < kMaxClones) g_clone_due.store(true);
        return false;
    }
    if (!r.last || !r.settled) {                 // the game is making its row again: never drawn at a stale place
        if (gx::visible(h)) gx::set_visible(h, false);
        g_place_due.store(true);
        return g_row.done;
    }
    const std::string text = row_hint_text();
    gx::set_text(h, text);                       // (compared first, R7)
    // its region as the game fits its own (a wide region could move a text not aligned left), its ControlPoint's y the
    // items' own (the row moves down when the description is empty)
    const float want_w = (float)lg::guide_text_w(text, r.fw) * 1.15f + 4.0f;
    float p[3] = {}, rw = 0, rh = 0;
    if (gx::size_of(h, "get_RegionSize", &rw, &rh) && !near_f(rw, want_w, 0.5f)) gx::set_region(h, want_w, rh);
    const float nx = r.x + r.w + r.fw;
    if (!gx::vec3_of(h, "get_Position", p) || !near_f(p[0], nx, 0.5f) || !near_f(p[1], r.y, 0.5f)) gx::set_pos(h, nx, r.y, p[2]);
    if (!gx::visible(h)) gx::set_visible(h, true);
    g_place_due.store(false);
    g_row.done = true;
    return true;
}

// UIPartsInputGuideList.OnLateUpdate post (any guide list): while the copy waits, our row placed right after the game's
// layout pass. Nothing else: one load per call otherwise.
void late_post_body() {
    std::lock_guard lk(ui::lock());
    try {
        if (!g_dlg.is_open() || !g_param || !handle_alive(g_param)) return;
        row_hints(g_param);
    } catch (...) { ui::fault("InputGuideList OnLateUpdate post"); }
}
int late_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void late_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_place_due.load() || !g_open.load() || cmn::closing() || !ui::active()) return;
    __try { late_post_body(); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("InputGuideList OnLateUpdate post (SEH)"); }
}

// The description area's text for row `row` (1-based; 0 = unknown: no description): a page of the row's lines (the
// pages in turn while the row keeps the cursor, from its first); the guide line under it only when the guide row could
// not take it (then after a blank line)
std::string guide_text(int row, bool hints_in_row, bool big, size_t* page_lines) {
    const std::string guide = model::labels::community_guide(g_pad_glyphs);
    // no row known (the game's own guide update while it makes the page again: SelectChildIndex -1): the row shown last
    // stays, its description is never blanked (27/10: the area went empty for a few frames at each move, and the game
    // dropped its guide row lower meanwhile)
    if (row < 1 && g_desc_row >= 1) row = g_desc_row;
    if (row != g_desc_row) { g_desc_row = row; g_desc_since = tick(); }
    *page_lines = 0;
    std::vector<std::string> pg;
    if (row >= 1 && row <= (int)g_extras.size()) {
        const int n = big ? (hints_in_row ? g_desc_lines.load() : std::max(1, g_desc_lines.load() - 2)) : lg::kComDescLinesSmall;
        pg = lg::com_desc_page(g_extras[(size_t)row - 1], n, (tick() - g_desc_since) / lg::kComDescPageFrames);
    }
    *page_lines = pg.size();
    if (!hints_in_row) return lg::com_guide_text(pg, guide);
    std::string s;
    for (const std::string& l : pg) s += (s.empty() ? "" : "\n") + l;
    return s;
}

void write_guide(MO* param, int row) {
    MO* gmt = gx::ref(param, "GuideMessageText");
    if (!gmt) return;
    // the row read once for this dialog keeps the hints even while the game makes its items again (they flickered under
    // the description at each move, 27/10); never read yet: 30 frames before the hints go under the description
    const bool in_row = row_hints(param) || (g_row.param == param && (g_row.done || tick() - g_row.since < 30));
    const bool big = desc_box(gmt);
    size_t shown = 0;
    const std::string t = guide_text(row, in_row, big, &shown);
    gx::set_text(gmt, t);                        // (the core compares before it writes, R7)
    const std::string lk = std::string(in_row ? "row" : "area") + (big ? "/big" : "/small");
    if (lk != g_layout_last) {
        g_layout_last = lk;
        cmn::journal("ui: Community hints %s", in_row ? "in the dialog's guide row" : "under the description (the guide row's item not found)");
    }
    const size_t all = row >= 1 && row <= (int)g_extras.size() ? g_extras[(size_t)row - 1].size() : 0;
    const std::string jk = std::to_string(row) + ":" + std::to_string(all);   // once per row shown, not per page
    if (jk != g_guide_last) {
        g_guide_last = jk;
        cmn::journal("ui: Community description area: row %d, %zu line(s), %zu a page", row, all, shown);
    }
}

void guide_post_body(MO* param) {
    std::lock_guard lk(ui::lock());
    try {
        if (!g_dlg.is_open() || !g_param || param != g_param) return;   // CRSM's own dialog only (other mods' dialogs untouched)
        int32_t sel = -1;
        if (gx::call_i32(param, "get_SelectChildIndex", &sel) && sel >= 0) g_dlg.guide_focus(sel + 1);
        write_guide(param, sel >= 0 ? sel + 1 : 0);
    } catch (...) { ui::fault("UpdateInputGuide post"); }
}
void guide_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    MO* p = s_guide.pop().param;
    if (!p || cmn::closing()) return;
    __try { guide_post_body(p); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("UpdateInputGuide post (SEH)"); }
}

// com_row_em.txt: what a row holds, in em (lg::text_em), when the estimate needs tuning in game; absent = kComRowEm.
// com_desc_lines.txt: the description area's lines a page; absent = kComDescLines.
void read_row_em() {
    double em = lg::kComRowEm;
    if (FILE* f = std::fopen(kRowEmFile, "rb")) {
        double v = 0;
        if (std::fscanf(f, "%lf", &v) == 1 && v >= 15.0 && v <= 80.0) em = v;
        std::fclose(f);
    }
    if (em != g_row_em.load()) cmn::journal("ui: Community rows hold %.1f em", em);
    g_row_em.store(em);
    int n = lg::kComDescLines;                   // com_desc_lines.txt: a page of the description area, in lines (1-8)
    if (FILE* f = std::fopen(kDescLinesFile, "rb")) {
        int v = 0;
        if (std::fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 8) n = v;
        std::fclose(f);
    }
    if (n != g_desc_lines.load()) cmn::journal("ui: Community description pages of %d line(s)", n);
    g_desc_lines.store(n);
}

// The probe dump (dump_on.txt, OFF by default, read-only): once per build of our page, every text of each row (its
// message, sizes, position: gx::describe_text) with the width the fit estimated, to tune kComRowEm against the game's.
void walk_texts(MO* e, int depth, int* budget, const std::string& head) {
    for (MO* c = e ? gx::first_child(e) : nullptr; c && *budget > 0; c = gx::next(c)) {
        if (gx::is_type(c, "via.gui.Text")) {
            --*budget;
            char b[64];
            std::snprintf(b, sizeof b, " est %.1f em: ", lg::text_em(gx::msg(c)));
            cmn::append_file(kDumpOut, head + b + gx::describe_text(c) + "\n");
        }
        if (depth < 5) walk_texts(c, depth + 1, budget, head);
    }
}

void probe_rows() {
    const SF6CoreAPI* a = api();
    if (!a || !g_param || !SF6CORE_HAS(a, opt_row_control) || g_probed == (int64_t)g_builds) return;
    g_probed = (int64_t)g_builds;
    int budget = 60;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        MO* ctrl = static_cast<MO*>(perf::counted(a)->opt_row_control(g_entries[i], g_param));
        if (!ctrl) continue;
        char h[64];
        std::snprintf(h, sizeof h, "[Community f%llu] row %zu", (unsigned long long)tick(), i + 1);
        walk_texts(ctrl, 0, &budget, h);
    }
    if (MO* gmt = gx::ref(g_param, "GuideMessageText"))     // the description area: where it is, how big, its alignment
        cmn::append_file(kDumpOut, "[Community] GuideMessageText: " + gx::describe_text(gmt) + "\n");
    MO* btn = guide_btn(g_param);                           // the guide row: its items, their texts and places
    cmn::append_file(kDumpOut, std::string("[Community] guide row c_guideBtn: ") + (btn ? "found" : "NOT found") + "\n");
    int left = 12;
    for (MO* c = btn ? gx::first_child(btn) : nullptr; c && left > 0; c = gx::next(c), --left) {
        float p[3] = {};
        gx::vec3_of(c, "get_Position", p);
        gx::TD* td = c->get_type_definition();
        char b[200];
        std::snprintf(b, sizeof b, "[Community] guide row child %s (%s) at %.0f, %.0f, visible %d: ", gx::name(c).c_str(),
                      td ? td->get_full_name().c_str() : "?", p[0], p[1], gx::visible(c) ? 1 : 0);
        MO* t = text_in(c, 0);
        cmn::append_file(kDumpOut, b + (t ? gx::describe_text(t) : std::string("(no text)")) + "\n");
    }
    cmn::journal("probe dump: Community rows (build %llu, %.1f em a row) in dump.txt", (unsigned long long)g_builds, g_row_em.load());
}

// the Manager's "Download Loadouts" (RMT:420-429)
void do_open() {
    if (g_dlg.is_open() || !community::on()) return;
    read_row_em();
    g_pad_glyphs = guide_pad().value_or(false);
    g_desc_row = 0;                              // a description starts on its first page
    g_layout_last.clear();                       // (the layout journaled once per opening)
    g_dlg.open(g_pad_glyphs);
    g_open.store(true);
    cmn::journal("ui: Community dialog opened");
}

// the Manager's "Share loadout" (RMT:397-419)
void do_share() {
    if (!community::on()) return;
    std::string ch, file, display;
    {
        std::lock_guard lk2(mech::lock());
        auto& e = mech::files::engine();
        const auto c = e.char_name();
        ch = c ? *c : std::string("Unknown");
        file = e.selected_file();
        display = e.selected_name();
    }
    page().share(ch, file, display);
}

}  // namespace

bool open() { return g_open.load(); }

void frame() {                                   // _com.tick (RMT:317-395), every frame
    if (g_share_request.exchange(false)) { try { do_share(); } catch (...) { ui::fault("Community share"); } }
    if (g_open_request.exchange(false)) { try { do_open(); } catch (...) { ui::fault("Community open"); } }
    if (!g_dlg.is_open()) return;
    const SF6CoreAPI* a = api();
    const bool window = a && perf::counted(a)->opt_window_open() != 0;
    if (window) g_window_seen = true;
    if (!window) {
        if (g_window_seen) closed_now("no window");
        else if (tick() > g_window_deadline) closed_now("the window never opened");
        return;                                  // (not open yet: the core opens it at its next frame)
    }
    if (g_param && !handle_alive(g_param)) {
        // a rebuild sent while the open (or an earlier rebuild) was still settling ends the dialog whose page we just
        // saw, and the new one brings its own param: not a close (2.2.2's first build lost the dialog there, stuck on
        // "Loading..."). A real close also lowers the window flag (the "no window" close above).
        if (!g_param_wait_until || tick() > g_param_wait_until) { closed_now("dialog ended"); return; }
        g_param = nullptr;
        cmn::journal("ui: Community dialog replaced by our rebuild, waiting for its page");
    }
    if (!g_param && tick() > g_param_wait_until && g_param_wait_until) g_param_wait_until = 0;   // (the window flag only from now)
    if (const int row = g_pressed_row.exchange(0)) g_dlg.button(row);   // download (confirm box, then the request)
    // the glyph set follows the game's guide (read every 5 frames: a pad / keyboard switch rewrites the title at once, the
    // guide line with the next refresh below; an unknown mode keeps the keyboard's)
    if (tick() % 5 == 0) g_pad_glyphs = guide_pad().value_or(false);
    const windows::PadHeld pad = windows::pad_held();   // a pad's L1 / R1 / Y / X do what A / E / T / R do
    g_dlg.tick(key_down(kKeyA) || pad.l1, key_down(kKeyE) || pad.r1, key_down(kKeyT) || pad.y, key_down(kKeyR) || pad.x, g_pad_glyphs);
    // the description area follows the cursor also when the game's UpdateInputGuide does not run (a rebuild in place, the
    // first frames of a page): every 5 frames, the core's compare-before-write leaves an unchanged text alone
    if (g_param && tick() % 5 == 2) {
        try { if (const int fr = g_port.focused_row()) write_guide(g_param, fr); } catch (...) { ui::fault("Community description area"); }
    }
    if (g_param && ui::dump::on()) { try { probe_rows(); } catch (...) { ui::fault("Community probe dump"); } }
}

void specs(std::vector<mech::hooks::Spec>& out) {
    ui::spec(out, "app.UIFlowOptionBGDialog.Param", "UpdateInputGuide", guide_pre, guide_post,
             "the Community dialog's \"Like / Sort\" line is the game's", &ok_guide);
    ui::spec(out, "app.UIFlowOptionBGDialog.Main", "OnUpdate", update_pre, update_post,
             "the Community dialog's \"Like / Sort\" go under its description (no entry in its guide row)", &ok_update);
    ui::spec(out, "app.UIPartsInputGuideList", "OnLateUpdate", late_pre, late_post,
             "the Community dialog's \"Like / Sort\" entry is placed at the next guide update, not right after the row's layout", &ok_late);
}

void hand_back() {
    if (g_dlg.is_open()) { closed_now("ui stopped"); if (const SF6CoreAPI* a = api()) perf::counted(a)->opt_close(gx::cl()); }
}

std::string status() {
    char b[200];
    std::snprintf(b, sizeof(b), "%s, builds %llu, rebuilds %llu, closes %llu, guide hook %d", g_dlg.is_open() ? "open" : "closed",
                  (unsigned long long)g_builds, (unsigned long long)g_navs, (unsigned long long)g_closes, ok_guide);
    return b;
}

}  // namespace ui::com

namespace ui::com_api {

// the Manager's buttons: their work (file reads, the option window) runs on the frame callback (game thread)
void open() { if (ui::active()) ui::com::g_open_request.store(true); }
void share() { if (ui::active()) ui::com::g_share_request.store(true); }

}  // namespace ui::com_api
