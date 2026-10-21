#include "slots_panel.hpp"

#include <windows.h>

#include "../common.hpp"

namespace screens::slots_panel {
namespace {

constexpr int kWhoDi = 2;                              // the Slots Loader's numbering: 1 its options, 2 our DI mode
using ClaimFn = int (*)(int side, int who, int op);
using EntryFn = int (*)(int side, int pad, char* buf, int cap);
using GuardFn = int (*)();
ClaimFn g_claim = nullptr;
EntryFn g_entry = nullptr;
GuardFn g_guard = nullptr;
bool g_looked = false;

void look() {                                          // every plugin is loaded before the first frame: asked once
    if (g_looked) return;
    g_looked = true;
    HMODULE h = GetModuleHandleW(L"SF6_CostumeSlotsNative.dll");
    if (!h) return;
    g_claim = reinterpret_cast<ClaimFn>(GetProcAddress(h, "SF6Slots_SelectPanelClaim"));
    g_entry = reinterpret_cast<EntryFn>(GetProcAddress(h, "SF6Slots_SelectGuideEntry"));
    g_guard = reinterpret_cast<GuardFn>(GetProcAddress(h, "SF6Slots_PopupGuard"));
    cmn::journal("slots panel: the Slots Loader's panel claim %s, its select guide entry %s, its popup guard %s",
                 g_claim ? "found (one owner per panel)" : "absent", g_entry ? "found" : "absent", g_guard ? "found" : "absent");
}

}  // namespace

bool take(int side) {
    look();
    if (!g_claim) return true;
    return g_claim(side, kWhoDi, 1) != 0;
}

void release(int side) {
    look();
    if (g_claim) g_claim(side, kWhoDi, 0);
}

bool slots_holds(int side) {
    look();
    return g_claim && g_claim(side, kWhoDi, -1) == 1;
}

std::string guide_entry(int side, bool pad) {
    look();
    if (!g_entry) return {};
    char buf[160] = {};
    const int n = g_entry(side, pad ? 1 : 0, buf, (int)sizeof buf);
    return n > 0 && n < (int)sizeof buf ? std::string(buf, (size_t)n) : std::string();
}

bool popup_guard() {
    look();
    return g_guard && g_guard() != 0;
}

}  // namespace screens::slots_panel
