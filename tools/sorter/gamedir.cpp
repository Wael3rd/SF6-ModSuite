// SF6_ModSorter -- the game folder (auto-detected, remembered), the texts' lookup and the language's choice.
//   1. --game <folder> (tests), else the one remembered in %APPDATA%\SF6_ModSorter\settings.ini (set when the user picks one)
//   2. the folder of SF6_ModSorter.exe when StreetFighter6.exe sits there
//   3. Steam: SteamPath (HKCU) or InstallPath (HKLM), its libraries (steamapps\libraryfolders.vdf), the library whose
//      steamapps\appmanifest_1364780.acf names the game's folder (installdir)

#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "sorter.hpp"

// ---------------------------------------------------------------------------------------------
// Texts
// ---------------------------------------------------------------------------------------------

static int g_lang = 0;   // an index in kLangCodes; 0: English

static int lang_index(const char* code) {
    for (int i = 0; i < kLangCount; ++i) if (_stricmp(code, kLangCodes[i]) == 0) return i;
    return -1;
}

bool set_language(const std::string& code) {
    const int i = lang_index(code.c_str());
    if (i >= 0) g_lang = i;
    return i >= 0;
}
void set_language_index(int index) { g_lang = index >= 0 && index < kLangCount ? index : 0; }
const char* language() { return kLangCodes[g_lang]; }
int language_index() { return g_lang; }
const wchar_t* language_locale() { return kLangLocales[g_lang]; }
bool language_rtl() { return strcmp(kLangCodes[g_lang], "ar") == 0; }

// Windows' language ids: the low 10 bits are the language, the rest its variant
int language_from_windows(unsigned langid) {
    const unsigned primary = langid & 0x3FF, variant = langid >> 10;
    const char* code = "en";
    switch (primary) {
        case 0x0C: code = "fr"; break;
        case 0x11: code = "ja"; break;
        case 0x04: code = variant == 0x02 || variant == 0x04 ? "zh-Hans" : "zh-Hant"; break;   // PRC, Singapore / Taiwan, Hong Kong, Macao
        case 0x10: code = "it"; break;
        case 0x07: code = "de"; break;
        case 0x0A: code = variant == 0x01 || variant == 0x03 ? "es" : "es-419"; break;           // Spain / the Americas
        case 0x19: code = "ru"; break;
        case 0x15: code = "pl"; break;
        case 0x16: code = "pt"; break;
        case 0x12: code = "ko"; break;
        case 0x01: code = "ar"; break;
    }
    const int i = lang_index(code);
    return i >= 0 ? i : 0;
}

static const char* text_of(int lang, TextId id) {
    if (id < 0 || id >= T__COUNT) return "";
    const char* s = kTextTable[lang][id];
    return *s ? s : kTextTable[0][id];   // not translated: English
}

static std::string fill(std::string s, const std::vector<std::string>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string key = "{" + std::to_string(i) + "}";
        for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + args[i].size())) s.replace(at, key.size(), args[i]);
    }
    return s;
}

std::string tr(TextId id) { return text_of(g_lang, id); }
std::string tr(TextId id, const std::vector<std::string>& args) { return fill(tr(id), args); }
std::string tr_en(const Msg& m) { return m.id == T__COUNT ? std::string() : fill(text_of(0, m.id), m.args); }

// ---------------------------------------------------------------------------------------------
// The game folder
// ---------------------------------------------------------------------------------------------

// StreetFighter6.exe is among the processes
bool game_process_running() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(s, &pe)) {
        do found = _wcsicmp(pe.szExeFile, L"StreetFighter6.exe") == 0; while (!found && Process32NextW(s, &pe));
    }
    CloseHandle(s);
    return found;
}

bool is_game_dir(const std::wstring& dir) {
    return !dir.empty() && file_exists_w(dir + L"\\StreetFighter6.exe");
}

static std::wstring reg_string(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &size) != ERROR_SUCCESS &&
        RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, buf, &(size = sizeof buf)) != ERROR_SUCCESS)
        return {};
    std::wstring s = buf;
    for (auto& c : s) if (c == L'/') c = L'\\';
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}

static std::string read_text(const std::wstring& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The values of every "<key>" "<value>" pair of a Valve KeyValues text (escaped backslashes undone)
static std::vector<std::string> vdf_values(const std::string& text, const char* key) {
    std::vector<std::string> out;
    const std::string k = std::string("\"") + key + "\"";
    for (size_t at = text.find(k); at != std::string::npos; at = text.find(k, at + k.size())) {
        size_t a = text.find('"', at + k.size());
        if (a == std::string::npos) break;
        std::string v;
        size_t i = a + 1;
        for (; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            v += text[i];
        }
        out.push_back(v);
    }
    return out;
}

// The folder as the file system spells it (Steam's registry keeps it in lower case)
static std::wstring true_case(const std::wstring& dir) {
    HANDLE h = CreateFileW(dir.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return dir;
    wchar_t buf[4096];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 4096, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= 4096) return dir;
    std::wstring p(buf, n);
    if (p.rfind(L"\\\\?\\UNC\\", 0) == 0) p = L"\\\\" + p.substr(8);
    else if (p.rfind(L"\\\\?\\", 0) == 0) p = p.substr(4);
    return p;
}

std::wstring detect_game_dir() {
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    if (n > 0 && n < MAX_PATH * 4) {
        std::wstring dir(exe, n);
        dir = dir.substr(0, dir.find_last_of(L'\\'));
        if (is_game_dir(dir)) return dir;
    }
    std::vector<std::wstring> steams;
    for (auto s : { reg_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
                    reg_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath") })
        if (!s.empty()) steams.push_back(s);
    for (auto& steam : steams) {
        std::vector<std::wstring> libs = { steam };
        for (auto& p : vdf_values(read_text(steam + L"\\steamapps\\libraryfolders.vdf"), "path")) {
            std::wstring w = from_utf8(p);
            for (auto& c : w) if (c == L'/') c = L'\\';
            while (!w.empty() && w.back() == L'\\') w.pop_back();
            if (!w.empty()) libs.push_back(w);
        }
        for (auto& lib : libs) {
            const std::string acf = read_text(lib + L"\\steamapps\\appmanifest_1364780.acf");
            std::vector<std::string> inst = vdf_values(acf, "installdir");
            const std::wstring dir = lib + L"\\steamapps\\common\\" + (inst.empty() ? std::wstring(L"Street Fighter 6") : from_utf8(inst[0]));
            if (is_game_dir(dir)) return true_case(dir);
        }
    }
    return {};
}

static std::wstring settings_path() {
    // tests: another folder than the user's own (set SF6_MODSORTER_SETTINGS=<folder>)
    if (const wchar_t* test = _wgetenv(L"SF6_MODSORTER_SETTINGS")) if (*test) return test;
    wchar_t* appdata = nullptr;
    std::wstring p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) p = std::wstring(appdata) + L"\\SF6_ModSorter";
    if (appdata) CoTaskMemFree(appdata);
    return p;
}

// Where the small pictures made of the mods' own are kept (this PC only; tests: under their settings folder)
std::wstring pictures_cache_dir() {
    if (const wchar_t* test = _wgetenv(L"SF6_MODSORTER_SETTINGS")) if (*test) return std::wstring(test) + L"\\thumbs";
    wchar_t* local = nullptr;
    std::wstring p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) p = std::wstring(local) + L"\\SF6_ModSorter\\thumbs";
    if (local) CoTaskMemFree(local);
    return p;
}

// settings.ini: "key=value" lines (game_dir, language), in UTF-8
static std::vector<std::pair<std::string, std::string>> read_settings() {
    std::vector<std::pair<std::string, std::string>> out;
    const std::wstring dir = settings_path();
    if (dir.empty()) return out;
    std::istringstream in(read_text(dir + L"\\settings.ini"));
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || eq == std::string::npos || eq == 0) continue;
        out.push_back({line.substr(0, eq), line.substr(eq + 1)});
    }
    return out;
}

static std::string setting(const char* key) {
    for (auto& kv : read_settings()) if (kv.first == key) return kv.second;
    return {};
}

// One value changed, the others kept
static void save_setting(const char* key, const std::string& value) {
    const std::wstring dir = settings_path();
    if (dir.empty()) return;
    auto all = read_settings();
    bool found = false;
    for (auto& kv : all) if (kv.first == key) { kv.second = value; found = true; }
    if (!found) all.push_back({key, value});
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring p = dir + L"\\settings.ini";
    FILE* f = _wfopen((p + L".tmp").c_str(), L"wb");
    if (!f) return;
    std::string text = "; SF6_ModSorter settings\r\n";
    for (auto& kv : all) text += kv.first + "=" + kv.second + "\r\n";
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    MoveFileExW((p + L".tmp").c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring settings_game_dir() { return from_utf8(setting("game_dir")); }
void save_settings_game_dir(const std::wstring& game_dir) { save_setting("game_dir", to_utf8(game_dir)); }
std::string settings_language() { return setting("language"); }
void save_settings_language(const std::string& code) { save_setting("language", code); }
