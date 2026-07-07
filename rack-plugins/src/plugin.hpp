#pragma once
#include <rack.hpp>

using namespace rack;

// Declare the Plugin, defined in plugin.cpp
extern Plugin* pluginInstance;

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
extern Model* modelBitcrusher;
extern Model* modelDelay;
extern Model* modelTapeEcho;
extern Model* modelDistortion;
extern Model* modelChorus;
extern Model* modelResonator;
extern Model* modelFlanger;
extern Model* modelPhaser;
extern Model* modelRingMod;
// SCAFFOLD:extern (new module extern declarations inserted above this line)
