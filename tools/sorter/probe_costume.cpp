// SF6_ModSorter -- the costume probe. This file compiles the Slots Loader's costume code whole (loader_core.cpp, included
// below: a unity build) so that the sorter decides what a costume mod is with the very functions the loader runs at game
// start: collect_mod_units (what a folder or an archive holds: natives trees, paks, nested archives, Fluffy menus),
// load_unit (scan_folder_mod / scan_mod_pak, detect_mod_costumes: which fighter, which outfit, a whole outfit or parts) and
// nsfw_detect (the NSFW keywords on the mod's name). Nothing of loader_core.cpp is copied or changed, and nothing here writes
// into the game folder: costume_loader_run, log_open and ensure_costume_mods_dirs are never called, and nested archives are
// unpacked into the sorter's own cache folder.
//
// The loader decides a costume's fighter from its content (natives/stm/product/model/esf/esfNNN/FFF/...), never from the
// <Character> folder it sits in: an archive holding outfits of several fighters gives each of them its outfits.

#include "loader_core.cpp"

#include "colour_files.hpp"
#include "probes.hpp"

bool g_is_exe = false;   // loader_core's logf (its printf): no console output; its log file is opened per probe instead

namespace {

std::unique_ptr<VanillaIndex> g_sorter_inv;
std::wstring g_capture;   // where a probe's printf lines go

std::string read_all_text(const std::wstring& p) {
    std::string s;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// The loader's state of one scan, emptied before each probe (scan_costume_mods does the same before a scan)
void reset_scan_state() {
    g_units.clear();
    g_menus.clear();
    g_pending_addons.clear();
    g_merge_audit.clear();
    g_unit_source = 0;
    g_nsfw_ids.clear();
    g_nsfw_kw_mods = g_nsfw_kw_options = 0;
    g_mod_names.clear();
    g_pak_ident.clear();
}

}  // namespace

namespace probe {

bool costume_init(const std::wstring& game_dir, const std::wstring& cache_dir, std::string& note) {
    g_sorter_inv.reset();
    const std::string gd = w2a(game_dir.c_str());
    const std::string tsv = gd + "\\reframework\\data\\SF6_Costumes_Data\\loader\\vanilla_costume_index.tsv";
    auto inv = std::make_unique<VanillaIndex>();
    if (!inv->load(tsv.c_str())) {
        note = "reframework\\data\\SF6_Costumes_Data\\loader\\vanilla_costume_index.tsv";   // the missing file (the texts say so)
        return false;
    }
    // Costumes a game update added since the index was made: inventoried from the game's paks, as the loader does at launch
    {
        PakReader base;
        if (base.open((gd + "\\re_chunk_000.pak").c_str())) {
            std::vector<std::unique_ptr<PakReader>> dlc;
            open_dlc_paks(gd, base, dlc);
            std::vector<int> fighters;
            for (auto& kv : game_fighter_names(gd)) fighters.push_back(kv.first);
            if (!fighters.empty()) complete_vanilla_index(*inv, base, fighters);
        }
    }
    // The loader's switches that change what a scan finds (costume_loader_run reads them the same way)
    const std::string registry_dir = gd + "\\reframework\\data\\SF6_Costumes_Data";
    g_manager_mode = detect_manager_mode(gd, registry_dir);
    g_swap_repairs = g_manager_mode && GetFileAttributesA((registry_dir + "\\swap_repairs.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (g_manager_mode) {
        g_nsfw_keywords.clear();
    } else {
        bool set = false;
        const std::string v = read_setting(gd + "\\reframework\\costume_mods\\settings.ini", "nsfwkeywords", &set);
        g_nsfw_keywords = set ? nsfw::parse_keywords(v) : nsfw::default_keywords();
    }
    g_archive_cache_root = w2a(cache_dir.c_str());
    g_archive_cache_used.clear();
    CreateDirectoryW(cache_dir.c_str(), nullptr);
    g_capture = cache_dir + L"\\costume_probe.log";
    g_sorter_inv = std::move(inv);
    char buf[160];
    snprintf(buf, sizeof buf, "%zu costumes in the index%s", g_sorter_inv->costumes.size(),
             g_manager_mode ? ", Slotsloader Manager mode (no NSFW keywords)" : "");
    note = buf;
    return true;
}

bool costume_ready() { return g_sorter_inv != nullptr; }

std::string outfit_name(int costume_no) { return game_outfit_name(costume_no); }

CostumeProbe costume_probe(const std::wstring& root, const std::string& rel, int kind, const std::wstring& pak_name) {
    CostumeProbe r;
    if (!g_sorter_inv) return r;
    const VanillaIndex& inv = *g_sorter_inv;
    reset_scan_state();

    // the loader's log lines of this item (its printf is its logf: they go to its log file), kept for the details and the tests
    g_log = _wfopen(g_capture.c_str(), L"w");
    const bool captured = g_log != nullptr;

    const std::string dir = w2a(root.c_str());
    std::vector<ModUnit> units;
    if (kind == 0) collect_mod_units(dir, rel, 0, 0, units);          // a folder, as scan_one_mod_folder reads it
    else if (kind == 1) collect_mod_units(dir, rel, 0, 1, units);     // an archive, as scan_mods_root reads it once unpacked
    else units.push_back({dir, w2a(pak_name.c_str()), rel});            // a .pak
    mark_nsfw(units, false);
    for (auto& u : units) if (!u.menu) ++r.units;

    std::vector<std::pair<std::string, std::vector<ModCostume>>> mods_out;
    std::unordered_map<std::string, ModFileRef> all_mod_out;
    std::vector<std::unique_ptr<PakReader>> readers;
    std::vector<FolderModInfo> infos;
    std::unordered_map<uint64_t, std::string> upgraded;
    ScanOutputs o{ inv, mods_out, all_mod_out, readers, infos, upgraded };
    process_units(units, rel, o);

    for (auto& uf : g_units) {
        if (uf.files.empty()) continue;
        ++r.loaded;
        for (auto& [p, mf] : uf.files) {
            ++r.files;
            const size_t sl = p.find_last_of('/');
            if (colour_files::is_colour_name(sl == std::string::npos ? p : p.substr(sl + 1))) ++r.colour_files;
        }
        std::string opt = uf.u.rel;
        if (opt.size() > rel.size() && opt.compare(0, rel.size(), rel) == 0 && opt[rel.size()] == '/') opt = opt.substr(rel.size() + 1);
        else if (opt == rel) opt.clear();
        std::set<std::string> whole;
        for (auto& mc : uf.costumes) {
            r.outfits.push_back({mc.fighter, mc.original_costume_no, game_outfit_name(mc.original_costume_no), opt, true});
            r.fighters.insert(mc.fighter);
            whole.insert(mc.fighter_dir + "/" + mc.original_folder);
        }
        for (auto& t : uf.targets) {
            if (whole.count(t)) continue;
            const size_t sl = t.find('/');
            const std::string fd = t.substr(0, sl), folder = t.substr(sl + 1);
            auto it = inv.folder_to_costume.find(fd + "\t" + folder);
            if (it == inv.folder_to_costume.end()) continue;   // a model folder of no outfit: nothing the loader can show
            const CostumeInfo& ci = inv.costumes[it->second];
            bool seen = false;
            for (auto& x : r.outfits) seen = seen || (x.fighter == ci.fighter && x.costume_no == ci.costume_no && x.option == opt);
            if (!seen) r.outfits.push_back({ci.fighter, ci.costume_no, game_outfit_name(ci.costume_no), opt, false});
            r.fighters.insert(ci.fighter);
        }
        if (uf.costumes.empty() && uf.targets.empty()) {
            // files of the shared folders (000) only: parts every outfit of the fighter may load
            bool any = false;
            for (auto& [p, mf] : uf.files) {
                std::string fd, folder;
                if (!parse_model_folder(p, fd, folder)) continue;
                for (auto& c : inv.costumes)
                    if (c.fighter_dir == fd) { r.fighters.insert(c.fighter); any = true; break; }
            }
            if (any) ++r.shared_only;
        }
        if (!uf.kw.empty()) r.nsfw_options.push_back((opt.empty() ? uf.name : opt) + ": " + uf.kw);
        if (uf.u.nsfw) r.nsfw = true;
    }

    if (!captured) r.log = "(the loader's lines could not be captured)";
    if (captured) {
        log_close();
        r.log = read_all_text(g_capture);
        if (r.log.empty()) r.log = "(no line from the loader in " + w2a(g_capture.c_str()) + ")";
        if (r.nsfw) {
            // nsfw_detect's own line says which name and which keyword
            const size_t at = r.log.find("  nsfw: ");
            if (at != std::string::npos) {
                const size_t e = r.log.find('\n', at);
                r.nsfw_why = r.log.substr(at + 8, (e == std::string::npos ? r.log.size() : e) - at - 8);
            }
        }
    }
    reset_scan_state();   // its units point into the paks read: emptied before the readers go
    readers.clear();
    return r;
}

}  // namespace probe
