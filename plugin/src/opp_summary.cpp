// SF6 Slots — Battle Settings > Other > Opponent Outfit Settings with more outfits than the game has.
//
// The game's bug (seen on 05/11 with Cammy: 5 outfits of the game + 15 slots): a fighter's page of that menu
// (app.UIFlowOtherMatchingFighterSetting.UIParam) shows a summary, one label per outfit ("Outfit 1: 1": the colours turned
// on). UpdateCosColorTextList writes it: it reads the length of the page's working copy of the settings
// (SettingDataDuplicated.CostumeSettings, one entry per outfit: our slots are there) once, then writes label i for entry i
// into the summary's widgets (mCosColorTextList.get_Items(): 6 of them, never more than the game's own outfits). With more
// outfits than widgets it writes the 6 widgets, then throws (index out of range) from inside the page's setup: the page never
// shows, the screen stays empty and Esc does nothing.
//
// The fix (the idea of ZZtaii's Slotsloader fork, MIT: SlotsloaderOpponentOutfitSummary.lua, 1.10.7): while that method runs,
// and only when the outfits outnumber the widgets, the list's length (List._size) is the number of widgets; it is put back
// as soon as the method returns. The method only reads the list (get_Item; its code was read on 05/11): the summary shows
// the first 6 outfits (the game's own come first), as for a fighter of the game, and every outfit stays in the list for the
// rest of the page (the outfit spin, the colour grid, the save). Nothing else changes, and nothing changes for a fighter
// whose outfits all have a widget.
//
// The summary's list shows 5 lines (its via.gui.ScrollList's ItemCount; the sixth widget is written and stays hidden), so nothing past the fifth outfit showed.
// Those lines are ours (frame, read 07/11): copies of the game's text under the list's parent, in its style, at its step, made in the guide list's update (R19,
// fsname::run_in_guide_update), found by name every frame (R35), only on this page. The name is the game's (CostumeInventory.GetName), the count the outfit's
// ValidColorIdList, what goes round them comes from a line the game wrote. A name too long for its place slides in it and the count stays (name_scroll.cpp);
// with more lines than rows they follow the outfit shown, and a row at an end says how many are past it (line_window). The list's ItemCount is not touched: a live
// change to a native list that the fix below keeps short is not worth the risk for one line.
//
// - One hook, pre + post, on that method only. The game calls it when the page opens and when a setting of the page changes
//   (no per-frame caller: the page has no update method). R18: hooked only once a check over every method of the game found
//   no other method entering its code, on a thread of its own (as the core's and the Recording Manager's checks), its result
//   kept per game version in SF6_CostumeSlots_data/codecheck.txt.
// - The hook body runs on the thread the game draws the page on, inside its own call on the page (`this` and its objects are
//   alive). It reads three fields and the widgets' count; the two writes (the length, then the length back) go through the
//   core's guarded field write (any thread). The list is held (add_ref) until its length is back.
// - Restored on every path: by the post hook; a draw that threw before its end (no post hook) is put back by the next draw,
//   else two frames later by the frame. A length the game changed meanwhile is left as it is (logged).
#include "slots.hpp"
#include "slots_ui.hpp"
#include "opp_summary.hpp"
#include "colormod_link.hpp"
#include "name_scroll.hpp"
#include "perf.hpp"
#include "scroll_logic.hpp"
#include "words.hpp"

#include <map>
#include <vector>

#include <mutex>

namespace slots::oppsum {
namespace {

constexpr const char* kType = "app.UIFlowOtherMatchingFighterSetting.UIParam";
constexpr const char* kMethod = "UpdateCosColorTextList";
constexpr const char* kCacheFile = "reframework/data/SF6_CostumeSlots_data/codecheck.txt";

TD* g_page = nullptr;                              // the page's type
Method* g_m = nullptr;                             // UpdateCosColorTextList()
API::Field *f_dup_init = nullptr, *f_dup = nullptr, *f_fid = nullptr, *f_sum = nullptr, *f_cos = nullptr, *f_size = nullptr;
Method* m_len = nullptr;                           // System.Array.get_Length

// ---- R18 ----
enum Verdict : int { kPending, kUnique, kShared, kFailed };
std::atomic<int> g_verdict{ kPending };
CodeCheck g_now, g_found;                          // the method now; the check's result (written before g_verdict)
uintptr_t g_raw = 0, g_code = 0;
bool g_from_cache = false;
uint64_t g_scan_ms = 0;
uint32_t g_scanned = 0;
const char* g_cache_why = "";
bool g_cache_written = false;
std::string g_key, g_label;
bool g_hook_done = false;                          // the verdict was acted on (hooked or not)

// ---- the length changed for the draw under way (or for one that threw) ----
struct Swap {
    Obj* list = nullptr;                           // held (add_ref) until its length is back
    int full = 0, shown = 0;
    uint32_t frame = 0;
    DWORD tid = 0;
};
std::mutex g_mx;
Swap g_swap;
std::atomic<bool> g_pending{ false };
std::atomic<uint64_t> g_last_note{ 0 };            // fighter / outfits / widgets of the last line logged
std::atomic<int> g_refusals{ 0 };

Obj* obj_at(Obj* o, API::Field* f) {
    void* p = o && f ? f->get_data_raw(o, false) : nullptr;
    return p ? *reinterpret_cast<Obj**>(p) : nullptr;
}
bool i32_at(Obj* o, API::Field* f, int32_t& out) {
    void* p = o && f ? f->get_data_raw(o, false) : nullptr;
    if (!p) return false;
    memcpy(&out, p, sizeof out);
    return true;
}

// The length back. Called with g_mx held.
void restore_locked(const char* why) {
    if (!g_swap.list) return;
    Swap s = g_swap;
    g_swap = Swap{};
    g_pending.store(false, std::memory_order_release);
    int32_t now = -1;
    i32_at(s.list, f_size, now);
    if (now != s.shown) {
        logf("Opponent Outfit Settings: the outfit list's length is %d, not the %d set for the summary: left as the game made "
             "it (full length %d)", now, s.shown, s.full);
    } else if (!ui::set_i64(s.list, "_size", s.full)) {
        // the core refused the write back (turned off meanwhile): our own change undone directly
        void* p = f_size->get_data_raw(s.list, false);
        if (p) memcpy(p, &s.full, sizeof s.full);
        logf("Opponent Outfit Settings: the core refused to put the outfit list's length back: written directly (%d)", s.full);
    }
    s.list->release();
    if (why) logf("Opponent Outfit Settings: the outfit list's full length put back (%s)", why);
}

// The character's Random outfit is no choice for an opponent (it is never worn: a draw replaces it): out of the page's two working copies
// of the settings (SettingDataDuplicated, which the page spins, shows and saves, and its _Initial twin), found by the registry's "random"
// number (costumes::random_outfit), never a fixed one. Only the entry of that CostumeId is taken out (RemoveAt): the entries before it keep
// their places (Random is the last outfit of its character), so no index the game, the Color Mod or the lines below hold moves. A page that
// has no such entry is left alone. Called before the list's length is read, in the draw's pre hook (the page's own thread).
void drop_random(Obj* page) {
    int32_t fid = -1;
    const int rno = i32_at(page, f_fid, fid) ? costumes::random_outfit(fid) : -1;
    if (rno < 0 || !f_cos) return;
    for (API::Field* fd : { f_dup, f_dup_init }) {
        Obj* list = obj_at(obj_at(page, fd), f_cos);
        if (!list) continue;
        for (int i = list_count(list) - 1; i >= 0; --i) {
            Obj* o = list_item(list, i);
            int32_t cos = -1;
            if (!o || !get_i32(o, "CostumeId", cos) || cos != rno) continue;
            if (!call(list, "RemoveAt", { arg_i32(i) }).exception_thrown) {
                static std::atomic<int> told{ 0 };
                if (told.fetch_add(1) < 10) logf("[F%d] Opponent Outfit Settings: Random (outfit %d) left out of the page's outfits", fid, rno);
            }
            break;
        }
    }
}

void on_draw(Obj* page) {
    if (!page || page->get_type_definition() != g_page) return;   // the page's own call only
    std::lock_guard<std::mutex> lk(g_mx);
    restore_locked("the last summary draw threw before its end");
    drop_random(page);
    Obj* list = obj_at(obj_at(page, f_dup), f_cos);
    Obj* summary = obj_at(page, f_sum);
    int32_t outfits = 0;
    if (!list || !summary || !i32_at(list, f_size, outfits)) return;
    Obj* items = call_obj(summary, "get_Items");                  // the widgets, as the method reads them
    Ret r = items ? call(m_len, items) : Ret{};
    const int widgets = items && !r.exception_thrown ? static_cast<int>(r.dword) : 0;
    const int shown = draw_length(outfits, widgets);
    int32_t fid = -1;
    i32_at(page, f_fid, fid);
    const uint64_t note = (static_cast<uint64_t>(static_cast<uint32_t>(fid)) << 32) |
                          (static_cast<uint64_t>(outfits & 0xFFFF) << 16) | static_cast<uint64_t>(widgets & 0xFFFF);
    if (shown < 0) return;
    if (g_last_note.exchange(note) != note)
        logf("[F%d] Opponent Outfit Settings: %d outfits, %d summary labels: the summary shows the first %d (the game's own "
             "outfits first); the full list is put back after it", fid, outfits, widgets, shown);
    list->add_ref();
    if (!ui::set_i64(list, "_size", shown)) {
        list->release();
        if (g_refusals.fetch_add(1) < 3) logf("[F%d] Opponent Outfit Settings: the core refused the outfit list's length: "
                                              "the page stays empty (the game's own bug with extra outfits)", fid);
        return;
    }
    g_swap = Swap{ list, outfits, shown, g_frame, GetCurrentThreadId() };
    g_pending.store(true, std::memory_order_release);
    post(EV_OPP_SUMMARY);                                         // the frame puts it back if the draw throws
}

std::atomic<bool> g_lines_dirty{ true };         // the game wrote its lines again: ours are read again

int pre_draw(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int64_t t0 = perf::begin();
    g_lines_dirty.store(true, std::memory_order_relaxed);
    if (argc >= 2 && !g_closing.load(std::memory_order_relaxed)) {
        try { on_draw(reinterpret_cast<Obj*>(argv[1])); } catch (...) { logf("Opponent Outfit Settings: EXCEPTION in the summary hook"); }
    }
    perf::end(perf::HkOppSummaryDraw, t0);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void post_draw(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_pending.load(std::memory_order_acquire)) return;
    const int64_t t0 = perf::begin();
    try {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_swap.list && g_swap.tid == GetCurrentThreadId()) restore_locked(nullptr);
    } catch (...) {
        logf("Opponent Outfit Settings: EXCEPTION in the summary hook (after)");
    }
    perf::end(perf::HkOppSummaryDraw, t0);
}

// ======================== the lines past the game's own ========================
constexpr const char* kAgent = "OtherMatchingFighterSetting";
constexpr const char* kListName = "p_ScrollList_texts_v";
constexpr const char* kLineName = "e_text_costume";
constexpr uint32_t kPollEvery = 15, kRefreshEvery = 30, kCopyAskEvery = 30;
constexpr float kLineStep = 31, kPlaceW = 320, kOffScreen = -4000;   // the list's items are 31 apart, its text's region 320 wide (read 07/11)
constexpr int kRefusedMax = 3;

struct Line { std::string whole, moving, fixed; };
std::vector<Line> g_lines;
int g_shown = 0;                                 // rows given a line at the last frame
bool g_up = false;
uint32_t g_tick = 0, g_read_at = 0, g_asked_at = 0;
float g_x = 0, g_y = 0;                          // the first line of the list, in its parent's units
int g_first = 5;                                 // the list's ItemCount
std::map<std::pair<int, int>, std::string> g_names;   // this page's visit
std::atomic<int> g_need{ 0 };
std::atomic<int> g_copy_refused{ 0 };
int g_lines_logged = -1;

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }
std::string line_name(char kind, int row) {
    char b[32];
    snprintf(b, sizeof b, "e_ss_opp_%c%02d", kind, row);
    return b;
}
// The page on screen: its main control, and its param (the page's type, checked)
bool page_up(Obj*& cm, Obj*& page) {
    void* buf[2] = {};
    const uint32_t n = A()->ui_agents(kAgent, buf, 2);
    for (uint32_t i = 0; i < n && i < 2; ++i) {
        Obj* agent = static_cast<Obj*>(buf[i]);
        Obj* c = call_obj(agent, "get_ControlMain");
        Obj* go = call_obj(agent, "get_GameObject");
        bool ok1 = false, ok2 = false;
        const bool v = c && call_bool(c, "get_ActualVisible", {}, &ok1);
        const bool d = go && call_bool(go, "get_DrawSelf", {}, &ok2);    // a hidden GUI keeps its controls' ActualVisible
        if (!ok1 || !ok2 || !v || !d) continue;
        Obj* deleg = get_obj(agent, "<InputDelegates>k__BackingField");
        Obj* flow = deleg ? get_obj(deleg, "<Flow>k__BackingField") : nullptr;
        Obj* p = flow ? get_obj(flow, "_Param") : nullptr;
        if (!p || p->get_type_definition() != g_page) continue;
        cm = c; page = p;
        return true;
    }
    return false;
}
// Inside the guide list's update (R19): the copies this page lacks, hidden; the frame places and shows them
void make_lines() {
    const SF6CoreAPI* a = A();
    if (!a || !C() || g_copy_refused.load() >= kRefusedMax) return;
    Obj* cm = nullptr;
    Obj* page = nullptr;
    if (!page_up(cm, page)) return;
    Obj* list = static_cast<Obj*>(a->ui_find_cached(cm, kListName));
    Obj* parent = list ? call_obj(list, "get_Parent") : nullptr;
    Obj* item = list ? call_obj(list, "get_Child") : nullptr;
    void* tmpl = item ? a->ui_find_child(item, kLineName, 3) : nullptr;
    if (!parent || !tmpl) return;
    const int need = g_need.load();
    for (int r = 0; r < need; ++r)
        for (const char kind : { 'l', 'r' }) {
            const std::string name = line_name(kind, r);
            if (a->ui_find_child(parent, name.c_str(), 0)) continue;
            void* c = nullptr;
            const SF6CoreStatus st = a->ui_clone_control(C(), tmpl, parent, name.c_str(), &c);
            if (st != SF6CORE_OK || !c) {
                logf("Opponent Outfit Settings: a line's copy refused (core status %d)", static_cast<int>(st));
                g_copy_refused.fetch_add(1);
                return;
            }
            SF6CoreArg m{}; m.type = SF6CORE_ARG_I64; m.v.i = 1;   // via.gui.MaskType.NonTarget: no mask of the new parent clips it
            a->obj_invoke(C(), c, "set_MaskType(via.gui.MaskType)", &m, 1, nullptr);
            SF6CoreArg fit{}; fit.type = SF6CORE_ARG_I64; fit.v.i = 0;   // never squeezed: a long name slides
            a->obj_invoke(C(), c, "set_AutoRegionFit(via.gui.RegionFitType)", &fit, 1, nullptr);
            a->ui_set_visible(C(), c, 0);
        }
}
std::string outfit_name(int fid, int costume) {
    auto it = g_names.find({ fid, costume });
    if (it != g_names.end()) return it->second;
    static Method* m_inv = method_of(api().tdb()->find_type("app.InventoryManager"), "get_Costume", 0);
    static Method* m_name = method_of(api().tdb()->find_type("app.InventoryManager.CostumeInventory"), "GetName", 2);
    Obj* inv = m_inv ? call_obj(m_inv, nullptr) : nullptr;
    std::string name = inv && m_name ? str(call_obj(m_name, inv, { arg_i32(fid), arg_i32(costume) })) : std::string();
    if (name.empty()) name = "#" + std::to_string(costume + 1);
    g_names[{ fid, costume }] = name;
    return name;
}
// What the lines are now: read from the page's own data, after the game's draw
void read_lines(Obj* page, Obj* list) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_swap.list) return;                     // the list's length isn't the full one until the draw is over
    Obj* outfits = obj_at(obj_at(page, f_dup), f_cos);
    int32_t fid = -1, shown_cos = -1;
    if (!outfits || !i32_at(page, f_fid, fid)) return;
    const Ret rc = call(page, "get_SettingOtherCostumeId");
    if (!rc.exception_thrown) shown_cos = static_cast<int32_t>(rc.dword);
    const Ret rn = call(list, "get_ItemCount");
    if (!rn.exception_thrown && static_cast<int32_t>(rn.dword) > 0) g_first = static_cast<int32_t>(rn.dword);
    float p[3] = {};
    const Ret rp = call(list, "get_Position");
    if (!rp.exception_thrown) { memcpy(p, rp.bytes.data(), sizeof p); g_x = p[0]; g_y = p[1]; }
    const int n = list_count(outfits);
    struct Row { int costume = 0, count = 0; };
    std::vector<Row> rows;
    int current = -1;
    for (int i = 0; i < n; ++i) {
        Obj* o = list_item(outfits, i);
        Row row;
        if (o) { get_i32(o, "CostumeId", row.costume); row.count = list_count(get_obj(o, "ValidColorIdList")) + colormod::opponent_ticks(fid, row.costume); }
        if (row.costume == shown_cos && i >= g_first) current = i - g_first;
        rows.push_back(row);
    }
    LineFormat fmt;
    void* item = A()->ui_find_child(list, "c_item", 0);    // the first line: the list's items come last first (read 07/11)
    void* first = item ? A()->ui_find_child(item, kLineName, 3) : nullptr;
    if (first && !rows.empty())
        fmt = line_format(str(call_obj(static_cast<Obj*>(first), "get_Message")), outfit_name(fid, rows[0].costume), rows[0].count);
    const int extras = n - g_first;
    const LineWindow w = line_window(extras, kOwnLinesMax, current);
    std::vector<Line> lines;
    const auto more = [&](int count) { lines.push_back({ words::trf(words::W::ReplayMore, { std::to_string(count) }), std::string(), std::string() }); };
    if (w.more_above) more(w.more_above);
    for (int i = 0; i < w.count; ++i) {
        const Row& row = rows[static_cast<size_t>(g_first + w.first + i)];
        Line l;
        l.moving = fmt.before + outfit_name(fid, row.costume);
        l.fixed = fmt.sep + std::to_string(row.count);
        l.whole = l.moving + l.fixed;
        lines.push_back(l);
    }
    if (w.more_below) more(w.more_below);
    g_lines = std::move(lines);
    g_need.store(static_cast<int>(g_lines.size()));
    if (static_cast<int>(g_lines.size()) != g_lines_logged) {
        g_lines_logged = static_cast<int>(g_lines.size());
        logf("[F%d] Opponent Outfit Settings: %d outfits, %d shown by the game, %d lines of ours", fid, n, g_first, g_lines_logged);
    }
}
// One row: one text while the line fits; too long, the name in that text, sliding in a window that leaves room for the count, in a second text. Arabic stays one
// text: the order of a split line there can't be checked. (As Replays > Settings' lines, the other way round.)
void put_line(void* t, void* v, const Line& l, float x, float y) {
    const SF6CoreAPI* a = A();
    const bool can_split = !l.fixed.empty() && words::lang() != std::string("ar");
    const float whole = can_split ? namescroll::known_width(t, l.whole) : -1;
    const bool wide = whole > 0 && scroll::overflows(whole, kPlaceW);
    const float tail = wide ? namescroll::known_width(v, l.fixed) : -1;
    const float window = tail > 0 ? kPlaceW - tail : 0;
    const bool split = window >= scroll::kSplitMinWindow;
    const std::string& in_t = split ? l.moving : l.whole;
    namescroll::release_unless(t, in_t);
    if (!namescroll::busy(t, x, y)) {
        a->ui_set_text(C(), t, in_t.c_str());
        a->ui_set_position(C(), t, x, y, 0);
    }
    a->ui_set_visible(C(), t, 1);
    namescroll::drive(t, split ? window : kPlaceW);
    const bool tail_up = wide && (tail < 0 || split);
    if (tail_up) {
        const float vx = split ? x + window : kOffScreen;
        namescroll::release_unless(v, l.fixed);
        if (!namescroll::busy(v, vx, y)) {
            a->ui_set_text(C(), v, l.fixed.c_str());
            a->ui_set_position(C(), v, vx, y, 0);
        }
    }
    a->ui_set_visible(C(), v, tail_up ? 1 : 0);
    namescroll::drive(v, kPlaceW);
}

// ======================== R18: the code check, as the core's (core/src/pausemenu/pausemenu.cpp) ========================

// The code an entry runs: its jump thunks followed (jmp rel32 / rel8 / jmp [rip+disp32]), as REFramework's add_hook does
uintptr_t code_of(uintptr_t p) {
    __try {
        for (int hop = 0; hop < 4 && p; ++hop) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
            if (b[0] == 0xE9) p = p + 5 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 1);
            else if (b[0] == 0xEB) p = p + 2 + (intptr_t) * reinterpret_cast<const int8_t*>(b + 1);
            else if (b[0] == 0xFF && b[1] == 0x25) p = *reinterpret_cast<const uintptr_t*>(p + 6 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 2));
            else break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return p;
}

uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
uintptr_t exe_size() {
    const auto* b = reinterpret_cast<const uint8_t*>(exe_base());
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew)->OptionalHeader.SizeOfImage;
}
long long rva(uintptr_t a) { return a ? static_cast<long long>(a - exe_base()) : 0; }
long long code_rva(uintptr_t a) {   // outside the exe (another module's hook): -1
    if (!a) return 0;
    return a >= exe_base() && a - exe_base() < exe_size() ? static_cast<long long>(a - exe_base()) : -1;
}

std::string exe_key(uint32_t methods) {
    const auto* base = reinterpret_cast<const uint8_t*>(exe_base());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    unsigned long long size = 0, time = 0;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        time = (static_cast<unsigned long long>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    }
    char b[200];
    snprintf(b, sizeof b, "exe %08lX %08lX %08lX size %llu time %llX methods %u", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
             static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage), static_cast<unsigned long>(nt->OptionalHeader.CheckSum), size, time, methods);
    return b;
}

// Every method of the game: those entering the method's entry or code. False when the game closed meanwhile.
bool scan(int& shared, long long& other, uint32_t& n) {
    auto* tdb = api().tdb();
    n = tdb->get_num_methods();
    for (uint32_t i = 0; i < n; ++i) {
        if ((i & 0xFFFF) == 0 && g_closing.load(std::memory_order_relaxed)) return false;
        Method* m = tdb->get_method(i);
        if (!m) continue;
        const uintptr_t fn = reinterpret_cast<uintptr_t>(m->get_function_raw());
        if (!fn) continue;
        bool same = fn == g_raw || fn == g_code;
        if (!same) { const uintptr_t c = code_of(fn); same = c && c == g_code; }
        if (!same || m->get_index() == g_now.index) continue;
        if (shared++ == 0) other = m->get_index();
    }
    return true;
}
bool scan_seh(int* shared, long long* other, uint32_t* n) {
    __try { return scan(*shared, *other, *n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

DWORD WINAPI check_thread(void*) {
    const ULONGLONG t0 = GetTickCount64();
    int shared = 0;
    long long other = -1;
    uint32_t n = 0;
    const bool ok = scan_seh(&shared, &other, &n);
    g_scan_ms = GetTickCount64() - t0;
    g_scanned = n;
    if (!ok) { g_verdict.store(kFailed, std::memory_order_release); return 0; }
    g_found = g_now;
    g_found.shared = shared;
    g_found.other = other;
    g_cache_written = write_file(kCacheFile, cache_text(g_key, g_label, g_found));
    g_verdict.store(shared == 0 ? kUnique : kShared, std::memory_order_release);
    return 0;
}

std::string method_label(long long index) {
    Method* m = index >= 0 ? api().tdb()->get_method(static_cast<uint32_t>(index)) : nullptr;
    if (!m) return "?";
    TD* t = m->get_declaring_type();
    return (t ? t->get_full_name() : std::string("?")) + "." + (m->get_name() ? m->get_name() : "?");
}

}  // namespace

void install_hooks() {
    auto* tdb = api().tdb();
    g_page = tdb->find_type(kType);
    g_m = method_of(g_page, kMethod, 0);
    TD* save = tdb->find_type("app.OtherFighterSettingSaveData");
    f_dup = g_page ? g_page->find_field("<SettingDataDuplicated>k__BackingField") : nullptr;
    f_dup_init = g_page ? g_page->find_field("<SettingDataDuplicated_Initial>k__BackingField") : nullptr;
    f_fid = g_page ? g_page->find_field("SettingFighterId") : nullptr;
    f_sum = g_page ? g_page->find_field("mCosColorTextList") : nullptr;
    f_cos = save ? save->find_field("CostumeSettings") : nullptr;
    TD* list = f_cos ? f_cos->get_type() : nullptr;
    f_size = list ? list->find_field("_size") : nullptr;
    m_len = method_of(tdb->find_type("System.Array"), "get_Length", 0);
    if (!g_m || !g_m->get_function_raw() || !f_dup || !f_fid || !f_sum || !f_cos || !f_size || !m_len) {
        logf("Opponent Outfit Settings: %s.%s or its fields not found (game update?): no summary fix", kType, kMethod);
        g_m = nullptr;
        return;
    }
    g_now.index = g_m->get_index();
    g_raw = reinterpret_cast<uintptr_t>(g_m->get_function_raw());
    g_code = code_of(g_raw);
    g_now.raw = rva(g_raw);
    g_now.code = code_rva(g_code);
    g_key = exe_key(tdb->get_num_methods());
    g_label = std::string(kType) + "." + kMethod;
    if (cache_hit(read_file(kCacheFile, 4096), g_key, g_label, g_now, g_found, &g_cache_why)) {
        g_from_cache = true;
        g_verdict.store(g_found.shared == 0 ? kUnique : kShared, std::memory_order_release);
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, check_thread, nullptr, 0, nullptr);
    if (!h) {
        logf("Opponent Outfit Settings: code check (R18) thread not created: no hook (never hooked blind)");
        g_verdict.store(kFailed, std::memory_order_release);
        return;
    }
    CloseHandle(h);
    logf("Opponent Outfit Settings: code check (R18) of %s started on a thread of its own (%s)", g_label.c_str(), g_cache_why);
}

bool busy() {
    return (g_m && !g_hook_done) || g_pending.load(std::memory_order_acquire);
}

void frame() {
    if (!g_m) return;
    ++g_tick;
    if (!g_up && g_tick % kPollEvery != 0) return;
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, ui_agents) || !SF6CORE_HAS(a, ui_find_cached) || !SF6CORE_HAS(a, ui_clone_control)) return;
    try {
        Obj* cm = nullptr;
        Obj* page = nullptr;
        if (!page_up(cm, page)) {
            if (g_up) { g_up = false; g_lines.clear(); g_names.clear(); g_shown = 0; g_lines_logged = -1; g_need.store(0); g_lines_dirty.store(true); }
            return;
        }
        g_up = true;
        Obj* list = static_cast<Obj*>(a->ui_find_cached(cm, kListName));
        Obj* parent = list ? call_obj(list, "get_Parent") : nullptr;
        if (!parent) return;
        if (g_lines_dirty.exchange(false) || g_tick - g_read_at >= kRefreshEvery) {
            g_read_at = g_tick;
            read_lines(page, list);
        }
        const int count = static_cast<int>(g_lines.size());
        bool missing = false;
        for (int r = 0; r < count || r < g_shown; ++r) {
            void* t = a->ui_find_cached(parent, line_name('l', r).c_str());
            void* v = a->ui_find_cached(parent, line_name('r', r).c_str());
            if (r >= count) {
                if (t) a->ui_set_visible(C(), t, 0);
                if (v) a->ui_set_visible(C(), v, 0);
                if (t) namescroll::drive(t);
                if (v) namescroll::drive(v);
                continue;
            }
            if (!t || !v) { missing = true; continue; }
            put_line(t, v, g_lines[static_cast<size_t>(r)], g_x, g_y + kLineStep * static_cast<float>(g_first + r));
        }
        g_shown = count;
        if (missing && g_tick - g_asked_at >= kCopyAskEvery && g_copy_refused.load() < kRefusedMax) {
            g_asked_at = g_tick;
            fsname::run_in_guide_update(make_lines);
        }
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logf("Opponent Outfit Settings: EXCEPTION in the summary's lines"); }
    }
}

void tick(uint32_t /*events*/) {
    if (!g_m) return;
    if (!g_hook_done) {
        const int v = g_verdict.load(std::memory_order_acquire);
        if (v == kPending) return;
        g_hook_done = true;
        char where[64];
        snprintf(where, sizeof where, "at 0x%llX (code 0x%llX)", static_cast<unsigned long long>(g_raw), static_cast<unsigned long long>(g_code));
        if (v == kFailed) {
            logf("Opponent Outfit Settings: code check (R18) of %s not done (%u methods, %llu ms): no hook", g_label.c_str(),
                 g_scanned, static_cast<unsigned long long>(g_scan_ms));
            return;
        }
        if (g_from_cache) logf("Opponent Outfit Settings: code check (R18): read from %s (same game executable)", kCacheFile);
        else logf("Opponent Outfit Settings: code check (R18): %u methods compared in %llu ms on a thread of its own; cache %s",
                  g_scanned, static_cast<unsigned long long>(g_scan_ms), g_cache_written ? "written" : "NOT written (file error)");
        if (v == kShared) {
            logf("hook %s %s: REFUSED (R18): %d other method(s) enter its code, e.g. %s: a fighter with more outfits than "
                 "summary labels still gets an empty Opponent Outfit Settings page", g_label.c_str(), where, g_found.shared,
                 method_label(g_found.other).c_str());
            return;
        }
        if (g_m->add_hook(pre_draw, post_draw, false)) logf("hook %s %s: installed (its own code)", g_label.c_str(), where);
        else logf("hook %s %s: add_hook refused: no summary fix", g_label.c_str(), where);
    }
    if (g_pending.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_swap.list && g_frame - g_swap.frame >= 2) restore_locked("its draw threw before its end: put back by the frame");
    }
}

}  // namespace slots::oppsum
