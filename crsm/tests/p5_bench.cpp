// p5_bench — offline tests of phase P5: the Manager page's game-free logic
// (src/ui/manager_logic.*), no game, no user data.
//
//   p5_bench --repo <crsm folder> --work <dir>
//
// Groups: the rows and their texts (the shipped locale.json, English and French), the loadout spin (labels, value, the
// core's wrap), the sub-page memory (RecordType), the hints, the action queue (order, threads), the actions on fakes
// (every ticker, confirmation text and dialog of RMT's Manager page), then the actions on the real files engine
// (model::files::Engine on a scratch folder under --work, a fake game): Save As, Save, Open, Reset, Rename, Erase; and the
// drill details dialog's texts in every language of the shipped locale.json (src/ui/details_logic.*).
// Exit code 0 only when every check passed.
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "model/files.hpp"
#include "model/labels.hpp"
#include "model/locale.hpp"
#include "model/luajson.hpp"
#include "ui/details_logic.hpp"
#include "ui/manager_logic.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace ui::mgr;

namespace {

// ---- reporting --------------------------------------------------------------------------------------------------------------
struct Group { int checks = 0; int fails = 0; };
std::map<std::string, Group> g_groups;
std::vector<std::string> g_order;
std::string g_group;

void begin(const std::string& name) {
    g_group = name;
    if (!g_groups.count(name)) g_order.push_back(name);
}

void check(bool ok, const std::string& what, const std::string& detail = "") {
    Group& g = g_groups[g_group];
    ++g.checks;
    if (ok) return;
    ++g.fails;
    std::printf("  FAIL [%s] %s%s%s\n", g_group.c_str(), what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
}

template <class A, class B>
void eq(const A& got, const B& want, const std::string& what) {
    std::ostringstream d;
    d << "got '" << got << "', want '" << want << "'";
    check(got == want, what, got == want ? "" : d.str());
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " | ") + x;
    return s;
}

// ---- the translator: the shipped locale.json through the P1 model -------------------------------------------------------------
model::locale::Locale g_loc;
std::vector<std::string> g_langs;                // the language blocks of the shipped file
Tr translator() {
    return [](const std::string& key, const std::vector<std::string>& args) { return g_loc.T(key, args); };
}

// ---- fakes ---------------------------------------------------------------------------------------------------------------------
struct FakeLoadouts final : Loadouts {
    std::vector<std::string> calls;
    std::string sel, chr = "Ryu", loaded;
    std::vector<std::string> list{ kNoFile };
    int index = 1;
    bool native_action = false;
    std::string import_res = "Loaded: x", reopen_res = "Loaded: x", export_res = "Saved: x", export_tk, erase_res = "Erased: x",
                rename_res = "Renamed: x", rename_tk;
    std::string selected_name() override { return sel; }
    std::string char_name() override { return chr; }
    std::string loaded_name() override { return loaded; }
    std::vector<std::string> dropdown() override { return list; }
    int dropdown_index() override { return index; }
    void set_dropdown_index(int i) override { calls.push_back("set_dropdown_index(" + std::to_string(i) + ")"); index = i; }
    void set_native_action(bool on) override { calls.push_back(on ? "native_action(on)" : "native_action(off)"); native_action = on; }
    std::string import_selected() override { calls.push_back("import_selected"); return import_res; }
    std::string reopen_loaded() override { calls.push_back("reopen_loaded"); return reopen_res; }
    std::string export_selected(std::string* tk) override { calls.push_back("export_selected"); *tk = export_tk; return export_res; }
    std::string export_as(const std::string& n, std::string* tk) override { calls.push_back("export_as(" + n + ")"); *tk = export_tk; return export_res; }
    std::string erase_selected() override { calls.push_back("erase_selected"); return erase_res; }
    std::string rename_selected(const std::string& n, std::string* tk) override { calls.push_back("rename_selected(" + n + ")"); *tk = rename_tk; return rename_res; }
    void invalidate() override { calls.push_back("invalidate"); }
    bool ensure() override { calls.push_back("ensure"); return false; }
};

// The screen: records every call; dialogs answered by the script (auto) or held until answered by the test.
struct FakeScreen final : Screen {
    std::vector<std::string> calls, tickers;
    bool auto_answer = true;
    bool confirm_answer = true;
    bool input_ok = true;
    std::string input_text;
    std::function<void(bool)> held_confirm;
    std::function<void(bool, const std::string&)> held_input;
    std::string last_confirm, last_confirm_title, last_input_title, last_input_initial;
    size_t last_max = 0;
    void ticker(const std::string& t) override { tickers.push_back(t); calls.push_back("ticker"); }
    void confirm(const std::string& text, const std::string& title, std::function<void(bool)> done) override {
        calls.push_back("confirm");
        last_confirm = text;
        last_confirm_title = title;
        if (auto_answer) done(confirm_answer); else held_confirm = std::move(done);
    }
    void text_input(const std::string& title, const std::string& initial, size_t max_bytes,
                    std::function<void(bool, const std::string&)> done) override {
        calls.push_back("text_input");
        last_input_title = title; last_input_initial = initial; last_max = max_bytes;
        if (auto_answer) done(input_ok, input_text); else held_input = std::move(done);
    }
    void community_open() override { calls.push_back("community_open"); }
    void community_share() override { calls.push_back("community_share"); }
    void check_slots() override { calls.push_back("check_slots"); }
    void rows_dirty() override { calls.push_back("rows_dirty"); }
    void list_changed() override { calls.push_back("list_changed"); }
    size_t count(const std::string& c) const { return (size_t)std::count(calls.begin(), calls.end(), c); }
};

bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }

// ---- groups ----------------------------------------------------------------------------------------------------------------------
// The drill details dialog's keys (rsm.cancel, rsm.details_*, rsm.source_*) in every language block, formatted with their
// arguments (a template string.format refuses comes back as it is: the arguments then never show)
void test_details_texts() {
    begin("P5 drill details texts (locale.json, every language)");
    const Tr T = translator();
    check(g_langs.size() >= 14, "14 languages", std::to_string(g_langs.size()));
    const char* plain[] = { "cancel", "source_community", "source_local", "source_local_short", "details_more_slots", "details_edit",
                            "details_confirm", "edit_drill_name", "details_help", "see_details", "details_saved_one", "details_md_help", "details_show_desc",
                            "details_md_newline", "details_md_heading", "details_md_emphasis", "details_md_violet", "details_md_magenta",
                            "details_md_purple", "details_md_blue", "details_md_list" };
    const char* md_words[] = { "details_md_newline", "details_md_heading", "details_md_emphasis", "details_md_violet", "details_md_magenta",
                               "details_md_purple", "details_md_blue", "details_md_list" };
    const std::string tags[] = { "<COLOR preset=\"FGM_Cmn_2\">", "<COLOR preset=\"FGM_Cmn_1\">", "<COLOR preset=\"FGM_Cmn_0\">", "</COLOR>",
                                 "<HLBGN Arg0 = \"3\">", "<HLEND Arg0 = \"3\">" };
    for (const std::string& l : g_langs) {
        g_loc.set_lang(l);
        for (const char* k : plain) {
            const std::string v = T(k, {});
            check(!v.empty() && v != k && g_loc.lookup(std::string("rsm.") + k).has_value(), l + " rsm." + k, v);
        }
        const std::string h = ui::details::header_text(T, 0, 2);
        check(h.find("1/2") != std::string::npos && h != "DETAILS_PAGE", l + " header page 1/2", h);
        check(h.find("15") == std::string::npos && h.find("SLOT") == std::string::npos, l + " header: the page only", h);
        const std::string sv = ui::details::saved_text(T, 14);
        check(sv.find("14") != std::string::npos && sv != "DETAILS_SAVED", l + " 14 slots saved", sv);
        const std::string s1 = ui::details::saved_text(T, 1);
        check(!s1.empty() && s1 != sv && s1.find('%') == std::string::npos, l + " one slot saved", s1);
        check(!g_loc.lookup("rsm.details_header").has_value() && !g_loc.lookup("rsm.details_header_one").has_value(), l + " no old header keys");
        const std::string sl = T("details_slot", { "27" });
        check(sl.find("27") != std::string::npos && sl != "details_slot", l + " slot label", sl);
        const std::string au = T("details_author", { "Chris_F" }), so = T("details_source", { "drillcodes.com" });
        check(au.find("Chris_F") != std::string::npos, l + " author line", au);
        check(so.find("drillcodes.com") != std::string::npos, l + " source line", so);
        for (const char* k : { "details_page", "details_saved", "details_slot", "details_author", "details_source", "details_md_note" })
            check(g_loc.lookup(std::string("rsm.") + k).has_value(), l + " has rsm." + k);
        eq(ui::details::short_source(T, "drillcodes.com"), std::string("DrillCodes"), l + " DrillCodes as a name");
        eq(ui::details::frame_text("Akuma", ui::details::frame_author("Chris_F (drillcodes)", "", "community"),
                                   ui::details::short_source(T, "community", "Chris_F (drillcodes)"), 0),
           std::string("Akuma - Chris_F - DrillCodes"), l + " a Community import of a drill code: DrillCodes, the author bare");
        check(T("details_show_desc", {}) != T("details_md_help", {}) && T("details_edit", {}) != T("edit_drill_name", {}),
              l + " the R entry's two labels and the two edits differ", T("details_show_desc", {}));
        const std::string ge = ui::details::guide_entries(T, false, "", false, true);
        check(ge.find(T("details_show_desc", {})) != std::string::npos && ge.find(T("details_md_help", {})) == std::string::npos
              && ge.find(T("details_edit", {})) != std::string::npos, l + " guide, the help shown: Show Drill Description", ge);
        const std::string fr = ui::details::frame_text("Ryu", "Wael3rd_PC", ui::details::short_source(T, ""), 0);
        check(fr.rfind("Ryu - Wael3rd_PC - ", 0) == 0 && fr.size() > 19, l + " top-left box line", fr);
        // the Markdown help: the translated words (as rendered), the syntax as typed, only verified tags, no [ ]
        const std::string help = ui::details::help_text(T);
        bool words = true;
        for (const char* k : md_words) if (help.find(T(k, {})) == std::string::npos) words = false;
        check(words && help.find(T("details_md_help", {})) != std::string::npos, l + " help: every translated word", help.substr(0, 80));
        bool syntax = true;
        for (const char* s : { "\n/n  =  ", "\n# Title  =  ", "\n**text**  =  ", "\n{violet}text{/}  =  ", "\n{magenta}text{/}  =  ",
                               "\n{purple}text{/}  =  ", "\n{blue}text{/}  =  ", "\n- item  =  " })
            if (help.find(s) == std::string::npos) syntax = false;
        check(syntax, l + " help: every syntax as typed");
        const std::string note = T("details_md_note", { "1000" });
        check(note.find("1000") != std::string::npos && note.find('%') == std::string::npos && help.find(note) != std::string::npos,
              l + " help: the note with its 1000", note);
        bool tags_ok = help.find('[') == std::string::npos && help.find(']') == std::string::npos;
        int spans = 0;
        for (size_t i = help.find('<'); tags_ok && i != std::string::npos; i = help.find('<', i + 1)) {
            const size_t e = help.find('>', i);
            const std::string tag = e == std::string::npos ? std::string() : help.substr(i, e - i + 1);
            if (std::find(std::begin(tags), std::end(tags), tag) == std::end(tags)) tags_ok = false;
            else if (tag[1] != '/' && tag.rfind("<HLEND", 0) != 0) ++spans;
        }
        check(tags_ok && spans == 7, l + " help: 7 spans of verified tags only, no [ ]", std::to_string(spans));
    }
    g_loc.set_lang("en");
    eq(ui::details::header_text(T, 1, 2), std::string("PAGE 2/2"), "en header");
    eq(ui::details::saved_text(T, 14), std::string("14 SLOTS SAVED"), "en saved count");
    eq(ui::details::saved_text(T, 1), std::string("1 SLOT SAVED"), "en one slot saved");
    eq(ui::details::frame_text("Ryu", "Wael3rd_PC", ui::details::short_source(T, ""), 0), std::string("Ryu - Wael3rd_PC - Local"), "en top-left box");
    eq(ui::details::short_source(T, "community"), std::string("Community"), "en Community");
    eq(T("details_md_help", {}), std::string("Markdown Help"), "en guide: the Markdown help");
    eq(ui::details::row_text(T("details_slot", { "7" }), "Fireball", 64, 0), std::string("Slot 7 - Fireball - 64F"), "en slot row");
    eq(ui::details::detail_text(T, "Do it.", "Chris_F", ui::details::source_label(T, "community")),
       std::string("Do it.\n\nAuthor: Chris_F\nSource: Community"), "en description box");
    eq(ui::details::source_label(T, ""), std::string("Local file"), "en local file");
    eq(T("open", {}), std::string("Open drill"), "en left button: the Manager's own label");
    eq(T("cancel", {}), std::string("Cancel"), "en right button");
    eq(T("details_more_slots", {}), std::string("Show More Slots"), "en guide: the slot pages");
    eq(T("details_edit", {}), std::string("Edit Drill Description"), "en guide: the edit");
    eq(T("details_show_desc", {}), std::string("Show Drill Description"), "en guide: R while the Markdown help is shown");
    eq(T("details_confirm", {}), std::string("Confirm"), "en guide: confirm");
    eq(T("edit_drill_name", {}), std::string("Edit Drill Name"), "en Drill row hint: the rename");
    eq(T("details_help", {}), std::string("Up / down: scroll the description"), "en key guide: the help line");
    eq(T("see_details", {}), std::string("See Details"), "en Drill row hint: the details");
}

void test_texts() {
    begin("P5 rows and texts (locale.json)");
    const Tr T = translator();
    g_loc.set_lang("en");
    const char* titles[] = { "Drill", "Open drill", "Save drill", "Save drill as", "Reset drill", "Erase drill",
                             "Share drill", "Download Drills" };
    const char* guides[] = { "Drill (set of recordings) of the current character. Left / right to select.",
                             "Open the selected drill into the 30 slots.", "Save the 30 slots into the selected drill.",
                             "Save the 30 slots into a new drill: type its name.", "Back to the open drill (its file read again).",
                             "Erase the selected drill.", "Share the loaded drill with the community.",
                             "Browse and download drills shared by others." };
    for (int i = 0; i < kRowCount; ++i) {
        const RowText t = row_text((Row)i, T);
        eq(t.title, std::string(titles[i]), "en title of row " + std::to_string(i + 1));
        eq(t.guide, std::string(guides[i]), "en guide of row " + std::to_string(i + 1));
    }
    eq(page_title(T), std::string("Manager"), "en page title");
    eq(page_guide(T), std::string("Recording slot manager (SF6 Tools)."), "en page guide (rsm.page_desc)");
    g_loc.set_lang("fr");
    eq(page_title(T), std::string("Gestion"), "fr page title");
    check(row_text(kOpen, T).title != "Open drill" && !row_text(kOpen, T).title.empty(), "fr Open row translated", row_text(kOpen, T).title);
    eq(row_text(kShare, T).title, std::string("Partager le drill"), "fr Share row: rsm.share");
    eq(row_text(kDownload, T).guide, std::string("Parcourir et t\xC3\xA9l\xC3\xA9" "charger les drills partag\xC3\xA9s par les autres."), "fr Download guide");
    eq(page_guide(T), T("page_desc", {}), "fr page guide through rsm.page_desc");
    check(page_guide(T) != "Recording slot manager (SF6 Tools).", "fr page guide translated", page_guide(T));
    g_loc.set_lang("es-419");                                       // the near language of es-419 is es
    check(!row_text(kSave, T).title.empty() && row_text(kSave, T).title != "save", "es-419 Save row resolved", row_text(kSave, T).title);
    g_loc.set_lang("nl");                                           // no block: English
    eq(row_text(kErase, T).title, std::string("Erase drill"), "nl falls back to English");
    g_loc.set_lang("en");
    eq(T("opened", { "Ryu_Test" }), std::string("Drill opened: Ryu_Test"), "formatted ticker");
    eq(T("no_such_key", {}), std::string("no_such_key"), "missing key: the bare key (RMT T)");
    // the button rows map to their actions, the spin to none
    check(!act_of_row(kLoadout).has_value(), "the spin has no decide");
    const Act acts[] = { Act::Open, Act::Save, Act::SaveAs, Act::Reset, Act::Erase, Act::Share, Act::Download };
    for (int i = 1; i < kRowCount; ++i) check(act_of_row(i) == acts[i - 1], "decide of row " + std::to_string(i + 1));
    check(!act_of_row(kRowCount).has_value() && !act_of_row(-1).has_value(), "no action outside the rows");
}

// localise_status / localise_ticker: the model's English strings to the rsm.st_* / rsm.file_* texts (only the en block has them
// yet: another language gives the English text, so the checks run on a Tr that marks what it was asked)
void test_localise() {
    begin("P5 the model's texts at the screen's edge");
    g_loc.set_lang("en");
    const Tr T = translator();
    eq(localise_status("No file selected", T), std::string("No file selected"), "en: No file selected");
    eq(localise_status("No character", T), std::string("No character"), "en: No character");
    eq(localise_status("No drill loaded", T), std::string("No drill loaded"), "en: No drill loaded");
    eq(localise_status("Name already used: Combo A", T), std::string("Name already used: Combo A"), "en: Name already used keeps the name");
    eq(localise_status("Load Failed: Ryu_Test.json", T), std::string("Load Failed: Ryu_Test.json"), "en: Load Failed keeps the file");
    eq(localise_status("Data not found for ID 12", T), std::string("Data not found for ID 12"), "en: Data not found");
    eq(localise_status("slot 7 has no timeline/inputs", T), std::string("slot 7 has no timeline/inputs"), "en: slot N has no timeline");
    eq(localise_status("Cannot rename a.json: Access is denied.", T), std::string("Cannot rename a.json: Access is denied."), "en: Cannot rename");
    eq(localise_status("Import rejected (x.json): invalid slot entry (bad id)", T), std::string("Import rejected (x.json): invalid slot entry (bad id)"),
       "en: Import rejected with its reason");
    // a Tr that marks every key and argument: which key, which arguments, in order
    const Tr M = [](const std::string& key, const std::vector<std::string>& args) {
        std::string s = "[" + key;
        for (const std::string& a : args) s += "|" + a;
        return s + "]";
    };
    const struct { const char* in; const char* want; } cases[] = {
        { "No file selected", "[st_no_file]" }, { "No character", "[st_no_char]" }, { "No name", "[st_no_name]" },
        { "No drill selected", "[st_no_drill]" }, { "No drill loaded", "[st_no_loaded]" },
        { "Name already used: Combo A", "[st_name_used|Combo A]" },
        { "Cannot rename F.json: The system cannot find the file", "[st_cannot_rename|F.json|The system cannot find the file]" },
        { "Load Failed: Ryu_A.json", "[st_load_failed|Ryu_A.json]" },
        { "Alloc Failed: memory still insufficient after retry", "[st_alloc_failed]" },
        { "Import rejected (Ryu_A.json): empty or invalid slot data", "[st_import_rejected|Ryu_A.json|[st_bad_data]]" },
        { "Import rejected (B (copy).json): slot 3 has no timeline/inputs", "[st_import_rejected|B (copy).json|[st_no_timeline|3]]" },
        { "Import rejected (a.json): something unexpected", "[st_import_rejected|a.json|something unexpected]" },
        { "Unknown Character ID", "[st_unknown_char]" }, { "No TrainingManager", "[st_no_tm]" }, { "No RecFunc", "[st_no_recfunc]" },
        { "No Fighter List", "[st_no_fighters]" }, { "Data not found for ID 4", "[st_no_data|4]" },
        { "empty or invalid slot data", "[st_bad_data]" }, { "invalid slot entry (bad id)", "[st_bad_entry]" },
        { "slot 30 has no timeline/inputs", "[st_no_timeline|30]" },
        // technical or unknown: unchanged
        { "Crash: attempt to index a nil value", "Crash: attempt to index a nil value" },
        { "Error: bad argument #1", "Error: bad argument #1" },
        { "Data not found for ID x", "Data not found for ID x" },
        { "slot x has no timeline/inputs", "slot x has no timeline/inputs" },
        { "Cannot rename nocolon", "Cannot rename nocolon" },
        { "nil", "nil" }, { "", "" },
    };
    for (const auto& c : cases) eq(localise_status(c.in, M), std::string(c.want), std::string("status: ") + c.in);
    eq(localise_ticker("Ryu_A File Saved", M), std::string("[file_saved|Ryu_A]"), "ticker: File Saved");
    eq(localise_ticker("My Drill File Loaded", M), std::string("[file_loaded|My Drill]"), "ticker: File Loaded");
    eq(localise_ticker("Drill opened: x", M), std::string("Drill opened: x"), "ticker: another text unchanged");
    eq(localise_ticker(" File Saved", M), std::string(" File Saved"), "ticker: no name, unchanged");
    eq(localise_ticker("Ryu_A File Saved", T), std::string("Ryu_A File Saved"), "en ticker: the same text");
    // the spin's first entry in the player's language, the comparison text kept
    eq(join(spin_labels({}, "-- Choisir --")), std::string("-- Choisir --"), "empty list: the shown no-file entry");
    eq(join(spin_labels({ kNoFile, "A" }, "-- Choisir --")), std::string("-- Choisir -- | A"), "first entry replaced, the others kept");
    eq(join(spin_labels({ "A", kNoFile })), std::string("A | -- Select File --"), "only the first entry is the no-file one");
    // every key the helpers ask for is in the en block
    for (const char* k : { "st_no_file", "st_no_char", "st_no_name", "st_no_drill", "st_no_loaded", "st_name_used", "st_cannot_rename",
                           "st_load_failed", "st_alloc_failed", "st_import_rejected", "st_unknown_char", "st_no_tm", "st_no_recfunc",
                           "st_no_fighters", "st_no_data", "st_bad_data", "st_bad_entry", "st_no_timeline", "file_saved", "file_loaded",
                           "select_file", "share", "share_desc", "download", "download_desc", "page_desc", "rename" })
        check(T(k, { "a", "b" }) != k, std::string("key rsm.") + k + " in the en block");
}

void test_spin() {
    begin("P5 drill spin");
    eq(join(spin_labels({})), std::string(kNoFile), "empty list: the no-file entry");
    eq(join(spin_labels({ kNoFile, "A", "B" })), std::string("-- Select File -- | A | B"), "labels as the dropdown");
    const std::string long_name(80, 'x');
    const auto sl = spin_labels({ kNoFile, long_name });
    eq(sl[1], model::labels::short_list({ long_name })[0], "long name cut as short_list (RMT:59-64)");
    check(sl[1].size() < long_name.size() && sl[1].find("\xE2\x80\xA6") != std::string::npos, "long name ends with the ellipsis", sl[1]);
    std::vector<std::string> many{ kNoFile };
    for (int i = 0; i < 100; ++i) many.push_back("pack" + std::to_string(i));
    const auto ml = spin_labels(many);
    eq(ml.size(), (size_t)kSpinCapacity, "at most 64 options (capacity built once)");
    eq(ml.back(), std::string("pack62"), "the 64th option");
    eq(kSpinCapacity, 64u, "capacity");
    eq(kArrowSpread, 70.0f, "arrows 70 px apart");
    // value: 1-based dropdown index -> 0-based option, clamped
    eq(spin_value(1, 3), 0, "index 1 = option 0");
    eq(spin_value(3, 3), 2, "index 3 = option 2");
    eq(spin_value(9, 3), 2, "past the end: clamped");
    eq(spin_value(0, 3), 0, "index 0: clamped");
    eq(spin_value(1, 0), 0, "no label");
    // the core wraps (_Num modulo the count): D from the last option gives option 0, Q from option 0 the last one
    const size_t n = 4;
    eq(dropdown_of_spin((int32_t)((3 + 1) % n), n), 1, "D from the last: -- Select File -- (observed in v1.6)");
    eq(dropdown_of_spin((int32_t)((0 + n - 1) % n), n), 4, "Q from the first: the last drill");
    eq(dropdown_of_spin(2, n), 3, "option 2 = dropdown index 3");
    eq(dropdown_of_spin(99, n), 4, "out of range: clamped");
    eq(dropdown_of_spin(-1, n), 1, "negative: clamped");
    // round trip over every option
    bool rt = true;
    for (int32_t i = 0; i < 64; ++i) rt = rt && spin_value(dropdown_of_spin(i, 64), 64) == i;
    check(rt, "spin_value(dropdown_of_spin(i)) == i for the 64 options");
}

void test_memory() {
    begin("P5 sub-page memory (RecordType)");
    using P = PageMemory::Page;
    PageMemory m;
    eq(*m.record_type(0), 1, "nothing built yet, the game remembers Record (0): Record = index 1, not Manager");
    eq(*m.record_type(1), 2, "nothing built yet, the game remembers Replay (1): Replay = index 2, not Record");
    m.page_shown(false, true);
    check(!m.unresolved() && m.page() == P::None, "a game page before ours was ever shown: no edge to resolve");
    m.page_shown(true, true);
    eq(*m.record_type(1), 0, "Manager shown: reopen on index 0");
    m.page_shown(false, false);                                     // the menu closed on the Manager page
    eq(*m.record_type(0), 0, "menu closed on Manager: still Manager");
    m.page_shown(false, true);                                      // Record (or Replay) built after ours
    check(m.unresolved(), "a game page built: pending until RecordType says which");
    m.resolve(0);                                                   // the menu closed: Function(107) wrote 0
    check(m.page() == P::Record && !m.unresolved(), "RecordType 0 at the close: Record");
    eq(*m.record_type(0), 1, "reopen on Record = index 1");
    eq(*m.record_type(1), 1, "resolved: the same answer whatever RecordType holds meanwhile");
    m.page_shown(true, true);
    m.page_shown(false, true);
    m.resolve(1);                                                   // Function(108) wrote 1
    eq(*m.record_type(1), 2, "RecordType 1 at the close: Replay = index 2");
    m.page_shown(true, true);
    m.page_shown(false, true);
    eq(*m.record_type(1), 2, "unresolved at CreateViewData: resolved there (1 = Replay)");
    m.page_shown(true, true);
    m.page_shown(false, true);
    eq(*m.record_type(0), 1, "unresolved at CreateViewData: resolved there (0 = Record)");
    m.page_shown(false, false);                                     // the close after it: kept
    eq(*m.record_type(5), 1, "kept across a close");
    m.reset();
    check(m.page() == P::None && *m.record_type(0) == 1, "left training: forgotten (RMT:2095), the game's memory again");
    check(record_type_needs_reset(2, false), "2 with the menu closed: back to 0 (RMT:2111)");
    check(!record_type_needs_reset(2, true), "2 with the menu up: left");
    check(!record_type_needs_reset(1, false) && !record_type_needs_reset(0, false), "0 / 1: left");
}

// The bug of 14/10 ("left the menu on Replay, reopened on Record"): the core says shown 0 only when OUR page stops being
// shown, never for Record <-> Replay; the game's page builds come from P4's Function pre-hook (game_page), as RMT:2014.
void test_memory_game_pages() {
    begin("P5 sub-page memory: game page builds (Function 107 / 108)");
    using P = PageMemory::Page;
    PageMemory m;
    // a session as in Wael's journal of 14/10: Manager, Record, close, reopen on Record, Replay, close
    m.page_shown(true, true);                                       // Manager built (core: shown 1)
    m.page_shown(false, true);                                      // D: Record built after ours (core: shown 0, menu up)
    m.game_page(P::Record);                                         // Function(107) pre (after the core's hook)
    check(m.page() == P::Record && !m.unresolved(), "Record built after ours: Record, nothing pending");
    m.page_shown(false, false);                                     // the menu closed (no edge: ours not shown)
    m.resolve(0);
    eq(*m.record_type(0), 1, "reopen on Record");
    m.game_page(P::Record);                                         // the reopen built Record (Function(107) wrote 0)
    m.game_page(P::Replay);                                         // D: Replay (no core edge: ours was not shown)
    m.resolve(1);                                                   // the close: nothing pending
    eq(*m.record_type(1), 2, "left on Replay: reopen on Replay (was Record)");
    m.game_page(P::Replay);                                         // the reopen built Replay (Function(108) wrote 1)
    m.game_page(P::Record);                                         // Q: Record
    eq(*m.record_type(0), 1, "left on Record after a reopen on Replay: reopen on Record (was Replay)");
    // the Function hook before the core's edge: the edge does not reopen a pending read
    m.page_shown(true, true);
    m.game_page(P::Replay);
    m.page_shown(false, true);
    check(m.page() == P::Replay && !m.unresolved(), "Function(108) before the core's edge: Replay, nothing pending");
    m.resolve(0);                                                   // (a stale RecordType never overrides it)
    eq(*m.record_type(0), 2, "Replay kept whatever RecordType holds");
    // the edge first (the existing fallback when the Function hook is missing, R18): resolved from RecordType
    m.page_shown(true, true);
    m.page_shown(false, true);
    check(m.unresolved(), "edge first: pending");
    m.game_page(P::Record);
    check(m.page() == P::Record && !m.unresolved(), "then Function(107): Record, resolved");
    // only Record / Replay are game pages
    m.game_page(P::Manager);
    m.game_page(P::None);
    check(m.page() == P::Record, "game_page(Manager / None) ignored");
    m.page_shown(true, true);
    m.game_page(P::Replay);
    m.page_shown(true, true);                                       // back on ours
    eq(*m.record_type(1), 0, "Replay then Manager: reopen on Manager");
    m.reset();
    m.game_page(P::Replay);                                         // first build of a session on Replay (the game's pick)
    eq(*m.record_type(1), 2, "first page of a session: the build sets the memory");
    check(record_type_fits(2, 3) && record_type_fits(0, 3) && record_type_fits(1, 3), "3-page strip (ours in it): 0-2 written");
    check(!record_type_fits(1, 2) && !record_type_fits(2, 2), "2-page strip (ours not installed yet): the game's own pick");
    check(!record_type_fits(2, -1) && !record_type_fits(-1, 3) && !record_type_fits(3, 3), "unreadable strip / out of range: left");
    eq(std::string(page_name(P::Replay)), std::string("Replay"), "page_name");
    eq(std::string(page_name(P::None)), std::string("none"), "page_name(None)");
}

void test_hints() {
    begin("P5 hints");
    check(hints_for(true, false, kFtRecordSlot).empty(), "own row (pm_row_of >= 0): none");
    check(hints_for(true, true, kFtRecordSlot).empty(), "own row even with FuncType 118: none");
    const auto h = hints_for(false, true, kFtRecordSlot);
    check(h.size() == 2 && h[0].button == kButtonY && std::string(h[0].key) == "edit_name" && h[1].button == kButtonX &&
              std::string(h[1].key) == "delete_name",
          "Record slot row: Y edit, X delete (RMT:1555)");
    eq(kButtonY, 93, "Y = UI_Y 93");
    eq(kButtonX, 92, "X = UI_X 92");
    check(hints_for(false, true, 131).empty(), "Replay slot row (131): none");
    check(hints_for(false, true, 345).empty(), "another DYNAMIC row: none");
    check(hints_for(false, false, kFtRecordSlot).empty(), "FuncType unreadable: none");
    g_loc.set_lang("en");
    eq(g_loc.T("edit_name"), std::string("Edit slot name"), "hint text Y");
    eq(g_loc.T("delete_name"), std::string("Delete slot name"), "hint text X");
}

void test_queue() {
    begin("P5 action queue");
    Queue q;
    q.push({ Act::Select, 3 });
    q.push({ Act::Open, 0 });
    q.push({ Act::Select, 4 });
    eq(q.size(), (size_t)3, "three queued");
    eq(q.selects(), (size_t)2, "two selects waiting");
    const auto v = q.drain();
    check(v.size() == 3 && v[0].act == Act::Select && v[0].arg == 3 && v[1].act == Act::Open && v[2].arg == 4, "drained in order");
    eq(q.size(), (size_t)0, "empty after drain");
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back([&q, t] { for (int i = 0; i < 10; ++i) q.push({ Act::Select, t * 100 + i }); });
    for (auto& t : ts) t.join();
    const auto w = q.drain();
    eq(w.size(), (size_t)40, "40 pushes from 4 threads");
    bool ordered = true;                                            // each thread's own pushes keep their order
    for (int t = 0; t < 4; ++t) {
        int last = -1;
        for (const Action& a : w) if (a.arg / 100 == t) { ordered = ordered && a.arg > last; last = a.arg; }
    }
    check(ordered, "per-thread order kept");
    for (int i = 0; i < 100; ++i) q.push({ Act::Ensure, i });
    eq(q.size(), (size_t)64, "bounded at 64");
    q.drain();
}

void test_actions_fake() {
    begin("P5 actions on fakes (RMT tickers, boxes, dialogs)");
    g_loc.set_lang("en");
    {   // Open
        FakeLoadouts f; FakeScreen s; Runner r(f, s, translator());
        r.run({ Act::Open, 0 });
        check(s.tickers == std::vector<std::string>{ "No drill selected" } && !has(f.calls, "import_selected"), "Open without a selection");
        f.sel = "Ryu_Test";
        r.run({ Act::Open, 0 });
        check(f.native_action && has(f.calls, "import_selected"), "Open: native action on, import");
        check(s.count("check_slots") == 1 && s.count("rows_dirty") == 1, "Open: CheckMenuSlotData and the rows redrawn");
        eq(s.tickers.back(), std::string("Drill opened: Ryu_Test"), "Open ticker");
        check(s.count("confirm") == 0, "Open asks nothing");
        f.import_res = "Allocating slots... (Loaded later)";
        r.run({ Act::Open, 0 });
        eq(s.tickers.back(), std::string("Drill opened: Ryu_Test (allocating...)"), "Open with allocation");
        f.import_res = "Load Failed: Ryu_Test.json";
        r.run({ Act::Open, 0 });
        eq(s.tickers.back(), std::string("Open failed: Load Failed: Ryu_Test.json"), "Open failed");
    }
    {   // Save
        FakeLoadouts f; FakeScreen s; Runner r(f, s, translator());
        f.export_tk = "Ryu File Saved";
        r.run({ Act::Save, 0 });
        eq(s.last_confirm, std::string("Save the 30 slots into 'Ryu'?"), "Save without a selection: the character's name");
        eq(s.last_confirm_title, std::string("Save drill"), "Save box title (rsm.save, RMT:116)");
        check(has(f.calls, "export_selected"), "Save: export after Yes");
        check(s.tickers == std::vector<std::string>{ "Ryu File Saved", "Drill saved: Ryu" }, "Save tickers: MAIN's, then the page's",
              join(s.tickers));
        f.sel = "Combo A"; f.export_res = "Error: No RecFunc"; f.export_tk.clear();
        r.run({ Act::Save, 0 });
        eq(s.last_confirm, std::string("Save the 30 slots into 'Combo A'?"), "Save with a selection");
        eq(s.tickers.back(), std::string("Save failed: Error: No RecFunc"), "Save failed");
        s.confirm_answer = false;
        const size_t before = f.calls.size();
        r.run({ Act::Save, 0 });
        check(f.calls.size() == before, "No: nothing saved");
        f.chr.clear(); f.sel.clear(); s.confirm_answer = true;
        r.run({ Act::Save, 0 });
        eq(s.last_confirm, std::string("Save the 30 slots into ''?"), "no selection, no character (as v1.6)");
    }
    {   // Save As
        FakeLoadouts f; FakeScreen s; Runner r(f, s, translator());
        f.sel = "Old"; s.input_text = "Test <A>"; f.export_tk = "Ryu_Test A File Saved";
        r.run({ Act::SaveAs, 0 });
        eq(s.last_input_initial, std::string("Old"), "Save As proposes the selected name");
        eq(s.last_max, kNameMax, "64 bytes at most");
        eq(s.last_input_title, std::string("Save drill as"), "dialog title");
        eq(s.last_confirm, std::string("Save the 30 slots into 'Test <A>'?"), "Save As box text");
        eq(s.last_confirm_title, std::string("Save drill as"), "Save As box title (rsm.save_as, RMT:1615)");
        check(has(f.calls, "export_as(Test <A>)") && s.count("list_changed") == 1, "export_as with the typed text, the spin synced");
        check(s.tickers == std::vector<std::string>{ "Ryu_Test A File Saved", "Drill saved as: Test <A>" }, "Save As tickers", join(s.tickers));
        f.sel.clear();
        s.input_text = "";
        const size_t c0 = s.count("confirm");
        r.run({ Act::SaveAs, 0 });
        eq(s.last_input_initial, std::string("Ryu"), "no selection: the character proposed");
        check(s.count("confirm") == c0, "empty name: silently nothing");
        s.input_ok = false; s.input_text = "X";
        r.run({ Act::SaveAs, 0 });
        check(s.count("confirm") == c0, "cancelled: nothing");
        check(!r.dialog_open(), "no dialog left open");
    }
    {   // Reset
        FakeLoadouts f; FakeScreen s; Runner r(f, s, translator());
        r.run({ Act::Reset, 0 });
        check(s.tickers == std::vector<std::string>{ "No drill open" } && s.count("confirm") == 0, "Reset with nothing open");
        f.loaded = "Ryu_Test.json";                                 // Lua defect 4 kept: the name carries ".json" after Open
        r.run({ Act::Reset, 0 });
        eq(s.last_confirm, std::string("Reload 'Ryu_Test.json'? The current slots are lost."), "Reset box text");
        eq(s.last_confirm_title, std::string("Reset drill"), "Reset box title (rsm.reset, RMT:127)");
        check(f.native_action && has(f.calls, "reopen_loaded") && s.count("check_slots") == 1 && s.count("rows_dirty") == 1, "Reset: reopen");
        eq(s.tickers.back(), std::string("Drill reset: Ryu_Test.json"), "Reset ticker");
        f.reopen_res = "Allocating slots";
        r.run({ Act::Reset, 0 });
        eq(s.tickers.back(), std::string("Drill reset: Ryu_Test.json (allocating...)"), "Reset with allocation");
        f.reopen_res = "No drill loaded";
        r.run({ Act::Reset, 0 });
        eq(s.tickers.back(), std::string("Reset failed: No drill loaded"), "Reset failed");
    }
    {   // Erase
        FakeLoadouts f; FakeScreen s; Runner r(f, s, translator());
        r.run({ Act::Erase, 0 });
        check(s.tickers == std::vector<std::string>{ "No drill selected" }, "Erase without a selection");
        f.sel = "Combo A";
        r.run({ Act::Erase, 0 });
        eq(s.last_confirm, std::string("Erase 'Combo A'?"), "Erase box text");
        eq(s.last_confirm_title, std::string("Erase drill"), "Erase box title (rsm.erase, RMT:141)");
        const std::vector<std::string> want{ "erase_selected", "invalidate", "ensure" };
        check(std::search(f.calls.begin(), f.calls.end(), want.begin(), want.end()) != f.calls.end(), "erase, then the list read again",
              join(f.calls));
        check(s.count("list_changed") == 1 && s.count("rows_dirty") == 1 && s.count("check_slots") == 0, "Erase: spin synced, rows redrawn");
        eq(s.tickers.back(), std::string("Drill erased: Combo A"), "Erase ticker");
        f.erase_res = "No drill selected";
        r.run({ Act::Erase, 0 });
        eq(s.tickers.back(), std::string("Erase failed: No drill selected"), "Erase failed");
    }
    {   // Rename (T on the page)
        FakeLoadouts f; FakeScreen s; Runner r(f, s, translator());
        r.run({ Act::Rename, 0 });
        check(s.count("text_input") == 0 && s.tickers.empty(), "Rename without a selection: nothing");
        f.sel = "Old"; s.input_text = "New"; f.rename_tk = "Ryu_New File Saved";
        r.run({ Act::Rename, 0 });
        eq(s.last_input_initial, std::string("Old"), "Rename proposes the selected name");
        check(s.count("confirm") == 0, "Rename asks no confirmation");
        check(has(f.calls, "rename_selected(New)") && s.count("list_changed") == 1, "rename_selected, spin synced");
        check(s.tickers == std::vector<std::string>{ "Ryu_New File Saved", "Drill renamed: New" }, "Rename tickers", join(s.tickers));
        s.input_text = ""; f.rename_tk.clear();
        r.run({ Act::Rename, 0 });
        eq(s.tickers.back(), std::string("Rename failed: nil"), "empty name: 'Rename failed: nil' (as v1.6)");
        f.rename_res = "No drill selected"; s.input_text = "Z";
        r.run({ Act::Rename, 0 });
        eq(s.tickers.back(), std::string("Rename failed: No drill selected"), "Rename failed");
    }
    {   // one dialog at a time (RMT naming / confirm_box)
        FakeLoadouts f; FakeScreen s; Runner r(f, s, translator());
        f.sel = "A"; f.loaded = "A"; s.auto_answer = false;
        r.run({ Act::Save, 0 });
        check(r.dialog_open(), "a box is up");
        r.run({ Act::Erase, 0 });
        r.run({ Act::Reset, 0 });
        r.run({ Act::SaveAs, 0 });
        r.run({ Act::Rename, 0 });
        check(s.count("confirm") == 1 && s.count("text_input") == 0, "second box / dialogs refused while it is up");
        s.held_confirm(true);
        check(!r.dialog_open() && has(f.calls, "export_selected"), "Yes runs the save, the guard lifted");
        r.run({ Act::SaveAs, 0 });
        check(r.dialog_open() && s.count("text_input") == 1, "the name dialog is up");
        r.run({ Act::Rename, 0 });
        check(s.count("text_input") == 1, "rename refused while the name dialog is up");
        r.menu_closed();
        check(!r.dialog_open(), "the menu closed: no dialog of ours left");
        s.held_input(true, "Late");                                 // a late answer still applies (as RMT's apply_naming)
        check(s.count("confirm") == 2, "late answer: the box of Save As");
    }
    {   // Share, Download, Select, Ensure, Refresh
        FakeLoadouts f; FakeScreen s; Runner r(f, s, translator());
        r.run({ Act::Share, 0 });
        r.run({ Act::Download, 0 });
        check(s.calls == std::vector<std::string>{ "community_share", "community_open" }, "Share / Download: P4's Community dialog");
        r.run({ Act::Select, 5 });
        check(f.calls.back() == "set_dropdown_index(5)" && f.index == 5, "Select = set_dropdown_index (selecting loads nothing)");
        check(!has(f.calls, "import_selected"), "no import on a spin move");
        r.run({ Act::Ensure, 0 });
        check(f.calls.back() == "ensure" && s.calls.back() == "list_changed", "Ensure: ensure_files, then the spin synced");
        r.run({ Act::Refresh, 0 });
        const std::vector<std::string> want{ "invalidate", "ensure" };
        check(std::search(f.calls.begin(), f.calls.end(), want.begin(), want.end()) != f.calls.end() && s.calls.back() == "list_changed",
              "Refresh: invalidate, ensure, spin synced");
        eq(r.runs(), (uint64_t)5, "runs counted");
    }
    {   // French tickers
        FakeLoadouts f; FakeScreen s; Runner r(f, s, translator());
        g_loc.set_lang("fr");
        f.sel = "Ryu_Test";
        r.run({ Act::Open, 0 });
        eq(s.tickers.back(), g_loc.T("opened", { "Ryu_Test" }), "fr Open ticker from locale.json");
        check(s.tickers.back() != "Drill opened: Ryu_Test", "fr ticker is not the English one", s.tickers.back());
        r.run({ Act::Erase, 0 });
        eq(s.last_confirm_title, g_loc.T("erase", {}), "fr box title from locale.json (the row's title, as RMT's T(\"erase\"))");
        check(s.last_confirm_title != "Erase drill", "fr box title is not the English one", s.last_confirm_title);
        g_loc.set_lang("en");
    }
}

// ---- the real files engine on a scratch folder -------------------------------------------------------------------------------------
struct Slot { bool valid = false; int64_t frames = 0; json weight = 1; std::vector<uint16_t> buff; };
struct Live final : model::files::LiveSlots {
    int fid = 1;
    Slot s[8];
    std::string access(int f) override { return f == fid ? "" : (f == -1 ? "Unknown Character ID" : "Data not found for ID " + std::to_string(f)); }
    int read(int, int k, model::files::LiveRead* o) override {
        o->valid = s[k].valid; o->frames = s[k].frames; o->weight = s[k].weight; o->values.clear();
        if (s[k].valid && s[k].frames > 0) {
            if ((int64_t)s[k].buff.size() < s[k].frames) return 2;
            o->values.assign(s[k].buff.begin(), s[k].buff.begin() + (ptrdiff_t)s[k].frames);
        }
        return 0;
    }
    int64_t capacity(int, int k) override { return (int64_t)s[k].buff.size(); }
    bool allocate(int, int k, int64_t needed) override { s[k].buff.assign((size_t)std::max<int64_t>(3600, needed), 0); return true; }
    bool set_weight(int, int k, const json& w) override { s[k].weight = w; return true; }
    bool write_empty(int, int k) override { s[k].valid = false; s[k].frames = 0; return true; }
    bool write_values(int, int k, const uint16_t* v, size_t n) override {
        if (n > s[k].buff.size()) return false;
        std::copy(v, v + n, s[k].buff.begin());
        return true;
    }
    bool finish(int, int k, int64_t needed) override { s[k].valid = true; s[k].frames = needed; return true; }
    bool activate_valid(int) override { return true; }
};
struct Clock final : model::files::Clock {
    double t = 100;
    double seconds() override { return t += 10; }                 // past the 5 s backup gate at every import
    std::tm local_now() override { std::tm tm{}; tm.tm_year = 126; tm.tm_mon = 9; tm.tm_mday = 10; return tm; }
};
struct EngineLoadouts final : Loadouts {
    model::files::Engine& e;
    explicit EngineLoadouts(model::files::Engine& en) : e(en) {}
    std::string selected_name() override { return e.selected_name(); }
    std::string char_name() override { const auto c = e.char_name(); return c ? *c : std::string(); }
    std::string loaded_name() override { return e.loaded_name(); }
    std::vector<std::string> dropdown() override { return e.dropdown(); }
    int dropdown_index() override { return e.dropdown_index(); }
    void set_dropdown_index(int i) override { e.set_dropdown_index(i); }
    void set_native_action(bool on) override { e.set_native_action(on); }
    std::string import_selected() override { return e.import_selected(); }
    std::string reopen_loaded() override { return e.reopen_loaded(); }
    std::string export_selected(std::string* tk) override { return e.export_selected(tk); }
    std::string export_as(const std::string& n, std::string* tk) override { return e.export_as(json(n), tk); }
    std::string erase_selected() override { return e.erase_selected(); }
    std::string rename_selected(const std::string& n, std::string* tk) override {   // as the DLL's port: the file renamed
        if (tk) tk->clear();
        return e.rename_file(json(n));
    }
    void invalidate() override { e.invalidate(); }
    bool ensure() override { return e.ensure(1); }
};

size_t index_of(const std::vector<std::string>& v, const std::string& x) {
    const auto it = std::find(v.begin(), v.end(), x);
    return it == v.end() ? (size_t)-1 : (size_t)(it - v.begin());
}

void test_actions_engine(const fs::path& work) {
    begin("P5 actions on the files engine (scratch data)");
    g_loc.set_lang("en");
    const fs::path root = work / "p5_engine";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SF6_RecordingSlotManager_data" / "Ryu", ec);
    Live live;
    for (int k = 0; k < 3; ++k) {                                   // three recorded slots
        live.s[k].valid = true; live.s[k].frames = 20 + k; live.s[k].buff.assign(3600, 0);
        for (int i = 0; i < 20 + k; ++i) live.s[k].buff[(size_t)i] = (uint16_t)(i % 7 == 0 ? 16 : 0);
    }
    Clock clock;
    model::files::Engine eng(root, &live, &clock);
    eng.load_settings();
    EngineLoadouts L(eng);
    FakeScreen s;
    Runner r(L, s, translator());
    r.run({ Act::Ensure, 0 });
    eq(join(spin_labels(L.dropdown())), std::string(kNoFile), "a character without drills: the no-file entry only");
    eq(spin_value(L.dropdown_index(), spin_labels(L.dropdown()).size()), 0, "value 0");
    // Save As "Alpha"
    s.input_text = "Alpha";
    r.run({ Act::SaveAs, 0 });
    check(fs::exists(root / "SF6_RecordingSlotManager_data" / "Ryu" / "Ryu_Alpha.json"), "Save As wrote Ryu/Ryu_Alpha.json (the character prefix)");
    eq(s.tickers.back(), std::string("Drill saved as: Alpha"), "Save As ticker");
    auto labels = spin_labels(L.dropdown());
    const size_t ia = index_of(labels, "Alpha");
    check(ia != (size_t)-1, "Alpha in the spin", join(labels));
    eq(spin_value(L.dropdown_index(), labels.size()), (int32_t)ia, "the spin moved onto Alpha");
    // Save onto it
    r.run({ Act::Save, 0 });
    eq(s.last_confirm, std::string("Save the 30 slots into 'Alpha'?"), "Save box on the selection");
    eq(s.tickers.back(), std::string("Drill saved: Alpha"), "Save ticker");
    // Open: the slots emptied first, then the pack brings them back
    for (auto& sl : live.s) { sl.valid = false; sl.frames = 0; }
    r.run({ Act::Open, 0 });
    eq(s.tickers.back(), std::string("Drill opened: Alpha"), "Open ticker");
    check(live.s[0].valid && live.s[0].frames == 20 && live.s[2].frames == 22, "Open restored the three slots");
    check(s.count("check_slots") >= 1 && s.count("rows_dirty") >= 1, "Open: the rows follow");
    // Reset (the open loadout)
    r.run({ Act::Reset, 0 });
    check(s.last_confirm.find("Alpha") != std::string::npos, "Reset box names the open drill", s.last_confirm);
    check(s.tickers.back().rfind("Drill reset: ", 0) == 0, "Reset ticker", s.tickers.back());
    // Rename to Beta
    s.input_text = "Beta";
    r.run({ Act::Rename, 0 });
    eq(s.tickers.back(), std::string("Drill renamed: Beta"), "Rename ticker");
    labels = spin_labels(L.dropdown());
    check(index_of(labels, "Beta") != (size_t)-1 && index_of(labels, "Alpha") == (size_t)-1, "the spin shows Beta, not Alpha", join(labels));
    check(!fs::exists(root / "SF6_RecordingSlotManager_data" / "Ryu" / "Ryu_Alpha.json") ||
              model::luajson::load_file(root / "SF6_RecordingSlotManager_data" / "Ryu" / "Ryu_Alpha.json").is_null(),
          "the old file emptied (Lua cannot delete)");
    // a spin move to the no-file entry, then back to Beta
    r.run({ Act::Select, dropdown_of_spin(0, labels.size()) });
    eq(L.selected_name(), std::string(), "option 0 = nothing selected");
    r.run({ Act::Erase, 0 });
    eq(s.tickers.back(), std::string("No drill selected"), "Erase with nothing selected");
    r.run({ Act::Select, dropdown_of_spin((int32_t)index_of(labels, "Beta"), labels.size()) });
    eq(L.selected_name(), std::string("Beta"), "Beta selected again");
    // Erase
    r.run({ Act::Erase, 0 });
    eq(s.tickers.back(), std::string("Drill erased: Beta"), "Erase ticker");
    labels = spin_labels(L.dropdown());
    eq(join(labels), std::string(kNoFile), "the spin back to the no-file entry");
    std::ifstream f(root / "SF6_RecordingSlotManager_data" / "Ryu" / "Ryu_Beta.json", std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    eq(body, std::string("null"), "the erased file holds null (json.dump_file of {})");
    check(!r.dialog_open(), "no dialog left");

    f.close();                                                      // (an open file cannot be replaced on Windows)

    // Edit Drill Name (T): the file renamed, its content kept (not the game's slots), names checked
    begin("P5 Edit Drill Name on the files engine (scratch data)");
    const fs::path ryu = root / "SF6_RecordingSlotManager_data" / "Ryu";
    s.input_text = "Gamma";
    r.run({ Act::SaveAs, 0 });
    s.input_text = "Delta";
    r.run({ Act::SaveAs, 0 });                                      // Delta selected now
    const std::string delta_before = model::luajson::load_file(ryu / "Ryu_Delta.json").dump();
    for (auto& sl : live.s) { sl.valid = false; sl.frames = 0; } // the game's slots emptied: a rename must not save them
    s.input_text = "  Echo: <x>?  ";
    r.run({ Act::Rename, 0 });
    eq(s.tickers.back(), std::string("Drill renamed:   Echo: <x>?  "), "rename ticker (the typed name)");
    check(!fs::exists(ryu / "Ryu_Delta.json"), "the old file is gone");
    check(fs::exists(ryu / "Ryu_Echo x.json"), "Ryu_Echo x.json: trimmed, Windows' forbidden characters dropped, prefix added");
    eq(model::luajson::load_file(ryu / "Ryu_Echo x.json").dump(), delta_before, "the content kept (not the emptied game slots)");
    labels = spin_labels(L.dropdown());
    eq(L.selected_name(), std::string("Echo: <x>?"), "the renamed drill still selected, its display name kept");
    check(index_of(labels, "Echo: <x>?") != (size_t)-1 && index_of(labels, "Delta") == (size_t)-1, "the spin shows the new name", join(labels));
    eq(spin_value(L.dropdown_index(), labels.size()), (int32_t)index_of(labels, "Echo: <x>?"), "the spin's value on it");
    s.input_text = "gamma";
    r.run({ Act::Rename, 0 });
    eq(s.tickers.back(), std::string("Rename failed: Name already used: Gamma"), "a name another drill has (any case): refused");
    check(fs::exists(ryu / "Ryu_Echo x.json") && fs::exists(ryu / "Ryu_Gamma.json"), "both files untouched");
    s.input_text = "???";
    r.run({ Act::Rename, 0 });
    eq(s.tickers.back(), std::string("Rename failed: No name"), "nothing left after the cleaning: refused");
    s.input_text = "Beta";                                          // the erased drill's name is free again
    r.run({ Act::Rename, 0 });
    eq(s.tickers.back(), std::string("Drill renamed: Beta"), "onto an erased drill's name");
    eq(model::luajson::load_file(ryu / "Ryu_Beta.json").dump(), delta_before, "the erased file replaced by the drill");
    labels = spin_labels(L.dropdown());
    check(index_of(labels, "Beta") != (size_t)-1, "Beta listed again", join(labels));
    r.run({ Act::Select, dropdown_of_spin(0, labels.size()) });
    s.input_text = "Zeta";
    const size_t before = s.count("text_input");
    r.run({ Act::Rename, 0 });
    check(s.count("text_input") == before, "on \"-- Select File --\": no dialog");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    fs::path repo, work;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--repo" && i + 1 < argc) repo = argv[++i];
        else if (a == "--work" && i + 1 < argc) work = argv[++i];
    }
    if (repo.empty() || work.empty()) {
        std::fprintf(stderr, "usage: p5_bench --repo <crsm folder> --work <dir>\n");
        return 2;
    }
    fs::create_directories(work);
    const json file = model::luajson::load_file(repo / "data" / "SF6_CRSM_data" / "locale.json");
    if (file.is_null()) { std::fprintf(stderr, "locale.json not found under %s\n", repo.string().c_str()); return 2; }
    g_loc.load(file);
    for (auto it = file.begin(); it != file.end(); ++it) if (it.key() != "_readme" && it->is_object()) g_langs.push_back(it.key());
    std::printf("[P5] Manager page\n");
    test_texts();
    test_localise();
    test_spin();
    test_memory();
    test_memory_game_pages();
    test_hints();
    test_queue();
    test_actions_fake();
    test_actions_engine(work);
    test_details_texts();

    int checks = 0, fails = 0;
    std::printf("\n%-60s %8s %8s\n", "group", "checks", "failed");
    for (const std::string& n : g_order) {
        const Group& g = g_groups[n];
        std::printf("%-60s %8d %8d\n", n.c_str(), g.checks, g.fails);
        checks += g.checks; fails += g.fails;
    }
    std::printf("%-60s %8d %8d\n", "TOTAL", checks, fails);
    std::printf("\n%s\n", fails == 0 ? "ALL GREEN" : "FAILURES");
    return fails == 0 ? 0 : 1;
}
