// nsfw_option.cpp: the rows of the "Slots Loader" group in the game's Options > Interface > Other Settings, through the
// Native Core's option service (core >= 0.9.0, opt_group_place): "Display NSFW Outfits" (Off / On, settings.json's
// "display_nsfw") and "Outfit Others See" (Outfit 1 / DriveTech, "online_outfit": the carrier of the online alias,
// carrier.hpp). costumes.cpp reads and writes the file; this file only owns the rows.
// Only when the title packs' registry (reframework/data/SF6_TitleSlots_Data/registry.json, written by the loader at game start)
// lists at least one pack: a section of its own in Options > Interface, "Title Screen Selection", above Other Settings (core
// 0.9.9's opt_section_place, Wael 08/11: as Battle Settings' Stage Selection; its page is title_preview.cpp's), which sets
// settings.json's "title_pack" ("" = the game's own, a pack key, or "random"), and one more row here, "Skip intro screens"
// ("skip_intro"). The loader reads both at the next launch; the title's background
// also changes at the next return to the title (the loader's SF6TitleSlots_Select: its copies served in place of the movies). A pack's name is written
// whole: one too long for its place slides in it, and Confirm on the row shows the pack's picture (title_preview.cpp).
//
// One option-service callback set per client: when the runtime options menu (rto_menu.cpp) registered it, its
// on_value hands the entries it does not know to on_value() here; otherwise this file registers its own. on_value() hands the
// interface packs' rows to ui_packs.cpp.
// Without the core, or with a core that lacks opt_group_place: no row, one log line, the file setting keeps working.
// Game rules (slots.hpp): the core calls on_value on the game thread; setup() and tick() run from LateUpdateBehavior.
#include "slots.hpp"
#include "rto.hpp"
#include "slots_ui.hpp"
#include "words.hpp"
#include "json.hpp"

namespace slots::nsfwopt {
namespace {

constexpr int32_t kTabInterface = 1;       // app.Option.TabType
constexpr int32_t kSectionOthers = 290;    // app.Option.GroupType OthersSettings_Interface ("Other Settings")

bool g_done = false;          // setup() ran (row placed or given up)
int64_t g_group = -1, g_entry = -1, g_entry_outfit = -1;
// The rows are placed (make_row succeeded). It was "g_entry >= 0" while the Display NSFW Outfits row was always made: in
// Manager mode a row can be left out (menus::shown), the others and the title section still follow settings.json.
bool g_live = false;
bool g_shown = false;         // the value the row holds (what the core was given / told)
bool g_shown_dt = false;      // the value the "Outfit Others See" row holds
uint32_t g_words = 0;         // the language the rows were written in (words::version)

// The title packs (registry.json, in the order the loader wrote them)
constexpr const char* kRegistry = "reframework/data/SF6_TitleSlots_Data/registry.json";
struct Pack { std::string key, name, picture, thumb, band; };   // picture, thumb: their resources in the stage pak ("": none)
std::vector<Pack> g_packs;
uint64_t g_reg_stamp = 0;     // size and date of registry.json when read (0: no file)
bool g_reg_read = false;      // registry.json was read at least once
std::string g_game_picture;   // the game's own title's picture (registry.json's "game_picture"; "": none)
std::string g_random_picture; // Random's: the mosaic of the packs' pictures ("random_picture")
std::string g_game_thumb, g_game_band, g_check_off, g_check_on;   // "game_thumb", "check_off", "check_on" ("": none)
int64_t g_entry_title = -1, g_entry_skip = -1;
int64_t g_section = -1;       // the "Title Screen Selection" section's group (-1: none)
std::string g_section_words;  // the words it was given (its texts follow the game's language)
std::vector<std::string> g_title_keys;   // the values of the "Title screen" row: "", each pack's key, "random"
std::vector<std::string> g_title_labels; // their labels, as the row shows them
int g_shown_title = 1;        // the row's value (1-based index in g_title_keys)
bool g_shown_skip = false;
uint32_t g_tick_n = 0;        // the title rows' files are looked at every kTitleEvery ticks
constexpr uint32_t kTitleEvery = 30;

uint64_t registry_stamp() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(kRegistry, GetFileExInfoStandard, &fa)) return 0;
    return ((uint64_t(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime) ^ fa.nFileSizeLow;
}

// registry.json read again when its stamp changed (or the first time). True when the list of packs changed.
// A missing, empty or unreadable file is an empty list: no title rows.
bool read_registry() {
    const uint64_t st = registry_stamp();
    if (g_reg_read && st == g_reg_stamp) return false;
    g_reg_read = true;
    g_reg_stamp = st;
    std::vector<Pack> packs;
    std::string text = st ? read_file(kRegistry) : std::string();
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    Json j;
    g_game_picture.clear();
    g_random_picture.clear();
    g_game_thumb.clear();
    g_game_band.clear();
    g_check_off.clear();
    g_check_on.clear();
    if (st && json_parse(text, j)) {
        auto str_of = [&](const char* k, std::string& out) { if (const Json* v = j.get(k); v && v->kind == Json::String) out = v->s; };
        str_of("game_thumb", g_game_thumb);
        str_of("game_band", g_game_band);
        str_of("check_off", g_check_off);
        str_of("check_on", g_check_on);
        if (const Json* gp = j.get("game_picture"); gp && gp->kind == Json::String) g_game_picture = gp->s;
        if (const Json* rp = j.get("random_picture"); rp && rp->kind == Json::String) g_random_picture = rp->s;
        if (const Json* arr = j.get("packs"); arr && arr->kind == Json::Array) {
            for (const Json& p : arr->arr) {
                const Json* k = p.get("key");
                const Json* n = p.get("name");
                const Json* pic = p.get("preview");
                const Json* th = p.get("thumb");
                const Json* bd = p.get("band");
                if (!k || k->kind != Json::String || k->s.empty()) continue;
                packs.push_back({ k->s, n && n->kind == Json::String && !n->s.empty() ? n->s : k->s,
                                  pic && pic->kind == Json::String ? pic->s : std::string(),
                                  th && th->kind == Json::String ? th->s : std::string(),
                                  bd && bd->kind == Json::String ? bd->s : std::string() });
            }
        }
    }
    bool same = packs.size() == g_packs.size();
    for (size_t i = 0; same && i < packs.size(); ++i)
        same = packs[i].key == g_packs[i].key && packs[i].name == g_packs[i].name && packs[i].picture == g_packs[i].picture &&
               packs[i].thumb == g_packs[i].thumb && packs[i].band == g_packs[i].band;
    if (same) return false;
    g_packs = std::move(packs);
    return true;
}

// The 1-based value of the "Title screen" row for settings.json's title_pack (unknown: the game's own)
int title_value(const std::vector<std::string>& keys, const std::string& pack) {
    for (size_t i = 1; i < keys.size(); ++i) if (keys[i] == pack) return static_cast<int>(i) + 1;
    return 1;
}

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

void SF6CORE_CALL cb_value(void*, int64_t entry, int64_t value) { on_value(entry, value); }

// The loader's SF6TitleSlots_Select (amd_ags_x64.dll, stage_redirect.hpp): the choice served at the next return to the title.
// An older loader without it: the choice waits for the next launch.
void tell_loader(const std::string& choice) {
    using Fn = void (*)(const char*);
    static Fn fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE h = GetModuleHandleW(L"amd_ags_x64.dll")) fn = reinterpret_cast<Fn>(GetProcAddress(h, "SF6TitleSlots_Select"));
        logf("nsfw option: loader's SF6TitleSlots_Select %s", fn ? "found: a title change shows at the next return to the title"
                                                                   : "not found (older loader): a title change waits for the next launch");
    }
    if (fn) fn(choice.c_str());
}

// (Re)made group + entry + placement; the old group goes first. False when the core refused something.
bool make_row(const char** why) {
    const SF6CoreAPI* a = A();
    if (g_group >= 0) a->opt_remove_group(C(), g_group);
    g_group = g_entry = g_entry_outfit = g_entry_title = g_entry_skip = -1;
    g_live = false;
    g_shown = costumes::display_nsfw();
    g_shown_dt = costumes::online_drivetech();
    g_words = words::version();
    using words::G; using words::W; using words::game; using words::tr; using words::trf;
    // Manager mode: each row and the title screen's part can be left out (menus::shown, main.cpp); installed any other way
    // all of them are made, as always. The group is made for the rows there are.
    const bool row_nsfw = menus::shown(menus::NsfwRow), row_outfit = menus::shown(menus::OnlineOutfitRow);
    const bool title = menus::shown(menus::TitleScreen) && !g_packs.empty();
    if (row_nsfw || row_outfit || title) {
        g_group = a->opt_group(C(), "slots.interface", "Slots Loader", "");
        if (g_group < 0) { *why = "opt_group refused"; return false; }
    }
    const std::string off = game(G::Off), on = game(G::On), outfit1 = game(G::Outfit1), dt = game(G::OutfitDrivetech);
    const std::string desc = trf(W::NsfwDisplayDesc, { off, outfit1 }), desc_outfit = trf(W::NsfwOnlineOutfitDesc, { game(G::BattleSettings), dt, outfit1 });
    const char* labels[] = { off.c_str(), on.c_str() };
    if (row_nsfw) {
        SF6CoreOptEntryDesc d{};
        d.struct_size = sizeof d;
        d.kind = SF6CORE_OPT_CHOICE; d.key = "display_nsfw"; d.title = tr(W::NsfwDisplay); d.desc = desc.c_str();
        d.options = labels; d.n_options = 2; d.value = g_shown ? 2 : 1;
        g_entry = a->opt_entry(C(), g_group, &d);
        if (g_entry < 0) { *why = "opt_entry refused"; return false; }
    }
    const char* outfits[] = { outfit1.c_str(), dt.c_str() };
    if (row_outfit) {
        SF6CoreOptEntryDesc o{};
        o.struct_size = sizeof o;
        o.kind = SF6CORE_OPT_CHOICE; o.key = "online_outfit"; o.title = tr(W::NsfwOnlineOutfit); o.desc = desc_outfit.c_str();
        o.options = outfits; o.n_options = 2; o.value = g_shown_dt ? 2 : 1;
        g_entry_outfit = a->opt_entry(C(), g_group, &o);
        if (g_entry_outfit < 0) { *why = "opt_entry refused (Outfit Others See)"; return false; }
    }
    g_title_keys.clear();
    g_title_labels.clear();
    if (title) {
        const std::string pack_now = costumes::title_pack();
        g_title_keys.push_back("");
        std::vector<std::string> names = { tr(W::TitleGameOwn) };
        for (const Pack& p : g_packs) { g_title_keys.push_back(p.key); names.push_back(p.name); }
        g_title_keys.push_back("random");
        names.push_back(tr(W::TitleRandom));
        g_title_labels = names;
        g_shown_title = title_value(g_title_keys, pack_now);
        g_shown_skip = costumes::skip_intro();
        SF6CoreOptEntryDesc k{};
        k.struct_size = sizeof k;
        k.kind = SF6CORE_OPT_CHOICE; k.key = "skip_intro"; k.title = tr(W::TitleSkip); k.desc = tr(W::TitleSkipDesc);
        k.options = labels; k.n_options = 2; k.value = g_shown_skip ? 2 : 1;
        g_entry_skip = a->opt_entry(C(), g_group, &k);
        if (g_entry_skip < 0) { *why = "opt_entry refused (Skip intro screens)"; return false; }
    }
    if (g_group >= 0 && a->opt_group_place(C(), g_group, kTabInterface, kSectionOthers) != SF6CORE_OK) { *why = "opt_group_place refused"; return false; }
    // the title screen's section: made once, its texts updated in place (the core keeps its unit), withdrawn with the last pack
    // (`title`: there are packs, and the section is not left out)
    if (SF6CORE_HAS(a, opt_section_place)) {
        const std::string sec_words = std::string(tr(W::TitleSection)) + "|" + tr(W::TitleSectionDesc);
        if (title && g_section < 0) {
            g_section = a->opt_group(C(), "slots.title_section", tr(W::TitleSection), tr(W::TitleSectionDesc));
            if (g_section >= 0 && a->opt_section_place(C(), g_section, kTabInterface, kSectionOthers) != SF6CORE_OK) {
                logf("nsfw option: the title screen section refused by the core");
                a->opt_remove_group(C(), g_section);
                g_section = -1;
            }
            g_section_words = sec_words;
        } else if (title && sec_words != g_section_words) {
            a->opt_group_def(C(), g_section, tr(W::TitleSection), tr(W::TitleSectionDesc));
            g_section_words = sec_words;
        } else if (!title && g_section >= 0) {
            a->opt_section_place(C(), g_section, -1, -1);
            a->opt_remove_group(C(), g_section);
            g_section = -1;
        }
    }
    a->opt_rebuild(C());
    a->opt_ensure_built(C());
    g_live = true;
    return true;
}

} // namespace

// The core's on_value (game thread). True when the entry is this row's, or an interface pack's row (ui_packs.cpp).
bool on_value(int64_t entry, int64_t value) {
    if (uipacks::on_value(entry, value)) return true;
    if (g_entry_skip >= 0 && entry == g_entry_skip) {     // choice: 1 = Off, 2 = On
        const bool on = value == 2;
        if (on != g_shown_skip) {
            g_shown_skip = on;
            costumes::set_skip_intro(on);
        }
        return true;
    }
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
    if (!why) read_registry();                        // the title rows need the pack list before the group is made
    // the file is followed (and read now) even with no NSFW content, when its row is there to show it
    if (!why) costumes::nsfw_row_present(menus::shown(menus::NsfwRow));
    if (!why && make_row(&why)) {
        std::string rows;                             // the rows made (Manager mode can leave some out)
        auto row = [&](int64_t entry, const char* name) { if (entry >= 0) rows += std::string(rows.empty() ? "" : ", ") + name; };
        row(g_entry, "Display NSFW Outfits");
        row(g_entry_outfit, "Outfit Others See");
        row(g_entry_skip, "Skip Intro Screens");
        logf("nsfw option: rows placed in Interface > Other Settings (%s)%s", rows.empty() ? "none" : rows.c_str(),
             g_section >= 0 ? "; the Title Screen Selection section above it" : "");
        return;
    }
    logf("nsfw option: rows not placed (%s)", why);
    g_group = g_entry = g_entry_outfit = g_entry_title = g_entry_skip = -1;
    g_live = false;
}

bool title_row(TitleRow& out) {
    if (g_title_keys.empty() || g_title_labels.size() != g_title_keys.size()) return false;
    out.title = words::tr(words::W::TitleSection);
    out.labels = g_title_labels;
    out.pictures.assign(g_title_keys.size(), std::string());
    out.pictures[0] = g_game_picture;
    out.pictures.back() = g_random_picture;           // the last value: Random
    out.keys.assign(g_title_keys.size(), std::string());
    out.keys[0] = "game";                             // the game's own title (the core's title.cpp knows it by this key)
    for (size_t i = 1; i + 1 < g_title_keys.size(); ++i) out.keys[i] = g_title_keys[i];
    for (size_t i = 0; i < g_packs.size() && i + 1 < out.pictures.size(); ++i) out.pictures[i + 1] = g_packs[i].picture;   // 0: the game's own
    out.thumbs.assign(g_title_keys.size(), std::string());
    out.thumbs[0] = g_game_thumb;
    for (size_t i = 0; i < g_packs.size() && i + 1 < out.thumbs.size(); ++i) out.thumbs[i + 1] = g_packs[i].thumb;
    out.bands.assign(g_title_keys.size(), std::string());
    out.bands[0] = g_game_band;
    for (size_t i = 0; i < g_packs.size() && i + 1 < out.bands.size(); ++i) out.bands[i + 1] = g_packs[i].band;
    out.check_off = g_check_off;
    out.check_on = g_check_on;
    out.value = g_shown_title;
    return true;
}

void set_random_pool(const std::vector<std::string>& keys) {
    costumes::set_random_pool(keys);
    using Fn = void (*)(const char*);
    static Fn fn = nullptr;
    static bool looked = false;
    if (!looked) {
        looked = true;
        if (HMODULE h = GetModuleHandleW(L"amd_ags_x64.dll")) fn = reinterpret_cast<Fn>(GetProcAddress(h, "SF6TitleSlots_Pool"));
    }
    std::string csv;
    for (const std::string& k : keys) csv += (csv.empty() ? "" : ",") + k;
    if (fn) fn(csv.c_str());
    logf("nsfw option: Random's selection: %zu background(s)%s", keys.size(), fn ? "" : " (older loader: from the next launch)");
}

int64_t title_section() { return g_section; }

bool set_title_value(int value) {
    if (g_title_keys.empty() || value < 1 || value > static_cast<int>(g_title_keys.size())) return false;
    if (value == g_shown_title) return true;
    g_shown_title = value;
    costumes::set_title_pack(g_title_keys[static_cast<size_t>(value - 1)]);
    tell_loader(g_title_keys[static_cast<size_t>(value - 1)]);
    logf("nsfw option: title screen chosen: \"%s\"", g_title_keys[static_cast<size_t>(value - 1)].c_str());
    return true;
}

// The rows were written in another language than the game's: tick() has to write them again
bool retext_pending() { return g_live && g_words != words::version(); }

// The setting changed elsewhere (settings.json edited), the pack list did (registry.json) or the game's language did: the rows
// are made again with the new values and words. Not while an option window is open (a rebuild would pull the rows from under
// the player, and registry.json is not read then either).
void tick() {
    if (!g_live) return;
    const SF6CoreAPI* a = A();
    if (!a || !C()) return;
    bool title_changed = false;
    if (++g_tick_n % kTitleEvery == 0 && !a->opt_window_open()) {
        title_changed = read_registry();
        if (!title_changed && g_entry_skip >= 0)
            title_changed = title_value(g_title_keys, costumes::title_pack()) != g_shown_title || costumes::skip_intro() != g_shown_skip;
        if (!title_changed && (menus::shown(menus::TitleScreen) && !g_packs.empty()) != (g_entry_skip >= 0)) title_changed = true;
    }
    if (!title_changed && costumes::display_nsfw() == g_shown && costumes::online_drivetech() == g_shown_dt && !retext_pending()) return;
    if (a->opt_window_open()) return;
    const char* why = nullptr;
    if (make_row(&why)) logf("nsfw option: rows follow settings.json (NSFW %s, %s, %zu title pack%s)", g_shown ? "On" : "Off", g_shown_dt ? "DriveTech" : "Outfit 1", g_packs.size(), g_packs.size() == 1 ? "" : "s");
    else { logf("nsfw option: rows lost (%s)", why); g_group = g_entry = g_entry_outfit = g_entry_title = g_entry_skip = -1; g_live = false; }
}

} // namespace slots::nsfwopt
