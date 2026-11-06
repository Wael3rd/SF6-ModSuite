#include "ui/manager_logic.hpp"

#include <algorithm>
#include <cstring>

#include "model/labels.hpp"

namespace ui::mgr {
namespace {

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }

}  // namespace

// ---- rows ---------------------------------------------------------------------------------------------------------------
RowText row_text(Row r, const Tr& T) {
    switch (r) {
        case kLoadout: return { T("loadout", {}), T("loadout_desc", {}) };
        case kOpen: return { T("open", {}), T("open_desc", {}) };
        case kSave: return { T("save", {}), T("save_desc", {}) };
        case kSaveAs: return { T("save_as", {}), T("save_as_desc", {}) };
        case kReset: return { T("reset", {}), T("reset_desc", {}) };
        case kErase: return { T("erase", {}), T("erase_desc", {}) };
        case kShare: return { T("share", {}), T("share_desc", {}) };
        case kDownload: return { T("download", {}), T("download_desc", {}) };
        default: return {};
    }
}

std::string page_title(const Tr& T) { return T("page", {}); }
std::string page_guide(const Tr& T) { return T("page_desc", {}); }

// ---- the model's texts at the screen's edge ---------------------------------------------------------------------------
namespace {

// the text after `prefix` when status starts with it
bool after(const std::string& status, const char* prefix, std::string* rest) {
    if (!starts_with(status, prefix)) return false;
    *rest = status.substr(std::strlen(prefix));
    return true;
}

bool all_digits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace

std::string localise_status(const std::string& status, const Tr& T) {
    static const struct { const char* en; const char* key; } kExact[] = {
        { "No file selected", "st_no_file" }, { "No character", "st_no_char" }, { "No name", "st_no_name" },
        { "No drill selected", "st_no_drill" }, { "No drill loaded", "st_no_loaded" },
        { "Alloc Failed: memory still insufficient after retry", "st_alloc_failed" },
        { "Unknown Character ID", "st_unknown_char" }, { "No TrainingManager", "st_no_tm" }, { "No RecFunc", "st_no_recfunc" },
        { "No Fighter List", "st_no_fighters" }, { "empty or invalid slot data", "st_bad_data" },
        { "invalid slot entry (bad id)", "st_bad_entry" },
    };
    for (const auto& e : kExact) if (status == e.en) return T(e.key, {});
    std::string rest;
    if (after(status, "Name already used: ", &rest)) return T("st_name_used", { rest });
    if (after(status, "Load Failed: ", &rest)) return T("st_load_failed", { rest });
    if (after(status, "Data not found for ID ", &rest) && all_digits(rest)) return T("st_no_data", { rest });
    if (after(status, "Cannot rename ", &rest)) {                      // "<file>: <the system's message>" (a file name has no ':')
        const size_t c = rest.find(": ");
        if (c != std::string::npos) return T("st_cannot_rename", { rest.substr(0, c), rest.substr(c + 2) });
    }
    if (after(status, "Import rejected (", &rest)) {                   // "<source>): <why>"
        const size_t c = rest.find("): ");
        if (c != std::string::npos) return T("st_import_rejected", { rest.substr(0, c), localise_status(rest.substr(c + 3), T) });
    }
    if (after(status, "slot ", &rest)) {
        const size_t sp = rest.find(' ');
        if (sp != std::string::npos && all_digits(rest.substr(0, sp)) && rest.substr(sp) == " has no timeline/inputs")
            return T("st_no_timeline", { rest.substr(0, sp) });
    }
    return status;
}

std::string localise_ticker(const std::string& ticker, const Tr& T) {
    static const char kSaved[] = " File Saved", kLoaded[] = " File Loaded";
    auto ends = [&](const char* w) { const size_t n = std::strlen(w); return ticker.size() > n && ticker.compare(ticker.size() - n, n, w) == 0; };
    if (ends(kSaved)) return T("file_saved", { ticker.substr(0, ticker.size() - std::strlen(kSaved)) });
    if (ends(kLoaded)) return T("file_loaded", { ticker.substr(0, ticker.size() - std::strlen(kLoaded)) });
    return ticker;
}

// ---- the loadout spin --------------------------------------------------------------------------------------------------
std::vector<std::string> spin_labels(const std::vector<std::string>& dropdown, const std::string& no_file) {
    std::vector<std::string> v = dropdown.empty() ? std::vector<std::string>{ kNoFile } : model::labels::short_list(dropdown);
    if (v.size() > kSpinCapacity) v.resize(kSpinCapacity);
    if (!v.empty() && v[0] == kNoFile) v[0] = no_file;
    return v;
}

int32_t spin_value(int dropdown_index, size_t n_labels) {
    const int32_t n = n_labels == 0 ? 1 : (int32_t)n_labels;
    return std::clamp<int32_t>(dropdown_index - 1, 0, n - 1);
}

int dropdown_of_spin(int32_t index, size_t n_labels) {
    const int32_t n = n_labels == 0 ? 1 : (int32_t)n_labels;
    return std::clamp<int32_t>(index, 0, n - 1) + 1;
}

// ---- sub-page memory ------------------------------------------------------------------------------------------------------
void PageMemory::page_shown(bool shown, bool menu_up) {
    if (shown) { page_ = Page::Manager; pending_game_ = false; return; }
    if (menu_up && page_ == Page::Manager) pending_game_ = true;    // (game_page already said which: nothing pending)
}

void PageMemory::game_page(Page p) {
    if (p != Page::Record && p != Page::Replay) return;
    page_ = p;
    pending_game_ = false;
}

void PageMemory::resolve(int64_t rt) {
    if (!pending_game_) return;
    pending_game_ = false;
    page_ = rt <= 0 ? Page::Record : Page::Replay;
}

std::optional<int64_t> PageMemory::record_type(int64_t rt) {
    resolve(rt);
    if (page_ == Page::None) return rt <= 0 ? (int64_t)Page::Record : (int64_t)Page::Replay;
    return (int64_t)page_;
}

bool record_type_needs_reset(int64_t rt, bool menu_up) { return !menu_up && rt > 1; }

bool record_type_fits(int64_t want, int32_t strip_pages) { return strip_pages >= 3 && want >= 0 && want < strip_pages; }

const char* page_name(PageMemory::Page p) {
    switch (p) {
        case PageMemory::Page::Manager: return "Manager";
        case PageMemory::Page::Record: return "Record";
        case PageMemory::Page::Replay: return "Replay";
        default: return "none";
    }
}

// ---- hints ------------------------------------------------------------------------------------------------------------------
std::vector<HintSpec> hints_for(bool own_row, bool func_known, int64_t func_type) {
    if (own_row || !func_known || func_type != kFtRecordSlot) return {};
    return { { kButtonY, "edit_name" }, { kButtonX, "delete_name" } };
}

// ---- actions ----------------------------------------------------------------------------------------------------------------
const char* act_name(Act a) {
    switch (a) {
        case Act::Select: return "select";
        case Act::Ensure: return "ensure";
        case Act::Refresh: return "refresh";
        case Act::Open: return "open";
        case Act::Save: return "save";
        case Act::SaveAs: return "save as";
        case Act::Reset: return "reset";
        case Act::Erase: return "erase";
        case Act::Share: return "share";
        case Act::Download: return "download";
        case Act::Rename: return "rename";
    }
    return "?";
}

std::optional<Act> act_of_row(int row) {
    switch (row) {
        case kOpen: return Act::Open;
        case kSave: return Act::Save;
        case kSaveAs: return Act::SaveAs;
        case kReset: return Act::Reset;
        case kErase: return Act::Erase;
        case kShare: return Act::Share;
        case kDownload: return Act::Download;
        default: return std::nullopt;
    }
}

void Queue::push(Action a) {
    std::lock_guard lk(mx_);
    if (q_.size() < 64) q_.push_back(a);                 // a stuck frame callback never grows it without bound
}

std::vector<Action> Queue::drain() {
    std::lock_guard lk(mx_);
    std::vector<Action> v(q_.begin(), q_.end());
    q_.clear();
    return v;
}

size_t Queue::size() const {
    std::lock_guard lk(mx_);
    return q_.size();
}

size_t Queue::selects() const {
    std::lock_guard lk(mx_);
    return (size_t)std::count_if(q_.begin(), q_.end(), [](const Action& a) { return a.act == Act::Select; });
}

void Runner::run(const Action& a) {
    ++runs_;
    switch (a.act) {
        case Act::Select: files_.set_dropdown_index(a.arg); break;     // set_dropdown_index(i + 1) (RMT:67): selecting loads nothing
        case Act::Ensure: files_.ensure(); screen_.list_changed(); break;   // the page built: ensure_files (RMT:2009)
        case Act::Refresh:                                              // after a Community download (RMT:276-278)
            files_.invalidate(); files_.ensure(); screen_.list_changed(); break;
        case Act::Open: open(); break;
        case Act::Save: save(); break;
        case Act::SaveAs: save_as(); break;
        case Act::Reset: reset(); break;
        case Act::Erase: erase(); break;
        case Act::Share: screen_.community_share(); break;            // RMT:397-416: the checks and tickers are the dialog's
        case Act::Download: screen_.community_open(); break;          // RMT:418-426
        case Act::Rename: rename(); break;
    }
}

void Runner::ask(const std::string& text, const std::string& title, std::function<void()> yes) {
    if (dialog_open()) return;                                         // RMT:80: one box at a time, a second ask ignored
    ++dialogs_;
    screen_.confirm(text, title, [this, yes = std::move(yes)](bool ok) {
        if (dialogs_ > 0) --dialogs_;
        if (ok) yes();                                                 // 0 = the first button (Yes) (RMT:96)
    });
}

void Runner::name_dialog(const std::string& title, const std::string& initial, std::function<void(const std::string&)> apply) {
    ++dialogs_;
    screen_.text_input(title, initial, kNameMax, [this, apply = std::move(apply)](bool ok, const std::string& text) {
        if (dialogs_ > 0) --dialogs_;
        if (ok) apply(text);                                           // cancelled / closed unchanged: nothing (RMT:1636-1643)
    });
}

// RMT:99-110: no selection -> ticker; else the import, no confirmation, the game's "File Loaded" ticker kept quiet.
void Runner::open() {
    const std::string name = files_.selected_name();
    if (name.empty()) { screen_.ticker(T_("none_selected", {})); return; }
    files_.set_native_action(true);
    const std::string r = files_.import_selected();
    screen_.check_slots();
    screen_.rows_dirty();
    screen_.ticker(starts_with(r, "Loaded") ? T_("opened", { name })
                   : contains(r, "Allocating") ? T_("opened_alloc", { name })
                   : T_("open_failed", { localise_status(r, T_) }));
}

// RMT:111-118: the selected loadout, else the character's own file.
void Runner::save() {
    std::string name = files_.selected_name();
    if (name.empty()) name = files_.char_name();
    ask(T_("confirm_save", { name }), T_("save", {}), [this, name] {   // box title rsm.save (RMT:116)
        std::string tk;
        const std::string r = files_.export_selected(&tk);
        main_ticker(tk);
        screen_.ticker(contains(r, "Saved") ? T_("saved", { name }) : T_("save_failed", { localise_status(r, T_) }));
    });
}

// RMT:119-120, 1455-1459, 1612-1619: the text dialog (selected name, else the character), then the box, then export_as.
void Runner::save_as() {
    if (dialog_open()) return;
    std::string initial = files_.selected_name();
    if (initial.empty()) initial = files_.char_name();
    name_dialog(T_("save_as", {}), initial, [this](const std::string& str) {
        if (str.empty()) return;                                       // an empty name: silently nothing
        ask(T_("confirm_save", { str }), T_("save_as", {}), [this, str] {   // box title rsm.save_as (RMT:1615)
            std::string tk;
            const std::string r = files_.export_as(str, &tk);
            main_ticker(tk);
            screen_.list_changed();
            screen_.ticker(contains(r, "Saved") ? T_("saved_as", { str }) : T_("save_failed", { localise_status(r, T_) }));
        });
    });
}

// RMT:121-134: the open loadout read again, after the box.
void Runner::reset() {
    const std::string name = files_.loaded_name();
    if (name.empty()) { screen_.ticker(T_("none_open", {})); return; }
    ask(T_("confirm_reset", { name }), T_("reset", {}), [this, name] {   // box title rsm.reset (RMT:127)
        files_.set_native_action(true);
        const std::string r = files_.reopen_loaded();
        screen_.check_slots();
        screen_.rows_dirty();
        screen_.ticker(starts_with(r, "Loaded") ? T_("reset_done", { name })
                       : contains(r, "Allocating") ? T_("reset_alloc", { name })
                       : T_("reset_failed", { localise_status(r, T_) }));
    });
}

// RMT:135-146: emptied and hidden, the list read again.
void Runner::erase() {
    const std::string name = files_.selected_name();
    if (name.empty()) { screen_.ticker(T_("none_selected", {})); return; }
    ask(T_("confirm_erase", { name }), T_("erase", {}), [this, name] {   // box title rsm.erase (RMT:141)
        const std::string r = files_.erase_selected();
        files_.invalidate();
        files_.ensure();
        screen_.list_changed();
        screen_.rows_dirty();
        screen_.ticker(starts_with(r, "Erased") ? T_("erased", { name }) : T_("erase_failed", { localise_status(r, T_) }));
    });
}

// RMT:1460-1465, 1620-1625: T on the Manager page (no hint): the selected loadout renamed; an empty name gives
// "Rename failed: nil" as in v1.6.
void Runner::rename() {
    if (dialog_open()) return;
    const std::string sel = files_.selected_name();
    if (sel.empty()) return;
    name_dialog(T_("rename", {}), sel, [this](const std::string& str) {
        std::string r = "nil", tk;
        if (!str.empty()) r = files_.rename_selected(str, &tk);
        main_ticker(tk);
        screen_.list_changed();
        screen_.ticker(starts_with(r, "Renamed") ? T_("renamed", { str }) : T_("rename_failed", { localise_status(r, T_) }));
    });
}

}  // namespace ui::mgr
