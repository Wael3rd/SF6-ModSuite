// SF6 Slots — native REFramework plugin: entry points.
//
// One LateUpdateBehavior callback (REFramework offers no way to remove it). With no event pending
// and no work in progress it returns at once, having read nothing but this plugin's own memory.
// A post-LateUpdateBehavior callback paints the preview camera's hints while the runtime options menu is
// open (rto_camera.cpp); otherwise it returns at once on one flag of this plugin.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "rto.hpp"
#include "perf.hpp"
#include "wjournal.hpp"
#include "replay_tab.hpp"
#include "name_scroll.hpp"
#include "ui_retexture.hpp"
#include "version.hpp"
#include "../../updater/src/update_host.hpp"

namespace slots {

std::atomic<bool> g_closing{false};

// The plugin's additions to the game's menus (slots.hpp, menus::Item). Manager mode only (costumes::manager_mode: the
// Slotsloader Manager installed this loader): each has a key in settings.json, read once, and is shown unless its key says
// false; a key that is not there takes "menu_additions" (false: none of them), else it is shown. Installed any other way no
// key is read and everything is added, as always. What the rows set (display_nsfw, online_outfit) is still read from
// settings.json when a row is left out (costumes.cpp, as when the Native Core is not installed): a tool outside the game
// (the Slotsloader Manager) writes it then. menu_outfit_names is the loader's too (loader_core.cpp, g_outfit_labels: the
// slots keep their "Outfit V" names in the game's lists).
// The Display NSFW Outfits row has no key: in Manager mode it is never added (the loader marks no outfit NSFW there:
// no keywords, costume_mods_nsfw not read, so the row would show or hide nothing).
namespace menus {
namespace {
const char* const kKeys[kItems] = { "menu_outfit_names", "menu_fighter_settings_name", nullptr, "menu_online_outfit_row",
                                    "menu_title_screen", "menu_interface_packs", "menu_replay_settings" };
}
bool shown(Item item) {
    static bool loaded = false;
    static bool on[kItems];
    if (!loaded) {
        loaded = true;
        for (bool& b : on) b = true;
        if (costumes::manager_mode()) {   // nothing of this outside Manager mode
            on[NsfwRow] = false;          // whatever settings.json holds, also when it cannot be read
            std::string text = read_file("reframework/data/SF6_CostumeSlots_data/settings.json");
            if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);   // saved by an editor with a BOM
            Json j;
            if (!text.empty() && json_parse(text, j)) {
                bool all = true;
                if (const Json* k = j.get("menu_additions"); k && k->kind == Json::Bool) all = k->b;
                std::string left;
                for (int i = 0; i < kItems; ++i) {
                    if (!kKeys[i]) continue;   // no key: never added in Manager mode (set above)
                    on[i] = all;
                    if (const Json* k = j.get(kKeys[i]); k && k->kind == Json::Bool) on[i] = k->b;
                    if (!on[i]) left += std::string(left.empty() ? "" : ", ") + (kKeys[i] + 5);
                }
                if (!left.empty()) logf("menus: left out of the game's menus (settings.json): %s", left.c_str());
            }
        }
    }
    return item >= 0 && item < kItems ? on[item] : true;
}
}  // namespace menus

namespace {

// One of the rows or sections the plugin adds to Options is shown (they share one set of callbacks: nsfw_option.cpp)
bool options_shown() {
    return menus::shown(menus::NsfwRow) || menus::shown(menus::OnlineOutfitRow) || menus::shown(menus::TitleScreen)
        || menus::shown(menus::InterfacePacks);
}

bool g_hooks = false;
bool g_costumes_busy = true, g_stages_busy = true, g_rto_busy = true, g_name_busy = true, g_opp_busy = true;   // checked on the first frames

// Closing guard state (not cancellable for WM_DESTROY / WM_NCDESTROY / WM_ENDSESSION)
uint32_t s_close_frame = 0;
bool     s_close_final = false;

bool on_message(void* /*hwnd*/, unsigned int msg,
                unsigned long long wparam, long long /*lparam*/) {
    bool final_msg = false;
    const char* name = nullptr;
    switch (msg) {
    case 0x0010: name = "WM_CLOSE";     break;
    case 0x0002: name = "WM_DESTROY";   final_msg = true; break;
    case 0x0082: name = "WM_NCDESTROY"; final_msg = true; break;
    case 0x0016:
        if (wparam == 0) return true;
        name = "WM_ENDSESSION"; final_msg = true; break;
    default: return true;
    }
    if (!g_closing.exchange(true, std::memory_order_seq_cst)) {
        s_close_frame = g_frame;
        s_close_final = final_msg;
        logf("closing: the game window got %s, the plugin stops", name);
    } else if (final_msg && !s_close_final) {
        s_close_final = true;
    }
    return true;
}

void on_late_update() {
    ++g_frame;
    perf::frame();
    wjournal::frame();
    int64_t pt_outer = perf::begin();

    // Closing guard: skip all game work. Cancel WM_CLOSE after 30 s of frames.
    if (g_closing.load(std::memory_order_relaxed)) {
        if (!s_close_final && s_close_frame > 0 &&
            (g_frame - s_close_frame) > 60u * 30) {
            g_closing.store(false, std::memory_order_relaxed);
            s_close_frame = 0;
            logf("closing: cancelled, the plugin resumes");
        } else {
            perf::end(perf::OnLateUpdate, pt_outer);
            return;
        }
    }

    language_tick();                        // the language of our words (words.hpp): at most every 60 frames
    sf6up::host::frame();                   // the SF6 Mod Suite updater, when this copy runs it (updater/src/update_host.hpp)
    // (menus::shown: each of the plugin's additions to the game's menus can be left out in Manager mode)
    if (menus::shown(menus::OutfitNames)) namescroll::frame();     // long names scroll in the Costume rows: its own gate (registry reads every 15 frames)
    if (menus::shown(menus::TitleScreen)) titleprev::frame();      // Options' Title Screen row: its value slides, Confirm shows the pack's picture: its own gate
    if (menus::shown(menus::InterfacePacks)) uipacks::frame();     // Options' Interface Packs section and its editor: its own gate (nothing before setup)
    oppsum::frame();                        // Opponent Outfit Settings: the summary's lines past the game's: its own gate
    if (menus::shown(menus::ReplaySettings)) replaytab::frame();   // the Replays menu's "Replay Outfits" tab: its own gate (replay_tab.cpp)
    ui_retexture::frame();                  // UI packs: the textures the game keeps follow the overrides: nothing without any (ui_retexture.cpp)

    uint32_t ev = g_events.exchange(0, std::memory_order_acq_rel);
    if (!ev && !g_costumes_busy && !g_stages_busy && !g_rto_busy && !g_name_busy && !g_opp_busy && !nsfwopt::retext_pending()) {
        perf::end(perf::OnLateUpdate, pt_outer);
        return;
    }
    try {
        if (!g_hooks) {
            g_hooks = true;
            ui::log_path();
            costumes::install_hooks();
            stages::install_hooks();
            rto::install_hooks();
            if (menus::shown(menus::FighterName)) fsname::install_hooks();   // the outfit's name on Fighter Settings: one of the plugin's additions
            replay::install_hooks();
            // nsfwopt::setup takes the option service's callbacks for every row of ours, the interface packs' too, and makes
            // only the rows that are shown (nsfw_option.cpp): skipped when none of them is
            if (options_shown()) nsfwopt::setup();
            if (menus::shown(menus::InterfacePacks)) uipacks::setup();   // its section after the title one (both just above Other Settings)
        }
        if (ev & EV_DIAG) diag_flush();
        {
            int64_t pt = perf::begin();
            costumes::tick(ev);
            perf::end(perf::CostumesTick, pt);
        }
        {
            int64_t pt = perf::begin();
            stages::tick(ev);
            perf::end(perf::StagesTick, pt);
        }
        {
            int64_t pt = perf::begin();
            rto::tick(ev);                  // runtime costume options (after the costume work of the frame)
            perf::end(perf::RtoTick, pt);
        }
        {
            int64_t pt = perf::begin();
            if (menus::shown(menus::FighterName)) fsname::tick(ev);   // the outfit's name on Fighter Settings
            perf::end(perf::FsNameTick, pt);
        }
        oppsum::tick(ev);                   // Opponent Outfit Settings: the summary's hook after its code check
        if (options_shown()) nsfwopt::tick();   // the Options rows follow settings.json
        g_costumes_busy = costumes::busy();
        g_stages_busy = stages::busy();
        g_rto_busy = rto::busy();
        g_name_busy = menus::shown(menus::FighterName) && fsname::busy();
        g_opp_busy = oppsum::busy();
    } catch (const std::exception& e) {
        logf("EXCEPTION: %s", e.what());
    } catch (...) {
        logf("EXCEPTION (unknown)");
    }

    perf::end(perf::OnLateUpdate, pt_outer);
}

void on_post_late_update() {
    if (g_closing.load(std::memory_order_relaxed)) return;
    try {
        if (menus::shown(menus::OutfitNames)) namescroll::post_frame();   // a row's sliding name, if the game's focus animation moved it this frame
        if (rto::camera::active()) rto::camera::post_frame();
    } catch (...) {
        logf("EXCEPTION (post frame)");
    }
}

void update_log(const char* line) { logf("update: %s", line); }

} // namespace
} // namespace slots

// The version the SF6 Mod Suite updater reads (updater/src/update_game.cpp): the Slots Loader package's
extern "C" __declspec(dllexport) const char* sf6suite_package_version(void) { return slots::kPackageVersion; }

extern "C" __declspec(dllexport)
void reframework_plugin_required_version(REFrameworkPluginVersion* v) {
    if (!v) return;
    v->major = 1;
    v->minor = 10;    // API of REFramework 1.5.8
    v->patch = 0;
    v->game_name = nullptr;
}

extern "C" __declspec(dllexport)
bool reframework_plugin_initialize(const REFrameworkPluginInitializeParam* param) {
    if (!param || !param->functions || !param->functions->on_pre_application_entry) return false;
    try { reframework::API::initialize(param); } catch (...) { return false; }
    slots::log_open();
    slots::logf("SF6 Slots native plugin loaded (Slots Loader %s)", slots::kPackageVersion);
    // The SF6 Mod Suite updater (updater/): this copy or the Native Core runs it, decided at the first frame
    sf6up::host::init({ sf6up::host::kRolePlugin, slots::update_log });
    slots::ui::init();
    // The Lua scripts this plugin replaces, left by an update made by hand: both would handle the
    // same menus. Renamed, not deleted.
    for (const char* name : { "SF6_CostumeSlots.lua", "SF6_StageSlots.lua" }) {
        std::string p = std::string("reframework/autorun/") + name;
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        bool ok = MoveFileExA(p.c_str(), (p + ".old").c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        slots::logf("old script %s %s", name, ok ? "renamed to .lua.old (replaced by this plugin)" : "could not be renamed");
    }
    try {
        slots::rto::init();          // before the costumes: a Battle Settings choice of a former variant slot moves
        slots::costumes::init();
        slots::stages::init();
    } catch (...) {
        slots::logf("EXCEPTION at init");
    }
    param->functions->on_pre_application_entry("LateUpdateBehavior", slots::on_late_update);
    if (param->functions->on_post_application_entry)
        param->functions->on_post_application_entry("LateUpdateBehavior", slots::on_post_late_update);
    // Register window message callback (closing guard)
    if (param->functions->on_message) {
        param->functions->on_message(slots::on_message);
        slots::logf("registered on_message (closing guard)");
    } else {
        slots::logf("on_message not available (closing guard disabled)");
    }
    return true;
}
