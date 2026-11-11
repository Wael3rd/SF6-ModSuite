#!/usr/bin/env python3
"""Zen Mode's rule table, from the Lua it replaces.

profile_mod/zen_rules.lua is the source of truth: it started as the `exclude` table of ZenMode.lua. This script turns it
into profile_mod/zen_rules.inc (C++). A path's category is the one it names (`cat = "..."` inside the path), else the
one the Lua worked out from the names (SCREEN_CATEGORY, label_for_path).

  python tools/zen/gen_zen_rules.py            write the .inc
  python tools/zen/gen_zen_rules.py --check    1 when the .inc is not what the .lua gives
  python tools/zen/gen_zen_rules.py --json     the table as JSON (to compare with the Lua run in the game)
"""
import hashlib, json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "profile_mod" / "zen_rules.lua"
OUT = ROOT / "profile_mod" / "zen_rules.inc"

# ZenMode.lua's SCREEN_CATEGORY: whole elements carry their category by name
SCREEN_CATEGORY = {
    "RankUp": "Rank-up animation", "RankGauge_1P": "Rank gauge", "RankGauge_2P": "Rank gauge",
    "BattleHud_WinCount": "Win streak", "BattleHud_InputTypeIcon": "Control type",
    "BattleHud_ProfileName": "Player name", "BattleHud_TitlePlate": "Player title",
}


def label_for_path(path):
    """ZenMode.lua's label_for_path, test for test, in its order."""
    leaf = path[-1]
    joined = "/".join(path).lower()
    if "nationalflag" in joined: return "National flag"
    if "rankicon" in joined or "rank_num" in leaf.lower() or leaf == "e_text_rank": return "Rank icon"
    if "master" in joined or "scrolllist_chara" in joined or "masterrating" in joined: return "Master rate"
    if "league" in joined or "lp_num" in joined or "_lp" in joined or "leaguepoint" in joined: return "League points"
    if "streak" in joined: return "Win streak"
    if "chance" in joined: return "Rank-up chance"
    if "typeicon" in joined or "inputtype" in joined: return "Control type"
    if "platformicon" in joined: return "Platform icon"
    if "titleplate" in joined or "c_emblem" in joined: return "Player title"
    if "c_circle" in joined or "circleemblem" in joined: return "Team / club"
    if leaf == "e_text_name" or "c_fighter" in joined: return "Player name"
    return leaf


# What `cat = "..."` may name (a typo there would otherwise make a category of its own)
KNOWN = {"National flag", "Rank icon", "Master rate", "League points", "Win streak", "Rank-up chance", "Control type",
         "Platform icon", "Player title", "Team / club", "Player name"} | set(SCREEN_CATEGORY.values())


def parse(text):
    """`local exclude = { Name = { { "a", "b", cat = "C" }, ... }, ... };` -> [(name, [(category or None, [segments])])],
    in the file's order."""
    text = re.sub(r"--[^\n]*", "", text)
    toks = re.findall(r'"[^"]*"|[A-Za-z_][A-Za-z0-9_]*|[{}=,;]', text)
    rest = re.sub(r'"[^"]*"|[A-Za-z_][A-Za-z0-9_]*|[{}=,;]|\s+', "", text)
    if rest: raise SystemExit(f"zen_rules.lua: text not understood: {rest[:40]!r}")
    pos = 0

    def take(want=None):
        nonlocal pos
        if pos >= len(toks): raise SystemExit("zen_rules.lua: ends too soon")
        t = toks[pos]; pos += 1
        if want is not None and t != want: raise SystemExit(f"zen_rules.lua: {want!r} expected, {t!r} found (token {pos})")
        return t

    take("local"); take("exclude"); take("="); take("{")
    owners = []
    while toks[pos] != "}":
        name = take()
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name): raise SystemExit(f"zen_rules.lua: a name expected, {name!r} found")
        take("="); take("{")
        paths = []
        while toks[pos] != "}":
            take("{")
            segs, cat = [], None
            while toks[pos] != "}":
                s = take()
                if s == "cat":
                    take("=")
                    c = take()
                    if not c.startswith('"') or c[1:-1] not in KNOWN: raise SystemExit(f"zen_rules.lua: {name}: cat = {c}: not a category")
                    cat = c[1:-1]
                elif not s.startswith('"'): raise SystemExit(f"zen_rules.lua: {name}: a string expected, {s!r} found")
                else: segs.append(s[1:-1])
                if toks[pos] == ",": pos += 1
            take("}")
            if not segs: raise SystemExit(f"zen_rules.lua: {name}: an empty path")
            paths.append((cat, segs))
            if toks[pos] == ",": pos += 1
        take("}")
        if not paths: raise SystemExit(f"zen_rules.lua: {name}: no path")
        if any(n == name for n, _ in owners): raise SystemExit(f"zen_rules.lua: {name} twice")
        owners.append((name, paths))
        if toks[pos] == ",": pos += 1
    take("}")
    return owners


SEG = re.compile(r"(?:[A-Za-z0-9_]|%d\+?)+")   # the only Lua pattern items the engine knows: plain text, %d, %d+


def build(text):
    owners = parse(text)
    for name, paths in owners:
        if len(paths) > 32: raise SystemExit(f"{name}: more than 32 paths (the walk's mask is 32 bits)")
        for _, p in paths:
            if len(p) > 16: raise SystemExit(f"{name}: a path deeper than 16")
            for s in p:
                if not SEG.fullmatch(s): raise SystemExit(f"{name}: segment {s!r} uses a pattern item the engine does not know")
    table = [(name, [(c or SCREEN_CATEGORY.get(name) or label_for_path(p), p) for c, p in paths]) for name, paths in owners]
    cats = sorted({c for _, rules in table for c, _ in rules})   # table.sort's order: by bytes
    return table, cats


def render(text):
    table, cats = build(text)
    digest = hashlib.sha256(text.replace("\r\n", "\n").encode("utf-8")).hexdigest()
    L = ["// GENERATED by tools/zen/gen_zen_rules.py from profile_mod/zen_rules.lua: do not edit, run the script.",
         f"// zen_rules.lua sha256 (LF): {digest}",
         f"inline constexpr int kCatCount = {len(cats)};",
         "inline constexpr const char* kCatNames[kCatCount] = {"]
    L += [f'    "{c}",' for c in cats]
    L += ["};", ""]
    for oi, (name, rules) in enumerate(sorted(table)):
        for ri, (_, p) in enumerate(rules):
            L.append(f"inline constexpr const char* kPath_{oi}_{ri}[] = {{ " + ", ".join(f'"{s}"' for s in p) + " };")
        L.append(f"inline constexpr Rule kRules_{oi}[] = {{   // {name}")
        for ri, (c, p) in enumerate(rules):
            L.append(f"    {{ {cats.index(c)}, {len(p)}, kPath_{oi}_{ri} }},   // {c}")
        L.append("};")
    L += ["", "// sorted by name", "inline constexpr Owner kOwners[] = {"]
    for oi, (name, rules) in enumerate(sorted(table)):
        L.append(f'    {{ "{name}", kRules_{oi}, {len(rules)} }},')
    L += ["};", f"inline constexpr int kOwnerCount = {len(table)};", f"inline constexpr int kRuleCount = {sum(len(r) for _, r in table)};", ""]
    return "\n".join(L)


def main():
    text = SRC.read_text(encoding="utf-8")
    if "--json" in sys.argv:
        table, cats = build(text)
        print(json.dumps({"categories": cats, "owners": {n: [{"category": c, "path": p} for c, p in r] for n, r in table}}, indent=1))
        return 0
    want = render(text)
    if "--check" in sys.argv:
        have = OUT.read_text(encoding="utf-8").replace("\r\n", "\n") if OUT.exists() else ""
        if have != want:
            print("zen_rules.inc is not what zen_rules.lua gives: run tools/zen/gen_zen_rules.py")
            return 1
        print("zen_rules.inc: up to date")
        return 0
    OUT.write_text(want, encoding="utf-8", newline="\n")
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
