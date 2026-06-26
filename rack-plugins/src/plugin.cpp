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
	// SCAFFOLD:addModel (new addModel calls inserted above this line)

	// Any other plugin initialization may go here.
	// As an alternative, consider lazy-loading assets and lookup tables when
	// your module is created to reduce startup time of Rack.
}
