// SF6 Slots — Replay Outfits: what each fighter wears while a replay plays. Pure logic, no game access (plugin/tests);
// the game side is replay_outfits.cpp.
//
// Per character, in a replay only (never in a live battle):
//   - Default (no entry in replay_outfits.json, or "mode": "default"): what the players really wore. The replay only
//     holds the carrier of the online alias (carrier.hpp) for the player's own fighter, so the player's real outfit is
//     taken from the journal (replay_journal.jsonl, one line per online match, written when the game builds the match's
//     replay); only the player's side changes, the opponent keeps what the replay recorded. A replay with no journal
//     line shows what it recorded.
//   - Forced: the chosen outfit and colour on every side that plays that character, the player's own included.
// An extra outfit is named by its mod (registry.json "mod_id"), because the loader numbers slots again; a mod that is
// gone leaves the character on Default.
#pragma once

#include "json.hpp"
#include "replay_bgm.hpp"
#include "replay_options.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace slots::replay {

// ---- the replay's key ------------------------------------------------------------------------------------------------
// A replay is known by its app.ReplayInfo, the same in the replay the game builds at the end of a match and in the one
// the server gives back later: the match's random seed, the stage, the fighters and each round's random seed. The
// outfits and colours are left out of the key: the copy that holds the carrier and a copy that might hold the worn
// outfit must give the same key.
struct ReplayFacts {
    uint32_t seed = 0;
    int stage = 0;
    std::vector<int> fighters;            // FighterID per side (ReplayInfo.Fighters order: P1, P2)
    std::vector<uint32_t> round_seeds;    // ReplayRoundInfo.RandomSeed per round
};

inline uint64_t fnv1a(uint64_t h, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) { h ^= (v >> (8 * i)) & 0xFF; h *= 0x100000001B3ull; }
    return h;
}
// 16 hex digits; "" when the facts cannot name a match (no fighter, no round). The rounds' trailing zero seeds are left
// out: a replay given by the server has a round entry per possible round (5, the rounds not played at 0), the one built at
// the end of the match one per round played (seen 05/11).
inline std::string replay_key(const ReplayFacts& r) {
    size_t rounds = r.round_seeds.size();
    while (rounds && (r.round_seeds[rounds - 1] & 0xFFFF) == 0) --rounds;
    if (r.fighters.empty() || !rounds) return {};
    uint64_t h = 0xCBF29CE484222325ull;
    h = fnv1a(h, 1, 1);                   // key version
    h = fnv1a(h, r.seed, 4);
    h = fnv1a(h, static_cast<uint32_t>(r.stage), 4);
    h = fnv1a(h, r.fighters.size(), 1);
    for (int f : r.fighters) h = fnv1a(h, static_cast<uint32_t>(f), 4);
    h = fnv1a(h, rounds, 1);
    for (size_t i = 0; i < rounds; ++i) h = fnv1a(h, r.round_seeds[i] & 0xFFFF, 2);
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

// ---- the registry's slots (registry.json "slots") ----------------------------------------------------------------------
struct RegSlot { int fighter = 0, costume = 0, base = -1; std::string mod_id; };

inline std::vector<RegSlot> parse_registry(const std::string& text) {
    std::vector<RegSlot> out;
    Json j;
    if (!json_parse(text, j)) return out;
    const Json* ss = j.get("slots");
    if (!ss || ss->kind != Json::Array) return out;
    for (auto& e : ss->arr) {
        const Json* f = e.get("fighter"); const Json* n = e.get("new_costume_no"); const Json* m = e.get("mod_id");
        if (!f || !n || !m || m->kind != Json::String || m->s.empty()) continue;
        RegSlot s;
        s.fighter = static_cast<int>(f->as_int());
        s.costume = static_cast<int>(n->as_int());
        if (const Json* o = e.get("original_costume_no")) s.base = static_cast<int>(o->as_int(-1));
        s.mod_id = m->s;
        if (s.fighter > 0 && s.costume > 0) out.push_back(s);
    }
    return out;
}

// The slot's costume number now, from its mod: -1 when the mod has no slot for that character any more. A mod may have
// one slot per outfit it was made from (one mod, Outfits 1-3): the one made from `base` (when known), else the one that
// still has the number `costume`, else the first.
inline int resolve_mod(const std::vector<RegSlot>& reg, int fid, const std::string& mod_id, int costume, int base = -1) {
    const RegSlot* first = nullptr;
    const RegSlot* same_no = nullptr;
    for (auto& s : reg) {
        if (s.fighter != fid || s.mod_id != mod_id) continue;
        if (base >= 0 && s.base == base) return s.costume;
        if (s.costume == costume && !same_no) same_no = &s;
        if (!first) first = &s;
    }
    return same_no ? same_no->costume : first ? first->costume : -1;
}
// The registry's slot of a costume number (its mod and the outfit it was made from); nullptr: one of the game's outfits
inline const RegSlot* slot_of(const std::vector<RegSlot>& reg, int fid, int costume) {
    for (auto& s : reg) if (s.fighter == fid && s.costume == costume) return &s;
    return nullptr;
}

// ---- replay_outfits.json (written by the Replay Outfits tab) -----------------------------------------------------------
// {"version":1,"fighters":{"<fighter id>":{"mode":"forced","costume":<n>,"mod_id":"<mod>"(extra outfit),"color":<n>}}}
// colormod: a Color Mod entry's id (its file path) on colour 1, the entries' carrier (colormod/include/sf6colormod_entries.h)
// options: the costume options saved per outfit (replay_options.hpp)
// color_random: the colour is Random, a draw of that outfit's colours at each playback (absent in older files: false)
struct Forced { int costume = -1, color = 0; std::string mod_id, colormod; OutfitOptions options; bool random_color = false; };

// false: not a file of this format (the caller keeps what it had)
// `stage` (when given): "stage":{"mode":"forced","id":N} -> N (StageDesc.StageId), -1 = the replay's own stage
// `bgm` (when given): the Battle BGM row (replay_bgm.hpp), Random when absent
inline bool parse_outfits(const std::string& text, std::map<int, Forced>& out, int* stage = nullptr, BgmChoice* bgm = nullptr) {
    Json j;
    std::string t = text;
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);
    if (!json_parse(t, j) || j.kind != Json::Object) return false;
    out.clear();
    if (bgm) *bgm = parse_bgm(j);
    if (stage) {
        *stage = -1;
        if (const Json* st = j.get("stage"); st && st->kind == Json::Object) {
            const Json* mode = st->get("mode");
            const Json* id = st->get("id");
            if (mode && mode->kind == Json::String && mode->s == "forced" && id && id->kind == Json::Number && id->num >= 0)
                *stage = static_cast<int>(id->num);
        }
    }
    const Json* fs = j.get("fighters");
    if (!fs || fs->kind != Json::Object) return true;
    for (auto& [k, v] : fs->obj) {
        const int fid = std::atoi(k.c_str());
        const Json* mode = v.get("mode");
        if (fid <= 0 || !mode || mode->kind != Json::String || mode->s != "forced") continue;
        const Json* c = v.get("costume");
        if (!c || c->kind != Json::Number || c->num < 0) continue;
        Forced f;
        f.costume = static_cast<int>(c->num);
        if (const Json* col = v.get("color"); col && col->kind == Json::Number && col->num >= 0) f.color = static_cast<int>(col->num);
        if (const Json* m = v.get("mod_id"); m && m->kind == Json::String) f.mod_id = m->s;
        if (const Json* cm = v.get("colormod"); cm && cm->kind == Json::String && !cm->s.empty()) { f.colormod = cm->s; f.color = 1; }
        if (const Json* rc = v.get("color_random"); rc && rc->kind == Json::Bool && rc->b) { f.random_color = true; f.color = 0; f.colormod.clear(); }
        parse_outfit_options(v, f.options);
        out[fid] = f;
    }
    return true;
}

// ---- replay_journal.jsonl (this plugin's, one line per online match) ---------------------------------------------------
// {"v":1,"key":"<16 hex>","time":<unix s>,"mode":<game mode>,"me":<0|1>,"sides":[{"fid":..,"costume":..,"color":..},..],
//  "slot":{"mod_id":"<mod>","costume":<n>,"color":<n>,"base":<n>}|null}
// Numbers only: no names, no player IDs. "sides" is what the replay holds; "slot" what the player really wore (an extra
// outfit of the alias), null when the player wore what the replay holds.
struct Side { int fid = 0, costume = -1, color = 0; };
struct Entry {
    std::string key;
    long long time = 0;
    int mode = -1, me = -1;
    std::vector<Side> sides;
    bool has_slot = false;
    std::string mod_id;
    int slot_costume = -1, slot_color = 0, slot_base = -1;
};

inline std::string journal_line(const Entry& e) {
    std::string o = "{\"v\":1,\"key\":\"" + json_escape(e.key) + "\",\"time\":" + std::to_string(e.time) +
                    ",\"mode\":" + std::to_string(e.mode) + ",\"me\":" + std::to_string(e.me) + ",\"sides\":[";
    for (size_t i = 0; i < e.sides.size(); ++i)
        o += (i ? "," : "") + std::string("{\"fid\":") + std::to_string(e.sides[i].fid) + ",\"costume\":" +
             std::to_string(e.sides[i].costume) + ",\"color\":" + std::to_string(e.sides[i].color) + "}";
    o += "],\"slot\":";
    if (e.has_slot)
        o += "{\"mod_id\":\"" + json_escape(e.mod_id) + "\",\"costume\":" + std::to_string(e.slot_costume) +
             ",\"color\":" + std::to_string(e.slot_color) + ",\"base\":" + std::to_string(e.slot_base) + "}";
    else
        o += "null";
    return o + "}\n";
}

// Every readable line, by key (a later line of the same key wins: a rematch can never give the same key, a line written
// twice is the same line). A broken line (a crash in the middle of a write) is skipped.
inline size_t parse_journal(const std::string& text, std::map<std::string, Entry>& out) {
    size_t n = 0, pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        Json j;
        if (line.empty() || !json_parse(line, j) || j.kind != Json::Object) continue;
        const Json* k = j.get("key"); const Json* me = j.get("me"); const Json* ss = j.get("sides");
        if (!k || k->kind != Json::String || k->s.empty() || !me || me->kind != Json::Number || !ss || ss->kind != Json::Array) continue;
        Entry en;
        en.key = k->s;
        en.me = static_cast<int>(me->num);
        if (const Json* t = j.get("time")) en.time = t->as_int();
        if (const Json* m = j.get("mode")) en.mode = static_cast<int>(m->as_int(-1));
        for (auto& s : ss->arr) {
            Side sd;
            if (const Json* f = s.get("fid")) sd.fid = static_cast<int>(f->as_int());
            if (const Json* c = s.get("costume")) sd.costume = static_cast<int>(c->as_int(-1));
            if (const Json* c = s.get("color")) sd.color = static_cast<int>(c->as_int());
            en.sides.push_back(sd);
        }
        if (const Json* sl = j.get("slot"); sl && sl->kind == Json::Object) {
            const Json* m = sl->get("mod_id"); const Json* c = sl->get("costume");
            if (m && m->kind == Json::String && !m->s.empty() && c && c->kind == Json::Number) {
                en.has_slot = true;
                en.mod_id = m->s;
                en.slot_costume = static_cast<int>(c->num);
                if (const Json* col = sl->get("color")) en.slot_color = static_cast<int>(col->as_int());
                if (const Json* b = sl->get("base")) en.slot_base = static_cast<int>(b->as_int(-1));
            }
        }
        out[en.key] = en;
        ++n;
    }
    return n;
}

// ---- what each side wears ----------------------------------------------------------------------------------------------
struct Wear {
    bool change = false;                  // false: what the replay recorded
    int costume = -1, color = 0;
    const char* why = "recorded";         // for the log
    bool forced = false;                  // a forced outfit: the Color Mod is asked to dress it exactly (colormod or "")
    std::string colormod;                 // the forced Color Mod entry ("" = the game's own colours)
    bool random_color = false;            // the forced colour is Random: the playback draws it (replay_outfits.cpp)
};

// `recorded`: the replay's sides (fighter, outfit, colour, in team order). `entry`: the journal's line of this replay,
// nullptr when there is none. A side changes only to an outfit it does not wear already.
inline std::vector<Wear> plan(const std::vector<Side>& recorded, const Entry* entry, const std::map<int, Forced>& forced,
                              const std::vector<RegSlot>& reg) {
    std::vector<Wear> out(recorded.size());
    for (size_t i = 0; i < recorded.size(); ++i) {
        const Side& r = recorded[i];
        Wear& w = out[i];
        if (auto f = forced.find(r.fid); f != forced.end()) {
            int cos = f->second.costume;
            if (!f->second.mod_id.empty()) cos = resolve_mod(reg, r.fid, f->second.mod_id, f->second.costume);
            if (cos >= 0) {
                w.costume = cos; w.color = f->second.color; w.why = "forced";
                w.forced = true;
                w.colormod = f->second.colormod;
                w.random_color = f->second.random_color;
                w.change = cos != r.costume || w.color != r.color;
                continue;
            }
            w.why = "forced outfit's mod gone: default";
        }
        // Default: the player's own side, when the journal knows the match and the player wore an extra outfit there. The
        // side must still be the journal's fighter (a replay of the same seeds with other fighters is another match).
        if (!entry || entry->me != static_cast<int>(i) || !entry->has_slot) continue;
        if (i >= entry->sides.size() || entry->sides[i].fid != r.fid) { w.why = "journal side mismatch"; continue; }
        const int cos = resolve_mod(reg, r.fid, entry->mod_id, entry->slot_costume, entry->slot_base);
        if (cos < 0) { w.why = "worn outfit's mod gone: recorded"; continue; }
        w.costume = cos; w.color = entry->slot_color; w.why = "worn (journal)";
        w.change = cos != r.costume || w.color != r.color;
    }
    return out;
}

} // namespace slots::replay
