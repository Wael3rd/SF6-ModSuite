#include "fighter_names.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "pak.hpp"
#include "../costumes/loader/structural_msg.hpp"

namespace fs = std::filesystem;

static const char* const kNameMsg = "natives/stm/product/message/cmn/001/wordlist/fighternamemessage.msg.21";
static const char kPrefix[] = "FighterNameMessage";

static std::map<int, std::string> read_names(const std::string& game_dir) {
    std::map<int, std::string> result;
    std::error_code ec;
    const fs::path dir(game_dir);   // narrow path, like PakReader::open
    const fs::path base = dir / "re_chunk_000.pak";
    if (!fs::exists(base, ec)) return result;

    // Lookup order: the official patch paks, newest first, then the base pak.
    std::vector<std::string> patches;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.size() > 22 && n.compare(0, 22, "re_chunk_000.pak.patch") == 0 &&
            n.compare(n.size() - 4, 4, ".pak") == 0)
            patches.push_back(it->path().string());
    }
    std::sort(patches.rbegin(), patches.rend());
    patches.push_back(base.string());

    const uint64_t marker_costume = pak_path_hash(std::string_view(COSTUME_SLOTS_MARKER));
    const uint64_t marker_stage = pak_path_hash(std::string_view(STAGE_SLOTS_MARKER));
    const uint64_t want = pak_path_hash(std::string_view(kNameMsg));

    for (const std::string& path : patches) {
        PakReader pak;
        if (!pak.open(path.c_str())) continue;
        if (pak.find(marker_costume) || pak.find(marker_stage)) continue;   // a loader's own pak
        const PakEntry* e = pak.find(want);
        if (!e) continue;
        std::vector<uint8_t> data = pak.read(*e);
        std::map<std::string, std::string> texts;
        if (data.empty() || !read_msg_texts(data, 1, texts)) return result;   // English = via.Language 1
        for (const auto& [name, text] : texts) {
            if (text.empty() || name.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) continue;
            const std::string digits = name.substr(sizeof(kPrefix) - 1);
            if (digits.empty() || digits.size() > 5 ||
                !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
                continue;
            result[std::atoi(digits.c_str())] = text;
        }
        return result;
    }
    return result;
}

std::map<int, std::string> game_fighter_names(const std::string& game_dir) {
    try {
        return read_names(game_dir);
    } catch (...) {
        return {};
    }
}

std::string fighter_folder_name(const std::string& english) {
    std::string s;
    for (unsigned char c : english) {
        if (c < 0x20 || std::string_view("<>:\"/\\|?*").find(static_cast<char>(c)) != std::string_view::npos) continue;
        s.push_back(static_cast<char>(c));
    }
    auto trim = [&]() {
        while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
        size_t a = 0;
        while (a < s.size() && s[a] == ' ') ++a;
        s.erase(0, a);
    };
    trim();
    return s;
}
