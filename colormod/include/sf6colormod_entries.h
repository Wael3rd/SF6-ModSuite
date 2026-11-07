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
 *   - Costumes are FighterDesc.Costume numbers (0-based: the colour folder 001 is costume 0), slots included.
 * Any thread; no game call except the running flow map, read on the game thread by the Color Mod itself. */
#ifndef SF6COLORMOD_ENTRIES_H
#define SF6COLORMOD_ENTRIES_H

#ifdef __cplusplus
extern "C" {
#endif

#define SF6CM_ENTRIES_VERSION 1
#define SF6CM_SCOPE_PREVIEW 1
#define SF6CM_SCOPE_REPLAY 2

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

#ifdef __cplusplus
}
#endif

#endif /* SF6COLORMOD_ENTRIES_H */
