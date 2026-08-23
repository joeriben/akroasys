#include "MpeSettingsPage.h"
#include <cmath>

namespace
{
    // The per-note choices are not invented: 2 / 3 / 12 / 24 are exactly the
    // four a LinnStrument offers on its panel, 48 is the MPE spec's default and
    // that instrument's own MPE-mode setting, and 96 is the ceiling both the
    // spec and juce::MPEZoneLayout enforce. A device that transmits something
    // else is shown as it is -- see syncCombo.
    const int kPerNoteValues[] = { 2, 3, 12, 24, 48, 96 };
    const int kMasterValues[]  = { 2, 3, 12, 24, 48 };

    // What a full lateral lean is worth. One semitone is the smallest
    // unambiguously musical interval and is the default; the wide end is for
    // instruments whose Y is a long slide rather than a key that leans.
    const float kXScaleValues[] = { 0.5f, 1.0f, 2.0f, 3.0f, 7.0f, 12.0f, 24.0f };

    template <typename T, size_t N>
    constexpr int countOf (const T (&)[N]) { return (int) N; }

    /** Show `current` as the selection, adding it as an item when the device
        transmitted a value the table does not carry. Never snaps to a nearby
        entry: the point of the read-out below is that the page says what IS,
        and a silent round to the closest listed number is the opposite. */
    void syncCombo (juce::ComboBox& box, const int* values, int n, int current)
    {
        box.clear (juce::dontSendNotification);
        bool listed = false;
        for (int i = 0; i < n; ++i)
        {
            box.addItem (juce::String (values[i]) + " st", i + 1);
            listed = listed || values[i] == current;
        }
        if (! listed && current > 0)
            box.addItem (juce::String (current) + " st", n + 1);   // as transmitted

        for (int i = 0; i < box.getNumItems(); ++i)
            if (box.getItemText (i).getIntValue() == current)
            {
                box.setSelectedItemIndex (i, juce::dontSendNotification);
                return;
            }
    }

    juce::String semitoneText (float st)
    {
        return (st < 1.0f || st != std::floor (st))
                 ? juce::String (st, 2) + " st"
                 : juce::String ((int) st) + " st";
    }
}

MpeSettingsPage::MpeSettingsPage()
{
    for (auto* l : { &perNoteLabel_, &masterLabel_, &xScaleLabel_ })
    {
        l->setColour (juce::Label::textColourId, kTextPrimary);
        l->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (*l);
    }

    syncCombo (perNoteCombo_, kPerNoteValues, countOf (kPerNoteValues), 48);
    perNoteCombo_.onChange = [this]
    {
        const int v = perNoteCombo_.getText().getIntValue();
        shownPerNote_ = v;
        if (v > 0 && onPerNoteBendRangeChanged) onPerNoteBendRangeChanged (v);
        refreshReadout();
    };
    addAndMakeVisible (perNoteCombo_);

    syncCombo (masterCombo_, kMasterValues, countOf (kMasterValues), 2);
    masterCombo_.onChange = [this]
    {
        const int v = masterCombo_.getText().getIntValue();
        shownMaster_ = v;
        if (v > 0 && onMasterBendRangeChanged) onMasterBendRangeChanged (v);
    };
    addAndMakeVisible (masterCombo_);

    for (int i = 0; i < countOf (kXScaleValues); ++i)
        xScaleCombo_.addItem (semitoneText (kXScaleValues[i]), i + 1);
    xScaleCombo_.setSelectedId (2, juce::dontSendNotification);   // 1 semitone
    xScaleCombo_.onChange = [this]
    {
        const int idx = xScaleCombo_.getSelectedId() - 1;
        if (idx < 0 || idx >= countOf (kXScaleValues)) return;
        shownXScale_ = kXScaleValues[idx];
        if (onXFullScaleChanged) onXFullScaleChanged (shownXScale_);
        refreshReadout();
    };
    addAndMakeVisible (xScaleCombo_);

    clearBtn_.setColour (juce::TextButton::buttonColourId, kAccent.withAlpha (0.30f));
    clearBtn_.setColour (juce::TextButton::textColourOffId, kAccent);
    clearBtn_.onClick = [this]
    {
        if (onClearObservation) onClearObservation();
        refreshReadout();
    };
    addAndMakeVisible (clearBtn_);
}

MpeSettingsPage::~MpeSettingsPage()
{
    // BLOCKING, CLAUDE.md rule 1: before any member is destroyed.
    stopTimer();
}

void MpeSettingsPage::setRanges (int perNoteSemitones, int masterSemitones, float xFullScaleSemitones)
{
    shownPerNote_ = perNoteSemitones;
    shownMaster_  = masterSemitones;
    shownXScale_  = xFullScaleSemitones;
    syncCombo (perNoteCombo_, kPerNoteValues, countOf (kPerNoteValues), perNoteSemitones);
    syncCombo (masterCombo_,  kMasterValues,  countOf (kMasterValues),  masterSemitones);

    // Nearest listed X scale: unlike the two ranges, no device ever transmits
    // this one, so the only values it can hold are ones this page wrote.
    int best = 1;
    for (int i = 0; i < countOf (kXScaleValues); ++i)
        if (std::abs (kXScaleValues[i] - xFullScaleSemitones)
              < std::abs (kXScaleValues[best] - xFullScaleSemitones))
            best = i;
    xScaleCombo_.setSelectedId (best + 1, juce::dontSendNotification);
}

void MpeSettingsPage::visibilityChanged()
{
    updateWatch();              // a TAB switch; the overlay's own show/hide is the host's call
}

void MpeSettingsPage::updateWatch()
{
    if (isShowing())
    {
        refreshStatus();
        if (! isTimerRunning())
            startTimerHz (4);   // fast enough to feel live under a moving hand
    }
    else
    {
        stopTimer();
    }
}

void MpeSettingsPage::timerCallback()
{
    // Off screen: skip the work, but keep ticking. stopTimer() here would never
    // restart -- isShowing() is also false for a MINIMISED window, and neither
    // visibilityChanged() nor showSettings() fires on restore, so the read-out
    // would stay frozen until the user switched tabs. The real close path is
    // MainPanel::hideSettings() calling updateWatch(); what is left here costs
    // one walk up the parent chain, four times a second.
    if (! isShowing())
        return;
    refreshStatus();
}

void MpeSettingsPage::refreshStatus()
{
    if (! statusSource)
        return;

    // A range the DEVICE transmitted moves the control with it -- the combo
    // shows what is in force, and the read-out says who put it there. Only from
    // here, never from a combo's own onChange: syncCombo clears and refills the
    // box, which a box must not do while it is dispatching that callback.
    //
    // And never while the box's own popup is open. syncCombo can drop the "as
    // transmitted" item that a device's unlisted range added, and ComboBox
    // snapshots its menu by value when it opens -- the user would then click an
    // id that no longer exists and blank the control.
    const Status s = statusSource();

    if (s.perNoteBendRange != shownPerNote_ && ! perNoteCombo_.isPopupActive())
    {
        shownPerNote_ = s.perNoteBendRange;
        syncCombo (perNoteCombo_, kPerNoteValues, countOf (kPerNoteValues), s.perNoteBendRange);
    }
    if (s.masterBendRange != shownMaster_ && ! masterCombo_.isPopupActive())
    {
        shownMaster_ = s.masterBendRange;
        syncCombo (masterCombo_, kMasterValues, countOf (kMasterValues), s.masterBendRange);
    }

    refreshReadout();
}

void MpeSettingsPage::refreshReadout()
{
    if (! statusSource)
        return;

    const juce::String next = describe (statusSource());
    if (next == lastReadout_)
        return;                 // nothing new: no repaint, four times a second
    lastReadout_ = next;
    repaint (readoutBounds_);
}

juce::String MpeSettingsPage::channelList (juce::uint32 mask)
{
    if (mask == 0)
        return "none";

    juce::StringArray runs;
    int i = 1;
    while (i <= 16)
    {
        if ((mask & (1u << (i - 1))) == 0) { ++i; continue; }
        int j = i;
        while (j < 16 && (mask & (1u << j)) != 0) ++j;
        runs.add (j > i ? juce::String (i) + "-" + juce::String (j)
                        : juce::String (i));
        i = j + 1;
    }
    return runs.joinIntoString (", ");
}

juce::String MpeSettingsPage::describe (const Status& s)
{
    if (s.messages == 0)
        return "\t" "Nothing yet. Play a few notes and use whatever expression the instrument "
               "has - lean the keys sideways, press deeper, slide. This reads the live MIDI "
               "stream.";

    // One line per axis, key-tabbed-value. It was six paragraphs of prose first
    // and it clipped: the settings overlay's floor is 300 px (MainPanel.cpp) and
    // the read-out is the element that gives. A table also scans, which prose
    // four lines deep does not -- what a player wants from this page is one
    // glance at six facts, not a report.
    juce::StringArray out;
    auto line = [&out] (const char* key, juce::String value)
    {
        out.add (juce::String (key) + "\t" + value);
    };

    const juce::uint32 members = s.noteChannels & ~1u;
    if (members != 0)
        line ("Notes", "ch " + channelList (s.noteChannels) + " - per-note channels, MPE is live");
    else if ((s.noteChannels & 1u) != 0)
        line ("Notes", "channel 1 only - NOT sending per-note channels, so MPE is off");
    else
        line ("Notes", "none yet");

    line ("Bend", juce::String (s.perNoteBendRange) + " st"
                    + (s.rpnRange > 0
                         ? juce::String (", transmitted by the device on ch ") + juce::String (s.rpnChannel)
                         : juce::String (" - assumed, the device transmitted none")));

    line ("Zone", juce::String (s.upperZoneActive ? "upper + lower" : "lower") + ", "
                    + juce::String (s.memberChannels) + " members"
                    + (s.mcmSeen ? ", declared by the device" : " - assumed, none declared"));

    // min <= max, not max >= 0: "Watch again" resets these four atomics from the
    // message thread and can land between the audio thread's load and its store,
    // leaving min at its empty sentinel of 16384 with max already written. That
    // read as a full 8192 lean -- 100% of the axis -- in the very tick after the
    // player asked the page to start watching again.
    if (s.bendMin <= s.bendMax)
    {
        const int widest = juce::jmax (std::abs (s.bendMin - 8192), std::abs (s.bendMax - 8192));
        const float st   = (float) widest / 8192.0f * (float) s.perNoteBendRange;
        const int   pct  = juce::roundToInt (juce::jmin (1.0f, st / juce::jmax (0.001f, s.xFullScale)) * 100.0f);
        line ("Lean X", juce::String (widest) + " of 8192 = " + juce::String (st, 2)
                          + " st = " + juce::String (pct) + "% of the axis");
    }
    else
    {
        line ("Lean X", "no pitch bend received");
    }

    if (s.cc74Min <= s.cc74Max)
        line ("Slide Y", "CC74 " + juce::String (s.cc74Min) + ".." + juce::String (s.cc74Max)
                           + " on ch " + channelList (s.timbreChannels)
                           + (((s.timbreChannels & ~1u) != 0) ? ""
                                : " - ch 1 is read as the Scan knob, not as Y"));
    else
        line ("Slide Y", "no CC74 received");

    juce::StringArray z;
    if (s.memberPressureChannels != 0) z.add ("per-note on ch " + channelList (s.memberPressureChannels));
    if (s.masterPressure)              z.add ("channel-wide on the zone master");
    if (s.polyPressure)                z.add ("poly aftertouch");
    line ("Press Z", z.isEmpty() ? juce::String ("nothing received") : z.joinIntoString (", "));

    // The one sentence that turns "assumed" into something the player can act
    // on, and only while it applies. A key of "!" is the footer marker.
    if (s.rpnRange == 0 || ! s.mcmSeen)
        out.add ("!\tA device announces zone and range ONCE, at MPE-mode select - a plugin "
                 "opened later never hears it. Reconnect it with this window open, or set the "
                 "values above by hand.");

    return out.joinIntoString ("\n");
}

void MpeSettingsPage::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));

    g.setColour (kTextMuted);
    g.setFont (juce::FontOptions (11.0f));
    g.drawFittedText ("Describes the CONTROLLER, not the patch: machine-wide, never saved with a "
                      "preset. The preset carries the depths, this carries what a full gesture is "
                      "worth here.",
                      introBounds_, juce::Justification::topLeft, 2);

    // Section rule, same grammar as the model page's family headers.
    g.setFont (juce::FontOptions (10.0f).withStyle ("Bold"));
    g.setColour (kDimmer);
    g.drawText ("WHAT THE CONTROLLER IS SENDING", sectionBounds_.reduced (2, 0),
                juce::Justification::bottomLeft, false);
    g.setColour (kBorder);
    g.drawHorizontalLine (sectionBounds_.getBottom() - 1,
                          (float) sectionBounds_.getX(), (float) sectionBounds_.getRight());

    // Key column fixed, value column the rest; the footer spans both.
    auto        rows = readoutBounds_;
    const int   lineH = 15;
    const int   keyW  = 54;
    juce::StringArray lines;
    lines.addLines (lastReadout_);

    for (auto& l : lines)
    {
        if (rows.getHeight() < lineH) break;

        const int   tab   = l.indexOfChar ('\t');
        const auto  key   = tab >= 0 ? l.substring (0, tab) : juce::String();
        const auto  value = tab >= 0 ? l.substring (tab + 1) : l;

        if (key == "!")
        {
            // Two lines and the last thing on the page: give it what is left.
            g.setColour (kAccent);
            g.setFont (juce::FontOptions (11.0f));
            g.drawFittedText (value, rows.reduced (0, 2), juce::Justification::topLeft, 3);
            break;
        }

        g.setFont (juce::FontOptions (11.0f));
        if (key.isEmpty())
        {
            // A keyless line is a whole paragraph, not a table row -- the
            // "nothing received yet" case. It gets what is left, like the footer.
            g.setColour (kTextMuted);
            g.drawFittedText (value, rows, juce::Justification::topLeft, 4);
            break;
        }

        auto row = rows.removeFromTop (lineH);
        g.setColour (kTextPrimary);
        g.drawText (key, row.removeFromLeft (keyW), juce::Justification::centredLeft, false);
        g.setColour (kTextMuted);
        g.drawFittedText (value, row, juce::Justification::centredLeft, 1);
    }
}

void MpeSettingsPage::resized()
{
    layoutRows();
}

void MpeSettingsPage::layoutRows()
{
    auto r = getLocalBounds().reduced (18);

    // 24 px, two lines at 11 px: the read-out below is the element that has to
    // fit at the overlay's 300 px floor, and the footer it ends with is the one
    // sentence a player can act on.
    introBounds_ = r.removeFromTop (24);
    r.removeFromTop (8);

    auto row = [&r] (juce::Label& l, juce::ComboBox& c)
    {
        auto line = r.removeFromTop (26);
        l.setBounds (line.removeFromLeft (150));
        line.removeFromLeft (10);
        c.setBounds (line.removeFromLeft (110));
        r.removeFromTop (6);
    };
    row (perNoteLabel_, perNoteCombo_);
    row (masterLabel_,  masterCombo_);
    row (xScaleLabel_,  xScaleCombo_);

    r.removeFromTop (8);
    sectionBounds_ = r.removeFromTop (15);
    auto btn = sectionBounds_;
    clearBtn_.setBounds (btn.removeFromRight (96).withHeight (20).withY (btn.getY() - 3));
    r.removeFromTop (6);

    // The read-out takes whatever is left: the overlay's floor is 300 px, so
    // this must be the element that gives rather than a fixed block that clips.
    readoutBounds_ = r;
}
