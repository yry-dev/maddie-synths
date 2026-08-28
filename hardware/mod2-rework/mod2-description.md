### Overview

Seeed Xiao RP2350-based general-purpose Drum module.  
By changing the software, you can freely change the function.  
The sound quality is not good because the audio output is PWM, but it realizes simple, general-purpose, and inexpensive hardware.

With a generation of AI such as ChatGPT, you can generate most of the program.  
A sample program is provided, but it is assumed that the user will program and use it on their own.

There is a mod1 in the module of the same concept, but this time it is the audio output version.

<iframe data-src="https://note.com/embed/notes/nc05d8e8fd311" src="https://note.com/embed/notes/nc05d8e8fd311" height="207px"></iframe>

### Function

Power supply: +12V, -12V, +5V  
Size: 4HP, depth 28mm  
Variable resistance: 3  
Input: 2 Gate in / 1 CV in  
Output: 1 out ( PWM, Vpp 10V )  
Push SW  
LED: Brightness adjustable by PWM

### Pin Assignment

POT1     A0
POT2     A1
POT3     A2
IN1      GPIO7
IN2      GPIO0
CV       A2 (Shared with POT3)
BUTTON   GPIO6  
OUT      GPIO1    10-bit PWM audio output (~36.6 kHz)
LED      GPIO5

### AI Generation procedure