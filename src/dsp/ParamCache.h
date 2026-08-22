#pragma once
#include <JuceHeader.h>
#include <atomic>
#include "BlockParams.h"

/**
 * Audio-thread parameter pointer cache.
 *
 * `juce::AudioProcessorValueTreeState::getRawParameterValue(StringRef)` does a
 * hashmap lookup keyed by the parameter ID string. Doing 200 such lookups per
 * audio block (the BlockParams construction in `processBlock`) costs measurable
 * CPU at idle. The pointers themselves never move once APVTS is constructed —
 * each parameter has exactly one `std::atomic<float>` instance for the entire
 * plugin lifetime. So we look up every pointer once in `init()` (called from
 * the processor constructor) and the audio-thread reads become a direct
 * `cache.foo->load()` against a member pointer. Bit-identical output to the
 * pre-cache code path; the only difference is no per-block hash lookup.
 *
 * Maintenance: every APVTS parameter has its name listed in T5_PARAM_LIST
 * exactly once below. To add a new parameter, append one `X(name)` entry — the
 * X-Macro expands twice (field declaration + initialisation), so both halves
 * stay in lockstep automatically.
 */

#define T5_PARAM_LIST(X) \
    X(oscScan)     X(oscOctave)     X(engineMode)     X(voiceCount) \
    X(tuning)     X(masterVol)     X(ampAttack)     X(ampDecay) \
    X(ampSustain)     X(ampRelease)     X(ampAmount)     X(ampLoop) \
    X(ampTarget)     X(ampAttackCurve)     X(ampDecayCurve)     X(ampReleaseCurve) \
    X(ampAttackVelSens)     X(ampDecayVelSens)     X(ampReleaseVelSens) \
    X(mod1Attack)     X(mod1Decay)     X(mod1Sustain)     X(mod1Release) \
    X(mod1Amount)     X(mod1Loop)     X(mod1Target)     X(mod1AttackCurve) \
    X(mod1DecayCurve)     X(mod1ReleaseCurve)     X(mod1AttackVelSens)     X(mod1DecayVelSens) \
    X(mod1ReleaseVelSens)     X(mod2Attack)     X(mod2Decay) \
    X(mod2Sustain)     X(mod2Release)     X(mod2Amount)     X(mod2Loop) \
    X(mod2Target)     X(mod2AttackCurve)     X(mod2DecayCurve)     X(mod2ReleaseCurve) \
    X(mod2AttackVelSens)     X(mod2DecayVelSens)     X(mod2ReleaseVelSens) \
    X(mod3Attack)     X(mod3Decay)     X(mod3Sustain)     X(mod3Release) \
    X(mod3Amount)     X(mod3Loop)     X(mod3Target)     X(mod3AttackCurve) \
    X(mod3DecayCurve)     X(mod3ReleaseCurve)     X(mod3AttackVelSens) \
    X(mod3DecayVelSens)     X(mod3ReleaseVelSens) \
    X(mod4Attack)     X(mod4Decay)     X(mod4Sustain)     X(mod4Release) \
    X(mod4Amount)     X(mod4Loop)     X(mod4Target)     X(mod4AttackCurve) \
    X(mod4DecayCurve)     X(mod4ReleaseCurve)     X(mod4AttackVelSens) \
    X(mod4DecayVelSens)     X(mod4ReleaseVelSens) \
    X(lfo1Rate)     X(lfo1Depth)     X(lfo1Wave)     X(lfo1Target) \
    X(lfo1Mode)     X(lfo2Rate)     X(lfo2Depth)     X(lfo2Wave) \
    X(lfo2Target)     X(lfo2Mode)     X(lfo3Rate)     X(lfo3Depth) \
    X(lfo3Wave)     X(lfo3Target)     X(lfo3Mode) \
    X(aftertouchAmtLfo1Depth)   X(aftertouchAmtLfo2Depth)   X(aftertouchAmtLfo3Depth) \
    X(aftertouchAmtEnv1Sustain) X(aftertouchAmtEnv2Sustain) X(aftertouchAmtEnv3Sustain) \
    X(aftertouchAmtEnv4Sustain) X(aftertouchAmtEnv5Sustain) \
    X(aftertouchAmtCache)       X(aftertouchAmtSnap) \
    X(aftertouchAmtCutoff)      X(aftertouchAmtResonance)   X(aftertouchAmtScan) \
    X(aftertouchAmtDca)         X(aftertouchAmtPitch)       X(aftertouchAmtNoiseLevel) \
    X(driftEnabled)     X(driftRegen)     X(driftCrossfade) \
    X(drift1Rate)     X(drift1Depth)     X(drift1Target)     X(drift1Wave) \
    X(drift2Rate)     X(drift2Depth)     X(drift2Target)     X(drift2Wave) \
    X(drift3Rate)     X(drift3Depth)     X(drift3Target)     X(drift3Wave) \
    X(filterEnabled)     X(filterType)     X(filterSlope)     X(filterCutoff) \
    X(filterResonance)     X(filterMix)     X(filterKbdTrack)     X(filterDrive) \
    X(filterDriveOs)     X(filterAlgorithm)     X(filterWarpStyle)     X(delayType) \
    X(delayTime)     X(delayFeedback)     X(delayMix)     X(delayDamp) \
    X(fxDistOn)     X(fxChorusOn)     X(fxPhaserOn)     X(fxTremOn) \
    X(fxDistDrive)     X(fxDistMix)     X(fxTremRate)     X(fxTremDepth) \
    X(fxTremStereo)     X(fxTremWave)       X(fxChorusRate)     X(fxChorusDepth) \
    X(fxChorusMix) \
    X(fxPhaserRate)     X(fxPhaserDepth)     X(fxPhaserFeedback)     X(fxPhaserMix) \
    X(reverbType)     X(reverbMix)     X(algoRoom)     X(algoDamping) \
    X(algoWidth)     X(limiterThresh)     X(limiterRelease)     X(genAlpha) \
    X(genMagnitude)     X(genNoise)     X(genDuration)     X(genStart) \
    X(genCfg)     X(genSeed)     X(genHfBoost)     X(infSteps) \
    X(loopMode)     X(crossfadeMs)     X(normalize)     X(loopOptimize) \
    X(noiseLevel)     X(noiseType)     X(wtFrames)     X(wtSmooth) \
    X(wtAutoScan)     X(freezeTexture)     X(freezeStereo)     X(seqMode) \
    X(seqRunning)     X(seqBpm)     X(seqSteps)     X(seqDivision) \
    X(seqGlideTime)     X(seqGate)     X(seqShuffle)     X(seqOctave) \
    X(seqPreset)     X(arpMode)     X(arpRate)     X(arpOctaves) \
    X(genSeqRunning)     X(genSteps)     X(genPulses)     X(genRotation) \
    X(genMutation)     X(genRange)     X(genFixSteps)     X(genFixPulses) \
    X(genFixRotation)     X(genFixMutation)     X(scaleRoot)     X(scaleType) \
    X(genFieldMode)     X(genFieldRate)     X(genFieldCenterPc)     X(genFieldPivot) \
    X(genCoordinationMode)     X(genCoordinationCap)     X(genRole)     X(genOctave) \
    X(genDivMult)     X(genDominance)     X(gen2Enable)     X(gen2Role) \
    X(gen2Octave)     X(gen2DivMult)     X(gen2Dominance)     X(gen2Steps) \
    X(gen2Pulses)     X(gen2Rotation)     X(gen2Mutation)     X(gen2FixSteps) \
    X(gen2FixPulses)     X(gen2FixRotation)     X(gen2FixMutation)     X(gen3Enable) \
    X(gen3Role)     X(gen3Octave)     X(gen3DivMult)     X(gen3Dominance) \
    X(gen3Steps)     X(gen3Pulses)     X(gen3Rotation)     X(gen3Mutation) \
    X(gen3FixSteps)     X(gen3FixPulses)     X(gen3FixRotation)     X(gen3FixMutation) \
    X(gen4Enable)     X(gen4Role)     X(gen4Octave)     X(gen4DivMult) \
    X(gen4Dominance)     X(gen4Steps)     X(gen4Pulses)     X(gen4Rotation) \
    X(gen4Mutation)     X(gen4FixSteps)     X(gen4FixPulses)     X(gen4FixRotation) \
    X(gen4FixMutation)     X(gen5Enable)     X(gen5Role)     X(gen5Octave) \
    X(gen5DivMult)     X(gen5Dominance)     X(gen5Steps)     X(gen5Pulses) \
    X(gen5Rotation)     X(gen5Mutation)     X(gen5FixSteps)     X(gen5FixPulses) \
    X(gen5FixRotation)     X(gen5FixMutation)     X(lfo1ClockMode)     X(lfo1ClockDivision) \
    X(lfo2ClockMode)     X(lfo2ClockDivision)     X(lfo3ClockMode)     X(lfo3ClockDivision) \
    X(drift1ClockMode)     X(drift1ClockDivision)     X(drift2ClockMode)     X(drift2ClockDivision) \
    X(drift3ClockMode)     X(drift3ClockDivision)     X(delayClockMode)     X(delayClockDivision) \
    X(resynthAmount)     X(velAmt) \
    X(lroP1a)     X(lroP1b)     X(lroP1c)     X(lroP1d) \
    X(lroP2a)     X(lroP2b)     X(lroP2c)     X(lroP2d) \
    X(lroP3a)     X(lroP3b)     X(lroP3c)     X(lroP3d) \
    X(lroLvl1)     X(lroLvl2)     X(lroLvl3)

struct ParamCache
{
    #define T5_PARAM_FIELD(name) std::atomic<float>* name = nullptr;
    T5_PARAM_LIST(T5_PARAM_FIELD)
    #undef T5_PARAM_FIELD

    /** The mod envelopes once more, but INDEXED — what the block-rate read
        loops over instead of writing ENV 2..5 out four times. These are the
        SAME pointers as the named members above, not copies: APVTS owns exactly
        one atomic per parameter for the plugin's lifetime. */
    struct ModEnvPtrs {
        std::atomic<float>* attack        = nullptr;
        std::atomic<float>* decay         = nullptr;
        std::atomic<float>* sustain       = nullptr;
        std::atomic<float>* release       = nullptr;
        std::atomic<float>* amount        = nullptr;
        std::atomic<float>* loop          = nullptr;
        std::atomic<float>* target        = nullptr;
        std::atomic<float>* attackCurve   = nullptr;
        std::atomic<float>* decayCurve    = nullptr;
        std::atomic<float>* releaseCurve  = nullptr;
        std::atomic<float>* attackVelSens = nullptr;
        std::atomic<float>* decayVelSens  = nullptr;
        std::atomic<float>* releaseVelSens= nullptr;
    };
    ModEnvPtrs modEnv[kNumModEnvs];

    void init(juce::AudioProcessorValueTreeState& apvts)
    {
        #define T5_PARAM_INIT(name) name = apvts.getRawParameterValue(PID::name);
        T5_PARAM_LIST(T5_PARAM_INIT)
        #undef T5_PARAM_INIT

        for (int i = 0; i < kNumModEnvs; ++i)
        {
            const auto& id = PID::modEnv[i];
            auto& p = modEnv[i];
            p.attack         = apvts.getRawParameterValue(id.attack);
            p.decay          = apvts.getRawParameterValue(id.decay);
            p.sustain        = apvts.getRawParameterValue(id.sustain);
            p.release        = apvts.getRawParameterValue(id.release);
            p.amount         = apvts.getRawParameterValue(id.amount);
            p.loop           = apvts.getRawParameterValue(id.loop);
            p.target         = apvts.getRawParameterValue(id.target);
            p.attackCurve    = apvts.getRawParameterValue(id.attackCurve);
            p.decayCurve     = apvts.getRawParameterValue(id.decayCurve);
            p.releaseCurve   = apvts.getRawParameterValue(id.releaseCurve);
            p.attackVelSens  = apvts.getRawParameterValue(id.attackVelSens);
            p.decayVelSens   = apvts.getRawParameterValue(id.decayVelSens);
            p.releaseVelSens = apvts.getRawParameterValue(id.releaseVelSens);
        }
    }
};
