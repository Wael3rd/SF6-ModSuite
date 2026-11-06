#include "ui/details_logic.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "model/lutil.hpp"
#include "ui/logic.hpp"

namespace ui::details {
namespace {

size_t u8_len(const std::string& s, size_t i) {  // the bytes of the character at i (a stray byte counts alone)
    const unsigned char c = (unsigned char)s[i];
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
    if (i + n > s.size()) return 1;
    for (size_t k = 1; k < n; ++k) if (((unsigned char)s[i + k] & 0xC0) != 0x80) return 1;
    return n;
}

// The capital of a code point (itself when it has none here): ASCII, Latin-1, Latin Extended-A, Cyrillic.
uint32_t upper_cp(uint32_t c) {
    if (c < 0x80) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE) return c == 0xF7 ? c : c - 0x20;           // à-þ except ÷
    if (c == 0xFF) return 0x178;                                           // ÿ -> Ÿ
    if (c == 0x131) return 'I';                                            // dotless i
    if (c >= 0x100 && c <= 0x137) return (c & 1) ? c - 1 : c;              // Ā ā ... (Ĳ ĳ, İ stays)
    if (c >= 0x139 && c <= 0x148) return (c & 1) ? c : c - 1;              // Ĺ ĺ ... ň
    if (c >= 0x14A && c <= 0x177) return (c & 1) ? c - 1 : c;              // Ŋ ŋ ... ŷ
    if (c >= 0x179 && c <= 0x17E) return (c & 1) ? c : c - 1;              // Ź ź ... ž
    if (c >= 0x430 && c <= 0x44F) return c - 0x20;                         // а-я
    if (c >= 0x450 && c <= 0x45F) return c - 0x50;                         // ѐ-џ
    if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x52F)) return (c & 1) ? c - 1 : c;
    if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? c : c - 1;
    if (c == 0x4CF) return 0x4C0;
    return c;
}

void put_utf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
    else { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
}

void ascii_upper(std::string& s) { s = upper_utf8(s); }

std::string ascii_lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// one line of clean_text: control characters dropped (tabs as spaces), markup brackets replaced, spaces collapsed, trimmed
std::string clean_one(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    for (char ch : line) {
        if (ch == '\t') ch = ' ';
        else if (ch == '<' || ch == '[') ch = '(';   // (a '<' starts a game tag; the game's texts draw no square bracket)
        else if (ch == '>' || ch == ']') ch = ')';
        else if ((unsigned char)ch < 0x20 || ch == 0x7F) continue;
        if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += ch;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

}  // namespace

// ---- slot pages ------------------------------------------------------------------------------------------------------
int page_count(int saved) { return saved <= 0 ? 1 : (saved + kSlotsPerPage - 1) / kSlotsPerPage; }

int clamp_page(int page, int saved) { return std::clamp(page, 0, page_count(saved) - 1); }

Range page_range(int saved, int page) {
    Range r;
    if (saved <= 0) return r;
    r.first = clamp_page(page, saved) * kSlotsPerPage;
    r.last = std::min(saved, r.first + kSlotsPerPage);
    return r;
}

// ---- texts -------------------------------------------------------------------------------------------------------------
std::string upper_utf8(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const size_t n = u8_len(s, i);
        if (n == 1) { const unsigned char c = (unsigned char)s[i]; out += c < 0x80 ? (char)upper_cp(c) : s[i]; ++i; continue; }
        if (n > 3) { out.append(s, i, n); i += n; continue; }                    // past U+FFFF: no case here
        uint32_t cp = n == 2 ? ((unsigned char)s[i] & 0x1F) : ((unsigned char)s[i] & 0x0F);
        for (size_t k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        const uint32_t up = upper_cp(cp);
        if (up == cp) out.append(s, i, n); else put_utf8(out, up);
        i += n;
    }
    return out;
}

std::string header_text(const Tr& T, int page, int pages) {
    std::string s = T("details_page", { std::to_string(page + 1), std::to_string(std::max(1, pages)) });
    ascii_upper(s);
    return s;
}

std::string saved_text(const Tr& T, int saved) {
    std::string s = saved == 1 ? T("details_saved_one", {}) : T("details_saved", { std::to_string(std::max(0, saved)) });
    ascii_upper(s);
    return s;
}

std::string row_text(const std::string& slot_label, const std::string& name_raw, int64_t frames, double max_em) {
    const std::string name = clean_line(name_raw);
    const std::string tail = " - " + std::to_string(std::max<int64_t>(0, frames)) + "F";
    const std::string bare = slot_label + tail;
    const std::string full = name.empty() ? bare : slot_label + " - " + name + tail;
    if (max_em <= 0 || logic::text_em(full) <= max_em) return full;
    if (!name.empty()) {
        const double room = max_em - logic::text_em(slot_label + " - " + tail);
        if (room >= 3.0) {
            const std::string cut = slot_label + " - " + logic::cut_em(name, room) + tail;
            if (logic::text_em(cut) <= max_em) return cut;
        }
        if (logic::text_em(bare) <= max_em) return bare;
    }
    return logic::cut_em(full, max_em);
}

bool drillcodes_author(const std::string& author, std::string* bare) {
    static const std::string kSuffix = "(drillcodes)";
    const std::string a = clean_line(author);   // (one line, spaces collapsed, trimmed: "x (drillcodes)  " ends with it)
    const std::string low = ascii_lower(a);
    if (low.size() < kSuffix.size() || low.compare(low.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) return false;
    const size_t cut = low.size() - kSuffix.size();
    if (cut > 0 && low[cut - 1] != ' ') return false;   // ("Bob(drillcodes)" is a name, not the label)
    if (bare) {
        std::string b = a.substr(0, cut);
        while (!b.empty() && b.back() == ' ') b.pop_back();
        *bare = b;
    }
    return true;
}

std::string shown_author(const std::string& author) {
    std::string bare;
    return drillcodes_author(author, &bare) ? bare : clean_line(author);
}

std::string source_label(const Tr& T, const std::string& source, const std::string& author) {
    if (drillcodes_author(author)) return "drillcodes.com";
    const std::string s = clean_line(source);
    const std::string low = ascii_lower(s);
    if (s.empty()) return T("source_local", {});
    if (low == "community") return T("source_community", {});
    if (low == "drillcodes.com" || low == "drillcodes") return "drillcodes.com";
    return s;
}

Origin origin(const std::string& source) {
    const std::string low = ascii_lower(clean_line(source));
    if (low == "community") return Origin::Community;
    if (low == "drillcodes.com" || low == "drillcodes") return Origin::DrillCodes;
    return Origin::Local;
}

Origin origin(const std::string& source, const std::string& author) {
    return drillcodes_author(author) ? Origin::DrillCodes : origin(source);
}

std::string short_source(const Tr& T, const std::string& source, const std::string& author) {
    switch (origin(source, author)) {
        case Origin::Community: return T("source_community", {});
        case Origin::DrillCodes: return "DrillCodes";
        default: return T("source_local_short", {});
    }
}

std::string frame_author(const std::string& author, const std::string& own_name, const std::string& source) {
    const std::string a = shown_author(author);
    if (!a.empty()) return a;
    return origin(source, author) == Origin::Local ? clean_line(own_name) : std::string();
}

std::string frame_text(const std::string& character, const std::string& author, const std::string& source_label, double max_em) {
    const std::string c = clean_line(character), a = clean_line(author), s = clean_line(source_label);
    auto join = [](std::initializer_list<const std::string*> parts) {
        std::string r;
        for (const std::string* p : parts) if (!p->empty()) r += (r.empty() ? "" : " - ") + *p;
        return r;
    };
    const std::string full = join({ &c, &a, &s });
    if (max_em <= 0 || logic::text_em(full) <= max_em) return full;
    if (!a.empty()) {
        const std::string bare = join({ &c, &s });
        const double room = max_em - logic::text_em(bare) - (bare.empty() ? 0.0 : logic::text_em(" - "));
        if (room >= 3.0) {
            const std::string cut_a = logic::cut_em(a, room);
            const std::string cut = join({ &c, &cut_a, &s });
            if (logic::text_em(cut) <= max_em) return cut;
        }
        if (!bare.empty() && logic::text_em(bare) <= max_em) return bare;
    }
    return logic::cut_em(full, max_em);
}

std::string clean_text(const std::string& raw, size_t max_bytes) {
    const std::string in = model::lutil::utf8_cut(raw, max_bytes);
    std::string out;
    size_t blank = 0;                                   // line breaks waiting (a blank line = 2)
    size_t p = 0;
    while (p <= in.size()) {
        size_t e = in.find('\n', p);
        if (e == std::string::npos) e = in.size();
        const std::string line = clean_one(in.substr(p, e - p));
        if (line.empty()) {
            if (!out.empty()) blank = 2;                // (none before the first line)
        } else {
            if (!out.empty()) out.append(std::max<size_t>(blank, 1), '\n');
            out += line;
            blank = 0;
        }
        p = e + 1;
    }
    return out;
}

std::string clean_line(const std::string& raw) {
    std::string s = raw;
    std::replace(s.begin(), s.end(), '\n', ' ');
    std::string out = clean_one(s);
    if (!out.empty() && out.front() == ' ') out.erase(0, 1);
    return out;
}

// ---- the light Markdown ---------------------------------------------------------------------------------------------------
namespace {

// The presets as the game draws them (their colours stored 0xAABBGGRR, Wael 04/11): FGM_Cmn_2 light violet (the Frame
// Meter page's headings), FGM_Cmn_1 bright pink-magenta (R CE G 24 B CC), FGM_Cmn_0 dark purple (R 8F G 1E B B5)
enum class Ink { None, Light, Pink, Purple, Blue };
const char* open_tag(Ink k) {
    switch (k) {
        case Ink::Light: return "<COLOR preset=\"FGM_Cmn_2\">";
        case Ink::Pink: return "<COLOR preset=\"FGM_Cmn_1\">";
        case Ink::Purple: return "<COLOR preset=\"FGM_Cmn_0\">";
        case Ink::Blue: return "<HLBGN Arg0 = \"3\">";
        default: return "";
    }
}
const char* close_tag(Ink k) { return k == Ink::Blue ? "<HLEND Arg0 = \"3\">" : (k == Ink::None ? "" : "</COLOR>"); }
Ink ink_of(const std::string& word) {
    if (word == "violet") return Ink::Light;
    if (word == "magenta") return Ink::Pink;
    if (word == "purple") return Ink::Purple;
    if (word == "blue") return Ink::Blue;
    return Ink::None;
}

// markdown() with the game's tags (tags) or without anything for the markers (strip_markdown): one reader, so the two
// agree on what is a marker (a {/} that closes nothing, an unknown {word}: text). The stripped text is not trimmed: its
// heading's "# " may follow the line's own spaces.
std::string convert(const std::string& text, bool tags) {
    std::string out;
    size_t p = 0;
    while (p <= text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(p, e - p);
        Ink cur = Ink::None;
        bool cur_brace = false;                  // cur was opened by a {colour} marker (its {/} expected)
        int orphans = 0;                         // {colour} spans a nested one closed: their own {/} still to come
        size_t opened_at = 0;                    // out's size right after the open tag (an empty span is taken back)
        auto close = [&] {
            if (cur == Ink::None) return;
            if (tags) {
                const size_t ol = std::strlen(open_tag(cur));
                if (out.size() == opened_at) out.erase(opened_at - ol);
                else out += close_tag(cur);
            }
            cur = Ink::None;
        };
        auto open = [&](Ink k, bool brace) {
            if (cur != Ink::None && cur_brace) ++orphans;
            close();
            if (tags) out += open_tag(k);
            opened_at = out.size();
            cur = k;
            cur_brace = brace;
        };
        size_t i = 0;
        const size_t lead = tags ? 0 : std::min(line.size(), line.find_first_not_of(' '));
        if (line.compare(lead, 2, "# ") == 0) {
            out += line.substr(0, lead);
            open(Ink::Light, false);
            i = lead + 2;
        }
        while (i < line.size()) {
            if (line.compare(i, 2, "**") == 0) {
                if (cur == Ink::Pink) close(); else open(Ink::Pink, false);
                i += 2;
                continue;
            }
            if (line[i] == '{') {
                const size_t j = line.find('}', i + 1);
                if (j != std::string::npos) {
                    const std::string word = line.substr(i + 1, j - i - 1);
                    if (word == "/" && cur != Ink::None) { close(); i = j + 1; continue; }
                    if (word == "/" && orphans > 0) { --orphans; i = j + 1; continue; }   // (the closer of a span closed early)
                    if (const Ink k = ink_of(word); k != Ink::None) { open(k, true); i = j + 1; continue; }   // (else: text)
                }
            }
            out += line[i++];
        }
        close();
        if (e < text.size()) out += '\n';
        p = e + 1;
    }
    return out;
}

}  // namespace

std::string markdown(const std::string& clean) { return convert(clean, true); }

std::string strip_markdown(const std::string& text) { return convert(text, false); }

std::string detail_text(const Tr& T, const std::string& description, const std::string& author, const std::string& source_label) {
    std::string out = markdown(clean_text(description, kDescMaxBytes));
    if (!out.empty()) out += "\n\n";
    const std::string a = shown_author(author);
    if (!a.empty()) out += T("details_author", { a }) + "\n";
    out += T("details_source", { source_label });
    return out;
}

std::string help_text(const Tr& T) {
    // the syntax as typed (written as it is, never converted) and the Markdown source around the translated words that
    // shows what it does (converted)
    struct Row { const char* syntax; const char* key; const char* before; const char* after; };
    static const Row rows[] = {
        { "/n", "details_md_newline", "", "" },
        { "# Title", "details_md_heading", "# ", "" },
        { "**text**", "details_md_emphasis", "**", "**" },
        { "{violet}text{/}", "details_md_violet", "{violet}", "{/}" },
        { "{magenta}text{/}", "details_md_magenta", "{magenta}", "{/}" },
        { "{purple}text{/}", "details_md_purple", "{purple}", "{/}" },
        { "{blue}text{/}", "details_md_blue", "{blue}", "{/}" },
        { "- item", "details_md_list", "", "" },
    };
    auto words = [&](const char* key, const std::vector<std::string>& args) {   // (no marker character left: one span a row)
        std::string s = T(key, args);
        s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return ch == '#' || ch == '*' || ch == '{' || ch == '}'; }), s.end());
        return clean_line(s);
    };
    std::string out = markdown("# " + words("details_md_help", {}));
    for (const Row& r : rows)
        out += "\n" + clean_line(r.syntax) + "  =  " + markdown(r.before + words(r.key, {}) + r.after);
    out += "\n\n" + words("details_md_note", { std::to_string(kDescMaxUnits) });
    return out;
}

double em_for(float width_px, float font_w) {
    if (!(width_px > 0) || !(font_w > 0)) return 0;
    return (double)width_px / (double)font_w * 0.98;
}

// ---- the description edited ------------------------------------------------------------------------------------------
std::string cut_utf16(const std::string& s, size_t units) {
    size_t i = 0, used = 0;
    while (i < s.size()) {
        const size_t n = u8_len(s, i);
        const size_t u = n == 4 ? 2 : 1;
        if (used + u > units) break;
        used += u;
        i += n;
    }
    return s.substr(0, i);
}

std::string edit_text(const std::string& description) {
    std::string out;
    for (char ch : clean_text(description, kDescMaxBytes)) {
        if (ch == '\n') out += "/n";
        else out += ch;
    }
    return cut_utf16(out, kDescMaxUnits);
}

std::string edit_result(const std::string& text) {
    const std::string t = model::lutil::utf8_cut(text, kDescMaxBytes);
    std::string lines;
    for (size_t i = 0; i < t.size(); ++i) {
        if ((t[i] == '/' || t[i] == '\\') && i + 1 < t.size() && t[i + 1] == 'n') { lines += '\n'; ++i; continue; }
        lines += t[i];
    }
    return cut_utf16(clean_text(lines, kDescMaxBytes), kDescMaxUnits);
}

// ---- the key guide ------------------------------------------------------------------------------------------------------
std::string leading_combo_icons(const std::string& msg) {
    size_t p = 0;
    while (p < msg.size() && msg.compare(p, 6, "<ICON ") == 0) {
        const size_t e = msg.find('>', p);
        if (e == std::string::npos) return "";
        p = e + 1;
    }
    const std::string icons = msg.substr(0, p);
    return icons.find("<ICON +>") != std::string::npos ? icons : std::string();
}

std::string guide_entries(const Tr& T, bool pad, const std::string& tab_icons, bool more_pages, bool help_shown) {
    std::string s = std::string(pad ? "<ICON BtnD>" : "<ICON KeyF>") + " " + T("details_confirm", {});
    if (more_pages && !tab_icons.empty()) {
        std::string tabs = tab_icons;
        for (size_t p = tabs.find("<ICON +>"); p != std::string::npos; p = tabs.find("<ICON +>", p + 3)) tabs.replace(p, 8, " / ");
        s += "   " + tabs + " " + T("details_more_slots", {});
    }
    s += std::string("   ") + (pad ? "<ICON BtnU>" : "<ICON KeyT>") + " " + T("details_edit", {});
    s += std::string("   ") + (pad ? "<ICON BtnL>" : "<ICON KeyR>") + " " + T(help_shown ? "details_show_desc" : "details_md_help", {});
    return s;
}

std::vector<float> guide_layout(const std::vector<float>& x, const std::vector<float>& w, const std::vector<float>& font_w) {
    std::vector<float> out(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        if (i == 0) { out[i] = x[i]; continue; }
        const float pw = i - 1 < w.size() ? w[i - 1] : 0.0f, pf = i - 1 < font_w.size() ? font_w[i - 1] : 0.0f;
        out[i] = out[i - 1] + pw + pf;
    }
    return out;
}

// ---- the wrapped lines -------------------------------------------------------------------------------------------------
Lines line_starts(const std::string& text, const std::function<float(uint32_t g)>& baseline) {
    Lines r;
    if (text.empty()) return r;
    uint32_t g = baseline(0) == 0.0f ? 1u : 0u;
    float cur = baseline(g);
    bool any = cur != 0.0f;
    bool after_nl = false;
    for (size_t t = 0; t < text.size();) {
        if (text[t] == '<') {                    // a game tag (markdown()'s): no glyph
            const size_t e = text.find('>', t);
            if (e != std::string::npos) { t = e + 1; continue; }
        }
        const size_t n = u8_len(text, t);
        const char c = text[t];
        if (c == '\n') {
            r.starts.push_back(t + 1);
            after_nl = true;
        } else {
            const float b = baseline(g);
            if (b != 0.0f) any = true;
            if (after_nl) { cur = b; after_nl = false; ++g; }
            else if (b > cur + 1.0f) {
                cur = b;
                if (c == ' ') r.starts.push_back(t + 1);          // the space dropped at the break: no glyph
                else { r.starts.push_back(t); ++g; }
            } else ++g;
        }
        t += n;
    }
    r.laid_out = any;
    if (!any) r.starts = { 0 };
    return r;
}

int max_top(int total, int n) { return std::max(0, total - n); }

Span window(const std::vector<size_t>& starts, size_t text_size, int top, int n) {
    Span s;
    if (starts.empty()) { s.end = text_size; return s; }
    const int total = (int)starts.size();
    const int k = std::clamp(top, 0, max_top(total, n));
    s.begin = std::min(starts[(size_t)k], text_size);
    s.end = k + n < total ? std::min(starts[(size_t)(k + n)], text_size) : text_size;
    if (s.end < s.begin) s.end = s.begin;
    return s;
}

std::string window_text(const std::string& text, const std::vector<size_t>& starts, int top, int n) {
    const Span s = window(starts, text.size(), top, n);
    // the span open at a byte: the last open tag before it with no close tag after that one
    auto active = [&](size_t pos) {
        Ink k = Ink::None;
        for (size_t i = 0; i < pos && i < text.size();) {
            if (text[i] != '<') { ++i; continue; }
            const size_t e = text.find('>', i);
            if (e == std::string::npos || e >= pos) break;
            const std::string tag = text.substr(i, e - i + 1);
            if (tag == "</COLOR>" || tag == close_tag(Ink::Blue)) k = Ink::None;
            else for (Ink c : { Ink::Light, Ink::Pink, Ink::Purple, Ink::Blue }) if (tag == open_tag(c)) k = c;
            i = e + 1;
        }
        return k;
    };
    return std::string(open_tag(active(s.begin))) + text.substr(s.begin, s.end - s.begin) + close_tag(active(s.end));
}

Thumb thumb(float track_h, int total, int n, int top) {
    Thumb t;
    if (total <= n || n <= 0) { t.h = track_h; return t; }
    t.h = std::min(track_h, std::max(kThumbMin, track_h * (float)n / (float)total));
    t.y = (track_h - t.h) * (float)std::clamp(top, 0, total - n) / (float)(total - n);
    return t;
}

// ---- keys ----------------------------------------------------------------------------------------------------------------
bool HoldRepeat::step(bool down) {
    if (!down) { held = 0; return false; }
    ++held;
    return held == 1 || (held > kRepeatDelay && held % kRepeatEvery == 0);
}

// ---- the mouse wheel -------------------------------------------------------------------------------------------------------
GuiPoint client_to_gui(float x, float y, float client_w, float client_h) {
    GuiPoint p;
    if (!(client_w > 0) || !(client_h > 0)) return p;
    const float s = std::min(client_w / kGuiWidth, client_h / kGuiHeight);
    const float ox = (client_w - kGuiWidth * s) * 0.5f, oy = (client_h - kGuiHeight * s) * 0.5f;
    p.x = (x - ox) / s;
    p.y = (y - oy) / s;
    p.ok = true;
    return p;
}

Area wheel_area(float text_left, float text_top) {
    Area a;
    a.left = text_left;
    a.top = text_top;
    a.right = text_left + kDetailWidth + kBarGap + kBarWidth;
    a.bottom = text_top + kBarTopOffset + (float)kWindowLines * kLineHeight;
    return a;
}

bool contains(const Area& a, const GuiPoint& p) {
    return p.ok && p.x >= a.left && p.x <= a.right && p.y >= a.top && p.y <= a.bottom;
}

int WheelSteps::take(int delta) {
    if (delta != 0 && rest != 0 && (delta > 0) != (rest > 0)) rest = 0;   // (turned the other way: what was left is dropped)
    rest += delta;
    const int notches = rest / kWheelDelta;      // (toward zero: what is left of a notch waits, either way)
    rest -= notches * kWheelDelta;
    return -notches * kWheelLines;
}

}  // namespace ui::details
