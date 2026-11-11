// SF6 Mod Suite updater: a package's version read from its main DLL's file, for a host that is not the game (the mod sorter, with the
// game closed). Nothing is loaded and nothing is run: the file's bytes are read as a PE image.
//
// Every package's main DLL exports sf6suite_package_version(), a function that returns a constant string (README, "The version each
// package reports"). The compiler writes it as `lea rax, [rip + d32]` / `ret`: the export's address gives the string's. Any other
// shape (no export: a DLL built before the updater; another compiler's code) reads as "": such a package is never offered an update,
// the rule of the in-game host for a DLL that reports nothing.
#pragma once

#include <string>

namespace sf6up {

// The image's bytes (a whole DLL file). Read as hostile: every offset is checked against the size.
std::string image_package_version(const std::string& image);
// The file at `dll` (64 MiB at most)
std::string file_package_version(const std::wstring& dll);

}  // namespace sf6up
