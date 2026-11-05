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
#include "perf.hpp"

#include <mutex>

namespace slots::oppsum {
namespace {

constexpr const char* kType = "app.UIFlowOtherMatchingFighterSetting.UIParam";
constexpr const char* kMethod = "UpdateCosColorTextList";
constexpr const char* kCacheFile = "reframework/data/SF6_CostumeSlots_data/codecheck.txt";

TD* g_page = nullptr;                              // the page's type
Method* g_m = nullptr;                             // UpdateCosColorTextList()
API::Field *f_dup = nullptr, *f_fid = nullptr, *f_sum = nullptr, *f_cos = nullptr, *f_size = nullptr;
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

void on_draw(Obj* page) {
    if (!page || page->get_type_definition() != g_page) return;   // the page's own call only
    std::lock_guard<std::mutex> lk(g_mx);
    restore_locked("the last summary draw threw before its end");
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

int pre_draw(int argc, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    const int64_t t0 = perf::begin();
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
