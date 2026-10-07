// screens::guide - guide hints and corner_tick ported to native hooks (phase 3
// sub-step 3d, docs/CPP_PORT_PHASE3.md). This is the most crash-sensitive part
// of the mod (02/10 regression): ported 1:1 from the Lua code.
//
// The Lua keeps the business logic (when to show hints, what text, which screen
// is active). The DLL installs hooks on:
//   - UIPartsInputGuideBase.NewInputItem (6 overloads): lower raised templates
//     before cloning, raise them after (deterministic fix for the one-frame
//     artefact, Wael 04/09)
//   - UIPartsInputGuideList.OnLateUpdate: rewrite guide bar texts (suffix append)
//   - UIFlowOptionBGDialog.Param.UpdateInputGuide: editor guide bar fix
// and runs corner_tick every frame (stale template restore).
//
// Owner "screens_guide": native = hooks + corner_tick in the DLL; lua = everything
// in the Lua as before. SEH + fault counter: 5 faults switch the module off.
#pragma once

#include <cstdint>
#include <string>

namespace screens::guide {

bool install();                         // hook all methods; returns false on hard failure
bool ready();                           // hooks in place and not switched off by faults

// ---- Lua -> DLL (push) --------------------------------------------------

// Set a corner prompt on a template (guide_corner in the Lua). The Lua calls
// this every frame for each template it drives. addr = template address,
// btn_addr = parent c_guideBtn address, text = what to show (empty = clear),
// x/y = position, show = whether the template should be visible.
void set_corner(uint64_t addr, uint64_t btn_addr, const std::string& text,
                float x, float y, bool show);

// Release a corner: the Lua stopped driving that template (guide_corner with
// show=false already calls this). The DLL restores it on the next corner_tick.
void release_corner(uint64_t addr);

// Set the full text to write on a guide bar's text element after OnLateUpdate.
// part_addr = UIPartsInputGuideList address (for hook filtering). text_addr =
// text child address. ctrl_addr = parent control. right = right-hand bar.
// want = the complete text (game base + our suffix). Empty want = no suffix.
void set_fix(uint64_t part_addr, uint64_t text_addr, uint64_t ctrl_addr,
             bool right, const std::string& want);

// Clear all guide fix entries (called when the guide list is re-resolved).
void clear_fixes();

// Set the editor guide bar text. want = full hint text (empty = no bar fix).
// desc = description line below. param_addr = the Param address for filtering.
void set_bar(const std::string& want, const std::string& desc,
             uint64_t param_addr);

// State flags the hooks need (pushed by the Lua each frame).
void set_flags(bool fsm_visible, bool tcs_guide);

// ---- frame callback -----------------------------------------------------

void on_frame();                        // corner_tick: restore stale templates

std::string status();

}  // namespace screens::guide
