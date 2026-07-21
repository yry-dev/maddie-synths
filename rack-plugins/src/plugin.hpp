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
