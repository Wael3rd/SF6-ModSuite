SF6 Recording Manager v@VERSION@
================================

30 recording slots per character (1-8 are the game's own, 9-30 are extra), drills, slot names, a reversal picker and
Community sharing of drills, all in the game's own Training pause menu (Recording Settings > Manager / Record / Replay).
Everything is done by one native plugin, reframework/plugins/SF6_CRSMNative.dll. There is no Lua script.

REQUIRES
  - SF6 Native Core @CORE@ or newer (the SF6_NativeCore package of the SF6 Mod Suite release). Without it the plugin
    stays inactive.
  - REFramework 1.5.8 or newer (plugin API 1.10).

INSTALL
  1. Remove the old Lua version of the Recording Manager (v1.x), if you have it. In the game folder, delete from
     reframework/autorun/ :
        SF6_RecordingManager.lua
        SF6_RecordingSlotManager.lua
        the folder crsm/
        CRSM_REFrameworkUI.lua   (only if you ever added it)
     If any of these Lua scripts is still there, the plugin stands down and does NOT take over the menu (one engine at a
     time). The SF6 Training Suite (Training_ScriptManager.lua) does the same: do not use it together with this mod.
  2. Install this zip with Fluffy Mod Manager, or copy its reframework/ folder into the game folder.
  3. Start the game. The plugin takes over the Training pause menu by itself about 2 seconds after you enter Training
     (setting "standalone": "auto" in reframework/data/SF6_CRSMNative_data/settings.json; "off" keeps the game's own
     pages).

COMMUNITY (2.1.0)
  Sharing and likes need version 2.1.0 or newer and a Steam-verified identity. It is done automatically the
  first time you share or like: the plugin asks your running Steam client for a one-time proof (a Steam Web API ticket
  made for "sf6-community") and the Community server checks it with Steam. Nothing to click, no browser, no password.
  Only your CFN name (read from the game) is shown next to your drills, with a [V] mark; your Steam name and Steam ID
  are never shown, and the plugin never sends them. Your Community token is kept in
  reframework/data/SF6_Community_data/identity.json: delete that file to forget it on this PC (a new one is made at
  the next share or like). If Steam cannot verify you, browsing and downloading keep working; sharing and likes say why.

YOUR DATA
  Slots, drills and names are in reframework/data/SF6_RecordingSlotManager_data/ and Community settings in
  reframework/data/SF6_Community_data/. These are the same folders as v1.x: your recordings and drills from v1.x are
  kept and used as they are. They are never part of this package, so they stay when you update or uninstall.
  The plugin's own files (settings.json, status.json, debug_log.json) are in reframework/data/SF6_CRSMNative_data/.

DRILLS AND THE DUMMY'S SETTINGS (2.2)
  A drill saved by 2.2 also holds which slots are checked, the replay settings, the dummy's settings (guard, throw
  escape, recovery, drive reversal, counter, action) and, when you use them, the dummy's reversal rows. Opening such a
  drill (or a Community one) sets them as its author had them; your previous ones are in the pre-import backup
  (SF6_RecordingSlotManager_data/<character>/Backups/): copy that file into the character's folder and open it to get
  them back. To keep your own dummy, reversal and replay settings whatever you open, add
  "import_training_setup": false to SF6_RecordingSlotManager_data/config.json. Older drills change nothing there.

LOGS
  REFramework's log (lines starting with [CRSMNative]), reframework/data/SF6_CRSMNative_data/debug_log.json and
  status.json, and the Native Core's log reframework/data/sf6core/sf6core.log.
  Emergency stop: create an empty file reframework/data/_crsm_native_off.txt (the plugin does nothing at the next start).
