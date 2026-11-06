// Unit test of plugin/src/words.hpp and of what the plugin's pure code writes with it. Builds with build_words_test.bat.
#include "../src/replay_logic.hpp"
#include "../src/stages_logic.hpp"
#include "../src/words.hpp"

#include <cstdio>
#include <set>
#include <string>

using namespace slots;
using namespace slots::replay;
using words::G;
using words::W;

static int g_failed = 0, g_checks = 0;
static void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAILED: %s\n", what);
    }
}

// A game of pretence: its messages, as via.gui.message.get would answer in French ("" for what is "not loaded yet")
static bool g_game_loaded = false;
static std::string french_game(const char* guid) {
    if (!g_game_loaded) return std::string();
    const std::string g = guid;
    for (int i = 0; i < words::kGameCount; ++i) {
        if (g != words::kGameWords[i].guid) continue;
        const std::string key = words::kGameWords[i].key;
        if (key == "on") return "Oui";
        if (key == "off") return "Non";
        if (key == "default") return "Par d\xC3\xA9" "faut";
        if (key == "outfit.1") return "Type 1";
        if (key == "outfit.drivetech") return "V\xC3\xAAtements DriveTech";
        if (key == "outfit.n") return "Type {0}";
        if (key == "color.ex") return "EX {0}";
        return "fr:" + key;
    }
    for (int i = 0; i < words::kFighterCount; ++i) {
        if (g == words::kFighterWords[i].name_guid) return std::string("name:") + words::kFighterWords[i].english;
        if (g == words::kFighterWords[i].caption_guid) return std::string("CAPTION:") + words::kFighterWords[i].english;
    }
    return std::string();
}

int main() {
    // ---- the table ----
    check(words::kLangCount == 14 && std::string(words::kLangs[0]) == "en", "14 languages, English first (the fallback)");
    for (int w = 0; w < words::kWordCount; ++w)
        for (int l = 0; l < words::kLangCount; ++l) {
            const char* t = words::kText[l][w];
            check(t && *t, "every word in every language");
        }
    {
        std::set<std::string> guids;
        for (int i = 0; i < words::kGameCount; ++i) check(guids.insert(words::kGameWords[i].guid).second && *words::kGameWords[i].english, "a game word's GUID is unique, it has its English");
        uint8_t b[16];
        for (int i = 0; i < words::kGameCount; ++i) check(parse_guid(words::kGameWords[i].guid, b), "a game word's GUID parses");
        for (int i = 0; i < words::kFighterCount; ++i) check(parse_guid(words::kFighterWords[i].name_guid, b) && parse_guid(words::kFighterWords[i].caption_guid, b), "a fighter's GUIDs parse");
    }

    // ---- the words of ours: the language, the fallback ----
    check(std::string(words::tr(W::ReplayRowMusic)) == "Replay Music", "English by default");
    check(std::string(words::tr_in(W::ReplayRowMusic, "fr")) == "Musique des replays" &&
          std::string(words::tr_in(W::ReplayCustom, "fr")) == "Personnalis\xC3\xA9" "e" &&
          std::string(words::tr_in(W::ReplayCustomList, "fr")) == "Liste personnalis\xC3\xA9" "e", "Wael's French");
    check(std::string(words::tr_in(W::ReplayCustom, "nl")) == "Custom" && std::string(words::tr_in(W::ReplayCustom, "")) == "Custom", "a language not there: English");

    // ---- format ----
    check(words::format("%s: %s, %s", { "Luke", "Outfit 4", "01" }) == "Luke: Outfit 4, 01", "format: in order");
    check(words::format("+ %d more", { "6" }) == "+ 6 more", "format: %d");
    check(words::format("100%% %s", { "a%sb" }) == "100% a%sb", "format: %% and an argument holding a slot");
    check(words::format("%s %s", { "a" }) == "a ", "format: an argument missing leaves its place empty");

    // ---- the words of the game ----
    words::set_lang("fr");
    check(std::string(words::lang()) == "fr" && std::string(words::tr(W::ReplayStageDefault)) == "PAR D\xC3\x89" "FAUT", "French: our word");
    check(words::game(G::On) == "On" && words::fighter_name(1) == "Ryu" && words::fighter_caption(30) == "C. VIPER", "a message not known yet: the game's English");
    words::resolve_game_words(french_game);
    check(words::game(G::On) == "On", "the message file not loaded yet: English still");
    g_game_loaded = true;
    words::resolve_game_words(french_game);
    check(words::game(G::On) == "Oui" && words::game(G::Off) == "Non", "the game's word, once found");
    check(words::fighter_name(1) == "name:Ryu" && words::fighter_caption(1) == "CAPTION:Ryu", "the fighter's two names, once found");
    check(words::fighter_name(23).empty() && words::fighter_caption(23).empty(), "a fighter not in the table: none");

    // ---- the outfits ----
    check(words::outfit_name(0) == "Type 1" && words::outfit_name(4) == "V\xC3\xAAtements DriveTech" && words::outfit_name(7) == "Type 7", "the game's outfit names by costume number");
    check(words::outfit_label("Outfit 1") == "Type 1" && words::outfit_label("DriveTech") == "V\xC3\xAAtements DriveTech" && words::outfit_label("Drive Tech") == "V\xC3\xAAtements DriveTech" && words::outfit_label("Outfit 9") == "Type 9", "the loader's English label of a game outfit: the game's name");
    check(words::outfit_label("Outfit VII") == "Outfit VII" && words::outfit_label("Aloha Ryu") == "Aloha Ryu" && words::outfit_label("Outfit") == "Outfit", "any other label stays as it is");
    check(game_outfit_name(0) == "Type 1" && color_label(100) == "EX 1" && color_label(0) == "01", "the page's names");

    // ---- what the pure code writes, in French ----
    {
        Fighter luke;
        luke.fid = 2; luke.name = "Luke";
        luke.outfits = { { 3, "", "Type 4", false, { 0, 1, 2 } } };
        Choice c; c.forced = true; c.costume = 3; c.color = 0;
        check(summary_line(luke, c) == "Luke : Type 4, 01", "a summary line, French punctuation");
        Choice gone = c; gone.costume = 9;
        check(summary_line(luke, gone) == "Luke : Par d\xC3\xA9" "faut", "an outfit no longer listed");
        check(summary_lines({ luke }, {}, 20) == std::vector<std::string>{ "Par d\xC3\xA9" "faut" }, "nobody On");
        std::vector<Fighter> many;
        Choices cs;
        for (int i = 0; i < 25; ++i) {
            Fighter one = luke; one.fid = 100 + i; one.name = "F" + std::to_string(i);
            many.push_back(one);
            cs[one.fid] = c;
        }
        check(summary_lines(many, cs, 20).back() == "+ 6 autres", "the last line says how many more");
        check(row_text(luke, Choice{}) == "Default", "the log's line stays in English");
    }
    check(slots::stagelogic::variation_hint_text("<ICON KeyR>") == "<ICON KeyR> Variante du stage", "the guide bar's Stage Variation");

    // ---- sentences ----
    check(words::sentences("A.", "B.") == "A. B.", "sentences: a space");
    words::set_lang("ja");
    check(words::sentences("A", "B") == "AB", "sentences: none in Japanese");
    check(words::game(G::On) == "On", "another language: the old language's words are gone");
    words::set_lang("zh-Hans");
    check(words::sentences("A", "B") == "AB", "sentences: none in Chinese");
    const uint32_t v = words::version();
    words::set_lang("zh-Hans");
    check(words::version() == v, "the same language: nothing to write again");
    words::set_lang("de");
    check(words::version() == v + 1, "another language: written again");
    words::set_lang("en");

    // ---- units, cuts ----
    check(words::units("abc") == 3 && words::units("\xC3\xA9") == 1 && words::units("\xE8\xA8\xAD\xE5\xAE\x9A") == 4 && words::units("\xEC\x84\xA4") == 2 && words::units("\xD0\x90") == 1, "units: latin, accents, CJK, Hangul, Cyrillic");
    check(words::cut_units("abcdefghij", 10) == "abcdefghij" && words::cut_units("abcdefghijk", 10) == "abcdefg...", "cut: at most 10 units, ... included");
    check(words::cut_units("\xE8\xA8\xAD\xE5\xAE\x9A\xE8\xA8\xAD\xE5\xAE\x9A\xE8\xA8\xAD\xE5\xAE\x9A", 10) == "\xE8\xA8\xAD\xE5\xAE\x9A\xE8\xA8\xAD..." , "cut: whole CJK characters");
    check(words::units(words::cut_units("\xD0\x90\xD0\x91\xD0\x92\xD0\x93\xD0\x94\xD0\x95\xD0\x96\xD0\x97\xD0\x98\xD0\x99\xD0\x9A\xD0\x9B", 10)) == 10, "cut: Cyrillic by characters, not bytes");

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
