// slots_ui.cpp -- display operation dispatch: sf6core when present, else legacy.
//
// Core discovery: GetModuleHandleW + GetProcAddress, per ADR-0002/0007.
// SF6_CostumeSlotsNative.dll initializes BEFORE sf6core_native.dll (case-sensitive
// name order), so register_client is called before the core's own init -- it is
// queued and replayed.  Display calls may return ERR_NOT_READY or DISABLED early;
// callers retry naturally on later frames (the same way they tolerate a screen
// that is not ready yet).
#include "slots_ui.hpp"

namespace slots::ui {
namespace {

const SF6CoreAPI* g_api    = nullptr;
SF6CoreClient*    g_client = nullptr;
bool              g_logged = false;

bool core_ok() { return g_api != nullptr && g_client != nullptr; }

} // namespace

// ---- lifecycle ----------------------------------------------------------

void init() {
    HMODULE h = GetModuleHandleW(L"sf6core_native.dll");
    if (!h) return;
    auto get_api_fn = reinterpret_cast<SF6CoreGetApiFn>(
        GetProcAddress(h, "sf6core_get_api"));
    if (!get_api_fn) return;
    g_api = get_api_fn(SF6CORE_ABI_VERSION);
    if (!g_api || !SF6CORE_HAS(g_api, get_display_stats)) {
        g_api = nullptr;
        return;
    }
    SF6CoreClientDesc desc{};
    desc.struct_size    = sizeof(desc);
    desc.client_id      = "SF6_SlotsLoader";
    desc.client_version = nullptr;
    desc.priority       = 100;
    SF6CoreStatus st = g_api->register_client(&desc, &g_client);
    if (st != SF6CORE_OK) {
        logf("slots_ui: register_client failed (%d)", static_cast<int>(st));
        g_api    = nullptr;
        g_client = nullptr;
    }
}

void log_path() {
    if (g_logged) return;
    g_logged = true;
    if (core_ok()) {
        logf("slots_ui: display via sf6core %s", g_api->get_version());
    }
#ifdef SLOTS_LEGACY_UI_FALLBACK
    else {
        logf("slots_ui: display via legacy fallback");
    }
#else
    else {
        logf("slots_ui: sf6core not installed: display disabled");
    }
#endif
}

// ---- P1: data injection -------------------------------------------------

Obj* clone(Obj* src) {
    if (core_ok())
        return reinterpret_cast<Obj*>(g_api->obj_clone(g_client, src));
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::clone(src);
#else
    return nullptr;
#endif
}

bool set_i64(Obj* obj, const char* field, int64_t v) {
    if (core_ok())
        return g_api->obj_set_i64(g_client, obj, field, v) == SF6CORE_OK;
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::set_i64(obj, field, v);
#else
    return false;
#endif
}

bool set_ref(Obj* obj, const char* field, Obj* value) {
    if (core_ok())
        return g_api->obj_set_ref(g_client, obj, field, value) == SF6CORE_OK;
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::set_ref(obj, field, value);
#else
    return false;
#endif
}

bool set_color(Obj* obj, const char* field, uint32_t rgba) {
    if (core_ok())
        return g_api->obj_set_color(g_client, obj, field, rgba) == SF6CORE_OK;
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::set_color(obj, field, rgba);
#else
    return false;
#endif
}

bool dict_contains(Obj* dict, int64_t key, bool* ok) {
    if (core_ok()) {
        if (ok) *ok = true;
        return g_api->dict_contains(dict, key) != 0;
    }
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::dict_contains(dict, key, ok);
#else
    if (ok) *ok = false;
    return false;
#endif
}

bool dict_add(Obj* dict, int64_t key, Obj* value) {
    if (core_ok())
        return g_api->dict_add(g_client, dict, key, value) == SF6CORE_OK;
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::dict_add(dict, key, value);
#else
    return false;
#endif
}

Obj* list_range(Obj* list, int32_t start, int32_t count) {
    if (core_ok())
        return reinterpret_cast<Obj*>(
            g_api->list_get_range(g_client, list, start, count));
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::list_range(list, start, count);
#else
    return nullptr;
#endif
}

bool list_set(Obj* list, int32_t index, Obj* item) {
    if (core_ok())
        return g_api->list_set_item(g_client, list, index, item) == SF6CORE_OK;
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::list_set(list, index, item);
#else
    return false;
#endif
}

bool list_add(Obj* list, Obj* item) {
    if (core_ok())
        return g_api->list_add(g_client, list, item) == SF6CORE_OK;
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::list_add(list, item);
#else
    return false;
#endif
}

// ---- P2 + P3: text and texture ------------------------------------------

void set_effective(int fighter, int shown_costume, int real_costume, int real_colour) {
    if (core_ok() && SF6CORE_HAS(g_api, sel_set_effective))
        g_api->sel_set_effective(g_client, fighter, shown_costume, real_costume, real_colour);
}

bool set_text(Obj* text_obj, const char* utf8) {
    if (core_ok())
        return g_api->ui_set_text(g_client, text_obj, utf8) == SF6CORE_OK;
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::set_text(text_obj, utf8);
#else
    return false;
#endif
}

Obj* tex_load(const char* resource_path) {
    if (core_ok()) {
        void* out = nullptr;
        SF6CoreStatus st = g_api->ui_texture_load(g_client, resource_path, &out);
        return st == SF6CORE_OK ? reinterpret_cast<Obj*>(out) : nullptr;
    }
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::tex_load(resource_path);
#else
    return nullptr;
#endif
}

bool tex_ready(Obj* holder) {
    if (core_ok())
        return g_api->ui_texture_ready(holder) != 0;
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::tex_ready(holder);
#else
    return false;
#endif
}

bool set_tex(Obj* tex_obj, Obj* holder) {
    if (core_ok())
        return g_api->ui_set_texture(g_client, tex_obj, holder) == SF6CORE_OK;
#ifdef SLOTS_LEGACY_UI_FALLBACK
    return legacy::set_tex(tex_obj, holder);
#else
    return false;
#endif
}

// ---- GUI material texture (core only) -------------------------------------

bool has_mat_tex() {
    return core_ok() && SF6CORE_HAS(g_api, ui_get_material_texture);
}

Obj* get_mat_tex(Obj* material, const char* param, int* status) {
    if (status) *status = -1;
    if (!has_mat_tex()) return nullptr;
    void* out = nullptr;
    SF6CoreStatus st = g_api->ui_get_material_texture(g_client, material, param, &out);
    if (status) *status = static_cast<int>(st);
    return st == SF6CORE_OK ? reinterpret_cast<Obj*>(out) : nullptr;
}

bool set_mat_tex(Obj* material, const char* param, Obj* holder) {
    if (!has_mat_tex()) return false;
    return g_api->ui_set_material_texture(g_client, material, param, holder) == SF6CORE_OK;
}

} // namespace slots::ui
