// SF6_ModSorter -- one dropped item: what it is, where it goes, its placement. See classify.cpp (the decision), place.cpp
// (the copy), gamedir.cpp (the game folder), ui.cpp (the window).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "texts.hpp"

enum class Category { Unknown, Costume, Stage, UIPack, TitlePack, Colours };
enum class ItemKind { Missing, Folder, Archive, Pak, ColourFile, Other };

const char* category_key(Category c);          // "costume", "stage", "ui_pack", "title_pack", "colours", "unknown"
TextId category_text(Category c);

struct Classification {
    std::wstring path;                 // as dropped
    std::wstring name;                 // its file or folder name
    ItemKind kind = ItemKind::Missing;
    std::string archive_kind;          // "zip", "7z", "rar"
    Category cat = Category::Unknown;
    std::vector<int> fighters;         // costumes and colours: the fighters it is for (esf numbers)
    std::vector<std::string> characters;   // their folder names, same order
    Msg reason;                        // not recognised: why
    std::vector<Msg> found;            // what was found inside, one line each
    // Where it goes, relative to the game folder: the folder, and the name it gets there
    std::wstring dest_dir, dest_dir_nsfw;  // dest_dir_nsfw: costumes only (NSFW Yes)
    std::wstring dest_name;            // the item's name (an archive named without its kind gets .zip / .7z / .rar)
    bool nsfw_guess = false;
    Msg nsfw_why;
    std::vector<std::string> nsfw_options;
    std::string loader_log;            // the costume loader's own lines (tests)
};

// What classification needs, loaded once per game folder (the worker does it before the first item)
struct ClassifyContext {
    std::wstring game_dir;             // "" when none
    std::wstring work_dir;             // the sorter's scratch folder (archives are unpacked there, as the loaders do)
    bool costume_ok = false, stage_ok = false;
    std::string costume_note, stage_note;
    std::vector<std::pair<int, std::string>> game_names;   // the game's own fighter names, for fighters the list lacks
};

void classify_init(ClassifyContext& ctx);
Classification classify(const std::wstring& path, const ClassifyContext& ctx, int seq);
std::string classification_json(const Classification& c, const ClassifyContext& ctx, bool verbose = false);

// The <Character> folder of a fighter (the costume_mods / color_mods spelling)
std::string fighter_folder(int fighter, const ClassifyContext& ctx);

// ---- placement (place.cpp) ----
struct PlaceResult {
    enum State { Installed, Skipped, Failed } state = Failed;
    std::wstring dest;                 // relative to the game folder
    Msg error;
};
// The full destination of an item (relative to the game folder) for a NSFW choice
std::wstring destination_rel(const Classification& c, bool nsfw);
bool destination_exists(const std::wstring& game_dir, const Classification& c, bool nsfw);
// progress: bytes copied, bytes in all (may be called from the copying thread)
typedef void (*CopyProgress)(void* ctx, uint64_t done, uint64_t total);
PlaceResult place_item(const std::wstring& game_dir, const Classification& c, bool nsfw, bool replace,
                       CopyProgress progress = nullptr, void* progress_ctx = nullptr);

// An installed costume moved between reframework\costume_mods and reframework\costume_mods_nsfw (the Installed page's N)
struct MoveResult {
    bool ok = false;
    std::wstring dest;                 // where it is now (ok) or would have gone, relative to the game folder
    Msg error;                         // not moved: why (nothing changed)
};
// The mod <folder>\<name> of costume_mods (from_nsfw false) or costume_mods_nsfw (true) renamed into the other one, same
// folder and name (folder "": the root's own, the mods for several characters). Never over an existing name.
MoveResult move_costume_root(const std::wstring& game_dir, bool from_nsfw, const std::wstring& folder, const std::wstring& name);

// ---- the game folder (gamedir.cpp) ----
bool is_game_dir(const std::wstring& dir);
bool game_process_running();                   // StreetFighter6.exe is running
std::wstring detect_game_dir();                // the exe's own folder, else Steam's libraries (app 1364780); "" when none
std::wstring settings_game_dir();              // the remembered one (settings.ini under %APPDATA%\SF6_ModSorter), "" when none
void save_settings_game_dir(const std::wstring& dir);
std::string settings_language();               // the language chosen on the Options page (a code of kLangCodes), "" when none
void save_settings_language(const std::string& code);
std::wstring pictures_cache_dir();             // the small pictures made of the mods' own (%LOCALAPPDATA%\SF6_ModSorter\thumbs)

// ---- utilities ----
std::string to_utf8(const std::wstring& w);
std::wstring from_utf8(const std::string& s);
std::wstring lower_w(std::wstring s);
bool file_exists_w(const std::wstring& p);
bool dir_exists_w(const std::wstring& p);
std::wstring long_path_w(const std::wstring& p);
void delete_tree(const std::wstring& dir);
