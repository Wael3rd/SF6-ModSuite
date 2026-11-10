// SF6 Slots -- the pure part of the live texture overrides (ui_retexture.cpp): the overrides' text, paths as they are matched,
// what each element on screen needs, and what stays referenced. No game access here: tested by plugin/tests/ui_retexture_test.cpp.
#pragma once

#include <climits>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace slots::retex {

// ---------------------------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------------------------

inline bool starts_ci(std::string_view s, std::string_view head) {
    if (s.size() < head.size()) return false;
    for (size_t i = 0; i < head.size(); ++i) {
        char a = s[i], b = head[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

// A resource path as the loader or a hand writes it, as the game is asked for it: '/' between folders, without "natives/<platform>/"
// ahead ("natives/stm/") nor the version number a packed file carries after its extension ("x.tex.241101895" -> "x.tex"), nor spaces
// around. Case kept.
inline std::string clean_path(std::string_view p) {
    std::string s(trim(p));
    for (auto& c : s) if (c == '\\') c = '/';
    size_t lead = 0;
    while (lead < s.size() && s[lead] == '/') ++lead;
    s.erase(0, lead);
    if (starts_ci(s, "natives/")) {
        const size_t slash = s.find('/', 8);
        if (slash != std::string::npos) s.erase(0, slash + 1);
    }
    const size_t name = s.find_last_of('/') + 1;                 // (npos + 1 == 0: no folder)
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot >= name && dot + 1 < s.size() && s.find('.', name) < dot) {
        bool digits = true;
        for (size_t i = dot + 1; i < s.size(); ++i) digits = digits && s[i] >= '0' && s[i] <= '9';
        if (digits) s.resize(dot);
    }
    return s;
}
inline std::string lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
// How two paths are compared: cleaned, in lower case (the engine's paths do not depend on case; get_ResourcePath gives back the
// game's own spelling, "product/gui/data/Character_Image/001/caracterselect/tex_Esf001_face_L_s_IAM.tex")
inline std::string key_of(std::string_view p) { return lower(clean_path(p)); }

// ---------------------------------------------------------------------------------------------
// The overrides' text: lines "<real resource path>\t<resource path to show instead>" (the loader's SF6UISlots_Overrides)
// ---------------------------------------------------------------------------------------------

struct Parsed {
    std::map<std::string, std::string> map;      // real key -> target key
    std::map<std::string, std::string> paths;    // target key -> its path as asked of the game (clean_path)
    int lines = 0;                               // lines with something on them (a '#' line is a note: not counted)
    int bad = 0;                                 // without a tab, or a side empty
    int same = 0;                                // a file shown instead of itself: nothing to do
    int twice = 0;                               // a real file listed again: the last line counts
};
inline Parsed parse(std::string_view text) {
    Parsed p;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string_view::npos) e = text.size();
        const std::string_view line = trim(text.substr(i, e - i));
        i = e + 1;
        if (line.empty() || line.front() == '#') continue;
        ++p.lines;
        const size_t tab = line.find('\t');
        if (tab == std::string_view::npos) { ++p.bad; continue; }
        const std::string real = key_of(line.substr(0, tab));
        const std::string shown = clean_path(line.substr(tab + 1));
        const std::string target = lower(shown);
        if (real.empty() || target.empty() || target.find('\t') != std::string::npos) { ++p.bad; continue; }
        if (real == target) { ++p.same; continue; }
        if (!p.map.insert_or_assign(real, target).second) ++p.twice;
        p.paths[target] = shown;
    }
    // a target only a line listed again named: not wanted
    std::unordered_set<std::string> used;
    for (const auto& kv : p.map) used.insert(kv.second);
    for (auto it = p.paths.begin(); it != p.paths.end();) it = used.count(it->first) ? std::next(it) : p.paths.erase(it);
    return p;
}

// The loader's text through fn(buf, cap): its length (strlen) is returned, at most cap - 1 bytes written and a 0; a length >= cap
// says how much room it needs: asked once more with that room. kFault: fn faulted (the caller's SEH guard).
constexpr int kFault = INT_MIN;
enum ReadResult : int { ReadNotRunning = -1, ReadTooLong = -2, ReadFaulted = -3 };
template <class Fn>
int read_export(Fn&& fn, std::string& out, int first_cap = 1 << 16) {
    out.clear();
    if (first_cap < 2) first_cap = 2;
    std::vector<char> buf(static_cast<size_t>(first_cap), '\0');
    int n = fn(buf.data(), first_cap);
    if (n == kFault) return ReadFaulted;
    if (n < 0) return ReadNotRunning;
    if (n >= first_cap) {
        if (n > (1 << 28)) return ReadTooLong;
        const int cap = n + 1;
        buf.assign(static_cast<size_t>(cap), '\0');
        n = fn(buf.data(), cap);
        if (n == kFault) return ReadFaulted;
        if (n < 0) return ReadNotRunning;
        if (n >= cap) return ReadTooLong;                     // grew again between the two calls: the next refresh reads it
    }
    out.assign(buf.data(), strnlen(buf.data(), static_cast<size_t>(n)));
    return static_cast<int>(out.size());
}

// ---------------------------------------------------------------------------------------------
// Bookkeeping: the overrides, the elements changed (E: an element's identity, never used to reach it), the holders (H)
// ---------------------------------------------------------------------------------------------

enum class Step { None, Apply, Keep, Swap, Restore };

template <class E, class H>
struct Book {
    struct Rec {
        std::string real;          // the element's own file (key); "" when adopted without one
        std::string target;        // the file we made it show (key)
        H orig{};                  // the game's own holder it showed, one reference held by us ({}: none, never put back)
        H ours{};                  // our holder we gave it
        uint64_t ours_res = 0;     // its resource (an element may give back another holder of it)
        uint32_t at = 0;           // frame it was given ours
    };
    std::map<std::string, std::string> map;          // real key -> target key
    std::map<std::string, int> map_targets;          // target key -> how many real files show it
    std::unordered_map<E, Rec> recs;
    std::map<std::string, int> users;                // target key -> elements of ours that show it
    std::unordered_map<E, int> fights;               // an element the game put back to its own file soon after we changed it
    std::unordered_set<E> contested;                 // ... three times running: left alone until the overrides change

    void set_map(std::map<std::string, std::string> m) {
        map = std::move(m);
        map_targets.clear();
        for (const auto& kv : map) ++map_targets[kv.second];
        fights.clear();
        contested.clear();
    }
    // The real key as kept in the map (stable while the map is), nullptr when not overridden
    const std::string* real_key(const std::string& key) const {
        auto it = map.find(key);
        return it == map.end() ? nullptr : &it->first;
    }
    const std::string* target_of(const std::string& real) const {
        auto it = map.find(real);
        return it == map.end() ? nullptr : &it->second;
    }
    // An element we changed that still shows our holder: Keep, Swap (*to: the new target) or Restore (the game's own back)
    Step recorded(const Rec& r, const std::string** to) const {
        auto it = map.find(r.real);
        if (it == map.end()) return Step::Restore;
        if (to) *to = &it->second;
        return it->second == r.target ? Step::Keep : Step::Swap;
    }
    // An element not ours showing `cur` (key): Apply (*to: the target) or None
    Step fresh(const std::string& cur, const std::string** to) const {
        auto it = map.find(cur);
        if (it == map.end()) return Step::None;
        if (to) *to = &it->second;
        return Step::Apply;
    }
    void add(E e, const std::string& real, const std::string& target, H orig, H ours, uint64_t ours_res, uint32_t at) {
        take(e);
        recs[e] = Rec{ real, target, orig, ours, ours_res, at };
        ++users[target];
    }
    void retarget(E e, const std::string& target, H ours, uint64_t ours_res, uint32_t at) {
        auto it = recs.find(e);
        if (it == recs.end()) return;
        unuse(it->second.target);
        it->second.target = target;
        it->second.ours = ours;
        it->second.ours_res = ours_res;
        it->second.at = at;
        ++users[target];
    }
    // The record taken out (its orig is the caller's to release); a default Rec when there was none
    Rec take(E e) {
        auto it = recs.find(e);
        if (it == recs.end()) return Rec{};
        Rec r = std::move(it->second);
        recs.erase(it);
        unuse(r.target);
        return r;
    }
    // A target's holder is kept while a real file is overridden by it or an element of ours may show it
    bool needed(const std::string& target) const { return map_targets.count(target) != 0 || users.count(target) != 0; }
    // The game gave an element of ours back its own file (same_real) within `window` frames of our change: after `max` times running
    // the element is contested (true). Anything else ends the run.
    bool put_back(E e, bool same_real, uint32_t now, uint32_t at, uint32_t window, int max) {
        if (!same_real || now - at > window) { fights.erase(e); return false; }
        if (++fights[e] < max) return false;
        fights.erase(e);
        contested.insert(e);
        return true;
    }
    bool is_contested(E e) const { return contested.count(e) != 0; }

private:
    void unuse(const std::string& target) {
        auto u = users.find(target);
        if (u != users.end() && --u->second <= 0) users.erase(u);
    }
};

// ---------------------------------------------------------------------------------------------
// The scene's GUIs between two looks: which are new or newly shown (walked at once and a little later)
// ---------------------------------------------------------------------------------------------

struct GuiNow { uintptr_t id = 0; uintptr_t view = 0; bool shown = false; };
struct GuiSeen { uintptr_t view = 0; bool shown = false; uint32_t look = 0; };
// The GUIs to walk as fresh: listed for the first time while shown, shown again, or shown with another View than last time (an
// address reused by a new GUI). The ones no longer listed are forgotten. `first`: the first look (everything is only recorded: the
// pass over every GUI made when the overrides changed covers them).
inline std::vector<uintptr_t> diff_guis(std::unordered_map<uintptr_t, GuiSeen>& seen, const std::vector<GuiNow>& now, uint32_t look,
                                        bool first) {
    std::vector<uintptr_t> fresh;
    for (const GuiNow& g : now) {
        auto it = seen.find(g.id);
        const bool known = it != seen.end();
        if (!first && g.shown && (!known || !it->second.shown || it->second.view != g.view)) fresh.push_back(g.id);
        GuiSeen& s = known ? it->second : seen[g.id];
        s.view = g.view;
        s.shown = g.shown;
        s.look = look;
    }
    for (auto it = seen.begin(); it != seen.end();) it = it->second.look != look ? seen.erase(it) : std::next(it);
    return fresh;
}

// ---------------------------------------------------------------------------------------------
// References given back later: a holder no longer shown by an element of ours may still be drawn by the frame in flight
// ---------------------------------------------------------------------------------------------

template <class H>
struct Later {
    std::vector<std::pair<uint32_t, H>> items;       // (frame due, holder)
    void add(H h, uint32_t due) { if (h) items.push_back({ due, h }); }
    template <class F>
    size_t run(uint32_t now, F&& release) {
        size_t n = 0;
        for (auto it = items.begin(); it != items.end();) {
            if (static_cast<int32_t>(now - it->first) < 0) { ++it; continue; }
            release(it->second);
            it = items.erase(it);
            ++n;
        }
        return n;
    }
    bool empty() const { return items.empty(); }
};

}  // namespace slots::retex
