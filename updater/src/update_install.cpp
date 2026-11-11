// SF6 Mod Suite updater: the steps, without the game (update_install.hpp).
#include "update_install.hpp"

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
}

namespace {
std::mutex g_state_mx;
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
    update_state(p, [&](State& st) { st.cleanup = left; });
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

InstallResult run_install(const Paths& p, const Plan& plan, const Fetch& fetch, const LogFn& log, int fail_at) {
    InstallResult r;
    auto fail = [&](Fail f, const std::string& why) { r.fail = f; r.why = why; if (log) log("install failed: " + why); remove_tree(p.staging); return r; };
    if (plan.items.empty()) return fail(Fail::Package, "nothing to install");
    remove_tree(p.staging);
    const std::wstring zips = join(p.staging, "zips"), tree = join(p.staging, "tree");
    if (!make_dirs(zips)) return fail(Fail::Files, "the staging folder cannot be made");
    // a: the zips, each one as the release says
    for (auto& it : plan.items) {
        const std::wstring dest = join(zips, it.pkg.asset);
        std::string why;
        if (!fetch(it, dest, &why)) return fail(Fail::Download, it.pkg.asset + ": " + why);
        if (!verify_file(dest, it.pkg.size, it.pkg.sha256, &why)) return fail(Fail::Check, it.pkg.asset + ": " + why);
        if (log) log(it.pkg.asset + ": downloaded, size and sha256 as the release says");
    }
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
    remove_tree(p.staging);
    if (log)
        log("installed: " + std::to_string(r.swap.replaced) + " replaced, " + std::to_string(r.swap.added) + " added, " + std::to_string(r.swap.same) +
            " unchanged, " + std::to_string(r.swap.kept) + " kept (the player's), " + std::to_string(r.swap.dirs) + " folders made");
    r.ok = true;
    return r;
}

bool fetch_from_github(const std::string& tag, const PlanItem& item, const std::wstring& dest, std::string* why) {
    const HttpResult h = http_get_file(asset_url(tag, item.pkg.asset), item.pkg.size, dest);
    if (!h.ok && why) *why = h.why;
    return h.ok;
}

bool fetch_manifest(std::string* text, std::string* why) {
    const HttpResult h = http_get(kManifestUrl, kManifestMaxBytes, text);
    if (!h.ok && why) *why = h.why;
    return h.ok;
}

}  // namespace sf6up
