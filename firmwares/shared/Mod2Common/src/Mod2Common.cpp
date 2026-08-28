// Implementation of the MOD2 shared scaffolding — dual-slice PWM audio plus
// timer IRQ setup, and the panel-mode helpers.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Board support for HAGIWO MOD2 hardware (Seeed Xiao RP2350); the
// pin map and PWM-audio setup follow HAGIWO's CC0 1.0 MOD2 firmware, which
// places no conditions on derivative works.

#include "Mod2Common.h"

namespace mod2 {

void initAudioPwm(uint &audioSlice, uint &timerSlice, irq_handler_t handler) {
  // Audio output: GPIO1, 10-bit, fastest clock.
  pinMode(OUT_PIN, OUTPUT);
  gpio_set_function(OUT_PIN, GPIO_FUNC_PWM);
  audioSlice = pwm_gpio_to_slice_num(OUT_PIN);
  pwm_set_clkdiv(audioSlice, 1);
  pwm_set_wrap(audioSlice, PWM_AUDIO_WRAP);
  pwm_set_enabled(audioSlice, true);

  // Timing slice: GPIO2 wraps every 4096 cycles -> ~36.6 kHz wrap IRQ.
  pinMode(PWM_TIMER_PIN, OUTPUT);
  gpio_set_function(PWM_TIMER_PIN, GPIO_FUNC_PWM);
  timerSlice = pwm_gpio_to_slice_num(PWM_TIMER_PIN);
  pwm_set_clkdiv(timerSlice, 1);
  pwm_set_wrap(timerSlice, PWM_TIMER_WRAP);
  pwm_set_enabled(timerSlice, true);

  pwm_clear_irq(timerSlice);
  pwm_set_irq_enabled(timerSlice, true);
  irq_set_exclusive_handler(PWM_IRQ_WRAP, handler);
  irq_set_enabled(PWM_IRQ_WRAP, true);
}

uint initPwmOutput10bit(uint8_t pin) {
  pinMode(pin, OUTPUT);
  gpio_set_function(pin, GPIO_FUNC_PWM);
  uint slice = pwm_gpio_to_slice_num(pin);
  pwm_set_clkdiv(slice, 1);
  pwm_set_wrap(slice, PWM_AUDIO_WRAP);
  pwm_set_chan_level(slice, PWM_CHAN_B, 0);
  pwm_set_enabled(slice, true);
  return slice;
}

// ---- MCP4822 SPI DAC backend ----------------------------------------------
void dacBegin(uint32_t spiHz) {
  spi_init(spi0, spiHz);
  spi_set_format(spi0, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
  gpio_set_function(DAC_SCK_PIN, GPIO_FUNC_SPI);
  gpio_set_function(DAC_SDI_PIN, GPIO_FUNC_SPI);  // SPI0 TX -> MCP4822 SDI
  pinMode(DAC_CS_PIN, OUTPUT);
  digitalWrite(DAC_CS_PIN, HIGH);                 // idle deselected
}

void dacWriteRaw(uint8_t ch, uint16_t value12) {
  // MCP4822 16-bit command word:
  //   bit15 = A/B (0=VoutA, 1=VoutB), bit13 = GA (1 = gain 1x),
  //   bit12 = SHDN (1 = output active), bits 11..0 = data.
  uint16_t cmd = (uint16_t)((ch ? 0x8000 : 0x0000) | 0x3000 | (value12 & 0x0FFF));
  digitalWrite(DAC_CS_PIN, LOW);
  spi_write16_blocking(spi0, &cmd, 1);
  digitalWrite(DAC_CS_PIN, HIGH);                 // CS rising + LDAC(GND) -> latch
}

bool audioTimerBegin(repeating_timer_t *timer, float sampleRateHz,
                     repeating_timer_callback_t cb) {
  int64_t periodUs = (int64_t)(1000000.0f / sampleRateHz + 0.5f);
  // Negative period => fire every |periodUs| from the scheduled time (no drift).
  return add_repeating_timer_us(-periodUs, cb, nullptr, timer);
}

void Biquad::setBandpass(float fc, float q, float fs) {
  float w0 = 2.0f * PI * fc / fs;
  float sw = sinf(w0), cw = cosf(w0);
  float alpha = sw / (2.0f * q);
  float nb0 = q * alpha, nb1 = 0.0f, nb2 = -q * alpha;
  float na0 = 1.0f + alpha, na1 = -2.0f * cw, na2 = 1.0f - alpha;
  float ia0 = 1.0f / na0;  // normalise by a0
  b0 = nb0 * ia0;
  b1 = nb1 * ia0;
  b2 = nb2 * ia0;
  a1 = na1 * ia0;
  a2 = na2 * ia0;
}

void fillWhiteNoise(float *buf, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    buf[i] = 2.0f * (rand() / (float)RAND_MAX) - 1.0f;
  }
}

bool checkPickup(PickupParam &param, float currentPotValue, float threshold) {
  if (!param.pickupActive) {
    return true;  // no pickup needed
  }

  float target = param.targetValue;

  bool crossedFromBelow = (param.lastPotValue < target - threshold) &&
                          (currentPotValue >= target - threshold);
  bool crossedFromAbove = (param.lastPotValue > target + threshold) &&
                          (currentPotValue <= target + threshold);

  if (crossedFromBelow || crossedFromAbove ||
      fabs(currentPotValue - target) < threshold) {
    param.pickupActive = false;  // pickup complete
    return true;
  }

  param.lastPotValue = currentPotValue;
  return false;  // still waiting
}

}  // namespace mod2
