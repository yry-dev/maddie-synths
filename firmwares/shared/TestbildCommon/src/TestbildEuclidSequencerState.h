#pragma once

#include <Arduino.h>

namespace testbild {

struct EuclidSequencerState {
  int oldPosition = -999;
  int newPosition = -999;
  int i = 0;

  bool sw = 0;

  byte hits[6] = {4, 4, 5, 3, 2, 16};
  byte offset[6] = {0, 2, 0, 8, 3, 9};
  bool mute[6] = {0, 0, 0, 0, 0, 0};
  byte limit[6] = {16, 16, 16, 16, 16, 16};

  byte k = 0;
  bool offset_buf[6][16];

  bool trg_in = 0;
  bool old_trg_in = 0;
  byte playing_step[6] = {0, 0, 0, 0, 0, 0};
  unsigned long gate_timer = 0;

  byte select_menu = 0;
  byte select_ch = 0;

  unsigned int last_refresh = 0;
  int max_refresh_time = 70;
  bool disp_refresh = 1;

  byte hit_occ[6] = {0, 10, 20, 20, 40, 80};
  byte off_occ[6] = {10, 20, 20, 30, 40, 20};
  byte mute_occ[6] = {20, 20, 20, 20, 20, 20};
  byte hit_rng_max[6] = {0, 14, 16, 8, 9, 16};
  byte hit_rng_min[6] = {0, 13, 6, 1, 5, 10};

  byte bar_now = 1;
  byte bar_max[4] = {2, 4, 8, 16};
  byte bar_select = 1;
  byte step_cnt = 0;
};

}  // namespace testbild