// texts.cpp - increment 8: client texts (ADR-0006 amendment 2026-10-09, docs/ABI.md "Increment 8").
//
// A client gets message GUIDs that the game resolves to the client's texts through
// app.helper.hMsg.GetMessage(System.Guid): training pause-menu rows (TrainingMenuData._MessageID, _GuideMessage),
// input-guide entries, any data-driven UI that carries a message id (R17: such rows are data, the game renders their
// texts from the GUIDs at each draw). The table and the hook are the option engine's (options/guid_table.*,
// options/guid_text.*): one table and one GetMessage hook in the process, whoever comes first installs it.
//
// Rules:
//   - txt_new: a fresh GUID per call, never one GUID for two texts (R6; the dedup attempt crashed, L2).
//   - The texts belong to their client: the option engine's purge never touches them; txt_release (one, or all) and
//     unregister_client release them. They survive a breaker trip (data the game may still show).
//   - The hook is installed on the first txt_new of any client (no cost without clients): at once when that call runs
//     on the core's game thread after its init (a frame or flow callback), otherwise at the start of the next frame.
//   - Any thread: everything here is a table operation under the table's lock. obj_set_guid (display.cpp) writes a
//     GUID into a System.Guid field with the guard chain of obj_set_*.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include "sf6core_internal.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "options/guid_text.hpp"

namespace {

namespace gt = dialogs::guid_text;

std::atomic<bool> g_wanted{false};          // a client made a text: the hook is needed

gt::GuidBytes from16(const uint8_t g[16]) {
    gt::GuidBytes gb;
    std::memcpy(&gb.hi, g, 8);
    std::memcpy(&gb.lo, g + 8, 8);
    return gb;
}

// Checked under the table's lock: a client that unregistered meanwhile gets nothing stored (its texts were released).
bool client_registered(gt::Owner o) { return static_cast<const SF6CoreClient*>(o)->active; }

// The hook can never answer (install failed, or switched off by its fault counter): new texts are refused.
bool hook_dead() { return gt::install_failed() || (gt::installed() && !gt::ready()); }

const char* hook_state() {
    if (gt::installed()) return gt::ready() ? "on" : "off (faults)";
    if (gt::install_failed()) return "failed";
    return g_wanted.load(std::memory_order_relaxed) ? "pending" : "idle";
}

bool power_of_ten_from_1000(size_t n) {
    if (n < 1000) return false;
    while (n % 10 == 0) n /= 10;
    return n == 1;
}

}  // namespace

// ---- called by sf6core.cpp ---------------------------------------------------------------------------------------

// Start of each core frame (game thread, after init): the hook for texts made off the game thread or before init.
void texts_frame() {
    if (!g_wanted.load(std::memory_order_relaxed) || gt::installed() || gt::install_failed()) return;
    gt::install();                                  // logs its own failure
}

// unregister_client, after the client is marked inactive.
void texts_release_client(SF6CoreClient* c) {
    size_t n = 0;
    try { n = gt::release_owner(c); } catch (...) { return; }
    if (n) core_log(SF6CORE_LOG_INFO, c->client_id, "texts: %zu released (unregistered)", n);
}

// ---- the API (sf6core_api.h, increment 8) --------------------------------------------------------------------------

SF6CoreStatus SF6CORE_CALL api_txt_new(SF6CoreClient* client, const char* utf8, uint8_t out_guid16[16]) {
    if (!client || !out_guid16) return SF6CORE_ERR_INVALID_ARG;
    if (!client->active || core_closing() || hook_dead()) return SF6CORE_ERR_DISABLED;
    gt::GuidBytes gb;
    size_t held = 0;
    try {
        std::memset(out_guid16, 0, 16);             // a bad output pointer faults here, before anything is stored
        if (!gt::alloc_owned(client, std::string(utf8 ? utf8 : ""), client_registered, gb, &held))
            return SF6CORE_ERR_DISABLED;
        std::memcpy(out_guid16, &gb.hi, 8);
        std::memcpy(out_guid16 + 8, &gb.lo, 8);
    } catch (...) {
        return SF6CORE_ERR_FAULTED;
    }
    if (!gt::installed() && s_initialized.load(std::memory_order_acquire) && on_recorded_game_thread())
        gt::install();                              // the game may resolve the GUID as soon as it is in its data
    if (!g_wanted.exchange(true))
        core_log(SF6CORE_LOG_INFO, client->client_id, "texts: first client text, GetMessage hook %s",
                 gt::installed() ? "in place" : (gt::install_failed() ? "failed" : "installed at the next frame"));
    if (power_of_ten_from_1000(held))
        core_log(SF6CORE_LOG_WARN, client->client_id, "texts: %zu texts held (txt_release the ones no longer shown)",
                 held);
    return SF6CORE_OK;
}

SF6CoreStatus SF6CORE_CALL api_txt_set(SF6CoreClient* client, const uint8_t guid16[16], const char* utf8) {
    if (!client || !guid16) return SF6CORE_ERR_INVALID_ARG;
    if (core_closing()) return SF6CORE_ERR_DISABLED;
    try {
        return gt::retext_owned(client, from16(guid16), std::string(utf8 ? utf8 : "")) ? SF6CORE_OK
                                                                                       : SF6CORE_ERR_NOT_FOUND;
    } catch (...) {
        return SF6CORE_ERR_FAULTED;
    }
}

SF6CoreStatus SF6CORE_CALL api_txt_release(SF6CoreClient* client, const uint8_t guid16[16]) {
    if (!client) return SF6CORE_ERR_INVALID_ARG;
    try {
        if (!guid16) {
            const size_t n = gt::release_owner(client);
            if (n) core_log(SF6CORE_LOG_INFO, client->client_id, "texts: %zu released", n);
            return SF6CORE_OK;
        }
        return gt::release_owned(client, from16(guid16)) ? SF6CORE_OK : SF6CORE_ERR_NOT_FOUND;
    } catch (...) {
        return SF6CORE_ERR_FAULTED;
    }
}

uint32_t SF6CORE_CALL api_txt_status(char* buf, uint32_t cap) {
    std::string line;
    try {
        const gt::Stats st = gt::stats();
        std::vector<std::pair<gt::Owner, size_t>> per;
        gt::owners(per);
        char b[256];
        std::snprintf(b, sizeof b, "texts: hook %s, live %zu (options %zu, clients %zu), lookups %llu, hits %llu (clients %llu)",
                      hook_state(), st.live, st.live - st.owned, st.owned, (unsigned long long)st.lookups,
                      (unsigned long long)st.hits, (unsigned long long)st.owned_hits);
        line = b;
        for (size_t i = 0; i < per.size(); ++i) {
            // client handles are never freed (unregister_client keeps them): reading the id is safe
            std::snprintf(b, sizeof b, "%s%s %zu", i ? ", " : "; ", static_cast<const SF6CoreClient*>(per[i].first)->client_id,
                          per[i].second);
            line += b;
        }
    } catch (...) {
        line = "texts: status failed";
    }
    const uint32_t n = (uint32_t)line.size();
    if (buf && cap) {
        const uint32_t k = n < cap - 1 ? n : cap - 1;
        std::memcpy(buf, line.data(), k);
        buf[k] = 0;
        return k;
    }
    return n;
}
