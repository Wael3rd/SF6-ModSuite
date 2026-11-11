// SF6_ModSorter -- the SF6 Mod Suite's own updates (suite_update.hpp): the game's updater, driven from the sorter.
#include "suite_update.hpp"

#include <windows.h>

#include <algorithm>

#include "../../updater/src/update_fs.hpp"
#include "../../updater/src/update_install.hpp"
#include "../../updater/src/update_logic.hpp"
#include "../../updater/src/update_net.hpp"
#include "../../updater/src/update_pe.hpp"
#include "../../updater/src/update_sys.hpp"
#include "../../updater/src/update_text.hpp"
#include "version.hpp"

using namespace sf6up;

struct SuitePlan {
    std::string manifest;      // the text the check read
    bool local = false;        // from a file (tests): never installed
};

namespace {

constexpr const char* kSelfId = "mod_sorter";   // the sorter's entry in the manifest's "tools"

VersionReader file_versions(const std::wstring& root) {
    return [root](const KnownPackage& k) { return file_package_version(join(root, k.proof)); };
}

LogFn sorter_log(const Paths& p) {
    return [&p](const std::string& line) { append_log(p, "mod sorter: " + line); };
}

std::string json_text(const std::string& s) {
    std::string o = "\"";
    for (const char ch : s) {
        const unsigned char c = (unsigned char)ch;
        if (c == '"' || c == '\\') { o += '\\'; o += ch; }
        else if (c < 0x20) o += ' ';
        else o += ch;
    }
    return o + "\"";
}

std::string win_error(const char* what) { return std::string(what) + " (error " + std::to_string(GetLastError()) + ")"; }

}  // namespace

SuiteCheck suite_check(const std::wstring& game_dir, bool asked, const std::wstring& manifest_file, SelfCheck* self) {
    SuiteCheck c;
    const Paths p(game_dir);
    const VersionReader version_of = file_versions(p.root);
    State s;
    bool check_on = true;        // no game folder, or nothing of the suite in it: no state.json to turn the look-up off
    bool suite_wants = false;
    if (!game_dir.empty()) {
        for (auto& i : detect_installed(p.root, version_of)) {
            if (!i.present) continue;
            ++c.installed;
            if (i.version.empty()) ++c.unversioned;
        }
        if (!c.installed) c.state = SuiteCheck::NotInstalled;   // nothing of the suite there: nothing written
        else {
            s = load_state(p);
            check_on = s.check;
            if (!asked && !s.check) c.state = SuiteCheck::Off;
            else suite_wants = true;
        }
    }
    const bool self_wants = self && (asked || check_on);
    if (self) {
        *self = SelfCheck();
        self->current = kSorterVersion;
        self->state = self_wants ? SelfCheck::Unknown : SelfCheck::Off;
    }
    if (!suite_wants && !self_wants) return c;
    const LogFn log = suite_wants ? sorter_log(p) : LogFn([](const std::string&) {});
    auto plan = std::make_shared<SuitePlan>();
    std::string why;
    if (!manifest_file.empty()) {
        plan->local = true;
        if (!read_file(manifest_file, &plan->manifest, 1u << 20)) why = "the manifest file cannot be read";
    } else {
        fetch_manifest(&plan->manifest, &why);
    }
    if (!why.empty()) {
        if (suite_wants) { c.state = SuiteCheck::Failed; c.why = why; }
        if (self_wants) { self->state = SelfCheck::Failed; self->why = why; }
        log("no manifest from the latest release: " + why);
        return c;
    }
    if (self_wants) {
        Manifest m;
        std::string bad;
        if (!parse_manifest(plan->manifest, &m, &bad)) {
            self->state = SelfCheck::Failed;
            self->why = bad;
        } else {
            const ManifestTool* t = find_tool(m, kSelfId);
            self->latest = t ? t->version : "";
            self->state = t && newer(t->version, kSorterVersion) ? SelfCheck::Available : SelfCheck::Current;
            if (self->state == SelfCheck::Available) {
                self->plan = plan;
                log(std::string("the release ships Mod Sorter ") + t->version + " (this one: " + kSorterVersion + ")");
            }
        }
    }
    if (!suite_wants) return c;
    const CheckResult r = run_check(p, version_of, plan->manifest, "");
    if (!r.ok) {
        c.state = SuiteCheck::Failed;
        c.why = r.why;
        log("the latest release's manifest was refused: " + r.why);
        return c;
    }
    c.latest = r.plan.suite;
    for (auto& it : r.plan.items) c.lines.push_back({it.pkg.name, it.from, it.pkg.version});
    if (c.lines.empty()) {
        c.state = SuiteCheck::Current;
        log("nothing newer to offer (latest release " + c.latest + ")");
        return c;
    }
    c.state = SuiteCheck::Available;
    c.ignored = s.ignored == c.latest;
    c.plan = plan;
    std::string offer = "release " + c.latest + " offers:";
    for (auto& it : r.plan.items) offer += " " + it.pkg.id + " " + it.from + " -> " + it.pkg.version + ";";
    log(offer);
    return c;
}

SuiteInstall suite_install(const std::wstring& game_dir, const SuiteCheck& c, const std::string& lang, bool game_running,
                           Progress* progress) {
    SuiteInstall out;
    auto refuse = [&](const std::string& why) {
        out.why = why;
        out.message = text::failed(lang, Fail::Package, "");
        return out;
    };
    if (game_dir.empty() || c.state != SuiteCheck::Available || !c.plan) return refuse("nothing to install");
    if (c.plan->local) return refuse("a manifest read from a file is never installed");
    if (game_running) return refuse("the game is running");
    const Paths p(game_dir);
    const LogFn log = sorter_log(p);
    recover(p, log);                                   // a swap the game or the sorter did not finish: undone first
    // what is installed now: the game may have updated itself since the check
    const CheckResult r = run_check(p, file_versions(p.root), c.plan->manifest, "");
    if (!r.ok) return refuse(r.why);
    out.version = r.plan.suite;
    if (r.plan.items.empty()) {
        out.ok = out.nothing = true;
        cleanup(p, log);
        return out;
    }
    std::string start = "installing release " + r.plan.suite + ":";
    for (auto& it : r.plan.items) start += " " + it.pkg.id + " " + it.from + " -> " + it.pkg.version + ";";
    log(start);
    const std::string tag = r.plan.tag;
    const InstallResult i = run_install(p, r.plan, [tag](const PlanItem& it, const std::wstring& dest, const BytesFn& bytes, std::string* why) {
        return fetch_from_github(tag, it, dest, bytes, why);
    }, log, -1, progress);
    if (!i.ok) {
        out.why = i.why;
        out.message = text::failed(lang, i.fail, i.what);
        return out;
    }
    cleanup(p, log);                                   // the game is closed: the old copies go now
    out.ok = true;
    return out;
}

std::wstring self_exe_path() {
    std::wstring buf(1024, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (!n) return L"";
        if (n < buf.size()) { buf.resize(n); return buf; }
        if (buf.size() >= 32768) return L"";
        buf.resize(buf.size() * 2);
    }
}

void self_cleanup() {
    const std::wstring exe = self_exe_path();
    if (!exe.empty()) DeleteFileW((exe + L".old").c_str());
}

SelfInstall self_install(const SelfCheck& c, const std::string& lang, Progress* progress, const std::wstring& exe_given,
                         const std::wstring& assets) {
    SelfInstall out;
    std::wstring dir, fresh;                           // what a failure leaves nothing of
    auto fail = [&](Fail f, const std::string& why, const std::string& what = "") {
        if (!fresh.empty()) DeleteFileW(fresh.c_str());
        if (!dir.empty()) remove_tree(dir);
        out.why = why;
        out.message = text::failed(lang, f, what);
        return out;
    };
    if (c.state != SelfCheck::Available || !c.plan) return fail(Fail::Package, "nothing to install");
    if (c.plan->local && assets.empty()) return fail(Fail::Package, "a manifest read from a file is never installed");
    Manifest m;
    std::string why;
    if (!parse_manifest(c.plan->manifest, &m, &why)) return fail(Fail::Package, why);
    const ManifestTool* t = find_tool(m, kSelfId);
    if (!t || !newer(t->version, kSorterVersion)) return fail(Fail::Package, "the release ships no newer Mod Sorter");
    const std::wstring exe = exe_given.empty() ? self_exe_path() : exe_given;
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return fail(Fail::Files, "the exe to replace is not found");

    wchar_t tmp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (!n || n > MAX_PATH) return fail(Fail::Files, "no temporary folder");
    dir = std::wstring(tmp, n) + L"SF6_ModSorter_update";
    remove_tree(dir);
    if (!make_dirs(dir)) return fail(Fail::Files, "the temporary folder cannot be made");
    const std::wstring zip = dir + L"\\" + widen(t->asset);

    // the zip: downloaded (or, tests, copied from a folder), then its size and sha256 against the manifest's
    if (progress) { progress->reset(); progress->total.store(t->size); progress->step.store(Progress::Download); }
    if (!assets.empty()) {
        if (!CopyFileW((assets + L"\\" + widen(t->asset)).c_str(), zip.c_str(), FALSE)) return fail(Fail::Download, win_error("the zip is not in the assets folder"));
    } else {
        const int64_t size = t->size;
        const HttpResult h = http_get_file(asset_url(m.tag, t->asset), t->size, zip, [progress, size](int64_t got) {
            if (progress) progress->done.store(std::max<int64_t>(0, std::min(got, size)));
        });
        if (!h.ok) return fail(Fail::Download, t->asset + ": " + h.why);
    }
    if (progress) { progress->done.store(t->size); progress->step.store(Progress::Install); }
    if (!verify_file(zip, t->size, t->sha256, &why)) return fail(Fail::Check, t->asset + ": " + why);

    // the exe out of it, next to the one it replaces (the same volume: the swap is two renames), judged by Smart App Control
    fresh = exe + L".new";
    DeleteFileW(fresh.c_str());
    if (!extract_one(zip, t->file, fresh, &why)) return fail(Fail::Package, t->asset + ": " + why);
    const SacResult sac = sac_check(fresh);
    if (sac.verdict == Sac::Blocked) return fail(Fail::Blocked, "Smart App Control blocks " + t->file, t->file);
    if (sac.verdict != Sac::Ok) return fail(Fail::Package, t->file + " is no program Windows loads (error " + std::to_string(sac.error) + ")");

    // the swap: a running exe can be renamed, not overwritten
    const std::wstring old = exe + L".old";
    if (!MoveFileExW(exe.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return fail(Fail::Files, win_error("the exe cannot be renamed"));
    if (!MoveFileExW(fresh.c_str(), exe.c_str(), 0)) {
        const std::string e = win_error("the new exe cannot be put in place");
        MoveFileExW(old.c_str(), exe.c_str(), 0);      // back as it was
        return fail(Fail::Files, e);
    }
    fresh.clear();
    remove_tree(dir);
    out.ok = true;
    out.version = t->version;
    return out;
}

std::string suite_line_text(const SuiteLine& l, const std::string& lang) {
    return text::format(text::tr(lang, text::Word::Line), {l.name, l.from, l.to});
}

std::string suite_check_json(const SuiteCheck& c) {
    static const char* const names[] = { "no_game", "off", "not_installed", "failed", "current", "available" };
    std::string j = std::string("{\"state\":\"") + names[c.state] + "\",\"latest\":" + json_text(c.latest) + ",\"installed\":" +
                    std::to_string(c.installed) + ",\"unversioned\":" + std::to_string(c.unversioned) + ",\"ignored\":" +
                    (c.ignored ? "true" : "false") + ",\"why\":" + json_text(c.why) + ",\"lines\":[";
    for (size_t k = 0; k < c.lines.size(); ++k)
        j += std::string(k ? "," : "") + "{\"name\":" + json_text(c.lines[k].name) + ",\"from\":" + json_text(c.lines[k].from) + ",\"to\":" +
             json_text(c.lines[k].to) + "}";
    return j + "]}";
}

std::string self_check_json(const SelfCheck& c) {
    static const char* const names[] = { "unknown", "off", "failed", "current", "available" };
    return std::string("{\"sorter\":\"") + names[c.state] + "\",\"current\":" + json_text(c.current) + ",\"latest\":" +
           json_text(c.latest) + ",\"why\":" + json_text(c.why) + "}";
}
