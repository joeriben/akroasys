#include "SynthVoice.h"
#include <cstring>

namespace
{
// Per-target aftertouch normalization: each target carries its own bipolar
// amount (-1..+1), scaled into the target's natural units so heterogeneous
// targets feel comparable. [0..1]-range targets (Scan, Reso, Noise, Env
// Sustain, LFO Depth) add the signed drive directly. Cutoff and Pitch are NOT
// AT-specific: aftertouch feeds the shared modulation buses (ModCalib::
// kCutoffModOctaves / kPitchModSemitones, BlockParams.h) alongside
// env/LFO/Drift/timbre. DCA is the one remaining AT-only target and carries its
// own two-sided law (applyAftertouchDcaGain below): a positive amount attenuates
// the resting gain and pressure reopens it, a negative amount ducks from unity.

float applyNormalizedOffset(float baseValue, float modulationOffset)
{
    return juce::jlimit(0.0f, 1.0f, baseValue + modulationOffset);
}

// Single source of truth for "is this target driven by aftertouch?". A target is
// active when its per-target amount is non-zero; every DSP hook routes through
// here. Per-target bipolar amounts live in BlockParams::aftertouchTargetAmt.
bool aftertouchTargetActive(const BlockParams& p, int target)
{
    // Cache and Snap carry no per-voice depth: they are resolved once per block
    // in the processor and move the whole instrument. Reading one here would
    // hand a voice a modulation that does not exist.
    jassert(! AftertouchTarget::movesTheInstrument(target));
    // BOTH halves, not the amount alone. A row set to Ø says nothing drives it,
    // and a depth without an axis is not a routing -- it is a number nobody can
    // reach. Everywhere but the DCA that distinction is free, because drive 0
    // through applyNormalizedOffset is the identity; on the DCA it is not, and
    // the row would have kept its RESTING ATTENUATION while claiming to be off.
    return p.aftertouchTargetAmt[target] != 0.0f
        && p.aftertouchTargetSrc[(std::size_t) target] != ExprSource::None;
}

// The four expression axes a voice can be driven from, read once per block and
// passed down instead of the single pressure this used to take. Indexed by
// ExprSource, so a target's source is a lookup rather than a branch.
//
// Ranges are each source's own, and only X reaches below zero: a bend is
// genuinely bipolar (down is not "less up"), as is Y once its rest is the value
// the note began on; pressure and velocity have a floor at rest and no meaning
// below it.
struct ExprSources
{
    float v[ExprSource::kCount] {};   // last entry is ExprSource::None, always 0

    float operator[] (int src) const
    {
        return v[juce::jlimit(0, ExprSource::kCount - 1, src)];
    }
};

// One voice's axes, in ExprSource order. Built once per call rather than per
// target: seventeen targets read from the same handful of numbers. The last
// entry is None and stays zero; the row it belongs to is switched off one level
// up, in aftertouchTargetActive, because two targets are not pure multiplies.
ExprSources makeExprSources (float velocity, float bendNorm, float timbre, float pressure)
{
    ExprSources e;
    e.v[ExprSource::Velocity] = velocity;
    e.v[ExprSource::X]        = bendNorm;
    e.v[ExprSource::Y]        = timbre;
    e.v[ExprSource::Z]        = pressure;
    e.v[ExprSource::None]     = 0.0f;   // spelled out: an unrouted target reads 0
                                        // (and aftertouchTargetActive turns the
                                        // whole row off, which the DCA needs)
    return e;
}

// Signed expression drive for a target: the target's SOURCE value (clamped to
// [-1..+1]) times its bipolar amount, so drive ∈ [-1..+1]; 0 when off.
//
// The clamp was [0..1] while pressure was the only source, and for pressure and
// velocity the two are the same thing -- neither is ever negative. X and Y both
// need the lower half: a downward bend, or a finger sliding back past where the
// note began, has to be able to drive a target the other way, and rectifying it
// would fold it onto the upward one.
float aftertouchDrive(const BlockParams& p, int target, const ExprSources& src)
{
    return aftertouchTargetActive(p, target)
        ? juce::jlimit(-1.0f, 1.0f, src[p.aftertouchTargetSrc[(std::size_t) target]])
              * p.aftertouchTargetAmt[target]
        : 0.0f;
}

float computeEffectiveLfoDepth(const BlockParams& p, int target, float baseDepth,
                               float ampEnvVal, const float* modEnvVals)
{
    float depth = baseDepth;
    if (p.ampTarget == target)
        depth = applyNormalizedOffset(depth, ampEnvVal);
    for (int m = 0; m < kNumModEnvs; ++m)
        if (p.modEnv[m].target == target)
            depth = applyNormalizedOffset(depth, modEnvVals[m]);
    return depth;
}

// [0..1]-range additive targets (Scan, Resonance, Noise, Env Sustain, LFO Depth).
float applyAftertouchTarget(const BlockParams& p, int target, float baseValue,
                            const ExprSources& src)
{
    return applyNormalizedOffset(baseValue, aftertouchDrive(p, target, src));
}

// DCA: aftertouch spans the whole amp range instead of pushing past unity.
// A POSITIVE amount is the resting ATTENUATION the finger reopens — rest gain is
// (1 − amt), full pressure returns it to 1.0 (classic AT→VCA: at amt = 1 the note
// stays silent until pressed). A NEGATIVE amount leaves the rest at 1.0 and
// pressure ducks toward silence. The factor therefore always lands in [0..1]:
// pressure never manufactures a boost for the always-on master limiter to eat,
// and both directions get the full range rather than the +6 dB a ×(1 + drive)
// trim could reach.
//
// The clamp is what HOLDS that, and it is not belt-and-braces: the law was
// written when the only source was pressure, which rests at zero and cannot go
// below it, so drive stayed inside [0, amt] and the algebra alone kept the
// factor in range. X does not rest at zero-and-up — it leans both ways. At
// amt = +1 a full DOWN-bend gives 1 − 1 + (−1) = −1: full level, polarity
// inverted. At amt = −1 the same bend gives +2, the boost the paragraph above
// says this law exists to prevent. Y can reach the same place, for the same
// reason: its rest is where the note began, so it too runs both ways. V and Z
// are bounded to [0..1] and the clamp costs them exactly nothing.
float applyAftertouchDcaGain(const BlockParams& p, float gain, const ExprSources& src)
{
    // Through the gate, not straight out of the struct: the pedestal below is
    // built from the amount, so an unrouted row would have parked the voice at
    // (1 - amt) with no gesture able to lift it. Full depth on Ø is silence.
    const float amt = aftertouchTargetActive(p, AftertouchTarget::DCA)
                        ? p.aftertouchTargetAmt[AftertouchTarget::DCA]
                        : 0.0f;
    return gain * juce::jlimit(0.0f, 1.0f,
                               1.0f - std::max(0.0f, amt)
                                    + aftertouchDrive(p, AftertouchTarget::DCA, src));
}

// The VCA's control voltage is an EXCLUSIVE choice — spec: "jeder Zustand einer
// Stimme ist zu JEDER Zeit ein XODER: Wert (ENV→DCA) oder Wert Taste an/aus."
// Either the amp envelope is routed to the DCA and IS the level, or it is routed
// elsewhere and the KEY is the level. The old else-branch was a constant 1.0f,
// which is neither: the key never entered the level at all, so with the amp
// envelope pointed anywhere but the DCA a voice sounded at full scale for its
// whole lifetime — through the release, with no note held. `keyGate` is that
// missing arm (0/1 as the spec says: on/off, velocity belongs to the envelope),
// ramped by the caller so the gate edge is not a step.
// Mod envelopes on the DCA stay what they have always been — multiplicative
// trims on top of whichever authority holds the level, not an authority.
float computeDcaGain(const BlockParams& p, float ampEnvVal, const float* modEnvVals, float keyGate)
{
    float vca = (p.ampTarget == EnvTarget::DCA) ? ampEnvVal : keyGate;
    for (int m = 0; m < kNumModEnvs; ++m)
        if (p.modEnv[m].target == EnvTarget::DCA) vca *= (1.0f + modEnvVals[m]);
    return std::max(0.0f, vca);
}

// Signed per-stage velocity → time scale. velSense ∈ [-1..+1]:
//   +  harder hits LENGTHEN the stage (up to ×2 at full velocity),
//   -  harder hits SHORTEN it (down to ×0.5), 0 = no effect.
float computeVelocityTimeScale(float velSense, float velocity)
{
    if (velSense == 0.0f)
        return 1.0f;

    const float centeredVelocity = juce::jlimit(0.0f, 1.0f, velocity) * 2.0f - 1.0f;
    return std::pow(2.0f, centeredVelocity * velSense);
}

float computeVelocityTimedMs(float baseMs, float velSense, float velocity)
{
    return std::max(0.0f, baseMs * computeVelocityTimeScale(velSense, velocity));
}

// Global velocity → envelope note-on PEAK. velAmt ∈ [0..1]: 0 = velocity-
// independent (peak 1.0), 1 = peak tracks velocity 1:1. Linear blend — the
// synth's long-standing default (== the old per-env sustain_vel_sens at 1.0).
// Applied to ALL envelope peaks, so velocity scales each env's depth on
// whatever it targets: DCA loudness, filter cutoff, pitch, scan, noise…
float velPeakScale(float velAmt, float velocity)
{
    const float a = juce::jlimit(0.0f, 1.0f, velAmt);
    const float v = juce::jlimit(0.0f, 1.0f, velocity);
    return (1.0f - a) + a * v;
}
}

void SynthVoice::prepare(double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    maxBlockSize_ = samplesPerBlock;
    samplerBlockBuf_.resize(static_cast<size_t>(samplesPerBlock));
    osc.prepare(sampleRate, samplesPerBlock);
    sampler.prepare(sampleRate, samplesPerBlock);
    freezeEngine.prepare(sampleRate, samplesPerBlock);
    noise.prepare(sampleRate);
    ampEnv.prepare(sampleRate);
    for (auto& e : modEnvs) e.prepare(sampleRate);
    perVoiceLfo1.prepare(sampleRate);
    perVoiceLfo2.prepare(sampleRate);
    perVoiceLfo3.prepare(sampleRate);
    perVoiceLfoBuf1_.resize(static_cast<size_t>(samplesPerBlock));
    perVoiceLfoBuf2_.resize(static_cast<size_t>(samplesPerBlock));
    perVoiceLfoBuf3_.resize(static_cast<size_t>(samplesPerBlock));
    filter.prepare(sampleRate, samplesPerBlock);
    filterLadder.prepare(sampleRate, samplesPerBlock);
    filterWarp.prepare(sampleRate, samplesPerBlock);
    filterR.prepare(sampleRate, samplesPerBlock);
    filterLadderR.prepare(sampleRate, samplesPerBlock);
    filterWarpR.prepare(sampleRate, samplesPerBlock);
    // The nonlinear filters were just (re-)prepared at BASE rate. Invalidate the
    // cached OS factor so the sr×factor re-prepare re-runs on the next render.
    // Without this, a host prepareToPlay (sample-rate / buffer-size change) rewinds
    // the filters to base rate while filterPreparedOsFactor_ still claims 2/4, and
    // Phase C would oversample with base-rate coefficients (cutoff an octave off).
    filterPreparedOsFactor_ = 1;

    // Build + init the three oversamplers around the pre-filter tanh drive.
    // 2 channels (L+R) for stereo drive — same OS instance handles both with
    // channel-aligned internal state. Init size is SUB_BLOCK_SIZE because
    // renderBlock drives the OS in sub-block chunks.
    using Os = juce::dsp::Oversampling<float>;
    driveOs2x_ = std::make_unique<Os>(2, 1, Os::filterHalfBandPolyphaseIIR, true, false);
    driveOs4x_ = std::make_unique<Os>(2, 2, Os::filterHalfBandPolyphaseIIR, true, false);
    driveOs8x_ = std::make_unique<Os>(2, 3, Os::filterHalfBandPolyphaseIIR, true, false);
    driveOs2x_->initProcessing(static_cast<size_t>(SUB_BLOCK_SIZE));
    driveOs4x_->initProcessing(static_cast<size_t>(SUB_BLOCK_SIZE));
    driveOs8x_->initProcessing(static_cast<size_t>(SUB_BLOCK_SIZE));

    // Csound engine frequency smoother (Phase-1 spec D7): give it the sample
    // rate up front (mirroring every other per-voice component above) and
    // seed it at the voice's default pitch so it reads a valid value even
    // before the first noteOn/glideToNote.
    csoundFreq_.reset(sampleRate, 0.0);
    csoundFreq_.setCurrentAndTargetValue(baseFrequency);

    // Key gate: closed until a note arrives. A host prepareToPlay can land on a
    // HELD note, though — re-arm the ramp for the new rate, but do not close a
    // gate the player still has down, or the note would duck to silence and
    // ramp back in. (reset() is the path that genuinely clears it.)
    keyGate_.reset(sampleRate, KEY_GATE_MS * 0.001);
    keyGate_.setCurrentAndTargetValue(noteHeld ? 1.0f : 0.0f);
}

void SynthVoice::reset()
{
    osc.reset();
    sampler.reset();
    freezeEngine.reset();
    ampEnv.reset();
    for (auto& e : modEnvs) e.reset();
    filter.reset();
    filterLadder.reset();
    filterWarp.reset();
    filterR.reset();
    filterLadderR.reset();
    filterWarpR.reset();
    noise.reset();
    if (driveOs2x_) driveOs2x_->reset();
    if (driveOs4x_) driveOs4x_->reset();
    if (driveOs8x_) driveOs8x_->reset();
    active = false;
    noteHeld = false;
    keyGate_.setCurrentAndTargetValue(0.0f);
    currentNote = -1;
    aftertouch_ = 0.0f;
    lastAmpEnvLevel = 0.0f;
    for (auto& v : lastModVal_) v = 0.0f;
    lastModulatedCutoff_ = 20000.0f;
    lastModulatedResonance_ = 0.0f;
    lastModulatedScan_ = 0.0f;
    lastModulatedNoiseLevel_ = 0.0f;
    lastOutputSample_ = 0.0f;
    lastOutputSampleR_ = 0.0f;
    restartFadeTailSample_ = 0.0f;
    restartFadeTailSampleR_ = 0.0f;
    restartFadeSamplesLeft_ = 0;
    restartFadeTotalSamples_ = 1;
    samplerPreStretchNormGain_ = 1.0f;
    samplerPreStretchNormDirty_ = true;
    preStretchNormState_ = {};
}

void SynthVoice::beginRestartFade()
{
    restartFadeTailSample_  = lastOutputSample_;
    restartFadeTailSampleR_ = lastOutputSampleR_;
    restartFadeTotalSamples_ = std::max(1,
        static_cast<int>(RESTART_FADE_MS * 0.001f * static_cast<float>(sr)));
    restartFadeSamplesLeft_ = restartFadeTotalSamples_;
}

void SynthVoice::applyRestartFadeStereo(float& L, float& R)
{
    if (restartFadeSamplesLeft_ <= 0)
        return;

    const int samplesDone = restartFadeTotalSamples_ - restartFadeSamplesLeft_ + 1;
    const float t = juce::jlimit(0.0f, 1.0f,
        static_cast<float>(samplesDone) / static_cast<float>(restartFadeTotalSamples_));
    --restartFadeSamplesLeft_;

    L = restartFadeTailSample_  + (L - restartFadeTailSample_)  * t;
    R = restartFadeTailSampleR_ + (R - restartFadeTailSampleR_) * t;
}

void SynthVoice::noteOn(int note, float velocity, bool legato)
{
    currentNote = note;
    currentVelocity = velocity;
    noteHeld = true;
    active = true;
    applyVelocityTimedEnvelopeTimes();

    if (!legato)
    {
        // Velocity scales every envelope's note-on peak via the global Velocity
        // Amount (velAmt_). This drives the env's depth on whatever it targets —
        // DCA loudness, filter cutoff, pitch, scan… — so velocity is alive on all
        // targets, not just the DCA. Per-env Amt (static depth) stays orthogonal;
        // velSens shapes only the A/D/R times.
        const float peak = velPeakScale(velAmt_, velocity);
        ampEnv.noteOn(peak);
        for (auto& e : modEnvs) e.noteOn(peak);
        // Fresh note starts at the MPE timbre REST until its first CC74 arrives;
        // legato (held finger sliding to a new note) keeps the current timbre.
        timbre_     = 0.0f;
        timbreRest_ = kTimbreRest;   // VoiceManager overwrites it with the
                                     // channel's CC 74 right after, where there
                                     // is one; a note with no channel starts at
                                     // the bottom of the travel, as before.
    }
    samplerPreStretchNormDirty_ = true;

    // Set pitch (cache base for modulation reference)
    int shiftedNote = note + octaveShift_ * 12;
    baseFrequency = tunedHz(shiftedNote);
    osc.setFrequency(baseFrequency);

    if (engineMode == EngineMode::Sampler)
    {
        double ratio = static_cast<double>(tunedHz(shiftedNote))
                     / static_cast<double>(tunedHz(60));
        sampler.setTransposeRatio(ratio);
    }
    else if (engineMode == EngineMode::Freeze)
    {
        double ratio = static_cast<double>(tunedHz(shiftedNote))
                     / static_cast<double>(tunedHz(60));
        freezeEngine.setTransposeRatio(ratio);
        freezeEngine.retrigger();
    }
    else if (engineMode == EngineMode::Csound)
    {
        // Fresh note: snap the smoother straight to the new pitch (no glide) —
        // same semantics as osc.setFrequency()/sampler.setTransposeRatio()
        // above, which also snap rather than ramp on noteOn.
        csoundFreq_.setCurrentAndTargetValue(baseFrequency);
    }
}

void SynthVoice::noteOff()
{
    noteHeld = false;
    applyVelocityTimedEnvelopeTimes();
    ampEnv.noteOff();
    for (auto& e : modEnvs) e.noteOff();
}

void SynthVoice::glideToNote(int note, float glideMs)
{
    currentNote = note;
    int shiftedNote = note + octaveShift_ * 12;
    // Explicit per-engine switch (no silent fallthrough) — each engine mode
    // glides its own pitch state; Wavetable was formerly the catch-all
    // `else` here, which would have silently swallowed a future 5th engine.
    switch (engineMode)
    {
        case EngineMode::Sampler:
        {
            double ratio = static_cast<double>(tunedHz(shiftedNote))
                         / static_cast<double>(tunedHz(60));
            sampler.glideToRatio(ratio, glideMs);
            break;
        }
        case EngineMode::Freeze:
        {
            double ratio = static_cast<double>(tunedHz(shiftedNote))
                         / static_cast<double>(tunedHz(60));
            freezeEngine.glideToRatio(ratio, glideMs);
            break;
        }
        case EngineMode::Wavetable:
        {
            float targetFreq = tunedHz(shiftedNote);
            osc.glideToFrequency(targetFreq, glideMs);
            break;
        }
        case EngineMode::Csound:
        {
            // Phase-1 spec D7: sample-accurate glide isn't achievable through a
            // k-rate control channel; this block-rate SmoothedValue on the voice
            // (advanced at channel-write time in a later commit) plus the
            // orchestra's own portk is the accepted Phase-1 approximation.
            // juce::SmoothedValue::reset() SNAPS current to target, so the
            // current value must be captured BEFORE reset and restored after —
            // the exact same capture/restore dance as the DCO A/B presence
            // smoothers' `rearm` lambda above (dcoRecipeChanged branch).
            const float targetHz = tunedHz(shiftedNote);
            const float capturedCurrent = csoundFreq_.getCurrentValue();
            csoundFreq_.reset(sr, glideMs * 0.001);
            csoundFreq_.setCurrentAndTargetValue(capturedCurrent);
            csoundFreq_.setTargetValue(targetHz);
            break;
        }
    }
}

float SynthVoice::pitchBusSemitones(const BlockParams& p,
                                    float ampEnvVal, const float* modEnvVals,
                                    float lfo1Val, float lfo2Val, float lfo3Val) const
{
    const ExprSources expr = makeExprSources (currentVelocity,
                                             perVoicePitchBendNorm_,
                                             timbre_, aftertouch_);
    // Every source contributes a NORMALIZED semitone-fraction; the caller
    // applies ModCalib::kPitchModSemitones once, as an equal-tempered ratio.
    // The LFO terms arrive ALREADY depth-scaled, because the render loop has
    // its depths in hand and should not pay to resolve them twice.
    float semis = p.driftPitchOffset;
    if (p.ampTarget  == EnvTarget::Pitch) semis += ampEnvVal;
    for (int m = 0; m < kNumModEnvs; ++m)
        if (p.modEnv[m].target == EnvTarget::Pitch) semis += modEnvVals[m];
    if (p.lfo1Target == LfoTarget::Pitch) semis += lfo1Val;
    if (p.lfo2Target == LfoTarget::Pitch) semis += lfo2Val;
    if (p.lfo3Target == LfoTarget::Pitch) semis += lfo3Val;
    return semis + aftertouchDrive(p, AftertouchTarget::Pitch, expr);
}

float SynthVoice::pitchBusReachSemitones(const BlockParams& p) const
{
    // Mirrors pitchBusSemitones term for term, with each routed source at its
    // extreme instead of its current value. Envelopes run 0..1, so a pitch-
    // targeted envelope reaches 1; an LFO reaches its depth.
    //
    // The line drawn here: what the PATCH routes to pitch is counted at full
    // scale, live performance gestures are not. Pitch bend and aftertouch are
    // armed in every preset, so counting their range would put every note on the
    // stretcher and the exact read would become unreachable; a note struck at the
    // sample's own pitch and then bent is heard tape-style, which is the trade
    // that was chosen (2026-07-26).
    //
    // What that leaves under-counted: aftertouch→Pitch, and the envelope scaling
    // computeEffectiveLfoDepth can put on an LFO's depth. Both can push the real
    // bus past this number. That is the safe direction — such a note keeps the
    // direct read and follows its modulation by reading faster or slower, which is
    // a real bend, not a lost one (SamplePlayer::processSample folds pitchModFactor
    // into the read speed). Over-estimating instead would put quiet notes on the
    // stretcher for a movement that never arrives.
    //
    // Drift is counted by its REACH, not its current output: the offset crosses
    // zero constantly, so reading the instantaneous value made the path depend on
    // the drift waveform's phase at note-on — neighbouring notes in one chord
    // landing on different paths for no reason a player could hear or predict.
    float bus = std::abs(p.driftPitchReach);
    if (p.ampTarget  == EnvTarget::Pitch) bus += 1.0f;
    for (int m = 0; m < kNumModEnvs; ++m)
        if (p.modEnv[m].target == EnvTarget::Pitch) bus += 1.0f;
    if (p.lfo1Target == LfoTarget::Pitch) bus += std::abs(p.lfo1Depth);
    if (p.lfo2Target == LfoTarget::Pitch) bus += std::abs(p.lfo2Depth);
    if (p.lfo3Target == LfoTarget::Pitch) bus += std::abs(p.lfo3Depth);

    // The terms above are all normalized bus units, not semitones — the bus is a
    // normalized sum and kPitchModSemitones turns it into semitones exactly once
    // (BlockParams.h: ±1 summed → ±12 semitones). Converting here rather than at
    // the call site keeps the function's name true; a caller that had to apply
    // the factor itself would be one omission away from a 12x error.
    return bus * ModCalib::kPitchModSemitones;
}

float SynthVoice::pitchBusRatioFromRawLfo(const BlockParams& p,
                                          float lfo1Raw, float lfo2Raw, float lfo3Raw) const
{
    const ExprSources expr = makeExprSources (currentVelocity,
                                             perVoicePitchBendNorm_,
                                             timbre_, aftertouch_);
    // The env levels are this voice's LAST rendered values, one segment behind:
    // the bridge runs before the orchestra renders, so nothing newer exists yet.
    // At the control rate this is written (once per MIDI sub-segment, so once
    // per host block when no events fall inside it) that lag is well under a
    // millisecond of envelope travel and inaudible -- whereas the LFO term it
    // carries is the whole point.
    const float d1 = applyAftertouchTarget(p, AftertouchTarget::LFO1Depth,
        computeEffectiveLfoDepth(p, EnvTarget::LFO1Depth, p.lfo1Depth,
                                 lastAmpEnvLevel, lastModVal_), expr);
    const float d2 = applyAftertouchTarget(p, AftertouchTarget::LFO2Depth,
        computeEffectiveLfoDepth(p, EnvTarget::LFO2Depth, p.lfo2Depth,
                                 lastAmpEnvLevel, lastModVal_), expr);
    const float d3 = applyAftertouchTarget(p, AftertouchTarget::LFO3Depth,
        computeEffectiveLfoDepth(p, EnvTarget::LFO3Depth, p.lfo3Depth,
                                 lastAmpEnvLevel, lastModVal_), expr);
    const float semis = pitchBusSemitones(p, lastAmpEnvLevel, lastModVal_,
                                          lfo1Raw * d1, lfo2Raw * d2, lfo3Raw * d3);
    // Clamped like the freeze path's own pitch ratio: a full-scale bus is +-1
    // octave, but several sources summing can exceed that, and the orchestra's
    // `limit kfreq, 20, 12000` would then pin the note to a rail rather than
    // bend it.
    return juce::jlimit(0.0625f, 16.0f,
                        std::pow(2.0f, semis * ModCalib::kPitchModSemitones / 12.0f));
}

float SynthVoice::readCsoundFreq(int samplesToAdvance)
{
    // skip() advances the smoother exactly like calling getNextValue()
    // samplesToAdvance times (clamping to target if it overshoots the
    // remaining ramp) — juce::SmoothedValue::skip is documented to accept
    // this directly; a negative/zero request (shouldn't happen per the
    // processor's carry accounting, but a defensive floor costs nothing) is a
    // no-op read of the current value.
    if (samplesToAdvance > 0)
        csoundFreq_.skip(samplesToAdvance);
    return csoundFreq_.getCurrentValue();
}

void SynthVoice::configureForBlock(const BlockParams& p)
{
    const ExprSources expr = makeExprSources (currentVelocity,
                                             perVoicePitchBendNorm_,
                                             timbre_, aftertouch_);
    octaveShift_ = p.octaveShift;
    velAmt_ = p.velAmt;

    // Loop mode repurposes the Sustain control: the LEVEL is fixed and the slider
    // value becomes the per-cycle Hold duration (A→D→Hold→R→repeat). Depth comes
    // from the envelope Amount; A/D/R keep their normal meaning.
    constexpr float kLoopHoldLevel = 0.5f;
    auto loopHoldMs = [](float sus) { return sus * sus * 4000.0f; }; // 0..1 → 0..4s, fine at the short end

    ampAttackBaseMs_ = p.ampAttack;
    ampDecayBaseMs_ = p.ampDecay;
    ampReleaseBaseMs_ = p.ampRelease;
    ampAttackVelSens_ = p.ampAttackVelSens;
    ampDecayVelSens_ = p.ampDecayVelSens;
    ampReleaseVelSens_ = p.ampReleaseVelSens;
    if (p.ampLoop)
    {
        ampEnv.setSustain(kLoopHoldLevel);
        ampEnv.setHoldMs(loopHoldMs(p.ampSustain));
    }
    else
    {
        ampEnv.setSustain(applyAftertouchTarget(p, AftertouchTarget::Env1Sustain,
                                                p.ampSustain, expr));
        ampEnv.setHoldMs(0.0f);
    }
    ampEnv.setLooping(p.ampLoop);
    ampEnv.setAttackBend(p.ampAttackBend);
    ampEnv.setDecayBend(p.ampDecayBend);
    ampEnv.setReleaseBend(p.ampReleaseBend);

    for (int m = 0; m < kNumModEnvs; ++m)
    {
        const auto& mp = p.modEnv[m];
        auto& env = modEnvs[m];
        modAttackBaseMs_[m]     = mp.attack;
        modDecayBaseMs_[m]      = mp.decay;
        modReleaseBaseMs_[m]    = mp.release;
        modAttackVelSens_[m]    = mp.attackVelSens;
        modDecayVelSens_[m]     = mp.decayVelSens;
        modReleaseVelSens_[m]   = mp.releaseVelSens;
        if (mp.loop)
        {
            env.setSustain(kLoopHoldLevel);
            env.setHoldMs(loopHoldMs(mp.sustain));
        }
        else
        {
            env.setSustain(applyAftertouchTarget(p, AftertouchTarget::modEnvSustain(m),
                                                 mp.sustain, expr));
            env.setHoldMs(0.0f);
        }
        env.setLooping(mp.loop);
        env.setAttackBend(mp.attackBend);
        env.setDecayBend(mp.decayBend);
        env.setReleaseBend(mp.releaseBend);
    }

    applyVelocityTimedEnvelopeTimes();

    if (!active) return;

    updateSamplerPreStretchNorm(p);
}

void SynthVoice::applyVelocityTimedEnvelopeTimes()
{
    ampEnv.setAttack(computeVelocityTimedMs(ampAttackBaseMs_, ampAttackVelSens_, currentVelocity));
    ampEnv.setDecay(computeVelocityTimedMs(ampDecayBaseMs_, ampDecayVelSens_, currentVelocity));
    ampEnv.setRelease(computeVelocityTimedMs(ampReleaseBaseMs_, ampReleaseVelSens_, currentVelocity));

    for (int m = 0; m < kNumModEnvs; ++m)
    {
        modEnvs[m].setAttack(computeVelocityTimedMs(modAttackBaseMs_[m], modAttackVelSens_[m], currentVelocity));
        modEnvs[m].setDecay(computeVelocityTimedMs(modDecayBaseMs_[m], modDecayVelSens_[m], currentVelocity));
        modEnvs[m].setRelease(computeVelocityTimedMs(modReleaseBaseMs_[m], modReleaseVelSens_[m], currentVelocity));
    }
}

bool SynthVoice::modEnvStateMatches(const PreStretchNormState& st, const BlockParams& p)
{
    auto nearlyEqual = [] (float a, float b) { return std::abs(a - b) < 1.0e-5f; };

    for (int m = 0; m < kNumModEnvs; ++m)
    {
        const auto& a = st.modEnv[m];
        const auto& b = p.modEnv[m];
        if (a.target != b.target
            || a.loop != b.loop
            || !nearlyEqual(a.attackBend, b.attackBend)
            || !nearlyEqual(a.decayBend, b.decayBend)
            || !nearlyEqual(a.releaseBend, b.releaseBend)
            || !nearlyEqual(a.attack, b.attack)
            || !nearlyEqual(a.decay, b.decay)
            || !nearlyEqual(a.sustain, b.sustain)
            || !nearlyEqual(a.release, b.release)
            || !nearlyEqual(a.amount, b.amount)
            || !nearlyEqual(a.attackVelSens, b.attackVelSens)
            || !nearlyEqual(a.decayVelSens, b.decayVelSens)
            || !nearlyEqual(a.releaseVelSens, b.releaseVelSens))
            return false;
    }
    return true;
}

bool SynthVoice::preStretchNormStateMatches(const BlockParams& p) const
{
    auto nearlyEqual = [] (float a, float b)
    {
        return std::abs(a - b) < 1.0e-5f;
    };

    return nearlyEqual(preStretchNormState_.ampAttack, p.ampAttack)
        && nearlyEqual(preStretchNormState_.ampDecay, p.ampDecay)
        && nearlyEqual(preStretchNormState_.ampSustain, p.ampSustain)
        && nearlyEqual(preStretchNormState_.ampRelease, p.ampRelease)
        && nearlyEqual(preStretchNormState_.ampAmount, p.ampAmount)
        && preStretchNormState_.ampTarget == p.ampTarget
        && preStretchNormState_.ampLoop == p.ampLoop
        && nearlyEqual(preStretchNormState_.ampAttackBend, p.ampAttackBend)
        && nearlyEqual(preStretchNormState_.ampDecayBend, p.ampDecayBend)
        && nearlyEqual(preStretchNormState_.ampReleaseBend, p.ampReleaseBend)
        && nearlyEqual(preStretchNormState_.ampAttackVelSens, p.ampAttackVelSens)
        && nearlyEqual(preStretchNormState_.ampDecayVelSens, p.ampDecayVelSens)
        && nearlyEqual(preStretchNormState_.ampReleaseVelSens, p.ampReleaseVelSens)
        && modEnvStateMatches(preStretchNormState_, p)
        && nearlyEqual(preStretchNormState_.velocity, currentVelocity)
        && nearlyEqual(preStretchNormState_.velAmt, velAmt_)
        && nearlyEqual(preStretchNormState_.startPos, sampler.getStartPos())
        && nearlyEqual(preStretchNormState_.loopStart, sampler.getLoopStart())
        && nearlyEqual(preStretchNormState_.loopEnd, sampler.getLoopEnd())
        && nearlyEqual(preStretchNormState_.startPosOffset, sampler.getStartPosOffset())
        && nearlyEqual(preStretchNormState_.crossfadeMs, sampler.getCrossfadeMs())
        && preStretchNormState_.loopMode == static_cast<int>(sampler.getLoopMode())
        && preStretchNormState_.normalizeOn == sampler.getNormalize();
}

void SynthVoice::updateSamplerPreStretchNorm(const BlockParams& p)
{
    if (engineMode != EngineMode::Sampler || !sampler.hasAudio() || !sampler.getNormalize())
    {
        samplerPreStretchNormGain_ = 1.0f;
        samplerPreStretchNormDirty_ = false;
        sampler.setSourceGain(1.0f);
        preStretchNormState_.normalizeOn = sampler.getNormalize();
        return;
    }

    if (!samplerPreStretchNormDirty_ && preStretchNormStateMatches(p))
    {
        sampler.setSourceGain(samplerPreStretchNormGain_);
        return;
    }

    const int referencePathSamples = sampler.estimateReferenceLengthSamples();
    auto envWindowMs = [] (float attackMs, float decayMs, float releaseMs, bool looping)
    {
        constexpr float kHoldMs = 120.0f;
        float base = std::max(attackMs, 0.0f) + std::max(decayMs, 0.0f)
                   + (looping ? 0.0f : kHoldMs) + std::max(releaseMs, 0.0f) * 0.1f;
        return base;
    };

    const float ampAttackMs = computeVelocityTimedMs(p.ampAttack, p.ampAttackVelSens, currentVelocity);
    const float ampDecayMs = computeVelocityTimedMs(p.ampDecay, p.ampDecayVelSens, currentVelocity);
    const float ampReleaseMs = computeVelocityTimedMs(p.ampRelease, p.ampReleaseVelSens, currentVelocity);
    float modAttackMs[kNumModEnvs], modDecayMs[kNumModEnvs], modReleaseMs[kNumModEnvs];
    for (int m = 0; m < kNumModEnvs; ++m)
    {
        const auto& mp = p.modEnv[m];
        modAttackMs[m]  = computeVelocityTimedMs(mp.attack,  mp.attackVelSens,  currentVelocity);
        modDecayMs[m]   = computeVelocityTimedMs(mp.decay,   mp.decayVelSens,   currentVelocity);
        modReleaseMs[m] = computeVelocityTimedMs(mp.release, mp.releaseVelSens, currentVelocity);
    }

    float analysisMs = (p.ampTarget == EnvTarget::DCA)
        ? envWindowMs(ampAttackMs, ampDecayMs, ampReleaseMs, p.ampLoop)
        : 0.0f;
    for (int m = 0; m < kNumModEnvs; ++m)
        if (p.modEnv[m].target == EnvTarget::DCA)
            analysisMs = std::max(analysisMs, envWindowMs(modAttackMs[m], modDecayMs[m],
                                                          modReleaseMs[m], p.modEnv[m].loop));

    // This whole function runs on the AUDIO THREAD — configureForBlock calls it
    // every block, and noteOn marks it dirty, so it re-runs on every note in
    // sampler mode with Normalize on. The curve was a local std::vector sized
    // per call, i.e. up to sr×3 floats allocated per note (576 KB at 48 kHz).
    // It is now the pool's one shared buffer, lent by VoiceManager::prepare, and
    // its length IS the clamp: nothing below can ask for more than was lent.
    if (dcaScratch_ == nullptr || dcaScratchLen_ < 64)
    {
        // No buffer lent (a voice used before VoiceManager::prepare). Assert a
        // known gain rather than leave the sampler on whatever it last carried,
        // and stay dirty so the real analysis still runs once prepare has been.
        samplerPreStretchNormGain_ = 1.0f;
        sampler.setSourceGain(1.0f);
        return;
    }

    int analysisSamples = std::max(referencePathSamples,
        static_cast<int>(std::ceil(sr * analysisMs * 0.001)));
    analysisSamples = juce::jlimit(64, dcaScratchLen_, analysisSamples);

    float* dcaCurve = dcaScratch_;

    ADSREnvelope ampRef;
    ADSREnvelope modRef[kNumModEnvs];
    ampRef.prepare(sr);
    for (auto& e : modRef) e.prepare(sr);

    ampRef.setAttack(ampAttackMs);
    ampRef.setDecay(ampDecayMs);
    ampRef.setSustain(p.ampSustain);
    ampRef.setRelease(ampReleaseMs);
    ampRef.setLooping(p.ampLoop);
    ampRef.setAttackBend(p.ampAttackBend);
    ampRef.setDecayBend(p.ampDecayBend);
    ampRef.setReleaseBend(p.ampReleaseBend);

    for (int m = 0; m < kNumModEnvs; ++m)
    {
        const auto& mp = p.modEnv[m];
        modRef[m].setAttack(modAttackMs[m]);
        modRef[m].setDecay(modDecayMs[m]);
        modRef[m].setSustain(mp.sustain);
        modRef[m].setRelease(modReleaseMs[m]);
        modRef[m].setLooping(mp.loop);
        modRef[m].setAttackBend(mp.attackBend);
        modRef[m].setDecayBend(mp.decayBend);
        modRef[m].setReleaseBend(mp.releaseBend);
    }

    // Mirror the live envelopes (peak == velPeakScale) so the sampler pre-stretch
    // normalization analyses the same DCA curve playback will produce; otherwise
    // soft notes would be double-attenuated. Cache keys on currentVelocity + velAmt_.
    const float refPeak = velPeakScale(velAmt_, currentVelocity);
    ampRef.noteOn(refPeak);
    for (auto& e : modRef) e.noteOn(refPeak);

    for (int i = 0; i < analysisSamples; ++i)
    {
        const float ampEnvVal = ampRef.processSample() * p.ampAmount;
        float modEnvVals[kNumModEnvs];
        for (int m = 0; m < kNumModEnvs; ++m)
            modEnvVals[m] = modRef[m].processSample() * p.modEnv[m].amount;

        // The reference envelopes above are never released, so this analyses a
        // HELD note — the key gate is open for its whole length.
        dcaCurve[static_cast<size_t>(i)] = computeDcaGain(p, ampEnvVal, modEnvVals, 1.0f);
    }

    float analysisPeak = 0.0f;
    sampler.estimatePlaybackRms(dcaCurve, analysisSamples, &analysisPeak);

    // The VCA carries the engine trim (EngineCalib), and this analysis predicts
    // the POST-VCA peak, so the trim belongs in the prediction. Without it the
    // guard would enforce 0.95 * kSampler = 0.738 instead of 0.95 and attenuate
    // material that was never over -- and it does so PRE-STRETCH, upstream of
    // the drive and the filter, which is exactly the source-side timbre shift
    // the trim was sited at the VCA to avoid.
    analysisPeak *= EngineCalib::kSampler;

    static constexpr float kCeiling = 0.95f;
    samplerPreStretchNormGain_ = 1.0f;

    if (analysisPeak > kCeiling + 1.0e-6f)
    {
        // Keep Normalize from quietly turning into a loudness target.
        // The prepared buffer has already been peak-normalized, so the
        // remaining job here is just to catch rare post-DCA overs, not to
        // push sustained material down toward a fixed RMS.
        samplerPreStretchNormGain_ = kCeiling / analysisPeak;
    }

    sampler.setSourceGain(samplerPreStretchNormGain_);

    preStretchNormState_.ampAttack = p.ampAttack;
    preStretchNormState_.ampDecay = p.ampDecay;
    preStretchNormState_.ampSustain = p.ampSustain;
    preStretchNormState_.ampRelease = p.ampRelease;
    preStretchNormState_.ampAmount = p.ampAmount;
    preStretchNormState_.ampTarget = p.ampTarget;
    preStretchNormState_.ampLoop = p.ampLoop;
    preStretchNormState_.ampAttackBend = p.ampAttackBend;
    preStretchNormState_.ampDecayBend = p.ampDecayBend;
    preStretchNormState_.ampReleaseBend = p.ampReleaseBend;
    preStretchNormState_.ampAttackVelSens = p.ampAttackVelSens;
    preStretchNormState_.ampDecayVelSens = p.ampDecayVelSens;
    preStretchNormState_.ampReleaseVelSens = p.ampReleaseVelSens;
    for (int m = 0; m < kNumModEnvs; ++m)
    {
        const auto& mp = p.modEnv[m];
        auto& st = preStretchNormState_.modEnv[m];
        st.target         = mp.target;
        st.attack         = mp.attack;
        st.decay          = mp.decay;
        st.sustain        = mp.sustain;
        st.release        = mp.release;
        st.amount         = mp.amount;
        st.loop           = mp.loop;
        st.attackBend     = mp.attackBend;
        st.decayBend      = mp.decayBend;
        st.releaseBend    = mp.releaseBend;
        st.attackVelSens  = mp.attackVelSens;
        st.decayVelSens   = mp.decayVelSens;
        st.releaseVelSens = mp.releaseVelSens;
    }
    preStretchNormState_.velocity = currentVelocity;
    preStretchNormState_.velAmt = velAmt_;
    preStretchNormState_.startPos = sampler.getStartPos();
    preStretchNormState_.loopStart = sampler.getLoopStart();
    preStretchNormState_.loopEnd = sampler.getLoopEnd();
    preStretchNormState_.startPosOffset = sampler.getStartPosOffset();
    preStretchNormState_.crossfadeMs = sampler.getCrossfadeMs();
    preStretchNormState_.loopMode = static_cast<int>(sampler.getLoopMode());
    preStretchNormState_.normalizeOn = sampler.getNormalize();
    samplerPreStretchNormDirty_ = false;
}

void SynthVoice::renderBlock(float* output, float* outputRight, const BlockParams& p,
                              const float* lfo1Buf, const float* lfo2Buf, const float* lfo3Buf, int numSamples,
                              const float* csoundBuf)
{
    // Captured for the block (Phase-1 spec §3) — read by the Csound render
    // branch below, mirroring how lfo1Buf/lfo2Buf/lfo3Buf are already threaded
    // straight through as parameters. A null buffer (non-Csound mode, or a
    // not-yet-ready/stub engine) means that branch stays inert and this voice
    // renders silence in Csound mode — the documented safety net, not a crash.
    csoundBuf_ = csoundBuf;

    // This voice's four expression axes, held for the block. Everything below
    // that a target can be driven from reads out of here.
    const ExprSources expr = makeExprSources (currentVelocity,
                                              perVoicePitchBendNorm_,
                                              timbre_, aftertouch_);

    if (!active)
    {
        std::memset(output, 0, sizeof(float) * static_cast<size_t>(numSamples));
        if (outputRight != nullptr)
            std::memset(outputRight, 0, sizeof(float) * static_cast<size_t>(numSamples));
        return;
    }

    // Which of this voice's envelopes drive something OUTSIDE it — the delay,
    // the reverb, an LFO rate or depth? The processor reads ALL FIVE off the
    // newest active voice (PluginProcessor.cpp, `envSrc`: the amp envelope and
    // ENV 2..5 land in one array and feed the same targets), so any of them
    // still moving keeps the slot held. Constant for the block, so it is asked
    // once here rather than per sample in the voice-free test below.
    const bool ampOutsideVoice = EnvTarget::isOutsideTheVoice(p.ampTarget);
    bool modOutsideVoice[kNumModEnvs];
    for (int m = 0; m < kNumModEnvs; ++m)
        modOutsideVoice[m] = EnvTarget::isOutsideTheVoice(p.modEnv[m].target);

    // Per-voice Trig-mode LFOs — sync rate/waveform from global, fill the
    // voice's own buffer (reset at note-on by VoiceManager), then steer the
    // function-local pointers so all downstream readers see the per-voice
    // signal. Free mode leaves the parameters pointing at the shared global
    // buffer.
    auto fillPerVoice = [&](LFO& l, std::vector<float>& buf, float rate, int wave) {
        l.setRate(rate);
        l.setWaveform(wave);
        l.setDepth(1.0f);
        for (int i = 0; i < numSamples; ++i)
            buf[static_cast<size_t>(i)] = l.processSample();
    };
    if (p.lfo1TrigMode) { fillPerVoice(perVoiceLfo1, perVoiceLfoBuf1_, p.lfo1Rate, p.lfo1Wave); lfo1Buf = perVoiceLfoBuf1_.data(); }
    if (p.lfo2TrigMode) { fillPerVoice(perVoiceLfo2, perVoiceLfoBuf2_, p.lfo2Rate, p.lfo2Wave); lfo2Buf = perVoiceLfoBuf2_.data(); }
    if (p.lfo3TrigMode) { fillPerVoice(perVoiceLfo3, perVoiceLfoBuf3_, p.lfo3Rate, p.lfo3Wave); lfo3Buf = perVoiceLfoBuf3_.data(); }

    // Combine global pitch-bend ratio (all voices) with per-voice MPE pitch bend.
    // For standard MIDI perVoicePitchBendSemitones_ stays 0, so this is a no-op.
    const float effectivePitchRatio = p.performancePitchRatio
        * std::pow(2.0f, perVoicePitchBendSemitones_ / 12.0f);

    bool samplerMode = (engineMode == EngineMode::Sampler) && sampler.hasAudio();
    bool freezeMode = (engineMode == EngineMode::Freeze) && freezeEngine.hasAudio();
    bool oscReady = (engineMode == EngineMode::Wavetable) && osc.hasFrames();

    // Block-constant, and keyed on the engine MODE rather than on the three
    // readiness flags above. The flags flip the moment an engine receives its
    // audio -- a generation finishing while a note is held -- and a trim that
    // followed them would step the voice's level by up to 10.5 dB at that block
    // boundary, unramped. The mode only changes when the player changes it.
    const float engineTrim = engineMode == EngineMode::Sampler   ? EngineCalib::kSampler
                           : engineMode == EngineMode::Freeze    ? EngineCalib::kFreeze
                           : engineMode == EngineMode::Wavetable ? EngineCalib::kWavetable
                                                                 : EngineCalib::kCsound;

    // Single wavetable oscillator (the dual A+B DCO split is dead — BJ
    // 2026-07-17). Hoist: setInterpolation is a pure setter; tunedHz is
    // block-constant, so the base note is resolved once here, not per sample.
    float blockBaseFreqWavetable = 0.0f;
    if (oscReady)
    {
        blockBaseFreqWavetable = tunedHz(currentNote + octaveShift_ * 12);
        osc.setInterpolation(p.wtSmooth);
    }

    if (freezeMode)
    {
        freezeEngine.setTextureMode(p.freezeTexture);
        freezeEngine.setStereoWidth(p.freezeStereo);
    }

    // ── Sampler mode: pre-render pitch-shifted block via Signalsmith Stretch ──
    if (samplerMode)
    {
        // Block-rate pitch modulation (computed at block midpoint)
        int mid = numSamples / 2;
        const float lfo1Depth = applyAftertouchTarget(p, AftertouchTarget::LFO1Depth,
            computeEffectiveLfoDepth(p, EnvTarget::LFO1Depth, p.lfo1Depth,
                                     lastAmpEnvLevel, lastModVal_), expr);
        const float lfo2Depth = applyAftertouchTarget(p, AftertouchTarget::LFO2Depth,
            computeEffectiveLfoDepth(p, EnvTarget::LFO2Depth, p.lfo2Depth,
                                     lastAmpEnvLevel, lastModVal_), expr);
        const float lfo3Depth = applyAftertouchTarget(p, AftertouchTarget::LFO3Depth,
            computeEffectiveLfoDepth(p, EnvTarget::LFO3Depth, p.lfo3Depth,
                                     lastAmpEnvLevel, lastModVal_), expr);
        // ── Pitch modulation bus ──────────────────────────────────────
        // One full-scale (ModCalib::kPitchModSemitones) applied once as an
        // equal-tempered ratio. See SynthVoice::pitchBusSemitones for why the
        // sum itself lives in one place.
        const float pitchSemis = pitchBusSemitones(
            p, lastAmpEnvLevel, lastModVal_,
            lfo1Buf[mid] * lfo1Depth, lfo2Buf[mid] * lfo2Depth, lfo3Buf[mid] * lfo3Depth);
        sampler.setPitchModulation(effectivePitchRatio
            * std::pow(2.0f, pitchSemis * ModCalib::kPitchModSemitones / 12.0f));

        // How far this note's pitch can travel from the sample's own pitch. The
        // sampler reads it on the note's FIRST block to choose its render path
        // once, so it never switches engines under a sounding note. Pushed every
        // block because the first block is not knowable from here; only that
        // first read decides anything.
        sampler.setPitchModulationReach(
            std::abs(12.0f * std::log2(std::max(effectivePitchRatio, 1e-6f)))
            + pitchBusReachSemitones(p));

        sampler.renderPitchedBlock(samplerBlockBuf_.data(), numSamples);
    }

    // ── Nonlinear-filter oversampling: prepare Ladder/Warp at sr × factor ──
    // Their in-loop saturation aliases at base rate; running the per-sample loop
    // oversampled (Phase C) fixes it. The filter's coefficient g = tan(π·fc/sr)
    // is derived from its internal rate, so it must be prepared at sr×factor
    // BEFORE the sub-block setCutoff calls below. prepare() is allocation-free
    // (sr + reset + updateCoeffs) → audio-thread safe. Guarded so we only
    // re-prepare (and thus reset state) on an actual factor change, not per block.
    // SVF is linear and never oversampled; factor 1 = Off = base rate.
    if (p.filterEnabled && p.filterAlgorithm != FilterAlgorithm::SVF)
    {
        const int req    = juce::jmax(1, p.filterOsFactor);
        const int wantOs = (req >= 4) ? 4 : (req >= 2) ? 2 : 1;   // only 1/2/4 — match the OS instances
        if (wantOs != filterPreparedOsFactor_)
        {
            const double osr = sr * static_cast<double>(wantOs);
            filterLadder.prepare(osr, maxBlockSize_);
            filterLadderR.prepare(osr, maxBlockSize_);
            filterWarp.prepare(osr, maxBlockSize_);
            filterWarpR.prepare(osr, maxBlockSize_);
            filterPreparedOsFactor_ = wantOs;
        }
    }

    // Cutoff-bus depth curve for the sources whose depth is block-constant. The
    // curve is a pow(); these five values cannot change inside the block (drift
    // and LFO modulation of an env Amt already landed in `p` before the voice ran),
    // so they are resolved once per block instead of once per 32 samples. The
    // three LFO depths genuinely vary within the block — an env can be driving
    // them — and stay at the sub-block boundary below.
    float ampCutoffCurve = 0.0f, modCutoffCurve[kNumModEnvs] = {}, atCutoffCurve = 0.0f;
    if (p.filterEnabled)
    {
        if (p.ampTarget == EnvTarget::Filter)
            ampCutoffCurve = ModCalib::cutoffDepthCurve(p.ampAmount);
        for (int m = 0; m < kNumModEnvs; ++m)
            if (p.modEnv[m].target == EnvTarget::Filter)
                modCutoffCurve[m] = ModCalib::cutoffDepthCurve(p.modEnv[m].amount);
        atCutoffCurve = ModCalib::cutoffDepthCurve(p.aftertouchTargetAmt[AftertouchTarget::Cutoff]);
    }

    int pos = 0;
    while (pos < numSamples && active)
    {
        int subBlockEnd = std::min(pos + SUB_BLOCK_SIZE, numSamples);
        int subBlockLen = subBlockEnd - pos;

        // ── Sub-block boundary: update filter coefficients ONCE ──
        if (p.filterEnabled)
        {
            int midIdx = pos + subBlockLen / 2;
            const float lfo1Depth = applyAftertouchTarget(p, AftertouchTarget::LFO1Depth,
                computeEffectiveLfoDepth(p, EnvTarget::LFO1Depth, p.lfo1Depth,
                                         lastAmpEnvLevel, lastModVal_), expr);
            const float lfo2Depth = applyAftertouchTarget(p, AftertouchTarget::LFO2Depth,
                computeEffectiveLfoDepth(p, EnvTarget::LFO2Depth, p.lfo2Depth,
                                         lastAmpEnvLevel, lastModVal_), expr);
            const float lfo3Depth = applyAftertouchTarget(p, AftertouchTarget::LFO3Depth,
                computeEffectiveLfoDepth(p, EnvTarget::LFO3Depth, p.lfo3Depth,
                                         lastAmpEnvLevel, lastModVal_), expr);
            float lfo1Mid = lfo1Buf[midIdx] * lfo1Depth;
            float lfo2Mid = lfo2Buf[midIdx] * lfo2Depth;
            float lfo3Mid = lfo3Buf[midIdx] * lfo3Depth;

            float cutoffMod = p.baseCutoff;

            // Keyboard tracking. At kbd=1 the cutoff follows pitch 1:1 — one
            // octave of cutoff per octave of note, pivot at middle C (note 60);
            // kbd=0 leaves the cutoff fixed. Tracks the SOUNDING note (currentNote
            // plus the global OCT transpose octaveShift_), so the filter follows the
            // same pitch the oscillator plays.
            if (p.kbdTrack > 0.0f && currentNote >= 0)
            {
                const float soundingNote = static_cast<float>(currentNote + octaveShift_ * 12);
                cutoffMod *= std::pow(2.0f, (soundingNote - 60.0f) / 12.0f * p.kbdTrack);
            }

            // ── Cutoff modulation bus ──────────────────────────────────────
            // Every source contributes a NORMALIZED octave-fraction summed into a
            // single exponent; the destination owns the one full-scale, applied
            // once. Full depth == ±ModCalib::kCutoffModOctaves octaves — ten, the
            // whole 20 Hz–20 kHz span, so a filter envelope can open a filter from
            // any base. Each source's own DEPTH knob travels the shared curve
            // (ModCalib::cutoffDepthCurve) on the way in; its SHAPE does not, so
            // an envelope contour and an LFO waveform stay linear in octaves.
            //   env  → lastAmpEnvLevel etc. are already amount-scaled (peak == Amt)
            //   LFO  → lfo*Mid are already depth-scaled (lfoBuf · depth)
            //   Drift→ p.driftFilterOffset is normalized AND already curved
            //          (DriftLFO::depthForTarget owns the same law)
            //   expr → signed axis·amount drive in [-1..+1]. Which axis (V/X/Y/Z)
            //          is the target's own choice; timbre used to arrive here on
            //          a private path with no depth control, and no longer does.
            float cutoffOctaves = 0.0f;
            if (p.ampTarget  == EnvTarget::Filter)
                cutoffOctaves += lastAmpEnvLevel * ampCutoffCurve;
            for (int m = 0; m < kNumModEnvs; ++m)
                if (p.modEnv[m].target == EnvTarget::Filter)
                    cutoffOctaves += lastModVal_[m] * modCutoffCurve[m];
            if (p.lfo1Target == LfoTarget::Filter) cutoffOctaves += lfo1Mid * ModCalib::cutoffDepthCurve(lfo1Depth);
            if (p.lfo2Target == LfoTarget::Filter) cutoffOctaves += lfo2Mid * ModCalib::cutoffDepthCurve(lfo2Depth);
            if (p.lfo3Target == LfoTarget::Filter) cutoffOctaves += lfo3Mid * ModCalib::cutoffDepthCurve(lfo3Depth);
            cutoffOctaves += p.driftFilterOffset;
            cutoffOctaves += aftertouchDrive(p, AftertouchTarget::Cutoff, expr) * atCutoffCurve;
            // MPE timbre used to add its own ±4 octaves here, unconditionally and
            // with no depth control — the CC 74 travel WAS the amount. It is now
            // the Y source of the expression matrix instead, so it reaches the
            // cutoff the same way every other modulation does: only when the
            // player routes it there, and only as deep as they ask.
            cutoffMod *= std::pow(2.0f, cutoffOctaves * ModCalib::kCutoffModOctaves);

            cutoffMod = juce::jlimit(20.0f, 20000.0f, cutoffMod);
            const float resonanceMod = applyAftertouchTarget(
                p, AftertouchTarget::Resonance, p.baseReso, expr);
            lastModulatedCutoff_ = cutoffMod;
            lastModulatedResonance_ = resonanceMod;

            // Configure only the active filter model — the inactive ones sit
            // idle, so touching them would just waste cycles on coefficient
            // updates that no one hears. Mirror the same coefficients to the
            // right-channel instance so L and R filter identically (same
            // cutoff/reso/type/slope), with separate internal state.
            switch (p.filterAlgorithm)
            {
                case FilterAlgorithm::SVF:
                    filter.setCutoff(cutoffMod);
                    filter.setResonance(resonanceMod);
                    filter.setType(p.filterType);
                    filter.setSlope(p.filterSlope);
                    filter.setMix(p.filterMix);
                    filterR.setCutoff(cutoffMod);
                    filterR.setResonance(resonanceMod);
                    filterR.setType(p.filterType);
                    filterR.setSlope(p.filterSlope);
                    filterR.setMix(p.filterMix);
                    break;
                case FilterAlgorithm::Ladder:
                    filterLadder.setCutoff(cutoffMod);
                    filterLadder.setResonance(resonanceMod);
                    filterLadder.setType(p.filterType);
                    filterLadder.setSlope(p.filterSlope);
                    filterLadder.setMix(p.filterMix);
                    // Drive feeds the ladder's own tanh stages (Phase B stays
                    // linear for Ladder), so the character comes from the
                    // filter saturating, not from a shortcut pre-filter tanh.
                    filterLadder.setInputDrive(p.filterDriveGain);
                    filterLadderR.setCutoff(cutoffMod);
                    filterLadderR.setResonance(resonanceMod);
                    filterLadderR.setType(p.filterType);
                    filterLadderR.setSlope(p.filterSlope);
                    filterLadderR.setMix(p.filterMix);
                    filterLadderR.setInputDrive(p.filterDriveGain);
                    break;
                case FilterAlgorithm::Warp:
                    filterWarp.setCutoff(cutoffMod);
                    filterWarp.setResonance(resonanceMod);
                    filterWarp.setType(p.filterType);
                    filterWarp.setSlope(p.filterSlope);
                    filterWarp.setMix(p.filterMix);
                    filterWarp.setStyle(p.filterWarpStyle);
                    filterWarp.setInputDrive(p.filterDriveGain);
                    filterWarpR.setCutoff(cutoffMod);
                    filterWarpR.setResonance(resonanceMod);
                    filterWarpR.setType(p.filterType);
                    filterWarpR.setSlope(p.filterSlope);
                    filterWarpR.setMix(p.filterMix);
                    filterWarpR.setStyle(p.filterWarpStyle);
                    filterWarpR.setInputDrive(p.filterDriveGain);
                    break;
            }
        }

        // ── Phase A (per sample): generate raw osc/noise and cache VCA ──
        // Drive, filter and VCA are applied below as block operations so the
        // drive can be wrapped in oversampling without pulling the filter or
        // VCA up to the oversampled rate.
        float vcaScratch[SUB_BLOCK_SIZE] {};
        float outputRBuf[SUB_BLOCK_SIZE] {};   // right-channel scratch parallel to output[pos..]
        int lastI = subBlockEnd;      // exclusive end of the filled range
        bool goingIdle = false;
        for (int i = pos; i < subBlockEnd; ++i)
        {
            float ampEnvVal = ampEnv.processSample() * p.ampAmount;
            // The key's own control voltage, advanced every sample next to the
            // envelopes — unconditionally, so the ramp stays sample-locked no
            // matter which branch of computeDcaGain reads it this block.
            keyGate_.setTargetValue(noteHeld ? 1.0f : 0.0f);
            const float keyGate = keyGate_.getNextValue();
            float modEnvVals[kNumModEnvs];
            for (int m = 0; m < kNumModEnvs; ++m)
                modEnvVals[m] = modEnvs[m].processSample() * p.modEnv[m].amount;
            lastAmpEnvLevel = ampEnvVal;
            for (int m = 0; m < kNumModEnvs; ++m)
                lastModVal_[m] = modEnvVals[m];

            const float lfo1Depth = applyAftertouchTarget(p, AftertouchTarget::LFO1Depth,
                computeEffectiveLfoDepth(p, EnvTarget::LFO1Depth, p.lfo1Depth,
                                         ampEnvVal, modEnvVals), expr);
            const float lfo2Depth = applyAftertouchTarget(p, AftertouchTarget::LFO2Depth,
                computeEffectiveLfoDepth(p, EnvTarget::LFO2Depth, p.lfo2Depth,
                                         ampEnvVal, modEnvVals), expr);
            const float lfo3Depth = applyAftertouchTarget(p, AftertouchTarget::LFO3Depth,
                computeEffectiveLfoDepth(p, EnvTarget::LFO3Depth, p.lfo3Depth,
                                         ampEnvVal, modEnvVals), expr);
            float lfo1Val = lfo1Buf[i] * lfo1Depth;
            float lfo2Val = lfo2Buf[i] * lfo2Depth;
            float lfo3Val = lfo3Buf[i] * lfo3Depth;

            float sample = 0.0f;
            float sampleR = 0.0f;

            if (samplerMode)
            {
                // Sampler: read from pre-rendered pitch-shifted block (mono → duplicate to R)
                sample = samplerBlockBuf_[static_cast<size_t>(i)];
                sampleR = sample;
            }
            else if (freezeMode)
            {
                const float pitchSemis = pitchBusSemitones(
                    p, ampEnvVal, modEnvVals, lfo1Val, lfo2Val, lfo3Val);
                freezeEngine.setPitchModulation(effectivePitchRatio
                    * juce::jlimit(0.0625f, 16.0f,
                                   std::pow(2.0f, pitchSemis * ModCalib::kPitchModSemitones / 12.0f)));

                float scanMod = p.baseScan + p.driftScanOffset;
                if (p.ampTarget == EnvTarget::Scan) scanMod += ampEnvVal;
                for (int m = 0; m < kNumModEnvs; ++m)
                    if (p.modEnv[m].target == EnvTarget::Scan) scanMod += modEnvVals[m];
                if (p.lfo1Target == LfoTarget::Scan) scanMod += lfo1Val;
                if (p.lfo2Target == LfoTarget::Scan) scanMod += lfo2Val;
                if (p.lfo3Target == LfoTarget::Scan) scanMod += lfo3Val;
                scanMod = applyAftertouchTarget(p, AftertouchTarget::Scan, scanMod, expr);
                freezeEngine.setPosition(juce::jlimit(0.0f, 1.0f, scanMod));

                float freezeLeft = 0.0f;
                float freezeRight = 0.0f;
                freezeEngine.processSampleStereo(freezeLeft, freezeRight);
                sample = freezeLeft;
                sampleR = freezeRight;
                lastModulatedScan_ = freezeEngine.getCurrentPosition();
            }
            else if (oscReady)
            {
                // Wavetable: per-sample pitch/scan modulation.
                const float pitchSemis = pitchBusSemitones(
                    p, ampEnvVal, modEnvVals, lfo1Val, lfo2Val, lfo3Val);
                const float wtTargetFreq = blockBaseFreqWavetable * effectivePitchRatio
                    * std::pow(2.0f, pitchSemis * ModCalib::kPitchModSemitones / 12.0f);

                float scanBase = p.wtAutoScan ? 0.0f : p.baseScan;
                float scanMod = scanBase + p.driftScanOffset;
                if (p.ampTarget == EnvTarget::Scan) scanMod += ampEnvVal;
                for (int m = 0; m < kNumModEnvs; ++m)
                    if (p.modEnv[m].target == EnvTarget::Scan) scanMod += modEnvVals[m];
                if (p.lfo1Target == LfoTarget::Scan) scanMod += lfo1Val;
                if (p.lfo2Target == LfoTarget::Scan) scanMod += lfo2Val;
                if (p.lfo3Target == LfoTarget::Scan) scanMod += lfo3Val;
                scanMod = applyAftertouchTarget(p, AftertouchTarget::Scan, scanMod, expr);
                const float clampedScan = juce::jlimit(0.0f, 1.0f, scanMod);

                // Single wavetable oscillator (the dual A+B DCO split is dead —
                // BJ 2026-07-17). Same isGliding() gate / baseFrequency mutation /
                // operand order as the long-standing single-oscillator path.
                if (!osc.isGliding())
                {
                    baseFrequency = blockBaseFreqWavetable;
                    osc.setFrequency(wtTargetFreq);
                }
                osc.setScanPosition(clampedScan);
                sample = osc.processSample();
                sampleR = sample;

                // Effective position (includes any motion sweep) so the engine-
                // window WT scan cursor follows the gesture; identical to the
                // control-only value when motion is off.
                lastModulatedScan_ = osc.getEffectiveScanPosition();
            }
            else if (engineMode == EngineMode::Csound && csoundBuf_ != nullptr)
            {
                // Phase-1 Csound engine (spec §3): the processor-owned CsoundEngine
                // instance renders this voice's raw signal directly (its own
                // strike/orchestra DSP, driven by gate/freq/vel/pres/timb/trig
                // channels VoiceManager writes every sub-block). Everything
                // downstream from here — noise mix, drive, filter, VCA — is the
                // existing shared path, untouched. A null csoundBuf_ (non-ready
                // engine, or a stub build) already fails this branch's condition,
                // so `sample`/`sampleR` simply stay at their 0.0f initial value —
                // the documented silence safety net, never a crash.
                sample = csoundBuf_[i];
                sampleR = sample;
            }

            // Mix noise oscillator (goes through drive + filter + VCA with the main signal)
            float noiseLevel = p.noiseLevel;
            if (p.ampTarget == EnvTarget::NoiseLevel) noiseLevel += ampEnvVal;
            for (int m = 0; m < kNumModEnvs; ++m)
                if (p.modEnv[m].target == EnvTarget::NoiseLevel) noiseLevel += modEnvVals[m];
            if (p.lfo1Target == LfoTarget::NoiseLevel) noiseLevel += lfo1Val;
            if (p.lfo2Target == LfoTarget::NoiseLevel) noiseLevel += lfo2Val;
            if (p.lfo3Target == LfoTarget::NoiseLevel) noiseLevel += lfo3Val;
            // Aftertouch → Noise (additive, clamps to [0,1] internally).
            noiseLevel = applyAftertouchTarget(p, AftertouchTarget::NoiseLevel, noiseLevel, expr);
            lastModulatedNoiseLevel_ = noiseLevel;
            if (noiseLevel > 0.001f)
            {
                noise.setType(static_cast<NoiseType>(p.noiseType));
                const float n = noise.processSample() * noiseLevel;
                sample  += n;
                sampleR += n;
            }

            // Cache VCA for phase D; raw audio goes to output[i] / outputRBuf untouched.
            float vca = computeDcaGain(p, ampEnvVal, modEnvVals, keyGate);
            vca = applyAftertouchDcaGain(p, vca, expr);

            output[i] = sample;
            outputRBuf[i - pos] = sampleR;
            // Level-match the engines to one another (EngineCalib in
            // BlockParams.h carries the measurement and why the LRO is the
            // anchor). It rides the VCA, which puts it at the END of the voice
            // chain — after the noise mix, after drive, after the filter — so
            // it changes the LEVEL of the voice and nothing else about it.
            // Trimming the engine at its source instead would have retuned two
            // things a stored preset owns: the noise oscillator's balance
            // against the tone (noise is summed pre-filter and has its own
            // absolute level control), and how hard the tone drives the
            // saturating stages. Both would have moved by the trim, up to
            // 10.5 dB on the wavetable engine, and a level calibration has no
            // business changing anybody's timbre.
            //
            // WHAT THIS COSTS, because the two placements cannot both be had:
            // the noise oscillator and anything else generated inside the voice
            // (filter self-oscillation) ride the trim too, so their ABSOLUTE
            // level is now engine-dependent, up to 13.7 dB between the wavetable
            // and granular engines. That is the same span the tone used to have,
            // moved onto the noise. It is the lesser cost: the balance a preset
            // was authored with survives, and switching engines no longer moves
            // the instrument's principal voice.
            vcaScratch[i - pos] = vca * engineTrim;

            // Freeing the voice cuts its output dead AND ends it as a modulation
            // source, so it may only happen once neither job is left.
            //
            // LEVEL: whoever holds it has to be finished. Amp envelope on the
            // DCA — the envelope, exactly as before. Routed anywhere else, the
            // KEY holds it (computeDcaGain above) and its release ramp has to
            // land first, or a zero-release amp envelope would chop the gate's
            // fall mid-slope and click.
            //
            // MODULATION: an envelope routed to the delay, the reverb or an LFO
            // drives something OUTSIDE this voice, which the processor reads off
            // the newest active voice — so it keeps mattering long after the
            // voice has gone quiet, and the slot stays held for it. ALL FIVE
            // envelopes count, not just the amp one: the processor feeds them
            // into those targets through the same array. Against the voice's OWN
            // targets (filter, pitch, scan, noise) there is nothing to wait for:
            // they are inaudible the moment the level is zero.
            const bool levelDone = (p.ampTarget == EnvTarget::DCA)
                                 ? ampEnv.isIdle()
                                 : ! keyGate_.isSmoothing();
            bool stillModulating = ampOutsideVoice && ! ampEnv.isIdle();
            for (int m = 0; m < kNumModEnvs && ! stillModulating; ++m)
                stillModulating = modOutsideVoice[m] && ! modEnvs[m].isIdle();
            if (levelDone && !stillModulating && !noteHeld)
            {
                active = false;
                // Close the gate with the voice. A freed voice stops rendering,
                // so the ramp stops advancing too — and on the ENV1→DCA branch
                // the free above does not wait for it, so it can stop part-way
                // down. Left at that value, the next note to land on this slot
                // would start there instead of at zero: with a percussive amp
                // envelope (sustain 0, release 0) the envelope reaches idle in
                // under the 3 ms fall, and the following note begins at ~0.99 —
                // the full-scale step KEY_GATE_MS exists to prevent. Where the
                // key IS the authority the free already waited for the ramp, so
                // this is a no-op there.
                keyGate_.setCurrentAndTargetValue(0.0f);
                lastI = i + 1;
                for (int j = lastI; j < numSamples; ++j)
                {
                    output[j] = 0.0f;
                    if (outputRight != nullptr)
                        outputRight[j] = 0.0f;
                }
                goingIdle = true;
                break;
            }
        }

        const int driveLen = lastI - pos;

        // ── Phase B: drive stage ──
        // For SVF (linear filter): apply tanh as the saturation, optionally
        // oversampled — the SVF is LTI so the pre-filter tanh *is* the drive
        // character.
        // For Ladder / Warp (own nonlinearities): pre-filter tanh would flat-
        // clip the signal and leave nothing for the filter's internal stages
        // to shape. Instead the drive amount is forwarded to the filter via
        // setInputDrive() at sub-block setup time, and Phase B is a no-op.
        if (p.filterEnabled && p.filterDriveDb > 0.01f && driveLen > 0
            && p.filterAlgorithm == FilterAlgorithm::SVF)
        {
            const float driveGain = p.filterDriveGain;

            if (p.filterDriveOs == FilterDriveOs::Off)
            {
                for (int i = pos; i < lastI; ++i)
                {
                    output[i] = std::tanh(output[i] * driveGain);
                    outputRBuf[i - pos] = std::tanh(outputRBuf[i - pos] * driveGain);
                }
            }
            else
            {
                auto* os = (p.filterDriveOs == FilterDriveOs::X2) ? driveOs2x_.get()
                         : (p.filterDriveOs == FilterDriveOs::X4) ? driveOs4x_.get()
                         :                                           driveOs8x_.get();

                float* chPtrL = output + pos;
                float* chPtrR = outputRBuf;
                float* const channels[2] = { chPtrL, chPtrR };
                juce::dsp::AudioBlock<float> block(channels, 2, static_cast<size_t>(driveLen));
                juce::dsp::AudioBlock<const float> constBlock(block);
                auto upBlock = os->processSamplesUp(constBlock);
                const size_t upN = upBlock.getNumSamples();
                for (size_t ch = 0; ch < 2; ++ch)
                {
                    auto* upData = upBlock.getChannelPointer(ch);
                    for (size_t i = 0; i < upN; ++i)
                        upData[i] = std::tanh(upData[i] * driveGain);
                }
                os->processSamplesDown(block);
            }
        }

        // ── Phase C: per-sample filter (algorithm dispatch per sub-block) ──
        // Stereo when the source is stereo (freeze): L through left filter,
        // R through right filter, identical coefficients (mirrored at sub-block
        // setup), separate state.
        // Mono sources (sampler / wavetable) skip the right filter entirely —
        // that's the second-most-expensive piece of the voice. We then sync the
        // right filter state to the left so a switch into a stereo source
        // (freeze going active) inherits a sensible state instead of starting
        // cold. Phase D mirrors output[i] into the right channel.
        if (p.filterEnabled)
        {
            // Nonlinear filters (Ladder/Warp) optionally run oversampled to kill
            // their in-loop saturation aliasing; SVF is linear and stays at base
            // rate. osf = the factor they were prepared at this block (1/2/4),
            // so the OS instance below always matches the prepared coefficients.
            const int osf = (p.filterAlgorithm != FilterAlgorithm::SVF) ? filterPreparedOsFactor_ : 1;

            if (osf > 1)
            {
                // ── Oversampled nonlinear filter ──
                // Upsample the sub-block, run the filter per oversampled sample
                // (coeffs already set for sr×osf at sub-block setup), downsample.
                // Reuses the drive oversamplers — free here, since Phase B (drive)
                // is a no-op for non-SVF algorithms. Mirrors Phase B's block setup.
                auto* os = (osf == 2) ? driveOs2x_.get() : driveOs4x_.get();
                float* chPtrL = output + pos;
                float* chPtrR = outputRBuf;
                float* const channels[2] = { chPtrL, chPtrR };
                juce::dsp::AudioBlock<float> block(channels, 2, static_cast<size_t>(driveLen));
                juce::dsp::AudioBlock<const float> constBlock(block);
                auto upBlock = os->processSamplesUp(constBlock);
                const size_t upN = upBlock.getNumSamples();
                auto* up0 = upBlock.getChannelPointer(0);
                auto* up1 = upBlock.getChannelPointer(1);

                if (p.filterAlgorithm == FilterAlgorithm::Ladder)
                {
                    for (size_t i = 0; i < upN; ++i) up0[i] = filterLadder.processSample(up0[i]);
                    if (freezeMode)
                        for (size_t i = 0; i < upN; ++i) up1[i] = filterLadderR.processSample(up1[i]);
                }
                else // Warp
                {
                    for (size_t i = 0; i < upN; ++i) up0[i] = filterWarp.processSample(up0[i]);
                    if (freezeMode)
                        for (size_t i = 0; i < upN; ++i) up1[i] = filterWarpR.processSample(up1[i]);
                }

                os->processSamplesDown(block);

                if (! freezeMode)
                {
                    // Mono: only the left filter ran (Phase D mirrors L→R). Sync the
                    // right filter state to the left — matches the base-rate path so
                    // a later switch into a stereo source inherits sensible state.
                    if (p.filterAlgorithm == FilterAlgorithm::Ladder) filterLadderR = filterLadder;
                    else                                              filterWarpR   = filterWarp;
                }
            }
            else if (freezeMode)
            {
                switch (p.filterAlgorithm)
                {
                    case FilterAlgorithm::SVF:
                        for (int i = pos; i < lastI; ++i)
                        {
                            output[i]            = filter.processSample(output[i]);
                            outputRBuf[i - pos]  = filterR.processSample(outputRBuf[i - pos]);
                        }
                        break;
                    case FilterAlgorithm::Ladder:
                        for (int i = pos; i < lastI; ++i)
                        {
                            output[i]            = filterLadder.processSample(output[i]);
                            outputRBuf[i - pos]  = filterLadderR.processSample(outputRBuf[i - pos]);
                        }
                        break;
                    case FilterAlgorithm::Warp:
                        for (int i = pos; i < lastI; ++i)
                        {
                            output[i]            = filterWarp.processSample(output[i]);
                            outputRBuf[i - pos]  = filterWarpR.processSample(outputRBuf[i - pos]);
                        }
                        break;
                }
            }
            else
            {
                switch (p.filterAlgorithm)
                {
                    case FilterAlgorithm::SVF:
                        for (int i = pos; i < lastI; ++i)
                            output[i] = filter.processSample(output[i]);
                        filterR = filter;
                        break;
                    case FilterAlgorithm::Ladder:
                        for (int i = pos; i < lastI; ++i)
                            output[i] = filterLadder.processSample(output[i]);
                        filterLadderR = filterLadder;
                        break;
                    case FilterAlgorithm::Warp:
                        for (int i = pos; i < lastI; ++i)
                            output[i] = filterWarp.processSample(output[i]);
                        filterWarpR = filterWarp;
                        break;
                }
            }
        }

        // ── Phase D: per-sample VCA + write ──
        // Stereo source (freeze): each channel carries its independently-filtered
        // signal. Mono source: R is just a copy of L — Phase C only filtered the
        // left side. Restart fade applies the same time-ramp `t` to both channels
        // (decrements the counter once per sample-pair).
        for (int i = pos; i < lastI; ++i)
        {
            const float vca = vcaScratch[i - pos];
            float L = output[i] * vca;
            float R = freezeMode ? outputRBuf[i - pos] * vca : L;
            applyRestartFadeStereo(L, R);
            lastOutputSample_  = L;
            lastOutputSampleR_ = R;

            if (outputRight != nullptr)
            {
                output[i]      = L;
                outputRight[i] = R;
            }
            else
            {
                output[i] = 0.5f * (L + R);
            }
        }

        if (goingIdle)
            return;

        pos = subBlockEnd;
    }
}
