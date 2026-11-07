#pragma once
#include <JuceHeader.h>
#include <memory>
#include <vector>
#include "signalsmith-stretch.h"

/**
 * Sample playback engine (ported from useSamplePlayer.ts).
 *
 * Features:
 *   - Forward loop, one-shot, ping-pong (reflection with turnaround fade)
 *   - Loop crossfade baked into the tail, gain law from the measured coherence
 *   - Seam search (LoopFinder): windowed splice mismatch + fade-region correlation
 *   - Ping-pong turnaround fade at runtime (forward continuation blended into the reversal)
 *   - Signal-aware normalization (sustained vs. transient vs. hot/silent)
 *   - Fractional loop start/end ("brackets")
 *   - MIDI transposition via Signalsmith Stretch (pitch-preserving)
 *   - 6-tap Lanczos sinc interpolation for buffer reads
 *   - Retrigger (hard restart for non-legato)
 */
class SamplePlayer
{
public:
    enum class LoopMode { OneShot, Loop, PingPong };
    enum class PitchShiftQuality { Bypass, Efficient, Default, HighQuality };

    struct PrepareConfig
    {
        LoopMode loopMode = LoopMode::Loop;
        float crossfadeMs = 150.0f;
        bool normalizeOn = false;
        int loopOptimizeLevel = 0;
        float startPosFrac = 0.0f;
        float loopStartFrac = 0.0f;
        float loopEndFrac = 1.0f;
    };

    struct PreparedPlaybackState
    {
        juce::AudioBuffer<float> playBuffer;
        juce::AudioBuffer<float> firstPassBuffer;
        double bufferOriginalSR = 44100.0;
        int playStart = 0;
        int playEnd = 0;
        int coldStart = 0;
        bool audioLoaded = false;
        // PingPong turnaround fades (samples) and the mirror correlation that
        // shapes each — see readLoopedSample. 0 = hard reflection on that side.
        int   ppFadeStart = 0;
        int   ppFadeEnd   = 0;
        float ppCorrStart = 1.0f;
        float ppCorrEnd   = 1.0f;
        // Loop mode: which side the baked crossfade sits on (-1 tail, +1 head).
        // Decides at READ time which first pass may see the faded buffer.
        int   loopFadeSide = -1;
        // The mode this state was prepared under (its fades belong to it).
        LoopMode loopMode = LoopMode::Loop;
    };

    struct PreparedBufferLoad
    {
        juce::AudioBuffer<float> originalBuffer;
        PreparedPlaybackState playbackState;
    };

    SamplePlayer() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();

    /** Load audio data. Calls preparePlaybackBuffer() internally. */
    void loadBuffer(const juce::AudioBuffer<float>& buffer, double bufferSampleRate);

    /** Capture the settings which shape prepared playback data. */
    PrepareConfig capturePrepareConfig() const;

    /** Build immutable playback data off the audio thread. */
    PreparedBufferLoad prepareBufferLoad(const juce::AudioBuffer<float>& buffer,
                                         double bufferSampleRate,
                                         const PrepareConfig& config) const;

    /** Publish already-prepared playback data with matching config. */
    void applyPreparedBufferLoad(PreparedBufferLoad prepared, const PrepareConfig& config);

    /** Remove leading near-silence (sustained < ~-50 dB) from a source buffer.
     *  Idempotent — running on already-trimmed audio leaves it unchanged.
     *  Exposed so callers that compute start/loop fractions can pre-trim
     *  first, ensuring the fractions stay aligned with the buffer that
     *  prepareBufferLoad will later process. */
    void trimLeadingSilencePublic(juce::AudioBuffer<float>& buffer) const
    {
        trimLeadingSilence(buffer);
    }

    /** Remove a sustained near-silent TAIL (< ~-50 dB) from a source buffer.
     *  Symmetric counterpart to trimLeadingSilencePublic(): drops the dead
     *  post-content field that the granular/freeze engine (scan 0..1 across the
     *  whole buffer) would otherwise park in, and that the waveform display
     *  would show as flat tail. Idempotent; a generation whose content runs to
     *  the end is left unchanged. Call after the leading trim so the fractions
     *  computed downstream align with the buffer the engines ultimately play. */
    void trimTrailingSilencePublic(juce::AudioBuffer<float>& buffer) const
    {
        trimTrailingSilence(buffer);
    }

    /** Process a block (writes into output buffer). */
    void processBlock(juce::AudioBuffer<float>& output);

    /** Render one mono sample (channel 0, Lanczos sinc interpolation).
     *  Uses speed-based transposition (Bypass mode). Advances read position. */
    float processSample();

    /** Render a block of pitch-shifted mono samples.
     *  Uses Signalsmith Stretch for pitch-preserving transposition.
     *  Falls back to speed-based transposition in Bypass mode. */
    void renderPitchedBlock(float* output, int numSamples);

    bool hasAudio() const { return audioLoaded; }

    void play()  { playing = true; }
    void stop()  { playing = false; }

    /** Hard stop + restart from loop start. For non-legato MIDI retrigger. */
    void retrigger();

    /** Set transposition from MIDI note (60 = original pitch, 12-TET). */
    void setMidiNote(int note);

    /** Set transposition ratio directly (1.0 = original pitch).
     *  Use for microtuning: pass tunedHz(note) / tunedHz(60). */
    void setTransposeRatio(double ratio);

    /** Smooth pitch ramp to target semitones over durationMs (portamento, 12-TET). */
    void glideToSemitones(int semitones, float durationMs);

    /** Smooth pitch ramp to target ratio over durationMs (portamento, tuning-aware). */
    void glideToRatio(double targetRatio, float durationMs);

    /** Set pitch modulation factor (1.0 = no mod). Applied on top of transposeRatio
     *  in renderPitchedBlock. Use for envelope/LFO pitch modulation at block rate. */
    void setPitchModulation(float factor) { pitchModFactor = factor; }

    /** How far this note's pitch can be carried away from the sample's own
     *  pitch, in semitones — pitch bend plus everything routed to the pitch
     *  bus, at full scale. Push it before the note's first renderPitchedBlock;
     *  that block latches the render path for the whole note (see
     *  renderPitchedBlock). An UNDER-estimate is safe: the note then stays on
     *  the direct read, which is the chosen behaviour, not an artefact. */
    void setPitchModulationReach(float semitones)
        { pitchModReachSemis_ = std::abs(semitones); }
    void setSourceGain(float gain) { sourceGain_ = juce::jmax(0.0f, gain); }
    float getSourceGain() const { return sourceGain_; }

    bool isPlaying() const { return playing; }

    /** Current read position (samples into the playback buffer) and direction
     *  (+1 forward, -1 backward). Plain reads of audio-thread-owned state, for
     *  offline tools (tools/audition_loop_seams.cpp locates seams with them). */
    double getReadPosition() const { return readPosition; }
    int getPlayDirection() const { return playDirection_; }

    // ─── Loop region ("brackets") ───
    /** Set loop start as fraction of buffer (0.0–1.0). P2. Clamped against the
     *  CURRENT loop end, so the marker that moves cannot pass the one that
     *  stays. Right for a single edit, wrong for a stored pair — see
     *  setLoopRegion. */
    void setLoopStart(float frac);
    /** Set loop end as fraction of buffer (0.0–1.0). P3. Clamped against the
     *  current loop start, same as above. */
    void setLoopEnd(float frac);
    /** Set playback start position as fraction of buffer (0.0–1.0). P1. */
    void setStartPos(float frac);

    /** Move BOTH brackets at once (P2 and P3) — a preset's loop window, a
     *  history snapshot's, anything that arrives as a pair.
     *
     *  setLoopStart/setLoopEnd each clamp against the OTHER bracket's CURRENT
     *  value, so moving a region to the left means opening it first —
     *  setLoopEnd(1) → setLoopStart(new) → setLoopEnd(new) — and that
     *  intermediate 1.0 marks the buffer for a re-prepare even when the pair
     *  lands exactly where it started. A re-prepare republishes the snapshot,
     *  and every publication is something a held voice has to follow, so a
     *  no-op region move used to cost a full rebuild and a second crossfade
     *  (that is what made a SNAP recall audible). This clamps the PAIR once and
     *  marks a re-prepare only when a value actually moved.
     *
     *  And it is the only correct way to apply a STORED pair: clamping against
     *  the bracket currently held makes the result depend on what was loaded
     *  before, so the same file landed on a different window depending on which
     *  preset preceded it. Guard: tools/test_preset_loop_window_order.cpp. */
    void setLoopRegion(float startFrac, float endFrac);

    float getLoopStart() const { return loopStartFrac; }
    float getLoopEnd()   const { return loopEndFrac; }
    float getStartPos()  const { return startPosFrac; }

    /** True if P1/P2/P3 are locked (preset-preserved / user-adjusted).
     *  When set, auto-positioning on regeneration is skipped. Serialized
     *  as engine.pointsLocked in the preset JSON. */
    bool  getPointsLocked() const { return pointsLocked_; }
    void  setPointsLocked(bool v) { pointsLocked_ = v; }

    // ─── Wavetable extraction region (independent of Sampler P2/P3) ───
    void  setWtExtractStart(float frac) { wtExtractStartFrac_ = juce::jlimit(0.0f, 1.0f, frac); }
    void  setWtExtractEnd(float frac)   { wtExtractEndFrac_   = juce::jlimit(0.0f, 1.0f, frac); }
    float getWtExtractStart() const { return wtExtractStartFrac_; }
    float getWtExtractEnd()   const { return wtExtractEndFrac_; }

    // ─── Start position modulation offset (Scan→P1 in Sampler mode) ───
    void  setStartPosOffset(float v) { startPosOffset_ = v; }
    float getStartPosOffset() const  { return startPosOffset_; }

    /** Share playback buffer from a master player (for polyphonic voices).
     *  Shared-mode players have their own read position but read from the
     *  master's prepared buffer. loadBuffer/preparePlaybackBuffer are no-ops. */
    void shareBufferFrom(const SamplePlayer& master);

    /** Live-follow for an already-playing shared voice: equal-power crossfade
     *  from the current buffer to the master's freshly published snapshot over
     *  morphMs (the global Drift Crossfade time) — read position, direction and
     *  the per-voice noteOn normalization are preserved, only the underlying
     *  audio (and its region bounds) follow. This is how a HELD note plays the
     *  freshly generated sample during A/B-drift regenerate, click-free and
     *  honouring the Regen XFade control. Counterpart to
     *  WavetableOscillator::morphToFramesFrom and FreezeTextureEngine::
     *  morphToBufferFrom; morphMs<=0 collapses to an instant swap.
     *
     *  RT discipline: call ONLY on the audio thread (the thread that reads the
     *  snapshot). master.playbackSnapshot_ and this voice's own retiredSnapshot_
     *  are read/written here through the real atomic free-function API
     *  (atomic_load/store/exchange_explicit), not plain access, because
     *  getCallbackLock() does not cover every shipped format: the CLAP build
     *  calls processBlock with NO lock held at all (the clap-juce-extensions
     *  wrapper takes none — verified against clap-juce-wrapper.cpp: zero
     *  getCallbackLock references, processBlock called bare at both its call
     *  sites). The atomics are what actually excludes applyPreparedPlaybackState's
     *  off-thread republish and drainRetiredSnapshot's off-thread exchange, on
     *  every format including CLAP. getCallbackLock() (held by processBlock for
     *  its whole duration on Standalone/VST3/AU only, NOT on CLAP) is a second,
     *  redundant guard on those three formats and the real guard for this
     *  class's other, non-atomic state — see playbackSnapshot_'s declaration
     *  below for the full account. Call this off the audio thread and the swap
     *  races the publish/drain regardless of the atomics. The buffer being
     *  faded FROM is retained in
     *  morphFromSnapshot_; the snapshot it displaces is parked in a reclaim slot
     *  instead of being freed here, so the buffer free never lands on the audio
     *  thread — drainRetiredSnapshot() releases it off-thread. Pointer identity
     *  is the generation guard (same snapshot ⇒ no-op).
     *
     *  A newer snapshot arriving MID-crossfade drops whichever of the two
     *  sounding sides is quieter (below halfway that is the target, above it the
     *  fade-from) and restarts the ramp. Same rule as WavetableOscillator::
     *  beginMorphToMipData and FreezeTextureEngine::morphToBufferFrom; the
     *  definition carries the measured step at each point of the ramp, including
     *  the narrow band near halfway where it costs a little. This function always
     *  leaves playbackSnapshot_ holding the master's newest — shareBufferFrom()
     *  overwrites that pointer on the audio thread, so a voice must never be left
     *  the sole owner of a snapshot.
     *
     *  Consequence of the shared rule, worth knowing before wiring a new caller:
     *  while publications keep arriving FASTER than morphMs, each one restarts the
     *  ramp, so the kept fade-from stays dominant and the note does not walk
     *  forward through the generations — it lands on the newest buffer over one
     *  full morphMs after the stream stops. Guard case "publication stream" in
     *  tools/audition_sampler_follow.cpp pins that landing. */
    void morphToBufferFrom(const SamplePlayer& master, float morphMs);

    /** Release the reclaim slot populated by morphToBufferFrom(). MUST be called
     *  off the audio thread, sequenced before the master republishes its
     *  snapshot (frees the retired snapshot's buffers). The clear here is an
     *  atomic_exchange_explicit (acq_rel), not a plain reset — that is what
     *  excludes a concurrent morphToBufferFrom park on the audio thread on
     *  EVERY format, including CLAP, where processBlock runs under no lock at
     *  all (see playbackSnapshot_'s declaration below). Also take an explicit
     *  ScopedLock(getCallbackLock()) around the call: redundant with the atomic
     *  on CLAP, but the only guard against another off-thread caller on
     *  Standalone/VST3/AU, and against this object's other, non-atomic state. */
    void drainRetiredSnapshot();

    // ─── Modes and processing ───
    void setLoopMode(LoopMode mode);
    void setCrossfadeMs(float ms);
    void setNormalize(bool on);
    void setLoopOptimizeLevel(int level);   // 0=Off, 1=Low, 2=High

    LoopMode getLoopMode()  const { return loopMode; }
    float getCrossfadeMs()  const { return crossfadeMsVal; }
    bool  getNormalize()    const { return normalizeOn; }

    /** True if settings changed and buffer needs re-preparation. */
    bool needsReprepare() const { return needsReprepareFlag; }

    /** Re-build playBuffer from originalBuffer with current settings. */
    void preparePlaybackBuffer();

    /** Estimate mono RMS/peak over the untransposed P1/P2/P3 playback path.
     *  `gains` is typically a synthetic DCA envelope used only for analysis. */
    float estimatePlaybackRms(const float* gains, int numSamples, float* outPeak = nullptr) const;

    /** Estimated audible path length in output samples for one reference pass. */
    int estimateReferenceLengthSamples() const;

    /** One-line state dump for temporary diagnostics. */
    juce::String debugStateString() const;

    // ─── Pitch shift quality ───
    void setPitchShiftQuality(PitchShiftQuality quality);
    PitchShiftQuality getPitchShiftQuality() const { return pitchQuality; }

    // ─── Signal-aware normalization analysis (re-used externally for
    //     content classification, e.g. preset auto-tagging). ───
    enum class NormalizeMode { Bypass, PeakCap, Transient, Sustained };

    struct NormalizeAnalysis
    {
        float durationSeconds = 0.0f;
        float peak = 0.0f;
        float percentilePeak = 0.0f;
        float rms = 0.0f;
        float activeRms = 0.0f;
        float crestDb = 0.0f;
        float peakToPercentileDb = 0.0f;
        float activeRatio = 0.0f;
    };

    /** Analyze the audible region and choose a linear normalization mode. */
    NormalizeAnalysis analyzeNormalizeRegion(const juce::AudioBuffer<float>& buf,
                                             int regionStart,
                                             int regionEnd,
                                             double bufferSampleRate) const;
    static const char* normalizeModeName(NormalizeMode mode);
    NormalizeMode chooseNormalizeMode(const NormalizeAnalysis& analysis) const;
    /** Apply the sampler's signal-aware normalization to an external buffer region. */
    void normalizeBuffer(juce::AudioBuffer<float>& buf,
                         int regionStart,
                         int regionEnd,
                         double bufferSampleRate) const;

private:
    struct PlaybackSnapshot
    {
        juce::AudioBuffer<float> playBuffer;
        juce::AudioBuffer<float> firstPassBuffer;
        double bufferOriginalSR = 44100.0;
        // The loop geometry that belongs to THESE buffers travels with them:
        // a voice that acquires the snapshot pointer reads its loop points,
        // fades, gain laws and mode from here, never from the master's plain
        // fields, which a re-prepare on another thread may already have
        // moved on to the next buffer (the CLAP build runs processBlock with
        // no lock, so the plain fields can be torn or a generation ahead).
        int      playStart = 0, playEnd = 0, coldStart = 0;
        int      ppFadeStart = 0, ppFadeEnd = 0;
        float    ppCorrStart = 1.0f, ppCorrEnd = 1.0f;
        // Which side this buffer's loop fade sits on (-1 tail, +1 head): an
        // outgoing snapshot keeps being read by its own side during a morph,
        // not by the incoming buffer's (the side is geometric and can flip
        // with a Crossfade or marker change).
        int      loopFadeSide = -1;
        LoopMode loopMode = LoopMode::Loop;
        bool     audioLoaded = false;
    };

    // Original (unprocessed) buffer — kept for re-preparation when settings change
    juce::AudioBuffer<float> originalBuffer;
    // Prepared (crossfaded/palindromed/normalized) playback buffers.
    // playBuffer is used for steady-state looping; firstPassBuffer preserves
    // the linear source path until the first loop boundary has been crossed.
    juce::AudioBuffer<float> playBuffer;
    // On a MASTER instance: published by applyPreparedPlaybackState via
    // atomic_store_explicit (release), always under an explicit
    // ScopedLock(getCallbackLock()) too. On a VOICE instance: written by this
    // object's own morphToBufferFrom (atomic_store_explicit, release) or
    // shareBufferFrom (plain — voice-owned, see that function). Read via
    // atomic_load_explicit (acquire) in shareBufferFrom/morphToBufferFrom.
    // The atomics, NOT getCallbackLock(), are what makes this field safe on
    // every shipped format: the CLAP build calls processBlock with no lock
    // held at all (clap-juce-extensions' wrapper takes none — verified against
    // clap-juce-wrapper.cpp), so on CLAP the lock protects nothing here.
    // getCallbackLock() (held by processBlock for its whole duration on
    // Standalone/VST3/AU only) is a real, second guard on those three formats
    // — do not drop it trusting the atomics alone — but it is not sufficient
    // by itself, so do not drop the atomics trusting the lock alone either.
    std::shared_ptr<const PlaybackSnapshot> playbackSnapshot_;
    // Reclaim slot for morphToBufferFrom(): the audio thread MOVES the snapshot it
    // displaces from the crossfade here (never dropping the last reference on the
    // audio thread) so the std::vector free is deferred. drainRetiredSnapshot() —
    // off-thread, sequenced before the master republishes — releases it. Single
    // slot: the audio thread parks at most once per regenerate (the only event
    // that changes the master snapshot), and each regenerate drains first.
    // Accessed through atomic_load/store/exchange_explicit on both sides (the
    // audio-thread park in morphToBufferFrom, the off-thread drain in
    // drainRetiredSnapshot) — that pairing, not getCallbackLock(), is what
    // excludes the two from overlapping on CLAP, where processBlock holds no
    // lock. getCallbackLock() is still taken around the off-thread drain, and
    // still matters on Standalone/VST3/AU and against this class's other
    // plain state, but is not this field's guard on its own.
    std::shared_ptr<const PlaybackSnapshot> retiredSnapshot_;
    // The buffer a held voice is fading FROM during a Drift-Crossfade adopt
    // (morphToBufferFrom). Audio-thread-owned: written and read per-sample on the
    // audio thread (the morph runs there, unlike FreezeTextureEngine's off-thread
    // morph), so it needs no atomic. When a new morph supersedes it, the old
    // fade-from is moved into retiredSnapshot_ for off-thread release; on morph
    // completion it is retained and freed at the next park / reset.
    std::shared_ptr<const PlaybackSnapshot> morphFromSnapshot_;

    double playbackSampleRate = 44100.0;
    double bufferOriginalSR   = 44100.0;
    double readPosition       = 0.0;
    double transposeRatio     = 1.0;
    float  pitchModFactor     = 1.0f;  // block-rate pitch mod from envelopes/LFOs
    float  sourceGain_        = 1.0f;  // constant per-voice gain applied before stretch
    // Render path, decided ONCE at the note's first block and kept for the
    // whole note. See renderPitchedBlock for why it must not be re-decided.
    float  pitchModReachSemis_    = 0.0f;
    bool   pitchPathLatched_      = false;
    bool   pitchPathUsesStretch_  = false;
    // Held-note Drift-Crossfade adopt state (morphToBufferFrom). A held voice
    // equal-power crossfades from morphFromSnapshot_ to the new buffer over
    // morphMs (the global Drift Crossfade), so a regenerated sample swaps in
    // click-free. All audio-thread-owned (the morph is set up and advanced on the
    // audio thread): plain, no atomics. Dormant — costs one bool compare per
    // sample — when no crossfade is in flight.
    bool   morphActive_       = false; // true while a crossfade is ramping
    float  morphAlpha_        = 1.0f;  // 0 = all old buffer, 1 = all new buffer
    float  morphIncrement_    = 0.0f;  // per-source-sample ramp, 1/morphSamples
    double glideTargetRatio   = 1.0;
    double glideRatioIncr     = 0.0;  // per-sample increment
    int    glideSamplesLeft   = 0;
    bool   audioLoaded        = false;
    bool   playing            = false;

    // Loop region (fractions of original buffer length)
    float startPosFrac  = 0.0f;   // P1: playback start position
    float loopStartFrac = 0.0f;   // P2: loop begin
    float loopEndFrac   = 1.0f;   // P3: loop end
    bool  pointsLocked_ = false;  // true → Generate never touches P1/P2/P3

    // WT extraction region (independent of Sampler P2/P3)
    float wtExtractStartFrac_ = 0.0f;
    float wtExtractEndFrac_   = 1.0f;

    // Modulation offset for P1 (Scan→StartPos in Sampler mode)
    float startPosOffset_ = 0.0f;

    // Playback bounds in samples (within playBuffer)
    int playStart  = 0;
    int playEnd    = 0;
    int coldStart  = 0; // where a fresh share starts reading (= playStart)

    // PingPong turnaround fades, published with the region (applyPreparedPlaybackState)
    // and adopted with it (shareBufferFrom / morphToBufferFrom). Audio-thread reads.
    int   ppFadeStart_ = 0;
    int   ppFadeEnd_   = 0;
    float ppCorrStart_ = 1.0f;
    float ppCorrEnd_   = 1.0f;
    int   loopFadeSide_ = -1;

    /** Everything a read needs to know about WHERE a playhead is and what it
     *  last did, and about the buffer's loop geometry. The voice's own fields
     *  are these, spelled out as members (currentHead / currentGeometry pack
     *  them); the outgoing side of a held-note morph carries its own pair
     *  (morphTail_), because its loop points, fade side, mode and last
     *  reflection are the OLD buffer's and the shared position no longer
     *  wraps where that buffer's fade was baked. */
    struct Playhead
    {
        double pos       = 0.0;
        int    dir       = 1;
        bool   firstPass = true;
        double turnPos   = 0.0;    // last PingPong reflection: where ...
        int    turnFade  = 0;      // ... with which fade length (0 = none in flight)
        float  turnCorr  = 1.0f;   // ... and which gain law
    };
    struct LoopGeometry
    {
        int      playStart = 0, playEnd = 0;
        int      ppFadeStart = 0, ppFadeEnd = 0;
        float    ppCorrStart = 1.0f, ppCorrEnd = 1.0f;
        int      fadeSide = -1;
        LoopMode mode = LoopMode::Loop;
    };
    /** The outgoing side of a held-note morph: the old buffer keeps playing
     *  exactly as it did (its own position, direction, loop points, fades,
     *  mode) under the cos ramp, while the voice's own head carries on into
     *  the new buffer. Audio-thread-owned. */
    struct MorphTail
    {
        Playhead     head;
        LoopGeometry geo;
        bool         active = false;
    };
    MorphTail morphTail_;
    // The voice's own head has ended (a OneShot crossing, or it ran off the
    // material) while the outgoing side of a morph may still be sounding: the
    // voice then stays alive, silent on the incoming side, until the tail ends
    // or the ramp completes. Reset by retrigger().
    bool headEnded_ = false;
    // Level the fade-from side had when it became the fade-from (1 for a side
    // that was playing at full level). Its gain is fadeFromGain(morphFromGain_,
    // ramp): it starts at that level and recovers toward the plain cos law as
    // the ramp runs, so a side kept or promoted mid-crossfade carries on at
    // the level it has (no step) and a stream of publications does not drain
    // the held note.
    float morphFromGain_ = 1.0f;
    // Level the kept side recovers to: the level the whole voice had at the
    // publication (both sides' power), so a side that was quiet because the
    // note itself was fading stays on its fade, and a side that was quiet
    // only because the other side carried the rest comes back up.
    float morphRecoverTarget_ = 1.0f;
    // Ramp position at which that recovery is complete (10 ms into the
    // crossfade, or a quarter of a shorter crossfade).
    float morphRecoverAlpha_ = 1.0f;
    static float fadeFromGain(float startLevel, float target, float ramp, float recoverAlpha) noexcept;
    // Stretch path only: once the voice has ended, the stretcher still holds
    // one window of the sound; this many output samples are still rendered
    // from silent input so the end is the material's own, not a cut.
    int  stretchDrainLeft_ = 0;

    Playhead     currentHead() const;
    LoopGeometry currentGeometry() const;
    /** Take the loop geometry, mode and SR of a snapshot this voice has
     *  acquired (shareBufferFrom / morphToBufferFrom). */
    void adoptGeometry(const PlaybackSnapshot& snap);

    /** Loop mode, first pass: true when this pass must read the pristine copy.
     *  A forward pass plays THROUGH a tail fade (-1) and so reads the faded
     *  buffer, but must not pass through a head fade (+1); a backward pass
     *  (start beyond the loop end) is the mirror of that. Decided from the
     *  live direction, so a start position dragged or modulated across the
     *  loop end after prepare still reads the right buffer. */
    static bool firstPassReadsPristine(const Playhead& h, const LoopGeometry& g)
    {
        return h.firstPass && g.mode == LoopMode::Loop && ((h.dir > 0) == (g.fadeSide > 0));
    }
    bool firstPassReadsPristine() const { return firstPassReadsPristine(currentHead(), currentGeometry()); }

    /** One source-sample step of a playhead against a loop geometry: wrap,
     *  reflect (recording the reflection), or end a OneShot at a crossing.
     *  Returns false when the playhead has stopped. */
    static bool advanceHead(Playhead& h, const LoopGeometry& g, double speedMagnitude, int bufLen);

    // 3-point playback state (per-voice, reset on retrigger)
    bool inFirstPass_   = true;   // true until first loop boundary hit
    // The last PingPong reflection this voice made: where, with which fade
    // length and gain law. The turnaround fade is anchored on that EVENT, not
    // on whichever buffer is current, so a held note whose buffer (and loop
    // points) change mid-fade keeps the fade it was playing, and a reflection
    // after the change fades every sounding buffer around the same point.
    // Audio-thread-owned; reset by retrigger().
    double turnPos_  = 0.0;
    int    turnFade_ = 0;
    float  turnCorr_ = 1.0f;
    int  playDirection_ = 1;      // +1 forward, -1 backward

    // Settings
    LoopMode loopMode     = LoopMode::Loop;
    float crossfadeMsVal  = 150.0f;
    bool  normalizeOn     = false;
    int   loopOptimizeLevel = 0;   // 0=Off, 1=Low, 2=High

    // Dirty flag — owner must prepare off the realtime thread and publish
    // the resulting snapshot; render methods keep playing the previous state.
    bool needsReprepareFlag = false;

    // Shared-buffer mode: follows a published snapshot from another player.
    bool sharedMode = false;

    // ─── Pitch shifting (Signalsmith Stretch) ───
    signalsmith::stretch::SignalsmithStretch<float> stretcher;
    PitchShiftQuality pitchQuality = PitchShiftQuality::Default;
    bool stretcherPrepared = false;
    bool stretcherNeedsPriming = false;
    std::vector<float> rawReadBuf;
    // Pre-sized scratch buffers for primeStretcher() — keeps voice retriggers
    // off the heap. Sized in prepareStretcher() from the stretcher's reported
    // block/interval/input-latency. Capacity only grows; never shrinks.
    std::vector<float> primeSeekBuf;
    std::vector<float> primeInputBuf;
    std::vector<float> primeDiscardBuf;
    int maxBlockSize = 512;

    // ─── Catmull-Rom cubic interpolation (fast, high quality) ───

    /** 4-point Catmull-Rom cubic interpolation at fractional buffer position. */
    float cubicSample(double pos) const;
    float cubicSampleFrom(const juce::AudioBuffer<float>& buf, double pos) const;
    float playbackSample(double pos, bool useFirstPassBuffer) const;

    const juce::AudioBuffer<float>& currentPlaybackBuffer() const;
    const juce::AudioBuffer<float>& currentFirstPassBuffer() const;
    double currentBufferOriginalSR() const;

    /** Read raw samples at 1:1 speed (SR-corrected only, no transposition).
     *  Advances readPosition and handles loop wrapping. */
    void readRawSamples(float* output, int numSamples);

    /** Advance read position by speedMagnitude in current direction.
     *  Handles first-pass logic, loop wrapping, and direction reversal.
     *  Returns false if playback stopped (OneShot end). */
    bool advancePosition(double speedMagnitude);

    /** Initialize/reconfigure the Signalsmith Stretch instance. */
    void prepareStretcher();

    /** Prime the stretcher by feeding half-window of audio (output discarded).
     *  Eliminates STFT ramp-up latency so first real output sample is valid. */
    void primeStretcher();

    PreparedPlaybackState preparePlaybackState(const juce::AudioBuffer<float>& sourceBuffer,
                                               double sourceSampleRate,
                                               const PrepareConfig& config) const;
    void applyPreparedPlaybackState(PreparedPlaybackState preparedState);

    /** Bake the loop crossfade into the TAIL: x[loopEnd-fade .. loopEnd) becomes
     *  a fade from itself into the material that precedes the loop start,
     *  x[loopStart-fade .. loopStart), so the wrap lands on x[loopStart] as the
     *  original signal did. The gain law follows `correlation` (the measured
     *  coherence of the two regions): linear at 1, equal-power at 0 — see
     *  LoopFinder::crossfadeGains. The loop region itself is not shortened. */
    static void applyLoopCrossfade(juce::AudioBuffer<float>& buf, int loopStart, int loopEnd,
                                   int fadeSamples, int fadeSide, float correlation);


    /** playbackSample plus the PingPong turnaround fade: for the first
     *  ppFade* samples after a reflection the forward continuation beyond the
     *  boundary (x[E + tau]) is blended into the reversed playback (x[E - tau]),
     *  with gains shaped by the mirror correlation measured at prepare time.
     *  Time reversal negates every odd derivative; this is what removes the kink.
     *  Not applied during the first pass (no turnaround has happened yet). */
    float readLoopedSample(double pos, bool useFirstPassBuffer) const;

    /** One buffer read at `pos`: the boundary-aware cubic sample, plus the
     *  PingPong turnaround fade when the playhead is inside the last
     *  reflection's fade. readLoopedSample calls this for the current buffer
     *  and, during a held-note morph, for the outgoing snapshot, and blends
     *  the two. `pristine` = the first-pass copy (no boundary wrapping). */
    float loopedSampleFrom(const juce::AudioBuffer<float>& buf, bool pristine,
                           const Playhead& h, const LoopGeometry& g) const;

    /** Cubic read whose taps beyond a loop boundary come from where the
     *  playhead goes next (Loop: the other end; PingPong: the mirror image),
     *  instead of from guard samples written into the buffer. On the first
     *  pass only the boundary the playhead is moving toward wraps, so the
     *  attack before the loop and a reversed start beyond it play pristine. */
    float boundaryAwareSample(const juce::AudioBuffer<float>& buf, bool pristine,
                              const Playhead& h, const LoopGeometry& g) const;

    /** Cubic interpolation needs i1-1, i1, i1+1, i1+2 — at most 2 samples ahead and
     *  1 behind. We use 4 on each side to absorb overshoot at high transposition
     *  ratios (worst case ~ srRatio * transposeRatio per step). */

    float chooseNormalizeGain(const NormalizeAnalysis& analysis, NormalizeMode mode) const;

    void applyGainToRegion(juce::AudioBuffer<float>& buf,
                           int regionStart,
                           int regionEnd,
                           float gain) const;

    /** Remove leading near-silence (sustained < ~-50 dB) from a source buffer. */
    void trimLeadingSilence(juce::AudioBuffer<float>& buffer) const;

    /** Remove a sustained near-silent TAIL (< ~-50 dB) from a source buffer. */
    void trimTrailingSilence(juce::AudioBuffer<float>& buffer) const;

};
