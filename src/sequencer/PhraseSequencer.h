#pragma once
#include "PhraseModel.h"
#include "../dsp/VoiceEvent.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

/**
 * Phrase sequencer — the scheduler around PhraseModel
 * (docs/PHRASE_SEQ_CONCEPT.md §5).
 *
 * Time runs in quarter notes: the host PPQ position when the host drives the
 * transport, otherwise quarters counted since start(). The bar index follows
 * from that position and the time signature, and everything the model plays
 * is a function of the bar index, so a locate plays what continuous playback
 * plays there (§3.9).
 *
 * Threads:
 *  - Audio thread: setConfig / setTimeSignature / keyOn / keyOff / start /
 *    stop / allNotesOff / processBlock. No allocation, no locks: `out` must
 *    have spare capacity (the processor reserves it).
 *  - Message thread: the stored-motif API (dice / keep / fromSteps / undo /
 *    setStoredMotif). The stored motif reaches the audio thread through a
 *    seqlock of relaxed atomic words; the audio thread tries once per block
 *    and keeps the previous motif when it catches a write in progress. The
 *    motif the audio thread plays goes back the same way, so Keep stores
 *    what was heard and not a message-thread recomputation of it.
 *  - GUI: the *ForGui atomics and readView().
 *
 * Strands: the main line is strand −1 (like the step sequencer, so the
 * arpeggiator and slide handling treat it as the lead); for Stab that is the
 * whole chord. The layer is 5..8.
 */
class T5ynthPhraseSequencer
{
public:
    T5ynthPhraseSequencer();

    void prepare(double sampleRate);

    // ── Audio thread ─────────────────────────────────────────────────────
    void setConfig(const PhraseModel::Config& cfg);
    /** Host time signature; 4/4 when the host reports none. */
    void setTimeSignature(int numerator, int denominator);

    /** Keys harmony (§3.3): the chord is the set of keys pressed since all keys
     *  were last up, its lowest key the root. A change applies at the next beat. */
    void keyOn(int note);
    void keyOff(int note);
    void clearKeys();

    void start();
    void stop();
    bool isRunning() const { return running; }

    /** Note-offs for every sounding note at `sampleOffset`; clears all pending
     *  offs and held notes. Does not change running or position. */
    void allNotesOff(std::vector<VoiceEvent>& out, int sampleOffset = 0);

    /** Appends this block's events to `out`. `hostSync`: `hostPpq` is the
     *  block start position in quarters; otherwise the internal count runs. */
    void processBlock(int numSamples, double bpm, bool hostSync, double hostPpq,
                      std::vector<VoiceEvent>& out);

    /** Slide time: 25 % of a step, 10..200 ms (as the step sequencer). */
    float getGlideTime(double bpm) const;

    // ── Message thread: the stored motif ─────────────────────────────────
    /** Publishes `m` as the stored motif (no undo entry; preset / state load). */
    void setStoredMotif(const PhraseModel::Motif& m);
    /** The stored motif; its origin is kOriginAtNextEntry until it was heard.
     *  Callable from any thread (state save). */
    PhraseModel::Motif getStoredMotif() const;

    /** New seed, new motif from it. Origin: the phrase now playing, or the
     *  phrase where playback next starts when the transport is stopped. */
    void dice(const PhraseModel::Config& cfg, std::uint32_t newSeed);
    /** Stores the motif heard in the phrase now playing (or last played) as
     *  the new motif, so playback goes on unchanged; after a stop it starts
     *  from the phrase where playback resumes. A phrase before the stored
     *  motif's origin played the motif itself: Keep keeps that origin,
     *  running or stopped. Does nothing while the stored motif has not been
     *  heard yet: it is then itself what would be kept. */
    void keep();
    /** Stores the motif analysed from step-sequencer steps; origin as dice. */
    void fromSteps(const PhraseModel::StepInput* steps, int count, const PhraseModel::Config& cfg);
    /** One level: returns to the motif before the last dice / keep / fromSteps. */
    bool undo();
    bool canUndo() const { return hasUndo; }

    // ── GUI ──────────────────────────────────────────────────────────────
    std::atomic<int> currentStepForGui { -1 };    // step of the bar's grid, −1 stopped
    std::atomic<int> currentBarForGui { 0 };
    std::atomic<int> currentPhraseForGui { 0 };
    std::atomic<std::uint32_t> chordForGui { 0 };   // rootPc | iv1+1 << 4 | iv2+1 << 9 | iv3+1 << 14
    /** Set on every bar entry in continuous playback (drift-regen sync). */
    std::atomic<bool> barStartFlag { false };
    /** Bumped whenever the published bar view changes. */
    std::atomic<std::uint32_t> viewGeneration { 0 };

    /** The rendered bar's steps as the phrase view shows them. Returns the
     *  number of steps. Tear-tolerant: a step read mid-update is redrawn on
     *  the next generation. */
    int readView(std::array<PhraseModel::StepView, PhraseModel::kMaxBarSteps>& out) const;

private:
    // ── Motif hand-off ───────────────────────────────────────────────────
    /** A motif plus two tag words behind a seqlock of relaxed atomic words.
     *  One writer thread, one reader thread; a read that catches a write in
     *  progress fails and is retried by the caller. */
    class MotifChannel
    {
    public:
        MotifChannel();
        void write(const PhraseModel::Motif& m, std::uint32_t tagA, std::uint32_t tagB);
        bool tryRead(PhraseModel::Motif& m, std::uint32_t& tagA, std::uint32_t& tagB,
                     std::uint32_t& seqRead) const;
        /** The sequence number; even when no write is in progress. */
        std::uint32_t sequence() const { return seq.load(std::memory_order_relaxed); }
    private:
        static constexpr int kWords = PhraseModel::kMaxMotifSteps + 9;
        std::atomic<std::uint32_t> seq { 0 };
        std::array<std::atomic<std::uint32_t>, kWords> words;
    };
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(std::atomic<int>::is_always_lock_free);

    /** Message → audio: the stored motif. Tag A: 1 when the bar playing
     *  stands (Keep): the audio thread adopts the motif without re-rendering
     *  it. Tag B unused. */
    MotifChannel toAudio;
    std::uint32_t toAudioSeen = 0;       // audio thread: toAudio sequence of liveMotif
    void publish(const PhraseModel::Motif& m, bool barStands = false);

    /** Audio → message: the motif heard in the current phrase (evoCur). Tag A
     *  = the toAudio sequence it was derived from, tag B = its phrase. */
    MotifChannel heard;
    void publishHeard();
    /** The heard motif, if it was derived from the stored motif of toAudio
     *  sequence `storedSeq`. */
    bool readHeard(PhraseModel::Motif& m, int& phrase, std::uint32_t storedSeq) const;

    /** The phrase the motif heard now belongs to (audio thread writes);
     *  kOriginAtNextEntry while stopped. */
    std::atomic<int> editOrigin { PhraseModel::kOriginAtNextEntry };

    // Message-thread copies
    PhraseModel::Motif stored, undoMotif;
    bool hasUndo = false;
    /** A stored motif published as kOriginAtNextEntry gets its origin when it
     *  is first heard. The stored origin with that applied, and the adoption
     *  of it before every edit, so Undo republishes a motif as it was heard
     *  rather than anchoring it anew. */
    int resolvedStoredOrigin() const;
    void adoptResolvedOrigin();

    // ── Audio-thread state ───────────────────────────────────────────────
    double sampleRate = 44100.0;
    bool running = false;
    PhraseModel::Config cfg;
    bool cfgValid = false;
    int tsNum = 4, tsDen = 4;
    double barLenQ = 4.0;

    PhraseModel::Motif liveMotif;        // stored motif as the audio thread knows it
    PhraseModel::Motif fitted;
    bool motifDirty = false;             // the heard motif may differ: refit, rebuild evolution
    bool renderDirty = false;            // the bar renders differently: re-render it
    float latchedEvolve = 0.0f;          // Evolve as of the current phrase entry

    // Heard motifs of the phrases around the current bar
    PhraseModel::Motif evoPrev, evoCur, evoNext;
    int evoPhrase = 0;
    bool evoValid = false;
    void rebuildEvolution(int phrase);
    void advanceEvolution(int phrase);
    PhraseModel::Config latchedConfig() const;

    // Keys
    std::uint16_t keysMask = 0;          // pitch classes of the current latch
    int keysLowest = 128;
    int keysDown = 0;
    std::array<std::uint8_t, 128> keyIsDown {};
    PhraseModel::Chord keysChord {};     // in effect
    PhraseModel::Chord keysPending {};
    bool keysChanged = false;
    bool keysEverSet = false;
    PhraseModel::Chord currentKeysChord() const;

    // Transport
    double posQ = 0.0;                   // internal position (quarters)
    double expectedQ = 0.0;              // where the next block should start
    bool positioned = false;             // false → (re)enter at the next block
    bool lastHostSync = false;

    // Current bar
    int bar = 0;
    double barStartQ = 0.0;
    PhraseModel::BarRender render, scratch;
    int nextEvent = 0;                   // index into render.events
    int entryP = -1, entryPP = -1;       // Keys: the live line at the bar entry
    std::array<int, PhraseModel::kMaxVoices> entryVoicing {};
    int entryVoicingCount = 0;

    // Sounding notes
    struct PendingOff { double q; int note; int strand; };
    static constexpr int kMaxPending = 128;
    std::array<PendingOff, kMaxPending> pending {};
    int pendingCount = 0;
    static constexpr int kStrandSlots = 10;   // strands −1..8
    std::array<int, kStrandSlots> heldNote {};
    static int slotOf(int strand) { return strand + 1; }

    // GUI view
    std::array<std::atomic<std::uint32_t>, PhraseModel::kMaxBarSteps> viewWords;
    std::atomic<int> viewSteps { 0 };
    void publishView();

    void enterBar(int newBar, bool continuous);
    void renderCurrent();
    void releaseUnusedHeld(int fromIndex, std::vector<VoiceEvent>& out, int offset);
    /** Another event of the same strand starts at render.events[index]'s onset. */
    bool sharesOnset(int index) const;
    void emitOn(int index, std::vector<VoiceEvent>& out, int offset);
    void emitOff(int note, int strand, std::vector<VoiceEvent>& out, int offset);
    void flushPendingFor(int note, int strand, std::vector<VoiceEvent>& out, int offset);
    void addPending(double q, int note, int strand, std::vector<VoiceEvent>& out, int offset);
};
