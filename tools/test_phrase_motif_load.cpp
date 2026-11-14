// The phrase motif across a load.
//
// The phrase sequencer's motif is not a parameter: it is the stored asset
// Dice / Keep / From Step write (PhraseSequencer::setStoredMotif). Every load
// that replaces the patch replaces it too, so a patch never plays the motif
// the previous one diced: a JSON preset restores the motif it carries, and a
// preset without a phrase block (saved before the phrase sequencer existed)
// restores the empty record, which the seed and the parameters fill, as a
// reset does; a host state likewise. The load also ends the Undo level: Undo
// after a load would bring back the previous patch's motif.
//
// The legacy XML preset (an APVTS dump, the .t5p format of 2026-03/04, read by
// PresetFormat::loadFromFile into importLegacyXmlPreset) predates the phrase
// sequencer: its load sets the phrase parameters to their defaults and kept
// the motif diced before it, with its Undo. Found 2026-11-14 in the review of
// the phrase sequencer's load paths.
//
// Each case dices first, so the motif before the load is not the empty record
// and Undo is available; the dice is asserted, so a case whose dice did not
// take fails instead of passing vacuously.
//
// Build: the processor-level recipe of tools/test_preset_missing_key_defaults.cpp
// (libakroasys_SharedCode.a + libT5ynthData.a). Exits non-zero on failure.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"
#include "presets/PresetFormat.h"

#include <cstdio>

namespace
{
int failures = 0;

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;

void check(const char* what, bool ok)
{
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (! ok) ++failures;
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

bool sameMotif(const PhraseModel::Motif& a, const PhraseModel::Motif& b)
{
    if (a.len != b.len || a.stepsPerBar != b.stepsPerBar || a.pulses != b.pulses
        || a.rotation != b.rotation || a.part != b.part || a.seed != b.seed)
        return false;
    for (int i = 0; i < a.numSteps(); ++i)
    {
        const auto& x = a.slots[static_cast<size_t>(i)];
        const auto& y = b.slots[static_cast<size_t>(i)];
        if (x.fn != y.fn || x.member != y.member || x.octave != y.octave
            || x.accent != y.accent || x.slide != y.slide)
            return false;
    }
    return true;
}

enum class Load { LegacyXml, JsonWithout, JsonWith, StateWithout, StateWith };

/** Dices, then loads. `with` loads carry a motif diced and saved before the
 *  one now stored. Returns whether the stored motif after the load is the
 *  load's (the empty record for a load without one) and Undo is gone. */
bool loadReplacesMotif(Load load)
{
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);

    // What a load that carries a motif carries: the motif of an earlier dice.
    proc.phraseDice();
    const auto saved = proc.getPhraseSequencer().getStoredMotif();
    const juce::String json = proc.exportJsonPreset();
    juce::MemoryBlock state;
    proc.getStateInformation(state);

    proc.phraseDice();
    const auto before = proc.getPhraseSequencer().getStoredMotif();
    if (before.len == 0 || sameMotif(before, saved) || ! proc.canPhraseUndo())
    {
        std::printf("    the dice before the load did not take\n");
        return false;
    }

    bool loaded = false;
    if (load == Load::LegacyXml)
    {
        // An APVTS dump of 2026-03/04: no phrase parameters, no seq_engine.
        static const char* const kNewer[] = {
            PID::seqEngine, PID::phrPart, PID::phrHarm, PID::phrChord1, PID::phrChord2,
            PID::phrChord3, PID::phrChord4, PID::phrChordRate, PID::phrExt7, PID::phrLen,
            PID::phrPulses, PID::phrRotation, PID::phrBars, PID::phrForm, PID::phrVariation,
            PID::phrEvolve, PID::phrLayer, PID::phrSlide
        };
        auto tree = proc.getValueTreeState().copyState();
        for (const char* id : kNewer)
            tree.removeChild(tree.getChildWithProperty("id", id), nullptr);
        const juce::TemporaryFile file(".t5p");
        loaded = file.getFile().replaceWithText(tree.createXml()->toString())
              && PresetFormat::loadFromFile(file.getFile(), proc).success;
    }
    else if (load == Load::JsonWithout || load == Load::JsonWith)
    {
        auto parsed = juce::JSON::parse(json);
        if (load == Load::JsonWithout)
            if (auto* root = parsed.getDynamicObject())
                root->removeProperty("phraseSeq");
        loaded = proc.importJsonPreset(juce::JSON::toString(parsed));
    }
    else
    {
        std::unique_ptr<juce::XmlElement> xml(
            juce::AudioProcessor::getXmlFromBinary(state.getData(),
                                                   static_cast<int>(state.getSize())));
        if (xml != nullptr)
        {
            if (load == Load::StateWithout)
                xml->removeAttribute("phraseMotif");
            juce::MemoryBlock edited;
            juce::AudioProcessor::copyXmlToBinary(*xml, edited);
            proc.setStateInformation(edited.getData(), static_cast<int>(edited.getSize()));
            loaded = true;
        }
    }
    if (! loaded)
    {
        std::printf("    the load failed\n");
        return false;
    }
    blocks(proc, 3);

    const auto after = proc.getPhraseSequencer().getStoredMotif();
    const bool carries = load == Load::JsonWith || load == Load::StateWith;
    const bool motifOk = carries ? sameMotif(after, saved) : after.len == 0;
    const bool undoGone = ! proc.canPhraseUndo();
    std::printf("    motif after the load: len %d seed %u (%s), undo %s\n",
                after.len, static_cast<unsigned>(after.seed),
                sameMotif(after, before) ? "the dice before the load"
                    : sameMotif(after, saved) ? "the saved one" : "other",
                undoGone ? "gone" : "still there");
    return motifOk && undoGone;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf("A load replaces the phrase motif and ends Undo\n");
    check("legacy XML preset: the empty record",
          loadReplacesMotif(Load::LegacyXml));
    check("JSON preset without a phrase block: the empty record",
          loadReplacesMotif(Load::JsonWithout));
    check("JSON preset with a motif: that motif",
          loadReplacesMotif(Load::JsonWith));
    check("host state without a motif: the empty record",
          loadReplacesMotif(Load::StateWithout));
    check("host state with a motif: that motif",
          loadReplacesMotif(Load::StateWith));

    std::printf("failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
