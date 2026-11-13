#include "ReplayOverlay.h"
#include "../PluginProcessor.h"
#include "GuiHelpers.h"

namespace
{
    constexpr int kMargin       = 28;
    constexpr int kHeaderY      = 26;
    constexpr int kTransportY   = 74;
    constexpr int kTimelineY    = 112;
    constexpr int kTimelineH    = 64;
    constexpr int kNowY         = kTimelineY + kTimelineH + 26;
    constexpr int kFeedY        = kNowY + 34;
    constexpr int kFeedLineH    = 20;

    juce::String sourceLabel(NoteEventLogEntry::Source s)
    {
        switch (s)
        {
            case NoteEventLogEntry::Source::StepSequencer:        return "StepSeq";
            case NoteEventLogEntry::Source::GenerativeSequencer:  return "GenSeq";
            case NoteEventLogEntry::Source::PhraseSequencer:      return "PhraseSeq";
            case NoteEventLogEntry::Source::Arpeggiator:          return "Arp";
            case NoteEventLogEntry::Source::ExternalMidi:         return "MIDI";
        }
        return "?";
    }
}

ReplayOverlay::ReplayOverlay(T5ynthProcessor& p) : processorRef(p)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);   // swallow key presses (computer-keyboard notes are
                                   // also gated in the processor; this catches shortcuts)

    stopBtn.setColour(juce::TextButton::buttonColourId, kSurface);
    stopBtn.setColour(juce::TextButton::textColourOffId, kAccent);
    stopBtn.onClick = [this] { if (onStop) onStop(); };
    addAndMakeVisible(stopBtn);

    // The tape has no bar grid (wall-clock samples), so tempo is honestly a
    // time-scale. ×1 centred; live-changeable, resets to ×1 per tape.
    speedSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    speedSlider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    speedSlider.setRange(0.25, 4.0, 0.0);
    speedSlider.setSkewFactorFromMidPoint(1.0);
    speedSlider.setColour(juce::Slider::trackColourId, kSurface);
    speedSlider.setColour(juce::Slider::thumbColourId, kAccent);
    speedSlider.setTooltip("Replay speed (time-scale; the log has no bar grid)");
    speedSlider.onValueChange = [this]
    {
        processorRef.setReplayRate(static_cast<float>(speedSlider.getValue()));
        repaint();   // the ×N.NN readout next to the slider
    };
    addAndMakeVisible(speedSlider);
}

void ReplayOverlay::begin(const juce::String& tapeName)
{
    tapeName_ = tapeName;

    speedSlider.setValue(1.0, juce::dontSendNotification);

    timeline_ = {};   // rebuilt lazily in paint() at the current width
    posText_.clear();
    nowPromptA_.clear();
    nowPromptB_.clear();
    nowMetaText_.clear();
    genBusy_     = false;
    playheadPx_  = -1;
    feedNoteIdx_ = 0;
    feedParamIdx_ = 0;
    nowGenIdx_   = static_cast<size_t>(-1);
    feed_.clear();

    setVisible(true);
    toFront(false);
    grabKeyboardFocus();
    refresh();
}

void ReplayOverlay::end()
{
    setVisible(false);
    timeline_ = {};
    feed_.clear();
}

void ReplayOverlay::resized()
{
    const auto b = getLocalBounds();
    stopBtn.setBounds(b.getRight() - kMargin - 110, kHeaderY - 4, 110, 30);
    speedSlider.setBounds(b.getRight() - kMargin - 220, kTransportY - 6, 220, 24);
    if (timelineBounds_.getWidth() != b.getWidth() - 2 * kMargin)
        timeline_ = {};   // width changed → rebuild in next paint
}

juce::String ReplayOverlay::formatTapeTime(uint64_t samples) const
{
    const double sr = processorRef.getSampleRate() > 0.0 ? processorRef.getSampleRate() : 44100.0;
    const int total = juce::jmax(0, juce::roundToInt(static_cast<double>(samples) / sr));
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}

void ReplayOverlay::rebuildTimelineImage(int w, int h)
{
    timeline_ = juce::Image(juce::Image::ARGB, juce::jmax(1, w), juce::jmax(1, h), true);
    juce::Graphics g(timeline_);
    g.fillAll(kSurface.darker(0.25f));

    const auto total = processorRef.getReplayDurationSamples();
    if (total == 0)
        return;

    const auto xOf = [&](uint64_t t)
    { return static_cast<int>(static_cast<double>(t) / static_cast<double>(total) * (w - 1)); };

    // Note scatter: x = time, y = pitch (down = low). NoteOns only — density view.
    g.setColour(kDim.withAlpha(0.65f));
    for (const auto& n : processorRef.getReplayNoteEventsForUi())
    {
        if (n.type != VoiceEvent::Type::NoteOn)
            continue;
        const float yNorm = 1.0f - juce::jlimit(0.0f, 1.0f, (static_cast<float>(n.note) - 24.0f) / 84.0f);
        g.fillRect(xOf(n.timestamp), 4 + static_cast<int>(yNorm * (h - 12)), 2, 4);
    }

    // Param edits: faint tick row at the bottom.
    g.setColour(kDimmer.withAlpha(0.5f));
    for (const auto& p : processorRef.getReplayParamEventsForUi())
        g.fillRect(xOf(p.timestamp), h - 4, 1, 3);

    // Generations: full-height accent marks — the tape's timbre changes.
    g.setColour(kAccent.withAlpha(0.85f));
    for (const auto& gen : processorRef.getReplayGenerationEventsForUi())
        g.fillRect(xOf(gen.timestamp), 0, 2, h);
}

void ReplayOverlay::refresh()
{
    if (! isVisible() || ! processorRef.isReplayActive())
        return;

    const uint64_t playhead = processorRef.getReplayPlayhead();
    const uint64_t total    = processorRef.getReplayDurationSamples();
    bool dirty = false;

    // Position readout (changes once per second)
    auto pos = formatTapeTime(playhead) + " / " + formatTapeTime(total);
    if (pos != posText_) { posText_ = std::move(pos); dirty = true; }

    // Playhead pixel over the timeline strip
    if (total > 0 && timelineBounds_.getWidth() > 0)
    {
        const int px = static_cast<int>(
            juce::jlimit(0.0, 1.0, static_cast<double>(playhead) / static_cast<double>(total))
            * (timelineBounds_.getWidth() - 1));
        if (px != playheadPx_) { playheadPx_ = px; dirty = true; }
    }

    // Current generation = last one at/behind the playhead → prompts + seed
    const auto& gens = processorRef.getReplayGenerationEventsForUi();
    // Sentinel walk: size_t(-1)+1 wraps to 0, so the first iteration tests gens[0].
    size_t genIdx = nowGenIdx_;
    while (genIdx + 1 < gens.size() && gens[genIdx + 1].timestamp <= playhead)
        ++genIdx;
    if (genIdx != nowGenIdx_ && genIdx != static_cast<size_t>(-1) && genIdx < gens.size())
    {
        nowGenIdx_  = genIdx;
        const auto& ge = gens[genIdx];
        nowPromptA_ = ge.promptA;
        nowPromptB_ = ge.promptB;
        nowMetaText_ = "seed " + juce::String(ge.realizedSeed)
                     + "   gen " + juce::String(static_cast<int>(genIdx) + 1)
                     + "/" + juce::String(static_cast<int>(gens.size()));
        feed_.push_back(formatTapeTime(ge.timestamp) + "  GENERATION  \"" + ge.promptA.substring(0, 40) + "\"");
        dirty = true;
    }

    const bool busy = processorRef.isReplayGenerationInFlight();
    if (busy != genBusy_) { genBusy_ = busy; dirty = true; }

    // Feed: notes + param edits that just passed the playhead
    const auto& notes = processorRef.getReplayNoteEventsForUi();
    while (feedNoteIdx_ < notes.size() && notes[feedNoteIdx_].timestamp <= playhead)
    {
        const auto& n = notes[feedNoteIdx_++];
        if (n.type == VoiceEvent::Type::NoteOn)   // offs would double the feed for no info
        {
            feed_.push_back(formatTapeTime(n.timestamp) + "  NOTE  "
                            + juce::MidiMessage::getMidiNoteName(n.note, true, true, 4)
                            + "  (" + sourceLabel(n.source) + ")");
            dirty = true;
        }
    }
    const auto& params = processorRef.getReplayParamEventsForUi();
    while (feedParamIdx_ < params.size() && params[feedParamIdx_].timestamp <= playhead)
    {
        const auto& pe = params[feedParamIdx_++];
        juce::String line = formatTapeTime(pe.timestamp) + "  ";
        if (auto* rap = processorRef.getValueTreeState().getParameter(pe.paramId))
            line << rap->getName(24) << "  " << rap->getText(rap->convertTo0to1(pe.value), 12);
        else
            line << pe.paramId << "  " << juce::String(pe.value, 3);
        feed_.push_back(line);
        dirty = true;
    }
    while (feed_.size() > kFeedLines)
        feed_.pop_front();

    if (dirty)
        repaint();
}

void ReplayOverlay::paint(juce::Graphics& g)
{
    g.fillAll(kCard);

    const auto b = getLocalBounds();
    const int  w = b.getWidth() - 2 * kMargin;

    // Header: REPLAY + tape name
    g.setColour(kAccent);
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("REPLAY", kMargin, kHeaderY, 110, 26, juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::Font(juce::FontOptions(15.0f)));
    g.drawText(tapeName_, kMargin + 118, kHeaderY + 2, w - 118 - 130, 24,
               juce::Justification::centredLeft);

    // Transport row: position + speed readout (slider is a child component)
    g.setColour(kTextPrimary);
    g.setFont(juce::Font(juce::FontOptions(17.0f)));
    g.drawText(posText_, kMargin, kTransportY - 4, 200, 24, juce::Justification::centredLeft);
    g.setColour(kDim);
    g.setFont(juce::Font(juce::FontOptions(14.0f)));
    g.drawText("Speed x" + juce::String(speedSlider.getValue(), 2),
               speedSlider.getX() - 110, kTransportY - 2, 100, 20,
               juce::Justification::centredRight);

    // Timeline strip
    timelineBounds_ = { kMargin, kTimelineY, w, kTimelineH };
    if (! timeline_.isValid() || timeline_.getWidth() != w)
        rebuildTimelineImage(w, kTimelineH);
    g.drawImageAt(timeline_, timelineBounds_.getX(), timelineBounds_.getY());
    g.setColour(kSurface.brighter(0.3f));
    g.drawRect(timelineBounds_);
    if (playheadPx_ >= 0)
    {
        g.setColour(kTextPrimary);
        g.fillRect(timelineBounds_.getX() + playheadPx_, timelineBounds_.getY(), 2, kTimelineH);
    }

    // Now row: the tape's current sound identity (start patch until the first
    // generation crossfades in)
    g.setFont(juce::Font(juce::FontOptions(15.0f)));
    if (nowGenIdx_ == static_cast<size_t>(-1))
    {
        g.setColour(kDimmer);
        g.drawText("start patch (no generation yet)", kMargin, kNowY, w, 22,
                   juce::Justification::centredLeft);
    }
    else
    {
        juce::String now = "\"" + nowPromptA_ + "\"";
        if (nowPromptB_.isNotEmpty())   // U+21C4 via fromUTF8 — raw non-ASCII literals mojibake in JUCE
            now << juce::String::fromUTF8("   \xE2\x87\x84   ") << "\"" << nowPromptB_ << "\"";
        g.setColour(kTextPrimary);
        g.drawText(now, kMargin, kNowY, w - 320, 22, juce::Justification::centredLeft);
        g.setColour(kDim);
        g.drawText(nowMetaText_ + (genBusy_ ? "   generating..." : ""),
                   kMargin + (w - 310), kNowY, 310, 22, juce::Justification::centredRight);
    }

    // Recent-event feed
    g.setFont(juce::Font(juce::FontOptions(13.0f)));
    int y = kFeedY;
    for (const auto& line : feed_)
    {
        g.setColour(kDimmer);
        g.drawText(line, kMargin, y, w, kFeedLineH, juce::Justification::centredLeft);
        y += kFeedLineH;
    }
}
