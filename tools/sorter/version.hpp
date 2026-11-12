// SF6_ModSorter -- its version. Raised by hand when the sorter changes: packaging/make_sorter_package.py names the zip after it,
// packaging/make_manifest.py writes it into the release's manifest ("tools"), and the sorter compares it with that entry to know
// a newer one is out (suite_update.hpp, SelfCheck).
#pragma once

inline constexpr const char* kSorterVersion = "1.1.2";
