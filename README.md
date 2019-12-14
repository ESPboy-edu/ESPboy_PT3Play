# ESPboy_PT3Play
ESPboy PT3 music player with AY-3-8910 sound chip emulator

v1.0 13.12.2019 initial version by Shiru

<shiru@mail.ru>

<https://www.patreon.com/shiru8bit>

This is a PT3 format chiptune music player. The format is kind of standard de-facto for AY-3-8910 chiptune music on Russian ZX Spectrum clones. About ten thousands songs has been made since mid-90s when it was first introduced.

The program emulates AY sound chip and plays PT3 files loaded into the SPIFFS. The sound is output through the sigma-delta modulation based DAC via the built-in speaker. It also supports i2s stereo DAC if connected (change define in the sketch, no run-time option to switch between outputs at the moment).

Programming lesson learned: do not use sigma-delta while accessing SPIFFS, it crashes the device (i.e. disable sigma-delta while accessing files).

I have included some of my own music to test it out. See zxtunes.com or the TrSongs archive to get much more.
