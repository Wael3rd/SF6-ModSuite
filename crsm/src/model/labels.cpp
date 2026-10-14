#include "labels.hpp"

#include <cmath>

#include "luajson.hpp"
#include "lutil.hpp"

namespace model::labels {
namespace {

namespace lj = model::luajson;

// "%d" / "%+d" of a json value: a number with an integer value, or a string that reads as one
bool fmt_int(const json& v, bool plus, std::string* out) {
    const auto n = lutil::to_number(v);
    if (!n) return false;
    const auto k = lutil::to_int_key(*n);
    if (!k) return false;
    *out = (plus && *k >= 0 ? "+" : "") + std::to_string(*k);
    return true;
}

// x or "": a string where the Lua calls a string method on it; false when it is something else that is not nil / false
bool str_or(const json& v, const char* dflt, std::string* out) {
    if (!lutil::truthy(v)) { *out = dflt; return true; }
    if (!v.is_string()) return false;
    *out = v.get<std::string>();
    return true;
}

}  // namespace

bool community_label(const json& item, const json& downloaded_version, std::string* label) {
    if (!item.is_object()) return false;
    std::string name, author, date;
    if (!str_or(lj::index(item, "display_name"), "?", &name)) return false;
    name = lutil::strip_json_ext(name);
    if (!str_or(lj::index(item, "uploader_name"), "?", &author)) return false;
    if (author.compare(0, 7, "Player ") == 0) author.erase(0, 7);

    std::string ver;
    const json version = lj::index(item, "version");
    if (lutil::truthy(version)) {
        if (version.is_number()) ver = " v" + lutil::number_to_string(version);
        else if (version.is_string()) ver = " v" + version.get<std::string>();
        else return false;                                                   // attempt to concatenate a table / boolean value
    }

    const json upd = lj::index(item, "updated_at");
    json chosen;
    if (lutil::truthy(upd) && upd != json("")) chosen = upd;
    else {
        const json cr = lj::index(item, "created_at");
        chosen = lutil::truthy(cr) ? cr : json("");
    }
    if (!chosen.is_string()) return false;
    date = chosen.get<std::string>().substr(0, 10);

    // dl_mark: downloaded (lv) -> UPD when the server version is above it, DL otherwise
    std::string dl_mark;
    if (lutil::truthy(downloaded_version)) {
        const auto lv = lutil::to_number(downloaded_version);
        json svj = lutil::truthy(version) ? version : json(1);
        const auto sv = lutil::to_number(svj.is_string() ? json() : svj);       // comparing a string with a number raises
        if (!lv || !sv) return false;
        dl_mark = sv->get<double>() > lv->get<double>() ? "<color FFAA00>UPD</color> " : "<color 00FF00>DL</color> ";
    }

    const json score_j = lutil::truthy(lj::index(item, "score")) ? lj::index(item, "score") : json(0);
    std::string score, score_str;
    if (!fmt_int(score_j, true, &score)) return false;
    score_str = lutil::truthy(lj::index(item, "my_vote")) ? "<color 00CCFF>" + score + "</color>" : score;
    std::string slots, downloads;
    const json sc = lj::index(item, "slot_count"), dl = lj::index(item, "downloads");
    if (!fmt_int(lutil::truthy(sc) ? sc : json(0), false, &slots)) return false;
    if (!fmt_int(lutil::truthy(dl) ? dl : json(0), false, &downloads)) return false;

    *label = dl_mark + name + ver + "  by " + author + "  " + date + "  |  " + slots + " slots  " + score_str + "  " + downloads + "DL";
    return true;
}

std::string community_title(const std::string& sort_label, const std::string& char_name, int64_t page, int64_t total_pages) {
    return sort_label + "  " + char_name + " Loadouts\n<ICON KeyA>   Page " + std::to_string(page) + "/" + std::to_string(total_pages) + "   <ICON KeyE>";
}

int64_t community_pages(int64_t total, int64_t rows) {
    if (rows < 1) return 1;
    const int64_t p = (total + rows - 1) / rows;
    return p > 1 ? p : 1;
}

bool learn_slot_words(const std::string& msg, std::string* prefix) {
    size_t d = 0;
    while (d < msg.size() && !lutil::is_digit(msg[d])) ++d;
    if (d == msg.size()) return false;
    size_t e = d;
    while (e < msg.size() && lutil::is_digit(msg[e])) ++e;
    for (size_t i = e; i < msg.size(); ++i) if (!lutil::is_space(msg[i])) return false;
    *prefix = msg.substr(0, d);
    return true;
}

std::string slot_label(const std::string& prefix, int n, const std::string& suffix, const std::string& name) {
    const std::string base = prefix + std::to_string(n) + suffix;
    return name.empty() ? base : base + "  " + name;
}

std::string reversal_row_text(const std::string& prefix, int n, const std::string& suffix, const std::string& name, const std::string& frames_text) {
    return (name.empty() ? prefix + std::to_string(n) + suffix : name) + " (" + frames_text + ")";
}

std::string live_frames_text(bool valid, double frames, const std::string& empty_word) {
    if (!valid || frames <= 0) return empty_word;
    return std::to_string((long long)std::floor(frames)) + "F";
}

std::vector<std::string> short_list(const std::vector<std::string>& names) {
    std::vector<std::string> t;
    for (const std::string& n : names)
        t.push_back(n.size() > 64 ? lutil::utf8_cut(n, 63) + "\xE2\x80\xA6" : n);
    return t;
}

}  // namespace model::labels
