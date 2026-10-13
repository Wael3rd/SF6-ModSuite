# SF6 Slots Loader with Fluffy Mod Manager

Since 1.9.3 (beta), costume mods installed with [Fluffy Mod Manager](https://www.fluffyquack.com/) get
outfit slots of their own, like mods dropped in `reframework\costume_mods\`.

## What you get

- **One slot per mod ticked in Fluffy**, even when several mods replace the same outfit. Two Mai
  Outfit 1 mods ticked together give two slots, for example Outfit VII and Outfit VIII.
- **Unticking a mod in Fluffy removes its slot** at the next launch. Ticking one adds it.
- The first launch after a change in Fluffy rebuilds the costume pak: the game window takes up to a
  minute to appear. Launch the game once and wait.

## What does not change

- **The outfit a Fluffy mod replaces still shows that mod.** Fluffy copies the mods it installs into the
  game, and the game reads that copy over everything else. When two mods replace the same outfit, the
  outfit shows the one installed last. To keep the original outfit as well, do not tick the mod in
  Fluffy: put its archive in `reframework\costume_mods\<Character>\` instead.
- Stage mods installed with Fluffy still replace the stage (see [stages/README.md](../stages/README.md)).

## How the loader finds your mods

Fluffy keeps a list of the mods it installed (`Games\SF6\installed.ini` in Fluffy's folder) and the
archives themselves (`Games\SF6\Mods`). At each launch the loader reads that list and takes each mod
from its archive. Archives are unpacked once into `reframework\costume_mods\.cache`, then reused.

The loader looks for Fluffy in:

1. the folder named in `reframework\costume_mods\settings.ini` (see below);
2. the folder where it found Fluffy last time;
3. your Documents, Desktop and Downloads folders, the game folder, the folder above it, and the root of
   each local drive, with their sub-folders.

If your Fluffy is somewhere else, open `reframework\costume_mods\settings.ini` (the loader creates it)
and give the folder that holds `Modmanager.exe`:

```ini
FluffyModManager=D:\Tools\FluffyModManager
```

## Patch day (after a game update)

The slots exist as patch paks the loader writes into the game folder at launch. If the loader is turned off
before Fluffy Mod Manager re-reads the game archives, those paks stay and Fluffy records them as game files
(the slots then show even with every mod off). So, in this order:

1. The game updated: **before** letting Fluffy re-read the archives ("EXE changed") and before turning mods off,
   close the game and run `SF6_SlotsClean.exe` (double-click, in the game folder; `--dry-run` only lists). It removes
   the loader's runtime paks and state files, and nothing else: `reframework\costume_mods`, other mods' data and
   Fluffy's files are not touched.
2. Let Fluffy re-read the game archives.
3. Launch the game: the loader rebuilds the slots from `reframework\costume_mods`.

## Checking it worked

Open `SF6_CostumeLoader.log` in the game folder. Near the top:

```
Fluffy Mod Manager: C:\Users\you\Documents\FluffyModManager (found in 7 ms)
  Mai Racer: Mai-3018-1-1-1764202818.rar / mai racer
  2 mods installed, 2 found in their archives (2 with costumes; 2 archives read in 3 ms)
```

| Log line | Meaning |
|---|---|
| `Fluffy Mod Manager: <folder> (...)` | Fluffy was found there. |
| `Fluffy Mod Manager: not found` | Give its folder in `settings.ini`. |
| `<mod>: its archive is not in Fluffy's Mods folder` | The archive was deleted or renamed after the install: the loader uses the copy in the game, which can be mixed with another mod's files. Put the archive back, or reinstall the mod. |
| `check: colour file ... is not in the game's paks` | The game's pak lacks one of the outfit's colour files (Fluffy can take files out of it). The slot gets the copy in the game folder, or another colour of the outfit. |

`reframework\costume_mods\SF6_CostumeAudit.bat` lists the mods ticked in Fluffy under "Fluffy Mod
Manager (mods ticked in Fluffy, read from their archives)", each with the slots made from it.

## Reporting a problem

Send, with the name or link of the mod:

- `SF6_CostumeLoader.log` (game folder);
- `SF6_CostumeAudit.txt` (made by `SF6_CostumeAudit.bat`);
- `Games\SF6\installed.ini` from Fluffy's folder;
- whether REFramework's loose file loader is on (REFramework menu > LooseFileLoader).

Tested so far with Fluffy 3.084 and REFramework's loose file loader on. With the loose file loader off,
Fluffy changes the game's pak instead: that case is handled but has had little testing.
