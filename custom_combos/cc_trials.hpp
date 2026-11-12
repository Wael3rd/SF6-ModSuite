// cc_trials.hpp - Custom Combos: the trial files and the lists made from them, no game dependency.
//
// A fighter's folder is its list: reframework/data/SF6_CustomTrials_data/<Fighter>/*.json, one file per trial
// ("schema": "sf6.custom-trial.v1"). Read once per load by enumerating each fighter's folder (never the whole data tree).
// A file reduced to { "deleted": true } is a take the player deleted in game: removed from the disk here.
#pragma once

#include "cc_labels.hpp"

#include <algorithm>
#include <memory>

namespace cc {

// Fighter id -> folder name (the game's fighter ids)
struct FighterDir { int id; const char* dir; };
inline constexpr FighterDir kFighters[] = {
    { 1, "Ryu" }, { 2, "Luke" }, { 3, "Kimberly" }, { 4, "ChunLi" }, { 5, "Manon" }, { 6, "Zangief" }, { 7, "JP" },
    { 8, "Dhalsim" }, { 9, "Cammy" }, { 10, "Ken" }, { 11, "DeeJay" }, { 12, "Lily" }, { 13, "AKI" }, { 14, "Rashid" },
    { 15, "Blanka" }, { 16, "Juri" }, { 17, "Marisa" }, { 18, "Guile" }, { 19, "Ed" }, { 20, "EHonda" }, { 21, "Jamie" },
    { 22, "Akuma" }, { 25, "Sagat" }, { 26, "Bison" }, { 27, "Terry" }, { 28, "Mai" }, { 29, "Elena" }, { 30, "CViper" },
    { 31, "Alex" }, { 32, "Ingrid" }, { 33, "Yasmine" }, { 34, "Arjun" },
};
inline const char* fighter_dir(int id) {
    for (const auto& f : kFighters) if (f.id == id) return f.dir;
    return nullptr;
}

struct Step {
    std::string notation;
    std::vector<int64_t> ids;                  // every accepted action id (judged OR)
    std::vector<int64_t> file_ids;             // the file's own "action_ids" (end_delay looks them up); empty: none
    int64_t action_id = 0;                     // ids[0]
    int64_t check_type = 0, judge_type = 0, style_no = 0, trigger_no = 0, ex_action_type = 0;
    int64_t skill_id = 0;                      // the file's (0 = none)
    int64_t strength_type = 0, option1 = 0, option2 = 0, flags = 0, next_step = -1;
    std::optional<int64_t> progress_no, ext_no;
    std::string skill_name;                    // "skill": a skill by its name (resolved from the game's catalog)
    std::vector<std::string> skill_cmd;        // "skill_cmd": a skill by its Classic command, e.g. "236+K"
    std::string text;                          // "text": our own label (the game's tag syntax)
    bool has_text = false;
    bool blank = false;                        // the recording slot's empty rows
    // runtime
    int64_t skill = 0;                         // the skill resolved from skill_name / skill_cmd (0 = none yet)
    std::string own;                           // the text written into the row (no command in the control type on screen)
    int64_t shown_skill() const { return skill ? skill : skill_id; }
};

struct Trial;
using TrialP = std::shared_ptr<Trial>;

struct Trial {
    json j = json::object();                   // the file (demo, start, gauges, dummy_counter, dummy, unique, end_delay,
                                               // recipe, recorded): read where used, as the Lua did
    std::string title, path;                   // path: the file (empty for the slot)
    int fighter_id = 0;
    int level = 0;
    double order = 0;
    bool modern = false;                       // "control": "modern"
    bool rec = false;                          // the recording slot
    int file_order = 0;
    std::vector<Step> steps;
    // runtime, kept for the session (Lua: trial._*)
    int modern_seen = -1;                      // the control type the texts were laid for
    std::vector<std::string> texts;            // the rows' texts read from the game (long recipes)
    bool texts_done = false, texts_failed = false;
    int own_text = -1;                         // -1 unknown, 0 none of the steps has its own text, 1 some have
    struct Harvest { std::vector<int> offs; size_t i = 0; uint64_t since = 0; };
    std::optional<Harvest> harvest;
    int win_off = 0;
    std::optional<uint64_t> reset_wait;
};

// The ids of a step: an explicit list always wins, then the notation's group, then the single id.
inline void resolve_step(Step& st, const json& groups) {
    st.ids = st.file_ids;
    if (st.ids.empty() && !st.notation.empty()) {
        const json* g = arr(&groups, st.notation.c_str());
        if (g) for (const json& v : *g) if (auto d = num(v)) st.ids.push_back(static_cast<int64_t>(std::floor(*d)));
    }
    if (st.ids.empty() && st.action_id) st.ids.push_back(st.action_id);
    st.action_id = st.ids.empty() ? 0 : st.ids[0];
}

inline Step step_from(const json& s) {
    Step st;
    st.notation = str_of(at(s, "notation"));
    if (const json* a = arr(&s, "action_ids"))
        for (const json& v : *a) if (auto d = num(v)) st.file_ids.push_back(static_cast<int64_t>(std::floor(*d)));
    st.action_id = inum(at(s, "action_id"), 0);
    st.check_type = inum(at(s, "check_type"), 0);
    st.judge_type = inum(at(s, "judge_type"), 0);
    st.style_no = inum(at(s, "style_no"), 0);
    st.trigger_no = inum(at(s, "trigger_no"), 0);
    st.ex_action_type = inum(at(s, "ex_action_type"), 0);
    st.skill_id = inum(at(s, "skill_id"), 0);
    st.strength_type = inum(at(s, "strength_type"), 0);
    st.option1 = inum(at(s, "option1"), 0);
    st.option2 = inum(at(s, "option2"), 0);
    st.flags = inum(at(s, "flags"), 0);
    st.next_step = inum(at(s, "next_step"), -1);
    if (auto d = num(at(s, "progress_no"))) st.progress_no = static_cast<int64_t>(std::floor(*d));
    if (auto d = num(at(s, "ext_no"))) st.ext_no = static_cast<int64_t>(std::floor(*d));
    st.skill_name = str_of(at(s, "skill"));
    if (const json* c = at(s, "skill_cmd")) {
        if (c->is_string()) st.skill_cmd.push_back(c->get<std::string>());
        else if (c->is_array()) for (const json& v : *c) if (v.is_string()) st.skill_cmd.push_back(v.get<std::string>());
    }
    if (const json* t = at(s, "text"); t && t->is_string()) { st.text = t->get<std::string>(); st.has_text = true; }
    return st;
}

// A file exported by the trial editor may carry the game's own recipe, "recipe": { stage, teams, lines }. Without
// "steps", one step per ProgressNo is made from its lines: the ExtNo 0 line gives the displayed fields, every line of
// that ProgressNo an accepted id (the official alternatives, judged OR).
inline std::vector<Step> steps_from_recipe(const json& recipe) {
    std::vector<std::pair<double, std::vector<const json*>>> by;
    if (const json* lines = arr(&recipe, "lines"))
        for (const json& l : *lines) {
            const auto p = num(at(l, "ProgressNo"));
            const double pn = p ? *p : static_cast<double>(by.size());
            auto it = std::find_if(by.begin(), by.end(), [&](const auto& e) { return e.first == pn; });
            if (it == by.end()) { by.push_back({ pn, {} }); it = by.end() - 1; }
            it->second.push_back(&l);
        }
    std::stable_sort(by.begin(), by.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<Step> steps;
    for (auto& [pn, ls] : by) {
        std::stable_sort(ls.begin(), ls.end(), [](const json* a, const json* b) {
            return num(at(*a, "ExtNo")).value_or(0) < num(at(*b, "ExtNo")).value_or(0);
        });
        const json& h = *ls[0];
        Step st;
        for (const json* l : ls) if (auto d = num(at(*l, "ActionNo"))) st.file_ids.push_back(static_cast<int64_t>(std::floor(*d)));
        st.action_id = st.file_ids.empty() ? 0 : st.file_ids[0];
        st.check_type = inum(at(h, "CheckType"), 0);
        st.judge_type = inum(at(h, "JudgeType"), 0);
        st.style_no = inum(at(h, "StyleNo"), 0);
        st.trigger_no = inum(at(h, "TriggerNo"), 0);
        st.ex_action_type = inum(at(h, "ExActionType"), 0);
        st.skill_id = inum(at(h, "SkillID"), 0);
        st.strength_type = inum(at(h, "StrengthType"), 0);
        st.option1 = inum(at(h, "Option1Type"), 0);
        st.option2 = inum(at(h, "Option2Type"), 0);
        st.flags = inum(at(h, "Flags"), 0);
        st.next_step = inum(at(h, "NextStepNo"), -1);
        steps.push_back(std::move(st));
    }
    return steps;
}

// A trial from its file; nullptr when it has no step. `deleted`: the file is { "deleted": true } and nothing else.
inline TrialP trial_from(const json& t, const fs::path& path, int fighter, const Labels& L, bool* deleted) {
    if (deleted) *deleted = t.is_object() && t.size() == 1 && t.contains("deleted") && t["deleted"].is_boolean() && t["deleted"].get<bool>();
    if (!t.is_object()) return nullptr;
    auto tr = std::make_shared<Trial>();
    tr->j = t;
    if (const json* s = arr(&t, "steps")) {
        for (const json& v : *s) tr->steps.push_back(step_from(v.is_object() ? v : json::object()));
    } else if (const json* r = obj(&t, "recipe")) {
        tr->steps = steps_from_recipe(*r);
    }
    if (tr->steps.empty()) return nullptr;
    const json* title = at(&t, "title");
    if (title && title->is_string()) tr->title = title->get<std::string>();
    else if (title && title->is_number()) tr->title = key_of(title);
    else tr->title = u8(path.stem());
    tr->path = u8(path);
    tr->fighter_id = static_cast<int>(inum(at(&t, "fighter_id"), 0));
    tr->level = static_cast<int>(inum(at(&t, "level"), 0));
    if (tr->level < 0 || tr->level > 2) tr->level = 0;
    tr->order = num(at(&t, "order")).value_or(0);
    tr->modern = is_str(at(&t, "control"), "modern");
    for (Step& st : tr->steps) {
        resolve_step(st, L.groups);
        // a line no skill names shows its command instead of nothing
        if (st.skill_id == 0 && st.skill_name.empty() && st.skill_cmd.empty() && !st.has_text) {
            const std::string txt = step_text(L, st.notation, st.skill_id, static_cast<int>(st.strength_type), false);
            if (!txt.empty()) { st.text = txt; st.has_text = true; }
        }
    }
    (void)fighter;
    return tr;
}

// The ids every command of the fighter names (follow-up actions are the ones no command names)
inline std::vector<int64_t> command_ids(const Labels& L) {
    std::vector<int64_t> ids;
    for (auto it = L.groups.begin(); it != L.groups.end(); ++it)
        if (it->is_array()) for (const json& v : *it) if (auto d = num(v)) ids.push_back(static_cast<int64_t>(std::floor(*d)));
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// The signature the recorder writes: "notation/check" joined by ">", "M:" first in Modern
inline std::string signature(const Trial& t) {
    std::string sig = t.modern ? "M:" : "";
    for (size_t i = 0; i < t.steps.size(); ++i) {
        if (i) sig += ">";
        sig += t.steps[i].notation + "/" + std::to_string(t.steps[i].check_type);
    }
    return sig;
}

// The title shown in the list: the player's own text, then the combo's damage in brackets ("MP MAX COMBO (5731)"). The
// damage is the example's when it was built for this very recipe (demo.built, demo["for"]), else the take's when the
// recipe is still the take's (recorded.signature); a recipe edited since shows no figure. A title the recorder wrote
// with the damage inside ("REC 12/11 11:34 - 3731 dmg", older takes) loses that part.
inline std::string shown_title(const Trial& t) {
    std::string title = t.title;
    {   // gsub(" %- %d+ dmg$", "")
        const std::string tail = " dmg";
        if (title.size() > tail.size() && title.compare(title.size() - tail.size(), tail.size(), tail) == 0) {
            size_t k = title.size() - tail.size();
            size_t d = k;
            while (d > 0 && text::digit(title[d - 1])) --d;
            if (d < k && d >= 3 && title.compare(d - 3, 3, " - ") == 0) title.resize(d - 3);
        }
    }
    if (t.rec) return title;
    const std::string sig = signature(t);
    std::optional<double> dmg;
    const json* demo = obj(&t.j, "demo");
    const json* rec = obj(&t.j, "recorded");
    if (demo && obj(demo, "built") && is_str(at(demo, "for"), sig.c_str())) dmg = num(at(obj(demo, "built"), "damage"));
    else if (rec && is_str(at(rec, "signature"), sig.c_str())) dmg = num(at(rec, "damage"));
    if (dmg && *dmg > 0) title += " (" + std::to_string(static_cast<int64_t>(std::floor(*dmg))) + ")";
    return title;
}

// The row title of a list of that control type: the other type's trials say which they are
inline std::string row_title(const Trial& t, bool modern_list) {
    std::string prefix;
    if (!t.rec && t.modern != modern_list) prefix = modern_list ? "[Classic] " : "[Modern] ";
    return prefix + shown_title(t);
}

// One tab of one control type: a file's "level" puts it in that tab; each control type's lists show its own trials
// first (and the slot), the others after them, then by "order", then by file. extra_rows false: a tab holds at most
// the game's own number of trials (caps), its last ones spill into the next tab; past the last tab they are not shown.
struct LevelSet { std::vector<TrialP> rows; int size = 0; /* the official count of the list built from it */ };

inline std::array<LevelSet, 3> level_sets(const std::vector<TrialP>& all, bool modern, const int* caps /* 3 or null */) {
    std::array<LevelSet, 3> by;
    for (const TrialP& t : all) by[t->level >= 0 && t->level <= 2 ? t->level : 0].rows.push_back(t);
    auto before = [modern](const TrialP& a, const TrialP& b) {
        const int fa = (a->rec || a->modern == modern) ? 0 : 1, fb = (b->rec || b->modern == modern) ? 0 : 1;
        if (fa != fb) return fa < fb;
        if (a->order != b->order) return a->order < b->order;
        return a->file_order < b->file_order;
    };
    for (auto& l : by) std::stable_sort(l.rows.begin(), l.rows.end(), before);
    if (caps)
        for (int l = 0; l <= 1; ++l)
            while (static_cast<int>(by[l].rows.size()) > caps[l]) {
                by[l + 1].rows.insert(by[l + 1].rows.begin(), by[l].rows.back());
                by[l].rows.pop_back();
            }
    return by;
}

// The wait between the last line clearing and the end of the trial (EndDelayTime): the file's "end_delay", or what
// the fighter's official trials use for the same last action or skill (_end_delays.json), whichever is longer, 900 at most.
inline int64_t end_delay(const Trial& t, const json& delays) {
    int64_t best = std::max<int64_t>(0, inum(at(&t.j, "end_delay"), 0));
    const json* f = obj(&delays, std::to_string(t.fighter_id).c_str());
    if (f && !t.steps.empty()) {
        const Step& last = t.steps.back();
        const json* a = obj(f, "a");
        const json* s = obj(f, "s");
        const std::vector<int64_t> ids = last.file_ids.empty() ? std::vector<int64_t>{ last.action_id } : last.file_ids;
        if (a) for (int64_t id : ids) best = std::max(best, inum(at(a, std::to_string(id).c_str()), 0));
        if (s) best = std::max(best, inum(at(s, std::to_string(last.skill_id).c_str()), 0));
    }
    return std::min<int64_t>(best, 900);
}

// A skill by its Classic command: the command text's tags only, notes in parentheses dropped. Buttons are
// <CMDBTN CmnP|LowP|MidP|HighP|...K>, so "<CMD _236><ICON +><CMDBTN CmnK>" reads "236+K", "<ICON 4><ICON +><CMDBTN HighP>"
// reads "4+HP". Exact match only.
inline std::string cmd_key(const std::string& cmd_text) {
    std::string s;
    for (size_t i = 0; i < cmd_text.size();) {                // gsub("%b()", "")
        if (cmd_text[i] == '(') {
            const size_t k = text::balanced(cmd_text, i);
            if (k != std::string::npos) { i = k; continue; }
        }
        s += cmd_text[i++];
    }
    std::string out;
    for (size_t i = 0; i < s.size();) {                       // gmatch("<(.-)>")
        const size_t a = s.find('<', i);
        if (a == std::string::npos) break;
        const size_t b = s.find('>', a + 1);
        if (b == std::string::npos) break;
        const std::string tag = s.substr(a + 1, b - a - 1);
        i = b + 1;
        auto all_digits = [](const std::string& x) { if (x.empty()) return false; for (char c : x) if (!text::digit(c)) return false; return true; };
        if (tag.rfind("CMD _", 0) == 0 && all_digits(tag.substr(5))) { out += tag.substr(5); continue; }
        if (tag.rfind("ICON ", 0) == 0) {
            const std::string icon = tag.substr(5);
            if (icon == "+") { out += "+"; continue; }
            if (icon.size() == 1 && text::digit(icon[0])) { out += icon; continue; }
            if (!icon.empty()) continue;
        }
        if (tag.rfind("CMDBTN ", 0) == 0) {
            const std::string name = tag.substr(7);
            bool letters = !name.empty();
            for (char c : name) if (!text::alpha(c)) letters = false;
            const char last = name.empty() ? 0 : name.back();
            if (letters && (last == 'P' || last == 'K')) {
                const std::string lvl = name.substr(0, name.size() - 1);
                const char* l = lvl == "Cmn" ? "" : lvl == "Low" ? "L" : lvl == "Mid" ? "M" : lvl == "High" ? "H" : nullptr;
                if (l) { out += l; out += last; continue; }
            }
            std::string up = name;
            for (char& c : up) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
            out += up;
        }
    }
    return out;
}

}  // namespace cc
