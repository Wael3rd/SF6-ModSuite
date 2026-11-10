// SF6 Slots -- live texture overrides of the game's interface (ui_retexture.hpp; the pure part: ui_retexture_logic.hpp).
//
// Why: the loader's redirect (stages/loader/stage_redirect.cpp) serves the chosen option's copy when the game asks for a file, but
// the game asks for some of its interface textures once, at boot, and keeps them for the session (the character select's grid
// icons, gui/data/Character_Image/<id>/caracterselect/tex_Esf<id>_face_L_s_IAM.tex): a choice changed in the game would show only
// after a restart. Proved in game on 10/11 (Lua, reframework/agent/tmp/exp_retex.lua): a TextureResourceHolder made for another
// file and given (setTexture) to each via.gui.Texture that showed the first one changed the icon on screen at once.
//
// What:
// - the overrides (real file -> file to show) come from the loader (SF6UISlots_Overrides, refresh_from_loader) or set_overrides;
//   paths are matched in lower case, without "natives/stm/" nor a version number (ui_retexture_logic.hpp).
// - the elements: every via.gui.Texture (or a type made from it) under a via.gui.GUI's View (get_Child / get_Next), the GUIs from
//   the scene's list, fetched again in each frame that walks (R35: a GUI or an element is reached from that list only; an address
//   kept is for comparison, never used to reach an object). Whole GUIs per frame: nothing is held across frames but holders.
// - when: once over every GUI, shown or not, when the overrides change; a GUI new or newly shown, at once, 20 and 90 frames later
//   (a screen fills its elements over its first frames); every 2 s the GUIs shown (the sweep); a GUI in which a later walk found
//   an element to change (the game sets textures there after it is built: a picture that follows the cursor), every 12 frames for
//   10 s after the last such change. At most kBudgetUs of walks per frame; each pass's cost is logged.
// - a target's holder is made when an element first needs it (most overridden files are not on screen: the redirect serves them
//   when the game asks), or ahead when its real file was shown earlier this session; given to elements once loaded (60 frames, the
//   plugin's rule); kept while it is a target or an element of ours may show it. A texture set on an element keeps no reference we
//   can count on (rto.cpp learned it for materials, 09/11), so our reference outlives every element showing it: given back only
//   when no override names it, no element of ours shows it, a walk over every GUI found none showing it (a copy the game made) and
//   2 s passed (the frame in flight).
// - an element whose real file is not overridden any more gets back the game's own holder, referenced by us since we replaced it.
// - never touched: an element whose current file is not overridden, unless it shows our holder (to give the game's back).
// - reframework/data/SF6_UISlots_Data/retexture_off.txt: no override at all (read when the overrides are set), for tests;
//   retexture_test.txt there: overrides in the same lines, read once at the first frame (a test without the loader's export).
// Every engine call is under SEH; 16 faults stop the service for the session (what it set stays, referenced).
#include "slots.hpp"
#include "slots_ui.hpp"
#include "ui_retexture.hpp"
#include "ui_retexture_logic.hpp"

#include <deque>
#include <mutex>
#include <set>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")   // GetProcessMemoryInfo (the passes over every GUI log the process's memory)

namespace slots::ui_retexture {
namespace {

using Book = retex::Book<uintptr_t, Obj*>;
using Rec = Book::Rec;
using retex::Step;

const char* kOffFile = "reframework/data/SF6_UISlots_Data/retexture_off.txt";
const char* kTestFile = "reframework/data/SF6_UISlots_Data/retexture_test.txt";   // overrides read at the first frame (tests)
constexpr uint32_t kLookEvery = 10;        // frames between two looks at the scene's GUIs (new, newly shown), with overrides
constexpr uint32_t kLookIdle = 30;         // the same without, while elements of ours may come back on screen (to be put back)
constexpr uint32_t kSweepEvery = 120;      // every 2 s: the GUIs shown, walked again
constexpr uint32_t kHotEvery = 12;         // a GUI the game changes textures in after it is built: walked every 12 frames ...
constexpr uint32_t kHotFor = 600;          // ... until 10 s after the last change found there
constexpr uint32_t kFreshAt[] = { 0, 20, 90 };
constexpr int64_t  kBudgetUs = 600;        // walks per frame (whole GUIs: at least one)
constexpr uint32_t kLoadFrames = 60;       // a texture is given to an element once loaded (slots.hpp)
constexpr uint32_t kReleaseAfter = 120;    // a holder no override names and no element of ours shows: given back 2 s later at least
constexpr uint32_t kFightWindow = 300;     // the game put an element of ours back to its own file within 5 s ...
constexpr int      kFightMax = 3;          // ... three times running: that element is left alone until the overrides change
constexpr int      kFaultsMax = 16;        // engine faults before the service stops for the session
constexpr size_t   kNodesMax = 60000;      // elements per GUI (a guard)
constexpr int      kSiblingsMax = 8192;    // children of one element (a guard)

// ---------------------------------------------------------------------------------------------
// Engine access, under SEH (no C++ object with a destructor in these functions)
// ---------------------------------------------------------------------------------------------

decltype(REFrameworkTDBMethod::invoke) g_invoke = nullptr;                      // the SDK's invoke: no allocation per call
decltype(REFrameworkManagedObject::get_type_definition) g_type_of = nullptr;
decltype(REFrameworkManagedObject::add_ref) g_add_ref = nullptr;
decltype(REFrameworkManagedObject::release) g_release = nullptr;

void* raw0(Method* m, void* obj, bool& bad) {
    if (!m || !obj) return nullptr;
    reframework::InvokeRet out{};
    __try {
        if (g_invoke(reinterpret_cast<REFrameworkMethodHandle>(m), obj, nullptr, 0, &out, sizeof out) != REFRAMEWORK_ERROR_NONE ||
            out.exception_thrown)
            return nullptr;
        return out.ptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; return nullptr; }
}
bool raw0_bool(Method* m, void* obj, bool fallback, bool& bad) {
    if (!m || !obj) return fallback;
    reframework::InvokeRet out{};
    __try {
        if (g_invoke(reinterpret_cast<REFrameworkMethodHandle>(m), obj, nullptr, 0, &out, sizeof out) != REFRAMEWORK_ERROR_NONE ||
            out.exception_thrown)
            return fallback;
        return out.byte != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; return fallback; }
}
int32_t raw0_i32(Method* m, void* obj, bool& bad) {
    if (!m || !obj) return 0;
    reframework::InvokeRet out{};
    __try {
        if (g_invoke(reinterpret_cast<REFrameworkMethodHandle>(m), obj, nullptr, 0, &out, sizeof out) != REFRAMEWORK_ERROR_NONE ||
            out.exception_thrown)
            return 0;
        return static_cast<int32_t>(out.dword);
    } __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; return 0; }
}
void* raw1_i32(Method* m, void* obj, int32_t v, bool& bad) {
    if (!m || !obj) return nullptr;
    reframework::InvokeRet out{};
    void* args[1] = { arg_i32(v) };
    __try {
        if (g_invoke(reinterpret_cast<REFrameworkMethodHandle>(m), obj, args, sizeof(void*), &out, sizeof out) != REFRAMEWORK_ERROR_NONE ||
            out.exception_thrown)
            return nullptr;
        return out.ptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; return nullptr; }
}
TD* type_of(void* o, bool& bad) {
    if (!o) return nullptr;
    __try { return reinterpret_cast<TD*>(g_type_of(reinterpret_cast<REFrameworkManagedObjectHandle>(o))); }
    __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; return nullptr; }
}
// A holder's resource (+0x10, as the core's ui_set_texture compares two holders)
uintptr_t res_of(const void* holder, bool& bad) {
    if (!holder) return 0;
    __try { return *reinterpret_cast<const volatile uintptr_t*>(static_cast<const uint8_t*>(holder) + 0x10); }
    __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; return 0; }
}
bool add_ref(Obj* o) {
    if (!o) return false;
    __try { g_add_ref(reinterpret_cast<REFrameworkManagedObjectHandle>(o)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void release(Obj* o) {
    if (!o) return;
    __try { g_release(reinterpret_cast<REFrameworkManagedObjectHandle>(o)); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
using ExportFn = int (*)(char*, int);
int call_export(ExportFn fn, char* buf, int cap) {
    __try { return fn(buf, cap); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return retex::kFault; }
}

int64_t now_us() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / f.QuadPart) * 1000000 + (c.QuadPart % f.QuadPart) * 1000000 / f.QuadPart;
}
double ms(int64_t us) { return static_cast<double>(us) / 1000.0; }

int g_faults = 0;
bool g_broken = false;
void fault(const char* where) {
    ++g_faults;
    if (g_faults <= 4) logf("ui retexture: FAULT in %s (%d this session)", where, g_faults);
    if (g_faults >= kFaultsMax && !g_broken) {
        g_broken = true;
        logf("ui retexture: %d faults: the live texture swap stops for this session (what it set stays, its holders kept)", g_faults);
    }
}

// ---------------------------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------------------------

struct Types {
    bool ok = false;
    uint32_t tried_at = 0;
    TD* gui = nullptr; TD* transform = nullptr; TD* texture = nullptr; TD* play = nullptr;
    Method* view = nullptr;       // via.gui.GUI.get_View
    Method* enabled = nullptr;    // via.Component.get_Enabled
    Method* visible = nullptr;    // via.gui.PlayObject.get_Visible
    Method* child = nullptr;      // via.gui.TransformObject.get_Child (declared there: never called on a leaf, core/src/discovery.cpp)
    Method* next = nullptr;       // via.gui.PlayObject.get_Next
};
Types T;

bool resolve() {
    if (T.ok) return true;
    if (T.tried_at && g_frame - T.tried_at < 120) return false;
    T.tried_at = g_frame | 1;
    auto* tdb = api().tdb();
    T.gui = tdb->find_type("via.gui.GUI");
    T.transform = tdb->find_type("via.gui.TransformObject");
    T.texture = tdb->find_type("via.gui.Texture");
    T.play = tdb->find_type("via.gui.PlayObject");
    T.view = method_of(T.gui, "get_View", 0);
    T.enabled = method_of(T.gui, "get_Enabled", 0);
    T.visible = method_of(T.play, "get_Visible", 0);
    T.child = method_of(T.transform, "get_Child", 0);
    T.next = method_of(T.play, "get_Next", 0);
    const auto* sdk = api().sdk();
    g_invoke = sdk->method->invoke;
    g_type_of = sdk->managed_object->get_type_definition;
    g_add_ref = sdk->managed_object->add_ref;
    g_release = sdk->managed_object->release;
    T.ok = T.gui && T.transform && T.texture && T.play && T.view && T.enabled && T.child && T.next && g_invoke && g_type_of &&
           g_add_ref && g_release;
    static int told = 0;
    if (!T.ok && told++ < 2)
        logf("ui retexture: the game's GUI types are not all there (GUI %s, View %s, Enabled %s, TransformObject.get_Child %s, "
             "PlayObject.get_Next %s, Texture %s): no live texture swap", T.gui ? "ok" : "NO", T.view ? "ok" : "NO",
             T.enabled ? "ok" : "NO", T.child ? "ok" : "NO", T.next ? "ok" : "NO", T.texture ? "ok" : "NO");
    return T.ok;
}

// An element type: does it hold children, does it show a texture we may change (via.gui.Texture or a type made from it)
struct Kind { bool transform = false; Method* get = nullptr; };
std::unordered_map<TD*, Kind> g_kinds;
std::set<std::string> g_other_types;            // other element types with a getTexture: logged once, never changed here
bool g_other_logged = false;
Kind kind_of(TD* td) {
    auto it = g_kinds.find(td);
    if (it != g_kinds.end()) return it->second;
    Kind k;
    bool tex = false;
    for (TD* t = td; t; t = t->get_parent_type()) {
        if (t == T.transform) k.transform = true;
        if (t == T.texture) tex = true;
    }
    if (tex) k.get = method_of(td, "getTexture", 0);
    else if (method_of(td, "getTexture", 0)) g_other_types.insert(td->get_full_name());
    g_kinds.emplace(td, k);
    return k;
}

// A holder's file, as matched ("" not read)
std::unordered_map<TD*, Method*> g_path_m;
std::string holder_key(Obj* h, bool& bad) {
    TD* td = type_of(h, bad);
    if (!td) return {};
    auto it = g_path_m.find(td);
    Method* m = it != g_path_m.end() ? it->second : (g_path_m[td] = method_of(td, "get_ResourcePath", 0));
    void* s = raw0(m, h, bad);
    return s ? retex::key_of(str(static_cast<Obj*>(s))) : std::string();
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

std::mutex g_mx;
std::string g_text;                             // the overrides given (set_overrides), taken on the game thread
std::atomic<bool> g_dirty{false};
bool g_live = false;                            // anything to do or watch: overrides, elements of ours, holders to give back

Book g_book;                                    // the overrides and the elements of ours
std::map<std::string, std::string> g_paths;     // target key -> its path as asked of the game
std::unordered_map<uintptr_t, const std::string*> g_res_real;   // resource -> its real key in the map (nullptr: not overridden)
std::unordered_set<std::string> g_shown_reals;  // real files an element showed this session (their next targets made ahead)
int g_same_holder = -1;                         // getTexture gives back the very holder set: 1, another one of its texture: 0

struct Target {
    std::string path;                           // as asked of the game
    Obj* holder = nullptr;                      // one reference held
    uintptr_t res = 0;
    uint32_t born = 0;
    bool failed = false;
    uint32_t unneeded_since = 0;                // no override names it and no element of ours shows it, since this frame
    std::vector<uintptr_t> waiting;             // GUIs with an element waiting for it to load
};
std::map<std::string, Target> g_targets;
std::unordered_map<uintptr_t, std::string> g_by_res;       // our holders' resources -> target key
std::unordered_map<Obj*, std::string> g_by_holder;          // our holders -> target key
retex::Later<Obj*> g_later;                                 // the game's own holders given back: our reference let go later
int g_made = 0, g_load_failed = 0;

// The scene's GUIs this frame
struct Gui { uintptr_t id; Obj* obj; };
std::vector<Gui> g_list;
std::unordered_map<uintptr_t, Obj*> g_by_id;
std::unordered_map<uintptr_t, retex::GuiSeen> g_seen;
uint32_t g_look_no = 0, g_next_look = 0;
bool g_first_look_logged = false;

// Walks to do, by priority
enum JobKind { JFresh, JReady, JHot, JFull, JSweep, kKinds };
constexpr bool kAnyState[kKinds] = { false, true, false, true, false };   // a GUI not shown is walked too
const char* kKindName[kKinds] = { "screens shown", "files loaded", "changing screens", "every GUI", "sweep" };
struct Job { uintptr_t gid; bool late; };       // late: a walk after its GUI was built (a change found: the GUI is hot)
std::deque<Job> g_jobs[kKinds];
struct Fresh { uint32_t due; uintptr_t gid; bool late; };
std::vector<Fresh> g_fresh;
std::unordered_map<uintptr_t, uint32_t> g_hot;  // GUI -> walked every kHotEvery frames until then
uint32_t g_next_hot = 0, g_next_sweep = 0, g_next_gc = 0;
bool g_full_due = false;
const char* g_full_why = "";
uint32_t g_full_start = 0, g_last_full_start = 0;
int g_sweeps = 0;

struct Stats {
    uint32_t first = 0;                          // frame the pass began (0: none in progress)
    int guis = 0, frames = 0;
    size_t nodes = 0, textures = 0, paths = 0;
    int applied = 0, swapped = 0, restored = 0, forgotten = 0, adopted = 0, waiting = 0, refused = 0, made = 0;
    int64_t us = 0, max_us = 0, list_us = 0;
    int changes() const { return applied + swapped + restored + forgotten + adopted; }
};
Stats g_stats[kKinds];
bool jobs_left() {
    if (g_full_due || !g_fresh.empty()) return true;
    for (const auto& q : g_jobs) if (!q.empty()) return true;
    return false;
}

// ---------------------------------------------------------------------------------------------
// Targets
// ---------------------------------------------------------------------------------------------

Target* target_for(const std::string& key, Stats* st) {
    auto it = g_targets.find(key);
    if (it != g_targets.end()) return &it->second;
    auto p = g_paths.find(key);
    if (p == g_paths.end()) return nullptr;
    Target& t = g_targets[key];
    t.path = p->second;
    t.born = g_frame;
    t.holder = ui::tex_load(t.path.c_str());
    if (!t.holder) {
        t.failed = true;
        if (++g_load_failed <= 8) logf("ui retexture: %s cannot be loaded (not in the paks?): its elements keep the game's own", t.path.c_str());
        return &t;
    }
    bool bad = false;
    t.res = res_of(t.holder, bad);
    if (t.res) g_by_res[t.res] = key;
    g_by_holder[t.holder] = key;
    ++g_made;
    if (st) ++st->made;
    return &t;
}
bool ready(const Target& t) { return t.holder && !t.failed && g_frame - t.born >= kLoadFrames && ui::tex_ready(t.holder); }
void wait_for(Target& t, uintptr_t gid) {
    for (uintptr_t g : t.waiting) if (g == gid) return;
    t.waiting.push_back(gid);
}
// One of our holders (by holder, or by texture while an element may give back another holder of it), else nullptr
const std::string* mine_of(Obj* h, uintptr_t res) {
    if (auto it = g_by_holder.find(h); it != g_by_holder.end()) return &it->second;
    if (g_same_holder != 1 && res)
        if (auto it = g_by_res.find(res); it != g_by_res.end()) return &it->second;
    return nullptr;
}
bool shows(const Rec& r, Obj* h, uintptr_t res) { return g_same_holder == 1 ? h == r.ours : (h == r.ours || (res && res == r.ours_res)); }

// After a setTexture: does the element show the target now? (A fault reading it back counts as shown: its record keeps our holder.)
bool took(Obj* el, const Kind& k, const Target& t) {
    bool bad = false;
    Obj* back = static_cast<Obj*>(raw0(k.get, el, bad));
    const uintptr_t res = back ? res_of(back, bad) : 0;
    if (bad) { fault("getTexture after setTexture"); return true; }
    const bool same = back && back == t.holder;
    const bool on = same || (res && res == t.res);
    if (on && g_same_holder < 0) {
        g_same_holder = same ? 1 : 0;
        logf("ui retexture: an element gives back %s (getTexture): ours are recognised by %s", same ? "the very holder set on it" :
             "another holder of the texture set", same ? "holder" : "texture");
    }
    if (!on) {
        static int told = 0;
        if (told++ < 3) logf("ui retexture: setTexture did not take on an element (%s asked): left as it is", t.path.c_str());
    }
    return on;
}

// The map's real key of the file a holder shows, nullptr when not overridden. A resource known as not overridden is not read
// again until the overrides change; one overridden is read again each time (an address may have been reused).
const std::string* real_of(Obj* h, uintptr_t res, Stats& st) {
    if (res) {
        auto it = g_res_real.find(res);
        if (it != g_res_real.end() && !it->second) return nullptr;
    }
    bool bad = false;
    const std::string key = holder_key(h, bad);
    if (bad) { fault("get_ResourcePath"); return nullptr; }
    ++st.paths;
    const std::string* real = key.empty() ? nullptr : g_book.real_key(key);
    if (res) {
        if (g_res_real.size() > 50000) g_res_real.clear();
        g_res_real[res] = real;
    }
    return real;
}

// ---------------------------------------------------------------------------------------------
// One element
// ---------------------------------------------------------------------------------------------

void visit(Obj* el, const Kind& k, uintptr_t gid, Stats& st, int& changed) {
    bool bad = false;
    Obj* h = static_cast<Obj*>(raw0(k.get, el, bad));
    if (bad) { fault("getTexture"); return; }
    ++st.textures;
    if (!h) return;
    const uintptr_t eid = reinterpret_cast<uintptr_t>(el);
    const uintptr_t res = res_of(h, bad);
    if (bad) { fault("a holder's texture"); return; }

    bool forgot = false;
    std::string old_real;
    uint32_t old_at = 0;
    auto rit = g_book.recs.find(eid);
    if (rit != g_book.recs.end()) {
        Rec& r = rit->second;
        if (shows(r, h, res)) {
            const std::string* to = nullptr;
            const Step s = g_book.recorded(r, &to);
            if (s == Step::Keep) return;
            if (s == Step::Swap) {                                   // its file shows another target now
                Target* t = target_for(*to, &st);
                if (!t || t->failed) return;                         // the one it shows stays
                if (!ready(*t)) { wait_for(*t, gid); ++st.waiting; return; }
                ui::set_tex(el, t->holder);
                if (!took(el, k, *t)) { ++st.refused; return; }
                g_book.retarget(eid, *to, t->holder, t->res, g_frame);
                ++st.swapped;
                ++changed;
                return;
            }
            // Restore: its file is not overridden any more
            Obj* orig = r.orig;
            if (!orig) return;                                       // adopted without the game's own: ours stays, referenced
            ui::set_tex(el, orig);
            Obj* back = static_cast<Obj*>(raw0(k.get, el, bad));
            const uintptr_t bres = back ? res_of(back, bad) : 0;
            if (bad) { fault("getTexture after setTexture"); return; }   // not known: the record keeps ours referenced
            if (shows(r, back, bres)) { ++st.refused; return; }
            Rec gone = g_book.take(eid);
            g_later.add(gone.orig, g_frame + kReleaseAfter);
            ++st.restored;
            return;
        }
        // the game (or another mod) gave it another texture since
        Rec gone = g_book.take(eid);
        g_later.add(gone.orig, g_frame + kReleaseAfter);
        old_real = gone.real;
        old_at = gone.at;
        forgot = true;
        ++st.forgotten;
    }

    // One of our textures on an element we did not change (the game copied it from one of ours): ours stays while it shows it, with
    // the game's own of an element it came from when it is the very holder
    if (const std::string* mk = mine_of(h, res)) {
        const std::string tkey = *mk;
        auto t = g_targets.find(tkey);
        if (t == g_targets.end()) return;
        Obj* orig = nullptr;
        std::string real;
        if (h == t->second.holder)
            for (const auto& kv : g_book.recs)
                if (kv.second.target == tkey && kv.second.orig) {
                    if (add_ref(kv.second.orig)) { orig = kv.second.orig; real = kv.second.real; }
                    break;
                }
        g_book.add(eid, real, tkey, orig, t->second.holder, t->second.res, g_frame);
        ++st.adopted;
        return;
    }
    if (g_book.map.empty() || g_book.is_contested(eid)) return;
    const std::string* real = real_of(h, res, st);
    if (forgot && g_book.put_back(eid, real && *real == old_real, g_frame, old_at, kFightWindow, kFightMax)) {
        static int told = 0;
        if (told++ < 3)
            logf("ui retexture: an element showing %s is put back to the game's own each time (%d times within %u frames of our change): "
                 "left alone until the overrides change", real ? real->c_str() : "?", kFightMax, kFightWindow);
        return;
    }
    if (!real) return;
    const std::string* to = g_book.target_of(*real);
    Target* t = to ? target_for(*to, &st) : nullptr;
    if (!t || t->failed) return;
    if (!ready(*t)) { wait_for(*t, gid); ++st.waiting; return; }
    if (!add_ref(h)) return;                                         // the game's own, referenced until given back
    ui::set_tex(el, t->holder);
    if (!took(el, k, *t)) { release(h); ++st.refused; return; }
    g_book.add(eid, *real, *to, h, t->holder, t->res, g_frame);
    g_shown_reals.insert(*real);
    ++st.applied;
    ++changed;
}

// ---------------------------------------------------------------------------------------------
// One GUI, whole, in this frame
// ---------------------------------------------------------------------------------------------

std::vector<void*> g_stack;
void walk(Obj* gui, uintptr_t gid, Stats& st, bool late) {
    bool bad = false;
    void* view = raw0(T.view, gui, bad);
    if (bad) { fault("get_View"); return; }
    if (!view) return;
    const int64_t t0 = now_us();
    int changed = 0;
    size_t n = 0;
    g_stack.clear();
    g_stack.push_back(view);
    while (!g_stack.empty() && n < kNodesMax && !g_broken) {
        void* node = g_stack.back();
        g_stack.pop_back();
        ++n;
        TD* td = type_of(node, bad);
        if (bad) { fault("an element's type"); break; }
        if (!td) continue;
        const Kind k = kind_of(td);
        if (k.get) visit(static_cast<Obj*>(node), k, gid, st, changed);
        if (!k.transform) continue;
        int guard = 0;
        for (void* c = raw0(T.child, node, bad); c && !bad && guard < kSiblingsMax; c = raw0(T.next, c, bad), ++guard) g_stack.push_back(c);
        if (bad) { fault("get_Child / get_Next"); break; }
    }
    if (n >= kNodesMax) {
        static bool told = false;
        if (!told) { told = true; logf("ui retexture: a GUI with more than %zu elements: walked up to there", kNodesMax); }
    }
    const int64_t us = now_us() - t0;
    st.nodes += n;
    ++st.guis;
    st.us += us;
    st.max_us = std::max(st.max_us, us);
    if (changed && late) g_hot[gid] = g_frame + kHotFor;
}

// Shown: its component enabled and its View visible. *view: its View (nullptr when not enabled)
bool shown_of(Obj* gui, void** view, bool& bad) {
    const bool en = raw0_bool(T.enabled, gui, false, bad);
    void* v = en ? raw0(T.view, gui, bad) : nullptr;
    const bool vis = v && raw0_bool(T.visible, v, true, bad);
    *view = v;
    return en && vis;
}

bool list_guis() {
    g_list.clear();
    g_by_id.clear();
    Obj* arr = find_components(T.gui);
    if (!arr) return false;
    static TD* s_td = nullptr;
    static Method* s_count = nullptr;
    static Method* s_item = nullptr;
    bool bad = false;
    TD* td = type_of(arr, bad);
    if (!td) return false;
    if (td != s_td) { s_td = td; s_count = method_of(td, "get_Count", 0); s_item = method_of(td, "get_Item", 1); }
    const int32_t n = raw0_i32(s_count, arr, bad);
    for (int32_t i = 0; i < n && !bad; ++i)
        if (void* g = raw1_i32(s_item, arr, i, bad)) {
            const uintptr_t id = reinterpret_cast<uintptr_t>(g);
            g_list.push_back({ id, static_cast<Obj*>(g) });
            g_by_id[id] = static_cast<Obj*>(g);
        }
    if (bad) { fault("the scene's GUI list"); return false; }
    return true;
}

// The GUIs new or newly shown since the last look: walked at once and a little later
void look(int64_t list_us) {
    const int64_t t0 = now_us();
    std::vector<retex::GuiNow> now;
    now.reserve(g_list.size());
    bool bad = false;
    int shown = 0;
    for (const Gui& g : g_list) {
        void* v = nullptr;
        const bool on = shown_of(g.obj, &v, bad);
        if (bad) { fault("a GUI's state"); return; }
        shown += on ? 1 : 0;
        now.push_back({ g.id, reinterpret_cast<uintptr_t>(v), on });
    }
    const bool first = g_look_no == 0;
    const std::vector<uintptr_t> fresh = retex::diff_guis(g_seen, now, ++g_look_no, first);
    if (!g_first_look_logged) {
        g_first_look_logged = true;
        logf("ui retexture: %zu GUIs in the scene, %d shown; a look costs %.2f ms (their list %.2f ms, their states %.2f ms)",
             g_list.size(), shown, ms(list_us + now_us() - t0), ms(list_us), ms(now_us() - t0));
    }
    for (uintptr_t gid : fresh)
        for (uint32_t at : kFreshAt) g_fresh.push_back({ g_frame + at, gid, at != 0 });
}

// Every GUI listed now, those shown at the last look first
void queue_all(JobKind kd, bool shown_only) {
    for (int pass = 0; pass < 2; ++pass)
        for (const Gui& g : g_list) {
            auto s = g_seen.find(g.id);
            const bool on = s != g_seen.end() && s->second.shown;
            if (pass == 0 ? on : (!on && !shown_only)) g_jobs[kd].push_back({ g.id, kd == JSweep });
        }
}

void pass_done(int kd) {
    Stats& st = g_stats[kd];
    const bool log = kd == JFull || st.changes() || st.made || st.refused || (kd == JSweep && (g_sweeps <= 2 || g_sweeps % 150 == 0));
    if (log) {
        char what[128];
        if (kd == JFull) {
            PROCESS_MEMORY_COUNTERS_EX pmc{};                        // the holders kept cost memory: the process's, for the log
            pmc.cb = sizeof pmc;
            const bool mem = GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc) != 0;
            snprintf(what, sizeof what, "every GUI (%s; private %llu MB)", g_full_why,
                     mem ? static_cast<unsigned long long>(pmc.PrivateUsage >> 20) : 0ull);
        } else {
            snprintf(what, sizeof what, "%s", kKindName[kd]);
        }
        logf("ui retexture: %s: %d GUIs, %zu elements, %zu textures (%zu files read), %d changed, %d swapped, %d put back, %d given "
             "another by the game, %d copies of ours, %d refused, %d waiting for their file, %d holders made; %.2f ms in %d frames "
             "(largest GUI %.2f ms, GUI lists %.2f ms); %zu elements of ours, %zu holders", what, st.guis, st.nodes, st.textures, st.paths,
             st.applied, st.swapped, st.restored, st.forgotten, st.adopted, st.refused, st.waiting, st.made, ms(st.us), st.frames,
             ms(st.max_us), ms(st.list_us), g_book.recs.size(), g_targets.size());
    }
    if (kd == JFull) {
        g_last_full_start = g_full_start;
        if (!g_other_logged && !g_other_types.empty()) {
            g_other_logged = true;
            std::string names;
            for (const auto& n : g_other_types) names += (names.empty() ? "" : ", ") + n;
            logf("ui retexture: other element types with a getTexture (left as they are): %s", names.c_str());
        }
    }
    st = Stats{};
}

void walk_jobs(int64_t list_us) {
    const int64_t start = now_us();
    std::unordered_set<uintptr_t> done;
    bool any = false, out = false;
    bool worked[kKinds] = {};
    for (int kd = 0; kd < kKinds && !out; ++kd) {
        auto& q = g_jobs[kd];
        Stats& st = g_stats[kd];
        while (!q.empty() && !g_broken) {
            if (any && now_us() - start >= kBudgetUs) { out = true; break; }
            const Job j = q.front();
            q.pop_front();
            if (!st.first) st.first = g_frame;
            if (!done.insert(j.gid).second) continue;
            auto it = g_by_id.find(j.gid);
            if (it == g_by_id.end()) continue;                       // gone
            if (!kAnyState[kd]) {
                bool bad = false;
                void* v = nullptr;
                const bool on = shown_of(it->second, &v, bad);
                if (bad) { fault("a GUI's state"); continue; }
                if (!on) continue;
            }
            walk(it->second, j.gid, st, j.late);
            any = true;
            worked[kd] = true;
        }
    }
    for (int kd = 0; kd < kKinds; ++kd) {
        Stats& st = g_stats[kd];
        if (worked[kd]) { ++st.frames; st.list_us += list_us; }
        if (st.first && g_jobs[kd].empty()) pass_done(kd);
    }
}

// Holders given back: the game's own once their delay is over; ours once no override names them, no element of ours shows them, a
// walk over every GUI begun after that found no copy, and kReleaseAfter frames passed
void gc() {
    g_later.run(g_frame, [](Obj* h) { release(h); });
    int freed = 0;
    bool check = false;
    for (auto it = g_targets.begin(); it != g_targets.end();) {
        Target& t = it->second;
        if (g_book.needed(it->first)) { t.unneeded_since = 0; ++it; continue; }
        if (!t.unneeded_since) { t.unneeded_since = g_frame; ++it; continue; }
        if (!t.holder) { it = g_targets.erase(it); continue; }     // never made (failed)
        if (g_frame - t.unneeded_since < kReleaseAfter) { ++it; continue; }
        if (g_last_full_start < t.unneeded_since || g_full_due || !g_jobs[JFull].empty() || g_stats[JFull].first) { check = true; ++it; continue; }
        g_by_holder.erase(t.holder);
        if (t.res) g_by_res.erase(t.res);
        release(t.holder);
        ++freed;
        it = g_targets.erase(it);
    }
    if (check && !g_full_due && g_jobs[JFull].empty() && !g_stats[JFull].first) { g_full_due = true; g_full_why = "before holders are given back"; }
    if (freed) logf("ui retexture: %d holders given back (no override names them, no element shows them), %zu kept", freed, g_targets.size());
    for (auto it = g_hot.begin(); it != g_hot.end();) it = static_cast<int32_t>(g_frame - it->second) > 0 ? g_hot.erase(it) : std::next(it);
}

// ---------------------------------------------------------------------------------------------
// The overrides given, taken on the game thread
// ---------------------------------------------------------------------------------------------

void take() {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        text = g_text;
        g_dirty.store(false, std::memory_order_release);
    }
    retex::Parsed p = retex::parse(text);
    if (GetFileAttributesA(kOffFile) != INVALID_FILE_ATTRIBUTES && !p.map.empty()) {
        logf("ui retexture: %s is there: none of the %zu overrides is applied", kOffFile, p.map.size());
        p = retex::Parsed{};
    }
    if (p.map == g_book.map) return;
    const size_t before = g_book.map.size();
    g_book.set_map(std::move(p.map));
    g_paths = std::move(p.paths);
    g_res_real.clear();                                              // its values point into the map
    // the targets of files an element showed earlier this session: made now, loaded when their screen comes back
    int ahead = 0;
    for (const auto& kv : g_book.map)
        if (g_shown_reals.count(kv.first) && !g_targets.count(kv.second))
            if (const Target* t = target_for(kv.second, nullptr); t && !t->failed) ++ahead;
    logf("ui retexture: %zu overrides (were %zu; %d lines: %d not read, %d to the same file, %d listed twice), %d holders made ahead "
         "(files shown earlier); %zu elements of ours", g_book.map.size(), before, p.lines, p.bad, p.same, p.twice, ahead, g_book.recs.size());
    for (auto& q : g_jobs) q.clear();
    for (auto& st : g_stats) st = Stats{};
    g_fresh.clear();
    g_full_due = true;
    g_full_why = "the overrides changed";
    g_next_sweep = g_frame + kSweepEvery;
    g_live = true;
}

void tick() {
    const bool have = !g_book.map.empty();
    const bool looking = g_frame >= g_next_look;
    const bool sweep = have && g_frame >= g_next_sweep && g_jobs[JSweep].empty() && !g_stats[JSweep].first;
    const bool hot = have && !g_hot.empty() && g_frame >= g_next_hot;
    bool due = looking || sweep || hot || g_full_due;
    for (const auto& q : g_jobs) due = due || !q.empty();
    for (const Fresh& f : g_fresh) due = due || static_cast<int32_t>(g_frame - f.due) >= 0;
    for (const auto& kv : g_targets) due = due || (!kv.second.waiting.empty() && ready(kv.second));
    if (due) {
        const int64_t t0 = now_us();
        const bool listed = list_guis();
        const int64_t list_us = now_us() - t0;
        if (listed) {
            if (looking) { g_next_look = g_frame + (have ? kLookEvery : kLookIdle); look(list_us); }
            for (auto it = g_fresh.begin(); it != g_fresh.end();) {
                if (static_cast<int32_t>(g_frame - it->due) < 0) { ++it; continue; }
                g_jobs[JFresh].push_back({ it->gid, it->late });
                it = g_fresh.erase(it);
            }
            for (auto& kv : g_targets) {
                Target& t = kv.second;
                if (t.waiting.empty() || !ready(t)) continue;
                for (uintptr_t g : t.waiting) g_jobs[JReady].push_back({ g, false });
                t.waiting.clear();
            }
            if (hot) {
                g_next_hot = g_frame + kHotEvery;
                for (const auto& kv : g_hot) g_jobs[JHot].push_back({ kv.first, true });
            }
            if (g_full_due) {
                g_full_due = false;
                g_full_start = g_frame;
                queue_all(JFull, false);
                if (g_jobs[JFull].empty()) g_last_full_start = g_full_start;   // no GUI at all: nothing could show ours
            }
            if (sweep) { g_next_sweep = g_frame + kSweepEvery; ++g_sweeps; queue_all(JSweep, true); }
            walk_jobs(list_us);
        }
    }
    if (static_cast<int32_t>(g_frame - g_next_gc) >= 0) { g_next_gc = g_frame + 30; gc(); }
}

}  // namespace

void set_overrides(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_text = text;
    g_dirty.store(true, std::memory_order_release);
}

void refresh_from_loader() {
    static std::atomic<ExportFn> s_fn{ nullptr };
    static std::atomic<bool> s_missing_told{ false }, s_off_told{ false };
    ExportFn fn = s_fn.load(std::memory_order_acquire);
    if (!fn) {
        HMODULE h = GetModuleHandleW(L"amd_ags_x64.dll");
        fn = h ? reinterpret_cast<ExportFn>(GetProcAddress(h, "SF6UISlots_Overrides")) : nullptr;
        if (!fn) {
            if (!s_missing_told.exchange(true))
                logf("ui retexture: amd_ags_x64.dll %s SF6UISlots_Overrides: no live texture swap from the loader", h ? "has no" : "is not loaded: no");
            return;
        }
        s_fn.store(fn, std::memory_order_release);
    }
    std::string text;
    const int r = retex::read_export([fn](char* b, int cap) { return call_export(fn, b, cap); }, text);
    if (r == retex::ReadFaulted) { logf("ui retexture: SF6UISlots_Overrides faulted: the overrides stay as they are"); return; }
    if (r == retex::ReadTooLong) { logf("ui retexture: SF6UISlots_Overrides kept growing: the overrides stay as they are"); return; }
    if (r == retex::ReadNotRunning) {
        if (!s_off_told.exchange(true)) logf("ui retexture: the loader's UI slots are not running: no override");
        set_overrides(std::string());
        return;
    }
    set_overrides(text);
}

void frame() {
    static bool s_test_looked = false;                               // retexture_test.txt: overrides for a test without the loader, once
    if (!s_test_looked) {
        s_test_looked = true;
        const std::string test = read_file(kTestFile, 1u << 20);
        if (!test.empty()) {
            logf("ui retexture: %s read: its overrides until the loader gives others", kTestFile);
            set_overrides(test);
        }
    }
    const bool dirty = g_dirty.load(std::memory_order_acquire);
    if (!dirty && !g_live) return;
    if (g_broken) return;
    try {
        if (!resolve()) return;
        if (dirty) take();
        tick();
        const bool live = !g_book.map.empty() || !g_book.recs.empty() || !g_targets.empty() || !g_later.empty() || jobs_left();
        if (!live && g_live) {                                       // nothing left: the next override starts from a first look
            g_seen.clear();
            g_look_no = 0;
            g_hot.clear();
        }
        g_live = live;
    } catch (...) {
        static int told = 0;
        if (told++ < 3) logf("ui retexture: EXCEPTION in a frame");
    }
}

}  // namespace slots::ui_retexture
