#pragma once
#include "WavetableOscillator.h"
#include "SamplePlayer.h"
#include "FreezeTextureEngine.h"
#include "ADSREnvelope.h"
#include "LFO.h"
#include "StateVariableFilter.h"
#include "LadderFilter.h"
#include "CutoffWarpFilter.h"
#include "NoiseGenerator.h"
#include "BlockParams.h"
#include <juce_dsp/juce_dsp.h>
#include <memory>
#include <vector>

/**
 * Single synthesizer voice — owns all per-voice DSP state:
 * oscillator, sample player, envelopes, per-voice LFOs, and filter.
 *
 * Signal chain: Osc/Noise → Drive (tanh, optional) → Filter (SVF) → VCA → output
 */
class SynthVoice
{
public:
    SynthVoice() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();

    // ── Note lifecycle ──
    void noteOn(int note, float velocity, bool legato);
    void noteOff();
    void glideToNote(int note, float glideMs);
    // Csound voice bridge (Phase-1 spec §3, D7): advances the block-rate
    // csoundFreq_ smoother by samplesToAdvance (the number of samples rendered
    // since the previous channel write — VoiceManager passes renderPos delta)
    // and returns the resulting current value: the raw smoothed base Hz, no
    // performance-pitch-ratio or per-voice-bend applied (those are composed by
    // the caller, VoiceManager::writeCsoundControls, exactly like
    // effectivePitchRatio below).
    float readCsoundFreq(int samplesToAdvance);
    /** Smooth an immediate same-voice restart by fading from the last rendered sample. */
    void beginRestartFade();
    void setAftertouch(float pressure) { aftertouch_ = juce::jlimit(0.0f, 1.0f, pressure); }
    float getAftertouch() const { return aftertouch_; }

    // MPE per-note pitch bend — the X axis. Two numbers for one gesture, because
    // they answer different questions: the SEMITONES are what the pitch is bent
    // by (bend range already applied), the NORMALISED value is how far through
    // the wheel the finger travelled, ±1 at full deflection. Only the first can
    // move the pitch; only the second can drive a modulation target, since a
    // target has no idea what a semitone is. Deriving one from the other would
    // need the bend range in force, and that range can change mid-note.
    void setPerVoicePitchBend(float semitones, float normalised = 0.0f)
    {
        perVoicePitchBendSemitones_ = juce::jlimit(-48.0f, 48.0f, semitones);
        perVoicePitchBendNorm_      = juce::jlimit(-1.0f, 1.0f, normalised);
    }
    float getPerVoicePitchBend() const { return perVoicePitchBendSemitones_; }
    float getPerVoicePitchBendNorm() const { return perVoicePitchBendNorm_; }

    // MPE per-note Timbre (the Y axis, MIDI CC 74), normalised 0..1 and read
    // ABSOLUTE: 0 is "no effect", 127 is full effect. Rest is therefore 0, and a
    // note that never receives a CC 74 is exactly unmodulated.
    //
    // It used to be 64/127 — a centre detent, from the LinnStrument, whose Y is a
    // sideways travel and genuinely bipolar. That is not the general case and it
    // was measured to be wrong here: an Expressive E Osmose rests its Y at 0 and
    // rises with forward key travel (201 of 203 note-ons in a 120 s capture had
    // CC 74 = 0 in force), so a centred reading put every one of its notes four
    // octaves below the dialled cutoff. Absolute is also what MPE instruments
    // generally do with CC 74. A bipolar controller now rests at half scale
    // instead of at zero; that is the cost, and it is the smaller one.
    static constexpr float kTimbreRest = 0.0f;
    void setTimbre(float t) { timbre_ = juce::jlimit(0.0f, 1.0f, t); }
    float getTimbre() const { return timbre_; }

    // ── Per-block setup ──
    /** Configure envelopes from block params. Call once per block before the renderBlock loop. */
    void configureForBlock(const BlockParams& p);

    /** Block-based rendering with sub-block filter coefficient updates.
     *  Writes numSamples into output; outputRight receives Granular stereo when provided.
     *  csoundBuf: this voice's block-aligned Csound render (Phase-1 spec §3), already
     *  offset to the sub-range like the lfo buffers; nullptr in every non-Csound mode
     *  (and the safety net for a not-yet-ready Csound engine — renders silence). */
    void renderBlock(float* output, float* outputRight, const BlockParams& p,
                     const float* lfo1Buf, const float* lfo2Buf, const float* lfo3Buf, int numSamples,
                     const float* csoundBuf = nullptr);

    static constexpr int SUB_BLOCK_SIZE = 32;

    // Mod value accessors (for VoiceManager to capture after renderBlock)
    const float* getLastModVals() const { return lastModVal_; }   // [kNumModEnvs]
    float getLastModulatedCutoff() const { return lastModulatedCutoff_; }
    float getLastModulatedResonance() const { return lastModulatedResonance_; }
    float getLastModulatedScan() const { return lastModulatedScan_; }
    float getLastModulatedNoiseLevel() const { return lastModulatedNoiseLevel_; }

    // ── State queries ──
    bool isActive() const { return active; }
    bool isReleasing() const { return active && !noteHeld; }
    int  getCurrentNote() const { return currentNote; }
    float getAmpEnvLevel() const { return ampEnv.isIdle() ? 0.0f : lastAmpEnvLevel; }
    // Csound voice bridge (Phase-1 spec §3): VoiceManager::writeCsoundControls
    // reads these every sub-block to publish gate/vel to the orchestra.
    // currentVelocity/noteHeld were already private state with no getter.
    float getCurrentVelocity() const { return currentVelocity; }
    bool isNoteHeld() const { return noteHeld; }

    // ── The PITCH MODULATION BUS ──────────────────────────────────────────
    // Normalized semitone-fractions; multiply by ModCalib::kPitchModSemitones
    // for semitones. Every engine path resolves pitch through this ONE
    // function, the Csound bridge included, because the three copies that
    // preceded it were identical by hand and the Csound path was simply
    // missing -- vibrato worked on wavetable/sampler/freeze and was silently
    // dropped on the LCO. Three hand-kept copies is how that happens again.
    float pitchBusSemitones(const BlockParams& p,
                            float ampEnvVal, const float* modEnvVals,
                            float lfo1Val, float lfo2Val, float lfo3Val) const;

    // The widest |bus| the same sum can reach with every routed source at full
    // scale, in bus units. The sampler needs it at a note's FIRST block to pick
    // its render path once and keep it (SamplePlayer::setPitchModulationReach).
    float pitchBusReachSemitones(const BlockParams& p) const;

    // The same bus as a frequency RATIO, resolved from RAW (un-depthed) LFO
    // samples using this voice's current envelope levels. For callers OUTSIDE
    // the render loop: the Csound bridge must publish its control channels
    // BEFORE the orchestra renders, so it cannot use the per-sample values
    // renderBlock computes -- it reads the LFOs at the segment start instead.
    float pitchBusRatioFromRawLfo(const BlockParams& p,
                                  float lfo1Raw, float lfo2Raw, float lfo3Raw) const;

    // ── Tuning ──
    void setTuningTable(const float* table) { tuningHz_ = table; }

    /** Lend this voice the pool's shared DCA-analysis buffer (see dcaScratch_).
        VoiceManager::prepare owns it and calls this for every voice; the buffer
        must outlive the voice's rendering and must not be resized while a block
        is in flight. */
    void setDcaAnalysisScratch(float* buffer, int numSamples)
    {
        dcaScratch_ = buffer;
        dcaScratchLen_ = numSamples;
    }

    // ── Engine mode ──
    // Csound: a REAL 4th DSP path — a processor-owned CsoundEngine instance
    // renders this voice's audio directly (renderBlock's 4th branch, fed via
    // the csoundBuf parameter/csoundBuf_ member above). Renders silence
    // whenever csoundBuf_ is null — not ready yet, engine switched away from
    // Csound mid-note, or a Csound-less (stub) build.
    enum class EngineMode { Sampler, Wavetable, Freeze, Csound };
    void setEngineMode(EngineMode mode) { engineMode = mode; }
    EngineMode getEngineMode() const { return engineMode; }

    // ── Access to sub-components ──
    WavetableOscillator& getOsc() { return osc; }
    SamplePlayer& getSampler() { return sampler; }
    FreezeTextureEngine& getFreezeEngine() { return freezeEngine; }
    ADSREnvelope& getAmpEnvelope() { return ampEnv; }
    ADSREnvelope& getModEnvelope(int i) { return modEnvs[i]; }
    T5ynthFilter& getFilter() { return filter; }
    LFO& getPerVoiceLfo1() { return perVoiceLfo1; }
    LFO& getPerVoiceLfo2() { return perVoiceLfo2; }
    LFO& getPerVoiceLfo3() { return perVoiceLfo3; }

    // Voice age for stealing (monotonic counter set by VoiceManager). NOTE:
    // this bumps on a poly BIND continuation too (VoiceManager keeps a slid
    // voice "newest" so it isn't stolen mid-glide) — it is NOT a fresh-strike-
    // only signal. See triggerEpoch below for that.
    uint64_t noteOnTimestamp = 0;
    // Csound retrigger epoch (Phase-1 spec D8): set by VoiceManager to a
    // snapshot of noteOnTimestamp, but ONLY at a genuine fresh strike (mono/
    // poly/drone noteOn's non-legato path) — never on a legato-glide or a poly
    // BIND continuation (verified in VoiceManager.cpp: the bind path DOES bump
    // noteOnTimestamp itself, for voice-stealing-age purposes only, which would
    // wrongly re-strike the orchestra's changed2(trig) on every slide if reused
    // here). VoiceManager::writeCsoundControls publishes this (wrapped) as the
    // orchestra's trigN channel.
    uint64_t triggerEpoch = 0;

private:
    WavetableOscillator osc;
    // Csound engine frequency (Phase-1 spec D7): sample-accurate glide is not
    // achievable through a k-rate control channel, so glide in Csound mode is
    // a block-rate SmoothedValue here on the voice, advanced at channel-write
    // time (later commit); the orchestra's own portk provides the audible
    // smoothing between steps. noteOn snaps it, glideToNote re-arms it.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> csoundFreq_;
    // The key ON/OFF as a control voltage for the DCA — the "Taste an/aus" arm of
    // the voice-state XOR, read by computeDcaGain whenever the amp envelope is
    // routed off the DCA. A bare 0/1 would step the output by full scale on every
    // note edge, so it is ramped over KEY_GATE_MS. Advanced once per sample in
    // renderBlock, next to the envelopes.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> keyGate_;
    // This block's Csound render input (Phase-1 spec §3), stored from the
    // renderBlock parameter of the same name — mirrors how lfo1Buf/lfo2Buf/
    // lfo3Buf are threaded, just captured into a member so the render-branch
    // condition below (engineMode==Csound && csoundBuf_ != nullptr) reads
    // naturally next to the other engine-ready flags. Never owned/freed here;
    // VoiceManager/CsoundEngine own the buffer's lifetime.
    const float* csoundBuf_ = nullptr;
    SamplePlayer sampler;
    FreezeTextureEngine freezeEngine;
    ADSREnvelope ampEnv;
    ADSREnvelope modEnvs[kNumModEnvs];
    LFO perVoiceLfo1; // used when LFO mode == Trigger
    LFO perVoiceLfo2;
    LFO perVoiceLfo3;
    // Per-voice LFO output buffers — only filled when the corresponding
    // bp.lfoNTrigMode is true. In Free mode the global buffer is used and
    // these stay untouched.
    std::vector<float> perVoiceLfoBuf1_;
    std::vector<float> perVoiceLfoBuf2_;
    std::vector<float> perVoiceLfoBuf3_;
    // Stereo filter pairs — L processes output[], R processes outputRBuf[].
    // Same coefficients (mirrored at setCutoff/setReso/etc.), separate state.
    T5ynthFilter       filter;       // linear TPT SVF (low-CPU default)
    LadderFilter   filterLadder; // Huovilainen nonlinear ladder
    CutoffWarpFilter   filterWarp;   // Surge-XT-style ZDF ladder + style
    T5ynthFilter       filterR;
    LadderFilter   filterLadderR;
    CutoffWarpFilter   filterWarpR;
    // Drive oversamplers (polyphase IIR half-band), 2-channel for stereo drive.
    // Three instances, prepared in prepare(); renderBlock() picks the one
    // matching bp.filterDriveOs.
    std::unique_ptr<juce::dsp::Oversampling<float>> driveOs2x_;  // numStages=1, 2x
    std::unique_ptr<juce::dsp::Oversampling<float>> driveOs4x_;  // numStages=2, 4x
    std::unique_ptr<juce::dsp::Oversampling<float>> driveOs8x_;  // numStages=3, 8x
    // OS factor the nonlinear filters (Ladder/Warp) are currently prepared at.
    // 1 = base rate. renderBlock re-prepares them at sr×factor only when the
    // requested factor differs (rare → no per-block state reset). SVF never OS.
    int filterPreparedOsFactor_ = 1;
    NoiseGenerator noise;

    EngineMode engineMode = EngineMode::Sampler;

    int currentNote = -1;
    int octaveShift_ = 0;
    float currentVelocity = 0.0f;
    float aftertouch_ = 0.0f;
    float perVoicePitchBendSemitones_ = 0.0f;
    float perVoicePitchBendNorm_ = 0.0f;   // same gesture, ±1 at full wheel
    float timbre_ = kTimbreRest;           // MPE CC74, absolute; rest is 0
    bool active = false;
    bool noteHeld = false;
    float lastAmpEnvLevel = 0.0f;
    float lastOutputSample_ = 0.0f;   // L tail at retrigger time
    float lastOutputSampleR_ = 0.0f;  // R tail at retrigger time
    float baseFrequency = 440.0f;
    const float* tuningHz_ = nullptr;  // set by VoiceManager per-block

    /** Get frequency for MIDI note using tuning table (falls back to 12-TET). */
    float tunedHz(int midiNote) const
    {
        int n = std::max(0, std::min(127, midiNote));
        return (tuningHz_ != nullptr) ? tuningHz_[n]
            : static_cast<float>(juce::MidiMessage::getMidiNoteInHertz(n));
    }

    double sr = 44100.0;

    // Global velocity → envelope-peak amount [0..1] (updated per block from
    // BlockParams). Drives velPeakScale for ALL three envelopes' note-on peaks,
    // so velocity scales each env's depth on its target. Orthogonal to per-env Amt.
    float velAmt_ = 1.0f;

    // Per-stage velocity sensitivity, signed [-1..+1] (updated per block from
    // BlockParams). A/D/R scale the envelope TIMES only; the LEVEL is scaled by
    // velAmt_ above (velocity→peak), held level by Aftertouch.
    float ampAttackBaseMs_ = 0.0f;
    float ampDecayBaseMs_ = 0.0f;
    float ampReleaseBaseMs_ = 0.0f;
    float ampAttackVelSens_ = 0.0f;
    float ampDecayVelSens_ = 0.0f;
    float ampReleaseVelSens_ = 0.0f;
    float modAttackBaseMs_[kNumModEnvs] = {};
    float modDecayBaseMs_[kNumModEnvs] = {};
    float modReleaseBaseMs_[kNumModEnvs] = {};
    float modAttackVelSens_[kNumModEnvs] = {};
    float modDecayVelSens_[kNumModEnvs] = {};
    float modReleaseVelSens_[kNumModEnvs] = {};

    // Cached mod values from last renderBlock (for VoiceManager capture)
    float lastModVal_[kNumModEnvs] = {};
    float lastModulatedCutoff_ = 20000.0f;
    float lastModulatedResonance_ = 0.0f;
    float lastModulatedScan_ = 0.0f;
    float lastModulatedNoiseLevel_ = 0.0f;

    // Pre-rendered sampler block (pitch-shifted via Signalsmith Stretch)
    int maxBlockSize_ = 512;
    std::vector<float> samplerBlockBuf_;

    struct PreStretchNormState
    {
        float ampAttack = -1.0f;
        float ampDecay = -1.0f;
        float ampSustain = -1.0f;
        float ampRelease = -1.0f;
        float ampAmount = -1.0f;
        int ampTarget = EnvTarget::DCA;
        bool ampLoop = false;
        // Bends are floats now, and -1 is a LEGAL bend (Log) — the sentinel has
        // to sit outside the parameter range, like the -2 velocity sentinels.
        float ampAttackBend = -9.0f;
        float ampDecayBend = -9.0f;
        float ampReleaseBend = -9.0f;
        float ampAttackVelSens = -2.0f;
        float ampDecayVelSens = -2.0f;
        float ampReleaseVelSens = -2.0f;

        // Sentinels, not defaults: every field starts at a value no real
        // parameter can hold, so the first comparison always reports "changed".
        struct ModEnvState
        {
            int   target = EnvTarget::None;
            float attack = -1.0f;
            float decay = -1.0f;
            float sustain = -1.0f;
            float release = -1.0f;
            float amount = -1.0f;
            bool  loop = false;
            float attackBend = -9.0f;
            float decayBend = -9.0f;
            float releaseBend = -9.0f;
            float attackVelSens = -2.0f;
            float decayVelSens = -2.0f;
            float releaseVelSens = -2.0f;
        };
        ModEnvState modEnv[kNumModEnvs];

        float velocity = -1.0f;
        float velAmt = -1.0f;
        float startPos = -1.0f;
        float loopStart = -1.0f;
        float loopEnd = -1.0f;
        float startPosOffset = -999.0f;
        float crossfadeMs = -1.0f;
        int loopMode = -1;
        bool normalizeOn = false;
    };

    void updateSamplerPreStretchNorm(const BlockParams& p);
    bool preStretchNormStateMatches(const BlockParams& p) const;
    static bool modEnvStateMatches(const PreStretchNormState& st, const BlockParams& p);
    void applyVelocityTimedEnvelopeTimes();
    void applyRestartFadeStereo(float& L, float& R);

    float restartFadeTailSample_ = 0.0f;
    float restartFadeTailSampleR_ = 0.0f;
    int restartFadeSamplesLeft_ = 0;
    int restartFadeTotalSamples_ = 1;
    static constexpr float RESTART_FADE_MS = 3.0f;
    // Rise/fall of the key gate above. Same 3 ms as the retrigger seam — the
    // shortest fade this synth uses, long enough that the edge is not a click and
    // short enough that a gated note still reads as instant. Its own constant, so
    // changing one fade never silently moves the other.
    static constexpr float KEY_GATE_MS = 3.0f;

    PreStretchNormState preStretchNormState_;
    float samplerPreStretchNormGain_ = 1.0f;
    bool samplerPreStretchNormDirty_ = true;
    // Where updateSamplerPreStretchNorm writes the DCA curve it analyses. Not
    // owned: VoiceManager holds ONE buffer for the whole pool and hands it to
    // every voice in prepare() (setDcaAnalysisScratch below). It has to be
    // pre-allocated because that analysis runs on the audio thread — every
    // block, and re-computed on every note-on in sampler mode with Normalize on
    // — and it may not be per-voice, because the pool is 128 voices deep and the
    // ceiling is 3 s: 70 MB at 48 kHz for a buffer only ever used by one voice
    // at a time. Every configureForBlock caller runs under the processor's
    // callback lock, so the single buffer has exactly one writer.
    float* dcaScratch_ = nullptr;
    int    dcaScratchLen_ = 0;

};
