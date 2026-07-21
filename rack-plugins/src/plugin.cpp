#include "plugin.hpp"

Plugin* pluginInstance;

void init(Plugin* p) {
	pluginInstance = p;

	// Add modules here
	p->addModel(modelButterfly);
	p->addModel(modelClaves);
	// mod1 batch
	p->addModel(modelEg);
	p->addModel(modelDualADEnv);
	p->addModel(modelLFO);
	p->addModel(modelEuclidean);
	p->addModel(modelLogicPair);
	p->addModel(modelRandomCV);
	p->addModel(modelRandomLag);
	p->addModel(modelTriggerBurst);
	p->addModel(modelTapTempo);
	p->addModel(modelTerrainLFO);
	// mod2 batch
	p->addModel(modelVCO);
	p->addModel(modelSquareVCO);
	p->addModel(modelClap);
	p->addModel(modelHihat);
	p->addModel(modelKick);
	p->addModel(modelFMDrum);
	p->addModel(modelFlux);
	p->addModel(modelSpiral);
	p->addModel(modelAcid303);
	p->addModel(modelBreakbeats);
	p->addModel(modelSample);
	// WIP: the modules below are Claude-generated maddie synths originals (no
	// upstream Hagiwo/Rob Scape firmware) and are excluded from the build for
	// now. To re-enable one: uncomment it here and in plugin.hpp, remove its
	// .cpp from WIP_SOURCES in the Makefile, and move its entry from
	// "wipModules" back to "modules" in the root plugin.json.
	// p->addModel(modelBitcrusher);
	// p->addModel(modelDelay);
	// p->addModel(modelTapeEcho);
	// p->addModel(modelDistortion);
	// p->addModel(modelChorus);
	// p->addModel(modelResonator);
	// p->addModel(modelFlanger);
	// p->addModel(modelPhaser);
	// p->addModel(modelRingMod);
	// mod2 FX batch (WIP: all Claude-generated originals, see note above)
	// p->addModel(modelTremolo);
	// p->addModel(modelWavefolder);
	// p->addModel(modelFilter);
	// p->addModel(modelDynamics);
	// p->addModel(modelComb);
	// p->addModel(modelKarplus);
	// p->addModel(modelFreeze);
	// p->addModel(modelStutter);
	// p->addModel(modelReverseDelay);
	// p->addModel(modelGlitchDelay);
	// p->addModel(modelFreqShifter);
	// p->addModel(modelReverb);
	// p->addModel(modelGranular);
	// p->addModel(modelPitchShifter);
	// p->addModel(modelSpectralFreeze);
	// p->addModel(modelFx);
	// SCAFFOLD:addModel (new addModel calls inserted above this line)

	// Any other plugin initialization may go here.
	// As an alternative, consider lazy-loading assets and lookup tables when
	// your module is created to reduce startup time of Rack.
}
