// SF6 Mod Suite updater: the steps, without the game (update_install.hpp).
#include "update_install.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <ctime>
#include <mutex>
#include <set>

#include "update_net.hpp"
#include "update_sys.hpp"

namespace sf6up {

Paths::Paths(const std::wstring& game_root) : root(game_root) {
    data = join(root, kDataDir);
    state = join(data, "state.json");
    journal = join(data, "swap_journal.json");
    staging = join(data, "staging");
    log = join(data, "update.log");
    downloads = join(data, "downloads");
    previous = join(downloads, "previous_version");
}

namespace {
std::mutex g_state_mx;
}

void append_log(const Paths& p, const std::string& line) {
    static std::mutex mx;
    static bool started = false;
    std::lock_guard lk(mx);
    if (!started) {
        started = true;
        if (file_size(p.log) > 256 * 1024) DeleteFileW(p.log.c_str());
        make_dirs(p.data);
    }
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    std::strftime(stamp, sizeof stamp, "[%Y-%m-%d %H:%M:%S] ", &tm);
    // shared for writing: the game and the mod sorter may both be there, each line is one append
    HANDLE h = CreateFileW(p.log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const std::string text = stamp + line + "\r\n";
    DWORD put = 0;
    WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr);
    CloseHandle(h);
}

State load_state(const Paths& p) {
    std::lock_guard lk(g_state_mx);
    std::string text;
    read_file(p.state, &text, 256 * 1024);
    return parse_state(text);
}

bool update_state(const Paths& p, const std::function<void(State&)>& change) {
    std::lock_guard lk(g_state_mx);
    std::string text;
    read_file(p.state, &text, 256 * 1024);
    State s = parse_state(text);
    change(s);
    return write_file(p.state, state_json(s));
}

void recover(const Paths& p, const LogFn& log) {
    std::string l;
    if (recover_journal(p.root, p.journal, &l) && log) log(l);
    if (exists(p.staging)) remove_tree(p.staging);     // an install that did not finish: its downloads
}

void cleanup(const Paths& p, const LogFn& log) {
    State s = load_state(p);
    if (s.cleanup.empty()) return;
    std::string l;
    const std::vector<std::string> left = cleanup_backups(p.root, s.cleanup, &l);
    std::set<std::string> done(s.cleanup.begin(), s.cleanup.end());
    for (auto& x : left) done.erase(x);
    update_state(p, [&](State& st) {                    // only what it took care of: an install may have added some since
        std::vector<std::string> keep;
        for (auto& x : st.cleanup)
            if (!done.count(x)) keep.push_back(x);
        st.cleanup = keep;
    });
    if (log) log("clean-up of the last update: " + l);
}

CheckResult run_check(const Paths& p, const VersionReader& version_of, const std::string& manifest_text, const std::string& ignored) {
    CheckResult r;
    Manifest m;
    if (!parse_manifest(manifest_text, &m, &r.why)) return r;
    r.ok = true;
    const std::vector<Installed> installed = detect_installed(p.root, version_of);
    r.plan = make_plan(m, installed, manager_mode(p.root), ignored);
    for (auto& i : installed)
        if (i.present) r.plan.notes.push_back(i.id + " installed, reports " + (i.version.empty() ? std::string("no version") : i.version));
    return r;
}

int Progress::percent() const {
    const int64_t t = total.load(), d = done.load();
    if (t <= 0 || d <= 0) return 0;
    return d >= t ? 100 : (int)(d * 100 / t);
}

InstallResult run_install(const Paths& p, const Plan& plan, const Fetch& fetch, const LogFn& log, int fail_at, Progress* progress) {
    InstallResult r;
    auto fail = [&](Fail f, const std::string& why) { r.fail = f; r.why = why; if (log) log("install failed: " + why); remove_tree(p.staging); return r; };
    if (plan.items.empty()) return fail(Fail::Package, "nothing to install");
    remove_tree(p.staging);
    const std::wstring zips = join(p.staging, "zips"), tree = join(p.staging, "tree");
    if (!make_dirs(zips)) return fail(Fail::Files, "the staging folder cannot be made");
    // a: the zips, each one as the release says
    int64_t before = 0, all = 0;                    // bytes of the zips already there, of every zip
    for (auto& it : plan.items) all += it.pkg.size;
    if (progress) { progress->done.store(0); progress->total.store(all); progress->step.store(Progress::Download); }
    for (auto& it : plan.items) {
        const std::wstring dest = join(zips, it.pkg.asset);
        std::string why;
        const int64_t size = it.pkg.size;
        const BytesFn bytes = [progress, before, size](int64_t n) {
            if (progress) progress->done.store(before + (n < 0 ? 0 : n > size ? size : n));
        };
        if (!fetch(it, dest, bytes, &why)) return fail(Fail::Download, it.pkg.asset + ": " + why);
        if (!verify_file(dest, it.pkg.size, it.pkg.sha256, &why)) return fail(Fail::Check, it.pkg.asset + ": " + why);
        before += size;
        if (progress) progress->done.store(before);
        if (log) log(it.pkg.asset + ": downloaded, size and sha256 as the release says");
    }
    if (progress) progress->step.store(Progress::Install);
    // b: their files into the staging tree, every entry checked first
    Staged staged;
    for (auto& it : plan.items) {
        std::string why;
        if (!extract_zip(join(zips, it.pkg.asset), tree, it.pkg.id, it.from, &staged, &why)) return fail(Fail::Package, it.pkg.asset + ": " + why);
    }
    std::set<std::string> seen;
    for (auto& f : staged.files)
        if (!seen.insert(lower(f.rel)).second) return fail(Fail::Package, "two packages ship " + f.rel);
    // c: Smart App Control, on every program, before anything changes
    for (auto& f : staged.files) {
        if (!is_program(f.rel)) continue;
        const SacResult s = sac_check(join(tree, f.rel));
        if (s.verdict == Sac::Blocked) {
            r.what = f.rel.substr(f.rel.find_last_of('/') + 1);
            return fail(Fail::Blocked, f.rel + ": blocked by Smart App Control (error 4551)");
        }
        if (s.verdict != Sac::Ok) return fail(Fail::Package, f.rel + ": not a program Windows loads (error " + std::to_string(s.error) + ")");
    }
    if (log) log(std::to_string(staged.files.size()) + " files staged, programs accepted by Windows");
    // d: the swap, all or nothing
    r.swap = swap_in(p.root, tree, staged, p.journal, fail_at);
    if (!r.swap.ok) return fail(Fail::Files, r.swap.why);
    update_state(p, [&](State& st) {
        for (auto& b : r.swap.backups) st.cleanup.push_back(b);
    });
    std::vector<std::string> assets;
    for (auto& it : plan.items) assets.push_back(it.pkg.asset);
    keep_downloads(p, assets, zips, log);
    remove_tree(p.staging);
    if (log)
        log("installed: " + std::to_string(r.swap.replaced) + " replaced, " + std::to_string(r.swap.added) + " added, " + std::to_string(r.swap.same) +
            " unchanged, " + std::to_string(r.swap.kept) + " kept (the player's), " + std::to_string(r.swap.dirs) + " folders made");
    r.ok = true;
    return r;
}

namespace {
// The zips of a folder (names), not its subfolders
std::vector<std::string> zips_in(const std::wstring& dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.zip").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(narrow(fd.cFileName));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}
}  // namespace

int keep_downloads(const Paths& p, const std::vector<std::string>& assets, const std::wstring& from, const LogFn& log) {
    int kept = 0;
    for (auto& asset : assets) {
        const std::string fam = asset_family(asset);
        const std::wstring src = join(from, asset);
        if (fam.empty() || !exists(src)) continue;
        if (!make_dirs(p.downloads)) { if (log) log("downloads: the folder cannot be made"); return kept; }
        bool moved_ok = true;
        for (auto& n : zips_in(p.downloads)) {
            if (asset_family(n) != fam) continue;
            const std::wstring there = join(p.downloads, n);
            if (lower(n) == lower(asset)) { DeleteFileW(there.c_str()); continue; }   // the same zip again: the new copy replaces it
            // the one before: previous_version/, in place of an older one of the same package
            if (!make_dirs(p.previous)) { moved_ok = false; break; }
            for (auto& o : zips_in(p.previous))
                if (asset_family(o) == fam) DeleteFileW(join(p.previous, o).c_str());
            if (!MoveFileExW(there.c_str(), join(p.previous, n).c_str(), MOVEFILE_REPLACE_EXISTING)) moved_ok = false;
            else if (log) log("downloads: " + n + " moved to previous_version");
        }
        if (!moved_ok && log) log("downloads: an older zip of " + asset + " could not be moved to previous_version");
        if (MoveFileExW(src.c_str(), join(p.downloads, asset).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            ++kept;
            if (log) log("downloads: " + asset + " kept");
        } else if (log) {
            log("downloads: " + asset + " could not be kept (error " + std::to_string(GetLastError()) + ")");
        }
    }
    return kept;
}

std::wstring newest_download(const Paths& p) {
    std::wstring best;
    FILETIME bt{};
    for (auto& n : zips_in(p.downloads)) {
        const std::wstring f = join(p.downloads, n);
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &d)) continue;
        if (best.empty() || CompareFileTime(&d.ftLastWriteTime, &bt) > 0) { best = f; bt = d.ftLastWriteTime; }
    }
    return best;
}

bool show_downloads(const Paths& p) {
    wchar_t win[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(win, MAX_PATH)) return false;
    const std::wstring exe = std::wstring(win) + L"\\explorer.exe";
    const std::wstring zip = newest_download(p);
    std::wstring cmd = L"\"" + exe + L"\" ";
    if (!zip.empty()) cmd += L"/select,\"" + zip + L"\"";
    else if (exists(p.downloads)) cmd += L"\"" + p.downloads + L"\"";
    else cmd += L"\"" + p.data + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool fetch_from_github(const std::string& tag, const PlanItem& item, const std::wstring& dest, const BytesFn& bytes, std::string* why) {
    const HttpResult h = http_get_file(asset_url(tag, item.pkg.asset), item.pkg.size, dest, bytes);
    if (!h.ok && why) *why = h.why;
    return h.ok;
}

bool fetch_manifest(std::string* text, std::string* why) {
    const HttpResult h = http_get(kManifestUrl, kManifestMaxBytes, text);
    if (!h.ok && why) *why = h.why;
    return h.ok;
}

}  // namespace sf6up
