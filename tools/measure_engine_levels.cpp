// What level does each engine actually leave the voice chain at, and how does
// that grow with polyphony?
//
// Two numbers are needed before the output gain can be set, and neither was
// known: whether the four engines are level-matched to each other, and what a
// full chord costs over a single note. Voices at different pitches add
// INCOHERENTLY, so the chord factor is nearer sqrt(N) than N -- but "nearer" is
// not a number, and VoiceManager's own 1/N^0.1 compensation is folded into it,
// so it is measured here rather than assumed.
//
// The patch is deliberately NEUTRAL: filter OFF, one amp envelope holding at
// sustain, no mod envelope pointed anywhere, no LFO, no drift, no effects,
// velocity taken out of the picture. What is left is the engine and the DCA.
//
// The reading is referred BACK THROUGH the static output gain, so the column
// that matters is what the VOICE CHAIN produces -- the thing an engine trim
// would act on -- and not what today's master setting happens to make of it.
//
// The LRO does not render reproducibly (tools/measure_poly_independence.cpp
// found up to 8.6 dB of run-to-run variation in the second partial of one held
// note), so every configuration is rendered three times and the MEDIAN peak is
// reported. The spread is printed with it: a number whose three runs disagree
// is not a level, and the reader has to be able to see that.
//
// Build (same response-file recipe as tools/measure_note_latency.cpp):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSOUND_FW="$(brew --prefix csound)/Frameworks"
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/measure_engine_levels.cpp \
//     build_clean/T5ynth_artefacts/Release/libakroasys_SharedCode.a \
//     build_clean/libT5ynthData.a \
//     -F"$CSOUND_FW" -framework CsoundLib64 -Wl,-rpath,"$CSOUND_FW" \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o tools/measure_engine_levels
//   tools/measure_engine_levels [sampleRate] [blockSize]

#include <CoreFoundation/CoreFoundation.h>

#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    double gSampleRate = 48000.0;
    int    gBlockSize  = 512;

    constexpr double kHoldS    = 1.6;   // long enough to settle past the attack
    constexpr double kWindowLo = 0.9;   // the steady window
    constexpr double kWindowHi = 1.5;
    constexpr int    kRuns     = 3;

    // Spread and deliberately not stacked in octaves or fifths: coincident
    // partials would add COHERENTLY and report a chord factor that only that
    // chord has.
    const int kChordNotes[] = { 48, 51, 55, 58, 62, 65, 69, 72,
                                76, 79, 83, 86, 90, 93, 97, 100 };

    // The 64-voice switch position cannot be reached by that array, and cannot
    // be reached by a hand either -- 64 notes at once is a sequencer or MPE
    // texture. A chromatic block from C2 up is the honest stand-in: at that
    // density the pitches necessarily share partials, which is what such a
    // texture actually does.
    int chordNoteAt (int n)
    {
        if (n < 16) return kChordNotes[n];
        return 36 + n;                       // 52 .. 99 for n = 16 .. 63
    }

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

    juce::AudioBuffer<float> source (double sr)
    {
        const int n = (int) (sr * 2.0);
        juce::AudioBuffer<float> b (1, n);
        auto* w = b.getWritePointer (0);
        const double f0 = 220.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            double v = 0.0;
            for (int h = 1; h <= 24; ++h)
                v += std::sin (2.0 * juce::MathConstants<double>::pi * f0 * h * t) / h;
            w[i] = (float) (0.5 * v / 1.9);
        }
        return b;
    }

    struct Level { float peak = 0.0f; float rms = 0.0f; };

    Level renderChord (T5ynthProcessor& proc, int numNotes)
    {
        proc.prepareToPlay (gSampleRate, gBlockSize);
        pump (60);

        const int total = (int) (gSampleRate * kHoldS);
        const int lo    = (int) (gSampleRate * kWindowLo);
        const int hi    = (int) (gSampleRate * kWindowHi);

        juce::AudioBuffer<float> buf (2, gBlockSize);
        juce::MidiBuffer midi;

        Level out;
        double acc = 0.0;
        int    accN = 0;

        for (int pos = 0; pos < total; pos += gBlockSize)
        {
            buf.clear();
            midi.clear();
            if (pos == 0)
                for (int n = 0; n < numNotes; ++n)
                    midi.addEvent (juce::MidiMessage::noteOn (1, chordNoteAt (n),
                                                              (juce::uint8) 100), 0);
            proc.processBlock (buf, midi);

            const auto* l = buf.getReadPointer (0);
            for (int i = 0; i < gBlockSize && pos + i < total; ++i)
            {
                const int at = pos + i;
                if (at < lo || at >= hi) continue;
                out.peak = juce::jmax (out.peak, std::abs (l[i]));
                acc += (double) l[i] * l[i];
                ++accN;
            }
        }
        out.rms = accN > 0 ? (float) std::sqrt (acc / accN) : 0.0f;

        for (int b = 0; b < 40; ++b)
        {
            buf.clear();
            midi.clear();
            if (b == 0)
                for (int n = 0; n < numNotes; ++n)
                    midi.addEvent (juce::MidiMessage::noteOff (1, chordNoteAt (n)), 0);
            proc.processBlock (buf, midi);
        }
        return out;
    }

    struct Engine { const char* label; int mode; bool csound; };

    // Median of kRuns, with the spread, for the reason in the header.
    Level medianLevel (T5ynthProcessor& proc, int numNotes, float* spreadDbOut)
    {
        std::vector<Level> runs;
        for (int r = 0; r < kRuns; ++r) runs.push_back (renderChord (proc, numNotes));

        std::vector<float> peaks;
        for (const auto& l : runs) peaks.push_back (l.peak);
        std::sort (peaks.begin(), peaks.end());

        const float lo = peaks.front(), hi = peaks.back();
        *spreadDbOut = (lo > 1.0e-9f && hi > 1.0e-9f)
                     ? 20.0f * std::log10 (hi / lo) : 0.0f;

        std::vector<float> rmss;
        for (const auto& l : runs) rmss.push_back (l.rms);
        std::sort (rmss.begin(), rmss.end());

        Level out;
        out.peak = peaks[peaks.size() / 2];
        out.rms  = rmss[rmss.size() / 2];
        return out;
    }

    void selectEngine (T5ynthProcessor& proc, const Engine& e)
    {
        if (! e.csound)
        {
            setParam (proc, PID::engineMode, (float) e.mode);
            pump (250);
            return;
        }

        proc.forceCsoundEngineMode();
        // Wait for the compile by its RESULT, as measure_note_latency does: a
        // fixed sleep that is too short reports "the LRO is silent", which reads
        // as a finding rather than as a tool that measured too early.
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            pump (300);
            float ignored = 0.0f;
            if (medianLevel (proc, 1, &ignored).peak > 0.005f)
                break;
        }
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc > 1) gSampleRate = std::atof (argv[1]);
    if (argc > 2) gBlockSize  = std::atoi (argv[2]);

    T5ynthProcessor proc;
    proc.setRateAndBufferSizeDetails (gSampleRate, gBlockSize);
    proc.prepareToPlay (gSampleRate, gBlockSize);
    pump (200);

    proc.loadGeneratedAudio (source (gSampleRate), gSampleRate);
    pump (600);

    setParam (proc, PID::voiceCount, 6.0f);          // 64 -- the widest switch position,
                                                     // so every chord below fits under it

    setParam (proc, PID::ampAttack,  5.0f);
    setParam (proc, PID::ampDecay,   100.0f);
    setParam (proc, PID::ampSustain, 1.0f);
    setParam (proc, PID::ampRelease, 100.0f);
    setParam (proc, PID::ampAmount,  1.0f);
    setParam (proc, PID::ampTarget,  (float) EnvTarget::DCA);
    setParam (proc, PID::velAmt,     0.0f);

    for (int m = 0; m < kNumModEnvs; ++m)
        setParam (proc, PID::modEnv[m].target, (float) EnvTarget::None);

    setParam (proc, PID::filterType, 0.0f);          // OFF -- the engine, bare
    setParam (proc, PID::lfo1Depth,  0.0f);
    setParam (proc, PID::lfo2Depth,  0.0f);
    setParam (proc, PID::lfo3Depth,  0.0f);
    setParam (proc, PID::driftEnabled, 0.0f);
    setParam (proc, PID::delayMix,   0.0f);
    setParam (proc, PID::reverbMix,  0.0f);

    setParam (proc, PID::masterVol,     0.0f);
    setParam (proc, PID::limiterThresh, -3.0f);      // the shipped default
    pump (150);

    // What the master stage multiplies by (one gain at every voice-count
    // position), so table A below can be read back to the voice chain. Restated here rather
    // than taken from the processor, because the tool must not depend on a
    // private helper -- and must NOT be kept in step silently either: if this
    // number and PluginProcessor's kOutputGain disagree, table A is wrong and the
    // reader has to be able to see which one moved. Table B needs no such
    // number; it reads the buffer as the host receives it.
    const double outGain = 1.6;   // kOutputGain, the same at every switch position

    std::printf ("engine levels, neutral patch (filter OFF, one amp envelope at"
                 " sustain, nothing else)\n");
    std::printf ("sample rate %.0f Hz, block %d, velocity 100, %d runs per cell"
                 " (median)\n", gSampleRate, gBlockSize, kRuns);
    std::printf ("output gain x%.4f\n\n", outGain);

    const Engine engines[] = {
        { "Sampler",        EngineMode::Sampler,   false },
        { "Wavetable",      EngineMode::Wavetable, false },
        { "Granular",       EngineMode::Freeze,    false },
        { "LRO (built-in)", EngineMode::Csound,    true  },
    };
    // The SWITCH positions the UI offers (SynthPanel.h: 7 buttons, Mono..64).
    const int chords[] = { 1, 4, 6, 8, 12, 16, 64 };

    // ── Table 1: the voice chain, i.e. what the calibration acts ON ────────
    std::printf ("A. VOICE CHAIN -- peak before the output gain, voice switch"
                 " parked at 64 so that\n   every chord fits under it. This is"
                 " what the output gain's knee figures are read from.\n\n");
    std::printf ("%-16s", "");
    for (int c : chords) std::printf ("  %5d", c);
    std::printf ("   notes held\n");

    for (const auto& e : engines)
    {
        selectEngine (proc, e);
        std::printf ("%-16s", e.label);
        double first = 0.0, last = 0.0;
        float worstSpread = 0.0f;
        for (int c : chords)
        {
            float spread = 0.0f;
            const Level l = medianLevel (proc, c, &spread);
            const double v = l.peak / outGain;
            if (c == 1) first = v;
            last = v;
            worstSpread = juce::jmax (worstSpread, spread);
            std::printf ("  %5.3f", v);
        }
        if (first > 1.0e-6 && last > 1.0e-6)
            std::printf ("   %+5.1f dB over one note", 20.0 * std::log10 (last / first));
        if (worstSpread > 0.5f)
            std::printf ("   (spread up to %.1f dB)", worstSpread);
        std::printf ("\n");
    }

    // ── Table 2: the OUTPUT, with the switch set where the chord says ──────
    //
    // Each cell sets the voice-count switch to that position AND plays a chord
    // that fills it, then reads the buffer the host receives. With one output
    // gain at every position this is table A times that gain, and what it
    // checks is exactly that: B / A has to be the same number in every cell, or
    // the switch is moving the level again. It also shows which chords pass the
    // ceiling's 0.9 knee -- from four notes on Wavetable and Sampler at x1.6,
    // the cost PluginProcessor.cpp's outputGainForThreshold states.
    std::printf ("\nB. OUTPUT -- voice switch AT that position, chord filling it."
                 " The knee is 0.90.\n\n");
    std::printf ("%-16s", "");
    for (int c : chords) std::printf ("  %5d", c);
    std::printf ("   switch / notes\n");

    for (const auto& e : engines)
    {
        selectEngine (proc, e);
        std::printf ("%-16s", e.label);
        for (int k = 0; k < (int) (sizeof (chords) / sizeof (chords[0])); ++k)
        {
            setParam (proc, PID::voiceCount, (float) k);
            pump (60);
            float spread = 0.0f;
            std::printf ("  %5.3f", medianLevel (proc, chords[k], &spread).peak);
        }
        std::printf ("\n");
    }
    setParam (proc, PID::voiceCount, 6.0f);

    std::printf ("\nA is the voice chain the output gain in PluginProcessor.cpp acts"
                 " on; B is the output\nat that gain, the same at every switch"
                 " position. The LRO caps at 16 voices"
                 " (CsoundEngine::kMaxVoices), so its 64 column\nis 16 voices"
                 " playing a denser cluster, not 64.\n");
    return 0;
}
