#pragma once

#include <Arduino.h>

// Opt-in RP2350 overclock helper for MOD2 firmwares (Seeed Xiao RP2350).
//
// Call mod2Overclock() as the FIRST thing in setup() — before Serial.begin()
// and before any audio init — and it raises the system clock while keeping
// the QSPI flash at or below the speed the boot ROM configured (the XIAO's
// 2 MB flash part is only rated ~104 MHz, so the flash divider is widened
// BEFORE the clock goes up and never tightened past the boot margin).
//
// Why bother: PWM output resolution scales with the system clock. PWMAudio
// (mod2-tides / mod2-braids) derives its timing from the live clock, so a
// sketch that overclocks before DAC.begin() keeps its sample rate AND gains
// PWM counts per sample — e.g. 48 kHz audio gets ~3125 steps (~11.6 bits) at
// the stock 150 MHz but ~5200 steps (~12.3 bits) at 250 MHz, a lower
// quantization noise floor for free. It also buys plain DSP headroom.
//
//   void setup() {
//     mod2Overclock();          // 250 MHz; or mod2Overclock(300000000u)
//     ...
//   }
//
// Caveats:
//  - Sketches on the Mod2Common dual-slice PWM path must NOT use this as-is:
//    mod2::AUDIO_FS is a constexpr derived from a 150 MHz SYS_CLOCK, so the
//    wrap-IRQ sample rate scales with the overclock and everything detunes
//    sharp. PWMAudio-based sketches are unaffected (runtime clock).
//  - Call before Serial/I2C/SPI init: clk_peri follows clk_sys, and baud
//    dividers computed at the old clock come out wrong afterwards.
//  - USB, millis()/micros(), the hardware alarm timers and the ADC run from
//    fixed clocks (48 MHz / 1 MHz) and are unaffected.
//  - 250 MHz runs at a mild 1.15 V core bump; up to 300 MHz uses 1.20 V (both
//    inside the SDK's normal voltage limit — no vreg_disable_voltage_limit()
//    here on purpose; the 400+ MHz territory needs that plus tuned QMI RX
//    delays and is out of scope for a shared helper).
//  - RP2040 builds (e.g. `make MOD2_FQBN=rp2040:rp2040:rpipico`) compile but
//    return false: the RP2040 flash divider lives in a different peripheral
//    (SSI) and isn't handled here.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. The safe-divider-first clock-switch ordering follows the pattern
// discussed for the RP2350 by the PicoVintageSynthCollection project.

#if defined(PICO_RP2350)
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/qmi.h"
#endif

// Raise the system clock to `hz` (default 250 MHz). Returns true on success;
// on failure (unachievable PLL frequency) the clock, flash divider and core
// voltage are left at their previous settings. Safe to call only once, from
// core 0, at the top of setup().
inline bool mod2Overclock(uint32_t hz = 250000000u) {
#if !defined(PICO_RP2350)
  (void) hz;
  return false;
#else
  const uint32_t oldSys = clock_get_hz(clk_sys);
  if (hz <= oldSys) {
    return true;  // nothing to do (already at or above the requested clock)
  }

  // Flash speed the boot ROM chose (clk_sys / QMI divider). Keep that margin:
  // the new divider is the smallest that leaves flash no faster than boot.
  const uint32_t oldTiming = qmi_hw->m[0].timing;
  const uint32_t oldDiv = (oldTiming & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
  const uint32_t flashHz = oldSys / (oldDiv ? oldDiv : 1);
  uint32_t newDiv = (hz + flashHz - 1) / flashHz;  // ceil
  if (newDiv < oldDiv) newDiv = oldDiv;
  if (newDiv > (QMI_M0_TIMING_CLKDIV_BITS >> QMI_M0_TIMING_CLKDIV_LSB))
    return false;  // divider field would overflow — request is unreasonable

  // Core voltage first, and let the regulator settle before the PLL moves.
  const enum vreg_voltage volt = (hz > 266000000u) ? VREG_VOLTAGE_1_20 : VREG_VOLTAGE_1_15;
  vreg_set_voltage(volt);
  delay(10);

  // Widen the flash divider BEFORE raising clk_sys, so XIP never overruns the
  // flash part mid-switch. Barriers flush the pipeline around the retiming.
  qmi_hw->m[0].timing = (oldTiming & ~QMI_M0_TIMING_CLKDIV_BITS)
                        | (newDiv << QMI_M0_TIMING_CLKDIV_LSB);
  __asm volatile("dsb\n\tisb" ::: "memory");

  if (!set_sys_clock_khz(hz / 1000, false)) {
    // PLL couldn't hit the target; put the divider and voltage back.
    qmi_hw->m[0].timing = oldTiming;
    __asm volatile("dsb\n\tisb" ::: "memory");
    vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
    return false;
  }
  return true;
#endif
}
