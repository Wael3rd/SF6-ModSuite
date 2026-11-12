// cc_take.hpp - Custom Combos: the recorder's logic with no game dependency (compiled into the DLL and the offline test).
//
// A take is what the player did since the last rest: one input mask per frame of the fight (the game's record format),
// the moves started (action ids a command names, from the label tables) and the hits (combo counter going up, the
// judge's TrialOnAttack telling what hit). When the combo counter has been back to 0 for a moment the take closes and
// becomes a trial file: one step per move, the inputs as its example.
//
// How a take becomes a recipe (the rules of the reference recorder):
//   * one step per move the player starts; an action no command names (the hit of a held super, a follow-up the game
//     chains to) belongs to the move that led to it and is accepted for its hit;
//   * a move that hit is "must hit" (check 0), one that did not is "perform" (check 1);
//   * jumps and plain dashes before the first hit are positioning, not steps; jumps are never steps;
//   * a target combo whose whole chain is one of the fighter's skills is one line judged on its last hit.
// Frames are counted on the fight's own clock (rule 9): the game layer feeds `step` entries when the clock moved by
// more than one frame in a tick.
#pragma once

#include "cc_labels.hpp"

#include <algorithm>
#include <ctime>
#include <deque>
#include <set>

namespace cc::take {

constexpr int kNeutralReset = 20;   // frames of nothing before a new take starts from here
constexpr int kMaxFrames = 3500;    // the game's demo buffer is 3600 frames long
constexpr int kEndGrace = 12;       // frames the combo counter stays at 0 before the take closes
constexpr int kStale = 180;         // frames without a new move (and without a hit) before the take starts over
constexpr int kCarryBack = 40;      // frames searched before a carried move for the start of its command
constexpr int kCarryGap = 10;       // frames of neutral that separate two commands
constexpr int kReplayStale = 90;    // replays: frames without a move before a take that has not hit starts over
constexpr int kReplayLead = 14;     // replays: frames kept before the first move of the combo
constexpr int kReplaySetup = 60;    // replays: a move this long before the first hit still sets the combo up

inline bool movement(const std::string& n) { return n == "66" || n == "44" || n == "7" || n == "8" || n == "9" || n == "6" || n == "4"; }
inline bool jump(const std::string& n) { return n == "7" || n == "8" || n == "9"; }
inline bool parry(const std::string& n) { return n == "Normal" || n == "DP" || n == "PARRY"; }

// ---------------------------------------------------------------- the label tables as the recorder reads them
struct ActLabel { std::string note; int64_t skill = 0, strength = 0, check = 0; };
struct RecLabels {
    std::map<int64_t, ActLabel> actions;                          // action id -> { notation, skill, strength, check }
    std::map<int64_t, std::pair<int64_t, int64_t>> modern, assist; // action id -> { skill, strength } (official Modern trials)
    std::map<std::string, int64_t> chains;                         // a target combo's buttons -> its skill
    std::map<std::string, std::vector<int64_t>> group;             // notation -> every id of it (sorted)
    const Labels* L = nullptr;                                     // skills (texts)
};

inline RecLabels rec_labels(const Labels& L) {
    RecLabels R;
    R.L = &L;
    const json& raw = L.raw;
    if (L.loaded && raw.is_object()) {
        auto pairs = [&](const char* key, std::map<int64_t, std::pair<int64_t, int64_t>>& dst) {
            const json* t = obj(&raw, key);
            if (!t) return;
            for (auto it = t->begin(); it != t->end(); ++it) {
                json k = it.key();
                const auto id = num(k);
                if (!id || !it->is_array()) continue;
                dst[static_cast<int64_t>(std::floor(*id))] = { inum(it->size() > 0 ? &(*it)[0] : nullptr, 0), inum(it->size() > 1 ? &(*it)[1] : nullptr, 0) };
            }
        };
        pairs("modern", R.modern);
        pairs("assist", R.assist);
        if (const json* c = obj(&raw, "chains"))
            for (auto it = c->begin(); it != c->end(); ++it)
                if (auto v = num(*it)) R.chains[it.key()] = static_cast<int64_t>(std::floor(*v));
        if (const json* a = obj(&raw, "actions"))
            for (auto it = a->begin(); it != a->end(); ++it) {
                json k = it.key();
                const auto id = num(k);
                if (!id || !it->is_array()) continue;
                const json& v = *it;
                ActLabel al;
                if (v.size() > 0) al.note = v[0].is_string() ? v[0].get<std::string>() : key_of(&v[0]);
                al.skill = inum(v.size() > 1 ? &v[1] : nullptr, 0);
                al.strength = inum(v.size() > 2 ? &v[2] : nullptr, 0);
                al.check = inum(v.size() > 3 ? &v[3] : nullptr, 0);
                R.actions[static_cast<int64_t>(std::floor(*id))] = al;
            }
    } else {
        // no label table: the action groups still give the notation
        for (auto it = L.groups.begin(); it != L.groups.end(); ++it)
            if (it->is_array())
                for (const json& v : *it)
                    if (auto d = num(v)) R.actions[static_cast<int64_t>(std::floor(*d))] = ActLabel{ it.key(), 0, 0, 0 };
    }
    for (const auto& [id, a] : R.actions) if (!a.note.empty()) R.group[a.note].push_back(id);
    for (auto& [n, ids] : R.group) std::sort(ids.begin(), ids.end());
    return R;
}

inline const ActLabel* act_label(const RecLabels& L, int64_t id) {
    auto it = L.actions.find(id);
    return it == L.actions.end() ? nullptr : &it->second;
}

// A skill's Classic command without its notes ("<CMD _236236><ICON +><CMDBTN CmnP>")
inline std::string cmd_plain(const RecLabels& L, int64_t sid) {
    if (!L.L) return {};
    auto it = L.L->skills.find(std::to_string(sid));
    if (it == L.L->skills.end() || it->second.first.empty()) return {};
    return text::one_way(text::drop_notes(it->second.first));
}

// ... and the notation it reads as ("236236+P"); nullopt when a tag has no notation
inline std::optional<std::string> cmd_note(const std::string& cmd, int64_t strength) {
    std::string out;
    bool ok = true, any = false;
    for (size_t i = 0; i < cmd.size();) {
        const size_t a = cmd.find('<', i);
        if (a == std::string::npos) break;
        const size_t b = cmd.find('>', a + 1);
        if (b == std::string::npos) break;
        const std::string tag = cmd.substr(a + 1, b - a - 1);
        i = b + 1;
        any = true;
        auto digits = [](const std::string& s) { if (s.empty()) return false; for (char c : s) if (!text::digit(c)) return false; return true; };
        if (tag.rfind("CMD _", 0) == 0 && digits(tag.substr(5))) { out += tag.substr(5); continue; }
        if (tag.rfind("ICON ", 0) == 0 && tag.size() > 5) {
            const std::string icon = tag.substr(5);
            if (icon == "+") { out += "+"; continue; }
            if (icon == "next") { out += " >"; continue; }
            if (icon.size() == 1 && text::digit(icon[0])) { out += icon; continue; }
            ok = false;
            continue;
        }
        if (tag.rfind("CMDBTN ", 0) == 0) {                      // (%a-)([PK])$
            const std::string name = tag.substr(7);
            bool letters = !name.empty();
            for (char c : name) if (!text::alpha(c)) letters = false;
            const char fam = letters ? name.back() : 0;
            if (fam == 'P' || fam == 'K') {
                const std::string level = name.substr(0, name.size() - 1);
                const char* lv = level == "Low" ? "L" : level == "Mid" ? "M" : level == "High" ? "H" : level == "Cmn" ? "" : nullptr;
                if (lv) {
                    std::string l = lv, f(1, fam);
                    if (level == "Cmn") {
                        if (strength == 7) f += fam;
                        else l = strength == 1 ? "L" : strength == 2 ? "M" : strength == 3 ? "H" : "";
                    }
                    out += l + f;
                    continue;
                }
            }
        }
        ok = false;
    }
    if (!ok || !any || out.empty()) return std::nullopt;
    return out;
}

// ---------------------------------------------------------------- moves and takes
struct Move {
    int64_t act = 0;
    std::string note;
    int64_t skill = 0, strength = 0, label_check = 0;
    int64_t f = 0;                       // frames since the take started
    std::set<int64_t> ids, follow;       // its own ids; the ids it chains to by itself
    bool hit = false;
    int hits = 0, counter = 0;           // counter: 1 counter hit, 2 punish counter (first hit)
    std::optional<int64_t> first_hit;
    int cc = 0;
    bool proj = false;
    bool as = false;                     // Modern: started with Assist held
    uint32_t mbtn = 0;                   // Modern: the button pressed for it (pl_sw_new bits)
};

struct Px { std::optional<double> x1, x2; double y1 = 0; };

struct Take {
    int64_t start = 0;
    std::vector<uint16_t> inputs;        // one mask per frame of the fight
    std::vector<std::optional<double>> x1s;   // P1's position on each frame (carry)
    std::vector<Px> px;                  // replays: both positions and the attacker's height per frame
    std::vector<int> styles;             // replays: the attacker's own gauge per frame
    std::vector<Move> moves;
    int hits = 0, max_cc = 0, zero_for = 0, style = 0;
    double damage = 0;
    std::optional<double> x1, x2;
    bool facing_right = false, modern = false, quiet = false;
    int64_t last_act = -1, as_until = -1;
    int first_input = 0;                 // 1-based index of the first frame with an input, 0 = none
    std::optional<int64_t> last_hit, last_dmg;
    int64_t last_move_at = 0, busy_at = 0;
    struct Press { int64_t f; uint32_t bits; };
    std::optional<Press> press;
    int64_t len() const { return static_cast<int64_t>(inputs.size()); }
};

// What the game layer reads each tick from the attacker (P1 in the slot) and its opponent
struct Frame {
    int64_t act = -1;
    int cc = 0;
    uint32_t sw = 0;                     // pl_sw_new
    bool facing_right = false;           // rl_dir
    std::optional<double> x1, x2;        // positions (game units x 65536)
    double y1 = 0;
    int style = 0;                       // mStyleNo
    double dmg = 0;                      // the combo's damage (the larger of the two teams' mComboDamage)
    int counter = 0;                     // the opponent now: 2 punish counter, 1 counter, 0
    bool modern = false;                 // the control type of the take to start
};
struct HitEv { int64_t f; int64_t act; bool proj; };

inline Take new_take(const Frame& in, int64_t frame) {
    Take t;
    t.start = frame;
    t.x1 = in.x1;
    t.x2 = in.x2;
    t.facing_right = in.facing_right;
    t.last_act = in.act;
    t.modern = in.modern;
    return t;
}

// One input mask per frame in the game's record format: UP 1, DOWN 2, BACK 4, FORWARD 8, LP 16 ... HK 512. pl_sw_new
// holds the same bits with the directions as the screen sees them (RIGHT 4, LEFT 8), so facing right they swap.
inline uint16_t input_mask(uint32_t sw, bool facing_right, bool modern) {
    uint32_t m = sw & (modern ? 0x3FFFu : 0x3FFu);              // Modern demos use bits above HK
    if (facing_right) {
        const uint32_t d = m & 12u;
        if (d == 4) m = (m & ~12u) | 8u;
        else if (d == 8) m = (m & ~12u) | 4u;
    }
    return static_cast<uint16_t>(m);
}

// The take's latest move is still running: a charge held for seconds is one move, not "nothing going on"
inline bool move_running(const Take& t, int64_t act) {
    if (t.moves.empty() || act < 100) return false;
    const Move& cur = t.moves.back();
    return cur.ids.count(act) || cur.follow.count(act);
}

// An action started: true when it is a new move of the take
inline bool on_action(Take& t, const RecLabels& L, int64_t act, int64_t frame) {
    const ActLabel* a = act_label(L, act);
    Move* cur = t.moves.empty() ? nullptr : &t.moves.back();
    auto at = [&](const Move* m) { return frame - t.start - m->f; };
    if (a && !a->note.empty()) {
        if (jump(a->note)) return false;                        // a jump is never a step
        // a move the game comes back to after its own follow-up (Shinku Hadoken 1200 -> ... -> 1200) is one move
        if (cur && cur->act == act && !cur->follow.empty() && at(cur) < 240) return false;
        // Drive Rush out of a parry: the parry that starts it is not a step of its own
        if (a->note == "RAW DR" && cur && parry(cur->note) && !cur->hit && at(cur) < 40) {
            t.moves.pop_back();
            cur = t.moves.empty() ? nullptr : &t.moves.back();
        }
        const int64_t gap = cur ? at(cur) : 999;
        // the same move, another action: a charged or held version under the same command (its ids judged together)
        if (cur && cur->note == a->note && !cur->hit && gap < 20) { cur->ids.insert(act); return false; }
        // a move replaced at once by another never came out (the OD version entered through the heavy one)
        if (cur && !cur->hit && gap <= 3 && !movement(cur->note)) t.moves.pop_back();
        Move mv;
        mv.act = act; mv.note = a->note; mv.skill = a->skill; mv.strength = a->strength; mv.label_check = a->check;
        mv.f = frame - t.start;
        mv.ids = { act };
        mv.as = t.modern && frame <= t.as_until;
        if (t.press && frame - t.press->f <= 8) mv.mbtn = t.press->bits;
        t.moves.push_back(std::move(mv));
        return true;
    }
    // An id no command names but which the official trials label: when its skill is another command than the move
    // under way (or nothing is under way), a move of its own named from that skill's command (Zangief's SA1 is 1207)
    if (a && a->skill != 0 && act >= 100 && (!cur || cur->skill != 0)) {
        const std::string mine = cmd_plain(L, a->skill);
        if (!mine.empty() && (!cur || cmd_plain(L, cur->skill) != mine)) {
            Move mv;
            mv.act = act;
            mv.note = cmd_note(mine, a->strength).value_or("#" + std::to_string(act));
            mv.skill = a->skill; mv.strength = a->strength; mv.label_check = a->check;
            mv.f = frame - t.start;
            mv.ids = { act };
            t.moves.push_back(std::move(mv));
            return true;
        }
    }
    // an id no command names: part of the move under way when it is a real action
    if (cur && act >= 100) cur->follow.insert(act);
    return false;
}

// A hit: to the move whose ids hold the hitting action, the latest move otherwise
inline void on_hit(Take& t, const RecLabels& L, const HitEv* ev, int cc, int opp_counter, int64_t frame) {
    Move* target = nullptr;
    if (ev && ev->act > 0) {
        for (size_t i = t.moves.size(); i-- > 0;) {
            Move& mv = t.moves[i];
            if (mv.ids.count(ev->act) || mv.follow.count(ev->act)) { target = &mv; break; }
        }
        if (!target && ev->proj) {        // a projectile: the latest move with the same label (Shinku Hadoken 1200 -> 1205)
            const ActLabel* la = act_label(L, ev->act);
            if (la && la->skill != 0)
                for (size_t i = t.moves.size(); i-- > 0;)
                    if (t.moves[i].skill == la->skill || t.moves[i].skill % 10000 == la->skill % 10000) { target = &t.moves[i]; break; }
        }
    }
    if (!target && !t.moves.empty()) target = &t.moves.back();
    if (!target) {                        // a hit by an action no table names, with no move before it: kept under its id
        const int64_t id = ev ? ev->act : -1;
        if (id < 100) return;
        Move mv;
        mv.act = id; mv.note = "#" + std::to_string(id); mv.f = frame - t.start; mv.ids = { id };
        t.moves.push_back(std::move(mv));
        target = &t.moves.back();
    }
    target->hit = true;
    target->hits += 1;
    if (!target->first_hit) target->first_hit = frame - t.start;
    target->cc = cc;
    if (ev && ev->act > 0) {
        target->ids.insert(ev->act);
        if (ev->proj) target->proj = true;
    }
    if (target->hits == 1) target->counter = opp_counter;
    t.hits += 1;
    t.last_hit = frame - t.start;
}

// ---------------------------------------------------------------- from the take to a trial
inline std::string note_key(const std::string& note) {
    std::string n;
    for (char c : note) if (!text::sp(c)) n += c;
    size_t i = 0;
    while (i < n.size() && text::digit(n[i])) ++i;
    if (i == 0) return n;
    size_t b = i < n.size() && n[i] == '+' ? i + 1 : i;
    if (b >= n.size()) return n;
    for (size_t k = b; k < n.size(); ++k) if (!text::upper(n[k])) return n;
    std::string dirs = n.substr(0, i);
    if (dirs.size() > 1) {
        std::string d2;
        for (char c : dirs) if (c != '5') d2 += c;
        dirs = d2.empty() ? "5" : d2;
    }
    return dirs + "+" + n.substr(b);
}

constexpr int64_t kModernNormal[3][3] = { { 3820, 3920, 4020 }, { 4120, 4220, 4320 }, { 5320, 5420, 5520 } };   // stand, crouch, jump

// [LMH][PK]
inline bool lmh_pk(const std::string& s, size_t at) {
    return s.size() == at + 2 && (s[at] == 'L' || s[at] == 'M' || s[at] == 'H') && (s[at + 1] == 'P' || s[at + 1] == 'K');
}

// The label of a move: in Modern what the official Modern trials call that action (an Assisted Combo step when Assist
// was held), else its Classic skill (the game writes a special's Modern command from it)
inline std::pair<int64_t, int64_t> move_label(const RecLabels& L, const Move& mv, bool modern) {
    if (modern) {
        auto look = [&](const std::map<int64_t, std::pair<int64_t, int64_t>>& t) -> const std::pair<int64_t, int64_t>* {
            auto f = t.find(mv.act);
            if (f != t.end()) return &f->second;
            for (int64_t id : mv.ids) if ((f = t.find(id)) != t.end()) return &f->second;
            for (int64_t id : mv.follow) if ((f = t.find(id)) != t.end()) return &f->second;
            return nullptr;
        };
        const std::pair<int64_t, int64_t>* e = mv.as ? look(L.assist) : nullptr;
        if (!e) e = look(L.modern);
        if (e) return *e;
        // a normal the official Modern trials never use: the Modern button pressed names it
        const int k = mv.mbtn == 0x10 ? 0 : mv.mbtn == 0x80 ? 1 : mv.mbtn == 0x100 ? 2 : -1;
        if (k >= 0) {
            if (lmh_pk(mv.note, 0)) return { kModernNormal[0][k], 0 };
            if (mv.note.size() == 4 && mv.note[0] == '2' && mv.note[1] == '+' && lmh_pk(mv.note, 2)) return { kModernNormal[1][k], 0 };
            if (mv.note.size() == 4 && mv.note[0] == 'j' && mv.note[1] == '.' && lmh_pk(mv.note, 2)) return { kModernNormal[2][k], 0 };
        }
    }
    int64_t skill = mv.skill, strength = mv.strength;
    if (skill == 0)                                              // the hit id may carry the label (official hit ids)
        for (int64_t id : mv.ids)
            if (const ActLabel* a = act_label(L, id); a && a->skill != 0) { skill = a->skill; strength = a->strength; break; }
    // a special done with the SP button has no Light / Medium / Heavy in Modern (only its OD version differs)
    if (modern && skill != 0 && strength != 7 && L.L) {
        auto it = L.L->skills.find(std::to_string(skill));
        if (it != L.L->skills.end() && it->second.second.find("CMDBTN SM") != std::string::npos) strength = 0;
    }
    return { skill, strength };
}

inline std::string move_text(const RecLabels& L, const Move& mv, bool modern) {
    const auto [skill, strength] = move_label(L, mv, modern);
    return L.L ? step_text(*L.L, mv.note, skill, static_cast<int>(strength), modern) : mv.note;
}

// A recipe line as the recorder writes it ("frame" and "hits" are dropped before the file is written)
struct OutStep { json j; int64_t frame = 0; int hits = 0; };

inline std::vector<OutStep> build_steps(const RecLabels& L, const std::vector<Move>& moves, bool modern) {
    // 1. what counts: no trailing move that did not hit, no positioning before the first hit
    size_t last_hit = 0;
    for (size_t i = 0; i < moves.size(); ++i) if (moves[i].hit) last_hit = i + 1;
    std::vector<Move> kept;
    bool seen_hit = false;
    for (size_t i = 0; i < last_hit; ++i) {
        const Move& mv = moves[i];
        bool drop = false;
        if (!mv.hit) {
            if (movement(mv.note) && !seen_hit) drop = true;          // walking up, dashing in
            if (!seen_hit && mv.label_check == 0) drop = true;         // an attack that whiffed before the combo
        }
        if (mv.hit) seen_hit = true;
        if (!drop) kept.push_back(mv);
    }
    // 2. target combos: a move followed by ">X" moves is one line when the whole chain is a skill
    std::vector<Move> steps;
    for (size_t i = 0; i < kept.size();) {
        size_t j = i;
        while (j + 1 < kept.size() && !kept[j + 1].note.empty() && kept[j + 1].note[0] == '>') ++j;
        bool merged = false;
        if (j > i) {
            std::string key;
            for (char c : note_key(kept[i].note)) if (c != '+') key += c;
            for (size_t k = i + 1; k <= j; ++k) key += kept[k].note.substr(1);
            auto it = L.chains.find(key);
            if (it != L.chains.end()) {
                const Move& last = kept[j];
                Move m;
                for (size_t k = i; k <= j; ++k) m.note += (k > i ? " " : "") + kept[k].note;
                m.act = last.act; m.skill = it->second; m.strength = 0; m.as = last.as; m.label_check = 0; m.f = kept[i].f;
                m.ids = last.ids; m.follow = last.follow; m.hit = last.hit; m.counter = kept[i].counter; m.proj = last.proj;
                m.first_hit = last.first_hit;
                for (size_t k = i; k <= j; ++k) m.hits += kept[k].hits;
                steps.push_back(std::move(m));
                merged = true;
                i = j + 1;
            }
        }
        if (!merged) steps.push_back(kept[i++]);
    }
    // 3. the recipe lines
    std::vector<OutStep> out;
    for (const Move& mv : steps) {
        std::set<int64_t> ids = mv.ids;
        auto g = L.group.find(mv.note);
        if (g != L.group.end()) ids.insert(g->second.begin(), g->second.end());
        if (mv.hit) ids.insert(mv.follow.begin(), mv.follow.end());
        const auto [skill, strength] = move_label(L, mv, modern);
        OutStep st;
        st.j = { { "notation", mv.note }, { "action_ids", json(std::vector<int64_t>(ids.begin(), ids.end())) }, { "action_id", mv.act },
                 { "check_type", mv.hit ? 0 : 1 }, { "judge_type", 0 }, { "style_no", 0 }, { "trigger_no", 0 }, { "ex_action_type", 0 },
                 { "skill_id", skill }, { "strength_type", strength }, { "option1", mv.counter }, { "option2", 0 }, { "flags", 0 },
                 { "next_step", -1 } };
        if (mv.proj) st.j["is_projectile_hit"] = true;
        st.j["text"] = L.L ? step_text(*L.L, mv.note, skill, static_cast<int>(strength), modern) : mv.note;
        st.frame = mv.f;
        st.hits = mv.hits;
        out.push_back(std::move(st));
    }
    return out;
}

inline std::string mask_token(uint16_t m) {
    static const char* const kDir[16] = { "5", "8", "2", "5", "6", "9", "3", "5", "4", "7", "1", "5", "5", "5", "5", "5" };
    std::string s = kDir[m & 15];
    static const std::pair<int, const char*> kBtn[] = { { 16, "LP" }, { 32, "MP" }, { 64, "HP" }, { 128, "LK" }, { 256, "MK" }, { 512, "HK" } };
    for (const auto& [b, n] : kBtn) if (m & b) { s += "+"; s += n; }
    return s;
}

// The example: the take's inputs from a few frames before the first one to `last` (1-based), run-length encoded the way
// the training recorder writes them. "rle" holds the exact masks the game plays; "timeline" the readable form.
inline json build_demo(const std::vector<uint16_t>& inputs, int first, int64_t last, const std::vector<OutStep>& steps) {
    const int64_t n = static_cast<int64_t>(inputs.size());
    const int64_t from = std::max<int64_t>(1, (first ? first : 1) - 4);
    const int64_t to = std::min<int64_t>(n, last);
    json tl = json::array(), rle = json::array();
    int64_t run = 0;
    std::optional<uint16_t> cur;
    for (int64_t i = from; i <= to; ++i) {
        const uint16_t m = inputs[static_cast<size_t>(i - 1)];
        if (cur && m == *cur) { ++run; continue; }
        if (cur) { tl.push_back(std::to_string(run) + "f : " + mask_token(*cur)); rle.push_back({ run, *cur }); }
        cur = m;
        run = 1;
    }
    if (cur) { tl.push_back(std::to_string(run) + "f : " + mask_token(*cur)); rle.push_back({ run, *cur }); }
    json sf = json::array();
    for (const OutStep& st : steps) sf.push_back(std::max<int64_t>(0, st.frame - (from - 1)));
    return { { "timeline", tl }, { "rle", rle }, { "step_frames", sf }, { "frames", std::max<int64_t>(0, to - from + 1) } };
}

inline std::string signature(const std::vector<OutStep>& steps, bool modern) {
    std::string s = modern ? "M:" : "";
    for (size_t i = 0; i < steps.size(); ++i) {
        if (i) s += ">";
        s += str_of(at(steps[i].j, "notation")) + "/" + std::to_string(inum(at(steps[i].j, "check_type"), 0));
    }
    return s;
}

// ---------------------------------------------------------------- file names and times
struct Stamp { std::string file, title, at; int64_t epoch = 0; };
inline Stamp stamp_of(std::time_t now) {
    std::tm tm{};
    localtime_s(&tm, &now);
    char f[32], t[32], a[32];
    std::strftime(f, sizeof(f), "%Y%m%d_%H%M%S", &tm);
    std::strftime(t, sizeof(t), "REC %d/%m %H:%M", &tm);
    std::strftime(a, sizeof(a), "%Y-%m-%d %H:%M:%S", &tm);
    return { f, t, a, static_cast<int64_t>(now) };
}
// rec_<stamp>.json, or rec_<stamp>_<n>.json when that name is taken (two takes in one second never share a file)
template <class Taken> std::string take_file_name(const std::string& stamp, Taken taken) {
    std::string name = "rec_" + stamp + ".json";
    for (int n = 2; taken(name) && n < 1000; ++n) name = "rec_" + stamp + "_" + std::to_string(n) + ".json";
    return name;
}
// A file the recorder may delete: one of its takes ("rec_*.json") right in a fighter's folder of the data folder
inline bool is_take_file(const fs::path& p, const fs::path& data_dir) {
    const std::wstring name = p.filename().wstring();
    if (name.size() < 9 || name.rfind(L"rec_", 0) != 0 || p.extension().wstring() != L".json") return false;
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(p.parent_path().parent_path(), ec);
    const fs::path b = fs::weakly_canonical(data_dir, ec);
    return !ec && a == b && !p.parent_path().filename().empty() && p.parent_path().filename().wstring()[0] != L'_';
}

// ---------------------------------------------------------------- closing a take
inline bool stock_fighter(int id) {
    switch (id) { case 3: case 5: case 12: case 15: case 16: case 20: case 21: case 28: case 32: case 33: return true; default: return false; }
}

struct TrialOpts {
    int min_steps = 2;
    bool replay = false;                 // a replay's combo: the gauges full, nothing of the slot's dummy
    json gauges;                         // the slot's ("gauges" of _rec_settings.json)
    int dummy_state = 0, dummy_guard = 2, dummy_sa = 0, dummy_dg = 6;
    std::string dir;                     // the fighter's folder
    Stamp when;
};
struct Closed {
    enum Kind { None, Short, Made } kind = None;
    std::vector<OutStep> steps;          // frame / hits still in (the texts of the live rows)
    json trial;                          // Made: the file
    std::string sig;
};

inline Closed close_take(const Take& t, const RecLabels& L, int chara, const TrialOpts& o) {
    Closed c;
    if (t.hits == 0) return c;
    c.steps = build_steps(L, t.moves, t.modern);
    int hit_steps = 0;
    for (const OutStep& st : c.steps) if (inum(at(st.j, "check_type"), 1) == 0) ++hit_steps;
    if (static_cast<int>(c.steps.size()) < o.min_steps || hit_steps == 0) { c.kind = Closed::Short; return c; }
    const int64_t last = std::min<int64_t>(t.len(), (t.last_hit ? *t.last_hit : t.len()) + 45);
    json demo = build_demo(t.inputs, t.first_input, last, c.steps);
    bool punish = false, counter = false;
    json steps = json::array();
    for (const OutStep& st : c.steps) {
        const int64_t o1 = inum(at(st.j, "option1"), 0);
        if (o1 == 2) punish = true; else if (o1 == 1) counter = true;
        steps.push_back(st.j);
    }
    c.sig = signature(c.steps, t.modern);
    json tr = {
        { "control", t.modern ? "modern" : "classic" }, { "schema", "sf6.custom-trial.v1" }, { "title", o.when.title },
        { "character", o.dir }, { "fighter_id", chara }, { "notes", o.replay ? "Recorded from a replay by the Custom Combos recorder"
                                                                              : "Recorded in game by the Custom Combos recorder" },
        { "gauges", o.replay || !o.gauges.is_object() ? json{ { "sa_stock", "max" }, { "dg_stock", "max" }, { "sa_heal", "auto" }, { "dg_heal", "auto" } } : o.gauges },
        { "steps", steps }, { "demo", demo }, { "level", 0 }, { "order", -o.when.epoch },
        { "recorded", { { "at", o.when.at }, { "damage", static_cast<int64_t>(std::floor(t.damage)) }, { "hits", t.max_cc }, { "signature", c.sig },
                        { "frames", demo["frames"] }, { "recorder", 1 } } } };
    if (t.x1 && t.x2)
        tr["start"] = { { "p1", static_cast<int64_t>(std::floor(*t.x1 / 65536 + 0.5)) }, { "p2", static_cast<int64_t>(std::floor(*t.x2 / 65536 + 0.5)) } };
    if (stock_fighter(chara) && t.style > 0) tr["unique"] = { { "type", 1 }, { "stock", t.style } };
    // how long the combo goes on dealing damage after its last move first hit: the trial waits that long before ending
    const Move* last_move = nullptr;
    for (const Move& mv : t.moves) if (mv.hit) last_move = &mv;
    if (last_move && last_move->first_hit && t.last_dmg) tr["end_delay"] = std::max<int64_t>(0, *t.last_dmg - *last_move->first_hit);
    if (punish) tr["dummy_counter"] = "punish"; else if (counter) tr["dummy_counter"] = "counter";
    if (!o.replay) {                     // the dummy as the slot had it, when it is not the trials' own
        if (o.dummy_state != 0 || o.dummy_guard != 2) tr["dummy"] = { { "state", o.dummy_state }, { "guard", o.dummy_guard } };
        if (o.dummy_sa != 0 || o.dummy_dg != 6)
            tr["recipe"] = { { "teams", json::array({ json::object(), { { "SGaugeStock", o.dummy_sa }, { "DGaugeStock", o.dummy_dg } } }) } };
    }
    c.trial = std::move(tr);
    c.kind = Closed::Made;
    return c;
}

// What the player started after the combo's last hit is not lost with the take that closes: the new take gets the
// inputs back to the neutral before that command (at most kCarryBack frames, never before the last hit) and the moves
// started since the last hit. Those stay quiet: a move that only misses goes away with the next rest.
inline std::optional<Take> carry_take(const Take& old, const Frame& now, int64_t frame) {
    const int64_t n = old.len();
    if (n == 0 || n >= kMaxFrames || n != frame - old.start) return std::nullopt;
    const int64_t last_hit = old.last_hit.value_or(0);
    size_t last = 0;
    for (size_t i = 0; i < old.moves.size(); ++i) if (old.moves[i].hit) last = i + 1;
    std::vector<Move> tail;
    for (size_t i = last; i < old.moves.size(); ++i) if (old.moves[i].f > last_hit) tail.push_back(old.moves[i]);
    const int64_t E = std::min(n, tail.empty() ? n : tail.front().f);
    const int64_t lo = std::max<int64_t>({ last_hit + 1, E - kCarryBack, 1 });
    if (lo > E) return std::nullopt;
    int64_t S = lo, run = 0;
    for (int64_t i = E; i >= lo; --i) {
        if (old.inputs[static_cast<size_t>(i - 1)] == 0) {
            if (++run >= kCarryGap) { S = i + run - 4; break; }   // four frames of that neutral stay
        } else run = 0;
    }
    bool any = !tail.empty();
    for (int64_t i = S; i <= n && !any; ++i) if (old.inputs[static_cast<size_t>(i - 1)] != 0) any = true;
    if (!any) return std::nullopt;
    Take t = new_take(now, frame);       // the dummy where it stands now, the player's action as it is
    const int64_t off = S - 1;
    for (int64_t i = S; i <= n; ++i) {
        const uint16_t m = old.inputs[static_cast<size_t>(i - 1)];
        t.inputs.push_back(m);
        t.x1s.push_back(static_cast<size_t>(i - 1) < old.x1s.size() ? old.x1s[static_cast<size_t>(i - 1)] : std::nullopt);
        if (m != 0 && !t.first_input) t.first_input = static_cast<int>(i - off);
    }
    t.start = frame - (n - off);
    if (static_cast<size_t>(S - 1) < old.x1s.size() && old.x1s[static_cast<size_t>(S - 1)]) t.x1 = old.x1s[static_cast<size_t>(S - 1)];
    t.style = now.style;
    for (Move mv : tail) { mv.f = std::max<int64_t>(1, mv.f - off); t.moves.push_back(std::move(mv)); }
    if (!tail.empty()) { t.last_move_at = t.moves.back().f; t.quiet = true; }
    return t;
}

// ---------------------------------------------------------------- the recording slot, frame by frame
// The slot's take, fed once per tick with `step` fight frames. What the game layer must do follows from Out.
struct SlotCore {
    int64_t frame = 0;                   // frames of the fight the slot ran (its own clock)
    std::optional<Take> take;
    std::deque<HitEv> hits;              // what TrialOnAttack saw since the last tick
    int last_cc = 0, neutral_for = 0;
    uint32_t prev_sw = 0;

    struct Out {
        bool redraw = false;             // what the rows show changed
        bool recording = false;          // a take began showing moves: the last result leaves the rows
        bool reset = false;              // nothing kept (dropped, started over)
        std::optional<Take> closed;      // a take closed: make it a trial
    };

    bool live() const { return take && !take->quiet && !take->moves.empty(); }
    void push_hit(int64_t act, bool proj) { hits.push_back({ frame, act, proj }); }
    void drop() { take.reset(); hits.clear(); last_cc = 0; }

    Out tick(const Frame& in, int step, const RecLabels& L) {
        Out o;
        frame += step;
        const uint16_t mask = input_mask(in.sw, in.facing_right, take && take->modern);
        const bool idle = mask == 0 && in.act < 100 && in.y1 == 0;
        const int cc = in.cc;
        if (!take) {
            if (cc > 0) { last_cc = cc; return o; }       // a combo already under way is not ours
            take = new_take(in, frame);
        }
        Take* t = &*take;
        if (t->modern) {
            if (in.sw & 0x200) t->as_until = frame + 10;        // Assist held
            const uint32_t edge = in.sw & ~prev_sw & 0x190u;  // Light / Medium / Heavy just pressed
            if (edge) t->press = Take::Press{ frame, edge };
        }
        prev_sw = in.sw;
        // nothing going on: the take starts over from here (its demo and positions are those of the moment of rest)
        if (t->hits == 0 && cc == 0 && idle) ++neutral_for; else neutral_for = 0;
        const bool resting = neutral_for >= kNeutralReset;
        if (move_running(*t, in.act)) t->busy_at = t->len();
        const bool stale = t->hits == 0 && cc == 0 && (t->len() - std::max(t->last_move_at, t->busy_at)) > kStale;
        if (resting || stale) {
            if (!t->moves.empty() || t->len() > 0) {
                const bool had = !t->moves.empty();
                take = new_take(in, frame);
                t = &*take;
                if (had) { o.redraw = true; o.reset = true; }
            }
        }
        if (!resting) {
            if (t->len() == 0) { t->x1 = in.x1; t->x2 = in.x2; t->style = in.style; t->start = frame - step; }
            for (int k = 0; k < step; ++k)                     // one entry per frame of the fight
                if (t->len() < kMaxFrames) {
                    t->inputs.push_back(mask);
                    t->x1s.push_back(in.x1);
                    if (mask != 0 && !t->first_input) t->first_input = static_cast<int>(t->len());
                }
        }
        if (in.act != t->last_act) {
            t->last_act = in.act;
            if (in.act > 0 && on_action(*t, L, in.act, frame)) {
                t->last_move_at = t->len();
                if (t->moves.size() == 1 || t->quiet) { t->quiet = false; o.recording = true; }
                o.redraw = true;
            }
        }
        if (t->quiet && cc > last_cc) { t->quiet = false; o.recording = true; }   // the carried move hit
        if (cc > last_cc) {                                    // hits: the combo counter went up
            for (int k = 1; k <= cc - last_cc; ++k) {
                HitEv ev{ frame, in.act, false };              // the hook saw nothing: P1's own action
                if (!hits.empty()) { ev = hits.front(); hits.pop_front(); }
                on_hit(*t, L, &ev, last_cc + k, in.counter, frame);
            }
            o.redraw = true;
        }
        for (auto it = hits.begin(); it != hits.end();) it = (frame - it->f > 3) ? hits.erase(it) : it + 1;
        if (cc > t->max_cc) t->max_cc = cc;
        last_cc = cc;
        if (cc > 0 && in.dmg > t->damage) { t->damage = in.dmg; t->last_dmg = frame - t->start; }
        // the combo is over once the counter has been back to 0 for a moment
        if (t->hits > 0 && cc == 0) {
            if (++t->zero_for >= kEndGrace) {
                Take old = std::move(*take);
                take.reset();
                o.redraw = true;
                take = carry_take(old, in, frame);
                o.closed = std::move(old);
            }
        } else t->zero_for = 0;
        return o;
    }
};

// ---------------------------------------------------------------- replays
// A replay is not a trial: no judge, nobody standing still. Each side has its own take; a take is cut to start
// kReplayLead frames before the first move that sets the combo up, its start positions those of that frame, mirrored
// when the attacker stood on the right (the masks are already relative to facing).
struct Side { std::optional<Take> take; int last_cc = 0; uint32_t prev_sw = 0; };

// The take cut down to its combo; false when nothing hit
inline bool replay_cut(Take& t) {
    const Move* first = nullptr;
    for (const Move& mv : t.moves) if (mv.hit) { first = &mv; break; }
    if (!first) return false;
    std::vector<Move> kept;
    bool seen = false;
    const int64_t first_f = first->f;
    for (const Move& mv : t.moves) {
        if (&mv == first) seen = true;
        if (seen || (mv.f >= first_f - kReplaySetup && !movement(mv.note) && mv.label_check != 0)) kept.push_back(mv);
    }
    // a combo that opens in the air starts on the ground, before the jump
    int64_t at_ = std::max<int64_t>(1, std::min<int64_t>(t.len(), kept.front().f ? kept.front().f : 1));
    while (at_ > 1 && static_cast<size_t>(at_ - 1) < t.px.size() && t.px[static_cast<size_t>(at_ - 1)].y1 > 0) --at_;
    const int64_t cut = std::max<int64_t>(1, at_ - kReplayLead);
    const int64_t off = cut - 1;
    t.inputs.erase(t.inputs.begin(), t.inputs.begin() + std::min<int64_t>(off, t.len()));
    for (Move& mv : kept) { mv.f -= off; if (mv.first_hit) *mv.first_hit -= off; }
    t.moves = std::move(kept);
    if (t.last_hit) *t.last_hit -= off;
    if (t.last_dmg) *t.last_dmg -= off;
    t.last_move_at = std::max<int64_t>(0, t.last_move_at - off);
    t.first_input = 1;
    if (static_cast<size_t>(cut - 1) < t.px.size()) {
        const Px& p = t.px[static_cast<size_t>(cut - 1)];
        if (p.x1 && p.x2) {
            double x1 = *p.x1, x2 = *p.x2;
            if (x1 > x2) { x1 = -x1; x2 = -x2; }                // the trial's player always starts on the left
            const double gap = 70.0 * 65536;                  // one over the other (a cross-up): moved apart
            if (x2 - x1 < gap) { const double mid = (x1 + x2) / 2; x1 = std::floor(mid - gap / 2); x2 = std::floor(mid + gap / 2); }
            t.x1 = x1;
            t.x2 = x2;
        }
    }
    if (static_cast<size_t>(cut - 1) < t.styles.size()) t.style = t.styles[static_cast<size_t>(cut - 1)];
    return true;
}

// One side of a replay for `step` frames; returns the take that closed, if one did
inline std::optional<Take> replay_side(Side& s, const Frame& in, int step, int64_t frame, const RecLabels& L) {
    const int cc = in.cc;
    if (!s.take) {
        if (cc > 0) { s.last_cc = cc; return std::nullopt; }  // a combo already under way is not taken half-way
        s.take = new_take(in, frame);
    }
    Take* t = &*s.take;
    if (move_running(*t, in.act)) t->busy_at = t->len();
    if (t->hits == 0 && cc == 0 && t->len() - std::max(t->last_move_at, t->busy_at) > kReplayStale) {
        s.take = new_take(in, frame);
        t = &*s.take;
    }
    if (t->len() == 0) t->start = frame - step;
    const uint16_t mask = input_mask(in.sw, in.facing_right, t->modern);
    for (int k = 0; k < step; ++k)
        if (t->len() < kMaxFrames) {
            t->inputs.push_back(mask);
            t->px.push_back({ in.x1, in.x2, in.y1 });
            t->styles.push_back(in.style);
        }
    if (in.act != t->last_act) {
        t->last_act = in.act;
        if (in.act > 0 && on_action(*t, L, in.act, frame)) t->last_move_at = t->len();
    }
    if (cc > s.last_cc)
        for (int k = 1; k <= cc - s.last_cc; ++k) { HitEv ev{ frame, in.act, false }; on_hit(*t, L, &ev, s.last_cc + k, in.counter, frame); }
    if (cc > t->max_cc) t->max_cc = cc;
    s.last_cc = cc;
    if (cc > 0 && in.dmg > t->damage) { t->damage = in.dmg; t->last_dmg = frame - t->start; }
    if (t->hits > 0 && cc == 0) {
        if (++t->zero_for >= kEndGrace) {
            Take old = std::move(*s.take);
            s.take.reset();
            if (replay_cut(old)) return old;
        }
    } else t->zero_for = 0;
    return std::nullopt;
}

inline std::string combos_text(size_t n) { return n == 1 ? "1 combo" : std::to_string(n) + " combos"; }

}  // namespace cc::take
