"""Build the four Fluffy packages of a release (build.bat first):

  SF6_NativeCore_<core version>.zip    reframework/plugins/sf6core_native.dll
  SF6_SlotsLoader_<version>.zip        make_fluffy_package.py (works with or without the core), with report_mod.dll, Report a bug,
                                       which does nothing without the core: its texts and a config.json with the Worker's address alone
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
than every crsm/src file and holding its version string. --without-crsm skips that package, --without-report takes
report_mod.dll out of the Slots Loader's (a clone without the config needs both).

The Worker's address holds a private name: it is in no tracked file. report_mod's config.json gets it from the same local
crsm/package/config.json. Checked on every zip: the key only in the Recording Manager's config.json, the address only there
and in report_mod's, which holds the address and nothing else; no identity.json.

No DLL older than one of its sources goes into a package: each one is checked before the first zip is written.

Usage: python make_suite_packages.py --version <Slots Loader version> [--game "<game folder>"] [--out "<folder>"]
                                     [--without-crsm] [--without-report]
"""
import argparse, hashlib, json, os, re, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CRLF = chr(13) + chr(10)
sys.path.insert(0, HERE)
from make_fluffy_package import CHARACTERS      # the 32 character folders of costume_mods, reused for color_mods
CRSM = os.path.join(REPO, "crsm")
CRSM_MIN_CORE = "0.9.5"       # 0.9.5: on_confirm (Confirm on spin rows, the drill details dialog); 0.8.0: the Native Core's training pause-menu service (pm_*), needed by the Recording Manager
PROFILE_MIN_CORE = "0.8.2"   # ui_clone_control / ui_agents / ui_find_cached, as the Slots Loader's Replays tab
CRSM_OWNERS = ("community", "files", "replay", "reversal", "record", "ui")
CRSM_CONFIG_REL = "crsm/package/config.json"
G_CRSM_CONFIG = "reframework/data/SF6_Community_data/config.json"
G_REPORT_CONFIG = "reframework/data/SF6_ReportMod_data/config.json"
CRSM_FORBIDDEN = [
    (lambda p: p.lower().endswith("sf6_communitybridge.dll"), "SF6_CommunityBridge.dll (the v1.x bridge, replaced by the DLL's WinHTTP)"),
    (lambda p: p.lower().startswith("reframework/data/sf6_nativelocale_data/"), "the Color Mod's locale path (SF6_NativeLocale_data)"),
    (lambda p: p.lower().startswith("reframework/autorun/"), "a Lua script (the package ships the DLL alone)"),
    (lambda p: p.lower().endswith("identity.json"), "identity.json (a player's Community token: never in a package)"),
]

CODE = (".cpp", ".hpp", ".h", ".c", ".cc", ".inc")
HEADERS = (".hpp", ".h", ".inc")

def check_fresh(label, rel_dll, sources, version=None):
    """A DLL older than one of its sources never goes into a zip (the stale DLL of 1.23.0). sources: (a folder or a file, relative to the repo; the extensions looked at in a folder)."""
    dll = os.path.join(REPO, *rel_dll.split("/"))
    if not os.path.exists(dll):
        sys.exit("%s: missing %s: run build.bat first" % (label, rel_dll))
    t = os.path.getmtime(dll)
    for rel, exts in sources:
        top = os.path.join(REPO, *rel.split("/"))
        if os.path.isfile(top):
            walk = [(os.path.dirname(top), [], [os.path.basename(top)])]
        elif os.path.isdir(top):
            walk = os.walk(top)
        else:
            sys.exit("%s: the source %s is gone: update this list" % (label, rel))
        for root, _, names in walk:
            for n in names:
                p = os.path.join(root, n)
                if (os.path.isfile(top) or n.lower().endswith(exts)) and os.path.getmtime(p) > t:
                    sys.exit("%s: %s is older than %s: run build.bat first" % (label, rel_dll, os.path.relpath(p, REPO)))
    if version and version.encode("ascii") not in open(dll, "rb").read():
        sys.exit("%s: %s does not hold the version string %s: run build.bat first" % (label, rel_dll, version))

def local_config():
    """(key, Worker's host) of the local config, as bytes; (None, None) without it (the release checks then have nothing to look for)."""
    path = os.path.join(REPO, *CRSM_CONFIG_REL.split("/"))
    if not os.path.exists(path):
        return None, None
    cfg = json.loads(open(path, "rb").read().decode("utf-8-sig"))
    base = cfg.get("api_base") or ""
    host = base.split("://", 1)[-1].split("/", 1)[0]
    return (cfg.get("api_key") or "").encode("utf-8") or None, host.encode("utf-8") or None

def check_private(zips, with_report):
    """Every zip: the key only in the Recording Manager's config.json; the Worker's address only there and in report_mod's; report_mod's holds that and nothing else. Nothing of it is printed."""
    key, host = local_config()
    for zp in zips:
        name = os.path.basename(zp)
        with zipfile.ZipFile(zp) as z:
            for n in z.namelist():
                if n.endswith("/"):
                    continue
                if n.lower().endswith("identity.json"):
                    sys.exit("%s: %s: a player's token, never in a package" % (name, n))
                data = z.read(n)
                if key and key in data and not (n == G_CRSM_CONFIG and name.startswith("SF6_CRSM_")):
                    sys.exit("%s: %s holds the API key" % (name, n))
                if host and host in data and n not in (G_CRSM_CONFIG, G_REPORT_CONFIG):
                    sys.exit("%s: %s holds the Worker's address" % (name, n))
            has = G_REPORT_CONFIG in z.namelist()
            if has and not name.startswith("SF6_SlotsLoader_"):
                sys.exit("%s: report_mod's config.json belongs to the Slots Loader's package" % name)
            if name.startswith("SF6_SlotsLoader_"):
                dll = "reframework/plugins/report_mod.dll" in z.namelist()
                if dll != with_report or has != with_report:
                    sys.exit("%s: report_mod.dll and its config.json: expected %s" % (name, "both" if with_report else "neither"))
                if has:
                    cfg = json.loads(z.read(G_REPORT_CONFIG).decode("utf-8"))
                    if list(cfg) != ["api_base"] or not (host and host in cfg["api_base"].encode("utf-8")):
                        sys.exit("%s: report_mod's config.json must hold the Worker's address and nothing else" % name)

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
    "It also adds the OVERDRIVE EDITION line under the logo of the title screen. Its position and size are in",
    "reframework/data/sf6core/title.ini, made at the first launch and read when the game starts (pos_x and pos_y from 0 to 1, font_size).",
    "",
    "INSTALL: Fluffy Mod Manager (install this zip as a mod), or copy reframework/ into the game folder.",
    "REQUIREMENTS: REFramework in dinput8.dll, official build 1.5.8 or newer (plugin API 1.10).",
    "LOG: reframework/data/sf6core/sf6core.log",
    "UPDATES: once per launch the game asks GitHub (github.com) whether a newer SF6 Mod Suite release exists (nothing about",
    "you is sent) and, if so, offers it at the main menu. \"check_for_updates\": false in",
    "reframework/data/SF6_Update_data/state.json turns this off.",
    "",
])

COLORMOD_README = CRLF.join([
    "SF6 Color Mod v%s",
    "==================",
    "",
    "Extra costume colours, a colour editor (Fighter Settings > Edit Character, F on the Color row) and",
    "Drive colours per character and per player (character select and Training popup: X).",
    "Copy / paste a whole colour: on the editor's main page, C (pad X) copies every part of the colour, V (pad Y) pastes it",
    "onto the colour being edited, on any outfit of any character (the parts both have).",
    "With the SF6 Slots Loader 1.26 or newer, every Color row ends with Random (a colour drawn at each battle).",
    "",
    "REQUIRES the SF6 Native Core package (%s or newer) from the same release.",
    "Without it the mod stays inert; REFramework's log says: [ColorModNative] sf6core_native.dll not loaded: inert",
    "",
    "UPDATES: once per launch the game asks GitHub (github.com) whether a newer SF6 Mod Suite release exists (nothing about",
    "you is sent) and, if so, offers it at the main menu. \"check_for_updates\": false in",
    "reframework/data/SF6_Update_data/state.json turns this off.",
    "",
    "INSTALL: Fluffy Mod Manager (install this zip as a mod), or copy reframework/ into the game folder.",
    "REQUIREMENTS: REFramework in dinput8.dll, official build 1.5.8 or newer (plugin API 1.10).",
    "",
])

PROFILE_README = CRLF.join([
    "SF6 Profile Mod v%s",
    "===================",
    "",
    "Opens the full Fighter Profile where the game opens the limited one, using only what the server already sent: the",
    "sections it does not send stay empty. This does not bypass anyone's privacy settings.",
    "",
    "Steam name: R / pad X on a profile whose platform line already shows a Steam ID switches that line between the ID and",
    "the player's Steam name. Nothing changes when the profile opens. A line the game hides stays hidden. Only on a press,",
    "that ID is sent to steamcommunity.com to read the public name (a second request for an account with no community",
    "profile). Names are kept in memory for the session, never written to disk or to the log. On tabs where the game uses",
    "that key itself, the mod does nothing.",
    "",
    "Zen Mode (since 1.1.0): hides ranks (icons, gauges, rank-up notices), League Points and Master Rate, win streaks,",
    "player names, titles and clubs, and player details (country flags, platform icons, control types), to play with a",
    "clear mind: in matches, on the versus and result screens, in the menus, profiles, rankings, custom rooms and replays.",
    "Battle Settings has a new tab, Zen Mode: the first row turns it On / Off (Off when installed), five rows choose",
    "Hidden / Shown for each group. A change applies at once and is kept in reframework/data/zen_mode.json. It only",
    "changes what your own game draws: nothing is sent, nothing changes for the other player. During the fight itself",
    "it does no work on the game's thread: the fight's HUD is looked at around it only (a round's start and end, a pause).",
    "Zen Mode NEEDS A REFRAMEWORK NIGHTLY BUILD (plugin API 1.14 or newer; the stable 1.5.9.1 has 1.10). With an older",
    "REFramework the tab does not appear, REFramework's log says \"Zen Mode off\", and the rest of this mod works.",
    "If you used the ZenMode.lua script: its settings are taken at the first start (Zen Mode then starts On); remove",
    "reframework/autorun/ZenMode.lua, the two together do the same work twice.",
    "",
    "Switches (reframework/data/profile_mod.ini, read at start; the file is not shipped, create it to use them):",
    "  full_profile=0   keeps the game's limited profile",
    "  steam_name=0     no Steam name key, no request",
    "",
    "REQUIRES the SF6 Native Core package (%s or newer) from the same release. Without it the mod does nothing",
    "(REFramework's log says: profile_mod: off).",
    "",
    "UPDATES: once per launch the game asks GitHub (github.com) whether a newer SF6 Mod Suite release exists (nothing about",
    "you is sent) and, if so, offers it at the main menu. \"check_for_updates\": false in",
    "reframework/data/SF6_Update_data/state.json turns this off.",
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
    ap.add_argument("--without-report", action="store_true", help="no report_mod.dll in the Slots Loader's package (no local crsm/package/config.json)")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    core_v = version_in(os.path.join(REPO, "core", "src", "sf6core.cpp"), r'CORE_VERSION\s*=\s*"([^"]+)"')
    cm_v = version_in(os.path.join(REPO, "colormod", "src", "common.cpp"), r'kVersion\s*=\s*"([^"]+)"')
    pm_v = version_in(os.path.join(REPO, "profile_mod", "profile_mod.cpp"), r'kVersion\s*=\s*"([^"]+)"')
    # the Slots Loader's version is what its plugin reports to the in-game updater (sf6suite_package_version)
    sl_v = version_in(os.path.join(REPO, "plugin", "src", "version.hpp"), r'kPackageVersion\s*=\s*"([^"]+)"')
    if sl_v != a.version:
        sys.exit("Slots Loader: plugin/src/version.hpp says %s, --version %s: set kPackageVersion to the release's version" % (sl_v, a.version))

    # what build.bat compiles for each DLL: nothing stale goes into a zip
    common = ("common", CODE)
    core_api = ("core/include", HEADERS)
    check_fresh("Native Core", "build/core/sf6core_native.dll", [("core/src", CODE), core_api, ("updater/src", CODE)], core_v)
    check_fresh("Slots Loader (proxy)", "build/amd_ags_x64.dll", [("costumes/loader", CODE), ("stages/loader", CODE), ("proxy", CODE), common])
    updater = ("updater/src", CODE)        # the in-game updater, in the core and the plugin
    check_fresh("Slots Loader (plugin)", "build/SF6_CostumeSlotsNative.dll", [("plugin/src", CODE), ("plugin/include", HEADERS), core_api, updater], sl_v)
    check_fresh("Color Mod", "build/colormod/SF6_ColorModNative.dll", [("colormod/src", CODE), common, core_api, ("costumes/loader", HEADERS)], cm_v)
    check_fresh("Profile Mod", "build/profile_mod/profile_mod.dll", [("profile_mod", CODE), core_api])
    if not a.without_report:
        rm_v = version_in(os.path.join(REPO, "report_mod", "src", "host.cpp"), r'kVersion\s*=\s*"([^"]+)"')
        check_fresh("Report a bug", "build/report_mod/report_mod.dll", [("report_mod/src", CODE), ("crsm/src", CODE), ("plugin/src/scroll_logic.hpp", CODE), core_api], rm_v)

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
              dirs=["reframework/color_mods/%s" % n for n in CHARACTERS] + ["reframework/color_mods/all_chars"],
              texts=[("reframework/color_mods/README.txt", open(os.path.join(REPO, "colormod", "data", "color_mods_README.txt"), encoding="utf-8").read().replace("\r\n", "\n").replace("\n", CRLF))])

    print("== SF6 Profile Mod %s" % pm_v)
    pm_zip = os.path.join(a.out, "SF6_ProfileMod_%s.zip" % pm_v)
    write_zip(pm_zip,
              "name=SF6 Profile Mod\nversion=%s\ndescription=Opens the full Fighter Profile where the game opens the limited one (only what the server sent), swaps a shown Steam ID for the player's Steam name on a key press, and adds Zen Mode (a Battle Settings tab hiding ranks, points, win streaks and player details; needs a REFramework nightly build). Requires SF6 Native Core %s or newer.\nauthor=Wael\nNameAsBundle=SF6 Profile Mod\n" % (pm_v, PROFILE_MIN_CORE),
              PROFILE_README % (pm_v, PROFILE_MIN_CORE),
              [(os.path.join(REPO, "build", "profile_mod", "profile_mod.dll"), "reframework/plugins/profile_mod.dll")])

    print("== SF6 Slots Loader %s" % a.version)
    sl_zip = os.path.join(a.out, "SF6_SlotsLoader_v%s.zip" % a.version)
    cmd = [sys.executable, os.path.join(HERE, "make_fluffy_package.py"), "--version", a.version, "--out", sl_zip]
    if a.game: cmd += ["--game", a.game]
    if a.without_report: cmd += ["--without-report"]
    if subprocess.call(cmd) != 0: sys.exit("make_fluffy_package.py failed")

    zips = {"core": core_zip, "colormod": cm_zip, "profile": pm_zip, "slots": sl_zip}
    if not a.without_crsm:
        zips["crsm"] = crsm_package(a.out)
    check_private(zips.values(), not a.without_report)
    print("key and address: only in their config.json files; no identity.json")
    paths = {k: game_paths(v) for k, v in zips.items()}
    names = list(paths)
    for i in range(len(names)):
        for j in range(i + 1, len(names)):
            both = paths[names[i]] & paths[names[j]]
            if both: sys.exit("path shared by %s and %s: %s" % (names[i], names[j], sorted(both)))
    print("no game path shared by two packages")
    if not a.without_report:
        print("Report a bug: report_mod.dll in the Slots Loader's package, no key, the address only in its config.json")

if __name__ == "__main__":
    main()
