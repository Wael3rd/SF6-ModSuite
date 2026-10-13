// Offline check of vanilla_inventory.cpp against costumes/tools/build_vanilla_inventory.py + make_vanilla_index.py.
// Usage: inventory_test <game dir> <vanilla_costume_index.tsv>
//   Inventories every costume the TSV lists from the game's paks with the loader's C++ code and compares the
//   #COSTUMES row and the #FILES rows (same rows, same order) with the TSV's. It also checks that the costumes
//   the C++ finds by probing the paks are exactly the TSV's, and that the version suffixes it discovers
//   (starting from the Python tool's known ones only) are the TSV's #SUFFIXES.
//   inventory_test <game dir> <tsv> <fighter> <costume_no> prints the rows the C++ code writes for one costume.
//   --python-priority reads a file that is in a DLC pak and in re_chunk_000.pak from the DLC pak, as the Python tool
//   does (its dict of hashes keeps the last pak). The loader reads re_chunk_000.pak first; four costumes (esf007,
//   008, 012, 018 v00) have a color variation list (ccvd) in a DLC pak that differs from the base pak's, and the
//   TSV holds the DLC one's rows.
#include "vanilla_inventory.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

static std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> v;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return v;
    std::string cur;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == '\n') { if (!cur.empty() && cur.back() == '\r') cur.pop_back(); v.push_back(cur); cur.clear(); }
        else cur += (char)c;
    }
    if (!cur.empty()) v.push_back(cur);
    fclose(f);
    return v;
}

static std::vector<std::string> split_tabs(const std::string& s) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find('\t', a);
        out.push_back(s.substr(a, b == std::string::npos ? b : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

int main(int argc, char** argv) {
    // --python-priority: a file in a DLC pak wins over re_chunk_000.pak (the Python tool's dict of hashes, last pak
    // wins). Without it the loader's order is used: re_chunk_000.pak first, the DLC paks read under it.
    // --heuristic: also inventories every costume with the fallback (strings that look like paths, no layout) and
    // reports, per costume, the rows it misses and the rows it adds compared with the layout-driven result.
    bool python_priority = false, heuristic = false;
    for (int i = 1; i < argc;) {
        std::string a = argv[i];
        if (a == "--python-priority" || a == "--heuristic") {
            (a == "--heuristic" ? heuristic : python_priority) = true;
            for (int j = i; j + 1 < argc; j++) argv[j] = argv[j + 1];
            argc--;
        } else i++;
    }
    if (argc < 3) { printf("usage: inventory_test [--python-priority] [--heuristic] <game dir> <vanilla_costume_index.tsv> [<fighter> <costume_no>]\n"); return 2; }
    const std::string game = argv[1];

    // TSV: costume rows and file rows per (fighter, costume_no), in file order
    std::vector<std::string> lines = read_lines(argv[2]);
    if (lines.empty()) { printf("cannot read %s\n", argv[2]); return 2; }
    std::vector<std::pair<int, int>> order;                           // costumes in TSV order
    std::map<std::pair<int, int>, std::string> costume_row;
    std::map<std::pair<int, int>, std::vector<std::string>> file_rows;
    std::map<std::string, std::string> tsv_suffixes;
    int section = 0;
    for (auto& l : lines) {
        if (l.empty()) continue;
        if (l[0] == '#') { section = l == "#SUFFIXES" ? 1 : l == "#COSTUMES" ? 2 : l == "#FILES" ? 3 : 0; continue; }
        auto c = split_tabs(l);
        if (section == 1 && c.size() >= 2) tsv_suffixes[c[0]] = c[1];
        else if (section == 2 && c.size() >= 5) {
            std::pair<int, int> k{ atoi(c[0].c_str()), atoi(c[2].c_str()) };
            order.push_back(k);
            costume_row[k] = l;
        } else if (section == 3 && c.size() >= 5) {
            file_rows[{ atoi(c[1].c_str()), atoi(c[2].c_str()) }].push_back(l);
        }
    }
    printf("TSV: %zu costumes, %zu suffixes\n", order.size(), tsv_suffixes.size());

    // Game paks, as the loader opens them: re_chunk_000.pak with the DLC paks read over it
    PakReader base;
    if (!base.open((game + "\\re_chunk_000.pak").c_str())) { printf("cannot open re_chunk_000.pak\n"); return 2; }
    std::vector<std::string> dlc;
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA((game + "\\dlc\\*.pak").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do dlc.push_back(game + "\\dlc\\" + fd.cFileName); while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    std::sort(dlc.begin(), dlc.end());
    std::vector<std::unique_ptr<PakReader>> keep;
    for (auto& p : dlc) {
        auto r = std::make_unique<PakReader>();
        if (r->open(p.c_str())) keep.push_back(std::move(r));
    }
    PakReader* top = &base;
    if (python_priority && !keep.empty()) {
        top = keep.back().get();                       // last DLC pak first, then the others, then re_chunk_000.pak
        for (size_t i = keep.size() - 1; i-- > 0;) top->add_overlay(keep[i].get());
        top->add_overlay(&base);
    } else {
        for (auto& r : keep) base.add_overlay(r.get());
    }
    printf("pak priority: %s\n", python_priority ? "DLC paks over re_chunk_000.pak (the Python tool's)" : "re_chunk_000.pak first (the loader's)");

    // Costumes the paks have, fighters 1-49 as the Python tool probes them
    SuffixMap suffixes = vanilla_known_suffixes();
    std::vector<std::pair<int, int>> found;
    for (int f = 1; f < 50; f++)
        for (int no : vanilla_costume_numbers(*top, f, suffixes)) found.push_back({ f, no });
    printf("probing: %zu costumes found, TSV lists %zu: %s\n", found.size(), order.size(), found == order ? "SAME" : "DIFFERENT");

    if (heuristic) {
        size_t miss_total = 0, extra_total = 0, rows_total = 0;
        int clean = 0, with_miss = 0, with_extra = 0, shown = 0;
        std::map<std::string, int> miss_kind, extra_kind;
        for (auto& k : order) {
            VanillaCostume typed, heur;
            std::string err;
            vanilla_force_heuristic(false);
            bool ok1 = vanilla_costume_inventory(*top, k.first, k.second, suffixes, typed, &err);
            vanilla_force_heuristic(true);
            SuffixMap sfx2 = suffixes;
            bool ok2 = vanilla_costume_inventory(*top, k.first, k.second, sfx2, heur, &err);
            vanilla_force_heuristic(false);
            if (!ok1 || !ok2) { printf("%s: inventory failed (%s)\n", vanilla_costume_label(k.first, k.second).c_str(), err.c_str()); continue; }
            std::set<std::string> a, b;
            for (auto& r : vanilla_file_rows(typed)) a.insert(r);
            for (auto& r : vanilla_file_rows(heur)) b.insert(r);
            std::vector<std::string> miss, extra;
            for (auto& r : a) if (!b.count(r)) miss.push_back(r);
            for (auto& r : b) if (!a.count(r)) extra.push_back(r);
            bool row_same = vanilla_costume_row(typed) == vanilla_costume_row(heur);
            rows_total += a.size();
            miss_total += miss.size();
            extra_total += extra.size();
            with_miss += !miss.empty();
            with_extra += !extra.empty();
            for (auto& r : miss) miss_kind[split_tabs(r)[3]]++;
            for (auto& r : extra) extra_kind[split_tabs(r)[3]]++;
            if (miss.empty() && extra.empty() && row_same) { clean++; continue; }
            printf("%s: typed %zu rows, fallback %zu, missing %zu, extra %zu%s\n", vanilla_costume_label(k.first, k.second).c_str(),
                   a.size(), b.size(), miss.size(), extra.size(), row_same ? "" : ", #COSTUMES row differs");
            if (shown++ < 6) {
                for (size_t i = 0; i < miss.size() && i < 2; i++) printf("      missing: %s\n", miss[i].c_str());
                for (size_t i = 0; i < extra.size() && i < 2; i++) printf("      extra:   %s\n", extra[i].c_str());
                if (!row_same) printf("      C++ typed: %s\n      fallback:  %s\n", vanilla_costume_row(typed).c_str(), vanilla_costume_row(heur).c_str());
            }
        }
        printf("fallback vs typed over %zu costumes (%zu typed rows): %d identical, %d with missing rows (%zu in all), %d with extra rows (%zu in all)\n",
               order.size(), rows_total, clean, with_miss, miss_total, with_extra, extra_total);
        for (auto& kv : miss_kind) printf("  missing kind %s: %d\n", kv.first.c_str(), kv.second);
        for (auto& kv : extra_kind) printf("  extra kind %s: %d\n", kv.first.c_str(), kv.second);
        return 0;
    }

    if (argc >= 5) {
        VanillaCostume vc;
        std::string err;
        if (!vanilla_costume_inventory(*top, atoi(argv[3]), atoi(argv[4]), suffixes, vc, &err)) { puts(("failed: " + err).c_str()); return 1; }
        puts(vanilla_costume_row(vc).c_str());
        for (auto& r : vanilla_file_rows(vc)) puts(r.c_str());
        return 0;
    }

    int same = 0, diff = 0, shown = 0;
    size_t files_checked = 0;
    auto t0 = std::chrono::high_resolution_clock::now();
    for (auto& k : order) {
        VanillaCostume vc;
        std::string err;
        std::vector<std::string> mine;
        std::string my_costume;
        bool ok = vanilla_costume_inventory(*top, k.first, k.second, suffixes, vc, &err);
        if (ok) { my_costume = vanilla_costume_row(vc); mine = vanilla_file_rows(vc); }
        const auto& want = file_rows[k];
        files_checked += want.size();
        std::string why;
        if (!ok) why = "inventory failed: " + err;
        else if (my_costume != costume_row[k]) why = "#COSTUMES row\n      C++: " + my_costume + "\n      TSV: " + costume_row[k];
        else if (mine != want) {
            size_t i = 0;
            while (i < mine.size() && i < want.size() && mine[i] == want[i]) i++;
            why = "#FILES rows: C++ " + std::to_string(mine.size()) + ", TSV " + std::to_string(want.size()) + ", first difference at row " + std::to_string(i)
                + "\n      C++: " + (i < mine.size() ? mine[i] : "(none)") + "\n      TSV: " + (i < want.size() ? want[i] : "(none)");
        }
        if (why.empty()) same++;
        else {
            diff++;
            if (shown++ < 8) printf("DIFF %s: %s\n", vanilla_costume_label(k.first, k.second).c_str(), why.c_str());
        }
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("costumes: %d SAME, %d DIFF (%zu file rows compared) in %.0f ms, %.1f ms per costume\n",
           same, diff, files_checked, ms, order.empty() ? 0.0 : ms / (double)order.size());

    // Suffixes discovered along the way vs #SUFFIXES
    std::map<std::string, std::string> mine_sfx(suffixes.begin(), suffixes.end());
    int sfx_diff = 0;
    for (auto& kv : tsv_suffixes) {
        auto m = mine_sfx.find(kv.first);
        if (m == mine_sfx.end() || m->second != kv.second) { sfx_diff++; printf("SUFFIX %s: TSV %s, C++ %s\n", kv.first.c_str(), kv.second.c_str(), m == mine_sfx.end() ? "(none)" : m->second.c_str()); }
    }
    for (auto& kv : mine_sfx)
        if (!tsv_suffixes.count(kv.first)) { sfx_diff++; printf("SUFFIX %s: C++ %s, not in the TSV\n", kv.first.c_str(), kv.second.c_str()); }
    printf("suffixes: %s (%zu in the TSV, %zu in C++)\n", sfx_diff ? "DIFFERENT" : "SAME", tsv_suffixes.size(), mine_sfx.size());

    return (diff == 0 && sfx_diff == 0 && found == order) ? 0 : 1;
}
