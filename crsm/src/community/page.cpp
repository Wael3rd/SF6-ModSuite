#include "page.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "auth.hpp"
#include "model/labels.hpp"
#include "model/luajson.hpp"
#include "model/lutil.hpp"
#include "model/pack.hpp"

namespace community {
namespace {

namespace lj = model::luajson;
namespace lu = model::lutil;
namespace st = model::store;

const char* const kSortNames[3] = { "New", "Popular", "Most Downloaded" };           // English; the keys below
const char* const kSortWordKeys[3] = { "com_sort_new", "com_sort_popular", "com_sort_downloads" };
const char* const kSortKeys[3] = { "recent", "popular", "downloads" };

// "x or default" for a string member of a list item
std::string str_member(const json& item, const char* k, const char* dflt) {
    const json v = lj::index(item, k);
    return lu::truthy(v) ? lua_tostring(v) : std::string(dflt);
}

// a <= b for two Lua numbers held as json (version numbers; a legacy `true` is 1)
bool num_le(const json& a, const json& b) {
    const auto x = lu::to_number(a), y = lu::to_number(b);
    if (!x || !y) return false;
    return x->get<double>() <= y->get<double>();
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// a description cut to the Worker's limit: 1000 characters as JavaScript counts them (UTF-16 units), on a character boundary
std::string share_description(const std::string& s) {
    size_t i = 0, units = 0;
    while (i < s.size()) {
        const unsigned char c = (unsigned char)s[i];
        const size_t n = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        const size_t u = n == 4 ? 2 : 1;
        if (units + u > 1000) break;
        units += u;
        i += std::min(n, s.size() - i);
    }
    return s.substr(0, i);
}

// the server's `error` as text, "" when absent / not truthy
std::string error_text(const json& data) {
    if (!data.is_object()) return "";
    const json e = lj::index(data, "error");
    return lu::truthy(e) ? lua_tostring(e) : std::string();
}

}  // namespace

Msg auth_verdict(const json& data) {
    const std::string code = auth::error_code(data);
    if (code == auth::kUnavailable) return {"com_auth_unavailable", "", "Steam verification unavailable: sharing and likes are off for now"};
    if (code == "update_required") return {"com_auth_update", "", "Update SF6 Recording Manager to share, like or delete"};
    if (code == "invalid_token") return {"com_auth_invalid", "", "Steam verification failed: try again"};
    if (code == "banned" || code == "steam_banned") return {"com_auth_banned", "", "This Steam account is banned from SF6 Community"};
    if (code == "not_owner") return {"com_not_owner", "", "Only its author can delete this drill"};
    if (code == "rate_limited") return {"com_rate_limited", "", "Too many requests: try again later"};
    return {};
}

Msg busy_verdict(bool verifying) {
    if (verifying) return {"com_busy_verifying", "", "Steam verification in progress, try again in a moment"};
    return {"com_busy", "", "Community: please wait, a request is in progress"};
}

Msg verifying_notice() { return {"com_verifying", "", "Verifying with Steam..."}; }

std::string item_author(const json& item) {
    json a = lj::index(item, "author_cfn");
    if (!lu::truthy(a)) a = lj::index(item, "uploader_name");
    return lu::truthy(a) ? lua_tostring(a) : std::string("?");
}

bool item_verified(const json& item) {
    const json v = lj::index(item, "author_verified");
    return v.is_boolean() && v.get<bool>();
}

Msg share_verdict(bool ok, const json& resp, const std::string&) {
    const std::string err = error_text(resp);
    if (Msg a = auth_verdict(resp); !a.en.empty()) return a;
    if (!ok) return err.empty() ? Msg{"com_share_failed_unknown", "", "Share failed: unknown"} : Msg{"com_share_failed", err, "Share failed: " + err};
    // ok = the request completed. Shared only when the Worker says so ({ ok = true }): its refusals are { error = ... }
    if (err.empty()) {
        const json okv = resp.is_object() ? lj::index(resp, "ok") : json();
        if (lu::truthy(okv)) return {"", "", ""};
        return {"com_share_bad", "", "Share failed: unexpected server answer"};
    }
    const std::string code = auth::error_code(resp);
    if (code == "no_changes" || starts_with(err, "No changes")) return {"com_share_dup", "", "Already shared: nothing changed"};   // 409 identical pack
    if (code == "too_large") return {"com_share_big", "", "Drill too large to share"};                                // 413 (v4)
    if (starts_with(err, "File too large")) return {"com_share_big", "", "Drill too large to share"};            // 413
    if (starts_with(err, "Upload limit")) return {"com_share_limit", "", "Share limit reached, try again later"};   // 429 (20 per day)
    return {"com_share_refused", err, "Share refused: " + err};                                                      // 400 invalid, 401, 403, 500...
}

Msg download_verdict(bool ok, const json& data) {
    const json file = (ok && data.is_object()) ? lj::index(data, "file") : json();
    if (lu::truthy(file)) return {"", "", ""};
    const std::string err = error_text(data);
    if (!err.empty()) return {"com_dl_failed", err, "Download failed: " + err};
    if (!ok) return {"com_dl_failed_unknown", "", "Download failed: unknown"};
    return {"com_dl_nofile", "", "Download failed: the server sent no file"};      // was "table: 0x..." in the Lua
}

Msg list_verdict(bool ok, const json& data) {
    const json items = (ok && data.is_object()) ? lj::index(data, "items") : json();
    if (lu::truthy(items)) return {"", "", ""};
    const std::string err = error_text(data);
    if (!err.empty()) return {"com_list_failed", err, "Community: " + err};
    if (!ok) return {"com_list_failed_unknown", "", "Community: failed"};
    return {"com_list_bad", "", "Community: the server sent no list"};
}

json with_info(json pack, const std::string& description, const std::string& author) {
    if (!pack.is_array()) return pack;
    for (json& e : pack) {
        if (!e.is_object()) continue;
        const auto id = e.find("id");
        if (id == e.end() || !id->is_number() || id->get<double>() != 1.0) continue;
        json info = e.contains("info") && e["info"].is_object() ? e["info"] : json::object();
        if (!description.empty() || !info.contains("description") || !info["description"].is_string()) info["description"] = description;
        if (!author.empty() && author != "?") info["author"] = author;
        info["source"] = "community";
        e["info"] = std::move(info);
        break;                                                      // entry 1 is found by its id, the first one counts
    }
    return pack;
}

VoteEffect vote_effect(bool ok, const json& data, int sent) {
    VoteEffect v;
    const std::string err = error_text(data);
    const Msg bad{"com_vote_bad", "", "Vote failed: unexpected server answer"};
    if (Msg a = auth_verdict(data); !a.en.empty()) { v.err = a; return v; }
    if (!ok || !data.is_object()) {
        v.err = err.empty() ? Msg{"com_vote_failed_unknown", "", "Vote failed: unknown"} : Msg{"com_vote_failed", err, "Vote failed: " + err};
        return v;
    }
    if (!lu::truthy(lj::index(data, "ok"))) { v.err = err.empty() ? bad : Msg{"com_vote_failed", err, "Vote failed: " + err}; return v; }
    const json action = lj::index(data, "action");
    if (!action.is_string()) { v.err = bad; return v; }
    const std::string a = action.get<std::string>();
    const auto nv = lu::to_number(lj::index(data, "new_value"));
    const int new_value = nv ? (int)nv->get<double>() : sent;
    if (a == "voted") { v.delta = new_value; v.my_vote = new_value; }                    // no vote before
    else if (a == "flipped") { v.delta = 2 * new_value; v.my_vote = new_value; }          // from the opposite vote: a swing of 2
    else if (a == "removed") { v.delta = -sent; v.my_vote = 0; }                          // the same vote toggled off
    else { v.err = bad; return v; }
    v.valid = true;
    return v;
}

std::string Page::text(const Msg& m) const {
    if (!m.key.empty() && env_.tr) {
        std::vector<std::string> args;
        if (!m.arg.empty()) args.push_back(m.arg);
        if (!m.arg2.empty()) args.push_back(m.arg2);
        const std::string t = env_.tr(m.key, args);
        if (!t.empty() && t != m.key && t != "rsm." + m.key) return t;
    }
    return m.en;
}

Page::Page(Client& client, Env env) : client_(client), env_(std::move(env)) {
    client_.set_verifying_hook([this] { say(verifying_notice()); });
}

Page::~Page() { client_.set_verifying_hook(nullptr); }

std::string Page::tr_or(const std::string& key, const std::string& en) const {
    if (!env_.tr) return en;
    const std::string t = env_.tr(key, {});
    return (t.empty() || t == key || t == "rsm." + key) ? en : t;
}

model::labels::ComWords Page::words() const {
    model::labels::ComWords w;
    w.by = tr_or("com_row_by", w.by);
    w.slots = tr_or("com_row_slots", w.slots);
    w.downloads = tr_or("com_row_dl", w.downloads);
    w.mark_upd = tr_or("com_mark_upd", w.mark_upd);
    w.mark_dl = tr_or("com_mark_dl", w.mark_dl);
    w.window_title = tr_or("com_window_title", w.window_title);
    w.page = tr_or("com_page", w.page);
    return w;
}

std::string Page::verified_mark() const { return "<color 66DD66>" + tr_or("com_verified_mark", "[V]") + "</color>"; }

std::string Page::community_char() const {
    if (env_.current_char) { std::string c = env_.current_char(); if (!c.empty()) return c; }
    return "Ryu";
}

void Page::load_state() {
    state_ = st::com_state_load(lj::load_file(data_path(st::com_state_path())));
}

void Page::save_state() {
    std::string err;
    if (!lj::dump_file(data_path(st::com_state_path()), st::com_state_save(state_), &err)) note("com_state.json not written: " + err);
}

void Page::sync_with_disk(const std::string& char_name) {
    std::set<std::string> erased;
    const json e = lj::load_file(data_path(st::erased_path(char_name)));
    if (e.is_array()) for (const json& v : e) if (v.is_string()) erased.insert(v.get<std::string>());
    const bool changed = st::sync_downloaded(state_, erased, [&](const std::string& f) {
        std::error_code ec;
        return std::filesystem::exists(data_path(st::pack_path(char_name, f)), ec);
    });
    if (changed) save_state();
}

void Page::rebuild() {
    ++data_ver_;
    rows_.clear();
    const int64_t start = (int64_t)(page_ - 1) * kRows;
    for (int i = 1; i <= kRows; ++i) {
        const int64_t at = start + i - 1;
        if (!all_items_.is_array() || at < 0 || at >= (int64_t)all_items_.size()) continue;
        const json& item = all_items_[(size_t)at];
        if (item.is_null()) continue;
        json dv;
        if (item.is_object()) {
            auto v = st::dl_version(state_, lu::truthy(lj::index(item, "display_name")) ? lua_tostring(lj::index(item, "display_name")) : std::string());
            if (v) dv = *v;
        }
        // the author shown: author_cfn of the Worker v4 (uploader_name for the 2.0.0 answers) and the verified mark
        json shown = item;
        if (item.is_object() && (item.contains("author_cfn") || item_verified(item)))
            shown["uploader_name"] = item_author(item) + (item_verified(item) ? " " + verified_mark() : std::string());
        std::string label;
        if (!model::labels::community_label(shown, dv, &label, words())) {
            // the Lua raises inside community_rebuild: the rows built so far stay (fetch() pcalls it and journals the error)
            note("com_rebuild ERROR: an item of the list does not have the expected fields");
            return;
        }
        if (env_.fit_label) label = env_.fit_label(shown, dv, label);   // (the item's own fields stay in r.item)
        Row r;
        r.id = lu::truthy(lj::index(item, "id")) ? lua_tostring(lj::index(item, "id")) : std::string();
        r.label = std::move(label);
        r.item = item;
        // com_rows[i] is indexed by position: a missing item leaves a gap, the rows after it do not exist either
        rows_.push_back(std::move(r));
    }
    note("com_rebuild: page=" + std::to_string(page_) + " all=" + std::to_string(lj::len(all_items_)) + " rows=" + std::to_string(rows_.size()));
}

std::string Page::title(bool pad) const {
    const int s = (sort_idx_ - 1) % 3;
    const std::string shown_char = char_.empty() ? "?" : env_.display_char ? env_.display_char(char_) : char_;
    return model::labels::community_title(tr_or(kSortWordKeys[s], kSortNames[s]), shown_char, page_,
                                          model::labels::community_pages(total_, kRows), pad, words());
}

void Page::fetch(int page_after) {
    if (loading_) return;
    loading_ = true;
    if (char_.empty()) char_ = community_char();
    const Identity info = who();
    json cmd = json::object();
    cmd["cmd"] = "list";
    cmd["char"] = char_;
    cmd["sort"] = sort_.empty() ? std::string("recent") : sort_;
    cmd["page"] = "1";
    cmd["voter_cfn"] = info.cfn;
    cmd["per_page"] = std::to_string(kPerPage);
    client_.send(std::move(cmd), [this, page_after](bool ok, const json& data) {
        loading_ = false;
        const Msg bad = list_verdict(ok, data);
        if (bad.en.empty()) {
            all_items_ = lj::index(data, "items");
            total_ = (int64_t)lj::len(all_items_);
            page_ = page_after > 0 ? page_after : 1;
            rebuild();
            const std::string n = std::to_string(total_);
            say(Msg{total_ == 1 ? "com_list_one" : "com_list_many", n, "Community: " + n + " drill" + (total_ != 1 ? "s" : "")});
        } else {
            say(bad);
        }
    });
}

void Page::open() {
    char_ = community_char();
    sync_with_disk(char_);
    sort_idx_ = 1;
    sort_ = kSortKeys[0];
    page_ = 1;
    fetch(1);
}

bool Page::prev_page() {
    if (page_ <= 1) return false;
    --page_;
    rebuild();
    built_ver_ = data_ver_;
    return true;
}

bool Page::next_page() {
    if (page_ >= model::labels::community_pages(total_, kRows)) return false;
    ++page_;
    rebuild();
    built_ver_ = data_ver_;
    return true;
}

void Page::cycle_sort() {
    sort_idx_ = (sort_idx_ % 3) + 1;
    sort_ = kSortKeys[sort_idx_ - 1];
    page_ = 1;
    fetch(1);
}

bool Page::download_prompt(int row, std::string* message) {
    if (row < 1 || row > (int)rows_.size() || rows_[(size_t)row - 1].id.empty()) return false;
    const Row& r = rows_[(size_t)row - 1];
    if (client_.busy()) { say(busy_verdict(client_.verifying())); return false; }
    const std::string display = lu::truthy(lj::index(r.item, "display_name")) ? lua_tostring(lj::index(r.item, "display_name")) : std::string();
    const auto local_ver = st::dl_version(state_, display);
    const json sv = lj::index(r.item, "version");
    const json server_ver = lu::truthy(sv) ? sv : json(1);
    if (local_ver && num_le(server_ver, *local_ver)) {
        const std::string v = lua_tostring(*local_ver);
        say(Msg{"com_already", v, "Already downloaded (v" + v + ")"});
        return false;
    }
    if (local_ver) {
        const std::string sv_text = lua_tostring(server_ver), lv_text = lua_tostring(*local_ver);
        *message = text(Msg{"com_update_ask", sv_text, "Update v" + sv_text + " available (you have v" + lv_text + "). Download?", lv_text});
    } else {
        const std::string nm = str_member(r.item, "display_name", "?"), au = item_author(r.item);
        *message = text(Msg{"com_download_ask", nm, "Download \"" + nm + "\" by " + au + "?", au});
    }
    dl_row_ = r;
    return true;
}

void Page::download_confirmed() {
    const Row r = dl_row_;
    state_.last_id = r.id;
    save_state();
    json cmd = json::object();
    cmd["cmd"] = "download";
    cmd["id"] = r.id;
    client_.send(std::move(cmd), [this, r](bool ok, const json& data) {
        const Msg bad = download_verdict(ok, data);
        if (bad.en.empty()) {
            // the loadout's description (the download answer's, else the list item's) and author, on entry 1
            json dsc = lj::index(data, "description");
            if (!dsc.is_string() || dsc.get<std::string>().empty()) dsc = lj::index(r.item, "description");
            const json file = with_info(lj::index(data, "file"), dsc.is_string() ? dsc.get<std::string>() : std::string(), item_author(r.item));
            const std::string ch = char_.empty() ? community_char() : char_;
            const json nm = lj::index(data, "name");
            const std::string fname = st::community_filename(lu::truthy(nm) ? lua_tostring(nm) : std::string("community"));
            const std::string name = fname.substr(0, fname.size() - 5);
            const json rv = lj::index(r.item, "version");
            state_.downloaded[fname] = lu::truthy(rv) ? rv : json(1);
            save_state();
            std::string err;
            if (!lj::dump_file(data_path(st::pack_path(ch, fname)), file, &err)) note("pack not written: " + err);
            {   // the pack is no longer "erased"
                const std::string ep = data_path(st::erased_path(ch));
                const json erased = lj::load_file(ep);
                if (erased.is_array()) {
                    json keep = json::array();
                    for (const json& e : erased) if (!(e.is_string() && e.get<std::string>() == fname)) keep.push_back(e);
                    if (keep.size() != erased.size() && !lj::dump_file(ep, keep, &err)) note("_erased.json not written: " + err);
                }
            }
            if (env_.packs_changed) env_.packs_changed();
            say(Msg{"com_downloaded", name, "Downloaded: " + name});
            fetch(page_);
        } else {
            say(bad);
        }
    });
    say(Msg{"com_downloading", "", "Downloading..."});
}

void Page::vote(int row) {
    if (row < 1 || row > (int)rows_.size() || rows_[(size_t)row - 1].id.empty()) { say(Msg{"com_no_item", "", "No item"}); return; }
    const Row r = rows_[(size_t)row - 1];
    const std::string display = lu::truthy(lj::index(r.item, "display_name")) ? lua_tostring(lj::index(r.item, "display_name")) : std::string();
    if (!st::dl_version(state_, display)) { say(Msg{"com_download_first", "F", "Download it first (F)"}); return; }
    if (client_.busy()) { say(busy_verdict(client_.verifying())); return; }
    const Identity info = who();
    json cmd = json::object();
    cmd["cmd"] = "vote";
    cmd["id"] = r.id;
    cmd["cfn"] = info.cfn;
    cmd["value"] = 1;
    const std::string vote_id = r.id;
    client_.send(std::move(cmd), [this, vote_id](bool ok, const json& data) {
        const VoteEffect ve = vote_effect(ok, data, 1);
        if (ve.valid) {
            // the local score follows the answer (voted +1, flipped +2 from a dislike, removed -1), once
            if (all_items_.is_array()) {
                for (json& it : all_items_) {
                    const json id = it.is_object() ? lj::index(it, "id") : json();
                    if (id.is_string() && id.get<std::string>() == vote_id) {
                        const json sc = lj::index(it, "score");
                        const auto n = lu::truthy(sc) ? lu::to_number(sc) : std::optional<json>(json(0));
                        it["score"] = (n ? (int64_t)n->get<double>() : 0) + ve.delta;
                        if (ve.my_vote == 0) it.erase("my_vote"); else it["my_vote"] = ve.my_vote;
                    }
                }
            }
            rebuild();
            built_ver_ = data_ver_;
            say(ve.my_vote > 0 ? Msg{"com_liked", "", "Liked"} : Msg{"com_unliked", "", "Unliked"});
        } else {
            say(ve.err);
        }
    });
}

void Page::share(const std::string& char_name, const std::string& pack_file, const std::string& display_in) {
    if (pack_file.empty()) { say(Msg{"com_share_none", "", "No drill selected to share"}); return; }
    const std::string display = lu::strip_json_ext(display_in.empty() ? pack_file : display_in);
    const Identity info = who();
    const json data = lj::load_file(data_path(st::pack_path(char_name, pack_file)));
    if (!lu::truthy(data)) { say(Msg{"com_share_nofile", "", "Cannot read the drill file"}); return; }
    if (client_.busy()) { say(busy_verdict(client_.verifying())); return; }
    json cmd = json::object();
    cmd["cmd"] = "share";
    cmd["char"] = char_name;
    cmd["name"] = display;
    cmd["cfn"] = info.cfn;
    cmd["player"] = !info.name.empty() ? info.name : ("Player " + info.cfn);
    cmd["description"] = share_description(model::pack::description(data));   // the pack's own (info), "" when none
    cmd["file"] = lj::encode(data);
    client_.send(std::move(cmd), [this, display](bool ok, const json& resp) {
        const Msg m = share_verdict(ok, resp, display);
        if (m.en.empty()) say(Msg{"com_shared", display, "Drill shared: " + display});
        else say(m);       // a refusal of the Worker (409 / 400 / 413 / 429...) is not "shared"; nothing is marked
    });
    say(Msg{"com_sharing", "", "Sharing drill..."});
}

}  // namespace community
