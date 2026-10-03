# Compatibility

Costume mods are made for the game as it was on the day they were published. Capcom has updated the
game many times since June 2023, and a mod that loaded then can fail now. Most of these failures are
not visible as errors: the game waits forever for a resource it cannot load, the preview stays on the
previous character, and every costume shown afterwards stays empty.

The loader therefore does two things at every launch where something changed:

1. **It repairs what is known to go wrong**, in the files it writes to its own pak. The mod files on
   disk are never modified.
2. **It checks every slot the way the game will load it** before writing anything. A part that cannot
   load is rebuilt from the mod's shared version of that part, or from the original outfit; a slot
   that still cannot load is left out rather than shipped, and `SF6_CostumeLoader.log` says which mod
   and why.

## What the loader fixes on its own

| Problem found in a mod | Symptom in game | What the loader does |
|---|---|---|
| Material files point at textures by their original name, the textures move with the slot | Costume white or untextured | Rewrites those references to the relocated names |
| No material file shipped for a part | Wrong textures, see-through body | Takes the original material of **that part** and binds it to the mod's textures |
| Files only the mod has, stored in another outfit's folder | Loading never completes | Keeps them at their original path as well |
| Weapons and props listed outside the costume scene (`model_parts`) | Original weapons on a modded outfit | Gives the slot its own copy, pointing at the mod's meshes |
| Textures with the pre-2024 file suffix (`.tex.143230113`) | Textures ignored | Reads them as the current version |
| Colour files whose type signature predates a game update | Whole costume white, hair included | Brings the signature up to date (the layout did not change) |
| Colour files in the 2023 layout (no fur block per garment) | Whole costume white | Converts them to the current layout, with the game's own disabled fur block |
| Texture whose mip table declares a wrong row pitch | Loading never completes | Rewrites the pitch (the image data is left as is) |
| Texture whose small levels were sized with fractional blocks (6144x6144 BC7: the 6x6 level declared 36 bytes, 64 written) | The outfit's own part replaced by the original one (Feixue for Mai lost its body, and with it its face) | Rewrites the whole mip table when the table computed from the format fills the file exactly |
| Mesh whose materials its material file does not define | Loading never completes | Pairs the mesh with another material file of the outfit that defines its materials (a part hidden behind a stripped mesh of another part); else rebuilds the part from the mod's shared `000/` part, else from the original outfit |
| Mod whose own costume scene points a part at another folder (Mummy Dhalsim's head in `0L/`) | The original part shown (Dhalsim's own head) | Takes over the parts the mod's scene moved, when the file exists and the scene matches the game's |
| A part a mod made for an older layout of the game, in a folder the outfit's scene no longer uses (Aria for Ingrid hides her face and hair in Outfit 1's folders, Drive Tech takes them from Outfit 2's) | The mod's part is never shown | The slot takes the mod's part of the same number, with the original material file of the place it replaces, and the scene points at it (new in 1.8, to confirm in game) |
| A mod with a real costume and, next to it, a file or two for another outfit (Athena for Ingrid: one mesh for Drive Tech) | A second entry that is the other outfit almost untouched | The small entry is dropped: it is not an outfit of its own (new in 1.8) |
| Physics files next to the parts (`esf032_001_02_chain.chain`), where the game keeps them, rather than in a part folder | The new hair or cloth moves with the original outfit's physics, or not at all | Reads the part from the file name and points the slot's physics settings at the mod's files |
| Colour files, colour-variation data or high-resolution textures a mod does not ship, found among the files of another mod | An outfit shows the colours or textures of another mod, depending on which mods are installed (Cammy) | A slot only uses its own mod's files, else the game's |
| Texture references with doubled slashes or backslashes | The outfit waits forever for a texture that moved, or shows the original outfit | The references follow the texture to its slot |
| Files placed directly in `product/model/` (`1.tex`) | A texture named by the materials is missing, the outfit is replaced by the original | Taken from folder mods: the game keeps none there |
| Material file made for an older layout of the material (every material one parameter short of the game's) | Dark body, opaque fabric, wrong colours | Rebuilt on the game's file for that part, keeping the mod's textures, flags and values |
| Add-on made for another outfit's folder (a hair for Outfit 1), put with a costume in one sub-folder | The add-on becomes an outfit of its own or is ignored | Its parts go to the costume when it has none of that part; only the combined outfit is made |
| Model folder shared by two outfits of the character (Dhalsim) | Another outfit shown instead of the mod | Maps the mod to the outfit that owns the folder (the lowest costume number) |
| Character played on trial, or DLC unlocked by a script | 100 empty outfits, loading stalls on the first one | Only the slots in use are declared to the game |
| A Street Fighter 6 still closing holds the old pak | Old costumes kept for good | Waits for it, and if it does not let go, changes nothing and retries at the next launch |
| A mod is removed while the game still remembers its outfit (last choice, replay) | Loading stalls on the removed outfit | Every unused slot number shows the character's Outfit 1 instead |
| Slots keep their number after a mod is removed | Gaps in the names (Outfit I, III, IV) | Names follow each other per character (Outfit I, II, III); the numbers behind them stay |
| Modular mod built for Fluffy (main files, "pick one" groups, optional extras) | Every option combined with every other: over a hundred outfits, the character full | One outfit per shape option (body, shoes, jacket), first texture option (skin), optional parts left out, as Fluffy installs by default |
| Options of one mod replacing the same files | Two options on the same slot | Each option gets its own identity; outfits made of exactly the same files are installed once |
| A character with more than 100 outfits | Outfits past the 100th silently missing | Named in the log |
| Mod that only changes a part of an original outfit (glasses, earrings) | Ignored | The original outfit with the change, in a slot of its own; the original stays as it is |
| Textures of a part shipped without its material, or used by the material of another part (a retextured head, a hood that is part of the hair and uses the body's cloth, eyes in the shared `000/` folder) | That part keeps the original look | The slot gets its own copy of every original material that uses a texture of the mod, bound to it (since 1.9.0) |
| Wind settings of the mod (`weather/wind/.../*_chain_BattleSetting.chain`) | Cloth and hair move with the original outfit's wind | The slot's scene follows every file the slot has under its own name (since 1.9.0) |
| Newer colour files in the DLC paks (Outfit 1 of JP, Dhalsim, Lily, Guile) | Slot built from the older colours | The game's paks are read in the game's order (since 1.9.0) |

Every check is measured against the game's own files: the mesh and material rule holds for all 362
original costume parts, the texture rule for all 7,084 original costume textures, and the colour-file
reader rewrites all 1,530 original colour files byte for byte.

**Owning the original outfit is not needed**, for the outfit a mod replaces or for parts it takes from other
outfits: the files of every outfit are in the game's base pak, which every player has (the DLC paks, installed
for their owners, only hold newer colour files of a few Outfit 1s). When a part of a mod is missing in its
slot, `SF6_CostumeLoader.log` says which file on a `WARN: the slot loads ... from the game` line.

## Mods tested

Tested on 2026-09-27 and 2026-09-30 with official REFramework 1.5.8 and the game up to date. "Works" means seen in game
(training, character select, replays); Ryu Outfit I and Vegeta Majin were also played online.

| Character | Mod | Result | Notes |
|---|---|---|---|
| Ryu | [Aloha Ryu bundle](https://www.nexusmods.com/streetfighter6/mods/3214) | Works | 12 variants (outfit 2 and 3, gloves and headband add-ons) |
| Ryu | [Vagrant Ryu](https://www.nexusmods.com/streetfighter6/mods/2663) | Works | Installed with Fluffy Mod Manager (patch pak) |
| Akuma | [Akuma Classic Barechest](https://www.nexusmods.com/streetfighter6/mods/1693) | Works | A ready-made `.pak`, read from its `.rar` too |
| Ken | [Alpha Ken](https://www.nexusmods.com/streetfighter6/mods/2822) | Works | Read straight from the `.rar` |
| Ken | [Alpha Ken (Ripped Gi)](https://www.nexusmods.com/streetfighter6/mods/2822) | Works | |
| Ken | [Ken SFV style](https://www.nexusmods.com/streetfighter6/mods/1145) | Works | October 2023: old textures and 2023 colour files, both converted |
| Ken | [Ken SFV Hair (V2)](https://www.nexusmods.com/streetfighter6/mods/3897) | Works | Hair only. On its own: Outfit 2 with that hair. In one sub-folder with Ken SFV style: Ken SFV with that hair |
| Ken | [Vegeta Majin](https://www.nexusmods.com/streetfighter6/mods/126) | Works | June 2023: head part rebuilt, two textures repaired; online too |
| Cammy | [DOA4 Christie](https://www.nexusmods.com/streetfighter6/mods/3166) | Works | Colour files upgraded; the costume was white before, all 10 colours now show |
| Lily | [Nico bundle](https://www.nexusmods.com/streetfighter6/mods/78) | Works | Outfits with hair and weapon add-ons, weapons follow the slot |
| Zangief | [Specter Zangief](https://www.nexusmods.com/streetfighter6/mods/3816) | Works | |
| Dhalsim | [Mummy Dhalsim](https://www.nexusmods.com/streetfighter6/mods/3645) | Works | Its head lives in its own folder, named by its scene: showed the original head before 1.7 |
| Juri | [Oni Juri](https://www.nexusmods.com/streetfighter6/mods/3900) | Works | 1.9.0: its eyes (shared `000/` folder) shown, the original ones before |
| Juri | [Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3781) | Works | Together with Oni Juri, in either order |
| A.K.I. | [TFD Racer](https://www.nexusmods.com/streetfighter6/mods/3084) | Works | Same colour-file problem as Christie, fixed the same way |
| Akuma | [Specter Akuma](https://www.nexusmods.com/streetfighter6/mods/3805) | Works | |
| C. Viper | [Coat no mesh, Outfit 1](https://www.nexusmods.com/streetfighter6/mods/2966) | Works | Keeps the coat, removes the mesh fabric. Colours intact with the C3 body mod installed (reported lost with Fluffy alone) |
| C. Viper | An Outfit 3 body mod shared on Discord | Works | |
| C. Viper | [Swimsuit C. Viper](https://www.nexusmods.com/streetfighter6/mods/3909) | Works | Modular: 8 outfits (regular or thicc, barefoot or heels, gloves or not), default skin. 1.9.0: its wind settings used |
| C. Viper | [C.Viper Lace Lingerie](https://www.nexusmods.com/streetfighter6/mods/3015) | Works | Modular: 8 outfits (regular or thicc, barefoot or heels, jacket or not), default skin. 1.9.0: its retextured head shown |
| C. Viper | [C. Viper - Coatless C1](https://www.nexusmods.com/streetfighter6/mods/2967) | Works |  |
| C. Viper | [C. Viper C2 - Glasses Removed](https://www.nexusmods.com/streetfighter6/mods/2972) | Works | Changes the head only: gives Outfit 2 without glasses in a slot of its own (since 1.6.0) |
| Cammy | [Imperium Cammy](https://www.nexusmods.com/streetfighter6/mods/3046) | Works |  |
| Cammy | [Cammy Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3711) | Works |  |
| Dee Jay | [Specter Deejay](https://www.nexusmods.com/streetfighter6/mods/3810) | Works |  |
| Dhalsim | [Dhalsim The Wrestler](https://www.nexusmods.com/streetfighter6/mods/296) | Works | Reported loading Drive Tech with the old loader; fixed by the Dhalsim folder rule |
| Ingrid | [Ingrid as Athena (KOF97)](https://www.nexusmods.com/streetfighter6/mods/3579) | Works | One outfit since 1.8; the Drive Tech mesh it carried made a second entry before |
| Ingrid | [Chique Casual Ingrid](https://www.nexusmods.com/streetfighter6/mods/3699) | Works | Hair physics since 1.7 (its physics files sit next to the parts) |
| Ingrid | [AoD replace Ingrid](https://www.nexusmods.com/streetfighter6/mods/3977) | Works | Full replacement: keeps the original colour swatches. 1.8 also gives it its face and hair parts (to confirm in game) |
| Mai | [Changli Feixue SuiSui replace Mai](https://www.nexusmods.com/streetfighter6/mods/3921) | Works | Three outfits. Since 1.7: Feixue keeps its body and face (texture tables repaired), Mai's head and hair stay hidden as the mod intends, Changli is no longer left out |
| Mai | [Viper Racer](https://www.nexusmods.com/streetfighter6/mods/3018) | Works | Mai Racer (listed as a C. Viper mod in the community list) |
| Manon | [Manon Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3717) | Works |  |
| Marisa | [Marisa Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3782) | Works |  |
| Alex | [Minotaur Alex](https://www.nexusmods.com/streetfighter6/mods/3760) | Works |  |
| Yasmine | [Yasmine shorts](https://www.nexusmods.com/streetfighter6/mods/3904) | Works | Reported to crash on some AMD GPUs even without the loader |
| Cammy | [Camo Cammy](https://www.nexusmods.com/streetfighter6/mods/2277) | Works | 1.8.1: its material file names a texture with a doubled slash |
| Cammy | [Dance Outfit for Cammy](https://www.nexusmods.com/streetfighter6/mods/645) | Works | 2023 mod: 1.8.1 rebuilds its material file (dark body, opaque fabric before), takes its `1.tex`. With [BellyHair01](https://www.nexusmods.com/streetfighter6/mods/645) (hair made for Outfit 1) in one sub-folder: one outfit with the hair. The hair keeps its own colour (no colour mask) |
| Cammy | [Cammy cosplay Zani](https://www.nexusmods.com/streetfighter6/mods/2676) | Works | Costume, without horn, hair variants (bundle) |
| Cammy | [Cammy 2B Reincarnation](https://www.nexusmods.com/streetfighter6/mods/1609) | Works | Reported to make other Cammy mods lose their colours: that was the colour-file borrowing, fixed in 1.8.1 |
| Cammy | [Cammy - Escape from Shadaloo](https://www.nexusmods.com/streetfighter6/mods/2007) | Works | Mesh only: original colours (it borrowed another mod's before 1.8.1) |
| Cammy | [B Style Cammy Bunny Ver](https://www.nexusmods.com/streetfighter6/mods/3917) | Works | |
| Cammy | [Cammy Alt Costume Bare Legs](https://www.nexusmods.com/streetfighter6/mods/3911) | Works | Colours of the jacket and gloves wrong before 1.8.1 (high-resolution textures of another mod) |
| Lily | Lily Kenyan Summer C2 | Works | 1.9.0: its own cloth physics (the original outfit's before), clubs shown on the select screen from the first pick |
| C. Viper | [C. Viper Trench Coat](https://www.nexusmods.com/streetfighter6/mods/3107) | Works | Froze the select screen before 1.8.1 (backslash references) |
| C. Viper | [C Viper Bayonetta C2](https://www.nexusmods.com/streetfighter6/mods/3077) | Works | |
| C. Viper | [C. Viper Battlesuit C2](https://www.nexusmods.com/streetfighter6/mods/3055) | Works |  |
| C. Viper | [C. Viper Concept Outfit](https://www.nexusmods.com/streetfighter6/mods/3752) | Works | 1.9.0: its wind settings used |
| Lily | [Lily Haruka Hoodie](https://www.nexusmods.com/streetfighter6/mods/1874) | Works | 1.9.0: the hood (part of the hair, the body's cloth) shows the mod's cloth |
| Dhalsim | [Galaxy Avatar State Dhalsim](https://www.nexusmods.com/streetfighter6/mods/2985) | Works |  |

## Stage mods tested (experimental, since 1.9.0)

Stage mods go into `reframework/stage_mods/`, see [stages/README.md](stages/README.md). Tested on 2026-09-30.

| Stage | Mod | Result | Notes |
|---|---|---|---|
| Training Room | [NULSPACE](https://www.nexusmods.com/streetfighter6/mods/3564) | Works | Stage select (name, preview, UP / DOWN hints), VS screen, battle |
| Training Room | [Aokigahara with NPCs](https://www.nexusmods.com/streetfighter6/mods/3566) | Works | Battle |
| Training Room | [Aokigahara, no NPC](https://www.nexusmods.com/streetfighter6/mods/3566) | Hangs on the VS screen | The mod itself: it hangs the same way installed as a plain Fluffy pak, without the loader |
| 20 stages | [Stage Lighting Overhaul](https://www.nexusmods.com/streetfighter6/mods/3258) (24 options) | Partly seen | 1.9.1: the Training Room options' pictures repaired (selecting one hung the game on its way to Fighting Ground). Training Room Red and Yellow in battle, one after the other in the same session. Training Room Blue, Metro City Downtown, Carrier Byron Taylor seen on the stage select only. The 19 other options are built, not seen yet |
| Genbu Temple | [Genbu Temple Night Time](https://www.nexusmods.com/streetfighter6/mods/2857) | Recognised | A variant of Genbu Temple with its preview; not seen in battle yet |

## Not supported yet

- Mods that combine parts of two outfits (for example a head taken from Drive Tech Wear): they give
  one entry per outfit they touch.
- Colour-only mods, which change the colours of an original outfit without any model
  ([CVS Shin Akuma Inspired Color](https://www.nexusmods.com/streetfighter6/mods/3988)): ignored. How such
  a mod should appear (a slot of its own, or the original outfit changed) is still to be decided.
- Mods made only of textures and colours, without a model ([Cammy C4 Pale Skin](https://www.nexusmods.com/streetfighter6/mods/2850)),
  and a mod of one file ([A.K.I No Sleeves](https://www.nexusmods.com/streetfighter6/mods/1591)) whose file the
  loader does not take as an outfit: no outfit is made.
- A mod without colour files (Vegeta Majin) has one visible colour: the game's colours for that outfit apply
  to materials they do not name. As with the mod installed alone.
- Characters released after the loader's tables were built (they cover the 31 characters up to Yasmine).
- An accessory mod is applied to the original outfit, unless it sits in one sub-folder of the character
  with a costume for the same outfit, which it then completes.

## Reporting a mod

Send the mod link and `SF6_CostumeLoader.log` from the game folder. Lines starting with `WARN` or
`ERROR`, and the `check:` lines, tell what the loader found and repaired.
