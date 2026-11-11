#!/usr/bin/env python3
"""The modules' tables, and how their room is counted. Shared by check_locale.py, sync_game_words.py and export_tsv.py."""
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
LANGS = ["en", "fr", "ja", "zh-Hans", "it", "de", "es", "es-419", "ru", "pl", "pt", "ko", "zh-Hant", "ar"]
# module name, table, the sources read to check that every key used exists
MODULES = [
    ("Slots Loader", os.path.join(REPO, "plugin", "locale", "locale.json")),
    ("Color Mod", os.path.join(REPO, "colormod", "data", "SF6_NativeLocale_data", "locale.json")),
    ("Recording Manager", os.path.join(REPO, "crsm", "data", "SF6_CRSM_data", "locale.json")),
    ("Report a bug", os.path.join(REPO, "report_mod", "data", "SF6_ReportMod_data", "locale.json")),
    ("Updater", os.path.join(REPO, "updater", "locale", "locale.json")),
]
GAME_WORDS = os.path.join(REPO, "plugin", "locale", "game_words.json")


def read_table(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_table(path, table):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(table, f, ensure_ascii=False, indent=2)
        f.write("\n")


def is_wide(cp):
    return (0x1100 <= cp <= 0x115F or 0x2E80 <= cp <= 0xA4CF or 0xAC00 <= cp <= 0xD7A3 or 0xF900 <= cp <= 0xFAFF or
            0xFE30 <= cp <= 0xFE6F or 0xFF00 <= cp <= 0xFF60 or 0xFFE0 <= cp <= 0xFFE6 or 0x20000 <= cp <= 0x3FFFF)


SLOT = re.compile(r"%(?:%|[-0]*\d*[sd])")
TAG = re.compile(r"<[^<>]*>")


def units(text):
    """The room a text takes, as words.hpp counts it: 2 for a CJK, kana, Hangul or full-width character, 1 for any other;
    a slot counts for what it usually holds (%d: 2, %s: 8), a tag for nothing."""
    text = TAG.sub("", text)
    total = 0
    pos = 0
    for m in SLOT.finditer(text):
        total += sum(2 if is_wide(ord(c)) else 1 for c in text[pos:m.start()])
        total += 2 if m.group(0).endswith("d") else 8 if m.group(0).endswith("s") else 1
        pos = m.end()
    return total + sum(2 if is_wide(ord(c)) else 1 for c in text[pos:])


def slots(text):
    return [m for m in SLOT.findall(text) if m != "%%"]


def tags(text):
    return sorted(TAG.findall(text))
