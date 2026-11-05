// Stage slots: tile or variation. See stage_classify.hpp.
#include "stage_classify.hpp"

#include <cctype>

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

StageKind classify_stage_variant(const std::string& ess_in, const std::vector<std::string>& paths, size_t new_files) {
    const std::string ess = lower(ess_in);
    const std::string env = "environment/stage/scene/ess/" + ess + "/" + ess + "_env.scn.";
    const std::string arena = "environment/stage/resource/ess/" + ess + "/" + ess + ".mesh.";
    const std::string arena00 = "environment/stage/resource/ess/" + ess + "/" + ess + "_00.mesh.";
    bool has_env = false, has_arena = false, has_mcol = false;
    for (auto& p0 : paths) {
        const std::string p = lower(p0);
        has_env = has_env || contains(p, env);
        has_arena = has_arena || contains(p, arena) || contains(p, arena00);
        has_mcol = has_mcol || contains(p, ".mcol.");
    }
    StageKind k;
    auto add = [&](const std::string& why) { k.reason += (k.reason.empty() ? "" : ", ") + why; };
    if (has_env) add("replaces the root prop scene");
    if (has_arena) add("replaces the arena mesh");
    if (has_mcol) add("ships collision");
    if (new_files >= 20) add(std::to_string(new_files) + " files new to the game");
    k.tile = !k.reason.empty();
    if (!k.tile) k.reason = "lighting, material, texture or effect files only";
    return k;
}

std::vector<StageOverride> parse_stage_overrides(const std::string& text_in, std::vector<std::string>* bad) {
    std::vector<StageOverride> out;
    std::string text = text_in;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            size_t close = line.rfind(']');
            if (close == std::string::npos || close < 2) { if (bad) bad->push_back(line); continue; }
            StageOverride o;
            o.section = trim(line.substr(1, close - 1));
            if (o.section.empty()) { if (bad) bad->push_back(line); continue; }
            out.push_back(o);
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || out.empty()) { if (bad) bad->push_back(line); continue; }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "kind") {
            const std::string v = lower(value);
            if (v == "tile" || v == "variation") out.back().kind = v;
            else if (bad) bad->push_back(line);
        } else if (key == "name") {
            out.back().name = value;
        } else if (bad) {
            bad->push_back(line);
        }
    }
    return out;
}

const StageOverride* find_stage_override(const std::vector<StageOverride>& list, const std::string& key,
                                         const std::string& name, const std::string& source) {
    const std::string src = lower(source);
    std::string top = src.substr(0, src.find('/'));
    std::string top_stem = top;
    if (size_t d = top_stem.find_last_of('.'); d != std::string::npos && d > 0) top_stem = top_stem.substr(0, d);
    const std::string k = lower(key), n = lower(name);
    const StageOverride* hit = nullptr;
    for (auto& o : list) {
        const std::string s = lower(o.section);
        if (s == k || s == n || s == src || s == top || s == top_stem) hit = &o;
    }
    return hit;
}

const char* kStageOverridesTemplate =
    "; SF6 Stage Slots: what each stage mod is, when the automatic guess is wrong.\n"
    ";\n"
    "; A mod that is really another stage (a full stage over the Training Room) becomes a TILE: its own entry in\n"
    "; Battle Settings > Other > Stage Settings and in Training's Change Stage. A mod that only tweaks its stage\n"
    "; (lighting, colours) is a VARIATION, cycled on that stage's entry. The guess is in\n"
    "; reframework\\data\\SF6_StageSlots_Data\\registry.json (\"kind\" and \"kind_reason\" of each variant).\n"
    ";\n"
    "; One section per mod: its name (as shown in the game), or its archive / folder name in stage_mods.\n"
    ";   kind=tile or kind=variation\n"
    ";   name=the name shown in the menus\n"
    "; Example:\n"
    ";   [SIRN Training Room]\n"
    ";   kind=variation\n"
    ";\n"
    ";   [Suzaku Castle Over Training Room]\n"
    ";   name=Suzaku Castle\n";
