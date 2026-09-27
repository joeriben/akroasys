// Does a SNAP recall crossfade a held sampler note, or hard-swap it?
//
// tools/audition_sampler_follow.cpp already proves SamplePlayer's own
// morphToBufferFrom is a click-free equal-power crossfade. This tool asks the
// next question up: whether the path a SNAP button actually takes -- the one in
// MainPanel::restoreMainSnapshot -- still arrives at that crossfade, or whether
// something on the way breaks it. It drives the REAL processor, in real time
// (so the background sampler-reprepare thread runs at its own pace, as it does
// in the app), and reproduces the recall call for call:
//
//     lock { applyMarkers() }                 // the slot's loop/start points
//     loadGeneratedAudio(slot.audio, sr)      // the slot's sample
//     lock { applyMarkers(); setPointsLocked() }
//
// A plain regenerate (GENERATE) calls only the middle line, which is why the
// invariant guard passes while a recall can still click.
//
// Not reproduced: the Snap fade (T5ynthProcessor::snapFadeOut/snapFadeIn) that
// activateSnapshot now puts around the recall's parameter writes and the first
// applyMarkers. The sample swap measured here comes after the fade-in, where it
// still has to crossfade on its own, so this remains its test; it says nothing
// about the fade itself.
//
// Metric is audition_sampler_follow's: the largest sample-to-sample step in the
// crossfade window against the material's own natural step. WAVs are written so
// the seam can be heard rather than only read.
//
// Build (response-file recipe as in tools/measure_engine_levels.cpp):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSOUND_FW="$(brew --prefix csound)/Frameworks"
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/audition_snapshot_switch.cpp \
//     build_clean/T5ynth_artefacts/Release/libakroasys_SharedCode.a \
//     build_clean/libT5ynthData.a \
//     -F"$CSOUND_FW" -framework CsoundLib64 -Wl,-rpath,"$CSOUND_FW" \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o tools/audition_snapshot_switch
// Exits non-zero if any case is not a click-free, actually-transitioning recall.

#include <CoreFoundation/CoreFoundation.h>

#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    double gSampleRate = 48000.0;
    int    gBlockSize  = 512;

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

    void writeWav (const std::string& path, const std::vector<float>& mono, int sr)
    {
        std::ofstream f (path, std::ios::binary);
        auto u32 = [&](uint32_t v){ f.write (reinterpret_cast<char*> (&v), 4); };
        auto u16 = [&](uint16_t v){ f.write (reinterpret_cast<char*> (&v), 2); };
        uint32_t dataBytes = (uint32_t) mono.size() * 2u;
        f.write ("RIFF", 4); u32 (36 + dataBytes); f.write ("WAVE", 4);
        f.write ("fmt ", 4); u32 (16); u16 (1); u16 (1); u32 (sr); u32 (sr * 2); u16 (2); u16 (16);
        f.write ("data", 4); u32 (dataBytes);
        for (float s : mono)
        {
            int v = (int) std::lround (std::max (-1.0f, std::min (1.0f, s)) * 32767.0f);
            u16 ((uint16_t) (int16_t) v);
        }
    }

    // Two clearly different samples, so a hard swap cannot hide in the material.
    juce::AudioBuffer<float> tone (double sr, double f0, float amp)
    {
        const int n = (int) (sr * 2.0);
        juce::AudioBuffer<float> b (1, n);
        auto* w = b.getWritePointer (0);
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            double v = 0.0;
            for (int h = 1; h <= 12; ++h)
                v += std::sin (2.0 * juce::MathConstants<double>::pi * f0 * h * t) / h;
            w[i] = (float) (amp * v / 1.9);
        }
        return b;
    }

    // MainPanel::restoreMainSnapshot's marker block, verbatim in structure.
    // `openFirst` is the sequence the recall used to run -- open the region to
    // 1.0, then move both brackets -- which marks the master for a re-prepare
    // even when the pair ends where it already was. Kept as a case, because the
    // held voice must survive a second publication either way.
    void applyMarkers (T5ynthProcessor& proc, float loopStart, float loopEnd, float startPos,
                       bool openFirst)
    {
        auto& sampler = proc.getSampler();
        sampler.setPointsLocked (true);
        if (openFirst)
        {
            sampler.setLoopEnd (1.0f);
            sampler.setLoopStart (loopStart);
            sampler.setLoopEnd (loopEnd);
        }
        else
        {
            sampler.setLoopRegion (loopStart, loopEnd);
        }
        sampler.setStartPos (startPos);
        sampler.setWtExtractStart (0.0f);
        sampler.setWtExtractEnd (1.0f);
    }

    // Which parts of the recall's marker choreography to perform, so the step
    // can be attributed to a line rather than to "the recall".
    enum class Markers
    {
        None,        // plain GENERATE: loadGeneratedAudio alone
        Both,        // the recall as MainPanel::restoreMainSnapshot writes it
        BeforeOnly,  // markers set, then the sample -- no trailing block
        AfterOnly,   // the sample, then the markers
        Unchanged    // the recall, but the slot's markers equal the live ones
    };

    struct Case
    {
        const char* label;
        int         note;       // 60 = unity read, 67 = the stretch path
        Markers     markers;
        bool        openFirst;  // the old open-to-1.0 bracket dance
    };

    bool runCase (const Case& c, const std::string& wavOut)
    {
        T5ynthProcessor proc;
        proc.setRateAndBufferSizeDetails (gSampleRate, gBlockSize);
        proc.prepareToPlay (gSampleRate, gBlockSize);
        pump (200);

        // Neutral patch: what is left is the engine and the DCA, so the seam is
        // the only thing in the window that can move.
        setParam (proc, PID::engineMode, (float) EngineMode::Sampler);
        setParam (proc, PID::ampAttack,  5.0f);
        setParam (proc, PID::ampDecay,   100.0f);
        setParam (proc, PID::ampSustain, 1.0f);
        setParam (proc, PID::ampRelease, 100.0f);
        setParam (proc, PID::ampAmount,  1.0f);
        setParam (proc, PID::ampTarget,  (float) EnvTarget::DCA);
        setParam (proc, PID::velAmt,     0.0f);
        for (int m = 0; m < kNumModEnvs; ++m)
            setParam (proc, PID::modEnv[m].target, (float) EnvTarget::None);
        setParam (proc, PID::filterType,   0.0f);
        setParam (proc, PID::lfo1Depth,    0.0f);
        setParam (proc, PID::lfo2Depth,    0.0f);
        setParam (proc, PID::lfo3Depth,    0.0f);
        setParam (proc, PID::driftEnabled, 0.0f);
        setParam (proc, PID::delayMix,     0.0f);
        setParam (proc, PID::reverbMix,    0.0f);
        setParam (proc, PID::noiseLevel,   0.0f);
        setParam (proc, PID::driftCrossfade, 200.0f);   // Regen XFade, the default
        pump (150);

        const bool before = (c.markers == Markers::Both || c.markers == Markers::BeforeOnly
                          || c.markers == Markers::Unchanged);
        const bool after  = (c.markers == Markers::Both || c.markers == Markers::AfterOnly
                          || c.markers == Markers::Unchanged);

        // Slot 1: its markers, then its sample.
        if (before)
        {
            const juce::ScopedLock sl (proc.getCallbackLock());
            applyMarkers (proc, 0.0f, 1.0f, 0.0f, c.openFirst);
        }
        proc.loadGeneratedAudio (tone (gSampleRate, 220.0, 0.5f), gSampleRate);
        if (after)
        {
            const juce::ScopedLock sl (proc.getCallbackLock());
            applyMarkers (proc, 0.0f, 1.0f, 0.0f, c.openFirst);
            proc.getSampler().setPointsLocked (false);
        }
        pump (600);

        juce::AudioBuffer<float> buf (2, gBlockSize);
        juce::MidiBuffer midi;
        std::vector<float> out;

        const double blockMs   = 1000.0 * (double) gBlockSize / gSampleRate;
        const int    preBlocks = (int) (0.6 * gSampleRate / gBlockSize);
        const int    postBlocks= (int) (1.4 * gSampleRate / gBlockSize);

        // When the master's re-prepare flag rises and falls, in output samples:
        // the fall IS the second publication (applyPreparedBufferLoad), which is
        // what a held voice then crossfades onto a second time.
        std::vector<std::pair<int, bool>> repFlagEdges;
        bool lastRepFlag = false;
        // The master's prepared geometry. It changes exactly when a NEW snapshot
        // is published (applyPreparedBufferLoad), so a change here after the
        // recall IS a second publication -- the thing a held voice crossfades
        // onto a second time.
        juce::String lastGeom;
        std::vector<std::pair<int, juce::String>> geomChanges;

        auto render = [&] (int blocks, bool noteOnFirst)
        {
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                midi.clear();
                if (noteOnFirst && b == 0)
                    midi.addEvent (juce::MidiMessage::noteOn (1, c.note, (juce::uint8) 100), 0);
                {
                    // Three of the four shipped host wrappers (Standalone, VST3,
                    // AU) hold getCallbackLock() for the whole processBlock call —
                    // see juce_AudioProcessorPlayer.cpp,
                    // juce_audio_plugin_client_VST3.cpp,
                    // juce_audio_plugin_client_AU_1.mm. The fourth, CLAP, does
                    // NOT (verified against clap-juce-wrapper.cpp: zero
                    // getCallbackLock references, processBlock called bare), so
                    // SamplePlayer's snapshot pointers publish/read through the
                    // real atomic free-function API — that is what this harness's
                    // correctness actually depends on, on every format. The lock
                    // here is not load-bearing for that; it is taken anyway so
                    // this harness's concurrency against samplerReprepareThread
                    // matches actual Standalone/VST3/AU production conditions
                    // (rather than testing only the CLAP-shaped unlocked case,
                    // which this direct processBlock() call does not otherwise
                    // reproduce).
                    const juce::ScopedLock sl (proc.getCallbackLock());
                    proc.processBlock (buf, midi);
                }
                const auto* l = buf.getReadPointer (0);
                out.insert (out.end(), l, l + gBlockSize);
                // Real time, so the background reprepare thread runs at the pace
                // it has in the app -- the whole point of driving the processor
                // rather than the SamplePlayer.
                pump ((int) std::lround (blockMs));

                const bool rep = proc.getSampler().needsReprepare();
                if (rep != lastRepFlag)
                {
                    repFlagEdges.emplace_back ((int) out.size(), rep);
                    lastRepFlag = rep;
                }

                const juce::String st = proc.getSampler().debugStateString();
                juce::String geom = st.fromFirstOccurrenceOf ("playStart=", true, false)
                                      .upToFirstOccurrenceOf (" p1Off=", false, false);
                if (geom != lastGeom)
                {
                    geomChanges.emplace_back ((int) out.size(), geom);
                    lastGeom = geom;
                }
            }
        };

        render (preBlocks, /*noteOnFirst=*/true);

        const int seamIndex = (int) out.size();

        // ── The recall ──────────────────────────────────────────────────────
        // Slot 2's markers -- except in the Unchanged case, where the slot holds
        // the same points as the live patch and the block is a no-op in value
        // terms (it is not a no-op in flag terms: see setLoopEnd(1.0f)).
        const float l2 = (c.markers == Markers::Unchanged) ? 0.0f  : 0.05f;
        const float r2 = (c.markers == Markers::Unchanged) ? 1.0f  : 0.95f;
        const float s2 = (c.markers == Markers::Unchanged) ? 0.0f  : 0.05f;

        auto points = [&] ()
        {
            auto& s = proc.getSampler();
            return juce::String::formatted ("p1=%.4f p2=%.4f p3=%.4f dirty=%d",
                                            s.getStartPos(), s.getLoopStart(), s.getLoopEnd(),
                                            s.needsReprepare() ? 1 : 0);
        };
        juce::String stepA, stepB, stepC;

        if (before)
        {
            const juce::ScopedLock sl (proc.getCallbackLock());
            applyMarkers (proc, l2, r2, s2, c.openFirst);
        }
        stepA = points();
        proc.loadGeneratedAudio (tone (gSampleRate, 660.0, 0.85f), gSampleRate);
        stepB = points();
        if (after)
        {
            const juce::ScopedLock sl (proc.getCallbackLock());
            applyMarkers (proc, l2, r2, s2, c.openFirst);
            proc.getSampler().setPointsLocked (false);
        }
        stepC = points();

        // Synchronous, before a single block has run: did the recall hand the
        // master back dirty? A dirty master means the background thread will
        // rebuild and republish the same audio, which is a publication the held
        // note has to follow for no reason at all.
        const bool dirtyAfterRecall = proc.getSampler().needsReprepare();

        render (postBlocks, /*noteOnFirst=*/false);

        const int morphSamples = (int) std::lround (0.200 * gSampleRate);
        const int winEnd = seamIndex + morphSamples + 8192;   // + stretch latency guard

        float windowMax = 0.0f;
        int   worstAt   = seamIndex;
        for (int i = seamIndex - 1; i < winEnd && i < (int) out.size() - 1; ++i)
            if (std::abs (out[i + 1] - out[i]) > windowMax)
            {
                windowMax = std::abs (out[i + 1] - out[i]);
                worstAt = i;
            }

        float naturalMax = 0.0f;
        for (int i = winEnd; i < (int) out.size() - 1; ++i)
            naturalMax = std::max (naturalMax, std::abs (out[i + 1] - out[i]));

        auto rms = [&] (int a, int b)
        {
            double s = 0.0; int n = 0;
            for (int i = std::max (0, a); i < b && i < (int) out.size(); ++i) { s += (double) out[i] * out[i]; ++n; }
            return n ? std::sqrt (s / n) : 0.0;
        };

        writeWav (wavOut, out, (int) gSampleRate);

        const bool clickFree    = windowMax <= naturalMax * 1.5f + 1.0e-4f;
        const double rmsPre     = rms (seamIndex - 8000, seamIndex);
        const double rmsPost    = rms (winEnd, winEnd + 8000);
        const bool   transitioned = rmsPost > 1.0e-5;

        std::printf ("  [%-32s] %s  windowMax=%.5f (%.2fx natural, at %+6.1f ms)"
                     "  rmsPre=%.4f rmsPost=%.4f -> %s\n",
                     c.label,
                     dirtyAfterRecall ? "LEFT DIRTY" : "clean     ",
                     windowMax,
                     naturalMax > 1.0e-9f ? windowMax / naturalMax : 0.0f,
                     1000.0 * (double) (worstAt - seamIndex) / gSampleRate,
                     rmsPre, rmsPost,
                     (clickFree && transitioned) ? "CLICK-FREE XFADE"
                                                 : (clickFree ? "*** SILENT ***" : "*** STEP ***"));

        std::printf ("      after markers: %s | after load: %s | after re-assert: %s\n",
                     stepA.toRawUTF8(), stepB.toRawUTF8(), stepC.toRawUTF8());
        for (const auto& g : geomChanges)
            std::printf ("      master publish @%+7.1f ms  %s\n",
                         1000.0 * (double) (g.first - seamIndex) / gSampleRate,
                         g.second.toRawUTF8());

        return clickFree && transitioned;
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc > 1) gSampleRate = std::atof (argv[1]);
    if (argc > 2) gBlockSize  = std::atoi (argv[2]);

    std::printf ("SNAP recall on a held sampler note (A=220Hz -> B=660Hz, Regen XFade 200 ms,"
                 " %.0f Hz / %d):\n", gSampleRate, gBlockSize);

    bool ok = true;
    ok &= runCase ({ "GENERATE, unity",            60, Markers::None, false }, "/tmp/snap_generate_unity.wav");
    ok &= runCase ({ "SNAP recall, unity",         60, Markers::Both, false }, "/tmp/snap_recall_unity.wav");
    ok &= runCase ({ "GENERATE, stretch (+7)",     67, Markers::None, false }, "/tmp/snap_generate_pitched.wav");
    ok &= runCase ({ "SNAP recall, stretch (+7)",  67, Markers::Both, false }, "/tmp/snap_recall_pitched.wav");

    std::printf ("\nthe same recall with the old open-to-1.0 bracket dance, which leaves the\n"
                 "master dirty and lands a SECOND publication of the same audio ~10 ms later:\n");
    ok &= runCase ({ "old dance, unity",           60, Markers::Both, true },  "/tmp/snap_old_unity.wav");
    ok &= runCase ({ "old dance, stretch (+7)",    67, Markers::Both, true },  "/tmp/snap_old_pitched.wav");

    std::printf ("\nwhich line of the recall dirties the master (old dance, stretch read):\n");
    ok &= runCase ({ "markers BEFORE the sample only", 67, Markers::BeforeOnly, true }, "/tmp/snap_before_only.wav");
    ok &= runCase ({ "markers AFTER the sample only",  67, Markers::AfterOnly,  true }, "/tmp/snap_after_only.wav");
    ok &= runCase ({ "recall, markers unchanged",      67, Markers::Unchanged,  true }, "/tmp/snap_unchanged.wav");

    std::printf ("\nWAVs in /tmp/snap_*.wav\n%s\n", ok ? "ALL PASS" : "*** FAIL ***");
    return ok ? 0 : 1;
}
