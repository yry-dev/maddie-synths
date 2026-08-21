# MIT License

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

## Upstream firmware

Nearly every module in this plugin is a port of a firmware in `firmwares/` that
is released under the **CC0 1.0 Universal Public Domain Dedication**, which
places no conditions on derivative works — no attribution requirement, no notice
that has to travel with a copy. The MIT grant above therefore covers the Rack
side of the port outright: the panel layout, the parameter and jack mapping, the
widgets, and the Rack plumbing in `src/`.

Two Mod1 modules — `src/mod1-butterfly.cpp` and `src/mod1-dual-ad-env.cpp` —
port firmwares that are themselves MIT-licensed, forked from Rob Scape's
<https://github.com/rob-scape/hgw-mod1-firmwares/>. Each file's header names
that origin.

## Carve-out: rabid.audio CLK

`src/rabid-audio-clk.cpp` and the `ClkCore.h` engine it drives are **not**
covered by the CC0 story above. They derive from rabid.audio's `clock` module
(<https://github.com/rabidaudio/synthesizer>) and additionally carry:

> MIT License, Copyright 2015-2020 Julian Knight

MIT permits modification and redistribution, including commercially, only so
long as that copyright and permission notice ships with every copy or
substantial portion of the software. The full notice is kept verbatim at
`firmwares/rabid-audio-clk/LICENSE.md`. Keep it there, and ship it with any
distribution of this plugin.

## Not covered

- `.Rack-SDK/` is a vendored copy of the third-party VCV Rack SDK. It is
  distributed under its own license by VCV and nothing in this file applies to
  it.
- `src/mod2-breakbeats.cpp` and `src/mod2-sample.cpp` include a generated
  `sample.h` of PCM data that is separately licensed (Patreon-gated), is
  gitignored, and is not distributed with this repository. The MIT grant above
  covers those two source files only, never the sample data.
