// Zen Mode's engine: part of profile_mod.cpp (inside its unnamed namespace, after its helpers). Rules and decisions: zen_logic.hpp.
//
// What ZenMode.lua did on every GUI element drawn, every frame (three managed calls, then every path walked again from the view,
// every control's name read through Lua) is cut in two:
//   - the draw observer (REFramework's on_pre_gui_draw_element, the rendering thread) only keeps a table of the elements drawn:
//     an address, its GameObject's address, the frame it was last seen. No managed call, nothing written to the GUI. An element
//     new to it, or back after not being drawn, is handed to the game thread; while Zen Mode is on that element is not drawn for
//     the one or two frames this takes, so that nothing shows before it is hidden.
//   - LateUpdateBehavior (the game thread, before the render pass) names what was handed over, once, and applies the rules of the
//     owners in the table when zen::Sched says so: when they appear, a few times while they settle, then now and then. A frame
//     with nothing new and nothing due is two atomic reads and a comparison.
// Off since the game started: the observer is one atomic read per element, the game thread one.
//
// An element hidden as a whole (zen::whole_owner: the fight's name, title, control type and win count, the rank gauges, the
// rank-up animation) is not written to at all: the observer refuses its draw (ZSlot::veto, set by the game thread). The game
// writes ForceInvisible on those elements' c_main itself, off again at every round's start: a write of ours there did not last.
//
// During the fight itself the game thread looks at nothing: it reads the fight's state and the pause flag in the game's memory
// (two bytes, no call) and looks again when they change (zen_phase): a round's start and end, a pause.
//
// ForceInvisible stays on a control, and it is the game's own switch too (the rank-up notice of the player who has none is kept
// hidden with it; the Lua, and this engine until 11/11, set every ruled control back to visible and showed those). So every
// control Zen Mode hides is kept in a table (zg_marks, a reference held), and only those are shown again when they are no
// longer to be hidden; a control found hidden is the game's and is left alone (zen::decide). With Zen Mode off, or a group on
// Shown, since a screen came, nothing of that screen is written: the game's display, untouched.
// Once something was hidden the engine stays on for the session, even when Zen Mode is turned off, to give back the screens the
// game keeps loaded and shows again later.
// Not known: the game deciding to hide, with ForceInvisible, a control we already hide (no call tells it). Shown again by us,
// it stays so until the game sets its screen up again.

constexpr const char* kZenFile = "reframework/data/zen_mode.json";
constexpr const char* kZenTmp = "reframework/data/zen_mode.json.tmp";
constexpr const char* kZenLegacy = "reframework/data/ZenMode_config.json";
constexpr const char* kZenLua = "reframework/autorun/ZenMode.lua";
constexpr const char* kZenDebug = "reframework/data/zen_mode_debug.txt";      // tests: when it exists at start, the two below
constexpr const char* kZenStats = "reframework/data/zen_mode_stats.json";     //   counters, written every 300 frames
constexpr uint32_t kZSlots = 4096, kZProbe = 64, kZEvents = 256, kZHeldMax = 4, kZHoldAfter = 3;
constexpr uint32_t kZUpkeepEvery = 600, kZForget = 1800, kZNodes = 4096;

// REFramework's plugin functions from its plugin API 1.14 on: one more entry after those of the header this DLL is built with (1.10)
struct ZPluginFunctions114 {
    REFrameworkPluginFunctions base;
    bool (*on_pre_gui_draw_element)(bool (*)(void*, void*));
};

// ---- shared by the two threads, under zg_lock ----
struct ZSlot {
    uintptr_t elem, owner;
    uint32_t seen;
    uint8_t kind;      // 0 not named yet, 1 no rule for it, 2 an owner of the table
    uint8_t want;      // to be handed to the game thread
    uint8_t queued;    // handed, not answered yet
    uint8_t hold;      // not drawn until answered; held: for how many draws
    uint8_t held;
    uint8_t veto;      // an element hidden as a whole: not drawn (the game thread's word)
    uint8_t battle;    // of the fight's HUD: its draw says a fight is on screen
};
struct ZEvent { uintptr_t elem, owner; };
struct ZLock {
    SRWLOCK* l;
    explicit ZLock(SRWLOCK* x) : l(x) { AcquireSRWLockExclusive(l); }
    ~ZLock() { ReleaseSRWLockExclusive(l); }
    ZLock(const ZLock&) = delete;
    ZLock& operator=(const ZLock&) = delete;
};
SRWLOCK zg_lock = SRWLOCK_INIT;
ZSlot zg_slots[kZSlots];
ZSlot zg_slots_tmp[kZSlots];
uint32_t zg_slot_n = 0;
ZEvent zg_events[kZEvents];
uint32_t zg_event_n = 0;
std::atomic<uint32_t> zg_event_flag{ 0 };
std::atomic<uint32_t> zg_frame{ 0 };
std::atomic<uint32_t> zg_hud_seen{ 0 };         // the frame the fight's HUD was last drawn at
std::atomic<bool> zg_active{ false }, zg_hold{ false };
bool zg_debug = false;                         // set before the callbacks exist
std::atomic<uint64_t> zg_draws{ 0 }, zg_draw_qpc{ 0 }, zg_held_draws{ 0 };

// Under zg_lock. made: a slot just taken for it
ZSlot* zslot(uintptr_t e, bool create, bool* made = nullptr) {
    uint32_t i = static_cast<uint32_t>(((e >> 4) * 0x9E3779B97F4A7C15ull) >> 52);      // 12 bits
    for (uint32_t k = 0; k < kZProbe; ++k, i = (i + 1) & (kZSlots - 1)) {
        ZSlot& s = zg_slots[i];
        if (s.elem == e) return &s;
        if (!s.elem) {
            if (!create) return nullptr;
            s = ZSlot{};
            s.elem = e;
            ++zg_slot_n;
            if (made) *made = true;
            return &s;
        }
    }
    return nullptr;
}

// ---- the rendering thread ----
bool zen_on_draw(void* element, void*) {
    if (!zg_active.load(std::memory_order_relaxed)) return true;
    bool draw = true;
    LARGE_INTEGER t0{};
    if (zg_debug) QueryPerformanceCounter(&t0);
    try {
        const uintptr_t e = reinterpret_cast<uintptr_t>(element);
        if (!e) return true;
        const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(e + 0x10);        // a via.Component's GameObject
        const uint32_t f = zg_frame.load(std::memory_order_relaxed);
        ZLock lock(&zg_lock);
        bool made = false;
        if (ZSlot* s = zslot(e, true, &made)) {
            const bool fresh = made || s->owner != owner || f - s->seen > zen::kGone;
            if (s->owner != owner) { s->owner = owner; s->kind = 0; s->veto = 0; s->battle = 0; }
            if (fresh) {
                s->want = 1;
                if (zg_hold.load(std::memory_order_relaxed)) { s->hold = 1; s->held = 0; }
            }
            if (s->want && !s->queued && zg_event_n < kZEvents) {
                zg_events[zg_event_n++] = { e, owner };
                s->queued = 1;
                s->want = 0;
                zg_event_flag.store(1, std::memory_order_release);
            }
            s->seen = f;
            if (s->battle) zg_hud_seen.store(f, std::memory_order_relaxed);
            if (s->hold) {
                if (++s->held > kZHeldMax) s->hold = 0;       // the game thread did not answer: drawn as it is
                else draw = false;
            }
            if (s->veto) draw = false;
        }
    } catch (...) {}
    if (zg_debug) {
        LARGE_INTEGER t1{};
        QueryPerformanceCounter(&t1);
        zg_draws.fetch_add(1, std::memory_order_relaxed);
        zg_draw_qpc.fetch_add(static_cast<uint64_t>(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
        if (!draw) zg_held_draws.fetch_add(1, std::memory_order_relaxed);
    }
    return draw;
}

// ---- the game thread ----
struct ZState {
    uintptr_t elem = 0, owner = 0;
    int idx = -1;
    bool battle = false, whole = false;
    uint32_t walked = 0;       // the frame of its last whole walk
    zen::Sched sched;
};
struct ZDone { uintptr_t elem, owner; uint8_t kind, battle; };
struct ZType { TD* td; Method* child; Method* next; Method* name; };
struct ZTarget { uintptr_t ctrl; uint16_t rule; bool hide, copy; };
struct ZKeep { uintptr_t elem, ctrl; uint32_t at; uint16_t rule; };
struct ZMark { uintptr_t ctrl, elem; uint32_t seen; uint16_t rule; zen::Mark mark; };

zen::State zg_set;
std::vector<ZState> zg_states;
std::vector<ZType> zg_types;
std::vector<ZTarget> zg_targets;
std::vector<ZTarget> zg_merged;
std::vector<ZKeep> zg_kept;                     // what the last walk of an owner the game writes over found: a reference held on each
std::vector<ZMark> zg_marks;                    // the controls found hidden, ours or the game's: by address, a reference held on each
std::vector<ZDone> zg_done;
Method* zm_go_name = nullptr;
Method* zm_view = nullptr;
Method* zm_fi = nullptr;
bool zg_on = false;                            // the engine exists (REFramework and the core allow it)
uint32_t zg_f = 0, zg_next_due = 0, zg_upkeep_at = 0, zg_hold_at = 0;
FILETIME zg_file_time{};
struct {
    uint64_t work = 0, qpc = 0, qpc_max = 0, applies = 0, names = 0, writes = 0, events = 0, nodes_cut = 0, phases = 0, rechecks = 0, copies = 0, games = 0, undone = 0;
} zg_st;

// ---- the fight's phase ----
// gBattle.Game.fight_st and gBattle.System._IsPause. The two statics' slots and the two fields' offsets are asked once, when a
// fight's HUD first appears (the battle exists then); after that: memory reads, each frame the HUD is drawn.
API::Field* zf_game = nullptr;
API::Field* zf_sys = nullptr;
API::Field* zf_st = nullptr;
API::Field* zf_pause = nullptr;
const uintptr_t* zg_game_slot = nullptr;
const uintptr_t* zg_sys_slot = nullptr;
uintptr_t zg_st_off = 0, zg_pause_off = 0;
bool zg_fight_ok = false, zg_quiet = false;
uint16_t zg_phase = zen::kNoFight;
void zen_fight_resolve() {
    if (zg_fight_ok || !zf_game || !zf_sys || !zf_st || !zf_pause) return;
    auto* gs = reinterpret_cast<const uintptr_t*>(zf_game->get_data_raw(nullptr, false));
    auto* ss = reinterpret_cast<const uintptr_t*>(zf_sys->get_data_raw(nullptr, false));
    if (!gs || !ss || !*gs || !*ss) return;
    const uintptr_t a = reinterpret_cast<uintptr_t>(zf_st->get_data_raw(reinterpret_cast<void*>(*gs), false));
    const uintptr_t b = reinterpret_cast<uintptr_t>(zf_pause->get_data_raw(reinterpret_cast<void*>(*ss), false));
    if (a < *gs || a - *gs > 0x1000 || b < *ss || b - *ss > 0x1000) return;
    zg_game_slot = gs;
    zg_sys_slot = ss;
    zg_st_off = a - *gs;
    zg_pause_off = b - *ss;
    zg_fight_ok = true;
    if (zg_debug) logi("profile_mod: Zen Mode: the fight's state at +%u, the pause at +%u", static_cast<unsigned>(zg_st_off), static_cast<unsigned>(zg_pause_off));
}
void zen_phase(uint32_t f) {
    uint16_t ph = zen::kNoFight;
    if (zg_fight_ok && f - zg_hud_seen.load(std::memory_order_relaxed) <= zen::kGone + 1) {
        const uintptr_t g = *zg_game_slot, s = *zg_sys_slot;
        if (g && s) ph = zen::phase_of(*reinterpret_cast<const uint8_t*>(g + zg_st_off), *reinterpret_cast<const uint8_t*>(s + zg_pause_off) != 0);
    }
    zg_quiet = zen::quiet_phase(ph);
    if (ph == zg_phase) return;
    zg_phase = ph;
    ++zg_st.phases;
    for (ZState& st : zg_states) zen::sched_phase(st.sched, f, zg_quiet);
    zg_next_due = f;
}

Obj* zinv(Method* m, Obj* o) {
    if (!m || !o) return nullptr;
    auto r = m->invoke(o, std::span<void*>{});
    return r.exception_thrown ? nullptr : reinterpret_cast<Obj*>(r.ptr);
}
// A control name (ASCII): a managed string into a buffer, nothing allocated
bool zstr(Obj* s, char* buf, size_t cap) {
    buf[0] = 0;
    if (!s) return false;
    const int32_t n = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(s) + 0x10);
    if (n < 0 || n > 4096) return false;
    auto* w = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(s) + 0x14);
    size_t k = 0;
    for (int32_t i = 0; i < n && k + 1 < cap; ++i) buf[k++] = w[i] < 0x80 ? static_cast<char>(w[i]) : '?';
    buf[k] = 0;
    return true;
}
ZType ztype(Obj* o) {
    TD* td = o->get_type_definition();
    for (const ZType& t : zg_types) if (t.td == td) return t;
    zg_types.push_back({ td, method_of(td, "get_Child", 0), method_of(td, "get_Next", 0), method_of(td, "get_Name", 0) });
    return zg_types.back();
}
struct ZTree {
    Obj* child(Obj* n) { return zinv(ztype(n).child, n); }
    Obj* next(Obj* n) { return zinv(ztype(n).next, n); }
    bool name(Obj* n, char* buf, size_t cap) { ++zg_st.names; return zstr(zinv(ztype(n).name, n), buf, cap); }
};
// Still that element of that GameObject
bool zen_same(uintptr_t elem, uintptr_t owner) {
    return elem && owner && reinterpret_cast<Obj*>(elem)->is_managed_object() && *reinterpret_cast<const uintptr_t*>(elem + 0x10) == owner;
}
// ---- what the game writes over at every frame ----
// The custom room puts its rank icons back at every frame (seen on 11/11: 21 nodes written again at each look, the whole tree
// of 340 names walked each time, 0.15 ms a frame). Between two walks only the nodes the last walk found are put back. A
// reference is held on each until the next walk, so a node the game drops meanwhile is still an object; nothing is kept
// longer than 16 frames.
void zen_unkeep(uintptr_t elem) {
    size_t n = 0;
    for (size_t i = 0; i < zg_kept.size(); ++i) {
        if (zg_kept[i].elem == elem) reinterpret_cast<Obj*>(zg_kept[i].ctrl)->release();
        else zg_kept[n++] = zg_kept[i];
    }
    zg_kept.resize(n);
}
void zen_kept_purge(uint32_t f) {
    size_t n = 0;
    for (size_t i = 0; i < zg_kept.size(); ++i) {
        if (f - zg_kept[i].at > 2 * zen::kMenuEvery) reinterpret_cast<Obj*>(zg_kept[i].ctrl)->release();
        else zg_kept[n++] = zg_kept[i];
    }
    zg_kept.resize(n);
}
// ---- whose ForceInvisible it is (zen::decide) ----
// One ruled control of an element on screen: read, written if it has to be, and what it is from now on kept in zg_marks
void zen_drive(const ZState& st, uintptr_t ctrl, uint16_t rule, bool hide, bool copy, bool* wrote) {
    auto at = std::lower_bound(zg_marks.begin(), zg_marks.end(), ctrl, [](const ZMark& m, uintptr_t x) { return m.ctrl < x; });
    const bool have = at != zg_marks.end() && at->ctrl == ctrl;
    if (!hide && !have && !copy) return;                       // shown, or the game's: not ours to read
    Obj* c = reinterpret_cast<Obj*>(ctrl);
    auto r = zm_fi->invoke(c, std::span<void*>{});
    if (r.exception_thrown) return;
    const zen::Act a = zen::decide(have ? at->mark : zen::Mark::None, r.byte != 0, hide, copy);
    // Hidden by us, shown again by the game while nothing is looked at (the fight itself): it was on screen until this look
    if (zg_debug && zg_quiet && have && at->mark == zen::Mark::Ours && a.write && a.value && ++zg_st.undone <= 100)
        logi("profile_mod: Zen Mode: shown again by the game during the fight: %s, rule %u", zen::kOwners[st.idx].name, static_cast<unsigned>(rule));
    if (a.write) {
        if (g_core->ui_set_force_invisible(g_client, c, a.value ? 1 : 0) != SF6CORE_OK) return;
        *wrote = true;
        ++zg_st.writes;
    }
    if (have) {
        if (a.mark == zen::Mark::None) { c->release(); zg_marks.erase(at); }
        else { at->mark = a.mark; at->elem = st.elem; at->rule = rule; at->seen = zg_f; }
        return;
    }
    if (a.mark == zen::Mark::None) return;
    c->add_ref();
    zg_marks.insert(at, { ctrl, st.elem, zg_f, rule, a.mark });
    if (!zg_debug || a.write) return;
    uint64_t& n = a.mark == zen::Mark::Game ? zg_st.games : zg_st.copies;
    if (++n <= 200) logi("profile_mod: Zen Mode: found hidden, %s: %s, rule %u", a.mark == zen::Mark::Game ? "the game's" : "a copy of ours", zen::kOwners[st.idx].name, static_cast<unsigned>(rule));
}
// The marks of an element: those of controls no longer in its tree (targets: the walk's, sorted) dropped, and the rules under
// which it has controls hidden by us, by the game. all: every one of them dropped
void zen_marks_of(uintptr_t elem, bool all, bool prune, uint32_t* ours, uint32_t* game) {
    size_t n = 0;
    for (size_t i = 0; i < zg_marks.size(); ++i) {
        const ZMark& m = zg_marks[i];
        if (m.elem == elem) {
            const bool in_tree = !all && std::binary_search(zg_targets.begin(), zg_targets.end(), ZTarget{ m.ctrl, 0, false, false },
                                                            [](const ZTarget& a, const ZTarget& b) { return a.ctrl < b.ctrl; });
            if (in_tree && m.rule < 32) *(m.mark == zen::Mark::Ours ? ours : game) |= 1u << m.rule;
            if (all || (prune && !in_tree)) { reinterpret_cast<Obj*>(m.ctrl)->release(); continue; }
        }
        zg_marks[n++] = m;
    }
    zg_marks.resize(n);
}
// Now and then: the marks of controls the game dropped (ours is the last reference) let go
void zen_marks_purge() {
    size_t n = 0;
    for (size_t i = 0; i < zg_marks.size(); ++i) {
        Obj* c = reinterpret_cast<Obj*>(zg_marks[i].ctrl);
        if (c->get_ref_count() <= 1) c->release();
        else zg_marks[n++] = zg_marks[i];
    }
    zg_marks.resize(n);
}

// false: nothing kept for that element, to walk
bool zen_recheck(const ZState& st, bool* wrote) {
    bool any = false;
    for (size_t i = 0; i < zg_kept.size(); ++i) {
        const ZKeep k = zg_kept[i];
        if (k.elem != st.elem) continue;
        any = true;
        zen_drive(st, k.ctrl, k.rule, true, false, wrote);
    }
    if (any) ++zg_st.rechecks;
    return any;
}

// The rules of the table's owner `st.idx` on that element's view. false: no longer that element
bool zen_apply(ZState& st, bool* wrote) {
    if (!zen_same(st.elem, st.owner)) return false;
    const zen::Owner& o = zen::kOwners[st.idx];
    if (st.whole) {                                           // the whole element: not drawn, nothing written to the GUI
        const bool hide = zen::wants_hidden(zg_set, o.rules[0].cat);
        ZLock lock(&zg_lock);
        ZSlot* s = zslot(st.elem, false);
        if (s && s->owner == st.owner && (s->veto != 0) != hide) { s->veto = hide ? 1 : 0; *wrote = true; ++zg_st.writes; }
        ++zg_st.applies;
        return true;
    }
    const uint32_t f = zg_f;
    if (!zen::walk_due(st.sched, f, st.walked) && zen_recheck(st, wrote)) return true;
    zen_unkeep(st.elem);
    st.walked = f;
    bool any_hide = false, marked = false;
    for (int i = 0; i < o.n; ++i) any_hide = any_hide || zen::wants_hidden(zg_set, o.rules[i].cat);
    if (!any_hide) {                                          // nothing to hide here, nothing of ours left on it: not ours to walk
        bool ours_left = false;
        for (const ZMark& m : zg_marks) if (m.elem == st.elem) { marked = true; ours_left = ours_left || m.mark == zen::Mark::Ours; }
        if (!ours_left) {
            if (marked) { zg_targets.clear(); uint32_t a = 0, b = 0; zen_marks_of(st.elem, true, false, &a, &b); }
            ++zg_st.applies;
            return true;
        }
    }
    Obj* view = zinv(zm_view, reinterpret_cast<Obj*>(st.elem));
    if (!view) return true;
    zg_targets.clear();
    zg_merged.clear();
    ZTree tree;
    int budget = static_cast<int>(kZNodes);
    auto sink = [&](Obj* n, int rule) {
        const int cat = o.rules[rule].cat;
        zg_targets.push_back({ reinterpret_cast<uintptr_t>(n), static_cast<uint16_t>(rule), zen::wants_hidden(zg_set, cat), false });
    };
    zen::walk(tree, view, o, zen::all_rules(o), 0, sink, budget);
    if (budget <= 0) ++zg_st.nodes_cut;
    ++zg_st.applies;
    std::sort(zg_targets.begin(), zg_targets.end(), [](const ZTarget& a, const ZTarget& b) { return a.ctrl < b.ctrl; });
    // What was known before this look: the rules under which this element has controls hidden by us, by the game
    uint32_t ours = 0, game = 0;
    zen_marks_of(st.elem, false, budget > 0, &ours, &game);
    for (size_t i = 0; i < zg_targets.size();) {
        ZTarget t = zg_targets[i];
        size_t j = i;
        for (; j < zg_targets.size() && zg_targets[j].ctrl == t.ctrl; ++j) {
            t.hide = t.hide || zg_targets[j].hide;
            t.copy = t.copy || zen::copy_rule(ours, game, zg_targets[j].rule);
        }
        i = j;
        zen_drive(st, t.ctrl, t.rule, t.hide, t.copy, wrote);
        if (t.hide) zg_merged.push_back(t);
    }
    if (*wrote && !zen::walk_due(st.sched, f + 1, f)) {       // undone by the game, and looked at again at the next frame
        zg_kept.reserve(zg_kept.size() + zg_merged.size());
        for (const ZTarget& t : zg_merged) {
            reinterpret_cast<Obj*>(t.ctrl)->add_ref();
            zg_kept.push_back({ st.elem, t.ctrl, f, t.rule });
        }
    }
    return true;
}

std::string zen_read(const char* path, bool* ok) {
    *ok = false;
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    std::string text(65536, '\0');
    text.resize(fread(text.data(), 1, text.size(), f));
    fclose(f);
    *ok = true;
    return text;
}
FILETIME zen_file_time() {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    return GetFileAttributesExA(kZenFile, GetFileExInfoStandard, &d) ? d.ftLastWriteTime : FILETIME{};
}
void zen_save() {
    const std::string text = zen::state_json(zg_set);
    FILE* f = fopen(kZenTmp, "wb");
    if (!f) return;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (ok) MoveFileExA(kZenTmp, kZenFile, MOVEFILE_REPLACE_EXISTING);
    else DeleteFileA(kZenTmp);
    zg_file_time = zen_file_time();
}
// The settings from now on (the game thread, or before the callbacks exist)
void zen_use(const zen::State& s, bool save) {
    zg_set = s;
    bool any = false;
    for (int c = 0; c < zen::kCatCount; ++c)
        if (zen::wants_hidden(s, c)) any = true;
    if (zg_on) {
        if (any && !zg_active.load(std::memory_order_relaxed)) {
            {
                ZLock lock(&zg_lock);
                memset(zg_slots, 0, sizeof zg_slots);
                zg_slot_n = 0;
                zg_event_n = 0;
                zg_event_flag.store(0, std::memory_order_relaxed);
            }
            zg_states.clear();
            zg_upkeep_at = zg_f + kZUpkeepEvery;
            zg_active.store(true, std::memory_order_release);
        }
        // What is on screen now was drawn before: nothing of it is held back; what appears after is
        zg_hold.store(false, std::memory_order_relaxed);
        zg_hold_at = any ? zg_f + kZHoldAfter : 0;
        for (ZState& st : zg_states) { zen::sched_now(st.sched, zg_f); st.walked = 0; }      // the settings changed: every tree walked again
        zg_next_due = zg_f;
    }
    if (save) zen_save();
}
const zen::State& zen_settings() { return zg_set; }

void zen_events(uint32_t f) {
    ZEvent ev[kZEvents];
    uint32_t n = 0;
    {
        ZLock lock(&zg_lock);
        n = zg_event_n;
        memcpy(ev, zg_events, n * sizeof(ZEvent));
        zg_event_n = 0;
        zg_event_flag.store(0, std::memory_order_relaxed);
    }
    zg_st.events += n;
    for (uint32_t i = 0; i < n; ++i) {
        char name[96];
        name[0] = 0;
        int idx = -1;
        if (zen_same(ev[i].elem, ev[i].owner) && zstr(zinv(zm_go_name, reinterpret_cast<Obj*>(ev[i].owner)), name, sizeof name))
            idx = zen::owner_index(name);
        auto it = std::find_if(zg_states.begin(), zg_states.end(), [&](const ZState& s) { return s.elem == ev[i].elem; });
        if (idx < 0) {
            if (it != zg_states.end()) zg_states.erase(it);
            zg_done.push_back({ ev[i].elem, ev[i].owner, 1, 0 });
            continue;
        }
        if (it == zg_states.end()) { zg_states.emplace_back(); it = zg_states.end() - 1; }
        it->elem = ev[i].elem;
        it->owner = ev[i].owner;
        it->idx = idx;
        it->battle = zen::battle_owner(name);
        it->whole = zen::whole_owner(zen::kOwners[idx]);
        zen::sched_appear(it->sched, f);
        if (it->battle) zen_fight_resolve();
        zg_done.push_back({ ev[i].elem, ev[i].owner, 2, static_cast<uint8_t>(it->battle ? 1 : 0) });
    }
}
void zen_due(uint32_t f) {
    uint32_t next = zg_quiet ? zen::kNever : f + kZUpkeepEvery;
    for (size_t i = 0; i < zg_states.size();) {
        ZState& st = zg_states[i];
        if (!st.sched.dormant && st.sched.due <= f) {
            bool have = false;
            uint32_t seen = 0;
            {
                ZLock lock(&zg_lock);
                if (const ZSlot* s = zslot(st.elem, false)) { have = s->owner == st.owner; seen = s->seen; }
            }
            bool keep = have;
            if (have) {
                if (f - seen > zen::kGone) zen::sched_gone(st.sched);
                else {
                    bool wrote = false;
                    keep = zen_apply(st, &wrote);
                    if (keep) zen::sched_applied(st.sched, f, st.battle, wrote, st.whole, zg_quiet);
                }
            }
            if (!keep) { zg_states.erase(zg_states.begin() + static_cast<ptrdiff_t>(i)); continue; }
        }
        if (!st.sched.dormant && st.sched.due < next) next = st.sched.due;
        ++i;
    }
    zg_next_due = next;
}
// The answers to what the observer handed over: after the rules were applied, so that a held element is drawn hidden
void zen_answer() {
    if (zg_done.empty()) return;
    ZLock lock(&zg_lock);
    for (const ZDone& d : zg_done)
        if (ZSlot* s = zslot(d.elem, false)) {
            s->queued = 0;
            if (s->owner == d.owner) { s->kind = d.kind; s->battle = d.battle; s->hold = 0; }
        }
    zg_done.clear();
}
// Now and then: the table emptied of what is no longer drawn
void zen_upkeep(uint32_t f) {
    zg_upkeep_at = f + kZUpkeepEvery;
    zen_marks_purge();
    {
        ZLock lock(&zg_lock);
        if (zg_slot_n > kZSlots / 4) {
            memcpy(zg_slots_tmp, zg_slots, sizeof zg_slots);
            // What was drawn in the last half minute stays; too many of those: in the last second
            for (const uint32_t age : { kZForget, 60u }) {
                memset(zg_slots, 0, sizeof zg_slots);
                zg_slot_n = 0;
                bool full = false;
                for (const ZSlot& s : zg_slots_tmp) {
                    if (!s.elem || f - s.seen > age) continue;
                    if (zg_slot_n >= kZSlots / 2) { full = true; break; }
                    if (ZSlot* d = zslot(s.elem, true)) *d = s;
                }
                if (!full) break;
            }
        }
    }
    for (size_t i = 0; i < zg_states.size();) {
        bool have = false;
        {
            ZLock lock(&zg_lock);
            if (const ZSlot* s = zslot(zg_states[i].elem, false)) have = s->owner == zg_states[i].owner;
        }
        if (have) ++i; else zg_states.erase(zg_states.begin() + static_cast<ptrdiff_t>(i));
    }
}
void zen_stats_write(uint32_t f) {
    LARGE_INTEGER hz{};
    QueryPerformanceFrequency(&hz);
    const double us = 1e6 / static_cast<double>(hz.QuadPart);
    uint32_t slots = 0, drawn = 0;
    {
        ZLock lock(&zg_lock);
        slots = zg_slot_n;
        for (const ZSlot& s : zg_slots) if (s.elem && f - s.seen <= zen::kGone) ++drawn;
    }
    std::string owners;
    for (const ZState& st : zg_states) {
        if (st.sched.dormant) continue;
        if (!owners.empty()) owners += ", ";
        owners += std::string("\"") + zen::kOwners[st.idx].name + "\"";
    }
    char buf[1024];
    snprintf(buf, sizeof buf,
        "{\"frame\": %u, \"enabled\": %s, \"active\": %s, \"hold\": %s, \"slots\": %u, \"drawn\": %u, \"states\": %zu,\n"
        " \"work_frames\": %llu, \"work_us\": %.1f, \"work_max_us\": %.1f, \"applies\": %llu, \"names\": %llu, \"writes\": %llu,\n"
        " \"rechecks\": %llu, \"kept\": %u, \"marks\": %u, \"ours\": %u, \"copies\": %llu, \"games\": %llu, \"undone\": %llu, \"phase\": %u, \"quiet\": %s, \"phases\": %llu, \"events\": %llu, \"cut\": %llu, \"draws\": %llu, \"draw_us\": %.1f, \"held_draws\": %llu,\n \"on_screen\": [",
        f, zg_set.enabled ? "true" : "false", zg_active.load() ? "true" : "false", zg_hold.load() ? "true" : "false", slots, drawn,
        zg_states.size(), zg_st.work, static_cast<double>(zg_st.qpc) * us, static_cast<double>(zg_st.qpc_max) * us, zg_st.applies,
        zg_st.names, zg_st.writes, zg_st.rechecks, static_cast<unsigned>(zg_kept.size()), static_cast<unsigned>(zg_marks.size()),
        static_cast<unsigned>(std::count_if(zg_marks.begin(), zg_marks.end(), [](const ZMark& m) { return m.mark == zen::Mark::Ours; })), zg_st.copies, zg_st.games, zg_st.undone, static_cast<unsigned>(zg_phase), zg_quiet ? "true" : "false", zg_st.phases, zg_st.events, zg_st.nodes_cut, zg_draws.load(), static_cast<double>(zg_draw_qpc.load()) * us,
        zg_held_draws.load());
    FILE* out = fopen(kZenStats, "wb");
    if (!out) return;
    fputs(buf, out);
    fputs(owners.c_str(), out);
    fputs("]}\n", out);
    fclose(out);
}
// Tests: the file changed by hand while the game runs
void zen_debug_reload() {
    const FILETIME t = zen_file_time();
    if (!CompareFileTime(&t, &zg_file_time)) return;
    zg_file_time = t;
    bool ok = false;
    const std::string text = zen_read(kZenFile, &ok);
    zen::State s;
    if (ok && zen::parse_state(text, s) && !(s == zg_set)) { zen_use(s, false); logi("profile_mod: Zen Mode's file read again"); }
}

void zen_tab_late(uint32_t f);                 // zen_tab.inl
void zen_tab_init(const REFrameworkPluginInitializeParam* param);

void zen_late() {
    const uint32_t f = ++zg_f;
    zg_frame.store(f, std::memory_order_relaxed);
    zen_tab_late(f);
    if (zg_debug && f % 60 == 0) { try { zen_debug_reload(); if (f % 300 == 0) zen_stats_write(f); } catch (...) {} }
    if (!zg_kept.empty() && (f & 7) == 0) { try { zen_kept_purge(f); } catch (...) {} }
    if (!zg_active.load(std::memory_order_relaxed)) return;
    zen_phase(f);
    if (!zg_event_flag.load(std::memory_order_acquire) && f < zg_next_due && (zg_quiet || f < zg_upkeep_at) && !zg_hold_at) return;
    LARGE_INTEGER t0{};
    if (zg_debug) QueryPerformanceCounter(&t0);
    try {
        if (zg_hold_at && f >= zg_hold_at) { zg_hold_at = 0; zg_hold.store(zg_set.enabled, std::memory_order_relaxed); }
        if (zg_event_flag.load(std::memory_order_acquire)) zen_events(f);
        if (f >= zg_next_due) zen_due(f);
        zen_answer();
        if (f >= zg_upkeep_at && !zg_quiet) zen_upkeep(f);        // the tidying waits for the fight to pause or end
    } catch (...) {
        static bool once = false;
        if (!once) { once = true; logi("profile_mod: an exception in Zen Mode: what it knew of the screen dropped"); }
        zg_states.clear();
        zg_next_due = f + 1;
        try { zen_answer(); } catch (...) { zg_done.clear(); }
    }
    if (zg_debug) {
        LARGE_INTEGER t1{};
        QueryPerformanceCounter(&t1);
        const uint64_t d = static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        ++zg_st.work;
        zg_st.qpc += d;
        if (d > zg_st.qpc_max) zg_st.qpc_max = d;
    }
}

// From reframework_plugin_initialize, the core registered. false: no Zen Mode (said in the log)
bool zen_init(const REFrameworkPluginInitializeParam* param) {
    const bool has = param->version && (param->version->major > 1 || (param->version->major == 1 && param->version->minor >= 14));
    auto* fns = reinterpret_cast<const ZPluginFunctions114*>(param->functions);
    if (!has || !fns->on_pre_gui_draw_element) { logi("profile_mod: Zen Mode off (needs a REFramework with the plugin API 1.14 or newer)"); return false; }
    if (!SF6CORE_HAS(g_core, ui_set_force_invisible)) { logi("profile_mod: Zen Mode off (needs a newer SF6 Native Core)"); return false; }
    auto* tdb = API::get()->tdb();
    zm_go_name = method_of(tdb->find_type("via.GameObject"), "get_Name", 0);
    zm_view = method_of(tdb->find_type("via.gui.GUI"), "get_View", 0);
    zm_fi = method_of(tdb->find_type("via.gui.PlayObject"), "get_ForceInvisible", 0);
    if (!zm_go_name || !zm_view || !zm_fi) { logi("profile_mod: Zen Mode off (the GUI's methods not found: game update?)"); return false; }
    zg_debug = GetFileAttributesA(kZenDebug) != INVALID_FILE_ATTRIBUTES;
    zf_game = tdb->find_field("gBattle", "Game");
    zf_sys = tdb->find_field("gBattle", "System");
    zf_st = tdb->find_field("nBattle.sGame", "fight_st");
    zf_pause = tdb->find_field("app.BattleSystem", "_IsPause");
    if (!zf_game || !zf_sys || !zf_st || !zf_pause) logi("profile_mod: Zen Mode: the fight's state not found (game update?): the HUD looked at every 5 s during a fight");

    bool ok = false;
    std::string text = zen_read(kZenFile, &ok);
    zen::State s;
    bool first = !ok;
    if (ok && !zen::parse_state(text, s)) { s = zen::State(); logi("profile_mod: Zen Mode's file not understood: off, everything hidden once on"); }
    if (first) {
        bool legacy = false;
        const std::string old = zen_read(kZenLegacy, &legacy);
        s = zen::first_state(legacy, old);
        if (legacy && s.enabled) logi("profile_mod: Zen Mode's settings taken from ZenMode.lua's file");
    }
    if (GetFileAttributesA(kZenLua) != INVALID_FILE_ATTRIBUTES)
        logi("profile_mod: reframework\\autorun\\ZenMode.lua is still there: it does the same work every frame, remove it");

    zg_types.reserve(32);
    zg_states.reserve(64);
    zg_targets.reserve(512);
    zg_done.reserve(kZEvents);
    if (!fns->on_pre_gui_draw_element(zen_on_draw)) { logi("profile_mod: Zen Mode off (REFramework refused the draw observer)"); return false; }
    zg_on = true;
    zen_use(s, first);
    zg_file_time = zen_file_time();
    param->functions->on_pre_application_entry("LateUpdateBehavior", zen_late);
    int hidden = 0;
    for (bool h : zg_set.hidden) hidden += h ? 1 : 0;
    zen_tab_init(param);
    logi("profile_mod: Zen Mode %s (%d of %d categories to hide)%s", zg_set.enabled ? "on" : "off", hidden, zen::kCatCount, zg_debug ? ", test counters on" : "");
    return true;
}
