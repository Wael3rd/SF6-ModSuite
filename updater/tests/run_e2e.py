"""The updater's end-to-end run, outside the game (not shipped): a fake game folder holding a fresh install of one release, updated by the
updater's own code (update_e2e.exe) from local copies of the next release's zips, must end identical to a fresh install of that next
release. Then one real HTTPS fetch of a public release asset, checked against the digest the GitHub API reports.

Both release folders are only read. Everything is written under --work.

Usage: python run_e2e.py --old <release folder> --new <release folder> --work <folder> [--exe <update_e2e.exe>] [--fetch]
"""
import argparse, json, os, shutil, subprocess, sys, urllib.request, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "packaging"))
from make_manifest import BUNDLE_PREFIX, TOOL_PREFIXES, write_manifest  # noqa: E402

OURS = "reframework/data/SF6_Update_data/"      # the updater's own state and log: not part of an install


def packages(release):
    return sorted(n for n in os.listdir(release)
                  if n.lower().endswith(".zip") and not n.startswith(BUNDLE_PREFIX) and not n.startswith(TOOL_PREFIXES))


def fresh_install(release, game):
    """Every package zip's payload over an empty folder, as Fluffy Mod Manager or a copy by hand installs it (its own modinfo.ini and
    README.txt at the zip's root left out)."""
    shutil.rmtree(game, ignore_errors=True)
    os.makedirs(game)
    for n in packages(release):
        with zipfile.ZipFile(os.path.join(release, n)) as z:
            for e in z.infolist():
                if e.filename in ("modinfo.ini", "README.txt"):
                    continue
                dst = os.path.join(game, *e.filename.rstrip("/").split("/"))
                if e.is_dir():
                    os.makedirs(dst, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(dst, "wb") as f:
                    f.write(z.read(e))


def tree(root):
    out = {}
    for d, dirs, files in os.walk(root):
        rel = os.path.relpath(d, root).replace("\\", "/")
        rel = "" if rel == "." else rel + "/"
        if rel.startswith(OURS):
            continue
        for x in dirs:
            if (rel + x + "/") != OURS:
                out[rel + x + "/"] = None
        for x in files:
            out[rel + x] = open(os.path.join(d, x), "rb").read()
    out.pop(OURS, None)
    return out


def run(cmd):
    print("$", " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
    p = subprocess.run(cmd, capture_output=True, text=True)
    sys.stdout.write(p.stdout)
    if p.stderr:
        sys.stdout.write(p.stderr)
    return p.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True)
    ap.add_argument("--new", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--exe", default=os.path.join(REPO, "build", "update_tests", "update_e2e.exe"))
    ap.add_argument("--fetch", action="store_true")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)
    old_v = os.path.basename(a.old.rstrip("/\\")).split("_")[-1]
    new_v = os.path.basename(a.new.rstrip("/\\")).split("_")[-1]
    game, expected, assets = (os.path.join(a.work, x) for x in ("game", "expected", "assets"))
    print("== a fresh %s install, and a fresh %s install to compare with" % (old_v, new_v))
    fresh_install(a.old, game)
    fresh_install(a.new, expected)
    before = tree(game)
    shutil.rmtree(assets, ignore_errors=True)
    os.makedirs(assets)
    for n in packages(a.new):
        shutil.copyfile(os.path.join(a.new, n), os.path.join(assets, n))     # the local copies the "download" takes
    m_new = write_manifest(a.new, new_v, os.path.join(a.work, "manifest_%s.json" % new_v))
    m_old = write_manifest(a.old, old_v, os.path.join(a.work, "manifest_%s.json" % old_v))
    print("== the update")
    rc = run([a.exe, "update", "--game", game, "--manifest", m_new, "--reports", m_old, "--assets", assets])
    after = tree(game)
    want = tree(expected)
    diff = sorted(set(after) ^ set(want)) + sorted(k for k in set(after) & set(want) if after[k] != want[k])
    changed = sorted(k for k in set(before) & set(after) if before[k] != after[k])
    print("files changed by the update: %d" % len(changed))
    for k in changed:
        print("  " + k)
    print("tree identical to a fresh %s install: %s" % (new_v, "yes (%d files and folders)" % len(want) if not diff else "NO"))
    for k in diff[:30]:
        print("  differs: " + k)
    ok = rc == 0 and not diff
    print("== a zip that is not the release's (one byte changed, same size): the update stops, nothing changes")
    game2, assets2 = os.path.join(a.work, "game_bad"), os.path.join(a.work, "assets_bad")
    fresh_install(a.old, game2)
    before2 = tree(game2)
    shutil.rmtree(assets2, ignore_errors=True)
    shutil.copytree(assets, assets2)
    # a zip the update downloads: one this release changes (an unchanged package is never fetched), the Color Mod's when it is one
    kept = set(packages(a.old))
    victim = os.path.join(assets2, max(packages(a.new), key=lambda n: (n not in kept, "ColorMod" in n)))
    data = bytearray(open(victim, "rb").read())
    data[len(data) // 2] ^= 0xFF
    open(victim, "wb").write(bytes(data))
    rc_bad = run([a.exe, "update", "--game", game2, "--manifest", m_new, "--reports", m_old, "--assets", assets2])
    same = tree(game2) == before2
    print("game folder unchanged after the refused zip: %s" % ("yes" if same else "NO"))
    ok = ok and rc_bad != 0 and same
    print("== the main DLLs' versions, as the host reads them")
    ok = run([a.exe, "versions", "--game", game]) == 0 and ok      # and what each file says without being loaded: the same
    if a.fetch:
        print("== a real fetch (GitHub, HTTPS, redirects, sha256)")
        tag = "v" + new_v
        api = json.load(urllib.request.urlopen("https://api.github.com/repos/Wael3rd/SF6-ModSuite/releases/tags/" + tag, timeout=30))
        asset = min((x for x in api["assets"] if x["name"].endswith(".zip")), key=lambda x: x["size"])
        digest = (asset.get("digest") or "").split(":")[-1]
        print("GitHub API: %s, %d bytes, digest %s" % (asset["name"], asset["size"], digest or "(none)"))
        url = "https://github.com/Wael3rd/SF6-ModSuite/releases/download/%s/%s" % (tag, asset["name"])
        out = os.path.join(a.work, "fetch", asset["name"])
        os.makedirs(os.path.dirname(out), exist_ok=True)
        rc2 = run([a.exe, "fetch", "--url", url, "--size", str(asset["size"]), "--sha256", digest, "--out", out])
        run([a.exe, "manifest"])
        ok = ok and rc2 == 0 and bool(digest)
    print("E2E", "PASSED" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
