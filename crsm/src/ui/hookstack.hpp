// ui/hookstack — the pre -> post state of a hooked method, per thread, as a small stack (R29). The Lua's hook storage was
// per call: a method that runs again inside itself (GetNextIndex skipping rows, a Function inside a Function), or two
// hooked methods sharing one state that call each other (UIPartsScrollList.Next -> SetSelectedIndex), each get their own
// slot. Every pre pushes, active or not; every post pops (REFramework runs the post of every pre, even when the original
// was skipped). T: a trivially destructible struct (it lives next to __try).
#pragma once

namespace ui {

template <class T, int N = 16>
struct HookStack {
    T items[N]{};
    int depth = 0;
    T* push() {                                  // nullptr past N nested calls (their posts then see an empty state)
        const int i = depth++;
        if (i < 0 || i >= N) return nullptr;
        items[i] = T{};
        return &items[i];
    }
    T pop() {
        if (depth <= 0) { depth = 0; return T{}; }
        const int i = --depth;
        return i < N ? items[i] : T{};
    }
};

}  // namespace ui
