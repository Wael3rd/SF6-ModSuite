Drop colour mods here, exactly as downloaded from Nexus:
  reframework/color_mods/<Character>/<the .zip, .7z or .rar file>
or unpacked:
  reframework/color_mods/<Character>/<Mod folder>/
or the loose files (esfNNN_CCC_cmd_KKK.user.2) in any folder below this one.

The character folder is only for your own tidiness: the fighter and the costume come from the
file names (esf001 = Ryu, 001 = Outfit 1...). Files placed directly in this folder work too.

Archives are unpacked once, in the background, a few seconds after the game starts, into the
hidden folder .cache (only the colour files are kept). The new colours appear in the colour list
of the character as soon as you are out of a fight, the character select and the colour editor;
otherwise at the next launch. Delete the archive or the folder to remove its colours.

Colours from this folder are listed as RW n. The colour editor can edit them (SAVE keeps your
change apart, RESET goes back to the original) but never changes or deletes a mod's file.
The colours you make in the editor (SAVE AS, listed as MC n) are saved here too, in
<Character>/MANUALCOLORS_01, _02...; ERASE deletes them.
Colours of older versions (reframework/data/SF6_ColorSpinExtra_data/colors) were moved here
by the Color Mod; that folder is no longer read.
Folders and files whose name starts with a dot are ignored.
Messages are in reframework/data/SF6_ColorModNative_data/debug_log.json ("color_mods:").
