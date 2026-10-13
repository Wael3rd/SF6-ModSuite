// Offline check of add_costume_records / costume_table_records against make_static_structural.py's output.
// Usage: table_test <oracle dir>   (<oracle>/in/fightercostumeuserdata.user.2 = the game's table,
//                                   <oracle>/expected/fightercostumeuserdata.user.2 = the Python tool's)
#include "patch.hpp"
#include <cstdio>
#include <set>
#include <string>
#include <vector>

static std::vector<uint8_t> read_all(const std::string& p) {
    std::vector<uint8_t> v;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    v.resize((size_t)n);
    if (n > 0 && fread(v.data(), 1, (size_t)n, f) != (size_t)n) v.clear();
    fclose(f);
    return v;
}

static std::string roman_guid(int so) {
    char b[64];
    snprintf(b, sizeof b, "a1b2c3d4-%04x-4000-8000-%012x", so + 1, 5000 + so);
    return b;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: table_test <oracle dir>\n"); return 2; }
    const std::string dir = argv[1];
    auto game = read_all(dir + "/in/fightercostumeuserdata.user.2");
    auto want = read_all(dir + "/expected/fightercostumeuserdata.user.2");
    if (game.empty() || want.empty()) { printf("missing oracle files\n"); return 2; }

    std::vector<std::pair<int, int>> recs;
    if (!costume_table_records(game, recs)) { printf("FAIL costume_table_records\n"); return 1; }
    std::set<int> fighters;
    for (auto& r : recs) fighters.insert(r.first);
    printf("game table: %zu records, %zu fighters\n", recs.size(), fighters.size());

    std::vector<std::vector<uint8_t>> guids(100);
    for (int so = 0; so < 100; so++) guid_bytes(roman_guid(so), guids[so]);
    std::vector<CostumeSlotRecord> add;
    for (int f : fighters) {
        int idx = costume_slot_index(f);
        for (int so = 0; so < 100; so++)
            add.push_back({ (uint32_t)(5000 + idx * 100 + so), (uint32_t)f, (uint32_t)(5 + so),
                            (uint32_t)(101 + idx * 100 + so), (uint32_t)so });
    }
    auto out = game;
    std::string err;
    if (!add_costume_records(out, add, 5000, guids, &err)) { printf("FAIL add_costume_records: %s\n", err.c_str()); return 1; }
    if (out == want) printf("SAME fightercostumeuserdata.user.2 (%zu bytes)\n", out.size());
    else {
        size_t i = 0;
        while (i < out.size() && i < want.size() && out[i] == want[i]) i++;
        printf("DIFF at 0x%zx (sizes %zu / %zu)\n", i, out.size(), want.size());
        return 1;
    }
    // A table that already has slot records, and garbage, are refused
    auto again = out;
    if (add_costume_records(again, add, 5000, guids, &err) || again != out) { printf("FAIL: second add accepted\n"); return 1; }
    std::vector<uint8_t> junk(game.begin(), game.begin() + game.size() / 2);
    if (add_costume_records(junk, add, 5000, guids, &err)) { printf("FAIL: truncated table accepted\n"); return 1; }
    printf("refusals OK\n");
    return 0;
}
