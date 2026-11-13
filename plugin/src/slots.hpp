// SF6 Slots — native REFramework plugin, shared declarations.
//
// The game-side part of the slots, in one plugin: costume menus, save data, ownership, colours and
// the select screen UI, the online alias of a slot on its carrier outfit (costumes.cpp,
// carrier.hpp), the stage select screen and the VS screen of the stage slots (stages.cpp). It
// replaces the two Lua scripts.
//
// Driven by events, never by polling. Hooks on the game's own methods (a menu shown or closed, the
// select screen started, a stage focused, an outfit folder mounted) record what happened in
// atomics; the work is done on the next LateUpdateBehavior, on the game thread, and stops when it is
// done. Nothing in a fight triggers anything, so a fight costs one check of this plugin's own memory
// per frame: no read of the game. The only work not started by an event is at boot, once.
//
// Rules this code follows (each one cost a crash once):
//   - Game objects are touched only from LateUpdateBehavior. Hooks may be called on any thread:
//     they only store numbers.
//   - Objects kept across frames are checked with is_managed_object before use.
//   - A GUI text is written only when it differs, a texture only once it is loaded.
#pragma once

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma warning(push)
#pragma warning(disable: 4267 4189)   // REFramework's header: size_t -> unsigned, an unused local
#include <reframework/API.hpp>
#pragma warning(pop)

#include "json.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace slots {

using API    = reframework::API;
using Obj    = API::ManagedObject;
using TD     = API::TypeDefinition;
using Method = API::Method;
using Ret    = reframework::InvokeRet;

inline API& api() { return *API::get(); }

// ---- log: reframework/data/SF6_CostumeSlots_data/native_log.txt ----
void log_open();
void logf(const char* fmt, ...);

// ---- frame counter (LateUpdateBehavior calls) ----
extern uint32_t g_frame;

// ---- closing guard: set by the window message callback (WM_CLOSE etc.) ----
extern std::atomic<bool> g_closing;

// ---- events: set by hooks (any thread), taken on the game thread ----
enum Event : uint32_t {
    EV_MENU_OPEN     = 1u << 0,   // Battle Settings shown
    EV_MENU_CLOSE    = 1u << 1,   // Battle Settings closed
    EV_SELECT_START  = 1u << 2,   // character select screen started
    EV_SELECT_END    = 1u << 3,   // character select screen ended
    EV_HOLDER        = 1u << 4,   // an outfit's visual manifest was created: the game mounted its folder
    EV_STAGE_SHOW    = 1u << 5,   // stage select screen shown
    EV_STAGE_HIDE    = 1u << 6,   // stage select screen hidden
    EV_STAGE_DIRTY   = 1u << 7,   // focus changed, or the game rewrote the name / the preview
    EV_STAGE_INPUT   = 1u << 8,   // UP / DOWN on the stage select screen
    EV_VS            = 1u << 9,   // VS screen shown or rewritten by the game
    EV_TITLE_END     = 1u << 10,  // title screen left (the game clears DLC ownership at login)
    EV_RTO_MODEL     = 1u << 11,  // a fighter's model was built (runtime costume options, rto.cpp)
    EV_POPUP         = 1u << 12,  // the Training pause menu's character popup built or updated (rto_select.cpp)
    EV_FIGHTER_LOAD  = 1u << 13,  // the game asked for fighters' files or builds fighters from a description (costumes.cpp)
    EV_OPP_SUMMARY   = 1u << 14,  // Opponent Outfit Settings' summary drawn with a shorter outfit list (opp_summary.cpp)
    EV_RTO_REPLAY    = 1u << 16,  // Replays > Settings: a replay's forced options set, or its preview on (rto.cpp)
    EV_STAGE_GRID    = 1u << 15,  // a stage grid built, refreshed, focused, decided or a cycle key pressed (stages.cpp)
    EV_DIAG          = 1u << 31,  // a method watched for the log was called
};
// Methods watched for the log only: which of them the game calls, and when
void diag_watch(TD* td, const char* method);
void diag_flush();
extern std::atomic<uint32_t> g_events;
inline void post(uint32_t e) { g_events.fetch_or(e, std::memory_order_release); }

// ---- fields ----
API::Field* field_of(TD* td, const char* name);            // walks the parents
void* field_ptr(Obj* o, const char* name);                   // address of the field in the object
Obj* get_obj(Obj* o, const char* name);
bool get_i32(Obj* o, const char* name, int32_t& out);
bool get_u32(Obj* o, const char* name, uint32_t& out);
bool get_bool(Obj* o, const char* name, bool& out);
bool set_i32(Obj* o, const char* name, int32_t v);
bool set_u32(Obj* o, const char* name, uint32_t v);
bool set_bool(Obj* o, const char* name, bool v);
bool set_obj(Obj* o, const char* name, Obj* v);
inline bool alive(Obj* o) { return o && o->is_managed_object(); }

// ---- methods ----
Method* method_of(TD* td, const char* name, int nparams = -1);   // exact signature, else bare name
Ret call(Method* m, Obj* o, std::vector<void*> args = {});
Ret call(Obj* o, const char* name, std::vector<void*> args = {}); // looked up on the object's type
Obj* call_obj(Method* m, Obj* o, std::vector<void*> args = {});
Obj* call_obj(Obj* o, const char* name, std::vector<void*> args = {});
bool call_bool(Obj* o, const char* name, std::vector<void*> args = {}, bool* ok = nullptr);
inline void* arg_i32(int32_t v) { return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(v))); }
inline void* arg_bool(bool v)    { return reinterpret_cast<void*>(static_cast<uintptr_t>(v ? 1u : 0u)); }
inline void* arg_obj(Obj* o)     { return reinterpret_cast<void*>(o); }

// ---- collections ----
int list_count(Obj* list);
Obj* list_item(Obj* list, int i);
// The i-th element of a list whose elements may be objects or structs (a struct comes back by value)
struct Elem {
    Obj* obj = nullptr;
    TD* vt = nullptr;                    // set when the element is a struct
    Ret ret{};
    bool ok = false;
    void* field(const char* name) const;
    Obj* obj_field(const char* name) const { void* p = field(name); return p ? *reinterpret_cast<Obj**>(p) : nullptr; }
    bool u32_field(const char* name, uint32_t& out) const { void* p = field(name); if (!p) return false; memcpy(&out, p, 4); return true; }
};
Elem list_elem(Obj* list, int i);

// ---- strings ----
std::string str(Obj* s);                          // System.String -> UTF-8
std::string game_language();                      // the game's text language: "en", "fr", ... ("en" when not read). Game thread.
std::string game_message(const uint8_t guid[16]); // the game's own message, in the game's language ("" not found). Game thread.
void language_tick();                             // each frame: the language of words.hpp, read at most every 60 frames. Game thread.
Obj* new_string(const std::string& utf8);         // UTF-8 -> System.String
std::string type_name(Obj* o);

// ---- scene ----
Obj* current_scene();
Obj* find_components(TD* type);                   // scene:findComponents(System.Type)

// ---- hooks (REFramework plugin API) ----
int  hook_pass(int argc, void** argv, REFrameworkTypeDefinitionHandle* arg_tys, unsigned long long ret_addr);
void hook_post_pass(void** ret_val, REFrameworkTypeDefinitionHandle ret_ty, unsigned long long ret_addr);
// every method of the type (and its parents) with this name gets the hook; returns how many
int hook_all(TD* td, const char* method, REFPreHookFn pre, REFPostHookFn post);

// ---- files ----
std::string read_file(const char* path, size_t max_size = 4u << 20);
bool write_file(const char* path, const std::string& data);   // through a temporary file

// ---- JSON: json.hpp ----

// ---- the plugin's additions to the game's menus, each of which the Slotsloader Manager's player can leave out ----
// (main.cpp: Manager mode only, a key of settings.json each; installed any other way every one is shown)
namespace menus {
    enum Item {
        OutfitNames,        // the mod's name in the Costume rows (the loader's text), long names sliding
        FighterName,        // the outfit's name on Fighter Settings
        NsfwRow,            // Options > Interface: Display NSFW Outfits (Manager mode: never added, no key)
        OnlineOutfitRow,    // Options > Interface: Outfit Others See
        TitleScreen,        // Options > Interface: Title Screen Selection, Skip intro screens
        InterfacePacks,     // Options > Interface: a section per interface mod
        ReplaySettings,     // the Replays menu's Settings tab
        kItems
    };
    bool shown(Item item);
}

// ---- modules: install their hooks once, then work on events ----
namespace costumes {
    void init();                      // registry and state files, before the first frame
    bool manager_mode();              // the Slotsloader Manager installed this loader (common/manager_mode.hpp), as init() read it
    void install_hooks();             // first frame (the type database is ready)
    bool busy();                      // work left without a new event (boot, a mount in progress)
    void tick(uint32_t events);
    bool online();                    // an online battle is loaded or played (its description says so)
    bool select_active();             // the character select screen is open (read now, not watched)
    int battle_mode();                // the battle description's app.EGameMode (2 Training), -1 none (read now)
    // the last fighters were built from an offline Training description (app.EGameMode 2: Training, a Custom Room's
    // training area) and no fighter files were asked for since (a new battle asks for its fighters' files first)
    bool training_built();
    // The fighters of the load the game is doing (EV_FIGHTER_LOAD; rto asks for their options' files ahead): fighter id,
    // the outfit it will wear (the slot the plugin dressed it in, else the description's costume) and its side (its
    // team; online 0: the player's options are P1's). Read now, on the game thread, from the description of the last
    // load request or fighters' build; false: none. *why: which of the two
    struct LoadFighter { int fid, cos, side; };
    bool load_fighters(std::vector<LoadFighter>& out, const char** why);
    std::string slot_label(int fighter, int costume_no);   // one of our outfits: its label (outfits.json), else ""
    int slot_by_name(int fighter, const std::string& name); // one of our outfits by its game name ("Outfit II"), else -1
    bool display_nsfw();              // settings.json "display_nsfw": NSFW outfits and options shown
    int random_outfit(int fighter);   // the fighter's Random outfit's costume number (registry "random": true), -1: none
    void nsfw_row_present(bool on);   // the in-game row exists: settings.json is followed even with no NSFW content
    void set_display_nsfw(bool on);   // the row changed: settings.json written, applied on the next tick
    bool online_drivetech();          // settings.json "online_outfit": "DriveTech" (else Outfit 1) is the carrier
    void set_online_drivetech(bool on);   // the row changed: settings.json written, applied on the next tick
    std::string title_pack();         // settings.json "title_pack": "" (the game's own), a pack key or "random"
    bool skip_intro();                // settings.json "skip_intro"
    void set_title_pack(const std::string& key);   // the title rows changed: settings.json written (read by the loader at the next launch)
    void set_skip_intro(bool on);
    // settings.json "title_random_pool": the backgrounds Random draws from ("game" the game's own, packs' keys). False: never set
    // (Random draws from every pack).
    bool random_pool(std::vector<std::string>& keys);
    void set_random_pool(const std::vector<std::string>& keys);
    // settings.json "ui_choices": the interface packs' ticks ({ "u<pack>": [ "n<option>", ... ] }, ui_packs_logic.hpp), an
    // object (empty when not set); set_ui_choices writes it whole, every other key kept. False: not written.
    Json ui_choices();
    bool set_ui_choices(const Json& v);
    // The slot the online alias dressed the player's fighter in: that FighterDesc's, else the last one dressed for `fid` still
    // alive (any thread); false: none
    bool dressed_slot(Obj* fd, int fid, int& slot, int& color);
    // Replays > Settings: a replay fighter set to the character's Random outfit (`costume` its number) wears a draw of its pool,
    // the Random outfit's rules (random_logic.hpp: no repeat, never NSFW while hidden, the colour among the outfit's and the
    // Color Mod's entries). Any thread. The outfit and the game's colour drawn, the Color Mod's entry ("" a game colour);
    // false: not the Random outfit, or nothing to draw
    bool replay_random(int fighter, int costume, int& out_costume, int& out_color, std::string& entry);
    // The same for a replay fighter whose colour is Random (any outfit but the Random outfit): a colour of that outfit's round,
    // the one a Random colour draws in a battle (the game's colours the player has and the Color Mod's entries). Any thread.
    bool replay_random_colour(int fighter, int costume, int& out_color, std::string& entry);
}
// replay_outfits.cpp: Replay Outfits, what each fighter wears in a replay (rules: replay_outfits.hpp)
namespace replay {
    void install_hooks();             // first frame
}
// nsfw_option.cpp: the rows "Display NSFW Outfits" and "Outfit Others See" in Options > Interface > Other Settings
// (core option service)
namespace nsfwopt {
    void setup();                     // first frame, after rto::install_hooks (needs the core; once)
    void tick();                      // the row follows settings.json and the game's language
    bool retext_pending();            // the rows are written in another language than the game's (tick has work)
    bool on_value(int64_t entry, int64_t value);   // the core's on_value: true when the entry is the row's
    // The "Title screen" row, for its preview (title_preview.cpp). False: no such row (no pack, no core).
    struct TitleRow {
        std::string title;                     // the row's name, as the game shows it
        std::vector<std::string> labels;       // its values, as the game shows them
        std::vector<std::string> pictures;     // per value: the picture's resource ("": none)
        std::vector<std::string> keys;         // per value: the background's key for its title line ("game", a pack's; "" Random)
        std::vector<std::string> thumbs;       // per value: its thumbnail's resource ("": none; Random's live mosaic, its list's banner)
        std::vector<std::string> bands;        // per value: its 4:1 band at the list's cell size ("": none)
        std::string check_off, check_on;       // the Random list's check boxes ("": none)
        int value = 1;                         // 1-based, what the row holds
    };
    bool title_row(TitleRow& out);
    // The row's value set from the preview (LB / RB): settings.json, and the row on screen (core 0.9.8's opt_set_value)
    bool set_title_value(int value);
    // Random's selection changed (title_preview.cpp's list): settings.json, and the loader at once (SF6TitleSlots_Pool)
    void set_random_pool(const std::vector<std::string>& keys);
    // The "Title Screen Selection" section of Options > Interface (core 0.9.9's opt_section_place): its group id, -1 when there is
    // none (no pack, an older core)
    int64_t title_section();
}
// title_preview.cpp: the "Title screen" row of Options > Interface: its value slides when long, and Confirm shows the pack's picture
namespace titleprev {
    void frame();                     // every LateUpdateBehavior: its own gate (a look every 15 frames while the Options screen is not up)
}
// ui_packs.cpp: the interface mods (reframework\ui_mods, the loader's registry) in Options > Interface, one section per pack whose
// rows are the game's own (core 0.9.10); settings.json's "ui_choices" and the loader's SF6UISlots_Apply
namespace uipacks {
    void setup();                     // first frame, after nsfwopt::setup (the option service; once)
    void frame();                     // every LateUpdateBehavior: its own gate (a look every 15 frames while no pack is made)
    bool on_value(int64_t entry, int64_t value);   // the core's on_value (nsfwopt::on_value hands it on): true when the entry is a pack's
}
// fs_name.cpp: the outfit's name on Fighter Settings, at the right end of the bottom input guide bar
namespace fsname {
    void run_in_guide_update(void (*job)());   // once, inside the next guide list's update (R19: where a copy can be made). The hook is there once Battle Settings opened.
    void need_guide_hook();           // the guide list's hook now, without waiting for Battle Settings (the title preview's copies)
    void install_hooks();             // first frame (needs the core's ui_clone_control)
    bool busy();
    void tick(uint32_t events);
}
// opp_summary.cpp: Battle Settings > Other > Opponent Outfit Settings, a fighter with more outfits than the summary's labels
namespace oppsum {
    void install_hooks();             // from costumes::install_hooks (with slots only); starts the hook's code check (R18)
    bool busy();                      // the code check's verdict not acted on yet, or a list length to put back
    void frame();                     // each frame: the summary's lines past the game's own, on that page only (a look every 15 frames otherwise)
    void tick(uint32_t events);       // installs the hook after the check; puts back a length a draw that threw left
}
namespace stages {
    void init();
    void install_hooks();
    bool busy();
    void tick(uint32_t events);
}

} // namespace slots
