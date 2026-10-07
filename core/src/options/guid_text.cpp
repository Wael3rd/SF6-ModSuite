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

#include <cstring>
#include <string>
#include <string_view>

#include "ll_common.hpp"
#include "opt_common.hpp"

namespace dialogs::guid_text {
namespace {

using API = reframework::API;
using MO  = API::ManagedObject;

constexpr int kMaxFaults = 5;         // same threshold as the screens modules

bool g_on = false;
int  g_faults = 0;

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
    if (!g_on || live() == 0) return REFRAMEWORK_HOOK_CALL_ORIGINAL;   // :89 nothing registered: no lookup (online battle)
    __try { return pre_get_message_inner(argv); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("get_message_pre"); }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void post_get_message(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!g_on || !tl_hit) return;
    __try { post_get_message_inner(ret_val); }
    __except (EXCEPTION_EXECUTE_HANDLER) { fault("get_message_post"); }
}

#pragma warning(pop)

}  // namespace

bool install() {
    auto* tdb = API::get()->tdb();
    if (!tdb) return false;
    auto* hmsg_td = tdb->find_type("app.helper.hMsg");
    if (!hmsg_td) { cmn::log_warn("[guid_text] app.helper.hMsg not found: off"); return false; }
    auto* m = hmsg_td->find_method("GetMessage(System.Guid)");
    if (!m) {
        for (auto* method : hmsg_td->get_methods()) {
            if (method && method->get_name() && std::string_view{method->get_name()} == "GetMessage") { m = method; break; }
        }
    }
    if (!m || !m->get_function_raw()) { cmn::log_warn("[guid_text] GetMessage not found: off"); return false; }
    if (!m->add_hook(pre_get_message, post_get_message, false)) { cmn::log_warn("[guid_text] GetMessage hook failed: off"); return false; }
    g_on = true;
    cmn::journal("[guid_text] GetMessage hook installed");
    return true;
}

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
    std::snprintf(buf, sizeof(buf), "on=%d faults=%d issued=%zu live=%zu", (int)g_on, g_faults, count(), live());
    return buf;
}

}  // namespace dialogs::guid_text
