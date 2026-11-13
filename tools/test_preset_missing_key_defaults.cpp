// A preset key that is NOT in the file must load the parameter's LAYOUT
// DEFAULT — never the void juce::var's 0, and never the value the previously
// loaded patch left behind.
//
// getProperty() returns a void var for an absent key and a void var converts
// to 0.0f, so `setParam(p, PID::x, static_cast<float>(o->getProperty("k")))`
// silently writes a hard 0 whenever the key is missing. Found 2026-08-05 on
// effects.limiterThreshold: default -3 dB, absent -> 0 dB, and since
// 6c10bee3 that parameter drives the static output gain, so a preset lacking
// the key played 3 dB quiet. It was never one site — the same shape covered
// the cutoff (absent -> 20 Hz), the amp envelope's Amt (absent -> silence),
// the sequencer's BPM and step count (absent -> 0 and 0), the octave and
// frame-count choices (absent -> the FIRST entry, not the default one), and
// ~40 more.
//
// No file this app writes is affected: the exporter emits every key, and all
// 147 .t5p on the maintainer machine load bit-identically before and after the
// fix. This is about hand-edited, foreign, truncated and older-format presets,
// and about what PRESET_FORMAT.md §3.1.1 now promises a third-party writer.
//
// This test constructs the REAL processor and drives the real
// exportJsonPreset/importJsonPreset pair. Per case it asserts BOTH directions:
//
//   1. STRIPPED: export, delete the key, put the parameter somewhere that is
//      neither 0 nor its default, import -> the parameter reads its default.
//      A "kept the previous patch" bug and a "wrote 0" bug both fail this,
//      and they fail it differently, so the printout says which.
//   2. COMPLETE: the same export with the key left IN round-trips to the
//      value that was saved. This is the half that proves the guard did not
//      turn every load into a reset.
//
// The export the two halves work from is taken with EVERY parameter under test
// moved OFF its default first. Without that, half 2 is vacuous: the file would
// carry the defaults, so "restored what was saved" and "reset to the default"
// would be the same assertion, and an importer that read nothing out of the
// file at all would pass. (It did, and an adversarial pass caught it: with all
// five *FromJson helpers neutered to always default, the earlier version of
// this test still reported 0 failures.)
//
// Build: same recipe as the other tools/*.cpp, plus libT5ynthData.a (a
// processor-level tool needs BinaryData for the plate IRs prepareToPlay
// reaches for):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/test_preset_missing_key_defaults.cpp \
//     build_clean/T5ynth_artefacts/Release/libakroasys_SharedCode.a \
//     build_clean/libT5ynthData.a -framework ... -o /tmp/test_missing_keys
// Exits non-zero on failure.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"
#include "sequencer/PhraseModel.h"

#include <cmath>
#include <cstdio>
#include <iterator>
#include <vector>

static int failures = 0;

// ── JSON path resolution ───────────────────────────────────────────────────
// "effects"                 -> root.effects
// "modulation/envs[0]"      -> root.modulation.envs[0]
// "generativeSeq/strand0"   -> root.generativeSeq.strand0
// ""                        -> root itself
static juce::DynamicObject* resolvePath (juce::var& root, const juce::String& path)
{
    auto* obj = root.getDynamicObject();
    if (path.isEmpty()) return obj;

    juce::StringArray parts;
    parts.addTokens (path, "/", "");
    for (const auto& rawPart : parts)
    {
        if (obj == nullptr) return nullptr;
        juce::String name = rawPart;
        int index = -1;
        if (name.endsWithChar (']'))
        {
            index = name.fromFirstOccurrenceOf ("[", false, false).upToFirstOccurrenceOf ("]", false, false).getIntValue();
            name  = name.upToFirstOccurrenceOf ("[", false, false);
        }
        auto child = obj->getProperty (name);
        if (index >= 0)
        {
            auto* arr = child.getArray();
            if (arr == nullptr || index >= arr->size()) return nullptr;
            obj = (*arr)[index].getDynamicObject();
        }
        else
        {
            obj = child.getDynamicObject();
        }
    }
    return obj;
}

// ── Parameter helpers ──────────────────────────────────────────────────────
static float normOf (T5ynthProcessor& p, const char* pid)
{
    auto* par = p.getValueTreeState().getParameter (pid);
    return par != nullptr ? par->getValue() : -1.0f;
}
static float defaultNormOf (T5ynthProcessor& p, const char* pid)
{
    auto* par = p.getValueTreeState().getParameter (pid);
    return par != nullptr ? par->getDefaultValue() : -1.0f;
}
static float plainOf (T5ynthProcessor& p, const char* pid)
{
    auto* par = p.getValueTreeState().getParameter (pid);
    return par != nullptr ? par->convertFrom0to1 (par->getValue()) : 0.0f;
}
static void setNorm (T5ynthProcessor& p, const char* pid, float norm)
{
    if (auto* par = p.getValueTreeState().getParameter (pid))
        par->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, norm));
}
// Park a parameter at its default before a load that must move it AWAY from
// there — so "the importer wrote the saved value" cannot be confused with
// "the importer left the parameter alone".
static void setParamToLayoutDefaultFor (T5ynthProcessor& p, const char* pid)
{
    if (auto* par = p.getValueTreeState().getParameter (pid))
        par->setValueNotifyingHost (par->getDefaultValue());
}
// A normalised position the parameter actually LANDS on and that is not its
// default. Found by trying, not by arithmetic, for two reasons the first
// version of this file got wrong: a choice or int parameter SNAPS, so an
// arbitrary offset can round straight back to the default (osc_octave's
// default is the middle of five and 0.61 rounds to it); and a bool keeps
// whatever float it is handed, so 0.61 would be recorded as "saved" while a
// reload canonicalises it to 1.0. Returns the CANONICAL normalisation of the
// value that landed — what a save/load round trip will actually produce.
static float decoyNormFor (T5ynthProcessor& p, const char* pid)
{
    auto* par = p.getValueTreeState().getParameter (pid);
    if (par == nullptr) return 0.5f;

    const float restore   = par->getValue();
    const float dfltPlain = par->convertFrom0to1 (par->getDefaultValue());
    float found = -1.0f;
    for (float candidate : { 0.61f, 0.23f, 0.87f, 0.41f, 1.0f, 0.0f, 0.75f, 0.12f })
    {
        par->setValueNotifyingHost (candidate);
        const float landedPlain = par->convertFrom0to1 (par->getValue());
        if (std::abs (landedPlain - dfltPlain) > 1.0e-4f * juce::jmax (1.0f, std::abs (dfltPlain)))
        {
            found = par->convertTo0to1 (landedPlain);
            break;
        }
    }
    par->setValueNotifyingHost (restore);
    return found >= 0.0f ? found : par->getDefaultValue();
}

// ── The cases ──────────────────────────────────────────────────────────────
// Every key the importer used to read WITHOUT a hasProperty guard, i.e. every
// key whose absence used to mean 0. Grouped as they appear in the file.
struct Case { const char* path; const char* key; const char* pid; };

static const Case kCases[] = {
    // synth
    { "synth", "alpha",         PID::genAlpha },
    { "synth", "magnitude",     PID::genMagnitude },
    { "synth", "noise",         PID::genNoise },
    { "synth", "resynth",       PID::resynthAmount },
    { "synth", "duration",      PID::genDuration },
    { "synth", "startPosition", PID::genStart },
    { "synth", "steps",         PID::infSteps },
    { "synth", "cfg",           PID::genCfg },
    { "synth", "seed",          PID::genSeed },
    { "synth", "repromptStance",   PID::repromptStance },
    { "synth", "repromptCoupling", PID::repromptCoupling },
    { "synth", "resynthSource",    PID::resynthSource },

    // engine
    { "engine", "mode",         PID::engineMode },
    { "engine", "loopMode",     PID::loopMode },
    { "engine", "crossfadeMs",  PID::crossfadeMs },
    { "engine", PID::normalize, PID::normalize },
    { "engine", "loopOptimize", PID::loopOptimize },

    // modulation — amp envelope (index 0) and one mod envelope (index 1)
    { "modulation/envs[0]", "attackMs",  PID::ampAttack },
    { "modulation/envs[0]", "decayMs",   PID::ampDecay },
    { "modulation/envs[0]", "sustain",   PID::ampSustain },
    { "modulation/envs[0]", "releaseMs", PID::ampRelease },
    { "modulation/envs[0]", "amount",    PID::ampAmount },
    { "modulation/envs[0]", "loop",      PID::ampLoop },
    { "modulation/envs[1]", "decayMs",   PID::modEnv[0].decay },
    { "modulation/envs[1]", "sustain",   PID::modEnv[0].sustain },
    { "modulation/envs[1]", "releaseMs", PID::modEnv[0].release },
    { "modulation/envs[1]", "amount",    PID::modEnv[0].amount },

    // modulation — LFOs (LFO 2's default wave is triangle, not the first entry)
    { "modulation/lfos[0]", "rate",     PID::lfo1Rate },
    { "modulation/lfos[0]", "depth",    PID::lfo1Depth },
    { "modulation/lfos[0]", "waveform", PID::lfo1Wave },
    { "modulation/lfos[0]", "target",   PID::lfo1Target },
    { "modulation/lfos[0]", "mode",     PID::lfo1Mode },
    { "modulation/lfos[1]", "rate",     PID::lfo2Rate },
    { "modulation/lfos[1]", "waveform", PID::lfo2Wave },
    { "modulation/lfos[2]", "rate",     PID::lfo3Rate },

    // drift
    { "driftLfos[0]", "rate",     PID::drift1Rate },
    { "driftLfos[0]", "depth",    PID::drift1Depth },
    { "driftLfos[0]", "waveform", PID::drift1Wave },
    { "driftLfos[0]", "target",   PID::drift1Target },
    { "",             "driftEnabled",   PID::driftEnabled },
    { "",             "driftCrossfade", PID::driftCrossfade },
    { "",             "regenMode",      PID::driftRegen },

    // wavetable + noise
    { "wavetable", "scan",        PID::oscScan },
    { "wavetable", "octaveShift", PID::oscOctave },
    { "wavetable", "noiseLevel",  PID::noiseLevel },
    { "wavetable", "noiseType",   PID::noiseType },
    { "wavetable", "frames",      PID::wtFrames },
    { "wavetable", "smooth",      PID::wtSmooth },

    // granular
    { "freeze", "texture", PID::freezeTexture },
    { "freeze", "stereo",  PID::freezeStereo },

    // effects
    { "effects", "delayType",        PID::delayType },
    { "effects", "delayTimeMs",      PID::delayTime },
    { "effects", "delayFeedback",    PID::delayFeedback },
    { "effects", "delayMix",         PID::delayMix },
    { "effects", "delayDamp",        PID::delayDamp },
    { "effects", "reverbType",       PID::reverbType },
    { "effects", "reverbMix",        PID::reverbMix },
    { "effects", "algoRoom",         PID::algoRoom },
    { "effects", "algoDamping",      PID::algoDamping },
    { "effects", "algoWidth",        PID::algoWidth },
    { "effects", "limiterThreshold", PID::limiterThresh },   // the site this was found through
    { "effects", "limiterRelease",   PID::limiterRelease },

    // filter — `type` is here as well as in the enabled/type sub-test below,
    // so the two close the gap from both sides: this half proves a named type
    // survives a round trip, that half proves an absent `enabled` does not
    // overrule it.
    { "filter", "type",     PID::filterType },
    { "filter", "slope",    PID::filterSlope },
    { "filter", "cutoff",   PID::filterCutoff },
    { "filter", "resonance", PID::filterResonance },
    { "filter", "mix",      PID::filterMix },
    { "filter", "kbdTrack", PID::filterKbdTrack },

    // sequencer
    { "sequencer", "bpm",         PID::seqBpm },
    { "sequencer", "stepCount",   PID::seqSteps },
    { "sequencer", "octaveShift", PID::seqOctave },
    { "sequencer", "division",    PID::seqDivision },
    { "sequencer", "glideTime",   PID::seqGlideTime },
    { "sequencer", "gate",        PID::seqGate },
    { "sequencer", "shuffle",     PID::seqShuffle },
    { "sequencer", "scaleRoot",   PID::scaleRoot },
    { "sequencer", "scaleType",   PID::scaleType },
    // Stripped, the importer falls back to the legacy generativeSeq.enabled,
    // which the export writes as engine == Gen. The decoy saved here is not
    // Gen, so that fallback lands on Step, which is also the default; the
    // fallback's own cases are in sub-test 4.
    { "sequencer", "engine",      PID::seqEngine },

    // arpeggiator
    { "arpeggiator", "pattern",     PID::arpMode },
    { "arpeggiator", "rate",        PID::arpRate },
    { "arpeggiator", "octaveRange", PID::arpOctaves },

    // generative sequencer
    { "generativeSeq", "steps",       PID::genSteps },
    { "generativeSeq", "pulses",      PID::genPulses },
    { "generativeSeq", "rotation",    PID::genRotation },
    { "generativeSeq", "mutation",    PID::genMutation },
    { "generativeSeq", "range",       PID::genRange },
    { "generativeSeq", "fixSteps",    PID::genFixSteps },
    { "generativeSeq", "fixPulses",   PID::genFixPulses },
    { "generativeSeq", "fixRotation", PID::genFixRotation },
    { "generativeSeq", "fixMutation", PID::genFixMutation },

    { "generativeSeq/pitchField", "mode",     PID::genFieldMode },
    { "generativeSeq/pitchField", "rate",     PID::genFieldRate },
    { "generativeSeq/pitchField", "centerPc", PID::genFieldCenterPc },
    { "generativeSeq/pitchField", "pivot",    PID::genFieldPivot },

    { "generativeSeq/strand0", "role",      PID::genRole },
    { "generativeSeq/strand0", "octave",    PID::genOctave },
    { "generativeSeq/strand0", "divMult",   PID::genDivMult },
    { "generativeSeq/strand0", "dominance", PID::genDominance },

    { "generativeSeq/strand2", "enabled",     PID::gen2Enable },
    { "generativeSeq/strand2", "role",        PID::gen2Role },
    { "generativeSeq/strand2", "octave",      PID::gen2Octave },
    { "generativeSeq/strand2", "divMult",     PID::gen2DivMult },
    { "generativeSeq/strand2", "dominance",   PID::gen2Dominance },
    { "generativeSeq/strand2", "steps",       PID::gen2Steps },
    { "generativeSeq/strand2", "pulses",      PID::gen2Pulses },
    { "generativeSeq/strand2", "rotation",    PID::gen2Rotation },
    { "generativeSeq/strand2", "mutation",    PID::gen2Mutation },
    { "generativeSeq/strand2", "fixSteps",    PID::gen2FixSteps },
    { "generativeSeq/strand2", "fixPulses",   PID::gen2FixPulses },
    { "generativeSeq/strand2", "fixRotation", PID::gen2FixRotation },
    { "generativeSeq/strand2", "fixMutation", PID::gen2FixMutation },

    // phrase sequencer (the motif is not a parameter: sub-test 5)
    { "phraseSeq", "part",      PID::phrPart },
    { "phraseSeq", "harmony",   PID::phrHarm },
    { "phraseSeq", "chord1",    PID::phrChord1 },
    { "phraseSeq", "chord2",    PID::phrChord2 },
    { "phraseSeq", "chord3",    PID::phrChord3 },
    { "phraseSeq", "chord4",    PID::phrChord4 },
    { "phraseSeq", "chordRate", PID::phrChordRate },
    { "phraseSeq", "ext7",      PID::phrExt7 },
    { "phraseSeq", "len",       PID::phrLen },
    { "phraseSeq", "pulses",    PID::phrPulses },
    { "phraseSeq", "rotation",  PID::phrRotation },
    { "phraseSeq", "bars",      PID::phrBars },
    { "phraseSeq", "form",      PID::phrForm },
    { "phraseSeq", "variation", PID::phrVariation },
    { "phraseSeq", "evolve",    PID::phrEvolve },
    { "phraseSeq", "layer",     PID::phrLayer },
    { "phraseSeq", "slide",     PID::phrSlide },
};

static const char* const kPhraseParams[] = {
    PID::phrPart, PID::phrHarm, PID::phrChord1, PID::phrChord2, PID::phrChord3, PID::phrChord4,
    PID::phrChordRate, PID::phrExt7, PID::phrLen, PID::phrPulses, PID::phrRotation, PID::phrBars,
    PID::phrForm, PID::phrVariation, PID::phrEvolve, PID::phrLayer, PID::phrSlide
};

static void setPlain (T5ynthProcessor& p, const char* pid, float plain)
{
    if (auto* par = p.getValueTreeState().getParameter (pid))
        par->setValueNotifyingHost (par->convertTo0to1 (plain));
}

static bool sameBody (const PhraseModel::MotifBody& a, const PhraseModel::MotifBody& b)
{
    if (a.len != b.len || a.stepsPerBar != b.stepsPerBar || a.pulses != b.pulses
        || a.rotation != b.rotation || a.part != b.part || a.seed != b.seed)
        return false;
    for (int i = 0; i < a.numSteps(); ++i)
    {
        const auto& x = a.slots[(size_t) i];
        const auto& y = b.slots[(size_t) i];
        if (x.fn != y.fn || x.member != y.member || x.octave != y.octave
            || x.accent != y.accent || x.slide != y.slide)
            return false;
    }
    return true;
}

static bool sameMotif (const PhraseModel::Motif& a, const PhraseModel::Motif& b)
{
    if (! sameBody (a, b) || a.origin != b.origin || a.leadInKept != b.leadInKept)
        return false;
    return ! a.leadInKept || (sameBody (a.leadIn, b.leadIn) && a.leadInPhrase == b.leadInPhrase);
}

// A motif no default produces: two bars of eight, every field off its
// default, every slot field used, and a kept lead-in of its own shape.
static PhraseModel::Motif probeMotif()
{
    using namespace PhraseModel;
    Motif m;
    m.len = 2; m.stepsPerBar = 8; m.pulses = 6; m.rotation = 3;
    m.part = Part::Lead; m.seed = 0xdeadbeefu; m.origin = 5;
    const Fn fns[] = { Fn::ChordTone, Fn::Rest, Fn::Passing, Fn::ApproachChromatic,
                       Fn::NeighbourDown, Fn::Octave, Fn::Repeat, Fn::ApproachUp };
    for (int i = 0; i < m.numSteps(); ++i)
    {
        auto& sl = m.slots[(size_t) i];
        sl.fn = fns[i % 8];
        sl.member = (std::uint8_t) (i % 4);
        sl.octave = (std::int8_t) (i % 3 - 1);
        sl.accent = i % 3 == 0;
        sl.slide  = i % 5 == 1;
    }
    m.leadInKept = true;
    m.leadIn.len = 1; m.leadIn.stepsPerBar = 16; m.leadIn.pulses = 5; m.leadIn.rotation = 2;
    m.leadIn.part = Part::Riff; m.leadIn.seed = 77u;
    for (int i = 0; i < m.leadIn.numSteps(); ++i)
    {
        auto& sl = m.leadIn.slots[(size_t) i];
        sl.fn = i % 2 == 0 ? Fn::ChordTone : Fn::NeighbourUp;
        sl.member = (std::uint8_t) ((i + 1) % 4);
        sl.octave = (std::int8_t) ((i + 2) % 3 - 1);
        sl.accent = i % 4 == 2;
    }
    m.leadInPhrase = 5;
    return m;
}

// The motif in the file format, written from PRESET_FORMAT.md rather than by
// the exporter, so the reader is held to the format and not to its own twin.
static const char* const kFnKeys[] = { "rest", "chord_tone", "repeat", "octave", "passing",
                                       "neighbour_up", "neighbour_down", "approach_up",
                                       "approach_down", "approach_chromatic" };
static_assert (std::size (kFnKeys) == (size_t) PhraseModel::Fn::Count, "kFnKeys out of sync");

static void writeBody (const PhraseModel::MotifBody& b, juce::DynamicObject& o)
{
    o.setProperty ("len", b.len);
    o.setProperty ("stepsPerBar", b.stepsPerBar);
    o.setProperty ("pulses", b.pulses);
    o.setProperty ("rotation", b.rotation);
    o.setProperty ("part", PhrPart::kEntries[(int) b.part].key);
    o.setProperty ("seed", (juce::int64) b.seed);
    juce::Array<juce::var> slots;
    for (int i = 0; i < b.numSteps(); ++i)
    {
        const auto& sl = b.slots[(size_t) i];
        auto* so = new juce::DynamicObject();
        so->setProperty ("fn", kFnKeys[(int) sl.fn]);
        so->setProperty ("member", (int) sl.member);
        so->setProperty ("octave", (int) sl.octave);
        so->setProperty ("accent", sl.accent);
        so->setProperty ("slide", sl.slide);
        slots.add (juce::var (so));
    }
    o.setProperty ("slots", slots);
}

static juce::var motifJson (const PhraseModel::Motif& m)
{
    auto* o = new juce::DynamicObject();
    juce::var out (o);
    writeBody (m, *o);
    o->setProperty ("origin", m.origin);
    if (m.leadInKept)
    {
        auto* lo = new juce::DynamicObject();
        writeBody (m.leadIn, *lo);
        lo->setProperty ("phrase", m.leadInPhrase);
        o->setProperty ("leadIn", juce::var (lo));
    }
    return out;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    T5ynthProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    // Move every parameter under test OFF its default, so the export below
    // carries values the layout does not already supply. Read the position
    // BACK after setting: choice and int parameters snap, so the decoy that
    // survives is not always the decoy that was asked for.
    std::vector<float> saved (std::size (kCases), 0.0f);
    for (size_t i = 0; i < std::size (kCases); ++i)
    {
        setNorm (proc, kCases[i].pid, decoyNormFor (proc, kCases[i].pid));
        saved[i] = normOf (proc, kCases[i].pid);
    }
    // Cheap self-check on the premise: if a case's decoy did not actually move
    // the parameter, its COMPLETE half is vacuous for that case and says so.
    for (size_t i = 0; i < std::size (kCases); ++i)
        if (std::abs (saved[i] - defaultNormOf (proc, kCases[i].pid)) < 1.0e-4f)
        {
            std::printf ("  FAIL: %s/%s (%s) could not be moved off its default — "
                         "the complete-preset half would prove nothing\n",
                         kCases[i].path, kCases[i].key, kCases[i].pid);
            ++failures;
        }

    // The filter shape the export is about to record, for the enabled/type
    // sub-test at the end — which has to assert the NAMED type, not just
    // "not Off".
    const int savedFilterType = (int) std::lround (plainOf (proc, PID::filterType));

    // A full, well-formed export of THAT state. Every case's key is present in
    // it; the per-case work below removes exactly one.
    const juce::String fullJson = proc.exportJsonPreset();

    int stripChecked = 0, completeChecked = 0, discriminating = 0;

    for (size_t ci = 0; ci < std::size (kCases); ++ci)
    {
        const auto& c = kCases[ci];
        // ── 1. the key is REMOVED ──
        auto parsed = juce::JSON::parse (fullJson);
        auto* target = resolvePath (parsed, c.path);
        if (target == nullptr)
        {
            std::printf ("  FAIL: %s/%s — path does not resolve in the export\n", c.path, c.key);
            ++failures;
            continue;
        }
        if (! target->hasProperty (c.key))
        {
            std::printf ("  FAIL: %s/%s — key is not in the export at all (stale case?)\n", c.path, c.key);
            ++failures;
            continue;
        }
        target->removeProperty (c.key);

        // Park the parameter somewhere that is neither its default nor 0, so
        // "inherited the previous patch" and "wrote the void var's 0" are both
        // visible and distinguishable from a correct load.
        const float decoy = decoyNormFor (proc, c.pid);
        setNorm (proc, c.pid, decoy);
        const float parked = normOf (proc, c.pid);

        proc.importJsonPreset (juce::JSON::toString (parsed, true));

        const float got = normOf (proc, c.pid);
        const float want = defaultNormOf (proc, c.pid);
        ++stripChecked;
        if (std::abs (got - parked) < 1.0e-5f && std::abs (want - parked) > 1.0e-5f)
        {
            std::printf ("  FAIL: %s/%s (%s) kept the PREVIOUS patch (%.4f), want default %.4f\n",
                         c.path, c.key, c.pid, got, want);
            ++failures;
        }
        else if (std::abs (got - want) > 1.0e-4f)
        {
            std::printf ("  FAIL: %s/%s (%s) loaded %.4f (plain %.3f), want default %.4f\n",
                         c.path, c.key, c.pid, got, plainOf (proc, c.pid), want);
            ++failures;
        }
        // Can this case tell the DEFECT from a correct load at all? The defect
        // wrote a PLAIN 0, so the question is where a plain 0 normalises to,
        // not whether the normalised default is non-zero. They are different
        // questions on a range that does not start at 0: gen_octave spans
        // -2..2, so plain 0 IS the default and sits at normalised 0.5, and
        // counting it by the normalised test overstated the coverage.
        if (auto* par = proc.getValueTreeState().getParameter (c.pid))
            if (std::abs (par->convertTo0to1 (0.0f) - par->getDefaultValue()) > 1.0e-6f)
                ++discriminating;

        // ── 2. the key is PRESENT — the load must restore what was SAVED, ──
        // which is the off-default value the export was taken at, not the
        // layout default. This is what fails if a guard defaults unconditionally.
        setParamToLayoutDefaultFor (proc, c.pid);
        proc.importJsonPreset (fullJson);
        ++completeChecked;
        if (std::abs (normOf (proc, c.pid) - saved[ci]) > 1.0e-4f)
        {
            std::printf ("  FAIL: %s/%s (%s) complete preset restored %.4f, saved %.4f\n",
                         c.path, c.key, c.pid, normOf (proc, c.pid), saved[ci]);
            ++failures;
        }
    }

    // ── 3. filter enabled/type: two JSON keys, ONE parameter ──
    // `enabled` alone must not force the type Off when the file never said so,
    // and a file with neither key must land on the layout's own type.
    {
        auto parsed = juce::JSON::parse (fullJson);
        if (auto* filt = resolvePath (parsed, "filter"))
        {
            filt->removeProperty ("enabled");
            setNorm (proc, PID::filterType, decoyNormFor (proc, PID::filterType));
            proc.importJsonPreset (juce::JSON::toString (parsed, true));
            const int got = (int) std::lround (plainOf (proc, PID::filterType));
            // Assert the NAMED type, not merely "not Off": an importer that
            // ignored `type` and always wrote the layout's Lowpass would
            // satisfy "not Off" and replace every preset's filter shape.
            if (got != savedFilterType)
            {
                std::printf ("  FAIL: filter/type with an ABSENT `enabled` loaded %d, "
                             "the file named %d\n", got, savedFilterType);
                ++failures;
            }
        }

        auto parsed2 = juce::JSON::parse (fullJson);
        if (auto* filt = resolvePath (parsed2, "filter"))
        {
            filt->removeProperty ("enabled");
            filt->removeProperty ("type");
            setNorm (proc, PID::filterType, decoyNormFor (proc, PID::filterType));
            proc.importJsonPreset (juce::JSON::toString (parsed2, true));
            if (std::abs (normOf (proc, PID::filterType) - defaultNormOf (proc, PID::filterType)) > 1.0e-4f)
            {
                std::printf ("  FAIL: filter with NEITHER key did not land on the layout default\n");
                ++failures;
            }
        }

        // `enabled: false` must still win — that is the whole point of the key.
        auto parsed3 = juce::JSON::parse (fullJson);
        if (auto* filt = resolvePath (parsed3, "filter"))
        {
            filt->setProperty ("enabled", false);
            proc.importJsonPreset (juce::JSON::toString (parsed3, true));
            if ((int) std::lround (plainOf (proc, PID::filterType)) != FilterType::Off)
            {
                std::printf ("  FAIL: filter `enabled: false` no longer forces Off\n");
                ++failures;
            }
        }
    }

    // ── 4. the engine: sequencer.engine, and the bool it replaced ──
    // generativeSeq.enabled was the Step<->Gen switch before the Phrase
    // engine. A file that names the engine is read by it alone; a file from
    // before reads the bool; a file with neither lands on the layout default.
    {
        struct EngineCase
        {
            const char* what;
            bool keepEngine; const char* engine;       // engine key, when kept
            bool keepEnabled; bool enabled;            // legacy bool, when kept
            int decoy, want;
        };
        const EngineCase kEngine[] = {
            { "neither key",                       false, "",       false, false, SeqEngine::Gen,    SeqEngine::Step },
            { "only the legacy bool, true",        false, "",       true,  true,  SeqEngine::Phrase, SeqEngine::Gen },
            { "only the legacy bool, false",       false, "",       true,  false, SeqEngine::Gen,    SeqEngine::Step },
            { "engine phrase over a stale bool",   true,  "phrase", true,  true,  SeqEngine::Gen,    SeqEngine::Phrase },
            { "engine gen",                        true,  "gen",    true,  true,  SeqEngine::Step,   SeqEngine::Gen },
            { "engine step over a stale bool",     true,  "step",   true,  true,  SeqEngine::Phrase, SeqEngine::Step },
        };
        for (const auto& e : kEngine)
        {
            auto parsed = juce::JSON::parse (fullJson);
            auto* seq = resolvePath (parsed, "sequencer");
            auto* gs  = resolvePath (parsed, "generativeSeq");
            if (seq == nullptr || gs == nullptr)
            {
                std::printf ("  FAIL: engine — sequencer or generativeSeq missing from the export\n");
                ++failures;
                break;
            }
            if (e.keepEngine) seq->setProperty ("engine", e.engine); else seq->removeProperty ("engine");
            if (e.keepEnabled) gs->setProperty ("enabled", e.enabled); else gs->removeProperty ("enabled");
            setPlain (proc, PID::seqEngine, (float) e.decoy);
            proc.importJsonPreset (juce::JSON::toString (parsed, true));
            const int got = (int) std::lround (plainOf (proc, PID::seqEngine));
            if (got != e.want)
            {
                std::printf ("  FAIL: engine, %s: loaded %d, want %d\n", e.what, got, e.want);
                ++failures;
            }
        }

        // The export writes the bool as engine == Gen, so a reader that knows
        // only the bool still tells Gen from the rest.
        for (int eng = 0; eng < SeqEngine::kCount; ++eng)
        {
            setPlain (proc, PID::seqEngine, (float) eng);
            auto parsed = juce::JSON::parse (proc.exportJsonPreset());
            auto* seq = resolvePath (parsed, "sequencer");
            auto* gs  = resolvePath (parsed, "generativeSeq");
            if (seq == nullptr || gs == nullptr
                || seq->getProperty ("engine").toString() != SeqEngine::kEntries[eng].key
                || (bool) gs->getProperty ("enabled") != (eng == SeqEngine::Gen))
            {
                std::printf ("  FAIL: engine %d exported as engine \"%s\", enabled %d\n", eng,
                             seq != nullptr ? seq->getProperty ("engine").toString().toRawUTF8() : "?",
                             gs != nullptr ? (int) (bool) gs->getProperty ("enabled") : -1);
                ++failures;
            }
        }
    }

    // ── 5. the phraseSeq block and the stored motif ──
    {
        const auto phraseDecoys = [&proc]
        {
            for (const char* id : kPhraseParams)
                setNorm (proc, id, decoyNormFor (proc, id));
        };
        const auto withMotif = [&fullJson] (const juce::var& motif)
        {
            auto parsed = juce::JSON::parse (fullJson);
            if (auto* ph = resolvePath (parsed, "phraseSeq"))
                ph->setProperty ("motif", motif);
            return juce::JSON::toString (parsed, true);
        };
        const auto withoutPhrase = [&fullJson] (bool asPattern)
        {
            auto parsed = juce::JSON::parse (fullJson);
            auto* root = parsed.getDynamicObject();
            root->removeProperty ("phraseSeq");
            if (! asPattern)
                return juce::JSON::toString (parsed, true);
            // A .t5seq as SequencerPanel::writePatternTo writes it, minus the
            // block under test.
            juce::DynamicObject::Ptr pat = new juce::DynamicObject();
            pat->setProperty ("version", 1);
            pat->setProperty ("kind", "t5seq");
            for (const char* key : { "sequencer", "arpeggiator", "generativeSeq" })
                pat->setProperty (key, root->getProperty (key));
            return juce::JSON::toString (juce::var (pat.get()), true);
        };
        const auto stored = [&proc] { return proc.getPhraseSequencer().getStoredMotif(); };
        const PhraseModel::Motif probe = probeMotif();

        // a. The reader, held to the format.
        proc.importJsonPreset (withMotif (motifJson (probe)));
        if (! sameMotif (stored(), probe))
        {
            std::printf ("  FAIL: phraseSeq.motif written to the format did not load as written\n");
            ++failures;
        }

        // b. Writer and reader: what the export writes loads back as it was.
        {
            auto parsed = juce::JSON::parse (proc.exportJsonPreset());
            auto* ph = resolvePath (parsed, "phraseSeq");
            const juce::var exported = ph != nullptr ? ph->getProperty ("motif") : juce::var();
            proc.importJsonPreset (withoutPhrase (false));
            proc.importJsonPreset (withMotif (exported));
            if (! sameMotif (stored(), probe))
            {
                std::printf ("  FAIL: the exported motif did not load back as it was stored\n");
                ++failures;
            }
        }

        // c. A .t5seq without the block leaves the phrase sequencer as it is.
        phraseDecoys();
        std::vector<float> parked;
        for (const char* id : kPhraseParams) parked.push_back (normOf (proc, id));
        proc.importJsonPreset (withoutPhrase (true));
        for (size_t i = 0; i < std::size (kPhraseParams); ++i)
            if (std::abs (normOf (proc, kPhraseParams[i]) - parked[i]) > 1.0e-5f)
            {
                std::printf ("  FAIL: a .t5seq without phraseSeq moved %s\n", kPhraseParams[i]);
                ++failures;
            }
        if (! sameMotif (stored(), probe))
        {
            std::printf ("  FAIL: a .t5seq without phraseSeq replaced the stored motif\n");
            ++failures;
        }

        // d. A preset without the block predates the phrase sequencer: defaults
        //    and the empty motif, as a reset leaves them.
        proc.importJsonPreset (withoutPhrase (false));
        for (const char* id : kPhraseParams)
            if (std::abs (normOf (proc, id) - defaultNormOf (proc, id)) > 1.0e-4f)
            {
                std::printf ("  FAIL: a preset without phraseSeq left %s at %.4f, default %.4f\n",
                             id, normOf (proc, id), defaultNormOf (proc, id));
                ++failures;
            }
        if (! sameMotif (stored(), PhraseModel::Motif {}))
        {
            std::printf ("  FAIL: a preset without phraseSeq kept a motif\n");
            ++failures;
        }

        // e. The block without its motif: the empty motif, not the last one.
        proc.importJsonPreset (withMotif (motifJson (probe)));
        {
            auto parsed = juce::JSON::parse (fullJson);
            if (auto* ph = resolvePath (parsed, "phraseSeq"))
                ph->removeProperty ("motif");
            proc.importJsonPreset (juce::JSON::toString (parsed, true));
            if (! sameMotif (stored(), PhraseModel::Motif {}))
            {
                std::printf ("  FAIL: phraseSeq without a motif kept the previous one\n");
                ++failures;
            }
        }

        // f. A lead-in that names no phrase has no place to apply: off.
        {
            auto m = motifJson (probe);
            if (auto* lo = m.getDynamicObject()->getProperty ("leadIn").getDynamicObject())
                lo->removeProperty ("phrase");
            proc.importJsonPreset (withMotif (m));
            auto want = probe;
            want.leadInKept = false;
            want.leadIn = {};
            want.leadInPhrase = 0;
            if (! sameMotif (stored(), want))
            {
                std::printf ("  FAIL: a lead-in without \"phrase\" did not stay off\n");
                ++failures;
            }
        }

        // g. A motif not heard yet keeps "origin": "next" through a round trip.
        {
            auto m = motifJson (probe);
            m.getDynamicObject()->setProperty ("origin", "next");
            proc.importJsonPreset (withMotif (m));
            auto parsed = juce::JSON::parse (proc.exportJsonPreset());
            auto* ph = resolvePath (parsed, "phraseSeq");
            const auto* mo = ph != nullptr ? ph->getProperty ("motif").getDynamicObject() : nullptr;
            if (stored().origin != PhraseModel::kOriginAtNextEntry || mo == nullptr
                || mo->getProperty ("origin").toString() != "next")
            {
                std::printf ("  FAIL: origin \"next\" did not survive a round trip\n");
                ++failures;
            }
        }
    }

    std::printf ("\n%d cases stripped, %d round-tripped complete, %d of them able to see a "
                 "plain-0 write, %d failures\n",
                 stripChecked, completeChecked, discriminating, failures);
    return failures == 0 ? 0 : 1;
}
