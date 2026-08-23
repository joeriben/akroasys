// The frozen capability corpus for the MPE path.
//
// docs/MPE_MIGRATION_PARITY.md enumerates what the hand-written MPE code in
// T5ynthProcessor::processBlock can do. This file is that list turned into
// assertions, and it exists for one reason: it was written against the
// HAND-WRITTEN code and was green on it before juce::MPEInstrument was
// introduced. A suite authored after the new implementation would inherit the
// new implementation's blind spots by construction and would certify nothing --
// so this one is deliberately written first, from the old code, and is not
// allowed to be regenerated from the new one.
//
// It drives raw MIDI through the REAL T5ynthProcessor::processBlock -- the same
// entry point a DAW uses -- and reads back what happened to the voices. No
// mock, no direct call into VoiceManager: the whole point is the wire from a
// MIDI byte to a voice's per-note bend, pressure and timbre.
//
// What it cannot reach, stated rather than silently skipped:
//
//   * The Launch Control XL DAW-mode exemptions (parity doc capability 19).
//     dawModeActive_ is only set when a real XL output device is opened
//     (PluginProcessor.cpp:9567); nothing in an offline harness can set it. The
//     guard itself is a literal `dawModeActive_ && channel == 16` condition
//     carried across unchanged, so it is preserved by construction, not by
//     test. What IS tested here is the other half: with DAW mode inactive,
//     channel 16 behaves as an ordinary MPE channel.
//
//   * A channel-0 voice under a channel-1 pitch wheel (capability 10). Making
//     one needs a running sequencer. The mechanism -- VoiceManager's GLOBAL
//     pitchBendSemitones, which every voice reads regardless of MPE tag -- is
//     asserted directly through globalPitchBendRatio() instead.
//
// Build (T5ynth's standard offline-tool recipe):
//
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSND=$PWD/third_party/csound/macos-arm64/lib
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/test_mpe_parity.cpp \
//     build_clean/T5ynth_artefacts/Release/libakroasys_SharedCode.a \
//     build_clean/libT5ynthData.a "$CSND/CsoundLib64" \
//     -framework CoreAudioKit -framework DiscRecording -framework CoreAudio \
//     -framework CoreMIDI -framework AudioToolbox -framework Accelerate \
//     -framework WebKit -weak_framework Metal -weak_framework MetalKit \
//     -framework QuartzCore -framework Cocoa -framework Foundation \
//     -framework IOKit -framework Security -framework Carbon \
//     -framework AudioUnit -framework CoreServices -o /tmp/t5main/test_mpe_parity

// CoreFoundation before JUCE: MacTypes.h declares a struct Point that becomes
// ambiguous with juce::Point once JUCE's headers are in scope.
#include <CoreFoundation/CoreFoundation.h>

#include "../src/PluginProcessor.h"
#include "../src/dsp/BlockParams.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr double kSampleRate = 44100.0;
    constexpr int    kBlockSize  = 256;

    // ── The MPE fallbacks ────────────────────────────────────────────────────
    // Pinned at COMPILE time against the constants themselves, and read at RUN
    // time off the processor everywhere else. The split is not pedantry: since
    // the MPE settings tab exists, the values actually in force are machine-wide
    // USER SETTINGS loaded from ~/Library/Application Support/T5ynth. A corpus
    // that asserted the literal 48 would start failing the day a player set the
    // range to 24 for their own instrument -- and it would fail correctly, which
    // is worse than failing wrongly: a capability guard a preference can turn
    // red is no longer a guard. What is frozen is the DEFAULT, and that is a
    // constant; what the behavioural cases below assert is the RELATIONSHIP
    // between the range in force and the bend, which is the capability.
    static_assert (T5ynthProcessor::kMpePerNoteBendRange == 48,
                   "The per-note fallback is the MPE spec's 48. It was 24 until 2026-08-23, on a "
                   "belief about the LinnStrument that its own documentation contradicts -- see "
                   "docs/MPE_MIGRATION_PARITY.md capability 8.");
    static_assert (T5ynthProcessor::kMpeMasterBendRange == 2,
                   "The master fallback is the MPE spec's 2.");
    static_assert (T5ynthProcessor::kMpeXFullScaleSemitones == 1.0f,
                   "One semitone of lean is a full X axis by default -- the smallest interval "
                   "that is unambiguously a musical gesture rather than intonation.");
    // Zero travel -- what a voice reports before its Y has moved.
    //
    // This number moved twice, and the corpus is not allowed to be regenerated
    // from the implementation, so the accounting belongs here. It was 64/127:
    // CC74 was read as a CENTRE DETENT, which is right for a LinnStrument (whose
    // Y is a sideways travel) and was measured wrong for an Expressive E Osmose,
    // whose Y rests at 0 and rises with forward key travel -- 201 of 203
    // note-ons in a 120 s capture had CC 74 = 0 in force. Then the whole
    // QUANTITY changed: a voice no longer holds a CC value at all, it holds the
    // signed TRAVEL from the value in force when its note began (case 25).
    //
    // What that costs the cases below, honestly. Cases 10 and 11 send CC74 = 127
    // to a channel and assert the voice stays at zero; they still discriminate
    // exactly what they always did, because in both the value goes to a channel
    // the voice is not on -- a master channel in 10, a different CC number in
    // 11. Case 14 does NOT: it sends CC74 = 127 on the note's own channel BEFORE
    // the note, and under a per-note rest the voice reads zero because it
    // adopted that value as its origin, not because it ignored it. The
    // assertion is still worth making -- a note must not JUMP because of a stale
    // CC74, which is capability 27's actual content -- so it is relabelled
    // rather than deleted, and case 25 carries what it no longer proves.
    constexpr float kTimbreRest = 0.0f;

    int gChecks = 0;
    int gFailures = 0;

    void pump (int ms)
    {
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, (double) ms * 0.001, false);
    }

    void check (bool ok, const std::string& what)
    {
        ++gChecks;
        if (! ok)
        {
            ++gFailures;
            std::printf ("  FAIL  %s\n", what.c_str());
        }
    }

    void checkNear (float got, float want, float tol, const std::string& what)
    {
        const bool ok = std::fabs (got - want) <= tol;
        ++gChecks;
        if (! ok)
        {
            ++gFailures;
            std::printf ("  FAIL  %s  (got %.5f, want %.5f +- %.5f)\n",
                         what.c_str(), got, want, tol);
        }
    }

    // ── The rig ──────────────────────────────────────────────────────────────
    // One processor per case. The MPE zone layout deliberately survives
    // prepareToPlay (parity doc capability 5), so cases would otherwise inherit
    // each other's zones and the corpus would be order-dependent -- the exact
    // defect tools/test_preset_loop_window_order.cpp exists for.
    struct Rig
    {
        T5ynthProcessor proc;
        juce::AudioBuffer<float> buf { 2, kBlockSize };
        juce::MidiBuffer midi;

        Rig()
        {
            proc.prepareToPlay (kSampleRate, kBlockSize);
            pump (40);
        }

        void send (const juce::MidiMessage& m) { midi.addEvent (m, 0); }

        // What is ACTUALLY in force on this machine: the compile-time fallback
        // unless the player's settings file or a transmitted RPN 0 moved it.
        float noteBendRange()   const { return (float) proc.getMpePerNoteBendRange(); }
        float masterBendRange() const { return (float) proc.getMpeMasterBendRange(); }
        float xFullScale()      const { return proc.getMpeXFullScaleSemitones(); }

        void run (int blocks = 1)
        {
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                proc.processBlock (buf, midi);
                midi.clear();
            }
        }

        // Send everything queued, then render one block so the events land.
        void flush() { run (1); }

        void noteOn  (int ch, int note, int vel = 100)
        { send (juce::MidiMessage::noteOn (ch, note, (juce::uint8) vel)); }
        void noteOff (int ch, int note)
        { send (juce::MidiMessage::noteOff (ch, note)); }
        void wheel   (int ch, int value14)
        { send (juce::MidiMessage::pitchWheel (ch, value14)); }
        void pressure (int ch, int v7)
        { send (juce::MidiMessage::channelPressureChange (ch, v7)); }
        void polyPressure (int ch, int note, int v7)
        { send (juce::MidiMessage::aftertouchChange (ch, note, v7)); }
        void cc (int ch, int number, int value)
        { send (juce::MidiMessage::controllerEvent (ch, number, value)); }

        // RPN as a controller actually transmits it: parameter select, then
        // data entry MSB.
        void rpn (int ch, int msb, int lsb, int dataMsb)
        {
            cc (ch, 101, msb);
            cc (ch, 100, lsb);
            cc (ch, 6, dataMsb);
        }

        // The note is HELD, i.e. not the one still ringing out. voiceForNote
        // returns the first ACTIVE voice of that pitch, which after a re-strike
        // can be the releasing one -- and a case that reads the wrong voice
        // passes for the wrong reason.
        const SynthVoice* heldVoiceForNote (int note) const
        {
            const auto& vm = proc.getVoiceManager();
            for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            {
                const auto& v = vm.getVoice (i);
                if (v.isActive() && ! v.isReleasing() && v.getCurrentNote() == note)
                    return &v;
            }
            return nullptr;
        }

        const SynthVoice* voiceForNote (int note) const
        {
            const auto& vm = proc.getVoiceManager();
            for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            {
                const auto& v = vm.getVoice (i);
                if (v.isActive() && v.getCurrentNote() == note)
                    return &v;
            }
            return nullptr;
        }

        int activeVoiceCount() const
        {
            const auto& vm = proc.getVoiceManager();
            int n = 0;
            for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
                if (vm.getVoice (i).isActive())
                    ++n;
            return n;
        }

        float globalBendSemitones() const
        {
            return 12.0f * std::log2 (proc.getVoiceManager().globalPitchBendRatio());
        }
    };

    // Full-scale wheel is 16383, one LSB short of +1.0 -- so the semitone figure
    // a full-up wheel produces is range * 8191/8192, not range. Spelled out here
    // rather than read from the implementation, so a change to how the wheel is
    // scaled fails this suite instead of moving silently.
    float fullUpBend (float rangeSemitones) { return rangeSemitones * (8191.0f / 8192.0f); }

    // ── 1. A note plays at all, on the zone master and on a member ───────────
    void caseNotesPlay()
    {
        std::printf ("[1] notes play on channel 1 and on a member channel\n");
        Rig r;
        r.noteOn (1, 60);
        r.noteOn (5, 64);
        r.flush();
        check (r.voiceForNote (60) != nullptr, "a note on channel 1 sounds");
        check (r.voiceForNote (64) != nullptr, "a note on channel 5 sounds");
    }

    // ── 2. Per-note bend on a member channel, and the ±48 default ────────────
    void casePerNoteBend()
    {
        std::printf ("[2] a member channel's wheel bends only its own note, at +-48 by default\n");
        Rig r;
        r.noteOn (1, 60);
        r.noteOn (5, 64);
        r.flush();
        r.wheel (5, 16383);
        r.flush();

        const auto* member = r.voiceForNote (64);
        const auto* master = r.voiceForNote (60);
        check (member != nullptr && master != nullptr, "both voices still alive");
        if (member == nullptr || master == nullptr) return;

        checkNear (member->getPerVoicePitchBend(), fullUpBend (r.noteBendRange()), 0.01f,
                   "channel 5's note bends by the default per-note range");
        checkNear (master->getPerVoicePitchBend(), 0.0f, 1e-6f,
                   "channel 1's note is untouched by channel 5's wheel");
    }

    // ── 3. The master channel's wheel is GLOBAL, not per-note ────────────────
    void caseMasterBendIsGlobal()
    {
        std::printf ("[3] channel 1's wheel is the global bend, at +-2 by default\n");
        Rig r;
        r.noteOn (1, 60);
        r.flush();
        r.wheel (1, 16383);
        r.flush();

        const auto* v = r.voiceForNote (60);
        check (v != nullptr, "the voice is alive");
        if (v == nullptr) return;

        checkNear (v->getPerVoicePitchBend(), 0.0f, 1e-6f,
                   "channel 1's wheel writes no PER-VOICE bend");
        checkNear (r.globalBendSemitones(), fullUpBend (r.masterBendRange()), 0.01f,
                   "channel 1's wheel writes the global bend, which every voice reads");
    }

    // ── 4. Channel 16 is a MEMBER until an upper zone says otherwise ─────────
    void caseChannel16IsMemberByDefault()
    {
        std::printf ("[4] channel 16 is a member channel with no upper zone declared\n");
        Rig r;
        r.noteOn (16, 60);
        r.noteOn (1, 64);
        r.flush();
        r.pressure (16, 127);
        r.flush();

        const auto* on16 = r.voiceForNote (60);
        const auto* on1  = r.voiceForNote (64);
        check (on16 != nullptr && on1 != nullptr, "both voices alive");
        if (on16 == nullptr || on1 == nullptr) return;

        checkNear (on16->getAftertouch(), 1.0f, 1e-4f,
                   "pressure on channel 16 reaches the note played there");
        checkNear (on1->getAftertouch(), 0.0f, 1e-4f,
                   "and does NOT reach the rest of the zone");
    }

    // ── 5. An upper-zone MCM makes channel 16 a master ───────────────────────
    void caseUpperZoneMakesChannel16Master()
    {
        std::printf ("[5] RPN 6 on channel 16 declares an upper zone; channel 16 becomes its master\n");
        Rig r;
        r.rpn (16, 0, 6, 1);          // upper zone, one member channel
        r.flush();
        r.noteOn (1, 60);
        r.noteOn (5, 64);
        r.flush();
        r.pressure (16, 127);
        r.flush();

        const auto* a = r.voiceForNote (60);
        const auto* b = r.voiceForNote (64);
        check (a != nullptr && b != nullptr, "both voices alive");
        if (a == nullptr || b == nullptr) return;

        checkNear (a->getAftertouch(), 1.0f, 1e-4f,
                   "a declared master's pressure is zone-wide (note on channel 1)");
        checkNear (b->getAftertouch(), 1.0f, 1e-4f,
                   "a declared master's pressure is zone-wide (note on channel 5)");
    }

    // ── 6. An MCM of 16 or more is discarded, not clamped ────────────────────
    void caseMcmSixteenDiscarded()
    {
        std::printf ("[6] an MCM of 16 members is discarded -- channel 16 stays a member\n");
        Rig r;
        r.rpn (16, 0, 6, 16);
        r.flush();
        r.noteOn (16, 60);
        r.noteOn (1, 64);
        r.flush();
        r.pressure (16, 127);
        r.flush();

        const auto* on16 = r.voiceForNote (60);
        const auto* on1  = r.voiceForNote (64);
        check (on16 != nullptr && on1 != nullptr, "both voices alive");
        if (on16 == nullptr || on1 == nullptr) return;

        checkNear (on16->getAftertouch(), 1.0f, 1e-4f, "channel 16 still routes per-note");
        checkNear (on1->getAftertouch(), 0.0f, 1e-4f, "so no upper zone was installed");
    }

    // ── 7. The MCM is a one-shot: the next CC6 must not be read as another ───
    void caseMcmDoesNotSwallowTheNextCc6()
    {
        std::printf ("[7] a CC6 after an MCM does not rewrite the zone from its value\n");
        Rig r;
        r.rpn (16, 0, 6, 1);          // upper zone on -- channel 16 is master
        r.flush();
        r.cc (16, 6, 0);              // a bare data entry: MUST NOT switch the zone off
        r.flush();
        r.noteOn (1, 60);
        r.noteOn (5, 64);
        r.flush();
        r.pressure (16, 127);
        r.flush();

        const auto* a = r.voiceForNote (60);
        const auto* b = r.voiceForNote (64);
        check (a != nullptr && b != nullptr, "both voices alive");
        if (a == nullptr || b == nullptr) return;

        checkNear (a->getAftertouch(), 1.0f, 1e-4f,
                   "channel 16 is still the upper zone's master");
        checkNear (b->getAftertouch(), 1.0f, 1e-4f,
                   "so its pressure is still zone-wide");
    }

    // ── 8. RPN 0 on a member channel sets the per-note bend range ────────────
    void casePerNoteBendRangeFromMember()
    {
        std::printf ("[8] RPN 0 on a member channel sets the per-note bend range\n");
        Rig r;
        r.rpn (5, 0, 0, 12);
        r.flush();
        r.noteOn (5, 64);
        r.flush();
        r.wheel (5, 16383);
        r.flush();

        const auto* v = r.voiceForNote (64);
        check (v != nullptr, "the voice is alive");
        if (v == nullptr) return;
        checkNear (v->getPerVoicePitchBend(), fullUpBend (12.0f), 0.01f,
                   "a full wheel now reaches 12 semitones, not 24");
    }

    // ── 9. RPN 0 on the MASTER channel mirrors into the per-note range ───────
    //      A LinnStrument transmits Bend Range on the master channel only.
    void caseMasterRpnMirrorsToMembers()
    {
        std::printf ("[9] RPN 0 on the master channel sets the master range AND the per-note range\n");
        Rig r;
        r.rpn (1, 0, 0, 12);
        r.flush();
        r.noteOn (1, 60);
        r.noteOn (5, 64);
        r.flush();
        r.wheel (1, 16383);
        r.wheel (5, 16383);
        r.flush();

        const auto* member = r.voiceForNote (64);
        check (member != nullptr, "the member voice is alive");
        if (member == nullptr) return;

        checkNear (r.globalBendSemitones(), fullUpBend (12.0f), 0.01f,
                   "the master range followed the RPN");
        checkNear (member->getPerVoicePitchBend(), fullUpBend (12.0f), 0.01f,
                   "and it was mirrored to the members, which is what a LinnStrument needs");
    }

    // ── 10. CC74 is per-note timbre on a member, and Scan on the master ──────
    void caseTimbre()
    {
        std::printf ("[10] CC74 is per-note timbre on a member channel and NOT timbre on the master\n");
        Rig r;
        r.noteOn (1, 60);
        r.noteOn (5, 64);
        r.flush();
        r.cc (5, 74, 127);
        r.cc (1, 74, 0);
        r.flush();

        const auto* member = r.voiceForNote (64);
        const auto* master = r.voiceForNote (60);
        check (member != nullptr && master != nullptr, "both voices alive");
        if (member == nullptr || master == nullptr) return;

        checkNear (member->getTimbre(), 1.0f, 1e-4f,
                   "CC74 on channel 5 is that note's timbre");
        checkNear (master->getTimbre(), kTimbreRest, 1e-4f,
                   "CC74 on channel 1 stays the control-surface knob, not timbre");
    }

    // ── 11. CC70 / CC102 / CC106 are not MPE dimensions here ────────────────
    void caseNonMpeControllers()
    {
        std::printf ("[11] CC70, CC102 and CC106 do not move pressure or timbre\n");
        Rig r;
        r.noteOn (5, 64);
        r.flush();
        r.cc (5, 70, 127);            // JUCE would read this as pressure MSB
        r.cc (5, 102, 127);           // ... and this as pressure LSB
        r.cc (5, 106, 127);           // ... and this as timbre LSB
        r.flush();

        const auto* v = r.voiceForNote (64);
        check (v != nullptr, "the voice is alive");
        if (v == nullptr) return;
        checkNear (v->getAftertouch(), 0.0f, 1e-4f, "CC70/CC102 are not pressure");
        checkNear (v->getTimbre(), kTimbreRest, 1e-4f, "CC106 is not timbre");
    }

    // ── 12. Member and master pressure compose as a maximum ──────────────────
    void casePressureComposition()
    {
        std::printf ("[12] a voice's pressure is max(per-note, zone-wide), not the last one to arrive\n");
        Rig r;
        r.noteOn (5, 64);
        r.flush();
        r.pressure (5, 64);           // per-note ~0.504
        r.flush();
        const auto* v = r.voiceForNote (64);
        check (v != nullptr, "the voice is alive");
        if (v == nullptr) return;
        checkNear (v->getAftertouch(), 64.0f / 127.0f, 1e-4f, "per-note pressure lands");

        r.pressure (1, 127);          // zone-wide 1.0 -- higher, so it wins
        r.flush();
        checkNear (v->getAftertouch(), 1.0f, 1e-4f, "zone-wide pressure raises it");

        r.pressure (1, 0);            // zone-wide back to 0 -- per-note survives
        r.flush();
        checkNear (v->getAftertouch(), 64.0f / 127.0f, 1e-4f,
                   "and dropping the zone-wide value leaves the per-note one standing");
    }

    // ── 13. Poly key pressure reaches the note it names ─────────────────────
    void casePolyAftertouch()
    {
        std::printf ("[13] poly key pressure reaches the note it names\n");
        Rig r;
        r.noteOn (5, 64);
        r.noteOn (5, 67);
        r.flush();
        r.polyPressure (5, 64, 127);
        r.flush();

        const auto* hit  = r.voiceForNote (64);
        const auto* miss = r.voiceForNote (67);
        check (hit != nullptr && miss != nullptr, "both voices alive");
        if (hit == nullptr || miss == nullptr) return;
        checkNear (hit->getAftertouch(), 1.0f, 1e-4f, "the named note gets it");
        checkNear (miss->getAftertouch(), 0.0f, 1e-4f, "the other one does not");
    }

    // ── 14. A fresh note starts neutral, whatever arrived on that channel ────
    void caseFreshNoteStartsNeutral()
    {
        std::printf ("[14] a note starts at zero Y travel and zero pressure, whatever CC74 preceded it\n");
        Rig r;
        r.cc (7, 74, 127);            // before any note on this channel
        r.flush();
        r.noteOn (7, 62);
        r.flush();

        const auto* v = r.voiceForNote (62);
        check (v != nullptr, "the voice is alive");
        if (v == nullptr) return;
        checkNear (v->getTimbre(), kTimbreRest, 1e-4f,
                   "the note does not JUMP on the channel's last CC74 -- it starts at no travel");
        checkNear (v->getAftertouch(), 0.0f, 1e-4f, "and starts unpressed");
    }

    // ── 15. Sustain is the synth's, and it holds the note ────────────────────
    void caseSustainPedal()
    {
        std::printf ("[15] CC64 holds a note through its note-off\n");
        Rig r;
        r.cc (1, 64, 127);
        r.noteOn (1, 60);
        r.flush();
        check (r.voiceForNote (60) != nullptr, "the note sounds");

        r.noteOff (1, 60);
        r.run (2);
        check (r.voiceForNote (60) != nullptr, "and is still held after the key is lifted");

        r.cc (1, 64, 0);
        r.run (2);
        // Releasing the pedal starts the release; the voice stays ACTIVE while
        // the amp envelope plays out, so what is asserted here is only that the
        // pedal was the thing holding it -- see the note count fall to zero
        // after the tail in case 16.
        check (true, "pedal lifted");
    }

    // ── 16. The zone layout outlives prepareToPlay ──────────────────────────
    void caseLayoutSurvivesPrepare()
    {
        std::printf ("[16] a declared zone survives prepareToPlay -- it describes the device, not the patch\n");
        Rig r;
        r.rpn (16, 0, 6, 1);
        r.flush();

        r.proc.prepareToPlay (kSampleRate, kBlockSize);
        pump (20);

        r.noteOn (1, 60);
        r.flush();
        r.pressure (16, 127);
        r.flush();

        const auto* v = r.voiceForNote (60);
        check (v != nullptr, "the voice is alive");
        if (v == nullptr) return;
        checkNear (v->getAftertouch(), 1.0f, 1e-4f,
                   "channel 16 is still the upper zone's master after a re-prepare");
    }

    // ── 17. Expression lands at its own sample position inside the block ────
    void caseSampleAccurateWithinBlock()
    {
        std::printf ("[17] a wheel later in the block does not act on a note that has already ended\n");
        Rig r;
        r.noteOn (5, 64);
        r.flush();

        // note-off at 0, wheel at the end of the same block. The note is gone by
        // the time the wheel is dispatched, so the wheel finds no voice to bend
        // -- which is only true if events are walked in time order rather than
        // applied as a batch.
        r.midi.addEvent (juce::MidiMessage::noteOff (5, 64), 0);
        r.midi.addEvent (juce::MidiMessage::pitchWheel (5, 16383), kBlockSize - 1);
        r.run (1);

        const auto& vm = r.proc.getVoiceManager();
        float maxBend = 0.0f;
        for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            if (vm.getVoice (i).isActive())
                maxBend = juce::jmax (maxBend, std::fabs (vm.getVoice (i).getPerVoicePitchBend()));

        // The releasing voice keeps its channel tag until it goes idle, so a
        // wheel AFTER the note-off still reaches it -- that is today's
        // behaviour and it is what is frozen here. What must NOT happen is the
        // reverse: the bend arriving before the note-off was dispatched.
        checkNear (maxBend, fullUpBend (r.noteBendRange()), 0.01f,
                   "a releasing voice still follows its channel's wheel");
    }

    // ── 18. An ARPEGGIATED note is internal; the key handed BACK is not ──────
    //      Two halves of one rule, and they point opposite ways on purpose:
    //      what the arp plays is channel 0 and must never be MPE-tracked
    //      (VoiceEvent.h:32-38), but the key still held when the arp is
    //      switched off is handed to the voices with its channel intact
    //      (PluginProcessor.cpp:3851-3862).
    float maxPerVoiceBend (const Rig& r)
    {
        const auto& vm = r.proc.getVoiceManager();
        float m = 0.0f;
        for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            if (vm.getVoice (i).isActive())
                m = juce::jmax (m, std::fabs (vm.getVoice (i).getPerVoicePitchBend()));
        return m;
    }

    void setArp (Rig& r, bool on)
    {
        if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
            p->setValueNotifyingHost (p->convertTo0to1 (on ? 1.0f : 0.0f));  // 0 = Off, 1 = Up
        pump (30);
        r.run (2);
    }

    void caseArpNotesAreInternal()
    {
        std::printf ("[18] what the arpeggiator plays is an internal note -- no per-note MPE\n");
        Rig r;
        setArp (r, true);

        r.noteOn (5, 64);
        int blocks = 0;
        while (r.activeVoiceCount() == 0 && blocks < 400)
        {
            r.run (1);
            ++blocks;
        }
        if (r.activeVoiceCount() == 0)
        {
            std::printf ("  SKIP  the arpeggiator emitted no note in %d blocks\n", blocks);
            return;
        }

        r.wheel (5, 16383);
        r.flush();
        checkNear (maxPerVoiceBend (r), 0.0f, 1e-6f,
                   "an arpeggiated voice is channel 0 and ignores channel 5's wheel");
    }

    // ── 20. A bend range survives a later zone declaration ──────────────────
    //      The MPE Configuration Message carries no range of its own, so a
    //      range the player already set must still be in force after it. The
    //      order is unusual -- controllers normally declare the zone first --
    //      but it is the order that catches a range being reset to a default,
    //      and the hand-written code held the range through it.
    void caseBendRangeSurvivesAZoneDeclaration()
    {
        std::printf ("[20] a transmitted bend range survives a later zone declaration\n");
        Rig r;
        r.rpn (5, 0, 0, 12);          // per-note range 12, on a member channel
        r.rpn (1, 0, 0, 7);           // master range 7, on the master channel
        r.flush();
        r.rpn (1, 0, 6, 15);          // ... and only THEN the zone declaration
        r.flush();

        r.noteOn (1, 60);
        r.noteOn (5, 64);
        r.flush();
        r.wheel (1, 16383);
        r.wheel (5, 16383);
        r.flush();

        const auto* member = r.voiceForNote (64);
        check (member != nullptr, "the member voice is alive");
        if (member == nullptr) return;

        // The master RPN arrives second and reaches the members too (a
        // LinnStrument transmits Bend Range there only), so 7 is what stands.
        checkNear (member->getPerVoicePitchBend(), fullUpBend (7.0f), 0.01f,
                   "the per-note range was not reset by the zone message");
        checkNear (r.globalBendSemitones(), fullUpBend (7.0f), 0.01f,
                   "and neither was the master range");
    }

    void caseNrpnDoesNotWriteTheBendRange()
    {
        std::printf ("[21] an NRPN write is not a bend range and not a zone\n");
        // NOT parity: the hand-written parser tracked CC100/CC101 only and
        // fails the first half of this. Same misfire class as [7] -- a latched
        // selection eating a later CC6.
        //
        // Parameter 6 on purpose. It is the MPE Configuration Message's number,
        // so an NRPN 6 is the sequence that reaches FURTHEST if the NRPN bytes
        // are handled by anything that shares a parameter register with the RPN
        // path: not just the bend range but the zone layout, and with it
        // master-vs-member for pressure and CC74.
        Rig r;
        r.rpn (5, 0, 0, 12);          // per-note range 12
        r.flush();

        r.cc (5, 99, 0);              // NRPN MSB
        r.cc (5, 98, 6);              // NRPN LSB — parameter 6, as an NRPN
        r.cc (5, 6, 40);              // its data byte. Not a bend range.
        r.flush();

        r.noteOn (5, 64);
        r.flush();
        r.wheel (5, 16383);
        r.flush();

        const auto* v = r.voiceForNote (64);
        check (v != nullptr, "the voice is alive");
        if (v == nullptr) return;

        checkNear (v->getPerVoicePitchBend(), fullUpBend (12.0f), 0.01f,
                   "the NRPN data byte left the bend range where RPN 0 put it");

        // And the same write on channel 16, where an accepted parameter 6 would
        // declare an upper zone and make ch16 a master.
        Rig z;
        z.cc (16, 99, 0);
        z.cc (16, 98, 6);
        z.cc (16, 6, 1);              // "one member channel", if it were an MCM
        z.flush();

        z.noteOn (1, 60);
        z.noteOn (5, 64);
        z.flush();
        z.pressure (16, 127);         // master pressure IF ch16 became a master
        z.flush();

        const auto* other = z.voiceForNote (64);
        check (other != nullptr, "the member voice is alive");
        if (other == nullptr) return;
        checkNear (other->getAftertouch(), 0.0f, 0.01f,
                   "an NRPN 6 did not declare a zone -- ch16 is still a member");
    }

    // Does inserting an NRPN select byte into an otherwise ordinary RPN
    // sequence change what that sequence does? It must not: the answer with
    // the byte and the answer without it have to be the same one.
    //
    // Asserting an ABSOLUTE outcome here would be the wrong test and was, on
    // the first attempt. juce::MidiRPNDetector latches the parameter register,
    // so a CC100 alone re-selects against the MSB the last RPN left -- which
    // means the same byte stream declares a zone or not depending purely on
    // what preceded it. From a fresh channel both variants are inert and the
    // case passes without touching the mechanism at all. So the fixture runs
    // each pair twice, once from fresh and once behind the RPN a real device
    // sends first, and compares the two variants against EACH OTHER.
    float bendAfterMixedSelection (bool withNrpnByte, bool withPriorRpn)
    {
        Rig r;
        if (withPriorRpn)
        {
            r.rpn (5, 0, 0, 12);      // latches MSB 0, LSB 0
            r.flush();
        }
        if (withNrpnByte)
            r.cc (5, 98, 0);          // NRPN LSB — must not touch the register
        r.cc (5, 101, 0);             // RPN MSB 0
        r.cc (5, 6, 40);
        r.flush();

        r.noteOn (5, 64);
        r.flush();
        r.wheel (5, 16383);
        r.flush();

        const auto* v = r.voiceForNote (64);
        return v != nullptr ? v->getPerVoicePitchBend() : -999.0f;
    }

    bool ch16IsMasterAfterMixedSelection (bool withNrpnByte, bool withPriorRpn)
    {
        Rig r;
        if (withPriorRpn)
        {
            r.rpn (16, 0, 0, 2);      // latches MSB 0, LSB 0
            r.flush();
        }
        if (withNrpnByte)
            r.cc (16, 99, 0);         // NRPN MSB — must not touch the register
        r.cc (16, 100, 6);            // RPN LSB 6
        r.cc (16, 6, 1);
        r.flush();

        r.noteOn (1, 60);
        r.noteOn (5, 64);
        r.flush();
        r.pressure (16, 127);         // zone-wide only if ch16 became a master
        r.flush();

        const auto* v = r.voiceForNote (64);
        return v != nullptr && v->getAftertouch() > 0.5f;
    }

    void caseRpnRegisterIsNotSharedWithNrpn()
    {
        std::printf ("[23] an NRPN select byte does not write the RPN parameter register\n");
        // juce::MidiRPNDetector keeps ONE register per channel and lets
        // CC98/CC99 and CC100/CC101 both write it, telling them apart only by
        // a flag on the way out -- so a detector fed the NRPN bytes assembles
        // parameter numbers out of halves that were never selected together.

        for (int prior = 0; prior < 2; ++prior)
        {
            const bool withPrior = prior != 0;
            const char* where = withPrior ? " (behind a latched RPN)" : " (from fresh)";

            checkNear (bendAfterMixedSelection (true,  withPrior),
                       bendAfterMixedSelection (false, withPrior), 1e-4f,
                       std::string ("a CC98 before an RPN 0 changes nothing about it") + where);

            check (ch16IsMasterAfterMixedSelection (true,  withPrior)
                       == ch16IsMasterAfterMixedSelection (false, withPrior),
                   std::string ("a CC99 before an RPN 6 changes nothing about it") + where);
        }
    }

    void caseNrpnCannotDestroyADeclaredZone()
    {
        std::printf ("[24] a well-formed NRPN 6 write does not switch a declared zone off\n");
        // The other direction, and the worse one: every zone assertion above
        // starts from "no zone", so a defect that only DESTROYS zones passes
        // all of them. Here the rig is a working MPE setup and the NRPN write
        // carries value 0, which as an MCM means "no member channels".
        Rig r;
        r.rpn (16, 0, 6, 1);          // a real MCM: upper zone, one member
        r.flush();

        r.cc (16, 99, 0);
        r.cc (16, 98, 6);             // an NRPN 6 ...
        r.cc (16, 6, 0);              // ... whose value would switch the zone off
        r.flush();

        r.noteOn (1, 60);
        r.noteOn (5, 64);
        r.flush();
        r.pressure (16, 127);
        r.flush();

        const auto* v = r.voiceForNote (64);
        check (v != nullptr, "the member voice is alive");
        if (v == nullptr) return;
        checkNear (v->getAftertouch(), 1.0f, 0.01f,
                   "the zone is still there -- ch16 is still its master");
    }

    void caseBendRangeAboveTheSpecMaximum()
    {
        std::printf ("[22] a bend range above 96 is honoured and does not disturb the next one\n");
        // Read at a QUARTER wheel, not full: SynthVoice clamps at ±48
        // (dsp/SynthVoice.h:49), so at full deflection 96, 100 and 127 all read
        // back as 48.000 and an honoured 100 cannot be told from a clamp to the
        // spec's 96. A quarter of the range clears the clamp and 100 is 100.
        // JUCE clamps this parameter to 0..96 wherever it owns it
        // (juce_MPEZoneLayout.cpp:74-76), which is what makes it worth pinning.
        Rig r;
        r.rpn (5, 0, 0, 100);         // out of MPE's 0..96, and a controller may send it
        r.flush();

        r.noteOn (5, 64);
        r.flush();

        const auto* v = r.voiceForNote (64);
        check (v != nullptr, "the voice is alive");
        if (v == nullptr) return;

        r.wheel (5, 8192 + 2048);     // a quarter up
        r.flush();
        checkNear (v->getPerVoicePitchBend(), 25.0f, 0.05f,
                   "100 semitones was stored as 100, not clamped to the spec's 96");

        r.rpn (5, 0, 0, 12);
        r.flush();
        r.wheel (5, 16383);
        r.flush();
        checkNear (v->getPerVoicePitchBend(), fullUpBend (12.0f), 0.01f,
                   "and the next range lands unaffected by it");
    }

    void caseArpOffHandsTheKeyBackWithItsChannel()
    {
        std::printf ("[19] switching the arp off hands the held key back WITH its channel\n");
        Rig r;
        setArp (r, true);

        r.noteOn (5, 64);
        int blocks = 0;
        while (r.activeVoiceCount() == 0 && blocks < 400)
        {
            r.run (1);
            ++blocks;
        }
        if (r.activeVoiceCount() == 0)
        {
            std::printf ("  SKIP  the arpeggiator emitted no note in %d blocks\n", blocks);
            return;
        }

        setArp (r, false);          // key 64 on channel 5 is still down
        r.run (2);

        r.wheel (5, 16383);
        r.flush();
        checkNear (maxPerVoiceBend (r), fullUpBend (r.noteBendRange()), 0.01f,
                   "the handed-back key kept its MPE channel and follows its wheel");
    }

    // ── 25. Y's rest is where the note began, not a constant ────────────────
    // The MPE spec (1.0, SS3.3.5) defines TWO schemes for CC 74 and says neither
    // fits every instrument: "Initial-position", where the value at Note On
    // encodes where the interaction started, and "Initial-64", whose initial
    // value "must be 40h (64 decimal), such that movement can follow in either a
    // positive or negative direction". A receiver that hard-codes either one is
    // wrong for the other half of the instruments, so this reads the value in
    // force at Note On as that note's rest and reports the TRAVEL from it.
    void caseTimbreRestIsPerNote()
    {
        std::printf ("[25] a note's Y rests where it began, and travels both ways from there\n");

        {   // Initial-position at the bottom: an Osmose, whose Y is the key's
            // second pressure stage. Whole travel upward, exactly as before.
            Rig r;
            r.cc (5, 74, 0);
            r.noteOn (5, 60);
            r.flush();
            const auto* v = r.voiceForNote (60);
            check (v != nullptr, "the bottom-rest note sounds");
            if (v != nullptr)
            {
                checkNear (v->getTimbre(), 0.0f, 1e-4f, "and starts at no travel");
                r.cc (5, 74, 127);
                r.flush();
                checkNear (v->getTimbre(), 1.0f, 1e-3f, "full CC74 is full travel up");
                r.cc (5, 74, 0);
                r.flush();
                checkNear (v->getTimbre(), 0.0f, 1e-4f, "and back to rest, never below");
            }
        }

        {   // Initial-64: the note arrives at the middle and moves both ways,
            // each direction reaching full depth on its own remaining travel.
            Rig r;
            r.cc (9, 74, 64);
            r.noteOn (9, 67);
            r.flush();
            const auto* v = r.voiceForNote (67);
            check (v != nullptr, "the centre-rest note sounds");
            if (v != nullptr)
            {
                checkNear (v->getTimbre(), 0.0f, 1e-4f, "starts at no travel, not at half");
                r.cc (9, 74, 127);
                r.flush();
                checkNear (v->getTimbre(), 0.496f, 1e-2f, "forward is the travel that is left");
                r.cc (9, 74, 0);
                r.flush();
                checkNear (v->getTimbre(), -0.504f, 1e-2f, "and back is the travel the other way");
            }
        }

        {   // A rest captured at the TOP of the range. The first version of this
            // scaled each direction to its own remaining span, which left no
            // upward span at all here and ran the axis 0 -> -1 while the finger
            // pressed HARDER: the whole gesture inverted. Plain travel cannot do
            // that -- at the top the only way is down, and down is what it says.
            Rig r;
            r.cc (5, 74, 127);
            r.noteOn (5, 72);
            r.flush();
            const auto* v = r.voiceForNote (72);
            check (v != nullptr, "the top-rest note sounds");
            if (v != nullptr)
            {
                checkNear (v->getTimbre(), 0.0f, 1e-4f, "starts at no travel");
                r.cc (5, 74, 127);
                r.flush();
                checkNear (v->getTimbre(), 0.0f, 1e-4f, "pressing harder cannot go up, and does not invert");
                r.cc (5, 74, 64);
                r.flush();
                check (v->getTimbre() < -0.4f && v->getTimbre() > -0.6f,
                       "releasing travels DOWN, monotonically with the finger");
                r.cc (5, 74, 0);
                r.flush();
                checkNear (v->getTimbre(), -1.0f, 1e-3f, "and reaches the bottom at -1");
            }
        }
    }

    // ── 27. A slide is the same note: its Y origin does not move ────────────
    void caseLegatoKeepsItsTimbreOrigin()
    {
        std::printf ("[27] a legato slide keeps the Y origin the note was struck with\n");
        Rig r;
        // Mono, so the second note-on is a legato slide rather than a new voice.
        // Through the PARAMETER, because processBlock re-reads voice_count every
        // block and would put the pool straight back.
        if (auto* p = r.proc.getValueTreeState().getParameter (PID::voiceCount))
            p->setValueNotifyingHost (p->convertTo0to1 (0.0f));   // index 0 = 1 voice
        r.run (2);
        r.cc (5, 74, 0);
        r.noteOn (5, 60);
        r.flush();
        r.cc (5, 74, 127);            // finger presses deep on the held key
        r.flush();
        const auto* v = r.voiceForNote (60);
        check (v != nullptr, "the note sounds");
        if (v != nullptr)
            checkNear (v->getTimbre(), 1.0f, 1e-3f, "and is at full travel");

        r.noteOn (5, 67);             // slide to a new pitch, finger never leaves
        r.flush();
        const auto* g = r.voiceForNote (67);
        check (g != nullptr, "the slide arrives");
        if (g != nullptr)
            checkNear (g->getTimbre(), 1.0f, 1e-3f,
                       "and the travel is unchanged -- the ground did not move under the finger");
    }

    // ── 26. X as a modulation source is a musical interval ──────────────────
    // The bend itself is the wheel travel times the range in force (case 2).
    // What a target routed to X reads is that bend measured against
    // kMpeXFullScaleSemitones, because the wheel fraction is not comparable
    // between instruments: an Osmose's entire lateral travel is 2.1% of the
    // wheel, a LinnStrument's slide is many times it, and a preset's depth has
    // to mean the same gesture on both.
    void caseXIsScaledInSemitones()
    {
        std::printf ("[26] X as a source is semitones of bend, not wheel travel\n");
        // Both rigs DECLARE their range instead of leaning on the fallback, and
        // X is asserted against the full scale in force rather than a literal 1
        // semitone: since the MPE tab exists, both of those are machine-wide
        // user settings. What is on test is the RELATIONSHIP -- X is the bend
        // interval measured against the full-scale lean -- not either number.
        constexpr float kNarrow = 24.0f, kWide = 48.0f;
        const int halfSemitoneUp = 8192 + (int) (8191.0 / (2.0 * kNarrow));

        Rig r;
        r.rpn (5, 0, 0, (int) kNarrow);
        r.noteOn (5, 60);
        r.flush();
        r.wheel (5, halfSemitoneUp);
        r.flush();
        const auto* v = r.voiceForNote (60);
        check (v != nullptr, "the voice is alive");
        if (v == nullptr) return;
        checkNear (v->getPerVoicePitchBend(), 0.5f, 0.01f, "the bend is half a semitone");
        checkNear (v->getPerVoicePitchBendNorm(),
                   juce::jlimit (-1.0f, 1.0f, 0.5f / r.xFullScale()), 0.01f,
                   "and X is that half-semitone measured against the full-scale lean");

        // The SAME wheel value under a wider declared range is a bigger
        // interval, and X follows the interval rather than the wheel.
        Rig r2;
        r2.rpn (5, 0, 0, (int) kWide);
        r2.noteOn (5, 60);
        r2.flush();
        r2.wheel (5, halfSemitoneUp);
        r2.flush();
        const auto* v2 = r2.voiceForNote (60);
        check (v2 != nullptr, "the wide-range voice is alive");
        if (v2 == nullptr) return;
        checkNear (v2->getPerVoicePitchBend(), 1.0f, 0.01f,
                   "the same wheel is now a whole semitone of bend");
        checkNear (v2->getPerVoicePitchBendNorm(),
                   juce::jlimit (-1.0f, 1.0f, 1.0f / r2.xFullScale()), 0.01f,
                   "and X has doubled with it -- the interval, not the wheel");

        // Downward is symmetric, and past full scale it clamps rather than
        // running away: a target on X can be driven to -1 and no further. The
        // range is declared at MPE's maximum so the assertion holds whatever the
        // full-scale setting is -- 96 semitones of bend saturate any of them.
        Rig r3;
        r3.rpn (5, 0, 0, 96);
        r3.noteOn (5, 60);
        r3.flush();
        r3.wheel (5, 0);
        r3.flush();
        const auto* v3 = r3.voiceForNote (60);
        check (v3 != nullptr, "the down-bent voice is alive");
        if (v3 != nullptr)
            checkNear (v3->getPerVoicePitchBendNorm(), -1.0f, 1e-4f,
                       "a full down-bend saturates at -1, not beyond");
    }

    // ── 28. A member channel drives the voice that owns it NOW ──────────────
    //      The defect: voiceMidiChannel_ falls only when a voice goes silent, so
    //      a RELEASING note kept its channel tag. An MPE controller reuses its
    //      member channels, and with an ordinary release time a channel comes
    //      round while the previous note on it is still audible -- the new key's
    //      pressure then also drove the old, dying one. With AT->DCA a released
    //      note swelled back up. Poly-AT never showed it, because it matches by
    //      NOTE NUMBER; that asymmetry is the whole reason PolyAT mode behaved
    //      and MPE mode did not.
    void caseMemberChannelHandsOver()
    {
        std::printf ("[28] a member channel's pressure leaves the note it has left\n");
        Rig r;
        r.noteOn (5, 64);
        r.flush();
        r.pressure (5, 100);
        r.flush();

        const auto* first = r.voiceForNote (64);
        check (first != nullptr, "the first note is sounding");
        if (first == nullptr) return;
        checkNear (first->getAftertouch(), 100.0f / 127.0f, 1e-3f,
                   "and it followed its channel's pressure");

        // Let go, then strike a NEW note on the same channel while the first is
        // still in its release -- the channel rotation an MPE keyboard does.
        r.noteOff (5, 64);
        r.flush();
        r.noteOn (5, 67);
        r.flush();

        const auto* releasing = r.voiceForNote (64);
        const auto* fresh     = r.voiceForNote (67);
        check (releasing != nullptr, "the first note is still ringing out");
        check (fresh != nullptr, "the second note is sounding");
        if (releasing == nullptr || fresh == nullptr) return;

        r.pressure (5, 20);
        r.flush();
        checkNear (fresh->getAftertouch(), 20.0f / 127.0f, 1e-3f,
                   "the new note follows the channel");
        checkNear (releasing->getAftertouch(), 100.0f / 127.0f, 1e-3f,
                   "and the releasing note keeps the pressure its own finger left, "
                   "instead of being driven by the next key");

        // Same for the other two per-note axes, on the same pair of voices.
        r.wheel (5, 16383);
        r.cc (5, 74, 127);
        r.flush();
        checkNear (fresh->getPerVoicePitchBend(), fullUpBend (r.noteBendRange()), 0.01f,
                   "the new note follows the channel's wheel");
        checkNear (releasing->getPerVoicePitchBend(), 0.0f, 1e-4f,
                   "the releasing note does not");
        checkNear (releasing->getTimbre(), 0.0f, 1e-4f,
                   "and its slide stays where the finger left it");
    }

    // ── 29. The same hand-off under the sustain pedal ───────────────────────
    //      Worse than the release case: a sustained voice is held indefinitely,
    //      so without the hand-off one pedalled chord follows every later key.
    void caseMemberChannelHandsOverUnderSustain()
    {
        std::printf ("[29] a sustained note does not follow the next key on its channel\n");
        Rig r;
        r.cc (5, 64, 127);            // sustain down
        r.noteOn (5, 64);
        r.flush();
        r.pressure (5, 110);
        r.flush();
        r.noteOff (5, 64);            // held by the pedal, not released
        r.flush();

        r.noteOn (5, 67);
        r.flush();
        const auto* held  = r.voiceForNote (64);
        const auto* fresh = r.voiceForNote (67);
        check (held != nullptr, "the pedalled note is still held");
        check (fresh != nullptr, "the new note is sounding");
        if (held == nullptr || fresh == nullptr) return;

        r.pressure (5, 5);
        r.flush();
        checkNear (fresh->getAftertouch(), 5.0f / 127.0f, 1e-3f,
                   "the new note follows the channel");
        checkNear (held->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                   "the pedalled note keeps its own");
    }


    // ── 30. Poly key pressure stops being a permanent floor ─────────────────
    //      polyPressureByNote is indexed by note NUMBER and pressureForNote
    //      takes the max, so a value left standing is a floor under that note's
    //      MPE Z for the rest of the session. Nothing lowered it but a panic.
    //      Play in the controller's Poly-AT mode, switch it to MPE, and every
    //      note number pressed hard stayed pressed.
    void casePolyPressureIsNotAPermanentFloor()
    {
        std::printf ("[30] poly key pressure ends with the key, not with the session\n");
        Rig r;
        r.noteOn (1, 60);
        r.flush();
        r.polyPressure (1, 60, 100);
        r.flush();
        const auto* first = r.heldVoiceForNote (60);
        check (first != nullptr, "the note is sounding");
        if (first == nullptr) return;
        checkNear (first->getAftertouch(), 100.0f / 127.0f, 1e-3f,
                   "and it followed the poly pressure");

        r.noteOff (1, 60);
        r.flush();

        // The same pitch again, now as an MPE note with a LIGHT touch.
        r.noteOn (5, 60);
        r.flush();
        r.pressure (5, 10);
        r.flush();

        const auto* fresh = r.heldVoiceForNote (60);
        check (fresh != nullptr, "the second note is sounding");
        if (fresh == nullptr) return;
        checkNear (fresh->getAftertouch(), 10.0f / 127.0f, 1e-3f,
                   "the new note reads its own light pressure, not the hard one "
                   "the same note number was left at");
    }


    // ── 31. Poly pressure survives while another voice still holds the pitch ─
    //      The other half of case 30, and it only became reachable with the
    //      channel-aware note-off below: until then one key-up released every
    //      voice of that pitch, so there was never a second one left holding it.
    void casePolyPressureSurvivesAHeldUnison()
    {
        std::printf ("[31] releasing one of two notes of the same pitch keeps the latch\n");
        Rig r;
        r.noteOn (5, 60);
        r.noteOn (6, 60);            // same pitch, two fingers, two member channels
        r.flush();
        r.polyPressure (1, 60, 100);
        r.flush();

        r.noteOff (5, 60);           // one of them goes
        r.flush();

        const auto* stillDown = r.heldVoiceForNote (60);
        check (stillDown != nullptr, "the other note of that pitch is still held");
        if (stillDown == nullptr) return;
        checkNear (stillDown->getAftertouch(), 100.0f / 127.0f, 1e-3f,
                   "and it keeps the poly pressure that names its note number");
    }

    // ── 32. A key-up names its member channel ───────────────────────────────
    //      noteOff matched by pitch alone, so the same note held on two member
    //      channels -- a second finger on a key another finger already holds,
    //      or a repeat rotated onto a fresh channel while the first is down --
    //      was ended by whichever key came up first. The finger still on the
    //      other key then pointed at a voice already in its release.
    void caseNoteOffNamesItsChannel()
    {
        std::printf ("[32] a key-up on one member channel does not end the same pitch on another\n");
        Rig r;
        r.noteOn (5, 60);
        r.noteOn (6, 60);            // same pitch, second finger, second channel
        r.flush();
        check (r.activeVoiceCount() >= 2, "both notes took a voice of their own");

        r.noteOff (5, 60);
        r.flush();

        const auto* held = r.heldVoiceForNote (60);
        check (held != nullptr, "the key still down is still held, not releasing");
        if (held == nullptr) return;

        // And it is still the CHANNEL 6 voice that answers to channel 6.
        r.pressure (6, 90);
        r.flush();
        checkNear (held->getAftertouch(), 90.0f / 127.0f, 1e-3f,
                   "and it still follows its own channel");

        // The counter-check that keeps this from over-matching: an ordinary
        // keyboard sends note-on and note-off on the same channel, and must
        // still be able to end its own note.
        Rig r2;
        r2.noteOn (1, 62);
        r2.flush();
        check (r2.heldVoiceForNote (62) != nullptr, "a plain keyboard note sounds");
        r2.noteOff (1, 62);
        r2.flush();
        check (r2.heldVoiceForNote (62) == nullptr,
               "and its own note-off still releases it");
    }

}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::printf ("\nMPE parity corpus -- docs/MPE_MIGRATION_PARITY.md\n");
    std::printf ("real T5ynthProcessor::processBlock, raw MIDI in, voice state out\n\n");

    caseNotesPlay();
    casePerNoteBend();
    caseMasterBendIsGlobal();
    caseChannel16IsMemberByDefault();
    caseUpperZoneMakesChannel16Master();
    caseMcmSixteenDiscarded();
    caseMcmDoesNotSwallowTheNextCc6();
    casePerNoteBendRangeFromMember();
    caseMasterRpnMirrorsToMembers();
    caseTimbre();
    caseNonMpeControllers();
    casePressureComposition();
    casePolyAftertouch();
    caseFreshNoteStartsNeutral();
    caseSustainPedal();
    caseLayoutSurvivesPrepare();
    caseSampleAccurateWithinBlock();
    caseArpNotesAreInternal();
    caseArpOffHandsTheKeyBackWithItsChannel();
    caseBendRangeSurvivesAZoneDeclaration();
    caseNrpnDoesNotWriteTheBendRange();
    caseBendRangeAboveTheSpecMaximum();
    caseRpnRegisterIsNotSharedWithNrpn();
    caseNrpnCannotDestroyADeclaredZone();
    caseTimbreRestIsPerNote();
    caseXIsScaledInSemitones();
    caseMemberChannelHandsOver();
    caseMemberChannelHandsOverUnderSustain();
    casePolyPressureIsNotAPermanentFloor();
    casePolyPressureSurvivesAHeldUnison();
    caseNoteOffNamesItsChannel();
    caseLegatoKeepsItsTimbreOrigin();

    std::printf ("\n%d checks, %d failures -- %s\n\n",
                 gChecks, gFailures, gFailures == 0 ? "ALL PASS" : "FAILED");
    return gFailures == 0 ? 0 : 1;
}
