// mech::hooks — the game hooks of the native layers, installed once at the start and only for the layers settings.json
// hands to the DLL (none by default: no cost).
//
// Native-UI rule R18: a hook is put on machine code; when another method of the
// game enters the same code (COMDAT-folded bodies, or an entry that jumps into it), a hook on one fires for all of them.
// So, as the Native Core does before its pause-menu hooks (core/src/pausemenu/pausemenu.cpp, fold_scan /
// hook_verdicts): one pass over every method of the game finds who else enters each target's code (jump thunks
// followed); a target that shares its code with a method outside our own hooks with the same pre / post functions is
// REFUSED and logged. A required hook refused or missing hands its layer back to the Lua (it keeps working, in Lua).
#pragma once

#include <string>
#include <vector>

#include <reframework/API.hpp>

#include "common.hpp"

namespace mech::hooks {

struct Spec {
    cmn::Feature layer;
    const char* type;
    const char* sig;                 // method name or "name(param types)" as REFramework finds it
    REFPreHookFn pre;
    REFPostHookFn post;
    const char* without;             // what the layer loses without it (log line)
    bool required = true;            // false: the layer keeps working without it
    bool* installed = nullptr;       // set true when the hook is in place (own hook or covered by a shared body), P4
    // Instead of type / sig: the method whose code sits at this address of the game's exe (its static VA, image base
    // 0x140000000; rebased at run time). Found by the code scan, hooked only when exactly ONE method of the game enters
    // that code (R18); the method's name is logged. type / sig then only name the target in the log.
    uintptr_t va = 0;
};

// Code addresses of the exe named at each start (a log line each: every method of the game entering that code, with
// its entry, and how many share it). Added before install(); scanned in the same pass as the hooks' R18 check.
void add_survey(uintptr_t va, const char* what);

// Called once from mech::start(), game load time: specs of layers the DLL does not own are skipped.
void install(const std::vector<Spec>& specs);
std::string status();

}  // namespace mech::hooks
