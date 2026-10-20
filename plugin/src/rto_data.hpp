// SF6 Slots — runtime costume options (docs/RUNTIME_OPTIONS.md): the data side, no game access.
//
// The loader's manifest (costume_options.json: the options of every modular mod and, per slot, the
// files each option swaps on the model), the user's choices (costume_choices.json, written by the
// plugin), what a model must load for a set of chosen options, the menu rows, the rules that keep
// a set of options coherent, the former variant slots. Compiled by the offline test too
// (plugin/tests/rto_test.cpp).
#pragma once

#include "json.hpp"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace slots::rto {

constexpr const char* kManifestFormat = "sf6-costume-options-1";
constexpr const char* kChoicesFormat = "sf6-costume-choices-1";

// ---- the manifest ----
struct File {
    std::string kind, target, path;                          // kind: mesh, material, texture, chain, colour, other
    std::string unit;                                        // a part unit (its chain's target, else its mesh's): one swap
    int joints = -1;                                         // a unit's mesh: its joint count (logs)
};
struct Option {
    std::string id, name, folder, mode, apply;               // mode: base, pick, toggle
    int group = -1;
    int rank = 0;                                            // layering order: a later option's file wins
    bool nsfw = false;                                       // its name has an NSFW keyword, or it comes from an NSFW
                                                             // mod (costume_mods_nsfw) put on this outfit (loader):
                                                             // hidden() while "Display NSFW Outfits" is off
};
struct Group { std::string name; std::vector<int> options; };
struct Variant { std::string label; std::vector<std::string> ids; };
struct Mod {
    std::string key, name, source, defaults_from;
    std::vector<Group> groups;
    std::vector<Option> options;
    std::vector<Variant> variants;
    int find(const std::string& id) const;                   // index in options, -1
};
struct Former { int costume_no = 0; std::string folder, label; std::vector<std::string> ids; };
struct Slot {
    int fighter = 0, costume_no = 0;
    std::string fighter_dir, folder, name, label, mod;
    int mod_index = -1;
    std::vector<std::string> defaults;
    std::map<std::string, std::vector<File>> files;          // option id -> its files for this slot
    std::vector<File> base;                                  // a target without any chosen option shipping it
    std::vector<int> required;                               // pick groups without "None": always one of their options
    std::vector<Former> former;
    std::map<std::string, std::string> rev;                  // stored path -> target (filled by parse_manifest)
    std::string prefix;                                      // "product/model/esf/<fighter_dir>/<folder>/"
    bool game = false;                                       // a game outfit given a separate add-on's options (no slot of ours)
};
struct Manifest {
    std::string format, generated, loader, pak, layout;
    std::vector<Mod> mods;
    std::vector<Slot> slots;
    bool has_nsfw = false;                                   // an option is marked "nsfw"
    // "Display NSFW Outfits" (settings.json display_nsfw, set by the plugin): while false, NSFW options are not offered
    // nor worn; a saved choice of one is kept (keep_hidden), only not applied
    bool show_nsfw = false;
    const Mod* mod_of(const Slot& s) const { return s.mod_index >= 0 ? &mods[static_cast<size_t>(s.mod_index)] : nullptr; }
    const Mod* mod_by_key(const std::string& key) const;
    // The slot whose folder holds this resource ("product/model/esf/esf028/006/..."), lowercase path
    const Slot* slot_of_path(const std::string& path) const;
    // The slot a model wears, from the resources it loads: one of our slots before a game outfit (a slot of ours
    // may keep a part in the game's folder of the outfit it is made from)
    const Slot* slot_of_paths(const std::vector<std::string>& paths) const;
    const Slot* slot_of(int fighter, int costume_no) const;
};
bool parse_manifest(const std::string& text, Manifest& out, std::string& err);

// A resource path as the manifest names it: lowercase, '/' separators, without "natives/stm/"
std::string norm_path(const std::string& path);

// A name cut to max_chars characters (UTF-8), ending with "..." when cut (the menu's title)
std::string clip_name(const std::string& s, size_t max_chars);
// An option or group name as the menu shows it: no leading "!", "1)" or "addon -" (then first letter upper case)
std::string display_name(const std::string& s);
constexpr size_t kTitleNameChars = 40;   // the outfit's name in the menu title, before "  -  P1"

// ---- the user's choices ----
struct Side { bool changed = false; std::vector<std::string> ids, names; };
struct Choices {
    std::map<std::string, std::array<Side, 2>> mods;         // mod key -> P1, P2
    std::string former_done;                                 // "<generated> <pak>" of the manifest migrated
};
bool parse_choices(const std::string& text, Choices& out);
// Ids the manifest no longer has are matched by name, else dropped (one note each)
void sanitize(Choices& c, const Manifest& m, std::vector<std::string>& notes);
std::string write_choices(const Choices& c, const Manifest& m);

// A toggle the slot wears by default whose Off would change nothing on the model (no base copy of any of its
// files, nothing else ships them): not offered (Ken SFV Hair V2 before part units: Off did nothing)
bool removable(const Manifest& m, const Slot& s, const std::string& id);
// The joint count the manifest gives for a mesh (a stored copy, or a target: the slot's own file), -1
int joints_at(const Manifest& m, const Slot& s, const std::string& path);

// An NSFW option while "Display NSFW Outfits" is off: not offered, not worn
bool hidden(const Manifest& m, const Slot& s, const std::string& id);

// The options a side wears on a slot: the slot's defaults until the user changed something; hidden options left out
// (a required group then gets its first other option, complete)
std::vector<std::string> chosen_of(const Manifest& m, const Slot& s, const Choices& c, int side);

// What to save for a side the menu changed while NSFW options are hidden: `now` (the menu's set), plus the hidden
// options of `saved` (what the side had saved) the menu did not replace: a hidden toggle stays; a hidden pick stays
// unless the user picked something else in its group (its pick in `now` differs from `shown`, the set the menu showed).
// `now` as it is while NSFW options are shown.
std::vector<std::string> keep_hidden(const Manifest& m, const Slot& s, const std::vector<std::string>& saved,
                                     const std::vector<std::string>& shown, const std::vector<std::string>& now);

// A group the loader marked "required": a file the slot wears from its options has no base copy (no usable
// file to go back to), so the group always has one of its options. complete() gives each such group without
// a choice its default option (the slot's default in that group, else its first offered option).
bool required(const Slot& s, int group);
std::string default_pick(const Manifest& m, const Slot& s, int group);
// Two picks of one group in a set (a saved set from before the loader made two On / Off options one choice): the
// last one stays (a note each).
void complete(const Manifest& m, const Slot& s, std::vector<std::string>& chosen, std::vector<std::string>* notes = nullptr);

// ---- what the model loads ----
struct Want {
    std::string kind;
    bool orig = true;        // the slot's own file at the target (what the model loads without any change)
    std::string path;        // else this stored copy
    std::string unit;        // the part unit of the file wanted (a base copy's or an option's), "" = none
};
// target -> what to load there, for every target an option or a base copy of the slot names
std::map<std::string, Want> desired(const Manifest& m, const Slot& s, const std::vector<std::string>& chosen,
                                    std::vector<std::string>* notes = nullptr);

// ---- keeping a set coherent ----
// A pick group's option chosen (id empty: none of the group), a toggle switched. Options of other groups
// that come LATER in the layering and ship a file of the option chosen are turned off: they would hide it.
void choose(const Manifest& m, const Slot& s, std::vector<std::string>& chosen, int group, const std::string& id);
// A toggle switched on also turns off every other chosen toggle that ships one of its targets, of any group: two
// On / Off options never replace a same file together (the loader makes such options one choice; defensive, a note).
void toggle(const Manifest& m, const Slot& s, std::vector<std::string>& chosen, const std::string& id, bool on,
            std::vector<std::string>* notes = nullptr);
bool same_set(const std::vector<std::string>& a, const std::vector<std::string>& b);

// ---- the menu rows ----
// Only options that change something on the model for this slot are offered; one that also ships
// files the model cannot swap (colours, cloth) is marked partial.
bool offered(const Slot& s, const std::string& id, bool* partial = nullptr);
struct Row {
    enum Kind { Pick, Toggle } kind = Pick;
    int group = -1;
    int option = -1;                     // toggle: the option's index
    std::string title;
    std::vector<std::string> labels;     // the choice's labels (1-based value)
    std::vector<int> label_option;       // per label: option index, -1 = none / off
    int value = 1;
    bool partial = false;
    bool none = true;                    // pick: label 1 is "None" (a required group: only when split in rows)
};
constexpr size_t kMaxLabels = 15;        // a native spin crashed above 16 entries (R9)
std::vector<Row> rows(const Manifest& m, const Slot& s, const std::vector<std::string>& chosen);
void apply_row(const Manifest& m, const Slot& s, std::vector<std::string>& chosen, const Row& r, int value,
               std::vector<std::string>* notes = nullptr);
// The menu's pages: `total` option rows, at most `per_page` rows a page, the first page also holding `head` rows of its
// own (the player side, Restore defaults). Per page: the option rows' indexes, in order. At least one page (empty
// when there is no row).
std::vector<std::vector<int>> split_pages(int total, int per_page, int head);
// 0: no preset matches the set, k: the mod's variant k-1
int preset_of(const Mod& mod, const std::vector<std::string>& chosen);
std::string preset_label(const Mod& mod, size_t k);   // a variant's label without the mod's name

// ---- the character select's panel (rto_select.cpp): the option rows through the panel's own rows ----
// The panel has kPanelRows rows (Costume, Color, Control Type, Button Preset); `n` option rows go through them as a
// window: `cur` is the option row with the cursor (0..n-1), `win` the first one shown (the screen row of the cursor is
// cur - win). Up / Down move the cursor and the window follows it; at the ends they wrap: Down on the last row goes to
// the first (window at the top), Up on the first to the last (window at the bottom). The CRSM recording slots' rule.
constexpr int kPanelRows = 4;
struct PanelWindow { int cur = 0, win = 0; };
PanelWindow panel_move(PanelWindow w, int n, int dir, int shown = kPanelRows);
// The window made valid for n rows (after the rows changed, or the cursor set from the screen row the game focused)
PanelWindow panel_clamp(PanelWindow w, int n, int shown = kPanelRows);
// The screen row k (0-based) focused by the game (a mouse hover): the cursor goes there, the window stays
PanelWindow panel_focus(PanelWindow w, int n, int k, int shown = kPanelRows);
// The arrow of screen row k: "\xE2\x96\xB2 " (U+25B2) on the first row while rows lie above, "\xE2\x96\xBC " (U+25BC)
// on the last shown row while rows lie below, else "". Drawn in the row's left margin (panel_arrow_left), the title
// untouched; put before the title only when that row has no arrow copy (the copy could not be made)
const char* panel_arrow(int k, int win, int n, int shown = kPanelRows);
// The left edge of a row's arrow, in the row's own units: the row's outline starts at row_left, its title at
// title_left, the glyph is glyph_w wide. Near the outline (a margin from it) when the space left of the title holds
// the glyph and two margins; centred in that space when it holds only the glyph; else just left of the title.
double panel_arrow_left(double row_left, double title_left, double glyph_w, double margin);
// A model on the VS screen (the versus intro before a fight): its GameObject sits under Pos < Left | Right <
// FighterEmoteCtrl (the Color Mod's finding). `up` = the names of a colour controller's GameObject (up[0]) and of its
// parents (up[1] its parent, ...). The side (1 Left, 2 Right) when a Left / Right within the first five names has a
// parent named FighterEmoteCtrl, else 0; *root = that Left / Right's index in `up` (the model's root), -1 if none.
int vs_model_side(const std::vector<std::string>& up, int* root);
// A spin step on a row's value (1-based, `labels` values): wraps at both ends
int step_value(int value, int labels, int dir);

// ---- former variant slots (the launch that turned RuntimeOptions on) ----
// The kept slot of a fighter's former variant slot number, and that variant's options
const Slot* former_of(const Manifest& m, int fighter, int costume_no, const Former** out);

} // namespace slots::rto
