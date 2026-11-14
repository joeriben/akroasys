// The step grid after changes made while the transport is stopped.
//
// The sequencer panel's 10 Hz timer skips most of its work while the audio
// path is idle (docs/PERFORMANCE_GUIDE.md, the audioIdle gate), and its
// step-count sync used to sit below that gate. Ways the grid used to stay
// stale until PLAY:
//
//   A. Phrase -> Step and Gen -> Step copy the other engine's pattern into
//      the grid, on the audio thread, in the first block after the switch.
//      The timer that lays out the switch can run before that block, and
//      nothing told it the pattern was replaced afterwards. When the step
//      count stays the same, nothing redrew the grid: it showed the old
//      notes. The case asserts the panel asks for a repaint of the grid at
//      the next tick (a CachedComponentImage on the panel records what it
//      invalidates; there is no window here).
//   B. A load that brings its own pattern and switches to Step (no transfer,
//      SeqTransferHold) while Phrase is shown: the panel laid out Step with
//      the step count it last saw in Step mode. The case asserts the visible
//      step columns equal the loaded count at the first tick.
//   C. The control for the signal A uses: a step preset picked in Step mode
//      redraws the grid at the next tick (the refresh that existed for it).
//   D. A preset import that stays in Step, with Step shown. The import writes
//      the grid itself, on the calling thread, and the audio thread has
//      nothing to react to, so nothing told the panel. With another step count
//      the columns stayed at the old count; with the same count the grid was
//      not redrawn. The case asserts both at the first tick, before any block.
//   E. Init in Step with a hand-set step count (the step preset unchanged):
//      only seqSteps moves. The first tick shows the default count.
//   F. The host sets seqSteps with Step shown. The first tick shows it.
//   G. A step count outside the grid's window (seqSteps 1, or 33..64 from a
//      preset or the host): the grid shows the clamped count, and the idle
//      ticks after that lay nothing out. The step-count sync above the idle
//      gate compares the clamped value; comparing the raw one would resync
//      and lay the panel out on every tick, an idle-CPU regression that
//      repaints nothing, so the case counts resized() calls.
//
// All start in deep idle.
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

int plain(T5ynthProcessor& p, const char* id)
{
    return static_cast<int>(p.getValueTreeState().getRawParameterValue(id)->load());
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

/** One tick of the panel's 10 Hz timer: wait until it is due, then run the
 *  due timers on this (the message) thread; the panel's runs exactly once.
 *  Nothing dispatches the timer thread's messages here, so that thread
 *  waits up to 300 ms for each before it counts the timers down again; a
 *  wait shorter than 100 ms + 300 ms can find the panel's timer not yet due. */
void tick()
{
    juce::Thread::sleep(450);
    juce::Timer::callPendingTimersSynchronously();
}

/** The step columns the panel shows. StepColumn is the panel's private
 *  type, so the columns are found by their dynamic type's name. */
int visibleStepColumns(const SequencerPanel& panel)
{
    int n = 0;
    for (auto* c : panel.getChildren())
        if (c->isVisible() && std::strstr(typeid(*c).name(), "StepColumn") != nullptr)
            ++n;
    return n;
}

/** The bounds of the visible step columns, in the panel's space. */
juce::Rectangle<int> stepGridBounds(const SequencerPanel& panel)
{
    juce::Rectangle<int> r;
    for (auto* c : panel.getChildren())
        if (c->isVisible() && std::strstr(typeid(*c).name(), "StepColumn") != nullptr)
            r = r.isEmpty() ? c->getBounds() : r.getUnion(c->getBounds());
    return r;
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

/** A: Phrase or Gen -> Step in deep idle, the timer laying out the switch
 *  before the audio block that copies the other engine's pattern into the
 *  grid. The grid has that pattern's step count already, so only its notes
 *  change. */
void transferIntoStep(int fromEngine)
{
    std::printf("A. %s -> Step transfer, timer before the copying block\n",
                fromEngine == SeqEngine::Phrase ? "Phrase" : "Gen");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    // The phrase bar's steps at 4/4 in sixteenths; Gen takes the step count
    // from the grid it is seeded from.
    constexpr int kSteps = 16;
    {
        const juce::ScopedLock sl(proc.getCallbackLock());
        auto& st = proc.getStepSequencer();
        st.setNumSteps(kSteps);
        for (int i = 0; i < kSteps; ++i) { st.setStepNote(i, 50); st.setStepEnabled(i, true); }
    }
    setPlain(proc, PID::seqSteps, static_cast<float>(kSteps));
    blocks(proc, 2);
    {
        SequencerPanel panel(proc);
        auto* probe = new RepaintProbe();
        panel.setCachedComponentImage(probe);
        panel.setVisible(true);   // a hidden component passes no repaint on
        panel.setSize(864, 208);
        tick();
        check("Step shows the 16 steps before the switch", visibleStepColumns(panel) == kSteps);

        setPlain(proc, PID::seqEngine, static_cast<float>(fromEngine));
        blocks(proc, 2);
        tick();
        check("the other mode shows no step columns", visibleStepColumns(panel) == 0);
        if (fromEngine == SeqEngine::Phrase)
        {
            proc.phraseDice();
        }
        else
        {
            // Gen was seeded from the grid (all 50); move the grid away from
            // it, so the copy back changes the notes.
            const juce::ScopedLock sl(proc.getCallbackLock());
            auto& st = proc.getStepSequencer();
            for (int i = 0; i < kSteps; ++i) st.setStepNote(i, 62);
        }
        blocks(proc, 2);
        if (! reachIdle(proc)) { check("deep idle before the switch", false); return; }

        setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Step));
        tick();   // lays out Step before the audio thread copied anything
        const auto grid = stepGridBounds(panel);
        probe->reset();
        blocks(proc, 3);

        const auto& st = proc.getStepSequencer();
        bool notesChanged = false;
        for (int i = 0; i < kSteps; ++i)
            notesChanged = notesChanged || ! st.getStep(i).enabled
                           || st.getStep(i).note != (fromEngine == SeqEngine::Phrase ? 50 : 62);
        std::printf("    after the copy: seqSteps %d, audioIdle %d, columns %d\n",
                    plain(proc, PID::seqSteps), proc.audioIdle.load() ? 1 : 0,
                    visibleStepColumns(panel));
        check("the copy kept the step count", plain(proc, PID::seqSteps) == kSteps);
        check("the copy changed the notes", notesChanged);

        tick();
        check("the next tick shows the 16 steps", visibleStepColumns(panel) == kSteps);
        check("the next tick repaints the step grid", ! grid.isEmpty() && probe->covers(grid));
    }
}

/** B: a load that switches Phrase -> Step and brings its own step count,
 *  in deep idle. The tick runs before any audio block. */
void loadIntoStep()
{
    std::printf("B. a load that switches Phrase -> Step\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    setPlain(proc, PID::seqSteps, 8.0f);
    blocks(proc, 2);
    const auto eightSteps = proc.exportJsonPreset();
    setPlain(proc, PID::seqSteps, 24.0f);
    blocks(proc, 2);
    {
        SequencerPanel panel(proc);
        panel.setSize(864, 208);
        tick();
        check("Step shows the 24 steps before the switch", visibleStepColumns(panel) == 24);

        setPlain(proc, PID::seqEngine, static_cast<float>(SeqEngine::Phrase));
        blocks(proc, 2);
        tick();
        if (! reachIdle(proc)) { check("deep idle before the load", false); return; }

        check("the load succeeds", proc.importJsonPreset(eightSteps));
        std::printf("    after the load: engine %d, seqSteps %d, audioIdle %d\n",
                    plain(proc, PID::seqEngine), plain(proc, PID::seqSteps),
                    proc.audioIdle.load() ? 1 : 0);
        tick();
        std::printf("    first tick: columns %d\n", visibleStepColumns(panel));
        check("the first tick shows the loaded 8 steps", visibleStepColumns(panel) == 8);
        blocks(proc, 3);
        tick();
        check("and keeps them after the audio thread ran", visibleStepColumns(panel) == 8);
    }
}
/** C: a step preset picked in Step mode, in deep idle. The preset has the
 *  grid's step count, so only the notes change. */
void stepPresetInStep()
{
    std::printf("C. a step preset picked in Step mode\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    const int steps = plain(proc, PID::seqSteps);   // the default preset's 32
    {
        const juce::ScopedLock sl(proc.getCallbackLock());
        auto& st = proc.getStepSequencer();
        for (int i = 0; i < steps; ++i) { st.setStepNote(i, 50); st.setStepEnabled(i, true); }
    }
    {
        SequencerPanel panel(proc);
        auto* probe = new RepaintProbe();
        panel.setCachedComponentImage(probe);
        panel.setVisible(true);
        panel.setSize(864, 208);
        tick();
        if (! reachIdle(proc)) { check("deep idle before the preset", false); return; }
        tick();
        const auto grid = stepGridBounds(panel);
        probe->reset();

        setPlain(proc, PID::seqPreset, 1.0f);   // "Wide Leap", 32 steps
        blocks(proc, 3);
        const auto& st = proc.getStepSequencer();
        bool notesChanged = false;
        for (int i = 0; i < steps; ++i)
            notesChanged = notesChanged || ! st.getStep(i).enabled || st.getStep(i).note != 50;
        check("the preset kept the step count", plain(proc, PID::seqSteps) == steps);
        check("the preset changed the notes", notesChanged);
        tick();
        check("the next tick repaints the step grid", ! grid.isEmpty() && probe->covers(grid));
    }
}

/** Every step of the grid gets `note`, enabled. */
void fillGrid(T5ynthProcessor& proc, int note)
{
    const juce::ScopedLock sl(proc.getCallbackLock());
    auto& st = proc.getStepSequencer();
    for (int i = 0; i < 32; ++i) { st.setStepNote(i, note); st.setStepEnabled(i, true); }
}

/** D: a preset import that stays in Step while Step is shown, in deep idle.
 *  The tick runs before any audio block. */
void importInStep(bool otherCount)
{
    std::printf("D. a preset import that stays in Step (%s)\n",
                otherCount ? "24 -> 8 steps" : "16 steps, other notes");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    const int loaded = otherCount ? 8 : 16;
    const int shown  = otherCount ? 24 : 16;
    setPlain(proc, PID::seqSteps, static_cast<float>(loaded));
    blocks(proc, 2);
    fillGrid(proc, 72);
    const auto preset = proc.exportJsonPreset();
    setPlain(proc, PID::seqSteps, static_cast<float>(shown));
    blocks(proc, 2);
    fillGrid(proc, 50);
    {
        SequencerPanel panel(proc);
        auto* probe = new RepaintProbe();
        panel.setCachedComponentImage(probe);
        panel.setVisible(true);
        panel.setSize(864, 208);
        tick();
        if (! reachIdle(proc)) { check("deep idle before the import", false); return; }
        tick();
        check("Step shows the steps before the import", visibleStepColumns(panel) == shown);
        const auto grid = stepGridBounds(panel);
        probe->reset();

        check("the import succeeds", proc.importJsonPreset(preset));
        std::printf("    after the import: engine %d, seqSteps %d, note0 %d, audioIdle %d\n",
                    plain(proc, PID::seqEngine), plain(proc, PID::seqSteps),
                    proc.getStepSequencer().getStep(0).note, proc.audioIdle.load() ? 1 : 0);
        check("the import stays in Step", plain(proc, PID::seqEngine) == SeqEngine::Step);
        check("the import wrote the notes", proc.getStepSequencer().getStep(0).note == 72);
        tick();
        std::printf("    first tick: columns %d\n", visibleStepColumns(panel));
        if (otherCount)
            check("the first tick shows the loaded 8 steps", visibleStepColumns(panel) == loaded);
        else
            check("the first tick repaints the step grid", ! grid.isEmpty() && probe->covers(grid));
        blocks(proc, 3);
        tick();
        check("and keeps the loaded count after the audio thread ran",
              visibleStepColumns(panel) == loaded);
    }
}

/** E: Init in Step mode, in deep idle, with a hand-set step count. The tick
 *  runs before any audio block. */
void initInStep()
{
    std::printf("E. Init in Step with a hand-set step count\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    setPlain(proc, PID::seqSteps, 24.0f);
    blocks(proc, 2);
    {
        SequencerPanel panel(proc);
        panel.setSize(864, 208);
        tick();
        if (! reachIdle(proc)) { check("deep idle before Init", false); return; }
        tick();
        check("Step shows the 24 steps before Init", visibleStepColumns(panel) == 24);
        const int presetBefore = plain(proc, PID::seqPreset);

        proc.resetParametersToDefaults();
        const int steps = plain(proc, PID::seqSteps);
        std::printf("    after Init: seqSteps %d, seqPreset %d -> %d, audioIdle %d\n",
                    steps, presetBefore, plain(proc, PID::seqPreset),
                    proc.audioIdle.load() ? 1 : 0);
        check("Init moved the step count", steps != 24);
        check("and left the step preset as it was", plain(proc, PID::seqPreset) == presetBefore);
        tick();
        std::printf("    first tick: columns %d\n", visibleStepColumns(panel));
        check("the first tick shows Init's step count", visibleStepColumns(panel) == steps);
    }
}

/** F: the host sets seqSteps while Step is shown, in deep idle. */
void hostSetsSteps()
{
    std::printf("F. the host sets the step count\n");
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    setPlain(proc, PID::seqSteps, 16.0f);
    blocks(proc, 2);
    {
        SequencerPanel panel(proc);
        panel.setSize(864, 208);
        tick();
        if (! reachIdle(proc)) { check("deep idle before the change", false); return; }
        tick();
        check("Step shows the 16 steps before the change", visibleStepColumns(panel) == 16);

        setPlain(proc, PID::seqSteps, 12.0f);
        tick();
        std::printf("    first tick: columns %d, audioIdle %d\n", visibleStepColumns(panel),
                    proc.audioIdle.load() ? 1 : 0);
        check("the first tick shows the 12 steps", visibleStepColumns(panel) == 12);
    }
}
/** G: a step count the grid clamps, in deep idle; counts the layouts. */
struct CountingPanel final : SequencerPanel
{
    using SequencerPanel::SequencerPanel;
    int layouts = 0;
    void resized() override { ++layouts; SequencerPanel::resized(); }
};

void clampedStepsInIdle(float rawSteps, int shown)
{
    std::printf("G. seqSteps %d, which the grid shows as %d\n", static_cast<int>(rawSteps), shown);
    T5ynthProcessor proc;
    proc.prepareToPlay(kRate, kBlock);
    blocks(proc, 4);
    {
        CountingPanel panel(proc);
        panel.setSize(864, 208);
        tick();
        if (! reachIdle(proc)) { check("deep idle before the change", false); return; }
        tick();
        setPlain(proc, PID::seqSteps, rawSteps);
        check("the raw step count is outside the grid's window",
              plain(proc, PID::seqSteps) == static_cast<int>(rawSteps));
        tick();
        check("the first tick shows the clamped count", visibleStepColumns(panel) == shown);
        panel.layouts = 0;
        for (int i = 0; i < 4; ++i) tick();
        std::printf("    4 idle ticks: %d layouts, audioIdle %d\n", panel.layouts,
                    proc.audioIdle.load() ? 1 : 0);
        check("the idle ticks after it lay nothing out", panel.layouts == 0);
    }
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    T5ynthLookAndFeel lnf;
    juce::LookAndFeel::setDefaultLookAndFeel(&lnf);

    transferIntoStep(SeqEngine::Phrase);
    transferIntoStep(SeqEngine::Gen);
    loadIntoStep();
    stepPresetInStep();
    importInStep(true);
    importInStep(false);
    initInStep();
    hostSetsSteps();
    clampedStepsInIdle(1.0f, 2);
    clampedStepsInIdle(40.0f, 32);

    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    std::printf(failures == 0 ? "ALL PASS\n" : "%d FAIL\n", failures);
    return failures == 0 ? 0 : 1;
}
