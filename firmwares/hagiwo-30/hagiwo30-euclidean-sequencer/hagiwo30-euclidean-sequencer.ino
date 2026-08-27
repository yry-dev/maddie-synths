/* Euclidean Sequencer

Description:
Six-channel Euclidean gate sequencer for the HAGIWO #30 platform. Each channel
carries its own hit count, rotation offset, step limit and mute, all edited on
the OLED with the rotary encoder; the clock input advances every channel at
once. The engine itself lives in the shared Hagiwo30EuclideanSequencer library
— this sketch is only the Arduino entry point.

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO #30 sequencer platform (Arduino Nano)
*/
#include <Hagiwo30EuclideanSequencer.h>

EuclideanSequencer sequencer;

void setup() {
  sequencer.setup();
}

void loop() {
  sequencer.loop();
}
