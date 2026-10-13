"""
make_vanilla_index.py - convert the vanilla costume inventory JSON into the
loader's vanilla_costume_index.tsv (read by VanillaIndex::load in loader_core.cpp).

Usage:
    python make_vanilla_index.py <vanilla_costume_paths.json> <vanilla_costume_index.tsv>

Sections (tab separated, LF line endings, UTF-8 without BOM):
    #SUFFIXES   ext <TAB> version
    #COSTUMES   fighter <TAB> fighter_dir <TAB> costume_no <TAB> model_dir <TAB> scene
    #FILES      HASH(16 hex, uppercase) <TAB> fighter <TAB> costume_no <TAB> kind <TAB> path
"""

import json
import sys


def build_tsv(inv):
    lines = ["#SUFFIXES"]
    for ext, suffix in sorted(inv["version_suffixes"].items()):
        # JSON holds ".ext.VERSION"; the TSV keeps only VERSION.
        prefix = f".{ext}."
        if not suffix.startswith(prefix):
            raise ValueError(f"unexpected suffix for {ext}: {suffix!r}")
        lines.append(f"{ext}\t{suffix[len(prefix):]}")

    lines.append("#COSTUMES")
    for c in inv["costumes"]:
        lines.append(f"{c['fighter']}\t{c['fighter_dir']}\t{c['costume_no']}\t{c['model_dir']}\t{c['scene']}")

    lines.append("#FILES")
    for c in inv["costumes"]:
        for f in c["files"]:
            if not f.get("in_pak", True):
                continue
            lines.append(f"{f['hash']:016X}\t{c['fighter']}\t{c['costume_no']}\t{f['kind']}\t{f['path']}")
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    with open(sys.argv[1], "r", encoding="utf-8") as fh:
        inv = json.load(fh)
    if inv.get("unresolved"):
        print(f"[WARN] {len(inv['unresolved'])} unresolved entries in the inventory:")
        for k, v in list(inv["unresolved"].items())[:10]:
            print(f"  {k}: {v}")
    with open(sys.argv[2], "w", encoding="utf-8", newline="\n") as fh:
        fh.write(build_tsv(inv))
    print(f"Wrote {sys.argv[2]}")


if __name__ == "__main__":
    main()
