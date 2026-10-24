// ui::logic — the pure part of the DLL's own screens (phase P4, owner "ui"): the windows over 30 slots / 30 reversal
// rows, the cursor answers at the block edges, the arrows, the slot name fit, the game's learnt wording, the text dialog's
// outcome, the Community dialog (pages, sort, votes, and the cursor kept after a list refresh: Wael's fix). No game type,
// no REFramework: unit-tested offline (tests/ui_bench.cpp).
//
// Sources (line numbers of the shipped Lua, e8ac8cf): RecordingManagerTab.lua (RMT) 648-653, 677-700, 843-918 (windows,
// texts), 949-956, 1246-1278 (Replay), 1613-1683 (text dialog), 1756-1881 (cursor), 289-394 (Community);
// ReversalSlots.lua (RSL) 24, 337-353 (arrows), 530-571 (reversal window and cursor).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace ui::logic {

// ---- the Record / Replay windows (RMT:648-653) ------------------------------------------------------------------------
constexpr int kSlotTotal = 30;                 // SLOT_TOTAL = 8 + VIRTUAL_SLOTS
constexpr int kWindow = 8;                     // WINDOW: the game's 8 slot rows
constexpr int kMaxWin = kSlotTotal - kWindow;  // the last window: slots 23-30
constexpr int kFirstRow = 2, kLastRow = 9;     // FIRST_ROW, LAST_ROW: list indices of the slot rows (0 strip, 1 header)
constexpr int kWorkSlot = 7;                   // WORK_SLOT: live slot 8, 0-based

int clamp_win(int w);                          // math.max(0, math.min(SLOT_TOTAL - WINDOW, w))
// set_window (RMT:678-687): row k shows slot win + k; its _SlotID is n - 1 for a live slot, the work slot for 9-30. The
// Lua's `big` there is read before its declaration (always nil): Record rows 9-30 keep _SlotID 7 even while the 30-entry
// array is installed. Kept on purpose: F on them records through slot 8.
int record_slot_id(int n);
// set_rwindow (RMT:949-956): n - 1 while the 30-entry array is in place (every slot reads its own entry), else as Record
int replay_slot_id(int n, bool big);
// The arrow text of row k (1-8) of a window at `win` (RMT:892-894, 1234): "▲" on the first row when slots lie above,
// "▼" on the last when slots lie below, else ""
const char* window_arrow(int k, int win);

// ---- the reversal pages: 30 rows through the game's 10 (RSL:24, 351) -------------------------------------------------
constexpr int kRevTotal = 30, kRevPage = 10, kRevMaxWin = kRevTotal - kRevPage;
// idx = list index 1-10 of the row, win = first shown row - 1
const char* reversal_arrow(int idx, int win);

// ---- the cursor at the block edges (UIPartsListBase.GetNextIndex / GetPrevIndex post-hooks) ----------------------------
// What the post-hook does: optionally move the window to `win` (and redraw the rows), then either return `index` instead
// of the game's answer (replace) or leave the game's answer.
struct CursorAnswer {
    bool move = false; int win = 0;
    bool replace = false; int index = 0;
};
// Record page (RMT:1856-1871). cur = the row the cursor is on, nxt = the game's answer, dir = +1 (GetNextIndex) or -1,
// outside = the pre-hook saw the cursor just outside the slot block (strip / Recording Trigger / Countdown rows).
CursorAnswer record_cursor(int cur, int nxt, int dir, int win, bool outside);
// Replay page (RMT:1837-1855, replay_jump 1267-1278): the nearest valid (non-empty) slot that way, brought under a row.
// disabled(n) = slot n (1-30) is empty (its row greyed and skipped).
CursorAnswer replay_cursor(int cur, int nxt, int dir, int rwin, bool outside, const std::function<bool(int)>& disabled);
// Reversal pages (RSL:562-570, rev_shift 536-545): cur = 1..10, or 0 (the strip row). The window slides at the block's
// edges; at the ends of the 30 rows the game's wrap goes on to the other end (slot 30 Down -> slot 1, Up -> slot 30).
CursorAnswer reversal_cursor(int cur, int nxt, int dir, int win);

// ---- the slot name between "Slot N" and the frames (fit_name, RMT:849-875) ----------------------------------------------
struct FitIn {
    bool has_name = false; float name_w = 0;   // rendered width of "Slot N" (absent: 60)
    bool has_frames = false; float frames_w = 0;   // rendered width of the frames text (absent: 40)
    float own_w = 0;                           // rendered width of the name text now
    float font_w = 0;                          // its font size now
    float base_w = 0, base_h = 0;              // the font size copied at the borrow (the game's)
    float pos_x = 0;                           // its x now
};
struct FitOut { bool set_font = false; float font_w = 0, font_h = 0; bool set_x = false; float x = 0; };
FitOut fit_name(const FitIn& in);
constexpr float kFitLeftOrigin = -420.0f;      // "Slot N" is left-aligned at x -420 (RMT:855)
constexpr float kFitMargin = 26.0f;            // RMT:855-856
constexpr float kFitFill = 0.92f;              // RMT:857
constexpr float kFitMinRoom = 60.0f;           // RMT:857
constexpr float kFitMinScale = 0.5f;           // RMT:864

// ---- texts measured in em of the game's UI font: the Community rows, the loadout descriptions ------------------------------
// A Community row is an option entry of the core, built from its string: its rendered width cannot be read before it is
// built, so the width is estimated from Arial-like advance widths (each ASCII character its own, 1 em for CJK, full-width
// forms and "…", 0.6 em for any other character); the game's markup tags (<color ...>, </color>, <ICON ...>) count 0.
double text_em(const std::string& s);
// s when it fits max_em; otherwise its longest head (on a character boundary, trailing spaces dropped) + "…" that fits.
std::string cut_em(const std::string& s, double max_em);
// What one Community row holds, in em. Measured in game at 1920x1080 (24/10): the rows draw at 21.3 px per em of this
// estimate (829 px for 38.9 em, 734 px for 34.4 em, 968 px for 45.1 em: the same ratio within 1 %); the row's box is
// 800 px wide, 780 px usable: 36.5 em. reframework/data/SF6_CRSMNative_data/com_row_em.txt (a number, 15-80) replaces
// it, read when the dialog opens.
constexpr double kComRowEm = 36.5;
constexpr double kComPxPerEm = 21.3;           // (the probe dump's ratio check)
// The Manager page's help line (the training menu's bottom line): ~1700 px at ~20.5 px per em, kept under 78 em.
constexpr double kHelpLineEm = 78.0;
constexpr size_t kComNameMinChars = 12;        // a cut loadout name keeps at least this many characters (then "…")
constexpr size_t kComAuthorMinChars = 8;
// The Community row label fitted to budget_em. shown = the list item the label is built from (its uploader_name already
// the author shown, with its verified mark), dv = its downloaded version, full = model::labels::community_label(shown, dv)
// as built. Wider than the budget: the date goes first, then the loadout name is cut ("…", kComNameMinChars characters at
// least; a trailing tag of the name such as " [TEST]" is never cut: the name is cut before it), then the author's name
// (its mark kept, kComAuthorMinChars at least). The marks, the version, the slots, the score and the downloads always
// stay. Still wider: the shortest label made.
std::string com_row_fit(const nlohmann::json& shown, const nlohmann::json& dv, const std::string& full, double budget_em);
// A free text (a loadout's description) made safe for a game text, in at most max_lines lines of max_em: \r and other
// control characters dropped, tabs as spaces, '<' / '>' as '[' / ']' (the game's markup), each paragraph (\n) wrapped at
// word boundaries (a word wider than a line is split), empty lines dropped; past max_lines the last line kept ends with
// "…". The first 4000 bytes are read.
std::vector<std::string> desc_lines(const std::string& raw, double max_em, int max_lines);
// The same text on ONE line: its paragraphs joined with " | ", wrapped at a word boundary with "…" past max_em.
std::string desc_line(const std::string& raw, double max_em);
// The Community dialog's description area (UIFlowOptionBGDialog.Param.GuideMessageText, the game's own description of
// the focused unit, which CRSM's guide line replaces): `extra` lines first, then the guide line.
std::string com_guide_text(const std::vector<std::string>& extra, const std::string& guide);

// ---- the game's own wording, learnt on its rows ------------------------------------------------------------------------
// name_fmt (RMT:880-883): the game's "Slot 1" with its first run of digits replaced by the number
bool learn_name_format(const std::string& msg, std::string* prefix, std::string* suffix);
std::string slot_text(bool learnt, const std::string& prefix, const std::string& suffix, int n);   // "Slot %d" when not learnt
bool is_frames_text(const std::string& msg);   // msg:match("^%d+%s*F$")
// empty_word (RMT:888): a centre text the game wrote that is neither "" nor a frames text nor nil ("nil" = a null text)
bool learnable_empty_word(const std::string& msg);
// start_text (RMT:896): the focused row's right column when it is the game's own text (not an arrow, not empty / nil)
bool learnable_start_text(const std::string& msg);
extern const char* const kUp;                  // "▲" U+25B2
extern const char* const kDown;                // "▼" U+25BC

// ---- the text dialog's outcome (finish_naming / apply rules, RMT:1639-1647, 1652-1683) ----------------------------------
struct NameJob {
    std::string initial;                       // the text the dialog opened with
    bool decided = false;                      // a decide (InvokeResultEvent, Decide, InputEvent_Decide)
    bool cancelled = false;                    // Cancel / InputEvent_Cancel
    std::optional<std::string> raw;            // TextInputDecided(String), or the field read at a decide
    std::optional<std::string> result;         // the InvokeResultEvent string
};
// At a decide (Decide / InputEvent_Decide): the field's text becomes `raw` when raw is absent or empty (RMT:1674)
void naming_decide(NameJob& job, const std::optional<std::string>& field);
// What finish_naming applies (nullopt = nothing): cancelled -> nothing; text = raw, else result, else the field, else "";
// nothing when there was no decide and the text is the initial one. `field` = dialog_text(job) at the finish (nullopt when
// the dialog's field and input part are both empty).
std::optional<std::string> naming_outcome(const NameJob& job, const std::optional<std::string>& field);

// ---- raw key edges (RMT:329-337, 1757-1761) ----------------------------------------------------------------------------
struct KeyEdge {
    bool down = false;
    bool press(bool now) { const bool e = now && !down; down = now; return e; }
    void reset() { down = false; }
};

// ---- the name keys of the pause menu: the one reader of T / pad Y and G / R / pad X (RMT:1755-1770) ----------------------
// P4's slot rows and P5's Manager page share the keys: one reader (ui/windows.cpp), one decision. t / del = this frame's
// key edges (T or pad Y; G, R or pad X); busy = a text dialog or a box is up or asked, or the Community dialog (RMT:1762:
// `naming or confirm_box`); ready = the pages are installed and the Record rows captured (`installed and phys`); manager =
// the Manager page is the one shown (P5's page_shown, RMT:1764 manager_shown).
enum class NameKey { None, RenameLoadout, NameSlot, DeleteSlotName };
NameKey route_name_key(bool t, bool del, bool busy, bool ready, bool manager);

// ---- the pause menu's tab under CRSM's dialogs (RMT:1717-1745, 2044-2058) ---------------------------------------------------
// L1 + R1 together (keyboard A + E, the Community list's page keys) is a shortcut of the training menu, read by a
// TrainingManager update while the menu is shown, also under a dialog (no UIAgent tab input). Read in the exe (build of
// 13/10, code 0x145CC5A80): _tData.SelectIndex = 0 and SelectChildIndex = 12, then the primary tab list (Param._PrimaryList,
// a via.gui.ScrollList: its selected index is the game's real tab, the one L1 / R1 move from) setSelectedIndex(center
// offset, false), then InitSecondaryList (CreateViewData: the page rebuilt from the data), a sound, _PrimaryTab
// .set_PageIndex(SelectIndex). The list's selection change also runs UpdatePrimaryList. Held here: the spot the menu was
// on when the dialog opened (the data's tab and row, the list's selected index), until no dialog of CRSM is up. 2.0.1's
// first fix put the data back only: the page stayed right, the list stayed on the first tab (Wael 14/10: E after the
// dialog went to the second tab).
// Which dialogs hold the tab: the text dialog and the Community dialog (they also block every agent's tab inputs,
// RMT:1729-1733), a box only under the pause (box_tick polls it there: a box left up with the menu closed blocks nothing).
bool tabs_held(bool naming, bool community, bool box, bool paused);
// _tData.SelectIndex / SelectChildIndex, the primary tab list's selected index (-1 = not read)
struct TabSpot { int64_t tab = -1, child = -1, list = -1; };
struct TabFix { bool tab = false, child = false, list = false; TabSpot to; };   // what to write back (to = the spot held)
// Where the guard is asked: the frame callback (the only place a spot is kept: the frame a dialog opens on, before any
// input reaches it; the list's selection is read and put back there only, never from inside the game's call chain),
// CreateViewData's pre (the rebuild reads the tab and the cursor row from the data: both put back, as the Lua did under
// the text dialog), InitTabItem's pre (the tab items set up from the data after the rebuild: the tab).
enum class TabAt { Frame, Rebuild, Items };
class TabGuard {
public:
    // modal = a dialog of CRSM is up (tabs_held); now = the data read now. A first call under a dialog at the frame keeps
    // the spot; at a rebuild it keeps `fallback` (>= 0: the tab the dialog belongs to, the Recording tab for the text
    // dialog; the row unknown), else nothing (the game's way until the next frame). Without a dialog: everything
    // forgotten, nothing to do.
    TabFix check(bool modal, const TabSpot& now, TabAt at, int64_t fallback = -1);
    bool holding() const { return held_; }
    const TabSpot& held() const { return spot_; }

private:
    bool held_ = false;
    TabSpot spot_;
};
// The primary tab list's setSelectedIndex(value) pre-hook (the shortcut's own move, at its source): skipped under a dialog
// of CRSM unless it puts back the held selection (held_list < 0: not read yet, every move skipped). primary = the object is
// the training menu's _PrimaryList (any other list of the game: never).
bool block_list_select(bool modal, bool primary, int64_t held_list, int64_t value);

// ---- the layers' asynchronous tickers (mech::push_ticker: an import finished after an allocation...) ----------------------
// Lua = left for frame_poll (the Lua UI shows them: owner ui lua, or not claimed); Show = the DLL's ticker strip (ui runs);
// Drop = the DLL's UI runs but its GetMessage hook is missing (no ticker of CRSM at all: dropped, counted).
enum class TickerSink { Lua, Show, Drop };
TickerSink ticker_sink(bool ui_active, bool ticker_hook);

// ---- after a Community download (RMT:276-278: invalidate_files, ensure_files, manager_dirty) -----------------------------
// refresh_manager = P5's ui::manager::refresh_list(): the page re-reads the list and syncs its spin on the next frame (one
// glob); false = no Manager page running: the files layer re-reads at once (invalidate, then ensure).
void after_download(const std::function<bool()>& refresh_manager, const std::function<void()>& invalidate,
                    const std::function<void()>& ensure);

// ---- the Community dialog ("Download Loadouts", RMT:289-394) -------------------------------------------------------------
// The page (community::Page) and the option window (the core's opt_*) behind one port, so the dialog's rules run offline.
class ComPort {
public:
    virtual ~ComPort() = default;
    // the page
    virtual void page_open() = 0;                             // Page::open (char, disk sync, sort New, page 1, fetch)
    virtual std::string title() = 0;
    virtual bool loading() = 0;
    virtual int row_count() = 0;                              // rows built, from row 1
    virtual std::string row_label(int row) = 0;               // 1-based
    virtual std::string row_desc(int row) = 0;                // "F: Download  |  <uploader>  <created>" (+ its description)
    virtual bool data_changed() = 0;                          // an answer arrived since the last build
    virtual void mark_built() = 0;
    virtual int64_t version() = 0;
    virtual bool prev_page() = 0;
    virtual bool next_page() = 0;
    virtual void cycle_sort() = 0;
    virtual void vote(int row) = 0;                           // the tickers of a refused vote come from the page
    virtual bool download_prompt(int row, std::string* message) = 0;
    virtual void download_confirmed() = 0;
    // the window
    virtual void show(const std::string& title, const std::vector<std::string>& labels, const std::vector<std::string>& descs) = 0;
    virtual void open_menu() = 0;                             // the window opens on the page (default focus)
    virtual void navigate(int focus0) = 0;                    // rebuild in place, cursor on row focus0 + 1
    virtual int focused_row() = 0;                            // the row under the cursor, 1-based (0 = unknown)
    virtual void confirm(const std::string& message, std::function<void(bool yes)> done) = 0;   // title "Community"
};

class ComDialog {
public:
    static constexpr int kRows = 7;                           // _com.ROWS
    explicit ComDialog(ComPort& port) : p_(port) {}

    void open();                                              // the Manager's "Download Loadouts"
    void closed();                                            // the window or the dialog ended (Esc / B)
    // once per frame while the dialog is up: the raw keys A, E, T, R
    void tick(bool key_a, bool key_e, bool key_t, bool key_r);
    void button(int row);                                     // F / click on a row (1-based)
    void guide_focus(int row) { if (row >= 1) guide_focus_ = row; }   // the row the guide refresh saw focused
    bool is_open() const { return open_; }

    // what the window shows (for the tests and the journal)
    std::vector<std::string> labels() const { return labels_; }
    int last_focus() const { return last_focus_; }

private:
    void render();                                            // title + 7 rows to the window
    int current_row();                                        // the cursor row now (1-based)

    ComPort& p_;
    bool open_ = false;
    KeyEdge a_, e_, t_, r_;
    int64_t shown_version_ = -1;
    int vote_row_ = 0;                                        // the row of the last T: the vote's answer comes back there
    int guide_focus_ = 0;
    std::vector<std::string> labels_;
    int last_focus_ = -1;
};

}  // namespace ui::logic
