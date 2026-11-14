#include "SequencerPanel.h"
#include "../PluginProcessor.h"
#include "WaveformDisplay.h"
#include "../sequencer/PhraseModel.h"

// ─── Note name helper ──────────────────────────────────────────────
static juce::String noteName(int n)
{
    static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    int oct = n / 12 - 1;
    return juce::String(names[n % 12]) + juce::String(oct);
}

namespace
{
constexpr int kOverflowDivisionBase = 3000;
constexpr int kOverflowOctaveBase = 4000;
constexpr int kOverflowSavePattern = 5001;
constexpr int kOverflowLoadPattern = 5002;
constexpr int kOverflowShuffleBase = 6000;             // + percent value (0/25/50/75)
constexpr int kOverflowShuffleValues[] = { 0, 25, 50, 75 };

bool isWaveformOneShotDrag(const juce::var& description)
{
    if (description.isString())
        return description.toString() == WaveformDisplay::kSequencerOneShotDragDescription;

    if (auto* obj = description.getDynamicObject())
        return obj->getProperty("kind").toString() == WaveformDisplay::kSequencerOneShotDragDescription;

    return false;
}

bool getWaveformOneShotDragRegion(const juce::var& description, float& start, float& end)
{
    if (auto* obj = description.getDynamicObject())
    {
        if (obj->getProperty("kind").toString() != WaveformDisplay::kSequencerOneShotDragDescription)
            return false;

        start = static_cast<float>(obj->getProperty("start"));
        end = static_cast<float>(obj->getProperty("end"));
        return true;
    }

    return false;
}

// Drag kind for copying an already-captured one-shot from one slot to another
// (distinct from the waveform-region drag above). Carries the source step/slot.
constexpr const char* kOneShotCopyDragKind = "t5ynth-oneshot-copy";

bool isOneShotCopyDrag(const juce::var& description)
{
    if (auto* obj = description.getDynamicObject())
        return obj->getProperty("kind").toString() == kOneShotCopyDragKind;

    return false;
}

bool getOneShotCopyDragSource(const juce::var& description, int& srcStep, int& srcSlot)
{
    if (auto* obj = description.getDynamicObject())
    {
        if (obj->getProperty("kind").toString() != kOneShotCopyDragKind)
            return false;

        srcStep = static_cast<int>(obj->getProperty("srcStep"));
        srcSlot = static_cast<int>(obj->getProperty("srcSlot"));
        return true;
    }

    return false;
}

// The chord-loop slots' parameters, in slot order.
constexpr const char* kPhraseChordPIDs[] = {
    PID::phrChord1, PID::phrChord2, PID::phrChord3, PID::phrChord4
};

template <size_t N>
juce::StringArray choiceLabels(const ChoiceEntry (&entries)[N])
{
    juce::StringArray labels;
    for (const auto& e : entries)
        labels.add(e.label);
    return labels;
}

// The panel's dropdown look (as genScaleRootBox and the Gen boxes).
void styleSeqCombo(juce::ComboBox& box)
{
    box.setColour(juce::ComboBox::backgroundColourId, kSurface);
    box.setColour(juce::ComboBox::textColourId, kSeqCol);
    box.setColour(juce::ComboBox::outlineColourId, kBorder);
}

// A tag in front of a control without a label of its own (Gen's "Rng"):
// 11 px, white like the inline-bar labels beside it.
constexpr float kTagFontSize = 11.0f;

void styleTag(juce::Label& tag, const juce::String& text)
{
    tag.setText(text, juce::dontSendNotification);
    tag.setColour(juce::Label::textColourId, juce::Colours::white);
    tag.setJustificationType(juce::Justification::centredLeft);
    tag.setFont(juce::FontOptions(kTagFontSize));
    tag.setInterceptsMouseClicks(false, false);
}

// The tag's width: its text plus the Label's default border (3 px a side).
int tagWidth(const juce::Label& tag)
{
    return measureTextWidth(tag.getText(), kTagFontSize) + 6;
}

// A switch segment or button as wide as its label in the button font, plus
// the LookAndFeel's text indents (at most 0.6 x the font height per side),
// so no label is squeezed below the font size (as the mode switch).
int labelButtonWidth(juce::TextButton& b, int rowH)
{
    const float fs = b.getLookAndFeel().getTextButtonFont(b, rowH).getHeight();
    return measureTextWidth(b.getButtonText(), fs) + 2 * juce::roundToInt(fs * 0.6f) + 2;
}
}

// ─── IconLnF ──────────────────────────────────────────────────────
void SequencerPanel::IconLnF::drawButtonBackground(juce::Graphics& g, juce::Button& b,
                                                     const juce::Colour&, bool over, bool down)
{
    g.setColour(down ? kSurface.darker(0.1f) : over ? kSurface.brighter(0.15f) : kSurface);
    g.fillRect(b.getLocalBounds());
}

void SequencerPanel::IconLnF::drawButtonText(juce::Graphics& g, juce::TextButton& b,
                                               bool over, bool)
{
    auto bounds = b.getLocalBounds().toFloat().reduced(4.0f);
    g.setColour(over ? kSeqCol : kDim);
    g.strokePath(icon, juce::PathStrokeType(1.3f),
                 icon.getTransformToScaleToFit(bounds, true));
}


// ─── StepColumn ────────────────────────────────────────────────────

void SequencerPanel::StepColumn::paint(juce::Graphics& g)
{
    if (!processor) return;
    auto step = processor->getStepSequencer().getStep(stepIndex);
    auto b = getLocalBounds().reduced(1);
    int w = b.getWidth();

    // Background
    g.setColour(isCurrentStep ? kSeqCol.withAlpha(0.35f)
                : step.enabled ? kSurface : kBg);
    g.fillRect(b);

    // Beat group border
    if (stepIndex % 4 == 0)
    {
        g.setColour(kBorder);
        g.drawLine(static_cast<float>(b.getX()), static_cast<float>(b.getY()),
                   static_cast<float>(b.getX()), static_cast<float>(b.getBottom()), 1.0f);
    }

    // ── Note vertical slider (top 55%) ──
    int nB = noteBottom();
    auto noteR = b.removeFromTop(nB);
    {
        int semi = step.note;
        float frac = juce::jlimit(0.0f, 1.0f, static_cast<float>(semi - 36) / 48.0f);
        int fillH = juce::roundToInt(frac * static_cast<float>(noteR.getHeight()));

        g.setColour(kDimmer.withAlpha(0.3f));
        g.fillRect(noteR);
        g.setColour(step.enabled ? kSeqCol.withAlpha(0.5f) : kDim.withAlpha(0.3f));
        g.fillRect(noteR.getX(), noteR.getBottom() - fillH, noteR.getWidth(), fillH);

        g.setColour(step.enabled ? juce::Colours::white : kDim);
        float fs = juce::jlimit(7.0f, 13.0f, static_cast<float>(w) * 0.24f);
        g.setFont(juce::FontOptions(fs));
        auto noteTextR = noteR.withTrimmedTop(juce::jmin(4, juce::jmax(1, noteR.getHeight() / 12)));
        g.drawText(noteName(semi), noteTextR, juce::Justification::centredTop);
    }

    // ── One-shot sample slots ──
    b.removeFromTop(oneShotBottom() - noteBottom());
    auto shotR = oneShotArea();
    if (shotR.getHeight() > 2)
    {
        for (int slot = 0; slot < T5ynthStepSequencer::ONE_SHOT_SLOTS; ++slot)
        {
            auto r = oneShotSlotBounds(slot).reduced(1);
            const bool hasSample = processor->hasSequencerOneShotSample(stepIndex, slot);
            const auto mode = processor->getStepSequencer().getStepOneShotMode(stepIndex, slot);
            const bool hover = slot == dropHoverSlot;

            juce::Colour fill = kDimmer.withAlpha(0.12f);
            juce::Colour text = kDimmer;
            juce::String label = juce::String(slot + 1);

            if (hasSample)
            {
                if (mode == T5ynthStepSequencer::OneShotMode::Accent)
                {
                    fill = kSeqCol.brighter(0.45f).withAlpha(0.68f);
                    text = juce::Colours::white;
                    label = "A";
                }
                else if (mode == T5ynthStepSequencer::OneShotMode::Mute)
                {
                    fill = kDimmer.withAlpha(0.18f);
                    text = kDim;
                    label = "M";
                }
                else
                {
                    fill = kSeqCol.withAlpha(0.44f);
                    text = juce::Colours::white;
                    label = "N";
                }
            }

            if (hover)
                fill = kSeqCol.withAlpha(hasSample ? 0.82f : 0.38f);

            g.setColour(fill);
            g.fillRect(r);
            g.setColour(hover ? juce::Colours::white.withAlpha(0.9f) : kBorder);
            g.drawRect(r, 1);
            g.setColour(text);
            g.setFont(juce::FontOptions(juce::jlimit(6.0f, 9.0f, static_cast<float>(w) * 0.18f)));
            g.drawText(label, r, juce::Justification::centred);
        }
    }

    // ── Velocity horizontal bar ──
    int vB = velBottom() - oneShotBottom();
    auto velR = b.removeFromTop(vB);
    if (velR.getHeight() > 2)
    {
        g.setColour(kDimmer.withAlpha(0.3f));
        g.fillRect(velR);
        float velPx = step.velocity * static_cast<float>(velR.getWidth());
        g.setColour(step.enabled ? kSeqCol.withAlpha(0.7f) : kDim.withAlpha(0.4f));
        g.fillRect(velR.getX(), velR.getY(), juce::roundToInt(velPx), velR.getHeight());
    }

    // ── Bottom buttons: [On][Bind] side by side (remaining 32%) ──
    auto btnArea = b;
    int halfW = btnArea.getWidth() / 2;
    auto onR = btnArea.removeFromLeft(halfW);
    auto glR = btnArea;
    float btnFs = juce::jlimit(7.0f, 11.0f, static_cast<float>(w) * 0.20f);

    // On button
    g.setColour(step.enabled ? kSeqCol.withAlpha(0.45f) : kDimmer.withAlpha(0.15f));
    g.fillRect(onR.reduced(1));
    g.setColour(step.enabled ? juce::Colours::white : kDimmer);
    g.setFont(juce::FontOptions(btnFs));
    g.drawText("On", onR, juce::Justification::centred);

    // Bind button — 3-state cycle: Off → Bind → Glide → Off.
    // Glide reads brighter than Bind so the ramped variant is distinguishable.
    const bool isGlide = step.bindMode == T5ynthStepSequencer::BindMode::Glide;
    const bool bound   = step.bindMode != T5ynthStepSequencer::BindMode::Off;
    g.setColour(bound ? kSeqCol.withAlpha(isGlide ? 0.55f : 0.30f)
                      : kDimmer.withAlpha(0.15f));
    g.fillRect(glR.reduced(1));
    g.setColour(bound ? juce::Colours::white : kDimmer);
    g.setFont(juce::FontOptions(btnFs));
    g.drawText(isGlide ? "Glide" : "Bind", glR, juce::Justification::centred);

    // Step-record armed-target highlight: an unmistakable red frame on the step
    // about to receive the next played note.
    if (isRecordCursor)
    {
        g.setColour(kError);
        g.drawRect(getLocalBounds(), 2);
    }
}

void SequencerPanel::StepColumn::mouseDown(const juce::MouseEvent& e)
{
    if (!processor) return;
    auto& seq = processor->getStepSequencer();
    auto step = seq.getStep(stepIndex);
    int y = e.getPosition().getY();

    if (y < noteBottom())
    {
        // Start note drag
        dragZone = 1;
        dragStartVal = static_cast<float>(step.note);
        noteHoldPreviewActive = e.mods.isShiftDown() && processor->canUseStepHoldPreview();
        noteHoldPreviewNote = step.note;
        if (noteHoldPreviewActive)
            processor->beginStepHoldPreview(step.note);
    }
    else if (y < oneShotBottom())
    {
        int slot = oneShotSlotAt(e.getPosition());
        if (slot >= 0)
        {
            oneShotPressSlot = -1;
            oneShotDragStarted = false;
            if (e.mods.isRightButtonDown())
            {
                processor->clearSequencerOneShotSample(stepIndex, slot);
            }
            else
            {
                // Defer the mode-cycle to mouseUp: a press-and-drag on a filled
                // slot starts a copy drag (the captured sample is often no longer
                // shown in the wave display), while a plain click still cycles.
                oneShotPressSlot = slot;
            }
            dragZone = 4;
            repaint();
        }
    }
    else if (y < velBottom())
    {
        // Velocity drag
        dragZone = 3;
        float vel = static_cast<float>(e.getPosition().getX() - 1)
                    / static_cast<float>(juce::jmax(1, getWidth() - 2));
        seq.setStepVelocity(stepIndex, juce::jlimit(0.0f, 1.0f, vel));
    }
    else
    {
        // Bottom buttons: left half = On, right half = Bind/Glide (3-state cycle)
        if (e.getPosition().getX() < getWidth() / 2)
            seq.setStepEnabled(stepIndex, !step.enabled);
        else
            seq.cycleStepBindMode(stepIndex);
        dragZone = 2;
    }
    repaint();
}

void SequencerPanel::StepColumn::mouseDrag(const juce::MouseEvent& e)
{
    if (!processor) return;
    auto& seq = processor->getStepSequencer();

    if (dragZone == 1)
    {
        // Note: drag up = higher
        int noteH = noteBottom();
        if (noteH < 1) return;
        float deltaY = static_cast<float>(e.getDistanceFromDragStartY());
        float deltaSemi = -deltaY / static_cast<float>(noteH) * 48.0f;
        int newNote = juce::jlimit(36, 84, juce::roundToInt(dragStartVal + deltaSemi));
        seq.setStepNote(stepIndex, newNote);
        if (noteHoldPreviewActive && newNote != noteHoldPreviewNote)
        {
            processor->updateStepHoldPreview(newNote);
            noteHoldPreviewNote = newNote;
        }
        repaint();
    }
    else if (dragZone == 3)
    {
        // Velocity: horizontal
        float vel = static_cast<float>(e.getPosition().getX() - 1)
                    / static_cast<float>(juce::jmax(1, getWidth() - 2));
        seq.setStepVelocity(stepIndex, juce::jlimit(0.0f, 1.0f, vel));
        repaint();
    }
    else if (dragZone == 4 && !oneShotDragStarted && oneShotPressSlot >= 0
             && e.getDistanceFromDragStart() >= 5
             && processor->hasSequencerOneShotSample(stepIndex, oneShotPressSlot))
    {
        // Start a copy drag of the captured one-shot to another slot.
        if (auto* container = findParentComponentOfClass<juce::DragAndDropContainer>())
        {
            juce::DynamicObject::Ptr payload = new juce::DynamicObject();
            payload->setProperty("kind", kOneShotCopyDragKind);
            payload->setProperty("srcStep", stepIndex);
            payload->setProperty("srcSlot", oneShotPressSlot);
            container->startDragging(juce::var(payload.get()), this);
        }
        oneShotDragStarted = true;
    }
}

void SequencerPanel::StepColumn::mouseUp(const juce::MouseEvent&)
{
    if (processor != nullptr && noteHoldPreviewActive)
        processor->endStepHoldPreview();

    // One-shot slot: a plain click (no copy-drag) cycles the slot's playback
    // mode — the behavior that used to fire on mouseDown.
    if (dragZone == 4 && !oneShotDragStarted && oneShotPressSlot >= 0
        && processor != nullptr
        && processor->hasSequencerOneShotSample(stepIndex, oneShotPressSlot))
    {
        processor->getStepSequencer().cycleStepOneShotMode(stepIndex, oneShotPressSlot);
        repaint();
    }

    oneShotPressSlot = -1;
    oneShotDragStarted = false;
    noteHoldPreviewActive = false;
    noteHoldPreviewNote = -1;
    dragZone = -1;
}

juce::Rectangle<int> SequencerPanel::StepColumn::oneShotArea() const
{
    auto area = getLocalBounds().reduced(1);
    area.removeFromTop(noteBottom());
    return area.removeFromTop(oneShotBottom() - noteBottom());
}

juce::Rectangle<int> SequencerPanel::StepColumn::oneShotSlotBounds(int slot) const
{
    auto area = oneShotArea();
    if (slot < 0 || slot >= T5ynthStepSequencer::ONE_SHOT_SLOTS)
        return {};

    const int left = area.getX() + (area.getWidth() * slot)
        / T5ynthStepSequencer::ONE_SHOT_SLOTS;
    const int right = area.getX() + (area.getWidth() * (slot + 1))
        / T5ynthStepSequencer::ONE_SHOT_SLOTS;
    return { left, area.getY(), right - left, area.getHeight() };
}

int SequencerPanel::StepColumn::oneShotSlotAt(juce::Point<int> p) const
{
    for (int slot = 0; slot < T5ynthStepSequencer::ONE_SHOT_SLOTS; ++slot)
        if (oneShotSlotBounds(slot).contains(p))
            return slot;

    return -1;
}

int SequencerPanel::StepColumn::oneShotDropSlotAt(juce::Point<int> p) const
{
    const int slot = oneShotSlotAt(p);
    if (slot >= 0)
        return slot;

    if (dropHoverSlot >= 0)
        return dropHoverSlot;

    if (!oneShotArea().contains(p))
        return -1;

    const auto area = oneShotArea();
    const int relX = p.x - area.getX();
    const int slotW = juce::jmax(1, area.getWidth() / T5ynthStepSequencer::ONE_SHOT_SLOTS);
    return juce::jlimit(0, T5ynthStepSequencer::ONE_SHOT_SLOTS - 1, relX / slotW);
}

bool SequencerPanel::StepColumn::isInterestedInDragSource(const SourceDetails& details)
{
    return isWaveformOneShotDrag(details.description)
        || isOneShotCopyDrag(details.description);
}

void SequencerPanel::StepColumn::itemDragMove(const SourceDetails& details)
{
    const int slot = oneShotSlotAt(details.localPosition);
    if (slot != dropHoverSlot)
    {
        dropHoverSlot = slot;
        repaint();
    }
}

void SequencerPanel::StepColumn::itemDragExit(const SourceDetails&)
{
    if (dropHoverSlot >= 0)
    {
        dropHoverSlot = -1;
        repaint();
    }
}

void SequencerPanel::StepColumn::itemDropped(const SourceDetails& details)
{
    const int slot = oneShotDropSlotAt(details.localPosition);
    dropHoverSlot = -1;
    if (processor != nullptr && slot >= 0)
    {
        int srcStep = 0;
        int srcSlot = 0;
        if (getOneShotCopyDragSource(details.description, srcStep, srcSlot))
        {
            // Copy an existing captured one-shot from another slot.
            processor->copySequencerOneShotSample(srcStep, srcSlot, stepIndex, slot);
        }
        else
        {
            float start = 0.0f;
            float end = 1.0f;
            if (getWaveformOneShotDragRegion(details.description, start, end))
                processor->assignSequencerOneShotFromRegion(stepIndex, slot, start, end);
            else
                processor->assignSequencerOneShotFromCurrentRegion(stepIndex, slot);
        }
    }
    repaint();
}

// ─── SequencerPanel ────────────────────────────────────────────────

SequencerPanel::SequencerPanel(T5ynthProcessor& p)
    : processorRef(p)
{
    auto& apvts = p.getValueTreeState();

    // Section header
    paintSectionHeader(seqHeader, "SEQUENCER", kSeqCol);
    addAndMakeVisible(seqHeader);

    // ── Transport (single toggle) ──
    transportBtn.setColour(juce::TextButton::buttonColourId, kSurface);
    transportBtn.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff4caf50));
    transportBtn.setTooltip("Start/stop the sequencer transport");
    transportBtn.onClick = [this] {
        auto* par = processorRef.getValueTreeState().getParameter(PID::seqRunning);
        if (!par) return;
        // The raw value, as the label (timerCallback) and the processor read
        // it: the normalised getValue() of a host write of exactly 0.5 reads
        // stopped while the transport runs, and the click started it again.
        const bool playing = processorRef.getValueTreeState()
            .getRawParameterValue(PID::seqRunning)->load() > 0.5f;
        par->setValueNotifyingHost(playing ? 0.0f : 1.0f);
        if (playing) showCurrentStep(-1);
    };
    addAndMakeVisible(transportBtn);

    // ── Steps slider (step mode; gen mode shows its own genStepsRow with FIX) ──
    // Inline bar mirroring the old 2..32 step-count dropdown: writes seqSteps
    // directly (manual, no attachment) so the grid stays capped at MAX_COLS.
    seqStepsRow = std::make_unique<SliderRow>("Steps",
        [](double v) { return juce::String(juce::roundToInt(v)); }, kSeqCol);
    seqStepsRow->setInlineLabel(true);
    seqStepsRow->getSlider().setRange(2.0, static_cast<double>(MAX_COLS), 1.0);
    seqStepsRow->getSlider().setTooltip("Number of steps in the pattern (2-32)");
    addAndMakeVisible(*seqStepsRow);
    seqStepsRow->getSlider().onValueChange = [this] {
        int steps = juce::roundToInt(seqStepsRow->getSlider().getValue());
        if (steps < 2) return;
        if (auto* par = processorRef.getValueTreeState().getParameter(PID::seqSteps))
            par->setValueNotifyingHost(
                par->getNormalisableRange().convertTo0to1(static_cast<float>(steps)));
        syncStepCount();
    };

    // ── Division (note length) switchbox — drawn note glyphs ──
    // Note symbols (1/1..1/16) keep the 5-button switchbox compact enough for the
    // tight top strip while showing every option at once. The hidden ComboBox
    // carries the APVTS attachment; the buttons are a glyph radio group over it.
    juce::StringArray divisionItems;
    for (const auto& e : SeqDivision::kEntries) divisionItems.add(e.label);
    divisionHidden.addItemList(divisionItems, 1);
    divisionHidden.onChange = [this] {
        int id = divisionHidden.getSelectedId();
        for (int i = 0; i < kNumDivBtns; ++i)
            divBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
    };
    for (int i = 0; i < kNumDivBtns; ++i)
    {
        styleSwitchButton(divBtns[i], kSeqCol);
        setSwitchGlyph(divBtns[i],
                       static_cast<SwitchGlyph>(static_cast<int>(SwitchGlyph::NoteWhole) + i));
        divBtns[i].setTooltip(divisionItems[i]);
        divBtns[i].setClickingTogglesState(true);
        divBtns[i].setRadioGroupId(2001);
        divBtns[i].onClick = [this, i] { divisionHidden.setSelectedId(i + 1); };
        addAndMakeVisible(divBtns[i]);
    }
    divA = std::make_unique<CA>(apvts, PID::seqDivision, divisionHidden);

    // ── BPM ──
    bpmRow = std::make_unique<SliderRow>("BPM", [](double v) { return juce::String(juce::roundToInt(v)); }, kSeqCol);
    bpmRow->setInlineLabel(true);
    bpmRow->getSlider().setTooltip(juce::String::fromUTF8(
        "Tempo \xe2\x80\x94 drives the step sequencer, generative sequencer, and arpeggiator"));
    addAndMakeVisible(*bpmRow);
    bpmA = std::make_unique<SA>(apvts, PID::seqBpm, bpmRow->getSlider());
    bpmRow->getSlider().onValueChange = [this] { bpmRow->updateValue(); };
    bpmRow->updateValue();

    // ── MIDI monitor ──
    midiMonitor.setColour(juce::Label::textColourId, kSuccess);
    midiMonitor.setJustificationType(juce::Justification::centredRight);
    midiMonitor.setTooltip("Incoming MIDI activity");
    addAndMakeVisible(midiMonitor);

    headerOverflowBtn.setColour(juce::TextButton::buttonColourId, kSurface);
    headerOverflowBtn.setColour(juce::TextButton::textColourOffId, kDim);
    headerOverflowBtn.setTooltip("More: division, shuffle amount, save/load pattern");
    headerOverflowBtn.onClick = [this] { showHeaderOverflowMenu(); };
    addAndMakeVisible(headerOverflowBtn);

    // ── Preset ──
    juce::StringArray seqPresetItems;
    for (const auto& e : SeqPreset::kEntries) seqPresetItems.add(e.label);
    presetBox.addItemList(seqPresetItems, 1);
    presetBox.setTooltip("Load a built-in step pattern into the grid");
    addAndMakeVisible(presetBox);
    presetA = std::make_unique<CA>(apvts, PID::seqPreset, presetBox);

    // Save/Load buttons for sequencer patterns (icon-based)
    for (auto* btn : { &seqSaveBtn, &seqLoadBtn })
    {
        btn->setColour(juce::TextButton::buttonColourId, kSurface);
        btn->setColour(juce::TextButton::textColourOffId, kDim);
        addAndMakeVisible(btn);
    }
    // Save icon (floppy disk outline)
    {
        juce::Path p;
        p.startNewSubPath(2.0f, 1.0f);
        p.lineTo(11.0f, 1.0f); p.lineTo(14.0f, 4.0f);
        p.lineTo(14.0f, 15.0f); p.lineTo(2.0f, 15.0f); p.closeSubPath();
        p.startNewSubPath(5.0f, 1.0f);
        p.lineTo(5.0f, 5.5f); p.lineTo(11.0f, 5.5f); p.lineTo(11.0f, 1.0f);
        p.startNewSubPath(4.0f, 9.0f);
        p.lineTo(12.0f, 9.0f); p.lineTo(12.0f, 14.0f); p.lineTo(4.0f, 14.0f); p.closeSubPath();
        saveLnf.icon = p;
    }
    // Load icon (folder outline)
    {
        juce::Path p;
        p.startNewSubPath(2.0f, 5.0f); p.lineTo(2.0f, 2.0f);
        p.lineTo(7.0f, 2.0f); p.lineTo(8.5f, 5.0f);
        p.lineTo(14.0f, 5.0f); p.lineTo(14.0f, 15.0f);
        p.lineTo(2.0f, 15.0f); p.closeSubPath();
        loadLnf.icon = p;
    }
    seqSaveBtn.setLookAndFeel(&saveLnf);
    seqLoadBtn.setLookAndFeel(&loadLnf);
    seqSaveBtn.setTooltip("Save pattern");
    seqLoadBtn.setTooltip("Load pattern");
    seqSaveBtn.onClick = [this] { if (onOpenPatternLibrary) onOpenPatternLibrary(true); };
    seqLoadBtn.onClick = [this] { if (onOpenPatternLibrary) onOpenPatternLibrary(false); };

    // ── Gate ──
    gateRow = std::make_unique<SliderRow>("Gate", [](double v) { return juce::String(juce::roundToInt(v*100)) + "%"; }, kSeqCol);
    gateRow->setInlineLabel(true);
    gateRow->getSlider().setTooltip("Note length as a percentage of each step");
    addAndMakeVisible(*gateRow);
    gateA = std::make_unique<SA>(apvts, PID::seqGate, gateRow->getSlider());
    gateRow->getSlider().onValueChange = [this] { gateRow->updateValue(); };
    gateRow->updateValue();

    // ── Shuffle ──
    shuffleRow = std::make_unique<SliderRow>("Shuffle", [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; }, kSeqCol);
    shuffleRow->setInlineLabel(true);
    shuffleRow->getSlider().setTooltip("Swing applied to off-beat steps");
    addAndMakeVisible(*shuffleRow);
    shuffleA = std::make_unique<SA>(apvts, PID::seqShuffle, shuffleRow->getSlider());
    shuffleRow->getSlider().onValueChange = [this] { shuffleRow->updateValue(); };
    shuffleRow->updateValue();

    // ── Octave shift [-2][-1][0][+1][+2] ──
    juce::StringArray seqOctItems;
    for (const auto& e : SeqOctave::kEntries) seqOctItems.add(e.label);
    octShiftHidden.addItemList(seqOctItems, 1);
    octShiftHidden.onChange = [this] {
        int id = octShiftHidden.getSelectedId();
        for (int i = 0; i < kNumOctShiftBtns; ++i)
            octShiftBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
    };
    for (int i = 0; i < kNumOctShiftBtns; ++i)
    {
        octShiftBtns[i].setButtonText(seqOctItems[i]);
        styleSwitchButton(octShiftBtns[i], kSeqCol);
        octShiftBtns[i].setTooltip("Octave shift for the Step and Phrase sequences");
        octShiftBtns[i].setClickingTogglesState(true);
        octShiftBtns[i].setRadioGroupId(2004);
        octShiftBtns[i].onClick = [this, i] { octShiftHidden.setSelectedId(i + 1); };
        addAndMakeVisible(octShiftBtns[i]);
    }
    octShiftA = std::make_unique<CA>(apvts, PID::seqOctave, octShiftHidden);

    // ── Generative sequencer controls ──
    // engineHidden is the HIDDEN APVTS bridge for PID::seqEngine. The visible
    // control is the mode switchbox below, which drives it; the CA keeps the
    // param in sync both ways.
    {
        juce::StringArray engineItems;
        for (const auto& e : SeqEngine::kEntries) engineItems.add(e.label);
        engineHidden.addItemList(engineItems, 1);
    }
    engineHidden.onChange = [this] {
        const int engine = engineHidden.getSelectedId() - 1;
        modeStepBtn  .setToggleState(engine == SeqEngine::Step,   juce::dontSendNotification);
        modePhraseBtn.setToggleState(engine == SeqEngine::Phrase, juce::dontSendNotification);
        modeGenBtn   .setToggleState(engine == SeqEngine::Gen,    juce::dontSendNotification);
        // Leaving Step mode cancels an in-progress step-record.
        if (engine != SeqEngine::Step && processorRef.isStepRecordArmed())
            processorRef.toggleStepRecord();
    };
    engineA = std::make_unique<CA>(apvts, PID::seqEngine, engineHidden);

    // Step | Phrase | Gen mode switchbox (visible) — segments drive the hidden
    // bridge, left to right in SeqEngine's order.
    modeStepBtn  .setConnectedEdges(juce::Button::ConnectedOnRight);
    modePhraseBtn.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    modeGenBtn   .setConnectedEdges(juce::Button::ConnectedOnLeft);
    // Step = general bright green; the two generative modes = the dark green
    // their controls use, so the lit segment's colour tells hand-written from
    // generated.
    styleSwitchButton(modeStepBtn,   kSeqCol);
    styleSwitchButton(modePhraseBtn, kSeqFill);
    styleSwitchButton(modeGenBtn,    kSeqFill);
    modeStepBtn.setTooltip("Step mode: hand-edit the note grid. Double-click to arm step recording (capture incoming MIDI into steps).");
    modePhraseBtn.setTooltip("Phrase mode: a bass line, riff, lead or stab in a key, over chords, "
                             "repeating in 4/8/16-bar phrases that vary at the phrase end");
    modeGenBtn.setTooltip("Gen mode: generative Euclidean/Turing-machine sequencer");
    for (auto* b : { &modeStepBtn, &modePhraseBtn, &modeGenBtn })
    {
        b->setClickingTogglesState(true);
        b->setRadioGroupId(2010);
        addAndMakeVisible(*b);
    }
    modeStepBtn.onClick = [this] { engineHidden.setSelectedId(SeqEngine::Step + 1, juce::sendNotificationSync); };
    // Double-click the Step segment to latch step-record (handled in mouseDoubleClick).
    modeStepBtn.addMouseListener(this, false);
    modePhraseBtn.onClick = [this] { engineHidden.setSelectedId(SeqEngine::Phrase + 1, juce::sendNotificationSync); };
    modeGenBtn   .onClick = [this] { engineHidden.setSelectedId(SeqEngine::Gen + 1,    juce::sendNotificationSync); };
    {
        const int engine = engineHidden.getSelectedId() - 1;   // initial sync from param
        modeStepBtn  .setToggleState(engine == SeqEngine::Step,   juce::dontSendNotification);
        modePhraseBtn.setToggleState(engine == SeqEngine::Phrase, juce::dontSendNotification);
        modeGenBtn   .setToggleState(engine == SeqEngine::Gen,    juce::dontSendNotification);
    }

    auto intFmt = [](double v) { return juce::String(juce::roundToInt(v)); };
    genStepsRow = std::make_unique<SliderRow>("Steps", intFmt, kSeqFill);
    genStepsRow->getSlider().setTooltip("Number of steps in strand 1's Euclidean rhythm");
    addAndMakeVisible(*genStepsRow);
    genStepsA = std::make_unique<SA>(apvts, PID::genSteps, genStepsRow->getSlider());
    genStepsRow->getSlider().onValueChange = [this] { genStepsRow->updateValue(); };
    genStepsRow->updateValue();

    genPulsesRow = std::make_unique<SliderRow>("Pulses", intFmt, kSeqFill);
    genPulsesRow->getSlider().setTooltip("Number of onsets distributed evenly across the steps");
    addAndMakeVisible(*genPulsesRow);
    genPulsesA = std::make_unique<SA>(apvts, PID::genPulses, genPulsesRow->getSlider());
    genPulsesRow->getSlider().onValueChange = [this] { genPulsesRow->updateValue(); };
    genPulsesRow->updateValue();

    genRotationRow = std::make_unique<SliderRow>("Rotation", intFmt, kSeqFill);
    genRotationRow->getSlider().setTooltip("Rotates the Euclidean pattern's starting offset");
    addAndMakeVisible(*genRotationRow);
    genRotationA = std::make_unique<SA>(apvts, PID::genRotation, genRotationRow->getSlider());
    genRotationRow->getSlider().onValueChange = [this] { genRotationRow->updateValue(); };
    genRotationRow->updateValue();

    // Range — switchbox [1][2][3][4] on the genRange Choice param. Range sets the
    // octave span of the whole generative pitch field (shared by ALL strands, not
    // S1 alone — GenerativeSequencer::setRange marks every strand dirty). The four
    // discrete values wasted space as a slider, so it is a compact switchbox; a
    // hidden ComboBox carries the APVTS attachment (mirrors the octave switchbox).
    {
        juce::StringArray rangeItems;
        for (const auto& e : GenRange::kEntries) rangeItems.add(e.label);
        genRangeHidden.addItemList(rangeItems, 1);
        genRangeHidden.onChange = [this]
        {
            const int id = genRangeHidden.getSelectedId();
            for (int i = 0; i < kNumRangeBtns; ++i)
                genRangeBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
        };
        for (int i = 0; i < kNumRangeBtns; ++i)
        {
            genRangeBtns[i].setButtonText(rangeItems[i]);
            styleSwitchButton(genRangeBtns[i], kSeqFill);   // gen-specific dark green
            genRangeBtns[i].setTooltip("Octave span of the shared pitch field (all strands)");
            genRangeBtns[i].setClickingTogglesState(true);
            genRangeBtns[i].setRadioGroupId(2011);
            genRangeBtns[i].onClick = [this, i] { genRangeHidden.setSelectedId(i + 1); };
            addAndMakeVisible(genRangeBtns[i]);
        }
        genRangeA = std::make_unique<CA>(apvts, PID::genRange, genRangeHidden);

        // "Rng" tag in front of the switchbox (it has no value text of its own).
        // White to match the inline-bar labels it sits beside (Cyc).
        genRangeLabel.setText("Rng", juce::dontSendNotification);
        genRangeLabel.setColour(juce::Label::textColourId, juce::Colours::white);
        genRangeLabel.setJustificationType(juce::Justification::centredLeft);
        genRangeLabel.setFont(juce::FontOptions(11.0f));
        genRangeLabel.setInterceptsMouseClicks(false, false);
        addAndMakeVisible(genRangeLabel);
    }

    genMutationRow = std::make_unique<SliderRow>("Evolve",
        [](double v) { return juce::String(juce::roundToInt(v * 100)) + "%"; }, kSeqFill);
    genMutationRow->getSlider().setTooltip(
        "Probability the pattern mutates each cycle (rotate / add or remove a pulse / steps drift) "
        "unless the parameter is locked with FIX");
    addAndMakeVisible(*genMutationRow);
    genMutationA = std::make_unique<SA>(apvts, PID::genMutation, genMutationRow->getSlider());
    genMutationRow->getSlider().onValueChange = [this] { genMutationRow->updateValue(); };
    genMutationRow->updateValue();

    // Euclidean gen rows render as Poly-AT-style inline bars (label + value
    // inside the track); each bar's own border replaces the old framed card.
    for (auto* r : { genStepsRow.get(), genPulsesRow.get(),
                     genRotationRow.get(), genMutationRow.get() })
        r->setInlineLabel(true);

    // Fix toggle buttons (FIX = locked against drift)
    auto setupFixBtn = [this](juce::TextButton& btn, const juce::String& tip) {
        btn.setButtonText("FIX");
        btn.setClickingTogglesState(true);
        btn.setColour(juce::TextButton::buttonColourId, kSurface);
        btn.setColour(juce::TextButton::buttonOnColourId, kSeqFill);
        btn.setColour(juce::TextButton::textColourOffId, kDim);
        btn.setColour(juce::TextButton::textColourOnId, switchBoxSelectedTextColour(kSeqFill));
        btn.setColour(juce::ComboBox::outlineColourId, kSeqFill.withAlpha(0.5f));
        btn.setTooltip(tip);
        addAndMakeVisible(btn);
    };
    setupFixBtn(genFixStepsBtn,    "Lock Steps against drift");
    setupFixBtn(genFixPulsesBtn,   "Lock Pulses against drift");
    setupFixBtn(genFixRotationBtn, "Lock Rotation against drift");
    setupFixBtn(genFixMutationBtn, "Lock Evolve against drift");
    genFixStepsA    = std::make_unique<BA>(apvts, PID::genFixSteps,    genFixStepsBtn);
    genFixPulsesA   = std::make_unique<BA>(apvts, PID::genFixPulses,   genFixPulsesBtn);
    genFixRotationA = std::make_unique<BA>(apvts, PID::genFixRotation, genFixRotationBtn);
    genFixMutationA = std::make_unique<BA>(apvts, PID::genFixMutation, genFixMutationBtn);

    // ── Polyphony (Phase 5): field mode + rate, 3 extra strands ──
    {
        juce::StringArray fieldModeItems;
        for (const auto& e : FieldMode::kEntries) fieldModeItems.add(e.label);
        genFieldModeBox.addItemList(fieldModeItems, 1);
        genFieldModeBox.setColour(juce::ComboBox::backgroundColourId, kSurface);
        genFieldModeBox.setColour(juce::ComboBox::textColourId, kSeqCol);
        genFieldModeBox.setColour(juce::ComboBox::outlineColourId, kBorder);
        genFieldModeBox.setTooltip(
            "How the shared pitch field evolves over cycles: Static (fixed), Drift (wanders), "
            "Transform (row transposition/inversion/retrograde), Pivot (rotates by a fixed interval)");
        addAndMakeVisible(genFieldModeBox);
        genFieldModeA = std::make_unique<CA>(apvts, PID::genFieldMode, genFieldModeBox);

        // Ensemble coordination mode — global (one mode for all strands),
        // hence in the harmony row rather than any strand module.
        juce::StringArray coordItems;
        for (const auto& e : CoordinationMode::kEntries) coordItems.add(e.label);
        genCoordModeBox.addItemList(coordItems, 1);
        genCoordModeBox.setColour(juce::ComboBox::backgroundColourId, kSurface);
        genCoordModeBox.setColour(juce::ComboBox::textColourId, kSeqCol);
        genCoordModeBox.setColour(juce::ComboBox::outlineColourId, kBorder);
        genCoordModeBox.setTooltip(
            "How strands relate to each other: Independent, Density Budget (caps simultaneous "
            "onsets by role), Dialog (per-strand follow/counter stance toward Strand 1)");
        addAndMakeVisible(genCoordModeBox);
        genCoordModeA = std::make_unique<CA>(apvts, PID::genCoordinationMode, genCoordModeBox);

        // Cyc — inline slider on the genFieldRate Int param (1..32 cycles).
        genCycRow = std::make_unique<SliderRow>("Cyc", intFmt, kSeqFill);
        genCycRow->setInlineLabel(true);
        genCycRow->getSlider().setTooltip("Cycles between pitch-field evolution steps (Drift/Transform/Pivot)");
        addAndMakeVisible(*genCycRow);
        genFieldRateA = std::make_unique<SA>(apvts, PID::genFieldRate, genCycRow->getSlider());
        genCycRow->getSlider().onValueChange = [this] { genCycRow->updateValue(); };
        genCycRow->updateValue();
    }
    {
        juce::StringArray roleItems;
        for (const auto& e : StrandRole::kEntries) roleItems.add(e.label);
        static const char* kEnablePIDs[kNumExtraStrands] = {
            PID::gen2Enable, PID::gen3Enable, PID::gen4Enable, PID::gen5Enable
        };
        static const char* kRolePIDs[kNumExtraStrands] = {
            PID::gen2Role, PID::gen3Role, PID::gen4Role, PID::gen5Role
        };
        static const char* kStrandLabels[kNumExtraStrands] = { "S2", "S3", "S4", "S5" };

        for (int i = 0; i < kNumExtraStrands; ++i)
        {
            strandEnableBtns[i].setButtonText(kStrandLabels[i]);
            strandEnableBtns[i].setClickingTogglesState(true);
            strandEnableBtns[i].setColour(juce::TextButton::buttonColourId,   kSurface);
            strandEnableBtns[i].setColour(juce::TextButton::buttonOnColourId, kSeqFill);
            strandEnableBtns[i].setColour(juce::TextButton::textColourOffId,  kDim);
            strandEnableBtns[i].setColour(juce::TextButton::textColourOnId,   switchBoxSelectedTextColour(kSeqFill));
            strandEnableBtns[i].setTooltip("Enable polyphonic strand " + juce::String(i + 2));
            addAndMakeVisible(strandEnableBtns[i]);
            strandEnableA[i] = std::make_unique<BA>(apvts, kEnablePIDs[i], strandEnableBtns[i]);

            strandRoleBoxes[i].addItemList(roleItems, 1);
            strandRoleBoxes[i].setColour(juce::ComboBox::backgroundColourId, kSurface);
            strandRoleBoxes[i].setColour(juce::ComboBox::textColourId, kSeqCol);
            strandRoleBoxes[i].setColour(juce::ComboBox::outlineColourId, kBorder);
            strandRoleBoxes[i].setTooltip(juce::String(kStrandLabels[i])
                + " textural role in the ensemble (Anchor/Line/Density/Gesture)");
            addAndMakeVisible(strandRoleBoxes[i]);
            strandRoleA[i] = std::make_unique<CA>(apvts, kRolePIDs[i], strandRoleBoxes[i]);
        }

        // Second GEN row: per-strand Div× / Octave / Dominance, one cluster
        // per strand, aligned with the [Sx][Role] cluster above.
        static const char* kDivPIDs[kNumExtraStrands] = {
            PID::gen2DivMult, PID::gen3DivMult, PID::gen4DivMult, PID::gen5DivMult
        };
        static const char* kOctPIDs[kNumExtraStrands] = {
            PID::gen2Octave, PID::gen3Octave, PID::gen4Octave, PID::gen5Octave
        };
        static const char* kDomPIDs[kNumExtraStrands] = {
            PID::gen2Dominance, PID::gen3Dominance, PID::gen4Dominance, PID::gen5Dominance
        };

        juce::StringArray divItems;
        for (const auto& e : StrandDivMult::kEntries) divItems.add(e.label);

        for (int i = 0; i < kNumExtraStrands; ++i)
        {
            const juce::String sName = "Strand " + juce::String(i + 2);

            // Div: ComboBox is its own label, sorted by multiplier.
            strandDivBoxes[i].addItemList(divItems, 1);
            strandDivBoxes[i].setColour(juce::ComboBox::backgroundColourId, kSurface);
            strandDivBoxes[i].setColour(juce::ComboBox::textColourId, kSeqCol);
            strandDivBoxes[i].setColour(juce::ComboBox::outlineColourId, kBorder);
            strandDivBoxes[i].setTooltip(sName + " tempo multiplier (creates polymeter)");
            addAndMakeVisible(strandDivBoxes[i]);
            strandDivA[i] = std::make_unique<CA>(apvts, kDivPIDs[i], strandDivBoxes[i]);

            // Oct: hidden Slider as APVTS bridge + 5 visible toggle buttons,
            // matching the existing Seq-Octave switch convention.
            strandOctaveSliders[i].setRange(-2.0, 2.0, 1.0);
            strandOctaveSliders[i].setVisible(false);
            addChildComponent(strandOctaveSliders[i]);
            strandOctaveA[i] = std::make_unique<SA>(apvts, kOctPIDs[i], strandOctaveSliders[i]);
            strandOctaveSliders[i].onValueChange = [this, i] {
                const int v = juce::roundToInt(strandOctaveSliders[i].getValue());
                for (int b = 0; b < kStrandOctBtns; ++b)
                    strandOctBtns[i][b].setToggleState(b - 2 == v, juce::dontSendNotification);
            };
            for (int b = 0; b < kStrandOctBtns; ++b)
            {
                strandOctBtns[i][b].setButtonText(SeqOctave::kEntries[b].label);
                styleSwitchButton(strandOctBtns[i][b], kSeqFill);
                strandOctBtns[i][b].setClickingTogglesState(true);
                strandOctBtns[i][b].setRadioGroupId(3001 + i); // unique per strand
                strandOctBtns[i][b].setTooltip(sName + " octave shift");
                strandOctBtns[i][b].onClick = [this, i, b] {
                    strandOctaveSliders[i].setValue(static_cast<double>(b - 2));
                };
                addAndMakeVisible(strandOctBtns[i][b]);
            }
            strandOctaveSliders[i].onValueChange();   // initial toggle sync

            // Gravity: full-module-width inline bar (Poly-AT style), 0..1.
            // (Display name "Gravity" per BJ 2026-07-16; param IDs and preset
            // keys keep "dominance".)
            strandDomRows[i] = std::make_unique<SliderRow>("Gravity",
                [](double v) { return juce::String(v, 2); }, kSeqFill);
            strandDomRows[i]->setInlineLabel(true);
            strandDomRows[i]->getSlider().setTooltip(sName
                + juce::String::fromUTF8(" gravity \xe2\x80\x94 probability of snapping to field "
                                         "center at the cycle downbeat (0..1)"));
            addAndMakeVisible(*strandDomRows[i]);
            strandDomA[i] = std::make_unique<SA>(apvts, kDomPIDs[i], strandDomRows[i]->getSlider());
            strandDomRows[i]->getSlider().onValueChange = [this, i] { strandDomRows[i]->updateValue(); };
            strandDomRows[i]->updateValue();
        }
    }

    genMonitor = std::make_unique<GenSeqCRTMonitor> (processorRef.getGenerativeSequencer());
    addAndMakeVisible (*genMonitor);
    genMonitor->setVisible (false);   // shown by resized() in GEN mode

    juce::StringArray scaleRootItems;
    for (const auto& e : ScaleRoot::kEntries) scaleRootItems.add(e.label);
    genScaleRootBox.addItemList(scaleRootItems, 1);
    genScaleRootBox.setColour(juce::ComboBox::backgroundColourId, kSurface);
    genScaleRootBox.setColour(juce::ComboBox::textColourId, kSeqCol);
    genScaleRootBox.setColour(juce::ComboBox::outlineColourId, kBorder);
    genScaleRootBox.setTooltip("Root note of the key the Gen and Phrase pitches are built on");
    addAndMakeVisible(genScaleRootBox);
    genScaleRootA = std::make_unique<CA>(apvts, PID::scaleRoot, genScaleRootBox);

    juce::StringArray scaleTypeItems;
    for (const auto& e : ScaleType::kEntries) scaleTypeItems.add(e.label);
    genScaleTypeBox.addItemList(scaleTypeItems, 1);
    genScaleTypeBox.setColour(juce::ComboBox::backgroundColourId, kSurface);
    genScaleTypeBox.setColour(juce::ComboBox::textColourId, kSeqCol);
    genScaleTypeBox.setColour(juce::ComboBox::outlineColourId, kBorder);
    genScaleTypeBox.setTooltip("Scale/mode the Gen and Phrase pitches are quantized to");
    addAndMakeVisible(genScaleTypeBox);
    genScaleTypeA = std::make_unique<CA>(apvts, PID::scaleType, genScaleTypeBox);

    // ── Phrase mode controls (docs/PHRASE_SEQ_CONCEPT.md §4) ──
    // Three rows in the order of the use case: what (Part), in which key and
    // octave, over which harmony, in which rhythm, and how it unfolds over
    // the phrase. Dark green like Gen: generated, not hand-written. The chord
    // and Layer combos are built before the switches whose onSelect
    // relabels them, since an attachment selects at construction.
    {
        static_assert(std::size(kPhraseChordPIDs) == kNumChordSlots);
        const auto chordItems = choiceLabels(PhrChord::kEntries);   // relabelled per scale
        for (int i = 0; i < kNumChordSlots; ++i)
        {
            auto& box = phrChordBoxes[static_cast<size_t>(i)];
            box.addItemList(chordItems, 1);
            styleSeqCombo(box);
            box.setTooltip("Chord slot " + juce::String(i + 1) + ": a degree of the key, or - for none. "
                           "The slots in play loop, one chord per Rate bars (Harm = Chords)");
            addAndMakeVisible(box);
            phrChordA[static_cast<size_t>(i)] = std::make_unique<CA>(apvts, kPhraseChordPIDs[i], box);
        }

        phrLayerBox.addItemList(choiceLabels(PhrLayer::kEntries), 1);
        styleSeqCombo(phrLayerBox);
        phrLayerBox.setTooltip("A second line: Octave doubles the line an octave away, Chord plays the "
                               "chord on its heads (not with Stab), Answer is a second motif heard in "
                               "the B parts of the form");
        phrLayerBox.onChange = [this] { refreshPhraseLayerItems(); };
        addAndMakeVisible(phrLayerBox);
        phrLayerA = std::make_unique<CA>(apvts, PID::phrLayer, phrLayerBox);

        phrFormBox.addItemList(choiceLabels(PhrForm::kEntries), 1);
        styleSeqCombo(phrFormBox);
        phrFormBox.setTooltip("The phrase's form, one letter per quarter: A is the motif, B the motif "
                              "varied by Var. Var also varies the phrase's last bar");
        addAndMakeVisible(phrFormBox);
        phrFormA = std::make_unique<CA>(apvts, PID::phrForm, phrFormBox);

        setupPhraseSwitch(phrPartSwitch, "Part", choiceLabels(PhrPart::kEntries), 2020,
            "What the phrase plays: a Bass, Riff or Lead line, or Stab chords on every pulse");
        phrPartSwitch.onSelect = [this] { refreshPhraseLayerItems(); };
        phrPartA = std::make_unique<CA>(apvts, PID::phrPart, phrPartSwitch.hidden);

        setupPhraseSwitch(phrHarmSwitch, "Harm", choiceLabels(PhrHarm::kEntries), 2021,
            "Where the chord comes from: Pedal (the tonic throughout), Keys (held keys set it and "
            "do not sound), Chords (the chord slots)");
        phrHarmSwitch.onSelect = [this] { refreshPhraseChordSlots(); };
        phrHarmA = std::make_unique<CA>(apvts, PID::phrHarm, phrHarmSwitch.hidden);

        setupPhraseSwitch(phrRateSwitch, "Rate", choiceLabels(PhrChordRate::kEntries), 2022,
            "Bars per chord of the chord slots");
        phrRateA = std::make_unique<CA>(apvts, PID::phrChordRate, phrRateSwitch.hidden);

        // Ext is a Bool: the hidden combo's two items are false and true.
        setupPhraseSwitch(phrExtSwitch, "Ext", { "3", "7" }, 2023,
            "Chords as triads (3) or with their seventh (7)");
        phrExtA = std::make_unique<CA>(apvts, PID::phrExt7, phrExtSwitch.hidden);

        setupPhraseSwitch(phrLenSwitch, "Len", choiceLabels(PhrLen::kEntries), 2024,
            "Motif length in bars");
        phrLenA = std::make_unique<CA>(apvts, PID::phrLen, phrLenSwitch.hidden);

        setupPhraseSwitch(phrBarsSwitch, "Bars", choiceLabels(PhrBars::kEntries), 2025,
            "Phrase length in bars; the form divides it into quarters");
        phrBarsA = std::make_unique<CA>(apvts, PID::phrBars, phrBarsSwitch.hidden);

        styleTag(phrKeyTag, "Key");     addAndMakeVisible(phrKeyTag);
        styleTag(phrOctTag, "Oct");     addAndMakeVisible(phrOctTag);
        styleTag(phrLayerTag, "Layer"); addAndMakeVisible(phrLayerTag);
        styleTag(phrFormTag, "Form");   addAndMakeVisible(phrFormTag);

        auto pct = [](double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; };
        auto makeRow = [&](std::unique_ptr<SliderRow>& row, std::unique_ptr<SA>& attachment,
                           const char* name, const char* pid,
                           std::function<juce::String(double)> format, const juce::String& tip)
        {
            row = std::make_unique<SliderRow>(name, std::move(format), kSeqFill);
            row->setInlineLabel(true);
            row->getSlider().setTooltip(tip);
            addAndMakeVisible(*row);
            attachment = std::make_unique<SA>(apvts, pid, row->getSlider());
            row->updateValue();
        };
        makeRow(phrSlideRow, phrSlideA, "Slide", PID::phrSlide, pct,
                "Chance that a passing or approach note glides into the head after it");
        makeRow(phrPulsesRow, phrPulsesA, "Pulses", PID::phrPulses, intFmt,
                "Onsets in the motif, spread evenly over its steps");
        makeRow(phrRotationRow, phrRotationA, "Rot", PID::phrRotation, intFmt,
                "Rotates the motif's rhythm by steps");
        makeRow(phrVariationRow, phrVariationA, "Var", PID::phrVariation, pct,
                "How much the B parts and each phrase's last bar vary: re-pitch, lift, fill, "
                "ratchet, drop, added in that order");
        makeRow(phrEvolveRow, phrEvolveA, "Evolve", PID::phrEvolve, pct,
                "How many motif steps change their note at each phrase boundary; the rhythm "
                "stays. 0 is a locked loop");

        auto styleAction = [this](juce::TextButton& b, const juce::String& tip)
        {
            b.setColour(juce::TextButton::buttonColourId, kSurface);
            b.setColour(juce::TextButton::textColourOffId, kSeqCol);
            b.setTooltip(tip);
            addAndMakeVisible(b);
        };
        styleAction(phrDiceBtn, "A new motif from a new seed, for the current Part, Len, Pulses and Rot");
        styleAction(phrKeepBtn, "Keep the motif as it plays now (evolved) as the stored motif");
        styleAction(phrUndoBtn, "Back to the motif before the last Dice, Keep or From Step; "
                                "press again to redo");
        styleAction(phrFromStepBtn, "Take the step pattern (up to its first two bars) as the motif and "
                                    "set Len, Pulses and Rot to match. Needs a step switched on");
        // Each action can change whether there is something to undo.
        phrDiceBtn.onClick     = [this] { processorRef.phraseDice();      phrUndoBtn.setEnabled(processorRef.canPhraseUndo()); };
        phrKeepBtn.onClick     = [this] { processorRef.phraseKeep();      phrUndoBtn.setEnabled(processorRef.canPhraseUndo()); };
        phrUndoBtn.onClick     = [this] { processorRef.phraseUndo();      phrUndoBtn.setEnabled(processorRef.canPhraseUndo()); };
        phrFromStepBtn.onClick = [this] { processorRef.phraseFromSteps(); phrUndoBtn.setEnabled(processorRef.canPhraseUndo()); };
        phrUndoBtn.setEnabled(processorRef.canPhraseUndo());

        // The chord slots' labels follow the scale. The CA listens to the
        // combo, so its onChange is free; it also fires when the attachment
        // follows automation or a load.
        genScaleTypeBox.onChange = [this] { refreshPhraseChordSlots(); };
        refreshPhraseChordSlots();
        refreshPhraseLayerItems();
    }

    // ── Arp controls (SwitchBox: OFF/Up/Dn/U-D/Rnd) ──
    juce::StringArray arpModeItems;
    for (const auto& e : ArpMode::kEntries) arpModeItems.add(e.label);
    arpModeBox.addItemList(arpModeItems, 1);
    arpModeBox.onChange = [this] {
        int id = arpModeBox.getSelectedId();
        for (int i = 0; i < kNumModeBtns; ++i)
            arpModeBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
    };
    for (int i = 0; i < kNumModeBtns; ++i)
    {
        styleSwitchButton(arpModeBtns[i], kSeqCol);
        setSwitchGlyph(arpModeBtns[i],
                       static_cast<SwitchGlyph>(static_cast<int>(SwitchGlyph::ArpOff) + i));
        arpModeBtns[i].setTooltip(arpModeItems[i]);
        arpModeBtns[i].setClickingTogglesState(true);
        arpModeBtns[i].setRadioGroupId(2003);
        arpModeBtns[i].onClick = [this, i] { arpModeBox.setSelectedId(i + 1); };
        addAndMakeVisible(arpModeBtns[i]);
    }
    arpModeA = std::make_unique<CA>(apvts, PID::arpMode, arpModeBox);

    // "ARP" left-header: accent band over the full height, left of the mode
    // switchbox (same grammar as RE-PROMPT/VARIATION; dark bold title on kSeqCol).
    paintSectionHeader(arpModeLabel, "ARP", kSeqCol);
    arpModeLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(arpModeLabel);

    juce::StringArray arpRateItems;
    for (const auto& e : ArpRate::kEntries) arpRateItems.add(e.label);
    arpRateBox.addItemList(arpRateItems, 1);
    arpRateBox.setColour(juce::ComboBox::backgroundColourId, kSurface);
    arpRateBox.setColour(juce::ComboBox::textColourId, kSeqCol);
    arpRateBox.setColour(juce::ComboBox::outlineColourId, kBorder);
    arpRateBox.setTooltip("Arpeggiator note rate");
    addAndMakeVisible(arpRateBox);
    arpRateA = std::make_unique<CA>(apvts, PID::arpRate, arpRateBox);

    // "OCT" left-header (see ARP)
    paintSectionHeader(arpOctLabel, "OCT", kSeqCol);
    arpOctLabel.setInterceptsMouseClicks(false, false);
    addAndMakeVisible(arpOctLabel);

    // Arp octaves: toggle buttons [1][2][3][4] with hidden ComboBox for APVTS
    arpOctHidden.addItemList({"1","2","3","4"}, 1);
    arpOctHidden.onChange = [this] {
        int id = arpOctHidden.getSelectedId();
        for (int i = 0; i < kNumOctBtns; ++i)
            arpOctBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
    };
    for (int i = 0; i < kNumOctBtns; ++i)
    {
        arpOctBtns[i].setButtonText(juce::String(i + 1));
        styleSwitchButton(arpOctBtns[i], kSeqCol);
        arpOctBtns[i].setTooltip(juce::String::fromUTF8(
            "Arpeggiator octave range \xe2\x80\x94 repeats the arpeggio across this many octaves"));
        arpOctBtns[i].setClickingTogglesState(true);
        arpOctBtns[i].setRadioGroupId(2002);
        arpOctBtns[i].onClick = [this, i] { arpOctHidden.setSelectedId(i + 1); };
        addAndMakeVisible(arpOctBtns[i]);
    }
    arpOctA = std::make_unique<CA>(apvts, PID::arpOctaves, arpOctHidden);

    // ── Step columns ──
    for (int i = 0; i < MAX_COLS; ++i)
    {
        stepCols[static_cast<size_t>(i)] = std::make_unique<StepColumn>();
        stepCols[static_cast<size_t>(i)]->stepIndex = i;
        stepCols[static_cast<size_t>(i)]->processor = &p;
        stepCols[static_cast<size_t>(i)]->setTooltip(juce::String::fromUTF8(
            "Note: drag vertically to set pitch (hold Shift to preview while dragging) \xe2\x80\xa2 "
            "Sample slots 1-3: click to cycle playback mode, right-click to clear, drag to/from the "
            "waveform or another slot to capture/copy \xe2\x80\xa2 "
            "Velocity: drag horizontally \xe2\x80\xa2 "
            "On/Bind: click to toggle the step; Bind cycles Off \xe2\x86\x92 Bind \xe2\x86\x92 Glide"));
        addAndMakeVisible(*stepCols[static_cast<size_t>(i)]);
    }

    syncStepCount();
    startTimerHz(10);
}

SequencerPanel::~SequencerPanel()
{
    stopTimer();
}

// .t5seq is a partial preset: sequencer + arpeggiator + generative seq +
// phrase seq only.
// Schema is the snake_case-keyed subset of exportJsonPreset()/importJsonPreset()
// — single source of truth, no schema drift.
bool SequencerPanel::writePatternTo(const juce::File& file)
{
    auto fullJson = processorRef.exportJsonPreset();
    auto parsed = juce::JSON::parse(fullJson);
    auto* full = parsed.getDynamicObject();
    if (!full) return false;

    juce::DynamicObject::Ptr out = new juce::DynamicObject();
    out->setProperty("version", 1);
    out->setProperty("kind", "t5seq");
    out->setProperty("timestamp", juce::Time::getCurrentTime().toISO8601(true));

    for (const char* key : { "sequencer", "arpeggiator", "generativeSeq", "phraseSeq" })
    {
        if (!full->hasProperty(key))
            continue;

        if (juce::String(key) == "sequencer")
        {
            auto seqCopy = juce::JSON::parse(juce::JSON::toString(full->getProperty(key)));
            if (auto* seq = seqCopy.getDynamicObject())
            {
                seq->removeProperty("oneShotSamples");
                if (auto* steps = seq->getProperty("steps").getArray())
                    for (auto& step : *steps)
                        if (auto* stepObj = step.getDynamicObject())
                            stepObj->removeProperty("oneShots");
                out->setProperty(key, seqCopy);
            }
        }
        else
        {
            out->setProperty(key, full->getProperty(key));
        }
    }

    return file.replaceWithText(juce::JSON::toString(out.get(), true));
}

bool SequencerPanel::loadPatternFrom(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    // importJsonPreset is tolerant: missing sub-objects (synth, engine, …)
    // are skipped, so a partial .t5seq only touches sequencer / arp / gen /
    // phrase. It publishes the grid it writes (markPatternReplaced), and the
    // timer redraws the grid from that, with the audio idle too.
    return processorRef.importJsonPreset(file.loadFileAsString());
}

void SequencerPanel::showHeaderOverflowMenu()
{
    juce::PopupMenu menu;

    juce::PopupMenu divisionMenu;
    for (int i = 0; i < divisionHidden.getNumItems(); ++i)
        divisionMenu.addItem(kOverflowDivisionBase + i + 1, divisionHidden.getItemText(i), true,
                             divisionHidden.getSelectedId() == i + 1);
    menu.addSubMenu("Division", divisionMenu);

    juce::PopupMenu shuffleMenu;
    const int currentShufflePct = juce::roundToInt(shuffleRow->getSlider().getValue() * 100.0);
    for (size_t i = 0; i < std::size(kOverflowShuffleValues); ++i)
    {
        const int pct = kOverflowShuffleValues[i];
        shuffleMenu.addItem(kOverflowShuffleBase + static_cast<int>(i),
                            pct == 0 ? juce::String("Off") : juce::String(pct) + "%",
                            true, currentShufflePct == pct);
    }
    menu.addSubMenu("Shuffle", shuffleMenu);

    menu.addSeparator();
    menu.addItem(kOverflowSavePattern, "Save Pattern...");
    menu.addItem(kOverflowLoadPattern, "Load Pattern...");

    juce::Component::SafePointer<SequencerPanel> safeThis(this);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&headerOverflowBtn),
                       [safeThis](int result) {
        if (!safeThis || result == 0) return;

        if (result == kOverflowSavePattern) { if (safeThis->onOpenPatternLibrary) safeThis->onOpenPatternLibrary(true);  return; }
        if (result == kOverflowLoadPattern) { if (safeThis->onOpenPatternLibrary) safeThis->onOpenPatternLibrary(false); return; }
        if (result >= kOverflowDivisionBase && result < kOverflowOctaveBase)
        {
            safeThis->divisionHidden.setSelectedId(result - kOverflowDivisionBase, juce::sendNotificationSync);
            return;
        }
        if (result >= kOverflowShuffleBase
            && result < kOverflowShuffleBase + static_cast<int>(std::size(kOverflowShuffleValues)))
        {
            const int pct = kOverflowShuffleValues[result - kOverflowShuffleBase];
            safeThis->shuffleRow->getSlider().setValue(pct / 100.0, juce::sendNotificationSync);
        }
    });
}

void SequencerPanel::syncStepCount()
{
    int steps = static_cast<int>(processorRef.getValueTreeState()
                    .getRawParameterValue(PID::seqSteps)->load());
    numVisibleSteps = juce::jlimit(2, MAX_COLS, steps);

    for (int i = 0; i < MAX_COLS; ++i)
        stepCols[static_cast<size_t>(i)]->setVisible(i < numVisibleSteps);

    seqStepsRow->getSlider().setValue(numVisibleSteps, juce::dontSendNotification);
    seqStepsRow->updateValue();
    resized();
}

void SequencerPanel::showCurrentStep(int step)
{
    if (step == currentStep)
        return;
    for (int i : { currentStep, step })
    {
        if (i < 0 || i >= MAX_COLS)
            continue;
        auto& col = *stepCols[static_cast<size_t>(i)];
        col.isCurrentStep = (i == step);
        col.repaint();
    }
    currentStep = step;
}

int SequencerPanel::playedStep() const
{
    // The audio thread clears the playhead only in its first block after a
    // stop, so a tick between the STOP click and that block would read the
    // played step and light again the column the click put out. The transport
    // parameter is what starts and stops Step playback, and it reads stopped
    // from the click on.
    if (processorRef.getValueTreeState().getRawParameterValue(PID::seqRunning)->load() <= 0.5f)
        return -1;
    return processorRef.getStepSequencer().currentStepForGui.load(std::memory_order_relaxed);
}

void SequencerPanel::setupPhraseSwitch(PhraseSwitch& sw, const juce::String& tag,
                                       const juce::StringArray& items, int radioGroupId,
                                       const juce::String& tooltip)
{
    styleTag(sw.tag, tag);
    addAndMakeVisible(sw.tag);

    // The hidden combo is the attachment's target (as arpModeBox): a segment
    // click selects its item, the attachment writes the parameter, and a
    // parameter change from anywhere selects the item, which sets the
    // segments. Synchronous, so the segments follow within the click.
    sw.hidden.addItemList(items, 1);
    for (int i = 0; i < items.size(); ++i)
    {
        auto btn = std::make_unique<juce::TextButton>(items[i]);
        styleSwitchButton(*btn, kSeqFill);
        btn->setTooltip(tooltip);
        btn->setClickingTogglesState(true);
        btn->setRadioGroupId(radioGroupId);
        btn->onClick = [&sw, i] { sw.hidden.setSelectedId(i + 1, juce::sendNotificationSync); };
        addAndMakeVisible(*btn);
        sw.segments.push_back(std::move(btn));
    }
    sw.hidden.onChange = [&sw]
    {
        const int id = sw.hidden.getSelectedId();
        for (size_t i = 0; i < sw.segments.size(); ++i)
            sw.segments[i]->setToggleState(static_cast<int>(i) + 1 == id, juce::dontSendNotification);
        if (sw.onSelect)
            sw.onSelect();
    };
}

int SequencerPanel::phraseSwitchWidth(PhraseSwitch& sw, int rowH)
{
    int w = tagWidth(sw.tag);
    for (auto& seg : sw.segments)
        w += labelButtonWidth(*seg, rowH);
    return w;
}

void SequencerPanel::layoutPhraseSwitch(PhraseSwitch& sw, juce::Rectangle<int>& row, int rowH)
{
    sw.tag.setBounds(row.removeFromLeft(juce::jmin(tagWidth(sw.tag), row.getWidth())));
    const int n = static_cast<int>(sw.segments.size());
    for (int i = 0; i < n; ++i)
    {
        auto& seg = *sw.segments[static_cast<size_t>(i)];
        int edges = 0;
        if (i > 0)     edges |= juce::Button::ConnectedOnLeft;
        if (i < n - 1) edges |= juce::Button::ConnectedOnRight;
        seg.setConnectedEdges(edges);
        seg.setBounds(row.removeFromLeft(juce::jmin(labelButtonWidth(seg, rowH), row.getWidth())));
    }
    sw.frame = n > 0 ? sw.segments.front()->getBounds().getUnion(sw.segments.back()->getBounds())
                     : juce::Rectangle<int>{};
}

std::vector<juce::Component*> SequencerPanel::phraseControls()
{
    // Phrase-only controls. Key and Oct are the shared scale boxes and octave
    // switch, which resized() shows for Phrase alongside these.
    std::vector<juce::Component*> c {
        &phrKeyTag, &phrOctTag, &phrLayerTag, &phrFormTag, &phrLayerBox, &phrFormBox,
        phrSlideRow.get(), phrPulsesRow.get(), phrRotationRow.get(),
        phrVariationRow.get(), phrEvolveRow.get(),
        &phrDiceBtn, &phrKeepBtn, &phrUndoBtn, &phrFromStepBtn
    };
    for (auto& box : phrChordBoxes)
        c.push_back(&box);
    for (auto* sw : { &phrPartSwitch, &phrHarmSwitch, &phrRateSwitch,
                      &phrExtSwitch, &phrLenSwitch, &phrBarsSwitch })
    {
        c.push_back(&sw->tag);
        for (auto& seg : sw->segments)
            c.push_back(seg.get());
    }
    return c;
}

// Both refreshes run from combo onChange handlers, so they read the combos,
// never getRawParameterValue(): JUCE notifies a parameter's listeners in
// reverse order of registration (AudioProcessorParameter::
// sendValueChangedMessageToListeners), so for a change made on the message
// thread (a preset load, From Step writing Len/Pulses/Rot) the attachment
// sets its combo, and the combo calls onChange, before the APVTS adapter
// has stored the new value. The attachments' combos already hold it.
// Called on events only (Harm, scale type, Part, Layer, construction), never
// per tick; none of the calls below repaints when nothing changes.
void SequencerPanel::refreshPhraseChordSlots()
{
    const int scaleType = juce::jmax(0, genScaleTypeBox.getSelectedItemIndex());
    const bool hasChords = PhraseModel::chordsAvailable(scaleType);
    const int degrees    = PhraseModel::degreeCount(scaleType);
    const bool slotsInPlay = phrHarmSwitch.hidden.getSelectedItemIndex() == PhrHarm::Chords
                             && hasChords;

    for (auto& box : phrChordBoxes)
    {
        // Read before relabelling: every relabel ends by re-selecting, so the
        // shown text matches its item here (getSelectedId() is 0 otherwise).
        const int selectedId = box.getSelectedId();
        for (int d = 1; d <= PhrChord::D7; ++d)
        {
            const bool inScale = hasChords && d <= degrees;
            juce::String text;
            if (! hasChords)
                text = juce::String(d);   // chromatic: no degrees, no chords
            else if (inScale)
                text = juce::String::fromUTF8(PhraseModel::chordLabelText(
                           PhraseModel::chordLabel(d - 1, scaleType)).c_str());
            else
                text = "(" + juce::String(d) + ")";   // beyond a pentatonic's five
            box.changeItemText(d + 1, text);
            box.setItemEnabled(d + 1, inScale);
        }
        // Re-selecting shows the relabelled text (setSelectedId compares the
        // text too) without writing the parameter.
        box.setSelectedId(selectedId, juce::dontSendNotification);
        box.setEnabled(slotsInPlay);
        // The panel's combo look barely changes when disabled; dim it like
        // the BPM bar under an external clock.
        box.setAlpha(slotsInPlay ? 1.0f : 0.45f);
    }
    for (auto& seg : phrRateSwitch.segments)
        seg->setEnabled(slotsInPlay);
}

void SequencerPanel::refreshPhraseLayerItems()
{
    // Stab plays the chord already: its Chord layer does nothing. Not
    // offered for Stab, and dimmed when it is set and Part becomes Stab.
    const bool stab = phrPartSwitch.hidden.getSelectedItemIndex() == PhrPart::Stab;
    const bool chordLayer = phrLayerBox.getSelectedItemIndex() == PhrLayer::Chord;
    phrLayerBox.setItemEnabled(PhrLayer::Chord + 1, ! stab);
    phrLayerBox.setAlpha(stab && chordLayer ? 0.45f : 1.0f);
}

void SequencerPanel::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Double-click on the Step mode segment toggles step-record (latched).
    if (e.eventComponent == &modeStepBtn)
        processorRef.toggleStepRecord();
}

void SequencerPanel::timerCallback()
{
    const int engine = static_cast<int>(processorRef.getValueTreeState()
        .getRawParameterValue(PID::seqEngine)->load());
    const bool genRunning = engine == SeqEngine::Gen;
    const bool stepMode   = engine == SeqEngine::Step;
    bool seqRunning = processorRef.getValueTreeState()
        .getRawParameterValue(PID::seqRunning)->load() > 0.5f;

    // Mode changes must update the layout even while audio is idle. Otherwise
    // a switch away from GEN stays visually in gen mode until PLAY wakes the
    // timer path. Entering Step also re-reads the step count: a load or
    // automation may have changed it while another mode was shown. The
    // playhead highlight is re-read by resized() whenever it lays out Step.
    if (engine != layoutEngine)
    {
        if (stepMode)
        {
            syncStepCount();   // lays the panel out
        }
        else
        {
            resized();
        }
        repaint();
    }

    // A step preset, an engine switch's transfer or a preset import rewrites
    // the pattern — possibly while stopped, and possibly after this timer
    // already laid out the switch into Step. Refresh the grid once when it
    // happens, BEFORE the idle early-return below, or a pattern replaced while
    // stopped would never appear.
    int patternGen = processorRef.getStepSequencer().patternReplacedGen.load(std::memory_order_acquire);
    if (patternGen != lastPatternGen)
    {
        lastPatternGen = patternGen;
        if (stepMode)
        {
            syncStepCount();   // step count + column visibility follow the new pattern
            repaint();         // redraw its notes
        }
    }

    // The step count set from outside the panel (Init, host automation, a
    // load), also above the idle early-return: it is configuration, seen while
    // stopped. Compare against the CLAMPED value, not the raw param. seqSteps'
    // range is 1..64 but the grid caps at [2, MAX_COLS]; syncStepCount()
    // clamps into that window and never writes the param back (its slider set
    // is dontSend). A raw value outside the window (1, or 33..64 from a
    // preset/DAW automation) would otherwise keep steps != numVisibleSteps
    // permanently true, firing syncStepCount() (and its resized()) every
    // 100 ms — an idle-CPU regression.
    if (stepMode)
    {
        const int steps = static_cast<int>(processorRef.getValueTreeState()
                              .getRawParameterValue(PID::seqSteps)->load());
        if (juce::jlimit(2, MAX_COLS, steps) != numVisibleSteps)
            syncStepCount();
    }

    // ── Step-record (double-click Step latch) ───────────────────────────────
    // Runs ABOVE the idle early-return below: you record into steps with the
    // transport stopped. When disarmed this is just two atomic reads per tick.
    {
        const bool armed = processorRef.isStepRecordArmed();

        if (armed != recordArmedDisplayed)
        {
            recordArmedDisplayed = armed;
            // Red fill + "REC" label while recording. The colour flip alone is a
            // red↔green change (CVD-ambiguous), so the label change is the
            // accessible cue. kError's perceived brightness (0.515) lands just
            // under switchBoxSelectedTextColour's 0.55 white/ink threshold, so it
            // would pick white (3.76:1 — sub-AA); force dark ink for REC (5.05:1,
            // matching how the green Step state is inked).
            const auto accent = armed ? kError : kSeqCol;
            modeStepBtn.setColour(juce::TextButton::buttonOnColourId, accent);
            modeStepBtn.setColour(juce::TextButton::textColourOnId,
                                  armed ? kBg : switchBoxSelectedTextColour(accent));
            modeStepBtn.setButtonText(armed ? "REC" : "Step");
            modeStepBtn.repaint();
        }

        if (armed)
            processorRef.drainStepRecordQueue();   // write any MIDI-played notes into steps

        // Move the record cursor highlight; repaint only the affected columns
        // (the just-written one + the new target).
        const int recCur = armed ? processorRef.getStepRecordCursor() : -1;
        if (recCur != recordCursorDisplayed)
        {
            const int prev = recordCursorDisplayed;
            recordCursorDisplayed = recCur;
            for (int i = 0; i < MAX_COLS; ++i)
                stepCols[static_cast<size_t>(i)]->isRecordCursor = (i == recCur);
            const int lo = juce::jmax(0, juce::jmin(prev, recCur));
            const int hi = juce::jmin(MAX_COLS - 1, juce::jmax(prev, recCur));
            for (int i = lo; i <= hi; ++i)
                stepCols[static_cast<size_t>(i)]->repaint();
        }
    }

    // Dim the BPM slider when an external MIDI clock overrides it
    bpmRow->setAlpha(processorRef.isMidiClockActive() ? 0.45f : 1.0f);

    // Step-grid highlight (step mode only; the grid is hidden in Phrase and
    // Gen). Above the idle early-return: a STOP that does not come from the
    // button (automation, a controller, a state load) can let the audio reach
    // deep idle before this tick, and the column it leaves lit has to go out
    // all the same. Stopped, this is two atomic reads, and showCurrentStep()
    // does nothing once the column is out.
    if (stepMode)
        showCurrentStep(playedStep());

    // Phrase Undo is offered only when there is something to undo. A preset
    // load or an engine switch can change that without a button, possibly
    // while stopped, so it is read here above the idle gate: one atomic load,
    // and setEnabled() does nothing (no repaint) while the answer stands.
    phrUndoBtn.setEnabled(processorRef.canPhraseUndo());

    // The transport button follows seq_running, above the idle early-return:
    // a STOP that is not the button (automation, a controller, a state load)
    // can let the audio reach deep idle before this tick, and a button left
    // reading STOP would start the transport when clicked to stop it. Only a
    // flip touches the button.
    if (seqRunning != transportShowsRunning)
    {
        transportShowsRunning = seqRunning;
        transportBtn.setButtonText(seqRunning ? "STOP" : "PLAY");
        transportBtn.setColour(juce::TextButton::buttonColourId,
                               seqRunning ? kSeqCol.darker(0.3f) : kSurface);
        transportBtn.setColour(juce::TextButton::textColourOffId,
                               seqRunning ? juce::Colours::white : juce::Colour(0xff4caf50));
    }

    // Skip expensive updates when audio is idle and neither sequencer runs
    bool seqIdle = processorRef.audioIdle.load(std::memory_order_relaxed)
                   && !seqRunning && !genRunning;
    if (seqIdle)
        return;

    if (genRunning && genMonitor) genMonitor->poll();

    // MIDI monitor
    int note = processorRef.lastMidiNote.load(std::memory_order_relaxed);
    bool on  = processorRef.lastMidiNoteOn.load(std::memory_order_relaxed);
    int vel  = processorRef.lastMidiVelocity.load(std::memory_order_relaxed);
    if (note >= 0)
    {
        auto txt = on ? (noteName(note) + " v" + juce::String(vel))
                      : (noteName(note) + " off");
        midiMonitor.setText(txt, juce::dontSendNotification);
        midiMonitor.setColour(juce::Label::textColourId, on ? kSuccess : kDim);
    }

    // LED reflects lastMidiNoteOn alone — kept outside the note-number guard so
    // a reset of lastMidiNote can't strand the LED in its previous colour.
    if (on != midiLedDisplayedOn)
    {
        midiLedDisplayedOn = on;
        if (!midiLedBounds.isEmpty())
            repaint(midiLedBounds.getSmallestIntegerContainer());
    }
}

// kSeqCol is now a global in GuiHelpers.h

void SequencerPanel::paint(juce::Graphics& g)
{
    g.fillAll(kCard);

    // MIDI activity LED (next to note display)
    if (!midiLedBounds.isEmpty())
    {
        bool noteOn = processorRef.lastMidiNoteOn.load(std::memory_order_relaxed);
        g.setColour(noteOn ? kSuccess : kDimmer);
        g.fillEllipse(midiLedBounds);
    }

    // Separator above step grid
    if (!gridArea.isEmpty())
    {
        g.setColour(kBorder);
        g.drawHorizontalLine(gridArea.getY() - 1,
                             static_cast<float>(gridArea.getX()),
                             static_cast<float>(gridArea.getRight()));
    }

    // Separator above Arp row
    int arpY = arpModeBtns[0].getY();
    if (arpY > 0)
    {
        g.setColour(kBorder);
        g.drawHorizontalLine(arpY - 2, 6.0f, static_cast<float>(getWidth() - 6));
    }

    // Unified switchbox frames (GuiHelpers paintSwitchBoxBorder)
    paintSwitchBoxBorder(g, modeSwitchBounds);
    paintSwitchBoxBorder(g, divisionSwitchBounds);
    paintSwitchBoxBorder(g, octShiftSwitchBounds);
    paintSwitchBoxBorder(g, genRangeSwitchBounds);
    if (arpModeBtns[0].isVisible())
    {
        paintSwitchBoxBorder(g, arpModeSwitchBounds);
        paintSwitchBoxBorder(g, arpOctSwitchBounds);
    }
    for (int i = 0; i < kNumExtraStrands; ++i)
        paintSwitchBoxBorder(g, strandOctSwitchBounds[i]);
    for (const auto* sw : { &phrPartSwitch, &phrHarmSwitch, &phrRateSwitch,
                            &phrExtSwitch, &phrLenSwitch, &phrBarsSwitch })
        paintSwitchBoxBorder(g, sw->frame);

    // Harmony box card (gen-mode left column) + the thin divider above the voices.
    // Both reset (empty / -1) in step mode, so they don't draw there. Lighter
    // kSurface fill so the card reads on the kCard panel; drawn before the child
    // controls, which paint on top (FX/synth module-card recipe).
    if (!harmonyBoxBounds.isEmpty())
    {
        g.setColour(kSurface.withAlpha(0.62f)); g.fillRect(harmonyBoxBounds);
        g.setColour(juce::Colour(0xaa05070d)); g.drawRect(harmonyBoxBounds.expanded(1, 1), 1);
        g.setColour(kBorder.withAlpha(0.82f)); g.drawRect(harmonyBoxBounds, 1);
    }
    if (voicesDividerY >= 0)
    {
        g.setColour(kBorder);
        g.drawHorizontalLine(voicesDividerY, 6.0f, static_cast<float>(getWidth() - 6));
    }

    // Voice group cards (gen mode) — subtle kCard fill + border per voice slot,
    // mirroring the per-instance sub-cards in the modulation panel ("Abschattung").
    // Drawn before the child controls, which paint on top.
    auto paintVoiceCard = [&g](const juce::Rectangle<int>& r)
    {
        if (r.isEmpty()) return;
        g.setColour(kCard.withAlpha(0.42f));   g.fillRect(r);
        g.setColour(kBorder.withAlpha(0.72f)); g.drawRect(r, 1);
    };
    for (const auto& r : voiceModuleBounds) paintVoiceCard(r);
}

void SequencerPanel::resized()
{
    auto area = getLocalBounds().reduced(6, 0);
    float topH = getTopLevelComponent()
                     ? static_cast<float>(getTopLevelComponent()->getHeight()) : 800.0f;
    int headerH = juce::jlimit(14, 20, juce::roundToInt(topH * 0.022f));
    // Match the sibling top-headers (DELAY/REVERB/T5 OSCILLATOR/AXES): plain
    // weight at headerH*0.85. SEQUENCER was the only top-header on bold
    // ModuleTitle, so it read noticeably fatter than the rest.
    seqHeader.setFont(juce::FontOptions(static_cast<float>(headerH) * 0.85f));
    seqHeader.setBounds(area.removeFromTop(headerH));
    area.removeFromTop(juce::jmax(3, headerH / 5));

    int rH = 22;
    int g = 3;
    const int panelW = getWidth();
    const bool compactTopRow = panelW < 760;

    // ═══ Row 1: ALWAYS the same (shared) — Play/Stop, Step|Phrase|Gen, BPM, Division,
    //            Gate, Shuffle, MIDI. Steps/Octave/Preset are mode-dependent and
    //            live in the STEP-mode row below. ═══
    auto r1 = area.removeFromTop(rH);

    const float midiFont = uiFontSize(TextRole::Value, compactTopRow ? 0.0f
                                                                      : static_cast<float>(rH) * 0.6f);
    const int midiTextW = measureTextWidth("D#4 v127", midiFont) + 8;
    const int midiLedW = compactTopRow ? 10 : 14;
    const int midiGap = compactTopRow ? 2 : 5;

    auto layoutMidiCluster = [this, compactTopRow, midiTextW, midiLedW, midiFont](juce::Rectangle<int> areaForMidi)
    {
        midiMonitor.setVisible(true);
        midiMonitor.setFont(juce::FontOptions(midiFont));
        midiLedBounds = {};
        if (areaForMidi.getWidth() >= midiLedW + 8)
        {
            auto textArea = areaForMidi.removeFromRight(juce::jmin(midiTextW, areaForMidi.getWidth()));
            midiMonitor.setBounds(textArea);
            if (areaForMidi.getWidth() > 0)
            {
                auto ledArea = areaForMidi.removeFromRight(juce::jmin(midiLedW, areaForMidi.getWidth()));
                const float dotSize = compactTopRow ? 7.0f : 8.0f;
                midiLedBounds = juce::Rectangle<float>(dotSize, dotSize)
                    .withCentre({ static_cast<float>(ledArea.getCentreX()),
                                  static_cast<float>(textArea.getCentreY()) });
            }
        }
        else
        {
            midiMonitor.setBounds(areaForMidi);
        }
    };

    enum HeaderSlot
    {
        slotTransport,
        slotMode,
        slotBpm,
        slotDivision,
        slotGate,
        slotShuffle,
        slotMidi
    };

    const int transportW = 36;
    // Step | Phrase | Gen: each segment as wide as its label in the switch
    // font plus the LookAndFeel's text indents (at most 0.6 x the font height
    // per side), so no label is squeezed below the font size and "Phrase"
    // keeps the 11 px minimum (INTERFACE_DESIGN_POLICY). The Step segment
    // also fits "REC", its label while step recording is armed.
    auto modeSegW = [&](juce::TextButton& b, std::initializer_list<const char*> labels)
    {
        const float fs = b.getLookAndFeel().getTextButtonFont(b, rH).getHeight();
        int textW = 0;
        for (const char* label : labels)
            textW = juce::jmax(textW, measureTextWidth(label, fs));
        return textW + 2 * juce::roundToInt(fs * 0.6f) + 2;
    };
    const int modeStepW   = modeSegW(modeStepBtn,   { "Step", "REC" });
    const int modePhraseW = modeSegW(modePhraseBtn, { "Phrase" });
    const int modeGenW    = modeSegW(modeGenBtn,    { "Gen" });
    const int modeW = modeStepW + modePhraseW + modeGenW;
    const int divisionPrefW = kNumDivBtns * (compactTopRow ? 18 : 22);  // note-glyph switchbox
    const int divisionMinW  = kNumDivBtns * (compactTopRow ? 15 : 18);
    const int midiClusterW = midiTextW + midiLedW + midiGap;
    // Inline Gate/Shuffle draw "label  value" INSIDE the bar, but getMinimumWidth()
    // counts only the track (inline zeroes the label/value cells). Give an explicit
    // preferred width that fits the text or the strip starves them to the track
    // minimum and label+value overprint. Min stays small so they can still shed.
    const float hdrFs = 12.0f;
    const int gateMinW     = gateRow->getMinimumWidth();
    const int gatePrefW    = measureTextWidth("Gate", hdrFs)    + measureTextWidth("100%", hdrFs) + 20;
    const int shuffleMinW  = shuffleRow->getMinimumWidth();
    const int shufflePrefW = measureTextWidth("Shuffle", hdrFs) + measureTextWidth("100%", hdrFs) + 20;

    // Overflow drop order (highest tier sheds first; the note divisions, tier 1,
    // survive longest). Shuffle is the most expendable; BPM/Gate/MIDI, transport
    // and the Step|Phrase|Gen switch never drop.
    std::vector<ResponsiveStripItem> items {
        { transportW, transportW, 0, false, ResponsiveStripFallback::none },          // Play/Stop
        { modeW, modeW, 0, false, ResponsiveStripFallback::none },                    // Step|Phrase|Gen
        { bpmRow->getPreferredWidth(), bpmRow->getMinimumWidth(), 0, true, ResponsiveStripFallback::none }, // BPM
        { divisionPrefW, divisionMinW, 1, false, ResponsiveStripFallback::overflow }, // Division (kept longest)
        { gatePrefW, gateMinW, 0, false, ResponsiveStripFallback::none },             // Gate
        { shufflePrefW, shuffleMinW, 5, false, ResponsiveStripFallback::overflow },   // Shuffle (sheds first)
        { midiClusterW, midiClusterW, 0, false, ResponsiveStripFallback::none }       // MIDI
    };

    auto headerLayout = layoutResponsiveStrip(r1, items, g, compactTopRow ? 24 : 28);
    const auto hasBounds = [](juce::Rectangle<int> bounds) { return !bounds.isEmpty(); };

    transportBtn.setBounds(headerLayout.bounds[slotTransport]);

    // Step | Phrase | Gen switchbox — three connected, label-sized segments.
    {
        auto modeArea = headerLayout.bounds[slotMode];
        modeStepBtn  .setBounds(modeArea.removeFromLeft(modeStepW));
        modePhraseBtn.setBounds(modeArea.removeFromLeft(modePhraseW));
        modeGenBtn   .setBounds(modeArea);
        modeSwitchBounds = modeStepBtn.getBounds().getUnion(modeGenBtn.getBounds());
    }

    bpmRow->setBounds(headerLayout.bounds[slotBpm]);
    gateRow->setBounds(headerLayout.bounds[slotGate]);
    shuffleRow->setVisible(hasBounds(headerLayout.bounds[slotShuffle]));
    shuffleRow->setBounds(headerLayout.bounds[slotShuffle]);
    layoutMidiCluster(headerLayout.bounds[slotMidi]);

    headerOverflowBtn.setVisible(headerLayout.overflowUsed);
    headerOverflowBtn.setBounds(headerLayout.overflowUsed ? headerLayout.overflowBounds : juce::Rectangle<int>{});

    const bool divVisible = hasBounds(headerLayout.bounds[slotDivision]);
    for (int i = 0; i < kNumDivBtns; ++i)
    {
        divBtns[i].setVisible(divVisible);
        if (!divVisible)
            divBtns[i].setBounds({});
    }
    if (divVisible)
    {
        auto divArea = headerLayout.bounds[slotDivision];
        const int divBtnW = divArea.getWidth() / kNumDivBtns;
        for (int i = 0; i < kNumDivBtns; ++i)
        {
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumDivBtns - 1) edges |= juce::Button::ConnectedOnRight;
            divBtns[i].setConnectedEdges(edges);
            divBtns[i].setBounds(divArea.removeFromLeft(i == kNumDivBtns - 1 ? divArea.getWidth() : divBtnW));
        }
        divisionSwitchBounds = divBtns[0].getBounds().getUnion(divBtns[kNumDivBtns - 1].getBounds());
    }
    else
        divisionSwitchBounds = {};

    // Octave + preset management are laid out in the STEP-mode row (see the step
    // branch below), not the shared header. Reset the frame here; step sets it.
    octShiftSwitchBounds = {};

    area.removeFromTop(compactTopRow ? 5 : g);

    // ═══ Row 4 (bottom): Arp controls ═══
    auto r4 = area.removeFromBottom(rH);
    const float arpLabelBase = static_cast<float>(rH) * 0.6f;
    const float arpLabelFs = uiFontSize(TextRole::ModuleTitle, arpLabelBase);
    arpModeLabel.setFont(uiFont(TextRole::ModuleTitle, arpLabelBase, true));
    arpModeLabel.setBounds(r4.removeFromLeft(measureTextWidth(" ARP", arpLabelFs) + 6));
    r4.removeFromLeft(2);
    const int modeBtnW = 28;   // square-ish glyph cells (off/up/down/updown/random)
    for (int i = 0; i < kNumModeBtns; ++i)
    {
        int edges = 0;
        if (i > 0) edges |= juce::Button::ConnectedOnLeft;
        if (i < kNumModeBtns - 1) edges |= juce::Button::ConnectedOnRight;
        arpModeBtns[i].setConnectedEdges(edges);
        arpModeBtns[i].setBounds(r4.removeFromLeft(modeBtnW));
    }
    arpModeSwitchBounds = arpModeBtns[0].getBounds().getUnion(arpModeBtns[kNumModeBtns - 1].getBounds());
    r4.removeFromLeft(g);
    arpRateBox.setBounds(r4.removeFromLeft(60));   r4.removeFromLeft(g);
    arpOctLabel.setFont(uiFont(TextRole::ModuleTitle, arpLabelBase, true));
    arpOctLabel.setBounds(r4.removeFromLeft(measureTextWidth(" OCT", arpLabelFs) + 6));   r4.removeFromLeft(2);
    int arpOctBtnW = 22;
    for (int i = 0; i < kNumOctBtns; ++i)
    {
        int edges = 0;
        if (i > 0) edges |= juce::Button::ConnectedOnLeft;
        if (i < kNumOctBtns - 1) edges |= juce::Button::ConnectedOnRight;
        arpOctBtns[i].setConnectedEdges(edges);
        arpOctBtns[i].setBounds(r4.removeFromLeft(arpOctBtnW));
    }
    arpOctSwitchBounds = arpOctBtns[0].getBounds().getUnion(arpOctBtns[kNumOctBtns - 1].getBounds());
    area.removeFromBottom(g);

    // ═══ Determine mode ═══
    const int previousLayoutEngine = layoutEngine;
    layoutEngine = static_cast<int>(processorRef.getValueTreeState()
        .getRawParameterValue(PID::seqEngine)->load());
    const bool genModeActive    = layoutEngine == SeqEngine::Gen;
    const bool stepModeActive   = layoutEngine == SeqEngine::Step;
    const bool phraseModeActive = layoutEngine == SeqEngine::Phrase;

    // Visibility: step grid vs gen controls (Row 1 stays ALWAYS visible)
    for (int i = 0; i < MAX_COLS; ++i)
        stepCols[static_cast<size_t>(i)]->setVisible(stepModeActive && i < numVisibleSteps);

    // Laying out Step re-reads the playhead highlight. The timer keeps it
    // current only while Step is shown, so a column lit before another mode
    // was shown would come back lit, until the next tick, after Step playback
    // stopped meanwhile. Here and not in the timer: whichever layout first
    // sees the switch (the timer's, a window resize, a step-count sync) takes
    // the edge the timer would act on.
    if (stepModeActive && previousLayoutEngine != SeqEngine::Step)
        showCurrentStep(playedStep());

    genStepsRow->setVisible(genModeActive);
    genPulsesRow->setVisible(genModeActive);
    genRotationRow->setVisible(genModeActive);
    genMutationRow->setVisible(genModeActive);
    for (auto& b : genRangeBtns) b.setVisible(genModeActive);
    genRangeLabel.setVisible(genModeActive);
    genCycRow->setVisible(genModeActive);
    genScaleRootBox.setVisible(genModeActive || phraseModeActive);   // Phrase's Key
    genScaleTypeBox.setVisible(genModeActive || phraseModeActive);
    genFixStepsBtn.setVisible(genModeActive);
    genFixPulsesBtn.setVisible(genModeActive);
    genFixRotationBtn.setVisible(genModeActive);
    genFixMutationBtn.setVisible(genModeActive);
    genFieldModeBox.setVisible(genModeActive);
    genCoordModeBox.setVisible(genModeActive);
    for (int i = 0; i < kNumExtraStrands; ++i)
    {
        strandEnableBtns[i].setVisible(genModeActive);
        strandRoleBoxes[i].setVisible(genModeActive);
        strandDivBoxes[i].setVisible(genModeActive);
        // strandOctaveSliders are intentionally hidden — only their bound
        // toggle buttons (strandOctBtns) are shown.
        for (int b = 0; b < kStrandOctBtns; ++b)
            strandOctBtns[i][b].setVisible(genModeActive);
        strandDomRows[i]->setVisible(genModeActive);
    }

    // Step-mode-only controls (step row 2): unified Steps slider, preset
    // management. Hidden in gen mode, which has its own Steps (with FIX), and
    // in Phrase mode. The octave switch is Step's and Phrase's (Phrase's Oct).
    seqStepsRow->setVisible(stepModeActive);
    presetBox.setVisible(stepModeActive);
    seqSaveBtn.setVisible(stepModeActive);
    seqLoadBtn.setVisible(stepModeActive);
    for (int i = 0; i < kNumOctShiftBtns; ++i)
        octShiftBtns[i].setVisible(stepModeActive || phraseModeActive);

    for (auto* c : phraseControls())
        c->setVisible(phraseModeActive);
    for (auto* sw : { &phrPartSwitch, &phrHarmSwitch, &phrRateSwitch,
                      &phrExtSwitch, &phrLenSwitch, &phrBarsSwitch })
        sw->frame = {};   // set below only when Phrase is laid out

    // Reset gen-mode frames/divider; set below only when laid out (gen mode on),
    // so the empty-guards in paint() drop them when the step grid is showing.
    harmonyBoxBounds = {};
    voicesDividerY = -1;
    v1ModuleBounds = {};
    if (genMonitor) genMonitor->setVisible (false);
    for (auto& r : voiceModuleBounds) r = {};
    for (int i = 0; i < kNumExtraStrands; ++i)
        strandOctSwitchBounds[i] = {};
    genRangeSwitchBounds = {};

    // The octave switchbox [-2 .. +2]: Step's row 2 and Phrase's row P1.
    const int octW = kNumOctShiftBtns * (compactTopRow ? 20 : 24);
    auto layoutOctShift = [this](juce::Rectangle<int> octArea)
    {
        const int octBtnW = octArea.getWidth() / kNumOctShiftBtns;
        for (int i = 0; i < kNumOctShiftBtns; ++i)
        {
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumOctShiftBtns - 1) edges |= juce::Button::ConnectedOnRight;
            octShiftBtns[i].setConnectedEdges(edges);
            octShiftBtns[i].setBounds(octArea.removeFromLeft(
                i == kNumOctShiftBtns - 1 ? octArea.getWidth() : octBtnW));
        }
        octShiftSwitchBounds = octShiftBtns[0].getBounds()
            .getUnion(octShiftBtns[kNumOctShiftBtns - 1].getBounds());
    };

    if (genModeActive)
    {
        // ═══ Gen mode: 2-column layout ═══
        //   Row A:  Steps[FIX]  |  Pulses[FIX]
        //   Left column : harmony box (Note+Skala, Range, Cyc, Drift) in a card
        //   Right column: Rotation[FIX]  Evolve[FIX]  (one row)
        //   Below both, full width: a thin divider then the voice modules.
        const int genCtrlH = rH;
        const int colGap   = 8;
        const int fixW     = 28;
        const int fixGap   = 2;
        const int rowGap   = 4;
        const int intraGap = 2;
        const int colW     = (area.getWidth() - colGap) / 2;

        // Place an inline slider + its FIX button into a row rect.
        auto placeFixRow = [fixW, fixGap](juce::Rectangle<int> r, SliderRow& row,
                                          juce::TextButton& fix)
        {
            fix.setBounds(r.removeFromRight(fixW));
            r.removeFromRight(fixGap);
            row.setBounds(r);
        };

        // ── Row A: Steps[FIX] | Pulses[FIX] ──
        {
            auto rowA = area.removeFromTop(genCtrlH);
            placeFixRow(rowA.removeFromLeft(colW), *genStepsRow, genFixStepsBtn);
            rowA.removeFromLeft(colGap);
            placeFixRow(rowA, *genPulsesRow, genFixPulsesBtn);
        }
        area.removeFromTop(rowGap);

        // ── Split the region below Row A into the harmony columns (top) and the
        //    full-width voices (bottom), divider between. The HARMONY BOX is ONE
        //    row: Note · Skala · Range · Cyc · Drift. Voices take the remainder
        //    (Phase 5 redesigns them). ──
        const int dividerGap  = 6;
        const int boxPad      = 3;
        const int regionH     = juce::jmax(0, area.getHeight());
        const int rowH        = juce::jlimit(14, genCtrlH, regionH - dividerGap - 2 * boxPad);
        const int boxH        = rowH + 2 * boxPad;

        auto colsArea  = area.removeFromTop(juce::jmin(boxH, regionH));
        voicesDividerY = colsArea.getBottom() + dividerGap / 2;
        area.removeFromTop(juce::jmin(dividerGap, area.getHeight()));
        auto voicesArea = area;                          // full-width remainder

        auto leftCol  = colsArea.removeFromLeft(colW);
        colsArea.removeFromLeft(colGap);
        auto rightCol = colsArea;

        // LEFT column — harmony box (framed card; the frame is drawn in paint()).
        // Everything shares ONE row: Note · Skala · Range · Drift · Cyc · Coord.
        // The dropdowns get fixed text-sized widths (Note narrow for "C#",
        // Skala wider for "Chromatic", Coord for "Density Budget"); Cyc takes
        // the remainder.
        {
            harmonyBoxBounds = leftCol;
            auto row = harmonyBoxBounds.reduced(boxPad);
            const int g = 3;
            genScaleRootBox.setBounds(row.removeFromLeft(juce::jmin(44, row.getWidth())));   row.removeFromLeft(g);
            genScaleTypeBox.setBounds(row.removeFromLeft(juce::jmin(88, row.getWidth())));   row.removeFromLeft(g);
            // "Rng" label + [1][2][3][4] switchbox — compact fixed width (the four
            // discrete values needed only a fraction of the old slider). Then Drift,
            // then Cyc: pick the field-evolution MODE before its RATE. The label's
            // width is taken from Cyc (the remainder).
            genRangeLabel.setBounds(row.removeFromLeft(juce::jmin(26, row.getWidth())));
            {
                auto rangeArea = row.removeFromLeft(juce::jmin(56, row.getWidth()));   row.removeFromLeft(g);
                const int rbW = rangeArea.getWidth() / kNumRangeBtns;
                for (int i = 0; i < kNumRangeBtns; ++i)
                {
                    int edges = 0;
                    if (i > 0)                 edges |= juce::Button::ConnectedOnLeft;
                    if (i < kNumRangeBtns - 1) edges |= juce::Button::ConnectedOnRight;
                    genRangeBtns[i].setConnectedEdges(edges);
                    genRangeBtns[i].setBounds(rangeArea.removeFromLeft(
                        i == kNumRangeBtns - 1 ? rangeArea.getWidth() : rbW));
                }
                genRangeSwitchBounds = genRangeBtns[0].getBounds()
                    .getUnion(genRangeBtns[kNumRangeBtns - 1].getBounds());
            }
            genFieldModeBox.setBounds(row.removeFromLeft(juce::jmin(72, row.getWidth())));   row.removeFromLeft(g);
            // Coordination mode at the row's right end (global ensemble
            // switch; per BJ 2026-07-16 it lives in the harmony row). Fixed
            // width sized for "Density Budget", capped at half the remainder
            // so Cyc always keeps a usable slider.
            genCoordModeBox.setBounds(row.removeFromRight(juce::jmin(96, row.getWidth() / 2)));
            row.removeFromRight(g);
            genCycRow->setBounds(row);
        }

        // RIGHT column — Rotation[FIX] + Evolve[FIX] share one row.
        {
            auto rRow = rightCol.removeFromTop(genCtrlH);
            const int half = (rRow.getWidth() - colGap) / 2;
            placeFixRow(rRow.removeFromLeft(half), *genRotationRow, genFixRotationBtn);
            rRow.removeFromLeft(colGap);
            placeFixRow(rRow, *genMutationRow, genFixMutationBtn);
        }

        // ── Strand modules (full width, below the divider): S1 (dummy) + S2..S5,
        //    five equal slots. Each real module is three rows:
        //      Row 1: [Sx enable] [Role ~50%] [Tempo/Div rest]
        //      Row 2: Dominance inline slider, full module width
        //      Row 3: octave switchbox [-2..+2], full module width
        //    S1 is a display-only placeholder drawn in paint() (no params). ──
        {
            const int nVoices   = kNumExtraStrands + 1;     // V1 dummy + V2..V5
            const int moduleGap = 12;
            const int moduleW   = (voicesArea.getWidth() - (nVoices - 1) * moduleGap) / nVoices;
            const int cardPad   = 3;   // inset controls within each group card

            // Slot 0 — V1 dummy (card + "V1" + greyed placeholders, painted later).
            v1ModuleBounds = voicesArea.removeFromLeft(moduleW);
            if (genMonitor) { genMonitor->setBounds (v1ModuleBounds); genMonitor->setVisible (true); }
            voicesArea.removeFromLeft(moduleGap);

            for (int i = 0; i < kNumExtraStrands; ++i)
            {
                voiceModuleBounds[i] = voicesArea.removeFromLeft(moduleW);
                if (i < kNumExtraStrands - 1) voicesArea.removeFromLeft(moduleGap);

                // Controls inset within the group card so its fill/border show
                // (mirrors the mod-panel sub-cards). Row height derives from the
                // inset card so three rows always fit.
                auto module = voiceModuleBounds[i].reduced(cardPad);
                const int vRowH = juce::jmax(12, (module.getHeight() - 2 * intraGap) / 3);

                const int vEnW = 30;
                const int gap  = 3;

                // Row 1: [Vx enable] [Role 50% of module] [Tempo/Div rest]
                auto r1 = module.removeFromTop(vRowH);
                strandEnableBtns[i].setBounds(r1.removeFromLeft(vEnW));
                r1.removeFromLeft(gap);
                // Role box trimmed to 75% of its old half-module width so the
                // tempo-multiplier box beside it fits the full "1/16x" label.
                const int roleW = juce::jmax(0, juce::jmin(moduleW * 3 / 8, r1.getWidth()));
                strandRoleBoxes[i].setBounds(r1.removeFromLeft(roleW));
                r1.removeFromLeft(gap);
                strandDivBoxes[i].setBounds(r1);

                module.removeFromTop(intraGap);

                // Row 2: Dom inline slider, full module width.
                strandDomRows[i]->setBounds(module.removeFromTop(vRowH));

                module.removeFromTop(intraGap);

                // Row 3: octave switchbox [-2][-1][0][+1][+2], full module width.
                {
                    auto octRow = module.removeFromTop(vRowH);
                    const int octBtnW = juce::jmax(1, octRow.getWidth() / kStrandOctBtns);
                    for (int b = 0; b < kStrandOctBtns; ++b)
                    {
                        int edges = 0;
                        if (b > 0) edges |= juce::Button::ConnectedOnLeft;
                        if (b < kStrandOctBtns - 1) edges |= juce::Button::ConnectedOnRight;
                        strandOctBtns[i][b].setConnectedEdges(edges);
                        const int w = (b == kStrandOctBtns - 1) ? octRow.getWidth() : octBtnW;
                        strandOctBtns[i][b].setBounds(octRow.removeFromLeft(w));
                    }
                    strandOctSwitchBounds[i] = strandOctBtns[i][0].getBounds()
                        .getUnion(strandOctBtns[i][kStrandOctBtns - 1].getBounds());
                }
            }
        }

        gridArea = {};
    }
    else if (stepModeActive)
    {
        // ═══ Step mode ═══

        // Row 2: unified Steps slider (no FIX) · Octave switch · preset mgmt.
        // Steps is sized to EXACTLY the gen-mode Steps bar width (left column minus
        // the FIX cell), so the bar stays put when toggling Step<->Gen. Octave +
        // preset then occupy the right column, where Pulses sits in gen mode.
        auto stepRow = area.removeFromTop(rH);
        const int presetW = compactTopRow ? 76 : 96;
        const int iconW   = rH;
        const int sColGap = 8, sFixW = 28, sFixGap = 2;          // mirror the gen branch
        const int sColW   = (stepRow.getWidth() - sColGap) / 2;
        const int stepsW  = juce::jmax(40, sColW - sFixW - sFixGap);  // == genStepsRow width

        // Steps bar on the left, exactly the gen width.
        seqStepsRow->setBounds(stepRow.removeFromLeft(stepsW));

        // From the right: [Preset▾][S][L]
        seqLoadBtn.setBounds(stepRow.removeFromRight(iconW));
        seqSaveBtn.setBounds(stepRow.removeFromRight(iconW));
        stepRow.removeFromRight(2);
        presetBox.setBounds(stepRow.removeFromRight(presetW));
        stepRow.removeFromRight(g);

        // Octave switchbox (relocated from the shared header).
        layoutOctShift(stepRow.removeFromRight(octW));

        area.removeFromTop(compactTopRow ? 4 : g);

        // Step grid (remaining space, slightly reduced by the row above).
        gridArea = area;
        if (numVisibleSteps > 0 && gridArea.getWidth() > numVisibleSteps)
        {
            int stepW = gridArea.getWidth() / numVisibleSteps;
            for (int i = 0; i < MAX_COLS; ++i)
            {
                if (i < numVisibleSteps)
                    stepCols[static_cast<size_t>(i)]->setBounds(
                        gridArea.getX() + i * stepW, gridArea.getY(),
                        stepW, gridArea.getHeight());
            }
        }
    }
    else
    {
        // ═══ Phrase mode ═══ (docs/PHRASE_SEQ_CONCEPT.md §4)
        //   P1: Part · Key · Oct · Layer · Slide
        //   P2: Harm · four chord slots · Rate · Ext
        //   P3: Len · Pulses · Rot · Bars · Form · Var · Evolve · actions
        // Switches, combos and buttons get their content's width; the inline
        // sliders share what is left in their row, in proportion to the
        // width their label and widest value need. The space below P3 is the
        // phrase view's.
        const int groupGap = 8;
        // The width an inline bar needs for its label and widest value at
        // rH: SliderRow::paintInlineBar's font, its 4 px inset a side, the
        // value's 2 px slack and the 4 px between value and label.
        const float barFs = juce::jlimit(9.0f, 13.0f, static_cast<float>(rH) * 0.58f);
        auto barPrefW = [barFs](const char* label, const char* widestValue)
        {
            return measureTextWidth(label, barFs) + measureTextWidth(widestValue, barFs) + 14;
        };
        auto placeTag = [](juce::Label& tag, juce::Rectangle<int>& row)
        {
            tag.setBounds(row.removeFromLeft(juce::jmin(tagWidth(tag), row.getWidth())));
        };
        auto placeBars = [g](juce::Rectangle<int> row,
                             std::initializer_list<std::pair<SliderRow*, int>> bars)
        {
            int prefSum = 0;
            for (const auto& b : bars) prefSum += b.second;
            const int n = static_cast<int>(bars.size());
            const int avail = juce::jmax(0, row.getWidth() - (n - 1) * g);
            int k = 0;
            for (const auto& b : bars)
            {
                const int w = (k == n - 1) ? row.getWidth()
                                           : avail * b.second / juce::jmax(1, prefSum);
                b.first->setBounds(row.removeFromLeft(juce::jmin(w, row.getWidth())));
                if (k < n - 1) row.removeFromLeft(juce::jmin(g, row.getWidth()));
                ++k;
            }
        };

        // ── P1: what, in which key and octave ──
        {
            auto row = area.removeFromTop(rH);
            layoutPhraseSwitch(phrPartSwitch, row, rH);
            row.removeFromLeft(groupGap);
            placeTag(phrKeyTag, row);
            genScaleRootBox.setBounds(row.removeFromLeft(juce::jmin(44, row.getWidth())));   row.removeFromLeft(g);
            genScaleTypeBox.setBounds(row.removeFromLeft(juce::jmin(88, row.getWidth())));
            row.removeFromLeft(groupGap);
            placeTag(phrOctTag, row);
            layoutOctShift(row.removeFromLeft(juce::jmin(octW, row.getWidth())));
            row.removeFromLeft(groupGap);
            placeTag(phrLayerTag, row);
            phrLayerBox.setBounds(row.removeFromLeft(juce::jmin(76, row.getWidth())));
            row.removeFromLeft(groupGap);
            phrSlideRow->setBounds(row);
        }
        area.removeFromTop(g);

        // ── P2: over which harmony ──
        {
            auto row = area.removeFromTop(rH);
            layoutPhraseSwitch(phrHarmSwitch, row, rH);
            row.removeFromLeft(groupGap);
            // Rate and Ext from the right; the chord slots share the middle.
            auto right = row.removeFromRight(juce::jmin(row.getWidth(),
                phraseSwitchWidth(phrRateSwitch, rH) + groupGap + phraseSwitchWidth(phrExtSwitch, rH)));
            layoutPhraseSwitch(phrRateSwitch, right, rH);
            right.removeFromLeft(groupGap);
            layoutPhraseSwitch(phrExtSwitch, right, rH);
            row.removeFromRight(groupGap);
            const int slotW = juce::jmin(100, (row.getWidth() - (kNumChordSlots - 1) * g) / kNumChordSlots);
            for (auto& box : phrChordBoxes)
            {
                box.setBounds(row.removeFromLeft(juce::jmax(0, juce::jmin(slotW, row.getWidth()))));
                row.removeFromLeft(juce::jmin(g, row.getWidth()));
            }
        }
        area.removeFromTop(g);

        // ── P3: which rhythm, how it unfolds; the motif actions at the end ──
        {
            auto row = area.removeFromTop(rH);
            // Actions from the right: [Dice][Keep][Undo] [From Step].
            for (auto* b : { &phrFromStepBtn, &phrUndoBtn, &phrKeepBtn, &phrDiceBtn })
            {
                b->setBounds(row.removeFromRight(juce::jmin(labelButtonWidth(*b, rH), row.getWidth())));
                row.removeFromRight(juce::jmin(b == &phrFromStepBtn ? groupGap : 2, row.getWidth()));
            }
            row.removeFromRight(juce::jmin(groupGap - 2, row.getWidth()));

            layoutPhraseSwitch(phrLenSwitch, row, rH);
            row.removeFromLeft(groupGap);
            // Fixed parts of the row after Len: the Bars switch and Form.
            const int barsW = phraseSwitchWidth(phrBarsSwitch, rH);
            const int formW = tagWidth(phrFormTag) + 64;
            const int fixedW = barsW + formW + 3 * groupGap;
            // Pulses · Rot | Bars · Form | Var · Evolve — the bars around the
            // fixed groups share what is left.
            const int barsSpace = juce::jmax(0, row.getWidth() - fixedW);
            const int pPulses = barPrefW("Pulses", "32"), pRot = barPrefW("Rot", "31");
            const int pVar = barPrefW("Var", "100%"),     pEvo = barPrefW("Evolve", "100%");
            const int leftW = barsSpace * (pPulses + pRot) / juce::jmax(1, pPulses + pRot + pVar + pEvo);
            placeBars(row.removeFromLeft(juce::jmin(leftW, row.getWidth())),
                      { { phrPulsesRow.get(), pPulses }, { phrRotationRow.get(), pRot } });
            row.removeFromLeft(juce::jmin(groupGap, row.getWidth()));
            layoutPhraseSwitch(phrBarsSwitch, row, rH);
            row.removeFromLeft(juce::jmin(groupGap, row.getWidth()));
            placeTag(phrFormTag, row);
            phrFormBox.setBounds(row.removeFromLeft(juce::jmin(64, row.getWidth())));
            row.removeFromLeft(juce::jmin(groupGap, row.getWidth()));
            placeBars(row, { { phrVariationRow.get(), pVar }, { phrEvolveRow.get(), pEvo } });
        }

        gridArea = {};
    }
}
