//----------------Load Libraries---------------------------
// EEPROM library
#include <EEPROM.h>

// Encoder library
#define ENCODER_OPTIMIZE_INTERRUPTS // Encoder noise countermeasure
#include <Encoder.h>
#include <FastGPIO.h>
#include <TestbildCommon.h>
#include <TestbildSixChDisplay.h>
#include <TestbildSixChSequencerState.h>
#include "ProgramBanks.h"

//----------------Variable Definitions---------------------------

// Rotary encoder settings
Encoder myEnc(testbild::kEncoderPinA, testbild::kEncoderPinB); // For rotary encoder library
testbild::DebouncedActiveLowButton buttonDebounce(300, HIGH);

testbild::SixChSequencerState state;
testbild::SixChDisplay sixChDisplay;


// Options and AUTO mode state live in SequencerState.
// Refresh state lives in SequencerState.

void refreshDisplay();
void save_data();
void change_step();

static void loadStepsFromEeprom()
{
  unsigned int *steps[6] = {
      &state.ch1_step,
      &state.ch2_step,
      &state.ch3_step,
      &state.ch4_step,
      &state.ch5_step,
      &state.ch6_step};

  for (uint8_t ch = 0; ch < 6; ++ch)
  {
    const uint8_t base = 1 + (ch * 2);
    *steps[ch] = (static_cast<uint16_t>(EEPROM.read(base)) << 8) | EEPROM.read(base + 1);
  }
}

static void saveStepsToEeprom()
{
  const unsigned int steps[6] = {
      state.ch1_step,
      state.ch2_step,
      state.ch3_step,
      state.ch4_step,
      state.ch5_step,
      state.ch6_step};

  for (uint8_t ch = 0; ch < 6; ++ch)
  {
    const uint8_t base = 1 + (ch * 2);
    EEPROM.update(base, highByte(steps[ch]));
    EEPROM.update(base + 1, lowByte(steps[ch]));
  }
}

static void applyProgramPattern(uint8_t sectionOffset, bool updateBankSelection)
{
  const unsigned int (*bank)[12] = nullptr;
  byte *bankIndex = nullptr;
  uint8_t randomMax = 0;

  switch (state.genre)
  {
  case 0:
    bank = bnk1_ptn;
    bankIndex = &state.change_bnk1;
    randomMax = 7;
    break;
  case 1:
    bank = bnk2_ptn;
    bankIndex = &state.change_bnk2;
    randomMax = 4;
    break;
  case 2:
    bank = bnk3_ptn;
    bankIndex = &state.change_bnk3;
    randomMax = 4;
    break;
  default:
    return;
  }

  if (updateBankSelection && state.sw_done >= state.sw_max)
  {
    *bankIndex = random(0, randomMax);
  }

  unsigned int *steps[6] = {
      &state.ch1_step,
      &state.ch2_step,
      &state.ch3_step,
      &state.ch4_step,
      &state.ch5_step,
      &state.ch6_step};

  for (uint8_t ch = 0; ch < 6; ++ch)
  {
    *steps[ch] = pgm_read_word(&(bank[*bankIndex][sectionOffset + ch]));
  }
}

static void setChannelGateOutput(uint8_t channel, bool high)
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

static void toggleSelectedStep()
{
  unsigned int *steps[6] = {
      &state.ch1_step,
      &state.ch2_step,
      &state.ch3_step,
      &state.ch4_step,
      &state.ch5_step,
      &state.ch6_step};

  const uint8_t channel = (state.enc - 1) / 16;
  if (channel < 6)
  {
    *steps[channel] ^= state.enc_bit;
  }
}

static void toggleMuteForMenuBase(uint8_t menuBase)
{
  byte *mutes[6] = {
      &state.CH1_mute,
      &state.CH2_mute,
      &state.CH3_mute,
      &state.CH4_mute,
      &state.CH5_mute,
      &state.CH6_mute};

  const uint8_t channel = state.enc - menuBase;
  if (channel < 6)
  {
    *mutes[channel] = !*mutes[channel];
  }
}

static void handleManualButtonPress()
{
  if (state.enc <= 96)
  {
    toggleSelectedStep();
  }
  else if (state.enc == 97)
  {
    state.mode = 1;
    change_step(); // Switch to AUTO mode pattern
  }
  else if (state.enc == 98)
  {
    state.step_count = 1;
  }
  else if (state.enc == 99)
  {
    save_data(); // Save to EEPROM
    state.step_count = 1;
  }
  else if (state.enc >= 100 && state.enc <= 105)
  {
    toggleMuteForMenuBase(100);
  }
}

static void handleAutoButtonPress()
{
  if (state.enc == 1)
  {
    state.mode = 0; // Switch to MANUAL
    state.enc = 97; // When returning from AUTO to MANUAL, keep MANUAL selected. You can change this freely.
    loadStepsFromEeprom();
  }
  else if (state.enc == 2)
  {
    state.genre++;
    if (state.genre >= 3)
    {
      state.genre = 0;
    }
  }
  else if (state.enc == 3)
  {
    state.fillin = !state.fillin;
  }
  else if (state.enc == 4)
  {
    state.repeat++;
    if (state.repeat >= 5)
    { // 0=4times,1=8times,2=16times,3=32times,4=eternal
      state.repeat = 0;
    }
  }
  else if (state.enc == 5)
  {
    state.sw++;
    if (state.sw >= 5)
    { // 0=2,1=4,2=8,3=16,4=eternal
      state.sw = 0;
    }
  }
  else if (state.enc >= 6 && state.enc <= 11)
  {
    toggleMuteForMenuBase(6);
  }
}

void setup()
{

  // Initialize display
  sixChDisplay.begin();

  FastGPIO::Pin<testbild::kEncoderSwitchPin>::setInputPulledUp(); // BUTTON
  FastGPIO::Pin<testbild::kClockPin>::setInput();                 // CLK

  // Load saved data
  loadStepsFromEeprom();

  refreshDisplay();
}

void loop()
{

  state.old_clock_in = state.clock_in;
  state.refresh_display = false;

  //-----------Mode Check----------------

  // MANUAL mode
  if (state.mode == 0)
  {
    state.enc_max = 105;
  }

  // AUTO mode
  else if (state.mode == 1)
  {
    state.enc_max = 11;
  }

  //-----------Read Rotary Encoder----------------
  state.newPosition = myEnc.read() / testbild::kEncoderCountsPerRotation;

  if (state.newPosition < state.oldPosition)
  { // Turned left
    state.oldPosition = state.newPosition;
    state.enc = state.enc - 1;
    state.refresh_display = true;
  }

  else if (state.newPosition > state.oldPosition)
  { // Turned right
    state.oldPosition = state.newPosition;
    state.enc = state.enc + 1;
    state.refresh_display = true;
  }

  if (state.enc <= 0)
  {
    state.enc = state.enc_max; // If selection reaches mode minimum, wrap to maximum
  }
  else if (state.enc > state.enc_max)
  {
    state.enc = 1; // If selection reaches mode maximum, wrap to minimum
  }

  if (state.mode == 0)
  { // Bit handling for the selected pattern
    state.enc_bit = 0;
    bitSet(state.enc_bit, abs(state.enc % 16 - 16));
    if (abs(state.enc % 16 - 16) == 16)
    {
      bitSet(state.enc_bit, 0);
    }
  }

  //----------Read BUTTON----------------

  buttonDebounce.update(FastGPIO::Pin<testbild::kEncoderSwitchPin>::isInputHigh() ? HIGH : LOW, millis());

  if (buttonDebounce.fell())
  { // Register one push only on stable press edge
    state.button_on = 1;
    state.refresh_display = true;
  }
  else
  {
    state.button_on = 0;
  }

  //-------------------MANUAL mode-------------------------
  if (state.mode == 0)
  {
    if (state.button_on == 1)
    { // Press button on selected step to toggle gate ON/OFF
      handleManualButtonPress();
    }
  }

  //-------------------AUTO mode-------------------------
  else if (state.mode == 1)
  {
    if (state.button_on == 1)
    {
      handleAutoButtonPress();
    }
  }
  //-------------AUTO mode processing---------------

  switch (state.repeat)
  {
  case 0:
    state.repeat_max = 4;
    break;

  case 1:
    state.repeat_max = 8;
    break;

  case 2:
    state.repeat_max = 16;
    break;

  case 3:
    state.repeat_max = 32;
    break;

  case 4:
    state.repeat_max = 10000; // ETERNAL
    break;
  }

  switch (state.sw)
  {
  case 0:
    state.sw_max = 2;
    break;

  case 1:
    state.sw_max = 4;
    break;

  case 2:
    state.sw_max = 8;
    break;

  case 3:
    state.sw_max = 16;
    break;

  case 4:
    state.sw_max = 255; // ETERNAL
    break;
  }
  //--------------Detect External Clock Input, Count----------------

  state.clock_in = FastGPIO::Pin<testbild::kClockPin>::isInputHigh();

  if (state.old_clock_in == 0 && state.clock_in == 1)
  {
    if ((millis() - state.last_refresh) > state.max_refresh_time)
    {
      state.refresh_display = true;
      state.last_refresh = millis();
    }
    state.step_count++;
  }

  if (state.step_count >= 17)
  {
    state.step_count = 1;

    if (state.mode == 1)
    {
      state.repeat_done++;

      if (state.fillin == 1 && state.repeat_done == state.repeat_max - 1)
      {
        fillin_step();
      }

      else if (state.repeat_done >= state.repeat_max)
      {
        state.sw_done++;
        state.repeat_done = 0;
        change_step();
      }
    }
  }

  if (state.sw_done >= state.sw_max)
  {
    state.sw_done = 0;
  }

  //--------------Sequence------------------------------

  unsigned int *steps[6] = {
      &state.ch1_step,
      &state.ch2_step,
      &state.ch3_step,
      &state.ch4_step,
      &state.ch5_step,
      &state.ch6_step};
  byte *outputs[6] = {
      &state.CH1_output,
      &state.CH2_output,
      &state.CH3_output,
      &state.CH4_output,
      &state.CH5_output,
      &state.CH6_output};
  byte *mutes[6] = {
      &state.CH1_mute,
      &state.CH2_mute,
      &state.CH3_mute,
      &state.CH4_mute,
      &state.CH5_mute,
      &state.CH6_mute};

  for (uint8_t ch = 0; ch < 6; ++ch)
  {
    *outputs[ch] = bitRead(*steps[ch], 16 - state.step_count);
    setChannelGateOutput(ch, state.clock_in && *outputs[ch] && !*mutes[ch]);
  }

  //--------------OLED Output----------------------------------
  // Note: OLED updates only when clock input arrives, to avoid busy Arduino states.
  if ((state.old_clock_in == 0 && state.clock_in == 1) || (state.refresh_display))
  {
    refreshDisplay();
  }

  //  //For development
  //    Serial.print(state.repeat_done);
  //    Serial.print(",");
  //    Serial.print(state.sw_max);
  //    Serial.println("");
}
//--------------OLED Output----------------------------------
void refreshDisplay()
{
  testbild::SixChDisplayState displayState = {
      {state.ch1_step, state.ch2_step, state.ch3_step, state.ch4_step, state.ch5_step, state.ch6_step},
      {state.CH1_mute, state.CH2_mute, state.CH3_mute, state.CH4_mute, state.CH5_mute, state.CH6_mute},
      state.mode,
      state.enc,
      state.genre,
      state.fillin,
      state.repeat_done,
      state.repeat,
      state.repeat_max,
      state.sw_done,
      state.sw,
      state.sw_max,
      state.step_count};

  sixChDisplay.render(displayState);
}

void save_data()
{
  saveStepsToEeprom();
}

void change_step()
{
  // Automatically change STEP in AUTO mode
  applyProgramPattern(0, true);
}

void fillin_step()
{
  // Pattern settings during fill-in
  applyProgramPattern(6, false);
}
