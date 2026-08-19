#pragma once
#include <JuceHeader.h>
#include "GuiHelpers.h"   // SliderRow (house-standard inline-bar row)
#include "PromptPanel.h"
#include "AxesPanel.h"
#include "DimensionExplorer.h"
#include "SynthPanel.h"
#include "FxPanel.h"
#include "SequencerPanel.h"
#include "StatusBar.h"
#include "ReplayOverlay.h"
#include "SetupWizard.h"
#include "PresetManagerPanel.h"
#include "SequenceLibraryPanel.h"
#include "../dsp/BlockParams.h"
#include "../dsp/LroControls.h"
#include "../presets/PresetFormat.h"
#include "../presets/PresetUpdater.h"

class T5ynthProcessor;

class MainPanel : public juce::Component,
                  public juce::DragAndDropContainer,
                  private juce::Timer
{
public:
    explicit MainPanel(T5ynthProcessor& processor);
    ~MainPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    bool keyPressed(const juce::KeyPress& key) override;
    void toggleSettings();

private:
    class GenerateButton : public juce::TextButton
    {
    public:
        explicit GenerateButton(const juce::String& label);

        void setAnimationState(float phase, bool isGenerating);
        void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

    private:
        float animationPhase = 0.0f;
        bool generating = false;
    };

    T5ynthProcessor& processorRef;

    // Col 1: GENERATION — three cards with headers
    juce::Label oscHeader, axesNote, poweredByLabel;
    juce::TextButton oscModeToggle { juce::String::fromUTF8("\xc2\xbb LRO") };
    PromptPanel promptPanel;
    AxesPanel axesPanel;
    // Semantic Axes | Dim Explorer — a 2-segment view switch (switchbox template)
    // over ONE shared fixed-height box. Not tied to Easy/Advanced; default shows
    // the Axes controls. false = Semantic Axes, true = Dim Explorer mini-view.
    juce::TextButton axesDimSegBtns[2];
    bool showDimSegment_ = false;
    GenerateButton mainGenerateBtn { "GENERATE" };
    juce::Label snapLabel, cacheLabel;
    static constexpr int kNumInfCacheButtons = 5;   // OFF / 2 / 4 / 8 / 16
    static constexpr int kNumSnapshotSlots = 4;
    static constexpr int kNumSnapshotButtons = kNumSnapshotSlots + 1;

    struct MainSnapshot
    {
        bool valid = false;
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        juce::ValueTree parameters;
        juce::String promptA, promptB, device, model, injectionMode;
        std::array<AxesPanel::SlotState, 3> axes;
        std::vector<float> embeddingA, embeddingB;
        std::vector<std::pair<int, float>> dimensionOffsets;
        int seed = 0;
        bool randomSeed = false;
        float lateMixAmount = std::numeric_limits<float>::quiet_NaN();
        float splitStart = std::numeric_limits<float>::quiet_NaN();
        float splitEnd = std::numeric_limits<float>::quiet_NaN();
        float loopStart = 0.0f;
        float loopEnd = 1.0f;
        float startPos = 0.0f;
        float wtExtractStart = 0.0f;
        float wtExtractEnd = 1.0f;
        bool pointsLocked = false;
    };

    class SnapshotButton : public juce::TextButton,
                           private juce::Timer
    {
    public:
        SnapshotButton() = default;
        // JUCE rule 1 (CLAUDE.md): a Timer subclass stops its timer before any
        // member of it is gone — the long-press timer here would otherwise be
        // free to fire into a half-destroyed button on window close.
        ~SnapshotButton() override { stopTimer(); }
        void setSnapshotIndex(int index);
        void setSnapshotFilled(bool filled);
        void flashStored();
        void paintButton(juce::Graphics& g, bool highlighted, bool down) override;
        void mouseDown(const juce::MouseEvent& e) override;
        void mouseUp(const juce::MouseEvent& e) override;
        void mouseExit(const juce::MouseEvent& e) override;

        std::function<void(int)> onPressStarted;
        std::function<void(int)> onShortActivate;
        std::function<void(int)> onLongStore;

    private:
        void timerCallback() override;
        int snapshotIndex = 0;
        bool snapshotFilled = false;
        bool pressing = false;
        bool longFired = false;
        double pressStartMs = 0.0;
        double flashUntilMs = 0.0;
    };

    // Cache-capacity button — when selected and the cache is still filling,
    // the fill colour pulses subtly. Replaces the textual status row that
    // previously sat below the button strip.
    class CacheCapButton : public juce::TextButton
    {
    public:
        CacheCapButton() = default;
        void setPulsing(bool p);
        void setPulsePhase(float phase);
        void paintButton(juce::Graphics& g, bool highlighted, bool down) override;

    private:
        bool pulsing = false;
        float lastPhase = 0.0f;
    };

    SnapshotButton snapshotButtons[kNumSnapshotButtons];
    CacheCapButton infCacheButtons[kNumInfCacheButtons];
    // The CACHE row's rightmost cell: a MODE, not a depth, so it sits outside the
    // capacity radio group. 32 and 64 gave up their cells for it — a take that deep
    // wrote presets of hundreds of megabytes. Attachment declared AFTER the button
    // it drives (reverse destruction order).
    CacheCapButton cacheAsyncBtn;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cacheAsyncAttachment;
    juce::Rectangle<int> snapshotSwitchBounds;
    juce::Rectangle<int> cacheSwitchBounds;
    std::array<MainSnapshot, kNumSnapshotSlots> mainSnapshots;
    std::array<MainSnapshot, kNumSnapshotSlots> snapshotPressCaptures;
    // LCO SNAP slots: what a neural snapshot's audio buffer is to the T5osc, the
    // authored ORCHESTRA is to the LCO — the code IS the sound, so a slot stores
    // the Csound source plus the sound-shaping parameter set around it (the same
    // kMainSnapshotParamIds the neural side restores) and the disclosure that
    // explains it. A recall therefore sounds immediately, with no LLM pass: the
    // orchestra goes straight back to requestCsoundOrchestra(). Storing only the
    // prompt (the earlier behaviour) recalled a piece of text and left the user to
    // re-author minutes of LLM work for a sound that was never the same twice.
    // A slot taken BEFORE any bake still parks the prompt alone (orchestra empty)
    // — that capability is kept, not replaced. Slot i ↔ button i+1.
    struct LcoSnapshot
    {
        bool valid = false;
        juce::String prompt;        // the LCO prompt editor's text
        // What actually AUTHORED the orchestra in this slot, which is not the
        // same thing: `prompt` is parked editor text and may be the next idea
        // already typed while the previous sound is still playing. Only this one
        // may be persisted as the preset's csound_prompt — the other would
        // caption a sound with a prompt that never wrote it.
        juce::String authoringPrompt;
        juce::String orchestra;     // authored Csound source ("" = nothing baked yet)
        juce::String reading;       // "how it was heard" gloss
        juce::String paramsText;    // the parametrisation behind that reading
        juce::String authorModel;   // which model wrote the orchestra
        juce::ValueTree parameters; // APVTS state (kMainSnapshotParamIds subset applies)
        // What the twelve lro_p* parameters MEAN in this orchestra. Their VALUES
        // ride in `parameters` like any other, but a value without its reading is
        // worse than nothing here: recall a bowl into a panel still describing a
        // supersaw and the slider captioned "Detune" turns the bowl's mode series.
        LroControls controls;
    };
    std::array<LcoSnapshot, kNumSnapshotSlots> lcoSnapshots;
    std::array<LcoSnapshot, kNumSnapshotSlots> lcoPressCaptures;
    // True while an LCO authoring pass is in flight (PromptPanel::onLcoBusyChanged).
    // A recall is refused for its duration — the bake publishes its own orchestra
    // when it lands and would otherwise overwrite the recalled sound behind the
    // user's back. Storing stays allowed: a slot taken meanwhile keeps what is
    // sounding now, which is exactly what the press asked for.
    bool lcoBakeBusy_ = false;
    int activeSnapshotIndex = 0;  // 0=OFF, 1..4=session snapshot selected

    // Resynth (init_audio / i2i): SA3-gated, house-standard inline-bar SliderRow
    // (the same format as the sequencer's Shuffle row) — accent band label + fill
    // bar in kOscCol, Off..Full inline read-out. The int/ext source toggle sits to
    // its right. The SliderRow paints its own Drift ghost (setGhostValue/tickGhost).
    // JUCE destruction order (CLAUDE.md rule 3): attachments destruct first
    // (declared last); the row/buttons/ComboBox precede their attachments.
    std::unique_ptr<SliderRow> resynthRow;   // inline-bar row (Shuffle format), kOscCol
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> resynthA;
    // int/ext source toggle: two TextButtons drive a hidden ComboBox whose
    // attachment syncs with APVTS. Declared after resynthRow and resynthA so
    // resynthSrcA (the attachment) destructs first.
    juce::TextButton resynthSrcBtns[2];                 // "int" / "ext"
    juce::ComboBox   resynthSrcHidden;                  // drives the APVTS attachment
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> resynthSrcA;

    // (Re-Prompt controls now live in PromptPanel, under the prompts, next to the
    // loop logic that drives them — see PromptPanel's repromptStanceBar/coupling.)

    int lastInfCacheUiCapacity = -1;
    int lastInfCacheUiFill = -1;
    bool lastInfCacheUiFull = false;
    float glowPhase = 0.0f;
    double glowLastTimeSec = 0.0;
    bool glowGenerating = false;
    // Independent phase for the cache-fill pulse — runs at a fixed rate
    // (~2 Hz) whenever the cache is filling, decoupled from glowPhase which
    // crawls at 0.05 Hz when idle and would make the cache-pulse invisible.
    float cachePulsePhase = 0.0f;
    bool oscEasyMode = true;
    float oscModePulsePhase = 0.0f;
    bool oscModePulseActive_ = false;
    // Caches the last 8-bit ARGB applied to oscModeToggle so the pulse path
    // can skip the setColour cascade (each call triggers internalRepaint)
    // when the resulting Colour is unchanged at this quantization.
    juce::Colour lastAppliedOscFill_, lastAppliedOscText_;
    bool lastAppliedOscColoursValid_ = false;
    // Cache-hit feedback: when the inference cache is full, the primary
    // generation actions become cache playback actions and stay labelled
    // "cache hit" until the cache is cleared/disabled.
    bool cacheHitActive = false;
    double cacheHitUntilSec = 0.0;
    bool computerKeyboardEnabled = false;
    int computerKeyboardOctaveOffset = 0;
    bool spaceRestKeyDown_ = false;   // step-record: Space-rest edge (re-armed when the key lifts)
    // Octave-key edge, kept per editor because that is the only place it may live
    // where a strike is not a key event (see t5::physicalKeyStrikesAreEvents).
    bool computerKeyboardOctaveDownHeld_ = false;
    bool computerKeyboardOctaveUpHeld_ = false;
    bool computerKeyboardOctaveDownPolled_ = false;   // read down by the PREVIOUS poll pass
    bool computerKeyboardOctaveUpPolled_ = false;
    static constexpr int kComputerKeyboardKeyCount = 20; // C4–G5: a…k octave + o l p ö ä + #  (ü has no note)
    std::array<bool, kComputerKeyboardKeyCount> computerKeyboardNotesDown {};
    std::array<int, kComputerKeyboardKeyCount> computerKeyboardActiveNotes {};
    void timerCallback() override;
    void syncInferenceCacheUi();
    void updateGenerateButtonsForCacheState(bool pulseCacheHit);
    void setOscEasyMode(bool easy, bool persist);
    // Hands the engine to the paradigm the toggle just selected. User-click
    // path only — see the definition's comment on why startup/preset must not
    // call it.
    void applyOscModeToEngine(bool neural);
    bool loadOscEasyModeSetting() const;
    void saveOscEasyModeSetting() const;
    juce::String loadSa3TierSetting() const;
    void saveSa3TierSetting(const juce::String& tier) const;
    bool hasOscHiddenActiveState() const;
    void updateOscModeToggleVisual();
    MainSnapshot captureMainSnapshot();
    void restoreMainSnapshot(const MainSnapshot& snapshot);
    LcoSnapshot captureLcoSnapshot();
    void restoreLcoSnapshot(const LcoSnapshot& snapshot);
    void captureSnapshotPress(int slot);
    void storeSnapshotFromPress(int slot);
    void activateSnapshot(int slot);
    void syncSnapshotUi();
    // Marshals mainSnapshots → preset format for persistence, and the
    // reverse direction when applying a loaded preset.
    std::vector<PresetFormat::SnapshotState> buildSnapshotsForSave() const;
    void applySnapshotsFromLoad(const std::vector<PresetFormat::SnapshotState>& snapshots,
                                int calibEpoch);
    void triggerMainGeneration();
    void setComputerKeyboardEnabled(bool enabled);
    bool shiftComputerKeyboardOctave(int delta);   // true if the offset actually moved
    void pollComputerKeyboard();
    void releaseComputerKeyboardNotes();
    bool isTextEditingFocus() const;
    bool scanComputerKeyboard(bool allowStart);   // reconcile notes from physical key state
    int computerKeyboardNoteForIndex(int keyIndex) const;
    juce::String computerKeyboardBaseNoteName() const;
    juce::String computerKeyboardStatusText() const;

    // Col 2: ENGINE + FILTER + MODULATION
    SynthPanel synthPanel;

    // FX
    FxPanel fxPanel;

    // Bottom
    SequencerPanel sequencerPanel;
    StatusBar statusBar;

    // Master volume. MidiLearnSlider, not a bare juce::Slider, so the one fader
    // that sits outside every SliderRow still obeys Shift-is-fine.
    MidiLearnSlider masterVolKnob;
    juce::Label masterVolLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterVolA;

    // Overlay: Dimension Explorer
    // Scrim catches clicks outside DimExplorer to close the overlay
    struct Scrim : public juce::Component {
        std::function<void()> onClick;
        void mouseDown(const juce::MouseEvent&) override { if (onClick) onClick(); }
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xdd101016)); }
    };
    Scrim dimScrim;
    DimensionExplorer dimensionExplorer;
    juce::TextButton dimApplyBtn    { "Apply + Generate" };
    juce::TextButton dimUndoBtn     { "Undo" };
    juce::TextButton dimRedoBtn     { "Redo" };
    juce::TextButton dimResetBtn    { "Reset All" };
    bool dimExplorerVisible = false;

    void showDimExplorer();
    void hideDimExplorer();
    void updateAxesDimSegment();   // apply showDimSegment_ → visibility + relayout
    void tryLoadInferenceModels(bool forceRestart = false);
    void savePreset();
    void loadPreset();
    void renameCurrentPreset();
    void deleteCurrentPreset();
    void showPresetNameContextMenu(juce::Point<int> screenPos);
    void importPresetFile();
    void finishPresetUpdate(bool success,
                            PresetUpdater::Stats stats,
                            juce::String error);
    void exportWav();
    void saveSessionLog();   // Export menu → copy the current .t5evt to a chosen location
    void loadReplaySession();// Export menu → pick a .t5evt and hand it to the replay transport
    juce::String suggestedExportBaseName() const;   // filename proposed in the WAV save dialog
    void loadDefaultPreset();
    void loadInitPreset();
    bool savePresetToFile(const juce::File& file, bool includeInferenceCache = true);
    bool loadPresetFromFile(const juce::File& file);
    void applyLoadedPreset(const PresetFormat::LoadResult& result, const juce::File& sourceFile = {});
    void syncGuiStateForPresetSave();
    void showPresetManager();
    void hidePresetManager();
    void showSequenceLibrary(bool focusSave = false);
    void hideSequenceLibrary();
    enum class SaveNameMode { keepName, appendCopy };
    void enterLibrarySaveMode(SaveNameMode mode = SaveNameMode::keepName);
    juce::String getCurrentPresetDisplayName() const;
    /** Re-write a .t5p file's embedded JSON `name` field without touching
     *  any other field or the audio PCM. Used by the rename flow so the
     *  metadata stored inside the file stays consistent with the new
     *  filename. Returns true on success. */
    static bool patchPresetNameField(const juce::File& file, const juce::String& newName);

    /** Re-write a .t5p file's embedded JSON `tags` array in place — audio,
     *  prompts, axes etc. are untouched. This lets the user edit tags on
     *  ANY preset in the library without first loading it (a full re-save
     *  would otherwise overwrite the file with the engine's current state). */
    static bool patchPresetTagsField(const juce::File& file, const juce::StringArray& newTags);

    /** Phase-4 fork: copy `source` into the user-presets root with a
     *  "(mine)" suffix and apply the supplied tag list to the copy. Used
     *  when the user edits a preset that lives in the GitHub-synced
     *  UCDCAE bank — the original is left untouched so PresetUpdater can
     *  keep refreshing it without clobbering the user's hand-edits. The
     *  copy's JSON `name` field is updated to match the new filename so
     *  the embedded metadata stays consistent. Returns the new file on
     *  success, or {} on failure. */
    static juce::File forkPresetForUserEdit(const juce::File& source,
                                            const juce::StringArray& newTags);

    /** True if `file` is inside the UCDCAE-AI-Lab github-synced bank
     *  (lives under <userPresetsRoot>/<PresetUpdater::getBankName()>/). */
    static bool isGithubBankFile(const juce::File& file);
    void showSettings();
    void hideSettings();
    void showManual();
    void hideManual();

    // Model settings overlay
    SettingsPage settingsPage;
    GeneralSettingsPage generalSettingsPage;
    LroAuthorSettingsPage lroAuthorSettingsPage;
    // Tabbed settings overlay: "Models" (model manager, default) + "Settings" +
    // "LRO Author". Declared AFTER its tab contents → destroyed FIRST (it
    // references them).
    juce::TabbedComponent settingsTabs { juce::TabbedButtonBar::TabsAtTop };
    Scrim settingsScrim;
    bool settingsVisible = false;
    // One-shot: set when a newer release is found so the NEXT Settings-open lands
    // on the General/Settings tab (where the Download row is), then cleared. The
    // badge + banner persist on their own — this only steers the first open, so it
    // can't stick the tab for the whole session or hijack the model-setup open.
    bool pendingUpdateTabJump_ = false;
    bool pendingInferenceReload = false;

    // Preset manager overlay (also hosts the Save-Drawer in Save mode)
    Scrim presetScrim;
    PresetManagerPanel presetManager;
    bool presetManagerVisible = false;
    juce::File currentPresetFile;
    PresetUpdater presetUpdater;

    // Sequence-pattern (.t5seq) library overlay — compact, hosted exactly
    // like the preset manager (centered panel over a dimmed scrim).
    Scrim seqLibraryScrim;
    SequenceLibraryPanel seqLibrary;
    bool seqLibraryVisible = false;

    // Manual overlay — native WebView renders the shipped HTML guide
    // (resources/akroasys_Guide.html), bundled via juce_add_binary_data.
    Scrim manualScrim;
    juce::Component manualPanel;
    juce::WebBrowserComponent manualWeb { juce::WebBrowserComponent::Options{} };
    juce::TextButton manualCloseBtn { "Close" };
    bool manualVisible = false;
    bool manualLoaded = false;
    juce::File manualHtmlOnDisk;  // temp extraction of the bundled HTML

    // Replay transport overlay — covers everything above the StatusBar while a
    // .t5evt tape plays (the tape drives the engine; this is its display). Declared
    // last: destroyed first, holds only processorRef.
    ReplayOverlay replayOverlay_ { processorRef };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainPanel)
};
