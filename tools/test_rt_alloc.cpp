// Does processBlock allocate?
//
// CLAUDE.md's JUCE-safety rule 4 -- never allocate, lock or do file I/O on the
// audio thread -- had no gate, and the MPE corpus cannot be one: it reads voice
// state, and an allocation leaves none. So a note-on could carry eleven heap
// allocations for years while every suite stayed green. It did: the sampler's
// debug log guarded its BODY with `if constexpr`, which the message's own
// juce::String concatenation at the call site never reaches, and the messages
// were built and thrown away on every note-on in a build with logging off.
//
// This replaces global operator new with a counting one and drives real MIDI
// through the real T5ynthProcessor::processBlock. The counter is thread_local
// and armed only around the processBlock call, so the AudioComponent registry
// scanning that macOS does on its own threads cannot pollute the reading.
//
// Build (T5ynth's standard offline-tool recipe):
//
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSND=$PWD/third_party/csound/macos-arm64/lib
//   clang++ -std=c++17 -O2 -g @/tmp/h.rsp tools/test_rt_alloc.cpp \
//     build_clean/T5ynth_artefacts/Release/libakroasys_SharedCode.a \
//     build_clean/libT5ynthData.a "$CSND/CsoundLib64" \
//     -framework CoreAudioKit -framework DiscRecording -framework CoreAudio \
//     -framework CoreMIDI -framework AudioToolbox -framework Accelerate \
//     -framework WebKit -weak_framework Metal -weak_framework MetalKit \
//     -framework QuartzCore -framework Cocoa -framework Foundation \
//     -framework IOKit -framework Security -framework Carbon \
//     -framework AudioUnit -framework CoreServices -o /tmp/t5main/test_rt_alloc

// CoreFoundation before JUCE: MacTypes.h declares a struct Point that becomes
// ambiguous with juce::Point once JUCE's headers are in scope.
#include <CoreFoundation/CoreFoundation.h>

#include "../src/PluginProcessor.h"
#include "../src/dsp/BlockParams.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <map>
#include <vector>

namespace
{
    thread_local bool  gArmed = false;
    std::map<void*, long>* gSites = nullptr;

    constexpr double kSampleRate = 48000.0;
    constexpr int    kBlockSize  = 256;

    int gFailures = 0;
}

void* operator new (size_t n)
{
    if (gArmed && gSites != nullptr)
    {
        gArmed = false;                       // the map allocates; do not recurse
        (*gSites)[__builtin_return_address (0)] += 1;
        gArmed = true;
    }
    void* p = std::malloc (n != 0 ? n : 1);
    if (p == nullptr) std::abort();
    return p;
}
void operator delete (void* p) noexcept         { std::free (p); }
void operator delete (void* p, size_t) noexcept { std::free (p); }

namespace
{
    struct Rig
    {
        T5ynthProcessor proc;
        juce::AudioBuffer<float> buf { 2, kBlockSize };
        juce::MidiBuffer midi;
        std::map<void*, long> sites;

        Rig()
        {
            proc.prepareToPlay (kSampleRate, kBlockSize);
            run (60);
        }

        void run (int blocks = 1)
        {
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                gArmed = (gSites != nullptr);
                proc.processBlock (buf, midi);
                gArmed = false;
                midi.clear();
            }
        }

        void noteOn  (int ch, int note, int vel = 100)
        { midi.addEvent (juce::MidiMessage::noteOn (ch, note, (juce::uint8) vel), 0); }
        void noteOff (int ch, int note)
        { midi.addEvent (juce::MidiMessage::noteOff (ch, note), 0); }
        void cc (int ch, int number, int value)
        { midi.addEvent (juce::MidiMessage::controllerEvent (ch, number, value), 0); }

        void arm()    { gSites = &sites; }
        void disarm() { gSites = nullptr; }
    };

    void report (const char* what, std::map<void*, long>& sites)
    {
        long total = 0;
        for (auto& e : sites)
            total += e.second;

        if (total == 0)
        {
            std::printf ("  ok    %s -- no allocation\n", what);
            return;
        }

        ++gFailures;
        std::printf ("  FAIL  %s -- %ld allocations on the audio thread\n", what, total);

        std::vector<std::pair<void*, long>> v (sites.begin(), sites.end());
        std::sort (v.begin(), v.end(),
                   [] (const auto& a, const auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < v.size() && i < 12; ++i)
        {
            Dl_info info {};
            const char* name = (dladdr (v[i].first, &info) != 0 && info.dli_sname != nullptr)
                                 ? info.dli_sname : "?";
            std::printf ("        %6ld  %p  %s\n", v[i].second, v[i].first, name);
        }
    }

    // Every lazily-built thing the first note touches -- voice buffers, the
    // engines' scratch storage -- allocates once, legitimately, and would read
    // as a defect if it landed inside the measurement. Warm it first.
    void warm (Rig& r)
    {
        for (int i = 0; i < 8; ++i)
        {
            r.noteOn (1, 60 + i);
            r.run();
            r.noteOff (1, 60 + i);
            r.run();
        }
        r.run (40);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("\nprocessBlock allocation gate -- CLAUDE.md JUCE safety rule 4\n");
    std::printf ("real T5ynthProcessor::processBlock, counting operator new\n\n");

    {
        Rig r;
        warm (r);
        r.arm();
        for (int i = 0; i < 100; ++i)
        {
            r.noteOn (1, 60 + (i % 8));
            r.run();
            r.noteOff (1, 60 + (i % 8));
            r.run();
        }
        r.disarm();
        report ("100 note-on / note-off pairs", r.sites);
    }

    {
        Rig r;
        warm (r);
        r.arm();
        for (int i = 0; i < 100; ++i)                     // MPE traffic on members
        {
            r.noteOn (2 + (i % 8), 60 + (i % 8));
            r.run();
            r.cc (2 + (i % 8), 74, i % 128);
            r.midi.addEvent (juce::MidiMessage::channelPressureChange (2 + (i % 8), i % 128), 0);
            r.midi.addEvent (juce::MidiMessage::pitchWheel (2 + (i % 8), 8192 + (i * 37)), 0);
            r.run();
            r.noteOff (2 + (i % 8), 60 + (i % 8));
            r.run();
        }
        r.disarm();
        report ("100 MPE notes with per-note bend, slide and pressure", r.sites);
    }

    {
        Rig r;
        if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
            p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
        warm (r);
        r.noteOn (1, 60);
        r.noteOn (1, 64);
        r.noteOn (1, 67);
        r.run (20);
        r.arm();
        r.run (400);                                      // the arp dispatching steps
        r.disarm();
        report ("400 blocks of a chord under the arpeggiator", r.sites);
    }

    {
        // The arpeggiator's false->true EDGE, which the case above cannot see:
        // it sets arpMode before warm(), so the edge fires unarmed and all 400
        // armed blocks run with arpWasEnabled already true. The edge is where
        // the sequencer flush lives, and StepSequencer::allNotesOff push_back()s
        // without a capacity check -- so a deliberate allocation there passed
        // every gate this suite had.
        Rig r;
        auto set = [&r] (const char* pid, float v)
        {
            if (auto* p = r.proc.getValueTreeState().getParameter (pid))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };
        set (PID::genSeqRunning, 0.0f);
        r.run (2);
        auto& seq = r.proc.getStepSequencer();
        seq.setNumSteps (4);
        for (int i = 0; i < 4; ++i) { seq.setStepNote (i, 60 + 3 * i); seq.setStepEnabled (i, true); }
        set (PID::seqSteps, 4.0f);
        set (PID::seqBpm, 200.0f);
        set (PID::seqGate, 0.95f);
        set (PID::seqRunning, 1.0f);
        warm (r);
        r.noteOn (2, 72);
        r.run (20);
        r.arm();
        for (int t = 0; t < 20; ++t)                      // 20 crossings of the edge
        {
            set (PID::arpMode, 1.0f);
            r.run (6);
            set (PID::arpMode, 0.0f);
            r.run (6);
        }
        r.disarm();
        report ("20 arpeggiator switch-ons over a running sequencer", r.sites);
    }

    std::printf ("\n%s\n\n", gFailures == 0 ? "ALL PASS" : "FAILED");
    return gFailures == 0 ? 0 : 1;
}
