// SF6_ModSorter -- the stage probe, with the title packs' and the UI packs' part. This file compiles the stage loader whole
// (stage_loader.cpp, included below: a unity build) so that the sorter reads a mod with the stage loader's own functions:
// collect_options (what Fluffy would install: one option per folder holding a natives tree or a .pak; UI packs: every folder
// with a modinfo.ini too), the stage index (load_stage_index: which game files belong to which stage), the GUI rule (menu
// pictures are not stage files), classify_stage_variant (tile or variation), title_slots::is_title_file and
// ui_slots::fighter_of_path. Nothing of stage_loader.cpp is copied or changed; stage_loader_run and slog_open are never called
// (they write into the game folder).
//
// One difference, said here: the stage loader reads a .pak's whole data (it copies it into its own pak); the probe reads its
// table of contents only (kpka_index, the loader's own reader), which is all the classification uses.

#include "stage_loader.cpp"

#include "probes.hpp"

#include <unordered_set>

namespace {

struct StageIndex {
    std::unordered_map<uint64_t, std::string> code, gui, paths;
};
std::unique_ptr<StageIndex> g_sorter_stage;

const char* const kStreamGuiPrefix = "natives/stm/streaming/product/gui/";

bool is_gui_path(const std::string& low) {
    return low.compare(0, strlen(GUI_PREFIX), GUI_PREFIX) == 0 || low.compare(0, strlen(kStreamGuiPrefix), kStreamGuiPrefix) == 0;
}

// One .pak as an option: its entries' hashes from its table of contents (kpka_index, the stage loader's reader)
bool pak_option(const std::wstring& file, const std::string& rel, const std::string& name, std::vector<Option>& out, size_t& entries) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, lp(file).c_str(), L"rb") != 0 || !f) return false;
    std::vector<KpkaEntry> idx;
    const bool ok = kpka_index(f, idx);
    fclose(f);
    if (!ok) return false;
    Option opt;
    opt.rel = rel;
    opt.name = name;
    for (auto& k : idx) {
        OptFile of;
        of.hash = k.hash;
        of.attrib = k.attrib;
        of.dsize = k.dsize;
        opt.files.push_back(std::move(of));
    }
    entries += idx.size();
    out.push_back(std::move(opt));
    return true;
}

// Every .pak under dir (a folder the stage loader would read), as options of their own: the loader makes the folder holding
// a .pak an option; the probe lists the pak's entries instead of reading their data.
void collect_paks(const std::wstring& dir, const std::string& rel, int depth, std::vector<Option>& out, size_t& entries) {
    for (auto& e : list_dir(dir)) {
        const std::wstring full = dir + L"\\" + e.name;
        if (e.dir) {
            if (e.name[0] == L'.' || depth >= 6) continue;
            collect_paks(full, rel + "/" + narrow(e.name), depth + 1, out, entries);
            continue;
        }
        if (ends_with(e.name, L".pak")) pak_option(full, rel + "/" + narrow(e.name), narrow(stem(e.name)), out, entries);
    }
}

}  // namespace

namespace probe {

bool stage_init(const std::wstring& game_dir, std::string& note) {
    g_sorter_stage.reset();
    auto s = std::make_unique<StageIndex>();
    const std::wstring path = game_dir + L"\\reframework\\data\\SF6_StageSlots_Data\\loader\\stage_paths.txt";
    if (!load_stage_index(path, s->code, s->gui, s->paths)) {
        note = "reframework\\data\\SF6_StageSlots_Data\\loader\\stage_paths.txt";   // the missing file (the texts say so)
        return false;
    }
    char buf[96];
    snprintf(buf, sizeof buf, "%zu stage files in the index", s->code.size());
    note = buf;
    g_sorter_stage = std::move(s);
    return true;
}

bool stage_ready() { return g_sorter_stage != nullptr; }

StageProbe stage_probe(const std::wstring& root, const std::string& rel, const std::string& name, int kind,
                       const std::wstring& pak_name) {
    StageProbe r;
    std::vector<Option> options;     // as the stage loader's run() collects them (natives listed, not read)
    std::vector<Option> paks;
    if (kind == 2) {
        // a .pak dropped as it is: one option (a source of its own for the stage loader)
        pak_option(root + L"\\" + pak_name, rel, name, paks, r.pak_entries);
    } else {
        collect_options(root, rel, name, true, 0, options, false);
        collect_paks(root, rel, 0, paks, r.pak_entries);
    }
    if (!options.empty()) r.option_name = options[0].name;
    else if (!paks.empty()) r.option_name = paks[0].name;

    // natives files: title files, UI files, the rest
    for (auto& o : options)
        for (auto& f : o.files) {
            ++r.natives_files;
            const std::string low = lower_a(f.path);
            if (title_slots::is_title_file(low)) ++r.title_files;
            if (is_gui_path(low)) {
                ++r.ui_files;
                const std::string fid = ui_slots::fighter_of_path(low);
                if (!fid.empty()) r.ui_fighters.insert(fid);
            } else if (r.first_other.empty() && !(g_sorter_stage && g_sorter_stage->code.count(f.hash))) {
                r.first_other = f.path;
            }
        }
    // title files in its paks: a pak keeps hashes, the title files' paths are known (title_slots::known_paths, as the stage loader
    // names a title pack's pak entries)
    {
        static const std::unordered_set<uint64_t> title_hashes = [] {
            std::unordered_set<uint64_t> h;
            for (auto& p : title_slots::known_paths()) if (title_slots::is_title_file(p)) h.insert(pak_path_hash(std::string_view(p)));
            return h;
        }();
        for (auto& o : paks) {
            size_t t = 0;
            for (auto& f : o.files) t += title_hashes.count(f.hash);
            if (!t) continue;
            r.pak_title_files += t;
            r.pak_title_others += o.files.size() - t;
        }
    }
    // UI pack nodes, as scan_ui collects them (a folder with a modinfo.ini is a node too)
    if (kind != 2) {
        std::vector<Option> nodes;
        collect_options(root, rel, name, true, 0, nodes, false, true);
        r.ui_nodes = nodes.size();
    }

    if (!g_sorter_stage) return r;
    const StageIndex& ix = *g_sorter_stage;
    std::vector<Option*> all;
    for (auto& o : options) all.push_back(&o);
    for (auto& o : paks) all.push_back(&o);
    r.options = all.size();
    // One variant per option and per stage it changes: run()'s split of an option's files
    for (Option* opt : all) {
        std::map<std::string, std::vector<size_t>> by_ess, gui_by_ess;
        std::vector<size_t> shared, gui_shared;
        for (size_t i = 0; i < opt->files.size(); ++i) {
            const OptFile& f = opt->files[i];
            auto it = ix.code.find(f.hash);
            const bool gui = lower_a(f.path).compare(0, strlen(GUI_PREFIX), GUI_PREFIX) == 0 || ix.gui.count(f.hash);
            if (it != ix.code.end()) (gui ? gui_by_ess : by_ess)[it->second].push_back(i);
            else (gui ? gui_shared : shared).push_back(i);
        }
        if (by_ess.empty()) {
            for (auto& [e, v] : gui_by_ess) r.gui_files += v.size();
            continue;
        }
        for (auto& [ess, idx] : by_ess) {
            std::vector<std::string> kind_paths;
            for (size_t i : idx) {
                const OptFile& f = opt->files[i];
                auto ip = ix.paths.find(f.hash);
                if (!f.path.empty()) kind_paths.push_back(lower_a(f.path));
                else if (ip != ix.paths.end()) kind_paths.push_back(ip->second);
            }
            for (size_t i : shared) {
                const OptFile& f = opt->files[i];
                if (!f.path.empty()) kind_paths.push_back(lower_a(f.path));
            }
            const StageKind k = classify_stage_variant(ess, kind_paths, shared.size());
            StageVariantInfo v;
            v.ess = ess;
            v.option = by_ess.size() > 1 ? opt->name + " (" + ess + ")" : opt->name;
            v.tile = k.tile;
            v.reason = k.reason;
            v.files = idx.size() + shared.size();
            r.variants.push_back(v);
        }
    }
    return r;
}

}  // namespace probe
