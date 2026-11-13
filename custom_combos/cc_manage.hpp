// cc_manage.hpp - Custom Combos: the sub-menu of a trial in the trial list (Rename, Move to, Delete, Share Trial,
// Download Trial). Included by custom_combos.cpp after the recorder, inside its anonymous namespace. The offline part
// (the buttons per tab, the trash path, the names) is cc_trials.hpp's cc::manage.
//
// Interface: the game's own windows only.
//   * the sub-menu: the game's option window, through the Native Core's option service (opt_group / opt_entry /
//     opt_open_menu, called here directly), buttons only, each entry its own key (rows sharing one do not answer);
//   * Rename: the game's text input dialog (app.UITextInputDialog, the Recording Manager's recipe: no delegate is
//     given, the result is taken in a pre-hook of InvokeResultEvent, which is then skipped);
//   * Delete: the game's Yes / No box (app.UIFlowDialog.MessageBox.Start, default No, polled);
//   * Share Trial / Download Trial: the game's OK box, "coming soon" (the server's routes are not deployed).
// Key: T on the keyboard, the top face button on a pad (RUp), on a row that is a file of ours. The trial list's input is
// blocked while the window or a box is up, and given back two frames before the list is drawn again.
//
// Hooks of its own:
//   * UI11414.UIFlowSelectItemList.OnUpdate (post): the list drawn again after a change, on the list's own thread
//     (rule 24: from LateUpdateBehavior its drawing methods fault now and then). Nothing else of the suite hooks it.
//   * UITextInputDialog.InvokeResultEvent / Cancel / InputEvent_Cancel / FlowEvent_AfterHide (pre): only OUR dialog,
//     by its address; every other dialog's calls pass untouched. The Recording Manager hooks the same methods for its
//     own dialog, by its address too: two hooks that never answer for the same object (the reference ran so beside
//     the Recording Manager). No other way: the dialog's result goes to a delegate a native module cannot make.
// Everything else runs on the game thread (tick, from the mode's frame), with nothing of the game written from a hook
// but the dialog's own skip.
#pragma once

namespace manage_ui {

using namespace cc::manage;

constexpr int kKeyT = 0x54;
constexpr size_t kNameMax = 40;
constexpr const char* kPage = "custom_combos.manage";

// ---------------------------------------------------------------- input
std::atomic<bool> key_t_down{false};
// REFramework's window messages (the game window's only): T held or not; nothing is consumed
bool on_message(void*, unsigned int msg, unsigned long long wparam, long long) {
    if ((msg == 0x0100 || msg == 0x0104) && wparam == kKeyT) key_t_down = true;           // WM_KEYDOWN / WM_SYSKEYDOWN
    else if ((msg == 0x0101 || msg == 0x0105) && wparam == kKeyT) key_t_down = false;     // WM_KEYUP / WM_SYSKEYUP
    else if (msg == 0x0008) key_t_down = false;                                           // WM_KILLFOCUS
    return true;
}
uint32_t pad_top = 0;
uint32_t pad_mask() {
    if (!pad_top) {
        pad_top = 16;                            // RUp, read from the live enum when it has it
        for (const auto& [n, v] : enum_values("via.hid.GamePadButton")) if (n == "RUp" && v > 0) pad_top = static_cast<uint32_t>(v);
    }
    void* gp = nullptr;
    try { gp = API::get()->get_native_singleton("via.hid.GamePad"); } catch (...) {}
    Method* m = gp ? method_of(type("via.hid.GamePad"), "get_ConnectingDevices", 0) : nullptr;
    if (!m) return 0;
    auto r = invoke(m, gp, {});
    Obj* devs = !r.exception_thrown ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
    uint32_t all = 0;
    for (int32_t i = 0, n = list_count(devs); i < n; ++i)
        if (Obj* p = list_item(devs, i)) all |= static_cast<uint32_t>(call_i32(p, "get_Button").value_or(0));
    return all;
}
bool key_down() { return key_t_down.load() || (pad_mask() & pad_top) != 0; }

// ---------------------------------------------------------------- the option window (the core's)
struct Row { Button b; int64_t entry = -1; };
struct Ev { int kind; size_t row; };           // 1 a button, 3 the window closed
struct Opt {
    bool tried = false, service = false;
    int64_t group = -1;
    std::vector<Row> rows;
    std::atomic<bool> open{false};
    bool cleanup = false;
    std::mutex mx;                               // the events (the core's callbacks, on the game thread: a net)
    std::deque<Ev> events;
} OPT;

void SF6CORE_CALL on_button(void*, int64_t entry, SF6CoreOptButtonResult* out) {
    std::lock_guard lk(OPT.mx);
    for (size_t i = 0; i < OPT.rows.size(); ++i)
        if (OPT.rows[i].entry == entry) {
            OPT.events.push_back({ 1, i });
            if (out) out->kind = 1;              // every button closes the window
            return;
        }
}
void SF6CORE_CALL on_window_closed(void*) {
    std::lock_guard lk(OPT.mx);
    if (!OPT.open.exchange(false)) return;       // (every client hears of every window)
    OPT.cleanup = true;
    OPT.events.push_back({ 3, 0 });
}

bool available() { return OPT.service; }
void service_start() {
    if (OPT.tried || !core || !client) return;
    OPT.tried = true;
    if (!SF6CORE_HAS(core, opt_status) || !core->opt_set_callbacks || !core->opt_open_menu) { logf("trial sub-menu off: no option service in this core"); return; }
    SF6CoreOptCallbacks cb{};
    cb.struct_size = sizeof(cb);
    cb.on_button = on_button;
    cb.on_window_closed = on_window_closed;
    OPT.service = core->opt_set_callbacks(client, &cb) == SF6CORE_OK;
    logf("trial sub-menu %s", OPT.service ? "ready (T on a trial of ours)" : "off: the option service refused its callbacks");
}
// the page of the window that closed leaves the core's tree
void opt_cleanup() {
    if (OPT.group >= 0) {
        core->opt_remove_group(client, OPT.group);
        OPT.group = -1;
        core->opt_rebuild(client);
        core->opt_ensure_built(client);
    }
    for (Row& r : OPT.rows) r.entry = -1;
    OPT.cleanup = false;
}
bool open_menu(const Trial& t) {
    if (!OPT.service || core->opt_window_open()) return false;
    if (OPT.group >= 0) opt_cleanup();
    OPT.group = core->opt_group(client, kPage, t.title.empty() ? "Trial" : t.title.c_str(), "Custom Combos");
    if (OPT.group < 0) { logf("trial sub-menu: opt_group refused"); return false; }
    std::lock_guard lk(OPT.mx);
    OPT.rows.clear();
    OPT.events.clear();
    int i = 0;
    for (const Button& b : buttons(t.level)) {
        const std::string key = std::string(kPage) + ".r" + std::to_string(++i);   // each entry its own key
        SF6CoreOptEntryDesc d{};
        d.struct_size = sizeof(d);
        d.kind = SF6CORE_OPT_BUTTON;
        d.key = key.c_str();
        d.title = b.title.c_str();
        d.desc = b.guide.c_str();
        Row r{ b, core->opt_entry(client, OPT.group, &d) };
        if (r.entry < 0) { logf("trial sub-menu: opt_entry refused"); OPT.cleanup = true; return false; }
        OPT.rows.push_back(r);
    }
    core->opt_rebuild(client);
    core->opt_ensure_built(client);
    if (core->opt_open_menu(client, kPage, -1) != SF6CORE_OK) { logf("trial sub-menu: opt_open_menu refused"); OPT.cleanup = true; return false; }
    OPT.open = true;
    return true;
}

// ---------------------------------------------------------------- the game's dialogs
std::atomic<uintptr_t> dlg_addr{0};            // our text dialog (compared in the hooks)
std::atomic<bool> dlg_decided{false}, dlg_cancelled{false}, dlg_hidden{false};
std::mutex dlg_mx;
std::string dlg_text;

bool open_text_dialog(const std::string& initial) {
    Method* m = method_of(type("app.UITextInputDialog"), "OpenTextInputDialog", 5);
    Obj* init = recorder::managed_string(initial);
    if (!m || !init) return false;
    dlg_decided = dlg_cancelled = dlg_hidden = false;
    { std::lock_guard lk(dlg_mx); dlg_text.clear(); }
    // (RequestType 16 ReciperMemo: a free text; VisualType 1: Fighting Ground)
    auto r = invoke(m, nullptr, { nullptr, arg_i32(16), arg_i32(1), init, arg_i32(static_cast<int32_t>(kNameMax)) });
    Obj* dlg = !r.exception_thrown ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
    if (!dlg) return false;
    dlg_addr = reinterpret_cast<uintptr_t>(dlg);
    if (Obj* e = recorder::managed_string("")) call_ok(dlg, "SetupDefaultMessage(System.String)", { e });   // no explanation line
    core->obj_set_string(client, dlg, "DefaultMessage", "");
    return true;
}
bool ours(void* self) { const uintptr_t a = dlg_addr.load(); return a && reinterpret_cast<uintptr_t>(self) == a; }
// InvokeResultEvent(String), any thread: our dialog's text, the game's call skipped (no delegate was given)
int pre_dlg_result(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!ours(argv[1])) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try { std::lock_guard lk(dlg_mx); dlg_text = str(reinterpret_cast<Obj*>(argv[2])); } catch (...) {}
    dlg_decided = true;
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}
int pre_dlg_cancel(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (ours(argv[1])) dlg_cancelled = true;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
int pre_dlg_hide(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (ours(argv[1])) dlg_hidden = true;
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

// app.UIFlowDialog.MessageBox.Start(message, title, default, cancel, style, icon, game mode): the box, rooted while polled
Obj* box_start(const std::string& msg, const std::string& title, int def, int cancel, int style) {
    Method* m = method_of(type("app.UIFlowDialog.MessageBox"), "Start", 7);
    Obj* ms = recorder::managed_string(msg);
    Obj* ts = recorder::managed_string(title);
    if (!m || !ms || !ts) return nullptr;
    auto r = invoke(m, nullptr, { ms, ts, arg_i32(def), arg_i32(cancel), arg_i32(style), arg_i32(-1), arg_i32(1) });
    Obj* h = !r.exception_thrown ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
    if (h) h->add_ref();
    return h;
}
int box_style(const char* name, int fallback) {   // app.DialogButtonStyle: OK 1 ... YesNo 4 (YesNo measured)
    for (const auto& [n, v] : enum_values("app.DialogButtonStyle")) if (n == name) return static_cast<int>(v);
    return fallback;
}
// -1 still up, else the button chosen (0 = the first)
int box_poll(Obj* h) {
    const auto end = call_bool(h, "get_IsEnd");
    if (!end) return -2;                         // unreadable
    if (!*end) return -1;
    Method* m = method_of(type("app.UIFlowDialog.MessageBox"), "GetSelectValue", 1);
    auto r = invoke(m, nullptr, { h });
    return r.exception_thrown ? -2 : static_cast<int32_t>(r.dword);
}

// ---------------------------------------------------------------- the files
bool edit_file(Trial& t, const char* field, const json& value) {
    json data = read_json(recorder::upath(t.path));
    if (!data.is_object()) { logf("trial sub-menu: %s not read", t.path.c_str()); return false; }
    data[field] = value;
    if (!recorder::write_json(recorder::upath(t.path), data)) { logf("trial sub-menu: %s not written", t.path.c_str()); return false; }
    t.j[field] = value;
    return true;
}
bool do_rename(Trial& t, const std::string& raw) {
    const std::string name = clean_name(raw, kNameMax);
    if (name.empty() || name == t.title || !edit_file(t, "title", name)) return false;
    t.title = name;
    logf("trial renamed: %s", name.c_str());
    return true;
}
bool do_move(Trial& t, int level) {
    if (level < 0 || level > 2 || level == t.level || !edit_file(t, "level", level)) return false;
    t.level = level;
    logf("trial moved to %s: %s", level_name(level), t.title.c_str());
    return true;
}
// the content goes to _trash/<Fighter>/ (checked by reading it back), then the file becomes the marker
bool do_delete(const TrialP& t) {
    const fs::path p = recorder::upath(t->path);
    const json data = read_json(p);
    const char* dir = fighter_dir(t->fighter_id);
    if (!data.is_object() || !dir) { logf("trial sub-menu: delete refused (%s not read)", t->path.c_str()); return false; }
    const fs::path trash = trash_path(kData, dir, p, take::stamp_of(std::time(nullptr)).file);
    if (!recorder::write_json(trash, data) || read_json(trash) != data) { logf("trial sub-menu: delete refused (no copy in the trash)"); return false; }
    if (!recorder::write_json(p, deleted_marker())) { logf("trial sub-menu: delete refused (%s not written)", t->path.c_str()); return false; }
    logf("trial deleted, kept in %s", u8(trash).c_str());
    mode::on_discard(t->fighter_id, t->path);
    return true;
}

// ---------------------------------------------------------------- the list drawn again, on its own thread
std::atomic<Obj*> redraw_del{nullptr};         // the list's InputDelegates (rooted while asked)
std::atomic<double> redraw_at{0};
std::atomic<uint32_t> redrawn{0};
void ask_redraw(Obj* agent) {
    Obj* del = get_obj(agent, "<InputDelegates>k__BackingField");
    if (!del) return;
    del->add_ref();
    redraw_at = clock_s();
    if (Obj* old = redraw_del.exchange(del)) old->release();
}
// UI11414.UIFlowSelectItemList.OnUpdate (post), the list's thread: the rows read again from the level lists, the held tab
// set to another real tab as if the player came from it, and the list's own tab handler draws the tab (the cursor goes
// back to the tab's first row). Measured 20 of 20 without a fault; the same from LateUpdateBehavior faulted.
void post_list_update(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!redraw_del.load(std::memory_order_relaxed)) return;
    Obj* del = redraw_del.exchange(nullptr);
    if (!del) return;
    try {
        if (clock_s() - redraw_at.load() <= 2 && live(del)) {   // (asked long ago: the list went meanwhile)
            Obj* prm = call_obj(del, "get_Param");
            if (prm && call_ok(prm, "SetupAllItemList")) {
                const char* f = "<CurrentSelectTabCategory>k__BackingField";
                set(prm, f, inum(prm, f, 0) == 0 ? 1 : 0);
                call_ok(del, "EventTabChanged");
                redrawn.fetch_add(1);
            }
        }
    } catch (...) {}
    del->release();
}

// ---------------------------------------------------------------- the state machine (game thread)
enum class St { Idle, Menu, RenameOpen, Rename, ConfirmOpen, Confirm, InfoOpen, Info, Settle, Redraw };
struct S_ {
    St st = St::Idle;
    int wait = 0, quiet = 0;
    TrialP trial;
    int level = -1;                              // a Move asked
    std::optional<Act> next;
    bool dirty = false, key_was = false;
    Obj* list = nullptr;                         // the list agent blocked (rooted while blocked)
    Obj* box = nullptr;
    std::string info;
} S;

void block_list(Obj* agent, bool on) {
    if (on) {
        if (S.list || !agent) return;
        agent->add_ref();
        S.list = agent;
        if (core->ui_set_block_input) core->ui_set_block_input(client, agent, 1);
    } else if (S.list) {
        if (live(S.list) && core->ui_set_block_input) core->ui_set_block_input(client, S.list, 0);
        S.list->release();
        S.list = nullptr;
    }
}
void finish(bool dirty) {
    if (dirty) S.dirty = true;
    if (S.dirty && S.trial && S.next != Act::Delete) mode::manage_changed(S.trial->fighter_id);
    S.st = St::Settle;
    S.wait = S.dirty ? 30 : 10;
    dlg_addr = 0;
    if (S.box) { S.box->release(); S.box = nullptr; }
}

bool busy() { return S.st != St::Idle; }

Obj* list_agent() {
    for (Obj* a : agents(kListAgent))
        if (get_obj(get_obj(get_obj(a, "<InputDelegates>k__BackingField"), "<Flow>k__BackingField"), "_Param")) return a;
    return nullptr;
}

void tick() {
    if (custom_mode.load()) service_start();
    if (!OPT.service) return;
    if (OPT.cleanup && !OPT.open.load() && !core->opt_window_open()) { std::lock_guard lk(OPT.mx); opt_cleanup(); }
    Obj* agent = S.st == St::Idle && !custom_mode.load() ? nullptr : list_agent();
    switch (S.st) {
    case St::Idle: {
        if (!agent) { S.key_was = false; return; }
        const bool down = key_down();
        const bool pressed = down && !S.key_was;
        S.key_was = down;
        if (!pressed) return;
        const TrialP t = g_focus && frame - g_focus->at <= 2 ? g_focus->trial : nullptr;
        if (!t || !can_manage(*t)) return;       // a recording slot, or no row of ours under the cursor
        if (!open_menu(*t)) return;
        S = S_{};
        S.trial = t;
        S.st = St::Menu;
        S.key_was = true;
        block_list(agent, true);
        logf("trial sub-menu open: %s", t->title.c_str());
        return;
    }
    case St::Menu: {
        bool closed = false;
        {
            std::lock_guard lk(OPT.mx);
            while (!OPT.events.empty()) {
                const Ev e = OPT.events.front();
                OPT.events.pop_front();
                if (e.kind == 3) { closed = true; continue; }
                if (e.row >= OPT.rows.size()) continue;
                const Button& b = OPT.rows[e.row].b;
                switch (b.act) {
                case Act::Rename: case Act::Delete: S.next = b.act; break;
                case Act::Move: S.level = b.level; break;
                case Act::Share: S.next = b.act; S.info = share_trial(*S.trial); break;
                case Act::Download: S.next = b.act; S.info = download_trial(S.trial->fighter_id); break;
                }
            }
        }
        if (!closed) {                           // (a window that went without telling)
            S.quiet = OPT.open.load() && core->opt_window_open() ? 0 : S.quiet + 1;
            if (S.quiet < 60) return;
        }
        const bool moved = S.level >= 0 && do_move(*S.trial, S.level);
        if (moved) S.dirty = true;
        if (S.next == Act::Rename) { S.st = St::RenameOpen; S.wait = 12; }
        else if (S.next == Act::Delete) { S.st = St::ConfirmOpen; S.wait = 12; }
        else if (S.next == Act::Share || S.next == Act::Download) { S.st = St::InfoOpen; S.wait = 12; }
        else finish(moved);
        return;
    }
    case St::RenameOpen:
        if (--S.wait > 0) return;
        if (!open_text_dialog(S.trial->title)) { logf("trial sub-menu: text dialog not opened"); finish(false); return; }
        S.st = St::Rename;
        S.wait = 0;
        return;
    case St::Rename: {
        ++S.wait;
        if (!(dlg_decided || dlg_cancelled || dlg_hidden) && S.wait < 36000) return;
        std::string text;
        { std::lock_guard lk(dlg_mx); text = dlg_text; }
        finish(dlg_decided && !dlg_cancelled && do_rename(*S.trial, text));
        return;
    }
    case St::ConfirmOpen:
        if (--S.wait > 0) return;
        S.box = box_start("Delete '" + S.trial->title + "'?\nA copy is kept in the _trash folder.", "Delete", 1, 1, box_style("YesNo", 4));
        if (!S.box) { logf("trial sub-menu: message box not opened"); finish(false); return; }
        S.st = St::Confirm;
        return;
    case St::Confirm: {
        const int v = box_poll(S.box);
        if (v == -1) return;
        finish(v == 0 && do_delete(S.trial));    // 0: the first button, Yes
        return;
    }
    case St::InfoOpen:
        if (--S.wait > 0) return;
        S.box = box_start(S.info, S.next == Act::Share ? "Share Trial" : "Download Trial", 0, 0, box_style("OK", 1));
        if (!S.box) { finish(false); return; }
        S.st = St::Info;
        return;
    case St::Info:
        if (box_poll(S.box) == -1) return;
        finish(false);
        return;
    case St::Settle:
        if (--S.wait > 0) return;
        block_list(nullptr, false);              // the list gets its input back first, and is drawn again two frames later
        S.st = St::Redraw;
        S.wait = 2;
        return;
    case St::Redraw:
        if (--S.wait > 0) return;
        if (S.dirty && agent) ask_redraw(agent);
        S = S_{};
        S.key_was = true;
        return;
    }
}

// the list or the mode went with a window or a box up: input given back, nothing kept
void leave() {
    if (S.st == St::Idle) return;
    block_list(nullptr, false);
    if (S.box) { S.box->release(); S.box = nullptr; }
    dlg_addr = 0;
    S = S_{};
}

void install_hooks() {
    hook("app.esports.UI11414.UIFlowSelectItemList", "OnUpdate", -1, pre_pass, post_list_update);
    const char* D = "app.UITextInputDialog";
    hook(D, "InvokeResultEvent", -1, pre_dlg_result, post_pass);
    hook(D, "Cancel", -1, pre_dlg_cancel, post_pass);
    hook(D, "InputEvent_Cancel", -1, pre_dlg_cancel, post_pass);
    hook(D, "FlowEvent_AfterHide", -1, pre_dlg_hide, post_pass);
}

}  // namespace manage_ui
