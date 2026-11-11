"""Write manifest.json for a release folder: what the SF6 Mod Suite updater (updater/) reads from the latest GitHub release.

  manifest.json   the suite version, its tag, and per package zip of the folder (the all-in-one left out): the package's id, name and
                  version (its modinfo.ini), the zip's name, size and sha256, and the file that proves the package is installed (its
                  main DLL). Upload it to the release WITH the zips: the game reads
                  https://github.com/Wael3rd/SF6-ModSuite/releases/latest/download/manifest.json
                  A tool's zip (TOOLS: the Mod Sorter, make_sorter_package.py) is no package, the game never installs it:
                  it is listed apart, under "tools" (id, name, version, zip, size, sha256, the program in the zip), where
                  the tool itself reads whether a newer one is out (tools/sorter/suite_update.cpp). Older updaters ignore
                  that key.

make_bundle.py calls it at its end, so every release folder gets one. Checked, the script stops: a zip that is no known package or two
zips of one package; a package's modinfo.ini without a version; an entry the updater would refuse (the same rules as
updater/src/update_logic.cpp classify_entry, kept in step by hand: a refused entry stops the player's whole update); two zips
of one tool, a tool's zip without its program at the root or without a version.

Usage: python make_manifest.py --release "<folder>" --version <suite version> [--out "<file>"]
"""
import argparse, hashlib, json, os, re, sys, zipfile

BUNDLE_PREFIX = "SF6_ModSuite_AllInOne_"
# zips of a release that are no package (in the all-in-one, never among the manifest's packages): the zip's prefix, the tool's
# id in the manifest (the tool looks itself up by it), its program at the zip's root
TOOLS = [("SF6_ModSorter_", "mod_sorter", "SF6_ModSorter.exe")]
TOOL_PREFIXES = tuple(t[0] for t in TOOLS)
FORMAT = "sf6suite-manifest-1"
# id, the file that proves the package is installed: the same table as updater/src/update_logic.cpp known_packages()
KNOWN = [
    ("slots_loader", "reframework/plugins/SF6_CostumeSlotsNative.dll"),
    ("color_mod", "reframework/plugins/SF6_ColorModNative.dll"),
    ("native_core", "reframework/plugins/sf6core_native.dll"),
    ("recording_manager", "reframework/plugins/SF6_CRSMNative.dll"),
    ("profile_mod", "reframework/plugins/profile_mod.dll"),
]
ROOT_FILES = {"amd_ags_x64.dll", "amd_ags_x64_real.dll", "sf6_slotsclean.exe"}
MOD_FOLDERS = {"costume_mods", "costume_mods_nsfw", "color_mods", "stage_mods", "title_mods", "ui_mods"}
RESERVED = re.compile(r"^(con|prn|aux|nul|clock\$|conin\$|conout\$|com[0-9]|lpt[0-9])$", re.I)
VERSION = re.compile(r"^\d{1,6}(\.\d{1,6}){0,3}(-[A-Za-z0-9.]+)?$")
ASSET = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.\-]{0,95}\.zip$")


def entry_ok(name):
    """The updater's classify_entry, as a yes / no (modinfo.ini and README.txt at the root: yes, they are skipped)."""
    if not name or len(name) > 200:
        return False
    p = name.replace("\\", "/")
    is_dir = p.endswith("/")
    p = p.rstrip("/") if is_dir else p
    if not p or p.startswith("/"):
        return False
    seg = p.split("/")
    if len(seg) > 8:
        return False
    for s in seg:
        if s in ("", ".", "..") or s[-1] in ". " or s[0] == " " or any(ord(c) < 32 or c in ':<>"|?*' for c in s):
            return False
        if RESERVED.match(s.split(".")[0]):
            return False
    ls = [s.lower() for s in seg]
    if len(seg) == 1:
        return not is_dir and (ls[0] in ("modinfo.ini", "readme.txt") or ls[0] in ROOT_FILES)
    if ls[0] != "reframework":
        return False
    if len(seg) == 2:
        return is_dir
    if ls[1] == "plugins":
        return not is_dir and len(seg) == 3 and ls[2].endswith(".dll")
    if ls[1] == "data":
        if len(seg) >= 3 and ls[2] == "sf6_update_data":
            return False
        return is_dir or (len(seg) >= 4 and ls[-1] != "identity.json" and ".old-" not in ls[-1])
    if ls[1] in MOD_FOLDERS:
        return len(seg) == 3
    return False


def modinfo_of(z):
    info = {}
    for line in z.read("modinfo.ini").decode("utf-8", "replace").splitlines():
        m = re.match(r"\s*([A-Za-z]+)\s*=\s*(.*)$", line)
        if m:
            info[m.group(1).lower()] = m.group(2).strip()
    return info


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""):
            h.update(c)
    return h.hexdigest()


def build_manifest(release, version):
    if not VERSION.match(version):
        sys.exit("make_manifest: %s is not a version" % version)
    names = sorted(n for n in os.listdir(release)
                   if n.lower().endswith(".zip") and not n.startswith(BUNDLE_PREFIX) and not n.startswith(TOOL_PREFIXES))
    if not names:
        sys.exit("make_manifest: no package zip in " + release)
    packages, seen = [], {}
    for n in names:
        if not ASSET.match(n):
            sys.exit("make_manifest: %s: not a name the updater accepts" % n)
        path = os.path.join(release, n)
        with zipfile.ZipFile(path) as z:
            entries = z.namelist()
            bad = [e for e in entries if not entry_ok(e)]
            if bad:
                sys.exit("make_manifest: %s: the updater would refuse %s" % (n, bad[:5]))
            lower = {e.lower() for e in entries}
            ids = [(i, proof) for i, proof in KNOWN if proof.lower() in lower]
            if len(ids) != 1:
                sys.exit("make_manifest: %s ships %s: one package's main DLL expected" % (n, [p for _, p in ids] or "no main DLL"))
            pid, proof = ids[0]
            if pid in seen:
                sys.exit("make_manifest: %s and %s are both %s" % (seen[pid], n, pid))
            seen[pid] = n
            info = modinfo_of(z)
        v = info.get("version", "")
        if not VERSION.match(v):
            sys.exit("make_manifest: %s: its modinfo.ini has no version" % n)
        packages.append({"id": pid, "name": info.get("name") or pid, "version": v, "asset": n,
                         "size": os.path.getsize(path), "sha256": sha256(path), "proof": proof})
    order = [i for i, _ in KNOWN]
    packages.sort(key=lambda p: order.index(p["id"]))
    m = {"format": FORMAT, "suite": version, "tag": "v" + version, "packages": packages}
    tools = build_tools(release)
    if tools:
        m["tools"] = tools
    return m


def build_tools(release):
    """The tools' zips of the folder, as the manifest lists them (update_logic.cpp parse_manifest, "tools")."""
    tools = []
    for prefix, tid, exe in TOOLS:
        names = sorted(n for n in os.listdir(release) if n.lower().endswith(".zip") and n.startswith(prefix))
        if not names:
            continue
        if len(names) > 1:
            sys.exit("make_manifest: %s are both %s" % (" and ".join(names), tid))
        n = names[0]
        if not ASSET.match(n):
            sys.exit("make_manifest: %s: not a name the updater accepts" % n)
        path = os.path.join(release, n)
        with zipfile.ZipFile(path) as z:
            if exe not in z.namelist():
                sys.exit("make_manifest: %s: no %s at its root" % (n, exe))
            info = modinfo_of(z)
        v = info.get("version", "")
        if not VERSION.match(v):
            sys.exit("make_manifest: %s: its modinfo.ini has no version" % n)
        tools.append({"id": tid, "name": info.get("name") or tid, "version": v, "asset": n,
                      "size": os.path.getsize(path), "sha256": sha256(path), "file": exe})
    return tools


def write_manifest(release, version, out=None):
    m = build_manifest(release, version)
    out = out or os.path.join(release, "manifest.json")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    print("manifest: %s (%d packages, suite %s)" % (out, len(m["packages"]), version))
    for p in m["packages"]:
        print("  %-18s %-8s %-36s %9d %s" % (p["id"], p["version"], p["asset"], p["size"], p["sha256"][:16]))
    for p in m.get("tools", []):
        print("  %-18s %-8s %-36s %9d %s  (tool)" % (p["id"], p["version"], p["asset"], p["size"], p["sha256"][:16]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--release", required=True)
    ap.add_argument("--version", required=True, help="the suite version (the release tag without its v)")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    write_manifest(a.release, a.version, a.out)


if __name__ == "__main__":
    main()
