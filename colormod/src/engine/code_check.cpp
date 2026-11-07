// The R18 check of the Color Mod's hooks on the page and at the fight's pick: as the Slots Loader's (plugin/src/codecheck.cpp).
#include "code_check.hpp"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

#include "../common.hpp"

namespace engine::code_check {
namespace {

uintptr_t code_of(uintptr_t p) {                      // the code an entry runs: its jump thunks followed, as REFramework's add_hook does
    __try {
        for (int hop = 0; hop < 4 && p; ++hop) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(p);
            if (b[0] == 0xE9) p = p + 5 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 1);
            else if (b[0] == 0xEB) p = p + 2 + (intptr_t) * reinterpret_cast<const int8_t*>(b + 1);
            else if (b[0] == 0xFF && b[1] == 0x25) p = *reinterpret_cast<const uintptr_t*>(p + 6 + (intptr_t) * reinterpret_cast<const int32_t*>(b + 2));
            else break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return p;
}
uintptr_t exe_base() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
uintptr_t exe_size() {
    const auto* b = reinterpret_cast<const uint8_t*>(exe_base());
    return reinterpret_cast<const IMAGE_NT_HEADERS64*>(b + reinterpret_cast<const IMAGE_DOS_HEADER*>(b)->e_lfanew)->OptionalHeader.SizeOfImage;
}
long long rva(uintptr_t a) { return a ? static_cast<long long>(a - exe_base()) : 0; }
long long code_rva(uintptr_t a) {
    if (!a) return 0;
    return a >= exe_base() && a - exe_base() < exe_size() ? static_cast<long long>(a - exe_base()) : -1;
}
std::string exe_key(uint32_t methods) {
    const auto* base = reinterpret_cast<const uint8_t*>(exe_base());
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    unsigned long long size = 0, time = 0;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        size = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        time = (static_cast<unsigned long long>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
    }
    char b[200];
    snprintf(b, sizeof b, "exe %08lX %08lX %08lX size %llu time %llX methods %u", static_cast<unsigned long>(nt->FileHeader.TimeDateStamp),
             static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage), static_cast<unsigned long>(nt->OptionalHeader.CheckSum), size, time, methods);
    return b;
}
std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string();
}
DWORD WINAPI thread_main(void* p) {
    static_cast<Batch*>(p)->scan_thread();
    return 0;
}
// Every method of the game: those entering each checked method's entry or code. No C++ object here (SEH below).
bool scan_plain(Entry* found, size_t count, const uintptr_t* raw, const uintptr_t* code) {
    auto* tdb = reframework::API::get()->tdb();
    const uint32_t n = tdb->get_num_methods();
    for (uint32_t i = 0; i < n; ++i) {
        if ((i & 0xFFFF) == 0 && cmn::closing()) return false;
        Method* m = tdb->get_method(i);
        if (!m) continue;
        const uintptr_t fn = reinterpret_cast<uintptr_t>(m->get_function_raw());
        if (!fn) continue;
        uintptr_t c = 0;
        bool c_done = false;
        for (size_t k = 0; k < count; ++k) {
            if (!raw[k] || m->get_index() == found[k].index) continue;
            bool same = fn == raw[k] || fn == code[k];
            if (!same) {
                if (!c_done) { c = code_of(fn); c_done = true; }
                same = c && c == code[k];
            }
            if (!same) continue;
            if (found[k].shared++ == 0) found[k].other = m->get_index();
        }
    }
    return true;
}
bool scan_seh(Entry* found, size_t count, const uintptr_t* raw, const uintptr_t* code) {
    __try { return scan_plain(found, count, raw, code); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

}  // namespace

void Batch::add(Method* m, const std::string& label) {
    m_methods.push_back(m);
    Entry e;
    e.label = label;
    uintptr_t raw = 0, code = 0;
    if (m && m->get_function_raw()) {
        raw = reinterpret_cast<uintptr_t>(m->get_function_raw());
        code = code_of(raw);
        e.index = m->get_index();
        e.raw = rva(raw);
        e.code = code_rva(code);
    } else {
        e.label += " (missing)";
    }
    m_raw.push_back(raw);
    m_code.push_back(code);
    m_now.push_back(e);
}

void Batch::start(const char* cache_file, const char* who) {
    m_cache = cache_file;
    m_who = who;
    m_key = exe_key(reframework::API::get()->tdb()->get_num_methods());
    const char* why = "";
    if (slots::codecheck::cache_hit(read_all(m_cache), m_key, m_now, m_found, &why)) {
        cmn::journal("%s: code check (R18): read from %s (same game executable)", who, cache_file);
        m_state.store(1, std::memory_order_release);
        return;
    }
    HANDLE h = CreateThread(nullptr, 0, thread_main, this, 0, nullptr);
    if (!h) {
        cmn::journal("%s: code check (R18) thread not created: no hook (never hooked blind)", who);
        m_state.store(2, std::memory_order_release);
        return;
    }
    CloseHandle(h);
    cmn::journal("%s: code check (R18) of %zu methods started on a thread of its own (%s)", who, m_now.size(), why);
}

void Batch::scan_thread() {
    std::vector<Entry> found = m_now;
    for (auto& e : found) { e.shared = 0; e.other = -1; }
    if (!scan_seh(found.data(), found.size(), m_raw.data(), m_code.data())) { m_state.store(2, std::memory_order_release); return; }
    m_found = found;
    const std::string tmp = m_cache + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) out << slots::codecheck::cache_text(m_key, m_found);
    }
    MoveFileExA(tmp.c_str(), m_cache.c_str(), MOVEFILE_REPLACE_EXISTING);
    m_state.store(1, std::memory_order_release);
}

bool Batch::unique(size_t i) const {
    return m_state.load(std::memory_order_acquire) == 1 && i < m_found.size() && m_methods[i] && m_raw[i] && m_found[i].shared == 0;
}

}  // namespace engine::code_check
