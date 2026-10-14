#include "hooks.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <tuple>

#include <array>
#include <utility>

#include "codecheck_cache.hpp"
#include "game/gm.hpp"
#include "perf.hpp"

namespace mech::hooks {
namespace {

using API = reframework::API;

struct Target {
    const Spec* spec = nullptr;                      // null: a survey address
    API::Method* m = nullptr;
    uint32_t index = 0;
    uintptr_t raw = 0, code = 0;
    int shared = 0;                                  // other methods of the game entering the same entry or code
    std::vector<uint32_t> sharers;                   // their method indexes (the first 16)
    API::Method* other = nullptr;                    // one of them, for the log
    bool by_va = false;                              // a code address (Spec::va or a survey): every method entering it counts
    int entering = 0;                                // by_va: how many methods enter that code (the scan's count)
    uintptr_t va = 0;                                // by_va: the static VA asked
    const char* what = nullptr;                      // survey: what the address is
};

std::string g_status = "none";
int g_passes = 0;                                    // install passes that hooked something (the UI module runs its own)
std::vector<std::pair<uintptr_t, const char*>> g_surveys;   // add_survey (before install)

constexpr uintptr_t kImageBase = 0x140000000ull;     // the exe's preferred base: the static VAs of the survey notes
uintptr_t rebase(uintptr_t va) {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return va < kImageBase ? 0 : base + (va - kImageBase);
}

// The code a function entry runs: jump thunks followed (jmp rel32 / rel8 / jmp [rip+disp32]), as REFramework's add_hook
// does with ignore_jmp = false (the Native Core's code_of).
uintptr_t code_of(uintptr_t p) {
    __try {
        for (int hop = 0; hop < 4 && p; ++hop) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
            if (b[0] == 0xE9) p = p + 5 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 1);
            else if (b[0] == 0xEB) p = p + 2 + (intptr_t) * reinterpret_cast<const int8_t*>(b + 1);
            else if (b[0] == 0xFF && b[1] == 0x25) p = *reinterpret_cast<const uintptr_t*>(p + 6 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 2));
            else break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return p;
}

// One pass over every method of the game: who else enters each target's code.
void fold_scan(std::vector<Target>& ts) {
    auto* tdb = API::get()->tdb();
    const uint32_t n = tdb->get_num_methods();
    for (uint32_t i = 0; i < n; ++i) {
        API::Method* m = tdb->get_method(i);
        if (!m) continue;
        const uintptr_t fn = reinterpret_cast<uintptr_t>(m->get_function_raw());
        if (!fn) continue;
        uintptr_t code = 0;
        bool code_done = false;
        for (Target& t : ts) {
            if (!t.raw) continue;
            bool same = fn == t.raw || fn == t.code;
            if (!same) {
                if (!code_done) { code = code_of(fn); code_done = true; }
                same = code && code == t.code;
            }
            const uint32_t mi = m->get_index();
            if (!same || mi == t.index) continue;
            ++t.shared;
            if (!t.other) t.other = m;
            if (t.sharers.size() < 16) t.sharers.push_back(mi);
        }
    }
}
bool fold_scan_seh(std::vector<Target>* ts) {
    __try { fold_scan(*ts); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// ---- the code scan's cache (codecheck_cache.hpp): the scan only changes when the game does ---------------------------------
// REFramework logs an "Encoded offset is 0" error per method without a native function (~346k lines, 85 MB) at every scan.
constexpr const wchar_t* kCacheFile = L"reframework/data/SF6_CRSMNative_data/codecheck.txt";

long long rva_of(uintptr_t a) {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return a ? (long long)(a - base) : 0;
}
// A target's code in the cache: outside the exe it is another module's hook (a mod that hooked the method first: its
// trampoline moves at each start), -1; the entry's RVA and the method index still pin the target.
long long code_rva(uintptr_t a) {
    static const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    static const uintptr_t size = [] {
        const auto* b = reinterpret_cast<const uint8_t*>(base);
        return (uintptr_t)reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew)->OptionalHeader.SizeOfImage;
    }();
    if (!a) return 0;
    return a >= base && a - base < size ? (long long)(a - base) : -1;
}

// The game's version: exe PE header (stamp, image size, checksum), file size and write time, TDB method count.
std::string exe_key(uint32_t methods) {
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    unsigned long long size = 0, time = 0;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        time = ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime;
    }
    char b[200];
    std::snprintf(b, sizeof b, "exe %08lX %08lX %08lX size %llu time %llX methods %u", (unsigned long)nt->FileHeader.TimeDateStamp,
                  (unsigned long)nt->OptionalHeader.SizeOfImage, (unsigned long)nt->OptionalHeader.CheckSum, size, time, methods);
    return b;
}

// This pass's targets as cache entries (id + where they are now + what the scan found: shared / sharers).
std::vector<codecache::Entry> cache_entries(const std::vector<Target>& ts) {
    std::vector<codecache::Entry> es;
    for (const Target& t : ts) {
        codecache::Entry e;
        e.named = !t.by_va;
        e.id = t.by_va ? codecache::va_id(t.va) : codecache::named_id(t.spec->type, t.spec->sig);
        if (e.named) { e.index = t.index; e.raw = rva_of(t.raw); e.code = code_rva(t.code); }
        e.shared = t.shared;
        e.sharers = t.sharers;
        es.push_back(std::move(e));
    }
    return es;
}

bool cache_load(std::string& key, std::vector<codecache::Entry>& es) {   // false: no file, or not a complete file of this format
    std::ifstream f(kCacheFile, std::ios::binary);
    if (!f) return false;
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return codecache::parse(text, key, es);
}

bool cache_store(const std::string& text) {                              // temp file + replace: never a half file
    const std::wstring path = kCacheFile, tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        f.flush();
        if (!f) return false;
    }
    if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(tmp.c_str());
    return false;
}

std::string label(API::Method* m) {
    if (!m) return "?";
    API::TypeDefinition* t = m->get_declaring_type();
    return (t ? t->get_full_name() : std::string("?")) + "." + (m->get_name() ? m->get_name() : "?");
}

// The methods a code-address target found (the scan's sharers, first 16): "Type.Name (n params, entry 0x...)".
std::string entering_list(const Target& t, size_t max) {
    auto* tdb = API::get()->tdb();
    std::string out;
    for (size_t i = 0; i < t.sharers.size() && i < max; ++i) {
        API::Method* m = tdb->get_method(t.sharers[i]);
        if (!m) continue;
        char b[64];
        std::snprintf(b, sizeof(b), " (%u params, entry 0x%llX)", m->get_num_params(),
                      (unsigned long long)reinterpret_cast<uintptr_t>(m->get_function_raw()));
        if (!out.empty()) out += ", ";
        out += label(m) + b;
    }
    if ((size_t)t.entering > max) out += ", ...";
    return out;
}

void log_survey(const Target& t) {
    char head[160];
    std::snprintf(head, sizeof(head), "survey 0x%llX (%s, at 0x%llX): ", (unsigned long long)t.va, t.what ? t.what : "?",
                  (unsigned long long)t.code);
    std::string line = head;
    if (t.entering == 0) line += "no method of the game enters this code (reached by calls only: no method hook possible)";
    else line += std::to_string(t.entering) + " method(s) enter this code: " + entering_list(t, 8) +
                 (t.entering == 1 ? "; R18: unique, hookable" : "; R18: shared, not hookable");
    cmn::log_info("[CRSMNative] %s", line.c_str());   // (the journal too)
}

// ---- perf: every hook body goes through a timing thunk -----------------------------------------------------------------------
// REFramework's pre / post hook functions carry no context, so a pool of thunks is generated at compile time (one per
// slot); a slot holds the real function and the perf entry id ("pre:<Type>.<sig>" / "post:..."). The thunks hold no C++
// object (the real bodies run their own __try); times and counts are relaxed atomics (hooks run on any thread). Off, a
// thunk costs the extra call and one branch. A pool that is full leaves the next hooks unwrapped (logged).
constexpr size_t kThunks = 160;
struct PreSlot { REFPreHookFn fn; int id; };
struct PostSlot { REFPostHookFn fn; int id; };
PreSlot g_pre_slots[kThunks];
PostSlot g_post_slots[kThunks];
size_t g_pre_used = 0, g_post_used = 0;

template <size_t N> int pre_thunk(int argc, void** argv, REFrameworkTypeDefinitionHandle* tys, unsigned long long ra) {
    const int64_t t0 = perf::begin();
    const int r = g_pre_slots[N].fn(argc, argv, tys, ra);
    perf::end(g_pre_slots[N].id, t0);
    return r;
}
template <size_t N> void post_thunk(void** ret, REFrameworkTypeDefinitionHandle ty, unsigned long long ra) {
    const int64_t t0 = perf::begin();
    g_post_slots[N].fn(ret, ty, ra);
    perf::end(g_post_slots[N].id, t0);
}
template <size_t... I> constexpr std::array<REFPreHookFn, sizeof...(I)> make_pre(std::index_sequence<I...>) { return { &pre_thunk<I>... }; }
template <size_t... I> constexpr std::array<REFPostHookFn, sizeof...(I)> make_post(std::index_sequence<I...>) { return { &post_thunk<I>... }; }
const auto g_pre_thunks = make_pre(std::make_index_sequence<kThunks>{});
const auto g_post_thunks = make_post(std::make_index_sequence<kThunks>{});

std::string perf_name(const char* kind, const Spec& s) {        // "pre:Type.sig" (the sig up to its first " (", at most 56 chars)
    std::string sig = s.sig ? s.sig : "?";
    const size_t p = sig.find(" (");
    if (p != std::string::npos) sig.resize(p);
    if (sig.size() > 56) sig.resize(56);
    return std::string(kind) + ":" + (s.type ? s.type : "?") + "." + sig;
}
REFPreHookFn timed_pre(const Spec& s) {
    if (!s.pre) return nullptr;
    if (g_pre_used >= kThunks) { cmn::log_warn("[CRSMNative] perf: no free thunk, pre hook of %s.%s is not timed", s.type, s.sig); return s.pre; }
    g_pre_slots[g_pre_used] = { s.pre, perf::reg(perf_name("pre", s).c_str(), true) };
    return g_pre_thunks[g_pre_used++];
}
REFPostHookFn timed_post(const Spec& s) {
    if (!s.post) return nullptr;
    if (g_post_used >= kThunks) { cmn::log_warn("[CRSMNative] perf: no free thunk, post hook of %s.%s is not timed", s.type, s.sig); return s.post; }
    g_post_slots[g_post_used] = { s.post, perf::reg(perf_name("post", s).c_str(), true) };
    return g_post_thunks[g_post_used++];
}

}  // namespace

void add_survey(uintptr_t va, const char* what) { g_surveys.emplace_back(va, what); }

void install(const std::vector<Spec>& specs) {
    std::vector<Target> ts;
    for (const Spec& s : specs) {
        if (!cmn::owns(s.layer)) continue;
        Target t;
        t.spec = &s;
        if (s.va) {                                               // a code address: its method found by the scan
            t.by_va = true;
            t.va = s.va;
            t.what = s.sig;
            t.raw = t.code = rebase(s.va);
            t.index = 0xFFFFFFFFu;
        } else {
            game::gm::TD* td = game::gm::type(s.type);
            t.m = td ? td->find_method(s.sig) : nullptr;
            if (!t.m && td) {                                     // "Name()": the overload without parameters, by name and count
                const std::string_view sv = s.sig;
                if (sv.size() > 2 && sv.substr(sv.size() - 2) == "()")
                    t.m = game::gm::method_n(td, std::string(sv.substr(0, sv.size() - 2)).c_str(), 0);
            }
            if (t.m) {
                t.index = t.m->get_index();
                t.raw = reinterpret_cast<uintptr_t>(t.m->get_function_raw());
                t.code = t.raw ? code_of(t.raw) : 0;
            }
        }
        ts.push_back(t);
    }
    if (ts.empty()) { if (g_passes == 0) g_status = "none (every layer is the Lua's)"; return; }
    for (const auto& [va, what] : g_surveys) {                    // the survey addresses, same pass (once)
        Target t;
        t.by_va = true;
        t.va = va;
        t.what = what;
        t.raw = t.code = rebase(va);
        t.index = 0xFFFFFFFFu;
        ts.push_back(t);
    }
    g_surveys.clear();
    const uint64_t t0 = GetTickCount64();
    // the cache first: same game version, every target there and where it was -> no scan
    const std::string game_key = exe_key(API::get()->tdb()->get_num_methods());
    std::string fkey;
    std::vector<codecache::Entry> fes, found;
    const bool have = cache_load(fkey, fes);
    const codecache::Miss why = codecache::decide(have, fkey, fes, game_key, cache_entries(ts),
                                                  found);
    bool scanned = false, from_cache = false;
    if (why == codecache::Miss::None) {
        for (size_t i = 0; i < ts.size(); ++i) {
            Target& t = ts[i];
            t.shared = found[i].shared;
            t.sharers = found[i].sharers;
            t.other = t.sharers.empty() ? nullptr : API::get()->tdb()->get_method(t.sharers[0]);
        }
        scanned = from_cache = true;
        cmn::log_info("[CRSMNative] code scan cache: hit, %zu target(s)", ts.size());
    } else {
        cmn::log_info("[CRSMNative] code scan cache: miss (%s), scanning every method", codecache::miss_text(why));
        scanned = fold_scan_seh(&ts);
        if (scanned) {
            const bool ok = cache_store(codecache::serialize(game_key, codecache::merge(have, fkey, fes, game_key, cache_entries(ts))));
            cmn::log_info("[CRSMNative] code scan cache: %s", ok ? "written" : "NOT written (file error)");
        } else cmn::log_warn("[CRSMNative] code scan cache: scan failed, nothing written");
    }
    const uint64_t ms = GetTickCount64() - t0;
    for (Target& t : ts) {                                        // a code address: the one method entering it, if one
        if (!t.by_va) continue;
        t.entering = t.shared;
        if (scanned) log_survey(t);                               // a hook's code address is surveyed too
        if (!t.spec) continue;
        if (scanned && t.entering == 1 && !t.sharers.empty()) {
            t.m = API::get()->tdb()->get_method(t.sharers[0]);
            t.index = t.m ? t.m->get_index() : 0xFFFFFFFFu;
            t.shared = 0;
            t.sharers.clear();
            t.other = nullptr;
        }
    }
    int hooked = 0, refused = 0, missing = 0, covered = 0;
    // a body hooked once covers the targets sharing it with the same pre / post (two specs with other pre / post on one
    // method are two hooks: REFramework chains them)
    std::set<std::tuple<uintptr_t, uintptr_t, uintptr_t>> done;
    std::set<cmn::Feature> lost;
    for (Target& t : ts) {
        if (!t.spec) continue;                                    // a survey address
        const Spec& s = *t.spec;
        std::string name = std::string(s.type) + "." + s.sig;
        if (t.by_va) {
            char b[48];
            std::snprintf(b, sizeof(b), " (code 0x%llX)", (unsigned long long)t.va);
            name += b;
            if (t.m) name += " = " + label(t.m);
            if (scanned && t.entering > 1) {                      // several methods enter that code: R18
                ++refused;
                cmn::log_warn("[CRSMNative] hook %s: REFUSED (R18): %d methods enter that code: %s: %s", name.c_str(), t.entering,
                              entering_list(t, 4).c_str(), s.without);
                if (s.required) lost.insert(s.layer);
                continue;
            }
        }
        if (!t.m || !t.raw) {
            ++missing;
            cmn::log_warn("[CRSMNative] hook %s: not found (%s)", name.c_str(), s.without);
            if (s.required) lost.insert(s.layer);
            continue;
        }
        bool ok = scanned;
        if (ok && t.shared > 0) {                                 // only sharing with our own targets of the same pre / post is fine
            ok = (size_t)t.shared == t.sharers.size();
            for (uint32_t x : t.sharers) {
                bool ours = false;
                for (const Target& o : ts)
                    if (&o != &t && o.m && o.index == x && o.spec->pre == s.pre && o.spec->post == s.post) ours = true;
                if (!ours) ok = false;
            }
        }
        if (!ok) {
            ++refused;
            cmn::log_warn("[CRSMNative] hook %s at 0x%llX (code 0x%llX): %s: REFUSED (R18): %s", name.c_str(),
                          (unsigned long long)t.raw, (unsigned long long)t.code,
                          scanned ? ("shares its code with " + std::to_string(t.shared) + " other method(s), e.g. " + label(t.other)).c_str()
                                  : "the code check failed",
                          s.without);
            if (s.required) lost.insert(s.layer);
            continue;
        }
        const auto key = std::make_tuple(t.code, reinterpret_cast<uintptr_t>(s.pre), reinterpret_cast<uintptr_t>(s.post));
        if (done.count(key)) {
            ++covered;
            if (s.installed) *s.installed = true;
            cmn::journal("hook %s: covered by the hook on its shared code", name.c_str());
            continue;
        }
        if (t.m->add_hook(timed_pre(s), timed_post(s), false)) {
            done.insert(key);
            ++hooked;
            if (s.installed) *s.installed = true;
            if (t.by_va) cmn::log_info("[CRSMNative] hook %s at 0x%llX: installed", name.c_str(), (unsigned long long)t.raw);
            else cmn::journal("hook %s at 0x%llX: installed", name.c_str(), (unsigned long long)t.raw);
        } else {
            ++refused;
            cmn::log_warn("[CRSMNative] hook %s: add_hook refused (%s)", name.c_str(), s.without);
            if (s.required) lost.insert(s.layer);
        }
    }
    char b[200];
    std::snprintf(b, sizeof(b), "%d installed, %d covered, %d refused (R18 or add_hook), %d missing; code scan %s", hooked, covered,
                  refused, missing, from_cache ? "from cache" : (std::to_string(ms) + " ms").c_str());
    g_status = g_passes++ == 0 ? std::string(b) : g_status + "; " + b;
    cmn::log_info("[CRSMNative] hooks: %s", b);
    for (cmn::Feature f : lost) {                                 // a layer without its hooks goes back to the Lua at once
        cmn::log_warn("[CRSMNative] %s: a required hook is missing: the layer goes back to the Lua", cmn::feature_name(f));
        for (int i = 0; i < 5; ++i) cmn::fault(f, "required hook");
    }
}

std::string status() { return g_status; }

}  // namespace mech::hooks
