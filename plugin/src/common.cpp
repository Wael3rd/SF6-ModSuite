// SF6 Slots — native plugin: game access helpers, files, JSON, context.
#include "slots.hpp"
#include "perf.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace slots {

uint32_t g_frame = 0;
std::atomic<uint32_t> g_events{0};

// ---------------------------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------------------------

static const char* kLog    = "reframework/data/SF6_CostumeSlots_data/native_log.txt";
static const char* kLogOld = "reframework/data/SF6_CostumeSlots_data/native_log.old.txt";
static size_t g_log_bytes = 0;

void log_open() {
    CreateDirectoryA("reframework/data/SF6_CostumeSlots_data", nullptr);
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (GetFileAttributesExA(kLog, GetFileExInfoStandard, &d) && d.nFileSizeLow > 512 * 1024) {
        DeleteFileA(kLogOld);
        MoveFileA(kLog, kLogOld);
    }
    g_log_bytes = 0;
}

// A line is formatted here and written by perf's background writer (in the order logged): opening, writing and closing
// the file for each line cost the game thread up to milliseconds when a model's options were swapped (a line per part,
// 24/10). Before the first frame (plugin init: no thread waited on there) the line is written here, as before; while
// the game closes, logf waits for the line to be written (200 ms at most).
void logf(const char* fmt, ...) {
    if (g_log_bytes > 2 * 1024 * 1024) return;          // one session never writes more than 2 MB
    time_t now = time(nullptr);
    struct tm t; localtime_s(&t, &now);
    char head[32];
    int n = snprintf(head, sizeof head, "%02d:%02d:%02d f%u ", t.tm_hour, t.tm_min, t.tm_sec, g_frame);
    std::string line(head, static_cast<size_t>(n > 0 ? n : 0));
    char buf[1024];
    va_list a; va_start(a, fmt);
    int m = vsnprintf(buf, sizeof buf, fmt, a);
    va_end(a);
    if (m >= static_cast<int>(sizeof buf)) {
        std::string big(static_cast<size_t>(m) + 1, '\0');
        va_start(a, fmt);
        vsnprintf(big.data(), big.size(), fmt, a);
        va_end(a);
        line.append(big.data(), static_cast<size_t>(m));
    } else if (m > 0) {
        line.append(buf, static_cast<size_t>(m));
    }
    line += '\n';
    if (g_frame == 0) {
        FILE* f = fopen(kLog, "ab");
        if (!f) return;
        fwrite(line.data(), 1, line.size(), f);
        fclose(f);
    } else {
        perf::bg_write(kLog, line);
        if (g_closing.load(std::memory_order_relaxed)) perf::bg_flush(200);
    }
    g_log_bytes += static_cast<size_t>(n + (m > 0 ? m : 0) + 1);
}

// ---------------------------------------------------------------------------------------------
// Fields
// ---------------------------------------------------------------------------------------------

API::Field* field_of(TD* td, const char* name) {
    for (auto* t = td; t; t = t->get_parent_type())
        if (auto* f = t->find_field(name)) return f;
    return nullptr;
}

void* field_ptr(Obj* o, const char* name) {
    if (!o) return nullptr;
    auto* f = field_of(o->get_type_definition(), name);
    return f ? f->get_data_raw(o, false) : nullptr;
}

Obj* get_obj(Obj* o, const char* name) {
    void* p = field_ptr(o, name);
    return p ? *reinterpret_cast<Obj**>(p) : nullptr;
}
bool get_i32(Obj* o, const char* name, int32_t& out) {
    void* p = field_ptr(o, name); if (!p) return false;
    memcpy(&out, p, sizeof out); return true;
}
bool get_u32(Obj* o, const char* name, uint32_t& out) {
    void* p = field_ptr(o, name); if (!p) return false;
    memcpy(&out, p, sizeof out); return true;
}
bool get_bool(Obj* o, const char* name, bool& out) {
    void* p = field_ptr(o, name); if (!p) return false;
    out = *reinterpret_cast<uint8_t*>(p) != 0; return true;
}
bool set_i32(Obj* o, const char* name, int32_t v) {
    void* p = field_ptr(o, name); if (!p) return false;
    memcpy(p, &v, sizeof v); return true;
}
bool set_u32(Obj* o, const char* name, uint32_t v) {
    void* p = field_ptr(o, name); if (!p) return false;
    memcpy(p, &v, sizeof v); return true;
}
bool set_bool(Obj* o, const char* name, bool v) {
    void* p = field_ptr(o, name); if (!p) return false;
    *reinterpret_cast<uint8_t*>(p) = v ? 1 : 0; return true;
}
bool set_obj(Obj* o, const char* name, Obj* v) {
    // raw pointer write, no reference counting: a new object is add_ref'd by its creator, and a
    // swap of two referenced objects keeps both counts right
    void* p = field_ptr(o, name); if (!p) return false;
    *reinterpret_cast<Obj**>(p) = v;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Methods
// ---------------------------------------------------------------------------------------------

Method* method_of(TD* td, const char* name, int nparams) {
    if (!td) return nullptr;
    if (auto* m = td->find_method(name))
        if (nparams < 0 || m->get_num_params() == static_cast<uint32_t>(nparams)) return m;
    std::string bare = name;
    auto paren = bare.find('(');
    if (paren != std::string::npos) bare.resize(paren);
    for (auto* t = td; t; t = t->get_parent_type()) {
        for (auto* m : t->get_methods()) {
            if (!m || !m->get_name() || bare != m->get_name()) continue;
            if (nparams >= 0 && m->get_num_params() != static_cast<uint32_t>(nparams)) continue;
            return m;
        }
    }
    return nullptr;
}

Ret call(Method* m, Obj* o, std::vector<void*> args) {
    if (!m) { Ret r; r.exception_thrown = true; return r; }
    return m->invoke(o, args);
}
Ret call(Obj* o, const char* name, std::vector<void*> args) {
    if (!o) { Ret r; r.exception_thrown = true; return r; }
    int n = static_cast<int>(args.size());
    return call(method_of(o->get_type_definition(), name, n), o, std::move(args));
}
Obj* call_obj(Method* m, Obj* o, std::vector<void*> args) {
    auto r = call(m, o, std::move(args));
    return (!r.exception_thrown && r.ptr) ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
}
Obj* call_obj(Obj* o, const char* name, std::vector<void*> args) {
    auto r = call(o, name, std::move(args));
    return (!r.exception_thrown && r.ptr) ? reinterpret_cast<Obj*>(r.ptr) : nullptr;
}
bool call_bool(Obj* o, const char* name, std::vector<void*> args, bool* ok) {
    auto r = call(o, name, std::move(args));
    if (ok) *ok = !r.exception_thrown;
    return !r.exception_thrown && r.byte != 0;
}
int32_t call_i32(Obj* o, const char* name, std::vector<void*> args, bool* ok) {
    auto r = call(o, name, std::move(args));
    if (ok) *ok = !r.exception_thrown;
    return r.exception_thrown ? 0 : static_cast<int32_t>(r.dword);
}

int list_count(Obj* list) {
    if (!list) return 0;
    static std::unordered_map<TD*, Method*> cache;
    auto* td = list->get_type_definition();
    auto it = cache.find(td);
    Method* m = it != cache.end() ? it->second : (cache[td] = method_of(td, "get_Count", 0));
    auto r = call(m, list);
    return r.exception_thrown ? 0 : static_cast<int>(r.dword);
}
Obj* list_item(Obj* list, int i) {
    if (!list) return nullptr;
    static std::unordered_map<TD*, Method*> cache;
    auto* td = list->get_type_definition();
    auto it = cache.find(td);
    Method* m = it != cache.end() ? it->second : (cache[td] = method_of(td, "get_Item", 1));
    return call_obj(m, list, { arg_i32(i) });
}

void* Elem::field(const char* name) const {
    if (!ok) return nullptr;
    if (vt) {
        auto* f = field_of(vt, name);
        return f ? f->get_data_raw(const_cast<uint8_t*>(ret.bytes.data()), true) : nullptr;
    }
    return field_ptr(obj, name);
}

Elem list_elem(Obj* list, int i) {
    Elem e;
    if (!list) return e;
    static std::unordered_map<TD*, std::pair<Method*, TD*>> cache;
    auto* td = list->get_type_definition();
    auto it = cache.find(td);
    if (it == cache.end()) {
        Method* m = method_of(td, "get_Item", 1);
        TD* rt = m ? m->get_return_type() : nullptr;
        it = cache.emplace(td, std::make_pair(m, (rt && rt->is_valuetype()) ? rt : nullptr)).first;
    }
    if (!it->second.first) return e;
    e.ret = call(it->second.first, list, { arg_i32(i) });
    if (e.ret.exception_thrown) return e;
    e.vt = it->second.second;
    e.obj = e.vt ? nullptr : reinterpret_cast<Obj*>(e.ret.ptr);
    e.ok = e.vt != nullptr || e.obj != nullptr;
    return e;
}

// ---------------------------------------------------------------------------------------------
// Strings
// ---------------------------------------------------------------------------------------------

std::string str(Obj* s) {
    if (!s) return {};
    auto sz = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(s) + 0x10);
    if (sz <= 0 || sz > 8192) return {};
    auto* data = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(s) + 0x14);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, data, sz, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, data, sz, out.data(), bytes, nullptr, nullptr);
    return out;
}

Obj* new_string(const std::string& utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return nullptr;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
    auto fn = api().sdk()->functions->create_managed_string;
    return fn ? reinterpret_cast<Obj*>(fn(w.c_str())) : nullptr;
}

std::string type_name(Obj* o) {
    auto* td = o ? o->get_type_definition() : nullptr;
    return td ? td->get_full_name() : std::string{};
}

// ---------------------------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------------------------

Obj* current_scene() {
    static Method* m = nullptr;
    if (!m) m = method_of(api().tdb()->find_type("via.SceneManager"), "get_CurrentScene", 0);
    auto* sm = api().get_native_singleton("via.SceneManager");
    return sm ? call_obj(m, reinterpret_cast<Obj*>(sm)) : nullptr;
}

Obj* find_components(TD* type) {
    if (!type) return nullptr;
    static std::unordered_map<TD*, Obj*> types;
    static Method* m = nullptr;
    auto it = types.find(type);
    Obj* sys_type = it != types.end() ? it->second : (types[type] = api().typeof(type->get_full_name().c_str()));
    if (!sys_type) return nullptr;
    auto* sc = current_scene();
    if (!sc) return nullptr;
    if (!m) m = method_of(sc->get_type_definition(), "findComponents(System.Type)", 1);
    return call_obj(m, sc, { arg_obj(sys_type) });
}

// ---------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------

int hook_pass(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) { return REFRAMEWORK_HOOK_CALL_ORIGINAL; }
void hook_post_pass(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {}

// ---- diagnostic hooks: one pre function per slot (the hook API passes no user data) ----
namespace {
constexpr int kDiagMax = 32;
std::string g_diag_name[kDiagMax];
std::atomic<uint32_t> g_diag_bits{0};
int g_diag_n = 0;
template <int I> int diag_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    g_diag_bits.fetch_or(1u << I);
    post(EV_DIAG);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
template <int... I> constexpr REFPreHookFn diag_fn(int i, std::integer_sequence<int, I...>) {
    constexpr REFPreHookFn t[] = { &diag_pre<I>... };
    return t[i];
}
}

void diag_watch(TD* td, const char* method) {
    if (!td || g_diag_n >= kDiagMax) return;
    int i = g_diag_n;
    REFPreHookFn fn = diag_fn(i, std::make_integer_sequence<int, kDiagMax>{});
    if (hook_all(td, method, fn, nullptr)) {
        g_diag_name[i] = td->get_full_name() + "." + method;
        ++g_diag_n;
    }
}

void diag_flush() {
    uint32_t b = g_diag_bits.exchange(0);
    for (int i = 0; i < g_diag_n; ++i) if (b & (1u << i)) logf("called: %s", g_diag_name[i].c_str());
}

int hook_all(TD* td, const char* method, REFPreHookFn pre, REFPostHookFn post) {
    if (!td) { logf("hook: type missing for %s", method); return 0; }
    int n = 0;
    std::unordered_set<Method*> done;
    for (auto* t = td; t; t = t->get_parent_type()) {
        for (auto* m : t->get_methods()) {
            if (!m || !m->get_name() || strcmp(m->get_name(), method) != 0 || done.count(m)) continue;
            done.insert(m);
            m->add_hook(pre ? pre : hook_pass, post ? post : hook_post_pass, false);
            { auto* dt = m->get_declaring_type(); logf("hook: %s.%s hooked on declaring type %s", td->get_full_name().c_str(), method, dt ? dt->get_full_name().c_str() : "?"); }
            ++n;
        }
        if (n) break;   // the type's own methods (overrides), not the ones they replace
    }
    logf("hook: %s.%s x%d", td->get_full_name().c_str(), method, n);
    return n;
}

// ---------------------------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------------------------

std::string read_file(const char* path, size_t max_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0 || static_cast<size_t>(sz) > max_size) { fclose(f); return {}; }
    fseek(f, 0, SEEK_SET);
    std::string buf(static_cast<size_t>(sz), '\0');
    size_t got = fread(buf.data(), 1, static_cast<size_t>(sz), f);
    fclose(f);
    buf.resize(got);
    return buf;
}

bool write_file(const char* path, const std::string& data) {
    std::string tmp = std::string(path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) { DeleteFileA(tmp.c_str()); return false; }
    return MoveFileExA(tmp.c_str(), path, MOVEFILE_REPLACE_EXISTING) != 0;
}

bool file_mtime(const char* path, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return false;
    out = d.ftLastWriteTime;
    return true;
}

bool file_exists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

} // namespace slots
