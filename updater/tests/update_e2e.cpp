// update_e2e - the updater's own steps (update_install.cpp) on a fake game folder, outside the game. Not shipped.
//
//   update_e2e.exe update --game <fake game folder> --manifest <the new release's manifest.json> --reports <a manifest whose versions the
//                  installed DLLs "report"> --assets <folder holding the new release's zips>
//       The check (installed packages against the manifest), the install from local copies of the zips (sizes and sha256 checked, every
//       entry checked, Smart App Control on every program, the swap), then the next start (journal, clean-up of the old copies) and a
//       second check that must offer nothing. The versions come from --reports because the DLLs of a release made before the updater
//       carry no sf6suite_package_version export (in game the loaded DLLs answer).
//   update_e2e.exe fetch --url <https address> --size <bytes> --sha256 <hex> --out <file>
//       One real download through the updater's HTTPS code (redirects checked against its host list), then its size and sha256.
//   update_e2e.exe manifest
//       The latest release's manifest.json, as the game asks for it (prints the outcome).
//   update_e2e.exe versions --game <folder holding packages' DLLs in reframework/plugins>
//       Each main DLL there loaded into this process, then asked its version as the game's host asks it (update_game.cpp): the
//       sf6suite_package_version export, the Native Core's get_version for a core without it. Prints what each one reports.
//   Exit 0: all as expected.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../src/update_fs.hpp"
#include "../src/update_game.hpp"
#include "../src/update_install.hpp"
#include "../src/update_logic.hpp"
#include "../src/update_net.hpp"
#include "../src/update_pe.hpp"

using namespace sf6up;

static std::map<std::string, std::string> args(int argc, char** argv) {
    std::map<std::string, std::string> a;
    for (int i = 2; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    return a;
}

static void out(const std::string& s) { printf("%s\n", s.c_str()); fflush(stdout); }

static bool read_manifest(const std::string& path, Manifest* m) {
    std::string text, why;
    if (!read_file(widen(path), &text, kManifestMaxBytes + 1) || !parse_manifest(text, m, &why)) { out("cannot read " + path + ": " + why); return false; }
    return true;
}

static int cmd_update(std::map<std::string, std::string> a) {
    const Paths p(widen(a["--game"]));
    std::string text;
    if (!read_file(widen(a["--manifest"]), &text, kManifestMaxBytes + 1)) { out("no manifest"); return 1; }
    Manifest reports;
    if (!read_manifest(a["--reports"], &reports)) return 1;
    auto reader_of = [](const Manifest& m) {
        return [m](const KnownPackage& k) {
            for (auto& pk : m.packages)
                if (pk.id == k.id) return pk.version;
            return std::string();
        };
    };
    auto log = [](const std::string& l) { out("  log: " + l); };
    recover(p, log);
    const State st = load_state(p);
    CheckResult c = run_check(p, reader_of(reports), text, st.ignored);
    if (!c.ok) { out("check refused the manifest: " + c.why); return 1; }
    for (auto& n : c.plan.notes) out("  note: " + n);
    out("offered by " + c.plan.suite + ": " + std::to_string(c.plan.items.size()) + " package(s)");
    for (auto& it : c.plan.items) out("  " + it.pkg.name + " " + it.from + " -> " + it.pkg.version + " (" + it.pkg.asset + ")");
    const std::wstring assets = widen(a["--assets"]);
    const Fetch local = [&](const PlanItem& it, const std::wstring& dest, const BytesFn& bytes, std::string* why) {
        const std::wstring src = join(assets, it.pkg.asset);
        if (!CopyFileW(src.c_str(), dest.c_str(), FALSE)) { *why = "no local copy of " + it.pkg.asset; return false; }
        if (bytes) bytes(it.pkg.size);
        return true;
    };
    Progress progress;
    const InstallResult r = run_install(p, c.plan, local, log, -1, &progress);
    out("progress at the end: step " + std::to_string(progress.step.load()) + ", " + std::to_string(progress.done.load()) + " of " +
        std::to_string(progress.total.load()) + " bytes, " + std::to_string(progress.percent()) + " %");
    if (r.ok && (progress.percent() != 100 || progress.step.load() != Progress::Install)) { out("the progress did not reach its end"); return 1; }
    out(std::string("install: ") + (r.ok ? "ok" : "FAILED: " + r.why));
    if (!r.ok) return 1;
    // the zips kept for Fluffy Mod Manager's players: every one of the update, as the release says
    for (auto& it : c.plan.items) {
        std::string why;
        if (!verify_file(join(p.downloads, it.pkg.asset), it.pkg.size, it.pkg.sha256, &why)) { out("not kept in downloads: " + it.pkg.asset + ": " + why); return 1; }
    }
    out("zips kept in downloads: " + std::to_string(c.plan.items.size()) + ", the newest " + narrow(newest_download(p)).substr(narrow(p.downloads).size() + 1));
    if (exists(p.staging)) { out("the staging folder is still there"); return 1; }
    out("old copies kept: " + std::to_string(r.swap.backups.size()));
    for (auto& b : r.swap.backups) out("  " + b);
    // the next start: the new DLLs report the new versions
    Manifest now;
    parse_manifest(text, &now, nullptr);
    recover(p, log);
    cleanup(p, log);
    const State after = load_state(p);
    out("pending clean-up after the next start: " + std::to_string(after.cleanup.size()));
    CheckResult again = run_check(p, reader_of(now), text, after.ignored);
    out("offered at the next start: " + std::to_string(again.plan.items.size()));
    return c.plan.items.empty() || !again.plan.items.empty() || !after.cleanup.empty() ? 1 : 0;
}

static int cmd_fetch(std::map<std::string, std::string> a) {
    const std::wstring dest = widen(a["--out"]);
    DeleteFileW(dest.c_str());
    const int64_t size = _strtoi64(a["--size"].c_str(), nullptr, 10);
    const ULONGLONG t0 = GetTickCount64();
    int64_t seen = 0, calls = 0;
    const HttpResult h = http_get_file(a["--url"], size, dest, [&](int64_t n) { seen = n; ++calls; });
    out("progress: " + std::to_string(calls) + " call(s), the last at " + std::to_string(seen) + " of " + std::to_string(size) + " bytes");
    if (h.ok && seen != size) { out("the progress did not reach the size"); return 1; }
    out("fetch " + a["--url"] + ": " + (h.ok ? "200" : "FAILED: " + h.why) + ", last host " + h.final_host + ", " + std::to_string(GetTickCount64() - t0) + " ms");
    if (!h.ok) return 1;
    std::string why;
    const bool ok = verify_file(dest, size, a["--sha256"], &why);
    out(std::string("size and sha256: ") + (ok ? "as the GitHub API says" : "MISMATCH: " + why));
    // the guards: a host outside the list, plain http, never reached
    std::string body;
    const HttpResult other = http_get("https://example.com/", 1000, &body);
    const HttpResult plain = http_get("http://github.com/", 1000, &body);
    out("another host refused: " + std::string(!other.ok ? "yes (" + other.why + ")" : "NO"));
    out("plain http refused: " + std::string(!plain.ok ? "yes (" + plain.why + ")" : "NO"));
    return ok && !other.ok && !plain.ok ? 0 : 1;
}

static int cmd_manifest() {
    std::string text, why;
    const bool ok = fetch_manifest(&text, &why);
    out(std::string("latest release's manifest.json: ") + (ok ? "found, " + std::to_string(text.size()) + " bytes" : "not found: " + why));
    if (ok) {
        Manifest m;
        out(parse_manifest(text, &m, &why) ? "  valid, suite " + m.suite : "  refused: " + why);
    }
    return 0;
}

static int cmd_versions(std::map<std::string, std::string> a) {
    const std::wstring root = widen(a["--game"]);
    int bad = 0;
    for (auto& k : known_packages()) {
        const std::wstring dll = join(root, k.proof);
        if (!exists(dll)) continue;
        SetLastError(0);
        HMODULE h = LoadLibraryW(dll.c_str());
        if (!h) { out(std::string(k.id) + ": not loaded (error " + std::to_string(GetLastError()) + ")"); continue; }
        const std::string v = game::package_version(k, root);
        out(std::string(k.id) + ": " + (v.empty() ? "reports no version" : "reports " + v));
        const std::string f = file_package_version(dll);            // what the mod sorter reads, the game closed: the same
        out(std::string(k.id) + ": its file says " + (f.empty() ? "nothing" : f) + (f == v ? "" : " (NOT THE SAME)"));
        if (f != v) bad = 1;
    }
    return bad;
}

int main(int argc, char** argv) {
    if (argc < 2) { out("usage: update_e2e.exe update|fetch|manifest ..."); return 2; }
    const std::string c = argv[1];
    if (c == "update") return cmd_update(args(argc, argv));
    if (c == "fetch") return cmd_fetch(args(argc, argv));
    if (c == "manifest") return cmd_manifest();
    if (c == "versions") return cmd_versions(args(argc, argv));
    out("unknown command");
    return 2;
}
