// The Phrase mode's controls in the sequencer panel
// (docs/PHRASE_SEQ_CONCEPT.md §4).
//
//   A. Which controls each mode shows: the Phrase rows only in Phrase, Key
//      (the shared scale boxes) in Phrase and Gen, Oct (the shared octave
//      switch) in Step and Phrase.
//   B. The layout at the panel's size range (792x192, 864x208, 1300x280):
//      every visible control lies inside the panel, no two overlap, every
//      switch segment and action button is as wide as its label needs, every
//      inline bar as wide as its label and widest value need, every Phrase
//      combo as wide as its longest item needs.
//   C. The bindings: a segment click writes its parameter and lights its
//      segment only; a parameter changed elsewhere moves the segments; each
//      combo and bar writes its own parameter.
//   D. The chord slots follow the scale and Harm. They show the key's Roman
//      numerals and are relabelled at once when the scale changes on the
//      message thread. There the attachment updates its combo, and the combo
//      calls onChange, before the APVTS stores the new value (JUCE calls a
//      parameter's listeners in reverse order of registration), so a relabel
//      that read the parameter would show the previous scale. The slots and
//      Rate are enabled only for Harm = Chords in a scale with chords; a
//      pentatonic's degrees 6 and 7 read "(6)" "(7)" and are disabled.
//   E. Layer's Chord is not offered for Stab; a Chord layer set before Stab
//      is shown dimmed.
//   F. The motif actions: Dice, Undo and From Step call the processor's
//      actions and set Undo at once; Keep with nothing heard yet changes
//      nothing. Undo is re-read at every tick above the idle gate: an Init in
//      deep idle ends the Undo, and the next tick disables the button.
//   G. Phrase in deep idle: ticks lay nothing out and repaint nothing.
//
// Build: the processor-level recipe of tools/test_seq_engine_idle.cpp
// (libakroasys_SharedCode.a + libT5ynthData.a). Exits non-zero on failure.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"
#include "gui/SequencerPanel.h"
#include "gui/T5ynthLookAndFeel.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0;

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
// prepareToPlay idles the block after ~10 s of silence; a margin past that.
constexpr int kIdleBlocks = static_cast<int>(10.0 * kRate / kBlock) + 20;

// The Phrase switches' radio groups (SequencerPanel's constructor).
constexpr int kPartGroup = 2020, kHarmGroup = 2021, kRateGroup = 2022,
              kExtGroup = 2023, kLenGroup = 2024, kBarsGroup = 2025;

void check(const char* what, bool ok)
{
    std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (! ok) ++failures;
}

void setPlain(T5ynthProcessor& p, const char* id, float v)
{
    auto* par = p.getValueTreeState().getParameter(id);
    par->setValueNotifyingHost(par->convertTo0to1(v));
}

float plainF(T5ynthProcessor& p, const char* id)
{
    return p.getValueTreeState().getRawParameterValue(id)->load();
}

int plain(T5ynthProcessor& p, const char* id)
{
    return static_cast<int>(plainF(p, id));
}

void blocks(T5ynthProcessor& p, int n)
{
    juce::AudioBuffer<float> buf(2, kBlock);
    for (int i = 0; i < n; ++i)
    {
        buf.clear();
        juce::MidiBuffer midi;
        p.processBlock(buf, midi);
    }
}

/** One tick of the panel's 10 Hz timer (see test_seq_panel_step_switch.cpp
 *  for the 450 ms). */
void tick()
{
    juce::Thread::sleep(450);
    juce::Timer::callPendingTimersSynchronously();
}

bool reachIdle(T5ynthProcessor& p)
{
    blocks(p, kIdleBlocks);
    if (! p.audioIdle.load())
    {
        std::printf("    did not reach deep idle\n");
        return false;
    }
    return true;
}

/** Records what the panel invalidates, its own repaints and its children's.
 *  Owned by the panel (setCachedComponentImage). */
struct RepaintProbe final : juce::CachedComponentImage
{
    bool any = false;
    void paint(juce::Graphics&) override {}
    bool invalidateAll() override { any = true; return true; }
    bool invalidate(const juce::Rectangle<int>&) override { any = true; return true; }
    void releaseResources() override {}
};

struct CountingPanel final : SequencerPanel
{
    using SequencerPanel::SequencerPanel;
    int layouts = 0;
    void resized() override { ++layouts; SequencerPanel::resized(); }
};

// ── Finding the controls (the panel's members are private) ─────────────────

std::vector<juce::TextButton*> segments(const SequencerPanel& panel, int radioGroupId)
{
    std::vector<juce::TextButton*> v;
    for (auto* c : panel.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(c))
            if (b->getRadioGroupId() == radioGroupId)
                v.push_back(b);
    return v;
}

juce::TextButton* buttonLabelled(const SequencerPanel& panel, const char* text)
{
    for (auto* c : panel.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(c))
            if (b->getButtonText() == text)
                return b;
    return nullptr;
}

juce::Label* labelTexted(const SequencerPanel& panel, const char* text)
{
    for (auto* c : panel.getChildren())
        if (auto* l = dynamic_cast<juce::Label*>(c))
            if (l->getText() == text)
                return l;
    return nullptr;
}

template <typename Pred>
std::vector<juce::ComboBox*> combos(const SequencerPanel& panel, Pred pred)
{
    std::vector<juce::ComboBox*> v;
    for (auto* c : panel.getChildren())
        if (auto* b = dynamic_cast<juce::ComboBox*>(c))
            if (pred(*b))
                v.push_back(b);
    return v;
}

/** The inline bars labelled `label` whose slider holds `value`: the bar
 *  bound to a parameter set to a value no other bar holds. */
std::vector<SliderRow*> barsHolding(const SequencerPanel& panel, const char* label, double value)
{
    std::vector<SliderRow*> v;
    for (auto* c : panel.getChildren())
        if (auto* r = dynamic_cast<SliderRow*>(c))
            if (r->getLabel().getText() == label && std::abs(r->getSlider().getValue() - value) < 1e-6)
                v.push_back(r);
    return v;
}

// Values no other bar of the panel holds, so the Phrase bars are found by
// their binding (Gen has bars labelled Pulses and Evolve too).
struct BarSpec { const char* label; const char* pid; double value; const char* widest; };
const BarSpec kBars[] = {
    { "Slide",  PID::phrSlide,     0.37, "100%" },
    { "Pulses", PID::phrPulses,    23.0, "32"   },
    { "Rot",    PID::phrRotation,  17.0, "31"   },
    { "Var",    PID::phrVariation, 0.63, "100%" },
    { "Evolve", PID::phrEvolve,    0.29, "100%" },
};

void setDistinctBarValues(T5ynthProcessor& p)
{
    for (const auto& b : kBars)
        setPlain(p, b.pid, static_cast<float>(b.value));
}

struct PhraseUi
{
    std::vector<juce::TextButton*> part, harm, rate, ext, len, bars;
    juce::TextButton *dice = nullptr, *keep = nullptr, *undo = nullptr, *fromStep = nullptr;
    std::vector<juce::ComboBox*> chordSlots;
    juce::ComboBox *layer = nullptr, *form = nullptr;
    std::vector<SliderRow*> sliders;   // kBars order
    std::vector<juce::Label*> tags;

    /** Every Phrase-only control. */
    std::vector<juce::Component*> all() const
    {
        std::vector<juce::Component*> v;
        for (const auto* sw : { &part, &harm, &rate, &ext, &len, &bars })
            for (auto* b : *sw) v.push_back(b);
        for (auto* b : { dice, keep, undo, fromStep }) v.push_back(b);
        for (auto* c : chordSlots) v.push_back(c);
        v.push_back(layer);
        v.push_back(form);
        for (auto* s : sliders) v.push_back(s);
        for (auto* t : tags) v.push_back(t);
        return v;
    }
};

/** Finds the Phrase controls; the bars must hold kBars' values. */
bool findPhraseUi(const SequencerPanel& panel, PhraseUi& ui)
{
    ui.part = segments(panel, kPartGroup);
    ui.harm = segments(panel, kHarmGroup);
    ui.rate = segments(panel, kRateGroup);
    ui.ext  = segments(panel, kExtGroup);
    ui.len  = segments(panel, kLenGroup);
    ui.bars = segments(panel, kBarsGroup);
    ui.dice     = buttonLabelled(panel, "Dice");
    ui.keep     = buttonLabelled(panel, "Keep");
    ui.undo     = buttonLabelled(panel, "Undo");
    ui.fromStep = buttonLabelled(panel, "From Step");
    ui.chordSlots = combos(panel, [](juce::ComboBox& b)
        { return b.getNumItems() == PhrChord::kCount && b.getItemText(0) == "-"; });
    auto layers = combos(panel, [](juce::ComboBox& b)
        { return b.getNumItems() == PhrLayer::kCount && b.getItemText(PhrLayer::Answer) == "Answer"; });
    auto forms = combos(panel, [](juce::ComboBox& b)
        { return b.getNumItems() == PhrForm::kCount && b.getItemText(PhrForm::AAAB) == "AAAB"; });
    ui.layer = layers.size() == 1 ? layers[0] : nullptr;
    ui.form  = forms.size() == 1 ? forms[0] : nullptr;
    bool barsFound = true;
    ui.sliders.clear();
    for (const auto& b : kBars)
    {
        auto found = barsHolding(panel, b.label, b.value);
        barsFound = barsFound && found.size() == 1;
        ui.sliders.push_back(found.size() == 1 ? found[0] : nullptr);
    }
    ui.tags.clear();
    bool tagsFound = true;
    for (const char* t : { "Part", "Key", "Oct", "Layer", "Harm", "Rate", "Ext", "Len", "Bars", "Form" })
    {
        auto* l = labelTexted(panel, t);
        tagsFound = tagsFound && l != nullptr;
        ui.tags.push_back(l);
    }
    const bool ok = ui.part.size() == PhrPart::kCount && ui.harm.size() == PhrHarm::kCount
        && ui.rate.size() == PhrChordRate::kCount && ui.ext.size() == 2
        && ui.len.size() == PhrLen::kCount && ui.bars.size() == PhrBars::kCount
        && ui.dice && ui.keep && ui.undo && ui.fromStep
        && ui.chordSlots.size() == 4 && ui.layer && ui.form && barsFound && tagsFound;
    if (! ok)
        std::printf("    segments %zu %zu %zu %zu %zu %zu, chord slots %zu, layer %d, form %d, "
                    "bars %d, tags %d\n",
                    ui.part.size(), ui.harm.size(), ui.rate.size(), ui.ext.size(),
                    ui.len.size(), ui.bars.size(), ui.chordSlots.size(),
                    ui.layer != nullptr, ui.form != nullptr, barsFound, tagsFound);
    return ok;
}

juce::ComboBox* scaleTypeBox(const SequencerPanel& panel)
{
    auto v = combos(panel, [](juce::ComboBox& b)
        { return b.getNumItems() == ScaleType::kCount && b.getItemText(0) == "Chromatic"; });
    return v.size() == 1 ? v[0] : nullptr;
}

juce::ComboBox* scaleRootBox(const SequencerPanel& panel)
{
    auto v = combos(panel, [](juce::ComboBox& b)
        { return b.getNumItems() == 12 && b.getItemText(0) == "C"; });
    return v.size() == 1 ? v[0] : nullptr;
}

std::vector<juce::TextButton*> octaveButtons(const SequencerPanel& panel)
{
    std::vector<juce::TextButton*> v;
    for (const auto& e : SeqOctave::kEntries)
        if (auto* b = buttonLabelled(panel, e.label))
            v.push_back(b);
    return v;
}

/** The toggled segments' indices. */
std::vector<int> lit(const std::vector<juce::TextButton*>& segs)
{
    std::vector<int> v;
    for (size_t i = 0; i < segs.size(); ++i)
        if (segs[i]->getToggleState())
            v.push_back(static_cast<int>(i));
    return v;
}

bool onlyLit(const std::vector<juce::TextButton*>& segs, int index)
{
    const auto l = lit(segs);
    return l.size() == 1 && l[0] == index;
}

juce::String utf8(const char* s) { return juce::String::fromUTF8(s); }

// ── A ──────────────────────────────────────────────────────────────────────

void modesShowTheirControls()
{
    std::printf("A. Each mode shows its controls\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    setDistinctBarValues(proc);
    blocks(proc, 4);
    SequencerPanel panel(proc);
    panel.setSize(864, 208);
    PhraseUi ui;
    check("the Phrase controls are found", findPhraseUi(panel, ui));
    auto* type = scaleTypeBox(panel);
    auto* root = scaleRootBox(panel);
    const auto oct = octaveButtons(panel);
    check("the shared Key boxes and the octave switch are found",
          type && root && oct.size() == SeqOctave::kCount);
    if (! type || ! root || oct.size() != SeqOctave::kCount || ui.all().size() < 40) return;

    for (int engine : { SeqEngine::Step, SeqEngine::Phrase, SeqEngine::Gen, SeqEngine::Phrase })
    {
        setPlain(proc, PID::seqEngine, static_cast<float>(engine));
        blocks(proc, 2);
        tick();
        const bool phrase = engine == SeqEngine::Phrase;
        const char* name = engine == SeqEngine::Step ? "Step" : phrase ? "Phrase" : "Gen";
        int shown = 0;
        const auto all = ui.all();
        for (auto* c : all) shown += c->isVisible() ? 1 : 0;
        std::printf("    %s: %d of %zu Phrase controls shown\n", name, shown, all.size());
        check(phrase ? "Phrase shows every Phrase control" : "the mode shows no Phrase control",
              shown == (phrase ? static_cast<int>(all.size()) : 0));
        check("Key shows in Phrase and Gen only",
              type->isVisible() == (engine != SeqEngine::Step)
              && root->isVisible() == (engine != SeqEngine::Step));
        bool octShown = true;
        for (auto* b : oct) octShown = octShown && b->isVisible();
        bool octHidden = true;
        for (auto* b : oct) octHidden = octHidden && ! b->isVisible();
        check("the octave switch shows in Step and Phrase only",
              engine == SeqEngine::Gen ? octHidden : octShown);
    }
}

// ── B ──────────────────────────────────────────────────────────────────────

int buttonNeedW(juce::TextButton& b)
{
    const float fs = b.getLookAndFeel().getTextButtonFont(b, b.getHeight()).getHeight();
    return measureTextWidth(b.getButtonText(), fs) + 2 * juce::roundToInt(fs * 0.6f) + 2;
}

/** A combo item fits the text area positionComboBoxText gives its label,
 *  inside the Label's default 5 px border a side. */
bool comboFits(juce::ComboBox& box, const juce::String& text)
{
    juce::Label probe;
    box.getLookAndFeel().positionComboBoxText(box, probe);
    const auto border = probe.getBorderSize();
    return measureTextWidth(text, probe.getFont().getHeight())
           <= probe.getWidth() - border.getLeftAndRight();
}

void layoutAtSize(int w, int h)
{
    std::printf("B. Phrase layout at %dx%d\n", w, h);
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    setDistinctBarValues(proc);
    // Minor's labels for the slot-width check (the longest of the
    // heptatonic Roman numerals: ♭VII, ii°).
    setPlain(proc, PID::scaleType, static_cast<float>(ScaleType::Min));
    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
    blocks(proc, 4);
    SequencerPanel panel(proc);
    panel.setSize(w, h);
    PhraseUi ui;
    if (! findPhraseUi(panel, ui)) { check("the Phrase controls are found", false); return; }

    std::vector<juce::Component*> shown;
    for (auto* c : panel.getChildren())
        if (c->isVisible())
            shown.push_back(c);
    bool inside = true, overlapFree = true;
    for (size_t i = 0; i < shown.size(); ++i)
    {
        const auto bi = shown[i]->getBounds();
        if (bi.isEmpty() || ! panel.getLocalBounds().contains(bi))
        {
            inside = false;
            std::printf("    outside or empty: %s %s\n", typeid(*shown[i]).name(),
                        bi.toString().toRawUTF8());
        }
        for (size_t j = i + 1; j < shown.size(); ++j)
            if (bi.intersects(shown[j]->getBounds()))
            {
                overlapFree = false;
                std::printf("    overlap: %s %s / %s %s\n",
                            typeid(*shown[i]).name(), bi.toString().toRawUTF8(),
                            typeid(*shown[j]).name(), shown[j]->getBounds().toString().toRawUTF8());
            }
    }
    check("every visible control lies inside the panel", inside);
    check("no two visible controls overlap", overlapFree);

    bool buttonsFit = true;
    std::vector<juce::TextButton*> buttons { ui.dice, ui.keep, ui.undo, ui.fromStep };
    for (const auto* sw : { &ui.part, &ui.harm, &ui.rate, &ui.ext, &ui.len, &ui.bars })
        buttons.insert(buttons.end(), sw->begin(), sw->end());
    for (auto* b : buttons)
        if (b->getWidth() < buttonNeedW(*b))
        {
            buttonsFit = false;
            std::printf("    \"%s\" %d px < %d\n", b->getButtonText().toRawUTF8(),
                        b->getWidth(), buttonNeedW(*b));
        }
    check("every segment and action button fits its label", buttonsFit);

    bool barsFit = true;
    for (size_t i = 0; i < std::size(kBars); ++i)
    {
        auto* r = ui.sliders[i];
        const float fs = juce::jlimit(9.0f, 13.0f, static_cast<float>(r->getHeight()) * 0.58f);
        const int need = measureTextWidth(kBars[i].label, fs) + measureTextWidth(kBars[i].widest, fs) + 14;
        if (r->getWidth() < need)
        {
            barsFit = false;
            std::printf("    %s bar %d px < %d\n", kBars[i].label, r->getWidth(), need);
        }
    }
    check("every bar fits its label and widest value", barsFit);

    bool combosFit = true;
    auto allItemsFit = [&combosFit](juce::ComboBox& box, const char* what)
    {
        for (int i = 0; i < box.getNumItems(); ++i)
            if (! comboFits(box, box.getItemText(i)))
            {
                combosFit = false;
                std::printf("    %s item \"%s\" does not fit %d px\n", what,
                            box.getItemText(i).toRawUTF8(), box.getWidth());
            }
    };
    for (auto* box : ui.chordSlots) allItemsFit(*box, "chord slot");
    allItemsFit(*ui.layer, "Layer");
    allItemsFit(*ui.form, "Form");
    if (auto* root = scaleRootBox(panel)) allItemsFit(*root, "Key root");
    check("every Phrase combo fits its longest item", combosFit);
}

// ── C ──────────────────────────────────────────────────────────────────────

void bindings()
{
    std::printf("C. Segments, combos and bars write their parameters\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    setDistinctBarValues(proc);
    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
    blocks(proc, 4);
    SequencerPanel panel(proc);
    panel.setSize(864, 208);
    PhraseUi ui;
    if (! findPhraseUi(panel, ui)) { check("the Phrase controls are found", false); return; }

    struct Sw { const char* name; std::vector<juce::TextButton*>* segs; const char* pid; };
    Sw sws[] = { { "Part", &ui.part, PID::phrPart }, { "Harm", &ui.harm, PID::phrHarm },
                 { "Rate", &ui.rate, PID::phrChordRate }, { "Ext", &ui.ext, PID::phrExt7 },
                 { "Len", &ui.len, PID::phrLen }, { "Bars", &ui.bars, PID::phrBars } };
    for (auto& sw : sws)
    {
        auto& segs = *sw.segs;
        check((juce::String(sw.name) + ": the parameter's segment is lit at start").toRawUTF8(),
              onlyLit(segs, plain(proc, sw.pid)));
        // Click every segment, last first, so each click moves the value.
        bool clicksWrite = true;
        for (int i = static_cast<int>(segs.size()) - 1; i >= 0; --i)
        {
            segs[static_cast<size_t>(i)]->onClick();
            clicksWrite = clicksWrite && plain(proc, sw.pid) == i && onlyLit(segs, i);
        }
        check((juce::String(sw.name) + ": a click writes its value and lights its segment").toRawUTF8(),
              clicksWrite);
        const int last = static_cast<int>(segs.size()) - 1;
        setPlain(proc, sw.pid, static_cast<float>(last));
        check((juce::String(sw.name) + ": a value set elsewhere lights its segment").toRawUTF8(),
              onlyLit(segs, last));
    }

    ui.layer->setSelectedId(PhrLayer::Answer + 1, juce::sendNotificationSync);
    check("Layer writes phrLayer", plain(proc, PID::phrLayer) == PhrLayer::Answer);
    ui.form->setSelectedId(PhrForm::ABAB + 1, juce::sendNotificationSync);
    check("Form writes phrForm", plain(proc, PID::phrForm) == PhrForm::ABAB);
    const char* chordPids[] = { PID::phrChord1, PID::phrChord2, PID::phrChord3, PID::phrChord4 };
    bool slotsWrite = true;
    for (int i = 0; i < 4; ++i)
    {
        ui.chordSlots[static_cast<size_t>(i)]->setSelectedId(i + 3, juce::sendNotificationSync);
        slotsWrite = slotsWrite && plain(proc, chordPids[i]) == i + 2;
    }
    check("each chord slot writes its own phrChord parameter", slotsWrite);

    const double moved[] = { 0.81, 9.0, 5.0, 0.12, 0.74 };
    bool barsWrite = true;
    for (size_t i = 0; i < std::size(kBars); ++i)
    {
        ui.sliders[i]->getSlider().setValue(moved[i], juce::sendNotificationSync);
        const bool ok = std::abs(plainF(proc, kBars[i].pid) - moved[i]) < 1e-3;
        if (! ok) std::printf("    %s: %.3f\n", kBars[i].label, plainF(proc, kBars[i].pid));
        barsWrite = barsWrite && ok;
    }
    check("each bar writes its own parameter", barsWrite);
}

// ── D ──────────────────────────────────────────────────────────────────────

void chordSlots()
{
    std::printf("D. The chord slots follow the scale and Harm\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    setDistinctBarValues(proc);
    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
    setPlain(proc, PID::scaleType, static_cast<float>(ScaleType::Maj));
    setPlain(proc, PID::phrHarm, static_cast<float>(PhrHarm::Pedal));
    setPlain(proc, PID::phrChord1, 6.0f);   // degree 6
    setPlain(proc, PID::phrChord2, 1.0f);
    blocks(proc, 4);
    SequencerPanel panel(proc);
    panel.setSize(864, 208);
    PhraseUi ui;
    if (! findPhraseUi(panel, ui)) { check("the Phrase controls are found", false); return; }
    auto* slot1 = ui.chordSlots[0];
    auto* slot2 = ui.chordSlots[1];
    auto allEnabled = [](const std::vector<juce::TextButton*>& segs, bool on)
    {
        for (auto* b : segs) if (b->isEnabled() != on) return false;
        return true;
    };
    auto slotsEnabled = [&ui](bool on)
    {
        for (auto* b : ui.chordSlots) if (b->isEnabled() != on) return false;
        return true;
    };

    std::printf("    Major: slot 1 \"%s\", slot 2 \"%s\"\n",
                slot1->getText().toRawUTF8(), slot2->getText().toRawUTF8());
    check("Major: slot 1 (degree 6) reads vi, slot 2 reads I",
          slot1->getText() == "vi" && slot2->getText() == "I");
    check("Pedal: the slots and Rate are disabled", slotsEnabled(false) && allEnabled(ui.rate, false));

    setPlain(proc, PID::phrHarm, static_cast<float>(PhrHarm::Chords));
    check("Chords: the slots and Rate are enabled at once",
          slotsEnabled(true) && allEnabled(ui.rate, true));

    setPlain(proc, PID::scaleType, static_cast<float>(ScaleType::Min));
    std::printf("    Minor: slot 1 \"%s\", slot 2 \"%s\"\n",
                slot1->getText().toRawUTF8(), slot2->getText().toRawUTF8());
    check("Minor, set on the message thread: slot 1 reads \xe2\x99\xad" "VI at once",
          slot1->getText() == utf8("\xe2\x99\xad" "VI") && slot2->getText() == "i");
    check("and its items are Minor's: ii\xc2\xb0 and \xe2\x99\xad" "VII",
          slot1->getItemText(2) == utf8("ii\xc2\xb0") && slot1->getItemText(7) == utf8("\xe2\x99\xad" "VII"));
    check("the relabel leaves the slot parameters as they were",
          plain(proc, PID::phrChord1) == 6 && plain(proc, PID::phrChord2) == 1);
    check("a slot still reports its selection", slot1->getSelectedId() == 7);

    setPlain(proc, PID::scaleType, static_cast<float>(ScaleType::Pent));
    bool pentItems = slot1->getItemText(6) == "(6)" && slot1->getItemText(7) == "(7)"
                     && ! slot1->isItemEnabled(7) && ! slot1->isItemEnabled(8);
    for (int id = 2; id <= 6; ++id)
        pentItems = pentItems && slot1->isItemEnabled(id);
    check("Penta: degrees 6 and 7 read (6) (7) and are disabled, 1..5 enabled", pentItems);
    check("Penta: slot 1 (degree 6) reads (6), the slots stay enabled",
          slot1->getText() == "(6)" && slotsEnabled(true));

    setPlain(proc, PID::scaleType, static_cast<float>(ScaleType::Off));
    check("Chromatic: no chords, the slots and Rate are disabled",
          slotsEnabled(false) && allEnabled(ui.rate, false));
    check("Chromatic: the items read 1..7", slot1->getItemText(1) == "1" && slot1->getItemText(7) == "7"
                                            && slot1->getText() == "6");

    setPlain(proc, PID::scaleType, static_cast<float>(ScaleType::Min));
    check("back to Minor: enabled again, slot 1 reads \xe2\x99\xad" "VI",
          slotsEnabled(true) && slot1->getText() == utf8("\xe2\x99\xad" "VI"));
    slot2->setSelectedId(4 + 1, juce::sendNotificationSync);
    check("picking iv in slot 2 writes degree 4", plain(proc, PID::phrChord2) == 4);
}

// ── E ──────────────────────────────────────────────────────────────────────

void layerForStab()
{
    std::printf("E. Layer's Chord and Stab\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    setDistinctBarValues(proc);
    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
    setPlain(proc, PID::phrPart, static_cast<float>(PhrPart::Bass));
    setPlain(proc, PID::phrLayer, static_cast<float>(PhrLayer::Chord));
    blocks(proc, 4);
    SequencerPanel panel(proc);
    panel.setSize(864, 208);
    PhraseUi ui;
    if (! findPhraseUi(panel, ui)) { check("the Phrase controls are found", false); return; }
    const int chordId = PhrLayer::Chord + 1;
    check("Bass: Chord is offered, the box is not dimmed",
          ui.layer->isItemEnabled(chordId) && ui.layer->getAlpha() == 1.0f);
    ui.part[PhrPart::Stab]->onClick();
    check("Stab (clicked): Chord is not offered, the set Chord layer is dimmed",
          ! ui.layer->isItemEnabled(chordId) && ui.layer->getAlpha() < 1.0f);
    ui.layer->setSelectedId(PhrLayer::Octave + 1, juce::sendNotificationSync);
    check("Stab with Octave: not dimmed", ui.layer->getAlpha() == 1.0f);
    setPlain(proc, PID::phrLayer, static_cast<float>(PhrLayer::Chord));
    check("Stab with Chord set elsewhere: dimmed at once", ui.layer->getAlpha() < 1.0f);
    setPlain(proc, PID::phrPart, static_cast<float>(PhrPart::Lead));
    check("Lead set elsewhere: Chord offered and not dimmed at once",
          ui.layer->isItemEnabled(chordId) && ui.layer->getAlpha() == 1.0f);
}

// ── F ──────────────────────────────────────────────────────────────────────

void motifActions()
{
    std::printf("F. Dice, Keep, Undo, From Step\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    setDistinctBarValues(proc);
    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
    blocks(proc, 4);
    SequencerPanel panel(proc);
    panel.setSize(864, 208);
    PhraseUi ui;
    if (! findPhraseUi(panel, ui)) { check("the Phrase controls are found", false); return; }
    tick();
    check("nothing to undo at start: Undo disabled", ! proc.canPhraseUndo() && ! ui.undo->isEnabled());

    ui.keep->onClick();
    check("Keep with nothing heard yet: still nothing to undo",
          ! proc.canPhraseUndo() && ! ui.undo->isEnabled());

    const auto seedBefore = proc.getPhraseSequencer().getStoredMotif().seed;
    ui.dice->onClick();
    const auto seedDiced = proc.getPhraseSequencer().getStoredMotif().seed;
    check("Dice stores a new motif", seedDiced != seedBefore);
    check("and enables Undo at once", proc.canPhraseUndo() && ui.undo->isEnabled());
    ui.undo->onClick();
    check("Undo brings the motif before the Dice back",
          proc.getPhraseSequencer().getStoredMotif().seed == seedBefore);
    check("and stays enabled (a second Undo redoes)", ui.undo->isEnabled());

    // Init in deep idle ends the Undo; nothing on the panel is clicked.
    if (! reachIdle(proc)) { check("deep idle before Init", false); return; }
    tick();
    proc.resetParametersToDefaults();
    setDistinctBarValues(proc);
    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
    check("Init ended the Undo", ! proc.canPhraseUndo());
    check("Undo still enabled before the next tick", ui.undo->isEnabled());
    tick();
    check("still deep idle", proc.audioIdle.load());
    check("the next idle tick disables Undo", ! ui.undo->isEnabled());

    // From Step: five onsets in a 16-step bar.
    {
        const juce::ScopedLock sl(proc.getCallbackLock());
        auto& st = proc.getStepSequencer();
        st.setNumSteps(16);
        for (int i = 0; i < 16; ++i) st.setStepEnabled(i, false);
        for (int i : { 0, 3, 6, 10, 12 }) { st.setStepNote(i, 57); st.setStepEnabled(i, true); }
    }
    setPlain(proc, PID::seqSteps, 16.0f);
    const int pulsesBefore = plain(proc, PID::phrPulses);
    ui.fromStep->onClick();
    const auto& m = proc.getPhraseSequencer().getStoredMotif();
    std::printf("    From Step: pulses %d (param %d, was %d), len %d\n", m.pulses,
                plain(proc, PID::phrPulses), pulsesBefore, m.len);
    check("From Step analyses the steps and writes Pulses",
          plain(proc, PID::phrPulses) == m.pulses && m.pulses != pulsesBefore);
    check("and enables Undo at once", proc.canPhraseUndo() && ui.undo->isEnabled());
    check("the Pulses bar shows the written value",
          static_cast<int>(ui.sliders[1]->getSlider().getValue()) == m.pulses);
    check("the Len segments show the written length",
          onlyLit(ui.len, plain(proc, PID::phrLen)));
}

// ── G ──────────────────────────────────────────────────────────────────────

void phraseIdle()
{
    std::printf("G. Phrase in deep idle\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
    blocks(proc, 4);
    CountingPanel panel(proc);
    auto* probe = new RepaintProbe();
    panel.setCachedComponentImage(probe);
    panel.setVisible(true);   // a hidden component passes no repaint on
    panel.setSize(864, 208);
    tick();
    if (! reachIdle(proc)) { check("deep idle", false); return; }
    tick();
    panel.layouts = 0;
    probe->any = false;
    for (int i = 0; i < 4; ++i) tick();
    std::printf("    4 idle ticks: %d layouts, repaint %d, audioIdle %d\n", panel.layouts,
                probe->any ? 1 : 0, proc.audioIdle.load() ? 1 : 0);
    check("idle ticks lay nothing out", panel.layouts == 0);
    check("idle ticks repaint nothing", ! probe->any);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    T5ynthLookAndFeel lnf;
    juce::LookAndFeel::setDefaultLookAndFeel(&lnf);

    modesShowTheirControls();
    layoutAtSize(792, 192);
    layoutAtSize(864, 208);
    layoutAtSize(1300, 280);
    bindings();
    chordSlots();
    layerForStab();
    motifActions();
    phraseIdle();

    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAIL\n", failures);
    return failures == 0 ? 0 : 1;
}
