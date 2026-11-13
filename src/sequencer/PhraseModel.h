#pragma once
#include <array>
#include <cstdint>
#include <limits>

/**
 * Phrase sequencer — the pure model (docs/PHRASE_SEQ_CONCEPT.md §3).
 *
 * Everything in here is a function of its arguments: no state, no allocation,
 * no locks, no JUCE. The audio thread (T5ynthPhraseSequencer) and the message
 * thread (phrase view, Dice / Keep / From Step) call the same functions, so
 * what the GUI shows and what the audio thread plays cannot drift apart.
 *
 * Determinism (§3.9): the notes of bar b are a function of (stored motif,
 * its seed, the parameters, b). Every random draw is a hash of the seed and a
 * position, never an RNG stream.
 *
 * Positions are in quarter notes; a 4/4 bar is 4.0. The motif lives on a 4/4
 * grid of `stepsPerBar` steps measured from each bar start; a shorter host bar
 * truncates it, a longer one leaves the tail silent (§6, non-4/4 is open).
 */
namespace PhraseModel
{

static constexpr int kMaxBarSteps   = 16;              // 1/16 grid
static constexpr int kMaxMotifSteps = 2 * kMaxBarSteps; // Len 2
static constexpr int kMaxVoices     = 4;               // R 3 5 7
static constexpr int kMaxEvents     = 256;             // per rendered bar

enum class Part : int { Bass = 0, Riff, Lead, Stab };
enum class Harm : int { Pedal = 0, Keys, Chords };
enum class Form : int { Loop = 0, AAAB, AABA, ABAB };
enum class Layer : int { Off = 0, Octave, Chord, Answer };

/** What a step IS (§3.4). Approach directions name the motion INTO the
 *  target: ApproachUp comes from the scale step below T and rises into it. */
enum class Fn : std::uint8_t
{
    Rest = 0,
    ChordTone,
    Repeat,
    Octave,
    Passing,
    NeighbourUp,
    NeighbourDown,
    ApproachUp,
    ApproachDown,
    ApproachChromatic,
    Count
};

enum Member : std::uint8_t { Root = 0, Third = 1, Fifth = 2, Seventh = 3 };

struct Slot
{
    Fn           fn     = Fn::Rest;
    std::uint8_t member = Root;     // ChordTone only
    std::int8_t  octave = 0;        // ChordTone only, −1..+1
    bool         accent = false;
    bool         slide  = false;    // forced slide (From Step); generated motifs leave it off
};

/** The stored asset. Besides the slots it records the shape it was made for;
 *  when the parameters still match that shape the slots play verbatim, and
 *  when they do not, fitMotif() derives the heard motif without touching it. */
struct Motif
{
    std::array<Slot, kMaxMotifSteps> slots {};
    int           len         = 0;    // bars, 1 | 2; 0 = empty record (fits nothing)
    int           stepsPerBar = 16;   // 1 2 4 8 16
    int           pulses      = 4;
    int           rotation    = 0;
    Part          part        = Part::Bass;
    std::uint32_t seed        = 1;
    int           origin      = 0;    // phrase index evolution counts from (§3.8)

    /** Pedal / Chords: the main line's last two notes before the origin
     *  phrase, kept with the motif (Keep). A phrase before the origin plays
     *  the motif itself, so the origin phrase's first bar would otherwise
     *  enter from the motif's own last bar rather than from what preceded it
     *  when it was heard. Off: it enters from the phrase before (entryNotes). */
    bool          entryKept     = false;
    int           entryNote     = -1;   // −1: no note, as entryNotes reports it
    int           entryPrevNote = -1;

    int numSteps() const { return len * stepsPerBar; }
};

/** Motif::origin of a motif stored while the transport is stopped: it starts
 *  evolving from the phrase where it is first heard, which the scheduler
 *  fills in when the next start reads the motif (§3.8). Where playback
 *  resumes is the host's choice, so it cannot be known earlier. */
static constexpr int kOriginAtNextEntry = std::numeric_limits<int>::min();

/** Every parameter the model reads. Filled from APVTS once per block. */
struct Config
{
    Part  part        = Part::Bass;
    Harm  harm        = Harm::Pedal;
    std::array<int, 4> chordDegrees { 1, 0, 0, 0 };  // 0 = empty slot, 1.. = scale degree
    int   chordRateBars = 1;                        // 1 | 2 | 4
    bool  ext7        = false;
    int   len         = 1;
    int   stepsPerBar = 16;
    int   pulses      = 4;
    int   rotation    = 0;
    int   phraseBars  = 8;                          // 4 | 8 | 16
    Form  form        = Form::AAAB;
    float variation   = 0.0f;
    float evolve      = 0.0f;
    float slide       = 0.0f;
    Layer layer       = Layer::Off;
    int   scaleType   = 2;                          // ScaleQuantizer::Scale (Minor)
    int   scaleRoot   = 9;                          // pitch class (A)
    float gate        = 0.8f;                       // 0.1..1
    float shuffle     = 0.0f;                       // 0..0.75
    int   transpose   = 0;                          // Seq Octave, semitones

    bool operator==(const Config& o) const;
    bool operator!=(const Config& o) const { return !(*this == o); }
};

/** A chord: root pitch class plus the interval above it for R 3 5 7
 *  (−1 = the member is absent). */
struct Chord
{
    int rootPc = 0;
    std::array<int, kMaxVoices> iv { 0, -1, -1, -1 };
    bool operator==(const Chord& o) const { return rootPc == o.rootPc && iv == o.iv; }
};

// ─── Small helpers ──────────────────────────────────────────────────────────

int floorDiv(int a, int b);
int floorMod(int a, int b);

/** Uniform [0,1) from a hash of the seed and up to three positions. */
float hash01(std::uint32_t seed, int a, int b = 0, int c = 0);
std::uint32_t hashU32(std::uint32_t seed, int a, int b = 0, int c = 0);

/** Lerdahl/Jackendoff metric weight on the 16th grid: trailing zero bits,
 *  w(0) = 4 (§3.1). */
int metricWeight16(int p16);

/** Steps per 4/4 bar for the shared Division index (0 = 1/1 … 4 = 1/16). */
int stepsPerBarForDivision(int division);

/** Absolute pitch-class mask of the shared scale. */
std::uint16_t scaleMask(int scaleType, int scaleRoot);

/** The next scale tone strictly above (dir > 0) or below (dir < 0) `note`. */
int scaleStep(int note, int dir, std::uint16_t mask);
/** `n` scale steps from `note` (n may be negative). */
int scaleSteps(int note, int n, std::uint16_t mask);
/** Scale steps between two notes (|count| of scale tones crossed). */
int scaleDistance(int a, int b, std::uint16_t mask);

// ─── Harmony (§3.3) ─────────────────────────────────────────────────────────

/** Number of degrees of a scale; chromatic counts as 12. */
int degreeCount(int scaleType);
bool chordsAvailable(int scaleType);   // false for chromatic (Off)

/** Chord on a 0-based scale degree: stacked alternate steps for heptatonic
 *  scales, tertian bands for the others, R + 5 for chromatic. */
Chord chordForDegree(int degree0, int scaleType, int scaleRoot, bool ext7);

/** Chord from held keys: lowest key is the root; held pitch classes fill the
 *  tertian bands, leftovers fill empty members (sus4 → 4 as third). */
Chord chordFromKeys(int lowestNote, std::uint16_t pcMask);

/** The chord-loop slots in play, in order; returns their count (0 → Pedal). */
int activeChordSlots(const Config& cfg, std::array<int, 4>& degrees0);

/** Chord sounding in `bar` for the Pedal and Chords sources. */
Chord chordForBar(const Config& cfg, int bar);

/** Roman-numeral description of a degree's chord for the UI. */
struct ChordLabel
{
    int  numeral    = 1;   // 1..7
    int  accidental = 0;   // −1 flat, +1 sharp
    bool minor      = false;
    bool diminished = false;
    bool augmented  = false;
    bool noThird    = false;
};
ChordLabel chordLabel(int degree0, int scaleType, bool ext7);

// ─── Register ───────────────────────────────────────────────────────────────

int partAnchor(Part part);
/** Tonic note of the key inside [anchor, anchor + 11]. */
int tonicNote(int anchor, int scaleRoot);
/** Root note of a chord, placed nearest to the tonic: [tonic − 5, tonic + 6]. */
int chordRootNote(const Chord& c, int tonic);
/** A chord member at an octave; missing members fall back (7 → R+12,
 *  3 → 5 → R, 5 → 3 → R). */
int chordToneNote(const Chord& c, int member, int octave, int tonic);

/** Close-position voicing of `c` whose lowest note lies in
 *  [anchor − 6, anchor + 6], chosen for least movement from `prev` (or nearest
 *  to the anchor when prevCount == 0). Returns the voice count. */
int voiceChord(const Chord& c, int anchor, const int* prev, int prevCount, int* out);

// ─── Motif (§3.2, §3.6, §3.8) ───────────────────────────────────────────────

/** Head flags of a motif: per span (beat / half bar / every pulse) the pulse
 *  of highest metric weight, ties to the longer duration, then the earlier. */
void computeHeads(const Motif& m, std::array<bool, kMaxMotifSteps>& heads);

/** The motif the parameters ask for. Returns `stored` unchanged when its shape
 *  matches cfg; otherwise re-fits it to the cfg's Euclid rhythm, keeping every
 *  slot whose time position is still a pulse in the same role. Non-destructive:
 *  the stored motif is never modified. */
Motif fitMotif(const Motif& stored, const Config& cfg);

/** A fresh motif for `seed`, origin `origin`. */
Motif generateMotif(std::uint32_t seed, int origin, const Config& cfg);

/** The Answer layer's motif: same rhythm as `main`, functions drawn from a
 *  derived seed. */
Motif answerMotif(const Motif& main, const Config& cfg);

/** Applies phrase `phrase`'s evolution changes in place (§3.8). */
void evolveOnce(Motif& m, const Config& cfg, int phrase);

/** The heard motif at `phrase`: fit, then the evolution changes of every
 *  phrase in (origin, phrase]. Before origin the fitted motif plays, and so it
 *  does for an origin of kOriginAtNextEntry. */
Motif evolvedMotif(const Motif& fitted, const Config& cfg, int phrase);

/** True when `a` and `b` yield the same heard motif in every phrase, Evolve
 *  aside: they agree on every field the motif functions above read and on
 *  the phrase length. The other fields only change how a bar renders. */
bool sameMotifInputs(const Config& a, const Config& b);

// ─── Phrase grammar (§3.7) ──────────────────────────────────────────────────

/** Bars per phrase: 4, 8 or 16. */
int phraseBarsOf(const Config& cfg);

/** True when `bar` lies in a B unit of the form. */
bool isBUnit(const Config& cfg, int bar);

struct BarVariation
{
    int  repitchStep = -1;            // motif step whose embellishment changes kind
    Fn   repitchFn   = Fn::Rest;
    bool lift = false, fill = false, ratchet = false, drop = false;
    int  ratchetHits = 2;
    int  cadence = 0;                 // 0 none, 1 open (A), 2 closed (B) — ABAB only
};
BarVariation variationForBar(const Motif& m, const std::array<bool, kMaxMotifSteps>& heads,
                             const Config& cfg, int bar);

// ─── Rendering ──────────────────────────────────────────────────────────────

struct Event
{
    double onQ   = 0.0;    // quarters from bar start
    double offQ  = 0.0;    // quarters from bar start (may exceed the bar)
    int    note  = 60;
    float  vel   = 0.67f;  // 0..1
    int    strand = -1;    // VoiceEvent::strandId
    bool   glide = false;  // continues the strand's held voice (slide target)
    bool   held  = false;  // slide source: no note-off, the next onset glides it
};

/** What the phrase view shows per step of the rendered bar. */
struct StepView
{
    int  note   = -1;      // −1 = rest
    Fn   fn     = Fn::Rest;
    bool head   = false;
    bool accent = false;
    bool slide  = false;
};

struct BarRender
{
    std::array<Event, kMaxEvents> events {};
    int    count = 0;
    std::array<StepView, kMaxBarSteps> view {};
    int    viewSteps = 0;
    int    lastNote = -1, prevNote = -1;   // main line P and PP after the bar
    bool   endsHeld = false;               // the last main note slides into the next bar
    std::array<int, kMaxVoices> lastVoicing { -1, -1, -1, -1 };
    int    lastVoicingCount = 0;
    Chord  chord {};                       // chord at the bar start (for the view)
};

struct RenderInput
{
    const Config* cfg       = nullptr;
    const Motif*  motif     = nullptr;   // heard motif of this bar's phrase
    const Motif*  nextMotif = nullptr;   // heard motif of the next bar's phrase
    const Motif*  answer    = nullptr;   // Answer layer motif (nullptr = none)
    int    bar      = 0;
    double barLenQ  = 4.0;
    bool   keys     = false;             // Keys source: `keysChord` everywhere
    Chord  keysChord {};
    int    prevNote = -1, prevPrevNote = -1;
    bool   slideIn  = false;             // the previous bar's last note is held
    const int* prevVoicing = nullptr;    // Keys: live voicing for Stab / Chord layer
    int    prevVoicingCount = 0;
};

/** Renders one whole bar. A change inside a running bar re-renders the whole
 *  bar and the scheduler plays the events from the current position on. */
void renderBar(const RenderInput& in, BarRender& out);

/** The main line's P / PP entering `bar` for Pedal and Chords: the last notes
 *  of bar − 1 rendered on its own. Keeps continuous play and a locate equal.
 *  `motif` is the motif of bar's phrase: the first bar of its origin phrase
 *  enters from the notes kept with it when it has them (Motif::entryKept). */
void entryNotes(const Config& cfg, const Motif& prevMotif, const Motif& motif,
                int bar, double barLenQ, BarRender& scratch, int& p, int& pp);

/** Stab / Chord-layer voicing of the chord loop at `bar`, voice-led over two
 *  laps of the loop so it does not depend on where playback started. */
int voicingForBar(const Config& cfg, int bar, int anchor, int* out);

// ─── Step → Phrase (§4) ─────────────────────────────────────────────────────

struct StepInput { int note; bool enabled; bool slide; float velocity; };

/** Analyses step-grid notes against the chord of bar 0: heads → nearest chord
 *  member, other steps → the embellishment that explains them. The result has
 *  cfg's grid; its Len follows the step count (1 bar, 2 when the steps exceed
 *  one bar of the grid; later steps are dropped), pulses = the enabled steps
 *  within that length, rotation 0. The caller writes Len / Pulses / Rotation
 *  back so fitMotif keeps the analysed motif as it is. */
Motif motifFromSteps(const StepInput* steps, int count, const Config& cfg,
                     std::uint32_t seed, int origin);

} // namespace PhraseModel
