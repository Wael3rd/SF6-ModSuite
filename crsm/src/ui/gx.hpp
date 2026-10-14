// ui::gx — the game-side helpers of the DLL's screens (phase P4). Reads go through REFramework's API (read-only calls:
// names, children, messages, sizes); every WRITE to the native UI goes through the Native Core's display API
// (ui_set_text, ui_set_visible, ui_set_position, ui_set_font_size, obj_invoke, obj_set_*...: compare-before-write,
// quarantine, the client's breaker), as the Color Mod does.
//
// Nothing here throws a C++ exception on purpose; an access violation is the caller's to catch (hook bodies run under
// __try / try-catch with /EHa).
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

#include <reframework/API.hpp>

#include "sf6core_api.h"

namespace ui::gx {

using API = reframework::API;
using MO = API::ManagedObject;
using TD = API::TypeDefinition;

const SF6CoreAPI* api();                       // nullptr without the core
SF6CoreClient* cl();

// ---- strings -------------------------------------------------------------------------------------------------------------
std::string str(void* managed_string);         // System.String -> UTF-8 ("" for null)
std::string msg(MO* text);                     // get_Message; "nil" for a null message (Lua's tostring(nil))
std::string name(MO* play_object);             // get_Name
bool is_type(MO* o, const char* full_name);    // exact type (the Lua's get_type_definition():get_full_name() == ...)

// ---- the via.gui tree (read) --------------------------------------------------------------------------------------------
MO* first_child(MO* e);
MO* next(MO* e);
MO* parent(MO* e);
MO* child(MO* e, const char* name);            // a direct child by name (child_named, RMT:436-439)
MO* find_text(MO* e, const char* name, int depth = 0);   // a via.gui.Text by name under e, depth <= 4 (RSL:93-101)
bool visible(MO* c);
bool force_invisible(MO* c);
bool size_of(MO* o, const char* getter, float* w, float* h);   // via.Size getters (get_FontSize, get_DisplaySize...)
bool vec3_of(MO* o, const char* getter, float out[3]);       // via.vec3 getters (get_Position)
bool color_of(MO* o, const char* getter, uint32_t* rgba);    // via.Color getters (the rgba field)
bool call_i32(MO* o, const char* method, int32_t* out);       // a no-argument method returning an integer
bool call_bool(MO* o, const char* method, bool* out);
MO* call_obj(MO* o, const char* method);                      // a no-argument method returning an object
bool get_i64(MO* o, const char* field, int64_t* v);           // an integer / bool / enum field
MO* ref(MO* o, const char* field);

// ---- writes through the core ------------------------------------------------------------------------------------------
void set_text(MO* t, const std::string& s);                   // ui_set_text (compare-before-write, R7)
void set_visible(MO* c, bool on);
void set_force_invisible(MO* c, bool on);
void set_pos(MO* c, float x, float y, float z);
void set_font(MO* t, float w, float h);
void set_region(MO* t, float w, float h);
void set_size(MO* c, float w, float h);
void set_block_input(MO* agent, bool on);
SF6CoreArg I(int64_t v);
SF6CoreArg B(bool v);
SF6CoreArg P(const void* p);
SF6CoreArg S(const char* s);
SF6CoreArg F(double v);
// obj_invoke: false when the core refused or the call faulted. A value type argument goes by address (P), as REFramework's
// own call() passes ValueTypes (core ui_set_font_size does the same).
bool invoke(MO* o, const char* sig, std::initializer_list<SF6CoreArg> args = {}, SF6CoreArg* out = nullptr);
void set_field_i64(MO* o, const char* field, int64_t v);      // obj_set_i64, compared first
void set_field_str(MO* o, const char* field, const char* s);  // obj_set_string
void set_color(MO* t, const char* setter, uint32_t rgba);     // set_<Color prop>(via.Color) through obj_invoke
// the 65 style properties of a game text copied onto a borrowed one, property by property (RMT:732-742, 796):
// copyProperties would bind the target to the source's animation curves (R31). Returns how many were written; `failed`
// (optional) gets the names of those that were not ("name(status)").
// The lookups (each property's getter, its setter and the setter's full signature) are resolved once per (source type,
// target type) and kept. `shared` (optional): the source's values as read for a first target, handed to a second target
// copied from the SAME source in the same pass (a row's name and arrow): read once, the same setter calls.
struct StyleRead;
struct StyleShare {
    MO* src = nullptr;
    StyleRead* read = nullptr;                 // owned
    StyleShare() = default;
    StyleShare(const StyleShare&) = delete;
    StyleShare& operator=(const StyleShare&) = delete;
    ~StyleShare();
};
int copy_style(MO* dst, MO* ref, std::string* failed = nullptr, StyleShare* shared = nullptr);
// The warm-up of the first menu opening, run on the frame callback with the menu closed until it answers true: the
// via.gui.Text -> via.gui.Text plan of copy_style and REFramework's lookups of the full signatures the Native Core will be
// asked for (its first obj_invoke of each scans the type), a few per call, within `budget_ms`. Read-only (lookups).
bool warm_step(double budget_ms);
std::string warm_status();
// The probe dump (ui/dump.cpp, read-only): a value of type `t` at `p` as text (numbers, bools, the engine's small structs,
// strings, an object's type), and a text's style as one line: the 65 properties copy_style copies, its message,
// visibility, position, scale, sizes, and its parent path.
std::string format_value(reframework::API::TypeDefinition* t, const void* p);
std::string describe_text(MO* t);

// ---- finders ----------------------------------------------------------------------------------------------------------
MO* find_param();                              // the training pause menu's flow Param (UIFlowManager._Handles, RMT:440-446)
MO* pause_agent();                             // the pause menu's agent ui11200 (core discovery)
MO* singleton(const char* name);

}  // namespace ui::gx
