// Checks add_msg_entries() against a file produced by the Python reference (REasy-parser).
// Usage: msg_test <oracle folder>   (in\fightercostumemessage.msg.21, expected\fightercostumemessage.msg.21)
#include "structural_msg.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static std::string roman(int v) {
    static const int val[] = {100, 90, 50, 40, 10, 9, 5, 4, 1};
    static const char* sym[] = {"C", "XC", "L", "XL", "X", "IX", "V", "IV", "I"};
    std::string r;
    for (int i = 0; i < 9; ++i) while (v >= val[i]) { r += sym[i]; v -= val[i]; }
    return r;
}

static std::vector<MsgEntry> make_entries() {
    std::vector<MsgEntry> v;
    for (int so = 0; so < 100; ++so) {
        char g[64];
        std::snprintf(g, sizeof g, "a1b2c3d4-%04x-4000-8000-%012x", so + 1, 5000 + so);
        v.push_back({g, "FighterCostumeMessage" + std::to_string(6 + so), "Outfit " + roman(so + 1)});
    }
    return v;
}

static int fails = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: msg_test <oracle folder>\n"); return 2; }
    std::string dir = argv[1];
    std::vector<uint8_t> in, exp;
    if (!read_file(dir + "\\in\\fightercostumemessage.msg.21", in) ||
        !read_file(dir + "\\expected\\fightercostumemessage.msg.21", exp)) {
        std::printf("cannot read oracle files\n"); return 2;
    }

    // 1. Byte comparison with the reference.
    std::vector<uint8_t> out = in;
    std::string err;
    if (!add_msg_entries(out, make_entries(), &err)) { std::printf("DIFF: add_msg_entries failed: %s\n", err.c_str()); return 1; }
    size_t n = std::min(out.size(), exp.size()), i = 0;
    while (i < n && out[i] == exp[i]) ++i;
    if (i == n && out.size() == exp.size()) {
        std::printf("SAME (%zu bytes)\n", out.size());
    } else {
        uint64_t pool = *reinterpret_cast<const uint64_t*>(exp.data() + 32);
        uint32_t cnt = *reinterpret_cast<const uint32_t*>(exp.data() + 16);
        uint32_t lang = *reinterpret_cast<const uint32_t*>(exp.data() + 24);
        uint64_t table_end = 72 + 8ull * cnt;
        uint64_t entries_start = *reinterpret_cast<const uint64_t*>(exp.data() + 56);
        const char* where = i < 72 ? "header" : i < pool ? (i < entries_start ? "entry table / metadata" : "entry records") : "string block";
        (void)table_end; (void)lang;
        std::printf("DIFF: sizes %zu vs expected %zu, first difference at offset %zu (%s)\n",
                    out.size(), exp.size(), i, where);
        size_t a = i > 8 ? i - 8 : 0;
        std::printf("  got     :"); for (size_t k = a; k < a + 24 && k < out.size(); ++k) std::printf(" %02x", out[k]);
        std::printf("\n  expected:"); for (size_t k = a; k < a + 24 && k < exp.size(); ++k) std::printf(" %02x", exp[k]);
        std::printf("\n");
        ++fails;
    }

    // 2. Robustness: bad input must fail cleanly and leave the data alone.
    std::printf("robustness\n");
    {
        std::vector<uint8_t> t;
        std::string e;
        check(!add_msg_entries(t, make_entries(), &e), "empty input -> false");
        for (size_t len : {size_t(10), size_t(71), size_t(72), size_t(300), in.size() / 2, in.size() - 1}) {
            t.assign(in.begin(), in.begin() + static_cast<ptrdiff_t>(len));
            std::vector<uint8_t> before = t;
            bool ok = add_msg_entries(t, make_entries(), &e);
            char buf[64]; std::snprintf(buf, sizeof buf, "truncated to %zu bytes -> false, untouched", len);
            check(!ok && t == before, buf);
        }
        // Garbage: right size, wrong everything.
        t.assign(in.size(), 0xA5);
        std::vector<uint8_t> before = t;
        check(!add_msg_entries(t, make_entries(), &e) && t == before, "garbage input -> false, untouched");
        // Valid header, corrupted pointers.
        t = in;
        for (int k = 0; k < 8; ++k) t[72 + k] = 0xFF;   // first entry offset
        before = t;
        check(!add_msg_entries(t, make_entries(), &e) && t == before, "bad entry offset -> false, untouched");
        t = in;
        t[32] = 0xFF; t[33] = 0xFF;                      // pool offset past the file
        before = t;
        check(!add_msg_entries(t, make_entries(), &e) && t == before, "bad pool offset -> false, untouched");
        // Bad entry data.
        t = in; before = t;
        std::vector<MsgEntry> bad = {{"not-a-guid", "X", "Y"}};
        check(!add_msg_entries(t, bad, &e) && t == before, "bad GUID -> false, untouched");
        bad = {{"a1b2c3d4-0001-4000-8000-000000005000", "X", "\xC3"}};
        check(!add_msg_entries(t, bad, &e) && t == before, "bad UTF-8 -> false, untouched");
    }

    // 3. Zero entries: what does the rebuild do to the original?
    {
        std::vector<uint8_t> t = in;
        std::string e;
        bool ok = add_msg_entries(t, {}, &e);
        std::printf("zero entries: %s, %zu -> %zu bytes, %s\n", ok ? "true" : "false", in.size(), t.size(),
                    t == in ? "identical to input" : "differs from input");
        check(ok, "zero entries -> true");
    }

    // 4. Idempotence on the expected file: re-adding nothing keeps it as is.
    {
        std::vector<uint8_t> t = exp;
        std::string e;
        bool ok = add_msg_entries(t, {}, &e);
        check(ok && t == exp, "zero entries on the expected file -> unchanged");
    }

    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
