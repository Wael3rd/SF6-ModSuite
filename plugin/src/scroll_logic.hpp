// SF6 Slots -- a long name scrolling inside its place: the timing and the geometry. Pure (plugin/tests/names_test.cpp); the game side is
// name_scroll.cpp.
//
// As Wael chose: rest at the start about 1.5 s, slide to the end, rest about 1.5 s, come back to the start, repeat. A name that fits
// doesn't move. The way it is clipped is the game's own scrolling line's (app.UIPartsScrollText, read 07/11): the text's Position moves,
// and its ScissorRect, local to the text, moves the other way, so the window stays where it is on the screen.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace slots::scroll {

constexpr double kRestSeconds = 1.5;
constexpr double kUnitsPerSecond = 60.0;    // about the game's own line, 1 unit a frame at 60 fps

// How far the text is slid, `t` seconds after it started, when it is `overflow` units wider than its place: 0 is its start, `overflow`
// its end. Whole units: a write only when this changes. A right-to-left text starts from its other end (its beginning is on the right).
inline int offset_at(float overflow, double t, bool right_to_left = false) {
    if (!(overflow > 0)) return 0;
    if (t < 0) t = 0;
    const double slide = overflow / kUnitsPerSecond;
    const double period = 2 * kRestSeconds + 2 * slide;
    double x = std::fmod(t, period), off;
    if (x < kRestSeconds) off = 0;
    else if ((x -= kRestSeconds) < slide) off = x * kUnitsPerSecond;
    else if ((x -= slide) < kRestSeconds) off = overflow;
    else off = overflow - (x - kRestSeconds) * kUnitsPerSecond;
    const int whole = static_cast<int>(std::lround(off));
    const int last = static_cast<int>(std::lround(overflow));
    const int clamped = whole < 0 ? 0 : whole > last ? last : whole;
    return right_to_left ? last - clamped : clamped;
}

// When offset_at gives `offset`, on its way to the end: to go on from there when the overflow changes. Never past the new overflow.
inline double time_at(float overflow, int offset, bool right_to_left = false) {
    if (!(overflow > 0)) return 0;
    const int last = static_cast<int>(std::lround(overflow));
    int along = right_to_left ? last - offset : offset;
    along = along < 0 ? 0 : along > last ? last : along;
    return along == 0 ? 0.0 : kRestSeconds + along / kUnitsPerSecond;
}

// Where the text goes and what shows of it, for a text placed by its left point at `left`, in a place `width` wide: the clip is in the
// text's own space. Top and bottom far enough never to cut a glyph, whatever the point the text's height is measured from.
constexpr float kClipTop = -200.0f, kClipBottom = 400.0f;
struct Window { float x, clip_left, clip_top, clip_right, clip_bottom; };
inline Window window_at(float left, float width, int offset) {
    const float off = static_cast<float>(offset);
    return { left - off, off, kClipTop, off + width, kClipBottom };
}

// The left edge of a place, from the point a text was placed by (via.gui.ControlPoint: its column 0 left, 1 centre, 2 right)
inline float left_of(float x, int control_point, float width) { return x - static_cast<float>(control_point % 4) * 0.5f * width; }

// A name that needs to move
inline bool overflows(float natural, float width) { return natural > width + 0.5f; }

// A Costume row's place, from the point its text is placed by, in the units of the text's parent: from its left arrow's inner edge to its right
// arrow's, a margin inside each. `per_unit`: those units for one global unit. Too narrow to show a name (the arrows not where they should be): not valid.
constexpr float kSpanMinWidth = 60;
struct Span { float l = 0, r = 0; bool ok = false; };
inline Span span_between(float text_gx, float left_edge_gx, float right_edge_gx, float per_unit, float margin) {
    Span s;
    s.l = (left_edge_gx - text_gx) * per_unit + margin;
    s.r = (right_edge_gx - text_gx) * per_unit - margin;
    s.ok = s.r - s.l >= kSpanMinWidth;
    return s;
}
// Whether a text placed by that point stays inside the place (column of its point: 0 left, 1 centre, 2 right)
inline bool fits_span(float natural, const Span& s, int column) {
    constexpr float kSlack = 0.5f;
    if (column == 1) return natural * 0.5f <= (s.r < -s.l ? s.r : -s.l) + kSlack;
    if (column == 0) return natural <= s.r + kSlack;
    return natural <= -s.l + kSlack;
}

// A line in two parts, "Luke : " and "RE8 Ethan Winter Outfit, 01": where the moving part starts and how wide its window is, from what was measured:
// the whole line and the moving part alone. So the fixed part's width, spaces and kerning included, is the language's and the line's own, and the
// moving part is where it is in the whole line. A window too small to read: not split.
constexpr float kSplitMinWindow = 80;
struct SplitPlace { bool ok = false; float offset = 0, window = 0; };
inline SplitPlace split_place(float whole, float moving, float place) {
    SplitPlace s;
    const float fixed = whole - moving;
    if (!(fixed > 0) || place - fixed < kSplitMinWindow) return s;
    s.ok = true;
    s.offset = fixed;
    s.window = place - fixed;
    return s;
}

// Right-to-left: its first strong character is Arabic or Hebrew (what is measured is not reordered here: only the direction of the
// slide depends on this)
inline bool text_is_rtl(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        uint32_t cp = n == 1 ? c : c & (0xFFu >> (n + 1));
        for (int k = 1; k < n && i + static_cast<size_t>(k) < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + static_cast<size_t>(k)]) & 0x3Fu);
        i += static_cast<size_t>(n);
        if (cp < 0x80) { if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return false; continue; }
        if ((cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) || (cp >= 0xFE70 && cp <= 0xFEFF)) return true;
        if (cp >= 0x00C0) return false;     // any other script's letter (Latin accents, CJK...)
    }
    return false;
}

}  // namespace slots::scroll
