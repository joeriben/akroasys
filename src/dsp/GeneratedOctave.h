#pragma once

#include <JuceHeader.h>
#include <vector>

/**
 * Keeps a regenerated sample in the octave the series already sounds in.
 *
 * Why: a held note follows every regenerate (the held-note invariant in
 * CLAUDE.md), and a new seed or a wide drift moves the model's octave while the
 * note name stays: measured in docs/plans/PITCH_NORMALISATION_C3_REPORT.md §6,
 * octave jumps in all six presets under a new seed. BJ heard the offline
 * correction (2026-11-07): "bei Oktaven sehr gut wirksam ... implementiere das."
 *
 * The rule, all of it:
 *  - A sample's pitch is read with the detector the wavetable already ships
 *    (WavetableOscillator::analyzePitchWindow, windows of 4096 at hop 2048,
 *    confidence >= 0.9, the upper median), plus the two cut-offs the report
 *    measured it needs (§5): at least half of the windows voiced, and at least
 *    half of the voiced windows within 50 cents of that median. Below either,
 *    the sample has no pitch and is left alone.
 *  - The series remembers one pitch per note it has sounded, as it sounds.
 *  - A new sample on the same note as one of them, within 50 cents in any
 *    octave and at most two octaves away, is moved by whole octaves onto it.
 *    Any other sample is left as the model made it and joins the series. So
 *    only the octave is ever changed: never the note, never the tuning.
 *  - The move is tape-style, as on the listening page BJ judged: an octave up
 *    plays the material at twice the speed, an octave down at half.
 *  - A sample never comes out longer than the model made it. Moved up it is
 *    half as long (a quarter for two octaves). Moved down it keeps its length
 *    and holds the first half (quarter) of the material: the Duration the
 *    player set stays the longest a sample gets, and with it the cache's
 *    slot-second budget (maxInferenceCacheCapacityForDuration).
 */
class GeneratedOctave
{
public:
    struct Reading
    {
        bool  pitched = false;
        float midi    = 0.0f;   // the agreed pitch, MIDI note number with cents
    };

    /** The pitch of a sample, or none. Pure; any thread. About 0.2 s per 3 s of
     *  audio, at most about 0.75 s, so never on the message or audio thread. */
    static Reading read(const juce::AudioBuffer<float>& audio, double sampleRate);

    /** Octaves to move a sample with this reading by (positive = up), and the
     *  series now holds it as it will sound. 0 for a sample without a pitch, on
     *  a note the series has not sounded, or more than two octaves from it. */
    int fit(const Reading& fresh);

    /** The series starts from this sound. */
    void anchor(const Reading& current);

    /** The series is empty: the next sample with a pitch starts it. */
    void forget() { notes_.clear(); }

    /** The sample moved by whole octaves (positive = up), never longer than it
     *  was: see the class comment. Halfband FIR from JUCE's own 2x oversampler
     *  design (equiripple, the maximum-quality settings of
     *  juce::dsp::Oversampling), applied once per octave. Pure; any thread;
     *  seconds for a long render, so never on the message or audio thread. */
    static juce::AudioBuffer<float> moveOctaves(const juce::AudioBuffer<float>& audio, int octaves);

    static constexpr int kMaxOctaves = 2;

private:
    std::vector<float> notes_;
};

/** One regenerate's octave work. Taken on the message thread at the request
 *  (T5ynthProcessor::beginOctaveFit), run on the generation thread after the
 *  render (run), settled on the message thread when the result lands
 *  (T5ynthProcessor::finishOctaveFit). All the reading and moving happens in
 *  run(), against a copy of the series; the message thread only adopts it. */
struct GeneratedOctaveJob
{
    // Taken at the request.
    bool         skip = false;           // wavetable: no octave to keep, see beginOctaveFit
    GeneratedOctave series;              // the series as it stood
    bool         readLoaded = false;     // the series has to be read from what plays
    juce::AudioBuffer<float> loaded;     // what plays, copied only when readLoaded
    double       loadedRate = 0.0;
    juce::uint64 seriesVersion = 0;

    // Worked out by run().
    GeneratedOctave::Reading freshReading;
    GeneratedOctave::Reading loadedReading;
    int octaves = 0;
    juce::AudioBuffer<float> moved;      // the result moved, empty when octaves == 0

    /** On the generation thread, after the render. */
    void run(const juce::AudioBuffer<float>& fresh, double freshRate)
    {
        if (skip)
            return;
        freshReading = GeneratedOctave::read(fresh, freshRate);
        if (readLoaded)
        {
            loadedReading = GeneratedOctave::read(loaded, loadedRate);
            loaded.setSize(0, 0);
            series.anchor(loadedReading);
        }
        octaves = series.fit(freshReading);
        if (octaves != 0)
            moved = GeneratedOctave::moveOctaves(fresh, octaves);
    }
};

/** What finishOctaveFit hands back: the sample to load, and by how many octaves
 *  it was moved (the event log records it, so a replayed tape moves the same). */
struct FittedGeneration
{
    juce::AudioBuffer<float> audio;
    int octaves = 0;
};
