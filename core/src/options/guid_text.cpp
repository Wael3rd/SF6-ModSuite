#ifndef NOMINMAX
#define NOMINMAX
#endif
// dialogs::guid_text - the hMsg.GetMessage hook over the GUID -> text table (phase 4b).
// See guid_text.hpp and docs/CPP_PORT_PHASE4.md section 4b.
//
// Rules kept:
//   R6   fresh GUID per call (guid_table.cpp), one extra add_ref on the managed string handed to the game
//   R16  /EHa (SEH around every hook body)

#include "guid_text.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>

#include "../perf.hpp"
#include <string>
#include <string_view>

#include "ll_common.hpp"
#include "opt_common.hpp"
#include "../pausemenu/pausemenu.hpp"
#include "../sf6core_internal.h"

namespace dialogs::guid_text {
namespace {

using API = reframework::API;
using MO  = API::ManagedObject;

constexpr int kMaxFaults = 5;         // same threshold as the screens modules

bool g_on = false;
int  g_faults = 0;
std::mutex g_install_mx;                      // install() is called by the option service and by the pause-menu service
std::atomic<bool> g_hooked{false};            // read from any thread
std::atomic<bool> g_failed{false};

void fault(const char* where) {
    ++g_faults;
    cmn::journal("[guid_text] fault in %s (%d)", where, g_faults);
    if (g_faults >= kMaxFaults) {
        g_on = false;
        cmn::journal("[guid_text] too many faults, off (our rows lose their texts)");
    }
}

// The pre-hook stores the matched text; the post-hook creates the managed string from it.
thread_local std::string tl_hit_text;
thread_local bool tl_hit = false;

#pragma warning(push)
#pragma warning(disable: 4733)

// Inner logic kept apart: __try cannot appear in a function whose locals need unwinding.
int pre_get_message_inner(void** argv) {
    tl_hit = false;
    // argv: [0] = thread context, [1] = the Guid (a value-type pointer). GetMessage is static (:82-86 guid_lookup).
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(argv[1]);
    if (!ptr) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    uint64_t hi, lo;
    std::memcpy(&hi, ptr, 8);
    std::memcpy(&lo, ptr + 8, 8);
    if (!lookup(hi, lo, tl_hit_text)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    tl_hit = true;
    return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
}

void post_get_message_inner(void** ret_val) {
    // :74-79 managed_string: a fresh string per call, one extra reference for the game (R6)
    MO* str = ll::create_utf8_string(tl_hit_text);
    if (str) { str->add_ref(); *ret_val = str; }
    tl_hit = false;
}

int pre_get_message(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!g_on || live() == 0 || cmn::closing()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    pausemenu::text_barrier();        // no text of ours is looked up while a pause-menu page is being built elsewhere
    int64_t pt = perf::begin();
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { rc = pre_get_message_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("get_message_pre"); }
    perf::end(perf::GuidHook, pt);
    return rc;
}

void post_get_message(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on || !tl_hit) return;
    int64_t pt = perf::begin();
    __try { post_get_message_inner(ret_val); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("get_message_post"); }
    perf::end(perf::GuidHook, pt);
}

#pragma warning(pop)

}  // namespace

bool install() {
    std::lock_guard lk(g_install_mx);
    if (g_hooked.load()) return true;                 // already in place: one hook in the process (ADR-0006)
    if (g_failed.load()) return false;
    auto* tdb = API::get()->tdb();
    if (!tdb) return false;                           // not final: tried again by the next caller
    auto fail = [](const char* why) { cmn::log_warn("[guid_text] %s: off", why); g_failed = true; return false; };
    auto* hmsg_td = tdb->find_type("app.helper.hMsg");
    if (!hmsg_td) return fail("app.helper.hMsg not found");
    auto* m = hmsg_td->find_method("GetMessage(System.Guid)");
    if (!m) {
        for (auto* method : hmsg_td->get_methods()) {
            if (method && method->get_name() && std::string_view{method->get_name()} == "GetMessage") { m = method; break; }
        }
    }
    if (!m || !m->get_function_raw()) return fail("GetMessage not found");
    if (!m->add_hook(pre_get_message, post_get_message, false)) return fail("GetMessage hook failed");
    g_on = true;
    g_hooked = true;
    cmn::journal("[guid_text] GetMessage hook installed");
    return true;
}

bool installed() { return g_hooked.load(); }
bool install_failed() { return g_failed.load(); }
bool ready() { return g_on; }

bool set_field(void* obj, const char* field_name, const GuidBytes& gb) {
    if (!obj || !field_name) return false;
    auto* mo = reinterpret_cast<MO*>(obj);
    auto* field = ll::find_field_walk(mo->get_type_definition(), field_name);
    if (!field) return false;
    void* raw = field->get_data_raw(mo, false);
    if (!raw) return false;
    std::memcpy(raw, &gb.hi, 8);
    std::memcpy(reinterpret_cast<uint8_t*>(raw) + 8, &gb.lo, 8);
    return true;
}

std::string status() {
    char buf[160];
    const Stats st = stats();
    std::snprintf(buf, sizeof(buf), "on=%d faults=%d issued=%zu live=%zu clients=%zu", (int)g_on, g_faults, count(),
                  st.live, st.owned);
    return buf;
}

// ---- the client API (sf6core_api.h, increment 15): GUID texts owned by a client ---------------------------------
namespace {
bool client_keep(Owner o) { return o && static_cast<const SF6CoreClient*>(o)->active; }
void to_gb(const uint8_t g[16], GuidBytes& gb) { std::memcpy(&gb.hi, g, 8); std::memcpy(&gb.lo, g + 8, 8); }
}  // namespace

}  // namespace dialogs::guid_text

namespace gt = dialogs::guid_text;

SF6CoreStatus SF6CORE_CALL api_text_new(SF6CoreClient* client, const char* utf8, uint8_t out_guid16[16]) {
    if (!client || !utf8 || !out_guid16) return SF6CORE_ERR_INVALID_ARG;
    if (!client->active || client->state == SF6CORE_CLIENT_DISABLED_BY_BREAKER || kill_switch_active()) return SF6CORE_ERR_DISABLED;
    if (!s_initialized.load(std::memory_order_acquire)) return SF6CORE_ERR_NOT_READY;
    if (!gt::installed() && !gt::install()) return gt::install_failed() ? SF6CORE_ERR_DISABLED : SF6CORE_ERR_NOT_READY;
    gt::GuidBytes gb;
    if (!gt::alloc_owned(client, utf8, gt::client_keep, gb, nullptr)) return SF6CORE_ERR_DISABLED;
    std::memcpy(out_guid16, &gb.hi, 8);
    std::memcpy(out_guid16 + 8, &gb.lo, 8);
    return SF6CORE_OK;
}

SF6CoreStatus SF6CORE_CALL api_text_set(SF6CoreClient* client, const uint8_t guid16[16], const char* utf8) {
    if (!client || !guid16 || !utf8) return SF6CORE_ERR_INVALID_ARG;
    gt::GuidBytes gb;
    gt::to_gb(guid16, gb);
    return gt::retext_owned(client, gb, utf8) ? SF6CORE_OK : SF6CORE_ERR_NOT_FOUND;
}

SF6CoreStatus SF6CORE_CALL api_text_release(SF6CoreClient* client, const uint8_t guid16[16]) {
    if (!client || !guid16) return SF6CORE_ERR_INVALID_ARG;
    gt::GuidBytes gb;
    gt::to_gb(guid16, gb);
    return gt::release_owned(client, gb) ? SF6CORE_OK : SF6CORE_ERR_NOT_FOUND;
}

SF6CoreStatus SF6CORE_CALL api_obj_set_guid(SF6CoreClient* client, void* obj, const char* field, const uint8_t guid16[16]) {
    return core_obj_set_guid(client, obj, field, guid16);
}

// unregister_client: the client's texts go with it
void guid_text_release_client(SF6CoreClient* c) {
    const size_t n = gt::release_owner(c);
    if (n) core_log(SF6CORE_LOG_INFO, c->client_id, "[guid_text] %zu text(s) released (unregistered)", n);
}
