// model::labels — the texts the screens build from data: Community rows and title, slot names and frames texts, the
// reversal picker's row text, the pack spin's names. Sources: RMT:193-218 (community_rebuild), 290-295 (com_title),
// 62-63 (short_list), 826-838 (slot name format); RSL:92-103, 388-402 (slot_label, row text).
// Pure functions of strings and numbers; the ticker texts that come from locale.json are in model::locale.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace model::labels {

using json = nlohmann::json;

// community_rebuild: the label of one Community row from a Worker item (list answer). Mirrors the Lua format
//   "%s%s%s  by %s  %s  |  %d slots  %s  %dDL"  = dl_mark, name, " v<version>", author, date, slot_count, score, downloads
// with the dl_mark "<color FFAA00>UPD</color> " (server version above the downloaded one) or "<color 00FF00>DL</color> "
// (downloaded), the score "%+d" ("<color 00CCFF>%+d</color>" when the player voted), the date = the first 10 characters of
// updated_at (created_at when updated_at is empty or absent). `downloaded_version` = dl_version() of the item, null when
// never downloaded. Returns false (label untouched) where the Lua raises: a field that is not a number where %d needs one.
bool community_label(const json& item, const json& downloaded_version, std::string* label);

// com_title: "<sort>  <char> Loadouts\n<ICON KeyA>   Page <p>/<n>   <ICON KeyE>"
// `pad`: the pad's glyphs (<ICON BtnL1> / <ICON BtnR1>) instead of the keyboard's, since 2.2.2
std::string community_title(const std::string& sort_label, const std::string& char_name, int64_t page, int64_t total_pages, bool pad = false);
// the dialog's guide line: "<ICON KeyT> Like   <ICON KeyR> Sort", or the pad's Y / X (<ICON BtnU> / <ICON BtnL>)
std::string community_guide(bool pad);
// math.max(1, math.ceil(total / rows))
int64_t community_pages(int64_t total, int64_t rows);

// "Slot 12"-style texts. prefix / suffix are the game's own wording learnt on its rows ("Slot ", "").
// learn_slot_words: msg:match("^(%D-)(%d+)(%s*)$") -> prefix (the suffix is reset to ""). False when msg does not match.
bool learn_slot_words(const std::string& msg, std::string* prefix);
// slot_label: "<prefix><n><suffix>" and, with a name, "<prefix><n><suffix>  <name>"
std::string slot_label(const std::string& prefix, int n, const std::string& suffix, const std::string& name);
// The centre text of a recording row of the reversal picker: "<name or Slot n> (<frames text>)"
std::string reversal_row_text(const std::string& prefix, int n, const std::string& suffix, const std::string& name, const std::string& frames_text);
// frames text of a live slot: "<frames>F", or the empty word for an invalid / empty slot
std::string live_frames_text(bool valid, double frames, const std::string& empty_word);

// short_list: the pack spin's names, cut to 64 bytes (63 + "…" when longer), on a character boundary.
std::vector<std::string> short_list(const std::vector<std::string>& names);

}  // namespace model::labels
