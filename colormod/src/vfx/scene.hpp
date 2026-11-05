// vfx::scene - what the Drive VFX code walks in the game (phase 2 step 3, sub-steps C-D, docs/CPP_PORT_PHASE2_STEP3.md):
// EffectManager.DataInstanceMap (the loaded effect providers), a provider's components / EPV elements / extern
// parameters / resource holders, the fighters of a fight (gBattle) and SEH-guarded raw reads. Same access pattern as
// the Lua (SF6_DIRecolor each_extern / each_element, SF6_DriveRecolor enumerate_bases, func/EfxMeshColor): get_Count /
// get_Item on the lists, the components array, the same fields and methods. Game thread only.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ll_common.hpp"          // LL's helpers: fields, invoke, managed strings
#include "memguard.hpp"

namespace vfx::scene {

using MO = reframework::API::ManagedObject;

bool init();                                             // types, methods, field offsets; false = the VFX tick stays off

// ---- raw reads (SEH + page guard: an unmapped address reads as false, never a fault) ----
// A page cache kept across the frames of a pass (the caller ages it), for the reads below on this thread; nullptr =
// this frame's answers only (memguard::state). One VirtualQuery can cost 1-2 ms (huge heap regions, 28/10 perf logs):
// asked once per region per pass instead of once per region per frame. Only pages found readable are kept.
void set_read_cache(memguard::Held* h);
bool rd32(uint64_t a, uint32_t& out);
bool rd64(uint64_t a, uint64_t& out);
bool rd8(uint64_t a, uint8_t& out);
std::vector<uint8_t> cstr(uint64_t a, size_t maxn);      // bytes until NUL (EfxMeshColor cstr)
bool wr8(uint64_t a, uint8_t v);                         // static-path writes on live managed objects (Lua write_byte / write_dword)
bool wr32(uint64_t a, uint32_t v);

// ---- fighters ----
int battle_char(int idx);                                // gBattle.Player.mcPlayer[idx].mpActParam.ActionPart._CharaID, 0 = none
int side_char(int idx);                                  // Lua read_side_char: battle, then the TrainingManager select data
// SF6_DriveRecolor act_of: gBattle.Player.mcPlayer[idx].mpActParam.ActionPart._Engine:get_ActionID(); false = none
bool battle_act(int idx, int64_t& out);

// ---- DataInstanceMap ----
struct Entry { std::string key; MO* go; };               // key = the provider path, go = value.Instance (may be null)
std::vector<Entry> entries();                            // in map order (the Lua's get_elements order)

// ---- a provider's parts (callbacks get borrowed objects, valid during the call on the game thread) ----
// fn(el, idx): every EPV element of every component except via.Transform; idx restarts at 0 per component (Lua j)
void each_element(MO* go, const std::function<void(MO* el, int idx)>& fn);
// fn(param, el_idx, p_idx, name, resource_name): every extern parameter of every element
void each_extern(MO* go, const std::function<void(MO* param, int el, int pi, const std::string& name, const std::string& rn)>& fn);
// fn(param, p_idx, name, resource_name): the extern parameters of one element (each_extern's inner loop)
void each_param(MO* el, const std::function<void(MO* param, int pi, const std::string& name, const std::string& rn)>& fn);
// fn(holder, path): every resource holder of an element (getResourcesCount / getResources / get_ResourcePath)
void each_resource(MO* el, const std::function<void(MO* holder, const std::string& path)>& fn);
bool element_has_color(MO* el);                          // an extern named Color / SubColor / AColor (DI SLOT_OF)

// each_element one slice at a time: the same elements in the same order, resumed where the cursor stopped. fn returns
// false to stop after that element (frame budget). Returns 1 = every element visited, 0 = stopped (call again with the
// same cursor), -1 = the provider's component array or the current element list changed since the walk started (the
// caller starts over). Only the lists are re-read at each call (get_Components, Elements, get_Count), never kept.
struct WalkCursor { bool started = false, in_comp = false; uint32_t ncomp = 0, comp = 0; int32_t item = 0, nitem = 0; MO* comp_obj = nullptr; MO* list = nullptr; MO* again = nullptr; };
int walk_elements(MO* go, WalkCursor& c, const std::function<bool(MO* el, int idx)>& fn);
// the same walk, fn returning 1 = go on, 0 = stop after this element, 2 = stop and give this element again at the next
// call (its work is not finished; the walk checks it is still the same element there, else -1)
int walk_elements_ex(MO* go, WalkCursor& c, const std::function<int(MO* el, int idx)>& fn);
// each_resource one holder at a time: the count (-1 = getResourcesCount raised: no resources), then holder i (false =
// a null holder, skipped)
int resource_count(MO* el);
bool resource_at(MO* el, int i, MO*& h, std::string& path);

// EPVStandardData.Element.Color / ColorEnable offsets from the object (0 = unknown)
uint32_t off_tint();
uint32_t off_enable();

inline uint64_t addr(MO* o) { return reinterpret_cast<uint64_t>(o); }
std::string lower(std::string s);

}  // namespace vfx::scene
