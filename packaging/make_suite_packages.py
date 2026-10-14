"""Build the three Fluffy packages of a release (build.bat first):

  SF6_NativeCore_<core version>.zip    reframework/plugins/sf6core_native.dll
  SF6_SlotsLoader_<version>.zip        make_fluffy_package.py, unchanged (works with or without the core)
  SF6_ColorMod_<color mod version>.zip reframework/plugins/SF6_ColorModNative.dll + its locale table (needs the core)

The core and the Color Mod versions are read from their sources. No game path is shared by two packages: the core
is its own package, so every mod that needs it uses the same one.

Usage: python make_suite_packages.py --version <Slots Loader version> [--game "<game folder>"] [--out "<folder>"]
"""
import argparse, hashlib, os, re, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CRLF = chr(13) + chr(10)

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""): h.update(c)
    return h.hexdigest()[:16]

def version_in(path, pattern):
    m = re.search(pattern, open(path, encoding="utf-8").read())
    if not m: sys.exit("version not found in " + path)
    return m.group(1)

CORE_README = CRLF.join([
    "SF6 Native Core v%s",
    "===================",
    "",
    "The shared native-UI core of the SF6 mods of this release: reframework/plugins/sf6core_native.dll.",
    "Install it once; the mods that need it (SF6 Color Mod, SF6 Recording Manager) register with it at startup. On its own it",
    "changes nothing in the game.",
    "",
    "INSTALL: Fluffy Mod Manager (install this zip as a mod), or copy reframework/ into the game folder.",
    "REQUIREMENTS: REFramework in dinput8.dll, official build 1.5.8 or newer (plugin API 1.10).",
    "LOG: reframework/data/sf6core/sf6core.log",
    "",
])

COLORMOD_README = CRLF.join([
    "SF6 Color Mod v%s",
    "==================",
    "",
    "Extra costume colours, a colour editor (Fighter Settings > Edit Character, F on the Color row) and",
    "Drive colours per character and per player (character select and Training popup: X).",
    "",
    "REQUIRES the SF6 Native Core package (%s or newer) from the same release.",
    "Without it the mod stays inert; REFramework's log says: [ColorModNative] sf6core_native.dll not loaded: inert",
    "",
    "INSTALL: Fluffy Mod Manager (install this zip as a mod), or copy reframework/ into the game folder.",
    "REQUIREMENTS: REFramework in dinput8.dll, official build 1.5.8 or newer (plugin API 1.10).",
    "",
])

def write_zip(out, modinfo, readme, files):
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("modinfo.ini", modinfo)
        z.writestr("README.txt", readme)
        for src, dst in files:
            if not os.path.exists(src): sys.exit("missing: " + src)
            z.write(src, dst)
            print("  %-62s %10d  %s" % (dst, os.path.getsize(src), sha(src)))
    print("zip:", out, os.path.getsize(out), "bytes")

def game_paths(zp):
    with zipfile.ZipFile(zp) as z:
        return {n for n in z.namelist() if not n.endswith("/") and n not in ("modinfo.ini", "README.txt")}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="the Slots Loader version")
    ap.add_argument("--game", default=None, help="game folder (for the genuine AMD library), passed to make_fluffy_package.py")
    ap.add_argument("--out", default=os.path.join(REPO, "build", "release"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    core_v = version_in(os.path.join(REPO, "core", "src", "sf6core.cpp"), r'CORE_VERSION\s*=\s*"([^"]+)"')
    cm_v = version_in(os.path.join(REPO, "colormod", "src", "common.cpp"), r'kVersion\s*=\s*"([^"]+)"')

    print("== SF6 Native Core %s" % core_v)
    core_zip = os.path.join(a.out, "SF6_NativeCore_%s.zip" % core_v)
    write_zip(core_zip,
              "name=SF6 Native Core\nversion=%s\ndescription=Shared native-UI core of the SF6 mods (SF6 Color Mod needs it). Changes nothing on its own.\nauthor=Wael\nNameAsBundle=SF6 Native Core\n" % core_v,
              CORE_README % core_v,
              [(os.path.join(REPO, "build", "core", "sf6core_native.dll"), "reframework/plugins/sf6core_native.dll")])

    print("== SF6 Color Mod %s" % cm_v)
    cm_zip = os.path.join(a.out, "SF6_ColorMod_%s.zip" % cm_v)
    write_zip(cm_zip,
              "name=SF6 Color Mod\nversion=%s\ndescription=Extra costume colours, a colour editor and Drive colours per character and player, from the game's own menus. Requires SF6 Native Core %s or newer.\nauthor=Wael\nNameAsBundle=SF6 Color Mod\n" % (cm_v, core_v),
              COLORMOD_README % (cm_v, core_v),
              [(os.path.join(REPO, "build", "colormod", "SF6_ColorModNative.dll"), "reframework/plugins/SF6_ColorModNative.dll"),
               (os.path.join(REPO, "colormod", "data", "SF6_NativeLocale_data", "locale.json"), "reframework/data/SF6_NativeLocale_data/locale.json")])

    print("== SF6 Slots Loader %s" % a.version)
    sl_zip = os.path.join(a.out, "SF6_SlotsLoader_v%s.zip" % a.version)
    cmd = [sys.executable, os.path.join(HERE, "make_fluffy_package.py"), "--version", a.version, "--out", sl_zip]
    if a.game: cmd += ["--game", a.game]
    if subprocess.call(cmd) != 0: sys.exit("make_fluffy_package.py failed")

    zips = {"core": core_zip, "colormod": cm_zip, "slots": sl_zip}
    paths = {k: game_paths(v) for k, v in zips.items()}
    names = list(paths)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            both = paths[names[i]] & paths[names[j]]
            if both: sys.exit("path shared by %s and %s: %s" % (names[i], names[j], sorted(both)))
    print("no game path shared by two packages")

if __name__ == "__main__":
    main()
