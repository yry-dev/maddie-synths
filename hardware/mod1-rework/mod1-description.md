### Overview

General-purpose CV/Gate module based on Arduino nano compatible machine.  
By changing the software, you can freely change the function.  
By reducing the number of buffer circuits and compromising performance, free input/output settings and low component costs have been achieved.

With the generated AI, you can create Euclidean rhythm sequencers and random CV generators without programming at all.  
A sample program is provided, but it is assumed that the user will program and use it on their own.

Equipped with a protective circuit, it does not break and does not break other modules.

### Function

Power: +12V only.  
Size: 4HP, depth 28mm  
Variable resistance: 3  
Input/output: 4 in total (0~5V) flexibly changeable from 1in-3out to 4out  
Push SW  
LED: Brightness adjustable by PWM

### Pin Assignment

POT1  A0
POT2  A1
POT3  A2
F1    D17
F2    D9
F3    D10
F4    D11
BUTTON 4
LED 3

### AI Generation procedure

**1\. Enter prompts along the format**  
Fill in the description of the functions of the module according to the format below.  
A summary (overview) is like writing a manual. Write in detail in a sentence that is easy for AI to understand.  
I/O fill in the functions of each pin. I want you to look at the schematic at the destination of the pin connection.

Format

```ruby
Please program a Eurorack modular synthesizer module.

MCU board to use:
Arduino Nano

Programming language to use:
Arduino

Module to create:

Overview:

I/O:

Notes:
Use minimal libraries.
Write comments that are simple but clear for readability.
Generate the full program at once.

Please refer to or base the code on the existing program below when creating it.
```

Example of entry (EN)

```cs
Please program a Eurorack modular synthesizer module.

MCU board to use:
Arduino Nano

Programming language to use:
Arduino

Module to create:
Voltage Controlled Switch

Overview:
There are two external inputs: CV IN1 and CV IN2
There are two external outputs: OUT1 and OUT2

When CV IN1 is Low, output the input value of CV IN2 to OUT1. Output 0 to OUT2. Turn off the LED.
When CV IN1 is High, output the input value of CV IN2 to OUT2. Output 0 to OUT1. Turn on the LED.
The High/Low threshold for CV IN1 is adjustable with an external potentiometer in the 0~5V range.

When the push button is LOW, output the input value of CV IN2 to OUT2. Turn on the LED.

I/O:
A0 pin → Potentiometer (Analog input)
A3 pin → CV IN1 (Analog input)
A4 pin → CV IN2 (Analog input)
D10 → OUT1 (Analog output)
D11 → OUT2 (Analog output)
D4 → Push button (internal pull-up)
D3 → LED

Notes:
Use minimal libraries.
Write comments that are simple but clear for readability.
Generate the full program at once.

Please refer to or base the code on the existing program below when creating it.
```

### Sources
Translated from [original hagiwo post](https://note.com/solder_state/n/nc05d8e8fd311)