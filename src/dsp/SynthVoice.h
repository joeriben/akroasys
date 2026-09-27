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

    /** All Sound Off (CC 120) and the panic button: the fastest stop this synth
     *  has, which is not an instant one. The MIDI spec puts volume envelopes to
     *  zero "as soon as possible"; a truly instant stop is a full-scale step and
     *  clicks, which is the very thing KEY_GATE_MS and the envelope's own
     *  MIN_RAMP_SEC exist to prevent -- both 3 ms, and this asks for neither
     *  more nor less than that floor.
     *
     *  Unlike noteOff this is right to call on a voice ALREADY releasing: it
     *  restarts the release from the current level over the floor, so it can
     *  only shorten a tail. noteOff on such a voice restarts it over the
     *  PATCH's release time and lengthens it, which is why allNotesOff skips
     *  releasing voices on the key-up path and does not skip them here.
     *
     *  Both arms of the VCA are closed, because which one holds the level is a
     *  patch decision (computeDcaGain): with the amp envelope on the DCA the
     *  envelope is the level, otherwise the key gate is. */
    void cutSound();
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
    /** Whether the per-note bend is AUDIBLE. X reaching the pitch is a wiring
        now - the Pitch row's source - not a fact of the instrument, so the
        stored travel is kept either way and only its effect is switched. Kept
        rather than zeroed so moving the row back to X restores the bend of a
        finger that has not moved since. */
    void setXBendsPitch (bool b) { xBendsPitch_ = b; }
    float getPerVoicePitchBend() const
    {
        return xBendsPitch_ ? perVoicePitchBendSemitones_ : 0.0f;
    }
    float getPerVoicePitchBendNorm() const { return perVoicePitchBendNorm_; }

    // MPE per-note Timbre (the Y axis, MIDI CC 74). What this voice holds is not
    // the CC value: it is HOW FAR THIS NOTE HAS TRAVELLED from where it began,
    // signed, -1..+1, zero at note-on always.
    //
    // Neither a fixed 0 nor a fixed 64/127 can be right for every instrument,
    // and the MPE spec says so itself (1.0, §3.3.5): a controller sends CC 74
    // either "Initial-position", where the value at Note On encodes where the
    // interaction started, or "Initial-64", where it "must be 40h (64 decimal),
    // such that movement can follow in either a positive or negative direction".
    // The spec adds that neither scheme fits every mode of interaction, so a
    // receiver that picks one picks wrong for half the instruments.
    //
    // Reading the value IN FORCE AT NOTE ON as this note's rest fits both, needs
    // no setting and no table of devices. An Osmose sends CC 74 = 0 immediately
    // before each Note On (measured: 201 of 203 note-ons in a 120 s capture) --
    // its Y is the key's second, deeper pressure stage, so rest is the bottom and
    // the whole travel is upward, exactly as before this. An Initial-64
    // controller arrives at 64 and gets a rest of 64, so its Y is bipolar around
    // where the finger landed, which is what the sentence above asks for.
    //
    // The travel is the PLAIN difference, and deliberately not scaled to the
    // remaining span in each direction. Scaling was the first version and it
    // inverted the axis at the ends: a rest captured at 127 left no upward span
    // at all, so the reading was pinned at 0 going up and ran to -1 going down
    // -- the whole gesture sign-flipped, on the notes least able to afford it
    // (Y is what Cutoff and Scan start on). Plain, the same note reads 0 at the
    // top and travels down as the finger lifts, which is what the finger is
    // doing. Monotone with the hand, always, at every rest.
    //
    // The cost is that a rest in the middle of the axis has half the travel in
    // each direction rather than a full one. That is not a defect to correct:
    // the finger really does have half the key each way. For rest 0 this is
    // bit-identical to reading the CC absolutely, which is what keeps the Osmose
    // exactly where it was.
    static constexpr float kTimbreRest = 0.0f;

    /** Where this note's Y starts. Called once per note-on with the CC 74 value
        in force on the note's channel; the travel is measured from there. */
    void beginTimbre(float restAbsolute)
    {
        timbreRest_ = juce::jlimit(0.0f, 1.0f, restAbsolute);
        timbre_     = 0.0f;
    }

    void setTimbre(float absolute)
    {
        absolute = juce::jlimit(0.0f, 1.0f, absolute);
        timbre_  = absolute - timbreRest_;   // both in [0..1], so this is in [-1..+1]
    }

    /** Signed travel from this note's rest, -1..+1. */
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
    // Everything about the filter that a coefficient cannot carry: which model
    // runs, which of its stages or taps is the output, and at what rate. A
    // change to any of it is a transition (see renderBlock), never an
    // in-place switch — the model/stage it switches INTO has stale or zero
    // state and the output steps (measured +31..+46 dB of HF over the steady
    // state on Snap recalls that change only that setting).
    struct FilterCfg
    {
        bool enabled    = false;
        int  algorithm  = FilterAlgorithm::SVF;
        int  slope      = 0;
        int  type       = 0;
        int  warpStyle  = 0;   // Warp only
        bool svfDrive   = false; // SVF only: the Phase B tanh is in circuit
        int  svfDriveOs = 0;   // SVF only, and only with svfDrive: FilterDriveOs index
        int  nlOs       = 1;   // Ladder/Warp only: 1, 2 or 4

        bool operator== (const FilterCfg& o) const noexcept
        {
            return enabled == o.enabled && algorithm == o.algorithm && slope == o.slope
                && type == o.type && warpStyle == o.warpStyle && svfDrive == o.svfDrive
                && svfDriveOs == o.svfDriveOs && nlOs == o.nlOs;
        }
        bool operator!= (const FilterCfg& o) const noexcept { return ! (*this == o); }
    };

    // Canonical form of this block's filter configuration (BlockParams' loose
    // fields collapsed to only what a coefficient cannot carry). A private
    // static helper rather than a free function because FilterCfg above is
    // private — defined in SynthVoice.cpp.
    static FilterCfg filterCfgFrom (const BlockParams& p);

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

    // ── Filter transition bank ──────────────────────────────────────────
    // A second, independent set of the same six filters + three oversamplers,
    // used only while a discrete filter change is in flight. beginFilterTransition
    // (SynthVoice.cpp) configures it with the NEW configuration; renderBlock
    // crossfades its output in over the fade (FilterXf::Fade) and
    // completeFilterTransition promotes it to live when the fade ends. The
    // live members above keep running their OLD configuration, untouched,
    // for the whole transition.
    //
    // Two kinds, decided once in beginFilterTransition and latched in
    // filterXfShared_ below. SHARED (same model, same rate, same drive
    // stage) copies the live model's sounding state across and runs both
    // models through the LIVE bank's drive stage and oversampler
    // (processFilterPathShared) — the xf oversamplers are untouched. SEPARATE
    // (everything else: model change, rate change, drive-stage change,
    // filter on/off) starts the xf bank at zero and runs its own full path,
    // including its own oversamplers. Either way the target configuration is
    // pre-rolled once, in the transition's first sub-block, over
    // FILTER_XF_PREROLL_MS of the recent filter input
    // (preRollFilterTransition) so it starts settled instead of ringing up
    // from zero — see filterHistL_/filterHistR_ below.
    //
    // Outside a transition (FilterXf::None) this bank sits idle; the steady
    // state pays one configuration compare per render call, the history
    // write below, and two zeroed 32-sample scratch buffers per sub-block.
    T5ynthFilter     xfFilter_,  xfFilterR_;
    LadderFilter     xfLadder_,  xfLadderR_;
    CutoffWarpFilter xfWarp_,    xfWarpR_;
    std::unique_ptr<juce::dsp::Oversampling<float>> xfOs2x_, xfOs4x_, xfOs8x_;
    int xfPreparedOsFactor_ = 1;         // what xfLadder_/xfWarp_ (all four) are prepared at
    // Scratch SVF instances for the SHARED kind's pre-roll only (see
    // preRollFilterTransition): copy-assigned from xfFilter_/xfFilterR_ at
    // the target's coefficients, then run over the recent history to settle
    // the stages the copied live state does not already cover. Real members
    // (prepared in prepare(), reset in reset()) rather than stack locals — a
    // stack T5ynthFilter would allocate, since the juce TPT filter it wraps
    // holds std::vectors.
    T5ynthFilter xfPreRollSvf_, xfPreRollSvfR_;

    FilterCfg liveFilterCfg_;            // what the EXISTING members run
    FilterCfg xfFilterCfg_;              // what the transition bank runs
    // None: only the live bank runs. Fade: both banks are summed, live
    // fading out and xf fading in, over FILTER_XF_FADE_MS.
    enum class FilterXf { None, Fade };
    FilterXf filterXf_ = FilterXf::None;
    int  filterXfLeft_ = 0;              // samples left in the current phase
    int  filterXfFadeTotal_ = 1;
    // Which of the two kinds above the transition in flight is — set once in
    // beginFilterTransition, read by renderBlock's filter-path section
    // (which bank(s) run the drive stage/oversampler) and by
    // completeFilterTransition (whether the xf oversamplers hold this
    // signal's history to promote, or untouched instances to leave alone).
    // Cleared by the adopt branch and by completeFilterTransition;
    // meaningless outside a transition.
    bool filterXfShared_ = false;
    // True for exactly the transition's first sub-block (the coefficient
    // block): preRollFilterTransition has not yet run the recent history
    // through the target configuration. Cleared by adopt, prepare, reset,
    // and once the pre-roll has run.
    bool filterXfPreRollPending_ = false;
    // True right after prepare()/reset(), and for a note struck from idle
    // (noteOn): nothing is sounding through the filter (or an envelope
    // routing) yet, so the next renderBlock takes p's configuration outright
    // instead of transitioning into it — there is nothing to fade FROM. Also
    // set by beginRestartFade (a fresh strike on a voice that is still
    // sounding): the restart fade already carries the output from the old
    // note's last sample into the new one, so the new note takes the
    // configuration in force from its first sample instead of starting out
    // through the old one.
    bool filterCfgAdopt_ = true;

    // Set when a sub-block glided the cutoff; the next sub-block that does not
    // glide pushes its value with force (the filter models' setCutoff force
    // parameter) and clears this -- a glide's last step is one increment
    // short of the true end value, and the model's own dead-band would
    // otherwise swallow every later push of that same value.
    bool filterGlideOwed_ = false;

    // Ring buffer of the last FILTER_XF_PREROLL_MS of DRY filter input — the
    // signal any filter bank sees before Phase B/C run — written
    // unconditionally every sub-block, filter on or off, so a "filter on"
    // transition has something to pre-roll from too. filterHistPos_ is the
    // next WRITE index; filterHistCount_ is the number of valid samples
    // held (<= capacity). Sized and zeroed only in prepare() — the one place
    // this class may allocate.
    std::vector<float> filterHistL_, filterHistR_;
    int filterHistPos_ = 0;
    int filterHistCount_ = 0;

    // FILTER_XF_FADE_MS: the declick minimum (1 ms), the only smoothing this
    // project allows without the owner's order.
    static constexpr float FILTER_XF_FADE_MS    = 1.0f;
    // How much of the recent input (filterHistL_/R_) the target configuration
    // is run over, once, before the fade makes it audible, so that it starts
    // settled instead of ringing up from zero. It adds no latency: the fade
    // still starts in the sub-block the change arrives in. The cost is one
    // pass of the new path over this much input per transitioning voice, all
    // of it in that sub-block. Measured in the built standalone (48 kHz, held
    // C3, a source that holds still; the error 2 ms after the fade against
    // the settled new sound): a model change or the filter switching on lands
    // at -34..-68 dB at resonance 0.43 (from zero state: -6..-12 dB), and at
    // -15..-34 dB at resonance 0.85 (from zero state: -4..-5 dB). There the
    // filter rings longer than this history, and the error falls below
    // -40 dB about 20 ms after the change.
    // The pre-roll runs only while less than half of the host block's time has
    // passed (BlockParams::filterPreRollDeadlineTicks): one that would start
    // later does not run, one that reaches it stops after its current 32
    // samples, and that voice's new path starts from zero state instead. A
    // recall moving many voices at once settles the voices that fit instead of
    // overrunning the block.
    static constexpr float FILTER_XF_PREROLL_MS = 10.0f;

    // A lightweight VIEW onto one bank's six filters, three oversamplers and
    // prepared-OS-factor. Built on demand at each call site, never stored —
    // the oversampler pointers change when completeFilterTransition swaps
    // them, so a stored view could go stale. Lets configureFilterBank /
    // processFilterPath / prepareNonlinearAt run against either bank without
    // duplicating their body per bank.
    struct FilterBank
    {
        T5ynthFilter& svf;  T5ynthFilter& svfR;
        LadderFilter& ladder;  LadderFilter& ladderR;
        CutoffWarpFilter& warp;  CutoffWarpFilter& warpR;
        juce::dsp::Oversampling<float>* os2;
        juce::dsp::Oversampling<float>* os4;
        juce::dsp::Oversampling<float>* os8;
        int& preparedOs;
    };
    FilterBank liveBank() noexcept { return { filter, filterR, filterLadder, filterLadderR, filterWarp, filterWarpR,
                                              driveOs2x_.get(), driveOs4x_.get(), driveOs8x_.get(), filterPreparedOsFactor_ }; }
    FilterBank xfBank() noexcept   { return { xfFilter_, xfFilterR_, xfLadder_, xfLadderR_, xfWarp_, xfWarpR_,
                                              xfOs2x_.get(), xfOs4x_.get(), xfOs8x_.get(), xfPreparedOsFactor_ }; }

    // Filter-switch transition helpers (defined in SynthVoice.cpp). Together
    // they implement the bank handoff described above the xf members:
    // prepareNonlinearAt/configureFilterBank are the per-bank versions of
    // what renderBlock used to do only on the live members.
    // processDriveStage/processFilterStages are processFilterPath's own two
    // phases, split out so processFilterPathShared can run them once,
    // shared, for the shared kind's single drive stage/oversampler;
    // processFilterPath itself stays the live bank's (and the separate
    // kind's xf bank's) entry point — early-out then both phases, unchanged
    // in behaviour. preRollFilterTransition settles the target configuration
    // on the recent history; beginFilterTransition arms the xf bank and the
    // pre-roll; completeFilterTransition promotes the xf bank to live once
    // the fade ends.
    void prepareNonlinearAt (FilterBank b, int factor);
    void configureFilterBank (FilterBank b, const FilterCfg& c, float cutoffHz, float reso, float mix, float driveGain, bool exact = false);
    // While either ramp is in flight -- the envelope routing/amount ramp
    // above (ENV_ROUTE_RAMP_MS) or a filter-control ramp (FILTER_CTL_RAMP_MS
    // below) -- the filter does not stand for the 32 samples of a sub-block
    // and step at the next boundary: cutoff, resonance, mix and drive all
    // move from the sub-block's coefficients (the "from" values) to where
    // they stand by the sub-block's end, once per base-rate sample --
    // cutoff and drive geometrically, resonance and mix linearly. A step on a
    // sounding filter steps its output (the TPT SVF's low-pass output carries
    // g times its band-pass state), and a 1 ms ramp read only at sub-block
    // boundaries arrives as two such steps, up to a couple of octaves each.
    // Outside either ramp nothing glides and the filter stages run as before.
    struct FilterGlide
    {
        float fromHz = 20000.0f;
        float log2Ratio = 0.0f;   // log2 (end cutoff / fromHz)
        int   n = 1;              // base-rate samples it spans
        float hzAt (int j) const noexcept
        {
            return fromHz * std::exp2 (log2Ratio * static_cast<float> (j) / static_cast<float> (n));
        }
        float fromReso = 0.0f;
        float dReso = 0.0f;       // end minus start, linear
        float resoAt (int j) const noexcept
        {
            return fromReso + dReso * static_cast<float> (j) / static_cast<float> (n);
        }
        float fromMix = 1.0f;
        float dMix = 0.0f;        // end minus start, linear
        float mixAt (int j) const noexcept
        {
            return fromMix + dMix * static_cast<float> (j) / static_cast<float> (n);
        }
        float fromDrive = 1.0f;
        float log2Drive = 0.0f;   // log2 (end drive gain / fromDrive)
        float driveAt (int j) const noexcept
        {
            return fromDrive * std::exp2 (log2Drive * static_cast<float> (j) / static_cast<float> (n));
        }
    };
    // Sets the active model's cutoff/resonance/mix, left and/or right, and
    // (Ladder/Warp only) input drive, to g's value at base-rate sample j. The
    // SVF's drive is its tanh stage in processDriveStage, not a filter
    // setter -- see processDriveStage's own glide handling.
    static void glideFilter (FilterBank b, const FilterCfg& c, const FilterGlide& g, int j, bool left, bool right);
    void processDriveStage (FilterBank b, const FilterCfg& c, float* L, float* R, int n, float driveGain,
                            const FilterGlide* glide = nullptr);
    void processFilterStages (FilterBank b, const FilterCfg& c, float* L, float* R, int n, bool stereo,
                              const FilterGlide* glide = nullptr);
    void processFilterPath (FilterBank b, const FilterCfg& c, float* L, float* R, int n, bool stereo, float driveGain,
                            const FilterGlide* glide = nullptr);
    void preRollFilterTransition (bool stereo, float driveGain, long long deadlineTicks);
    void beginFilterTransition (const FilterCfg& target);
    void completeFilterTransition();
    void processFilterPathShared (float* L, float* R, int n, bool stereo, float driveGain,
                                  const FilterGlide* glide = nullptr);

    // ── Envelope routing/amount de-zippering ────────────────────────────
    // `p.ampTarget` / `p.modEnv[m].target` and their amounts are read straight
    // per sample elsewhere in this class (`acc += val` when `target == X`); a
    // Snap recall, a knob, automation or a MIDI CC that changes one while a
    // note sounds therefore steps the destination from one sample to the next.
    // For the destinations where that step is a discontinuity — DCA, Filter,
    // NoiseLevel, and the three LFO depths — renderBlock instead follows a
    // routing or amount change over ENV_ROUTE_RAMP_MS: routing as a crossfade
    // of per-target weights (old target's weight 1→0, new target's 0→1,
    // together), amount as a ramp of the STEP: the voice uses p's amount,
    // drift and LFO included, plus the difference to the amount it was using
    // when the change arrived, and that difference shrinks to zero over the
    // ramp. A change is noticed on the Amt parameter itself
    // (BlockParams::amountBase / ampAmountBase), so drift or an LFO moving an
    // Amt passes through exactly as before and starts no ramp. Pitch, Scan,
    // and everything the processor reads off this voice (delay/reverb/LFO
    // rate/depth) are deliberately not part of this.
    //
    // Envelope sources in routing order: 0 = ENV 1 (amp), 1..4 = ENV 2..5 (modEnvs[0..3]).
    static constexpr int kNumEnvSources = 1 + kNumModEnvs;
    float envRouteW_[kNumEnvSources][EnvTarget::kCount] {};      // current weight per target id (0..1)
    float envRouteWStep_[kNumEnvSources][EnvTarget::kCount] {};  // per-sample increment while ramping
    int   envRouteGoalTarget_[kNumEnvSources] {};                // target the running/last ramp heads to
    float envAmtGoal_[kNumEnvSources] {};                        // Amt parameter the running/last ramp heads to
    // The amount the last rendered sample used, where a new ramp starts. Taken
    // from the amounts themselves rather than rebuilt from the parameter: drift
    // and LFO offsets are clamped into [0, 1] with the parameter, so the old
    // parameter plus the new offset is not always the amount that was heard.
    float envAmtLast_[kNumEnvSources] {};
    float envAmtDelta_[kNumEnvSources] {};                       // used minus p's amount; exactly 0 once settled
    float envAmtDeltaStep_[kNumEnvSources] {};
    int   envRouteRampLeft_ = 0;                                 // samples left; 0 = settled
    bool  envRouteAdopt_ = true;                                 // next renderBlock takes p's routing outright
    float lastAmpEnvLevelSm_ = 0.0f;                             // de-zippered twins of lastAmpEnvLevel /
    float lastModValSm_[kNumModEnvs] = {};                       // lastModVal_ for the ramped destinations
    // The declick minimum (1 ms) -- the only smoothing this project allows
    // without the owner's explicit order (mirrors FILTER_XF_FADE_MS above).
    // The DCA ramp runs per sample, and so does the cutoff while it runs
    // (FilterGlide above), although renderBlock evaluates the cutoff bus only
    // at SUB_BLOCK_SIZE (32-sample) boundaries.
    static constexpr float ENV_ROUTE_RAMP_MS = 1.0f;

    // Weights are indexed by the EnvTarget id; an id outside
    // [0, EnvTarget::kCount) gets no weight (treated as None). Pure reads of
    // `p`, so private STATIC helpers -- mirrors filterCfgFrom above.
    static int   envSourceTarget (const BlockParams& p, int e);
    // Whether target t is one of the destinations the ramp above actually
    // declicks (DCA, Filter, NoiseLevel, the three LFO depths) -- the ones
    // read through envRouteW_/envAmtEff rather than straight off p elsewhere
    // in this class. Pitch, Scan and the processor-side targets (delay/
    // reverb/LFO rate) are read straight everywhere, so a routing change onto
    // or off one of those, with neither end in this list, is taken outright
    // instead of arming a ramp (updateEnvRouteGoals) -- it would only cost
    // CPU and start the cutoff glide for a destination that cannot hear it.
    static bool  envTargetRamped (int t);
    static float envSourceAmount (const BlockParams& p, int e);
    static float envSourceAmountBase (const BlockParams& p, int e);
    // Settled (envRouteRampLeft_ == 0): source e's amount, verbatim. Mid-ramp:
    // p's amount plus the part of the step still left (envAmtDelta_).
    float envAmtEff (const BlockParams& p, int e) const;
    // Per renderBlock call: notices a routing/amount change against the
    // running goal and arms a new ramp from wherever the current one is.
    void updateEnvRouteGoals (const BlockParams& p);
    // Per sample: advances the ramp one step, snapping to the goal when it ends.
    void advanceEnvRoute() noexcept;
    // An LFO depth and the DCA gain from the routing weights instead of p's
    // targets. computeDcaGain in SynthVoice.cpp stays on p's targets for
    // updateSamplerPreStretchNorm, which analyses the patch, not this voice.
    static float computeEffectiveLfoDepthRouted (const float (&w)[kNumEnvSources][EnvTarget::kCount],
                                                 int target, float baseDepth,
                                                 float ampEnvVal, const float* modEnvVals);
    static float computeDcaGainRouted (const float (&w)[kNumEnvSources][EnvTarget::kCount],
                                       float ampEnvVal, const float* modEnvVals, float keyGate);

    // ── Continuous filter-control de-zippering ──────────────────────────
    // renderBlock reads p.baseCutoff/p.kbdTrack/p.baseReso/p.filterMix/
    // p.filterDriveDb (and the processor-derived p.filterDriveGain) once per
    // sub-block, straight into the coefficient block -- a Snap recall, a
    // preset change, automation or a MIDI CC that moves one of them
    // therefore steps a sounding filter's coefficients and clicks (measured:
    // 15..35 dB of HF above the settled sound on a recall's cutoff/kbd/reso/
    // drive step). This follows the same 1 ms declick the envelope routing/
    // amount ramp above uses, but keyed on the KNOB values alone --
    // envelopes, LFOs, drift, aftertouch and key position keep modulating
    // exactly as now, ramp or no ramp, since none of those ever touch
    // ctlSeen*_.
    static constexpr float FILTER_CTL_RAMP_MS = 1.0f;   // declick minimum, as ENV_ROUTE_RAMP_MS above

    // The five knob values this voice last took from p -- what a change is
    // noticed against (updateFilterCtlGoals).
    float ctlSeenCutoffHz_ = 20000.0f;
    float ctlSeenKbd_ = 0.0f;
    float ctlSeenReso_ = 0.0f;
    float ctlSeenMix_ = 1.0f;
    float ctlSeenDriveDb_ = 0.0f;
    // Used minus p, in the unit each control glides in: cutoff (with key
    // tracking) in octaves and drive in dB, so both move geometrically;
    // resonance and mix linearly. Exactly 0 once settled, the remaining step
    // mid-ramp -- same shape as envAmtDelta_ above.
    float ctlCutoffOct_ = 0.0f;   // octaves
    float ctlReso_ = 0.0f;        // linear
    float ctlMix_ = 0.0f;         // linear
    float ctlDriveDb_ = 0.0f;     // dB
    float ctlCutoffOctStep_ = 0.0f;
    float ctlResoStep_ = 0.0f;
    float ctlMixStep_ = 0.0f;
    float ctlDriveDbStep_ = 0.0f;
    int  ctlRampLeft_ = 0;                // samples left; 0 = settled
    bool filterCtlAdopt_ = true;          // next renderBlock takes p's knob values outright

    // Per renderBlock call: notices a knob change against the seen values and
    // arms a new ramp from wherever the current one is.
    void updateFilterCtlGoals (const BlockParams& p);
    // Per sample: advances the ramp one step, snapping to 0 when it ends.
    void advanceFilterCtl() noexcept;

    NoiseGenerator noise;

    EngineMode engineMode = EngineMode::Sampler;

    int currentNote = -1;
    int octaveShift_ = 0;
    float currentVelocity = 0.0f;
    float aftertouch_ = 0.0f;
    float perVoicePitchBendSemitones_ = 0.0f;
    bool  xBendsPitch_ = true;
    float perVoicePitchBendNorm_ = 0.0f;   // same gesture, ±1 at full wheel
    float timbre_     = 0.0f;              // signed travel from timbreRest_
    float timbreRest_ = kTimbreRest;       // the CC 74 in force when this note began
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
