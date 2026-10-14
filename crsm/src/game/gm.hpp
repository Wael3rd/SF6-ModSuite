// game::gm — the small set of game accesses the mechanics layers need (phase P3), on REFramework's C++ API.
//
// Rules (native-UI rules R16 / R28 / R34):
//  * nothing here throws a C++ exception; an access violation is the caller's to catch (the hooks run their body under
//    __try, the bridge and frame functions under try / catch(...), /EHa);
//  * value fields are written with the field's own size, after a type check (never past a field);
//  * reference fields: add_ref(new) -> atomic exchange -> release(old) (R34), as REFramework's Lua set_field does, on a
//    refcounted target; a plain store on a local target (negative reference count), as the game's own stores do;
//  * objects the DLL creates (arrays, instances) get one add_ref and are never released: the game may keep pointers to
//    them (the reversal candidates share the virtual buffers, R28);
//  * UInt16[] buffers are read and written in place only after a one-time layout check (u16_ok()); without it the layers
//    that need buffers stay with the Lua.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

#include <reframework/API.hpp>

namespace game::gm {

using API = reframework::API;
using MO = API::ManagedObject;
using TD = API::TypeDefinition;

TD* type(const char* full_name);                                   // cached
API::Field* field(TD* td, const char* name);                       // walks the parent types
API::Method* method(TD* td, const char* name_or_signature);        // walks the parent types
API::Method* method_n(TD* td, const char* name, uint32_t nparams); // an overload by name and parameter count

MO* singleton(const char* name);
bool is_managed(const void* p);                                     // REFramework's is_managed_object

// ---- fields ----------------------------------------------------------------------------------------------------------
MO* ref(MO* o, const char* name);                                   // a reference field (nullptr when null / absent)
bool get_int(MO* o, const char* name, int64_t* v);                 // integer, enum or bool field (signedness of its type)
bool get_bool(MO* o, const char* name, bool* v);
bool set_int(MO* o, const char* name, int64_t v);                  // integer / enum field: its own size; never a float
bool set_bool(MO* o, const char* name, bool v);                    // a 1-byte System.Boolean field only
bool set_ref(MO* o, const char* name, MO* v);                      // R34, as the game's stores (local target: plain store)
std::string ref_status();                                          // set_ref's counts (status "refs")
bool is_float_field(MO* o, const char* name);                      // System.Single / System.Double
// An integer / enum / bool field read in place: int_field finds it once (counted as a get_int), read_int then reads it
// straight from memory, no REFramework call. The caller keeps o alive (add_ref) as long as it reads through it.
struct IntField { const void* p = nullptr; uint32_t size = 0; bool is_unsigned = false; bool is_bool = false; };
bool int_field(MO* o, const char* name, IntField* out);
bool read_int(const IntField& f, int64_t* v);

// ---- calls -------------------------------------------------------------------------------------------------------------
void* arg_i32(int32_t v);
bool call(MO* o, API::Method* m, std::initializer_list<void*> args, reframework::InvokeRet* out = nullptr);
bool call(MO* o, const char* sig, std::initializer_list<void*> args, reframework::InvokeRet* out = nullptr);
MO* call_obj(MO* o, const char* sig, std::initializer_list<void*> args = {});
bool call_i32(MO* o, const char* sig, int32_t* v, std::initializer_list<void*> args = {});
bool call_bool(MO* o, const char* sig, bool* v, std::initializer_list<void*> args = {});

// ---- arrays and lists ------------------------------------------------------------------------------------------------
int32_t array_len(MO* arr);                                         // -1 when it cannot be read
MO* array_get(MO* arr, int32_t i);                                  // System.Array.GetValue(Int32)
bool array_set(MO* arr, int32_t i, MO* v);                          // System.Array.SetValue(Object, Int32): the game's own refcounting
MO* new_array(TD* element, uint32_t n);                             // System.Array.CreateInstance + add_ref (held for the session)
MO* new_object(TD* td);                                             // create_instance + add_ref (held for the session)
int32_t list_count(MO* list);                                       // List`1.get_Count, -1 on failure
MO* list_item(MO* list, int32_t i);                                 // List`1.get_Item(Int32)

// ---- UInt16[] in place ------------------------------------------------------------------------------------------------
// The layout check: a System.UInt16[3] is created, its element count read raw and through get_Length, a value written
// raw and read back through GetValue (boxed "mValue"). Done once, on the first call; false = no in-place access.
bool u16_ok();
std::string u16_report();                                           // what the check found (status / journal)
uint16_t* u16_data(MO* arr, int32_t* len);                          // nullptr unless a System.UInt16[] and u16_ok()

}  // namespace game::gm
