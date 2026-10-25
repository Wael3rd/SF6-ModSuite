"""Build the four Fluffy packages of a release (build.bat first):

  SF6_NativeCore_<core version>.zip    reframework/plugins/sf6core_native.dll
  SF6_SlotsLoader_<version>.zip        make_fluffy_package.py, unchanged (works with or without the core)
  SF6_ColorMod_<color mod version>.zip reframework/plugins/SF6_ColorModNative.dll + its locale table (needs the core)
  SF6_CRSM_<recording manager version>.zip  the SF6 Recording Manager: reframework/plugins/SF6_CRSMNative.dll, its locale
                                       table, settings.json (every owner native, standalone auto) and the Community
                                       config.json (needs the core)

The core, Color Mod and Recording Manager versions are read from their sources. No game path is shared by two
packages: the core is its own package, so every mod that needs it uses the same one.

The Recording Manager's config.json holds the Community Worker's address and its API key. It never goes into git: it
is read from crsm/package/config.json, a local file git ignores (copy crsm/package/config.example.json and fill it).
Checked, the build stops on the first broken rule: the config present, untracked, ignored, not the template, an https
api_base; the key in no packaged file but that config.json (the DLL included); no identity.json (a player's Community
token), no reframework/autorun/ (no Lua), no SF6_CommunityBridge.dll, no SF6_NativeLocale_data/ path; the DLL newer
than every crsm/src file and holding its version string. --without-crsm builds the three others (a clone without the
config).

Usage: python make_suite_packages.py --version <Slots Loader version> [--game "<game folder>"] [--out "<folder>"]
                                     [--without-crsm]
"""
import argparse, hashlib, json, os, re, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CRLF = chr(13) + chr(10)
sys.path.insert(0, HERE)
from make_fluffy_package import CHARACTERS      # the 32 character folders of costume_mods, reused for color_mods
CRSM = os.path.join(REPO, "crsm")
CRSM_MIN_CORE = "0.8.0"       # the Native Core's training pause-menu service (pm_*), needed by the Recording Manager
CRSM_OWNERS = ("community", "files", "replay", "reversal", "record", "ui")
CRSM_CONFIG_REL = "crsm/package/config.json"
G_CRSM_CONFIG = "reframework/data/SF6_Community_data/config.json"
CRSM_FORBIDDEN = [
    (lambda p: p.lower().endswith("sf6_communitybridge.dll"), "SF6_CommunityBridge.dll (the v1.x bridge, replaced by the DLL's WinHTTP)"),
    (lambda p: p.lower().startswith("reframework/data/sf6_nativelocale_data/"), "the Color Mod's locale path (SF6_NativeLocale_data)"),
    (lambda p: p.lower().startswith("reframework/autorun/"), "a Lua script (the package ships the DLL alone)"),
    (lambda p: p.lower().endswith("identity.json"), "identity.json (a player's Community token: never in a package)"),
]

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
    "changes nothing in the game, except that it makes sure Windows gives the game the 1 ms timer its 60 fps limiter needs",
    "(Windows 11 sometimes ignored the game's request: sessions stuck at 45 fps); it checks this every second and asks",
    "again when needed. An empty reframework/data/sf6core/timer_off.txt turns this off.",
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

def write_zip(out, modinfo, readme, files, dirs=(), texts=()):
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("modinfo.ini", modinfo)
        z.writestr("README.txt", readme)
        for name, text in texts:                      # extra text files (name inside the zip, content)
            z.writestr(name, text)
        for d in dirs:                                # empty folders shipped as they are (like costume_mods/<Character>/)
            zi = zipfile.ZipInfo(d if d.endswith("/") else d + "/")
            zi.external_attr = (0o40755 << 16) | 0x10
            z.writestr(zi, b"")
        for src, dst in files:
            if not os.path.exists(src): sys.exit("missing: " + src)
            z.write(src, dst)
            print("  %-62s %10d  %s" % (dst, os.path.getsize(src), sha(src)))
    print("zip:", out, os.path.getsize(out), "bytes")

def game_paths(zp):
    with zipfile.ZipFile(zp) as z:
        return {n for n in z.namelist() if not n.endswith("/") and n not in ("modinfo.ini", "README.txt")}

# ---- SF6 Recording Manager (crsm/) ------------------------------------------------------------------------------------
def crsm_fail(msg):
    sys.exit("SF6 Recording Manager: " + msg)

def git_rc(*args):
    try:
        return subprocess.run(["git", "-C", REPO] + list(args), capture_output=True, text=True).returncode
    except OSError:
        return None

def crsm_config():
    """(bytes, key) of the local crsm/package/config.json, after its checks."""
    path = os.path.join(REPO, *CRSM_CONFIG_REL.split("/"))
    if not os.path.exists(path):
        crsm_fail("missing %s (the local, git-ignored Community config with the Worker's API key: copy "
                  "crsm/package/config.example.json, fill it; it never goes into git), or use --without-crsm" % CRSM_CONFIG_REL)
    if git_rc("ls-files", "--error-unmatch", CRSM_CONFIG_REL) == 0:
        crsm_fail(CRSM_CONFIG_REL + " is tracked by git: it must stay local (.gitignore)")
    if git_rc("check-ignore", "-q", CRSM_CONFIG_REL) not in (0, None):
        crsm_fail(CRSM_CONFIG_REL + " is not ignored by git (.gitignore must list it)")
    data = open(path, "rb").read()
    try:
        cfg = json.loads(data.decode("utf-8-sig"))
    except ValueError as e:
        crsm_fail("config.json is not JSON: %s" % e)
    key, base = cfg.get("api_key"), cfg.get("api_base")
    if not isinstance(key, str) or len(key) < 8 or key.startswith("<"):
        crsm_fail("config.json has no real api_key (still the template?)")
    if not isinstance(base, str) or not base.startswith("https://"):
        crsm_fail("config.json has no https api_base")
    return data, key

def crsm_dll(version):
    dll = os.path.join(REPO, "build", "crsm", "SF6_CRSMNative.dll")
    if not os.path.exists(dll):
        crsm_fail("missing %s: run build.bat (or crsm\\build.bat) first" % dll)
    t = os.path.getmtime(dll)
    for root, _, names in os.walk(os.path.join(CRSM, "src")):
        for n in names:
            p = os.path.join(root, n)
            if os.path.getmtime(p) > t:
                crsm_fail("the DLL is older than %s: run build.bat first" % os.path.relpath(p, REPO))
    if version.encode("ascii") not in open(dll, "rb").read():
        crsm_fail("the DLL does not hold the version string %s: run build.bat first" % version)
    return dll

def crsm_settings():
    path = os.path.join(CRSM, "package", "settings.json")
    try:
        j = json.loads(open(path, "rb").read().decode("utf-8"))
    except ValueError as e:
        crsm_fail("%s: %s" % (path, e))
    owners = j.get("owners")
    if not isinstance(owners, dict) or sorted(owners) != sorted(CRSM_OWNERS) or any(v != "native" for v in owners.values()) \
            or j.get("standalone") != "auto":
        crsm_fail("crsm/package/settings.json: the package needs the six owners \"native\" and \"standalone\": \"auto\"")
    return path

def crsm_text(name, version, crlf):
    s = open(os.path.join(CRSM, "package", name), encoding="utf-8").read()
    s = s.replace("@VERSION@", version).replace("@CORE@", CRSM_MIN_CORE).replace("\r\n", "\n")
    m = re.search(r"@[A-Z]+@", s)
    if m:
        crsm_fail("crsm/package/%s: unknown placeholder %s" % (name, m.group(0)))
    return s.replace("\n", CRLF) if crlf else s

def crsm_package(out_dir):
    version = version_in(os.path.join(CRSM, "src", "common.cpp"), r'kVersion\s*=\s*"([^"]+)"')
    print("== SF6 Recording Manager %s" % version)
    dll = crsm_dll(version)
    locale = os.path.join(CRSM, "data", "SF6_CRSM_data", "locale.json")
    json.loads(open(locale, "rb").read().decode("utf-8-sig"))
    config, key = crsm_config()
    settings = crsm_settings()
    modinfo, readme = crsm_text("modinfo.ini", version, False), crsm_text("README.txt", version, True)
    files = [(dll, "reframework/plugins/SF6_CRSMNative.dll"),
             (locale, "reframework/data/SF6_CRSM_data/locale.json"),
             (settings, "reframework/data/SF6_CRSMNative_data/settings.json"),
             (os.path.join(REPO, *CRSM_CONFIG_REL.split("/")), G_CRSM_CONFIG)]
    dsts = [d for _, d in files]
    if len(set(d.lower() for d in dsts)) != len(dsts):
        crsm_fail("a path appears twice")
    for _, dst in files:
        for test, why in CRSM_FORBIDDEN:
            if test(dst):
                crsm_fail("%s is forbidden: %s" % (dst, why))
    k = key.encode("utf-8")
    for name, data in [("modinfo.ini", modinfo.encode("utf-8")), ("README.txt", readme.encode("utf-8"))] + \
                      [(dst, open(src, "rb").read()) for src, dst in files if dst != G_CRSM_CONFIG]:
        if k in data:
            crsm_fail("the API key appears in %s" % name)
    if open(files[-1][0], "rb").read() != config:
        crsm_fail(CRSM_CONFIG_REL + " changed while packaging")
    out = os.path.join(out_dir, "SF6_CRSM_%s.zip" % version)
    write_zip(out, modinfo, readme, files)
    print("  API key only in %s; no identity.json, no Lua, no bridge DLL; needs SF6 Native Core %s" % (G_CRSM_CONFIG, CRSM_MIN_CORE))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True, help="the Slots Loader version")
    ap.add_argument("--game", default=None, help="game folder (for the genuine AMD library), passed to make_fluffy_package.py")
    ap.add_argument("--out", default=os.path.join(REPO, "build", "release"))
    ap.add_argument("--without-crsm", action="store_true", help="skip the Recording Manager (no local crsm/package/config.json)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    core_v = version_in(os.path.join(REPO, "core", "src", "sf6core.cpp"), r'CORE_VERSION\s*=\s*"([^"]+)"')
    cm_v = version_in(os.path.join(REPO, "colormod", "src", "common.cpp"), r'kVersion\s*=\s*"([^"]+)"')

    print("== SF6 Native Core %s" % core_v)
    core_zip = os.path.join(a.out, "SF6_NativeCore_%s.zip" % core_v)
    write_zip(core_zip,
              "name=SF6 Native Core\nversion=%s\ndescription=Shared native-UI core of the SF6 mods (SF6 Color Mod and SF6 Recording Manager need it). Changes nothing on its own.\nauthor=Wael\nNameAsBundle=SF6 Native Core\n" % core_v,
              CORE_README % core_v,
              [(os.path.join(REPO, "build", "core", "sf6core_native.dll"), "reframework/plugins/sf6core_native.dll")])

    print("== SF6 Color Mod %s" % cm_v)
    cm_zip = os.path.join(a.out, "SF6_ColorMod_%s.zip" % cm_v)
    write_zip(cm_zip,
              "name=SF6 Color Mod\nversion=%s\ndescription=Extra costume colours, a colour editor and Drive colours per character and player, from the game's own menus. Requires SF6 Native Core %s or newer.\nauthor=Wael\nNameAsBundle=SF6 Color Mod\n" % (cm_v, core_v),
              COLORMOD_README % (cm_v, core_v),
              [(os.path.join(REPO, "build", "colormod", "SF6_ColorModNative.dll"), "reframework/plugins/SF6_ColorModNative.dll"),
               (os.path.join(REPO, "colormod", "data", "SF6_NativeLocale_data", "locale.json"), "reframework/data/SF6_NativeLocale_data/locale.json")],
              # reframework/color_mods: one folder per character + its usage text (the DLL creates them too if missing)
              dirs=["reframework/color_mods/%s" % n for n in CHARACTERS],
              texts=[("reframework/color_mods/README.txt", open(os.path.join(REPO, "colormod", "data", "color_mods_README.txt"), encoding="utf-8").read().replace("\r\n", "\n").replace("\n", CRLF))])

    print("== SF6 Slots Loader %s" % a.version)
    sl_zip = os.path.join(a.out, "SF6_SlotsLoader_v%s.zip" % a.version)
    cmd = [sys.executable, os.path.join(HERE, "make_fluffy_package.py"), "--version", a.version, "--out", sl_zip]
    if a.game: cmd += ["--game", a.game]
    if subprocess.call(cmd) != 0: sys.exit("make_fluffy_package.py failed")

    zips = {"core": core_zip, "colormod": cm_zip, "slots": sl_zip}
    if not a.without_crsm:
        zips["crsm"] = crsm_package(a.out)
    paths = {k: game_paths(v) for k, v in zips.items()}
    names = list(paths)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            both = paths[names[i]] & paths[names[j]]
            if both: sys.exit("path shared by %s and %s: %s" % (names[i], names[j], sorted(both)))
    print("no game path shared by two packages")

if __name__ == "__main__":
    main()
