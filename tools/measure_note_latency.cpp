// How long after a note-on does each engine actually make sound?
//
// Drives the REAL T5ynthProcessor (not a rig, not a mirror) exactly as a host
// does: prepareToPlay, then processBlock with a note-on at a known sample
// offset, then reads the output back. Everything outside the plugin — audio
// driver, buffer size, MIDI transport — is identical for every engine mode, so
// whatever this tool reports IS the whole of the plugin's own contribution to
// the latency a player feels, and a difference between two modes here is the
// only difference the plugin itself can be responsible for.
//
// Reported per mode, all relative to the note-on sample:
//   onset   first sample above -80 dBFS
//   t10/t50 first sample at 10 % / 50 % of the STEADY level (median of a late
//           window, so a detuned stack's beat cannot fake a slow attack)
//
// The neural engines (Sampler/Wavetable) are fed a synthetic "generated" buffer
// with an instant onset via loadGeneratedAudio(), so their numbers are a floor,
// not a property of any particular generated sound. The LCO/LRO runs whatever
// orchestra is compiled — the built-in one unless a path is passed.
//
// One reading of this tool needs its condition stated with it: the sampler's
// pitch shifter is only late when its start point has no audio in front of it
// to prime the STFT with. loadGeneratedAudio() trims leading silence and then
// auto-positions P1 at the first active window, which puts P1 at ~0 — which is
// the state the scenario table below measures. The dedicated P1 sweep further
// down is what says how much of that is the shifter and how much is the
// missing pre-roll; do not quote the table's sampler row without it.
//
// Build (same response-file recipe as audition_csound_swap.cpp):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSOUND_FW="$(brew --prefix csound)/Frameworks"
//   clang++ -std=c++17 -O2 @/tmp/h.rsp \
//     tools/measure_note_latency.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -F"$CSOUND_FW" -framework CsoundLib64 -Wl,-rpath,"$CSOUND_FW" \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit \
//     -o tools/measure_note_latency
//   tools/measure_note_latency [sampleRate] [blockSize] [orchestra.csd]

// CoreFoundation FIRST: JuceHeader.h pulls in `using namespace juce`, after
// which MacTypes.h's `struct Point` collides with juce::Point.
#include <CoreFoundation/CoreFoundation.h>

#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    double gSampleRate = 96000.0;
    int    gBlockSize  = 512;
    juce::String gOrchestraPath;

    // MessageManager::runDispatchLoopUntil only exists when JUCE_MODAL_LOOPS_
    // PERMITTED is on, and it is off in this build — so pump the run loop the
    // JUCE message queue actually posts to (juce_osx_MessageQueue installs a
    // CFRunLoopSource in kCFRunLoopCommonModes). Async updates, timers and the
    // background orchestra compile's completion callback all land here.
    void pump (int ms)
    {
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, (double) ms * 0.001, false);
    }

    void setParam (T5ynthProcessor& proc, const char* id, float realValue)
    {
        auto& s = proc.getValueTreeState();
        if (auto* p = s.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (realValue));
        else
            std::printf ("  !! no such parameter: %s\n", id);
    }

    // A synthetic stand-in for a generated sample: an instantly-starting bright
    // tone. Its job is to give the neural engines a signal whose own onset is
    // provably at sample 0, so their reading is the ENGINE's latency and not
    // the attack of whatever happened to be generated last.
    juce::AudioBuffer<float> instantOnsetSource (double sr)
    {
        const int n = (int) (sr * 2.0);
        juce::AudioBuffer<float> b (1, n);
        auto* w = b.getWritePointer (0);
        const double f0 = 220.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            double v = 0.0;
            for (int h = 1; h <= 12; ++h)                 // band-limited saw
                v += std::sin (2.0 * juce::MathConstants<double>::pi * f0 * h * t) / h;
            w[i] = (float) (0.5 * v / 1.6);
        }
        return b;
    }

    struct LatResult { int onset = -1, t10 = -1, t50 = -1; float steady = 0.0f, peak = 0.0f; };

    // Render `note` at block boundary + `offsetInBlock`, capture what follows.
    LatResult measure (T5ynthProcessor& proc, int note, int offsetInBlock, int captureBlocks,
                       int velocity = 100)
    {
        juce::AudioBuffer<float> buf (2, gBlockSize);
        juce::MidiBuffer midi;

        // Settle. Long enough to outlast the master limiter's 200 ms release —
        // an earlier version used 8 blocks (43 ms at 96k/512), so every row
        // inherited the gain reduction the LOUDEST previous row had left behind.
        const int settleBlocks = juce::jmax (8, (int) (gSampleRate * 0.4 / gBlockSize));
        for (int b = 0; b < settleBlocks; ++b)
        {
            buf.clear();
            midi.clear();
            proc.processBlock (buf, midi);
        }

        std::vector<float> cap;
        cap.reserve ((size_t) (captureBlocks * gBlockSize));

        for (int b = 0; b < captureBlocks; ++b)
        {
            buf.clear();
            midi.clear();
            if (b == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) velocity),
                               offsetInBlock);
            proc.processBlock (buf, midi);

            const auto* l = buf.getReadPointer (0);
            const int from = (b == 0 ? offsetInBlock : 0);   // t=0 IS the note-on sample
            for (int i = from; i < gBlockSize; ++i)
                cap.push_back (l[i]);
        }

        // Release, and let the tail actually die before the next row starts.
        for (int b = 0; b < settleBlocks; ++b)
        {
            buf.clear();
            midi.clear();
            if (b == 0) midi.addEvent (juce::MidiMessage::noteOff (1, note), 0);
            proc.processBlock (buf, midi);
        }

        // 1 ms rectified envelope, then a MEDIAN over a late window as the
        // reference level. Referencing the rise to the peak of the first 300 ms
        // (what this tool did first) turns a slow BEAT into a fake slow attack:
        // detuned stacks — including the built-in orchestra's 2.000/2.007
        // partial pair, which beats with a ~0.5 s period — simply start in a
        // trough, and the "peak" is wherever the beat happened to crest. A
        // median cannot be moved by that.
        const int win = juce::jmax (1, (int) (gSampleRate * 0.001));
        std::vector<float> env;
        env.reserve (cap.size());
        double acc = 0.0;
        for (size_t i = 0; i < cap.size(); ++i)
        {
            acc += std::abs (cap[i]);
            if (i >= (size_t) win) acc -= std::abs (cap[i - (size_t) win]);
            env.push_back ((float) (acc / win));
        }

        LatResult r;
        for (float v : env) r.peak = juce::jmax (r.peak, v);

        const int lo = juce::jmin ((int) env.size(), (int) (gSampleRate * 0.25));
        const int hi = juce::jmin ((int) env.size(), (int) (gSampleRate * 0.45));
        if (hi > lo)
        {
            std::vector<float> late (env.begin() + lo, env.begin() + hi);
            std::nth_element (late.begin(), late.begin() + late.size() / 2, late.end());
            r.steady = late[late.size() / 2];
        }
        // A decaying sound has no steady level; fall back to its peak so the
        // thresholds below still mean "a fraction of what this note IS".
        const float ref = r.steady > 1.0e-4f ? r.steady : r.peak;

        const float absFloor = 1.0e-4f;                       // -80 dBFS
        for (int i = 0; i < (int) env.size(); ++i)
        {
            const float a = env[(size_t) i];
            if (r.onset < 0 && a > absFloor)          r.onset = i;
            if (r.t10  < 0 && a > 0.10f * ref)        r.t10   = i;
            if (r.t50  < 0 && a > 0.50f * ref)        { r.t50 = i; break; }
        }
        return r;
    }

    // What one processBlock costs, against the wall-clock the audio device
    // allows for it. A mode that eats most of the buffer period has no margin
    // left for the GUI and the rest of the machine, and the callback starts
    // arriving late — which a player feels as latency even though every number
    // above says the note starts on time.
    void measureLoad (T5ynthProcessor& proc, const char* label, int heldNotes)
    {
        juce::AudioBuffer<float> buf (2, gBlockSize);
        juce::MidiBuffer midi;

        for (int n = 0; n < heldNotes; ++n)
        {
            buf.clear();
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOn (1, 48 + n * 3, (juce::uint8) 100), 0);
            proc.processBlock (buf, midi);
        }
        for (int b = 0; b < 20; ++b) { buf.clear(); midi.clear(); proc.processBlock (buf, midi); }

        const int n = 400;
        double total = 0.0, worst = 0.0;
        for (int b = 0; b < n; ++b)
        {
            buf.clear();
            midi.clear();
            const auto t0 = juce::Time::getHighResolutionTicks();
            proc.processBlock (buf, midi);
            const double us = juce::Time::highResolutionTicksToSeconds (
                                  juce::Time::getHighResolutionTicks() - t0) * 1.0e6;
            total += us;
            worst = juce::jmax (worst, us);
        }

        for (int n2 = 0; n2 < heldNotes; ++n2)
        {
            buf.clear();
            midi.clear();
            midi.addEvent (juce::MidiMessage::noteOff (1, 48 + n2 * 3), 0);
            proc.processBlock (buf, midi);
        }
        for (int b = 0; b < 40; ++b) { buf.clear(); midi.clear(); proc.processBlock (buf, midi); }

        const double budgetUs = 1.0e6 * gBlockSize / gSampleRate;
        std::printf ("%-22s  %2d notes   mean %7.1f us (%5.1f %% of block)   worst %7.1f us (%5.1f %%)\n",
                     label, heldNotes, total / n, 100.0 * (total / n) / budgetUs,
                     worst, 100.0 * worst / budgetUs);
    }

    void report (const char* label, const LatResult& r)
    {
        auto ms = [] (int s) { return s < 0 ? -1.0 : 1000.0 * s / gSampleRate; };
        if (r.peak <= 1.0e-4f)
        {
            std::printf ("%-22s   SILENT (peak %.2e)\n", label, r.peak);
            return;
        }
        std::printf ("%-22s  onset %7.2f ms   t10 %7.2f ms   t50 %7.2f ms   steady %.3f  peak %.3f\n",
                     label, ms (r.onset), ms (r.t10), ms (r.t50), r.steady, r.peak);
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc > 1) gSampleRate = std::atof (argv[1]);
    if (argc > 2) gBlockSize  = std::atoi (argv[2]);
    if (argc > 3) gOrchestraPath = juce::String (juce::CharPointer_UTF8 (argv[3]));

    std::printf ("note-on -> sound, measured through the real T5ynthProcessor\n");
    std::printf ("sample rate %.0f Hz, block %d samples (%.2f ms)\n\n",
                 gSampleRate, gBlockSize, 1000.0 * gBlockSize / gSampleRate);

    T5ynthProcessor proc;
    // A host sets these before prepareToPlay; the background orchestra compile
    // reads getSampleRate()/getBlockSize(), so without this it would compile at
    // 0 Hz and the LRO would never sound.
    proc.setRateAndBufferSizeDetails (gSampleRate, gBlockSize);
    proc.prepareToPlay (gSampleRate, gBlockSize);
    pump (200);

    // Fastest possible envelope: what is left is the ENGINE's own onset, not
    // the ADSR's (which is shared by every mode anyway).
    setParam (proc, PID::ampAttack,  0.0f);
    setParam (proc, PID::ampDecay,   50.0f);
    setParam (proc, PID::ampSustain, 1.0f);
    setParam (proc, PID::ampRelease, 50.0f);
    pump (50);

    proc.loadGeneratedAudio (instantOnsetSource (gSampleRate), gSampleRate);
    pump (500);

    const int captureBlocks = juce::jmax (4, (int) (gSampleRate * 0.5 / gBlockSize));

    struct Mode { const char* label; int engineMode; };
    const Mode neural[] = { { "Sampler (T5osc)", 0 }, { "Wavetable (T5osc)", 1 } };

    // LCO / LRO. forceCsoundEngineMode is the same entry point the prompt path
    // uses; the orchestra compiles on a background thread, so pump until it
    // sounds rather than guessing a fixed wait. With a .csd argument the
    // measurement runs on THAT orchestra — the one actually being played —
    // instead of the built-in fallback, via the same requestCsoundOrchestra()
    // call the prompt path makes.
    proc.forceCsoundEngineMode();
    if (gOrchestraPath.isNotEmpty())
    {
        const juce::File f (gOrchestraPath);
        const juce::String text = f.loadFileAsString();
        if (text.isEmpty())
            std::printf ("  !! could not read orchestra: %s\n", gOrchestraPath.toRawUTF8());
        else if (! proc.requestCsoundOrchestra (text))
            std::printf ("  !! requestCsoundOrchestra refused the text\n");
        else
            std::printf ("orchestra: %s (%d chars)\n\n",
                         f.getFileName().toRawUTF8(), text.length());
        pump (3000);   // background compile + prime, then the swap crossfade
    }
    // Wait for the compile by its RESULT (the engine sounds), not by a guessed
    // duration — a fixed sleep that is too short reports "the LRO is silent",
    // which reads as a finding rather than as a tool that measured too early.
    for (int attempt = 0; attempt < 30; ++attempt)
    {
        pump (300);
        // Ready means sound IN THE FIRST TWO BLOCKS, not sound somewhere in the
        // capture: a capture that begins mid-compile would otherwise be accepted
        // and its huge t10 reported as if it were the engine's latency.
        const LatResult probe = measure (proc, 60, 0, 2);
        if (probe.peak > 0.02f) break;
    }

    // ── the three scenarios, each varying exactly ONE thing ──
    // An earlier version changed the note AND the offset between two rows and
    // labelled the pair "mid-block", which made the sampler's pitch-shifter
    // latency (below) look like a block-alignment effect.
    struct Scenario { const char* label; int note; int offset; };
    const int kMisaligned = gBlockSize / 2 + 31;   // deliberately NOT a multiple
                                                   // of kKsmps: the Csound bridge
                                                   // can only act on a control-
                                                   // block boundary, and 256 is
                                                   // already one, so the aligned
                                                   // offset hides the whole cost.
    const Scenario scenarios[] = {
        { "note-on at block start, ROOT note (C4)",              60, 0 },
        { "note-on at block start, TRANSPOSED note (G4)",        67, 0 },
        { "note-on mid-block (offset not ksmps-aligned), C4",    60, kMisaligned },
    };

    for (const auto& sc : scenarios)
    {
        std::printf ("\n%s:\n", sc.label);
        for (const auto& m : neural)
        {
            setParam (proc, PID::engineMode, (float) m.engineMode);
            pump (200);
            report (m.label, measure (proc, sc.note, sc.offset, captureBlocks));
        }
        proc.forceCsoundEngineMode();
        pump (300);
        report (gOrchestraPath.isNotEmpty() ? "LRO (authored)" : "LRO (built-in orch)",
                measure (proc, sc.note, sc.offset, captureBlocks));
    }

    // ── the sampler's pitch shifter, against the pre-roll it is given ──
    // primeStretcher() pays the STFT latency in two steps: seek() fills the
    // analysis context with audio from BEFORE the start point, then a
    // process()+discard of inputLatency() samples. Step one can only read what
    // is THERE — `seekStart = readPosition - seekLen*srRatio`, and a negative
    // seekStart clamps seekLen down to whatever fits (SamplePlayer.cpp:1093).
    // With P1 at the very start of the buffer there is no pre-roll at all and
    // the unpaid remainder shows up as latency, on transposed notes only (the
    // root takes the nearUnity bypass and never enters the stretcher).
    //
    // This sweep is here because the first version of this tool measured only
    // P1 = 0 and reported its 46 ms as a property of the sampler.
    std::printf ("\ntransposed note (G4), sampler only, vs. source audio before P1:\n");
    {
        setParam (proc, PID::engineMode, 0.0f);   // Sampler
        pump (200);
        proc.getSampler().setPointsLocked (true); // keep our P1, don't re-auto-position
        const float p1s[] = { 0.0f, 0.005f, 0.02f, 0.05f, 0.07f, 0.15f };
        for (float p1 : p1s)
        {
            proc.getSampler().setStartPos (p1);
            pump (100);
            char label[64];
            std::snprintf (label, sizeof (label), "P1 = %.3f  (%.0f ms before)",
                           p1, p1 * 2000.0);   // the synthetic source is 2 s long
            report (label, measure (proc, 67, 0, captureBlocks));
        }
        proc.getSampler().setStartPos (0.0f);
        proc.getSampler().setPointsLocked (false);
        pump (100);
    }

    // ── does velocity reach the output the same way in every mode? ──
    // The orchestra multiplies its own output by the vel channel AND the voice
    // envelope's peak already tracks velocity, so if the LRO scales as vel^2
    // where the neural engines scale as vel, soft playing is quieter on the LRO
    // by exactly that difference — and a quiet onset is heard as a late one.
    std::printf ("\npeak vs MIDI velocity (same note, same envelope):\n");
    std::printf ("%-22s %9s %9s %9s   ratio 100:30\n", "", "vel 30", "vel 60", "vel 100");
    auto velRow = [&] (const char* label)
    {
        const float p30  = measure (proc, 60, 0, captureBlocks,  30).peak;
        const float p60  = measure (proc, 60, 0, captureBlocks,  60).peak;
        const float p100 = measure (proc, 60, 0, captureBlocks, 100).peak;
        std::printf ("%-22s %9.4f %9.4f %9.4f   %6.2f  (vel^1 = %.2f)\n",
                     label, p30, p60, p100, p30 > 0 ? p100 / p30 : 0.0, 100.0 / 30.0);
    };
    for (const auto& m : neural)
    {
        setParam (proc, PID::engineMode, (float) m.engineMode);
        pump (200);
        velRow (m.label);
    }
    proc.forceCsoundEngineMode();
    pump (300);
    velRow (gOrchestraPath.isNotEmpty() ? "LRO (authored)" : "LRO (built-in orch)");

    // ── audio-thread cost, the other way a mode can arrive late ──
    std::printf ("\nprocessBlock cost (budget %.2f ms per block):\n",
                 1000.0 * gBlockSize / gSampleRate);
    for (const auto& m : neural)
    {
        setParam (proc, PID::engineMode, (float) m.engineMode);
        pump (200);
        measureLoad (proc, m.label, 0);
        measureLoad (proc, m.label, 1);
        measureLoad (proc, m.label, 8);
    }
    proc.forceCsoundEngineMode();
    pump (300);
    const char* lroLabel = gOrchestraPath.isNotEmpty() ? "LRO (authored)" : "LRO (built-in orch)";
    measureLoad (proc, lroLabel, 0);   // 0 notes = processBlock's idle early-out,
    measureLoad (proc, lroLabel, 1);   // NOT "Csound costs nothing at rest": the
    measureLoad (proc, lroLabel, 8);   // 16 always-on instruments run as soon as
                                       // ANY voice (or tail) is alive, which is
                                       // why 1 and 8 notes cost nearly the same.

    return 0;
}
