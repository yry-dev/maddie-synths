#pragma once
// Compatibility shim for the Rack (desktop) build only.
//
// The mod2-sample firmware's generated sample.h does `#include <pgmspace.h>`
// and decorates its PCM arrays with PROGMEM (an Arduino/RP2040 flash-storage
// macro). On desktop there is no such header and PROGMEM is meaningless, so we
// provide an empty header here and neuter the macro. This dir is added to the
// Rack build's include path (-Isrc/compat) and is NOT on the firmware build's
// path, so the real pgmspace.h is still used when compiling firmware.
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PGM_P
#define PGM_P const char*
#endif
