# fastpak_test: the costume pak, byte for byte

Offline checks of the costume loader's pak builder (`costume_loader.exe`) on a copy of a game folder
(the "lab": `reframework\costume_mods`, `reframework\data\SF6_Costumes_Data`, a `dlc` folder or a
junction to the game's, no `re_chunk_000.pak`: it is passed with `--base-pak`).

## pak_manifest.py

The SHA-256 of a whole pak and, per entry (table order), its hash, offset, sizes, attributes and the
SHA-1 of its data.

```
python pak_manifest.py <pak> -o before.txt
python pak_manifest.py --compare before.txt <pak or manifest>
```

`--compare` prints whether the whole files are identical and whether the entries are (offsets left
out), and exits 0 only when both are. Used to prove that a loader change leaves its pak unchanged:
build with the previous loader, keep the manifest, build with the new one from the same registry
state (blank the `"fingerprint"` of `registry.json` to force a build), compare.

## incremental_test.ps1

IncrementalPak (the next build takes the files that did not change from the pak in place, see
`pak_sources.tsv` in `loader_core.cpp`) against a full build. For each scenario: a full build makes
the previous pak; the scenario changes the mods; an incremental build and a full build start from the
same registry state; their paks must be identical byte for byte, and the files written beside them
too.

```
powershell -File incremental_test.ps1 -Exe build\costume_loader.exe -Lab <lab> -BasePak <game>\re_chunk_000.pak `
    -Work <empty folder> -TouchFile <an archive of costume_mods> -ByteFile <a loose mod file the slots use> `
    -RemoveFile <another archive> [-Scenarios none,touch,byte,remove,byte_on]
```

Scenarios: `none` (nothing changed: the whole pak comes from the previous one), `touch` (an archive's
date changes: extracted again, its files read again), `byte` (one byte of a loose mod file changes, with
its date), `remove` (an archive taken out: the slots after it move). Runtime options are always on in these builds.
Every change is undone and the lab's data folder put back at the end; the lab's
pak is the last one built. Needs about three times the pak's size free on the lab's disk; three builds
per scenario. Exit code 0 when every scenario is identical.
