"""Build the all-in-one Fluffy package of a release from its separate packages:

  SF6_ModSuite_AllInOne_v<version>.zip   every file of every package zip found in the release folder, one modinfo.ini,
                                         one README.txt (this package's notice, then each package's README)

Run it after make_suite_packages.py, once every package of the release is in the folder (the Recording Manager's zip
is built apart and copied there). Two packages shipping the same path with different contents stop the build.

Usage: python make_bundle.py --version <release version> [--release "<folder>"]
"""
import argparse, hashlib, os, re, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CRLF = chr(13) + chr(10)
BUNDLE_PREFIX = "SF6_ModSuite_AllInOne_"

NOTICE = [
    "SF6 Mod Suite v%s (all-in-one)",
    "================================",
    "",
    "Every package of this release in one Fluffy Mod Manager package:",
    "%s",
    "",
    "Install EITHER this package OR the separate ones, never both: two packages shipping the same files conflict in",
    "Fluffy Mod Manager. To switch, disable or remove the one you have first.",
    "",
    "The README of each package follows.",
    "",
]


def modinfo_of(z):
    info = {}
    for line in z.read("modinfo.ini").decode("utf-8", "replace").splitlines():
        m = re.match(r"\s*([A-Za-z]+)\s*=\s*(.*)$", line)
        if m:
            info[m.group(1).lower()] = m.group(2).strip()
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--release", default=os.path.join(REPO, "build", "release"))
    a = ap.parse_args()

    names = sorted(n for n in os.listdir(a.release)
                   if n.lower().endswith(".zip") and not n.startswith(BUNDLE_PREFIX))
    if not names:
        sys.exit("no package zip in " + a.release)
    files, owner, dirs, packages, readmes = {}, {}, {}, [], []
    for n in names:
        with zipfile.ZipFile(os.path.join(a.release, n)) as z:
            info = modinfo_of(z)
            label = "%s %s" % (info.get("name", n), info.get("version", "?"))
            packages.append(label)
            if "README.txt" in z.namelist():
                readmes.append(z.read("README.txt").decode("utf-8").replace(CRLF, "\n").rstrip("\n"))
            for zi in z.infolist():
                e = zi.filename
                if e.endswith("/"):                          # folders a package creates (costume_mods/<Character>/...)
                    dirs.setdefault(e, zi)
                    continue
                if e in ("modinfo.ini", "README.txt"):
                    continue
                data = z.read(e)
                if e in files and files[e] != data:
                    sys.exit("%s: shipped by %s and %s with different contents" % (e, owner[e], n))
                files[e] = data
                owner.setdefault(e, n)

    readme = "\n".join(NOTICE) % (a.version, "\n".join("  - " + p for p in packages))
    readme = (readme + "\n" + ("\n\n" + "-" * 78 + "\n\n").join(readmes) + "\n").replace("\n", CRLF)
    modinfo = CRLF.join([
        "name=SF6 Mod Suite (all-in-one)",
        "version=%s" % a.version,
        "description=Every SF6 Mod Suite package in one: %s. Install either this or the separate packages, never both."
        % ", ".join(packages),
        "author=Wael",
        "NameAsBundle=SF6 Mod Suite",
        "",
    ])
    out = os.path.join(a.release, "%sv%s.zip" % (BUNDLE_PREFIX, a.version))
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("modinfo.ini", modinfo)
        z.writestr("README.txt", readme)
        for e in sorted(dirs):
            z.writestr(dirs[e], b"")
        for e in sorted(files):
            z.writestr(e, files[e])
    print("== SF6 Mod Suite %s (all-in-one): %d files, %d folders from %d packages" % (a.version, len(files), len(dirs), len(names)))
    for p in packages:
        print("  " + p)
    print("zip: %s %d bytes md5 %s" % (out, os.path.getsize(out), hashlib.md5(open(out, "rb").read()).hexdigest()))


if __name__ == "__main__":
    main()
