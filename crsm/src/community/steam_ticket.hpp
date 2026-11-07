// community::steam - ADR-0018 method A: a Steam Web API ticket for "sf6-community", asked from the Steam client the game
// runs with, for the Worker's /auth/steam/ticket (AuthenticateUserTicket, identity sf6-community).
//
// SF6's steam_api64.dll (SDK of SteamUser021) has no GetAuthTicketForWebApi; the Steam client has it (SteamUser023, SDK 1.57+):
//   1. SteamAPI_GetHSteamUser()                                       the game's HSteamUser
//   2. SteamInternal_FindOrCreateUserInterface(user, "SteamUser023")   ISteamUser023* (the Steam client's own object)
//   3. SteamAPI_RegisterCallback(&cb, 168)                             GetTicketForWebApiResponse_t comes through the game's
//                                                                       own SteamAPI_RunCallbacks (SF6 imports it and none of
//                                                                       SteamAPI_ManualDispatch_*: classic dispatch)
//   4. vtable slot 14: HAuthTicket GetAuthTicketForWebApi(const char* pchIdentity)
//   5. callback 168 { m_hAuthTicket, m_eResult (1 = k_EResultOK), m_cubTicket, m_rgubTicket[2560] }; UnregisterCallback
//   6. once the Worker answered (or at once for the probe): vtable slot 17: void CancelAuthTicket(HAuthTicket)
// Slots: Steamworks SDK public/steam/isteamuser.h, class ISteamUser, STEAMUSER_INTERFACE_VERSION "SteamUser023" (SDK 1.57,
// mirror github.com/rlabrecque/SteamworksSDK): 13 GetAuthSessionTicket, 14 GetAuthTicketForWebApi, 15 BeginAuthSession,
// 16 EndAuthSession, 17 CancelAuthTicket. Checked against binaries on 2026-10-14: a v023 steam_api64.dll's flat
// SteamAPI_ISteamUser_GetAuthTicketForWebApi is `mov rax,[rcx]; jmp [rax+70h]` (14 * 8) and its CancelAuthTicket
// `jmp [rax+88h]` (17 * 8); SF6's own v021 dll has GetAuthSessionTicket at +68h (13), BeginAuthSession +70h, CancelAuthTicket
// +80h (16): v023 inserted GetAuthTicketForWebApi as slot 14 and moved the rest by one.
//
// Threads: every Steam call runs in Engine::frame() (the core's frame callback, game thread, where the game's own Steam calls
// run: RegisterCallback never races the game's RunCallbacks from another thread). The callback's Run() runs inside the game's
// SteamAPI_RunCallbacks and only copies the answer. The Worker thread waits in get(), never the game thread. Every Steam call
// and every read of Steam memory is under SEH. A fault, a missing export or interface = method A off for the session; no
// callback within 5 s or a refused ticket = no new try for 30 s. The caller turns any failure into "verification unavailable"
// for the write; reads never need a ticket.
//
// The probe (Engine::probe, once per session): the same steps, logged one by one, and the ticket cancelled at once: it is
// never sent anywhere. The log never holds a ticket byte (its length and first 4 hex digits) nor a SteamID.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace community {

// Where the Worker thread gets a ticket from (steam::Engine in the game; a fake in the tests).
class TicketSource {
public:
    struct Ticket {
        bool ok = false;
        uint32_t handle = 0;               // HAuthTicket, for release()
        std::vector<uint8_t> bytes;
        std::string why;                   // failure, for the journal
    };
    virtual ~TicketSource() = default;
    // Blocks the caller up to timeout_ms (never call it on the game thread). One caller at a time (the Worker thread).
    virtual Ticket get(int timeout_ms) = 0;
    // The ticket is no longer needed (the Worker answered, or the request gave up): CancelAuthTicket. Any thread.
    virtual void release(uint32_t handle) = 0;
};

namespace steam {

constexpr const char* kIdentity = "sf6-community";
constexpr const char* kInterface = "SteamUser023";
constexpr int kSlotGetAuthTicketForWebApi = 14;
constexpr int kSlotCancelAuthTicket = 17;
constexpr int kCallbackId = 168;                    // k_iSteamUserCallbacks (100) + 68
constexpr int kTicketMax = 2560;                    // GetTicketForWebApiResponse_t::k_nCubTicketMaxLength
constexpr uint64_t kCallbackWaitMs = 5000;
constexpr uint64_t kCooldownMs = 30000;
constexpr int32_t kResultOk = 1;                    // k_EResultOK

// GetTicketForWebApiResponse_t (isteamuser.h, VALVE_CALLBACK_PACK_LARGE = pack 8 on Windows: no padding in it)
#pragma pack(push, 8)
struct TicketResponse {
    uint32_t hAuthTicket;
    int32_t eResult;
    int32_t cubTicket;
    uint8_t rgubTicket[kTicketMax];
};
#pragma pack(pop)
static_assert(sizeof(TicketResponse) == 2572, "GetTicketForWebApiResponse_t is 2572 bytes");

// CCallbackBase of steam_api_common.h, declared the same way (same members, same virtuals in the same order) so that MSVC
// lays its vtable out as steam_api64.dll (MSVC too) expects: MSVC orders overloaded virtuals by its own rule, only the same
// declaration guarantees the same slots.
class CallbackBase {
public:
    CallbackBase() { flags_ = 0; id_ = 0; }
    // no virtual destructor: the SDK's binary interface has none
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool io_failure, uint64_t api_call) = 0;
    int GetICallback() { return id_; }
    virtual int GetCallbackSizeBytes() = 0;
protected:
    uint8_t flags_;                                 // m_nCallbackFlags, set by SteamAPI_RegisterCallback
    int id_;                                        // m_iCallback
    CallbackBase(const CallbackBase&) = delete;
    CallbackBase& operator=(const CallbackBase&) = delete;
};

// The steam_api64.dll entry points used (the tests inject fakes)
struct Api {
    int32_t (*get_hsteamuser)() = nullptr;
    void* (*find_or_create_user_interface)(int32_t user, const char* version) = nullptr;
    void (*register_callback)(CallbackBase* callback, int id) = nullptr;
    void (*unregister_callback)(CallbackBase* callback) = nullptr;
    std::string where;                              // for the log: the module and the exports found
    std::string dispatch;                           // dispatch_hint()
};
// From the steam_api64.dll the game loaded; false + why when it is not loaded or an export is missing.
bool resolve(Api* out, std::string* why);
// How the game pumps Steam callbacks, from its own import tables (plain and delay-load): "classic: ..." when it imports
// SteamAPI_RunCallbacks and not SteamAPI_ManualDispatch_Init, "manual: ..." when it imports the latter (a registered
// callback is then never called), "unknown: ..." otherwise.
std::string dispatch_hint();
// "name.dll+0x1234" for a code address, "" when it is in no loaded module
std::string module_of(const void* p);

class Engine final : public TicketSource {
public:
    using Resolver = std::function<bool(Api*, std::string*)>;
    using Log = std::function<void(const std::string&)>;
    using Clock = std::function<uint64_t()>;             // milliseconds, monotonic

    Engine(Resolver resolver, Log log, Clock now_ms);
    ~Engine() override;                                  // the game's engine is never destroyed (Steam may be gone at exit)

    Ticket get(int timeout_ms) override;
    void release(uint32_t handle) override;

    // Game thread, every frame: the Steam calls. Nothing to do = two atomic loads.
    void frame();
    // One diagnostic run per session (the switch file): false when one already ran or was asked.
    bool probe(const std::string& why);
    // One line for status.json: the last attempt's result
    std::string status() const;

    // Run() of the callback object: the thread of SteamAPI_RunCallbacks
    void on_callback(const void* param);

private:
    enum class Kind { None, Probe, Flow };
    class Callback final : public CallbackBase {
    public:
        explicit Callback(Engine* e) : e_(e) {}
        void Run(void* param) override { e_->on_callback(param); }
        void Run(void* param, bool, uint64_t) override { e_->on_callback(param); }
        int GetCallbackSizeBytes() override { return (int)sizeof(TicketResponse); }
    private:
        Engine* e_;
    };

    void begin(Kind kind, uint64_t gen, const std::string& why);
    void check_wait();
    void finish_ok(const TicketResponse& r);
    void finish_fail(const std::string& why, bool for_session);
    void cancel(uint32_t handle, const char* why);
    void unregister();
    void say(const std::string& s) const { if (log_) log_(s); }
    void touch() { work_.store(true, std::memory_order_release); }

    Resolver resolver_;
    Log log_;
    Clock now_;

    // shared with the Worker thread
    mutable std::mutex mx_;
    std::condition_variable cv_;
    std::atomic<bool> work_{false};                     // something for frame() to do
    uint64_t gen_ = 0, want_flow_ = 0, result_gen_ = 0, abandoned_ = 0;
    std::string want_probe_;
    bool probe_asked_ = false;
    Ticket result_;
    std::vector<uint32_t> cancels_;
    bool off_ = false;                                  // method A off for the session
    std::string off_why_;
    uint64_t cool_until_ = 0;
    std::string cool_why_;
    std::string last_ = "not tried";

    // game thread only
    bool resolved_ = false;
    Api api_;
    void* iface_ = nullptr;
    bool registered_ = false;
    bool waiting_ = false;
    Kind kind_ = Kind::None;
    uint64_t at_gen_ = 0, t0_ = 0, deadline_ = 0;
    uint32_t handle_ = 0;
    int attempt_ = 0;

    // the callback (Run() on Steam's dispatch thread)
    Callback cb_{this};
    std::mutex cb_mx_;
    std::atomic<bool> got_{false};
    std::atomic<int> callbacks_{0};
    TicketResponse resp_{};
};

}  // namespace steam
}  // namespace community
