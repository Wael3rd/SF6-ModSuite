#include "ui/manager_page.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <reframework/API.hpp>

#include "common.hpp"
#include "game/gm.hpp"
#include "mech/mech.hpp"
#include "model/files.hpp"
#include "model/luajson.hpp"
#include "model/pack.hpp"
#include "model/store.hpp"
#include "ui/details_logic.hpp"
#include "ui/dialogs.hpp"
#include "ui/drill_details.hpp"
#include "ui/dump.hpp"
#include "ui/gx.hpp"
#include "ui/internal.hpp"
#include "ui/lang.hpp"
#include "ui/logic.hpp"
#include "ui/manager_logic.hpp"
#include "ui/ui.hpp"
#include "perf.hpp"

namespace ui::manager {
namespace {

namespace gm = game::gm;
using F = cmn::Feature;
using mgr::Act;
using mgr::Action;
using Lock = std::lock_guard<std::recursive_mutex>;

// ---- state -------------------------------------------------------------------------------------------------------------------
// g_mx is a leaf lock (never held across a call out of this file): the labels given to the core, the spin's value and its
// sequence, the sub-page memory, the hint texts. The ids are written once by claim() before any callback can run.
std::mutex g_mx;
const SF6CoreAPI* g_api = nullptr;
SF6CoreClient* g_client = nullptr;
std::atomic<int64_t> g_page{-1};
std::array<std::atomic<int64_t>, mgr::kRowCount> g_rows{};
std::vector<std::string> g_labels;            // under g_mx
int32_t g_sel = 0;                            // under g_mx: the spin's value (0-based), what spin_value answers
uint64_t g_sel_seq = 0;                       // under g_mx: +1 at each on_spin (a sync never overwrites a newer move)
mgr::PageMemory g_mem;                        // under g_mx
std::array<std::string, 2> g_hint_text;       // under g_mx: edit_name, delete_name in the game's language
mgr::Queue g_queue;
std::atomic<uint64_t> g_rows_epoch{0};
std::atomic<bool> g_rt_high{false};           // RecordType written above 1: the closed-menu safety watches it
std::atomic<uint64_t> g_decides{0}, g_spins{0}, g_hints{0}, g_rt_writes{0}, g_actions{0}, g_confirms{0};
std::atomic<bool> g_details_due{false};       // Confirm on the Drill row (the core's on_confirm): the details on the frame
// The Confirm is taken only once the decide input (F, pad A / Cross) has been seen up on a frame with no dialog of CRSM up
// (a text dialog, a box, the Community dialog, the drill details) or asked: the F / A that closes one (the drill's new
// name confirmed in the text dialog: the Drill row has the focus behind it) never opens the details (Wael 04/11).
// Written by this page's frame callback, read by cb_confirm (the core's frame, before it): the game thread both.
std::atomic<bool> g_confirm_armed{true};
std::atomic<uint64_t> g_confirm_unarmed{0};
std::atomic<bool> g_edit_due{false};          // T / pad Y in the details: the description edited on the frame
std::mutex g_claim_mx;                        // claim(): the Lua loader's thread
std::atomic<bool> g_ready{false};             // start() found the core's pm service and files native: the page can be asked
std::atomic<bool> g_claimed{false};           // claim() ran (the page asked, or refused once)

// frame thread only
bool g_menu_up = false;
bool g_training = false;
bool g_list_dirty = true;
uint64_t g_native_action_until = 0;
uint64_t g_lang_seen = 0;
uint64_t g_list_seen = 0;                     // the files layer's list_version the spin last followed
// the Loadout row's guide: the selected loadout's info.description (docs/PACK_FORMAT.md) in place of the row's own help
std::string g_guide_key = "\x01";            // char/file#list_version of the last look ("\x01" = look again)
struct PackDesc { std::filesystem::file_time_type t{}; uintmax_t size = 0; std::string desc; };
std::map<std::string, PackDesc> g_pack_desc;  // by pack path: a pack is read again only when it changed
// The game reads the focused row's guide into its help line at a focus and when the spin moves, in its own handling of the
// press, and not again while the row keeps the focus: a guide given from the frame callback came one move late (27/10:
// the help line always showed the previous loadout's). So the guides of the spin's neighbours are made ahead on the frame
// (g_spin_guide) and cb_on_spin (inside the game's Function for the press) gives the new one to the core at the move.
std::mutex g_guide_mx;                        // leaf lock, held across pm_set_texts only (the core calls no callback under it)
std::string g_guide_given;                    // under g_guide_mx: the Loadout row's guide as the core has it now
std::map<int32_t, std::string> g_spin_guide;  // under g_mx: spin option -> its guide, the options near the selected one
constexpr int32_t kGuideNear = 2;             // options made ahead on each side (a held key moves one option a press)
std::atomic<uint64_t> g_guide_at_spin{0};
// The Drill row's help line: the description whole (no "…" cut), on one line; the game's own ScrollText part scrolls a
// help line longer than the bar by itself (in game 04/11: "it works"), so nothing more is written. help_pages_on.txt (the
// data folder) gives the fallback instead (logic::help_pages): the guide given to the core is page 1 (the game writes it
// at a focus or a spin move), the next pages are written every kHelpPageFrames into the help line shown now (the live
// InputGuide's p_ScrollText_0 / e_text, found again at each write), only while the Drill row has the focus (cb_hints says
// so at each focus), no dialog is up and the menu is up, and only over our own page or the guide the game wrote (an
// empty read: a text set by its message id). Back to page 1 at each focus or spin move; a write at a page change only.
std::vector<std::string> g_help_pages;        // frame thread: the selected drill's line or pages (one or none: no cycling)
size_t g_help_page = 0;
uint64_t g_help_next = 0;
uint64_t g_help_focus_seen = 0;
std::atomic<uint64_t> g_help_focus{0};        // +1 at each focus / spin move on the Drill row (cb_hints, the menu's threads)
std::atomic<bool> g_drill_focused{false};
bool g_help_paged = false;                    // help_pages_on.txt, read when the pages are made
uint64_t g_help_writes = 0;
constexpr const char* kHelpPagesFile = "reframework/data/SF6_CRSMNative_data/help_pages_on.txt";

bool live() { return cmn::owns(F::Ui) && !cmn::closing() && g_page.load() >= 0; }
model::files::Engine& eng() { return mech::files::engine(); }

std::string tr(const std::string& key, const std::vector<std::string>& args) { return ui::lang::T(key, args); }

// ---- the ports of the actions ------------------------------------------------------------------------------------------------
// Each call into the files layer under mech::lock() and nothing else: P4's dialogs and the tickers are called with no lock
// of ours held (lock order: ui lock, then model lock).
class FilesPort final : public mgr::Loadouts {
public:
    std::string selected_name() override { Lock lk(mech::lock()); return eng().selected_name(); }
    std::string char_name() override {
        Lock lk(mech::lock());
        const auto c = eng().char_name();
        return c ? *c : std::string();
    }
    std::string loaded_name() override { Lock lk(mech::lock()); return eng().loaded_name(); }
    std::vector<std::string> dropdown() override { Lock lk(mech::lock()); return eng().dropdown(); }
    int dropdown_index() override { Lock lk(mech::lock()); return eng().dropdown_index(); }
    void set_dropdown_index(int idx1) override { Lock lk(mech::lock()); eng().set_dropdown_index(idx1); }
    void set_native_action(bool on) override { Lock lk(mech::lock()); eng().set_native_action(on); }
    std::string import_selected() override {
        Lock lk(mech::lock());
        const std::string r = eng().import_selected();
        mech::files::after_call();
        cmn::journal("ui: manager import_selected -> %s", r.c_str());
        return r;
    }
    std::string reopen_loaded() override {
        Lock lk(mech::lock());
        const std::string r = eng().reopen_loaded();
        mech::files::after_call();
        cmn::journal("ui: manager reopen_loaded -> %s", r.c_str());
        return r;
    }
    std::string export_selected(std::string* tk) override {
        Lock lk(mech::lock());
        const std::string r = eng().export_selected(tk);
        cmn::journal("ui: manager export_selected -> %s", r.c_str());
        return r;
    }
    std::string export_as(const std::string& name, std::string* tk) override {
        Lock lk(mech::lock());
        const std::string r = eng().export_as(model::files::json(name), tk);
        cmn::journal("ui: manager export_as -> %s", r.c_str());
        return r;
    }
    std::string erase_selected() override {
        Lock lk(mech::lock());
        const std::string r = eng().erase_selected();
        cmn::journal("ui: manager erase_selected -> %s", r.c_str());
        return r;
    }
    std::string rename_selected(const std::string& name, std::string* tk) override {   // the file renamed, its content kept
        Lock lk(mech::lock());
        if (tk) tk->clear();
        const std::string r = eng().rename_file(model::files::json(name));
        cmn::journal("ui: manager rename (Edit Drill Name) -> %s", r.c_str());
        return r;
    }
    void invalidate() override { Lock lk(mech::lock()); eng().invalidate(); }
    bool ensure() override { Lock lk(mech::lock()); return eng().ensure(mech::files::select_menu_id()); }
};

class GameScreen final : public mgr::Screen {
public:
    void ticker(const std::string& text) override { ui::dialogs::ticker(text); }
    void confirm(const std::string& text, const std::string& title, std::function<void(bool yes)> done) override {
        ui::dialogs::confirm(text, title, std::move(done));    // the titled box of v1.6 ("Save loadout"...: RMT:116-141)
    }
    void text_input(const std::string& title, const std::string& initial, size_t max_bytes,
                    std::function<void(bool ok, const std::string& text)> done) override {
        ui::dialogs::text_input(title, initial, max_bytes, std::move(done));
    }
    void community_open() override { ui::dialogs::community_open(); }
    void community_share() override { ui::dialogs::community_share(); }
    void check_slots() override {
        if (gm::MO* tm = gm::singleton("app.training.TrainingManager")) gm::call(tm, "CheckMenuSlotData", {});
    }
    void rows_dirty() override {               // RMT rows_dirty: P4's Record / Replay rows redrawn at their list's next pass
        g_rows_epoch.fetch_add(1);
        ui::rows_changed();
    }
    void list_changed() override { g_list_dirty = true; }
};

FilesPort g_files;
GameScreen g_screen;
mgr::Runner g_runner(g_files, g_screen, tr);

// ---- game reads ---------------------------------------------------------------------------------------------------------------
// The menu is up as the core sees it (pausemenu.cpp frame_inner): PauseManager._CurrentPauseTypeBit not 64 / 2112, or an
// option window.
bool menu_up_live() {
    bool up = false;
    if (gm::MO* pm = gm::singleton("app.PauseManager")) {
        int64_t bit = 64;
        if (gm::get_int(pm, "_CurrentPauseTypeBit", &bit)) up = bit != 64 && bit != 2112;
    }
    if (!up && SF6CORE_HAS(g_api, opt_window_open) && perf::counted(g_api)->opt_window_open()) up = true;
    return up;
}

// TrainingManager.get_RecordFunc()._tData.RecordSetting (RMT:2027-2030)
gm::MO* record_setting(gm::MO* tm) {
    gm::MO* rf = tm ? gm::call_obj(tm, "get_RecordFunc") : nullptr;
    return gm::ref(gm::ref(rf, "_tData"), "RecordSetting");
}

// The primary tab whose _FuncType is RECORD (RMT:2031-2034)
int64_t recording_tab_index(gm::MO* tm) {
    gm::MO* arr = gm::call_obj(gm::ref(tm, "_UIData"), "get_MenuData");
    const int32_t n = gm::array_len(arr);
    for (int32_t i = 0; i < n; ++i) {
        int64_t ft = -1;
        if (gm::MO* t = gm::array_get(arr, i); t && gm::get_int(t, "_FuncType", &ft) && ft == mgr::kRecordFunc) return i;
    }
    return -1;
}

// The pages of the Recording tab's strip (_ChildData[0]._ChildData): 3 with ours installed, 2 = the game's own strip
int32_t strip_pages(gm::MO* tm, int64_t tab) {
    gm::MO* arr = gm::call_obj(gm::ref(tm, "_UIData"), "get_MenuData");
    gm::MO* strip = gm::array_get(gm::ref(gm::array_get(arr, (int32_t)tab), "_ChildData"), 0);
    return gm::array_len(gm::ref(strip, "_ChildData"));
}

bool read_record_type(int64_t* rt) {
    gm::MO* rs = record_setting(gm::singleton("app.training.TrainingManager"));
    return rs && gm::get_int(rs, "RecordType", rt);
}

// ---- texts ----------------------------------------------------------------------------------------------------------------------
void retext() {
    g_lang_seen = ui::lang::version();
    const int64_t page = g_page.load();
    if (page < 0) return;
    perf::counted(g_api)->pm_set_texts(g_client, page, mgr::page_title(tr).c_str(), mgr::page_guide(tr).c_str());
    for (int i = 0; i < mgr::kRowCount; ++i) {
        const mgr::RowText t = mgr::row_text((mgr::Row)i, tr);
        if (i != mgr::kLoadout) { perf::counted(g_api)->pm_set_texts(g_client, g_rows[i].load(), t.title.c_str(), t.guide.c_str()); continue; }
        std::lock_guard gl(g_guide_mx);
        g_guide_given = t.guide;
        perf::counted(g_api)->pm_set_texts(g_client, g_rows[i].load(), t.title.c_str(), t.guide.c_str());
    }
    std::lock_guard lk(g_mx);
    g_hint_text = { tr("edit_name", {}), tr("delete_name", {}) };
    g_guide_key = "\x01";                     // the Loadout row has its own guide again: the description is put back
    g_spin_guide.clear();
    g_list_dirty = true;                      // the spin's first entry (no file) in the new language
}

// info.description of the pack at `path` ("" when none), read once per change of the file
std::string pack_desc(const std::filesystem::path& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return "";
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) return "";
    const std::string key = path.string();
    if (auto it = g_pack_desc.find(key); it != g_pack_desc.end() && it->second.t == t && it->second.size == size) return it->second.desc;
    if (g_pack_desc.size() >= 64) g_pack_desc.clear();
    PackDesc d{ t, size, model::pack::description(model::luajson::load_file(path)) };
    g_pack_desc[key] = d;
    return d.desc;
}

// The pause menu's help line text while it shows `shown` (the text the game took from the focused row's guide): found
// under the menu's agent (ui11200) by its message, read-only walk, at most kHelpWalk elements. nullptr = not shown now.
constexpr int kHelpWalk = 4000;
gm::MO* find_help_text(gm::MO* e, const std::string& shown, int depth, int* left) {
    for (gm::MO* c = e ? ui::gx::first_child(e) : nullptr; c && *left > 0; c = ui::gx::next(c)) {
        --*left;
        if (ui::gx::is_type(c, "via.gui.Text") && ui::gx::msg(c) == shown) return c;
        if (depth < 16) if (gm::MO* t = find_help_text(c, shown, depth + 1, left)) return t;
    }
    return nullptr;
}

// The help line shown now, from `before` to `after`, for a guide changed without a spin move (a save, a rename, a
// download while the Loadout row keeps the focus): written into the shown text (through the core's ui_set_text: compared
// first, R7) when a text under the menu's agent shows exactly our previous guide; another row's help is never touched.
// (27/10: never found there; the help line at the bottom of the screen may belong to another agent. The guide is right
// from the row's next focus or spin move either way.)
void help_line_now(const std::string& before, const std::string& after) {
    if (before == after || !perf::counted(g_api)->pm_page_shown(g_page.load())) return;
    gm::MO* agent = ui::gx::pause_agent();
    gm::MO* root = agent ? ui::gx::call_obj(agent, "get_ControlMain") : nullptr;
    int left = kHelpWalk;
    gm::MO* t = root ? find_help_text(root, before, 0, &left) : nullptr;
    if (t) ui::gx::set_text(t, after);
    if (t && ui::dump::on())                                     // the help line's place and size (dump_on.txt)
        cmn::append_file("reframework/data/SF6_CRSMNative_data/dump.txt", "[Manager] help line: " + ui::gx::describe_text(t) + "\n");
    cmn::journal("ui: manager help line %s", t ? "rewritten in place" : "not found under the menu's agent: right at the row's next focus or move");
}

// The help line of the loadout at `path` ("" = no file): its description without its Markdown markers, its paragraphs on
// one line (" | "), whole (the game scrolls it); help_pages_on.txt: in pages of kHelpLineEm
std::vector<std::string> help_pages_for(const std::filesystem::path& path) {
    const std::string desc = path.empty() ? std::string() : pack_desc(path);
    if (desc.empty()) return {};
    return ui::logic::help_pages(ui::details::strip_markdown(desc), g_help_paged ? ui::logic::kHelpLineEm : 1e9);
}

// The guide of the loadout at `path`: its help line (its first page), else the row's own help `base`
std::string guide_for(const std::filesystem::path& path, const std::string& base) {
    const std::vector<std::string> pages = help_pages_for(path);
    return pages.empty() ? base : pages[0];
}

// The Loadout row's guide to the core (pm_set_texts: the row's guide in place, as the language change does) when it is not
// the one the core has. Any thread: the frame callback and cb_on_spin (the menu's job thread; pm_* allowed there).
bool give_guide(const std::string& guide, std::string* before) {
    std::lock_guard lk(g_guide_mx);
    if (guide == g_guide_given) return false;
    if (before) *before = g_guide_given;
    g_guide_given = guide;
    perf::counted(g_api)->pm_set_texts(g_client, g_rows[mgr::kLoadout].load(), nullptr, guide.c_str());
    return true;
}

// The Loadout row's guide (its bottom help line): the selected loadout's description when it has one, else the row's own
// help (guide_for). Made when the selection or the list changed, with the guides of the kGuideNear options on each side
// of the selected one for cb_on_spin (the spin wraps around: so do they). Given to the core when it changes: normally
// already done by cb_on_spin at the move; otherwise here, and written into the help line shown now (help_line_now).
void loadout_guide(uint64_t list_version) {
    if (g_queue.selects() != 0) return;       // a move not applied yet (its guide given at the move): looked at next frame
    std::string ch, file;
    std::filesystem::path path;
    std::vector<std::pair<int32_t, std::filesystem::path>> near;
    {
        Lock lk(mech::lock());
        const auto c = eng().char_name();
        ch = c ? *c : std::string();
        file = eng().selected_file();
        if (!ch.empty() && !file.empty()) path = eng().abs(model::store::pack_path(ch, file));
        const std::vector<std::string>& files = eng().files();
        const int32_t n = (int32_t)std::clamp<size_t>(eng().dropdown().size(), 1, mgr::kSpinCapacity);   // the spin's options
        const int32_t cur = mgr::spin_value(eng().dropdown_index(), (size_t)n);
        for (int32_t d = -kGuideNear; d <= kGuideNear; ++d) {
            const int32_t j = ((cur + d) % n + n) % n;       // option j = dropdown entry j + 1 = files()[j - 1] (0: no file)
            if (d == 0 || j == cur) continue;
            std::filesystem::path p;
            if (!ch.empty() && j >= 1 && (size_t)j <= files.size()) p = eng().abs(model::store::pack_path(ch, files[(size_t)j - 1]));
            near.emplace_back(j, std::move(p));
        }
    }
    const std::string key = ch + "/" + file + "#" + std::to_string(list_version);   // (a move, a save, a download: a new version)
    if (key == g_guide_key) return;
    g_guide_key = key;
    std::error_code ec;
    g_help_paged = std::filesystem::exists(kHelpPagesFile, ec);
    g_help_pages = help_pages_for(path);           // the pages in turn from page 1 (help_tick)
    g_help_page = 0;
    g_help_next = cmn::frame() + ui::logic::kHelpPageFrames;
    const std::string base = mgr::row_text(mgr::kLoadout, tr).guide;
    const std::string after = guide_for(path, base);
    std::map<int32_t, std::string> ahead;
    for (const auto& [j, p] : near) ahead[j] = guide_for(p, base);
    { std::lock_guard lk(g_mx); g_spin_guide = std::move(ahead); }
    std::string before;
    if (!give_guide(after, &before)) return;
    cmn::journal("ui: manager Loadout guide (frame): %s", after == base ? "its own help" : "the loadout's description");
    try { help_line_now(before, after); } catch (...) { cmn::fault(F::Ui, "help line"); }
}

// The spin's labels and value from the files layer: labels given to the core only when they changed (the core refreshes a
// shown spin at once and rebuilds the page 2 frames later, R4); the value unless the player moved the spin meanwhile.
void sync() {
    g_list_dirty = false;
    uint64_t seq;
    { std::lock_guard lk(g_mx); seq = g_sel_seq; }
    std::vector<std::string> dd;
    int idx = 1;
    {
        Lock lk(mech::lock());
        dd = eng().dropdown();
        idx = eng().dropdown_index();
    }
    const std::vector<std::string> labels = mgr::spin_labels(dd, tr("select_file", {}));
    bool changed = false;
    size_t before = 0;
    int32_t sel = 0;
    {
        std::lock_guard lk(g_mx);
        before = g_labels.size();
        if (labels != g_labels) { g_labels = labels; changed = true; }
        if (g_sel_seq == seq && g_queue.selects() == 0) g_sel = mgr::spin_value(idx, labels.size());
        sel = g_sel;
    }
    if (!changed) return;
    if (ui::dump::on() && perf::counted(g_api)->pm_page_shown(g_page.load())) ui::dump::spin_labels(g_rows[mgr::kLoadout].load(), before, labels.size());
    std::vector<const char*> p;
    p.reserve(labels.size());
    for (const std::string& s : labels) p.push_back(s.c_str());
    const SF6CoreStatus st = perf::counted(g_api)->pm_set_options(g_client, g_rows[mgr::kLoadout].load(), p.data(), (uint32_t)p.size());
    // (a few per session: the list changes at an open, a Save As, a rename, an Erase, a download; the core logs the spin
    // part's range on the shown page)
    cmn::journal("ui: manager spin labels %zu -> %zu, option %d, page %s -> %d", before, labels.size(), (int)sel,
                 perf::counted(g_api)->pm_page_shown(g_page.load()) ? "shown" : "not shown", (int)st);
}

// ---- the core's callbacks (the game's UI job threads, synchronous; R4: no menu update method called back) ------------------------
void SF6CORE_CALL cb_decide(void*, int64_t row) {
    try {
        if (!live()) return;
        for (int i = 0; i < mgr::kRowCount; ++i) {
            if (g_rows[i].load() != row) continue;
            if (const auto a = mgr::act_of_row(i)) {
                g_queue.push(Action{ *a, 0 });
                g_decides.fetch_add(1);
            }
            return;
        }
    } catch (...) { cmn::fault(F::Ui, "on_decide"); }
}

int32_t SF6CORE_CALL cb_spin_value(void*, int64_t) {
    try {
        std::lock_guard lk(g_mx);
        return g_sel;
    } catch (...) { return 0; }
}

void SF6CORE_CALL cb_on_spin(void*, int64_t row, int32_t index) {
    try {
        if (!live() || row != g_rows[mgr::kLoadout].load()) return;
        std::string guide;
        bool ahead = false;
        {
            std::lock_guard lk(g_mx);
            const int idx1 = mgr::dropdown_of_spin(index, g_labels.size());
            g_sel = idx1 - 1;
            ++g_sel_seq;
            g_queue.push(Action{ Act::Select, idx1 });              // set_dropdown_index(i + 1) on the frame callback
            g_spins.fetch_add(1);
            if (const auto it = g_spin_guide.find(index); it != g_spin_guide.end()) { guide = it->second; ahead = true; }
        }
        // the new option's guide to the core now, before the game reads it into the help line (g_guide_mx: no lock of ours
        // held but it; the core holds none while a callback runs)
        if (ahead && give_guide(guide, nullptr) && g_guide_at_spin.fetch_add(1) < 3)
            cmn::journal("ui: manager Loadout guide given at the spin move (option %d)", (int)index);   // (the first 3 only)
    } catch (...) { cmn::fault(F::Ui, "on_spin"); }
}

// Confirm (F, pad A / Cross) on the Drill row: the core reads it raw (core 0.9.5; the game gives a spin row no decide) and
// calls this on the game thread, from its frame. The dialog is asked from this page's frame callback (details_open).
void SF6CORE_CALL cb_confirm(void*, int64_t row) {
    try {
        if (!live() || row != g_rows[mgr::kLoadout].load()) return;
        if (!g_confirm_armed.load() || ui::dialogs::busy()) {   // a press begun under a dialog, or closing one: not a Confirm
            g_confirm_armed.store(false);
            if (g_confirm_unarmed.fetch_add(1) < 3) cmn::journal("ui: manager Confirm on the Drill row ignored (a dialog was up)");
            return;
        }
        g_confirms.fetch_add(1);
        g_details_due.store(true);
    } catch (...) { cmn::fault(F::Ui, "on_confirm"); }
}

void SF6CORE_CALL cb_page_shown(void*, int64_t page, int32_t shown) {
    try {
        if (page != g_page.load()) return;
        const bool up = shown ? true : menu_up_live();
        {
            std::lock_guard lk(g_mx);
            g_mem.page_shown(shown != 0, up);
        }
        if (shown && live()) g_queue.push(Action{ Act::Ensure, 0 });   // ensure_files when the page is entered (RMT:2009)
    } catch (...) { cmn::fault(F::Ui, "on_page_shown"); }
}

constexpr int32_t kButtonDecide = 90;            // app.InputAssign.Digital.ConfigId UI_DECIDE (the game draws the device's glyph)

uint32_t SF6CORE_CALL cb_hints(void*, void* focused, SF6CorePmHint* out, uint32_t max) {
    try {
        if (!live() || !focused || !out || max == 0) return 0;
        const int64_t row = SF6CORE_HAS(g_api, pm_row_of) ? perf::counted(g_api)->pm_row_of(g_client, focused) : -1;
        const bool drill = row >= 0 && row == g_rows[mgr::kLoadout].load();   // (the game builds the guide at each focus /
        g_drill_focused.store(drill);                                          //  spin move: the help line pages restart)
        if (drill) g_help_focus.fetch_add(1);
        if (row >= 0 && row == g_rows[mgr::kLoadout].load()) {   // "See Details" (Confirm), "Edit Drill Name" (T / pad Y):
            thread_local std::array<std::string, 2> texts;        //  a drill selected only (read by the core before this returns)
            {
                std::lock_guard lk(g_mx);
                if (g_sel <= 0) return 0;                         // option 0: "-- Select File --"
            }
            texts = { tr("see_details", {}), tr("edit_drill_name", {}) };
            const int32_t buttons[2] = { kButtonDecide, mgr::kButtonY };
            uint32_t n = 0;
            for (; n < 2 && n < max; ++n) { out[n].button = buttons[n]; out[n].text = texts[n].c_str(); }
            g_hints.fetch_add(n);
            return n;
        }
        const bool own = row >= 0;
        int64_t ft = -1;
        const bool known = !own && gm::get_int(static_cast<gm::MO*>(focused), "_FuncType", &ft);
        const std::vector<mgr::HintSpec> hs = mgr::hints_for(own, known, ft);
        if (hs.empty()) return 0;
        thread_local std::array<std::string, 2> texts;              // read by the core before this returns
        uint32_t n = 0;
        std::lock_guard lk(g_mx);
        for (const mgr::HintSpec& h : hs) {
            if (n >= max || n >= texts.size()) break;
            texts[n] = std::string(h.key) == "edit_name" ? g_hint_text[0] : g_hint_text[1];
            out[n].button = h.button;
            out[n].text = texts[n].c_str();
            ++n;
        }
        g_hints.fetch_add(n);
        return n;
    } catch (...) {
        cmn::fault(F::Ui, "hints");
        return 0;
    }
}

// ---- UIFlowTrainingMenu.Param.CreateViewData pre: the sub-page to reopen on (RMT:2022-2065) ---------------------------------------
// Run by P4's CreateViewData pre-hook (ui/windows.cpp), last, as the Lua's one hook did: the record finalize, the 30-entry
// arrays and the slot windows first, then RecordType (RMT:2038-2065).
void cvd_body() {
    gm::MO* tm = gm::singleton("app.training.TrainingManager");
    if (!tm) return;
    const int64_t tab = recording_tab_index(tm);
    int64_t sel = -1;
    if (tab < 0 || !gm::get_int(gm::ref(tm, "_tData"), "SelectIndex", &sel) || sel != tab) return;   // Recording tab only
    gm::MO* rs = record_setting(tm);
    int64_t rt = 0;
    if (!rs || !gm::get_int(rs, "RecordType", &rt)) return;
    std::optional<int64_t> want;
    {
        std::lock_guard lk(g_mx);
        want = g_mem.record_type(rt);
    }
    if (!want || *want == rt) return;
    if (!mgr::record_type_fits(*want, strip_pages(tm, tab))) return;   // ours not in the strip (yet / any more): the game's pick
    if (gm::set_int(rs, "RecordType", *want)) {
        g_rt_writes.fetch_add(1);
        if (*want > 1) g_rt_high = true;
        cmn::journal("ui: RecordType %lld -> %lld (reopen on %s)", (long long)rt, (long long)*want, mgr::page_name((mgr::PageMemory::Page)*want));
    }
}

// TrainingMenuFunc.Function(107 / 108) pre (P4's hook): the game built its Record / Replay page (RMT:2012-2014).
void game_page_body(bool replay) {
    using P = mgr::PageMemory::Page;
    P before, after;
    {
        std::lock_guard lk(g_mx);
        before = g_mem.page();
        g_mem.game_page(replay ? P::Replay : P::Record);
        after = g_mem.page();
    }
    if (after != before) cmn::journal("ui: sub-page memory: %s -> %s", mgr::page_name(before), mgr::page_name(after));
}

// ---- the drill details (Confirm on the Drill row) --------------------------------------------------------------------------------
// The selected drill's details in the game's image dialog (ui/drill_details); "Open drill" there = this page's Open.
// No drill selected: what Open does then ("No drill selected"), and no dialog. Ignored while a dialog of CRSM is up (a
// text dialog's typed F, a box's decide) or asked.
void details_open() {
    if (ui::dialogs::busy() || g_runner.dialog_open()) { cmn::journal("ui: manager Confirm on the Drill row ignored (a dialog is up)"); return; }
    std::string ch, name, title;
    std::filesystem::path path;
    {
        Lock lk(mech::lock());
        const auto c = eng().char_name();
        ch = c ? *c : std::string();
        name = eng().selected_name();
        const std::string file = eng().selected_file();
        if (!ch.empty() && !file.empty()) path = eng().abs(model::store::pack_path(ch, file));
        const std::vector<std::string> labels = mgr::spin_labels(eng().dropdown(), tr("select_file", {}));
        const int32_t i = mgr::spin_value(eng().dropdown_index(), labels.size());
        title = (size_t)i < labels.size() ? labels[(size_t)i] : name;
    }
    if (name.empty() || path.empty()) { g_screen.ticker(tr("none_selected", {})); return; }
    const nlohmann::json data = model::luajson::load_file(path);
    ui::details::Drill d;
    d.title = title;
    d.character = ui::lang::fighter(ch);                      // shown in the details frame: the player's language
    d.source = model::pack::source(data);
    d.author = model::pack::author(data);
    d.description = model::pack::description(data);
    // the player's own drill without an author: the top-left box names the player as the Community names their shares
    if (d.author.empty() && ui::details::origin(d.source) == ui::details::Origin::Local) {
        try { d.own_name = ui::com::share_name(); } catch (...) { cmn::fault(F::Ui, "drill details: the share name"); }
    }
    d.slots = model::pack::slot_summaries(data);
    const size_t n = d.slots.size();
    const char* who = !d.author.empty() ? "info.author" : !d.own_name.empty() ? "the share name" : "none";
    const bool asked = ui::details::request(std::move(d), [] {     // (the dialog's end, the UI frame callback: run here next)
        if (live()) g_queue.push(Action{ Act::Open, 0 });
    }, [] { g_edit_due.store(true); });
    cmn::journal("ui: manager drill details %s (%zu slot entries, author: %s)", asked ? "asked" : "refused (one is up, or the UI is off)", n, who);
}

// T / pad Y in the drill details: the selected drill's description edited in the game's text dialog (the one of the slot
// names and Save As), on one line with each line break shown as "/n", at most details::kDescMaxUnits (the Community
// Worker's limit), over the details dialog (in game 04/11: it draws over it, Esc closes only it), which shows the new
// text in place after (details::edit_done, OK or not). OK: "/n" (or a
// typed "\n") back to line breaks, written into the drill file's info.description (its info made when missing, the rest kept), in the
// files layer's format (luajson::dump_file: the bytes of its writer) under mech::lock, as Save; the help line's
// description cache dropped.
void details_edit() {
    std::string rel;
    nlohmann::json data;
    {
        Lock lk(mech::lock());
        const auto c = eng().char_name();
        const std::string file = eng().selected_file();
        if (c && !c->empty() && !file.empty()) {
            rel = model::store::pack_path(*c, file);
            data = model::luajson::load_file(eng().abs(rel));
        }
    }
    if (rel.empty() || !data.is_array()) { cmn::journal("ui: manager description edit: no drill file"); ui::details::edit_done(false, ""); return; }
    const std::string initial = ui::details::edit_text(model::pack::description(data));
    ui::dialogs::text_input("drill description", initial, ui::details::kDescMaxUnits, [rel, initial](bool ok, const std::string& text) {
        bool written = false;
        std::string desc;
        try {
            if (ok) {
                desc = ui::details::edit_result(text);
                std::string err;
                {
                    Lock lk(mech::lock());
                    nlohmann::json out;               // (the files layer's bytes, through a temporary file: a failed
                    const std::filesystem::path path = eng().abs(rel);   //  dump leaves the drill as it was)
                    if (model::pack::with_description(model::luajson::load_file(path), desc, &out))
                        written = model::luajson::dump_file(path, out, &err);
                    g_pack_desc.erase(eng().abs(rel).string());
                }
                g_guide_key = "\x01";               // the Drill row's help line made again from the file
                cmn::journal("ui: manager description edit: %zu bytes %s%s", desc.size(), written ? "written" : "NOT written",
                             err.empty() ? "" : (" (" + err + ")").c_str());
            } else {
                cmn::journal("ui: manager description edit cancelled: nothing changed");
            }
        } catch (...) { cmn::fault(F::Ui, "description edit"); }
        ui::details::edit_done(written, desc);       // the details dialog shows it, takes its keys again
    });
    cmn::journal("ui: manager description edit asked (%zu bytes)", initial.size());
}

// The help line's next page (help_pages_on.txt only), every kHelpPageFrames, while the Drill row has the focus and no
// dialog is up (see g_help_pages)
void help_tick(uint64_t fr, bool up) {
    if (g_help_pages.size() < 2) return;
    if (const uint64_t f = g_help_focus.load(); f != g_help_focus_seen) {   // a focus / spin move: the game wrote page 1
        g_help_focus_seen = f;
        g_help_page = 0;
        g_help_next = fr + ui::logic::kHelpPageFrames;
        return;
    }
    if (fr < g_help_next) return;
    g_help_next = fr + ui::logic::kHelpPageFrames;
    if (!up || !g_drill_focused.load() || ui::dialogs::busy() || g_runner.dialog_open() || !page_shown()) return;
    gm::MO* t = ui::gx::input_guide_help();
    if (!t) return;
    const std::string cur = ui::gx::msg(t);           // ours (a page of this drill) or the game's own (by message id: empty)
    const bool ours = cur.empty() || cur == "nil" || std::find(g_help_pages.begin(), g_help_pages.end(), cur) != g_help_pages.end();
    if (!ours) return;
    g_help_page = (g_help_page + 1) % g_help_pages.size();
    ui::gx::set_text(t, g_help_pages[g_help_page]);
    if (g_help_writes++ == 0) cmn::journal("ui: manager help line in %zu pages, 4 s each", g_help_pages.size());
}

// ---- frame callback (game thread) ------------------------------------------------------------------------------------------------
void native_action_off() {
    g_native_action_until = 0;
    g_files.set_native_action(false);
}

void menu_closed(uint64_t fr) {
    g_runner.menu_closed();                                         // a dialog of ours cannot outlive the pause menu
    g_native_action_until = fr + mgr::kNativeActionHold;           // RMT:1157 (this page is the one owner of set_native_action)
    bool pending;
    { std::lock_guard lk(g_mx); pending = g_mem.unresolved(); }
    int64_t rt = 0;
    if (pending && read_record_type(&rt)) {                        // the game page the player left the menu on
        std::lock_guard lk(g_mx);
        g_mem.resolve(rt);
    }
}

void rt_safety() {
    gm::MO* rs = record_setting(gm::singleton("app.training.TrainingManager"));
    int64_t rt = 0;
    if (!rs || !gm::get_int(rs, "RecordType", &rt)) return;
    if (mgr::record_type_needs_reset(rt, false)) gm::set_int(rs, "RecordType", 0);
    g_rt_high = false;
}

// The menu closed: the loadout list of the dummy read ahead (one glob, only when the files layer's list is stale), so the
// page's first build already shows it. Only when the files layer has no dummy yet or the same one as the training
// menu: a disagreement is left to the ensure of the page build (RMT's only call) and to MAIN's dummy hook.
bool ready_list() {
    Lock lk(mech::lock());
    const int id = mech::files::select_menu_id();
    const int held = eng().fighter();
    if (id == -1 || (held != -1 && held != id)) return false;
    return eng().ensure(id);
}

// When ready_list runs (it reads the training menu's dummy id, 6 game reads): only when it can change something (no dummy
// held, or the held dummy's list not read: a dummy change, an invalidate), every 60 frames with the menu closed, at most
// kReadyTries times after each mech event (flow change, training edge, menu close) or change of the held dummy / of the
// list's staleness. Its side effects need exactly that (ensure(id) with a fresh list of the same dummy does nothing).
constexpr int kReadyTries = 10;
uint64_t g_ready_events = UINT64_MAX;
int g_ready_held = INT32_MIN;
bool g_ready_stale = false;
int g_ready_tries = 0;

void ready_tick(uint64_t fr, bool up) {
    int held = -1;
    bool stale = true;
    {
        Lock lk(mech::lock());
        held = eng().fighter();
        stale = held == -1 || !eng().list_fresh();
    }
    const uint64_t ev = mech::events();
    if (ev != g_ready_events || held != g_ready_held || (stale && !g_ready_stale)) g_ready_tries = kReadyTries;
    g_ready_events = ev;
    g_ready_held = held;
    g_ready_stale = stale;
    if (up || !stale || g_ready_tries <= 0 || fr % 60 != 45) return;
    if (mech::pause_now().up()) return;                            // (the menu up while nothing polled the pause: not a try)
    --g_ready_tries;
    if (ready_list()) g_list_dirty = true;
}

void left_training() {
    {
        std::lock_guard lk(g_mx);
        g_mem.reset();                                              // RMT:2095: current_page = nil outside training
    }
    g_queue.drain();                                                // nothing queued in a menu survives the training session
    g_runner.menu_closed();
    g_menu_up = false;
    if (g_native_action_until) native_action_off();
}

void frame_body(uint64_t fr) {
    if (!cmn::owns(F::Files)) { cmn::decline(F::Ui, "the files layer went back to the Lua"); return; }
    const mech::State st = mech::state();
    if (!st.training) {
        if (g_training) left_training();
        g_training = false;
        return;
    }
    g_training = true;
    if (ui::lang::version() != g_lang_seen) retext();              // (the language probe runs in ui.cpp's frame callback)
    // the menu (the pause bit or an option window): read while this page saw it up or mech polls it (the windows' open menu,
    // a dialog, the 120 frames after CreateViewData); otherwise nothing of ours is up
    const bool up = (st.polled || g_menu_up) && mech::pause_now().up();
    if (g_menu_up && !up) menu_closed(fr);
    g_menu_up = up;
    if (g_native_action_until && fr > g_native_action_until) native_action_off();
    for (const Action& a : g_queue.drain()) {
        g_actions.fetch_add(1);
        if (a.act != Act::Select && a.act != Act::Ensure) cmn::journal("ui: manager %s", mgr::act_name(a.act));
        g_runner.run(a);
    }
    // the Drill row's Confirm armed again once the decide input is up with no dialog (cb_confirm): read only while unarmed
    if (ui::dialogs::busy() || g_runner.dialog_open()) g_confirm_armed.store(false);
    else if (!g_confirm_armed.load()) {
        bool held = cmn::key_down(0x46);                            // F
        if (!held) { std::lock_guard lk(ui::lock()); held = ui::windows::pad_held().south; }   // pad A / Cross
        if (!held) g_confirm_armed.store(true);
    }
    if (g_edit_due.exchange(false)) {                               // (never left waiting: the details take their keys back)
        bool asked = false;
        if (up) { try { details_edit(); asked = true; } catch (...) { cmn::fault(F::Ui, "description edit"); } }
        if (!asked) ui::details::edit_done(false, "");
    }
    if (g_details_due.exchange(false) && up) {
        try { details_open(); } catch (...) { cmn::fault(F::Ui, "drill details"); }
    }
    ready_tick(fr, up);
    // the files layer's labels (no game read): when its list or index changed (list_version follows both), when an action
    // asked, and once a second (was: every frame while the menu was up, 4.5 us a frame for the same labels)
    uint64_t lv = 0;
    { Lock lk(mech::lock()); lv = eng().list_version(); }
    if (g_list_dirty || lv != g_list_seen || fr % 60 == 45) { g_list_seen = lv; sync(); loadout_guide(lv); }
    try { help_tick(fr, up); } catch (...) { cmn::fault(F::Ui, "help line pages"); }
    if (g_rt_high && !up && fr % 30 == 7 && !mech::pause_now().up()) rt_safety();
}

bool menu_up_seen() { return g_menu_up; }

void frame(uint64_t fr) {
    if (!ui::active() || g_page.load() < 0) return;                 // only once the Lua loader handed the UI over (claim)
    try { frame_body(fr); } catch (...) { cmn::fault(F::Ui, "manager frame"); }
}

// Given back to the Lua (faults, declined): the page taken out at the core's next pass, the memory and queue dropped.
void hand_back() {
    const int64_t page = g_page.exchange(-1);
    if (page >= 0 && g_api && g_client) perf::counted(g_api)->pm_remove(g_client, page);
    try {
        if (g_rt_high && !menu_up_live()) rt_safety();
        if (g_native_action_until) native_action_off();
    } catch (...) {}
    {
        std::lock_guard lk(g_mx);
        g_mem.reset();
        g_labels.clear();
    }
    g_queue.drain();
    g_runner.menu_closed();
    if (page >= 0) cmn::log_info("[CRSMNative] ui: Manager page removed (the strip goes back to Record / Replay at the core's next pass)");
}

std::string status() {
    if (g_page.load() < 0)
        return cmn::owns(F::Ui) ? (g_claimed ? "native, no page" : "native, the page waits for the Lua loader's claim") : "lua";
    const char* mem = "none";
    size_t labels = 0;
    {
        std::lock_guard lk(g_mx);
        mem = g_mem.page() == mgr::PageMemory::Page::None ? "the game's memory" : mgr::page_name(g_mem.page());
        labels = g_labels.size();
    }
    char b[416];
    std::snprintf(b, sizeof(b), "native, page %lld (8 rows), %zu loadout labels, reopen on %s, %s, %llu decides, %llu spins, %llu actions, "
                  "%llu hints, %llu RecordType writes, %llu guides at a move, %llu Confirms on the Drill row, queue %zu",
                  (long long)g_page.load(), labels, mem, ui::lang::status().c_str(), (unsigned long long)g_decides.load(),
                  (unsigned long long)g_spins.load(), (unsigned long long)g_actions.load(), (unsigned long long)g_hints.load(),
                  (unsigned long long)g_rt_writes.load(), (unsigned long long)g_guide_at_spin.load(),
                  (unsigned long long)g_confirms.load(), g_queue.size());
    return b;
}

bool build_page() {
    SF6CorePmCallbacks cb{};
    cb.struct_size = sizeof(cb);
    cb.on_decide = cb_decide;
    cb.spin_value = cb_spin_value;
    cb.on_spin = cb_on_spin;
    cb.on_page_shown = cb_page_shown;
    cb.hints = cb_hints;
    cb.on_confirm = cb_confirm;                                     // core 0.9.5 (an older core ignores it: no details)
    const SF6CoreStatus cst = perf::counted(g_api)->pm_set_callbacks(g_client, &cb);
    if (cst != SF6CORE_OK) {
        cmn::decline(F::Ui, ("pm_set_callbacks refused (" + std::to_string((int)cst) + ")").c_str());
        return false;
    }
    const std::string title = mgr::page_title(tr);
    SF6CorePmPageDesc pd{};
    pd.struct_size = sizeof(pd);
    pd.primary_func = mgr::kRecordFunc;
    pd.strip_index = mgr::kStripIndex;
    pd.title = title.c_str();
    const std::string guide = mgr::page_guide(tr);
    pd.guide = guide.c_str();
    const int64_t page = perf::counted(g_api)->pm_page(g_client, &pd);
    if (page < 0) { cmn::decline(F::Ui, "pm_page refused"); return false; }
    for (int i = 0; i < mgr::kRowCount; ++i) {
        const mgr::RowText t = mgr::row_text((mgr::Row)i, tr);
        SF6CorePmRowDesc rd{};
        rd.struct_size = sizeof(rd);
        rd.kind = i == mgr::kLoadout ? SF6CORE_PM_SPIN : SF6CORE_PM_BUTTON;
        rd.title = t.title.c_str();
        rd.guide = t.guide.c_str();
        rd.capacity = i == mgr::kLoadout ? mgr::kSpinCapacity : 0;
        rd.keep_open = 1;                                           // every button keeps the pause menu open (RMT:110...)
        rd.arrow_spread = i == mgr::kLoadout ? mgr::kArrowSpread : 0.0f;
        const int64_t row = perf::counted(g_api)->pm_row(g_client, page, &rd);
        if (row < 0) {
            perf::counted(g_api)->pm_remove(g_client, page);
            cmn::decline(F::Ui, ("pm_row refused for row " + std::to_string(i + 1)).c_str());
            return false;
        }
        g_rows[i].store(row);
        if (i == mgr::kLoadout) { std::lock_guard gl(g_guide_mx); g_guide_given = t.guide; }
    }
    const std::string no_file = tr("select_file", {});
    const char* first[] = { no_file.c_str() };
    perf::counted(g_api)->pm_set_options(g_client, g_rows[mgr::kLoadout].load(), first, 1);
    {
        std::lock_guard lk(g_mx);
        g_labels = { no_file };
        g_hint_text = { tr("edit_name", {}), tr("delete_name", {}) };
    }
    g_lang_seen = ui::lang::version();
    g_page.store(page);
    return true;
}

}  // namespace

void init() {
    for (auto& r : g_rows) r.store(-1);
    cmn::set_implemented(F::Ui);
    cmn::on_hand_back(F::Ui, hand_back);
    cmn::add_frame_fn(frame, "ui_manager_page");
    cmn::add_status("ui_manager", status);
}

void start() {
    if (!cmn::owns(F::Ui)) return;
    g_api = cmn::core();
    g_client = cmn::client();
    if (!g_api || !g_client || !SF6CORE_HAS(g_api, pm_status)) {
        cmn::decline(F::Ui, "the Native Core has no training pause-menu service (needs core 0.8.0+), no Manager page");
        return;
    }
    if (!cmn::owns(F::Files)) {
        cmn::decline(F::Ui, "needs files=native: the Manager page runs on the DLL's files layer");
        return;
    }
    g_ready = true;                                                 // the page is asked at the claim: never two engines in the strip
}

bool claim() {
    std::lock_guard lk(g_claim_mx);
    if (g_page.load() >= 0) return true;
    if (g_claimed || !g_ready || !cmn::owns(F::Ui) || cmn::closing()) return false;
    g_claimed = true;
    try {
        if (!build_page()) return false;                            // declined (one log line): the loader keeps the Lua UI
        char b[256] = {};
        perf::counted(g_api)->pm_status(b, sizeof(b));
        cmn::log_info("[CRSMNative] ui: Manager page asked of the core (page %lld, 8 rows, Recording strip index 0); core: %s",
                      (long long)g_page.load(), b);
        return true;
    } catch (...) {
        cmn::decline(F::Ui, "the Manager page could not be asked of the core");
        return false;
    }
}

void before_create_view_data() {
    if (g_page.load() < 0) return;
    try { cvd_body(); } catch (...) { cmn::fault(F::Ui, "CreateViewData pre (sub-page memory)"); }
}

void game_page_built(bool replay) {
    if (g_page.load() < 0) return;
    try { game_page_body(replay); } catch (...) { cmn::fault(F::Ui, "Function pre (sub-page memory)"); }
}

bool page_shown() {
    if (!live() || !g_api) return false;
    return perf::counted(g_api)->pm_page_shown(g_page.load()) != 0;
}

uint64_t rows_epoch() { return g_rows_epoch.load(); }

bool needs_pause() { return menu_up_seen(); }

bool refresh_list() {
    if (!live()) return false;
    g_queue.push(Action{ Act::Refresh, 0 });
    return true;
}

void rename_key() {
    if (live()) g_queue.push(Action{ Act::Rename, 0 });            // run on the frame callback, as a decide (RMT:1764)
}

}  // namespace ui::manager
