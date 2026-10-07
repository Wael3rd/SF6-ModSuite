#pragma once

// ll_common.hpp - REFramework plugin helpers: logging, Method::invoke argument slots, field access through
// the type chain, method lookup with a bare-name fallback, managed strings. Header-only.
// Credit: LL (used with his permission).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#pragma warning(push, 0)
#include <reframework/API.hpp>
#pragma warning(pop)

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ll {
using API = reframework::API;
using InvokeRet = reframework::InvokeRet;

// Opt-in verbose diagnostics; compile-time/debugger-only toggle.
inline bool g_diag = false;

inline constexpr const char* kLogPrefix = "[sf6core] ";

// Accessor for the initialized REFramework API.
// Throws std::runtime_error (via API::get) if called before initialization;
// entry-point code guards for that. Defined above log_v because the logging
// helpers call it.
inline reframework::API& reapi() {
    return *reframework::API::get();
}

// ---------------------------------------------------------------------------
// Logging (hardened
// variadic path): every call formats into a local 512-byte buffer and passes
// the result through a single "%s%s" call, so a bad format string can never
// crash the engine's vsnprintf. All output is prefixed with kLogPrefix.
// ---------------------------------------------------------------------------
enum class LogLevel { Info, Warn, Error };

inline void log_v(LogLevel level, const char* format, va_list ap) {
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), format, ap);
    auto& api = reapi();
    switch (level) {
    case LogLevel::Warn:  api.log_warn("%s%s", kLogPrefix, buf); break;
    case LogLevel::Error: api.log_error("%s%s", kLogPrefix, buf); break;
    default:              api.log_info("%s%s", kLogPrefix, buf); break;
    }
}

inline void log_info(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    log_v(LogLevel::Info, format, ap);
    va_end(ap);
}

inline void log_warn(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    log_v(LogLevel::Warn, format, ap);
    va_end(ap);
}

inline void log_error(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    log_v(LogLevel::Error, format, ap);
    va_end(ap);
}

// Verbose diagnostics line; only emitted when g_diag is true.
inline void log_diag(const char* format, ...) {
    if (!g_diag) {
        return;
    }
    va_list ap;
    va_start(ap, format);
    log_v(LogLevel::Info, format, ap);
    va_end(ap);
}


// ---------------------------------------------------------------------------
// Argument-slot encoders for Method::invoke.
// Method::invoke passes each 8-byte slot as the argument value
// (RETypeDB.cpp in_data semantics). Scalars are passed by reinterpreting the
// value as the slot address; managed reference arguments use the object pointer
// itself.
// ---------------------------------------------------------------------------
inline void* arg_u32(uint32_t value) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
}

inline void* arg_i32(int32_t value) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(value)));
}

inline void* arg_bool(bool value) {
    return reinterpret_cast<void*>(static_cast<uintptr_t>(value ? 1u : 0u));
}

inline void* arg_f32(float value) {
    // RE Engine invoke wrappers promote System.Single argument slots to double.
    const double promoted=value;
    uintptr_t bits=0;
    static_assert(sizeof(bits)==sizeof(promoted));
    std::memcpy(&bits,&promoted,sizeof(bits));
    return reinterpret_cast<void*>(bits);
}

// ---------------------------------------------------------------------------
// Field helpers. All take a managed object, walk the object's own type
// definition chain (mirrors Lua obj:get_field: the VM finds inherited fields
// too), and return false / zero / nullptr when the object or field is absent.
// Scalar reads/writes go through API::Field::get_data_raw(obj, false) — the
// object-level (non-value-type) path.
// ---------------------------------------------------------------------------
// Walks the object's own type definition chain for the field (mirrors Lua
// obj:get_field, which finds inherited fields too; API.hpp TypeDefinition
// exposes get_parent_type() at line 591).
inline API::Field* find_field_walk(API::TypeDefinition* td, const char* name) {
    if (td == nullptr || name == nullptr) {
        return nullptr;
    }
    for (auto* t = td; t != nullptr; t = t->get_parent_type()) {
        if (auto* f = t->find_field(name); f != nullptr) {
            return f;
        }
    }
    return nullptr;
}

inline bool read_field_raw(API::ManagedObject* obj, const char* name, void* out, size_t size) {
    if (obj == nullptr || name == nullptr || out == nullptr) {
        return false;
    }
    auto* field = find_field_walk(obj->get_type_definition(), name);
    if (field == nullptr) {
        return false;
    }
    // Object-level (non-value-type) storage path.
    void* raw = field->get_data_raw(obj, false);
    if (raw == nullptr) {
        return false;
    }
    std::memcpy(out, raw, size);
    return true;
}

inline bool read_field_u32(API::ManagedObject* obj, const char* name, uint32_t& out) {
    return read_field_raw(obj, name, &out, sizeof(out));
}

inline bool read_field_i32(API::ManagedObject* obj, const char* name, int32_t& out) {
    return read_field_raw(obj, name, &out, sizeof(out));
}

inline bool read_field_u64(API::ManagedObject* obj, const char* name, uint64_t& out) {
    return read_field_raw(obj, name, &out, sizeof(out));
}

inline bool read_field_f32(API::ManagedObject* obj, const char* name, float& out) {
    return read_field_raw(obj, name, &out, sizeof(out));
}

// RE Engine stores System.Boolean fields as 1 byte.
inline bool read_field_bool(API::ManagedObject* obj, const char* name, bool& out) {
    uint8_t byte = 0;
    if (!read_field_raw(obj, name, &byte, sizeof(byte))) {
        return false;
    }
    out = byte != 0;
    return true;
}

// Reads a reference-typed field slot. Returns the raw pointer WITHOUT
// add_ref — borrowed; the field keeps the object alive while the owner lives.
inline API::ManagedObject* read_field_object(API::ManagedObject* obj, const char* name) {
    if (obj == nullptr || name == nullptr) {
        return nullptr;
    }
    auto* field = find_field_walk(obj->get_type_definition(), name);
    if (field == nullptr) {
        return nullptr;
    }
    void* raw = field->get_data_raw(obj, false);
    if (raw == nullptr) {
        return nullptr;
    }
    return *reinterpret_cast<API::ManagedObject**>(raw);
}

inline bool write_field_raw(API::ManagedObject* obj, const char* name, const void* value, size_t size) {
    if (obj == nullptr || name == nullptr || value == nullptr) {
        return false;
    }
    auto* field = find_field_walk(obj->get_type_definition(), name);
    if (field == nullptr) {
        return false;
    }
    void* raw = field->get_data_raw(obj, false);
    if (raw == nullptr) {
        return false;
    }
    std::memcpy(raw, value, size);
    return true;
}

inline bool write_field_i32(API::ManagedObject* obj, const char* name, int32_t value) {
    return write_field_raw(obj, name, &value, sizeof(value));
}

inline bool write_field_u32(API::ManagedObject* obj, const char* name, uint32_t value) {
    return write_field_raw(obj, name, &value, sizeof(value));
}

inline bool write_field_f32(API::ManagedObject* obj, const char* name, float value) {
    return write_field_raw(obj, name, &value, sizeof(value));
}

// ---------------------------------------------------------------------------
// Method helpers.
//
// find_method_full tries the TDB name lookup with the full signature
// ("set_Message(System.String)"); on miss it scans td->get_methods() for a
// bare-name match (text before the first '('), ignoring the parameter text —
// It walks base types.
// ---------------------------------------------------------------------------
inline API::Method* find_method_full(API::TypeDefinition* td, const char* full_name_with_params) {
    if (td == nullptr || full_name_with_params == nullptr) {
        return nullptr;
    }

    if (auto* exact = td->find_method(full_name_with_params); exact != nullptr) {
        return exact;
    }

    // Fallback: bare-name match (text before the first '('), ignoring the
    // parameter text, walking base types for inherited methods.
    const std::string_view requested{full_name_with_params};
    const auto paren = requested.find('(');
    const auto bare = requested.substr(0, paren);

    for (auto* t = td; t != nullptr; t = t->get_parent_type()) {
        for (auto* method : t->get_methods()) {
            if (method == nullptr || method->get_name() == nullptr) {
                continue;
            }
            if (bare == method->get_name()) {
                return method;
            }
        }
    }

    return nullptr;
}

inline API::Method* find_method_noargs(API::TypeDefinition* td, const char* bare_name) {
    if (td == nullptr || bare_name == nullptr) {
        return nullptr;
    }

    for (auto* t = td; t != nullptr; t = t->get_parent_type()) {
        if (auto* exact = t->find_method(bare_name); exact != nullptr && exact->get_num_params() == 0) {
            return exact;
        }
        for (auto* method : t->get_methods()) {
            if (method == nullptr || method->get_name() == nullptr) {
                continue;
            }
            if (std::string_view{bare_name} == method->get_name() && method->get_num_params() == 0) {
                return method;
            }
        }
    }

    return nullptr;
}

// Thin invoke wrappers. All log (warn level, method name included) when
// ret.exception_thrown is set and return false / nullptr respectively.
// obj may be nullptr for static methods (mirrors Method::invoke passing the
// object straight through).
inline reframework::InvokeRet invoke_ret(API::ManagedObject* obj, API::Method* method, std::vector<void*> args) {
    if (method == nullptr) {
        return {};
    }

    auto ret = method->invoke(obj, args);
    if (ret.exception_thrown) {
        log_warn("invoke of %s threw an exception",
                 method->get_name() != nullptr ? method->get_name() : "(unknown)");
    }
    return ret;
}

inline bool invoke_void(API::ManagedObject* obj, API::Method* method, std::vector<void*> args) {
    return !invoke_ret(obj, method, std::move(args)).exception_thrown;
}

// Returns (API::ManagedObject*)ret.ptr — borrowed (the VM bridge holds the
// reference for the current frame); nullptr on exception or null result.
inline API::ManagedObject* invoke_object_ret(API::ManagedObject* obj, API::Method* method, std::vector<void*> args) {
    auto ret = invoke_ret(obj, method, std::move(args));
    if (ret.exception_thrown) {
        return nullptr;
    }
    return reinterpret_cast<API::ManagedObject*>(ret.ptr);
}

// ---------------------------------------------------------------------------
// String helpers (Win32 WideCharToMultiByte / MultiByteToWideChar, CP_UTF8).
// ---------------------------------------------------------------------------

// Reads a RE Engine System.String. Layout (evidence: upstream
// /tmp/ref-src/shared/sdk/ReClass_Internal_SF6.hpp:1045-1051 —
// `class SystemString : public REManagedObject { int32_t size; //0x0010
// wchar_t data[...]; //0x0014 }`, and the same memory read is what
// utility::re_string::get_string does at REString.hpp:100-106).
// Returns "" on null.
inline std::string managed_string_to_utf8(API::ManagedObject* str) {
    if (str == nullptr) {
        return {};
    }

    // RE Engine System.String layout (evidence: upstream
    // /tmp/ref-src/shared/sdk/ReClass_Internal_SF6.hpp:1045-1051 —
    // `class SystemString : public REManagedObject { int32_t size; //0x0010
    // wchar_t data[...]; //0x0014 }`); the identical memory read is what
    // utility::re_string::get_string does (REString.hpp:100-106).
    const auto size = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(str) + 0x10);
    if (size <= 0) {
        return {};
    }

    const auto* data = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(str) + 0x14);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, data, size, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }

    std::string out(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, data, size, out.data(), bytes, nullptr, nullptr) <= 0) {
        return {};
    }
    return out;
}

// Converts UTF-8 -> UTF-16 -> reapi().create_managed_string(wstr.c_str()).
// The returned SystemString is a fresh VM object (upstream
// REContext.cpp:711-747 builds it via System.String::Clone). Like the Lua
// path, we do NOT manually release it after passing it into a managed call:
// the invoke bridge pins argument objects for the call and the VM GC reclaims
// the string once nothing references it. Only release() it yourself if you
// add_ref'd or stored it beyond the call.
inline API::ManagedObject* create_utf8_string(const std::string& utf8) {
    int wlen = 0;
    if (!utf8.empty()) {
        wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0) {
            return nullptr;
        }
    }

    std::wstring wstr(static_cast<size_t>(wlen), L'\0');
    if (wlen > 0 && MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wstr.data(), wlen) <= 0) {
        return nullptr;
    }

    return reapi().create_managed_string(wstr.c_str());
}

// Writes text into a via.gui.Text widget by invoking set_Message(System.String),
// resolved via find_method_full walking the widget's type chain.
// Mirrors the Lua set_message path.
//
// set_Message is the wiki-documented safe write; refresh() from external
// callers is a documented crash source and is intentionally not called.
inline bool write_text_message(API::ManagedObject* via_gui_text, const std::string& utf8) {
    if (via_gui_text == nullptr) {
        return false;
    }

    auto* set_message = find_method_full(via_gui_text->get_type_definition(), "set_Message(System.String)");
    if (set_message == nullptr) {
        log_warn("write_text_message: set_Message(System.String) not found on %s",
                 via_gui_text->get_type_definition() != nullptr
                     ? via_gui_text->get_type_definition()->get_full_name().c_str()
                     : "(no type)");
        return false;
    }

    auto* str = create_utf8_string(utf8);
    if (str == nullptr) {
        log_warn("write_text_message: failed to create managed string");
        return false;
    }

    // arg slots are raw values: object pointer itself (RETypeDB.cpp in_data semantics)
    if (!invoke_void(via_gui_text, set_message, {(void*)str})) {
        return false;
    }
    return true;
}

} // namespace ll
