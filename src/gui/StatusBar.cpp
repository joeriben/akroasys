#include "StatusBar.h"
#include "GuiHelpers.h"

namespace
{
juce::String fitTextToWidth(const juce::Font& font, juce::String text, int maxWidth)
{
    text = text.trim();
    if (text.isEmpty() || maxWidth <= 0)
        return {};

    if (font.getStringWidth(text) <= maxWidth)
        return text;

    constexpr const char* suffix = "...";
    const int suffixW = font.getStringWidth(suffix);
    if (suffixW >= maxWidth)
        return {};

    int lo = 0;
    int hi = text.length();
    int best = 0;
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        auto candidate = text.substring(0, mid).trimEnd();
        if (font.getStringWidth(candidate) + suffixW <= maxWidth)
        {
            best = mid;
            lo = mid + 1;
        }
        else
        {
            hi = mid - 1;
        }
    }

    return text.substring(0, best).trimEnd() + suffix;
}
}

StatusBar::StatusBar()
{
    for (auto* btn : { &newBtn, &saveBtn, &loadBtn, &exportBtn, &sessionBtn, &settingsBtn, &manualBtn, &panicBtn, &keyboardBtn })
    {
        btn->setColour(juce::TextButton::buttonColourId, kSurface);
        btn->setColour(juce::TextButton::textColourOffId, kDim);
        addAndMakeVisible(btn);
    }

    keyboardBtn.setClickingTogglesState(true);
    keyboardBtn.setTooltip("Enable computer-keyboard note input");

    // Red text so the destructive action is visible. Same red as the
    // disconnect-state dot (kept inline rather than promoted to a palette
    // entry — Panic is the only red surface in the bar).
    panicBtn.setColour(juce::TextButton::textColourOffId, kError);
    panicBtn.setTooltip(juce::String::fromUTF8("MIDI Panic \xe2\x80\x94 release all hanging voices"));

    newBtn.onClick      = [this] { if (onNewPreset) onNewPreset(); };
    saveBtn.onClick     = [this] { if (onSavePreset) onSavePreset(); };
    loadBtn.onClick     = [this] { if (onLoadPreset) onLoadPreset(); };
    exportBtn.onClick   = [this] { if (onExportWav) onExportWav(); };
    sessionBtn.onClick  = [this]
    {
        // The session log in one place: recording it, saving it, playing one
        // back. Each file item ends in the standard async FileChooser handled by
        // the callbacks' owner (MainPanel). The separator divides the running
        // recording from a tape played back.
        juce::PopupMenu m;
        const bool wasRecording = sessionRecording_;
        m.addItem(1, "Record", onSessionRecordingChanged != nullptr, wasRecording);
        const bool hasLog = sessionLogAvailable && sessionLogAvailable();
        m.addItem(2, juce::String::fromUTF8("Save Session Log\xe2\x80\xa6"), hasLog && onSaveSessionLog != nullptr);
        m.addSeparator();
        const bool replaying = replayActive && replayActive();
        m.addItem(3, juce::String::fromUTF8("Play Session Log\xe2\x80\xa6"), onPlaySessionLog != nullptr && ! replaying);
        // SafePointer, not raw `this`: the callback fires later off the message
        // loop (matches the SafePointer idiom elsewhere in the GUI). Record flips
        // the state the menu SHOWED, not whatever it is when the choice lands.
        juce::Component::SafePointer<StatusBar> safeThis(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&sessionBtn),
            [safeThis, wasRecording](int r)
            {
                auto* self = safeThis.getComponent();
                if (self == nullptr) return;
                if      (r == 1) { if (self->onSessionRecordingChanged) self->onSessionRecordingChanged(! wasRecording); }
                else if (r == 2) { if (self->onSaveSessionLog)          self->onSaveSessionLog(); }
                else if (r == 3) { if (self->onPlaySessionLog)          self->onPlaySessionLog(); }
            });
    };
    settingsBtn.onClick = [this] { if (onSettings) onSettings(); };
    manualBtn.onClick   = [this] { if (onManual) onManual(); };
    panicBtn.onClick    = [this] { if (onMidiPanic) onMidiPanic(); };
    keyboardBtn.onClick = [this]
    {
        setKeyboardInputEnabled(keyboardBtn.getToggleState());
        if (onKeyboardInputChanged)
            onKeyboardInputChanged(keyboardBtn.getToggleState());
    };

    // MIDI output device combo + "Ext. Clock" toggle.
    // Standalone: these live in JUCE's "MIDI/Audio Settings" dialog instead
    // (MidiOutputSettingsPanel), keeping the bottom row clean — nothing left of Panic.
    // Plugin: no such dialog, so they stay here. The "XL Map" button is gone: selecting
    // an XL output applies its mapping automatically (PluginProcessor side).
    if (!standalone_)
    {
        midiOutCombo_.setColour(juce::ComboBox::backgroundColourId, kSurface);
        midiOutCombo_.setColour(juce::ComboBox::textColourId, kDim);
        midiOutCombo_.setColour(juce::ComboBox::outlineColourId, kSurface);
        midiOutCombo_.setTooltip("MIDI output device for LED feedback");
        addAndMakeVisible(midiOutCombo_);

        midiOutCombo_.onChange = [this]
        {
            if (!onMidiOutputDeviceChanged) return;
            const int idx = midiOutCombo_.getSelectedId();
            if (idx <= 1)
            {
                onMidiOutputDeviceChanged({});
            }
            else
            {
                // IDs 2..N map to device index (idx - 2)
                const auto devices = juce::MidiOutput::getAvailableDevices();
                const int devIdx = idx - 2;
                if (devIdx >= 0 && devIdx < devices.size())
                    onMidiOutputDeviceChanged(devices[devIdx].identifier);
            }
        };

        // "Ext. Clock" toggle — sync T5ynth BPM to incoming MIDI clock
        extClockBtn_.setClickingTogglesState(true);
        extClockBtn_.setColour(juce::TextButton::buttonColourId,  kSurface);
        extClockBtn_.setColour(juce::TextButton::textColourOffId, kDim);
        extClockBtn_.setColour(juce::TextButton::textColourOnId,  juce::Colours::white);
        extClockBtn_.setTooltip("Sync BPM to incoming MIDI clock (overrides host transport + Seq BPM)");
        extClockBtn_.onClick = [this]
        {
            if (onMidiClockEnabledChanged)
                onMidiClockEnabledChanged(extClockBtn_.getToggleState());
        };
        addAndMakeVisible(extClockBtn_);
    }

    refreshMidiOutputDevices();
}

void StatusBar::mouseDown(const juce::MouseEvent& e)
{
    // Right-click the preset name display to manage the loaded preset
    // (rename/delete/reveal) without going through the library browser.
    if (e.mods.isPopupMenu()
        && presetName.isNotEmpty()
        && presetNameBounds.contains(e.getPosition())
        && onPresetNameContextMenu)
    {
        onPresetNameContextMenu(e.getScreenPosition());
    }
}

void StatusBar::paint(juce::Graphics& g)
{
    g.fillAll(kCard);

    float h = static_cast<float>(getHeight());
    float dotSize = juce::jmax(5.0f, h * 0.30f);
    float dotX = 8.0f;
    g.setColour(backendConnected ? kSuccess : kError);
    g.fillEllipse(dotX, (h - dotSize) * 0.5f, dotSize, dotSize);

    float fs = juce::jlimit(10.0f, 14.0f, h * 0.55f);
    juce::Font font { juce::FontOptions(fs) };
    g.setFont(font);

    int textX = juce::roundToInt(dotX + dotSize + 6.0f);
    // Status text + preset name share the space left of the leftmost control.
    // Plugin: the MIDI-out combo is leftmost. Standalone: those controls moved into
    // the MIDI/Audio dialog, so Panic is now the leftmost control.
    int rightEdge = (standalone_ ? panicBtn.getX() : midiOutCombo_.getX()) - 8;
    auto statusBounds = juce::Rectangle<int>(textX, 0,
                                             juce::jmax(0, rightEdge - textX),
                                             getHeight());

    // Status text (left side)
    g.setColour(juce::Colour(0xffe3e3e3));
    {
        juce::Graphics::ScopedSaveState clip(g);
        g.reduceClipRegion(statusBounds);
        g.drawText(fitTextToWidth(font, statusText, statusBounds.getWidth()),
                   statusBounds,
                   juce::Justification::centredLeft,
                   false);
    }

    // Preset name (centered between status text and buttons). Bounds are
    // remembered so the right-click hit-test in mouseDown() lines up with
    // the painted text.
    if (presetName.isNotEmpty())
    {
        g.setColour(kAccent);
        const int nameW = juce::jmin(rightEdge - textX,
                                     font.getStringWidth(presetName) + 24);
        const int nameX = (textX + rightEdge - nameW) / 2;
        presetNameBounds = juce::Rectangle<int>(nameX, 0, nameW, getHeight());
        g.drawText(presetName, presetNameBounds, juce::Justification::centred);
    }
    else
    {
        presetNameBounds = {};
    }
}

void StatusBar::resized()
{
    auto b = getLocalBounds();
    int btnH = b.getHeight() - 2;
    int y = 1;
    int gap = 4;

    // Right to left: Manual, Settings, Session, Export, Library, Save, Init, Kbd, Panic.
    // Plugin only, further left: Ext. Clock, MIDI out combo (standalone moves these
    // into the MIDI/Audio dialog, so Panic is the leftmost control there).
    int manualW   = 60;
    int settingsW = 60;
    int sessionW  = 60;
    int exportW   = 54;
    int libraryW  = 64;
    int saveW     = 50;
    int newW      = 40;
    int kbdW      = 42;
    int panicW    = 50;
    int extClockW = 72;
    int comboW    = 130;

    manualBtn.setBounds(b.getRight() - manualW - gap, y, manualW, btnH);
    settingsBtn.setBounds(manualBtn.getX() - settingsW - gap, y, settingsW, btnH);
    sessionBtn.setBounds(settingsBtn.getX() - sessionW - gap, y, sessionW, btnH);
    exportBtn.setBounds(sessionBtn.getX() - exportW - gap, y, exportW, btnH);
    loadBtn.setBounds(exportBtn.getX() - libraryW - gap, y, libraryW, btnH);
    saveBtn.setBounds(loadBtn.getX() - saveW - gap, y, saveW, btnH);
    newBtn.setBounds(saveBtn.getX() - newW - gap, y, newW, btnH);
    keyboardBtn.setBounds(newBtn.getX() - kbdW - gap, y, kbdW, btnH);
    panicBtn.setBounds(keyboardBtn.getX() - panicW - gap, y, panicW, btnH);

    if (!standalone_)
    {
        extClockBtn_.setBounds(panicBtn.getX() - extClockW - gap, y, extClockW, btnH);
        midiOutCombo_.setBounds(extClockBtn_.getX() - comboW - gap, y, comboW, btnH);
    }
}

void StatusBar::setStatusText(const juce::String& text)
{
    statusText = text;
    repaint();
}

void StatusBar::setConnected(bool connected)
{
    backendConnected = connected;
    repaint();
}

void StatusBar::setPresetName(const juce::String& name)
{
    presetName = name;
    repaint();
}

void StatusBar::setReplayActive(bool active)
{
    for (auto* btn : { &newBtn, &saveBtn, &loadBtn })
        btn->setEnabled(! active);
    replayActive_ = active;
    refreshSessionIndicator();
}

void StatusBar::setUpdateBadge(bool available, const juce::String& version)
{
    if (updateBadge_ == available)
        return;
    updateBadge_ = available;
    settingsBtn.setTooltip(available
        ? "Update available (" + version + ") - open Settings to download"
        : juce::String());
    repaint();
}

void StatusBar::setSessionRecording(bool recording)
{
    if (sessionRecording_ == recording)
        return;
    sessionRecording_ = recording;
    refreshSessionIndicator();
}

void StatusBar::refreshSessionIndicator()
{
    const bool recordingNow = sessionRecording_ && ! replayActive_;
    sessionBtn.setTooltip(recordingNow ? "Recording the session log" : juce::String());
    repaint();
}

void StatusBar::paintOverChildren(juce::Graphics& g)
{
    // A small accent dot on a button's top-right corner (Chrome/VS Code
    // convention): on Settings while an update is pending (detail + Download
    // live in the General Settings page), on Session while the session log
    // records (Record on and no replay running). Painted over the child button
    // so it sits on top, and only while its state holds — zero layout cost, no
    // reserved slot.
    const auto paintDot = [&g](const juce::Component& btn)
    {
        const float d = 7.0f;
        const auto r = btn.getBounds().toFloat();
        const float cx = r.getRight() - d * 0.5f - 2.0f;
        const float cy = r.getY() + d * 0.5f + 2.0f;
        g.setColour(kCard);
        g.fillEllipse(cx - d * 0.5f - 1.0f, cy - d * 0.5f - 1.0f, d + 2.0f, d + 2.0f);
        g.setColour(kAccent);
        g.fillEllipse(cx - d * 0.5f, cy - d * 0.5f, d, d);
    };
    if (updateBadge_)
        paintDot(settingsBtn);
    if (sessionRecording_ && ! replayActive_)
        paintDot(sessionBtn);
}

void StatusBar::setKeyboardInputEnabled(bool enabled)
{
    keyboardBtn.setToggleState(enabled, juce::dontSendNotification);
    keyboardBtn.setColour(juce::TextButton::buttonColourId, enabled ? kAccent : kSurface);
    keyboardBtn.setColour(juce::TextButton::textColourOffId,
                          enabled ? juce::Colours::white : kDim);
    keyboardBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    keyboardBtn.repaint();
}

void StatusBar::refreshMidiOutputDevices()
{
    const auto currentId = midiOutCombo_.getSelectedId();
    midiOutCombo_.clear(juce::dontSendNotification);
    midiOutCombo_.addItem("No MIDI output", 1);
    const auto devices = juce::MidiOutput::getAvailableDevices();
    for (int i = 0; i < devices.size(); ++i)
        midiOutCombo_.addItem(devices[i].name, i + 2);
    // Restore selection if it was set
    if (currentId > 0)
        midiOutCombo_.setSelectedId(currentId, juce::dontSendNotification);
    else
        midiOutCombo_.setSelectedId(1, juce::dontSendNotification);
}

void StatusBar::setMidiClockState(bool enabled, bool active, float bpm)
{
    // Suppress repaints when nothing has changed (called at 30 Hz from timerCallback)
    const float bpmRounded = std::floor(bpm * 10.0f + 0.5f) / 10.0f;
    if (enabled == extClockLastEnabled_ && active == extClockLastActive_
        && bpmRounded == extClockLastBpm_)
        return;
    extClockLastEnabled_ = enabled;
    extClockLastActive_  = active;
    extClockLastBpm_     = bpmRounded;

    extClockBtn_.setToggleState(enabled, juce::dontSendNotification);

    if (!enabled)
    {
        extClockBtn_.setColour(juce::TextButton::buttonColourId, kSurface);
        extClockBtn_.setColour(juce::TextButton::textColourOffId, kDim);
        extClockBtn_.setTooltip("Sync BPM to incoming MIDI clock (overrides host transport + Seq BPM)");
    }
    else if (active)
    {
        extClockBtn_.setColour(juce::TextButton::buttonColourId, kSuccess.withAlpha(0.25f));
        extClockBtn_.setColour(juce::TextButton::textColourOffId, kSuccess);
        extClockBtn_.setTooltip(juce::String::fromUTF8("Ext. Clock active \xe2\x80\x94 ")
                                + juce::String(bpm, 1) + " BPM");
    }
    else
    {
        extClockBtn_.setColour(juce::TextButton::buttonColourId, kWarning.withAlpha(0.20f));
        extClockBtn_.setColour(juce::TextButton::textColourOffId, kWarning);
        extClockBtn_.setTooltip(juce::String::fromUTF8("Ext. Clock enabled \xe2\x80\x94 no signal received"));
    }
    extClockBtn_.repaint();
}

void StatusBar::setMidiOutputDeviceId(const juce::String& deviceId)
{
    if (deviceId.isEmpty())
    {
        midiOutCombo_.setSelectedId(1, juce::dontSendNotification);
        return;
    }
    const auto devices = juce::MidiOutput::getAvailableDevices();
    for (int i = 0; i < devices.size(); ++i)
    {
        if (devices[i].identifier == deviceId)
        {
            midiOutCombo_.setSelectedId(i + 2, juce::dontSendNotification);
            return;
        }
    }
    // Device not currently available — show "No MIDI output"
    midiOutCombo_.setSelectedId(1, juce::dontSendNotification);
}
