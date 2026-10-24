// SF6_CRSMNative - shared pieces: core client, feature ownership, journal, status, settings, kill switch.
// Phase P0 of the port: the skeleton, nothing visible, no user data touched.
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include "sf6core_api.h"

namespace cmn {

// Layers handed over from the Lua scripts one at a time. A layer is the DLL's only when this build
// implements it (set_implemented) AND settings.json says "native" (a missing settings.json: owners.hpp).
enum class Feature : int { Community = 0, Files, Replay, Reversal, Record, Ui, Count };

const char* feature_name(Feature f);
bool owns(Feature f);            // true = this DLL runs that layer (the Lua stands down); false after its faults
bool active();                   // false = kill file present, or the core is missing / refused us
bool closing();                  // true from the moment the game window gets WM_CLOSE / WM_DESTROY / WM_ENDSESSION
uint64_t frame();                // frames seen by the core's frame callback (0 before the first one)
uint32_t frame_thread();         // id of the thread that ran the last frame callback (0 before the first one)
const SF6CoreAPI* core();        // nullptr when the core is absent
SF6CoreClient* client();
const char* version();

// Logging: REFramework log (and the journal). "[CRSMNative]" prefix: the Lua side keeps "[CRSM]".
void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
// Journal only: data/SF6_CRSMNative_data/debug_log.json, 400 lines "[f<frame>] ...", flushed every 30 frames.
void journal(const char* fmt, ...);
// The game window's keyboard state as REFramework's own is_key_down reads it (WM_KEYDOWN / WM_KEYUP, all up when the window
// loses the focus): a key typed in another application is not seen (P4's raw keys, RMT:329-332, 1758-1760).
bool key_down(int vk);
// Plain text file (append only, off the game thread), for example the probes log.
void append_file(const char* path, std::string text);

// Called once from reframework_plugin_initialize. Returns false only on a hard failure (the plugin then stays loaded
// but inert).
bool init();
// Rewrites status.json (called again when a module has something new to say).
void refresh_status();

// Hooks of other modules into the frame callback and the status (set by main.cpp before init()).
extern void (*g_probe_frame)(uint64_t frame);
extern std::string (*g_probe_status)();      // one-line summary for status.json / the Lua bridge

// ---- layers (phase P3: files, replay, reversal, record) -------------------------------------------------------------
// A layer module declares itself implemented before init(); settings.json then decides who owns it (owners.hpp).
// replay / reversal / record need files (their state lives in the DLL's model of the files).
void set_implemented(Feature f);
bool requested(Feature f);                   // settings.json asks "native" (whatever the build implements)
// A fault of a layer (an access violation caught around a game call, a game call that failed where it must not):
// after 5 the layer goes back to the Lua (owns() false from then on; its hand-back runs on the next frame callback,
// game thread). files going back takes the three layers that need it.
void fault(Feature f, const char* where);
bool tripped(Feature f);
// A layer the DLL owns that cannot run in this session (a core service missing, a layer it needs went back): back to the
// Lua at once, without counting faults; its hand-back functions run on the next frame callback. One log line.
void decline(Feature f, const char* why);
void on_hand_back(Feature f, void (*fn)());  // several modules may register for one layer (ui: P4 and P5); all run
uint32_t owners_mask();                      // bit (1 << Feature) per layer the DLL owns now, for the Lua's frame refresh
// settings.json "standalone" as written (nullopt = missing): parsed by standalone::parse_mode (P6, ui/ui.cpp).
std::optional<std::string> setting_standalone();
// A Lua script called the DLL's bridge (ui_claim, frame_poll, owns): a CRSM Lua is running (P6: the DLL then never claims
// the UI by itself). Any thread.
void note_lua_call();
bool lua_called();
// Module hooks: called from the core's frame callback (game thread, LateUpdateBehavior), in registration order.
void add_frame_fn(void (*fn)(uint64_t frame), const char* perf_name = nullptr);   // perf_name: the perf log's entry
// Flow listeners: called from the core's flow callback (game thread) with the old and the new flow id, in registration
// order. flow_events() = the core took our flow callback (otherwise the flow id must be read by the caller).
void add_flow_fn(void (*fn)(int32_t old_id, int32_t new_id));
bool flow_events();
// One-line status per module for status.json ("name" -> text).
void add_status(const char* name, std::string (*fn)());
// Lua bridge functions of the modules: SF6Core.clients["SF6_CRSMNative"].<name>(...), registered when the core connects.
void bridge_fn(const char* name, SF6CoreLuaFn fn);
// lua_register_fn through the perf wrapper (timed as "Lua:<name>"; the function gets a null context, as everywhere here).
SF6CoreStatus lua_register_timed(const char* name, SF6CoreLuaFn fn);

// Background file writer: a single thread that drains queued file writes off the game thread.
// Only for plain file I/O (journal, status, probes log); never touches game objects.
namespace bgwriter {
void start();                                        // call once from cmn::init()
void write(const char* path, std::string content);   // truncate-write
void append(const char* path, std::string content);  // append-write
// Switch files: the writer thread looks at `path` about once a second (once it runs) and publishes whether it exists in
// *flag, so the game thread reads an atomic instead of calling the file system. Register before or after start().
void watch_file(const char* path, std::atomic<bool>* flag);
}  // namespace bgwriter

}  // namespace cmn
