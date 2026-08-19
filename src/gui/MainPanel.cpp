#include "MainPanel.h"
#include "../PluginProcessor.h"
#include "../dsp/BlockParams.h"
#include "../presets/TagVocabulary.h"
#include "../presets/CalibrationMigration.h"
#include "GuiHelpers.h"
#include "BinaryData.h"
#include "../ProductName.h"
#include "../StandaloneAudioInput.h"   // Resynth ext opens the input on demand
#include "PhysicalKeyState.h"   // t5::physicalKeyDown — layout-independent (isolated TU)
#include <cmath>
#include <cstring>
#include <thread>

namespace
{
// Computer-keyboard → note map, by PHYSICAL key POSITION (macOS virtual
// keycodes kVK_ANSI_*, from HIToolbox/Events.h — the same table JUCE itself uses
// in translateVirtualToAsciiKeyCode). Matching the hardware position, NOT the
// produced character, makes this layout-INDEPENDENT: the same physical keys play
// the same notes on US / German / French / … and the old z=G# layout bug is gone.
// Index = semitone above kComputerKeyboardBaseMidiNote. Standard Ableton-style
// typing layout: home row = white keys, the row above = black keys, each black
// key sitting above the GAP between its two white keys. Keys above a no-black-key
// gap (E–F and B–C) are simply unused — so [/Ü is intentionally NOT a note (it
// sits above the E–F gap, like R and I lower down). Range C4–G5 (20 keys); the
// macOS layout API confirms the physical map, so it stays layout-independent.
constexpr int kComputerKeyboardNoteKeys[] = {
    0x00, 0x0D, 0x01, 0x0E, 0x02, 0x03, 0x11, 0x05, 0x10, 0x04, 0x20, // C4 … A#4
    //C    C#    D     D#    E     F     F#    G     G#    A     A#   (a w s e d f t g y/z h u)
    0x26, 0x28, 0x1F, 0x25, 0x23, 0x29, 0x27, 0x1E, 0x2A             // B4 … G5
    //B    C     C#    D     D#    E     F     F#    G               (j k o l p ;/oe '/ae ]/+ #)
};
// Octave shift: the bottom-left two letters (physical Z-pos / X). US labels Z X,
// German labels Y X — same physical keys, so octave is layout-independent too.
constexpr int kComputerKeyboardOctaveDownKey = 0x06; // physical Z-position (DE Y)
constexpr int kComputerKeyboardOctaveUpKey   = 0x07; // physical X
constexpr int kComputerKeyboardBaseMidiNote = 60;
constexpr int kComputerKeyboardMinOctaveOffset = -5;
constexpr int kComputerKeyboardMaxOctaveOffset = 4;
constexpr const char* kUiSettingsFileName = "ui_settings.json";
constexpr const char* kOscEasyModeKey = "oscEasyMode";
constexpr const char* kSa3TierKey = "sa3Tier";

const char* const kMainSnapshotParamIds[] = {
    PID::genAlpha, PID::genMagnitude, PID::genNoise, PID::resynthAmount, PID::genDuration,
    PID::genStart, PID::genCfg, PID::genSeed, PID::genHfBoost, PID::infSteps,
    PID::engineMode, PID::voiceCount, PID::tuning, PID::loopMode,
    PID::crossfadeMs, PID::normalize, PID::loopOptimize,
    PID::oscScan, PID::oscOctave, PID::noiseLevel, PID::noiseType,
    PID::wtFrames, PID::wtSmooth, PID::wtAutoScan,
    PID::ampAttack, PID::ampDecay, PID::ampSustain, PID::ampRelease,
    PID::ampAmount, PID::ampLoop, PID::ampTarget,
    PID::ampAttackCurve, PID::ampDecayCurve, PID::ampReleaseCurve,
    PID::ampAttackVelSens, PID::ampDecayVelSens, PID::ampReleaseVelSens,
    PID::mod1Attack, PID::mod1Decay, PID::mod1Sustain, PID::mod1Release,
    PID::mod1Amount, PID::mod1Loop, PID::mod1Target,
    PID::mod1AttackCurve, PID::mod1DecayCurve, PID::mod1ReleaseCurve,
    PID::mod1AttackVelSens, PID::mod1DecayVelSens, PID::mod1ReleaseVelSens,
    PID::mod2Attack, PID::mod2Decay, PID::mod2Sustain, PID::mod2Release,
    PID::mod2Amount, PID::mod2Loop, PID::mod2Target,
    PID::mod2AttackCurve, PID::mod2DecayCurve, PID::mod2ReleaseCurve,
    PID::mod2AttackVelSens, PID::mod2DecayVelSens, PID::mod2ReleaseVelSens,
    PID::mod3Attack, PID::mod3Decay, PID::mod3Sustain, PID::mod3Release,
    PID::mod3Amount, PID::mod3Loop, PID::mod3Target,
    PID::mod3AttackCurve, PID::mod3DecayCurve, PID::mod3ReleaseCurve,
    PID::mod3AttackVelSens, PID::mod3DecayVelSens, PID::mod3ReleaseVelSens,
    PID::mod4Attack, PID::mod4Decay, PID::mod4Sustain, PID::mod4Release,
    PID::mod4Amount, PID::mod4Loop, PID::mod4Target,
    PID::mod4AttackCurve, PID::mod4DecayCurve, PID::mod4ReleaseCurve,
    PID::mod4AttackVelSens, PID::mod4DecayVelSens, PID::mod4ReleaseVelSens,
    PID::lfo1Rate, PID::lfo1Depth, PID::lfo1Wave, PID::lfo1Target, PID::lfo1Mode,
    PID::lfo1ClockMode, PID::lfo1ClockDivision,
    PID::lfo2Rate, PID::lfo2Depth, PID::lfo2Wave, PID::lfo2Target, PID::lfo2Mode,
    PID::lfo2ClockMode, PID::lfo2ClockDivision,
    PID::lfo3Rate, PID::lfo3Depth, PID::lfo3Wave, PID::lfo3Target, PID::lfo3Mode,
    PID::lfo3ClockMode, PID::lfo3ClockDivision,
    PID::aftertouchAmtLfo1Depth, PID::aftertouchAmtLfo2Depth, PID::aftertouchAmtLfo3Depth,
    PID::aftertouchAmtEnv1Sustain, PID::aftertouchAmtEnv2Sustain, PID::aftertouchAmtEnv3Sustain,
    PID::aftertouchAmtCutoff, PID::aftertouchAmtResonance, PID::aftertouchAmtScan,
    PID::aftertouchAmtDca, PID::aftertouchAmtPitch, PID::aftertouchAmtNoiseLevel,
    PID::aftertouchAmtEnv4Sustain, PID::aftertouchAmtEnv5Sustain,
    PID::driftEnabled, PID::driftRegen, PID::driftCrossfade,
    PID::drift1Rate, PID::drift1Depth, PID::drift1Target, PID::drift1Wave,
    PID::drift1ClockMode, PID::drift1ClockDivision,
    PID::drift2Rate, PID::drift2Depth, PID::drift2Target, PID::drift2Wave,
    PID::drift2ClockMode, PID::drift2ClockDivision,
    PID::drift3Rate, PID::drift3Depth, PID::drift3Target, PID::drift3Wave,
    PID::drift3ClockMode, PID::drift3ClockDivision,
    PID::filterEnabled, PID::filterType, PID::filterSlope, PID::filterCutoff,
    PID::filterResonance, PID::filterMix, PID::filterKbdTrack, PID::filterDrive,
    PID::filterDriveOs, PID::filterAlgorithm, PID::filterWarpStyle
};

// The ids above an LCO SNAP deliberately does NOT restore. Everything else in
// kMainSnapshotParamIds shapes a Csound voice exactly as it shapes a neural one
// (envelopes, LFOs, drift modulators, filter, aftertouch, noise, voices, tuning),
// so the LCO slot carries the same sound state rather than a second, divergent
// list. Excluded here:
//   - engineMode: the mode toggle and the bake own it (a slot must never move the
//     instrument between paradigms — CLAUDE.md platform invariants). The recall
//     forces Csound itself, and only when it actually has an orchestra.
//   - the neural generation + sample-path settings: they have no meaning for a
//     live oscillator, and a LCO recall must not silently rewrite the T5osc
//     session the user returns to (seed, duration, steps, loop points, the
//     wavetable extraction, the sample regen rate).
// NOT excluded, despite sitting in the same Drift block as the regen rate:
// driftCrossfade ("Regen XFade") IS the Csound swap fade — processBlock derives
// csoundFadeLen_ from it — so it is the shape of the very transition a recall
// performs, and belongs to the stored sound.
const char* const kLcoSnapshotSkipParamIds[] = {
    PID::engineMode,
    PID::genAlpha, PID::genMagnitude, PID::genNoise, PID::genDuration,
    PID::genStart, PID::genCfg, PID::genSeed, PID::genHfBoost, PID::infSteps,
    PID::resynthAmount,
    PID::loopMode, PID::crossfadeMs, PID::normalize, PID::loopOptimize,
    PID::oscScan, PID::wtFrames, PID::wtSmooth, PID::wtAutoScan,
    PID::driftRegen
};

bool isLcoSnapshotSkippedParam(const char* id)
{
    for (auto* skip : kLcoSnapshotSkipParamIds)
        if (juce::String(skip) == id)
            return true;
    return false;
}

bool findParameterValue(const juce::ValueTree& state, const juce::String& id, float& value)
{
    for (int i = 0; i < state.getNumChildren(); ++i)
    {
        auto child = state.getChild(i);
        if (child.getProperty("id").toString() == id && child.hasProperty("value"))
        {
            value = static_cast<float>(static_cast<double>(child.getProperty("value")));
            return true;
        }
    }
    return false;
}

void restoreParameterFromState(juce::AudioProcessorValueTreeState& apvts,
                               const juce::ValueTree& state,
                               const char* id)
{
    auto* param = apvts.getParameter(id);
    if (param == nullptr)
        return;

    // A parameter the stored slot does not carry is restored to its DEFAULT, not
    // left where the previous recall put it. A slot written before a parameter
    // existed otherwise leaks that parameter across recalls — the same failure
    // the BPM-clock defaults in PluginProcessor::setStateInformation exist to
    // prevent, and the one ENV4/5 would hit against every older snapshot.
    float value = 0.0f;
    param->setValueNotifyingHost(findParameterValue(state, id, value)
                                     ? param->convertTo0to1(value)
                                     : param->getDefaultValue());
}

juce::File getUiSettingsFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("T5ynth")
        .getChildFile(kUiSettingsFileName);
}

#if JUCE_WINDOWS
juce::StringArray getWindowsCompanionBackendRoots()
{
    juce::StringArray roots;
    auto addRoot = [&roots](const juce::String& path)
    {
        auto trimmed = path.trim();
        if (trimmed.isNotEmpty())
            roots.addIfNotAlreadyThere(trimmed);
    };

    constexpr auto wow64 = juce::WindowsRegistry::WoW64_64bit;

    // Both product names, akroasys first. The installer writes Software\akroasys
    // now; Software\T5ynth is what an existing install left behind, and its
    // backend works perfectly well for this plugin. Dropping it would leave a
    // Windows VST3 saying "Backend not found" next to a working T5ynth install.
    for (const char* product : { "akroasys", "T5ynth" })
    {
        const juce::String key = juce::String("\\Software\\") + product;

        addRoot(juce::WindowsRegistry::getValue("HKEY_LOCAL_MACHINE" + key + "\\BackendDir", {}, wow64));
        addRoot(juce::WindowsRegistry::getValue("HKEY_CURRENT_USER" + key + "\\BackendDir", {}, wow64));

        for (const char* hive : { "HKEY_LOCAL_MACHINE", "HKEY_CURRENT_USER" })
        {
            const auto installDir = juce::WindowsRegistry::getValue(juce::String(hive) + key + "\\InstallDir", {}, wow64);
            if (installDir.isNotEmpty())
                addRoot(juce::File(installDir).getChildFile("backend").getFullPathName());
        }

        addRoot(juce::String("C:\\Program Files\\") + product + "\\backend");
        addRoot(juce::String("C:\\Program Files (x86)\\") + product + "\\backend");
    }
    return roots;
}
#endif

}

MainPanel::GenerateButton::GenerateButton(const juce::String& label)
    : juce::TextButton(label)
{
    setTooltip("Generate audio from the current impulses and latent controls");
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void MainPanel::GenerateButton::setAnimationState(float phase, bool isGenerating)
{
    if (!isGenerating && !generating)
        return;
    animationPhase = phase;
    generating = isGenerating;
    repaint();
}

void MainPanel::GenerateButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    auto bounds = getLocalBounds().toFloat();
    if (bounds.getWidth() <= 0.0f || bounds.getHeight() <= 0.0f)
        return;

    const bool active = isEnabled() || generating;
    const auto label = getButtonText().trim().isNotEmpty() ? getButtonText().trim()
                                                           : juce::String("GENERATE");

    // Section-header style: same kOscCol fill as oscHeader / the axes|dim
    // segment switch above this button. Flat, sharp corners, no border.
    // The button reads as the active counterpart to those headers — the
    // primary action of the prompt/oscillator section.
    auto fill = kOscCol;
    if (down)            fill = fill.darker(0.10f);
    else if (highlighted) fill = fill.brighter(0.10f);

    if (generating)
    {
        // Pulse fades the fill toward the page background (kBg, near-black)
        // and back. A clearly visible "thinking / working" signal that
        // stays inside the flat design language — no sweep, no stripe.
        const float pulse = 0.5f + 0.5f * std::sin(animationPhase);
        fill = fill.interpolatedWith(kBg, pulse * 0.70f);
    }

    if (! active)
        fill = fill.withAlpha(0.45f);

    g.setColour(fill);
    g.fillRect(bounds);

    // Label — light on accent, like paintSectionHeader. Font size is bounded
    // by BOTH height and width so the label can't grow out of proportion when
    // the button is short and wide (or tall and narrow).
    const float fromHeight = bounds.getHeight() * 0.50f;
    const float fromWidth  = bounds.getWidth() / 11.0f;  // ~"GENERATE" + chevrons
    const float fontSize = juce::jlimit(16.0f, 30.0f, juce::jmin(fromHeight, fromWidth));
    const float chevronSize = fontSize * 0.50f;

    // Three chevrons → "forward / run / advance" mark, clearer than one.
    const auto chevronStr = juce::String::charToString(0x276F)
                          + juce::String::charToString(0x276F)
                          + juce::String::charToString(0x276F);

    // Inverted button text follows the system convention used by every other
    // active/toggled button in the L&F: TextButton::textColourOnId = kTextPrimary
    // (near-white). Section labels (paintSectionHeader / kHeaderText) now use the
    // same light text on the accent fill, so button and header agree.
    const auto fgText = kTextPrimary;
    const float textAlpha = active ? (down ? 0.85f : 1.0f) : 0.55f;

    auto font = juce::Font(juce::FontOptions(fontSize, juce::Font::bold))
                    .withExtraKerningFactor(0.10f);
    auto chevronFont = juce::Font(juce::FontOptions(chevronSize, juce::Font::bold))
                           .withExtraKerningFactor(-0.05f);

    // Measure with GlyphArrangement — Font::getStringWidthFloat ignores
    // extraKerningFactor in this JUCE build, which previously clipped the
    // last glyph of "GENERATE". GlyphArrangement respects all font attributes.
    auto measureWith = [](const juce::Font& f, const juce::String& s) -> float
    {
        if (s.isEmpty()) return 0.0f;
        juce::GlyphArrangement ga;
        ga.addLineOfText(f, s, 0.0f, 0.0f);
        return ga.getBoundingBox(0, -1, true).getWidth();
    };

    // Compose label + chevrons as a single centered unit so the chevrons
    // sit immediately next to the word rather than pinned to the right edge.
    const float labelW = measureWith(font, label);
    const float chevW  = measureWith(chevronFont, chevronStr);
    const float gap    = fontSize * 0.55f;
    const float compositeW = labelW + gap + chevW;

    const float minPad = 8.0f;
    const float subPxPad = 2.0f;  // absorbs sub-pixel rounding from .toNearestInt()
    const bool fits = compositeW + minPad * 2.0f <= bounds.getWidth();

    if (fits)
    {
        const float startX = juce::jmax(minPad, bounds.getCentreX() - compositeW * 0.5f);
        auto labelArea = juce::Rectangle<float>(startX, bounds.getY(),
                                                 labelW + subPxPad, bounds.getHeight());
        auto chevArea  = juce::Rectangle<float>(startX + labelW + gap, bounds.getY(),
                                                 chevW + subPxPad, bounds.getHeight());

        g.setFont(font);
        g.setColour(fgText.withAlpha(textAlpha));
        g.drawText(label, labelArea.toNearestInt(),
                   juce::Justification::centredLeft, false);

        g.setFont(chevronFont);
        g.setColour(fgText.withAlpha(textAlpha * 0.75f));
        g.drawText(chevronStr, chevArea.toNearestInt(),
                   juce::Justification::centredLeft, false);
    }
    else
    {
        // Extremely narrow: drop chevrons, fit label only.
        g.setFont(font);
        g.setColour(fgText.withAlpha(textAlpha));
        g.drawFittedText(label, bounds.reduced(minPad, 0.0f).toNearestInt(),
                         juce::Justification::centred, 1, 0.5f);
    }
}

// ─── SnapshotButton ─────────────────────────────────────────────────────────
void MainPanel::SnapshotButton::setSnapshotIndex(int index)
{
    snapshotIndex = index;
}

void MainPanel::SnapshotButton::setSnapshotFilled(bool filled)
{
    if (snapshotFilled != filled)
    {
        snapshotFilled = filled;
        repaint();
    }
}

void MainPanel::SnapshotButton::flashStored()
{
    flashUntilMs = juce::Time::getMillisecondCounterHiRes() + 430.0;
    startTimerHz(30);
    repaint();
}

void MainPanel::SnapshotButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    auto& lf = getLookAndFeel();
    auto base = findColour(getToggleState() ? juce::TextButton::buttonOnColourId
                                            : juce::TextButton::buttonColourId);
    lf.drawButtonBackground(g, *this, base, highlighted, down);
    lf.drawButtonText(g, *this, highlighted, down);

    const double now = juce::Time::getMillisecondCounterHiRes();
    if (flashUntilMs > now)
    {
        const float alpha = static_cast<float>((flashUntilMs - now) / 430.0);
        g.setColour(juce::Colours::white.withAlpha(0.28f * juce::jlimit(0.0f, 1.0f, alpha)));
        g.fillRect(getLocalBounds());
    }

    if (snapshotIndex > 0 && snapshotFilled)
    {
        auto dot = getLocalBounds().reduced(3, 0).removeFromBottom(2);
        g.setColour((getToggleState() ? juce::Colours::white : kOscCol).withAlpha(0.85f));
        g.fillRect(dot);
    }

    if (snapshotIndex > 0 && pressing && !longFired)
    {
        g.setColour(kOscCol.withAlpha(0.8f));
        g.drawRect(getLocalBounds().reduced(1), 1);
    }
}

void MainPanel::SnapshotButton::mouseDown(const juce::MouseEvent& e)
{
    pressing = true;
    longFired = false;
    pressStartMs = juce::Time::getMillisecondCounterHiRes();
    if (snapshotIndex > 0 && onPressStarted)
        onPressStarted(snapshotIndex);
    startTimerHz(30);
    juce::TextButton::mouseDown(e);
    repaint();
}

void MainPanel::SnapshotButton::mouseUp(const juce::MouseEvent& e)
{
    const bool wasShort = pressing && !longFired;
    pressing = false;
    if (wasShort && onShortActivate)
        onShortActivate(snapshotIndex);
    if (flashUntilMs <= juce::Time::getMillisecondCounterHiRes())
        stopTimer();
    juce::TextButton::mouseUp(e);
    repaint();
}

void MainPanel::SnapshotButton::mouseExit(const juce::MouseEvent& e)
{
    juce::TextButton::mouseExit(e);
    repaint();
}

void MainPanel::SnapshotButton::timerCallback()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (pressing && !longFired && snapshotIndex > 0 && now - pressStartMs >= 650.0)
    {
        longFired = true;
        if (onLongStore)
            onLongStore(snapshotIndex);
    }

    if (!pressing && flashUntilMs <= now)
        stopTimer();

    repaint();
}

// ─── CacheCapButton ──────────────────────────────────────────────────────────
// Replaces the dropped "inference cache N/M" status row: while the cache is
// filling, the *selected* capacity button pulses its fill colour subtly. When
// the cache is full, pulsing stops and the button sits at solid kOscCol.
void MainPanel::CacheCapButton::setPulsing(bool p)
{
    if (pulsing != p)
    {
        pulsing = p;
        repaint();
    }
}

void MainPanel::CacheCapButton::setPulsePhase(float phase)
{
    lastPhase = phase;
    if (pulsing && getToggleState())
        repaint();
}

void MainPanel::CacheCapButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    auto& lf = getLookAndFeel();
    auto base = findColour(getToggleState() ? juce::TextButton::buttonOnColourId
                                            : juce::TextButton::buttonColourId);

    const bool isPulsing = pulsing && getToggleState();
    float p = 0.0f;
    if (isPulsing)
    {
        p = 0.5f + 0.5f * std::sin(lastPhase);
        base = base.brighter(0.45f * p);
    }

    lf.drawButtonBackground(g, *this, base, highlighted, down);

    if (isPulsing)
    {
        // White overlay tick adds a clearly-visible blink layer on top of
        // the brightness pulse — without it, the brighten-only effect
        // disappears on small buttons at typical viewing distance.
        g.setColour(juce::Colours::white.withAlpha(0.22f * p));
        g.fillRect(getLocalBounds());
    }

    lf.drawButtonText(g, *this, highlighted, down);
}

MainPanel::MainPanel(T5ynthProcessor& processor)
    : processorRef(processor),
      promptPanel(processor),
      axesPanel(processor.getValueTreeState()),
      synthPanel(processor),
      fxPanel(processor.getValueTreeState(), processor),
      sequencerPanel(processor)
{
    setOpaque(true);
    computerKeyboardActiveNotes.fill(-1);
    // Feeds t5::physicalKeyDown from this app's own key events. Ref-counted, so a
    // second plugin editor in the same host shares the one monitor; ~MainPanel
    // hands its reference back.
    t5::startPhysicalKeyMonitor();
    // Allow keyboard shortcuts (⌘S) to reach the panel even when no inner
    // text editor has focus.
    setWantsKeyboardFocus(true);
    addAndMakeVisible(promptPanel);
    addAndMakeVisible(axesPanel);
    addAndMakeVisible(synthPanel);
    addAndMakeVisible(fxPanel);
    addAndMakeVisible(sequencerPanel);
    addAndMakeVisible(statusBar);
    addChildComponent(replayOverlay_);   // hidden until a tape plays; begin()/end()

    // Left column section headers
    paintSectionHeader(oscHeader, "T5 OSCILLATOR", kOscCol);
    addAndMakeVisible(oscHeader);

    poweredByLabel.setText("Powered by Stability AI", juce::dontSendNotification);
    labelAsCaption(poweredByLabel, kBg.withAlpha(0.7f));   // dark sub-label on the periwinkle header band
    poweredByLabel.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(poweredByLabel);

    oscModeToggle.setColour(juce::TextButton::buttonColourId, kSurface.darker(0.45f));
    oscModeToggle.setColour(juce::TextButton::buttonOnColourId, kSurface.darker(0.45f));
    oscModeToggle.setColour(juce::TextButton::textColourOffId, kOscCol);
    oscModeToggle.setColour(juce::TextButton::textColourOnId, kOscCol);
    oscModeToggle.setTooltip("Switch oscillator mode: T5osc (neural generator) / LRO (language-resonant)");
    oscModeToggle.onClick = [this] {
        const bool wantNeural = !oscEasyMode;
        setOscEasyMode(wantNeural, true);
        applyOscModeToEngine(wantNeural);
    };
    addAndMakeVisible(oscModeToggle);

    // Semantic Axes | Dim Explorer — a 2-segment switch (switchbox template, same
    // as the resynth int/ext toggle) replacing the two old section headers. The
    // active segment shows either the Axes controls or the DimExplorer mini-view
    // in one shared fixed-height box below. Default = Semantic Axes.
    {
        static constexpr const char* segLabels[2] = { "Semantic Axes", "Dim Explorer" };
        for (int i = 0; i < 2; ++i)
        {
            auto& s = axesDimSegBtns[i];
            s.setButtonText(segLabels[i]);
            styleSwitchButton(s, kOscCol);
            s.setClickingTogglesState(true);
            s.setRadioGroupId(3021);   // fresh id (3019 resynth src, 3017 cache)
            s.setConnectedEdges(i == 0 ? juce::Button::ConnectedOnRight
                                       : juce::Button::ConnectedOnLeft);
            s.onClick = [this, i] { showDimSegment_ = (i == 1); updateAxesDimSegment(); };
            addAndMakeVisible(s);
        }
        axesDimSegBtns[0].setToggleState(true, juce::dontSendNotification);
    }

    // Axes description note is inside AxesPanel

    // Wire StatusBar buttons
    statusBar.onNewPreset    = [this] { loadInitPreset(); };
    statusBar.onSavePreset   = [this] { savePreset(); };
    statusBar.onLoadPreset   = [this] { loadPreset(); };
    statusBar.onExportWav    = [this] { exportWav(); };
    statusBar.onSaveSessionLog   = [this] { saveSessionLog(); };
    statusBar.sessionLogAvailable = [this] { return processorRef.getEventLogCurrentFile().existsAsFile(); };
    statusBar.onPlaySessionLog   = [this] { loadReplaySession(); };
    statusBar.replayActive       = [this] { return processorRef.isReplayActive(); };
    replayOverlay_.onStop = [this]
    {
        processorRef.stopReplay();
        replayOverlay_.end();
        statusBar.setStatusText(juce::String::fromUTF8("Replay stopped \xe2\x80\x94 patch restored"));
    };
    // Cleared in ~MainPanel: the processor outlives this panel and would otherwise
    // call into a dead `this` when a tape reaches its end.
    processorRef.onReplayFinished = [this]
    {
        replayOverlay_.end();
        statusBar.setStatusText(juce::String::fromUTF8("Replay finished \xe2\x80\x94 patch restored"));
    };
    statusBar.onSettings     = [this] { if (settingsVisible) hideSettings(); else showSettings(); };
    statusBar.onManual       = [this] { showManual(); };
    statusBar.onMidiPanic    = [this] { processorRef.requestMidiPanic(); };
    statusBar.onKeyboardInputChanged = [this](bool enabled) { setComputerKeyboardEnabled(enabled); };
    statusBar.onPresetNameContextMenu = [this](juce::Point<int> p) { showPresetNameContextMenu(p); };
    statusBar.setKeyboardInputEnabled(false);

    processorRef.onMidiLearnStateChanged = [this](bool learning, int boundCc) {
        if (learning)
            statusBar.setStatusText("MIDI Learn: touch a CC on your controller...");
        else if (boundCc >= 0)
            statusBar.setStatusText("Assigned to CC " + juce::String(boundCc));
        // cancelled / aborted: leave the current status text unchanged
    };

    // XL "Generate" button (CC 37) → main generation (same path as the on-screen button).
    processorRef.onGenerateRequested = [this] { triggerMainGeneration(); };

    // XL snapshot buttons (CC 45-48) → recall slot 1-4 (same path as the on-screen
    // snapshot buttons; activateSnapshot restores the params and syncs the UI).
    processorRef.onSnapshotRequested = [this](int slot) { activateSnapshot(slot); };

    // XL cache button (CC 49) → toggle the inference cache between 4 and Off, mirroring
    // the on-screen radio buttons (read current, flip, then refresh the radio UI).
    processorRef.onCacheToggleRequested = [this]
    {
        const int cur = processorRef.getInferenceCacheCapacity();
        processorRef.setInferenceCacheCapacity(cur == 4 ? 0 : 4);
        syncInferenceCacheUi();
    };

    // MIDI output device selector
    statusBar.onMidiOutputDeviceChanged = [this](const juce::String& deviceId)
    {
        processorRef.openMidiOutputDevice(deviceId);
    };
    statusBar.refreshMidiOutputDevices();
    statusBar.setMidiOutputDeviceId(processorRef.getMidiOutputDeviceId());

    statusBar.onMidiClockEnabledChanged = [this](bool e)
    {
        processorRef.setMidiClockEnabled(e);
    };
    statusBar.setMidiClockState(processorRef.isMidiClockEnabled(),
                                processorRef.isMidiClockActive(),
                                processorRef.getMidiClockBpm());

    // Settings overlay (same pattern as DimExplorer)
    settingsScrim.onClick = [this] { hideSettings(); };
    settingsScrim.setVisible(false);
    addChildComponent(settingsScrim);

    // Settings overlay is tabbed, in the order a user meets the instrument:
    // "Sound Models" (the generation engines, default-open), then "Language
    // Model" (the LRO's author — local install or an external API, one subject
    // on one page), then "Settings" (global options). The language model used
    // to be offered among the sound models on the first tab while a third tab
    // explained it; nobody could follow that, so the install row itself now
    // lives on the Language Model tab (adoptCoderRow below).
    // deleteWhenNotNeeded=false — MainPanel owns the pages; the TabbedComponent
    // only displays them (and reparents them).
    settingsTabs.addTab("Sound Models",   kBg, &settingsPage,          false);
    settingsTabs.addTab("Language Model", kBg, &lroAuthorSettingsPage, false);
    settingsTabs.addTab("Settings",       kBg, &generalSettingsPage,   false);
    settingsTabs.setCurrentTabIndex(0);
    settingsTabs.setOutline(0);
    addChildComponent(settingsTabs);

    generalSettingsPage.onOsQualityChanged = [this](int idx) { processorRef.setFilterOsQuality(idx); };
    generalSettingsPage.setOsQuality(processorRef.getFilterOsQuality());
    generalSettingsPage.onLroOsQualityChanged = [this](int idx) { processorRef.setLroOsQuality(idx); };
    generalSettingsPage.setLroOsQuality(processorRef.getLroOsQuality());

    generalSettingsPage.onCheckForUpdatesChanged = [this](bool enabled) { processorRef.setCheckForUpdatesEnabled(enabled); };
    generalSettingsPage.setCheckForUpdatesEnabled(processorRef.getCheckForUpdatesEnabled());

    generalSettingsPage.onEventLogEnabledChanged = [this](bool enabled) { processorRef.setEventLogEnabled(enabled); };
    generalSettingsPage.setEventLogEnabled(processorRef.getEventLogEnabled());

    settingsPage.onModelReady = [this]
    {
        if (promptPanel.isGenerating())
        {
            pendingInferenceReload = true;
            statusBar.setStatusText("Model installed. Backend reload is queued.");
            return;
        }

        tryLoadInferenceModels(true);
    };

    // Gate every LLM-dependent control (the LCO bake and its model tab, the
    // Re-Prompt stance bar + coupling, the Translate flag) on the language
    // author's availability: disabled and explained via tooltip when absent,
    // re-enabled the instant it is available (no restart). ONE author does all
    // three jobs (translate/interpret/csound), so it is one gate — now
    // satisfied by EITHER the local model being installed OR the LRO Author
    // tab's external API being fully configured (an alternative engine, not a
    // fallback: never silently prefers one over the other, see
    // LroAuthorSettingsPage's own doc comment). The tab also NAMES whichever
    // one WOULD author right now; each bake's own author_model claim still
    // overrides this static label on the wire once a generation actually runs.
    auto refreshLlmAvailability = [this]
    {
        const bool localReady = settingsPage.isCoderModelInstalled();
        const bool externalReady = lroAuthorSettingsPage.isExternallyConfigured();
        promptPanel.setLcoResolvedModel(
            externalReady ? (lroAuthorSettingsPage.getConfig().apiModel + " (external)")
                          : settingsPage.resolvedCoderDirName());
        promptPanel.setLlmAvailable(localReady || externalReady);
    };

    settingsPage.onCoderModelChanged = [this, refreshLlmAvailability](bool)
    {
        refreshLlmAvailability();
    };
    lroAuthorSettingsPage.onConfigChanged = [this, refreshLlmAvailability]
    {
        processorRef.setLroAuthorProviderConfig(lroAuthorSettingsPage.getConfig());
        refreshLlmAvailability();
    };

    // The language model's install row moves onto the Language Model tab, where
    // it is explained. SettingsPage keeps owning it and every download it runs;
    // only the visual parent changes. Safe by declaration order: this page is
    // declared after settingsPage, so it destructs first and un-parents the row
    // before its owner deletes it.
    lroAuthorSettingsPage.adoptCoderRow(settingsPage.coderRowComponent());
    // The running API bill, pulled when the tab is opened. Reading it takes the
    // spend mutex only, never stateMutex_, so it cannot block behind an
    // authoring that is in flight — which is exactly when a user goes looking.
    lroAuthorSettingsPage.apiSpendSource = [this]
    {
        return processorRef.getPipeInference().authorApiSpend();
    };
    lroAuthorSettingsPage.setConfig(processorRef.getLroAuthorProviderConfig());
    refreshLlmAvailability();

    presetScrim.onClick = [this] { hidePresetManager(); };
    presetScrim.setVisible(false);
    addChildComponent(presetScrim);

    // ── Sequence-pattern library overlay ────────────────────────────────
    // Pure view: Load/Save need the processor, so they route through the
    // SequencerPanel; Delete/Rename are file ops the panel does itself.
    seqLibraryScrim.onClick = [this] { hideSequenceLibrary(); };
    seqLibraryScrim.setVisible(false);
    addChildComponent(seqLibraryScrim);

    seqLibrary.setVisible(false);
    seqLibrary.onCloseRequested = [this] { hideSequenceLibrary(); };
    seqLibrary.onLoadRequested = [this](const juce::File& file)
    {
        return sequencerPanel.loadPatternFrom(file);
    };
    seqLibrary.onSaveRequested = [this](const juce::File& file)
    {
        return sequencerPanel.writePatternTo(file);
    };
    addChildComponent(seqLibrary);

    sequencerPanel.onOpenPatternLibrary = [this](bool focusSave)
    {
        showSequenceLibrary(focusSave);
    };

    presetManager.setVisible(false);
    presetManager.onCloseRequested = [this] { hidePresetManager(); };
    presetManager.onLoadRequested = [this](const juce::File& file)
    {
        if (loadPresetFromFile(file))
            hidePresetManager();
        else
            presetManager.setStatusText("Preset load failed", true);
    };
    presetManager.onImportRequested = [this] { importPresetFile(); };
    presetManager.onGetFromGithubRequested = [this]
    {
        if (presetUpdater.isRunning())
            return;

        presetManager.setUpdaterBusy(true, "Contacting GitHub...");

        juce::Component::SafePointer<MainPanel> safeThis(this);
        presetUpdater.start(
            [safeThis](double, juce::String message)
            {
                if (auto* self = safeThis.getComponent())
                    self->presetManager.setUpdaterBusy(true, message);
            },
            [safeThis](bool success, PresetUpdater::Stats stats, juce::String error)
            {
                if (auto* self = safeThis.getComponent())
                    self->finishPresetUpdate(success, stats, error);
            });
    };
    presetManager.onSaveRequested = [this](const juce::String& presetName,
                                           const juce::StringArray& tags,
                                           const juce::String& bank,
                                           bool includeInferenceCache)
    {
        const auto userDir = PresetFormat::getUserPresetsDirectory();
        auto bankDir = userDir;
        if (bank.isNotEmpty())
            bankDir = bankDir.getChildFile(bank);
        bankDir.createDirectory();

        // String-concat (not withFileExtension) — withFileExtension strips
        // at the last dot, so a user-typed name like "Pad 0.5" would be
        // truncated to "Pad 0.t5p" and could clobber an unrelated preset.
        const auto fileName = presetName + ".t5p";
        auto target = bankDir.getChildFile(fileName);

        // Cross-bank uniqueness gate: a preset name must be unique across
        // every bank. Without this, "Pad" could exist in both My Presets
        // and "UCDCAE AI Lab" — different files, identical labels in the
        // library list — which is the duplication users hit before the
        // fork-on-edit rule landed. Same-bank replace is still allowed
        // (handled by the dialog below); only sibling-bank collisions are
        // blocked here.
        juce::File collidingFile;
        juce::String collidingBank;
        if (bank.isNotEmpty())
        {
            auto rootCandidate = userDir.getChildFile(fileName);
            if (rootCandidate.existsAsFile())
            {
                collidingFile = rootCandidate;
                collidingBank = PresetManagerPanel::kRootUserBank();
            }
        }
        if (! collidingFile.existsAsFile())
        {
            for (auto& d : userDir.findChildFiles(juce::File::findDirectories, false, "*"))
            {
                if (d.getFileName().equalsIgnoreCase(bank))
                    continue;
                auto cand = d.getChildFile(fileName);
                if (cand.existsAsFile())
                {
                    collidingFile = cand;
                    collidingBank = d.getFileName();
                    break;
                }
            }
        }
        // Bank label shown in the Replace confirmation below. Normally the
        // user-chosen target bank; on the maintainer redirect (see below) it
        // becomes the bank that actually owns the colliding file.
        const juce::String chosenBankLabel = bank.isEmpty()
                                            ? PresetManagerPanel::kRootUserBank() : bank;
        juce::String replaceBankLabel = chosenBankLabel;
        // True once the redirect has moved the save off the bank the drawer
        // shows — the confirmation has to say so instead of pretending the
        // chosen bank already held this preset.
        bool retargetedToOtherBank = false;

        if (collidingFile.existsAsFile())
        {
            // Maintainer machine: the presets dir IS the git checkout that
            // publishes the bank, so a name that already lives in another
            // bank is not a user-duplication hazard — it's the maintainer
            // updating the published preset. Redirect the save onto the
            // existing bank file and fall through to the Replace dialog to
            // confirm the overwrite. Regular installs get the cross-bank
            // block instead, which steers fork-on-edit copies away from
            // colliding labels.
            if (PresetFormat::userPresetsDirIsGitCheckout())
            {
                // If the chosen bank holds no file of this name, redirect the
                // save onto the colliding bank file so the maintainer updates
                // the published preset in place. If the chosen bank ALREADY
                // holds this name it's an ordinary same-bank replace — leave
                // target/label untouched and fall through to the Replace
                // dialog below, which overwrites the chosen file directly.
                if (! target.existsAsFile())
                {
                    target                = collidingFile;
                    replaceBankLabel      = collidingBank;
                    retargetedToOtherBank = true;
                }
            }
            else
            {
                juce::AlertWindow::showAsync(
                    juce::MessageBoxOptions()
                        .withIconType(juce::MessageBoxIconType::WarningIcon)
                        .withTitle("Name Already In Use")
                        .withMessage("A preset named \"" + presetName
                                     + "\" already exists in bank \"" + collidingBank
                                     + "\".\n\nPick a different name, or delete \""
                                     + presetName + "\" from \"" + collidingBank
                                     + "\" first. Two banks cannot share the same preset name.")
                        .withButton("OK")
                        .withParentComponent(this),
                    nullptr);
                presetManager.setStatusText("\"" + presetName + "\" already exists in \""
                                                + collidingBank
                                                + juce::String::fromUTF8("\" \xe2\x80\x94 pick another name"),
                                            true);
                return;
            }
        }

        juce::Component::SafePointer<MainPanel> safeThis(this);
        auto performSave = [safeThis, target, tags, includeInferenceCache]
        {
            if (! safeThis) return;
            auto* self = safeThis.getComponent();
            self->processorRef.setLastTags(tags);
            if (self->savePresetToFile(target, includeInferenceCache))
            {
                self->presetManager.leaveSaveMode();
                self->hidePresetManager();
            }
            else
            {
                self->presetManager.setStatusText("Preset save failed", true);
            }
        };

        if (! target.existsAsFile())
        {
            performSave();
            return;
        }

        // Two different confirmations. The plain one is an ordinary overwrite of
        // the file the drawer points at. The redirect one has to name the file
        // it is REALLY about to overwrite and say the chosen bank is not it —
        // borrowing the plain wording read as a lie ("Replace X in bank Y?" for
        // a preset that does not exist in the bank being saved to).
        const juce::String replaceMessage =
            retargetedToOtherBank
                ? ("\"" + presetName + "\" already exists in bank \"" + replaceBankLabel
                   + "\".\n\nSaving replaces THAT file, in \"" + replaceBankLabel
                   + juce::String::fromUTF8("\" \xe2\x80\x94 not in the selected bank \"")
                   + chosenBankLabel
                   + "\". Two banks cannot share a preset name.")
                : ("Replace \"" + target.getFileNameWithoutExtension()
                   + "\" in bank \"" + replaceBankLabel + "\"?");

        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Replace Preset")
                .withMessage(replaceMessage)
                .withButton("Replace")
                .withButton("Cancel")
                .withParentComponent(this),
            [performSave](int result)
            {
                if (result == 1)
                    performSave();
            });
    };
    presetManager.onTagsChanged = [this](const juce::File& file,
                                         const juce::StringArray& newTags)
    {
        // Surgical JSON-only patch — audio PCM and other fields are
        // preserved byte-for-byte. Works for ANY preset in the library, not
        // just the loaded one. If this IS the loaded preset, also keep the
        // processor's lastTags in sync so a subsequent Save Preset doesn't
        // resurrect the old tag list.
        //
        // EXCEPTION — github-bank files: the UCDCAE bank is owned by the
        // upstream manifest, so editing it in place would just be undone on
        // the next "Update Library" sync. Fork to <userRoot>/<name>
        // (mine).t5p instead, apply the new tags there, and select the
        // fork in the library list so the user can keep editing it. The
        // upstream file is left untouched.
        //
        // On the maintainer machine (presets dir is the git checkout that
        // publishes the bank) the fork would be a stale duplicate of a
        // pending update — there, bank files take the in-place patch like
        // any other preset and git carries the change upstream.
        if (isGithubBankFile(file) && ! PresetFormat::userPresetsDirIsGitCheckout())
        {
            const auto fork = forkPresetForUserEdit(file, newTags);
            if (! fork.existsAsFile())
            {
                presetManager.setStatusText("Tag save failed", true);
                return;
            }
            const bool wasLoaded = (file == currentPresetFile);
            if (wasLoaded)
            {
                currentPresetFile = fork;
                processorRef.setLastTags(newTags);
                processorRef.setLastPresetName(fork.getFileNameWithoutExtension());
            }
            presetManager.refreshLibrary();
            // Keep the "currently loaded" marker on whatever is actually
            // loaded — but ALWAYS jump the list selection to the fork so
            // the user can see where their edit went. Without this jump,
            // editing tags on an unrelated UCDCAE preset would leave the
            // list selection on the loaded preset (which has nothing to
            // do with the edit), and the freshly-written fork would sit
            // unselected somewhere in the user-root.
            presetManager.setCurrentPreset(currentPresetFile, getCurrentPresetDisplayName());
            presetManager.selectFileInList(fork);
            presetManager.setStatusText(wasLoaded
                                           ? "Forked to " + fork.getFileNameWithoutExtension()
                                           : "Tags saved to " + fork.getFileNameWithoutExtension());
            return;
        }

        if (! patchPresetTagsField(file, newTags))
        {
            presetManager.setStatusText("Tag save failed", true);
            return;
        }
        if (file == currentPresetFile)
            processorRef.setLastTags(newTags);
        presetManager.updateTagsForFile(file, newTags);
        presetManager.setStatusText("Tags saved");
    };
    presetManager.onRenameRequested = [this](const juce::File& file)
    {
        // Modal AlertWindow with a single text editor pre-filled with the
        // current name. Save = rename file on disk + patch the JSON `name`
        // field so the in-file metadata stays consistent with the filename.
        auto* alert = new juce::AlertWindow("Rename Preset",
                                            "New name for \"" + file.getFileNameWithoutExtension() + "\":",
                                            juce::MessageBoxIconType::QuestionIcon, this);
        alert->addTextEditor("name", file.getFileNameWithoutExtension());
        alert->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        alert->enterModalState(true,
            juce::ModalCallbackFunction::create([this, alert, file](int result)
            {
                std::unique_ptr<juce::AlertWindow> deleter(alert);
                if (result != 1) return;

                const auto requested = juce::File::createLegalFileName(
                    alert->getTextEditorContents("name").trim());
                if (requested.isEmpty() || requested == file.getFileNameWithoutExtension())
                    return;

                // String-concat (not withFileExtension) — see save callback above.
                auto target = file.getParentDirectory().getChildFile(requested + ".t5p");
                if (target.existsAsFile())
                {
                    presetManager.setStatusText("Rename failed: name already exists", true);
                    return;
                }
                if (! file.moveFileTo(target))
                {
                    presetManager.setStatusText("Rename failed: could not move file", true);
                    return;
                }
                // Patch the JSON `name` field inside the file so the embedded
                // metadata stays consistent with the new filename.
                patchPresetNameField(target, requested);

                if (currentPresetFile == file)
                    currentPresetFile = target;

                presetManager.refreshLibrary();
                presetManager.setCurrentPreset(currentPresetFile, getCurrentPresetDisplayName());
                presetManager.setStatusText("Renamed to " + requested);
            }), false);
    };
    presetManager.onDeleteRequested = [this](const juce::File& file)
    {
        // Async confirmation — the legacy showOkCancelBox returns false on
        // Linux without ever showing the dialog, which made Delete appear
        // permanently broken. The MessageBoxOptions/showAsync path works on
        // every platform.
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::WarningIcon)
                .withTitle("Delete Preset")
                .withMessage("Delete \"" + file.getFileNameWithoutExtension()
                             + "\" from the user library?")
                .withButton("Delete")
                .withButton("Cancel")
                .withParentComponent(this),
            [this, file](int result)
            {
                // showAsync convention: result == 1 → first button (Delete).
                if (result != 1) return;

                if (file.deleteFile())
                {
                    if (currentPresetFile == file)
                        currentPresetFile = juce::File();
                    presetManager.refreshLibrary();
                    presetManager.setCurrentPreset(currentPresetFile,
                                                   getCurrentPresetDisplayName());
                    presetManager.setStatusText("Deleted "
                                                + file.getFileNameWithoutExtension());
                }
                else
                {
                    presetManager.setStatusText("Preset delete failed (write-protected?)",
                                                true);
                }
            });
    };
    presetManager.onRevealRequested = [](const juce::File& file)
    {
        // JUCE's revealToUser() opens the platform file manager (Finder /
        // Explorer / Nautilus) with the file selected.
        if (file.existsAsFile()) file.revealToUser();
    };
    presetManager.onDuplicateRequested = [this](const juce::File& file)
    {
        // Byte-for-byte copy in the same bank, with a "<name> copy"
        // suffix (or " copy 2", " copy 3" if that is taken). The JSON
        // `name` field inside the new file is patched so the embedded
        // metadata stays consistent with the duplicate's filename.
        const auto stem = file.getFileNameWithoutExtension();
        const auto parent = file.getParentDirectory();
        // Build the target name by string concatenation, NOT withFileExtension —
        // withFileExtension() strips at the last `.`, which destroys stems
        // containing a dot ("Bass v1.2" → "Bass v1.t5p") and can land the
        // duplicate on top of an unrelated existing file.
        auto target = parent.getChildFile(stem + " copy.t5p");
        for (int n = 2; target.existsAsFile() && n < 1000; ++n)
            target = parent.getChildFile(stem + " copy " + juce::String(n) + ".t5p");

        // Final belt-and-braces guard: never overwrite, even if the loop
        // bailed out at n=1000 against a wedged collision.
        if (target.existsAsFile())
        {
            presetManager.setStatusText("Duplicate failed: could not allocate a free name", true);
            return;
        }

        if (! file.copyFileTo(target))
        {
            presetManager.setStatusText("Duplicate failed: could not copy file", true);
            return;
        }
        patchPresetNameField(target, target.getFileNameWithoutExtension());
        presetManager.refreshLibrary();
        presetManager.setStatusText("Duplicated to " + target.getFileNameWithoutExtension());
    };
    addChildComponent(presetManager);

    // Manual overlay — hosts the native WebBrowserComponent that renders
    // the shipped HTML guide. Clicking outside the panel or the close
    // button hides the overlay without destroying the web view, so the
    // page stays loaded for subsequent opens.
    manualScrim.onClick = [this] { hideManual(); };
    manualScrim.setVisible(false);
    addChildComponent(manualScrim);

    manualPanel.setVisible(false);
    addChildComponent(manualPanel);
    manualPanel.addAndMakeVisible(manualWeb);
    manualWeb.setVisible(false);

    manualCloseBtn.setColour(juce::TextButton::buttonColourId, kSurface);
    manualCloseBtn.setColour(juce::TextButton::textColourOffId, kAccent);
    manualCloseBtn.onClick = [this] { hideManual(); };
    manualPanel.addAndMakeVisible(manualCloseBtn);

    // Master volume — vertical slider
    masterVolKnob.setSliderStyle(juce::Slider::LinearVertical);
    masterVolKnob.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 50, 14);
    masterVolKnob.setColour(juce::Slider::trackColourId, kAccent);
    masterVolKnob.setColour(juce::Slider::backgroundColourId, kSurface);
    masterVolKnob.setColour(juce::Slider::textBoxTextColourId, kDim);
    masterVolKnob.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    masterVolKnob.setTextValueSuffix(" dB");
    addAndMakeVisible(masterVolKnob);

    masterVolLabel.setText("Vol", juce::dontSendNotification);
    labelAsCaption(masterVolLabel, kDim);
    masterVolLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(masterVolLabel);

    masterVolA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getValueTreeState(), PID::masterVol, masterVolKnob);

    // Main Generate button at bottom of left column. Custom paintButton
    // handles fill/border/text colours directly — no TextButton::ColourIds
    // needed.
    mainGenerateBtn.onClick = [this] { triggerMainGeneration(); };
    addAndMakeVisible(mainGenerateBtn);

    // LCO: the reused GENERATE button drives the bake/re-prompt; disable it while an
    // LCO authoring pass runs (the neural glow/cache path never fires in LCO).
    promptPanel.onLcoBusyChanged = [this](bool busy) {
        // Tracked in BOTH modes (the bake keeps running when the user leaves the
        // LCO): a SNAP recall must not hand an orchestra to the engine that the
        // in-flight bake would then silently replace seconds later.
        lcoBakeBusy_ = busy;
        if (!oscEasyMode)
            mainGenerateBtn.setEnabled(!busy);
    };

    // A fresh bake makes the current audio a new, unsaved sound — detach it from
    // whatever preset was last loaded/saved so Save's "Replace" warning (and its
    // pre-filled name) stops pointing at an unrelated old file (prompts/tags/seed
    // stay untouched; only the saved-file identity is stale here, unlike Init).
    promptPanel.onNewGenerationStarted = [this] {
        currentPresetFile = juce::File();
        processorRef.setLastPresetName({});
        statusBar.setPresetName({});
        presetManager.setCurrentPreset(currentPresetFile, {});
    };

    // Left-header: same grammar as the RE-PROMPT/VARIATION headers (accent band,
    // dark bold left-aligned title), rotated to the left edge of this single-row
    // switchbox module. No frame on the label itself -- the frame is the switchbox's.
    paintSectionHeader(snapLabel, "SNAP", kOscCol);
    snapLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(snapLabel);

    {
        static constexpr const char* labels[kNumSnapshotButtons] = { "OFF", "1", "2", "3", "4" };
        for (int i = 0; i < kNumSnapshotButtons; ++i)
        {
            auto& b = snapshotButtons[i];
            b.setButtonText(labels[i]);
            b.setSnapshotIndex(i);
            styleSwitchButton(b, kOscCol);
            b.setTooltip(i == 0 ? "Disable snapshot recall"
                                : "Snapshot slot " + juce::String(i));
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumSnapshotButtons - 1) edges |= juce::Button::ConnectedOnRight;
            b.setConnectedEdges(edges);
            b.onPressStarted = [this](int slot) { captureSnapshotPress(slot); };
            b.onShortActivate = [this](int slot) { activateSnapshot(slot); };
            b.onLongStore = [this](int slot) { storeSnapshotFromPress(slot); };
            addAndMakeVisible(b);
        }
    }

    paintSectionHeader(cacheLabel, "CACHE", kOscCol);   // left-header (see SNAP)
    cacheLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(cacheLabel);

    {
        static constexpr const char* labels[kNumInfCacheButtons] = {
            "OFF", "2", "4", "8", "16"
        };
        static constexpr int values[kNumInfCacheButtons] = {
            0, 2, 4, 8, 16
        };
        for (int i = 0; i < kNumInfCacheButtons; ++i)
        {
            auto& b = infCacheButtons[i];
            b.setButtonText(labels[i]);
            styleSwitchButton(b, kOscCol);
            b.setClickingTogglesState(true);
            b.setRadioGroupId(3017);
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumInfCacheButtons - 1) edges |= juce::Button::ConnectedOnRight;
            b.setConnectedEdges(edges);
            b.onClick = [this, value = values[i]]
            {
                processorRef.setInferenceCacheCapacity(value);
                syncInferenceCacheUi();
            };
            addAndMakeVisible(b);
        }

        // Offline take. Stands apart from the capacity cells (own switch, no
        // radio group, no connected edges): it does not say how DEEP the cache
        // records, it says that the parameters wait at each cache point until the
        // render lands instead of sweeping on while the machine catches up.
        // "A/S", not "ASYNC": at the minimum window width this row gives a cell
        // ~17 pt, which already ellipsises OFF and 16. The take switch gets the
        // width the two dropped depths freed (see the layout) and a label short
        // enough to survive there; the tooltip carries the meaning.
        cacheAsyncBtn.setButtonText("A/S");
        styleSwitchButton(cacheAsyncBtn, kOscCol);
        cacheAsyncBtn.setClickingTogglesState(true);
        cacheAsyncBtn.setTooltip(
            "Offline take: hold the generation parameters at each cache point until "
            "the render lands, then step on by one Regenerate interval. The recorded "
            "run is the same on a fast and a slow machine, and the Resynth/Re-Prompt "
            "chain keeps every step. Applies while a cache is recording.");
        addAndMakeVisible(cacheAsyncBtn);
        cacheAsyncAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processorRef.getValueTreeState(), PID::cacheAsync, cacheAsyncBtn);
    }
    syncSnapshotUi();
    syncInferenceCacheUi();

    // Resynth (init_audio / i2i) — one Off->Full slider under the snap/cache row,
    // enabled only for SA3 (gated in onModelChanged below). Left = off (text-only);
    // dragging right feeds the last raw generation back as the denoise seed so each
    // render evolves from the previous one, up to Full at the right. A word readout
    // (Off..Full) replaces the 0-1 number, so "full" is a visible end position you
    // navigate to rather than a value to guess.
    // Left-title band ("RESYNTH" on accent, dark ink) — the snap/cache treatment.
    // RESYNTH (init_audio / i2i): house-standard inline-bar SliderRow (the same
    // format as the sequencer's Shuffle row) — accent band label + fill bar in
    // kOscCol, with an Off..Full word read-out as the inline value. SA3-gated.
    // The int/ext source toggle sits to its right (where Shuffle shows value+dot).
    resynthRow = std::make_unique<SliderRow>(
        "RESYNTH",
        [](double v) {
            auto onAnchor = [v](double a) { return std::abs(v - a) < 0.02; };
            if (onAnchor(0.0))  return juce::String("Off");
            if (onAnchor(0.05)) return juce::String("Min");
            if (onAnchor(0.25)) return juce::String("Subtle");
            if (onAnchor(0.50)) return juce::String("Medium");
            if (onAnchor(0.75)) return juce::String("Strong");
            if (onAnchor(1.0))  return juce::String("Full");
            return juce::String(juce::roundToInt(v * 100.0)) + "%";
        },
        kOscCol);
    resynthRow->setInlineLabel(true);
    resynthRow->getSlider().setTooltip(juce::String::fromUTF8(
        "Resynth (SA3): Off = normal generation. Drag right to feed the source back "
        "as the seed \xe2\x80\x94 further right = the next render follows it more."));
    addAndMakeVisible(*resynthRow);

    resynthA = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processor.getValueTreeState(), PID::resynthAmount, resynthRow->getSlider());
    // Keep the inline read-out in sync (the attachment overwrites onValueChange).
    resynthRow->getSlider().onValueChange = [this] { resynthRow->updateValue(); };
    resynthRow->updateValue();

    // int/ext source toggle — two TextButtons + hidden ComboBox with APVTS attachment.
    // Replaces the slider's value readout; mirrors the repromptCoupling mechanism in
    // PromptPanel (ComboBox attachment syncs the buttons).
    {
        static constexpr const char* labels[2] = { "int", "ext" };
        resynthSrcHidden.addItemList({ "int", "ext" }, 1);
        resynthSrcHidden.onChange = [this] {
            const int id = resynthSrcHidden.getSelectedId();
            for (int i = 0; i < 2; ++i)
                resynthSrcBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);

            // "ext" is the moment the standalone actually needs to listen. The
            // input device is not opened at launch (src/StandaloneAudioInput.h
            // has the why), so it is opened here — reported rather than failing
            // quietly. Idempotent: a second ext with the input already open
            // does nothing. In a plugin it is a no-op; the host owns the input.
            //
            // DEFERRED, never inline. This lambda also runs from the parameter
            // attachment's own initial update, which happens INSIDE the editor's
            // constructor — in the standalone that is inside the app's
            // initialise(), before the window is ever shown. Opening the device
            // there would rebuild the very no-window-at-launch state this whole
            // change exists to prevent, for anyone whose last session ended on
            // ext. callAsync puts it after initialise() has returned.
            if (id == 2)
            {
                juce::Component::SafePointer<MainPanel> safeThis(this);
                juce::MessageManager::callAsync([safeThis]
                {
                    if (safeThis == nullptr)
                        return;
                    const auto error = t5ynth::standalone::openParkedInputDevice();
                    if (error.isNotEmpty())
                        safeThis->statusBar.setStatusText("Resynth ext: " + error);
                });
            }
        };
        addChildComponent(resynthSrcHidden);   // hidden, not visible
        for (int i = 0; i < 2; ++i)
        {
            auto& b = resynthSrcBtns[i];
            b.setButtonText(labels[i]);
            styleSwitchButton(b, kOscCol);
            b.setClickingTogglesState(true);
            b.setRadioGroupId(3019);            // fresh id (3017 cache, 3001 filter, 2038 seed)
            b.setConnectedEdges(i == 0 ? juce::Button::ConnectedOnRight
                                       : juce::Button::ConnectedOnLeft);
            b.setTooltip(i == 0 ? "Int: resynthesise the last generation (self-feedback)."
                                : "Ext: resynthesise live audio input (needs an input source in Audio settings).");
            b.onClick = [this, i] { resynthSrcHidden.setSelectedId(i + 1); };
            addAndMakeVisible(b);
        }
        resynthSrcA = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processor.getValueTreeState(), PID::resynthSource, resynthSrcHidden);
    }

    // (Re-Prompt stance/coupling controls moved into PromptPanel — they now sit
    // directly under the prompts, co-located with the loop logic. See PromptPanel's
    // constructor for their setup.)

    // Wire axis values callback for drift auto-regen (offsets applied per slot)
    promptPanel.getAxisValuesCallback = [this](float o1, float o2, float o3) {
        return axesPanel.getAxisValuesWithOffsets(o1, o2, o3);
    };

    // Semantic Axes AND the Dimension Explorer now run for SA3 too — the backend
    // confines the embedding edit to the real (non-padded) tokens, so both panels
    // stay live on every engine. Only Resynth remains model-gated (below).
    promptPanel.onModelChanged = [this]() {
        // Resynth is the INVERSE gate: enabled only for SA3, the inpaint engine
        // whose init_audio path it drives (SAO is backend-capable too, but the
        // element is scoped to SA3; AudioLDM2's diffusers path can't take it).
        // Separate guard — its enabled state is the opposite of the cards above.
        const bool resynthOk = promptPanel.selectedModelIsSA3();
        axesPanel.setModelIsSA3(promptPanel.selectedModelIsSA3());
        if (resynthRow->isEnabled() != resynthOk)
        {
            resynthRow->setEnabled(resynthOk);
            resynthRow->setAlpha(resynthOk ? 1.0f : 0.4f);   // dims the inline band label too
        }
        // The int/ext source toggle follows resynthOk; "ext" additionally requires
        // an available input bus. This sets the state on model change; ext-
        // availability is then refreshed live in timerCallback (the input bus can
        // appear/vanish without a model change, e.g. user selects an input device).
        const bool extOk = resynthOk && processorRef.hasExternalInputAvailable();
        resynthSrcBtns[0].setEnabled(resynthOk);
        resynthSrcBtns[0].setAlpha(resynthOk ? 1.0f : 0.4f);
        resynthSrcBtns[1].setEnabled(extOk);
        resynthSrcBtns[1].setAlpha(extOk ? 1.0f : 0.4f);
        // (Re-Prompt is engine-agnostic — not SA3-gated — and its controls now live
        // in PromptPanel; nothing to flip here.)
    };
    promptPanel.onModelChanged();  // set initial state (no-op until a model is selected)

    // Persist the SA3 tier when the user flips the switch (machine-local, not preset).
    promptPanel.onSa3TierChanged = [this](juce::String tier) { saveSa3TierSetting(tier); };



    // Status callback — drive Generate animation and status bar text.
    startTimerHz(20);  // lightweight glow animation + UI polling

    promptPanel.onStatusChanged = [this](const juce::String& text, bool isGenerating) {
        const bool cacheHit = (text == "From cache");

        if (cacheHit)
        {
            // Cache-hit: keep the pulse running and keep the action labelled
            // as cache playback while the cache remains full. Button stays
            // enabled so the user can fire the next cache entry immediately.
            cacheHitActive = true;
            cacheHitUntilSec = juce::Time::getMillisecondCounterHiRes() * 0.001 + 1.5;
            glowGenerating = true;
            updateGenerateButtonsForCacheState(true);
            mainGenerateBtn.setAnimationState(glowPhase, glowGenerating);
        }
        else
        {
            // Real generation status — drop any pending cache-hit display.
            if (isGenerating && activeSnapshotIndex != 0)
            {
                activeSnapshotIndex = 0;
                syncSnapshotUi();
            }
            cacheHitActive = false;
            glowGenerating = isGenerating;
            mainGenerateBtn.setAnimationState(glowPhase, glowGenerating);

            if (isGenerating && !processorRef.isInferenceCacheFull())
            {
                mainGenerateBtn.setButtonText("GENERATE");
                mainGenerateBtn.setEnabled(false);
                dimApplyBtn.setButtonText("generating...");
                dimApplyBtn.setEnabled(false);
            }
            else
            {
                updateGenerateButtonsForCacheState(false);
            }
        }

        statusBar.setStatusText(text);
        syncInferenceCacheUi();
    };

    // Scrim (click outside DimExplorer overlay to close)
    dimScrim.onClick = [this] { hideDimExplorer(); };
    dimScrim.setVisible(false);
    addChildComponent(dimScrim);

    // DimExplorer — always visible (mini-view in left column, overlay on click)
    addAndMakeVisible(dimensionExplorer);
    dimensionExplorer.onClicked = [this] {
        if (!dimExplorerVisible) showDimExplorer();
    };

    // Wire PromptPanel → DimensionExplorer (embedding stats after generation)
    promptPanel.onEmbeddingsReady = [this](const std::vector<float>& a,
                                           const std::vector<float>& b,
                                           const std::vector<float>& baseline) {
        dimensionExplorer.setEmbeddings(a, b, baseline);
    };

    // "Anwenden + generieren" — green, triggers generation with offsets
    dimApplyBtn.setColour(juce::TextButton::buttonColourId, kOscCol);
    dimApplyBtn.setColour(juce::TextButton::textColourOffId, kBg);
    dimApplyBtn.onClick = [this] {
        activeSnapshotIndex = 0;
        syncSnapshotUi();
        promptPanel.setSemanticAxes(axesPanel.getAxisValues());
        auto offsets = dimensionExplorer.getDimensionOffsets();
        promptPanel.triggerGenerationWithOffsets(std::move(offsets));
    };
    dimApplyBtn.setVisible(false);
    addChildComponent(dimApplyBtn);

    dimUndoBtn.setColour(juce::TextButton::buttonColourId, kSurface);
    dimUndoBtn.setColour(juce::TextButton::textColourOffId, kDim);
    dimUndoBtn.onClick = [this] { dimensionExplorer.undo(); };
    dimUndoBtn.setVisible(false);
    addChildComponent(dimUndoBtn);

    dimRedoBtn.setColour(juce::TextButton::buttonColourId, kSurface);
    dimRedoBtn.setColour(juce::TextButton::textColourOffId, kDim);
    dimRedoBtn.onClick = [this] { dimensionExplorer.redo(); };
    dimRedoBtn.setVisible(false);
    addChildComponent(dimRedoBtn);

    // "Alle zurücksetzen" — orange
    dimResetBtn.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff4e2700));
    dimResetBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xffff9800));
    dimResetBtn.onClick = [this] { dimensionExplorer.resetOffsets(); };
    dimResetBtn.setVisible(false);
    addChildComponent(dimResetBtn);

    setOscEasyMode(loadOscEasyModeSetting(), false);
    updateAxesDimSegment();   // seed the default segment (Semantic Axes) + visibility

    // Restore the per-machine SA3 tier before the backend handshake. PromptPanel has
    // no models yet, so this just records the choice; the first populateModelSelector
    // (driven by onModelReady) honors it when it slots the SA3 checkpoints.
    promptPanel.setSa3Tier(loadSa3TierSetting(), false);

    // First-launch empty-library hint: surface the situation immediately
    // rather than letting the user sit in front of an empty preset
    // browser wondering what happened. Presets are no longer bundled into
    // the binary — they live in <userPresetsDir>/UCDCAE AI Lab/ and are
    // fetched from the public GitHub mirror on demand via the Preset
    // Manager's "Update Library" button.
    if (PresetFormat::getAllPresetFiles().isEmpty())
    {
        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::InfoIcon)
                .withTitle("No Presets Found")
                .withMessage("Your preset library is empty.\n\n"
                             "Open the Preset Manager and click "
                             "\"Update Library\" to download the "
                             "UCDCAE AI Lab bank from GitHub.")
                .withButton("OK")
                .withParentComponent(this),
            nullptr);
    }

    // Load default preset (if no audio loaded yet)
    loadDefaultPreset();

    // Normalize any previously discovered external model directories into
    // T5ynth's canonical model slots before the backend scans them.
    settingsPage.importDiscoveredModels();

    // Load native inference only after Settings has confirmed at least one
    // model slot contains model files the backend can scan.
    if (settingsPage.hasAnyInstalledModel())
    {
        tryLoadInferenceModels();
    }
    else
    {
        statusBar.setConnected(false);
        statusBar.setStatusText("Model setup required");
        showSettings();
    }
}

void MainPanel::showDimExplorer()
{
    dimExplorerVisible = true;
    dimensionExplorer.setOverlayMode(true);
    dimScrim.setVisible(true);
    dimScrim.toFront(false);
    dimApplyBtn.setVisible(true);
    dimUndoBtn.setVisible(true);
    dimRedoBtn.setVisible(true);
    dimResetBtn.setVisible(true);
    dimensionExplorer.toFront(false);
    dimApplyBtn.toFront(false);
    dimUndoBtn.toFront(false);
    dimRedoBtn.toFront(false);
    dimResetBtn.toFront(false);
    resized();
    repaint();
}

void MainPanel::hideDimExplorer()
{
    dimExplorerVisible = false;
    dimensionExplorer.setOverlayMode(false);
    dimScrim.setVisible(false);
    dimApplyBtn.setVisible(false);
    dimUndoBtn.setVisible(false);
    dimRedoBtn.setVisible(false);
    dimResetBtn.setVisible(false);
    resized();  // repositions DimExplorer back to mini-view
    repaint();
}

void MainPanel::updateAxesDimSegment()
{
    const bool dim = showDimSegment_;
    axesDimSegBtns[0].setToggleState(!dim, juce::dontSendNotification);
    axesDimSegBtns[1].setToggleState(dim,  juce::dontSendNotification);

    // Leaving the Dim segment with the overlay open → close the overlay first.
    if (!dim && dimExplorerVisible)
        hideDimExplorer();

    // Easy-only either way: in Advanced the DCO panel owns the whole column
    // (setOscEasyMode hides the segment buttons and everything below them).
    axesPanel.setVisible(!dim && oscEasyMode);
    dimensionExplorer.setVisible(dim && oscEasyMode);
    resized();
    repaint();
}

bool MainPanel::loadOscEasyModeSetting() const
{
    auto file = getUiSettingsFile();
    if (!file.existsAsFile())
        return true;

    auto parsed = juce::JSON::parse(file.loadFileAsString());
    if (auto* obj = parsed.getDynamicObject())
    {
        juce::ignoreUnused(obj);
        return static_cast<bool>(parsed.getProperty(kOscEasyModeKey, true));
    }

    return true;
}

void MainPanel::saveOscEasyModeSetting() const
{
    auto file = getUiSettingsFile();
    file.getParentDirectory().createDirectory();

    juce::var parsed;
    juce::DynamicObject::Ptr root;
    if (file.existsAsFile())
    {
        parsed = juce::JSON::parse(file.loadFileAsString());
        if (auto* obj = parsed.getDynamicObject())
            root = obj;
    }

    if (root == nullptr)
        root = juce::DynamicObject::Ptr(new juce::DynamicObject());

    root->setProperty(kOscEasyModeKey, oscEasyMode);
    file.replaceWithText(juce::JSON::toString(juce::var(root.get()), true));
}

// Per-machine SA3 tier (small | medium). Stored alongside oscEasyMode in
// ui_settings.json — a hardware-capability choice, never part of a preset. Default
// "small" so a medium install never silently supersedes the lighter checkpoint.
juce::String MainPanel::loadSa3TierSetting() const
{
    auto file = getUiSettingsFile();
    if (!file.existsAsFile())
        return "small";

    auto parsed = juce::JSON::parse(file.loadFileAsString());
    if (parsed.getDynamicObject() != nullptr)
    {
        const juce::String t = parsed.getProperty(kSa3TierKey, "small").toString();
        return t.equalsIgnoreCase("medium") ? "medium" : "small";
    }

    return "small";
}

void MainPanel::saveSa3TierSetting(const juce::String& tier) const
{
    auto file = getUiSettingsFile();
    file.getParentDirectory().createDirectory();

    juce::var parsed;
    juce::DynamicObject::Ptr root;
    if (file.existsAsFile())
    {
        parsed = juce::JSON::parse(file.loadFileAsString());
        if (auto* obj = parsed.getDynamicObject())
            root = obj;
    }

    if (root == nullptr)
        root = juce::DynamicObject::Ptr(new juce::DynamicObject());

    root->setProperty(kSa3TierKey, tier.equalsIgnoreCase("medium") ? "medium" : "small");
    file.replaceWithText(juce::JSON::toString(juce::var(root.get()), true));
}

void MainPanel::setOscEasyMode(bool easy, bool persist)
{
    oscEasyMode = easy;

    promptPanel.setEasyMode(oscEasyMode);

    // Advanced IS the DCO panel — a different paradigm, not a neural variant.
    // The entire neural generation column below the prompt block (axes/dim
    // segment + box, GENERATE, snapshots, inference cache, resynth, and the
    // Stability credit) is Easy-only; the DCO prompt canvas absorbs the whole
    // column in Advanced (see resized()). Axes state stays live either way —
    // hasOscHiddenActiveState pulses the toggle when hidden-but-active.
    const bool neural = oscEasyMode;
    if (!neural && dimExplorerVisible)
        hideDimExplorer();

    // Panel title is mode-dependent: neural keeps "T5 OSCILLATOR", LCO spells
    // out the mode's real name instead (no separate subtitle line any more —
    // the LCO panel's own dcoSubtitleLabel was removed, PromptPanel.cpp).
    oscHeader.setText(neural ? juce::String(" T5 OSCILLATOR")
                             : juce::String::fromUTF8(" Language-Resonant Oscillator"),
                      juce::dontSendNotification);

    // Axes|Dim segment + the Stability credit are neural-only (no LCO meaning).
    axesDimSegBtns[0].setVisible(neural);
    axesDimSegBtns[1].setVisible(neural);
    poweredByLabel.setVisible(neural);

    // The GENERATE control block stays in BOTH modes — RUHE on switch, and GENERATE
    // is reused for the LCO bake. Controls with no meaning for a deterministic
    // language bake are shown DISABLED (dimmed) rather than removed: CACHE (no
    // inference cache) and RESYNTH (no init-audio). GENERATE + row labels stay live.
    const float dimA = neural ? 1.0f : 0.4f;
    mainGenerateBtn.setVisible(true);
    snapLabel.setVisible(true);
    cacheLabel.setVisible(true);
    snapLabel.setAlpha(1.0f);       // SNAP is live in both modes
    cacheLabel.setAlpha(dimA);
    // SNAP stays live in BOTH modes: neural recalls audio snapshots, LCO recalls the
    // authored orchestra + its sound (lcoSnapshots). Only CACHE/RESYNTH go
    // disabled+dimmed in LCO.
    for (auto& bSnap : snapshotButtons)
    {
        bSnap.setVisible(true);
        bSnap.setEnabled(true);
        bSnap.setAlpha(1.0f);
    }
    for (auto& bCache : infCacheButtons)
    {
        bCache.setVisible(true);
        bCache.setEnabled(neural);
        bCache.setAlpha(dimA);
    }
    cacheAsyncBtn.setVisible(true);
    cacheAsyncBtn.setEnabled(neural);
    cacheAsyncBtn.setAlpha(dimA);
    for (auto& bSrc : resynthSrcBtns)
        bSrc.setVisible(true);
    if (resynthRow)
        resynthRow->setVisible(true);

    if (neural)
    {
        // Restore the SA3-gated RESYNTH + int/ext enabled/alpha state exactly.
        if (promptPanel.onModelChanged)
            promptPanel.onModelChanged();
    }
    else
    {
        // LCO: force RESYNTH off + dimmed, and clear any leftover neural GENERATE
        // state (cache-hit label, glow, disabled-during-generation) so the reused
        // button reads a clean, live "GENERATE".
        for (auto& bSrc : resynthSrcBtns) { bSrc.setEnabled(false); bSrc.setAlpha(dimA); }
        if (resynthRow)                   { resynthRow->setEnabled(false); resynthRow->setAlpha(dimA); }
        glowGenerating = false;
        mainGenerateBtn.setAnimationState(glowPhase, false);
        mainGenerateBtn.setButtonText("GENERATE");
        mainGenerateBtn.setEnabled(true);
    }

    // Force the inference-cache UI (button pulsing) to re-evaluate for the new mode
    // on the next timer tick — the syncInferenceCacheUi memo would otherwise early-
    // return and leave neural pulsing frozen on the now-dimmed LCO buttons (or leave
    // them un-pulsing after returning to neural mid-fill).
    lastInfCacheUiCapacity = -1;

    // The SNAP store is mode-specific (audio snapshots vs LCO orchestras), so clear
    // the active highlight and refresh the filled dots for the mode we just entered.
    activeSnapshotIndex = 0;
    syncSnapshotUi();

    updateAxesDimSegment();   // re-applies axesPanel/dimensionExplorer (mode-aware)

    oscModeToggle.setButtonText(oscEasyMode ? juce::String::fromUTF8("\xc2\xbb LRO")
                                            : juce::String::fromUTF8("\xc2\xbb T5osc"));
    updateOscModeToggleVisual();

    if (persist)
        saveOscEasyModeSetting();

    resized();
    repaint();
}

void MainPanel::applyOscModeToEngine(bool neural)
{
    // The toggle IS the paradigm switch (BlockParams.h: engineMode's Lco/Csound
    // values ARE the LCO, Sampler/Wavetable/Granular are the neural T5osc), so
    // it owns the engine too. Without this the panel flipped and the previous
    // paradigm kept sounding: switching LCO -> T5osc left the authored orchestra
    // playing, and only a SUCCESSFUL generation happened to restore the engine —
    // never on a failed one, and never at all in a session that started in
    // Csound (an LCO preset / DAW state has no stashed engine to restore).
    //
    // Called ONLY from the user's own toggle click: setOscEasyMode's other
    // callers (startup default, preset load) get their engine mode from state
    // that is already authoritative, and must not have it rewritten from the
    // panel's GUI state.
    if (neural)
    {
        processorRef.restoreNeuralEngineMode();
        return;
    }

    // Back to the LCO: return to the exact paradigm the toggle left — Csound,
    // or the legacy Lco wavetable a restored v5 preset baked. Only when the
    // toggle has never left one does the authored-orchestra test decide.
    // OPEN (BJ's call): with nothing ever authored in the session the engine is
    // deliberately left alone, so the last neural sample keeps sounding under
    // an LCO panel until the first bake. Pairing it would mean switching to an
    // orchestra nobody wrote (an empty orchestra text is the engine's BUILT-IN
    // one, which does sound) — a language oscillator making sound with no
    // language behind it.
    if (!processorRef.restoreLanguageEngineMode() && processorRef.hasCsoundOrchestra())
        processorRef.forceCsoundEngineMode();
}

bool MainPanel::hasOscHiddenActiveState() const
{
    if (oscEasyMode)
        return promptPanel.hasHiddenActiveState() || dimensionExplorer.hasOffsets();

    // In Advanced, axes are hidden — pulse toggle when any axis is selected AND non-zero.
    // dropdownId == 1 means "---" (no axis chosen); value alone is unreliable because
    // the slider retains its last value even after the dropdown reverts to "---".
    for (const auto& s : axesPanel.getSlotStates())
        if (s.dropdownId > 1 && std::abs(s.value) > 1e-5f)
            return true;
    return false;
}

void MainPanel::updateOscModeToggleVisual()
{
    const bool pulse = hasOscHiddenActiveState();
    if (!pulse && !oscModePulseActive_)
        return;

    // Skip the animation when audio is idle — the pulse provides no useful
    // feedback while silent, and every setColour() cascades into a deferred
    // AppKit repaint (CFRunLoopWakeUp → CA::Transaction commit). The pulse
    // transition still applies its terminal colours once when the active
    // state flips, so the toggle reflects its semantic state correctly.
    if (pulse == oscModePulseActive_
        && processorRef.audioIdle.load(std::memory_order_relaxed))
    {
        return;
    }

    auto fill = kSurface.darker(0.45f);
    auto text = kOscCol;

    if (oscEasyMode && pulse)
    {
        const float p = 0.5f + 0.5f * std::sin(oscModePulsePhase);
        fill = fill.interpolatedWith(kOscCol, 0.10f + 0.18f * p);
        text = kTextPrimary;
    }

    // setColour() always triggers an internal repaint of the button. The pulse
    // phase advances ~6° per 20 Hz tick and the resulting 8-bit ARGB is often
    // identical to the previous frame at this quantization; without this guard
    // the toggle button repaints ~20×/s in idle.
    if (lastAppliedOscColoursValid_
        && lastAppliedOscFill_ == fill
        && lastAppliedOscText_ == text)
    {
        oscModePulseActive_ = pulse;
        return;
    }
    lastAppliedOscFill_ = fill;
    lastAppliedOscText_ = text;
    lastAppliedOscColoursValid_ = true;

    oscModeToggle.setColour(juce::TextButton::buttonColourId, fill);
    oscModeToggle.setColour(juce::TextButton::buttonOnColourId, fill);
    oscModeToggle.setColour(juce::TextButton::textColourOffId, text);
    oscModeToggle.setColour(juce::TextButton::textColourOnId, text);
    oscModeToggle.repaint();

    oscModePulseActive_ = pulse;
}

void MainPanel::showPresetManager()
{
    presetManagerVisible = true;
    // Push the canonical tag taxonomy before refreshing the library so the
    // Detail-cloud merge sees both the curated set and the user-seen tags
    // in a single applyTagVocabulary() call inside refreshLibrary(). The
    // "generative" tag only ships when the generative sequencer is
    // currently running — see TagVocabulary.h for the rationale.
    const bool genSeqActive = processorRef.getValueTreeState()
                                  .getRawParameterValue(PID::genSeqRunning)->load() > 0.5f;
    presetManager.setTagVocabularyCanonical(TagVocabulary::getCanonical(genSeqActive));
    presetManager.setRuntimeDevice(promptPanel.getInferenceDevice());
    presetManager.refreshLibrary();
    presetManager.setCurrentPreset(currentPresetFile, getCurrentPresetDisplayName());
    presetScrim.setVisible(true);
    presetManager.setVisible(true);
    presetScrim.toFront(false);
    presetManager.toFront(false);
    resized();
    repaint();
}

void MainPanel::hidePresetManager()
{
    presetManagerVisible = false;
    // Defensive: if the panel was closed while still in Save mode (× icon,
    // scrim click) restore Browse so the next open is in a clean state.
    presetManager.leaveSaveMode();
    presetScrim.setVisible(false);
    presetManager.setVisible(false);
    repaint();
}

void MainPanel::showSequenceLibrary(bool focusSave)
{
    seqLibraryVisible = true;
    seqLibraryScrim.setVisible(true);
    seqLibrary.setVisible(true);
    seqLibraryScrim.toFront(false);
    seqLibrary.toFront(false);
    resized();                            // place the panel at its centered bounds
    seqLibrary.prepareForShow(focusSave); // rescan + focus, now visible + sized
    repaint();
}

void MainPanel::hideSequenceLibrary()
{
    seqLibraryVisible = false;
    seqLibraryScrim.setVisible(false);
    seqLibrary.setVisible(false);
    repaint();
}

void MainPanel::enterLibrarySaveMode(SaveNameMode mode)
{
    auto defaultName = getCurrentPresetDisplayName();
    if (defaultName.isEmpty()) defaultName = "New Preset";
    if (mode == SaveNameMode::appendCopy && getCurrentPresetDisplayName().isNotEmpty())
        defaultName = defaultName + " copy";

    juce::StringArray existingBanks;
    std::set<juce::String> existingPathKeys;
    auto userDir = PresetFormat::getUserPresetsDirectory();
    if (userDir.isDirectory())
    {
        for (auto& f : userDir.findChildFiles(juce::File::findFiles, true, "*.t5p"))
        {
            const auto rel = f.getRelativePathFrom(userDir).replace("\\", "/");
            existingPathKeys.insert(rel.toLowerCase());
        }
        for (auto& d : userDir.findChildFiles(juce::File::findDirectories, false, "*"))
            existingBanks.add(d.getFileName());
    }
    existingBanks.removeEmptyStrings();
    existingBanks.removeDuplicates(true);
    existingBanks.sortNatural();

    juce::String currentBank;
    if (currentPresetFile.existsAsFile())
    {
        const auto parent = currentPresetFile.getParentDirectory();
        if (parent != userDir && parent.isAChildOf(userDir))
            currentBank = parent.getFileName();
    }

    // Phase-4 rule: saving from a UCDCAE-bank source forks to My Presets
    // with a mandatory "(mine)" rename. The user can still override both
    // fields in the drawer — this only picks the default. Without the
    // rename, a Save would either overwrite the github file (which the
    // next library sync would undo) or silently land beside it with the
    // same name, creating exactly the duplication the user wants to
    // avoid.
    //
    // EXCEPTION — maintainer machine (presets dir IS the git checkout of
    // the public bank): there the local edit is the next upstream, so the
    // prefill keeps the original name + bank and Save overwrites the bank
    // file directly. "(mine)" forks on that machine only created stale
    // duplicates of pending updates.
    if (currentBank.equalsIgnoreCase(PresetUpdater::getBankName())
        && ! PresetFormat::userPresetsDirIsGitCheckout())
    {
        currentBank.clear();
        const auto lc = defaultName.toLowerCase();
        if (! lc.contains("(mine")
            && ! lc.endsWith(" copy")
            && ! lc.endsWith("copy"))
            defaultName = defaultName + " (mine)";
    }

    PresetManagerPanel::SavePrefill prefill;
    prefill.defaultName      = defaultName;
    // Tags are deliberately NOT prefilled from processor.getLastTags():
    // that copied the previously LOADED preset's tags into every save
    // regardless of target, so unrelated presets inherited stale tags
    // ('perc' on filter sweeps, …). The SaveDrawer's chips now follow the
    // save target instead (PresetManagerPanel::refreshSaveDrawerAutoTags):
    // overwriting an existing preset mirrors that file's on-disk tags, a
    // fresh name starts untagged. No audio/prompt-content heuristic either
    // — the pre-Phase-1 auto-suggester injected "hot" into almost
    // everything and silently overwrote hand-curated tags.
    prefill.currentBank      = currentBank;
    prefill.existingBanks    = existingBanks;
    prefill.existingPathKeys = std::move(existingPathKeys);
    prefill.promptA          = promptPanel.getPromptA();
    prefill.promptB          = promptPanel.getPromptB();
    prefill.canIncludeInferenceCache = processorRef.getInferenceCacheCapacity() > 0;

    showPresetManager();
    presetManager.enterSaveMode(std::move(prefill));
}

namespace
{
/** Write a Serum-format wavetable .wav: standard RIFF/WAVE container with
 *  a `clm ` metadata chunk indicating frame size. Recognised by Serum,
 *  Vital, Bitwig Polysynth and most modern wavetable importers — they
 *  use the `<!>FRAME_SIZE …` text in the chunk to slice the data into
 *  single-cycle frames.
 *
 *  Layout: 32-bit IEEE-float PCM, mono, frames concatenated frame-major. */
bool writeWavetableWav(const juce::File& file,
                       const std::vector<float>& samples,
                       int frameSize,
                       int numFrames,
                       double sampleRate)
{
    if (frameSize <= 0 || numFrames <= 0) return false;
    if ((int) samples.size() != frameSize * numFrames) return false;
    if (sampleRate <= 0.0) sampleRate = 44100.0;

    auto out = file.createOutputStream();
    if (out == nullptr) return false;
    out->setPosition(0);
    out->truncate();

    const uint16_t numChannels   = 1;
    const uint16_t bitsPerSample = 32;
    const uint16_t blockAlign    = static_cast<uint16_t>(numChannels * (bitsPerSample / 8));
    const auto sr32              = static_cast<uint32_t>(sampleRate);
    const uint32_t avgBytesPerSec = sr32 * blockAlign;
    const uint32_t pcmBytes       = static_cast<uint32_t>(samples.size() * sizeof(float));

    // <!> + frame size + version-id + name. Serum reads the first integer
    // after "<!>" as the per-frame sample count.
    const juce::String clmText = "<!>" + juce::String(frameSize) + " 0 t5ynth";
    const auto clmUtf8         = clmText.toRawUTF8();
    const auto clmLen          = static_cast<uint32_t>(std::strlen(clmUtf8));
    const auto clmPad          = static_cast<uint32_t>(clmLen % 2);  // RIFF chunks 2-byte aligned

    const uint32_t fmtChunkBytes  = 16;
    const uint32_t totalRiffSize  = 4                                // "WAVE"
                                  + (8 + fmtChunkBytes)              // fmt
                                  + (8 + clmLen + clmPad)            // clm
                                  + (8 + pcmBytes);                  // data

    // JUCE's writeInt / writeShort are little-endian by default — the
    // explicit BigEndian variants exist but the LE ones are the
    // unsuffixed defaults. RIFF / WAVE is little-endian.
    auto writeFourCC = [&out](const char* fcc) { out->write(fcc, 4); };
    auto writeU32LE  = [&out](uint32_t v) { out->writeInt(static_cast<int>(v)); };
    auto writeU16LE  = [&out](uint16_t v) { out->writeShort(static_cast<int16_t>(v)); };

    writeFourCC("RIFF");
    writeU32LE(totalRiffSize);
    writeFourCC("WAVE");

    writeFourCC("fmt ");
    writeU32LE(fmtChunkBytes);
    writeU16LE(3);                  // wFormatTag = 3 → IEEE float
    writeU16LE(numChannels);
    writeU32LE(sr32);
    writeU32LE(avgBytesPerSec);
    writeU16LE(blockAlign);
    writeU16LE(bitsPerSample);

    // RIFF: chunk size reports the unpadded payload length; the optional
    // pad byte exists only to 2-byte-align the *next* chunk and is NOT
    // included in this size field. (Strict importers like Serum mis-frame
    // when the size is overstated by 1.)
    writeFourCC("clm ");
    writeU32LE(clmLen);
    out->write(clmUtf8, clmLen);
    if (clmPad) out->writeByte(0);

    writeFourCC("data");
    writeU32LE(pcmBytes);
    out->write(samples.data(), pcmBytes);
    return true;
}

/** Generic in-place patcher for the JSON header of a .t5p — caller mutates
 *  the parsed DynamicObject; PCM tail is preserved byte-for-byte. */
template <typename Mutator>
bool patchPresetJson(const juce::File& file, Mutator mutate)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData(data)) return false;
    const auto* bytes = static_cast<const uint8_t*>(data.getData());
    const auto size = data.getSize();
    if (size < 12 || std::memcmp(bytes, "T5YN", 4) != 0) return false;

    const uint32_t version    = *reinterpret_cast<const uint32_t*>(bytes + 4);
    const uint32_t oldJsonLen = *reinterpret_cast<const uint32_t*>(bytes + 8);
    if (12 + (size_t) oldJsonLen > size) return false;

    const juce::String oldJson(reinterpret_cast<const char*>(bytes + 12),
                               static_cast<size_t>(oldJsonLen));
    auto parsed = juce::JSON::parse(oldJson);
    auto* root = parsed.getDynamicObject();
    if (root == nullptr) return false;

    mutate(*root);

    const juce::String newJson = juce::JSON::toString(parsed, true);
    const uint32_t newJsonLen = static_cast<uint32_t>(newJson.getNumBytesAsUTF8());

    juce::TemporaryFile tmp(file);
    juce::FileOutputStream out(tmp.getFile());
    if (out.failedToOpen()) return false;

    out.write("T5YN", 4);
    out.writeInt(static_cast<int>(version));
    out.writeInt(static_cast<int>(newJsonLen));
    out.write(newJson.toRawUTF8(), static_cast<size_t>(newJsonLen));

    const size_t audioOffset = 12 + (size_t) oldJsonLen;
    if (audioOffset < size)
        out.write(bytes + audioOffset, size - audioOffset);

    out.flush();
    if (! out.getStatus().wasOk()) return false;
    return tmp.overwriteTargetFileWithTemporary();
}
}  // namespace

bool MainPanel::patchPresetNameField(const juce::File& file, const juce::String& newName)
{
    return patchPresetJson(file, [&](juce::DynamicObject& root)
    {
        root.setProperty("name", newName);
    });
}

bool MainPanel::patchPresetTagsField(const juce::File& file, const juce::StringArray& newTags)
{
    return patchPresetJson(file, [&](juce::DynamicObject& root)
    {
        juce::Array<juce::var> arr;
        for (auto& t : newTags) arr.add(t);
        root.setProperty("tags", arr);
    });
}

bool MainPanel::isGithubBankFile(const juce::File& file)
{
    if (! file.existsAsFile()) return false;
    const auto bankDir = PresetFormat::getUserPresetsDirectory()
                            .getChildFile(PresetUpdater::getBankName());
    return file.isAChildOf(bankDir);
}

juce::File MainPanel::forkPresetForUserEdit(const juce::File& source,
                                            const juce::StringArray& newTags)
{
    if (! source.existsAsFile()) return {};

    // The fork's filename is always "<source> (mine).t5p" — the rename is
    // mandatory per the user's directive ("MUSS umbenennen") and there is
    // no collision-suffix ladder ("(mine 2)", "(mine 3)", …). If a fork
    // already exists from a previous edit of the same upstream preset, we
    // surgically update its `tags` array in place rather than creating a
    // second fork; the user explicitly accepts destructive overwrite to
    // avoid the duplication that the (mine 2)/(mine 3)/… ladder would
    // produce on every successive edit.
    const auto userRoot = PresetFormat::getUserPresetsDirectory();
    if (! userRoot.isDirectory()) userRoot.createDirectory();
    const auto baseName = source.getFileNameWithoutExtension();
    const auto newName  = baseName + " (mine)";
    juce::File target   = userRoot.getChildFile(newName + ".t5p");

    if (target.existsAsFile())
    {
        // Existing fork — refresh just the tag list. PCM / prompts / axes
        // stay as the user last saved them.
        if (! patchPresetTagsField(target, newTags)) return {};
        return target;
    }

    // First-time fork: byte-copy upstream, then patch both `name` (to
    // match the new filename) and `tags` in a single JSON rewrite. A
    // failed patch leaves a half-renamed copy on disk; delete it so the
    // library doesn't show a leaky orphan.
    if (! source.copyFileTo(target)) return {};
    const bool patched = patchPresetJson(target, [&](juce::DynamicObject& root)
    {
        root.setProperty("name", newName);
        juce::Array<juce::var> arr;
        for (auto& t : newTags) arr.add(t);
        root.setProperty("tags", arr);
    });
    if (! patched)
    {
        target.deleteFile();
        return {};
    }
    return target;
}

juce::String MainPanel::getCurrentPresetDisplayName() const
{
    auto stored = processorRef.getLastPresetName().trim();
    if (stored.isNotEmpty())
        return stored;

    if (currentPresetFile.existsAsFile())
        return currentPresetFile.getFileNameWithoutExtension();

    return {};
}

void MainPanel::syncGuiStateForPresetSave()
{
    processorRef.setLastPrompts(promptPanel.getPromptA(), promptPanel.getPromptB());
    processorRef.setLastSeed(promptPanel.getSeed());
    processorRef.setLastInjection(promptPanel.getInjectionMode(),
                                   promptPanel.getLateMixAmount(),
                                   promptPanel.getSplitStart(),
                                   promptPanel.getSplitEnd());

    auto axStates = axesPanel.getSlotStates();
    std::array<T5ynthProcessor::AxisSlotState, 3> procAxes;
    for (int i = 0; i < 3; ++i)
    {
        procAxes[static_cast<size_t>(i)].dropdownId = axStates[static_cast<size_t>(i)].dropdownId;
        procAxes[static_cast<size_t>(i)].value = axStates[static_cast<size_t>(i)].value;
    }
    processorRef.setLastAxes(procAxes);
}

void MainPanel::applyLoadedPreset(const PresetFormat::LoadResult& result, const juce::File& sourceFile)
{
    // Suppresses the per-param ParamEvent flood this whole-preset apply would
    // otherwise generate (every attachment-driven setValueNotifyingHost below);
    // one coarse PresetLoaded marker is logged instead at the end.
    processorRef.beginBulkParamLoad();

    activeSnapshotIndex = 0;
    applySnapshotsFromLoad(result.snapshots, result.calibEpoch);

    // Drop any pending axis-restore stash left over from a PRIOR state, BEFORE
    // loadPresetData runs. loadPresetData can fire onModelChanged -> setModelIsSA3,
    // which would otherwise PROMOTE a stale pending id into the visible dropdown
    // (whenever the loaded preset's model flips the axis table to that id's
    // family) and leak it into this preset — even an axis-less legacy one. The
    // if(result.hasAxes) branch below re-establishes the correct pending for THIS
    // preset via setSlotStates; a legacy preset correctly ends with no stash.
    axesPanel.clearPendingRestore();

    promptPanel.loadPresetData(result.promptA, result.promptB,
                               result.seed, result.randomSeed, result.device, result.model,
                               result.injectionMode,
                               result.lateMixAmount,
                               result.splitStart,
                               result.splitEnd);

    if (result.hasAxes)
    {
        std::array<AxesPanel::SlotState, 3> states;
        for (int i = 0; i < 3; ++i)
        {
            states[static_cast<size_t>(i)].dropdownId = result.axes[static_cast<size_t>(i)].dropdownId;
            states[static_cast<size_t>(i)].value = result.axes[static_cast<size_t>(i)].value;
        }
        // Ensure the dropdown holds the id set matching the just-loaded model
        // (loadPresetData above already applied it) BEFORE restoring the
        // preset's saved ids, so SAO-table ids and SA3-table ids land in the
        // matching table instead of no-op'ing blank.
        axesPanel.setModelIsSA3(promptPanel.selectedModelIsSA3());
        axesPanel.setSlotStates(states);
    }

    if (result.hasAudio)
    {
        processorRef.loadGeneratedAudio(result.audio, result.sampleRate);
        processorRef.setLastSeed(result.seed);
        processorRef.setLastPrompts(result.promptA, result.promptB);
    }

    processorRef.setInferenceCacheCapacity(0);
    promptPanel.disarmOfflineTakeStep();   // an in-flight render must not claim this cache
    if (result.inferenceCacheCapacity > 0)
    {
        processorRef.setInferenceCacheCapacity(result.inferenceCacheCapacity);
        for (const auto& entry : result.inferenceCache)
            processorRef.addInferenceCacheEntry(entry.audio, entry.sampleRate);
        // After the entries, not before: setInferenceCacheCapacity clears the flag
        // along with the cache it is about.
        processorRef.setInferenceCacheOfflineTake(result.inferenceCacheIsOfflineTake);
    }
    syncInferenceCacheUi();

    if (!result.embeddingA.empty())
    {
        processorRef.setLastEmbeddings(result.embeddingA, result.embeddingB);
        auto& apvts = processorRef.getValueTreeState();
        auto baseline = DimensionExplorer::estimateBaselineValues(
            result.embeddingA, result.embeddingB,
            apvts.getRawParameterValue(PID::genAlpha)->load(),
            apvts.getRawParameterValue(PID::genMagnitude)->load());
        dimensionExplorer.setEmbeddings(result.embeddingA, result.embeddingB, baseline, false);
    }

    // LCO/DCO bake restore (format v5+). Deliberately AFTER the result.hasAudio
    // block above: a stale neural audio_meta buffer sitting in generatedAudioFull
    // from before the bake (LCO bakes never touch it) would otherwise have its
    // loadGeneratedAudio call re-extract neural frames into masterOsc and clear
    // the just-imported LCO snapshot (importJsonPreset::clearLcoBakeSnapshot
    // path) — running this block last means the LCO restore always wins.
    // Absence (result.hasLco == false) covers both legacy v3/v4 files and a v5
    // file saved with no active bake; nothing to do.
    if (result.hasLco)
    {
        if (result.lcoOscAHasContent)
            processorRef.loadDcoWavetable(result.lcoFramesA, result.lcoMotionRateHz);
        // The dual A+B oscillator's B half is dead (BJ 2026-07-17): an old
        // preset restores only its A wavetable (above); any stationsB payload it
        // still carries is parsed but never brought back to life.
        // Re-cache the RESTORED (not re-baked) values so an immediate re-save
        // round-trips without ever calling the LLM again.
        processorRef.setLcoBakeSnapshot(result.lcoPrompt, result.lcoReadingA, result.lcoReadingB,
                                        result.lcoMotionRateHz,
                                        result.lcoOscAHasContent, result.lcoGainA,
                                        result.lcoOscBHasContent, result.lcoGainB);
        // "LCO", not "LRO": this lands in `synth.model` inside the .t5p, so it is
        // a STORED token like engine.mode, not a label. Every shipped build up to
        // 2.5.x compares it against "LCO" — writing the new spelling makes an
        // older copy read this preset as "LCO · LRO" in the Detail card, and files
        // a neural preset saved afterwards under a model row named "LRO" that
        // does not exist. The rename happens on READ instead
        // (PresetManagerPanel::parseEntry), where it costs nothing either way.
        processorRef.setLastModel("LCO");
        promptPanel.setLcoPrompt(result.lcoPrompt);
        // The retired wavetable LCO: no Csound author wrote this, so the trace
        // names none rather than inheriting whoever wrote the last orchestra.
        promptPanel.setLcoRecalledTrace(result.lcoPrompt, result.lcoReadingA, {},
                                        /*csoundBody=*/{});
    }

    // Csound orchestra restore (Phase 5, SPEC_phase4_5_csound_llm_preset.md):
    // importJsonPreset already issued requestCsoundOrchestra()/setCsoundReading()
    // as a side effect of parsing the "engine" block (see its own comment there) —
    // PromptPanel's trace view is the one piece importJsonPreset (a plain
    // processor method, no GUI access) cannot reach directly, so push the
    // now-current stash into it. Gated on the just-restored engine mode rather
    // than a new LoadResult field: exportJsonPreset only ever writes
    // csound_orchestra/csound_reading together with mode "csound", so
    // "mode == Csound" IS "there was a csound block" for any file this build
    // wrote.
    //
    // A preset stores the reading and the body, never the consultation, the
    // repairs or the author's name — so this restores the stations it HAS and
    // leaves the rest out. The authored body is not lost: it stays in
    // csoundParamsText and is what the card's back side shows.
    //
    // The PROMPT comes from the file's own csound_prompt, written beside the
    // orchestra since this build. It is never taken from the editor: that holds
    // whatever the user last typed, which did not author this sound. A file
    // saved before csound_prompt existed has none, and the HEARD station is
    // then simply absent — the preset genuinely does not know what was asked
    // for, and inventing a prompt for it would be the one thing this panel must
    // not do.
    if (static_cast<int>(processorRef.getValueTreeState().getRawParameterValue(PID::engineMode)->load())
            == static_cast<int>(EngineMode::Csound))
    {
        const auto restoredPrompt = processorRef.getCsoundPrompt();
        // The editor follows it, AND the Re-Prompt chain adopts it — the same
        // pair the SNAP recall does. GENERATE with a stance engaged re-reads the
        // chain, not the editor, so setting the text alone would rewrite and
        // bake the PREVIOUS sound's prompt and overwrite the restored one on
        // screen while doing it.
        if (restoredPrompt.isNotEmpty())
            promptPanel.setLcoPrompt(restoredPrompt);
        // The READING has to be adopted whether or not the file carried a
        // prompt: a stance turn reads the panel's last machine reading, and
        // leaving the previous bake's there makes GENERATE rewrite a sound that
        // is no longer loaded.
        promptPanel.adoptRecalledOrchestra(restoredPrompt, processorRef.getCsoundReading());
        promptPanel.setLcoRecalledTrace(restoredPrompt,
                                        processorRef.getCsoundReading(), {},
                                        processorRef.getCsoundParamsText());
        // The restore issued its own requestCsoundOrchestra() inside
        // importJsonPreset, so open a compile window for it: without one the
        // RUNNING station could only say "not observed" for an orchestra whose
        // compile is knowable, and a failed restore would never surface at all.
        promptPanel.beginCsoundCompileWatch();
    }

    // Preset selection must ALSO switch the oscillator MODE to match the restored
    // engine (BJ 2026-07-21: it did not — an LCO preset opened inside the neural
    // panel). The neural<->LCO toggle is GUI state (oscEasyMode), NOT an APVTS
    // param, so restoring engineMode alone left the panel and the GENERATE/prompt
    // routing in the previous mode. engineMode's two language values — Lco (the
    // legacy wavetable bake) and Csound (the live oscillator) — ARE the LCO;
    // Sampler/Wavetable/Freeze are the neural T5osc. persist=false: loading a
    // preset must not rewrite the user's global default mode. Guarded so an
    // already-correct mode does not trigger a redundant relayout.
    {
        const int em = static_cast<int>(
            processorRef.getValueTreeState().getRawParameterValue(PID::engineMode)->load());
        const bool presetIsLco = (em == static_cast<int>(EngineMode::Lco)
                                  || em == static_cast<int>(EngineMode::Csound));
        const bool wantNeural = ! presetIsLco;
        if (oscEasyMode != wantNeural)
            setOscEasyMode(wantNeural, /*persist=*/false);
    }

    // The standalone's session restore ("_buffer.t5p") carries a real preset
    // name whenever one was live at quit — that one is restored as before. But
    // when nothing was, PresetFormat falls back to the FILENAME (saveToFile),
    // and the session came back calling itself "_buffer": in the status bar, and
    // pre-filled as the Save name (it is not a recognised placeholder, so the
    // drawer's auto-title never kicks in) — one Return away from writing
    // _buffer.t5p into the preset library. Drop only that literal.
    const auto restoredName = (sourceFile.getFileName() == "_buffer.t5p"
                               && result.presetName.trim() == "_buffer")
                                  ? juce::String() : result.presetName;
    processorRef.setLastPresetName(restoredName);
    processorRef.setLastTags(result.tags);
    statusBar.setPresetName(restoredName);

    if (sourceFile.existsAsFile()
        && sourceFile.getFileName() != "_buffer.t5p"
        && sourceFile.isAChildOf(PresetFormat::getUserPresetsDirectory()))
    {
        currentPresetFile = sourceFile;
    }
    else
    {
        currentPresetFile = juce::File();
    }

    // Passive session-restore (the standalone "_buffer.t5p" written on quit) must NOT
    // auto-resume the Re-Prompt loop on launch — the standalone analogue of
    // setStateInformation's anti-surprise force-Off. importJsonPreset deliberately
    // restores the stance so a DELIBERATE .t5p load (user clicks a preset) reproduces
    // the machine patch; only this passive buffer path nukes it back to Off.
    // Both stances: the LCO one drives pollLcoRepromptCadence exactly as the neural
    // one drives pollDriftRegen, so a quit in LRO with a stance engaged and a
    // non-Manual REGENERATE would otherwise re-author the restored orchestra on the
    // next launch, unattended, before the user has touched anything.
    if (sourceFile.getFileName() == "_buffer.t5p")
        for (auto* pid : { PID::repromptStance, PID::dcoRepromptStance })
            if (auto* stanceParam = processorRef.getValueTreeState().getParameter(pid))
                stanceParam->setValueNotifyingHost(0.0f);

    presetManager.setCurrentPreset(currentPresetFile, restoredName);

    // The event log still records what was actually loaded, buffer included —
    // that line is a session-restore marker, not the save identity.
    processorRef.endBulkParamLoad(result.presetName);
}

bool MainPanel::savePresetToFile(const juce::File& file, bool includeInferenceCache)
{
    syncGuiStateForPresetSave();

    auto target = file.withFileExtension("t5p");
    processorRef.setLastPresetName(target.getFileNameWithoutExtension());

    const auto snaps = buildSnapshotsForSave();
    if (!PresetFormat::saveToFile(target, processorRef, includeInferenceCache,
                                   snaps.empty() ? nullptr : &snaps))
    {
        statusBar.setStatusText("Preset save failed");
        return false;
    }

    currentPresetFile = target;
    statusBar.setPresetName(target.getFileNameWithoutExtension());
    statusBar.setStatusText("Saved preset: " + target.getFileName());
    presetManager.setCurrentPreset(target, target.getFileNameWithoutExtension());
    return true;
}

bool MainPanel::loadPresetFromFile(const juce::File& file)
{
    auto result = PresetFormat::loadFromFile(file, processorRef);
    if (!result.success)
    {
        statusBar.setStatusText("Preset load failed");
        return false;
    }

    applyLoadedPreset(result, file);
    statusBar.setStatusText("Loaded preset: " + result.presetName);
    return true;
}

void MainPanel::finishPresetUpdate(bool success,
                                   PresetUpdater::Stats stats,
                                   juce::String error)
{
    presetManager.setUpdaterBusy(false);

    if (! success)
    {
        const auto msg = error.isNotEmpty() ? error
                                            : juce::String("Library update failed.");
        presetManager.setStatusText(msg, true);
        return;
    }

    // Refresh after a successful run so the new bank entries show up
    // immediately. Even when nothing changed we still rescan — cheap, and
    // protects against rare race conditions where the user deleted a file
    // between the diff and the prune pass.
    presetManager.refreshLibrary();

    juce::String summary;
    summary << juce::String::fromUTF8("Library up to date \xe2\x80\x94 ")
            << stats.added     << " added, "
            << stats.updated   << " updated, "
            << stats.unchanged << " unchanged";
    if (stats.failed  > 0) summary << ", " << stats.failed  << " failed";
    presetManager.setStatusText(summary, stats.failed > 0);
}

void MainPanel::importPresetFile()
{
    // Multi-select file picker — drops the per-file overwrite confirm
    // (silently broken on Linux anyway) in favour of automatic suffixing
    // " (1)", " (2)", … on filename collision. Imported files are NOT
    // auto-loaded; the user double-clicks in the library to load.
    auto presetsDir = PresetFormat::getUserPresetsDirectory();
    auto chooser = std::make_shared<juce::FileChooser>(
        "Import Presets", presetsDir, "*.t5p");

    juce::Component::SafePointer<MainPanel> safeThis(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                       | juce::FileBrowserComponent::canSelectFiles
                       | juce::FileBrowserComponent::canSelectMultipleItems,
        [safeThis, chooser](const juce::FileChooser& fc)
        {
            if (! safeThis) return;
            auto* self = safeThis.getComponent();

            const auto results = fc.getResults();
            if (results.isEmpty()) return;

            const auto userDir = PresetFormat::getUserPresetsDirectory();
            int imported = 0, renamed = 0, skippedNonT5p = 0, failed = 0;
            juce::File lastImported;

            for (auto& src : results)
            {
                if (! src.existsAsFile())          { ++failed; continue; }
                if (! src.hasFileExtension("t5p")) { ++skippedNonT5p; continue; }

                // Do not import a file that already lives in the user dir
                if (src.isAChildOf(userDir)) { continue; }

                auto target = userDir.getChildFile(src.getFileName());
                bool didRename = false;
                if (target.existsAsFile())
                {
                    target = target.getNonexistentSibling(true);
                    didRename = true;
                }

                if (src.copyFileTo(target))
                {
                    ++imported;
                    if (didRename) ++renamed;
                    lastImported = target;
                }
                else
                {
                    ++failed;
                }
            }

            self->presetManager.refreshLibrary();
            if (lastImported.existsAsFile())
                self->presetManager.setCurrentPreset(self->currentPresetFile,
                                                    self->getCurrentPresetDisplayName());

            juce::String msg;
            msg << "Imported " << imported << (imported == 1 ? " preset" : " presets");
            if (renamed > 0)        msg << " (" << renamed << " renamed to avoid clash)";
            if (skippedNonT5p > 0)  msg << ", skipped " << skippedNonT5p << " non-.t5p";
            if (failed > 0)         msg << ", " << failed << " failed";
            self->presetManager.setStatusText(msg, failed > 0);
        });
}

void MainPanel::mouseDown(const juce::MouseEvent& e)
{
    // Close overlays on click outside
    if (dimExplorerVisible)
    {
        auto dimBounds = dimensionExplorer.getBounds();
        if (!dimBounds.contains(e.x, e.y))
            hideDimExplorer();
    }
    if (settingsVisible)
    {
        auto settingsBounds = settingsTabs.getBounds();   // tabs are the MainPanel-space child now
        if (!settingsBounds.contains(e.x, e.y))
            hideSettings();
    }
    if (manualVisible)
    {
        auto mb = manualPanel.getBounds();
        if (!mb.contains(e.x, e.y))
            hideManual();
    }
}

bool MainPanel::keyPressed(const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();

    // Replay overlay up: swallow ALL shortcuts. The overlay blocks the mouse by
    // covering the panel, but key events propagate up to this handler regardless of
    // focus — without this, Cmd+S would save the TAPE's patch as a user preset,
    // Shift+Return would fire a generation against the tape, and 1-4 would apply
    // snapshots over it. (Computer-keyboard notes are separately gated in the
    // processor.) Consume rather than pass: no shortcut may act on tape state.
    if (replayOverlay_.isVisible())
        return true;

    if ((mods.isCommandDown() || mods.isCtrlDown()) && key.getTextCharacter() == 'k')
    {
        setComputerKeyboardEnabled(!computerKeyboardEnabled);
        return true;
    }

    if (key.getKeyCode() == juce::KeyPress::returnKey
        && (mods.isShiftDown() || mods.isCommandDown() || mods.isCtrlDown()))
    {
        if (settingsVisible || manualVisible || presetManagerVisible || seqLibraryVisible)
            return false;
        // Easy-only: in Advanced (the DCO panel) the neural prompts are hidden —
        // a shortcut generating from invisible text is exactly the paradigm
        // leak the DCO panel exists to prevent. BAKE has its own Return key.
        if (!oscEasyMode)
            return false;
        triggerMainGeneration();
        return true;
    }

    // ⌘S / Ctrl+S opens the Library in Save mode. Skipped when another
    // modal overlay (settings, manual, dim explorer) is up so the
    // shortcut doesn't yank the user out of an unrelated workflow. Also
    // skipped if the Library is already in Save mode (a no-op re-entry
    // would just reset the typed name).
    if ((mods.isCommandDown() || mods.isCtrlDown()) && key.getTextCharacter() == 's')
    {
        if (settingsVisible || manualVisible || dimExplorerVisible || seqLibraryVisible) return false;
        if (presetManagerVisible
            && presetManager.getMode() == PresetManagerPanel::Mode::Save)
            return true;   // already there; consume so nothing else fires
        savePreset();
        return true;
    }

    if (!isTextEditingFocus() && !settingsVisible && !manualVisible && !presetManagerVisible
        && !seqLibraryVisible)
    {
        const juce_wchar c = key.getTextCharacter();
        if (c >= '1' && c <= '4')
        {
            const int slot = static_cast<int>(c - '0');
            if (mods.isShiftDown())
            {
                captureSnapshotPress(slot);   // neural audio, or the LCO orchestra
                storeSnapshotFromPress(slot);
            }
            else
            {
                activateSnapshot(slot);
            }
            return true;
        }

        const bool plainKeyboardCommand = computerKeyboardEnabled
                                       && !mods.isCommandDown()
                                       && !mods.isCtrlDown()
                                       && !mods.isAltDown();

        // Step-record: Space inserts an empty step (rest) while armed — independent
        // of the typing-keyboard piano mode (Space is never a note key), so it works
        // whether you play in via MIDI or the computer keyboard. Edge-gated via
        // spaceRestKeyDown_ (re-armed in pollComputerKeyboard when the key lifts)
        // so OS auto-repeat doesn't spam rests.
        if (processorRef.isStepRecordArmed() && key.getKeyCode() == juce::KeyPress::spaceKey
            && ! mods.isCommandDown() && ! mods.isCtrlDown() && ! mods.isAltDown())
        {
            if (! spaceRestKeyDown_)
            {
                spaceRestKeyDown_ = true;
                processorRef.recordStepRest();
            }
            return true;
        }

        // keyPressed only fires while we hold keyboard focus, so this is the
        // focus-scoped note-ON path (allowStart = true). Returns true when a
        // mapped physical key is held → consume so it doesn't trigger shortcuts.
        if (plainKeyboardCommand && scanComputerKeyboard(true))
            return true;
    }
    return false;
}

void MainPanel::toggleSettings()
{
    if (settingsVisible) hideSettings(); else showSettings();
}

void MainPanel::showSettings()
{
    settingsVisible = true;
    // When the Settings badge is what drew the user here (a pending update),
    // open straight to the "Settings" tab (index 2, after Sound Models and
    // Language Model) where the Download row is, so the update is visible
    // immediately — Chrome/VS Code "open → it's right there" behaviour.
    // One-shot: cleared after the jump so later opens keep the default tab and
    // the model-setup open (tab 0) is never hijacked.
    if (pendingUpdateTabJump_)
    {
        settingsTabs.setCurrentTabIndex(2);
        pendingUpdateTabJump_ = false;
    }
    settingsScrim.setVisible(true);
    settingsScrim.toFront(false);
    settingsTabs.setVisible(true);
    settingsTabs.toFront(false);
    // The API bill is fetched on OPEN as well as on tab change: the page's own
    // visibilityChanged() only fires when its own flag flips, and re-showing the
    // overlay on a tab that was already the current one does not flip it — so
    // without this the number would freeze at whatever it was when the tab was
    // last switched to.
    lroAuthorSettingsPage.refreshApiSpend();
    resized();
}

void MainPanel::hideSettings()
{
    settingsVisible = false;
    settingsScrim.setVisible(false);
    settingsTabs.setVisible(false);
    resized();
}

void MainPanel::tryLoadInferenceModels(bool forceRestart)
{
    statusBar.setConnected(false);
    statusBar.setStatusText(forceRestart ? "Refreshing model..." : "Loading model...");
    settingsPage.setBackendStarting();

    const auto bundledBackendMode = juce::SystemStats::getEnvironmentVariable("T5YNTH_REQUIRE_BUNDLED_BACKEND", {})
                                        .trim();
    const auto forceBundledBackend = bundledBackendMode.equalsIgnoreCase("1")
                                  || bundledBackendMode.equalsIgnoreCase("true");

    // Find backend directory — accepts either:
    //   backend/pipe_inference.py  (dev: Python script)
    //   backend/pipe_inference     (release: PyInstaller binary)
    //   backend/pipe_inference.exe (Windows release)
    //   backend/dist/pipe_inference/pipe_inference      (local PyInstaller build)
    //   backend/dist/pipe_inference/pipe_inference.exe  (local Windows PyInstaller build)
    auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::File backendDir;

    auto hasBackend = [](const juce::File& dir) {
        return dir.getChildFile("pipe_inference.py").existsAsFile()
            || dir.getChildFile("pipe_inference").existsAsFile()
            || dir.getChildFile("pipe_inference.exe").existsAsFile()
            || dir.getChildFile("dist/pipe_inference/pipe_inference").existsAsFile()
            || dir.getChildFile("dist/pipe_inference/pipe_inference.exe").existsAsFile();
    };

    // 1. Standalone macOS app bundle: Contents/MacOS/T5ynth → Contents/Resources/backend
    //    (This branch hits when running the T5ynth Standalone.app directly.)
    auto resources = exe.getParentDirectory().getSiblingFile("Resources").getChildFile("backend");
    if (hasBackend(resources))
        backendDir = resources;

   #if JUCE_MAC
    // Test mode for validating packaged apps on a dev machine: only accept the
    // backend embedded in the current app bundle, never a repo/companion fallback.
    const bool allowSearchUpwards = !forceBundledBackend;
   #else
    const bool allowSearchUpwards = true;
   #endif

    // 2. Walk up from executable (dev builds, Linux/Windows standalone layout:
    //    T5ynth.exe next to backend/).
    if (!backendDir.exists() && allowSearchUpwards)
    {
        auto search = exe.getParentDirectory();
        for (int i = 0; i < 8; ++i)
        {
            auto candidate = search.getChildFile("backend");
            if (hasBackend(candidate))
            {
                backendDir = candidate;
                break;
            }
            search = search.getParentDirectory();
        }
    }

    // 3. Plugin context (VST3/AU): the exe is the DAW, not the instrument. Look
    //    for a companion Standalone install and borrow its bundled backend.
    //    Release archives ship the heavy backend only with the Standalone —
    //    VST3/AU plugins piggy-back on it so the plugin downloads stay small.
    //    The pre-rename bundle name stays in the list: a machine that still has
    //    T5ynth installed has a perfectly good backend, and the plugin has no
    //    reason to refuse it.
    if (!backendDir.exists() && !forceBundledBackend)
    {
       #if JUCE_MAC
        juce::Array<juce::File> companionApps {
            juce::File("/Applications/akroasys.app"),
            juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                .getChildFile("Applications/akroasys.app"),
            juce::File("/Applications/T5ynth.app"),
            juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                .getChildFile("Applications/T5ynth.app")
        };
        for (const auto& app : companionApps)
        {
            auto candidate = app.getChildFile("Contents/Resources/backend");
            if (hasBackend(candidate))
            {
                backendDir = candidate;
                break;
            }
        }
       #elif JUCE_WINDOWS
        // Windows: prefer the installer-written registry path, then fall back
        // to the default install prefixes.
        auto companionRoots = getWindowsCompanionBackendRoots();
        for (const auto& p : companionRoots)
        {
            juce::File candidate (p);
            if (hasBackend(candidate))
            {
                backendDir = candidate;
                break;
            }
        }
       #elif JUCE_LINUX
        juce::StringArray companionRoots {
            "/opt/akroasys/backend",
            "/usr/local/share/akroasys/backend",
            "/opt/T5ynth/backend",
            "/usr/local/share/T5ynth/backend"
        };
        for (const auto& p : companionRoots)
        {
            juce::File candidate (p);
            if (hasBackend(candidate))
            {
                backendDir = candidate;
                break;
            }
        }
       #endif
    }

    // 4. Last-resort dev fallback: compile-time project backend path.
    //    Only active in dev builds where T5YNTH_BACKEND_DIR is defined.
   #ifdef T5YNTH_BACKEND_DIR
    if (!backendDir.exists() && !forceBundledBackend)
    {
        juce::File devBackend (T5YNTH_BACKEND_DIR);
        if (hasBackend(devBackend))
            backendDir = devBackend;
    }
   #endif

    if (backendDir.exists())
    {
        juce::Component::SafePointer<MainPanel> safeThis(this);
        auto pipePtr = processorRef.getPipeInferencePtr();
        std::thread([safeThis, pipePtr, backendDir, forceRestart]()
        {
            if (forceRestart)
                pipePtr->shutdown();

            bool ok = pipePtr->launch(backendDir);
            auto errorMsg = ok ? juce::String() : pipePtr->getLastError();
            juce::MessageManager::callAsync([safeThis, ok, errorMsg]()
            {
                if (auto* self = safeThis.getComponent())
                {
                    if (ok)
                    {
                        self->statusBar.setConnected(true);
                        self->statusBar.setStatusText("Ready");
                        self->settingsPage.setBackendConnected(true);
                        self->promptPanel.refreshInferenceChoices();
                    }
                    else
                    {
                        self->statusBar.setConnected(false);
                        self->statusBar.setStatusText("Backend start failed");
                        self->settingsPage.setBackendFailed(errorMsg);
                    }
                }
            });
        }).detach();
    }
    else
    {
        // Plugin context with no companion install is the most common failure —
        // give the user an actionable hint instead of the generic message.
        const auto msg = forceBundledBackend
                         ? juce::String("Bundled backend not found in app")
                         : (juce::JUCEApplicationBase::isStandaloneApp()
                            ? juce::String::fromUTF8("Backend not found \xe2\x80\x94 reinstall ") + productName()
                            : juce::String::fromUTF8("Backend not found \xe2\x80\x94 install the ") + productName() + " app");
        statusBar.setStatusText(msg);
        settingsPage.setBackendFailed(forceBundledBackend ? "Bundled backend missing" : "Not found");
    }
}

// ── Buffer preset: persist full state on Standalone quit ──
static juce::File getBufferPresetFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("T5ynth")
               .getChildFile("_buffer.t5p");
}

MainPanel::~MainPanel()
{
    // FIRST: a replay running at quit must be stopped here, not in ~PromptPanel.
    // promptPanel is declared first in MainPanel.h and so is destroyed LAST — after
    // this body has already written the standalone buffer preset below. Restoring
    // there would be too late: the buffer would carry the TAPE's patch and the
    // user's unsaved work would be gone on next launch. (PresetFormat::saveToFile
    // reads the live APVTS, so it cannot benefit from getStateInformation's own
    // mid-replay guard.) Guarded: stopReplay() raises a MIDI panic, which must not
    // fire on every ordinary editor close and cut the notes the user is holding.
    if (processorRef.isReplayActive())
        processorRef.stopReplay();

    statusBar.onMidiOutputDeviceChanged = nullptr;
    processorRef.onReplayFinished = nullptr;   // captures `this`; processor outlives us
    processorRef.onMidiLearnStateChanged = nullptr;
    processorRef.onGenerateRequested = nullptr;
    processorRef.onSnapshotRequested = nullptr;
    processorRef.onCacheToggleRequested = nullptr;
    releaseComputerKeyboardNotes();
    t5::stopPhysicalKeyMonitor();
    stopTimer();

    if (!juce::JUCEApplicationBase::isStandaloneApp())
        return;

    syncGuiStateForPresetSave();
    auto bufFile = getBufferPresetFile();
    bufFile.getParentDirectory().createDirectory();
    const auto snaps = buildSnapshotsForSave();
    PresetFormat::saveToFile(bufFile, processorRef, true,
                             snaps.empty() ? nullptr : &snaps);
}

void MainPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBg);

    float w = static_cast<float>(getWidth());
    float footerTop = static_cast<float>(sequencerPanel.getY());

    g.setColour(kBorder);
    float x1 = static_cast<float>(promptPanel.getRight() + 4);
    g.drawVerticalLine(juce::roundToInt(x1), 0.0f, footerTop);
    g.drawHorizontalLine(juce::roundToInt(footerTop), 0.0f, w);

    int inset = 4;

    // Card 1: OSCILLATOR (oscHeader + promptPanel)
    {
        int top = oscHeader.getY() - inset;
        int bot = promptPanel.getBottom() + inset;
        int left = oscHeader.getX() - inset;
        int cardW = promptPanel.getWidth() + inset * 2;
        paintCard(g, juce::Rectangle<int>(left, top, cardW, bot - top));
    }

    // Card 2: SEMANTIC AXES | DIM EXPLORER (segment switch + shared box).
    // Easy-only: in Advanced the DCO panel owns the whole column and these
    // components keep stale last-Easy bounds — painting from them would draw
    // orphaned card chrome behind/next to the DCO canvas.
    if (oscEasyMode)
    {
        int top = axesDimSegBtns[0].getY() - inset;
        int bot = axesPanel.getBottom() + inset;
        int left = axesDimSegBtns[0].getX() - inset;
        int cardW = axesPanel.getWidth() + inset * 2;
        paintCard(g, juce::Rectangle<int>(left, top, cardW, bot - top));
    }

    // Card 3: Generate button + cache/resynth block (below the shared box)
    if (oscEasyMode && !dimExplorerVisible)
    {
        int top = axesPanel.getBottom() + inset;
        int bot = sequencerPanel.getY() - inset;
        int left = promptPanel.getX() - inset;
        int cardW = promptPanel.getWidth() + inset * 2;
        if (bot > top)
            paintCard(g, juce::Rectangle<int>(left, top, cardW, bot - top));
    }

    if (!snapshotSwitchBounds.isEmpty())
        paintSwitchBoxBorder(g, snapshotSwitchBounds);
    if (!cacheSwitchBounds.isEmpty())
        paintSwitchBoxBorder(g, cacheSwitchBounds);

    if (oscEasyMode && glowGenerating)
    {
        const float pulse = 0.5f + 0.5f * std::sin(glowPhase);
        auto gb = mainGenerateBtn.getBounds().toFloat().expanded(7.0f, 5.0f);
        g.setColour(kOscCol.withAlpha(0.10f + 0.10f * pulse));
        g.fillRect(gb);
        g.setColour(kOscCol.withAlpha(0.18f + 0.10f * pulse));
        g.drawRect(gb, 1.0f);
    }
}

void MainPanel::syncInferenceCacheUi()
{
    const int capacity = processorRef.getInferenceCacheCapacity();
    const int fill = processorRef.getInferenceCacheFillCount();
    const bool full = processorRef.isInferenceCacheFull();
    updateGenerateButtonsForCacheState(false);

    if (capacity == lastInfCacheUiCapacity
        && fill == lastInfCacheUiFill
        && full == lastInfCacheUiFull)
        return;

    lastInfCacheUiCapacity = capacity;
    lastInfCacheUiFill = fill;
    lastInfCacheUiFull = full;

    static constexpr int values[kNumInfCacheButtons] = { 0, 2, 4, 8, 16 };

    // Pulse the *selected* button while the cache is filling. Once full,
    // pulsing stops and the button sits at solid kOscCol — that solid state
    // is the "cache full" signal, replacing the dropped status text row.
    // Neural-only: the cache buttons are disabled + dimmed in LCO — never animate them.
    const bool isFilling = oscEasyMode && (capacity > 0) && (fill < capacity);
    for (int i = 0; i < kNumInfCacheButtons; ++i)
    {
        infCacheButtons[i].setToggleState(capacity == values[i], juce::dontSendNotification);
        infCacheButtons[i].setPulsing(isFilling);
    }
}

void MainPanel::updateGenerateButtonsForCacheState(bool pulseCacheHit)
{
    // Neural-only. In LCO the reused GENERATE button is owned by onLcoBusyChanged
    // (bake busy-state) + setOscEasyMode (mode reset); letting this 20 Hz cache path
    // touch it would re-enable it mid-bake and could leak a "cache hit" label there.
    if (!oscEasyMode)
        return;

    const bool cachePlaybackReady = processorRef.isInferenceCacheFull();
    if (cachePlaybackReady)
    {
        mainGenerateBtn.setButtonText("cache hit");
        mainGenerateBtn.setEnabled(true);
        dimApplyBtn.setButtonText("cache hit");
        dimApplyBtn.setEnabled(true);
        if (pulseCacheHit)
            glowGenerating = true;
        return;
    }

    if (processorRef.getInferenceCacheCapacity() == 0)
    {
        cacheHitActive = false;
        if (!promptPanel.isGenerating())
            glowGenerating = false;
    }

    if (!promptPanel.isGenerating() && !glowGenerating && !cacheHitActive)
    {
        mainGenerateBtn.setButtonText("GENERATE");
        mainGenerateBtn.setEnabled(true);
        dimApplyBtn.setButtonText("Apply + Generate");
        dimApplyBtn.setEnabled(true);
    }
}

MainPanel::MainSnapshot MainPanel::captureMainSnapshot()
{
    MainSnapshot snapshot;

    const auto& rawAudio = processorRef.getGeneratedAudioRaw();
    const auto& processedAudio = processorRef.getGeneratedAudio();
    const auto& sourceAudio = rawAudio.getNumSamples() > 0 ? rawAudio : processedAudio;
    if (sourceAudio.getNumChannels() <= 0 || sourceAudio.getNumSamples() <= 0)
        return snapshot;

    snapshot.audio.makeCopyOf(sourceAudio);
    snapshot.sampleRate = processorRef.getGeneratedSampleRate();
    if (snapshot.sampleRate <= 0.0)
        snapshot.sampleRate = processorRef.getSampleRate() > 0.0 ? processorRef.getSampleRate() : 44100.0;

    snapshot.parameters = processorRef.getValueTreeState().copyState();
    snapshot.promptA = promptPanel.getPromptA();
    snapshot.promptB = promptPanel.getPromptB();
    snapshot.device = processorRef.getLastDevice();
    snapshot.model = processorRef.getLastModel();
    snapshot.injectionMode = promptPanel.getInjectionMode();
    snapshot.seed = promptPanel.getSeed();
    snapshot.randomSeed = promptPanel.isRandomSeed();
    snapshot.lateMixAmount = promptPanel.getLateMixAmount();
    snapshot.splitStart = promptPanel.getSplitStart();
    snapshot.splitEnd = promptPanel.getSplitEnd();
    snapshot.axes = axesPanel.getSlotStates();
    snapshot.embeddingA = processorRef.getLastEmbeddingA();
    snapshot.embeddingB = processorRef.getLastEmbeddingB();
    snapshot.dimensionOffsets = dimensionExplorer.getDimensionOffsets();

    {
        const juce::ScopedLock sl(processorRef.getCallbackLock());
        auto& sampler = processorRef.getSampler();
        snapshot.loopStart = sampler.getLoopStart();
        snapshot.loopEnd = sampler.getLoopEnd();
        snapshot.startPos = sampler.getStartPos();
        snapshot.wtExtractStart = sampler.getWtExtractStart();
        snapshot.wtExtractEnd = sampler.getWtExtractEnd();
        snapshot.pointsLocked = sampler.getPointsLocked();
    }

    snapshot.valid = true;
    return snapshot;
}

void MainPanel::restoreMainSnapshot(const MainSnapshot& snapshot)
{
    if (!snapshot.valid)
        return;

    auto& apvts = processorRef.getValueTreeState();
    // Guard as restoreLcoSnapshot does: an invalid tree is indistinguishable
    // from "every id absent", and restoreParameterFromState now defaults on
    // absence — without this, a slot saved without a parameter tree would
    // factory-reset the live patch instead of leaving it alone.
    if (snapshot.parameters.isValid())
        for (auto* id : kMainSnapshotParamIds)
            restoreParameterFromState(apvts, snapshot.parameters, id);

    promptPanel.loadPresetData(snapshot.promptA, snapshot.promptB,
                               snapshot.seed, snapshot.randomSeed,
                               snapshot.device, snapshot.model,
                               snapshot.injectionMode,
                               snapshot.lateMixAmount,
                               snapshot.splitStart,
                               snapshot.splitEnd);
    axesPanel.setSlotStates(snapshot.axes);
    std::array<T5ynthProcessor::AxisSlotState, 3> procAxes;
    for (int i = 0; i < 3; ++i)
    {
        procAxes[static_cast<size_t>(i)].dropdownId = snapshot.axes[static_cast<size_t>(i)].dropdownId;
        procAxes[static_cast<size_t>(i)].value = snapshot.axes[static_cast<size_t>(i)].value;
    }

    processorRef.setLastDevice(snapshot.device);
    processorRef.setLastModel(snapshot.model);
    processorRef.setLastSeed(snapshot.seed);
    processorRef.setLastPrompts(snapshot.promptA, snapshot.promptB);
    processorRef.setLastInjection(snapshot.injectionMode,
                                  snapshot.lateMixAmount,
                                  snapshot.splitStart,
                                  snapshot.splitEnd);
    processorRef.setLastAxes(procAxes);

    if (!snapshot.embeddingA.empty())
    {
        processorRef.setLastEmbeddings(snapshot.embeddingA, snapshot.embeddingB);
        const float alpha = apvts.getRawParameterValue(PID::genAlpha)->load();
        const float magnitude = apvts.getRawParameterValue(PID::genMagnitude)->load();
        auto baseline = DimensionExplorer::estimateBaselineValues(snapshot.embeddingA,
                                                                  snapshot.embeddingB,
                                                                  alpha,
                                                                  magnitude);
        dimensionExplorer.setEmbeddings(snapshot.embeddingA, snapshot.embeddingB, baseline, false);
        dimensionExplorer.setDimensionOffsets(snapshot.dimensionOffsets);
    }
    else
    {
        dimensionExplorer.clear();
    }

    auto applyMarkers = [&]()
    {
        const float loopStart = juce::jlimit(0.0f, 0.99f, snapshot.loopStart);
        float loopEnd = juce::jlimit(0.01f, 1.0f, snapshot.loopEnd);
        if (loopEnd < loopStart + 0.01f)
            loopEnd = juce::jmin(1.0f, loopStart + 0.01f);

        auto& sampler = processorRef.getSampler();
        sampler.setPointsLocked(true);
        // One call, not the open-it-first dance: this lambda runs a SECOND time
        // after loadGeneratedAudio has published the slot's sample, and the old
        // sequence's intermediate setLoopEnd(1.0f) marked the master for a
        // re-prepare even though the pair ends where it already was. The
        // re-prepare republished the same audio ~10 ms later, and a held note
        // had to follow a publication that carried no change at all.
        sampler.setLoopRegion(loopStart, loopEnd);
        sampler.setStartPos(juce::jlimit(0.0f, 1.0f, snapshot.startPos));
        sampler.setWtExtractStart(juce::jlimit(0.0f, 1.0f, snapshot.wtExtractStart));
        sampler.setWtExtractEnd(juce::jlimit(0.0f, 1.0f, snapshot.wtExtractEnd));
    };

    {
        const juce::ScopedLock sl(processorRef.getCallbackLock());
        applyMarkers();
    }
    processorRef.loadGeneratedAudio(snapshot.audio, snapshot.sampleRate);
    {
        const juce::ScopedLock sl(processorRef.getCallbackLock());
        applyMarkers();
        processorRef.getSampler().setPointsLocked(snapshot.pointsLocked);
    }
}

// ── LCO SNAP: the authored orchestra IS the sound ────────────────────────────
// The neural side snapshots rendered audio; the LCO has none — its sound is the
// Csound source the LLM wrote plus the parameter chain around it. So a slot
// stores the orchestra text, the disclosure that explains it, and the same
// sound-shaping parameters the neural slots restore (minus the neural-only ids,
// see kLcoSnapshotSkipParamIds). A recall then sounds within one compile, with
// no LLM pass — where parking the prompt alone meant re-authoring minutes of
// work for a sound the author never reproduces exactly.
MainPanel::LcoSnapshot MainPanel::captureLcoSnapshot()
{
    LcoSnapshot snapshot;
    snapshot.prompt      = promptPanel.getLcoPrompt().trim();
    // From the processor, not the editor: the editor may already hold the next
    // idea while the sound this slot is keeping still plays. Everything else
    // here comes from the processor for the same reason.
    snapshot.authoringPrompt = processorRef.getCsoundPrompt();
    snapshot.orchestra   = processorRef.getCsoundOrchestraText();
    snapshot.reading     = processorRef.getCsoundReading();
    snapshot.paramsText  = processorRef.getCsoundParamsText();
    snapshot.authorModel = promptPanel.getLcoAuthorModel();
    snapshot.controls    = processorRef.getCsoundControls();
    snapshot.parameters  = processorRef.getValueTreeState().copyState();
    // Prompt-only slots stay possible (nothing baked yet) — that was the whole
    // of the old behaviour and it is kept, not replaced. Neither prompt nor
    // orchestra means there is nothing to bring back.
    snapshot.valid = snapshot.prompt.isNotEmpty() || snapshot.orchestra.isNotEmpty();
    return snapshot;
}

void MainPanel::restoreLcoSnapshot(const LcoSnapshot& snapshot)
{
    if (!snapshot.valid)
        return;

    // A slot restores what it actually holds and touches nothing else. Empty
    // prompt = the user had cleared the editor when the slot was taken; that is
    // not an instruction to clear what they have typed since. The Re-Prompt chain
    // follows the recalled words either way — with a stance engaged, GENERATE
    // rewrites dcoLoopLast_, not the editor, so a prompt recalled but not adopted
    // would be silently replaced by the previous one instead of being baked.
    if (snapshot.prompt.isNotEmpty())
    {
        promptPanel.setLcoPrompt(snapshot.prompt);
        promptPanel.adoptRecalledPrompt(snapshot.prompt);
    }

    // Stored before the first bake: the prompt is ALL there is. No orchestra, and
    // therefore no parameters either — the patch around a sound this slot never
    // captured belongs to the sound that is playing now. Leave the engine, the
    // current orchestra, the HEARD AS card and every knob exactly where they are.
    if (snapshot.orchestra.isEmpty())
        return;

    auto& apvts = processorRef.getValueTreeState();
    if (snapshot.parameters.isValid())
        for (auto* id : kMainSnapshotParamIds)
            if (!isLcoSnapshotSkippedParam(id))
                restoreParameterFromState(apvts, snapshot.parameters, id);

    // The knobs of the orchestra in this slot: what they MEAN first, then where
    // the player had them. Both, and neither on its own — the reading without
    // the positions gives correctly named sliders on a sound that is not the one
    // the slot took, and the positions without the reading put the previous
    // instrument's captions over this one's channels. `applyValues=false` because
    // the values are the SLOT'S, not the author's starting positions: recalling a
    // slot must return the sound that was stored, tuning included.
    // They are restored here rather than through kMainSnapshotParamIds because
    // they belong to a written orchestra, which is what only an LCO slot holds.
    processorRef.setCsoundControls(snapshot.controls, /*applyValues=*/false);
    if (snapshot.parameters.isValid())
    {
        static constexpr const char* kLroKnobIds[] = {
            PID::lroP1a, PID::lroP1b, PID::lroP1c, PID::lroP1d,
            PID::lroP2a, PID::lroP2b, PID::lroP2c, PID::lroP2d,
            PID::lroP3a, PID::lroP3b, PID::lroP3c, PID::lroP3d,
            // The parts' levels belong to the recalled instrument exactly as its
            // knobs do: a snapshot that brought back the sound but not the
            // balance between its parts would recall a different mix every time.
            PID::lroLvl1, PID::lroLvl2, PID::lroLvl3 };
        for (auto* id : kLroKnobIds)
            restoreParameterFromState(apvts, snapshot.parameters, id);
    }

    // The disclosure travels with the code, so the card explains the orchestra
    // that is actually sounding — and a Save right after a recall round-trips the
    // recalled sound rather than the last bake's (exportJsonPreset reads these).
    processorRef.setCsoundPrompt(snapshot.authoringPrompt);
    processorRef.setCsoundReading(snapshot.reading);
    processorRef.setCsoundParamsText(snapshot.paramsText);
    // A slot stores the reading, the body and the author — not the consultation
    // or the repairs, which happened at bake time. The trace shows what the slot
    // actually holds; the missing stations stay absent rather than blank.
    promptPanel.setLcoRecalledTrace(snapshot.prompt, snapshot.reading, snapshot.authorModel,
                                    processorRef.getCsoundParamsText());
    // Whoever wrote THIS orchestra — including "not known", which must clear the
    // previous bake's name rather than let it stand over a different sound.
    if (snapshot.authorModel.isNotEmpty())
        promptPanel.setLcoAuthorModel(snapshot.authorModel);
    else
        promptPanel.resetLcoAuthorModel();
    // Re-Prompt reads the panel's own last reading and prompt to build its next
    // turn; without this a stance press would rewrite the bake the recall just
    // replaced (docs/DCO_REPROMPT_CONCEPT.md).
    promptPanel.adoptRecalledOrchestra(snapshot.prompt, snapshot.reading);

    // Same hand-off as a fresh bake (PromptPanel::triggerDcoBake): force Csound —
    // a slot can be recalled in the LCO before anything was ever authored this
    // session — then queue the orchestra. requestCsoundOrchestra() compiles on the
    // processor's own background thread and crossfades the swap in.
    processorRef.forceCsoundEngineMode();
    processorRef.requestCsoundOrchestra(snapshot.orchestra);
    promptPanel.beginCsoundCompileWatch();
}

std::vector<PresetFormat::SnapshotState> MainPanel::buildSnapshotsForSave() const
{
    std::vector<PresetFormat::SnapshotState> out;
    out.reserve(kNumSnapshotSlots);
    for (int i = 0; i < kNumSnapshotSlots; ++i)
    {
        const auto& src = mainSnapshots[static_cast<size_t>(i)];
        if (!src.valid) continue;
        if (src.audio.getNumSamples() <= 0 || src.audio.getNumChannels() <= 0) continue;

        PresetFormat::SnapshotState dst;
        dst.slot           = i;
        dst.valid          = true;
        dst.audio.makeCopyOf(src.audio);
        dst.sampleRate     = src.sampleRate;
        dst.promptA        = src.promptA;
        dst.promptB        = src.promptB;
        dst.device         = src.device;
        dst.model          = src.model;
        dst.injectionMode  = src.injectionMode;
        dst.seed           = src.seed;
        dst.randomSeed     = src.randomSeed;
        dst.lateMixAmount  = std::isnan(src.lateMixAmount) ? 0.75f : src.lateMixAmount;
        dst.splitStart     = std::isnan(src.splitStart)    ? 4.0f  : src.splitStart;
        dst.splitEnd       = std::isnan(src.splitEnd)      ? 16.0f : src.splitEnd;

        for (int a = 0; a < 3; ++a)
        {
            dst.axes[static_cast<size_t>(a)].dropdownId =
                src.axes[static_cast<size_t>(a)].dropdownId;
            dst.axes[static_cast<size_t>(a)].value =
                src.axes[static_cast<size_t>(a)].value;
        }

        dst.embeddingA       = src.embeddingA;
        dst.embeddingB       = src.embeddingB;
        dst.dimensionOffsets = src.dimensionOffsets;

        if (src.parameters.isValid())
            dst.parametersXml = src.parameters.toXmlString();

        dst.loopStart      = src.loopStart;
        dst.loopEnd        = src.loopEnd;
        dst.startPos       = src.startPos;
        dst.wtExtractStart = src.wtExtractStart;
        dst.wtExtractEnd   = src.wtExtractEnd;
        dst.pointsLocked   = src.pointsLocked;

        out.push_back(std::move(dst));
    }
    return out;
}

void MainPanel::applySnapshotsFromLoad(const std::vector<PresetFormat::SnapshotState>& snapshots,
                                       int calibEpoch)
{
    // Clear all session snapshots first; only the slots present in the
    // preset are populated. Slot index from JSON is authoritative; values
    // outside [0, kNumSnapshotSlots) are ignored defensively.
    for (auto& s : mainSnapshots) s = {};
    // The LCO slots go too, and for a sharper reason than symmetry: a preset
    // brings its own orchestra, and a slot left over from before the load still
    // reads as filled while holding the PREVIOUS session's orchestra and its
    // whole parameter set. Pressing it would bury the just-loaded preset under a
    // sound the user did not load. (Preset files carry no LCO slots of their own
    // yet, so there is nothing to repopulate them with — they simply start empty
    // with the preset.)
    for (auto& s : lcoSnapshots)     s = {};
    for (auto& s : lcoPressCaptures) s = {};

    for (const auto& src : snapshots)
    {
        if (!src.valid) continue;
        if (src.slot < 0 || src.slot >= kNumSnapshotSlots) continue;

        MainSnapshot dst;
        dst.valid       = true;
        dst.audio.makeCopyOf(src.audio);
        dst.sampleRate  = src.sampleRate;
        dst.promptA     = src.promptA;
        dst.promptB     = src.promptB;
        dst.device      = src.device;
        dst.model       = src.model;
        dst.injectionMode = src.injectionMode;
        dst.seed        = src.seed;
        dst.randomSeed  = src.randomSeed;
        dst.lateMixAmount = src.lateMixAmount;
        dst.splitStart  = src.splitStart;
        dst.splitEnd    = src.splitEnd;

        for (int a = 0; a < 3; ++a)
        {
            dst.axes[static_cast<size_t>(a)].dropdownId =
                src.axes[static_cast<size_t>(a)].dropdownId;
            dst.axes[static_cast<size_t>(a)].value =
                src.axes[static_cast<size_t>(a)].value;
        }

        dst.embeddingA       = src.embeddingA;
        dst.embeddingB       = src.embeddingB;
        dst.dimensionOffsets = src.dimensionOffsets;

        if (src.parametersXml.isNotEmpty())
        {
            if (auto xml = juce::parseXML(src.parametersXml))
            {
                dst.parameters = juce::ValueTree::fromXml(*xml);
                // Snapshot param values were stored under the file's calibration
                // epoch; rescale so recalling this slot sounds identical post-update.
                Calibration::migrateValueTree(dst.parameters, calibEpoch);
            }
        }

        dst.loopStart      = src.loopStart;
        dst.loopEnd        = src.loopEnd;
        dst.startPos       = src.startPos;
        dst.wtExtractStart = src.wtExtractStart;
        dst.wtExtractEnd   = src.wtExtractEnd;
        dst.pointsLocked   = src.pointsLocked;

        mainSnapshots[static_cast<size_t>(src.slot)] = std::move(dst);
    }
    syncSnapshotUi();
}

void MainPanel::captureSnapshotPress(int slot)
{
    if (slot < 1 || slot > kNumSnapshotSlots)
        return;
    // Both paradigms pre-capture at press time, so a store always keeps the state
    // that was sounding when the finger went down — a bake or a drift regen that
    // completes during the long-press must not end up in the slot instead.
    if (!oscEasyMode)
    {
        lcoPressCaptures[static_cast<size_t>(slot - 1)] = captureLcoSnapshot();
        return;
    }
    snapshotPressCaptures[static_cast<size_t>(slot - 1)] = captureMainSnapshot();
}

void MainPanel::storeSnapshotFromPress(int slot)
{
    if (slot < 1 || slot > kNumSnapshotSlots)
        return;

    if (!oscEasyMode)
    {
        // LCO: store the authored orchestra + its sound parameters (and, before a
        // first bake, the prompt alone). The status names which of the two the slot
        // actually holds — "saved" without that distinction would let a prompt-only
        // slot pass for a stored sound.
        auto& pendingLco = lcoPressCaptures[static_cast<size_t>(slot - 1)];
        if (!pendingLco.valid)
        {
            statusBar.setStatusText("No LRO sound or prompt to snapshot");
            return;
        }
        const bool hasSound = pendingLco.orchestra.isNotEmpty();
        lcoSnapshots[static_cast<size_t>(slot - 1)] = std::move(pendingLco);
        pendingLco = {};
        activeSnapshotIndex = slot;
        syncSnapshotUi();
        snapshotButtons[slot].flashStored();
        statusBar.setStatusText(hasSound
            ? "LRO snapshot " + juce::String(slot) + " saved (orchestra + sound)"
            : "LRO prompt " + juce::String(slot) + " saved (no orchestra yet)");
        return;
    }

    auto& pending = snapshotPressCaptures[static_cast<size_t>(slot - 1)];
    if (!pending.valid)
    {
        statusBar.setStatusText("No audio to snapshot");
        return;
    }

    mainSnapshots[static_cast<size_t>(slot - 1)] = std::move(pending);
    pending = {};
    activeSnapshotIndex = slot;
    syncSnapshotUi();
    snapshotButtons[slot].flashStored();
    statusBar.setStatusText("Snapshot " + juce::String(slot) + " saved");
}

void MainPanel::activateSnapshot(int slot)
{
    if (slot <= 0)
    {
        activeSnapshotIndex = 0;
        syncSnapshotUi();
        return;
    }

    if (!oscEasyMode)
    {
        // LCO: bring the stored orchestra back — it compiles and sounds, no LLM
        // pass, no re-authoring. A slot stored before any bake still only parks the
        // prompt (the user bakes via GENERATE when ready).
        if (slot > kNumSnapshotSlots || !lcoSnapshots[static_cast<size_t>(slot - 1)].valid)
        {
            statusBar.setStatusText("LRO snapshot " + juce::String(slot) + " empty");
            syncSnapshotUi();
            return;
        }
        const auto& lcoSnap = lcoSnapshots[static_cast<size_t>(slot - 1)];
        const bool hasSound = lcoSnap.orchestra.isNotEmpty();
        if (hasSound && lcoBakeBusy_)
        {
            // The running bake publishes its orchestra when it lands; handing this
            // one over now would sound for a few seconds and then be replaced by a
            // sound the user did not ask for. Say so instead of half-doing it. A
            // prompt-only slot is unaffected — it touches no sound at all.
            statusBar.setStatusText(juce::String::fromUTF8(
                "LRO is authoring \xe2\x80\x94 recall when the bake lands"));
            syncSnapshotUi();
            return;
        }
        restoreLcoSnapshot(lcoSnap);
        activeSnapshotIndex = slot;
        syncSnapshotUi();
        statusBar.setStatusText(hasSound
            ? "LRO snapshot " + juce::String(slot) + " recalled"
            : "LRO prompt " + juce::String(slot) + " recalled (no orchestra stored)");
        return;
    }

    if (slot > kNumSnapshotSlots || !mainSnapshots[static_cast<size_t>(slot - 1)].valid)
    {
        statusBar.setStatusText("Snapshot " + juce::String(slot) + " empty");
        syncSnapshotUi();
        return;
    }

    restoreMainSnapshot(mainSnapshots[static_cast<size_t>(slot - 1)]);
    activeSnapshotIndex = slot;
    syncSnapshotUi();
    statusBar.setStatusText("Snapshot " + juce::String(slot) + " recalled");
}

void MainPanel::syncSnapshotUi()
{
    for (int i = 0; i < kNumSnapshotButtons; ++i)
    {
        snapshotButtons[i].setToggleState(activeSnapshotIndex == i, juce::dontSendNotification);
        // Filled dot reflects the store for the CURRENT mode: neural audio snapshots
        // vs LCO orchestras.
        const bool filled = i > 0 && (oscEasyMode
            ? mainSnapshots[static_cast<size_t>(i - 1)].valid
            : lcoSnapshots[static_cast<size_t>(i - 1)].valid);
        snapshotButtons[i].setSnapshotFilled(filled);
    }
}

void MainPanel::triggerMainGeneration()
{
    // Central trigger for EVERY caller (on-screen GENERATE button, the Cmd/Shift+
    // Return shortcut, XL Generate CC 37 via onGenerateRequested). In LCO the SAME
    // button is reused to author the language oscillator, so route to the LCO action
    // here — one entry point keeps button + shortcut + MIDI in agreement (paradigm
    // boundary, BlockParams.h). triggerLcoGenerate owns its own busy/Qwen gates.
    // The LCO's own drop-to-Manual escape hatch lives INSIDE triggerLcoGenerate, not
    // here: the prompt editor's Return key calls it directly (PromptPanel.cpp ~390),
    // and Cmd/Shift+Return deliberately refuses in Advanced — so this is not the only
    // manual route on that side, unlike the neural one below.
    if (!oscEasyMode)
    {
        promptPanel.triggerLcoGenerate();
        return;
    }

    if (!mainGenerateBtn.isEnabled())
        return;

    // Generate is an explicit manual action: drop the regen loop OUT of any auto/bar
    // cadence so a deliberate Generate isn't immediately overwritten by the auto-loop.
    // This lives in the shared path so it applies to BOTH the on-screen Generate button
    // and the XL Generate button (CC 37). The REGENERATE switchbox is attached to
    // drift_regen (SynthPanel hidden combo → radio buttons), so the UI follows. The
    // auto-loop reads the driftRegenMode atomic mirror and early-outs on Manual, so this
    // cleanly stops auto-regen without touching the loop's own generation path.
    if (auto* p = processorRef.getValueTreeState().getParameter(PID::driftRegen))
        p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(DriftRegen::Manual)));

    activeSnapshotIndex = 0;
    syncSnapshotUi();
    promptPanel.setSemanticAxes(axesPanel.getAxisValues());
    promptPanel.triggerGenerationWithOffsets({});
}

void MainPanel::setComputerKeyboardEnabled(bool enabled)
{
    if (computerKeyboardEnabled == enabled)
    {
        statusBar.setKeyboardInputEnabled(enabled);
        return;
    }

    computerKeyboardEnabled = enabled;
    statusBar.setKeyboardInputEnabled(enabled);
    if (!enabled)
        releaseComputerKeyboardNotes();
    statusBar.setStatusText(enabled ? computerKeyboardStatusText()
                                    : "Computer keyboard off");
}

// Returns true if the offset actually moved — the caller needs to know, because a
// shift silences the held notes and only a real shift may bring them back.
bool MainPanel::shiftComputerKeyboardOctave(int delta)
{
    const int nextOffset = juce::jlimit(kComputerKeyboardMinOctaveOffset,
                                       kComputerKeyboardMaxOctaveOffset,
                                       computerKeyboardOctaveOffset + delta);
    if (nextOffset == computerKeyboardOctaveOffset)
    {
        statusBar.setStatusText("Kbd octave limit: " + computerKeyboardBaseNoteName());
        return false;
    }

    releaseComputerKeyboardNotes();
    computerKeyboardOctaveOffset = nextOffset;
    statusBar.setStatusText("Kbd octave: " + computerKeyboardBaseNoteName());
    return true;
}

bool MainPanel::isTextEditingFocus() const
{
    for (auto* c = juce::Component::getCurrentlyFocusedComponent(); c != nullptr; c = c->getParentComponent())
        if (dynamic_cast<juce::TextEditor*>(c) != nullptr)
            return true;
    return false;
}

// Reconcile note/octave state from the physical keyboard.
//   allowStart=true  (keyPressed, focus-scoped): acts on strikes — begin or
//   retrigger notes, shift the octave.
//   allowStart=false (poll): release-only. It must not begin anything, because
//   inside a DAW the key stream this reads is the whole HOST's, so acting on a
//   strike here would play what the user typed at the host.
// Returns true if any mapped key is held, or if a note key was just struck.
bool MainPanel::scanComputerKeyboard(bool allowStart)
{
    static_assert (static_cast<int>(sizeof(kComputerKeyboardNoteKeys) / sizeof(kComputerKeyboardNoteKeys[0]))
                       == kComputerKeyboardKeyCount,
                   "kComputerKeyboardNoteKeys length must equal kComputerKeyboardKeyCount "
                   "(computerKeyboardNotesDown/ActiveNotes are sized by it).");
    bool anyDown = false;

    // Two different questions, deliberately answered by two different things. What
    // BEGINS anything is a STRIKE — an actual key-down event that no pass has acted
    // on yet. What ENDS it is the held-state. A key that reads "down" while nobody
    // is touching it can then only delay a release; it can never start a note, and
    // this is exactly what a stuck key used to do: one keycode wrongly reading down
    // (0x00, physical A) started a C on every OTHER key's press, hung, and made A
    // itself dead. Do not "simplify" this back into one predicate.
    // One strike, one octave shift. Where a strike is a real key event that needs no
    // edge flag at all — a flag can only re-arm from a pass, so two quick taps became
    // one shift; a strike is already one-per-keystroke, which is what the flag was
    // imitating. The flags below therefore exist ONLY for the platforms where a
    // strike is not an event.
    //
    // Summed to ONE shift, never applied twice: both octave keys can have a strike
    // waiting when a pass finally runs — pressed under the replay overlay, or with a
    // key-up lost — and two calls would release and re-attack every held note twice
    // for a net offset change that may even be zero.
    // Where a strike is a real key event it is already one-per-keystroke and stands
    // alone. Where it is only "the key reads down" it repeats while held, and the
    // edge has to come from this editor's own flag — which is also why the flag is
    // still here and not in PhysicalKeyState: a shared one would let a second plugin
    // editor swallow this one's keystrokes. Cost of the polled edge, unchanged from
    // before: it can only re-arm from a pass, so two taps inside one 20 Hz tick are
    // one shift.
    const bool eventStrikes = t5::physicalKeyStrikesAreEvents();
    const bool octaveDownHeld = t5::physicalKeyDown(kComputerKeyboardOctaveDownKey);
    const bool octaveUpHeld   = t5::physicalKeyDown(kComputerKeyboardOctaveUpKey);

    int octaveDelta = 0;
    if (allowStart && t5::physicalKeyWasStruck(kComputerKeyboardOctaveDownKey)
        && (eventStrikes || !computerKeyboardOctaveDownHeld_))
        --octaveDelta;
    if (allowStart && t5::physicalKeyWasStruck(kComputerKeyboardOctaveUpKey)
        && (eventStrikes || !computerKeyboardOctaveUpHeld_))
        ++octaveDelta;
    // Cleared by any pass that sees the key up. Armed by a pass that could have ACTED
    // on it — and by the release-only poll only once the key has read down on TWO
    // successive poll passes. Both halves of that are needed. On Windows the key
    // state is GetAsyncKeyState, which goes true at the hardware event, while JUCE
    // delivers its timer tick as a POSTED message that Win32 hands over ahead of
    // queued key input: a single tick landing in that gap would mark the press as
    // already handled and swallow the octave keystroke. But never arming from the
    // poll is just as wrong — a key held through a state where the poll runs and
    // keyPressed does not (the replay overlay, a modal popup, a Ctrl chord) would
    // then shift the octave on the next plain keystroke, unasked. The gap lasts one
    // dispatch; a key genuinely held lasts ticks, so two ticks tell them apart.
    const bool pollArmsDown = octaveDownHeld && computerKeyboardOctaveDownPolled_;
    const bool pollArmsUp   = octaveUpHeld   && computerKeyboardOctaveUpPolled_;
    if (!allowStart)
    {
        computerKeyboardOctaveDownPolled_ = octaveDownHeld;
        computerKeyboardOctaveUpPolled_   = octaveUpHeld;
    }
    computerKeyboardOctaveDownHeld_ = octaveDownHeld
        && (allowStart || pollArmsDown || computerKeyboardOctaveDownHeld_);
    computerKeyboardOctaveUpHeld_ = octaveUpHeld
        && (allowStart || pollArmsUp || computerKeyboardOctaveUpHeld_);

    // Keys that were sounding when the octave moved and are still held follow it to
    // the new offset. Decided here rather than inside the shift so that a key which
    // is ALSO struck in this same pass is begun exactly once, not twice.
    std::array<bool, kComputerKeyboardKeyCount> resume {};
    if (octaveDelta != 0)
    {
        const auto wasDown = computerKeyboardNotesDown;
        if (shiftComputerKeyboardOctave(octaveDelta))
            resume = wasDown;
    }
    // octaveDelta too: a tap short enough that the key already reads up still shifted,
    // and the press must be consumed rather than fall through to a host shortcut.
    anyDown |= octaveDownHeld || octaveUpHeld || octaveDelta != 0;

    for (int i = 0; i < kComputerKeyboardKeyCount; ++i)
    {
        const bool down = t5::physicalKeyDown(kComputerKeyboardNoteKeys[i]);
        const bool struck = allowStart
                         && t5::physicalKeyWasStruck(kComputerKeyboardNoteKeys[i]);
        anyDown |= down || struck;

        auto& isDown = computerKeyboardNotesDown[static_cast<size_t>(i)];
        auto& active = computerKeyboardActiveNotes[static_cast<size_t>(i)];

        // The octave moved under a key that is still held: the same note continues at
        // a new pitch. Not a keystroke, so step-record must not see it.
        const bool resumes = resume[static_cast<size_t>(i)] && down;

        // A strike on a note that is STILL sounding means the key was let go and hit
        // again inside one poll tick — the release itself was never seen, because
        // only the poll reconciles it. Where a strike is a real key event that is
        // unambiguous (auto-repeat carries none), so the second attack is played
        // rather than swallowed; repeated notes at any speed the player can reach are
        // what this exists for. Where a strike is only "the key reads down" the same
        // test would fire again on every keystroke while the key is held, so there
        // the note's own state supplies the edge and a re-strike inside one tick
        // stays swallowed, exactly as before.
        const bool strikes = struck && (eventStrikes || !isDown);

        if (strikes || resumes)
        {
            if (isDown && active >= 0)
                processorRef.endComputerKeyboardNote(active);

            isDown = true;
            active = computerKeyboardNoteForIndex(i);
            // Step-record hears a keystroke only where one can be told apart from the
            // octave carrying a held note across. Where a strike is a real key event
            // it can: a key struck in the same pass as an octave shift really was
            // struck, and belongs in the pattern. Where it is only "the key reads
            // down" the two are indistinguishable, so the resume wins and nothing is
            // recorded — a missed step is recoverable, a pattern filling itself with
            // notes nobody played is not.
            processorRef.beginComputerKeyboardNote(active, 0.82f,
                                                   strikes && (eventStrikes || !resumes));
        }
        else if (!down && isDown)
        {
            isDown = false;
            if (active >= 0)
                processorRef.endComputerKeyboardNote(active);
            active = -1;
        }
    }

    // Every pass drains, the release-only poll included, so a strike is acted on at
    // most once and never lingers. It cannot eat a strike keyPressed still needs:
    // the key event, the map update and keyPressed all happen in one dispatch, with
    // no chance for the timer to run in between. What it does sweep is a strike no
    // pass could act on — a note key pressed under a Ctrl chord, or while the
    // keyboard was off — which otherwise sits waiting to become an attack nobody
    // played — a strike whose key-up was lost, or one left by the replay overlay,
    // which is the one gated path that does not reach the guard's own discard.
    // (A no-op where a strike is not an event: there it holds no state, and the edge
    // is the note's own sounding-state plus the octave flags above.)
    t5::drainPhysicalKeyStrikes();
    return anyDown;
}

int MainPanel::computerKeyboardNoteForIndex(int keyIndex) const
{
    return kComputerKeyboardBaseMidiNote + computerKeyboardOctaveOffset * 12 + keyIndex;
}

juce::String MainPanel::computerKeyboardBaseNoteName() const
{
    return juce::MidiMessage::getMidiNoteName(computerKeyboardNoteForIndex(0), true, true, 4);
}

juce::String MainPanel::computerKeyboardStatusText() const
{
    // Layout-agnostic: keys map by physical position, so name rows, not glyphs.
    return "Kbd on: home row = white keys, row above = black, bottom-left = octave -/+, from "
           + computerKeyboardBaseNoteName();
}

void MainPanel::pollComputerKeyboard()
{
    // Re-arm the Space-rest edge once the key physically lifts. Runs every tick,
    // independent of piano mode (Space-rest is gated only on step-record). Global
    // read — worst case is a missed rest while another app holds space.
    if (! juce::KeyPress::isKeyCurrentlyDown(juce::KeyPress::spaceKey))
        spaceRestKeyDown_ = false;

    if (!computerKeyboardEnabled || isTextEditingFocus()
        || settingsVisible || manualVisible || presetManagerVisible || seqLibraryVisible
        || !juce::Process::isForegroundProcess())
    {
        releaseComputerKeyboardNotes();
        // Strikes are dropped here too — these are exactly the states in which no
        // pass runs, so a keystroke that arrived while the keyboard was off or an
        // overlay was up would otherwise sit waiting and become an attack nobody
        // played on the way back. Safe against a second plugin editor doing it 20
        // times a second: a strike is consumed inside the same event dispatch that
        // created it, with no chance for a timer to run in between.
        //
        // The HELD map is process-wide and stays untouched, precisely because it is
        // not per-editor: an instance sitting here with its typing keyboard off must
        // not wipe the key state the instance you are playing reads from. Its own
        // forgetting (app inactive, Command held, menu tracking) lives in
        // PhysicalKeyStateMac.mm.
        t5::discardPhysicalKeyStrikes();
        // The octave edge is this editor's own, so the guard has to carry it: set to
        // what the keys read NOW, so a key held through this state is not a fresh
        // press on the way out, and one released during it can be pressed again
        // immediately. (Unused where a strike is a real key event.)
        computerKeyboardOctaveDownHeld_ = t5::physicalKeyDown(kComputerKeyboardOctaveDownKey);
        computerKeyboardOctaveUpHeld_   = t5::physicalKeyDown(kComputerKeyboardOctaveUpKey);
        return;
    }

    // Release-only reconcile (allowStart=false): note-ON is the focus-scoped
    // keyPressed path. The poll MUST NOT begin notes — inside a DAW the key map is
    // fed by the whole HOST's key events, so starting here would pick up typing
    // aimed at the host. Intended consequence where a strike is a real key event: a
    // note key still physically held across a focus-loss/disable is released by the
    // guard above and stays silent until re-pressed. (Where it is not, "held" is all
    // there is, so the next plain keystroke restarts it — that is what the polled
    // platforms have always done.) Do NOT "fix" this by starting notes here: the poll
    // reads the host's key stream, and that reopens the leak.
    scanComputerKeyboard(false);
}

void MainPanel::releaseComputerKeyboardNotes()
{
    for (int i = 0; i < static_cast<int>(computerKeyboardNotesDown.size()); ++i)
    {
        if (!computerKeyboardNotesDown[static_cast<size_t>(i)])
            continue;
        computerKeyboardNotesDown[static_cast<size_t>(i)] = false;
        const int note = computerKeyboardActiveNotes[static_cast<size_t>(i)];
        processorRef.endComputerKeyboardNote(note >= 0 ? note : computerKeyboardNoteForIndex(i));
        computerKeyboardActiveNotes[static_cast<size_t>(i)] = -1;
    }
}

void MainPanel::timerCallback()
{
    // (Event Log draining is NOT here — the EventLogWriterThread owns its ingress
    // FIFOs and drains itself, so recording never depends on this editor being open.)

    // Replay overlay readouts (playhead, feed, prompts). refresh() repaints only
    // when something visible changed, so this costs nothing per tick when idle.
    if (replayOverlay_.isVisible())
        replayOverlay_.refresh();

    // Surface a background update-check result (if any) once, non-blocking —
    // does not touch model loading/PipeInference at all. Chrome/VS Code pattern:
    // an accent dot on Settings; the Download button lives in General Settings.
    {
        juce::String updVer, updUrl;
        if (processorRef.takeAvailableUpdate(updVer, updUrl))
        {
            pendingUpdateTabJump_ = true;
            statusBar.setUpdateBadge(true, updVer);
            generalSettingsPage.setUpdateAvailable(updVer, updUrl);
        }
    }

    // Stop the temporary pulse after a cache hit, but keep the cache-hit
    // label as long as the cache is still full.
    if (cacheHitActive)
    {
        const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (nowSec >= cacheHitUntilSec)
        {
            cacheHitActive = false;
            if (! promptPanel.isGenerating())
                glowGenerating = false;
        }
    }

    const bool isAuto = processorRef.driftRegenMode.load() != 0;
    const bool isHover = mainGenerateBtn.isMouseOver(false);
    float speedHz;
    if (glowGenerating)               speedHz = 1.00f;
    else if (isAuto && isHover)       speedHz = 1.00f;
    else if (isAuto)                  speedHz = 0.75f;
    else if (isHover)                 speedHz = 0.50f;
    else                              speedHz = 0.05f;   // manual idle (slow drift)

    const double nowSec = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (glowLastTimeSec <= 0.0)
        glowLastTimeSec = nowSec;
    const float dt = static_cast<float>(juce::jmin(0.5, nowSec - glowLastTimeSec));
    glowLastTimeSec = nowSec;

    glowPhase += speedHz * juce::MathConstants<float>::twoPi * dt;
    while (glowPhase > juce::MathConstants<float>::twoPi)
        glowPhase -= juce::MathConstants<float>::twoPi;

    oscModePulsePhase += 0.33f * juce::MathConstants<float>::twoPi * dt;
    while (oscModePulsePhase > juce::MathConstants<float>::twoPi)
        oscModePulsePhase -= juce::MathConstants<float>::twoPi;
    updateOscModeToggleVisual();

    mainGenerateBtn.setAnimationState(glowPhase, glowGenerating);
    updateGenerateButtonsForCacheState(false);
    syncInferenceCacheUi();

    // Live-refresh the Resynth "ext" source button: it needs an available input
    // bus, which can appear or vanish (the user picks an input device in the
    // standalone Audio settings) WITHOUT a model change, so onModelChanged alone
    // would leave it stale. Change-guarded: the uncontended mutex read is cheap and
    // setEnabled only repaints when the state actually flips, so idle costs nothing.
    // Neural-only: in LCO the RESYNTH int/ext toggle is force-disabled+dimmed
    // (setOscEasyMode); this live-refresh must not re-enable "ext" behind that.
    if (oscEasyMode)
    {
        const bool extOk = promptPanel.selectedModelIsSA3()
                        && processorRef.hasExternalInputAvailable();
        if (resynthSrcBtns[1].isEnabled() != extOk)
        {
            resynthSrcBtns[1].setEnabled(extOk);
            resynthSrcBtns[1].setAlpha(extOk ? 1.0f : 0.4f);
        }
    }
    pollComputerKeyboard();

    // Drive the cache-button pulse phase while the cache is still filling.
    // Uses an independent 2 Hz counter (not glowPhase, which crawls at idle)
    // so the selected button visibly blinks. Only the toggled button repaints.
    if (oscEasyMode && lastInfCacheUiCapacity > 0 && lastInfCacheUiFill < lastInfCacheUiCapacity)
    {
        cachePulsePhase += 2.0f * juce::MathConstants<float>::twoPi * dt;
        while (cachePulsePhase > juce::MathConstants<float>::twoPi)
            cachePulsePhase -= juce::MathConstants<float>::twoPi;
        for (auto& b : infCacheButtons)
            b.setPulsePhase(cachePulsePhase);
    }

    // Poll drift ghost offsets for AxesPanel (30Hz)
    auto& mv = processorRef.modulatedValues;
    axesPanel.setGhostOffsets(
        mv.driftAxis1.load(std::memory_order_relaxed),
        mv.driftAxis2.load(std::memory_order_relaxed),
        mv.driftAxis3.load(std::memory_order_relaxed));

    // Resynth drift ghost: the SliderRow paints + smooths it natively (setGhostValue
    // + tickGhost, like the FX rows). driftResynth is NaN whenever Drift is not
    // targeting Resynth → clearGhost, so at idle tickGhost early-returns (no repaint).
    // Suppress on a disabled (non-SA3) row so a greyed control sprouts no orphan ghost.
    if (resynthRow->isEnabled())
        resynthRow->setGhostValue(mv.driftResynth.load(std::memory_order_relaxed));
    else
        resynthRow->clearGhost();
    resynthRow->tickGhost();

    if (pendingInferenceReload && !promptPanel.isGenerating())
    {
        pendingInferenceReload = false;
        tryLoadInferenceModels(true);
    }

    // Poll MIDI Clock state (30 Hz — no need for a separate timer)
    statusBar.setMidiClockState(processorRef.isMidiClockEnabled(),
                                processorRef.isMidiClockActive(),
                                processorRef.getMidiClockBpm());
}

void MainPanel::resized()
{
    auto b = getLocalBounds();
    float w = static_cast<float>(b.getWidth());
    float h = static_cast<float>(b.getHeight());

    int statusH = 20;
    int footerH = juce::jlimit(160, 280, juce::roundToInt(h * 0.24f));
    statusBar.setBounds(b.removeFromBottom(statusH));

    // Replay overlay covers everything ABOVE the status bar: the bar stays live as
    // the info/error channel (generation status) and keeps Panic reachable.
    replayOverlay_.setBounds(getLocalBounds().withTrimmedBottom(statusH));

    // Gap between footer and main content
    b.removeFromBottom(6);

    // Footer
    auto footer = b.removeFromBottom(footerH);
    int volW = juce::jlimit(40, 60, juce::roundToInt(w * 0.05f));
    auto volArea = footer.removeFromRight(volW);
    masterVolLabel.setFont(juce::FontOptions(kUiLabelFontMin));
    masterVolLabel.setBounds(volArea.removeFromTop(14));
    masterVolKnob.setBounds(volArea);
    int footerGap = juce::jlimit(4, 8, juce::roundToInt(w * 0.005f));
    footer.removeFromRight(footerGap);  // gap Vol–FX

    const int footerContentW = footer.getWidth() - footerGap;
    const int fxPrefW = juce::jlimit(180, 400, juce::roundToInt(w * 0.28f));
    const int fxMinW = 160;
    const int seqMinW = 360;

    int fxW = juce::jmin(fxPrefW, juce::jmax(fxMinW, footerContentW - seqMinW));
    int seqW = footerContentW - fxW;
    if (seqW < seqMinW)
    {
        fxW = juce::jmax(120, footerContentW - seqMinW);
        seqW = footerContentW - fxW;
    }

    seqW = juce::jmax(280, seqW);
    fxW = juce::jmax(120, footerContentW - seqW);

    fxPanel.setBounds(footer.removeFromRight(fxW));
    footer.removeFromRight(footerGap);  // gap FX–Seq
    sequencerPanel.setBounds(footer.removeFromRight(seqW));

    // ═══ Col 1: Three cards — OSCILLATOR, AXES, DIM EXPLORER ═══
    int col1W = juce::jlimit(240, 420, juce::roundToInt(w * 0.25f));
    auto genCol = b.removeFromLeft(col1W).reduced(6, 2);
    // Guarantee breathing room between this column's lowest control (RESYNTH) and
    // the SEQUENCER header below: reserve a bottom gap up-front so neither the
    // DimExplorer nor the centered Generate block can grow flush against the footer.
    genCol.removeFromBottom(juce::jlimit(10, 16, juce::roundToInt(h * 0.015f)));

    int headerH = juce::jlimit(14, 20, juce::roundToInt(h * 0.022f));
    int kGap = juce::jlimit(3, 6, juce::roundToInt(h * 0.005f));
    constexpr int kMinOscH = 220;
    constexpr int kMinAxesH = 84;
    constexpr int kMinGenerateButtonH = 38;
    const int cacheRowH = juce::jlimit(16, 20, juce::roundToInt(h * 0.022f));
    // Resynth is a single [left-title | slider] row, same height as the snap/cache
    // row right above it (no separate header strip → frees that vertical space).
    const int resynthBlockH  = cacheRowH;
    const int genCacheGap = juce::jlimit(22, 36, juce::roundToInt(h * 0.032f));

    int genBtnH = juce::jlimit(50, 72,
                               juce::roundToInt(juce::jmax(static_cast<float>(genCol.getWidth()) * 0.18f,
                                                           h * 0.060f)));

    int oscH = juce::jmax(kMinOscH, promptPanel.getPreferredHeightForWidth(genCol.getWidth()));
    int axesH = juce::jlimit(kMinAxesH, 144, juce::roundToInt(h * 0.133f));
    const int reservedGenerateBlockH = kMinGenerateButtonH + genCacheGap + cacheRowH + kGap + resynthBlockH;
    // Two headers (osc + the axes|dim segment switch) and ONE shared fixed-height
    // box (axesH) hosting either the Axes controls or the DimExplorer mini-view —
    // same band height in every segment and mode, so the Generate block never
    // shifts. If the column is too short, trim the box (to kMinAxesH) then oscH.
    const int headerCount = 2;
    int bandBudget = genCol.getHeight() - (headerH * headerCount + kGap * headerCount
                                           + reservedGenerateBlockH + oscH + axesH);
    if (bandBudget < 0)
    {
        int shortage = -bandBudget;
        const int trimAxes = juce::jmin(shortage, juce::jmax(0, axesH - kMinAxesH));
        axesH -= trimAxes;
        shortage -= trimAxes;
        if (shortage > 0)
            oscH = juce::jmax(kMinOscH, oscH - shortage);
    }

    // Card 1: OSCILLATOR
    auto oscHeaderBounds = genCol.removeFromTop(headerH);
    oscHeader.setBounds(oscHeaderBounds);
    // Font is fit-to-width below: "Language-Controlled Oscillator" (LCO title)
    // is longer than "T5 OSCILLATOR" and must not clip against the mode toggle.
    // toggleW is sized off the UNFIT base font (matches the original behaviour —
    // the toggle button's own size doesn't depend on how much the title shrinks).
    float oscHeaderFs = static_cast<float>(headerH) * 0.85f;
    const int toggleW = juce::jlimit(58, 78,
        measureTextWidth(oscModeToggle.getButtonText(), juce::jmax(kUiControlFontMin, oscHeaderFs * 0.72f)) + 16);
    const int availTitleW = juce::jmax(1, oscHeaderBounds.getWidth() - toggleW - 10);
    const float measuredW = measureTextWidth(oscHeader.getText(), oscHeaderFs);
    if (measuredW > availTitleW && measuredW > 0.0f)
        oscHeaderFs = juce::jmax(kUiControlFontMin, oscHeaderFs * availTitleW / measuredW);
    oscHeader.setFont(juce::FontOptions(oscHeaderFs));
    oscModeToggle.setBounds(oscHeaderBounds.removeFromRight(toggleW).reduced(2, 2));
    const int titleW = measureTextWidth(oscHeader.getText(), oscHeaderFs) + 8;
    auto poweredBounds = oscHeader.getBounds();
    poweredBounds.removeFromLeft(juce::jmin(titleW, poweredBounds.getWidth()));
    poweredBounds.removeFromRight(toggleW + 4);
    poweredByLabel.setFont(juce::FontOptions(juce::jmax(kUiLabelFontMin,
                                                        static_cast<float>(headerH) * 0.6f)));
    poweredByLabel.setBounds(poweredBounds);
    // LCO (Advanced) reuses the neural column skeleton: the prompt block absorbs
    // everything ABOVE the reserved GENERATE control block (GENERATE + snap/cache +
    // resynth), which the shared block further down lays out in the SAME place as
    // neural — RUHE on switch. No axes segment/box in LCO. CACHE/RESYNTH are shown
    // disabled by setOscEasyMode; the block is still laid out so the skeleton — and
    // the switchbox-border sentinels — stay identical across the mode switch.
    if (!oscEasyMode)
    {
        const int lcoReservedH = genBtnH + genCacheGap + cacheRowH + kGap + resynthBlockH;
        // Transfer T5osc's GENERATE breathing room EXACTLY. Neural fills the column
        // with oscH (prompt) + kGap + headerH (axes segment) + axesH (axes card) +
        // kGap, and the shared block below CENTERS GENERATE in whatever genCol is
        // left → real air above the button. LCO has no axes card, so if the prompt
        // panel eats the whole column (the old `genCol.getHeight() - lcoReservedH -
        // kGap`) then remainH == controlsH and the centering slack is ZERO — GENERATE
        // glued to the panel. Give the LCO prompt panel the SAME combined height as
        // neural's prompt+axes region instead, so the leftover handed to the shared
        // block is IDENTICAL to neural and GENERATE floats with the same gap. Capped
        // at the fill-everything max so a tiny host window never pushes GENERATE off.
        const int lcoPromptPreferred = oscH + headerH + axesH + kGap;
        const int lcoPromptMax = juce::jmax(kMinOscH, genCol.getHeight() - lcoReservedH - kGap);
        const int lcoPromptH   = juce::jlimit(kMinOscH, lcoPromptMax, lcoPromptPreferred);
        promptPanel.setBounds(genCol.removeFromTop(lcoPromptH));
        genCol.removeFromTop(kGap);
    }
    else
    {

        promptPanel.setBounds(genCol.removeFromTop(oscH));
        genCol.removeFromTop(kGap);

        // Card 2: SEMANTIC AXES | DIM EXPLORER — a 2-segment switch over one shared
        // fixed-height box (axesH). The active segment shows the Axes controls or the
        // DimExplorer mini-view; the box height is constant across segment AND osc
        // mode, so the Generate block below never shifts. (Visibility of axesPanel vs
        // dimensionExplorer is driven by updateAxesDimSegment, not here.)
        {
            auto switchBar = genCol.removeFromTop(headerH);
            const int segW = switchBar.getWidth() / 2;
            axesDimSegBtns[0].setBounds(switchBar.removeFromLeft(segW));
            axesDimSegBtns[1].setBounds(switchBar);

            auto boxArea = genCol.removeFromTop(axesH);
            axesPanel.setBounds(boxArea);
            if (!dimExplorerVisible)
                dimensionExplorer.setBounds(boxArea);   // overlay path repositions it below
            genCol.removeFromTop(kGap);
        }
    }   // end neural-only axes segment; the GENERATE block below is shared

    // ── Shared GENERATE control block (BOTH modes) ──────────────────────────────
    // GENERATE + snap/cache + resynth, laid out in the remaining genCol. In LCO the
    // CACHE/RESYNTH rows are disabled (setOscEasyMode) but still positioned here so
    // the column skeleton is identical across the mode switch; GENERATE is reused
    // for the LCO bake (MainPanel::triggerMainGeneration routes to the LCO action).
    {
        // Generate + InfCache controls get all slack freed by the explorer cap,
        // centered in the remaining card area so the controls have breathing room.
        // genCacheGap is intentionally larger than kGap so Generate doesn't read
        // as glued to the cache row — separation here marks Generate as a
        // standalone primary control, not a label for the cache row.
        int remainH = genCol.getHeight();
        int effectiveGenCacheGap = genCacheGap;
        if (remainH < kMinGenerateButtonH + effectiveGenCacheGap + cacheRowH + kGap + resynthBlockH)
            effectiveGenCacheGap = juce::jmin(effectiveGenCacheGap, kGap);

        // The resynth row sits below the snap/cache row, so it is part of the centered
        // control block: reserve its height (+ a gap) here so the Generate button
        // doesn't claim it and the row stays inside genCol instead of colliding with
        // the sequencer below.
        const int availableGenButtonH = juce::jmax(0, remainH - effectiveGenCacheGap - cacheRowH - kGap - resynthBlockH);
        genBtnH = juce::jlimit(0, genBtnH, availableGenButtonH);
        const int controlsH = genBtnH + effectiveGenCacheGap + cacheRowH + kGap + resynthBlockH;
        int genBtnY = genCol.getY() + juce::jmax(0, (remainH - controlsH) / 2);
        auto genBtnArea = juce::Rectangle<int>(genCol.getX(), genBtnY, genCol.getWidth(), genBtnH);
        int genW = juce::roundToInt(static_cast<float>(genBtnArea.getWidth()) * 0.66f);
        int genX = genBtnArea.getX() + (genBtnArea.getWidth() - genW) / 2;
        mainGenerateBtn.setBounds(genX, genBtnArea.getY(), genW, genBtnArea.getHeight());

        auto snapCacheRow = juce::Rectangle<int>(genCol.getX(),
                                                 mainGenerateBtn.getBottom() + effectiveGenCacheGap,
                                                 genCol.getWidth(),
                                                 cacheRowH).reduced(1, 0);
        const float switchFs = juce::jmax(kUiLabelFontMin, static_cast<float>(cacheRowH) * 0.58f);
        // Left-header titles: ModuleTitle, bold (matches RE-PROMPT/VARIATION).
        setUiFont(snapLabel, TextRole::ModuleTitle, switchFs, true);
        setUiFont(cacheLabel, TextRole::ModuleTitle, switchFs, true);

        const bool veryNarrow = snapCacheRow.getWidth() < 260;
        const int gap = veryNarrow ? 3 : 4;
        // Band width = the (space-prefixed) title + a little right breathing room.
        const float hdrFs = uiFontSize(TextRole::ModuleTitle, switchFs);
        const int labelPad = veryNarrow ? 4 : 7;
        const int snapLabelW = measureTextWidth(" SNAP", hdrFs) + labelPad;
        const int cacheLabelW = measureTextWidth(" CACHE", hdrFs) + labelPad;
        const int snapGroupW = veryNarrow ? 94 : juce::jlimit(108, 128,
                                                              juce::roundToInt(static_cast<float>(snapCacheRow.getWidth()) * 0.28f));

        snapLabel.setBounds(snapCacheRow.removeFromLeft(snapLabelW));
        auto snapGroup = snapCacheRow.removeFromLeft(juce::jmin(snapGroupW, snapCacheRow.getWidth()));
        snapCacheRow.removeFromLeft(juce::jmin(gap, snapCacheRow.getWidth()));
        cacheLabel.setBounds(snapCacheRow.removeFromLeft(juce::jmin(cacheLabelW, snapCacheRow.getWidth())));
        snapCacheRow.removeFromLeft(juce::jmin(gap, snapCacheRow.getWidth()));

        auto layoutWeightedButtons = [](auto& buttons, int count, juce::Rectangle<int> area, const float* weights)
        {
            float remainingWeight = 0.0f;
            for (int i = 0; i < count; ++i)
                remainingWeight += weights[i];

            for (int i = 0; i < count; ++i)
            {
                const int cellW = (i == count - 1)
                    ? area.getWidth()
                    : juce::jmax(1, juce::roundToInt(static_cast<float>(area.getWidth()) * weights[i] / remainingWeight));
                buttons[i].setBounds(area.removeFromLeft(cellW));
                remainingWeight -= weights[i];
            }
        };

        static constexpr float snapshotWeights[kNumSnapshotButtons] = {
            1.65f, 1.00f, 1.00f, 1.00f, 1.00f
        };
        layoutWeightedButtons(snapshotButtons, kNumSnapshotButtons, snapGroup, snapshotWeights);
        snapshotSwitchBounds = snapshotButtons[0].getBounds();
        for (int i = 1; i < kNumSnapshotButtons; ++i)
            snapshotSwitchBounds = snapshotSwitchBounds.getUnion(snapshotButtons[i].getBounds());

        auto cacheGroup = snapCacheRow;
        // What the two dropped depths bought: the word-shaped cells (OFF, 16, and
        // the take switch) get the room the digits do not need, so the row reads at
        // the minimum window width instead of ellipsising three of its six cells.
        static constexpr float cacheWeights[kNumInfCacheButtons] = {
            2.30f, 0.85f, 0.85f, 0.85f, 1.60f
        };
        // The take switch takes the right end of the row, with a hairline gap: it is
        // read as its own control, not as a sixth depth in the connected group.
        {
            constexpr float kAsyncWeight = 2.30f;
            float capWeight = 0.0f;
            for (float w : cacheWeights) capWeight += w;
            const int asyncW = juce::jmax(1, juce::roundToInt(
                static_cast<float>(cacheGroup.getWidth()) * kAsyncWeight / (capWeight + kAsyncWeight)));
            cacheAsyncBtn.setBounds(cacheGroup.removeFromRight(asyncW).withTrimmedLeft(4));
        }
        layoutWeightedButtons(infCacheButtons, kNumInfCacheButtons, cacheGroup, cacheWeights);
        cacheSwitchBounds = infCacheButtons[0].getBounds();
        for (int i = 1; i < kNumInfCacheButtons; ++i)
            cacheSwitchBounds = cacheSwitchBounds.getUnion(infCacheButtons[i].getBounds());
        cacheSwitchBounds = cacheSwitchBounds.getUnion(cacheAsyncBtn.getBounds());

        // Resynth row beneath the snap/cache row: a "RESYNTH" left-title band + the
        // Off→Full slider to its right — the snap/cache treatment, just with a slider
        // (single-row module → left-header). Y derived like snapCacheRow (from the
        // Generate button's bottom) so it tracks the centered control block exactly;
        // resynthBlockH is reserved above.
        auto resynthArea = juce::Rectangle<int>(
            genCol.getX(),
            mainGenerateBtn.getBottom() + effectiveGenCacheGap + cacheRowH + kGap,
            genCol.getWidth(),
            resynthBlockH).reduced(1, 0);
        auto srcToggle = resynthArea.removeFromRight(56);     // 2× ~28px int/ext buttons
        resynthArea.removeFromRight(juce::jmin(gap, resynthArea.getWidth()));
        resynthRow->setBounds(resynthArea);                   // inline SliderRow draws its own band label + value
        for (int i = 0; i < 2; ++i)
            resynthSrcBtns[i].setBounds(srcToggle.removeFromLeft(srcToggle.getWidth() / (2 - i)));
    }   // end shared GENERATE control block

    // (The Re-Prompt control row that used to sit beneath Resynth now lives in
    // PromptPanel, directly under the prompts.)

    // Col 2: ENGINE
    synthPanel.setBounds(b);



    // Scrims cover everything
    dimScrim.setBounds(getLocalBounds());
    settingsScrim.setBounds(getLocalBounds());
    presetScrim.setBounds(getLocalBounds());
    manualScrim.setBounds(getLocalBounds());
    seqLibraryScrim.setBounds(getLocalBounds());

    if (presetManagerVisible)
    {
        int panelW = juce::jlimit(720, 1100, juce::roundToInt(w * 0.78f));
        int panelH = juce::jlimit(440, 720, juce::roundToInt(h * 0.78f));
        presetManager.setBounds((getWidth() - panelW) / 2,
                                (getHeight() - panelH) / 2,
                                panelW,
                                panelH);
    }
    else
    {
        presetManager.setBounds({});
    }

    if (seqLibraryVisible)
    {
        int panelW = juce::jlimit(420, 560, juce::roundToInt(w * 0.40f));
        int panelH = juce::jlimit(360, 520, juce::roundToInt(h * 0.62f));
        seqLibrary.setBounds((getWidth() - panelW) / 2,
                             (getHeight() - panelH) / 2,
                             panelW,
                             panelH);
    }
    else
    {
        seqLibrary.setBounds({});
    }

    // Manual overlay (centered). Leaves a strip at the bottom of the
    // panel for the close button; the WebBrowserComponent fills the rest.
    if (manualVisible)
    {
        int manW = juce::jlimit(720, 1100, juce::roundToInt(w * 0.8f));
        int manH = juce::jlimit(480, 820, juce::roundToInt(h * 0.85f));
        int mx = (getWidth() - manW) / 2;
        int my = (getHeight() - manH) / 2;
        manualPanel.setBounds(mx, my, manW, manH);

        auto inner = manualPanel.getLocalBounds().reduced(8);
        auto btnRow = inner.removeFromBottom(30);
        inner.removeFromBottom(6);
        manualWeb.setBounds(inner);
        manualCloseBtn.setBounds(btnRow.removeFromRight(90));
    }
    else
    {
        manualPanel.setBounds({});
        manualWeb.setBounds(-10000, -10000, 1, 1);
    }

    // Settings overlay (bottom-right, above StatusBar)
    if (settingsVisible)
    {
        int settingsW = juce::jlimit(400, 600, juce::roundToInt(w * 0.4f));
        int settingsH = juce::jlimit(300, 500, juce::roundToInt(h * 0.55f));
        int sx = getWidth() - settingsW - 20;
        int sy = getHeight() - statusH - settingsH - 30;
        settingsTabs.setBounds(sx, sy, settingsW, settingsH);
    }

    // DimExplorer overlay
    if (dimExplorerVisible)
    {
        auto overlayBounds = getLocalBounds().reduced(40);
        int btnH = 30;
        int applyW = 180;
        int smallW = 70;
        int resetW = 140;
        int btnGap = 10;

        auto btnArea = overlayBounds.removeFromBottom(btnH + 10);
        int totalBtnW = applyW + smallW * 2 + resetW + btnGap * 3;
        int startX = btnArea.getCentreX() - totalBtnW / 2;
        int y = btnArea.getY();

        dimApplyBtn.setBounds(startX, y, applyW, btnH);
        dimUndoBtn.setBounds(startX + applyW + btnGap, y, smallW, btnH);
        dimRedoBtn.setBounds(startX + applyW + smallW + btnGap * 2, y, smallW, btnH);
        dimResetBtn.setBounds(startX + applyW + smallW * 2 + btnGap * 3, y, resetW, btnH);

        dimensionExplorer.setBounds(overlayBounds.reduced(20, 10));
    }
}

// ═══════════════════════════════════════════════════════════════════
// Default / Init state
// ═══════════════════════════════════════════════════════════════════

void MainPanel::loadDefaultPreset()
{
    // Only load if no audio present (fresh launch, not DAW session restore)
    if (processorRef.getGeneratedAudio().getNumSamples() > 0)
        return;

    // Standalone: restore previous session state if available
    if (juce::JUCEApplicationBase::isStandaloneApp())
    {
        auto bufFile = getBufferPresetFile();
        if (bufFile.existsAsFile())
        {
            auto result = PresetFormat::loadFromFile(bufFile, processorRef);
            if (result.success)
            {
                applyLoadedPreset(result, bufFile);
                return;
            }
        }
    }

    // First launch or DAW: use the same clean state as the Init button.
    loadInitPreset();
}

void MainPanel::loadInitPreset()
{
    for (auto* param : processorRef.getParameters())
        if (param != nullptr)
            param->setValueNotifyingHost(param->getDefaultValue());

    // Init is a clean MODERN preset: reset the modality epoch to current so a sound
    // built right after loading a legacy preset routes under the v2.5.0 behaviour.
    // (The epoch is processor state, not an APVTS param, so the reset loop above and
    // the Init defaults don't touch it.)
    processorRef.setModalityEpoch(T5ynthProcessor::kModalityEpoch);

    juce::AudioBuffer<float> emptyAudio;
    const double sampleRate = processorRef.getSampleRate() > 0.0
        ? processorRef.getSampleRate()
        : 44100.0;
    processorRef.loadGeneratedAudio(emptyAudio, sampleRate);
    processorRef.setInferenceCacheCapacity(0);
    processorRef.setLastPrompts({}, {});
    processorRef.setLastPresetName({});
    processorRef.setLastTags({});
    processorRef.setLastEmbeddings({}, {});
    processorRef.setLastSeed(static_cast<int>(
        processorRef.getValueTreeState().getRawParameterValue(PID::genSeed)->load()));
    processorRef.setLastInjection("linear", 0.75f, 4.0f, 16.0f);
    processorRef.setLastAxes({});

    promptPanel.loadPresetData({}, {}, processorRef.getLastSeed(), false,
                               {}, {}, "linear", 0.75f, 4.0f, 16.0f);
    axesPanel.setSlotStates({});
    dimensionExplorer.clear();

    currentPresetFile = juce::File();
    statusBar.setPresetName({});
    statusBar.setStatusText("Initialized");
    presetManager.setCurrentPreset(currentPresetFile, {});
    syncInferenceCacheUi();
}

// ═══════════════════════════════════════════════════════════════════
// WAV Export
// ═══════════════════════════════════════════════════════════════════

void MainPanel::exportWav()
{
    // Single Export entry-point that branches on engine mode:
    //   Sampler   → 24-bit PCM .wav of the loaded audio buffer.
    //   Wavetable → Serum-format .wav (32-bit float, mono, frames
    //               concatenated, with `clm ` chunk for frame size).
    const bool isWavetable = processorRef.isWavetableMode();

    if (isWavetable)
    {
        if (!processorRef.getMasterOscConst().hasFrames())
        {
            statusBar.setStatusText("No wavetable to export");
            return;
        }
    }
    else
    {
        if (processorRef.getGeneratedAudio().getNumSamples() == 0)
        {
            statusBar.setStatusText("No audio to export");
            return;
        }
    }

    // Propose a filename (Desktop / <preset-or-prompt>.wav) so the save dialog
    // isn't an empty field the user must fill from scratch.
    auto chooser = std::make_shared<juce::FileChooser>(
        isWavetable ? "Export Wavetable" : "Export WAV",
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory)
            .getChildFile(suggestedExportBaseName() + ".wav"),
        "*.wav");

    juce::Component::SafePointer<MainPanel> safeThis(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectFiles
                         | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis, chooser, isWavetable](const juce::FileChooser& fc)
        {
            if (!safeThis) return;
            auto* self = safeThis.getComponent();
            auto file = fc.getResult();
            if (file == juce::File()) return;

            // String-concat (not withFileExtension) — the user-typed name
            // may contain dots (e.g. "Pad 0.5"); withFileExtension would
            // truncate everything after the last dot.
            if (!file.hasFileExtension("wav"))
                file = file.getParentDirectory().getChildFile(file.getFileName() + ".wav");

            double sr = self->processorRef.getGeneratedSampleRate();
            if (sr <= 0.0) sr = 44100.0;

            if (isWavetable)
            {
                std::vector<float> samples;
                int frameSize = 0, numFrames = 0;
                if (! self->processorRef.getMasterOscConst()
                        .snapshotLevel0Frames(samples, frameSize, numFrames))
                {
                    self->statusBar.setStatusText("Wavetable export failed: no frames");
                    return;
                }
                if (writeWavetableWav(file, samples, frameSize, numFrames, sr))
                    self->statusBar.setStatusText("Exported wavetable: "
                                                  + file.getFileName()
                                                  + "  (" + juce::String(numFrames) + " frames)");
                else
                    self->statusBar.setStatusText("Wavetable export failed");
                return;
            }

            // Sampler mode — write the loaded audio buffer as 24-bit PCM.
            const auto& buf = self->processorRef.getGeneratedAudio();
            auto outStream = file.createOutputStream();
            if (!outStream) { self->statusBar.setStatusText("Export failed"); return; }

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer(
                wav.createWriterFor(outStream.release(), sr,
                                    static_cast<unsigned int>(buf.getNumChannels()),
                                    24, {}, 0));
            if (writer)
            {
                writer->writeFromAudioSampleBuffer(buf, 0, buf.getNumSamples());
                self->statusBar.setStatusText("Exported: " + file.getFileName());
            }
            else
                self->statusBar.setStatusText("Export failed");
        });
}

// A human-friendly default filename for the WAV export dialog: the preset name
// if the user has one, else the A prompt, sanitised to a legal filename and
// length-capped. Falls back to the app name so the field is never empty.
juce::String MainPanel::suggestedExportBaseName() const
{
    juce::String base = processorRef.getLastPresetName().trim();
    if (base.isEmpty() || base == "T5ynth Export" || base == "Init")
        base = processorRef.getLastPromptA();
    base = base.replaceCharacters("\r\n\t", "   ").trim();
    if (base.length() > 48)
        base = base.substring(0, 48).trim();
    base = juce::File::createLegalFileName(base);
    return base.isNotEmpty() ? base : juce::String(kProductNameAscii);
}

// Copy the current session's .t5evt (the continuously-recorded event log) to a
// user-chosen location. The live file keeps recording; this exports a snapshot
// of everything up to the last flush. Loading/replaying one needs the replay
// Player, which is a separate future piece — this is the "save" half only.
void MainPanel::loadReplaySession()
{
    auto chooser = std::make_shared<juce::FileChooser>(
        "Play Session Log",
        processorRef.getEventLogCurrentFile().existsAsFile()
            ? processorRef.getEventLogCurrentFile().getParentDirectory()
            : juce::File::getSpecialLocation(juce::File::userDesktopDirectory),
        "*.t5evt");

    juce::Component::SafePointer<MainPanel> safeThis(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                         | juce::FileBrowserComponent::canSelectFiles,
        [safeThis, chooser](const juce::FileChooser& fc)
        {
            if (! safeThis) return;
            auto* self = safeThis.getComponent();
            const auto file = fc.getResult();
            if (file == juce::File() || ! file.existsAsFile()) return;

            EventLogReader reader;
            if (! reader.loadFile(file))
            {
                self->statusBar.setStatusText("Could not read session log: " + reader.getErrorMessage());
                return;
            }
            // startReplay() refuses a tape whose start patch is missing (pre-R0
            // formatVersion 1) or undecodable — replaying one against whatever is
            // loaded now would play the right notes with the wrong sound. It snapshots
            // the current patch and restores it on Stop, so this is non-destructive.
            if (! self->processorRef.startReplay(reader))
            {
                self->statusBar.setStatusText(juce::String::fromUTF8(
                    "Session log has no usable start patch \xe2\x80\x94 cannot replay"));
                return;
            }
            self->statusBar.setStatusText("Replaying " + file.getFileName());
            self->replayOverlay_.begin(file.getFileName());
        });
}

void MainPanel::saveSessionLog()
{
    const auto src = processorRef.getEventLogCurrentFile();
    if (! src.existsAsFile())
    {
        statusBar.setStatusText(juce::String::fromUTF8(
            "No session log yet \xe2\x80\x94 enable \"Record Event Log\" in Settings"));
        return;
    }

    auto chooser = std::make_shared<juce::FileChooser>(
        "Save Session Log",
        juce::File::getSpecialLocation(juce::File::userDesktopDirectory).getChildFile(src.getFileName()),
        "*.t5evt");

    juce::Component::SafePointer<MainPanel> safeThis(this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                         | juce::FileBrowserComponent::canSelectFiles
                         | juce::FileBrowserComponent::warnAboutOverwriting,
        [safeThis, chooser, src](const juce::FileChooser& fc)
        {
            if (! safeThis) return;
            auto* self = safeThis.getComponent();
            auto dest = fc.getResult();
            if (dest == juce::File()) return;

            if (! dest.hasFileExtension("t5evt"))
                dest = dest.getParentDirectory().getChildFile(dest.getFileName() + ".t5evt");

            // Re-check existence at write time; recording may have produced the
            // file between the menu opening and the chooser returning.
            if (! src.existsAsFile())
            {
                self->statusBar.setStatusText("Session log no longer available");
                return;
            }
            if (src.copyFileTo(dest))
                self->statusBar.setStatusText("Saved session log: " + dest.getFileName());
            else
                self->statusBar.setStatusText("Session log save failed");
        });
}

// ═══════════════════════════════════════════════════════════════════
// Manual — native WebView renders the bundled HTML guide.
// The HTML is compiled into the plugin as BinaryData and extracted
// once per app-session to a temp file so the WKWebView / WebView2 /
// WebKitGTK backend has a stable file:// URL to load. Anchor links
// (#setup, #gen, …) work natively; external https:// links launch
// in the user's default browser.
// ═══════════════════════════════════════════════════════════════════

void MainPanel::showManual()
{
    manualVisible = true;
    manualScrim.setVisible(true);
    manualScrim.toFront(false);
    manualPanel.setVisible(true);
    manualPanel.toFront(false);
#if JUCE_LINUX
    if (manualWeb.getParentComponent() != &manualPanel)
        manualPanel.addAndMakeVisible(manualWeb);
#endif
    manualWeb.setVisible(true);

    if (!manualLoaded)
    {
        // Extract the bundled HTML to a temp file once per session.
        manualHtmlOnDisk = juce::File::getSpecialLocation(juce::File::tempDirectory)
                               .getChildFile("akroasys_Guide.html");
        manualHtmlOnDisk.replaceWithData(BinaryData::akroasys_Guide_html,
                                         static_cast<size_t>(BinaryData::akroasys_Guide_htmlSize));

        manualWeb.goToURL(juce::URL(manualHtmlOnDisk).toString(false));
        manualLoaded = true;
    }

    resized();
}

void MainPanel::hideManual()
{
    manualVisible = false;
    manualScrim.setVisible(false);
    manualPanel.setVisible(false);
    manualWeb.setVisible(false);
    manualWeb.setBounds(-10000, -10000, 1, 1);
#if JUCE_LINUX
    // Linux WebKit child windows can leak through hidden parents; detach the
    // native view while the overlay is closed to avoid the white artefact.
    if (manualWeb.getParentComponent() == &manualPanel)
        manualPanel.removeChildComponent(&manualWeb);
#endif
}

// ═══════════════════════════════════════════════════════════════════
// Preset Save / Load
// ═══════════════════════════════════════════════════════════════════

void MainPanel::savePreset()
{
    // Always open the Library in Save mode. The drawer's conflict-aware
    // Save button is the only path that overwrites an existing preset, and
    // the user has to click the explicit red "Replace \"NAME\"" button to
    // confirm. There is no Undo in the synth, so silent overwrites of disk
    // state are not acceptable.
    enterLibrarySaveMode(SaveNameMode::keepName);
}

void MainPanel::loadPreset()
{
    showPresetManager();
}

void MainPanel::renameCurrentPreset()
{
    if (! currentPresetFile.existsAsFile()) return;
    if (presetManager.onRenameRequested) presetManager.onRenameRequested(currentPresetFile);
}

void MainPanel::deleteCurrentPreset()
{
    if (! currentPresetFile.existsAsFile()) return;
    if (presetManager.onDeleteRequested) presetManager.onDeleteRequested(currentPresetFile);
}

void MainPanel::showPresetNameContextMenu(juce::Point<int>)
{
    juce::PopupMenu menu;
    const bool havePreset = currentPresetFile.existsAsFile();
    menu.addItem(1, juce::String::fromUTF8("Rename\xe2\x80\xa6"), havePreset);
    menu.addItem(2, "Delete",                                     havePreset);
    menu.addSeparator();
    menu.addItem(3, "Reveal in file manager",                     havePreset);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&statusBar),
        [this](int result)
        {
            switch (result)
            {
                case 1: renameCurrentPreset(); break;
                case 2: deleteCurrentPreset(); break;
                case 3: if (currentPresetFile.existsAsFile()) currentPresetFile.revealToUser(); break;
                default: break;
            }
        });
}
