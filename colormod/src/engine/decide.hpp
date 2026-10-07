// The colour engine's decisions, native (feature "decisions", docs/CPP_PORT_PLAN.md phase 1 step 3).
//
// Port of what SF6_ColorSpinExtra.lua decides around the patch (lines ~630-1850 and squares ~2495):
//   - the RW / MC entries appended to ColorInventory.GetColorList (labels, extra_first / extra_count);
//   - on PlayerColorController.SetupColor: which side a controller is (ctrl_side, side_from_battle, the P2 marker),
//     what it should wear (wanted_entry, resolve_tree: entry file, its override, a game-colour override) and the
//     patch itself through engine::live (applied signatures, setups counters, worn trees);
//   - SetColor: P2_VIRTUAL_ID -> BASE_COLOR and the P2 marker;
//   - the mirror rule (GuiEsportsManager.setTempCosColData / setTempColor, FighterDesc[1].Color, P2 repush);
//   - the battle pass every 30 frames, the P2 colour poll every 3 frames;
//   - squares(): the two chip colours a side wears (DI / Drive "Follow").
// The Lua screens stay (phase 3): they push what the decisions read from them (the colour menus' Param and
// whether their cursor is live, the Fighter Settings spin text, the three selections of state.json) and pull
// what they read from here (list_args, extra_first / extra_count, setups, the mirror rule clearing P2's choice).
// Their own hooks stay installed but stand by while owns("decisions") is true; if this module switches itself
// off (faults), owns() turns false and the Lua takes over at the next frame.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace engine::decide {

bool install();                  // hooks, catalogue, state.json; false = types missing, the feature stays off
bool ready();                    // installed and not switched off by faults
bool active();                   // ready, and the settings give this DLL both "decisions" and "engine"

void on_setup_pre(void* ctrl);   // from engine::live's SetupColor pre-hook, lock held, after the stale restore
std::string applied_sig(void* ctrl);   // what that controller wears ("k:3", "ov:2", "edit"; "" = nothing), lock held
void on_frame();                 // core frame callback (LateUpdateBehavior): P2 poll, battle pass, P2 repush

// ---- Lua bridge (screens -> decisions) ----
// the colour menus' Param (0 = none) and whether their cursor is live (Lua: m.text and m.active and not
// m.restore_pending); the Fighter Settings spin text (0 = closed)
void push_ui(uint64_t p1_param, bool p1_live, uint64_t p2_param, bool p2_live, uint64_t fs_text);
// state.p1 / state.p2 / state.fs: { rw, fighter, costume } x 3 (fighter / costume < 0 = nil)
void push_sels(const int v[9]);
struct Pull {
    bool list_known = false; uint32_t list_fighter = 0, list_costume = 0;   // list_args (nil until a GetColorList)
    int extra_first = -1; int extra_count = 0;                             // extra_first -1 = nil
    uint64_t setups[3] = {};                                               // dbg.setups[1..3]
    uint64_t mirror_clears = 0;                                            // P2's selection cleared by the mirror rule
};
Pull pull();
int rebuild_catalog();           // after the Lua rebuilt its own (SAVE AS, ERASE, script load): lists count
std::optional<std::pair<uint32_t, uint32_t>> squares(int fighter_id, int only_side);   // only_side 0 = any
std::string follow_debug(int fighter_id);                                  // lines, '\n'-separated
std::string status();
// the player's own side (1 / 2) from the battle description: online our session member, offline the first fighter on
// a pad; 1 when unknown. The light fight mode gives it the effect data both players share.
int own_side();

}  // namespace engine::decide
