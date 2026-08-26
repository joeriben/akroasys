#include "FxPanel.h"
#include <cmath>
#include "../dsp/BlockParams.h"
#include "../PluginProcessor.h"
#include "MidiLearnMenu.h"
#include "../ProductName.h"

static juce::String fmtMs(double v)
{
    int ms = juce::roundToInt(v);
    if (ms >= 1000) return juce::String(v / 1000.0, 2) + "s";
    return juce::String(ms) + "ms";
}
// Snap to the printed precision first: the phaser's feedback is bipolar and
// lands on a tiny negative often enough that the read-out said "-0.00".
static juce::String fmtF2(double v)  { return juce::String(std::abs(v) < 0.005 ? 0.0 : v, 2); }
static juce::String fmtF3(double v)  { return juce::String(v, 3); }
static juce::String fmtDb1(double v) { return juce::String(v, 1) + "dB"; }
static juce::String fmtHz2(double v) { return juce::String(v, 2) + "Hz"; }

// The chorus's Amt is a fraction of the ensemble's own sweep, so it can say what
// it actually does: how far each of the three taps travels either side of the
// centre delay. The sweep comes from T5ynthChorus rather than a second copy of
// the number here, which could drift away from the one the DSP uses.
static juce::String fmtChorusSweep(double v)
{
    return juce::String::charToString(0x00b1)   // PLUS-MINUS SIGN, never a literal
         + juce::String(v * T5ynthChorus::kSweepMs, 2) + "ms";
}

// The phaser's Amt is a fraction of its own sweep span, so it can say what it
// actually does: how far the corner travels either side of the fixed 600 Hz
// centre. The span comes from T5ynthPhaser rather than a second copy of the
// number here, which could drift away from the one the DSP uses.
static juce::String fmtPhaserSweep(double v)
{
    return juce::String::charToString(0x00b1)   // PLUS-MINUS SIGN, never a literal
         + juce::String(v * T5ynthPhaser::kSpanOctaves, 2) + "oct";
}

static juce::String fmtDampPct(double v)
{
    // Damp is a normalized 0..1 trim, NOT a single cutoff: the DSP adds a
    // per-mode intrinsic baseline rolloff (Tape darker than Digital at the same
    // value, and it compounds per feedback pass), so a kHz readout would
    // misrepresent it. Show the knob amount instead.
    return juce::String(juce::roundToInt(v * 100.0)) + "%";
}

// ── Family tables ────────────────────────────────────────────────────────────
// Each family cell in the type switchboxes owns a dropdown listing exactly its
// own variants. The tables give the menu ORDER (mildest -> wildest, which is not
// the parameter's index order) and the short cell labels.
static const int kTapeFamily[] = { DelayType::Tape, DelayType::TapeWarm,
                                   DelayType::TapeWild, DelayType::TapeOld };
static const int kBbdFamily[]  = { DelayType::BbdClean, DelayType::Bbd,
                                   DelayType::BbdDegraded };
static const int kPlateFamily[] = { ReverbType::Dark, ReverbType::Medium, ReverbType::Bright };
static const int kAlgoFamily[]  = { ReverbType::Algo, ReverbType::AlgoPlus };

// Cell labels for the active variant. Indexed by DelayType::character().
// Family prefix stays visible when active ("Tp Clean", not a bare "Clean"):
// without it the cell no longer said WHICH family is running, and Tape and BBD
// both own a "Clean"/"Warm" variant, so the bare word was ambiguous. "Degrd"
// is elided to keep one line in the cell; the tooltip carries the full name.
static const char* kTapeVariantLbls[] = { "Tp Clean", "Tp Warm", "Tp Wild", "Tp Old" };
static const char* kBbdVariantLbls[]  = { "BBD Warm", "BBD Clean", "BBD Degrd" };

// The title row, in the order the signal takes: the amplifier chain first
// (PluginProcessor's processBlock — the dirt on the note, the modulation on the
// dirty note, the tremolo last, as in an amp), then delay and reverb, which run
// serially after it with the reverb send taken behind the delay.
static const char* kFxSelLabels[] = { "Dist", "Chor", "Phas", "Trem", "Dly", "Rev" };
static const char* kFxSelTips[] = {
    "Distortion - an overdriven amplifier, first in the chain",
    "Chorus",
    "Phaser",
    "Tremolo - last of the amp chain, as in an amplifier's power stage",
    "Delay",
    "Reverb - its send is taken behind the delay"
};

FxPanel::FxPanel(juce::AudioProcessorValueTreeState& apvts, T5ynthProcessor& processor)
    : processorRef(processor)
{
    // ══════════ The switch title row ══════════
    // It is the card's heading and its navigation at once, which is what makes
    // six effects fit where two cards used to be: no per-effect header strip,
    // no per-effect switchbox row. A cell's TEXT reports whether that effect is
    // running (see effectRunning), so the row states all six at a glance and the
    // selection only decides which one is being edited.
    for (int i = 0; i < kNumFxSel; ++i)
    {
        fxSelBtns[i].setButtonText(kFxSelLabels[i]);
        fxSelBtns[i].setTooltip(kFxSelTips[i]);
        styleSwitchButton(fxSelBtns[i], kFxCol);
        fxSelBtns[i].setClickingTogglesState(false);
        fxSelBtns[i].onClick = [this, i]
        {
            if (fxSelected_ == i) return;
            fxSelected_ = i;
            updateVisibility();
            resized();     // a different effect fills the rows; this one DOES relayout
        };
        addAndMakeVisible(fxSelBtns[i]);
    }

    // ══════════ DELAY section ══════════
    // Delay type switchbox: 5 cells [OFF][Dig][PP][Tape][BBD].
    //
    // Tape and BBD are FAMILIES, not single voicings — each has character variants.
    // Those variants used to be reached by clicking the cell again and again
    // (Tp1→Tp2→Tp3→Tp4), which hid four states behind one button and was the only
    // place in this UI that worked that way. Each family cell now opens its OWN
    // dropdown instead: one dropdown per category, listing only that category's
    // variants. Off/Digital/Ping-Pong have no variants, so they stay plain cells.
    //
    // All 10 choices map to the flat delayType APVTS param via delayTypeHidden.
    {
        juce::StringArray delayTypeItems;
        for (const auto& e : DelayType::kEntries) delayTypeItems.add(e.label);
        delayTypeHidden.addItemList(delayTypeItems, 1);
    }

    // Update toggle states + family-cell labels whenever the APVTS selection
    // changes. A family cell shows the FAMILY name while another type is active
    // and the ACTIVE VARIANT's name while its own family is playing, so the panel
    // always states which character is running. Full name in the tooltip.
    delayTypeHidden.onChange = [this]
    {
        const int dt       = delayTypeHidden.getSelectedId() - 1;
        const int baseType = DelayType::baseMode(dt);
        for (int i = 0; i < kNumDelayBtns; ++i)
            delayTypeBtns[i].setToggleState(i == baseType, juce::dontSendNotification);

        const bool tapeOn = (baseType == DelayType::Tape);
        const bool bbdOn  = (baseType == DelayType::Bbd);
        delayTypeBtns[3].setButtonText(tapeOn ? kTapeVariantLbls[DelayType::character(dt)] : "Tape");
        delayTypeBtns[4].setButtonText(bbdOn  ? kBbdVariantLbls[DelayType::character(dt)]   : "BBD");
        delayTypeBtns[3].setTooltip(tapeOn ? juce::String("Tape echo - ") + DelayType::kEntries[dt].label
                                           : juce::String("Tape echo - pick a character"));
        delayTypeBtns[4].setTooltip(bbdOn  ? juce::String("Bucket-brigade - ") + DelayType::kEntries[dt].label
                                           : juce::String("Bucket-brigade - pick a character"));
        updateVisibility();
    };

    static const char* delayLabels[] = { "OFF", "Dig", "PP", "Tape", "BBD" };
    for (int i = 0; i < kNumDelayBtns; ++i)
    {
        delayTypeBtns[i].setButtonText(delayLabels[i]);
        styleSwitchButton(delayTypeBtns[i], kFxCol);
        // The toggle state is driven by delayTypeHidden.onChange, never by the
        // click itself: a family click only OPENS a menu, and dismissing that
        // menu must leave the switchbox exactly as it was.
        delayTypeBtns[i].setClickingTogglesState(false);
        addAndMakeVisible(delayTypeBtns[i]);
    }
    // Off / Digital / Ping-Pong: no variants, simple select.
    for (int i = 0; i < 3; ++i)
        delayTypeBtns[i].onClick = [this, i] { delayTypeHidden.setSelectedId(i + 1); };
    // Tape / BBD: the cell IS the dropdown for its family, and wears the corner
    // triangle that says so — the same one every ComboBox here carries.
    delayTypeBtns[3].onClick = [this] { showDelayFamilyMenu(3, kTapeFamily, 4); };
    delayTypeBtns[4].onClick = [this] { showDelayFamilyMenu(4, kBbdFamily,  3); };
    setSwitchMenuCell(delayTypeBtns[3]);
    setSwitchMenuCell(delayTypeBtns[4]);

    delayTimeRow = std::make_unique<SliderRow>("Time", fmtMs, kFxCol);
    delayFbRow   = std::make_unique<SliderRow>("FB",   fmtF2, kFxCol);
    delayDampRow = std::make_unique<SliderRow>("Damp", fmtDampPct, kFxCol);
    delayMixRow  = std::make_unique<SliderRow>("Mix",  fmtF3, kFxCol);

    // Division row swaps in for the Time slider when ClockMode == Sync.
    // Same screen rect, label "Time", but the value formatter returns
    // a musical-division name and the slider is stepped 0..12.
    delayDivisionRow = std::make_unique<SliderRow>("Time",
        [](double v) {
            const int idx = juce::jlimit(0, ClockDivision::kCount - 1,
                                          juce::roundToInt(v));
            return juce::String(ClockDivision::kEntries[idx].label);
        }, kFxCol);
    delayDivisionRow->getSlider().setRange(
        0.0, static_cast<double>(ClockDivision::kCount - 1), 1.0);

    for (auto* r : { delayTimeRow.get(), delayFbRow.get(), delayDampRow.get(),
                     delayMixRow.get(), delayDivisionRow.get() })
    {
        r->setInlineLabel(true);      // the standard bar — see the note above resized()
        addAndMakeVisible(*r);
    }

    // BPM-sync clock button — sits left of the Time row. Hidden ComboBox
    // holds the APVTS state, click cycles it Off ↔ Sync.
    juce::StringArray clockItems;
    for (const auto& e : ClockMode::kEntries) clockItems.add(e.label);
    delayClockModeHidden.addItemList(clockItems, 1);
    delayClockBtn.setLookAndFeel(&delayClockLnf);
    // The clock toggle sits in the Time row's label slot, so it doubles as that
    // row's band: cyan @0.7 when free (matches the sibling label bands below),
    // amber when tempo-synced (the shared sync indicator).
    delayClockLnf.offFill = kFxCol.withAlpha(0.7f);
    delayClockBtn.setClickingTogglesState(false);
    delayClockBtn.onClick = [this] {
        const int cur = delayClockModeHidden.getSelectedId();
        delayClockModeHidden.setSelectedId(cur == 1 ? 2 : 1);
    };
    addAndMakeVisible(delayClockBtn);

    delayTimeA      = std::make_unique<SA>(apvts, PID::delayTime,     delayTimeRow->getSlider());
    delayFbA        = std::make_unique<SA>(apvts, PID::delayFeedback, delayFbRow->getSlider());
    delayDampA      = std::make_unique<SA>(apvts, PID::delayDamp,     delayDampRow->getSlider());
    delayMixA       = std::make_unique<SA>(apvts, PID::delayMix,      delayMixRow->getSlider());
    delayDivisionA  = std::make_unique<SA>(apvts, PID::delayClockDivision,
                                           delayDivisionRow->getSlider());

    delayTimeRow->onRightClick     = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::delayTime,            p); };
    delayFbRow->onRightClick       = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::delayFeedback,        p); };
    delayDampRow->onRightClick     = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::delayDamp,            p); };
    delayMixRow->onRightClick      = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::delayMix,             p); };
    delayDivisionRow->onRightClick = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::delayClockDivision,   p); };

    delayTimeRow->updateValue();
    delayFbRow->updateValue();
    delayDampRow->updateValue();
    delayMixRow->updateValue();
    delayDivisionRow->updateValue();

    // Wire onChange BEFORE attachment so the CA's initial setSelectedId
    // call fires it and syncs the visible state.
    delayClockModeHidden.onChange = [this] {
        const bool sync = delayClockModeHidden.getSelectedId() == 2;
        delayClockBtn.setToggleState(sync, juce::dontSendNotification);
        delayClockBtn.repaint();
        // The Time/Division swap goes through updateVisibility rather than
        // setting the two rows here: since the title row, "which of the two" is
        // no longer the whole question — the other five effects own these rows'
        // space when one of them is selected, and a direct setVisible(true) here
        // would put a delay slider on top of the phaser.
        updateVisibility();
    };

    // Attach APVTS AFTER buttons are set up (triggers onChange → updateVisibility)
    delayTypeA       = std::make_unique<CA>(apvts, PID::delayType,      delayTypeHidden);
    delayClockModeA  = std::make_unique<CA>(apvts, PID::delayClockMode, delayClockModeHidden);

    // ══════════ REVERB section ══════════
    // Reverb type switchbox: 3 cells [OFF][Plate][Freeverb].
    //
    // Dark/Med/Brt were never three reverbs — they are three IRs of the SAME
    // EMT-140 plate, one reverb in three tone colours, so they belong under one
    // cell with their own dropdown. Freeverb likewise now has two voicings
    // (Freeverb and Freeverb+, the latter with the early-reflection front end
    // Freeverb omits), reached from that cell's dropdown. One dropdown per
    // category, exactly like Tape and BBD above.
    juce::StringArray reverbTypeItems;
    for (const auto& e : ReverbType::kEntries) reverbTypeItems.add(e.label);
    reverbTypeHidden.addItemList(reverbTypeItems, 1);
    reverbTypeHidden.onChange = [this]
    {
        const int rt = reverbTypeHidden.getSelectedId() - 1;
        const bool plateOn = ReverbType::isPlate(rt);
        const bool algoOn  = ReverbType::isAlgorithmic(rt);
        reverbTypeBtns[0].setToggleState(rt == ReverbType::Off, juce::dontSendNotification);
        reverbTypeBtns[1].setToggleState(plateOn, juce::dontSendNotification);
        reverbTypeBtns[2].setToggleState(algoOn,  juce::dontSendNotification);
        // Same rule as the delay families: family name when idle, the active
        // variant's own name when playing.
        reverbTypeBtns[1].setButtonText(plateOn ? ReverbType::kEntries[rt].label : "Plate");
        reverbTypeBtns[2].setButtonText(algoOn  ? ReverbType::kEntries[rt].label : "Freeverb");
        reverbTypeBtns[1].setTooltip(plateOn ? juce::String("EMT-140 plate - ") + ReverbType::kEntries[rt].label
                                             : juce::String("EMT-140 plate - pick a tone colour"));
        reverbTypeBtns[2].setTooltip(algoOn  ? juce::String("Algorithmic - ") + ReverbType::kEntries[rt].label
                                             : juce::String("Algorithmic - pick a voicing"));
        updateVisibility();
    };

    static const char* reverbLabels[] = { "OFF", "Plate", "Freeverb" };
    for (int i = 0; i < kNumReverbBtns; ++i)
    {
        reverbTypeBtns[i].setButtonText(reverbLabels[i]);
        styleSwitchButton(reverbTypeBtns[i], kFxCol);
        reverbTypeBtns[i].setClickingTogglesState(false);  // see the delay cells
        addAndMakeVisible(reverbTypeBtns[i]);
    }
    reverbTypeBtns[0].onClick = [this] { reverbTypeHidden.setSelectedId(ReverbType::Off + 1); };
    reverbTypeBtns[1].onClick = [this] { showReverbFamilyMenu(1, kPlateFamily, 3); };
    reverbTypeBtns[2].onClick = [this] { showReverbFamilyMenu(2, kAlgoFamily,  2); };
    setSwitchMenuCell(reverbTypeBtns[1]);
    setSwitchMenuCell(reverbTypeBtns[2]);

    reverbMixRow  = std::make_unique<SliderRow>("Mix",   fmtF3, kFxCol);
    algoRoomRow   = std::make_unique<SliderRow>("Room",  fmtF2, kFxCol);
    algoDampRow   = std::make_unique<SliderRow>("Damp",  fmtF2, kFxCol);
    algoWidthRow  = std::make_unique<SliderRow>("Width", fmtF2, kFxCol);

    for (auto* r : { reverbMixRow.get(), algoRoomRow.get(), algoDampRow.get(), algoWidthRow.get() })
    {
        r->setInlineLabel(true);
        addAndMakeVisible(*r);
    }

    reverbMixA = std::make_unique<SA>(apvts, PID::reverbMix,   reverbMixRow->getSlider());
    algoRoomA  = std::make_unique<SA>(apvts, PID::algoRoom,    algoRoomRow->getSlider());
    algoDampA  = std::make_unique<SA>(apvts, PID::algoDamping, algoDampRow->getSlider());
    algoWidthA = std::make_unique<SA>(apvts, PID::algoWidth,   algoWidthRow->getSlider());

    reverbMixRow->onRightClick = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::reverbMix,    p); };
    algoRoomRow->onRightClick  = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::algoRoom,     p); };
    algoDampRow->onRightClick  = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::algoDamping,  p); };
    algoWidthRow->onRightClick = [this](juce::Point<int> p) { showMidiLearnMenu(processorRef, PID::algoWidth,    p); };

    reverbMixRow->updateValue();
    algoRoomRow->updateValue();
    algoDampRow->updateValue();
    algoWidthRow->updateValue();

    // ══════════ The amplifier chain: distortion, chorus, phaser, tremolo ══════════
    // Twelve parameters that have run in every block and saved into every preset
    // since they were added, and that nothing in the GUI could reach.
    //
    // Their OFF is a bool of its own, because unlike delay and reverb they have
    // no type parameter to hold an Off value. The visible cell is a plain button
    // and the APVTS state sits on a hidden ToggleButton — the same shape the two
    // type switchboxes above use with their hidden ComboBoxes, so the click path
    // and the automation path stay separate here too.
    for (int i = 0; i < kNumAmpFx; ++i)
    {
        styleSwitchButton(ampStateBtns[i], kFxCol);
        ampStateBtns[i].setClickingTogglesState(false);
        ampStateBtns[i].onClick = [this, i]
        {
            // The cell owns the BYPASS and nothing else, so the click flips the
            // bypass and nothing else. It does not reach for the Mix: raising a
            // Mix the player left at zero would be this panel deciding how loud
            // an effect should be, and at mix zero the Mix slider immediately
            // below is both the reason and the cure.
            ampOnHidden[i].setToggleState(! ampOnHidden[i].getToggleState(),
                                          juce::sendNotificationSync);
        };
        addAndMakeVisible(ampStateBtns[i]);
        // The cell NAMES THE STATE IT IS IN, like every other switchbox here:
        // the lit cell is what is currently true. A fixed "OFF" caption meant
        // the one thing you had to click to switch an effect ON was the word
        // OFF (BJ, 2026-08-01) — five cells away, delay's lit OFF means the
        // delay IS off, so the same word was saying two opposite things on one
        // card. With a single cell, its own text has to carry the state.
        // ...and the state it is in includes the MIX. The bypass being open is
        // not the effect running: at mix zero it is switched on, costs its
        // processing and cannot be heard, and this cell said ON. BJ, 26.08.2026:
        // "bei den Effekten sollte Mix/Amt=0 das Element automatisch auf OFF
        // schalten." Refreshed from updateVisibility, which the 30 Hz timer
        // already calls whenever the running picture changes, so pulling a Mix
        // to zero moves this cell with it.
        ampOnHidden[i].onStateChange = [this] { updateVisibility(); };
        ampStateBtns[i].setButtonText("OFF");   // until the attachment reports in
    }

    distDriveRow   = std::make_unique<SliderRow>("Drive",  fmtDb1,  kFxCol);
    distMixRow     = std::make_unique<SliderRow>("Mix",    fmtF2,   kFxCol);
    chorRateRow    = std::make_unique<SliderRow>("Rate",   fmtHz2,  kFxCol);
    chorAmtRow     = std::make_unique<SliderRow>("Amt",    fmtChorusSweep, kFxCol);
    chorMixRow     = std::make_unique<SliderRow>("Mix",    fmtF2,   kFxCol);
    phasRateRow    = std::make_unique<SliderRow>("Rate",   fmtHz2,  kFxCol);
    phasAmtRow     = std::make_unique<SliderRow>("Amt",    fmtPhaserSweep, kFxCol);
    phasFbRow      = std::make_unique<SliderRow>("FB",     fmtF2,   kFxCol);
    phasMixRow     = std::make_unique<SliderRow>("Mix",    fmtF2,   kFxCol);
    tremRateRow    = std::make_unique<SliderRow>("Rate",   fmtHz2,  kFxCol);
    tremAmtRow     = std::make_unique<SliderRow>("Amt",    fmtF2,   kFxCol);
    tremStereoRow  = std::make_unique<SliderRow>("Stereo", fmtF2,   kFxCol);

    for (auto* r : { distDriveRow.get(), distMixRow.get(),
                     chorRateRow.get(), chorAmtRow.get(), chorMixRow.get(),
                     phasRateRow.get(), phasAmtRow.get(), phasFbRow.get(), phasMixRow.get(),
                     tremRateRow.get(), tremAmtRow.get(), tremStereoRow.get() })
    {
        r->setInlineLabel(true);
        addChildComponent(*r);        // one effect at a time — see updateVisibility
    }

    distDriveA  = std::make_unique<SA>(apvts, PID::fxDistDrive,      distDriveRow->getSlider());
    distMixA    = std::make_unique<SA>(apvts, PID::fxDistMix,        distMixRow->getSlider());
    chorRateA   = std::make_unique<SA>(apvts, PID::fxChorusRate,     chorRateRow->getSlider());
    chorAmtA    = std::make_unique<SA>(apvts, PID::fxChorusDepth,    chorAmtRow->getSlider());
    chorMixA    = std::make_unique<SA>(apvts, PID::fxChorusMix,      chorMixRow->getSlider());
    phasRateA   = std::make_unique<SA>(apvts, PID::fxPhaserRate,     phasRateRow->getSlider());
    phasAmtA    = std::make_unique<SA>(apvts, PID::fxPhaserDepth,    phasAmtRow->getSlider());
    phasFbA     = std::make_unique<SA>(apvts, PID::fxPhaserFeedback, phasFbRow->getSlider());
    phasMixA    = std::make_unique<SA>(apvts, PID::fxPhaserMix,      phasMixRow->getSlider());
    tremRateA   = std::make_unique<SA>(apvts, PID::fxTremRate,       tremRateRow->getSlider());
    tremAmtA    = std::make_unique<SA>(apvts, PID::fxTremDepth,      tremAmtRow->getSlider());
    tremStereoA = std::make_unique<SA>(apvts, PID::fxTremStereo,     tremStereoRow->getSlider());

    // The tremolo's shape. The classic tremolos are often square, or nearly, but
    // triangle and sine belong to the family just as much (BJ, 2026-08-01), so
    // all four are on the panel rather than one being picked for the player.
    // Mildest to hardest, the order the family menus use.
    {
        juce::StringArray waveItems;
        for (const auto& e : TremWave::kEntries) waveItems.add(e.label);
        tremWaveHidden.addItemList(waveItems, 1);
    }
    tremWaveHidden.onChange = [this]
    {
        const int w = tremWaveHidden.getSelectedId() - 1;
        for (int i = 0; i < kNumTremWaveBtns; ++i)
            tremWaveBtns[i].setToggleState(i == w, juce::dontSendNotification);
    };
    static const char* kTremWaveTips[kNumTremWaveBtns] = {
        "Sine - the smooth one",
        "Triangle",
        "Soft square - the rounded switching of an optical tremolo",
        "Square - fast-edged, still click-free"
    };
    for (int i = 0; i < kNumTremWaveBtns; ++i)
    {
        tremWaveBtns[i].setButtonText(TremWave::kEntries[i].label);
        tremWaveBtns[i].setTooltip(kTremWaveTips[i]);
        styleSwitchButton(tremWaveBtns[i], kFxCol);
        tremWaveBtns[i].setClickingTogglesState(false);   // the parameter drives it
        tremWaveBtns[i].onClick = [this, i] { tremWaveHidden.setSelectedId(i + 1); };
        addChildComponent(tremWaveBtns[i]);
    }

    struct AmpRowPid { SliderRow* row; const char* pid; };
    for (const auto& e : std::initializer_list<AmpRowPid>{
             { distDriveRow.get(),  PID::fxDistDrive },
             { distMixRow.get(),    PID::fxDistMix },
             { chorRateRow.get(),   PID::fxChorusRate },
             { chorAmtRow.get(),    PID::fxChorusDepth },
             { chorMixRow.get(),    PID::fxChorusMix },
             { phasRateRow.get(),   PID::fxPhaserRate },
             { phasAmtRow.get(),    PID::fxPhaserDepth },
             { phasFbRow.get(),     PID::fxPhaserFeedback },
             { phasMixRow.get(),    PID::fxPhaserMix },
             { tremRateRow.get(),   PID::fxTremRate },
             { tremAmtRow.get(),    PID::fxTremDepth },
             { tremStereoRow.get(), PID::fxTremStereo } })
    {
        const juce::String id(e.pid);
        e.row->onRightClick = [this, id](juce::Point<int> p) { showMidiLearnMenu(processorRef, id, p); };
        e.row->updateValue();
    }

    addAndMakeVisible(wordmark);

    // Attach APVTS AFTER buttons are set up
    reverbTypeA = std::make_unique<CA>(apvts, PID::reverbType, reverbTypeHidden);
    static constexpr const char* kAmpOnPid[kNumAmpFx] = {
        PID::fxDistOn, PID::fxChorusOn, PID::fxPhaserOn, PID::fxTremOn };
    for (int i = 0; i < kNumAmpFx; ++i)
        ampOnA[i] = std::make_unique<BA>(apvts, kAmpOnPid[i], ampOnHidden[i]);
    tremWaveA = std::make_unique<CA>(apvts, PID::fxTremWave, tremWaveHidden);

    // The running lamps' parameters, resolved once (see the members).
    static constexpr const char* kAmpWetPid[kNumAmpFx] = {
        PID::fxDistMix, PID::fxChorusMix, PID::fxPhaserMix, PID::fxTremDepth };
    for (int i = 0; i < kNumAmpFx; ++i)
    {
        runOnPtr_[i]  = apvts.getRawParameterValue(kAmpOnPid[i]);
        runWetPtr_[i] = apvts.getRawParameterValue(kAmpWetPid[i]);
    }
    runDelayMixPtr_  = apvts.getRawParameterValue(PID::delayMix);
    runReverbMixPtr_ = apvts.getRawParameterValue(PID::reverbMix);
    updateVisibility();
    startTimerHz(30); // ghost slider updates + the title row's running lamps
}

void FxPanel::timerCallback()
{
    // The title row's running lamps FIRST, ahead of the audioIdle gate: a player
    // pulling a Mix to zero on a silent synth still has to see the cell go dim,
    // and this is six relaxed loads and an int compare — a repaint only follows
    // when the six-bit picture actually changed. Everything below the gate is
    // the expensive per-frame work and stays behind it
    // (docs/PERFORMANCE_GUIDE.md).
    int mask = 0;
    for (int i = 0; i < kNumFxSel; ++i)
        if (effectRunning(i)) mask |= (1 << i);
    if (mask != fxRunningMask_)
    {
        fxRunningMask_ = mask;
        updateVisibility();
    }

    if (processorRef.audioIdle.load(std::memory_order_relaxed)) return;
    auto& mv = processorRef.modulatedValues;
    delayTimeRow->setGhostValue(mv.delayTime.load(std::memory_order_relaxed));
    delayFbRow->setGhostValue(mv.delayFeedback.load(std::memory_order_relaxed));
    delayMixRow->setGhostValue(mv.delayMix.load(std::memory_order_relaxed));
    reverbMixRow->setGhostValue(mv.reverbMix.load(std::memory_order_relaxed));

    for (auto* r : { delayTimeRow.get(), delayFbRow.get(), delayMixRow.get(), reverbMixRow.get() })
        r->tickGhost();
}

// Opens a family's dropdown anchored to its switchbox cell. The menu is async,
// so the callback must survive the panel being torn down while it is open —
// hence the SafePointer. Dismissing without a pick changes nothing.
void FxPanel::showDelayFamilyMenu(int btnIndex, const int* types, int numTypes)
{
    const int cur = delayTypeHidden.getSelectedId() - 1;
    juce::PopupMenu m;
    for (int i = 0; i < numTypes; ++i)
        m.addItem(types[i] + 1, DelayType::kEntries[types[i]].label, true, types[i] == cur);

    juce::Component::SafePointer<FxPanel> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options()
                        .withTargetComponent(&delayTypeBtns[btnIndex])
                        .withMinimumWidth(juce::jmax(96, delayTypeBtns[btnIndex].getWidth())),
                    [safe](int id)
                    {
                        if (safe != nullptr && id > 0)
                            safe->delayTypeHidden.setSelectedId(id);
                    });
}

void FxPanel::showReverbFamilyMenu(int btnIndex, const int* types, int numTypes)
{
    const int cur = reverbTypeHidden.getSelectedId() - 1;
    juce::PopupMenu m;
    for (int i = 0; i < numTypes; ++i)
        m.addItem(types[i] + 1, ReverbType::kEntries[types[i]].label, true, types[i] == cur);

    juce::Component::SafePointer<FxPanel> safe(this);
    m.showMenuAsync(juce::PopupMenu::Options()
                        .withTargetComponent(&reverbTypeBtns[btnIndex])
                        .withMinimumWidth(juce::jmax(96, reverbTypeBtns[btnIndex].getWidth())),
                    [safe](int id)
                    {
                        if (safe != nullptr && id > 0)
                            safe->reverbTypeHidden.setSelectedId(id);
                    });
}

bool FxPanel::effectRunning(int sel) const
{
    // "Running" is the same question the DSP asks: the bypass is open AND the
    // effect's own wet control is off zero (AmpEffects: each is bypassed at its
    // own default and costs nothing there). For delay and reverb the type IS
    // the switch, so Off is type 0.
    if (sel >= 0 && sel < kNumAmpFx)
    {
        auto* on  = runOnPtr_[sel];
        auto* wet = runWetPtr_[sel];
        return on != nullptr && wet != nullptr
            && on->load(std::memory_order_relaxed)  > 0.5f
            && wet->load(std::memory_order_relaxed) > 0.0001f;
    }
    // ...and the same second half for these two, which had only the first. A
    // type other than Off with the mix at zero is an effect that is switched on,
    // costs its processing, and cannot be heard - and the cell said it was
    // running. BJ, 26.08.2026: "wir haben derzeit als laufend angezeigte Effekte
    // die aber wegen mix=0 faktisch off sind."
    // ...and the mix that decides it is the one the DSP applies, which is the
    // BASE PLUS ITS MODULATION: Dly Mix and Rev Mix are modulation destinations
    // for all three sources, so an envelope on Dly Mix over a base of zero is an
    // audible delay that a base-only reading would call off - with the ghost
    // marker on that very row, on this very card, showing the mix well above
    // zero. The published value is NaN when nothing modulates it; then the base
    // is the whole answer. (The four above cannot be modulated at all.)
    // A mix that is MODULATED counts as in play whatever its value reads at this
    // instant. Not the instantaneous number: over a base of zero the sum sits at
    // zero for a whole negative half-cycle and above it for the whole positive
    // one, so reading the number would flip this lamp twice per cycle - at up to
    // 30 Hz, each flip a full repaint of the card (docs/PERFORMANCE_GUIDE.md).
    // The delay is not switching on and off there; it is being played.
    //
    // ...and only while the audio thread is actually publishing. The modulated
    // values are written below processBlock's deep-idle early return, so after
    // ten seconds of silence they freeze at whatever they last held; trusting
    // them then would leave the cell stuck ON with the Mix pulled to zero, which
    // is the exact complaint. Silent instrument: the base mix is the answer.
    auto liveMix = [this] (const std::atomic<float>* base, const std::atomic<float>& modulated)
    {
        if (base == nullptr) return 0.0f;
        if (! processorRef.audioIdle.load (std::memory_order_relaxed)
            && ! std::isnan (modulated.load (std::memory_order_relaxed)))
            return 1.0f;
        return base->load (std::memory_order_relaxed);
    };
    if (sel == SelDelay)
        return delayTypeHidden.getSelectedId() > 1
            && liveMix (runDelayMixPtr_, processorRef.modulatedValues.delayMix) > 0.0001f;
    if (sel == SelReverb)
        return reverbTypeHidden.getSelectedId() > 1
            && liveMix (runReverbMixPtr_, processorRef.modulatedValues.reverbMix) > 0.0001f;
    return false;
}

void FxPanel::updateVisibility()
{
    // Guard: called by APVTS attachment before all components are created
    if (!reverbMixRow || !tremStereoRow)
        return;

    constexpr float dimAlpha = 0.3f;

    // ── The title row: which cell is being edited, and which effects run ──
    // Two different things on one cell, and they use two different channels so
    // neither can be mistaken for the other: the FILL says what is selected, the
    // TEXT COLOUR says what is running.
    for (int i = 0; i < kNumFxSel; ++i)
    {
        const bool running = effectRunning(i);
        fxSelBtns[i].setToggleState(i == fxSelected_, juce::dontSendNotification);
        fxSelBtns[i].setColour(juce::TextButton::textColourOffId, running ? kFxCol : kDim);
        // BOTH text colours, because the selected cell resolves textColourOnId
        // and never looks at the Off one — writing only the Off colour left the
        // one cell the player is actually looking at unable to report its own
        // state, which is precisely what this row promises.
        const auto ink = switchBoxSelectedTextColour(kFxCol);
        fxSelBtns[i].setColour(juce::TextButton::textColourOnId,
                               running ? ink : ink.withAlpha(0.45f));
    }

    // ── Which effect owns the rows ──
    const bool showDist   = fxSelected_ == SelDist;
    const bool showChorus = fxSelected_ == SelChorus;
    const bool showPhaser = fxSelected_ == SelPhaser;
    const bool showTrem   = fxSelected_ == SelTrem;
    const bool showDelay  = fxSelected_ == SelDelay;
    const bool showReverb = fxSelected_ == SelReverb;

    for (int i = 0; i < kNumAmpFx; ++i)
    {
        ampStateBtns[i].setVisible(fxSelected_ == i);
        // TWO facts on one cell, on two channels, the way the select row above
        // already carries them: the WORD is whether the effect is running -
        // bypass open AND mix off zero, so a mix of zero reads OFF and stops the
        // card claiming an effect nobody can hear (BJ, 26.08.2026). The FILL is
        // the bypass, which is what this cell switches. They come apart in
        // exactly one place, and it is the place that matters: at mix zero the
        // cell can read OFF and still be lit - switched on, not audible - so
        // every click still changes something visible. With the word alone the
        // bypass would flip under an unchanging caption, and since this cell is
        // its only control it could be left inverted with nothing on the card
        // to show it, and saved that way into the preset.
        ampStateBtns[i].setButtonText(effectRunning(i) ? "ON" : "OFF");
        ampStateBtns[i].setToggleState(ampOnHidden[i].getToggleState(),
                                       juce::dontSendNotification);
    }
    for (auto* b : { &delayTypeBtns[0], &delayTypeBtns[1], &delayTypeBtns[2],
                     &delayTypeBtns[3], &delayTypeBtns[4] })
        b->setVisible(showDelay);
    for (auto* b : { &reverbTypeBtns[0], &reverbTypeBtns[1], &reverbTypeBtns[2] })
        b->setVisible(showReverb);
    delayClockBtn.setVisible(showDelay);

    for (auto* r : { distDriveRow.get(), distMixRow.get() })                        r->setVisible(showDist);
    for (auto* r : { chorRateRow.get(), chorAmtRow.get(), chorMixRow.get() })       r->setVisible(showChorus);
    for (auto* r : { phasRateRow.get(), phasAmtRow.get(), phasFbRow.get(),
                     phasMixRow.get() })                                            r->setVisible(showPhaser);
    for (auto* r : { tremRateRow.get(), tremAmtRow.get(), tremStereoRow.get() })    r->setVisible(showTrem);
    for (auto& b : tremWaveBtns)                                                    b.setVisible(showTrem);
    for (auto* r : { delayFbRow.get(), delayDampRow.get(), delayMixRow.get() })     r->setVisible(showDelay);
    for (auto* r : { algoRoomRow.get(), algoDampRow.get(), algoWidthRow.get(),
                     reverbMixRow.get() })                                          r->setVisible(showReverb);
    // Time and Division are exclusive of each other AND of the other five
    // effects; the clock mode decides which of the two, the selection whether
    // either.
    const bool sync = delayClockModeHidden.getSelectedId() == 2;
    delayTimeRow->setVisible(showDelay && ! sync);
    delayDivisionRow->setVisible(showDelay && sync);

    // ── Dimming inside the selected effect ──
    // DIMMED ONLY, never disabled. In this instrument a dimmed control means
    // "does not act right now", never "cannot be touched": live, a parameter is
    // set BEFORE its stage is switched on, and setEnabled(false) forces the
    // opposite order — switch on with the old values, audibly, then correct
    // (BJ, 2026-08-01). The rest of the tree still has to be swept.
    for (int i = 0; i < kNumAmpFx; ++i)
    {
        // The BYPASS, not "is it running": the dimmed set of every one of these
        // four CONTAINS its own Mix, and dimming a control to say "this has no
        // effect right now" while it is the only control that has any effect is
        // the opposite of the signal. It would also split the card - the delay
        // below dims on its type alone, so type=Digital at mix zero would show
        // five bright rows beside four dim ones under the identical condition.
        const bool on = ampOnHidden[i].getToggleState();
        const float a = on ? 1.0f : dimAlpha;
        auto apply = [&](std::initializer_list<SliderRow*> rows)
        {
            for (auto* r : rows) r->setAlpha(a);
        };
        switch (i)
        {
            case SelDist:   apply({ distDriveRow.get(), distMixRow.get() }); break;
            case SelChorus: apply({ chorRateRow.get(), chorAmtRow.get(), chorMixRow.get() }); break;
            case SelPhaser: apply({ phasRateRow.get(), phasAmtRow.get(),
                                    phasFbRow.get(), phasMixRow.get() }); break;
            case SelTrem:   apply({ tremRateRow.get(), tremAmtRow.get(),
                                    tremStereoRow.get() });
                            for (auto& b : tremWaveBtns) b.setAlpha(a);
                            break;
            default: break;
        }
    }

    // Delay: always visible, dimmed when OFF
    bool delayOn = delayTypeHidden.getSelectedId() > 1;
    float delayAlpha = delayOn ? 1.0f : dimAlpha;
    for (auto* r : { delayTimeRow.get(), delayFbRow.get(), delayDampRow.get(),
                     delayMixRow.get(), delayDivisionRow.get() })
        r->setAlpha(delayAlpha);
    delayClockBtn.setAlpha(delayAlpha);

    // Reverb: always visible; dim params based on mode
    bool reverbOn = reverbTypeHidden.getSelectedId() > 1;
    bool algoOn   = ReverbType::isAlgorithmic(reverbTypeHidden.getSelectedId() - 1);
    float reverbAlpha = reverbOn ? 1.0f : dimAlpha;
    // Room/Damp/Width: active only for Algo, dimmed for Convolution and OFF
    float algoParamAlpha = algoOn ? 1.0f : dimAlpha;
    for (auto* r : { algoRoomRow.get(), algoDampRow.get(), algoWidthRow.get() })
        r->setAlpha(algoParamAlpha);
    // Mix: active whenever reverb is on
    reverbMixRow->setAlpha(reverbAlpha);

    // NOTE: no resized() here. This changes visual state (alpha/toggle/label/
    // visibility), none of which affects the geometry. Calling resized()
    // on every type click re-derived headerH from getTopLevelComponent()->
    // getHeight(), which (depending on when the window settled its size) could
    // differ from the initial layout pass by a pixel or two — a visible header
    // jump on the first click. Choosing a different EFFECT does relayout, and
    // calls resized() from the title cell itself for that reason; the rects it
    // produces are the same ones every time, so nothing jumps there either.
    repaint();
}

float FxPanel::fs() const
{
    float topH = (getTopLevelComponent() != nullptr)
                     ? static_cast<float>(getTopLevelComponent()->getHeight()) : 800.0f;
    return juce::jlimit(12.0f, 22.0f, topH * 0.022f);
}

void FxPanel::WordmarkComponent::paint(juce::Graphics& g)
{
    const int panelW = getWidth();
    const int panelH = getHeight();
    if (panelW <= 0 || panelH <= 0)
        return;

    struct LetterColor { char ch; juce::Colour col; };
    LetterColor letters[] = {
        {'U', juce::Colour(0xff667eea)}, {'C', juce::Colour(0xffe91e63)},
        {'D', juce::Colour(0xff7C4DFF)}, {'C', juce::Colour(0xffFF6F00)},
        {'A', juce::Colour(0xff4CAF50)}, {'E', juce::Colour(0xff00BCD4)},
        {' ', {}},
        {'A', juce::Colour(0xff667eea)}, {'I', juce::Colour(0xffe91e63)},
        {' ', {}},
        {'L', juce::Colour(0xff7C4DFF)}, {'A', juce::Colour(0xffFF6F00)},
        {'B', juce::Colour(0xff4CAF50)}
    };

    auto measureBrandWidth = [&letters](float fontSize)
    {
        int total = 0;
        const int tracking = juce::roundToInt(fontSize * 0.15f);
        bool first = true;

        for (auto& lc : letters)
        {
            if (!first)
                total += tracking;

            char text[] = { lc.ch, 0 };
            total += measureTextWidth(juce::String(text), fontSize);
            first = false;
        }

        return total;
    };

    const int usableW = juce::jmax(1, panelW - 8);
    float prefixFs = juce::jlimit(9.0f, 15.0f,
                                  juce::jmin(static_cast<float>(panelW) / 11.0f,
                                             static_cast<float>(panelH) * 0.20f));
    float brandFs = juce::jlimit(12.0f, 24.0f,
                                 juce::jmin(static_cast<float>(panelW) / 11.5f,
                                            static_cast<float>(panelH) * 0.34f));

    int brandW = measureBrandWidth(brandFs);
    if (brandW > usableW)
    {
        brandFs = juce::jmax(10.0f, brandFs * static_cast<float>(usableW) / static_cast<float>(brandW));
        brandW = measureBrandWidth(brandFs);
    }

    const int prefixH = juce::roundToInt(prefixFs * 1.35f);
    const int brandH = juce::roundToInt(brandFs * 1.35f);
    const int lineGap = juce::jmax(1, juce::roundToInt(static_cast<float>(panelH) * 0.06f));
    const int totalH = prefixH + lineGap + brandH;
    int y = juce::jmax(0, (panelH - totalH) / 2);

    g.setFont(juce::FontOptions(prefixFs));
    g.setColour(kDimmer);
    g.drawText(productName() + " by", 0, y, panelW, prefixH, juce::Justification::centred);

    y += prefixH + lineGap;
    const int tracking = juce::roundToInt(brandFs * 0.15f);
    int x = juce::roundToInt((static_cast<float>(panelW) - static_cast<float>(brandW)) * 0.5f);
    x = juce::jlimit(4, juce::jmax(4, panelW - brandW - 4), x);

    g.setFont(juce::FontOptions(brandFs));
    bool first = true;
    for (auto& lc : letters)
    {
        if (!first)
            x += tracking;

        char text[] = { lc.ch, 0 };
        juce::String ch(text);
        int cw = measureTextWidth(ch, brandFs);
        if (lc.ch != ' ')
        {
            g.setColour(lc.col);
            g.drawText(ch, x, y, cw + 1, brandH, juce::Justification::centredLeft);
        }
        x += cw;
        first = false;
    }
}

void FxPanel::paint(juce::Graphics& g)
{
    g.fillAll(kCard);

    // Vertical separator on left edge (between Seq and FX)
    g.setColour(kBorder);
    g.drawVerticalLine(0, 0.0f, static_cast<float>(getHeight()));

    // The framed module card — same recipe as the synth easy-view module blocks
    // (paintEasyBlock): a lighter fill so the card stands out on this kCard
    // panel, plus a border. Drawn BEFORE the child controls so they sit on top.
    // There is ONE now, and its top strip is the switch title row rather than an
    // accent header band: the row names the effect it is showing, so a header
    // that repeated the name would be the same word twice.
    auto paintFxCard = [&g](juce::Rectangle<int> b)
    {
        if (b.isEmpty()) return;
        g.setColour(kSurface.withAlpha(0.62f));
        g.fillRect(b);
        g.setColour(juce::Colour(0xaa05070d));
        g.drawRect(b.expanded(1, 1), 1);
        g.setColour(kBorder.withAlpha(0.82f));
        g.drawRect(b, 1);
    };
    paintFxCard(fxCardBounds);

    // SwitchBox borders — the title row always, and whichever state row the
    // selected effect owns.
    paintSwitchBoxBorder(g, fxSelSwitchBounds);
    if (fxSelected_ == SelDelay)       paintSwitchBoxBorder(g, delayTypeSwitchBounds);
    else if (fxSelected_ == SelReverb) paintSwitchBoxBorder(g, reverbTypeSwitchBounds);
    else                               paintSwitchBoxBorder(g, ampStateBounds);
    // The tremolo is the one amp effect with a second switchbox of its own.
    if (fxSelected_ == SelTrem)        paintSwitchBoxBorder(g, tremWaveSwitchBounds);
}

// Every parameter here is a SliderRow in INLINE-BAR mode — the standard bar the
// sequencer's Gate/Shuffle and the Poly-AT rows already use: one filled bar with
// the name inside it on the left and the value inside it on the right. The FX
// column was the last place still using the older side-cell form (label band |
// track | value cell), which cost three separate widths per row and had to have
// them forced to match across the column so the tracks would line up. A bar has
// no such widths, so the pairs below are plain halves and the alignment problem
// does not exist (BJ, 2026-08-01: overdue, cf. Sequencer).
void FxPanel::resized()
{
    auto area = getLocalBounds().reduced(6, 0);
    float topH = getTopLevelComponent()
                     ? static_cast<float>(getTopLevelComponent()->getHeight()) : 800.0f;
    int headerH = juce::jlimit(14, 20, juce::roundToInt(topH * 0.022f));
    float f = static_cast<float>(headerH);

    int rowH = juce::jmin(juce::roundToInt(static_cast<float>(getHeight()) * 0.14f), 20);
    int gap = 2;
    const int pairGap = 2;

    // ONE framed module card, four rows tall: the switch title row, then the
    // selected effect's state row (OFF at the left, then whatever variants it
    // has), then up to two rows of parameters. Four is what delay and reverb
    // need and nothing needs more, and the card is that tall whichever effect is
    // showing — a card that resized itself per effect would move the title row
    // out from under the cursor that just clicked it.
    const int fxPad = juce::jmax(3, juce::roundToInt(f * 0.3f));
    const int cardBodyH = rowH * 4 + gap * 3;

    auto card = area.removeFromTop(gap + cardBodyH + gap);
    fxCardBounds = card;

    auto cc = card;
    cc.removeFromTop(gap);
    cc.removeFromBottom(gap);
    cc.reduce(fxPad, 0);                 // side pad → value read-outs sit inside the frame

    // ── Row 1: the switch title row ──
    {
        auto selRow = cc.removeFromTop(rowH);
        const int cellW = selRow.getWidth() / kNumFxSel;
        for (int i = 0; i < kNumFxSel; ++i)
        {
            int edges = 0;
            if (i > 0)                edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumFxSel - 1)    edges |= juce::Button::ConnectedOnRight;
            fxSelBtns[i].setConnectedEdges(edges);
            // The last cell takes the remainder so the row ends flush with the
            // card's inner edge whatever the width does not divide by six.
            fxSelBtns[i].setBounds(i == kNumFxSel - 1 ? selRow
                                                      : selRow.removeFromLeft(cellW));
        }
        fxSelSwitchBounds = fxSelBtns[0].getBounds()
            .getUnion(fxSelBtns[kNumFxSel - 1].getBounds());
    }
    cc.removeFromTop(gap);

    const int offW = juce::jmax(28, cc.getWidth() / kNumFxSel);
    const int columnW = juce::jmax(0, (cc.getWidth() - pairGap) / 2);

    // Every effect's rows are laid out, not only the selected one's: a rect a
    // hidden row already owns costs nothing, and computing all of them here
    // keeps this function a pure function of the geometry — updateVisibility
    // decides what is SEEN, resized() only where. Rows 3 and 4 are the same two
    // rects for all six.
    auto stateRow = cc.removeFromTop(rowH);
    cc.removeFromTop(gap);
    auto paramRow1 = cc.removeFromTop(rowH);
    cc.removeFromTop(gap);
    auto paramRow2 = cc.removeFromTop(rowH);

    // ── Delay: the state row is its type switchbox, all five cells ──
    {
        auto sw = stateRow;
        const int cellW = sw.getWidth() / kNumDelayBtns;
        for (int i = 0; i < kNumDelayBtns; ++i)
        {
            int edges = 0;
            if (i > 0)                  edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumDelayBtns - 1)  edges |= juce::Button::ConnectedOnRight;
            delayTypeBtns[i].setConnectedEdges(edges);
            delayTypeBtns[i].setBounds(i == kNumDelayBtns - 1 ? sw : sw.removeFromLeft(cellW));
        }
        delayTypeSwitchBounds = delayTypeBtns[0].getBounds()
            .getUnion(delayTypeBtns[kNumDelayBtns - 1].getBounds());

        // The clock button gets a cell of its own at the head of the LEFT
        // column, and Damp below is indented by the same width so the two bars
        // start on one line. It used to be overlaid on the Time row's reserved
        // label slot; an inline bar has no such slot — the label is drawn inside
        // the bar — so the button needs real space rather than borrowed space.
        const int clockW = juce::jmin(rowH + 6, columnW / 3);

        auto l1 = paramRow1.withWidth(columnW);
        auto r1 = paramRow1.withTrimmedLeft(columnW + pairGap);
        delayClockBtn.setBounds(l1.removeFromLeft(clockW));
        l1.removeFromLeft(pairGap);
        delayTimeRow->setBounds(l1);
        delayDivisionRow->setBounds(l1);
        delayFbRow->setBounds(r1);

        auto l2 = paramRow2.withWidth(columnW).withTrimmedLeft(clockW + pairGap);
        delayDampRow->setBounds(l2);
        delayMixRow->setBounds(paramRow2.withTrimmedLeft(columnW + pairGap));
    }

    // ── Reverb: the state row is its type switchbox ──
    {
        auto sw = stateRow;
        const int cellW = sw.getWidth() / kNumReverbBtns;
        for (int i = 0; i < kNumReverbBtns; ++i)
        {
            int edges = 0;
            if (i > 0)                   edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumReverbBtns - 1)  edges |= juce::Button::ConnectedOnRight;
            reverbTypeBtns[i].setConnectedEdges(edges);
            reverbTypeBtns[i].setBounds(i == kNumReverbBtns - 1 ? sw : sw.removeFromLeft(cellW));
        }
        reverbTypeSwitchBounds = reverbTypeBtns[0].getBounds()
            .getUnion(reverbTypeBtns[kNumReverbBtns - 1].getBounds());

        algoRoomRow->setBounds(paramRow1.withWidth(columnW));
        algoDampRow->setBounds(paramRow1.withTrimmedLeft(columnW + pairGap));
        algoWidthRow->setBounds(paramRow2.withWidth(columnW));
        reverbMixRow->setBounds(paramRow2.withTrimmedLeft(columnW + pairGap));
    }

    // ── The four amp effects: OFF at the left of the state row, and the rest of
    // that row is theirs. Distortion is finished in it; the other three take one
    // parameter row, and only the phaser takes two.
    {
        auto rest = stateRow;
        // OFF is one title cell wide, so the two switch columns line up down
        // the card. It has no neighbour to connect to — the parameters that
        // follow it are slider rows, not switch cells.
        ampStateBounds = rest.removeFromLeft(offW);
        for (int i = 0; i < kNumAmpFx; ++i)
        {
            ampStateBtns[i].setConnectedEdges(0);
            ampStateBtns[i].setBounds(ampStateBounds);
        }
        rest.removeFromLeft(pairGap);

        // Two bars in what is left of the state row, then the wider pair rows
        // below. The four differ only in how many bars they need.
        const int restColW = juce::jmax(0, (rest.getWidth() - pairGap) / 2);
        auto restL = rest.withWidth(restColW);
        auto restR = rest.withTrimmedLeft(restColW + pairGap);
        auto row1L = paramRow1.withWidth(columnW);
        auto row1R = paramRow1.withTrimmedLeft(columnW + pairGap);

        distDriveRow->setBounds(restL);        // Distortion is finished in one row
        distMixRow->setBounds(restR);

        chorRateRow->setBounds(restL);
        chorAmtRow->setBounds(restR);
        chorMixRow->setBounds(row1L);

        phasRateRow->setBounds(restL);         // the only one that needs both
        phasAmtRow->setBounds(restR);
        phasFbRow->setBounds(row1L);
        phasMixRow->setBounds(row1R);

        tremRateRow->setBounds(restL);
        tremAmtRow->setBounds(restR);
        tremStereoRow->setBounds(row1L);
        {
            auto sw = row1R;                       // the shape sits beside Stereo
            const int cellW = sw.getWidth() / kNumTremWaveBtns;
            for (int i = 0; i < kNumTremWaveBtns; ++i)
            {
                int edges = 0;
                if (i > 0)                     edges |= juce::Button::ConnectedOnLeft;
                if (i < kNumTremWaveBtns - 1)  edges |= juce::Button::ConnectedOnRight;
                tremWaveBtns[i].setConnectedEdges(edges);
                tremWaveBtns[i].setBounds(i == kNumTremWaveBtns - 1 ? sw
                                                                   : sw.removeFromLeft(cellW));
            }
            tremWaveSwitchBounds = tremWaveBtns[0].getBounds()
                .getUnion(tremWaveBtns[kNumTremWaveBtns - 1].getBounds());
        }
    }

    area.removeFromTop(gap);
    wordmark.setBounds(area);
    wordmark.setVisible(area.getHeight() >= 24);
}
