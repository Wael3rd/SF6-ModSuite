#ifndef NOMINMAX
#define NOMINMAX
#endif
// pausemenu::core - see pm_core.hpp. No game header in this file: it is also compiled, as it is, into the offline
// bench of the state machine.

#include "pm_core.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace pausemenu::core {
namespace {

constexpr uint64_t kNever = ~0ULL;
constexpr size_t kMaxRetired = 4096;

struct KeyHash {
    size_t operator()(const Key& k) const { return (size_t)(k.hi ^ (k.lo * 0x9E3779B97F4A7C15ULL)); }
};

struct RowRec {
    int64_t id = -1, page = -1;
    Owner owner = nullptr;
    Kind kind = Kind::Button;
    std::string title, guide;
    uint32_t capacity = 1;
    bool keep_open = false;
    float spread = 0.0f;
    std::vector<std::string> labels;
    // built (install pass)
    Obj data = nullptr;
    Key key, guide_key;
    std::vector<Obj> opts;                   // `capacity` option objects, built once
    std::vector<Key> opt_keys;
    int32_t opt_n = 0;                       // options in the array the game reads
    std::vector<Obj> arrays;                 // every option array made for it (held until the row is released)
    // live (hooks)
    int32_t row_index = -1;
    uint64_t built_frame = kNever;
    uint64_t force_until = 0;
    bool forced = false;
    int32_t value = 0;
    bool has_value = false;
    float base_w = 0, base_h = 0;
    bool has_base = false;
    int cool = 0;
    bool shrunk = false;                     // the engine wrote a smaller font on its option text
    bool retext = false;                     // labels changed while its page was shown
    bool removed = false;
    bool out_of_page = false;                // removed, and its page's rows array no longer holds it
};

struct PageRec {
    int64_t id = -1;
    Owner owner = nullptr;
    int32_t primary_func = 0, strip_index = 0;
    std::string title, guide;
    std::vector<int64_t> rows;               // live rows, in order
    Obj data = nullptr;
    Obj rows_array = nullptr;
    std::vector<Obj> arrays;                 // rows arrays made for it (held until the page is released)
    Key key, guide_key;
    bool rows_dirty = true;
    bool removed = false;
    bool installed = false;
    bool shown = false;
};

struct StripRec {
    Obj row = nullptr;
    int32_t orig_type = 0;                   // the strip's type without our pages
    std::vector<Obj> game_pages;
    bool fail_logged = false;
    size_t fail_count = 0;
};

struct IdentRec { What what = What::None; int64_t id = -1; int32_t option = -1; };

struct HintRec {
    Owner owner = nullptr;
    int32_t button = 0;
    std::string text;
    Key key;
    Obj obj = nullptr;
    bool removed = false;
};

Backend be;
std::recursive_mutex mx;                     // recursive: a game call made from here may fire one of the hooks
std::map<int64_t, PageRec> g_pages;
std::map<int64_t, RowRec> g_rows;
std::unordered_map<Key, IdentRec, KeyHash> g_keys;
std::unordered_set<Key, KeyHash> g_retired;  // keys of data the core built and released since (the purge's answer)
std::map<int32_t, StripRec> g_strips;        // per primary_func
std::map<int32_t, int64_t> g_row_at;         // list index -> the row last laid out there
std::vector<HintRec> g_hints;
bool g_hints_full_logged = false;
int64_t g_next_id = 0;
std::atomic<bool> g_game_rows{false};
std::atomic<bool> g_any_shown{false};
int64_t g_shown = -1;
std::atomic<int64_t> g_rebuild_page{-1};    // read without the lock (every scroll group asks, every frame)
uint64_t g_rebuild_at = 0;
int64_t g_building = -1;                    // the page being built now (its Function post-hook)
uint64_t g_training_since = kNever;
bool g_force_pass = false;
uint64_t g_passes = 0, g_writes = 0;
std::set<int32_t> g_logged_no_strip;
thread_local std::string tl_error;

void logf(const char* fmt, ...) {
    if (!be.log) return;
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    be.log(buf);
}

// A trace line (the backend's trace mode): nothing is formatted unless the backend is tracing on this thread.
void tracef(const char* fmt, ...) {
    if (!be.trace || !be.tracing || !be.tracing()) return;
    char buf[480];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    be.trace(buf);
}

int64_t fail(const char* why) { tl_error = why; return -1; }

PageRec* page_of(Owner owner, int64_t id) {
    auto it = g_pages.find(id);
    if (it == g_pages.end() || it->second.removed || (owner && it->second.owner != owner)) return nullptr;
    return &it->second;
}
RowRec* row_of(Owner owner, int64_t id) {
    auto it = g_rows.find(id);
    if (it == g_rows.end() || it->second.removed || (owner && it->second.owner != owner)) return nullptr;
    return &it->second;
}
RowRec* row_any(int64_t id) {
    auto it = g_rows.find(id);
    return it == g_rows.end() ? nullptr : &it->second;
}

int32_t labels_n_of(const RowRec& r) {
    const size_t n = std::min<size_t>(r.labels.size(), r.capacity);
    return n < 1 ? 1 : (int32_t)n;
}

void retire(const Key& k) {
    if (k.zero()) return;
    g_keys.erase(k);
    if (g_retired.size() >= kMaxRetired) g_retired.clear();
    g_retired.insert(k);
}

void text_release(Key& k) {
    if (k.zero()) return;
    if (be.text_release) be.text_release(k);
    retire(k);
    k = Key{};
}

void drop(Obj& o) {
    if (o && be.release) be.release(o);
    o = nullptr;
}

// A text made for something that could not be built (never registered).
void rel(const Key& k) { if (!k.zero() && be.text_release) be.text_release(k); }

Ident ident_of(const IdentRec& ir) {
    Ident out;
    out.what = ir.what;
    out.option = ir.option;
    if (ir.what == What::Page) {
        auto it = g_pages.find(ir.id);
        if (it == g_pages.end()) return Ident{};
        out.id = ir.id;
        out.page = ir.id;
        out.removed = it->second.removed;
        out.owner = it->second.owner;
        return out;
    }
    auto it = g_rows.find(ir.id);
    if (it == g_rows.end()) return Ident{};
    const RowRec& r = it->second;
    out.id = r.id;
    out.page = r.page;
    out.removed = r.removed;
    out.owner = r.owner;
    out.kind = r.kind;
    out.keep_open = r.keep_open;
    return out;
}

Ident identify_locked(const Key& k) {
    if (k.zero()) return Ident{};
    auto it = g_keys.find(k);
    return it == g_keys.end() ? Ident{} : ident_of(it->second);
}

void recompute_any_shown() {
    bool any = false;
    for (auto& [id, p] : g_pages) if (p.shown) { any = true; break; }
    g_any_shown = any;
    if (!any) g_shown = -1;
}

void page_set_not_shown(PageRec& p, std::vector<Edge>* edges) {
    if (!p.shown) return;
    p.shown = false;
    if (edges) edges->push_back({ p.id, p.owner, false });
    if (g_shown == p.id) g_shown = -1;
}

// ---- building (install pass) ----

bool build_row(RowRec& r) {
    if (r.data) return true;
    if (!be.text_new || !be.new_row_data) return false;
    Key k, g;
    if (!be.text_new(r.title, k)) return false;
    if (!be.text_new(r.guide, g)) { rel(k); return false; }
    Obj d = be.new_row_data(r.kind == Kind::Spin ? kItemSpin : kItemTextOnly, k, g);
    if (!d) { rel(k); rel(g); return false; }
    r.data = d; r.key = k; r.guide_key = g;
    g_keys[k] = { What::Row, r.id, -1 };
    if (r.kind == Kind::Spin) {
        // Option slots: `capacity` objects built once, their texts changed in place; the array the game reads holds
        // the first labels_n of them (the count shown follows the labels, ADR-0016 point 3).
        for (uint32_t i = 0; i < r.capacity; ++i) {
            Key ok;
            if (!be.text_new(i < r.labels.size() ? r.labels[i] : std::string(), ok)) break;
            Obj o = be.new_option_data ? be.new_option_data(ok) : nullptr;
            if (!o) { rel(ok); break; }
            r.opts.push_back(o);
            r.opt_keys.push_back(ok);
            g_keys[ok] = { What::Option, r.id, (int32_t)i };
        }
        const int32_t n = std::min<int32_t>(labels_n_of(r), (int32_t)r.opts.size());
        if (n > 0 && be.new_array && be.set_children) {
            Obj arr = be.new_array(std::vector<Obj>(r.opts.begin(), r.opts.begin() + n));
            if (arr && be.set_children(d, arr)) { r.arrays.push_back(arr); r.opt_n = n; }
            else if (arr) drop(arr);
        }
    }
    tracef("row %lld built: data %p, %s, %zu option object(s), %d in its option array", (long long)r.id, d,
           r.kind == Kind::Spin ? "spin" : "button", r.opts.size(), r.opt_n);
    return true;
}

std::vector<Obj> live_rows_data(const PageRec& p) {
    std::vector<Obj> v;
    for (int64_t rid : p.rows) {
        RowRec* r = row_any(rid);
        if (r && !r->removed && r->data) v.push_back(r->data);
    }
    return v;
}

bool build_page(PageRec& p, Obj template_page) {
    for (int64_t rid : p.rows) {
        RowRec* r = row_any(rid);
        if (r && !r->removed && !build_row(*r)) { logf("[pausemenu] page %lld: row %lld could not be built", (long long)p.id, (long long)rid); return false; }
    }
    const std::vector<Obj> rows = live_rows_data(p);
    if (rows.empty()) return false;
    if (!p.data) {
        if (!template_page || !be.new_page_data || !be.new_array || !be.text_new) return false;
        Key k, g;
        if (!be.text_new(p.title, k)) return false;
        if (!be.text_new(p.guide, g)) { rel(k); return false; }
        Obj arr = be.new_array(rows);
        Obj d = arr ? be.new_page_data(template_page, k, g, arr) : nullptr;
        if (!d) {
            tracef("page %lld: %s", (long long)p.id, arr ? "page data not built" : "rows array not built");
            drop(arr); rel(k); rel(g); return false;
        }
        p.data = d; p.key = k; p.guide_key = g;
        p.rows_array = arr; p.arrays.push_back(arr);
        g_keys[k] = { What::Page, p.id, -1 };
        p.rows_dirty = false;
        tracef("page %lld built: data %p, rows array %p (%zu rows), template %p", (long long)p.id, d, arr, rows.size(),
               template_page);
    } else if (p.rows_dirty) {
        Obj arr = be.new_array ? be.new_array(rows) : nullptr;
        if (!arr || !be.set_children || !be.set_children(p.data, arr)) { drop(arr); return false; }
        p.rows_array = arr; p.arrays.push_back(arr);
        p.rows_dirty = false;
        for (auto& [id, r] : g_rows) if (r.page == p.id && r.removed) r.out_of_page = true;
    }
    return true;
}

// 2 pages: the 2-page strip prefab, 3 pages: the 3-page one; the game's own count: its own type (nothing of ours).
int32_t strip_type(size_t count, size_t game_count, int32_t orig_type) {
    if (count == game_count) return orig_type;
    if (count == 2) return kStripTwo;
    if (count == 3) return kStripThree;
    return -1;
}

void install_strip(int32_t pf) {
    Strip s;
    if (!be.strip_read || !be.strip_read(pf, s) || !s.row) {
        if (g_logged_no_strip.insert(pf).second)
            logf("[pausemenu] primary tab %d: no sub-page strip found (its first row is not a strip of type %d / %d): its pages wait",
                 pf, kStripTwo, kStripThree);
        for (auto& [id, p] : g_pages) if (p.primary_func == pf) p.installed = false;
        return;
    }
    g_logged_no_strip.erase(pf);
    StripRec& sr = g_strips[pf];
    std::vector<Obj> game;
    bool ours_present = false;
    for (size_t i = 0; i < s.pages.size(); ++i) {
        const Ident id = i < s.keys.size() ? identify_locked(s.keys[i]) : Ident{};
        if (id.what == What::Page) ours_present = true;
        else game.push_back(s.pages[i]);   // the game's pages, and unknown ones (another engine's): never ours to drop
    }
    tracef("primary tab %d: strip row %p, type %d, %zu page(s): %zu of the game's, ours %s", pf, s.row, s.type,
           s.pages.size(), game.size(), ours_present ? "present" : "absent");
    if (!ours_present || sr.row != s.row) {   // the game's strip as it built it (first sight, or rebuilt by the game)
        sr.row = s.row;
        if (!ours_present || !sr.orig_type) sr.orig_type = s.type;
    }
    sr.game_pages = game;

    std::vector<PageRec*> want;
    for (auto& [id, p] : g_pages) {
        if (p.primary_func != pf || p.removed) continue;
        bool any_row = false;
        for (int64_t rid : p.rows) { RowRec* r = row_any(rid); if (r && !r->removed) { any_row = true; break; } }
        if (any_row) want.push_back(&p);
        else p.installed = false;
    }
    std::stable_sort(want.begin(), want.end(), [](const PageRec* a, const PageRec* b) {
        return a->strip_index != b->strip_index ? a->strip_index < b->strip_index : a->id < b->id;
    });
    std::vector<PageRec*> built;
    for (PageRec* p : want) {
        if (build_page(*p, game.empty() ? nullptr : game.front())) built.push_back(p);
        else p->installed = false;
    }
    // strip_index = the page's index in the final strip; pages asking for the same index keep their creation order
    std::vector<Obj> order = game;
    std::vector<Obj> placed;
    for (PageRec* p : built) {
        size_t pos = std::min<size_t>((size_t)std::max<int32_t>(p->strip_index, 0), order.size());
        while (pos < order.size() && std::find(placed.begin(), placed.end(), order[pos]) != placed.end()) ++pos;
        order.insert(order.begin() + (ptrdiff_t)pos, p->data);
        placed.push_back(p->data);
    }
    int32_t type = strip_type(order.size(), game.size(), sr.orig_type);
    if (type < 0) {
        if (!sr.fail_logged || sr.fail_count != order.size())
            logf("[pausemenu] primary tab %d: %zu pages wanted in its strip (%zu of the game's), no strip layout for that count: not installed",
                 pf, order.size(), game.size());
        sr.fail_logged = true;
        sr.fail_count = order.size();
        built.clear();
        order = game;
        type = sr.orig_type;
    } else {
        sr.fail_logged = false;
    }
    bool ok = true;
    tracef("primary tab %d: %zu page(s) of ours built, strip of %zu page(s), type %d (%s)", pf, built.size(), order.size(),
           type, (order != s.pages || type != s.type) ? "to write" : "unchanged");
    if (order != s.pages || type != s.type) {
        ok = be.strip_write && be.strip_write(s, order, type);
        if (ok) {
            ++g_writes;
            logf("[pausemenu] primary tab %d: strip written, %zu pages (%zu ours), type %d", pf, order.size(), built.size(), type);
        } else {
            logf("[pausemenu] primary tab %d: strip write failed", pf);
        }
    }
    for (auto& [id, p] : g_pages) {
        if (p.primary_func != pf) continue;
        const bool in = ok && std::find(built.begin(), built.end(), &p) != built.end();
        p.installed = in;
    }
    if (built.empty() && ok) {                 // nothing of ours left in it: forget it (the game's own strip again)
        bool any_page = false;
        for (auto& [id, p] : g_pages) if (p.primary_func == pf) { any_page = true; break; }
        if (!any_page) g_strips.erase(pf);
    }
}

void release_row(RowRec& r) {
    text_release(r.key);
    text_release(r.guide_key);
    for (auto& k : r.opt_keys) text_release(k);
    r.opt_keys.clear();
    for (auto& o : r.opts) drop(o);
    r.opts.clear();
    for (auto& a : r.arrays) drop(a);
    r.arrays.clear();
    drop(r.data);
}

void release_removed() {
    for (auto it = g_pages.begin(); it != g_pages.end();) {
        PageRec& p = it->second;
        if (p.removed && !p.installed) {
            text_release(p.key);
            text_release(p.guide_key);
            for (auto& a : p.arrays) drop(a);
            p.arrays.clear();
            drop(p.data);
            it = g_pages.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_rows.begin(); it != g_rows.end();) {
        RowRec& r = it->second;
        auto pit = g_pages.find(r.page);
        const bool page_holds = pit != g_pages.end() && pit->second.data && !r.out_of_page;   // still in its rows array
        if (r.removed && (!r.data || !page_holds)) {
            for (auto i = g_row_at.begin(); i != g_row_at.end();) { if (i->second == r.id) i = g_row_at.erase(i); else ++i; }
            release_row(r);
            it = g_rows.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_hints.begin(); it != g_hints.end();) {
        if (it->removed) { text_release(it->key); drop(it->obj); it = g_hints.erase(it); }
        else ++it;
    }
}

void pass() {
    std::set<int32_t> funcs;
    for (auto& [id, p] : g_pages) funcs.insert(p.primary_func);
    for (auto& [pf, s] : g_strips) funcs.insert(pf);
    tracef("pass %llu: %zu page(s), %zu row(s), %zu primary tab(s) to look at", (unsigned long long)g_passes,
           g_pages.size(), g_rows.size(), funcs.size());
    for (int32_t pf : funcs) install_strip(pf);
    release_removed();
}

// The backend's pass_end, also when the pass is left by a fault (the stack unwinds through it: /EHa).
struct PassScope {
    PassScope() { if (be.pass_begin) be.pass_begin(); }
    ~PassScope() { if (be.pass_end) be.pass_end(); }
    PassScope(const PassScope&) = delete;
    PassScope& operator=(const PassScope&) = delete;
};

}  // namespace

// ======================== public ========================

void set_backend(const Backend& b) { std::lock_guard lk(mx); be = b; }

std::string last_error() { return tl_error; }

int64_t add_page(Owner owner, int32_t primary_func, int32_t strip_index, const std::string& title, const std::string& guide) {
    if (!owner) return fail("no client");
    if (primary_func <= 0) return fail("primary_func must be a TrainingFuncType (> 0)");
    std::lock_guard lk(mx);
    PageRec p;
    p.id = g_next_id++;
    p.owner = owner;
    p.primary_func = primary_func;
    p.strip_index = strip_index < 0 ? 0 : strip_index;
    p.title = title;
    p.guide = guide;
    g_pages.emplace(p.id, p);
    return p.id;
}

int64_t add_row(Owner owner, int64_t page, Kind kind, const std::string& title, const std::string& guide,
                uint32_t capacity, bool keep_open, float arrow_spread) {
    if (!owner) return fail("no client");
    std::lock_guard lk(mx);
    PageRec* p = page_of(owner, page);
    if (!p) return fail("not one of the client's pages");
    if (p->rows.size() >= kMaxRows) return fail("13 rows at most per page (R9)");
    if (kind != Kind::Button && kind != Kind::Spin) return fail("unknown kind");
    if (kind == Kind::Spin && (capacity < 1 || capacity > kMaxCapacity)) return fail("spin capacity must be 1..64");
    if (!(arrow_spread >= 0.0f && arrow_spread <= 300.0f)) return fail("arrow_spread must be 0..300");
    RowRec r;
    r.id = g_next_id++;
    r.page = page;
    r.owner = owner;
    r.kind = kind;
    r.title = title;
    r.guide = guide;
    r.capacity = kind == Kind::Spin ? capacity : 0;
    r.keep_open = kind == Kind::Button && keep_open;
    r.spread = kind == Kind::Spin ? arrow_spread : 0.0f;
    g_rows.emplace(r.id, r);
    p->rows.push_back(r.id);
    p->rows_dirty = true;
    return r.id;
}

Result set_texts(Owner owner, int64_t id, const std::string* title, const std::string* guide) {
    std::lock_guard lk(mx);
    if (PageRec* p = page_of(owner, id)) {
        if (title) { p->title = *title; if (!p->key.zero() && be.text_set) be.text_set(p->key, *title); }
        if (guide) { p->guide = *guide; if (!p->guide_key.zero() && be.text_set) be.text_set(p->guide_key, *guide); }
        return kOk;
    }
    if (RowRec* r = row_of(owner, id)) {
        if (title) { r->title = *title; if (!r->key.zero() && be.text_set) be.text_set(r->key, *title); }
        if (guide) { r->guide = *guide; if (!r->guide_key.zero() && be.text_set) be.text_set(r->guide_key, *guide); }
        return kOk;
    }
    return kNotFound;
}

Result set_options(Owner owner, int64_t row, const std::vector<std::string>& labels, uint64_t frame) {
    std::lock_guard lk(mx);
    RowRec* r = row_of(owner, row);
    if (!r || r->kind != Kind::Spin) return kNotFound;
    const bool truncated = labels.size() > r->capacity;
    std::vector<std::string> v(labels.begin(), labels.begin() + (ptrdiff_t)std::min<size_t>(labels.size(), r->capacity));
    if (v == r->labels) return truncated ? kTruncated : kOk;   // nothing changed: no retext, no rebuild
    r->labels = std::move(v);
    for (size_t i = 0; i < r->opt_keys.size(); ++i) {
        if (i < r->labels.size()) { if (be.text_set) be.text_set(r->opt_keys[i], r->labels[i]); }
        else if (i == 0 && be.text_set) be.text_set(r->opt_keys[0], std::string());   // no label: one empty option
    }
    // Shown: the visible text refreshed at the next frame of the list, the page rebuilt 2 frames later (the carousel
    // and its count follow the new labels), as the reference engine's client did after a list change.
    auto pit = g_pages.find(r->page);
    if (pit != g_pages.end() && pit->second.shown && r->data && g_building != r->page) {   // (during its build: no rebuild)
        r->retext = true;
        g_rebuild_page = r->page;
        g_rebuild_at = frame + kRebuildDelay;
    }
    return truncated ? kTruncated : kOk;
}

Result remove(Owner owner, int64_t id, std::vector<Edge>* edges) {
    std::lock_guard lk(mx);
    if (PageRec* p = page_of(owner, id)) {
        p->removed = true;
        page_set_not_shown(*p, edges);
        for (int64_t rid : p->rows) if (RowRec* r = row_any(rid)) r->removed = true;
        if (g_rebuild_page == id) g_rebuild_page = -1;
        recompute_any_shown();
        return kOk;
    }
    if (RowRec* r = row_of(owner, id)) {
        r->removed = true;
        auto pit = g_pages.find(r->page);
        if (pit != g_pages.end()) {
            auto& rows = pit->second.rows;
            rows.erase(std::remove(rows.begin(), rows.end(), id), rows.end());
            pit->second.rows_dirty = true;
        }
        return kOk;
    }
    return kNotFound;
}

size_t remove_owner(Owner owner, std::vector<Edge>* edges) {
    if (!owner) return 0;
    std::lock_guard lk(mx);
    size_t n = 0;
    for (auto& [id, p] : g_pages) {
        if (p.owner != owner || p.removed) continue;
        p.removed = true;
        page_set_not_shown(p, edges);
        if (g_rebuild_page == id) g_rebuild_page = -1;
        ++n;
    }
    for (auto& [id, r] : g_rows) if (r.owner == owner && !r.removed) { r.removed = true; ++n; }
    for (auto& h : g_hints) if (h.owner == owner) h.removed = true;
    recompute_any_shown();
    return n;
}

Ident identify(const Key& key) {
    std::lock_guard lk(mx);
    return identify_locked(key);
}

int64_t row_of(Owner owner, const Key& key) {
    if (!owner) return -1;
    const Ident i = identify(key);
    if ((i.what != What::Row && i.what != What::Option) || i.removed || i.owner != owner) return -1;
    return i.id;
}

bool removable(const Key& key, bool has_delegate) {
    if (has_delegate || key.zero()) return false;
    std::lock_guard lk(mx);
    auto it = g_keys.find(key);
    if (it != g_keys.end()) return ident_of(it->second).removed;   // ours: only once removed by its client
    return g_retired.count(key) != 0;                               // built by the core, released since
}

Route route(int32_t ftype, Ctx ctx, bool game_rows_present) {
    if (ftype != kDynamic) return Route::Pass;
    if (ctx == Ctx::Ours) return Route::Ours;
    if (ctx == Ctx::Foreign) return Route::Pass;
    return game_rows_present ? Route::Pass : Route::Default;
}

int64_t const_answer(Method m, Route r, int32_t wanted) {
    switch (m) {
    case kGetIsActive: return 1;
    case kGetOptionIndex: return r == Route::Ours ? wanted : 0;
    default: return 0;
    }
}

void set_game_rows(bool present) { g_game_rows = present; }
bool game_rows() { return g_game_rows.load(); }

FnPlan plan_function(const FnInput& in) {
    FnPlan plan;
    if (in.ftype != kDynamic) return plan;
    std::lock_guard lk(mx);
    Ident it = in.data;
    if (it.what == What::None && in.parent.what != What::None) it = in.parent;   // secondary pages: ParentData = the row
    if (it.what == What::None) {
        if (!in.data_null) return plan;                    // a game row (its delegate runs) or an unknown one: passes
        if (in.ctx != Ctx::Ours || in.ctx_row < 0) return plan;
        RowRec* r = row_any(in.ctx_row);                    // no data on the ViewData: the row being laid out
        if (!r) return plan;
        it = ident_of({ What::Row, r->id, -1 });
    }
    plan.owner = it.owner;
    if (it.removed) { plan.act = FnAct::Retired; plan.id = it.id; return plan; }
    if (it.what == What::Page) { plan.act = FnAct::Page; plan.id = it.id; return plan; }
    RowRec* r = row_any(it.id);
    if (!r) return FnPlan{};
    if (in.row_index >= 0) { r->row_index = in.row_index; g_row_at[in.row_index] = r->id; }
    plan.id = r->id;
    plan.at_build = r->built_frame == in.frame;
    plan.keep_open = r->keep_open;
    plan.act = r->kind == Kind::Spin ? FnAct::Spin : FnAct::Button;
    return plan;
}

void building(int64_t page) { std::lock_guard lk(mx); g_building = page; }

// The option array of a spin sized to its labels, so the arrows stop at the last one (reference: prepare_page). The old
// array stays held (r.arrays): a reader of the previous one still reads live objects. Under mx.
static bool size_row(RowRec& r) {
    if (r.kind != Kind::Spin || !r.data) return false;
    const int32_t n = std::min<int32_t>(labels_n_of(r), (int32_t)r.opts.size());
    if (n <= 0 || n == r.opt_n || !be.new_array || !be.set_children) return false;
    Obj arr = be.new_array(std::vector<Obj>(r.opts.begin(), r.opts.begin() + n));
    if (arr && be.set_children(r.data, arr)) { r.arrays.push_back(arr); r.opt_n = n; return true; }
    if (arr) drop(arr);
    return false;
}

bool page_built(int64_t page, uint64_t frame) {
    std::lock_guard lk(mx);
    if (g_building == page) g_building = -1;
    PageRec* p = page_of(nullptr, page);
    if (!p || !p->data) return false;
    for (int64_t rid : p->rows) {
        RowRec* r = row_any(rid);
        if (!r || r->removed) continue;
        r->built_frame = frame;
        size_row(*r);
        r->retext = false;
    }
    return true;
}

bool size_options(int64_t row) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    return r && !r->removed && size_row(*r);
}

bool at_build(int64_t row, uint64_t frame) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    return r && r->built_frame == frame;
}

std::vector<Edge> set_shown(int64_t page) {
    std::lock_guard lk(mx);
    std::vector<Edge> edges;
    PageRec* p = page_of(nullptr, page);
    if (!p) return edges;
    for (auto& [id, o] : g_pages) if (id != page && o.shown && o.primary_func == p->primary_func) page_set_not_shown(o, &edges);
    if (!p->shown) { p->shown = true; edges.push_back({ p->id, p->owner, true }); }
    g_shown = page;
    g_any_shown = true;
    return edges;
}

std::vector<Edge> game_page_built(Obj page_data_obj) {
    std::lock_guard lk(mx);
    std::vector<Edge> edges;
    if (!page_data_obj) return edges;
    for (auto& [pf, s] : g_strips) {
        if (std::find(s.game_pages.begin(), s.game_pages.end(), page_data_obj) == s.game_pages.end()) continue;
        for (auto& [id, p] : g_pages) if (p.primary_func == pf) page_set_not_shown(p, &edges);
    }
    recompute_any_shown();
    return edges;
}

std::vector<Edge> menu_closed() {
    std::lock_guard lk(mx);
    std::vector<Edge> edges;
    for (auto& [id, p] : g_pages) page_set_not_shown(p, &edges);
    g_row_at.clear();
    g_rebuild_page = -1;                     // nothing to rebuild in a closed menu (its next build does it)
    recompute_any_shown();
    return edges;
}

bool any_shown() { return g_any_shown.load(std::memory_order_relaxed); }
int64_t shown_page() { std::lock_guard lk(mx); return g_shown; }

Obj page_data(int64_t page) {
    std::lock_guard lk(mx);
    auto it = g_pages.find(page);
    return it == g_pages.end() ? nullptr : it->second.data;
}

std::vector<int64_t> page_rows(int64_t page) {
    std::lock_guard lk(mx);
    auto it = g_pages.find(page);
    return it == g_pages.end() ? std::vector<int64_t>{} : it->second.rows;
}

bool is_game_page(Obj page_data_obj) {
    if (!page_data_obj) return false;
    std::lock_guard lk(mx);
    for (auto& [pf, s] : g_strips)
        if (std::find(s.game_pages.begin(), s.game_pages.end(), page_data_obj) != s.game_pages.end()) return true;
    return false;
}

bool row_info(int64_t row, RowInfo& out) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    if (!r) return false;
    out.id = r->id; out.page = r->page; out.owner = r->owner; out.kind = r->kind; out.keep_open = r->keep_open;
    out.removed = r->removed; out.spread = r->spread; out.capacity = r->capacity; out.labels_n = labels_n_of(*r);
    out.opt_n = r->opt_n; out.row_index = r->row_index; out.data = r->data; out.key = r->key;
    return true;
}

int32_t clamp_value(int64_t row, int32_t v) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    const int32_t n = r ? labels_n_of(*r) : 1;
    return v < 0 ? 0 : (v >= n ? n - 1 : v);
}

int32_t wrap(int64_t row, int32_t num) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    const int32_t n = r ? labels_n_of(*r) : 1;
    return ((num % n) + n) % n;
}

std::string label(int64_t row, int32_t index) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    if (!r || index < 0 || (size_t)index >= r->labels.size()) return std::string();
    return r->labels[(size_t)index];
}

Obj option_data(int64_t row, int32_t index) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    if (!r || index < 0 || index >= r->opt_n || (size_t)index >= r->opts.size()) return nullptr;
    return r->opts[(size_t)index];
}

void store_value(int64_t row, int32_t v) {
    std::lock_guard lk(mx);
    if (RowRec* r = row_any(row)) { r->value = v; r->has_value = true; }
}

bool stored_value(int64_t row, int32_t& v) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    if (!r || !r->has_value) return false;
    v = r->value;
    return true;
}

void set_row_index(int64_t row, int32_t index) {
    if (index < 0) return;
    std::lock_guard lk(mx);
    if (RowRec* r = row_any(row)) { r->row_index = index; g_row_at[index] = row; }
}

int64_t row_at(int32_t index) {
    std::lock_guard lk(mx);
    auto it = g_row_at.find(index);
    return it == g_row_at.end() ? -1 : it->second;
}

void force(int64_t row, uint64_t frame) {
    std::lock_guard lk(mx);
    if (RowRec* r = row_any(row)) { r->force_until = frame + kForceFrames; r->forced = true; }
}

bool forcing(int64_t row, uint64_t frame) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    return r && r->forced && frame <= r->force_until;
}

Fit fit_pure(float base_w, float base_h, float cur_w, float display_w, float spread, int& cool, bool& shrunk) {
    Fit f;
    const float room = (kArrowRightX + spread) - (kArrowLeftX - spread) - kTextMargin;
    if (!(display_w > 1.0f && cur_w > 0.0f)) return f;
    const float w_base = display_w * (base_w / cur_w);   // the natural width at the base font (stable)
    float scale = 1.0f;
    if (w_base > room) scale = std::max(kMinFontScale, room / w_base);
    const float want = base_w * scale;
    // A text that fits keeps the game's font: the game's focus state writes that font too, and a second writer on it
    // was a job-thread crash of the reference engine (08/09). The engine writes it only to shrink a long text, or once
    // to put back the size its own shrink changed.
    if (scale >= 1.0f) {
        if (!shrunk) return f;
        if (std::fabs(cur_w - want) <= kEpsilon) { shrunk = false; return f; }
    }
    cool -= 1;
    if (std::fabs(cur_w - want) > kEpsilon && cool <= 0) {
        cool = kFitCooldown;
        f.write = true;
        f.w = want;
        f.h = base_h * scale;
        shrunk = scale < 1.0f;
    }
    return f;
}

Fit fit(int64_t row, float cur_w, float cur_h, float display_w) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    if (!r || r->spread <= 0.0f) return Fit{};
    if (!r->has_base) { r->base_w = cur_w; r->base_h = cur_h; r->has_base = cur_w > 0.0f; }
    if (!r->has_base) return Fit{};
    return fit_pure(r->base_w, r->base_h, cur_w, display_w, r->spread, r->cool, r->shrunk);
}

float arrow_x(bool right, float spread) { return right ? kArrowRightX + spread : kArrowLeftX - spread; }

bool base_font(int64_t row, float& w, float& h) {
    std::lock_guard lk(mx);
    RowRec* r = row_any(row);
    if (!r || !r->has_base) return false;
    w = r->base_w; h = r->base_h;
    return true;
}

void request_rebuild(int64_t page, uint64_t at_frame) {
    std::lock_guard lk(mx);
    g_rebuild_page = page;
    g_rebuild_at = at_frame;
}

bool rebuild_pending() { return g_rebuild_page.load(std::memory_order_relaxed) >= 0; }

int64_t take_rebuild(uint64_t frame, bool hold) {
    std::lock_guard lk(mx);
    if (g_rebuild_page < 0 || frame < g_rebuild_at || hold) return -1;   // held while an option window is up
    const int64_t p = g_rebuild_page.exchange(-1);
    if (g_shown != p) return -1;            // the player left the page: the game rebuilds it at its next visit anyway
    PageRec* rec = page_of(nullptr, p);
    if (!rec || !rec->data) return -1;
    return p;                                // the caller runs page_built (arrays sized, rows "at build") then the rebuild
}

std::vector<int64_t> take_retext(int64_t page) {
    std::lock_guard lk(mx);
    std::vector<int64_t> out;
    auto it = g_pages.find(page);
    if (it == g_pages.end()) return out;
    for (int64_t rid : it->second.rows) {
        RowRec* r = row_any(rid);
        if (r && r->retext) { r->retext = false; out.push_back(rid); }
    }
    return out;
}

Obj hint_obj(Owner owner, int32_t button, const std::string& text) {
    std::lock_guard lk(mx);
    for (auto& h : g_hints) if (!h.removed && h.owner == owner && h.button == button && h.text == text) return h.obj;
    if (g_hints.size() >= kMaxHints) {
        if (!g_hints_full_logged) logf("[pausemenu] %u hint texts cached: new hint texts are not shown", kMaxHints);
        g_hints_full_logged = true;
        return nullptr;
    }
    if (!be.text_new || !be.new_hint) return nullptr;
    HintRec h;
    h.owner = owner; h.button = button; h.text = text;
    if (!be.text_new(text, h.key)) return nullptr;
    h.obj = be.new_hint(button, h.key);
    if (!h.obj) { rel(h.key); return nullptr; }
    g_hints.push_back(h);
    return h.obj;
}

std::vector<Verdict> hook_verdicts(const std::vector<HookCode>& hooks) {
    std::vector<Verdict> v(hooks.size(), Verdict::Refused);
    for (size_t i = 0; i < hooks.size(); ++i) {
        const HookCode& h = hooks[i];
        if (!h.found || !h.raw) { v[i] = Verdict::Missing; continue; }
        if (h.shared < 0) { v[i] = Verdict::Refused; continue; }           // not checked: never hooked blind
        if (h.shared == 0) { v[i] = Verdict::Hook; continue; }
        bool same_group = (size_t)h.shared == h.sharers.size();            // every sharer known...
        for (uint32_t x : h.sharers) {                                     // ... and a hook of the same group
            bool in = false;
            for (size_t j = 0; j < hooks.size(); ++j)
                if (j != i && hooks[j].found && hooks[j].index == x && hooks[j].group == h.group) in = true;
            if (!in) same_group = false;
        }
        if (!same_group) { v[i] = Verdict::Refused; continue; }
        if (h.raw != h.code) { v[i] = Verdict::Covered; continue; }        // it enters a group member's body
        bool taken = false;                                                // folded entries: the first one hooks
        for (size_t j = 0; j < i; ++j) if (v[j] == Verdict::Hook && hooks[j].code == h.code) taken = true;
        v[i] = taken ? Verdict::Covered : Verdict::Hook;
    }
    for (size_t i = 0; i < hooks.size(); ++i) {                           // covered only by a hook that goes in
        if (v[i] != Verdict::Covered) continue;
        bool body = false;
        for (size_t j = 0; j < hooks.size(); ++j) if (v[j] == Verdict::Hook && hooks[j].code == hooks[i].code) body = true;
        if (!body) v[i] = Verdict::Refused;
    }
    return v;
}

void tick(uint64_t frame, bool training, bool paused) {
    std::lock_guard lk(mx);
    if (!training) { g_training_since = kNever; return; }
    if (g_training_since == kNever) g_training_since = frame;
    if (paused) return;                                  // rule 10: the menu data is changed with the menu closed
    // A page not installed yet (training just started, the game's strip not built yet) is tried every kInstallRetry frames
    // during the first kRetryWindow frames of training; otherwise the pass checks every kInstallEvery.
    bool waiting = false;
    for (auto& [id, p] : g_pages) if (!p.removed && !p.installed) { waiting = true; break; }
    const uint64_t ran = frame - g_training_since;
    const bool due = g_force_pass || (ran >= (uint64_t)kInstallDelay &&
                     (waiting && ran < (uint64_t)kRetryWindow ? ran % kInstallRetry == 0 : (int)(frame % kInstallEvery) == kInstallPhase));
    if (!due) return;
    g_force_pass = false;
    ++g_passes;
    PassScope scope;
    pass();
}

void install_now() { std::lock_guard lk(mx); g_force_pass = true; }

Stats stats() {
    std::lock_guard lk(mx);
    Stats s;
    for (auto& [id, p] : g_pages) { if (p.removed) ++s.removed; else { ++s.pages; if (p.installed) ++s.pages_installed; } }
    for (auto& [id, r] : g_rows) { if (r.removed) ++s.removed; else ++s.rows; }
    s.hints = g_hints.size();
    s.shown = g_shown;
    s.game_rows = g_game_rows.load();
    s.passes = g_passes;
    s.writes = g_writes;
    return s;
}

std::string status() {
    const Stats s = stats();
    char b[256];
    std::snprintf(b, sizeof b, "pages %zu (installed %zu), rows %zu, removed pending %zu, hints %zu, shown %lld, game dynamic rows %s, passes %llu, strip writes %llu",
                  s.pages, s.pages_installed, s.rows, s.removed, s.hints, (long long)s.shown, s.game_rows ? "yes" : "no",
                  (unsigned long long)s.passes, (unsigned long long)s.writes);
    return b;
}

void reset() {
    std::lock_guard lk(mx);
    g_pages.clear(); g_rows.clear(); g_keys.clear(); g_retired.clear(); g_strips.clear(); g_row_at.clear();
    g_hints.clear(); g_hints_full_logged = false; g_next_id = 0; g_game_rows = false; g_any_shown = false; g_shown = -1;
    g_rebuild_page = -1; g_rebuild_at = 0; g_building = -1; g_training_since = kNever; g_force_pass = false; g_passes = 0; g_writes = 0;
    g_logged_no_strip.clear();
}

}  // namespace pausemenu::core
