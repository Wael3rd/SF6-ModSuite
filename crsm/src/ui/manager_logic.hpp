// ui/manager_logic — the Manager page of the Recording strip (phase P5), game-free:
// the rows and their texts, the loadout spin (labels, value, selection), the sub-page memory (RecordType), the choice of
// the key-guide hints, the queue of actions and the actions themselves on two ports (the files layer, the screen).
// The game side (ui/manager_page.cpp) plugs the Native Core's pause-menu service (pm_*), the files layer
// (mech::files::engine) and P4's dialogs (ui/dialogs.hpp) into it; the offline bench (tests/p5_bench.cpp) drives
// the same code with fakes.
//
// Source: RecordingManagerTab.lua (RMT) of v1.6: 56-146 (the page, the spin, Open / Save / Save As / Reset / Erase),
// 396-427 (Share, Download), 1449-1460 (the name dialogs), 1555-1563 (hints), 1611-1626 (apply_naming), 1755-1768 (T on
// the Manager page), 2022-2065 and 2111 (RecordType).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ui::mgr {

constexpr int32_t kRecordFunc = 4;            // TrainingFuncType.RECORD: the "Recording Settings" primary tab (RMT:31)
constexpr int32_t kStripIndex = 0;            // Manager > Record > Replay (RMT:1840-1841)
constexpr uint32_t kSpinCapacity = 64;        // PACK_CAPACITY: option slots built once (RMT:56)
constexpr float kArrowSpread = 70.0f;         // the spin's arrows pushed 70 px apart (RMT:68)
constexpr size_t kNameMax = 64;               // NAME_MAX of the text input dialog (RMT:1316)
constexpr int64_t kFtRecordSlot = 118;        // TrainingFuncType RECORD_SLOT: the Record page's 8 slot rows (RMT:525)
constexpr int32_t kButtonX = 92;              // app.InputAssign.Digital.ConfigId UI_X (core/docs/PAUSE_MENU.md, Hints)
constexpr int32_t kButtonY = 93;              // UI_Y
constexpr uint64_t kNativeActionHold = 240;   // MAIN's own tickers kept quiet 240 frames after the menu closes (RMT:1157)
constexpr const char* kNoFile = "-- Select File --";
// The page guide stays English, as in v1.6 (RMT:1831: the tab's guide is hard-coded, rsm.page_desc is not used there).
constexpr const char* kPageGuide = "Recording slot manager (SF6 Tools).";

// ---- rows ---------------------------------------------------------------------------------------------------------------
enum Row : int { kLoadout = 0, kOpen, kSave, kSaveAs, kReset, kErase, kShare, kDownload, kRowCount };

using Tr = std::function<std::string(const std::string& key, const std::vector<std::string>& args)>;   // RMT's T(key, ...)

struct RowText { std::string title, guide; };
RowText row_text(Row r, const Tr& T);         // rows 1-6 localized (rsm.*), Share / Download English (RMT:397, 418)
std::string page_title(const Tr& T);          // rsm.page "Manager"

// ---- the loadout spin --------------------------------------------------------------------------------------------------
// The option labels: the files layer's dropdown ("-- Select File --", display names...) cut by short_list (64 bytes, "…"),
// at most kSpinCapacity (the loadouts past it are not reachable, as in v1.6).
std::vector<std::string> spin_labels(const std::vector<std::string>& dropdown);
// The spin's value: the 1-based dropdown index as a 0-based option, clamped to the options shown.
int32_t spin_value(int dropdown_index, size_t n_labels);
// on_spin(index): the dropdown index (1-based) to select. The core already wrapped the index (Q / D loop).
int dropdown_of_spin(int32_t index, size_t n_labels);

// ---- sub-page memory -------------------------------------------------------------------------------
// The menu reopens on the last sub-page used: just before the game's CreateViewData on the Recording tab, RecordType is
// set to that sub-page (0 Manager, 1 Record, 2 Replay); the game's Function(107 / 108) gives RecordType its own meaning
// back (0 Record, 1 Replay) when it builds its pages, and the game picks _ChildData[RecordType] (one page to the left in
// our 3-page strip). Game pages come from P4's Function pre-hook (game_page, RMT:2012-2014). The core's edges cover our
// page only: it says shown 0 when OUR page stops being shown, never for Record <-> Replay (pm_core page_set_not_shown):
// the edge alone left the memory on the first game page after ours (fix 14/10: "left on Replay, reopened on Record").
class PageMemory {
public:
    enum class Page : int { None = -1, Manager = 0, Record = 1, Replay = 2 };
    // on_page_shown(page, shown): 1 = ours was built. 0 with the menu up = a game page of the strip was built after ours
    // (pending until game_page or RecordType says which, the Function hook may run before or after the core's);
    // 0 with the menu closed = the menu closed or the page was removed: kept.
    void page_shown(bool shown, bool menu_up);
    // Function(107 / 108): the game built its Record / Replay page (any order with the core's edge).
    void game_page(Page p);
    // A game page is pending: RecordType holds its game meaning now (0 Record, else Replay). Called at the menu close
    // and before a write.
    void resolve(int64_t record_type_now);
    bool unresolved() const { return pending_game_; }
    // The RecordType to write before CreateViewData (Recording tab only). Nothing built yet in this training session:
    // the game's own memory (RecordType 0 Record, else Replay) moved onto our strip, never one page to the left.
    std::optional<int64_t> record_type(int64_t record_type_now);
    Page page() const { return page_; }
    void reset() { page_ = Page::None; pending_game_ = false; }
private:
    Page page_ = Page::None;
    bool pending_game_ = false;
};
// RMT:2111: never left above 1 with the menu closed.
bool record_type_needs_reset(int64_t record_type, bool menu_up);
// A RecordType write only with our page in the Recording strip (3 pages) and onto one of its pages: a 2-page strip (ours not
// installed yet, early in training, or taken out) is the game's own, where RecordType keeps its meaning (2 = out of range).
bool record_type_fits(int64_t want, int32_t strip_pages);
const char* page_name(PageMemory::Page p);   // "Manager" / "Record" / "Replay" / "none" (logs, status)

// ---- hints (RMT:1555-1563) ---------------------------------------------------------------------------------------------
struct HintSpec { int32_t button; const char* key; };
// "Edit slot name" (Y) and "Delete slot name" (X) on the Record page's slot rows only: none on the client's own rows
// (pm_row_of >= 0) and none on any other game row.
std::vector<HintSpec> hints_for(bool own_row, bool func_known, int64_t func_type);

// ---- actions ------------------------------------------------------------------------------------------------------------
enum class Act { Select, Ensure, Refresh, Open, Save, SaveAs, Reset, Erase, Share, Download, Rename };
struct Action { Act act = Act::Ensure; int32_t arg = 0; };
const char* act_name(Act a);
std::optional<Act> act_of_row(int row);       // the buttons' decide (rows 1-7); nullopt for the spin

// The callbacks run on the game's UI job threads (R29), the actions on the game thread: a decide or a spin move is
// queued here and run by the frame callback, in order (no file I/O on a menu thread).
class Queue {
public:
    void push(Action a);
    std::vector<Action> drain();
    size_t size() const;
    size_t selects() const;                    // Select actions waiting (the spin's cached value wins meanwhile)
private:
    mutable std::mutex mx_;
    std::deque<Action> q_;
};

// The files layer as the page sees it (the DLL: model::files::Engine under mech::lock(), MAIN's _rsm_api).
class Loadouts {
public:
    virtual ~Loadouts() = default;
    virtual std::string selected_name() = 0;
    virtual std::string char_name() = 0;                                  // "" when no character
    virtual std::string loaded_name() = 0;
    virtual std::vector<std::string> dropdown() = 0;
    virtual int dropdown_index() = 0;                                     // 1-based
    virtual void set_dropdown_index(int idx1) = 0;
    virtual void set_native_action(bool on) = 0;
    virtual std::string import_selected() = 0;                            // + the replay layer's follow-up of an import
    virtual std::string reopen_loaded() = 0;                              // idem
    virtual std::string export_selected(std::string* main_ticker) = 0;   // main_ticker: MAIN's own "<x> File Saved"
    virtual std::string export_as(const std::string& name, std::string* main_ticker) = 0;
    virtual std::string erase_selected() = 0;
    virtual std::string rename_selected(const std::string& name, std::string* main_ticker) = 0;
    virtual void invalidate() = 0;
    virtual bool ensure() = 0;                                            // ensure_files: the dummy read from SelectMenu
};

// What the actions show (the DLL: P4's ui::dialogs and two game calls).
class Screen {
public:
    virtual ~Screen() = default;
    virtual void ticker(const std::string& text) = 0;
    // the Yes / No box with its title (RMT:116, 127, 141, 1615: "Save loadout", "Reset loadout", "Erase loadout", "Save loadout as")
    virtual void confirm(const std::string& text, const std::string& title, std::function<void(bool yes)> done) = 0;
    virtual void text_input(const std::string& title, const std::string& initial, size_t max_bytes,
                            std::function<void(bool ok, const std::string& text)> done) = 0;
    virtual void community_open() = 0;
    virtual void community_share() = 0;
    virtual void check_slots() = 0;            // TrainingManager.CheckMenuSlotData: the rows' frames texts follow (RMT:107)
    virtual void rows_dirty() = 0;             // the Record / Replay slot rows redrawn (RMT rows_dirty)
    virtual void list_changed() = 0;           // the spin's labels and value synced again (RMT manager_dirty)
};

// The page's actions, as RMT runs them (same checks, same tickers, same order). Game thread.
class Runner {
public:
    Runner(Loadouts& files, Screen& screen, Tr T) : files_(files), screen_(screen), T_(std::move(T)) {}
    void run(const Action& a);
    bool dialog_open() const { return dialogs_ > 0; }   // one of the page's dialogs is up (RMT naming / confirm_box)
    void menu_closed() { dialogs_ = 0; }                // the pause menu closed: no dialog of ours is left
    uint64_t runs() const { return runs_; }
private:
    void open();
    void save();
    void save_as();
    void reset();
    void erase();
    void rename();
    // RMT confirm(message, title, fn): ignored while one of ours is up
    void ask(const std::string& text, const std::string& title, std::function<void()> yes);
    void name_dialog(const std::string& title, const std::string& initial, std::function<void(const std::string&)> apply);
    void main_ticker(const std::string& t) { if (!t.empty()) screen_.ticker(t); }
    Loadouts& files_;
    Screen& screen_;
    Tr T_;
    int dialogs_ = 0;
    uint64_t runs_ = 0;
};

}  // namespace ui::mgr
