// Zen Mode's pure part (no game, no Windows): the rule table, the Lua pattern items it uses, the walk, the settings and their file,
// the schedule. Tested by logic_test.cpp.
//
// The rules are zen_rules.lua (ZenMode.lua's `exclude` table at first, its containers split since), turned into zen_rules.inc by
// tools/zen/gen_zen_rules.py: for a GUI GameObject's name, paths of control names from its view; the control at the end of a path
// is hidden (ForceInvisible). A path segment is a Lua pattern, not anchored; every sibling that matches is followed.
// An owner whose one rule is the view's own child is hidden as a whole: that element is not drawn at all (whole_owner).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace zen {

struct Rule { uint8_t cat; uint8_t n; const char* const* path; };
struct Owner { const char* name; const Rule* rules; uint8_t n; };

#include "zen_rules.inc"

inline int cat_index(const char* name) {
    for (int i = 0; i < kCatCount; ++i) if (!strcmp(kCatNames[i], name)) return i;
    return -1;
}
inline int owner_index(const char* name) {
    int lo = 0, hi = kOwnerCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2, c = strcmp(name, kOwners[mid].name);
        if (!c) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

// ---- the Lua pattern items the table uses: plain text, %d (a digit), %d+ (digits, as many as it can, giving back) ----
inline bool seg_here(const char* s, const char* p) {
    for (;;) {
        if (!*p) return true;
        if (p[0] == '%' && p[1] == 'd') {
            if (p[2] == '+') {
                int n = 0;
                while (s[n] >= '0' && s[n] <= '9') ++n;
                for (; n >= 1; --n) if (seg_here(s + n, p + 3)) return true;
                return false;
            }
            if (!(*s >= '0' && *s <= '9')) return false;
            ++s; p += 2;
            continue;
        }
        if (*s != *p) return false;
        ++s; ++p;
    }
}
// string.match(name, pat) ~= nil
inline bool seg_match(const char* name, const char* pat) {
    if (!strchr(pat, '%')) return strstr(name, pat) != nullptr;
    for (const char* s = name;; ++s) {
        if (seg_here(s, pat)) return true;
        if (!*s) return false;
    }
}

// ---- the walk: ZenMode.lua's hide_path for every rule of an owner at once (a name read once per control) ----
// Tree: Node child(Node), Node next(Node), bool name(Node, char*, size_t); a Node is false when there is none.
// sink(node, rule): the control at the end of that rule's path. `budget`: controls looked at, at most.
template <class Tree, class Node, class Sink>
void walk(Tree& t, Node node, const Owner& o, uint32_t mask, int depth, Sink& sink, int& budget) {
    uint32_t cont = 0;
    for (uint32_t i = 0; i < o.n; ++i) {
        if (!((mask >> i) & 1u)) continue;
        if (o.rules[i].n == depth) sink(node, static_cast<int>(i));
        else cont |= 1u << i;
    }
    if (!cont) return;
    char name[128];
    for (Node c = t.child(node); c && budget > 0; c = t.next(c)) {
        --budget;
        if (!t.name(c, name, sizeof name)) continue;
        uint32_t m = 0;
        for (uint32_t i = 0; i < o.n; ++i)
            if (((cont >> i) & 1u) && seg_match(name, o.rules[i].path[depth])) m |= 1u << i;
        if (m) walk(t, c, o, m, depth + 1, sink, budget);
    }
}
inline uint32_t all_rules(const Owner& o) { return o.n >= 32 ? 0xFFFFFFFFu : ((1u << o.n) - 1u); }
// One rule, the view's own child (c_main): the whole element. It is not drawn instead of being written to: the game itself
// writes ForceInvisible on its HUD's c_main (off again at every round's start, measured 11/11), so a write there does not last.
inline bool whole_owner(const Owner& o) { return o.n == 1 && o.rules[0].n == 1; }

// ---- the settings ----
// The file keeps the categories (ZenMode.lua's names); the screen shows them as five groups.
enum Group { kRanks, kPoints, kStreak, kNames, kDetails, kGroupCount };
inline constexpr const char* kGroupCats[kGroupCount][5] = {
    { "Rank icon", "Rank gauge", "Rank-up chance", "Rank-up animation", nullptr },
    { "League points", "Master rate", nullptr },
    { "Win streak", nullptr },
    { "Player name", "Player title", "Team / club", nullptr },
    { "National flag", "Platform icon", "Control type", nullptr },
};
inline int group_of(int cat) {
    for (int g = 0; g < kGroupCount; ++g)
        for (const char* const* c = kGroupCats[g]; *c; ++c)
            if (cat >= 0 && cat < kCatCount && !strcmp(*c, kCatNames[cat])) return g;
    return -1;
}

struct State {
    bool enabled = false;            // Zen Mode itself: off until the player turns it on
    bool hidden[kCatCount];          // a category hidden while it is on: all of them at first, as the Lua
    State() { for (bool& h : hidden) h = true; }
    bool operator==(const State& o) const { return enabled == o.enabled && !memcmp(hidden, o.hidden, sizeof hidden); }
};
inline bool wants_hidden(const State& s, int cat) { return s.enabled && cat >= 0 && cat < kCatCount && s.hidden[cat]; }

// ---- whose ForceInvisible it is ----
// The game hides with ForceInvisible too (the rank-up notice of the player who has none, a row's lock): a control found hidden
// is the game's, and is never shown by us. Only what Zen Mode hid itself is shown again when it is no longer to be hidden, along
// with the copies the game made of it meanwhile (a list's new row is a copy of a row: it comes hidden when that row was).
// With Zen Mode off, or a group on Shown, since a screen came: nothing of it is ever written.
enum class Mark : uint8_t { None, Ours, Game };      // shown, nothing to know / hidden by Zen Mode / hidden by the game
struct Act { bool write; bool value; Mark mark; };   // what to write, if anything, and what the control is from now on
// known: what was kept on that control. invisible: its ForceInvisible now. hide: the settings want it hidden. copy: under its
// rule, on its screen, controls were hidden by us before this look and none by the game (told by copy_rule).
inline Act decide(Mark known, bool invisible, bool hide, bool copy) {
    Mark m = known;
    if (!invisible) m = Mark::None;                                       // the game put it back, or it never was hidden
    else if (m == Mark::None) m = copy ? Mark::Ours : Mark::Game;        // found hidden
    if (hide) return invisible ? Act{ false, true, m } : Act{ true, true, Mark::Ours };
    if (m == Mark::Ours) return { true, false, Mark::None };
    return { false, false, m };
}
// A control found hidden that nothing is known of: a copy of one of ours only under a rule where the game hides nothing itself
// (where it does, it writes its rows one by one: hidden is its word). ours, game: the screen's rules with such controls, as bits
inline bool copy_rule(uint32_t ours, uint32_t game, int rule) { return rule >= 0 && rule < 32 && ((ours & ~game) >> rule & 1u) != 0; }

enum class GroupValue { Hidden, Shown, Mixed };
inline GroupValue group_value(const State& s, int g) {
    int hid = 0, n = 0;
    for (int c = 0; c < kCatCount; ++c)
        if (group_of(c) == g) { ++n; hid += s.hidden[c] ? 1 : 0; }
    return hid == n ? GroupValue::Hidden : hid == 0 ? GroupValue::Shown : GroupValue::Mixed;
}
inline void set_group(State& s, int g, bool hidden) {
    for (int c = 0; c < kCatCount; ++c) if (group_of(c) == g) s.hidden[c] = hidden;
}
// A step on a group's row: Hidden <-> Shown; a group whose categories differ (a file edited by hand) becomes Hidden first
inline void step_group(State& s, int g) { set_group(s, g, group_value(s, g) != GroupValue::Hidden); }

// ---- the file: {"enabled": bool, "hidden": {"<category>": bool}} ----
// ZenMode.lua's ZenMode_config.json is the same without "enabled": {"hidden": {...}}, a missing category = hidden.
namespace json {
struct Cur { const char* p; const char* e; };
inline void ws(Cur& c) { while (c.p < c.e && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r' || *c.p == '\n')) ++c.p; }
inline bool ch(Cur& c, char x) { ws(c); if (c.p < c.e && *c.p == x) { ++c.p; return true; } return false; }
inline bool str(Cur& c, std::string& out) {
    ws(c);
    if (c.p >= c.e || *c.p != '"') return false;
    ++c.p;
    out.clear();
    while (c.p < c.e && *c.p != '"') {
        if (*c.p == '\\') {
            if (++c.p >= c.e) return false;
            if (*c.p == 'u') { if (c.e - c.p < 5) return false; c.p += 4; out += '?'; }
            else out += (*c.p == 'n' ? '\n' : *c.p == 't' ? '\t' : *c.p);
        } else out += *c.p;
        ++c.p;
    }
    if (c.p >= c.e) return false;
    ++c.p;
    return true;
}
inline bool word(Cur& c, const char* w) {
    ws(c);
    const size_t n = strlen(w);
    if (static_cast<size_t>(c.e - c.p) < n || strncmp(c.p, w, n) != 0) return false;
    c.p += n;
    return true;
}
// Any value, skipped. depth: nesting allowed below.
inline bool skip(Cur& c, int depth = 16) {
    ws(c);
    if (c.p >= c.e || depth < 0) return false;
    if (*c.p == '"') { std::string s; return str(c, s); }
    if (*c.p == '{' || *c.p == '[') {
        const char close = *c.p == '{' ? '}' : ']';
        const bool obj = close == '}';
        ++c.p;
        if (ch(c, close)) return true;
        for (;;) {
            if (obj) { std::string k; if (!str(c, k) || !ch(c, ':')) return false; }
            if (!skip(c, depth - 1)) return false;
            if (ch(c, ',')) continue;
            return ch(c, close);
        }
    }
    const char* start = c.p;
    while (c.p < c.e && (isalnum(static_cast<unsigned char>(*c.p)) || *c.p == '-' || *c.p == '+' || *c.p == '.')) ++c.p;
    return c.p > start;
}
// A value read as Lua reads a JSON value in `v and true or false`: false -> 0, null -> -1 (as missing), anything else -> 1
inline int truthy(Cur& c) {
    if (word(c, "false")) return 0;
    if (word(c, "null")) return -1;
    return skip(c) ? 1 : -2;
}
}  // namespace json

// false: not such a file (the settings stay as given). `enabled` is only read when the file has it.
inline bool parse_state(const std::string& text, State& out, bool* had_enabled = nullptr) {
    State s = out;
    json::Cur c{ text.data(), text.data() + text.size() };
    if (text.size() >= 3 && !memcmp(text.data(), "\xEF\xBB\xBF", 3)) c.p += 3;
    if (!json::ch(c, '{')) return false;
    bool seen_enabled = false;
    if (!json::ch(c, '}')) {
        for (;;) {
            std::string key;
            if (!json::str(c, key) || !json::ch(c, ':')) return false;
            if (key == "enabled") {
                const int v = json::truthy(c);
                if (v == -2) return false;
                if (v >= 0) { s.enabled = v == 1; seen_enabled = true; }
            } else if (key == "hidden" && json::ch(c, '{')) {
                if (!json::ch(c, '}')) {
                    for (;;) {
                        std::string cat;
                        if (!json::str(c, cat) || !json::ch(c, ':')) return false;
                        const int v = json::truthy(c);
                        if (v == -2) return false;
                        const int i = cat_index(cat.c_str());
                        if (i >= 0 && v >= 0) s.hidden[i] = v == 1;
                        if (json::ch(c, ',')) continue;
                        if (!json::ch(c, '}')) return false;
                        break;
                    }
                }
            } else if (!json::skip(c)) return false;
            if (json::ch(c, ',')) continue;
            if (!json::ch(c, '}')) return false;
            break;
        }
    }
    out = s;
    if (had_enabled) *had_enabled = seen_enabled;
    return true;
}
inline std::string state_json(const State& s) {
    std::string o = "{\n  \"enabled\": ";
    o += s.enabled ? "true" : "false";
    o += ",\n  \"hidden\": {\n";
    for (int i = 0; i < kCatCount; ++i) {
        o += "    \"";
        o += kCatNames[i];
        o += s.hidden[i] ? "\": true" : "\": false";
        o += i + 1 < kCatCount ? ",\n" : "\n";
    }
    o += "  }\n}\n";
    return o;
}
// The first start: no file of ours. The Lua's file, when there is one, gives its categories, and Zen Mode starts on (that player
// had it on: the script was installed); otherwise off, everything hidden once turned on.
inline State first_state(bool has_legacy, const std::string& legacy_text) {
    State s;
    if (has_legacy && parse_state(legacy_text, s)) s.enabled = true;
    return s;
}

// ---- when an owner's rules are applied ----
// A whole element (whole_owner): when it appears and when the settings change, nothing else.
// During the fight itself (the game's fight state NOW, not paused) nothing is looked at, whatever the owner: the looks are
// made when the fight's phase changes (a round's start and end, a pause), and when something appears (it settles: a few
// looks in its first 90 frames).
// Around the fight (the intro, READY, a round's end, a pause) the fight's owners (the HUD, the rank gauges) are looked at
// every frame: the game brings the HUD in during READY and sets its nodes then, and what it shows at that frame has to be
// hidden at that frame, since nothing is looked at after (seen on 11/11: the rank icon set a second or more into READY).
// Anywhere else (menus: lists filled late, rows made while scrolling): every frame for a while after it appears or after anything
// had to be hidden, then every few frames.
inline constexpr uint32_t kRetryAt[] = { 0, 2, 10, 30, 90 };
inline constexpr uint32_t kRetries = sizeof(kRetryAt) / sizeof(kRetryAt[0]);
inline constexpr uint32_t kMenuEvery = 4, kMenuDense = 60;
inline constexpr uint32_t kGone = 2;         // frames not drawn after which an owner counts as gone
inline bool battle_owner(const char* name) { return !strncmp(name, "BattleHud_", 10) || !strncmp(name, "RankGauge_", 10); }

inline constexpr uint32_t kNever = 0xFFFFFFFFu;
// nBattle.FIGHT_ST, read in the game on 11/11: STAGE_INIT 0, ROUND_INIT 1, APPEAR 2, READY 3, NOW 4, WIN_WAIT 5, WIN 6, FINISH 7
inline constexpr uint8_t kFightNow = 4;
inline constexpr uint16_t kNoFight = 0;      // no fight's HUD drawn
inline uint16_t phase_of(uint8_t fight_st, bool paused) { return static_cast<uint16_t>(0x100u | (paused ? 0x80u : 0u) | (fight_st & 0x7Fu)); }
inline bool quiet_phase(uint16_t phase) { return phase == phase_of(kFightNow, false); }

struct Sched {
    uint32_t appear = 0, due = 0, dense_until = 0;
    uint8_t step = 0;
    bool dormant = true;                     // not drawn: nothing until it is seen again
};
inline void sched_appear(Sched& s, uint32_t frame) {
    s.appear = frame; s.due = frame; s.dense_until = frame + kMenuDense; s.step = 0; s.dormant = false;
}
// After the rules were applied at `frame`; wrote: something had to be hidden or shown
// quiet: the fight itself
inline void sched_applied(Sched& s, uint32_t frame, bool battle, bool wrote, bool whole = false, bool quiet = false) {
    if (whole) { s.due = kNever; return; }                                    // only the settings change it (sched_now)
    if (quiet) {
        if (wrote && s.step >= kRetries) { s.appear = frame; s.step = 0; }       // the game rebuilt it: settle again
        while (s.step < kRetries && s.appear + kRetryAt[s.step] <= frame) ++s.step;
        s.due = s.step < kRetries ? s.appear + kRetryAt[s.step] : kNever;
        return;
    }
    if (battle) { s.due = frame + 1; return; }                                // around the fight: every frame
    if (wrote) s.dense_until = frame + kMenuDense;
    s.due = frame < s.dense_until ? frame + 1 : frame + kMenuEvery;
}
inline void sched_gone(Sched& s) { s.dormant = true; }
// The owner's whole tree walked at this look, or only what the last walk found put back. Some screens write their nodes back
// at every frame (the custom room's rank icons): what we wrote is undone each time, so the owner is looked at every frame;
// its tree is then walked as often as any menu's (kMenuEvery), not more.
inline bool walk_due(const Sched& s, uint32_t frame, uint32_t walked) { return frame < s.appear + kMenuDense || frame - walked >= kMenuEvery; }
inline void sched_now(Sched& s, uint32_t frame) { if (!s.dormant) s.due = frame; }
// The fight's phase changed. Into the fight itself: one look (more only if it had something to hide again); any other
// phase: as when the owner appeared.
inline void sched_phase(Sched& s, uint32_t frame, bool quiet) {
    if (s.dormant) return;
    s.due = frame;
    if (quiet) s.step = static_cast<uint8_t>(kRetries);
    else { s.appear = frame; s.step = 0; s.dense_until = frame + kMenuDense; }
}

// ---- the words, by the game's language ----
struct Words {
    const char* lang;
    const char* tab;                 // the tab and the first row
    const char* on; const char* off;
    const char* hidden; const char* shown; const char* mixed;
    const char* group[kGroupCount];
    const char* help_master;
    const char* help_group[kGroupCount];
};
inline constexpr Words kWords[] = {
    { "en", "Zen Mode", "On", "Off", "Hidden", "Shown", "Mixed",
      { "Ranks", "League Points / MR", "Win Streaks", "Player Names", "Player Details" },
      "Hides ranks, points and player details, to play with a clear mind.",
      { "Rank icons, rank gauges and rank-up notices.",
        "League Points and Master Rate.",
        "Win streak counters.",
        "Player names, titles and clubs, during matches.",
        "Country flags, platform icons and control types, during matches." } },
    { "fr", "Mode Zen", "Activé", "Désactivé", "Masqué", "Affiché", "Mixte",
      { "Rangs", "Points de ligue / PM", "Séries de victoires", "Noms des joueurs", "Détails des joueurs" },
      "Masque les rangs, les points et les détails des joueurs, pour jouer l'esprit libre.",
      { "Icônes de rang, jauges de rang et annonces de promotion.",
        "Points de ligue et Master Rate.",
        "Compteurs de séries de victoires.",
        "Noms des joueurs, titres et clubs, pendant les matchs.",
        "Drapeaux, icônes de plateforme et types de commandes, pendant les matchs." } },
    // The twelve below: machine translations, to be read by native speakers
    { "ja", "禅モード", "オン", "オフ", "非表示", "表示", "混在",
      { "ランク", "リーグポイント / MR", "連勝数", "プレイヤー名", "プレイヤー情報" },
      "ランク、ポイント、プレイヤー情報を非表示にして、落ち着いてプレイできます。",
      { "ランクアイコン、ランクゲージ、昇格の演出。",
        "リーグポイントとマスターレート。",
        "連勝数の表示。",
        "対戦中のプレイヤー名、称号、クラブ。",
        "対戦中の国旗、プラットフォームアイコン、操作タイプ。" } },
    { "zh-Hans", "禅模式", "开", "关", "隐藏", "显示", "混合",
      { "段位", "联赛积分 / MR", "连胜", "玩家名称", "玩家详情" },
      "隐藏段位、积分和玩家详情，让你心无旁骛地对战。",
      { "段位图标、段位槽和升段提示。",
        "联赛积分和大师积分。",
        "连胜计数。",
        "对战中的玩家名称、称号和俱乐部。",
        "对战中的国旗、平台图标和操作类型。" } },
    { "it", "Modalità Zen", "Attiva", "Disattiva", "Nascosto", "Visibile", "Misto",
      { "Gradi", "Punti Lega / MR", "Serie di vittorie", "Nomi dei giocatori", "Dettagli dei giocatori" },
      "Nasconde gradi, punti e dettagli dei giocatori, per giocare a mente libera.",
      { "Icone del grado, barre del grado e avvisi di promozione.",
        "Punti Lega e Master Rate.",
        "Contatori delle serie di vittorie.",
        "Nomi dei giocatori, titoli e club, durante gli incontri.",
        "Bandiere, icone della piattaforma e tipi di comandi, durante gli incontri." } },
    { "de", "Zen-Modus", "An", "Aus", "Ausgeblendet", "Angezeigt", "Gemischt",
      { "Ränge", "Ligapunkte / MR", "Siegesserien", "Spielernamen", "Spielerdetails" },
      "Blendet Ränge, Punkte und Spielerdetails aus, um mit freiem Kopf zu spielen.",
      { "Rangsymbole, Ranganzeigen und Aufstiegshinweise.",
        "Ligapunkte und Master Rate.",
        "Zähler für Siegesserien.",
        "Spielernamen, Titel und Clubs, während der Kämpfe.",
        "Flaggen, Plattformsymbole und Steuerungstypen, während der Kämpfe." } },
    { "es", "Modo Zen", "Activado", "Desactivado", "Oculto", "Visible", "Mixto",
      { "Rangos", "Puntos de liga / MR", "Rachas de victorias", "Nombres de jugadores", "Detalles de jugadores" },
      "Oculta los rangos, los puntos y los detalles de los jugadores, para jugar con la mente despejada.",
      { "Iconos de rango, indicadores de rango y avisos de ascenso.",
        "Puntos de liga y Master Rate.",
        "Contadores de rachas de victorias.",
        "Nombres de jugadores, títulos y clubes, durante los combates.",
        "Banderas, iconos de plataforma y tipos de control, durante los combates." } },
    { "es-419", "Modo Zen", "Activado", "Desactivado", "Oculto", "Visible", "Mixto",
      { "Rangos", "Puntos de liga / MR", "Rachas de victorias", "Nombres de jugadores", "Detalles de jugadores" },
      "Oculta los rangos, los puntos y los detalles de los jugadores, para jugar con la mente despejada.",
      { "Íconos de rango, indicadores de rango y avisos de ascenso.",
        "Puntos de liga y Master Rate.",
        "Contadores de rachas de victorias.",
        "Nombres de jugadores, títulos y clubes, durante los combates.",
        "Banderas, íconos de plataforma y tipos de control, durante los combates." } },
    { "ru", "Режим Дзен", "Вкл.", "Выкл.", "Скрыто", "Показано", "Смешанно",
      { "Ранги", "Очки лиги / MR", "Серии побед", "Имена игроков", "Данные игроков" },
      "Скрывает ранги, очки и данные игроков, чтобы играть со спокойной головой.",
      { "Значки ранга, шкалы ранга и уведомления о повышении.",
        "Очки лиги и Master Rate.",
        "Счётчики серий побед.",
        "Имена игроков, титулы и клубы во время матчей.",
        "Флаги, значки платформ и типы управления во время матчей." } },
    { "pl", "Tryb Zen", "Wł.", "Wył.", "Ukryte", "Widoczne", "Mieszane",
      { "Rangi", "Punkty ligowe / MR", "Serie zwycięstw", "Nazwy graczy", "Szczegóły graczy" },
      "Ukrywa rangi, punkty i szczegóły graczy, aby grać ze spokojną głową.",
      { "Ikony rang, wskaźniki rang i powiadomienia o awansie.",
        "Punkty ligowe i Master Rate.",
        "Liczniki serii zwycięstw.",
        "Nazwy graczy, tytuły i kluby podczas meczów.",
        "Flagi, ikony platform i typy sterowania podczas meczów." } },
    { "pt", "Modo Zen", "Ativado", "Desativado", "Oculto", "Visível", "Misto",
      { "Ranques", "Pontos de liga / MR", "Sequências de vitórias", "Nomes dos jogadores", "Detalhes dos jogadores" },
      "Oculta ranques, pontos e detalhes dos jogadores, para jogar com a mente tranquila.",
      { "Ícones de ranque, medidores de ranque e avisos de promoção.",
        "Pontos de liga e Master Rate.",
        "Contadores de sequências de vitórias.",
        "Nomes dos jogadores, títulos e clubes, durante as partidas.",
        "Bandeiras, ícones de plataforma e tipos de controle, durante as partidas." } },
    { "ko", "젠 모드", "켜기", "끄기", "숨김", "표시", "혼합",
      { "랭크", "리그 포인트 / MR", "연승", "플레이어 이름", "플레이어 정보" },
      "랭크, 포인트, 플레이어 정보를 숨겨 편안한 마음으로 플레이할 수 있습니다.",
      { "랭크 아이콘, 랭크 게이지, 승급 알림.",
        "리그 포인트와 마스터 레이트.",
        "연승 카운터.",
        "대전 중 플레이어 이름, 칭호, 클럽.",
        "대전 중 국기, 플랫폼 아이콘, 조작 타입." } },
    { "zh-Hant", "禪模式", "開", "關", "隱藏", "顯示", "混合",
      { "段位", "聯賽積分 / MR", "連勝", "玩家名稱", "玩家詳情" },
      "隱藏段位、積分和玩家詳情，讓你心無旁騖地對戰。",
      { "段位圖示、段位量表和升段提示。",
        "聯賽積分和大師積分。",
        "連勝計數。",
        "對戰中的玩家名稱、稱號和俱樂部。",
        "對戰中的國旗、平台圖示和操作類型。" } },
    { "ar", "وضع زن", "تشغيل", "إيقاف", "مخفي", "ظاهر", "مختلط",
      { "الرتب", "نقاط الدوري / MR", "سلاسل الانتصارات", "أسماء اللاعبين", "تفاصيل اللاعبين" },
      "يخفي الرتب والنقاط وتفاصيل اللاعبين للعب بذهن صافٍ.",
      { "أيقونات الرتبة ومقاييس الرتبة وإشعارات الترقية.",
        "نقاط الدوري و Master Rate.",
        "عدادات سلاسل الانتصارات.",
        "أسماء اللاعبين والألقاب والأندية أثناء المباريات.",
        "الأعلام وأيقونات المنصات وأنواع التحكم أثناء المباريات." } },
};
inline const Words& words_of(const std::string& lang) {
    for (const Words& w : kWords) if (lang == w.lang) return w;
    return kWords[0];
}

}  // namespace zen
