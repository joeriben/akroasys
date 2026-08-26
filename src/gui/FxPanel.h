#pragma once
#include <JuceHeader.h>
#include "GuiHelpers.h"

/**
 * FX column (20%): the whole effects chain, in ONE card.
 *
 * A SWITCH TITLE ROW names the six effects in the order they are wired —
 * dist, chorus, phaser, tremolo (the amplifier chain, PluginProcessor's
 * processBlock), then delay and reverb, which run serially after it with the
 * reverb send taken behind the delay. Clicking a cell chooses which effect the
 * card is editing; the cell's own text says whether that effect is RUNNING, so
 * all six states are readable without clicking through them.
 *
 * Below it, row 2 always begins with OFF on the left — for delay and reverb that
 * is their type switchbox, which already worked that way; for the other four it
 * is the bypass, and the rest of the row is theirs to use. Rows 3 and 4 carry
 * what is left. The card's height is fixed by the tallest of the six so nothing
 * moves when the selection changes.
 *
 * This replaced two stacked cards that showed delay and reverb only. The other
 * four effects had twelve parameters, ran in every block, saved into every
 * preset and sat on the LRO author's shelf — and appeared nowhere in the GUI, so
 * the author could set them and the player could not (BJ, 2026-08-01). Four
 * cards beside the existing two would have needed 382 px of a column that is at
 * most 280 tall; one card needs 90, which is less than the two it replaces.
 *
 * Limiter is internal only — no GUI controls.
 */
class T5ynthProcessor; // forward decl

class FxPanel : public juce::Component,
                private juce::Timer
{
public:
    FxPanel(juce::AudioProcessorValueTreeState& apvts, T5ynthProcessor& processor);
    ~FxPanel() override { stopTimer(); }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class WordmarkComponent : public juce::Component
    {
    public:
        void paint(juce::Graphics& g) override;
    };

    /** The six cells of the title row, in signal order. Also indexes the four
     *  amp-effect arrays below, which is why those four come first. */
    enum FxSel { SelDist = 0, SelChorus, SelPhaser, SelTrem, SelDelay, SelReverb };
    static constexpr int kNumFxSel = 6;
    static constexpr int kNumAmpFx = 4;   // the ones with a bypass of their own

    void timerCallback() override;
    float fs() const;
    void updateVisibility();
    /** Is this effect doing anything — the state the title cell reports. */
    bool effectRunning(int sel) const;
    // One dropdown per category: opens the family's variant menu on its cell.
    void showDelayFamilyMenu(int btnIndex, const int* types, int numTypes);
    void showReverbFamilyMenu(int btnIndex, const int* types, int numTypes);
    T5ynthProcessor& processorRef;

    // Clock-button LnF — declared BEFORE the button using it so destruction
    // order is button-first, LnF-second. Never call setLookAndFeel(nullptr)
    // on the button during teardown.
    ClockButtonLnF delayClockLnf;

    // ── The title row ──
    juce::TextButton fxSelBtns[kNumFxSel];
    int fxSelected_ = SelDelay;            // what the column showed before this row existed
    int fxRunningMask_ = -1;               // one bit per cell; -1 forces the first paint
    juce::Rectangle<int> fxSelSwitchBounds;
    juce::Rectangle<int> fxCardBounds;     // the one framed module card
    // The eight parameters the running lamps read, looked up ONCE. Their
    // atomics never move for the plugin's lifetime, and timerCallback reads
    // them at 30 Hz ahead of the audioIdle gate — where a string-keyed lookup
    // over every parameter is exactly what ParamCache.h exists to avoid.
    std::atomic<float>* runOnPtr_ [4] { };
    std::atomic<float>* runWetPtr_[4] { };
    // Delay and reverb have no bypass of their own - their type IS the switch -
    // but they do have a mix, and a type other than Off at mix zero is an
    // effect that cannot be heard. Same question as runWetPtr_, for the two
    // cells that were not asking it.
    std::atomic<float>* runDelayMixPtr_  = nullptr;
    std::atomic<float>* runReverbMixPtr_ = nullptr;

    // ── The four amp effects ──
    // Their OFF is a bool of its own (delay and reverb have OFF as a value of
    // their type parameter). The visible cell is a plain button; the APVTS state
    // lives on a hidden ToggleButton, the same shape as the type ComboBoxes.
    juce::TextButton   ampStateBtns[kNumAmpFx];
    juce::ToggleButton ampOnHidden[kNumAmpFx];
    juce::Rectangle<int> ampStateBounds;
    std::unique_ptr<SliderRow> distDriveRow, distMixRow;
    std::unique_ptr<SliderRow> chorRateRow, chorAmtRow, chorMixRow;
    std::unique_ptr<SliderRow> phasRateRow, phasAmtRow, phasFbRow, phasMixRow;
    std::unique_ptr<SliderRow> tremRateRow, tremAmtRow, tremStereoRow;
    // The tremolo's LFO shape, the one amp effect with a choice of its own:
    // [Sine][Tri][Soft][Square], same hidden-ComboBox shape as the two type
    // switchboxes below.
    static constexpr int kNumTremWaveBtns = 4;
    juce::TextButton tremWaveBtns[kNumTremWaveBtns];
    juce::ComboBox   tremWaveHidden;
    juce::Rectangle<int> tremWaveSwitchBounds;

    // ── Delay ──
    static constexpr int kNumDelayBtns = 5;
    juce::TextButton delayTypeBtns[kNumDelayBtns]; // [OFF][Dig][PP][Tape v][BBD v]; family cells open their own dropdown
    juce::ComboBox   delayTypeHidden;              // APVTS-attached, not visible
    juce::Rectangle<int> delayTypeSwitchBounds;
    juce::TextButton delayClockBtn;            // BPM-sync clock button
    juce::ComboBox   delayClockModeHidden;     // hidden, APVTS-attached
    std::unique_ptr<SliderRow> delayTimeRow, delayFbRow, delayDampRow, delayMixRow;
    std::unique_ptr<SliderRow> delayDivisionRow; // same rect as delayTimeRow when sync

    // ── Reverb ──
    static constexpr int kNumReverbBtns = 3;
    juce::TextButton reverbTypeBtns[kNumReverbBtns]; // [OFF][Plate v][Freeverb v]
    juce::ComboBox reverbTypeHidden;
    juce::Rectangle<int> reverbTypeSwitchBounds;
    std::unique_ptr<SliderRow> reverbMixRow;
    std::unique_ptr<SliderRow> algoRoomRow, algoDampRow, algoWidthRow;

    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

    // Attachments last: they must be destroyed BEFORE the components they point
    // at, and members die in reverse declaration order.
    std::unique_ptr<BA> ampOnA[kNumAmpFx];
    std::unique_ptr<SA> distDriveA, distMixA;
    std::unique_ptr<SA> chorRateA, chorAmtA, chorMixA;
    std::unique_ptr<SA> phasRateA, phasAmtA, phasFbA, phasMixA;
    std::unique_ptr<SA> tremRateA, tremAmtA, tremStereoA;
    std::unique_ptr<SA> delayTimeA, delayFbA, delayDampA, delayMixA;
    std::unique_ptr<SA> delayDivisionA;
    std::unique_ptr<SA> reverbMixA, algoRoomA, algoDampA, algoWidthA;
    std::unique_ptr<CA> delayTypeA, reverbTypeA, delayClockModeA, tremWaveA;

    WordmarkComponent wordmark;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxPanel)
};
