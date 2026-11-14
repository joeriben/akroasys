// The phrase view while the phrase sequencer is stopped.
//
// Stopped, the view shows the bar playback starts in on the sequencer's own
// transport, the first one, rendered from the stored motif and the parameters
// as they are now. Phrase -> Step copies that view into the step grid.
//
// Before 2026-11-14 the sequencer's processBlock returned at once while
// stopped, and in deep idle the processor never reached it: a Dice, a load or
// a phrase parameter changed while stopped left the view on the bar from
// before the change, and Phrase -> Step copied that bar (found in the review of
// the phrase sequencer's load paths).
//
// The oracle is the start itself: the bar the view shows while stopped must
// be the bar a start then enters, read one block after it. Each change is
// made in deep idle (asserted), must change the bar (asserted, so a case
// cannot pass with the view simply left as it was), and must let the
// instrument idle again (asserted: a wake that never clears is an idle-CPU
// regression).
//
// A change made in the same block as Phrase -> Step reaches the grid: the
// phrase section, which brings the view up to date, does not run in a block
// that switches away from Phrase, and the switch copied the bar from before
// the change (found in the review of db2a968a).
//
// A view rendered while stopped is not heard: a motif diced while stopped
// keeps its open origin and Keep does nothing with it, and a second Dice
// still anchors where playback next starts; a start, from a count-in or from
// the middle of the song, resolves that origin to the phrase it enters.
//
// A kept lead-in is refitted with the parameters: a Part change made while
// stopped must reach the bar through the lead-in as a load of the same state
// renders it. The lead-in reaches bar 0 only through an embellishment before
// the first head, and a scale change does not refit at all, so the case uses
// rotation 3, changes Part and searches dice for one whose lead-in shows
// (asserted: a load without the lead-in must differ).
//
// The Phrase -> Step transfer asks the sequencer whether it runs, not PLAY: a
// Dice, PLAY and the switch in one block copy the stopped view.
//
// Build: the processor-level recipe of tools/test_preset_missing_key_defaults.cpp
// (libakroasys_SharedCode.a + libT5ynthData.a). Exits non-zero on failure.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"

#include <array>
#include <cstdio>
#include <functional>
#include <string>

namespace
{
int failures = 0;

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
// Deep idle follows ~10 s of silence (tailBlocks); allow it twice.
constexpr int kIdleLimit = static_cast<int>(2 * 10.0 * kRate / kBlock) + 16;

void check(const char* what, bool ok)
{
    std::printf("  %-66s %s\n", what, ok ? "ok" : "FAIL");
    if (! ok) ++failures;
}

struct PlayHead : juce::AudioPlayHead
{
    bool playing = false;
    double ppq = 0.0;
    double bpm = 120.0;
    int num = 4, den = 4;

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setIsPlaying(playing);
        info.setBpm(bpm);
        info.setPpqPosition(ppq);
        info.setTimeSignature(TimeSignature { num, den });
        return info;
    }
};

struct Rig
{
    T5ynthProcessor proc;
    PlayHead head;

    Rig()
    {
        proc.setPlayHead(&head);
        proc.prepareToPlay(kRate, kBlock);
        blocks(4);
        set(PID::seqEngine, SeqEngine::Phrase);
        // A bar per chord, so the first bar is not the bar playing later
        // (Chords needs a scale with degrees; Off has none).
        set(PID::scaleType, ScaleType::Maj);
        set(PID::phrHarm, PhrHarm::Chords);
        set(PID::phrChord1, 1.0f);
        set(PID::phrChord2, 6.0f);
        set(PID::phrChord3, 7.0f);
        set(PID::phrChord4, 4.0f);
        set(PID::phrPulses, 7.0f);
        set(PID::phrEvolve, 0.6f);
        blocks(4);
    }

    void set(const char* id, float plain)
    {
        auto* par = proc.getValueTreeState().getParameter(id);
        par->setValueNotifyingHost(par->convertTo0to1(plain));
    }

    void blocks(int n, const juce::MidiBuffer* first = nullptr)
    {
        juce::AudioBuffer<float> buf(2, kBlock);
        for (int i = 0; i < n; ++i)
        {
            buf.clear();
            juce::MidiBuffer midi;
            if (i == 0 && first != nullptr) midi = *first;
            proc.processBlock(buf, midi);
            if (head.playing) head.ppq += kBlock * head.bpm / (60.0 * kRate);
        }
    }

    bool idle() const { return proc.audioIdle.load(std::memory_order_relaxed); }

    /** Runs blocks until the instrument is in deep idle; false if it never is. */
    bool waitIdle()
    {
        for (int i = 0; i < kIdleLimit; ++i)
        {
            if (idle()) return true;
            blocks(1);
        }
        return idle();
    }
};

struct Shown
{
    int n = 0;
    std::array<PhraseModel::StepView, PhraseModel::kMaxBarSteps> v {};
};

Shown shown(const T5ynthProcessor& p)
{
    Shown s;
    s.n = p.getPhraseSequencer().readView(s.v);
    return s;
}

bool same(const Shown& a, const Shown& b)
{
    if (a.n != b.n) return false;
    for (int i = 0; i < a.n; ++i)
    {
        const auto& x = a.v[static_cast<size_t>(i)];
        const auto& y = b.v[static_cast<size_t>(i)];
        if (x.note != y.note || x.fn != y.fn || x.head != y.head || x.accent != y.accent
            || x.slide != y.slide)
            return false;
    }
    return true;
}

std::string str(const Shown& s)
{
    std::string r = std::to_string(s.n) + ":";
    for (int i = 0; i < s.n; ++i)
        r += " " + (s.v[static_cast<size_t>(i)].note < 0 ? std::string(".")
                                                          : std::to_string(s.v[static_cast<size_t>(i)].note));
    return r;
}

/** The bar a start enters, read one block after it; then stops again. */
Shown startAndRead(Rig& r)
{
    r.set(PID::seqRunning, 1.0f);
    r.blocks(1);
    const Shown s = shown(r.proc);
    r.set(PID::seqRunning, 0.0f);
    r.blocks(2);
    return s;
}

/** The common shape: idle, change, the view follows, idle again, the start
 *  enters the bar the view showed. */
void changeWhileIdle(const char* name, Rig& r, const std::function<void()>& change)
{
    std::printf("%s\n", name);
    const bool idleBefore = r.waitIdle();
    const Shown before = shown(r.proc);
    change();
    r.blocks(3);
    const Shown stopped = shown(r.proc);
    const bool idleAgain = r.waitIdle();
    const bool stable = same(shown(r.proc), stopped);
    const Shown started = startAndRead(r);
    std::printf("    before  %s\n    stopped %s\n    started %s\n",
                str(before).c_str(), str(stopped).c_str(), str(started).c_str());
    check("  the change was made in deep idle", idleBefore);
    check("  the change changes the bar", ! same(stopped, before));
    check("  the view shows the bar the start enters", same(stopped, started));
    check("  the instrument idles again, the view unchanged", idleAgain && stable);
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    // A Dice draws its seed from the system random and can draw the bar it
    // replaces (10 runs in 300 failed a "changes the bar" check on that):
    // seeded, every run draws the same bars.
    juce::Random::getSystemRandom().setSeed(20261114);

    {
        Rig r;
        changeWhileIdle("Dice while idle", r, [&] { r.proc.phraseDice(); });
    }
    {
        Rig r;
        changeWhileIdle("A phrase parameter while idle (Part)", r,
                        [&] { r.set(PID::phrPart, PhrPart::Lead); });
    }
    {
        Rig r;
        r.proc.phraseDice();
        r.blocks(2);
        const juce::String json = r.proc.exportJsonPreset();
        r.proc.phraseDice();
        r.blocks(2);
        changeWhileIdle("A preset load while idle", r,
                        [&] { r.proc.importJsonPreset(json); });
    }
    {
        Rig r;
        changeWhileIdle("Time signature while idle (3/4)", r, [&] { r.head.num = 3; });
        // A denominator the sequencer does not run in counts as 4: the view
        // follows the numerator and the wake clears.
        changeWhileIdle("Time signature while idle (5/3, read as 5/4)", r,
                        [&] { r.head.num = 5; r.head.den = 3; });
    }
    {
        Rig r;
        r.set(PID::phrHarm, PhrHarm::Keys);
        r.blocks(2);
        changeWhileIdle("Keys chord while idle", r, [&] {
            juce::MidiBuffer on;
            for (int note : { 62, 65, 69 })
                on.addEvent(juce::MidiMessage::noteOn(1, note, 0.8f), 0);
            r.blocks(1, &on);
            juce::MidiBuffer off;
            for (int note : { 62, 65, 69 })
                off.addEvent(juce::MidiMessage::noteOff(1, note), 0);
            r.blocks(1, &off);
        });
    }
    {
        // Evolve reaches the first bar only past the motif's origin: a motif
        // diced in a host's count-in (bar -8, phrase -1 of 8 bars).
        Rig r;
        r.head.playing = true;
        r.head.ppq = -32.0;
        r.set(PID::seqRunning, 1.0f);
        r.blocks(2);
        r.proc.phraseDice();
        r.blocks(2);
        r.set(PID::seqRunning, 0.0f);
        r.head.playing = false;
        r.blocks(2);
        std::printf("    stored origin %d\n", r.proc.getPhraseSequencer().getStoredMotif().origin);
        changeWhileIdle("Evolve while idle, the first bar past the origin", r,
                        [&] { r.set(PID::phrEvolve, 0.0f); });
    }
    {
        std::printf("Stop: the view goes to the first bar\n");
        Rig r;
        r.set(PID::seqRunning, 1.0f);
        r.blocks(250);   // into the second bar at 120 BPM
        const Shown playing = shown(r.proc);
        r.set(PID::seqRunning, 0.0f);
        r.blocks(2);
        const Shown stopped = shown(r.proc);
        const Shown started = startAndRead(r);
        std::printf("    playing %s\n    stopped %s\n    started %s\n",
                    str(playing).c_str(), str(stopped).c_str(), str(started).c_str());
        check("  the bar playing at the stop is not the first", ! same(playing, stopped));
        check("  the view shows the bar the start enters", same(stopped, started));
    }
    {
        std::printf("A motif rendered while stopped is not heard\n");
        Rig r;
        r.waitIdle();
        r.proc.phraseDice();
        r.blocks(3);
        const auto diced = r.proc.getPhraseSequencer().getStoredMotif();
        r.proc.phraseKeep();
        r.blocks(2);
        const auto kept = r.proc.getPhraseSequencer().getStoredMotif();
        r.proc.phraseDice();
        r.blocks(3);
        const auto second = r.proc.getPhraseSequencer().getStoredMotif();
        const bool keptSame = kept.seed == diced.seed && kept.origin == diced.origin
                           && kept.leadInKept == diced.leadInKept;
        std::printf("    origins: diced %d, after Keep %d (leadIn %d), second dice %d\n",
                    diced.origin, kept.origin, kept.leadInKept ? 1 : 0, second.origin);
        check("  the diced motif's origin stays open",
              diced.origin == PhraseModel::kOriginAtNextEntry);
        check("  Keep does nothing", keptSame);
        check("  a second Dice anchors where playback next starts",
              second.origin == PhraseModel::kOriginAtNextEntry);
    }
    {
        std::printf("Phrase -> Step after a Dice while idle\n");
        Rig r;
        const bool idleBefore = r.waitIdle();
        const Shown before = shown(r.proc);
        r.proc.phraseDice();
        r.blocks(3);
        const Shown view = shown(r.proc);
        r.set(PID::seqEngine, SeqEngine::Step);
        r.blocks(2);
        auto& st = r.proc.getStepSequencer();
        bool gridIsView = st.getNumSteps() == view.n;
        std::string grid = std::to_string(st.getNumSteps()) + ":";
        for (int i = 0; i < st.getNumSteps(); ++i)
        {
            const auto& s = st.getStep(i);
            const int note = s.enabled ? s.note : -1;
            grid += " " + (note < 0 ? std::string(".") : std::to_string(note));
            if (i < view.n && note != view.v[static_cast<size_t>(i)].note) gridIsView = false;
        }
        std::printf("    before %s\n    view   %s\n    grid   %s\n",
                    str(before).c_str(), str(view).c_str(), grid.c_str());
        check("  the dice was made in deep idle", idleBefore);
        check("  the dice changes the bar", ! same(view, before));
        check("  the step grid is the bar the view shows", gridIsView);
    }

    {
        // The stopped view shows a motif diced while stopped as heard at the
        // first bar's phrase, but the motif keeps its open origin: a host
        // start resolves it to the phrase the start enters, a count-in's
        // included.
        for (const double startPpq : { -32.0, 32.0 })
        {
            const int expected = startPpq < 0.0 ? -1 : 1;   // phrases of 8 bars
            std::printf("A host start at quarter %.0f after a Dice while idle\n", startPpq);
            Rig r;
            const bool idleBefore = r.waitIdle();
            r.proc.phraseDice();
            r.blocks(3);
            const int stoppedOrigin = r.proc.getPhraseSequencer().getStoredMotif().origin;
            r.head.playing = true;
            r.head.ppq = startPpq;
            r.set(PID::seqRunning, 1.0f);
            r.blocks(2);
            const int heardOrigin = r.proc.getPhraseSequencer().getStoredMotif().origin;
            std::printf("    origin stopped %d, after the start %d (expected %d)\n",
                        stoppedOrigin, heardOrigin, expected);
            check("  the dice was made in deep idle", idleBefore);
            check("  the rendered motif's origin stays open",
                  stoppedOrigin == PhraseModel::kOriginAtNextEntry);
            check("  the start resolves it to the phrase the start enters", heardOrigin == expected);
        }
    }
    {
        // A kept motif carries the motif its phrase was entered from as its
        // lead-in, fitted to the parameters as they are now. Changed while
        // stopped, the view refits it: the bar the view shows is the bar a
        // load of the same state shows. The lead-in reaches the bar only
        // through an embellishment before its first head, and only a change
        // of Part or of the rhythm refits it at all (fitMotif ignores the
        // scale), so the case dices until a load of the same state without
        // the lead-in shows another bar, or it would prove nothing.
        std::printf("Keep with a lead-in, then Part while idle\n");
        bool hasLeadIn = true, idleBefore = true, idleAgain = true, viewIsLoaded = true;
        int reached = 0, attempts = 0;
        for (; attempts < 12 && reached == 0; ++attempts)
        {
            Rig r;
            r.set(PID::phrRotation, 3.0f);   // the bar opens off a head
            // Diced in a host's count-in (phrase -1 of 8 bars), played into
            // phrase 0 and kept there: origin 0, phrase -1's motif the lead-in.
            r.head.playing = true;
            r.head.ppq = -32.0;
            r.set(PID::seqRunning, 1.0f);
            r.blocks(2);
            r.proc.phraseDice();
            while (r.head.ppq < 2.0) r.blocks(1);
            r.proc.phraseKeep();
            r.blocks(2);
            r.set(PID::seqRunning, 0.0f);
            r.head.playing = false;
            r.blocks(2);
            const auto kept = r.proc.getPhraseSequencer().getStoredMotif();
            hasLeadIn = hasLeadIn && kept.leadInKept && kept.origin == 0 && kept.leadInPhrase == 0;
            idleBefore = r.waitIdle() && idleBefore;
            r.set(PID::phrPart, PhrPart::Lead);
            r.blocks(3);
            const Shown view = shown(r.proc);
            idleAgain = r.waitIdle() && idleAgain;

            const juce::String json = r.proc.exportJsonPreset();
            auto loadedView = [](const juce::String& preset) {
                Rig fresh;
                fresh.proc.importJsonPreset(preset);
                fresh.blocks(3);
                return shown(fresh.proc);
            };
            const Shown loaded = loadedView(json);
            juce::var parsed = juce::JSON::parse(json);
            bool stripped = false;
            if (auto* ph = parsed.getProperty("phraseSeq", {}).getDynamicObject())
                if (auto* mo = ph->getProperty("motif").getDynamicObject())
                {
                    stripped = mo->hasProperty("leadIn");
                    mo->removeProperty("leadIn");
                }
            const Shown noLeadIn = loadedView(juce::JSON::toString(parsed));
            if (stripped && ! same(noLeadIn, loaded)) ++reached;
            viewIsLoaded = viewIsLoaded && same(view, loaded);
            std::printf("    view       %s\n    loaded     %s\n    no lead-in %s\n",
                        str(view).c_str(), str(loaded).c_str(), str(noLeadIn).c_str());
        }
        std::printf("    dice: %d\n", attempts);
        check("  the kept motif carries a lead-in into phrase 0", hasLeadIn);
        check("  the change was made in deep idle", idleBefore);
        check("  the instrument idles again", idleAgain);
        check("  the lead-in reaches the bar (a load without it differs)", reached > 0);
        check("  the view is the bar a load of the same state shows", viewIsLoaded);
    }
    {
        // A change and Phrase -> Step in one block: the phrase section, which
        // brings the stopped view up to date, does not run in a block that
        // switches away from Phrase, so the switch must take the change into
        // the view before it copies it. The oracle is the view the phrase
        // sequencer shows once switched back (its config reads no step
        // parameter the switch sets).
        auto switchInBlockOfChange = [](const char* name, bool fromIdle,
                                        const std::function<void(Rig&)>& change) {
            std::printf("%s\n", name);
            Rig r;
            const bool idleBefore = ! fromIdle || r.waitIdle();
            const Shown before = shown(r.proc);
            change(r);
            r.set(PID::seqEngine, SeqEngine::Step);
            r.blocks(2);
            auto& st = r.proc.getStepSequencer();
            Shown grid;
            grid.n = st.getNumSteps();
            for (int i = 0; i < grid.n && i < PhraseModel::kMaxBarSteps; ++i)
            {
                const auto& s = st.getStep(i);
                grid.v[static_cast<size_t>(i)].note = s.enabled ? s.note : -1;
            }
            r.set(PID::seqEngine, SeqEngine::Phrase);
            r.blocks(3);
            const Shown back = shown(r.proc);
            bool gridIsView = grid.n == back.n;
            for (int i = 0; gridIsView && i < back.n; ++i)
                gridIsView = grid.v[static_cast<size_t>(i)].note == back.v[static_cast<size_t>(i)].note;
            std::printf("    before %s\n    grid   %s\n    view   %s\n",
                        str(before).c_str(), str(grid).c_str(), str(back).c_str());
            if (fromIdle) check("  the change was made in deep idle", idleBefore);
            check("  the change changes the bar", ! same(back, before));
            check("  the step grid is the bar the view shows after the change", gridIsView);
        };
        switchInBlockOfChange("Dice and Phrase -> Step in one block, idle", true,
                              [](Rig& r) { r.proc.phraseDice(); });
        switchInBlockOfChange("Dice and Phrase -> Step in one block, awake", false,
                              [](Rig& r) { r.proc.phraseDice(); });
        switchInBlockOfChange("Part and Phrase -> Step in one block, idle", true,
                              [](Rig& r) { r.set(PID::phrPart, PhrPart::Lead); });
        switchInBlockOfChange("3/4 and Phrase -> Step in one block, idle", true,
                              [](Rig& r) { r.head.num = 3; });
        // PLAY in the same block: the sequencer has not started yet, the
        // view it shows is still the stopped one, and the switch copies it
        // with the change (the transfer asks the sequencer, not PLAY).
        switchInBlockOfChange("Dice, PLAY and Phrase -> Step in one block, idle", true,
                              [](Rig& r) { r.proc.phraseDice(); r.set(PID::seqRunning, 1.0f); });
    }

    std::printf("failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
