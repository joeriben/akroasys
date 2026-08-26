#pragma once
#include <JuceHeader.h>
#include <array>
#include "WaveformDisplay.h"
#include "GuiHelpers.h"
#include "AftertouchBar.h"
#include "ExprSourceSwitch.h"
#include "VelocityBar.h"
#include "../dsp/BlockParams.h"   // kNumModEnvs, PID, AftertouchTarget
#include "../dsp/LroControls.h"   // the authored instrument's own knobs

class T5ynthProcessor;

/**
 * Column 2 (55%): ENGINE + FILTER + MODULATION
 * Contains: engine mode, waveform, loop controls, scan, filter,
 *           5 envelopes, 3 LFOs, drift, explore button.
 * ALL controls are compact linear slider rows — zero rotary knobs.
 */
class SynthPanel : public juce::Component, private juce::Timer
{
public:
    explicit SynthPanel(T5ynthProcessor& processor);
    ~SynthPanel() override { stopTimer(); }

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    float fs() const;
    void updateVisibility();
    void reconcileWaveformDisplayMode();   // WT fan vs sample view + region label
    void followModParamToTab(const juce::String& paramId);  // easy-mode tab follows controller
    /** The LRO card's inner rectangle — the one area both the knob grid and the
     *  library list live in. One definition, so the painted headings cannot
     *  drift away from the placed sliders. */
    juce::Rectangle<int> lroCardBounds() const;
    /** Place the authored instrument's knobs in that card, one column per part.
     *  Fills lroColumnBounds and hides every row the body did not declare. */
    void layoutLroKnobs();
    bool initialized = false;
    bool pendingWtReextract_ = false;

    T5ynthProcessor& processorRef;

    // ── Engine mode ──
    juce::TextButton samplerBtn { "Sampler" };
    juce::TextButton wavetableBtn { "Wavetable" };
    juce::TextButton freezeBtn { "Granular" };
    juce::ComboBox engineModeHidden;
    WaveformDisplay waveformDisplay;
    // LRO (Csound): the "PROMPT ORCHESTRA" mask painted over the (otherwise
    // stale) waveform card also names the curated instruments from
    // lco_library.json, so the card reads as the library the coding LLM
    // already consults — never a menu, just the same orientation it reads
    // (project_lco_llm_authors_csound). Kept as the KEYS, not a pre-joined
    // sentence: paintOverChildren lays them out as a real multi-column list at
    // a real font size, which a single run-on string cannot be. Refreshed in
    // updateVisibility() while the LRO is showing — so an instrument approved
    // and built into the library during the session arrives at the next panel
    // event instead of only after the editor is reopened; PipeInference
    // memoises the file, so a refresh that changes nothing costs a few stats —
    // and only ever READ in paintOverChildren(), never loaded there: a paint
    // callback doing file I/O on every repaint is exactly the idle-CPU
    // regression class docs/PERFORMANCE_GUIDE.md warns about.
    juce::StringArray lroInstrumentNames_;

    // ── Voice count ──
    // 7 visible buttons (Mono, 4, 6, 8, 12, 16, 64). The 128-voice option
    // remains in the backend (BlockParams enum + voiceCounts array) for host
    // automation / presets, but is hidden from the UI — too many voices for
    // typical use.
    static constexpr int kNumVoiceBtns = 7;
    juce::TextButton voiceBtns[kNumVoiceBtns];
    juce::ComboBox voiceCountHidden;
    juce::Rectangle<int> voiceSwitchBounds;

    // ── Tuning ──
    juce::ComboBox tuningBox;

    // ── Loop controls ──
    juce::TextButton oneshotBtn;   // → one-shot (icon drawn in paintOverChildren)
    juce::TextButton loopModeBtn;  // ↻ loop
    juce::TextButton pingpongBtn;  // ⇄ ping-pong
    juce::ComboBox loopModeHidden;
    std::unique_ptr<SliderRow> crossfadeRow;
    juce::TextButton loopOptimizeBtn { "Opt: Off" };
    juce::TextButton normalizeToggle { "Norm" };
    juce::TextButton hfBoostBtn { "HF" };

    // ── Scan ──
    std::unique_ptr<SliderRow> scanRow;
    juce::Label scanHint;

    // ── Wavetable controls row ──
    static constexpr int kNumFrameBtns = 4;
    juce::TextButton frameBtns[kNumFrameBtns];
    juce::ComboBox framesHidden;
    juce::Rectangle<int> framesSwitchBounds;
    juce::TextButton smoothToggle { "Smooth" };
    juce::TextButton autoScanToggle { "AutoScan" };
    juce::Label frameCountLabel;

    // ── Granular controls row ──
    static constexpr int kNumFreezeTextureBtns = 4;
    juce::TextButton freezeTextureBtns[kNumFreezeTextureBtns];
    juce::ComboBox freezeTextureHidden;
    juce::Rectangle<int> freezeTextureSwitchBounds;
    std::unique_ptr<SliderRow> freezeStereoRow;

    // ── Octave shift ──
    static constexpr int kNumOctBtns = 5;
    juce::TextButton octBtns[kNumOctBtns];
    juce::ComboBox octaveHidden;
    juce::Rectangle<int> octaveSwitchBounds;

    // ── Noise (shared by all engine modes) ──
    static constexpr int kNumNoiseBtns = 3;
    juce::TextButton noiseBtns[kNumNoiseBtns];
    juce::ComboBox noiseTypeHidden;
    juce::Rectangle<int> noiseSwitchBounds;
    std::unique_ptr<SliderRow> noiseLevelRow;

    // ── LRO: the knobs the AUTHORED instrument gives the player ──
    // Twelve rows, always constructed and always attached to the twelve fixed
    // lro_p* parameters, because those parameters are always there. What
    // CHANGES with each authored instrument is which of them are visible and
    // what they are called — both from the library parameters the written body
    // kept (LroControls). A row the current body has no line for is hidden,
    // never relabelled with a guess.
    static constexpr int kNumLroKnobs = 12;
    std::array<std::unique_ptr<SliderRow>, kNumLroKnobs> lroKnobRows;
    // One LEVEL per column, above that column's knobs. Not a library parameter
    // and never named by one: it is the part's own attenuator, on the scaffold's
    // `osc1vol`..`osc3vol` that every authored body already reads as `kvolN`.
    static constexpr int kNumLroLevels = 3;
    std::array<std::unique_ptr<SliderRow>, kNumLroLevels> lroLevelRows;
    // Declared AFTER the rows: attachments must die before what they point at.
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>,
               kNumLroKnobs> lroKnobA;
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>,
               kNumLroLevels> lroLevelA;
    // The panel's copy of what the processor holds, refreshed when the
    // processor's revision moves (timerCallback) rather than re-read per paint.
    LroControls lroControls;
    int lroControlsSeen = -1;
    // Card geometry, computed ONCE in layoutLroKnobs and read by
    // paintOverChildren — never derived a second time from the same
    // expressions, which is how the caption and the part names came to be drawn
    // into each other. One rect per shown part, in the order they are drawn.
    juce::Array<juce::Rectangle<int>> lroColumnBounds;
    juce::Rectangle<int> lroCaptionBounds;    // the author's READING
    juce::Rectangle<int> lroPartNameBounds;   // the row the part names sit in

    // ── Section headers ──
    juce::Label engineHeader, filterHeader, modHeader, lfoHeader, driftHeader;

    // ── Layout rects for paint() ──
    juce::Rectangle<int> engineSwitchBounds, loopSwitchBounds, optSwitchBounds;
    juce::Rectangle<int> filterSlopeSwitchBounds, filterAlgSwitchBounds;
    int engineCardBottom = 0;
    int modCardBottom = 0;
    juce::Rectangle<int> envTabSwitchBounds, lfoTabSwitchBounds, driftTabSwitchBounds;
    juce::Rectangle<int> filterEasyBlockBounds, envEasyBlockBounds, lfoEasyBlockBounds, driftEasyBlockBounds, generateEasyBlockBounds;
    juce::Rectangle<int> aftertouchEasyBlockBounds;
    std::array<juce::Rectangle<int>, 3> lfoEasyModuleBounds;
    std::array<juce::Rectangle<int>, 3> driftEasyModuleBounds;

    // ── Filter ──
    // Type: OFF LP HP BP — drives filter_type APVTS via hidden ComboBox; the
    // visible affordance for PID::filterType: one dropdown, OFF LP HP BP.
    // OFF is a value of the parameter, so bypass and type cannot disagree.
    juce::ComboBox filterTypeBox;
    // Slope switchbox: 6dB 12dB 18dB 24dB
    static constexpr int kNumSlopeBtns = 4;
    juce::TextButton filterSlopeBtns[kNumSlopeBtns];
    juce::ComboBox filterSlopeHidden;
    std::unique_ptr<SliderRow> cutoffRow, resoRow, filterMixRow, kbdTrackRow, filterDriveRow;
    // Drive oversampling: Off 2x 4x 8x — hidden carrier only, no visible
    // switchbox; drive oversampling is controlled elsewhere.
    juce::ComboBox filterDriveOsHidden;
    // Filter algorithm switchbox: SVF Ladder Warp
    static constexpr int kNumAlgBtns = 3;
    juce::TextButton filterAlgBtns[kNumAlgBtns];
    juce::ComboBox filterAlgHidden;
    // Warp style selector (only active when algorithm == Warp)
    juce::ComboBox filterWarpStyleBox;

    // Easy-mode Warp style "hold-to-dropdown" button.
    // Click (<350 ms): activate Warp algorithm, keep current style.
    // Hold (≥350 ms): open style picker popup; chosen style becomes new preselection.
    // Sits in the 4th cell of the Easy algorithm row, replacing filterAlgBtns[Warp].
    struct WarpHoldBtn : juce::TextButton, private juce::Timer
    {
        WarpHoldBtn() = default;
        std::function<void()>    onTap;       // short click
        std::function<void(int)> onStylePick; // popup pick, 1-based style id

        void mouseDown (const juce::MouseEvent& e) override;
        void mouseUp   (const juce::MouseEvent& e) override;
        void mouseExit (const juce::MouseEvent& e) override;
        void paintButton(juce::Graphics& g, bool highlighted, bool down) override;
    private:
        void timerCallback() override;
        bool holdTriggered_ = false;
    };
    WarpHoldBtn filterEasyWarpBtn;

    // ── Envelope sections ──
    struct EnvSection
    {
        juce::Label targetHeader;   // "Target" left-header band
        juce::ComboBox targetBox;

        // ── "Velocity Amount" box ──
        // Vertical drag-fill bars (AftertouchBar feel). Att/Dec/Rel are bipolar
        // (velocity→stage TIME); Level drives the GLOBAL velAmt (unipolar,
        // velocity→peak). velAmt is global, so every env section's Level bar
        // attaches to the same parameter — only the selected env's box is
        // visible at a time.
        ModuleBox velBox;   // framed card + accent top-header, same template as DURATION/RE-PROMPT
        std::unique_ptr<VelocityBar> attVB, decVB, relVB, levelVB;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attVBA, decVBA, relVBA, levelVBA;

        // Loop toggle: turns the env into a self-retriggering
        // A→D→Hold→R complex-LFO cycle while the note is held. In loop mode the
        // Sustain control is repurposed as the per-cycle Hold time.
        juce::TextButton loopBtn;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> loopBtnA;

        // Which envelope this section drives. Held as the whole id set so
        // COPY/PASTE can move an entire envelope without a second list of
        // parameter names that could fall behind PID::ModEnvIds.
        PID::ModEnvIds ids {};

        // Copy the whole envelope — every id in ids.all(), target included —
        // from one tab to another. Deliberately NOT APVTS-attached: these are
        // momentary actions on the tree, not parameters of their own.
        juce::TextButton copyBtn, pasteBtn;

        // Graphical ADSR editor — attaches directly to the APVTS envelope
        // parameters. Declared LAST so it is destroyed FIRST.
        std::unique_ptr<AdsrGraph> graph;
    };
    // Envelope clipboard: real (denormalised) values in PID::ModEnvIds::all()
    // order. PASTE stays disabled until COPY has filled it. Declared BEFORE the
    // sections, so it outlives the buttons whose onClick reads it.
    using EnvValueSet = std::array<float, std::tuple_size<decltype(PID::ModEnvIds{}.all())>::value>;
    EnvValueSet envClipboard_ {};
    bool envClipboardFilled_ = false;
    bool envClipboardFromAmp_ = false;   // ENV 1's target is the DCA and does not travel
    void copyEnvelope (const EnvSection& env);
    void pasteEnvelope (const EnvSection& env);

    EnvSection ampEnv;
    EnvSection modEnvSections[kNumModEnvs];   // ENV 2..5
    // ENV 1..5 in tab order; index 0 is the amp envelope.
    EnvSection& envSection (int i) { return i == 0 ? ampEnv : modEnvSections[i - 1]; }
    static constexpr int kNumModTabs = 1 + kNumModEnvs;   // ENV 1..5; LFO/Drift use the first 3
    static constexpr int kNumWaveBtns = 6;  // sine/tri/saw/sq/s&h/saw-down (mirrors LfoWave + DriftWave)
    static constexpr int kNumLfoModeBtns = 2;
    std::array<juce::TextButton, kNumModTabs> envTabBtns;
    // LFO and Drift still have three each — they share the tab-strip helper,
    // so their arrays are sized alike and only the first kNumLfoTabs are used.
    static constexpr int kNumLfoTabs = 3;
    std::array<juce::TextButton, kNumModTabs> lfoTabBtns, driftTabBtns;
    int activeEnvTab = 0;
    int activeLfoTab = 0;
    int activeDriftTab = 0;
    uint32_t lastSeenMidiTouchSeq_ = 0;  // tracks processor's midiTouchSeq_ for tab-follow

    // ── Clock-button LnFs (shared across the section's clock buttons) ──
    //   Declared BEFORE LfoSection/DriftSection so destruction order is
    //   button-first, LnF-second. Never call setLookAndFeel(nullptr) on
    //   the buttons during teardown.
    ClockButtonLnF lfoClockLnf, driftClockLnf;
    LockButtonLnF  lfoModeLnf;   // Free/Trig padlock (LFO only; Drift has no mode)

    // ── LFO sections ──
    //   modeHidden + clockModeHidden are APVTS-attached (not addAndMakeVisible'd);
    //   modeBtn + clockBtn are the visible 1-cycle controls that drive them.
    //   divisionRow occupies the same screen rect as rateRow — visibility
    //   swaps based on ClockMode.
    struct LfoSection
    {
        juce::Label header;
        juce::ComboBox targetBox, waveBox;
        std::array<juce::TextButton, kNumWaveBtns> waveBtns;
        juce::Rectangle<int> waveSwitchBounds;
        juce::ComboBox modeHidden, clockModeHidden;
        juce::TextButton modeBtn, clockBtn;
        std::array<juce::TextButton, kNumLfoModeBtns> modeBtns;
        juce::Rectangle<int> modeSwitchBounds;
        std::unique_ptr<SliderRow> rateRow, depthRow, divisionRow;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
            rateA, depthA, divisionA;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
            waveA, modeA, clockModeA;
    };
    LfoSection lfo1, lfo2, lfo3;

    // ── MIDI performance modulation ──
    // Easy-panel AT module: 12 bipolar drag-fill bars (one per target, enum
    // order 1..12) + column header. Bars declared BEFORE their attachments so
    // the attachments destruct first (JUCE reverse-destruction-order rule).
    juce::Label aftertouchHeader;
    // One bar per aftertouch target except None — AftertouchTarget::kCount - 1.
    std::array<std::unique_ptr<AftertouchBar>, AftertouchTarget::kCount - 1> aftertouchBars;
    // Must track aftertouchBars exactly — the two grow together in one loop, and
    // a shorter attachment array writes unique_ptrs over whatever member follows.
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>,
               AftertouchTarget::kCount - 1> aftertouchBarA;
    static_assert(std::tuple_size<decltype(aftertouchBarA)>::value
                      == std::tuple_size<decltype(aftertouchBars)>::value,
                  "aftertouchBarA and aftertouchBars must be the same length.");
    // The source switch that shares each bar's row: which of V/X/Y/Z drives that
    // target, or Ø for none. Same length rule and same declaration order as the bars above —
    // switches before their attachments, so the attachments destruct first.
    std::array<std::unique_ptr<ExprSourceSwitch>, AftertouchTarget::kCount - 1> exprSrcSwitches;
    /** The two raw values behind each expression row, so the timer can ask the
     *  one question the switch cannot answer for itself: is this row at zero and
     *  still on the axis it shipped with. Resolved once. */
    std::atomic<float>* atRowAmtPtr_[AftertouchTarget::kCount - 1] { };
    std::atomic<float>* atRowSrcPtr_[AftertouchTarget::kCount - 1] { };
    // Which target each ROW carries (display order != enum order), and whether
    // the player has moved that row at all. Both feed refreshExprRowOffState.
    int  atRowTarget_ [AftertouchTarget::kCount - 1] { };
    bool atRowTouched_[AftertouchTarget::kCount - 1] { };
    /** One bit per row, so sixteen relaxed loads a tick cost one int compare and
     *  a repaint follows only when the picture actually changed. */
    juce::uint32 atRowOffMask_ = 0xffffffffu;   // forces the first pass
    void refreshExprRowOffState();
    std::array<std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>,
               AftertouchTarget::kCount - 1> exprSrcSwitchA;
    static_assert(std::tuple_size<decltype(exprSrcSwitchA)>::value
                      == std::tuple_size<decltype(exprSrcSwitches)>::value,
                  "exprSrcSwitchA and exprSrcSwitches must be the same length.");

    // ── Drift ──
    //   Same dual-control pattern as LFO, minus the F/T mode (Drift has
    //   no Free/Trig).
    struct DriftSection
    {
        juce::Label header;
        juce::ComboBox targetBox, waveBox;
        std::array<juce::TextButton, kNumWaveBtns> waveBtns;
        juce::Rectangle<int> waveSwitchBounds;
        juce::ComboBox clockModeHidden;
        juce::TextButton clockBtn;
        std::unique_ptr<SliderRow> rateRow, depthRow, divisionRow;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
            rateA, depthA, divisionA;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
            targetA, waveA, clockModeA;
    };
    DriftSection drift1, drift2, drift3;
    // Regenerate cadence switchbox: manual / a.s.a.p. / 1 / 2 / 4 / 8 / 16 bars
    juce::Label regenHeader;
    static constexpr int kNumRegenBtns = 7;
    juce::TextButton regenBtns[kNumRegenBtns];
    juce::ComboBox regenHidden;
    juce::Rectangle<int> regenSwitchBounds;
    std::unique_ptr<SliderRow> crossfadeRegenRow;

    // ── APVTS attachments ──
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<CA> engineModeA, loopModeA, voiceCountA, tuningA;
    std::unique_ptr<SA> scanA, noiseLevelA, cutoffA, resoA, filterMixA, kbdTrackA, crossfadeA;
    std::unique_ptr<CA> noiseTypeA;
    std::unique_ptr<CA> octaveA;
    std::unique_ptr<CA> wtFramesA;
    std::unique_ptr<BA> wtSmoothA;
    std::unique_ptr<BA> wtAutoScanA;
    std::unique_ptr<CA> freezeTextureA;
    std::unique_ptr<SA> freezeStereoA;
    std::unique_ptr<CA> filterTypeA, filterSlopeA, filterDriveOsA, filterAlgA, filterWarpStyleA;
    std::unique_ptr<SA> filterDriveA;
    std::unique_ptr<CA> driftRegenA;
    std::unique_ptr<SA> crossfadeRegenA;

    // ENV/LFO target attachments (routed in processBlock)
    std::unique_ptr<CA> ampTargetA, lfo1TargetA, lfo2TargetA, lfo3TargetA;
    std::unique_ptr<CA> modTargetA[kNumModEnvs];

    void initEnv(EnvSection& env, const juce::String& name, int defaultTarget,
                 const PID::ModEnvIds& ids, juce::AudioProcessorValueTreeState& apvts);
    void initLfo(LfoSection& lfo, const juce::String& name,
                 const juce::String& rateId, const juce::String& depthId,
                 const juce::String& waveId, const juce::String& modeId,
                 const juce::String& clockModeId, const juce::String& divisionId,
                 juce::AudioProcessorValueTreeState& apvts);
    void initDrift(DriftSection& drift, const juce::String& name,
                   const juce::String& rateId, const juce::String& depthId,
                   const juce::String& targetId, const juce::String& waveId,
                   const juce::String& clockModeId, const juce::String& divisionId,
                   juce::AudioProcessorValueTreeState& apvts);

    void layoutModEasy(juce::Rectangle<int>& area, juce::Rectangle<int> modHeaderRow, float f, int rowH, int gap, int headerH, float headerFs);
    void layoutFilterEasy(juce::Rectangle<int> area, float f, int rowH, int gap);
    void layoutEnvEasy(EnvSection& env, juce::Rectangle<int> area, float f, int rowH, int gap);
    void layoutLfoEasy(LfoSection& lfo, juce::Rectangle<int> area, float f, int rowH, int gap);
    void layoutDriftEasy(DriftSection& drift, juce::Rectangle<int> area, float f, int rowH, int gap);
    void layoutAftertouchEasy(juce::Rectangle<int> area);
    void layoutGenerateEasy(juce::Rectangle<int> area, float f, int rowH, int gap, bool ownHeader = true);

    void syncModTabButtons();
    void selectFirstActiveModTabs();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SynthPanel)
};
