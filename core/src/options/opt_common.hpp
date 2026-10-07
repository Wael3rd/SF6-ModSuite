// opt_common.hpp - what the option engine's files (moved from SF6_ColorModNative, docs/OPTIONS.md) used from the
// mod's common.hpp: the journal, a warning, the core's API and the client its display calls are made for.
#pragma once

#include <sf6core/sf6core_api.h>

namespace cmn {

void journal(const char* fmt, ...);        // the core's log, INFO
void log_warn(const char* fmt, ...);       // the core's log, WARN
const SF6CoreAPI* core();                  // this DLL's own API table
SF6CoreClient* client();                   // the core's "sf6core.options" client (display calls, breaker)

}  // namespace cmn
