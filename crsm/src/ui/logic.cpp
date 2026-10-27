#include "logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "model/labels.hpp"
#include "model/lutil.hpp"

namespace ui::logic {

const char* const kUp = "\xE2\x96\xB2";       // U+25B2 (RMT:842 UP)
const char* const kDown = "\xE2\x96\xBC";     // U+25BC (RMT:842 DOWN)

namespace {
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
}  // namespace

int clamp_win(int w) { return std::max(0, std::min(kMaxWin, w)); }

int record_slot_id(int n) { return n <= 8 ? n - 1 : kWorkSlot; }

int replay_slot_id(int n, bool big) { return (big || n <= 8) ? n - 1 : kWorkSlot; }

const char* window_arrow(int k, int win) {
    if (k == 1 && win > 0) return kUp;
    if (k == kWindow && win < kMaxWin) return kDown;
    return "";
}

const char* reversal_arrow(int idx, int win) {
    if (idx == 1 && win > 0) return kUp;                         // RSL:351
    if (idx == kRevPage && win < kRevMaxWin) return kDown;
    return "";
}

// ---- cursor ------------------------------------------------------------------------------------------------------------
CursorAnswer record_cursor(int cur, int nxt, int dir, int win, bool outside) {
    CursorAnswer a;
    if (nxt == cur) return a;                                   // RMT:1836
    const bool nxt_inside = nxt >= kFirstRow && nxt <= kLastRow;
    if (outside) {                                              // from the strip / Recording Trigger: slot 1 / slot 30
        const bool wrap_top = cur > kLastRow && dir > 0 && nxt_inside;   // Down from Countdown loops to the top
        if ((cur < kFirstRow && dir > 0) || wrap_top) {
            a.move = true; a.win = 0;                           // apply_shift(param, -win)
            a.replace = true; a.index = kFirstRow;
            return a;
        }
        if (cur > kLastRow && dir < 0) {
            a.move = true; a.win = kMaxWin;                     // apply_shift(param, SLOT_TOTAL - WINDOW - win)
            a.replace = true; a.index = kLastRow;
        }
        return a;
    }
    if (nxt_inside) return a;                                   // staying inside the block
    int delta = 0;
    if (dir > 0 && win < kMaxWin) delta = 1;                    // leaving it: the window slides instead
    else if (dir < 0 && win > 0) delta = -1;
    if (delta == 0) return a;
    a.move = true; a.win = clamp_win(win + delta);
    a.replace = true; a.index = cur;                            // the cursor stays on its row
    return a;
}

CursorAnswer replay_cursor(int cur, int nxt, int dir, int rwin, bool outside, const std::function<bool(int)>& disabled) {
    CursorAnswer a;
    if (nxt == cur) return a;
    if (outside) {                                              // from just outside the block: the first / last valid slot
        const bool wrap_top = cur > kLastRow && dir > 0 && nxt >= kFirstRow && nxt <= kLastRow;   // Down from the last spin
        if (wrap_top) dir = 1;
        else if (!((cur < kFirstRow && dir > 0) || (cur > kLastRow && dir < 0))) return a;
        if (wrap_top) cur = kFirstRow - 1;                      // searched from the top of the whole list
    } else {
        if (nxt >= kFirstRow && nxt <= kLastRow) return a;      // staying inside the block
    }
    // replay_jump (RMT:1267-1278)
    const bool in_block = cur >= kFirstRow && cur <= kLastRow;
    const int k = in_block ? cur - kFirstRow + 1 : 0;
    const int from = in_block ? (rwin + k) : (dir > 0 ? 0 : kSlotTotal + 1);
    int m = 0;
    for (int n = from + dir; n >= 1 && n <= kSlotTotal; n += dir)
        if (!disabled(n)) { m = n; break; }
    if (m == 0) {
        if (!outside) { a.move = true; a.win = dir > 0 ? kMaxWin : 0; }   // nothing valid that way: the list scrolls to its end
        return a;
    }
    const int nw = clamp_win(in_block ? (m - k) : (dir > 0 ? (m - 1) : (m - kWindow)));
    a.move = true; a.win = nw;
    a.replace = true; a.index = kFirstRow + (m - nw) - 1;
    return a;
}

CursorAnswer reversal_cursor(int cur, int nxt, int dir, int win) {
    CursorAnswer a;
    if (nxt == cur) return a;
    const bool inside = nxt >= 1 && nxt <= kRevPage;
    if (cur < 1) {                                              // from the strip row: Down to slot 1, Up (the game's wrap to
        const int nw = dir > 0 ? 0 : kRevMaxWin;                // the last row) to slot 30, as the game's own 10-row page
        if (!inside || nw == win) return a;
        a.move = true; a.win = nw;                              // the cursor goes where the game sends it
        return a;
    }
    const bool forward = (dir > 0 && nxt > cur) || (dir < 0 && nxt < cur);
    if (inside && forward) return a;                            // staying inside the block (a wrap is not "inside")
    const int nw = std::max(0, std::min(kRevMaxWin, win + dir));
    if (nw != win) {
        a.move = true; a.win = nw;
        a.replace = true; a.index = cur;                        // the window slid: the cursor stays on its row
        return a;
    }
    // the window at its end: leaving the block forward (the strip above slot 1) is the game's; the game's wrap goes on
    // to the other end of the 30 rows (Down on slot 30 -> slot 1, Up into the last row -> slot 30). RSL left the window
    // where it was: Down on slot 30 showed slot 21.
    if (forward || (dir < 0 && !inside)) return a;
    const int ww = dir > 0 ? 0 : kRevMaxWin;
    if (ww == win) return a;
    a.move = true; a.win = ww;                                  // the cursor goes where the game sends it (row 1 / row 10)
    return a;
}

// ---- fit ---------------------------------------------------------------------------------------------------------------
FitOut fit_name(const FitIn& in) {
    FitOut o;
    const float left = kFitLeftOrigin + (in.has_name ? in.name_w : 60.0f) + kFitMargin;   // right edge of "Slot N"
    const float right = -(in.has_frames ? in.frames_w : 40.0f) / 2.0f - kFitMargin;       // left edge of the frames
    float avail = (right - left) * kFitFill;
    if (avail < kFitMinRoom) avail = kFitMinRoom;
    const float centre = (left + right) / 2.0f;
    float scale = 1.0f;
    if (in.own_w > 1.0f && in.font_w > 0.0f) {                  // the natural width at the base font: stable
        const float w_base = in.own_w * (in.base_w / in.font_w);
        if (w_base > avail) scale = std::max(kFitMinScale, avail / w_base);
    }
    o.font_w = in.base_w * scale;
    o.font_h = in.base_h * scale;
    o.set_font = std::fabs(in.font_w - o.font_w) > 0.5f;
    o.x = centre;
    o.set_x = std::fabs(in.pos_x - centre) > 1.0f;
    return o;
}

// ---- texts in em ---------------------------------------------------------------------------------------------------------
namespace {

const char* const kEllipsis = "\xE2\x80\xA6";   // U+2026, as short_list cuts the spin's names
constexpr double kEllipsisEm = 1.0;
const char* const kNoDate = "\x01";              // a date that community_label copies as it is: then taken out of the label

// Arial's advance widths, thousandths of an em, ASCII 32-126
constexpr uint16_t kAsciiW[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,   // space ! " # $ % & ' ( ) * + , - . /
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,   // 0-9 : ; < = > ?
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,  // @ A-O
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,   // P-Z [ \ ] ^ _
    333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,   // ` a-o
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584 };      // p-z { | } ~

size_t u8_len(const std::string& s, size_t i) {  // the bytes of the character at i (a stray byte counts alone)
    const unsigned char c = (unsigned char)s[i];
    const size_t n = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    return std::min(n, s.size() - i);
}

double char_em(const std::string& s, size_t i, size_t n) {
    const unsigned char c = (unsigned char)s[i];
    if (n == 1) return (c >= 32 && c < 127) ? kAsciiW[c - 32] / 1000.0 : 0.0;
    if (s.compare(i, n, kEllipsis) == 0) return kEllipsisEm;
    if (n == 3 && c >= 0xE3 && c <= 0xED) return 1.0;            // U+3000-U+DFFF: CJK, kana, Hangul
    if (n == 3 && c == 0xEF && i + 1 < s.size() && ((unsigned char)s[i + 1] == 0xBC || (unsigned char)s[i + 1] == 0xBD)) return 1.0;   // full-width forms
    return 0.6;
}

// a markup tag at i ("<color ...>", "</color>", "<ICON ...>"): its length, 0 when '<' is a plain character
size_t tag_len(const std::string& s, size_t i) {
    if (s[i] != '<' || i + 1 >= s.size()) return 0;
    const char n = s[i + 1];
    if (!(n == '/' || (n >= 'A' && n <= 'Z') || (n >= 'a' && n <= 'z'))) return 0;
    const size_t e = s.find('>', i + 1);
    return (e == std::string::npos || e - i > 64) ? 0 : e - i + 1;
}

size_t char_count(const std::string& s) {
    size_t k = 0;
    for (size_t i = 0; i < s.size(); i += u8_len(s, i)) ++k;
    return k;
}

std::string head_chars(const std::string& s, size_t chars) {   // the first `chars` characters
    size_t i = 0;
    for (size_t k = 0; k < chars && i < s.size(); ++k) i += u8_len(s, i);
    return s.substr(0, i);
}

// the longest head of s (no markup) whose width is <= max_em
std::string head_em(const std::string& s, double max_em) {
    double w = 0;
    size_t i = 0;
    while (i < s.size()) {
        const size_t n = u8_len(s, i);
        const double cw = char_em(s, i, n);
        if (w + cw > max_em + 1e-9) break;
        w += cw;
        i += n;
    }
    return s.substr(0, i);
}

void trim_right(std::string& s) { while (!s.empty() && s.back() == ' ') s.pop_back(); }

void trim(std::string& s) {
    trim_right(s);
    size_t b = 0;
    while (b < s.size() && s[b] == ' ') ++b;
    s.erase(0, b);
}

// s cut to max_em with "…", but never below min_chars characters (s itself when it is not longer than that)
std::string cut_min(const std::string& s, double max_em, size_t min_chars) {
    std::string c = cut_em(s, max_em);
    if (c == s) return c;
    if (char_count(s) <= min_chars) return s;
    std::string body = c.substr(0, c.size() - std::strlen(kEllipsis));
    if (char_count(body) < min_chars) { body = head_chars(s, min_chars); trim_right(body); }
    return body + kEllipsis;
}

// community_label of `item` with the date taken out when it is kNoDate (and its two spaces)
bool label_of(const nlohmann::json& item, const nlohmann::json& dv, std::string* out) {
    if (!model::labels::community_label(item, dv, out)) return false;
    const std::string gap = std::string("  ") + kNoDate;
    for (size_t p = out->find(gap); p != std::string::npos; p = out->find(gap)) out->erase(p, gap.size());
    return true;
}

}  // namespace

double text_em(const std::string& s) {
    double w = 0;
    size_t i = 0;
    while (i < s.size()) {
        if (const size_t t = tag_len(s, i)) { i += t; continue; }
        const size_t n = u8_len(s, i);
        w += char_em(s, i, n);
        i += n;
    }
    return w;
}

std::string cut_em(const std::string& s, double max_em) {
    if (text_em(s) <= max_em + 1e-9) return s;
    std::string h = head_em(s, max_em - kEllipsisEm);
    trim_right(h);
    return h + kEllipsis;
}

std::string com_row_fit(const nlohmann::json& shown, const nlohmann::json& dv, const std::string& full, double budget_em) {
    using nlohmann::json;
    if (text_em(full) <= budget_em || !shown.is_object()) return full;
    json it = shown;
    std::string label;
    it["updated_at"] = kNoDate;                                 // 1. the date
    if (!label_of(it, dv, &label)) return full;
    if (text_em(label) <= budget_em) return label;
    const json dn = it.contains("display_name") ? it["display_name"] : json();
    if (dn.is_string()) {                                       // 2. the loadout name (its trailing " [TAG]" kept whole)
        const std::string name = model::lutil::strip_json_ext(dn.get<std::string>());
        std::string head = name, tail;
        const size_t tb = name.rfind(" [");
        if (!name.empty() && name.back() == ']' && tb != std::string::npos && tb > 0 && name.size() - tb <= 16) {
            head = name.substr(0, tb);
            tail = name.substr(tb);
        }
        const std::string ch = cut_min(head, text_em(head) - (text_em(label) - budget_em), kComNameMinChars);
        const std::string cut = ch == head ? name : ch + tail;
        if (cut != name) {
            it["display_name"] = cut;
            std::string l;
            if (label_of(it, dv, &l)) label = l;
        }
        if (text_em(label) <= budget_em) return label;
    }
    const json up = it.contains("uploader_name") ? it["uploader_name"] : json();
    if (up.is_string()) {                                       // 3. the author's name (the mark after it kept)
        std::string plain = up.get<std::string>(), mark;
        const size_t m = plain.find('<');
        if (m != std::string::npos) { mark = plain.substr(m); plain.erase(m); }
        trim_right(plain);
        if (plain.compare(0, 7, "Player ") == 0) plain.erase(0, 7);   // (community_label drops it: the same width)
        const std::string cut = cut_min(plain, text_em(plain) - (text_em(label) - budget_em), kComAuthorMinChars);
        if (cut != plain) {
            it["uploader_name"] = mark.empty() ? cut : cut + " " + mark;
            std::string l;
            if (label_of(it, dv, &l)) label = l;
        }
    }
    return label;
}

std::vector<std::string> desc_lines(const std::string& raw, double max_em, int max_lines) {
    std::vector<std::string> lines;
    if (max_lines < 1) return lines;
    std::string clean;
    const std::string in = model::lutil::utf8_cut(raw, 4000);
    for (char ch : in) {
        if (ch == '\r') continue;
        if (ch == '\t') ch = ' ';
        else if (ch == '<') ch = '[';
        else if (ch == '>') ch = ']';
        else if (ch != '\n' && ((unsigned char)ch < 0x20 || ch == 0x7F)) continue;
        clean += ch;
    }
    bool more = false;                                          // lines past max_lines were dropped
    auto push = [&](std::string l) {
        trim(l);
        if (l.empty()) return;
        if ((int)lines.size() < max_lines) lines.push_back(std::move(l));
        else more = true;
    };
    size_t p = 0;
    while (p <= clean.size()) {
        size_t e = clean.find('\n', p);
        if (e == std::string::npos) e = clean.size();
        std::string cur;
        size_t w = p;
        while (w < e) {                                         // the paragraph's words
            while (w < e && clean[w] == ' ') ++w;
            size_t we = w;
            while (we < e && clean[we] != ' ') ++we;
            if (we == w) break;
            std::string word = clean.substr(w, we - w);
            w = we;
            const std::string cand = cur.empty() ? word : cur + " " + word;
            if (text_em(cand) <= max_em) { cur = cand; continue; }
            push(cur);
            cur = std::move(word);
            while (text_em(cur) > max_em) {                     // a word wider than a line
                std::string h = head_em(cur, max_em);
                if (h.empty()) h = head_chars(cur, 1);
                cur.erase(0, h.size());
                push(h);
            }
        }
        push(cur);
        p = e + 1;
    }
    if (more && !lines.empty()) {
        std::string& last = lines.back();
        std::string h = text_em(last) + kEllipsisEm <= max_em ? last : head_em(last, max_em - kEllipsisEm);
        trim_right(h);
        last = h + kEllipsis;
    }
    return lines;
}

std::string desc_line(const std::string& raw, double max_em) {
    std::string joined;
    for (const std::string& l : desc_lines(raw, 1e9, 1000)) joined += (joined.empty() ? "" : " | ") + l;
    const std::vector<std::string> one = desc_lines(joined, max_em, 1);
    return one.empty() ? std::string() : one[0];
}

std::vector<std::string> com_desc_page(const std::vector<std::string>& lines, int per_page, uint64_t page) {
    if (lines.empty()) return {};
    const size_t per = (size_t)std::max(1, per_page);
    const size_t pages = (lines.size() + per - 1) / per;
    const size_t p = (size_t)(page % pages);
    std::vector<std::string> out(lines.begin() + (ptrdiff_t)(p * per), lines.begin() + (ptrdiff_t)std::min(lines.size(), (p + 1) * per));
    if (pages > 1) out.back() += "  (" + std::to_string(p + 1) + "/" + std::to_string(pages) + ")";
    return out;
}

bool keep_bottom_y(int32_t control_point, float y_old, float h_old, float h_new, float* y_new) {
    const int32_t v = control_point >> 2;
    if (control_point < 0 || v > 2 || (control_point & 3) > 2) return false;
    const float below = 1.0f - 0.5f * (float)v;             // the share of the box under its ControlPoint
    if (y_new) *y_new = y_old + below * (h_old - h_new);
    return true;
}

double guide_text_w(const std::string& msg, double font_w) {
    size_t icons = 0;
    for (size_t p = msg.find("<ICON"); p != std::string::npos; p = msg.find("<ICON", p + 5)) ++icons;
    return (text_em(msg) + kGuideIconEm * (double)icons) * font_w;
}

bool guide_w_fits(const std::string& msg, double region_w, double font_w) {
    const double est = guide_text_w(msg, font_w);
    return est > 0 && region_w >= 0.6 * est && region_w <= 1.6 * est;
}

std::string com_guide_text(const std::vector<std::string>& extra, const std::string& guide) {
    std::string s;
    for (const std::string& l : extra) if (!l.empty()) s += l + "\n";
    return s.empty() ? guide : s + "\n" + guide;
}

// ---- wording -----------------------------------------------------------------------------------------------------------
bool learn_name_format(const std::string& msg, std::string* prefix, std::string* suffix) {
    size_t d = 0;
    while (d < msg.size() && !is_digit(msg[d])) ++d;
    if (d == msg.size()) return false;                          // t:find("%d")
    size_t e = d;
    while (e < msg.size() && is_digit(msg[e])) ++e;
    *prefix = msg.substr(0, d);                                 // t:gsub("%d+", "%%d", 1)
    *suffix = msg.substr(e);
    return true;
}

std::string slot_text(bool learnt, const std::string& prefix, const std::string& suffix, int n) {
    if (!learnt) return "Slot " + std::to_string(n);
    return prefix + std::to_string(n) + suffix;
}

bool is_frames_text(const std::string& msg) {
    size_t i = 0;
    while (i < msg.size() && is_digit(msg[i])) ++i;
    if (i == 0) return false;
    while (i < msg.size() && is_space(msg[i])) ++i;
    return i + 1 == msg.size() && msg[i] == 'F';
}

bool learnable_empty_word(const std::string& msg) { return !msg.empty() && !is_frames_text(msg) && msg != "nil"; }

bool learnable_start_text(const std::string& msg) {
    return !msg.empty() && msg != "nil" && msg != kUp && msg != kDown;
}

// ---- text dialog -------------------------------------------------------------------------------------------------------
void naming_decide(NameJob& job, const std::optional<std::string>& field) {
    job.decided = true;
    if (field && (!job.raw || job.raw->empty())) job.raw = *field;
}

std::optional<std::string> naming_outcome(const NameJob& job, const std::optional<std::string>& field) {
    if (job.cancelled) return std::nullopt;
    // Lua `job.raw or job.result or dialog_text(job) or ""`: an empty string is a value there (only nil falls through)
    std::string str;
    if (job.raw) str = *job.raw;
    else if (job.result) str = *job.result;
    else if (field) str = *field;
    if (!job.decided && str == job.initial) return std::nullopt;   // closed without a decision and nothing typed
    return str;
}

// ---- Community dialog --------------------------------------------------------------------------------------------------
void ComDialog::render() {
    labels_.assign(kRows, std::string());
    std::vector<std::string> descs(kRows);
    const bool loading_empty = p_.loading() && p_.row_count() == 0;
    const int n = p_.row_count();
    for (int i = 1; i <= kRows; ++i) {
        // RMT:300-303: every row's title function tests the same condition, so all 7 rows read "Loading..." until the
        // first answer when no rows were built (all of them, not row 1 only)
        if (loading_empty) labels_[i - 1] = "Loading...";
        else labels_[i - 1] = i <= n ? p_.row_label(i) : "--";
        descs[i - 1] = i <= n ? p_.row_desc(i) : "";
    }
    p_.show(p_.title(pad_), labels_, descs);
    shown_version_ = p_.version();
}

int ComDialog::current_row() {
    int f = p_.focused_row();
    if (f < 1) f = guide_focus_;
    if (f < 1 || f > kRows) f = 1;
    return f;
}

void ComDialog::open(bool pad) {
    p_.page_open();                                             // char, sync, sort New, page 1, fetch (RMT:421-426)
    open_ = true;
    pad_ = pad;                                                 // the first render already right: no restart at the first tick
    a_.reset(); e_.reset(); t_.reset(); r_.reset();
    guide_focus_ = 0;
    render();
    p_.open_menu();
}

void ComDialog::closed() {
    open_ = false;
    a_.reset(); e_.reset(); t_.reset(); r_.reset();
}

void ComDialog::tick(bool key_a, bool key_e, bool key_t, bool key_r, bool pad) {
    if (!open_) return;
    if (pad != pad_) {                                          // the player changed device: the title's glyphs follow
        pad_ = pad;
        const int row = current_row();
        render();
        last_focus_ = row - 1;
        p_.navigate(last_focus_);
    }
    // the answer arrived after the dialog was built: rebuilt in place. The Lua meant to keep the cursor row there
    // (com_focus_idx) but wrote it as a global: the row was always 1. Fixed: the cursor stays on its row.
    if (p_.data_changed()) {
        p_.mark_built();
        const int row = current_row();
        render();
        last_focus_ = row - 1;
        p_.navigate(last_focus_);
    } else if (p_.version() != shown_version_) {
        // a vote's answer (the page rebuilt and marked itself built, RMT:385-388): back on the voted row
        const int row = vote_row_ >= 1 ? vote_row_ : current_row();
        render();
        last_focus_ = row - 1;
        p_.navigate(last_focus_);
    }
    const bool a_edge = a_.press(key_a);
    const bool e_edge = e_.press(key_e) && !key_a;              // E ignored while A is held (RMT:334)
    const bool t_edge = t_.press(key_t);
    const bool r_edge = r_.press(key_r);
    if (a_edge && p_.prev_page()) {                             // A / E: client-side pages, cursor to row 1
        render(); last_focus_ = 0; p_.navigate(0);
    } else if (e_edge && p_.next_page()) {
        render(); last_focus_ = 0; p_.navigate(0);
    }
    if (r_edge) {                                               // R: next sort, page 1, refetch, cursor to row 1
        p_.cycle_sort();
        render(); last_focus_ = 0; p_.navigate(0);
    }
    if (t_edge) {                                               // T: like / unlike the focused row (must be downloaded)
        const int row = p_.focused_row();
        vote_row_ = row;
        p_.vote(row);
    }
}

void ComDialog::button(int row) {
    std::string msg;
    if (!p_.download_prompt(row, &msg)) return;                 // empty row / busy / already downloaded (tickers inside)
    p_.confirm(msg, [this](bool yes) { if (yes) p_.download_confirmed(); });
}

// ---- integration with the Manager page (P5) -------------------------------------------------------------------------------
NameKey route_name_key(bool t, bool del, bool busy, bool ready, bool manager) {
    if (busy || !ready) return NameKey::None;                   // RMT:1762 (the edges are still taken: no press is kept)
    if (t) return manager ? NameKey::RenameLoadout : NameKey::NameSlot;   // RMT:1764-1765
    if (del) return manager ? NameKey::None : NameKey::DeleteSlotName;    // (no Record row under the cursor on the Manager)
    return NameKey::None;
}

bool tabs_held(bool naming, bool community, bool box, bool paused) { return naming || community || (box && paused); }

TabFix TabGuard::check(bool modal, const TabSpot& now, TabAt at, int64_t fallback) {
    if (!modal) { held_ = false; spot_ = TabSpot{}; return TabFix{}; }
    if (!held_) {
        if (at == TabAt::Frame) {                                    // the dialog's own spot (unread: next frame)
            if (now.tab >= 0) { held_ = true; spot_ = now; }
            return TabFix{};
        }
        if (fallback < 0) return TabFix{};                           // a rebuild first, nothing known: the game's
        held_ = true;
        spot_ = TabSpot{ fallback, -1 };
    }
    TabFix f;
    f.to = spot_;
    f.tab = now.tab >= 0 && now.tab != spot_.tab;
    f.child = at == TabAt::Rebuild && spot_.child >= 0 && now.child >= 0 && now.child != spot_.child;
    f.list = at == TabAt::Frame && spot_.list >= 0 && now.list >= 0 && now.list != spot_.list;
    return f;
}

bool block_list_select(bool modal, bool primary, int64_t held_list, int64_t value) {
    return modal && primary && (held_list < 0 || value != held_list);
}

TickerSink ticker_sink(bool ui_active, bool ticker_hook) {
    if (!ui_active) return TickerSink::Lua;
    return ticker_hook ? TickerSink::Show : TickerSink::Drop;
}

void after_download(const std::function<bool()>& refresh_manager, const std::function<void()>& invalidate,
                    const std::function<void()>& ensure) {
    if (refresh_manager && refresh_manager()) return;
    if (invalidate) invalidate();
    if (ensure) ensure();
}

}  // namespace ui::logic
