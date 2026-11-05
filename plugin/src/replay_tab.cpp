// SF6 Slots — "Replay Outfits": a 6th tab of the Replays menu (agent CFNReplayTop), after Search.
//
// The page is laid out like Battle Settings > Other > Opponent Outfit Settings: a grid of character portraits (4 columns,
// the game's order, "On" over a character whose replays get a chosen outfit, the name below) and, on the left, the
// focused character's settings as the game's page has them: Outfit Settings On / Off, Costume, Color. Confirm (F, pad A)
// on a portrait edits its rows (Up / Down a row, Left / Right its value, Confirm or Back to the grid); R (pad X / Square)
// cycles the colour anywhere. On = the outfit a replay's fighter of that character wears (replay_logic.hpp; the playback
// override reads the file), Off = what the replay holds. Every change writes
// reframework/data/SF6_CostumeSlots_data/replay_outfits.json at once (temporary file + rename).
//
// The game's own page (app.UIFlowOtherMatchingFighterSetting.Start(fighter, save)) cannot run here: its flow looks up the
// GameObject "OtherMatchingFighterSetting" by name (UIParam.get_ObjectName, empty folder path), which only Battle
// Settings' scene holds; started from the Replays menu its flow ends at once (tried 05/11). So the look is rebuilt from
// the Replays screen's own pieces.
//
// How (a virtual tab: the game never hears of a 6th tab, its own tab list stays on Search):
//   - The tab bar (CFNReplayTop > p_SimpleList_0_tab) has six items, c_item_00 - c_item_05, the game shows five
//     (app.UICFNReplayTop.TabType NUM 5). c_item_05 is shown with our label; its mouse hit area is hidden.
//   - E / R1 on Search: app.UIAgent.InputTab / InputTabNext of the CFNReplayTop agent (its _TabItem, an
//     app.UIPartsSimpleList, says SelectedIndex 4) are swallowed (pre-hook, SKIP) and the tab becomes ours. Next on
//     ours: swallowed (nothing after us). The core's code check found these three methods unique (pm_codecheck.txt, R18).
//   - While ours: the Search page's agents (every visible CFN* agent but CFNTop and CFNReplayTop: CFNReplayChildSearch,
//     CFNSearchItemList) get ForceInvisible on their main control and BlockInput; both are put back when we leave, when
//     the game changes tab by itself (a mouse click) and when the screen goes.
//   - The page is made of copies of three plain controls of the tab bar's screen (the core's ui_clone_control, inside
//     the tab hook: R19, the screen's own input update): the header's title text, the bar's solid mask rectangle
//     (e_rect_mask_00, turned into a plain coloured rectangle: MaskType NonTarget; cells, rows, the focus borders) and the
//     "new" badge's texture (the portraits: the game's own characterselect face pictures, loaded by path through the
//     core, shown once loaded). Named e_ro_*, under c_title
//     (root space); they live and die with the screen's tree (R35: found again by name, every write through the core).
//     Text, place, size, colour, texture and visibility from LateUpdateBehavior (R1), written when the page changed and
//     every 20 frames.
//   - Input: read natively while the game window has the focus (keys by scancode, so any keyboard layout: W A S D
//     places and the arrows, F / Enter; via.hid.GamePad's merged device: d-pad, left stick, A, X / Square), never hooked.
//     The screen's whole input goes through the page's agent (its BlockInput also stops the tab bar's A / E and Esc:
//     seen 05/11), so Back is ours too: the tab-back key (Q place, the game's default), Esc, pad L1 or B go back to the
//     grid from the rows, to Search from the grid; the page gets its input back only once that key is released.
//   - The tab items' look is their via.gui PlayState: c_item_05 is DISABLE as the game leaves it, DEFAULT shown,
//     SELECT while ours (c_item_04 DEFAULT meanwhile, SELECT again when we leave).
//   - The focused character in 3D, as the game's own page shows it: Battle Settings' fighter capture room, borrowed
//     while our page is up (namespace preview below); its render target on a texture copy, the 2D portrait meanwhile.
// Gate: nothing but one core registry read every 15 frames while the Replays screen is not up.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "replay_logic.hpp"
#include "replay_tab.hpp"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>

namespace slots::replaytab {
namespace {

using namespace slots::replay;

constexpr const char* kTop = "CFNReplayTop";
constexpr int kSearch = 4;                        // app.UICFNReplayTop.TabType.SEARCH
constexpr uint32_t kPollEvery = 15;
constexpr int kMaxRefused = 3;
constexpr uint32_t kActFlags = 2 | 8;             // app.InputDigitalFlag Trigger | Repeat
constexpr uint32_t kDirTabBack = 16, kDirTabNext = 32;   // app.UIPartsItemDirection
constexpr const char* kTabLabel = "Replay Outfits";
// layout, root space (1920 x 1080; c_title sits at the root's origin), Battle Settings > Other's own places
constexpr int kCols = 4, kVisRows = 8, kCells = kCols * kVisRows, kPanelRows = 3;
constexpr float kGridX = 760, kGridY = 184, kCellW = 160, kCellH = 72, kStepX = 168, kStepY = 80;
constexpr float kRowX = 60, kRowY = 250, kRowW = 640, kRowH = 58, kRowStep = 70;
constexpr float kBigX = 200, kBigY = 500, kBigW = 400, kBigH = 300;
// the 3D preview: the middle of the render target (16:9, the fighter stands in its middle), drawn below the rows
// (read on Battle Settings' page, which draws the whole target at 2304 x 1296 centred on 1437, 605: Luke spans u 0.32 -
// 0.54, v 0.21 - 0.82; a tall fighter's head reaches v 0.05)
constexpr float kModelX = 130, kModelY = 452, kModelW = 520, kModelH = 500;
constexpr double kModelU = 0.18, kModelV = 0.05, kModelUW = 0.50, kModelVH = 0.85;
constexpr float kFontCell = 24, kFontRow = 30, kFontName = 44, kFontHelp = 24;
// via.Color raw (R in the low byte)
constexpr uint32_t kColText = 0xFFFFFFFF, kColDim = 0xFF8C8C8C, kColFocus = 0xFF40D0FF, kColOn = 0xFFFFB040;
constexpr uint32_t kColCell = 0xE6342C2A, kColCellFocus = 0xF5EEEEEE, kColBorder = 0xFFFFA040, kColRow = 0xCC342C2A;
constexpr uint32_t kColRowFocus = 0xF0B06020, kColDark = 0xFF2A2A2A;   // the focused cell: light, dark caption (the game's)
// Draw order: the GUI sorts its elements by Priority (child order only breaks ties): borders, frames, pictures, texts
constexpr int kPrioBorder = 1, kPrioFrame = 2, kPrioImage = 3, kPrioText = 4;
constexpr float kFaceV0 = 0.125f, kFaceVH = 0.75f;   // the portrait's middle band (cells and the big picture are 4:3)

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

// ---- the hooks' side (any thread) ----
std::atomic<uintptr_t> g_top_addr{ 0 };          // the CFNReplayTop agent while its screen is up (frame)
std::atomic<uintptr_t> g_main_addr{ 0 };         // its main control (frame)
std::atomic<int> g_tab{ -1 };                     // the game's tab as the frame last read it
std::atomic<bool> g_ours{ false };
std::atomic<bool> g_enter{ false }, g_leave{ false };
bool g_leaving = false;                           // Back pressed on the grid: the page waits for the key's release
std::atomic<int> g_refused{ 0 };
std::atomic<int> g_copy_status{ 1000 };
std::atomic<uint32_t> g_swallowed{ 0 };
std::atomic<uint32_t> g_leave_frame{ 0 };
thread_local bool tl_skipped = false;

// ---- frame state ----
bool g_init = false, g_ok = false;
bool g_up = false;
uintptr_t g_main_seen = 0;                        // the screen build the label / guide were set up for
bool g_guide_moved = false;
std::vector<std::string> g_hidden;                // agents hidden for our page (names, found again to restore)
std::vector<Fighter> g_cat;
Choices g_choices;
std::map<int, Choice> g_last_on;                  // per character, the outfit it had before being turned Off
int g_cursor = 0, g_top_row = 0;                  // the focused cell (catalog index), the first grid row shown
bool g_edit = false;                              // the focused character's rows have the input
int g_row = 0;                                    // the focused row while editing
bool g_copies_shown = false, g_dirty = true;
uint32_t g_drawn_at = 0;
uintptr_t g_model_tex = 0;                        // the model copy given the render target (address)
bool g_model_shown = false;
std::set<uintptr_t> g_styled;
int g_status_logged = 1000;
bool g_place_logged = false;
uint32_t g_enter_frame = 0;
std::string g_back_hint = "L1";                   // the bar's tab-back guide text (the game's icon of the key)
int64_t g_cp_lefttop = -1;                        // via.gui.ControlPoint.LeftTop
std::map<int, void*> g_faces;                     // fighter -> portrait TextureResourceHolder (one reference held)

// ---- the page's copies, made inside the tab hook (R19) ----
// Three kinds, each a copy of a plain control of the tab bar's screen (no UIParts)
enum Kind { kText, kRect, kImage };
struct Copy { std::string name; Kind kind; };

std::string cname(const char* what, int i) {
    char b[48];
    std::snprintf(b, sizeof b, "e_ro_%s_%02d", what, i);
    return b;
}
// Draw order = creation order (each copy becomes its parent's last child): frames, focus frames, pictures, texts
const std::vector<Copy>& copies() {
    static std::vector<Copy> v;
    if (!v.empty()) return v;
    v.push_back({ "e_ro_rfocus", kRect });         // the borders first: under the frame they surround
    for (int r = 0; r < kPanelRows; ++r) v.push_back({ cname("rbg", r), kRect });
    v.push_back({ "e_ro_cfocus", kRect });
    for (int i = 0; i < kCells; ++i) v.push_back({ cname("cbg", i), kRect });
    for (int i = 0; i < kCells; ++i) v.push_back({ cname("cimg", i), kImage });
    v.push_back({ "e_ro_bigimg", kImage });
    v.push_back({ "e_ro_model", kImage });         // the 3D preview: the capture room's render target
    for (int i = 0; i < kCells; ++i) v.push_back({ cname("cname", i), kText });
    for (int i = 0; i < kCells; ++i) v.push_back({ cname("con", i), kText });
    for (int r = 0; r < kPanelRows; ++r) { v.push_back({ cname("rlbl", r), kText }); v.push_back({ cname("rval", r), kText }); }
    v.push_back({ "e_ro_pname", kText });
    v.push_back({ "e_ro_help", kText });
    return v;
}

bool make_copies() {
    const SF6CoreAPI* a = A();
    if (!a || g_refused.load() >= kMaxRefused) return false;
    void* main = reinterpret_cast<void*>(g_main_addr.load(std::memory_order_acquire));
    void* title = main ? a->ui_find_child(main, "c_title", 8) : nullptr;
    void* t_text = title ? a->ui_find_child(title, "e_text_title", 0) : nullptr;
    void* bar = main ? a->ui_find_child(main, "p_SimpleList_0_tab", 8) : nullptr;
    void* bar_bg = bar ? a->ui_find_child(bar, "c_bg", 0) : nullptr;
    void* t_rect = bar_bg ? a->ui_find_child(bar_bg, "e_rect_mask_00", 0) : nullptr;
    void* item0 = bar ? a->ui_find_child(bar, "c_item_00", 0) : nullptr;
    void* batch = item0 ? a->ui_find_child(item0, "c_batch", 0) : nullptr;
    void* t_image = batch ? a->ui_find_child(batch, "e_texture_new", 0) : nullptr;
    if (!title || !t_text || !t_rect || !t_image) { ++g_refused; g_copy_status.store(-3000); return false; }
    for (const Copy& cp : copies()) {
        if (a->ui_find_child(title, cp.name.c_str(), 0)) continue;
        void* tmpl = cp.kind == kText ? t_text : cp.kind == kRect ? t_rect : t_image;
        void* c = nullptr;
        const SF6CoreStatus st = a->ui_clone_control(C(), tmpl, title, cp.name.c_str(), &c);
        g_copy_status.store(st == SF6CORE_OK && !c ? -1000 : static_cast<int>(st));
        if (st != SF6CORE_OK || !c) { ++g_refused; return false; }
        SF6CoreArg m{}; m.type = SF6CORE_ARG_I64; m.v.i = 1;    // via.gui.MaskType.NonTarget (the rectangle was a mask: 2)
        a->obj_invoke(C(), c, "set_MaskType(via.gui.MaskType)", &m, 1, nullptr);
        a->ui_set_visible(C(), c, 0);                           // placed and shown by the frame
    }
    return true;
}

constexpr int kCall = REFRAMEWORK_HOOK_CALL_ORIGINAL, kSkip = REFRAMEWORK_HOOK_SKIP_ORIGINAL;

int decide(void* self, int dir, uint32_t flag) {
    const uintptr_t top = g_top_addr.load(std::memory_order_acquire);
    if (!top || reinterpret_cast<uintptr_t>(self) != top || g_closing.load(std::memory_order_relaxed)) return kCall;
    const bool acts = (flag & kActFlags) != 0;
    if (g_ours.load(std::memory_order_acquire)) {
        if (acts && dir < 0) { g_ours.store(false); g_leave.store(true); g_leave_frame.store(g_frame); }
        g_swallowed.fetch_add(1);
        return kSkip;                                   // our tab: the game's list stays on Search
    }
    if (dir < 0 && g_leave_frame.load() == g_frame) return kSkip;   // the same press seen again (InputTab + InputTabBack)
    if (dir > 0 && acts && g_tab.load(std::memory_order_acquire) == kSearch) {
        bool ok = false;
        try { ok = make_copies(); } catch (...) { ++g_refused; g_copy_status.store(-2000); }
        if (!ok) return kCall;                          // no page to show: the game does its own thing
        g_ours.store(true);
        g_enter.store(true);
        g_swallowed.fetch_add(1);
        return kSkip;
    }
    return kCall;
}
uint32_t u32(void* a) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a) & 0xFFFFFFFF); }

int pre_input_tab(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {   // InputTab(dir, flag)
    tl_skipped = false;
    const uint32_t d = u32(argv[2]);
    const int dir = (d & kDirTabNext) ? 1 : (d & kDirTabBack) ? -1 : 0;
    if (!dir) return kCall;
    const int rc = decide(argv[1], dir, u32(argv[3]));
    tl_skipped = rc == kSkip;
    return rc;
}
void post_input_tab(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (tl_skipped && ret_val) *ret_val = nullptr;      // System.Boolean false: no tab change happened
    tl_skipped = false;
}
int pre_tab_next(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) { return decide(argv[1], 1, u32(argv[2])); }
int pre_tab_back(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) { return decide(argv[1], -1, u32(argv[2])); }
void post_none(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// ---- the catalog: every character's outfits and their colours ----
void build_catalog() {
    std::map<int, Fighter> by;
    std::set<std::pair<int, int>> slot_keys;
    // the Slots Loader's outfits: registry.json (possession: fighter, costume_no, name, nsfw; slots: mod_id), labels
    // from outfits.json
    Json reg, outs;
    std::map<std::pair<int, int>, std::string> mod_of, label_of;
    if (json_parse(read_file("reframework/data/SF6_Costumes_Data/outfits.json"), outs))
        if (const Json* os = outs.get("outfits"); os && os->kind == Json::Array)
            for (auto& e : os->arr) {
                const Json* f = e.get("fighter"); const Json* c = e.get("costume_no"); const Json* l = e.get("label");
                if (f && c && l) label_of[{ static_cast<int>(f->as_int()), static_cast<int>(c->as_int()) }] = l->as_str();
            }
    int slots = 0, nsfw_left = 0;
    if (json_parse(read_file("reframework/data/SF6_Costumes_Data/registry.json"), reg)) {
        if (const Json* ss = reg.get("slots"); ss && ss->kind == Json::Array)
            for (auto& e : ss->arr) {
                const Json* f = e.get("fighter"); const Json* n = e.get("new_costume_no"); const Json* m = e.get("mod_id");
                if (f && n && m) mod_of[{ static_cast<int>(f->as_int()), static_cast<int>(n->as_int()) }] = m->as_str();
            }
        const bool nsfw_ok = costumes::display_nsfw();
        if (const Json* ps = reg.get("possession"); ps && ps->kind == Json::Array)
            for (auto& e : ps->arr) {
                const Json* f = e.get("fighter"); const Json* c = e.get("costume_no");
                if (!f || !c) continue;
                const int fid = static_cast<int>(f->as_int()), cos = static_cast<int>(c->as_int());
                if (fid <= 0 || cos <= 0) continue;
                slot_keys.insert({ fid, cos });
                if (const Json* ns = e.get("nsfw"); ns && ns->kind == Json::Bool && ns->b && !nsfw_ok) { ++nsfw_left; continue; }
                Outfit o;
                o.costume = cos;
                auto m = mod_of.find({ fid, cos });
                o.extra = true;
                if (m != mod_of.end()) o.mod_id = m->second;            // none known: the file names the slot by its number
                auto l = label_of.find({ fid, cos });
                const Json* nm = e.get("name");
                o.name = l != label_of.end() && !l->second.empty() ? l->second : nm ? nm->as_str() : "Outfit " + std::to_string(cos);
                for (int k = 0; k < 10; ++k) o.colors.push_back(k);   // costumes.cpp gives every slot colours 0-9
                by[fid].outfits.push_back(o);
                ++slots;
            }
    }
    // the game's outfits: CostumeInventory.GetMasterDataRecord, records 1.. (the loader's start at 5000), the colours of
    // each from TableDataManager.GetFighterCostumeColors(record, false)
    int game = 0;
    auto* im = api().get_managed_singleton("app.InventoryManager");
    Obj* ci = im ? get_obj(im, "_Costume") : nullptr;
    Method* m_rec = ci ? method_of(ci->get_type_definition(), "GetMasterDataRecord", 1) : nullptr;
    auto* tm = api().get_managed_singleton("app.TableDataManager");
    Method* m_cols = tm ? method_of(tm->get_type_definition(), "GetFighterCostumeColors", 2) : nullptr;
    std::set<std::pair<int, int>> seen;
    if (m_rec && m_cols)
        for (int rid = 1, last = 0; rid < 5000 && rid - last <= 100; ++rid) {
            Obj* rec = call_obj(m_rec, ci, { arg_i32(rid) });
            int32_t fid = 0, cn = -1;
            if (!rec || !get_i32(rec, "fighterId", fid) || !get_i32(rec, "costumeNo", cn)) continue;
            last = rid;
            if (fid <= 0 || cn < 0 || slot_keys.count({ fid, cn }) || !seen.insert({ fid, cn }).second) continue;
            Outfit o;
            o.costume = cn;
            o.name = game_outfit_name(cn);
            if (Obj* cls = call_obj(m_cols, tm, { arg_i32(rid), arg_bool(false) })) {
                std::set<int> cs;
                for (int j = 0, n = list_count(cls); j < n; ++j) {
                    int32_t c = 0;
                    if (Obj* cr = list_item(cls, j); cr && get_i32(cr, "colorNo", c) && c >= 0) cs.insert(c);
                }
                o.colors.assign(cs.begin(), cs.end());
            }
            by[fid].outfits.push_back(o);
            ++game;
        }
    g_cat.clear();
    for (auto& [fid, f] : by) {
        f.fid = fid;
        const char* n = fighter_name(fid);
        f.name = n ? n : "Fighter " + std::to_string(fid);
        sort_outfits(f.outfits);
        if (!f.outfits.empty()) g_cat.push_back(f);
    }
    order_fighters(g_cat);
    logf("replay tab: %zu characters, %d game outfits, %d extra outfits%s", g_cat.size(), game, slots,
         nsfw_left ? (" (" + std::to_string(nsfw_left) + " NSFW hidden)").c_str() : "");
}

void load_choices() {
    Choices c;
    const std::string text = read_file(kFile);
    if (!text.empty() && !from_json(text, c)) logf("replay tab: %s unreadable, starting from Default", kFile);
    g_choices = c;
}
void save_choices() {
    if (!write_file(kFile, to_json(g_choices))) logf("replay tab: could not write %s", kFile);
}

// The portraits: loaded once per session (the core holds them loading; drawn once ready)
void load_faces() {
    for (const Fighter& f : g_cat) {
        if (g_faces.count(f.fid)) continue;
        void* h = nullptr;
        const std::string path = face_path(f.fid);
        if (A()->ui_texture_load(C(), path.c_str(), &h) == SF6CORE_OK && h) g_faces[f.fid] = h;
        else g_faces[f.fid] = nullptr;
    }
}

// ---- the 3D preview: Battle Settings' fighter capture room, borrowed while our page is up ----
// The CFN scene's app.worldtour.CaptureRoomObserver has the capture room scenes of every kind registered, Battle Settings'
// fighter room included (type 5, product/content/gui/CaptureRoom/CaptureRoom_BattleSettingFighter). The way the game's
// UIPartsBattleSettingFighterCapture shows a fighter (its SetupCaptureRoom / ResetCapture, read 05/11):
// DeactivateCaptureRoom(5, "") then RequestLoadCaptureRoom(5, [RequestFighterData(fighter, costume, colour, near)], null,
// ""); once the room's controller is set up, controller.Activate("", false, false). The room's camera draws into
// Product/GUI/menu/ui91000/ui91060/RTT_MatchingFighterSetting.rtex (Battle Settings' preview texture): a holder of that
// render target shown on one of our texture copies shows the model. Never CaptureRoomParamBase.ChangeMainCamera: called
// from the Replays menu it left the main menu's rendering broken (05/11).
// A change of fighter / outfit / colour: the room given back and asked again (as the game does), after the choice held
// still for kPreviewSettle frames. Shown only once the room's fighter is the one asked, ready (CurrentState 6) for
// kPreviewShowAfter frames; the 2D portrait meanwhile and whenever anything fails (kPreviewTries requests without a
// model: the portrait for the rest of the session). Given back when the page goes (tab back, another tab, the screen
// closed): no room nor fighter left behind.
namespace preview {
constexpr int kRoomType = 5;                      // app.worldtour.CaptureRoomObserver.CaptureRoomType.BattleSettingFighter
constexpr int kStateReady = 6;                    // app.worldtour.CaptureRoomFighter.State when drawn in Battle Settings
constexpr uint32_t kSettle = 15, kShowAfter = 8, kTimeout = 600, kPollEvery = 5;
// The room given back lives on a frame or two: its fighter (same character, maybe the same colour) is not the one asked.
// A fighter counts only kFresh frames after the request, and wearing the costume asked (its VisualId, the costume number
// for every outfit seen on 05/11) or, failing that, kVisualWait frames after it.
constexpr uint32_t kFresh = 20, kVisualWait = 150;
constexpr int kTries = 3;
const char* const kRtt = "Product/GUI/menu/ui91000/ui91060/RTT_MatchingFighterSetting.rtex";

struct Want { int fid = 0, cos = 0, col = 0; bool operator==(const Want& o) const { return fid == o.fid && cos == o.cos && col == o.col; } };
bool g_off = false;                               // given up for the session (types missing, or kTries failures)
bool g_ready_types = false;
Method *m_request = nullptr, *m_deactivate = nullptr, *m_activate = nullptr, *m_list_add = nullptr, *m_list_ctor = nullptr;
TD *t_list = nullptr, *t_req = nullptr, *t_ctrl = nullptr, *t_obs = nullptr;
Obj* g_rtt = nullptr;                             // RenderTargetTextureResourceHolder, held for the session
Want g_want, g_asked;                             // what the page shows; what the room was asked for
uint32_t g_want_since = 0, g_asked_at = 0, g_ready_since = 0, g_poll_at = 0;
bool g_room = false;                              // a room of ours is asked or up
bool g_activated = false, g_ready = false;
Obj* g_ctrl = nullptr;                            // the room's controller once found (checked alive before each use)
Obj *g_list = nullptr, *g_req = nullptr;          // our request objects, held until the next request
int g_fails = 0;
uint32_t g_requests = 0;

bool types() {
    if (g_ready_types) return true;
    auto* tdb = api().tdb();
    t_obs = tdb->find_type("app.worldtour.CaptureRoomObserver");
    t_ctrl = tdb->find_type("app.worldtour.CaptureRoomController");
    t_req = tdb->find_type("app.worldtour.CaptureRoomObserver.RequestFighterData");
    t_list = tdb->find_type("System.Collections.Generic.List`1<app.worldtour.CaptureRoomObserver.RequestObjDataBase>");
    m_request = t_obs ? method_of(t_obs, "RequestLoadCaptureRoom", 4) : nullptr;
    m_deactivate = t_obs ? method_of(t_obs, "DeactivateCaptureRoom", 2) : nullptr;
    m_activate = t_ctrl ? t_ctrl->find_method("Activate(System.String, System.Boolean, System.Boolean)") : nullptr;
    if (!m_activate && t_ctrl) m_activate = method_of(t_ctrl, "Activate", 3);
    m_list_add = t_list ? method_of(t_list, "Add", 1) : nullptr;
    m_list_ctor = t_list ? t_list->find_method(".ctor(System.Int32)") : nullptr;
    g_ready_types = t_obs && t_ctrl && t_req && t_list && m_request && m_deactivate && m_activate && m_list_add && m_list_ctor;
    if (!g_ready_types) {
        g_off = true;
        logf("replay tab: 3D preview off (capture room types or methods missing: observer %d controller %d request %d list %d, "
             "methods %d %d %d %d %d)", t_obs != nullptr, t_ctrl != nullptr, t_req != nullptr, t_list != nullptr, m_request != nullptr,
             m_deactivate != nullptr, m_activate != nullptr, m_list_add != nullptr, m_list_ctor != nullptr);
    }
    return g_ready_types;
}

Obj* observer() {
    Obj* list = find_components(t_obs);
    return list_count(list) > 0 ? list_item(list, 0) : nullptr;
}
Obj* find_controller() {
    Obj* list = find_components(t_ctrl);
    for (int i = 0, n = list_count(list); i < n; ++i) {
        Obj* c = list_item(list, i);
        int32_t t = -1;
        if (c && get_i32(c, "<RoomType>k__BackingField", t) && t == kRoomType) return c;
    }
    return nullptr;
}
Obj* rtt() {
    if (g_rtt) return g_rtt;
    auto* rm = api().resource_manager();
    auto* res = rm ? rm->create_resource("via.render.RenderTargetTextureResource", kRtt) : nullptr;
    if (!res) return nullptr;
    res->add_ref();
    auto fn = api().sdk()->resource->create_holder;
    Obj* h = fn ? reinterpret_cast<Obj*>(fn(reinterpret_cast<REFrameworkResourceHandle>(res), "via.render.RenderTargetTextureResourceHolder")) : nullptr;
    res->release();
    if (h) h->add_ref();
    g_rtt = h;
    return g_rtt;
}

void drop_request_objects() {
    if (g_req) { g_req->release(); g_req = nullptr; }
    if (g_list) { g_list->release(); g_list = nullptr; }
}
// DeactivateCaptureRoom(5, ""): the room and its fighter go
void give_back(const char* why) {
    if (!g_room) return;
    g_room = false;
    g_ready = false;
    g_activated = false;
    g_ctrl = nullptr;
    if (Obj* obs = observer()) {
        Obj* empty = new_string("");
        if (empty) empty->add_ref();
        call(m_deactivate, obs, { arg_i32(kRoomType), arg_obj(empty) });
        if (empty) empty->release();
    }
    drop_request_objects();
    logf("replay tab: 3D preview room given back (%s)", why);
}
// RequestLoadCaptureRoom(5, [RequestFighterData], null, "")
bool ask(const Want& w) {
    Obj* obs = observer();
    if (!obs) return false;
    Obj* list = t_list->create_instance(0);
    if (!list) list = t_list->create_instance(1);
    bool owned = false;                           // a copy from the core comes with our reference already
    if (!list) {
        // a copy of an existing list of that type (a capture controller's), given its own array by .ctor(Int32)
        Obj* cl = find_components(t_ctrl);
        for (int i = 0, n = list_count(cl); i < n && !list; ++i)
            if (Obj* src = get_obj(list_item(cl, i), "RequestObjDatas")) { list = ui::clone(src); owned = list != nullptr; }
    }
    Obj* req = t_req->create_instance(1);
    if (!list || !req) {
        if (list && owned) list->release();
        logf("replay tab: 3D preview: request objects not made (list %p, request %p)", (void*)list, (void*)req);
        return false;
    }
    if (!owned) list->add_ref();
    req->add_ref();
    call(m_list_ctor, list, { arg_i32(4) });
    set_i32(req, "FighterID", w.fid);
    set_i32(req, "CostumeID", w.cos);
    set_i32(req, "ColorId", w.col);
    set_bool(req, "IsNear", false);
    call(m_list_add, list, { arg_obj(req) });
    if (list_count(list) != 1) { req->release(); list->release(); logf("replay tab: 3D preview: request list refused the fighter"); return false; }
    Obj* empty = new_string("");
    if (empty) empty->add_ref();
    const auto r = call(m_request, obs, { arg_i32(kRoomType), arg_obj(list), nullptr, arg_obj(empty) });
    if (empty) empty->release();
    if (r.exception_thrown) { req->release(); list->release(); logf("replay tab: 3D preview: the request threw"); return false; }
    drop_request_objects();
    g_list = list;
    g_req = req;
    g_room = true;
    g_asked = w;
    g_asked_at = g_frame;
    g_activated = false;
    g_ready = false;
    g_ctrl = nullptr;
    ++g_requests;
    return true;
}

// The page shows this fighter in this outfit (fid 0: nothing)
void want(int fid, int cos, int col) {
    Want w{ fid, cos, col };
    if (!(w == g_want)) { g_want = w; g_want_since = g_frame; }
}

// The room's fighter: the one asked, and how far it is
bool fighter_state(Obj* ctrl, int* fid, int* col, int* vis, int* state) {
    Obj* oc = ctrl ? get_obj(ctrl, "ObjectCtrl") : nullptr;
    Obj* mo = oc ? get_obj(oc, "ManageObjects") : nullptr;
    if (list_count(mo) < 1) return false;
    Obj* f = list_item(mo, 0);
    if (!alive(f)) return false;
    get_i32(f, "<FighterId>k__BackingField", *fid);
    get_i32(f, "<ColorId>k__BackingField", *col);
    get_i32(f, "<VisualId>k__BackingField", *vis);
    get_i32(f, "CurrentState", *state);
    return true;
}

// Every frame while the page is up: the room follows the wanted fighter
void frame() {
    if (g_off || !types()) return;
    if (!g_want.fid) { give_back("no fighter"); return; }
    // ask again once the choice held still
    if ((!g_room || !(g_asked == g_want)) && g_frame - g_want_since >= kSettle) {
        if (g_room) give_back("another fighter / outfit / colour");
        if (!ask(g_want)) {
            if (++g_fails >= kTries) { g_off = true; logf("replay tab: 3D preview off for the session (%d failed requests)", g_fails); }
            g_want_since = g_frame;               // tried again after kSettle
            return;
        }
        logf("replay tab: 3D preview asked: fighter %d costume %d colour %d (request %u)", g_want.fid, g_want.cos, g_want.col, g_requests);
    }
    if (!g_room) return;
    if (g_ctrl && !alive(g_ctrl)) g_ctrl = nullptr;
    if (!g_ctrl && (g_frame - g_poll_at >= kPollEvery)) { g_poll_at = g_frame; g_ctrl = find_controller(); }
    if (!g_ctrl) {
        if (g_frame - g_asked_at > kTimeout) {
            logf("replay tab: 3D preview: no room %u frames after the request", kTimeout);
            give_back("no room");
            if (++g_fails >= kTries) { g_off = true; logf("replay tab: 3D preview off for the session (%d failures)", g_fails); }
        }
        return;
    }
    bool setup = false;
    get_bool(g_ctrl, "<IsSetUpEnd>k__BackingField", setup);
    if (setup && !g_activated) {
        g_activated = true;
        Obj* empty = new_string("");
        if (empty) empty->add_ref();
        call(m_activate, g_ctrl, { arg_obj(empty), arg_bool(false), arg_bool(false) });
        if (empty) empty->release();
    }
    int fid = 0, col = -1, vis = -1, state = -1;
    const bool has = fighter_state(g_ctrl, &fid, &col, &vis, &state);
    const uint32_t since = g_frame - g_asked_at;
    const bool fresh = since >= kFresh && (vis == g_asked.cos || since >= kVisualWait);
    const bool now = has && fresh && fid == g_asked.fid && col == g_asked.col && state == kStateReady && g_activated;
    if (now && !g_ready) {
        if (!g_ready_since) g_ready_since = g_frame;
        if (g_frame - g_ready_since >= kShowAfter) {
            g_ready = true;
            g_fails = 0;
            logf("replay tab: 3D preview ready: fighter %d visual %d colour %d, %u frames after the request", fid, vis, col, g_frame - g_asked_at);
        }
    } else if (!now) {
        g_ready_since = 0;
        if (g_ready) logf("replay tab: 3D preview lost its model (fighter %d state %d)", fid, state);
        g_ready = false;
        if (g_frame - g_asked_at > kTimeout && has && state != kStateReady) {
            logf("replay tab: 3D preview: the fighter stays in state %d", state);
            give_back("model not ready");
            if (++g_fails >= kTries) { g_off = true; logf("replay tab: 3D preview off for the session (%d failures)", g_fails); }
        }
    }
}
// The model shows exactly what the page wants
bool showing() { return !g_off && g_ready && g_asked == g_want; }
void stop(const char* why) {
    give_back(why);
    g_want = Want{};
    g_ready_since = 0;
}
}  // namespace preview

// ---- input ----
struct Rep {
    bool held = false;
    uint32_t next = 0;
    bool step(bool now) {                          // a press, then repeats after 18 frames, every 5
        if (!now) { held = false; return false; }
        if (!held) { held = true; next = g_frame + 18; return true; }
        if (g_frame >= next) { next = g_frame + 5; return true; }
        return false;
    }
    bool press(bool now) {                         // a press only
        const bool p = now && !held;
        held = now;
        return p;
    }
    void prime(bool now) { held = now; next = g_frame + 1000000; }   // a key held at entry acts only once released
};
Rep k_up, k_down, k_left, k_right, k_color, k_back, k_ok;
Method* m_pad_merged = nullptr;
Method* m_pad_button = nullptr;
uint32_t b_up = 1, b_down = 2, b_left = 4, b_right = 8, b_x = 64, b_l1 = 256, b_b = 128, b_a = 32;   // via.hid.GamePadButton
uint32_t b_sup = 0, b_sdown = 0, b_sleft = 0, b_sright = 0;

bool focused() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}
bool sc_down(UINT sc) {
    const UINT vk = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK);
    return vk && (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}
bool vk_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
uint32_t pad_buttons() {
    void* s = api().get_native_singleton("via.hid.GamePad");
    Obj* dev = (s && m_pad_merged) ? call_obj(m_pad_merged, reinterpret_cast<Obj*>(s)) : nullptr;
    if (!dev) return 0;
    if (!m_pad_button) m_pad_button = method_of(dev->get_type_definition(), "get_Button", 0);
    auto r = call(m_pad_button, dev);
    return r.exception_thrown ? 0u : r.dword;
}
struct Keys { bool up, down, left, right, color, back, ok; };
Keys read_keys() {
    Keys k{};
    if (focused()) {
        k.up = vk_down(VK_UP) || sc_down(0x11);
        k.down = vk_down(VK_DOWN) || sc_down(0x1F);
        k.left = vk_down(VK_LEFT) || sc_down(0x1E);
        k.right = vk_down(VK_RIGHT) || sc_down(0x20);
        k.color = sc_down(0x13);                   // R (its place on a QWERTY keyboard)
        k.back = vk_down(VK_ESCAPE) || sc_down(0x10);   // Esc, the tab-back key (Q's place: A on AZERTY)
        k.ok = vk_down(VK_RETURN) || sc_down(0x21);     // Enter, F (the game's Confirm)
    }
    const uint32_t b = pad_buttons();
    k.up = k.up || (b & b_up) || (b_sup && (b & b_sup));
    k.down = k.down || (b & b_down) || (b_sdown && (b & b_sdown));
    k.left = k.left || (b & b_left) || (b_sleft && (b & b_sleft));
    k.right = k.right || (b & b_right) || (b_sright && (b & b_sright));
    k.color = k.color || (b & b_x);
    k.back = k.back || (b & b_l1) || (b & b_b);
    k.ok = k.ok || (b & b_a);
    return k;
}
void prime_keys() {
    const Keys k = read_keys();
    k_up.prime(k.up); k_down.prime(k.down); k_left.prime(k.left); k_right.prime(k.right); k_color.prime(k.color);
    k_back.prime(k.back); k_ok.prime(k.ok);
}

const Fighter* focused_fighter() {
    if (g_cat.empty()) return nullptr;
    g_cursor = std::max(0, std::min(g_cursor, static_cast<int>(g_cat.size()) - 1));
    return &g_cat[static_cast<size_t>(g_cursor)];
}
Choice choice_of(const Fighter& f) {
    auto it = g_choices.find(f.fid);
    return it != g_choices.end() ? it->second : Choice{};
}
// A row's Left / Right on the focused character (Outfit Settings On / Off, Costume, Color)
Choice row_step(const Fighter& f, const Choice& cur, int row, int dir) {
    if (row == 0) {                                // On / Off
        if (cur.forced) { g_last_on[f.fid] = cur; return Choice{}; }
        auto l = g_last_on.find(f.fid);
        if (l != g_last_on.end() && outfit_index(f, l->second) >= 0) return l->second;
        return cycle_outfit(f, Choice{}, 1);
    }
    if (row == 1) {                                // the outfit; Default is the Off of row 0, skipped here
        Choice n = cycle_outfit(f, cur, dir);
        if (!n.forced) n = cycle_outfit(f, n, dir);
        return n;
    }
    return cur.forced ? cycle_color(f, cur, dir) : cur;   // the colour
}
void apply_choice(const Fighter& f, const Choice& cur, const Choice& next) {
    if (next == cur) return;
    if (next.forced) g_choices[f.fid] = next; else g_choices.erase(f.fid);
    save_choices();
    g_dirty = true;
    logf("replay tab: %s -> %s", f.name.c_str(), row_text(f, next).c_str());
}

void input() {
    const Keys k = read_keys();
    const bool back = k_back.press(k.back), ok = k_ok.press(k.ok);
    const Fighter* f = focused_fighter();
    if (g_edit) {
        if (back || ok) { g_edit = false; g_dirty = true; return; }   // back to the grid
        if (k_up.step(k.up)) { g_row = (g_row + kPanelRows - 1) % kPanelRows; g_dirty = true; }
        if (k_down.step(k.down)) { g_row = (g_row + 1) % kPanelRows; g_dirty = true; }
        if (!f) return;
        const Choice cur = choice_of(*f);
        Choice next = cur;
        if (k_left.step(k.left)) next = row_step(*f, next, g_row, -1);
        if (k_right.step(k.right)) next = row_step(*f, next, g_row, 1);
        if (k_color.step(k.color)) next = cycle_color(*f, next, 1);
        apply_choice(*f, cur, next);
        return;
    }
    if (back) {                                    // back to Search (the page's input comes back once released)
        g_ours.store(false);
        g_leaving = true;
        return;
    }
    const int n = static_cast<int>(g_cat.size());
    const int before = g_cursor;
    if (k_up.step(k.up)) g_cursor = grid_move(g_cursor, 0, -1, n, kCols);
    if (k_down.step(k.down)) g_cursor = grid_move(g_cursor, 0, 1, n, kCols);
    if (k_left.step(k.left)) g_cursor = grid_move(g_cursor, -1, 0, n, kCols);
    if (k_right.step(k.right)) g_cursor = grid_move(g_cursor, 1, 0, n, kCols);
    if (g_cursor != before) g_dirty = true;
    if (!f) return;
    if (ok) { g_edit = true; g_row = 0; g_dirty = true; return; }
    if (k_color.step(k.color)) { const Choice cur = choice_of(*f); apply_choice(*f, cur, cycle_color(*f, cur, 1)); }
}

// ---- the screen ----
bool actual_visible(Obj* ctrl) {
    bool ok = false;
    const bool v = ctrl && call_bool(ctrl, "get_ActualVisible", {}, &ok);
    return ok && v;
}
void* find(void* root, const char* name) { return root ? A()->ui_find_cached(root, name) : nullptr; }
void set_vis(void* c, bool on) { if (c) A()->ui_set_visible(C(), c, on ? 1 : 0); }

// A tab item's look (via.gui PlayState: DEFAULT, SELECT, DISABLE), written when it differs
void set_state(void* item, const char* st) {
    if (!item) return;
    if (str(call_obj(static_cast<Obj*>(item), "get_PlayState")) == st) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_STR; a.v.s = st;
    A()->obj_invoke(C(), item, "set_PlayState(System.String)", &a, 1, nullptr);
}

// The tab bar: c_item_05 shown with our label (its mouse hit area hidden: the game's list has 5 items), selected while
// ours; the bar's right guide moved past it, once per screen build
void tab_bar(Obj* main) {
    void* item5 = find(main, "c_item_05");
    if (!item5) return;
    set_vis(item5, true);
    if (void* t = A()->ui_find_child(item5, "e_text_menu", 1)) A()->ui_set_text(C(), t, kTabLabel);
    set_vis(A()->ui_find_child(item5, "e_hit_mouse", 1), false);
    set_vis(A()->ui_find_child(item5, "c_batch", 1), false);
    set_state(item5, g_ours.load() ? "SELECT" : "DEFAULT");
    if (g_ours.load()) set_state(find(main, "c_item_04"), "DEFAULT");
    if (!g_guide_moved) {
        g_guide_moved = true;
        Obj* guide = static_cast<Obj*>(find(main, "p_InputGuide_R_s_dUITabR"));
        Obj* item4 = static_cast<Obj*>(find(main, "c_item_04"));
        float gp[3] = {}, p4[3] = {}, p5[3] = {};
        auto pos = [](Obj* o, float out[3]) {
            if (!o) return false;
            auto r = call(o, "get_Position");
            if (r.exception_thrown) return false;
            std::memcpy(out, r.bytes.data(), 12);
            return true;
        };
        if (pos(guide, gp) && pos(item4, p4) && pos(static_cast<Obj*>(item5), p5)) {
            const float x = gp[0] + (p5[0] - p4[0]);
            A()->ui_set_position(C(), guide, x, gp[1], gp[2]);
            logf("replay tab: tab bar: item 4 x %.0f, item 5 x %.0f, right guide x %.0f -> %.0f", p4[0], p5[0], gp[0], x);
        }
    }
}

// The Search page's agents: every visible CFN* agent but the hub and the tab bar's
void hide_page(bool quiet) {
    g_hidden.clear();
    TD* td = api().tdb()->find_type("app.UIAgent");
    Obj* list = find_components(td);
    for (int i = 0, n = list_count(list); i < n; ++i) {
        Obj* ag = list_item(list, i);
        Obj* go = ag ? call_obj(ag, "get_GameObject") : nullptr;
        const std::string nm = go ? str(call_obj(go, "get_Name")) : "";
        if (nm.rfind("CFN", 0) != 0 || nm == "CFNTop" || nm == kTop) continue;
        Obj* cm = call_obj(ag, "get_ControlMain");
        if (!actual_visible(cm)) continue;
        A()->ui_set_block_input(C(), ag, 1);
        A()->ui_set_force_invisible(C(), cm, 1);
        g_hidden.push_back(nm);
    }
    std::string names;
    for (auto& s : g_hidden) names += (names.empty() ? "" : ", ") + s;
    if (!quiet || !names.empty()) logf("replay tab: page hidden: %s", names.empty() ? "(no agent found)" : names.c_str());
}
void restore_page() {
    for (const std::string& nm : g_hidden) {
        void* buf[4] = {};
        const uint32_t n = A()->ui_agents(nm.c_str(), buf, 4);
        for (uint32_t i = 0; i < n && i < 4; ++i) {
            Obj* ag = static_cast<Obj*>(buf[i]);
            A()->ui_set_block_input(C(), ag, 0);
            if (Obj* cm = call_obj(ag, "get_ControlMain")) A()->ui_set_force_invisible(C(), cm, 0);
        }
    }
    g_hidden.clear();
}

void hide_copies(Obj* main) {
    if (!g_copies_shown) return;
    g_copies_shown = false;
    void* title = find(main, "c_title");
    if (!title) return;
    for (const Copy& cp : copies()) set_vis(A()->ui_find_child(title, cp.name.c_str(), 0), false);
}

void* copy_of(void* title, const std::string& name) { return A()->ui_find_cached(title, name.c_str()); }
void show(void* c, bool on) {
    if (!c) return;
    A()->ui_set_force_invisible(C(), c, 0);
    A()->ui_set_visible(C(), c, on ? 1 : 0);
}
void set_priority(void* c, int prio) {
    SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = prio;
    A()->obj_invoke(C(), c, "set_Priority(System.UInt16)", &a, 1, nullptr);
}
// Frames and pictures: placed by their top left corner
void style_box(void* c, int prio) {
    if (!c || g_styled.count(reinterpret_cast<uintptr_t>(c))) return;
    g_styled.insert(reinterpret_cast<uintptr_t>(c));
    if (g_cp_lefttop >= 0) {
        SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = g_cp_lefttop;
        A()->obj_invoke(C(), c, "set_ControlPoint(via.gui.ControlPoint)", &a, 1, nullptr);
    }
    set_priority(c, prio);
}
void put_box(void* title, const std::string& name, float x, float y, float w, float h, uint32_t col, bool on, int prio = kPrioFrame) {
    void* c = copy_of(title, name);
    if (!c) return;
    style_box(c, prio);
    if (on) {
        A()->ui_set_color(C(), c, col);
        A()->ui_set_position(C(), c, x, y, 0);
        A()->ui_set_size(C(), c, w, h);
    }
    show(c, on);
}
void put_image(void* title, const std::string& name, int fid, float x, float y, float w, float h, bool on) {
    void* c = copy_of(title, name);
    if (!c) return;
    style_box(c, kPrioImage);
    auto it = g_faces.find(fid);
    void* holder = it != g_faces.end() ? it->second : nullptr;
    const bool ready = on && holder && A()->ui_texture_ready(holder) == 1;
    if (ready) {
        A()->ui_set_texture_rect(C(), c, holder, 0.0f, kFaceV0, 1.0f, kFaceVH);
        A()->ui_set_color(C(), c, kColText);
        A()->ui_set_position(C(), c, x, y, 0);
        A()->ui_set_size(C(), c, w, h);
    } else if (on && holder) {
        g_dirty = true;                            // drawn again once its picture has loaded
    }
    show(c, ready);
}
void put_model(void* title, bool on) {
    void* c = copy_of(title, "e_ro_model");
    if (!c) return;
    style_box(c, kPrioImage);
    if (on && g_model_tex != reinterpret_cast<uintptr_t>(c)) {
        Obj* h = preview::rtt();
        if (!h) on = false;
        else {
            SF6CoreArg a{}; a.type = SF6CORE_ARG_PTR; a.v.p = h;
            A()->obj_invoke(C(), c, "setTexture(via.render.TextureResourceHolder)", &a, 1, nullptr);
            SF6CoreArg t{}; t.type = SF6CORE_ARG_I64; t.v.i = 1;
            A()->obj_invoke(C(), c, "set_UVType(via.gui.UVValueType)", &t, 1, nullptr);
            auto setf = [&](const char* sig, double v) { SF6CoreArg f{}; f.type = SF6CORE_ARG_F64; f.v.f = v; A()->obj_invoke(C(), c, sig, &f, 1, nullptr); };
            setf("set_UVU(System.Single)", kModelU);
            setf("set_UVV(System.Single)", kModelV);
            setf("set_UVW(System.Single)", kModelUW);
            setf("set_UVH(System.Single)", kModelVH);
            g_model_tex = reinterpret_cast<uintptr_t>(c);
        }
    }
    if (on) {
        A()->ui_set_color(C(), c, kColText);
        A()->ui_set_position(C(), c, kModelX, kModelY, 0);
        A()->ui_set_size(C(), c, kModelW, kModelH);
    }
    show(c, on);
}
void put_text(void* title, const std::string& name, const std::string& text, float x, float y, float font, uint32_t col, bool on = true) {
    void* c = copy_of(title, name);
    if (!c) return;
    if (!g_styled.count(reinterpret_cast<uintptr_t>(c))) {
        g_styled.insert(reinterpret_cast<uintptr_t>(c));
        SF6CoreArg t{}; t.type = SF6CORE_ARG_I64; t.v.i = 1;
        A()->obj_invoke(C(), c, "set_AutoRegionFit(System.Boolean)", &t, 1, nullptr);
        set_priority(c, kPrioText);
    }
    if (on) {
        A()->ui_set_text(C(), c, text.c_str());
        A()->ui_set_font_size(C(), c, font, font);
        A()->ui_set_color(C(), c, col);
        A()->ui_set_position(C(), c, x, y, 0);
    }
    show(c, on);
}

void render(Obj* main) {
    void* title = find(main, "c_title");
    if (!title) return;
    if (!g_dirty && g_copies_shown && g_frame - g_drawn_at < 20) return;
    g_dirty = false;
    g_drawn_at = g_frame;
    const int n = static_cast<int>(g_cat.size());
    const int rows = (n + kCols - 1) / kCols;
    g_top_row = window_top(g_top_row, n ? g_cursor / kCols : 0, rows, kVisRows);
    // the grid
    for (int i = 0; i < kCells; ++i) {
        const int idx = g_top_row * kCols + i;
        const bool on = idx < n;
        const float x = kGridX + (i % kCols) * kStepX, y = kGridY + (i / kCols) * kStepY;
        put_box(title, cname("cbg", i), x, y, kCellW, kCellH, idx == g_cursor ? kColCellFocus : kColCell, on);
        if (!on) {
            put_image(title, cname("cimg", i), 0, 0, 0, 0, 0, false);
            put_text(title, cname("cname", i), "", 0, 0, 0, 0, false);
            put_text(title, cname("con", i), "", 0, 0, 0, 0, false);
            continue;
        }
        const Fighter& f = g_cat[static_cast<size_t>(idx)];
        const bool forced = choice_of(f).forced;
        put_image(title, cname("cimg", i), f.fid, x + kCellW - 96, y, 96, kCellH, true);
        put_text(title, cname("cname", i), grid_caption(f.name), x + 10, y + 40, kFontCell, idx == g_cursor ? kColDark : kColText);
        put_text(title, cname("con", i), "On", x + 10, y + 6, kFontCell, kColOn, forced);
    }
    const int vis = g_cursor - g_top_row * kCols;
    const bool cell_on = n > 0 && vis >= 0 && vis < kCells;
    put_box(title, "e_ro_cfocus", kGridX + (vis % kCols) * kStepX - 3, kGridY + (vis / kCols) * kStepY - 3, kCellW + 6, kCellH + 6,
            kColBorder, cell_on, kPrioBorder);
    // the focused character's settings, on the left
    const Fighter* f = focused_fighter();
    const Choice c = f ? choice_of(*f) : Choice{};
    const int oi = f ? outfit_index(*f, c) : -1;
    put_text(title, "e_ro_pname", f ? grid_caption(f->name) : "", kRowX + 20, kRowY - 80, kFontName, kColText, f != nullptr);
    const char* labels[kPanelRows] = { "Outfit Settings", "Costume", "Color" };
    // a long mod name is cut so that it stays inside its row (the grid starts right after it)
    auto fit = [](std::string v) {
        constexpr size_t kMax = 18;
        if (v.size() <= kMax) return v;
        size_t cut = kMax - 1;
        while (cut > 0 && (static_cast<unsigned char>(v[cut]) & 0xC0) == 0x80) --cut;   // not inside a UTF-8 sequence
        return v.substr(0, cut) + "...";
    };
    std::string values[kPanelRows] = { c.forced ? "On" : "Off", oi >= 0 ? fit(f->outfits[static_cast<size_t>(oi)].name) : "-",
                                       c.forced ? color_label(c.color) : "-" };
    for (int r = 0; r < kPanelRows; ++r) {
        const float y = kRowY + r * kRowStep;
        const bool foc = g_edit && r == g_row;
        const uint32_t col = (foc || r == 0 || c.forced) ? kColText : kColDim;
        put_box(title, cname("rbg", r), kRowX, y, kRowW, kRowH, foc ? kColRowFocus : kColRow, f != nullptr);
        put_text(title, cname("rlbl", r), labels[r], kRowX + 24, y + 12, kFontRow, col, f != nullptr);
        put_text(title, cname("rval", r), values[r], kRowX + 380, y + 12, kFontRow, col, f != nullptr);
    }
    put_box(title, "e_ro_rfocus", kRowX - 3, kRowY + g_row * kRowStep - 3, kRowW + 6, kRowH + 6, kColBorder, g_edit && f != nullptr,
            kPrioBorder);
    // Off (Default): no preview at all, the room given back; On: the model, the portrait while it loads
    const bool forced = f != nullptr && c.forced;
    preview::want(forced ? f->fid : 0, forced ? c.costume : 0, forced ? c.color : 0);
    const bool model = forced && preview::showing();
    g_model_shown = model;
    put_model(title, model);
    put_image(title, "e_ro_bigimg", f ? f->fid : 0, kBigX, kBigY, kBigW, kBigH, forced && !model);
    const std::string help = g_edit ? "Up / Down: setting     Left / Right: change     R / Square (X): colour     F / Esc: done"
                                    : "F: edit     Arrows: character     R / Square (X): colour     " + g_back_hint + " / Esc: back";
    put_text(title, "e_ro_help", help, kRowX + 20, 960, kFontHelp, kColDim);
    g_copies_shown = true;
    if (!g_place_logged && g_frame - g_enter_frame >= 3) {
        g_place_logged = true;
        if (Obj* cc = static_cast<Obj*>(copy_of(title, cname("cbg", 0)))) {
            auto r = call(cc, "get_Position");
            float p[3] = {};
            if (!r.exception_thrown) std::memcpy(p, r.bytes.data(), 12);
            logf("replay tab: first cell placed at %.0f, %.0f (asked %.0f, %.0f), control point LeftTop = %lld", p[0], p[1], kGridX,
                 kGridY, static_cast<long long>(g_cp_lefttop));
        }
    }
}

void enter(Obj* main) {
    g_enter_frame = g_frame;
    if (void* g = find(main, "p_InputGuide_L_s_dUITabL")) {
        if (Obj* t = static_cast<Obj*>(A()->ui_find_child(g, "e_text_0", 1))) {
            const std::string m = str(call_obj(t, "get_Message"));
            if (!m.empty()) g_back_hint = m;
        }
    }
    g_place_logged = false;
    g_edit = false;
    g_dirty = true;
    build_catalog();                              // each time: the NSFW setting may have changed
    load_faces();
    load_choices();
    hide_page(false);
    prime_keys();
    g_cursor = std::max(0, std::min(g_cursor, static_cast<int>(g_cat.size()) - 1));
    logf("replay tab: entered (%zu forced in the file, back key shown as %s)", g_choices.size(), g_back_hint.c_str());
}

void leave(Obj* main, const char* why) {
    preview::stop(why);
    restore_page();
    hide_copies(main);
    if (main) set_state(find(main, "c_item_04"), "SELECT");
    if (main) set_state(find(main, "c_item_05"), "DEFAULT");
    g_ours.store(false);
    g_edit = false;
    logf("replay tab: left (%s)", why);
}

void down() {                                      // the screen went: nothing of ours left behind
    g_leaving = false;
    preview::stop("the Replays screen closed");
    if (g_ours.load() || !g_hidden.empty()) leave(nullptr, "the Replays screen closed");
    g_enter.store(false);
    g_leave.store(false);
    g_top_addr.store(0);
    g_main_addr.store(0);
    g_tab.store(-1);
    g_up = false;
    g_main_seen = 0;
    g_copies_shown = false;
    g_styled.clear();
    g_model_tex = 0;
}

void init() {
    const SF6CoreAPI* a = A();
    if (!a || !C() || !SF6CORE_HAS(a, ui_clone_control) || !SF6CORE_HAS(a, ui_agents) || !SF6CORE_HAS(a, ui_find_cached) ||
        !SF6CORE_HAS(a, ui_set_texture_rect)) {
        logf("replay tab: off (needs the core with ui_clone_control and ui_set_texture_rect, 0.8.2)");
        return;
    }
    TD* td = api().tdb()->find_type("app.UIAgent");
    Method* m_tab = td ? td->find_method("InputTab(app.UIPartsItemDirection, app.InputDigitalFlag)") : nullptr;
    if (!m_tab) m_tab = td ? method_of(td, "InputTab", 2) : nullptr;
    Method* m_next = td ? method_of(td, "InputTabNext", 1) : nullptr;
    Method* m_back = td ? method_of(td, "InputTabBack", 1) : nullptr;
    const bool h1 = m_tab && m_tab->get_function_raw() && m_tab->add_hook(pre_input_tab, post_input_tab, false);
    const bool h2 = m_next && m_next->get_function_raw() && m_next->add_hook(pre_tab_next, post_none, false);
    const bool h3 = m_back && m_back->get_function_raw() && m_back->add_hook(pre_tab_back, post_none, false);
    if (!h1 && !h2) {
        logf("replay tab: off (app.UIAgent.InputTab / InputTabNext not hooked)");
        return;
    }
    TD* tdb_pad = api().tdb()->find_type("via.hid.GamePad");
    m_pad_merged = tdb_pad ? method_of(tdb_pad, "get_MergedDevice", 0) : nullptr;
    if (TD* btn = api().tdb()->find_type("via.hid.GamePadButton")) {
        auto bit = [&](const char* f, uint32_t fb) { auto* x = btn->find_field(f); return x ? static_cast<uint32_t>(x->get_data<int32_t>(nullptr)) : fb; };
        b_up = bit("LUp", 1); b_down = bit("LDown", 2); b_left = bit("LLeft", 4); b_right = bit("LRight", 8); b_x = bit("RLeft", 64);
        b_l1 = bit("LTrigTop", 256); b_b = bit("RRight", 128); b_a = bit("RDown", 32);
        b_sup = bit("EmuLup", 0); b_sdown = bit("EmuLdown", 0); b_sleft = bit("EmuLleft", 0); b_sright = bit("EmuLright", 0);
    }
    if (TD* cp = api().tdb()->find_type("via.gui.ControlPoint"))
        if (auto* fld = cp->find_field("LeftTop")) g_cp_lefttop = fld->get_data<int32_t>(nullptr);
    g_ok = true;
    logf("replay tab: on (hooks InputTab %d, InputTabNext %d, InputTabBack %d; pad bits %u %u %u %u x %u a %u; LeftTop %lld)", h1,
         h2, h3, b_up, b_down, b_left, b_right, b_x, b_a, static_cast<long long>(g_cp_lefttop));
}

void tick() {
    void* buf[2] = {};
    const uint32_t n = A()->ui_agents(kTop, buf, 2);
    Obj* agent = n ? static_cast<Obj*>(buf[0]) : nullptr;
    Obj* main = agent ? call_obj(agent, "get_ControlMain") : nullptr;
    if (!main || !actual_visible(main)) {
        if (g_up) down();
        return;
    }
    if (!g_up) logf("replay tab: Replays screen up");
    g_up = true;
    g_top_addr.store(reinterpret_cast<uintptr_t>(agent), std::memory_order_release);
    g_main_addr.store(reinterpret_cast<uintptr_t>(main), std::memory_order_release);
    if (g_main_seen != reinterpret_cast<uintptr_t>(main)) {   // a new build of the screen
        g_main_seen = reinterpret_cast<uintptr_t>(main);
        g_guide_moved = false;
        g_styled.clear();
        g_copies_shown = false;
        g_dirty = true;
        g_model_tex = 0;
    }
    int32_t sel = -1;
    if (Obj* ti = get_obj(agent, "_TabItem")) {
        auto r = call(ti, "get_SelectedIndex");
        if (!r.exception_thrown) sel = static_cast<int32_t>(r.dword);
    }
    const int st = g_copy_status.load();
    if (st != g_status_logged) { g_status_logged = st; logf("replay tab: copies status %d (refused %d)", st, g_refused.load()); }
    if (g_ours.load()) {
        if (sel != kSearch && sel >= 0) {             // the game changed tab by itself (a mouse click): ours gives way
            leave(main, "the game changed tab");
            g_enter.store(false);
        }
    } else {
        g_tab.store(sel, std::memory_order_release);
    }
    if (g_enter.exchange(false) && g_ours.load()) enter(main);
    if (g_leave.exchange(false)) leave(main, "tab back");
    if (g_leaving) {
        hide_copies(main);
        const Keys k = read_keys();
        if (!k.back) { g_leaving = false; leave(main, "back key"); }
    }
    tab_bar(main);
    if (g_ours.load()) {
        if (g_frame % 30 == 0 && g_hidden.empty()) hide_page(true);   // the page's agents were not up yet at entry
        input();
        render(main);                             // sets the wanted model
        preview::frame();
        if (preview::showing() != g_model_shown) g_dirty = true;
    } else {
        hide_copies(main);
    }
}

}  // namespace

void frame() {
    if (!g_init) { g_init = true; try { init(); } catch (...) { logf("replay tab: EXCEPTION at init"); } }
    if (!g_ok) return;
    if (!g_up && !g_ours.load() && !g_leaving && (g_frame % kPollEvery) != 0) return;
    try {
        tick();
    } catch (const std::exception& e) {
        logf("replay tab: EXCEPTION %s", e.what());
    } catch (...) {
        logf("replay tab: EXCEPTION (unknown)");
    }
}

}  // namespace slots::replaytab
