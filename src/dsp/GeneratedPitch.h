#pragma once

#include <JuceHeader.h>
#include <array>
#include <bitset>

/**
 * Keeps a regenerated sample on the note the prompt names.
 *
 * Why: a held note follows every regenerate (the held-note invariant in
 * CLAUDE.md), and a new seed or a wide drift moves the model's pitch while the
 * prompt still says "c3": measured in docs/plans/PITCH_NORMALISATION_C3_REPORT.md
 * §6, octave jumps in all six presets under a new seed. BJ, 2026-11-08: "aus
 * einer Spieler-perspektive - und die aktiviert man mit dem Schalter bzw. "c3" -
 * will man keinen Sprung", and "entweder ich arbeite modular und eher fließend,
 * oder ich verwende den Synth mit einem Keyboard in live oder Aufnahme → dann
 * kann ich keine Intervallsprünge gebrauchen."
 *
 * The rule, all of it:
 *  - It acts only when a prompt names a note (a trailing "c3", "g2", as
 *    Music::parse reads it). Without one the sample plays as the model made it:
 *    that is the fluid way of playing.
 *  - A sample's pitch is read with the detector the wavetable already ships
 *    (WavetableOscillator::analyzePitchWindow, windows of 4096 at hop 2048,
 *    confidence >= 0.9, the upper median), plus the two cut-offs the report
 *    measured it needs (§5): at least half of the windows voiced, and at least
 *    half of the voiced windows within 50 cents of that median. Below either,
 *    the sample has no pitch and is left alone.
 *  - The series has one target note, tuned as the synth's Tuning and Scale
 *    Root tune that pitch class. Every sample with a pitch, the first one too,
 *    is moved exactly onto it, note and cents, up to two octaves away, whatever
 *    note the model made: a "c3" prompt also gives real G4 square waves (five
 *    of 48 regenerates of "steady square wave, c3", the stored Dirty
 *    Squarewave buffer too; their C3 partials lie 49 to 66 dB under the G4),
 *    and left alone each of them is a jump of a twelfth under the finger.
 *  - Where the target sits: a sample within a semitone of a named note is that
 *    note out of tune, and the series takes the octave it plays in (with two
 *    named notes, the one it is on). A sample further from every named note
 *    shows no octave of the named note, so the series takes the note as the
 *    prompt writes it ("c3" is C3), if that is within two octaves of the
 *    sample; otherwise the sample is left as the model made it and sets
 *    nothing.
 *  - The move is tape-style, as on the listening page BJ judged: up plays the
 *    material faster, down slower, by the interval.
 *  - A sample never comes out longer than the model made it. Moved up it is
 *    shorter by the interval's ratio. Moved down it keeps its length and holds
 *    the first part of the material: the Duration the player set stays the
 *    longest a sample gets, and with it the cache's slot-second budget
 *    (maxInferenceCacheCapacityForDuration).
 */
class GeneratedPitch
{
public:
    struct Reading
    {
        bool  pitched = false;
        float midi    = 0.0f;   // the agreed pitch, MIDI note number with cents
    };

    /** The notes the prompts name, and where the synth's tuning puts each pitch
     *  class. No named note: nothing is read and nothing is moved. */
    struct Named
    {
        int classes = 0;                   // bit n set: pitch class n (0 = C) is named
        std::bitset<128> notes;            // the named notes as written, MIDI (c3 = 48)
        std::array<float, 12> cents {};    // the tuning's offset from 12-TET at A = 440, per pitch class

        bool any() const { return classes != 0; }
    };

    /** Message thread or any: the notes the two prompts name (Music::parse),
     *  tuned by the Tuning and Scale Root choices. */
    static Named namedIn(const juce::String& promptA, const juce::String& promptB,
                         int tuningType, int scaleRoot);

    /** The pitch of a sample, or none. Pure; any thread. About 0.2 s per 3 s of
     *  audio, at most about 0.75 s, so never on the message or audio thread. */
    static Reading read(const juce::AudioBuffer<float>& audio, double sampleRate);

    /** Semitones to move a sample with this reading by (positive = up); the
     *  series has its target once a sample could take one. 0 when nothing is
     *  named, for a sample without a pitch, for one more than two octaves off
     *  the target, and below a cent. */
    float fit(const Named& named, const Reading& fresh);

    /** The series goes on from this sound, what plays: its target is chosen
     *  from it as from the first sample of a series. A sound that can take no
     *  target leaves the target as it was. */
    void anchor(const Named& named, const Reading& current);

    /** The series is empty: the next sample with a pitch sets its target. */
    void forget() { hasTarget_ = false; }

    /** Both name the same notes: the same pitch classes and the same notes as
     *  written. */
    static bool sameNotes(const Named& a, const Named& b)
    { return a.classes == b.classes && a.notes == b.notes; }

    /** The sample moved by this many semitones (positive = up), never longer
     *  than it was: see the class comment. Band-limited interpolation (Smith &
     *  Gossett 1984; J. O. Smith, "Digital Audio Resampling", CCRMA) with a
     *  Kaiser-windowed sinc, the cutoff lowered by the speed when moving up so
     *  nothing aliases. Pure; any thread; up to seconds for a long render, so
     *  never on the message or audio thread. */
    static juce::AudioBuffer<float> move(const juce::AudioBuffer<float>& audio, float semitones);

    static constexpr float kMaxMove = 24.0f;   // semitones: two octaves
    // The furthest a sample is moved: two octaves, counted to the nearest
    // semitone, so a sample two octaves and a cent away is still moved.
    static constexpr float kReach = kMaxMove + 0.5f;
    static constexpr float kMinMove = 0.01f;   // semitones: one cent, below it nothing is moved
    static constexpr float kOnNamed = 1.0f;    // semitones: a reading this close to a named note is that note

private:
    /** The named pitch class nearest a reading, in the octave nearest it, and
     *  how far the reading is from it. */
    static int nearestNamed(const Named& named, float midi, float& distance);
    /** The named note as written nearest a reading, and how far the reading is
     *  from it; -1 when no written note is a MIDI note. */
    static int nearestWritten(const Named& named, float midi, float& distance);
    /** Sets the target from the first sample of a series; false when the
     *  sample can take none (see the class comment). */
    bool choose(const Named& named, const Reading& reading);

    bool hasTarget_ = false;
    int  target_    = 0;     // the MIDI note the series sounds on
    int  classes_   = 0;     // the named set the target was chosen from:
    std::bitset<128> notes_; // its pitch classes and its notes as written
};

/** One regenerate's pitch work. Taken on the message thread at the request
 *  (T5ynthProcessor::beginPitchFit), run on the generation thread after the
 *  render (run), settled on the message thread when the result lands
 *  (T5ynthProcessor::finishPitchFit). All the reading and moving happens in
 *  run(), against a copy of the series; the message thread only adopts it. */
struct GeneratedPitchJob
{
    // Taken at the request.
    bool         skip = false;           // nothing named, or the wavetable: see beginPitchFit
    GeneratedPitch series;               // the series as it stood
    GeneratedPitch::Named named;         // what the request's prompts name
    bool         readLoaded = false;     // the series has to be read from what plays
    juce::AudioBuffer<float> loaded;     // what plays, copied only when readLoaded
    double       loadedRate = 0.0;
    juce::uint64 seriesVersion = 0;

    // Worked out by run().
    GeneratedPitch::Reading freshReading;
    GeneratedPitch::Reading loadedReading;
    float semitones = 0.0f;
    juce::AudioBuffer<float> moved;      // the result moved, empty when semitones == 0

    /** On the generation thread, after the render. */
    void run(const juce::AudioBuffer<float>& fresh, double freshRate)
    {
        if (skip)
            return;
        freshReading = GeneratedPitch::read(fresh, freshRate);
        if (readLoaded)
        {
            loadedReading = GeneratedPitch::read(loaded, loadedRate);
            loaded.setSize(0, 0);
            series.anchor(named, loadedReading);
        }
        semitones = series.fit(named, freshReading);
        if (semitones != 0.0f)
            moved = GeneratedPitch::move(fresh, semitones);
    }
};

/** What finishPitchFit hands back: the sample to load, and by how many
 *  semitones it was moved (the event log records it, so a replayed tape moves
 *  the same). */
struct FittedGeneration
{
    juce::AudioBuffer<float> audio;
    float semitones = 0.0f;
};
