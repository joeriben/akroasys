#pragma once
#include <memory>
#include <string>
#include <vector>

/**
 * Phase-1 Csound engine — one processor-owned Csound instance running 16
 * always-on, gate-sustained instruments (one per voice slot), driven purely
 * by named control channels the audio thread writes every sub-block.
 *
 * Phase 2 (SPEC_phase2_csound_swap.md) adds a swappable orchestra: prepare()
 * takes an optional orchestra-text override (nullptr = the Phase-1 built-in),
 * orchestraText() reports whichever text is currently compiled, and
 * primeForTakeover() silently settles a freshly-compiled instance's retrigger
 * state so a HELD note's later takeover doesn't audibly re-strike (S3). The
 * processor (PluginProcessor.h/.cpp) owns TWO of these and crossfades between
 * them; this class itself still only ever knows about ONE Csound instance.
 *
 * This header is compiled on EVERY platform/build and must never #include a
 * csound header (csound/sysdep.h leaks LIKELY, UNLIKELY, DIRSEP, the
 * ATOMIC_ family, and CS_PURE macro names that collide with identifiers
 * elsewhere in the codebase — the #undef hygiene block for that lives in
 * CsoundEngine.cpp,
 * copied from tools/csound_poc.cpp:94-102, confined to that one TU) nor any
 * JUCE header, so this class stays linkable from standalone offline test
 * binaries that don't pull in the plugin's JUCE tree.
 *
 * Real implementation (CsoundEngine.cpp) is only compiled when CMake found
 * CsoundLib64 (T5YNTH_HAS_CSOUND==1). Otherwise the inert stub below this
 * class (T5YNTH_HAS_CSOUND==0) makes every call a no-op / false / silence,
 * so call sites never need a CMake-level #ifdef — see SPEC §1/D5.
 */
class CsoundEngine
{
public:
    static constexpr int kMaxVoices = 16;   // fixed instrument/channel count, Phase 1
    static constexpr int kKsmps     = 64;
    /** Ceiling renderBareOscillator clamps its render rate to. Set to the same
     *  absolute engine-rate cap effectiveOversampleFactor applies, so a probe of a
     *  legitimately-oversampled orchestra is never silently clamped below the rate
     *  its body was compiled for. A caller deriving an output rate from the ratio
     *  must clamp with THIS before dividing, or its arithmetic drifts from the
     *  render's at any host rate above the ceiling. */
    static constexpr double kMaxProbeSampleRate = 200000.0;

    CsoundEngine();
    ~CsoundEngine();

    // Message/setup thread only (prepareToPlay, or the D9 background-compile thread).
    // Compiles the orchestra at this sample rate, runs the warmup (D4), resolves all
    // channel pointers. orchestraText == nullptr (default) compiles the Phase-1
    // built-in orchestra, unchanged from before Phase 2; a non-null string is compiled
    // verbatim (a full CSD, following the Phase-1 channel/instrument contract — the
    // caller/assembler guarantees that, Phase 3; this method only cares that it
    // compiles and all 16x6 channels resolve). An explicit orchestraText always forces
    // a genuine (re)compile even at an unchanged sample rate — the D9 "already
    // prepared, just grow the buffers" fast path only ever applies when the text is
    // the SAME as whatever is currently compiled (see orchestraText() below), so an
    // orchestra-swap request can never be silently swallowed by that fast path.
    // Returns false on any failure. Two kinds: the Csound library is not there at
    // all (Windows delay-loads it from beside the module, so this IS reachable — a
    // broken installation gives a silent LRO instead of a crash), or the orchestra
    // did not compile (loud in DBG; leaves the engine inert, never half-armed, and
    // never disturbs whatever was compiled before).
    //
    // oversampleFactor (1, 2 or 4) runs Csound ITSELF at sampleRate × factor and
    // decimates each voice back to sampleRate before voiceBuffer() hands it out —
    // the caller's whole view (startBlock/renderUpTo/voiceBuffer, all in HOST
    // samples) is unchanged. This is the only place aliasing can be prevented:
    // none of Csound's generators except vco2 is band-limited, so gbuzz stacks,
    // foscili sidebands, tanh() shaping and syncphasor edges fold at the rate
    // they are COMPUTED at, and folded energy is indistinguishable from real
    // partials afterwards — the downstream filter oversampling in SynthVoice
    // cannot undo it, only avoid adding more. Measured at f0 = 523 Hz (octave 5),
    // aliasing relative to signal: cymbal −7.7 dB at 1×, −13.9 at 2×, −181 at 4×;
    // `saw`+`warm` −27.6 / −47.5 / −48.4. Cost is linear in the factor; the
    // heaviest library patch (9 mode resonators × 16 voices) still renders 6×
    // faster than realtime at 4×. Any other value is clamped to 1; kKsmps must
    // stay divisible by it (64 is, for 1/2/4).
    bool prepare (double sampleRate, int maxBlockSize, const char* orchestraText = nullptr,
                  int oversampleFactor = 1);

    /** What `prepare` would ACTUALLY use for this rate and request — the requested
     *  factor after validity clamping and the absolute-engine-rate cap. Callers
     *  that compare "what is compiled" against "what the user asked for" must
     *  compare against THIS, not against the raw request: at a 96 kHz host rate a
     *  request of 4 compiles as 2, and a naive comparison would see a permanent
     *  mismatch and recompile forever. Static — no instance, no state. */
    static int effectiveOversampleFactor (double sampleRate, int requested);

    /** Absolute path of the Csound library THIS module is bound to (the one
     *  `&csoundCreate` resolves to in this image, never a lookup across the whole
     *  process, where a DAW's other Csound could win). Empty on Windows, in a
     *  build without Csound, or when the loader cannot say. The backend gate
     *  compiles against exactly this library (T5YNTH_CSOUND_LIB, ticket #25). */
    static std::string loadedLibraryPath();

    /** The factor the currently compiled orchestra was prepared with (1 when
     *  nothing is compiled). Message/compile-thread only, like orchestraText(). */
    int oversampleFactor() const;

    // Currently compiled orchestra text (Phase-2 spec S2): empty when the built-in
    // orchestra is active. Message/compile-thread only (Phase 5 preset save reads
    // this off the message thread; never called from the audio thread).
    const std::string& orchestraText() const;

    bool isReady() const;

    /** True once the compiled performance has ended (renderUpTo latched a
     *  non-zero csoundPerformKsmps) — the engine emits only silence until a
     *  full recompile. Any thread. */
    bool performanceHasEnded() const;

    /** The static gain `prepare()` measured for the orchestra it compiled, and that
     *  `renderUpTo` applies to every voice — 1.0 when nothing is compiled, or when
     *  the reference render came back silent.
     *
     *  WHY THIS EXISTS. Every other engine hands the VCA a NORMALISED source: the
     *  sampler normalises its buffer (SamplePlayer::normalizeBuffer, −1 dBFS peak
     *  ceiling), the wavetable normalises every frame. The LRO had nothing of the
     *  kind — it delivered whatever the authored body happened to peak at, times
     *  the fixed `HEADROOM = 0.32` in backend/lco_write.py's tail. Since the author
     *  is also told to keep `asig` near ±0.5, that stacks two independent headroom
     *  factors and lands a typical body at ~0.13 peak / −26.5 dBFS rms where a
     *  T5-oscillator sample lands at 0.69 / −14.1 dBFS. MEASURED on BJ's own A/B
     *  (2026-08-05, `a fog horn 16" + a violin 8"` against A:foghorn B:violin):
     *  12.4 dB rms / 14.3 dB peak quieter, at identical synth settings. The
     *  built-in placeholder orchestra sat 7.9 dB down by the same measurement.
     *
     *  A CONSTANT CANNOT FIX IT, which is why this is a measurement. The 30 library
     *  bodies span 0.32…1.65 in `asig` peak (14 dB) and 0.155…0.516 in rms (10 dB),
     *  so any single factor large enough to lift the quiet ones drives the loud ones
     *  into the tail's `clip` and turns a safety net into a permanent shaper.
     *
     *  Message/compile-thread only, like orchestraText(). */
    float outputTrim() const;

    /** Where `prepare()`'s reference render puts the orchestra, and the ceiling it
     *  may not exceed doing it — the two numbers SamplePlayer's own normaliser
     *  lands a real T5-oscillator render on, so the two engines match by sharing
     *  one law instead of by two separately-fitted trims.
     *
     *  BOTH ARE BUFFER-LEVEL, i.e. they describe the source BEFORE EngineCalib.
     *  That is the whole point: after this normalisation the LRO hands the VCA the
     *  same kind of source the sampler does, so it takes the sampler's own trim
     *  (EngineCalib::kCsound == kSampler) and the delivered levels follow. Reading
     *  either of these as a delivered level puts the LRO 2.2 dB over the sampler.
     *
     *  The ceiling is SamplePlayer::kNormalizeCeilingDb verbatim. The rms target is
     *  where SamplePlayer's PeakCap branch actually leaves SA3 material: an SA3
     *  render peaks at ~1.0 (measured 0.9451 over the playback region of the very
     *  session that produced this finding), i.e. inside kHotHeadroomDb, so it is
     *  peak-capped rather than driven to kSustainedTargetDb, and its buffer rms
     *  comes out at −11.9 dBFS. Matching the sampler's NOMINAL sustained target
     *  instead would leave the LRO 6 dB under the engine a player A/Bs it against —
     *  which is the same error that made the old calibration wrong, since
     *  tools/measure_engine_levels.cpp levels the sampler against a synthetic saw
     *  quiet enough to take that sustained branch. */
    static constexpr float kLevelTargetRmsDb   = -12.0f;
    static constexpr float kLevelPeakCeilingDb =  -1.0f;

    // ---- audio thread (RT-safe: raw pointer writes + performKsmps, zero lookups/locks) ----
    struct VoiceControls { float gate, freqHz, velocity, pressure, timbre, trigEpoch; };
    void setVoiceControls (int voiceIndex, const VoiceControls&);   // writes via cached MYFLT*

    /** The player's knobs for the AUTHORED instrument — twelve global control
     *  channels `lroP1a`…`lroP3d`, all 0..1, shared by every voice because they
     *  describe the instrument and not the note (backend/lco_write.py wires each
     *  one into a library parameter line). Same discipline as setVoiceControls: cached raw
     *  MYFLT* written straight, so this is audio-thread safe and does no name
     *  lookup. `values` must point at kNumGlobalControls floats.
     *
     *  A body that reads none of them is not a special case — the channels
     *  exist in every orchestra the host wraps, and writing an unread channel
     *  costs a store.
     *
     *  After the knobs come THREE MORE: one level per part, on the scaffold's
     *  own `osc1vol`…`osc3vol`, which every authored body already reads as
     *  `kvol1`…`kvol3`. They were constants in the orchestra head until now. */
    static constexpr int kNumKnobControls   = 12;
    static constexpr int kNumPartLevels     = 3;
    static constexpr int kNumGlobalControls = kNumKnobControls + kNumPartLevels;
    void setGlobalControls (const float* values);
    // Renders forward so that at least `upToSample+1` samples (block-relative) exist in every
    // per-voice buffer; carries ksmps surplus internally across calls and blocks.
    void renderUpTo (int upToSample);
    void startBlock (int numSamples);            // begins a new host block (resets write pos, replays carry)
    const float* voiceBuffer (int voiceIndex) const;  // block-aligned, sized maxBlockSize
    // ------------------------------------------------------------------------------------

    // Message/compile-thread only (Phase-2 spec S3): call AFTER a successful prepare()
    // and BEFORE this engine is published as a swap target. Seeds every voice's
    // trig+freq channel from the snapshot (epochs[kMaxVoices], freqs[kMaxVoices]; gates
    // stay 0, whatever prepare()'s warmup left them at), then pumps ~0.25s of ksmps,
    // discarding the output. Rationale: warmup leaves trig=0, so the FIRST real
    // channel write after a takeover would flip 0->epoch, changed2() would fire, and a
    // HELD note would audibly re-strike at the swap point. Priming fires that strike
    // silently (gate closed) and lets portk/the strike envelopes settle before the
    // real, audible gate-open ever happens. Never called on the audio thread.
    void primeForTakeover (const float* epochs, const float* freqs);

    /** ONE-SHOT OFFLINE RENDER of an orchestra text to mono samples — the BARE
     *  oscillator: no ADSR, no filter, no VCA, only what the orchestra itself
     *  does. For machine-listening probes (the self-check), never for playback.
     *
     *  Static and instance-free ON PURPOSE. It creates, uses and destroys its own
     *  CSOUND*, so it never touches the processor's two live engines and cannot
     *  disturb a sounding note. It also deliberately skips prepare()'s ~1.2 s
     *  warmup: that warmup exists so a freshly compiled engine can take over a
     *  HELD note without a click, which has no meaning for a render nobody hears.
     *
     *  THREADING: call from a background thread. It cannot reach the processor's
     *  csoundLifecycleMutex_ (static, owns no engine), so instead it takes the
     *  process-wide lock every prepare() also takes, which keeps the
     *  create/compile/start/destroy sequence serialised across ALL instances —
     *  the invariant the processor already maintains for its two engines, rather
     *  than an exemption from it.
     *
     *  IT DOES NOT WAIT FOR A CONCURRENT ORCHESTRA SWAP — the swap waits for IT.
     *  Called right after a bake, this thread reaches create/compile before the
     *  message thread has run the swap's own prepare(), and wins the lock. The
     *  render loop releases it (only create/compile/start/destroy are held, ~50-165
     *  ms measured), but for that window a live swap stalls, so the new sound is
     *  audible that much later. Do not call this on a path where that delay is not
     *  acceptable.
     *
     *  Voice 1 only (channel 0 of the 16-channel bus). Gate opens at t=0 with a
     *  single trigger and closes at `gateOffSeconds`, so a self-decaying key gets
     *  its tail inside the window and a standing tone gets a clean release.
     *
     *  Peak-normalised to -12 dBFS, because an absolute level would confound a
     *  learned ear that is level-sensitive. A render whose peak never leaves the
     *  noise floor is returned EMPTY rather than amplified: "the oscillator made
     *  no sound" is a finding, and normalising silence would hide it behind
     *  amplified numerical dust. Both run AFTER any decimation, so with
     *  decimateBy > 1 they judge the band the caller is given: an orchestra whose
     *  entire output sits above that band's Nyquist reports as no sound, which for
     *  a listener is exactly what it is.
     *
     *  @param decimateBy  1 (default), 2 or 4. The orchestra is rendered at
     *          `sampleRate` (clamped to kMaxProbeSampleRate) — an authored body may
     *          derive its partial count or FM index from `sr`, so it must see the
     *          rate the engine compiles it at —
     *          and the result is then band-limited and decimated by this factor
     *          through the SAME halfband stages renderUpTo runs on the live signal.
     *          Pass the LRO oversampling factor to get back exactly the band a
     *          player hears; leave it at 1 to get the raw render. A ratio the render
     *          length is not a whole multiple of leaves the render untouched.
     *
     *  @return interleaved-free mono samples at `sampleRate / decimateBy`; EMPTY on
     *          a compile error, a contract violation (ksmps/nchnls/MYFLT), a silent
     *          render, or in a build without Csound. Never throws.
     *
     *  LIVE AGAIN (BJ 2026-07-28): this offline probe was written for the
     *  self-listen / compare loop, which stays switched OFF (PromptPanel.cpp
     *  T5YNTH_LCO_SELFCHECK = 0) — but the LCO Re-Prompt ear now renders through
     *  it on every STEP (PromptPanel::triggerDcoReprompt). Hearing the sound is
     *  not judging it: no comparer and no correction pass came back with it. */
    static std::vector<float> renderBareOscillator (const std::string& orchestraText,
                                                    double sampleRate     = 48000.0,
                                                    double freqHz         = 220.0,
                                                    double seconds        = 3.0,
                                                    double gateOffSeconds = 2.5,
                                                    int    decimateBy     = 1);

private:
    struct Impl;                 // owns CSOUND*, spout ptr, channel MYFLT* [16][6], FIFO carry
    std::unique_ptr<Impl> impl;  // null in the stub build
};

// Build-config safety default: every real TU gets T5YNTH_HAS_CSOUND=0-or-1
// from CMake's always-defined compile definition (JUCE_USE_CURL precedent,
// CMakeLists.txt), but a standalone TU that forgot to pass it should fall
// back to the safe (inert) stub rather than silently treating an undefined
// macro as 0 via -Wundef-style preprocessor arithmetic.
#ifndef T5YNTH_HAS_CSOUND
#define T5YNTH_HAS_CSOUND 0
#endif

#if !T5YNTH_HAS_CSOUND

// Inert stub: framework not found at configure time (or on this platform).
// Empty Impl keeps the pimpl's class layout identical across both build
// configurations — CsoundEngine.cpp defines the real Impl and is only
// compiled into the target when CMake found the framework.
struct CsoundEngine::Impl {};

inline CsoundEngine::CsoundEngine() = default;
inline CsoundEngine::~CsoundEngine() = default;

inline bool CsoundEngine::prepare (double, int, const char*, int) { return false; }
inline int  CsoundEngine::effectiveOversampleFactor (double, int) { return 1; }
inline std::string CsoundEngine::loadedLibraryPath() { return {}; }
inline int  CsoundEngine::oversampleFactor() const { return 1; }
inline const std::string& CsoundEngine::orchestraText() const
{
    static const std::string kEmpty;
    return kEmpty;
}
inline bool CsoundEngine::isReady() const { return false; }
inline bool CsoundEngine::performanceHasEnded() const { return false; }
inline float CsoundEngine::outputTrim() const { return 1.0f; }
inline void CsoundEngine::setVoiceControls (int, const VoiceControls&) {}
inline void CsoundEngine::setGlobalControls (const float*) {}
inline void CsoundEngine::renderUpTo (int) {}
inline void CsoundEngine::startBlock (int) {}
inline void CsoundEngine::primeForTakeover (const float*, const float*) {}
inline std::vector<float> CsoundEngine::renderBareOscillator (const std::string&, double,
                                                              double, double, double, int)
{
    // Empty, not silence: callers distinguish "no Csound in this build" from
    // "the orchestra rendered nothing", and both are correctly "nothing to hear".
    return {};
}

inline const float* CsoundEngine::voiceBuffer (int) const
{
    // Fixed-size zero buffer, sized well above any realistic host block size
    // (JUCE/DAW blocks are typically <= 4096 samples). Documented bound: a
    // caller requesting more than kStubZeroBufSize samples from this stub
    // reads past the array. Not expected in practice — the real (non-stub)
    // engine sizes its buffers exactly to the maxBlockSize passed to
    // prepare(), and the stub's own prepare() always returns false, so no
    // well-behaved caller pulls audio from this stub in the first place.
    static constexpr int kStubZeroBufSize = 8192;
    static const float kZero[kStubZeroBufSize] = {};
    return kZero;
}

#endif // !T5YNTH_HAS_CSOUND
