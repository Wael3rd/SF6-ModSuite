#include "files.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <system_error>

#include "chars.hpp"
#include "luajson.hpp"
#include "lutil.hpp"
#include "store.hpp"
#include "timeline.hpp"

namespace model::files {
namespace {

namespace fs = std::filesystem;
namespace lj = model::luajson;

// ipairs over a decoded table: the elements up to the first hole
std::vector<const json*> ipairs(const json& t) {
    std::vector<const json*> v;
    if (!t.is_array()) return v;
    for (const json& e : t) { if (e.is_null()) break; v.push_back(&e); }
    return v;
}

// tostring() of what the Lua concatenates into its messages
std::string lua_tostring(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number()) return lutil::number_to_string(v);
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_null()) return "nil";
    return "table";
}

// s:match("[/\\]?([^/\\]+)$") or s, then gsub("%.json$", "")
std::string ticker_stem(const std::string& s) {
    size_t cut = s.find_last_of("/\\");
    std::string last = (cut == std::string::npos) ? s : s.substr(cut + 1);
    if (last.empty()) last = s;
    return lutil::strip_json_ext(last);
}

// The Lua errors the import and the serializer can raise: the text after "Crash: " / "Error: " (line numbers of the
// shipped SF6_RecordingSlotManager.lua with its P3 shims; p3_bench compares them with the Lua's own messages). Only
// malformed packs or a slot without InputData get there.
constexpr const char* kErrNoInputData = "SF6_RecordingSlotManager.lua:615: attempt to index a nil value";
constexpr const char* kErrEmptyNoInput = "SF6_RecordingSlotManager.lua:683: attempt to index a nil value";
constexpr const char* kErrForLimit = "SF6_RecordingSlotManager.lua:703: 'for' limit must be a number";
constexpr const char* kErrSetValue = "SF6_RecordingSlotManager.lua:703: SetValue failed (index out of range)";
constexpr const char* kErrBadInput = "SF6_RecordingSlotManager.lua:709: bad argument to 'create_uint16'";
constexpr const char* kErrSerNoInput = "SF6_RecordingSlotManager.lua:981: attempt to index a nil value";
constexpr const char* kErrSerBuffer = "SF6_RecordingSlotManager.lua:986: attempt to index a nil value";
constexpr const char* kErrSaveNoInput = "SF6_RecordingSlotManager.lua:1096: attempt to index a nil value";
constexpr const char* kErrSaveBuffer = "SF6_RecordingSlotManager.lua:1101: attempt to index a nil value";

// e1 ~= e2 for the values uninstall_big compares (nil, booleans, numbers by value, strings)
bool lua_ne(const json& a, const json& b) {
    if (a.is_number() && b.is_number()) return a.get<double>() != b.get<double>();
    if (a.is_null() || b.is_null()) return !(a.is_null() && b.is_null());
    if (a.type() != b.type()) return true;
    return a != b;
}

}  // namespace

std::vector<std::string> timeline_lines(const json& timeline) {
    std::vector<std::string> v;
    if (!timeline.is_array()) return v;
    for (const json& x : timeline) { if (!x.is_string()) break; v.push_back(x.get<std::string>()); }
    return v;
}

Engine::Engine(fs::path root, LiveSlots* live, Clock* clock) : root_(std::move(root)), live_(live), clock_(clock) {}

fs::path Engine::abs(const std::string& rel) const { return root_ / fs::path(rel); }

json Engine::load_json(const std::string& rel) const { return lj::load_file(abs(rel)); }

// json.dump_file: the same bytes. When nlohmann's dump throws (invalid UTF-8: the cut names cannot carry it any more),
// REFramework has already opened, so truncated, the file: an empty file. When the file cannot be opened: no file.
bool Engine::write_json(const std::string& rel, const json& value) {
    std::string text, err;
    try { text = lj::encode(value).dump(4); }
    catch (const json::exception&) {
        std::error_code ec;
        fs::create_directories(abs(rel).parent_path(), ec);
        std::ofstream f(abs(rel), std::ios::binary | std::ios::trunc);
        return false;
    }
    return lj::write_text(abs(rel), text, &err);
}

std::vector<std::string> Engine::glob_char(const std::string& char_name) const {
    // fs.glob(DATA .. "\\\\" .. char .. "\\\\.*json"): std::regex_match on the path relative to the data folder (backslashes,
    // the code page's narrow string), every regular file under it. Only the character's folder can match.
    std::vector<std::string> out;
    std::regex re;
    try {
        re = std::regex(std::string(model::store::kDataFolder) + "\\\\" + char_name + "\\\\.*json");
    } catch (const std::regex_error&) { return out; }
    std::error_code ec;
    const fs::path dir = root_ / fs::path(std::string(model::store::kDataFolder)) / fs::path(char_name);
    if (!fs::is_directory(dir, ec)) return out;
    fs::recursive_directory_iterator it(dir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) && !it->is_symlink(e2)) continue;
        std::string rel;
        try { rel = fs::relative(it->path(), root_, e2).string(); } catch (...) { continue; }
        if (e2) continue;
        if (rel.size() < 4 || rel.compare(rel.size() - 4, 4, "json") != 0) continue;   // (the pattern ends with a literal "json": cheap pre-filter)
        if (std::regex_match(rel, re)) out.push_back(rel);
    }
    return out;
}

void Engine::load_settings() {
    const json cfg = load_json(model::store::config_path());
    activate_on_load_ = model::store::config_activate_on_load(cfg, activate_on_load_);
    import_setup_ = model::store::config_import_setup(cfg);
}

std::vector<std::string> Engine::migrate_unknown_folders() {
    std::vector<std::string> out;
    const fs::path data = root_ / fs::path(std::string(model::store::kDataFolder));
    for (const auto& c : model::chars::all()) {
        std::error_code ec;
        const std::string old_name = "Unknown(" + std::to_string(c.id) + ")";
        const fs::path from = data / fs::path(old_name), to = data / fs::path(std::string(c.name));
        if (!fs::is_directory(from, ec)) continue;
        if (!fs::exists(to, ec)) {
            fs::rename(from, to, ec);
            out.push_back(old_name + " -> " + c.name + (ec ? " FAILED: " + ec.message() : " (renamed)"));
            continue;
        }
        int moved = 0, kept = 0;
        std::vector<fs::path> files;
        for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2)) files.push_back(it->path());
        }
        for (const fs::path& f : files) {
            std::error_code e2;
            const fs::path dst = to / fs::relative(f, from, e2);
            if (e2 || fs::exists(dst, e2)) { ++kept; continue; }
            fs::create_directories(dst.parent_path(), e2);
            fs::rename(f, dst, e2);
            if (e2) ++kept; else ++moved;
        }
        if (kept == 0) fs::remove_all(from, ec);
        out.push_back(old_name + " -> " + c.name + ": " + std::to_string(moved) + " file(s) moved, " +
                      std::to_string(kept) + " left in " + old_name);
    }
    return out;
}

std::optional<std::string> Engine::char_name() const {
    if (fid_ == -1) return std::nullopt;
    return model::chars::get_char_name(fid_);
}

// ---- MAIN: erased, names, lists ------------------------------------------------------------------------------------------
void Engine::load_erased() {
    erased_.clear();
    if (fid_ == -1) return;
    erased_ = model::store::erased_load(load_json(model::store::erased_path(model::chars::get_char_name(fid_))));
}

void Engine::mark_erased(const std::string& filename) {
    erased_.insert(filename);
    write_json(model::store::erased_path(model::chars::get_char_name(fid_)), model::store::erased_save(erased_));
}

void Engine::load_names() {
    names_.clear();
    if (fid_ == -1) return;
    names_ = model::store::names_load(load_json(model::store::names_path(model::chars::get_char_name(fid_))));
}

void Engine::set_display_name(const std::string& filename, const std::string* display) {
    if (filename.empty()) return;
    model::store::set_display_name(names_, filename, display);
    write_json(model::store::names_path(model::chars::get_char_name(fid_)), model::store::names_save(names_));
}

std::string Engine::display_name(const std::string& file) const { return model::store::display_name(file, names_); }

void Engine::refresh_file_list() {
    cached_file_list_.clear();
    files_char_ = fid_;
    if (fid_ == -1) return;
    load_erased();
    load_names();
    cached_file_list_ = model::store::pack_files(glob_char(model::chars::get_char_name(fid_)), erased_);
}

void Engine::refresh_filtered_list() {
    const std::string previous = custom_file_input_;
    filtered_file_list_.clear();
    display_list_ = { "-- Select File --" };
    dropdown_index_ = 1;
    custom_file_input_.clear();
    for (const std::string& f : cached_file_list_) {
        filtered_file_list_.push_back(f);
        display_list_.push_back(display_name(f));
    }
    if (!previous.empty()) {
        for (size_t i = 0; i < filtered_file_list_.size(); ++i) {
            if (filtered_file_list_[i] == previous) { dropdown_index_ = (int)i + 2; custom_file_input_ = previous; break; }
        }
    }
    ++list_version_;
}

bool Engine::ensure_file_list() {
    if (files_char_ && *files_char_ == fid_) return false;
    refresh_file_list();
    refresh_filtered_list();
    return true;
}

void Engine::reset_selection() {
    custom_file_input_.clear();
    loaded_file_name_.clear();
    dropdown_index_ = 1;
    ++list_version_;
}

void Engine::fighter_changed(int new_id) {
    if (new_id == fid_) return;
    fid_ = new_id;
    for (json& n : slot_names_) n = "";
    reset_selection();
    files_char_.reset();
    cached_file_list_.clear();
    refresh_filtered_list();
}

bool Engine::ensure(int id) {
    bool changed = false;
    if (id != -1 && id != fid_) {
        changed = true;
        fid_ = id;
        reset_selection();
    }
    if (ensure_file_list()) return true;
    // A fresh list is read again too: a pack copied into the folder by hand (or removed) shows at the next entry of the
    // Manager page, the call's only regular caller (once per entry; the other calls follow an action or a stale list).
    // The list and its version change only when the folder did; the selection is kept while its file is still there.
    const std::vector<std::string> before = cached_file_list_;
    const std::vector<std::string> shown(display_list_.begin() + (display_list_.empty() ? 0 : 1), display_list_.end());
    refresh_file_list();
    bool same = cached_file_list_ == before && shown.size() == cached_file_list_.size();
    for (size_t i = 0; same && i < cached_file_list_.size(); ++i) same = display_name(cached_file_list_[i]) == shown[i];
    if (!same) refresh_filtered_list();
    return changed || !same;
}

void Engine::set_dropdown_index(const json& idx_in) {
    int idx = 1;
    if (const auto n = lutil::to_number(idx_in)) {
        const double d = n->get<double>();
        idx = (d == (double)(int64_t)d && d > -1e9 && d < 1e9) ? (int)d : 1;
    }
    if (idx < 1 || idx > (int)display_list_.size()) idx = 1;
    dropdown_index_ = idx;
    const int file_idx = idx - 1;
    if (file_idx >= 1 && file_idx <= (int)filtered_file_list_.size()) custom_file_input_ = filtered_file_list_[(size_t)file_idx - 1];
    else custom_file_input_.clear();
    ++list_version_;
}

std::string Engine::selected_name() const {
    if (custom_file_input_.empty()) return "";
    return display_name(custom_file_input_);
}

std::string Engine::loaded_name() const {
    if (loaded_file_name_.empty()) return "";
    return display_name(loaded_file_name_ + ".json");
}

// ---- the live slots --------------------------------------------------------------------------------------------------------
std::optional<json> Engine::serialize_slots(int use_id, std::string* char_name, std::string* err, bool* crashed, bool save_site,
                                            std::array<bool, 8>* actives) {
    if (crashed) *crashed = false;
    const std::string a = live_->access(use_id);
    if (!a.empty()) { if (err) *err = a; return std::nullopt; }
    json data = json::array();
    if (char_name) *char_name = model::chars::get_char_name(use_id);
    for (int i = 0; i < 8; ++i) {
        LiveRead r;
        const int rc = live_->read(use_id, i, &r);
        if (rc != 0) {
            if (err) *err = rc == 1 ? (save_site ? kErrSaveNoInput : kErrSerNoInput) : (save_site ? kErrSaveBuffer : kErrSerBuffer);
            if (crashed) *crashed = true;
            return std::nullopt;
        }
        if (actives) (*actives)[(size_t)i] = r.active;
        json e = json::object();
        e["id"] = i + 1;
        e["weight"] = r.weight;
        const json& nm = slot_names_[(size_t)i];
        if (!(nm.is_string() && nm.get<std::string>().empty())) e["name"] = nm;
        if (r.valid && r.frames > 0) {
            json tl = json::array();
            for (auto& s : model::timeline::compress(r.values.data(), r.values.size())) tl.push_back(s);
            e["timeline"] = std::move(tl);
            e["empty"] = false;
        } else {
            e["empty"] = true;
            e["timeline"] = json::array();
        }
        data.push_back(std::move(e));
    }
    return data;
}

// The pack's `active` flags (live from the game, 9-30 from _virtual_slots.json) and the training block. An export carries
// the reversal rows only when one of them is valid (an unused reversal setup would overwrite the importer's for nothing),
// trimmed to the game's 10 rows when CRSM's 11-30 are all empty; a backup always carries them, so that importing it puts
// back what an import replaced. The replay settings and the dummy's settings are always carried (each is a setting).
void Engine::stamp_pack(json& pack, int use_id, const std::array<bool, 8>& live_active, bool backup) {
    std::array<bool, model::setup::kSlotTotal> on{};
    for (size_t i = 0; i < 8; ++i) on[i] = live_active[i];
    if (!backup) {
        model::vslots::VSlots& v = vslots();
        for (int n = model::vslots::kFirst; n <= model::vslots::kLast; ++n) {
            const json* e = v.get(n);
            on[(size_t)n - 1] = e && model::setup::active_of(*e).value_or(false) && v.frames(n) > 0;
        }
    }
    model::setup::stamp_active(pack, on);
    if (!setup_) return;
    model::setup::Setup st;
    if (!setup_->read(use_id, &st)) return;
    if (st.reversal) {
        if (!backup && !model::setup::rows_in_use(*st.reversal)) st.reversal.reset();
        else model::setup::trim(*st.reversal);
    }
    if (st.reversal || st.playback || st.dummy) model::setup::stamp(pack, st);
}

void Engine::backup_live_slots(int use_id) {
    if (!backup_gate_.allow(use_id, clock_->seconds())) return;
    std::string p2_name, err;
    std::array<bool, 8> act{};
    auto data = serialize_slots(use_id, &p2_name, &err, nullptr, false, &act);
    if (!data) return;
    stamp_pack(*data, use_id, act, true);
    const std::string folder = model::store::backups_folder(p2_name);
    write_json(folder + "/" + model::pack::backup_filename(p2_name, clock_->local_now()), *data);
}

// After the slots of a pack are written: its `active` flags over activate_on_load (an entry without one keeps what
// activate_on_load gave it), then its training block (config.json import_training_setup = false skips the block).
void Engine::apply_flags_and_setup(int use_id, const json& data) {
    setup_report_.clear();
    for (const json* s : ipairs(data)) {
        const auto a = model::setup::active_of(*s);
        if (!a) continue;
        const int k0 = (int)lj::index(*s, "id").get<double>() - 1;
        if (k0 < 0 || k0 > 7) continue;
        live_->set_active(use_id, k0, *a && !lutil::truthy(lj::index(*s, "empty")));
    }
    std::string why;
    const auto st = model::setup::of_pack(data, &why);
    if (!st) return;
    if (!import_setup_) { setup_report_ = "training block skipped (config.json import_training_setup = false)"; return; }
    if (!setup_) { setup_report_ = "training block skipped (no game port)"; return; }
    setup_->write(use_id, *st, &setup_report_);
}

// apply_data_to_character (MAIN:581-744). `crashed` = the Lua raised (the caller's pcall gives "Crash: " / "Error: ").
std::string Engine::apply(int target_id, json data, const std::string& source_name, bool* crashed, std::string* ticker) {
    *crashed = false;
    const int use_id = target_id;
    std::string verr;
    if (!model::pack::validate(data, &verr)) return "Import rejected (" + source_name + "): " + verr;
    const std::string access = live_->access(use_id);
    if (!access.empty()) return "Error: " + access;

    if (source_name != "virtual slot") {                       // packs hold 30 slots: 9-30 to the virtual slots
        const model::pack::Split sp = model::pack::split(data);
        import_virtual(sp.virt);
        data = sp.live;
        if (lj::len(data) == 0) return "Loaded: " + source_name + " (virtual slots only)";
    }
    if (source_name != "virtual slot") backup_live_slots(use_id);

    // 1. memory of every slot
    std::vector<int> missing;
    for (const json* s : ipairs(data)) {
        if (lutil::truthy(lj::index(*s, "empty"))) continue;
        const int k0 = (int)lj::index(*s, "id").get<double>() - 1;
        const int64_t cap = live_->capacity(use_id, k0);
        if (cap < 0) { *crashed = true; return kErrNoInputData; }  // slot.InputData nil: the Lua indexes nil
        const json tl = lj::index(*s, "timeline");
        int64_t needed = 0;
        if (lutil::truthy(tl)) {
            if (!tl.is_array() && !tl.is_object()) { *crashed = true; return kErrForLimit; }
            needed = model::timeline::needed_frames(timeline_lines(tl));
        } else needed = (int64_t)lj::len(lj::index(*s, "inputs"));
        if (cap < needed && !live_->allocate(use_id, k0, needed)) missing.push_back(k0);
    }
    // 2. memory missing: the recording-based allocation, then the import again
    if (!missing.empty()) {
        queue_.clear();
        for (int k0 : missing) { AllocAction a; a.type = AllocAction::Type::Alloc; a.slot = k0; queue_.push_back(a); }
        AllocAction w;
        w.type = AllocAction::Type::WriteData;
        w.target_id = use_id;
        w.data = data;
        w.name = source_name;
        queue_.push_back(w);
        return "Auto-Allocating " + std::to_string(missing.size()) + " slots... Please wait.";
    }
    // 3. write
    for (const json* s : ipairs(data)) {
        const int k0 = (int)lj::index(*s, "id").get<double>() - 1;
        const json w = lj::index(*s, "weight");
        if (lutil::truthy(w)) live_->set_weight(use_id, k0, w);
        if (lutil::truthy(lj::index(*s, "empty"))) {
            // IsValid / Frame / IsActive, then InputData.Num: without InputData the Lua raises after the three fields
            if (!live_->write_empty(use_id, k0)) { *crashed = true; return kErrEmptyNoInput; }
            continue;
        }
        const json tl = lj::index(*s, "timeline");
        int64_t needed = 0;
        if (lutil::truthy(tl)) {
            const auto lines = timeline_lines(tl);
            needed = model::timeline::needed_frames(lines);
            const model::timeline::Expanded ex = model::timeline::expand(lines);
            if (!ex.values.empty() && !live_->write_values(use_id, k0, ex.values.data(), ex.values.size())) { *crashed = true; return kErrSetValue; }
            if (!ex.ok) { *crashed = true; return kErrForLimit; }    // the lines before the bad one are written, as the Lua
        } else {
            const json inputs = lj::index(*s, "inputs");
            needed = (int64_t)lj::len(inputs);
            std::vector<uint16_t> vals;
            bool bad = false;
            for (int64_t f = 0; f < needed; ++f) {
                const json& v = inputs[(size_t)f];
                if (v.is_string()) vals.push_back((uint16_t)model::timeline::encode_from_numpad(v.get<std::string>()));
                else if (v.is_number()) {
                    const double d = v.get<double>();
                    if (d != (double)(int64_t)d) { bad = true; break; }
                    vals.push_back((uint16_t)(int64_t)d);
                } else { bad = true; break; }
            }
            if (!vals.empty() && !live_->write_values(use_id, k0, vals.data(), vals.size())) { *crashed = true; return kErrSetValue; }
            if (bad) { *crashed = true; return kErrBadInput; }
        }
        live_->finish(use_id, k0, needed);
    }
    for (const json* s : ipairs(data)) {                          // the names (a "virtual slot" write names slot 8 too)
        const int k0 = (int)lj::index(*s, "id").get<double>() - 1;
        if (k0 < 0 || k0 > 7) continue;
        const json nm = lj::index(*s, "name");
        slot_names_[(size_t)k0] = lutil::truthy(nm) ? nm : json("");
    }
    if (activate_on_load_) live_->activate_valid(use_id);
    if (source_name != "virtual slot") apply_flags_and_setup(use_id, data);   // a virtual slot's entry: slot 8 keeps its IsActive
    if (ticker && !quiet_ && !native_action_) *ticker = ticker_stem(source_name) + " File Loaded";   // MAIN:739-742
    return "Loaded: " + source_name;
}

void Engine::import_virtual(const json& list) {
    model::vslots::VSlots& v = vslots();
    v.import_virtual(list);
    vslots_save();
    ++vslots_imports_;
}

std::string Engine::save_custom_file_text(std::string* ticker, bool* crashed) {
    *crashed = false;
    std::string filepath = custom_file_input_;
    if (filepath.empty()) return "Empty Path";
    const std::string char_name = model::chars::get_char_name(fid_);
    filepath = model::pack::save_filename(filepath, char_name);
    std::string err, p2;
    bool ser_crash = false;
    std::array<bool, 8> act{};
    const auto live = serialize_slots(fid_, &p2, &err, &ser_crash, true, &act);
    if (!live) {
        if (ser_crash) { *crashed = true; return err; }
        return "Error: " + err;
    }
    json data = model::pack::assemble(*live, vslots().export_virtual());
    stamp_pack(data, fid_, act, false);
    // The loaded pack's `info` (its description, author, source) goes with what the game now holds, read before the
    // write (Save over the loaded file replaces it): Save and Save As keep a downloaded drill's description.
    if (!loaded_file_name_.empty()) {
        const json src = load_json(model::store::pack_path(char_name, lutil::strip_json_ext(loaded_file_name_) + ".json"));
        const json* e1 = model::setup::entry_one(src);          // entry_one finds objects only
        json* d1 = model::setup::entry_one(data);
        if (e1 && d1 && e1->contains(model::setup::kInfoKey) && (*e1)[model::setup::kInfoKey].is_object())
            (*d1)[model::setup::kInfoKey] = (*e1)[model::setup::kInfoKey];
    }
    write_json(model::store::pack_path(char_name, filepath), data);
    custom_file_input_ = filepath;
    loaded_file_name_ = lutil::strip_json_ext(filepath);
    refresh_file_list();
    refresh_filtered_list();
    if (ticker) *ticker = lutil::strip_json_ext(filepath) + " File Saved";
    return "Custom Saved: " + filepath;
}

std::string Engine::import_custom_file_text(bool* crashed) {
    *crashed = false;
    std::string filepath = custom_file_input_;
    if (filepath.empty()) return "Empty Path";
    if (!lutil::ends_with(filepath, ".json")) filepath += ".json";
    const std::string char_name = model::chars::get_char_name(fid_);
    const json data = load_json(model::store::pack_path(char_name, filepath));
    if (data.is_null()) return "Load Failed: " + filepath;
    const std::string result = apply(fid_, data, filepath, crashed, nullptr);
    if (!*crashed && result.rfind("Loaded:", 0) == 0) loaded_file_name_ = custom_file_input_;   // with ".json", as the Lua
    return result;
}

// ---- the actions ---------------------------------------------------------------------------------------------------------
std::string Engine::import_selected() {
    if (custom_file_input_.empty()) { status_ = "No file selected"; return status_; }
    quiet_ = true;
    bool crashed = false;
    const std::string res = import_custom_file_text(&crashed);
    quiet_ = false;
    status_ = crashed ? "Crash: " + res : res;
    return status_;
}

std::string Engine::export_as(const json& name_in, std::string* ticker) {
    if (fid_ == -1) { status_ = "No character"; return status_; }
    const std::string typed = (name_in.is_null() || (name_in.is_boolean() && !name_in.get<bool>())) ? std::string() : lua_tostring(name_in);
    const model::pack::TypedName tn = model::pack::typed_name(typed);
    if (tn.file.empty()) { status_ = "No name"; return status_; }
    custom_file_input_ = tn.file;
    bool crashed = false;
    const std::string res = save_custom_file_text(ticker, &crashed);
    status_ = crashed ? "Crash: " + res : res;
    if (!crashed && res.find("Saved") != std::string::npos && !loaded_file_name_.empty()) {
        set_display_name(loaded_file_name_ + ".json", &tn.display);
        refresh_filtered_list();
    }
    for (size_t i = 0; i < filtered_file_list_.size(); ++i)
        if (filtered_file_list_[i] == tn.file) { dropdown_index_ = (int)i + 2; ++list_version_; }
    return status_;
}

std::string Engine::export_selected(std::string* ticker) {
    if (fid_ == -1) { status_ = "No character"; return status_; }
    if (custom_file_input_.empty()) custom_file_input_ = model::chars::get_char_name(fid_) + ".json";
    bool crashed = false;
    const std::string res = save_custom_file_text(ticker, &crashed);
    status_ = crashed ? "Crash: " + res : res;
    return status_;
}

std::string Engine::erase_selected() {
    if (fid_ == -1 || custom_file_input_.empty()) { status_ = "No loadout selected"; return status_; }
    const std::string name = custom_file_input_;
    write_json(model::store::pack_path(model::chars::get_char_name(fid_), name), json::object());
    mark_erased(name);
    set_display_name(name, nullptr);
    if (loaded_file_name_ + ".json" == name || loaded_file_name_ == name) loaded_file_name_.clear();
    custom_file_input_.clear();
    refresh_file_list();
    refresh_filtered_list();
    status_ = "Erased: " + name;
    return status_;
}

std::string Engine::rename_selected(const json& new_name, std::string* ticker) {
    if (fid_ == -1 || custom_file_input_.empty()) { status_ = "No loadout selected"; return status_; }
    const std::string old_name = custom_file_input_;
    const std::string res = export_as(new_name, ticker);
    if (res.find("Saved") == std::string::npos) return res;
    if (custom_file_input_ != old_name) {
        write_json(model::store::pack_path(model::chars::get_char_name(fid_), old_name), json::object());
        mark_erased(old_name);
        set_display_name(old_name, nullptr);
        if (loaded_file_name_ + ".json" == old_name) loaded_file_name_ = lutil::strip_json_ext(custom_file_input_);
        refresh_file_list();
        refresh_filtered_list();
    }
    status_ = "Renamed: " + old_name + " -> " + custom_file_input_;
    return status_;
}

std::string Engine::reopen_loaded() {
    if (fid_ == -1) { status_ = "No character"; return status_; }
    std::string name = loaded_file_name_;
    if (name.empty()) { status_ = "No loadout loaded"; return status_; }
    if (!lutil::ends_with(name, ".json")) name += ".json";
    const json data = load_json(model::store::pack_path(model::chars::get_char_name(fid_), name));
    if (data.is_null()) { status_ = "Load Failed: " + name; return status_; }
    quiet_ = true;
    bool crashed = false;
    const std::string res = apply(fid_, data, name, &crashed, nullptr);
    quiet_ = false;
    status_ = crashed ? "Crash: " + res : res;
    return status_;
}

// ---- slot API ----------------------------------------------------------------------------------------------------------------
json Engine::slot_name(int idx0) const {
    if (idx0 < 0 || idx0 > 7) return "";
    return slot_names_[(size_t)idx0];
}

void Engine::set_slot_name(int idx0, const json& name) {
    if (idx0 < 0 || idx0 > 7) return;
    if (name.is_string()) slot_names_[(size_t)idx0] = lutil::utf8_cut(name.get<std::string>(), model::vslots::kNameMax);
    else slot_names_[(size_t)idx0] = lutil::truthy(name) ? name : json("");
}

std::optional<json> Engine::read_slot(int idx0, std::string* err) {
    std::string p2;
    bool crashed = false;
    const auto data = serialize_slots(fid_, &p2, err, &crashed, false);
    if (!data || idx0 < 0 || idx0 >= (int)data->size()) return std::nullopt;
    return (*data)[(size_t)idx0];
}

std::string Engine::write_slot(const json& entry) {
    if (fid_ == -1) return "No character";
    quiet_ = true;
    bool crashed = false;
    json one = json::array();
    one.push_back(entry);
    const std::string res = apply(fid_, one, "virtual slot", &crashed, nullptr);
    quiet_ = false;
    return crashed ? "Error: " + res : res;
}

// ---- virtual slots ---------------------------------------------------------------------------------------------------------
model::vslots::VSlots& Engine::vslots() {
    const auto c = char_name();
    if (c && (!vslots_char_ || *vslots_char_ != *c)) {
        vslots_char_ = *c;
        vslots_.load(load_json(model::store::virtual_slots_path(*c)));
        ++vslots_version_;
    }
    return vslots_;
}

void Engine::vslots_save() {
    if (!vslots_char_) return;
    write_json(model::store::virtual_slots_path(*vslots_char_), vslots_.to_file());
    ++vslots_version_;
}

void Engine::name_slot(int n, const json& name) {
    const std::string s = name.is_string() ? name.get<std::string>() : (name.is_null() ? std::string() : lua_tostring(name));
    const std::string cut = lutil::utf8_cut(s, model::vslots::kNameMax);
    if (n <= 8) { set_slot_name(n - 1, json(cut)); return; }
    vslots().set_name(n, cut);
    vslots_save();
}

void Engine::vslot_delete(int n) {
    model::vslots::VSlots& v = vslots();
    v.erase(n);
    vslots_save();
}

void Engine::vslot_put(int n, const json& entry, bool save) {
    vslots().put(n, entry);
    if (save) vslots_save();
}

void Engine::vslots_active_weight(const json& patch) {
    model::vslots::VSlots& v = vslots();
    bool changed = false;
    if (patch.is_object()) {
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            const auto key = lutil::to_number(json(it.key()));
            const auto n = key ? lutil::to_int_key(*key) : std::nullopt;
            if (!n || !it.value().is_array() || it.value().size() < 2) continue;
            json* e = v.mut((int)*n);
            if (!e || !e->is_object()) continue;
            const json act = json(it.value()[0].is_boolean() && it.value()[0].get<bool>());
            const json w = it.value()[1];
            if (lua_ne(lj::index(*e, "active"), act) || lua_ne(lj::index(*e, "weight"), w)) {
                (*e)["active"] = act;
                (*e)["weight"] = w;
                changed = true;
            }
        }
    }
    if (changed) vslots_save();
}

// ---- ALLOC / WRITE_DATA (MAIN:871-954) ---------------------------------------------------------------------------------------
std::string Engine::alloc_tick(bool paused, AllocEnv& env, bool* crashed) {
    if (crashed) *crashed = false;
    if (queue_.empty() || paused) return "";
    AllocAction& action = queue_.front();
    if (action.type == AllocAction::Type::Alloc) {
        switch (action.step) {
            case AllocAction::Step::Init:
                if (env.start_recording(action.slot)) {
                    action.timer = 0;
                    action.step = AllocAction::Step::Waiting;
                    status_ = "Allocating Slot " + std::to_string(action.slot + 1) + "...";
                } else queue_.pop_front();
                break;
            case AllocAction::Step::Waiting:
                ++action.timer;
                if (action.timer > 10) action.step = AllocAction::Step::Stop1;
                break;
            case AllocAction::Step::Stop1:
                env.stop_phase1();
                action.step = AllocAction::Step::Stop2;
                break;
            case AllocAction::Step::Stop2:
                env.stop_phase2();
                queue_.pop_front();
                break;
        }
        return "";
    }
    // WRITE_DATA: the import again (it may clear the queue and queue new allocations, as the Lua does)
    std::string res;
    std::string ticker;
    const int target = action.target_id;
    const std::string name = action.name;
    if (action.already_retried) {
        res = "Alloc Failed: memory still insufficient after retry";
    } else {
        bool c = false;
        const json data = action.data;
        res = apply(target, data, name, &c, &ticker);
        if (c) {                                                  // the Lua's on_frame raises here: the action stays queued
            status_ = res;
            if (crashed) *crashed = true;
            return "";
        }
        for (AllocAction& q : queue_)
            if (q.type == AllocAction::Type::WriteData && q.target_id == target) q.already_retried = true;
    }
    status_ = res;
    if (res.rfind("Loaded:", 0) == 0) {
        loaded_file_name_ = lutil::strip_json_ext(name);
        custom_file_input_ = name;
        ++list_version_;
    }
    if (!queue_.empty()) queue_.pop_front();                    // table.remove(action_queue, 1): the head of the queue as it is now
    return ticker;
}

}  // namespace model::files
