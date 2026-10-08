// options_service.cpp - increment 6: the native option dialogs as a core service (ADR-0013, docs/OPTIONS.md).
//
// The engine (options_core: registry, dialog life cycle, value polling, events; options: the game side and its eight
// hooks; guid_text: the GetMessage hook) moved here unchanged from SF6_ColorModNative 0.18.5. This file is the driver
// that the mod had around it (its dialogs/options_driver.cpp, itself a port of func/NativeOptions.lua's native block),
// made generic: every group and entry belongs to the client that created it, its events go back to that client's
// callbacks, and the dialog events (created / page set up / popped / closed) go to every client.
//
// Frame model: tick() runs in the core's LateUpdateBehavior callback, before the clients' frame callbacks (the mod
// ran it at the start of its screens' frame); pump() runs in BeginRendering (the mod's second callback). The hooks are
// installed on the first frame after a client asked for the service, so a core without option clients hooks nothing.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../sf6core_internal.h"
#include "opt_common.hpp"
#include "guid_text.hpp"
#include "options.hpp"
#include "options_core.hpp"

// ---- the engine files' cmn:: (opt_common.hpp) -------------------------------------------------------------------
namespace cmn {
namespace { SF6CoreClient* s_client = nullptr; }
void journal(const char* fmt, ...) {
    char buf[480];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    core_log(SF6CORE_LOG_INFO, nullptr, "%s", buf);
}
void log_warn(const char* fmt, ...) {
    char buf[480];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    core_log(SF6CORE_LOG_WARN, nullptr, "%s", buf);
}
const SF6CoreAPI* core() { return sf6core_api_ptr(); }
SF6CoreClient* client() { return s_client; }
void set_client(SF6CoreClient* c) { s_client = c; }
bool closing() { return core_closing(); }
}  // namespace cmn

namespace {

namespace oc = dialogs::options::core;
constexpr char US = '\x1f', RS = '\x1e';

// ---- clients ------------------------------------------------------------------------------------------------------
struct Owner { SF6CoreClient* client = nullptr; SF6CoreOptCallbacks cb{}; };
std::vector<Owner> g_owners;

Owner* owner_of(SF6CoreClient* c) {
    for (auto& o : g_owners) if (o.client == c) return &o;
    return nullptr;
}
bool client_live(SF6CoreClient* c) { return c && c->active && c->state == SF6CORE_CLIENT_ACTIVE; }

// One callback call, SEH-guarded (no C++ object in the __try frame).
struct CbCall {
    int what = 0;                         // 1 value, 2 button, 3 created, 4 page, 5 popped, 6 closed
    const SF6CoreOptCallbacks* cb = nullptr;
    int64_t a = 0, b = 0;
    void* p = nullptr;
    uint32_t n = 0;
    SF6CoreOptButtonResult* out = nullptr;
};
bool call_seh(const CbCall* c, DWORD* exc) {
    __try {
        const SF6CoreOptCallbacks* cb = c->cb;
        switch (c->what) {
        case 1: if (cb->on_value) cb->on_value(cb->user, c->a, c->b); break;
        case 2: if (cb->on_button) cb->on_button(cb->user, c->a, c->out); break;
        case 3: if (cb->on_dialog_created) cb->on_dialog_created(cb->user, c->p); break;
        case 4: if (cb->on_page_setup) cb->on_page_setup(cb->user, c->p, c->n); break;
        case 5: if (cb->on_page_popped) cb->on_page_popped(cb->user, c->p); break;
        case 6: if (cb->on_window_closed) cb->on_window_closed(cb->user); break;
        default: break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc = GetExceptionCode();
        return false;
    }
    return true;
}
void call(SF6CoreClient* client, CbCall c) {
    Owner* o = owner_of(client);
    if (!o || !client_live(client)) return;
    const SF6CoreOptCallbacks cb = o->cb;                          // the call may change g_owners
    c.cb = &cb;
    DWORD exc = 0;
    if (!call_seh(&c, &exc)) handle_fault(client, exc, "opt_cb");
}
void call_all(const CbCall& c) {
    std::vector<SF6CoreClient*> cs;
    for (auto& o : g_owners) cs.push_back(o.client);
    for (auto* cl : cs) call(cl, c);
}

// ---- the driver's registry (the mod's options_driver.cpp, NativeOptions.lua :1180-1481) -------------------------
struct DEntry {
    int64_t eid = -1;
    SF6CoreClient* client = nullptr;
    std::string kind, key, title, desc;                // "button" | "slider" | "choice"
    std::vector<std::string> options;                  // choice
    bool has_min = false; int64_t min = 0, max = 0;    // slider
    int64_t value = 0;                                 // choice 1-based, slider raw, button 0
    int64_t last_int = 0;
    bool has_sig = false; std::string sig;
};
struct DGroup {
    SF6CoreClient* client = nullptr;
    bool has_gid = false; int64_t gid = -1;
    std::string key, title, desc;
    bool has_p = false; std::string pt, pd;
    std::vector<std::shared_ptr<DEntry>> entries;
};
struct Queued { std::shared_ptr<DEntry> e; bool nav = false; std::string key; int flags = 0; };

std::vector<std::shared_ptr<DGroup>> g_groups;
std::map<int64_t, std::shared_ptr<DEntry>> g_by_id;
std::vector<std::shared_ptr<DEntry>> g_dead;
std::vector<Queued> g_button_queue;
int64_t g_tick = 0;
bool g_window = false;
uint64_t g_faults = 0;
bool g_wanted = false, g_installed = false, g_install_failed = false;

template <class F> void guard(const char* where, F&& f) {
    try { f(); } catch (...) { ++g_faults; dialogs::options::note_fault(where); }
}

int64_t to_int(const DEntry& e) { return e.kind == "choice" ? e.value - 1 : e.value; }
int64_t from_int(const DEntry& e, int64_t n) { return e.kind == "choice" ? n + 1 : n; }

void sweep_dead() {
    for (auto& e : g_dead) g_by_id.erase(e->eid);
    g_dead.clear();
}

std::shared_ptr<DGroup> group_by_id(SF6CoreClient* c, int64_t gid) {
    for (auto& g : g_groups) if (g->has_gid && g->gid == gid && g->client == c) return g;
    return nullptr;
}

void remove_group(const std::shared_ptr<DGroup>& g) {
    for (auto it = g_groups.begin(); it != g_groups.end(); ++it) if (*it == g) { g_groups.erase(it); break; }
    if (g->has_gid) guard("opt_remove_group", [&] { oc::remove_group(g->gid); });
    for (auto& e : g->entries) if (e->eid >= 0) g_dead.push_back(e);
    if (!g_window) sweep_dead();
}

// ---- build ----
void sync_defs() {
    for (auto& g : g_groups) {
        if (g->has_gid && (!g->has_p || g->title != g->pt || g->desc != g->pd)) {
            guard("opt_group_def", [&] { oc::set_group_def(g->gid, g->title, g->desc); });
            g->has_p = true; g->pt = g->title; g->pd = g->desc;
        }
        for (auto& e : g->entries) {
            if (e->eid < 0) continue;
            std::string opts;
            int64_t v = 0;
            if (e->kind != "button") {
                if (e->kind != "slider") for (const auto& o : e->options) { opts += o; opts += US; }
                v = to_int(*e);
            }
            const int64_t mn = e->has_min ? e->min : 0, mx = e->has_min ? e->max : 0;
            const std::string sig = e->title + RS + e->desc + RS + opts + RS + std::to_string(mn) + RS + std::to_string(mx) + RS + std::to_string(v);
            if (e->has_sig && sig == e->sig) continue;
            guard("opt_entry_def", [&] {
                oc::set_entry_def(e->eid, e->title, e->desc, e->kind == "choice" ? e->options : std::vector<std::string>{}, (int32_t)mn, (int32_t)mx, v);
            });
            e->has_sig = true; e->sig = sig;
        }
    }
}
void prepare_build() {
    try { sync_defs(); } catch (...) {}
    guard("opt_purge_rows", [] { dialogs::options::set_purge_pause_rows(true); });
}
bool built() { bool b = false; guard("opt_built", [&] { b = oc::built(); }); return b; }
void native_build() { prepare_build(); guard("opt_build", [] { oc::build(); }); }
void ensure_built() { if (!built()) { guard("opt_rebuild", [] { oc::rebuild(); }); native_build(); } }

// ---- events ----
std::vector<std::string> split(const std::string& s, char sep, bool keep_last_empty) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) { if (c == sep) { out.push_back(cur); cur.clear(); } else cur += c; }
    if (keep_last_empty || !cur.empty()) out.push_back(cur);
    return out;
}
int64_t num(const std::string& s, int64_t dflt) {
    if (s.empty()) return dflt;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    return (end && *end == 0) ? (int64_t)v : dflt;
}
void pump_now() {
    std::string s;
    guard("opt_pull", [&] { s = oc::pull_events(); });
    if (s.empty()) return;
    for (const std::string& rec : split(s, RS, false)) {
        if (rec.empty()) continue;
        const auto f = split(rec, US, true);
        const std::string& t = f[0];
        if (t == "V") {
            auto it = g_by_id.find(num(f.size() > 1 ? f[1] : "", -1));
            const bool has_n = f.size() > 2 && !f[2].empty();
            if (it != g_by_id.end() && has_n) {
                auto e = it->second;
                const int64_t n = num(f[2], 0);
                e->last_int = n; e->value = from_int(*e, n);
                if (e->kind != "button") { CbCall c; c.what = 1; c.a = e->eid; c.b = e->value; call(e->client, c); }
            }
        } else if (t == "B") {
            auto it = g_by_id.find(num(f.size() > 1 ? f[1] : "", -1));
            if (it != g_by_id.end()) g_button_queue.push_back({ it->second });
        } else if (t == "N") {
            Queued q; q.nav = true; q.key = f.size() > 1 ? f[1] : ""; q.flags = (int)num(f.size() > 2 ? f[2] : "", 0);
            g_button_queue.push_back(q);
        } else if (t == "C") {
            const uint64_t param = (uint64_t)num(f.size() > 1 ? f[1] : "", 0);
            if (param) { CbCall c; c.what = 3; c.p = reinterpret_cast<void*>(param); call_all(c); }
        } else if (t == "S") {
            const uint64_t param = (uint64_t)num(f.size() > 1 ? f[1] : "", 0);
            if (!param) continue;
            std::map<SF6CoreClient*, uint32_t> rows;           // one row per entry, counted for its client
            std::map<int64_t, bool> seen;
            for (size_t i = 2; i + 1 < f.size(); i += 2) {
                const int64_t eid = num(f[i], -1);
                auto it = g_by_id.find(eid);
                if (it == g_by_id.end() || num(f[i + 1], 0) == 0 || seen[eid]) continue;
                seen[eid] = true;
                ++rows[it->second->client];
            }
            std::vector<SF6CoreClient*> cs;
            for (auto& o : g_owners) cs.push_back(o.client);
            for (auto* cl : cs) { CbCall c; c.what = 4; c.p = reinterpret_cast<void*>(param); c.n = rows[cl]; call(cl, c); }
        } else if (t == "P") {
            const uint64_t param = (uint64_t)num(f.size() > 1 ? f[1] : "", 0);
            CbCall c; c.what = 5; c.p = reinterpret_cast<void*>(param); call_all(c);
        } else if (t == "X") {
            g_window = false;
            sweep_dead();
            CbCall c; c.what = 6; call_all(c);
        }
    }
}

// ---- requests ----
void open_menu(const std::string& title, int focus) {
    if (g_window) return;
    guard("opt_request_open_menu", [&] { oc::request_open_menu(title, focus); });
}
void navigate(const std::string& key, bool rebuild_tree, int focus, bool now, bool push, bool pop) {
    if (!g_window) return;
    bool runs = false;
    guard("opt_navigate_now", [&] { runs = oc::navigate_runs_now(now); });
    if (runs && (rebuild_tree || !built())) prepare_build();
    guard("opt_navigate", [&] { oc::navigate(key, rebuild_tree, focus, now, push, pop); });
    pump_now();
}

// ---- the per-frame callback ----
void tick() {
    ++g_tick;
    pump_now();
    int code = 1;
    guard("opt_tick_a", [&] { code = oc::tick_a(g_tick, false, false); });
    if (code == 1) return;
    if (code == 2) { native_build(); return; }
    if (code == 3) {
        guard("opt_phase1", [] { oc::phase1(); });
        g_window = true;
    } else if (code == 4) {
        guard("opt_rebuild", [] { oc::rebuild(); });
        native_build();
    }
    pump_now();
    guard("opt_tick_a2", [] { oc::tick_a2(); });
    if (!g_button_queue.empty()) {
        std::vector<Queued> q;
        q.swap(g_button_queue);
        bool want_close = false, want_open = false, want_rebuild = false, want_push = false, want_pop = false;
        std::string open_key;
        for (const Queued& it : q) {
            if (it.nav) {
                want_open = true; open_key = it.key;
                if (it.flags & 1) want_rebuild = true;
                if (it.flags & 2) want_push = true;
                if (it.flags & 4) want_pop = true;
            } else if (it.e && it.e->kind == "button") {
                SF6CoreOptButtonResult r{};
                CbCall c; c.what = 2; c.a = it.e->eid; c.out = &r;
                call(it.e->client, c);
                r.key[sizeof r.key - 1] = 0;
                if (r.kind == 1) want_close = true;
                else if (r.kind == 2) {
                    want_open = true; open_key = r.key;
                    if (r.rebuild) want_rebuild = true;
                    if (r.push) want_push = true;
                    if (r.pop) want_pop = true;
                }
            }
        }
        if (want_close || want_open) {
            bool builds = false;
            guard("opt_button_builds", [&] { builds = oc::button_result_builds(want_close, want_open, want_rebuild, false); });
            if (builds) prepare_build();
            guard("opt_button_result", [&] { oc::button_result(want_close, want_open, open_key, want_rebuild, want_push, want_pop, false); });
            pump_now();
        }
    }
    guard("opt_tick_b", [] { oc::tick_b(); });
    pump_now();
}

bool usable() { return g_installed && dialogs::options::ready(); }
const char* s(const char* p) { return p ? p : ""; }

}  // namespace

// ---- called by sf6core.cpp ---------------------------------------------------------------------------------------
void options_frame() {
    if (g_wanted && !g_installed && !g_install_failed) {          // the first frame after a client asked
        SF6CoreClientDesc d{ sizeof(SF6CoreClientDesc), "sf6core.options", "1", 0 };
        SF6CoreClient* c = nullptr;
        if (sf6core_api_ptr()->register_client(&d, &c) == SF6CORE_OK) cmn::set_client(c);
        if (!dialogs::guid_text::install()) { g_install_failed = true; core_log(SF6CORE_LOG_WARN, nullptr, "[options] guid_text not installed: option service off"); }
        else if (!dialogs::options::install()) { g_install_failed = true; core_log(SF6CORE_LOG_WARN, nullptr, "[options] hooks not installed: option service off"); }
        else g_installed = true;
    }
    if (!usable()) return;
    try { tick(); } catch (...) { ++g_faults; }
}
void options_pump() {
    if (!usable() || core_closing()) return;
    try { pump_now(); } catch (...) { ++g_faults; }
}

// ---- the API (sf6core_api.h, increment 6) --------------------------------------------------------------------------
SF6CoreStatus SF6CORE_CALL api_opt_set_callbacks(SF6CoreClient* client, const SF6CoreOptCallbacks* cb) {
    if (!client || !cb) return SF6CORE_ERR_INVALID_ARG;
    if (g_install_failed) return SF6CORE_ERR_DISABLED;
    Owner* o = owner_of(client);
    if (!o) { g_owners.push_back({ client, {} }); o = &g_owners.back(); }
    o->cb = SF6CoreOptCallbacks{};
    std::memcpy(&o->cb, cb, cb->struct_size < sizeof(SF6CoreOptCallbacks) ? cb->struct_size : sizeof(SF6CoreOptCallbacks));
    o->cb.struct_size = sizeof(SF6CoreOptCallbacks);
    g_wanted = true;
    return SF6CORE_OK;
}
int64_t SF6CORE_CALL api_opt_group(SF6CoreClient* client, const char* key, const char* title, const char* desc) {
    if (!client) return -1;
    auto g = std::make_shared<DGroup>();
    g->client = client; g->title = s(title); g->desc = s(desc); g->key = s(key);
    g_groups.push_back(g);
    guard("opt_group", [&] { g->gid = oc::add_group(g->key); g->has_gid = true; });
    return g->has_gid ? g->gid : -1;
}
SF6CoreStatus SF6CORE_CALL api_opt_group_def(SF6CoreClient* client, int64_t group, const char* title, const char* desc) {
    auto g = group_by_id(client, group);
    if (!g) return SF6CORE_ERR_NOT_FOUND;
    g->title = s(title); g->desc = s(desc);
    return SF6CORE_OK;
}
SF6CoreStatus SF6CORE_CALL api_opt_remove_group(SF6CoreClient* client, int64_t group) {
    auto g = group_by_id(client, group);
    if (!g) return SF6CORE_ERR_NOT_FOUND;
    remove_group(g);
    return SF6CORE_OK;
}
int64_t SF6CORE_CALL api_opt_entry(SF6CoreClient* client, int64_t group, const SF6CoreOptEntryDesc* d) {
    auto g = group_by_id(client, group);
    if (!g || !d) return -1;
    auto e = std::make_shared<DEntry>();
    e->client = client; e->key = s(d->key); e->title = s(d->title); e->desc = s(d->desc);
    if (d->kind == SF6CORE_OPT_BUTTON) e->kind = "button";
    else if (d->kind == SF6CORE_OPT_SLIDER) { e->kind = "slider"; e->has_min = true; e->min = d->min_val; e->max = d->max_val; e->value = d->value; }
    else if (d->kind == SF6CORE_OPT_CHOICE) {
        e->kind = "choice";
        for (uint32_t i = 0; d->options && i < d->n_options; ++i) e->options.push_back(s(d->options[i]));
        e->value = d->value;
    } else return -1;
    g->entries.push_back(e);
    int64_t id = -1;
    guard("opt_entry", [&] { id = dialogs::options::add_entry(g->gid, e->kind, e->key); });
    if (id >= 0) { e->eid = id; g_by_id[id] = e; }
    return id;
}
SF6CoreStatus SF6CORE_CALL api_opt_rebuild(SF6CoreClient*) { guard("opt_rebuild", [] { oc::rebuild(); }); return SF6CORE_OK; }
SF6CoreStatus SF6CORE_CALL api_opt_ensure_built(SF6CoreClient*) { ensure_built(); return SF6CORE_OK; }
SF6CoreStatus SF6CORE_CALL api_opt_open_menu(SF6CoreClient*, const char* title, int32_t focus) {
    if (core_closing()) return SF6CORE_ERR_DISABLED;
    open_menu(s(title), focus >= 0 ? focus : -1);
    return SF6CORE_OK;
}
SF6CoreStatus SF6CORE_CALL api_opt_navigate(SF6CoreClient*, const char* key, int32_t flags, int32_t focus) {
    if (core_closing()) return SF6CORE_ERR_DISABLED;
    navigate(s(key), (flags & SF6CORE_OPT_NAV_REBUILD) != 0, focus >= 0 ? focus : -1, (flags & SF6CORE_OPT_NAV_NOW) != 0,
             (flags & SF6CORE_OPT_NAV_PUSH) != 0, (flags & SF6CORE_OPT_NAV_POP) != 0);
    return SF6CORE_OK;
}
SF6CoreStatus SF6CORE_CALL api_opt_close(SF6CoreClient*) { if (core_closing()) return SF6CORE_ERR_DISABLED; guard("opt_close", [] { oc::close(); }); return SF6CORE_OK; }
int32_t SF6CORE_CALL api_opt_window_open(void) { return oc::window_open() ? 1 : 0; }
void* SF6CORE_CALL api_opt_current_param(void) { return oc::current_param(); }
void* SF6CORE_CALL api_opt_parent_param(void) { return oc::parent_param(); }
int64_t SF6CORE_CALL api_opt_focused_entry(void) { return usable() ? dialogs::options::focused_entry() : -1; }
void* SF6CORE_CALL api_opt_row_control(int64_t entry, void* param) {
    if (core_closing()) return nullptr;
    return usable() ? reinterpret_cast<void*>(dialogs::options::row_control(entry, (uint64_t)reinterpret_cast<uintptr_t>(param))) : nullptr;
}
void* SF6CORE_CALL api_opt_row_part(int64_t entry) { if (core_closing()) return nullptr; return usable() ? reinterpret_cast<void*>(dialogs::options::row_part(entry)) : nullptr; }
SF6CoreStatus SF6CORE_CALL api_opt_set_option_text(SF6CoreClient*, int64_t entry, int32_t index, const char* text) {
    if (!usable() || core_closing()) return SF6CORE_ERR_NOT_READY;
    return dialogs::options::set_option_text(entry, index, s(text)) ? SF6CORE_OK : SF6CORE_ERR_FAULTED;
}
uint32_t SF6CORE_CALL api_opt_status(char* buf, uint32_t cap) {
    char b[512];
    const std::string eng = g_installed ? dialogs::options::status() : std::string("not installed");
    std::snprintf(b, sizeof b, "options: %s, clients %zu, tick %lld, groups %zu, entries %zu, window %d, faults %llu; %s",
                  g_installed ? "on" : (g_install_failed ? "failed" : "idle"), g_owners.size(), (long long)g_tick, g_groups.size(),
                  g_by_id.size(), g_window ? 1 : 0, (unsigned long long)g_faults, eng.c_str());
    const uint32_t n = (uint32_t)std::strlen(b);
    if (buf && cap) { const uint32_t k = n < cap - 1 ? n : cap - 1; std::memcpy(buf, b, k); buf[k] = 0; return k; }
    return n;
}
