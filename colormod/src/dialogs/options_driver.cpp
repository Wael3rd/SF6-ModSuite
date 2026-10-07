// dialogs::options::driver — see options_driver.hpp. The relay of SF6_ColorSpinExtra.lua (N) at tag phase5c2-ref,
// over the core's option service; the driver it used to sit on is the core's (options/options_service.cpp).

#include "options_driver.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "../common.hpp"
#include "editor.hpp"

namespace dialogs::options::driver {
namespace {

constexpr char US = '\x1f', RS = '\x1e', GS = '\x1d';

bool g_installed = false;
std::map<int, int64_t> g_relay;                    // the relay's N.groups: the editor's group id -> the core's
std::map<int, std::vector<int64_t>> g_group_rows;  // the editor's group id -> the core entries made for it
std::map<int64_t, int> g_editor_id;                // the core's entry id -> the editor's id (N.button / N.value)
std::vector<int64_t> g_dead;                       // entries of removed groups, forgotten once the window is closed

const SF6CoreAPI* api() {
    const SF6CoreAPI* a = cmn::core();
    return (a && SF6CORE_HAS(a, opt_status)) ? a : nullptr;
}
SF6CoreClient* cl() { return cmn::client(); }

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
bool tri(const std::string& s) { return s == "1"; }            // N.tri, as a boolean argument (nil -> false)

void sweep_dead() {
    for (int64_t eid : g_dead) g_editor_id.erase(eid);
    g_dead.clear();
}

// ---- the core's callbacks (the relay's N.button / N.value / ed_created / ed_page_setup / ed_popped) ----
void SF6CORE_CALL cb_value(void*, int64_t entry, int64_t value) {
    auto it = g_editor_id.find(entry);
    if (it == g_editor_id.end()) return;
    dialogs::editor::on_value(it->second, (int)value);
    apply();
}
void SF6CORE_CALL cb_button(void*, int64_t entry, SF6CoreOptButtonResult* out) {
    auto it = g_editor_id.find(entry);
    if (it == g_editor_id.end() || !out) return;
    const auto r = dialogs::editor::on_button(it->second);
    apply();
    out->kind = r.kind;
    out->rebuild = r.rebuild == 1; out->push = r.push == 1; out->pop = r.pop == 1;
    std::snprintf(out->key, sizeof out->key, "%s", r.key.c_str());
}
void SF6CORE_CALL cb_created(void*, void* param) {
    if (param && dialogs::editor::on_created((uint64_t)reinterpret_cast<uintptr_t>(param)) > 0) apply();
}
void SF6CORE_CALL cb_page(void*, void* param, uint32_t rows) {
    if (param && dialogs::editor::on_page_setup((uint64_t)reinterpret_cast<uintptr_t>(param), (int)rows) > 0) apply();
}
void SF6CORE_CALL cb_popped(void*, void* param) {
    if (dialogs::editor::on_popped((uint64_t)reinterpret_cast<uintptr_t>(param)) > 0) apply();
}
void SF6CORE_CALL cb_closed(void*) { sweep_dead(); }

}  // namespace

bool install() {
    const SF6CoreAPI* a = api();
    if (!a || !cl()) { cmn::log_warn("[ColorModNative] the core has no option service (sf6core < 0.6.0): option dialogs off"); return false; }
    SF6CoreOptCallbacks cb{};
    cb.struct_size = sizeof cb;
    cb.on_value = cb_value; cb.on_button = cb_button;
    cb.on_dialog_created = cb_created; cb.on_page_setup = cb_page; cb.on_page_popped = cb_popped;
    cb.on_window_closed = cb_closed;
    g_installed = a->opt_set_callbacks(cl(), &cb) == SF6CORE_OK;
    if (!g_installed) cmn::log_warn("[ColorModNative] the core's option service refused us: option dialogs off");
    return g_installed;
}

bool ready() {
    if (!g_installed || !api()) return false;
    char b[256];
    api()->opt_status(b, sizeof b);
    return std::strncmp(b, "options: failed", 15) != 0;
}

// ---- the relay: the editor's orders (SF6_ColorSpinExtra.lua N.apply) ----------------------------------------------
void apply() {
    const SF6CoreAPI* a = api();
    std::string s = dialogs::editor::take_ops();
    if (s.empty() || !a) return;
    for (const std::string& rec : split(s, RS, false)) {
        const auto f = split(rec, US, true);
        auto at = [&](size_t i) -> std::string { return i < f.size() ? f[i] : std::string(); };
        const std::string& t = f.empty() ? std::string() : f[0];
        try {
            if (t == "G") {
                const int gid = (int)num(at(1), -1);
                const int64_t cg = a->opt_group(cl(), at(2).c_str(), at(3).c_str(), at(4).c_str());
                if (cg >= 0) g_relay[gid] = cg;
            } else if (t == "R") {
                auto it = g_relay.find((int)num(at(1), -1));
                if (it != g_relay.end()) {
                    a->opt_remove_group(cl(), it->second);
                    for (int64_t eid : g_group_rows[it->first]) g_dead.push_back(eid);
                    g_group_rows.erase(it->first);
                    g_relay.erase(it);
                    if (!a->opt_window_open()) sweep_dead();
                }
            } else if (t == "B" || t == "S" || t == "C") {
                const int id = (int)num(at(1), -1);
                const int egid = (int)num(at(2), -1);
                auto git = g_relay.find(egid);
                if (git == g_relay.end()) continue;
                const std::string key = at(3), title = at(4), desc = at(5);
                SF6CoreOptEntryDesc d{};
                d.struct_size = sizeof d;
                d.key = key.c_str(); d.title = title.c_str(); d.desc = desc.c_str();
                std::vector<std::string> opts;
                std::vector<const char*> optp;
                if (t == "B") d.kind = SF6CORE_OPT_BUTTON;
                else if (t == "S") {
                    d.kind = SF6CORE_OPT_SLIDER;
                    d.min_val = num(at(6), 0); d.max_val = num(at(7), 0);
                    d.value = f.size() > 8 && !at(8).empty() ? num(at(8), d.min_val) : d.min_val;      // default or min
                } else {
                    d.kind = SF6CORE_OPT_CHOICE;
                    opts = split(at(7), GS, true);                                              // ((f[8] or "") .. "\29"):gmatch("(.-)\29")
                    for (const auto& o : opts) optp.push_back(o.c_str());
                    d.options = optp.data(); d.n_options = (uint32_t)optp.size();
                    d.value = !at(6).empty() ? num(at(6), 1) : 1;                               // default or 1
                }
                const int64_t eid = a->opt_entry(cl(), git->second, &d);
                if (eid >= 0) {
                    g_editor_id[eid] = id;
                    g_group_rows[egid].push_back(eid);
                    dialogs::editor::map_entry(id, eid);
                }
            } else if (t == "D") {
                auto it = g_relay.find((int)num(at(1), -1));
                if (it != g_relay.end()) a->opt_group_def(cl(), it->second, at(2).c_str(), at(3).c_str());
            } else if (t == "RB") a->opt_rebuild(cl());
            else if (t == "EB") a->opt_ensure_built(cl());
            else if (t == "OM") { const int64_t fo = num(at(2), -1); a->opt_open_menu(cl(), at(1).c_str(), fo >= 0 ? (int32_t)fo : -1); }
            else if (t == "NV") {
                const int64_t fo = num(at(3), -1);
                int32_t flags = 0;
                if (tri(at(2))) flags |= SF6CORE_OPT_NAV_REBUILD;
                if (tri(at(4))) flags |= SF6CORE_OPT_NAV_NOW;
                if (tri(at(5))) flags |= SF6CORE_OPT_NAV_PUSH;
                if (tri(at(6))) flags |= SF6CORE_OPT_NAV_POP;
                a->opt_navigate(cl(), at(1).c_str(), flags, fo >= 0 ? (int32_t)fo : -1);
            }
            else if (t == "CL") a->opt_close(cl());
            else if (t == "TD") { g_relay.clear(); g_group_rows.clear(); }   // the DLL gave back what its triggers blocked
        } catch (...) {
            cmn::journal("editor relay: order %s failed", t.c_str());
        }
    }
}

bool window_open() { const SF6CoreAPI* a = api(); return a && a->opt_window_open() != 0; }
void* current_param() { const SF6CoreAPI* a = api(); return a ? a->opt_current_param() : nullptr; }
void* parent_param() { const SF6CoreAPI* a = api(); return a ? a->opt_parent_param() : nullptr; }
int64_t focused_entry() { const SF6CoreAPI* a = api(); return a ? a->opt_focused_entry() : -1; }
void* row_control(int64_t entry, void* param) { const SF6CoreAPI* a = api(); return a ? a->opt_row_control(entry, param) : nullptr; }

std::string status() {
    const SF6CoreAPI* a = api();
    char b[600] = "options: the core has no option service";
    if (a) a->opt_status(b, sizeof b);
    char out[700];
    std::snprintf(out, sizeof out, "%s; relay groups %zu, rows %zu", b, g_relay.size(), g_editor_id.size());
    return out;
}

}  // namespace dialogs::options::driver
