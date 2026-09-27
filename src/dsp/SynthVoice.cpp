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
    // A HALF-STEP, not `!= 0.0f`. The amount comes off a 0.01-step control whose
    // rest value is -2.235e-08 rather than 0 (juce::NormalisableRange computes
    // -1.0f + 0.01f * 100.0f; see AftertouchBar::isAtRest), so an exact
    // comparison calls every untouched row active. Nothing audible follows from
    // a depth of 2e-08, but "off" should mean off where the player set it off.
    //
    // BOTH halves, not the amount alone. A row set to Ø says nothing drives it,
    // and a depth without an axis is not a routing -- it is a number nobody can
    // reach. Everywhere but the DCA that distinction is free, because drive 0
    // through applyNormalizedOffset is the identity; on the DCA it is not, and
    // the row would have kept its RESTING ATTENUATION while claiming to be off.
    return std::abs (p.aftertouchTargetAmt[target]) >= kAftertouchAmtEpsilon
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
    // The transition bank mirrors the live one at base rate; a transition's
    // prepareNonlinearAt re-preps its nonlinear filters to sr×factor on
    // demand, exactly like the live bank below.
    xfFilter_.prepare(sampleRate, samplesPerBlock);
    xfLadder_.prepare(sampleRate, samplesPerBlock);
    xfWarp_.prepare(sampleRate, samplesPerBlock);
    xfFilterR_.prepare(sampleRate, samplesPerBlock);
    xfLadderR_.prepare(sampleRate, samplesPerBlock);
    xfWarpR_.prepare(sampleRate, samplesPerBlock);
    // Pre-roll scratch instances for the shared-kind SVF transition (see the
    // class declaration comment) — real members so the pre-roll can run
    // allocation-free on the audio thread.
    xfPreRollSvf_.prepare(sampleRate, samplesPerBlock);
    xfPreRollSvfR_.prepare(sampleRate, samplesPerBlock);
    // The nonlinear filters were just (re-)prepared at BASE rate. Invalidate the
    // cached OS factor so the sr×factor re-prepare re-runs on the next render.
    // Without this, a host prepareToPlay (sample-rate / buffer-size change) rewinds
    // the filters to base rate while filterPreparedOsFactor_ still claims 2/4, and
    // Phase C would oversample with base-rate coefficients (cutoff an octave off).
    filterPreparedOsFactor_ = 1;
    xfPreparedOsFactor_ = 1;

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

    // Transition bank's own oversamplers — same construction as the live
    // ones, so completeFilterTransition's swap always exchanges like for like.
    xfOs2x_ = std::make_unique<Os>(2, 1, Os::filterHalfBandPolyphaseIIR, true, false);
    xfOs4x_ = std::make_unique<Os>(2, 2, Os::filterHalfBandPolyphaseIIR, true, false);
    xfOs8x_ = std::make_unique<Os>(2, 3, Os::filterHalfBandPolyphaseIIR, true, false);
    xfOs2x_->initProcessing(static_cast<size_t>(SUB_BLOCK_SIZE));
    xfOs4x_->initProcessing(static_cast<size_t>(SUB_BLOCK_SIZE));
    xfOs8x_->initProcessing(static_cast<size_t>(SUB_BLOCK_SIZE));

    // Filter-input history (pre-roll source) — the ONLY place this buffer is
    // sized. std::max(1, ...) so a division against its capacity is always
    // safe even at a pathologically low sample rate.
    filterHistL_.assign(static_cast<size_t>(std::max(1, juce::roundToInt(FILTER_XF_PREROLL_MS * 0.001 * sampleRate))), 0.0f);
    filterHistR_.assign(filterHistL_.size(), 0.0f);
    filterHistPos_ = 0;
    filterHistCount_ = 0;

    // No transition survives a (re-)prepare, and nothing has been heard
    // through the filter yet — the next renderBlock adopts p's configuration
    // outright (see noteOn / filterCfgAdopt_).
    filterXf_ = FilterXf::None;
    liveFilterCfg_ = FilterCfg{};
    filterCfgAdopt_ = true;
    filterXfPreRollPending_ = false;

    // Same story for the envelope routing/amount ramp (see noteOn /
    // envRouteAdopt_): nothing is sounding through it yet, so the next
    // renderBlock takes p's routing outright. The de-zippered twins are
    // copied from the raw levels they mirror rather than zeroed: prepare()
    // can land mid-note (a host prepareToPlay while a note is held survives
    // the sample-rate change), and a block-rate reader that runs before
    // Phase A writes them for real this block (the sampler-mode pitch
    // computation below) would otherwise see a phantom zero next to a real,
    // sounding envelope level.
    envRouteAdopt_ = true;
    envRouteRampLeft_ = 0;
    lastAmpEnvLevelSm_ = lastAmpEnvLevel;
    for (int m = 0; m < kNumModEnvs; ++m)
        lastModValSm_[m] = lastModVal_[m];

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
    // Mirrors the live filters/oversamplers below: a reset voice has no
    // transition in flight and nothing sounding to preserve state for.
    xfFilter_.reset();
    xfLadder_.reset();
    xfWarp_.reset();
    xfFilterR_.reset();
    xfLadderR_.reset();
    xfWarpR_.reset();
    xfPreRollSvf_.reset();
    xfPreRollSvfR_.reset();
    noise.reset();
    if (driveOs2x_) driveOs2x_->reset();
    if (driveOs4x_) driveOs4x_->reset();
    if (driveOs8x_) driveOs8x_->reset();
    if (xfOs2x_) xfOs2x_->reset();
    if (xfOs4x_) xfOs4x_->reset();
    if (xfOs8x_) xfOs8x_->reset();
    // No transition survives a reset; the next renderBlock adopts p's filter
    // configuration outright (see noteOn / filterCfgAdopt_).
    filterXf_ = FilterXf::None;
    liveFilterCfg_ = FilterCfg{};
    filterCfgAdopt_ = true;
    filterXfPreRollPending_ = false;
    filterHistPos_ = 0;
    filterHistCount_ = 0;
    // Same story for the envelope routing/amount ramp (see prepare()).
    envRouteAdopt_ = true;
    envRouteRampLeft_ = 0;
    active = false;
    noteHeld = false;
    keyGate_.setCurrentAndTargetValue(0.0f);
    currentNote = -1;
    aftertouch_ = 0.0f;
    lastAmpEnvLevel = 0.0f;
    for (auto& v : lastModVal_) v = 0.0f;
    lastAmpEnvLevelSm_ = 0.0f;
    for (auto& v : lastModValSm_) v = 0.0f;
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
    // A fresh strike on a sounding voice: see filterCfgAdopt_ in SynthVoice.h.
    filterCfgAdopt_ = true;
    envRouteAdopt_ = true;
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
    // Captured before active flips, so a note struck from idle (wasActive ==
    // false) can be told apart from a steal/retrigger/legato on a voice that
    // is already sounding — see the filterCfgAdopt_ use right below.
    const bool wasActive = active;
    currentNote = note;
    currentVelocity = velocity;
    noteHeld = true;
    active = true;
    // A note from idle has nothing sounding through the filter (or an
    // envelope routing) to fade from: the next renderBlock takes p's filter
    // configuration outright rather than transitioning into it (see FilterCfg
    // / renderBlock). A steal or retrigger of a sounding voice does the same
    // through beginRestartFade, which VoiceManager calls first; only a legato
    // continuation keeps whatever transition is already running or due.
    if (! wasActive)
    {
        filterCfgAdopt_ = true;
        envRouteAdopt_ = true;
        // A note from idle has no recent input of its own to pre-roll from.
        filterHistPos_ = 0;
        filterHistCount_ = 0;
    }
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

void SynthVoice::cutSound()
{
    if (! active) return;
    noteHeld = false;               // the key gate closes over KEY_GATE_MS
    // 0 rather than a number of my own: ADSREnvelope::beginRelease floors every
    // release at MIN_RAMP_SEC, so this asks for the floor and there is exactly
    // one place that says what the floor is. The value does not leak into the
    // patch -- beginRelease has already captured releaseTotalSamples, and both
    // noteOn and configureForBlock re-apply the patch's own times anyway.
    ampEnv.setRelease(0.0f);
    ampEnv.noteOff();
    // The mod envelopes to the SAME floor, not to the patch's release. Two
    // reasons, and the second is the one that bites. beginRelease restarts from
    // the current level over whatever releaseMs says, and configureForBlock has
    // just re-applied the patch's -- so a plain noteOff() here LENGTHENS a mod
    // envelope that was already falling, measured +580 ms on a 2 s release,
    // which is the very thing the key-up path skips releasing voices to avoid.
    // And a mod envelope whose target is outside the voice (delay, reverb, the
    // LFO rates -- EnvTarget::isOutsideTheVoice) holds `stillModulating` true
    // in renderBlock, so the slot stays allocated and goes on sweeping the
    // master delay and reverb for its whole release: default 4 s, up to 10,
    // after a message whose name is All Sound Off.
    for (auto& e : modEnvs)
    {
        e.setRelease(0.0f);
        e.noteOff();
    }
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
    //
    // Clamped like the freeze path's own pitch ratio: a full-scale bus is +-1
    // octave, but several sources summing can exceed that, and the orchestra's
    // `limit kfreq, 20, 12000` would then pin the note to a rail rather than
    // bend it.
    auto ratio = [&] (const float (&w)[kNumEnvSources][EnvTarget::kCount],
                      float ampLevel, const float* modLevels) -> float
    {
        const float d1 = applyAftertouchTarget(p, AftertouchTarget::LFO1Depth,
            computeEffectiveLfoDepthRouted(w, EnvTarget::LFO1Depth, p.lfo1Depth, ampLevel, modLevels), expr);
        const float d2 = applyAftertouchTarget(p, AftertouchTarget::LFO2Depth,
            computeEffectiveLfoDepthRouted(w, EnvTarget::LFO2Depth, p.lfo2Depth, ampLevel, modLevels), expr);
        const float d3 = applyAftertouchTarget(p, AftertouchTarget::LFO3Depth,
            computeEffectiveLfoDepthRouted(w, EnvTarget::LFO3Depth, p.lfo3Depth, ampLevel, modLevels), expr);
        const float semis = pitchBusSemitones(p, lastAmpEnvLevel, lastModVal_,
                                              lfo1Raw * d1, lfo2Raw * d2, lfo3Raw * d3);
        return juce::jlimit(0.0625f, 16.0f,
                            std::pow(2.0f, semis * ModCalib::kPitchModSemitones / 12.0f));
    };

    if (envRouteAdopt_)
    {
        // VoiceManager calls this before the voice's first renderBlock of a
        // new note, so while adopt is pending envRouteW_ still describes the
        // SLOT'S PREVIOUS note. Reproduce what renderBlock's own adopt branch
        // is about to do instead: a one-hot weight built from p's own targets.
        // That branch also copies the raw levels into their de-zippered twins,
        // so the raw levels used below are the same arithmetic either way.
        float w[kNumEnvSources][EnvTarget::kCount] {};
        for (int e = 0; e < kNumEnvSources; ++e)
        {
            const int t = envSourceTarget(p, e);
            if (t >= 0 && t < EnvTarget::kCount)
                w[e][t] = 1.0f;
        }
        return ratio(w, lastAmpEnvLevel, lastModVal_);
    }
    return ratio(envRouteW_, lastAmpEnvLevelSm_, lastModValSm_);
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

// ── Envelope routing/amount de-zippering ──────────────────────────────────
// See the envRouteW_ / envAmtDelta_ member comments in SynthVoice.h for the
// design. e indexes envelope sources: 0 = ENV 1 (amp), 1..4 = ENV 2..5.

int SynthVoice::envSourceTarget (const BlockParams& p, int e)
{
    return e == 0 ? p.ampTarget : p.modEnv[e - 1].target;
}

float SynthVoice::envSourceAmount (const BlockParams& p, int e)
{
    return e == 0 ? p.ampAmount : p.modEnv[e - 1].amount;
}

float SynthVoice::envSourceAmountBase (const BlockParams& p, int e)
{
    return e == 0 ? p.ampAmountBase : p.modEnv[e - 1].amountBase;
}

float SynthVoice::envAmtEff (const BlockParams& p, int e) const
{
    if (envRouteRampLeft_ == 0)
        return envSourceAmount(p, e);
    // Mid-ramp: p's amount, drift and LFO included, plus the part of the step
    // still left. Both lie in [0, 1] at the ramp's start, so the clamp only
    // catches drift moving p's amount during the ramp itself.
    return juce::jlimit(0.0f, 1.0f, envSourceAmount(p, e) + envAmtDelta_[e]);
}

void SynthVoice::updateEnvRouteGoals (const BlockParams& p)
{
    int tgt[kNumEnvSources];
    float base[kNumEnvSources];
    for (int e = 0; e < kNumEnvSources; ++e)
    {
        tgt[e]  = envSourceTarget(p, e);
        base[e] = envSourceAmountBase(p, e);
    }

    if (envRouteAdopt_)
    {
        // Nothing is sounding through this routing yet (note from idle,
        // prepare/reset): take it outright, no ramp.
        for (int e = 0; e < kNumEnvSources; ++e)
        {
            for (int t = 0; t < EnvTarget::kCount; ++t)
                envRouteW_[e][t] = 0.0f;
            if (tgt[e] >= 0 && tgt[e] < EnvTarget::kCount)
                envRouteW_[e][tgt[e]] = 1.0f;
            envAmtGoal_[e] = base[e];
            envAmtLast_[e] = envSourceAmount(p, e);
            envAmtDelta_[e] = 0.0f;
            envRouteGoalTarget_[e] = tgt[e];
        }
        // The de-zippered twins may still hold the SLOT'S PREVIOUS note (freed
        // mid-ramp, or mid-transition via beginRestartFade), and two readers run
        // before this call's Phase A sets them for real: the sampler-mode pitch
        // (computed once per renderBlock, before the sub-block loop) and the
        // first sub-block's cutoff (modulatedCutoffHz). Settled, twin == raw
        // everywhere else, so this just makes that true immediately.
        lastAmpEnvLevelSm_ = lastAmpEnvLevel;
        for (int m = 0; m < kNumModEnvs; ++m)
            lastModValSm_[m] = lastModVal_[m];
        envRouteRampLeft_ = 0;
        envRouteAdopt_ = false;
        return;
    }

    // Per source: did ITS OWN routing move against the running goal? Only
    // this test arms anything -- a neighbour's change never restarts a
    // source that did not itself move.
    bool changed[kNumEnvSources];
    for (int e = 0; e < kNumEnvSources; ++e)
        changed[e] = tgt[e] != envRouteGoalTarget_[e] || ! juce::exactlyEqual(base[e], envAmtGoal_[e]);

    // A change entirely between non-ramped destinations (Pitch, Scan, and the
    // processor-side targets: delay/reverb/LFO rate) has nothing to declick --
    // those are read straight off p everywhere, never through envRouteW_/
    // envAmtEff. Take it outright instead of arming a ramp: a ramp there would
    // only spend CPU and (via cutoffGlides below) start the per-sample cutoff
    // glide for a destination that cannot hear it.
    // Not while the source still carries weight on a ramped destination,
    // though: a change off one less than a ramp ago is still fading it out,
    // and taking the next change outright would cut that fade off in a step.
    const auto carriesRampedWeight = [this] (int e)
    {
        for (int t = 0; t < EnvTarget::kCount; ++t)
            if (envTargetRamped(t) && ! juce::exactlyEqual(envRouteW_[e][t], 0.0f))
                return true;
        return false;
    };
    bool armRamp = false;
    for (int e = 0; e < kNumEnvSources; ++e)
    {
        if (! changed[e])
            continue;
        if (! envTargetRamped(envRouteGoalTarget_[e]) && ! envTargetRamped(tgt[e])
            && ! carriesRampedWeight(e))
        {
            for (int t = 0; t < EnvTarget::kCount; ++t)
                envRouteW_[e][t] = 0.0f;
            if (tgt[e] >= 0 && tgt[e] < EnvTarget::kCount)
                envRouteW_[e][tgt[e]] = 1.0f;
            for (int t = 0; t < EnvTarget::kCount; ++t)
                envRouteWStep_[e][t] = 0.0f;
            envAmtGoal_[e] = base[e];
            envRouteGoalTarget_[e] = tgt[e];
            // envAmtDelta_[e] is left as it is: no ramped destination reads this
            // source (no weight on one, now or before), and nothing here starts it.
            changed[e] = false;
        }
        else
        {
            armRamp = true;
        }
    }
    if (! armRamp)
        return;

    // A new goal -- from WHEREVER the ramp currently is, so a second change
    // mid-ramp stays continuous rather than restarting from the last target.
    // Only a source that ITSELF changed takes a new amount delta, measured
    // against what the last sample actually used (envAmtLast_). An unchanged
    // source (including one just taken outright above) keeps its current
    // envAmtDelta_ -- exactly 0 once settled, or whatever is left of an
    // earlier ramp -- and only its step is rebuilt, so it still reaches 0
    // exactly when this ramp ends. That is what makes the routing comment's
    // claim true: drift or an LFO moving an Amt (which moves envSourceAmount
    // but never envAmtGoal_/the base) never restarts a ramp on a source a
    // neighbour's change happens to be re-arming.
    const int n = std::max(1, juce::roundToInt(ENV_ROUTE_RAMP_MS * 0.001 * sr));
    for (int e = 0; e < kNumEnvSources; ++e)
    {
        for (int t = 0; t < EnvTarget::kCount; ++t)
        {
            const float goal = (t == tgt[e]) ? 1.0f : 0.0f;
            envRouteWStep_[e][t] = (goal - envRouteW_[e][t]) / static_cast<float>(n);
        }
        if (changed[e])
            envAmtDelta_[e] = envAmtLast_[e] - envSourceAmount(p, e);
        envAmtDeltaStep_[e] = envAmtDelta_[e] / static_cast<float>(n);
        envAmtGoal_[e] = base[e];
        envRouteGoalTarget_[e] = tgt[e];
    }
    envRouteRampLeft_ = n;
}

// Ramped destinations: the ones renderBlock reads through envRouteW_/
// envAmtEff rather than straight off p (see the class comment above). Kept as
// a plain OR rather than a lookup table -- six names read faster than an
// array only EnvTarget.h's constants explain.
bool SynthVoice::envTargetRamped (int t)
{
    return t == EnvTarget::DCA || t == EnvTarget::Filter || t == EnvTarget::NoiseLevel
        || t == EnvTarget::LFO1Depth || t == EnvTarget::LFO2Depth || t == EnvTarget::LFO3Depth;
}

void SynthVoice::advanceEnvRoute() noexcept
{
    if (envRouteRampLeft_ <= 0)
        return;
    if (--envRouteRampLeft_ == 0)
    {
        // Snap to the exact goal rather than trust n steps of float addition
        // to have landed there -- the steady-state test right after this
        // depends on the weights being EXACTLY 0.0f/1.0f, not merely close.
        for (int e = 0; e < kNumEnvSources; ++e)
        {
            for (int t = 0; t < EnvTarget::kCount; ++t)
                envRouteW_[e][t] = (t == envRouteGoalTarget_[e]) ? 1.0f : 0.0f;
            envAmtDelta_[e] = 0.0f;
        }
    }
    else
    {
        for (int e = 0; e < kNumEnvSources; ++e)
        {
            for (int t = 0; t < EnvTarget::kCount; ++t)
                envRouteW_[e][t] += envRouteWStep_[e][t];
            envAmtDelta_[e] -= envAmtDeltaStep_[e];
        }
    }
}

// An LFO's depth with the envelopes routed to it added on: weighted by
// envRouteW_ rather than read from p.ampTarget/p.modEnv[m].target, so a
// routing change ramps instead of stepping. Amp envelope first, then ENV 2..5,
// the running depth clamped into [0, 1] after each, and a zero weight adds
// nothing, so settled (every weight exactly 0.0f or 1.0f) this is the sum of
// the envelopes routed to that LFO's depth.
float SynthVoice::computeEffectiveLfoDepthRouted (const float (&w)[kNumEnvSources][EnvTarget::kCount],
                                                  int target, float baseDepth,
                                                  float ampEnvVal, const float* modEnvVals)
{
    float depth = baseDepth;
    if (! juce::exactlyEqual(w[0][target], 0.0f))
        depth = applyNormalizedOffset(depth, w[0][target] * ampEnvVal);
    for (int m = 0; m < kNumModEnvs; ++m)
        if (! juce::exactlyEqual(w[m + 1][target], 0.0f))
            depth = applyNormalizedOffset(depth, w[m + 1][target] * modEnvVals[m]);
    return depth;
}

// computeDcaGain above with the routing weights instead of p's targets. The
// XOR its comment describes holds whenever envRouteW_[0][DCA] is 0.0f or
// 1.0f; while a routing change is in flight the level is a linear blend of
// the envelope and the key instead of a switch between them.
float SynthVoice::computeDcaGainRouted (const float (&w)[kNumEnvSources][EnvTarget::kCount],
                                        float ampEnvVal, const float* modEnvVals, float keyGate)
{
    const float a = w[0][EnvTarget::DCA];
    float vca = a >= 1.0f ? ampEnvVal
              : a <= 0.0f ? keyGate
              : a * ampEnvVal + (1.0f - a) * keyGate;
    for (int m = 0; m < kNumModEnvs; ++m)
        if (! juce::exactlyEqual(w[m + 1][EnvTarget::DCA], 0.0f))
            vca *= (1.0f + w[m + 1][EnvTarget::DCA] * modEnvVals[m]);
    return std::max(0.0f, vca);
}

// ── Filter-switch transition (click-free, latency-free discrete filter
// changes) ──────────────────────────────────────────────────────────────
// See FilterCfg / the xf* members in SynthVoice.h for the design: a discrete
// change never lands on the live filters in place. filterCfgFrom reduces a
// block's params to the canonical, comparable configuration; the functions
// below run that configuration on a bank (live or transition), pre-roll the
// transition bank's target configuration over the recent input before it is
// heard, and hand the live bank off to the transition bank once the fade
// ends.

SynthVoice::FilterCfg SynthVoice::filterCfgFrom (const BlockParams& p)
{
    // Canonical form: a field that cannot matter for the given model/state is
    // pinned to its default, so two configs that would SOUND identical also
    // COMPARE identical — otherwise an inaudible field (say, warpStyle while
    // running SVF) could start a transition for nothing.
    if (! p.filterEnabled)
        return {};

    FilterCfg c;
    c.enabled   = true;
    c.algorithm = p.filterAlgorithm;
    c.slope     = p.filterSlope;
    c.type      = p.filterType;
    c.warpStyle = (c.algorithm == FilterAlgorithm::Warp) ? p.filterWarpStyle : 0;
    // Same threshold as Phase B's own drive-active guard.
    c.svfDrive   = (c.algorithm == FilterAlgorithm::SVF) && (p.filterDriveDb > 0.01f);
    c.svfDriveOs = c.svfDrive ? p.filterDriveOs : 0;
    if (c.algorithm != FilterAlgorithm::SVF)
    {
        // Today's wantOs mapping (the old block-level OS re-prepare guard):
        // only 1/2/4 are real oversampler instances, so the request folds
        // down to the nearest one at or below it.
        const int req = juce::jmax(1, p.filterOsFactor);
        c.nlOs = (req >= 4) ? 4 : (req >= 2) ? 2 : 1;
    }
    return c;
}

void SynthVoice::prepareNonlinearAt (FilterBank b, int factor)
{
    // Allocation-free (sr + reset + updateCoeffs) — audio-thread safe. Called
    // only on an actual factor change (see call sites), never per block.
    const double osr = sr * static_cast<double>(factor);
    b.ladder.prepare(osr, maxBlockSize_);
    b.ladderR.prepare(osr, maxBlockSize_);
    b.warp.prepare(osr, maxBlockSize_);
    b.warpR.prepare(osr, maxBlockSize_);
    b.preparedOs = factor;
}

void SynthVoice::configureFilterBank (FilterBank b, const FilterCfg& c, float cutoffHz, float reso, const BlockParams& p, bool exact)
{
    // Configure only the active filter model — the inactive ones sit idle, so
    // touching them would just waste cycles on coefficient updates that no
    // one hears. Mirror the same coefficients to the right-channel instance
    // so L and R filter identically (same cutoff/reso/type/slope), with
    // separate internal state.
    switch (c.algorithm)
    {
        case FilterAlgorithm::SVF:
            b.svf.setCutoff(cutoffHz, exact);
            b.svf.setResonance(reso);
            b.svf.setType(c.type);
            b.svf.setSlope(c.slope);
            b.svf.setMix(p.filterMix);
            b.svfR.setCutoff(cutoffHz, exact);
            b.svfR.setResonance(reso);
            b.svfR.setType(c.type);
            b.svfR.setSlope(c.slope);
            b.svfR.setMix(p.filterMix);
            break;
        case FilterAlgorithm::Ladder:
            b.ladder.setCutoff(cutoffHz, exact);
            b.ladder.setResonance(reso);
            b.ladder.setType(c.type);
            b.ladder.setSlope(c.slope);
            b.ladder.setMix(p.filterMix);
            // Drive feeds the ladder's own tanh stages (Phase B stays linear
            // for Ladder), so the character comes from the filter
            // saturating, not from a shortcut pre-filter tanh.
            b.ladder.setInputDrive(p.filterDriveGain);
            b.ladderR.setCutoff(cutoffHz, exact);
            b.ladderR.setResonance(reso);
            b.ladderR.setType(c.type);
            b.ladderR.setSlope(c.slope);
            b.ladderR.setMix(p.filterMix);
            b.ladderR.setInputDrive(p.filterDriveGain);
            break;
        case FilterAlgorithm::Warp:
            b.warp.setCutoff(cutoffHz, exact);
            b.warp.setResonance(reso);
            b.warp.setType(c.type);
            b.warp.setSlope(c.slope);
            b.warp.setMix(p.filterMix);
            b.warp.setStyle(c.warpStyle);
            b.warp.setInputDrive(p.filterDriveGain);
            b.warpR.setCutoff(cutoffHz, exact);
            b.warpR.setResonance(reso);
            b.warpR.setType(c.type);
            b.warpR.setSlope(c.slope);
            b.warpR.setMix(p.filterMix);
            b.warpR.setStyle(c.warpStyle);
            b.warpR.setInputDrive(p.filterDriveGain);
            break;
    }
}

void SynthVoice::glideCutoff (FilterBank b, const FilterCfg& c, const CutoffGlide& g, int j, bool left, bool right)
{
    const float hz = g.hzAt(j);
    switch (c.algorithm)
    {
        case FilterAlgorithm::SVF:
            if (left)  b.svf.setCutoff(hz);
            if (right) b.svfR.setCutoff(hz);
            break;
        case FilterAlgorithm::Ladder:
            if (left)  b.ladder.setCutoff(hz);
            if (right) b.ladderR.setCutoff(hz);
            break;
        case FilterAlgorithm::Warp:
            if (left)  b.warp.setCutoff(hz);
            if (right) b.warpR.setCutoff(hz);
            break;
    }
}

void SynthVoice::processDriveStage (FilterBank b, const FilterCfg& c, float* L, float* R, int n, float driveGain)
{
    // ── Phase B: drive stage ──
    // For SVF (linear filter): apply tanh as the saturation, optionally
    // oversampled — the SVF is LTI so the pre-filter tanh *is* the drive
    // character.
    // For Ladder / Warp (own nonlinearities): pre-filter tanh would flat-
    // clip the signal and leave nothing for the filter's internal stages
    // to shape. Instead the drive amount is forwarded to the filter via
    // setInputDrive() in configureFilterBank, and Phase B is a no-op.
    if (c.algorithm == FilterAlgorithm::SVF && c.svfDrive)
    {
        if (c.svfDriveOs == FilterDriveOs::Off)
        {
            for (int j = 0; j < n; ++j)
            {
                L[j] = std::tanh(L[j] * driveGain);
                R[j] = std::tanh(R[j] * driveGain);
            }
        }
        else
        {
            auto* os = (c.svfDriveOs == FilterDriveOs::X2) ? b.os2
                     : (c.svfDriveOs == FilterDriveOs::X4) ? b.os4
                     :                                       b.os8;

            float* const channels[2] = { L, R };
            juce::dsp::AudioBlock<float> block(channels, 2, static_cast<size_t>(n));
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
}

void SynthVoice::processFilterStages (FilterBank b, const FilterCfg& c, float* L, float* R, int n, bool stereo,
                                      const CutoffGlide* glide)
{
    // ── Phase C: per-sample filter (algorithm dispatch) ──
    // Stereo when the source is stereo (freeze): L through left filter, R
    // through right filter, identical coefficients (mirrored in
    // configureFilterBank), separate state.
    // Mono sources (sampler / wavetable) skip the right filter entirely —
    // that's the second-most-expensive piece of the voice. We then sync the
    // right filter state to the left so a switch into a stereo source
    // (freeze going active) inherits a sensible state instead of starting
    // cold. Phase D (renderBlock) mirrors L into the right channel.
    // With a glide (CutoffGlide), each base-rate sample, and each osf-sample
    // group of an oversampled one, first takes its own cutoff.
    const int osf = (c.algorithm != FilterAlgorithm::SVF) ? b.preparedOs : 1;
    const auto glideAt = [&] (size_t i, bool left, bool right)
    {
        if (glide != nullptr && i % static_cast<size_t>(osf) == 0)
            glideCutoff(b, c, *glide, static_cast<int>(i / static_cast<size_t>(osf)), left, right);
    };

    if (osf > 1)
    {
        // ── Oversampled nonlinear filter ──
        // Upsample the sub-block, run the filter per oversampled sample
        // (coeffs already set for sr×osf in configureFilterBank), downsample.
        // Reuses the drive oversamplers — free here, since Phase B (drive) is
        // a no-op for non-SVF algorithms. Mirrors Phase B's block setup.
        auto* os = (osf == 2) ? b.os2 : b.os4;
        float* const channels[2] = { L, R };
        juce::dsp::AudioBlock<float> block(channels, 2, static_cast<size_t>(n));
        juce::dsp::AudioBlock<const float> constBlock(block);
        auto upBlock = os->processSamplesUp(constBlock);
        const size_t upN = upBlock.getNumSamples();
        auto* up0 = upBlock.getChannelPointer(0);
        auto* up1 = upBlock.getChannelPointer(1);

        if (c.algorithm == FilterAlgorithm::Ladder)
        {
            for (size_t i = 0; i < upN; ++i) { glideAt(i, true, false); up0[i] = b.ladder.processSample(up0[i]); }
            if (stereo)
                for (size_t i = 0; i < upN; ++i) { glideAt(i, false, true); up1[i] = b.ladderR.processSample(up1[i]); }
        }
        else // Warp
        {
            for (size_t i = 0; i < upN; ++i) { glideAt(i, true, false); up0[i] = b.warp.processSample(up0[i]); }
            if (stereo)
                for (size_t i = 0; i < upN; ++i) { glideAt(i, false, true); up1[i] = b.warpR.processSample(up1[i]); }
        }

        os->processSamplesDown(block);

        if (! stereo)
        {
            // Mono: only the left filter ran (Phase D mirrors L→R). Sync the
            // right filter state to the left — matches the base-rate path so
            // a later switch into a stereo source inherits sensible state.
            if (c.algorithm == FilterAlgorithm::Ladder) b.ladderR = b.ladder;
            else                                        b.warpR   = b.warp;
        }
    }
    else if (stereo)
    {
        switch (c.algorithm)
        {
            case FilterAlgorithm::SVF:
                for (int j = 0; j < n; ++j)
                {
                    glideAt(static_cast<size_t>(j), true, true);
                    L[j] = b.svf.processSample(L[j]);
                    R[j] = b.svfR.processSample(R[j]);
                }
                break;
            case FilterAlgorithm::Ladder:
                for (int j = 0; j < n; ++j)
                {
                    glideAt(static_cast<size_t>(j), true, true);
                    L[j] = b.ladder.processSample(L[j]);
                    R[j] = b.ladderR.processSample(R[j]);
                }
                break;
            case FilterAlgorithm::Warp:
                for (int j = 0; j < n; ++j)
                {
                    glideAt(static_cast<size_t>(j), true, true);
                    L[j] = b.warp.processSample(L[j]);
                    R[j] = b.warpR.processSample(R[j]);
                }
                break;
        }
    }
    else
    {
        switch (c.algorithm)
        {
            case FilterAlgorithm::SVF:
                for (int j = 0; j < n; ++j)
                {
                    glideAt(static_cast<size_t>(j), true, false);
                    L[j] = b.svf.processSample(L[j]);
                }
                b.svfR = b.svf;
                break;
            case FilterAlgorithm::Ladder:
                for (int j = 0; j < n; ++j)
                {
                    glideAt(static_cast<size_t>(j), true, false);
                    L[j] = b.ladder.processSample(L[j]);
                }
                b.ladderR = b.ladder;
                break;
            case FilterAlgorithm::Warp:
                for (int j = 0; j < n; ++j)
                {
                    glideAt(static_cast<size_t>(j), true, false);
                    L[j] = b.warp.processSample(L[j]);
                }
                b.warpR = b.warp;
                break;
        }
    }
}

void SynthVoice::processFilterPath (FilterBank b, const FilterCfg& c, float* L, float* R, int n, bool stereo, float driveGain,
                                    const CutoffGlide* glide)
{
    // A disabled path is the identity. The transition machinery in
    // renderBlock calls this for both banks whenever a transition is in
    // flight, whether or not each side is actually enabled — this bail-out
    // is what keeps a disabled bank from touching audio at all.
    if (! c.enabled || n <= 0)
        return;

    processDriveStage(b, c, L, R, n, driveGain);
    processFilterStages(b, c, L, R, n, stereo, glide);
}

// Once per transition, in its first sub-block (called from the coefficient
// block in renderBlock, right after the xf bank has been configured at this
// sub-block's target cutoff/resonance/type/slope/mix/drive): run the recent
// filter-input history through the target configuration so it starts settled
// instead of ringing up from zero when the fade makes it audible. With no
// history yet (a note from idle, or right after prepare/reset —
// noteOn/prepare/reset all zero filterHistCount_) there is nothing to settle
// on, and the target starts from zero state, as it does past the deadline.
//
// deadlineTicks is the block's pre-roll deadline
// (BlockParams::filterPreRollDeadlineTicks). A pre-roll that would start
// after it does not run, and one that reaches it stops after the chunk in
// flight. Either way the target starts from zero state: never from a state
// settled on only the older part of the history, which would then continue
// on the current input and join two points of the signal with nothing in
// between, and never from the stale state an idle stage of the live filter
// left in the shared-kind copy.
void SynthVoice::preRollFilterTransition (bool stereo, float driveGain, long long deadlineTicks)
{
    const int cap = static_cast<int>(filterHistL_.size());
    const int oldestIdx = ((filterHistPos_ - filterHistCount_) + cap) % cap;
    auto pastDeadline = [deadlineTicks] { return juce::Time::getHighResolutionTicks() >= deadlineTicks; };

    float bufL[SUB_BLOCK_SIZE];
    float bufR[SUB_BLOCK_SIZE];

    if (! filterXfShared_)
    {
        // Separate kind: the xf bank runs its OWN full path (drive stage +
        // filter stages, oversampled where the model calls for it) over the
        // recent input, oldest sample first. This also primes the xf
        // oversampler the new path runs through for real once the fade
        // starts — beginFilterTransition reset it to empty right before this,
        // which is also the zero state a skipped pre-roll leaves.
        if (pastDeadline())
            return;
        int readPos = oldestIdx;
        int remaining = filterHistCount_;
        while (remaining > 0)
        {
            const int len = std::min(remaining, SUB_BLOCK_SIZE);
            for (int j = 0; j < len; ++j)
            {
                const int idx = (readPos + j) % cap;
                bufL[j] = filterHistL_[static_cast<size_t>(idx)];
                bufR[j] = filterHistR_[static_cast<size_t>(idx)];
            }
            processFilterPath(xfBank(), xfFilterCfg_, bufL, bufR, len, stereo, driveGain);
            readPos = (readPos + len) % cap;
            remaining -= len;
            if (remaining > 0 && pastDeadline())
            {
                // Stopped: back to the zero state beginFilterTransition set.
                switch (xfFilterCfg_.algorithm)
                {
                    case FilterAlgorithm::SVF:    xfFilter_.reset(); xfFilterR_.reset(); break;
                    case FilterAlgorithm::Ladder: xfLadder_.reset(); xfLadderR_.reset(); break;
                    case FilterAlgorithm::Warp:   xfWarp_.reset();   xfWarpR_.reset();   break;
                }
                xfOs2x_->reset();
                xfOs4x_->reset();
                xfOs8x_->reset();
                return;
            }
        }
    }
    else if (xfFilterCfg_.algorithm == FilterAlgorithm::SVF)
    {
        // Shared kind, SVF: the state beginFilterTransition copied from the
        // live filter is exact for the stages whose input the slope/type
        // change leaves alone (keep1/keep2/keepOP below) — the others need
        // settled state.
        //
        // Same dispatch as T5ynthFilter::processSample (and
        // juce::dsp::StateVariableTPTFilter): filter1's input is always the
        // post-drive signal and its TPT state is type-independent, so a stage
        // the LIVE config already ran (keep1) stays valid state for any target
        // type/slope. filter2 (24 dB) and the one-pole instead read another
        // stage's TYPED output, so keeping their state also needs that
        // upstream typing to match.
        auto lpOrHp = [] (int t) { return t == 0 || t == 1; };
        auto runs1  = [&lpOrHp] (const FilterCfg& c) { return ! (c.slope == 0 && lpOrHp(c.type)); };
        auto runs2  = [] (const FilterCfg& c) { return c.slope == 3; };
        auto runsOP = [&lpOrHp] (const FilterCfg& c) { return lpOrHp(c.type) && (c.slope == 0 || c.slope == 2); };

        const FilterCfg& L = liveFilterCfg_;
        const FilterCfg& X = xfFilterCfg_;
        const bool keep1  = runs1(L);                        // input always the post-drive signal; TPT state is type-independent
        const bool keep2  = runs2(L) && L.type == X.type;     // input = filter1's output of the type
        const bool keepOP = runsOP(L) && L.slope == X.slope
                          && (X.slope == 0 || L.type == X.type);   // 6 dB: post-drive input (LP and HP both); 18 dB: filter1's output of the type
        const bool take1  = runs1(X)  && ! keep1;
        const bool take2  = runs2(X)  && ! keep2;
        const bool takeOP = runsOP(X) && ! keepOP;

        // A change that brings no stage in (a type change at 12 dB, 18 or
        // 24 dB -> 12 dB, 6 dB LP <-> HP) finds the copy exact as it stands,
        // and a pre-roll would be thrown away. Otherwise settle the stages to
        // take by running the SAME history through a scratch instance
        // configured at the TARGET coefficients (this sub-block's
        // configureFilterBank has already run on xfFilter_/xfFilterR_, so
        // copying from them picks those up).
        if (! (take1 || take2 || takeOP))
            return;

        xfPreRollSvf_  = xfFilter_;
        xfPreRollSvfR_ = xfFilterR_;
        xfPreRollSvf_.reset();
        xfPreRollSvfR_.reset();

        // The stages taken below continue on the LIVE drive stage's output,
        // so they must be settled on that same signal, its oversampler's
        // latency included: a base-rate tanh would leave them a few samples
        // ahead of their input, which rings at the cutoff like any other
        // state error. So the history goes through the xf bank's own drive
        // stage, which is the live one's configuration (the shared kind
        // requires it). Its oversamplers are free here: the shared kind runs
        // the fade on the live bank's and completeFilterTransition leaves
        // them unswapped. Reset first, so their few samples of memory come
        // from this history and not from whatever last ran through them.
        // Past the deadline the scratch stays at its reset state, and the
        // stages taken below enter from zero.
        if (! pastDeadline())
        {
            if (xfFilterCfg_.svfDrive)
            {
                xfOs2x_->reset();
                xfOs4x_->reset();
                xfOs8x_->reset();
            }

            int readPos = oldestIdx;
            int remaining = filterHistCount_;
            while (remaining > 0)
            {
                const int len = std::min(remaining, SUB_BLOCK_SIZE);
                for (int j = 0; j < len; ++j)
                {
                    const int idx = (readPos + j) % cap;
                    bufL[j] = filterHistL_[static_cast<size_t>(idx)];
                    bufR[j] = filterHistR_[static_cast<size_t>(idx)];
                }
                processDriveStage(xfBank(), xfFilterCfg_, bufL, bufR, len, driveGain);
                for (int j = 0; j < len; ++j)
                    bufL[j] = xfPreRollSvf_.processSample(bufL[j]);
                if (stereo)
                    for (int j = 0; j < len; ++j)
                        bufR[j] = xfPreRollSvfR_.processSample(bufR[j]);

                readPos = (readPos + len) % cap;
                remaining -= len;
                if (remaining > 0 && pastDeadline())
                {
                    xfPreRollSvf_.reset();
                    xfPreRollSvfR_.reset();
                    break;
                }
            }
        }

        xfFilter_.takeStagesFrom(xfPreRollSvf_, take1, take2, takeOP);
        stereo ? xfFilterR_.takeStagesFrom(xfPreRollSvfR_, take1, take2, takeOP) : (void) (xfFilterR_ = xfFilter_);
    }
    // Shared kind, Ladder/Warp: never pending (see beginFilterTransition) —
    // slope/type/style only select the output tap, so the state evolution
    // does not depend on them and the plain copy made there is already exact.
}

void SynthVoice::beginFilterTransition (const FilterCfg& target)
{
    const FilterCfg& live = liveFilterCfg_;
    filterXfShared_ = live.enabled && target.enabled && live.algorithm == target.algorithm
        && (target.algorithm == FilterAlgorithm::SVF
              ? (live.svfDrive == target.svfDrive && live.svfDriveOs == target.svfDriveOs)
              : (live.nlOs == target.nlOs));

    xfFilterCfg_ = target;

    if (target.enabled)
    {
        const bool nl = target.algorithm != FilterAlgorithm::SVF;
        if (nl && xfPreparedOsFactor_ != target.nlOs)
            prepareNonlinearAt(xfBank(), target.nlOs);

        if (filterXfShared_)
        {
            // Same model, same rate, same drive stage: the xf model state
            // starts as an exact, UNTRIMMED copy of the live one — a stage
            // the live slope/type left idle may be exactly what the TARGET
            // needs, and the pre-roll (preRollFilterTransition) is what
            // settles it, not a zeroing here. The xf bank's oversamplers are
            // neither reset nor used: both configurations run through the
            // LIVE bank's drive stage and oversampler for the whole
            // transition (processFilterPathShared).
            switch (target.algorithm)
            {
                case FilterAlgorithm::SVF:
                    xfFilter_ = filter;
                    xfFilterR_ = filterR;
                    filterXfPreRollPending_ = true;
                    break;
                case FilterAlgorithm::Ladder:
                    // Slope/type only select the output TAP
                    // (LadderFilter::tapOutput) — the state evolution itself
                    // does not depend on them, so the copy above is already
                    // exact. No pre-roll.
                    xfLadder_ = filterLadder;
                    xfLadderR_ = filterLadderR;
                    break;
                case FilterAlgorithm::Warp:
                    // Same as Ladder; a style change carries the state into
                    // the new curve, which is what switching styles on a
                    // running circuit does. No pre-roll.
                    xfWarp_ = filterWarp;
                    xfWarpR_ = filterWarpR;
                    break;
            }
        }
        else
        {
            // A different model, rate or drive stage has no state in common
            // with what is live: start at zero rather than from whatever
            // this voice's xf bank held from an earlier, unrelated
            // transition, and let the pre-roll settle it from the recent
            // input instead.
            switch (target.algorithm)
            {
                case FilterAlgorithm::SVF:    xfFilter_.reset(); xfFilterR_.reset(); break;
                case FilterAlgorithm::Ladder: xfLadder_.reset(); xfLadderR_.reset(); break;
                case FilterAlgorithm::Warp:   xfWarp_.reset();   xfWarpR_.reset();   break;
            }

            // The transition bank's oversamplers hold whatever last ran
            // through them — NOT "never the live bank's history": after a
            // completion the swapped-in instances held exactly that. They
            // may well have carried live signal once, just not THIS
            // transition's recent past, which is what the pre-roll below
            // fills them with instead.
            xfOs2x_->reset();
            xfOs4x_->reset();
            xfOs8x_->reset();

            filterXfPreRollPending_ = true;
        }
    }
    // Filter off (target disabled): the identity has no state to copy,
    // reset, or pre-roll.

    filterXf_ = FilterXf::Fade;
    filterXfFadeTotal_ = std::max(1, juce::roundToInt(FILTER_XF_FADE_MS * 0.001 * sr));
    filterXfLeft_ = filterXfFadeTotal_;
}

void SynthVoice::completeFilterTransition()
{
    if (xfFilterCfg_.enabled)
    {
        if (xfFilterCfg_.algorithm == FilterAlgorithm::SVF)
        {
            filter  = xfFilter_;
            filterR = xfFilterR_;
        }
        else
        {
            // Both nonlinear filters copy across even though only one is the
            // active algorithm — cheap. The real reason: filterPreparedOsFactor_
            // below declares the rate of ALL FOUR live nonlinear filters, and
            // the adopt path trusts it — copying only the active model would
            // leave the idle one at another rate than declared, and a later
            // adopt into it would skip its re-prepare and run it an octave
            // off. (Not, as this comment used to say, to keep a later
            // transition's shared branch from reading the idle model as
            // sounding state — that branch requires the live algorithm to
            // equal the target, so the idle model is never read as sounding
            // state regardless.)
            filterLadder  = xfLadder_;
            filterLadderR = xfLadderR_;
            filterWarp    = xfWarp_;
            filterWarpR   = xfWarpR_;
            filterPreparedOsFactor_ = xfPreparedOsFactor_;   // all four live nonlinear filters now at that factor
        }
    }

    // The separate kind's xf oversamplers actually ran this signal's recent
    // past (the pre-roll, then the fade) and become the live ones. The
    // shared kind never touched them at all — both models ran through the
    // LIVE bank's drive stage/oversampler for the whole transition
    // (processFilterPathShared) — so swapping here would hand the next live
    // bank a stale or reset instance in place of the one that actually
    // carries the signal. Only the unique_ptrs move; the Oversampling
    // objects themselves never do (no heap traffic).
    if (! filterXfShared_)
    {
        std::swap(driveOs2x_, xfOs2x_);
        std::swap(driveOs4x_, xfOs4x_);
        std::swap(driveOs8x_, xfOs8x_);
    }

    liveFilterCfg_ = xfFilterCfg_;
    filterXf_ = FilterXf::None;
    filterXfShared_ = false;
}

void SynthVoice::processFilterPathShared (float* L, float* R, int n, bool stereo, float driveGain,
                                          const CutoffGlide* glide)
{
    // Same model, same rate, same drive stage (filterXfShared_, decided in
    // beginFilterTransition): running two independent drive stages and two
    // independent oversamplers here would double CPU for nothing the two
    // configurations don't already share, and would leave two independent
    // phase responses fighting each other for the length of the fade. One
    // drive stage, one oversampler, both models fed from the SAME
    // upsampled/driven signal — only the filter stages themselves (the SVF/
    // Ladder/Warp model state) differ between live and xf.
    auto live = liveBank();
    auto xf = xfBank();
    const int osf = (liveFilterCfg_.algorithm != FilterAlgorithm::SVF) ? live.preparedOs : 1;
    const int done = filterXfFadeTotal_ - filterXfLeft_;
    // Both banks glide alike (CutoffGlide): the same cutoff, per base-rate sample.
    const auto glideAt = [&] (FilterBank b, const FilterCfg& c, size_t i, bool left, bool right)
    {
        if (glide != nullptr && i % static_cast<size_t>(osf) == 0)
            glideCutoff(b, c, *glide, static_cast<int>(i / static_cast<size_t>(osf)), left, right);
    };

    if (osf > 1)   // Ladder/Warp at 2x/4x: one oversampler, both models inside it
    {
        auto* os = (osf == 2) ? live.os2 : live.os4;
        float* const channels[2] = { L, R };
        juce::dsp::AudioBlock<float> block(channels, 2, static_cast<size_t>(n));
        juce::dsp::AudioBlock<const float> constBlock(block);
        auto upBlock = os->processSamplesUp(constBlock);
        const size_t upN = upBlock.getNumSamples();
        auto* up0 = upBlock.getChannelPointer(0);
        auto* up1 = upBlock.getChannelPointer(1);

        float xUp0[SUB_BLOCK_SIZE * 4];
        float xUp1[SUB_BLOCK_SIZE * 4];
        std::copy(up0, up0 + upN, xUp0);
        std::copy(up1, up1 + upN, xUp1);

        if (liveFilterCfg_.algorithm == FilterAlgorithm::Ladder)
        {
            for (size_t i = 0; i < upN; ++i) { glideAt(live, liveFilterCfg_, i, true, false); up0[i]  = live.ladder.processSample(up0[i]); }
            for (size_t i = 0; i < upN; ++i) { glideAt(xf,   xfFilterCfg_,   i, true, false); xUp0[i] = xf.ladder.processSample(xUp0[i]); }
            if (stereo)
            {
                for (size_t i = 0; i < upN; ++i) { glideAt(live, liveFilterCfg_, i, false, true); up1[i]  = live.ladderR.processSample(up1[i]); }
                for (size_t i = 0; i < upN; ++i) { glideAt(xf,   xfFilterCfg_,   i, false, true); xUp1[i] = xf.ladderR.processSample(xUp1[i]); }
            }
        }
        else // Warp
        {
            for (size_t i = 0; i < upN; ++i) { glideAt(live, liveFilterCfg_, i, true, false); up0[i]  = live.warp.processSample(up0[i]); }
            for (size_t i = 0; i < upN; ++i) { glideAt(xf,   xfFilterCfg_,   i, true, false); xUp0[i] = xf.warp.processSample(xUp0[i]); }
            if (stereo)
            {
                for (size_t i = 0; i < upN; ++i) { glideAt(live, liveFilterCfg_, i, false, true); up1[i]  = live.warpR.processSample(up1[i]); }
                for (size_t i = 0; i < upN; ++i) { glideAt(xf,   xfFilterCfg_,   i, false, true); xUp1[i] = xf.warpR.processSample(xUp1[i]); }
            }
        }

        if (filterXf_ == FilterXf::Fade)
        {
            // Raised cosine, summing to one: both paths filter the same input, so they are
            // correlated and an equal-POWER law would bump the level mid-fade. Time base is
            // OUTPUT (base-rate) samples: upsampled sample i is (i+1)/osf of one output sample.
            for (size_t i = 0; i < upN; ++i)
            {
                const float t = juce::jlimit(0.0f, 1.0f,
                    (static_cast<float>(done) + (static_cast<float>(i) + 1.0f) / static_cast<float>(osf))
                        / static_cast<float>(filterXfFadeTotal_));
                const float g = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * t);
                up0[i] += (xUp0[i] - up0[i]) * g;
                if (stereo)
                    up1[i] += (xUp1[i] - up1[i]) * g;
            }
        }

        os->processSamplesDown(block);

        if (! stereo)   // mono sync, both banks — matches processFilterStages' own
        {
            if (liveFilterCfg_.algorithm == FilterAlgorithm::Ladder) { live.ladderR = live.ladder; xf.ladderR = xf.ladder; }
            else                                                     { live.warpR   = live.warp;   xf.warpR   = xf.warp;   }
        }
    }
    else           // SVF (with or without its drive stage), and Ladder/Warp at base rate
    {
        processDriveStage(live, liveFilterCfg_, L, R, n, driveGain);   // identical for both: same drive stage

        float xL[SUB_BLOCK_SIZE];
        float xR[SUB_BLOCK_SIZE];
        std::copy(L, L + n, xL);
        std::copy(R, R + n, xR);

        processFilterStages(live, liveFilterCfg_, L, R, n, stereo, glide);
        processFilterStages(xf,   xfFilterCfg_,   xL, xR, n, stereo, glide);

        if (filterXf_ == FilterXf::Fade)
        {
            // Raised cosine, summing to one: both paths filter the same input, so they are
            // correlated and an equal-POWER law would bump the level mid-fade.
            for (int j = 0; j < n; ++j)
            {
                const float t = juce::jlimit(0.0f, 1.0f,
                    static_cast<float>(done + j + 1) / static_cast<float>(filterXfFadeTotal_));
                const float g = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * t);
                L[j] += (xL[j] - L[j]) * g;
                R[j] += (xR[j] - R[j]) * g;
            }
        }
    }

    filterXfLeft_ -= n;
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

    // Notice a routing/amount change against the running ramp goal and arm a
    // new one before anything below reads envRouteW_/envAmtDelta_ — the
    // sampler-mode block-rate path just below is the first reader.
    updateEnvRouteGoals(p);

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
    // For standard MIDI the per-voice bend stays 0, so this is a no-op - and the
    // accessor, not the field, because whether X reaches the pitch at all is the
    // Pitch row's wiring (SynthVoice::setXBendsPitch).
    const float effectivePitchRatio = p.performancePitchRatio
        * std::pow(2.0f, getPerVoicePitchBend() / 12.0f);

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
            computeEffectiveLfoDepthRouted(envRouteW_, EnvTarget::LFO1Depth, p.lfo1Depth,
                                           lastAmpEnvLevelSm_, lastModValSm_), expr);
        const float lfo2Depth = applyAftertouchTarget(p, AftertouchTarget::LFO2Depth,
            computeEffectiveLfoDepthRouted(envRouteW_, EnvTarget::LFO2Depth, p.lfo2Depth,
                                           lastAmpEnvLevelSm_, lastModValSm_), expr);
        const float lfo3Depth = applyAftertouchTarget(p, AftertouchTarget::LFO3Depth,
            computeEffectiveLfoDepthRouted(envRouteW_, EnvTarget::LFO3Depth, p.lfo3Depth,
                                           lastAmpEnvLevelSm_, lastModValSm_), expr);
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

    // ── Filter configuration: adopt / transition / continue ──
    // A discrete change (algorithm/slope/type/style/on-off/drive-stage/OS
    // factor) is never applied in place on the live filters — see FilterCfg
    // and the xf* members in SynthVoice.h. Ladder/Warp's prepare() at
    // sr×factor (their coefficient g = tan(π·fc/sr) is derived from the
    // internal rate, so it must happen BEFORE the sub-block setCutoff calls
    // below) now lives in prepareNonlinearAt — allocation-free (sr + reset +
    // updateCoeffs) → audio-thread safe, called only on an actual factor
    // change, never per block. SVF is linear and never oversampled; factor
    // 1 = Off = base rate.
    const FilterCfg want = filterCfgFrom(p);
    if (filterCfgAdopt_)
    {
        // Nothing has been heard through this voice's filter yet (a note from idle, or right
        // after prepare/reset): take the configuration outright, no transition.
        if (want != liveFilterCfg_ && want.enabled)
        {
            // A model or stage that was not the live one starts from zero, not from whatever it
            // held the last time this voice used it.
            switch (want.algorithm)
            {
                case FilterAlgorithm::SVF:    filter.reset();       filterR.reset();       break;
                case FilterAlgorithm::Ladder: filterLadder.reset(); filterLadderR.reset(); break;
                case FilterAlgorithm::Warp:   filterWarp.reset();   filterWarpR.reset();   break;
            }
            driveOs2x_->reset();
            driveOs4x_->reset();
            driveOs8x_->reset();
        }
        if (want.enabled && want.algorithm != FilterAlgorithm::SVF && filterPreparedOsFactor_ != want.nlOs)
            prepareNonlinearAt(liveBank(), want.nlOs);
        liveFilterCfg_ = want;
        filterXf_ = FilterXf::None;
        filterCfgAdopt_ = false;
        filterXfPreRollPending_ = false;
        filterXfShared_ = false;
    }
    else
    {
        // A fade that ended on the last call's final sample is completed here, not in the first
        // sub-block below, so a target this call brings starts with this call.
        if (filterXf_ == FilterXf::Fade && filterXfLeft_ <= 0)
            completeFilterTransition();
        if (filterXf_ == FilterXf::None && want != liveFilterCfg_)
            beginFilterTransition(want);
    }
    // A fade still running: a newer target waits for it to end and starts at the first sub-block
    // boundary after that (the phase advance below), in this call or at the top of the next: at
    // most FILTER_XF_FADE_MS plus 31 samples after the voice first sees it, 1.6 ms at 48 kHz.

    // Cutoff-bus depth curve for the sources whose depth is block-constant. The
    // curve is a pow(); these five values cannot change inside the block (drift
    // and LFO modulation of an env Amt already landed in `p` before the voice ran),
    // so they are resolved once per block instead of once per 32 samples. The
    // three LFO depths genuinely vary within the block — an env can be driving
    // them — and stay at the sub-block boundary below.
    float ampCutoffCurve = 0.0f, modCutoffCurve[kNumModEnvs] = {}, atCutoffCurve = 0.0f;
    // want too: a transition the phase advance starts mid-call goes into it.
    const bool anyFilterPath = liveFilterCfg_.enabled || want.enabled
        || (filterXf_ != FilterXf::None && xfFilterCfg_.enabled);
    if (anyFilterPath)
    {
        // Only the settled cutoff bus reads these, and settled, the routing
        // weights are exactly p's targets (a ramp ends on the goal
        // updateEnvRouteGoals took from this block's p); mid-ramp the bus
        // evaluates the curve itself.
        if (p.ampTarget == EnvTarget::Filter)
            ampCutoffCurve = ModCalib::cutoffDepthCurve(p.ampAmount);
        for (int m = 0; m < kNumModEnvs; ++m)
            if (p.modEnv[m].target == EnvTarget::Filter)
                modCutoffCurve[m] = ModCalib::cutoffDepthCurve(p.modEnv[m].amount);
        atCutoffCurve = ModCalib::cutoffDepthCurve(p.aftertouchTargetAmt[AftertouchTarget::Cutoff]);
    }

    // The cutoff the filter runs at, from the voice's envelope and routing
    // state as it stands when it is asked: at each sub-block boundary for the
    // coefficients, and, while a routing/amount ramp is in flight, once more
    // after Phase A for where the ramp has taken it by the sub-block's end
    // (CutoffGlide in SynthVoice.h).
    const auto modulatedCutoffHz = [&] (int midIdx) -> float
    {
        const float lfo1Depth = applyAftertouchTarget(p, AftertouchTarget::LFO1Depth,
            computeEffectiveLfoDepthRouted(envRouteW_, EnvTarget::LFO1Depth, p.lfo1Depth,
                                           lastAmpEnvLevelSm_, lastModValSm_), expr);
        const float lfo2Depth = applyAftertouchTarget(p, AftertouchTarget::LFO2Depth,
            computeEffectiveLfoDepthRouted(envRouteW_, EnvTarget::LFO2Depth, p.lfo2Depth,
                                           lastAmpEnvLevelSm_, lastModValSm_), expr);
        const float lfo3Depth = applyAftertouchTarget(p, AftertouchTarget::LFO3Depth,
            computeEffectiveLfoDepthRouted(envRouteW_, EnvTarget::LFO3Depth, p.lfo3Depth,
                                           lastAmpEnvLevelSm_, lastModValSm_), expr);
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
        // Settled, the weights are exactly 0 or 1 and this is the envelope
        // routed to the filter times its block-constant curve. Mid-ramp the
        // weight is a crossfade fraction and the curve follows the ramped
        // amount (envAmtEff).
        const bool routeSettled = envRouteRampLeft_ == 0;
        if (! juce::exactlyEqual(envRouteW_[0][EnvTarget::Filter], 0.0f))
            cutoffOctaves += envRouteW_[0][EnvTarget::Filter] * lastAmpEnvLevelSm_
                * (routeSettled ? ampCutoffCurve : ModCalib::cutoffDepthCurve(envAmtEff(p, 0)));
        for (int m = 0; m < kNumModEnvs; ++m)
            if (! juce::exactlyEqual(envRouteW_[m + 1][EnvTarget::Filter], 0.0f))
                cutoffOctaves += envRouteW_[m + 1][EnvTarget::Filter] * lastModValSm_[m]
                    * (routeSettled ? modCutoffCurve[m] : ModCalib::cutoffDepthCurve(envAmtEff(p, m + 1)));
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
        return juce::jlimit(20.0f, 20000.0f, cutoffMod);
    };

    int pos = 0;
    while (pos < numSamples && active)
    {
        int subBlockEnd = std::min(pos + SUB_BLOCK_SIZE, numSamples);
        int subBlockLen = subBlockEnd - pos;

        // ── Filter transition: advance phase ──
        // Runs before the coefficient block below so a completion landing in
        // THIS sub-block is reflected in subFilterPath immediately, instead
        // of running the now-promoted live bank through one more sub-block
        // of stale xf-bank coefficients.
        if (filterXf_ == FilterXf::Fade && filterXfLeft_ <= 0)
        {
            completeFilterTransition();
            // A target that arrived while that fade ran starts here, in this call.
            if (want != liveFilterCfg_)
                beginFilterTransition(want);
        }

        // ── Sub-block boundary: update filter coefficients ONCE ──
        const bool subFilterPath = liveFilterCfg_.enabled
            || (filterXf_ != FilterXf::None && xfFilterCfg_.enabled);
        // A routing/amount ramp in flight moves the cutoff across the sub-block
        // (CutoffGlide), not only at its boundaries.
        const bool cutoffGlides = subFilterPath && envRouteRampLeft_ > 0;
        // A sub-block that glided owes an exact landing once it stops: the
        // glide's last step is one increment short of the true end value, and
        // the model's own dead-band can otherwise swallow every later push of
        // that same value (see setCutoff's force parameter).
        const bool landExact = filterGlideOwed_ && ! cutoffGlides;
        const int midIdx = pos + subBlockLen / 2;
        float cutoffMod = 0.0f;
        if (subFilterPath)
        {
            cutoffMod = modulatedCutoffHz(midIdx);
            const float resonanceMod = applyAftertouchTarget(
                p, AftertouchTarget::Resonance, p.baseReso, expr);
            lastModulatedCutoff_ = cutoffMod;
            lastModulatedResonance_ = resonanceMod;

            // Each bank is configured with its OWN discrete config (never
            // p's directly) — the live bank keeps running whatever it was
            // switched to, and the xf bank (while a transition is in flight)
            // runs the newest target; only the continuous cutoff/reso/mix/
            // drive values track p every sub-block, for both.
            if (liveFilterCfg_.enabled)
                configureFilterBank(liveBank(), liveFilterCfg_, cutoffMod, resonanceMod, p, landExact);
            if (filterXf_ != FilterXf::None && xfFilterCfg_.enabled)
            {
                configureFilterBank(xfBank(), xfFilterCfg_, cutoffMod, resonanceMod, p, landExact);
                // Once per transition, in its first sub-block: settle the xf
                // bank's target configuration on the recent filter input
                // before the fade makes it audible, instead of starting it
                // from zero.
                // Past the block's pre-roll deadline it starts from zero state
                // instead (see preRollFilterTransition).
                if (filterXfPreRollPending_)
                {
                    preRollFilterTransition(freezeMode, p.filterDriveGain, p.filterPreRollDeadlineTicks);
                    filterXfPreRollPending_ = false;
                }
            }
            if (landExact)
                filterGlideOwed_ = false;
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
            // Advance the routing/amount ramp once per sample, before anything
            // below reads envRouteW_ / envAmtDelta_ (see updateEnvRouteGoals, which
            // armed it, at the top of renderBlock).
            advanceEnvRoute();
            const bool routeSettled = envRouteRampLeft_ == 0;
            const float ampContour = ampEnv.processSample();
            float ampEnvVal = ampContour * p.ampAmount;
            // The key's own control voltage, advanced every sample next to the
            // envelopes — unconditionally, so the ramp stays sample-locked no
            // matter which branch of computeDcaGain reads it this block.
            keyGate_.setTargetValue(noteHeld ? 1.0f : 0.0f);
            const float keyGate = keyGate_.getNextValue();
            float modContour[kNumModEnvs];
            float modEnvVals[kNumModEnvs];
            for (int m = 0; m < kNumModEnvs; ++m)
            {
                modContour[m] = modEnvs[m].processSample();
                modEnvVals[m] = modContour[m] * p.modEnv[m].amount;
            }
            // De-zippered twins of the two values above, for the destinations this
            // section ramps (DCA / Filter / NoiseLevel / the three LFO depths).
            // Settled, these equal the raw values above exactly, so every routed
            // reader below computes what the unrouted one did; mid-ramp, the SAME
            // contour is instead scaled by the amount on its way from the one the
            // voice was using to p's (envAmtEff) — the step this whole section
            // exists to remove.
            float ampEnvValSm = ampEnvVal;
            float modEnvValsSm[kNumModEnvs];
            for (int m = 0; m < kNumModEnvs; ++m)
                modEnvValsSm[m] = modEnvVals[m];
            if (! routeSettled)
            {
                ampEnvValSm = ampContour * envAmtEff(p, 0);
                for (int m = 0; m < kNumModEnvs; ++m)
                    modEnvValsSm[m] = modContour[m] * envAmtEff(p, m + 1);
            }
            lastAmpEnvLevel = ampEnvVal;
            for (int m = 0; m < kNumModEnvs; ++m)
                lastModVal_[m] = modEnvVals[m];
            lastAmpEnvLevelSm_ = ampEnvValSm;
            for (int m = 0; m < kNumModEnvs; ++m)
                lastModValSm_[m] = modEnvValsSm[m];

            const float lfo1Depth = applyAftertouchTarget(p, AftertouchTarget::LFO1Depth,
                computeEffectiveLfoDepthRouted(envRouteW_, EnvTarget::LFO1Depth, p.lfo1Depth,
                                               ampEnvValSm, modEnvValsSm), expr);
            const float lfo2Depth = applyAftertouchTarget(p, AftertouchTarget::LFO2Depth,
                computeEffectiveLfoDepthRouted(envRouteW_, EnvTarget::LFO2Depth, p.lfo2Depth,
                                               ampEnvValSm, modEnvValsSm), expr);
            const float lfo3Depth = applyAftertouchTarget(p, AftertouchTarget::LFO3Depth,
                computeEffectiveLfoDepthRouted(envRouteW_, EnvTarget::LFO3Depth, p.lfo3Depth,
                                               ampEnvValSm, modEnvValsSm), expr);
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
            if (! juce::exactlyEqual(envRouteW_[0][EnvTarget::NoiseLevel], 0.0f))
                noiseLevel += envRouteW_[0][EnvTarget::NoiseLevel] * ampEnvValSm;
            for (int m = 0; m < kNumModEnvs; ++m)
                if (! juce::exactlyEqual(envRouteW_[m + 1][EnvTarget::NoiseLevel], 0.0f))
                    noiseLevel += envRouteW_[m + 1][EnvTarget::NoiseLevel] * modEnvValsSm[m];
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
            float vca = computeDcaGainRouted(envRouteW_, ampEnvValSm, modEnvValsSm, keyGate);
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
            // Settled, dcaW is exactly 1.0f when the amp envelope is on the DCA and
            // exactly 0.0f otherwise, so this is the two-way test above; mid-ramp
            // it waits for BOTH the envelope and the key, since either arm's share
            // of the crossfade may still be holding the voice open.
            const float dcaW = envRouteW_[0][EnvTarget::DCA];
            const bool levelDone = dcaW >= 1.0f ? ampEnv.isIdle()
                                 : dcaW <= 0.0f ? ! keyGate_.isSmoothing()
                                 : (ampEnv.isIdle() && ! keyGate_.isSmoothing());
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

        // ── Filter-input history (pre-roll source) ──
        // Ring buffer of the DRY signal any filter bank would see, written
        // unconditionally — also while the filter is off, so a "filter on"
        // transition has something to pre-roll from too. Must run before
        // anything below processes this sub-block's dry signal (the xf copy,
        // and the live filter path itself, which mutates output/outputRBuf
        // in place).
        if (driveLen > 0 && ! filterHistL_.empty())
        {
            const int cap = static_cast<int>(filterHistL_.size());
            const int histN = std::min(driveLen, cap);
            const int srcOff = driveLen - histN;   // pathological sr only: keep the newest `cap` samples
            const int firstLen = std::min(histN, cap - filterHistPos_);
            std::copy(output + pos + srcOff, output + pos + srcOff + firstLen, filterHistL_.begin() + filterHistPos_);
            std::copy(outputRBuf + srcOff, outputRBuf + srcOff + firstLen, filterHistR_.begin() + filterHistPos_);
            if (histN > firstLen)
            {
                std::copy(output + pos + srcOff + firstLen, output + pos + srcOff + histN, filterHistL_.begin());
                std::copy(outputRBuf + srcOff + firstLen, outputRBuf + srcOff + histN, filterHistR_.begin());
            }
            filterHistPos_ = (filterHistPos_ + histN) % cap;
            filterHistCount_ = std::min(cap, filterHistCount_ + histN);
        }

        // ── Filter path: live bank always, transition bank while in flight ──
        // The live bank runs its own (possibly-just-switched) configuration
        // on the real output/outputRBuf, exactly as the single-bank code did.
        // While a transition is running, the SAME dry input also goes through
        // the xf bank too, crossfaded in over the fade — either sharing the
        // live bank's drive stage and oversampler (processFilterPathShared,
        // for a same-model/same-rate/same-drive-stage transition) or running
        // its own full path (everything else). filterXfPreRollPending_,
        // consumed above in the coefficient block, is what keeps that xf path
        // from starting at zero: it has already run the recent history
        // through whichever of those two paths applies, in this transition's
        // first sub-block, before any of this was heard.
        // See FilterCfg / beginFilterTransition / completeFilterTransition in
        // SynthVoice.h for why a discrete switch is never applied in place.
        // Where a ramp in flight has taken the cutoff bus by the end of this
        // sub-block (Phase A advanced it): the filter stages glide there from
        // cutoffMod instead of stepping at the next boundary.
        CutoffGlide glide;
        const bool glides = cutoffGlides && driveLen > 0;
        if (glides)
        {
            glide.fromHz = cutoffMod;
            glide.log2Ratio = std::log2(modulatedCutoffHz(midIdx) / cutoffMod);
            glide.n = driveLen;

            filterGlideOwed_ = true;
        }
        const CutoffGlide* const glideIn = glides ? &glide : nullptr;

        const bool xfRun = filterXf_ != FilterXf::None && driveLen > 0;
        if (xfRun && filterXfShared_)
        {
            processFilterPathShared(output + pos, outputRBuf, driveLen, freezeMode, p.filterDriveGain, glideIn);
        }
        else
        {
            float xfL[SUB_BLOCK_SIZE] {};
            float xfR[SUB_BLOCK_SIZE] {};
            if (xfRun)
            {
                std::copy(output + pos, output + pos + driveLen, xfL);
                std::copy(outputRBuf, outputRBuf + driveLen, xfR);
            }
            processFilterPath(liveBank(), liveFilterCfg_, output + pos, outputRBuf, driveLen, freezeMode, p.filterDriveGain, glideIn);
            if (xfRun)
            {
                processFilterPath(xfBank(), xfFilterCfg_, xfL, xfR, driveLen, freezeMode, p.filterDriveGain, glideIn);
                if (filterXf_ == FilterXf::Fade)
                {
                    // Raised cosine, summing to one: both paths filter the same input, so they are
                    // correlated and an equal-POWER law would bump the level mid-fade.
                    const int done = filterXfFadeTotal_ - filterXfLeft_;
                    for (int j = 0; j < driveLen; ++j)
                    {
                        const float t = juce::jlimit(0.0f, 1.0f,
                            static_cast<float>(done + j + 1) / static_cast<float>(filterXfFadeTotal_));
                        const float g = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::pi * t);
                        output[pos + j] += (xfL[j] - output[pos + j]) * g;
                        outputRBuf[j]   += (xfR[j] - outputRBuf[j]) * g;
                    }
                }
                filterXfLeft_ -= driveLen;
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

    // Where the next routing/amount ramp starts (updateEnvRouteGoals): the
    // amounts this call's last sample used.
    for (int e = 0; e < kNumEnvSources; ++e)
        envAmtLast_[e] = envAmtEff(p, e);
}
