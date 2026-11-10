// SF6 Slots — "Settings": a 6th tab of the Replays menu (agent CFNReplayTop), after Search.
//
// Laid out and working like Battle Settings > Other (read 05/11): three option rows on the left, "Replay Outfit Settings",
// "Stage Settings" and Replay Music (Up / Down, wrapping; Confirm goes into the focused row's page; Back (Esc, the tab-back key,
// pad B / L1) goes back to Search), the divider line, and on the right the focused row's page:
//   - Replay Outfit Settings, as Opponent Outfit Settings:
//     - the grid: the character portraits (4 columns, the game's order, "On" over a character whose replays get a chosen
//       outfit, the name below), dimmed while the option rows have the focus; in it the row turns dark with its small
//       marker, the panel lights up, the focused portrait is light (the last one focused). Arrows move (wrapping),
//       Confirm opens the character's page, Esc (pad B) goes back to the rows; the tab keys do nothing (as the game's);
//     - the character's page: the tab bar gives way to the character's name (the game's header), its settings as the
//       game's rows ("Outfit Settings < On >", "Costume < Outfit 1 >", "Color < 01 >": Up / Down a row, wrapping, Left /
//       Right its value; Off disables the two rows below, the focus stays on the first; a costume change puts the colour
//       back to 01) and the fighter in 3D at the game's place (a colour change recolours it in place, as the game's page
//       does). The colours: the outfit's game colours, then the Color Mod's RW / MC entries with its own labels
//       (colormod_link.hpp: each rides on colour 02; the preview and the replay ask the Color Mod for exactly that
//       entry, or for the game's own colours), then Random (the game's word: each playback draws one of the outfit's colours,
//       as a Random colour does in a battle; previewed in 01). The character's Random outfit is the Costume row's last value, its Color row
//       reading Random, previewed as Outfit 1: a replay set to it wears a draw of the character's pool at each playback (the
//       Random outfit's rules, replay_outfits.cpp). For an outfit with costume options, R / pad X opens the same Costume
//       options menu as Edit Character (without its Player side row; the guide bar says "Costume options"), the choices
//       kept per fighter and outfit for replays only. Back (Esc, pad B) goes back to the grid; Confirm and the tab
//       keys do nothing there, as on the game's page;
//   - Stage Settings, as the game's: the banner of the focused (or chosen) stage with its name, a 3-column grid with its
//     scroll bar, "Default" first (the replay's own stage), the chosen cell marked; Confirm picks the focused cell and
//     stays, Esc goes back to the rows. The list: stage_list(), Battle Settings' own, with the stage slots as Battle
//     Settings shows them (stages.hpp): the tiles as cells of their own after the game's stages, a stage's variations
//     cycled on its cell with the pad's X button (its keyboard key: stages::variation_key_down; "<key> Stage Variation"
//     in the guide bar while the focused stage has some), its one name line in the banner (with the game's shadow), the
//     chosen cell's yellow corner triangle. The choice is the replay menu's own
//     (stages::choose(kReplay), never Battle Settings' nor Training's): the replay keeps a vanilla stage id, the stage
//     slots serve the chosen tile / variation when the replay's description is set up.
//   - Replay Music, as the game's Battle BGM row (its values, in its words: replay_bgm.hpp), nothing on the right: Left / Right
//     its value; T / pad Y on any value (the game's "<key> Character Music" in the guide bar) opens the game's own Character
//     Music screen (replay_music.cpp): its playlists are the save's, shared with online; only the row's value is ours. While it
//     runs: our copies, the tab bar and the description give way, no input here; Back on it comes back to the row. On the row's
//     last value, Custom (ours), the entry reads "Custom Music List" and T opens the game's playlist dialog alone, on the list
//     of ours, which Back saves. The words: words.hpp (the game's own by GUID, ours by language).
//   - The summary panel on the right of the pages, as Battle Settings' (the title pill, the fighters' outfit lines, the
//     stage's picture and line, the music), drawn from the same choices.
// The game picks the colours with a grid of colour boxes (several at once); a choice here is one colour, hence the
// third row. On = the outfit a replay's fighter of that character wears (replay_logic.hpp; the playback override,
// replay_outfits.cpp, reads the file), Off = what the replay holds; the stage likewise. Every change writes
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
//   - The page is made of copies (the core's ui_clone_control, inside the tab hook: R19, the screen's own input
//     update), named e_ro_*, under c_title (root space); they live and die with the screen's tree (R35: found again by
//     name, every write through the core). Of the tab bar's screen: the header's title text (captions, the name), the
//     bar's solid mask rectangle (e_rect_mask_00, made a plain coloured rectangle: MaskType NonTarget; the cells, the
//     grid's panel), the header's line (e_scale9grid_line_00, the line with its end dots: turned 90 degrees, the
//     divider, as the game's own divider is the same line turned) and the "new" badge's texture (the portraits: the
//     game's own characterselect face pictures, loaded by path through the core, shown once loaded; the 3D model). The
//     rows (the option row too) are the game's own row: a
//     via.gui.SelectItem of the resident GUI Resident_Cmn_CertifiedMatch (c_SelectItem_0: the very background of the
//     game's selector rows, c_bg_m, dark with a bevel, light with the blue bar when selected, its PlayState DEFAULT /
//     SELECT; its value text, its darken; the label a copy of its plain value text, its own title text being tinted),
//     sized like the game's rows. The arrows: the main menu banner's two arrow textures (StartMenuUI, the same
//     12 x 25 arrows; rows without arrows when that screen is not there). Both GUIs stay loaded
//     longer than the Replays screen (the copies never outlive what they were copied from). The Search page's rows were
//     not used: their agent is rebuilt with each of its sub-tabs (Keyword / Conditional Search).
//     Text, place, size, colour, state and visibility from LateUpdateBehavior (R1), written when the page changed and
//     every 20 frames.
//   - The character's page hides the tab bar's items, guides, lower line and background and the title (ForceInvisible,
//     put back on the grid, on leaving, on the screen's next build) and writes our description in the bottom line
//     (InputGuide > p_ScrollText_0; the game's text put back when we leave, if it still shows ours).
//   - Input: read natively while the game window has the focus (keys by scancode, so any keyboard layout: W A S D
//     places and the arrows, F / Enter; via.hid.GamePad's merged device: d-pad, left stick, A, B, L1), never hooked.
//     The screen's whole input goes through the page's agent (its BlockInput also stops the tab bar's A / E and Esc:
//     seen 05/11), so Back is ours too; the page gets its input back only once that key is released.
//   - The tab items' look is their via.gui PlayState: c_item_05 is DISABLE as the game leaves it, DEFAULT shown,
//     SELECT while ours (c_item_04 DEFAULT meanwhile, SELECT again when we leave).
//   - The character in 3D, as the game's own page shows it: Battle Settings' fighter capture room, borrowed while the
//     character's page is up (namespace preview below); its render target on a texture copy, nothing meanwhile.
// Gate: nothing but one core registry read every 15 frames while the Replays screen is not up.
#include "slots.hpp"
#include "slots_ui.hpp"
#include "game_cell.hpp"
#include "replay_logic.hpp"
#include "name_scroll.hpp"
#include "scroll_logic.hpp"
#include "replay_music.hpp"
#include "replay_tab.hpp"
#include "words.hpp"
#include "colormod_link.hpp"
#include "rto.hpp"
#include "stages.hpp"
#include "stages_logic.hpp"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <set>

namespace slots::replaytab {
namespace {

using namespace slots::replay;
using words::G;
using words::W;
using words::game;
using words::tr;
using words::trf;

constexpr const char* kTop = "CFNReplayTop";
constexpr int kSearch = 4;                        // app.UICFNReplayTop.TabType.SEARCH
constexpr int kFirstTab = 0;                       // the game's first tab (Recommended Replays)
constexpr uint32_t kPollEvery = 15;
constexpr int kMaxRefused = 3;
constexpr uint32_t kActFlags = 2 | 8;             // app.InputDigitalFlag Trigger | Repeat
constexpr uint32_t kDirTabBack = 16, kDirTabNext = 32;   // app.UIPartsItemDirection
// layout, root space (1920 x 1080; c_title sits at the root's origin): Battle Settings > Other > Opponent Outfit
// Settings' own places (read 05/11)
constexpr int kCols = 4, kVisRows = 8, kCells = kCols * kVisRows, kRows = 3;
constexpr float kGridX = 760, kGridY = 184, kCellW = 160, kCellH = 72, kStepX = 168, kStepY = 80;
// the option list: its row 640 x 58 at (60, 150) (p_Group_0_v > p_GroupItem_*); the divider: the line 703 x 5 at
// (739, 150) turned 90 degrees, colour 80FFFFFF; the grid's panel 960 x 702 at (731, 150), 66000000, its focus tint
// 1CE98A31 while the grid has the focus (p_FighterSelectSimple_cos_h)
constexpr float kListX = 60, kListY = 150, kListW = 640, kListStep = 70;
constexpr float kLineX = 739, kLineY = 150, kLineLen = 703, kLineW = 5;
// The right side's background: ONE box for both pages (Wael 05/11, measured on his 42.png and 43.png), its left edge on the
// divider's line (the middle of its 5 units: the outfit page's began at 731, left of the squares), its top and bottom those of
// the outfit page (the stage page's sat at 155 - 860), its right end 1691.
constexpr float kBoxL = kLineX - kLineW / 2, kBoxT = 150, kBoxR = 1691, kBoxB = 852;
constexpr int kListRows = 3;                                // the option rows: Replay Outfit Settings, Stage Settings, Replay Music
constexpr int kBgmRow = 2;
std::string list_label(int row) {                           // the option rows' names (the stage's: the game's own)
    return row == 1 ? game(G::StageSettings) : tr(row == kBgmRow ? W::ReplayRowMusic : W::ReplayRowOutfits);
}
// the Replay Music row, in its 640 x 58 (the game's p_Spin_2, 05/11 on Wael's 59.png): the value's centre at (450, 29), the arrows'
// centres at (302, 29) and (599, 29)
constexpr float kListValueX = 450, kListValueW = 262, kListArrowL = 302, kListArrowR = 599;
// Stage Settings' page (c_StageSelect > p_StageSelectSimple_0_h, read 05/11): the banner 776 x 194 at (760, 184), its name
// centred on (1356, 359); the grid 3 columns of 252 x 63 cells, 262 x 73 apart from (760, 404), 6 rows shown; its scroll
// bar 6 x 430 at (1546, 404)
constexpr float kBannerX = 760, kBannerY = 184, kBannerW = 776, kBannerH = 194, kStageNameX = 1356, kStageNameY = 359;
// the name, as the game's e_text_stage (read 05/11, probe p14): its region 361 x 46 (centred 180 from the banner's right
// edge), font slot 6, 30 (raster 60), its colour, no shadow: a black glow, blur 20, strength 3. Never squeezed: a longer name
// is right-aligned, its region then the banner's width (stages_logic.hpp banner_name)
constexpr float kStageNameW = 361, kStageNameH = 46, kStageNameRaster = 60, kStageNameGlowBlur = 20, kStageNameGlow = 3;
constexpr float kStageNameRightW = stagelogic::kNameMaxW + 10;
constexpr int64_t kRightCenter = 6;                // via.gui.ControlPoint / PageAlignment: the text's right middle
constexpr int64_t kStageNameFontSlot = 6;
constexpr uint32_t kColStageName = 0xFFFFF3FA, kColStageNameGlow = 0xFF000000;
constexpr float kSGridX = 760, kSGridY = 404, kSCellW = 252, kSCellH = 63, kSStepX = 262, kSStepY = 73;
constexpr float kSBarX = 1546, kSBarY = 404, kSBarW = 6, kSBarH = 430;
constexpr int kSCols = 3, kSVisRows = 6, kSCells = kSCols * kSVisRows;
constexpr float kFontStageName = 30, kFontStageCell = 24;
// the chosen cell's mark: a yellow corner triangle (legs 20 along the cell's top and left edges, read
// on Battle Settings' grid 05/11), drawn as rows one unit high
constexpr int kSMarkLeg = 20;
// the character's page: its rows (p_Spin_*) 740 x 58, 70 apart from (156, 210); in a row, the label's left middle at
// (23, 29), the value's centre at (524, 29), the arrows' centres at (348, 29) and (700, 29); label font 25, value 23
constexpr float kRowX = 156, kRowY = 210, kRowW = 740, kRowH = 58, kRowStep = 70, kRowMatH = 280;
constexpr float kLabelX = 23, kMidY = 29, kLabelW = 320, kValueX = 524, kValueW = 300, kArrowL = 348, kArrowR = 700;
constexpr float kFontLabel = 25, kFontValue = 23;
constexpr size_t kValueMax = 24;                  // units (words.hpp): a longer outfit name is cut (it stays between the arrows)
// its fighter: the capture room's render target drawn 1:1 from x 320 (e_texture_rtt, 1920 x 1080 at 320, 0); the part
// on screen: u 0 - 0.8333
constexpr float kModelX = 320, kModelY = 0, kModelW = 1600, kModelH = 1080;
constexpr double kModelU = 0.0, kModelV = 0.0, kModelUW = 1600.0 / 1920.0, kModelVH = 1.0;
// its header: the character's name at the title's place, in the title's font (e_text_name, 58, 22, font 32)
constexpr float kNameX = 58, kNameY = 22, kFontName = 32;
constexpr float kFontCell = 24;
// via.Color raw (R in the low byte)
constexpr uint32_t kColText = 0xFFFFFFFF, kColOn = 0xFFFDBF3F, kColName = 0xFFFBBE40;   // "On": the game's e_text_on
constexpr uint32_t kColCell = 0xE62A201E, kColCellFocus = 0xF5EEEEEE;
constexpr uint32_t kColDark = 0xFF2A2A2A;         // the focused cell: light, dark caption (the game's)
// the option row: the grid dims its cells by 40 % (their e_darken 66000000); the cells, the panel and its tint: plain
// rectangles matched to the game's on screen (its panel's textured Scale9Grids, 66000000 / 1CE98A31, show lighter)
constexpr uint32_t kColCellDim = 0xE6241F1F, kColTextDim = 0xFF999999, kColOnDim = 0xFF987226;
constexpr uint32_t kColPanel = 0x3A000000, kColPanelFocus = 0x0CE98A31, kColLine = 0x80FFFFFF;
// a row's texts and arrows: light, dark on the selected row's light background, dimmer on a disabled row; the game
// darkens an unselected row by 20 % and a disabled one by 50 % (its e_darken over the row): the texts under the row's
// darken (its value) are written light, the ones over it (label, arrows) at the result (read on the game's page)
constexpr uint32_t kColRowText = 0xFFF3F3F3, kColRowTextSel = 0xFF4D4949, kColRowTextDim = 0xFFC1C1C1;
constexpr uint32_t kColRowTextOff = 0xFF7A7A7A;
constexpr uint32_t kColDarkenRow = 0x33000000, kColDarkenOff = 0x80000000;
// the option row while the grid has the focus (the game's p_GroupItem: SELECT, no blue bar, dark material, its marker)
constexpr uint32_t kColListMatDecided = 0xE61A1A1A, kColListDefaultDecided = 0x1AFFFFFF, kColListTextDecided = 0xFFF1F1F1;
constexpr uint32_t kColRowBar = 0xFFD76407, kColRowMarker = 0xFFF0F0F0;   // the blue bar, the marker (the template's is clear)
constexpr uint32_t kColListMatPlain = 0x05FFFFFF, kColListDefaultPlain = 0x33FFFFFF;   // a plain option row (the game's DEFAULT)
// the stage grid: the focused cell's blue frame, the chosen cell's yellow corner, the scroll bar (the game's colours)
constexpr uint32_t kColStageFocus = 0xFFFF8C28, kColStageMark = 0xFF00E6FF, kColScrollTrack = 0x0DCCCCCC, kColScrollThumb = 0xFFF0A040;
// Draw order: the GUI sorts its elements by Priority (child order only breaks ties): the model and the grid's panel
// under the cells and rows, pictures and arrows over them, texts on top
constexpr int kPrioPanel = 1, kPrioModel = 1, kPrioFrame = 2, kPrioImage = 3, kPrioText = 4;
// The summary on the right, as the game's: Battle Settings > Other's "Game Settings" (MatchingSetting
// p_BattleSettingProfileGameSetting_) and the Opponent Outfit page's (OtherMatchingFighterSetting c_CostumeSetting), read
// 05/11 (probes g45, g48a). The panel at (1596, 189) drawn at 0.81 (0.9 x 0.9), in its own units below (replay_logic.hpp:
// the items' y). Its title pill (c_title): a UVS_hub shadow (sequence 15, 334 x 36.5) under c_mask (colour scale alpha
// 0.302): a white-to-grey rect (334 x 32, ColorType Horizontal) masked by sequence 14.
// The title at (20, 16), font 24, 0xFFD9D9D9; the names at x 36 (0xFFFFBF65), the values at x 48 (0xFFEEEEEE), font 23,
// region 320 x 36, LeftCenter; the game's UI font (FontSlot 8); the stage picture (248 x 62) at x 50. The title's shadow
// 54000000, 3 at 45, blur 5, strength 2, no glow; the others' shadow CC000000, 0, blur 3, strength 5 and glow 80000000,
// blur 10, strength 4.
constexpr float kSumX = 1596, kSumY = 189, kSumScale = 0.81f;
constexpr float kPillW = 334, kPillH = 32, kPillShadowH = 36.5f;
constexpr float kSumTitleX = 20, kSumTitleY = 16, kSumNameX = 36, kSumLineX = 48, kSumPicX = 50, kSumPicW = 248;
constexpr float kSumTitleRegionW = 430, kSumTitleRegionH = 37, kSumRegionW = 320, kSumRegionH = 36;
constexpr float kFontSumTitle = 24, kFontSumLine = 23;
constexpr int64_t kSumFontSlot = 8;
constexpr int kSumTexts = 25, kSumMaxFighters = 20;   // 3 names + 20 outfit lines + the stage's line + the music's, the page's 6
constexpr float kSumOffScreen = -4000;             // a value while it is measured, before its place is known
constexpr float kSumDrawRegionW = 1200;            // a line's region: wider than any line (never clips what is measured)
constexpr uint32_t kColSumTitle = 0xFFD9D9D9, kColSumName = 0xFFFFBF65, kColSumLine = 0xFFEEEEEE;
// The pill's frames can't be had on a copied Scale9Grid: a copy keeps the BorderRect of the frames it was loaded with
// (setSequenceNo, setPatternNo and giving the sheet again leave it: 124.5 and 86 against the game's 25.5 and 22, log 05/11),
// taller than the pill, so it draws nothing. So the same frames as plain Textures: those frames' top and bottom rows have no
// height (borders 11 / 0 / 11 / 0 on 14, 11 / 0 / 14.5 / 0 on 15), only the middle row's 3 patterns (LC 3, CC 4, RC 5) draw:
// a left end, a middle, a right end, drawn tinted (the frame's own alpha stays, as under the game's mask).
struct PillStrip { uint32_t seq; float h, cap_l, cap_r; };
constexpr PillStrip kPillStrips[2] = { { 15, kPillShadowH, 11, 14.5f }, { 14, kPillH, 11, 11 } };   // the shadow, the pill
constexpr int kPillParts = 6;
constexpr uint32_t kPillAlpha = 0x4D;              // c_mask's colour scale alpha, 0.302
constexpr float kPillGreyLeft = 255, kPillGreyRight = 0x5A;   // the game's rect, left to right
constexpr int64_t kPillSampler = 5;                // the game's Scale9Grids' SamplerType
struct TextLook { uint32_t shadow; float dist, rot, blur, strength; bool glow; uint32_t glow_col; float glow_blur, glow_strength; };
constexpr TextLook kSumTitleLook{ 0x54000000, 3, 45, 5, 2, false, 0x80000000, 10, 4 };
constexpr TextLook kSumLineLook{ 0xCC000000, 0, 0, 3, 5, true, 0x80000000, 10, 4 };
constexpr float kFaceV0 = 0.125f, kFaceVH = 0.75f;   // the portrait's middle band (the cells' pictures are 4:3)
const char* const kRowTemplateAgent = "Resident_Cmn_CertifiedMatch";
const char* const kArrowTemplateAgent = "StartMenuUI";

const SF6CoreAPI* A() { return ui::core(); }
SF6CoreClient* C() { return ui::client(); }

// ---- the hooks' side (any thread) ----
std::atomic<uintptr_t> g_top_addr{ 0 };          // the CFNReplayTop agent while its screen is up (frame)
std::atomic<uintptr_t> g_main_addr{ 0 };         // its main control (frame)
std::atomic<int> g_tab{ -1 };                     // the game's tab as the frame last read it
std::atomic<bool> g_ours{ false };
std::atomic<bool> g_enter{ false }, g_leave{ false };
std::atomic<bool> g_music_open{ false };          // the game's music screen is up (its Back must not reach the tab)
bool g_leaving = false;                           // Back pressed on the grid: the page waits for the key's release
bool g_leaving_next = false;                      // ... the tab-next key: the game's first tab selected then
uint32_t g_wrap_enter_at = 0;                     // tab back on the game's first tab: ours comes up once the game is on Search
uint32_t g_wrap_search_at = 0;                    // ... and its page has had a few frames to replace the first tab's
std::atomic<int> g_refused{ 0 };
std::atomic<int> g_copy_status{ 1000 };
std::atomic<uint32_t> g_swallowed{ 0 };
std::atomic<uint32_t> g_leave_frame{ 0 };
std::atomic<uint32_t> g_wrap_frame{ 0xFFFFFFFFu };   // a press that wraps round the tabs: the game's own move, once (+1: the frame)
std::atomic<bool> g_to_first{ false };          // tab next on Settings: the frame selects the game's first tab (its list does not
                                                // wrap forward from Search, seen 10/11)
thread_local bool tl_skipped = false;

// ---- frame state ----
bool g_init = false, g_ok = false;
bool g_up = false;
uintptr_t g_main_seen = 0;                        // the screen build the label / guide were set up for
bool g_guide_moved = false;
std::vector<std::string> g_hidden;                // agents hidden for our page (names, found again to restore)
std::vector<Fighter> g_cat;
Choices g_choices;
OptionStore g_options;                            // the costume options saved per fighter and outfit, for replays
BgmChoice g_bgm;                                  // the Replay Music row
std::string g_music_hint;                         // the game's own "Character Music" in that language
std::map<int, Choice> g_last_on;                  // per character, the outfit it had before being turned Off
int g_cursor = 0, g_top_row = 0;                  // the focused cell (catalog index), the first grid row shown
// The page's level, as on Battle Settings > Other: the option row (the grid shown dimmed), the grid, a character's page
// (the stage grid, the second option row's, has the same place as the faces grid)
enum Level { kLevelList = 0, kLevelGrid = 1, kLevelPage = 2, kLevelStage = 3 };
std::atomic<int> g_level{ kLevelList };           // the hooks read it
int g_list_row = 0;                               // the focused option row (0 outfits, 1 stage, 2 music)
int g_row = 0;                                    // the character's page's focused row
bool g_options_open = false;                      // the Costume Options menu was up (input waits for its keys' release)
bool g_header_hidden = false;                     // the tab bar's pieces hidden for the character's page
std::string g_desc_game, g_desc_ours;             // the bottom line: the game's text, ours while it shows
bool g_desc_keep = false;                         // the game's text is known (a music screen's own came since)
bool g_copies_shown = false, g_dirty = true;
uint32_t g_drawn_at = 0;
uintptr_t g_model_tex = 0;                        // the model copy given the render target (address)
bool g_model_shown = false;
std::set<uintptr_t> g_styled;
int g_status_logged = 1000;
bool g_place_logged = false;
uint32_t g_enter_frame = 0;
int64_t g_cp_lefttop = -1;                        // via.gui.ControlPoint.LeftTop
bool g_no_arrows_logged = false, g_no_pill_logged = false, g_pill_logged = false;
std::map<int, void*> g_faces;                     // fighter -> portrait TextureResourceHolder (one reference held)
// The stages: the cells after Default (the game's stages, then the stage slots' tiles), each stage's variations (cycled on
// its cell), the pictures (path -> holder), the choice
struct StageVar { std::string key, name, banner, bundle; };
struct StageEntry {
    int id = 0;                                   // the vanilla stage id (a tile's too: the game only ever holds vanilla ids)
    std::string key;                              // "" a stage of the game, else a tile's key (stages.hpp)
    int sort = 0;
    bool named = false; uint8_t guid[16] = {};    // a stage of the game: its name (the game's message, shown by its id)
    std::string name;                             // a tile's name; a stage's name as text (for the variations' label)
    std::string banner;                           // the cell's and the banner's 4:1 picture
};
std::vector<StageEntry> g_stages;
std::map<int, std::vector<StageVar>> g_stage_vars;   // vanilla stage id -> its variations (stages.hpp)
stagelogic::Preview g_stage_preview;             // the variation key's preview on the focused cell (Confirm chooses it)
bool g_stage_slots = false;                       // the stage slots have tiles or variations here
std::map<std::string, void*> g_stage_tex;         // picture path -> TextureResourceHolder (one reference held)
StageChoice g_stage;
int g_stage_cursor = 0, g_stage_top = 0;          // the focused cell (0 = Default), the first grid row shown

// ---- the page's copies, made inside the tab hook (R19) ----
// Plain controls of the tab bar's screen (no UIParts), the game's row and its plain text, the banner's two arrows; the
// summary's title pill: the row's marker, a plain Texture on the same UVS_hub sheet as the game's pill
enum Kind { kText, kRect, kImage, kLine, kRow, kLabel, kLeftArrow, kRightArrow, kPillPart };
struct Copy { std::string name; Kind kind; };

constexpr const char* kCellPrefix = "e_ro_";       // the grid's cells (game_cell.hpp): "e_ro_g?NN"
std::string cname(const char* what, int i) {
    char b[48];
    std::snprintf(b, sizeof b, "e_ro_%s_%02d", what, i);
    return b;
}
// Draw order: Priority (set by the frame), then creation order (each copy becomes its parent's last child)
const std::vector<Copy>& copies() {
    static std::vector<Copy> v;
    if (!v.empty()) return v;
    v.push_back({ "e_ro_gpanel", kRect });         // the grid's panel and its focus tint: under the cells
    v.push_back({ "e_ro_gfocus", kRect });
    // (the grid's cells: the game's, game_cell.hpp, made after this list: make_copies)
    v.push_back({ "e_ro_sfocus", kRect });         // the stage grid: the focus frame (under its cell), cells, banner
    for (int i = 0; i < kSCells; ++i) v.push_back({ cname("simg", i), kImage });
    v.push_back({ "e_ro_sbanner", kImage });
    for (int i = 0; i < kSMarkLeg; ++i) v.push_back({ cname("smk", i), kRect });   // the chosen stage's corner mark, the scroll bar
    v.push_back({ "e_ro_sbarbg", kRect });
    v.push_back({ "e_ro_sbar", kRect });
    v.push_back({ "e_ro_model", kImage });         // the 3D preview: the capture room's render target
    v.push_back({ "e_ro_divider", kLine });
    for (int r = 0; r < kListRows; ++r) v.push_back({ cname("lrow", r), kRow });   // the option rows
    for (int r = 0; r < kRows; ++r) v.push_back({ cname("row", r), kRow });
    for (int r = 0; r < kRows; ++r) { v.push_back({ cname("arl", r), kLeftArrow }); v.push_back({ cname("arr", r), kRightArrow }); }
    v.push_back({ "e_ro_larl", kLeftArrow });      // the Replay Music option row's arrows
    v.push_back({ "e_ro_larr", kRightArrow });
    for (int r = 0; r < kListRows; ++r) v.push_back({ cname("llbl", r), kLabel });
    for (int r = 0; r < kRows; ++r) v.push_back({ cname("rlbl", r), kLabel });
    v.push_back({ "e_ro_name", kText });
    v.push_back({ "e_ro_sname", kText });          // the banner's stage name, the Default cell's label
    v.push_back({ "e_ro_sdeflbl", kText });
    for (int i = 0; i < kPillParts; ++i) v.push_back({ cname("ppart", i), kPillPart });   // the summary on the right: its title pill,
    v.push_back({ "e_ro_sumpic", kImage });        // picture and texts
    v.push_back({ "e_ro_sumtitle", kLabel });
    for (int i = 0; i < kSumTexts; ++i) v.push_back({ cname("sum", i), kLabel });
    for (int i = 0; i < kSumTexts; ++i) v.push_back({ cname("sumv", i), kLabel });   // the moving part of a line too long for its place
    return v;
}

// The main control of the first agent of that name (nullptr: none)
void* agent_main(const char* agent) {
    void* buf[2] = {};
    if (!A()->ui_agents(agent, buf, 2) || !buf[0]) return nullptr;
    return call_obj(static_cast<Obj*>(buf[0]), "get_ControlMain");
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
    void* t_line = bar_bg ? a->ui_find_child(bar_bg, "e_scale9grid_line_00", 0) : nullptr;
    void* item0 = bar ? a->ui_find_child(bar, "c_item_00", 0) : nullptr;
    void* batch = item0 ? a->ui_find_child(item0, "c_batch", 0) : nullptr;
    void* t_image = batch ? a->ui_find_child(batch, "e_texture_new", 0) : nullptr;
    void* rows = agent_main(kRowTemplateAgent);
    void* t_row = rows ? a->ui_find_child(rows, "c_SelectItem_0", 8) : nullptr;
    // the row's labels: its plain text (e_text_detail; its own title text has a tinted style)
    void* t_label = t_row ? a->ui_find_child(t_row, "e_text_detail", 2) : nullptr;
    void* menu = agent_main(kArrowTemplateAgent);
    void* arrow_l = menu ? a->ui_find_child(menu, "p_MouseDecideOperable_L", 8) : nullptr;
    void* arrow_r = menu ? a->ui_find_child(menu, "p_MouseDecideOperable_R", 8) : nullptr;
    void* t_arrow_l = arrow_l ? a->ui_find_child(arrow_l, "e_texture_arrow", 2) : nullptr;
    void* t_arrow_r = arrow_r ? a->ui_find_child(arrow_r, "e_texture_arrow", 2) : nullptr;
    const bool arrows = t_arrow_l && t_arrow_r;
    // the summary's pill: the row's marker (c_bg_m > c_bg > e_focus_bar), a plain Texture drawing the game's UVS_hub sheet
    void* t_part = t_row ? a->ui_find_child(t_row, "e_focus_bar", 3) : nullptr;
    if (!title || !t_text || !t_rect || !t_line || !t_image || !t_row || !t_label) {
        ++g_refused;
        g_copy_status.store(t_row && t_label ? -3000 : -3001);
        return false;
    }
    if (!arrows && !g_no_arrows_logged) { g_no_arrows_logged = true; logf("replay tab: no arrow to copy (%s not up): rows without arrows", kArrowTemplateAgent); }
    if (!t_part && !g_no_pill_logged) { g_no_pill_logged = true; logf("replay tab: the row has no e_focus_bar to copy: a summary without its title pill"); }
    for (const Copy& cp : copies()) {
        if ((cp.kind == kLeftArrow || cp.kind == kRightArrow) && !arrows) continue;
        if (cp.kind == kPillPart && !t_part) continue;
        if (a->ui_find_child(title, cp.name.c_str(), 0)) continue;
        void* tmpl = cp.kind == kText ? t_text : cp.kind == kRect ? t_rect : cp.kind == kLine ? t_line : cp.kind == kRow ? t_row
                   : cp.kind == kLabel ? t_label : cp.kind == kPillPart ? t_part
                   : cp.kind == kLeftArrow ? t_arrow_l : cp.kind == kRightArrow ? t_arrow_r : t_image;
        void* c = nullptr;
        const SF6CoreStatus st = a->ui_clone_control(C(), tmpl, title, cp.name.c_str(), &c);
        g_copy_status.store(st == SF6CORE_OK && !c ? -1000 : static_cast<int>(st));
        if (st != SF6CORE_OK || !c) { ++g_refused; return false; }
        if (cp.kind == kRect) {
            SF6CoreArg m{}; m.type = SF6CORE_ARG_I64; m.v.i = 1;    // via.gui.MaskType.NonTarget (the rectangle was a mask: 2)
            a->obj_invoke(C(), c, "set_MaskType(via.gui.MaskType)", &m, 1, nullptr);
        }
        a->ui_set_visible(C(), c, 0);                           // placed and shown by the frame
    }
    // the grid's cells: the game's (game_cell.hpp, Wael 10/11: the plain rectangles were not the game's look), from this screen's
    // parts: the resident row's background panel (c_bg_m) and darken, the header's title text, the badge's texture
    gamecell::Parts parts;
    parts.shell = a->ui_find_child(t_row, "c_bg_m", 1);
    parts.text = t_text;
    parts.tex = t_image;
    parts.darken = a->ui_find_child(t_row, "e_darken", 1);
    if (!parts.ok()) {
        ++g_refused;
        g_copy_status.store(-3002);
        return false;
    }
    for (int i = 0; i < kCells; ++i) {
        const SF6CoreStatus st = gamecell::make(title, parts, kCellPrefix, i);
        if (st != SF6CORE_OK) { ++g_refused; g_copy_status.store(static_cast<int>(st)); return false; }
    }
    return true;
}

constexpr int kCall = REFRAMEWORK_HOOK_CALL_ORIGINAL, kSkip = REFRAMEWORK_HOOK_SKIP_ORIGINAL;

// The tabs wrap round as the game's do (Wael 10/11): Settings is the last tab, after Search, before Recommended Replays.
//   - Settings, tab next: ours gives way and the frame selects the game's first tab (its list's SetSelectedIndex, a mouse click's
//     way; the game's own next does not wrap forward from Search, seen 10/11: the press is swallowed).
//   - Recommended Replays, tab back: the game's own back runs (to Search, its list then where ours keeps it) and ours comes up.
// One press reaches decide twice (InputTab, then InputTabNext / InputTabBack): the second of a wrapping press lets the game go on.
int decide(void* self, int dir, uint32_t flag) {
    const uintptr_t top = g_top_addr.load(std::memory_order_acquire);
    if (!top || reinterpret_cast<uintptr_t>(self) != top || g_closing.load(std::memory_order_relaxed)) return kCall;
    const bool acts = (flag & kActFlags) != 0;
    if (g_wrap_frame.load() == g_frame) return kCall;   // the same wrapping press seen again: the game's own move
    if (g_ours.load(std::memory_order_acquire)) {
        if (g_music_open.load()) return kSkip;          // the music screen has the keys: this tab stays
        // tab back leaves from the option row; in the grid and on the character's page the tab keys do nothing (the game's)
        if (acts && dir < 0 && g_level.load() == kLevelList) { g_ours.store(false); g_leave.store(true); g_leave_frame.store(g_frame); }
        if (acts && dir > 0 && g_level.load() == kLevelList) {      // tab next: round to the game's first tab (the frame)
            g_ours.store(false);
            g_leave.store(true);
            g_leave_frame.store(g_frame);
            g_to_first.store(true);
        }
        g_swallowed.fetch_add(1);
        return kSkip;                                   // our tab: the game's list stays on Search
    }
    if (g_leave_frame.load() == g_frame) return kSkip;   // the same press seen again (InputTab + InputTabBack / InputTabNext)
    if (dir < 0 && acts && g_tab.load(std::memory_order_acquire) == kFirstTab) {   // tab back on the first tab: round to ours
        bool ok = false;
        try { ok = make_copies(); } catch (...) { ++g_refused; g_copy_status.store(-2000); }
        if (!ok) return kCall;                          // no page to show: the game does its own thing (Search)
        g_wrap_enter_at = g_frame;                      // ours comes up once the game is on Search (the frame)
        g_wrap_search_at = 0;
        g_wrap_frame.store(g_frame);
        return kCall;                                   // the game goes to Search, where ours keeps its list
    }
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
                // the Random outfit (costumes.cpp): never worn as such, a replay set to it wears a draw of the character's pool
                // (costumes::replay_random); the last outfit, its one colour reading Random, its preview Outfit 1
                if (const Json* rn = e.get("random"); rn && rn->kind == Json::Bool && rn->b) {
                    Outfit o;
                    o.costume = cos;
                    o.extra = true;
                    o.random = true;
                    o.name = game(G::Random);
                    o.colors = { 0 };
                    by[fid].outfits.push_back(o);
                    continue;
                }
                if (const Json* ns = e.get("nsfw"); ns && ns->kind == Json::Bool && ns->b && !nsfw_ok) { ++nsfw_left; continue; }
                Outfit o;
                o.costume = cos;
                auto m = mod_of.find({ fid, cos });
                o.extra = true;
                if (m != mod_of.end()) o.mod_id = m->second;            // none known: the file names the slot by its number
                auto l = label_of.find({ fid, cos });
                const Json* nm = e.get("name");
                o.name = l != label_of.end() && !l->second.empty() ? l->second : nm ? nm->as_str() : words::game_arg(G::OutfitN, std::to_string(cos));
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
    // the Color Mod's entries (RW / MC) of every outfit, its labels and order (none without the Color Mod)
    int cm_entries = 0;
    if (colormod::available())
        for (auto& [fid, f] : by)
            for (Outfit& o : f.outfits) {
                for (const colormod::Entry& e : colormod::entries(fid, o.costume)) o.cm.push_back({ e.id, e.label });
                cm_entries += static_cast<int>(o.cm.size());
            }
    g_cat.clear();
    for (auto& [fid, f] : by) {
        f.fid = fid;
        f.name = words::fighter_name(fid);
        f.caption = words::fighter_caption(fid);
        if (f.name.empty()) f.name = trf(W::ReplayFighterN, { std::to_string(fid) });
        sort_outfits(f.outfits);
        if (!f.outfits.empty()) g_cat.push_back(f);
    }
    order_fighters(g_cat);
    logf("replay tab: %zu characters, %d game outfits, %d extra outfits%s, %s", g_cat.size(), game, slots,
         nsfw_left ? (" (" + std::to_string(nsfw_left) + " NSFW hidden)").c_str() : "",
         colormod::available() ? (std::to_string(cm_entries) + " Color Mod entries").c_str() : "no Color Mod entries (no Color Mod, or an older one)");
}

void load_choices() {
    Choices c;
    StageChoice st;
    OptionStore op;
    BgmChoice bgm;
    const std::string text = read_file(kFile);
    if (!text.empty() && !from_json(text, c, &st, &op, &bgm)) logf("replay tab: %s unreadable, starting from Default", kFile);
    g_choices = c;
    g_stage = st;
    g_options = op;
    g_bgm = bgm;
}
void save_choices() {
    if (!write_file(kFile, to_json(g_choices, g_stage, g_options, g_bgm))) logf("replay tab: could not write %s", kFile);
}

// ---- the stages ----
// The stages the page offers, the one place to change when it offers more: for now exactly the list of Battle Settings'
// Stage Settings, the game's own: app.helper.hGUI.GetStageSelectStageIdList(false, false, false), the list
// app.UIPartsStageSelectSimple fills its grid from (21 stages, Training Room included, no Random, read 05/11). Without it,
// the same list rebuilt from the stage table (TableDataManager.GetStages(): the records selectable in Stage Settings,
// not Battle Hub's, the Random entry left out, in SortIndex order: the same 21, same order). Each one's name: the stage
// message table's (StageMessageUserDataDict[id].RecordData.StageName.GUID), shown by its message id.
std::vector<StageEntry> stage_ids_from_table(Obj* tm);
std::vector<StageEntry> stage_list() {
    std::vector<StageEntry> out;
    auto* tm = api().get_managed_singleton("app.TableDataManager");
    if (TD* hg = api().tdb()->find_type("app.helper.hGUI")) {
        if (Method* m = method_of(hg, "GetStageSelectStageIdList", 3)) {
            Obj* ids = call_obj(m, nullptr, { arg_bool(false), arg_bool(false), arg_bool(false) });
            for (int i = 0, n = list_count(ids); i < n; ++i) {
                auto r = call(ids, "get_Item", { arg_i32(i) });   // List<UInt32>: the value itself
                if (r.exception_thrown) { out.clear(); break; }
                StageEntry s;
                s.id = static_cast<int>(r.dword);
                s.sort = i;
                out.push_back(s);
            }
        }
    }
    if (out.empty()) out = stage_ids_from_table(tm);
    Obj* names = tm ? get_obj(tm, "StageMessageUserDataDict") : nullptr;
    TD* ntd = names ? names->get_type_definition() : nullptr;
    Method* m_has = ntd ? method_of(ntd, "ContainsKey", 1) : nullptr;
    Method* m_item = ntd ? method_of(ntd, "get_Item", 1) : nullptr;
    for (StageEntry& e : out) {
        if (!m_has || !m_item) break;
        auto has = call(m_has, names, { arg_i32(e.id) });
        if (has.exception_thrown || !(has.byte & 1)) continue;
        Obj* holder = call_obj(m_item, names, { arg_i32(e.id) });
        Obj* rec = holder ? get_obj(holder, "RecordData") : nullptr;
        Obj* msg = rec ? get_obj(rec, "StageName") : nullptr;
        void* g = msg ? field_ptr(msg, "GUID") : nullptr;
        if (g) { std::memcpy(e.guid, g, 16); e.named = true; }
    }
    for (StageEntry& e : out) e.banner = stage_image_path(e.id);
    // the stage slots (stages.hpp), as Battle Settings' grid shows them: the tiles of a stage the list holds as cells of
    // their own after the game's stages (their order), the variations on their stage's cell (g_stage_vars)
    g_stage_vars.clear();
    std::set<int> held;
    for (const StageEntry& e : out) held.insert(e.id);
    size_t tiles = 0, vars = 0;
    for (const stages::Entry& e : stages::entries()) {
        const int base = static_cast<int>(e.base_id);
        if (e.kind == stages::Entry::Vanilla) {
            for (StageEntry& s : out) if (s.id == base && s.key.empty()) s.name = e.name;
            continue;
        }
        if (!held.count(base) || e.key.empty()) continue;
        if (e.kind == stages::Entry::Tile) {
            StageEntry t;
            t.id = base;
            t.key = e.key;
            t.sort = 100000 + static_cast<int>(tiles++);
            t.name = e.name;
            t.banner = e.banner.empty() ? stage_image_path(base) : e.banner;
            out.push_back(t);
        } else {
            g_stage_vars[base].push_back({ e.key, e.name, e.banner.empty() ? stage_image_path(base) : e.banner, e.bundle });
            ++vars;
        }
    }
    g_stage_slots = tiles + vars > 0;
    if (g_stage_slots) logf("replay tab: stage slots: %zu tile(s), %zu variation(s) on %zu stage(s)", tiles, vars, g_stage_vars.size());
    return out;
}
std::vector<StageEntry> stage_ids_from_table(Obj* tm) {
    std::vector<StageEntry> out;
    Obj* en = tm ? call_obj(tm, "GetStages") : nullptr;
    Obj* it = en ? call_obj(en, "GetEnumerator") : nullptr;
    TD* itd = it ? it->get_type_definition() : nullptr;
    Method* m_next = itd ? method_of(itd, "MoveNext", 0) : nullptr;
    Method* m_cur = itd ? method_of(itd, "get_Current", 0) : nullptr;
    if (!m_next || !m_cur) { logf("replay tab: the stage table could not be read (GetStages)"); return out; }
    it->add_ref();                                  // held while it is walked (no other reference to it)
    struct Release { Obj* o; ~Release() { o->release(); } } hold{ it };
    for (int guard = 0; guard < 500; ++guard) {
        auto r = call(m_next, it);
        if (r.exception_thrown || !(r.byte & 1)) break;
        Obj* rec = call_obj(m_cur, it);
        uint32_t id = 0;
        int32_t sort = 0;
        bool select = false, hub = false;
        if (!rec || !get_u32(rec, "ManageId", id) || !get_bool(rec, "IsStageSelect", select)) continue;
        get_bool(rec, "IsBattleHub", hub);
        get_i32(rec, "SortIndex", sort);
        if (!select || hub || id == 999999) continue;   // app.UIPartsStageSelectSimple.STAGE_ID_RANDOM
        StageEntry e;
        e.id = static_cast<int>(id);
        e.sort = sort;
        out.push_back(e);
    }
    std::stable_sort(out.begin(), out.end(), [](const StageEntry& a, const StageEntry& b) { return a.sort < b.sort; });
    return out;
}
// The pictures: loaded once per session (the core holds them loading; drawn once ready)
void load_stage_pictures() {
    auto load = [](const std::string& path) {
        if (path.empty() || g_stage_tex.count(path)) return;
        void* h = nullptr;
        g_stage_tex[path] = (A()->ui_texture_load(C(), path.c_str(), &h) == SF6CORE_OK) ? h : nullptr;
    };
    load(kStageDefaultImage);
    for (const StageEntry& e : g_stages) load(e.banner);
    for (auto& [id, vars] : g_stage_vars) for (const StageVar& v : vars) load(v.banner);
}
// A stage's variation by key: 1-based position in its cycle, 0 = none (its own look, or not one of its variations)
int variation_index(int id, const std::string& key) {
    auto it = g_stage_vars.find(id);
    if (key.empty() || it == g_stage_vars.end()) return 0;
    for (size_t i = 0; i < it->second.size(); ++i) if (it->second[i].key == key) return static_cast<int>(i) + 1;
    return 0;
}
// The cell of the stage choice (0: Default; a tile: its cell; a variation or the stage's own look: the stage's cell; a
// forced stage no longer listed: Default)
int stage_cell_of_choice() {
    if (!g_stage.forced) return 0;
    int own = 0;
    for (size_t i = 0; i < g_stages.size(); ++i) {
        const StageEntry& s = g_stages[i];
        if (s.id != g_stage.id) continue;
        if (!s.key.empty() && s.key == g_stage.entry) return static_cast<int>(i) + 1;
        if (s.key.empty() && !own) own = static_cast<int>(i) + 1;
    }
    return own;
}
// What every cell shows, from three things only (Wael 05/11: one source per frame): the choice (g_stage), the focused cell
// (g_stage_cursor) and the variation key's preview on it (g_stage_preview, stages_logic.hpp). A stage's own cell shows the
// preview while it is focused, else the chosen variation if it is the chosen cell, else its own look; a tile its picture.
int chosen_variation() {                          // the chosen cell's variation (0: a tile, its own look, Default)
    return g_stage.forced ? variation_index(g_stage.id, g_stage.entry) : 0;
}
int cell_variation(int cell) {
    if (cell <= 0 || cell > static_cast<int>(g_stages.size()) || !g_stages[static_cast<size_t>(cell - 1)].key.empty()) return 0;
    return stagelogic::shown_variation(cell, stage_cell_of_choice(), chosen_variation(), g_stage_preview);
}
struct StageLook { std::string pic; const StageVar* var = nullptr; };
StageLook stage_look(int cell) {
    StageLook l;
    if (cell <= 0 || cell > static_cast<int>(g_stages.size())) { l.pic = kStageDefaultImage; return l; }
    const StageEntry& s = g_stages[static_cast<size_t>(cell - 1)];
    l.pic = s.banner;
    auto it = g_stage_vars.find(s.id);
    const int v = cell_variation(cell);
    if (it != g_stage_vars.end() && v > 0 && v <= static_cast<int>(it->second.size())) {
        l.var = &it->second[static_cast<size_t>(v - 1)];
        l.pic = l.var->banner;
    }
    return l;
}
// The Replay Music row's words, as text (copies show one text then another: never by a message id, see put_line_text): its name
// and Custom ours, by language (words.hpp); the game's values its own, English when a message is not found
std::string bgm_title() { return tr(W::ReplayRowMusic); }
std::string bgm_value() {
    if (g_bgm.mode == BgmMode::Custom) return tr(W::ReplayCustom);
    uint8_t g[16];
    std::string m = parse_guid(bgm_value_guid(g_bgm.mode), g) ? game_message(g) : std::string();
    return m.empty() ? bgm_value_english(g_bgm.mode) : m;
}
// The summary's line: the value, with Custom's number of tracks
std::string bgm_summary_line() {
    return g_bgm.mode == BgmMode::Custom ? bgm_custom_line(bgm_value(), g_bgm.list.size()) : bgm_value();
}
// A cell's name, always as text: the banner's name has one writer mode (a message id there left its id behind under a
// written text, and the core's compare then skipped the next stage's id: Wael 05/11, "moving keeps the old name")
std::string cell_name(int cell) {
    if (cell <= 0 || cell > static_cast<int>(g_stages.size())) return game(G::Default);
    const StageEntry& s = g_stages[static_cast<size_t>(cell - 1)];
    std::string name = s.name;
    if (s.key.empty() && s.named && (name.empty() || name.rfind("ess", 0) == 0)) {
        const std::string m = game_message(s.guid);
        if (!m.empty()) name = m;
    }
    if (name.empty()) name = trf(W::ReplayStageN, { std::to_string(s.id) });
    const StageLook l = stage_look(cell);
    return l.var ? stagelogic::variation_label(name, l.var->name, l.var->bundle) : name;
}
// The stage slots serve the replay menu's own choice (stages::choose(kReplay), never Battle Settings' nor Training's):
// at most the forced stage's tile / variation, nothing for Default (the replay's own stage on its own look). The replay
// itself keeps a vanilla stage id (replay_outfits.cpp writes it; stages::choose_replay_stage says the same to the stage
// slots); the stage slots serve the chosen files when the replay's description is set up. Only with stage slots.
void sync_stage_slots() {
    if (!g_stage_slots) return;
    std::set<int> bases;
    for (const StageEntry& s : g_stages) bases.insert(s.id);
    for (int id : bases) {
        const std::string want = g_stage.forced && g_stage.id == id ? g_stage.entry : std::string();
        if (stages::chosen(stages::kReplay, static_cast<uint32_t>(id)) != want && !stages::choose(stages::kReplay, static_cast<uint32_t>(id), want))
            logf("replay tab: the stage slots refused %s for stage %d", want.c_str(), id);
    }
    stages::choose_replay_stage(g_stage.forced ? g_stage.id : -1);
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
// A colour change only: the room's fighter is recoloured in place, CaptureRoomFighter.UpdateFighterColor(colour), the
// way the game's own page previews a colour (moving over its colour boxes, the room's controller and fighter stay the same
// objects, state 6, its ColorId field untouched; the call recolours that same fighter at once: read 05/11). A colour the
// fighter has no data for (IsExistColorData false) falls back to a new room.
// A change of fighter / outfit: the room given back, and asked again only once it is really gone (no
// type-5 controller left, about 6 frames), after the choice held still for kSettle frames. Asked in the frame it was given
// back (as the game's own SetupCaptureRoom does), the old room's controller was reused while its deactivation was still
// under way: the new fighter loaded right (Luke VisualId 1, folder esf002v01, Outfit 2's meshes esf002_002_*) but the
// page kept showing Outfit 1, the render target no longer refreshed by the room's camera (Wael 05/11; the same outfit
// asked in a fresh room drew right). Shown only once the room's fighter is the one asked (fighter, VisualId = the costume
// number, colour as loaded: true of every outfit seen, game and Slots Loader) and ready (CurrentState 6) for kShowAfter frames;
// nothing meanwhile and whenever anything fails (kTries failures: no model for the rest of the session). Given
// back when the character's page goes (back to the grid, tab back, another tab, the screen closed) and when the character
// is Off: no room nor fighter left behind.
namespace preview {
constexpr int kRoomType = 5;                      // app.worldtour.CaptureRoomObserver.CaptureRoomType.BattleSettingFighter
constexpr int kStateReady = 6;                    // app.worldtour.CaptureRoomFighter.State when drawn in Battle Settings
constexpr uint32_t kSettle = 15, kShowAfter = 8, kTimeout = 600, kPollEvery = 5;
constexpr uint32_t kDrainMax = 300;               // frames a given-back room may take to go before we ask anyway (logged)
constexpr int kTries = 3;
const char* const kRtt = "Product/GUI/menu/ui91000/ui91060/RTT_MatchingFighterSetting.rtex";

// cm: a Color Mod entry's id on colour 1 ("" = the game colour): asked of the Color Mod (scope 1), which dresses this room's
// fighter with exactly it, not with its own Fighter Settings choice (colormod_link.hpp)
struct Want {
    int fid = 0, cos = 0, col = 0;
    std::string cm;
    bool operator==(const Want& o) const { return fid == o.fid && cos == o.cos && col == o.col && cm == o.cm; }
};
bool g_off = false;                               // given up for the session (types missing, or kTries failures)
bool g_ready_types = false;
Method *m_request = nullptr, *m_deactivate = nullptr, *m_activate = nullptr, *m_list_add = nullptr, *m_list_ctor = nullptr;
Method *m_recolor = nullptr, *m_has_color = nullptr;   // CaptureRoomFighter.UpdateFighterColor / IsExistColorData
TD *t_list = nullptr, *t_req = nullptr, *t_ctrl = nullptr, *t_obs = nullptr;
int g_color_now = 0;                              // the colour the room's fighter shows (loaded, then recoloured)
std::string g_cm_now;                             // the Color Mod entry it shows ("" = the game colour)
// What the room's fighter wears, asked of the Color Mod before the game dresses it (its SetupColor): the entry, or the
// game's own colours
void ask_colormod(const Want& w) { colormod::request(SF6CM_SCOPE_PREVIEW, w.fid, w.cos, w.cm); }
Obj* g_rtt = nullptr;                             // RenderTargetTextureResourceHolder, held for the session
Want g_want, g_asked;                             // what the page shows; what the room was asked for
uint32_t g_want_since = 0, g_asked_at = 0, g_ready_since = 0, g_poll_at = 0;
bool g_room = false;                              // a room of ours is asked or up
bool g_draining = false;                          // a room given back, not gone yet: no new request meanwhile
uint32_t g_drain_since = 0;
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
    if (TD* t_f = tdb->find_type("app.worldtour.CaptureRoomFighter")) {   // optional: without them, a colour change reloads
        m_recolor = method_of(t_f, "UpdateFighterColor", 1);
        m_has_color = method_of(t_f, "IsExistColorData", 1);
    }
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
    g_draining = true;
    g_drain_since = g_frame;
    logf("replay tab: 3D preview room given back (%s)", why);
}
// The room given back is gone (no type-5 controller left); polled every kPollEvery frames
bool drained() {
    if (!g_draining) return true;
    if (g_frame - g_poll_at < kPollEvery) return false;
    g_poll_at = g_frame;
    if (!find_controller()) {
        g_draining = false;
        logf("replay tab: 3D preview room gone, %u frames after it was given back", g_frame - g_drain_since);
        return true;
    }
    if (g_frame - g_drain_since > kDrainMax) {
        g_draining = false;
        logf("replay tab: 3D preview: the room given back is still there after %u frames: asked again anyway", kDrainMax);
        return true;
    }
    return false;
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
    ask_colormod(w);                              // before the room dresses its fighter
    const auto r = call(m_request, obs, { arg_i32(kRoomType), arg_obj(list), nullptr, arg_obj(empty) });
    if (empty) empty->release();
    if (r.exception_thrown) { req->release(); list->release(); logf("replay tab: 3D preview: the request threw"); return false; }
    drop_request_objects();
    g_list = list;
    g_req = req;
    g_room = true;
    g_asked = w;
    g_color_now = w.col;
    g_cm_now = w.cm;
    g_asked_at = g_frame;
    g_activated = false;
    g_ready = false;
    g_ctrl = nullptr;
    ++g_requests;
    return true;
}

// The page shows this fighter in this outfit, colour and Color Mod entry (fid 0: nothing)
void want(int fid, int cos, int col, const std::string& cm) {
    Want w{ fid, cos, col, cm };
    if (!(w == g_want)) { g_want = w; g_want_since = g_frame; }
}

// The room's fighter (app.worldtour.CaptureRoomFighter), nullptr while there is none
Obj* room_fighter(Obj* ctrl) {
    Obj* oc = ctrl ? get_obj(ctrl, "ObjectCtrl") : nullptr;
    Obj* mo = oc ? get_obj(oc, "ManageObjects") : nullptr;
    if (list_count(mo) < 1) return nullptr;
    Obj* f = list_item(mo, 0);
    return alive(f) ? f : nullptr;
}
// The fighter recoloured in place (the game's way); false: not possible (no method, no data for that colour)
bool recolor(int col) {
    Obj* f = (m_recolor && m_has_color && g_ctrl && alive(g_ctrl)) ? room_fighter(g_ctrl) : nullptr;
    if (!f) return false;
    auto has = call(m_has_color, f, { arg_i32(col) });
    if (has.exception_thrown || !(has.byte & 1)) return false;
    return !call(m_recolor, f, { arg_i32(col) }).exception_thrown;
}

// The room's fighter: the one asked, and how far it is; `model` = its model's first mesh (for the log)
bool fighter_state(Obj* ctrl, int* fid, int* col, int* vis, int* state, std::string* model) {
    Obj* f = room_fighter(ctrl);
    if (!f) return false;
    get_i32(f, "<FighterId>k__BackingField", *fid);
    get_i32(f, "<ColorId>k__BackingField", *col);
    get_i32(f, "<VisualId>k__BackingField", *vis);
    get_i32(f, "CurrentState", *state);
    model->clear();
    if (Obj* fp = get_obj(f, "FighterFolderPaths"); fp && list_count(fp) > 0) *model = str(list_item(fp, 0));
    return true;
}

// Every frame while the page is up: the room follows the wanted fighter
void frame() {
    if (g_off || !types()) return;
    if (!g_want.fid) { give_back("no fighter"); drained(); return; }
    // a colour change only, the room ready: the same fighter recoloured at once (no reload); otherwise (another fighter or
    // outfit, or a colour that cannot be applied in place) ask again once the choice held still and the room given back
    // is gone
    // (a Color Mod entry: the Color Mod asked first, then the same recolouring makes the game dress the fighter again)
    const bool same_outfit = g_room && g_asked.fid == g_want.fid && g_asked.cos == g_want.cos;
    if (same_outfit && g_ready && (g_color_now != g_want.col || g_cm_now != g_want.cm)) {
        ask_colormod(g_want);
        // the same colour, another Color Mod entry (or the game's 02 again): through colour 01 and back in the same frame,
        // so that the game dresses the fighter again (a fresh SetupColor) and the Color Mod applies the new request
        if (g_color_now == g_want.col) recolor(g_want.col == 0 ? kColorModCarrier : 0);
        if (recolor(g_want.col)) {
            logf("replay tab: 3D preview recoloured in place: fighter %d costume %d colour %d -> %d%s", g_want.fid, g_want.cos, g_color_now,
                 g_want.col, g_want.cm.empty() ? "" : (" (Color Mod " + g_want.cm + ")").c_str());
            g_color_now = g_want.col;
            g_cm_now = g_want.cm;
        } else {
            logf("replay tab: 3D preview: colour %d not applicable in place: a new room", g_want.col);
            g_asked.col = -1;                     // makes the room be asked again below
        }
    }
    const bool reload = !g_room || g_asked.fid != g_want.fid || g_asked.cos != g_want.cos || g_asked.col < 0;
    if (reload && g_frame - g_want_since >= kSettle) {
        if (g_room) give_back("another fighter / outfit");
        if (!drained()) return;
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
    std::string model;
    const bool has = fighter_state(g_ctrl, &fid, &col, &vis, &state, &model);
    const bool now = has && fid == g_asked.fid && vis == g_asked.cos && (col == g_asked.col || g_asked.col < 0) && state == kStateReady &&
                     g_activated;
    if (now && !g_ready) {
        if (!g_ready_since) g_ready_since = g_frame;
        if (g_frame - g_ready_since >= kShowAfter) {
            g_ready = true;
            g_fails = 0;
            logf("replay tab: 3D preview ready: fighter %d visual %d colour %d, model %s, %u frames after the request", fid, vis, col,
                 model.c_str(), g_frame - g_asked_at);
        }
    } else if (!now) {
        g_ready_since = 0;
        if (g_ready) logf("replay tab: 3D preview lost its model (fighter %d state %d)", fid, state);
        g_ready = false;
        if (g_frame - g_asked_at > kTimeout && has) {
            logf("replay tab: 3D preview: no right model %u frames after the request (fighter %d visual %d colour %d state %d, model "
                 "%s)", kTimeout, fid, vis, col, state, model.c_str());
            give_back("model not right");
            if (++g_fails >= kTries) { g_off = true; logf("replay tab: 3D preview off for the session (%d failures)", g_fails); }
        }
    }
}
// The model shows exactly what the page wants
bool showing() {
    return !g_off && g_ready && g_asked.fid == g_want.fid && g_asked.cos == g_want.cos && g_asked.col >= 0 && g_color_now == g_want.col &&
           g_cm_now == g_want.cm;
}
void stop(const char* why) {
    give_back(why);
    colormod::clear(SF6CM_SCOPE_PREVIEW);         // the room's fighter is the Color Mod's again (Battle Settings' preview)
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
Rep k_up, k_down, k_left, k_right, k_esc, k_tab, k_next, k_ok, k_var, k_music;
Method* m_pad_merged = nullptr;
Method* m_pad_button = nullptr;
uint32_t b_up = 1, b_down = 2, b_left = 4, b_right = 8, b_l1 = 256, b_r1 = 512, b_b = 128, b_a = 32;   // via.hid.GamePadButton
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
struct Keys { bool up, down, left, right, esc, tab, next, ok, var, music; };   // esc: Back; tab: the tab-back key; next: the tab-next key; var: the stage variation key; music: T / pad Y
Keys read_keys() {
    Keys k{};
    if (focused()) {
        k.up = vk_down(VK_UP) || sc_down(0x11);
        k.down = vk_down(VK_DOWN) || sc_down(0x1F);
        k.left = vk_down(VK_LEFT) || sc_down(0x1E);
        k.right = vk_down(VK_RIGHT) || sc_down(0x20);
        k.esc = vk_down(VK_ESCAPE);
        k.tab = sc_down(0x10);                     // the tab-back key (Q's place: A on AZERTY)
        k.next = sc_down(0x12);                    // the tab-next key (E's place)
        k.ok = vk_down(VK_RETURN) || sc_down(0x21);     // Enter, F (the game's Confirm)
    }
    const uint32_t b = pad_buttons();
    k.up = k.up || (b & b_up) || (b_sup && (b & b_sup));
    k.down = k.down || (b & b_down) || (b_sdown && (b & b_sdown));
    k.left = k.left || (b & b_left) || (b_sleft && (b & b_sleft));
    k.right = k.right || (b & b_right) || (b_sright && (b & b_sright));
    k.esc = k.esc || (b & b_b);
    k.tab = k.tab || (b & b_l1);
    k.next = k.next || (b & b_r1);
    k.ok = k.ok || (b & b_a);
    k.var = stages::replay_key_down();             // the pad's X / its keyboard key (stages.hpp)
    k.music = stages::replay_music_key_down();     // the pad's Y / its keyboard key
    return k;
}
void prime_keys() {
    const Keys k = read_keys();
    k_up.prime(k.up); k_down.prime(k.down); k_left.prime(k.left); k_right.prime(k.right);
    k_esc.prime(k.esc); k_tab.prime(k.tab); k_next.prime(k.next); k_ok.prime(k.ok); k_var.prime(k.var); k_music.prime(k.music);
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
// The options of the outfit the character's page shows: its slot when the fighter is On and that outfit has options, what is
// saved for it (nullptr: nothing, its defaults)
const rto::Slot* options_slot(const Fighter& f, const Choice& c) { return c.forced ? rto::outfit_slot(f.fid, c.costume) : nullptr; }
const std::vector<std::string>* saved_options(const rto::Slot* s) {
    if (!s) return nullptr;
    auto fi = g_options.find(s->fighter);
    if (fi == g_options.end()) return nullptr;
    auto oi = fi->second.find(s->mod);
    return oi != fi->second.end() ? &oi->second : nullptr;
}
// The options menu changed them (nullptr: Restore defaults): saved here, per outfit; never the player's own
void options_changed(const rto::Slot* s, const std::vector<std::string>* ids) {
    if (!s) return;
    if (ids) g_options[s->fighter][s->mod] = *ids;
    else if (auto fi = g_options.find(s->fighter); fi != g_options.end()) {
        fi->second.erase(s->mod);
        if (fi->second.empty()) g_options.erase(fi);
    }
    save_choices();
    g_dirty = true;
}
// A row's Left / Right on the focused character (Outfit Settings On / Off, Costume, Color)
Choice row_step(const Fighter& f, const Choice& cur, int row, int dir) {
    if (row == 0) {                                // On / Off
        if (cur.forced) { g_last_on[f.fid] = cur; return Choice{}; }
        auto l = g_last_on.find(f.fid);
        if (l != g_last_on.end() && outfit_index(f, l->second) >= 0) return l->second;
        return cycle_outfit(f, Choice{}, 1);
    }
    if (row == 1) {                                // the outfit, its colour back to 01; Default is the Off of row 0, skipped
        Choice n = cycle_outfit(f, cur, dir, false);
        if (!n.forced) n = cycle_outfit(f, n, dir, false);
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
    // the options menu is up: its window has the keys; closed: a key still held (its Back, its Confirm) acts only once released
    if (rto::menu::active()) { g_options_open = true; return; }
    if (g_options_open) { g_options_open = false; prime_keys(); g_dirty = true; return; }
    // the game's music screen likewise (its Back must not leave this tab too)
    if (g_music_open.load()) {
        if (replaymusic::up()) return;
        g_music_open.store(false);
        std::vector<uint32_t> marked;              // Back on the dialog: what it has marked is the list
        if (replaymusic::take_custom_list(marked) && marked != g_bgm.list) {
            g_bgm.list = marked;
            save_choices();
            logf("replay tab: Custom list, %zu track%s", marked.size(), marked.size() == 1 ? "" : "s");
        }
        prime_keys();
        g_dirty = true;
        return;
    }
    const Keys k = read_keys();
    const bool esc = k_esc.press(k.esc), tab = k_tab.press(k.tab), tab_next = k_next.press(k.next), ok = k_ok.press(k.ok);
    const Fighter* f = focused_fighter();
    const int level = g_level.load();
    if (level == kLevelList) {                     // the option rows: Up / Down (wrapping), Confirm into its grid, Back to Search
        if (esc || tab) { g_ours.store(false); g_leaving = true; return; }   // (the page's input comes back once released)
        if (tab_next) { g_ours.store(false); g_leaving = true; g_leaving_next = true; return; }   // round to the game's first tab
        if (k_up.step(k.up)) { g_list_row = (g_list_row + kListRows - 1) % kListRows; g_dirty = true; }
        if (k_down.step(k.down)) { g_list_row = (g_list_row + 1) % kListRows; g_dirty = true; }
        const bool music_press = k_music.press(k.music);
        if (g_list_row == kBgmRow) {               // its value; T / pad Y: the game's Character Music screen, on Custom its dialog alone
            BgmChoice next = g_bgm;                // (the list stays whatever the value)
            if (k_left.step(k.left)) next.mode = bgm_cycle(next.mode, -1);
            if (k_right.step(k.right)) next.mode = bgm_cycle(next.mode, 1);
            if (!(next == g_bgm)) {
                g_bgm = next;
                save_choices();
                g_dirty = true;
                logf("replay tab: music -> %s", bgm_value_english(g_bgm.mode));
            }
            const bool custom = g_bgm.mode == BgmMode::Custom;
            stages::replay_music_hint(custom ? tr(W::ReplayCustomList) : g_music_hint.c_str());
            if (music_press && (custom ? replaymusic::open_custom_list(g_bgm.list, tr(W::ReplayCustomList))
                                       : replaymusic::open_character_music())) { g_music_open.store(true); g_dirty = true; }
            return;
        }
        if (ok && g_list_row == 0 && f) { g_level.store(kLevelGrid); g_dirty = true; }
        if (ok && g_list_row == 1) {
            g_level.store(kLevelStage);
            g_stage_cursor = stage_cell_of_choice();
            g_stage_preview = {};
            g_dirty = true;
        }
        return;
    }
    if (level == kLevelStage) {                    // the stage grid: Confirm picks the focused cell and stays (the game's)
        if (esc) { g_level.store(kLevelList); g_stage_preview = {}; g_dirty = true; return; }   // left: no preview
        const int n = static_cast<int>(g_stages.size()) + 1;
        const int before = g_stage_cursor;
        if (k_up.step(k.up)) g_stage_cursor = grid_move(g_stage_cursor, 0, -1, n, kSCols);
        if (k_down.step(k.down)) g_stage_cursor = grid_move(g_stage_cursor, 0, 1, n, kSCols);
        if (k_left.step(k.left)) g_stage_cursor = grid_move(g_stage_cursor, -1, 0, n, kSCols);
        if (k_right.step(k.right)) g_stage_cursor = grid_move(g_stage_cursor, 1, 0, n, kSCols);
        if (g_stage_cursor != before) {
            g_stage_preview = stagelogic::preview_follow(g_stage_preview, g_stage_cursor);   // left: the cell as it was
            g_dirty = true;
        }
        // the variation key (the pad's X): one press previews the focused stage's next variation (its own look, then each
        // one, wrapping), Confirm chooses it, as Battle Settings' grid; its "<key> Stage Variation" in the guide bar
        const bool var_press = k_var.press(k.var);
        if (g_stage_cursor > 0 && g_stage_cursor <= static_cast<int>(g_stages.size())) {
            const StageEntry& s = g_stages[static_cast<size_t>(g_stage_cursor - 1)];
            auto it = g_stage_vars.find(s.id);
            if (s.key.empty() && it != g_stage_vars.end() && !it->second.empty()) {
                stages::replay_variation_hint();
                if (var_press) {
                    g_stage_preview = stagelogic::preview_next(g_stage_cursor, stage_cell_of_choice(), chosen_variation(), g_stage_preview,
                                                               static_cast<int>(it->second.size()));
                    g_dirty = true;
                }
            }
        }
        if (ok) {
            StageChoice next;
            if (g_stage_cursor > 0 && g_stage_cursor <= static_cast<int>(g_stages.size())) {
                const StageEntry& s = g_stages[static_cast<size_t>(g_stage_cursor - 1)];
                next.forced = true;
                next.id = s.id;
                if (!s.key.empty()) next.entry = s.key;                         // a tile
                else if (const StageVar* v = stage_look(g_stage_cursor).var) next.entry = v->key;   // the variation shown
            }
            g_stage_preview = {};                  // chosen: the cell now rests on it
            if (!(next == g_stage)) {
                g_stage = next;
                save_choices();
                sync_stage_slots();
                logf("replay tab: stage -> %s%s", next.forced ? std::to_string(next.id).c_str() : "Default",
                     next.entry.empty() ? "" : (" (" + next.entry + ")").c_str());
            }
            g_dirty = true;
        }
        return;
    }
    if (level == kLevelPage) {                     // the character's page (the tab keys: nothing, as the game's)
        if (esc || !f) { g_level.store(kLevelGrid); g_dirty = true; return; }   // back to the grid
        const Choice cur = choice_of(*f);
        // the Costume Options: the fighter On and its outfit with options: the guide bar's entry, one press of its key (R on a
        // keyboard, the pad's X) opens the same menu as Edit Character's
        const bool var_press = k_var.press(k.var);
        if (const rto::Slot* slot = options_slot(*f, cur)) {
            stages::replay_guide_hint(tr(W::OptionsTitle));
            if (var_press && rto::menu::open_replay(slot, saved_options(slot), options_changed)) { g_options_open = true; return; }
        }
        const int rows = cur.forced ? kRows : 1;   // Off: the rows below are disabled, the focus stays on the first
        if (k_up.step(k.up)) { g_row = (g_row + rows - 1) % rows; g_dirty = true; }
        if (k_down.step(k.down)) { g_row = (g_row + 1) % rows; g_dirty = true; }
        Choice next = cur;
        if (k_left.step(k.left)) next = row_step(*f, next, g_row, -1);
        if (k_right.step(k.right)) next = row_step(*f, next, g_row, 1);
        apply_choice(*f, cur, next);
        return;
    }
    // the grid: Esc back to the option row, the tab keys nothing (as the game's)
    if (esc) { g_level.store(kLevelList); g_dirty = true; return; }
    const int n = static_cast<int>(g_cat.size());
    const int before = g_cursor;
    if (k_up.step(k.up)) g_cursor = grid_move(g_cursor, 0, -1, n, kCols);
    if (k_down.step(k.down)) g_cursor = grid_move(g_cursor, 0, 1, n, kCols);
    if (k_left.step(k.left)) g_cursor = grid_move(g_cursor, -1, 0, n, kCols);
    if (k_right.step(k.right)) g_cursor = grid_move(g_cursor, 1, 0, n, kCols);
    if (g_cursor != before) g_dirty = true;
    if (ok && f) { g_level.store(kLevelPage); g_row = 0; g_dirty = true; }
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
    if (void* t = A()->ui_find_child(item5, "e_text_menu", 1)) A()->ui_set_text(C(), t, tr(W::ReplayTab));
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
void hide_page(bool quiet, bool more = false) {
    if (!more) g_hidden.clear();
    const size_t had = g_hidden.size();
    TD* td = api().tdb()->find_type("app.UIAgent");
    Obj* list = find_components(td);
    for (int i = 0, n = list_count(list); i < n; ++i) {
        Obj* ag = list_item(list, i);
        Obj* go = ag ? call_obj(ag, "get_GameObject") : nullptr;
        const std::string nm = go ? str(call_obj(go, "get_Name")) : "";
        if (nm.rfind("CFN", 0) != 0 || nm == "CFNTop" || nm == kTop) continue;
        if (more && std::find(g_hidden.begin(), g_hidden.end(), nm) != g_hidden.end()) continue;
        Obj* cm = call_obj(ag, "get_ControlMain");
        if (!actual_visible(cm)) continue;
        A()->ui_set_block_input(C(), ag, 1);
        A()->ui_set_force_invisible(C(), cm, 1);
        g_hidden.push_back(nm);
    }
    std::string names;
    for (size_t i = more ? had : 0; i < g_hidden.size(); ++i) names += (names.empty() ? "" : ", ") + g_hidden[i];
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
    for (int i = 0; i < kCells; ++i)                     // the grid's cells (their pictures go with their shell)
        for (gamecell::What w : { gamecell::kShell, gamecell::kName, gamecell::kOn, gamecell::kDarken })
            set_vis(A()->ui_find_child(title, gamecell::name_of(kCellPrefix, w, i).c_str(), 0), false);
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
void put_image(void* title, const std::string& name, int fid, float x, float y, float w, float h, bool on, uint32_t col = kColText) {
    void* c = copy_of(title, name);
    if (!c) return;
    style_box(c, kPrioImage);
    auto it = g_faces.find(fid);
    void* holder = it != g_faces.end() ? it->second : nullptr;
    const bool ready = on && holder && A()->ui_texture_ready(holder) == 1;
    if (ready) {
        A()->ui_set_texture_rect(C(), c, holder, 0.0f, kFaceV0, 1.0f, kFaceVH);
        A()->ui_set_color(C(), c, col);
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
    style_box(c, kPrioModel);
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

// Through the core: an enum value (via.gui.ControlPoint and PageAlignment: LeftCenter 4, CenterCenter 5, read 05/11)
constexpr int64_t kLeftCenter = 4, kCenterCenter = 5;
void set_enum(void* c, const char* sig, int64_t v) {
    if (!c) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_I64; a.v.i = v;
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void set_float(void* c, const char* sig, double v) {
    if (!c) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_F64; a.v.f = v;
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
// A value type (via.Color, via.Size, a border...) passed by address, as the core's own set_Color / set_Position
void set_value(void* c, const char* sig, void* value) {
    if (!c) return;
    SF6CoreArg a{}; a.type = SF6CORE_ARG_PTR; a.v.p = value;
    A()->obj_invoke(C(), c, sig, &a, 1, nullptr);
}
void place(void* c, float x, float y) { if (c) A()->ui_set_position(C(), c, x, y, 0); }
void resize(void* c, float w, float h) { if (c) A()->ui_set_size(C(), c, w, h); }

// A text of a row: placed by `point` (LeftCenter, CenterCenter), aligned the same in its region (w x the row's height)
void style_row_text(void* e, int64_t point, float w, float font) {
    if (!e) return;
    set_enum(e, "set_ControlPoint(via.gui.ControlPoint)", point);
    set_enum(e, "set_PageAlignment(via.gui.PageAlignment)", point);
    A()->ui_set_region_size(C(), e, w, kRowH);
    A()->ui_set_font_size(C(), e, font, font);
    set_enum(e, "set_AutoRegionFit(System.Boolean)", 0);   // never squeezed: every text of a row fits by itself (plugin/locale: max)
}
// A row (a copy of the game's row), once: the size of the game's rows (w x 58, its material w x 280, its shadow
// w + 20 x 77 at -8, -6), the template's own extras hidden (colour block, frame, mouse area, its tinted title: the label
// is a copy of its plain text), its value centred on (524, 29) (`value`: false, no value: the option row)
void style_row(void* row, float w, bool value, float value_x = kValueX, float value_w = kValueW) {
    if (!row || g_styled.count(reinterpret_cast<uintptr_t>(row))) return;
    g_styled.insert(reinterpret_cast<uintptr_t>(row));
    set_priority(row, kPrioFrame);
    auto el = [row](const char* n) { return A()->ui_find_cached(row, n); };
    for (const char* n : { "c_color", "e_scale9grid_frame", "e_hit_mouse", "e_text_title" }) set_vis(el(n), false);
    resize(el("e_scale9grid_shadow"), w + 20, kRowH + 19);
    for (const char* n : { "e_scale9grid_mask", "e_scale9grid_default", "e_darken" }) resize(el(n), w, kRowH);
    resize(el("e_mat_bg"), w, kRowMatH);
    void* v = el("e_text_detail");
    if (!value) { set_vis(v, false); return; }
    style_row_text(v, kCenterCenter, value_w, kFontValue);
    place(v, value_x, kMidY);
}
// A row's look by its own PlayState (set on the row and its two background panels: SELECT, DEFAULT)
void row_state(void* row, const char* st) {
    set_state(row, st);
    set_state(A()->ui_find_cached(row, "c_bg_m"), st);
    set_state(A()->ui_find_cached(row, "c_bg"), st);
}
// The darken over a row: none on the selected row, 20 % on the others, 50 % on a disabled one (the game's)
void row_darken(void* row, bool sel, bool disabled) {
    void* d = A()->ui_find_cached(row, "e_darken");
    if (!d) return;
    if (!sel) A()->ui_set_color(C(), d, disabled ? kColDarkenOff : kColDarkenRow);
    set_vis(d, !sel);
}
// A label copy (over its row): its left middle at (23, 29) of the row
void put_label(void* title, const std::string& name, const std::string& text, float x, float y, uint32_t col, bool on) {
    void* l = copy_of(title, name);
    if (!l) return;
    if (on) {
        if (g_styled.insert(reinterpret_cast<uintptr_t>(l)).second) {
            style_row_text(l, kLeftCenter, kLabelW, kFontLabel);
            set_priority(l, kPrioText);
        }
        place(l, x + kLabelX, y + kMidY);
        A()->ui_set_text(C(), l, text.c_str());
        A()->ui_set_color(C(), l, col);
    }
    show(l, on);
}
// The character's page's row r: SELECT on the focused row, DEFAULT; texts and arrows coloured as the game's (the value
// under the row's darken, the label and arrows over it); a disabled row darker, without arrows
void put_row(void* title, int r, const std::string& label, const std::string& value, bool on, bool sel, bool disabled) {
    void* row = copy_of(title, cname("row", r));
    if (!row) return;
    const float y = kRowY + r * kRowStep;
    const uint32_t over = sel ? kColRowTextSel : disabled ? kColRowTextOff : kColRowTextDim;
    if (on) {
        style_row(row, kRowW, true);
        row_state(row, sel ? "SELECT" : "DEFAULT");
        place(row, kRowX, y);
        if (void* t = A()->ui_find_cached(row, "e_text_detail")) {
            A()->ui_set_text(C(), t, value.c_str());
            A()->ui_set_color(C(), t, sel ? kColRowTextSel : kColRowText);
        }
        row_darken(row, sel, disabled);
    }
    show(row, on);
    put_label(title, cname("rlbl", r), label, kRowX, y, over, on);
    for (int s = 0; s < 2; ++s) {
        void* a = copy_of(title, cname(s ? "arr" : "arl", r));
        if (!a) continue;
        const bool vis = on && !disabled;
        if (vis) {
            if (g_styled.insert(reinterpret_cast<uintptr_t>(a)).second) set_priority(a, kPrioImage);
            place(a, kRowX + (s ? kArrowR : kArrowL), y + kMidY);   // its centre (the template's ControlPoint)
            A()->ui_set_color(C(), a, over);
        }
        show(a, vis);
    }
}

// The option rows, the divider and the grid's panel (on: the option rows' level or one of their grids is up; row: the
// focused / entered row; entered: its grid has the focus). A row's look, as the game's Other list: the focused row selected (SELECT: blue bar,
// light material, its marker); the row whose grid has the focus "entered" (the game's p_GroupItem stays SELECT with its
// blue bar hidden, its material dark, its bevel shown, its small marker, its text light); the other rows plain (DEFAULT,
// darkened 20 %). Every piece written each time: the copies keep what was written to them. The Replay Music row: its value and
// arrows as the game's p_Spin_2; `side`: false while it has the focus, nothing on the right then (the game's).
void render_list(void* title, bool on, int focus, bool entered, bool side) {
    for (int r = 0; r < kListRows; ++r) {
        void* row = copy_of(title, cname("lrow", r));
        const float y = kListY + r * kListStep;
        const bool sel = r == focus && !entered, ent = r == focus && entered;
        if (row) {
            if (on) {
                style_row(row, kListW, r == kBgmRow, kListValueX, kListValueW);
                row_state(row, sel || ent ? "SELECT" : "DEFAULT");
                place(row, kListX, y);
                auto el = [row](const char* n) { return A()->ui_find_cached(row, n); };
                if (void* bar = el("e_rect_bar")) { A()->ui_set_color(C(), bar, kColRowBar); set_vis(bar, sel); }
                if (void* mk = el("e_focus_bar")) { A()->ui_set_color(C(), mk, kColRowMarker); set_vis(mk, sel || ent); }
                if (void* m = el("e_mat_bg")) A()->ui_set_color(C(), m, ent ? kColListMatDecided : sel ? kColText : kColListMatPlain);
                if (void* d = el("e_scale9grid_default")) {
                    A()->ui_set_color(C(), d, ent ? kColListDefaultDecided : kColListDefaultPlain);
                    set_vis(d, !sel);
                }
                row_darken(row, sel || ent, false);
                if (r == kBgmRow)
                    if (void* t = el("e_text_detail")) {
                        A()->ui_set_text(C(), t, bgm_value().c_str());
                        A()->ui_set_color(C(), t, sel ? kColRowTextSel : kColRowText);
                    }
            }
            show(row, on);
        }
        const uint32_t label_col = ent ? kColListTextDecided : sel ? kColRowTextSel : kColRowTextDim;
        put_label(title, cname("llbl", r), list_label(r), kListX, y, label_col, on);
        if (r == kBgmRow)
            for (int s = 0; s < 2; ++s) {
                void* a = copy_of(title, s ? "e_ro_larr" : "e_ro_larl");
                if (!a) continue;
                if (on) {
                    if (g_styled.insert(reinterpret_cast<uintptr_t>(a)).second) set_priority(a, kPrioImage);
                    place(a, kListX + (s ? kListArrowR : kListArrowL), y + kMidY);   // its centre (the template's ControlPoint)
                    A()->ui_set_color(C(), a, label_col);
                }
                show(a, on);
            }
    }
    // the divider: the header's line turned 90 degrees about its left end (the game's divider is that same line turned)
    if (void* l = copy_of(title, "e_ro_divider")) {
        if (on) {
            if (g_styled.insert(reinterpret_cast<uintptr_t>(l)).second) {
                set_enum(l, "set_ControlPoint(via.gui.ControlPoint)", g_cp_lefttop >= 0 ? g_cp_lefttop : 0);
                set_priority(l, kPrioFrame);
                float rot[4] = { 0.0f, 0.0f, 90.0f, 0.0f };   // via.vec3, passed by address as the core's own set_Position
                SF6CoreArg a{}; a.type = SF6CORE_ARG_PTR; a.v.p = rot;
                A()->obj_invoke(C(), l, "set_Rotation(via.vec3)", &a, 1, nullptr);
            }
            place(l, kLineX, kLineY);
            resize(l, kLineLen, kLineW);
            A()->ui_set_color(C(), l, kColLine);
        }
        show(l, on && side);
    }
    put_box(title, "e_ro_gpanel", kBoxL, kBoxT, kBoxR - kBoxL, kBoxB - kBoxT, kColPanel, on && side, kPrioPanel);
    put_box(title, "e_ro_gfocus", kBoxL, kBoxT, kBoxR - kBoxL, kBoxB - kBoxT, kColPanelFocus, on && entered, kPrioPanel);
}

// A centred text copy (the banner's stage name, the Default cell's label): the title's display font, its middle on (x, y);
// `guid`: the game's message shown by its id (the player's language), otherwise `text`
void put_center_text(void* title, const char* name, const char* text, const uint8_t* guid, float x, float y, float w, float font,
                     uint32_t col, bool on) {
    void* c = copy_of(title, name);
    if (!c) return;
    if (on) {
        if (g_styled.insert(reinterpret_cast<uintptr_t>(c)).second) {
            style_row_text(c, kCenterCenter, w, font);
            set_priority(c, kPrioText);
        }
        if (guid) A()->ui_set_message_id(C(), c, guid);
        else A()->ui_set_text(C(), c, text);
        A()->ui_set_color(C(), c, col);
        place(c, x, y);
    }
    show(c, on);
}
// The banner's name, once per copy, as the game's (kStageName*): its font, no shrinking, no shadow, its black glow. Value
// types go by address (via.Color, via.Size), as the core's own set_Position.
std::set<uintptr_t> g_shadowed;
void banner_name_style(void* c) {
    if (!c || !g_shadowed.insert(reinterpret_cast<uintptr_t>(c)).second) return;
    set_enum(c, "set_FontSlot(via.gui.FontSlot)", kStageNameFontSlot);
    float raster[2] = { kStageNameRaster, kStageNameRaster };
    set_value(c, "set_RasterSize(via.Size)", raster);
    set_enum(c, "set_AutoRegionFit(System.Boolean)", 0);
    set_enum(c, "set_Bold(System.Boolean)", 0);
    set_enum(c, "set_ShadowEnable(System.Boolean)", 0);
    set_enum(c, "set_GlowEnable(System.Boolean)", 1);
    uint32_t glow = kColStageNameGlow;
    set_value(c, "set_GlowColor(via.Color)", &glow);
    float blur[2] = { kStageNameGlowBlur, kStageNameGlowBlur };
    set_value(c, "set_GlowBlurSize(via.Size)", blur);
    set_float(c, "set_GlowStrength(System.Single)", kStageNameGlow);
}
// A stage cell's (or the banner's) picture: the whole texture (4:1, the game's own thumbnail and banner, a stage slot's own)
void put_stage_picture(void* title, const std::string& name, const std::string& path, float x, float y, float w, float h, uint32_t col,
                       bool on) {
    void* c = copy_of(title, name);
    if (!c) return;
    style_box(c, kPrioImage);
    auto it = g_stage_tex.find(path);
    void* holder = it != g_stage_tex.end() ? it->second : nullptr;
    const bool ready = on && holder && A()->ui_texture_ready(holder) == 1;
    if (ready) {
        A()->ui_set_texture_rect(C(), c, holder, 0.0f, 0.0f, 1.0f, 1.0f);
        A()->ui_set_color(C(), c, col);
        A()->ui_set_position(C(), c, x, y, 0);
        A()->ui_set_size(C(), c, w, h);
    } else if (on && holder) {
        g_dirty = true;                            // drawn again once its picture has loaded
    }
    show(c, ready);
}

// The stage page (on: shown; grid: its grid has the focus): the banner of the focused cell (of the chosen one while the
// option row has the focus), the grid of 3 columns (Default first, then the stages), dimmed behind the option row, the
// focused cell framed, the chosen one marked in its corner, the scroll bar
void render_stage(void* title, bool on, bool grid) {
    const int n = on ? static_cast<int>(g_stages.size()) + 1 : 0;
    const int chosen = stage_cell_of_choice();
    const int shown = grid ? g_stage_cursor : chosen;
    const int rows = (n + kSCols - 1) / kSCols;
    if (on) g_stage_top = window_top(g_stage_top, shown / kSCols, rows, kSVisRows);
    for (int i = 0; i < kSCells; ++i) {
        const int idx = g_stage_top * kSCols + i;
        const bool cell = idx < n;
        const float x = kSGridX + (i % kSCols) * kSStepX, y = kSGridY + (i / kSCols) * kSStepY;
        put_stage_picture(title, cname("simg", i), cell ? stage_look(idx).pic : std::string(), x, y, kSCellW, kSCellH, grid ? kColText : kColTextDim, cell);
    }
    // the Default cell's label, over its picture while it is in view
    const int def_vis = 0 - g_stage_top * kSCols;
    const bool def_on = on && def_vis >= 0 && def_vis < kSCells;
    put_center_text(title, "e_ro_sdeflbl", tr(W::ReplayStageDefault), nullptr, kSGridX + kSCellW / 2, kSGridY + kSCellH / 2, kSCellW, kFontStageCell,
                    grid && g_stage_cursor == 0 ? kColText : kColTextDim, def_on);
    // the focus frame (behind the focused cell), the chosen cell's corner mark
    const int fv = g_stage_cursor - g_stage_top * kSCols;
    const bool f_on = on && grid && fv >= 0 && fv < kSCells;
    put_box(title, "e_ro_sfocus", kSGridX + (fv % kSCols) * kSStepX - 4, kSGridY + (fv / kSCols) * kSStepY - 4, kSCellW + 8, kSCellH + 8,
            kColStageFocus, f_on, kPrioFrame);
    const int cv = chosen - g_stage_top * kSCols;
    const bool c_on = on && cv >= 0 && cv < kSCells;
    for (int i = 0; i < kSMarkLeg; ++i)            // its rows: the full leg on the cell's top edge, one unit shorter each row
        put_box(title, cname("smk", i), kSGridX + (cv % kSCols) * kSStepX, kSGridY + (cv / kSCols) * kSStepY + static_cast<float>(i),
                static_cast<float>(kSMarkLeg - i), 1.0f, kColStageMark, c_on, kPrioText);
    // the banner: the focused cell's picture and name (the chosen cell's while the option row has the focus), every frame
    // from cell_name / stage_look: one source, one writer mode (text)
    put_stage_picture(title, "e_ro_sbanner", stage_look(shown).pic, kBannerX, kBannerY, kBannerW, kBannerH, kColText, on);
    // its name: never squeezed. All written in one go, before the frame draws: right from the first frame.
    const stagelogic::BannerName bname = stagelogic::banner_name(cell_name(shown));
    void* sname = copy_of(title, "e_ro_sname");
    if (sname && on) namescroll::release_unless(sname, bname.text);      // another name: its place back first
    const bool name_moves = sname && on && namescroll::busy(sname);      // the same name, wider than the banner: its place is the scroller's
    if (name_moves) show(sname, true);
    else put_center_text(title, "e_ro_sname", bname.text.c_str(), nullptr, bname.right ? kStageNameX + stagelogic::kNameRightFromCentre : kStageNameX,
                         kStageNameY, kStageNameW, kFontStageName, kColStageName, on);
    if (void* c = on && !name_moves ? sname : nullptr) {
        banner_name_style(c);
        const int64_t point = bname.right ? kRightCenter : kCenterCenter;
        set_enum(c, "set_ControlPoint(via.gui.ControlPoint)", point);
        set_enum(c, "set_PageAlignment(via.gui.PageAlignment)", point);
        A()->ui_set_region_size(C(), c, bname.right ? kStageNameRightW : kStageNameW, kStageNameH);
    }
    // the scroll bar: its track and its thumb (rows shown / rows)
    put_box(title, "e_ro_sbarbg", kSBarX, kSBarY, kSBarW, kSBarH, kColScrollTrack, on && rows > kSVisRows, kPrioFrame);
    const float thumb = rows > kSVisRows ? kSBarH * kSVisRows / rows : kSBarH;
    const float ty = rows > kSVisRows ? kSBarY + (kSBarH - thumb) * g_stage_top / (rows - kSVisRows) : kSBarY;
    put_box(title, "e_ro_sbar", kSBarX, ty, kSBarW, thumb, kColScrollThumb, on && rows > kSVisRows, kPrioImage);
}

// The character's page: the tab bar gives way to the name (the bar's items, guides, lower line and background, the
// title), as on the game's page; put back otherwise
const char* const kHeaderParts[] = { "c_item_00", "c_item_01", "c_item_02", "c_item_03", "c_item_04", "c_item_05",
                                     "p_InputGuide_L_s_dUITabL", "p_InputGuide_R_s_dUITabR", "e_scale9grid_line_01",
                                     "e_scale9grid_bg" };
void header(Obj* main, bool hide) {
    if (!main || hide == g_header_hidden) return;
    g_header_hidden = hide;
    for (const char* n : kHeaderParts)
        if (void* c = find(main, n)) A()->ui_set_force_invisible(C(), c, hide ? 1 : 0);
    void* title = find(main, "c_title");
    if (void* t = title ? A()->ui_find_child(title, "e_text_title", 0) : nullptr) A()->ui_set_force_invisible(C(), t, hide ? 1 : 0);
}

// The bottom line: the shown InputGuide's p_ScrollText_0 > e_text (the description of the page)
Obj* desc_text() {
    void* buf[4] = {};
    const uint32_t n = A()->ui_agents("InputGuide", buf, 4);
    for (uint32_t i = 0; i < n && i < 4; ++i) {
        Obj* cm = call_obj(static_cast<Obj*>(buf[i]), "get_ControlMain");
        if (!actual_visible(cm)) continue;
        void* st = A()->ui_find_child(cm, "p_ScrollText_0", 4);
        if (void* t = st ? A()->ui_find_child(st, "e_text", 1) : nullptr) return static_cast<Obj*>(t);
    }
    return nullptr;
}
void set_desc(const char* text) {
    Obj* t = desc_text();
    if (!t) return;
    const std::string cur = str(call_obj(t, "get_Message"));
    if (cur == text) return;
    if (cur != g_desc_ours && !g_desc_keep) g_desc_game = cur;   // the game's own text, put back when we leave
    g_desc_ours = text;
    A()->ui_set_text(C(), t, text);
}
// Only if it still shows ours: the game writes its own when it changes tab
void restore_desc() {
    if (g_desc_ours.empty()) return;
    if (Obj* t = desc_text(); t && str(call_obj(t, "get_Message")) == g_desc_ours) A()->ui_set_text(C(), t, g_desc_game.c_str());
    g_desc_ours.clear();
}

// ---- the summary on the right (the game's own on Battle Settings > Other and on the Opponent Outfit page) ----
float sum_x(float x) { return kSumX + x * kSumScale; }
float sum_y(float y) { return kSumY + y * kSumScale; }
// The pill: three plain Textures per strip on the sheet's frames (kPillStrips), at the panel's scale, the pill's own grey
// across its 3 pieces as the game's rect across its width (their corner colours: EachVertex)
uint32_t pill_color(float x) {
    const uint32_t g = static_cast<uint32_t>(kPillGreyLeft + (kPillGreyRight - kPillGreyLeft) * x / kPillW + 0.5f);
    return (kPillAlpha << 24) | (g << 16) | (g << 8) | g;
}
void put_pill(void* title, bool on) {
    for (int s = 0; s < 2; ++s) {
        const PillStrip& st = kPillStrips[s];
        const float xs[3] = { 0, st.cap_l, kPillW - st.cap_r };
        const float ws[3] = { st.cap_l, kPillW - st.cap_l - st.cap_r, st.cap_r };
        for (int k = 0; k < 3; ++k) {
            void* c = copy_of(title, cname("ppart", s * 3 + k));
            if (!c) continue;
            if (on) {
                if (g_styled.insert(reinterpret_cast<uintptr_t>(c)).second) {
                    set_priority(c, s ? kPrioImage : kPrioFrame);     // the shadow under the pill
                    if (g_cp_lefttop >= 0) set_enum(c, "set_ControlPoint(via.gui.ControlPoint)", g_cp_lefttop);
                    set_enum(c, "set_UVSequenceNo(System.UInt32)", st.seq);
                    set_enum(c, "set_UVPatternNo(System.UInt32)", 3 + k);
                    set_enum(c, "set_SamplerType", kPillSampler);
                    uint32_t l = s ? pill_color(xs[k]) : kColText, r = s ? pill_color(xs[k] + ws[k]) : kColText;
                    for (const char* m : { "set_Color", "set_ColorLeftTop", "set_ColorLeftBottom" }) set_value(c, m, &l);
                    for (const char* m : { "set_ColorRightTop", "set_ColorRightBottom" }) set_value(c, m, &r);
                }
                place(c, sum_x(xs[k]), sum_y(0));
                resize(c, ws[k] * kSumScale, st.h * kSumScale);
            }
            show(c, on);
        }
    }
}
// Once, a few frames after the pill is first shown: what the game makes of it (the pill's middle piece)
void log_pill(void* title) {
    if (g_pill_logged || g_frame - g_enter_frame < 10) return;
    g_pill_logged = true;
    void* c = copy_of(title, cname("ppart", 4));
    if (!c) { logf("replay tab: summary pill: no piece (the row has no e_focus_bar to copy)"); return; }
    Obj* o = static_cast<Obj*>(c);
    auto num = [o](const char* m) { auto r = call(o, m); return r.exception_thrown ? -1 : static_cast<int>(r.dword); };
    logf("replay tab: summary pill: middle piece shown %d, sequence %d, pattern %d", num("get_ActualVisible") & 1, num("get_UVSequenceNo"),
         num("get_UVPatternNo"));
}
// A text of the summary, once per copy: the game's place (its left middle), alignment, region, font, shadow and glow,
// at the panel's scale. A Text has no Scale (only Panels do: ui_set_scale finds no get_Scale on it), so every size is
// written scaled: the font (23 x 0.81 = 18.6), the title's region (a line's is wider than any line: kSumDrawRegionW), the
// shadow's distance and blur, the glow's blur.
void style_sum_text(void* t, bool head) {
    const TextLook& k = head ? kSumTitleLook : kSumLineLook;
    const float s = kSumScale;
    set_enum(t, "set_ControlPoint(via.gui.ControlPoint)", kLeftCenter);
    set_enum(t, "set_PageAlignment(via.gui.PageAlignment)", kLeftCenter);
    A()->ui_set_region_size(C(), t, head ? kSumTitleRegionW * s : kSumDrawRegionW, (head ? kSumTitleRegionH : kSumRegionH) * s);
    const float font = (head ? kFontSumTitle : kFontSumLine) * s;
    A()->ui_set_font_size(C(), t, font, font);
    set_enum(t, "set_FontSlot(via.gui.FontSlot)", kSumFontSlot);
    set_enum(t, "set_AutoRegionFit(System.Boolean)", 0);   // never squeezed (the game's is on): a long line scrolls (name_scroll.cpp)
    set_enum(t, "set_Bold(System.Boolean)", 0);
    set_enum(t, "set_ShadowEnable(System.Boolean)", 1);
    uint32_t shadow = k.shadow;
    set_value(t, "set_ShadowColor(via.Color)", &shadow);
    set_float(t, "set_ShadowDistance(System.Single)", k.dist * s);
    set_float(t, "set_ShadowRotation(System.Single)", k.rot);
    float blur[2] = { k.blur * s, k.blur * s };
    set_value(t, "set_ShadowBlurSize(via.Size)", blur);
    set_float(t, "set_ShadowStrength(System.Single)", k.strength);
    set_enum(t, "set_GlowEnable(System.Boolean)", k.glow ? 1 : 0);
    if (k.glow) {
        uint32_t glow = k.glow_col;
        set_value(t, "set_GlowColor(via.Color)", &glow);
        float gb[2] = { k.glow_blur * s, k.glow_blur * s };
        set_value(t, "set_GlowBlurSize(via.Size)", gb);
        set_float(t, "set_GlowStrength(System.Single)", k.glow_strength);
    }
    set_priority(t, kPrioText);
}
// A line: `text` at the game's size, never squeezed: wider than the game's region (320 x 0.81), it scrolls inside it (name_scroll.cpp).
// Always written as text, never by a message id: a copy shows one line then another, and a text written over a message id leaves the id
// behind, so the core's compare would skip the same id later and keep the old text (the stage agent's finding, 05/11).
void put_sum_text(void* title, const std::string& name, bool head, const std::string& text, float x, float y, uint32_t col, bool on) {
    void* t = copy_of(title, name);
    if (!t) return;
    if (on) {
        if (g_styled.insert(reinterpret_cast<uintptr_t>(t)).second) style_sum_text(t, head);
        if (!head) namescroll::release_unless(t, text);   // another line: its place back first
        if (head || !namescroll::busy(t, x, y)) {
            A()->ui_set_text(C(), t, text.c_str());
            place(t, x, y);
        }
        A()->ui_set_color(C(), t, col);
    }
    show(t, on);
}
// The summary: the title pill and its heading, then the items (names, lines, a picture: text = its path) where
// summary_layout puts them. `fixed`: the start of a line's text that stays when the line is too long for its place ("Luke : ").
struct SumItem { SumKind kind; std::string text; std::string fixed; };
float g_sum_window[kSumTexts] = {};                // each line's moving part: its window, 0 when it shows none
// A line. It stays one text, as any line, while it fits. Too long, and with a fixed part: the fixed part in that text, and the rest, in a second
// text (sumv) that starts where the rest starts in the whole line (both widths measured, scroll_logic.hpp split_place), in a window from there to
// the place's end; only that one moves. Until both widths are known, the whole text in its place, clipped at the place's end: the same letters
// where they will be. Arabic stays whole: the order of a split line there isn't known (the line's start is on its right), so it moves as one.
void put_sum_line(void* title, int i, const SumItem& it, float x, float y, uint32_t col) {
    void* t = copy_of(title, cname("sum", i));
    void* v = copy_of(title, cname("sumv", i));
    if (!t) return;
    if (g_styled.insert(reinterpret_cast<uintptr_t>(t)).second) style_sum_text(t, false);
    const float place_w = kSumRegionW * kSumScale;
    const bool can_split = v && !it.fixed.empty() && it.fixed.size() < it.text.size() && std::strcmp(words::lang(), "ar") != 0;
    const std::string moving = can_split ? it.text.substr(it.fixed.size()) : std::string();
    const float whole = can_split ? namescroll::known_width(t, it.text) : -1;
    const bool wide = whole > 0 && scroll::overflows(whole, place_w);
    const float value = wide ? namescroll::known_width(v, moving) : -1;
    const scroll::SplitPlace sp = value > 0 ? scroll::split_place(whole, value, place_w) : scroll::SplitPlace{};
    if ((can_split && whole < 0) || (wide && value < 0)) g_dirty = true;       // the next step comes with those widths
    const std::string& in_t = sp.ok ? it.fixed : it.text;
    namescroll::release_unless(t, in_t);               // another text: its place back first
    if (!namescroll::busy(t, x, y)) {
        A()->ui_set_text(C(), t, in_t.c_str());
        place(t, x, y);
    }
    A()->ui_set_color(C(), t, col);
    show(t, true);
    if (!v) return;
    const bool value_up = wide && (value < 0 || sp.ok);
    g_sum_window[i] = value_up ? (sp.ok ? sp.window : place_w) : 0;
    if (value_up) {
        if (g_styled.insert(reinterpret_cast<uintptr_t>(v)).second) style_sum_text(v, false);
        const float vx = sp.ok ? x + sp.offset : kSumOffScreen;
        namescroll::release_unless(v, moving);
        if (!namescroll::busy(v, vx, y)) {
            A()->ui_set_text(C(), v, moving.c_str());
            place(v, vx, y);
        }
        A()->ui_set_color(C(), v, col);
    }
    show(v, value_up);
}
void render_summary(void* title, bool on, const std::string& heading, const std::vector<SumItem>& items) {
    put_pill(title, on);
    if (on) log_pill(title);
    put_sum_text(title, "e_ro_sumtitle", true, heading, sum_x(kSumTitleX), sum_y(kSumTitleY), kColSumTitle, on);
    std::vector<SumKind> kinds;
    for (const SumItem& it : items) kinds.push_back(it.kind);
    const std::vector<float> ys = summary_layout(kinds);
    int t = 0;
    std::string pic;
    float pic_y = 0;
    for (size_t i = 0; on && i < items.size(); ++i) {
        const SumItem& it = items[i];
        if (it.kind == SumKind::Picture) { pic = it.text; pic_y = ys[i]; continue; }
        if (t >= kSumTexts) continue;
        const bool name = it.kind == SumKind::Name;
        put_sum_line(title, t++, it, sum_x(name ? kSumNameX : kSumLineX), sum_y(ys[i]), name ? kColSumName : kColSumLine);
    }
    for (; t < kSumTexts; ++t) {
        put_sum_text(title, cname("sum", t), false, std::string(), 0, 0, 0, false);
        g_sum_window[t] = 0;
        if (void* v = copy_of(title, cname("sumv", t))) show(v, false);
    }
    put_stage_picture(title, "e_ro_sumpic", pic, sum_x(kSumPicX), sum_y(pic_y), kSumPicW * kSumScale, kSumPicH * kSumScale, kColText,
                      on && !pic.empty());
}
// The chosen stage as the summary names it, always as text: a tile, its name; a stage of the game, its name (the stage
// slots' one, else the game's message as text, as cell_name); with a variation, the banner's one name line; Default (or a
// stage no longer listed), the Default cell's picture and "Default"
void stage_summary(std::string& pic, std::string& name) {
    const int cell = stage_cell_of_choice();
    if (cell <= 0 || cell > static_cast<int>(g_stages.size())) { pic = kStageDefaultImage; name = game(G::Default); return; }
    const StageEntry& e = g_stages[static_cast<size_t>(cell - 1)];
    std::string stage = e.name;
    if (e.key.empty() && e.named && (stage.empty() || stage.rfind("ess", 0) == 0))
        if (std::string m = game_message(e.guid); !m.empty()) stage = m;
    if (stage.empty()) stage = trf(W::ReplayStageN, { std::to_string(e.id) });
    pic = e.banner;
    name = stage;
    if (!e.key.empty()) return;
    if (const int v = variation_index(e.id, g_stage.entry)) {
        const StageVar& var = g_stage_vars[e.id][static_cast<size_t>(v - 1)];
        pic = var.banner;
        name = stagelogic::variation_label(stage, var.name, var.bundle);   // the banner's one name line
    }
}
// The option rows' levels: "Replay Settings": Replay Outfit Settings (a line per character set to On, or Default), Stage
// Settings (the stage's picture and name), Replay Music (its value)
std::vector<SumItem> grid_summary() {
    std::vector<SumItem> v;
    v.push_back({ SumKind::Name, list_label(0) });
    for (const LineParts& l : summary_parts(g_cat, g_choices, kSumMaxFighters)) v.push_back({ SumKind::Line, l.whole(), l.fixed });
    v.push_back({ SumKind::Name, list_label(1) });
    std::string pic, name;
    stage_summary(pic, name);
    v.push_back({ SumKind::Picture, pic });
    v.push_back({ SumKind::Line, name });
    v.push_back({ SumKind::Name, bgm_title() });
    v.push_back({ SumKind::Line, bgm_summary_line() });
    return v;
}

// The grid (on: the option row or the grid is up; grid: the grid has the focus: the focused portrait light; otherwise
// every portrait dimmed, none focused, as the game's grid behind its option row)
// The grid's cells (game_cell.hpp), found again when the screen is built again or one is missing
std::vector<gamecell::Cell> g_cells;
void* g_cells_title = nullptr;
const std::vector<gamecell::Cell>& grid_cells(void* title) {
    bool whole = title == g_cells_title && g_cells.size() == static_cast<size_t>(kCells);
    if (whole) for (const gamecell::Cell& c : g_cells) if (!c.all()) { whole = false; break; }
    if (!whole) {
        g_cells.clear();
        for (int i = 0; i < kCells; ++i) g_cells.push_back(gamecell::find(title, kCellPrefix, i));
        g_cells_title = title;
    }
    return g_cells;
}
void render_grid(void* title, bool on, bool grid) {
    const int n = on ? static_cast<int>(g_cat.size()) : 0;
    if (on) g_top_row = window_top(g_top_row, n ? g_cursor / kCols : 0, (n + kCols - 1) / kCols, kVisRows);
    const auto& cells = grid_cells(title);
    const std::string on_word = game(G::On);
    for (int i = 0; i < kCells; ++i) {
        const gamecell::Cell& c = cells[static_cast<size_t>(i)];
        if (!c.all()) continue;
        const int idx = g_top_row * kCols + i;
        const bool cell = idx < n;
        const bool foc = grid && idx == g_cursor;
        const float x = kGridX + (i % kCols) * kStepX, y = kGridY + (i / kCols) * kStepY;
        if (g_styled.insert(reinterpret_cast<uintptr_t>(c.get(gamecell::kShell))).second)   // once per screen build
            gamecell::lay_out(c, x, y, 0, 0, kPrioFrame, on_word.c_str());
        for (gamecell::What w : { gamecell::kShell, gamecell::kName, gamecell::kDarken }) show(c.get(w), cell);
        if (!cell) { show(c.get(gamecell::kOn), false); continue; }
        const Fighter& f = g_cat[static_cast<size_t>(idx)];
        gamecell::look(c, foc ? gamecell::Look::Focus : grid ? gamecell::Look::Plain : gamecell::Look::Dim);
        A()->ui_set_text(C(), c.get(gamecell::kName), (f.caption.empty() ? grid_caption(f.name) : f.caption).c_str());
        auto it = g_faces.find(f.fid);
        void* holder = it != g_faces.end() ? it->second : nullptr;
        if (holder && A()->ui_texture_ready(holder) == 1) gamecell::picture(c, holder);
        else {
            gamecell::no_picture(c);
            if (holder) g_dirty = true;                // drawn again once its picture has loaded
        }
        show(c.get(gamecell::kOn), choice_of(f).forced);
    }
}

// The character's page (f: its character; nullptr: not up)
void render_page(void* title, const Fighter* f) {
    const bool on = f != nullptr;
    const Choice c = on ? choice_of(*f) : Choice{};
    // Off: the disabled rows show what On would bring back (the game keeps a disabled row's value)
    const Choice shown = on && !c.forced ? row_step(*f, c, 0, 1) : c;
    const int oi = on ? outfit_index(*f, shown) : -1;
    put_text(title, "e_ro_name", on ? (f->caption.empty() ? grid_caption(f->name) : f->caption) : "", kNameX, kNameY, kFontName, kColName, on);
    const std::string labels[kRows] = { game(G::OutfitSettings), game(G::Costume), game(G::Color) };
    const std::string values[kRows] = { game(c.forced ? G::On : G::Off), oi >= 0 ? words::cut_units(f->outfits[static_cast<size_t>(oi)].name, kValueMax) : "-",
                                        shown.forced && oi >= 0 ? choice_color_label(f->outfits[static_cast<size_t>(oi)], shown) : "-" };
    const rto::Slot* slot = on ? options_slot(*f, c) : nullptr;
    const std::vector<std::string>* saved = saved_options(slot);
    for (int r = 0; r < kRows; ++r) put_row(title, r, labels[r], values[r], on, r == g_row, r > 0 && !c.forced);
    // the summary: "Outfit Settings (Replay)", the three settings (Off: the outfit and colour are the replay's own)
    if (on) {
        const std::string costume = c.forced && oi >= 0 ? f->outfits[static_cast<size_t>(oi)].name : game(G::Default);
        const std::string color = c.forced && oi >= 0 ? choice_color_label(f->outfits[static_cast<size_t>(oi)], c) : game(G::Default);
        std::vector<SumItem> items = { { SumKind::Name, labels[0] }, { SumKind::Line, values[0] },
                                       { SumKind::Name, labels[1] }, { SumKind::Line, costume },
                                       { SumKind::Name, labels[2] }, { SumKind::Line, color } };
        // one line per option not at its default (nothing when all are)
        if (slot) {
            const std::vector<std::string> lines = rto::option_lines(rto::manifest(), *slot, rto::forced_options(rto::manifest(), *slot, saved));
            if (!lines.empty()) items.push_back({ SumKind::Name, tr(W::OptionsTitle) });
            for (size_t i = 0; i < lines.size() && static_cast<int>(items.size()) < kSumTexts; ++i)
                items.push_back({ SumKind::Line, lines[i] });
        }
        render_summary(title, true, tr(W::ReplaySumPage), items);
    }
    // the preview wears this choice live, not the player's own (rto.hpp); the page left, or Off: back to what it was
    if (on && c.forced) rto::replay_preview(slot, saved);
    else rto::replay_preview_end();
    // Off: no model at all, the room given back; On: the model once it shows the choice, nothing meanwhile. Random: Outfit 1,
    // as the game previews it
    const bool forced = on && c.forced;
    const bool rnd = forced && oi >= 0 && f->outfits[static_cast<size_t>(oi)].random;
    preview::want(forced ? f->fid : 0, forced && !rnd ? c.costume : 0, forced && !rnd ? c.color : 0, forced && !rnd ? c.colormod : std::string());
    const bool model = forced && preview::showing();
    g_model_shown = model;
    put_model(title, model);
}

void render(Obj* main) {
    void* title = find(main, "c_title");
    if (!title) return;
    if (g_music_open.load()) {                     // the game's music screen: nothing of this page behind it
        hide_copies(main);
        header(main, true);
        restore_desc();
        g_desc_keep = true;
        return;
    }
    if (!g_dirty && g_copies_shown && g_frame - g_drawn_at < 20) return;
    g_dirty = false;
    g_drawn_at = g_frame;
    const int level = g_level.load();
    const Fighter* f = level == kLevelPage ? focused_fighter() : nullptr;
    // the right side: the stage page with the second option row focused or its grid up, the faces grid otherwise
    const bool stage = level == kLevelStage || (level == kLevelList && g_list_row == 1);
    const bool bgm = level == kLevelList && g_list_row == kBgmRow;   // nothing on the right, as the game's value rows
    const int row = level == kLevelStage ? 1 : level == kLevelGrid ? 0 : g_list_row;
    header(main, f != nullptr);
    set_desc(tr(f ? W::ReplayDescPage : stage ? W::ReplayDescStage : bgm ? W::ReplayDescMusic : W::ReplayDescGrid));
    render_list(title, f == nullptr, row, level == kLevelGrid || level == kLevelStage, !bgm);
    render_grid(title, f == nullptr && !stage && !bgm, level == kLevelGrid);
    render_stage(title, f == nullptr && stage, level == kLevelStage);
    render_page(title, f);
    if (!f) render_summary(title, true, tr(W::ReplaySumGrid), grid_summary());   // the page draws its own
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
    g_place_logged = false;
    g_level.store(kLevelList);
    g_list_row = 0;                               // the first option row, as the game's tab opens
    g_dirty = true;
    g_pill_logged = false;
    build_catalog();                              // each time: the NSFW setting may have changed
    load_faces();
    load_choices();
    g_music_hint = replaymusic::guide_text(stagelogic::kCharaMusicKeyId);
    if (g_music_hint.empty()) g_music_hint = game(G::CharaMusic);
    g_bgm.list = bgm_keep_owned(g_bgm.list, replaymusic::owned_tracks());   // kept in the file until a change is saved
    g_stages = stage_list();                      // each time: the stage list may grow (stage_list())
    load_stage_pictures();
    sync_stage_slots();                           // the stage slots' replay choice: the file's (edited by hand, another PC)
    g_stage_preview = {};
    hide_page(false);
    prime_keys();
    g_cursor = std::max(0, std::min(g_cursor, static_cast<int>(g_cat.size()) - 1));
    void* title = find(main, "c_title");
    logf("replay tab: entered (%zu forced in the file, stage %s; %zu stages; rows %d, arrows %d)", g_choices.size(),
         g_stage.forced ? std::to_string(g_stage.id).c_str() : "Default", g_stages.size(),
         title && copy_of(title, cname("row", 0)) ? 1 : 0, title && copy_of(title, cname("arl", 0)) ? 1 : 0);
}

void leave(Obj* main, const char* why) {
    rto::menu::replay_close(why);
    rto::replay_preview_end();
    g_options_open = false;
    g_music_open.store(false);
    replaymusic::forget();
    g_desc_keep = false;
    preview::stop(why);
    restore_page();
    hide_copies(main);
    header(main, false);
    restore_desc();
    if (main) set_state(find(main, "c_item_04"), "SELECT");
    if (main) set_state(find(main, "c_item_05"), "DEFAULT");
    g_ours.store(false);
    g_level.store(kLevelList);
    logf("replay tab: left (%s)", why);
}

void down() {                                      // the screen went: nothing of ours left behind
    g_leaving = false;
    rto::menu::replay_close("the Replays screen closed");
    rto::replay_preview_end();
    g_options_open = false;
    g_music_open.store(false);
    replaymusic::forget();
    g_desc_keep = false;
    preview::stop("the Replays screen closed");
    if (g_ours.load() || !g_hidden.empty()) leave(nullptr, "the Replays screen closed");
    restore_desc();                                // the guide outlives the screen (resident)
    g_header_hidden = false;                       // the tab bar went with the screen
    g_enter.store(false);
    g_leave.store(false);
    g_top_addr.store(0);
    g_main_addr.store(0);
    g_tab.store(-1);
    g_up = false;
    g_main_seen = 0;
    g_copies_shown = false;
    g_styled.clear();
    g_shadowed.clear();
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
        b_up = bit("LUp", 1); b_down = bit("LDown", 2); b_left = bit("LLeft", 4); b_right = bit("LRight", 8);
        b_l1 = bit("LTrigTop", 256); b_r1 = bit("RTrigTop", 512); b_b = bit("RRight", 128); b_a = bit("RDown", 32);
        b_sup = bit("EmuLup", 0); b_sdown = bit("EmuLdown", 0); b_sleft = bit("EmuLleft", 0); b_sright = bit("EmuLright", 0);
    }
    if (TD* cp = api().tdb()->find_type("via.gui.ControlPoint"))
        if (auto* fld = cp->find_field("LeftTop")) g_cp_lefttop = fld->get_data<int32_t>(nullptr);
    g_ok = true;
    logf("replay tab: on (hooks InputTab %d, InputTabNext %d, InputTabBack %d; pad bits %u %u %u %u a %u b %u l1 %u; LeftTop %lld)",
         h1, h2, h3, b_up, b_down, b_left, b_right, b_a, b_b, b_l1, static_cast<long long>(g_cp_lefttop));
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
    if (!g_up) {
        logf("replay tab: Replays screen up");
        colormod::clear(SF6CM_SCOPE_REPLAY);      // a replay played from here is over (the Color Mod drops them too)
        rto::replay_wear_clear();                 // its costume options too: the player's own choices again
        replaymusic::arm_replay(nullptr, {});     // its Custom list too
    }
    g_up = true;
    g_top_addr.store(reinterpret_cast<uintptr_t>(agent), std::memory_order_release);
    g_main_addr.store(reinterpret_cast<uintptr_t>(main), std::memory_order_release);
    if (g_main_seen != reinterpret_cast<uintptr_t>(main)) {   // a new build of the screen
        g_main_seen = reinterpret_cast<uintptr_t>(main);
        g_guide_moved = false;
        g_styled.clear();
    g_shadowed.clear();
        g_copies_shown = false;
        g_dirty = true;
        g_model_tex = 0;
        g_header_hidden = false;                  // a new tab bar: nothing of it hidden
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
        if (g_wrap_enter_at) {                        // tab back on the first tab: Search reached, its page settled for 6 frames
            if (sel == kSearch && !g_wrap_search_at) g_wrap_search_at = g_frame;
            if (g_wrap_search_at && g_frame - g_wrap_search_at >= 6) {
                g_wrap_enter_at = g_wrap_search_at = 0;
                g_ours.store(true);
                g_enter.store(true);
            } else if (g_frame - g_wrap_enter_at > 60) {
                g_wrap_enter_at = g_wrap_search_at = 0;
                logf("replay tab: tab back on the first tab: the game did not reach Search, Settings not opened");
            }
        }
    }
    if (g_enter.exchange(false) && g_ours.load()) enter(main);
    if (g_leave.exchange(false)) leave(main, "tab back");
    // tab next on Settings: the game's first tab selected, then the screen's flow told the tab changed (its InputDelegates'
    // EventTabChanged: what switches the page; the list's selection alone moved the highlight and left Search's page, seen 10/11)
    if (g_to_first.exchange(false)) {
        Obj* ti = get_obj(agent, "_TabItem");
        Obj* dg = get_obj(agent, "<InputDelegates>k__BackingField");
        if (ti && dg) {
            call(ti, "SetSelectedIndex", { arg_i32(kFirstTab), arg_bool(false) });
            call(dg, "EventTabChanged");
            logf("replay tab: tab next on Settings: the game's first tab selected, its flow told");
        } else logf("replay tab: tab next on Settings: no tab list / input delegates: Search stays");
    }
    if (g_leaving) {
        hide_copies(main);
        const Keys k = read_keys();
        if (!k.esc && !k.tab && !k.next) {
            g_leaving = false;
            leave(main, g_leaving_next ? "tab next" : "back key");
            if (g_leaving_next) { g_leaving_next = false; g_to_first.store(true); }
        }
    }
    tab_bar(main);
    if (g_ours.load()) {
        if (g_frame % 30 == 0) hide_page(true, true);   // a page up after the entry (not up yet then, or the game's own switch)
        input();
        render(main);                             // sets the wanted model
        if (void* title = find(main, "c_title")) {     // long names scroll inside their place (name_scroll.cpp)
            for (int i = 0; i < kSumTexts; ++i) {
                namescroll::drive(copy_of(title, cname("sum", i)), kSumRegionW * kSumScale, 10);
                const float window = g_sum_window[i];
                namescroll::drive(copy_of(title, cname("sumv", i)), window > 0 ? window : kSumRegionW * kSumScale, window > 0 ? 1 : 10);
            }
            namescroll::drive(copy_of(title, "e_ro_sname"), stagelogic::kNameMaxW);
        }
        preview::frame();
        if (preview::showing() != g_model_shown) g_dirty = true;
    } else {
        hide_copies(main);
    }
}

}  // namespace

void frame() {
    replaymusic::frame();                         // its hooks' check, whatever the screen: they must be in before a replay
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
