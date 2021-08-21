# space-shuttle-illumination

A tiny Arduino project to control the lighting of a space shuttle model

A `shuttle-illumination` project implements a time-limited illumination of a Shuttle Atlantis plastic model
The hardware consists of:
   - 6F22 (a.k.a Krona) 9v battery;
   - Arduino Pro Mini 3v3 (with the voltage regulator and the power LED removed);
   - A power supply module (9v to 3v3 buck converter and a pair of MOSFETs implementing a `power on by button and power off by a signal from an Arduino` logic)
     (see https://circuitjournal.com/arduino-auto-power-off)
   - set of LEDs accompanied by the resistors and a transistor to drive the illumination of the Shuttle;
   - Power button; used to turn on the lights, set the illumination timer or turn off the lights;
       (long press in Idling mode) Turn on the light, set the timer (last timer value used)
       (short press in Active mode) Indicate current timer value (in minutes)
       (short press once again in Active mode less than 5 sec elapsed since the previous key press) Increase current timer value by one (in minutes)
       (long press in Active mode) Turn off the lights
   - Five LEDs to indicate the illumination timer value (displayed as a binary code in range from 1 to 32 minutes)
   - LightLevel button for adjusting the light level (short press to select next light level; there are five levels available)
