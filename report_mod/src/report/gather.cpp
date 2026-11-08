#include "report/gather.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>

namespace report {
namespace {

namespace fs = std::filesystem;

constexpr size_t kJsonMaxBytes = 16u * 1024 * 1024;       // outfits.json, registry.json: 15 KB and 50 KB on a large install
constexpr size_t kLogReadHead = 32 * 1024;                // a log is read around its two ends, whatever its size
constexpr size_t kLogReadTail = 256 * 1024;
constexpr uint64_t kHashMaxBytes = 128u * 1024 * 1024;

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string env_utf8(const wchar_t* name) {
    wchar_t b[512];
    const DWORD n = GetEnvironmentVariableW(name, b, 512);
    return n && n < 512 ? narrow(std::wstring(b, n)) : std::string();
}

bool read_all(const fs::path& p, size_t max_bytes, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::error_code ec;
    const uintmax_t size = fs::file_size(p, ec);
    if (ec || size > max_bytes) return false;
    out->resize((size_t)size);
    f.read(out->data(), (std::streamsize)size);
    return (size_t)f.gcount() == (size_t)size;
}

// A log's two ends, with the middle left out when it is large
bool read_log(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::error_code ec;
    const uintmax_t size = fs::file_size(p, ec);
    if (ec) return false;
    if (size <= kLogReadHead + kLogReadTail) {
        out->resize((size_t)size);
        f.read(out->data(), (std::streamsize)size);
        return true;
    }
    std::string head(kLogReadHead, '\0'), tail(kLogReadTail, '\0');
    f.read(head.data(), (std::streamsize)head.size());
    f.seekg((std::streamoff)(size - kLogReadTail));
    f.read(tail.data(), (std::streamsize)tail.size());
    *out = head + "\n...\n" + tail;
    return true;
}

json read_json(const fs::path& p, bool* ok) {
    std::string text;
    *ok = false;
    if (!read_all(p, kJsonMaxBytes, &text)) return nullptr;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return nullptr;
    *ok = true;
    return j;
}

std::string str_of(const json& o, const char* k) {
    if (!o.is_object()) return "";
    auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// One file's size and the start of its SHA-256 (how a build is named here), without reading a huge one
void file_id(const fs::path& root, const char* rel, json* into) {
    std::error_code ec;
    const fs::path p = root / rel;
    const uintmax_t size = fs::file_size(p, ec);
    if (ec) return;
    json o = json::object();
    o["bytes"] = size;
    std::string data;
    if (size <= kHashMaxBytes && read_all(p, (size_t)kHashMaxBytes, &data)) o["sha256"] = sha256_hex(data.data(), data.size()).substr(0, 12);
    (*into)[fs::path(rel).filename().string()] = std::move(o);
}

// name= and version= of the all-in-one's modinfo.ini, when one is in the game folder
std::string modinfo_line(const fs::path& p) {
    std::string text;
    if (!read_all(p, 64 * 1024, &text)) return "";
    std::string name, version;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(at, e - at);
        at = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.rfind("name=", 0) == 0) name = line.substr(5);
        else if (line.rfind("version=", 0) == 0) version = line.substr(8);
    }
    return name.empty() && version.empty() ? "" : name + " " + version;
}

json versions_of(const Snapshot& s, const fs::path& root, const json& outfits, const json& variants) {
    json v = json::object();
    v["report_mod"] = s.reporter_version;
    v["native_core"] = s.core_version;
    bool ok = false;
    const json rm = read_json(root / "reframework/data/SF6_CRSMNative_data/status.json", &ok);
    if (ok && !str_of(rm, "version").empty()) v["recording_manager"] = str_of(rm, "version");
    ok = false;
    const json cm = read_json(root / "reframework/data/SF6_ColorModNative_data/status.json", &ok);
    if (ok && !str_of(cm, "version").empty()) v["color_mod"] = str_of(cm, "version");
    if (!str_of(outfits, "loader").empty()) v["slots_loader_pak"] = str_of(outfits, "loader");
    if (variants.is_object()) {
        if (!str_of(variants, "runtime_options").empty()) v["runtime_options"] = str_of(variants, "runtime_options");
        if (variants.contains("contract")) v["manager_contract"] = variants["contract"];
    }
    const std::string mi = modinfo_line(root / "modinfo.ini");
    if (!mi.empty()) v["modinfo"] = mi;
    json files = json::object();
    for (const char* rel : { "reframework/plugins/report_mod.dll", "reframework/plugins/SF6_CRSMNative.dll", "reframework/plugins/sf6core_native.dll", "reframework/plugins/SF6_ColorModNative.dll",
                             "reframework/plugins/SF6_CostumeSlotsNative.dll", "reframework/plugins/profile_mod.dll", "amd_ags_x64.dll", "dinput8.dll" })
        file_id(root, rel, &files);
    v["files"] = std::move(files);
    return v;
}

// A folder's names, two levels down: character folders and what is in them
void list_folder(const fs::path& root, const char* rel, const char* kind, const Scrub& scrub, json* mods, size_t* bytes) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root / rel, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (it.depth() >= 1) it.disable_recursion_pending();
        const std::string name = narrow(it->path().filename().wstring());
        if (name.empty() || name[0] == '.' || (it.depth() == 0 && it->is_directory(ec))) continue;
        json o = json::object();
        o["kind"] = kind;
        o["source"] = clean_paths(narrow(it->path().lexically_relative(root / rel).generic_wstring()), scrub);
        const size_t add = o.dump(-1, ' ', false, json::error_handler_t::replace).size() + 1;
        if (*bytes + add > kModsMaxBytes) return;
        *bytes += add;
        mods->push_back(std::move(o));
    }
}

// What the loader's log says of this outfit is found by: its mod's archive (name and stem, as in "archive: Mai/Mai - Animal Print.rar"), the mod's id,
// the label, the slot's folder ("esf028/012"). A game outfit has none of them: the log is capped as before.
LogKeys log_keys_of(const json* outfit, const json* slot, const Scrub& scrub) {
    LogKeys k;
    const auto add = [](std::vector<std::string>* v, const std::string& s) { if (s.size() >= 4 && std::find(v->begin(), v->end(), s) == v->end()) v->push_back(s); };
    if (outfit) {
        const std::string source = clean_paths(str_of(*outfit, "source"), scrub);
        size_t from = 0;
        while (from <= source.size()) {                                  // each part of the source: the archive's name, its stem
            size_t to = source.find_first_of("/\\", from);
            if (to == std::string::npos) to = source.size();
            const std::string part = source.substr(from, to - from);
            const size_t dot = part.find_last_of('.');
            const bool last = to == source.size();
            std::string ext = dot == std::string::npos ? "" : part.substr(dot);
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            const bool archive = ext == ".zip" || ext == ".rar" || ext == ".7z";
            if (archive || last) { add(&k.blocks, part); if (dot != std::string::npos && dot > 0) add(&k.blocks, part.substr(0, dot)); }
            from = to + 1;
        }
        add(&k.names, clean_paths(str_of(*outfit, "label"), scrub));
        add(&k.blocks, str_of(*outfit, "mod_id"));
    }
    if (slot) {
        add(&k.blocks, str_of(*slot, "mod_id"));
        const std::string dir = str_of(*slot, "fighter_dir"), folder = str_of(*slot, "new_folder");
        if (!dir.empty() && !folder.empty()) k.slot = dir + "/" + folder;
    }
    return k;
}

json mods_of(const fs::path& root, const json& outfits, const Scrub& scrub, std::vector<std::string>* notes) {
    json mods = json::array();
    size_t bytes = 2, left_out = 0;
    std::set<std::string> seen;
    const json* list = outfits.is_object() && outfits.contains("outfits") && outfits["outfits"].is_array() ? &outfits["outfits"] : nullptr;
    if (list) {
        for (const json& o : *list) {
            const std::string source = clean_paths(str_of(o, "source"), scrub), label = clean_paths(str_of(o, "label"), scrub);
            if (!seen.insert(source + "|" + label).second) continue;
            json m = json::object();
            m["kind"] = "costume";
            m["label"] = label;
            m["source"] = source;
            if (o.contains("fighter")) m["fighter"] = o["fighter"];
            if (o.contains("nsfw")) m["nsfw"] = o["nsfw"];
            const size_t add = m.dump(-1, ' ', false, json::error_handler_t::replace).size() + 1;
            if (bytes + add > kModsMaxBytes) { ++left_out; continue; }
            bytes += add;
            mods.push_back(std::move(m));
        }
    } else {
        list_folder(root, "reframework/costume_mods", "costume", scrub, &mods, &bytes);
    }
    list_folder(root, "reframework/stage_mods", "stage", scrub, &mods, &bytes);
    list_folder(root, "reframework/color_mods", "colour", scrub, &mods, &bytes);
    if (left_out) notes->push_back(std::to_string(left_out) + " costume mods left out of the list (its size cap)");
    return mods;
}

}  // namespace

Scrub scrub_here(const fs::path& root) {
    Scrub s;
    for (const std::string& n : { env_utf8(L"USERNAME"), env_utf8(L"COMPUTERNAME") }) if (!n.empty()) s.names.push_back(n);
    const std::string profile = env_utf8(L"USERPROFILE");    // its last part may differ from USERNAME
    if (const size_t at = profile.find_last_of("\\/"); at != std::string::npos && at + 1 < profile.size()) {
        const std::string last = profile.substr(at + 1);
        if (std::find(s.names.begin(), s.names.end(), last) == s.names.end()) s.names.push_back(last);
    }
    s.game_root = narrow(root.wstring());
    return s;
}

Report gather(const Snapshot& snap, const fs::path& root, const Scrub& scrub) {
    Report r;
    std::vector<std::string> notes;
    r.text = snap.text;
    r.character = snap.character;
    r.fighter = snap.fighter;
    r.outfit_row = row_name(snap.outfit_row);
    r.colour = shown_safe(snap.colour_row);
    r.jpeg = snap.jpeg;
    if (r.jpeg.empty()) notes.push_back("no picture: " + (snap.jpeg_note.empty() ? std::string("none taken") : cut_utf8(snap.jpeg_note, kNoteMaxBytes)));
    if (snap.fighter <= 0) notes.push_back("the character could not be read on the screen");

    bool have_outfits = false, have_registry = false, have_variants = false;
    const json outfits = read_json(root / "reframework/data/SF6_Costumes_Data/outfits.json", &have_outfits);
    const json registry = read_json(root / "reframework/data/SF6_Costumes_Data/registry.json", &have_registry);
    const json variants = read_json(root / "reframework/data/SF6_Costumes_Data/variants.json", &have_variants);
    if (!have_outfits) notes.push_back("outfits.json could not be read (no Slots Loader, or never run)");
    if (!have_registry) notes.push_back("registry.json could not be read");

    const json* outfit = have_outfits ? find_outfit_number(outfits, snap.fighter, snap.costume_no) : nullptr;
    if (!outfit && have_outfits) outfit = find_outfit(outfits, snap.fighter, r.outfit_row);
    int number = snap.costume_no;
    if (outfit && outfit->contains("costume_no") && (*outfit)["costume_no"].is_number_integer()) number = (*outfit)["costume_no"].get<int>();
    const json* slot = have_registry && outfit ? find_slot(registry, snap.fighter, number) : nullptr;
    r.costume_no = number;
    r.outfit = outfit ? clean_paths(str_of(*outfit, "label"), scrub) : r.outfit_row;
    if (r.outfit.empty()) r.outfit = r.outfit_row;
    r.costume_key = costume_key(snap.fighter, r.outfit_row, outfit, slot, have_outfits && snap.fighter > 0, number);

    if (outfit || slot) {
        json rec = json::object();
        if (outfit) { rec["outfit"] = *outfit; if (rec["outfit"].contains("source")) rec["outfit"]["source"] = clean_paths(str_of(*outfit, "source"), scrub); }
        if (slot) rec["slot"] = *slot;
        if (rec.dump(-1, ' ', false, json::error_handler_t::replace).size() > kRecordMaxBytes) { rec = json::object(); notes.push_back("the outfit's record was over its size cap"); }
        r.record = std::move(rec);
    } else if (have_outfits) {
        notes.push_back("no record: a game outfit, or the row names none");
    }

    for (const LogFile& l : kLogs) {
        std::string text;
        if (read_log(root / l.path, &text)) r.logs[l.key] = std::string(l.key) == "loader" ? cap_log(clean_log(text, scrub), log_keys_of(outfit, slot, scrub)) : cap_log(clean_log(text, scrub));
        else notes.push_back(std::string("the ") + l.key + " log could not be read");
    }
    r.versions = versions_of(snap, root, have_outfits ? outfits : json(nullptr), have_variants ? variants : json(nullptr));
    r.mods = mods_of(root, have_outfits ? outfits : json(nullptr), scrub, &notes);
    json n = json::array();
    for (const std::string& s : notes) n.push_back(s);
    r.notes = std::move(n);
    return r;
}

}  // namespace report
