// Whether a module mapped in this process holds a text in its read-only data: how report_mod.dll knows that a Recording Manager running beside it has its own "Report a bug"
// (the literal only that version holds, host.cpp). Pure reads, under SEH.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstring>

namespace report {

inline bool image_holds(const void* module, const char* text, size_t n) {
    __try {
        auto* base = static_cast<const unsigned char*>(module);
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (std::memcmp(sec->Name, ".rdata", 6) != 0) continue;
            const unsigned char* p = base + sec->VirtualAddress;
            const size_t size = sec->Misc.VirtualSize;
            for (size_t k = 0; k + n <= size; ++k)
                if (p[k] == (unsigned char)text[0] && std::memcmp(p + k, text, n) == 0) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;                                 // unreadable: the safe answer here is yes, no second entry
    }
    return false;
}

}  // namespace report
