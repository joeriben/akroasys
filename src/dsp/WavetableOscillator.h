#pragma once
#include <JuceHeader.h>
#include <memory>
#include <vector>
#include <array>
#include <cmath>
#include <atomic>
#include <limits>

/**
 * Mip-mapped wavetable oscillator.
 *
 * Ported from AI4ArtsEd's wavetable-processor.js + useWavetableOsc.ts.
 *
 * Architecture:
 * - Frames extracted from AI-generated audio via pitch-synchronous analysis
 * - 8 mip levels per frame (band-limited: 1024 → 8 harmonics)
 * - Phase accumulator with Catmull-Rom cubic interpolation between frames
 * - Scan position smoothing (one-pole lowpass, ~5ms)
 * - Mip level selection based on playback frequency
 *
 * Thread safety:
 * - Immutable MipData snapshots are published through the real atomic
 *   free-function API (publishedMipData_ — atomic_load/store_explicit,
 *   acquire/release), because getCallbackLock() does not cover every shipped
 *   format: the CLAP build calls processBlock with no lock held at all
 *   (verified against clap-juce-wrapper.cpp — zero getCallbackLock
 *   references). getCallbackLock() is a second, real guard on
 *   Standalone/VST3/AU (where processBlock holds it for its whole duration)
 *   and on this class's other, non-atomic state. The expensive part (frame
 *   extraction + per-level FFT) runs off the lock via the prepareXXX() family
 *   below; only the final atomic publish (applyPreparedMipData()) needs
 *   synchronizing, so the realtime path is never blocked for the compute.
 * - Shared-mode voices keep their own phase/scan state and can morph from
 *   one published bank generation to the next.
 */
class WavetableOscillator
{
public:
    static constexpr int FRAME_SIZE = 2048;
    static constexpr int HALF_FRAME = FRAME_SIZE / 2;
    static constexpr int NUM_MIP_LEVELS = 8;
    static constexpr int MIN_FRAMES = 8;

    // Largest additive-bank partial count synthesized in real time (per-voice
    // phase state is a fixed array of this size — no audio-thread allocation).
    static constexpr int MAX_ADDITIVE_PARTIALS = 64;

    // Largest number of index-aligned additive "stations" one bank can hold.
    // User-level recipes carry 1–5 stations (key-waves); character passes may
    // later insert perturbed sub-stations between them (spec §6.4), so the
    // internal cap is generous. Memory is trivial (K small vectors); per-voice
    // phase state is per-INDEX, not per-station, so it does not scale with K.
    static constexpr int MAX_ADDITIVE_SETS = 64;

    // One inharmonic partial for real-time additive synthesis. h is an ARBITRARY
    // ratio of the fundamental: an integer h is an ordinary harmonic, a NON-integer
    // h is a genuinely inharmonic partial (bell/metal/glass). A single looped
    // wavetable cycle is periodic on the fundamental and so can hold ONLY integer
    // harmonics — inharmonic partials therefore cannot be baked into a frame; they
    // are synthesized directly (setAdditiveBank), one running phase accumulator per
    // partial, wrapped mod 2*pi (click-free for any h).
    struct AdditivePartial { float h = 1.0f; float a = 0.0f; float phase = 0.0f; };

    /** Immutable snapshot of one published WT bank generation. Either a mip-mapped
     *  wavetable (isAdditive=false, the frames path) OR an inharmonic additive bank
     *  (isAdditive=true: partials synthesized in real time, frames unused). Public
     *  (moved up from the private section that used to hold it) so the prepareXXX()
     *  family's result can be held by a caller between the compute and publish
     *  steps — mirrors SamplePlayer::PreparedBufferLoad /
     *  FreezeTextureEngine::Snapshot. */
    struct MipData {
        std::vector<std::vector<std::vector<float>>> frames; // [level][frameIdx][sample]
        int numFrames = 0;
        int numLevels = 0;
        uint64_t generation = 0;

        bool isAdditive = false;                    // true: synthesize `partialSets`, ignore frames
        // K index-aligned additive stations (isAdditive only). ALL sets are EXACTLY
        // the same length N — index i is the SAME partial in every set (same running
        // phase accumulator, same Nyquist gate). Movement = per-index lerp of (a,h)
        // across the sets, blended by scanNow. K==1 degenerates to the former
        // single-set path (byte-identical math). phase is a per-index property, read
        // from set 0 (identical across sets by backend contract).
        std::vector<std::vector<AdditivePartial>> partialSets;
        float additiveGain = 1.0f;                  // 0.95 / max_over_sets(sum|a_i|), precomputed
    };

    using MipDataPtr = std::shared_ptr<const MipData>;

    WavetableOscillator() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();

    /** Extract wavetable frames from an audio buffer (pitch-synchronous or windowed).
     *  startFrac/endFrac define the extraction region as fraction of the buffer (0–1).
     *  Combined compute+publish convenience: ends in applyPreparedMipData()'s
     *  atomic_store_explicit into publishedMipData_ — real atomics, not a lock,
     *  since getCallbackLock() does not cover every shipped format (CLAP calls
     *  processBlock with no lock at all). Call this under an explicit
     *  ScopedLock(getCallbackLock()) anyway UNLESS nothing else can be touching
     *  this instance concurrently (a fresh, single-threaded tools-dir *.cpp
     *  harness — the only callers today besides the already-locked
     *  reextractWavetable): the lock is real and necessary on Standalone/VST3/AU
     *  and for this class's non-atomic state, even though the atomics alone
     *  cover the publish itself on every format. A caller that must NOT hold
     *  the lock across the extraction/FFT work uses
     *  prepareFramesFromBuffer()/applyPreparedMipData() below instead. */
    void extractFramesFromBuffer(const juce::AudioBuffer<float>& buffer, double bufferSampleRate,
                                 float startFrac = 0.0f, float endFrac = 1.0f,
                                 int maxFrames = 256);

    /** Off-lock compute phase of extractFramesFromBuffer(): identical extraction +
     *  pitch-analysis + mip-level FFT work, returned WITHOUT publishing. The
     *  expensive part is kept off getCallbackLock() so it never delays the audio
     *  thread's next processBlock. Returns nullptr on any early-out (matches
     *  extractFramesFromBuffer's original behaviour: leave the previous bank
     *  untouched rather than clear it). Pass the result to applyPreparedMipData()
     *  to publish it. */
    MipDataPtr prepareFramesFromBuffer(const juce::AudioBuffer<float>& buffer, double bufferSampleRate,
                                       float startFrac = 0.0f, float endFrac = 1.0f,
                                       int maxFrames = 256);

    /** Publish phase shared by the whole prepareXXX() family below (and by
     *  setAdditiveBank() internally). An atomic_store_explicit (release) into
     *  publishedMipData_, paired with the atomic_load_explicit (acquire) in
     *  loadPublishedMipData() — that pairing, not a lock, is what prevents a
     *  torn read / UAF against the audio-thread readers (hasFrames(),
     *  getNumFrames(), processSample(), and shareFramesFrom()/
     *  morphToFramesFrom() reading a MASTER's published data cross-instance)
     *  on every shipped format. getCallbackLock() does NOT cover this on its
     *  own: processBlock holds it for its whole duration on Standalone/VST3/AU,
     *  but the CLAP build calls processBlock with no lock at all (verified
     *  against clap-juce-wrapper.cpp — zero getCallbackLock references,
     *  processBlock called bare at both its call sites). Call this under an
     *  explicit ScopedLock(getCallbackLock()) anyway — real and necessary on
     *  the other three formats, and for this class's non-atomic state — but do
     *  not treat the lock as sufficient by itself. A nullptr snapshot is a
     *  no-op — every prepareXXX() early-out means "leave the previous bank
     *  untouched", not "clear it". */
    void applyPreparedMipData(MipDataPtr mipData);

    /** Set playback frequency in Hz. Cancels any active glide. */
    void setFrequency(float hz)
    {
        // NaN-safe non-negative clamp: !(hz >= 0) is true for negatives AND NaN.
        // Negative or NaN frequency would drive phase out of bounds in readMipSample.
        targetFrequency = (!(hz >= 0.0f)) ? 0.0f : hz;
        glideFreqSamplesLeft = 0;
    }
    float getFrequency() const { return targetFrequency; }

    /** True if a frequency glide is in progress. */
    bool isGliding() const { return glideFreqSamplesLeft > 0; }

    /** Smooth frequency ramp to target Hz over durationMs (portamento). */
    void glideToFrequency(float hz, float durationMs);

    /** Set scan control (0–1).
     *  Without auto-scan this is the absolute frame position.
     *  With auto-scan it acts as a forward bias on the temporal path. */
    void setScanPosition(float pos) { scanControl_ = juce::jlimit(0.0f, 1.0f, pos); }
    float getCurrentScanPosition() const { return juce::jlimit(0.0f, 1.0f, smoothedScan); }

    /** The EFFECTIVE scan position last read by processSample(), clamped [0,1].
     *  Equals getCurrentScanPosition() (kept as a separate cached sample-accurate
     *  read for the engine-window WT display's scan cursor). */
    float getEffectiveScanPosition() const { return lastScanNow_; }

    /** Enable/disable Catmull-Rom interpolation between frames. */
    void setInterpolation(bool enabled) { doInterpolate = enabled; }

    /** Set WT bank-morph time for live retargeting of held notes. */
    void setMorphTimeMs(float ms) { morphTimeMs_ = juce::jlimit(0.0f, 2000.0f, ms); }
    float getMorphTimeMs() const { return morphTimeMs_; }

    // ── Auto-scan (sampler-style temporal progression) ──────────────

    /** Enable auto-scan: scan position advances per sample at the original
     *  audio rate, independent of playback pitch. Sampling = wavetable + auto-scan. */
    void setAutoScan(bool on) { autoScan_ = on; }
    bool isAutoScan() const { return autoScan_; }

    /** Set auto-scan rate from original audio timing.
     *  Scan advances from 0→1 in (bufferLen / bufferSR) seconds. */
    void setAutoScanRate(double bufferSR, int bufferLen);

    /** Set auto-scan rate directly in Hz (full 0->1 sweeps per second), for
     *  callers that know a tempo rather than a source buffer's duration (the
     *  DCO/LCO recipe's motion_rate_hz). Drives the SAME autoScanIncr_ the
     *  transport above reads — there is only one scan-advance mechanism, so a
     *  DCO/LCO table's authored motion moves through it exactly like a neural
     *  buffer's auto-scan does. Equivalent to setAutoScanRate(rateHz, 1). */
    void setAutoScanRateHz(float rateHz);

    /** Set auto-scan loop region (frame fractions). */
    enum class LoopMode { OneShot, Loop, PingPong };
    void setAutoScanLoop(float startFrac, float endFrac, LoopMode mode);

    /** Set ONLY the traversal loop mode, without disturbing the loop brackets
     *  or rate. Lets the One-shot/Loop/Ping-pong buttons drive a DCO/LCO
     *  table live: its motion runs through this SAME auto-scan transport
     *  (loaded with its own rate/brackets once at DCO load time, via
     *  setAutoScanRateHz, rather than every block). */
    void setAutoScanLoopMode(LoopMode mode) { autoScanLoopMode_ = mode; }

    /** Set auto-scan start position (P1). Fraction 0–1. */
    void setAutoScanStartPos(float frac);
    float getAutoScanStartPos() const { return autoScanStartPos_; }

    /** Reset scan to start position (call on noteOn). */
    void retriggerAutoScan();

    /** Extract contiguous (non-pitch-synchronous) frames from audio buffer.
     *  For sampler-style playback where frames represent temporal chunks.
     *  Combined compute+publish convenience — same lock contract as
     *  extractFramesFromBuffer() above. */
    void extractContiguousFrames(const juce::AudioBuffer<float>& buffer, double bufferSR,
                                 float startFrac = 0.0f, float endFrac = 1.0f);

    /** Off-lock compute phase of extractContiguousFrames() — see
     *  prepareFramesFromBuffer()'s doc comment for the shared nullptr/publish
     *  contract. */
    MipDataPtr prepareContiguousFrames(const juce::AudioBuffer<float>& buffer, double bufferSR,
                                       float startFrac = 0.0f, float endFrac = 1.0f);

    /** Adopt a strip of pre-sliced single-cycle frames BIT-EXACTLY (DCO bakes:
     *  mono, N*FRAME_SIZE samples on exact frame boundaries). Unlike
     *  extractContiguousFrames there is NO seam ramp and NO per-frame renorm —
     *  the baker's closed-form cycles are already loop-exact per cycle, and
     *  both "corrections" audibly corrupt them (measured 0.43 max sample
     *  error on a pwm bake: the renorm re-levels every width step of the
     *  authored sweep). Same mip/publish path as extraction — combined
     *  compute+publish convenience, same lock contract as
     *  extractFramesFromBuffer() above. */
    void setExactFrames(const juce::AudioBuffer<float>& strip);

    /** Off-lock compute phase of setExactFrames() — see prepareFramesFromBuffer()'s
     *  doc comment for the shared nullptr/publish contract. */
    MipDataPtr prepareExactFrames(const juce::AudioBuffer<float>& strip);

    /** Publish an INHARMONIC additive spectrum as the bank: the voice synthesizes
     *  sum_i a_i * sin(2*pi * f0 * h_i * t + phase_i) in real time instead of reading
     *  baked frames, so non-integer h_i sound at their TRUE (inharmonic) frequencies
     *  — a bell/metal timbre a single looped cycle cannot represent. Same
     *  publish/share/morph path as setExactFrames, so held-note crossfade-follow
     *  works table<->additive and additive<->additive. Partials at/above Nyquist are
     *  dropped per-sample at playback (built-in anti-aliasing; no mip needed).
     *  Amplitudes are scaled by 0.95/sum|a_i| so the sum stays in range. A recipe
     *  whose partials are ALL integer h should still use setExactFrames (bit-exact
     *  baked path); this is only for the inharmonic case. */
    void setAdditiveBank(const std::vector<AdditivePartial>& partials);

    /** Publish K index-aligned additive "stations" as the bank (spec §3). All sets
     *  must have the SAME length N — index i is the SAME partial in every set (one
     *  shared running phase accumulator, one Nyquist gate). Movement is a per-index
     *  linear blend of (a,h) between the two stations bracketing the current scan
     *  position, using the SAME scanNow the wavetable path reads — no parameter
     *  trajectories, no new synthesis path. Gain is 0.95/max_over_sets(sum|a_i|)
     *  (bank-wide worst case, NOT per-position renorm — loudness must breathe). K==1
     *  degenerates EXACTLY to the single-set overload above. Sanitize rules mirror
     *  it: unequal lengths cap to the shortest, index i is dropped from ALL sets if
     *  ANY set's h there is non-finite or <= 0 (dropping per-set would break the
     *  alignment). Same atomic publish/share/morph path (applyPreparedMipData())
     *  as extraction — see extractFramesFromBuffer()'s
     *  doc comment for the lock contract; no production caller holds it today
     *  because none calls this yet (see loadPublishedMipData()'s comment). */
    void setAdditiveBank(const std::vector<std::vector<AdditivePartial>>& sets);

    /** Process a single sample. */
    float processSample();

    /** True if frames have been loaded. */
    bool hasFrames() const;

    int getNumFrames() const;

    /** Share mip-mapped frames from a master oscillator (for polyphonic voices).
     *  Shared-mode oscillators have their own phase/frequency/scan but adopt
     *  the master's latest published bank immediately. */
    void shareFramesFrom(const WavetableOscillator& source);

    /** Retarget a held voice towards the master's latest published bank. */
    void morphToFramesFrom(const WavetableOscillator& source);

    /** True if a live WT bank-morph is still in progress. */
    bool isMorphing() const { return morphActive_; }

    /** Snapshot the latest published level-0 frames as a flat buffer
     *  (numFrames × FRAME_SIZE floats, frame-major). Used by the
     *  Wavetable-WAV exporter — full bandwidth, no mip downsampling.
     *  Returns false if no frames have been published yet, or if the
     *  published bank's frame size disagrees with FRAME_SIZE. */
    bool snapshotLevel0Frames(std::vector<float>& outFlat,
                              int& outFrameSize,
                              int& outNumFrames) const;

    /** Snapshot the latest published additive bank (K index-aligned stations
     *  of AdditivePartial) plus its precomputed gain. Used by preset save to
     *  persist a real-time-additive DCO/LCO bake's exact station data.
     *  Returns false if the published bank is not additive (isAdditive==false)
     *  or no bank has been published yet. */
    bool snapshotAdditiveBank(std::vector<std::vector<AdditivePartial>>& outSets,
                              float& outGain) const;

private:
    struct PitchEstimate {
        float hz = -1.0f;
        float confidence = 0.0f;
    };

    // Published bank state. Accessed through the real atomic free-function API
    // (atomic_load/store_explicit, acquire/release) — see loadPublishedMipData()
    // and applyPreparedMipData() above. Every write goes through
    // applyPreparedMipData() (the prepareXXX() family and setAdditiveBank()),
    // adoptMipData(), beginMorphToMipData(), shareFramesFrom(),
    // morphToFramesFrom(); every read goes through loadPublishedMipData()
    // (hasFrames(), getNumFrames(), snapshotLevel0Frames(),
    // snapshotAdditiveBank(), processSample(), shareFramesFrom(),
    // morphToFramesFrom()). Some of those run on the audio thread, inside
    // processBlock — locked by getCallbackLock() for its whole duration on
    // Standalone/VST3/AU, but NOT on CLAP: the clap-juce-extensions wrapper
    // calls processBlock with no lock held at all (verified against
    // clap-juce-wrapper.cpp — zero getCallbackLock references, processBlock
    // called bare). The atomics are what actually excludes the audio thread on
    // every format, CLAP included; the message-thread writers ALSO take
    // getCallbackLock() explicitly, which is real and matters on the other
    // three formats and against this class's other, non-atomic state, but is
    // not this field's guard on its own.
    // `mutable` predates this conversion and is not required by it — no const
    // method writes this member; left as found.
    mutable MipDataPtr publishedMipData_;
    uint64_t nextPublishedGeneration_ = 0;

    // Shared mode: voice adopts new banks from a master oscillator.
    const WavetableOscillator* sharedSource_ = nullptr;
    MipDataPtr activeMorphMipData_;
    MipDataPtr targetMorphMipData_;
    float morphAlpha_ = 1.0f;
    float morphIncrement_ = 0.0f;
    bool morphActive_ = false;
    float morphTimeMs_ = 200.0f;

    // Real-time additive synthesis: per-voice running phase (radians) for each
    // partial INDEX of the active/target additive bank. Fixed size — never allocated
    // on the audio thread. Phase is per-index (shared across the K stations), so only
    // the first partialSets[0].size() (== N) entries are live. Advanced once per
    // sample (mod 2*pi) when the bank isAdditive.
    std::array<double, MAX_ADDITIVE_PARTIALS> activeAddPhase_{};
    std::array<double, MAX_ADDITIVE_PARTIALS> targetAddPhase_{};

    double sampleRate = 44100.0;

    // Phase accumulator
    double phase = 0.0;
    float targetFrequency = 440.0f;

    // Frequency glide state
    float glideFreqTarget = 440.0f;
    float glideFreqIncr = 0.0f;
    int   glideFreqSamplesLeft = 0;

    // Scan control + smoothed effective position
    float scanControl_ = 0.0f;
    float smoothedScan = 0.0f;
    float scanSmoothCoeff = 0.0f;

    bool doInterpolate = true;

    // Auto-scan state (per-voice, not shared)
    bool autoScan_ = false;
    double autoScanPos_ = 0.0;           // 0.0–1.0 current position
    double autoScanIncr_ = 0.0;          // per-sample increment
    float autoScanStartPos_ = 0.0f;      // P1: start position (fraction 0–1)
    float autoScanLoopStart_ = 0.0f;     // P2: loop begin (fraction 0–1)
    float autoScanLoopEnd_ = 1.0f;       // P3: loop end (fraction 0–1)
    LoopMode autoScanLoopMode_ = LoopMode::Loop;
    bool  autoScanInFirstPass_ = true;   // true until first loop boundary hit
    int   autoScanDirection_ = 1;        // +1 forward, -1 backward

    // Last EFFECTIVE scan (== smoothedScan, clamped [0,1]) — the frame actually
    // being read by the last processSample() call. Published for the
    // engine-window WT display's scan cursor via getEffectiveScanPosition();
    // functionally the same value as getCurrentScanPosition() today, kept as a
    // separate cached field since the two accessors serve different call sites.
    float  lastScanNow_ = 0.0f;

    // Atomic accessor for publishedMipData_ — see the lock contract on that
    // member's declaration above. Real atomic_load_explicit (acquire), because
    // getCallbackLock() does not cover the CLAP build (no lock around
    // processBlock there); the caller's getCallbackLock() (explicit off-thread,
    // ambient via processBlock on Standalone/VST3/AU) is a second, real guard
    // on those three formats, not what makes every production call site safe.
    MipDataPtr loadPublishedMipData() const;
    void syncSharedConfigFrom(const WavetableOscillator& source);
    void adoptMipData(MipDataPtr mipData, bool seedAdditivePhase = true);  // by value: see .cpp (reset aliasing)
    void beginMorphToMipData(const MipDataPtr& mipData);
    static float readMipSample(const MipData& mipData, int mipLevel, double phase,
                               float scanPosition, bool interpolate);

    // ── Real-time additive synthesis (inharmonic banks) ──
    // Seed dst[i] from the bank's per-index phase offsets (set 0; phase is per-index,
    // identical across stations). Fresh start / morph-in.
    void seedAdditivePhases(std::array<double, MAX_ADDITIVE_PARTIALS>& dst,
                            const MipData& mip) const;
    // Sum a_i*sin(phaseAcc_i) over in-band partials (>= Nyquist dropped), (a,h)
    // interpolated between the stations bracketing scanNow. Read-only. K==1: the
    // former single-set math, byte-identical.
    float synthAdditiveSample(const MipData& mip,
                              const std::array<double, MAX_ADDITIVE_PARTIALS>& phaseAcc,
                              float scanNow) const;
    // Advance each in-use partial phase by w * h_i(scanNow) (same station blend as the
    // sample sum), wrapped mod 2*pi. K==1: the former single-set advance, byte-identical.
    void advanceAdditivePhases(std::array<double, MAX_ADDITIVE_PARTIALS>& phaseAcc,
                               const MipData& mip, float scanNow) const;

    // Cached mip-level selector. log2/ceil only recomputed when targetFrequency changes.
    // NaN sentinel: any comparison with NaN is false in IEEE-754, so the first sample after
    // construction or prepare() always triggers a cache miss and recomputes from current state.
    float lastMipFreq_ = std::numeric_limits<float>::quiet_NaN();
    int   cachedMipRawCeil_ = 0;

    // FFT helpers for mip-level generation
    static void fft(std::vector<double>& re, std::vector<double>& im);
    static void ifft(std::vector<double>& re, std::vector<double>& im);
    // Private: called only by prepareFramesFromBuffer/prepareContiguousFrames/
    // prepareExactFrames above (their shared FFT compute engine). See its .cpp
    // doc comment for the off-lock/publish contract.
    MipDataPtr prepareMipLevels(const std::vector<std::vector<float>>& srcFrames);

    // Pitch detection (simplified YIN)
    static PitchEstimate analyzePitchWindow(const float* data, int length, double sr);
    static float detectPitch(const float* data, int length, double sr);
    static int nearestZeroCrossing(const float* data, int length, int pos, int maxSearch);
    static std::vector<float> extractResampledPeriod(const float* data, int totalSamples,
                                                     double start, double periodSamples);
    static double computeLoopBoundaryError(const std::vector<float>& frame);

    // Lanczos sinc interpolation for frame extraction
    static constexpr int SINC_KERNEL_A = 6;
    static float lanczosSample(const float* src, int srcLen, double pos);
};
