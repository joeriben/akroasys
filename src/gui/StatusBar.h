#pragma once
#include <JuceHeader.h>
#include <functional>

class StatusBar : public juce::Component
{
public:
    StatusBar();
    ~StatusBar() override = default;

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

    void setStatusText(const juce::String& text);
    void setConnected(bool connected);

    /** Show loaded preset name (empty = no preset). */
    void setPresetName(const juce::String& name);

    /** Shows/hides a small accent dot on the Settings button (Chrome/VS Code
     *  convention) — the download itself lives in the General Settings page.
     *  Called by MainPanel when the background UpdateChecker finds a newer
     *  GitHub release. `version` only feeds the Settings-button tooltip. */
    void setUpdateBadge(bool available, const juce::String& version = {});

    /** Callbacks for buttons. */
    std::function<void()> onNewPreset;
    std::function<void()> onSavePreset;
    std::function<void()> onLoadPreset;
    std::function<void()> onExportWav;
    std::function<void()> onSaveSessionLog;          // Export menu → save the current .t5evt
    std::function<bool()> sessionLogAvailable;       // enables the menu item iff a session has been recorded
    std::function<void()> onPlaySessionLog;          // Export menu → pick a .t5evt and replay it
    std::function<bool()> replayActive;              // gates the menu item while a tape runs (transport = ReplayOverlay)
    /** While a session-log replay runs, Init, Save and Library are off. The
     *  live patch is then the TAPE's: Save would store it as the user's preset,
     *  and Init or a load would change state under the tape that Stop puts back
     *  only in part. Same rule as the Play item above and the shortcuts
     *  MainPanel::keyPressed swallows while the overlay is up. Called by
     *  MainPanel with the overlay (beginReplayUi / endReplayUi). */
    void setReplayActive(bool active);
    std::function<void()> onSettings;
    std::function<void()> onManual;
    std::function<void(bool)> onKeyboardInputChanged;
    void setKeyboardInputEnabled(bool enabled);
    /** Fired when the user clicks the red "Panic" button — host should flush
     *  all hanging voices/notes. Momentary; no toggle state. */
    std::function<void()> onMidiPanic;
    /** Right-clicking the displayed preset name opens this menu — kept as
     *  a callback so MainPanel can populate it with rename/delete/etc. */
    std::function<void(juce::Point<int> screenPos)> onPresetNameContextMenu;

    // ── MIDI Output device selector ─────────────────────────────────────────
    /** Called when user selects a device. Empty string = no output. */
    std::function<void(const juce::String& deviceId)> onMidiOutputDeviceChanged;
    /** Sync the combo box to the currently-open device (call after processor restore). */
    void setMidiOutputDeviceId(const juce::String& deviceId);
    /** Refresh the device list (called on startup or when devices change). */
    void refreshMidiOutputDevices();

    // ── MIDI Clock toggle ────────────────────────────────────────────────────
    /** Fired when the user toggles "Ext. Clock". */
    std::function<void(bool enabled)> onMidiClockEnabledChanged;
    /** Called by MainPanel's timer to update button colour and tooltip. */
    void setMidiClockState(bool enabled, bool active, float bpm);

    void mouseDown(const juce::MouseEvent& e) override;

private:
    juce::String statusText = "Ready";
    juce::String presetName;
    bool backendConnected = false;
    juce::Rectangle<int> presetNameBounds;     // updated during paint() for hit-test

    juce::TextButton newBtn    { "Init" };
    juce::TextButton saveBtn   { "Save" };
    juce::TextButton loadBtn   { "Library" };
    juce::TextButton exportBtn { "Export" };
    juce::TextButton settingsBtn { "Settings" };
    juce::TextButton manualBtn { "Manual" };
    juce::TextButton panicBtn { "Panic" };
    juce::TextButton keyboardBtn { "Kbd" };
    bool updateBadge_ = false;   // accent dot on Settings when an update is available

    // In the standalone app these two controls live in JUCE's "MIDI/Audio Settings"
    // dialog (see MidiOutputSettingsPanel), so the bottom row is left clean — nothing
    // sits left of Panic. In a plugin (no such dialog) they remain here. The "XL Map"
    // button is gone entirely: selecting an XL output now applies its mapping
    // automatically (PluginProcessor::openMidiOutputDevice).
    const bool       standalone_ { juce::JUCEApplicationBase::isStandaloneApp() };
    juce::ComboBox   midiOutCombo_;
    juce::TextButton extClockBtn_   { "Ext.Clock" };

    // Cache to suppress unnecessary repaints in setMidiClockState (called 30 Hz)
    bool  extClockLastEnabled_ = false;
    bool  extClockLastActive_  = false;
    float extClockLastBpm_     = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StatusBar)
};
