// nsfw_option.cpp: the rows of the "Slots Loader" group in the game's Options > Interface > Other Settings, through the
// Native Core's option service (core >= 0.9.0, opt_group_place): "Display NSFW Outfits" (Off / On, settings.json's
// "display_nsfw") and "Outfit Others See" (Outfit 1 / DriveTech, "online_outfit": the carrier of the online alias,
// carrier.hpp). costumes.cpp reads and writes the file; this file only owns the rows.
//
// One option-service callback set per client: when the runtime options menu (rto_menu.cpp) registered it, its
// on_value hands the entries it does not know to on_value() here; otherwise this file registers its own.
// Without the core, or with a core that lacks opt_group_place: no row, one log line, the file setting keeps working.
// Game rules (slots.hpp): the core calls on_value on the game thread; setup() and tick() run from LateUpdateBehavior.
#include "slots.hpp"
#include "rto.hpp"
#include "slots_ui.hpp"

namespace slots::nsfwopt {
namespace {

constexpr int32_t kTabInterface = 1;       // app.Option.TabType
constexpr int32_t kSectionOthers = 290;    // app.Option.GroupType OthersSettings_Interface ("Other Settings")
const char* kDesc = "Shows the outfits from costume_mods_nsfw and the options marked NSFW (see through, pubic, ...). "
                    "Off hides them everywhere; a fighter wearing one goes back to Outfit 1.";
const char* kDescOutfit = "When you wear a modded outfit chosen in Battle Settings, other players see you in this outfit "
                          "(you see your mod). DriveTech: characters without it use Outfit 1. Applies from the next battle.";

bool g_done = false;          // setup() ran (row placed or given up)
int64_t g_group = -1, g_entry = -1, g_entry_outfit = -1;
bool g_shown = false;         // the value the row holds (what the core was given / told)
bool g_shown_dt = false;      // the value the "Outfit Others See" row holds

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

void SF6CORE_CALL cb_value(void*, int64_t entry, int64_t value) { on_value(entry, value); }

// (Re)made group + entry + placement; the old group goes first. False when the core refused something.
bool make_row(const char** why) {
    const SF6CoreAPI* a = A();
    if (g_group >= 0) a->opt_remove_group(C(), g_group);
    g_group = g_entry = g_entry_outfit = -1;
    g_shown = costumes::display_nsfw();
    g_shown_dt = costumes::online_drivetech();
    g_group = a->opt_group(C(), "slots.interface", "Slots Loader", "");
    if (g_group < 0) { *why = "opt_group refused"; return false; }
    const char* labels[] = { "Off", "On" };
    SF6CoreOptEntryDesc d{};
    d.struct_size = sizeof d;
    d.kind = SF6CORE_OPT_CHOICE; d.key = "display_nsfw"; d.title = "Display NSFW Outfits"; d.desc = kDesc;
    d.options = labels; d.n_options = 2; d.value = g_shown ? 2 : 1;
    g_entry = a->opt_entry(C(), g_group, &d);
    if (g_entry < 0) { *why = "opt_entry refused"; return false; }
    const char* outfits[] = { "Outfit 1", "DriveTech" };
    SF6CoreOptEntryDesc o{};
    o.struct_size = sizeof o;
    o.kind = SF6CORE_OPT_CHOICE; o.key = "online_outfit"; o.title = "Outfit Others See"; o.desc = kDescOutfit;
    o.options = outfits; o.n_options = 2; o.value = g_shown_dt ? 2 : 1;
    g_entry_outfit = a->opt_entry(C(), g_group, &o);
    if (g_entry_outfit < 0) { *why = "opt_entry refused (Outfit Others See)"; return false; }
    if (a->opt_group_place(C(), g_group, kTabInterface, kSectionOthers) != SF6CORE_OK) { *why = "opt_group_place refused"; return false; }
    a->opt_rebuild(C());
    a->opt_ensure_built(C());
    return true;
}

} // namespace

// The core's on_value (game thread). True when the entry is this row's.
bool on_value(int64_t entry, int64_t value) {
    if (g_entry_outfit >= 0 && entry == g_entry_outfit) {   // choice: 1 = Outfit 1, 2 = DriveTech
        const bool dt = value == 2;
        if (dt != g_shown_dt) {
            g_shown_dt = dt;
            costumes::set_online_drivetech(dt);
        }
        return true;
    }
    if (g_entry < 0 || entry != g_entry) return false;
    const bool on = value == 2;                       // choice: 1 = Off, 2 = On
    if (on != g_shown) {
        g_shown = on;
        costumes::set_display_nsfw(on);
    }
    return true;
}

void setup() {
    if (g_done) return;
    g_done = true;
    const SF6CoreAPI* a = A();
    const char* why = nullptr;
    if (!a || !C()) why = "no core";
    else if (!SF6CORE_HAS(a, opt_group_place)) why = "this core has no opt_group_place (older than 0.9.0)";
    else if (!SF6CORE_HAS(a, opt_set_callbacks) || !SF6CORE_HAS(a, opt_entry) || !SF6CORE_HAS(a, opt_rebuild)) why = "no option service";
    if (!why && !rto::menu::service()) {              // the runtime options menu did not take the callbacks: ours
        SF6CoreOptCallbacks cb{};
        cb.struct_size = sizeof cb;
        cb.on_value = cb_value;
        if (a->opt_set_callbacks(C(), &cb) != SF6CORE_OK) why = "opt_set_callbacks refused";
    }
    if (!why) costumes::nsfw_row_present(true);       // the file is followed (and read now) even with no NSFW content
    if (!why && make_row(&why)) {
        logf("nsfw option: rows placed in Interface > Other Settings (Display NSFW Outfits, Outfit Others See)");
        return;
    }
    logf("nsfw option: rows not placed (%s)", why);
    g_group = g_entry = g_entry_outfit = -1;
}

// The setting changed elsewhere (settings.json edited): the row is made again with the new value. Not while an
// option window is open (a rebuild would pull the rows from under the player).
void tick() {
    if (g_entry < 0 || (costumes::display_nsfw() == g_shown && costumes::online_drivetech() == g_shown_dt)) return;
    const SF6CoreAPI* a = A();
    if (!a || !C() || a->opt_window_open()) return;
    const char* why = nullptr;
    if (make_row(&why)) logf("nsfw option: rows follow settings.json (NSFW %s, %s)", g_shown ? "On" : "Off", g_shown_dt ? "DriveTech" : "Outfit 1");
    else { logf("nsfw option: rows lost (%s)", why); g_group = g_entry = g_entry_outfit = -1; }
}

} // namespace slots::nsfwopt
