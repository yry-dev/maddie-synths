#!/usr/bin/env python3
"""Scaffold a new VCV Rack module that shares a core with a firmware.

Generates (and registers) the four files a port needs, following the pattern in
vcvrack/PORTING.md and the Claves/Butterfly references:

  firmwares/shared/SynthCore/src/<Slug>Core.h   pure sc:: voice-core stub
  vcvrack/src/<Slug>.cpp                          Module + Widget + model line
  vcvrack/res/<Slug>.svg                          placeholder panel
  + registration in vcvrack/src/plugin.{hpp,cpp} (at SCAFFOLD: sentinels)
  + a module entry in vcvrack/plugin.json

The generated DSP/widget is a STUB (3 knobs, trigger in, CV in, audio out, LED)
clearly marked TODO — fill in the real algorithm from the firmware, then run
scripts/check-vcv.fish. The panel is a placeholder: real panels come from the
KiCad faceplate pipeline (see scripts/panels/tools), so replace res/<Slug>.svg and its
widget hole-centre coordinates from the faceplate.

Usage: _new_vcv_module.py <repo_root> <Slug> "<Display Name>" "<tag1,tag2>" "<description>"
"""
import json
import os
import re
import sys

CORE_TMPL = '''#pragma once

// {disp} voice — shared core for the {slug} module.  STUB — fill in from firmware.
//
// Used by:
//   - firmwares/<firmware>/<firmware>.ino   (TODO: wire this core in)
//   - vcvrack/src/{slug}.cpp
//
// Pure C++: include only sc_math.h / sc_dsp.h. NO Arduino.h, rack.hpp, Pico SDK.
// float only, no heap, no STL — must compile on AVR, RP2350 and desktop.

#include "sc_math.h"
// #include "sc_dsp.h"   // uncomment for Biquad / noise / soft-clip / DC-block

namespace sc {{

struct {slug}Frame {{
  float audio;  // -1..+1   (or CV; scale on each platform)
  float env;    // 0..1     (LED brightness)
}};

struct {slug}Voice {{
  bool  playing = false;
  float phase   = 0.0f;
  // TODO: real state

  void reset() {{ playing = false; phase = 0.0f; }}

  // TODO: a strike()/setParams() that latches normalised 0..1 controls.
  void strike() {{ phase = 0.0f; playing = true; }}

  // Advance one sample of `dt` seconds (sample-rate independent).
  // Firmware passes 1/AUDIO_FS; VCV passes args.sampleTime.
  {slug}Frame process(float dt) {{
    {slug}Frame f = {{0.0f, 0.0f}};
    (void)dt;
    // TODO: real synthesis
    return f;
  }}
}};

}}  // namespace sc
'''

CPP_TMPL = '''#include "plugin.hpp"
#include <{slug}Core.h>  // Shared {disp} voice (also used by the firmware)

/*
\t{disp} — TODO one-line description.

\tPort of firmwares/<firmware>/<firmware>.ino. Synthesis lives in sc::{slug}Voice;
\tthis file owns only the Rack I/O. STUB — fill in params/jacks to match the
\tfirmware, and replace the placeholder panel + coordinates with the KiCad
\tfaceplate output (see scripts/panels/tools).
*/

struct {slug} : Module {{
\tenum ParamId {{ KNOB_A_PARAM, KNOB_B_PARAM, KNOB_C_PARAM, TRIG_PARAM, PARAMS_LEN }};
\tenum InputId {{ TRIG_INPUT, CV_INPUT, INPUTS_LEN }};
\tenum OutputId {{ AUDIO_OUTPUT, OUTPUTS_LEN }};
\tenum LightId {{ ENV_LIGHT, LIGHTS_LEN }};

\tsc::{slug}Voice voice;
\tdsp::SchmittTrigger gateTrigger;
\tdsp::BooleanTrigger buttonTrigger;

\t{slug}() {{
\t\tconfig(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
\t\tconfigParam(KNOB_A_PARAM, 0.f, 1.f, 0.5f, "A");
\t\tconfigParam(KNOB_B_PARAM, 0.f, 1.f, 0.5f, "B");
\t\tconfigParam(KNOB_C_PARAM, 0.f, 1.f, 0.5f, "C");
\t\tconfigButton(TRIG_PARAM, "Manual trigger");
\t\tconfigInput(TRIG_INPUT, "Trigger");
\t\tconfigInput(CV_INPUT, "CV");
\t\tconfigOutput(AUDIO_OUTPUT, "Audio");
\t}}

\tvoid onReset() override {{ voice.reset(); }}

\tvoid process(const ProcessArgs& args) override {{
\t\tconst bool gate = gateTrigger.process(inputs[TRIG_INPUT].getVoltage(), 0.1f, 1.f);
\t\tconst bool button = buttonTrigger.process(params[TRIG_PARAM].getValue() > 0.5f);
\t\tif (gate || button)
\t\t\tvoice.strike();  // TODO: latch params here

\t\tconst sc::{slug}Frame f = voice.process(args.sampleTime);
\t\toutputs[AUDIO_OUTPUT].setVoltage(f.audio * 5.f);  // -1..1 -> +/-5V
\t\tlights[ENV_LIGHT].setBrightnessSmooth(f.env, args.sampleTime);
\t}}
}};

struct {slug}Widget : ModuleWidget {{
\t{slug}Widget({slug}* module) {{
\t\tsetModule(module);
\t\tsetPanel(createPanel(asset::plugin(pluginInstance, "res/{slug}.svg")));

\t\t// TODO: replace these placeholder coords with KiCad faceplate hole centres.
\t\tconst float cx = 15.24f;  // 6 HP center
\t\taddChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(cx, 18.5f)), module, {slug}::ENV_LIGHT));
\t\taddParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 30.f)), module, {slug}::KNOB_A_PARAM));
\t\taddParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 47.f)), module, {slug}::KNOB_B_PARAM));
\t\taddParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(cx, 64.f)), module, {slug}::KNOB_C_PARAM));
\t\taddParam(createParamCentered<VCVButton>(mm2px(Vec(cx, 82.f)), module, {slug}::TRIG_PARAM));
\t\taddInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.16f, 98.f)), module, {slug}::TRIG_INPUT));
\t\taddInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.32f, 98.f)), module, {slug}::CV_INPUT));
\t\taddOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(20.32f, 113.f)), module, {slug}::AUDIO_OUTPUT));
\t}}
}};

Model* model{slug} = createModel<{slug}, {slug}Widget>("{slug}");
'''

SVG_TMPL = '''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="30.48mm" height="128.5mm" viewBox="0 0 90 379.42">
  <rect width="90" height="379.42" fill="#1b1b22"/>
  <rect x="2" y="2" width="86" height="375.42" fill="none" stroke="#3a3a48" stroke-width="1"/>
  <g font-family="Helvetica, Arial, sans-serif" fill="#e6e6ee" text-anchor="middle">
    <text x="45" y="26" font-size="11" font-weight="bold" letter-spacing="1">{disp_upper}</text>
    <text x="45" y="38" font-size="6" fill="#9a9aaa" letter-spacing="1">PLACEHOLDER PANEL</text>
    <text x="45" y="112" font-size="9">A</text>
    <text x="45" y="162" font-size="9">B</text>
    <text x="45" y="213" font-size="9">C</text>
    <text x="45" y="263" font-size="8">TRIG</text>
  </g>
</svg>
'''


def insert_before_sentinel(path, sentinel, line):
    txt = open(path).read()
    if line.strip() in txt:
        return  # already present
    if sentinel not in txt:
        sys.exit(f"ERROR: sentinel {sentinel!r} not found in {path}")
    txt = txt.replace(sentinel, line + "\n" + sentinel, 1)
    open(path, "w").write(txt)


def main():
    root, slug, disp, tags_csv, desc = sys.argv[1:6]
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9]*", slug):
        sys.exit(f"ERROR: slug {slug!r} must be alphanumeric, starting with a letter")
    core = os.path.join(root, "firmwares/shared/SynthCore/src", slug + "Core.h")
    cpp = os.path.join(root, "vcvrack/src", slug + ".cpp")
    svg = os.path.join(root, "vcvrack/res", slug + ".svg")
    for p in (core, cpp, svg):
        if os.path.exists(p):
            sys.exit(f"ERROR: {p} already exists — refusing to overwrite")

    open(core, "w").write(CORE_TMPL.format(slug=slug, disp=disp))
    open(cpp, "w").write(CPP_TMPL.format(slug=slug, disp=disp))
    open(svg, "w").write(SVG_TMPL.format(disp_upper=disp.upper()))

    insert_before_sentinel(
        os.path.join(root, "vcvrack/src/plugin.hpp"),
        "// SCAFFOLD:extern", f"extern Model* model{slug};")
    insert_before_sentinel(
        os.path.join(root, "vcvrack/src/plugin.cpp"),
        "\t// SCAFFOLD:addModel", f"\tp->addModel(model{slug});")

    pj_path = os.path.join(root, "vcvrack/plugin.json")
    pj = json.load(open(pj_path))
    if any(m["slug"] == slug for m in pj["modules"]):
        print(f"  (plugin.json already has {slug})")
    else:
        pj["modules"].append({
            "slug": slug, "name": disp, "description": desc, "manualUrl": "",
            "tags": [t.strip() for t in tags_csv.split(",") if t.strip()],
        })
        json.dump(pj, open(pj_path, "w"), indent=2)
        open(pj_path, "a").write("\n")

    print(f"Scaffolded {slug}:")
    for p in (core, cpp, svg):
        print("  + " + os.path.relpath(p, root))
    print("  + registered in plugin.hpp / plugin.cpp / plugin.json")
    print("\nNext: fill in the core DSP from the firmware, wire the core into the")
    print("firmware .ino, replace the placeholder panel + coords from scripts/panels/tools,")
    print("then run scripts/check-vcv.fish")


if __name__ == "__main__":
    main()
