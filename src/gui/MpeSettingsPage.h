#pragma once
#include <JuceHeader.h>
#include "GuiHelpers.h"

class T5ynthProcessor;

// ─────────────────────────────────────────────────────────────────────────────
// MpeSettingsPage — the "MPE" tab beside Sound Models / Language Model /
// Settings.
//
// Why a tab at all, when MPE is a standard. Because the parts of it that carry
// NUMBERS are negotiated exactly once: a controller announces its zone with an
// MPE Configuration Message and its bend range with RPN 0, both at power-on or
// when its MPE mode is selected. A plugin instantiated after that moment never
// hears either, and MIDI has no read-back to ask with -- so it falls back to a
// default and cannot know whether the default is what the device meant. An
// Osmose capture of 120 s of ordinary playing (tools/midi_monitor.cpp) contains
// no RPN 0 at all, and that is not a fault of the device: it had already said
// so, before anyone was listening.
//
// So the page is two halves and the lower one is the reason for the upper one:
//   • what the controller is sending, watched live off the audio thread
//   • the three numbers to set once you can see it
//
// Machine-wide, never part of a preset. That is what lets one preset drive
// different MPE controllers: the preset carries DEPTHS, this carries what a
// full gesture is worth on THIS instrument, so a depth of 0.6 on X means the
// same lean on an Osmose and on a LinnStrument.
//
// Processor-agnostic in the same way its sibling pages are: it emits
// on...Changed and reads its read-out through statusSource, and MainPanel wires
// both to the processor.
// ─────────────────────────────────────────────────────────────────────────────
class MpeSettingsPage : public juce::Component,
                        private juce::Timer
{
public:
    MpeSettingsPage();
    ~MpeSettingsPage() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** Seed the controls from the stored values without firing the callbacks. */
    void setRanges (int perNoteSemitones, int masterSemitones, float xFullScaleSemitones);

    std::function<void(int)>   onPerNoteBendRangeChanged;
    std::function<void(int)>   onMasterBendRangeChanged;
    std::function<void(float)> onXFullScaleChanged;

    /** Forget everything observed so far. Wired to the "Watch again" button. */
    std::function<void()> onClearObservation;

    /** Everything the read-out prints, formatted here rather than upstream.
        Asked ~4x a second while this page is VISIBLE and never otherwise --
        a timer running behind a closed overlay is the idle-CPU pattern
        docs/PERFORMANCE_GUIDE.md catalogues. */
    struct Status
    {
        int          messages = 0;
        juce::uint32 noteChannels = 0;
        juce::uint32 bendChannels = 0;
        juce::uint32 timbreChannels = 0;
        juce::uint32 memberPressureChannels = 0;
        bool         masterPressure = false;
        bool         polyPressure = false;
        int          rpnRange = 0;
        int          rpnChannel = 0;
        bool         mcmSeen = false;
        int          cc74Min = 128, cc74Max = -1;
        int          bendMin = 16384, bendMax = -1;
        int          perNoteBendRange = 48;
        int          masterBendRange = 2;
        float        xFullScale = 1.0f;
        int          memberChannels = 0;
        bool         upperZoneActive = false;
    };
    std::function<Status()> statusSource;

    /** Re-read statusSource once, now. Called by MainPanel when the overlay
        opens: re-showing an overlay on the tab that was already current does
        not flip this page's own visibility flag, so the page alone cannot
        notice it -- the same reason LroAuthorSettingsPage::refreshApiSpend
        exists. */
    void refreshStatus();

    /** Start or stop the 4 Hz watch by whether this page is actually ON SCREEN.
        The host MUST call it when the overlay is shown or hidden:
        Component::setVisible fires visibilityChanged() on that component ALONE
        and never recurses into its children (juce_Component.cpp:278-322), so
        hiding the tab strip leaves this page's own visible flag true and its
        timer running behind a closed overlay -- precisely the idle-CPU pattern
        docs/PERFORMANCE_GUIDE.md catalogues. isVisible() cannot see that;
        isShowing() walks the parent chain and can. */
    void updateWatch();

    /** The text only -- safe to call from a control's own callback. */
    void refreshReadout();

private:
    void timerCallback() override;
    void visibilityChanged() override;
    void layoutRows();

    /** "2-9", "1, 5-7", or "none" for a 16-bit channel mask. */
    static juce::String channelList (juce::uint32 mask);
    static juce::String describe (const Status& s);

    juce::Label    perNoteLabel_ { {}, "Bend range, per note" };
    juce::ComboBox perNoteCombo_;
    juce::Label    masterLabel_  { {}, "Bend range, master" };
    juce::ComboBox masterCombo_;
    juce::Label    xScaleLabel_  { {}, "Full lean (X) is" };
    juce::ComboBox xScaleCombo_;

    juce::TextButton clearBtn_ { "Watch again" };

    // The read-out is PAINTED, not a Label: it is one block of prose that
    // changes only when the device does something new, and repainting it only
    // then is the difference between a quiet page and a 4 Hz repaint of a
    // component tree behind an overlay (docs/PERFORMANCE_GUIDE.md).
    juce::Rectangle<int> introBounds_, sectionBounds_, readoutBounds_;
    juce::String         lastReadout_;
    int                  shownPerNote_ = 0;      // what the combos currently show,
    int                  shownMaster_  = 0;      // so an RPN 0 that moves the range
    float                shownXScale_  = 0.0f;   // in force moves them with it

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MpeSettingsPage)
};
