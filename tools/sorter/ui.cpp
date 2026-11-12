// SF6_ModSorter -- the window, drawn like Street Fighter 6's own menus (Battle Settings: the page header with its thin line,
// the bevelled dark rows, the white / blue selected row, the value control with its arrows, the summary panel at the right,
// the key hints at the bottom). Everything is drawn with shapes and colours (Direct2D) and Bahnschrift (DirectWrite, shipped
// with Windows): no asset of the game. The window has no Windows title bar: the page header is its title bar (drag, minimise,
// close), the borders resize it, DWM gives it its shadow. Per-monitor DPI aware: everything is laid out in DIPs.
//
// Threads: the window's thread draws and handles input; one worker classifies and copies, one item at a time (the loaders'
// code it runs keeps its state in globals); one more reads the mods' pictures (pictures.cpp), an archive never at the same
// time as the worker. --shot does the same work in line, renders into a WIC bitmap and never makes a window.

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <wincodec.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "pictures.hpp"
#include "sorter.hpp"
#include "suite_update.hpp"
#include "ui.hpp"
#include "version.hpp"
#include "../../updater/src/update_install.hpp"   // sf6up::Progress

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------------------------
// Colours (sampled from the game's Battle Settings page at 1080p)
// ---------------------------------------------------------------------------------------------

D2D1_COLOR_F rgb(uint32_t c, float a = 1.f) {
    return D2D1::ColorF(((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f, (c & 0xFF) / 255.f, a);
}

const uint32_t kBgA = 0x080B17, kBgB = 0x121931, kLine = 0xB3B3B3, kText = 0xE4E4E4, kTextSoft = 0xC9CCD4, kTextDim = 0x7D8088,
               kDark = 0x34363B, kBlue = 0x5BB0F0, kBlueDeep = 0x1756B8, kBar = 0x1565D8, kYellow = 0xFDBF3F, kRed = 0xFF7B7B,
               kRedDeep = 0xB23232, kRowA = 0x23242A, kRowB = 0x1A1B20, kRowEdge = 0x5A5B63;

// ---------------------------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------------------------

struct Item {
    int id = 0;
    std::wstring path;
    bool reading = true;
    Classification c;
    bool nsfw = false, replace = false;
    enum Done { None, Installing, Installed, Skipped, Failed } done = None;
    std::wstring placed_to;
    Msg error;
    float progress = -1.f;
    bool exists = false;
};

enum Action { A_NONE, A_ROW, A_LEFT, A_RIGHT, A_TOGGLE, A_BROWSE, A_GAME, A_INSTALL, A_REMOVE, A_QUIT, A_MIN, A_CLOSE,
              A_TAB, A_CAT, A_OPEN, A_LAUNCH, A_CELL, A_MAX, A_NSFW, A_UPDATE, A_ZIP };   // A_TAB / A_CAT / A_CELL: item is the tab / the category / the cell

struct Hit {
    D2D1_RECT_F r;
    Action a;
    int item = -1;
    bool sub = false;
};

// The Installed page: what the game folder already holds, one InstCat per mod folder
struct InstEntry {
    std::wstring name;
    uint64_t stamp = 0;                // its size and date: a mod replaced by another has its picture read again
};
struct InstGroup {
    enum Kind { Character, Several, AllChars, Flat } kind = Flat;
    std::wstring name;                 // Character: the folder's name
    std::wstring folder;               // the folder its entries are in, under the category's ("": the category's own)
    std::vector<InstEntry> entries;
};
struct InstCat {
    TextId label = T__COUNT;
    std::wstring dir;                  // under reframework
    bool present = false;              // the folder is there
    bool pictures = false;             // shown as a grid of the mods' pictures (colour files have none: a list of names)
    int count = 0;
    std::vector<InstGroup> groups;
};

const UINT MSG_INIT = WM_APP + 1, MSG_CLASSIFIED = WM_APP + 2, MSG_PROGRESS = WM_APP + 3, MSG_PLACED = WM_APP + 4,
           MSG_PICTURE = WM_APP + 5, MSG_UPDATE = WM_APP + 6;

struct InitResult { int gen; bool costume_ok, stage_ok; std::string costume_note, stage_note; };
struct ClassResult { int gen; int item; Classification c; };
struct PlacedResult { int item; PlaceResult r; };
struct PicResult { std::wstring key; bool ok = false; ModPicture pic; };
// kind: 0 a look-up (check, self), 1 the suite installed (inst), 2 the sorter itself (sinst)
struct UpdateResult { int gen = 0; int kind = 0; SuiteCheck check; SelfCheck self; SuiteInstall inst; SelfInstall sinst; };

// A mod's picture as the window holds it. Its key: the size asked, the mod's stamp, its path.
struct Pic {
    enum State { Loading, None, Ready } state = Loading;
    int w = 0, h = 0;
    std::vector<uint8_t> px;           // until its bitmap is made
    ID2D1Bitmap* bmp = nullptr;
    ID2D1RenderTarget* owner = nullptr;   // the target the bitmap was made for
};

// ---------------------------------------------------------------------------------------------
// The worker: classification and copies, one at a time
// ---------------------------------------------------------------------------------------------

struct Job {
    enum Type { Init, Classify, Place } type = Classify;
    int gen = 0, item = 0, seq = 0;
    std::wstring path, game_dir;
    Classification c;
    bool nsfw = false, replace = false;
};

class Worker {
public:
    HWND target = nullptr;
    std::wstring work_dir;
    std::atomic<bool> quit{false};

    void start() { th_ = std::thread([this] { loop(); }); }
    void push(Job j) { { std::lock_guard<std::mutex> l(m_); q_.push_back(std::move(j)); } cv_.notify_one(); }
    void stop() {
        quit = true;
        cv_.notify_one();
        if (th_.joinable()) th_.join();
    }
    bool busy() { std::lock_guard<std::mutex> l(m_); return busy_ || !q_.empty(); }

private:
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    bool busy_ = false;
    ClassifyContext ctx_;

    struct Prog { HWND h; int item; int last; std::atomic<bool>* quit; };
    static void on_progress(void* p, uint64_t done, uint64_t total) {
        Prog* pr = static_cast<Prog*>(p);
        const int pm = total ? int(done * 1000 / total) : 0;
        if (pm != pr->last) { pr->last = pm; PostMessageW(pr->h, MSG_PROGRESS, (WPARAM)pr->item, (LPARAM)pm); }
    }

    void loop() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return quit || !q_.empty(); });
                if (quit) return;
                j = std::move(q_.front());
                q_.pop_front();
                busy_ = true;
            }
            if (j.type == Job::Init) {
                ctx_.game_dir = j.game_dir;
                ctx_.work_dir = work_dir;
                classify_init(ctx_);
                auto* r = new InitResult{j.gen, ctx_.costume_ok, ctx_.stage_ok, ctx_.costume_note, ctx_.stage_note};
                if (!PostMessageW(target, MSG_INIT, 0, (LPARAM)r)) delete r;
            } else if (j.type == Job::Classify) {
                auto* r = new ClassResult{j.gen, j.item, {}};
                {
                    std::lock_guard<std::mutex> lock(archive_mutex());   // not while the picture thread reads an archive
                    r->c = classify(j.path, ctx_, j.seq);
                }
                if (!PostMessageW(target, MSG_CLASSIFIED, 0, (LPARAM)r)) delete r;
            } else {
                Prog pr{target, j.item, -1, &quit};
                auto* r = new PlacedResult{j.item, place_item(j.game_dir, j.c, j.nsfw, j.replace, on_progress, &pr)};
                if (!PostMessageW(target, MSG_PLACED, 0, (LPARAM)r)) delete r;
            }
            std::lock_guard<std::mutex> l(m_);
            busy_ = false;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// The picture thread: the mods' pictures, in the order they were asked for
// ---------------------------------------------------------------------------------------------

class PicThread {
public:
    HWND target = nullptr;
    std::wstring work_dir, cache_dir;

    void start() { th_ = std::thread([this] { loop(); }); }
    // when: the time of the drawing that asks. What the last drawing asked for is read first (the cells in sight now, not the
    // ones scrolled past), in the order it asked.
    void want(const std::wstring& key, const std::wstring& path, int side, double when) {
        { std::lock_guard<std::mutex> l(m_); q_.push_back({key, path, side, when}); }
        cv_.notify_one();
    }
    void stop() {
        { std::lock_guard<std::mutex> l(m_); quit_ = true; }
        cv_.notify_one();
        if (th_.joinable()) th_.join();
    }

private:
    struct Job { std::wstring key, path; int side = 0; double when = 0; };
    std::thread th_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> q_;
    bool quit_ = false;

    void loop() {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        {
            PictureLoader loader(work_dir, cache_dir);
            for (;;) {
                Job j;
                {
                    std::unique_lock<std::mutex> l(m_);
                    cv_.wait(l, [&] { return quit_ || !q_.empty(); });
                    if (quit_) break;
                    auto pick = q_.begin();
                    for (auto it = q_.begin(); it != q_.end(); ++it) if (it->when > pick->when) pick = it;
                    j = std::move(*pick);
                    q_.erase(pick);
                }
                auto* r = new PicResult;
                r->key = j.key;
                r->ok = loader.load(j.path, j.side, r->pic);
                if (!PostMessageW(target, MSG_PICTURE, 0, (LPARAM)r)) delete r;
            }
        }
        if (com) CoUninitialize();
    }
};

// ---------------------------------------------------------------------------------------------
// The update thread: SF6 Mod Suite's latest release, looked up and installed off the window's thread
// ---------------------------------------------------------------------------------------------

enum { OPT_LANG = 0, OPT_SELF = 1, OPT_SUITE = 2, OPT_ROWS = 3 };   // the Options page's rows, top to bottom

struct UpdateJob {
    int kind = 0;                      // as UpdateResult's
    int gen = 0;
    std::wstring game_dir, manifest;
    bool asked = false;                // a look-up: the player pressed the key
    SuiteCheck check;                  // an install: what the look-up offered
    SelfCheck self;                    // the sorter's own: the same
    sf6up::Progress* progress = nullptr;   // an install: where it is, read by the window
    std::wstring assets;               // the sorter's own install, tests: its zip read from that folder
    std::string lang;
    bool running = false;              // an install: the game was running when it was asked
};

// Detached: a look-up still on the network when the window closes is left behind, never waited for (what it works on is
// shared with the window and outlives it). An install keeps the window open instead (WM_CLOSE).
class UpdateThread {
public:
    void start(HWND target) {
        s_->target = target;
        std::thread([s = s_] { loop(*s); }).detach();
    }
    void push(UpdateJob j) {
        { std::lock_guard<std::mutex> l(s_->m); s_->q.push_back(std::move(j)); }
        s_->cv.notify_one();
    }
    void stop() {
        { std::lock_guard<std::mutex> l(s_->m); s_->quit = true; }
        s_->cv.notify_one();
    }

private:
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        std::deque<UpdateJob> q;
        bool quit = false;
        HWND target = nullptr;
    };
    std::shared_ptr<Shared> s_ = std::make_shared<Shared>();

    static void loop(Shared& s) {
        for (;;) {
            UpdateJob j;
            {
                std::unique_lock<std::mutex> l(s.m);
                s.cv.wait(l, [&] { return s.quit || !s.q.empty(); });
                if (s.quit) return;
                j = std::move(s.q.front());
                s.q.pop_front();
            }
            auto* r = new UpdateResult;
            r->gen = j.gen;
            r->kind = j.kind;
            if (j.kind == 1) r->inst = suite_install(j.game_dir, j.check, j.lang, j.running, j.progress);
            else if (j.kind == 2) r->sinst = self_install(j.self, j.lang, j.progress, L"", j.assets);
            else r->check = suite_check(j.game_dir, j.asked, j.manifest, &r->self);
            std::lock_guard<std::mutex> l(s.m);
            if (s.quit || !PostMessageW(s.target, MSG_UPDATE, 0, (LPARAM)r)) delete r;
            if (s.quit) return;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Drawing resources
// ---------------------------------------------------------------------------------------------

struct Fonts {
    IDWriteTextFormat *row = nullptr, *cat = nullptr, *sub = nullptr, *band = nullptr, *label = nullptr, *value = nullptr,
                      *value_wrap = nullptr, *footer = nullptr, *key = nullptr, *caption = nullptr, *drop_sub = nullptr,
                      *drop_kinds = nullptr, *button = nullptr, *strip = nullptr, *title = nullptr, *drop_title = nullptr;
};

ID2D1Factory* g_d2d = nullptr;
IDWriteFactory* g_dw = nullptr;
Fonts g_f;

IDWriteTextFormat* make_format(float size, DWRITE_FONT_WEIGHT w, DWRITE_FONT_STRETCH st, bool wrap = false,
                               DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextFormat* f = nullptr;
    // Bahnschrift has Latin, Greek and Cyrillic; for the rest DirectWrite's fallback picks a font for the locale given here
    // (which Han shapes: Japanese, Simplified or Traditional Chinese)
    g_dw->CreateTextFormat(L"Bahnschrift", nullptr, w, DWRITE_FONT_STYLE_NORMAL, st, size, language_locale(), &f);
    if (!f) return nullptr;
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_EMERGENCY_BREAK : DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetTextAlignment(al);
    return f;
}

// The formats carry the language's locale (the font DirectWrite falls back to): made again when the language changes
bool make_fonts() {
    for (IDWriteTextFormat** f : {&g_f.row, &g_f.cat, &g_f.sub, &g_f.band, &g_f.label, &g_f.value, &g_f.value_wrap, &g_f.footer, &g_f.key,
                                  &g_f.caption, &g_f.drop_sub, &g_f.drop_kinds, &g_f.button, &g_f.strip, &g_f.title, &g_f.drop_title})
        release(*f);
    const auto SC = DWRITE_FONT_STRETCH_SEMI_CONDENSED, C = DWRITE_FONT_STRETCH_CONDENSED;
    g_f.row = make_format(19.5f, DWRITE_FONT_WEIGHT_NORMAL, SC);
    g_f.cat = make_format(16.f, DWRITE_FONT_WEIGHT_NORMAL, SC);
    g_f.sub = make_format(15.5f, DWRITE_FONT_WEIGHT_NORMAL, SC);
    g_f.band = make_format(17.f, DWRITE_FONT_WEIGHT_NORMAL, SC);
    g_f.label = make_format(16.5f, DWRITE_FONT_WEIGHT_NORMAL, SC);
    g_f.value = make_format(16.f, DWRITE_FONT_WEIGHT_NORMAL, SC);
    g_f.value_wrap = make_format(16.f, DWRITE_FONT_WEIGHT_NORMAL, SC, true);
    g_f.footer = make_format(17.f, DWRITE_FONT_WEIGHT_NORMAL, SC);
    g_f.key = make_format(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.caption = make_format(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, SC);
    g_f.drop_sub = make_format(17.5f, DWRITE_FONT_WEIGHT_NORMAL, SC, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.drop_kinds = make_format(15.f, DWRITE_FONT_WEIGHT_NORMAL, SC, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.button = make_format(19.5f, DWRITE_FONT_WEIGHT_NORMAL, SC, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_f.strip = make_format(16.f, DWRITE_FONT_WEIGHT_NORMAL, SC);
    g_f.title = make_format(23.f, DWRITE_FONT_WEIGHT_BOLD, SC);
    g_f.drop_title = make_format(34.f, DWRITE_FONT_WEIGHT_BOLD, SC);
    return g_f.row != nullptr;
}

bool init_graphics() {
    if (g_d2d) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_dw)))) return false;
    return make_fonts();
}

// ---------------------------------------------------------------------------------------------
// The application
// ---------------------------------------------------------------------------------------------

struct App {
    UiOptions opt;
    std::wstring game_dir;
    int gen = 0;                      // game folder generation: results of an older one are ignored
    bool loading = false, costume_ok = false, stage_ok = false;
    std::string costume_note, stage_note;
    std::vector<Item> items;
    int next_id = 1, seq = 0;
    int sel = 0;                      // selected item (index)
    bool sel_sub = false;             // its sub-row (the Skip / Replace choice)
    float list_scroll = 0, panel_scroll = 0, list_h = 0, list_view = 0, panel_h = 0, panel_view = 0;
    D2D1_RECT_F list_rect{}, panel_rect{};
    std::vector<Hit> hits;
    POINT mouse{-1, -1};
    double sel_t0 = 0;                // when the selection changed (the marquee waits a little)
    bool installing = false, install_done_note = false;
    int install_total = 0, install_count = 0;
    HWND hwnd = nullptr;
    Worker* worker = nullptr;
    ID2D1HwndRenderTarget* rt = nullptr;
    ClassifyContext shot_ctx;         // --shot: classification in line
    float W = 1180, H = 720;          // DIPs
    bool tracking = false;
    bool animating = false;           // the last frame drew something that moves (a marquee, the reading dots)
    int page = 0;                     // 0: To install, 1: Installed
    int cat = 0;                      // Installed: the selected category
    std::vector<InstCat> inst;
    bool inst_grid = false;           // Installed: the cursor is in the grid of pictures (else on the categories)
    int inst_sel = 0;                 // the selected cell there (in the order they are drawn)
    std::map<std::wstring, Pic> pics; // the mods' pictures asked for so far
    PicThread* pic_thread = nullptr;
    PictureLoader* shot_loader = nullptr;   // --shot: the pictures read in line
    bool game_running = false;        // StreetFighter6.exe is there (looked at every two seconds)
    double game_checked = -100;       // when it was last looked at
    double launch_t0 = -100;          // when Steam was last asked to start the game
    bool launch_failed = false;
    Msg inst_note;                    // Installed: what the last N did (a move, or why not), shown until the next key or click
    bool inst_note_error = false;
    UpdateThread* updater = nullptr;
    int opt_sel = 0;                  // Options: the selected row (OPT_LANG, OPT_SELF, OPT_SUITE)
    int up_busy = 0;                  // 1: the latest release is being looked up, 2: an update is being installed
    bool up_self = false;             // up_busy 2: the sorter's own update (else the suite's)
    bool self_restart = false;        // the sorter replaced itself: the new exe is started, this window closes
    sf6up::Progress up_progress;      // up_busy 2: where the install is (written by the update thread)
    SuiteCheck up_check;              // what the last look-up found
    SelfCheck self_check;             // the same for the sorter itself
    Msg up_note;                      // the update was installed: said until the next update action
    bool kept_zip = false;            // a zip kept by the updates (has_kept_zip), looked at on the Options page and after an update
    std::string up_fail;              // it was not: the updater's own sentence, in the language of the moment

    Item* find(int id) { for (auto& it : items) if (it.id == id) return &it; return nullptr; }
};

App g_app;

double now_s() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

std::wstring W8(const std::string& s) { return from_utf8(s); }
bool rtl_letter(wchar_t c) { return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) || (c >= 0xFE70 && c <= 0xFEFF); }

// A letter of a script without capitals (Hebrew, Arabic, kana, Han, Hangul): the game's titles keep one size there
bool uncased_letter(wchar_t c) {
    return rtl_letter(c) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x2E80 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFF66 && c <= 0xFFDC);
}

// The paragraph direction of a text: the one of its first letter (a mod's Latin name stays left to right in Arabic)
bool rtl_text(const std::wstring& s) {
    for (wchar_t c : s) {
        if (c == 0x200F || rtl_letter(c)) return true;
        if (c == 0x200E || IsCharAlphaW(c)) return false;
    }
    return false;
}

// One of the tool's own sentences: in Arabic it reads right to left even when it starts with "Slots Loader" or a file name
// (a right-to-left mark in front: no width, and the first "letter" rtl_text() and DirectWrite meet)
std::wstring marked(std::wstring s) {
    for (wchar_t c : s) if (rtl_letter(c)) { s.insert(s.begin(), L'\u200F'); break; }
    return s;
}

std::wstring T(TextId id) { return marked(from_utf8(tr(id))); }
// What is put into a right-to-left sentence (a file name in quotes, a path, a count) keeps its own direction and stays in
// one piece: an embedding of its own direction around each argument
std::vector<std::string> isolated(const std::vector<std::string>& a) {
    if (!language_rtl()) return a;
    std::vector<std::string> o;
    // U+202B / U+202A (right-to-left / left-to-right embedding) ... U+202C (pop)
    for (const std::string& s : a) o.push_back((rtl_text(from_utf8(s)) ? "\xE2\x80\xAB" : "\xE2\x80\xAA") + s + "\xE2\x80\xAC");
    return o;
}

std::wstring T(TextId id, const std::vector<std::string>& a) { return marked(from_utf8(tr(id, isolated(a)))); }
std::wstring T(const Msg& m) { return m.id == T__COUNT ? std::wstring() : marked(from_utf8(tr(m.id, isolated(m.args)))); }

std::wstring upper(const std::wstring& s) {
    std::wstring u = s;
    if (!u.empty()) LCMapStringEx(language_locale(), LCMAP_UPPERCASE, s.c_str(), (int)s.size(), &u[0], (int)u.size(), nullptr, nullptr, 0);
    return u;
}

bool item_selectable_sub(const Item& it) {
    return !it.reading && it.c.cat != Category::Unknown && it.exists && it.done != Item::Installed && it.done != Item::Installing;
}

bool item_has_sub(const Item& it) {
    if (it.reading) return false;
    if (it.c.cat == Category::Unknown) return true;
    return it.exists || it.done != Item::None;
}

void refresh_exists(App& a) {
    for (auto& it : a.items)
        it.exists = !it.reading && it.c.cat != Category::Unknown && it.done != Item::Installed &&
                    destination_exists(a.game_dir, it.c, it.nsfw);
    if (a.sel_sub && (a.sel >= (int)a.items.size() || !item_selectable_sub(a.items[a.sel]))) a.sel_sub = false;
}

void invalidate(App& a) { if (a.hwnd) InvalidateRect(a.hwnd, nullptr, FALSE); }

// ---- actions ----

void queue_classify(App& a, Item& it) {
    it.reading = true;
    it.done = Item::None;
    if (a.worker) {
        Job j;
        j.type = Job::Classify;
        j.gen = a.gen;
        j.item = it.id;
        j.path = it.path;
        j.seq = ++a.seq;
        a.worker->push(j);
    }
}

void add_paths(App& a, const std::vector<std::wstring>& paths) {
    for (auto& p : paths) {
        bool dup = false;
        for (auto& it : a.items) dup = dup || lower_w(it.path) == lower_w(p);
        if (dup) continue;
        Item it;
        it.id = a.next_id++;
        it.path = p;
        it.c.path = p;
        std::wstring q = p;
        while (!q.empty() && (q.back() == L'\\' || q.back() == L'/')) q.pop_back();
        it.c.name = q.substr(q.find_last_of(L"\\/") + 1);
        a.items.push_back(it);
        queue_classify(a, a.items.back());
    }
    a.install_done_note = false;
    invalidate(a);
}

void scan_installed(App& a);
void queue_update_check(App& a, bool asked);

void set_game_dir(App& a, const std::wstring& dir) {
    a.game_dir = dir;
    if (a.page == 1) scan_installed(a);
    ++a.gen;
    a.loading = !dir.empty();
    a.costume_ok = a.stage_ok = false;
    if (a.worker) {
        Job j;
        j.type = Job::Init;
        j.gen = a.gen;
        j.game_dir = dir;
        a.worker->push(j);
        for (auto& it : a.items) if (it.done != Item::Installed) queue_classify(a, it);
    }
    a.up_check = SuiteCheck();        // another folder: what is installed there is looked at again (an older answer is ignored)
    a.up_busy = 0;
    queue_update_check(a, false);
    invalidate(a);
}

void select(App& a, int i, bool sub) {
    if (a.items.empty()) return;
    i = std::max(0, std::min(i, (int)a.items.size() - 1));
    if (i != a.sel || sub != a.sel_sub) { a.sel_t0 = now_s(); a.panel_scroll = 0; }
    a.sel = i;
    a.sel_sub = sub && item_selectable_sub(a.items[i]);
}

void change_value(App& a, int i, bool sub, int dir) {
    if (i < 0 || i >= (int)a.items.size()) return;
    Item& it = a.items[i];
    (void)dir;
    if (sub) {
        if (item_selectable_sub(it)) { it.replace = !it.replace; if (it.done == Item::Skipped) it.done = Item::None; }
    } else if (!it.reading && it.c.cat == Category::Costume && it.done != Item::Installed && it.done != Item::Installing) {
        it.nsfw = !it.nsfw;
        if (it.done == Item::Skipped || it.done == Item::Failed) it.done = Item::None;
        refresh_exists(a);
    }
    invalidate(a);
}

void install_all(App& a) {
    if (a.installing || a.game_dir.empty() || a.up_busy == 2) return;   // not while the suite itself is being replaced
    int n = 0;
    for (auto& it : a.items) {
        if (it.reading || it.c.cat == Category::Unknown || it.done == Item::Installed) continue;
        if (it.exists && !it.replace) { it.done = Item::Skipped; continue; }
        it.done = Item::Installing;
        it.progress = 0;
        if (a.worker) {
            Job j;
            j.type = Job::Place;
            j.item = it.id;
            j.c = it.c;
            j.game_dir = a.game_dir;
            j.nsfw = it.nsfw;
            j.replace = it.exists && it.replace;
            a.worker->push(j);
        }
        ++n;
    }
    a.install_total = n;
    a.install_count = 0;
    a.installing = n > 0;
    a.install_done_note = n == 0;
    invalidate(a);
}

void remove_selected(App& a) {
    if (a.items.empty() || a.sel >= (int)a.items.size()) return;
    if (a.items[a.sel].done == Item::Installing) return;
    a.items.erase(a.items.begin() + a.sel);
    a.sel_sub = false;
    if (a.sel >= (int)a.items.size()) a.sel = std::max(0, (int)a.items.size() - 1);
    a.sel_t0 = now_s();
    invalidate(a);
}

void browse(App& a) {
    if (!a.hwnd) return;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    d->SetTitle(T(D_ADD).c_str());
    COMDLG_FILTERSPEC spec[] = { { L"Mods (*.zip; *.7z; *.rar; *.pak; *.user.2)", L"*.zip;*.7z;*.rar;*.pak;*.user.2" }, { L"*.*", L"*.*" } };
    d->SetFileTypes(2, spec);
    std::vector<std::wstring> paths;
    if (SUCCEEDED(d->Show(a.hwnd))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(d->GetResults(&arr)) && arr) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* si = nullptr;
                if (FAILED(arr->GetItemAt(i, &si))) continue;
                PWSTR p = nullptr;
                if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { paths.push_back(p); CoTaskMemFree(p); }
                si->Release();
            }
            arr->Release();
        }
    }
    d->Release();
    if (!paths.empty()) add_paths(a, paths);
}

void pick_game(App& a) {
    if (!a.hwnd || a.up_busy == 2) return;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(T(D_PICK_GAME).c_str());
    if (!a.game_dir.empty()) {
        IShellItem* f = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(a.game_dir.c_str(), nullptr, IID_PPV_ARGS(&f))) && f) { d->SetFolder(f); f->Release(); }
    }
    std::wstring chosen;
    if (SUCCEEDED(d->Show(a.hwnd))) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(d->GetResult(&si)) && si) {
            PWSTR p = nullptr;
            if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { chosen = p; CoTaskMemFree(p); }
            si->Release();
        }
    }
    d->Release();
    if (chosen.empty()) return;
    if (!is_game_dir(chosen)) {
        MessageBoxW(a.hwnd, T(D_NOT_GAME).c_str(), T(T_TITLE).c_str(),
                    MB_OK | MB_ICONWARNING | (language_rtl() ? MB_RTLREADING | MB_RIGHT : 0));
        return;
    }
    save_settings_game_dir(chosen);
    set_game_dir(a, chosen);
}

// ---- the Installed page ----

int compare_names(const std::wstring& x, const std::wstring& y) {
    return CompareStringEx(LOCALE_NAME_INVARIANT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, x.c_str(), (int)x.size(), y.c_str(),
                           (int)y.size(), nullptr, nullptr, 0) - CSTR_EQUAL;
}

// What a mod folder holds: its folders and its mod files, sorted. The loaders' own files (README, settings, caches, the
// folders whose name starts with a dot) are not mods.
void list_mods(const std::wstring& dir, bool colours, std::vector<InstEntry>& folders, std::vector<InstEntry>& files) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(long_path_w(dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n.empty() || n[0] == L'.') continue;
        const uint64_t stamp = ((uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime) ^
                               ((uint64_t(fd.nFileSizeLow) << 32) | fd.nFileSizeHigh);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { folders.push_back({n, stamp}); continue; }
        const std::wstring l = lower_w(n);
        auto ends = [&](const wchar_t* e) { const size_t k = wcslen(e); return l.size() > k && l.compare(l.size() - k, k, e) == 0; };
        if (ends(L".zip") || ends(L".7z") || ends(L".rar") || ends(L".pak") || (colours && ends(L".user.2"))) files.push_back({n, stamp});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    auto less = [](const InstEntry& x, const InstEntry& y) { return compare_names(x.name, y.name) < 0; };
    std::sort(folders.begin(), folders.end(), less);
    std::sort(files.begin(), files.end(), less);
}

void scan_installed(App& a) {
    static const struct { TextId label; const wchar_t* dir; bool by_char, colours; } kCats[] = {
        { T_I_COSTUMES, L"costume_mods", true, false }, { T_I_COSTUMES_NSFW, L"costume_mods_nsfw", true, false },
        { T_I_STAGES, L"stage_mods", false, false },    { T_I_UI, L"ui_mods", false, false },
        { T_I_TITLE, L"title_mods", false, false },     { T_CAT_COLOURS, L"color_mods", true, true },
    };
    a.inst.clear();
    for (auto& k : kCats) {
        InstCat c;
        c.label = k.label;
        c.dir = k.dir;
        c.pictures = !k.colours;
        const std::wstring dir = a.game_dir + L"\\reframework\\" + k.dir;
        const DWORD at = a.game_dir.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(long_path_w(dir).c_str());
        c.present = at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
        if (c.present) {
            const auto by_name = [](const InstEntry& x, const InstEntry& y) { return compare_names(x.name, y.name) < 0; };
            std::vector<InstEntry> folders, files;
            list_mods(dir, k.colours, folders, files);
            if (k.by_char) {
                // one folder per character; a mod at the root is for several characters
                InstGroup all, several;
                all.kind = InstGroup::AllChars;
                several.kind = InstGroup::Several;
                several.entries = files;
                for (auto& fe : folders) {
                    const std::wstring& f = fe.name;
                    InstGroup g;
                    g.kind = InstGroup::Character;
                    g.name = g.folder = f;
                    std::vector<InstEntry> in_folders, in_files;
                    list_mods(dir + L"\\" + f, k.colours, in_folders, in_files);
                    g.entries = in_folders;
                    g.entries.insert(g.entries.end(), in_files.begin(), in_files.end());
                    std::sort(g.entries.begin(), g.entries.end(), by_name);
                    if (g.entries.empty()) continue;
                    if (lower_w(f) == L"all_chars") { all.entries = g.entries; all.folder = f; continue; }
                    c.groups.push_back(g);
                }
                if (!all.entries.empty()) c.groups.insert(c.groups.begin(), all);
                if (!several.entries.empty()) c.groups.insert(c.groups.begin(), several);
            } else {
                InstGroup g;
                g.entries = folders;
                g.entries.insert(g.entries.end(), files.begin(), files.end());
                std::sort(g.entries.begin(), g.entries.end(), by_name);
                if (!g.entries.empty()) c.groups.push_back(g);
            }
            for (auto& g : c.groups) c.count += (int)g.entries.size();
        }
        a.inst.push_back(c);
    }
    a.cat = std::max(0, std::min(a.cat, (int)a.inst.size() - 1));
}

std::wstring entry_path(const App& a, const InstCat& c, const InstGroup& g, const InstEntry& e) {
    return a.game_dir + L"\\reframework\\" + c.dir + L"\\" + (g.folder.empty() ? std::wstring() : g.folder + L"\\") + e.name;
}

// The Installed page's panel: where it is in the window, and the grid of pictures of the selected category (a section per
// group, its heading first; y from the top of what scrolls). The same for the drawing and for the cursor's moves.
struct InstPanel { float px, pr, top, y0, bottom, lx, gx1; };
InstPanel inst_panel(const App& a) {
    InstPanel p;
    p.px = 36.f + std::floor((a.W - 72.f) * 0.33f) + 48.f;
    p.pr = a.W - 36.f;
    p.top = 108.f;
    p.y0 = p.top + 40.f;
    p.bottom = a.H - 100.f;
    p.lx = p.px + 22.f;
    p.gx1 = p.pr - 16.f;
    return p;
}
struct GridCell { D2D1_RECT_F r; int group = 0, entry = 0; };
struct GridHead { float y = 0; int group = 0; };
struct Grid {
    std::vector<GridCell> cells;
    std::vector<GridHead> heads;
    float height = 0, pic_h = 0;
};
const float kHeadH = 34.f, kNameH = 30.f;

Grid grid_layout(const App& a) {
    Grid g;
    if (a.cat < 0 || a.cat >= (int)a.inst.size() || !a.inst[a.cat].pictures) return g;
    const InstPanel p = inst_panel(a);
    const float gap = 12.f, min_w = 196.f;
    const int cols = std::max(1, int((p.gx1 - p.lx + gap) / (min_w + gap)));
    const float cw = std::floor((p.gx1 - p.lx - gap * (cols - 1)) / cols);
    g.pic_h = std::floor(cw * 9.f / 16.f);
    const float ch = g.pic_h + kNameH;
    const InstCat& c = a.inst[a.cat];
    float y = 0;
    for (int gi = 0; gi < (int)c.groups.size(); ++gi) {
        const InstGroup& gr = c.groups[gi];
        if (gr.entries.empty()) continue;
        if (gr.kind != InstGroup::Flat) { g.heads.push_back({y, gi}); y += kHeadH; }
        for (int ei = 0; ei < (int)gr.entries.size(); ++ei) {
            const int col = ei % cols;
            if (ei && !col) y += ch + gap;
            const float x = p.lx + col * (cw + gap);
            g.cells.push_back({D2D1::RectF(x, y, x + cw, y + ch), gi, ei});
        }
        y += ch + 18.f;
    }
    g.height = y;
    return g;
}

void ensure_cell_visible(App& a, const Grid& g) {
    if (a.inst_sel < 0 || a.inst_sel >= (int)g.cells.size()) return;
    const InstPanel p = inst_panel(a);
    const D2D1_RECT_F& r = g.cells[a.inst_sel].r;
    const float view = p.bottom - p.y0, top = r.top - kHeadH - 2.f;   // with its section's heading when it is right above
    if (r.bottom + 10.f > a.panel_scroll + view) a.panel_scroll = r.bottom + 10.f - view;
    if (top < a.panel_scroll) a.panel_scroll = std::max(0.f, top);
}

// Right from the categories: the cursor goes into the grid
void enter_grid(App& a) {
    const Grid g = grid_layout(a);
    if (g.cells.empty()) return;
    a.inst_grid = true;
    a.inst_sel = std::max(0, std::min(a.inst_sel, (int)g.cells.size() - 1));
    a.sel_t0 = now_s();
    ensure_cell_visible(a, g);
    invalidate(a);
}

void leave_grid(App& a) {
    a.inst_grid = false;
    a.sel_t0 = now_s();
    invalidate(a);
}

// The arrows in the grid. Left from the first column: back to the categories.
void move_cell(App& a, int dx, int dy) {
    const Grid g = grid_layout(a);
    const int n = (int)g.cells.size();
    if (!n) { leave_grid(a); return; }
    int i = std::max(0, std::min(a.inst_sel, n - 1));
    const D2D1_RECT_F cur = g.cells[i].r;
    if (dx) {
        const int j = i + dx;
        if (j >= 0 && j < n && g.cells[j].r.top == cur.top && g.cells[j].group == g.cells[i].group) i = j;
        else if (dx < 0) { leave_grid(a); return; }
    } else {
        // the nearest cell of the row above or below (the next section's when this one ends)
        int best = -1;
        for (int j = i + dy; j >= 0 && j < n; j += dy) {
            const GridCell& c = g.cells[j];
            if (c.r.top == cur.top) continue;
            if (best >= 0 && c.r.top != g.cells[best].r.top) break;
            if (best < 0 || std::fabs(c.r.left - cur.left) < std::fabs(g.cells[best].r.left - cur.left)) best = j;
        }
        if (best >= 0) i = best;
    }
    if (i != a.inst_sel) a.sel_t0 = now_s();
    a.inst_sel = i;
    ensure_cell_visible(a, g);
    invalidate(a);
}

// The bitmaps belong to a render target: released with it (the pictures still being read stay asked for)
void drop_pictures(App& a, bool all) {
    for (auto it = a.pics.begin(); it != a.pics.end();) {
        release(it->second.bmp);
        if (all || it->second.state != Pic::Loading) it = a.pics.erase(it);
        else ++it;
    }
}

// The zips the updates keep (SF6_Update_data\\downloads): O on the Options page, Explorer with the newest one selected
bool has_kept_zip(const App& a) {
    return !a.game_dir.empty() && !sf6up::newest_download(sf6up::Paths(a.game_dir)).empty();
}
void show_zip(App& a) {
    if (has_kept_zip(a)) sf6up::show_downloads(sf6up::Paths(a.game_dir));
}

void set_page(App& a, int page) {
    page = ((page % 3) + 3) % 3;   // 0: To install, 1: Installed, 2: Options; the tabs wrap
    if (page == a.page) return;
    a.page = page;
    a.inst_grid = false;
    a.panel_scroll = 0;
    a.sel_t0 = now_s();
    if (page == 1) scan_installed(a);
    if (page == 2) a.kept_zip = has_kept_zip(a);
    invalidate(a);
}

// The Options page's Language row: the next or the previous language (they wrap), shown at once and remembered
void change_language(App& a, int dir) {
    set_language_index((language_index() + dir + kLangCount) % kLangCount);
    make_fonts();
    save_settings_language(language());
    if (a.hwnd) SetWindowTextW(a.hwnd, (T(T_TITLE) + L" \u2014 SF6").c_str());
    a.sel_t0 = now_s();
    invalidate(a);
}

void select_category(App& a, int i) {
    if (a.inst.empty()) return;
    i = std::max(0, std::min(i, (int)a.inst.size() - 1));
    if (i != a.cat) { a.panel_scroll = 0; a.sel_t0 = now_s(); a.inst_sel = 0; }
    a.cat = i;
    a.inst_grid = false;
    invalidate(a);
}

// The selected category's folder, in Explorer; the cursor on a mod's picture: its folder, the mod selected there
void open_category(App& a) {
    if (!a.hwnd || a.cat < 0 || a.cat >= (int)a.inst.size() || !a.inst[a.cat].present) return;
    if (a.inst_grid) {
        const InstCat& c = a.inst[a.cat];
        const Grid g = grid_layout(a);
        if (a.inst_sel >= 0 && a.inst_sel < (int)g.cells.size()) {
            const InstGroup& gr = c.groups[g.cells[a.inst_sel].group];
            const std::wstring arg = L"/select,\"" + entry_path(a, c, gr, gr.entries[g.cells[a.inst_sel].entry]) + L"\"";
            ShellExecuteW(a.hwnd, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
    }
    const std::wstring dir = a.game_dir + L"\\reframework\\" + a.inst[a.cat].dir;
    ShellExecuteW(a.hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The cursor on a mod's picture in one of the two costume categories: N moves it to the other one
bool nsfw_movable(const App& a) {
    if (a.page != 1 || !a.inst_grid || a.cat < 0 || a.cat >= (int)a.inst.size()) return false;
    const std::wstring& d = a.inst[a.cat].dir;
    return (d == L"costume_mods" || d == L"costume_mods_nsfw") && a.inst_sel >= 0 && a.inst_sel < (int)grid_layout(a).cells.size();
}

// N: the selected costume goes to the other costume folder (costume_mods <-> costume_mods_nsfw), same character folder, by a
// rename. The cursor stays in the category, on the cell that takes the moved one's place (the categories when none is left);
// the footer's note says where it went, or why it did not move (nothing changed then).
void move_nsfw_selected(App& a) {
    if (!nsfw_movable(a)) return;
    const InstCat& c = a.inst[a.cat];
    const GridCell gc = grid_layout(a).cells[a.inst_sel];
    const InstGroup& gr = c.groups[gc.group];
    const MoveResult r = move_costume_root(a.game_dir, c.dir == L"costume_mods_nsfw", gr.folder, gr.entries[gc.entry].name);
    a.inst_note_error = !r.ok;
    a.inst_note = r.ok ? Msg{T_NOTE_MOVED, {to_utf8(r.dest.substr(0, r.dest.find_last_of(L'\\')))}} : r.error;
    if (r.ok) {
        scan_installed(a);
        const Grid g = grid_layout(a);
        if (g.cells.empty()) a.inst_grid = false;
        else {
            a.inst_sel = std::min(a.inst_sel, (int)g.cells.size() - 1);
            ensure_cell_visible(a, g);
        }
    }
    a.sel_t0 = now_s();
    invalidate(a);
}

// ---- the game: running or not, started through Steam ----

// True when the state changed. --running forces it (the tests' pictures).
bool check_game(App& a, bool force = false) {
    const double t = now_s();
    if (!force && t - a.game_checked < 2.0) return false;
    a.game_checked = t;
    const bool r = a.opt.running >= 0 ? a.opt.running == 1 : game_process_running();
    if (r == a.game_running) return false;
    a.game_running = r;
    if (r) a.launch_t0 = -100;
    return true;
}

// Steam was asked a moment ago and the game is not there yet
bool game_starting(const App& a) { return a.opt.running == 2 || (!a.game_running && now_s() - a.launch_t0 < 20.0); }

void launch_game(App& a) {
    if (!a.hwnd || a.game_running || game_starting(a) || a.up_busy == 2) return;
    const HINSTANCE r = ShellExecuteW(a.hwnd, L"open", L"steam://rungameid/1364780", nullptr, nullptr, SW_SHOWNORMAL);
    a.launch_failed = reinterpret_cast<INT_PTR>(r) <= 32;
    if (!a.launch_failed) a.launch_t0 = now_s();
    invalidate(a);
}

// ---- SF6 Mod Suite's own updates (suite_update.hpp) ----

// asked: F on the Options page's row (looked up even when the automatic check is turned off in the game's state.json)
void queue_update_check(App& a, bool asked) {
    if (!a.updater || a.up_busy) return;
    a.up_note = Msg();
    a.up_fail.clear();
    if (a.game_dir.empty()) a.up_check = SuiteCheck();   // no game folder: the sorter's own version is still looked up
    UpdateJob j;
    j.gen = a.gen;
    j.game_dir = a.game_dir;
    j.manifest = a.opt.manifest;
    j.asked = asked;
    a.up_busy = 1;
    a.updater->push(std::move(j));
    invalidate(a);
}

// What F does on the suite's row: the install when a release waits and nothing forbids it (the game running, mods being copied)
bool update_installable(const App& a) {
    return a.up_check.state == SuiteCheck::Available && a.up_check.plan && !a.game_running && !a.installing;
}
// On the sorter's own row: the game may run (the exe is the only file replaced), mods must not be on their way (it starts again)
bool self_installable(const App& a) {
    return a.self_check.state == SelfCheck::Available && a.self_check.plan && !a.installing;
}

void update_action(App& a) {
    if (!a.updater || a.up_busy || a.opt_sel == OPT_LANG) return;
    const bool self = a.opt_sel == OPT_SELF;
    if (!self && a.game_dir.empty()) return;
    if (self ? !self_installable(a) : !update_installable(a)) { queue_update_check(a, true); return; }
    a.up_note = Msg();
    a.up_fail.clear();
    a.up_progress.reset();
    UpdateJob j;
    j.kind = self ? 2 : 1;
    j.gen = a.gen;
    j.game_dir = a.game_dir;
    j.check = a.up_check;
    j.self = a.self_check;
    j.lang = language();
    j.running = a.game_running;
    j.progress = &a.up_progress;
    j.assets = a.opt.assets;
    a.up_self = self;
    a.up_busy = 2;
    a.updater->push(std::move(j));
    invalidate(a);
}

void apply_update(App& a, UpdateResult& r) {
    a.up_busy = 0;
    if (r.kind == 0) { a.up_check = std::move(r.check); a.self_check = std::move(r.self); return; }
    const bool ok = r.kind == 2 ? r.sinst.ok : r.inst.ok;
    if (!ok) {
        a.up_fail = r.kind == 2 ? r.sinst.message : r.inst.message;   // it stays offered: F tries again
        for (char& c : a.up_fail) if (c == '\n') c = ' ';
    } else if (r.kind == 2) {
        a.self_check.state = SelfCheck::Current;   // the exe on disk; this window is still the old one: it hands over (MSG_UPDATE)
        a.self_check.current = r.sinst.version;
        a.self_check.plan.reset();
        a.up_note = Msg{T_NOTE_SELF_UPDATED, {r.sinst.version}};
        a.self_restart = true;
    } else {
        a.up_check.state = SuiteCheck::Current;
        a.up_check.latest = r.inst.version;
        a.up_check.lines.clear();
        a.up_check.plan.reset();
        a.up_check.ignored = false;
        a.up_note = Msg{T_NOTE_UPDATED, {r.inst.version}};
        a.kept_zip = has_kept_zip(a);
    }
}

// The exe as it is on disk now, started: after the sorter replaced itself
bool start_self() {
    const std::wstring exe = self_exe_path();
    if (exe.empty()) return false;
    std::wstring cmd = L"\"" + exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void select_option(App& a, int row) {
    if (a.opt_sel != row) { a.opt_sel = row; a.sel_t0 = now_s(); }
    invalidate(a);
}

// ---------------------------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------------------------

struct Painter {
    ID2D1RenderTarget* rt;
    ID2D1SolidColorBrush* br = nullptr;
    App& a;
    double t;

    Painter(ID2D1RenderTarget* r, App& app, double time) : rt(r), a(app), t(time) { rt->CreateSolidColorBrush(rgb(0xFFFFFF), &br); }
    ~Painter() { release(br); }

    ID2D1SolidColorBrush* B(uint32_t c, float al = 1.f) { br->SetColor(rgb(c, al)); return br; }

    void fill(const D2D1_RECT_F& r, uint32_t c, float al = 1.f) { rt->FillRectangle(r, B(c, al)); }
    void line(float x0, float y0, float x1, float y1, uint32_t c, float al = 1.f, float w = 1.f) {
        rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), B(c, al), w);
    }

    ID2D1PathGeometry* poly(std::initializer_list<D2D1_POINT_2F> pts) {
        ID2D1PathGeometry* g = nullptr;
        g_d2d->CreatePathGeometry(&g);
        ID2D1GeometrySink* s = nullptr;
        g->Open(&s);
        auto it = pts.begin();
        s->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
        for (++it; it != pts.end(); ++it) s->AddLine(*it);
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        s->Release();
        return g;
    }

    IDWriteTextLayout* layout(const std::wstring& s, IDWriteTextFormat* f, float w = 10000.f, float h = 100.f) {
        IDWriteTextLayout* l = nullptr;
        g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, std::max(1.f, w), std::max(1.f, h), &l);
        if (l && rtl_text(s)) {
            l->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
            const DWRITE_TEXT_ALIGNMENT al = f->GetTextAlignment();
            if (al == DWRITE_TEXT_ALIGNMENT_LEADING) l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            else if (al == DWRITE_TEXT_ALIGNMENT_TRAILING) l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        return l;
    }
    float text_w(const std::wstring& s, IDWriteTextFormat* f) {
        IDWriteTextLayout* l = layout(s, f);
        if (!l) return 0;
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        l->Release();
        return m.widthIncludingTrailingWhitespace;
    }
    float text_h(const std::wstring& s, IDWriteTextFormat* f, float w) {
        IDWriteTextLayout* l = layout(s, f, w, 10000.f);
        if (!l) return 0;
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        l->Release();
        return m.height;
    }
    void text(const std::wstring& s, IDWriteTextFormat* f, float x, float y, float w, float h, uint32_t c, float al = 1.f) {
        IDWriteTextLayout* l = layout(s, f, w, h);
        if (!l) return;
        rt->DrawTextLayout(D2D1::Point2F(x, y), l, B(c, al), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        l->Release();
    }

    // The game's page titles: capitals, the first letter of each word taller ("PARAMETRES DE COMBAT")
    float small_caps(const std::wstring& s, IDWriteTextFormat* f, float big_size, float small_size, float x, float y, uint32_t c,
                     float spacing, bool centre = false, bool draw = true) {
        const std::wstring u = upper(s);
        IDWriteTextLayout* l = layout(u, f, 10000.f, big_size * 1.6f);
        if (!l) return 0;
        bool uncased = false;
        for (wchar_t ch : u) if (uncased_letter(ch)) { uncased = true; break; }
        if (uncased) {
            // No capitals to play with (Japanese, Chinese, Korean, Arabic): the whole title at one size, between the two
            l->SetFontSize(small_size + (big_size - small_size) * 0.55f, DWRITE_TEXT_RANGE{0, (UINT32)u.size()});
            if (rtl_text(u)) spacing = 0;   // Arabic letters join: spacing would break the words
        } else {
            bool word_start = true;
            for (UINT32 i = 0; i < u.size(); ++i) {
                const bool space = u[i] == L' ' || u[i] == L'-';   // "MOD-SORTIERER": two words
                if (!space && !word_start) l->SetFontSize(small_size, DWRITE_TEXT_RANGE{i, 1});
                word_start = space;
            }
        }
        IDWriteTextLayout1* l1 = nullptr;
        if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&l1))) && l1) {
            l1->SetCharacterSpacing(0, spacing, 0, DWRITE_TEXT_RANGE{0, (UINT32)u.size()});
            l1->Release();
        }
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        const float w = m.widthIncludingTrailingWhitespace - spacing;
        if (draw) rt->DrawTextLayout(D2D1::Point2F(centre ? x - w / 2 : x, y), l, B(c), D2D1_DRAW_TEXT_OPTIONS_NONE);
        l->Release();
        return w;
    }

    // A thin line with the small squares the game puts at its ends
    void rule(float x0, float y, float x1, float al = 0.85f) {
        line(x0, y + 0.5f, x1, y + 0.5f, kLine, al);
        fill(D2D1::RectF(x0 - 1.5f, y - 1.f, x0 + 1.5f, y + 2.f), 0xD8D8D8, al);
        fill(D2D1::RectF(x1 - 1.5f, y - 1.f, x1 + 1.5f, y + 2.f), 0xD8D8D8, al);
    }
    void vrule(float x, float y0, float y1, float al = 0.75f) {
        line(x + 0.5f, y0, x + 0.5f, y1, kLine, al);
        fill(D2D1::RectF(x - 1.f, y0 - 1.5f, x + 2.f, y0 + 1.5f), 0xD8D8D8, al);
        fill(D2D1::RectF(x - 1.f, y1 - 1.5f, x + 2.f, y1 + 1.5f), 0xD8D8D8, al);
    }

    // ---- the background: deep navy, lighter towards the right, two faint diagonal light shafts ----
    void background() {
        ID2D1LinearGradientBrush* g = nullptr;
        ID2D1GradientStopCollection* st = nullptr;
        D2D1_GRADIENT_STOP s[3] = { {0.f, rgb(kBgA)}, {0.55f, rgb(0x0C1022)}, {1.f, rgb(kBgB)} };
        rt->CreateGradientStopCollection(s, 3, &st);
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(a.W, a.H)), st, &g);
        rt->FillRectangle(D2D1::RectF(0, 0, a.W, a.H), g);
        release(g);
        release(st);
        auto shaft = [&](float x0, float x1, float x2, float x3, uint32_t c, float al) {
            ID2D1PathGeometry* p = poly({D2D1::Point2F(x0, 0), D2D1::Point2F(x1, 0), D2D1::Point2F(x2, a.H), D2D1::Point2F(x3, a.H)});
            rt->FillGeometry(p, B(c, al));
            release(p);
        };
        shaft(a.W * 0.62f, a.W * 0.80f, a.W * 0.52f, a.W * 0.34f, 0x3A4A86, 0.10f);
        shaft(a.W * 0.86f, a.W * 1.02f, a.W * 0.80f, a.W * 0.66f, 0x4A5A9A, 0.09f);
        shaft(a.W * 0.20f, a.W * 0.27f, a.W * 0.05f, a.W * -0.02f, 0x33407A, 0.06f);
        // a soft glow at the top right, as the game's page is lit from there
        ID2D1RadialGradientBrush* rg = nullptr;
        D2D1_GRADIENT_STOP rs[2] = { {0.f, rgb(0x4B5CA0, 0.22f)}, {1.f, rgb(0x4B5CA0, 0.f)} };
        rt->CreateGradientStopCollection(rs, 2, &st);
        rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(a.W * 0.92f, a.H * 0.05f), D2D1::Point2F(0, 0),
                                                                          a.W * 0.55f, a.H * 0.8f), st, &rg);
        rt->FillRectangle(D2D1::RectF(0, 0, a.W, a.H), rg);
        release(rg);
        release(st);
    }

    // ---- the selected look: white to light blue, faint crystal facets, the blue bar at the left ----
    void selected_fill(ID2D1Geometry* shape, const D2D1_RECT_F& r, bool bar = true) {
        ID2D1LinearGradientBrush* g = nullptr;
        ID2D1GradientStopCollection* st = nullptr;
        D2D1_GRADIENT_STOP s[4] = { {0.f, rgb(0xEEF2F7)}, {0.45f, rgb(0xD4E3F8)}, {0.78f, rgb(0xEAF1F9)}, {1.f, rgb(0xD5E1EF)} };
        rt->CreateGradientStopCollection(s, 4, &st);
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.bottom)), st, &g);
        rt->FillGeometry(shape, g);
        release(g);
        release(st);
        ID2D1Layer* layer = nullptr;
        rt->CreateLayer(&layer);
        rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape), layer);
        const float w = r.right - r.left, h = r.bottom - r.top;
        auto facet = [&](float ax, float ay, float bx, float by, float cx, float cy, uint32_t c, float al) {
            ID2D1PathGeometry* p = poly({D2D1::Point2F(r.left + ax * w, r.top + ay * h), D2D1::Point2F(r.left + bx * w, r.top + by * h),
                                         D2D1::Point2F(r.left + cx * w, r.top + cy * h)});
            rt->FillGeometry(p, B(c, al));
            release(p);
        };
        facet(0.05f, 0.f, 0.32f, 0.f, 0.18f, 1.f, 0xFFFFFF, 0.45f);
        facet(0.30f, 1.f, 0.52f, 0.f, 0.60f, 1.f, 0xBFD3F2, 0.35f);
        facet(0.58f, 0.f, 0.78f, 0.f, 0.70f, 1.f, 0xFFFFFF, 0.55f);
        facet(0.80f, 1.f, 0.92f, 0.f, 1.02f, 1.f, 0xC6D6EE, 0.40f);
        facet(0.40f, 0.f, 0.47f, 0.f, 0.36f, 0.55f, 0xFFFFFF, 0.35f);
        rt->PopLayer();
        release(layer);
        if (bar) fill(D2D1::RectF(r.left, r.top, r.left + 5.f, r.bottom), kBar);
    }

    // ---- a row: the bevelled dark cell, or the selected look ----
    void row_shell(const D2D1_RECT_F& r, bool selected, bool hover, float cut = 9.f, bool darker = false) {
        if (selected) {
            ID2D1PathGeometry* p = poly({D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.top), D2D1::Point2F(r.right, r.bottom - cut),
                                         D2D1::Point2F(r.right - cut, r.bottom), D2D1::Point2F(r.left, r.bottom)});
            selected_fill(p, r);
            release(p);
            return;
        }
        ID2D1PathGeometry* p = poly({D2D1::Point2F(r.left + cut, r.top), D2D1::Point2F(r.right, r.top), D2D1::Point2F(r.right, r.bottom - cut),
                                     D2D1::Point2F(r.right - cut, r.bottom), D2D1::Point2F(r.left, r.bottom), D2D1::Point2F(r.left, r.top + cut)});
        ID2D1LinearGradientBrush* g = nullptr;
        ID2D1GradientStopCollection* st = nullptr;
        D2D1_GRADIENT_STOP s[2] = { {0.f, rgb(darker ? 0x18191D : kRowA, 0.97f)}, {1.f, rgb(darker ? 0x131417 : kRowB, 0.97f)} };
        rt->CreateGradientStopCollection(s, 2, &st);
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.left, r.bottom)), st, &g);
        rt->FillGeometry(p, g);
        rt->DrawGeometry(p, B(hover ? 0x9EA2AE : kRowEdge, hover ? 0.9f : 0.75f), 1.f);
        release(g);
        release(st);
        release(p);
    }

    // ---- the value control: < value > ----
    void chevron(float cx, float cy, bool left, uint32_t c, float s = 1.f) {
        const float dx = 4.5f * s, dy = 7.5f * s;
        ID2D1PathGeometry* p = left ? poly({D2D1::Point2F(cx + dx, cy - dy), D2D1::Point2F(cx + dx - 2.6f * s, cy - dy),
                                             D2D1::Point2F(cx - dx, cy), D2D1::Point2F(cx + dx - 2.6f * s, cy + dy), D2D1::Point2F(cx + dx, cy + dy),
                                             D2D1::Point2F(cx - dx + 2.6f * s, cy)})
                                    : poly({D2D1::Point2F(cx - dx, cy - dy), D2D1::Point2F(cx - dx + 2.6f * s, cy - dy),
                                             D2D1::Point2F(cx + dx, cy), D2D1::Point2F(cx - dx + 2.6f * s, cy + dy), D2D1::Point2F(cx - dx, cy + dy),
                                             D2D1::Point2F(cx + dx - 2.6f * s, cy)});
        rt->FillGeometry(p, B(c));
        release(p);
    }
    void value_control(const D2D1_RECT_F& r, const std::wstring& v, bool selected, bool enabled, int item, bool sub, uint32_t vc,
                       const wchar_t* locale = nullptr) {
        const float cy = (r.top + r.bottom) / 2;
        const uint32_t ac = selected ? kDark : 0xC4C4C4;
        if (enabled) {
            chevron(r.left + 12.f, cy, true, ac);
            chevron(r.right - 12.f, cy, false, ac);
            a.hits.push_back({D2D1::RectF(r.left, r.top, r.left + 30.f, r.bottom), A_LEFT, item, sub});
            a.hits.push_back({D2D1::RectF(r.right - 30.f, r.top, r.right, r.bottom), A_RIGHT, item, sub});
            a.hits.push_back({D2D1::RectF(r.left + 30.f, r.top, r.right - 30.f, r.bottom), A_TOGGLE, item, sub});
        }
        IDWriteTextLayout* l = layout(v, g_f.cat, r.right - r.left - 50.f, r.bottom - r.top);
        if (l) {
            l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            if (locale) l->SetLocaleName(locale, DWRITE_TEXT_RANGE{0, (UINT32)v.size()});   // its own Han shapes
            rt->DrawTextLayout(D2D1::Point2F(r.left + 25.f, r.top), l, B(vc, enabled ? 1.f : 0.6f), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            l->Release();
        }
    }

    // ---- a text that never gets cut with "...": clipped with a fade at the edge, scrolled when selected (the game's marquee) ----
    void marquee(const std::wstring& s, IDWriteTextFormat* f, float x, float y, float w, float h, uint32_t c, bool scroll,
                 uint32_t fade_bg) {
        const float tw = text_w(s, f);
        float off = 0;
        if (scroll && tw > w) {
            a.animating = true;
            const double speed = 45.0, pause = 1.2, travel = (tw - w + 12.f) / speed;
            const double cycle = pause + travel + pause;
            const double ph = std::fmod(std::max(0.0, t - a.sel_t0), cycle);
            off = ph < pause ? 0.f : ph < pause + travel ? float((ph - pause) * speed) : tw - w + 12.f;
        }
        if (tw > w && rtl_text(s)) off = (tw - w) - off;   // its first word is at the right: shown first, the rest comes from the left
        (void)fade_bg;
        Fade fd = push_fade(D2D1::RectF(x, y, x + w, y + h), off > 0.5f ? 16.f : 0.f, tw - off > w + 0.5f ? 22.f : 0.f, 0.f);
        text(s, f, x - off, y, tw + 40.f, h, c);
        pop_fade(fd);
    }

    // What is drawn inside r fades out over its last pixels (an opacity mask: right over any background)
    struct Fade { ID2D1Layer* layer = nullptr; ID2D1LinearGradientBrush* brush = nullptr; bool clip = false; };
    Fade push_fade(const D2D1_RECT_F& r, float left, float right, float bottom) {
        Fade fd;
        if (left <= 0 && right <= 0 && bottom <= 0) {
            rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
            fd.clip = true;
            return fd;
        }
        const bool vert = bottom > 0;
        const float len = vert ? r.bottom - r.top : r.right - r.left;
        const float a0 = vert ? 0.f : left / len, a1 = vert ? bottom / len : right / len;
        D2D1_GRADIENT_STOP s[4] = { {0.f, D2D1::ColorF(0, 0, 0, a0 > 0 ? 0.f : 1.f)}, {std::max(0.001f, a0), D2D1::ColorF(0, 0, 0, 1.f)},
                                    {std::min(0.999f, 1.f - a1), D2D1::ColorF(0, 0, 0, 1.f)}, {1.f, D2D1::ColorF(0, 0, 0, a1 > 0 ? 0.f : 1.f)} };
        ID2D1GradientStopCollection* st = nullptr;
        rt->CreateGradientStopCollection(s, 4, &st);
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.left, r.top),
                                                                          vert ? D2D1::Point2F(r.left, r.bottom) : D2D1::Point2F(r.right, r.top)),
                                      st, &fd.brush);
        release(st);
        rt->CreateLayer(&fd.layer);
        rt->PushLayer(D2D1::LayerParameters(r, nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), 1.f, fd.brush), fd.layer);
        return fd;
    }
    void pop_fade(Fade& fd) {
        if (fd.clip) rt->PopAxisAlignedClip();
        else { rt->PopLayer(); release(fd.layer); release(fd.brush); }
    }

    // ---- a key of the key hints: a small dark keycap ----
    float keycap(float x, float cy, const std::wstring& k, bool hot) {
        const float w = std::max(22.f, text_w(k, g_f.key) + 10.f), h = 22.f;
        D2D1_RECT_F r = D2D1::RectF(x, cy - h / 2, x + w, cy + h / 2);
        ID2D1LinearGradientBrush* g = nullptr;
        ID2D1GradientStopCollection* st = nullptr;
        D2D1_GRADIENT_STOP s[2] = { {0.f, rgb(0x34353A)}, {1.f, rgb(0x1B1C1F)} };
        rt->CreateGradientStopCollection(s, 2, &st);
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, r.top), D2D1::Point2F(0, r.bottom)), st, &g);
        rt->FillRectangle(r, g);
        release(g);
        release(st);
        rt->DrawRectangle(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), B(hot ? 0xFFFFFF : 0x9C9C9C), 1.f);
        text(k, g_f.key, r.left, r.top, w, h, hot ? 0xFFFFFF : 0xE2E2E2);
        return w;
    }
    float hint(float x, float cy, const std::wstring& k, const std::wstring& label, Action act, bool enabled = true) {
        const float lw = text_w(label, g_f.footer);
        const float total = std::max(22.f, text_w(k, g_f.key) + 10.f) + 8.f + lw;
        const D2D1_RECT_F hr = D2D1::RectF(x - 4.f, cy - 15.f, x + total + 4.f, cy + 15.f);
        bool hot = false;
        if (enabled) {
            a.hits.push_back({hr, act});
            hot = a.mouse.x >= 0 && hit_at(hr);
        }
        const float kw = keycap(x, cy, k, hot);
        text(label, g_f.footer, x + kw + 8.f, cy - 14.f, lw + 4.f, 28.f, hot ? 0xFFFFFF : (enabled ? kText : kTextDim));
        return x + total + 28.f;
    }
    bool hit_at(const D2D1_RECT_F& r) {
        const float mx = (float)a.mouse.x, my = (float)a.mouse.y;
        return mx >= r.left && mx < r.right && my >= r.top && my < r.bottom;
    }
    bool hovered(Action act) {
        for (auto& h : a.hits) if (h.a == act && hit_at(h.r)) return true;
        return false;
    }

    // ---- the page header: the tool's name, the line, minimise and close ----
    // ---- the two pages, as the game's tab bar: its keys at the ends, the page of the moment in the selected look ----
    void tabs(float x) {
        const float cy = 24.f, top = 10.f, bh = 28.f;
        {
            const float kw = keycap(x, cy, L"Q", false);
            a.hits.push_back({D2D1::RectF(x - 4.f, top, x + kw + 4.f, top + bh), A_TAB, -1});
            x += kw + 10.f;
        }
        const TextId ids[3] = { T_TAB_SORT, T_TAB_INSTALLED, T_TAB_OPTIONS };
        for (int k = 0; k < 3; ++k) {
            const std::wstring s = T(ids[k]);
            const float tw = text_w(s, g_f.band), w = tw + 34.f;
            const D2D1_RECT_F r = D2D1::RectF(x, top, x + w, top + bh);
            const bool on = a.page == k, hot = !on && a.mouse.x >= 0 && hit_at(r);
            if (on) {
                ID2D1PathGeometry* p = poly({D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.top), D2D1::Point2F(r.right, r.bottom - 6.f),
                                             D2D1::Point2F(r.right - 6.f, r.bottom), D2D1::Point2F(r.left, r.bottom)});
                selected_fill(p, r, false);
                release(p);
            }
            text(s, g_f.band, r.left + 17.f, r.top, tw + 4.f, bh, on ? kDark : hot ? 0xFFFFFF : kTextSoft);
            // Options: a release of the suite, or a version of the sorter, waits there
            if (k == 2 && !on && ((a.up_check.state == SuiteCheck::Available && !a.up_check.ignored) ||
                                  a.self_check.state == SelfCheck::Available))
                fill(D2D1::RectF(r.right - 12.f, r.top + 5.f, r.right - 6.f, r.top + 11.f), kYellow);
            a.hits.push_back({r, A_TAB, k});
            x += w + 4.f;
        }
        x += 6.f;
        const float kw = keycap(x, cy, L"E", false);
        a.hits.push_back({D2D1::RectF(x - 4.f, top, x + kw + 4.f, top + bh), A_TAB, -2});
    }

    void header() {
        const float title_w = small_caps(T(T_TITLE), g_f.title, 23.f, 17.f, 36.f, 9.f, 0xFFFFFF, 1.6f);
        tabs(36.f + title_w + 44.f);
        const float bw = 36.f, bh = 28.f, top = 10.f;
        const D2D1_RECT_F close = D2D1::RectF(a.W - 30.f - bw, top, a.W - 30.f, top + bh);
        const D2D1_RECT_F maxi = D2D1::RectF(close.left - 8.f - bw, top, close.left - 8.f, top + bh);
        const D2D1_RECT_F mini = D2D1::RectF(maxi.left - 8.f - bw, top, maxi.left - 8.f, top + bh);
        // maximised: the button restores, its glyph two squares (as Windows' own)
        const bool zoomed = a.hwnd ? IsZoomed(a.hwnd) != FALSE : a.opt.maximized;
        for (int k = 0; k < 3; ++k) {
            const D2D1_RECT_F r = k == 0 ? mini : k == 1 ? maxi : close;
            const bool hot = a.mouse.x >= 0 && hit_at(r);
            if (hot) {
                ID2D1PathGeometry* p = poly({D2D1::Point2F(r.left, r.top), D2D1::Point2F(r.right, r.top), D2D1::Point2F(r.right, r.bottom - 6.f),
                                             D2D1::Point2F(r.right - 6.f, r.bottom), D2D1::Point2F(r.left, r.bottom)});
                selected_fill(p, r, true);
                release(p);
            }
            const uint32_t gc = hot ? kDark : 0xC9CDD8;
            const float cx = (r.left + r.right) / 2 + (hot ? 2.f : 0.f), cy = (r.top + r.bottom) / 2;
            if (k == 0) line(cx - 6.5f, cy + 0.5f, cx + 6.5f, cy + 0.5f, gc, 1.f, 1.6f);
            else if (k == 1 && !zoomed) {
                rt->DrawRectangle(D2D1::RectF(cx - 5.5f, cy - 5.5f, cx + 5.5f, cy + 5.5f), B(gc), 1.6f);
            } else if (k == 1) {
                // the window's square in front, the one it goes back to behind it (only its top and right edges show)
                rt->DrawRectangle(D2D1::RectF(cx - 5.5f, cy - 3.f, cx + 3.f, cy + 5.5f), B(gc), 1.6f);
                line(cx - 3.f, cy - 3.f, cx - 3.f, cy - 5.5f, gc, 1.f, 1.6f);
                line(cx - 3.8f, cy - 5.5f, cx + 6.3f, cy - 5.5f, gc, 1.f, 1.6f);
                line(cx + 5.5f, cy - 5.5f, cx + 5.5f, cy + 3.f, gc, 1.f, 1.6f);
                line(cx + 5.5f, cy + 3.f, cx + 3.f, cy + 3.f, gc, 1.f, 1.6f);
            } else {
                line(cx - 5.5f, cy - 5.5f, cx + 5.5f, cy + 5.5f, gc, 1.f, 1.6f);
                line(cx - 5.5f, cy + 5.5f, cx + 5.5f, cy - 5.5f, gc, 1.f, 1.6f);
            }
            a.hits.push_back({r, k == 0 ? A_MIN : k == 1 ? A_MAX : A_CLOSE});
        }
        rule(34.f, 46.f, a.W - 34.f);
    }

    // ---- the game folder strip, framed like the game's tab bar ----
    void strip() {
        const float y0 = 52.f, h = 34.f;
        float x = 40.f;
        const std::wstring lab = T(T_GAME_FOLDER);
        text(lab, g_f.strip, x, y0, 400.f, h, kBlue);
        x += text_w(lab, g_f.strip) + 14.f;
        std::wstring path = a.game_dir.empty() ? T(T_GAME_NOT_FOUND) : a.game_dir;
        uint32_t pc = a.game_dir.empty() ? kYellow : kText;
        // the right side: the state of the indexes and the key to change the folder
        const std::wstring change = T(T_CHANGE);
        const float hint_w = 22.f + 8.f + text_w(change, g_f.footer) + 8.f;
        const float hx = a.W - 40.f - hint_w;
        std::wstring state;
        uint32_t sc = kTextDim;
        if (!a.game_dir.empty()) {
            if (a.loading) state = T(T_LOADING_INDEX);
            else if (!a.costume_ok) { state = T(T_INDEX_MISSING, {a.costume_note}); sc = kYellow; }
        }
        float avail = hx - 24.f - x;
        if (!state.empty()) {
            const float sw = std::min(text_w(state, g_f.strip), avail * 0.55f);
            marquee(state, g_f.strip, hx - 24.f - sw, y0, sw, h, sc, true, 0x0D1122);
            avail -= sw + 20.f;
        }
        marquee(path, g_f.strip, x, y0, std::max(40.f, avail), h, pc, true, 0x0D1122);   // a long path scrolls, never cut
        hint(hx, y0 + h / 2, L"G", change, A_GAME);
        rule(34.f, 92.f, a.W - 34.f, 0.6f);
    }

    // ---- empty: the drop zone ----
    void drop_zone() {
        const D2D1_RECT_F r = D2D1::RectF(36.f, 112.f, a.W - 36.f, a.H - 104.f);
        fill(r, 0x0A0E1D, 0.55f);
        rt->DrawRectangle(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), B(kLine, 0.45f), 1.f);
        for (float cx : {r.left, r.right}) for (float cy : {r.top, r.bottom}) fill(D2D1::RectF(cx - 2.f, cy - 2.f, cx + 2.f, cy + 2.f), 0xD8D8D8, 0.9f);
        // inner frame, dashed like the game's grid outlines
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2 - 30.f;
        // the drop glyph: a tray and an arrow going down into it
        const uint32_t ic = kBlue;
        line(cx, cy - 92.f, cx, cy - 52.f, ic, 1.f, 3.f);
        ID2D1PathGeometry* head = poly({D2D1::Point2F(cx - 13.f, cy - 60.f), D2D1::Point2F(cx + 13.f, cy - 60.f), D2D1::Point2F(cx, cy - 44.f)});
        rt->FillGeometry(head, B(ic));
        release(head);
        line(cx - 34.f, cy - 52.f, cx - 34.f, cy - 34.f, ic, 0.9f, 2.f);
        line(cx + 34.f, cy - 52.f, cx + 34.f, cy - 34.f, ic, 0.9f, 2.f);
        line(cx - 35.f, cy - 34.f, cx + 35.f, cy - 34.f, ic, 0.9f, 2.f);
        small_caps(T(T_DROP_HERE), g_f.drop_title, 34.f, 26.f, cx, cy - 22.f, 0xFFFFFF, 2.0f, true);
        text(T(T_DROP_SUB), g_f.drop_sub, r.left, cy + 26.f, r.right - r.left, 28.f, kTextSoft);
        text(T(T_DROP_KINDS), g_f.drop_kinds, r.left, cy + 54.f, r.right - r.left, 24.f, 0x8D92A0);
        // Browse: a row of the game, selected (the only thing to choose here)
        const float bw = 300.f, bh = 42.f;
        const D2D1_RECT_F b = D2D1::RectF(cx - bw / 2, cy + 102.f, cx + bw / 2, cy + 102.f + bh);
        row_shell(b, true, false);
        text(T(T_BROWSE), g_f.button, b.left, b.top, bw, bh, kDark);
        a.hits.push_back({b, A_BROWSE});
    }

    std::wstring category_label(const Item& it) {
        if (it.reading) return T(T_CAT_READING);
        std::wstring s = T(category_text(it.c.cat));
        if ((it.c.cat == Category::Costume || it.c.cat == Category::Colours) && !it.c.characters.empty()) {
            if (it.c.characters.size() <= 2) {
                std::string who;
                for (auto& x : it.c.characters) who += (who.empty() ? "" : ", ") + x;
                s += L" \u00B7 " + W8(who);
            } else {
                s += L" \u00B7 " + T(T_CAT_N_CHARACTERS, {std::to_string(it.c.characters.size())});
            }
        }
        return s;
    }

    std::wstring dest_dir_of(const Item& it) {
        const std::wstring& d = (it.nsfw && !it.c.dest_dir_nsfw.empty()) ? it.c.dest_dir_nsfw : it.c.dest_dir;
        return d;
    }

    // ---- the list ----
    void list() {
        const float x0 = 36.f, x1 = 36.f + std::floor((a.W - 72.f) * 0.585f);
        const float top = 126.f, bottom = a.H - 100.f;
        a.list_rect = D2D1::RectF(x0, top, x1, bottom);
        // column captions
        const float rw = x1 - x0 - 14.f;
        const float cat_x = x0 + rw * 0.47f, val_x = x0 + rw * 0.755f;
        text(upper(T(T_COL_MOD)), g_f.caption, x0 + 18.f, 102.f, 200.f, 20.f, 0x8E94A6);
        text(upper(T(T_COL_CATEGORY)), g_f.caption, cat_x, 102.f, 200.f, 20.f, 0x8E94A6);
        {
            IDWriteTextLayout* l = layout(upper(T(T_COL_NSFW)), g_f.caption, x0 + rw - val_x, 20.f);
            if (l) { l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); rt->DrawTextLayout(D2D1::Point2F(val_x, 102.f), l, B(0x8E94A6)); l->Release(); }
        }
        // content height
        const float rh = 40.f, gap = 8.f, sh = 30.f, sgap = 4.f;
        float total = 0;
        for (auto& it : a.items) total += rh + gap + (item_has_sub(it) ? sh + sgap : 0.f);
        a.list_h = total;
        a.list_view = bottom - top;
        a.list_scroll = std::max(0.f, std::min(a.list_scroll, std::max(0.f, total - a.list_view)));
        rt->PushAxisAlignedClip(D2D1::RectF(x0 - 4.f, top - 2.f, x1 + 4.f, bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        float y = top - a.list_scroll;
        for (int i = 0; i < (int)a.items.size(); ++i) {
            Item& it = a.items[i];
            const bool selrow = i == a.sel && !a.sel_sub;
            const D2D1_RECT_F r = D2D1::RectF(x0, y, x0 + rw, y + rh);
            if (r.bottom > top - 60.f && r.top < bottom + 60.f) {
                const bool hot = !selrow && hit_at(r) && a.mouse.y >= top && a.mouse.y < bottom;
                row_shell(r, selrow, hot);
                a.hits.push_back({r, A_ROW, i, false});
                const bool unknown = !it.reading && it.c.cat == Category::Unknown;
                const uint32_t name_c = selrow ? kDark : unknown ? 0x9A9DA6 : kText;
                const uint32_t bgc = selrow ? 0xDCE7F5 : 0x1F2025;
                marquee(it.c.name, g_f.row, x0 + 18.f, y, cat_x - x0 - 30.f, rh, name_c, selrow, bgc);
                // category
                uint32_t cc = selrow ? kBlueDeep : kBlue;
                if (it.reading) cc = selrow ? 0x5A5D66 : kTextDim;
                else if (unknown) cc = selrow ? kRedDeep : kRed;
                const std::wstring cl = category_label(it);
                marquee(cl, g_f.cat, cat_x, y, val_x - cat_x - 8.f, rh, cc, selrow, selrow ? 0xE2EAF5 : 0x1F2025);
                if (it.reading) spinner(cat_x + text_w(cl, g_f.cat) + 14.f, y + rh / 2, selrow ? kBlueDeep : kBlue);
                // NSFW value (costumes)
                if (!it.reading && it.c.cat == Category::Costume) {
                    const bool can = it.done != Item::Installed && it.done != Item::Installing;
                    value_control(D2D1::RectF(val_x, y, x0 + rw - 6.f, y + rh), T(it.nsfw ? T_YES : T_NO), selrow, can, i, false,
                                  selrow ? kDark : (it.nsfw ? kYellow : kText));
                }
            }
            y += rh;
            if (item_has_sub(it)) {
                y += sgap;
                sub_row(it, i, D2D1::RectF(x0 + 26.f, y, x0 + rw, y + sh), val_x, top, bottom);
                y += sh;
            }
            y += gap;
        }
        rt->PopAxisAlignedClip();
        // scroll bar, the game's thin blue bar
        if (total > a.list_view + 1.f) {
            const float bx = x1 - 3.f;
            fill(D2D1::RectF(bx, top, bx + 3.f, bottom), 0xFFFFFF, 0.08f);
            const float th = std::max(30.f, a.list_view * a.list_view / total);
            const float ty = top + (a.list_view - th) * (a.list_scroll / (total - a.list_view));
            fill(D2D1::RectF(bx, ty, bx + 3.f, ty + th), 0x1E6FD9);
        }
        vrule(x1 + 22.f, 112.f, a.H - 104.f, 0.55f);
    }

    void spinner(float cx, float cy, uint32_t c) {
        for (int k = 0; k < 3; ++k) {
            const float ph = float(std::fmod(t * 2.2 + k * 0.33, 1.0));
            const float al = 0.25f + 0.75f * (0.5f + 0.5f * std::sin(ph * 6.2831f));
            fill(D2D1::RectF(cx + k * 9.f, cy - 2.f, cx + k * 9.f + 4.f, cy + 2.f), c, al);
        }
    }

    void sub_row(Item& it, int i, const D2D1_RECT_F& r, float val_x, float top, float bottom) {
        if (r.bottom < top - 40.f || r.top > bottom + 40.f) return;
        const bool selectable = item_selectable_sub(it);
        const bool sel = i == a.sel && a.sel_sub && selectable;
        // the link to its row: a short vertical tick, as the game's sub-options are indented under their row
        line(r.left - 12.f, r.top - 4.f, r.left - 12.f, r.top + 15.f, kLine, 0.45f);
        line(r.left - 12.f, r.top + 15.f, r.left - 2.f, r.top + 15.f, kLine, 0.45f);
        row_shell(r, sel, selectable && !sel && hit_at(r), 7.f, true);
        if (selectable) a.hits.push_back({r, A_ROW, i, true});
        std::wstring s;
        uint32_t c = kTextSoft;
        const std::wstring dir = dest_dir_of(it);
        bool control = false;
        if (it.c.cat == Category::Unknown) {
            s = T(it.c.reason);
            c = 0xB9BCC4;
        } else if (it.done == Item::Installing) {
            s = it.progress >= 0 ? T(T_SUB_INSTALLING_PCT, {std::to_string(int(it.progress * 100.f + 0.5f))}) : T(T_SUB_INSTALLING);
            c = kText;
            const float w = (r.right - r.left - 20.f) * std::max(0.f, std::min(1.f, it.progress));
            fill(D2D1::RectF(r.left + 10.f, r.bottom - 4.f, r.right - 10.f, r.bottom - 2.f), 0xFFFFFF, 0.08f);
            fill(D2D1::RectF(r.left + 10.f, r.bottom - 4.f, r.left + 10.f + w, r.bottom - 2.f), 0x1E6FD9);
        } else if (it.done == Item::Installed) {
            s = T(T_SUB_INSTALLED, {to_utf8(it.placed_to)});
            c = kYellow;
        } else if (it.done == Item::Failed) {
            s = T(T_SUB_ERROR, {tr(it.error)});
            c = kRed;
        } else if (it.exists) {
            s = it.done == Item::Skipped ? T(T_SUB_SKIPPED, {to_utf8(dir)}) : T(T_SUB_EXISTS, {to_utf8(dir)});
            c = it.done == Item::Skipped ? 0xA4A7B0 : 0xE9C46A;
            control = true;
        }
        if (sel) c = kDark;
        const float tw = control ? val_x - r.left - 22.f : r.right - r.left - 26.f;
        marquee(s, g_f.sub, r.left + 14.f, r.top, tw, r.bottom - r.top, c, sel, sel ? 0xDCE7F5 : 0x161719);
        if (control)
            value_control(D2D1::RectF(val_x, r.top, r.right - 6.f, r.bottom), T(it.replace ? T_REPLACE : T_SKIP), sel, true, i, true,
                          sel ? kDark : (it.replace ? kYellow : kText));
    }

    // ---- a mod's picture: asked for once (the picture thread answers; --shot reads it in line), its bitmap made for this target ----
    int picture_side(int base) {
        float dx = 96.f, dy = 96.f;
        rt->GetDpi(&dx, &dy);
        return dx > 100.f ? base * 3 / 2 : base;   // a scaled-up screen: more pixels in the same place
    }
    Pic& picture(const std::wstring& path, uint64_t stamp, int side) {
        const std::wstring key = std::to_wstring(side) + L"|" + std::to_wstring(stamp) + L"|" + path;
        auto it = a.pics.find(key);
        if (it == a.pics.end()) {
            Pic p;
            if (a.pic_thread) {
                a.pic_thread->want(key, path, side, t);
            } else {
                ModPicture m;
                if (a.shot_loader && a.shot_loader->load(path, side, m)) {
                    p.state = Pic::Ready;
                    p.w = m.w;
                    p.h = m.h;
                    p.px = std::move(m.bgra);
                } else {
                    p.state = Pic::None;
                }
            }
            it = a.pics.emplace(key, std::move(p)).first;
        }
        Pic& p = it->second;
        if (p.state == Pic::Ready && (!p.bmp || p.owner != rt)) {
            release(p.bmp);
            if (p.px.empty()) {
                // its pixels went into a bitmap of a target that is gone: read again (from the small picture kept on disk)
                p.state = a.pic_thread ? Pic::Loading : Pic::None;
                if (a.pic_thread) a.pic_thread->want(key, path, side, t);
            } else {
                rt->CreateBitmap(D2D1::SizeU((UINT32)p.w, (UINT32)p.h), p.px.data(), (UINT32)p.w * 4,
                                 D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)), &p.bmp);
                p.owner = rt;
                if (a.pic_thread && p.bmp) std::vector<uint8_t>().swap(p.px);
            }
        }
        if (p.state == Pic::Loading) a.animating = true;
        return p;
    }
    // The picture fills the shape (r: its box). What does not fit is cut, more at the bottom than at the top (where a head is).
    void fill_picture(ID2D1Geometry* shape, const D2D1_RECT_F& r, ID2D1Bitmap* b) {
        const D2D1_SIZE_F s = b->GetSize();
        const float w = r.right - r.left, h = r.bottom - r.top;
        const float k = std::max(w / s.width, h / s.height);
        const float dx = r.left - (s.width * k - w) * 0.5f, dy = r.top - (s.height * k - h) * 0.25f;
        ID2D1BitmapBrush* bb = nullptr;
        rt->CreateBitmapBrush(b, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                              D2D1::BrushProperties(1.f, D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(dx, dy)), &bb);
        if (bb) rt->FillGeometry(shape, bb);
        release(bb);
    }
    // No picture: a small frame with a hill, faint (no word: the same in every language); being read: the three dots
    void no_picture(const D2D1_RECT_F& r, bool loading) {
        const float cx = std::floor((r.left + r.right) / 2), cy = std::floor((r.top + r.bottom) / 2);
        if (loading) { spinner(cx - 11.f, cy, 0xC8CCD8); return; }
        rt->DrawRectangle(D2D1::RectF(cx - 15.5f, cy - 11.5f, cx + 15.5f, cy + 11.5f), B(0xFFFFFF, 0.20f), 1.f);
        ID2D1PathGeometry* hill = poly({D2D1::Point2F(cx - 12.f, cy + 8.f), D2D1::Point2F(cx - 4.f, cy - 2.f), D2D1::Point2F(cx + 1.f, cy + 3.f),
                                        D2D1::Point2F(cx + 5.f, cy - 1.f), D2D1::Point2F(cx + 12.f, cy + 8.f)});
        rt->FillGeometry(hill, B(0xFFFFFF, 0.16f));
        release(hill);
        fill(D2D1::RectF(cx + 6.f, cy - 8.f, cx + 10.f, cy - 4.f), 0xFFFFFF, 0.18f);
    }
    // A mod's cell: its picture, its name under it (scrolled when the cursor is on it), in the rows' shell
    void cell(const D2D1_RECT_F& r, float pic_h, const std::wstring& name, Pic& p, bool sel, bool hot) {
        row_shell(r, sel, hot);
        const float cut = 9.f;
        // selected: the light shell shows around the picture, its blue bar at the left
        const D2D1_RECT_F pr = sel ? D2D1::RectF(r.left + 9.f, r.top + 4.f, r.right - 4.f, r.top + pic_h)
                                   : D2D1::RectF(r.left + 1.f, r.top + 1.f, r.right - 1.f, r.top + pic_h);
        ID2D1PathGeometry* shape = sel ? poly({D2D1::Point2F(pr.left, pr.top), D2D1::Point2F(pr.right, pr.top), D2D1::Point2F(pr.right, pr.bottom),
                                               D2D1::Point2F(pr.left, pr.bottom)})
                                       : poly({D2D1::Point2F(pr.left + cut, pr.top), D2D1::Point2F(pr.right, pr.top),
                                               D2D1::Point2F(pr.right, pr.bottom), D2D1::Point2F(pr.left, pr.bottom),
                                               D2D1::Point2F(pr.left, pr.top + cut)});
        rt->FillGeometry(shape, B(0x131418));
        if (p.bmp) fill_picture(shape, pr, p.bmp);
        else no_picture(pr, p.state == Pic::Loading);
        release(shape);
        if (!sel) line(r.left + 1.f, r.top + pic_h + 0.5f, r.right - 1.f, r.top + pic_h + 0.5f, 0xFFFFFF, 0.10f);
        const float nx = r.left + (sel ? 14.f : 11.f);
        marquee(name, g_f.sub, nx, r.top + pic_h, r.right - 10.f - nx, r.bottom - r.top - pic_h, sel ? kDark : kText, sel, 0);
    }

    // The band with a panel's name
    void panel_band(float px, float pr, float top, const std::wstring& s) {
        const float bh = 28.f;
        ID2D1PathGeometry* p = poly({D2D1::Point2F(px + 9.f, top), D2D1::Point2F(pr - 2.f, top), D2D1::Point2F(pr - 11.f, top + bh),
                                     D2D1::Point2F(px, top + bh)});
        ID2D1LinearGradientBrush* g = nullptr;
        ID2D1GradientStopCollection* st = nullptr;
        D2D1_GRADIENT_STOP gs[2] = { {0.f, rgb(0x9AA2C4, 0.50f)}, {1.f, rgb(0x6A7398, 0.30f)} };
        rt->CreateGradientStopCollection(gs, 2, &st);
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(px, top), D2D1::Point2F(pr, top)), st, &g);
        rt->FillGeometry(p, g);
        line(px + 9.f, top + 0.5f, pr - 2.f, top + 0.5f, 0xFFFFFF, 0.28f);
        release(g);
        release(st);
        release(p);
        text(s, g_f.band, px + 16.f, top, pr - px - 30.f, bh, 0xEDEDED);
    }

    // An update on its way, as its row and the footer say it: the download's percentage, then the install
    std::wstring progress_text() {
        return a.up_progress.step.load() == sf6up::Progress::Download ? T(T_UP_DOWNLOADING, {std::to_string(a.up_progress.percent())})
                                                                     : T(T_SUB_INSTALLING);
    }

    // ---- the Options page: the game's value rows (the label at the left, < value > at the right) ----
    void options() {
        const float x0 = 36.f, rw = std::min(a.W - 72.f, 860.f), rh = 40.f, top = 112.f;
        const float vw = std::min(330.f, rw * 0.45f);
        a.list_rect = D2D1::RectF(x0, top, x0 + rw, a.H - 100.f);
        float y = top;
        {   // the language
            const bool sel = a.opt_sel == 0;
            const D2D1_RECT_F r = D2D1::RectF(x0, y, x0 + rw, y + rh);
            row_shell(r, sel, !sel && a.mouse.x >= 0 && hit_at(r));
            a.hits.push_back({r, A_ROW, 0, false});
            marquee(T(T_O_LANGUAGE), g_f.row, x0 + 18.f, y, rw - vw - 36.f, rh, sel ? kDark : kText, sel, 0);
            const int i = language_index();
            value_control(D2D1::RectF(r.right - 6.f - vw, y, r.right - 6.f, y + rh), from_utf8(kLangNames[i]), sel, true, 0, false,
                          sel ? kDark : kText, kLangLocales[i]);
            y += rh + 6.f;
        }
        // The two update rows: what the last look-up found (a state, not a choice: no arrows), F acts on the selected one.
        // installing: that row's update is on its way (the download's percentage, a thin bar as under a mod being copied)
        auto status_row = [&](int row, const std::wstring& label, std::wstring v, uint32_t idle, bool installing, bool looking) {
            const bool sel = a.opt_sel == row;
            const D2D1_RECT_F r = D2D1::RectF(x0, y, x0 + rw, y + rh);
            row_shell(r, sel, !sel && a.mouse.x >= 0 && hit_at(r));
            a.hits.push_back({r, A_ROW, row, false});
            marquee(label, g_f.row, x0 + 18.f, y, rw - vw - 36.f, rh, sel ? kDark : kText, sel, 0);
            uint32_t vc = sel ? kDark : idle;
            if (installing) v = progress_text();
            else if (looking) v = T(T_UP_CHECKING);
            if (installing || looking) vc = sel ? kDark : kText;
            const float vx = r.right - 6.f - vw + 25.f, vtw = vw - 50.f;
            if (IDWriteTextLayout* l = layout(v, g_f.cat, vtw, rh)) {
                l->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                DWRITE_TEXT_METRICS m{};
                l->GetMetrics(&m);
                rt->DrawTextLayout(D2D1::Point2F(vx, y), l, B(vc), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                l->Release();
                if (installing || looking) {
                    const bool rtl = rtl_text(v);
                    spinner(rtl ? vx + (vtw - m.width) / 2 - 34.f : vx + (vtw + m.width) / 2 + 12.f, y + rh / 2, vc);
                    a.animating = true;
                }
            }
            if (installing) {
                const int step = a.up_progress.step.load();
                const float f = step == sf6up::Progress::Download ? a.up_progress.percent() / 100.f : step == sf6up::Progress::Install ? 1.f : 0.f;
                fill(D2D1::RectF(r.left + 10.f, r.bottom - 4.f, r.right - 10.f, r.bottom - 2.f), sel ? 0x000000 : 0xFFFFFF, sel ? 0.18f : 0.08f);
                fill(D2D1::RectF(r.left + 10.f, r.bottom - 4.f, r.left + 10.f + (r.right - r.left - 20.f) * f, r.bottom - 2.f), 0x1E6FD9);
            }
            y += rh + 6.f;
        };
        {   // the sorter's own updates
            const SelfCheck& me = a.self_check;
            std::wstring v;
            uint32_t vc = kText;
            switch (me.state) {
                case SelfCheck::Unknown: v = W8(std::string(kSorterVersion)); break;
                case SelfCheck::Off: v = T(T_UP_OFF); break;
                case SelfCheck::Failed: v = T(T_UP_FAILED); break;
                case SelfCheck::Current: v = T(T_UP_CURRENT, {me.current}); break;
                case SelfCheck::Available: v = T(T_UP_AVAILABLE, {me.latest}); vc = kYellow; break;
            }
            status_row(OPT_SELF, T(T_O_SELF_UPDATES), v, vc, a.up_busy == 2 && a.up_self, a.up_busy == 1);
        }
        // SF6 Mod Suite's own updates
        const SuiteCheck& c = a.up_check;
        {
            std::wstring v;
            uint32_t vc = kText;
            switch (c.state) {
                case SuiteCheck::NoGame: v = L"\u2014"; break;
                case SuiteCheck::Off: v = T(T_UP_OFF); break;
                case SuiteCheck::NotInstalled: v = T(T_UP_NOT_INSTALLED); break;
                case SuiteCheck::Failed: v = T(T_UP_FAILED); break;
                // packages older than 1.32.0 report nothing and are never offered: "up to date" would be a guess
                case SuiteCheck::Current: v = c.unversioned > 0 ? T(T_UP_BY_HAND) : T(T_UP_CURRENT, {c.latest}); break;
                case SuiteCheck::Available: v = T(T_UP_AVAILABLE, {c.latest}); vc = kYellow; break;
            }
            status_row(OPT_SUITE, T(T_O_UPDATES), v, vc, a.up_busy == 2 && !a.up_self, a.up_busy == 1 && !a.game_dir.empty());
        }
        y += 6.f;
        // under it: what an install changes, as the game's box lists it
        for (auto& ln : c.lines) {
            text(W8(suite_line_text(ln, language())), g_f.sub, x0 + 18.f, y, rw - 36.f, 24.f, kTextSoft);
            y += 24.f;
        }
        if (c.unversioned > 0 && (c.state == SuiteCheck::Current || c.state == SuiteCheck::Available)) {
            if (!c.lines.empty()) y += 6.f;
            const std::wstring old = T(T_UP_OLD);
            const float h = std::max(21.f, text_h(old, g_f.value_wrap, rw - 36.f));
            text(old, g_f.value_wrap, x0 + 18.f, y, rw - 36.f, h + 4.f, kTextSoft);
        }
    }

    // ---- the Installed page: the mod folders at the left, what the selected one holds at the right ----
    void installed() {
        const float x0 = 36.f, x1 = 36.f + std::floor((a.W - 72.f) * 0.33f);
        const float top = 126.f, bottom = a.H - 100.f;
        a.list_rect = D2D1::RectF(x0, top, x1, bottom);
        text(upper(T(T_COL_CATEGORY)), g_f.caption, x0 + 18.f, 102.f, x1 - x0 - 18.f, 20.f, 0x8E94A6);
        const float rh = 40.f, gap = 8.f, rw = x1 - x0 - 14.f;
        float y = top;
        for (int i = 0; i < (int)a.inst.size(); ++i) {
            const InstCat& c = a.inst[i];
            const bool held = i == a.cat && a.inst_grid;   // the cursor is in its grid: the row keeps the blue bar only
            const bool sel = i == a.cat && !held;
            const D2D1_RECT_F r = D2D1::RectF(x0, y, x0 + rw, y + rh);
            row_shell(r, sel, !sel && a.mouse.x >= 0 && hit_at(r));
            if (held) fill(D2D1::RectF(r.left, r.top + 9.f, r.left + 5.f, r.bottom), kBar);
            a.hits.push_back({r, A_CAT, i});
            const std::wstring n = std::to_wstring(c.count);
            const float nw = text_w(n, g_f.cat);
            marquee(T(c.label), g_f.row, x0 + 18.f, y, rw - 18.f - nw - 34.f, rh, sel ? kDark : c.count ? kText : 0x9A9DA6, sel, 0);
            text(n, g_f.cat, r.right - 18.f - nw, y, nw + 4.f, rh, sel ? kBlueDeep : c.count ? kBlue : kTextDim);
            y += rh + gap;
        }
        vrule(x1 + 22.f, 112.f, a.H - 104.f, 0.55f);

        const InstPanel ip = inst_panel(a);
        const float px = ip.px, pr = ip.pr, ptop = ip.top;
        a.panel_rect = D2D1::RectF(px, ptop, pr, bottom);
        const InstCat* c = a.cat >= 0 && a.cat < (int)a.inst.size() ? &a.inst[a.cat] : nullptr;
        panel_band(px, pr, ptop, c ? T(c->label) : std::wstring());
        const float y0 = ip.y0;
        // the grid of pictures: its height is known before anything is drawn
        const bool pictures = c && c->pictures && !c->groups.empty();
        const Grid grid = pictures ? grid_layout(a) : Grid();
        if (pictures) a.panel_h = grid.height;
        const float h_before = a.panel_h;
        a.panel_view = bottom - y0;
        a.panel_scroll = std::max(0.f, std::min(a.panel_scroll, std::max(0.f, a.panel_h - a.panel_view)));
        Fade pf = push_fade(D2D1::RectF(px, y0 - 2.f, pr + 2.f, bottom), 0.f, 0.f, a.panel_h > a.panel_view + 1.f ? 28.f : 0.f);
        float py = y0 - a.panel_scroll;
        const float lx = px + 22.f, vx = px + 32.f, vw = pr - vx - 16.f;
        auto value = [&](const std::wstring& s, uint32_t col) {
            const float h = std::max(21.f, text_h(s, g_f.value_wrap, vw));
            if (py + h > y0 - 4.f && py < bottom) text(s, g_f.value_wrap, vx, py, vw, h + 4.f, col);
            py += h + 1.f;
        };
        auto group_name = [&](const InstGroup& g) {
            return g.kind == InstGroup::Character ? g.name : T(g.kind == InstGroup::Several ? T_I_SEVERAL : T_I_ALL_CHARS);
        };
        if (!c || c->groups.empty()) {
            value(T(T_I_EMPTY), kTextDim);
        } else if (pictures) {
            // a section per character: its name, how many mods it has, then their pictures
            for (auto& hd : grid.heads) {
                const float y = py + hd.y;
                if (y + kHeadH < y0 - 4.f || y > bottom) continue;
                const InstGroup& g = c->groups[hd.group];
                const std::wstring n = std::to_wstring(g.entries.size());
                const float nw = text_w(n, g_f.cat);
                marquee(group_name(g), g_f.label, lx, y, ip.gx1 - lx - nw - 16.f, 24.f, kBlue, false, 0);
                text(n, g_f.cat, ip.gx1 - nw, y, nw + 4.f, 24.f, 0x8E94A6);
                line(lx, y + 26.5f, ip.gx1, y + 26.5f, kLine, 0.22f);
            }
            const int side = picture_side(256);
            for (int i = 0; i < (int)grid.cells.size(); ++i) {
                const GridCell& gc = grid.cells[i];
                const D2D1_RECT_F r = D2D1::RectF(gc.r.left, py + gc.r.top, gc.r.right, py + gc.r.bottom);
                if (r.bottom < y0 - 4.f || r.top > bottom) continue;   // only what shows asks for its picture
                const InstGroup& g = c->groups[gc.group];
                const InstEntry& e = g.entries[gc.entry];
                const bool sel = a.inst_grid && i == a.inst_sel;
                const D2D1_RECT_F vis = D2D1::RectF(r.left, std::max(r.top, y0), r.right, std::min(r.bottom, bottom));
                cell(r, grid.pic_h, e.name, picture(entry_path(a, *c, g, e), e.stamp, side), sel, !sel && a.mouse.x >= 0 && hit_at(vis));
                a.hits.push_back({vis, A_CELL, i});
            }
            py += grid.height;
        } else {
            for (auto& g : c->groups) {
                if (g.kind != InstGroup::Flat) {
                    if (py + 24.f > y0 - 4.f && py < bottom) text(group_name(g), g_f.label, lx, py, pr - lx, 24.f, kBlue);
                    py += 24.f;
                }
                for (auto& e : g.entries) value(e.name, 0xE6E6E6);
                py += 9.f;
            }
        }
        pop_fade(pf);
        a.panel_h = py + a.panel_scroll - y0;
        if (std::fabs(a.panel_h - h_before) > 1.f) a.animating = true;   // laid out with the height of before: once more
        if (a.panel_h > a.panel_view + 1.f) {
            const float bx = pr - 3.f;
            fill(D2D1::RectF(bx, y0, bx + 3.f, bottom), 0xFFFFFF, 0.08f);
            const float th = std::max(30.f, a.panel_view * a.panel_view / a.panel_h);
            const float ty = y0 + (a.panel_view - th) * (a.panel_scroll / (a.panel_h - a.panel_view));
            fill(D2D1::RectF(bx, ty, bx + 3.f, ty + th), 0x1E6FD9);
        }
    }

    // ---- the button that starts the game (through Steam), at the right of the key hints ----
    void launch_button(float cy) {
        const bool starting = game_starting(a);
        const bool enabled = !a.game_running && !starting && a.up_busy != 2;   // not into an update being installed
        const std::wstring label = T(a.game_running ? T_GAME_RUNNING : starting ? T_GAME_STARTING : T_H_LAUNCH);
        const float lw = text_w(label, g_f.footer), kw = enabled ? 22.f + 8.f : 0.f;
        const float w = 18.f + kw + lw + 22.f, h = 36.f;
        const D2D1_RECT_F r = D2D1::RectF(a.W - 36.f - w, cy - h / 2, a.W - 36.f, cy + h / 2);
        const bool hot = enabled && a.mouse.x >= 0 && hit_at(r);
        row_shell(r, hot, false, 8.f, !enabled);
        float x = r.left + 18.f;
        if (enabled) {
            x += keycap(x, cy, L"L", false) + 8.f;
            a.hits.push_back({r, A_LAUNCH});
        }
        text(label, g_f.footer, x, cy - 14.f, lw + 4.f, 28.f, hot ? kDark : enabled ? kText : kTextDim);
    }

    // ---- the summary panel at the right ----
    void panel() {
        const float px = 36.f + std::floor((a.W - 72.f) * 0.585f) + 48.f, pr = a.W - 36.f;
        const float top = 108.f, bottom = a.H - 100.f;
        a.panel_rect = D2D1::RectF(px, top, pr, bottom);
        const Item* it = a.sel < (int)a.items.size() ? &a.items[a.sel] : nullptr;
        // the band with the section's name
        const std::wstring band = it && !it->reading ? T(category_text(it->c.cat)) : T(T_P_DETAILS);
        panel_band(px, pr, top, band);
        const float y0 = top + 40.f;
        a.panel_view = bottom - y0;
        // what does not fit fades out at the bottom (the wheel scrolls it)
        Fade pf = push_fade(D2D1::RectF(px, y0 - 2.f, pr + 2.f, bottom), 0.f, 0.f, a.panel_h > a.panel_view + 1.f ? 28.f : 0.f);
        float y = y0 - a.panel_scroll;
        const float lx = px + 22.f, vx = px + 32.f, vw = pr - vx - 4.f;
        auto label = [&](TextId id) {
            text(T(id), g_f.label, lx, y, pr - lx, 24.f, kBlue);
            y += 24.f;
        };
        auto value = [&](const std::wstring& s, uint32_t c = 0xE6E6E6) {
            const float h = std::max(21.f, text_h(s, g_f.value_wrap, vw));
            text(s, g_f.value_wrap, vx, y, vw, h + 4.f, c);
            y += h + 1.f;
        };
        auto gapv = [&]() { y += 9.f; };
        if (!it) {
            value(T(T_P_NOTHING), kTextDim);
        } else {
            if (!it->reading && it->c.cat != Category::Unknown && (it->c.kind == ItemKind::Folder || it->c.kind == ItemKind::Archive)) {
                // the mod's own picture, whole; the box is there from the start (while it is read, and for a mod that has none):
                // nothing under it moves
                Pic& p = picture(it->path, 0, picture_side(512));
                {
                    const float bw = pr - lx - 4.f;
                    // never more than a good third of the panel: the mod's state stays in sight under it
                    const float most = std::min(std::floor(bw * 0.75f), std::floor((bottom - y0) * 0.42f));
                    const float bh = std::min(most, std::floor(p.bmp && p.w > 0 ? bw * p.h / p.w : bw * 9.f / 16.f));
                    const D2D1_RECT_F box = D2D1::RectF(lx, y, lx + bw, y + bh);
                    fill(box, 0x131418);
                    if (p.bmp) {
                        const float k = std::min(bw / p.w, bh / p.h), dw = p.w * k, dh = p.h * k;
                        const float dx = lx + std::floor((bw - dw) / 2), dy = y + std::floor((bh - dh) / 2);
                        rt->DrawBitmap(p.bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    } else {
                        no_picture(box, p.state == Pic::Loading);
                    }
                    rt->DrawRectangle(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f), B(0xFFFFFF, 0.20f), 1.f);
                    y += bh + 12.f;
                }
            }
            label(T_P_NAME);
            value(it->c.name);
            gapv();
            if (it->reading) {
                label(T_P_STATE);
                value(T(T_ST_READING), kTextSoft);
            } else if (it->c.cat == Category::Unknown) {
                label(T_P_WHY);
                value(T(it->c.reason), 0xF0B0B0);
                gapv();
                label(T_P_STATE);
                value(T(T_ST_NOT_PLACED), kTextSoft);
                gapv();
                if (!it->c.found.empty()) {
                    label(T_P_FOUND);
                    for (auto& f : it->c.found) value(T(f));
                }
            } else {
                if (!it->c.characters.empty()) {
                    label(it->c.characters.size() > 1 ? T_P_CHARACTERS : T_P_CHARACTER);
                    std::string who;
                    for (auto& x : it->c.characters) who += (who.empty() ? "" : ", ") + x;
                    value(W8(who));
                    gapv();
                }
                label(T_P_DEST);
                value(destination_rel(it->c, it->nsfw));
                gapv();
                label(T_P_STATE);
                switch (it->done) {
                    case Item::Installing: value(T(T_ST_INSTALLING), kText); break;
                    case Item::Installed: value(T(T_ST_INSTALLED), kYellow); break;
                    case Item::Skipped: value(T(T_ST_SKIPPED), 0xA4A7B0); break;
                    case Item::Failed: value(T(T_ST_ERROR, {tr(it->error)}), kRed); break;
                    default: value(it->exists ? T(T_ST_EXISTS) : T(T_ST_READY), it->exists ? 0xE9C46A : kText); break;
                }
                gapv();
                if (it->c.cat == Category::Costume) {
                    label(T_P_NSFW);
                    value(T(it->nsfw ? T_YES : T_NO), it->nsfw ? kYellow : 0xE6E6E6);
                    value(T(it->c.nsfw_why), 0xA9ADB8);
                    value(T(T_NSFW_HINT), 0x8D92A0);
                    gapv();
                }
                label(T_P_FOUND);
                for (auto& f : it->c.found) value(T(f));
            }
        }
        pop_fade(pf);
        a.panel_h = y + a.panel_scroll - y0;
    }

    // ---- the footer: a description line and the key hints ----
    void footer() {
        const SuiteCheck& up = a.up_check;
        const SelfCheck& me = a.self_check;
        const bool up_row = a.page == 2 && a.opt_sel == OPT_SUITE;   // on the suite's updates row: what it does, and F
        const bool self_row = a.page == 2 && a.opt_sel == OPT_SELF;  // on the sorter's own
        std::wstring note = T(a.page == 1 ? T_NOTE_INSTALLED : a.page != 2 ? T_NOTE : up_row ? T_NOTE_UPDATES
                              : self_row ? T_NOTE_SELF_UPDATES : T_NOTE_OPTIONS);
        uint32_t nc = 0xDADADA;
        if (a.launch_failed) { note = T(T_NOTE_LAUNCH_FAILED); nc = kRed; }
        else if (a.page == 1 && a.inst_note.id != T__COUNT) { note = T(a.inst_note); nc = a.inst_note_error ? kRed : kYellow; }
        else if (a.up_busy == 2) note = progress_text() + L"  " + (a.up_self ? T(T_TITLE) + L" " + W8(me.latest) : L"SF6 Mod Suite " + W8(up.latest));
        else if (a.installing) note = T(T_SUB_INSTALLING) + L"  " + std::to_wstring(a.install_count) + L" / " + std::to_wstring(a.install_total);
        else if (a.page == 2 && !a.up_fail.empty()) { note = marked(W8(a.up_fail)); nc = kRed; }
        else if (a.page == 2 && a.up_note.id != T__COUNT) { note = T(a.up_note); nc = kYellow; }
        else if (a.page == 0 && a.install_done_note) { note = T(a.game_running ? T_NOTE_DONE_RUNNING : T_NOTE_DONE); nc = kYellow; }
        else if (up.state == SuiteCheck::Available && (up_row || (!up.ignored && a.page != 2))) {
            // a release of the suite waits: said on the other pages (not when Ignore was answered in game) and on its own row
            note = T(!up_row ? T_NOTE_UPDATE : a.game_running ? T_NOTE_UPDATE_RUNNING : T_NOTE_UPDATE_HERE, {up.latest});
            nc = kYellow;
        }
        else if (me.state == SelfCheck::Available && (self_row || a.page != 2)) {
            // a version of the sorter itself waits: the same way
            note = T(self_row ? T_NOTE_SELF_UPDATE_HERE : T_NOTE_SELF_UPDATE, {me.latest});
            nc = kYellow;
        }
        else if (((up_row && up.state == SuiteCheck::Failed) || (self_row && me.state == SelfCheck::Failed)) && !a.up_busy)
            note = T(T_NOTE_UPDATE_FAILED);
        else if (a.game_running) note = T(T_NOTE_RUNNING);
        marquee(note, g_f.footer, 36.f, a.H - 88.f, a.W - 72.f, 28.f, nc, true, 0);
        const float cy = a.H - 42.f;
        launch_button(cy);
        // The keys are the game's own: F confirms, Esc leaves (Enter does what F does)
        float x = 38.f;
        if (a.page == 1) {
            x = hint(x, cy, L"F", T(T_H_OPEN_FOLDER), A_OPEN, a.cat >= 0 && a.cat < (int)a.inst.size() && a.inst[a.cat].present);
            // on a costume's picture: N moves it to the other costume folder (the normal ones <-> the NSFW ones)
            if (nsfw_movable(a)) x = hint(x, cy, L"N", T(a.inst[a.cat].dir == L"costume_mods" ? T_H_MARK_NSFW : T_H_UNMARK_NSFW), A_NSFW);
            x = hint(x, cy, L"G", T(T_H_GAME), A_GAME);
            x = hint(x, cy, L"Esc", T(a.inst_grid ? T_H_BACK : T_H_QUIT), A_QUIT);
        } else if (a.page == 2) {
            if (a.opt_sel == OPT_SUITE)
                x = hint(x, cy, L"F", T(update_installable(a) ? T_H_UPDATE : T_H_CHECK), A_UPDATE, !a.up_busy && !a.game_dir.empty());
            else if (a.opt_sel == OPT_SELF)
                x = hint(x, cy, L"F", T(self_installable(a) ? T_H_UPDATE : T_H_CHECK), A_UPDATE, !a.up_busy);
            if (a.opt_sel == OPT_SUITE && a.kept_zip) x = hint(x, cy, L"O", T(T_H_SHOW_ZIP), A_ZIP);
            x = hint(x, cy, L"G", T(T_H_GAME), A_GAME, a.up_busy != 2);
            x = hint(x, cy, L"Esc", T(T_H_QUIT), A_QUIT);
        } else if (a.items.empty()) {
            x = hint(x, cy, L"F", T(T_H_BROWSE), A_BROWSE);
            x = hint(x, cy, L"G", T(T_H_GAME), A_GAME);
            x = hint(x, cy, L"Esc", T(T_H_QUIT), A_QUIT);
        } else {
            bool any = false;
            for (auto& it : a.items) any = any || (!it.reading && it.c.cat != Category::Unknown && it.done != Item::Installed);
            x = hint(x, cy, L"F", T(T_H_INSTALL_ALL), A_INSTALL, any && !a.installing && !a.game_dir.empty());
            x = hint(x, cy, L"Del", T(T_H_REMOVE), A_REMOVE, !a.installing);
            x = hint(x, cy, L"B", T(T_H_ADD), A_BROWSE);
            x = hint(x, cy, L"Esc", T(T_H_QUIT), A_QUIT);
        }
    }

    HRESULT all() {
        a.hits.clear();
        a.animating = false;
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        background();
        header();
        strip();
        if (a.page == 1) installed();
        else if (a.page == 2) options();
        else if (a.items.empty()) drop_zone();
        else { list(); panel(); }
        footer();
        return rt->EndDraw();
    }
};

HRESULT render(App& a, ID2D1RenderTarget* rt) {
    Painter p(rt, a, now_s());
    return p.all();
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void ensure_visible(App& a) {
    const float rh = 40.f, gap = 8.f, sh = 30.f, sgap = 4.f;
    float y = 0;
    for (int i = 0; i < (int)a.items.size(); ++i) {
        const float h = rh + (item_has_sub(a.items[i]) ? sgap + sh : 0.f);
        if (i == a.sel) {
            const float t0 = y + (a.sel_sub ? rh + sgap : 0.f), t1 = y + (a.sel_sub ? h : rh);
            if (t0 < a.list_scroll) a.list_scroll = t0;
            if (t1 > a.list_scroll + a.list_view) a.list_scroll = t1 - a.list_view;
            return;
        }
        y += h + gap;
    }
}

void move_sel(App& a, int dir) {
    if (a.items.empty()) return;
    int i = a.sel;
    bool sub = a.sel_sub;
    if (dir > 0) {
        if (!sub && item_selectable_sub(a.items[i])) sub = true;
        else if (i + 1 < (int)a.items.size()) { ++i; sub = false; }
    } else {
        if (sub) sub = false;
        else if (i > 0) { --i; sub = item_selectable_sub(a.items[i]); }
    }
    select(a, i, sub);
    ensure_visible(a);
    invalidate(a);
}

void do_action(App& a, const Hit& h) {
    if (a.page == 2 && h.a == A_ROW) { select_option(a, h.item); return; }       // the Options page's rows
    if (a.page == 2 && (h.a == A_LEFT || h.a == A_RIGHT || h.a == A_TOGGLE)) {   // its value row: the language
        select_option(a, 0);
        change_language(a, h.a == A_LEFT ? -1 : 1);
        return;
    }
    switch (h.a) {
        case A_ROW: select(a, h.item, h.sub); invalidate(a); break;
        case A_LEFT: select(a, h.item, h.sub); change_value(a, h.item, h.sub, -1); break;
        case A_RIGHT: case A_TOGGLE: select(a, h.item, h.sub); change_value(a, h.item, h.sub, 1); break;
        case A_BROWSE: browse(a); break;
        case A_GAME: pick_game(a); break;
        case A_INSTALL: install_all(a); break;
        case A_REMOVE: remove_selected(a); break;
        case A_QUIT: if (a.page == 1 && a.inst_grid) { leave_grid(a); break; }   // Esc in the grid: back to the categories
        case A_CLOSE: PostMessageW(a.hwnd, WM_CLOSE, 0, 0); break;
        case A_MIN: ShowWindow(a.hwnd, SW_MINIMIZE); break;
        case A_MAX: ShowWindow(a.hwnd, IsZoomed(a.hwnd) ? SW_RESTORE : SW_MAXIMIZE); break;
        case A_NSFW: move_nsfw_selected(a); break;
        case A_TAB: set_page(a, h.item == -1 ? a.page - 1 : h.item == -2 ? a.page + 1 : h.item); break;   // Q, E, a tab
        case A_CAT: select_category(a, h.item); break;
        case A_CELL:
            if (!a.inst_grid || a.inst_sel != h.item) a.sel_t0 = now_s();
            a.inst_grid = true;
            a.inst_sel = h.item;
            invalidate(a);
            break;
        case A_OPEN: open_category(a); break;
        case A_LAUNCH: launch_game(a); break;
        case A_UPDATE: update_action(a); break;
        case A_ZIP: show_zip(a); break;
        default: break;
    }
}

const Hit* hit_test(App& a, float x, float y) {
    // the last drawn wins (drawn on top): value arrows before their row
    for (int i = (int)a.hits.size() - 1; i >= 0; --i) {
        const Hit& h = a.hits[i];
        if (h.a == A_NONE) continue;
        if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) {
            if (h.a == A_ROW || h.a == A_LEFT || h.a == A_RIGHT || h.a == A_TOGGLE) {
                const D2D1_RECT_F& lr = a.list_rect;
                if (y < lr.top || y >= lr.bottom) continue;
            }
            return &h;
        }
    }
    return nullptr;
}

float dip_scale(HWND h) { return GetDpiForWindow(h) / 96.f; }

bool in_buttons(App& a, float x, float y) {
    for (auto& h : a.hits)
        if ((h.a == A_MIN || h.a == A_MAX || h.a == A_CLOSE || h.a == A_GAME || h.a == A_TAB) && x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) return true;
    return false;
}

void create_rt(App& a) {
    if (a.rt) return;
    RECT rc;
    GetClientRect(a.hwnd, &rc);
    const float dpi = (float)GetDpiForWindow(a.hwnd);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi);
    g_d2d->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(a.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &a.rt);
}

bool needs_timer(App& a) {
    if (a.installing || a.loading || a.up_busy) return true;
    for (auto& it : a.items) if (it.reading || it.done == Item::Installing) return true;
    return a.animating;   // a marquee scrolling
}

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    App& a = g_app;
    switch (m) {
    case WM_NCCALCSIZE:
        if (wp) {
            // no Windows frame: the whole window is ours; maximised, keep it inside the monitor
            if (IsZoomed(h)) {
                NCCALCSIZE_PARAMS* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                const UINT dpi = GetDpiForWindow(h);
                const int f = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += f; p->rgrc[0].right -= f; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
                // Windows puts a maximised window on the monitor's work area (the taskbar stays in sight). An auto-hidden
                // taskbar is not out of the work area: it comes back when the mouse reaches its edge, which a window
                // covering that edge would stop, so the client area leaves it one pixel there.
                APPBARDATA ab{sizeof ab};
                if (SHAppBarMessage(ABM_GETSTATE, &ab) & ABS_AUTOHIDE) {
                    MONITORINFO mi{sizeof mi};
                    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
                    const UINT edges[4] = {ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM};
                    for (UINT e : edges) {
                        APPBARDATA q{sizeof q};
                        q.uEdge = e;
                        q.rc = mi.rcMonitor;
                        if (!SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &q)) continue;
                        if (e == ABE_LEFT) p->rgrc[0].left += 1;
                        else if (e == ABE_TOP) p->rgrc[0].top += 1;
                        else if (e == ABE_RIGHT) p->rgrc[0].right -= 1;
                        else p->rgrc[0].bottom -= 1;
                    }
                }
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        RECT rc;
        GetClientRect(h, &rc);
        const float s = dip_scale(h);
        const int b = int(6 * s);
        if (!IsZoomed(h)) {
            const bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, bo = pt.y >= rc.bottom - b;
            if (t && l) return HTTOPLEFT;
            if (t && r) return HTTOPRIGHT;
            if (bo && l) return HTBOTTOMLEFT;
            if (bo && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (t) return HTTOP;
            if (bo) return HTBOTTOM;
        }
        const float x = pt.x / s, y = pt.y / s;
        if (y < 46.f && !in_buttons(a, x, y)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_ACTIVATE: {
        MARGINS mg{0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(h, &mg);
        if (LOWORD(wp) != WA_INACTIVE) {
            refresh_exists(a);
            if (a.page == 1) scan_installed(a);
            check_game(a, true);
            invalidate(a);
        }
        break;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
        const float s = h ? dip_scale(h) : 1.f;
        mm->ptMinTrackSize.x = LONG(900 * s);
        mm->ptMinTrackSize.y = LONG(580 * s);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (a.rt) a.rt->SetDpi((float)HIWORD(wp), (float)HIWORD(wp));
        return 0;
    }
    case WM_SIZE:
        if (a.rt) a.rt->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        {
            const float s = dip_scale(h);
            a.W = LOWORD(lp) / s;
            a.H = HIWORD(lp) / s;
        }
        invalidate(a);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        create_rt(a);
        if (a.rt) {
            if (render(a, a.rt) == D2DERR_RECREATE_TARGET) {   // made again at the next paint, the pictures' bitmaps with it
                drop_pictures(a, false);
                release(a.rt);
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER: {
        static bool was_starting = false;
        const bool starting = game_starting(a);
        const bool changed = check_game(a);
        if (changed && !a.game_running) queue_update_check(a, false);   // the game was closed: it may have updated itself
        if (changed || starting != was_starting || needs_timer(a)) invalidate(a);
        was_starting = starting;
        return 0;
    }
    case WM_MOUSEMOVE: {
        const float s = dip_scale(h);
        a.mouse = {LONG(GET_X_LPARAM(lp) / s), LONG(GET_Y_LPARAM(lp) / s)};
        if (!a.tracking) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            a.tracking = true;
        }
        invalidate(a);
        return 0;
    }
    case WM_MOUSELEAVE:
        a.mouse = {-1, -1};
        a.tracking = false;
        invalidate(a);
        return 0;
    case WM_LBUTTONDOWN: {
        const float s = dip_scale(h);
        const float x = GET_X_LPARAM(lp) / s, y = GET_Y_LPARAM(lp) / s;
        if (a.inst_note.id != T__COUNT) { a.inst_note = Msg(); invalidate(a); }
        if (const Hit* hit = hit_test(a, x, y)) { Hit copy = *hit; do_action(a, copy); }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(h, &pt);
        const float s = dip_scale(h);
        const float x = pt.x / s, d = -GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 72.f;
        if (a.page == 2) return 0;
        if (a.page == 1 || x >= a.panel_rect.left) a.panel_scroll = std::max(0.f, std::min(a.panel_scroll + d, std::max(0.f, a.panel_h - a.panel_view)));
        else a.list_scroll = std::max(0.f, std::min(a.list_scroll + d, std::max(0.f, a.list_h - a.list_view)));
        invalidate(a);
        return 0;
    }
    case WM_KEYDOWN:
        if (a.inst_note.id != T__COUNT) { a.inst_note = Msg(); invalidate(a); }   // the last move's note: until the next key or click
        // the game's keys: Q / E change the tab (they wrap), F confirms, Esc leaves
        switch (wp) {
            case 'Q': set_page(a, a.page - 1); return 0;
            case 'E': set_page(a, a.page + 1); return 0;
            case 'L': launch_game(a); return 0;
            case 'G': pick_game(a); return 0;
            case VK_ESCAPE:
                if (a.page == 1 && a.inst_grid) leave_grid(a);   // as in the game: out of the grid first
                else PostMessageW(h, WM_CLOSE, 0, 0);
                return 0;
            default: break;
        }
        if (a.page == 1) {
            const float most = std::max(0.f, a.panel_h - a.panel_view), step = std::max(60.f, a.panel_view - 60.f);
            switch (wp) {
                case VK_UP: if (a.inst_grid) move_cell(a, 0, -1); else select_category(a, a.cat - 1); break;
                case VK_DOWN: if (a.inst_grid) move_cell(a, 0, 1); else select_category(a, a.cat + 1); break;
                case VK_LEFT: if (a.inst_grid) move_cell(a, -1, 0); break;
                case VK_RIGHT: if (a.inst_grid) move_cell(a, 1, 0); else enter_grid(a); break;
                case VK_RETURN: case 'F': open_category(a); break;
                case 'N': if (!(lp & (1 << 30))) move_nsfw_selected(a); break;   // not again while the key is held
                case VK_PRIOR: a.panel_scroll = std::max(0.f, a.panel_scroll - step); invalidate(a); break;
                case VK_NEXT: a.panel_scroll = std::min(most, a.panel_scroll + step); invalidate(a); break;
                case VK_HOME: a.panel_scroll = 0; invalidate(a); break;
                case VK_END: a.panel_scroll = most; invalidate(a); break;
                default: break;
            }
            return 0;
        }
        if (a.page == 2) {
            switch (wp) {
                case VK_UP: select_option(a, (a.opt_sel + OPT_ROWS - 1) % OPT_ROWS); break;   // the rows wrap
                case VK_DOWN: select_option(a, (a.opt_sel + 1) % OPT_ROWS); break;
                case VK_LEFT: if (a.opt_sel == 0) change_language(a, -1); break;
                case VK_RIGHT: if (a.opt_sel == 0) change_language(a, 1); break;
                case VK_RETURN: case 'F': if (a.opt_sel != OPT_LANG && !(lp & (1 << 30))) update_action(a); break;   // not again while held
                case 'O': if (a.opt_sel == OPT_SUITE && !(lp & (1 << 30))) show_zip(a); break;
                default: break;
            }
            return 0;
        }
        switch (wp) {
            case VK_UP: move_sel(a, -1); break;
            case VK_DOWN: move_sel(a, 1); break;
            case VK_LEFT: change_value(a, a.sel, a.sel_sub, -1); break;
            case VK_RIGHT: change_value(a, a.sel, a.sel_sub, 1); break;
            case VK_RETURN: case 'F': if (a.items.empty()) browse(a); else install_all(a); break;
            case VK_DELETE: case VK_BACK: if (!a.installing) remove_selected(a); break;
            case 'B': browse(a); break;
            case VK_HOME: select(a, 0, false); ensure_visible(a); invalidate(a); break;
            case VK_END: select(a, (int)a.items.size() - 1, false); ensure_visible(a); invalidate(a); break;
            default: break;
        }
        return 0;
    case WM_DROPFILES: {
        HDROP d = reinterpret_cast<HDROP>(wp);
        const UINT n = DragQueryFileW(d, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(d, i, nullptr, 0);
            std::wstring p(len, L'\0');
            DragQueryFileW(d, i, &p[0], len + 1);
            paths.push_back(p);
        }
        DragFinish(d);
        add_paths(a, paths);
        set_page(a, 0);   // dropped on another tab: the list they went to is shown
        return 0;
    }
    case MSG_INIT: {
        std::unique_ptr<InitResult> r(reinterpret_cast<InitResult*>(lp));
        if (r->gen == a.gen) {
            a.loading = false;
            a.costume_ok = r->costume_ok;
            a.stage_ok = r->stage_ok;
            a.costume_note = r->costume_note;
            a.stage_note = r->stage_note;
        }
        invalidate(a);
        return 0;
    }
    case MSG_UPDATE: {
        std::unique_ptr<UpdateResult> r(reinterpret_cast<UpdateResult*>(lp));
        if (r->gen == a.gen) apply_update(a, *r);   // another game folder since: its own look-up is on the way
        if (a.self_restart) {                       // the sorter replaced itself: the new exe takes over
            a.self_restart = false;
            if (start_self()) { DestroyWindow(h); return 0; }
        }                                           // not started: the footer says to start it again
        invalidate(a);
        return 0;
    }
    case MSG_CLASSIFIED: {
        std::unique_ptr<ClassResult> r(reinterpret_cast<ClassResult*>(lp));
        if (r->gen != a.gen) return 0;
        if (Item* it = a.find(r->item)) {
            it->c = std::move(r->c);
            it->reading = false;
            it->nsfw = it->c.nsfw_guess;
            refresh_exists(a);
        }
        invalidate(a);
        return 0;
    }
    case MSG_PROGRESS:
        if (Item* it = a.find((int)wp)) { it->progress = (float)lp / 1000.f; invalidate(a); }
        return 0;
    case MSG_PICTURE: {
        std::unique_ptr<PicResult> r(reinterpret_cast<PicResult*>(lp));
        Pic& p = a.pics[r->key];
        release(p.bmp);
        p.state = r->ok ? Pic::Ready : Pic::None;
        p.w = r->pic.w;
        p.h = r->pic.h;
        p.px = std::move(r->pic.bgra);
        invalidate(a);
        return 0;
    }
    case MSG_PLACED: {
        std::unique_ptr<PlacedResult> r(reinterpret_cast<PlacedResult*>(lp));
        if (Item* it = a.find(r->item)) {
            it->done = r->r.state == PlaceResult::Installed ? Item::Installed : r->r.state == PlaceResult::Skipped ? Item::Skipped : Item::Failed;
            it->placed_to = r->r.dest.substr(0, r->r.dest.find_last_of(L'\\'));
            it->error = r->r.error;
            it->progress = -1.f;
        }
        if (++a.install_count >= a.install_total) { a.installing = false; a.install_done_note = true; }
        refresh_exists(a);
        invalidate(a);
        return 0;
    }
    case WM_CLOSE:
        if (a.installing || a.up_busy == 2) return 0;   // a copy or an update is running: the window stays until it is done
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------------------------------------
// --shot: the same drawing into a WIC bitmap, saved as a PNG; no window, no focus taken
// ---------------------------------------------------------------------------------------------

bool save_png(IWICImagingFactory* wic, IWICBitmap* bmp, const std::wstring& path) {
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr));
    if (ok) {
        UINT w = 0, h = 0;
        bmp->GetSize(&w, &h);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(fr->SetSize(w, h)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) && SUCCEEDED(fr->WriteSource(bmp, nullptr)) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    }
    release(fr);
    release(enc);
    release(st);
    return ok;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------

int run_shot(const UiOptions& o) {
    App& a = g_app;
    a.opt = o;
    a.W = o.width;
    a.H = o.height;
    a.game_dir = o.game_dir;
    a.shot_ctx.game_dir = o.game_dir;
    a.shot_ctx.work_dir = o.work_dir;
    classify_init(a.shot_ctx);
    a.costume_ok = a.shot_ctx.costume_ok;
    a.stage_ok = a.shot_ctx.stage_ok;
    a.costume_note = a.shot_ctx.costume_note;
    a.stage_note = a.shot_ctx.stage_note;
    for (auto& p : o.items) {
        Item it;
        it.id = a.next_id++;
        it.path = p;
        it.c = classify(p, a.shot_ctx, ++a.seq);
        it.reading = false;
        it.nsfw = it.c.nsfw_guess;
        a.items.push_back(std::move(it));
    }
    for (int n : o.nsfw_yes) if (n >= 0 && n < (int)a.items.size()) a.items[n].nsfw = true;
    for (int n : o.replace) if (n >= 0 && n < (int)a.items.size()) a.items[n].replace = true;
    refresh_exists(a);
    if (o.install) {
        for (auto& it : a.items) {
            if (it.c.cat == Category::Unknown) continue;
            const PlaceResult r = place_item(a.game_dir, it.c, it.nsfw, it.exists && it.replace);
            it.done = r.state == PlaceResult::Installed ? Item::Installed : r.state == PlaceResult::Skipped ? Item::Skipped : Item::Failed;
            it.placed_to = r.dest.substr(0, r.dest.find_last_of(L'\\'));
            it.error = r.error;
        }
        a.install_done_note = true;
        refresh_exists(a);
    }
    if (o.select >= 0 && !a.items.empty()) {
        a.sel = std::min(o.select, (int)a.items.size() - 1);
        a.sel_sub = o.select_sub && item_selectable_sub(a.items[a.sel]);
    }
    a.page = o.page == 1 || o.page == 2 ? o.page : 0;
    a.cat = o.category;
    if (a.page == 1) scan_installed(a);
    if (a.page == 2) a.kept_zip = has_kept_zip(a);
    if (a.page == 1 && o.select >= 0) { a.inst_grid = true; a.inst_sel = o.select; }
    check_game(a, true);
    if (!init_graphics()) return 3;
    for (int k = 0; k < std::abs(o.lang_step); ++k) change_language(a, o.lang_step < 0 ? -1 : 1);
    // --update: the updates row's states, in line (--select 1 on the Options page puts the cursor on it)
    if (a.page == 2 && o.select >= 0) a.opt_sel = std::min(o.select, OPT_ROWS - 1);
    if (o.update == 1 || o.update >= 3) a.up_check = suite_check(a.game_dir, false, o.manifest, &a.self_check);
    if (o.update == 2) a.up_busy = 1;
    if (o.update == 3 || o.update == 5 || o.update == 6) a.up_busy = 2;
    if (o.update == 5 || o.update == 6) {   // 42 % of the download: the suite's, the sorter's own
        a.up_self = o.update == 6;
        a.up_progress.total.store(1000);
        a.up_progress.done.store(420);
        a.up_progress.step.store(sf6up::Progress::Download);
    }
    if (o.update == 4) {
        UpdateResult r;
        r.kind = 1;
        r.inst = suite_install(a.game_dir, a.up_check, language(), a.game_running);
        apply_update(a, r);
    }
    PictureLoader loader(o.work_dir, pictures_cache_dir());
    a.shot_loader = &loader;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return 3;
    IWICBitmap* bmp = nullptr;
    wic->CreateBitmap(UINT(a.W), UINT(a.H), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ID2D1RenderTarget* rt = nullptr;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
    if (!bmp || FAILED(g_d2d->CreateWicBitmapRenderTarget(bmp, p, &rt))) { release(bmp); release(wic); return 3; }
    a.sel_t0 = now_s();
    // twice: the first pass lays the list out (its height, the hit list); the selected row is then scrolled into view, as on screen
    render(a, rt);
    ensure_visible(a);
    if (a.page == 1 && a.inst_grid) {
        const Grid g = grid_layout(a);
        a.inst_sel = std::max(0, std::min(a.inst_sel, (int)g.cells.size() - 1));
        a.inst_grid = !g.cells.empty();
        ensure_cell_visible(a, g);
        if (o.move) move_nsfw_selected(a);   // --move: N on that picture (the tests' folders only), then the page it leaves
    }
    render(a, rt);
    const bool ok = save_png(wic, bmp, o.shot);
    drop_pictures(a, true);
    a.shot_loader = nullptr;
    release(rt);
    release(bmp);
    release(wic);
    return ok ? 0 : 4;
}

int run_window(const UiOptions& o) {
    App& a = g_app;
    a.opt = o;
    self_cleanup();   // the exe a self-update replaced, kept as <exe>.old until then
    if (!init_graphics()) return 3;
    Worker worker;
    worker.work_dir = o.work_dir;
    a.worker = &worker;

    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"SF6_ModSorter";
    RegisterClassExW(&wc);
    const std::wstring title = T(T_TITLE) + L" \u2014 SF6";
    a.hwnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_ACCEPTFILES, wc.lpszClassName, title.c_str(),
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, wc.hInstance, nullptr);
    if (!a.hwnd) return 2;
    worker.target = a.hwnd;
    worker.start();
    PicThread pictures;
    pictures.target = a.hwnd;
    pictures.work_dir = o.work_dir;
    pictures.cache_dir = pictures_cache_dir();
    pictures.start();
    a.pic_thread = &pictures;
    UpdateThread updates;
    updates.start(a.hwnd);
    a.updater = &updates;
    {
        const float s = dip_scale(a.hwnd);
        const int w = int(o.width * s), hgt = int(o.height * s);
        HMONITOR mon = MonitorFromWindow(a.hwnd, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(mon, &mi);
        const RECT& wa = mi.rcWork;
        const int ww = std::min(w, int(wa.right - wa.left)), hh = std::min(hgt, int(wa.bottom - wa.top));
        SetWindowPos(a.hwnd, nullptr, wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - hh) / 2, ww, hh,
                     SWP_NOZORDER | SWP_FRAMECHANGED);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(a.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
        COLORREF border = RGB(0x2A, 0x31, 0x4E);
        DwmSetWindowAttribute(a.hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);
        int corner = 1;   // DWMWCP_DONOTROUND: square, like the game's frames
        DwmSetWindowAttribute(a.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof corner);
    }
    set_game_dir(a, o.game_dir);
    if (!o.items.empty()) add_paths(a, o.items);
    a.cat = o.category;
    if (o.page == 1 || o.page == 2) set_page(a, o.page);
    check_game(a, true);
    ShowWindow(a.hwnd, SW_SHOWNORMAL);
    UpdateWindow(a.hwnd);
    SetTimer(a.hwnd, 1, 33, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker.stop();
    a.worker = nullptr;
    pictures.stop();
    a.pic_thread = nullptr;
    updates.stop();
    a.updater = nullptr;
    drop_pictures(a, true);
    release(a.rt);
    return 0;
}
