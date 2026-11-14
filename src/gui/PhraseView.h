#pragma once
#include <JuceHeader.h>
#include <array>
#include <cstdint>
#include "../sequencer/PhraseModel.h"

class T5ynthProcessor;

/**
 * The read-only phrase view (docs/PHRASE_SEQ_CONCEPT.md §4). The phrase strip
 * on top: one cell per bar of the phrase, the form letter where a unit of the
 * form starts, the chord where it changes, the bar the grid shows lit. Under
 * it the grid of that bar: one column per step, the note name over the step's
 * glyph (PhraseModel::stepGlyphText); heads drawn stronger, an accent marked
 * with a wedge, a slide with a tie to the step it glides into, the step
 * playing lit.
 *
 * It reads the phrase sequencer's GUI atomics and the phrase parameters and
 * never writes. poll() is driven by SequencerPanel's timer while the view
 * shows, and repaints only the cells whose content changed; paint() draws
 * only what poll() cached.
 */
class PhraseView : public juce::Component
{
public:
    explicit PhraseView(T5ynthProcessor& p);

    /** Reads the sequencer and the parameters; repaints what changed. */
    void poll();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct StripText
    {
        juce::String form, chord;
        bool operator==(const StripText& o) const { return form == o.form && chord == o.chord; }
    };
    struct StepText
    {
        juce::String note, glyph;
        bool rest = true, head = false, accent = false;
        /** A slide: the step it glides into, the bar's step count when that
         *  is in the next bar; −1 none. */
        int tieTo = -1;
        bool operator==(const StepText& o) const
        {
            return note == o.note && glyph == o.glyph && rest == o.rest && head == o.head
                && accent == o.accent && tieTo == o.tieTo;
        }
    };

    void pollGrid(int stepsPerBar);
    void pollStrip(const PhraseModel::Config& cfg);
    juce::Rectangle<int> stripCell(int j) const;
    juce::Rectangle<int> stepColumn(int k) const;
    juce::Rectangle<float> tieBounds(int k) const;
    /** Repaints the grid columns lo..hi (clamped to the bar's steps). */
    void repaintSteps(int lo, int hi);

    T5ynthProcessor& processor;

    // Grid: the published view, re-read on a new generation.
    std::uint32_t viewGen = 0;
    bool viewRead = false;
    std::array<StepText, PhraseModel::kMaxBarSteps> steps {};
    int numSteps = 0;
    int currentStep = -1;
    int stepsPerQuarter = 4;

    // Strip: recomputed when the parameters, the phrase or (Keys) the bar
    // and its held chord change.
    PhraseModel::Config stripConfig {};
    bool stripRead = false;
    int stripFirstBar = 0;
    int stripKeysBar = 0;
    PhraseModel::Chord stripKeysChord {};
    std::array<StripText, PhraseModel::kMaxPhraseBars> strip {};
    int numBars = 0;
    bool stripHasForm = false;   // the form letters' column is reserved in every cell
    int litBar = -1;

    // Geometry and fonts, from resized().
    juce::Rectangle<int> stripRow, gridRow, noteRow, glyphRow;
    int formW = 0;
    juce::Font stripFont { juce::FontOptions(11.0f) }, stripBold { juce::FontOptions(11.0f) },
               stepFont { juce::FontOptions(11.0f) }, stepBold { juce::FontOptions(11.0f) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhraseView)
};
