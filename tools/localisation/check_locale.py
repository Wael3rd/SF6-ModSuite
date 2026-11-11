#!/usr/bin/env python3
"""The localisation test: every key in every language, the same placeholders and tags in every language, no text over its stated max,
a context note for every key, every key the code uses in the table, the plugin's generated table up to date. --game [folder]: also
every character of ours appears in the game's own text in that language. Exit 0: all ok."""
import glob
import os
import re
import sys

from gen_plugin_words import generate, OUT
import gen_updater_words
from locale_common import GAME_WORDS, LANGS, MODULES, REPO, read_table, slots, tags, units

FAIL = []
CHECKS = 0


def check(ok, what):
    global CHECKS
    CHECKS += 1
    if not ok:
        FAIL.append(what)


def source(*patterns):
    text = {}
    for pat in patterns:
        for p in glob.glob(os.path.join(REPO, pat), recursive=True):
            with open(p, encoding="utf-8", errors="replace") as f:
                text[p] = f.read()
    return text


def table_checks(module, table):
    en = table.get("en", {})
    check(len(en) > 0, "%s: no English" % module)
    ctx = table.get("_context", {})
    for lang in LANGS:
        t = table.get(lang)
        check(isinstance(t, dict), "%s: no %s block" % (module, lang))
        if not isinstance(t, dict):
            continue
        for k, source_text in en.items():
            text = t.get(k)
            check(isinstance(text, str) and text != "", "%s: %s: missing in %s" % (module, k, lang))
            if not isinstance(text, str):
                continue
            check(slots(text) == slots(source_text), "%s: %s [%s]: placeholders %s, English %s" % (module, k, lang, slots(text), slots(source_text)))
            check(tags(text) == tags(source_text), "%s: %s [%s]: tags differ from English" % (module, k, lang))
            check(text == text.strip() or source_text != source_text.strip(), "%s: %s [%s]: spaces around the text" % (module, k, lang))
            mx = ctx.get(k, {}).get("max")
            if mx:
                check(units(text) <= mx, "%s: %s [%s]: %d units, max %d: %s" % (module, k, lang, units(text), mx, text))
        for k in t:
            check(k in en, "%s: %s [%s]: not in English" % (module, k, lang))
    for k in en:
        check(ctx.get(k, {}).get("where"), "%s: %s: no context note" % (module, k))
    for k in ctx:
        check(k in en, "%s: %s: a context note for no text" % (module, k))


def keys_used_checks():
    # Color Mod: L("key"), and the key tables in its code
    t = read_table(MODULES[1][1])["en"]
    src = source("colormod/src/**/*.cpp", "colormod/src/**/*.hpp")
    for p, text in src.items():
        for key in re.findall(r'\bL\(\s*"([A-Za-z0-9_.]+)"', text):
            check(key.endswith('.') or key in t, "Color Mod: %s uses %s: not in the table" % (os.path.basename(p), key))
        for key in re.findall(r'"(vfx\.[a-z_.]+|quick\.[a-z_]+|slider\.[a-z_]+|di\.[a-z_.]+|side\.p[12])"', text):
            check(key in t, "Color Mod: %s names %s: not in the table" % (os.path.basename(p), key))
    # Recording Manager: T("key") is L("rsm." .. key); Msg{"key", ...}; tr_or("key", ...)
    t = read_table(MODULES[2][1])["en"]
    src = source("crsm/src/**/*.cpp", "crsm/src/**/*.hpp")
    for p, text in src.items():
        if os.path.basename(p) in ("locale.cpp", "locale.hpp", "lang.cpp", "lang.hpp"):
            continue
        for key in set(re.findall(r'\b(?:T|tr|T_|tr_or)\(\s*"([a-z0-9_]+)"', text)) | set(re.findall(r'Msg\{?\s*\{?\s*"((?:com|rsm)_[a-z0-9_]+)"', text)):
            check("rsm." + key in t, "Recording Manager: %s uses %s: not in the table" % (os.path.basename(p), key))


def report_keys_checks():
    # report_mod.dll: its own table holds what its sources ask, and what the shared text dialog asks (T("key") is L("rsm." .. key))
    t = read_table(MODULES[3][1])["en"]
    src = source("report_mod/src/**/*.cpp", "report_mod/src/**/*.hpp")
    src.update(source("crsm/src/ui/dialogs.cpp"))
    rx = r'\b(?:T|tr|T_|tr_or)\(\s*"([a-z0-9_]+)"'
    used = set()
    for p, text in src.items():
        for key in set(re.findall(rx, text)):
            used.add(key)
            check("rsm." + key in t, "Report a bug: %s uses %s: not in the table" % (os.path.basename(p), key))
    rm = read_table(MODULES[2][1])["en"]
    check(not [k for k in rm if k.startswith("rsm.report_")], "Recording Manager: rsm.report_* belongs to report_mod's table now")


def plugin_checks():
    cur = ""
    if os.path.exists(OUT):
        with open(OUT, encoding="utf-8", newline="") as f:
            cur = f.read().replace("\r\n", "\n")
    check(cur == generate(), "plugin: words_table.inc is not up to date: run tools/localisation/gen_plugin_words.py")
    game = read_table(GAME_WORDS)
    guid = re.compile(r"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
    for w in game["words"]:
        check(guid.match(w["guid"]) and w["english"] and w["where"], "plugin: game word %s: incomplete" % w["key"])
    for f in game["fighters"]:
        check(guid.match(f["name_guid"]) and guid.match(f["caption_guid"]) and f["english"], "plugin: fighter %s: incomplete" % f["fid"])
    # the code writes the French Wael decided
    fr = read_table(MODULES[0][1])["fr"]
    check(fr["replay.row_music"] == "Musique des replays", "French: Musique des replays")
    check(fr["replay.custom"] == "Personnalisée", "French: Personnalisée")
    check(fr["replay.custom_list"] == "Liste personnalisée", "French: Liste personnalisée")
    # nothing of the old hard-coded words left in the plugin's menus
    src = source("plugin/src/*.cpp", "plugin/src/*.hpp")
    for p, text in src.items():
        if os.path.basename(p) in ("words.hpp", "words_table.inc"):
            continue
        for line in text.split("\n"):
            code = line.split("//")[0]
            if "logf(" in code:
                continue
            for word in ("Costume options", "Restore defaults", "Stage Variation", "Replay Settings", "Display NSFW Outfits"):
                check('"' + word + '"' not in code, "plugin: %s still writes %s" % (os.path.basename(p), word))


def updater_checks():
    # the updater's generated table (compiled into the core and the plugin) up to date; every Word the host uses is a key
    cur = ""
    if os.path.exists(gen_updater_words.OUT):
        with open(gen_updater_words.OUT, encoding="utf-8", newline="") as f:
            cur = f.read().replace("\r\n", "\n")
    check(cur == gen_updater_words.generate(), "updater: update_words.inc is not up to date: run tools/localisation/gen_updater_words.py")
    keys = read_table(MODULES[4][1])["en"]
    words = {"".join(p[:1].upper() + p[1:] for p in re.split(r"[._-]", k[len("update."):])) for k in keys}
    for p, text in source("updater/src/*.cpp", "updater/src/*.hpp").items():
        for w in set(re.findall(r"\bWord::([A-Za-z]+)", text)):
            check(w == "Count" or w in words, "updater: %s uses Word::%s: not in the table" % (os.path.basename(p), w))


def glyph_checks(game_dir):
    """Every character of ours appears in the game's own text in that language: its fonts for that language draw it (--game)."""
    import game_msgs
    have = {lang: set() for lang in LANGS}
    for m in game_msgs.load(game_dir).values():
        for lang, text in m["text"].items():
            if lang in have:
                have[lang].update(text)
    for module, path in MODULES:
        table = read_table(path)
        for lang in LANGS:
            for k, text in table[lang].items():
                missing = sorted(c for c in set(text) if c not in have[lang] and ord(c) > 32)
                check(not missing, "%s: %s [%s]: no game text in this language uses %s" % (module, k, lang, missing))


def main():
    for module, path in MODULES:
        table = read_table(path)
        table_checks(module, table)
    keys_used_checks()
    report_keys_checks()
    plugin_checks()
    updater_checks()
    for ok, what in __import__("gen_sorter_texts").checks():
        check(ok, "sorter: " + what)
    if "--game" in sys.argv:
        at = sys.argv.index("--game") + 1
        glyph_checks(sys.argv[at] if at < len(sys.argv) else __import__("game_msgs").DEFAULT_GAME)
    for f in FAIL:
        print("FAILED:", f)
    print("%d checks, %d failed" % (CHECKS, len(FAIL)))
    sys.exit(1 if FAIL else 0)


if __name__ == "__main__":
    main()
