// SF6 Slots — the Random outfit: what each draw takes. Pure logic, no game access (plugin/tests); the game side is costumes.cpp.
//
// The loader gives every character one more outfit, "Random" (registry.json possession "random": true): the last of its
// Costume row, a copy of Outfit 1 (its preview is Outfit 1), with one colour, which the Color row shows as Random too. Random
// is never worn as such: where the outfit worn is decided, a draw takes its place.
//   - A character's pool: the outfits its Costume row lists (the game's own the player has and the Slots Loader's the player
//     sees), Random left out, never an NSFW outfit while those are hidden (left out when the list is read, checked again at
//     each draw). A draw takes one the pool has not given in this round; once every one was given, a new round starts (never
//     with the last one drawn, when there are two or more).
//   - The colour: the same rule among the colours the drawn outfit's Color row lists (the player's own) and the Color Mod's RW /
//     MC entries of that outfit (when the Color Mod can dress them in a battle), one round per outfit. An entry rides on colour
//     02 (1): the game wears 02, the Color Mod dresses the entry on this PC (its battle requests); online the opponent sees 02.
//   - A Random colour of any outfit (the Color Mod's Random value in its Color row, SF6ColorMod_RandomChoice): the same colour
//     draw for the outfit chosen, the same round as the Random outfit's draws of it. Battle Settings keeps its next one
//     (next_colour), as it keeps the Random outfit's next draw.
//   - Local modes (the outfit is picked on a select screen): a draw at each battle load.
//   - Online, rooms, Battle Hub (Battle Settings' choice): the next draw is made before matchmaking. A game outfit goes in the
//     save, which the fighter profile sends (the opponent sees it); a Slots Loader outfit leaves the carrier there and the alias
//     wears it on this PC only. A match against a new opponent (the first load of its fighters' files) takes the next draw for
//     the whole set (rematches keep it) and makes the one after (bind_set: a game outfit no profile carried since its draw is
//     worn on this PC only, as a Slots Loader outfit).
// The rounds and the next draw are kept in state.json ("random"): they go on after a restart.
#pragma once

#include "json.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace slots::rnd {

// One round of draws: what it gave so far (in order) and the last value drawn (kept across rounds)
struct Pool {
    std::vector<int> drawn;
    int last = -1;
};

// An outfit and its colour (costume -1: none)
struct Draw {
    int costume = -1, color = -1;
    bool valid() const { return costume >= 0; }
    bool operator==(const Draw& o) const { return costume == o.costume && color == o.color; }
};

// A character's rounds and its next draw for Battle Settings' Random
struct Fighter {
    Pool outfits;
    std::map<int, Pool> colours;   // per outfit drawn
    Draw next;
    Draw next_colour;              // Battle Settings' Random colour: its outfit and its next colour (none: not chosen)
};

// ---- the colours a draw takes: the game's numbers (0 = Color 1...) and the Color Mod's RW / MC entries ------------------
// An entry is drawn as a number of its own, from its id (the entry file's path): the same file gives the same number whatever
// the Color Mod's order (a new pack renumbers the labels, not the files), so the rounds kept in state.json stay right.
constexpr int kEntryMin = 1000;    // at or above: an entry
constexpr int kEntryCarrier = 1;   // the game's colour an entry rides on (02)
inline bool is_entry(int v) { return v >= kEntryMin; }
inline int entry_value(const std::string& id) {
    uint32_t h = 2166136261u;                               // FNV-1a
    for (unsigned char ch : id) { h ^= ch; h *= 16777619u; }
    return kEntryMin + static_cast<int>(h % (0x7FFFFFFFu - static_cast<uint32_t>(kEntryMin)));
}
// The colour number the game wears for a value drawn
inline int worn_colour(int v) { return is_entry(v) ? kEntryCarrier : v; }

// A source of random numbers (costumes.cpp: a Mersenne Twister; the tests: a fixed sequence)
using Rng = std::function<uint32_t()>;
using Allowed = std::function<bool(int)>;

inline bool has(const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// One of `candidates` that `allowed` accepts (none given: all) and the pool has not given in this round; when none is left a
// new round starts, without the last one drawn while another can be. What the round gave that is no longer a candidate is
// forgotten. -1: no candidate is allowed (the pool is left as it was).
inline int draw(Pool& p, const std::vector<int>& candidates, const Allowed& allowed, const Rng& rng) {
    std::vector<int> ok;
    for (int c : candidates)
        if ((!allowed || allowed(c)) && !has(ok, c)) ok.push_back(c);
    if (ok.empty()) return -1;
    p.drawn.erase(std::remove_if(p.drawn.begin(), p.drawn.end(), [&](int d) { return !has(ok, d); }), p.drawn.end());
    std::vector<int> left;
    for (int c : ok)
        if (!has(p.drawn, c)) left.push_back(c);
    if (left.empty()) {
        p.drawn.clear();
        for (int c : ok)
            if (c != p.last || ok.size() == 1) left.push_back(c);
    }
    const int v = left[rng() % left.size()];
    p.drawn.push_back(v);
    p.last = v;
    return v;
}

// A colour of one outfit from its pool: `colours` the candidates (the game's and the entries), `allowed` those it may take
// now (a mirror's colour left out; none given: all). Nothing to take: colour 0 (Color 1, which every outfit has).
inline int draw_colour(Fighter& f, int costume, const std::vector<int>& colours, const Allowed& allowed, const Rng& rng) {
    const int v = draw(f.colours[costume], colours, allowed, rng);
    return v < 0 ? 0 : v;
}

// An outfit from the character's pool, then its colour from that outfit's pool. `colours_of` gives the colours the outfit's
// Color row lists (and its entries); none known: colour 0. `colour_allowed(costume, colour)`: what the colour may be (none
// given: any). Not valid: no outfit allowed.
inline Draw draw_outfit(Fighter& f, const std::vector<int>& outfits, const Allowed& allowed,
                        const std::function<std::vector<int>(int)>& colours_of, const Rng& rng,
                        const std::function<bool(int, int)>& colour_allowed = nullptr) {
    Draw d;
    d.costume = draw(f.outfits, outfits, allowed, rng);
    if (d.costume < 0) return Draw{};
    const std::vector<int> cs = colours_of ? colours_of(d.costume) : std::vector<int>{};
    const int cos = d.costume;
    d.color = draw_colour(f, cos, cs, colour_allowed ? Allowed([&](int v) { return colour_allowed(cos, v); }) : Allowed(), rng);
    return d;
}

// A new round of outfits ("Display NSFW Outfits" changed: the pool is made again from what the row lists now)
inline void refill(Fighter& f) { f.outfits.drawn.clear(); }

// What the save holds outside Battle Settings while Random is the choice: the next draw when it is one of the game's outfits
// (the profile sends it: the opponent sees it), an entry as the colour it rides on; else nothing (the carrier, as for any
// Slots Loader outfit)
inline Draw save_holds(const Draw& next, bool next_is_ours) {
    return next.valid() && !next_is_ours ? Draw{ next.costume, worn_colour(next.color) } : Draw{};
}

// The first load of an online match's fighters (a new opponent): what the player's fighter wears for the whole set.
// `net_cos` / `net_col`: what the match holds for it (what the opponent sees). A game outfit drawn that the match holds (the
// profile carried it) is worn by the game itself, on both screens. Any other draw is worn on this PC only, over what the match
// holds (the alias, `local`): a Slots Loader outfit always, a game outfit when no profile was sent since the draw. Outfit 1
// (costume 0) cannot be worn that way (0 is the alias's "no outfit"): the set wears what the match holds and the draw waits.
// An entry drawn is worn over colour 02 where the outfit wears it (the Color Mod, on this PC): with the match's outfit when the
// match holds 02, with the alias's always.
// consumed: the next draw was taken (a new one is due).
struct Bind {
    Draw set;
    bool consumed = false;
    bool local = false;
};
inline Bind bind_set(const Draw& next, bool next_is_ours, int net_cos, int net_col) {
    const Draw net{ net_cos, net_col };
    if (!next.valid()) return { net, false, false };
    if (!next_is_ours && net_cos == next.costume)
        return { Draw{ net_cos, is_entry(next.color) && net_col == kEntryCarrier ? next.color : net_col }, true, false };
    if (next_is_ours || next.costume > 0) return { next, true, true };
    return { net, false, false };
}

// The same for Battle Settings' Random colour of one of the game's outfits (`next`: next_colour), the match holding `net_cos`
// / `net_col`: taken for the whole set when the match holds that outfit in the colour drawn (the profile carried it; an entry:
// its colour 02, the Color Mod dresses it on this PC). Any other colour in the match (no profile sent since the draw): the set
// wears it, the draw waits.
inline Bind bind_colour(const Draw& next, int net_cos, int net_col) {
    const Draw net{ net_cos, net_col };
    if (!next.valid() || next.costume != net_cos || worn_colour(next.color) != net_col) return { net, false, false };
    return { next, true, false };
}

// ---- state.json "random" ----------------------------------------------------------------------------------------------
// { "<fighter id>": { "outfits": { "drawn": [7, 0], "last": 0 }, "next": [7, 3], "next_colour": [2, 5],
//                     "colours": { "7": { "drawn": [3], "last": 3 } } } }

inline std::string ints_json(const std::vector<int>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) o += (i ? ", " : "") + std::to_string(v[i]);
    return o + "]";
}
inline std::string pool_json(const Pool& p) {
    return "{ \"drawn\": " + ints_json(p.drawn) + ", \"last\": " + std::to_string(p.last) + " }";
}
// The object's text, its lines indented by `indent` spaces (its first line not)
inline std::string state_json(const std::map<int, Fighter>& all, int indent) {
    const std::string pad(static_cast<size_t>(indent), ' ');
    std::string o = "{";
    bool first = true;
    for (auto& [fid, f] : all) {
        if (f.outfits.drawn.empty() && f.outfits.last < 0 && !f.next.valid() && !f.next_colour.valid() && f.colours.empty()) continue;
        o += std::string(first ? "" : ",") + "\n" + pad + "    \"" + std::to_string(fid) + "\": { \"outfits\": " + pool_json(f.outfits);
        if (f.next.valid()) o += ", \"next\": [" + std::to_string(f.next.costume) + ", " + std::to_string(f.next.color) + "]";
        if (f.next_colour.valid())
            o += ", \"next_colour\": [" + std::to_string(f.next_colour.costume) + ", " + std::to_string(f.next_colour.color) + "]";
        if (!f.colours.empty()) {
            o += ", \"colours\": {";
            bool fc = true;
            for (auto& [cos, p] : f.colours) {
                o += std::string(fc ? " " : ", ") + "\"" + std::to_string(cos) + "\": " + pool_json(p);
                fc = false;
            }
            o += " }";
        }
        o += " }";
        first = false;
    }
    return o + (first ? "}" : "\n" + pad + "}");
}

inline std::vector<int> json_ints(const Json* a) {
    std::vector<int> out;
    if (a && a->kind == Json::Array)
        for (auto& e : a->arr)
            if (e.kind == Json::Number) out.push_back(static_cast<int>(e.num));
    return out;
}
inline Pool json_pool(const Json* p) {
    Pool out;
    if (!p || p->kind != Json::Object) return out;
    out.drawn = json_ints(p->get("drawn"));
    if (const Json* l = p->get("last"); l && l->kind == Json::Number) out.last = static_cast<int>(l->num);
    return out;
}
// state.json's "random" (anything unreadable is left out)
inline std::map<int, Fighter> parse_state(const Json* j) {
    std::map<int, Fighter> out;
    if (!j || j->kind != Json::Object) return out;
    for (auto& [k, v] : j->obj) {
        const int fid = std::atoi(k.c_str());
        if (fid <= 0 || v.kind != Json::Object) continue;
        Fighter f;
        f.outfits = json_pool(v.get("outfits"));
        const std::vector<int> nx = json_ints(v.get("next"));
        if (nx.size() == 2 && nx[0] >= 0 && nx[1] >= 0) f.next = { nx[0], nx[1] };
        const std::vector<int> nc = json_ints(v.get("next_colour"));
        if (nc.size() == 2 && nc[0] >= 0 && nc[1] >= 0) f.next_colour = { nc[0], nc[1] };
        if (const Json* cs = v.get("colours"); cs && cs->kind == Json::Object)
            for (auto& [ck, cv] : cs->obj) {
                const int cos = std::atoi(ck.c_str());
                if (cos >= 0 && (ck == "0" || cos > 0)) f.colours[cos] = json_pool(&cv);
            }
        out[fid] = std::move(f);
    }
    return out;
}

} // namespace slots::rnd
