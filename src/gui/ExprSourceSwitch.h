#pragma once
#include <JuceHeader.h>
#include "GuiHelpers.h"
#include "../dsp/BlockParams.h"

// ─────────────────────────────────────────────────────────────────────────────
// ExprSourceSwitch — which of the player's expression axes drives one target, or
// none. ONE small box at the right-hand end of that target's AftertouchBar,
// showing the state it is in; a click steps to the next.
//
//      V velocity   X per-note bend   Y CC74   Z pressure   Ø nothing
//
// Cycling order is ExprSource order, so the box and the enum cannot drift — Ø
// is last in the enum because a DAW session stores a Choice as its index, and
// it therefore comes round after Z rather than before V.
//
// It was a 2x2 grid of all four letters first, which showed the whole choice at
// once and read as far too much furniture down a column of sixteen rows — the
// column is a set of depths, and the axis is a footnote on each. So the box
// carries only the current letter, and the choice is one click away rather than
// four permanently on screen.
//
// Ø is the quiet state and the ONLY grey one: muted on the module's surface,
// the way a bar at zero is muted, because it says the same thing — nothing is
// driving this. Every real axis takes the module's orange and a white letter, so
// what is WIRED is what shows down a column of sixteen, and how many are wired
// reads without being counted.
//
// Backed by a juce::ComboBox for the same reason AftertouchBar is backed by a
// Slider: an APVTS ComboBoxAttachment then drives the value with no bespoke
// parameter plumbing. The popup is what we do NOT want, so mouseDown steps the
// selection instead of opening it.
// ─────────────────────────────────────────────────────────────────────────────
class ExprSourceSwitch : public juce::ComboBox
{
public:
    ExprSourceSwitch()
    {
        for (int i = 0; i < ExprSource::kCount; ++i)
            addItem(ExprSource::kEntries[i].label, i + 1);   // ComboBox ids are 1-based
        setSelectedId(ExprSource::None + 1, juce::dontSendNotification);
        setWantsKeyboardFocus(false);
    }

    /** Right-click hook, so the switch offers the same MIDI-learn menu as the
        bar beside it rather than being the one dead spot in the row. */
    std::function<void(juce::Point<int>)> onRightClick;

    /** The player picked an axis on this row. The row is "manually changed" from
        here on, so the panel stops drawing it OFF whatever its amount says. */
    std::function<void()> onUserPick;

    /** The ROW is off, whatever this switch's value says: its amount is at zero
        and its source has never been moved off the default it shipped with. Then
        the box draws Ø like any unrouted row, because that is the truth - a wired
        source at depth zero drives nothing, and three of the sixteen rows ship
        that way, so a fresh patch showed three lit boxes for three things that
        were not happening. BJ, 26.08.2026: "in der Expression-Spalte sollen alle
        Parameter die =0 liegen UND nicht bereits manuell verändert wurden auf OFF
        stehen."

        The VALUE is untouched, deliberately. Raise that row's amount off zero and
        the default axis is right there and working - which is what the defaults
        were for. Only the claim that it is doing something now goes. */
    void setRowIsOff (bool off)
    {
        if (off == rowIsOff_)
            return;
        rowIsOff_ = off;
        repaint();
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            if (onRightClick) onRightClick(e.getPosition());
            return;
        }
        const int stored = juce::jmax(0, getSelectedId() - 1);
        const bool wasOff = rowIsOff_;
        // Drop OFF here, not a timer tick later: the click makes this row a
        // touched row by definition, so waiting for the panel to notice would
        // paint one stale Ø frame over the value it is about to show.
        rowIsOff_ = false;
        if (onUserPick) onUserPick();
        if (wasOff && stored != ExprSource::None)
        {
            // The row draws Ø and HOLDS an axis - its own default, or the Z a
            // file predating sources wrote into all sixteen rows. The first
            // click REVEALS that value instead of stepping past it: the player
            // cannot see what they would be replacing, and stepping would drop
            // a stored routing on a click that looks like it is only arming the
            // row. The step happens on the next click, from a value now on
            // screen. A row that really holds Ø steps straight on, so no click
            // anywhere in the cycle is invisible.
            repaint();
            return;
        }
        const int next = (stored + 1) % ExprSource::kCount;
        setSelectedId(next + 1, juce::sendNotificationSync);
        repaint();
    }

    // The base class opens its popup on these; nothing here should.
    void mouseUp(const juce::MouseEvent&) override {}
    void mouseDoubleClick(const juce::MouseEvent&) override {}

    // A ComboBox draws the selected item's text through a CHILD Label that is
    // painted OVER paint() and is re-coloured on every layout by the look and
    // feel (T5ynthLookAndFeel::positionComboBoxText), so setting a transparent
    // ComboBox::textColourId does not survive. Hiding the child does. resized()
    // is the hook because that is what re-runs positionComboBoxText.
    void resized() override
    {
        juce::ComboBox::resized();
        for (int i = 0; i < getNumChildComponents(); ++i)
            if (auto* c = getChildComponent(i))
                c->setVisible(false);
    }

    void paint(juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const int selected = juce::jlimit(0, ExprSource::kCount - 1, getSelectedId() - 1);
        const bool routed = (selected != ExprSource::None) && ! rowIsOff_;

        g.setColour(routed ? kAtCol : kSurface);
        g.fillRect(b);

        g.setColour(routed ? juce::Colours::white : kTextMuted);
        g.setFont(juce::FontOptions(juce::jlimit(9.0f, 13.0f, b.getHeight() * 0.58f)));
        // U+00D8 through CharPointer_UTF8, never a bare literal: a juce::String
        // built from a const char* is read as Latin-1 and the two bytes would
        // come out as two characters. The four real axes are ASCII and take the
        // table's own label.
        //
        // U+00D8 and not U+2205 EMPTY SET, which is the obvious choice and is
        // absent from every font this app can reach — Helvetica, Helvetica Neue,
        // SF NS and Arial all lack it, and "Inter" (the requested default) is not
        // installed, so the letters render in SF. A missing glyph would be left
        // to CoreText substitution at 14 px beside four ASCII capitals. Ø is a
        // plain Latin letter, present in all four, and is the crossed-out o this
        // is meant to be.
        const juce::String glyph = routed
            ? juce::String(ExprSource::kEntries[selected].label)
            : juce::String(juce::CharPointer_UTF8("\xC3\x98"));
        g.drawText(glyph, b, juce::Justification::centred, false);

        g.setColour(kBorder);
        g.drawRect(b, 1.0f);
    }

private:
    bool rowIsOff_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExprSourceSwitch)
};
