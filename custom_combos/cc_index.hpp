// cc_index.hpp - Custom Combos: the trial manager's index made real for rows past the game's own count (rule 26), and
// the two displays that look a trial up by first match (rule 28). Included by custom_combos.cpp inside its anonymous
// namespace, after the recorder and the sub-menu. The offline part is cc_trials.hpp's cc::rows.
//
// A row past the game's count is a MemberwiseClone of an official entry: same StageID (the stage asset it borrows),
// same UniqueID (a hash of the user data's name). The game never keeps the row chosen: ComboTrialManager.GetStageIndex
// answers the FIRST entry with that StageID. Read in the game's code, two callers only, nothing else writes the index:
//   * UI11414.Param.EventDecideComboTrialItem(ItemDataInfo): a row decided in the trial list;
//   * UIFlowESportsPauseMenu.Param.OnEnd(): Change Trial, as the pause menu closes; it first compares the UniqueIDs
//     of the chosen and current entries and does nothing when they are equal.
// (Giving each copy a user data of its own would make the UniqueIDs differ, but GetStageIndex goes by StageID, which the
// copy must keep to load its stage: these hooks are needed either way.)
// So: both callers say which entry they are about to ask for (its row is noted when the array is built, row_map), and
// GetStageIndex's answer becomes that row when the game's own answer is the entry it was copied from. For Change
// Trial, when both rows sit on one official entry, the manager stands on a neighbour row for the length of the call and
// is put back if the game changed nothing. ChangeTrial / SetStageIndex / SetLevel are folded bodies: never hooked.
//
// Displays: the recipe's header (UI11439.Param.UpdateContents writes "<level> <_TrialDataIndex + 1>", the index found
// by first match over the official entry it is given: put on the manager's index on the way in), and Change Trial's
// spin (UIFlowESportsPauseMenu.Param.ResetParam_ComboTrial puts it on the first trial with the current UniqueID: put
// on the row under way right after, when it stands on that row's official original).
//
// These hooks run on the menus' threads (rule 17): field reads, arrays read from memory once their layout was checked
// on a list of ours, plain field writes, and the menu's own getters the reference calls there (GetCurrentSpinData,
// GetSpinFocusIndex, SetSpinFocusIndex). No lock of the mode; the row maps under idx_mx (map reads only). Nothing else
// of the suite hooks these methods. All pass through outside the custom section.
#pragma once

namespace index_ui {

thread_local struct Want { bool on = false; int row = 0, size = 0; uint32_t id = 0; } tl_want;
thread_local uint32_t tl_asked = 0;
thread_local bool tl_done = false;             // GetStageIndex answered our row in this call
thread_local struct Moved { bool on = false; int from = 0, to = 0; } tl_moved;
thread_local Obj* tl_fill = nullptr;
thread_local bool tl_fill_busy = false;

// an entry's row; `ours`: only while its list is a list of ours, with that row in it
std::optional<RowOf> row_of(Obj* entry, bool ours) {
    if (!entry) return std::nullopt;
    std::lock_guard lk(idx_mx);
    auto it = row_map.find(entry);
    if (it == row_map.end()) return std::nullopt;
    if (ours) {
        auto n = list_n.find(it->second.list);
        if (!custom_mode.load() || n == list_n.end() || n->second <= it->second.row) return std::nullopt;
    }
    return it->second;
}
int list_rows(Obj* list) { std::lock_guard lk(idx_mx); auto n = list_n.find(list); return n == list_n.end() ? 0 : n->second; }
Obj* mgr() { Obj* m = g_ctm.load(); return m && live(m) ? m : nullptr; }
int i32(Obj* o, const char* f) { return static_cast<int>(inum(o, f, -1)); }
// The control type: the game reads and writes the InputType fields as ONE byte (movsx / mov byte): compare that byte
int in8(Obj* o, const char* f) { return static_cast<int>(inum(o, f, -1) & 0xFF); }

// ---- GetStageIndex(UInt32): the answer for the entry the caller named
int pre_index(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_asked = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(argv[2]) & 0xFFFFFFFF);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_index(void** ret, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (!tl_want.on || !ret) return;
    const Want w = tl_want;
    tl_want.on = false;
    const int64_t got = static_cast<int32_t>(reinterpret_cast<uintptr_t>(*ret) & 0xFFFFFFFF);
    const int64_t ans = rows::stage_index(got, tl_asked, w.id, w.row, w.size);
    if (ans != got) { *ret = reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<uint32_t>(ans))); tl_done = true; }
}

// ---- EventDecideComboTrialItem(ItemDataInfo): a row of the list decided
int pre_list_decide(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_want = Want{};
    tl_done = false;
    if (!custom_mode.load()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try {
        Obj* entry = get_obj(reinterpret_cast<Obj*>(argv[2]), "<BattleFGComboTrialData>k__BackingField");
        if (auto r = row_of(entry, true)) {
            tl_want = Want{ true, r->row, r->size, static_cast<uint32_t>(inum(entry, "StageID", 0)) };
            g_decided = true;
        }
    } catch (...) { tl_want = Want{}; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_list_decide(void**, REFrameworkTypeDefinitionHandle, unsigned long long) { tl_want.on = false; }

// ---- the pause menu's Change Trial row: its data, its trials, how many (fields and arrays read from memory)
struct Spin { Obj* dx = nullptr; Obj* arr = nullptr; int n = 0; };
Spin trial_spin(Obj* param) {
    Spin s;
    if (!raw_ok.load()) return s;
    Obj* list = get_obj(param, "<PauseMenuDataExList>k__BackingField");
    Obj* items = get_obj(list, "_items");
    for (int32_t i = 0, n = static_cast<int32_t>(inum(list, "_size", 0)); items && i < n; ++i) {
        Obj* dx = arr_get_raw(items, i);
        Obj* sl = get_obj(dx, "<SpinDataList>k__BackingField");
        const int sn = static_cast<int>(inum(sl, "_size", 0));
        Obj* arr_ = sn > 0 ? get_obj(sl, "_items") : nullptr;
        Obj* first = arr_ ? arr_get_raw(arr_, 0) : nullptr;
        if (first && get_obj(first, "<ComboTrialData>k__BackingField")) return Spin{ dx, arr_, sn };
    }
    return s;
}

// ---- OnEnd(): Change Trial applied as the menu closes
int pre_pause_end(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_want = Want{};
    tl_moved = Moved{};
    tl_done = false;
    if (!custom_mode.load()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try {
        Obj* m = mgr();
        const Spin sp = m ? trial_spin(reinterpret_cast<Obj*>(argv[1])) : Spin{};
        Obj* sd = sp.dx ? call_obj(sp.dx, "GetCurrentSpinData") : nullptr;   // (the menu's own getter, called by OnEnd next)
        Obj* entry = sd ? get_obj(sd, "<ComboTrialData>k__BackingField") : nullptr;
        const auto r = row_of(entry, true);
        if (!r) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
        const int cur = i32(m, "_StageIndex");
        const bool same_list = i32(m, "_Level") == i32(sd, "<LevelCategory>k__BackingField") &&
                               in8(m, "_InputType") == in8(sd, "<InputType>k__BackingField");
        if (same_list && cur == r->row) return REFRAMEWORK_HOOK_CALL_ORIGINAL;    // the trial under way
        tl_want = Want{ true, r->row, r->size, static_cast<uint32_t>(inum(entry, "StageID", 0)) };
        g_decided = true;
        // two rows of ours behind one official entry: one UniqueID, the game would change nothing
        if (auto nb = rows::neighbour(cur, r->row, r->size, same_list)) {
            tl_moved = Moved{ true, cur, *nb };
            set(m, "_StageIndex", *nb);
        }
    } catch (...) { tl_want = Want{}; }
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_pause_end(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    const Moved mv = tl_moved;
    const bool done = tl_done;
    tl_want = Want{};
    tl_moved = Moved{};
    if (!mv.on || done) return;
    try { if (Obj* m = mgr(); m && i32(m, "_StageIndex") == mv.to) set(m, "_StageIndex", mv.from); } catch (...) {}   // nothing changed: back
}

// ---- ResetParam_ComboTrial(): Change Trial's spin on the row under way
int pre_spin(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    tl_fill = reinterpret_cast<Obj*>(argv[1]);
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}
void post_spin(void**, REFrameworkTypeDefinitionHandle, unsigned long long) {
    Obj* param = tl_fill;
    tl_fill = nullptr;
    if (!param || tl_fill_busy || !custom_mode.load()) return;
    tl_fill_busy = true;
    try {
        Obj* m = mgr();
        const Spin sp = m ? trial_spin(param) : Spin{};
        const int at = sp.dx ? call_i32(sp.dx, "GetSpinFocusIndex").value_or(-1) : -1;
        Obj* sd = at >= 0 && at < sp.n ? arr_get_raw(sp.arr, at) : nullptr;
        const auto r = sd ? row_of(get_obj(sd, "<ComboTrialData>k__BackingField"), false) : std::nullopt;
        const int cur = m ? i32(m, "_StageIndex") : -1, lv = m ? i32(m, "_Level") : -1;
        if (r && i32(sd, "<LevelCategory>k__BackingField") == lv && in8(sd, "<InputType>k__BackingField") == in8(m, "_InputType"))
            if (auto to = rows::spin_target(at, cur, r->row, r->size, sp.n)) {
                Obj* sd2 = arr_get_raw(sp.arr, *to);
                const auto r2 = sd2 ? row_of(get_obj(sd2, "<ComboTrialData>k__BackingField"), false) : std::nullopt;
                if (r2 && r2->row == cur && r2->list == r->list && i32(sd2, "<LevelCategory>k__BackingField") == lv)
                    call_ok(sp.dx, "SetSpinFocusIndex(System.Int32)", { arg_i32(*to) });
            }
    } catch (...) {}
    tl_fill_busy = false;
}

// ---- UI11439.Param.UpdateContents(): the header's index on the row under way, before the title is written
int pre_header(int, void** argv, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    if (!custom_mode.load()) return REFRAMEWORK_HOOK_CALL_ORIGINAL;
    try {
        Obj* self = reinterpret_cast<Obj*>(argv[1]);
        const auto r = row_of(get_obj(self, "<TrialData>k__BackingField"), false);   // (the OFFICIAL entry, measured)
        Obj* m = mgr();
        if (r && m && r->size > 0) {
            const int cur = i32(m, "_StageIndex");
            Obj* own = get_obj(self, "<OwnerTrialDataList>k__BackingField");
            if (cur >= r->size && cur < list_rows(r->list) && cur % r->size == r->row % r->size && own &&
                inum(own, "LevelCategory", -2) == inum(m, "_Level", -3))
                set(self, "_TrialDataIndex", cur);
        }
    } catch (...) {}
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void install_hooks() {
    const bool idx = hook("app.training.ComboTrialManager", "GetStageIndex", 1, pre_index, post_index);
    const bool dec = hook("app.esports.UI11414.Param", "EventDecideComboTrialItem", 1, pre_list_decide, post_list_decide);
    real_index = idx && dec;
    if (!real_index.load()) { logf("index hooks not placed: rows past the game's count are told by the list's cursor"); return; }
    hook("app.esports.UIFlowESportsPauseMenu.Param", "OnEnd", 0, pre_pause_end, post_pause_end);
    hook("app.esports.UIFlowESportsPauseMenu.Param", "ResetParam_ComboTrial", -1, pre_spin, post_spin);
    hook("app.esports.UI11439.Param", "UpdateContents", -1, pre_header, post_pass);
    logf("index hooks placed: rows past the game's count run as themselves");
}

}  // namespace index_ui
