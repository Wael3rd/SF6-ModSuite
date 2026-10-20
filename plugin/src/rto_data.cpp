// SF6 Slots — runtime costume options: the data side (rto_data.hpp). No game access.
#include "rto_data.hpp"

#include <algorithm>
#include <set>

namespace slots::rto {

namespace {

std::string str_of(const Json* j) { return j ? j->as_str() : std::string(); }
int int_of(const Json* j, int def = 0) { return j ? static_cast<int>(j->as_int(def)) : def; }

std::vector<std::string> strings_of(const Json* j) {
    std::vector<std::string> out;
    if (j && j->kind == Json::Array)
        for (auto& e : j->arr) if (e.kind == Json::String) out.push_back(e.s);
    return out;
}

std::vector<File> files_of(const Json* j) {
    std::vector<File> out;
    if (!j || j->kind != Json::Array) return out;
    for (auto& e : j->arr) {
        File f;
        f.kind = str_of(e.get("kind"));
        f.target = norm_path(str_of(e.get("target")));
        f.path = norm_path(str_of(e.get("path")));
        f.unit = norm_path(str_of(e.get("unit")));
        f.joints = int_of(e.get("joints"), -1);
        if (!f.kind.empty() && !f.target.empty() && !f.path.empty()) out.push_back(std::move(f));
    }
    return out;
}

bool live_kind(const std::string& k) { return k == "mesh" || k == "material" || k == "texture" || k == "chain"; }

bool has(const std::vector<std::string>& v, const std::string& id) { return std::find(v.begin(), v.end(), id) != v.end(); }

void erase_id(std::vector<std::string>& v, const std::string& id) { v.erase(std::remove(v.begin(), v.end(), id), v.end()); }

const std::vector<File>* files_for(const Slot& s, const std::string& id) {
    auto it = s.files.find(id);
    return it == s.files.end() ? nullptr : &it->second;
}

// Options of other groups that come later than `id` and ship one of its targets would hide it: off
void drop_later(const Manifest& m, const Slot& s, std::vector<std::string>& chosen, const std::string& id) {
    const Mod* mod = m.mod_of(s);
    if (!mod) return;
    const int x = mod->find(id);
    const auto* fx = files_for(s, id);
    if (x < 0 || !fx) return;
    std::set<std::string> targets;
    for (auto& f : *fx) targets.insert(f.target);
    for (auto it = chosen.begin(); it != chosen.end();) {
        const int y = mod->find(*it);
        bool drop = false;
        if (y >= 0 && y != x && mod->options[static_cast<size_t>(y)].mode != "base"
            && mod->options[static_cast<size_t>(y)].rank > mod->options[static_cast<size_t>(x)].rank)
            if (const auto* fy = files_for(s, *it))
                for (auto& f : *fy) if (targets.count(f.target)) { drop = true; break; }
        it = drop ? chosen.erase(it) : it + 1;
    }
}

} // namespace

std::string norm_path(const std::string& path) {
    std::string s = path;
    for (auto& c : s) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (s.compare(0, 12, "natives/stm/") == 0) s.erase(0, 12);
    return s;
}

std::string clip_name(const std::string& s, size_t max_chars) {
    // UTF-8: a character starts at every byte that is not a continuation byte (10xxxxxx)
    size_t chars = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) ++chars;
    if (chars <= max_chars || max_chars < 4) return s;
    size_t keep = max_chars - 3, n = 0, end = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) continue;
        if (n++ == keep) { end = i; break; }
    }
    while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '-' || s[end - 1] == '+')) --end;
    return s.substr(0, end) + "...";
}

std::string display_name(const std::string& s) {
    // Mods name their options for Fluffy's sort order: "!C2 ...", "1) Main files", "3) addon - ..."
    size_t i = 0;
    auto skip_ws = [&] { while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i; };
    skip_ws();
    while (i < s.size() && (s[i] == '!' || s[i] == '#' || s[i] == '_')) ++i;
    skip_ws();
    size_t d = i;
    while (d < s.size() && s[d] >= '0' && s[d] <= '9') ++d;
    if (d > i && d < s.size() && (s[d] == ')' || s[d] == '.' || s[d] == '-' || s[d] == ']')) { i = d + 1; skip_ws(); }
    auto starts_ci = [&](const std::string& w) {
        if (s.size() < i + w.size()) return false;
        for (size_t k = 0; k < w.size(); ++k) {
            char c = s[i + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (c != w[k]) return false;
        }
        return true;
    };
    for (const std::string w : { "addon", "add-on", "add on" }) {
        const size_t n = w.size();
        if (!starts_ci(w)) continue;
        size_t j = i + n;
        while (j < s.size() && s[j] == ' ') ++j;
        if (j < s.size() && (s[j] == '-' || s[j] == ':')) { i = j + 1; skip_ws(); }
        break;
    }
    std::string out = s.substr(i);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.empty()) return s;
    if (i > 0 && out[0] >= 'a' && out[0] <= 'z') out[0] = static_cast<char>(out[0] - 'a' + 'A');   // a prefix was taken off
    return out;
}

int Mod::find(const std::string& id) const {
    for (size_t i = 0; i < options.size(); ++i) if (options[i].id == id) return static_cast<int>(i);
    return -1;
}

const Mod* Manifest::mod_by_key(const std::string& key) const {
    for (auto& m : mods) if (m.key == key) return &m;
    return nullptr;
}

const Slot* Manifest::slot_of_path(const std::string& path) const {
    for (auto& s : slots) if (!s.prefix.empty() && path.compare(0, s.prefix.size(), s.prefix) == 0) return &s;
    return nullptr;
}

const Slot* Manifest::slot_of_paths(const std::vector<std::string>& paths) const {
    // Several game outfits can share one model folder (A.K.I's Outfit 1 and 4, each its own mdf2): a game outfit
    // whose options replace a mesh or a material is worn only when the model loads one of those files
    auto worn = [&](const Slot& s) {
        bool model_files = false;
        for (auto& [stored, target] : s.rev) {
            const bool mesh = target.size() > 5 && target.compare(target.size() - 5, 5, ".mesh") == 0;
            const bool mdf2 = target.size() > 5 && target.compare(target.size() - 5, 5, ".mdf2") == 0;
            if (!mesh && !mdf2) continue;
            model_files = true;
            for (auto& p : paths) if (p == target || p == stored) return true;   // the game's file, or an option's copy worn
        }
        return !model_files;
    };
    const Slot* game = nullptr;
    for (auto& p : paths) {
        for (auto& s : slots) {
            if (s.prefix.empty() || p.compare(0, s.prefix.size(), s.prefix) != 0) continue;
            if (!s.game) return &s;
            if (!game && worn(s)) game = &s;
        }
    }
    return game;
}

const Slot* Manifest::slot_of(int fighter, int costume_no) const {
    for (auto& s : slots) if (s.fighter == fighter && s.costume_no == costume_no) return &s;
    return nullptr;
}

bool parse_manifest(const std::string& text, Manifest& out, std::string& err) {
    out = Manifest{};
    Json j;
    if (text.empty()) { err = "no file"; return false; }
    if (!json_parse(text, j) || j.kind != Json::Object) { err = "not JSON"; return false; }
    out.format = str_of(j.get("format"));
    if (out.format != kManifestFormat) { err = "format '" + out.format + "' is not " + kManifestFormat; return false; }
    out.generated = str_of(j.get("generated"));
    out.loader = str_of(j.get("loader"));
    out.pak = str_of(j.get("pak"));
    out.layout = str_of(j.get("layout"));
    if (const Json* ms = j.get("mods"); ms && ms->kind == Json::Array) {
        for (auto& jm : ms->arr) {
            Mod m;
            m.key = str_of(jm.get("key"));
            m.name = str_of(jm.get("name"));
            m.source = str_of(jm.get("source"));
            m.defaults_from = str_of(jm.get("defaults_from"));
            int rank = 0;
            if (const Json* gs = jm.get("groups"); gs && gs->kind == Json::Array) {
                for (auto& jg : gs->arr) {
                    Group g;
                    g.name = str_of(jg.get("name"));
                    if (const Json* os = jg.get("options"); os && os->kind == Json::Array) {
                        for (auto& jo : os->arr) {
                            Option o;
                            o.id = str_of(jo.get("id"));
                            if (o.id.empty()) continue;
                            o.name = str_of(jo.get("name"));
                            o.folder = str_of(jo.get("folder"));
                            o.mode = str_of(jo.get("mode"));
                            o.apply = str_of(jo.get("apply"));
                            if (const Json* ns = jo.get("nsfw"); ns && ns->kind == Json::Bool && ns->b) o.nsfw = out.has_nsfw = true;
                            o.group = static_cast<int>(m.groups.size());
                            o.rank = rank++;
                            g.options.push_back(static_cast<int>(m.options.size()));
                            m.options.push_back(std::move(o));
                        }
                    }
                    m.groups.push_back(std::move(g));
                }
            }
            if (const Json* vs = jm.get("variants"); vs && vs->kind == Json::Array)
                for (auto& jv : vs->arr) m.variants.push_back({ str_of(jv.get("label")), strings_of(jv.get("options")) });
            if (!m.key.empty()) out.mods.push_back(std::move(m));
        }
    }
    if (const Json* ss = j.get("slots"); ss && ss->kind == Json::Array) {
        for (auto& js : ss->arr) {
            Slot s;
            s.fighter = int_of(js.get("fighter"));
            s.costume_no = int_of(js.get("costume_no"), -1);
            s.fighter_dir = norm_path(str_of(js.get("fighter_dir")));
            s.folder = norm_path(str_of(js.get("folder")));
            s.name = str_of(js.get("name"));
            s.label = str_of(js.get("label"));
            s.mod = str_of(js.get("mod"));
            s.defaults = strings_of(js.get("defaults"));
            if (const Json* gm = js.get("game"); gm && gm->kind == Json::Bool) s.game = gm->b;
            if (const Json* os = js.get("options"); os && os->kind == Json::Object)
                for (auto& [id, jf] : os->obj) s.files[id] = files_of(&jf);
            s.base = files_of(js.get("base"));
            if (const Json* rq = js.get("required"); rq && rq->kind == Json::Array)
                for (auto& e : rq->arr) if (e.kind == Json::Number) s.required.push_back(static_cast<int>(e.as_int(-1)));
            if (const Json* fs = js.get("former"); fs && fs->kind == Json::Array)
                for (auto& jf : fs->arr)
                    s.former.push_back({ int_of(jf.get("costume_no"), -1), str_of(jf.get("folder")), str_of(jf.get("label")),
                                         strings_of(jf.get("options")) });
            for (size_t i = 0; i < out.mods.size(); ++i) if (out.mods[i].key == s.mod) s.mod_index = static_cast<int>(i);
            if (s.mod_index < 0 || s.fighter <= 0 || s.costume_no < 0 || s.fighter_dir.empty() || s.folder.empty()) continue;
            s.prefix = "product/model/esf/" + s.fighter_dir + "/" + s.folder + "/";
            for (auto& [id, files] : s.files) for (auto& f : files) s.rev[f.path] = f.target;
            for (auto& f : s.base) s.rev[f.path] = f.target;
            out.slots.push_back(std::move(s));
        }
    }
    if (out.slots.empty()) { err = "no slot with options"; return false; }
    return true;
}

// ---- choices ----

bool parse_choices(const std::string& text, Choices& out) {
    out = Choices{};
    Json j;
    if (text.empty() || !json_parse(text, j) || j.kind != Json::Object) return false;
    if (str_of(j.get("format")) != kChoicesFormat) return false;
    out.former_done = str_of(j.get("former_done"));
    if (const Json* ms = j.get("mods"); ms && ms->kind == Json::Object) {
        for (auto& [key, jm] : ms->obj) {
            auto& sides = out.mods[key];
            for (int i = 0; i < 2; ++i) {
                const Json* js = jm.get(i ? "p2" : "p1");
                if (!js) continue;
                const Json* ch = js->get("changed");
                sides[static_cast<size_t>(i)].changed = ch && ch->kind == Json::Bool && ch->b;
                sides[static_cast<size_t>(i)].ids = strings_of(js->get("options"));
                sides[static_cast<size_t>(i)].names = strings_of(js->get("names"));
            }
        }
    }
    return true;
}

void sanitize(Choices& c, const Manifest& m, std::vector<std::string>& notes) {
    for (auto& [key, sides] : c.mods) {
        const Mod* mod = m.mod_by_key(key);
        if (!mod) continue;                                  // a mod not installed now: its choices are kept
        for (int i = 0; i < 2; ++i) {
            Side& sd = sides[static_cast<size_t>(i)];
            std::vector<std::string> ids, names;
            for (size_t k = 0; k < sd.ids.size(); ++k) {
                const std::string& id = sd.ids[k];
                const std::string name = k < sd.names.size() ? sd.names[k] : std::string();
                if (mod->find(id) >= 0) { ids.push_back(id); names.push_back(mod->options[static_cast<size_t>(mod->find(id))].name); continue; }
                int hit = -1, n = 0;
                if (!name.empty())
                    for (size_t o = 0; o < mod->options.size(); ++o) if (mod->options[o].name == name) { hit = static_cast<int>(o); ++n; }
                if (n == 1) {
                    ids.push_back(mod->options[static_cast<size_t>(hit)].id); names.push_back(name);
                    notes.push_back(key + (i ? " P2" : " P1") + ": option '" + name + "' found under a new id");
                } else {
                    notes.push_back(key + (i ? " P2" : " P1") + ": option " + id + " ('" + name + "') is gone, dropped");
                }
            }
            sd.ids = std::move(ids);
            sd.names = std::move(names);
        }
    }
}

std::string write_choices(const Choices& c, const Manifest& m) {
    std::string o = "{\n  \"format\": \"" + std::string(kChoicesFormat) + "\",\n  \"former_done\": \"" + json_escape(c.former_done)
                  + "\",\n  \"mods\": {";
    bool first = true;
    for (auto& [key, sides] : c.mods) {
        const Mod* mod = m.mod_by_key(key);
        o += std::string(first ? "" : ",") + "\n    \"" + json_escape(key) + "\": {";
        first = false;
        for (int i = 0; i < 2; ++i) {
            const Side& sd = sides[static_cast<size_t>(i)];
            o += std::string(i ? ",\n      \"p2\": " : "\n      \"p1\": ");
            if (!sd.changed) { o += "{\"changed\": false}"; continue; }
            std::string ids, names;
            for (size_t k = 0; k < sd.ids.size(); ++k) {
                std::string name = k < sd.names.size() ? sd.names[k] : std::string();
                if (mod) if (int x = mod->find(sd.ids[k]); x >= 0) name = mod->options[static_cast<size_t>(x)].name;
                ids += std::string(k ? ", " : "") + "\"" + json_escape(sd.ids[k]) + "\"";
                names += std::string(k ? ", " : "") + "\"" + json_escape(name) + "\"";
            }
            o += "{\"changed\": true, \"options\": [" + ids + "], \"names\": [" + names + "]}";
        }
        o += "\n    }";
    }
    o += "\n  }\n}\n";
    return o;
}

bool hidden(const Manifest& m, const Slot& s, const std::string& id) {
    if (m.show_nsfw) return false;
    const Mod* mod = m.mod_of(s);
    const int x = mod ? mod->find(id) : -1;
    return x >= 0 && mod->options[static_cast<size_t>(x)].nsfw;
}

std::vector<std::string> chosen_of(const Manifest& m, const Slot& s, const Choices& c, int side) {
    const Mod* mod = m.mod_of(s);
    auto it = c.mods.find(s.mod);
    auto unhide = [&](std::vector<std::string>& v) {         // NSFW options while they are hidden: not worn
        v.erase(std::remove_if(v.begin(), v.end(), [&](const std::string& id) { return hidden(m, s, id); }), v.end());
    };
    if (!mod || it == c.mods.end() || side < 0 || side > 1 || !it->second[static_cast<size_t>(side)].changed) {
        std::vector<std::string> out = s.defaults;
        unhide(out);
        complete(m, s, out);
        return out;
    }
    std::vector<std::string> out;
    for (auto& id : it->second[static_cast<size_t>(side)].ids) if (mod->find(id) >= 0 && !has(out, id)) out.push_back(id);
    for (auto& id : s.defaults) {                            // what is always worn stays
        int x = mod->find(id);
        if (x >= 0 && mod->options[static_cast<size_t>(x)].mode == "base" && !has(out, id)) out.push_back(id);
    }
    unhide(out);
    complete(m, s, out);                                     // a saved "None" of a required group: its default
    return out;
}

std::vector<std::string> keep_hidden(const Manifest& m, const Slot& s, const std::vector<std::string>& saved,
                                     const std::vector<std::string>& shown, const std::vector<std::string>& now) {
    const Mod* mod = m.mod_of(s);
    if (m.show_nsfw || !mod) return now;
    auto pick_in = [&](const std::vector<std::string>& v, int group) -> std::string {
        for (auto& id : v) {
            const int x = mod->find(id);
            if (x >= 0 && mod->options[static_cast<size_t>(x)].mode == "pick" && mod->options[static_cast<size_t>(x)].group == group) return id;
        }
        return {};
    };
    std::vector<std::string> out = now;
    for (auto& id : saved) {
        const int x = mod->find(id);
        if (x < 0 || !hidden(m, s, id) || has(out, id)) continue;
        const Option& o = mod->options[static_cast<size_t>(x)];
        if (o.mode == "pick") {
            const std::string was = pick_in(shown, o.group), is = pick_in(now, o.group);
            if (was != is) continue;                         // the user picked in that group: the hidden pick goes
            if (!is.empty()) erase_id(out, is);              // what stood in for it while hidden
        }
        out.push_back(id);
    }
    return out;
}

bool required(const Slot& s, int group) {
    return std::find(s.required.begin(), s.required.end(), group) != s.required.end();
}

std::string default_pick(const Manifest& m, const Slot& s, int group) {
    const Mod* mod = m.mod_of(s);
    if (!mod || group < 0 || group >= static_cast<int>(mod->groups.size())) return {};
    auto pick = [&](const std::string& id) {
        const int x = mod->find(id);
        return x >= 0 && mod->options[static_cast<size_t>(x)].group == group && mod->options[static_cast<size_t>(x)].mode == "pick"
            && offered(s, id) && !hidden(m, s, id);
    };
    for (auto& id : s.defaults) if (pick(id)) return id;
    for (int oi : mod->groups[static_cast<size_t>(group)].options)
        if (pick(mod->options[static_cast<size_t>(oi)].id)) return mod->options[static_cast<size_t>(oi)].id;
    return {};
}

void complete(const Manifest& m, const Slot& s, std::vector<std::string>& chosen, std::vector<std::string>* notes) {
    const Mod* mod = m.mod_of(s);
    if (!mod) return;
    // one pick per group: the last one listed stays
    for (size_t k = chosen.size(); k-- > 0;) {
        const int x = mod->find(chosen[k]);
        if (x < 0 || mod->options[static_cast<size_t>(x)].mode != "pick") continue;
        const int g = mod->options[static_cast<size_t>(x)].group;
        for (size_t q = k; q-- > 0;) {
            const int y = mod->find(chosen[q]);
            if (y < 0 || y == x || mod->options[static_cast<size_t>(y)].mode != "pick" || mod->options[static_cast<size_t>(y)].group != g)
                continue;
            if (notes) notes->push_back("\"" + mod->options[static_cast<size_t>(y)].name + "\" and \"" + mod->options[static_cast<size_t>(x)].name
                                        + "\" are one choice now: \"" + mod->options[static_cast<size_t>(x)].name + "\" kept");
            chosen.erase(chosen.begin() + static_cast<std::ptrdiff_t>(q));
            --k;
        }
    }
    for (int g : s.required) {
        if (g < 0 || g >= static_cast<int>(mod->groups.size())) continue;
        bool any = false;
        for (int oi : mod->groups[static_cast<size_t>(g)].options) {
            const Option& o = mod->options[static_cast<size_t>(oi)];
            if (o.mode == "pick" && has(chosen, o.id)) any = true;
        }
        if (any) continue;
        const std::string id = default_pick(m, s, g);
        if (!id.empty()) chosen.push_back(id);               // as worn by default: no option turned off for it
    }
}

// ---- what the model loads ----

std::map<std::string, Want> desired(const Manifest& m, const Slot& s, const std::vector<std::string>& chosen,
                                    std::vector<std::string>* notes) {
    std::map<std::string, Want> out;
    const Mod* mod = m.mod_of(s);
    if (!mod) return out;
    // per target: the options of this slot that ship it (rank, id, stored path)
    struct Ship { int rank; std::string id, path, unit; };
    std::map<std::string, std::vector<Ship>> by_target;
    std::map<std::string, std::string> kind_of;
    for (auto& [id, files] : s.files) {
        const int x = mod->find(id);
        if (x < 0) continue;
        for (auto& f : files) {
            if (!live_kind(f.kind)) continue;
            by_target[f.target].push_back({ mod->options[static_cast<size_t>(x)].rank, id, f.path, f.unit });
            kind_of[f.target] = f.kind;
        }
    }
    std::map<std::string, const File*> base;
    for (auto& f : s.base) if (live_kind(f.kind)) { base[f.target] = &f; kind_of.emplace(f.target, f.kind); }
    auto winner = [&](const std::vector<Ship>* ships, const std::vector<std::string>& set) -> const Ship* {
        const Ship* w = nullptr;
        if (ships) for (auto& sh : *ships) if (has(set, sh.id) && (!w || sh.rank > w->rank)) w = &sh;
        return w;
    };
    for (auto& [target, kind] : kind_of) {
        auto it = by_target.find(target);
        const auto* ships = it == by_target.end() ? nullptr : &it->second;
        const Ship* wc = winner(ships, chosen);
        const Ship* wd = winner(ships, s.defaults);
        Want w;
        w.kind = kind;
        if ((wc && wd && wc->id == wd->id) || (!wc && !wd)) {
            w.orig = true;
        } else if (wc) {
            w.orig = false; w.path = wc->path; w.unit = wc->unit;
        } else if (auto b = base.find(target); b != base.end()) {
            w.orig = false; w.path = b->second->path; w.unit = b->second->unit;
        } else {
            w.orig = true;                                   // no way back without the option: the slot's own file
            if (notes) notes->push_back("no base copy of " + target + ": the slot's own file stays");
        }
        out[target] = std::move(w);
    }
    return out;
}

bool removable(const Manifest& m, const Slot& s, const std::string& id) {
    if (!has(s.defaults, id)) return true;                   // not worn by default: On adds it, Off goes back
    std::vector<std::string> without = s.defaults;
    erase_id(without, id);
    for (auto& [t, w] : desired(m, s, without)) if (!w.orig) return true;   // the slot's defaults: every target orig
    return false;
}

int joints_at(const Manifest& m, const Slot& s, const std::string& path) {
    for (auto& f : s.base) if (f.path == path) return f.joints;
    for (auto& [id, files] : s.files) for (auto& f : files) if (f.path == path) return f.joints;
    // a target: the slot's own file there is that of the default option that wins it (same bytes)
    const Mod* mod = m.mod_of(s);
    int rank = -1, j = -1;
    if (mod)
        for (auto& [id, files] : s.files) {
            const int x = mod->find(id);
            if (x < 0 || !has(s.defaults, id) || mod->options[static_cast<size_t>(x)].rank < rank) continue;
            for (auto& f : files) if (f.target == path) { rank = mod->options[static_cast<size_t>(x)].rank; j = f.joints; }
        }
    return j;
}

// ---- keeping a set coherent ----

void choose(const Manifest& m, const Slot& s, std::vector<std::string>& chosen, int group, const std::string& id) {
    const Mod* mod = m.mod_of(s);
    if (!mod || group < 0 || group >= static_cast<int>(mod->groups.size())) return;
    for (int oi : mod->groups[static_cast<size_t>(group)].options) {
        const Option& o = mod->options[static_cast<size_t>(oi)];
        if (o.mode == "pick" && o.id != id) erase_id(chosen, o.id);
    }
    if (id.empty()) return;
    if (!has(chosen, id)) chosen.push_back(id);
    drop_later(m, s, chosen, id);
}

void toggle(const Manifest& m, const Slot& s, std::vector<std::string>& chosen, const std::string& id, bool on,
            std::vector<std::string>* notes) {
    if (!on) { erase_id(chosen, id); return; }
    if (!has(chosen, id)) chosen.push_back(id);
    drop_later(m, s, chosen, id);
    // another On / Off option that replaces one of its files, earlier or later: off (the second one's file would hide
    // the first one's, the first one's other files would stay: a set no outfit has)
    const Mod* mod = m.mod_of(s);
    const auto* fx = files_for(s, id);
    const int x = mod ? mod->find(id) : -1;
    if (x < 0 || !fx) return;
    std::set<std::string> targets;
    for (auto& f : *fx) if (live_kind(f.kind)) targets.insert(f.target);
    for (auto it = chosen.begin(); it != chosen.end();) {
        const int y = mod->find(*it);
        bool drop = false;
        if (*it != id && y >= 0 && mod->options[static_cast<size_t>(y)].mode == "toggle")
            if (const auto* fy = files_for(s, *it))
                for (auto& f : *fy) if (live_kind(f.kind) && targets.count(f.target)) { drop = true; break; }
        if (drop && notes)
            notes->push_back("\"" + mod->options[static_cast<size_t>(y)].name + "\" turned off: it replaces a file of \"" + mod->options[static_cast<size_t>(x)].name + "\"");
        it = drop ? chosen.erase(it) : it + 1;
    }
}

bool same_set(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::set<std::string> sa(a.begin(), a.end()), sb(b.begin(), b.end());
    return sa == sb;
}

// ---- the menu rows ----

bool offered(const Slot& s, const std::string& id, bool* partial) {
    const auto* files = files_for(s, id);
    bool live = false, other = false;
    if (files) for (auto& f : *files) (live_kind(f.kind) ? live : other) = true;
    if (partial) *partial = live && other;
    return live;
}

std::vector<Row> rows(const Manifest& m, const Slot& s, const std::vector<std::string>& chosen) {
    std::vector<Row> out;
    const Mod* mod = m.mod_of(s);
    if (!mod) return out;
    for (size_t gi = 0; gi < mod->groups.size(); ++gi) {
        const Group& g = mod->groups[gi];
        std::vector<int> picks, toggles;
        for (int oi : g.options) {
            const Option& o = mod->options[static_cast<size_t>(oi)];
            if (!offered(s, o.id)) continue;
            if (hidden(m, s, o.id)) continue;                              // NSFW while "Display NSFW Outfits" is off
            if (o.mode == "toggle" && !removable(m, s, o.id)) continue;   // its Off would change nothing
            if (o.mode == "pick") picks.push_back(oi);
            else if (o.mode == "toggle") toggles.push_back(oi);
        }
        // A required group has no "None"; split in rows, its first label only says the choice is on another row
        const bool req = required(s, static_cast<int>(gi));
        const bool none = !req || picks.size() > kMaxLabels;
        const size_t per = none ? kMaxLabels - 1 : kMaxLabels;
        const size_t chunks = (picks.size() + per - 1) / per;
        for (size_t c = 0; c < chunks; ++c) {
            Row r;
            r.kind = Row::Pick;
            r.group = static_cast<int>(gi);
            r.title = display_name(g.name);
            if (chunks > 1) r.title += " (" + std::to_string(c + 1) + "/" + std::to_string(chunks) + ")";
            r.none = none;
            if (none) {
                r.labels.push_back(req ? "(other row)" : "None");
                r.label_option.push_back(-1);
            }
            for (size_t k = c * per; k < picks.size() && k < (c + 1) * per; ++k) {
                const Option& o = mod->options[static_cast<size_t>(picks[k])];
                bool partial = false;
                offered(s, o.id, &partial);
                r.partial = r.partial || partial;
                r.labels.push_back(display_name(o.name) + (partial ? " *" : ""));
                r.label_option.push_back(picks[k]);
                if (has(chosen, o.id)) r.value = static_cast<int>(r.labels.size());
            }
            out.push_back(std::move(r));
        }
        for (int oi : toggles) {
            const Option& o = mod->options[static_cast<size_t>(oi)];
            Row r;
            r.kind = Row::Toggle;
            r.group = static_cast<int>(gi);
            r.option = oi;
            offered(s, o.id, &r.partial);
            r.title = display_name(o.name) + (r.partial ? " *" : "");
            r.labels = { "Off", "On" };
            r.label_option = { -1, oi };
            r.value = has(chosen, o.id) ? 2 : 1;
            out.push_back(std::move(r));
        }
    }
    return out;
}

void apply_row(const Manifest& m, const Slot& s, std::vector<std::string>& chosen, const Row& r, int value,
               std::vector<std::string>* notes) {
    const Mod* mod = m.mod_of(s);
    if (!mod || value < 1 || value > static_cast<int>(r.label_option.size())) return;
    const int oi = r.label_option[static_cast<size_t>(value - 1)];
    if (r.kind == Row::Toggle) {
        if (r.option >= 0) toggle(m, s, chosen, mod->options[static_cast<size_t>(r.option)].id, oi >= 0, notes);
    } else if (oi >= 0) {
        choose(m, s, chosen, r.group, mod->options[static_cast<size_t>(oi)].id);
    } else {
        // None on this row: only when the group's choice is one of this row's options (a group split in
        // several rows keeps a choice made on another row); a required group goes back to its default
        for (int x : r.label_option)
            if (x >= 0 && has(chosen, mod->options[static_cast<size_t>(x)].id)) {
                choose(m, s, chosen, r.group, required(s, r.group) ? default_pick(m, s, r.group) : "");
                break;
            }
    }
    complete(m, s, chosen, notes);                           // an option turned off for this one may leave a required group empty
}

std::vector<std::vector<int>> split_pages(int total, int per_page, int head) {
    std::vector<std::vector<int>> pages;
    per_page = std::max(1, per_page);
    int i = 0;
    do {
        const int room = std::max(1, per_page - (pages.empty() ? head : 0));
        std::vector<int> pg;
        for (; i < total && static_cast<int>(pg.size()) < room; ++i) pg.push_back(i);
        pages.push_back(std::move(pg));
    } while (i < total);
    return pages;
}

PanelWindow panel_clamp(PanelWindow w, int n, int shown) {
    shown = std::max(1, shown);
    if (n <= 0) return {};
    w.cur = std::max(0, std::min(w.cur, n - 1));
    w.win = std::max(0, std::min(w.win, std::max(0, n - shown)));
    if (w.cur < w.win) w.win = w.cur;
    if (w.cur >= w.win + shown) w.win = w.cur - shown + 1;
    return w;
}

PanelWindow panel_move(PanelWindow w, int n, int dir, int shown) {
    shown = std::max(1, shown);
    if (n <= 0) return {};
    w = panel_clamp(w, n, shown);
    if (dir > 0) {
        if (w.cur >= n - 1) return { 0, 0 };                              // Down on the last row: the first
        ++w.cur;
        if (w.cur >= w.win + shown) w.win = w.cur - shown + 1;
    } else if (dir < 0) {
        if (w.cur <= 0) return { n - 1, std::max(0, n - shown) };         // Up on the first row: the last
        --w.cur;
        if (w.cur < w.win) w.win = w.cur;
    }
    return w;
}

PanelWindow panel_focus(PanelWindow w, int n, int k, int shown) {
    w = panel_clamp(w, n, shown);
    if (n <= 0 || k < 0 || k >= std::max(1, shown) || w.win + k >= n) return w;
    w.cur = w.win + k;
    return w;
}

const char* panel_arrow(int k, int win, int n, int shown, bool hollow) {
    const int last = std::min(std::max(1, shown), n - win) - 1;          // the last screen row in use
    if (k == 0 && win > 0) return hollow ? "\xE2\x96\xB3 " : "\xE2\x96\xB2 ";
    if (k == last && win + shown < n) return hollow ? "\xE2\x96\xBD " : "\xE2\x96\xBC ";
    return "";
}

double panel_arrow_left(double title_left, double glyph_w, double offset, double gap) {
    glyph_w = std::max(0.0, glyph_w);
    return std::min(title_left - std::max(0.0, offset), title_left - std::max(0.0, gap) - glyph_w);
}

int vs_model_side(const std::vector<std::string>& up, int* root) {
    if (root) *root = -1;
    for (size_t i = 0; i < up.size() && i < 5; ++i) {
        const int side = up[i] == "Left" ? 1 : up[i] == "Right" ? 2 : 0;
        if (!side || i + 1 >= up.size() || up[i + 1] != "FighterEmoteCtrl") continue;
        if (root) *root = static_cast<int>(i);
        return side;
    }
    return 0;
}

int step_value(int value, int labels, int dir) {
    if (labels <= 0) return 1;
    value = std::max(1, std::min(value, labels));
    if (dir == 0) return value;
    return ((value - 1 + (dir > 0 ? 1 : -1)) % labels + labels) % labels + 1;
}

int preset_of(const Mod& mod, const std::vector<std::string>& chosen) {
    for (size_t k = 0; k < mod.variants.size(); ++k) if (same_set(mod.variants[k].ids, chosen)) return static_cast<int>(k) + 1;
    return 0;
}

std::string preset_label(const Mod& mod, size_t k) {
    if (k >= mod.variants.size()) return {};
    const std::string& l = mod.variants[k].label;
    const std::string pre = mod.name + " - ";
    if (l.size() > pre.size() && l.compare(0, pre.size(), pre) == 0) return l.substr(pre.size());
    return l;
}

const Slot* former_of(const Manifest& m, int fighter, int costume_no, const Former** out) {
    if (m.layout != "one") return nullptr;
    for (auto& s : m.slots) {
        if (s.fighter != fighter) continue;
        for (auto& f : s.former)
            if (f.costume_no == costume_no) { if (out) *out = &f; return &s; }
    }
    return nullptr;
}

} // namespace slots::rto
