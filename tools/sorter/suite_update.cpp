// SF6_ModSorter -- the SF6 Mod Suite's own updates (suite_update.hpp): the game's updater, driven from the sorter.
#include "suite_update.hpp"

#include <algorithm>

#include "../../updater/src/update_fs.hpp"
#include "../../updater/src/update_install.hpp"
#include "../../updater/src/update_logic.hpp"
#include "../../updater/src/update_pe.hpp"
#include "../../updater/src/update_text.hpp"

using namespace sf6up;

struct SuitePlan {
    std::string manifest;      // the text the check read
    bool local = false;        // from a file (tests): never installed
};

namespace {

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

}  // namespace

SuiteCheck suite_check(const std::wstring& game_dir, bool asked, const std::wstring& manifest_file) {
    SuiteCheck c;
    if (game_dir.empty()) return c;
    const Paths p(game_dir);
    const VersionReader version_of = file_versions(p.root);
    for (auto& i : detect_installed(p.root, version_of)) {
        if (!i.present) continue;
        ++c.installed;
        if (i.version.empty()) ++c.unversioned;
    }
    if (!c.installed) { c.state = SuiteCheck::NotInstalled; return c; }   // nothing of the suite there: nothing fetched, nothing written
    const State s = load_state(p);
    if (!asked && !s.check) { c.state = SuiteCheck::Off; return c; }
    const LogFn log = sorter_log(p);
    auto plan = std::make_shared<SuitePlan>();
    std::string why;
    if (!manifest_file.empty()) {
        plan->local = true;
        if (!read_file(manifest_file, &plan->manifest, 1u << 20)) why = "the manifest file cannot be read";
    } else {
        fetch_manifest(&plan->manifest, &why);
    }
    if (!why.empty()) {
        c.state = SuiteCheck::Failed;
        c.why = why;
        log("no manifest from the latest release: " + why);
        return c;
    }
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

SuiteInstall suite_install(const std::wstring& game_dir, const SuiteCheck& c, const std::string& lang, bool game_running) {
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
    const InstallResult i = run_install(p, r.plan, [tag](const PlanItem& it, const std::wstring& dest, std::string* why) {
        return fetch_from_github(tag, it, dest, why);
    }, log);
    if (!i.ok) {
        out.why = i.why;
        out.message = text::failed(lang, i.fail, i.what);
        return out;
    }
    cleanup(p, log);                                   // the game is closed: the old copies go now
    out.ok = true;
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
