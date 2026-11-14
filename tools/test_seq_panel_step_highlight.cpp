// The step grid's playhead highlight once Step playback has stopped.
//
// The panel lights the column of the step being played (StepColumn's
// isCurrentStep) from StepSequencer::currentStepForGui, which the audio
// thread sets to -1 when Step playback stops. Two ways the last played column
// used to stay lit after that:
//
//   A. STOP pressed in Step mode. The button cleared the panel's mirror of the
//      current step but not the column's flag; the timer then saw the mirror
//      already equal to the stopped -1 and never unlit the column. Once the
//      click put the column out, a tick landing before the audio thread's next
//      block read the still-set playhead and lit it again for one tick; the
//      panel now shows no playhead while the transport parameter reads
//      stopped.
//   B. Step running -> Phrase -> stop -> deep idle -> Step. The highlight is
//      only updated in Step mode, and below the timer's audioIdle gate. The
//      Phrase stretch left the flag set, and entering Step in deep idle laid
//      the grid out with it, so the column showed lit until audio activity.
//   C. As B, but a layout other than the timer's sees the switch first: a
//      window resize (or a step-count sync) runs resized(), which took the
//      engine edge, so the timer's entering-Step re-read of the playhead never
//      ran and the column stayed lit. resized() now re-reads it itself.
//   D. STOP arrives as a parameter change in Step (automation, a controller,
//      a state load), not the button, and the audio reaches deep idle before
//      the panel's next tick. Every tick then returned at the audioIdle gate,
//      which the highlight sat below, and the column stayed lit until audio
//      activity. The highlight now runs above the gate; the idle ticks after
//      it go out invalidate nothing.
//
// A column is lit when its snapshot differs from the others: every step
// carries the same data, so unlit columns of one kind (with or without the
// beat border on every fourth) render identically. A snapshot paints the
// flag whether or not the panel asked for a repaint, so A also checks that
// the column lit and the column unlit at the click are invalidated (a CachedComponentImage
// on the panel records it; there is no window here).
//
// Build: the processor-level recipe of tools/test_seq_engine_idle.cpp
// (libakroasys_SharedCode.a + libT5ynthData.a). Exits non-zero on failure.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"
#include "gui/SequencerPanel.h"
#include "gui/T5ynthLookAndFeel.h"

#include <cstdio>
#include <cstring>
#include <typeinfo>
#include <vector>

namespace
{
int failures = 0;

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
// prepareToPlay idles the block after ~10 s of silence; a margin past that.
constexpr int kIdleBlocks = static_cast<int>(10.0 * kRate / kBlock) + 20;
constexpr int kSteps = 16;

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

/** One tick of the panel's 10 Hz timer (see test_seq_panel_step_switch.cpp
 *  for why 450 ms). */
void tick()
{
    juce::Thread::sleep(450);
    juce::Timer::callPendingTimersSynchronously();
}

std::vector<juce::Component*> stepColumns(const SequencerPanel& panel)
{
    std::vector<juce::Component*> cols;
    for (auto* c : panel.getChildren())
        if (c->isVisible() && std::strstr(typeid(*c).name(), "StepColumn") != nullptr)
            cols.push_back(c);
    return cols;
}

bool sameImage(const juce::Image& a, const juce::Image& b)
{
    if (a.getWidth() != b.getWidth() || a.getHeight() != b.getHeight())
        return false;
    for (int y = 0; y < a.getHeight(); ++y)
        for (int x = 0; x < a.getWidth(); ++x)
            if (a.getPixelAt(x, y) != b.getPixelAt(x, y))
                return false;
    return true;
}

/** The indices of the lit columns: within each kind (beat-border column or
 *  not), the columns whose rendering differs from the kind's majority. */
std::vector<int> litColumns(const SequencerPanel& panel)
{
    const auto cols = stepColumns(panel);
    std::vector<juce::Image> shots;
    for (auto* c : cols)
        shots.push_back(c->createComponentSnapshot(c->getLocalBounds(), false));

    std::vector<int> lit;
    for (size_t i = 0; i < cols.size(); ++i)
    {
        int same = 0, kind = 0;
        for (size_t j = 0; j < cols.size(); ++j)
        {
            if ((i % 4 == 0) != (j % 4 == 0) || i == j) continue;
            ++kind;
            if (sameImage(shots[i], shots[j])) ++same;
        }
        if (same * 2 < kind)
            lit.push_back(static_cast<int>(i));
    }
    return lit;
}

/** Makes every step of the 16 the same. */
void levelSteps(T5ynthProcessor& proc)
{
    const juce::ScopedLock sl(proc.getCallbackLock());
    auto& st = proc.getStepSequencer();
    for (int i = 0; i < kSteps; ++i)
    {
        st.setStepNote(i, 50);
        st.setStepEnabled(i, true);
        st.setStepVelocity(i, 0.8f);
        st.setStepGate(i, 0.8f);
        st.setStepBindMode(i, T5ynthStepSequencer::BindMode::Off);
        for (int s = 0; s < T5ynthStepSequencer::ONE_SHOT_SLOTS; ++s)
            st.setStepOneShotMode(i, s, T5ynthStepSequencer::OneShotMode::Normal);
    }
}

/** A processor with a 16-step grid in which every step is the same. */
void uniformGrid(T5ynthProcessor& proc)
{
    {
        const juce::ScopedLock sl(proc.getCallbackLock());
        proc.getStepSequencer().setNumSteps(kSteps);
    }
    levelSteps(proc);
    setPlain(proc, PID::seqSteps, static_cast<float>(kSteps));
    blocks(proc, 2);
}

/** Plays Step until its playhead is past the first step, then ticks: the
 *  played column is lit. Returns that column. */
int playStepAndShow(T5ynthProcessor& proc)
{
    setPlain(proc, PID::seqRunning, 1.0f);
    int step = -1;
    for (int i = 0; i < 400 && step < 2; ++i)
    {
        blocks(proc, 1);
        step = proc.getStepSequencer().currentStepForGui.load();
    }
    tick();
    return step;
}

/** Records what the panel invalidates, its own repaints and its children's.
 *  Owned by the panel (setCachedComponentImage). */
struct RepaintProbe final : juce::CachedComponentImage
{
    bool all = false;
    juce::Rectangle<int> area;

    void paint(juce::Graphics&) override {}
    bool invalidateAll() override { all = true; return true; }
    bool invalidate(const juce::Rectangle<int>& r) override
    {
        area = area.isEmpty() ? r : area.getUnion(r);
        return true;
    }
    void releaseResources() override {}

    void reset() { all = false; area = {}; }
    bool covers(juce::Rectangle<int> r) const { return all || area.contains(r); }
};

juce::TextButton* buttonLabelled(const SequencerPanel& panel, const char* text)
{
    for (auto* c : panel.getChildren())
        if (auto* b = dynamic_cast<juce::TextButton*>(c))
            if (b->getButtonText() == text)
                return b;
    return nullptr;
}

void printLit(const std::vector<int>& lit)
{
    std::printf("    lit columns:");
    for (int i : lit) std::printf(" %d", i);
    std::printf(lit.empty() ? " none\n" : "\n");
}

/** A: STOP pressed in Step mode. */
void stopButtonInStep()
{
    std::printf("A. STOP pressed while Step plays\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    uniformGrid(proc);
    SequencerPanel panel(proc);
    auto* probe = new RepaintProbe();
    panel.setCachedComponentImage(probe);
    panel.setVisible(true);   // a hidden component passes no repaint on
    panel.setSize(864, 208);
    tick();
    check("no column lit before playback", litColumns(panel).empty());

    probe->reset();
    const int step = playStepAndShow(proc);
    const auto playing = litColumns(panel);
    printLit(playing);
    check("the played column is lit", playing.size() == 1 && playing[0] == step);
    if (playing.size() != 1) return;
    const auto litBounds = stepColumns(panel)[static_cast<size_t>(playing[0])]->getBounds();
    check("and repainted", probe->covers(litBounds));

    auto* stop = buttonLabelled(panel, "STOP");
    check("the transport button reads STOP", stop != nullptr);
    if (stop == nullptr) return;
    probe->reset();
    stop->onClick();
    const auto clicked = litColumns(panel);
    printLit(clicked);
    check("the click puts the column out at once", clicked.empty());
    check("and repaints it", probe->covers(litBounds));

    // A tick before the audio thread's next block: the playhead still reads
    // the played step.
    check("the playhead still reads the played step",
          proc.getStepSequencer().currentStepForGui.load() == step);
    tick();
    const auto early = litColumns(panel);
    printLit(early);
    check("a tick before the next audio block keeps it out", early.empty());

    blocks(proc, 3);   // the audio thread stops Step playback
    check("Step playback stopped",
          proc.getStepSequencer().currentStepForGui.load() == -1 && ! proc.audioIdle.load());
    tick();
    const auto stopped = litColumns(panel);
    printLit(stopped);
    check("no column lit after STOP", stopped.empty());
}

/** B: Step running -> Phrase -> stop -> deep idle -> Step. The tick that
 *  lays out Step runs before any audio block. */
void stepAfterPhraseInIdle()
{
    std::printf("B. back to Step from a stopped Phrase, in deep idle\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    uniformGrid(proc);
    SequencerPanel panel(proc);
    panel.setSize(864, 208);
    tick();

    const int step = playStepAndShow(proc);
    const auto playing = litColumns(panel);
    check("the played column is lit", playing.size() == 1 && playing[0] == step);

    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
    blocks(proc, 4);
    tick();
    setPlain(proc, PID::seqRunning, 0.0f);
    blocks(proc, 4);
    tick();
    blocks(proc, kIdleBlocks);
    check("deep idle before the switch", proc.audioIdle.load());

    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Step));
    tick();
    const auto entered = litColumns(panel);
    printLit(entered);
    check("Step shows its 16 columns", stepColumns(panel).size() == kSteps);
    check("no column lit when Step is laid out", entered.empty());
    // The switch copies Phrase's pattern into the grid in this first block;
    // level it again so the columns compare.
    blocks(proc, 3);
    levelSteps(proc);
    tick();
    const auto afterBlock = litColumns(panel);
    printLit(afterBlock);
    check("Step shows its 16 columns after the copy", stepColumns(panel).size() == kSteps);
    check("none after the audio thread ran", afterBlock.empty());
}

/** C: as B, but a resize lays out Step before the timer sees the switch. */
void stepThroughResizeInIdle()
{
    std::printf("C. back to Step through a resize, in deep idle\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    uniformGrid(proc);
    SequencerPanel panel(proc);
    panel.setSize(864, 208);
    tick();

    const int step = playStepAndShow(proc);
    const auto playing = litColumns(panel);
    check("the played column is lit", playing.size() == 1 && playing[0] == step);

    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
    blocks(proc, 4);
    tick();
    setPlain(proc, PID::seqRunning, 0.0f);
    blocks(proc, 4);
    tick();
    blocks(proc, kIdleBlocks);
    check("deep idle before the switch", proc.audioIdle.load());

    setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Step));
    panel.setSize(900, 208);   // resized() sees the switch before the timer
    const auto resizedLit = litColumns(panel);
    printLit(resizedLit);
    check("Step shows its 16 columns after the resize", stepColumns(panel).size() == kSteps);
    check("no column lit when the resize lays out Step", resizedLit.empty());
    tick();
    tick();
    const auto ticked = litColumns(panel);
    printLit(ticked);
    check("nor after the timer ran", ticked.empty());
}
/** D: a STOP that is not the button, deep idle before the next tick. */
void paramStopThenIdle()
{
    std::printf("D. STOP by parameter in Step, deep idle before the next tick\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    uniformGrid(proc);
    SequencerPanel panel(proc);
    auto* probe = new RepaintProbe();
    panel.setCachedComponentImage(probe);
    panel.setVisible(true);
    panel.setSize(864, 208);
    tick();

    const int step = playStepAndShow(proc);
    const auto playing = litColumns(panel);
    printLit(playing);
    check("the played column is lit", playing.size() == 1 && playing[0] == step);
    if (playing.size() != 1) return;
    const auto litBounds = stepColumns(panel)[static_cast<size_t>(playing[0])]->getBounds();

    setPlain(proc, PID::seqRunning, 0.0f);   // automation, not the button
    blocks(proc, kIdleBlocks);
    check("deep idle with the playhead cleared before any tick",
          proc.audioIdle.load() && proc.getStepSequencer().currentStepForGui.load() == -1);

    probe->reset();
    tick();
    const auto ticked = litColumns(panel);
    printLit(ticked);
    check("the next tick puts the column out", ticked.empty());
    check("and repaints it", probe->covers(litBounds));

    probe->reset();
    for (int i = 0; i < 4; ++i) tick();
    check("the idle ticks after it invalidate nothing", !probe->all && probe->area.isEmpty());
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    T5ynthLookAndFeel lnf;
    juce::LookAndFeel::setDefaultLookAndFeel(&lnf);

    stopButtonInStep();
    stepAfterPhraseInIdle();
    stepThroughResizeInIdle();
    paramStopThenIdle();

    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAIL\n", failures);
    return failures == 0 ? 0 : 1;
}
