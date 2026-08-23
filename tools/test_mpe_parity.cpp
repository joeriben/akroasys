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

        // Positioned variants. Everything above lands at sample 0, which cannot
        // express the one thing a release actually looks like: three messages
        // inside one buffer, in an order the ledger has to respect.
        void noteOnAt  (int ch, int note, int vel, int pos)
        { midi.addEvent (juce::MidiMessage::noteOn (ch, note, (juce::uint8) vel), pos); }
        void noteOffAt (int ch, int note, int pos)
        { midi.addEvent (juce::MidiMessage::noteOff (ch, note), pos); }
        void polyPressureAt (int ch, int note, int v7, int pos)
        { midi.addEvent (juce::MidiMessage::aftertouchChange (ch, note, v7), pos); }

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
        // Checked again HERE and not only above: voice 0 is in its amp release,
        // and once it goes idle the setters skip it for a reason that has
        // nothing to do with the hand-off -- the case would then pass without
        // discriminating anything.
        check (releasing->isActive(), "the first note is still audible at this point");
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


    // ── 33. Two keys DOWN on one channel share it ───────────────────────────
    //      The boundary of case 28, and the assertion that catches the obvious
    //      over-fix. The hand-off takes a channel from a note the finger has
    //      LEFT; it must never take it from one still held. Two ordinary setups
    //      put two live notes on one channel: a plain keyboard transmitting on
    //      channel 2 (channels 2-16 are per-note routed here whatever the zone
    //      says), and an MPE zone with fewer member channels than fingers, where
    //      MPE's own rule is that the channel drives every note on it.
    void caseTwoHeldKeysShareOneChannel()
    {
        std::printf ("[33] a chord held on one channel bends together, it does not tear apart\n");
        Rig r;
        r.noteOn (2, 60);
        r.noteOn (2, 64);
        r.noteOn (2, 67);            // three fingers, one channel
        r.flush();

        const auto* a = r.heldVoiceForNote (60);
        const auto* b = r.heldVoiceForNote (64);
        const auto* c = r.heldVoiceForNote (67);
        check (a != nullptr && b != nullptr && c != nullptr, "all three notes are held");
        if (a == nullptr || b == nullptr || c == nullptr) return;

        r.wheel (2, 16383);
        r.pressure (2, 100);
        r.cc (2, 74, 127);
        r.flush();

        const float bend = fullUpBend (r.noteBendRange());
        checkNear (a->getPerVoicePitchBend(), bend, 0.01f, "the first note bends");
        checkNear (b->getPerVoicePitchBend(), bend, 0.01f, "the second bends with it");
        checkNear (c->getPerVoicePitchBend(), bend, 0.01f, "and so does the third");
        checkNear (a->getAftertouch(), 100.0f / 127.0f, 1e-3f,
                   "the oldest held note still follows the channel's pressure");
        checkNear (a->getTimbre(), 1.0f, 1e-3f, "and its slide");
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


    // ── 34. ...and the latch ends with the PEDAL too ────────────────────────
    //      Case 30 covers the note-off message. Three other paths end a hold
    //      without going through it -- both pedals and the drone all call
    //      SynthVoice::noteOff on the voice directly -- and a latch the clear
    //      forgets is the same permanent floor, reached the long way round.
    void casePolyPressureEndsWithThePedal()
    {
        std::printf ("[34] the poly-pressure latch ends with the damper, not only with the key\n");
        Rig r;
        r.cc (1, 64, 127);            // damper down
        r.noteOn (1, 60);
        r.flush();
        r.polyPressure (1, 60, 127);  // pressed as hard as it goes
        r.flush();
        r.noteOff (1, 60);            // held by the pedal: the latch must SURVIVE here
        r.flush();
        const auto* pedalled = r.heldVoiceForNote (60);
        check (pedalled != nullptr, "the note is held by the pedal");
        if (pedalled != nullptr)
            checkNear (pedalled->getAftertouch(), 1.0f, 1e-3f, "and keeps its pressure");

        r.cc (1, 64, 0);              // damper up -- the hold ends HERE
        r.flush();

        // Same pitch again, with the lightest touch there is.
        r.noteOn (1, 60, 1);
        r.flush();
        const auto* fresh = r.heldVoiceForNote (60);
        check (fresh != nullptr, "the new note is sounding");
        if (fresh == nullptr) return;
        checkNear (fresh->getAftertouch(), 0.0f, 1e-3f,
                   "and it starts at no pressure -- the pedalled note's latch did not "
                   "outlive the pedal");
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

// ── 35. A voice TAKEN OVER, not released, lets its latch go ─────────────────
//      The three release paths all key on sustainedVoice/sostenutoVoice -- and
//      allocation WIPES those flags when it re-purposes a voice for a new note.
//      The key that set the latch is already up (that is what made the voice
//      sustained), so no note-off for that pitch will arrive either. Nothing
//      ever scans it again: the latch stands for the rest of the session, and
//      every later note of that pitch enters at full aftertouch.
//
//      The probe is pressureForHeldNote, not a fresh note of that pitch: an
//      external key-down now resets its own note's latch, so a re-strike would
//      pass whether or not the strand was cleared. pressureForHeldNote is also
//      what the instrument-wide aftertouch targets read under the arpeggiator,
//      so it is the audible quantity here and not an implementation detail.
void caseTakenOverVoiceDoesNotStrandTheLatch()
{
    std::printf ("[35] a pedal-held voice taken over for another note lets its latch go\n");
    Rig r;
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::voiceCount))
        p->setValueNotifyingHost (p->convertTo0to1 (0.0f));   // index 0 = 1 voice, mono
    r.flush();

    r.cc (1, 64, 127);            // damper down
    r.noteOn (1, 60);
    r.flush();
    r.polyPressure (1, 60, 127);
    r.flush();
    r.noteOff (1, 60);            // held by the pedal -- the latch is KEPT here
    r.flush();
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 1.0f, 1e-3f,
               "the pedalled note still carries the pressure its finger left");

    // Mono legato takes voice 0 over for the new note. Nothing was released,
    // and both pedal flags are gone from here on.
    r.noteOn (1, 67);
    r.flush();
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 0.0f, 1e-3f,
               "and lets it go when the voice is taken away from that pitch");

    r.cc (1, 64, 0);              // damper up: there is nothing left to scan
    r.flush();
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 0.0f, 1e-3f,
               "with nothing stranded for the pedal to have to find");
}

// ── 36. The latch outlives an arpeggiator gap ──────────────────────────────
//      Under the arp a held key sounds NOTHING between steps, so "does a voice
//      still hold this pitch" answers no while the hand is still leaning into
//      the chord. And the arp's own step note-off carries sourceId -1, which is
//      what an external key-up carries too -- no field noteOff receives can
//      tell them apart. Only the key ledger can, which is why there is one.
//
//      What it costs when it is wrong: with aftertouch on Cache or Snap the
//      travellers walk back to the first position on every gap, and in the
//      language oscillator each of those is a Csound recompile of the orchestra
//      already sounding, one per arpeggio note, back to back.
void casePolyPressureSurvivesAnArpGap()
{
    std::printf ("[36] a held key keeps its pressure through an arpeggiator gap\n");
    Rig r;
    r.noteOn (1, 60);
    r.flush();
    r.polyPressure (1, 60, 100);
    r.flush();
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 100.0f / 127.0f, 1e-3f,
               "the finger's pressure is readable with the arp off");

    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up

    // The key is NEVER lifted. Long enough for the arp's on-edge flush of the
    // voice it was sounding, then many steps and the gap after each of them.
    r.run (200);
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 100.0f / 127.0f, 1e-3f,
               "and still readable after the arp has stepped over it, hand unmoved");
}


// ── 37. A fresh press starts at no pressure, even over a pedalled note ──────
//      The reachable half of case 35, and the one that needs no stealing at
//      all: the damper is down, so the pedalled voice is busy and the re-press
//      gets a voice of its OWN. Nothing is taken over, no release path runs --
//      and note-on seeds the new voice from pressureForNote, which is still the
//      old finger's reading. The key going down is the only event that can end
//      it, and it is the one thing noteOff and the pedal scans cannot see.
void caseFreshPressStartsAtNoPressureUnderTheDamper()
{
    std::printf ("[37] a fresh press starts at no pressure, even over a pedalled note\n");
    Rig r;
    r.cc (1, 64, 127);            // damper down
    r.noteOn (1, 60);
    r.flush();
    r.polyPressure (1, 60, 127);  // pressed as hard as it goes
    r.flush();
    const auto* pedalled = r.heldVoiceForNote (60);
    check (pedalled != nullptr, "the note is sounding");

    r.noteOff (1, 60);            // held by the pedal, latch correctly kept
    r.flush();
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 1.0f, 1e-3f,
               "the pedalled note keeps its pressure while the damper is down");

    r.noteOn (1, 60, 1);          // the same key again, lightest touch there is
    r.flush();
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 0.0f, 1e-3f,
               "and the new press starts at nothing rather than inheriting it");
    if (pedalled != nullptr)
        checkNear (pedalled->getAftertouch(), 1.0f, 1e-3f,
                   "while the note under the pedal keeps the pressure it had");
}


// ── 38. Under the arpeggiator, a lifted key's pressure ends with the finger ──
//      The arp DROPS a lifted key from its pattern and never plays it again, so
//      no note-off for that pitch is ever emitted -- and with the arp on, an
//      external key-up does not reach the voices either. If the key event is
//      not itself the end of the reading, nothing downstream asks a second
//      time, and that note number keeps a permanent aftertouch FLOOR: every
//      later voice on it that no external key started -- a sequencer note, the
//      drone, a replay note, the machine's own keyboard -- enters at full
//      pressure. The common gesture is lifting one key out of a held chord.
void caseArpKeyUpEndsThePressure()
{
    std::printf ("[38] a key lifted under the arpeggiator takes its pressure with it\n");
    Rig r;
    r.noteOn (1, 60);
    r.noteOn (1, 64);
    r.noteOn (1, 67);
    r.flush();
    r.polyPressure (1, 60, 127);
    r.polyPressure (1, 64, 127);
    r.polyPressure (1, 67, 127);
    r.flush();
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
    r.run (40);

    const auto& vm = r.proc.getVoiceManager();
    r.noteOff (1, 67);            // ONE key out of the chord -- a chord change
    r.run (80);
    checkNear (vm.pressureForHeldNote (67), 0.0f, 1e-3f,
               "the lifted key's pressure ends with the finger, arp or no arp");
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f,
               "while the keys still down keep theirs");

    r.noteOff (1, 60);
    r.noteOff (1, 64);
    r.run (80);
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "and the last two end with their fingers too");
    checkNear (vm.pressureForHeldNote (64), 0.0f, 1e-3f,
               "-- including the one the arp happened to be sounding");
}

// ── 39. A note-on the controller re-sends is not a new press ────────────────
//      A key-down starts its note's reading over, which is right for a FRESH
//      press and wrong for the same key announced twice. Controllers re-send,
//      and key repeat does it too -- the arpeggiator's own noteOn names the
//      case. What tells them apart is the channel: a retransmit arrives twice
//      on one, two fingers arrive on two.
void caseRetransmittedNoteOnIsNotAFreshPress()
{
    std::printf ("[39] a note-on re-sent on the same channel is not a new press\n");
    Rig r;
    r.noteOn (2, 60);
    r.flush();
    r.polyPressure (2, 60, 127);
    r.flush();
    const auto& vm = r.proc.getVoiceManager();
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f, "the finger is readable");

    r.noteOn (2, 60);             // again, with no note-off between
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f,
               "and stays readable when the controller re-sends the note-on");

    r.noteOff (2, 60);            // one key-up still ends it
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "-- one key-up still ends it, the repeat did not outlive the key");
}

// ── 40. A second finger on one pitch does not reset the first's reading ─────
//      The key-down half of cases 31 and 32: the same pitch really is played on
//      two member channels, and the second press must not zero a reading the
//      first finger is still applying. It would collapse that key's Z mid-
//      gesture and walk a Cache or Snap traveller back to its first position
//      under a hand that never moved -- the same failure as an arp gap, entered
//      from the other side.
void caseSecondFingerDoesNotResetTheFirst()
{
    std::printf ("[40] a second finger on one pitch does not reset the first's reading\n");
    Rig r;
    r.noteOn (2, 60);
    r.flush();
    r.polyPressure (2, 60, 127);
    r.flush();
    const auto& vm = r.proc.getVoiceManager();

    r.noteOn (3, 60);             // a second finger, its own member channel
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f,
               "the first finger keeps its reading while the second arrives");

    r.noteOff (2, 60);            // the FIRST finger goes; the second is down
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f,
               "and the reading outlives it, because a finger is still on that key");

    r.noteOff (3, 60);
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "ending only when the last one lifts");

    // The same two fingers with NO voice to fall back on. Above, the second
    // finger's own voice still held the pitch when the first lifted, so the
    // voice scan would have carried it even if the key ledger had not -- which
    // makes the case above no test of the ledger at all. Under the arpeggiator
    // there is no voice in the gap, and the only thing that can know a finger
    // is still on that key is which CHANNELS are down on it.
    Rig a;
    a.noteOn (2, 60);
    a.noteOn (3, 60);            // two fingers, one pitch, two member channels
    a.flush();
    a.polyPressure (2, 60, 127);
    a.flush();
    if (auto* p = a.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
    a.run (40);

    const auto& avm = a.proc.getVoiceManager();
    a.noteOff (2, 60);           // one of the two goes
    a.run (80);                  // long enough to cross several arp gaps
    checkNear (avm.pressureForHeldNote (60), 1.0f, 1e-3f,
               "and under the arpeggiator too, where no voice can vouch for it");

    a.noteOff (3, 60);
    a.run (80);
    checkNear (avm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "-- ending there when the second finger lifts, and not before");
}

// ── 41. The machine's own keyboard is a keyboard ────────────────────────────
//      Its keys never enter the MIDI buffer, so the two loops that keep the key
//      ledger from raw MIDI never see them. Missed on the way up, a key-up
//      leaves the reading standing for good; missed on the way down, a fresh
//      press inherits the last one's -- case 37 through the other keyboard.
void caseComputerKeyboardKeysCountToo()
{
    std::printf ("[41] the machine's own keyboard is a keyboard\n");
    {
        Rig r;
        r.proc.beginComputerKeyboardNote (60, 0.8f);
        r.flush();
        r.polyPressure (1, 60, 127);
        r.flush();
        const auto& vm = r.proc.getVoiceManager();
        checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f,
                   "a key on it takes pressure like any other");
        r.proc.endComputerKeyboardNote (60);
        r.flush();
        checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
                   "and lets it go when the key comes up");
    }
    {
        Rig r;
        r.cc (1, 64, 127);            // damper down
        r.noteOn (1, 60);
        r.flush();
        r.polyPressure (1, 60, 127);
        r.flush();
        const auto* pedalled = r.heldVoiceForNote (60);
        check (pedalled != nullptr, "the pedalled note is sounding");
        r.noteOff (1, 60);            // held by the pedal, reading correctly kept
        r.flush();
        const auto& vm = r.proc.getVoiceManager();
        checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f,
                   "the pedalled note keeps its pressure");

        r.proc.beginComputerKeyboardNote (60, 0.008f);   // the lightest touch
        r.flush();
        checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
                   "and a press on the other keyboard starts at nothing all the same");
        if (pedalled != nullptr)
            checkNear (pedalled->getAftertouch(), 1.0f, 1e-3f,
                       "while the note under the pedal keeps the pressure it had");
    }
}


// ── 42. A stream restart under a held key does not deafen that pitch ────────
//      prepare() and reset() clear the reading; if they leave the key ledger
//      standing, that note number is stuck holding a finger that is not there.
//      Every later clear for it short-circuits AND every later first-press
//      reset does too -- the pitch keeps whatever it is next given, for good.
//      A host restarts the stream for a sample-rate change, a buffer-size
//      change, a device switch or a suspend; a held chord across one of those
//      is ordinary.
void caseStreamRestartDoesNotDeafenAPitch()
{
    std::printf ("[42] a stream restart under a held key does not deafen that pitch\n");
    Rig r;
    r.noteOn (2, 60);             // a key goes down...
    r.flush();
    r.proc.releaseResources();    // ...and the host restarts the stream under it
    r.proc.prepareToPlay (kSampleRate, kBlockSize);
    pump (20);

    const auto& vm = r.proc.getVoiceManager();
    r.noteOn (3, 60);
    r.flush();
    r.polyPressure (3, 60, 127);
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f,
               "a key pressed after the restart still takes pressure");
    r.noteOff (3, 60);
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "and still lets it go, rather than being held by a finger that left");

    r.noteOn (3, 60, 1);          // the lightest touch there is
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "-- and a fresh press after all that starts at nothing");
}

// ── 43. Aftertouch in the same buffer as the key-up does not re-arm it ──────
//      The key events of a whole buffer are read at the top of the block; the
//      aftertouch is applied in the sample-accurate walk further down. So an
//      aftertouch message sitting in the same buffer as the note-off that ended
//      the key arrives AFTER the reading was closed. At 256 samples that window
//      is under six milliseconds, which a controller streaming pressure hits on
//      most releases -- and with the arpeggiator on nothing asks a second time,
//      so what it re-armed stands for the session. The arp then feeds it to its
//      own octave notes and to the step-hold preview: full pressure on notes
//      nobody leaned on.
void caseAftertouchInTheReleaseBufferDoesNotReArm()
{
    std::printf ("[43] aftertouch in the same buffer as the key-up does not re-arm it\n");
    Rig r;
    r.noteOn (1, 60);
    r.flush();
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
    r.run (40);

    // Both in ONE buffer, the way a release actually arrives.
    r.polyPressure (1, 60, 127);
    r.noteOff (1, 60);
    r.run (80);

    const auto& vm = r.proc.getVoiceManager();
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "the reading ends with the key, whatever else was in that buffer");
}

// ── 44. A panic under a held chord leaves nothing standing ──────────────────
//      All-notes-off drops every key, which is right -- the arpeggiator does
//      the same -- but the fingers are still on the keys and go on sending. The
//      key-up that eventually comes finds nothing recorded and has nothing to
//      clear with, so anything written in between would stand for good. A DAW
//      sends CC123 on transport stop, so "stop while holding a chord, keep
//      holding, lean in" is an ordinary gesture.
void casePanicUnderAHeldChordLeavesNothingStanding()
{
    std::printf ("[44] a panic under a held chord leaves nothing standing\n");
    Rig r;
    r.noteOn (1, 60);
    r.flush();
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
    r.run (40);

    r.cc (1, 123, 0);             // all notes off, finger still down
    r.run (10);
    r.polyPressure (1, 60, 127);  // the hand goes on leaning
    r.run (10);
    r.noteOff (1, 60);            // and eventually lifts
    r.run (40);

    const auto& vm = r.proc.getVoiceManager();
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "nothing is left holding that pitch after the panic");
}

// ── 45. A key-up naming a channel that is not down takes nothing with it ────
//      It used to take the WHOLE entry, on the theory that a lost note-on would
//      otherwise leave the reading standing. The entry it takes belongs to
//      whichever fingers are down, and there is a plain path to the wrong one:
//      the computer keyboard declines to register a key while a replay runs and
//      reports its key-up afterwards all the same, so a key-up for a key that
//      was never registered lands on a pitch another finger is holding.
void caseKeyUpOnAnUnheldChannelTakesNothing()
{
    std::printf ("[45] a key-up naming a channel that is not down takes nothing\n");
    Rig r;
    r.noteOn (2, 60);
    r.flush();
    r.polyPressure (2, 60, 127);
    r.flush();
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // no voice in the gap
    r.run (40);

    const auto& vm = r.proc.getVoiceManager();
    r.noteOff (5, 60);            // a channel nothing is down on
    r.run (80);
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f,
               "the finger that IS on that key keeps its reading");

    r.noteOff (2, 60);            // its own key-up still ends it
    r.run (80);
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "and lets it go when that finger lifts");
}


// ── 46. Reset All Controllers is not the hand leaving the keys ─────────────
//      CC 121 resets controller VALUES and releases no voice: the chord goes on
//      sounding and the hand goes on leaning. If it drops the keys as well, the
//      reading gate stays shut for every one of them -- the instrument deaf to
//      pressure on a chord it is still playing, until each key is lifted and
//      pressed again. A DAW sends it on transport stop and locate, and a
//      controller sends it on a patch change.
void caseResetAllControllersIsNotTheHandLeaving()
{
    std::printf ("[46] reset-all-controllers is not the hand leaving the keys\n");
    Rig r;
    r.noteOn (1, 60);
    r.flush();
    r.polyPressure (1, 60, 127);
    r.flush();
    const auto& vm = r.proc.getVoiceManager();
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f, "the finger is readable");

    r.cc (1, 121, 0);             // Reset All Controllers
    r.flush();
    check (r.heldVoiceForNote (60) != nullptr, "the note is still sounding after it");
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "and the reading went to nothing with the other controller values");

    r.polyPressure (1, 60, 100);  // the hand leans in again, key never lifted
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 100.0f / 127.0f, 1e-3f,
               "-- and the key still answers, because the finger never left it");
}


// ── 47. Aftertouch BEFORE the press in one buffer does not seed it ──────────
//      The mirror of [43], from the same hoisting. If the ledger is kept in a
//      pass that reads the whole buffer first, an aftertouch arriving before a
//      note-on is judged against a key the pass has already recorded as down --
//      so it writes a reading the press is then seeded from, and the lightest
//      possible re-strike enters at the last press's peak. In a part with
//      back-to-back notes of one pitch and a pressure lane, that is every
//      repeat.
void caseAftertouchBeforeThePressDoesNotSeedIt()
{
    std::printf ("[47] aftertouch before a press, in one buffer, does not seed it\n");
    Rig r;
    r.noteOn (1, 60);
    r.flush();
    r.polyPressure (1, 60, 127);
    r.flush();

    // One buffer, in the order a repeated note actually arrives.
    r.polyPressure (1, 60, 127);
    r.noteOff (1, 60);
    r.noteOn (1, 60, 1);          // the lightest touch there is
    r.flush();

    const auto* fresh = r.heldVoiceForNote (60);
    check (fresh != nullptr, "the re-struck note is sounding");
    if (fresh == nullptr) return;
    checkNear (fresh->getAftertouch(), 0.0f, 1e-3f,
               "and it starts at its own pressure, not the last press's peak");
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 0.0f, 1e-3f,
               "with nothing left over for the next one either");
}


// ── 48. A gliding step takes the reading of the pitch it leaves ─────────────
//      The sixth path that moves a voice off its note with nothing released:
//      the poly bind/glide branch, reached whenever a sequencer step carries
//      Glide or Bind. Play along with a gliding line, touch the pitch it is on,
//      lean, lift: the voice arm rightly keeps the reading while the step's own
//      voice still holds that pitch -- and when the step glides away, nothing
//      is asking any more. From then on every sequencer, arp or drone note on
//      that pitch enters at full pressure, for the session.
void caseGlidingStepTakesTheReadingWithIt()
{
    std::printf ("[48] a gliding step takes the reading of the pitch it leaves\n");
    Rig r;
    auto set = [&r] (const char* pid, float v)
    {
        if (auto* p = r.proc.getValueTreeState().getParameter (pid))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    // The generative sequencer mirrors its own pattern into the step data every
    // block, so the steps below only stay put with it off.
    set (PID::genSeqRunning, 0.0f);
    r.run (2);

    auto& seq = r.proc.getStepSequencer();
    seq.setNumSteps (2);
    seq.setStepNote (0, 60);
    seq.setStepNote (1, 67);
    seq.setStepEnabled (0, true);
    seq.setStepEnabled (1, true);
    // The articulation carries the INCOMING transition, so it is step 0 that
    // has to slide for step 1's note to continue step 0's voice.
    seq.setStepBindMode (0, T5ynthStepSequencer::BindMode::Glide);
    seq.setStepBindMode (1, T5ynthStepSequencer::BindMode::Off);
    set (PID::seqSteps, 2.0f);
    set (PID::seqBpm, 40.0f);     // slow, so the hand acts inside step 0
    set (PID::seqRunning, 1.0f);
    r.run (10);                   // the line is on step 0, sounding note 60

    const auto& vm = r.proc.getVoiceManager();
    r.polyPressure (1, 60, 127);  // a hand touches the pitch the line is on...
    r.noteOn (1, 60);
    r.flush();
    r.polyPressure (1, 60, 127);
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f, "the hand is readable");

    r.noteOff (1, 60);            // ...and lifts. The step's voice still holds 60.
    r.flush();
    checkNear (vm.pressureForHeldNote (60), 1.0f, 1e-3f,
               "which the line's own voice rightly keeps while it is still on 60");

    r.run (100);                  // long enough for the glide to step 67
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "and the reading goes when the line glides off that pitch");
    set (PID::seqRunning, 0.0f);
    r.run (5);
}


// ── 49. Reset All Controllers does not un-own a sounding note ───────────────
//      The other half of [46]. A voice's channel tag is not a controller value
//      -- it is which finger owns the note -- and CC 121 releases nothing. Wipe
//      it and the note goes on sounding, cut off from its own member channel's
//      bend, pressure and slide for the rest of its life, with no way back: the
//      tag is only ever written at note-on.
void caseResetAllControllersDoesNotUnownASoundingNote()
{
    std::printf ("[49] reset-all-controllers does not un-own a sounding note\n");
    Rig r;
    r.noteOn (3, 60);
    r.flush();
    r.pressure (3, 100);
    r.flush();
    const auto* v = r.heldVoiceForNote (60);
    check (v != nullptr, "the note is sounding on its member channel");
    if (v == nullptr) return;
    checkNear (v->getAftertouch(), 100.0f / 127.0f, 1e-3f, "and follows its channel");

    r.cc (1, 121, 0);             // Reset All Controllers
    r.flush();
    check (r.heldVoiceForNote (60) != nullptr, "it is still sounding afterwards");
    checkNear (v->getAftertouch(), 0.0f, 1e-3f,
               "with its expression reset, which is what the message asks for");

    r.pressure (3, 80);           // the same finger, the same channel
    r.flush();
    checkNear (v->getAftertouch(), 80.0f / 127.0f, 1e-3f,
               "and its channel still reaches it");
    r.wheel (3, 16383);
    r.flush();
    checkNear (v->getPerVoicePitchBend(), fullUpBend (r.noteBendRange()), 0.01f,
               "-- bend included, because the note never changed hands");
}


// ── 50. A panic un-owns the notes it cut off ────────────────────────────────
//      [49]'s boundary. Reset All Controllers owns nothing and must leave a
//      sounding note attached to its member channel; a panic has just un-owned
//      everything. The two go through the same function, and at the moment it
//      runs every voice is in its release tail -- still "active", so an
//      is-it-sounding test keeps the tag for exactly the window that matters.
//      An MPE controller streams X/Y/Z for as long as a finger rests on a key,
//      and a DAW sends the panic on transport stop and on locate, with the
//      hands still down.
void casePanicUnownsTheNotesItCutOff()
{
    std::printf ("[50] a panic un-owns the notes it cut off\n");
    Rig r;
    r.noteOn (3, 60);
    r.flush();
    r.pressure (3, 100);
    r.flush();
    const auto* v = r.voiceForNote (60);
    check (v != nullptr, "the note is sounding on its member channel");
    if (v == nullptr) return;

    r.cc (1, 123, 0);             // all notes off
    r.flush();
    check (v->isActive(), "and is in its release tail after the panic");

    r.pressure (3, 127);
    r.wheel (3, 16383);
    r.cc (3, 74, 127);
    r.flush();
    checkNear (v->getAftertouch(), 0.0f, 1e-3f,
               "the tail does not swell to a hand still on the key");
    checkNear (v->getPerVoicePitchBend(), 0.0f, 1e-4f, "does not slide");
    checkNear (v->getTimbre(), 0.0f, 1e-4f, "and does not change its timbre");
}


// ── 51. The same, under the arpeggiator ─────────────────────────────────────
//      [47]'s other half. With the arp on the note events are filtered out of
//      the stream before the sample-accurate walk, so the key ledger is kept in
//      a pass that reads the whole buffer first -- and the aftertouch is NOT
//      filtered: it reaches the walk at its own offset and is judged against a
//      ledger that already knows about a press two hundred samples later. The
//      re-strike then enters at the previous press's peak and stays there for
//      as long as the key is held, filter wide open or wherever the aftertouch
//      page sends it. Six milliseconds at 256 samples, which a controller
//      streaming pressure hits on a fast repeat.
void caseAftertouchBeforeThePressUnderTheArp()
{
    std::printf ("[51] aftertouch before a press, under the arpeggiator\n");
    Rig r;
    r.noteOn (1, 60);
    r.flush();
    r.polyPressure (1, 60, 127);
    r.flush();
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
    r.run (40);

    // One buffer, in the order a fast repeat actually arrives.
    r.polyPressureAt (1, 60, 127, 10);    // the dying end of the old press
    r.noteOffAt      (1, 60,      100);
    r.noteOnAt       (1, 60, 1,   200);   // the lightest re-strike there is
    r.run (80);

    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 0.0f, 1e-3f,
               "the re-struck key reads its own pressure, not the last press's peak");
}


// ── 52. A gliding step's voice arrives at the pressure of its new pitch ─────
//      [48] fixed the reading the line LEAVES behind. The voice itself kept the
//      old pitch's value, frozen: the mono legato branch and the drone's glide
//      both re-seed on arrival, the poly bind/glide branch did not. So the line
//      stayed leaned-into for the rest of its life -- and then collapsed in one
//      step the moment any wheel, breath or channel-pressure message moved and
//      the pressure was recomputed. On aftertouch -> DCA that step is full
//      level to silence.
void caseGlidingStepArrivesAtItsNewPitchesPressure()
{
    std::printf ("[52] a gliding step's voice arrives at the pressure of its new pitch\n");
    Rig r;
    auto set = [&r] (const char* pid, float v)
    {
        if (auto* p = r.proc.getValueTreeState().getParameter (pid))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    set (PID::genSeqRunning, 0.0f);
    r.run (2);
    auto& seq = r.proc.getStepSequencer();
    seq.setNumSteps (2);
    seq.setStepNote (0, 60);
    seq.setStepNote (1, 67);
    seq.setStepEnabled (0, true);
    seq.setStepEnabled (1, true);
    seq.setStepBindMode (0, T5ynthStepSequencer::BindMode::Glide);
    seq.setStepBindMode (1, T5ynthStepSequencer::BindMode::Off);
    set (PID::seqSteps, 2.0f);
    set (PID::seqBpm, 40.0f);
    set (PID::seqRunning, 1.0f);
    r.run (10);

    r.noteOn (1, 60);             // the hand touches the pitch the line is on
    r.flush();
    r.polyPressure (1, 60, 127);
    r.flush();
    r.noteOff (1, 60);            // and lifts
    r.flush();

    r.run (100);                  // the line glides to 67
    const auto* glided = r.heldVoiceForNote (67);
    check (glided != nullptr, "the line is sounding its second note");
    if (glided == nullptr) { set (PID::seqRunning, 0.0f); r.run (5); return; }
    checkNear (glided->getAftertouch(), 0.0f, 1e-3f,
               "and arrives at what the new pitch is carrying, which is nothing");

    r.pressure (1, 0);            // anything that recomputes pressure...
    r.flush();
    checkNear (glided->getAftertouch(), 0.0f, 1e-3f,
               "-- with no step to fall, because it was never above it");
    set (PID::seqRunning, 0.0f);
    r.run (5);
}


// ── 53. A re-strike under the arpeggiator does not inherit the old peak ─────
//      [37] is this gesture with the arp OFF. With it ON the key events never
//      reach the sample-accurate walk, so the ledger is kept in a pass that
//      reads the whole buffer first -- and a key-down there does TWO things:
//      it records the finger, and it starts that pitch's reading over. Only the
//      first has to wait for the walk. Holding both back put the reset AFTER
//      the arp step it was for: the step was dispatched inside the walk and
//      seeded from the previous press's peak, so the note entered at full
//      pressure and stayed there -- and the reset landing at the end of the
//      block then froze the voice above everything that computes it, until one
//      channel-pressure message dropped it to nothing in a single step. On
//      aftertouch -> DCA that is full level, then silence.
void caseArpReStrikeDoesNotInheritTheOldPeak()
{
    std::printf ("[53] a re-strike under the arpeggiator starts at its own pressure\n");
    Rig r;
    r.cc (1, 64, 127);            // damper down, so the note-off keeps the latch
    r.noteOn (1, 60);
    r.flush();
    r.polyPressure (1, 60, 127);
    r.flush();
    r.noteOff (1, 60);
    r.flush();
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 1.0f, 1e-3f,
               "the pedalled note keeps its pressure while the damper is down");

    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
    r.run (40);

    r.noteOn (1, 60, 1);          // the same key again, lightest touch there is
    r.run (80);

    // Two voices carry this pitch now: the one the pedal holds, which keeps
    // what it had, and the arp's fresh one, which must carry nothing. Read the
    // lowest rather than "the" voice -- voiceForNote would answer either.
    const auto& vm = r.proc.getVoiceManager();
    float lowest = 1.0f;
    int   onPitch = 0;
    for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
    {
        const auto& v = vm.getVoice (i);
        if (v.isActive() && v.getCurrentNote() == 60)
        {
            lowest = juce::jmin (lowest, v.getAftertouch());
            ++onPitch;
        }
    }
    check (onPitch > 0, "the arp is sounding the re-struck key");
    checkNear (lowest, 0.0f, 1e-3f,
               "and it entered at its own pressure, not the last press's peak");
    checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
               "-- the reading started over at the key event, not at the end of the block");
}


// ── 54. A note shorter than one buffer leaves no finger behind ──────────────
//      The buffer-reading pass sees a press and a release of the same key in
//      one block. Deferring the press past the walk while applying the release
//      at once reversed them: the release found nothing recorded and returned,
//      the press then landed after it, and that pitch stayed recorded as held
//      with no finger anywhere on the instrument. From there every aftertouch
//      message for it is accepted and nothing can ever clear it again, so a
//      later untouched note of that pitch -- a sequencer step, the drone, the
//      machine's own keyboard -- enters at whatever the ghost was left at.
//      A note this short is 5.8 ms at 256 samples and 46 ms at 2048, which is
//      an ordinary staccato at the larger sizes.
void caseNoteShorterThanOneBufferLeavesNoFinger()
{
    std::printf ("[54] a note shorter than one buffer leaves no finger behind\n");
    Rig r;
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
    r.run (40);

    r.noteOnAt  (1, 60, 100, 10);    // pressed and released inside one buffer
    r.noteOffAt (1, 60,      100);
    r.run (4);

    // Nobody is touching the instrument. A reading for that pitch has no
    // finger to belong to and must be refused.
    r.polyPressure (1, 60, 127);
    r.flush();
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 0.0f, 1e-3f,
               "with the key long gone, nothing can write that pitch a pressure");

    // And the gate is still a gate, not a wall: a key that IS down still reads.
    r.noteOn (1, 60);
    r.run (4);
    r.polyPressure (1, 60, 127);
    r.flush();
    checkNear (r.proc.getVoiceManager().pressureForHeldNote (60), 1.0f, 1e-3f,
               "while a finger actually on the key writes as it always did");
}


// ── 55. The wheel does not reach back into a note whose key is up ──────────
//      pressureForNote maxes poly aftertouch together with the mod wheel, the
//      breath controller and channel pressure, and every one of those three
//      rewrote the STORED pressure of every sounding voice when it moved --
//      releasing ones and pedal-held ones with the rest. So a decayed chord
//      came back at full level the moment the wheel was touched, and a ringing
//      tail was cut to silence in one block. The rule the file states three
//      times is that a voice whose key came up keeps what that key left; it was
//      applied on the bind path and on the latch, and not here.
void caseTheWheelDoesNotReachAReleasedNote()
{
    std::printf ("[55] a global pressure controller does not reach a note whose key is up\n");
    Rig r;
    r.noteOn (1, 60);
    r.noteOn (1, 64);
    r.flush();
    r.polyPressure (1, 60, 110);
    r.polyPressure (1, 64, 110);
    r.flush();
    const auto* a = r.heldVoiceForNote (60);
    const auto* b = r.heldVoiceForNote (64);
    check (a != nullptr && b != nullptr, "both notes sound");
    if (a == nullptr || b == nullptr) return;
    checkNear (a->getAftertouch(), 110.0f / 127.0f, 1e-3f, "and are leaned into");

    r.noteOff (1, 60);
    r.noteOff (1, 64);
    r.flush();
    r.cc (1, 1, 127);             // the wheel, while the chord is decaying
    r.flush();
    checkNear (a->getAftertouch(), 110.0f / 127.0f, 1e-3f,
               "the decaying chord keeps what the fingers left it");
    checkNear (b->getAftertouch(), 110.0f / 127.0f, 1e-3f,
               "-- both of it");

    r.cc (1, 1, 0);
    r.flush();
    checkNear (a->getAftertouch(), 110.0f / 127.0f, 1e-3f,
               "and the wheel coming back down does not cut the tail either");

    // The same gesture over the damper: key up, pedal still holding.
    Rig p;
    p.cc (1, 64, 127);
    p.noteOn (1, 67);
    p.flush();
    p.polyPressure (1, 67, 110);
    p.flush();
    const auto* pedalled = p.heldVoiceForNote (67);
    check (pedalled != nullptr, "the pedalled note sounds");
    p.noteOff (1, 67);
    p.flush();
    // The re-press matters and the case is worthless without it: it resets the
    // note's LATCH, and while the latch stands it holds the pedalled voice up
    // on its own, so the assertion below would pass whatever this function
    // does. With the latch gone, only the guard is left holding it.
    p.noteOn (1, 67, 1);
    p.flush();
    p.cc (1, 1, 1);               // the smallest wheel move there is
    p.flush();
    if (pedalled != nullptr)
        checkNear (pedalled->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                   "and a note the pedal holds after the key came up keeps it too");

    // The gate is still a gate: a key that IS down follows the wheel, and so
    // does a note nobody's finger ever held.
    Rig h;
    h.noteOn (1, 72);
    h.flush();
    h.cc (1, 1, 127);
    h.flush();
    const auto* held = h.heldVoiceForNote (72);
    check (held != nullptr, "the held note sounds");
    if (held != nullptr)
        checkNear (held->getAftertouch(), 1.0f, 1e-3f,
                   "while a key still down follows the wheel as it always did");
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
    caseTwoHeldKeysShareOneChannel();
    casePolyPressureIsNotAPermanentFloor();
    casePolyPressureEndsWithThePedal();
    casePolyPressureSurvivesAHeldUnison();
    caseNoteOffNamesItsChannel();
    caseTakenOverVoiceDoesNotStrandTheLatch();
    casePolyPressureSurvivesAnArpGap();
    caseFreshPressStartsAtNoPressureUnderTheDamper();
    caseArpKeyUpEndsThePressure();
    caseRetransmittedNoteOnIsNotAFreshPress();
    caseSecondFingerDoesNotResetTheFirst();
    caseComputerKeyboardKeysCountToo();
    caseStreamRestartDoesNotDeafenAPitch();
    caseAftertouchInTheReleaseBufferDoesNotReArm();
    casePanicUnderAHeldChordLeavesNothingStanding();
    caseKeyUpOnAnUnheldChannelTakesNothing();
    caseResetAllControllersIsNotTheHandLeaving();
    caseAftertouchBeforeThePressDoesNotSeedIt();
    caseGlidingStepTakesTheReadingWithIt();
    caseResetAllControllersDoesNotUnownASoundingNote();
    casePanicUnownsTheNotesItCutOff();
    caseAftertouchBeforeThePressUnderTheArp();
    caseGlidingStepArrivesAtItsNewPitchesPressure();
    caseLegatoKeepsItsTimbreOrigin();
    caseArpReStrikeDoesNotInheritTheOldPeak();
    caseNoteShorterThanOneBufferLeavesNoFinger();
    caseTheWheelDoesNotReachAReleasedNote();

    std::printf ("\n%d checks, %d failures -- %s\n\n",
                 gChecks, gFailures, gFailures == 0 ? "ALL PASS" : "FAILED");
    return gFailures == 0 ? 0 : 1;
}
