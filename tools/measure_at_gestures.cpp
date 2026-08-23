// The four acceptance gestures, measured instead of estimated by ear.
//
// The criterion is "eine losgelassene Note schwillt nicht wieder an", and that
// is a NUMBER, not an impression: with AT -> DCA at full amount the shipped law
// is gain * jlimit(0, 1, 1 - amt + amt * pressure) (SynthVoice.cpp:133-142), so
// at amt = 1 the voice's level is exactly ampEnv * aftertouch. A swell after a
// key-up therefore IS a rise in that voice's aftertouch, and nothing else. This
// tool drives the four gestures through the real T5ynthProcessor::processBlock
// and watches every voice whose key has come up, block by block.
//
// AT -> Cutoff needs no column of its own. Every continuous aftertouch target
// goes through the same aftertouchDrive(p, target, src) = src[axis] * amount
// (SynthVoice.cpp:88-94), so with the same source and amount the cutoff's
// contribution is the same function of Z as the DCA's. A Z that never rises
// after the key-up is the criterion for both, and for the other eleven targets
// with it. The routing is still switched on below so the whole path runs.
//
// Nothing here is a model of hearing. Every number printed is one the DSP
// itself uses: SynthVoice::getAftertouch (what every aftertouch target reads),
// getAmpEnvLevel, and VoiceManager::pressureForHeldNote (what the
// instrument-wide Cache and Snap travellers consume).
//
// The criterion is FROZEN, not merely "does not swell". Once a key is up its
// voice keeps exactly the pressure that key left it with. A rise is the swell
// the criterion names; a fall is the other half of the same rule, and it is
// what a controller's between-note reset burst used to do to a tail -- silent
// as a swell, very audible as a decaying note cut off in one block.
//
// WHAT THIS TOOL CATCHES, measured against the real code, not asserted:
//
//   M1  the wheel reaches released and pedal-held voices again
//       (drop refreshPerformancePressure's isReleasing / sustained guard)
//       -> 14 violations: gesture B twelve times, gesture C twice.
//   M2  the expression tag is left standing past the key-up
//       (drop voiceExprChannel_[i] = 0 in noteOff)
//       -> 7 violations: the tails are cut to 0 sixteen ms after the key-up,
//          gestures A and B.
//   M6  the poly-pressure gate rejects every message
//       -> gesture D: the reading no longer follows the hand.
//
// Three mutations it does NOT catch, said plainly rather than left to look
// covered: M3 (the expression hand-off strips a channel a key still holds) and
// M4 (the fresh-press mark fires on re-strikes too) pass every gesture here --
// they are gated by cases 53 and 57 in tools/test_mpe_parity.cpp instead. M5
// (the Cache/Snap traveller reads decaying voices too) passes because with the
// arpeggiator on the reading comes from the held-key latch, not from a voice
// traversal.
//
// Two of gesture D's read-outs cannot fail and are printed as read-outs, with
// the reason at the site. That is deliberate: a line that reads as a guarantee
// and checks nothing is worse than no line.
//
// The harness renders SILENCE -- with no engine data loaded no voice produces a
// sample -- so the audio itself cannot be measured here. The control path can,
// and it is the whole of the criterion.
//
// Build (T5ynth's standard offline-tool recipe):
//
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSND=$PWD/third_party/csound/macos-arm64/lib
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/measure_at_gestures.cpp \
//     build_clean/T5ynth_artefacts/Release/libakroasys_SharedCode.a \
//     build_clean/libT5ynthData.a "$CSND/CsoundLib64" \
//     -framework CoreAudioKit -framework DiscRecording -framework CoreAudio \
//     -framework CoreMIDI -framework AudioToolbox -framework Accelerate \
//     -framework WebKit -weak_framework Metal -weak_framework MetalKit \
//     -framework QuartzCore -framework Cocoa -framework Foundation \
//     -framework IOKit -framework Security -framework Carbon \
//     -framework AudioUnit -framework CoreServices -o /tmp/t5main/measure_at_gestures

// CoreFoundation before JUCE: MacTypes.h declares a struct Point that becomes
// ambiguous with juce::Point once JUCE's headers are in scope.
#include <CoreFoundation/CoreFoundation.h>

#include "../src/PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr int    kBlockSize  = 256;
    constexpr float  kBlockMs    = 1000.0f * kBlockSize / (float) kSampleRate;

    int gViolations = 0;

    struct Watch
    {
        bool  armed    = false;   // this voice's key has come up
        int   note     = -1;      // the pitch it was on when that happened
        float frozenAt = 0.0f;    // the pressure the finger left it with
        int   atBlock  = 0;
    };

    struct Rig
    {
        T5ynthProcessor proc;
        juce::AudioBuffer<float> buf { 2, kBlockSize };
        juce::MidiBuffer midi;
        std::array<Watch, VoiceManager::MAX_VOICES> watch {};
        int block = 0;
        const char* gesture = "";
        float worstMove = 0.0f;

        Rig() { proc.prepareToPlay (kSampleRate, kBlockSize); run (40); resetWatch(); }

        void resetWatch() { for (auto& w : watch) w = Watch {}; }

        void set (const char* pid, float v)
        {
            if (auto* p = proc.getValueTreeState().getParameter (pid))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        }

        void send (const juce::MidiMessage& m) { midi.addEvent (m, 0); }
        void noteOn  (int ch, int n, int v = 100)
        { send (juce::MidiMessage::noteOn (ch, n, (juce::uint8) v)); }
        void noteOff (int ch, int n) { send (juce::MidiMessage::noteOff (ch, n)); }
        void pressure (int ch, int v) { send (juce::MidiMessage::channelPressureChange (ch, v)); }
        void polyAt (int ch, int n, int v) { send (juce::MidiMessage::aftertouchChange (ch, n, v)); }
        void cc (int ch, int n, int v) { send (juce::MidiMessage::controllerEvent (ch, n, v)); }

        // A key came up. Every voice standing on that pitch is watched from now
        // on: its pressure may fall and must never climb back.
        void armKeyUp (int note)
        {
            const auto& vm = proc.getVoiceManager();
            for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            {
                const auto& v = vm.getVoice (i);
                if (v.isActive() && v.getCurrentNote() == note)
                    watch[(size_t) i] = { true, note, v.getAftertouch(), block };
            }
        }

        void run (int blocks = 1)
        {
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                proc.processBlock (buf, midi);
                midi.clear();
                ++block;
                inspect();
            }
        }

        void inspect()
        {
            const auto& vm = proc.getVoiceManager();
            for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            {
                auto& w = watch[(size_t) i];
                if (! w.armed) continue;
                const auto& v = vm.getVoice (i);
                // The slot was taken over by another note: it is a different
                // voice now and the old one's trajectory has ended.
                if (! v.isActive() || v.getCurrentNote() != w.note) { w.armed = false; continue; }

                // Stronger than "does not swell": once the key is up the note
                // keeps exactly the pressure that key left it with. A rise is
                // the swell; a fall is the other half of the same rule, and it
                // is what a controller's between-note reset burst used to do to
                // a tail -- inaudible as a swell, very audible as a tail cut to
                // silence in one block.
                const float z = v.getAftertouch();
                const float d = z - w.frozenAt;
                if (std::fabs (d) > 1e-4f)
                {
                    worstMove = juce::jmax (worstMove, std::fabs (d));
                    ++gViolations;
                    std::printf ("  %s  %-34s voice %d, note %d: %.4f -> %.4f  "
                                 "(%+.4f) %.0f ms after the key came up\n",
                                 d > 0.0f ? "SWELL" : "DROP ", gesture, i, w.note,
                                 w.frozenAt, z, d,
                                 (float) (block - w.atBlock) * kBlockMs);
                    w.frozenAt = z;          // report each move once
                }
            }
        }

        void trace (const char* label)
        {
            const auto& vm = proc.getVoiceManager();
            std::printf ("   %-30s", label);
            for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            {
                const auto& v = vm.getVoice (i);
                if (! v.isActive()) continue;
                std::printf (" [v%d n%d %s Z=%.3f env=%.3f lvl=%.3f]",
                             i, v.getCurrentNote(), v.isReleasing() ? "rel" : "HELD",
                             v.getAftertouch(), v.getAmpEnvLevel(),
                             v.getAmpEnvLevel() * v.getAftertouch());
            }
            std::printf ("\n");
        }
    };

    // AT -> DCA and AT -> Cutoff, both at full amount, both driven by pressure.
    // At amt = 1 the DCA law reduces to level = ampEnv * pressure exactly, so
    // "lvl" in the trace is the voice's real gain, not an approximation.
    void routePressureToDcaAndCutoff (Rig& r)
    {
        r.set (PID::exprSrcDca,    (float) ExprSource::Z);
        r.set (PID::aftertouchAmtDca, 1.0f);
        r.set (PID::exprSrcCutoff, (float) ExprSource::Z);
        r.set (PID::aftertouchAmtCutoff, 1.0f);
        // The filter is OFF in the default patch and its whole cutoff block sits
        // behind that flag (SynthVoice.cpp, `if (p.filterEnabled)`), so without
        // this the routed cutoff path would not run at all.
        r.set (PID::filterEnabled, 1.0f);
        r.set (PID::filterCutoff, 1000.0f);
        r.run (2);
    }

    // ── A. Repeated strikes of one pitch ───────────────────────────────────
    void gestureRepeatedStrikes()
    {
        std::printf ("\nA. repeated strikes of one pitch, AT -> DCA and AT -> Cutoff at full\n");
        Rig r;
        r.gesture = "repeated strikes";
        routePressureToDcaAndCutoff (r);

        for (int strike = 0; strike < 4; ++strike)
        {
            r.noteOn (2, 60, strike == 0 ? 100 : 20);
            r.run (2);
            r.pressure (2, 120);                  // lean in hard
            r.run (6);
            r.trace (strike == 0 ? "leaning in" : "leaning in again");
            // The key comes up while the finger is still leaning: that is the
            // order a controller sends, and it is the case that matters -- the
            // tail carries the pressure, and the question is whether anything
            // lifts it afterwards.
            r.noteOff (2, 60);
            r.armKeyUp (60);
            r.run (2);
            r.trace ("key up, still leaning");
            r.pressure (2, 0);                    // the channel reset that follows
            r.run (2);
            r.trace ("after the channel reset");
            r.run (10);                           // the tail, with the next strike
        }                                         // landing on top of it
        r.run (40);
        r.trace ("after the last tail");
    }

    // ── B. Chords with overlapping release ─────────────────────────────────
    void gestureOverlappingRelease()
    {
        std::printf ("\nB. a chord released one finger at a time, wheel moving over the tails\n");
        Rig r;
        r.gesture = "overlapping release";
        routePressureToDcaAndCutoff (r);

        const int notes[3] = { 60, 64, 67 };
        const int chans[3] = { 2, 3, 4 };
        for (int i = 0; i < 3; ++i) { r.noteOn (chans[i], notes[i]); r.run (2); }
        for (int i = 0; i < 3; ++i) { r.pressure (chans[i], 110); r.run (2); }
        r.trace ("chord leaned into");

        for (int i = 0; i < 3; ++i)
        {
            r.noteOff (chans[i], notes[i]);
            r.armKeyUp (notes[i]);
            r.run (4);
            r.trace ("one finger fewer");
            // The two things that used to reach back into a tail: the wheel,
            // and the reset burst the controller sends before its next note.
            r.cc (1, 1, 127);
            r.run (2);
            r.cc (1, 1, 0);
            r.run (2);
            r.pressure (chans[i], 0);
            r.cc (chans[i], 74, 0);
            r.run (2);
            r.trace ("after wheel + channel reset");
        }
        r.run (40);
    }

    // ── C. The sustain pedal ───────────────────────────────────────────────
    void gestureSustainPedal()
    {
        std::printf ("\nC. sustain pedal: keys lifted, notes still singing\n");
        Rig r;
        r.gesture = "sustain pedal";
        routePressureToDcaAndCutoff (r);

        r.cc (1, 64, 127);                        // damper down
        r.run (2);
        r.noteOn (2, 60);
        r.run (2);
        r.pressure (2, 120);
        r.run (6);
        r.trace ("pedalled note, leaned into");

        r.noteOff (2, 60);
        r.armKeyUp (60);
        r.run (4);
        r.trace ("key up, pedal holding");

        r.noteOn (2, 60, 20);                     // the same key again, lightly
        r.run (6);
        r.trace ("re-pressed over the pedalled note");
        // The wheel goes ABOVE what the pedalled tail is holding. Below it the
        // max() in pressureForVoice hides the whole question: a released voice
        // that is wrongly still following the wheel looks identical to one that
        // is correctly frozen, because its own frozen value is the larger of
        // the two. Only a wheel above the tail can tell them apart.
        r.cc (1, 1, 127);
        r.run (4);
        r.trace ("wheel above the pedalled tail");
        r.cc (1, 1, 1);                           // and back down to a resting wheel
        r.run (4);
        r.trace ("wheel back down");
        r.cc (1, 64, 0);                          // pedal up
        r.run (40);
        r.trace ("pedal up");
    }

    // ── D. The arpeggiator with Cache / Snap ───────────────────────────────
    //      These two do not modulate a voice, they MOVE THE INSTRUMENT: a
    //      landing is a Csound recompile. What they traverse is the reading
    //      below, so a reading that collapses and returns under a hand that
    //      never moved is a landing, and then another one on the way back.
    void gestureArpWithCacheSnap()
    {
        std::printf ("\nD. arpeggiator on, chord held, AT -> Cache and AT -> Snap\n");
        Rig r;
        r.gesture = "arp + cache/snap";
        r.set (PID::exprSrcCache, (float) ExprSource::Z);
        r.set (PID::aftertouchAmtCache, 1.0f);
        r.set (PID::exprSrcSnap,  (float) ExprSource::Z);
        r.set (PID::aftertouchAmtSnap, 1.0f);
        r.run (2);

        r.noteOn (2, 60); r.noteOn (3, 64); r.noteOn (4, 67);
        r.run (2);
        r.polyAt (2, 60, 120); r.polyAt (3, 64, 120); r.polyAt (4, 67, 120);
        r.run (4);
        r.set (PID::arpMode, 1.0f);               // 0 = Off, 1 = Up
        r.run (10);

        // Exactly what processBlock's axisReading computes for Z, and for the
        // same reason it is written the way it is: a note-indexed reading here
        // and a voice-traversal there would agree in every quiet case and part
        // company precisely where this gesture is aimed. Reading three note
        // numbers straight out of the latch, as this did first, measures a pure
        // function of the latch -- constant by construction, mutation-proof,
        // and green whatever the instrument does.
        const auto& vm = r.proc.getVoiceManager();
        auto reading = [&r, &vm]
        {
            float value = vm.maxHeldExpression (ExprSource::Z);
            for (const auto& k : r.proc.getArpeggiator().getHeldKeys())
                value = juce::jmax (value, vm.pressureForHeldNote (k.note));
            return value;
        };

        float lowest = reading(), highest = reading();
        int   collapses = 0;
        bool  wasDown = false;
        const float startedAt = reading();
        std::printf ("   reading with the hand leaning in: %.4f\n", startedAt);

        for (int b = 0; b < 400; ++b)             // ~2.1 s of arpeggio, hand still
        {
            // A hand leaning on a key does not send its pressure once, it
            // STREAMS it -- the Osmose retransmits continuously. That matters
            // here: the latch is re-accepted on every one of those messages,
            // and the gate that decides whether to accept sits on the key
            // ledger the arpeggiator is hammering. Sending the pressure once
            // and then reading a constant would only prove the latch is
            // sticky; sending it under the arp's re-strikes is what proves the
            // gate lets a leaning hand through while the arp plays.
            if (b % 8 == 0)
            {
                r.polyAt (2, 60, 120); r.polyAt (3, 64, 120); r.polyAt (4, 67, 120);
            }
            r.run (1);
            const float z = reading();
            lowest  = juce::jmin (lowest, z);
            highest = juce::jmax (highest, z);
            const bool down = z < startedAt * 0.5f;
            if (down && ! wasDown) ++collapses;
            wasDown = down;
        }
        std::printf ("   over 400 blocks (%.0f ms) with nobody moving:"
                     "  lowest %.4f  highest %.4f  collapses %d\n",
                     400.0f * kBlockMs, lowest, highest, collapses);
        // A reading that is already 0 makes the collapse test vacuous -- nothing
        // can fall below half of nothing -- so the floor is asserted first.
        if (startedAt <= 1.0e-4f)
        {
            ++gViolations;
            std::printf ("  DEAF  the traveller reads nothing at all with a hand leaning in\n");
        }
        else if (collapses != 0 || lowest < startedAt * 0.5f)
        {
            ++gViolations;
            std::printf ("  DROP  the traveller's reading falls away under a hand that never moved"
                         " -- %d landings, and %d on the way back\n", collapses, collapses);
        }

        // Second half, and the one a constant pressure cannot ask: does the
        // reading FOLLOW a hand that is moving, while the arpeggiator plays?
        // A blocked write and a held value are indistinguishable as long as the
        // hand is still -- both read 0.9449. Only a hand that leans harder and
        // lighter separates "the gate lets the pressure through" from "the gate
        // dropped it and the old value happened to be right".
        float reachedLow = 1.0f, reachedHigh = 0.0f;
        for (int sweep = 0; sweep < 3; ++sweep)
        {
            for (int v : { 40, 120 })
            {
                for (int b = 0; b < 24; ++b)
                {
                    if (b % 8 == 0)
                    {
                        r.polyAt (2, 60, v); r.polyAt (3, 64, v); r.polyAt (4, 67, v);
                    }
                    r.run (1);
                    const float z = reading();
                    reachedLow  = juce::jmin (reachedLow,  z);
                    reachedHigh = juce::jmax (reachedHigh, z);
                }
            }
        }
        std::printf ("   hand leaning harder and lighter under the arp:"
                     "  reading spans %.4f .. %.4f  (sent 40/127 = %.4f, 120/127 = %.4f)\n",
                     reachedLow, reachedHigh, 40.0f / 127.0f, 120.0f / 127.0f);
        if (std::fabs (reachedLow - 40.0f / 127.0f) > 0.01f
            || std::fabs (reachedHigh - 120.0f / 127.0f) > 0.01f)
        {
            ++gViolations;
            std::printf ("  STUCK the reading does not follow the hand while the arp plays\n");
        }

        // And the criterion itself, on the way out.
        r.noteOff (2, 60); r.armKeyUp (60);
        r.run (80);
        r.noteOff (3, 64); r.armKeyUp (64);
        r.noteOff (4, 67); r.armKeyUp (67);
        // Read it while the tails are still SOUNDING. Waiting for silence first
        // asks the question of an empty voice pool, which every version of the
        // code answers with 0 -- the reading has to be taken at the one moment
        // the two answers differ, with the chord decaying and no finger on it.
        r.run (8);
        // Printed, not asserted, and deliberately: with no key down the held-key
        // list is empty and no voice is key-held, so both halves of the reading
        // are 0 by construction. An assertion here could not fail whatever the
        // instrument did -- it would read as a guarantee and check nothing. The
        // question it looks like it is asking (does a released note keep
        // driving anything?) is asked where it can actually fail: by the frozen
        // -Z watch above, and by cases 55 and 58 in tools/test_mpe_parity.cpp.
        std::printf ("   reading after every finger has left: %.4f  (read-out, not a gate)\n",
                     reading());
        if (reading() > 1e-3f)
        {
            ++gViolations;
            std::printf ("  HELD  a reading survives with no hand on the instrument\n");
        }
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("\naftertouch acceptance gestures, measured\n");
    std::printf ("real T5ynthProcessor::processBlock; Z = the voice's pressure, "
                 "lvl = ampEnv * Z = its gain at AT -> DCA full\n");
    std::printf ("criterion: after a key comes up, that voice's Z is FROZEN -- "
                 "it neither swells nor collapses\n");

    gestureRepeatedStrikes();
    gestureOverlappingRelease();
    gestureSustainPedal();
    gestureArpWithCacheSnap();

    std::printf ("\n%s -- %d violation(s)\n\n",
                 gViolations == 0 ? "ALL CLEAR" : "FAILED", gViolations);
    return gViolations == 0 ? 0 : 1;
}
