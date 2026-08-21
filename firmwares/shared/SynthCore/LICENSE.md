# MIT License

The platform-pure voice and effect cores in `src/`. Almost all of them are
substantial rewrites of HAGIWO's MOD1 and MOD2 firmware — released by HAGIWO
under CC0 1.0, which places no conditions on derivative works — reworked here
into `dt`-driven, sample-rate-independent engines that the Arduino sketches and
the VCV Rack modules compile from the same source. The MIT terms below cover
that work and travel with it.

Four cores descend from Rob Scape's
<https://github.com/rob-scape/hgw-mod1-firmwares/> rather than from HAGIWO
directly. `LorenzVoice.h` and `DualADEnvCore.h` come via sketches that declare
MIT — MIT to MIT, so the terms below still govern them. `TerrainLfoCore.h` and
`RandomLagCore.h` come via sketches that declare CC0 1.0, which likewise places
no conditions on derivative works. Either way the terms below cover the work
here and travel with it.

Copyright (c) 2026 Madelyn Yeary

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

---

## Carve-out: `src/ClkCore.h`

`ClkCore.h` is **not** covered by the above. It is derived from rabid.audio's
`clock` firmware (<https://github.com/rabidaudio/synthesizer>) under the MIT
License, **Copyright 2015-2020 Julian Knight**. Unlike the CC0-derived cores
beside it, that upstream licence carries conditions: its copyright and
permission notice must ship with every copy or substantial portion. The full
notice is kept verbatim at `firmwares/rabid-audio-clk/LICENSE.md` — keep it
there.
