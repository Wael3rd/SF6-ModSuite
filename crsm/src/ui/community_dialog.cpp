// ui/community_dialog — the Community dialog ("Download Loadouts") on the core's option service (phase P4, owner "ui"),
// RecordingManagerTab.lua (RMT) 149-431 and 1564-1586 in C++. The page's state and actions are community::Page (P2: list,
// sort, pages, votes, downloads, com_state.json, tickers); the dialog's rules are ui::logic::ComDialog (pure); here: the
// option window (opt_group / opt_entry / opt_navigate / opt_open_menu, the Color Mod's pattern for per-page texts: the
// group removed and made again with the same key, then a rebuild in place), the raw keys A / E / T / R, the guide line
// ("Like / Sort") on our dialog only, the prompt close detection, the Manager's "Share loadout".
//
// The one intended difference from the Lua (Wael's decision): after the list is refreshed (an answer arrived: open, sort,
// after a download) the cursor stays on its row; the Lua sent it back to row 1 (com_focus_idx written as a global).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <memory>

#include "common.hpp"
#include "community/community.hpp"
#include "community/page.hpp"
#include "dialogs.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "hookstack.hpp"
#include "internal.hpp"
#include "lang.hpp"
#include "logic.hpp"
#include "manager_page.hpp"
#include "mech/mech.hpp"
#include "model/files.hpp"
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
constexpr const char* kGuide = "<ICON KeyT> Like   <ICON KeyR> Sort";   // RMT:1579
constexpr int kKeyA = 0x41, kKeyE = 0x45, kKeyT = 0x54, kKeyR = 0x52;   // RMT:329-332
constexpr uint64_t kSetupWait = 60;              // frames a rebuilt dialog may take to show its page

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
    std::string title() override { return page().title(); }
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

// -- UIFlowOptionBGDialog.Param.UpdateInputGuide post: the controls in the description area, our dialog only (RMT:1564-1586)
struct GuideCall { MO* param = nullptr; };
thread_local HookStack<GuideCall> s_guide;
int guide_pre(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    GuideCall* s = s_guide.push();
    if (s && ui::active() && g_open.load()) s->param = static_cast<MO*>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void guide_post_body(MO* param) {
    std::lock_guard lk(ui::lock());
    try {
        if (!g_dlg.is_open() || !g_param || param != g_param) return;   // CRSM's own dialog only (other mods' dialogs untouched)
        int32_t sel = -1;
        if (gx::call_i32(param, "get_SelectChildIndex", &sel) && sel >= 0) g_dlg.guide_focus(sel + 1);
        if (MO* gmt = gx::ref(param, "GuideMessageText")) gx::set_text(gmt, kGuide);
    } catch (...) { ui::fault("UpdateInputGuide post"); }
}
void guide_post(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    MO* p = s_guide.pop().param;
    if (!p || cmn::closing()) return;
    __try { guide_post_body(p); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("UpdateInputGuide post (SEH)"); }
}

// the Manager's "Download Loadouts" (RMT:420-429)
void do_open() {
    if (g_dlg.is_open() || !community::on()) return;
    g_dlg.open();
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
    if (g_param && !handle_alive(g_param)) { closed_now("dialog ended"); return; }
    if (!g_param && tick() > g_param_wait_until && g_param_wait_until) g_param_wait_until = 0;   // (the window flag only from now)
    if (const int row = g_pressed_row.exchange(0)) g_dlg.button(row);   // download (confirm box, then the request)
    g_dlg.tick(key_down(kKeyA), key_down(kKeyE), key_down(kKeyT), key_down(kKeyR));
}

void specs(std::vector<mech::hooks::Spec>& out) {
    ui::spec(out, "app.UIFlowOptionBGDialog.Param", "UpdateInputGuide", guide_pre, guide_post,
             "the Community dialog's \"Like / Sort\" line is the game's", &ok_guide);
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
