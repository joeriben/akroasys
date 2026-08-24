// A GATE, not a one-off probe: exits non-zero if switching the arpeggiator on
// while a sequencer runs leaves anything holding. Run it after touching the arp
// edges, the sequencers' flush, or the lead filter.
//
// Does switching the arpeggiator on, while a sequencer is running, leave a note
// hanging forever? It did, on both sequencers, until 2026-08-24.
//
// The mechanism, read out of the code before it was measured:
//
//   PluginProcessor.cpp, arp false->true edge -- the sequencer's currently
//   sounding note is flushed by QUEUEING a note-off into internalNoteEvents_
//   (StepSequencer::allNotesOff, which also sets lastPlayedNote = -1).
//
//   PluginProcessor.cpp, later in the SAME block, inside if (arpEnabled) { if
//   (seqRunning) -- the lead filter erases every event with strandId < 0 (step
//   mode) or strandId == 0 (generative mode): "drop lead note-ons AND
//   note-offs". It erases the flush that was queued a few hundred lines above.
//
// And because lastPlayedNote is already -1 by then, no later flush can re-emit
// it. The voice never receives a note-off from anyone. Stopping the sequencer
// does not help, switching the arpeggiator back off does not help; only a MIDI
// panic ends it.
//
// The window is why this is easy to miss: the sequencer must be INSIDE a gated
// step at the exact block the arpeggiator comes on. With short gates most
// switch-ons land in a gap and nothing hangs, so the same gesture is harmless
// nine times and drones the tenth.
//
// Measured against the code as it stood before the fix: 2 stuck notes, one per
// sequencer, still holding after 10.7 s of silence with the transport stopped
// and the arpeggiator switched back off. Only a MIDI panic ended them.
//
// Build (T5ynth's standard offline-tool recipe):
//
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSND=$PWD/third_party/csound/macos-arm64/lib
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/repro_arp_edge_stuck.cpp \
//     build_clean/T5ynth_artefacts/Release/libakroasys_SharedCode.a \
//     build_clean/libT5ynthData.a "$CSND/CsoundLib64" \
//     -framework CoreAudioKit -framework DiscRecording -framework CoreAudio \
//     -framework CoreMIDI -framework AudioToolbox -framework Accelerate \
//     -framework WebKit -weak_framework Metal -weak_framework MetalKit \
//     -framework QuartzCore -framework Cocoa -framework Foundation \
//     -framework IOKit -framework Security -framework Carbon \
//     -framework AudioUnit -framework CoreServices -o /tmp/t5main/repro_arp_edge_stuck

#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "../src/PluginProcessor.h"
#include "../src/dsp/BlockParams.h"
#include <cstdio>

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr int    kBlockSize  = 256;

    int gStuck = 0;

    struct Rig
    {
        T5ynthProcessor      proc;
        juce::AudioBuffer<float> buf { 2, kBlockSize };
        juce::MidiBuffer     midi;

        Rig()
        {
            proc.prepareToPlay (kSampleRate, kBlockSize);
            buf.clear();
        }

        void set (const char* pid, float v)
        {
            if (auto* p = proc.getValueTreeState().getParameter (pid))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        }

        void run (int blocks = 1)
        {
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                proc.processBlock (buf, midi);
                midi.clear();
            }
        }

        // Every voice that is still HELD -- gate open, not in its release tail.
        int heldVoices (int* noteOut = nullptr) const
        {
            const auto& vm = proc.getVoiceManager();
            int n = 0;
            for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            {
                const auto& v = vm.getVoice (i);
                if (v.isActive() && ! v.isReleasing())
                {
                    if (noteOut != nullptr && n == 0) *noteOut = v.getCurrentNote();
                    ++n;
                }
            }
            return n;
        }
    };

    // The sequencer on one long-gated step, so the switch-on lands INSIDE the
    // note rather than in a gap. That is the whole difference between seeing
    // this and not seeing it.
    void armSequencer (Rig& r, int note)
    {
        r.set (PID::genSeqRunning, 0.0f);
        r.run (2);
        auto& seq = r.proc.getStepSequencer();
        seq.setNumSteps (1);
        seq.setStepNote (0, note);
        seq.setStepEnabled (0, true);
        seq.setStepBindMode (0, T5ynthStepSequencer::BindMode::Off);
        r.set (PID::seqSteps, 1.0f);
        r.set (PID::seqBpm, 40.0f);
        r.set (PID::seqGate, 0.95f);
        r.set (PID::seqRunning, 1.0f);
    }

    void probe (const char* title, bool genMode)
    {
        std::printf ("\n%s\n", title);
        Rig r;
        if (genMode)
        {
            // genSeqRunning is the STEP<->GEN toggle, not the transport
            // (PluginProcessor.cpp: "PID::genSeqRunning is a STEP/GEN toggle,
            // not transport"). Without seqRunning as well, nothing plays and
            // the probe measures an idle machine.
            r.set (PID::genSeqRunning, 1.0f);
            r.set (PID::seqBpm, 40.0f);
            r.set (PID::seqRunning, 1.0f);
        }
        else
        {
            armSequencer (r, 60);
        }

        // Wait for the line to actually be INSIDE a note. Switching on during a
        // gap proves nothing, and a fixed block count lands in a gap as often as
        // not -- at 40 BPM one quarter is 281 blocks.
        int note = -1;
        int waited = 0;
        while (r.heldVoices (&note) == 0 && waited < 4000) { r.run (1); ++waited; }
        const int sounding = r.heldVoices (&note);
        std::printf ("   waited %d block(s) for a gated note\n", waited);
        std::printf ("   the line is sounding:            %d held voice(s), note %d\n",
                     sounding, note);
        if (sounding == 0)
        {
            std::printf ("   (nothing was gated at the switch-on -- this run says nothing)\n");
            return;
        }

        r.set (PID::arpMode, 1.0f);              // arpeggiator ON, mid-note
        r.run (1);
        std::printf ("   arpeggiator switched on:         %d held voice(s)\n", r.heldVoices());

        r.set (PID::arpMode, 0.0f);              // and off again
        r.set (PID::seqRunning, 0.0f);
        r.set (PID::genSeqRunning, 0.0f);
        r.run (400);                             // ~2.1 s with nothing playing

        int left = -1;
        const int after = r.heldVoices (&left);
        std::printf ("   arp off, transport stopped:      %d held voice(s)", after);
        if (after > 0) std::printf (", note %d", left);
        std::printf ("\n");

        r.run (2000);                            // ~10.7 s more
        const int late = r.heldVoices (&left);
        std::printf ("   another 10.7 s of silence later:  %d held voice(s)", late);
        if (late > 0) std::printf (", note %d  <<< STUCK", left);
        std::printf ("\n");
        if (late > 0)
        {
            ++gStuck;
            // A panic is the only thing that ends it -- worth showing, because
            // it is what a player has to reach for today.
            r.midi.addEvent (juce::MidiMessage::controllerEvent (1, 123, 0), 0);
            r.run (200);
            std::printf ("   after a MIDI panic:              %d held voice(s)\n",
                         r.heldVoices());
        }
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf ("switching the arpeggiator on while a sequencer runs\n");
    std::printf ("criterion: when the transport stops and the arpeggiator is off, "
                 "nothing is left holding\n");

    probe ("A. step sequencer, switch-on inside a gated step", false);
    probe ("B. generative sequencer, same gesture", true);

    std::printf ("\n%s -- %d stuck note(s)\n\n",
                 gStuck == 0 ? "ALL CLEAR" : "FAILED", gStuck);
    return gStuck == 0 ? 0 : 1;
}
