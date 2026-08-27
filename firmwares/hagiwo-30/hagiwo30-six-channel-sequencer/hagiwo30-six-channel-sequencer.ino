/* Six Channel Sequencer

Description:
Six-channel, 16-step drum/gate sequencer for the HAGIWO #30 platform. MANUAL
mode edits each channel's steps by hand and keeps them in EEPROM; AUTO mode
plays the PROGMEM pattern banks (techno / dub / house) with fill-ins, repeat
counts and per-channel mutes. The clock input advances the sequence. The engine
itself lives in the shared Hagiwo30SixChannelSequencer library — this sketch is
only the Arduino entry point.

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO #30 sequencer platform (Arduino Nano)
*/
#include <Hagiwo30SixChannelSequencer.h>

SixChannelSequencer sequencer;

void setup() {
  sequencer.setup();
}

void loop() {
  sequencer.loop();
}
