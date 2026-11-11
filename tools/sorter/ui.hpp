// SF6_ModSorter -- the window (Direct2D + DirectWrite, frameless, per-monitor DPI aware) and its offscreen render (--shot).
#pragma once

#include <string>
#include <vector>

struct UiOptions {
    std::wstring game_dir;             // "" when none was found
    std::wstring work_dir;             // the sorter's scratch folder
    std::vector<std::wstring> items;   // added at start (--items)
    std::wstring shot;                 // --shot <png>: render offscreen and exit, no window
    int select = -1;                   // --select N: the item selected (0 first; -1: the first)
    bool select_sub = false;           // --select-sub N: its Skip / Replace row instead
    bool install = false;              // --install: place every ready item before the shot (tests: an installed row)
    std::vector<int> nsfw_yes;         // --nsfw N: NSFW Yes on item N (tests)
    std::vector<int> replace;          // --replace N: Replace on item N (tests)
    float width = 1180, height = 720;  // the window's size in DIPs (--size WxH)
    int lang_step = 0;                 // --lang-step N (tests): the Options tab's Language row pressed N times before the render
    int page = 0;                      // --page installed: 1, the Installed tab; --page options: 2, the Options tab
    int category = 0;                  // --category N: the category selected there
    int running = -1;                  // --running 0|1|2: as if the game were not running / running / being started (tests); -1: look
    bool move = false;                 // --move (tests): N pressed on the Installed tab's selected picture before the render
    bool maximized = false;            // --maximized (tests): the header's buttons as in a maximised window
    int update = 0;                    // --update check|checking|installing|install|downloading|self-downloading (tests): the
                                       // Options tab's updates rows after the check (1), as if looking up (2), as if installing
                                       // (3), after the suite's install (4), 42 % into the suite's download (5), the sorter's (6)
    std::wstring manifest;             // --manifest <file> (tests): the release's manifest read from a file, never installed
    std::wstring assets;               // --assets <folder> (tests, with --manifest): the sorter's own zip read there, which
                                       // is the one thing a manifest from a file installs (the exe that runs replaces itself)
};

int run_window(const UiOptions& o);
int run_shot(const UiOptions& o);
