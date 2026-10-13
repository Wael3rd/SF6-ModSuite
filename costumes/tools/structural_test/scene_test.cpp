// Byte-compares add_scene_folders against scenes produced by REasy-parser.
// Usage: scene_test <oracle folder holding in\ and expected\>
#include "structural_scene.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static bool read_file(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static int g_fail = 0;

static void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "OK  " : "FAIL", what.c_str());
    if (!ok) g_fail++;
}

static void report_diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t n = a.size() < b.size() ? a.size() : b.size(), i = 0;
    while (i < n && a[i] == b[i]) i++;
    std::printf("      sizes: got %zu, expected %zu; first difference at 0x%zX\n", a.size(), b.size(), i);
    size_t s = i > 8 ? i - 8 : 0;
    for (int k = 0; k < 2; k++) {
        const auto& v = k ? b : a;
        std::printf("      %s:", k ? "/expected/" : "got     ");
        for (size_t j = s; j < s + 24 && j < v.size(); j++) std::printf(" %02x", v[j]);
        std::printf("\n");
    }
}

struct Case {
    const char* file;
    bool prefer_v04;
    std::vector<std::string> fighters;   // empty = every fighter holding an esfNNNv00 folder
};

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: scene_test <oracle folder>\n"); return 2; }
    std::string root = argv[1];
    std::vector<Case> cases = {
        {"esf.scn.20", false, {}},
        {"ver_02_0300_esf.scn.20", true, {"esf013", "esf014", "esf019", "esf022", "esf025", "esf026",
                                          "esf027", "esf028", "esf029", "esf030", "esf031", "esf032"}},
        {"ver_02_0400_esf.scn.20", true, {"esf033"}},
        {"ver_02_0500_esf.scn.20", true, {"esf034"}},
    };
    for (auto& c : cases) {
        std::vector<uint8_t> in, want;
        if (!read_file(root + "/in/" + c.file, in) || !read_file(root + "/expected/" + c.file, want)) {
            check(false, std::string(c.file) + ": cannot read oracle files");
            continue;
        }
        auto before = scene_costume_folders(in);
        std::vector<std::string> fighters = c.fighters;
        if (fighters.empty())
            for (auto& kv : before) if (kv.second.count(0)) fighters.push_back(kv.first);

        std::string err;
        std::vector<uint8_t> got = in;
        bool ok = add_scene_folders(got, fighters, c.prefer_v04, 5, 105, &err);
        if (!ok) { check(false, std::string(c.file) + ": add_scene_folders failed: " + err); continue; }
        bool same = got == want;
        std::printf("%s  %s (%zu fighters, %zu -> %zu bytes)\n", same ? "SAME" : "DIFF", c.file,
                    fighters.size(), in.size(), got.size());
        if (!same) { g_fail++; report_diff(got, want); }

        // The listing must show v05..v104 for every added fighter.
        auto after = scene_costume_folders(want);
        bool all = true;
        for (auto& fd : fighters) {
            auto it = after.find(fd);
            if (it == after.end()) { all = false; break; }
            for (int k = 5; k < 105; k++) if (!it->second.count(k)) all = false;
        }
        check(all, std::string(c.file) + ": scene_costume_folders lists v05..v104 for every added fighter");
        check(scene_costume_folders(got) == after, std::string(c.file) + ": listing of our output == listing of expected");
    }

    // Garbage / truncated input: must return false, leave the data untouched, never crash.
    std::vector<uint8_t> good;
    if (read_file(root + "/in/ver_02_0500_esf.scn.20", good)) {
        std::vector<std::string> fd = {"esf034"};
        int bad = 0, tried = 0;
        auto try_one = [&](std::vector<uint8_t> v) {
            std::vector<uint8_t> copy = v;
            std::string err;
            tried++;
            if (add_scene_folders(v, fd, true, 5, 105, &err) || v != copy || err.empty()) bad++;
            scene_costume_folders(copy);
        };
        try_one({});
        try_one({'S', 'C', 'N', 0});
        try_one(std::vector<uint8_t>(500, 0xAB));
        for (size_t n = 0; n < good.size(); n += 7) try_one(std::vector<uint8_t>(good.begin(), good.begin() + n));
        std::vector<uint8_t> extra = good; extra.push_back(0);
        try_one(extra);
        std::printf("garbage/truncated: %d inputs, %d accepted or modified\n", tried, bad);
        check(bad == 0, "garbage / truncated / padded inputs: false, data untouched, err set, no crash");
        // Flipped header bytes: some still describe a valid scene (a name, a reserved field); only a crash matters.
        for (size_t o = 0; o < 140; o++) { auto v = good; v[o] ^= 0xFF; std::string e; add_scene_folders(v, fd, true, 5, 105, &e); }
        check(true, "140 flipped-byte variants of a real scene: no crash");
        std::vector<uint8_t> trunc(good.begin(), good.end() - 5);
        std::string err;
        bool r = add_scene_folders(trunc, fd, true, 5, 105, &err);
        check(!r && trunc.size() == good.size() - 5 && !err.empty(), "truncated scene: false, data untouched, err = \"" + err + "\"");
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "ALL OK\n", g_fail);
    return g_fail ? 1 : 0;
}
