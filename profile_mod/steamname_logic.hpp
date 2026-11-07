// profile_mod: the pure part (logic_test.cpp). No game access, no network.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace psn {

// Is a real profile what the player is on? The GUIs alone don't say: Battle Settings keeps CFNFighterProfileSimpleTop loaded, and draws
// it on its own tabs, its lines holding his id. So, read every frame:
//   - its GameObject drawn and its main control visible;
inline bool profile_drawn(bool go_draw_self, bool main_actual_visible) { return go_draw_self && main_actual_visible; }
//   - no flow of Battle Settings' at all (Wael 07/11): UIMatchingSettingFightersProfile* flows, its own tab's, are no profile either;
//   - the flow of that very screen: the full profile's param for CFNFighterProfileTop, the limited one's for CFNFighterProfileSimpleTop.
constexpr const char* kFlowFullProfile = "app.UICFNFightersProfileTop.FlowParam";
constexpr const char* kFlowLimitedProfile = "app.UICFNFightersProfileSimpleTop.FlowParam";
inline bool flow_is_battle_settings(const std::string& type) {
    return type.rfind("app.UIFlowMatchingSetting.", 0) == 0 || type.rfind("app.UIFlowMatchingFighterSetting.", 0) == 0;
}
inline bool profile_allowed(const std::vector<std::string>& flows, bool full_profile_screen) {
    const char* own = full_profile_screen ? kFlowFullProfile : kFlowLimitedProfile;
    bool found = false;
    for (const std::string& t : flows) {
        if (flow_is_battle_settings(t)) return false;
        if (t == own) found = true;
    }
    return found;
}

constexpr size_t kIdLen = 17;
constexpr size_t kNameMaxBytes = 96;        // far above Steam's 32 characters

// Where a line holds a SteamID64: a run of exactly 17 digits starting 7656, on its own (npos: none). The "94" of the line's
// <PLATMSG Arg0 = "94"> is no such run, nor a longer or a shorter number.
inline size_t find_steam_id(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        if (!isdigit(static_cast<unsigned char>(s[i]))) { ++i; continue; }
        size_t e = i;
        while (e < s.size() && isdigit(static_cast<unsigned char>(s[e]))) ++e;
        if (e - i == kIdLen && s.compare(i, 4, "7656") == 0) return i;
        i = e;
    }
    return std::string::npos;
}

// The name in a Steam profile's XML: <steamID><![CDATA[...]]></steamID>; "" when there is none
inline std::string xml_name(const std::string& xml) {
    static const std::string open = "<steamID><![CDATA[", close = "]]></steamID>";
    const size_t a = xml.find(open);
    if (a == std::string::npos) return {};
    const size_t from = a + open.size(), b = xml.find(close, from);
    return b == std::string::npos ? std::string() : xml.substr(from, b - from);
}

// HTML entities decoded: &amp; &lt; &gt; &quot; &#39; &apos; and numeric ones. A number that is no character is dropped, anything else
// stays as it is. One pass: "&amp;lt;" gives "&lt;". What it gives still goes through sanitize.
inline std::string decode_entities(const std::string& s) {
    auto utf8 = [](uint32_t cp, std::string& out) {
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    };
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const size_t semi = s[i] == '&' ? s.find(';', i) : std::string::npos;
        if (semi == std::string::npos || semi - i > 24) { out += s[i++]; continue; }
        const std::string e = s.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (e.size() >= 2 && e[0] == '#') {
            const bool hex = e[1] == 'x' || e[1] == 'X';
            uint64_t cp = 0;
            bool ok = e.size() > (hex ? 2u : 1u);
            for (size_t k = hex ? 2 : 1; ok && k < e.size(); ++k) {
                const unsigned char c = static_cast<unsigned char>(e[k]);
                if (!(hex ? isxdigit(c) : isdigit(c))) { ok = false; break; }
                cp = cp * (hex ? 16 : 10) + static_cast<uint64_t>(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
                if (cp > 0x10FFFF) cp = 0x110000;       // stays too big
            }
            if (!ok) { out += s[i++]; continue; }
            utf8(static_cast<uint32_t>(cp), out);
        } else { out += s[i++]; continue; }
        i = semi + 1;
    }
    return out;
}

// The name in a profile's HTML page (raw, its entities not decoded): <span class="actual_persona_name">, else the tab's title
// "Steam Community :: <name>". "" on Steam's error page, whose title is "Steam Community :: Error": its generic text is never a name.
inline std::string html_name_raw(const std::string& html) {
    if (html.find("could not be found") != std::string::npos || html.find("error_ctn") != std::string::npos ||
        html.find("profile_fatalerror") != std::string::npos) return {};
    static const std::string span = "<span class=\"actual_persona_name\">", title = "<title>Steam Community :: ";
    if (const size_t a = html.find(span); a != std::string::npos) {
        const size_t from = a + span.size(), b = html.find("</span>", from);
        return b == std::string::npos ? std::string() : html.substr(from, b - from);
    }
    if (const size_t a = html.find(title); a != std::string::npos) {
        const size_t from = a + title.size(), b = html.find("</title>", from);
        if (b == std::string::npos) return {};
        const std::string t = html.substr(from, b - from);
        return t == "Error" ? std::string() : t;
    }
    return {};
}

// A stranger's text, before it goes into a game text: valid UTF-8 only, no control characters (C0, DEL, C1), no line or
// paragraph separator, no bidirectional control, no < or > (message tags), no space at its ends, at most kNameMaxBytes
inline std::string sanitize(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const int n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        if (!n || i + static_cast<size_t>(n) > in.size()) { ++i; continue; }         // a stray or cut byte: dropped
        uint32_t cp = n == 1 ? c : c & (0xFFu >> (n + 1));
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            const unsigned char t = static_cast<unsigned char>(in[i + static_cast<size_t>(k)]);
            if ((t & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (t & 0x3Fu);
        }
        if (!ok) { ++i; continue; }
        const bool overlong = (n == 3 && cp < 0x800) || (n == 4 && cp < 0x10000);
        const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
        const bool control = cp < 0x20 || (cp >= 0x7F && cp <= 0x9F);
        const bool layout = cp == 0x2028 || cp == 0x2029 || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200E || cp == 0x200F;
        if (!overlong && !surrogate && !control && !layout && cp != '<' && cp != '>' && cp <= 0x10FFFF) out.append(in, i, static_cast<size_t>(n));
        i += static_cast<size_t>(n);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t first = 0;
    while (first < out.size() && out[first] == ' ') ++first;
    out.erase(0, first);
    if (out.size() > kNameMaxBytes) {
        size_t cut = kNameMaxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out.resize(cut);
    }
    return out;
}

// What the two requests gave: a name; no name (the pages answered, there is none: not asked again this session); a network failure
// (no answer: the next press may try again). The page is asked for only when the XML answered with no name.
enum class NameResult { Name, NoName, NetFail };
struct NameAnswer { NameResult result = NameResult::NetFail; std::string name; };
inline std::string name_from_xml(const std::string& xml) { return sanitize(xml_name(xml)); }
inline std::string name_from_page(const std::string& html) { return sanitize(decode_entities(html_name_raw(html))); }
inline bool needs_page(bool xml_ok, const std::string& xml) { return xml_ok && name_from_xml(xml).empty(); }
inline NameAnswer name_answer(bool xml_ok, const std::string& xml, bool page_ok, const std::string& page) {
    NameAnswer a;
    if (!xml_ok) return a;
    a.name = name_from_xml(xml);
    if (a.name.empty() && !page_ok) return a;
    if (a.name.empty()) a.name = name_from_page(page);
    a.result = a.name.empty() ? NameResult::NoName : NameResult::Name;
    return a;
}

// `keep` bytes of a name, cut on a UTF-8 boundary, "..." after them; the whole name when it fits
inline std::string cut_name(const std::string& name, size_t keep) {
    if (keep >= name.size()) return name;
    size_t cut = keep;
    while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
    return name.substr(0, cut) + "...";
}

// The game's line with the name in place of the id
inline std::string line_with_name(const std::string& line, size_t id_at, const std::string& name) {
    return line.substr(0, id_at) + name + line.substr(id_at + kIdLen);
}

// What a line shows, without its tags: the bytes that take room
inline size_t shown_bytes(const std::string& line) {
    size_t n = 0;
    bool tag = false;
    for (const char c : line) {
        if (c == '<') tag = true;
        else if (c == '>') tag = false;
        else if (!tag) ++n;
    }
    return n;
}
// The line drew `drawn` wide in a region of `region`: the name's bytes to keep next, in proportion, always fewer, never under 4
inline size_t next_keep(size_t keep, size_t line_bytes, float drawn, float region) {
    if (drawn <= region + 0.5f || keep <= 4 || drawn <= 0 || line_bytes == 0) return keep;
    const size_t drop = static_cast<size_t>((drawn - region) / drawn * static_cast<float>(line_bytes)) + 4;   // + "..."
    return drop + 4 >= keep ? 4 : keep - drop;
}

// The keyboard key of a game symbol ("<ICON KeyR>", named by its QWERTY place): its scan code, 0 none
inline unsigned qwerty_scan(const std::string& symbol) {
    const std::string tag = "<ICON Key";
    const size_t at = symbol.find(tag);
    if (at == std::string::npos || at + tag.size() + 1 >= symbol.size() || symbol[at + tag.size() + 1] != '>') return 0;
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(symbol[at + tag.size()])));
    static const char* rows[] = { "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM" };
    static const unsigned base[] = { 0x10, 0x1E, 0x2C };
    for (int r = 0; r < 3; ++r)
        for (const char* k = rows[r]; *k; ++k)
            if (*k == c) return base[r] + static_cast<unsigned>(k - rows[r]);
    if (c >= '1' && c <= '9') return 0x02 + static_cast<unsigned>(c - '1');
    if (c == '0') return 0x0B;
    return 0;
}

// The guide bar's words, by the game's language
struct Words { const char* lang; const char* name; const char* id; const char* unavailable; };
inline constexpr Words kWords[] = {
    { "en",      "Steam Name",      "Steam ID",    "Name unavailable" },
    { "fr",      "Pseudo Steam",    "ID Steam",    "Pseudo indisponible" },
    { "ja",      "Steam名",         "Steam ID",    "名前を取得できません" },
    { "zh-Hans", "Steam昵称",       "Steam ID",    "无法获取名称" },
    { "it",      "Nome Steam",      "ID Steam",    "Nome non disponibile" },
    { "de",      "Steam-Name",      "Steam-ID",    "Name nicht verfügbar" },
    { "es",      "Nombre de Steam", "ID de Steam", "Nombre no disponible" },
    { "es-419",  "Nombre de Steam", "ID de Steam", "Nombre no disponible" },
    { "ru",      "Имя в Steam",     "Steam ID",    "Имя недоступно" },
    { "pl",      "Nazwa Steam",     "ID Steam",    "Nazwa niedostępna" },
    { "pt",      "Nome na Steam",   "ID Steam",    "Nome indisponível" },
    { "ko",      "Steam 이름",      "Steam ID",    "이름을 확인할 수 없음" },
    { "zh-Hant", "Steam暱稱",       "Steam ID",    "無法取得名稱" },
    { "ar",      "اسم Steam",       "معرّف Steam", "الاسم غير متاح" },
};
inline const Words& words_of(const std::string& lang) {
    for (const Words& w : kWords) if (lang == w.lang) return w;
    return kWords[0];
}
inline const char* word_name(const std::string& lang) { return words_of(lang).name; }
inline const char* word_id(const std::string& lang) { return words_of(lang).id; }
inline const char* word_unavailable(const std::string& lang) { return words_of(lang).unavailable; }

}  // namespace psn
