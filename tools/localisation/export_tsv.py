#!/usr/bin/env python3
"""The localisation sheets, UTF-8 TSV: every text of ours and the game's words, ready to paste into a spreadsheet.

  export_tsv.py --out <folder> [--game <game folder>]

  localisation_strings.tsv     module, key, context, max length, English, one column per language, source, status
  localisation_game_words.tsv  the game's own words we show: its message, its GUID, every language (reference)
  localisation_glossary.tsv    the vocabulary the translations use

The game's folder is only read (its message files); without it the game-words sheet holds the English alone."""
import argparse
import json
import os

import game_msgs
from locale_common import GAME_WORDS, HERE, LANGS, MODULES, read_table

STATUS_OURS = "machine, needs native review"
STATUS_GAME = "game's own"


def clean(text):
    return text.replace("\t", " ").replace("\r\n", "\\n").replace("\r", "\\n").replace("\n", "\\n")


def row(cells):
    return "\t".join(clean(str(c)) for c in cells) + "\n"


def strings_sheet(out):
    head = ["module", "key", "context", "max length", "English"] + LANGS[1:] + ["source", "status"]
    n_game = n_ours = 0
    with open(os.path.join(out, "localisation_strings.tsv"), "w", encoding="utf-8", newline="") as f:
        f.write(row(head))
        for module, path in MODULES:
            t = read_table(path)
            ctx = t.get("_context", {})
            for key, en in t["en"].items():
                note = ctx.get(key, {})
                source = "game's own (copied, %s)" % note["game"] if note.get("game") else "ours"
                status = STATUS_GAME if note.get("game") else STATUS_OURS
                if note.get("game"):
                    n_game += 1
                else:
                    n_ours += 1
                f.write(row([module, key, note.get("where", ""), note.get("max", ""), en] + [t[l].get(key, "") for l in LANGS[1:]] + [source, status]))
    return n_game, n_ours


def game_sheet(out, msgs):
    head = ["used by", "key", "where", "message", "GUID", "how", "English"] + LANGS[1:]
    n = 0
    with open(os.path.join(out, "localisation_game_words.tsv"), "w", encoding="utf-8", newline="") as f:
        f.write(row(head))

        def line(used, key, where, msg, guid, how, english):
            m = msgs.get(msg, {}).get("text", {}) if msgs else {}
            f.write(row([used, key, where, msg, guid, how, m.get("en", english)] + [m.get(l, "") for l in LANGS[1:]]))

        game = read_table(GAME_WORDS)
        for w in game["words"]:
            line("Slots Loader", w["key"], w["where"], w["msg"], w["guid"], "at run time, by GUID", w["english"])
            n += 1
        for fr in game["fighters"]:
            line("Slots Loader", "fighter.%d.name" % fr["fid"], "A fighter's name in a line", fr["name_msg"], fr["name_guid"], "at run time, by GUID", fr["english"])
            line("Slots Loader", "fighter.%d.caption" % fr["fid"], "A fighter's name in a grid or a header", fr["caption_msg"], fr["caption_guid"], "at run time, by GUID", fr["english"].upper())
            n += 2
        for module, path in MODULES:
            t = read_table(path)
            for key, note in t.get("_context", {}).items():
                if note.get("game"):
                    line(module, key, note.get("where", ""), note["game"], note.get("guid", ""), "copied into the table" + (", in capitals" if note.get("caps") else ""), t["en"][key])
                    n += 1
    return n


def glossary_sheet(out, msgs):
    with open(os.path.join(HERE, "glossary.json"), encoding="utf-8") as f:
        terms = json.load(f)["terms"]
    tables = {module: read_table(path) for module, path in MODULES}
    head = ["term", "source", "note", "English"] + LANGS[1:]
    with open(os.path.join(out, "localisation_glossary.tsv"), "w", encoding="utf-8", newline="") as f:
        f.write(row(head))
        for t in terms:
            if t.get("keep"):
                f.write(row([t["term"], "never translated", t.get("note", ""), t["term"]] + [t["term"]] * (len(LANGS) - 1)))
            elif t.get("msg"):
                m = msgs.get(t["msg"], {}).get("text", {}) if msgs else {}
                f.write(row([t["term"], "the game: " + t["msg"], t.get("note", ""), m.get("en", t["term"])] + [m.get(l, "") for l in LANGS[1:]]))
            else:
                module, key = t["table"]
                tb = tables[module]
                f.write(row([t["term"], "ours: %s %s" % (module, key), t.get("note", ""), tb["en"].get(key, t["term"])] + [tb[l].get(key, "") for l in LANGS[1:]]))
    return len(terms)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--game", default=game_msgs.DEFAULT_GAME)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    msgs = None
    if os.path.exists(os.path.join(a.game, "re_chunk_000.pak")):
        msgs = game_msgs.load(a.game)
    else:
        print("no game folder: the game's words in English only")
    n_game, n_ours = strings_sheet(a.out)
    n_refs = game_sheet(a.out, msgs)
    n_terms = glossary_sheet(a.out, msgs)
    print("strings: %d ours, %d the game's own copied; game words sheet: %d rows; glossary: %d terms" % (n_ours, n_game, n_refs, n_terms))
    print(a.out)


if __name__ == "__main__":
    main()
