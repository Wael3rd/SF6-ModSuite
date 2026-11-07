// Opponent Outfit Settings: the RW / MC colours ticked there. Pure (no game access); tests: tests/opponent_ticks_test.cpp.
//
// The game's own save holds colour numbers only (OtherFighterSettingPerCostume.ValidColorIdList), and an entry is not one: it rides on colour 02 (id 1). So
// the ticks of entries live in a file of ours and the game's save never holds anything but real colours: a save with no entry ticked is exactly the game's,
// and removing the mod leaves nothing behind. One line per tick: fighter, costume, the entry's id (its stable path), separated by tabs.
//
// At a fight, the game picks evenly among every (outfit, colour) ticked for that fighter. Each ticked entry counts there as colour 1 of its outfit, once per
// tick, for the time of the game's pick only. When the pick lands on colour 1 of an outfit, what it means is settled here (look_after_pick), so every tick
// is as likely as any other.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace engine::opponent {

constexpr const char* kFileHeader = "SF6 Color Mod opponent colours 1";

class Ticks {
public:
    bool has(int fighter, int costume, const std::string& id) const {
        auto it = t_.find({ fighter, costume });
        return it != t_.end() && it->second.count(id) != 0;
    }
    // Returns whether it is ticked now
    bool toggle(int fighter, int costume, const std::string& id) {
        set(fighter, costume, id, !has(fighter, costume, id));
        return has(fighter, costume, id);
    }
    void set(int fighter, int costume, const std::string& id, bool on) {
        if (fighter < 0 || costume < 0 || id.empty()) return;
        const std::pair<int, int> key{ fighter, costume };
        if (on) { t_[key].insert(id); return; }
        auto it = t_.find(key);
        if (it == t_.end()) return;
        it->second.erase(id);
        if (it->second.empty()) t_.erase(it);
    }
    // The entries ticked for an outfit, in a stable order
    std::vector<std::string> of(int fighter, int costume) const {
        std::vector<std::string> out;
        auto it = t_.find({ fighter, costume });
        if (it != t_.end()) out.assign(it->second.begin(), it->second.end());
        return out;
    }
    int count(int fighter, int costume) const {
        auto it = t_.find({ fighter, costume });
        return it == t_.end() ? 0 : static_cast<int>(it->second.size());
    }
    // The outfits of a fighter with an entry ticked
    std::vector<int> costumes(int fighter) const {
        std::vector<int> out;
        for (const auto& [key, ids] : t_) if (key.first == fighter && !ids.empty()) out.push_back(key.second);
        return out;
    }
    bool any(int fighter) const { return !costumes(fighter).empty(); }
    void clear(int fighter, int costume) { t_.erase({ fighter, costume }); }

    std::string to_text() const {
        std::string out = std::string(kFileHeader) + "\n";
        for (const auto& [key, ids] : t_)
            for (const std::string& id : ids) out += std::to_string(key.first) + "\t" + std::to_string(key.second) + "\t" + id + "\n";
        return out;
    }
    // False, and nothing kept, for another format. A line that can't be read is left out.
    bool from_text(const std::string& text) {
        t_.clear();
        std::istringstream in(text);
        std::string line;
        if (!std::getline(in, line)) return true;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line != kFileHeader) return false;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t a = line.find('\t');
            const size_t b = a == std::string::npos ? std::string::npos : line.find('\t', a + 1);
            if (b == std::string::npos || b + 1 >= line.size()) continue;
            int f = -1, c = -1;
            try {
                size_t used = 0;
                f = std::stoi(line.substr(0, a), &used);
                if (used != a) continue;
                c = std::stoi(line.substr(a + 1, b - a - 1), &used);
                if (used != b - a - 1) continue;
            } catch (...) {
                continue;
            }
            set(f, c, line.substr(b + 1), true);
        }
        return true;
    }

private:
    std::map<std::pair<int, int>, std::set<std::string>> t_;
};

// The page's list of an outfit changed (its colour ids, before and after): what the player did. The game only knows colour numbers, so a tick on an entry's cell
// toggles colour 1 in that list.
//   kEntry:   a tick on the entry's cell under the cursor (only colour 1 changed, and the cursor is on an entry)
//   kAllOn:   the "all the outfit's colours" key turned them on (several of 01-10 added)
//   kAllOff:  that key turned them off (several of 01-10 gone, or one that isn't what the cursor is on)
//   kGame:    anything else: a tick on a colour of the game, the EX / DX key
enum class Change { kNone, kEntry, kAllOn, kAllOff, kGame };
constexpr int kBaseColorId = 1, kOutfitColors = 10;     // ids 0-9 are the colours 01-10
inline Change classify(const std::vector<int>& before, const std::vector<int>& after, bool cursor_on_entry, int cursor_color) {
    const std::set<int> b(before.begin(), before.end()), a(after.begin(), after.end());
    if (a == b) return Change::kNone;
    std::vector<int> added, removed;
    for (int id : a) if (!b.count(id)) added.push_back(id);
    for (int id : b) if (!a.count(id)) removed.push_back(id);
    const auto outfit_colors = [](const std::vector<int>& ids) {
        int n = 0;
        for (int id : ids) if (id >= 0 && id < kOutfitColors) ++n;
        return n;
    };
    const size_t changed = added.size() + removed.size();
    if (changed == 1 && (added.empty() ? removed[0] : added[0]) == kBaseColorId && cursor_on_entry) return Change::kEntry;
    if (outfit_colors(added) > 1) return Change::kAllOn;
    if (outfit_colors(removed) > 1) return Change::kAllOff;
    if (added.empty() && removed.size() == 1 && outfit_colors(removed) == 1 && (cursor_on_entry || removed[0] != cursor_color)) return Change::kAllOff;
    return Change::kGame;
}

// The game's pick landed on colour 1 of an outfit: what it wears. "" = the plain colour 02.
//   plain_ticked: colour 02 itself is ticked for that outfit
//   entries:      the entries ticked for it that still exist
//   own:          what the player wears, when the same fighter and outfit on colour 1: not the same look as the player when there's another (the game's rule)
//   last:         what this fighter wore the last time on this colour: not twice in a row when there's another (the game's rule)
//   roll:         a random number
// With nothing to choose from (every entry gone, colour 02 not ticked): "", and the caller says so.
inline std::string look_after_pick(bool plain_ticked, const std::vector<std::string>& entries, const std::optional<std::string>& own,
                                   const std::optional<std::string>& last, uint32_t roll) {
    std::vector<std::string> looks;
    if (plain_ticked) looks.push_back(std::string());
    looks.insert(looks.end(), entries.begin(), entries.end());
    if (looks.empty()) return std::string();
    const auto without = [&](const std::optional<std::string>& not_this) {
        if (!not_this || looks.size() < 2) return;
        std::vector<std::string> kept;
        for (const std::string& l : looks) if (l != *not_this) kept.push_back(l);
        if (!kept.empty()) looks = std::move(kept);
    };
    without(own);
    without(last);
    return looks[roll % looks.size()];
}

}  // namespace engine::opponent
