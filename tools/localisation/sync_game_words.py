#!/usr/bin/env python3
"""The Color Mod's and the Recording Manager's texts that are the game's own words (a "game" note in _context): copies of the game's
message, in every language, refreshed from its paks. --check: only says what differs (exit 1), writes nothing.
The Slots Loader needs none of this: it asks the game by GUID at run time (plugin/locale/game_words.json)."""
import argparse
import sys

import game_msgs
from locale_common import LANGS, MODULES, read_table, write_table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=game_msgs.DEFAULT_GAME)
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    msgs = game_msgs.load(a.game)
    bad = 0
    for module, path in MODULES:
        table = read_table(path)
        changed = 0
        for key, note in table.get("_context", {}).items():
            msg = note.get("game")
            if not msg:
                continue
            m = msgs.get(msg)
            if not m:
                print("%s: %s: the game has no message %s" % (module, key, msg))
                bad += 1
                continue
            for lang in LANGS:
                text = (m["text"].get(lang) or m["text"]["en"]).replace("<PLUNUM>", "%d")   # the game's own number slot
                if note.get("caps"):
                    text = text.upper()
                if table[lang].get(key) != text:
                    if a.check:
                        print("%s: %s [%s]: %r, the game says %r" % (module, key, lang, table[lang].get(key), text))
                        bad += 1
                    table[lang][key] = text
                    changed += 1
        if changed and not a.check:
            write_table(path, table)
            print("%s: %d texts refreshed" % (module, changed))
    if a.check and bad:
        sys.exit("%d texts differ from the game's" % bad)
    print("game words: up to date" if a.check else "done")


if __name__ == "__main__":
    main()
