// report::logic - the pure parts of "Report a bug" (no game, no core, no file): what the guards promise, so the bench can hold them to it.
//   - the logs cleaned (clean_log) and capped (cap_log), every other cap;
//   - which outfit a costume row means and the costume key (one report per account per costume, on the server);
//   - what goes out (make_command), the menu's rules (Session, summary_lines): nothing leaves without Send, and Back leaves nothing behind.
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
constexpr size_t kJpegMaxBytes = 450 * 1000;    // the picture; the Worker refuses over 500 KB (the free plan's 10 ms of CPU a request)
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
//   game:<fighter>:c<number>                           a game outfit, its number (read on the screen's part)
//   game:<fighter>:<row's hash>                        a game outfit whose number could not be read: its text
//   unk:<fighter>:c<number> or <row's hash>            the mods' data could not be read
std::string costume_key(int fighter, const std::string& row, const json* outfit, const json* slot, bool data_read, int costume_no = -1);
// The row of outfits.json for that fighter and outfit number; nullptr = none (a game outfit)
const json* find_outfit_number(const json& outfits, int fighter, int costume_no);
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
// Wael, 07/11. Rows are real actions only. The first row is STATUS:
//   no report on this costume             OK (default): nothing else shows.     BUG: Describe the problem / Change the description, Send.
//   a report on the server, waiting       WAITING FOR FIX, no choice: the player cannot change it, only the admin's action moves it on. Change the description, Update.
//   the admin marked it fixed             TO TEST, OK, BUG. OK changes nothing yet: one row shows, MARK AS FIXED, and only that row sends it (the admin sees the tester says it is
//                                         fixed); moving STATUS away cancels. BUG: still broken: as a report waiting, WAITING FOR FIX once sent. A row's text never changes by itself.
//   the tester said it is fixed           OK, as with no report; BUG: the old text, Update (the row on the server is replaced).
// Update does nothing until the text differs from what was sent: dimmed. Under the rows: Character, Outfit, Description only; all the rest still goes, it is just not listed.
// The messages (sent, not sent, what is missing, what Update replaces, what goes with a report) are on the guide's line, message_text.
// The status comes from the server when the menu opens: STATUS says Checking... meanwhile, and if the server cannot be reached the message says so; OK and BUG still work.
// These rules live here only, to be easy to change.
enum class Stage { Idle, Shooting, Open };
enum class Server { Checking, Unknown, None, Waiting, Fixed, Confirmed };
enum class Pick { Ok, Bug, Held };              // Held: the status the server has (WAITING FOR FIX, TO TEST, Checking...)
enum class Busy { None, Sending, Status };
enum class Row { Status, Text, Send, MarkFixed };

// The server's answer to "my report on this costume": { exists, status, text }
Server server_from(bool exists, const std::string& status);
// The action the status request carries for a change the player made: "" = none
inline const char* action_confirm() { return "confirm"; }

// One report's life in the menu. Game thread only.
class Session {
public:
    Stage stage() const { return stage_; }
    Server server() const { return server_; }
    Pick pick() const { return pick_; }
    Busy busy() const { return busy_; }
    // The key: a new report. false = one is already up.
    bool begin();
    // The picture is taken, or given up: the menu may open
    void shot_settled() { if (stage_ == Stage::Shooting) stage_ = Stage::Open; }
    // The server's answer when the menu opened. text: what it holds. Nothing the player has typed meanwhile is overwritten.
    void server_answer(Server s, const std::string& text);
    void server_unreachable() { if (server_ == Server::Checking) { server_ = Server::Unknown; pick_ = Pick::Ok; } }
    bool has_report() const { return server_ == Server::Waiting || server_ == Server::Fixed || server_ == Server::Confirmed; }
    // The STATUS row: its options, as locale keys, and which one is shown
    std::vector<std::string> options() const;
    int option_index() const;
    // The player moved STATUS. true: something changed.
    bool choose(int index);
    // The action rows under STATUS
    std::vector<Row> rows() const;
    void set_text(const std::string& text) { text_ = text; text_typed_ = true; outcome_ = Outcome::None; error_.clear(); }
    const std::string& text() const { return text_; }
    void set_text_field_ok(bool ok) { field_ok_ = ok; }
    bool needs_text() const { return field_ok_ && text_.empty(); }
    // Update, while the text is what the server has
    bool update_blocked() const { return has_report() && text_ == sent_text_; }
    bool send_disabled() const { return needs_text() || update_blocked(); }
    // The Send / Update row: true = it goes out now. Never twice at once; never with nothing to say while the field works; never the same text again.
    bool send();
    // MARK AS FIXED's row: the status request goes out now
    bool start_status();
    enum class Outcome { None, Sent, Failed };
    void answered(bool sent, const std::string& error);         // Send's answer: sent = WAITING FOR FIX
    void status_answered(bool ok, Server now, const std::string& error);
    Outcome outcome() const { return outcome_; }
    const std::string& error() const { return error_; }
    // The window closed, for any reason. true = nothing is on its way: the caller drops the picture and all (this object already dropped
    // the text). false = a request was sent before the close: its answer still comes, answered() ends it.
    bool closed();
    bool window_gone() const { return window_gone_; }
private:
    void reset();
    Stage stage_ = Stage::Idle;
    Server server_ = Server::Checking;
    Pick pick_ = Pick::Held;
    Busy busy_ = Busy::None;
    std::string text_, sent_text_, error_;
    bool field_ok_ = true, window_gone_ = false, text_typed_ = false;
    Outcome outcome_ = Outcome::None;
};

using Tr = std::function<std::string(const std::string& key, const std::vector<std::string>& args)>;
// What the text under the rows names
struct MenuValues { std::string character, outfit, colour; };
// A row's title in the player's language
std::string row_title(const Session& s, Row row, const Tr& tr);
// The list under the rows, as lines to wrap at the window's width: Character, Outfit, the label of the description. Empty: nothing shows.
struct SummaryLine { std::string text; int max_lines = 1; };
std::vector<SummaryLine> summary_lines(const Session& s, const MenuValues& v, const Tr& tr);
// All of it wrapped, kSummaryLines at most: the list, which stays still, then the description's lines from the top scroll_top says. wrap(paragraph, max lines) is the UI's measure
// (never squeezed: lines cut with "..."). frames: how long this description has been up. scroll, when given: what the bar beside the description follows.
constexpr int kSummaryLines = 12;
constexpr int kDescriptionMaxLines = 60;
struct SummaryScroll { int first_line = 0, total = 0, window = 0, top = 0; };
using Wrap = std::function<std::vector<std::string>(const std::string&, int)>;
std::string summary_text(const Session& s, const MenuValues& v, const Tr& tr, const Wrap& wrap, uint64_t frames, SummaryScroll* scroll = nullptr);
// The first line a window of `window` lines shows of `total`, after `frames`: rest, a line at a time, rest, back to the top, and again. No key needed.
constexpr uint64_t kScrollRestFrames = 150, kScrollStepFrames = 120;
int scroll_top(int total, int window, uint64_t frames);
// CJK, kana and Hangul characters
size_t wide_chars(const std::string& s);
// The width in text_em's terms to wrap this line at so that it holds in `em`: a wide character draws about 1.35 of what text_em counts (the game's font is condensed for Latin
// only: what took Japanese past its room on 07/11).
constexpr double kWideDrawn = 1.35;
// How wide a text draws at the text-under-the-rows' font, from the table of text_em (given by the caller, em_of(one character)): never less than the game draws, as far as it was read
// in game (07/11): a capital 'A' 12.8 px, a digit 11.6, lower-case text 8.9 a character. The game's font is condensed unevenly: one figure per em cannot hold for "AAAA...".
using EmOf = std::function<double(const std::string&)>;
double drawn_px(const std::string& s, const EmOf& em_of);
// Lines at most max_px wide: at spaces, inside a word that does not fit a line. Past max_lines the last one is cut with "...".
std::vector<std::string> wrap_px(const std::string& text, double max_px, int max_lines, const EmOf& em_of);
// What Ctrl+V puts in the text field (UTF-16): line breaks as /n (edit_result turns them back), control characters out, < and > dropped as the game does, max_units at most.
std::wstring paste_text(const std::wstring& clipboard, size_t max_units);
// The message on the guide's line: "" = none. focus: the row with the cursor. One message at a time, the most urgent.
std::string message_text(const Session& s, Row focus, const Tr& tr);
// A text of a player's or a mod's for a game text: < > [ ] made ( ), one line (an unknown message tag crashes the game's render thread)
std::string shown_safe(const std::string& s);
// The error's text: known server codes in the player's language, anything else the general one
std::string error_key(const std::string& code);

// ---- asleep / awake --------------------------------------------------------------------------------------------------------------------
// While awake, on each frame: the calls of the screen's own update, counted by its hook. Nothing here knows of Training or of the UI's claim.
constexpr uint64_t kLeftFrames = 20;            // frames with no call: the screen's flow has left its main state (a long frame is one frame)
class LeftWatch {
public:
    bool seen() const { return have_; }         // false: the first frame of this waking
    void beat(uint32_t beats, uint64_t frame) { if (!have_ || beats != beats_) { have_ = true; beats_ = beats; at_ = frame; } }
    bool left(uint32_t beats, uint64_t frame) { beat(beats, frame); return frame - at_ > kLeftFrames; }
    void reset() { have_ = false; }
private:
    bool have_ = false;
    uint32_t beats_ = 0;
    uint64_t at_ = 0;
};

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
