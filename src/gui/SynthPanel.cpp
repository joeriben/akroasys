#include "SynthPanel.h"
#include "../PluginProcessor.h"
#include "MidiLearnMenu.h"
#include <algorithm>
#include <cmath>

// ── Helper: format ms (integer) ──
static juce::String fmtMs(double v)
{
    if (std::abs(v) < 1.0)
        return juce::String(v, 2) + "ms";
    if (std::abs(v) < 10.0)
        return juce::String(v, 1) + "ms";
    return juce::String(juce::roundToInt(v)) + "ms";
}
static juce::String fmtF2(double v)  { return juce::String(v, 2); }
static juce::String fmtPct(double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; }
// Percent with one decimal below 10% — keeps the sub-1% range readable
// for skewed depth knobs (where pitch LFO is musically usable) without
// noise-precision at higher values.
static juce::String fmtPctFine(double v)
{
    double p = v * 100.0;
    return std::abs(p) < 10.0 ? juce::String(p, 1) + "%"
                              : juce::String(juce::roundToInt(p)) + "%";
}
static juce::String fmtHz(double v)  { return juce::String(juce::roundToInt(v)) + " Hz"; }
static juce::String fmtDb(double v)  { return juce::String(v, 1) + " dB"; }
static juce::String fmtHzF1(double v){ return juce::String(v, 1) + " Hz"; }
static juce::String fmtHzF2(double v){ return juce::String(v, 2) + " Hz"; }
static juce::String fmtHzF3(double v){ return juce::String(v, 3) + " Hz"; }
// Drift "Rate" in FREE mode reads as the cycle PERIOD (seconds) rather than a
// three-decimal sub-Hz value: 0.002 Hz → "500 s/cyc", 1 Hz → "1.0 s/cyc".
// Adaptive precision keeps it short. (Sync mode uses the musical-division label.)
static juce::String fmtPeriodSc(double hz)
{
    const double s = (hz > 1.0e-6) ? 1.0 / hz : 0.0;
    const juce::String num = (s >= 100.0) ? juce::String(juce::roundToInt(s))
                           : (s >= 1.0)   ? juce::String(s, 1)
                                          : juce::String(s, 2);
    return num + " s/cyc";
}

// ──────────────────────────────────────────────────────────────────────────────
// Envelope init
// ──────────────────────────────────────────────────────────────────────────────
void SynthPanel::initEnv(EnvSection& env, const juce::String& name, int defaultTarget,
                          const PID::ModEnvIds& ids, juce::AudioProcessorValueTreeState& apvts)
{
    juce::ignoreUnused(name);   // no longer shown: the advanced-view title was removed

    env.ids = ids;

    // Loop toggle — turns the env into a self-retriggering A→D→Hold→R cycle.
    // In loop mode the Sustain control becomes the per-cycle Hold time.
    env.loopBtn.setButtonText("LOOP");
    styleSwitchButton(env.loopBtn, kEnvCol);
    env.loopBtn.setClickingTogglesState(true);
    env.loopBtn.setTooltip("Loop the envelope as an A-D-Hold-R cycle. In loop mode Sustain sets the Hold time.");
    addAndMakeVisible(env.loopBtn);
    env.loopBtnA = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(apvts, ids.loop, env.loopBtn);

    // COPY / PASTE — move a whole envelope between the five tabs. Momentary,
    // so no setClickingTogglesState. PASTE is dead until something is copied,
    // which is also the only way to tell the two apart at a glance.
    env.copyBtn.setButtonText("COPY");
    styleSwitchButton(env.copyBtn, kEnvCol);
    env.copyBtn.setTooltip("Copy this envelope: every value, target included.");
    env.copyBtn.onClick = [this, &env] { copyEnvelope(env); };
    addAndMakeVisible(env.copyBtn);

    env.pasteBtn.setButtonText("PASTE");
    styleSwitchButton(env.pasteBtn, kEnvCol);
    env.pasteBtn.setTooltip("Paste the copied envelope onto this one.");
    env.pasteBtn.setEnabled(false);
    env.pasteBtn.onClick = [this, &env] { pasteEnvelope(env); };
    addAndMakeVisible(env.pasteBtn);

    // Labels driven from BlockParams::EnvTarget::kEntries (single source of
    // truth for enum index ↔ human-readable label).
    juce::StringArray envItems;
    for (const auto& e : EnvTarget::kEntries) envItems.add(e.label);
    env.targetBox.addItemList(envItems, 1);
    env.targetBox.setSelectedId(defaultTarget, juce::dontSendNotification);
    env.targetBox.onChange = [this] { updateVisibility(); resized(); };
    addAndMakeVisible(env.targetBox);

    // Easy-view "Target" left-header band (accent@0.7 + white, like SNAP/CACHE).
    paintSectionHeader(env.targetHeader, "Target", kEnvCol);
    addAndMakeVisible(env.targetHeader);

    // ── Easy-view "Velocity Amount" box (vertical drag-fill bars) ──
    // Att/Dec/Rel are bipolar (velocity→stage TIME, mirror the velSens above);
    // Level is unipolar (the GLOBAL velocity→peak amount, velAmt). Same widget
    // feel as the aftertouch bars, just vertical.
    env.attVB   = std::make_unique<VelocityBar>(true);
    env.decVB   = std::make_unique<VelocityBar>(true);
    env.relVB   = std::make_unique<VelocityBar>(true);
    env.levelVB = std::make_unique<VelocityBar>(false);
    env.attVB->setBarLabel("Att");
    env.decVB->setBarLabel("Dec");
    env.relVB->setBarLabel("Rel");
    env.levelVB->setBarLabel("Level");
    for (auto* vb : { env.attVB.get(), env.decVB.get(), env.relVB.get(), env.levelVB.get() })
        addAndMakeVisible(*vb);

    env.attVBA   = std::make_unique<SA>(apvts, ids.attackVelSens,  *env.attVB);
    env.decVBA   = std::make_unique<SA>(apvts, ids.decayVelSens,   *env.decVB);
    env.relVBA   = std::make_unique<SA>(apvts, ids.releaseVelSens, *env.relVB);
    env.levelVBA = std::make_unique<SA>(apvts, PID::velAmt, *env.levelVB);

    // Host automation gestures (the attachment can't, since we bypass the base
    // drag) + MIDI-learn parity with the advanced rows.
    auto wireVB = [this, &apvts](VelocityBar& vb, const juce::String& pid) {
        if (auto* p = apvts.getParameter(pid))
        {
            vb.onDragStart = [p] { p->beginChangeGesture(); };
            vb.onDragEnd   = [p] { p->endChangeGesture(); };
        }
        vb.onRightClick = [this, pid](juce::Point<int> pt) {
            showMidiLearnMenu(processorRef, pid, pt); };
    };
    wireVB(*env.attVB,   ids.attackVelSens);
    wireVB(*env.decVB,   ids.decayVelSens);
    wireVB(*env.relVB,   ids.releaseVelSens);
    wireVB(*env.levelVB, PID::velAmt);

    env.velBox.configure("VELOCITY AMOUNT", kEnvCol, Icon::numIcons);
    addAndMakeVisible(env.velBox);
    env.velBox.toBack();   // decorative frame — must sit behind the bars it frames

    // ── Graphical ADSR editor ──
    // Attaches DIRECTLY to the APVTS parameters (not to the faders), so APVTS is
    // the single source of truth and no hidden slider is load-bearing. fmtMs
    // formats A/D/R, fmtF2 formats Sustain/Amt — mirroring the fader read-outs.
    env.graph = std::make_unique<AdsrGraph>(kEnvCol);
    env.graph->bind(apvts, ids.attack, ids.decay, ids.sustain, ids.release, ids.amount,
                    ids.attackCurve, ids.decayCurve, ids.releaseCurve, fmtMs, fmtF2);
    addAndMakeVisible(*env.graph);
}

// ──────────────────────────────────────────────────────────────────────────────
// Envelope copy / paste
// ──────────────────────────────────────────────────────────────────────────────
void SynthPanel::copyEnvelope(const EnvSection& env)
{
    auto& apvts = processorRef.getValueTreeState();
    const auto ids = env.ids.all();
    for (size_t k = 0; k < ids.size(); ++k)
    {
        auto* prm = apvts.getParameter(ids[k]);
        envClipboard_[k] = prm != nullptr ? prm->convertFrom0to1(prm->getValue()) : 0.0f;
    }
    envClipboardFilled_ = true;
    envClipboardFromAmp_ = (&env == &ampEnv);
    for (int i = 0; i < PID::kNumEnvs; ++i)
        envSection(i).pasteBtn.setEnabled(true);
}

void SynthPanel::pasteEnvelope(const EnvSection& env)
{
    if (! envClipboardFilled_)
        return;

    // Real values, re-normalised against the DESTINATION's own range, so a copy
    // between envelopes whose ranges differ lands on the nearest legal value
    // instead of a rescaled one.
    // The TARGET travels between mod envelopes, which are interchangeable — but
    // never to or from ENV 1. ENV 1's target IS the DCA: pasting a filter
    // routing onto it takes the loudness envelope off every voice, and pasting
    // DCA off it puts a second envelope on the level (+6 dB). The player's
    // loudness is not part of what "copy the whole envelope" was asked for.
    const bool ampInvolved = (&env == &ampEnv) || envClipboardFromAmp_;
    auto& apvts = processorRef.getValueTreeState();
    const auto ids = env.ids.all();
    for (size_t k = 0; k < ids.size(); ++k)
        if (ampInvolved && ids[k] == env.ids.target)
            continue;
        else if (auto* prm = apvts.getParameter(ids[k]))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost(juce::jlimit(0.0f, 1.0f, prm->convertTo0to1(envClipboard_[k])));
            prm->endChangeGesture();
        }

    // The target may have moved — same follow-up the target box itself does.
    updateVisibility();
    resized();
}

// ──────────────────────────────────────────────────────────────────────────────
// LFO init
// ──────────────────────────────────────────────────────────────────────────────
void SynthPanel::initLfo(LfoSection& lfo, const juce::String& name,
                          const juce::String& rateId, const juce::String& depthId,
                          const juce::String& waveId, const juce::String& modeId,
                          const juce::String& clockModeId, const juce::String& divisionId,
                          juce::AudioProcessorValueTreeState& apvts)
{
    lfo.header.setText(name, juce::dontSendNotification);
    labelAsTitle(lfo.header, kLfoCol);
    addAndMakeVisible(lfo.header);

    // Labels driven from BlockParams::LfoTarget::kEntries (single source of
    // truth for enum index ↔ human-readable label).
    juce::StringArray lfoItems;
    for (const auto& e : LfoTarget::kEntries) lfoItems.add(e.label);
    lfo.targetBox.addItemList(lfoItems, 1);
    lfo.targetBox.setSelectedId(1, juce::dontSendNotification);
    lfo.targetBox.onChange = [this] { updateVisibility(); resized(); };
    addAndMakeVisible(lfo.targetBox);

    juce::StringArray lfoWaveItems;
    for (const auto& e : LfoWave::kEntries) lfoWaveItems.add(e.label);
    lfo.waveBox.addItemList(lfoWaveItems, 1);
    addAndMakeVisible(lfo.waveBox);
    for (int i = 0; i < kNumWaveBtns; ++i)
    {
        auto& btn = lfo.waveBtns[static_cast<size_t>(i)];
        btn.setButtonText(lfoWaveItems[i]);
        btn.setColour(juce::TextButton::buttonColourId, kSurface);
        btn.setColour(juce::TextButton::buttonOnColourId, kLfoCol);
        btn.setColour(juce::TextButton::textColourOffId, kDim);
        btn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        btn.setClickingTogglesState(false);
        btn.onClick = [&lfo, i] { lfo.waveBox.setSelectedId(i + 1); };
        addAndMakeVisible(btn);
    }

    // Free/Trig: hidden ComboBox holds the APVTS state; visible TextButton
    // cycles between F and T on click. Replaces the old inline "Free⌄"
    // dropdown which was visually too wide for the row.
    juce::StringArray lfoModeItems;
    for (const auto& e : LfoMode::kEntries) lfoModeItems.add(e.label);
    lfo.modeHidden.addItemList(lfoModeItems, 1);
    const juce::StringArray lfoModeBtnLabels { "Free", "Trig" };
    for (int i = 0; i < kNumLfoModeBtns; ++i)
    {
        auto& btn = lfo.modeBtns[static_cast<size_t>(i)];
        btn.setButtonText(lfoModeBtnLabels[i]);
        btn.setColour(juce::TextButton::buttonColourId, kSurface);
        btn.setColour(juce::TextButton::buttonOnColourId, kLfoCol);
        btn.setColour(juce::TextButton::textColourOffId, kDim);
        btn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        btn.setClickingTogglesState(false);
        btn.onClick = [&lfo, i] { lfo.modeHidden.setSelectedId(i + 1); };
        addAndMakeVisible(btn);
    }
    lfo.modeBtn.setLookAndFeel(&lfoModeLnf);   // padlock: open = Free, closed = Trig
    lfo.modeBtn.setTooltip("LFO phase: unlocked = free-running, locked = reset on each note-on");
    lfo.modeBtn.setClickingTogglesState(false);
    lfo.modeBtn.onClick = [&lfo] {
        const int cur = lfo.modeHidden.getSelectedId();
        lfo.modeHidden.setSelectedId(cur == 1 ? 2 : 1);
    };
    lfo.modeHidden.onChange = [&lfo] {
        const bool trig = lfo.modeHidden.getSelectedId() == 2;
        // Drive the padlock: locked (closed) = Trig, open = Free.
        lfo.modeBtn.setToggleState(trig, juce::dontSendNotification);
        lfo.modeBtn.repaint();
        for (int i = 0; i < kNumLfoModeBtns; ++i)
        {
            auto& btn = lfo.modeBtns[static_cast<size_t>(i)];
            const bool active = lfo.modeHidden.getSelectedId() == i + 1;
            btn.setToggleState(active, juce::dontSendNotification);
            btn.setColour(juce::TextButton::buttonColourId, active ? kLfoCol : kSurface);
            btn.setColour(juce::TextButton::textColourOffId, active ? juce::Colours::white : kDim);
        }
    };
    addAndMakeVisible(lfo.modeBtn);

    // BPM-sync clock: hidden ComboBox holds APVTS state, visible icon
    // button drives it. ClockMode change swaps which row is visible
    // (Hz rateRow vs musical-division divisionRow).
    juce::StringArray clockItems;
    for (const auto& e : ClockMode::kEntries) clockItems.add(e.label);
    lfo.clockModeHidden.addItemList(clockItems, 1);
    lfo.clockBtn.setButtonText("SYNC");
    lfo.clockBtn.setLookAndFeel(&lfoClockLnf);
    lfo.clockBtn.setClickingTogglesState(false);
    lfo.clockBtn.onClick = [&lfo] {
        const int cur = lfo.clockModeHidden.getSelectedId();
        lfo.clockModeHidden.setSelectedId(cur == 1 ? 2 : 1);
    };
    addAndMakeVisible(lfo.clockBtn);

    // Hz row + Division row occupy the same on-screen rect; visibility
    // swaps based on ClockMode (Off → rate, Sync → division).
    lfo.rateRow     = std::make_unique<SliderRow>("Rate", fmtHzF1, kLfoCol);
    lfo.depthRow    = std::make_unique<SliderRow>("Amt", fmtPctFine, kLfoCol);
    lfo.divisionRow = std::make_unique<SliderRow>("Rate",
        [](double v) {
            const int idx = juce::jlimit(0, ClockDivision::kCount - 1,
                                          juce::roundToInt(v));
            return juce::String(ClockDivision::kEntries[idx].label);
        }, kLfoCol);
    lfo.divisionRow->getSlider().setRange(
        0.0, static_cast<double>(ClockDivision::kCount - 1), 1.0);
    // Lock the value column to a fixed pixel width so the slider track's
    // RIGHT edge stays put when ClockMode swaps "1.5 Hz" ↔ "1/16T". The
    // label column is forced from SynthPanel::resized() to the
    // modulation-section-wide left column width — do NOT force it here.
    lfo.rateRow->setForcedValueWidth(56);
    lfo.divisionRow->setForcedValueWidth(56);
    addAndMakeVisible(*lfo.rateRow);
    addAndMakeVisible(*lfo.depthRow);
    addAndMakeVisible(*lfo.divisionRow);

    lfo.rateA      = std::make_unique<SA>(apvts, rateId,      lfo.rateRow->getSlider());
    lfo.depthA     = std::make_unique<SA>(apvts, depthId,     lfo.depthRow->getSlider());
    lfo.waveA      = std::make_unique<CA>(apvts, waveId,      lfo.waveBox);
    lfo.modeA      = std::make_unique<CA>(apvts, modeId,      lfo.modeHidden);
    lfo.divisionA  = std::make_unique<SA>(apvts, divisionId,  lfo.divisionRow->getSlider());
    lfo.clockModeA = std::make_unique<CA>(apvts, clockModeId, lfo.clockModeHidden);

    auto wireMidiRow = [this](SliderRow* row, const juce::String& pid) {
        row->onRightClick = [this, pid](juce::Point<int> pos) {
            showMidiLearnMenu(processorRef, pid, pos); };
    };
    wireMidiRow(lfo.rateRow.get(),     rateId);
    wireMidiRow(lfo.depthRow.get(),    depthId);
    wireMidiRow(lfo.divisionRow.get(), divisionId);

    lfo.waveBox.onChange = [&lfo] {
        const int selected = lfo.waveBox.getSelectedId();
        for (int i = 0; i < kNumWaveBtns; ++i)
        {
            auto& btn = lfo.waveBtns[static_cast<size_t>(i)];
            const bool active = selected == i + 1;
            btn.setToggleState(active, juce::dontSendNotification);
            btn.setColour(juce::TextButton::buttonColourId, active ? kLfoCol : kSurface);
            btn.setColour(juce::TextButton::textColourOffId, active ? juce::Colours::white : kDim);
        }
    };

    // The CA was attached above; its initial setSelectedId fired before
    // this lambda existed, so we set the lambda now and invoke it
    // explicitly below to sync the visible state.
    lfo.clockModeHidden.onChange = [&lfo] {
        const bool sync = lfo.clockModeHidden.getSelectedId() == 2;
        lfo.clockBtn.setToggleState(sync, juce::dontSendNotification);
        lfo.clockBtn.repaint();
        if (lfo.rateRow)     lfo.rateRow->setVisible(!sync);
        if (lfo.divisionRow) lfo.divisionRow->setVisible(sync);
    };
    // Sync initial state from the loaded APVTS values.
    lfo.modeHidden.onChange();
    lfo.clockModeHidden.onChange();
    lfo.waveBox.onChange();

    lfo.rateRow->updateValue();
    lfo.depthRow->updateValue();
    lfo.divisionRow->updateValue();
}

// ──────────────────────────────────────────────────────────────────────────────
// Drift init
// ──────────────────────────────────────────────────────────────────────────────
void SynthPanel::initDrift(DriftSection& drift, const juce::String& name,
                            const juce::String& rateId, const juce::String& depthId,
                            const juce::String& targetId, const juce::String& waveId,
                            const juce::String& clockModeId, const juce::String& divisionId,
                            juce::AudioProcessorValueTreeState& apvts)
{
    drift.header.setText(name, juce::dontSendNotification);
    labelAsTitle(drift.header, kDriftCol);
    addAndMakeVisible(drift.header);

    juce::StringArray driftTargetItems;
    for (const auto& e : DriftTarget::kEntries) driftTargetItems.add(e.label);
    drift.targetBox.addItemList(driftTargetItems, 1);
    drift.targetBox.onChange = [this] { updateVisibility(); resized(); };
    addAndMakeVisible(drift.targetBox);

    juce::StringArray driftWaveItems;
    for (const auto& e : DriftWave::kEntries) driftWaveItems.add(e.label);
    drift.waveBox.addItemList(driftWaveItems, 1);
    addAndMakeVisible(drift.waveBox);
    for (int i = 0; i < kNumWaveBtns; ++i)
    {
        auto& btn = drift.waveBtns[static_cast<size_t>(i)];
        btn.setButtonText(driftWaveItems[i]);
        btn.setColour(juce::TextButton::buttonColourId, kSurface);
        btn.setColour(juce::TextButton::buttonOnColourId, kDriftCol);
        btn.setColour(juce::TextButton::textColourOffId, kDim);
        btn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        btn.setClickingTogglesState(false);
        btn.onClick = [&drift, i] { drift.waveBox.setSelectedId(i + 1); };
        addAndMakeVisible(btn);
    }

    // BPM-sync clock — same pattern as LFO; Drift has no Free/Trig so no
    // mode button.
    juce::StringArray clockItems;
    for (const auto& e : ClockMode::kEntries) clockItems.add(e.label);
    drift.clockModeHidden.addItemList(clockItems, 1);
    drift.clockBtn.setButtonText("SYNC");
    drift.clockBtn.setLookAndFeel(&driftClockLnf);
    drift.clockBtn.setClickingTogglesState(false);
    drift.clockBtn.onClick = [&drift] {
        const int cur = drift.clockModeHidden.getSelectedId();
        drift.clockModeHidden.setSelectedId(cur == 1 ? 2 : 1);
    };
    addAndMakeVisible(drift.clockBtn);

    drift.rateRow     = std::make_unique<SliderRow>("Rate", fmtPeriodSc, kDriftCol);
    drift.depthRow    = std::make_unique<SliderRow>("Amt", fmtPctFine, kDriftCol);
    drift.divisionRow = std::make_unique<SliderRow>("Rate",
        [](double v) {
            const int idx = juce::jlimit(0, DriftDivision::kCount - 1,
                                          juce::roundToInt(v));
            return juce::String(DriftDivision::kEntries[idx].label);
        }, kDriftCol);
    drift.divisionRow->getSlider().setRange(
        0.0, static_cast<double>(DriftDivision::kCount - 1), 1.0);
    // Lock the value column to a fixed pixel width so the slider track's
    // RIGHT edge stays put when ClockMode swaps. Free mode now shows the period
    // (e.g. "128 s/cyc" at the floor, "0.50 s/cyc" fast), wider than "Hz", hence 84.
    // BOTH rows share the width so the track edge is stable across the swap. The
    // label column is forced from SynthPanel::resized() to the
    // modulation-section-wide left column width — do NOT force it here.
    drift.rateRow->setForcedValueWidth(84);
    drift.divisionRow->setForcedValueWidth(84);
    addAndMakeVisible(*drift.rateRow);
    addAndMakeVisible(*drift.depthRow);
    addAndMakeVisible(*drift.divisionRow);

    drift.rateA      = std::make_unique<SA>(apvts, rateId,      drift.rateRow->getSlider());
    drift.depthA     = std::make_unique<SA>(apvts, depthId,     drift.depthRow->getSlider());
    drift.targetA    = std::make_unique<CA>(apvts, targetId,    drift.targetBox);
    drift.waveA      = std::make_unique<CA>(apvts, waveId,      drift.waveBox);
    drift.divisionA  = std::make_unique<SA>(apvts, divisionId,  drift.divisionRow->getSlider());
    drift.clockModeA = std::make_unique<CA>(apvts, clockModeId, drift.clockModeHidden);

    auto wireMidiRow = [this](SliderRow* row, const juce::String& pid) {
        row->onRightClick = [this, pid](juce::Point<int> pos) {
            showMidiLearnMenu(processorRef, pid, pos); };
    };
    wireMidiRow(drift.rateRow.get(),     rateId);
    wireMidiRow(drift.depthRow.get(),    depthId);
    wireMidiRow(drift.divisionRow.get(), divisionId);

    drift.waveBox.onChange = [&drift] {
        const int selected = drift.waveBox.getSelectedId();
        for (int i = 0; i < kNumWaveBtns; ++i)
        {
            auto& btn = drift.waveBtns[static_cast<size_t>(i)];
            const bool active = selected == i + 1;
            btn.setToggleState(active, juce::dontSendNotification);
            btn.setColour(juce::TextButton::buttonColourId, active ? kDriftCol : kSurface);
            btn.setColour(juce::TextButton::textColourOffId, active ? juce::Colours::white : kDim);
        }
    };

    drift.clockModeHidden.onChange = [&drift] {
        const bool sync = drift.clockModeHidden.getSelectedId() == 2;
        drift.clockBtn.setToggleState(sync, juce::dontSendNotification);
        drift.clockBtn.repaint();
        if (drift.rateRow)     drift.rateRow->setVisible(!sync);
        if (drift.divisionRow) drift.divisionRow->setVisible(sync);
    };
    drift.clockModeHidden.onChange();
    drift.waveBox.onChange();

    drift.rateRow->updateValue();
    drift.depthRow->updateValue();
    drift.divisionRow->updateValue();
}

// ──────────────────────────────────────────────────────────────────────────────
// Constructor
// ──────────────────────────────────────────────────────────────────────────────
SynthPanel::SynthPanel(T5ynthProcessor& processor)
    : processorRef(processor)
{
    auto& apvts = processor.getValueTreeState();

    // ── Engine mode ──
    auto styleBtn = [](juce::TextButton& btn, bool on) {
        styleSwitchButton(btn, kAccent);
        btn.setClickingTogglesState(true);
        btn.setRadioGroupId(1001);
        btn.setToggleState(on, juce::dontSendNotification);
    };
    styleBtn(samplerBtn, true);
    styleBtn(wavetableBtn, false);
    styleBtn(freezeBtn, false);
    samplerBtn.setConnectedEdges(juce::Button::ConnectedOnRight);
    wavetableBtn.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    freezeBtn.setConnectedEdges(juce::Button::ConnectedOnLeft);
    addAndMakeVisible(samplerBtn);
    addAndMakeVisible(wavetableBtn);
    addAndMakeVisible(freezeBtn);

    juce::StringArray engineModeItems;
    for (const auto& e : EngineMode::kEntries) engineModeItems.add(e.label);
    engineModeHidden.addItemList(engineModeItems, 1);
    engineModeHidden.onChange = [this] {
        const int id = engineModeHidden.getSelectedId();
        // ComboBox ids are 1-based EngineMode indices+1. LCO (index 3, id 4) is
        // bake/restore-only — there's no 4th switch button — so it lights up
        // the SAME Wavetable button as plain Wavetable (an LCO bake IS a
        // wavetable, just with a distinct preset identity).
        bool isSampler = id == 1;
        bool isWavetable = id == 2 || id == EngineMode::Lco + 1;
        bool isFreeze = id == 3;
        // Csound (index 4, id 5) is a REAL 4th DSP path (Phase 1: enum/glide
        // plumbing only — no bridge yet, so selecting it renders silence). It
        // has no switch button of its own (D6: no new UI) — isCsound documents
        // and verifies that none of the three buttons above lights up for it,
        // rather than leaving that an implicit side effect of id exclusion.
        const bool isCsound = (id == EngineMode::Csound + 1);
        jassertquiet(!isCsound || (!isSampler && !isWavetable && !isFreeze));
        samplerBtn.setToggleState(isSampler, juce::dontSendNotification);
        wavetableBtn.setToggleState(isWavetable, juce::dontSendNotification);
        freezeBtn.setToggleState(isFreeze, juce::dontSendNotification);
        updateVisibility();
        resized();
    };
    samplerBtn.onClick = [this] { engineModeHidden.setSelectedId(1); };
    wavetableBtn.onClick = [this] { engineModeHidden.setSelectedId(2); };
    freezeBtn.onClick = [this] { engineModeHidden.setSelectedId(3); };
    engineModeA = std::make_unique<CA>(apvts, PID::engineMode, engineModeHidden);

    // ── Voice count switchbox ──
    {
        juce::StringArray vcLabels;
        for (const auto& e : VoiceCount::kEntries) vcLabels.add(e.label);
        voiceCountHidden.addItemList(vcLabels, 1);
        voiceCountHidden.onChange = [this] {
            int id = voiceCountHidden.getSelectedId();
            for (int i = 0; i < kNumVoiceBtns; ++i)
                voiceBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
        };
        for (int i = 0; i < kNumVoiceBtns; ++i)
        {
            voiceBtns[i].setButtonText(vcLabels[i]);
            styleSwitchButton(voiceBtns[i], kAccent);
            voiceBtns[i].setClickingTogglesState(true);
            voiceBtns[i].setRadioGroupId(1002);
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumVoiceBtns - 1) edges |= juce::Button::ConnectedOnRight;
            voiceBtns[i].setConnectedEdges(edges);
            voiceBtns[i].onClick = [this, i] { voiceCountHidden.setSelectedId(i + 1); };
            addAndMakeVisible(voiceBtns[i]);
        }
        voiceCountA = std::make_unique<CA>(apvts, PID::voiceCount, voiceCountHidden);
    }

    // ── Tuning ──
    {
        juce::StringArray tuningItems;
        for (const auto& e : TuningType::kEntries) tuningItems.add(e.label);
        tuningBox.addItemList(tuningItems, 1);
        tuningBox.setColour(juce::ComboBox::backgroundColourId, kSurface.brighter(0.04f));
        tuningBox.setColour(juce::ComboBox::textColourId, juce::Colour(0xffd7dde9));
        tuningBox.setColour(juce::ComboBox::outlineColourId, kBorder);
        addAndMakeVisible(tuningBox);
        tuningA = std::make_unique<CA>(apvts, PID::tuning, tuningBox);
    }

    addAndMakeVisible(waveformDisplay);

    // Wire bracket handles: both engines use the same P2/P3 loop semantics.
    waveformDisplay.onLoopRegionChanged = [this](float start, float end) {
        const juce::ScopedLock sl (processorRef.getCallbackLock());
        // As a PAIR — the display hands over both brackets, and setting them one
        // at a time clamps the first against the OTHER one's old value. Same
        // outcome for a single dragged handle, except when it is dragged to
        // within 1% of its partner: mouseDrag enforces the swap rule but not
        // the minimum width, so the pair arrives sub-minimum. setLoopRegion
        // then keeps the start and opens the END out to start + 1%, whichever
        // of the two the user was holding — so P3 sits up to 1% past where the
        // bracket is drawn until the next mirror. Measured, not reasoned.
        processorRef.getSampler().setLoopRegion(start, end);
        processorRef.getSampler().setPointsLocked(true);
        waveformDisplay.getLockButton().setLocked(true);
        if (processorRef.isWavetableMode())
            pendingWtReextract_ = true;
    };

    // P1 (start position) handle
    waveformDisplay.onStartPosChanged = [this](float pos) {
        const bool freezeMode = processorRef.isFreezeMode();
        {
            const juce::ScopedLock sl (processorRef.getCallbackLock());
            processorRef.getSampler().setStartPos(pos);
            processorRef.getSampler().setPointsLocked(true);
            waveformDisplay.getLockButton().setLocked(true);
            if (!freezeMode && processorRef.isWavetableMode())
                pendingWtReextract_ = true;
        }
        if (freezeMode)
            scanRow->getSlider().setValue(static_cast<double>(pos), juce::sendNotificationSync);
    };

    waveformDisplay.onMarkerDragFinished = [this]() {
        if (pendingWtReextract_ && processorRef.isWavetableMode())
            processorRef.reextractWavetable();
        pendingWtReextract_ = false;
    };

    // Lock button: toggles P1/P2/P3 preservation across Generate
    waveformDisplay.getLockButton().onToggled = [this](bool locked) {
        const juce::ScopedLock sl (processorRef.getCallbackLock());
        processorRef.getSampler().setPointsLocked(locked);
    };

    // Scan position: dragging in WaveformDisplay updates the APVTS slider
    waveformDisplay.onScanChanged = [this](float pos) {
        scanRow->getSlider().setValue(static_cast<double>(pos), juce::sendNotificationSync);
    };

    // ── Loop mode ──
    auto styleLoopBtn = [](juce::TextButton& btn) {
        styleSwitchButton(btn, kAccent);
        btn.setClickingTogglesState(true);
        btn.setRadioGroupId(1003);
    };
    styleLoopBtn(oneshotBtn);
    styleLoopBtn(loopModeBtn);
    styleLoopBtn(pingpongBtn);
    oneshotBtn.setConnectedEdges(juce::Button::ConnectedOnRight);
    loopModeBtn.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    pingpongBtn.setConnectedEdges(juce::Button::ConnectedOnLeft);
    oneshotBtn.setToggleState(true, juce::dontSendNotification);
    oneshotBtn.setTooltip("One-shot");
    loopModeBtn.setTooltip("Loop");
    pingpongBtn.setTooltip("Ping-pong");
    addAndMakeVisible(oneshotBtn);
    addAndMakeVisible(loopModeBtn);
    addAndMakeVisible(pingpongBtn);

    juce::StringArray loopModeItems;
    for (const auto& e : LoopMode::kEntries) loopModeItems.add(e.label);
    loopModeHidden.addItemList(loopModeItems, 1);
    loopModeHidden.onChange = [this] {
        int id = loopModeHidden.getSelectedId();
        oneshotBtn.setToggleState(id == 1, juce::dontSendNotification);
        loopModeBtn.setToggleState(id == 2, juce::dontSendNotification);
        pingpongBtn.setToggleState(id == 3, juce::dontSendNotification);
        updateVisibility();
        resized();
    };
    oneshotBtn.onClick  = [this] { loopModeHidden.setSelectedId(1); };
    loopModeBtn.onClick = [this] { loopModeHidden.setSelectedId(2); };
    pingpongBtn.onClick = [this] { loopModeHidden.setSelectedId(3); };
    loopModeA = std::make_unique<CA>(apvts, PID::loopMode, loopModeHidden);

    // Crossfade
    crossfadeRow = std::make_unique<SliderRow>("Xfade", fmtMs);
    addAndMakeVisible(*crossfadeRow);
    crossfadeA = std::make_unique<SA>(apvts, PID::crossfadeMs, crossfadeRow->getSlider());
    crossfadeRow->updateValue();
    crossfadeRow->onRightClick = [this](juce::Point<int> pos) {
        showMidiLearnMenu(processorRef, PID::crossfadeMs, pos); };

    // Normalize toggle
    normalizeToggle.setConnectedEdges(juce::Button::ConnectedOnLeft);
    normalizeToggle.setColour(juce::TextButton::buttonColourId, kSurface);
    normalizeToggle.setColour(juce::TextButton::buttonOnColourId, kAccent);
    normalizeToggle.setColour(juce::TextButton::textColourOffId, kDim);
    normalizeToggle.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    normalizeToggle.setClickingTogglesState(true);
    addAndMakeVisible(normalizeToggle);
    normalizeToggle.onClick = [this] {
        auto* param = processorRef.getValueTreeState().getParameter(PID::normalize);
        if (param) param->setValueNotifyingHost(normalizeToggle.getToggleState() ? 1.0f : 0.0f);
    };
    normalizeToggle.setToggleState(
        apvts.getRawParameterValue(PID::normalize)->load() > 0.5f, juce::dontSendNotification);

    // HF boost toggle — compensates VAE decoder high-frequency rolloff
    hfBoostBtn.setConnectedEdges(juce::Button::ConnectedOnRight);
    hfBoostBtn.setColour(juce::TextButton::buttonColourId, kSurface);
    hfBoostBtn.setColour(juce::TextButton::buttonOnColourId, kAccent);
    hfBoostBtn.setColour(juce::TextButton::textColourOffId, kDim);
    hfBoostBtn.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    hfBoostBtn.setClickingTogglesState(true);
    hfBoostBtn.setToggleState(
        apvts.getRawParameterValue(PID::genHfBoost)->load() > 0.5f, juce::dontSendNotification);
    hfBoostBtn.onClick = [this] {
        bool on = hfBoostBtn.getToggleState();
        processorRef.getValueTreeState().getParameter(PID::genHfBoost)
            ->setValueNotifyingHost(on ? 1.0f : 0.0f);
        const auto& raw = processorRef.getGeneratedAudioRaw();
        if (raw.getNumSamples() > 0)
            processorRef.loadGeneratedAudio(raw, processorRef.getGeneratedSampleRate());
    };
    addAndMakeVisible(hfBoostBtn);

    // Loop optimize cycling button (Off → Low → High)
    loopOptimizeBtn.setConnectedEdges(juce::Button::ConnectedOnLeft);
    loopOptimizeBtn.setColour(juce::TextButton::buttonColourId, kSurface);
    loopOptimizeBtn.setColour(juce::TextButton::textColourOffId, kDim);
    addAndMakeVisible(loopOptimizeBtn);
    {
        int initLevel = static_cast<int>(apvts.getRawParameterValue(PID::loopOptimize)->load());
        static const char* labels[] = { "Opt: Off", "Opt: Low", "Opt: High" };
        loopOptimizeBtn.setButtonText(labels[juce::jlimit(0, 2, initLevel)]);
        loopOptimizeBtn.setToggleState(initLevel > 0, juce::dontSendNotification);
        if (initLevel > 0)
        {
            loopOptimizeBtn.setColour(juce::TextButton::buttonColourId, kAccent);
            loopOptimizeBtn.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        }
    }
    loopOptimizeBtn.onClick = [this] {
        auto* param = processorRef.getValueTreeState().getParameter(PID::loopOptimize);
        if (!param) return;
        int cur = static_cast<int>(param->convertFrom0to1(param->getValue()));
        int next = (cur + 1) % 3;
        param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(next)));
        static const char* labels[] = { "Opt: Off", "Opt: Low", "Opt: High" };
        loopOptimizeBtn.setButtonText(labels[next]);
        loopOptimizeBtn.setToggleState(next > 0, juce::dontSendNotification);
        auto col = next > 0 ? kAccent : kSurface;
        auto textCol = next > 0 ? juce::Colours::white : kDim;
        loopOptimizeBtn.setColour(juce::TextButton::buttonColourId, col);
        loopOptimizeBtn.setColour(juce::TextButton::textColourOffId, textCol);
    };

    // ── Scan ──
    scanRow = std::make_unique<SliderRow>("", fmtF2);
    addAndMakeVisible(*scanRow);
    scanHint.setText("Morph between frames (0 = start, 1 = end)", juce::dontSendNotification);
    labelAsCaption(scanHint, kDimmer);
    addAndMakeVisible(scanHint);
    scanA = std::make_unique<SA>(apvts, PID::oscScan, scanRow->getSlider());
    scanRow->updateValue();
    scanRow->onRightClick = [this](juce::Point<int> pos) {
        showMidiLearnMenu(processorRef, PID::oscScan, pos); };

    // ── Octave shift switchbox: -2 | -1 | 0 | +1 | +2 ──
    {
        juce::StringArray octLabels;
        for (const auto& e : OscOctave::kEntries) octLabels.add(e.label);
        octaveHidden.addItemList(octLabels, 1);
        octaveHidden.onChange = [this] {
            int id = octaveHidden.getSelectedId();
            for (int i = 0; i < kNumOctBtns; ++i)
                octBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
        };
        for (int i = 0; i < kNumOctBtns; ++i)
        {
            octBtns[i].setButtonText(octLabels[i]);
            styleSwitchButton(octBtns[i], kAccent);
            octBtns[i].setClickingTogglesState(false);
            octBtns[i].onClick = [this, i] { octaveHidden.setSelectedId(i + 1); };
            addAndMakeVisible(octBtns[i]);
        }
        addChildComponent(octaveHidden);
        octaveA = std::make_unique<CA>(apvts, PID::oscOctave, octaveHidden);
    }

    // ── Noise type switchbox: W | P | B  (shared: both modes) ──
    {
        juce::StringArray noiseLabels;
        for (const auto& e : NoiseKind::kEntries) noiseLabels.add(e.label);
        noiseTypeHidden.addItemList(noiseLabels, 1);
        noiseTypeHidden.onChange = [this] {
            int id = noiseTypeHidden.getSelectedId();
            for (int i = 0; i < kNumNoiseBtns; ++i)
                noiseBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
        };
        for (int i = 0; i < kNumNoiseBtns; ++i)
        {
            noiseBtns[i].setButtonText(noiseLabels[i]);
            styleSwitchButton(noiseBtns[i], kAccent);
            noiseBtns[i].setClickingTogglesState(false);
            noiseBtns[i].onClick = [this, i] { noiseTypeHidden.setSelectedId(i + 1); };
            addAndMakeVisible(noiseBtns[i]);
        }
        noiseTypeA = std::make_unique<CA>(apvts, PID::noiseType, noiseTypeHidden);
    }

    noiseLevelRow = std::make_unique<SliderRow>("Lvl", fmtF2);
    addAndMakeVisible(*noiseLevelRow);
    noiseLevelA = std::make_unique<SA>(apvts, PID::noiseLevel, noiseLevelRow->getSlider());
    noiseLevelRow->updateValue();
    noiseLevelRow->onRightClick = [this](juce::Point<int> pos) {
        showMidiLearnMenu(processorRef, PID::noiseLevel, pos); };

    // ── The authored LRO instrument's own knobs ──
    // Built once, all twelve, and attached to the twelve fixed parameters: what
    // an instrument declares is a NAME and a starting value, never the
    // existence of a parameter (a parameter that came and went could be neither
    // automated nor stored). The label is set from the author's PARAM line in
    // updateVisibility; nothing here invents one, so a row with no declaration
    // is hidden rather than shown with a placeholder.
    {
        static constexpr const char* kLroIds[kNumLroKnobs] = {
            PID::lroP1a, PID::lroP1b, PID::lroP1c, PID::lroP1d,
            PID::lroP2a, PID::lroP2b, PID::lroP2c, PID::lroP2d,
            PID::lroP3a, PID::lroP3b, PID::lroP3c, PID::lroP3d };
        for (int i = 0; i < kNumLroKnobs; ++i)
        {
            // The oscillator's colour, because this IS the oscillator's panel —
            // the same kImpulseA the library list is drawn in.
            lroKnobRows[i] = std::make_unique<SliderRow>("", fmtF2, kImpulseA);
            // The standard bar — name inside it on the left, value inside it on
            // the right, as in the sequencer and the FX column. The instrument
            // writes these names, so their length is not knowable here; a bar
            // clips one word gracefully where a label cell had to be forced to
            // a width that fits the longest of them.
            lroKnobRows[i]->setInlineLabel(true);
            addChildComponent(*lroKnobRows[i]);        // shown only when declared
            lroKnobA[i] = std::make_unique<SA>(apvts, kLroIds[i],
                                               lroKnobRows[i]->getSlider());
            lroKnobRows[i]->updateValue();
            lroKnobRows[i]->onRightClick = [this, id = juce::String(kLroIds[i])]
                (juce::Point<int> pos) { showMidiLearnMenu(processorRef, id, pos); };
        }
        static constexpr const char* kLvlIds[kNumLroLevels] = {
            PID::lroLvl1, PID::lroLvl2, PID::lroLvl3 };
        for (int i = 0; i < kNumLroLevels; ++i)
        {
            // Named here and not from the instrument: a part's level is the
            // host's, not something the library declared, so it is the one row
            // in this card whose word does not travel with the sound.
            lroLevelRows[i] = std::make_unique<SliderRow>("Level", fmtF2, kImpulseA);
            lroLevelRows[i]->setInlineLabel(true);
            addChildComponent(*lroLevelRows[i]);
            lroLevelA[i] = std::make_unique<SA>(apvts, kLvlIds[i],
                                                lroLevelRows[i]->getSlider());
            lroLevelRows[i]->updateValue();
            lroLevelRows[i]->onRightClick = [this, id = juce::String(kLvlIds[i])]
                (juce::Point<int> pos) { showMidiLearnMenu(processorRef, id, pos); };
        }
    }

    // ── Wavetable controls: frame count switchbox ──
    // Frame count switchbox: 32 | 64 | 128 | 256
    {
        juce::StringArray frameLabels;
        for (const auto& e : WtFrames::kEntries) frameLabels.add(e.label);
        framesHidden.addItemList(frameLabels, 1);
        framesHidden.onChange = [this] {
            int id = framesHidden.getSelectedId();
            for (int i = 0; i < kNumFrameBtns; ++i)
                frameBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
            // The re-slice follows the parameter, not this box: see the
            // processor's parameterChanged.
        };
        for (int i = 0; i < kNumFrameBtns; ++i)
        {
            frameBtns[i].setButtonText(frameLabels[i]);
            styleSwitchButton(frameBtns[i], kAccent);
            frameBtns[i].setClickingTogglesState(false);
            frameBtns[i].onClick = [this, i] { framesHidden.setSelectedId(i + 1); };
            addAndMakeVisible(frameBtns[i]);
        }
        wtFramesA = std::make_unique<CA>(apvts, PID::wtFrames, framesHidden);
    }

    auto setupWtToggle = [](juce::TextButton& btn) {
        btn.setClickingTogglesState(true);
        btn.onClick = [&btn] {
            bool on = btn.getToggleState();
            btn.setColour(juce::TextButton::buttonColourId,
                          on ? kAccent : juce::Colours::transparentBlack);
            btn.setColour(juce::TextButton::textColourOffId,
                          on ? juce::Colour(0xff0e1018) : kDimmer);
            btn.setColour(juce::TextButton::textColourOnId,
                          on ? juce::Colour(0xff0e1018) : juce::Colours::white);
        };
    };

    // Smooth toggle
    setupWtToggle(smoothToggle);
    smoothToggle.setToggleState(true, juce::dontSendNotification);
    addAndMakeVisible(smoothToggle);
    wtSmoothA = std::make_unique<BA>(apvts, PID::wtSmooth, smoothToggle);
    smoothToggle.onClick(); // sync initial colors

    // Auto-scan toggle
    setupWtToggle(autoScanToggle);
    autoScanToggle.setToggleState(true, juce::dontSendNotification);
    addAndMakeVisible(autoScanToggle);
    wtAutoScanA = std::make_unique<BA>(apvts, PID::wtAutoScan, autoScanToggle);
    autoScanToggle.onClick(); // sync initial colors

    labelAsCaption(frameCountLabel, kDimmer);
    frameCountLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(frameCountLabel);

    // ── Granular controls: curated texture macro + independent stereo width ──
    {
        juce::StringArray textureLabels;
        for (const auto& e : FreezeTexture::kEntries)
            textureLabels.add(e.label);
        freezeTextureHidden.addItemList(textureLabels, 1);
        freezeTextureHidden.onChange = [this] {
            const int id = freezeTextureHidden.getSelectedId();
            for (int i = 0; i < kNumFreezeTextureBtns; ++i)
                freezeTextureBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
        };
        for (int i = 0; i < kNumFreezeTextureBtns; ++i)
        {
            freezeTextureBtns[i].setButtonText(textureLabels[i]);
            styleSwitchButton(freezeTextureBtns[i], kAccent);
            freezeTextureBtns[i].setClickingTogglesState(false);
            freezeTextureBtns[i].onClick = [this, i] { freezeTextureHidden.setSelectedId(i + 1); };
            addAndMakeVisible(freezeTextureBtns[i]);
        }
        freezeTextureA = std::make_unique<CA>(apvts, PID::freezeTexture, freezeTextureHidden);
        freezeTextureHidden.onChange();
    }

    freezeStereoRow = std::make_unique<SliderRow>("Stereo", fmtPct);
    addAndMakeVisible(*freezeStereoRow);
    freezeStereoA = std::make_unique<SA>(apvts, PID::freezeStereo, freezeStereoRow->getSlider());
    freezeStereoRow->onRightClick = [this](juce::Point<int> pos) {
        showMidiLearnMenu(processorRef, PID::freezeStereo, pos); };
    freezeStereoRow->updateValue();

    // ── Section headers — inverted (colored bg, light text) ──
    auto makeHeader = [this](juce::Label& lbl, const juce::String& text, juce::Colour col) {
        lbl.setText(" " + text, juce::dontSendNotification);
        labelAsHeaderBand(lbl, col);
        lbl.setJustificationType(juce::Justification::centredLeft);
        addAndMakeVisible(lbl);
    };
    makeHeader(engineHeader, "ENGINE", kAccent);
    makeHeader(filterHeader, "FILTER", kFilterCol);
    makeHeader(modHeader, "ENVELOPES", kModCol);

    // The Easy/Advanced modulation view was removed — the panel is always the
    // unified view. The mode toggle is no longer created or shown.

    auto setupModTabs = [this](std::array<juce::TextButton, kNumModTabs>& tabs,
                               const char* const* labels,
                               int radioGroup,
                               int count)
    {
        // LFO and Drift have three tabs, ENV has five; the arrays are all sized
        // for the larger so they can share this helper. Reading `labels` past
        // `count` would run off the end of a 3-element literal.
        for (int i = 0; i < count; ++i)
        {
            auto& btn = tabs[static_cast<size_t>(i)];
            btn.setButtonText(labels[i]);
            styleSwitchButton(btn, kModCol);
            btn.setClickingTogglesState(true);
            btn.setRadioGroupId(radioGroup);
            btn.onClick = [this, i, radioGroup]
            {
                if (radioGroup == 4001)      activeEnvTab = i;
                else if (radioGroup == 4002) activeLfoTab = i;
                else                         activeDriftTab = i;
                syncModTabButtons();
                updateVisibility();
                resized();
                repaint();
            };
            addAndMakeVisible(btn);
        }
    };
    const char* const envTabLabels[] = { "ENV1", "ENV2", "ENV3", "ENV4", "ENV5" };
    const char* const lfoTabLabels[] = { "LFO 1", "LFO 2", "LFO 3" };
    const char* const driftTabLabels[] = { "Drift 1", "Drift 2", "Drift 3" };
    setupModTabs(envTabBtns, envTabLabels, 4001, kNumModTabs);
    setupModTabs(lfoTabBtns, lfoTabLabels, 4002, kNumLfoTabs);
    setupModTabs(driftTabBtns, driftTabLabels, 4003, kNumLfoTabs);

    // ── Filter mode: OFF LP HP BP, one dropdown ──
    // OFF is a value of this parameter, not a separate switch, so bypass and
    // type are the same choice and cannot disagree. It replaces the pair of
    // buttons this used to drive; the cell that pair cost in the slope row is
    // what gives 18 dB its segment back.
    {
        const juce::StringArray typeLabels { "OFF", "LP", "HP", "BP" };
        filterTypeBox.addItemList(typeLabels, 1);
        filterTypeBox.setColour(juce::ComboBox::backgroundColourId, kSurface);
        filterTypeBox.setColour(juce::ComboBox::textColourId, juce::Colours::white);
        filterTypeBox.setColour(juce::ComboBox::outlineColourId, kFilterCol);
        filterTypeBox.setJustificationType(juce::Justification::centred);
        filterTypeBox.onChange = [this] {
            updateVisibility();
            // updateVisibility() hides lfoHeader/driftHeader; in the columnar mod
            // view they double as the LFO/DRIFT column header bars and are
            // re-shown only by layoutModEasy. Re-run layout so they don't vanish
            // when the filter is toggled. (Mirrors every other onChange in this file.)
            resized();
        };
        addAndMakeVisible(filterTypeBox);
    }

    // ── Filter slope switchbox: 6dB 12dB 18dB 24dB ──
    {
        juce::StringArray slopeLabels;
        for (const auto& e : FilterSlope::kEntries) slopeLabels.add(e.label);
        filterSlopeHidden.addItemList(slopeLabels, 1);
        filterSlopeHidden.onChange = [this] {
            int id = filterSlopeHidden.getSelectedId();
            for (int i = 0; i < kNumSlopeBtns; ++i)
                filterSlopeBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
        };
        for (int i = 0; i < kNumSlopeBtns; ++i)
        {
            filterSlopeBtns[i].setButtonText(slopeLabels[i]);
            styleSwitchButton(filterSlopeBtns[i], kFilterCol);
            filterSlopeBtns[i].setClickingTogglesState(true);
            filterSlopeBtns[i].setRadioGroupId(3002);
            { int edges = 0;
              if (i > 0) edges |= juce::Button::ConnectedOnLeft;
              if (i < kNumSlopeBtns - 1) edges |= juce::Button::ConnectedOnRight;
              filterSlopeBtns[i].setConnectedEdges(edges); }
            filterSlopeBtns[i].onClick = [this, i] { filterSlopeHidden.setSelectedId(i + 1); };
            addAndMakeVisible(filterSlopeBtns[i]);
        }
    }

    // ── Filter algorithm switchbox: SVF Ladder Warp ──
    {
        juce::StringArray algLabels;
        for (const auto& e : FilterAlgorithm::kEntries) algLabels.add(e.label);
        filterAlgHidden.addItemList(algLabels, 1);
        filterAlgHidden.onChange = [this] {
            int id = filterAlgHidden.getSelectedId();
            for (int i = 0; i < kNumAlgBtns; ++i)
                filterAlgBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
            updateVisibility();
            // See filterTypeBox.onChange: re-run layout so the columnar
            // LFO/DRIFT header bars (re-shown only in layoutModEasy) survive a
            // filter-algorithm change in easy mode.
            resized();
        };
        for (int i = 0; i < kNumAlgBtns; ++i)
        {
            filterAlgBtns[i].setButtonText(algLabels[i]);
            styleSwitchButton(filterAlgBtns[i], kFilterCol);
            filterAlgBtns[i].setClickingTogglesState(true);
            filterAlgBtns[i].setRadioGroupId(3005);
            { int edges = 0;
              if (i > 0) edges |= juce::Button::ConnectedOnLeft;
              if (i < kNumAlgBtns - 1) edges |= juce::Button::ConnectedOnRight;
              filterAlgBtns[i].setConnectedEdges(edges); }
            filterAlgBtns[i].onClick = [this, i] {
                if (i == FilterAlgorithm::Warp)
                    filterWarpStyleBox.setSelectedId(FilterWarpStyle::SoftClip + 1);
                // OFF + algorithm buttons form one 4-way switch: picking an
                // algorithm while bypassed re-enables the filter.
                if (filterTypeBox.getSelectedId() == FilterType::Off + 1)
                    filterTypeBox.setSelectedId(FilterType::Lowpass + 1);
                filterAlgHidden.setSelectedId(i + 1);
            };
            addAndMakeVisible(filterAlgBtns[i]);
        }
    }

    // ── Warp style combo: tanh / softclip / ojd / sin / digital / asym ──
    {
        juce::StringArray styleLabels;
        for (const auto& e : FilterWarpStyle::kEntries) styleLabels.add(e.label);
        filterWarpStyleBox.addItemList(styleLabels, 1);
        filterWarpStyleBox.setColour(juce::ComboBox::backgroundColourId, kSurface);
        filterWarpStyleBox.setColour(juce::ComboBox::textColourId, juce::Colours::white);
        filterWarpStyleBox.setColour(juce::ComboBox::outlineColourId, kFilterCol);
        filterWarpStyleBox.onChange = [this] {
            const int id = filterWarpStyleBox.getSelectedId();
            if (id >= 1 && id <= FilterWarpStyle::kCount)
                filterEasyWarpBtn.setButtonText(
                    juce::String(FilterWarpStyle::kEntries[id - 1].label));
        };
        addAndMakeVisible(filterWarpStyleBox);
    }

    // ── Easy-mode Warp hold button ──
    {
        styleSwitchButton(filterEasyWarpBtn, kFilterCol);
        filterEasyWarpBtn.setClickingTogglesState(false);
        filterEasyWarpBtn.setConnectedEdges(juce::Button::ConnectedOnLeft);
        addAndMakeVisible(filterEasyWarpBtn);

        filterEasyWarpBtn.onTap = [this] {
            if (filterTypeBox.getSelectedId() == FilterType::Off + 1)
                filterTypeBox.setSelectedId(FilterType::Lowpass + 1);
            filterAlgHidden.setSelectedId(FilterAlgorithm::Warp + 1);
        };
        filterEasyWarpBtn.onStylePick = [this](int id) {
            filterWarpStyleBox.setSelectedId(id);
            if (filterTypeBox.getSelectedId() == FilterType::Off + 1)
                filterTypeBox.setSelectedId(FilterType::Lowpass + 1);
            filterAlgHidden.setSelectedId(FilterAlgorithm::Warp + 1);
        };
    }

    // ── Filter drive oversampling: Off 2x 4x 8x ──
    // filterDriveOsHidden is a hidden carrier only — no visible switchbox in
    // the unified view; drive oversampling is controlled elsewhere.
    {
        juce::StringArray osLabels;
        for (const auto& e : FilterDriveOs::kEntries) osLabels.add(e.label);
        filterDriveOsHidden.addItemList(osLabels, 1);
    }

    cutoffRow      = std::make_unique<SliderRow>("Cutoff",    fmtHz,  kFilterCol);
    resoRow        = std::make_unique<SliderRow>("Resonance", fmtF2,  kFilterCol);
    filterMixRow   = std::make_unique<SliderRow>("Mix",       fmtPct, kFilterCol);
    kbdTrackRow    = std::make_unique<SliderRow>("Kbd Track", fmtPct, kFilterCol);
    filterDriveRow = std::make_unique<SliderRow>("Drive",     fmtDb,  kFilterCol);
    for (auto* r : { cutoffRow.get(), resoRow.get(), filterMixRow.get(),
                     kbdTrackRow.get(), filterDriveRow.get() })
        addAndMakeVisible(*r);

    cutoffA        = std::make_unique<SA>(apvts, PID::filterCutoff,    cutoffRow->getSlider());
    resoA          = std::make_unique<SA>(apvts, PID::filterResonance, resoRow->getSlider());
    filterMixA     = std::make_unique<SA>(apvts, PID::filterMix,       filterMixRow->getSlider());
    kbdTrackA      = std::make_unique<SA>(apvts, PID::filterKbdTrack,  kbdTrackRow->getSlider());
    filterDriveA   = std::make_unique<SA>(apvts, PID::filterDrive,     filterDriveRow->getSlider());

    cutoffRow->onRightClick     = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::filterCutoff,    p); };
    resoRow->onRightClick       = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::filterResonance, p); };
    filterMixRow->onRightClick  = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::filterMix,       p); };
    kbdTrackRow->onRightClick   = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::filterKbdTrack,  p); };
    filterDriveRow->onRightClick = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::filterDrive,    p); };

    filterTypeA      = std::make_unique<CA>(apvts, PID::filterType,      filterTypeBox);
    filterSlopeA     = std::make_unique<CA>(apvts, PID::filterSlope,     filterSlopeHidden);
    filterDriveOsA   = std::make_unique<CA>(apvts, PID::filterDriveOs,   filterDriveOsHidden);
    filterAlgA       = std::make_unique<CA>(apvts, PID::filterAlgorithm, filterAlgHidden);
    filterWarpStyleA = std::make_unique<CA>(apvts, PID::filterWarpStyle, filterWarpStyleBox);
    // Sync WarpHoldBtn label from APVTS directly — ComboBoxAttachment notification
    // timing on first paint is unreliable (same reason syncRadioRow exists below).
    {
        const int raw = static_cast<int>(apvts.getRawParameterValue(PID::filterWarpStyle)->load());
        const int idx = juce::jlimit(0, FilterWarpStyle::kCount - 1, raw);
        filterEasyWarpBtn.setButtonText(juce::String(FilterWarpStyle::kEntries[idx].label));
    }

    // Initial radio-button visual sync. Reading the APVTS choice index
    // directly is robust against ComboBoxAttachment notification-timing
    // quirks across JUCE versions — the attachment drives the hidden combo,
    // but whether its onChange lambda fires on the very first paint has
    // historically been unreliable, leaving the filter card with no active
    // SLOPE / ALG button until the user clicks one. Direct APVTS read
    // sidesteps that entirely.
    auto syncRadioRow = [&apvts](const juce::String& pid,
                                  juce::TextButton* btns, int n) {
        const int raw = static_cast<int>(apvts.getRawParameterValue(pid)->load());
        const int active = juce::jlimit(0, n - 1, raw);
        for (int i = 0; i < n; ++i)
            btns[i].setToggleState(i == active, juce::dontSendNotification);
    };
    syncRadioRow(PID::filterSlope,     filterSlopeBtns,   kNumSlopeBtns);
    syncRadioRow(PID::filterAlgorithm, filterAlgBtns,     kNumAlgBtns);
    syncRadioRow(PID::freezeTexture,   freezeTextureBtns, kNumFreezeTextureBtns);

    cutoffRow->updateValue();
    resoRow->updateValue();
    filterMixRow->updateValue();
    kbdTrackRow->updateValue();
    filterDriveRow->updateValue();

    // ── Envelopes ──
    initEnv(ampEnv, "ENV 1", 2, PID::ampEnv, apvts);
    for (int i = 0; i < kNumModEnvs; ++i)
        initEnv(modEnvSections[i], "ENV " + juce::String(i + 2), 1, PID::modEnv[i], apvts);

    // ── LFOs ──
    initLfo(lfo1, "LFO 1",
            PID::lfo1Rate, PID::lfo1Depth, PID::lfo1Wave, PID::lfo1Mode,
            PID::lfo1ClockMode, PID::lfo1ClockDivision, apvts);
    initLfo(lfo2, "LFO 2",
            PID::lfo2Rate, PID::lfo2Depth, PID::lfo2Wave, PID::lfo2Mode,
            PID::lfo2ClockMode, PID::lfo2ClockDivision, apvts);
    initLfo(lfo3, "LFO 3",
            PID::lfo3Rate, PID::lfo3Depth, PID::lfo3Wave, PID::lfo3Mode,
            PID::lfo3ClockMode, PID::lfo3ClockDivision, apvts);

    // ── Easy-panel AT module: one bipolar drag-fill bar per target ──
    {
        // A row is an AMOUNT and a SOURCE: how deep, and driven by which of the
        // player's expression axes, or Ø. The two params travel together everywhere.
        // The TARGET travels with the row. The rows below are in DISPLAY order
        // and AftertouchTarget is in a different one (BlockParams.h:746), so the
        // row index is NOT the target and deriving one from the other silently
        // asks the wrong row whether it is wired to its default axis.
        struct AtBar { const char* pid; const char* srcPid; const char* label; int target; };
        // Order follows the canonical EnvTarget order (BlockParams.h): voice
        // destinations first (DCA, Filter=Cutoff+Reso, Scan, Pitch, Noise), then
        // the mod-source levels (LFO depths, then env sustains). "Amt" matches the
        // LFO module's own depth label in the easy panel.
        static const AtBar atBars[] = {
            { PID::aftertouchAmtDca,         PID::exprSrcDca,         "DCA" , AftertouchTarget::DCA },
            { PID::aftertouchAmtCutoff,      PID::exprSrcCutoff,      "Cutoff" , AftertouchTarget::Cutoff },
            { PID::aftertouchAmtResonance,   PID::exprSrcResonance,   "Reso" , AftertouchTarget::Resonance },
            { PID::aftertouchAmtScan,        PID::exprSrcScan,        "Scan" , AftertouchTarget::Scan },
            { PID::aftertouchAmtPitch,       PID::exprSrcPitch,       "Pitch" , AftertouchTarget::Pitch },
            { PID::aftertouchAmtNoiseLevel,  PID::exprSrcNoiseLevel,  "Noise" , AftertouchTarget::NoiseLevel },
            { PID::aftertouchAmtLfo1Depth,   PID::exprSrcLfo1Depth,   "LFO1 Amt" , AftertouchTarget::LFO1Depth },
            { PID::aftertouchAmtLfo2Depth,   PID::exprSrcLfo2Depth,   "LFO2 Amt" , AftertouchTarget::LFO2Depth },
            { PID::aftertouchAmtLfo3Depth,   PID::exprSrcLfo3Depth,   "LFO3 Amt" , AftertouchTarget::LFO3Depth },
            { PID::aftertouchAmtEnv1Sustain, PID::exprSrcEnv1Sustain, "ENV1 Sus" , AftertouchTarget::Env1Sustain },
            { PID::aftertouchAmtEnv2Sustain, PID::exprSrcEnv2Sustain, "ENV2 Sus" , AftertouchTarget::Env2Sustain },
            { PID::aftertouchAmtEnv3Sustain, PID::exprSrcEnv3Sustain, "ENV3 Sus" , AftertouchTarget::Env3Sustain },
            { PID::aftertouchAmtEnv4Sustain, PID::exprSrcEnv4Sustain, "ENV4 Sus" , AftertouchTarget::Env4Sustain },
            { PID::aftertouchAmtEnv5Sustain, PID::exprSrcEnv5Sustain, "ENV5 Sus" , AftertouchTarget::Env5Sustain },
            // Last, and apart in kind: this one does not modulate the voice, it
            // moves the instrument - to another cached sample. Same bar, same
            // bipolar amount, and the sign means the same thing it means
            // everywhere else here: which way pressure travels. Through the
            // cache that is the order the samples were generated in, forwards or
            // back. There is no Snap row beside it - see AftertouchTarget::Snap.
            { PID::aftertouchAmtCache,       PID::exprSrcCache,       "Cache" , AftertouchTarget::Cache },
        };
        static constexpr int kNumAtBars = sizeof(atBars) / sizeof(atBars[0]);
        static_assert(kNumAtBars == AftertouchTarget::kNumExprRows,
                      "Every target in the expression matrix needs a bar here.");
        for (int i = 0; i < kNumAtBars; ++i)
        {
            const char* pid = atBars[i].pid;
            auto bar = std::make_unique<AftertouchBar>();
            bar->setTargetLabel(atBars[i].label);
            if (auto* p = apvts.getParameter(pid))
            {
                // NOT a "touched" mark. AftertouchBar fires this on every
                // mouseDown, before it knows whether anything changes - so a
                // bare click, and worse the double-click whose whole job is to
                // put the row back to zero, would light the box for a row that
                // drives nothing. A row whose amount is off zero already shows
                // its axis without any flag; the flag is only for the case a
                // value cannot express, which is the SOURCE being chosen.
                bar->onDragStart = [p] { p->beginChangeGesture(); };
                bar->onDragEnd   = [p] { p->endChangeGesture(); };
            }
            bar->onRightClick = [this, pid](juce::Point<int> pt) {
                showMidiLearnMenu(processorRef, pid, pt); };
            addAndMakeVisible(*bar);
            aftertouchBarA[i] = std::make_unique<SA>(apvts, pid, *bar);
            aftertouchBars[i] = std::move(bar);

            const char* srcPid = atBars[i].srcPid;
            auto sw = std::make_unique<ExprSourceSwitch>();
            sw->onRightClick = [this, srcPid](juce::Point<int> pt) {
                showMidiLearnMenu(processorRef, srcPid, pt); };
            sw->onUserPick = [this, i] { atRowTouched_[i] = true; };
            addAndMakeVisible(*sw);
            exprSrcSwitchA[i] = std::make_unique<
                juce::AudioProcessorValueTreeState::ComboBoxAttachment>(apvts, srcPid, *sw);
            exprSrcSwitches[i] = std::move(sw);

            atRowAmtPtr_[i] = apvts.getRawParameterValue(pid);
            atRowSrcPtr_[i] = apvts.getRawParameterValue(srcPid);
            atRowTarget_[i] = atBars[i].target;
        }
        addChildComponent(aftertouchHeader);   // shown by the columns easy-layout
    }

    // ── Drift ──
    initDrift(drift1, "D1",
              PID::drift1Rate, PID::drift1Depth, PID::drift1Target, PID::drift1Wave,
              PID::drift1ClockMode, PID::drift1ClockDivision, apvts);
    initDrift(drift2, "D2",
              PID::drift2Rate, PID::drift2Depth, PID::drift2Target, PID::drift2Wave,
              PID::drift2ClockMode, PID::drift2ClockDivision, apvts);
    initDrift(drift3, "D3",
              PID::drift3Rate, PID::drift3Depth, PID::drift3Target, PID::drift3Wave,
              PID::drift3ClockMode, PID::drift3ClockDivision, apvts);

    paintSectionHeader(lfoHeader, "LFO", kLfoCol);
    addAndMakeVisible(lfoHeader);

    paintSectionHeader(driftHeader, "DRIFT", kDriftCol);
    addAndMakeVisible(driftHeader);

    // Regenerate mode switchbox
    paintSectionHeader(regenHeader, "REGENERATE", kRegenCol);
    addAndMakeVisible(regenHeader);

    juce::StringArray regenItems;
    for (const auto& e : DriftRegen::kEntries) regenItems.add(juce::String(juce::CharPointer_UTF8(e.label)));
    regenHidden.addItemList(regenItems, 1);
    regenHidden.onChange = [this] {
        int id = regenHidden.getSelectedId();
        for (int i = 0; i < kNumRegenBtns; ++i)
            regenBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
    };
    addAndMakeVisible(regenHidden);
    for (int i = 0; i < kNumRegenBtns; ++i)
    {
        regenBtns[i].setButtonText(regenItems[i]);
        styleSwitchButton(regenBtns[i], kDriftCol);
        regenBtns[i].setClickingTogglesState(true);
        regenBtns[i].setRadioGroupId(3006);
        regenBtns[i].onClick = [this, i] { regenHidden.setSelectedId(i + 1); };
        addAndMakeVisible(regenBtns[i]);
    }
    driftRegenA = std::make_unique<CA>(apvts, PID::driftRegen, regenHidden);

    // Crossfade slider for drift regeneration
    crossfadeRegenRow = std::make_unique<SliderRow>("XFade", fmtMs, kDriftCol);
    addAndMakeVisible(*crossfadeRegenRow);
    crossfadeRegenA = std::make_unique<SA>(apvts, PID::driftCrossfade, crossfadeRegenRow->getSlider());
    crossfadeRegenRow->onRightClick = [this](juce::Point<int> p) {
        showMidiLearnMenu(processorRef, PID::driftCrossfade, p); };
    crossfadeRegenRow->updateValue();

    // All components are now set up — enable callbacks and trigger initial state
    initialized = true;

    // Deferred APVTS attachments for target ComboBoxes
    // (must come after initialized=true so onChange → updateVisibility works)
    ampTargetA = std::make_unique<CA>(apvts, PID::ampTarget, ampEnv.targetBox);
    for (int i = 0; i < kNumModEnvs; ++i)
        modTargetA[i] = std::make_unique<CA>(apvts, PID::modEnv[i].target,
                                             modEnvSections[i].targetBox);
    lfo1TargetA = std::make_unique<CA>(apvts, PID::lfo1Target, lfo1.targetBox);
    lfo2TargetA = std::make_unique<CA>(apvts, PID::lfo2Target, lfo2.targetBox);
    lfo3TargetA = std::make_unique<CA>(apvts, PID::lfo3Target, lfo3.targetBox);

    // Easy/Advanced view removed: the panel is permanently the unified view.
    selectFirstActiveModTabs();
    syncModTabButtons();
    updateVisibility();
    resized();
    repaint();
    updateVisibility();
    // Before the first paint, not a timer period after it: the three rows that
    // ship wired at amount zero would otherwise be drawn lit and flip to Ø up
    // to 33 ms later, which is the exact picture this is here to remove.
    refreshExprRowOffState();
    startTimerHz(30);
}

// ──────────────────────────────────────────────────────────────────────────────
// Waveform display polling
// ──────────────────────────────────────────────────────────────────────────────
void SynthPanel::followModParamToTab(const juce::String& paramId)
{
    if (paramId.isEmpty()) return;

    // env A/D/S/R → ENV tab (amp = ENV1, mod1 = ENV2, mod2 = ENV3).
    // amp_amount (the volume fader) deliberately excluded — it is not an envelope edit.
    auto isEnvAdsr = [&paramId](const char* prefix)
    {
        return paramId.startsWith(prefix)
            && (paramId.endsWith("_attack") || paramId.endsWith("_decay")
             || paramId.endsWith("_sustain") || paramId.endsWith("_release"));
    };

    int group = -1;  // 0 = env, 1 = lfo, 2 = drift
    int index = -1;  // tab within the group (0..2)
    if      (isEnvAdsr("amp_"))             { group = 0; index = 0; }
    else if (isEnvAdsr("mod1_"))            { group = 0; index = 1; }
    else if (isEnvAdsr("mod2_"))            { group = 0; index = 2; }
    else if (isEnvAdsr("mod3_"))            { group = 0; index = 3; }
    else if (isEnvAdsr("mod4_"))            { group = 0; index = 4; }
    else if (paramId.startsWith("lfo1_"))   { group = 1; index = 0; }
    else if (paramId.startsWith("lfo2_"))   { group = 1; index = 1; }
    else if (paramId.startsWith("lfo3_"))   { group = 1; index = 2; }
    else if (paramId.startsWith("drift1_")) { group = 2; index = 0; }
    else if (paramId.startsWith("drift2_")) { group = 2; index = 1; }
    else if (paramId.startsWith("drift3_")) { group = 2; index = 2; }
    if (group < 0) return;

    int& active = (group == 0) ? activeEnvTab : (group == 1) ? activeLfoTab : activeDriftTab;
    if (active == index) return;  // already showing → avoid needless relayout/repaint

    active = index;
    syncModTabButtons();
    updateVisibility();
    resized();
    repaint();
}

void SynthPanel::refreshExprRowOffState()
{
    // Which expression rows are OFF, and OFF means what it says: the row's
    // amount is at zero, so nothing is driven, AND its source is still the one
    // it shipped with, so the player has not chosen this wiring. Three of the
    // sixteen ship wired that way (DCA on Z, Cutoff and Scan on Y) at amount
    // zero, and the column showed three lit boxes for three things that were
    // not happening. BJ, 26.08.2026. A fourth ships wired - Pitch on X - and is
    // excluded below, because that one IS happening.
    //
    // "Nicht bereits manuell verändert" is BJ's own second condition and it is
    // the reason atRowTouched_ exists rather than a purely stateless compare.
    // Without it the box could not show the default axis at all while the amount
    // sits at zero: click round the DCA row and Z - its own default - would draw
    // as Ø, and the None state would be unreachable, because the next click
    // steps from what is SHOWN. A row the player has moved always shows its
    // value; only a row still standing exactly where it shipped reads OFF.
    //
    // The flag lives for the life of the EDITOR, and its two edges are stated
    // rather than papered over: a preset load does not clear it, so a touched
    // row put back to default-at-zero draws its axis instead of Ø; and closing
    // the window does clear it, so that same row comes back Ø. Both edges only
    // ever concern a row that is driving nothing either way, and the box is
    // telling the truth about its VALUE in both. The alternative - no flag,
    // purely a function of the patch - costs more than it buys: at amount zero
    // the box could then not show the default axis at all, and the click cycle
    // would silently skip it and Ø both, which is a broken control rather than
    // an overstated history.
    //
    // Sixteen relaxed loads and one int compare per tick; the switches are only
    // touched when the picture changes, and setRowIsOff repaints only on a real
    // change of its own (docs/PERFORMANCE_GUIDE.md).
    juce::uint32 mask = 0;
    for (size_t i = 0; i < exprSrcSwitches.size(); ++i)
    {
        const auto* amt = atRowAmtPtr_[i];
        const auto* src = atRowSrcPtr_[i];
        if (amt == nullptr || src == nullptr)
            continue;
        const int wired = static_cast<int>(std::lround(src->load(std::memory_order_relaxed)));
        // TWO values count as "still where it shipped". The row's own default is
        // one. ExprSource::kLegacy is the other, and it is the bigger case: a
        // preset or DAW session written before sources existed gets kLegacy - Z,
        // the one axis that path had - written into ALL SIXTEEN rows, because
        // that is what such a file meant. Against the per-row default alone,
        // fifteen of those sixteen then read as a wiring the player chose, and
        // opening one of the shipped presets lit fifteen orange boxes for
        // fifteen routings at depth zero. Not a fresh patch: the file BJ is
        // most likely to open.
        const bool shipped = (wired == ExprSource::defaultFor(atRowTarget_[i])
                           || wired == ExprSource::kLegacy);
        // ...except PITCH, the one row where the source does something on its
        // own. Everywhere else it only says WHAT MODULATES the target, so at
        // depth zero nothing happens and Ø is the truth. On this row it also
        // says whether the per-note bend is HEARD - X at depth zero IS the
        // lateral finger bending the note, and drawing that Ø would grey out
        // the one control that decides it while it is working.
        const bool off = atRowTarget_[i] != AftertouchTarget::Pitch
                      && ! atRowTouched_[i] && shipped
                      && std::abs(amt->load(std::memory_order_relaxed)) < kAftertouchAmtEpsilon;
        if (off)
            mask |= (1u << static_cast<juce::uint32>(i));
    }
    if (mask == atRowOffMask_)
        return;
    atRowOffMask_ = mask;
    for (size_t i = 0; i < exprSrcSwitches.size(); ++i)
        if (exprSrcSwitches[i] != nullptr)
            exprSrcSwitches[i]->setRowIsOff((mask & (1u << static_cast<juce::uint32>(i))) != 0);
}

void SynthPanel::timerCallback()
{
    // AHEAD of the audioIdle gate below, like the FX panel's running lamps and
    // for the same reason: a player pulling an expression amount to zero on a
    // silent synth still has to see the box go dim. Sixteen relaxed loads.
    refreshExprRowOffState();

    // A newly authored instrument brings its own knobs. One int compare per
    // tick — the panel takes a copy only when the processor's revision moves,
    // and a copy is the whole cost: the set is at most twelve short strings.
    // No polling of the backend, no work while nothing is being written.
    if (const int rev = processorRef.getCsoundControlsRevision(); rev != lroControlsSeen)
    {
        lroControlsSeen = rev;
        lroControls = processorRef.getCsoundControls();
        updateVisibility();   // which rows exist, and what they are called
        resized();            // where they sit
        repaint();            // the caption and the part names over them
    }

    // Tab follows controller: when an incoming mapped CC changes a mod param,
    // switch the visible ENV/LFO/Drift tab to the affected one.
    {
        const uint64_t packed = processorRef.getMidiTouchPacked();
        const uint32_t seq = static_cast<uint32_t>(packed >> 32);
        if (seq != lastSeenMidiTouchSeq_)
        {
            lastSeenMidiTouchSeq_ = seq;
            followModParamToTab(
                processorRef.getCcMappingCopy(static_cast<int>(packed & 0xffffffffu)).paramId);
        }
    }

    if (processorRef.hasNewWaveform())
    {
        auto& snap = processorRef.getWaveformSnapshot();
        int numSamples = snap.getNumSamples();
        if (numSamples > 0)
        {
            const int displayPoints = 1024;
            const float* src = snap.getReadPointer(0);

            if (numSamples > displayPoints * 2)
            {
                // Peak-preserving downsample
                std::vector<float> display(displayPoints);
                int bucketSize = numSamples / displayPoints;
                for (int i = 0; i < displayPoints; ++i)
                {
                    int start = i * bucketSize;
                    int end = juce::jmin(start + bucketSize, numSamples);
                    float peak = 0.0f;
                    int peakIdx = start;
                    for (int j = start; j < end; ++j)
                    {
                        float absVal = std::abs(src[j]);
                        if (absVal > peak) { peak = absVal; peakIdx = j; }
                    }
                    display[static_cast<size_t>(i)] = src[peakIdx];
                }
                waveformDisplay.setWaveform(display.data(), displayPoints);
            }
            else
            {
                waveformDisplay.setWaveform(src, numSamples);
            }
        }
        // Update buffer duration for time labels
        double sr = processorRef.getSampleRate();
        if (sr > 0)
            waveformDisplay.setBufferDuration(static_cast<float>(numSamples / sr));

        // Sync shared P1/P2/P3 playback markers from the processor.
        //
        // The four reads are taken TOGETHER under the callback lock: the
        // reprepare thread publishes all three markers at once
        // (applyPreparedBufferLoad), so landing between two of these reads
        // hands over an old start with a new end, and a torn pair is exactly
        // what the pair setter below cannot detect — it
        // resolves it into brackets that look deliberate, which the next drag
        // then sends back to the sampler as if the user had put them there.
        float s = 0.0f, e = 1.0f, p1 = 0.0f;
        bool locked = false;
        {
            const juce::ScopedLock sl (processorRef.getCallbackLock());
            s      = processorRef.getSampler().getLoopStart();
            e      = processorRef.getSampler().getLoopEnd();
            p1     = processorRef.getSampler().getStartPos();
            locked = processorRef.getSampler().getPointsLocked();
        }
        // And applied as a pair: mirroring one marker at a time clamps the
        // incoming window against the one still drawn, so the brackets would
        // depend on the window they are replacing.
        waveformDisplay.setLoopRegion(s, e);
        waveformDisplay.setStartPos(p1);
        waveformDisplay.getLockButton().setLocked(locked);

        processorRef.clearNewWaveformFlag();

        // A fresh neural sample means the table is no longer a DCO/LCO bake:
        // setWaveform already dropped wtMode; reconcile the region label too (a
        // neural gen on the Wavetable engine may not re-run updateVisibility).
        reconcileWaveformDisplayMode();

        // Update frame count display
        int nf = processorRef.getMasterOsc().getNumFrames();
        frameCountLabel.setText(juce::String(nf) + "f", juce::dontSendNotification);
    }

    // Baked wavetable (LCO/DCO): draw the actual table in the engine window as a
    // 2.5D fan with a scan cursor, in place of the last neural sample. Distinct
    // from hasNewWaveform — a bake publishes frames + this display strip but NO
    // sample snapshot, so the extraction-region sample view never fires.
    if (processorRef.hasNewWtDisplay())
    {
        waveformDisplay.setWavetableFrames(processorRef.getWtDisplaySnapshot(),
                                           WavetableOscillator::FRAME_SIZE);
        processorRef.clearNewWtDisplayFlag();
        reconcileWaveformDisplayMode();   // ensure the "Wavetable" label + fan mode
        int nf = processorRef.getMasterOsc().getNumFrames();
        frameCountLabel.setText(juce::String(nf) + "f", juce::dontSendNotification);
    }

    // Update ghost targets from modulated values (skip when audio is idle)
    if (!processorRef.audioIdle.load(std::memory_order_relaxed))
    {
        auto& mv = processorRef.modulatedValues;
        cutoffRow->setGhostValue(mv.filterCutoff.load(std::memory_order_relaxed));
        resoRow->setGhostValue(mv.filterResonance.load(std::memory_order_relaxed));
        noiseLevelRow->setGhostValue(mv.noiseLevel.load(std::memory_order_relaxed));
        if (lfo1.rateRow)  lfo1.rateRow->setGhostValue(mv.lfo1Rate.load(std::memory_order_relaxed));
        if (lfo1.depthRow) lfo1.depthRow->setGhostValue(mv.lfo1Depth.load(std::memory_order_relaxed));
        if (lfo2.rateRow)  lfo2.rateRow->setGhostValue(mv.lfo2Rate.load(std::memory_order_relaxed));
        if (lfo2.depthRow) lfo2.depthRow->setGhostValue(mv.lfo2Depth.load(std::memory_order_relaxed));
        if (lfo3.rateRow)  lfo3.rateRow->setGhostValue(mv.lfo3Rate.load(std::memory_order_relaxed));
        if (lfo3.depthRow) lfo3.depthRow->setGhostValue(mv.lfo3Depth.load(std::memory_order_relaxed));
        if (drift1.depthRow) drift1.depthRow->setGhostValue(mv.drift1Depth.load(std::memory_order_relaxed));
        if (drift2.depthRow) drift2.depthRow->setGhostValue(mv.drift2Depth.load(std::memory_order_relaxed));
        if (drift3.depthRow) drift3.depthRow->setGhostValue(mv.drift3Depth.load(std::memory_order_relaxed));
        if (waveformDisplay.isVisible())
            waveformDisplay.setScanPosition(mv.scanPosition.load(std::memory_order_relaxed));
    }

    // Advance ghost smoothing (runs every frame at 30 Hz)
    cutoffRow->tickGhost();
    resoRow->tickGhost();
    noiseLevelRow->tickGhost();
    if (lfo1.rateRow)  lfo1.rateRow->tickGhost();
    if (lfo1.depthRow) lfo1.depthRow->tickGhost();
    if (lfo2.rateRow)  lfo2.rateRow->tickGhost();
    if (lfo2.depthRow) lfo2.depthRow->tickGhost();
    if (lfo3.rateRow)  lfo3.rateRow->tickGhost();
    if (lfo3.depthRow) lfo3.depthRow->tickGhost();
    if (drift1.depthRow) drift1.depthRow->tickGhost();
    if (drift2.depthRow) drift2.depthRow->tickGhost();
    if (drift3.depthRow) drift3.depthRow->tickGhost();
    waveformDisplay.tickScan();
}

// Reconcile the engine-window waveform display with the engine state: show the
// 2.5D WT fan while a DCO/LCO table is active, else the interactive sample view,
// and set the region label to match. Idempotent and cheap (no per-tick cost of
// note) — safe to call from updateVisibility AND the timer's new-data blocks so
// an engine switch, a bake, or a neural gen each land on the right mode + label.
juce::Rectangle<int> SynthPanel::lroCardBounds() const
{
    const float f = fs();
    return waveformDisplay.getBounds().reduced(juce::roundToInt(f * 1.2f),
                                               juce::roundToInt(f * 0.3f));
}

void SynthPanel::layoutLroKnobs()
{
    lroColumnBounds.clearQuick();

    const bool showing = engineModeHidden.getSelectedId() == EngineMode::Csound + 1
                      && (! lroControls.isEmpty() || ! lroControls.layers.empty());
    if (! showing)
    {
        // Off-screen rather than merely hidden: a hidden component still owns
        // its bounds, and the next mode switch would flash them before the next
        // resized() ran.
        for (auto& r : lroKnobRows)
            if (r != nullptr)
                r->setBounds(-1000, -1000, 10, 10);
        for (auto& r : lroLevelRows)
            if (r != nullptr)
                r->setBounds(-1000, -1000, 10, 10);
        return;
    }

    const float f = fs();
    auto card = lroCardBounds();
    // The author's own READING across the card. Kept, not recomputed in the
    // paint pass: the two used to be derived independently from the same
    // expressions and drifted into each other.
    const float capFs  = juce::jlimit(11.0f, 14.0f, f * 0.85f);
    const float partFs = juce::jlimit(10.0f, 13.0f, f * 0.8f);
    lroCaptionBounds = card.removeFromTop(juce::roundToInt(capFs * 1.7f));

    const int nCols = juce::jmax(1, static_cast<int>(lroControls.parts.size()));
    // Columns divide the card evenly and a single part uses the whole width:
    // three narrow columns with two empty thirds beside them would be a grid
    // pretending the instrument has parts it does not have.
    const int colW = card.getWidth() / nCols;
    const int gutter = juce::roundToInt(f * 0.8f);

    // Rows are the panel's own row height, but never taller than the card can
    // hold: four declared knobs must still fit, since four is what the contract
    // allows, plus the band of layer levels above them when there is one. The
    // part-name strip is taken out FIRST — it is text, not a row, and dividing
    // the card as though it were pushed the fourth knob off the bottom of the
    // smallest window. The label column is forced to the SAME width in every
    // column so the tracks line up across the grid — the rows are the same
    // component the rest of the synth uses, and this is the only thing that has
    // to be said about them here.
    const int nLayers  = static_cast<int>(lroControls.layers.size());
    const int nameH    = juce::roundToInt(partFs * 1.6f);
    const int rowsHigh = 4 + (nLayers > 0 ? 1 : 0);
    const int rowH = juce::jlimit(juce::roundToInt(f * 0.9f), juce::roundToInt(f * 1.5f),
                                  (card.getHeight() - nameH) / rowsHigh);

    // ── the layer levels, ACROSS the card and not inside a column ──
    // They cannot sit in a column, because a column is a library entry and a
    // layer is a `kvolN` the body reads, and neither decides the other: an
    // `a > b` transition shares `kvol1` across two entries, two layers can come
    // out of one entry. Placing one level per column drove `kvol2` from a
    // heading that had nothing to do with it. So the levels stand together,
    // above the columns, in the instrument's own layer order.
    if (nLayers > 0)
    {
        auto band = card.removeFromTop(rowH);
        const int cellW = band.getWidth() / nLayers;
        for (int i = 0; i < nLayers; ++i)
        {
            const int layer = lroControls.layers[static_cast<size_t>(i)];
            if (layer < 1 || layer > kNumLroLevels
                || lroLevelRows[static_cast<size_t>(layer - 1)] == nullptr)
                continue;
            auto cell = (i + 1 == nLayers) ? band : band.removeFromLeft(cellW);
            lroLevelRows[static_cast<size_t>(layer - 1)]->setBounds(cell.reduced(gutter / 2, 0));
        }
    }

    // The part names sit directly over their columns, BELOW the level band, so
    // a heading stands on the knobs it names.
    lroPartNameBounds = card.removeFromTop(nameH);

    for (size_t c = 0; c < lroControls.parts.size(); ++c)
    {
        auto col = card.removeFromLeft(c + 1 == lroControls.parts.size()
                                       ? card.getWidth() : colW);
        lroColumnBounds.add(col);
        auto inner = col.reduced(gutter / 2, 0);

        const auto knobs = lroControls.knobsOf(lroControls.parts[c].number);
        for (const auto& k : knobs)
        {
            // static_cast before the subtraction, not after. `juce::String::operator[]`
            // returns `juce_wchar`, which is a SIGNED 32-bit wchar_t on macOS and
            // Linux and `juce::uint32` on Windows — so `k.slot[0] - 'a'` is `int`
            // there and `unsigned int` here, `jmax(0, …)` cannot deduce one type, and
            // MSVC refuses the whole line. Unsigned would also have made the `jmax`
            // clamp dead: a slot below 'a' would wrap to a huge positive instead of
            // going negative.
            const int idx = (k.part - 1) * 4
                          + juce::jmax(0, static_cast<int>(k.slot[0]) - 'a');
            if (idx < 0 || idx >= kNumLroKnobs || lroKnobRows[static_cast<size_t>(idx)] == nullptr)
                continue;
            auto& row = *lroKnobRows[static_cast<size_t>(idx)];
            row.setBounds(inner.removeFromTop(rowH));
        }
    }
}

void SynthPanel::reconcileWaveformDisplayMode()
{
    // Fan for EVERY wavetable (DCO/LCO or neural), gated on "wavetable mode AND a
    // published table strip exists" rather than DCO-only. The strip persists in the
    // processor across editor lifetimes, so this also re-adopts the fan when the
    // display was reset (editor reopen, or a Wavetable→other→Wavetable round trip):
    // re-arm the one-shot publish and the timer reloads it next tick.
    const bool showWtTable = processorRef.isWavetableMode()
                          && processorRef.getWtDisplaySnapshot().getNumSamples() > 0;
    if (! showWtTable && waveformDisplay.isWavetableMode())
        waveformDisplay.exitWavetableMode();   // left the table → restore sample view + brackets
    else if (showWtTable && ! waveformDisplay.isWavetableMode())
        processorRef.republishWtDisplayIfActive();   // display lost the fan → re-adopt it
    // LRO (Csound): no baked table, no loaded sample — "Loop interval" would
    // name a Sampler concept a live-rendered voice doesn't have.
    const bool isCsoundEngine = engineModeHidden.getSelectedId() == EngineMode::Csound + 1;
    // The library list is PAINTED OVER this widget (paintOverChildren), but the
    // widget underneath still holds the last sampler buffer and its markers: a
    // click on the LRO card would drag P1/P2/P3 and move a loop nobody can see.
    waveformDisplay.setInert(isCsoundEngine);
    // HIDDEN, not merely masked, because the mask cannot stay above it. The
    // instrument's sliders are children of this panel, so they paint BETWEEN
    // paint() and paintOverChildren(): a mask drawn over the children to blank
    // this widget blanks the sliders with it, which is what it did — twelve rows
    // laid out, painted, and painted out again, leaving the captions over an
    // empty grid. So the widget goes away and the card's ground is drawn in
    // paint(), under the rows.
    waveformDisplay.setVisible(! isCsoundEngine);
    waveformDisplay.setRegionLabel(showWtTable        ? "Wavetable"
                                 : processorRef.isWavetableMode() ? "Extraction region"
                                 : processorRef.isFreezeMode()    ? "Granular position"
                                 : isCsoundEngine                 ? juce::String()
                                                                  : "Loop interval");
}

// ──────────────────────────────────────────────────────────────────────────────
// Visibility
// ──────────────────────────────────────────────────────────────────────────────
void SynthPanel::updateVisibility()
{
    if (!initialized) return;

    // All sections always visible — inactive ones get dimmed via alpha
    constexpr float dimAlpha = 0.3f;

    bool filterOn = (filterTypeBox.getSelectedId() > 1);  // 1=OFF, 2+=LP/HP/BP
    float filterAlpha = filterOn ? 1.0f : dimAlpha;
    for (int i = 0; i < kNumSlopeBtns; ++i)
    {
        filterSlopeBtns[i].setAlpha(filterAlpha);
        filterSlopeBtns[i].setEnabled(filterOn);
    }
    for (auto* r : { cutoffRow.get(), resoRow.get(), filterMixRow.get(),
                     kbdTrackRow.get(), filterDriveRow.get() })
    {
        r->setAlpha(filterAlpha);
        r->setEnabled(filterOn);
    }
    for (int i = 0; i < kNumAlgBtns; ++i)
    {
        // The algorithm buttons join the OFF segment as one 4-way switch:
        // always live (so any segment can re-enable the filter), and
        // highlighted only while the filter is on and this algorithm is picked.
        filterAlgBtns[i].setAlpha(1.0f);
        filterAlgBtns[i].setEnabled(true);
        filterAlgBtns[i].setToggleState(filterOn && filterAlgHidden.getSelectedId() == i + 1,
                                        juce::dontSendNotification);
    }
    // TYPE toggle: stays live like the algorithm buttons (so it can re-enable
    // the filter from bypass), lit only while the filter is on. Label set in onChange.

    // Warp Style dims further (to 0.3× of the already-filter-dim) when the
    // selected algorithm isn't Warp — style only applies to the warp ladder.
    const bool warpActive = filterAlgHidden.getSelectedId() == (FilterAlgorithm::Warp + 1);
    const float styleAlpha = filterAlpha * (warpActive ? 1.0f : 0.35f);
    filterWarpStyleBox.setAlpha(styleAlpha);
    filterWarpStyleBox.setEnabled(filterOn && warpActive);
    filterHeader.setVisible(true);
    // All four slopes are shown: the mode dropdown carries OFF/LP/HP/BP on its
    // own, so nothing has to borrow a cell from this row any more.
    for (int i = 0; i < kNumSlopeBtns; ++i)
        filterSlopeBtns[i].setVisible(true);
    for (int i = 0; i < kNumAlgBtns; ++i)
        filterAlgBtns[i].setVisible(i != FilterAlgorithm::Warp);
    filterEasyWarpBtn.setVisible(true);
    filterEasyWarpBtn.setAlpha(1.0f);
    filterEasyWarpBtn.setEnabled(true);
    filterEasyWarpBtn.setToggleState(filterOn && filterAlgHidden.getSelectedId() == FilterAlgorithm::Warp + 1,
                                     juce::dontSendNotification);
    filterWarpStyleBox.setVisible(false);

    filterTypeBox.setVisible(true);
    filterAlgBtns[FilterAlgorithm::SVF].setConnectedEdges(
        juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);

    const int engineId = engineModeHidden.getSelectedId();
    // LCO (id EngineMode::Lco+1 = 4) is a Wavetable bake — same control set
    // as plain Wavetable (see the engineModeHidden.onChange mapping above).
    bool isWavetable = engineId == 2 || engineId == EngineMode::Lco + 1;
    bool isFreeze = engineId == 3;
    bool isSampler = !isWavetable && !isFreeze;
    // Csound (id EngineMode::Csound+1 = 5) is not scan-driven (spec §2 item 8:
    // it's neither Wavetable nor Freeze), so it falls into the isSampler
    // bucket above for that purpose — but it is its OWN paradigm (LRO), not a
    // Sampler variant, and the panel below now says so.
    const bool isCsound = (engineId == EngineMode::Csound + 1);
    jassertquiet(!isCsound || isSampler);

    // LRO (Csound) is entered ONLY through MainPanel's oscModeToggle
    // (project_lco_mode_toggle_owns_engine) — never through these three
    // buttons. Leaving them live let a click set engineMode straight to
    // Sampler/Wavetable/Freeze while LRO was sounding, bypassing the toggle's
    // paradigm ownership (and its lcoEngineMode_/dcoPrevEngineMode_
    // bookkeeping) entirely — a real bypass, not just visual noise, so they
    // hide rather than sit unlit. isBufferSampler gates the controls that
    // reprocess a captured BUFFER (loop traversal, crossfade, HF boost,
    // normalize) — a live Csound voice renders per-sample straight from the
    // orchestra and has none of those (there is no buffer to loop, crossfade
    // or boost). Octave/tuning/filter/envelopes/LFO/drift/noise stay: all of
    // them demonstrably still drive a Csound voice (SynthVoice::noteOn/
    // glideToNote feed tunedHz(shiftedNote) into csoundFreq_; noise mix,
    // drive, filter and the VCA sit downstream of csoundBuf_[i] in the one
    // shared render tail, per SynthVoice.cpp).
    samplerBtn.setVisible(!isCsound);
    wavetableBtn.setVisible(!isCsound);
    freezeBtn.setVisible(!isCsound);
    const bool isBufferSampler = isSampler && !isCsound;

    // The authored instrument's knobs: which rows exist at all, and what each
    // one is called. Both come from the library parameters the body kept and
    // from nowhere else — a row without one is hidden, never labelled with
    // something plausible. The tooltip carries the library's own line on what
    // that parameter does.
    for (int i = 0; i < kNumLroKnobs; ++i)
    {
        auto& row = *lroKnobRows[static_cast<size_t>(i)];
        const juce::String want = juce::String(1 + i / 4) + juce::String::charToString(
                                      static_cast<juce::juce_wchar>("abcd"[i % 4]));
        const LroControls::Knob* found = nullptr;
        for (const auto& k : lroControls.knobs)
            if (juce::String(k.part) + k.slot == want)
            {
                found = &k;
                break;
            }
        row.setVisible(isCsound && found != nullptr);
        if (found != nullptr)
        {
            row.getLabel().setText(found->name, juce::dontSendNotification);
            row.getSlider().setTooltip(found->gloss);
        }
    }
    // A layer's level exists exactly as long as the layer does — and a layer is
    // a `kvolN` the BODY reads, not a column. A body that kept no library line
    // at all still has layers, and still gets their levels.
    for (int i = 0; i < kNumLroLevels; ++i)
    {
        const bool has = std::find(lroControls.layers.begin(), lroControls.layers.end(),
                                   i + 1) != lroControls.layers.end();
        auto& lvl = *lroLevelRows[static_cast<size_t>(i)];
        lvl.setVisible(isCsound && has);
        // Numbered only when the instrument has more than one — a single-layer
        // body has nothing to tell apart.
        lvl.getLabel().setText(lroControls.layers.size() > 1
                                   ? "Level " + juce::String(i + 1) : juce::String("Level"),
                               juce::dontSendNotification);
    }

    // Re-read the curated instruments on every panel update while the LRO is
    // showing. Not once, and not "until it is non-empty": the list changes
    // when BJ approves an instrument and tools/lco_build_library.py runs, and
    // a latch here held the old list until the editor was reopened — the panel
    // showing something other than what he has taken is the exact complaint
    // this list was rebuilt for (2026-07-31).
    //
    // Costs a handful of stats, not a parse: PipeInference memoises on the
    // file's path+timestamp+size and re-reads only when one of them moves. It
    // has to be that cheap, because this runs from resized() — every frame of
    // a window drag — and, rarely, from the timer via followModParamToTab().
    //
    // Nothing POLLS the library, deliberately: a rebuild during the session
    // reaches the card at the next panel event (engine switch, window resize,
    // any filter/env/LFO/drift control), and putting a file check on the 30 Hz
    // timer to save that click would be idle work of exactly the kind
    // docs/PERFORMANCE_GUIDE.md is about.
    if (isCsound)
    {
        if (auto pipePtr = processorRef.getPipeInferencePtr())
        {
            auto names = pipePtr->getCuratedInstrumentNames();
            if (names != lroInstrumentNames_)
                lroInstrumentNames_ = std::move(names);
        }
    }

    // A DCO/LCO table owns the oscillator: lock the engine away from Sampler/
    // Granular (the baked table is the sound) and pin the documented 256-frame
    // standard (grey the frame-count buttons — reextractWavetable is a no-op for a
    // baked table anyway). Wavetable stays selectable; released as soon as a neural
    // gen or re-slice clears the lock. updateVisibility re-runs at bake and restore.
    const bool dcoLock = isWavetable && processorRef.isDcoTableActive();
    samplerBtn.setEnabled(!dcoLock);
    freezeBtn.setEnabled(!dcoLock);
    samplerBtn.setAlpha(dcoLock ? 0.4f : 1.0f);
    freezeBtn.setAlpha(dcoLock ? 0.4f : 1.0f);
    for (int i = 0; i < kNumFrameBtns; ++i)
    {
        frameBtns[i].setEnabled(!dcoLock);
        frameBtns[i].setAlpha(dcoLock ? 0.4f : 1.0f);
    }

    // Shared playback traversal controls — a live Csound voice has no buffer
    // to traverse (host_owns_note_off: the orchestra has no decay time of its
    // own either), so these hide for LRO along with the buffer-only row below.
    oneshotBtn.setVisible(!isFreeze && !isCsound);
    loopModeBtn.setVisible(!isFreeze && !isCsound);
    pingpongBtn.setVisible(!isFreeze && !isCsound);

    // Sampler-only controls
    crossfadeRow->setVisible(isBufferSampler);
    loopOptimizeBtn.setVisible(isBufferSampler);
    normalizeToggle.setVisible(isBufferSampler || isFreeze);
    hfBoostBtn.setVisible(!isCsound);
    hfBoostBtn.setConnectedEdges(isWavetable ? 0 : juce::Button::ConnectedOnRight);
    normalizeToggle.setConnectedEdges(juce::Button::ConnectedOnLeft);
    // OCTAVE: shown in EVERY engine, because it acts in every engine —
    // SynthVoice::noteOn shifts the note before the pitch reaches the sampler,
    // the wavetable (SynthVoice.cpp:807), granular and the Csound voice alike.
    // WAVETABLE was the one mode that hid it (`isSampler` above is
    // !isWavetable && !isFreeze, so LRO was never in the hidden set), which
    // left the transpose live and unreachable there; and now that the buttons
    // sit in the shared top bar, hiding them would leave a hole exactly where
    // the same five stand in the other three modes.
    for (int i = 0; i < kNumOctBtns; ++i)
        octBtns[i].setVisible(true);

    // Scan controls drive Wavetable frame position or Granular hold position.
    scanRow->setVisible(isWavetable || isFreeze);
    scanHint.setVisible(false);
    for (int i = 0; i < kNumFrameBtns; ++i)
        frameBtns[i].setVisible(isWavetable);
    smoothToggle.setVisible(isWavetable);
    autoScanToggle.setVisible(isWavetable);
    frameCountLabel.setVisible(isWavetable);
    for (int i = 0; i < kNumFreezeTextureBtns; ++i)
        freezeTextureBtns[i].setVisible(isFreeze);
    freezeStereoRow->setVisible(isFreeze);

    reconcileWaveformDisplayMode();   // WT fan vs sample view + region label

    if (isBufferSampler)
    {
        bool isOneshot = loopModeHidden.getSelectedId() == 1;
        crossfadeRow->setAlpha(isOneshot ? dimAlpha : 1.0f);
        crossfadeRow->setEnabled(!isOneshot);
    }

    // Env-selector tab greys out for any env that drives nothing (target =
    // None) — inactive envs read as inactive at a glance. Alpha is orthogonal
    // to syncGroup's colour/toggle setup, so it survives.
    {
        EnvSection* envs[kNumModTabs] = { &ampEnv, &modEnvSections[0], &modEnvSections[1],
                                          &modEnvSections[2], &modEnvSections[3] };
        for (int i = 0; i < kNumModTabs; ++i)
            envTabBtns[static_cast<size_t>(i)].setAlpha(
                envs[i]->targetBox.getSelectedId() != 1 ? 1.0f : dimAlpha);
    }

    auto setLfoDimmed = [](LfoSection& lfo) {
        bool active = lfo.targetBox.getSelectedId() != 1; // 1 = "---"
        float alpha = active ? 1.0f : dimAlpha;
        lfo.header.setAlpha(1.0f);
        lfo.targetBox.setAlpha(alpha);
        lfo.waveBox.setAlpha(alpha);
        lfo.modeBtn.setAlpha(alpha);
        lfo.clockBtn.setAlpha(alpha);
        if (lfo.rateRow)     lfo.rateRow->setAlpha(alpha);
        if (lfo.depthRow)    lfo.depthRow->setAlpha(alpha);
        if (lfo.divisionRow) lfo.divisionRow->setAlpha(alpha);
    };
    setLfoDimmed(lfo1);
    setLfoDimmed(lfo2);
    setLfoDimmed(lfo3);

    auto setDriftDimmed = [](DriftSection& drift) {
        bool active = drift.targetBox.getSelectedId() != 1; // 1 = "---"
        float alpha = active ? 1.0f : dimAlpha;
        drift.header.setAlpha(1.0f);
        drift.targetBox.setAlpha(alpha);
        drift.waveBox.setAlpha(alpha);
        drift.clockBtn.setAlpha(alpha);
        if (drift.rateRow)     drift.rateRow->setAlpha(alpha);
        if (drift.depthRow)    drift.depthRow->setAlpha(alpha);
        if (drift.divisionRow) drift.divisionRow->setAlpha(alpha);
    };
    setDriftDimmed(drift1);
    setDriftDimmed(drift2);
    setDriftDimmed(drift3);

    driftHeader.setAlpha(1.0f);
    regenHeader.setAlpha(1.0f);
    for (int i = 0; i < kNumRegenBtns; ++i)
        regenBtns[i].setAlpha(1.0f);
    if (crossfadeRegenRow)
        crossfadeRegenRow->setAlpha(1.0f);

    for (auto& btn : envTabBtns)   btn.setVisible(true);
    for (auto& btn : lfoTabBtns)   btn.setVisible(false);
    for (auto& btn : driftTabBtns) btn.setVisible(false);

    auto setEnvControlsVisible = [](EnvSection& env, bool selected)
    {
        env.targetBox.setVisible(selected);
        env.loopBtn.setVisible(selected);
        env.copyBtn.setVisible(selected);
        env.pasteBtn.setVisible(selected);
        env.targetHeader.setVisible(selected);
        // Graph + "Velocity Amount" box: selected env only.
        if (env.graph) env.graph->setVisible(selected);
        env.velBox.setVisible(selected);
        for (auto* vb : { env.attVB.get(), env.decVB.get(), env.relVB.get(), env.levelVB.get() })
            if (vb) vb->setVisible(selected);
    };
    setEnvControlsVisible(ampEnv,  activeEnvTab == 0);
    for (int i = 0; i < kNumModEnvs; ++i)
        setEnvControlsVisible(modEnvSections[i], activeEnvTab == i + 1);

    auto setLfoControlsVisible = [](LfoSection& lfo)
    {
        const bool sync = lfo.clockModeHidden.getSelectedId() == 2;
        lfo.header.setVisible(true);
        lfo.targetBox.setVisible(true);
        lfo.waveBox.setVisible(true);
        for (auto& btn : lfo.waveBtns)
            btn.setVisible(false);
        lfo.modeBtn.setVisible(true);   // Free/Trig sits left of the sync clock
        for (auto& btn : lfo.modeBtns)
            btn.setVisible(false);
        lfo.clockBtn.setVisible(true);
        if (lfo.rateRow)     lfo.rateRow->setVisible(!sync);
        if (lfo.divisionRow) lfo.divisionRow->setVisible(sync);
        if (lfo.depthRow)    lfo.depthRow->setVisible(true);
    };
    setLfoControlsVisible(lfo1);
    setLfoControlsVisible(lfo2);
    setLfoControlsVisible(lfo3);
    lfoHeader.setVisible(false);

    // Per-target AT bars are the easy-panel module; the column header is
    // re-shown by the columns easy-layout (hidden in the stacked fallback).
    aftertouchHeader.setVisible(false);
    for (auto& bar : aftertouchBars)
        if (bar) bar->setVisible(true);
    for (auto& sw : exprSrcSwitches)
        if (sw) sw->setVisible(true);

    auto setDriftControlsVisible = [](DriftSection& drift)
    {
        const bool sync = drift.clockModeHidden.getSelectedId() == 2;
        drift.header.setVisible(true);
        drift.targetBox.setVisible(true);
        drift.waveBox.setVisible(true);
        for (auto& btn : drift.waveBtns)
            btn.setVisible(false);
        drift.clockBtn.setVisible(true);
        if (drift.rateRow)     drift.rateRow->setVisible(!sync);
        if (drift.divisionRow) drift.divisionRow->setVisible(sync);
        if (drift.depthRow)    drift.depthRow->setVisible(true);
    };
    setDriftControlsVisible(drift1);
    setDriftControlsVisible(drift2);
    setDriftControlsVisible(drift3);
    driftHeader.setVisible(false);
    regenHeader.setVisible(true);
    {
        // Default to the compact dropdown; layoutGenerateEasy upgrades to the
        // vertical "repeat every" switchbox when the column is tall enough.
        for (int i = 0; i < kNumRegenBtns; ++i)
            regenBtns[i].setVisible(false);
        regenHidden.setVisible(true);
    }
    if (crossfadeRegenRow)
        crossfadeRegenRow->setVisible(true);

    syncModTabButtons();
}

void SynthPanel::selectFirstActiveModTabs()
{
    auto pickLfo = [](const LfoSection& a, const LfoSection& b, const LfoSection& c)
    {
        if (a.targetBox.getSelectedId() > 1) return 0;
        if (b.targetBox.getSelectedId() > 1) return 1;
        if (c.targetBox.getSelectedId() > 1) return 2;
        return 0;
    };
    auto pickDrift = [](const DriftSection& a, const DriftSection& b, const DriftSection& c)
    {
        if (a.targetBox.getSelectedId() > 1) return 0;
        if (b.targetBox.getSelectedId() > 1) return 1;
        if (c.targetBox.getSelectedId() > 1) return 2;
        return 0;
    };

    activeEnvTab = 0;
    if (ampEnv.targetBox.getSelectedId() <= 1)
        for (int i = 0; i < kNumModEnvs; ++i)
            if (modEnvSections[i].targetBox.getSelectedId() > 1) { activeEnvTab = i + 1; break; }
    activeLfoTab = pickLfo(lfo1, lfo2, lfo3);
    activeDriftTab = pickDrift(drift1, drift2, drift3);
}

void SynthPanel::syncModTabButtons()
{
    auto syncGroup = [](std::array<juce::TextButton, kNumModTabs>& tabs,
                        int activeIndex,
                        juce::Colour accent,
                        int count)
    {
        for (int i = 0; i < count; ++i)
        {
            auto& btn = tabs[static_cast<size_t>(i)];
            const bool active = i == activeIndex;
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < count - 1) edges |= juce::Button::ConnectedOnRight;
            btn.setConnectedEdges(edges);
            btn.setToggleState(active, juce::dontSendNotification);
            // Selected-tab text follows the unified switchbox rule: dark ink on
            // bright accents (amber env/lfo), white on dark ones (drift) — same as
            // styleSwitchButton. (textColourOffId's active branch is dead since
            // active == toggled, so JUCE reads textColourOnId; kept symmetric.)
            const auto onTxt = switchBoxSelectedTextColour(accent);
            btn.setColour(juce::TextButton::buttonColourId, active ? accent : kSurface);
            btn.setColour(juce::TextButton::buttonOnColourId, accent);
            btn.setColour(juce::TextButton::textColourOffId, active ? onTxt : kDim);
            btn.setColour(juce::TextButton::textColourOnId, onTxt);
        }
    };

    syncGroup(envTabBtns, activeEnvTab, kModCol, kNumModTabs);
    syncGroup(lfoTabBtns, activeLfoTab, kLfoCol, kNumLfoTabs);
    syncGroup(driftTabBtns, activeDriftTab, kDriftCol, kNumLfoTabs);
}

float SynthPanel::fs() const
{
    // Derive font size from available height so all content fits.
    // Content budget: ~53 f-units (27 rows * 1.4 + headers + gaps)
    // plus waveform at 12% of panel height.
    float h = static_cast<float>(getHeight());
    float padY = h * 0.005f;
    float available = h - 2.0f * padY;
    float waveform = h * 0.08f;
    float remaining = available - waveform;
    float maxF = remaining / 57.5f;
    return juce::jlimit(9.0f, 20.0f, maxF);
}

template <size_t N>
static int choiceBoxWidthFor(const ChoiceEntry (&entries)[N], float f, int fallbackWidth)
{
    const float fontSize = juce::jmax(kUiControlFontMin, juce::jmin(f, 13.0f));
    int maxTextWidth = 0;
    for (const auto& e : entries)
        maxTextWidth = juce::jmax(maxTextWidth,
                                  measureTextWidth(juce::String(juce::CharPointer_UTF8(e.label)), fontSize));

    return juce::jmax(fallbackWidth, maxTextWidth + juce::roundToInt(fontSize * 2.8f));
}

// ──────────────────────────────────────────────────────────────────────────────
// Layout helpers
// ──────────────────────────────────────────────────────────────────────────────
template <size_t N>
static void layoutModTabStrip(std::array<juce::TextButton, N>& tabs,
                               juce::Rectangle<int> area,
                               juce::Rectangle<int>& switchBounds)
{
    const int count = static_cast<int>(tabs.size());
    const int cellW = area.getWidth() / count;
    switchBounds = {};
    for (int i = 0; i < count; ++i)
    {
        auto cell = (i == count - 1)
            ? area
            : area.removeFromLeft(cellW);
        tabs[static_cast<size_t>(i)].setBounds(cell);
        switchBounds = switchBounds.isEmpty() ? cell : switchBounds.getUnion(cell);
    }
}

template <size_t N>
static void layoutSegmentedButtons(std::array<juce::TextButton, N>& buttons,
                                   juce::Rectangle<int> area,
                                   juce::Rectangle<int>& switchBounds,
                                   bool vertical)
{
    const int count = static_cast<int>(buttons.size());
    const int cellSize = vertical ? area.getHeight() / count
                                  : area.getWidth() / count;
    switchBounds = {};

    for (int i = 0; i < count; ++i)
    {
        auto cell = (i == count - 1)
            ? area
            : (vertical ? area.removeFromTop(cellSize)
                        : area.removeFromLeft(cellSize));
        auto& btn = buttons[static_cast<size_t>(i)];
        int edges = 0;
        if (vertical)
        {
            if (i > 0) edges |= juce::Button::ConnectedOnTop;
            if (i < count - 1) edges |= juce::Button::ConnectedOnBottom;
        }
        else
        {
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < count - 1) edges |= juce::Button::ConnectedOnRight;
        }

        btn.setConnectedEdges(edges);
        btn.setBounds(cell);
        switchBounds = switchBounds.isEmpty() ? cell : switchBounds.getUnion(cell);
    }
}

void SynthPanel::layoutFilterEasy(juce::Rectangle<int> area, float f, int rowH, int gap)
{
    for (auto* r : { cutoffRow.get(), resoRow.get(), filterDriveRow.get(),
                     kbdTrackRow.get(), filterMixRow.get() })
        if (r)
        {
            r->clearForcedLabelWidth();
            r->clearForcedValueWidth();
            r->setKnobMode(true);
        }

    cutoffRow->getLabel().setText("Cutoff", juce::dontSendNotification);
    resoRow->getLabel().setText("Reso", juce::dontSendNotification);
    filterDriveRow->getLabel().setText("Drive", juce::dontSendNotification);
    kbdTrackRow->getLabel().setText("Kbd", juce::dontSendNotification);
    filterMixRow->getLabel().setText("Mix", juce::dontSendNotification);

    const int rowGap = juce::jmax(gap, 6);

    auto layoutButtons = [](auto& buttons, int count, juce::Rectangle<int> row,
                            juce::Rectangle<int>& switchBounds)
    {
        const int cellW = row.getWidth() / count;
        switchBounds = {};
        for (int i = 0; i < count; ++i)
        {
            auto cell = (i == count - 1) ? row : row.removeFromLeft(cellW);
            buttons[i].setBounds(cell);
            switchBounds = switchBounds.isEmpty() ? cell : switchBounds.getUnion(cell);
        }
    };

    filterAlgBtns[FilterAlgorithm::SVF].setButtonText("SVF");
    filterAlgBtns[FilterAlgorithm::Ladder].setButtonText("Ladder");
    // filterAlgBtns[Warp] is hidden in Easy; WarpHoldBtn occupies its cell.
    {
        // Four equal cells: [mode dropdown] | SVF | Ladder | [WarpHoldBtn]. The
        // dropdown carries OFF, so the algorithm switch begins at SVF and the
        // frame is drawn around those three alone.
        auto algRow = area.removeFromTop(rowH);
        const int cellW = algRow.getWidth() / (kNumAlgBtns + 1);
        filterTypeBox.setBounds(algRow.removeFromLeft(cellW).reduced(0, 1));
        filterAlgBtns[FilterAlgorithm::SVF].setBounds(algRow.removeFromLeft(cellW));
        filterAlgBtns[FilterAlgorithm::Ladder].setBounds(algRow.removeFromLeft(cellW));
        filterEasyWarpBtn.setBounds(algRow);           // remaining cell
        filterAlgSwitchBounds = filterAlgBtns[FilterAlgorithm::SVF].getBounds()
            .getUnion(filterEasyWarpBtn.getBounds());
    }
    area.removeFromTop(rowGap);
    {
        // Slope is a 4-way switch across four equal cells: 6 / 12 / 18 / 24. The
        // 18 dB segment used to be dropped so the LP/HP/BP toggle could stand in
        // its cell; the mode dropdown carries the type now, so it has it back.
        auto slopeRow = area.removeFromTop(rowH);
        const int cellW = slopeRow.getWidth() / kNumSlopeBtns;
        filterSlopeSwitchBounds = {};
        for (int i = 0; i < kNumSlopeBtns; ++i)
        {
            auto cell = (i == kNumSlopeBtns - 1) ? slopeRow : slopeRow.removeFromLeft(cellW);
            filterSlopeBtns[i].setBounds(cell);
            filterSlopeSwitchBounds = filterSlopeSwitchBounds.isEmpty()
                                          ? cell : filterSlopeSwitchBounds.getUnion(cell);
        }
    }
    area.removeFromTop(rowGap);

    auto knobArea = area;
    const int knobGap = juce::jmax(8, juce::roundToInt(f * 0.65f));
    const int desiredSmallH = juce::jmax(rowH * 4, juce::roundToInt(f * 6.6f));
    const int minCutoffH = juce::jmax(rowH * 6, juce::roundToInt(f * 9.5f));
    const int maxCutoffH = juce::roundToInt(static_cast<float>(desiredSmallH) * 2.0f);
    const int availableH = knobArea.getHeight();
    int cutoffH = juce::jlimit(minCutoffH,
                               juce::jmax(minCutoffH, maxCutoffH),
                               availableH - desiredSmallH * 2 - knobGap * 2);
    if (cutoffH < minCutoffH || availableH - cutoffH - knobGap * 2 < desiredSmallH * 2)
        cutoffH = juce::jmin(maxCutoffH,
                             juce::jmax(minCutoffH, availableH - desiredSmallH * 2 - knobGap * 2));

    const int remainingRowsH = juce::jmax(0, availableH - cutoffH - knobGap * 2);
    const int row1H = remainingRowsH / 2;
    const int row2H = remainingRowsH - row1H;

    auto cutoffArea = knobArea.removeFromTop(juce::jmin(cutoffH, knobArea.getHeight()));
    const int cutoffW = juce::jmin(cutoffArea.getWidth(),
                                   juce::jmax(rowH * 5, juce::roundToInt(static_cast<float>(cutoffArea.getWidth()) * 0.72f)));
    cutoffRow->setBounds(juce::Rectangle<int>(cutoffW, cutoffArea.getHeight()).withCentre(cutoffArea.getCentre()));

    knobArea.removeFromTop(knobGap);
    auto row1 = knobArea.removeFromTop(row1H);
    knobArea.removeFromTop(knobGap);
    auto row2 = knobArea.removeFromTop(juce::jmin(row2H, knobArea.getHeight()));

    auto placePair = [&](SliderRow& left, SliderRow& right, juce::Rectangle<int> row)
    {
        const int cellW = juce::jmax(1, (row.getWidth() - knobGap) / 2);
        auto leftCell = row.removeFromLeft(cellW);
        row.removeFromLeft(knobGap);
        left.setBounds(leftCell);
        right.setBounds(row);
    };

    placePair(*resoRow, *filterDriveRow, row1);
    placePair(*kbdTrackRow, *filterMixRow, row2);
}

void SynthPanel::layoutEnvEasy(EnvSection& env, juce::Rectangle<int> area, float f, int rowH, int gap)
{
    // Top row: [Target left-header][short dropdown]
    auto targetRow = area.removeFromTop(rowH);
    const float headerFs = juce::jmax(kUiControlFontMin, juce::jmin(13.0f, static_cast<float>(rowH) * 0.58f));
    env.targetHeader.setFont(juce::FontOptions(headerFs, juce::Font::bold));
    const int targetHdrW = measureTextWidth("Target", headerFs) + 16;
    env.targetHeader.setBounds(targetRow.removeFromLeft(juce::jmin(targetHdrW, targetRow.getWidth())));
    targetRow.removeFromLeft(gap);
    // LOOP | COPY | PASTE, right-aligned and reserved BEFORE the target box, so
    // the three shrink together instead of dropping off one by one as the
    // envelope column narrows — a PASTE button that quietly went missing at
    // small widths would be worse than three tight ones. The target box keeps
    // at least f*4 and takes whatever is left.
    const int tgtW = choiceBoxWidthFor(EnvTarget::kEntries, f, juce::roundToInt(f * 7.0f));
    {
        // What is left once the Target box has the width its own labels need:
        // the three shrink together into it, so PASTE cannot quietly go missing
        // at small widths AND the longest target label cannot be clipped.
        const int trioW = juce::jlimit(0, juce::roundToInt(f * 3.2f) * 3,
                                       targetRow.getWidth() - juce::jmin(tgtW, targetRow.getWidth()));
        auto trio = targetRow.removeFromRight(trioW);
        const int cellW = trio.getWidth() / 3;
        env.loopBtn .setBounds(trio.removeFromLeft(cellW).reduced(1));
        env.copyBtn .setBounds(trio.removeFromLeft(cellW).reduced(1));
        env.pasteBtn.setBounds(trio.reduced(1));
    }
    env.targetBox.setBounds(targetRow.removeFromLeft(juce::jmin(tgtW, targetRow.getWidth())));
    area.removeFromTop(gap);

    const int rowGap = juce::jmax(gap, 6);

    // Split the full remaining height: the ADSR graph is the dominant hero
    // (~2/3); the "Velocity Amount" box takes the secondary third.
    // One gap between them; minimums keep both usable when the column is short.
    const int avail     = juce::jmax(0, area.getHeight() - rowGap);
    const int minGraphH = juce::jmax(rowH * 3, juce::roundToInt(f * 5.0f));
    const int minVelH   = juce::jmax(rowH * 3, juce::roundToInt(f * 4.5f));  // title + bars
    const int loGraph   = juce::jmin(minGraphH, avail);
    const int hiGraph   = juce::jmax(loGraph, avail - juce::jmin(minVelH, avail));
    const int graphH    = juce::jlimit(loGraph, hiGraph,
                                       juce::roundToInt(static_cast<float>(avail) * 0.60f));
    auto graphArea = area.removeFromTop(juce::jmin(graphH, area.getHeight()));
    if (env.graph) env.graph->setBounds(graphArea);
    area.removeFromTop(rowGap);

    // "Velocity Amount" ModuleBox: accent top-header (same template as DURATION/
    // RE-PROMPT/RESYNTH) framing four vertical drag-fill bars (Att/Dec/Rel =
    // bipolar velocity→stage TIME; Level = the global velocity→peak amount,
    // velAmt — last, set slightly apart as the sum attenuation).
    {
        const int headerH = juce::jlimit(12, rowH, juce::roundToInt(f * 1.4f));
        env.velBox.setBaseFont(f);
        env.velBox.setHeaderHeight(headerH);
        env.velBox.setContentPadding(juce::jmax(3, juce::roundToInt(f * 0.3f)));
        env.velBox.setBounds(area);
        auto vsArea = env.velBox.getContentBounds();   // PARENT-relative

        VelocityBar* vb[4] = { env.attVB.get(), env.decVB.get(), env.relVB.get(), env.levelVB.get() };
        constexpr int vGap = 4;
        constexpr int levelGap = 8;   // wider gap before Level — it is the sum attenuation
        const int vW = juce::jmax(1, (vsArea.getWidth() - vGap * 2 - levelGap) / 4);
        for (int i = 0; i < 4; ++i)
        {
            auto cell = (i == 3) ? vsArea : vsArea.removeFromLeft(vW);
            if (i < 3) vsArea.removeFromLeft(i == 2 ? levelGap : vGap);
            if (vb[i]) vb[i]->setBounds(cell);
        }
    }
}

void SynthPanel::layoutLfoEasy(LfoSection& lfo, juce::Rectangle<int> area, float f, int rowH, int gap)
{
    for (auto* r : { lfo.rateRow.get(), lfo.depthRow.get(), lfo.divisionRow.get() })
        if (r)
        {
            r->clearForcedLabelWidth();
            r->clearForcedValueWidth();
            r->setKnobMode(true);
        }

    lfo.rateRow->getLabel().setText("Rate", juce::dontSendNotification);
    lfo.divisionRow->getLabel().setText("Rate", juce::dontSendNotification);
    lfo.depthRow->getLabel().setText("Amt", juce::dontSendNotification);

    const int rowGap = juce::jmax(gap, 6);
    const int controlGap = juce::jmax(7, juce::roundToInt(f * 0.55f));
    const float topFontSize = juce::jmax(kUiControlFontMin, juce::jmin(13.0f, static_cast<float>(rowH) * 0.58f));

    const int syncNudge = juce::jmax(2, juce::roundToInt(f * 0.18f));

    auto headerRow = area.removeFromTop(rowH);
    const int headerW = juce::jmax(54, measureTextWidth(lfo.header.getText(), topFontSize) + 18);
    lfo.header.setFont(juce::FontOptions(topFontSize, juce::Font::bold));
    lfo.header.setJustificationType(juce::Justification::centred);
    // Header band: light text on accent@0.7 — same treatment as every other
    // module/section header (see labelAsHeaderBand). Replaces the old opaque
    // amber chip whose brightness-ink rule disagreed with Drift's white.
    labelAsHeaderBand(lfo.header, kLfoCol);
    lfo.header.setBounds(headerRow.removeFromLeft(juce::jmin(headerW, headerRow.getWidth())));
    // Free/Trig padlock sits at the header's right edge — directly above the sync
    // clock — so the control row below stays a clean [target][wave][clock], pixel-
    // identical to Drift. (Packed into the control row it was a 4th item that
    // overcrowded the column and shoved the sync clock against/past the frame.)
    const int lockW = juce::jmin(rowH, headerRow.getWidth());
    if (lockW > 0)
        lfo.modeBtn.setBounds(headerRow.removeFromRight(lockW).translated(-syncNudge, 0));
    lfo.modeHidden.onChange();
    area.removeFromTop(rowGap);

    auto top = area.removeFromTop(rowH);
    const int targetW = choiceBoxWidthFor(LfoTarget::kEntries, f, juce::roundToInt(f * 8.4f));
    const int waveW = choiceBoxWidthFor(LfoWave::kEntries, f, juce::roundToInt(f * 4.8f));
    const int syncW = rowH;

    const std::vector<ResponsiveStripItem> rowItems {
        { targetW, juce::roundToInt(f * 5.4f), 0, true, ResponsiveStripFallback::none },
        { waveW,   juce::roundToInt(f * 3.7f), 0, false, ResponsiveStripFallback::none },
        { syncW,   rowH, 0, false, ResponsiveStripFallback::none }
    };
    auto rowLayout = layoutResponsiveStrip(top, rowItems, controlGap);

    lfo.targetBox.setBounds(rowLayout.bounds[0]);
    lfo.waveBox.setBounds(rowLayout.bounds[1]);
    lfo.clockBtn.setBounds(rowLayout.bounds[2].translated(-syncNudge, 0));
    lfo.waveSwitchBounds = {};
    lfo.modeSwitchBounds = {};

    area.removeFromTop(rowGap);

    auto knobs = area;
    const int knobGap = juce::jmax(12, juce::roundToInt(f * 0.95f));
    const int cellW = juce::jmax(1, (knobs.getWidth() - knobGap) / 2);
    auto rateCell = knobs.removeFromLeft(cellW);
    knobs.removeFromLeft(knobGap);
    auto amtCell = knobs;

    lfo.rateRow->setBounds(rateCell);
    lfo.divisionRow->setBounds(rateCell);
    lfo.depthRow->setBounds(amtCell);
}

void SynthPanel::layoutDriftEasy(DriftSection& drift, juce::Rectangle<int> area, float f, int rowH, int gap)
{
    for (auto* r : { drift.rateRow.get(), drift.depthRow.get(), drift.divisionRow.get() })
        if (r)
        {
            r->clearForcedLabelWidth();
            r->clearForcedValueWidth();
            r->setKnobMode(true);
        }

    drift.rateRow->getLabel().setText("Rate", juce::dontSendNotification);
    drift.divisionRow->getLabel().setText("Rate", juce::dontSendNotification);
    drift.depthRow->getLabel().setText("Amt", juce::dontSendNotification);

    const int rowGap = juce::jmax(gap, 6);
    const int controlGap = juce::jmax(7, juce::roundToInt(f * 0.55f));
    const float topFontSize = juce::jmax(kUiControlFontMin, juce::jmin(13.0f, static_cast<float>(rowH) * 0.58f));

    const juce::String title = &drift == &drift2 ? "DRIFT 2" : (&drift == &drift3 ? "DRIFT 3" : "DRIFT 1");
    auto headerRow = area.removeFromTop(rowH);
    const int headerW = juce::jmax(68, measureTextWidth(title, topFontSize) + 18);
    drift.header.setText(title, juce::dontSendNotification);
    drift.header.setFont(juce::FontOptions(topFontSize, juce::Font::bold));
    drift.header.setJustificationType(juce::Justification::centred);
    // Header band: light text on accent@0.7 (see labelAsHeaderBand).
    labelAsHeaderBand(drift.header, kDriftCol);
    drift.header.setBounds(headerRow.removeFromLeft(juce::jmin(headerW, headerRow.getWidth())));
    area.removeFromTop(rowGap);

    auto top = area.removeFromTop(rowH);
    const int targetW = choiceBoxWidthFor(DriftTarget::kEntries, f, juce::roundToInt(f * 8.4f));
    const int waveW = choiceBoxWidthFor(DriftWave::kEntries, f, juce::roundToInt(f * 4.8f));
    const int syncW = rowH;

    const std::vector<ResponsiveStripItem> rowItems {
        { targetW, juce::roundToInt(f * 5.4f), 0, true, ResponsiveStripFallback::none },
        { waveW,   juce::roundToInt(f * 3.7f), 0, false, ResponsiveStripFallback::none },
        { syncW,   rowH, 0, false, ResponsiveStripFallback::none }
    };
    auto rowLayout = layoutResponsiveStrip(top, rowItems, controlGap);

    drift.targetBox.setBounds(rowLayout.bounds[0]);
    drift.waveBox.setBounds(rowLayout.bounds[1]);
    const int syncNudge = juce::jmax(2, juce::roundToInt(f * 0.18f));
    drift.clockBtn.setBounds(rowLayout.bounds[2].translated(-syncNudge, 0));
    drift.waveSwitchBounds = {};

    area.removeFromTop(rowGap);

    auto knobs = area;
    const int knobGap = juce::jmax(12, juce::roundToInt(f * 0.95f));
    const int cellW = juce::jmax(1, (knobs.getWidth() - knobGap) / 2);
    auto rateCell = knobs.removeFromLeft(cellW);
    knobs.removeFromLeft(knobGap);
    auto amtCell = knobs;

    drift.rateRow->setBounds(rateCell);
    drift.divisionRow->setBounds(rateCell);
    drift.depthRow->setBounds(amtCell);
}

void SynthPanel::layoutAftertouchEasy(juce::Rectangle<int> area)
{
    const int n = (int) aftertouchBars.size();
    if (n <= 0 || area.isEmpty())
        return;

    // Equal stacked rows; each cell paints its own kBorder edge so neighbours
    // share a 1px divider (no gap). Last row absorbs the rounding remainder.
    //
    // A row is amount THEN source, left to right: the bar carries the depth and
    // the small box on its right says which axis drives it. One letter wide —
    // the axis is a footnote on a depth, not a control of equal weight — and
    // floored at 14 px, below which the letter stops being readable.
    const int rowH = juce::jmax(1, area.getHeight() / n);
    const int switchW = juce::jlimit(14, 24, juce::roundToInt((float) rowH * 0.7f));
    for (int i = 0; i < n; ++i)
    {
        auto row = (i == n - 1) ? area : area.removeFromTop(rowH);
        // Never let the switch eat the bar: on a very narrow column the depth is
        // the control that has to survive, so it keeps at least half the width.
        auto sw = row.removeFromRight(juce::jmin(switchW, row.getWidth() / 2));
        if (aftertouchBars[i])
            aftertouchBars[i]->setBounds(row);
        if (exprSrcSwitches[i])
            exprSrcSwitches[i]->setBounds(sw);
    }
}

void SynthPanel::layoutGenerateEasy(juce::Rectangle<int> area, float f, int rowH, int gap, bool ownHeader)
{
    const int rowGap = juce::jmax(gap, 6);
    const float chipFontSize = juce::jmax(kUiControlFontMin,
                                          juce::jmin(13.0f, static_cast<float>(rowH) * 0.58f));

    // Header chip "REGENERATE" — drift-orange (subordinated to drift). In the
    // columnar mod view this column already has an aligned REGENERATE header bar
    // above it (layoutModEasy), so the in-column chip is skipped (ownHeader=false).
    if (ownHeader)
    {
        auto headerRow = area.removeFromTop(juce::jmin(rowH, area.getHeight()));
        const int headerW = juce::jmax(72, measureTextWidth("REGENERATE", chipFontSize) + 18);
        regenHeader.setText(" REGENERATE", juce::dontSendNotification);
        regenHeader.setFont(juce::FontOptions(chipFontSize, juce::Font::bold));
        regenHeader.setJustificationType(juce::Justification::centred);
        // Header band: light text on accent@0.7 (see labelAsHeaderBand).
        labelAsHeaderBand(regenHeader, kRegenCol);
        regenHeader.setBounds(headerRow.removeFromLeft(juce::jmin(headerW, headerRow.getWidth())));
        area.removeFromTop(rowGap);
    }

    // Vertical 7-way switchbox replaces the dropdown when the column is tall
    // enough; otherwise fall back to the compact dropdown. The "REGENERATE"
    // chip above already names the control, so there is no caption row.
    const int segGap   = 1;
    const int btnH     = juce::jmax(11, juce::roundToInt(rowH * 0.66f));
    const int switchH  = btnH * kNumRegenBtns + segGap * (kNumRegenBtns - 1);
    const int xfadeMin = juce::roundToInt(rowH * 2.2f);   // keep the slider usable
    const bool useSwitch = area.getHeight() >= switchH + rowGap + xfadeMin;

    if (useSwitch)
    {
        regenHidden.setVisible(false);

        auto switchCol = area.removeFromTop(juce::jmin(switchH, area.getHeight()));
        const int segH = juce::jmax(1,
            (switchCol.getHeight() - segGap * (kNumRegenBtns - 1)) / kNumRegenBtns);
        for (int i = 0; i < kNumRegenBtns; ++i)
        {
            int edges = 0;
            if (i > 0)                 edges |= juce::Button::ConnectedOnTop;
            if (i < kNumRegenBtns - 1) edges |= juce::Button::ConnectedOnBottom;
            regenBtns[i].setConnectedEdges(edges);
            regenBtns[i].setVisible(true);
            regenBtns[i].setBounds(switchCol.removeFromTop(juce::jmin(segH, switchCol.getHeight())));
            if (i < kNumRegenBtns - 1)
                switchCol.removeFromTop(juce::jmin(segGap, switchCol.getHeight()));
        }
        regenSwitchBounds = regenBtns[0].getBounds()
            .getUnion(regenBtns[kNumRegenBtns - 1].getBounds());
        area.removeFromTop(rowGap);
    }
    else
    {
        for (int i = 0; i < kNumRegenBtns; ++i)
            regenBtns[i].setVisible(false);
        regenHidden.setVisible(true);
        auto comboRow = area.removeFromTop(juce::jmin(rowH, area.getHeight()));
        regenHidden.setBounds(comboRow);
        area.removeFromTop(rowGap);
        regenSwitchBounds = {};
    }

    // Vertical XFade slider fills the remaining column height.
    if (crossfadeRegenRow)
    {
        crossfadeRegenRow->clearForcedLabelWidth();
        crossfadeRegenRow->clearForcedValueWidth();
        crossfadeRegenRow->setVerticalMode(true);
        crossfadeRegenRow->getLabel().setText("XFade", juce::dontSendNotification);
        crossfadeRegenRow->setBounds(area);
    }
}

void SynthPanel::layoutModEasy(juce::Rectangle<int>& area, juce::Rectangle<int> modHeaderRow, float f, int rowH, int gap, int headerH, float headerFs)
{
    juce::ignoreUnused(headerH);

    auto* env = (activeEnvTab >= 1 && activeEnvTab <= kNumModEnvs)
                  ? &modEnvSections[activeEnvTab - 1] : &ampEnv;

    const int tabH = rowH;
    const int blockGap = juce::jmax(7, juce::roundToInt(f * 0.65f));
    const bool columns = area.getWidth() >= juce::roundToInt(f * 46.0f);
    const int contentInset = juce::jmax(4, juce::roundToInt(f * 0.35f));
    int easyModStackWidth = 0;

    filterHeader.setText(" FILTER", juce::dontSendNotification);
    filterHeader.setFont(juce::FontOptions(headerFs));
    labelAsHeaderBand(filterHeader, kFilterCol);
    filterHeader.setJustificationType(juce::Justification::centredLeft);

    auto layoutBlock = [&](std::array<juce::TextButton, kNumModTabs>& tabs,
                           juce::Rectangle<int>& tabBounds,
                           juce::Rectangle<int>& blockBounds,
                           juce::Rectangle<int> block,
                           auto&& layoutContent)
    {
        blockBounds = block;
        // Uniform padding inside the card. ENV was the only easy block insetting
        // its content horizontally only (reduced(contentInset, 0)); its siblings
        // (Filter/Generate) inset both axes, so ENV's controls + value read-outs
        // touched the top/bottom frame edges — the "falsche Randabstände" vs the
        // Duration card template.
        block.reduce(contentInset, contentInset);
        auto tabRow = block.removeFromTop(tabH);
        layoutModTabStrip(tabs, tabRow, tabBounds);
        block.removeFromTop(juce::jmax(gap * 2, 8));
        layoutContent(block);
    };

    auto layoutLfoStack = [&](juce::Rectangle<int> block)
    {
        if (columns && easyModStackWidth > 0)
            block.setWidth(juce::jmin(block.getWidth(), easyModStackWidth));

        lfoEasyBlockBounds = block;
        lfoTabSwitchBounds = {};

        auto content = block.reduced(contentInset, contentInset);
        const int moduleGap = juce::jmax(9, juce::roundToInt(f * 0.72f));
        const int availableH = juce::jmax(0, content.getHeight() - moduleGap * 2);
        const int moduleH = juce::jmax(rowH * 3, availableH / 3);
        LfoSection* lfos[] = { &lfo1, &lfo2, &lfo3 };

        for (int i = 0; i < 3; ++i)
        {
            auto module = (i == 2)
                ? content
                : content.removeFromTop(juce::jmin(moduleH, content.getHeight()));
            lfoEasyModuleBounds[static_cast<size_t>(i)] = module;
            layoutLfoEasy(*lfos[i], module, f, rowH, gap);

            if (i < 2)
                content.removeFromTop(moduleGap);
        }
    };

    auto layoutDriftStack = [&](juce::Rectangle<int> block)
    {
        driftEasyBlockBounds = block;
        driftTabSwitchBounds = {};

        auto content = block.reduced(contentInset, contentInset);
        const int moduleGap = juce::jmax(9, juce::roundToInt(f * 0.72f));
        const int availableH = juce::jmax(0, content.getHeight() - moduleGap * 2);
        const int moduleH = juce::jmax(rowH * 3, availableH / 3);
        DriftSection* drifts[] = { &drift1, &drift2, &drift3 };

        for (int i = 0; i < 3; ++i)
        {
            auto module = (i == 2)
                ? content
                : content.removeFromTop(juce::jmin(moduleH, content.getHeight()));
            driftEasyModuleBounds[static_cast<size_t>(i)] = module;
            layoutDriftEasy(*drifts[i], module, f, rowH, gap);

            if (i < 2)
                content.removeFromTop(moduleGap);
        }
    };

    if (columns)
    {
        const int blockW = area.getWidth();
        const int colGap = juce::jmax(22, juce::roundToInt(f * 1.7f));
        const int envW = juce::jlimit(juce::roundToInt(f * 16.0f),
                                      juce::roundToInt(f * 24.0f),
                                      juce::roundToInt(static_cast<float>(blockW) * 0.28f));

        const int previousDriftW = juce::jlimit(juce::roundToInt(f * 23.0f),
                                                juce::roundToInt(f * 34.0f),
                                                juce::roundToInt(static_cast<float>(blockW) * 0.28f));
        const int previousLfoAreaW = juce::jmax(0, blockW - previousDriftW - colGap);
        const int previousLfoW = previousLfoAreaW >= juce::roundToInt(f * 30.0f)
            ? juce::jlimit(juce::roundToInt(f * 23.0f),
                           juce::roundToInt(f * 34.0f),
                           previousLfoAreaW / 2)
            : previousLfoAreaW;
        easyModStackWidth = juce::jlimit(juce::roundToInt(f * 14.0f),
                                         juce::jmax(1, blockW),
                                         juce::roundToInt(static_cast<float>(previousLfoW) * 0.60f));

        // 6 columns: filter | env | lfo | drift | AT | generate. AT is a thin
        // column (~82% of a stack column), generate the narrowest (~55%). The
        // parts denominator 100+100+100+82+55=437 carves AT's width out of the
        // flexible columns, leaving env (envW) untouched.
        const int stackW = juce::jmin(easyModStackWidth,
                                      juce::jmax(1, (blockW - envW - colGap * 5) * 100 / 437));
        const int aftertouchW = juce::jmax(juce::roundToInt(f * 6.0f),
                                           juce::roundToInt(static_cast<float>(stackW) * 0.82f));
        const int generateW = juce::jmax(juce::roundToInt(f * 7.0f),
                                         juce::roundToInt(static_cast<float>(stackW) * 0.55f));

        // Per-module header bars, each aligned to its column below
        // (filter | env | lfo | drift | generate) — replaces the single "CONTROLS"
        // umbrella so every module names itself. Widths mirror the block row exactly.
        auto styleHeaderBar = [headerFs](juce::Label& l, const juce::String& t, juce::Colour c)
        {
            l.setText(t, juce::dontSendNotification);
            l.setFont(juce::FontOptions(headerFs));
            l.setJustificationType(juce::Justification::centredLeft);
            labelAsHeaderBand(l, c);
            l.setVisible(true);
        };
        filterHeader.setBounds(modHeaderRow.removeFromLeft(stackW));
        modHeaderRow.removeFromLeft(colGap);
        modHeader.setText(" ENVELOPES", juce::dontSendNotification);   // env column (bg/font already set in resized())
        modHeader.setBounds(modHeaderRow.removeFromLeft(envW));
        modHeaderRow.removeFromLeft(colGap);
        styleHeaderBar(lfoHeader, " LFO", kLfoCol);
        lfoHeader.setBounds(modHeaderRow.removeFromLeft(stackW));
        modHeaderRow.removeFromLeft(colGap);
        styleHeaderBar(driftHeader, " DRIFT", kDriftCol);
        driftHeader.setBounds(modHeaderRow.removeFromLeft(stackW));
        modHeaderRow.removeFromLeft(colGap);
        styleHeaderBar(aftertouchHeader, " Expression", kAtCol);
        aftertouchHeader.setBounds(modHeaderRow.removeFromLeft(aftertouchW));
        modHeaderRow.removeFromLeft(colGap);
        styleHeaderBar(regenHeader, " REGENERATE", kRegenCol);   // generate column (was an in-column chip)
        regenHeader.setBounds(modHeaderRow.removeFromLeft(juce::jmin(generateW, modHeaderRow.getWidth())));

        const int blockH = area.getHeight();
        auto block = area.removeFromTop(blockH);
        auto filterArea = block.removeFromLeft(stackW);
        block.removeFromLeft(colGap);
        auto envArea = block.removeFromLeft(envW);
        block.removeFromLeft(colGap);
        auto lfoArea = block.removeFromLeft(stackW);
        block.removeFromLeft(colGap);
        auto driftArea = block.removeFromLeft(stackW);
        block.removeFromLeft(colGap);
        auto aftertouchArea = block.removeFromLeft(aftertouchW);
        block.removeFromLeft(colGap);
        auto generateArea = block.removeFromLeft(juce::jmin(generateW, block.getWidth()));

        filterEasyBlockBounds = filterArea;
        layoutFilterEasy(filterArea.reduced(contentInset, contentInset), f, rowH, gap);
        layoutBlock(envTabBtns, envTabSwitchBounds, envEasyBlockBounds, envArea,
                    [&](juce::Rectangle<int> content) { layoutEnvEasy(*env, content, f, rowH, gap); });
        layoutLfoStack(lfoArea);
        layoutDriftStack(driftArea);

        aftertouchEasyBlockBounds = aftertouchArea;
        layoutAftertouchEasy(aftertouchArea.reduced(contentInset, contentInset));

        generateEasyBlockBounds = generateArea;
        layoutGenerateEasy(generateArea.reduced(contentInset, contentInset), f, rowH, gap, false);
        return;
    }

    // Stack layout: small FILTER chip on left of header, CONTROLS bar takes the rest.
    {
        const int filterChipW = juce::jmax(54, measureTextWidth("FILTER", headerFs) + 18);
        filterHeader.setBounds(modHeaderRow.removeFromLeft(juce::jmin(filterChipW, modHeaderRow.getWidth())));
        modHeaderRow.removeFromLeft(juce::jmax(gap, 4));
        modHeader.setBounds(modHeaderRow);
    }

    const int totalGap = blockGap * 5;
    const int usableH = juce::jmax(0, area.getHeight() - totalGap);
    const int filterH = usableH * 8 / 52;
    const int envH = usableH * 7 / 52;
    const int lfoH = usableH * 12 / 52;
    const int generateH = usableH * 5 / 52;
    const int aftertouchH = usableH * 8 / 52;
    const int driftH = usableH - filterH - envH - lfoH - generateH - aftertouchH;

    auto filterArea = area.removeFromTop(filterH);
    filterEasyBlockBounds = filterArea;
    layoutFilterEasy(filterArea.reduced(contentInset, contentInset), f, rowH, gap);
    area.removeFromTop(blockGap);

    auto envArea = area.removeFromTop(envH);
    layoutBlock(envTabBtns, envTabSwitchBounds, envEasyBlockBounds, envArea,
                [&](juce::Rectangle<int> content) { layoutEnvEasy(*env, content, f, rowH, gap); });
    area.removeFromTop(blockGap);

    auto lfoArea = area.removeFromTop(lfoH);
    layoutLfoStack(lfoArea);
    area.removeFromTop(blockGap);

    auto driftArea = area.removeFromTop(driftH);
    layoutDriftStack(driftArea);
    area.removeFromTop(blockGap);

    auto aftertouchArea = area.removeFromTop(aftertouchH);
    aftertouchEasyBlockBounds = aftertouchArea;
    layoutAftertouchEasy(aftertouchArea.reduced(contentInset, contentInset));
    area.removeFromTop(blockGap);

    auto generateArea = area.removeFromTop(generateH);
    generateEasyBlockBounds = generateArea;
    layoutGenerateEasy(generateArea.reduced(contentInset, contentInset), f, rowH, gap);
}

// ──────────────────────────────────────────────────────────────────────────────
// Paint
// ──────────────────────────────────────────────────────────────────────────────
void SynthPanel::paint(juce::Graphics& g)
{
    g.fillAll(kBg);
    if (!initialized) return;

    int padX = juce::roundToInt(static_cast<float>(getWidth()) * 0.02f);
    int inset = 4;

    // Card: Engine mode + Waveform + Loop controls + Scan
    {
        int top = engineHeader.getY() - inset;
        int bot = engineCardBottom + inset;
        paintCard(g, juce::Rectangle<int>(padX, top, getWidth() - padX * 2, bot - top));

        // The LRO card's ground. Drawn here and not in paintOverChildren
        // because the authored instrument's sliders are children of this panel:
        // anything painted over them to blank the waveform widget blanks the
        // sliders too. The widget itself is hidden in this mode
        // (reconcileWaveformDisplayMode), so there is nothing left to mask —
        // this only gives the rows and the captions a ground to sit on.
        if (engineModeHidden.getSelectedId() == EngineMode::Csound + 1)
        {
            const auto wf = waveformDisplay.getBounds();
            if (! wf.isEmpty())
            {
                g.setColour(kBg);
                g.fillRect(wf);
            }
        }

        // Switchbox borders
        if (samplerBtn.isVisible())
            paintSwitchBoxBorder(g, engineSwitchBounds);
        paintSwitchBoxBorder(g, voiceSwitchBounds);
        if (oneshotBtn.isVisible())
            paintSwitchBoxBorder(g, loopSwitchBounds);
        if (frameBtns[0].isVisible())
            paintSwitchBoxBorder(g, framesSwitchBounds);
        if (freezeTextureBtns[0].isVisible())
            paintSwitchBoxBorder(g, freezeTextureSwitchBounds);
        if (octBtns[0].isVisible())
            paintSwitchBoxBorder(g, octaveSwitchBounds);
        if (noiseBtns[0].isVisible())
            paintSwitchBoxBorder(g, noiseSwitchBounds);
    }

    // Card: Modulation (ENVs + LFOs + Drift)
    {
        int top = modHeader.getY() - inset;
        int bot = juce::jmax(filterEasyBlockBounds.getBottom(), envEasyBlockBounds.getBottom(),
                             lfoEasyBlockBounds.getBottom(), driftEasyBlockBounds.getBottom());
        bot = juce::jmax(bot, generateEasyBlockBounds.getBottom(),
                         aftertouchEasyBlockBounds.getBottom());
        bot = juce::jmax(bot, modCardBottom);
        paintCard(g, juce::Rectangle<int>(padX, top, getWidth() - padX * 2, bot - top + inset));

        // Plain framed card (fill + border). The module-colour left stripe was
        // removed per design review — it read as an unwanted embellishment.
        auto paintEasyBlock = [&](juce::Rectangle<int> bounds)
        {
            if (bounds.isEmpty())
                return;

            g.setColour(kSurface.withAlpha(0.62f));
            g.fillRect(bounds);
            g.setColour(juce::Colour(0xaa05070d));
            g.drawRect(bounds.expanded(1, 1), 1);
            g.setColour(kBorder.withAlpha(0.82f));
            g.drawRect(bounds, 1);
        };

        paintEasyBlock(filterEasyBlockBounds);
        paintEasyBlock(envEasyBlockBounds);
        paintEasyBlock(lfoEasyBlockBounds);
        paintEasyBlock(driftEasyBlockBounds);
        paintEasyBlock(aftertouchEasyBlockBounds);
        paintEasyBlock(generateEasyBlockBounds);

        for (const auto& moduleBounds : lfoEasyModuleBounds)
        {
            if (moduleBounds.isEmpty())
                continue;

            g.setColour(kCard.withAlpha(0.42f));
            g.fillRect(moduleBounds);
            g.setColour(kBorder.withAlpha(0.72f));
            g.drawRect(moduleBounds, 1);
        }

        for (const auto& moduleBounds : driftEasyModuleBounds)
        {
            if (moduleBounds.isEmpty())
                continue;

            g.setColour(kCard.withAlpha(0.42f));
            g.fillRect(moduleBounds);
            g.setColour(kBorder.withAlpha(0.72f));
            g.drawRect(moduleBounds, 1);
        }

        paintSwitchBoxBorder(g, filterAlgSwitchBounds);
        paintSwitchBoxBorder(g, filterSlopeSwitchBounds);
        paintSwitchBoxBorder(g, envTabSwitchBounds);
        paintSwitchBoxBorder(g, regenSwitchBounds);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Play-mode icon drawing (painted over children so icons sit on top of buttons)
// ──────────────────────────────────────────────────────────────────────────────
void SynthPanel::paintOverChildren(juce::Graphics& g)
{
    auto drawSegmentGroup = [&](juce::Rectangle<int> bounds, int count, bool vertical)
    {
        if (bounds.isEmpty())
            return;

        const auto outer = bounds.expanded(1, 1);
        g.setColour(juce::Colour(0xee05070d));
        g.drawRect(outer, 1);
        g.setColour(kBorder.brighter(0.35f));
        g.drawRect(bounds, 1);

        g.setColour(juce::Colour(0xdd05070d));
        for (int i = 1; i < count; ++i)
        {
            if (vertical)
            {
                const int y = bounds.getY() + (bounds.getHeight() * i) / count;
                g.drawHorizontalLine(y, static_cast<float>(bounds.getX()),
                                     static_cast<float>(bounds.getRight()));
            }
            else
            {
                const int x = bounds.getX() + (bounds.getWidth() * i) / count;
                g.drawVerticalLine(x, static_cast<float>(bounds.getY()),
                                   static_cast<float>(bounds.getBottom()));
            }
        }

        if (!vertical)
        {
            g.setColour(kBorder.brighter(0.55f));
            g.drawHorizontalLine(bounds.getBottom(), static_cast<float>(bounds.getX()),
                                 static_cast<float>(bounds.getRight()));
        }
    };

    drawSegmentGroup(envTabSwitchBounds, kNumModTabs, false);

    // Csound engine mode (SPEC_phase4_5_csound_llm_preset.md, Phase 4/5): the
    // waveform display has nothing of its own to show here — the "sound" is a
    // prompt-authored Csound orchestra, not a table/sample — so whatever the
    // PREVIOUS engine mode last drew (sample peaks, wavetable fan, frame
    // count) would otherwise sit there stale. Mask the display area and paint
    // the "PROMPT ORCHESTRA" caption plus — once cached (updateVisibility) —
    // the curated instrument keys from lco_library.json, so the space reads as
    // the library the coding LLM already consults, not an empty engine (BJ
    // 2026-07-30: "nicht als Einschränkung, sondern Bibliothek des Coding-
    // LLM" — orientation, never a menu the user is restricted to). Drawn here,
    // in the panel's existing paintOverChildren pass (already re-triggered
    // whenever the mode changes — see the isCsound branches in
    // engineModeHidden.onChange and resized()) — reads the cached string only,
    // no file I/O in a paint callback, no new timer or repaint loop
    // (docs/PERFORMANCE_GUIDE.md: idle-CPU regressions are this project's #1
    // historical bug class). Non-interactive; no other new UI.
    if (engineModeHidden.getSelectedId() == EngineMode::Csound + 1)
    {
        auto wfBounds = waveformDisplay.getBounds();
        if (!wfBounds.isEmpty())
        {
            // The ground is paint()'s job — see there. Filling it HERE would
            // paint over the instrument's sliders, which are children.
            const float f = fs();
            auto card = lroCardBounds();

            // ── Once the author has written an instrument: ITS knobs ──
            // The card has one subject at a time. Before an orchestra exists
            // that is the library the coding LLM reads; after one exists it is
            // the instrument that was written, and the library list would be a
            // second list over a sound that is no longer being chosen
            // (docs/plans/PLAN_lro_param_panel.md §8.3). The sliders themselves
            // are real components laid out in layoutLroKnobs — what is painted
            // here is only what a component cannot be: the author's READING as
            // the card's caption, and the part names over their columns.
            if (! lroControls.isEmpty() || ! lroControls.layers.empty())
            {
                // Every rectangle here comes from layoutLroKnobs. Deriving them
                // a second time from the same expressions is what once drew the
                // instrument's own sentence through the middle of its column
                // headings, and the levels moved a strip since.
                const float capFs  = juce::jlimit(11.0f, 14.0f, f * 0.85f);
                const float partFs = juce::jlimit(10.0f, 13.0f, f * 0.8f);

                const auto reading = processorRef.getCsoundReading().trim();
                g.setColour(kImpulseA.withAlpha(0.75f));
                g.setFont(juce::FontOptions(capFs));
                g.drawText(reading.isNotEmpty() ? reading : juce::String("PROMPT ORCHESTRA"),
                           lroCaptionBounds, juce::Justification::centred, true);

                for (int c = 0; c < lroColumnBounds.size(); ++c)
                {
                    auto col = lroColumnBounds[c];
                    if (c > 0)
                    {
                        // A thin rule between the parts — they are different
                        // instruments, not one list of twelve knobs. It starts
                        // at the part names, so it never crosses the level band
                        // above them, which belongs to the whole instrument.
                        g.setColour(kBorder);
                        g.drawVerticalLine(col.getX(),
                                           static_cast<float>(lroPartNameBounds.getY()),
                                           static_cast<float>(card.getBottom()));
                    }
                    if (c < static_cast<int>(lroControls.parts.size()))
                    {
                        g.setColour(kImpulseA.withAlpha(0.95f));
                        g.setFont(juce::FontOptions(partFs, juce::Font::bold));
                        g.drawText(lroControls.parts[static_cast<size_t>(c)].name.toUpperCase(),
                                   col.withY(lroPartNameBounds.getY())
                                      .withHeight(lroPartNameBounds.getHeight())
                                      .reduced(juce::roundToInt(f * 0.5f), 0),
                                   juce::Justification::centredLeft, true);
                    }
                }
                // The library list below belongs to the empty card, not to this
                // one. Returning is safe rather than merely convenient: what
                // follows this block is the loop/ping-pong iconography, and its
                // own first line returns when oneshotBtn is hidden — which it
                // always is in Csound mode (updateVisibility).
                return;
            }

            // The LIST is the deliverable here, not a footnote under a headline
            // (BJ 2026-07-30: "eine liste vorhandener Orchester/Instrumente ...
            // Bibliothek des Coding-LLM"). So every size below is a real, fixed
            // font size with a legible floor, and the names are laid out as an
            // actual multi-column list. What this replaces: one run-on comma
            // string through drawFittedText, which shrinks without any floor of
            // its own until it fits whatever the headline left over — that is
            // how 30 names ended up unreadable.
            // ONE header line, not a title block over a subtitle: those two
            // together ate half the card's height and left the list — the
            // actual content — squeezed into the rest (BJ, 2026-07-30). The
            // panel's own ENGINE header already names the section, so this is a
            // caption, and it says in the UI what the architecture is:
            // orientation the coding LLM reads, never a menu the user is
            // confined to (project_lco_llm_authors_csound). ASCII only — a raw
            // non-ASCII literal mojibakes in JUCE
            // (feedback_juce_nonascii_strings).
            const float headFs = juce::jlimit(11.0f, 14.0f, f * 0.85f);
            const float nameFs = juce::jlimit(12.0f, 17.0f, f * 1.0f);
            const int headLineH = juce::roundToInt(headFs * 1.7f);

            // The caption and the list are ONE block, centred in the card. The
            // card grew when the controls row below it went away (its octave
            // and noise strip now live in the panel's top bar), and a block
            // pinned to the top of it leaves the rest of the card an empty
            // rectangle. Measured first, drawn after, so the header moves with
            // the list instead of the list hanging under a fixed header.
            auto content = card;
            auto headArea = content.removeFromTop(headLineH);

            if (! lroInstrumentNames_.isEmpty())
            {
                // NAMES ONLY. The parameters are the player's surface
                // (project_lco_params_are_the_user_surface) and belong here —
                // but not as `refl 0.7..0.85`: a lexicon key and a numeric
                // range are the author's identifiers, and nobody writing a
                // prompt can do anything with them (BJ 2026-07-30). No
                // parameter in the lexicon carries a player-readable name yet
                // (`range`, `default`, `note`, `anchors` — the note is
                // paragraphs of measurement prose), so there is nothing
                // truthful to put on that line, and a catalogue of every control
                // laid out in advance is not what this surface does anyway. The
                // library still
                // carries `params`, so the day a short human label exists per
                // parameter this becomes a second line and nothing else.
                const int n = lroInstrumentNames_.size();
                const juce::Font nameFont { juce::FontOptions(nameFs) };

                juce::Array<int> blockW;
                for (const auto& nm : lroInstrumentNames_)
                    // +2: a cell sized to the measured width exactly is a pixel
                    // short once drawText rounds, and every name ellipsizes.
                    blockW.add(2 + juce::roundToInt(
                        juce::GlyphArrangement::getStringWidth(nameFont, nm)));

                const int lineH  = juce::jmax(1, juce::roundToInt(nameFs * 1.45f));
                const int blockH = lineH;
                const int gutter = juce::jmax(1, juce::roundToInt(nameFs * 2.0f));

                // Each column is packed to ITS OWN widest entry, not to one
                // uniform cell sized to the longest label in the whole list: a
                // uniform cell has its gutter eaten the moment the grid is
                // wider than the card, and then the columns touch.
                auto widthsFor = [&](int rowCount, juce::Array<int>& w)
                {
                    w.clearQuick();
                    const int cols = (n + rowCount - 1) / rowCount;
                    for (int c = 0; c < cols; ++c)
                    {
                        int mx = 0;
                        for (int r = 0; r < rowCount; ++r)
                        {
                            const int i = c * rowCount + r;
                            if (i < n) mx = juce::jmax(mx, blockW[i]);
                        }
                        w.add(mx + gutter);
                    }
                };

                // Fewest rows (so the widest, flattest block) that still fits
                // the card's width; if none does, use every row the height
                // affords and let the longest names ellipsize.
                const int rowsMax = juce::jlimit(1, n, content.getHeight() / blockH);
                juce::Array<int> widths;
                int rows = rowsMax;
                for (int r = 1; r <= rowsMax; ++r)
                {
                    widthsFor(r, widths);
                    int sum = -gutter;
                    for (auto v : widths) sum += v;
                    if (sum <= content.getWidth()) { rows = r; break; }
                }
                widthsFor(rows, widths);

                int total = -gutter;
                for (auto v : widths) total += v;

                // Caption and list centred together in the card.
                const int listH = rows * blockH;
                const int slack = juce::jmax(0, content.getHeight() - listH);
                headArea = headArea.withY(headArea.getY() + slack / 2);
                const int listY = headArea.getBottom();

                int x = content.getX() + juce::jmax(0, (content.getWidth() - total) / 2);
                juce::Graphics::ScopedSaveState clipToCard(g);
                g.reduceClipRegion(card);

                for (int c = 0; c < widths.size(); ++c)
                {
                    for (int r = 0; r < rows; ++r)
                    {
                        // Column-major: the eye reads each column down, then
                        // across, the way a printed index does.
                        const int i = c * rows + r;
                        if (i >= n) break;
                        const juce::Rectangle<int> block { x, listY + r * blockH,
                                                           widths[c] - gutter, blockH };

                        g.setColour(kImpulseA.withAlpha(0.95f));
                        g.setFont(nameFont);
                        g.drawText(lroInstrumentNames_[i], block, juce::Justification::centredLeft);
                    }
                    x += widths[c];
                }
            }
            else
            {
                // No list yet (library unreadable, or nothing approved): the
                // caption is the whole block, so it centres on its own instead
                // of hanging at the top of an otherwise empty card.
                headArea = headArea.withY(card.getCentreY() - headLineH / 2);
            }

            g.setColour(kImpulseA.withAlpha(0.75f));
            g.setFont(juce::FontOptions(headFs));
            g.drawText("PROMPT ORCHESTRA - curated instruments; the coding agent "
                       "will use these or invent its own code",
                       headArea, juce::Justification::centred);
        }
    }

    if (!oneshotBtn.isVisible()) return;

    auto iconCol = [](const juce::TextButton& btn) {
        return btn.getToggleState() ? juce::Colours::white : kDim;
    };

    // → One-shot: forward arrow
    {
        auto b = oneshotBtn.getBounds().toFloat();
        float cx = b.getCentreX(), cy = b.getCentreY();
        float hw = b.getHeight() * 0.28f;
        float sw = juce::jmax(1.5f, hw * 0.2f);
        float as = hw * 0.7f;

        g.setColour(iconCol(oneshotBtn));
        g.drawLine(cx - hw, cy, cx + hw, cy, sw);
        juce::Path ah;
        ah.addTriangle(cx + hw + as * 0.3f, cy,
                       cx + hw - as * 0.5f, cy - as * 0.5f,
                       cx + hw - as * 0.5f, cy + as * 0.5f);
        g.fillPath(ah);
    }

    // ↻ Loop: circular arc with arrowhead
    {
        auto b = loopModeBtn.getBounds().toFloat();
        float cx = b.getCentreX(), cy = b.getCentreY();
        float r = b.getHeight() * 0.3f;
        float sw = juce::jmax(1.5f, r * 0.22f);

        juce::Path arc;
        float startA = -juce::MathConstants<float>::halfPi;
        float endA = startA + juce::MathConstants<float>::twoPi * 0.78f;
        arc.addCentredArc(cx, cy, r, r, 0.0f, startA, endA, true);
        g.setColour(iconCol(loopModeBtn));
        g.strokePath(arc, juce::PathStrokeType(sw, juce::PathStrokeType::curved));

        float ex = cx + r * std::cos(endA);
        float ey = cy + r * std::sin(endA);
        float as = r * 0.55f;
        float tx = -std::sin(endA), ty = std::cos(endA);
        float nx = std::cos(endA),  ny = std::sin(endA);
        juce::Path ah;
        ah.addTriangle(ex + tx * as * 0.6f, ey + ty * as * 0.6f,
                       ex - nx * as * 0.45f, ey - ny * as * 0.45f,
                       ex + nx * as * 0.45f, ey + ny * as * 0.45f);
        g.fillPath(ah);
    }

    // ⇄ Ping-pong: two opposing arrows (→ above, ← below)
    {
        auto b = pingpongBtn.getBounds().toFloat();
        float cx = b.getCentreX(), cy = b.getCentreY();
        float hw = b.getWidth() * 0.28f;
        float voff = b.getHeight() * 0.15f;
        float sw = juce::jmax(1.5f, b.getHeight() * 0.09f);
        float as = b.getHeight() * 0.18f;

        g.setColour(iconCol(pingpongBtn));

        // Top: → right arrow
        float ty = cy - voff;
        g.drawLine(cx - hw, ty, cx + hw, ty, sw);
        juce::Path ra;
        ra.addTriangle(cx + hw + as * 0.3f, ty,
                       cx + hw - as * 0.5f, ty - as * 0.65f,
                       cx + hw - as * 0.5f, ty + as * 0.65f);
        g.fillPath(ra);

        // Bottom: ← left arrow
        float by = cy + voff;
        g.drawLine(cx + hw, by, cx - hw, by, sw);
        juce::Path la;
        la.addTriangle(cx - hw - as * 0.3f, by,
                       cx - hw + as * 0.5f, by - as * 0.65f,
                       cx - hw + as * 0.5f, by + as * 0.65f);
        g.fillPath(la);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Resized
// ──────────────────────────────────────────────────────────────────────────────
void SynthPanel::resized()
{
    if (!initialized) return;
    updateVisibility();

    float w = static_cast<float>(getWidth());
    float h = static_cast<float>(getHeight());
    int padX = juce::roundToInt(w * 0.02f);
    int padY = juce::roundToInt(h * 0.005f);
    auto area = getLocalBounds().reduced(padX, padY);
    float f = fs();
    int rowH = juce::roundToInt(f * 1.4f);
    int gap = juce::roundToInt(f * 0.25f);

    // ── Section header — derived from the panel's own height so it is stable
    // across resizes (getTopLevelComponent() returns null on the very first
    // resize, which previously caused a one-time layout shift after the first
    // user interaction).
    int headerH = juce::jlimit(14, 20, juce::roundToInt(h * 0.030f));
    float headerFs = static_cast<float>(headerH) * 0.85f;
    int headerGap = juce::jmax(3, headerH / 5);  // ~20% of header height

    engineHeader.setFont(juce::FontOptions(headerFs));
    auto engineHeaderRow = area.removeFromTop(headerH);
    engineHeader.setBounds(engineHeaderRow);
    area.removeFromTop(headerGap);

    // Engine-mode booleans, needed immediately below to reclaim the 3 switch
    // buttons' width for LRO, and again further down for the waveform/controls
    // branch — computed once here rather than twice.
    const int engineId = engineModeHidden.getSelectedId();
    // LCO (id EngineMode::Lco+1 = 4) lays out identically to plain Wavetable.
    const bool isWavetable = engineId == 2 || engineId == EngineMode::Lco + 1;
    const bool isFreeze = engineId == 3;
    // Csound (id EngineMode::Csound+1 = 5) is not scan-driven, so it gets its
    // own `else if (isCsound)` layout branch below (same waveH reservation,
    // same waveformDisplay.setScanVisible(false), but only octave + noise in
    // the controls row — updateVisibility() hid the rest for LRO).
    const bool isCsound = (engineId == EngineMode::Csound + 1);
    jassertquiet(!isCsound || (!isWavetable && !isFreeze));

    // A CONTROL KEEPS ITS PLACE IN EVERY MODE. BJ, 2026-07-31, on the engine
    // switchboxes sliding left the moment the LRO came on: "Das hier, das
    // herumflippen ansonsten identischer UI-Elemente, ist ein HARTES NoGo."
    // So a control that is hidden in one mode leaves its SLOT behind and the
    // row is never re-flowed to close the hole. This is why every group below
    // is placed against a fixed edge of the row and never against its
    // neighbour's end.
    // ── Engine mode + Voice count + Octave + Noise: the panel's one top bar ──
    //
    // OCTAVE and the NOISE strip belong to every engine, so they belong to the
    // bar every engine shares: "das hier kommt in beiden Panels in die obere
    // Leiste" (BJ, 2026-07-31). They used to sit in each branch's own controls
    // row, at three different x positions.
    //
    // THE BAR HAS ITS OWN UNIT, because every group in it is a fixed multiple
    // of `f` and nothing in it stretches. It now carries 80.7 units where it
    // carried 43.1, and the row below used to absorb any shortfall in
    // `crossfadeRow` / `freezeStereoRow`, which do stretch. Whatever does not
    // fit lands on whatever is placed LAST — voice count and the tuning box —
    // and JUCE clamps a negative width to zero, so the failure mode is a
    // control that is silently GONE, not one that looks cramped.
    //
    // At the shapes the editor actually takes it fits: PluginEditor pins a 3:2
    // aspect ratio, `f` follows the panel's HEIGHT, and the tightest point in
    // the whole 1200x800..2400x1600 range still has ~15% headroom, so `fb == f`
    // there and this scale is dormant. It is the guard for the two ways that
    // ends: a host that resizes the editor past the constrainer (at 1200x1000
    // the pre-scale bar wants 872 px in an 864 px row and the temperament
    // control disappears), and anything added to this bar later. The scale
    // reads the row's WIDTH and nothing else — never the engine mode — so the
    // bar shrinks as a whole and every control still sits at the same x in
    // every mode.
    auto modeRow = area.removeFromTop(rowH);
    {
        const float uEngine = 5.0f, uGap = 1.5f, uVoice = 2.8f, uTune = 5.5f;
        const float uOct = 2.5f, uNoise = 4.0f, uLevel = 11.5f, uTight = 0.8f;
        const float wanted = uEngine * 3 + uGap + uVoice * kNumVoiceBtns + uGap + uTune
                           + uOct * kNumOctBtns + uTight
                           + uNoise * kNumNoiseBtns + uLevel + uTight;
        const float fb = f * juce::jmin(1.0f, static_cast<float>(modeRow.getWidth())
                                              / juce::jmax(1.0f, wanted * f));
        auto cells = [fb](float u) { return juce::jmax(1, juce::roundToInt(fb * u)); };

        // The three engine buttons are hidden for LRO (updateVisibility) — it
        // is entered only through MainPanel's oscModeToggle, never through
        // them — but their SLOT stays reserved, so voice count and tuning sit
        // at the same x whichever engine is on.
        {
            const int cellW = cells(uEngine);
            auto engineArea = modeRow.removeFromLeft(cellW * 3);
            if (! isCsound)
            {
                samplerBtn.setBounds(engineArea.removeFromLeft(cellW));
                wavetableBtn.setBounds(engineArea.removeFromLeft(cellW));
                freezeBtn.setBounds(engineArea.removeFromLeft(cellW));
                engineSwitchBounds = samplerBtn.getBounds().getUnion(freezeBtn.getBounds());
            }
            modeRow.removeFromLeft(cells(uGap));
        }

        // Right edge first: it is the card's and does not move, so the two
        // groups anchored to it cannot drift when the row's left half changes.
        {
            const int nCellW = cells(uNoise);
            auto noiseArea = modeRow.removeFromRight(
                juce::jmin(nCellW * kNumNoiseBtns + cells(uLevel), modeRow.getWidth()));
            modeRow.removeFromRight(cells(uTight));
            for (int i = 0; i < kNumNoiseBtns; ++i)
                noiseBtns[i].setBounds(noiseArea.removeFromLeft(nCellW));
            noiseSwitchBounds = noiseBtns[0].getBounds()
                                    .getUnion(noiseBtns[kNumNoiseBtns - 1].getBounds());
            noiseLevelRow->setBounds(noiseArea);
        }

        const int oCellW = cells(uOct);
        auto octaveArea = modeRow.removeFromRight(oCellW * kNumOctBtns);
        modeRow.removeFromRight(cells(uTight));
        for (int i = 0; i < kNumOctBtns; ++i)
            octBtns[i].setBounds(octaveArea.removeFromLeft(oCellW));
        octaveSwitchBounds = octBtns[0].getBounds().getUnion(octBtns[kNumOctBtns - 1].getBounds());

        const int vcW = cells(uVoice);
        for (int i = 0; i < kNumVoiceBtns; ++i)
            voiceBtns[i].setBounds(modeRow.removeFromLeft(vcW));
        voiceSwitchBounds = voiceBtns[0].getBounds().getUnion(voiceBtns[kNumVoiceBtns - 1].getBounds());

        modeRow.removeFromLeft(cells(uGap));
        tuningBox.setBounds(modeRow.removeFromLeft(cells(uTune)));
        tuningBox.setJustificationType(juce::Justification::centred);
    }
    area.removeFromTop(gap);

    // ── Waveform — give it all space not needed by sections below ──
    // Calculate height needed below: sampler/WT controls + filter + mod + LFO + drift
    // Always reserve same space for engine controls (max of sampler/WT)
    // so waveform height stays stable when switching modes
    const int waveformReserveH = juce::roundToInt(WaveformDisplay::HANDLE_RADIUS * 2.0f + 4.0f);
    int samplerCtrlH = waveformReserveH + rowH + gap * 2; // waveform handles + one controls row
    int filterH = headerH + headerGap + rowH + gap + rowH * 2 + gap; // advanced filter height, folded into Easy block
    int modH = gap * 3 + headerH + headerGap; // section gap + header
    int envH = (rowH * 4 + gap) * 3; // 3 envelopes × (header + 3 slider rows + gap)
    int lfoH = gap + headerH + headerGap + (rowH + gap) * 3;              // lfo header + 3 single-row LFOs
    int aftertouchH = rowH + gap;                                         // MIDI aftertouch row
    int driftH = gap + headerH + headerGap + (rowH + gap) * 3;            // drift header + 3 single-row drifts
    int regenH = gap + headerH;                                            // regen controls live in the header row
    const int advancedModBodyH = envH + lfoH + aftertouchH + driftH + regenH + gap * 5;
    // Keep Easy in the same vertical slot as Advanced so the engine waveform,
    // filter and footer sections don't jump when switching interface modes.
    int belowWave = samplerCtrlH + filterH + modH + advancedModBodyH;
    int waveH = juce::jmax(0, area.getHeight() - belowWave);

    // HF boost and Normalize are the same two buttons in every mode that has
    // them, so they get one place: hard against the row's right edge, with
    // Normalize's slot reserved where it is hidden (Wavetable). Laying HF out
    // from the LEFT there put it a third of the row away from where the other
    // two modes draw it.
    auto layoutHfNorm = [&](juce::Rectangle<int>& row, bool withNormalize)
    {
        const int normW = juce::roundToInt(f * 4.0f);
        const int hfW = juce::roundToInt(f * 2.8f);
        auto normArea = row.removeFromRight(normW);
        if (withNormalize)
            normalizeToggle.setBounds(normArea);
        hfBoostBtn.setBounds(row.removeFromRight(hfW));
        row.removeFromRight(juce::roundToInt(f * 1.5f));
    };

    if (isWavetable || isFreeze)
    {
        // ── Scan-driven engines: dot + brackets on one line below waveform ──
        int scanLineH = waveformReserveH;
        auto waveformBlock = area.removeFromTop(waveH + scanLineH);

        waveformDisplay.setBottomReserve(scanLineH);
        waveformDisplay.setScanVisible(true);
        waveformDisplay.setBounds(waveformBlock);

        // Hide the scanRow slider (APVTS still connected), scan is drawn by WaveformDisplay
        scanRow->setBounds(-1000, -1000, 10, 10);
        scanHint.setVisible(false);

        area.removeFromTop(gap);

        if (isFreeze)
        {
            // ── Granular: position dot + texture + stereo + HF/Norm ──
            //    (octave and the noise strip are in the top bar, one place for
            //     every engine)
            auto granularRow = area.removeFromTop(rowH);
            const int granularRowBottom = granularRow.getBottom();
            layoutHfNorm(granularRow, true);

            const int tCellW = juce::roundToInt(f * 3.8f);
            for (int i = 0; i < kNumFreezeTextureBtns; ++i)
                freezeTextureBtns[i].setBounds(granularRow.removeFromLeft(tCellW));
            freezeTextureSwitchBounds = freezeTextureBtns[0].getBounds()
                .getUnion(freezeTextureBtns[kNumFreezeTextureBtns - 1].getBounds());
            granularRow.removeFromLeft(juce::roundToInt(f * 0.8f));
            freezeStereoRow->setBounds(granularRow);

            frameCountLabel.setBounds(-1000, -1000, 10, 10);
            area.removeFromTop(gap);
            engineCardBottom = granularRowBottom;
        }
        else
        {
            // [→][↻][⇄] [Nf] [32|64|128|256] [Smooth] [Auto] ......... [HF][ ]
            auto wtRow = area.removeFromTop(rowH);
            const int wtRowBottom = wtRow.getBottom();
            // Normalize is hidden here, and its slot stays empty rather than
            // pulling HF across the row.
            layoutHfNorm(wtRow, false);
            auto leftCol = wtRow;

            // ── Left column: loop icons + frame switchbox + smooth ──
            // Same width as in the Sampler branch below: these are the SAME
            // three buttons, so they must not change size or x when the engine
            // changes. They were f*2.6 here and f*2.8 there, which moved the
            // ping-pong icon and the drawn switchbox border on every switch.
            int iconW = juce::roundToInt(f * 2.8f);
            oneshotBtn.setBounds(leftCol.removeFromLeft(iconW));
            loopModeBtn.setBounds(leftCol.removeFromLeft(iconW));
            pingpongBtn.setBounds(leftCol.removeFromLeft(iconW));
            loopSwitchBounds = oneshotBtn.getBounds().getUnion(pingpongBtn.getBounds());
            leftCol.removeFromLeft(juce::roundToInt(f * 0.35f));

            int frameCountW = juce::roundToInt(f * 2.6f);
            frameCountLabel.setBounds(leftCol.removeFromLeft(frameCountW));
            leftCol.removeFromLeft(juce::roundToInt(f * 0.35f));

            int cellW = juce::roundToInt(f * 2.6f);
            for (int i = 0; i < kNumFrameBtns; ++i)
                frameBtns[i].setBounds(leftCol.removeFromLeft(cellW));
            framesSwitchBounds = frameBtns[0].getBounds().getUnion(frameBtns[kNumFrameBtns - 1].getBounds());
            leftCol.removeFromLeft(juce::roundToInt(f * 0.35f));

            int smoothW = juce::jmin(juce::roundToInt(f * 4.0f), leftCol.getWidth());
            smoothToggle.setBounds(leftCol.removeFromLeft(smoothW));
            leftCol.removeFromLeft(juce::roundToInt(f * 0.25f));
            int autoW = juce::jmin(juce::roundToInt(f * 3.8f), leftCol.getWidth());
            autoScanToggle.setBounds(leftCol.removeFromLeft(autoW));

            area.removeFromTop(gap);
            engineCardBottom = wtRowBottom;
        }
    }
    else if (isCsound)
    {
        // ── LRO: the waveform card has no baked table and no loaded sample to
        // draw (reconcileWaveformDisplayMode blanks its region label above),
        // and NOTHING is left for a controls row: octave and the noise strip
        // moved up into the top bar, and the loop icons, Opt, Xfade, HF and
        // Normalize all reprocess a captured Sampler/Freeze/Wavetable BUFFER
        // that a live Csound voice never has (updateVisibility hid them).
        //
        // So the row's height goes to the card, which is what BJ asked for
        // when he moved the strip up: "So gewinnen wir die untere Zeile,
        // entweder als Raum oder als Statusanzeige" (2026-07-31). Space, for
        // now — a status display is UI that has not been ordered yet. The
        // sections BELOW keep their place either way, because the card's total
        // reservation (samplerCtrlH) is the same in every mode.
        int handleLineH = waveformReserveH;
        waveformDisplay.setBottomReserve(handleLineH);
        waveformDisplay.setScanVisible(false);
        waveformDisplay.setBounds(area.removeFromTop(waveH + handleLineH + gap + rowH));

        area.removeFromTop(gap);
        engineCardBottom = waveformDisplay.getBottom();
        frameCountLabel.setBounds(-1000, -1000, 10, 10);
    }
    else
    {
        // ── Sampler: waveform + bracket handles + controls row ──
        int handleLineH = waveformReserveH;
        waveformDisplay.setBottomReserve(handleLineH);
        waveformDisplay.setScanVisible(false);
        waveformDisplay.setBounds(area.removeFromTop(waveH + handleLineH));
        area.removeFromTop(gap);  // spacing to controls

        // [→][↻][⇄] [Opt] Xfade[========] ................... [HF][Norm]
        auto loopRow = area.removeFromTop(rowH);
        const int loopRowBottom = loopRow.getBottom();

        // ── Left column: loop icons + Opt + Xfade ──
        auto leftCol = loopRow;
        layoutHfNorm(leftCol, true);

        int iconW = juce::roundToInt(f * 2.8f);
        oneshotBtn.setBounds(leftCol.removeFromLeft(iconW));
        loopModeBtn.setBounds(leftCol.removeFromLeft(iconW));
        pingpongBtn.setBounds(leftCol.removeFromLeft(iconW));
        loopSwitchBounds = oneshotBtn.getBounds().getUnion(pingpongBtn.getBounds());
        leftCol.removeFromLeft(juce::roundToInt(f * 0.3f));

        int optW = juce::roundToInt(f * 4.5f);
        loopOptimizeBtn.setBounds(leftCol.removeFromLeft(optW));
        leftCol.removeFromLeft(2);

        crossfadeRow->setBounds(leftCol);

        area.removeFromTop(gap);
        engineCardBottom = loopRowBottom;

        // Hide wavetable-only controls in sampler mode
        frameCountLabel.setBounds(-1000, -1000, 10, 10);
    }
    // The authored instrument's knobs go into the LRO card — after the branch,
    // because it is the branch that gave the card its bounds, and because every
    // OTHER engine has to park the rows off-screen rather than leave them
    // sitting on top of a sampler waveform.
    layoutLroKnobs();
    area.removeFromTop(gap * 2);

    int sectionGap = gap * 3;

    // ── MODULATION section header ──
    area.removeFromTop(sectionGap);
    modHeader.setText(" CONTROLS", juce::dontSendNotification);
    labelAsHeaderBand(modHeader, kModCol);
    modHeader.setFont(juce::FontOptions(headerFs));
    auto modHeaderBounds = area.removeFromTop(headerH);

    // layoutModEasy splits the header row between the FILTER chip and the
    // CONTROLS bar.
    area.removeFromTop(headerGap);
    area.removeFromBottom(juce::jmax(headerH, juce::roundToInt(f * 1.0f)));
    layoutModEasy(area, modHeaderBounds, f, rowH, gap, headerH, headerFs);
    modCardBottom = area.getY();
}

// ── WarpHoldBtn ──

void SynthPanel::WarpHoldBtn::mouseDown(const juce::MouseEvent& e)
{
    holdTriggered_ = false;
    startTimer(350);
    TextButton::mouseDown(e);
}

void SynthPanel::WarpHoldBtn::mouseUp(const juce::MouseEvent& e)
{
    if (!holdTriggered_)
    {
        stopTimer();
        if (onTap) onTap();
    }
    TextButton::mouseUp(e);
}

void SynthPanel::WarpHoldBtn::mouseExit(const juce::MouseEvent& e)
{
    stopTimer();
    holdTriggered_ = false;
    TextButton::mouseExit(e);
}

void SynthPanel::WarpHoldBtn::timerCallback()
{
    stopTimer();
    holdTriggered_ = true;
    juce::PopupMenu menu;
    for (int i = 0; i < FilterWarpStyle::kCount; ++i)
        menu.addItem(i + 1, juce::String(FilterWarpStyle::kEntries[i].label));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this),
                       [this](int r) { if (r > 0 && onStylePick) onStylePick(r); });
}

void SynthPanel::WarpHoldBtn::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    TextButton::paintButton(g, highlighted, down);
    // Corner triangle — same visual language as ComboBox dropdown indicator.
    const float s = juce::jlimit(8.0f, 14.0f, static_cast<float>(getHeight()) * 0.52f);
    g.setColour(findColour(getToggleState() ? juce::TextButton::textColourOnId
                                            : juce::TextButton::textColourOffId)
                    .withAlpha(0.22f));
    juce::Path p;
    p.addTriangle(static_cast<float>(getWidth()) - s,  static_cast<float>(getHeight()),
                  static_cast<float>(getWidth()),        static_cast<float>(getHeight()) - s,
                  static_cast<float>(getWidth()),        static_cast<float>(getHeight()));
    g.fillPath(p);
}
