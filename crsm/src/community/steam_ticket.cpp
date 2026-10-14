#include "steam_ticket.hpp"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>

#include "auth.hpp"

namespace community::steam {
namespace {

using GetTicketFn = uint32_t (*)(void* self, const char* identity);
using CancelFn = void (*)(void* self, uint32_t ticket);

// ---- SEH frames: plain data only (no object with a destructor may live in a function with __try under /EHa) -----------
bool seh_user(int32_t (*fn)(), int32_t* out) {
    __try { *out = fn(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_iface(void* (*fn)(int32_t, const char*), int32_t user, void** out) {
    __try { *out = fn(user, kInterface); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_slots(void* iface, void** s_get, void** s_cancel) {
    __try {
        void** vt = *static_cast<void***>(iface);
        *s_get = vt[kSlotGetAuthTicketForWebApi];
        *s_cancel = vt[kSlotCancelAuthTicket];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_register(void (*fn)(CallbackBase*, int), CallbackBase* cb) {
    __try { fn(cb, kCallbackId); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_unregister(void (*fn)(CallbackBase*), CallbackBase* cb) {
    __try { fn(cb); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_get_ticket(void* iface, uint32_t* out) {
    __try {
        void** vt = *static_cast<void***>(iface);
        *out = reinterpret_cast<GetTicketFn>(vt[kSlotGetAuthTicketForWebApi])(iface, kIdentity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool seh_cancel(void* iface, uint32_t h) {
    __try {
        void** vt = *static_cast<void***>(iface);
        reinterpret_cast<CancelFn>(vt[kSlotCancelAuthTicket])(iface, h);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The callback's record, read from Steam's buffer: a size out of 1..2560 is kept as -1 (no byte copied)
bool seh_copy(TicketResponse* dst, const void* src) {
    __try {
        const uint8_t* s = static_cast<const uint8_t*>(src);
        std::memcpy(dst, s, 12);
        if (dst->cubTicket < 1 || dst->cubTicket > kTicketMax) { dst->cubTicket = -1; return true; }
        std::memcpy(dst->rgubTicket, s + 12, (size_t)dst->cubTicket);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- the game's import tables: which of SteamAPI_RunCallbacks / SteamAPI_ManualDispatch_Init it imports -------------------
void scan_names(const uint8_t* base, uint32_t int_rva, bool* run, bool* manual) {
    const IMAGE_THUNK_DATA64* th = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + int_rva);
    for (int i = 0; i < 4096 && th[i].u1.AddressOfData; ++i) {
        if (IMAGE_SNAP_BY_ORDINAL64(th[i].u1.Ordinal)) continue;
        const IMAGE_IMPORT_BY_NAME* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + th[i].u1.AddressOfData);
        if (_stricmp(ibn->Name, "SteamAPI_RunCallbacks") == 0) *run = true;
        else if (_stricmp(ibn->Name, "SteamAPI_ManualDispatch_Init") == 0) *manual = true;
    }
}
bool seh_scan_imports(const uint8_t* base, bool* found, bool* run, bool* manual) {
    __try {
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const IMAGE_NT_HEADERS64* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imp.VirtualAddress) {
            const IMAGE_IMPORT_DESCRIPTOR* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imp.VirtualAddress);
            for (int i = 0; i < 1024 && d[i].Name; ++i) {
                if (_stricmp(reinterpret_cast<const char*>(base + d[i].Name), "steam_api64.dll") != 0) continue;
                *found = true;
                if (d[i].OriginalFirstThunk) scan_names(base, d[i].OriginalFirstThunk, run, manual);   // (the IAT holds addresses)
            }
        }
        const IMAGE_DATA_DIRECTORY& dl = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
        if (dl.VirtualAddress) {                     // SF6 delay-loads steam_api64.dll
            const IMAGE_DELAYLOAD_DESCRIPTOR* d = reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dl.VirtualAddress);
            for (int i = 0; i < 1024 && d[i].DllNameRVA; ++i) {
                if (!d[i].Attributes.RvaBased || !d[i].ImportNameTableRVA) continue;
                if (_stricmp(reinterpret_cast<const char*>(base + d[i].DllNameRVA), "steam_api64.dll") != 0) continue;
                *found = true;
                scan_names(base, d[i].ImportNameTableRVA, run, manual);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08x", v);
    return b;
}

long long us_since(std::chrono::steady_clock::time_point t) {
    return (long long)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

std::string dispatch_hint() {
    bool found = false, run = false, manual = false;
    if (!seh_scan_imports(reinterpret_cast<const uint8_t*>(GetModuleHandleA(nullptr)), &found, &run, &manual))
        return "unknown: the game's import tables could not be read";
    if (!found) return "unknown: the game does not import steam_api64.dll by name";
    if (manual) return "manual: the game imports SteamAPI_ManualDispatch_Init (a registered callback is then never called)";
    if (run) return "classic: the game imports SteamAPI_RunCallbacks and no SteamAPI_ManualDispatch_*";
    return "unknown: the game imports neither SteamAPI_RunCallbacks nor SteamAPI_ManualDispatch_Init";
}

std::string module_of(const void* p) {
    HMODULE m = nullptr;
    if (!p || !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCSTR>(p), &m) || !m)
        return "";
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');                // the file name only: no folder of the player's PC
    char b[MAX_PATH + 32];
    std::snprintf(b, sizeof(b), "%s+0x%llx", name ? name + 1 : path, (unsigned long long)((uintptr_t)p - (uintptr_t)m));
    return b;
}

bool resolve(Api* out, std::string* why) {
    Api a;
    HMODULE h = GetModuleHandleA("steam_api64.dll");                 // the game loaded it; never loaded by us
    if (!h) { a.where = "steam_api64.dll is not loaded"; *out = a; *why = a.where; return false; }
    a.get_hsteamuser = reinterpret_cast<int32_t (*)()>(GetProcAddress(h, "SteamAPI_GetHSteamUser"));
    a.find_or_create_user_interface = reinterpret_cast<void* (*)(int32_t, const char*)>(GetProcAddress(h, "SteamInternal_FindOrCreateUserInterface"));
    a.register_callback = reinterpret_cast<void (*)(CallbackBase*, int)>(GetProcAddress(h, "SteamAPI_RegisterCallback"));
    a.unregister_callback = reinterpret_cast<void (*)(CallbackBase*)>(GetProcAddress(h, "SteamAPI_UnregisterCallback"));
    std::string missing;
    if (!a.get_hsteamuser) missing += " SteamAPI_GetHSteamUser";
    if (!a.find_or_create_user_interface) missing += " SteamInternal_FindOrCreateUserInterface";
    if (!a.register_callback) missing += " SteamAPI_RegisterCallback";
    if (!a.unregister_callback) missing += " SteamAPI_UnregisterCallback";
    char b[96];
    std::snprintf(b, sizeof(b), "steam_api64.dll at %p", (void*)h);
    a.where = std::string(b) + (missing.empty() ? ": GetHSteamUser, FindOrCreateUserInterface, Register/UnregisterCallback found"
                                                : ": missing" + missing);
    a.dispatch = dispatch_hint();
    *out = a;
    if (!missing.empty()) { *why = "missing export(s):" + missing; return false; }
    return true;
}

// ---- Engine --------------------------------------------------------------------------------------------------------------

Engine::Engine(Resolver resolver, Log log, Clock now_ms) : resolver_(std::move(resolver)), log_(std::move(log)), now_(std::move(now_ms)) {}

Engine::~Engine() { if (registered_) unregister(); }

TicketSource::Ticket Engine::get(int timeout_ms) {
    std::unique_lock<std::mutex> lk(mx_);
    Ticket t;
    if (off_) { t.why = "method A is off for this session (" + off_why_ + ")"; return t; }
    const uint64_t now = now_();
    if (now < cool_until_) {
        t.why = cool_why_ + " (no new try for " + std::to_string((cool_until_ - now + 999) / 1000) + " s)";
        return t;
    }
    const uint64_t my = ++gen_;
    want_flow_ = my;
    touch();
    if (!cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return result_gen_ == my; })) {
        if (want_flow_ == my) want_flow_ = 0;      // never started: no frame ran
        else abandoned_ = my;                      // started: frame() cancels the ticket when it comes
        t.why = "no answer from the game thread within " + std::to_string(timeout_ms) + " ms";
        return t;
    }
    t = std::move(result_);
    result_ = Ticket{};
    return t;
}

void Engine::release(uint32_t handle) {
    if (!handle) return;
    std::lock_guard<std::mutex> lk(mx_);
    cancels_.push_back(handle);
    touch();
}

bool Engine::probe(const std::string& why) {
    std::lock_guard<std::mutex> lk(mx_);
    if (probe_asked_) return false;
    probe_asked_ = true;
    want_probe_ = why.empty() ? std::string("probe") : why;
    touch();
    return true;
}

std::string Engine::status() const {
    std::lock_guard<std::mutex> lk(mx_);
    return last_;
}

void Engine::frame() {
    if (!work_.load(std::memory_order_acquire)) return;
    std::vector<uint32_t> cancels;
    Kind start = Kind::None;
    uint64_t gen = 0;
    std::string why;
    bool refused = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        cancels.swap(cancels_);
        if (!waiting_) {
            if (off_ && want_flow_) {                  // method A went off (a fault, no interface) after get() asked: no new call
                result_ = Ticket{};
                result_.why = "method A is off for this session (" + off_why_ + ")";
                result_gen_ = want_flow_;
                want_flow_ = 0;
                refused = true;
            }
            if (off_) want_probe_.clear();
            if (want_flow_) { start = Kind::Flow; gen = want_flow_; want_flow_ = 0; why = "a Community write needs a token"; }
            else if (!want_probe_.empty()) { start = Kind::Probe; why = want_probe_; want_probe_.clear(); }
            if (start == Kind::None) work_.store(false, std::memory_order_release);   // (new requests set it again under mx_)
        }
    }
    if (refused) cv_.notify_all();
    for (uint32_t h : cancels) cancel(h, "the Worker answered");
    if (waiting_) check_wait();
    else if (start != Kind::None) begin(start, gen, why);
}

void Engine::begin(Kind kind, uint64_t gen, const std::string& why) {
    kind_ = kind;
    at_gen_ = gen;
    t0_ = now_();
    handle_ = 0;
    ++attempt_;
    say("#" + std::to_string(attempt_) + (kind == Kind::Probe ? " probe" : " token") + " (" + why + "): start");
    if (!resolved_) {
        std::string w;
        const bool ok = resolver_ && resolver_(&api_, &w);
        if (!api_.where.empty()) say(api_.where);
        if (!api_.dispatch.empty()) say("callback dispatch: " + api_.dispatch);
        if (!ok) { finish_fail(w.empty() ? std::string("steam_api64.dll is not usable") : w, true); return; }
        resolved_ = true;
    }
    if (!iface_) {
        int32_t user = 0;
        if (!seh_user(api_.get_hsteamuser, &user)) { finish_fail("SteamAPI_GetHSteamUser faulted", true); return; }
        say("SteamAPI_GetHSteamUser() = " + std::to_string(user));
        if (user == 0) { finish_fail("HSteamUser 0: the game is not logged on to Steam", false); return; }
        void* p = nullptr;
        const auto t = std::chrono::steady_clock::now();
        if (!seh_iface(api_.find_or_create_user_interface, user, &p)) { finish_fail("SteamInternal_FindOrCreateUserInterface faulted", true); return; }
        say(std::string("SteamInternal_FindOrCreateUserInterface(") + kInterface + ") = " + (p ? "found" : "NOT FOUND") + " (" + std::to_string(us_since(t)) + " us)");
        if (!p) { finish_fail("the Steam client gave no SteamUser023 interface", true); return; }
        void* s_get = nullptr;
        void* s_cancel = nullptr;
        if (!seh_slots(p, &s_get, &s_cancel)) { finish_fail("the SteamUser023 vtable is not readable", true); return; }
        const std::string m_get = module_of(s_get), m_cancel = module_of(s_cancel);
        say("vtable slot 14 GetAuthTicketForWebApi -> " + (m_get.empty() ? std::string("no module") : m_get) +
            ", slot 17 CancelAuthTicket -> " + (m_cancel.empty() ? std::string("no module") : m_cancel));
        if (m_get.empty() || m_cancel.empty()) { finish_fail("the SteamUser023 vtable points outside every loaded module", true); return; }
        iface_ = p;
    }
    got_.store(false, std::memory_order_release);
    if (!registered_) {
        if (!seh_register(api_.register_callback, &cb_)) { finish_fail("SteamAPI_RegisterCallback faulted", true); return; }
        registered_ = true;
        say("callback 168 registered (GetTicketForWebApiResponse_t, " + std::to_string(sizeof(TicketResponse)) + " bytes)");
    }
    uint32_t h = 0;
    const auto t = std::chrono::steady_clock::now();
    if (!seh_get_ticket(iface_, &h)) { unregister(); finish_fail("GetAuthTicketForWebApi faulted", true); return; }
    say(std::string("GetAuthTicketForWebApi(\"") + kIdentity + "\") = handle " + hex32(h) + " (" + std::to_string(us_since(t)) + " us)");
    if (h == 0) { unregister(); finish_fail("GetAuthTicketForWebApi gave no handle (k_HAuthTicketInvalid)", false); return; }
    handle_ = h;
    deadline_ = t0_ + kCallbackWaitMs;
    waiting_ = true;
}

void Engine::check_wait() {
    if (got_.exchange(false, std::memory_order_acq_rel)) {
        TicketResponse r;
        { std::lock_guard<std::mutex> lk(cb_mx_); r = resp_; }
        if (r.hAuthTicket == handle_) { finish_ok(r); return; }
        say("callback 168 for another ticket (" + hex32(r.hAuthTicket) + "): not ours, still waiting");
    }
    if (now_() < deadline_) return;
    unregister();
    cancel(handle_, "no callback");
    handle_ = 0;
    finish_fail("no callback 168 within " + std::to_string(kCallbackWaitMs) + " ms (callbacks received: " +
                std::to_string(callbacks_.load()) + "; dispatch " + (api_.dispatch.empty() ? std::string("?") : api_.dispatch) + ")", false);
}

void Engine::finish_ok(const TicketResponse& r) {
    unregister();
    const uint64_t ms = now_() - t0_;
    say("callback 168 after " + std::to_string(ms) + " ms: eResult " + std::to_string(r.eResult) + (r.eResult == kResultOk ? " (OK)" : "") +
        ", handle " + hex32(r.hAuthTicket) + ", ticket " + (r.cubTicket > 0 ? auth::ticket_brief(r.rgubTicket, (size_t)r.cubTicket) : std::string("none")));
    const uint32_t h = handle_;
    if (r.eResult != kResultOk || r.cubTicket < 1) {
        cancel(h, "refused ticket");
        handle_ = 0;
        finish_fail("Steam gave no ticket (eResult " + std::to_string(r.eResult) + ", " + std::to_string(r.cubTicket) + " bytes)", false);
        return;
    }
    waiting_ = false;
    handle_ = 0;
    const std::string n = "#" + std::to_string(attempt_);
    if (kind_ == Kind::Probe) {
        cancel(h, "probe: the ticket is never sent");
        { std::lock_guard<std::mutex> lk(mx_); last_ = "probe OK: a ticket of " + std::to_string(r.cubTicket) + " bytes in " + std::to_string(ms) + " ms"; }
        say(n + " probe result: OK, a ticket of " + std::to_string(r.cubTicket) + " bytes in " + std::to_string(ms) + " ms (method A works in this game)");
        return;
    }
    bool gave_up = false;
    {
        std::lock_guard<std::mutex> lk(mx_);
        gave_up = abandoned_ == at_gen_;
        if (!gave_up) {
            result_ = Ticket{};
            result_.ok = true;
            result_.handle = h;
            result_.bytes.assign(r.rgubTicket, r.rgubTicket + r.cubTicket);
            result_gen_ = at_gen_;
        }
        last_ = "token: a ticket of " + std::to_string(r.cubTicket) + " bytes in " + std::to_string(ms) + " ms";
    }
    if (gave_up) { cancel(h, "the request stopped waiting"); say(n + " ticket came after the request stopped waiting: cancelled"); return; }
    cv_.notify_all();
    say(n + " ticket handed to the Worker thread for /auth/steam/ticket (" + std::to_string(ms) + " ms)");
}

void Engine::finish_fail(const std::string& why, bool for_session) {
    waiting_ = false;
    const uint64_t ms = now_() - t0_;
    {
        std::lock_guard<std::mutex> lk(mx_);
        if (for_session) { off_ = true; off_why_ = why; }
        else { cool_until_ = now_() + kCooldownMs; cool_why_ = why; }
        last_ = std::string(kind_ == Kind::Probe ? "probe" : "token") + " failed: " + why;
        if (kind_ == Kind::Flow && abandoned_ != at_gen_) {
            result_ = Ticket{};
            result_.why = why;
            result_gen_ = at_gen_;
        }
    }
    cv_.notify_all();
    say("#" + std::to_string(attempt_) + (kind_ == Kind::Probe ? " probe" : " token") + " FAILED after " + std::to_string(ms) + " ms: " + why +
        (for_session ? " (method A off for this session)" : " (no new try for " + std::to_string(kCooldownMs / 1000) + " s)"));
}

void Engine::cancel(uint32_t handle, const char* why) {
    if (!handle || !iface_) return;
    if (seh_cancel(iface_, handle)) say("CancelAuthTicket(" + hex32(handle) + ") (" + why + ")");
    else say("CancelAuthTicket(" + hex32(handle) + ") faulted");
}

void Engine::unregister() {
    if (!registered_) return;
    registered_ = false;
    if (!seh_unregister(api_.unregister_callback, &cb_)) say("SteamAPI_UnregisterCallback faulted");
}

void Engine::on_callback(const void* param) {
    try {
        callbacks_.fetch_add(1, std::memory_order_relaxed);
        TicketResponse tmp;
        if (!param || !seh_copy(&tmp, param)) return;
        { std::lock_guard<std::mutex> lk(cb_mx_); resp_ = tmp; }
        got_.store(true, std::memory_order_release);
    } catch (...) { /* never into steam_api64.dll */ }
}

}  // namespace community::steam
