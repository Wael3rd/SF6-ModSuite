// SF6 Slots — runtime costume options: the menu (rto.hpp, docs/RUNTIME_OPTIONS.md).
//
// Battle Settings > character (Fighter Settings, agent MatchingFighterSetting): when the outfit on the
// preview has options, the bottom input guide bar gets an "F  Costume options" entry while the costume row
// is focused; that key on the costume row opens the game's option window through the core's option service
// (the Color Mod's colour editor opens the same way from the Color row; its keys are F on the Color row, X,
// and C / V inside its editor). Pages of at most 7 rows (the dialog's pool): the player side, one choice
// row per pick group (None + its options, at most 15 labels a row: R9), one On / Off row per toggle,
// More options, Restore defaults. A change is saved at once and shown on the preview by rto.cpp.
// (No preset row: the mod's former variants only serve the migration of a Battle Settings choice, rto.cpp.)
#include "rto.hpp"
#include "slots_ui.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace slots::rto::menu {
namespace {

constexpr int kRowsPerPage = 7;
constexpr float kDockX = -430.0f;                   // the dialog docked left: the preview stays visible

struct Act { enum Kind { Side, Option, More, Restore } kind = Option; int page = 0, pos = 0, row = -1; };

struct State {
    bool active = false;
    uint32_t opened_at = 0, closed_at = 0, refresh_at = 0;
    const Slot* slot = nullptr;
    int side = 0, page = 0;
    std::vector<Row> rows;
    std::vector<std::string> chosen;
    std::vector<std::vector<int>> pages;            // per page: indexes into rows
    std::vector<int64_t> groups;
    std::map<int64_t, Act> acts;                    // every entry made while the menu is open (a parent page keeps old ids)
    std::set<int> stale;                            // pages whose values changed since they were built
    std::map<int, int> focus;                       // per page: the row to land on when it is rebuilt
    int rebuild_page = -1, rebuild_pos = 0;         // asked by a callback, done on the next frame
};
State S;
bool g_svc = false;
int g_last_side = 0;

// ---- the trigger and the hint ----
struct Hint {
    Obj* agent = nullptr;
    Obj* text = nullptr;                            // the costume row's text (read only: tells when the outfit changed)
    Obj* gtext = nullptr;                           // the input guide bar's text (see guide_apply)
    uint32_t found_at = 0, gfound_at = 0;
    std::string base;                               // the costume row's text
    uint32_t base_at = 0;                           // when the game last wrote it (the outfit changed)
    bool pad = false;
    bool key_was = false;
    bool raw_was = false;                           // F / A down, any row (log of every press)
    int text_log = -1, has_log = -1, guide_log = -1;// last logged: costume row text found, outfit with options, guide text found
    std::vector<uint32_t> gwrites;                  // frames of our guide writes (log of a fight)
    bool fight_logged = false;
} H;

uint32_t g_pad_a = 32;
bool g_pad_bits = false;

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }
std::string page_key(int p) { return "__slots_rto_" + std::to_string(p); }

std::string upper(std::string s) { for (auto& c : s) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A'); return s; }

uint32_t pad_mask() {
    static Method* m_dev = nullptr;
    static bool tried = false;
    void* gp = api().get_native_singleton("via.hid.GamePad");
    if (!gp) return 0;
    if (!tried) {
        tried = true;
        if (auto* td = api().tdb()->find_type("via.hid.GamePad")) m_dev = method_of(td, "get_ConnectingDevices", 0);
    }
    if (!g_pad_bits) {
        g_pad_bits = true;
        if (auto* td = api().tdb()->find_type("via.hid.GamePadButton"))
            if (auto* f = td->find_field("RDown")) g_pad_a = static_cast<uint32_t>(f->get_data<int32_t>(nullptr));
    }
    Obj* devs = m_dev ? call_obj(m_dev, reinterpret_cast<Obj*>(gp)) : nullptr;
    uint32_t mask = 0;
    for (int i = 0, n = list_count(devs); i < n; ++i)
        if (Obj* pad = list_item(devs, i)) { auto r = call(pad, "get_Button"); if (!r.exception_thrown) mask |= r.dword; }
    return mask;
}
bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
bool game_has_focus() {
    DWORD pid = 0;
    HWND w = GetForegroundWindow();
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

// ---- pages ----
void layout_pages() {
    S.pages.clear();
    const int head = 2;                                                     // side, restore
    const int total = static_cast<int>(S.rows.size());
    int i = 0, p = 0;
    do {
        const int room = kRowsPerPage - (p == 0 ? head : 0);
        const bool last = total - i <= room;
        const int take = last ? total - i : room - 1;                       // - "More options"
        std::vector<int> pg;
        for (int k = 0; k < take; ++k) pg.push_back(i + k);
        i += take;
        S.pages.push_back(pg);
        ++p;
    } while (i < total && p < 16);
}

int64_t add_entry(int64_t g, int kind, const std::string& key, const std::string& title, const std::string& desc,
                  const std::vector<std::string>& labels, int value) {
    std::vector<const char*> lp;
    for (auto& l : labels) lp.push_back(l.c_str());
    SF6CoreOptEntryDesc d{};
    d.struct_size = sizeof d;
    d.kind = kind;
    d.key = key.c_str(); d.title = title.c_str(); d.desc = desc.c_str();
    d.options = lp.empty() ? nullptr : lp.data();
    d.n_options = static_cast<uint32_t>(lp.size());
    d.value = value;
    return A()->opt_entry(C(), g, &d);
}

void build() {
    for (int64_t g : S.groups) A()->opt_remove_group(C(), g);
    S.groups.clear();
    S.rows = rows(manifest(), *S.slot, S.chosen);
    layout_pages();
    const std::string who = S.side == 1 ? "P2" : "P1";
    const std::string title = upper(S.slot->label.empty() ? S.slot->name : S.slot->label) + "  -  " + who;
    const std::string live = "The preview shows it at once; saved for " + who + ".";
    for (int p = 0; p < static_cast<int>(S.pages.size()); ++p) {
        const int64_t g = A()->opt_group(C(), page_key(p).c_str(), title.c_str(), "Costume options");
        S.groups.push_back(g);
        if (g < 0) continue;
        int pos = 0;
        auto put = [&](int64_t e, Act a) { a.page = p; a.pos = pos++; if (e >= 0) S.acts[e] = a; };
        if (p == 0) {
            put(add_entry(g, SF6CORE_OPT_CHOICE, "side", "Player side",
                          "Whose options this menu shows and changes: the fighter on that side in Training and Versus. "
                          "Online, your own fighter wears P1's options.", { "P1", "P2" }, S.side + 1), { Act::Side });
        }
        for (int ri : S.pages[static_cast<size_t>(p)]) {
            const Row& r = S.rows[static_cast<size_t>(ri)];
            std::string desc = r.kind == Row::Pick ? "One option of this group at a time (None: the outfit without it). " + live
                                                   : "On or off. " + live;
            if (r.partial) desc += " * Some files of this option (colours, cloth, physics) only change at the next launch, or not at all.";
            Act a; a.kind = Act::Option; a.row = ri;
            put(add_entry(g, SF6CORE_OPT_CHOICE, "row" + std::to_string(ri), r.title, desc, r.labels, r.value), a);
        }
        if (p + 1 < static_cast<int>(S.pages.size()))
            put(add_entry(g, SF6CORE_OPT_BUTTON, "more", "More options", "The next options of this outfit.", {}, 0), { Act::More });
        if (p == 0)
            put(add_entry(g, SF6CORE_OPT_BUTTON, "restore", "Restore defaults",
                          "Back to the options the outfit was installed with (Fluffy's choice or the loader's), for " + who + ".", {}, 0),
                { Act::Restore });
    }
}

// From a callback: on the next frame (the core is in its value / button pass now)
void rebuild_later(int page, int pos) { S.rebuild_page = page; S.rebuild_pos = pos; }

void rebuild(int page, int pos) {
    build();
    for (int p = 0; p < static_cast<int>(S.pages.size()); ++p) if (p != page) S.stale.insert(p);
    S.stale.erase(page);
    S.focus[page] = pos;
    A()->opt_navigate(C(), page_key(page).c_str(), SF6CORE_OPT_NAV_REBUILD, pos);
}

void commit(const char* why) {
    auto& sd = choices().mods[S.slot->mod][static_cast<size_t>(S.side)];
    sd.changed = true;
    sd.ids = S.chosen;
    sd.names.clear();
    save_choices();
    choices_changed();
    logf("rto: menu %s, %s: %s (%zu options)", S.side ? "P2" : "P1", S.slot->mod.c_str(), why, S.chosen.size());
}

void teardown(const char* why) {
    if (!S.active) return;
    for (int64_t g : S.groups) A()->opt_remove_group(C(), g);
    A()->opt_rebuild(C());
    A()->opt_ensure_built(C());
    g_last_side = S.side;
    S = State{};
    S.closed_at = g_frame;
    choices_changed();                              // the preview goes back to P1's options
    logf("rto: menu closed (%s)", why);
}

void open() {
    const Slot* slot = preview_slot();
    if (!slot || !g_svc) { logf("rto: menu not opened (%s)", !slot ? "no outfit with options on the preview" : "no option service"); return; }
    S = State{};
    S.slot = slot;
    S.side = g_last_side;
    S.chosen = chosen_of(manifest(), *slot, choices(), S.side);
    build();
    A()->opt_rebuild(C());
    A()->opt_ensure_built(C());
    A()->opt_open_menu(C(), page_key(0).c_str(), -1);
    S.active = true;
    S.opened_at = g_frame;
    logf("rto: menu opened on %s/%s (%s), %zu rows on %zu pages", slot->fighter_dir.c_str(), slot->folder.c_str(),
         slot->label.c_str(), S.rows.size(), S.pages.size());
}

// ---- the core's callbacks (game thread, guarded by the core) ----
void SF6CORE_CALL cb_value(void*, int64_t entry, int64_t value) {
    if (!S.active) return;
    auto it = S.acts.find(entry);
    if (it == S.acts.end()) return;
    const Act a = it->second;
    try {
        if (a.kind == Act::Side) {
            const int side = value == 2 ? 1 : 0;
            if (side == S.side) return;
            S.side = side;
            S.chosen = chosen_of(manifest(), *S.slot, choices(), side);
            choices_changed();
            rebuild_later(a.page, a.pos);
        } else if (a.kind == Act::Option && a.row >= 0 && a.row < static_cast<int>(S.rows.size())) {
            const Row r = S.rows[static_cast<size_t>(a.row)];
            apply_row(manifest(), *S.slot, S.chosen, r, static_cast<int>(value));
            commit((r.title + " = " + (value >= 1 && value <= static_cast<int64_t>(r.labels.size()) ? r.labels[static_cast<size_t>(value - 1)] : "?")).c_str());
            // other rows changed (an option turned off), or this one did not take the value: rebuilt
            const auto now = rows(manifest(), *S.slot, S.chosen);
            bool same = now.size() == S.rows.size();
            for (size_t k = 0; same && k < now.size(); ++k)
                if (static_cast<int>(k) == a.row ? now[k].value != value : now[k].value != S.rows[k].value) same = false;
            if (!same) rebuild_later(a.page, a.pos);
            else {
                S.rows = now;
                for (int p = 0; p < static_cast<int>(S.pages.size()); ++p) if (p != a.page) S.stale.insert(p);
            }
        }
    } catch (...) {
        logf("rto: EXCEPTION in the menu (value)");
    }
}

void SF6CORE_CALL cb_button(void*, int64_t entry, SF6CoreOptButtonResult* out) {
    if (!S.active || !out) return;
    auto it = S.acts.find(entry);
    if (it == S.acts.end()) return;
    const Act a = it->second;
    try {
        if (a.kind == Act::More) {
            S.focus[a.page] = a.pos;
            S.page = a.page + 1;
            out->kind = 2; out->rebuild = 1; out->push = 1;
            std::snprintf(out->key, sizeof out->key, "%s", page_key(S.page).c_str());
            S.stale.erase(S.page);
        } else if (a.kind == Act::Restore) {
            choices().mods[S.slot->mod][static_cast<size_t>(S.side)] = Side{};
            S.chosen = chosen_of(manifest(), *S.slot, choices(), S.side);
            save_choices();
            choices_changed();
            logf("rto: menu %s, %s: defaults restored", S.side ? "P2" : "P1", S.slot->mod.c_str());
            rebuild_later(a.page, a.pos);
        }
    } catch (...) {
        logf("rto: EXCEPTION in the menu (button)");
    }
}

void dock(Obj* param) {
    Obj* agent = param ? get_obj(param, "<Agent>k__BackingField") : nullptr;
    Obj* cm = agent ? call_obj(agent, "get_ControlMain") : nullptr;
    if (!cm) return;
    auto r = call(cm, "get_Position");
    float x = 0;
    if (!r.exception_thrown) std::memcpy(&x, r.bytes.data(), 4);
    if (r.exception_thrown || std::fabs(x - kDockX) > 0.5f) A()->ui_set_position(C(), cm, kDockX, 0.0f, 0.0f);
}
void SF6CORE_CALL cb_created(void*, void* param) {
    if (!S.active) return;
    try { dock(reinterpret_cast<Obj*>(param)); } catch (...) {}
}
void SF6CORE_CALL cb_popped(void*, void*) {
    if (!S.active) return;
    if (S.page > 0) --S.page;
    if (S.stale.count(S.page)) S.refresh_at = g_frame + 15;    // its values changed on the page above
}
void SF6CORE_CALL cb_closed(void*) {
    if (S.active) teardown("window closed");
}

// ---- Fighter Settings: the guide bar entry and the key ----
//
// The entry goes into the bottom input guide bar, as the Color Mod does for its Color row ("<ICON KeyF><ICON
// MouseL> Edit Color"). Both DLLs write the same text object (the bar's e_text_0_g_*, else the first
// e_text_0_d_*, found the same way), so the scheme avoids a fight:
//  - The Color Mod strips everything from the first "      <ICON" (SIX spaces) of the game's text and appends
//    its own entry after it, and only on its Color row; elsewhere it writes the text back unchanged. Our entry
//    uses FOUR spaces ("    <ICON KeyF> label"): the Color Mod's strip does not match it, so it keeps ours
//    as "game text" (its stored copy then includes ours: no rewrite, no flicker), and when its own entry is
//    added it lands after ours.
//  - We strip only our exact entry (either icon), wherever it is in the text, and add it only on the costume
//    row of an outfit with options. On the Color row, or any other row, ours is removed and the rest of the
//    text (the game's, and the Color Mod's entry if it is there) is left as it is.
//  - We never write an identical text, and rebuild from what is in the text each frame, so a text the game or
//    the Color Mod rewrote is handled as it comes. The Color Mod refreshes its stored copy only every 5 frames
//    (and writes it each frame), so for a few frames after a change its stale copy can rewrite the text and we
//    write ours again; the screen is drawn after both. A fight of more than 60 writes in 120 frames is logged once.
const char* kEntryLabel = "Costume options";        // English only: the plugin has no locale system
std::string entry_text(bool pad) { return std::string("    ") + (pad ? "<ICON BtnD> " : "<ICON KeyF> ") + kEntryLabel; }
void erase_all(std::string& s, const std::string& what) {
    for (size_t at; (at = s.find(what)) != std::string::npos;) s.erase(at, what.size());
}

Obj* find_guide_text(const SF6CoreAPI* a) {
    void* buf[4] = {};
    const uint32_t n = a->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* ag = reinterpret_cast<Obj*>(buf[i]);
        Obj* cm = call_obj(ag, "get_ControlMain");
        bool ok = false;
        const bool vis = cm && call_bool(cm, "get_ActualVisible", {}, &ok);
        if (!(ok && vis)) continue;
        Obj* lst = get_obj(get_obj(ag, "_PartsManager"), "_List");
        for (int k = 0, cnt = list_count(lst); k < cnt; ++k) {
            Obj* part = list_item(lst, k);
            auto* td = part ? part->get_type_definition() : nullptr;
            if (!(td && std::string(td->get_full_name()) == "app.UIPartsInputGuideList")) continue;
            Obj* ctrl = get_obj(part, "<Control>k__BackingField");
            Obj* btn = ctrl ? reinterpret_cast<Obj*>(a->ui_find_child(ctrl, "c_guideBtn", 8)) : nullptr;
            Obj* first_d = nullptr;
            for (Obj* c = btn ? call_obj(btn, "get_Child") : nullptr; c; c = call_obj(c, "get_Next")) {
                const std::string nm = str(call_obj(c, "get_Name"));
                if (nm.rfind("e_text_0_g_", 0) == 0) return c;
                if (nm.rfind("e_text_0_d_", 0) == 0 && !first_d) first_d = c;
            }
            if (first_d) return first_d;
            break;                                  // the guide list of this bar has no text: next bar
        }
    }
    return nullptr;
}

// show: the entry belongs in the bar now; false: take ours out (the bar keeps the rest as it is)
void guide_apply(bool show) {
    const SF6CoreAPI* a = A();
    if (!a) return;
    if (show && (!alive(H.gtext) || g_frame - H.gfound_at > 60)) {
        H.gfound_at = g_frame;
        H.gtext = find_guide_text(a);
        const int found = H.gtext ? 1 : 0;
        if (found != H.guide_log) { H.guide_log = found; logf("rto: Fighter Settings: guide bar text %s", found ? "found" : "not found"); }
    }
    if (!alive(H.gtext)) { H.gtext = nullptr; return; }
    const std::string cur = str(call_obj(H.gtext, "get_Message"));
    if (cur.empty()) return;                        // the game has not written it yet
    std::string want = cur;
    erase_all(want, entry_text(false));
    erase_all(want, entry_text(true));
    if (show) want += entry_text(H.pad);
    if (want == cur) return;
    H.gwrites.push_back(g_frame);
    while (!H.gwrites.empty() && g_frame - H.gwrites.front() > 120) H.gwrites.erase(H.gwrites.begin());
    if (H.gwrites.size() > 60 && !H.fight_logged) { H.fight_logged = true; logf("rto: the guide bar text is rewritten by another writer every frame (the Color Mod's stored copy): rewriting ours"); }
    const SF6CoreStatus st = a->ui_set_text(C(), H.gtext, want.c_str());
    if (st != SF6CORE_OK) logf("rto: guide bar text not written (core status %d)", static_cast<int>(st));
}

void hint_tick() {
    const SF6CoreAPI* a = A();
    if (!a || !SF6CORE_HAS(a, ui_agents)) return;
    void* buf[4] = {};
    Obj* agent = a->ui_agents("MatchingFighterSetting", buf, 4) ? reinterpret_cast<Obj*>(buf[0]) : nullptr;
    if (!agent) { guide_apply(false); H = Hint{}; return; }
    if (agent != H.agent || !alive(H.text) || g_frame - H.found_at > 60) {
        if (agent != H.agent) { guide_apply(false); const bool pad = H.pad; H = Hint{}; H.pad = pad; }
        H.agent = agent;
        H.found_at = g_frame;
        Obj* cm = call_obj(agent, "get_ControlMain");
        Obj* spin = cm ? reinterpret_cast<Obj*>(a->ui_find_child(cm, "p_Spin_costume", 8)) : nullptr;
        H.text = spin ? reinterpret_cast<Obj*>(a->ui_find_child(spin, "e_text", 4)) : nullptr;
        const int found = H.text ? 1 : 0;
        if (found != H.text_log) {
            H.text_log = found;
            logf("rto: Fighter Settings: costume row %s", found ? "text found" : spin ? "found, no e_text in it" : "(p_Spin_costume) not found");
        }
    }
    // the device last used: the key shown is the decide key of that device
    if (pad_mask()) H.pad = true;
    else for (int vk : { 0x46, 0x1B, 0x0D, 0x25, 0x26, 0x27, 0x28, 0x5A, 0x51, 0x53, 0x44 }) if (key_down(vk)) { H.pad = false; break; }

    // the costume row's text (never written by us): when it changes, the outfit changed
    const Slot* slot = preview_slot();
    const std::string base = alive(H.text) ? str(call_obj(H.text, "get_Message")) : std::string();
    if (base != H.base) { H.base_at = H.base.empty() ? 0 : g_frame; H.base = base; }   // the first read is no change
    // the preview was identified after the outfit shown changed: it is that outfit's
    const bool has = slot && preview_since() >= static_cast<int>(H.base_at);
    if ((has ? 1 : 0) != H.has_log) {
        H.has_log = has ? 1 : 0;
        if (has) logf("rto: costume row '%s': %s/%s has options (%s)", base.c_str(), slot->fighter_dir.c_str(), slot->folder.c_str(), slot->label.c_str());
        else logf("rto: costume row '%s': no options (%s)", base.c_str(),
                  !slot ? "the preview wears no outfit with options" : "the preview was not seen again since the outfit changed");
    }

    // the focused row
    Obj* item = call_obj(agent, "GetFocusItem");
    Obj* ctrl = item ? get_obj(item, "<Control>k__BackingField") : nullptr;
    const std::string row = ctrl ? str(call_obj(ctrl, "get_Name")) : std::string();
    const bool on_row = row == "p_Spin_costume";
    guide_apply(on_row && has && !S.active);

    // the key: F / A on the costume row
    const bool raw = key_down(0x46) || (pad_mask() & g_pad_a) != 0;
    const bool down = on_row && game_has_focus() && raw;
    if (raw && !H.raw_was && !S.active) {                // every press, and what it led to
        const char* why = !game_has_focus() ? "the game window is not in front"
                        : !on_row ? "not on the costume row"
                        : !has ? "this outfit has no options"
                        : a->opt_window_open() ? "an option window is already open"
                        : g_frame - S.closed_at <= 30 ? "the menu just closed"
                        : nullptr;
        logf("rto: key %s on row '%s': %s", key_down(0x46) ? "F" : "A", row.empty() ? "?" : row.c_str(), why ? why : "opening the menu");
    }
    H.raw_was = raw;
    if (down && !H.key_was && has && !S.active && !a->opt_window_open() && g_frame - S.closed_at > 30) open();
    H.key_was = down;
}

} // namespace

void install() {
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, opt_status) || !SF6CORE_HAS(a, ui_agents)) {
        logf("rto: no option service in the core (or no core): the options menu is off, the choices apply");
        return;
    }
    SF6CoreOptCallbacks cb{};
    cb.struct_size = sizeof cb;
    cb.on_value = cb_value; cb.on_button = cb_button;
    cb.on_dialog_created = cb_created; cb.on_page_popped = cb_popped; cb.on_window_closed = cb_closed;
    g_svc = a->opt_set_callbacks(C(), &cb) == SF6CORE_OK;
    logf("rto: options menu %s", g_svc ? "ready (F on the costume row of Fighter Settings, entry in the guide bar)" : "refused by the core's option service");
}

bool active() { return S.active; }
int side() { return S.side; }
bool busy() { return S.active; }

void tick(bool fs_visible) {
    if (!g_svc) return;
    try {
        if (S.active) {
            const SF6CoreAPI* a = A();
            if (!a->opt_window_open()) {
                if (g_frame - S.opened_at > 120) teardown("window gone");
                return;
            }
            void* buf[8] = {};
            const uint32_t n = a->ui_agents("OptionDialog", buf, 8);
            for (uint32_t i = 0; i < n && i < 8; ++i) {         // every stacked page stays docked
                Obj* cm = call_obj(reinterpret_cast<Obj*>(buf[i]), "get_ControlMain");
                auto r = cm ? call(cm, "get_Position") : Ret{};
                float x = 0;
                if (cm && !r.exception_thrown) { std::memcpy(&x, r.bytes.data(), 4); if (std::fabs(x - kDockX) > 0.5f) a->ui_set_position(C(), cm, kDockX, 0.0f, 0.0f); }
            }
            if (S.rebuild_page >= 0) {
                const int p = S.rebuild_page;
                S.rebuild_page = -1;
                if (p == S.page) rebuild(p, S.rebuild_pos);
                else S.stale.insert(p);
            }
            if (S.refresh_at && g_frame >= S.refresh_at) {
                S.refresh_at = 0;
                const int pos = S.focus.count(S.page) ? S.focus[S.page] : 0;
                rebuild(S.page, pos);
            }
        }
        if (fs_visible || S.active) hint_tick();
        else if (H.agent) { guide_apply(false); H = Hint{}; }
    } catch (...) {
        logf("rto: EXCEPTION in the menu (tick)");
    }
}

} // namespace slots::rto::menu
