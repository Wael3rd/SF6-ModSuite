// screens::slots_panel — the character select's panels shared with the Slots Loader (SF6_CostumeSlotsNative.dll).
//
// The Slots Loader's costume options mode (R1 / C in a side's panel) borrows the same four rows of a side's panel as
// our DI mode (X). Two writers on one UI crashed the game before: ONE owner per panel, kept by the Slots Loader
// (SF6Slots_SelectPanelClaim). DI mode takes the claim before it borrows the rows and gives it back after; X is
// refused while the Slots Loader holds the panel. Without that DLL (or an older one without the export) there is
// nothing to share: take() always grants. The Slots Loader reads SF6ColorMod_DiPanelActive (main.cpp) to free a claim
// we left behind and to know we take part in the claim at all (an older Color Mod turns its mode off).
#pragma once

#include <string>

namespace screens::slots_panel {

bool take(int side);                  // DI mode may borrow side's select panel (1 / 2): granted, or no claim to ask
void release(int side);               // given back (only ours)
bool slots_holds(int side);           // the Slots Loader's costume options show in side's panel now
// The Slots Loader's guide entry for side's corner ("<ICON BtnR1> Costume options", the keyboard glyph when !pad);
// "" when that side's outfit has no options or no such export. Each call also keeps the entry fresh.
std::string guide_entry(int side, bool pad);

}  // namespace screens::slots_panel
