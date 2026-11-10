// Colours follow their outfit. The Color Mod keeps an outfit's colours by fighter and costume number: the editor's MC
// colours in reframework\color_mods\<Character>\MANUALCOLORS_NN\esfFFF_CCC_cmd_KKK.user.2 (CCC = the number + 1), their
// edited copies and the game colours it edited in data\SF6_ColorSpinExtra_data\overrides\F_N_*.user.2, the game colours it
// captured in pristine\F_N_game_*.user.2, and what names them by number or path in state.json (the choices), chip_slots.json
// (the squares) and opponent_colours.txt (the ticks). A mod outfit's number can change (an updated mod, an outfit numbered
// again, a removed outfit installed again), so at each launch, before the Color Mod reads anything, the outfit's colours are
// moved to its new number (Ryu's "Tekken 8 Jin" showed another outfit's MC colours after a number was reused, 08/11).
//
// An outfit is its mod's content id (registry mod_id), its fighter and the game costume it is based on. A mod updated (new
// files, so a new id) is the same outfit when exactly one outfit of that fighter and name (outfits.json "label", case and
// spaces left out) went away and exactly one appeared. An outfit that goes away: its colours set aside in
// .former_outfits\<identity> beside them, put back when the same outfit is installed again, at whatever number. Files of no
// known outfit at a number another outfit gets: set aside in .former_outfits\<date>, as before. The colours of colour packs
// (RW, made by their authors for a number) and their edited copies stay with the number. Nothing is deleted.
//
// colour_owners.txt (SF6_Costumes_Data) says which outfit the colours at each number belong to; colour_follow.journal, there
// too, finishes a pass cut short. Pure functions (decide, make_plan, the renaming and the rewrites) are checked by
// build\colourtest.exe --selftest (costumes/tools/colourtest.cpp); run() is the launch's pass, the only part touching files.
#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace colour_follow {

// One outfit of ours as registry.json and outfits.json describe it
struct Outfit {
    int fighter = 0, number = 0;   // costume number (registry new_costume_no)
    std::string mod_id;            // its mod's content id
    int based_on = 0;              // the game costume it is based on (registry original_costume_no)
    std::string name;              // its label ("" = none that tells it: a Fluffy pak's file name)
};

std::string identity_key(const Outfit& o);         // "<mod_id>_f001_c00": the folder its set-aside colours go to
bool is_identity_key(const std::string& s);
std::string normal_name(const std::string& name);  // lower case, spaces trimmed and single

// What the colours at a number do
enum class Why {
    Same,      // the same outfit, at another number now
    Updated,   // its mod updated (new id, same fighter and name), at another number now
    Back,      // installed again: its colours come back from .former_outfits\<key>
    Gone,      // no longer installed: its colours go to .former_outfits\<key>
    Unknown    // files of no known outfit at a number an outfit gets: to .former_outfits\<date>
};
struct Step {
    Why why = Why::Same;
    int fighter = 0;
    int from = -1;        // the number they are at (-1: Back, in .former_outfits\<key>)
    int to = -1;          // the number they go to (-1: Gone / Unknown, set aside)
    std::string key;      // the outfit's identity (its folder if they are set aside), the date for Unknown
    std::string label;    // for the log
};
// former: who the colours on disk belong to; now: this launch's outfits; aside: the identities with colours set aside
std::vector<Step> decide(const std::vector<Outfit>& former, const std::vector<Outfit>& now,
                         const std::set<std::string>& aside, const std::string& stamp);

// File names and paths
bool parse_manual_name(const std::string& name, int& fighter, int& number);    // esfFFF_CCC_cmd_KKK.user.2
std::string renumber_manual_name(const std::string& name, int number);
bool parse_data_name(const std::string& name, int& fighter, int& number, std::string& rest);   // F_N_<rest>.user.2
bool override_follows(const std::string& rest);    // false for an edited copy of a colour pack's file (RW)
std::string renumber_data_name(const std::string& name, int number);
bool parse_manual_path(const std::string& path, int& fighter, int& number);   // "..\color_mods\...MANUALCOLORS...\<MC file>"
std::string renumber_manual_path(const std::string& path, int number);

// A colour file of the Color Mod: live at base\[sub\]name, set aside at base\.former_outfits\<key>\[sub\]name
struct ColourFile {
    enum Kind { Manual, Override, Pristine } kind = Manual;
    std::string base, sub, name, key;   // key: "" while live
    int fighter = 0, number = 0;
};
std::string live_path(const ColourFile& f);
std::string aside_path(const ColourFile& f, const std::string& key);

// What a pass finds on disk
struct Listing {
    std::vector<ColourFile> files;                 // live and set aside
    std::map<std::string, std::string> records;    // key -> its colour_entries.txt (the entries set aside with the files)
    std::string state, chips, ticks;               // the Color Mod's three files ("" with has_* false: none)
    bool has_state = false, has_chips = false, has_ticks = false;
};
std::set<std::string> aside_keys(const Listing& in);   // identities with something set aside

// The pass: first every colour file leaving its name goes to a temporary name beside it (".colfollow_..."), then each goes
// to its place; a place taken when the move is made: to the fallback (set aside), never over a file. writes: our files and
// the Color Mod's three files written whole beside their place (<path>.colfollow_new), put in place at the end of the second
// phase (replace = true).
struct Op {
    std::string src, dst, fallback;
    bool replace = false;
};
struct Plan {
    std::vector<Op> first, second;
    std::vector<std::pair<std::string, std::string>> writes;   // path -> content
    std::vector<std::string> lines;                            // for the log
    int files = 0, entries = 0;
};
struct Place {
    std::string color_mods;   // reframework\color_mods
    std::string data;         // reframework\data\SF6_ColorSpinExtra_data
};
Plan make_plan(const std::vector<Step>& steps, const Listing& in, const Place& place,
               const std::function<bool(const std::string&)>& exists);
std::string journal_text(const Plan& plan, int phase);
bool parse_journal(const std::string& text, Plan& plan, int& phase);

// The Color Mod's files: what the steps do to the entries that name a number or an MC file
struct Renumber {
    std::map<std::pair<int, int>, std::pair<int, std::string>> move;   // (fighter, from) -> (to, the outfit's key)
    std::map<std::pair<int, int>, std::string> out;                    // (fighter, from) -> where they are set aside
    std::map<std::pair<int, int>, std::string> back;                   // (fighter, to) -> the key they come back from
};
struct Record {                       // .former_outfits\<key>\colour_entries.txt
    struct Chip { int fighter = 0, number = 0; std::string key, value; };   // chip_slots.json member (JSON text)
    struct Tick { int fighter = 0, number = 0; std::string id; };            // opponent_colours.txt line
    std::vector<Chip> chips;
    std::vector<Tick> ticks;
    bool empty() const { return chips.empty() && ticks.empty(); }
};
Record parse_record(const std::string& text);
std::string record_text(const Record& r);
struct Rewrite {
    std::string text;                  // the new file ("" = unchanged)
    int changed = 0;
    std::map<std::string, Record> add;    // key -> entries taken out of the file
    std::map<std::string, Record> left;   // key -> entries of its record not put back (their place was taken)
};
Rewrite rewrite_state(const std::string& text, const Renumber& r);
Rewrite rewrite_chips(const std::string& text, const Renumber& r, const std::map<std::string, Record>& records);
Rewrite rewrite_ticks(const std::string& text, const Renumber& r, const std::map<std::string, Record>& records);

// colour_owners.txt: the outfits the colours on disk belong to, and the registries it goes with (a digest of their slots:
// one written by another loader version since means the files follow that registry)
struct Owners {
    bool found = false;
    std::vector<Outfit> outfits;
    std::set<std::string> registries;
};
std::string registry_digest(const std::vector<Outfit>& slots);
bool parse_owners(const std::string& text, Owners& out);
std::string owners_text(const std::vector<Outfit>& outfits, const std::set<std::string>& registries);
// Who the colours on disk belong to. registry: registry.json's slots as this launch found them; labels: outfits.json's
// (fighter, number) -> label, "" for a Fluffy pak. source: what was trusted, for the log ("" = nothing known: first install).
std::vector<Outfit> choose_former(const Owners& owners, const std::vector<Outfit>& registry,
                                  const std::map<std::pair<int, int>, std::string>& labels, std::string& source);
std::map<std::pair<int, int>, std::string> outfit_labels(const std::string& outfits_json);

// Minimal JSON, enough for the Color Mod's files: keeps every token as written, prints as json.dump_file does
struct JVal {
    char t = 0;                                         // 'o' object, 'a' array, 's' string, 'v' number / true / false / null
    std::string raw;                                    // 's' and 'v': the token as written ('s' with its quotes)
    std::vector<std::pair<std::string, JVal>> members;  // 'o': (key token with its quotes, value)
    std::vector<JVal> items;                            // 'a'
};
bool json_parse(const std::string& text, JVal& out, std::string* tail = nullptr);
std::string json_dump(const JVal& v, const char* eol, int level = 0);   // eol "" = on one line
std::string json_unquote(const std::string& token);
std::string json_quote(const std::string& s);

// The launch's pass (the loader calls it once the new pak is in place, before saving registry.json)
struct RunInput {
    std::string game_dir;                  // the game folder (ANSI, as the loader has it)
    std::string registry_dir;              // reframework\data\SF6_Costumes_Data
    std::vector<Outfit> registry;          // registry.json's slots as this launch found them
    std::vector<Outfit> now;               // this launch's slots, with their labels
    std::function<void(const std::string&)> log;
};
void run(const RunInput& in);

}  // namespace colour_follow
