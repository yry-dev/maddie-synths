#pragma once
#include <rack.hpp>

using namespace rack;

// HAGIWO MOD1/MOD2 panel pots are wired reverse-polarity: turning a physical
// knob clockwise LOWERS the ADC reading. The firmware and these Rack ports
// share one synthesis core and the same value->sound mapping, so an ordinary
// Rack knob (clockwise = higher value) turns opposite to the hardware. Wrapping
// a knob widget in Reversed<> swaps only its visual sweep, so a Rack knob
// tracks the same direction as the panel pot. The parameter value, tooltips,
// saved presets and the shared core are all left unchanged -- this is purely a
// presentation fix for the hardware's pot wiring.
template <typename TKnob>
struct Reversed : TKnob {
	Reversed() {
		float a = this->minAngle;
		this->minAngle = this->maxAngle;
		this->maxAngle = a;
	}
};

// Declare the Plugin, defined in plugin.cpp
extern Plugin* pluginInstance;

// ─── Mod2 generic-panel support ─────────────────────────────────────────────
// Every Mod2 module is the same physical hardware (HAGIWO's general-purpose
// drum module) running different firmware, so any of them can display the real
// generic hardware faceplate (res/mod2-generic.svg) instead of its per-module
// labeled panel. Each instance remembers its choice, persisted with the patch.

// Base for Mod2 modules: adds panelStyle (0 = labeled, 1 = generic hardware).
// Modules with no other saved state inherit this persistence for free; modules
// that keep their own JSON call the mod2*PanelStyle helpers below instead.
struct Mod2Module : Module {
	int panelStyle = 0;  // 0 = labeled panel, 1 = generic Mod2 hardware panel

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "panelStyle", json_integer(panelStyle));
		return root;
	}
	void dataFromJson(json_t* root) override {
		if (json_t* j = json_object_get(root, "panelStyle"))
			panelStyle = json_integer_value(j);
	}
};

// Merge panelStyle into a Mod2 module that already keeps its own JSON state.
inline void mod2WritePanelStyle(json_t* root, int panelStyle) {
	json_object_set_new(root, "panelStyle", json_integer(panelStyle));
}
inline void mod2ReadPanelStyle(json_t* root, int& panelStyle) {
	if (json_t* j = json_object_get(root, "panelStyle"))
		panelStyle = json_integer_value(j);
}

// Panel that swaps between the labeled art and the shared generic faceplate,
// following its module's panelStyle each frame (like the SDK's ThemedSvgPanel).
struct Mod2Panel : app::SvgPanel {
	int* panelStyle = nullptr;  // -> owning module's field (null in the browser)
	std::shared_ptr<window::Svg> labeledSvg, genericSvg;
	int shown = -1;

	void step() override {
		int s = panelStyle ? *panelStyle : 0;
		if (s != shown) {
			setBackground(s ? genericSvg : labeledSvg);
			shown = s;
		}
		app::SvgPanel::step();
	}
};

// Build + attach a Mod2Panel to a widget, in place of the usual setPanel(...).
// `labeledPath` is the per-module SVG; the generic faceplate is shared.
inline void setMod2Panel(ModuleWidget* mw, Module* module, const std::string& labeledPath) {
	Mod2Panel* panel = new Mod2Panel;
	panel->labeledSvg = APP->window->loadSvg(asset::plugin(pluginInstance, labeledPath));
	panel->genericSvg = APP->window->loadSvg(asset::plugin(pluginInstance, "res/mod2-generic.svg"));
	if (module)
		panel->panelStyle = &static_cast<Mod2Module*>(module)->panelStyle;
	panel->setBackground(panel->labeledSvg);
	mw->setPanel(panel);
}

// Right-click "Panel" chooser for Mod2 modules; call from appendContextMenu.
inline void appendMod2PanelMenu(Menu* menu, Module* module) {
	if (!module)
		return;
	Mod2Module* m = static_cast<Mod2Module*>(module);
	menu->addChild(new MenuSeparator);
	menu->addChild(createIndexPtrSubmenuItem("Panel",
		{"Labeled", "Generic hardware"}, &m->panelStyle));
}

// Declare each Model, defined in each module source file
extern Model* modelButterfly;
extern Model* modelClaves;
// mod1 batch
extern Model* modelEg;
extern Model* modelDualADEnv;
extern Model* modelLFO;
extern Model* modelEuclidean;
extern Model* modelLogicPair;
extern Model* modelRandomCV;
extern Model* modelRandomLag;
extern Model* modelTriggerBurst;
extern Model* modelTapTempo;
extern Model* modelTerrainLFO;
// mod2 batch
extern Model* modelVCO;
extern Model* modelSquareVCO;
extern Model* modelClap;
extern Model* modelHihat;
extern Model* modelKick;
extern Model* modelFMDrum;
extern Model* modelFlux;
extern Model* modelSpiral;
extern Model* modelAcid303;
extern Model* modelBreakbeats;
extern Model* modelSample;
// WIP: Claude-generated maddie synths originals, excluded from the build.
// See the matching WIP block in plugin.cpp for how to re-enable one.
// extern Model* modelBitcrusher;
// extern Model* modelDelay;
// extern Model* modelTapeEcho;
// extern Model* modelDistortion;
// extern Model* modelChorus;
// extern Model* modelResonator;
// extern Model* modelFlanger;
// extern Model* modelPhaser;
// extern Model* modelRingMod;
// mod2 FX batch (WIP: all Claude-generated originals)
// extern Model* modelTremolo;
// extern Model* modelWavefolder;
// extern Model* modelFilter;
// extern Model* modelDynamics;
// extern Model* modelComb;
// extern Model* modelKarplus;
// extern Model* modelFreeze;
// extern Model* modelStutter;
// extern Model* modelReverseDelay;
// extern Model* modelGlitchDelay;
// extern Model* modelFreqShifter;
// extern Model* modelReverb;
// extern Model* modelGranular;
// extern Model* modelPitchShifter;
// extern Model* modelSpectralFreeze;
// extern Model* modelFx;
// SCAFFOLD:extern (new module extern declarations inserted above this line)
