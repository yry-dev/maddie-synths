#pragma once

#include <Arduino.h>

namespace hagiwo30 {

struct EuclidSequencerState {
  int oldPosition = -999;
  int newPosition = -999;

  byte hits[6] = {4, 4, 5, 3, 2, 16};
  byte offset[6] = {0, 2, 0, 8, 3, 9};
  bool mute[6] = {0, 0, 0, 0, 0, 0};
  byte limit[6] = {16, 16, 16, 16, 16, 16};

  bool trg_in = 0;
  bool old_trg_in = 0;
  byte playing_step[6] = {0, 0, 0, 0, 0, 0};
  unsigned long gate_timer = 0;

  byte select_menu = 0;
  byte select_ch = 0;

  unsigned int last_refresh = 0;
  int max_refresh_time = 70;
  bool disp_refresh = 1;

  byte bar_now = 1;
  byte bar_select = 1;
  byte step_cnt = 0;
};

}  // namespace hagiwo30