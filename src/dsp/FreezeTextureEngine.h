#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

/**
 * Conservative freeze/texture playback engine.
 *
 * This is intentionally not an open granular spray engine. It uses a small
 * fixed set of time-scheduled grains around a held sample position, keeping
 * grain duration/density independent from pitch while staying close to the
 * source material.
 */
class FreezeTextureEngine
{
public:
    /** Immutable, published buffer snapshot. Public (moved up from the private
     *  section that used to hold it) so prepareBufferLoad's result can be held
     *  by a caller between the compute and publish steps — mirrors
     *  SamplePlayer::PreparedBufferLoad. */
    struct Snapshot
    {
        std::vector<float> samples;
        double sampleRate = 44100.0;
        juce::uint64 generation = 0;
    };

    using SnapshotPtr = std::shared_ptr<const Snapshot>;

    FreezeTextureEngine() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    void reset();

    /** Build AND publish a new snapshot in one call. Ends in an atomic_store_explicit
     *  (release) into publishedSnapshot_ (see its declaration below, and
     *  publishSnapshot()) — real atomics, not a lock, because getCallbackLock()
     *  does not cover every shipped format: the CLAP build calls processBlock
     *  with no lock held at all (verified against clap-juce-wrapper.cpp — zero
     *  getCallbackLock references). The one production call site
     *  (serviceSamplerReprepare) also happens to be inside an explicit
     *  ScopedLock, for OTHER state it touches; the other two production sites
     *  (loadGeneratedAudio, reloadProcessedAudio) use prepareBufferLoad()/
     *  applyPreparedBufferLoad() below instead, specifically to keep the
     *  expensive mixdown off the lock. A handful of offline tools-dir *.cpp
     *  harnesses also call this directly on a freshly constructed,
     *  single-threaded engine with no concurrent reader — safe there regardless
     *  of lock or atomics, because nothing else touches that instance. */
    void loadBuffer(const juce::AudioBuffer<float>& buffer, double bufferSampleRate);

    /** Off-lock compute phase of loadBuffer(): mixes `buffer` down to mono into
     *  a new Snapshot and returns it WITHOUT publishing. Safe to call from any
     *  thread while others read the CURRENT publishedSnapshot_ — the only shared
     *  state it touches is nextGeneration_, mutated here exactly as unguarded as
     *  loadBuffer() always mutated it (pre-existing, not a hazard introduced by
     *  this split). The allocation + per-sample mixdown is the expensive part,
     *  kept off getCallbackLock() so it never delays the audio thread's next
     *  processBlock (where that lock is even held — see applyPreparedBufferLoad).
     *  Empty/invalid `buffer` returns nullptr (matches loadBuffer's early-out).
     *  Pass the result to applyPreparedBufferLoad() to publish it. */
    SnapshotPtr prepareBufferLoad(const juce::AudioBuffer<float>& buffer, double bufferSampleRate);

    /** Publish phase. This is an atomic_store_explicit (release) into
     *  publishedSnapshot_, paired with the atomic_load_explicit (acquire) in
     *  loadPublishedSnapshot() — that pairing, not a lock, is what prevents a
     *  torn read / UAF against the audio-thread readers (hasAudio(),
     *  processSampleStereo()) on every shipped format. getCallbackLock() does
     *  NOT cover this on its own: processBlock holds it for its whole duration
     *  on Standalone/VST3/AU, but the CLAP build calls processBlock with no
     *  lock at all (verified against clap-juce-wrapper.cpp — zero
     *  getCallbackLock references, processBlock called bare). Call this under
     *  an explicit ScopedLock(getCallbackLock()) anyway — real and necessary on
     *  the other three formats, and for this class's non-atomic state — but do
     *  not treat the lock as sufficient by itself. Pass nullptr for an
     *  empty/invalid buffer. */
    void applyPreparedBufferLoad(SnapshotPtr snapshot);

    void shareBufferFrom(const FreezeTextureEngine& master);

    /** Live, click-free crossfade to the master's current buffer.
     *  Counterpart to WavetableOscillator::morphToFramesFrom: a held granular
     *  voice adopts a newly generated buffer by spraying both old and new
     *  snapshots and equal-power crossfading over `morphMs` (the global Drift
     *  Crossfade time). Generation-guarded — same buffer ⇒ no-op (never
     *  restarts an in-flight morph); `morphMs<=0` or no current buffer ⇒
     *  instant adopt. MUST be called off the audio thread (it may release the
     *  retained old snapshot); see distributeFreezeBuffer's allowMorph gate. */
    void morphToBufferFrom(const FreezeTextureEngine& master, float morphMs);

    bool hasAudio() const;

    void retrigger();
    void setPosition(float position);
    float getCurrentPosition() const { return currentPosition_; }

    void setTextureMode(int mode);
    void setTextureLengthMs(float ms);
    void setStereoWidth(float width);
    void setTransposeRatio(double ratio);
    void glideToRatio(double targetRatio, float durationMs);
    void setPitchModulation(float factor);

    float processSample();
    void processSampleStereo(float& left, float& right);

private:
    struct Grain
    {
        bool active = false;
        int ageSamples = 0;
        int durationSamples = 1;
        double readPosLeft = 0.0;
        double readPosRight = 0.0;
        double readStep = 1.0;
        float ampLeft = 1.0f;
        float ampRight = 1.0f;
    };

    static constexpr int kMaxGrains = 12;

    // A self-contained grain sprayer: a grain pool plus its spawn-scheduling
    // state. The engine runs one primary cloud; during a crossfade it also
    // runs a second "morph" cloud so the old and new buffers spray at once.
    // Trivially copyable (POD only) — handing the primary cloud to the morph
    // cloud at morph start is a plain value copy, safe on any thread.
    struct GrainCloud
    {
        std::array<Grain, kMaxGrains> grains {};
        int spawnSamplesUntilNext = 0;
        int nextGrainSlot = 0;
        int spawnIndex = 0;
    };

    struct TextureConfig
    {
        float defaultLengthMs = 260.0f;
        int overlap = 8;
        float blur = 0.35f;
        float motionMs = 10.0f;
        float spreadMs = 18.0f;
        float pitchCents = 2.0f;
    };

    // Atomic accessors for publishedSnapshot_/morphFromSnapshot_ — see the lock
    // contract on those members' declarations below. Real atomic_load/store_explicit
    // in all four, because getCallbackLock() does not cover the CLAP build (no
    // lock around processBlock there); the caller's getCallbackLock() (explicit
    // off-thread, ambient via processBlock on Standalone/VST3/AU) is a second
    // guard, real on those formats but not what makes every call site safe.
    SnapshotPtr loadPublishedSnapshot() const;
    void publishSnapshot(SnapshotPtr snapshot);
    SnapshotPtr loadMorphFromSnapshot() const;
    void publishMorphFromSnapshot(SnapshotPtr snapshot);

    void resetCloud(GrainCloud& cloud);
    TextureConfig getTextureConfig() const;
    void spawnGrain(GrainCloud& cloud, const Snapshot& snapshot, double effectiveRatio);
    void renderCloud(GrainCloud& cloud,
                     const Snapshot& snapshot,
                     double effectiveRatio,
                     int grainDurationSamples,
                     float& left,
                     float& right);
    void processGrain(Grain& grain,
                      const Snapshot& snapshot,
                      float& left,
                      float& right,
                      float& weight) const;
    float cubicSample(const Snapshot& snapshot, double position) const;
    int getGrainDurationSamples() const;
    int getNextHopSamples(const GrainCloud& cloud, int grainDurationSamples) const;

    // Published buffer state. Both go through the real atomic free-function API
    // (atomic_load/store_explicit, acquire/release) — see publishSnapshot() /
    // publishMorphFromSnapshot() / loadPublishedSnapshot() / loadMorphFromSnapshot()
    // above. Every write goes through publishSnapshot()/publishMorphFromSnapshot()
    // (loadBuffer, applyPreparedBufferLoad, reset, shareBufferFrom,
    // morphToBufferFrom); every read goes through loadPublishedSnapshot()/
    // loadMorphFromSnapshot() (hasAudio(), processSampleStereo(),
    // morphToBufferFrom(), shareBufferFrom()). Some of those run on the audio
    // thread, inside processBlock — locked by getCallbackLock() for its whole
    // duration on Standalone/VST3/AU, but NOT on CLAP: the clap-juce-extensions
    // wrapper calls processBlock with no lock held at all (verified against
    // clap-juce-wrapper.cpp — zero getCallbackLock references, processBlock
    // called bare). The atomics are what actually excludes the audio thread on
    // every format, CLAP included; the message thread and the
    // samplerReprepareThread worker ALSO take getCallbackLock() explicitly
    // around their writes, which is real and matters on the other three
    // formats and against this class's non-atomic state, but is not this
    // field's guard on its own. Without the atomics this is an unprotected
    // shared_ptr race on CLAP — torn reads and use-after-free, not a benign
    // data race.
    SnapshotPtr publishedSnapshot_;
    SnapshotPtr morphFromSnapshot_;   // old buffer retained during a crossfade; same lock discipline as publishedSnapshot_ above

    // Off-audio-thread reclaim bin. morphToBufferFrom overwrites the two snapshot
    // members on a voice the audio thread is actively rendering; the audio thread
    // holds a one-sample-lived local of each (processSampleStereo's `snapshot`/
    // `oldSnap`). To guarantee that local can never be the last reference (which
    // would free the std::vector<float> on the audio thread — CLAUDE.md #4), the
    // current snapshots are parked here before being overwritten and released only
    // at the top of the NEXT morphToBufferFrom (message thread). morph calls are at
    // least an inference apart — orders of magnitude longer than a one-sample local
    // — so by the next call no audio local can still alias them. The audio thread
    // NEVER reads these, so overwriting/clearing them never races an audio local.
    SnapshotPtr retiredPublished_;
    SnapshotPtr retiredMorphFrom_;

    juce::uint64 nextGeneration_ = 1;

    double playbackSampleRate_ = 44100.0;
    int maxBlockSize_ = 512;

    double transposeRatio_ = 1.0;
    double glideTargetRatio_ = 1.0;
    double glideRatioIncr_ = 0.0;
    int glideSamplesLeft_ = 0;
    float pitchModFactor_ = 1.0f;

    float targetPosition_ = 0.5f;
    float smoothedPosition_ = 0.5f;
    float currentPosition_ = 0.5f;
    float positionSmoothCoeff_ = 1.0f;
    float textureLengthMs_ = 260.0f;
    int textureMode_ = 1;
    float stereoWidth_ = 0.25f;

    // Crossfade-morph state (all morph work in processSampleStereo is gated on
    // morphActive_, so steady state costs nothing).
    //
    // morphActive_ is atomic and doubles as the publication gate for the whole
    // morph setup. morphToBufferFrom (message/reprepare thread) writes morphCloud_,
    // the snapshots, morphAlpha_ and morphIncrement_ and THEN stores morphActive_
    // with release; processSampleStereo (audio thread) loads it with acquire FIRST.
    // That happens-before edge is what makes the setup visible across the
    // message→audio hand-off on weakly-ordered cores (Apple Silicon) — on EVERY
    // format, not as belt-and-suspenders: processBlock holds getCallbackLock()
    // for its whole duration on Standalone/VST3/AU, but the CLAP build calls
    // processBlock with no lock at all (verified against clap-juce-wrapper.cpp —
    // zero getCallbackLock references), so on CLAP this release/acquire is the
    // ONLY guard, not a redundant one — do NOT drop it trusting the lock. Where
    // the lock IS in effect (Standalone/VST3/AU), it excludes the same two
    // sides again, redundantly; do not drop the atomic trusting the lock either.
    // On CLAP, the morphAlpha_/grain writes during a *re*-morph (morphActive_
    // already true) DO race — accepted as the same crash-safe, glitch-only POD
    // race Wavetable accepts (aligned fields, clamped reads, no pointers in
    // Grain), a real case on that format, not a hypothetical.
    std::atomic<bool> morphActive_ { false };
    float morphAlpha_ = 1.0f;       // 0 = all old buffer, 1 = all new buffer
    float morphIncrement_ = 0.0f;

    GrainCloud cloud_;        // primary cloud (the new/current buffer)
    GrainCloud morphCloud_;   // old buffer's cloud, only live during a crossfade
};
