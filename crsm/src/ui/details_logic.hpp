// ui/details_logic — the drill details dialog (the Manager page's Confirm on the Drill row), game-free: the slot pages
// and the texts of the list, the description text, the wrapped lines of that text found from the game's own baselines,
// the window of lines shown and the scroll bar's thumb, the hold-repeat of the keys. ui/drill_details.cpp drives the
// game's dialog with it; tests/ui_bench.cpp checks it.
//
// Validated in game with a Lua demo (Wael 04/11): the game wraps the full text once
// (AutoWrap + ForceAutoWrap, AutoRegionFit 0, PageAlignment LeftTop, region 930 wide); the next frame
// via.gui.Text.getCharBaseLine(g) gives each glyph's baseline, so the lines start where the baseline jumps; then
// set_Message(the text from line k's start to line k + 18's start) shows a window that the game wraps the same way.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ui::details {

using Tr = std::function<std::string(const std::string& key, const std::vector<std::string>& args)>;   // ui::T

constexpr int kWindowLines = 18;               // lines shown (Wael: one more than the box's 17; the text and the bar
                                               // then end at the left box's bottom)
constexpr float kLineHeight = 32.5f;           // px at 1080p (font 23, line space -2)
constexpr int kSlotsPerPage = 10;              // the list's rows c_item1..c_item10 (c_item0: the header)
constexpr float kDetailWidth = 930.0f;         // the description's region width (the game's lines end at that margin)
constexpr float kBarGap = 30.0f;               // the scroll bar: that far right of the region
constexpr float kBarWidth = 8.0f;
constexpr float kBarTopOffset = 6.0f;          // under the region's top, as the demo placed it
constexpr float kThumbMin = 40.0f;
constexpr int kRepeatDelay = 12;               // a held key: one step at the press, then one every kRepeatEvery frames
constexpr int kRepeatEvery = 3;                // once it has been held kRepeatDelay frames
constexpr size_t kDescMaxBytes = 6000;         // the description read (a UTF-8 boundary)
constexpr size_t kDescMaxUnits = 1000;         // an edited description: the Community Worker's limit (UTF-16 units)

// ---- slot pages ------------------------------------------------------------------------------------------------------
int page_count(int saved);                     // max(1, ceil(saved / kSlotsPerPage))
int clamp_page(int page, int saved);           // into 0 .. page_count - 1
struct Range { int first = 0, last = 0; };     // the saved slots [first, last) shown on a page
Range page_range(int saved, int page);

// ---- texts -------------------------------------------------------------------------------------------------------------
// The list's header: rsm.details_page "PAGE %d/%d", page counted from 0 here and from 1 on screen, in capitals (ASCII
// letters; the locale's own texts are already written so).
std::string header_text(const Tr& T, int page, int pages);
// The top-left box's second line: rsm.details_saved "%d SLOTS SAVED" (rsm.details_saved_one "1 SLOT SAVED" for a single
// slot), in capitals as header_text.
std::string saved_text(const Tr& T, int saved);
// A slot row: "<slot_label> - <name> - <frames>F" ("<slot_label> - <frames>F" without a name), slot_label being
// rsm.details_slot "Slot %d". Wider than max_em (logic::text_em; <= 0: no limit): the name cut with "…", or dropped
// when less than 3 em of it would stay; still wider: the whole row cut.
std::string row_text(const std::string& slot_label, const std::string& name, int64_t frames, double max_em);
// A drill the Community imported from drillcodes.com: its author label "<author> (drillcodes)" (the server's drillcodes.js
// authorLabel: the author cut to 19 characters + the fixed " (drillcodes)"), written into info.author by a Community
// download (with info.source "community"). True for such an author (the suffix in any case, spaces after it tolerated;
// "(drillcodes)" alone too); *bare (optional) = the author without it (one line, trimmed). Read at display time: the drills
// downloaded before show right too (Wael 04/11: "Akuma - Chris_F - DrillCodes").
bool drillcodes_author(const std::string& author, std::string* bare = nullptr);
// The author as the dialog names it: clean_line, the " (drillcodes)" suffix dropped
std::string shown_author(const std::string& author);
// info.source as shown: "community" -> rsm.source_community, "drillcodes.com" / "drillcodes" as it is (a site),
// none -> rsm.source_local ("Local file"), anything else as written (one line). An author with the " (drillcodes)" suffix
// makes it "drillcodes.com" whatever the source.
std::string source_label(const Tr& T, const std::string& source, const std::string& author = "");
// Where a pack came from, as the top-left box names it: info.source "community" (any case) = Community,
// "drillcodes.com" / "drillcodes" = DrillCodes, none or anything else = Local (the player's own drill). The two-argument
// form: an author with the " (drillcodes)" suffix = DrillCodes whatever the source (a Community import of a drill code).
enum class Origin { Local, Community, DrillCodes };
Origin origin(const std::string& source);
Origin origin(const std::string& source, const std::string& author);
// The top-left box's source: rsm.source_local_short ("Local"), rsm.source_community ("Community"), "DrillCodes" (a name,
// not translated); origin(source, author).
std::string short_source(const Tr& T, const std::string& source, const std::string& author = "");
// The author the top-left box names: info.author when there is one (shown_author: its " (drillcodes)" dropped); else, for
// a Local drill, own_name (the name the Community shows for the player's shares, "" when unknown); else "".
std::string frame_author(const std::string& author, const std::string& own_name, const std::string& source);
// The top-left box's first line: "<Character> - <Author> - <Source>" (the parts that are empty left out, each one line).
// Wider than max_em (logic::text_em; <= 0: no limit): the author cut with "…", or dropped when less than 3 em of it would
// stay; still wider: the whole line cut.
std::string frame_text(const std::string& character, const std::string& author, const std::string& source_label, double max_em);
// A free text made safe for a game text, its paragraphs kept: the first max_bytes (UTF-8 boundary), \r and the other
// control characters dropped, tabs as spaces, '<' / '>' and '[' / ']' as '(' / ')' (a '<' starts the game's markup; its
// texts draw no square bracket: "[x]" showed "x", 04/11), runs of spaces as one, each
// line trimmed, at most one blank line in a row, no blank line first or last.
std::string clean_text(const std::string& raw, size_t max_bytes);
std::string clean_line(const std::string& raw);   // the same on one line (line breaks as spaces)
// ---- the description's light Markdown (Wael 04/11; info.description holds the source, docs/PACK_FORMAT.md) -------------
// Per line of a clean_text text (its '<' / '>' already '(' / ')': only this converter writes game tags):
//   "# " at the line's start = a heading: the rest of the line light violet;   "**text**" = magenta;
//   {violet}text{/} light violet, {magenta} magenta, {purple} dark purple, {blue} the game's blue highlight; an unknown
//   {word}, and a {/} that closes nothing, stay as typed ({/} after a {colour} span a nested one closed early is that
//   span's own closer: dropped, one per such span); "- " stays (the bullet glyph is not verified in the font).
// One span at a time: a span opened inside another closes it first; a span still open at the line's end is closed there;
// an empty span is not written. The only tags written (each verified in game; an unknown or malformed tag crashed the
// game's text renderer): <COLOR preset="FGM_Cmn_2"> (light violet, the Frame Meter page's headings), FGM_Cmn_1
// (bright pink-magenta), FGM_Cmn_0 (dark purple), each closed by </COLOR>, and <HLBGN Arg0 = "3"> ... <HLEND Arg0 = "3">
// (blue).
std::string markdown(const std::string& clean);
// The same text without the markers (the Manager's help line, the Community dialog's description area): the heading's
// "# " (after the line's own spaces), "**", the known {colour} markers and the {/} that close one dropped; the rest as
// it is (the same reader as markdown())
std::string strip_markdown(const std::string& text);

// The description box: the description (clean_text, then markdown), a blank line, rsm.details_author "Author: %s" (when
// there is one; shown_author: its " (drillcodes)" dropped), rsm.details_source "Source: %s" (source_label(T, source,
// author): "drillcodes.com" for such an author).
std::string detail_text(const Tr& T, const std::string& description, const std::string& author, const std::string& source_label);
// The Markdown help (R / pad X in the dialog, shown in the description box): a heading rsm.details_md_help, then one line
// a syntax, "<syntax as typed>  =  <what it does>", the words of what it does (rsm.details_md_*) rendered by that syntax
// itself through markdown() (the heading's and the colours' words in their colour), then a blank line and
// rsm.details_md_note (the 1000 limit, Ctrl+V, the brackets). The syntax shown as typed is never converted: written as
// it is (clean_line: no '<' nor '[' in it), next to the converted part; the translated words are cleaned (clean_line)
// and lose every marker character ('#', '*', '{', '}') first, so each converted part is the one span its syntax makes.
std::string help_text(const Tr& T);
// What a text fits in em of logic::text_em: its region width over its font width (px per em of the estimate ~ the font
// size: the guide row items, 27/10), with a 2 % margin. 0 when unknown.
double em_for(float width_px, float font_w);

// ---- the description edited (T / pad Y: the game's one-line text dialog) ------------------------------------------------
// s cut to `units` UTF-16 units (a character past U+FFFF counts 2, as JavaScript and the Worker count), on a character
// boundary (community/page.cpp's share cut, any limit)
std::string cut_utf16(const std::string& s, size_t units);
// The description as the text dialog shows it (one line): clean_text, each line break written "/n" (a paragraph break is
// "/n/n"), cut to kDescMaxUnits (Wael 04/11: "Test /n Test" typed for a new line)
std::string edit_text(const std::string& description);
// The text dialog's text back to a description: "/n" and a typed "\n" (backslash, n) are line breaks, then clean_text
// (each line trimmed, one blank line at most in a row), cut to kDescMaxUnits
std::string edit_result(const std::string& text);

// ---- the key guide at the bottom (the game's InputGuide GUI, while the dialog is up) ------------------------------------
// In game (04/11): each entry of the bar is one text with its glyphs inline ("<ICON KeyEsc><ICON MouseR> Back"); for the
// game's own image dialog the bar holds Back and Confirm, for ours the Drill row's Back and "<ICON KeyA><ICON +><ICON
// KeyE> Go to Basic Settings" (the tab keys, drawn by the game for the device and layout in use).
// The leading glyphs of a guide text when they are a key combination ("<ICON KeyA><ICON +><ICON KeyE>" of "... Go to
// Basic Settings"): the game's own tab-left / tab-right glyphs. "" otherwise.
std::string leading_combo_icons(const std::string& msg);
// The dialog's entries on one text: "<decide> Confirm   <tab-left> / <tab-right> Show More Slots   <T> Edit Drill
// Description   <R> Markdown Help" (the slot pages only with more than one page and tab glyphs known; the game's
// "<ICON +>" between the two tab glyphs written " / ", as the sub-tab header's "[W] / [Q]"); pad: <ICON BtnD> / <ICON BtnU>
// / <ICON BtnL> (the west button, as the Community dialog's Sort), keyboard <ICON KeyF> / <ICON KeyT> / <ICON KeyR>. The
// R entry follows the pane (Wael 04/11): rsm.details_md_help "Markdown Help" while the description is shown,
// rsm.details_show_desc "Show Drill Description" while the Markdown help is (help_shown).
std::string guide_entries(const Tr& T, bool pad, const std::string& tab_icons, bool more_pages, bool help_shown = false);
// The bar's items laid out as the game lays them, left to right: the first keeps its x, each next one at the previous
// x + its region width + its font width (the option window's guide row, measured 27/10)
std::vector<float> guide_layout(const std::vector<float>& x, const std::vector<float>& w, const std::vector<float>& font_w);

// ---- the wrapped lines -------------------------------------------------------------------------------------------------
// The byte offsets where the game starts each line of `text` (UTF-8), from baseline(g) = getCharBaseLine(g) read after
// the game wrapped the whole text. g counts glyphs from 1 (from 0 when baseline(0) is not 0) and skips every "\n" and
// the space the game drops at each wrap break: walking the text, a "\n" starts a line without taking a g; a baseline
// that jumps (more than 1 px) on a space is that dropped space (the line starts after it, no g taken); a jump on any
// other character starts the line there. The game's tags ("<...>": only markdown()'s, a user's '<' is '(') are no
// glyphs: walked over without a g. laid_out = some baseline was not 0 (all 0: the game had not laid it out yet; starts
// is then { 0 }).
struct Lines { std::vector<size_t> starts{ 0 }; bool laid_out = false; };
Lines line_starts(const std::string& text, const std::function<float(uint32_t g)>& baseline);
// Lines [top, top + n) of a text of text_size bytes: from starts[top] to starts[top + n] (to the end past the last
// line). top is clamped into 0 .. max_top.
struct Span { size_t begin = 0, end = 0; };
Span window(const std::vector<size_t>& starts, size_t text_size, int top, int n);
// The window of a markdown() text as set_Message gets it, valid on its own: the colour span open at its start opened
// again before it, the one still open at its end closed after it.
std::string window_text(const std::string& text, const std::vector<size_t>& starts, int top, int n);
int max_top(int total, int n);                 // max(0, total - n)
// The scroll bar's thumb in a track track_h high: max(kThumbMin, track_h * n / total) high (at most the track), at
// (track_h - h) * top / (total - n) from the track's top. total <= n: the whole track.
struct Thumb { float y = 0, h = 0; };
Thumb thumb(float track_h, int total, int n, int top);

// ---- keys ----------------------------------------------------------------------------------------------------------------
// A key held: true at the press, then every kRepeatEvery frames once held more than kRepeatDelay frames.
struct HoldRepeat {
    int held = 0;
    bool step(bool down);
    void reset() { held = 0; }
};

// ---- the mouse wheel over the description (Wael 04/11) ------------------------------------------------------------------
// The wheel scrolls the description (or the Markdown help) when it has a scroll bar and the cursor is over its text or its
// bar; anywhere else, or without a bar, it does nothing.
constexpr float kGuiWidth = 1920.0f;           // the game's UI is laid out at 1920 x 1080 (its global positions: 1080p units)
constexpr float kGuiHeight = 1080.0f;
constexpr int kWheelDelta = 120;               // WHEEL_DELTA: one notch of WM_MOUSEWHEEL
constexpr int kWheelLines = 3;                 // lines one notch scrolls
// A point of the game window's client area (pixels) in the UI's 1920 x 1080 units: the UI drawn at the largest scale that
// fits, min(w / 1920, h / 1080), centred (bars left and right in a wider window, above and below in a taller one). ok false
// for an empty client area.
struct GuiPoint { float x = 0, y = 0; bool ok = false; };
GuiPoint client_to_gui(float x, float y, float client_w, float client_h);
// The area the wheel acts on, in the UI's units, from the description's region left / top edges: kDetailWidth wide plus
// the scroll bar (kBarGap further, kBarWidth wide), the window's kWindowLines lines high (+ the bar's kBarTopOffset)
struct Area { float left = 0, top = 0, right = 0, bottom = 0; };
Area wheel_area(float text_left, float text_top);
bool contains(const Area& a, const GuiPoint& p);   // edges included; a point not ok is never inside
// The wheel's deltas summed (a smooth wheel or a touchpad sends less than a notch at a time), whole notches taken out: the
// lines to scroll, forward (a positive delta) = up (negative lines), kWheelLines a notch. A delta the other way drops what
// was left of a notch first.
struct WheelSteps {
    int rest = 0;
    int take(int delta);
    void reset() { rest = 0; }
};

}  // namespace ui::details
