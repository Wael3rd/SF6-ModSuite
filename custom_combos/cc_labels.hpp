// cc_labels.hpp - Custom Combos: the label tables (_labels/<Fighter>.json, _actions/<Fighter>.json) and the tag text of a
// recipe line, no game dependency.
//
// The native recipe draws a line from the game's own tag text: <CMD _236> (a motion), <ICON 2> (a direction), <ICON +>,
// <ICON lp> (a button), <ICON next> (the arrow of a target combo). A skill's Classic command text (FGAthleteCommand)
// names its button by family and level, <CMDBTN CmnP|LowP|MidP|HighP|...>: the level becomes the strength played.
// What the game itself writes (read from its rows): "<ICON 2><ICON +><ICON mk>M" (the icon, then the level's letter),
// OD "<CMD _214><ICON +><ICON p><ICON p>", any strength "<ICON k>", the two-button moves "<ICON mp>M<ICON mk>M".
// Modern: "<ICON 2><ICON +><ICON sm>" (the SP button), a super "<ICON 2><ICON +><ICON sm><ICON hs>H", an OD special
// "<ICON 4><ICON +><ICON auto><ICON hd><ICON +><ICON sm>" (Assist held), Drive Parry "<ICON dp>".
//
// The recorder (part 2) reads the same tables: `raw` keeps the whole file for it (actions, modern, assist, chains).
#pragma once

#include "cc_util.hpp"

#include <array>
#include <map>
#include <string_view>

namespace cc {

struct Labels {
    bool loaded = false;                        // a _labels file was read
    json raw;                                   // the _labels file, as read (part 2: actions, modern, assist, chains)
    std::map<std::string, std::pair<std::string, std::string>> skills;   // skill id -> { cmd (Classic), m (Modern) }
    std::array<int, 3> slot_classic{}, slot_modern{};
    bool has_slots = false;
    json groups = json::object();               // _actions/<Fighter>.json "groups": notation -> action ids
};

inline Labels load_labels(const fs::path& data_dir, const std::string& fighter_dir) {
    Labels L;
    json raw = read_json(data_dir / "_labels" / (fighter_dir + ".json"));
    if (raw.is_object()) {
        L.loaded = true;
        if (const json* sk = obj(&raw, "skills"))
            for (auto it = sk->begin(); it != sk->end(); ++it)
                if (it->is_object()) L.skills[it.key()] = { str_of(at(*it, "cmd")), str_of(at(*it, "m")) };
        if (const json* s = obj(&raw, "slots")) {
            const json* c = arr(s, "classic");
            if (c) {
                L.has_slots = true;
                const json* m = arr(s, "modern");
                for (size_t i = 0; i < 3; ++i) {
                    const int cv = static_cast<int>(inum(i < c->size() ? &(*c)[i] : nullptr, 0));
                    L.slot_classic[i] = cv;
                    L.slot_modern[i] = m ? static_cast<int>(inum(i < m->size() ? &(*m)[i] : nullptr, 0)) : cv;
                }
            }
        }
        L.raw = std::move(raw);
    }
    json g = read_json(data_dir / "_actions" / (fighter_dir + ".json"));
    if (const json* gr = obj(&g, "groups")) L.groups = *gr;
    return L;
}

namespace text {

inline bool sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
inline bool alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
inline bool digit(char c) { return c >= '0' && c <= '9'; }
inline bool upper(char c) { return c >= 'A' && c <= 'Z'; }

// Lua %b(): the index past the ')' that closes the '(' at i, npos when it is never closed
inline size_t balanced(const std::string& s, size_t i) {
    int depth = 0;
    for (size_t k = i; k < s.size(); ++k) {
        if (s[k] == '(') ++depth;
        else if (s[k] == ')' && --depth == 0) return k + 1;
    }
    return std::string::npos;
}

// gsub("<.->", ""): every tag out
inline std::string no_tags(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '<') {
            const size_t e = s.find('>', i + 1);
            if (e != std::string::npos) { i = e + 1; continue; }
        }
        out += s[i++];
    }
    return out;
}

// gsub("%s*(%b())%s*", ...): the notes in parentheses go (more than 6 letters once the tags are out), the grouped
// inputs stay: "(<ICON 5> or <ICON 6>)" has no sentence in it
inline std::string drop_notes(const std::string& s) {
    std::string out;
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && sp(s[j])) ++j;
        if (j < n && s[j] == '(') {
            const size_t k = balanced(s, j);
            if (k != std::string::npos) {
                size_t m = k;
                while (m < n && sp(s[m])) ++m;
                int letters = 0;
                for (char c : no_tags(s.substr(j, k - j))) if (alpha(c)) ++letters;
                if (letters <= 6) out.append(s, i, m - i);
                i = m;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// "<ICON 5> or <ICON 6><ICON +>..." keeps its first way; "<ICON 6><ICON 6> or (...)+..." stops before the "or"
inline std::string one_way(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {                 // gsub("%s+or%s+%(.*$", "")
        if (!sp(s[i])) continue;
        size_t j = i;
        while (j < s.size() && sp(s[j])) ++j;
        if (s.compare(j, 2, "or") != 0) { i = j - 1; continue; }
        size_t k = j + 2;
        while (k < s.size() && sp(s[k])) ++k;
        if (k > j + 2 && k < s.size() && s[k] == '(') { s.resize(i); break; }
        i = j - 1;
    }
    auto tag_end = [&](size_t i) -> size_t {                 // <[^>]+> at i: the index past it, npos when none
        if (i >= s.size() || s[i] != '<') return std::string::npos;
        size_t k = i + 1;
        while (k < s.size() && s[k] != '>') ++k;
        return (k < s.size() && k > i + 1) ? k + 1 : std::string::npos;
    };
    std::string out;                                         // gsub("(<[^>]+>)%s+or%s+<[^>]+>", "%1")
    for (size_t i = 0; i < s.size();) {
        const size_t a = tag_end(i);
        if (a != std::string::npos) {
            size_t j = a;
            while (j < s.size() && sp(s[j])) ++j;
            if (j > a && s.compare(j, 2, "or") == 0) {
                size_t k = j + 2;
                while (k < s.size() && sp(s[k])) ++k;
                const size_t b = k > j + 2 ? tag_end(k) : std::string::npos;
                if (b != std::string::npos) { out.append(s, i, a - i); i = b; continue; }
            }
        }
        out += s[i++];
    }
    return out;
}

// gsub("<CMDBTN (%a+)>", fn): fn returns the replacement, or nullopt to keep the tag as it is
template <typename F> std::string each_cmdbtn(const std::string& s, F fn) {
    static constexpr std::string_view kTag = "<CMDBTN ";
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s.compare(i, kTag.size(), kTag) == 0) {
            size_t k = i + kTag.size();
            while (k < s.size() && alpha(s[k])) ++k;
            if (k > i + kTag.size() && k < s.size() && s[k] == '>') {
                const std::optional<std::string> r = fn(s.substr(i + kTag.size(), k - i - kTag.size()));
                if (r) out += *r; else out.append(s, i, k + 1 - i);
                i = k + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

inline const char* pair_tag(const std::string& name) {
    if (name == "DriveP") return "<ICON mp>M<ICON mk>M";
    if (name == "DriveI") return "<ICON hp>H<ICON hk>H";
    if (name == "Throw") return "<ICON lp>L<ICON lk>L";
    return nullptr;
}

inline std::optional<std::string> button_tag(const std::string& level, char fam_c, int strength) {
    const std::string fam(1, static_cast<char>(fam_c == 'P' ? 'p' : 'k'));
    std::string lv = level == "Low" ? "l" : level == "Mid" ? "m" : level == "High" ? "h" : "";
    if (lv.empty() && level == "Cmn") {
        if (strength == 7) return "<ICON " + fam + "><ICON " + fam + ">";
        lv = strength == 1 ? "l" : strength == 2 ? "m" : strength == 3 ? "h" : "";
        if (lv.empty()) return "<ICON " + fam + ">";
    }
    if (lv.empty()) return std::nullopt;
    std::string up = lv;
    up[0] = static_cast<char>(up[0] - 'a' + 'A');
    return "<ICON " + lv + fam + ">" + up;
}

inline std::optional<std::string> skill_text(const Labels& L, int64_t skill, int strength) {
    auto it = L.skills.find(std::to_string(skill));
    if (it == L.skills.end() || it->second.first.empty()) return std::nullopt;
    std::string cmd = one_way(drop_notes(it->second.first));
    return each_cmdbtn(cmd, [&](const std::string& name) -> std::optional<std::string> {
        if (const char* p = pair_tag(name)) return std::string(p);
        const char last = name.back();
        std::optional<std::string> r;
        if (last == 'P' || last == 'K') r = button_tag(name.substr(0, name.size() - 1), last, strength);
        return r ? r : std::optional<std::string>("<CMDBTN " + name + ">");
    });
}

inline const char* mbtn(const std::string& n) {
    static const std::pair<const char*, const char*> k[] = {
        { "LowS", "<ICON ls>L" }, { "MidS", "<ICON ms>M" }, { "HighS", "<ICON hs>H" }, { "SM", "<ICON sm>" },
        { "SA", "<ICON sm><ICON hs>H" }, { "CmnS", "<ICON s>" }, { "CmnSS", "<ICON s><ICON s>" },
        { "CmnSSS", "<ICON s><ICON s><ICON s>" }, { "Auto", "<ICON auto>" }, { "DriveP", "<ICON dp>" } };
    for (const auto& e : k) if (n == e.first) return e.second;
    return nullptr;
}

inline std::optional<std::string> skill_text_m(const Labels& L, int64_t skill, int strength) {
    auto it = L.skills.find(std::to_string(skill));
    if (it == L.skills.end() || it->second.second.empty()) return std::nullopt;
    std::string cmd = one_way(drop_notes(it->second.second));
    bool unknown = false;
    std::string out = each_cmdbtn(cmd, [&](const std::string& name) -> std::optional<std::string> {
        if (strength == 7 && name == "SM") return std::string("<ICON auto><ICON hd><ICON +><ICON sm>");
        if (strength == 7 && name == "CmnS") return std::string("<ICON s><ICON s>");
        const char* m = mbtn(name);
        if (!m) { unknown = true; return std::nullopt; }
        return std::string(m);
    });
    if (unknown) return std::nullopt;                       // a button we never saw the game write: its own row is safer
    return out;
}

inline const char* note_btn(const std::string& b) {
    static const std::pair<const char*, const char*> k[] = {
        { "LP", "lp>L" }, { "MP", "mp>M" }, { "HP", "hp>H" }, { "LK", "lk>L" }, { "MK", "mk>M" }, { "HK", "hk>H" },
        { "P", "p>" }, { "K", "k>" }, { "PP", "p><ICON p>" }, { "KK", "k><ICON k>" } };
    for (const auto& e : k) if (b == e.first) return e.second;
    return nullptr;
}

inline std::optional<std::string> note_word(const std::string& n) {
    if (n == "DRC" || n == "RAW DR") return std::string("<ICON 6><ICON 6>");
    if (n == "DP") return std::string(pair_tag("DriveP"));
    if (n == "DI") return std::string(pair_tag("DriveI"));
    if (n == "Throw") return std::string(pair_tag("Throw"));
    if (n == "4+THROW") return "<ICON 4><ICON +>" + std::string(pair_tag("Throw"));
    if (n == "6+DI") return "<ICON 6><ICON +>" + std::string(pair_tag("DriveI"));
    return std::nullopt;
}

inline std::string note_text(const std::string& note) {
    if (auto w = note_word(note)) return *w;
    std::string n;
    for (char c : note) if (!sp(c)) n += c;
    if (n.size() > 1 && n[0] == '>') {
        const std::string chain = n.substr(1);
        const char* b = note_btn(chain);
        return b ? "<ICON next><ICON " + std::string(b) : ">" + chain;
    }
    const bool air = n.size() > 2 && n[0] == 'j' && n[1] == '.';
    if (air) n = n.substr(2);
    size_t i = 0;
    while (i < n.size() && digit(n[i])) ++i;
    const std::string dirs = n.substr(0, i);
    size_t b = i < n.size() && n[i] == '+' ? i + 1 : i;
    bool caps = b < n.size();
    for (size_t k = b; k < n.size(); ++k) if (!upper(n[k])) caps = false;
    std::string out = air ? "<ICON 8>" : "";
    if (caps) {
        if (const char* nb = note_btn(n.substr(b))) {
            if (dirs.size() > 1) out += "<CMD _" + dirs + ">";
            else if (dirs.size() == 1) out += "<ICON " + dirs + ">";
            if (!dirs.empty()) out += "<ICON +>";
            out += "<ICON " + std::string(nb);
            return out;
        }
    }
    bool all_digits = !n.empty();
    for (char c : n) if (!digit(c)) all_digits = false;
    if (all_digits) {
        for (char c : n) out += "<ICON " + std::string(1, c) + ">";
        return out;
    }
    return note;
}

}  // namespace text

// The tag text of a line from its notation, skill and strength (step_text of the recorder)
inline std::string step_text(const Labels& L, const std::string& notation, int64_t skill_id, int strength, bool modern) {
    if (modern && skill_id != 0)
        if (auto t = text::skill_text_m(L, skill_id, strength)) return *t;
    if (!notation.empty() && notation[0] == '>') return text::note_text(notation);   // the next hit of a target combo
    if (auto w = text::note_word(notation)) return *w;                               // throws, Drive moves
    if (skill_id != 0)
        if (auto t = text::skill_text(L, skill_id, strength)) return *t;
    return text::note_text(notation);
}

// Does this skill have a command in that control type? A line whose skill has none is drawn blank by the game (a
// Classic normal in a Modern session, an Assisted Combo in a Classic one). Unknown skills are left to the game.
inline bool has_cmd(const Labels& L, int64_t sid, bool modern) {
    auto it = L.skills.find(std::to_string(sid));
    if (it == L.skills.end()) return true;
    if (modern) {
        if (it->second.second.empty()) return false;
        return text::skill_text_m(L, sid, 0).has_value();
    }
    return !it->second.first.empty();
}

}  // namespace cc
