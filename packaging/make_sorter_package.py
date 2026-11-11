"""Build the Mod Sorter's zip of a release (tools\\sorter\\build.bat first):

  SF6_ModSorter_<version>.zip   SF6_ModSorter.exe, with a modinfo.ini and a README.txt: unzipped anywhere, or installed as a Fluffy
                                package (the exe lands in the game folder)

It is a tool, not a package of the updater: make_manifest.py lists it under the manifest's "tools" (never among the packages),
where the sorter reads that a newer one is out and updates itself; make_bundle.py puts its exe in the all-in-one. The version is
tools/sorter/version.hpp's (compiled into the exe, raised by hand when the sorter changes): this zip takes it.
Checked, the script stops: the exe older than one of its sources; the exe answering another version than version.hpp's (--version);
a --version given that is not that one.

Usage: python make_sorter_package.py [--version <sorter version>] [--out "<folder>"]
"""
import argparse, hashlib, os, re, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CRLF = chr(13) + chr(10)
EXE = os.path.join(REPO, "build", "SF6_ModSorter.exe")
VERSION_HPP = os.path.join(REPO, "tools", "sorter", "version.hpp")
SOURCES = ("tools/sorter", "updater/src", "common", "costumes/loader", "stages/loader")   # what tools\sorter\build.bat compiles
CODE = (".cpp", ".hpp", ".h", ".c", ".cc", ".inc")

README = CRLF.join([
    "SF6 Mod Sorter v%s",
    "====================",
    "",
    "A small Windows program, outside the game: drop the mods you downloaded on it, it says what each one is and copies it",
    "into the right folder of the game. Part of SF6 Mod Suite (https://github.com/Wael3rd/SF6-ModSuite).",
    "",
    "WHAT IT TAKES: mods as downloaded: archives (.zip, .7z, .rar), folders, .pak files, colour files. Nothing is unpacked by",
    "hand and nothing is ever run from an archive. Your downloads are copied, never moved or changed.",
    "",
    "WHERE EACH MOD GOES (in the game folder):",
    "  Costumes         reframework/costume_mods/<Character>   (costume_mods_nsfw when you answer NSFW: Yes)",
    "  Stages           reframework/stage_mods",
    "  Interface packs  reframework/ui_mods",
    "  Title packs      reframework/title_mods",
    "  Colours          reframework/color_mods/<Character>",
    "A mod it does not recognise is not placed, and it says why. A mod already there: you choose Skip or Replace.",
    "",
    "TABS: To install (drop, check, Install all), Installed (what is in the game folder, by category, with the mods' pictures;",
    "a costume can be marked or unmarked NSFW there), Options (the language, 14 of them, this tool's updates and SF6 Mod",
    "Suite's).",
    "",
    "SF6 MOD SUITE UPDATES (Options tab): when the tool starts it reads the latest SF6 Mod Suite release on GitHub and compares",
    "it with the packages in the game folder. A newer release is shown with each package old -> new; F installs it, with the",
    "game closed, the way the game's own update box does (same checks, same all-or-nothing swap); the row shows the",
    "download's percentage. Packages installed before suite 1.32.0 report no version: update those by hand once. With",
    "Fluffy Mod Manager, replace the zip there too.",
    "",
    "THIS TOOL'S UPDATES (Options tab): the same look-up says when a newer Mod Sorter is out. F downloads it (size and",
    "sha256 checked), replaces SF6_ModSorter.exe where it is and starts it again. Version 1.0.0 cannot do it: replace it by",
    "hand once.",
    "",
    "INSTALL: unzip anywhere and run SF6_ModSorter.exe (it finds the game through Steam; G chooses another folder), or install",
    "this zip with Fluffy Mod Manager (the exe lands in the game folder).",
    "REQUIREMENTS: Windows 10 or 11, 64-bit. The SF6 Slots Loader installed in the game folder: costumes and stages are",
    "recognised with its indexes.",
    "WINDOWS WARNING: the program is not signed. Windows SmartScreen may ask before the first run (More info, Run anyway);",
    "with Smart App Control on, Windows may refuse it.",
    "NETWORK: github.com and its download servers only, over HTTPS, for the update check; nothing about you or your PC is sent.",
    "It is off when \"check_for_updates\" is false in reframework/data/SF6_Update_data/state.json (F still checks when asked).",
    "SETTINGS: %%APPDATA%%/SF6_ModSorter/settings.ini (the game folder, the language).",
    "",
])


def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


def check_fresh():
    if not os.path.exists(EXE):
        sys.exit("missing: %s (run tools\\sorter\\build.bat)" % EXE)
    t = os.path.getmtime(EXE)
    for rel in SOURCES:
        for root, dirs, names in os.walk(os.path.join(REPO, *rel.split("/"))):
            dirs[:] = [d for d in dirs if d != "third_party"]
            for n in names:
                p = os.path.join(root, n)
                if n.lower().endswith(CODE) and os.path.getmtime(p) > t:
                    sys.exit("SF6_ModSorter.exe is older than %s: run tools\\sorter\\build.bat" % os.path.relpath(p, REPO))


def source_version():
    m = re.search(r'kSorterVersion\s*=\s*"([0-9.]+)"', open(VERSION_HPP, encoding="utf-8").read())
    if not m:
        sys.exit("no kSorterVersion in " + VERSION_HPP)
    return m.group(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default=None, help="optional: checked against tools/sorter/version.hpp")
    ap.add_argument("--out", default=os.path.join(REPO, "build", "release"))
    a = ap.parse_args()
    version = source_version()
    if a.version and a.version != version:
        sys.exit("--version %s, but tools/sorter/version.hpp says %s: raise it there, rebuild" % (a.version, version))
    check_fresh()
    said = subprocess.run([EXE, "--version"], capture_output=True, text=True, timeout=30).stdout.strip()
    if said != version:
        sys.exit("SF6_ModSorter.exe --version answers %r, version.hpp says %s: run tools\\sorter\\build.bat" % (said, version))
    modinfo = ("name=SF6 Mod Sorter\nversion=%s\ndescription=Drop downloaded mods on it: costumes, stages, interface packs, title packs "
               "and colours each go to their folder of the game. Also checks for SF6 Mod Suite updates. A program outside the game: "
               "SF6_ModSorter.exe in the game folder.\nauthor=Wael\nNameAsBundle=SF6 Mod Sorter\n" % version)
    out = os.path.join(a.out, "SF6_ModSorter_%s.zip" % version)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("modinfo.ini", modinfo)
        z.writestr("README.txt", README % version)
        z.write(EXE, "SF6_ModSorter.exe")
    print("== SF6 Mod Sorter %s" % version)
    print("  %-30s %10d  %s" % ("SF6_ModSorter.exe", os.path.getsize(EXE), sha(EXE)))
    print("zip:", out, os.path.getsize(out), "bytes")


if __name__ == "__main__":
    main()
