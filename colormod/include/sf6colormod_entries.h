/* SF6_ColorModNative.dll: its colour entries (RW / MC) for other mods, and what a fighter wears at their request.
 *
 * The suite's DLL-to-DLL pattern: exported C functions, resolved by the caller with
 * GetModuleHandleW(L"SF6_ColorModNative.dll") + GetProcAddress (see docs/RUNTIME_OPTIONS.md, "Color Mod entries").
 * Missing exports = no Color Mod or an older one: the caller lists the game's own colours only.
 *
 *   - An RW / MC entry is not a game colour: every one rides on the game's colour 02 (id 1). A fighter wears it when its
 *     colour is 1 and the Color Mod dresses it with the entry's file. An entry's id is that file's path (relative to
 *     reframework/data, the archive cache's hash left out): stable when the list is reordered.
 *   - A request says what one fighter / costume wears, over the player's own Color Mod choices (never changed): an entry,
 *     by id, on colour 02; or "" = the game's own colours (no entry, no edited game colour).
 *       scope 1 (preview): a capture-room fighter outside a battle (Battle Settings' preview room, borrowed by the
 *                          requester), while the requester's page is up; the requester clears it when the page goes.
 *       scope 2 (replay):  the fighters of one replay's battle; `battle` = its flow map (app.battle.ReplayFlowMap).
 *                          Applied only while it is the running flow map (bFlowManager.m_flow_work._FlowMap); dropped
 *                          once another one runs, or when it never ran within 10 s. A new request replaces the old.
 *       scope 3 / 4 (battle, P1 / P2): the battle fighter of that side (its model and sub-models, never the character
 *                          select's previews) while it wears colour 02, from the request until the requester replaces or
 *                          clears it: the Slots Loader's Random colour drew that entry for this battle (or set, online).
 *                          A replay's request (scope 2) comes first. `battle` unused. Older Color Mods refuse them.
 *   - Costumes are FighterDesc.Costume numbers (0-based: the colour folder 001 is costume 0), slots included.
 *   - The Random value: while the Slots Loader (SF6_CostumeSlotsNative.dll) exports SF6Slots_RandomColour, the Color row of
 *     each outfit it names gets one more value after the entries, named with the word it gives (the game's own), on colour
 *     02 like them; never on Opponent Outfit Settings' page. The Color Mod remembers it where it remembers an entry (the
 *     character select's / training popup's P1 and P2) and, for Fighter Settings, per fighter (SF6ColorMod_FighterRandom);
 *     it dresses nothing of its own for it (the plain colour 02): the Slots Loader draws among the game's colours and the
 *     entries, and asks for an entry by scope 3 / 4. Confirmed in the training popup where the game reloads nothing, the
 *     Color Mod asks the Slots Loader for a draw at once (SF6Slots_RandomNow) and recolours that side in place.
 * Any thread; no game call except the running flow map, read on the game thread by the Color Mod itself. */
#ifndef SF6COLORMOD_ENTRIES_H
#define SF6COLORMOD_ENTRIES_H

#ifdef __cplusplus
extern "C" {
#endif

#define SF6CM_ENTRIES_VERSION 1
#define SF6CM_SCOPE_PREVIEW 1
#define SF6CM_SCOPE_REPLAY 2
#define SF6CM_SCOPE_BATTLE_P1 3
#define SF6CM_SCOPE_BATTLE_P2 4

typedef struct SF6CMEntry {
    char label[16];             /* "RW 3", "MC 1": the Color Mod's own label, as its colour menus show it */
    char id[512];               /* stable id: the entry file's path (UTF-8) */
} SF6CMEntry;

/* SF6CM_ENTRIES_VERSION while the colour engine runs (its decisions on), 0 otherwise: requests then do nothing. */
typedef int (*SF6ColorMod_EntriesVersionFn)(void);
/* The entries of a fighter / costume, in the Color Mod's order (RW first, then MC). Writes up to `cap` of them into
 * `out` (may be NULL when cap is 0) and returns how many there are (may exceed cap); 0 = none. */
typedef int (*SF6ColorMod_EntriesFn)(int fighter, int costume, SF6CMEntry* out, int cap);
/* A request (see above). id NULL or "" = the game's own colours. battle: scope 2 only (NULL refused). 1 = taken. */
typedef int (*SF6ColorMod_RequestFn)(int scope, int fighter, int costume, const char* id, void* battle);
/* Every request of that scope goes. */
typedef void (*SF6ColorMod_ClearRequestsFn)(int scope);
/* Added with the ticks of Opponent Outfit Settings, absent from older Color Mods: how many entries are ticked for an outfit there, on top of the colour numbers the
 * game's save holds for it. Any thread. */
typedef int (*SF6ColorMod_OpponentTicksFn)(int fighter, int costume);
/* Added with the Random value, absent from older Color Mods (which refuse scopes 3 and 4 too): 1 when the remembered choice
 * of `side` (1 = the character select's / training popup's P1, 2 = P2, 3 = Battle Settings' Fighter Settings) is the Random
 * value, its fighter and costume written to *fighter / *costume (either may be NULL); 0 otherwise. Any thread. */
typedef int (*SF6ColorMod_RandomChoiceFn)(int side, int* fighter, int* costume);
/* Added after it (absent from Color Mod 0.26.0, whose side 3 above is the last fighter shown only): Fighter Settings' Random
 * colour of `fighter`, remembered per fighter (set when Random is picked on its Color row, dropped when another colour is
 * picked there or Fighter Settings is left on another outfit of it): the outfit it was chosen on, -1 none. Any thread. */
typedef int (*SF6ColorMod_FighterRandomFn)(int fighter);

/* The other way, SF6_CostumeSlotsNative.dll's export, looked for by the Color Mod (absent = no Random value anywhere):
 * 1 when the Slots Loader draws a Random colour for that outfit (every outfit but a character's Random outfit, whose one
 * colour is Random already), the game's word for Random written to `word` (UTF-8, NUL-terminated) when cap > 0;
 * 0 otherwise. Any thread, no game call. */
typedef int (*SF6Slots_RandomColourFn)(int fighter, int costume, char* word, int cap);
/* SF6_CostumeSlotsNative.dll's too (absent from older ones): the Random value chosen where the game reloads nothing (the
 * training popup confirmed on Random from a value already on colour 02): a draw now for battle side `side` (1 / 2) of that
 * fighter / outfit. Returns the game's colour that side must wear now (an entry drawn: 1, the Slots Loader has asked for it
 * by scope 3 / 4), -1 none (nothing drawn: what it wears stays). Any thread; call it without holding a lock the Slots
 * Loader's own calls into the Color Mod would wait for. */
typedef int (*SF6Slots_RandomNowFn)(int side, int fighter, int costume);

#ifdef __cplusplus
}
#endif

#endif /* SF6COLORMOD_ENTRIES_H */
