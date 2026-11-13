// A sequencer-engine switch made while the instrument sits in deep idle.
//
// processBlock applies a change of PID::seqEngine (the Step / Phrase / Gen
// switch) below its deep-idle return, and the computer keyboard routes its
// keys by the engine in effect (phraseTakesKeys: Phrase with Keys harmony
// takes them as chord input, every other engine plays them). A computer key
// is no MIDI and wakes nothing, so a switch made while silent — the panel
// button, a preset load — has to wake the block by itself, or the keys keep
// following the engine that was there before. Found 2026-11-14 on 11466052:
// after Phrase+Keys -> Step in deep idle every computer key went into the
// phrase sequencer and sounded nothing, in the standalone without a MIDI
// device for as long as nothing else started a block.
//
// The mark a preset load leaves (SeqTransferHold) is the other side of it:
// the switch a load causes carries no pattern over, and the load's end marks
// the next block's switch as the load's. That next block must consume the
// mark even when it returns at the idle gate; otherwise the mark outlives
// the load, and the user's own next switch carries nothing over (Gen -> Step
// did not copy the pattern into the step grid). Consuming it must not wake
// the block: a load made while silent that leaves the engine as it is keeps
// the instrument idle (docs/PERFORMANCE_GUIDE.md, the audioIdle gate).
// Init (resetParametersToDefaults) is such a load: from Gen or Phrase it sets
// the engine to Step and leaves the step grid as Init from Step leaves it.
//
// Each case runs the same switch awake and in deep idle; the awake run is the
// control, and `audioIdle` is asserted before an idle switch, so an idle case
// that never reached idle fails instead of passing vacuously.
//
// Build: the processor-level recipe of tools/test_preset_missing_key_defaults.cpp
// (libakroasys_SharedCode.a + libT5ynthData.a). Exits non-zero on failure.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"

#include <array>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0;

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
// prepareToPlay idles the block after ~10 s of silence; a margin past that.
constexpr int kIdleBlocks = static_cast<int>(10.0 * kRate / kBlock) + 20;

void check(const char* what, bool ok)
{
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (! ok) ++failures;
}

void setPlain(T5ynthProcessor& p, const char* id, float v)
{
    auto* par = p.getValueTreeState().getParameter(id);
    par->setValueNotifyingHost(par->convertTo0to1(v));
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

/** Switches from (fromEngine, fromHarm) to (toEngine, toHarm), in deep idle
 *  or awake, then presses one computer key. Returns the voices it sounds;
 *  -1 when an idle run did not reach idle before the switch. */
int keyAfterSwitch(int fromEngine, int fromHarm, int toEngine, int toHarm, bool idle)
{
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    setPlain(proc, PID::phrHarm, static_cast<float>(fromHarm));
    setPlain(proc, PID::seqEngine, static_cast<float>(fromEngine));
    blocks(proc, 2);
    if (idle)
    {
        blocks(proc, kIdleBlocks);
        if (! proc.audioIdle.load())
        {
            std::printf("    did not reach deep idle before the switch\n");
            return -1;
        }
    }
    setPlain(proc, PID::phrHarm, static_cast<float>(toHarm));
    setPlain(proc, PID::seqEngine, static_cast<float>(toEngine));
    blocks(proc, 3);
    proc.beginComputerKeyboardNote(60, 0.8f, true);
    blocks(proc, 3);
    const int voices = proc.getVoiceManager().getActiveVoiceCount();
    std::printf("    voices after the key: %d\n", voices);
    proc.endComputerKeyboardNote(60);
    blocks(proc, 2);
    return voices;
}
/** Gen live, a step grid that differs from Gen's pattern, then (optionally)
 *  a preset import that leaves the engine at Gen, in deep idle or awake.
 *  Then the user picks Step. Returns whether the Gen pattern was copied into
 *  the step grid; false also when the case could not show the copy. */
bool genToStepCopiesAfterImport(bool import, bool idle)
{
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Gen));
    blocks(proc, 2);
    {
        const juce::ScopedLock sl(proc.getCallbackLock());
        auto& st = proc.getStepSequencer();
        st.setNumSteps(8);
        for (int i = 0; i < 8; ++i) { st.setStepNote(i, 50); st.setStepEnabled(i, true); }
    }
    setPlain(proc, PID::seqSteps, 8.0f);
    blocks(proc, 2);
    if (idle)
    {
        blocks(proc, kIdleBlocks);
        if (! proc.audioIdle.load())
        {
            std::printf("    did not reach deep idle before the import\n");
            return false;
        }
    }
    if (import && ! proc.importJsonPreset(proc.exportJsonPreset()))
    {
        std::printf("    import failed\n");
        return false;
    }
    blocks(proc, 3);

    const auto& gs = proc.getGenerativeSequencer();
    const int genN = gs.numStepsForGui.load();
    std::vector<int> pattern;
    bool differs = genN != 8;
    for (int i = 0; i < genN; ++i)
    {
        pattern.push_back(gs.notePatternForGui[static_cast<size_t>(i)].load());
        differs = differs || pattern.back() != 50;
    }
    if (genN <= 0 || ! differs)
    {
        std::printf("    Gen's pattern cannot show a copy (%d steps)\n", genN);
        return false;
    }

    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Step));
    blocks(proc, 2);
    auto& st = proc.getStepSequencer();
    bool copied = st.getNumSteps() == genN;
    for (int i = 0; copied && i < genN; ++i)
    {
        const auto& step = st.getStep(i);
        copied = pattern[static_cast<size_t>(i)] > 0
            ? step.enabled && step.note == pattern[static_cast<size_t>(i)]
            : ! step.enabled;
    }
    std::printf("    Gen pattern %d steps, step grid %d steps, copied: %s\n",
                genN, st.getNumSteps(), copied ? "yes" : "no");
    return copied;
}

/** A preset whose engine is Gen, imported into a processor running Step, in
 *  deep idle or awake. The switch is the load's: Gen keeps the preset's step
 *  count instead of being seeded from the step grid. The preset fixes Gen's
 *  steps, so Gen's drift does not write its own count back over the param. */
bool importedSwitchCarriesNothing(bool idle)
{
    juce::String json;
    {
        T5ynthProcessor src;
        src.prepareToPlay(kRate, kBlock);
        blocks(src, 4);
        // The switch first: Step -> Gen seeds Gen's step count from the grid.
        setPlain(src, PID::seqEngine, static_cast<float>(SeqEngine::Gen));
        blocks(src, 2);
        setPlain(src, PID::genFixSteps, 1.0f);
        setPlain(src, PID::genSteps, 11.0f);
        setPlain(src, PID::seqSteps, 8.0f);
        blocks(src, 2);
        json = src.exportJsonPreset();
    }
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    if (idle)
    {
        blocks(proc, kIdleBlocks);
        if (! proc.audioIdle.load())
        {
            std::printf("    did not reach deep idle before the import\n");
            return false;
        }
    }
    if (! proc.importJsonPreset(json))
    {
        std::printf("    import failed\n");
        return false;
    }
    blocks(proc, 3);
    auto& apvts = proc.getValueTreeState();
    const int engine = static_cast<int>(apvts.getRawParameterValue(PID::seqEngine)->load());
    const int genSteps = static_cast<int>(apvts.getRawParameterValue(PID::genSteps)->load());
    std::printf("    engine %d, gen steps %d (preset 11, step grid 8)\n", engine, genSteps);
    return engine == SeqEngine::Gen && genSteps == 11;
}
/** `from` (Gen or Phrase) plays for a moment and stops, so it has a pattern
 *  to carry (the phrase view stays filled after a stop); then a step grid
 *  that differs from that pattern, then Init's parameters
 *  (resetParametersToDefaults), in deep idle or awake. Init sets the engine
 *  to Step. Returns whether the step grid kept its notes, i.e. that switch
 *  carried nothing over; false also when the case could not show a copy. */
bool initCarriesNothing(int from, bool idle)
{
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    setPlain(proc, PID::seqEngine, static_cast<float>(from));
    blocks(proc, 2);
    setPlain(proc, PID::seqRunning, 1.0f);
    blocks(proc, 20);
    setPlain(proc, PID::seqRunning, 0.0f);
    blocks(proc, 2);
    {
        const juce::ScopedLock sl(proc.getCallbackLock());
        auto& st = proc.getStepSequencer();
        st.setNumSteps(8);
        for (int i = 0; i < 8; ++i) { st.setStepNote(i, 50); st.setStepEnabled(i, true); }
    }
    setPlain(proc, PID::seqSteps, 8.0f);
    blocks(proc, 2);
    if (idle)
    {
        blocks(proc, kIdleBlocks);
        if (! proc.audioIdle.load())
        {
            std::printf("    did not reach deep idle before Init\n");
            return false;
        }
    }

    // What a switch to Step would carry: Gen's pattern, or the bar the phrase
    // view shows; -1 for a rest.
    std::vector<int> carried;
    if (from == SeqEngine::Gen)
    {
        const auto& gs = proc.getGenerativeSequencer();
        for (int i = 0; i < gs.numStepsForGui.load(); ++i)
        {
            const int note = gs.notePatternForGui[static_cast<size_t>(i)].load();
            carried.push_back(note > 0 ? note : -1);
        }
    }
    else
    {
        std::array<PhraseModel::StepView, PhraseModel::kMaxBarSteps> view {};
        const int n = proc.getPhraseSequencer().readView(view);
        for (int i = 0; i < n; ++i)
            carried.push_back(view[static_cast<size_t>(i)].note);
    }
    bool differs = false;
    for (size_t i = 0; i < carried.size() && i < 8; ++i)
        differs = differs || carried[i] != 50;
    if (! differs)
    {
        std::printf("    the engine's pattern cannot show a copy (%d steps)\n",
                    static_cast<int>(carried.size()));
        return false;
    }

    proc.resetParametersToDefaults();
    blocks(proc, 3);
    const int engine = static_cast<int>(
        proc.getValueTreeState().getRawParameterValue(PID::seqEngine)->load());
    auto& st = proc.getStepSequencer();
    bool kept = engine == SeqEngine::Step;
    for (int i = 0; i < 8; ++i)
        kept = kept && st.getStep(i).enabled && st.getStep(i).note == 50;
    std::printf("    engine %d, step grid kept its notes: %s\n", engine, kept ? "yes" : "no");
    return kept;
}

/** A preset import made in deep idle that leaves the engine as it is.
 *  Returns whether the block is still idle after it; false also when the
 *  case did not reach idle before the import. */
bool idleImportStaysIdle()
{
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4 + kIdleBlocks);
    if (! proc.audioIdle.load())
    {
        std::printf("    did not reach deep idle before the import\n");
        return false;
    }
    if (! proc.importJsonPreset(proc.exportJsonPreset()))
    {
        std::printf("    import failed\n");
        return false;
    }
    blocks(proc, 3);
    const bool idle = proc.audioIdle.load();
    std::printf("    idle after the import: %s\n", idle ? "yes" : "no");
    return idle;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const int step = SeqEngine::Step, phrase = SeqEngine::Phrase;
    const int keys = PhrHarm::Keys, pedal = PhrHarm::Pedal;

    std::printf("The computer keyboard follows an engine switch\n");
    check("Phrase+Keys -> Step, awake: the key sounds",
          keyAfterSwitch(phrase, keys, step, keys, false) > 0);
    check("Phrase+Keys -> Step, deep idle: the key sounds",
          keyAfterSwitch(phrase, keys, step, keys, true) > 0);
    check("Step -> Phrase+Keys, awake: the key is chord input, no voice",
          keyAfterSwitch(step, pedal, phrase, keys, false) == 0);
    check("Step -> Phrase+Keys, deep idle: the key is chord input, no voice",
          keyAfterSwitch(step, pedal, phrase, keys, true) == 0);

    std::printf("A preset load's transfer mark ends with the load\n");
    check("no import, Gen -> Step: the pattern is copied",
          genToStepCopiesAfterImport(false, true));
    check("import awake, then Gen -> Step: the pattern is copied",
          genToStepCopiesAfterImport(true, false));
    check("import in deep idle, then Gen -> Step: the pattern is copied",
          genToStepCopiesAfterImport(true, true));
    check("Gen preset into Step, awake: the load's switch seeds nothing",
          importedSwitchCarriesNothing(false));
    check("Gen preset into Step, deep idle: the load's switch seeds nothing",
          importedSwitchCarriesNothing(true));
    check("import in deep idle, engine unchanged: the block stays idle",
          idleImportStaysIdle());
    const int gen = SeqEngine::Gen;
    check("Init from Gen, awake: the step grid keeps its notes",
          initCarriesNothing(gen, false));
    check("Init from Gen, deep idle: the step grid keeps its notes",
          initCarriesNothing(gen, true));
    check("Init from Phrase, awake: the step grid keeps its notes",
          initCarriesNothing(phrase, false));
    check("Init from Phrase, deep idle: the step grid keeps its notes",
          initCarriesNothing(phrase, true));

    std::printf("failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
