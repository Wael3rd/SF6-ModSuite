// codecache_bench -- offline tests of mech/codecheck_cache.hpp (the code scan's per-game-version cache): round trip, key
// mismatch, missing target, index / RVA mismatch, multi-pass merge, truncated / garbage file. No game, no data.
//   codecache_bench      exit code 0 only when every check passed.
#include <cstdio>
#include <string>
#include <vector>

#include "mech/codecheck_cache.hpp"

namespace cc = codecache;
namespace {

int g_fail = 0, g_n = 0;
void check(bool ok, const char* what) {
    ++g_n;
    if (!ok) { ++g_fail; std::printf("FAIL: %s\n", what); }
}

cc::Entry named(const char* t, const char* s, uint32_t idx, long long raw, long long code, int shared = 0, std::vector<uint32_t> sh = {}) {
    cc::Entry e;
    e.id = cc::named_id(t, s);
    e.index = idx; e.raw = raw; e.code = code; e.shared = shared; e.sharers = std::move(sh);
    return e;
}
cc::Entry byva(unsigned long long va, int entering, std::vector<uint32_t> sh = {}) {
    cc::Entry e;
    e.named = false;
    e.id = cc::va_id(va);
    e.shared = entering; e.sharers = std::move(sh);
    return e;
}
bool same(const cc::Entry& a, const cc::Entry& b) {
    return a.id == b.id && a.named == b.named && a.index == b.index && a.raw == b.raw && a.code == b.code && a.shared == b.shared && a.sharers == b.sharers;
}
bool same(const std::vector<cc::Entry>& a, const std::vector<cc::Entry>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!same(a[i], b[i])) return false;
    return true;
}

const char* kKey = "exe 6A97881D 2618F000 25BB1081 size 632956832 time 1DD5AD806B56D43 methods 1455453";
const char* kH = "SF6_CRSMNative code scan cache 1\n";

}  // namespace

int main() {
    std::vector<cc::Entry> es = {
        named("app.UIPartsSpin", "Init(System.Int32, System.Action)", 186635, 52297600, 52297600),
        named("app.X", "Fold", 12, 1000, 3417251136, 2, { 7, 9 }),
        byva(0x1409A1230ull, 3, { 1, 2, 3 }),
        byva(0x1409A9990ull, 0),
        named("app.Missing", "Gone", 0, 0, 0),                                  // a target not found: all zero
    };
    const std::string text = cc::serialize(kKey, es);

    // round trip
    std::string k;
    std::vector<cc::Entry> back;
    check(cc::parse(text, k, back), "round trip parses");
    check(k == kKey, "round trip key");
    check(same(es, back), "round trip entries");
    std::vector<uint32_t> sixteen;
    for (uint32_t i = 0; i < 16; ++i) sixteen.push_back(1000000 + i);
    std::vector<cc::Entry> big = { named("t", "s", 4294967295u, 9000000000ll, 9000000001ll, 40, sixteen) };
    std::vector<cc::Entry> bback;
    check(cc::parse(cc::serialize(kKey, big), k, bback) && same(big, bback), "round trip 16 sharers, big values");
    check(cc::parse(cc::serialize(kKey, {}), k, bback) && bback.empty(), "round trip empty");

    // hit
    std::vector<cc::Entry> found;
    check(cc::decide(true, kKey, es, kKey, es, found) == cc::Miss::None && same(found, es), "hit, entries returned");
    std::vector<cc::Entry> sub = { es[3], es[1] };
    check(cc::decide(true, kKey, es, kKey, sub, found) == cc::Miss::None && found.size() == 2 && same(found[0], es[3]) && same(found[1], es[1]),
          "hit on a subset, in the caller's order");
    std::vector<cc::Entry> cur = es;                                            // shared / sharers of the current state are ignored
    cur[1].shared = 0; cur[1].sharers.clear(); cur[2].shared = 99;
    check(cc::decide(true, kKey, es, kKey, cur, found) == cc::Miss::None && found[1].shared == 2 && found[2].shared == 3, "scan results come from the file");

    // misses
    check(cc::decide(false, "", {}, kKey, es, found) == cc::Miss::NoFile && found.empty(), "no file");
    check(cc::decide(true, "exe 00000000", es, kKey, es, found) == cc::Miss::Key, "key mismatch");
    cur = es; cur.push_back(named("app.New", "M", 5, 1, 1));
    check(cc::decide(true, kKey, es, kKey, cur, found) == cc::Miss::Missing && found.empty(), "named target missing");
    cur = es; cur.push_back(byva(0x140000001ull, 0));
    check(cc::decide(true, kKey, es, kKey, cur, found) == cc::Miss::Missing, "VA target missing");
    cur = es; cur[1].index = 13;
    check(cc::decide(true, kKey, es, kKey, cur, found) == cc::Miss::Moved, "index mismatch");
    cur = es; cur[0].raw += 16;
    check(cc::decide(true, kKey, es, kKey, cur, found) == cc::Miss::Moved, "raw RVA mismatch");
    cur = es; cur[1].code += 16;
    check(cc::decide(true, kKey, es, kKey, cur, found) == cc::Miss::Moved, "code RVA mismatch");
    cur = es; cur[2].named = true;                                              // asked as named while cached by VA
    check(cc::decide(true, kKey, es, kKey, cur, found) == cc::Miss::Moved, "named vs VA kind mismatch");
    cur = es; cur[2].raw = 5; cur[2].index = 77;                                // by VA: index / RVAs are not compared
    check(cc::decide(true, kKey, es, kKey, cur, found) == cc::Miss::None, "by-VA targets ignore index / RVAs");

    // multi-pass merge
    std::vector<cc::Entry> pass1 = { es[0], es[2] }, pass2 = { es[1], es[3] };
    std::vector<cc::Entry> m1 = cc::merge(false, "", {}, kKey, pass1);
    check(same(m1, pass1), "merge: first pass alone");
    std::vector<cc::Entry> m2 = cc::merge(true, kKey, m1, kKey, pass2);
    check(m2.size() == 4 && same(m2[0], es[0]) && same(m2[1], es[2]) && same(m2[2], es[1]) && same(m2[3], es[3]), "merge: second pass added, first kept");
    check(cc::decide(true, kKey, m2, kKey, pass1, found) == cc::Miss::None && cc::decide(true, kKey, m2, kKey, pass2, found) == cc::Miss::None,
          "merged file serves both passes");
    std::vector<cc::Entry> p3 = { named("app.X", "Fold", 12, 1000, 3417251136, 5, { 1 }) };
    std::vector<cc::Entry> m3 = cc::merge(true, kKey, m2, kKey, p3);
    const cc::Entry* x = cc::find(m3, p3[0].id);
    check(m3.size() == 4 && x && x->shared == 5 && x->sharers.size() == 1 && x->sharers[0] == 1, "merge: a target of this pass replaces its old entry");
    check(cc::merge(true, "other key", m2, kKey, pass1).size() == 2, "merge: another key drops everything old");
    check(cc::merge(false, "", {}, kKey, { es[0], es[0] }).size() == 1, "merge: a duplicate id is kept once");
    check(cc::parse(cc::serialize(kKey, m3), k, back) && same(back, m3), "merged file round trips");

    // truncated / garbage: every prefix of a good file is a miss, never a partial hit
    bool prefixes_ok = true;
    for (size_t n = 0; n < text.size(); ++n) {
        std::string kk;
        std::vector<cc::Entry> bb;
        if (cc::parse(text.substr(0, n), kk, bb) || !bb.empty()) prefixes_ok = false;
    }
    check(prefixes_ok, "every truncation of a good file is rejected");
    const std::string h = kH;
    const std::vector<std::string> garbage = {
        "", "\n", "garbage\n", "binary \x01\x02\xff\xfe\n", h, h + "key \nend 0\n", "SF6_CRSMNative code scan cache 2\nkey k\nend 0\n",
        h + "key k\nt 1 1 2 3 0 0 -\tN:a|b\n",                       // no end
        h + "key k\nt 1 1 2 3 0 0 -\tN:a|b\nend 2\n",               // count mismatch
        h + "key k\nt 1 1 2 3 0 0 -\tN:a|b\nend 1\nextra\n",        // junk after end
        h + "key k\nt 1 1 2 3 0 0 -\n",                              // no id
        h + "key k\nt 1 1 2 3 0 2 5\tN:a|b\nend 1\n",               // 2 sharers announced, 1 given
        h + "key k\nt 1 1 2 3 0 1 5,6\tN:a|b\nend 1\n",             // 1 announced, 2 given
        h + "key k\nt 1 1 2 3 0 17 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17\tN:a|b\nend 1\n",
        h + "key k\nt 1 1 2 x 0 0 -\tN:a|b\nend 1\n",                // not a number
        h + "key k\nt 0 1 2 3 0 0 -\tN:a|b\nend 1\n",               // kind vs id
        h + "key k\nt 1 1 2 3 0 0 -\tQ:a|b\nend 1\n",               // bad id tag
        h + "key k\nt 1 1 2 3 -1 0 -\tN:a|b\nend 1\n",              // negative shared
    };
    bool bad_ok = true;
    for (const std::string& g : garbage) {
        std::string kk;
        std::vector<cc::Entry> bb;
        if (cc::parse(g, kk, bb) || !bb.empty()) { bad_ok = false; std::printf("accepted: %.60s\n", g.c_str()); }
    }
    check(bad_ok, "garbage files rejected");
    check(cc::parse(h + "key k\nt 1 1 2 3 0 2 5,6\tN:a|b\nend 1\n", k, back) && back.size() == 1 && back[0].sharers.size() == 2, "hand-written good file parses");
    check(cc::parse("SF6_CRSMNative code scan cache 1\r\nkey k\r\nt 1 1 2 3 0 0 -\tN:a|b\r\nend 1\r\n", k, back) && k == "k", "CRLF tolerated");

    std::printf("codecache_bench: %d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
