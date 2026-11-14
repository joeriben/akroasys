// The sequencer's transport button follows seq_running.
//
// The button's onClick reads the parameter and toggles it, so its label is
// the only thing telling the user what a click will do. The timer set the
// label below its audioIdle gate: a STOP that is not the button (automation,
// a controller, a state load) could let the audio reach deep idle before the
// next tick, and from then on every tick returned at the gate. The button
// kept reading STOP indefinitely, and clicking it to stop STARTED the
// transport. The label now follows the parameter above the gate, touched
// only when the parameter flips.
//
//   A. STOP by parameter, deep idle before the next tick, in Step and in
//      Phrase: the next tick reads PLAY, the idle ticks after it invalidate
//      nothing, and a click then starts the transport.
//   B. A panel built while the transport runs (an editor opened during
//      playback) reads STOP after its first tick, and a click stops it.
//
// Build: the processor-level recipe of tools/test_seq_engine_idle.cpp
// (libakroasys_SharedCode.a + libT5ynthData.a). Exits non-zero on failure.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"
#include "gui/SequencerPanel.h"
#include "gui/T5ynthLookAndFeel.h"

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

bool running(T5ynthProcessor& p)
{
    return p.getValueTreeState().getRawParameterValue(PID::seqRunning)->load() > 0.5f;
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

juce::TextButton* transportButton(const SequencerPanel& panel)
{
    for (auto* c : panel.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(c))
            if (b->getTooltip() == "Start/stop the sequencer transport")
                return b;
    return nullptr;
}

/** A: STOP by parameter, deep idle before the panel's next tick. */
void paramStopThenIdle(int engine, const char* name)
{
    std::printf("A. STOP by parameter in %s, deep idle before the next tick\n", name);
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    setPlain(proc, PID::seqEngine, static_cast<float>(engine));
    blocks(proc, 4);
    SequencerPanel panel(proc);
    auto* probe = new RepaintProbe();
    panel.setCachedComponentImage(probe);
    panel.setVisible(true);   // a hidden component passes no repaint on
    panel.setSize(864, 208);
    auto* button = transportButton(panel);
    check("the transport button is found", button != nullptr);
    if (! button) return;
    tick();
    check("stopped: it reads PLAY", button->getButtonText() == "PLAY");

    button->onClick();
    blocks(proc, 8);
    tick();
    check("a click starts the transport, and the button reads STOP",
          running(proc) && button->getButtonText() == "STOP");

    setPlain(proc, PID::seqRunning, 0.0f);   // automation, not the button
    blocks(proc, kIdleBlocks);
    check("deep idle before any tick", proc.audioIdle.load() && ! running(proc));
    check("the button still reads STOP before the tick", button->getButtonText() == "STOP");

    probe->any = false;
    tick();
    check("the next tick reads PLAY", button->getButtonText() == "PLAY");
    check("and repaints", probe->any);

    probe->any = false;
    for (int i = 0; i < 4; ++i) tick();
    check("the idle ticks after it invalidate nothing", ! probe->any);
    check("still deep idle", proc.audioIdle.load());

    button->onClick();
    check("a click on PLAY starts the transport", running(proc));
}

/** B: the panel is built while the transport runs. */
void builtWhileRunning()
{
    std::printf("B. A panel built while the transport runs\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Step));
    setPlain(proc, PID::seqRunning, 1.0f);
    blocks(proc, 8);
    SequencerPanel panel(proc);
    panel.setSize(864, 208);
    auto* button = transportButton(panel);
    check("the transport button is found", button != nullptr);
    if (! button) return;
    tick();
    check("its first tick reads STOP", button->getButtonText() == "STOP");
    button->onClick();
    check("a click on STOP stops the transport", ! running(proc));
    tick();
    check("and the next tick reads PLAY", button->getButtonText() == "PLAY");
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    T5ynthLookAndFeel lnf;
    juce::LookAndFeel::setDefaultLookAndFeel(&lnf);

    paramStopThenIdle(SeqEngine::Step, "Step");
    paramStopThenIdle(SeqEngine::Phrase, "Phrase");
    builtWhileRunning();

    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAIL\n", failures);
    return failures == 0 ? 0 : 1;
}
