#ifndef NOMINMAX
#define NOMINMAX
#endif
// dialogs::options::core - see options_core.hpp. No game header in this file: it is also compiled, as it is,
// into the offline harness (tools/options_harness).
//
// Each block cites the lines of func/NativeOptions.lua it ports; the order of the statements is the Lua's.

#include "options_core.hpp"

#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace dialogs::options::core {
namespace {

constexpr char kFieldSep = '\x1f';
constexpr char kRecordSep = '\x1e';
constexpr int64_t kNone = -1;          // a Lua nil tick (ticks start at 1)

struct OldEntry { Obj h; int64_t at; };

Backend be;
std::recursive_mutex mx;               // recursive: a hook can fire inside a game call made from here (Start)

// registry
std::vector<std::shared_ptr<Group>> g_groups;
std::unordered_map<int64_t, std::weak_ptr<Entry>> g_entries_by_id;
int64_t g_next_group_id = 1, g_next_entry_id = 1;
std::unordered_map<int32_t, std::shared_ptr<Entry>> g_known_ids;      // :108
std::unordered_map<std::string, int32_t> g_ids_by_key;                // :209
int32_t g_next_type_id = 0;
bool g_type_id_inited = false;

// built state (:200-201)
bool g_built = false;
Obj g_root = nullptr;
bool g_any_root = false;                // a root was added to the game's list since the last reset (:598 ids_by_key["__root"])
bool g_any_placed = false;              // a group was ever placed in a game section (increment 11): its rows are checked

// dialog lifecycle (:655-665, :681, :714, :730)
std::string g_open_request;  bool g_has_request = false;
std::string g_deferred_open; bool g_has_deferred = false;
int g_deferred_stable = 0;
int64_t g_open_at = kNone, g_open_started = kNone, g_reopen_wait = kNone, g_rehold_wait = kNone, g_close_rebuild = kNone;
std::string g_reopen_title;  bool g_has_reopen_title = false;
int64_t g_closed_tick = -1000;
int64_t g_tick = 0;
bool g_menu_mode = false;
bool g_pause_held = false;
bool g_paused = false;                 // _G.TrainingGamePaused, given at each tick
bool g_window_open = false;            // _G.NativeOptionsWindowOpen

// :397-402
int g_pending_focus = -1;
Obj g_dialog_handle = nullptr;
Obj g_old_handle = nullptr;  int64_t g_old_end_at = kNone;
Obj g_old_parent = nullptr;
std::vector<OldEntry> g_old_queue;
std::vector<Obj> g_dialog_stack, g_param_stack;
bool g_replace_pending = false, g_pop_parent = false;
Obj g_current_param = nullptr;         // M.current_param
std::unordered_map<Obj, std::vector<std::shared_ptr<Entry>>> g_page_entries;

std::vector<std::string> g_events;

// ---- references: the Lua holds one on every game object it keeps in a variable ---------------------------------
Obj hold(Obj o) { if (o && be.retain) be.retain(o); return o; }
void drop(Obj& o) { if (o && be.release) be.release(o); o = nullptr; }
void drop_all(std::vector<Obj>& v) { for (auto& o : v) drop(o); v.clear(); }
void set_current_param(Obj p) { Obj old = g_current_param; g_current_param = hold(p); drop(old); }

void event(std::initializer_list<std::string> fields) {
    std::string r;
    for (auto& f : fields) { if (!r.empty()) r += kFieldSep; r += f; }
    g_events.push_back(std::move(r));
}
std::string addr(Obj o) { return std::to_string(reinterpret_cast<uint64_t>(o)); }

// :110-121
int32_t new_type_id() {
    if (!g_type_id_inited) { g_next_type_id = be.type_id_base ? be.type_id_base() : 100000; g_type_id_inited = true; }
    return ++g_next_type_id;
}

Group* find_group(int64_t id) {
    for (auto& g : g_groups) if (g->id == id) return g.get();
    return nullptr;
}

// :715-720
void hold_pause(bool on) {
    if (g_menu_mode && on) return;                              // menus: nothing to freeze
    if (on == g_pause_held) return;
    if (be.request_pause && be.request_pause(on)) g_pause_held = on;
}

void end_one(Obj& h) { if (h) { if (be.end_handle) be.end_handle(h); drop(h); } }

// :731-748
void end_dialog() {
    end_one(g_dialog_handle);
    if (g_old_handle) { g_old_end_at = kNone; end_one(g_old_handle); }
    end_one(g_old_parent);
    for (auto& q : g_old_queue) end_one(q.h);
    g_old_queue.clear();
    for (int i = (int)g_dialog_stack.size() - 1; i >= 0; --i) end_one(g_dialog_stack[i]);
    g_dialog_stack.clear();
    drop_all(g_param_stack); g_replace_pending = false; g_pop_parent = false;   // param_stack = {} (its two flags are fields of the table)
    g_page_entries.clear();
}

// :752-761
void start_dialog(Obj unit) {
    if (g_dialog_handle) end_one(g_dialog_handle);              // only the current one (the stacked parents stay)
    Obj h = be.start_dialog ? be.start_dialog(unit) : nullptr;
    g_dialog_handle = hold(h);
}

// :762-770 (the composite window is not ported: no unit to open)
void do_open(const std::string& title) {
    for (auto& g : g_groups) {
        if (((!g->key.empty() && g->key == title) || g->title == title) && g->unit && g->unit != g_root) {
            start_dialog(g->unit);
            return;
        }
    }
}

// :774-791
void seamless_open(const std::string& key, bool rebuild_tree, bool push, bool pop) {
    Obj old = g_dialog_handle; g_dialog_handle = nullptr;
    if (rebuild_tree || !g_built) { rebuild(); build(); }
    if (g_built) {
        do_open(key); g_open_started = g_tick;
        if (old) {
            if (push) { g_dialog_stack.push_back(old); old = nullptr; }      // parent stays alive underneath
            else {
                if (g_old_handle) g_old_queue.push_back({ g_old_handle, g_old_end_at });   // previous replace still pending
                if (g_old_parent) { g_old_queue.push_back({ g_old_parent, g_old_end_at }); g_old_parent = nullptr; }
                g_old_handle = old; old = nullptr; g_old_end_at = g_tick + kSeamlessOldEnd; g_replace_pending = true;
                if (pop && !g_dialog_stack.empty()) {               // the new page replaces the parent too
                    g_old_parent = g_dialog_stack.back(); g_dialog_stack.pop_back(); g_pop_parent = true;
                }
            }
        }
    }
    drop(old);                                                   // not built: the Lua lets its local go
}

// :725-728 fire_entry is the wrapper's; here: the value read back from the unit
void poll(const std::shared_ptr<Entry>& e) {
    if (e->kind == Kind::Button || !e->unit || !be.get_value) return;   // a button's unit has no value (the Lua's pcall fails)
    int32_t n = 0;
    if (!be.get_value(e->unit, n)) return;
    if (n != e->last_int) {
        e->last_int = n; e->value = n;
        event({ "V", std::to_string(e->id), std::to_string(n) });
    }
}

// :822-849, the branches after the first one of the chain
int chain_rest() {
    if (g_open_at != kNone && g_tick >= g_open_at) {
        const std::string t = g_open_request; g_has_request = false; g_open_request.clear(); g_open_at = kNone; g_open_started = g_tick;
        do_open(t);
        if (!g_menu_mode) hold_pause(true);
    } else if (g_close_rebuild != kNone) {
        // the window is closing: rebuild for the NEXT open only once the dialog agent is gone (90-tick safety net)
        if ((g_tick - g_close_rebuild) > kCloseRebuildSafety || !(be.agent_present && be.agent_present(kAgentName))) {
            g_close_rebuild = kNone;
            return kTickRebuild;
        }
    } else if (g_reopen_wait != kNone) {
        const int64_t elapsed = g_tick - g_reopen_wait;
        if (elapsed > kReopenWaitSafety || !(be.agent_present && be.agent_present(kAgentName))) {
            g_reopen_wait = kNone; g_open_started = g_tick;
            do_open(g_has_reopen_title ? g_reopen_title : std::string(kComposite));
            g_has_reopen_title = false; g_reopen_title.clear();
            g_rehold_wait = g_tick;
        } else if (elapsed == kReopenPauseRelease) {
            hold_pause(false);
        }
    } else if (g_rehold_wait != kNone) {
        const int64_t elapsed = g_tick - g_rehold_wait;
        if ((elapsed >= kReholdWaitMin && be.agent_present && be.agent_present(kAgentName)) || elapsed > kReholdWaitSafety) {
            g_rehold_wait = kNone;
            if (g_window_open && !g_menu_mode) hold_pause(true);
        }
    }
    return kTickNone;
}

}  // namespace

void set_backend(const Backend& b) { std::lock_guard lk(mx); be = b; }

// ---- registry --------------------------------------------------------------------------------------------------

int64_t add_group(const std::string& key) {
    std::lock_guard lk(mx);
    auto g = std::make_shared<Group>();
    g->id = g_next_group_id++; g->key = key;
    g_groups.push_back(g);
    return g->id;
}

// :136-138. The entries stay alive for as long as a built tree or a page still shows them, like the Lua's tables.
void remove_group(int64_t group_id) {
    std::lock_guard lk(mx);
    for (auto it = g_groups.begin(); it != g_groups.end(); ++it) if ((*it)->id == group_id) { g_groups.erase(it); return; }
}

int64_t add_entry(int64_t group_id, Kind kind, const std::string& key) {
    std::lock_guard lk(mx);
    Group* g = find_group(group_id);
    if (!g) return -1;
    auto e = std::make_shared<Entry>();
    e->id = g_next_entry_id++; e->kind = kind; e->key = key;
    e->placed = g->place_tab >= 0;
    g->entries.push_back(e);
    g_entries_by_id[e->id] = e;
    return e->id;
}

// Increment 11. Applied at the next build (the units are made there); the rows a group leaves are removed from the
// game's section by the backend's sync_placed.
void set_group_place(int64_t group_id, int32_t tab, int32_t section) {
    std::lock_guard lk(mx);
    Group* g = find_group(group_id);
    if (!g) return;
    const bool on = tab >= 0 && section >= 0;
    g->place_tab = on ? tab : -1; g->place_section = on ? section : -1;
    for (auto& e : g->entries) e->placed = on;
    if (on) g_any_placed = true;
}

void set_group_def(int64_t group_id, const std::string& title, const std::string& desc) {
    std::lock_guard lk(mx);
    if (Group* g = find_group(group_id)) { g->title = title; g->desc = desc; }
}
std::shared_ptr<Entry> entry_by_id(int64_t entry_id) {
    std::lock_guard lk(mx);
    auto it = g_entries_by_id.find(entry_id);
    if (it == g_entries_by_id.end()) return nullptr;
    auto e = it->second.lock();
    if (!e) g_entries_by_id.erase(it);
    return e;
}
void set_entry_def(int64_t entry_id, const std::string& title, const std::string& desc,
                   const std::vector<std::string>& options, int32_t min_val, int32_t max_val, int64_t value) {
    std::lock_guard lk(mx);
    auto e = entry_by_id(entry_id);
    if (!e) return;
    e->title = title; e->desc = desc; e->options = options; e->min_val = min_val; e->max_val = max_val;
    e->value = value;                                             // InitValue of the next build; last_int is the unit's
}

std::shared_ptr<Entry> entry_by_type(int32_t type_id) {
    std::lock_guard lk(mx);
    auto it = g_known_ids.find(type_id);
    return it == g_known_ids.end() ? nullptr : it->second;
}
bool known(int32_t type_id) { std::lock_guard lk(mx); return g_known_ids.count(type_id) != 0; }
bool placed_type(int32_t type_id) {
    std::lock_guard lk(mx);
    auto it = g_known_ids.find(type_id);
    return it != g_known_ids.end() && it->second->placed;
}

// :193-197 Group:set and :1095-1101 M.set_value: the value is the game's int here
void set_value(int64_t entry_id, int64_t value, bool refresh) {
    std::lock_guard lk(mx);
    auto e = entry_by_id(entry_id);
    if (!e) return;
    e->value = value; e->last_int = value;
    if (e->unit && be.set_value) be.set_value(e->unit, (int32_t)value, refresh);
}

// ---- build -----------------------------------------------------------------------------------------------------

const std::vector<std::shared_ptr<Group>>& groups() { return g_groups; }
std::string id_key_of(const Group& g) { return g.key.empty() ? g.title : g.key; }

int32_t type_id_for_key(const std::string& key) {
    std::lock_guard lk(mx);
    auto it = g_ids_by_key.find(key);
    if (it != g_ids_by_key.end()) return it->second;
    const int32_t id = new_type_id();
    g_ids_by_key[key] = id;
    return id;
}
bool has_type_id_for_key(const std::string& key, int32_t& out) {
    std::lock_guard lk(mx);
    auto it = g_ids_by_key.find(key);
    if (it == g_ids_by_key.end()) return false;
    out = it->second;
    return true;
}
void build_set_root(Obj root) { std::lock_guard lk(mx); g_root = root; g_any_root = true; }
void build_set_group(Group& g, Obj unit) { std::lock_guard lk(mx); g.unit = unit; }
void build_set_entry(const std::shared_ptr<Entry>& e, int32_t type_id, Obj unit) {
    std::lock_guard lk(mx);
    e->type_id = type_id; g_known_ids[type_id] = e; e->unit = unit;
    if (e->kind != Kind::Button) e->last_int = e->value;        // :329 e.last_int = vs.InitValue
}
Obj root_unit() { return g_root; }
bool built() { return g_built; }

bool build() {
    std::lock_guard lk(mx);
    if (be.build_units && be.build_units()) {                   // :384
        g_built = true;
        // built with the window closed: no dialog can show a retired tree, the roots this one replaces go now
        // (they used to wait for the next close: two "SF6 Tools" rows in Options > General)
        if (!g_window_open && be.purge_roots) be.purge_roots(true);
    }
    return g_built;
}

// :608-612. The old root stays in the game's list (its tree stays alive under the dialogs still showing it);
// purge_roots removes it once the window is closed.
void rebuild() {
    std::lock_guard lk(mx);
    g_root = nullptr; g_built = false; g_known_ids.clear();
    for (auto& g : g_groups) { g->unit = nullptr; for (auto& e : g->entries) e->unit = nullptr; }
}

// ---- requests ---------------------------------------------------------------------------------------------------

void request_open(const std::string& title) { std::lock_guard lk(mx); g_open_request = title; g_has_request = true; }

void request_open_menu(const std::string& title, int focus_index) {
    std::lock_guard lk(mx);
    if (g_window_open) return;
    g_menu_mode = true;
    g_open_request = title; g_has_request = true;
    g_pending_focus = focus_index;
}

void open_after_unpause(const std::string& title) {
    std::lock_guard lk(mx);
    g_deferred_open = title; g_has_deferred = true;
    g_deferred_stable = 0;
}

bool navigate_runs_now(bool now) { std::lock_guard lk(mx); return g_window_open && now && g_menu_mode; }

void navigate(const std::string& key, bool rebuild_tree, int focus_index, bool now, bool push, bool pop) {
    std::lock_guard lk(mx);
    if (!g_window_open) return;
    g_pending_focus = focus_index;
    if (now && g_menu_mode) { seamless_open(key, rebuild_tree, push, pop); return; }
    event({ "N", key, std::to_string((rebuild_tree ? 1 : 0) | (push ? 2 : 0) | (pop ? 4 : 0)) });
}

void close() { std::lock_guard lk(mx); end_dialog(); }

// ---- the per-frame callback --------------------------------------------------------------------------------------

int tick_a(int64_t tick, bool paused, bool training_active) {
    std::lock_guard lk(mx);
    g_tick = tick; g_paused = paused;
    if (!g_built) return (g_tick % kBuildRetry == 1 && !g_groups.empty()) ? kTickBuild : kTickNotBuilt;
    // :802-813 deferred open (from pause-menu buttons): both flags false for 5 stable ticks
    if (g_has_deferred) {
        if (!paused && !g_window_open) {
            if (++g_deferred_stable >= kDeferredStable) {
                g_open_request = g_deferred_open; g_has_request = true;
                g_has_deferred = false; g_deferred_open.clear();
                g_deferred_stable = 0;
            }
        } else {
            g_deferred_stable = 0;
        }
    }
    // :816 pause won the race: drop
    if (g_has_request && paused && !g_menu_mode && !g_window_open) { g_has_request = false; g_open_request.clear(); }
    // :817 two-phase open, first phase: the wrapper re-reads its dynamic lists and getters, then calls phase1()
    if (g_has_request && (g_menu_mode || !paused) && !g_window_open && (training_active || g_menu_mode)) return kTickPhase1;
    return chain_rest();
}

void phase1() { std::lock_guard lk(mx); g_window_open = true; g_open_at = g_tick + kOpenDelay; }

void tick_a2() {
    std::lock_guard lk(mx);
    // :850-854
    if (g_old_handle && g_old_end_at != kNone && g_tick >= g_old_end_at) {
        g_old_end_at = kNone; end_one(g_old_handle);
        end_one(g_old_parent);
    }
    // :855-861
    if (!g_old_queue.empty()) {
        std::vector<OldEntry> keep;
        for (auto& q : g_old_queue) { if (g_tick >= (q.at == kNone ? 0 : q.at)) end_one(q.h); else keep.push_back(q); }
        g_old_queue = keep;
    }
}

bool button_result_builds(bool want_close, bool want_open, bool want_rebuild, bool want_refresh) {
    std::lock_guard lk(mx);
    if (want_close) return false;
    if (want_open) return want_rebuild || !g_built;
    return want_refresh;
}

void button_result(bool want_close, bool want_open, const std::string& open_key, bool want_rebuild,
                   bool want_push, bool want_pop, bool want_refresh) {
    std::lock_guard lk(mx);
    if (want_close) {                                            // :872-879 the action wants the window gone
        end_dialog(); hold_pause(false); g_close_rebuild = g_tick;
    } else if (want_open) {                                      // :880-888 navigate to another page
        const bool was_open = g_window_open;
        if (was_open && g_menu_mode) {
            seamless_open(open_key, want_rebuild, want_push, want_pop);
        } else {
            end_dialog();
            if (want_rebuild || !g_built) { rebuild(); build(); }
            if (was_open && g_built) { g_reopen_title = open_key; g_has_reopen_title = true; g_reopen_wait = g_tick; }
        }
    } else if (want_refresh) {                                   // :889-894 dynamic titles: rebuild and reopen
        const bool was_open = g_window_open;
        end_dialog();
        rebuild(); build();
        if (was_open && g_built) g_reopen_wait = g_tick;
    }
}

void tick_b() {
    std::lock_guard lk(mx);
    // :908-915 the game closed the top page (Esc / B): the parent is up
    if (g_menu_mode && g_window_open && g_dialog_handle && !g_dialog_stack.empty() && !g_old_handle) {
        bool ended = false;
        if (be.get_is_end && be.get_is_end(g_dialog_handle, ended) && ended) {
            drop(g_dialog_handle);
            g_dialog_handle = g_dialog_stack.back(); g_dialog_stack.pop_back();
            if (!g_param_stack.empty()) { drop(g_param_stack.back()); g_param_stack.pop_back(); }
            set_current_param(g_param_stack.empty() ? nullptr : g_param_stack.back());
            event({ "P", addr(g_current_param) });
        }
    }
    // :922-944 the page on screen is read every tick; a page not recorded: every entry
    if (g_window_open) {
        auto it = g_current_param ? g_page_entries.find(g_current_param) : g_page_entries.end();
        if (it != g_page_entries.end()) {
            const auto shown = it->second;                       // copy: a poll can not change it, a hook could
            for (auto& e : shown) poll(e);
        } else {
            const auto all = g_groups;
            for (auto& g : all) { const auto es = g->entries; for (auto& e : es) poll(e); }
        }
    }
    if (g_tick % kValueFullPass != 0) return;
    // :946-958 close detection
    if (g_window_open && g_open_at == kNone && g_reopen_wait == kNone && !g_old_handle
        && (g_tick - (g_open_started == kNone ? 0 : g_open_started)) > kCloseDetectMin) {
        bool ended = false;
        if (g_dialog_handle && !(be.get_is_end && be.get_is_end(g_dialog_handle, ended))) ended = false;
        if (ended || !(be.agent_present && be.agent_present(kAgentName))) {
            g_window_open = false; g_closed_tick = g_tick; drop(g_dialog_handle);
            hold_pause(false); g_menu_mode = false;
            drop_all(g_param_stack); g_replace_pending = false; g_pop_parent = false; g_page_entries.clear();
            // the window is closed, no dialog can reference a retired tree
            if (be.purge_roots) be.purge_roots(true);
            event({ "X" });
        }
    }
    // :959-988 composite window (mode spin, mirrored entries): not ported
    // :989-995 every entry, every 10 ticks (the deferred callbacks of :996-999 are the wrapper's)
    const auto all = g_groups;
    for (auto& g : all) { const auto es = g->entries; for (auto& e : es) poll(e); }
    // increment 11: the rows placed in a game section are still there (the game may rebuild its lists), the rows of
    // removed / moved groups are gone. Nothing to do for a tree that never placed a group.
    if (g_any_placed && g_tick % kPlaceCheck == 0 && be.sync_placed) be.sync_placed(true);
}

// :1003-1015, then what a fresh Lua state starts from
void reset() {
    std::lock_guard lk(mx);
    end_dialog();
    if (g_any_root && be.purge_roots) be.purge_roots(false);        // nothing of ours in the list: nothing to purge
    g_any_root = false; g_any_placed = false;
    g_groups.clear(); g_entries_by_id.clear(); g_next_group_id = 1; g_next_entry_id = 1;
    g_known_ids.clear(); g_ids_by_key.clear(); g_type_id_inited = false;
    g_built = false; g_root = nullptr;
    g_open_request.clear(); g_has_request = false; g_deferred_open.clear(); g_has_deferred = false; g_deferred_stable = 0;
    g_open_at = g_open_started = g_reopen_wait = g_rehold_wait = g_close_rebuild = kNone;
    g_reopen_title.clear(); g_has_reopen_title = false;
    g_closed_tick = -1000; g_tick = 0; g_menu_mode = false; g_pause_held = false; g_paused = false; g_window_open = false;
    g_pending_focus = -1;
    set_current_param(nullptr);
    g_events.clear();
}

// ---- hooks -------------------------------------------------------------------------------------------------------

bool window_open() { return g_window_open; }

void on_created_object(Obj param) {
    std::lock_guard lk(mx);
    if (!param) return;
    if (g_replace_pending) { g_replace_pending = false; if (!g_param_stack.empty()) { drop(g_param_stack.back()); g_param_stack.pop_back(); } }
    if (g_pop_parent) { g_pop_parent = false; if (!g_param_stack.empty()) { drop(g_param_stack.back()); g_param_stack.pop_back(); } }
    g_param_stack.push_back(hold(param));
    set_current_param(param);
    event({ "C", addr(param) });
}

int take_pending_focus() {
    std::lock_guard lk(mx);
    if (g_pending_focus < 0 || !g_window_open) return -1;
    const int n = g_pending_focus; g_pending_focus = -1;
    return n;
}

bool on_page_setup(Obj param, const std::vector<PagePart>& parts) {
    std::lock_guard lk(mx);
    bool ours = false;
    std::vector<std::shared_ptr<Entry>> shown;
    std::string rec = "S"; rec += kFieldSep; rec += addr(param);
    for (auto& p : parts) {
        auto it = g_known_ids.find(p.type_id);
        if (it == g_known_ids.end() || it->second->placed) continue;   // a placed row is in a game page, not one of ours
        ours = true; shown.push_back(it->second);
        rec += kFieldSep; rec += std::to_string(it->second->id); rec += kFieldSep; rec += addr(p.part);
    }
    if (ours) { g_page_entries[param] = std::move(shown); g_events.push_back(std::move(rec)); }
    else g_page_entries.erase(param);                            // the every-tick value pass reads only our pages
    return ours;
}

bool on_button(int32_t type_id) {
    std::lock_guard lk(mx);
    auto it = g_known_ids.find(type_id);
    if (it == g_known_ids.end() || it->second->kind != Kind::Button) return false;
    event({ "B", std::to_string(it->second->id) });
    return true;
}

bool suppress_open_menu() { return g_window_open || (g_tick - g_closed_tick) < kOpenMenuSuppress; }

Obj current_param() { return g_current_param; }
Obj parent_param() {
    std::lock_guard lk(mx);
    return g_param_stack.size() >= 2 ? g_param_stack[g_param_stack.size() - 2] : nullptr;
}

std::string pull_events() {
    std::lock_guard lk(mx);
    std::string out;
    for (auto& r : g_events) { if (!out.empty()) out += kRecordSep; out += r; }
    g_events.clear();
    return out;
}

std::string status() {
    std::lock_guard lk(mx);
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "built=%d groups=%zu known=%zu win=%d menu=%d pause=%d tick=%lld handle=%d stack=%zu params=%zu old=%d queue=%zu "
        "open_at=%lld close_rebuild=%lld reopen=%lld rehold=%lld pages=%zu events=%zu",
        (int)g_built, g_groups.size(), g_known_ids.size(), (int)g_window_open, (int)g_menu_mode, (int)g_pause_held,
        (long long)g_tick, g_dialog_handle ? 1 : 0, g_dialog_stack.size(), g_param_stack.size(), g_old_handle ? 1 : 0,
        g_old_queue.size(), (long long)g_open_at, (long long)g_close_rebuild, (long long)g_reopen_wait,
        (long long)g_rehold_wait, g_page_entries.size(), g_events.size());
    return buf;
}

}  // namespace dialogs::options::core
