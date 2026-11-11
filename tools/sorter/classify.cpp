// SF6_ModSorter -- what a dropped item is and where it goes.
//
// The decision is the loaders' (probes.hpp): the costume loader says whether it makes outfits of it and for which fighters
// (from the files, never from a folder name), the stage loader whether it changes stages, holds a title screen or interface
// files, the Color Mod's file rule whether it holds colours. In this order, the first that takes it wins:
//   Costume      the costume loader finds costume files of a known outfit (whole outfits, or parts that become options)
//   Title pack   a natives tree with title screen files (title_slots::is_title_file), or a .pak holding some (by their hashes)
//   Stage        files of the stage index outside the menus' pictures (the stage loader makes variants of it)
//   Colours      colour files (esfFFF_CCC_cmd_KKK.user.2, not empty)
//   Interface    a natives tree with interface files (natives/stm/[streaming/]product/gui/): ui_mods serves any Fluffy option
//   Not recognised, with the reason, otherwise. Nothing is placed then.
//
// An archive is read the way the loaders read it, through common/archive.cpp: its names first (no data), then the entries whose
// bytes are read unpacked for real (modinfo.ini, .pak, nested archives, colour files, and the first natives file, which proves the
// archive can be unpacked), every other natives file as an empty file at its path (the loaders tell those apart by their paths
// alone). Nothing is ever run from an archive.

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "archive.hpp"
#include "colour_files.hpp"
#include "fighter_names.hpp"
#include "pak.hpp"
#include "probes.hpp"
#include "sorter.hpp"

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring from_utf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string to_acp(const std::wstring& w) {   // the loaders' narrow paths (their w2a)
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n - 1, 0);
    WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring lower_w(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring long_path_w(const std::wstring& p) {
    if (p.size() < 240 || p.rfind(L"\\\\?\\", 0) == 0 || p.size() < 3 || p[1] != L':') return p;
    std::wstring q = p;
    for (auto& c : q) if (c == L'/') c = L'\\';
    return L"\\\\?\\" + q;
}

bool file_exists_w(const std::wstring& p) {
    DWORD a = GetFileAttributesW(long_path_w(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists_w(const std::wstring& p) {
    DWORD a = GetFileAttributesW(long_path_w(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void delete_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(long_path_w(dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            const std::wstring full = dir + L"\\" + fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                delete_tree(full);
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(long_path_w(full).c_str());
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(long_path_w(full).c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(long_path_w(full).c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(long_path_w(dir).c_str());
}

static std::wstring leaf_of(const std::wstring& p) {
    std::wstring q = p;
    while (!q.empty() && (q.back() == L'\\' || q.back() == L'/')) q.pop_back();
    const size_t s = q.find_last_of(L"\\/");
    return s == std::wstring::npos ? q : q.substr(s + 1);
}

static std::wstring parent_of(const std::wstring& p) {
    std::wstring q = p;
    while (!q.empty() && (q.back() == L'\\' || q.back() == L'/')) q.pop_back();
    const size_t s = q.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : q.substr(0, s);
}

static std::wstring ext_of(const std::wstring& name) {
    const size_t d = name.find_last_of(L'.');
    return d == std::wstring::npos || d == 0 ? std::wstring() : lower_w(name.substr(d));
}

static std::wstring stem_of(const std::wstring& name) {
    const size_t d = name.find_last_of(L'.');
    return d == std::wstring::npos || d == 0 ? name : name.substr(0, d);
}

static bool make_dirs(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); ++i)
        if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(long_path_w(dir.substr(0, i)).c_str(), nullptr);
    return dir_exists_w(dir);
}

static std::string num(size_t n) { return std::to_string(n); }

const char* category_key(Category c) {
    switch (c) {
        case Category::Costume: return "costume";
        case Category::Stage: return "stage";
        case Category::UIPack: return "ui_pack";
        case Category::TitlePack: return "title_pack";
        case Category::Colours: return "colours";
        default: return "unknown";
    }
}

TextId category_text(Category c) {
    switch (c) {
        case Category::Costume: return T_CAT_COSTUME;
        case Category::Stage: return T_CAT_STAGE;
        case Category::UIPack: return T_CAT_UI;
        case Category::TitlePack: return T_CAT_TITLE;
        case Category::Colours: return T_CAT_COLOURS;
        default: return T_CAT_UNKNOWN;
    }
}

std::string fighter_folder(int fighter, const ClassifyContext& ctx) {
    std::string s = colour_files::character_folder(fighter);   // the costume_mods / color_mods list
    if (!s.empty()) return s;
    for (auto& [id, name] : ctx.game_names)                    // a fighter added by a game update: the loader's own naming
        if (id == fighter) {
            s = fighter_folder_name(name);
            if (!s.empty()) return s;
        }
    char b[16];
    snprintf(b, sizeof b, "esf%03d", fighter);
    return b;
}

// ---------------------------------------------------------------------------------------------
// Archives: names, then the entries whose bytes matter
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<std::wstring> g_real;          // sorted: entries unpacked for real
bool g_first_natives = false;              // the first natives entry is unpacked too (it proves the archive can be unpacked)

bool is_natives_entry(const std::wstring& low) {
    return low.rfind(L"natives/", 0) == 0 || low.find(L"/natives/") != std::wstring::npos;
}

bool keep_real(const std::wstring& entry) {
    if (std::binary_search(g_real.begin(), g_real.end(), entry)) return true;
    if (!g_first_natives && is_natives_entry(lower_w(entry))) { g_first_natives = true; return true; }
    return false;
}

struct Materialised {
    bool ok = false;
    Msg error;
    size_t entries = 0;
};

Materialised materialise(const std::wstring& archive, const std::wstring& dest) {
    Materialised m;
    std::vector<std::wstring> names;
    if (!archive_list(archive, names)) { m.error = {R_ARCHIVE_UNREADABLE, {}}; return m; }
    m.entries = names.size();
    g_real.clear();
    g_first_natives = false;
    std::vector<std::wstring> empty;
    for (auto& n : names) {
        const std::wstring low = lower_w(n);
        const std::wstring base = leaf_of(low);
        const std::wstring ext = ext_of(base);
        const bool real = base == L"modinfo.ini" || ext == L".pak" || ext == L".zip" || ext == L".7z" || ext == L".rar" ||
                          colour_files::is_colour_name(to_utf8(base));
        if (real) g_real.push_back(n);
        else if (is_natives_entry(low)) empty.push_back(n);
    }
    std::sort(g_real.begin(), g_real.end());
    if (!make_dirs(dest)) { m.error = {R_ARCHIVE_UNPACK, {"cannot create a scratch folder"}}; return m; }
    const ArchiveResult r = archive_extract(archive, dest, keep_real);
    if (!r.ok) { m.error = {R_ARCHIVE_UNPACK, {r.error}}; return m; }
    for (auto& n : empty) {
        std::wstring p = dest + L"\\" + n;
        for (auto& c : p) if (c == L'/') c = L'\\';
        if (file_exists_w(p)) continue;   // the first natives entry, unpacked
        make_dirs(parent_of(p));
        HANDLE h = CreateFileW(long_path_w(p).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    m.ok = true;
    return m;
}

// Colour files under a folder (the Color Mod's catalogue: the name rule, not empty; dot folders and files skipped)
struct ColourCount { std::map<std::pair<int, int>, int> per;   // (fighter, costume folder) -> colours
                     size_t files = 0, empty = 0, all_files = 0; };

void walk_colours(const std::wstring& dir, ColourCount& cc, int depth = 0) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(long_path_w(dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 24 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) walk_colours(full, cc, depth + 1);
            continue;
        }
        ++cc.all_files;
        const std::string n = to_utf8(fd.cFileName);
        if (!colour_files::is_colour_name(n)) continue;
        if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow == 0) { ++cc.empty; continue; }
        ++cc.files;
        cc.per[{atoi(n.substr(3, 3).c_str()), atoi(n.substr(7, 3).c_str())}]++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string fighter_list(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------------------------

void classify_init(ClassifyContext& ctx) {
    ctx.costume_ok = ctx.stage_ok = false;
    ctx.costume_note.clear();
    ctx.stage_note.clear();
    ctx.game_names.clear();
    if (ctx.game_dir.empty()) return;
    make_dirs(ctx.work_dir);
    ctx.costume_ok = probe::costume_init(ctx.game_dir, ctx.work_dir + L"\\cache", ctx.costume_note);
    ctx.stage_ok = probe::stage_init(ctx.game_dir, ctx.stage_note);
    for (auto& kv : game_fighter_names(to_acp(ctx.game_dir))) ctx.game_names.push_back(kv);
}

// ---------------------------------------------------------------------------------------------
// One item
// ---------------------------------------------------------------------------------------------

Classification classify(const std::wstring& path_in, const ClassifyContext& ctx, int seq) {
    Classification c;
    std::wstring path = path_in;
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    c.path = path;
    c.name = leaf_of(path);
    const DWORD attr = GetFileAttributesW(long_path_w(path).c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) { c.kind = ItemKind::Missing; c.reason = {R_NOT_FOUND, {}}; return c; }

    std::wstring root, pak_name, tree;
    int kind = -1;
    size_t files_inside = 0;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        c.kind = ItemKind::Folder;
        root = path;
        kind = 0;
    } else {
        const ArchiveKind ak = archive_sniff(long_path_w(path));
        const std::wstring ext = ext_of(c.name);
        if (ak != ArchiveKind::None) {
            c.kind = ItemKind::Archive;
            c.archive_kind = archive_kind_name(ak);
            tree = ctx.work_dir + L"\\item" + std::to_wstring(seq);
            delete_tree(tree);
            Materialised m = materialise(long_path_w(path), tree);
            if (!m.ok) { c.reason = m.error; delete_tree(tree); return c; }
            files_inside = m.entries;
            root = tree;
            kind = 1;
        } else if (ext == L".pak") {
            c.kind = ItemKind::Pak;
            PakReader pr;
            if (!pr.open(to_acp(path).c_str())) { c.reason = {R_PAK_UNREADABLE, {}}; return c; }
            files_inside = pr.entry_count();
            root = parent_of(path);
            pak_name = c.name;
            kind = 2;
        } else if (colour_files::is_colour_name(to_utf8(c.name))) {
            c.kind = ItemKind::ColourFile;
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            GetFileAttributesExW(long_path_w(path).c_str(), GetFileExInfoStandard, &fa);
            if (fa.nFileSizeHigh == 0 && fa.nFileSizeLow == 0) { c.reason = {R_EMPTY_COLOUR, {}}; return c; }
            const std::string n = to_utf8(c.name);
            const int f = atoi(n.substr(3, 3).c_str()), cf = atoi(n.substr(7, 3).c_str());
            c.cat = Category::Colours;
            c.fighters = {f};
            c.characters = {fighter_folder(f, ctx)};
            c.found.push_back({T_F_COLOUR_FILE, {}});
            c.found.push_back({T_F_COLOUR_1, {c.characters[0], probe::outfit_name(cf - 1)}});
            c.dest_dir = L"reframework\\color_mods\\" + from_utf8(c.characters[0]);
            c.dest_name = c.name;
            return c;
        } else {
            c.kind = ItemKind::Other;
            if (ext == L".lua") c.reason = {R_SCRIPT, {}};
            else if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".gif" || ext == L".webp" || ext == L".bmp" || ext == L".dds")
                c.reason = {R_IMAGE, {}};
            else if (ext == L".txt" || ext == L".md" || ext == L".ini" || ext == L".json" || ext == L".log" || ext == L".pdf")
                c.reason = {R_TEXT, {}};
            else c.reason = {R_OTHER_FILE, {ext.empty() ? std::string("?") : to_utf8(ext)}};
            return c;
        }
    }

    // ---- the loaders' view ----
    const std::string rel = "sorter/" + to_acp(c.name);
    probe::CostumeProbe cp;
    if (ctx.costume_ok) cp = probe::costume_probe(root, rel, kind, pak_name);
    c.loader_log = cp.log;
    const std::string stem_u = to_utf8(kind == 0 ? c.name : stem_of(c.name));
    probe::StageProbe sp = probe::stage_probe(root, kind == 2 ? rel : rel, stem_u, kind, pak_name);
    ColourCount cc;
    if (kind != 2) walk_colours(root, cc);
    if (kind == 0) files_inside = cc.all_files;
    if (!tree.empty()) delete_tree(tree);

    // what it is as a whole
    if (c.kind == ItemKind::Archive) c.found.push_back({files_inside == 1 ? T_F_ARCHIVE_1 : T_F_ARCHIVE, {c.archive_kind, num(files_inside)}});
    else if (c.kind == ItemKind::Folder) c.found.push_back({files_inside == 1 ? T_F_FOLDER_1 : T_F_FOLDER, {num(files_inside)}});
    else c.found.push_back({T_F_PAK, {num(files_inside)}});

    const std::wstring ext = ext_of(c.name);
    const bool named_archive = ext == L".zip" || ext == L".7z" || ext == L".rar";
    // a costume / colour archive must be named .zip / .7z / .rar (those loaders list archives by extension): the copy gets one
    const std::wstring archive_name = c.kind == ItemKind::Archive && !named_archive
                                      ? c.name + L"." + from_utf8(c.archive_kind) : c.name;

    // a colour pack's files are costume files too for the costume loader (parts of the outfit they colour): an item whose costume
    // files are all colour files, with no whole outfit, is the Color Mod's
    bool whole = false;
    for (auto& o : cp.outfits) whole = whole || o.full;
    const bool colour_pack = !whole && cp.files > 0 && cp.colour_files == cp.files;
    if ((!cp.outfits.empty() || cp.shared_only > 0) && !colour_pack) {
        // ---- Costume ----
        c.cat = Category::Costume;
        for (int f : cp.fighters) { c.fighters.push_back(f); c.characters.push_back(fighter_folder(f, ctx)); }
        std::map<std::tuple<int, int, bool>, int> groups;
        std::set<int> named;
        for (auto& o : cp.outfits) { groups[{o.fighter, o.costume_no, !o.full}]++; named.insert(o.fighter); }
        for (auto& [k, n] : groups) {
            const auto [f, no, parts] = k;
            const bool full = !parts;   // whole outfits listed before parts
            const std::string who = fighter_folder(f, ctx), what = probe::outfit_name(no);
            if (!full) c.found.push_back({T_F_PARTS, {who, what}});
            else if (n == 1) c.found.push_back({T_F_OUTFIT, {who, what}});
            else c.found.push_back({T_F_OUTFIT_N, {who, what, num((size_t)n)}});
        }
        for (int f : cp.fighters) if (!named.count(f)) c.found.push_back({T_F_SHARED, {fighter_folder(f, ctx)}});
        std::wstring sub;
        if (c.fighters.size() == 1) sub = L"\\" + from_utf8(c.characters[0]);
        else if (c.kind == ItemKind::Archive) c.found.push_back({T_F_MULTI_ARCHIVE, {}});
        else { sub = L"\\" + from_utf8(c.characters[0]); c.found.push_back({T_F_MULTI_FOLDER, {}}); }
        c.dest_dir = L"reframework\\costume_mods" + sub;
        c.dest_dir_nsfw = L"reframework\\costume_mods_nsfw" + sub;
        // A .pak goes in the character's folder as it is, like an archive. Not in a folder of its own: a mod alone in a
        // sub-folder is its own outfit for the loader and left out of its add-on rules (an accessory would never attach).
        c.dest_name = archive_name;
        c.nsfw_guess = cp.nsfw;
        if (cp.nsfw) {
            std::string why = cp.nsfw_why;   // "<rel> (<where> \"<name>\": \"<keyword>\"): an NSFW mod, ..."
            const size_t a = why.find(" ("), b = why.find("): ");
            if (a != std::string::npos && b != std::string::npos && b > a) why = why.substr(a + 2, b - a - 2);
            // The loader says where in English ("archive name"): the name and the keyword alone read in every language
            const size_t q = why.find('"');
            if (q != std::string::npos && q > 0) why = why.substr(q);
            c.nsfw_why = {T_NSFW_NAME, {why}};
        } else {
            c.nsfw_why = {T_NSFW_NONE, {}};
        }
        c.nsfw_options = cp.nsfw_options;
        if (!cp.nsfw && !cp.nsfw_options.empty())
            c.found.push_back({cp.nsfw_options.size() == 1 ? T_NSFW_OPTION_1 : T_NSFW_OPTIONS, {num(cp.nsfw_options.size())}});
        return c;
    }
    if (sp.title_files + sp.pak_title_files > 0) {
        // ---- Title pack ---- (a natives tree, or a .pak: alone, or in the archive or folder; the stage loader reads both)
        c.cat = Category::TitlePack;
        c.found.push_back({T_F_TITLE_FILES, {num(sp.title_files + sp.pak_title_files),
                                             num(sp.natives_files - sp.title_files + sp.pak_title_others)}});
        c.dest_dir = L"reframework\\title_mods";
        c.dest_name = c.name;
        return c;
    }
    if (!sp.variants.empty()) {
        // ---- Stage ----
        c.cat = Category::Stage;
        for (auto& v : sp.variants) c.found.push_back({v.tile ? T_F_STAGE_TILE : T_F_STAGE_VAR, {v.ess, v.option}});
        c.dest_dir = L"reframework\\stage_mods";
        c.dest_name = c.name;
        return c;
    }
    if (cc.files > 0) {
        // ---- Colours ----
        c.cat = Category::Colours;
        std::set<int> fs;
        for (auto& [k, n] : cc.per) fs.insert(k.first);
        for (int f : fs) { c.fighters.push_back(f); c.characters.push_back(fighter_folder(f, ctx)); }
        for (auto& [k, n] : cc.per)
            c.found.push_back({n == 1 ? T_F_COLOUR_1 : T_F_COLOURS, {fighter_folder(k.first, ctx), probe::outfit_name(k.second - 1), num((size_t)n)}});
        c.dest_dir = L"reframework\\color_mods\\" + (fs.size() == 1 ? from_utf8(c.characters[0]) : std::wstring(L"all_chars"));
        c.dest_name = archive_name;
        return c;
    }
    if (kind != 2 && sp.ui_files > 0) {
        // ---- Interface pack ----
        c.cat = Category::UIPack;
        c.found.push_back({sp.ui_files == 1 ? T_F_UI_FILE_1 : T_F_UI_FILES, {num(sp.ui_files)}});
        if (sp.ui_nodes > 1) c.found.push_back({T_F_UI_NODES, {num(sp.ui_nodes)}});
        if (sp.gui_files) c.found.push_back({T_F_GUI_ONLY, {num(sp.gui_files)}});
        c.dest_dir = L"reframework\\ui_mods";
        c.dest_name = c.name;
        return c;
    }

    // ---- Not recognised: why ----
    const bool costume_paths = cp.units > 0 || sp.natives_files > 0;
    const bool stage_paths = sp.first_other.find("/environment/stage/") != std::string::npos;
    if (kind == 2 || (sp.natives_files == 0 && sp.pak_entries > 0)) {
        // a pak's entries are hashes: only the loaders' indexes name them
        if (!ctx.costume_ok || !ctx.stage_ok) c.reason = {R_PAK_NO_INDEX, {!ctx.costume_ok ? ctx.costume_note : ctx.stage_note}};
        else c.reason = {R_PAK_UNKNOWN, {num(kind == 2 ? files_inside : sp.pak_entries)}};
    } else if (sp.natives_files == 0 && sp.options == 0 && cp.units == 0 && cc.empty == 0) {
        c.reason = {R_NOTHING_INSIDE, {}};
    } else if (cc.empty > 0 && sp.natives_files == 0) {
        c.reason = {R_EMPTY_COLOUR, {}};
    } else if (!ctx.costume_ok && costume_paths && cp.units == 0 && sp.first_other.find("/model/esf/") != std::string::npos) {
        c.reason = {R_NO_COSTUME_INDEX, {ctx.costume_note}};
    } else if (cp.loaded > 0 || sp.first_other.find("/product/model/esf/") != std::string::npos) {
        // costume files the loader read, but for no outfit it knows (a model folder no costume uses, a partial with no target)
        std::string ex = sp.first_other.empty() ? std::string("natives/stm/product/model/esf/...") : sp.first_other;
        c.reason = {R_COSTUME_PARTS_UNKNOWN, {ex}};
    } else if (!ctx.stage_ok && stage_paths) {
        c.reason = {R_NO_STAGE_INDEX, {ctx.stage_note}};
    } else {
        c.reason = {R_OTHER_FILES, {sp.first_other.empty() ? std::string("?") : sp.first_other}};
    }
    if (ctx.game_dir.empty() && (c.reason.id == R_NO_COSTUME_INDEX || c.reason.id == R_NO_STAGE_INDEX || c.reason.id == R_PAK_NO_INDEX))
        c.reason = {R_NO_GAME, {}};
    if (sp.gui_files) c.found.push_back({T_F_GUI_ONLY, {num(sp.gui_files)}});
    return c;
}

// ---------------------------------------------------------------------------------------------
// One JSON line (--classify)
// ---------------------------------------------------------------------------------------------

static std::string jstr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
        else if (ch == '\n') o += "\\n";
        else if (ch == '\r') o += "\\r";
        else if (ch == '\t') o += "\\t";
        else if (ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
        else o += (char)ch;
    }
    return o + "\"";
}

static const char* kind_key(ItemKind k) {
    switch (k) {
        case ItemKind::Folder: return "folder";
        case ItemKind::Archive: return "archive";
        case ItemKind::Pak: return "pak";
        case ItemKind::ColourFile: return "colour_file";
        case ItemKind::Other: return "other";
        default: return "missing";
    }
}

std::string classification_json(const Classification& c, const ClassifyContext&, bool verbose) {
    std::string j = "{";
    j += "\"path\": " + jstr(to_utf8(c.path));
    j += ", \"kind\": " + jstr(kind_key(c.kind));
    j += ", \"category\": " + jstr(category_key(c.cat));
    j += ", \"character\": " + jstr(fighter_list(c.characters));
    j += ", \"fighters\": [";
    for (size_t i = 0; i < c.fighters.size(); ++i) j += (i ? ", " : "") + std::to_string(c.fighters[i]);
    j += "]";
    std::string reason;
    if (c.cat == Category::Unknown) reason = tr_en(c.reason);
    else for (size_t i = 1; i < c.found.size(); ++i) reason += (reason.empty() ? "" : "; ") + tr_en(c.found[i]);
    j += ", \"reason\": " + jstr(reason);
    j += ", \"destination\": " + jstr(c.cat == Category::Unknown ? std::string() : to_utf8(destination_rel(c, false)));
    if (c.cat == Category::Costume) j += ", \"destination_nsfw\": " + jstr(to_utf8(destination_rel(c, true)));
    j += std::string(", \"nsfw_guess\": ") + (c.nsfw_guess ? "true" : "false");
    if (c.cat == Category::Costume) j += ", \"nsfw_reason\": " + jstr(tr_en(c.nsfw_why));
    if (!c.nsfw_options.empty()) {
        j += ", \"nsfw_options\": [";
        for (size_t i = 0; i < c.nsfw_options.size(); ++i) j += (i ? ", " : "") + jstr(c.nsfw_options[i]);
        j += "]";
    }
    j += ", \"found\": [";
    for (size_t i = 0; i < c.found.size(); ++i) j += (i ? ", " : "") + jstr(tr_en(c.found[i]));
    j += "]";
    if (verbose) j += ", \"loader_log\": " + jstr(c.loader_log);
    j += "}";
    return j;
}
