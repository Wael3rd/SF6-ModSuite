// pausemenu::core - the training pause-menu engine's state machine, without any game dependency (increment 8,
// ADR-0016, docs/PAUSE_MENU.md).
//
// The engine answers the game's training pause menu (app.training.UIFlowTrainingMenu) for the pages and rows of the
// core's clients. This file holds what does not need the game: the registry (pages, rows, their texts and data), the
// identity of the menu data (GUID key, R5), the routing of the game's questions (ours / the game's / nobody's), the
// page placement in a sub-page strip, the spin rules and the timings. Every game operation goes through Backend, so
// the same file is compiled into the DLL (real backend, pausemenu.cpp) and into an offline bench (scripted backend).
//
// It is a port of a Lua engine that ran these pages in game (the "reference engine" in the comments): the comments
// name the behaviour kept and the reasoning where this one differs.
//
// Threads: every function may be called from any thread (an internal recursive mutex). The hooks call it from the
// game's UI job threads, concurrently (R29); the install pass runs on the game thread with the menu closed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pausemenu::core {

using Obj = void*;           // a game object (menu data, array, strip row); never dereferenced here
using Owner = const void*;   // the client that owns a page / row (its handle; never dereferenced here)

// A message GUID (System.Guid, 16 bytes): the identity of a TrainingMenuData (its _MessageID).
struct Key {
    uint64_t hi = 0, lo = 0;
    bool zero() const { return hi == 0 && lo == 0; }
    bool operator==(const Key& o) const { return hi == o.hi && lo == o.lo; }
};

// ---- constants (the reference engine's values) ------------------------------------------------------------------
constexpr int32_t kDynamic = 345;          // TrainingFuncType.DYNAMIC: the game's own dynamic rows use it too (R5)
constexpr int32_t kItemTextOnly = 0;       // TrainingMenuData._Type of a button row (ItemType.TEXT_ONLY)
constexpr int32_t kItemSpin = 1;           // ItemType.SPIN
constexpr int32_t kStripTwo = 18;          // ItemType.RECORD_TAB: the 2-page strip prefab (p_TrainingSecondaryTab_18)
constexpr int32_t kStripThree = 19;        // ItemType.REVERSAL_TAB: the 3-page strip prefab (p_TrainingSecondaryTab_19)
constexpr uint32_t kMaxCapacity = 64;      // the largest spin run in this menu (R9 says 16 elsewhere; ADR-0016 risks)
constexpr uint32_t kMaxRows = 13;          // rows per page (R9: the game's tabs never exceed 13, pool of 20)
constexpr int kInstallEvery = 60;          // the install pass: every 60 frames ...
constexpr int kInstallPhase = 33;          // ... at this phase (the reference engine's tick % 60 == 33)
constexpr int kInstallDelay = 300;         // ... once training has run 300 frames, menu closed (rule 10)
constexpr int kForceFrames = 2;            // UIPartsSpin.set_Num rewritten for 2 frames after Init (the flow's own set_Num)
constexpr int kFitCooldown = 15;           // a font write at most every 15 calls (the game's focus state writes it too)
constexpr int kRebuildDelay = 2;           // a page rebuilt 2 frames after a change of its spin's count (R4)
constexpr float kArrowLeftX = 10.0f;       // the game's arrow positions in a spin part (p_MouseDecideOperable_L / _R)
constexpr float kArrowRightX = 570.0f;
constexpr float kTextMargin = 110.0f;      // room lost between the arrows (measured by the reference engine)
constexpr float kMinFontScale = 0.5f;      // a long option text never shrinks below half the font
constexpr float kEpsilon = 0.5f;           // positions and font sizes: a write only past this difference
constexpr uint32_t kMaxHints = 256;        // cached hint objects (one per client, button, text)

enum class Kind { Button = 0, Spin = 1 };

// ---- the game operations ------------------------------------------------------------------------------------------
// The sub-page strip of a primary tab: its row (TrainingMenuData of _Type 18 / 19) and its pages, in order.
struct Strip {
    Obj row = nullptr;
    int32_t type = 0;
    Obj array = nullptr;              // the row's _ChildData
    std::vector<Obj> pages;
    std::vector<Key> keys;            // the pages' _MessageID
};

struct Backend {
    // texts (GUIDs answered by the core's GetMessage hook, owned by the engine)
    bool (*text_new)(const std::string& text, Key& out) = nullptr;
    bool (*text_set)(const Key& key, const std::string& text) = nullptr;
    void (*text_release)(const Key& key) = nullptr;
    // menu data (game thread, menu closed; option arrays also at a page build, on the menu's thread)
    Obj  (*new_row_data)(int32_t item_type, const Key& title, const Key& guide) = nullptr;   // held (add_ref)
    Obj  (*new_option_data)(const Key& text) = nullptr;                                     // held
    Obj  (*new_array)(const std::vector<Obj>& items) = nullptr;                            // TrainingMenuData[], held
    bool (*set_children)(Obj data, Obj array) = nullptr;                                    // data._ChildData = array (R34)
    Obj  (*new_page_data)(Obj template_page, const Key& title, const Key& guide, Obj rows_array) = nullptr;   // held
    Obj  (*new_hint)(int32_t button, const Key& text) = nullptr;                            // app.InputGuideData, held
    void (*release)(Obj o) = nullptr;
    // the strip of a primary tab (game thread, menu closed)
    bool (*strip_read)(int32_t primary_func, Strip& out) = nullptr;
    bool (*strip_write)(const Strip& strip, const std::vector<Obj>& pages, int32_t type) = nullptr;
    void (*log)(const char* line) = nullptr;
    // diagnostics (optional): the install pass starts / ends (game thread), a trace line while tracing() says so
    void (*pass_begin)() = nullptr;
    void (*pass_end)() = nullptr;
    bool (*tracing)() = nullptr;
    void (*trace)(const char* line) = nullptr;
};
void set_backend(const Backend& b);

// ---- registry (any thread) ----------------------------------------------------------------------------------------
int64_t add_page(Owner owner, int32_t primary_func, int32_t strip_index, const std::string& title, const std::string& guide);
int64_t add_row(Owner owner, int64_t page, Kind kind, const std::string& title, const std::string& guide,
                uint32_t capacity, bool keep_open, float arrow_spread);
std::string last_error();             // why the last add_page / add_row of this thread failed

enum Result { kOk = 0, kNotFound = 1, kInvalid = 2, kTruncated = 3 };
Result set_texts(Owner owner, int64_t id, const std::string* title, const std::string* guide);   // null = unchanged
Result set_options(Owner owner, int64_t row, const std::vector<std::string>& labels, uint64_t frame);

// A shown-state change for the owner of a page (on_page_shown).
struct Edge { int64_t page = -1; Owner owner = nullptr; bool shown = false; };
Result remove(Owner owner, int64_t id, std::vector<Edge>* edges);
size_t remove_owner(Owner owner, std::vector<Edge>* edges);   // pages + rows + hints of a client that unregistered

// ---- identity (any thread) ----------------------------------------------------------------------------------------
enum class What { None, Page, Row, Option };
struct Ident {
    What what = What::None;
    int64_t id = -1;                  // the page or row id (an option: its row)
    int64_t page = -1;                // a row's / option's page
    int32_t option = -1;              // an option's index
    bool removed = false;             // ours, but removed: the next pass takes it out of the menu
    Owner owner = nullptr;
    Kind kind = Kind::Button;
    bool keep_open = false;
};
Ident identify(const Key& key);
// The row id of `owner` carried by that menu data key: a row, or the option of a spin (its row). -1 for a page, a
// game row, an unknown key, a removed row, or a row of another owner (pm_row_of).
int64_t row_of(Owner owner, const Key& key);
// The option-build purge: may this DYNAMIC row of a primary tab be removed? Only a row the core built and no longer
// owns. The game's rows (a delegate) and unknown rows (another engine's) stay (ADR-0016 point 2, fixes M1).
bool removable(const Key& key, bool has_delegate);

// ---- routing of the game's questions ------------------------------------------------------------------------------
// The four FuncType-only methods and the option texts carry no row: the row being laid out on this thread (set by
// ViewUpdate / UpdateSpinBox / Function, kept per thread by the hooks, R29) says whose it is.
enum class Ctx { None, Ours, Foreign };
enum class Route { Pass, Ours, Default };
// Pass: the game's own answer. Ours: the row's answer. Default: the reference engine's answer for a DYNAMIC row whose
// owner is unknown, given only while no game DYNAMIC row exists in the menu (offline training): the reference engine
// answered every DYNAMIC call; with a game row present (Battle Hub) the game's answer is the only right one for it.
Route route(int32_t ftype, Ctx ctx, bool game_rows);
enum Method { kGetIsActive, kIsValueType, kGetVisibleCase, kIsChangedValue, kGetOptionIndex };
int64_t const_answer(Method m, Route r, int32_t wanted);   // GetIsActive 1, IsValueType / GetVisibleCase / IsChangedValue 0;
                                                            // GetOptionIndex: wanted (ours, a display probe), 0 (default)
void set_game_rows(bool present);     // a game DYNAMIC row (with its delegate) is in the menu data
bool game_rows();

// TrainingMenuFunc.Function(ftype, param, viewData, value)
enum class FnAct { Pass, Page, Retired, Button, Spin };
struct FnInput {
    int32_t ftype = 0;
    Ident data;                       // viewData.Data
    Ident parent;                     // viewData.ParentData (secondary pages: Data = the option shown, ParentData = the row)
    bool data_null = false;
    bool game_owned = false;          // the data is in a primary tab's DynamicChildData with the game's delegate
    Ctx ctx = Ctx::None;
    int64_t ctx_row = -1;
    uint64_t frame = 0;
    int32_t row_index = -1;           // `value`: the row's index in the menu's list
};
struct FnPlan {
    FnAct act = FnAct::Pass;
    int64_t id = -1;                  // page or row
    Owner owner = nullptr;
    bool at_build = false;            // the row's Function at its page build (no decide, no change)
    bool keep_open = false;
};
FnPlan plan_function(const FnInput& in);

// ---- pages: build, shown state ------------------------------------------------------------------------------------
// The page's Function ran (the strip selected it): rows marked as built this frame, the option arrays sized to the
// labels (backend.new_array / set_children). Returns false when the page is not ours / not built.
void building(int64_t page);                         // the page's build starts: label changes now need no rebuild
bool page_built(int64_t page, uint64_t frame);
bool at_build(int64_t row, uint64_t frame);
std::vector<Edge> set_shown(int64_t page);           // this page shown: the other pages of its strip not shown
std::vector<Edge> game_page_built(Obj page_data);    // a game page of a strip we use was built: ours there not shown
std::vector<Edge> menu_closed();                      // every page not shown
bool any_shown();
int64_t shown_page();
Obj page_data(int64_t page);
std::vector<int64_t> page_rows(int64_t page);          // the live rows of a page, in order
bool is_game_page(Obj page_data);                      // one of the game's pages of a strip we installed into

// ---- rows: spins --------------------------------------------------------------------------------------------------
struct RowInfo {
    int64_t id = -1, page = -1;
    Owner owner = nullptr;
    Kind kind = Kind::Button;
    bool keep_open = false, removed = false;
    float spread = 0.0f;
    uint32_t capacity = 0;
    int32_t labels_n = 1;             // options shown: max(1, min(labels, capacity))
    int32_t opt_n = 0;                // options in the array the game reads (sized to labels_n at a build / size_options)
    int32_t row_index = -1;
    Obj data = nullptr;
    Key key;
};
bool row_info(int64_t row, RowInfo& out);
int32_t clamp_value(int64_t row, int32_t v);         // into 0 .. labels_n - 1
int32_t wrap(int64_t row, int32_t num);              // the spin part's _Num after a press: num % labels_n
std::string label(int64_t row, int32_t index);
Obj option_data(int64_t row, int32_t index);         // the option object (viewData.Data at the build)
void store_value(int64_t row, int32_t v);            // the value when the client keeps none (no spin_value)
bool stored_value(int64_t row, int32_t& v);
void set_row_index(int64_t row, int32_t index);      // the row's index in the menu's list (Function / UpdateSpinBox)
int64_t row_at(int32_t index);                        // the row last laid out at that index (-1 = none)
void force(int64_t row, uint64_t frame);             // UIPartsSpin.Init rewritten: hold set_Num for kForceFrames
bool forcing(int64_t row, uint64_t frame);
// The option text fitted to the room between the arrows: base font recorded at the first call (the natural width at
// the base font decides, stable), a write only when it differs and at most every kFitCooldown calls. A text that fits
// keeps the game's font (its focus state writes it too): written only to shrink, or once to undo the engine's shrink.
struct Fit { bool write = false; float w = 0, h = 0; };
Fit fit(int64_t row, float cur_w, float cur_h, float display_w);
Fit fit_pure(float base_w, float base_h, float cur_w, float display_w, float spread, int& cool, bool& shrunk);   // the rule
float arrow_x(bool right, float spread);
bool base_font(int64_t row, float& w, float& h);

// ---- deferred work (R4: never from inside the menu's own update chain) ---------------------------------------------
void request_rebuild(int64_t page, uint64_t at_frame);
bool rebuild_pending();
int64_t take_rebuild(uint64_t frame, bool hold);     // the page due now (cleared), -1 = none; hold = an option window is up.
                                                     // The caller runs page_built(page) (arrays sized, rows at build) then rebuilds
std::vector<int64_t> take_retext(int64_t page);      // spins of the page whose labels changed (visible text to refresh)
// A shown spin whose labels changed: its option array sized to them now, as the reference engine's prepare_page did at
// a list change (the deferred rebuild rebuilds the rows' data; the spin part's range follows the array in the repair).
// True when the array changed. Menu thread, page shown.
bool size_options(int64_t row);

// ---- hints ----------------------------------------------------------------------------------------------------------
Obj hint_obj(Owner owner, int32_t button, const std::string& text);   // created once per (owner, button, text), held

// ---- R18: which hooks may be installed --------------------------------------------------------------------------
// A hook is put on machine code: when another method of the game enters the same code (folded bodies, or an entry
// that jumps into it), a hook on one is a hook on all of them. The game side measures that for each hook; this
// decides. Sharing only with methods of the same hook group (same behaviour, e.g. an overload jumping into the other)
// is fine: the body is hooked once and covers them.
struct HookCode {
    bool found = false;               // the method exists and has code
    uint32_t index = 0;               // its index in the game's type database
    uintptr_t raw = 0, code = 0;      // its entry, and the code that entry runs (jump thunks followed)
    int group = 0;                    // hooks with the same pre / post functions
    int shared = 0;                   // other methods of the game entering the same entry or code (-1: check failed)
    std::vector<uint32_t> sharers;    // their indexes (the first ones found)
};
enum class Verdict { Hook, Covered, Refused, Missing };
std::vector<Verdict> hook_verdicts(const std::vector<HookCode>& hooks);

// ---- the install pass (game thread) -------------------------------------------------------------------------------
// training: in a training flow with the game's training data; paused: a menu or an option window is up.
// Builds what is missing, puts the pages into their strips, takes removed ones out, releases what nothing shows.
void tick(uint64_t frame, bool training, bool paused);
void install_now();                                   // the next tick runs the pass (bench, tests)

// ---- state ------------------------------------------------------------------------------------------------------------
struct Stats {
    size_t pages = 0, pages_installed = 0, rows = 0, removed = 0, hints = 0;
    int64_t shown = -1;
    bool game_rows = false;
    uint64_t passes = 0, writes = 0;
};
Stats stats();
std::string status();
void reset();                                         // forget everything (bench)

}  // namespace pausemenu::core
