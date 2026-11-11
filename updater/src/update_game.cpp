// SF6 Mod Suite updater: what it asks of the game (update_game.hpp). Built with /EHa: a fault in a game call is caught by catch (...).
#include "update_game.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <vector>

#pragma warning(push, 0)                // REFramework's header: size_t -> unsigned, an unused local
#include <reframework/API.hpp>
#pragma warning(pop)

#include "update_fs.hpp"

namespace sf6up::game {

namespace {

using API = reframework::API;
using TD = API::TypeDefinition;
using Method = API::Method;
using Obj = API::ManagedObject;

void* arg_i32(int32_t v) { return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(v))); }

TD* type(const char* name) {
    auto& a = API::get();
    return a ? a->tdb()->find_type(name) : nullptr;
}

// A method by name and number of parameters, on the type or a parent
Method* method_of(TD* td, const char* name, int nparams) {
    for (TD* t = td; t; t = t->get_parent_type()) {
        if (Method* m = t->find_method(name))
            if (nparams < 0 || m->get_num_params() == (uint32_t)nparams) return m;
        for (Method* m : t->get_methods())
            if (m && std::strcmp(m->get_name(), name) == 0 && (nparams < 0 || m->get_num_params() == (uint32_t)nparams)) return m;
    }
    return nullptr;
}

bool is_managed(void* p) {
    auto& a = API::get();
    return p && a && a->sdk()->managed_object->is_managed_object(p);
}

// A field's address on an object (its type or a parent's)
void* field_ptr(Obj* o, const char* name) {
    if (!is_managed(o)) return nullptr;
    for (TD* t = o->get_type_definition(); t; t = t->get_parent_type())
        if (API::Field* f = t->find_field(name)) return f->is_static() ? nullptr : f->get_data_raw(o, false);
    return nullptr;
}

Obj* field_obj(Obj* o, const char* name) {
    void* p = field_ptr(o, name);
    Obj* v = p ? *reinterpret_cast<Obj**>(p) : nullptr;
    return is_managed(v) ? v : nullptr;
}

bool call(Method* m, Obj* self, std::vector<void*> args, reframework::InvokeRet* out) {
    if (!m) return false;
    *out = m->invoke(self, args);
    return !out->exception_thrown;
}

bool call_named(Obj* self, const char* name, int nparams, std::vector<void*> args, reframework::InvokeRet* out) {
    if (!is_managed(self)) return false;
    return call(method_of(self->get_type_definition(), name, nparams), self, std::move(args), out);
}

Obj* new_string(const std::string& s) {
    auto& a = API::get();
    return a ? reinterpret_cast<Obj*>(a->sdk()->functions->create_managed_string_normal(s.c_str())) : nullptr;
}

Obj* g_box = nullptr;
std::string g_third;                // the third button's word, still to be written on the box (empty: nothing to write)
int g_third_tries = 0;

// The third button of the game's three-button box reads the game's "Cancel": our word instead. The box's handle holds its Param, whose
// SimpleList_3 has one item per button, each with a text that carries a message id; the id is emptied and the word written, once.
// False while the buttons are not built yet.
bool label_third(Obj* box, const std::string& word) {
    Obj* param = field_obj(box, "<Param>k__BackingField");
    Obj* list = param ? field_obj(param, "SimpleList_3") : nullptr;
    if (!list) return false;
    reframework::InvokeRet r{};
    if (!call_named(list, "get_Items", 0, {}, &r)) return false;
    Obj* items = static_cast<Obj*>(r.ptr);
    if (!call_named(items, "get_Count", 0, {}, &r) || (int32_t)r.dword < 3) return false;
    if (!call_named(items, "get_Item", 1, { arg_i32(2) }, &r)) return false;
    Obj* node = static_cast<Obj*>(r.ptr);
    if (!call_named(node, "get_Child", 0, {}, &r)) return false;
    node = static_cast<Obj*>(r.ptr);
    for (int i = 0; i < 16 && is_managed(node); ++i) {
        if (node->get_type_definition()->get_full_name() == "via.gui.Text") {
            static const uint8_t kNoId[16] = {};
            Obj* str = new_string(word);
            if (!str) return false;
            reframework::InvokeRet v{};
            if (!call_named(node, "set_MessageId", 1, { const_cast<uint8_t*>(kNoId) }, &v)) return false;
            return call_named(node, "set_Message", 1, { str }, &v);
        }
        if (!call_named(node, "get_Next", 0, {}, &r)) return false;
        node = static_cast<Obj*>(r.ptr);
    }
    return false;
}

Obj* flow_map() {
    auto& a = API::get();
    Obj* fm = a ? a->get_managed_singleton("app.bFlowManager") : nullptr;
    Obj* work = fm ? field_obj(fm, "m_flow_work") : nullptr;
    return work ? field_obj(work, "_FlowMap") : nullptr;
}

}  // namespace

bool box_open(const std::string& text, const std::string& title, Buttons buttons, const std::string& third) {
    try {
        if (g_box) return false;
        Method* m = method_of(type("app.UIFlowDialog.MessageBox"), "Start", 7);
        if (!m) return false;
        Obj* msg = new_string(text);
        Obj* ttl = new_string(title);
        if (!msg || !ttl) return false;
        // app.DialogButtonStyle: OK 1, YesNo 4, YesNoCancel 5. The cursor starts on No, Back answers No (OK: the only button).
        const int32_t style = buttons == Buttons::Ok ? 1 : buttons == Buttons::YesNoThird ? 5 : 4;
        const int32_t no = buttons == Buttons::Ok ? 0 : 1;
        reframework::InvokeRet r{};
        if (!call(m, nullptr, { msg, ttl, arg_i32(no), arg_i32(no), arg_i32(style), arg_i32(-1), arg_i32(1) }, &r)) return false;
        Obj* box = static_cast<Obj*>(r.ptr);
        if (!is_managed(box)) return false;
        box->add_ref();
        g_box = box;
        g_third = buttons == Buttons::YesNoThird ? third : std::string();
        g_third_tries = 0;
        return true;
    } catch (...) {
        return false;
    }
}

int box_poll() {
    if (!g_box) return -2;
    try {
        reframework::InvokeRet r{};
        const bool read = call_named(g_box, "get_IsEnd", 0, {}, &r);
        if (read && !(r.byte & 1)) {
            if (!g_third.empty()) {                 // the buttons are built a frame or two after Start
                bool done = false;
                try { done = label_third(g_box, g_third); } catch (...) {}
                if (done || ++g_third_tries >= 120) g_third.clear();
            }
            return -1;
        }
        int answer = -2;                // unreadable: the box is lost, no answer
        if (read) {
            reframework::InvokeRet v{};
            if (call(method_of(type("app.UIFlowDialog.MessageBox"), "GetSelectValue", 1), nullptr, { g_box }, &v)) {
                const int32_t sel = (int32_t)v.dword;        // the button's place: 0 Yes (OK), 1 No, 2 the third
                answer = sel == 0 ? 1 : sel == 2 ? 2 : 0;
            }
        }
        g_box->release();
        g_box = nullptr;
        g_third.clear();
        return answer;
    } catch (...) {
        g_box = nullptr;            // not released: the object may be gone
        g_third.clear();
        return -2;
    }
}

void box_drop() {
    try { if (g_box) g_box->release(); } catch (...) {}
    g_box = nullptr;
    g_third.clear();
}

bool box_is_open() { return g_box != nullptr; }

int32_t flow_id() {
    try {
        Obj* map = flow_map();
        void* p = map ? field_ptr(map, "_ID") : nullptr;
        return p ? *reinterpret_cast<int32_t*>(p) : -1;
    } catch (...) {
        return -1;
    }
}

bool in_battle() {
    try {
        Obj* map = flow_map();
        return map && field_obj(map, "m_desc") != nullptr;
    } catch (...) {
        return true;                // unreadable: not a moment to ask anything
    }
}

bool game_dialog_up() {
    try {
        auto& a = API::get();
        Obj* mgr = a ? a->get_managed_singleton("app.UIFlowManager") : nullptr;
        Obj* handles = mgr ? field_obj(mgr, "_Handles") : nullptr;
        if (!handles) return false;
        reframework::InvokeRet n{};
        if (!call_named(handles, "get_Count", 0, {}, &n)) return false;
        const int32_t count = (int32_t)n.dword;
        for (int32_t i = 0; i < count && i < 64; ++i) {
            reframework::InvokeRet it{};
            if (!call_named(handles, "get_Item", 1, { arg_i32(i) }, &it)) continue;
            Obj* h = static_cast<Obj*>(it.ptr);
            Obj* param = h ? field_obj(h, "<Param>k__BackingField") : nullptr;
            if (param && param->get_type_definition()->get_full_name() == "app.UIFlowDialog.MessageBoxParam") return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

namespace {
const char* lang_code(int32_t v) {
    switch (v) {
    case 0: return "ja"; case 1: return "en"; case 2: return "fr"; case 3: return "it"; case 4: return "de";
    case 5: return "es"; case 6: return "ru"; case 7: return "pl"; case 9: case 10: return "pt";
    case 11: return "ko"; case 12: return "zh-Hant"; case 13: return "zh-Hans"; case 21: return "ar"; case 32: return "es-419";
    default: return nullptr;
    }
}
}  // namespace

std::string language() {
    int32_t val = -1;
    try {
        TD* opt = type("app.Option");
        Method* get = opt ? method_of(opt, "GetOptionValue", 1) : nullptr;
        API::Field* list_f = opt ? opt->find_field("LanguageList") : nullptr;
        if (get && list_f) {
            reframework::InvokeRet ri{};
            void* slot = call(get, nullptr, { arg_i32(611) }, &ri) ? list_f->get_data_raw(nullptr, false) : nullptr;   // ValueType.DispLanguage
            Obj* arr = slot ? *reinterpret_cast<Obj**>(slot) : nullptr;
            const int32_t idx = (int32_t)ri.dword;
            reframework::InvokeRet len{};
            if (is_managed(arr) && idx >= 0 && call_named(arr, "get_Length", 0, {}, &len) && idx < (int32_t)len.dword) {
                reframework::InvokeRet v{};
                if (call_named(arr, "get_Item", 1, { arg_i32(idx) }, &v)) val = (int32_t)v.dword;
            }
        }
        if (val < 0) {
            reframework::InvokeRet r{};
            if (call(method_of(type("via.SystemService"), "get_Language", 0), nullptr, {}, &r)) val = (int32_t)r.dword;
        }
    } catch (...) {
    }
    const char* c = lang_code(val);
    return c ? c : "en";
}

std::string package_version(const KnownPackage& k, const std::wstring& root) {
    const std::string proof = k.proof;
    const std::wstring base = widen(proof.substr(proof.find_last_of('/') + 1));
    HMODULE h = GetModuleHandleW(base.c_str());
    if (!h) return {};
    // the module running is the game folder's file, not another copy of the same name
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(h, path.data(), (DWORD)path.size()));
    const std::wstring want = join(root, proof);
    if (CompareStringOrdinal(path.c_str(), (int)path.size(), want.c_str(), (int)want.size(), TRUE) != CSTR_EQUAL) return {};
    const char* v = nullptr;
    try {
        using VersionFn = const char* (*)();
        if (auto fn = reinterpret_cast<VersionFn>(GetProcAddress(h, "sf6suite_package_version"))) v = fn();
        if (!v && std::strcmp(k.id, "native_core") == 0) {
            using GetApi = const void* (__cdecl*)(uint32_t);
            if (auto get = reinterpret_cast<GetApi>(GetProcAddress(h, "sf6core_get_api"))) {
                // SF6CoreAPI: abi_version, struct_size, then get_version (sf6core_api.h, append-only)
                struct Head { uint32_t abi, size; const char* (__cdecl* get_version)(void); };
                if (const Head* api = static_cast<const Head*>(get(1)); api && api->size >= sizeof(Head) && api->get_version) v = api->get_version();
            }
        }
    } catch (...) {
        v = nullptr;
    }
    if (!v) return {};
    std::string s(v, strnlen(v, 32));
    return parse_version(s) ? s : std::string();
}

}  // namespace sf6up::game
