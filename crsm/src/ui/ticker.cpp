// ui/ticker — the game's ticker strip (bottom right) with CRSM's texts (phase P4, owner "ui"), NativeTicker.lua (TK) in
// C++: a TickerRequestData handed to hTicker.Manager.RequestShowTicker; the game asks the request for its message through
// TickerRequestData.GetMessage, which our hook answers for our requests only (keyed by the request's RequestId).
// Texts pushed from any thread are shown on the frame callback; before the game is out of its boot flow they wait (20 at
// most, TK:27-31). Entries are dropped a minute after their request (the Lua's table only grew).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "common.hpp"
#include "game/gm.hpp"
#include "gx.hpp"
#include "internal.hpp"
#include "logic.hpp"
#include "mech/mech.hpp"
#include "perf.hpp"

namespace ui::ticker {
namespace {

namespace gm = game::gm;
using gx::MO;

constexpr int64_t kInitArg = 112;               // TickerRequestData.Init(112, nil) (TK:21)
constexpr int64_t kTickerId = 1;                // TK:22
constexpr int64_t kCategory = 6;                // TK:25 default category
constexpr double kSeconds = 3.0;                // RMT:79 ticker(text): 3 s
constexpr size_t kQueueMax = 20;                // TK:28
constexpr uint64_t kKeepMs = 60000;             // an answered request's text is kept a minute (display: 3 s)
constexpr size_t kKeepMax = 64;

struct Entry { uint8_t id[16]; std::string text; uint64_t at; MO* req = nullptr; };   // req: our reference, released at the purge
std::mutex g_mx;                                 // everything below: the GetMessage hook runs on any thread
std::deque<std::string> g_pending;
std::vector<Entry> g_entries;
uint64_t g_shown = 0, g_answered = 0, g_dropped = 0;
bool ok_get = false;

bool ready() {                                   // _ticker_is_ready (TK:14-17): out of the boot flow
    MO* mgr = gx::singleton("app.bFlowManager");
    int32_t id = 1;
    return mgr && gm::call_i32(mgr, "get_MainFlowID", &id) && id != 1;
}

bool request_id(MO* req, uint8_t out[16]) {      // RequestId (a 16-byte value field), the Lua's key (TK:35, 54)
    if (!req) return false;
    reframework::API::Field* f = gm::field(req->get_type_definition(), "RequestId");
    if (!f || f->is_static()) return false;
    gx::TD* t = f->get_type();
    if (!t || !t->is_valuetype() || t->get_valuetype_size() < 16) return false;
    void* p = f->get_data_raw(req, false);
    if (!p) return false;
    std::memcpy(out, p, 16);
    return true;
}

// show_custom_ticker (TK:24-42), on the game thread
void show(const std::string& text) {
    gx::TD* util = gm::type("app.TickerUtil");
    gx::TD* rtd = gm::type("app.TickerRequestData");
    gx::TD* htk = gm::type("app.helper.hTicker");
    if (!util || !rtd || !htk) return;
    // the request: what the Lua's .cctor pre-hook created (create_instance(simplify) + Init(112, nil), TickerId 1), then the
    // static constructor of TickerUtil runs, as the Lua's own call of it did (TK:33)
    MO* req = rtd->create_instance(1);
    if (!req) return;
    req->add_ref();                              // held (the game keeps the request after RequestShowTicker)
    gx::invoke(req, "Init(System.Int32, System.Object)", { gx::I(kInitArg), gx::P(nullptr) });
    gx::set_field_i64(req, "TickerId", kTickerId);
    if (reframework::API::Method* cctor = util->find_method(".cctor")) gm::call(nullptr, cctor, {});
    Entry e{};
    if (!request_id(req, e.id)) { req->release(); cmn::journal("ui: ticker request without a RequestId: dropped"); return; }
    e.text = text;
    e.at = GetTickCount64();
    e.req = req;
    std::vector<MO*> done;
    {
        std::lock_guard lk(g_mx);
        std::erase_if(g_entries, [&](const Entry& x) { const bool old = e.at - x.at > kKeepMs; if (old && x.req) done.push_back(x.req); return old; });
        if (g_entries.size() >= kKeepMax) { if (g_entries.front().req) done.push_back(g_entries.front().req); g_entries.erase(g_entries.begin()); }
        g_entries.push_back(e);
    }
    for (MO* r : done) r->release();             // a minute after their request: the game shows a ticker 3 s
    gx::set_field_i64(req, "Category", kCategory);
    if (gx::api()) perf::counted(gx::api())->obj_set_f64(gx::cl(), req, "DisplaySecond", kSeconds);
    reframework::API::Method* gm_mgr = htk->find_method("get_Manager");
    reframework::InvokeRet r{};
    MO* manager = (gm_mgr && gm::call(nullptr, gm_mgr, {}, &r)) ? static_cast<MO*>(r.ptr) : nullptr;
    if (manager) gx::invoke(manager, "RequestShowTicker(app.TickerRequestData)", { gx::P(req) });
    ++g_shown;
}

// -- TickerRequestData.GetMessage: our requests' texts (TK:52-67)
thread_local bool t_hit = false;
thread_local std::string t_text;
int get_pre_body(void** argv) {
    t_hit = false;
    uint8_t id[16];
    {
        std::lock_guard lk(g_mx);
        if (g_entries.empty()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;   // every ticker of the game comes here: fast path
    }
    if (!request_id(static_cast<MO*>(argv[1]), id)) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    std::lock_guard lk(g_mx);
    for (const Entry& e : g_entries) {
        if (std::memcmp(e.id, id, 16) != 0) continue;
        t_text = e.text;
        t_hit = true;
        ++g_answered;
        return REFRAMEWORK_HOOK_SKIP_ORIGINAL;
    }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void get_post_body(void** ret) {
    if (!t_hit || !ret) return;
    t_hit = false;
    // managed strings handed to the game: one extra reference for it (the game releases what GetMessage returns, TK:43-50)
    MO* s = reframework::API::get()->create_managed_string_normal(t_text.c_str());
    if (s) { s->add_ref(); *ret = s; }
}
int get_pre(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (cmn::closing() || !ui::active()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    int rc = REFRAMEWORK_HOOK_CALL_ORIGINAL;
    __try { rc = get_pre_body(argv); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("TickerRequestData.GetMessage pre (SEH)"); }
    return rc;
}
void get_post(void** ret, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (cmn::closing()) return;
    __try { get_post_body(ret); } __except (EXCEPTION_EXECUTE_HANDLER) { ui::fault("TickerRequestData.GetMessage post (SEH)"); }
}

}  // namespace

void push(const std::string& text) {
    if (text.empty()) return;
    std::lock_guard lk(g_mx);
    if (g_pending.size() >= kQueueMax) { ++g_dropped; return; }   // (the Lua queued 20 before the boot, showed the rest at once)
    g_pending.push_back(text);
}

void frame() {
    std::string t;
    if (ui::logic::ticker_sink(ui::active(), ok_get) == ui::logic::TickerSink::Drop) {   // no GetMessage hook: no ticker of CRSM
        uint64_t n = 0;
        while (mech::pop_ticker(&t)) ++n;        // (the layers' too: frame_poll leaves them to the DLL's UI)
        std::lock_guard lk(g_mx);
        g_dropped += g_pending.size() + n;
        g_pending.clear();
        return;
    }
    while (mech::pop_ticker(&t)) push(t);       // the layers' asynchronous tickers (an import finished after an allocation)
    {
        std::lock_guard lk(g_mx);
        if (g_pending.empty()) return;           // nothing to show: no game read (the boot-flow check below is for a text only)
    }
    if (!ready()) return;
    std::deque<std::string> q;
    {
        std::lock_guard lk(g_mx);
        q.swap(g_pending);
    }
    for (const std::string& s : q) show(s);
}

void specs(std::vector<mech::hooks::Spec>& out) {
    ui::spec(out, "app.TickerRequestData", "GetMessage", get_pre, get_post, "no ticker of CRSM", &ok_get);
}

std::string status() {
    std::lock_guard lk(g_mx);
    char b[160];
    std::snprintf(b, sizeof(b), "shown %llu, answered %llu, dropped %llu, waiting %zu, hook %d", (unsigned long long)g_shown,
                  (unsigned long long)g_answered, (unsigned long long)g_dropped, g_pending.size(), ok_get);
    return b;
}

}  // namespace ui::ticker
