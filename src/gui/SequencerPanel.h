#pragma once
#include <JuceHeader.h>
#include <array>
#include "GuiHelpers.h"
#include "GenSeqCRTMonitor.h"

class T5ynthProcessor;

/**
 * SequencerPanel — sequencer + arpeggiator controls + step grid.
 *
 * Layout:
 *   Row 1 (SHARED): [>/■] [Step|Phrase|Gen]  BPM[==]  [1/4|1/8|1/16]  Gate[==] Shuffle[==] · MIDI
 *   STEP mode row 2: Steps[==●]16   Oct[-2 -1 0 +1 +2]   [Preset▾][S][L]
 *   STEP mode grid : dot | vertical note slider | glide dot | horiz velocity
 *   PHRASE mode    : the Phrase controls and view (docs/PHRASE_SEQ_CONCEPT.md §4)
 *   GEN  mode      : 2-column Euclidean + harmony box + voices (no genVisArea)
 *   Row 4 (bottom): [Arp ✓] [Up▾] [1/16▾]  [Oct 1 2 3 4]
 */
class SequencerPanel : public juce::Component, private juce::Timer
{
public:
    explicit SequencerPanel(T5ynthProcessor& processor);
    ~SequencerPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    /** Opens the pattern Library overlay (hosted by MainPanel). The bool
     *  asks the library to focus its name field for an immediate save. */
    std::function<void(bool /*focusSave*/)> onOpenPatternLibrary;

    /** Write the current sequencer/arp/gen state to `file` as a partial
     *  .t5seq preset. Returns true on success. Used by SequenceLibraryPanel. */
    bool writePatternTo(const juce::File& file);

    /** Load a .t5seq partial preset and refresh the grid. Returns true on
     *  success (false if the file is missing or the import fails). */
    bool loadPatternFrom(const juce::File& file);

private:
    void timerCallback() override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    void syncStepCount();
    void showHeaderOverflowMenu();

    T5ynthProcessor& processorRef;

    // Section header
    juce::Label seqHeader;

    // Row 1: Transport + step config
    juce::TextButton transportBtn { "PLAY" };
    // Step | Phrase | Gen mode switchbox — the visible control; drives
    // engineHidden (the hidden APVTS bridge for PID::seqEngine).
    juce::TextButton modeStepBtn { "Step" }, modePhraseBtn { "Phrase" }, modeGenBtn { "Gen" };
    juce::Rectangle<int> modeSwitchBounds;
    // Unified Steps slider (step mode, inline bar, no FIX). Manual write to
    // seqSteps (like the old dropdown) so the grid stays capped at MAX_COLS.
    std::unique_ptr<SliderRow> seqStepsRow;
    static constexpr int kNumDivBtns = 5;
    juce::TextButton divBtns[kNumDivBtns];        // note-length glyph switchbox [1/1..1/16]
    juce::ComboBox divisionHidden;                 // hidden, for APVTS attachment
    juce::Rectangle<int> divisionSwitchBounds;     // unified frame around the division switchbox
    std::unique_ptr<SliderRow> bpmRow;
    juce::Label midiMonitor;
    juce::Rectangle<float> midiLedBounds;
    // LED state tracked on the message thread so the 10 Hz timer only repaints
    // the LED region when the on/off bit flips, instead of every tick.
    bool midiLedDisplayedOn = false;
    juce::TextButton headerOverflowBtn { "..." };

    // Octave shift [-2][-1][0][+1][+2]
    static constexpr int kNumOctShiftBtns = 5;
    juce::TextButton octShiftBtns[kNumOctShiftBtns];
    juce::ComboBox octShiftHidden;
    juce::Rectangle<int> octShiftSwitchBounds;     // unified frame

    // Icon LookAndFeel for save/load buttons (declared before buttons — destroyed after)
    struct IconLnF : juce::LookAndFeel_V4
    {
        juce::Path icon;
        void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
        void drawButtonText(juce::Graphics&, juce::TextButton&, bool, bool) override;
    };
    IconLnF saveLnf, loadLnf;

    // Row 2: Seq params
    juce::ComboBox presetBox;
    juce::TextButton seqSaveBtn { "S" };
    juce::TextButton seqLoadBtn { "L" };
    std::unique_ptr<SliderRow> gateRow;
    std::unique_ptr<SliderRow> shuffleRow;

    // Row 3: Step grid
    struct StepColumn : public juce::Component,
                        public juce::DragAndDropTarget,
                        public juce::SettableTooltipClient
    {
        int stepIndex = 0;
        T5ynthProcessor* processor = nullptr;
        bool isCurrentStep = false;
        bool isRecordCursor = false;   // step-record armed-target highlight (red)
        int dragZone = -1;        // 0=dot, 1=note, 2=bind/glide, 3=velocity, 4=one-shot
        int dropHoverSlot = -1;
        int oneShotPressSlot = -1;       // slot pressed in the one-shot zone (cycle on click / copy-drag source)
        bool oneShotDragStarted = false; // a copy-drag began this gesture → suppress click-cycle on mouseUp
        float dragStartVal = 0.f;
        bool noteHoldPreviewActive = false;
        int noteHoldPreviewNote = -1;
        void paint(juce::Graphics& g) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseDrag(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        bool isInterestedInDragSource(const SourceDetails& details) override;
        void itemDragMove(const SourceDetails& details) override;
        void itemDragExit(const SourceDetails& details) override;
        void itemDropped(const SourceDetails& details) override;
        // Zone geometry (set by paint based on bounds)
        int noteBottom() const { return juce::roundToInt(static_cast<float>(getHeight()) * 0.60f); }
        int oneShotBottom() const { return juce::roundToInt(static_cast<float>(getHeight()) * 0.72f); }
        int velBottom() const { return juce::roundToInt(static_cast<float>(getHeight()) * 0.84f); }
        juce::Rectangle<int> oneShotArea() const;
        juce::Rectangle<int> oneShotSlotBounds(int slot) const;
        int oneShotSlotAt(juce::Point<int> p) const;
        int oneShotDropSlotAt(juce::Point<int> p) const;
        // bottom 16%: [On][Bind/Glide] buttons (half height, like velocity bar)
    };
    static constexpr int MAX_COLS = 32;
    std::array<std::unique_ptr<StepColumn>, MAX_COLS> stepCols;
    int numVisibleSteps = 16;
    juce::Rectangle<int> gridArea;

    // Generative sequencer controls
    juce::ComboBox engineHidden;                 // hidden, for APVTS (seq_engine)
    // Euclidean + harmony controls as inline-label bars. Cyc binds genFieldRate
    // (Int) via a SliderAttachment; Range is a separate switchbox (see below).
    std::unique_ptr<SliderRow> genStepsRow, genPulsesRow, genRotationRow,
                               genMutationRow, genCycRow;
    juce::ComboBox genScaleRootBox, genScaleTypeBox;
    // Range switchbox [1][2][3][4] on the genRange Choice — the octave span of the
    // whole pitch field (shared by ALL voices). Hidden ComboBox = APVTS bridge.
    static constexpr int kNumRangeBtns = 4;
    juce::TextButton genRangeBtns[kNumRangeBtns];
    juce::ComboBox   genRangeHidden;
    juce::Rectangle<int> genRangeSwitchBounds;       // unified frame (gen mode only)
    juce::Label      genRangeLabel;                  // "Rng" tag before the switchbox
    // Harmony box (framed card in the gen-mode left column) + the thin divider
    // above the voices. Recorded in resized(), drawn in paint() (gen mode only).
    juce::Rectangle<int> harmonyBoxBounds;
    int voicesDividerY = -1;

    // Fix toggle buttons (lock/unlock icons)
    juce::TextButton genFixStepsBtn, genFixPulsesBtn, genFixRotationBtn, genFixMutationBtn;

    // ── Polyphony (Phase 5) ──────────────────────────────────────────────
    // Shared pitch-field controls. "Drift" mode stays a dropdown; "Cyc"
    // (genFieldRate) is now the genCycRow inline slider declared above.
    juce::ComboBox genFieldModeBox;
    // Ensemble coordination mode (Independent / Density Budget / Dialog) —
    // global, so it lives in the harmony row, not in a strand module.
    juce::ComboBox genCoordModeBox;
    // Per-extra-strand (indices 0..3 map to strands 2..5)
    static constexpr int kNumExtraStrands = 4;
    juce::TextButton strandEnableBtns[kNumExtraStrands];
    juce::ComboBox   strandRoleBoxes[kNumExtraStrands];
    // Per-strand differentiators on the second GEN row, vertically aligned
    // beneath the [Sx][Role] cluster they belong to.
    //   Div:  ComboBox showing sorted StrandDivMult values — value is its own label
    //   Oct:  5-switch-button row "-2 -1 0 +1 +2" mirroring SeqOctave's
    //         convention; the slider stays as a hidden APVTS bridge.
    //   Dom:  small Label "Dom" + slider 0..1
    static constexpr int kStrandOctBtns = 5;
    juce::ComboBox   strandDivBoxes[kNumExtraStrands];
    juce::TextButton strandOctBtns[kNumExtraStrands][kStrandOctBtns];
    juce::Rectangle<int> strandOctSwitchBounds[kNumExtraStrands]; // unified frames (gen mode only)
    juce::Slider     strandOctaveSliders[kNumExtraStrands]; // hidden APVTS bridge
    // Dom = full-module-width inline bar (Poly-AT style), label+value in the track.
    std::unique_ptr<SliderRow> strandDomRows[kNumExtraStrands];
    // V1 is a display-only dummy module (no params); its frame + placeholders are
    // drawn in paint() from this rect (gen mode only).
    juce::Rectangle<int> v1ModuleBounds;
    // Per-voice (V2..V5) module rects, recorded in resized() and drawn as subtle
    // group cards in paint() (gen mode only) — the mod-panel sub-card "Abschattung".
    juce::Rectangle<int> voiceModuleBounds[kNumExtraStrands];
    // GEN-mode "core monitor" occupying the S1 slot — makes the engine's
    // per-cycle euclidean/Turing decisions legible. Visible only in GEN mode.
    std::unique_ptr<GenSeqCRTMonitor> genMonitor;

    // The SeqEngine resized() last laid the panel out for (-1 before the
    // first layout); the timer re-lays out when the parameter moves away.
    int layoutEngine = -1;

    // Row 4: Arp controls (left-labels + glyph/numeric switchboxes)
    juce::Label arpModeLabel;                    // "ARP" left-label
    static constexpr int kNumModeBtns = 5;
    juce::TextButton arpModeBtns[kNumModeBtns]; // glyphs: off/up/down/updown/random
    juce::ComboBox arpModeBox;                   // hidden, for APVTS
    juce::ComboBox arpRateBox;
    juce::Label arpOctLabel;                      // "OCT" left-label
    static constexpr int kNumOctBtns = 4;
    juce::TextButton arpOctBtns[kNumOctBtns];  // [1][2][3][4]
    juce::ComboBox arpOctHidden;                 // hidden, for APVTS
    juce::Rectangle<int> arpModeSwitchBounds, arpOctSwitchBounds;  // unified frames

    // APVTS attachments
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<SA> bpmA, gateA, shuffleA, genStepsA, genPulsesA, genRotationA,
                        genMutationA, genFieldRateA;
    std::unique_ptr<CA> divA, presetA, arpModeA, arpRateA, arpOctA, octShiftA,
                        genScaleRootA, genScaleTypeA, genRangeA;
    std::unique_ptr<CA> engineA;
    std::unique_ptr<BA> genFixStepsA, genFixPulsesA, genFixRotationA, genFixMutationA;

    // Polyphony attachments
    std::unique_ptr<CA> genFieldModeA, genCoordModeA;
    std::unique_ptr<BA> strandEnableA[kNumExtraStrands];
    std::unique_ptr<CA> strandRoleA[kNumExtraStrands];
    std::unique_ptr<CA> strandDivA[kNumExtraStrands];
    std::unique_ptr<SA> strandOctaveA[kNumExtraStrands];
    std::unique_ptr<SA> strandDomA[kNumExtraStrands];

    int currentStep = -1;
    int lastPresetGen = 0;   // mirrors StepSequencer::presetAppliedGen for grid refresh

    // Step-record display mirrors (updated only when the bit/cursor changes, so
    // the 10 Hz timer never repaints idly).
    bool recordArmedDisplayed = false;
    int  recordCursorDisplayed = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SequencerPanel)
};
