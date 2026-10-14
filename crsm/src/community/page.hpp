// community::Page - the state of the Community page and what its actions do, the "community block" of
// RecordingManagerTab.lua (lines 148-430) without the screen: list, sort, pages, rows, votes, versions downloaded,
// com_state.json, the pack file written by a download, the tickers. The display (NativeOptions dialog today, the core's
// option service in P4) calls these methods and shows `title()` / `rows()`; tickers go out through Env::ticker.
//
// Same texts as the Lua ("Community: 12 loadouts", "Community: timeout", "Busy, try again", "Download failed: ...",
// "Vote failed: ...", "Share failed: ...", ...). All file IO happens in the caller's thread (the core's frame callback):
// never call these from a game hook.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "client.hpp"
#include "identity.hpp"
#include "model/store.hpp"

namespace community {

// What the user is told about an answer of the Worker. `key` = the locale key ("rsm.com_..." without the prefix, "" = no
// translation: the text is the server's own); `arg` fills its %s; `en` = the full English text (also the fallback).
struct Msg {
    std::string key, arg, en;
};

// The decisions on the Worker's answers, pure (tested without a server). `ok` = the request completed; the Worker's own
// refusals (409, 400, 413, 429, 401...) arrive as ok + { error = ... } and are NOT a success.
//   share_verdict    : key "" and en "" = shared (answer { ok = true }); otherwise the message to show, nothing is marked
//   download_verdict : same for a download ({ file = ... } expected)
//   list_verdict     : same for a list ({ items = ... } expected)
Msg share_verdict(bool ok, const json& resp, const std::string& display);
Msg download_verdict(bool ok, const json& data);
Msg list_verdict(bool ok, const json& data);
// The refusals of a write that are about the Steam-verified identity (ADR-0018), by code (auth::error_code):
// verification_unavailable (no token could be had: never sent), update_required (426), invalid_token (401), banned
// (403), not_owner (403), rate_limited (429). key "" and en "" = none of them.
Msg auth_verdict(const json& data);
// The author of a list item: author_cfn (Worker v4), else uploader_name (2.0.0 answers), "?" when neither; and whether the
// Worker vouches for it (author_verified: a Steam-verified account)
std::string item_author(const json& item);
bool item_verified(const json& item);
// A vote answer { ok, action = voted | removed | flipped, new_value } for a vote of `sent` (always 1: only likes exist).
// delta = what the local score changes by (voted +new_value, flipped +2*new_value: from the opposite vote to this one,
// removed -sent: the same vote toggled off); my_vote = the local state after it (1 liked, 0 none, -1 disliked).
// valid = false: the answer lacks what is needed (no ok, unknown action): `err` is the message.
struct VoteEffect {
    bool valid = false;
    int delta = 0, my_vote = 0;
    Msg err;
};
VoteEffect vote_effect(bool ok, const json& data, int sent);

class Page {
public:
    struct Env {
        std::string root;                                   // prepended to the data-relative paths ("reframework/data/" in the game)
        std::function<std::string()> current_char;          // rsm().current_char_name() or "Ryu"
        std::function<void(const std::string&)> ticker;     // the game's ticker strip
        std::function<void()> packs_changed;                // after a pack is written: invalidate_files + ensure_files + manager_dirty
        std::function<Identity()> identity;                 // CFN / player_id / anon_
        std::function<void(const std::string&)> log;        // rec_log
        std::function<std::string(const std::string& key, const std::vector<std::string>& args)> tr;   // locale "rsm.<key>"; returns the key when missing (unset = English)
    };
    struct Row {
        std::string id;
        std::string label;
        json item;                                          // the Worker's list item
    };

    static constexpr int kRows = 7;                         // _com.ROWS
    static constexpr int kPerPage = 50;                     // the list is fetched once (per_page 50), paged on the client

    Page(Client& client, Env env);

    // com_state.json -> last_id, downloaded (the file is read once, at construction in the Lua)
    void load_state();
    void save_state();

    // the "Download Loadouts" button: char, sync with the disk, sort New, page 1, fetch
    void open();
    void sync_with_disk(const std::string& char_name);

    // fetch of the whole list (50 items) for the current char and sort
    void fetch(int page_after);
    // client-side navigation and sort (A / E / R keys); each returns true when something changed
    bool prev_page();
    bool next_page();
    void cycle_sort();                                      // sort New -> Popular -> Most Downloaded, page 1, refetch

    // download of the focused row (1-based): false + ticker when refused; `*message` = the confirm box text
    bool download_prompt(int row, std::string* message);
    void download_confirmed();                              // after the user said yes (the row kept at the prompt)

    // like / unlike (T key) on a row; the row must be downloaded
    void vote(int row);

    // Share loadout: pack_file = the selected pack's file name, display = its shown name
    void share(const std::string& char_name, const std::string& pack_file, const std::string& display);

    // ---- what the screen reads
    std::string title() const;
    const std::vector<Row>& rows() const { return rows_; }
    bool loading() const { return loading_; }
    int page_no() const { return page_; }
    int64_t total() const { return total_; }
    int sort_index() const { return sort_idx_; }            // 1-3
    const std::string& sort_key() const { return sort_; }   // "recent" | "popular" | "downloads"
    const std::string& char_name() const { return char_; }
    const json& all_items() const { return all_items_; }
    const model::store::ComState& state() const { return state_; }
    // data_ver / built_ver: the dialog rebuilds itself when data arrived while it was open
    bool data_changed() const { return data_ver_ != built_ver_ && !loading_; }
    void mark_built() { built_ver_ = data_ver_; }
    // community_rebuild's counter: changes at every rebuild (a list answer, a page, a vote answer); the dialog of the DLL
    // (P4) compares it with what it shows to rebuild after a vote too (the Lua's vote callback navigated itself)
    int64_t version() const { return data_ver_; }

    void rebuild();                                         // community_rebuild
    // Env::tr of "rsm.<key>" when the locale has it, else `en`
    std::string tr_or(const std::string& key, const std::string& en) const;
    // After the author of a verified item (author_verified): the locale's rsm.com_verified_mark ("[V]") in green. A tag, not
    // a check mark: the glyph tables of the game's UI fonts (obfuscated .oft) could not be read, ASCII always draws.
    std::string verified_mark() const;

private:
    std::string community_char() const;
    std::string data_path(const std::string& rel) const { return env_.root + rel; }
    void say(const std::string& s) const { if (env_.ticker) env_.ticker(s); }
    void say(const Msg& m) const;                           // translated through Env::tr when the key exists, else m.en
    void note(const std::string& s) const { if (env_.log) env_.log(s); }
    Identity who() const { return env_.identity ? env_.identity() : Identity{"anon_000000", ""}; }

    Client& client_;
    Env env_;
    model::store::ComState state_;
    json all_items_ = json::array();
    std::vector<Row> rows_;
    int page_ = 1;
    int sort_idx_ = 1;
    std::string sort_ = "recent";
    int64_t total_ = 0;
    bool loading_ = false;
    std::string char_;                                      // "" = nil
    int64_t data_ver_ = 0, built_ver_ = 0;
    Row dl_row_;                                            // the row the confirm box is about (the Lua closure's `r`)
};

}  // namespace community
