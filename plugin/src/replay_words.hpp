// SF6 Slots — Replays > Settings: the words of ours that follow the game's language. Pure (plugin/tests). A language not here, or a
// word left out (nullptr): English. Non-ASCII bytes as escapes: the build has no source encoding of its own.
#pragma once

#include <string>

namespace slots::replay {

enum class Word { ReplayMusic = 0, Custom, CustomMusicList, Count };

struct WordRow { const char* lang; const char* text[static_cast<int>(Word::Count)]; };
inline constexpr WordRow kWords[] = {
    { "en", { "Replay Music", "Custom", "Custom Music List" } },    // first: the fallback
    { "fr", { "Musique des replays", "Personnalis\xC3\xA9" "e", "Liste personnalis\xC3\xA9" "e" } },
};

// `lang`: slots::game_language()'s code ("en", "fr", ...)
inline const char* word(Word w, const std::string& lang) {
    const int i = static_cast<int>(w);
    for (const WordRow& r : kWords)
        if (lang == r.lang && r.text[i]) return r.text[i];
    return kWords[0].text[i];
}

}  // namespace slots::replay
