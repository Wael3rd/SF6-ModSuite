// Colour files catalogue: the RW / MC entries of every fighter / costume, and the edited copies (overrides).
// Port of SF6_ColorSpinExtra.lua build_catalog / entries_for / label_to_k / override_* (lines ~260-385): same file
// name rule, same order, same labels, same override keys, so both builds see the same entries and files.
// Paths are kept exactly as the Lua side stores them (state.json "path", override keys): relative to
// reframework/data, backslash-separated. Checked offline by tools/rsz_harness/run.ps1 (catalogue section).
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "rsz.hpp"

namespace engine {

struct Entry {
    int n = 0;                 // colour number in the file name (cmd_NNN)
    std::string path;          // "SF6_ColorSpinExtra_data\\colors\\...\\esfFFF_CCC_cmd_NNN.user.2"
    int src = 0;               // 0 = SF6_ColorSpinExtra_data\colors, 1 = reframework\color_mods (the editor never writes there)
    bool manual = false;      // saved by the editor (a MANUALCOLORS folder): labelled MC, after the packs
    std::string ident;         // stable identity: path, with the archive cache hash left out (what state.json and the overrides key on)
    std::string okey;          // override key "<fighter>_<costume>_file_<sanitised path>"
    int k = 0;                 // 1-based rank in its list (the "entry number" the menus use)
    std::string label;         // "RW n" / "MC n"
};

class Catalog {
public:
    // data_dir = the game's reframework/data folder (absolute or relative to the working directory).
    void build(const std::filesystem::path& data_dir);
    const std::vector<Entry>* entries(int fighter, int costume) const;   // null when none
    int label_to_k(int fighter, int costume, const std::string& label) const;   // 0 = not one of ours
    const std::map<std::string, std::vector<Entry>>& all() const { return lists_; }

    // file tree of an entry: its override when one exists (unless raw), else the file itself; cached
    rsz::NodeRef entry_tree(const Entry& e, bool raw = false);
    rsz::NodeRef override_tree(const std::string& key);        // empty / missing override file = none
    bool set_override(const std::string& key, const rsz::NodeRef& tree);   // null tree = clear (empty file)

    static std::string ident_of(const std::string& rel);       // see Entry::ident
    int find_ident(int fighter, int costume, const std::string& ident) const;   // rank k of that colour, 0 = gone
    static std::string sanitize(const std::string& path);      // every non-alphanumeric byte -> '_'

private:
    std::filesystem::path data_dir_;
    std::map<std::string, std::vector<Entry>> lists_;          // "<fighter>_<costume>"
    std::map<std::string, rsz::NodeRef> trees_;                // path -> parsed (null = unreadable)
    std::map<std::string, rsz::NodeRef> overrides_;            // key -> tree (null = none)
    std::map<std::string, bool> override_known_;
    rsz::NodeRef read_tree(const std::string& rel_path);
};

}  // namespace engine
