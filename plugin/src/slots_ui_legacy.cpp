// slots_ui_legacy.cpp -- previous direct implementations of the ported display
// operations, compiled only when SLOTS_LEGACY_UI_FALLBACK is defined.
// Same behaviour as the original code.  Delete this file when the core becomes
// mandatory (see slots_ui.hpp header comment for the full removal recipe).
#ifdef SLOTS_LEGACY_UI_FALLBACK

#include "slots_ui.hpp"
#include <unordered_map>

namespace slots::ui::legacy {
namespace {

// Texture creation frames: tex_load records the frame, tex_ready checks it.
std::unordered_map<void*, uint32_t> g_tex_frames;
constexpr uint32_t kLoadFrames = 60;

} // namespace

// ---- P1: data injection -------------------------------------------------

// MemberwiseClone + add_ref (matches the old code in costumes.cpp).
Obj* clone(Obj* src) {
    Obj* r = call_obj(src, "MemberwiseClone");
    if (r) r->add_ref();
    return r;
}

// Any integer / bool / enum field.  Dispatches by the field's value-type size.
bool set_i64(Obj* obj, const char* field, int64_t v) {
    void* p = field_ptr(obj, field);
    if (!p) return false;
    auto* fd = field_of(obj->get_type_definition(), field);
    auto* ft = fd ? fd->get_type() : nullptr;
    uint32_t sz = ft ? ft->get_valuetype_size() : 4;
    switch (sz) {
        case 1: { uint8_t  b = static_cast<uint8_t>(v);  memcpy(p, &b, 1); break; }
        case 2: { int16_t  s = static_cast<int16_t>(v);  memcpy(p, &s, 2); break; }
        case 8: {                                         memcpy(p, &v, 8); break; }
        default:{ int32_t  i = static_cast<int32_t>(v);  memcpy(p, &i, 4); break; }
    }
    return true;
}

// Raw pointer write (the old set_obj: no reference counting at the assignment
// site -- the clone path already add_ref'd the object).
bool set_ref(Obj* obj, const char* field, Obj* value) {
    return set_obj(obj, field, value);
}

// via.Color value field written via memcpy (the old write_color).
bool set_color(Obj* obj, const char* field, uint32_t rgba) {
    void* p = field_ptr(obj, field);
    if (!p) return false;
    memcpy(p, &rgba, sizeof rgba);
    return true;
}

// Dictionary ContainsKey.
bool dict_contains(Obj* dict, int64_t key, bool* ok) {
    return call_bool(dict, "ContainsKey", { arg_i32(static_cast<int32_t>(key)) }, ok);
}

// Dictionary Add.
bool dict_add(Obj* dict, int64_t key, Obj* value) {
    auto r = call(dict, "Add", { arg_i32(static_cast<int32_t>(key)), arg_obj(value) });
    return !r.exception_thrown;
}

// List GetRange + add_ref.
Obj* list_range(Obj* list, int32_t start, int32_t count) {
    Obj* r = call_obj(list, "GetRange", { arg_i32(start), arg_i32(count) });
    if (r) r->add_ref();
    return r;
}

// List set_Item.
bool list_set(Obj* list, int32_t index, Obj* item) {
    auto r = call(list, "set_Item", { arg_i32(index), arg_obj(item) });
    return !r.exception_thrown;
}

// List Add.
bool list_add(Obj* list, Obj* item) {
    auto r = call(list, "Add", { arg_obj(item) });
    return !r.exception_thrown;
}

// ---- P2 + P3: text and texture ------------------------------------------

// set_Message with compare-before-write (the old stages.cpp set_text).
bool set_text(Obj* text_obj, const char* utf8) {
    if (!text_obj) return false;
    std::string want(utf8);
    if (str(call_obj(text_obj, "get_Message")) == want) return true;   // identical = skip (R7 / crash guard)
    Obj* ms = new_string(want);
    if (!ms) return false;
    ms->add_ref();
    call(text_obj, "set_Message", { arg_obj(ms) });
    ms->release();
    return true;
}

// Create a TextureResourceHolder for a resource path.
Obj* tex_load(const char* resource_path) {
    auto* rm = api().resource_manager();
    auto* res = rm ? rm->create_resource("via.render.TextureResource", resource_path)
                   : nullptr;
    if (!res) return nullptr;
    res->add_ref();
    auto fn = api().sdk()->resource->create_holder;
    if (!fn) return nullptr;
    Obj* h = reinterpret_cast<Obj*>(
        fn(reinterpret_cast<REFrameworkResourceHandle>(res),
           "via.render.TextureResourceHolder"));
    if (!h) return nullptr;
    h->add_ref();
    g_tex_frames[h] = g_frame;
    return h;
}

// True once >= kLoadFrames (60) frames have passed since creation.
bool tex_ready(Obj* holder) {
    auto it = g_tex_frames.find(holder);
    if (it == g_tex_frames.end()) return true;   // unknown holder = assume ready
    return g_frame - it->second >= kLoadFrames;
}

// setTexture with resource-equality compare-before-write (the old stages.cpp
// set_texture).  resource_of reads the TextureResourceHolder's native resource
// at offset +0x10.
bool set_tex(Obj* tex_obj, Obj* holder) {
    if (!tex_obj || !holder) return false;
    Obj* cur = call_obj(tex_obj, "getTexture");
    auto resource_of = [](Obj* h) -> uint64_t {
        return h ? *reinterpret_cast<const uint64_t*>(
            reinterpret_cast<const uint8_t*>(h) + 0x10) : 0;
    };
    if (cur && resource_of(cur) == resource_of(holder)) return true;   // same texture
    call(tex_obj, "setTexture", { arg_obj(holder) });
    return true;
}

} // namespace slots::ui::legacy

#endif // SLOTS_LEGACY_UI_FALLBACK
