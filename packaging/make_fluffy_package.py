# The built files come from this repository (build.bat first: build/amd_ags_x64.dll,
# build/SF6_CostumeSlotsNative.dll), the generated data files and the genuine AMD library from a
# live installation (--game), since neither lives in this repository.
"""Assemble le zip Fluffy du mod de base "SF6 Slots Loader" (costumes + stages).

Contenu (chemins relatifs a la racine du jeu, modinfo.ini a la racine du zip) :
  amd_ags_x64.dll                       proxy AGS + loaders costumes et stages (build/amd_ags_x64.dll ;
                                        --costumes-only : build/costumes_only/amd_ags_x64.dll, sans stages)
  amd_ags_x64_real.dll                  la vraie bibliotheque AMD AGS du jeu (MIT), que le proxy relaie
  reframework/plugins/SF6_CostumeSlotsNative.dll   (tout le jeu : menus, couleurs, alias en ligne, stages)
  reframework/data/SF6_Costumes_Data/loader/vanilla_costume_index.tsv
  reframework/data/SF6_Costumes_Data/loader/static/*      (5 fichiers structurels + static_meta.json)
  reframework/data/SF6_StageSlots_Data/loader/stage_paths.txt   (du depot, fins de ligne LF)
  reframework/costume_mods/<32 personnages>/, reframework/stage_mods/   (vides, avec leur mode d'emploi)
  reframework/costume_mods_nsfw/<32 personnages>/   (vide, avec son mode d'emploi ; le loader le cree aussi)
  reframework/title_mods/               (vide, avec son mode d'emploi : les packs d'ecran titre ; avec stages seulement)
  reframework/ui_mods/                  (vide, avec son mode d'emploi : les mods d'interface ; avec stages seulement)
  reframework/data/SF6_TitleSlots_Data/game_title.jpg   (l'image de l'ecran titre du jeu, pour sa case ; prise dans le jeu --game)

Usage : python make_fluffy_package.py [--version 1.0] [--game "<dossier du jeu>"] [--out "<chemin>.zip"] [--costumes-only]
La vraie AGS est prise dans l'ordre : <jeu>/amd_ags_x64_real.dll, puis reframework/_backup/amd_ags_x64_real_orig_20260922.dll.
"""
import argparse, hashlib, json, os, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)                     # racine du depot
DEFAULT_GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Street Fighter 6"

def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for c in iter(lambda: f.read(1 << 20), b""): h.update(c)
    return h.hexdigest()[:16]

def is_real_ags(p):
    # la vraie lib AMD importe VCRUNTIME/USER32 et ne contient pas nos chaines ; le proxy contient "amd_ags_x64_real"
    b = open(p, "rb").read()
    return b"amd_ags_x64_real" not in b and b"hardread_core" not in b

CHARACTERS = [
    "Ryu", "Luke", "Kimberly", "Chun-Li", "Manon", "Zangief", "JP",
    "Dhalsim", "Cammy", "Ken", "Dee Jay", "Lily", "A.K.I", "Rashid",
    "Blanka", "Juri", "Marisa", "Guile", "Ed", "E. Honda", "Jamie",
    "Akuma", "Sagat", "M. Bison", "Terry", "Mai", "Elena", "C. Viper",
    "Alex", "Ingrid", "Yasmine", "Arjun",
]

CRLF = chr(13) + chr(10)

COSTUME_MODS_TXT = CRLF.join([
    "Drop costume mods here, exactly as downloaded from Nexus:",
    "  reframework/costume_mods/<Character>/<the .zip, .7z or .rar file>",
    "or unpacked, one folder per costume:",
    "  reframework/costume_mods/<Character>/<Costume name>/",
    "",
    "Nothing to unpack, nothing to convert. Restart the game: each costume becomes an EXTRA",
    "outfit slot, shown under the mod's name. Bundles with several options give one slot per option,",
    "and add-on options (no gloves, alternative hair...) are combined with their base outfit.",
    "Modular mods (main files, body options, skin options, optional extras) give one slot whose",
    "options you choose in game (runtime costume options, below). A mod that only",
    "changes an accessory of an original outfit (glasses, earrings) gives that outfit with the",
    "change, in a slot of its own.",
    "Delete the file or folder and restart to remove the slot.",
    "",
    "Costume mods installed with Fluffy Mod Manager work too: each mod ticked in Fluffy gets a slot",
    "of its own, even two mods of the same outfit (the loader reads them from Fluffy's archives).",
    "If SF6_CostumeLoader.log says Fluffy was not found, give its folder in settings.ini (this folder).",
    "",
    "To combine a costume with an add-on that comes in another archive (hair, weapon...), put both",
    "archives in one sub-folder of the character: reframework/costume_mods/<Character>/<any name>/.",
    "",
    "To see what is installed: double-click SF6_CostumeAudit.bat (in this folder). It lists the",
    "costume mods found, where they come from and the outfits made of them, and saves the report",
    "to SF6_CostumeAudit.txt, to send along with SF6_CostumeLoader.log when reporting a problem.",
    "",
    "The first launch after adding a large archive takes a few seconds longer: the archive is",
    "unpacked once into the hidden .cache folder, then reused.",
    "",
    "NSFW outfits: a mod whose name says it is NSFW (nsfw, nude, naked, topless, see through, no",
    "panties...) is handled as if it were in costume_mods_nsfw: hidden in game until \"display_nsfw\"",
    "is true in reframework/data/SF6_CostumeSlots_data/settings.json (or the \"Display NSFW Outfits\"",
    "row of Options > Interface > Other Settings). An option whose name says so is",
    "an NSFW option (not offered, not worn, until then). The word list can be changed in settings.ini",
    "(NsfwKeywords=); the log lists every mod and option it caught (lines starting with \"nsfw:\").",
    "",
    "Runtime costume options (always on): a modular mod or an outfit with add-ons is one outfit",
    "whose options you choose in game: F on the Costume row of Fighter Settings (Battle Settings),",
    "R1 / C in your panel of the character select. The RuntimeOptions= setting of older versions",
    "is gone: a line left in settings.ini is ignored.",
    "",
])

COSTUME_MODS_NSFW_TXT = CRLF.join([
    "NSFW costume mods go here, laid out like costume_mods:",
    "  reframework/costume_mods_nsfw/<Character>/<the .zip, .7z or .rar file, or a folder>",
    "",
    "They become outfits like the others, listed after them, but the game shows them only while",
    "\"display_nsfw\" is true in reframework/data/SF6_CostumeSlots_data/settings.json (false by",
    "default; in game: Options > Interface > Other Settings > Display NSFW Outfits). With it false,",
    "a Battle Settings choice of one goes back to Outfit 1.",
    "Both folders are read as one: an add-on, a retouch or a version from here still goes on the",
    "outfits it fits (the game's included) as an NSFW option, hidden with the NSFW outfits. Only",
    "sub-folders stay apart (a sub-folder here never groups with one of costume_mods).",
    "",
    "Mods left in costume_mods whose name says they are NSFW are handled the same way (see the",
    "LISEZMOI.txt of costume_mods). An empty folder changes nothing.",
    "",
])

STAGE_MODS_TXT = CRLF.join([
    "Drop stage mods here, exactly as downloaded from Nexus: the .zip, .7z or .rar file itself,",
    "a folder with a natives tree, or a .pak. Restart the game. EXPERIMENTAL.",
    "",
    "On the stage select screen of Fighting Ground > Versus, a stage that has mods shows its name",
    "between UP / DOWN symbols: UP / DOWN cycles it through its original look and each mod made for",
    "it. The name and the preview image follow, the VS screen shows the choice, and the battle loads",
    "it. The choice is kept per stage. Each option of a mod gives one variant of the stage it changes.",
    "",
    "The choice is local, and it applies to every load of that stage: training, arcade, replays and",
    "online too (the other player sees their own version). Online play has not been tested. To get",
    "the original back, select it again on the stage select screen.",
    "",
    "To remove a mod, delete its archive or folder: the stage pak is rebuilt at the next launch.",
    "The first launch after adding a large archive takes a few seconds longer: it is unpacked once",
    "into the hidden .cache folder, which can be deleted at any time.",
    "",
    "Stage mods installed with Fluffy Mod Manager still replace the stage (not variants yet). A mod",
    "that hangs the game on its own hangs it here too. When reporting a problem, send",
    "SF6_StageSlots.log from the game folder and the mod link.",
    "",
])

README_TXT = CRLF.join([
    "SF6 Slots Loader v%s",
    "====================",
    "",
    "Costume mods become EXTRA outfit slots instead of replacing an existing one, and the",
    "original outfit stays available. Nothing to do at runtime, nothing to convert.",
    "",
    "Since 1.9 (experimental): stage mods become variants of their stage. On the stage select",
    "screen, UP / DOWN cycles a stage through its original look and each mod installed for it.",
    "Without stage mods, no stage is changed and nothing is hooked.",
    "",
    "Since 1.20: stage mods also show in Battle Settings > Other > Stage Settings and Training's Change Stage",
    "(a mod that is another stage gets its own cell, a lighting mod is a variation cycled with X / R), and",
    "Replays has a Settings tab: the outfit and the stage shown in replays (this PC only).",
    "",
    "Since 1.25: every character has a Random outfit (the last one, previewed as Outfit 1): each battle (each",
    "opponent online) wears an outfit drawn from the character's outfits, never twice before all were drawn, never an",
    "NSFW outfit while they are hidden. Colours made with the Color Mod stay on their outfit when its number changes.",
    "Since 1.26: a Random colour at the end of every Color row, Random in Replays > Settings, and in ranked matches",
    "Random draws only the outfits the player really owns (an unlock-all script or not).",
    "",
    "INSTALL (manual): copy the contents of this archive into the Street Fighter 6 folder,",
    "keeping the directory structure:",
    "  amd_ags_x64.dll             loader (AMD AGS proxy, loaded by the game itself)",
    "  amd_ags_x64_real.dll        the original AMD library, called by the proxy",
    "  SF6_SlotsClean.exe          patch-day clean-up (see PATCH DAY below), run on demand",
    "  reframework/plugins/        native plugin: menus, colours, online, stage select (no Lua script)",
    "  reframework/data/           static tables, vanilla costume index, stage path index",
    "  reframework/costume_mods/   drop your costume mods here: .zip/.7z/.rar as downloaded, or folders;",
    "                              SF6_CostumeAudit.bat there lists what is installed",
    "  reframework/costume_mods_nsfw/  NSFW costume mods: shown in game only once display_nsfw is true",
    "  reframework/stage_mods/     drop your stage mods here, as downloaded",
    "  reframework/title_mods/     drop title screen mods here, as downloaded (Options > Interface > Title Screen Selection)",
    "  reframework/ui_mods/        drop interface mods here, as downloaded (Options > Interface: one section per mod)",
    "",
    "INSTALL (Fluffy Mod Manager): install this zip as a mod, then install costume mods as usual.",
    "Updating from an older version: disable or remove the old one in Fluffy first.",
    "",
    "UPDATES: once per launch the game asks GitHub (github.com) whether a newer SF6 Mod Suite release exists (nothing about",
    "you is sent) and, if so, offers it at the main menu. \"check_for_updates\": false in",
    "reframework/data/SF6_Update_data/state.json turns this off.",
    "",
    "REQUIREMENTS: REFramework in dinput8.dll (official 1.5.8 or newer).",
    "OPTIONAL: SF6 Native Core 0.9.10 or newer (SF6_NativeCore zip). Without it everything else works; the Replays",
    "Settings tab is not added, the stage menus have no Stage Variation hint in the guide bar, and Options > Interface",
    "has no Title Screen Selection (core 0.9.9 needed) and no section for the interface mods (core 0.9.10 needed: without",
    "it the choices already in settings.json still apply at launch).",
    "",
    "CONFLICTS: this DLL loads nothing else. If another tool wants the same entry point",
    "(HARD READ, MatchScout, any amd_ags_x64.dll proxy), rename THAT one to amd_ags_x64_chain.dll",
    "and keep the genuine AMD library as amd_ags_x64_real.dll: both are then loaded, in that order.",
    "If the game already has an amd_ags_x64.dll from another tool, chain it instead of",
    "overwriting it: rename that one to amd_ags_x64_real.dll first.",
    "",
    "The loader rebuilds its patch pak (the re_chunk_000.pak.patch_NNN.pak after those of other",
    "mods) at launch when the costume mods changed, and writes SF6_CostumeLoader.log in the game",
    "folder. That takes a few seconds, up to a few minutes the first time large archives are added",
    "(they are unpacked once); with nothing changed it takes about 30 ms. The game window appears",
    "once it is done. Mods made for older versions of the game are brought up to date on the way;",
    "an outfit the game could not load is left out and named in the log.",
    "",
    "COSTUME OPTIONS (since 1.13, always on): a modular mod or an outfit with its",
    "add-ons is one outfit whose options you choose in game, live, per player: F on the Costume row",
    "of Fighter Settings (Battle Settings), R1 / C in your panel of the character select. The",
    "RuntimeOptions= setting of older versions is gone: a line left in settings.ini is ignored.",
    "",
    "OWNED OUTFITS ONLY (since 1.11): a mod made for a fighter's Outfit 2, 3 or 4 gets its slot only",
    "when the game counts that outfit as yours (default, bought, or given by a DLC; an unlock-all",
    "script counts too). Mods made for Outfit 1 or the Drive Tech outfit always show. Hidden slots are",
    "listed in reframework/data/SF6_CostumeSlots_data/native_log.txt.",
    "",
    "GAME UPDATES (since 1.11): a fighter or an outfit added by an update is picked up from the game",
    "at the next launch, no new Slots Loader release needed. Do the PATCH DAY steps below first.",
    "",
    "PATCH DAY (after a game update): the slots are patch paks the loader writes into the game folder.",
    "If they are still there when Fluffy re-reads the game archives, Fluffy records them as game files",
    "(the slots then show even with every mod off). In this order:",
    "  1. BEFORE Fluffy re-reads the archives (\"EXE changed\") and before turning mods off: close the game",
    "     and run SF6_SlotsClean.exe (double-click, in the game folder; --dry-run only lists). It removes",
    "     the loader's runtime paks and state; reframework/costume_mods and other mods are not touched.",
    "  2. Let Fluffy re-read the game archives.",
    "  3. Launch the game: the loader rebuilds the slots from reframework/costume_mods.",
    "",
    "COLOUR SQUARES (experimental, since 1.7): the two squares next to each colour in the costume",
    "menu are computed from each mod's own colours (its two largest colour zones). Capcom picks its",
    "squares by hand, so they look like the outfit without matching Capcom's choice. This may be removed.",
    "",
])

REPORT_TXT = CRLF.join([
    "REPORT A BUG (reframework/plugins/report_mod.dll; needs the SF6 Native Core, does nothing without it)",
    "  On Battle Settings > Character, Character tab, R (pad: X) takes a picture of the game and opens a menu. Its first row is STATUS: OK shows nothing",
    "  else. BUG shows Describe the problem and Send. A report also holds the picture, the character, outfit and colour with the outfit's record, the versions and list of",
    "  your mods, the Slots Loader's two logs (your Windows user name, the paths of your user folder and anything that looks like a platform ID removed first), and your Steam",
    "  ID, which the server takes from your Steam verification. When the menu opens it only asks the server the state of your report on this outfit (the outfit's key alone",
    "  goes). No report leaves until you choose Send. Back sends nothing and keeps nothing. A report waits as WAITING FOR FIX; once the author fixes it, it shows TO TEST: choose",
    "  OK and press MARK AS FIXED if it is fixed, BUG if not. The server keeps one report per account per outfit for 90 days; only the mod's author reads them. Details: report_mod/README.md in the source.",
    "  Your token is reframework/data/SF6_Community_data/identity.json, the file the Recording Manager uses too: delete it to forget it on this PC.",
    "  To turn this off, delete reframework/plugins/report_mod.dll. A Recording Manager 2.2.8 beside it keeps its own Report a bug, and this DLL stays off.",
    "",
    "",
])

G_REPORT_CONFIG = "reframework/data/SF6_ReportMod_data/config.json"

def report_config():
    """report_mod's config.json: the Worker's address alone, from the local git-ignored config the Recording Manager's is packaged from. No key: its routes need none."""
    path = os.path.join(REPO, "crsm", "package", "config.json")
    if not os.path.exists(path):
        sys.exit("missing crsm/package/config.json (the local, git-ignored Community config: report_mod's address comes from it; never in git), or use --without-report")
    base = json.loads(open(path, "rb").read().decode("utf-8-sig")).get("api_base")
    if not isinstance(base, str) or not base.startswith("https://"):
        sys.exit("crsm/package/config.json has no https api_base")
    return json.dumps({"api_base": base}, indent=2) + "\n"

def manager_contract():
    # The Slotsloader Manager contract the loader implements (MANAGER_CONTRACT in loader_core.cpp,
    # docs/MANAGER_CONTRACT.md): the Manager reads it from this modinfo.ini
    import re
    src = open(os.path.join(REPO, "costumes", "loader", "loader_core.cpp"), encoding="utf-8").read()
    m = re.search(r"MANAGER_CONTRACT\s*=\s*(\d+)", src)
    if not m: sys.exit("MANAGER_CONTRACT introuvable dans loader_core.cpp")
    return int(m.group(1))

UI_MODS_TXT = CRLF.join([
    "Interface mods go here, as downloaded: the .zip, .7z or .rar file itself, or a folder.",
    "(Fluffy-style interface mods: a modinfo.ini and a natives folder, e.g. Fighting Ground artwork, character select",
    "portraits, battle HUD portraits, loading screens, round announcements.)",
    "",
    "Then, in game: Options > Interface. Each mod is a section above Other Settings, with the game's own rows:",
    "  a set of alternatives is one row, Left / Right cycles the game's own files and each option;",
    "  an option on its own is an Off / On row; the picture of the option shows under the rows.",
    "A mod with a portrait per character (battle HUD portraits) is a grid of characters: Confirm on the section,",
    "then X (R on a keyboard) shows the character's next portrait on its cell, A (Confirm) chooses it,",
    "Y gives it to every character that has it, B goes back.",
    "A choice applies at once: a screen shows it the next time it opens. Restore Default Settings puts the game's",
    "own files back. Archives are unpacked once into .cache here.",
    "Needs SF6 Native Core 0.9.10 or newer for the sections (without it, the choices in settings.json apply at launch).",
    "",
]) + CRLF

TITLE_MODS_TXT = CRLF.join([
    "Title screen mods go here, as downloaded: the .zip, .7z or .rar file itself, a folder, or a .pak.",
    "(Fluffy-style packs: a modinfo.ini and a natives folder with the title screen's movies, e.g. the",
    "\"Quick Startup\" packs.)",
    "",
    "Then, in game: Options > Interface > Title Screen Selection. Confirm on it, then:",
    "  A        choose the background (the game's own, a pack, or Random)",
    "  Y        Random Intro Settings: tick the backgrounds Random draws from",
    "  X        edit the OVERDRIVE EDITION line of that background (place, size, shown or not)",
    "  B        back",
    "The choice shows at the next return to the title screen. Archives are unpacked once into .cache here.",
    "Needs SF6 Native Core 0.9.9 or newer for the Title Screen Selection page.",
    "",
]) + CRLF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.0")
    ap.add_argument("--out", default=None)
    ap.add_argument("--game", default=DEFAULT_GAME)
    ap.add_argument("--costumes-only", action="store_true", help="the costume loader alone, without stage slots")
    ap.add_argument("--without-report", action="store_true", help="no report_mod.dll (a clone without the local crsm/package/config.json)")
    a = ap.parse_args()
    GAME = a.game                                # racine du jeu
    REF = os.path.join(GAME, "reframework")
    out = a.out or os.path.join(os.path.expanduser("~"), "Documents", "SF6_SlotsLoader_v%s.zip" % a.version)
    stages = not a.costumes_only

    proxy = os.path.join(REPO, "build", "amd_ags_x64.dll") if stages else os.path.join(REPO, "build", "costumes_only", "amd_ags_x64.dll")
    real = None
    for cand in (os.path.join(GAME, "amd_ags_x64_real.dll"), os.path.join(REF, "_backup", "amd_ags_x64_real_orig_20260922.dll")):
        if os.path.exists(cand) and is_real_ags(cand): real = cand; break
    if real is None: sys.exit("vraie amd_ags_x64.dll introuvable")
    if not os.path.exists(proxy) or is_real_ags(proxy): sys.exit(proxy + " manquant ou pas le proxy (build.bat ?)")
    if stages and b"stageslots-" not in open(proxy, "rb").read():
        sys.exit(proxy + " ne contient pas les stages (build.bat ?)")
    plugin = os.path.join(REPO, "build", "SF6_CostumeSlotsNative.dll")
    loader_dir = os.path.join(REF, "data", "SF6_Costumes_Data", "loader")
    files = [
        (proxy, "amd_ags_x64.dll"),
        (real, "amd_ags_x64_real.dll"),
        (os.path.join(REPO, "build", "SF6_SlotsClean.exe"), "SF6_SlotsClean.exe"),
        (plugin, "reframework/plugins/SF6_CostumeSlotsNative.dll"),
        (os.path.join(loader_dir, "vanilla_costume_index.tsv"), "reframework/data/SF6_Costumes_Data/loader/vanilla_costume_index.tsv"),
    ]
    for n in ("SF6_CostumeAudit.bat", "SF6_CostumeAudit.ps1"):
        files.append((os.path.join(REPO, "costumes", "audit", n), "reframework/costume_mods/" + n))
    for n in sorted(os.listdir(os.path.join(loader_dir, "static"))):
        if n.lower().endswith((".md",)): continue
        files.append((os.path.join(loader_dir, "static", n), "reframework/data/SF6_Costumes_Data/loader/static/" + n))
    report_cfg = None
    if not a.without_report:
        report_cfg = report_config()
        files.append((os.path.join(REPO, "build", "report_mod", "report_mod.dll"), "reframework/plugins/report_mod.dll"))
        files.append((os.path.join(REPO, "report_mod", "data", "SF6_ReportMod_data", "locale.json"), "reframework/data/SF6_ReportMod_data/locale.json"))
    for src, _ in files:
        if not os.path.exists(src): sys.exit("manquant : " + src)
    modinfo = (
        "name=SF6 Slots Loader\n"
        "version=%s\n"
        "description=Turns installed costume mods (Fluffy paks) into EXTRA outfit slots at game launch, keeping the original outfit. Costume mods can also be dropped as downloaded (.zip, .7z, .rar) in reframework/costume_mods/<Character>/. Experimental: stage mods dropped in reframework/stage_mods/ become variants of their stage, UP / DOWN on the stage select screen. Install once; then install mods as usual and restart the game.\n"
        "author=Wael\n"
        "NameAsBundle=SF6 Slots Loader\n"
        "SlotsloaderContract=%d\n" % (a.version, manager_contract()))
    readme = README_TXT % a.version
    if report_cfg:
        readme += REPORT_TXT
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("modinfo.ini", modinfo)
        z.writestr("README.txt", readme)
        for src, dst in files:
            z.write(src, dst)
            print("  %-70s %10d  %s" % (dst, os.path.getsize(src), sha(src)))
        if report_cfg:
            z.writestr(G_REPORT_CONFIG, report_cfg)
            print("  %-70s %10d  %s" % (G_REPORT_CONFIG, len(report_cfg), "the address alone"))
        if stages:
            # the stage path index as the repository stores it (LF), whatever the checkout did to it
            idx = open(os.path.join(REPO, "stages", "data", "loader", "stage_paths.txt"), "rb").read().replace(b"\r\n", b"\n")
            z.writestr("reframework/data/SF6_StageSlots_Data/loader/stage_paths.txt", idx)
            print("  %-70s %10d  %s" % ("reframework/data/SF6_StageSlots_Data/loader/stage_paths.txt", len(idx), hashlib.sha256(idx).hexdigest()[:16]))
            z.writestr("reframework/stage_mods/README.txt", STAGE_MODS_TXT)
            print("  %-70s %10s  %s" % ("reframework/stage_mods/", "-", "structure"))
            # title screen packs: the folder (its presence turns the title packs on: the game's own title then shows the
            # OVERDRIVE EDITION line at its default place) and the game's own title's picture for its cell
            z.writestr("reframework/title_mods/README.txt", TITLE_MODS_TXT)
            print("  %-70s %10s  %s" % ("reframework/title_mods/", "-", "structure"))
            # interface mods: the folder (its presence turns the interface packs on; empty, nothing is changed)
            z.writestr("reframework/ui_mods/README.txt", UI_MODS_TXT)
            print("  %-70s %10s  %s" % ("reframework/ui_mods/", "-", "structure"))
            gt = os.path.join(REF, "data", "SF6_TitleSlots_Data", "game_title.jpg")
            if not os.path.exists(gt): sys.exit("manquant : " + gt)
            z.write(gt, "reframework/data/SF6_TitleSlots_Data/game_title.jpg")
            print("  %-70s %10d  %s" % ("reframework/data/SF6_TitleSlots_Data/game_title.jpg", os.path.getsize(gt), sha(gt)))
        # structure vide livree telle quelle : un dossier par personnage + son mode d'emploi
        z.writestr("reframework/costume_mods/LISEZMOI.txt", COSTUME_MODS_TXT)
        for name in CHARACTERS:
            zi = zipfile.ZipInfo("reframework/costume_mods/%s/" % name)
            zi.external_attr = (0o40755 << 16) | 0x10   # repertoire
            z.writestr(zi, b"")
        print("  %-70s %10s  %s" % ("reframework/costume_mods/<32 personnages>/", "-", "structure"))
        # costume_mods_nsfw : la meme structure (le loader la cree aussi au lancement si elle manque)
        z.writestr("reframework/costume_mods_nsfw/README.txt", COSTUME_MODS_NSFW_TXT)
        for name in CHARACTERS:
            zi = zipfile.ZipInfo("reframework/costume_mods_nsfw/%s/" % name)
            zi.external_attr = (0o40755 << 16) | 0x10   # repertoire
            z.writestr(zi, b"")
        print("  %-70s %10s  %s" % ("reframework/costume_mods_nsfw/<32 personnages>/", "-", "structure"))
    print("zip :", out, os.path.getsize(out), "octets")

if __name__ == "__main__":
    main()
