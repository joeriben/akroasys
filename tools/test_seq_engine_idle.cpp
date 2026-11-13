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
// Each case runs the same switch awake and in deep idle; the awake run is the
// control, and `audioIdle` is asserted before an idle switch, so an idle case
// that never reached idle fails instead of passing vacuously.
//
// Build: the processor-level recipe of tools/test_preset_missing_key_defaults.cpp
// (libakroasys_SharedCode.a + libT5ynthData.a). Exits non-zero on failure.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"

#include <cstdio>

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

    std::printf("failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
