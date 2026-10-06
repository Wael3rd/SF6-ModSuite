# What still needs testing

Everything below was either fixed without being seen in game yet, or never exercised on a real
installation. Each item says what to do and what to look for. `SF6_CostumeLoader.log` sits in the game
folder; the loader rewrites it at every launch where something changed.

## Confirmed in game on 2026-09-27

- Colours of Cammy DOA4 Christie and A.K.I. TFD Racer, all 10 colours.
- Alpha Ken (Ripped Gi), Lily Nico Outfits I and II, the 12 Aloha Ryu variants one by one.
- Replays with modded outfits.
- Online casual match with Ryu Outfit I and with Ken's Vegeta Majin slot, on official REFramework 1.5.8.
- Removing a mod: the outfit goes away, names stay contiguous, no stall.
- A character played on trial lists only the installed mods' outfits.
- Fifteen more mods installed at once as archives (see COMPATIBILITY.md): first launch about three
  minutes, later ones instant.
- C. Viper Swimsuit and Lace Lingerie as 8 outfits each, and Outfit 2 without glasses.
- A costume and an add-on from two archives combined by putting them in one sub-folder (Ken SFV
  style + Ken SFV Hair).
- Mummy Dhalsim's own head; Changli, Feixue and SuiSui for Mai; Chique Casual Ingrid's hair physics;
  AoD replace Ingrid (2026-09-29).
- Mods whose physics files sit next to the parts now use them (Aloha Ryu, Mummy Dhalsim, the C. Viper
  modular mods, Athena, Manon, Juri, Akuma, Cammy, Mai): nothing broken (2026-09-29).

## Confirmed in game on 2026-09-30

- Cammy: Camo, Dance Outfit (with its hair, sheer fabric), Zani, 2B Reincarnation, Escape from Shadaloo, Bunny,
  Alt Bare Legs (colours), Christie, Imperium; no mod borrows another's colours any more.
- C. Viper Trench Coat no longer freezes; Swimsuit's eight variants and the other Viper mods load.
- Ingrid AoD replace Ingrid (hidden face and hair kept), Athena as one outfit, Chique Casual's hair physics.
- Mummy Dhalsim's head.
- 1.8.1, all of it: C. Viper Bayonetta (wings included), Ken SFV with Ken SFV Hair in one sub-folder (only the
  combined outfit), several mods of the same outfit installed together (colours no longer mixed).

## New in 1.9.0, to check in game

- Checked on 2026-09-30: boot (colour records, select screen data, ownership), ownership given back after the
  title screen, Battle Settings shown and closed (intents kept when entering Versus), stage select (hints,
  previews, UP / DOWN, Change Stage from the result screen), VS screen, a battle on NULSPACE, weapons of Lily's
  mods on the select screen, nothing in the log during fights, and **the online alias with the native plugin**:
  three online matches with Lily Kenyan Summer C2 on DriveTech, its clubs and colour 3, the colour put back
  between matches. The plugin's log (`reframework\data\SF6_CostumeSlots_data\native_log.txt`) shows
  `outfit mounted, alias check`, `manifest vN -> v4 = 1`, `alias check done`.
- **The alias without going online**: Training, Arcade, Versus and Team Battle pick the costume on a select
  screen, so they never alias (a DriveTech picked there is DriveTech). Combo Trials does not: set the
  character and its slot in Battle Settings (R on the Fighting Ground menu), then Practice > Combo Trials,
  the character in **DriveTech Wear**, any trial. The slot must show; the plugin's log shows
  `slot vN mounted (the game mounted DriveTech)`, `manifest vN -> v4 = 1`, and `colour C placed under colour 0`
  when the slot's colour is not one DriveTech has. Checked on 2026-10-06 with Ryu (slot 8) and Lily (slot 13).

- **First launch after updating**: the costumes are rebuilt once (the stage pak no longer counts in their
  fingerprint), then `Up to date` again. Adding or removing a stage mod afterwards must not rebuild the
  costumes (`SF6_CostumeLoader.log`: `stage slots pak, not a costume mod`, then `Up to date`).
- **Stage slots online**: select a variant, then play a casual or ranked match on that stage. Only you see it;
  check that nothing else happens.
- **UP / DOWN hints with a pad** (only a keyboard was tried).
- **Other stage select screens** (rooms, Battle Hub, matchmaking settings).
- **Stage Lighting Overhaul** options other than Training Room Red and Yellow, and **Genbu Temple Night Time**,
  in battle.
- **Without stage mods**: `SF6_StageSlots.log` shows no `hook installed`.

## New in 1.7, to check in game

- **Colour swatches** (experimental): in the costume menu, the two squares of each colour of a modded
  outfit should differ from colour to colour and look like the outfit; the original outfits must keep
  Capcom's squares. The plugin's log (`reframework\data\SF6_CostumeSlots_data\native_log.txt`) reports
  `pastilles propres=N`, the number of colours given their own squares.

## Loader paths never triggered by a real mod

- **A slot left out**: no installed mod has needed it so far. When one does, the log shows
  `WARN: slot ... left out, the game could not load it: <reason>` and that outfit is simply absent.
  Report the mod and the log.
- **The pak in use**: launch the game right after closing it, while the previous process is still
  exiting. If it does not let go within about ten seconds, the log shows
  `ERROR: ... in use by another Street Fighter 6 process` and the costumes stay as they were; the next
  launch must regenerate them. Tested on the bench only.

## Installation and first launch

- **Audit script**: run `reframework\costume_mods\SF6_CostumeAudit.bat` on a fresh installation, before and
  after the first launch, and check that every mod and outfit is listed with its origin.
- **Fluffy Mod Manager**: install the release archive through Fluffy on a clean game, then add costume
  mods both through Fluffy and as archives in `reframework\costume_mods\<Character>\`.
- **First launch with many archives**: the archives are unpacked once into `costume_mods\.cache`.
  Note how long the first launch takes; antivirus scanning can make it much longer.
- **Characters owned or not**: a character you do not own (trial) must list only the installed mods'
  outfits, and none of them must stall.

## Online, needs two players

- **Your opponent's modded outfit**, mirrored as Drive Tech on your side.
- **Spectator mode** with modded outfits.

## Mods to try

From the community compatibility list, not tested yet with the current loader. The mods it reported
as broken with earlier versions have all been retested (see COMPATIBILITY.md).

### Same families as mods that work (quick confirmations)

- [Jopok Lily](https://www.nexusmods.com/streetfighter6/mods/2995) (Lily, TonKumaTsu)
- [Ingrid Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3665) (Ingrid, THEJAMK)
- [Chun Li Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3710) (Chun-Li, THEJAMK)
- [Mai Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3712) (Mai, THEJAMK)
- [Lily Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3716) (Lily, THEJAMK)
- [Aki Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3718) (A.K.I., THEJAMK)
- [C Viper Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3929) (C. Viper, THEJAMK)
- [Yasmine Drive Tech Wear Alt](https://www.nexusmods.com/streetfighter6/mods/3930) (Yasmine, THEJAMK)
- [Marisa Of Silence](https://www.nexusmods.com/streetfighter6/mods/3961) (Marisa, TonKumaTsu)

### Others

- [Oni (Akuma)](https://www.nexusmods.com/streetfighter6/mods/2868) (Akuma, GhostDog): generates without warnings; its
  Color Previews add-on (the select-screen squares only) is not needed since the loader computes them
- [SFV ALEX](https://www.nexusmods.com/streetfighter6/mods/3368) (Alex, THEJAMK)
- [C. Viper Battlesuit C2](https://www.nexusmods.com/streetfighter6/mods/3055) (C. Viper, Sleepy Scrub)
- [C. Viper Bayonetta C2](https://www.nexusmods.com/streetfighter6/mods/3077) (C. Viper, usetrial)
- [C. Viper Trench Coat](https://www.nexusmods.com/streetfighter6/mods/3107) (C. Viper, AntiSky101)
- [C Viper Classic Tombraider C2](https://www.nexusmods.com/streetfighter6/mods/3126) (C. Viper, usetrial)
- [C. Viper Concept Outfit](https://www.nexusmods.com/streetfighter6/mods/3752) (C. Viper, GhostDog)
- [B Style Cammy Bunny Ver](https://www.nexusmods.com/streetfighter6/mods/3917) (Cammy, migerumods)
- [Galaxy Avatar State Dhalsim](https://www.nexusmods.com/streetfighter6/mods/2985) (Dhalsim, Dark Touch Studios): generates without warnings
- [Ingrid Durst](https://www.nexusmods.com/streetfighter6/mods/3541) (Ingrid, SirCheeseburgur)
- [More Faithful Midnight Bliss Ingrid](https://www.nexusmods.com/streetfighter6/mods/3573) (Ingrid, Corythan)
- [Ingrid Shiori Novella](https://www.nexusmods.com/streetfighter6/mods/3701) (Ingrid, SirCheeseburgur)
- [Lynae replaces Kimberly](https://www.nexusmods.com/streetfighter6/mods/3970) (Kimberly, DolinOfficial)
- [Lily Angel](https://www.nexusmods.com/streetfighter6/mods/815) (Lily, Haise Sasaki)
- [Lily Summer Outfit](https://www.nexusmods.com/streetfighter6/mods/1446) (Lily, CrystalMang0)
- [Lily Haruka Hoodie](https://www.nexusmods.com/streetfighter6/mods/1874) (Lily, SirCheeseburgur)
- [Lily C1 - Squirrel Girl](https://www.nexusmods.com/streetfighter6/mods/2320) (Lily, HugueKas97)
- [Lily Jun Kazama Cosplay](https://www.nexusmods.com/streetfighter6/mods/2505) (Lily, SirCheeseburgur)
- [Lily Zoro Cosplay](https://www.nexusmods.com/streetfighter6/mods/2640) (Lily, SirCheeseburgur)
- [Yasmine with Shoes C1](https://www.nexusmods.com/streetfighter6/mods/3793) (Yasmine, ZZtaii)
- [RE9 Grace Ashcroft Outfit for Yasmine C2](https://www.nexusmods.com/streetfighter6/mods/3797) (Yasmine, monkeygigabuster)
- [Yasmine No Jacket with Shoes C1](https://www.nexusmods.com/streetfighter6/mods/3857) (Yasmine, ZZtaii)
- [Yasmine Josie Rider Outfit](https://www.nexusmods.com/streetfighter6/mods/3938) (Yasmine, SirCheeseburgur)
- [Sporty Shorts for Yasmine](https://www.nexusmods.com/streetfighter6/mods/3990) (Yasmine, beanpole_brando)
