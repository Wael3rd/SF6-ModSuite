// ui/dump — a read-only probe dump for the in-game diagnosis of two test B step 6 bugs, OFF by default.
// Switch: reframework/data/SF6_CRSMNative_data/dump_on.txt (looked at every 2 s); output: dump.txt next to it (appended,
// written off the game thread), one journal line per stage. Nothing is written into the game: getters and field reads.
//
// (1) Record page, the slot NAME text: at the first borrow after the switch, the borrowed text before and after its
//     styling and the centre text it copies; then, from the Record row updater's post (the thread the game writes them
//     on), the focused row and one unfocused row (our name, "Slot N", the frames, the right column, our arrow text):
//     stage S1, again 2+ frames later (S2), and once the focus moved to another slot (S3).
// (2) Manager spin: every field of the spin part, of its carousel part (child 0) and of that list's own list, the row's
//     ViewData entry, its Data / ParentData and their option arrays, the carousel control's index / count: before a
//     change of the loadout labels on the shown page, 3 frames after it, and 2 frames after each of the next 8 Left /
//     Right key presses (keyboard: Q / D / arrows) while the Manager page is shown.
// (3) The game's tab methods (switch tab_dump_on.txt, read once at the start; output tab_methods.txt, appended): every
//     method whose name holds "Tab", "Primary" or "Select" of the pause menu's types and their parents, with its signature,
//     index and code address; with it, dialogs.cpp surveys the L1+R1 shortcut's code (the log names its method). Written for the tab moves under CRSM's dialogs (L1+R1 / A+E): "InitTabItem(System.Int32)" never
//     matched (the method has no parameter); the next run names what the game has, without a debugger.
#include "dump.hpp"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#include "common.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "internal.hpp"
#include "manager_page.hpp"
#include "perf.hpp"

namespace ui::dump {
namespace {

namespace gm = game::gm;
using gx::MO;
using API = reframework::API;

constexpr const char* kSwitch = "reframework/data/SF6_CRSMNative_data/dump_on.txt";
constexpr const char* kOut = "reframework/data/SF6_CRSMNative_data/dump.txt";
constexpr const char* kTabSwitch = "reframework/data/SF6_CRSMNative_data/tab_dump_on.txt";
constexpr const char* kTabOut = "reframework/data/SF6_CRSMNative_data/tab_methods.txt";
// (3) the pause menu's types (the training menu, its tab parts, the agents, the dialogs CRSM opens over it)
constexpr const char* kTabTypes[] = { "app.training.UIFlowTrainingMenu.Param", "app.training.UIFlowTrainingMenu",
                                      "app.UIPartsPageIndex", "app.UIPartsTrainingTab", "app.UIPartsSecondaryTabItem",
                                      "app.UIAgent", "app.training.TrainingManager", "app.training.UITrainingMenu",
                                      "app.UIFlowOptionBGDialog.Param", "app.UITextInputDialog", "app.UIFlowDialog.MessageBox",
                                      "via.gui.ScrollList" };
constexpr int kMaxPresses = 8;

std::atomic<bool> g_on{false};
std::atomic<bool> g_want{false};      // dump_on.txt exists (published by the background writer)
bool g_watching = false;
// (1)
std::atomic<int> g_borrow_stage{0};              // 0: to do, 1: done
int g_row_stage = 0;                             // 0 none, 1 S1 written, 2 S2 written, 3 S3 written (done)
uint64_t g_s1_frame = 0;
int g_s1_slot = -1;
bool g_stage_focused = false, g_stage_unfocused = false;
// (2)
const SF6CoreAPI* g_api = nullptr;
int64_t g_spin_row = -1;
uint64_t g_after_at = 0;                         // frame of the "after" dump (0 = none)
int g_presses = 0;
uint64_t g_press_at = 0;
const char* g_press_key = "";
bool g_keys_prev[4]{};

void out(const std::string& line) { cmn::append_file(kOut, line + "\n"); }

// Every instance field of `o` (its type and its parents'), one line.
std::string fields(MO* o) {
    if (!o || !gm::is_managed(o)) return "(none)";
    API::TypeDefinition* td = o->get_type_definition();
    std::string s = (td ? td->get_full_name() : std::string("?")) + ":";
    for (API::TypeDefinition* t = td; t; t = t->get_parent_type()) {
        const std::string tn = t->get_full_name();
        if (tn == "System.Object" || tn == "via.clr.ManagedObject") break;
        for (API::Field* f : t->get_fields()) {
            if (!f || f->is_static()) continue;
            const void* p = f->get_data_raw(o, false);
            s += std::string(" ") + (f->get_name() ? f->get_name() : "?") + "=" + gx::format_value(f->get_type(), p);
        }
    }
    return s;
}

void text_row(const char* stage, const char* which, int n, MO* N, MO* name, MO* centre, MO* sub, MO* R) {
    char h[96];
    std::snprintf(h, sizeof(h), "[%s %s slot %d f%llu]", stage, which, n, (unsigned long long)cmn::frame());
    out(std::string(h) + " our name: " + gx::describe_text(N));
    out(std::string(h) + " Slot N (e_txt_name): " + gx::describe_text(name));
    out(std::string(h) + " frames (e_txt_center): " + gx::describe_text(centre));
    out(std::string(h) + " right column (e_txt_sub): " + gx::describe_text(sub));
    out(std::string(h) + " our arrow: " + gx::describe_text(R));
}

// (2) the spin's objects, from the core's row part (the row's UIPartsGroup while its page is shown)
void spin_dump(const std::string& when) {
    if (!g_api || g_spin_row < 0 || !SF6CORE_HAS(g_api, pm_row_part)) return;
    char h[160];
    std::snprintf(h, sizeof(h), "[spin %s f%llu]", when.c_str(), (unsigned long long)cmn::frame());
    MO* group = static_cast<MO*>(perf::counted(g_api)->pm_row_part(g_spin_row));
    if (!group || !gm::is_managed(group)) { out(std::string(h) + " the row's part: none (page not shown)"); return; }
    MO* spin = nullptr;
    MO* ch = gm::ref(group, "_Children");
    for (int32_t j = 0, n = gm::list_count(ch); j < n && !spin; ++j) {
        MO* p = gm::list_item(ch, j);
        if (p && gx::is_type(p, "app.UIPartsSpin")) spin = p;
    }
    out(std::string(h) + " group: " + fields(group));
    out(std::string(h) + " spin part: " + fields(spin));
    MO* carousel = spin ? gm::call_obj(spin, "GetChild(System.Int32)", { gm::arg_i32(0) }) : nullptr;
    out(std::string(h) + " spin child 0: " + fields(carousel));
    out(std::string(h) + " its _List: " + fields(gm::ref(carousel, "_List")));
    MO* ctrl = spin ? gm::call_obj(spin, "get_Control") : nullptr;
    MO* sl = ctrl ? gx::child(ctrl, "p_ScrollList_0_h") : nullptr;
    int32_t sel = -999, cnt = -999;
    if (sl) { gx::call_i32(sl, "get_SelectedIndex", &sel); gx::call_i32(sl, "get_ItemCount", &cnt); }
    char c[128];
    std::snprintf(c, sizeof(c), " carousel control p_ScrollList_0_h: SelectedIndex %d, ItemCount %d", sel, cnt);
    out(std::string(h) + c);
    // the row's ViewData entry: the list index of this pooled group
    MO* param = gx::find_param();
    MO* groups = gm::ref(gm::ref(param, "_SecondaryList"), "_Children");
    int32_t idx = -1;
    for (int32_t i = 0, n = gm::list_count(groups); i < n; ++i) if (gm::list_item(groups, i) == group) { idx = i; break; }
    MO* list = gm::ref(param, "_ViewDataList");
    MO* vd = (idx >= 0 && idx < gm::list_count(list)) ? gm::list_item(list, idx) : nullptr;
    std::snprintf(c, sizeof(c), " list index %d", idx);
    out(std::string(h) + c + ", ViewData: " + fields(vd));
    MO* data = gm::ref(vd, "Data");
    MO* parent = gm::ref(vd, "ParentData");
    out(std::string(h) + " ViewData.Data: " + fields(data));
    out(std::string(h) + " ViewData.ParentData: " + fields(parent));
    out(std::string(h) + " Data._ChildData len " + std::to_string(gm::array_len(gm::ref(data, "_ChildData"))) +
        ", ParentData._ChildData len " + std::to_string(gm::array_len(gm::ref(parent, "_ChildData"))));
    out(std::string(h) + " menu Param: " + fields(param));
}

}  // namespace

bool on() { return g_on.load(std::memory_order_relaxed); }

// (1) style (windows.cpp): the first borrow after the switch, before and after
void borrowed(MO* t, MO* ref, bool after) {
    if (!on() || g_borrow_stage.load() != 0) return;
    const char* when = after ? "after our styling" : "before our styling";
    out(std::string("[S0 borrow f") + std::to_string(cmn::frame()) + "] borrowed text " + when + ": " + gx::describe_text(t));
    if (!after) out(std::string("[S0 borrow] the centre text it copies: ") + gx::describe_text(ref));
    else { g_borrow_stage = 1; cmn::journal("probe dump: S0 (a borrowed name text before / after its styling) in dump.txt"); }
}

// (1) slot_row_texts (windows.cpp, the Record row updater's post): S1, S2 (2+ frames later), S3 (focus on another slot)
void slot_row(int n, bool focused, MO* N, MO* name, MO* centre, MO* sub, MO* R) {
    if (!on() || g_row_stage >= 3) return;
    const uint64_t fr = cmn::frame();
    const int next = g_row_stage + 1;
    if (next == 2 && fr < g_s1_frame + 2) return;
    if (focused) {
        if (g_stage_focused) return;
        if (next == 3 && n == g_s1_slot) return;            // S3: the focus on another slot
        text_row(next == 1 ? "S1" : next == 2 ? "S2" : "S3", "focused", n, N, name, centre, sub, R);
        g_stage_focused = true;
        if (next == 1) { g_s1_frame = fr; g_s1_slot = n; }
    } else {
        if (g_stage_unfocused || !g_stage_focused) return;  // the unfocused row after the focused one of the stage
        text_row(next == 1 ? "S1" : next == 2 ? "S2" : "S3", "unfocused", n, N, name, centre, sub, R);
        g_stage_unfocused = true;
    }
    if (g_stage_focused && g_stage_unfocused) {
        g_row_stage = next;
        g_stage_focused = g_stage_unfocused = false;
        cmn::journal("probe dump: Record rows stage S%d in dump.txt", next);
    }
}

// (2) manager_page.cpp sync(): the loadout labels change on the shown page
void spin_labels(int64_t row, size_t before, size_t after) {
    if (!on()) return;
    g_spin_row = row;
    spin_dump("before the labels change " + std::to_string(before) + " -> " + std::to_string(after));
    g_after_at = cmn::frame() + 3;
    g_presses = 0;
    cmn::journal("probe dump: Manager spin before its labels change (%zu -> %zu) in dump.txt", before, after);
}

// Frame callback (game thread, ui::lock held): the switch, the spin's later dumps
void frame(bool paused) {
    const uint64_t fr = cmn::frame();
    if (!g_watching) { g_watching = true; cmn::bgwriter::watch_file(kSwitch, &g_want); }
    {
        const bool want = g_want.load(std::memory_order_relaxed);      // dump_on.txt, polled by the background writer
        if (want != g_on.load()) {
            g_on = want;
            if (want) {
                g_api = cmn::core();
                std::time_t now = std::time(nullptr);
                char b[64]{};
                std::tm tm{};
                localtime_s(&tm, &now);
                std::strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
                out(std::string("==== probe dump on, ") + b + " (CRSMNative, read-only) ====");
                cmn::journal("probe dump: on (dump_on.txt), writing dump.txt");
            } else {
                cmn::journal("probe dump: off");
            }
        }
    }
    if (!on() || g_spin_row < 0) return;
    if (g_after_at && fr >= g_after_at) {
        g_after_at = 0;
        spin_dump("3 frames after the labels change");
        cmn::journal("probe dump: Manager spin after its labels change in dump.txt");
    }
    if (g_press_at && fr >= g_press_at) {
        g_press_at = 0;
        spin_dump(std::string("2 frames after the key ") + g_press_key + " (press " + std::to_string(g_presses) + ")");
        if (g_presses == kMaxPresses) cmn::journal("probe dump: Manager spin, %d presses in dump.txt (done)", kMaxPresses);
    }
    if (!paused || g_presses >= kMaxPresses || !ui::manager::page_shown()) return;
    static const int kVk[4] = { 'Q', 'D', VK_LEFT, VK_RIGHT };
    static const char* const kNames[4] = { "Left (Q)", "Right (D)", "Left (arrow)", "Right (arrow)" };
    for (int i = 0; i < 4; ++i) {
        const bool down = cmn::key_down(kVk[i]);
        if (down && !g_keys_prev[i] && !g_press_at) { ++g_presses; g_press_at = fr + 2; g_press_key = kNames[i]; }
        g_keys_prev[i] = down;
    }
}

bool tab_switch() { std::error_code ec; return std::filesystem::exists(kTabSwitch, ec); }

// (3) at the plugin start (ui::start): the type database only, nothing of the game called
void tab_methods() {
    if (!tab_switch()) return;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::time_t now = std::time(nullptr);
    char when[64]{};
    std::tm tm{};
    localtime_s(&tm, &now);
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    std::string text = std::string("==== tab methods, ") + when + " (CRSMNative, read-only; " +
                       std::to_string(API::get()->tdb()->get_num_methods()) + " methods in the game) ====\n";
    int listed = 0;
    for (const char* tn : kTabTypes) {
        API::TypeDefinition* td = API::get()->tdb()->find_type(tn);
        if (!td) { text += std::string(tn) + ": type not found\n"; continue; }
        for (API::TypeDefinition* t = td; t; t = t->get_parent_type()) {
            const std::string name = t->get_full_name();
            if (name == "System.Object" || name == "via.clr.ManagedObject") break;
            text += (t == td ? "" : "  parent ") + name + ":\n";
            for (API::Method* m : t->get_methods()) {
                const char* mn = m ? m->get_name() : nullptr;
                if (!mn || (!std::strstr(mn, "Tab") && !std::strstr(mn, "Primary") && !std::strstr(mn, "Select"))) continue;
                std::string sig = std::string(mn) + "(";
                const auto ps = m->get_params();
                for (size_t i = 0; i < ps.size(); ++i) {
                    auto* pt = reinterpret_cast<API::TypeDefinition*>(ps[i].t);
                    sig += (i ? ", " : "") + (pt ? pt->get_full_name() : std::string("?")) + (ps[i].name ? std::string(" ") + ps[i].name : "");
                }
                sig += ")";
                API::TypeDefinition* rt = m->get_return_type();
                const uintptr_t fn = reinterpret_cast<uintptr_t>(m->get_function_raw());
                char b[96];
                std::snprintf(b, sizeof(b), "    #%u  code 0x%llX (rva 0x%llX)  ", m->get_index(), (unsigned long long)fn,
                              (unsigned long long)(fn > base ? fn - base : 0));
                text += b + (rt ? rt->get_full_name() : std::string("?")) + " " + sig + "\n";
                ++listed;
            }
        }
    }
    cmn::append_file(kTabOut, text);
    cmn::log_info("[CRSMNative] tab methods: %d listed in SF6_CRSMNative_data/tab_methods.txt (tab_dump_on.txt; delete it to stop)", listed);
}

}  // namespace ui::dump
