// pm_service.cpp - increment 9: the training pause menu as a core service (ADR-0016, docs/PAUSE_MENU.md).
//
// The engine: pm_core (the state machine, no game header) and pausemenu (the game side: backend, hooks). This file
// is the service around it: the clients and their callbacks, the api_pm_* functions, the frame entry and the clean-up
// when a client unregisters.
//
// Callbacks run synchronously on the thread of the hook that needs them (the game's UI job threads, concurrently,
// R29): each call is SEH-guarded (no C++ object in the __try frame) and a fault counts on the client's breaker
// (ADR-0008); a client that is disabled by its breaker or unregistered is skipped (its spins then show the value the
// core kept, its buttons do nothing). Nothing is locked while a callback runs: it may call any pm_* function.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "../sf6core_internal.h"
#include "../perf.hpp"
#include "../options/guid_text.hpp"
#include "pausemenu.hpp"
#include "pm_core.hpp"

namespace {

namespace pc = pausemenu::core;
constexpr uint32_t kMaxHintsPerCall = 8;       // hints one client may add for one focused row
constexpr size_t kHintText = 128;

struct Owner { SF6CoreClient* client = nullptr; SF6CorePmCallbacks cb{}; };
std::mutex g_mx;                               // g_owners (never held while a callback runs)
std::vector<Owner> g_owners;
std::atomic<bool> g_wanted{false}, g_installed{false}, g_failed{false}, g_any_hints{false};
SF6CoreClient* g_self = nullptr;               // the engine's own client "sf6core.pausemenu"
std::mutex g_fault_mx;                         // breaker accounting from the job threads, one at a time
std::atomic<uint64_t> g_cb_faults{0}, g_decides{0}, g_spins{0};

bool client_live(const SF6CoreClient* c) { return c && c->active && c->state == SF6CORE_CLIENT_ACTIVE; }

// The callbacks of a live client (copied: the owners may change meanwhile).
bool callbacks_of(const void* owner, SF6CorePmCallbacks& out, SF6CoreClient*& client) {
    std::lock_guard lk(g_mx);
    for (const auto& o : g_owners) {
        if (o.client != owner) continue;
        if (!client_live(o.client)) return false;
        out = o.cb;
        client = o.client;
        return true;
    }
    return false;
}

// ---- one callback call under SEH ----
struct PmCall {
    int what = 0;                              // 1 decide, 2 spin_value, 3 on_spin, 4 page_shown, 5 hints
    const SF6CorePmCallbacks* cb = nullptr;
    int64_t id = 0;
    int32_t arg = 0;
    void* data = nullptr;
    int32_t ret = 0;
    uint32_t n = 0;
    SF6CorePmHint* hints = nullptr;            // kMaxHintsPerCall
    int32_t* buttons = nullptr;                // out: copied before the callback's memory can go
    char (*texts)[kHintText] = nullptr;
};
bool call_seh(PmCall* c, DWORD* exc) {
    __try {
        const SF6CorePmCallbacks* cb = c->cb;
        switch (c->what) {
        case 1: cb->on_decide(cb->user, c->id); break;
        case 2: c->ret = cb->spin_value(cb->user, c->id); break;
        case 3: cb->on_spin(cb->user, c->id, c->arg); break;
        case 4: cb->on_page_shown(cb->user, c->id, c->arg); break;
        case 5: {
            uint32_t n = cb->hints(cb->user, c->data, c->hints, kMaxHintsPerCall);
            if (n > kMaxHintsPerCall) n = kMaxHintsPerCall;
            for (uint32_t i = 0; i < n; ++i) {
                c->buttons[i] = c->hints[i].button;
                c->texts[i][0] = 0;
                if (c->hints[i].text) strncpy_s(c->texts[i], kHintText, c->hints[i].text, _TRUNCATE);
            }
            c->n = n;
            break;
        }
        default: break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exc = GetExceptionCode();
        return false;
    }
    return true;
}
bool run(SF6CoreClient* client, PmCall& c, const char* phase) {
    DWORD exc = 0;
    if (call_seh(&c, &exc)) return true;
    ++g_cb_faults;
    std::lock_guard lk(g_fault_mx);
    handle_fault(client, exc, phase);
    return false;
}

// ---- pausemenu::Calls ----
void c_decide(pausemenu::Owner owner, int64_t row) {
    SF6CorePmCallbacks cb; SF6CoreClient* cl = nullptr;
    if (!callbacks_of(owner, cb, cl) || !cb.on_decide) return;
    ++g_decides;
    PmCall c; c.what = 1; c.cb = &cb; c.id = row;
    run(cl, c, "pm_on_decide");
}
bool c_spin_value(pausemenu::Owner owner, int64_t row, int32_t& out) {
    SF6CorePmCallbacks cb; SF6CoreClient* cl = nullptr;
    if (!callbacks_of(owner, cb, cl) || !cb.spin_value) return false;
    PmCall c; c.what = 2; c.cb = &cb; c.id = row;
    if (!run(cl, c, "pm_spin_value")) return false;
    out = c.ret;
    return true;
}
void c_on_spin(pausemenu::Owner owner, int64_t row, int32_t index) {
    SF6CorePmCallbacks cb; SF6CoreClient* cl = nullptr;
    if (!callbacks_of(owner, cb, cl) || !cb.on_spin) return;
    ++g_spins;
    PmCall c; c.what = 3; c.cb = &cb; c.id = row; c.arg = index;
    run(cl, c, "pm_on_spin");
}
void c_page_shown(pausemenu::Owner owner, int64_t page, bool shown) {
    SF6CorePmCallbacks cb; SF6CoreClient* cl = nullptr;
    if (!callbacks_of(owner, cb, cl) || !cb.on_page_shown) return;
    PmCall c; c.what = 4; c.cb = &cb; c.id = page; c.arg = shown ? 1 : 0;
    run(cl, c, "pm_on_page_shown");
}
bool c_any_hints() { return g_any_hints.load(std::memory_order_relaxed); }
void c_hints(void* focused, std::vector<pausemenu::Hint>& out) {
    std::vector<Owner> owners;
    {
        std::lock_guard lk(g_mx);
        for (const auto& o : g_owners) if (o.cb.hints && client_live(o.client)) owners.push_back(o);
    }
    for (auto& o : owners) {                      // in the clients' registration order
        SF6CorePmHint hints[kMaxHintsPerCall] = {};
        int32_t buttons[kMaxHintsPerCall] = {};
        char texts[kMaxHintsPerCall][kHintText] = {};
        PmCall c; c.what = 5; c.cb = &o.cb; c.data = focused; c.hints = hints; c.buttons = buttons; c.texts = texts;
        if (!run(o.client, c, "pm_hints")) continue;
        for (uint32_t i = 0; i < c.n; ++i)
            if (texts[i][0]) out.push_back({ o.client, buttons[i], std::string(texts[i]) });
    }
}

const char* s(const char* p) { return p ? p : ""; }

void refresh_any_hints() {
    bool any = false;
    for (const auto& o : g_owners) if (o.cb.hints) { any = true; break; }
    g_any_hints = any;
}

}  // namespace

// ---- called by sf6core.cpp -----------------------------------------------------------------------------------------

// LateUpdateBehavior, after the option dialogs and before the clients' frame callbacks.
void pausemenu_frame() {
    if (g_wanted.load() && !g_installed.load() && !g_failed.load()) {     // the first frame after a client asked
        SF6CoreClientDesc d{ sizeof(SF6CoreClientDesc), "sf6core.pausemenu", "1", 0 };
        if (!g_self && sf6core_api_ptr()->register_client(&d, &g_self) != SF6CORE_OK) g_self = nullptr;
        pausemenu::Calls calls;
        calls.decide = c_decide; calls.spin_value = c_spin_value; calls.on_spin = c_on_spin;
        calls.page_shown = c_page_shown; calls.hints = c_hints; calls.any_hints = c_any_hints;
        if (!g_self) { g_failed = true; core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] engine client not registered: service off"); }
        else if (!dialogs::guid_text::install()) { g_failed = true; core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] GetMessage hook not installed: service off"); }
        else if (!pausemenu::install(calls, g_self)) { g_failed = true; core_log(SF6CORE_LOG_WARN, nullptr, "[pausemenu] engine not installed: service off"); }
        else g_installed = true;
    }
    if (!g_installed.load() || core_closing()) return;
    pausemenu::frame(core_frame_pub());
}

// unregister_client, once the client is inactive: its callbacks gone, its pages and rows out of the menu at the
// next pass (their texts released then).
void pausemenu_release_client(SF6CoreClient* c) {
    {
        std::lock_guard lk(g_mx);
        for (auto it = g_owners.begin(); it != g_owners.end(); ++it)
            if (it->client == c) { g_owners.erase(it); break; }
        refresh_any_hints();
    }
    std::vector<pc::Edge> edges;
    const size_t n = pc::remove_owner(c, &edges);   // no callback: the client is gone
    if (n) core_log(SF6CORE_LOG_INFO, c->client_id, "[pausemenu] %zu page(s) / row(s) removed (unregistered)", n);
}

// ---- the API (sf6core_api.h, increment 9) --------------------------------------------------------------------------

SF6CoreStatus SF6CORE_CALL api_pm_set_callbacks(SF6CoreClient* client, const SF6CorePmCallbacks* cb) {
    if (!client || !cb || cb->struct_size < sizeof(uint32_t) + sizeof(void*)) return SF6CORE_ERR_INVALID_ARG;
    if (g_failed.load()) return SF6CORE_ERR_DISABLED;
    if (!client->active) return SF6CORE_ERR_DISABLED;
    {
        std::lock_guard lk(g_mx);
        Owner* o = nullptr;
        for (auto& x : g_owners) if (x.client == client) { o = &x; break; }
        if (!o) { g_owners.push_back({ client, {} }); o = &g_owners.back(); }
        o->cb = SF6CorePmCallbacks{};
        std::memcpy(&o->cb, cb, cb->struct_size < sizeof(SF6CorePmCallbacks) ? cb->struct_size : sizeof(SF6CorePmCallbacks));
        o->cb.struct_size = sizeof(SF6CorePmCallbacks);
        refresh_any_hints();
    }
    if (!g_wanted.exchange(true))
        core_log(SF6CORE_LOG_INFO, client->client_id, "[pausemenu] callbacks set: the engine is installed at the next frame");
    return SF6CORE_OK;
}

int64_t SF6CORE_CALL api_pm_page(SF6CoreClient* client, const SF6CorePmPageDesc* desc) {
    if (!client || !desc || desc->struct_size < sizeof(SF6CorePmPageDesc)) return -1;
    const int64_t id = pc::add_page(client, desc->primary_func, desc->strip_index, s(desc->title), s(desc->guide));
    if (id < 0) core_log(SF6CORE_LOG_WARN, client->client_id, "[pausemenu] pm_page refused: %s", pc::last_error().c_str());
    return id;
}

int64_t SF6CORE_CALL api_pm_row(SF6CoreClient* client, int64_t page, const SF6CorePmRowDesc* desc) {
    if (!client || !desc || desc->struct_size < sizeof(SF6CorePmRowDesc)) return -1;
    if (desc->kind != SF6CORE_PM_BUTTON && desc->kind != SF6CORE_PM_SPIN) {
        core_log(SF6CORE_LOG_WARN, client->client_id, "[pausemenu] pm_row refused: unknown kind %d", desc->kind);
        return -1;
    }
    const int64_t id = pc::add_row(client, page, desc->kind == SF6CORE_PM_SPIN ? pc::Kind::Spin : pc::Kind::Button,
                                   s(desc->title), s(desc->guide), desc->capacity, desc->keep_open != 0, desc->arrow_spread);
    if (id < 0) core_log(SF6CORE_LOG_WARN, client->client_id, "[pausemenu] pm_row refused: %s", pc::last_error().c_str());
    return id;
}

SF6CoreStatus SF6CORE_CALL api_pm_set_texts(SF6CoreClient* client, int64_t page_or_row, const char* title, const char* guide) {
    if (!client) return SF6CORE_ERR_INVALID_ARG;
    const std::string t = s(title), g = s(guide);
    return pc::set_texts(client, page_or_row, title ? &t : nullptr, guide ? &g : nullptr) == pc::kOk ? SF6CORE_OK
                                                                                                  : SF6CORE_ERR_NOT_FOUND;
}

SF6CoreStatus SF6CORE_CALL api_pm_set_options(SF6CoreClient* client, int64_t row, const char* const* labels, uint32_t n) {
    if (!client || (n && !labels)) return SF6CORE_ERR_INVALID_ARG;
    std::vector<std::string> v;
    v.reserve(n);
    for (uint32_t i = 0; i < n; ++i) v.emplace_back(s(labels[i]));
    const pc::Result r = pc::set_options(client, row, v, core_frame_pub());
    if (r == pc::kNotFound) return SF6CORE_ERR_NOT_FOUND;
    return SF6CORE_OK;                         // kTruncated: the labels past the capacity are dropped (documented)
}

SF6CoreStatus SF6CORE_CALL api_pm_remove(SF6CoreClient* client, int64_t page_or_row) {
    if (!client) return SF6CORE_ERR_INVALID_ARG;
    std::vector<pc::Edge> edges;
    if (pc::remove(client, page_or_row, &edges) != pc::kOk) return SF6CORE_ERR_NOT_FOUND;
    for (const auto& e : edges) c_page_shown(e.owner, e.page, e.shown);
    return SF6CORE_OK;
}

int32_t SF6CORE_CALL api_pm_page_shown(int64_t page) {
    return g_installed.load() ? pausemenu::page_shown(page) : 0;
}

void* SF6CORE_CALL api_pm_row_part(int64_t row) {
    return g_installed.load() ? pausemenu::row_part(row) : nullptr;
}

int64_t SF6CORE_CALL api_pm_row_of(SF6CoreClient* client, void* menu_data) {
    if (!client || !menu_data || !g_installed.load()) return -1;
    return pausemenu::row_of(client, menu_data);
}

uint32_t SF6CORE_CALL api_pm_status(char* buf, uint32_t cap) {
    std::string line;
    try {
        size_t clients = 0;
        { std::lock_guard lk(g_mx); clients = g_owners.size(); }
        char b[256];
        std::snprintf(b, sizeof b, "pausemenu: %s, clients %zu, callbacks: decides %llu, spin changes %llu, faults %llu; ",
                      g_installed ? "on" : (g_failed ? "failed" : (g_wanted ? "pending" : "idle")), clients,
                      (unsigned long long)g_decides.load(), (unsigned long long)g_spins.load(),
                      (unsigned long long)g_cb_faults.load());
        line = b;
        line += g_installed ? pausemenu::status() : pc::status();
    } catch (...) {
        line = "pausemenu: status failed";
    }
    const uint32_t n = (uint32_t)line.size();
    if (buf && cap) {
        const uint32_t k = n < cap - 1 ? n : cap - 1;
        std::memcpy(buf, line.data(), k);
        buf[k] = 0;
        return k;
    }
    return n;
}
