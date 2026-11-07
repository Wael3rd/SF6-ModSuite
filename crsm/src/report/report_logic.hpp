// report::logic - the pure parts of "Report a bug" (no game, no core, no file): what the guards promise, so the bench can hold them to it.
//   - the logs cleaned (clean_log) and capped (cap_log), every other cap;
//   - which outfit a costume row means and the costume key (one report per account per costume, on the server);
//   - what goes out (make_command), the menu's rows (menu_rows) and the rule of its life (Session): nothing leaves without Send, and
//     Back leaves nothing behind.
// The game side is ui/bug_report.cpp, the files report/gather.cpp, the picture report/capture.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace report {

using json = nlohmann::json;

// ---- the caps ----------------------------------------------------------------------------------------------------------------
constexpr size_t kTextMaxUnits = 1000;          // the text field (UTF-16 units): the most the game's field was tried with (04/11)
constexpr size_t kJpegMaxBytes = 700 * 1024;    // the picture; the Worker refuses over 800 KB
constexpr size_t kLogMaxBytes = 64 * 1024;      // each log: its start (where it says what was loaded) + its end (what just happened)
constexpr size_t kLogHeadBytes = 8 * 1024;
constexpr size_t kRecordMaxBytes = 16 * 1024;   // the outfit's record (the Worker refuses over 32 KB)
constexpr size_t kModsMaxBytes = 96 * 1024;     // the list of installed mods (the Worker refuses over 128 KB)
constexpr size_t kNoteMaxBytes = 300;

// The only logs a report may read: ours, by exact path below the game folder. Nothing else is ever opened, hook_trace.txt least of all.
struct LogFile { const char* key; const char* path; };
inline constexpr LogFile kLogs[] = {
    { "loader", "SF6_CostumeLoader.log" },
    { "plugin", "reframework/data/SF6_CostumeSlots_data/native_log.txt" },
};

// ---- cleaning -----------------------------------------------------------------------------------------------------------------
struct Scrub {
    std::vector<std::string> names;             // the Windows user name, the PC's name (replaced wherever they appear, any case; 3 bytes at least)
    std::string game_root;                      // the game folder: "<game>" in every path
};
// The text with: any path under a user's folder cut down to "<user>" (with its drive, both slashes, escaped, long-path prefix), the game folder to
// "<game>", the names to "<user>", and whatever looks like a platform ID to "<id>": a SteamID64, Steam's other forms ([U:1:n], STEAM_0:1:n),
// a Steam profile address, any lone run of 9 digits or more (CFN short ids, Xbox and PSN numbers).
std::string clean_log(const std::string& text, const Scrub& s);
// The paths and names only: for what is no log (a mod's file name may hold a long number, and no platform writes those)
std::string clean_paths(const std::string& text, const Scrub& s);
// At most max_bytes: its first head_bytes + its end, on line and UTF-8 boundaries, with a line saying how much was left out
std::string cap_log(const std::string& text, size_t max_bytes = kLogMaxBytes, size_t head_bytes = kLogHeadBytes);
// At most max_bytes on a UTF-8 boundary
std::string cut_utf8(const std::string& s, size_t max_bytes);

std::string sha256_hex(const void* data, size_t n);
std::string fnv64_hex(const std::string& s);
std::string base64(const uint8_t* p, size_t n);

// ---- the outfit -----------------------------------------------------------------------------------------------------------------
// A costume row's text without what mods add to it ("  <ICON ...")
std::string row_name(const std::string& row);
// The row of outfits.json's "outfits" this costume row shows: its "name" ("Outfit VII", in every language) or its label (what names-scroll's
// loader writes in the game's own list). Manager mode has one row per variant: the same rule. nullptr = none (a game outfit).
const json* find_outfit(const json& outfits, int fighter, const std::string& row);
// The slot of registry.json for an outfit: nullptr = none
const json* find_slot(const json& registry, int fighter, int costume_no);
// The costume's identity on the server, /^(mod|game|unk):[a-z0-9:._-]{1,72}$/:
//   mod:<mod id>:f<fighter>:c<outfit it is based on>   one of ours: its mod's id, so a mod's update allows a new report
//   game:<fighter>:<row's hash>                        a game outfit (its text, as no number can be read on this screen)
//   unk:<fighter>:<row's hash>                         the mods' data could not be read
std::string costume_key(int fighter, const std::string& row, const json* outfit, const json* slot, bool data_read);
bool costume_key_ok(const std::string& key);

// ---- what goes out ---------------------------------------------------------------------------------------------------------------
struct Report {
    std::string costume_key, text, character, outfit, outfit_row, colour;
    int fighter = -1, costume_no = -1;
    json versions = json::object(), logs = json::object(), record = nullptr, mods = json::array(), notes = json::array();
    std::vector<uint8_t> jpeg;                  // empty: a report without a picture, its reason in notes
};
// The Community command: { cmd = "report", ... } (protocol.hpp). No identity: the server takes the account from the verified token.
json make_command(const Report& r);

// ---- the menu ---------------------------------------------------------------------------------------------------------------------
enum class Stage { Idle, Shooting, Open, Sending };
enum class Row { Text = 0, Shot, Outfit, Versions, Logs, Steam, Send, Count };

// The life of one report. Game thread only.
class Session {
public:
    Stage stage() const { return stage_; }
    // The key: a new report. false = one is already up.
    bool begin();
    // The picture is taken, or given up: the menu may open
    void shot_settled() { if (stage_ == Stage::Shooting) stage_ = Stage::Open; }
    void set_text(const std::string& text) { text_ = text; }
    const std::string& text() const { return text_; }
    void set_text_field_ok(bool ok) { field_ok_ = ok; }
    // The Send row: true = goes out now. Never twice; never with nothing to say while the field works.
    bool send();
    bool needs_text() const { return field_ok_ && text_.empty(); }
    enum class Outcome { None, Sent, Failed };
    // The server's answer, or a failure before it
    void answered(bool sent, const std::string& error);
    Outcome outcome() const { return outcome_; }
    const std::string& error() const { return error_; }
    // The window closed, for any reason. true = nothing is on its way: the caller drops the picture and all (this object already dropped
    // the text). false = the report was sent before the close: its answer still comes, answered() ends it.
    bool closed();
    bool window_gone() const { return window_gone_; }
private:
    void reset();
    Stage stage_ = Stage::Idle;
    std::string text_, error_;
    bool field_ok_ = true, window_gone_ = false;
    Outcome outcome_ = Outcome::None;
};

using Tr = std::function<std::string(const std::string& key, const std::vector<std::string>& args)>;
struct RowText { Row row; std::string title, desc; };
struct MenuInput {
    const Session* session = nullptr;
    std::string what;                           // "Ryu, Aloha Ryu, Color 3": what the screen shows
    bool shot_ok = true;
    double row_em = 0;                          // what a row holds, in em
    std::function<std::string(const std::string&, double)> cut;   // a text cut to that many em (with its "...")
};
std::vector<RowText> menu_rows(const MenuInput& in, const Tr& tr);
// A text of a player's or a mod's for a game text: < > [ ] made ( ), one line (an unknown message tag crashes the game's render thread)
std::string shown_safe(const std::string& s);
// The error's text for the Send row: known server codes in the player's language, anything else the general one
std::string error_key(const std::string& code);

// ---- the guide entry and the key ---------------------------------------------------------------------------------------------------
// The scancode of a guide symbol's key ("<ICON KeyR>": the game names its keys at their QWERTY place), 0 when it names no letter or digit
unsigned qwerty_scan(const std::string& symbol);

// What must hold for the entry and the key on Fighter Settings. The agent and the main control stay drawn and ActualVisible on hidden tabs,
// so they alone prove nothing: the tab is the screen's, and the costume row must really show.
struct ScreenRead {
    bool agent_drawn = false;                   // the agent's GameObject is drawn and its main control ActualVisible
    int tab = -1;                               // _TabItem.SelectedIndex: 0 Character, 1 Titles, 2 Profile; -1 unread
    bool costume_row = false;                   // p_Spin_costume ActualVisible
    bool input_on = true;                       // the agent takes input (no dialog of the game's over it)
    bool window_open = false;                   // any client's option window
    bool dialog_up = false;                     // CRSM's text dialog, box, Community dialog
    bool key_taken = false;                     // the bar's own entries show the same key: the game uses it here
    bool can_report = true;                     // the Community client is on
};
bool entry_allowed(const ScreenRead& s);

}  // namespace report
