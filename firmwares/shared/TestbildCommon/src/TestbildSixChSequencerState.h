#pragma once

#include <Arduino.h>

namespace testbild {

struct SixChSequencerState {
  byte step_count = 0;   // Increments on every clock-in. Returns to 1 at 17. Turns OUTPUT on when advanced.
  byte clock_in = 0;     // External clock input state. H=1, L=0
  byte old_clock_in = 0; // Variable for detecting 0->1 edge

  unsigned int ch1_step = 0x8888; // For testing
  byte CH1_output = 0;
  byte CH1_mute = 0; // 0=not muted, 1=muted

  unsigned int ch2_step = 0x0808; // For testing
  byte CH2_output = 0;
  byte CH2_mute = 0; // 0=not muted, 1=muted

  unsigned int ch3_step = 0xCCCC; // For testing
  byte CH3_output = 0;
  byte CH3_mute = 0; // 0=not muted, 1=muted

  unsigned int ch4_step = 0x2222; // For testing
  byte CH4_output = 0;
  byte CH4_mute = 0; // 0=not muted, 1=muted

  unsigned int ch5_step = 0xFFFF; // For testing
  byte CH5_output = 0;
  byte CH5_mute = 0; // 0=not muted, 1=muted

  unsigned int ch6_step = 0x0000; // For testing
  byte CH6_output = 0;
  byte CH6_mute = 0; // 0=not muted, 1=muted

  int oldPosition = -999;
  int newPosition = -999;
  byte enc = 96;      // Currently selected encoder position. Starts with MANUAL shown.
  byte enc_max = 105; // In MANUAL mode max=99(16*6ch+option3+mute6). In AUTO mode max=11(MANUAL,genre,fillin,repeat,sw+mute6)
  unsigned int enc_bit = 0x00;

  byte button = 0;     // 0=OFF,1=ON
  byte old_button = 0; // Debounce
  byte button_on = 0;  // Button state after debounce. 0=OFF,1=ON

  byte mode = 0;        // 0=MANUAL,1=AUTO
  byte count_reset = 0; // Set to 1 to reset count to 0
  byte save = 0;        // Set to 1 to execute save, then immediately return to 0

  byte genre = 0;  // Pattern genre 0=techno,1=dub,2=house
  byte repeat = 2; // 0=4times,1=8times,2=16times,3=32times,4=eternal
  byte fillin = 1; // 0=OFF,1=ON
  byte sw = 0;     // 0=2,1=4,2=8,3=16,4=eternal

  int repeat_max = 4;   // Trigger fill-in when repeat_done reaches this value
  int repeat_done = 0;  // Increments when step reaches 16
  byte test = 0;        // For development
  byte change_bnk1 = 1; // Preset 1
  byte change_bnk2 = 1; // Preset 2
  byte change_bnk3 = 1; // Preset 3 (you can add more if desired)
  byte sw_max = 1;      // Change pattern (random) when sw_done reaches this value
  byte sw_done = 0;     // Increments when repeat_done reaches repeat_max

  unsigned int last_refresh = 0;
  int max_refresh_time = 100;
  bool refresh_display = true;
};

}  // namespace testbild