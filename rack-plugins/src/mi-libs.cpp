/*
	Single-compilation-unit bodies for the external Mutable Instruments
	libraries (poetaster/arduinoMI packaging) shared by the MI ports:
	mod2-tides (Tiny Tides) and mod2-braids (Tiny Braids). The lookup tables
	and out-of-line definitions must be compiled exactly once in the plugin,
	so they live here and nowhere else — the module .cpps include only the
	library headers.

	The libraries are deliberately not vendored in this repo: install locally
	per the root README.md ("MOD2 Braids / Tides"); CI fetches them at the
	commits pinned in .github/mi-libs.env. To build without them, exclude
	this file AND the MI module .cpps via WIP_SOURCES (see the Makefile).

	License:
	MIT License, Copyright (c) 2026 Madelyn Yeary — see rack-plugins/LICENSE.md.
	The included Mutable Instruments sources are Copyright (c) Émilie Gillet,
	MIT License.
*/

#if !__has_include(<tides2/poly_slope_generator.h>) || !__has_include(<braids/macro_oscillator.h>)
#error "mi-libs.cpp needs the external Mutable Instruments STMLIB/TIDES/BRAIDS libraries (poetaster/arduinoMI) — see README.md 'MOD2 Braids / Tides'. Install them (MI_LIB_DIR in rack-plugins/Makefile), or exclude mi-libs.cpp, mod2-tides.cpp and mod2-braids.cpp via WIP_SOURCES + plugin.{hpp,cpp} + the root plugin.json."
#endif

// TEST is Mutable's own desktop-build switch: it turns off embedded-only
// attributes in the stmlib headers (IN_RAM's ELF ".ramtext" section is invalid
// for Mach-O/PE). Audible Instruments builds the same sources the same way.
#define TEST
#include <STMLIB.h>
#include <stmlib_all.cpp>
#include <tides_all.cpp>
#include <braids_all.cpp>

// poetaster's arduinoMI packaging comments out Settings::Save() and
// Settings::CheckPaques() (they need the STM32 EEPROM storage layer), but
// Settings::Init() still *calls* CheckPaques(), so the amalgamation leaves the
// symbol undefined. The plugin links with -undefined dynamic_lookup, which
// hides that until Rack dlopens the dylib and every maddie-synths module
// vanishes. Supply the body: the marquee easter egg is permanently off here.
namespace braids {
void Settings::CheckPaques() {
	paques_ = false;
}
}  // namespace braids
