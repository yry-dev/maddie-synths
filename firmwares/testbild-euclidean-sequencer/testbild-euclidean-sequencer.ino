
// Encoder setting
#define ENCODER_OPTIMIZE_INTERRUPTS // countermeasure of encoder noise
#include <Encoder.h>
#include <FastGPIO.h>
#include <TestbildCommon.h>
#include <TestbildEuclidDisplay.h>
#include <TestbildEuclidSequencerState.h>
#include "PatternBanks.h"

// rotery encoder
Encoder myEnc(testbild::kEncoderPinA, testbild::kEncoderPinB); // use 3pin 2pin
// push button
testbild::DebouncedActiveLowButton buttonDebounce(300, HIGH);

// display state and sequencer state
testbild::EuclidDisplay euclidDisplay;
testbild::EuclidSequencerState state;

static void setGateOutputByChannel(byte channel, bool high)
{
  switch (channel)
  {
  case 0:
    FastGPIO::Pin<testbild::kOutCh1>::setOutputValue(high);
    break;
  case 1:
    FastGPIO::Pin<testbild::kOutCh2>::setOutputValue(high);
    break;
  case 2:
    FastGPIO::Pin<testbild::kOutCh3>::setOutputValue(high);
    break;
  case 3:
    FastGPIO::Pin<testbild::kOutCh4>::setOutputValue(high);
    break;
  case 4:
    FastGPIO::Pin<testbild::kOutCh5>::setOutputValue(high);
    break;
  case 5:
    FastGPIO::Pin<testbild::kOutCh6>::setOutputValue(high);
    break;
  }
}

static void setAllGateOutputs(bool high)
{
  for (byte ch = 0; ch <= 5; ch++)
  {
    setGateOutputByChannel(ch, high);
  }
}

void setup()
{
  // OLED setting
  euclidDisplay.begin();
  refreshDisplay();

  // pin mode setting
  FastGPIO::Pin<testbild::kEncoderSwitchPin>::setInputPulledUp(); // BUTTON
  FastGPIO::Pin<testbild::kClockPin>::setInput();                 // CLK
}

void loop()
{

  state.old_trg_in = state.trg_in;
  state.oldPosition = state.newPosition;
  //-----------------Rotery encoder read----------------------
  state.newPosition = myEnc.read() / testbild::kEncoderCountsPerClick;

  if (state.newPosition < state.oldPosition)
  { // turn left
    state.oldPosition = state.newPosition;
    state.disp_refresh = 1; // Enable while debugging.
    if (state.select_menu != 0)
    {
      state.select_menu--;
    }
  }

  else if (state.newPosition > state.oldPosition)
  { // turn right
    state.oldPosition = state.newPosition;
    state.disp_refresh = 1; // Enable while debugging.
    state.select_menu++;
  }
  if (state.select_ch != 6)
  { // not random mode
    state.select_menu = constrain(state.select_menu, 0, 5);
  }
  else if (state.select_ch == 6)
  { // random mode
    state.select_menu = constrain(state.select_menu, 0, 1);
  }

  //-----------------push button----------------------

  state.sw = 1;
  buttonDebounce.update(FastGPIO::Pin<testbild::kEncoderSwitchPin>::isInputHigh() ? HIGH : LOW, millis());
  if (buttonDebounce.fell())
  {
    state.sw = 0;
    state.disp_refresh = 1; // Enable while debugging.
  }

  if (state.sw == 0)
  { // push button on
    switch (state.select_menu)
    {
    case 0: // select chanel
      state.select_ch++;
      if (state.select_ch >= 7)
      {
        state.select_ch = 0;
      }
      break;

    case 1: // hits
      if (state.select_ch != 6)
      { // not random mode
        state.hits[state.select_ch]++;
        if (state.hits[state.select_ch] >= 17)
        {
          state.hits[state.select_ch] = 0;
        }
      }
      else if (state.select_ch == 6)
      { // random mode
        state.bar_select++;
        if (state.bar_select >= 4)
        {
          state.bar_select = 0;
        }
      }
      break;

    case 2: // offset
      state.offset[state.select_ch]++;
      if (state.offset[state.select_ch] >= 16)
      {
        state.offset[state.select_ch] = 0;
      }
      break;

    case 3: // limit
      state.limit[state.select_ch]++;
      if (state.limit[state.select_ch] >= 17)
      {
        state.limit[state.select_ch] = 0;
      }

      break;

    case 4: // mute
      state.mute[state.select_ch] = !state.mute[state.select_ch];
      break;

    case 5: // reset
      for (state.k = 0; state.k <= 5; state.k++)
      {
        state.playing_step[state.k] = 0;
      }
      break;
    }
  }

  //-----------------offset setting----------------------
  for (state.k = 0; state.k <= 5; state.k++)
  { // k = 1~6ch
    for (state.i = state.offset[state.k]; state.i <= 15; state.i++)
    {
      state.offset_buf[state.k][state.i - state.offset[state.k]] = (pgm_read_byte(&(euc16[state.hits[state.k]][state.i])));
    }

    for (state.i = 0; state.i < state.offset[state.k]; state.i++)
    {
      state.offset_buf[state.k][16 - state.offset[state.k] + state.i] = (pgm_read_byte(&(euc16[state.hits[state.k]][state.i])));
    }
  }

  //-----------------trigger detect & output----------------------
  state.trg_in = FastGPIO::Pin<testbild::kClockPin>::isInputHigh(); // external trigger in
  if (state.old_trg_in == 0 && state.trg_in == 1)
  {
    state.gate_timer = millis();
    for (state.i = 0; state.i <= 5; state.i++)
    {
      state.playing_step[state.i]++; // When the trigger in, increment the step by 1.
      if (state.playing_step[state.i] >= state.limit[state.i])
      {
        state.playing_step[state.i] = 0; // When the step limit is reached, the step is set back to 0.
      }
    }
    for (state.k = 0; state.k <= 5; state.k++)
    { // output gate signal
      if (state.offset_buf[state.k][state.playing_step[state.k]] == 1 && state.mute[state.k] == 0)
      {
        setGateOutputByChannel(state.k, HIGH);
      }
    }
    // if the bpm is too high we want to still have output but not update the screen so that the arduino is not busy
    if ((millis() - state.last_refresh) > state.max_refresh_time)
    {
      state.disp_refresh = 1;
      state.last_refresh = millis();
    }
    // Updates the display where the trigger was entered.If it update it all the time, the response of gate on will be worse.

    if (state.select_ch == 6)
    { // random mode setting
      state.step_cnt++;
      if (state.step_cnt >= 16)
      {
        state.bar_now++;
        state.step_cnt = 0;
        if (state.bar_now > state.bar_max[state.bar_select])
        {
          state.bar_now = 1;
          Random_change();
        }
      }
    }
  }

  if (state.gate_timer + 100 <= millis())
  { // off all gate , gate time is 10msec
    setAllGateOutputs(LOW);
  }

  if (state.disp_refresh == 1)
  {
    refreshDisplay(); // refresh display
    state.disp_refresh = 0;
  }
}

void Random_change()
{ // when random mode and full of bar_now ,
  for (state.k = 1; state.k <= 5; state.k++)
  {

    if (state.hit_occ[state.k] >= random(1, 100))
    { // hit random change
      state.hits[state.k] = random(state.hit_rng_min[state.k], state.hit_rng_max[state.k]);
    }

    if (state.off_occ[state.k] >= random(1, 100))
    { // hit random change
      state.offset[state.k] = random(0, 16);
    }

    if (state.mute_occ[state.k] >= random(1, 100))
    { // hit random change
      state.mute[state.k] = 1;
    }
    else if (state.mute_occ[state.k] < random(1, 100))
    { // hit random change
      state.mute[state.k] = 0;
    }
  }
}

void refreshDisplay()
{
  testbild::EuclidDisplayState displayState = {
      state.hits,
      state.offset,
      state.mute,
      state.limit,
      state.playing_step,
      state.select_menu,
      state.select_ch,
      state.bar_now,
      state.bar_max,
      state.bar_select,
      state.offset_buf};

  euclidDisplay.render(displayState);
}
