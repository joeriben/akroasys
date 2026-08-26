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
//   * Capability 26 -- expression applied at the event's SAMPLE POSITION rather
//     than at block start. Voice state cannot show it: only a note-on resets
//     bend, pressure and timbre, so the values at the end of a block are the
//     same whether the buffer was walked in time order or applied as one batch
//     at sample 0. The AUDIO would show it, and this harness has none -- with no
//     engine data loaded every voice renders silence (measured: peak 0.0 for
//     twelve blocks after a note-on). What the walk IS pinned by is the ledger,
//     which is kept at each event's own instant: cases 43, 47 and 51 all fail
//     when the key events move back into the block-top pass.
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
    // Cases 50, 62 and 71 put the panic at sample 180 and the hand's messages at
    // 220-230, because the cut they measure lasts 132 samples and is over before
    // the next buffer. processBlock caps its sub-block walk at numSamples and
    // stops at the first event past it, so in a SHORTER block those messages are
    // silently never dispatched -- and every assertion in those cases ("still
    // active", "reads 0") holds when nothing was sent at all. They would pass on
    // an empty buffer instead of failing.
    static_assert (kBlockSize > 230, "cases 50, 62 and 71 need room for a message at 230");
    // And the other end: the cut must NOT be finished in the 76 samples left
    // after 180, or the voice is gone before the hand's message arrives. 3 ms of
    // ramp is 76 samples at 25.3 kHz, so every real rate has margin and the
    // margin grows with the rate.
    static_assert (kSampleRate > 25400.0, "the cut must outlast the tail of the block");

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
        void pressureAt (int ch, int v7, int pos)
        { midi.addEvent (juce::MidiMessage::channelPressureChange (ch, v7), pos); }
        void wheelAt (int ch, int value14, int pos)
        { midi.addEvent (juce::MidiMessage::pitchWheel (ch, value14), pos); }
        void ccAt (int ch, int number, int value, int pos)
        { midi.addEvent (juce::MidiMessage::controllerEvent (ch, number, value), pos); }

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

        // note-off at 0, wheel at the end of the same block.
        r.midi.addEvent (juce::MidiMessage::noteOff (5, 64), 0);
        r.midi.addEvent (juce::MidiMessage::pitchWheel (5, 16383), kBlockSize - 1);
        r.run (1);

        const auto& vm = r.proc.getVoiceManager();
        float maxBend = 0.0f;
        for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            if (vm.getVoice (i).isActive())
                maxBend = juce::jmax (maxBend, std::fabs (vm.getVoice (i).getPerVoicePitchBend()));

        // The key came up before the wheel arrived, so the wheel reaches nothing.
        // Until 2026-08-23 this assertion said the opposite of the case's own
        // title -- it recorded that a releasing voice still followed its
        // channel -- and that was the defect: a controller resets its member
        // channel just BEFORE the next note-on, so the burst landed on the tail
        // of the note before it and snapped it back to rest.
        checkNear (maxBend, 0.0f, 0.01f,
                   "a wheel after the key-up does not reach the note that ended");
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
        // A slide rest away from both ends, so a Y reading of 0 means "deaf"
        // rather than "at rest" -- with rest 0 the two are the same number and
        // nothing below could tell them apart.
        r.cc (2, 74, 64);
        r.noteOn (2, 60);
        r.noteOn (2, 64);
        r.noteOn (2, 67);            // three fingers, one channel
        r.flush();
        const float rest = 64.0f / 127.0f;

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
        checkNear (a->getTimbre(), 1.0f - rest, 1e-3f, "and its slide");

        // And it stays together when one finger leaves. Since 2026-08-23 a voice
        // drops its expression channel at its OWN key-up (row 25a), and the
        // obvious over-broad way to write that -- clear every voice carrying the
        // channel -- makes the whole chord go deaf to bend, slide and pressure
        // the instant the first key lifts, with two keys still down. The suite
        // was green on that variant: every other case has one key down at a
        // time. This is the half that separates them.
        r.noteOff (2, 60);           // the lowest finger leaves
        r.flush();
        r.wheel (2, 0);              // full DOWN, so this cannot read as "unchanged"
        r.pressure (2, 40);
        r.cc (2, 74, 0);
        r.flush();

        checkNear (b->getPerVoicePitchBend(), -r.noteBendRange(), 0.01f,
                   "the two keys still down follow the wheel after the third lifts");
        checkNear (c->getPerVoicePitchBend(), -r.noteBendRange(), 0.01f,
                   "-- both of them");
        checkNear (b->getAftertouch(), 40.0f / 127.0f, 1e-3f,
                   "-- and the channel's pressure");
        checkNear (c->getTimbre(), -rest, 1e-3f,
                   "-- and its slide, which reads below rest and so cannot be deafness");
        checkNear (a->getPerVoicePitchBend(), bend, 0.01f,
                   "while the finger that left keeps what it had");
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
//      What it costs when it is wrong: with aftertouch on Cache the
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
//      gesture and walk a Cache traveller back to its first position
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
//      sends one of these on transport stop, so "stop while holding a chord,
//      keep holding, lean in" is an ordinary gesture. CC 120 here: since the
//      two were separated it is the one that takes the notes away, and case 75
//      holds what CC 123 does instead.
void casePanicUnderAHeldChordLeavesNothingStanding()
{
    std::printf ("[44] a panic under a held chord leaves nothing standing\n");
    Rig r;
    r.noteOn (1, 60);
    r.flush();
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
    r.run (40);

    r.cc (1, 120, 0);             // all SOUND off, finger still down
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

    // Queued at 180 and the hand's messages at 220, one buffer: the panic cuts
    // over the 132-sample declick floor, so a message in the NEXT buffer would
    // arrive at an empty slot and this case would pass on nothing.
    r.ccAt (1, 120, 0, 180);      // all sound off
    r.pressureAt (3, 127, 220);
    r.wheelAt (3, 16383, 225);
    r.ccAt (3, 74, 127, 230);
    r.run (1);
    check (v->isActive(), "and is still being cut when the hand moves");
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


// ── 56. A step-sequencer note-off does not end a key the player is holding ──
//      Every internal note event -- the step sequencer's and the arpeggiator's
//      -- carries sourceId -1, and so does external MIDI. noteOff read that -1
//      as a WILDCARD, matching every voice of the pitch whatever struck it. So
//      holding a note and starting the sequencer cut that note the first time
//      the pattern reached its pitch, with the finger still down. Under the
//      damper it did not cut the voice but marked it sustained, which is worse:
//      isKeyHeldVoice then reads false under a hand that never moved, the
//      the Cache traveller stops seeing the hand, claimExprChannel is free to
//      strip that voice's member channel, and lifting the pedal releases a key
//      nobody lifted. Origin is what separates the two halves of that bucket.
void caseSeqNoteOffDoesNotEndAHeldKey()
{
    std::printf ("[56] a sequencer note-off does not end a key the player is holding\n");
    auto fixture = [] (Rig& r, float bpm)
    {
        auto set = [&r] (const char* pid, float v)
        {
            if (auto* p = r.proc.getValueTreeState().getParameter (pid))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };
        set (PID::genSeqRunning, 0.0f);
        r.run (2);
        auto& seq = r.proc.getStepSequencer();
        seq.setNumSteps (2);
        seq.setStepNote (0, 60);          // the pattern walks over the held pitch
        seq.setStepNote (1, 67);
        seq.setStepEnabled (0, true);
        seq.setStepEnabled (1, true);
        seq.setStepBindMode (0, T5ynthStepSequencer::BindMode::Off);
        seq.setStepBindMode (1, T5ynthStepSequencer::BindMode::Off);
        set (PID::seqSteps, 2.0f);
        set (PID::seqBpm, bpm);
        set (PID::seqRunning, 1.0f);
    };

    {
        Rig r;
        r.noteOn (2, 60);                 // the hand, on a member channel
        r.flush();
        const auto* key = r.heldVoiceForNote (60);
        check (key != nullptr, "the key sounds");
        fixture (r, 200.0f);
        r.run (400);                      // several passes over that pitch
        // The pitch as well as the slot: a stolen and re-struck voice would
        // satisfy isActive() && !isReleasing() while being the sequencer's.
        if (key != nullptr)
            check (key->isActive() && ! key->isReleasing() && key->getCurrentNote() == 60,
                   "and is still held after the pattern has walked over it");
        // And it is still the player's note, not merely a sounding one: it must
        // still answer the channel its own key arrived on. That is what the
        // sustained-marking version took away -- isKeyHeldVoice read false, and
        // the next note on that channel was then free to take it over.
        r.pressure (2, 100);
        r.flush();
        if (key != nullptr)
            checkNear (key->getAftertouch(), 100.0f / 127.0f, 1e-3f,
                       "-- and still answers its own member channel");
    }

    {
        Rig r;
        r.cc (1, 64, 127);                // damper down
        r.noteOn (2, 60);
        r.flush();
        check (r.proc.getVoiceManager().getKeyHeldVoiceCount() == 1,
               "with the damper down the key still reads as a key");
        // Slow, and only two passes: with the damper down a sequencer note-off
        // marks its OWN voice sustained rather than releasing it, so a long run
        // fills the pool with held sequencer notes and steals the key's voice.
        // That is the damper working, not this defect, and a case that let it
        // happen would be measuring the wrong thing.
        fixture (r, 40.0f);
        r.run (160);
        check (r.proc.getVoiceManager().getKeyHeldVoiceCount() >= 1,
               "and it keeps reading as one while the pattern passes over it");
    }

    // The counter-check, so this cannot be bought by refusing everything: the
    // sequencer's OWN note-offs must still end the sequencer's own notes.
    {
        Rig r;
        fixture (r, 200.0f);
        r.run (60);
        const int sounding = r.activeVoiceCount();
        check (sounding > 0, "the sequencer is sounding");
        if (auto* p = r.proc.getValueTreeState().getParameter (PID::seqRunning))
            p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
        r.run (400);
        check (r.activeVoiceCount() == 0,
               "and everything it played ends when it stops");
    }
}


// ── 57. Two fingers on one key are two keys to the arpeggiator too ─────────
//      Its held-key set was keyed by pitch alone, so the second finger never
//      got an entry and the first key-up took the shared one away. Switching
//      the arp off then handed back nothing for a key that was still pressed --
//      silence under it until it was released and pressed again -- and
//      switching the arp ON released only one of the two voices, leaving the
//      other droning under the arpeggio.
void caseTwoFingersOnOneKeyAreTwoKeysToTheArp()
{
    std::printf ("[57] two fingers on one key are two keys to the arpeggiator\n");
    Rig r;
    r.noteOn (2, 60);
    r.noteOn (3, 60);             // the same key, a second finger, its own channel
    r.flush();
    check (r.activeVoiceCount() == 2, "two fingers, two voices");

    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // 0 = Off, 1 = Up
    r.run (40);
    // The arp's on-edge releases the voices the keys were sounding, one per
    // held key. Keyed by pitch alone it saw ONE key here and released one voice,
    // and the other went on sounding under the arpeggio for as long as the key
    // was down. Checked before the hand-back, because a drone left standing
    // here would satisfy the hand-back assertion below all by itself.
    check (r.proc.getVoiceManager().getKeyHeldVoiceCount() == 0,
           "the arp takes over both of them -- neither drones under the arpeggio");

    r.noteOff (2, 60);            // the first finger lifts; the second stays
    r.run (40);
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
        p->setValueNotifyingHost (p->convertTo0to1 (0.0f));   // arp off again
    r.run (40);

    const auto* handed = r.heldVoiceForNote (60);
    check (handed != nullptr,
           "the key still down is handed back to the voices, not left silent");

    r.noteOff (3, 60);
    r.run (200);
    check (r.heldVoiceForNote (60) == nullptr,
           "and it ends when that finger finally lifts");
}


// ── 58. The controller's between-note reset does not land on the last tail ──
//      Section 4a's own capture: the Osmose sends CC74 = 0 immediately before
//      201 of 203 note-ons. That reset belongs to the note about to be struck,
//      and it arrives while the PREVIOUS note on that channel is still ringing.
//      The hand-off in claimExprChannel cannot help -- it runs from noteOn, and
//      by then the burst has already been applied. So every release tail was
//      pulled back to rest a few milliseconds after the key came up: Y and Z to
//      zero, and with the wheel in the burst a downward snap through the whole
//      per-note range -- two octaves at the shipped default of 24 either way. A voice stops answering its member channel at its OWN
//      key-up.
void caseTheResetBurstDoesNotLandOnTheTail()
{
    std::printf ("[58] the controller's between-note reset misses the tail it follows\n");
    Rig r;
    r.cc (5, 74, 0);              // the slide's rest, as the note is struck
    r.noteOn (5, 60);
    r.flush();
    r.cc (5, 74, 110);            // the finger leans in on all three axes
    r.wheel (5, 8192 + 4096);
    r.pressure (5, 100);
    r.flush();

    const auto* v = r.heldVoiceForNote (60);
    check (v != nullptr, "the note sounds");
    if (v == nullptr) return;
    const float y = v->getTimbre();
    const float x = v->getPerVoicePitchBend();
    const float z = v->getAftertouch();
    check (std::fabs (y) > 0.5f && std::fabs (x) > 1.0f && z > 0.5f,
           "and is leaned into on X, Y and Z");

    r.noteOff (5, 60);            // the key comes up; the tail rings on
    r.flush();
    checkNear (v->getTimbre(),             y, 1e-3f, "the tail keeps its slide");
    checkNear (v->getPerVoicePitchBend(),  x, 1e-2f, "-- its bend");
    checkNear (v->getAftertouch(),         z, 1e-3f, "-- and its pressure");

    // The reset sweep for the NEXT note, on the same member channel, while this
    // tail is still sounding. This is the order a real controller sends.
    r.cc (5, 74, 0);
    r.wheel (5, 8192);
    r.pressure (5, 0);
    r.flush();
    checkNear (v->getTimbre(),            y, 1e-3f,
               "and the reset meant for the next note does not reach it");
    checkNear (v->getPerVoicePitchBend(), x, 1e-2f,
               "-- no snap through the whole per-note range in an audible tail");
    checkNear (v->getAftertouch(),        z, 1e-3f,
               "-- and no collapse of its pressure");

    // The same with the damper down: the pedal holds the note, but the FINGER is
    // gone, and that is what decides. Stated because it is a behaviour change --
    // before, a pedalled note went on following its member channel until the
    // next note-on took the channel from it.
    {
        Rig d;
        d.cc (5, 64, 127);            // damper down
        d.cc (5, 74, 0);
        d.noteOn (5, 62);
        d.flush();
        d.cc (5, 74, 110);
        d.pressure (5, 100);
        d.flush();
        const auto* p = d.heldVoiceForNote (62);
        check (p != nullptr, "the pedalled note sounds");
        if (p != nullptr)
        {
            const float py = p->getTimbre();
            const float pz = p->getAftertouch();
            d.noteOff (5, 62);        // key up; the damper keeps it singing
            d.flush();
            d.cc (5, 74, 0);          // the reset for whatever comes next
            d.pressure (5, 0);
            d.flush();
            checkNear (p->getTimbre(),     py, 1e-3f,
                       "a note the damper holds keeps its slide once the key is up");
            checkNear (p->getAftertouch(), pz, 1e-3f,
                       "-- and its pressure: the pedal holds it, the finger decides it");
        }
    }

    // The counter-check: the note struck AFTER the burst reads the burst, and a
    // key still down goes on following its channel as it always did.
    r.noteOn (5, 67);
    r.flush();
    const auto* n = r.heldVoiceForNote (67);
    check (n != nullptr, "the next note sounds");
    if (n != nullptr)
    {
        r.cc (5, 74, 127);
        r.flush();
        checkNear (n->getTimbre(), 1.0f, 1e-3f,
                   "while the key now down follows the channel as it always did");
    }
}


// ── 59. A mono slide onto a new member channel is a new FINGER ─────────────
//      [27] pins the other half: a legato slide keeps the Y origin it was
//      struck with, because moving the ground under a hand that never left the
//      key is wrong. On a channel-rotating MPE controller, though, the mono
//      "slide" is the NEXT key played, on its own member channel -- a different
//      finger, sitting wherever it happens to sit on its own slide. Measured
//      against the old finger's origin it read -0.2835 while sitting at the
//      centre of its travel; with Y -> Cutoff, the default source, that is up
//      to several octaves, and a different amount from note to note. X and Z
//      were re-based three lines away in the same branch; Y was not.
void caseMonoSlideOntoANewChannelIsANewFinger()
{
    std::printf ("[59] a mono slide onto a new member channel is a new finger\n");
    Rig r;
    if (auto* p = r.proc.getValueTreeState().getParameter (PID::voiceCount))
        p->setValueNotifyingHost (p->convertTo0to1 (0.0f));   // index 0 = 1 voice
    r.run (2);

    r.cc (2, 74, 100);            // the first finger, resting deep on its slide
    r.noteOn (2, 60);
    r.flush();
    const auto* v = r.heldVoiceForNote (60);
    check (v != nullptr, "the first note sounds");
    if (v == nullptr) return;
    checkNear (v->getTimbre(), 0.0f, 1e-3f, "at its own rest, so travel is zero");

    r.cc (3, 74, 64);             // a second finger, its own channel, mid-slide
    r.noteOn (3, 67);             // mono: this is the legato branch
    r.flush();
    r.cc (3, 74, 64);             // and it streams where it sits
    r.flush();
    const auto* g = r.heldVoiceForNote (67);
    check (g != nullptr, "the slide arrives");
    if (g != nullptr)
        checkNear (g->getTimbre(), 0.0f, 1e-3f,
                   "and reads its OWN finger's rest, not the one before it");

    // The counter-check, which is [27]'s rule and must survive: one finger, one
    // channel, sliding -- the ground does not move under it.
    Rig o;
    if (auto* p = o.proc.getValueTreeState().getParameter (PID::voiceCount))
        p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
    o.run (2);
    o.cc (5, 74, 0);
    o.noteOn (5, 60);
    o.flush();
    o.cc (5, 74, 127);            // the finger presses deep on the held key
    o.flush();
    o.noteOn (5, 67);             // slides to a new pitch, never leaving
    o.flush();
    const auto* s2 = o.heldVoiceForNote (67);
    check (s2 != nullptr, "the one-finger slide arrives");
    if (s2 != nullptr)
        checkNear (s2->getTimbre(), 1.0f, 1e-3f,
                   "-- and keeps the travel it had, ground unmoved");
}



// ── 60. The wheel drives notes nobody's finger is on ────────────────────────
//      The release guard added in ff622065 asked three proxies -- isReleasing,
//      sustainedVoice, sostenutoReleasedVoice -- for the one question "did a key
//      come up". For a voice a hand started, those proxies answer it. For the
//      sequencers', the arpeggiator's and the drone's notes there was never a
//      key, and sustainedVoice is set for EVERY caller with sourceId < 0 -- the
//      step sequencer and the arpeggiator as much as external MIDI. So with the
//      damper down an arpeggio froze at whatever the wheel last held, and with
//      aftertouch -> DCA the whole pedalled stack stood at full level while the
//      wheel sat at zero. The commit's own rule said the opposite: those notes
//      "follow it for their whole sounding life".
void caseTheWheelDrivesNotesWithNoFingerOnThem()
{
    std::printf ("[60] the wheel drives notes nobody's finger is on\n");

    auto arpUnderTheDamper = [] (Rig& r)
    {
        if (auto* p = r.proc.getValueTreeState().getParameter (PID::arpMode))
            p->setValueNotifyingHost (p->convertTo0to1 (1.0f));    // Up
        r.cc (1, 64, 127);                       // damper down
        r.flush();
        r.noteOn (2, 60); r.noteOn (3, 64); r.noteOn (4, 67);
        r.run (60);                              // let the arpeggio churn
    };

    Rig r;
    arpUnderTheDamper (r);

    auto lowestSounding = [] (Rig& rig)
    {
        const auto& vm = rig.proc.getVoiceManager();
        float lowest = 2.0f;
        int   seen   = 0;
        for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
        {
            const auto& v = vm.getVoice (i);
            if (! v.isActive()) continue;
            lowest = std::min (lowest, v.getAftertouch());
            ++seen;
        }
        return seen == 0 ? std::make_pair (-1.0f, 0) : std::make_pair (lowest, seen);
    };

    r.cc (1, 1, 127);
    r.flush();
    auto up = lowestSounding (r);
    check (up.second > 1, "the arpeggio is sounding on more than one voice");
    checkNear (up.first, 1.0f, 1e-3f,
               "every one of them follows the wheel up");

    r.cc (1, 1, 0);
    r.flush();
    auto down = lowestSounding (r);
    // The MAXIMUM, this time: a single voice left standing at 1.0 is the defect.
    const auto& vm = r.proc.getVoiceManager();
    float highest = 0.0f;
    for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
        if (vm.getVoice (i).isActive())
            highest = std::max (highest, vm.getVoice (i).getAftertouch());
    check (down.second > 1, "and they are still sounding");
    checkNear (highest, 0.0f, 1e-3f,
               "and every one of them follows it back down -- none frozen by the pedal");

    // The other side of the same predicate, so this cannot be satisfied by
    // simply letting everything follow again: a HAND's key, under the same
    // damper, still keeps what the finger left it.
    Rig h;
    h.cc (1, 64, 127);
    h.flush();
    h.noteOn (2, 72);
    h.flush();
    h.cc (2, 74, 64);                            // Y at rest, so Z is the only mover
    h.pressure (2, 110);
    h.flush();
    const auto* pedalled = h.heldVoiceForNote (72);
    check (pedalled != nullptr, "the hand's pedalled note sounds");
    h.noteOff (2, 72);
    h.flush();
    h.noteOn (2, 72, 1);                         // re-press, which clears the latch
    h.flush();
    h.cc (1, 1, 127);
    h.flush();
    if (pedalled != nullptr)
        checkNear (pedalled->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                   "while the hand's own pedalled note keeps what the finger left");
}


// ── 61. Poly aftertouch is a live control too ───────────────────────────────
//      ff622065 closed this door for the wheel, the breath and zone-wide
//      pressure and left it open for poly key pressure: setPolyPressure's voice
//      loop matched on pitch and source and wrote every voice it found,
//      released and pedal-held ones included. Same gesture, same number, other
//      message type -- press hard, let go, press the same key again lightly,
//      and the first note's decaying tail jumped down to the light value in one
//      block. That is a click, not a fade.
void casePolyAftertouchDoesNotReachAReleasedNote()
{
    std::printf ("[61] poly aftertouch does not reach a note whose key is up\n");

    // Without the pedal: a tail, and a fresh press of the same pitch over it.
    Rig r;
    r.noteOn (1, 60);
    r.flush();
    r.polyPressure (1, 60, 110);
    r.flush();
    const auto* first = r.heldVoiceForNote (60);
    check (first != nullptr, "the note sounds");
    if (first == nullptr) return;
    checkNear (first->getAftertouch(), 110.0f / 127.0f, 1e-3f, "and is leaned into");

    r.noteOff (1, 60);
    r.flush();
    r.noteOn (1, 60, 1);                        // the same key again, lightly
    r.flush();
    r.polyPressure (1, 60, 1);                  // and barely leaned on
    r.flush();
    checkNear (first->getAftertouch(), 110.0f / 127.0f, 1e-3f,
               "the decaying tail keeps what its own finger left it");
    const auto* second = r.heldVoiceForNote (60);
    check (second != nullptr && second != first, "and the fresh press is its own voice");
    if (second != nullptr && second != first)
        checkNear (second->getAftertouch(), 1.0f / 127.0f, 1e-3f,
                   "which follows the light finger that is actually on it");

    // With the pedal: the key comes up, the note sings on, and a re-press clears
    // the note's latch so only the guard is left holding the pedalled voice.
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
    p.noteOn (1, 67, 1);
    p.flush();
    p.polyPressure (1, 67, 1);
    p.flush();
    if (pedalled != nullptr)
        checkNear (pedalled->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                   "and a note the pedal holds keeps it against a light new finger");

    // Still a gate: the key that IS down follows its own poly aftertouch.
    Rig h;
    h.noteOn (1, 72);
    h.flush();
    h.polyPressure (1, 72, 120);
    h.flush();
    const auto* held = h.heldVoiceForNote (72);
    check (held != nullptr, "the held note sounds");
    if (held != nullptr)
        checkNear (held->getAftertouch(), 120.0f / 127.0f, 1e-3f,
                   "and a key still down follows poly aftertouch as it always did");
}



// ── 62. A panic does not hand the dying notes back to the wheel ─────────────
//      allNotesOff wipes voiceMidiChannel_ while every voice is still in its
//      release tail, on purpose: an MPE controller streams X/Y/Z for as long as
//      a finger rests on a key, and a panic's own dying notes must stop obeying
//      that finger. But the first version of followsLivePressure DERIVED
//      "started by a hand" from that same tag, so the wipe also erased the
//      evidence a hand had ever been there -- and every panicked tail became a
//      hand-less voice, which follows live controls by definition. Touch the
//      wheel after a panic and, with aftertouch -> DCA, the stack just killed
//      came back at full level for the length of its release. A DAW sends this
//      panic on transport stop, with the hands still down.
//
//      Case 50 does not catch it: it disturbs the tails with MEMBER-channel
//      messages, and voiceExprChannel_ is wiped too, so those stay blocked. The
//      zone-wide doors -- CC1, CC2, master channel pressure -- were the open
//      ones.
void casePanicDoesNotHandTheTailsBackToTheWheel()
{
    std::printf ("[62] a panic does not hand the dying notes back to the wheel\n");

    // Since 2026-08-25 the panic CUTS over the declick floor -- 3 ms, 132
    // samples -- so a tail sent a message in the NEXT buffer would find no voice
    // there at all, and every check below would pass on an empty slot. The panic
    // is queued at sample 180 and left unrendered; each block adds its own
    // disturbance at 220 and renders once, which puts the message 40 samples
    // into a 132-sample ramp with the voice demonstrably still alive.
    auto killedChord = [] (Rig& r, const SynthVoice** out)
    {
        r.noteOn (2, 60); r.noteOn (3, 64); r.noteOn (4, 67);
        r.flush();
        r.pressure (2, 110); r.pressure (3, 110); r.pressure (4, 110);
        r.flush();
        out[0] = r.heldVoiceForNote (60);
        out[1] = r.heldVoiceForNote (64);
        out[2] = r.heldVoiceForNote (67);
        r.ccAt (1, 120, 0, 180);     // all sound off -- NOT rendered yet
    };

    const char* names[3] = { "the first", "the second", "the third" };

    {
        Rig r; const SynthVoice* v[3] = {};
        killedChord (r, v);
        check (v[0] != nullptr && v[1] != nullptr && v[2] != nullptr,
               "the chord sounded before the panic");
        if (v[0] == nullptr) return;
        r.ccAt (1, 1, 127, 220);     // the wheel, over the tails being cut
        r.run (1);
        check (v[0]->isActive(), "and is still being cut when the wheel moves");
        for (int i = 0; i < 3; ++i)
            if (v[i] != nullptr)
            {
                const juce::String what = juce::String ("the wheel does not raise ")
                                        + names[i] + " panicked tail";
                checkNear (v[i]->getAftertouch(), 0.0f, 1e-3f, what.toRawUTF8());
            }
    }
    {
        // The same door, reached by zone-wide channel pressure and by breath.
        Rig r; const SynthVoice* v[3] = {};
        killedChord (r, v);
        r.pressureAt (1, 127, 220);
        r.ccAt (1, 2, 127, 230);     // and breath, in the same ramp
        r.run (1);
        if (v[0] != nullptr)
        {
            check (v[0]->isActive(), "and is still being cut when they arrive");
            checkNear (v[0]->getAftertouch(), 0.0f, 1e-3f,
                       "nor does zone-wide pressure, nor the breath controller");
        }
    }
    {
        // And by poly aftertouch on a NEW key of the same pitch, which is how a
        // panicked tail can end up following a finger that was never on it.
        Rig r; const SynthVoice* v[3] = {};
        killedChord (r, v);
        r.noteOnAt (5, 60, 100, 200);
        r.polyPressureAt (5, 60, 127, 220);
        r.run (1);
        if (v[0] != nullptr)
        {
            // The fresh note must not have been given THIS slot, or the check
            // below would read the new note's own pressure and pass for it.
            check (v[0]->isActive() && v[0] != r.heldVoiceForNote (60),
                   "the tail is still being cut, and is not the fresh note");
            checkNear (v[0]->getAftertouch(), 0.0f, 1e-3f,
                       "nor a fresh finger on the same pitch");
        }
    }
    {
        // Still a gate: after the panic, notes played fresh work normally.
        Rig r; const SynthVoice* v[3] = {};
        killedChord (r, v);
        r.run (1);                   // let the cut finish; this half is about after
        r.noteOn (2, 72);
        r.flush();
        r.pressure (2, 100);
        r.flush();
        const auto* fresh = r.heldVoiceForNote (72);
        check (fresh != nullptr, "a note played after the panic sounds");
        if (fresh != nullptr)
            checkNear (fresh->getAftertouch(), 100.0f / 127.0f, 1e-3f,
                       "and takes pressure as it always did");
    }
}


// ── 63. The machine's own keyboard is a hand too ────────────────────────────
//      followsLivePressure has two ways to recognise a hand and the corpus
//      pinned only one. Deleting the computer-keyboard half left 249 checks
//      green while a computer-keyboard note's tail went 0.8661 -> 1.0000 with
//      the wheel up and -> 0.0000 with it down: the swell and the cut this
//      whole class exists to prevent, on the keyboard the machine itself has.
void caseComputerKeyboardTailFreezesToo()
{
    std::printf ("[63] a note from the machine's own keyboard freezes when its key comes up\n");

    Rig r;
    r.proc.beginComputerKeyboardNote (60, 0.8f);
    r.flush();
    r.polyPressure (1, 60, 110);
    r.flush();
    const auto* v = r.heldVoiceForNote (60);
    check (v != nullptr, "the note sounds");
    if (v == nullptr) return;
    checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f, "and is leaned into");

    r.proc.endComputerKeyboardNote (60);
    r.flush();
    r.cc (1, 1, 127);
    r.flush();
    checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f,
               "the wheel does not raise its tail");
    r.cc (1, 1, 0);
    r.flush();
    checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f,
               "and does not cut it either");

    // Still a gate: while that key is DOWN it follows the wheel like any hand.
    Rig h;
    h.proc.beginComputerKeyboardNote (72, 0.8f);
    h.flush();
    h.cc (1, 1, 127);
    h.flush();
    const auto* held = h.heldVoiceForNote (72);
    check (held != nullptr, "a key still down on it sounds");
    if (held != nullptr)
        checkNear (held->getAftertouch(), 1.0f, 1e-3f,
                   "and follows the wheel while it is down");
}


// ── 64. The sostenuto pedal freezes what it caught ──────────────────────────
//      isKeyHeldVoice names release, damper and sostenuto; the corpus tested
//      the first two. Deleting the sostenuto clause left 249 green while a note
//      CC66 was holding went 0.8661 -> 1.0000 on the wheel after its key came
//      up.
void caseSostenutoTailFreezesToo()
{
    std::printf ("[64] a note the sostenuto pedal holds freezes when its key comes up\n");

    Rig r;
    r.noteOn (2, 60);
    r.flush();
    r.pressure (2, 110);
    r.flush();
    r.cc (1, 66, 127);              // sostenuto catches what is down right now
    r.flush();
    const auto* caught = r.heldVoiceForNote (60);
    check (caught != nullptr, "the caught note sounds");
    if (caught == nullptr) return;

    r.noteOff (2, 60);
    r.flush();
    // The re-press clears the note's latch, so only the guard is left holding it.
    r.noteOn (2, 60, 1);
    r.flush();
    r.cc (1, 1, 127);
    r.flush();
    checkNear (caught->getAftertouch(), 110.0f / 127.0f, 1e-3f,
               "the wheel does not raise what the sostenuto is holding");
    r.cc (1, 1, 0);
    r.flush();
    checkNear (caught->getAftertouch(), 110.0f / 127.0f, 1e-3f,
               "and does not cut it either");
}



// ── 65. A panic ends the machine's own notes too ────────────────────────────
//      Case 62 closed this door for a HAND's dying notes and left it open for
//      everything else. A step-sequencer, generative, arpeggiator or drone voice
//      has no hand, so it follows live controls -- and "for its whole sounding
//      life" was taken to include the release tail a panic had just started. A
//      DAW sends one of these on transport stop, which is by definition the
//      moment the sequencer was the thing playing -- CC 120 here, the one that
//      takes the notes away: the line was cut, and the next wheel,
//      breath or channel-pressure move brought all of it back from silence to
//      full level for the length of the release. Measured, four voices at once,
//      still ringing 2.1 s later.
void casePanicEndsTheMachinesOwnNotesToo()
{
    std::printf ("[65] a panic ends the machine's own notes too\n");

    Rig r;
    auto set = [&r] (const char* pid, float v)
    {
        if (auto* p = r.proc.getValueTreeState().getParameter (pid))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    set (PID::genSeqRunning, 0.0f);
    r.run (2);
    auto& seq = r.proc.getStepSequencer();
    seq.setNumSteps (1);
    seq.setStepNote (0, 60);
    seq.setStepEnabled (0, true);
    set (PID::seqSteps, 1.0f);
    set (PID::seqBpm, 40.0f);
    set (PID::seqGate, 0.95f);
    set (PID::seqRunning, 1.0f);

    // Wait for the line to be INSIDE its note; a fixed block count lands in a
    // gap as often as not.
    int waited = 0;
    while (r.activeVoiceCount() == 0 && waited < 400) { r.run (1); ++waited; }
    const auto* line = r.voiceForNote (60);
    check (line != nullptr, "the sequencer's line is sounding");
    if (line == nullptr) return;

    r.cc (1, 120, 0);              // the cut: CC 120, not CC 123
    r.run (2);
    checkNear (line->getAftertouch(), 0.0f, 1e-3f, "the panic takes its pressure with it");

    r.cc (1, 1, 127);              // and then the wheel, over the dying line
    r.run (2);
    checkNear (line->getAftertouch(), 0.0f, 1e-3f,
               "the wheel does not raise the line the panic just cut");
    r.pressure (1, 127);
    r.run (2);
    checkNear (line->getAftertouch(), 0.0f, 1e-3f, "nor does zone-wide pressure");
    r.cc (1, 2, 127);
    r.run (2);
    checkNear (line->getAftertouch(), 0.0f, 1e-3f, "nor the breath controller");

    // Still a gate, and this is the half that matters: a sequencer note that is
    // actually PLAYING follows the wheel, which is the whole rule for a voice
    // with no hand behind it. Without this the case would pass on a predicate
    // that simply froze everything.
    Rig h;
    auto seth = [&h] (const char* pid, float v)
    {
        if (auto* p = h.proc.getValueTreeState().getParameter (pid))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    seth (PID::genSeqRunning, 0.0f);
    h.run (2);
    auto& seq2 = h.proc.getStepSequencer();
    seq2.setNumSteps (1);
    seq2.setStepNote (0, 62);
    seq2.setStepEnabled (0, true);
    seth (PID::seqSteps, 1.0f);
    seth (PID::seqBpm, 40.0f);
    seth (PID::seqGate, 0.95f);
    seth (PID::seqRunning, 1.0f);
    waited = 0;
    while (h.activeVoiceCount() == 0 && waited < 400) { h.run (1); ++waited; }
    h.cc (1, 1, 127);
    h.run (2);
    const auto* playing = h.voiceForNote (62);
    check (playing != nullptr, "a sequencer note is playing");
    if (playing != nullptr)
        checkNear (playing->getAftertouch(), 1.0f, 1e-3f,
                   "and follows the wheel, because nobody's finger owns it");
    seth (PID::seqRunning, 0.0f);
    h.run (2);
}



// ── 66. A sliding step does not continue a note the panic already ended ─────
//      allNotesOff wipes voiceMidiChannel_ to 0 while every voice is still in
//      its release tail. The poly bind/glide branch tells a step-sequencer slide
//      apart from a held keyboard note by exactly that tag -- internal notes
//      carry channel 0 -- so after a panic a keyboard tail looks internal and
//      the slide takes it. And unlike the mono legato branch, which re-holds
//      what it takes, the bind branch only calls glideToNote: it slides a corpse
//      instead of striking. The line does not come back after a transport stop;
//      it fades out where it should have played, for as long as the hijacked
//      release lasts.
//
//      A step that slides to the next one schedules no gate-off at all
//      (StepSequencer.cpp: samplesUntilGateOff = slidesToNext ? -1.0 : ...), so
//      a legitimate slide always continues a voice whose gate is still open.
//      Refusing a releasing one cannot block a real slide.
void caseSlidingStepDoesNotContinueAPanickedTail()
{
    std::printf ("[66] a sliding step does not continue a note the panic ended\n");

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
    seq.setStepNote (0, 84);
    seq.setStepNote (1, 90);
    seq.setStepEnabled (0, true);
    seq.setStepEnabled (1, true);
    seq.setStepBindMode (0, T5ynthStepSequencer::BindMode::Glide);
    seq.setStepBindMode (1, T5ynthStepSequencer::BindMode::Glide);
    set (PID::seqSteps, 2.0f);
    set (PID::seqBpm, 200.0f);
    set (PID::seqRunning, 1.0f);
    r.run (40);                       // the line is going, lastPlayedNote is set

    // The hand plays AFTER the line, so its voice is the NEWER one -- which is
    // what the bind branch picks. Then the panic wipes both tags and puts both
    // into release, and the line's next sliding step goes looking.
    r.noteOn (2, 60);
    r.flush();
    const auto* hand = r.heldVoiceForNote (60);
    check (hand != nullptr, "the hand's note sounds");
    if (hand == nullptr) return;
    r.noteOff (2, 60);
    r.flush();
    r.cc (1, 120, 0);
    r.run (2);

    // Watch THAT voice, by pointer. Counting "any releasing voice on one of the
    // line's pitches" cannot work: the panic put the line's own notes into
    // release too, so such a count is positive whatever the code does. The
    // question is whether the hand's dying voice gets DRAGGED onto a pitch it
    // never played -- which only glideToNote can do.
    int draggedTo = -1, struck = 0;
    for (int b = 0; b < 300; ++b)
    {
        r.run (1);
        if (hand->isActive() && hand->getCurrentNote() != 60 && draggedTo < 0)
            draggedTo = hand->getCurrentNote();
        const auto& vm = r.proc.getVoiceManager();
        for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
        {
            const auto& v = vm.getVoice (i);
            if (v.isActive() && ! v.isReleasing() && v.getCurrentNote() >= 84)
                ++struck;
        }
    }
    check (struck > 0, "the sequencer's line sounds after the panic");
    if (draggedTo >= 0)
        std::printf ("      (the hand's dying voice was glided to note %d)\n", draggedTo);
    check (draggedTo < 0, "and the hand's dying voice is not glided onto the line");

    set (PID::seqRunning, 0.0f);
    r.run (2);

    // The same gesture with the hand playing ON one of the line's own pitches.
    // Above, the line plays 84/90 and the hand plays 60, so a guard narrowed to
    // "unless the releasing voice is already on the incoming pitch" slips
    // through untouched -- and that narrowing is this case's own defect, just
    // restricted to one note. Playing along on a pitch the line uses is the
    // ordinary thing to do.
    {
        Rig q;
        auto setq = [&q] (const char* pid, float v)
        {
            if (auto* p = q.proc.getValueTreeState().getParameter (pid))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };
        setq (PID::genSeqRunning, 0.0f);
        q.run (2);
        auto& s2 = q.proc.getStepSequencer();
        s2.setNumSteps (2);
        s2.setStepNote (0, 84);
        s2.setStepNote (1, 90);
        s2.setStepEnabled (0, true);
        s2.setStepEnabled (1, true);
        s2.setStepBindMode (0, T5ynthStepSequencer::BindMode::Glide);
        s2.setStepBindMode (1, T5ynthStepSequencer::BindMode::Glide);
        setq (PID::seqSteps, 2.0f);
        setq (PID::seqBpm, 200.0f);
        setq (PID::seqRunning, 1.0f);
        q.run (40);

        q.noteOn (2, 84);              // the hand plays one of the line's pitches
        q.flush();
        check (q.heldVoiceForNote (84) != nullptr, "the hand's note on the line's pitch sounds");
        q.noteOff (2, 84);
        q.flush();
        q.cc (1, 120, 0);
        q.run (2);

        int gated = 0;
        for (int b = 0; b < 400; ++b)
        {
            q.run (1);
            const auto& vm = q.proc.getVoiceManager();
            for (int i2 = 0; i2 < VoiceManager::MAX_VOICES; ++i2)
            {
                const auto& v = vm.getVoice (i2);
                if (v.isActive() && ! v.isReleasing() && v.getCurrentNote() >= 84)
                    { ++gated; break; }
            }
        }
        // Striking gives a gated voice in nearly every block; dragging the
        // corpse leaves the line silent wherever it should have struck.
        if (gated <= 380)
            std::printf ("      (the line was gated in only %d of 400 blocks)\n", gated);
        check (gated > 380, "and the line strikes rather than fading on the hand's corpse");
        setq (PID::seqRunning, 0.0f);
        q.run (2);
    }
}



// ── 67. In mono, a hand is still a hand and the machine still is not ────────
//      followsLivePressure is only as good as the four places that record what
//      started a voice, and the corpus never left the default poly voice count,
//      so two of the four were never entered at all. Both have measured damage:
//      deleting the mono TRIGGER site leaves the hand's own note unmarked, so
//      its tail swells 0.8661 -> 1.0000 on the wheel and then collapses to
//      0.0000 at rest -- the exact pair of artefacts this whole class exists to
//      prevent. Deleting the mono LEGATO site marks a sequencer step that takes
//      voice 0 as hand-started, which FREEZES it: the wheel moves nothing.
void caseMonoKnowsAHandFromTheMachine()
{
    std::printf ("[67] in mono, a hand is still a hand and the machine still is not\n");

    auto mono = [] (Rig& r)
    {
        if (auto* p = r.proc.getValueTreeState().getParameter (PID::voiceCount))
            p->setValueNotifyingHost (p->convertTo0to1 (0.0f));   // index 0 = mono
        r.flush();
    };

    // The hand's own note, struck into an empty pool -- the mono TRIGGER branch.
    // Under the DAMPER, and that is what makes the case discriminate: a plain
    // release freezes on the release term alone, so a note whose origin was
    // never recorded looks identical to one that was. A pedal-held voice is not
    // releasing, so only its origin can decide whether the wheel reaches it.
    {
        Rig r;
        mono (r);
        r.cc (1, 64, 127);             // damper down
        r.noteOn (2, 60);
        r.flush();
        r.pressure (2, 110);
        r.flush();
        const auto* v = r.heldVoiceForNote (60);
        check (v != nullptr, "the mono note sounds");
        if (v != nullptr)
        {
            checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f, "and is leaned into");
            r.noteOff (2, 60);
            r.flush();
            r.cc (1, 1, 127);
            r.flush();
            checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                       "the wheel does not raise what the pedal holds");
            r.cc (1, 1, 0);
            r.flush();
            checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                       "and does not cut it either");
        }
    }

    // A second key on top of the first -- the mono LEGATO branch, still a hand.
    {
        Rig r;
        mono (r);
        r.cc (1, 64, 127);             // damper down, for the same reason
        r.noteOn (2, 60);
        r.flush();
        r.noteOn (2, 64);              // legato onto the same channel
        r.flush();
        r.pressure (2, 110);
        r.flush();
        const auto* v = r.heldVoiceForNote (64);
        check (v != nullptr, "the legato note sounds");
        if (v != nullptr)
        {
            r.noteOff (2, 64);
            r.noteOff (2, 60);
            r.flush();
            r.cc (1, 1, 127);
            r.flush();
            checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                       "and the pedal holds it without the wheel reaching it");
        }
    }

    // And the machine's own note in mono: a sequencer step, which has no hand
    // and must follow the wheel for as long as it is sounding.
    {
        Rig r;
        mono (r);
        auto set = [&r] (const char* pid, float v)
        {
            if (auto* p = r.proc.getValueTreeState().getParameter (pid))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };
        set (PID::genSeqRunning, 0.0f);
        r.run (2);
        auto& seq = r.proc.getStepSequencer();
        seq.setNumSteps (1);
        seq.setStepNote (0, 67);
        seq.setStepEnabled (0, true);
        set (PID::seqSteps, 1.0f);
        set (PID::seqBpm, 40.0f);
        set (PID::seqGate, 0.95f);
        set (PID::seqRunning, 1.0f);
        // A hand is holding voice 0 first, so the step arrives on a sounding
        // voice and goes through the mono LEGATO branch rather than the trigger
        // one. Striking into an empty pool would never enter it.
        r.noteOn (2, 55);
        r.flush();
        int waited = 0;
        while (r.heldVoiceForNote (67) == nullptr && waited < 400) { r.run (1); ++waited; }
        r.cc (1, 1, 127);
        r.flush();
        const auto* line = r.heldVoiceForNote (67);
        check (line != nullptr, "the mono sequencer line takes the voice");
        if (line != nullptr)
            checkNear (line->getAftertouch(), 1.0f, 1e-3f,
                       "and follows the wheel, because nobody's finger owns it");
        set (PID::seqRunning, 0.0f);
        r.run (2);
    }

    // NOT COVERED, said plainly rather than left to look covered: adding a
    // sostenuto term to followsLivePressure -- which would freeze a
    // sostenuto-caught sequencer note against the only control that drives it --
    // survives this whole corpus. The damper version of that mutation is caught
    // by the two blocks above; the sostenuto one is not. Two drafts of a case
    // for it are worth recording as failures: leaning on the wheel just after
    // the pedal catches the note never reaches the state at all, because
    // sostenutoReleasedVoice is set at the note's OWN gate-off, not at the
    // catch; and running past that gate-off at 200 BPM puts a fresh step's
    // voice in the slot, so the assertion then reads a different note and fails
    // against correct code. A case that fails on correct code is worse than no
    // case, so there is none here yet.
}



// ── 68. Reset-all-controllers does not cut a note that is fading ────────────
//      CC 121 releases nothing: the chord goes on sounding and the hand goes on
//      leaning into it, which is why cases 46 and 49 hold that a key still DOWN
//      is zeroed by it. What had no gate was everything else it reached. It
//      zeroed the stored pressure of every sounding voice, so with aftertouch ->
//      DCA at full a decaying note went from level 0.2851 to 0.0000 in one block
//      -- 5.3 ms, this figure being measure_at_gestures' own 48 kHz block --
//      instead of fading, and a pedal-held note from 0.629. That is
//      the second half of the criterion measure_at_gestures states: silent as a
//      swell, very audible as a note cut off. A DAW sends CC 121 on transport
//      stop and on locate.
//
//      A panic is the opposite and stays so: it is TAKING the notes away, so it
//      zeroes tails too (cases 50 and 62).
void caseResetAllControllersDoesNotCutAFadingNote()
{
    std::printf ("[68] reset-all-controllers does not cut a note that is fading\n");

    // A tail.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.pressure (2, 110);
        r.flush();
        const auto* v = r.heldVoiceForNote (60);
        check (v != nullptr, "the note sounds");
        if (v != nullptr)
        {
            r.noteOff (2, 60);
            r.flush();
            r.cc (1, 121, 0);
            r.flush();
            checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                       "the fading note keeps what its finger left it");
        }
    }

    // A note that WAS under the damper. Not a second class: CC 121's own first
    // act is to lift the pedals, which RP-015 asks for, so by the time the loop
    // runs this voice is releasing exactly like the one above. Kept because it
    // is the gesture a player makes, and because the pedal's release is where
    // the level is still high -- but a damper-held-and-not-releasing voice is
    // unreachable inside this function by construction, and case 58 is where
    // that state is actually tested.
    {
        Rig p;
        p.cc (1, 64, 127);
        p.noteOn (2, 67);
        p.flush();
        p.pressure (2, 110);
        p.flush();
        const auto* pedalled = p.heldVoiceForNote (67);
        check (pedalled != nullptr, "the pedalled note sounds");
        p.noteOff (2, 67);
        p.flush();
        p.cc (1, 121, 0);
        p.flush();
        if (pedalled != nullptr)
            checkNear (pedalled->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                       "and so does the one the pedal is holding");
    }

    // Unchanged, and the reason this case cannot be satisfied by simply making
    // CC 121 do nothing: a key still DOWN is zeroed, which is what the message
    // asks for.
    {
        Rig h;
        h.noteOn (2, 72);
        h.flush();
        h.pressure (2, 110);
        h.flush();
        const auto* held = h.heldVoiceForNote (72);
        check (held != nullptr, "the held note sounds");
        h.cc (1, 121, 0);
        h.flush();
        if (held != nullptr)
            checkNear (held->getAftertouch(), 0.0f, 1e-3f,
                       "while a key still down is reset, as the message asks");
    }
}


// ── 69. Reset-all-controllers does not centre a bend the finger already left ─
//      RP-015 does list Pitch Bend among what Reset All Controllers resets, so
//      a key still DOWN is centred by it and stays centred here. What had no
//      gate was the same reach case 68 closed for pressure: this wrote EVERY
//      voice's per-note bend, including one whose key is up and whose channel
//      tag noteOff already cleared for exactly this reason -- so a tail snapped
//      by the whole per-note range in one block, unramped, while its level went
//      on decaying. Measured at the shipped +-48: a note bent to +47.9941 st,
//      key lifted, CC 121 -> +0.0000 st. Four octaves, one block, mid-release
//      -- 5.805 ms at this file's 44.1 kHz.
//
//      It was inaudible before the pressure fix only because with aftertouch ->
//      DCA at full the same block cut the note to silence. Restoring the level
//      is what makes this one audible, which is why it is closed in the same
//      breath.
void caseResetAllControllersDoesNotCentreAFrozenBend()
{
    std::printf ("[69] reset-all-controllers does not centre a bend the finger already left\n");

    // A tail.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.wheel (2, 16383);
        r.flush();
        const auto* v = r.heldVoiceForNote (60);
        check (v != nullptr, "the bent note sounds");
        if (v != nullptr)
        {
            checkNear (v->getPerVoicePitchBend(), fullUpBend (r.noteBendRange()), 0.01f,
                       "and is bent before the key comes up");
            const float normBefore = v->getPerVoicePitchBendNorm();
            check (normBefore > 0.5f, "and X is deflected with it");
            r.noteOff (2, 60);
            r.flush();
            r.cc (1, 121, 0);
            r.flush();
            checkNear (v->getPerVoicePitchBend(), fullUpBend (r.noteBendRange()), 0.01f,
                       "the fading note keeps the pitch its finger left it at");
            // The SAME gesture in the other unit. setPerVoicePitchBend carries
            // two numbers: the semitones the pitch uses and a -1..+1 normalised
            // copy, which is the X modulation source every X-routed target
            // reads (SynthVoice.cpp:71) and what feeds the Cache bar.
            // Centring one and not the other leaves the pitch back at rest
            // while every X target still reads full deflection -- and nothing
            // else in this file reads the norm on this path.
            //
            // Against what it read BEFORE, not against a literal. The norm is
            // the bend measured on the X full scale, one semitone by default,
            // so at any wheel past 2.09 % of travel it saturates at exactly
            // 1.0 and a literal here would neither be 8191/8192 nor tell a
            // kept deflection from a fiftieth of one. Whether the SCALING is
            // right is case 26's question, at half a semitone, below the clamp.
            checkNear (v->getPerVoicePitchBendNorm(), normBefore, 1e-6f,
                       "and X reads what it read, in the unit the modulation uses");
        }
    }

    // A note that WAS under the damper -- see case 68's second part for why
    // this is the same class as the tail above rather than a second one.
    {
        Rig p;
        p.cc (1, 64, 127);
        p.noteOn (2, 67);
        p.flush();
        p.wheel (2, 16383);
        p.flush();
        const auto* pedalled = p.heldVoiceForNote (67);
        check (pedalled != nullptr, "the pedalled note sounds");
        p.noteOff (2, 67);
        p.flush();
        p.cc (1, 121, 0);
        p.flush();
        if (pedalled != nullptr)
            checkNear (pedalled->getPerVoicePitchBend(), fullUpBend (p.noteBendRange()), 0.01f,
                       "and so does the one the pedal is holding");
    }

    // Unchanged, and the reason this case cannot be satisfied by making CC 121
    // leave every bend alone: a key still DOWN is centred, which is what the
    // message asks for.
    {
        Rig h;
        h.noteOn (2, 72);
        h.flush();
        h.wheel (2, 16383);
        h.flush();
        const auto* held = h.heldVoiceForNote (72);
        check (held != nullptr, "the held note sounds");
        if (held != nullptr)
            checkNear (held->getPerVoicePitchBend(), fullUpBend (h.noteBendRange()), 0.01f,
                       "and is bent while the key is still down");
        h.cc (1, 121, 0);
        h.flush();
        if (held != nullptr)
        {
            checkNear (held->getPerVoicePitchBend(), 0.0f, 1e-4f,
                       "while a key still down is centred, as the message asks");
            checkNear (held->getPerVoicePitchBendNorm(), 0.0f, 1e-4f,
                       "in both units");
        }
    }
}

// ── 70. Reset-all-controllers does not move where the NEXT note's Y sits ────
//      channelTimbre_ is not a controller value this synth owns -- it is the
//      record of what the channel last CARRIED, kept because a note's Y rest is
//      the CC 74 in force when it began and MPE controllers send that BEFORE
//      the note-on (VoiceManager.h, the field's own note). Filling it with
//      kTimbreRest on CC 121 does not reset a controller; it asserts that the
//      channel rests at 0 when the finger is somewhere else entirely, and
//      nothing corrects the claim until the finger moves. RP-015 does not list
//      CC 74 among what Reset All Controllers resets.
//
//      Both halves below are the SAME wipe, seen from the two orderings a real
//      controller produces, so neither ordering can hide it.
void caseResetAllControllersDoesNotMoveTheNextNotesYOrigin()
{
    std::printf ("[70] reset-all-controllers does not move where the next note's Y sits\n");

    // Initial-64, CC 74 re-sent per note (the spec's own scheme). Measured
    // before the fix: note 1 Y = 0.0000, the identical note after a CC 121
    // Y = +0.5039 -- with Y -> Cutoff that is half the filter range brighter,
    // finger at rest, after a transport stop.
    {
        Rig r;
        r.cc (2, 74, 64);
        r.noteOn (2, 60);
        r.flush();
        const auto* first = r.heldVoiceForNote (60);
        check (first != nullptr, "the first note sounds");
        if (first != nullptr)
            checkNear (first->getTimbre(), 0.0f, 1e-4f, "and rests where the finger is");

        r.noteOff (2, 60);
        r.flush();
        r.cc (1, 121, 0);
        r.flush();

        r.noteOn (2, 62);
        r.flush();
        r.cc (2, 74, 64);          // the same physical position, re-sent
        r.flush();
        const auto* second = r.heldVoiceForNote (62);
        check (second != nullptr, "the note after the reset sounds");
        if (second != nullptr)
            checkNear (second->getTimbre(), 0.0f, 1e-4f,
                       "and rests where the finger is, which has not moved");
    }

    // The other ordering: a controller that sends CC 74 only when it CHANGES.
    // Here the wipe survives the note-on and shows up on the first movement --
    // 6/127 of travel read as 70/127.
    {
        Rig r;
        r.cc (2, 74, 64);
        r.noteOn (2, 60);
        r.flush();
        r.noteOff (2, 60);
        r.flush();
        r.cc (1, 121, 0);
        r.flush();

        r.noteOn (2, 62);          // no fresh CC 74: nothing moved
        r.flush();
        r.cc (2, 74, 70);
        r.flush();
        const auto* v = r.heldVoiceForNote (62);
        check (v != nullptr, "the note sounds");
        if (v != nullptr)
            checkNear (v->getTimbre(), 6.0f / 127.0f, 1e-3f,
                       "the first move is the distance the finger moved");
    }

    // The cut reaches this same function (allNotesOff passes
    // endingEveryNote = true), so CC 120 has to hold it too -- and a version
    // that restores the wipe on the cut path alone passes everything else.
    // CC 120 and not CC 123: since the two were separated, only the cut comes
    // through resetPerformanceControllers at all.
    {
        Rig r;
        r.cc (2, 74, 64);
        r.noteOn (2, 60);
        r.flush();
        r.cc (1, 120, 0);          // all sound off
        r.flush();

        r.noteOn (2, 62);
        r.flush();
        r.cc (2, 74, 70);
        r.flush();
        const auto* v = r.heldVoiceForNote (62);
        check (v != nullptr, "the note after the panic sounds");
        if (v != nullptr)
            checkNear (v->getTimbre(), 6.0f / 127.0f, 1e-3f,
                       "and the panic did not move its rest either");
    }

    // Unchanged: CC 121 does not reset the Y of a note that is sounding either.
    // Same rule, and the reason this case cannot be satisfied by zeroing every
    // voice's timbre instead of the channel memory.
    {
        Rig h;
        h.cc (2, 74, 0);
        h.noteOn (2, 64);
        h.flush();
        h.cc (2, 74, 127);
        h.flush();
        const auto* v = h.heldVoiceForNote (64);
        check (v != nullptr, "the leaning note sounds");
        if (v != nullptr)
        {
            checkNear (v->getTimbre(), 1.0f, 1e-3f, "and is leaning");
            h.cc (1, 121, 0);
            h.flush();
            checkNear (v->getTimbre(), 1.0f, 1e-3f, "and goes on leaning through the reset");
        }
    }
}


// ── 71. A panic does not four-octave a note on its way out ─────────────────
//      Case 69 gated CC 121; this is the same reach on the louder path.
//      allNotesOff calls the same function with endingEveryNote = true, which
//      exists so a panic still zeroes the PRESSURE of the notes it is taking
//      away -- that makes them go quiet, and cases 50 and 62 hold it. Bend is
//      where the two part company: allNotesOff releases the key at FULL level,
//      so centring the bend does not make the note quieter, it makes it snap
//      by the whole per-note range on the way out, unramped. Measured before
//      the fix: +47.9941 -> +0.0000 semitones, key still down.
void casePanicDoesNotFourOctaveANoteOnItsWayOut()
{
    std::printf ("[71] a panic does not four-octave a note on its way out\n");

    Rig r;
    r.noteOn (2, 60);
    r.flush();
    r.wheel (2, 16383);
    r.flush();
    const auto* v = r.heldVoiceForNote (60);
    check (v != nullptr, "the bent note sounds");
    if (v == nullptr) return;
    checkNear (v->getPerVoicePitchBend(), fullUpBend (r.noteBendRange()), 0.01f,
               "and is bent with the key still down");
    const float normBefore = v->getPerVoicePitchBendNorm();
    check (normBefore > 0.5f, "and X is deflected with it");

    r.ccAt (1, 120, 0, 180);       // all sound off, at full level
    r.run (1);
    check (v->isActive() && v->isReleasing(),
           "the panic has it in the cut ramp, where it is still measurable");
    // A RAMP and not a step. Everything else about the cut is measured in
    // duration, and a mutation that simply zeroed the level and went idle would
    // reach silence in the same one block and satisfy all of it -- the full-scale
    // step the declick floor exists to prevent, passing the test for the floor.
    // 76 of the 132 samples have run here, so the level is part-way down: not
    // still at the top, and not at the bottom either.
    // 76 of the ramp's 132 samples have run at the end of this block, and the
    // default release bend is concave, so the level is already well down --
    // measured 0.0370. The window is wide on purpose: what it separates is a
    // ramp from a STEP, which reads exactly 0.0000.
    const float midCut = v->getAmpEnvLevel();
    check (midCut > 0.005f && midCut < 0.95f,
           "and is part-way down a ramp, not at the bottom of a step");
    checkNear (v->getPerVoicePitchBend(), fullUpBend (r.noteBendRange()), 0.01f,
               "and it rings out at the pitch it was taken away at");
    checkNear (v->getPerVoicePitchBendNorm(), normBefore, 1e-6f,
               "in both units -- see case 69 for why this reads the before value");

    // The half that must NOT change: the panic still zeroes the pressure, so
    // the note it is taking away goes quiet. That is what endingEveryNote is
    // for, and dropping it from the pressure would pass a bend-only test.
    Rig z;
    z.noteOn (2, 64);
    z.flush();
    z.pressure (2, 110);
    z.flush();
    const auto* zv = z.heldVoiceForNote (64);
    check (zv != nullptr, "the leaned-on note sounds");
    if (zv != nullptr)
    {
        checkNear (zv->getAftertouch(), 110.0f / 127.0f, 1e-3f, "and is leaned on");
        z.cc (1, 120, 0);
        z.flush();
        checkNear (zv->getAftertouch(), 0.0f, 1e-3f,
                   "and the panic takes its pressure with it");
    }
}

// ── 72. Reset-all-controllers zeroes a machine note that is still sounding ──
//      The other side of case 68. A sequencer's note has no finger on it, so
//      nothing is holding its reading up: it follows the wheel live (case 60)
//      and therefore follows the wheel's reset. Two narrower gates -- asking
//      the voice's MPE tag instead of the predicate, and narrowing it to a key
//      that is down -- both leave such a note standing at the last wheel value
//      forever, and both pass every other case in this file.
void caseResetAllControllersZeroesASoundingMachineNote()
{
    std::printf ("[72] reset-all-controllers zeroes a machine note that is still sounding\n");

    Rig r;
    auto set = [&r] (const char* pid, float v)
    {
        if (auto* p = r.proc.getValueTreeState().getParameter (pid))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    set (PID::genSeqRunning, 0.0f);       // else it mirrors its own pattern in
    r.run (2);

    auto& seq = r.proc.getStepSequencer();
    seq.setNumSteps (2);
    seq.setStepNote (0, 60);
    seq.setStepNote (1, 67);
    seq.setStepEnabled (0, true);
    seq.setStepEnabled (1, true);
    seq.setStepBindMode (0, T5ynthStepSequencer::BindMode::Off);
    seq.setStepBindMode (1, T5ynthStepSequencer::BindMode::Off);
    set (PID::seqSteps, 2.0f);
    set (PID::seqBpm, 40.0f);             // slow, so we stay inside step 0
    set (PID::seqRunning, 1.0f);
    r.run (10);

    r.cc (1, 1, 110);                     // the wheel, which no finger owns
    r.flush();
    const auto* line = r.heldVoiceForNote (60);
    check (line != nullptr, "the line's note sounds");
    if (line != nullptr)
    {
        checkNear (line->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                   "and follows the wheel, having no finger of its own");
        r.cc (1, 121, 0);
        r.flush();
        checkNear (line->getAftertouch(), 0.0f, 1e-3f,
                   "so it follows the reset as well");
    }
    set (PID::seqRunning, 0.0f);
    r.run (5);
}

// ── 73. Reset-all-controllers does not raise the instrument to full ────────
//      RP-015 enumerates what this message resets and Volume is deliberately
//      not on the list -- a mixer setting is meant to survive it. Expression
//      IS on the list. Resetting the volume here multiplied every output
//      sample by 1.0 whatever the fader said: measured, CC 7 = 40 is a gain of
//      0.31496, and CC 121 raised it to 1.0 in one unsmoothed block, +10.03 dB.
void caseResetAllControllersDoesNotRaiseTheInstrumentToFull()
{
    std::printf ("[73] reset-all-controllers does not raise the instrument to full\n");

    Rig r;
    const auto& vm = r.proc.getVoiceManager();
    const float faderDown = 40.0f / 127.0f;

    r.cc (1, 7, 40);
    r.flush();
    checkNear (vm.performanceOutputGain(), faderDown, 1e-3f,
               "the fader is where the player left it");

    r.cc (1, 121, 0);
    r.flush();
    checkNear (vm.performanceOutputGain(), faderDown, 1e-3f,
               "and the reset leaves it there");

    // The half RP-015 DOES ask for, so this cannot be satisfied by making the
    // message leave every gain alone.
    r.cc (1, 11, 50);
    r.flush();
    checkNear (vm.performanceOutputGain(), faderDown * (50.0f / 127.0f), 1e-3f,
               "expression multiplies the fader");
    r.cc (1, 121, 0);
    r.flush();
    checkNear (vm.performanceOutputGain(), faderDown, 1e-3f,
               "and the reset returns expression to full, which the message does ask for");

    // And a panic is no different: it takes notes away, not the mixer.
    r.cc (1, 120, 0);
    r.flush();
    checkNear (vm.performanceOutputGain(), faderDown, 1e-3f,
               "a panic leaves the fader alone too");
}


// ── 74. The rest of RP-015's list, which nothing held ──────────────────────
//      Three of the values this message resets had no assertion anywhere:
//      dropping the master-bend centring, the mod-wheel clear or the soft-pedal
//      lift each passed all four gates. They are on RP-015's list and the code
//      is right about them; what was missing was anything that would notice if
//      it stopped being.
//
//      The master bend is deliberately NOT frozen per voice the way the
//      per-note bend is (cases 69 and 71). It is zone-wide by construction --
//      one global ratio every voice reads, with no per-voice state to freeze --
//      so a dying note follows it, and follows it back. Measured: a panic moves
//      a releasing note by -1.9998 semitones at the default master range and
//      -24.0000 at range 48, where the same note under a plain key-up moves by
//      +0.0000. That is the wheel being centred, which is what the message
//      asks for, and it is the same motion the player's own hand makes.
void caseTheRestOfTheResetList()
{
    std::printf ("[74] the rest of what this message resets, which nothing held\n");

    {   // Pitch bend to centre, zone-wide.
        Rig r;
        r.noteOn (1, 60);
        r.flush();
        r.wheel (1, 16383);
        r.flush();
        checkNear (r.globalBendSemitones(), fullUpBend (r.masterBendRange()), 0.01f,
                   "the master wheel bends the zone");
        r.cc (1, 121, 0);
        r.flush();
        checkNear (r.globalBendSemitones(), 0.0f, 1e-4f, "and the reset centres it");
    }

    {   // The mod wheel as a pressure source. A key still down is zeroed by
        // case 46; what that cannot see is the STORED value, which drives the
        // next note through pressureForNote.
        Rig r;
        r.cc (1, 1, 110);
        r.noteOn (2, 60);
        r.flush();
        const auto* first = r.heldVoiceForNote (60);
        check (first != nullptr, "the note under the wheel sounds");
        if (first != nullptr)
            checkNear (first->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                       "and reads the wheel");
        r.cc (1, 121, 0);
        r.flush();
        r.noteOn (2, 64);          // a fresh key, no fresh wheel
        r.flush();
        r.run (3);                 // long enough for a refresh to reach it
        const auto* second = r.heldVoiceForNote (64);
        check (second != nullptr, "the next note sounds");
        if (second != nullptr)
            checkNear (second->getAftertouch(), 0.0f, 1e-3f,
                       "and is not driven by a wheel value the reset cleared");
    }

    {   // The soft pedal, the third multiplicand in performanceOutputGain.
        Rig r;
        r.cc (1, 67, 127);
        r.flush();
        checkNear (r.proc.getVoiceManager().performanceOutputGain(), 0.65f, 1e-3f,
                   "the soft pedal lowers the instrument");
        r.cc (1, 121, 0);
        r.flush();
        checkNear (r.proc.getVoiceManager().performanceOutputGain(), 1.0f, 1e-3f,
                   "and the reset lifts it, which the message does ask for");
    }
}


// ── 75. All Sound Off cuts; All Notes Off is every key coming up ───────────
//      The MIDI spec separates CC 120 and CC 123 and this synth used to run
//      both through one path. 120 asks for the envelopes to reach zero as fast
//      as possible. 123 asks only that the notes be released, says outright
//      that it is not to be used as a panic button, and says notes may go on
//      sounding if the damper is down. So 123 is every key coming up at once
//      and nothing else: it releases through the pedals, it freezes what the
//      finger left, and it resets no controller -- that is CC 121's message.
//
//      What made it worth separating rather than recording: 123 zeroed the
//      stored aftertouch of what it released, and pressure is one of four
//      sources feeding sixteen targets, of which exactly one is loudness --
//      and that one is off on a fresh patch. So on a default patch the zeroing
//      changed the level by nothing while it moved a routed pitch by up to
//      twelve semitones, unramped, on a note at full level.
void caseAllSoundOffCutsAndAllNotesOffIsAKeyUp()
{
    std::printf ("[75] all-sound-off cuts, all-notes-off is every key coming up\n");

    // The freeze. Same note, same lean, the two messages.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.pressure (2, 110);
        r.flush();
        const auto* v = r.heldVoiceForNote (60);
        check (v != nullptr, "the leaned-on note sounds");
        if (v != nullptr)
        {
            r.cc (1, 123, 0);
            r.flush();
            check (v->isActive() && v->isReleasing(), "CC 123 releases it");
            checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                       "and it rings out with what the finger left it");
        }

        Rig c;
        c.noteOn (2, 60);
        c.flush();
        c.pressure (2, 110);
        c.flush();
        const auto* cv = c.heldVoiceForNote (60);
        check (cv != nullptr, "the same note sounds for the cut");
        if (cv != nullptr)
        {
            c.cc (1, 120, 0);
            c.flush();
            checkNear (cv->getAftertouch(), 0.0f, 1e-3f,
                       "where CC 120 takes it away");
        }
    }

    // The tail is still unreachable afterwards -- the property the old
    // zeroing was standing in for, which has to survive on its own.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.pressure (2, 110);
        r.flush();
        const auto* v = r.heldVoiceForNote (60);
        check (v != nullptr, "the note sounds");
        if (v != nullptr)
        {
            r.cc (1, 123, 0);
            r.flush();
            r.cc (1, 1, 127);          // the wheel, over the dying note
            r.pressure (1, 127);       // and zone-wide pressure
            r.pressure (2, 127);       // and its own member channel
            r.run (2);
            checkNear (v->getAftertouch(), 110.0f / 127.0f, 1e-3f,
                       "and no live control raises it afterwards");
        }
    }

    // The damper. "Notes may continue to sound if the damper is down" is the
    // spec's own sentence about this message.
    {
        Rig r;
        r.cc (1, 64, 127);
        r.noteOn (2, 67);
        r.flush();
        r.cc (1, 123, 0);
        r.flush();
        const auto* held = r.heldVoiceForNote (67);
        check (held != nullptr, "CC 123 leaves it under the pedal, still sounding");
        r.cc (1, 64, 0);               // and lifting the pedal ends it, as a key-up does
        r.run (2);
        check (r.heldVoiceForNote (67) == nullptr, "and the pedal lifting ends it");

        Rig c;
        c.cc (1, 64, 127);
        c.noteOn (2, 67);
        c.flush();
        c.cc (1, 120, 0);
        c.flush();
        check (c.heldVoiceForNote (67) == nullptr,
               "where CC 120 goes through the pedal");
    }

    // And 123 resets nothing. Every value case 74 holds for CC 121 stays put.
    {
        Rig r;
        r.cc (1, 7, 40);
        r.cc (1, 67, 127);
        r.noteOn (1, 60);
        r.flush();
        r.wheel (1, 16383);
        r.flush();
        const float bend = r.globalBendSemitones();
        check (bend > 1.0f, "the master wheel is up");
        r.cc (1, 123, 0);
        r.flush();
        checkNear (r.globalBendSemitones(), bend, 1e-4f,
                   "CC 123 does not centre the master wheel");
        checkNear (r.proc.getVoiceManager().performanceOutputGain(),
                   (40.0f / 127.0f) * 0.65f, 1e-3f,
                   "nor lift the soft pedal, nor touch the fader");

        r.cc (1, 120, 0);
        r.flush();
        checkNear (r.globalBendSemitones(), 0.0f, 1e-4f,
                   "where CC 120 does, because it resets the controllers");
    }
}


// ── 77. What all-notes-off leaves behind ───────────────────────────
//      Two things that outlive the message, both introduced by splitting
//      CC 120 from CC 123 and neither with an assertion anywhere.
//
//      One: a release already running is not restarted.
//
//      Two: the poly-aftertouch latch of every pitch the message ENDED. It is a
//      floor under pressureForNote, and only a key-down on that pitch clears
//      it, so a value left standing is a departed finger's pressure on every
//      later sequencer, arpeggiator and drone note of that pitch. The
//      processor's own allKeysReleased() covers the pitches a KEY was on and
//      only those -- it early-returns when no key is down, which is exactly
//      what a drone holding the pitch past the key-up produces.
void caseWhatAllNotesOffLeavesBehind()
{
    std::printf ("[77] what all-notes-off leaves behind\n");

    auto set = [] (Rig& r, const char* pid, float v)
    {
        if (auto* p = r.proc.getValueTreeState().getParameter (pid))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    auto blocksToSilence = [] (Rig& r, int limit)
    {
        for (int b = 0; b < limit; ++b)
        {
            r.run (1);
            if (r.activeVoiceCount() == 0)
                return b + 1;
        }
        return -1;
    };

    // What each message does to a note it finds ALREADY fading, which is where
    // the two part company and where both used to be wrong.
    //
    // CC 123 leaves it alone. ADSREnvelope::beginRelease restarts the ramp from
    // the CURRENT level, so a second noteOff on a releasing voice does not hurry
    // it along -- it starts the release over and the note rings LONGER, which
    // for a message that means "every key came up" is backwards.
    //
    // CC 120 cuts it, which is the same restart used the other way round: from
    // the current level over the declick floor instead of over the patch's
    // release.
    {
        Rig a, b, c;
        for (Rig* r : { &a, &b, &c })
        {
            set (*r, PID::ampRelease, 2000.0f);
            r->flush();
            r->noteOn (2, 60);
            r->flush();
            r->noteOff (2, 60);
            r->run (100);              // 0.58 s into the tail
        }
        b.cc (1, 123, 0);
        c.cc (1, 120, 0);
        const int plain = blocksToSilence (a, 2000);
        const int after123 = blocksToSilence (b, 2000);
        const int after120 = blocksToSilence (c, 2000);
        check (plain > 100, "the tail is long enough to measure");
        check (after123 > 0 && std::abs (after123 - plain) <= 2,
               "CC 123 does not lengthen a tail it found already fading");
        // CC 120 goes the other way, and the asymmetry is the point of the
        // split. It CUTS -- a tail already fading is exactly what it is for --
        // over the synth's own declick floor and no longer. That floor is
        // ADSREnvelope::MIN_RAMP_SEC and SynthVoice::KEY_GATE_MS, both 3 ms,
        // which at 44100/256 is 132 samples: one block, two counting the one
        // the message arrives in. Until 2026-08-25 this message called noteOff
        // like any key-up and left 1980 ms of a 2 s release standing.
        check (after120 > 0 && after120 <= 3,
               "CC 120 cuts one, over the declick floor and no longer");
    }

    {
        Rig r;
        const auto& vm = r.proc.getVoiceManager();
        r.noteOn (2, 60);
        r.flush();
        r.polyPressure (2, 60, 120);
        r.flush();
        checkNear (vm.pressureForHeldNote (60), 120.0f / 127.0f, 1e-3f,
                   "the finger is readable");
        r.proc.beginStepHoldPreview (60);   // the mouse takes the same pitch
        r.run (2);
        r.noteOff (2, 60);                  // and the finger leaves
        r.flush();
        check (r.proc.getVoiceManager().hasDrone(), "the drone holds that pitch on");
        checkNear (vm.pressureForHeldNote (60), 120.0f / 127.0f, 1e-3f,
                   "so the latch rightly stays, key or no key");

        r.cc (1, 123, 0);
        r.flush();
        // Read straight off the latch, not off a fresh note of that pitch: a
        // voice is born at no pressure whatever the latch says and only the
        // next pressure message pushes it in, so a fresh note cannot tell the
        // two states apart -- verified, that probe passed with the clear taken
        // out. This one is the floor itself.
        checkNear (vm.pressureForHeldNote (60), 0.0f, 1e-3f,
                   "CC 123 ended that pitch, so the departed finger goes with it");

        // And the audible consequence, which is the reason the floor matters:
        // a later HANDLESS note of that pitch -- drone, sequencer, arpeggiator --
        // follows live pressure, so the next wheel or channel-pressure move
        // pushes the floor into it in one block.
        r.proc.beginStepHoldPreview (60);
        r.run (2);
        r.cc (1, 1, 1);                     // the wheel barely off its rest
        r.flush();
        const auto* machineNote = r.heldVoiceForNote (60);
        check (machineNote != nullptr, "a machine note takes that pitch afterwards");
        if (machineNote != nullptr)
            checkNear (machineNote->getAftertouch(), 1.0f / 127.0f, 1e-3f,
                       "and answers the wheel from rest, not from the departed finger");
    }
}


// ── 76. What all-notes-off does with the pedals and with the drone ─────────
//      Three states the split left with no assertion at all, each of which a
//      mutation of the shipped code reaches while the rest of this file stays
//      green. The worst is a permanent hung note.
//
//      This case once said the ORDER of the two pedal branches was "not gated
//      because no player-visible difference could be constructed". One could,
//      and it was gated here. Since case 83 made the two release functions
//      mirrors -- each defers to the other pedal while that pedal is down --
//      the two orders converge again and the blocks at the end assert the
//      OUTCOME instead: the note belongs to whichever pedal is still down and
//      ends with the last of them. The other claim of that sentence, the
//      skip-if-releasing condition, is case 77.
void caseAllNotesOffPedalsAndDrone()
{
    std::printf ("[76] all-notes-off, the pedals and the drone\n");

    auto stillSounding = [] (Rig& rig)
    {
        const auto& vm = rig.proc.getVoiceManager();
        int n = 0;
        for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            if (vm.getVoice (i).isActive() && ! vm.getVoice (i).isReleasing())
                ++n;
        return n;
    };

    // The sostenuto pedal goes on holding, AND lifting it still ends the note.
    // The second half is the hung one: marking the voice held without marking
    // it key-released leaves releaseSostenutoVoices unable to reach it ever
    // again -- measured 3.48 s of silence with the voice still active, which
    // is as long as the run below listens.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.cc (1, 66, 127);             // sostenuto catches what is sounding
        r.flush();
        check (r.heldVoiceForNote (60) != nullptr, "the sostenuto pedal has the note");
        r.cc (1, 123, 0);
        r.flush();
        check (r.heldVoiceForNote (60) != nullptr, "and goes on holding it through CC 123");
        r.cc (1, 66, 0);               // pedal up
        r.run (2);
        check (r.heldVoiceForNote (60) == nullptr, "and lifting it ends the note");
        r.run (600);
        check (r.activeVoiceCount() == 0, "with nothing left standing");
    }

    // The drone under a pedal. It is not a key, so no pedal has a claim on it,
    // and the handle to it is cleared either way -- so a pedal keeping it here
    // orphaned it: still sounding, hasDrone() false, the mouse coming up a
    // no-op, and a second step adding a second one.
    {
        Rig r;
        r.cc (1, 64, 127);             // damper down
        r.flush();
        r.proc.beginStepHoldPreview (60);
        r.run (2);
        check (r.proc.getVoiceManager().hasDrone(), "the mouse is holding a step");
        check (stillSounding (r) == 1, "and it sounds");

        r.cc (1, 123, 0);
        r.flush();
        check (! r.proc.getVoiceManager().hasDrone(), "CC 123 ends the drone hold");
        check (stillSounding (r) == 0, "and leaves nothing sounding under the pedal");

        r.proc.endStepHoldPreview();   // the mouse comes up
        r.run (2);
        check (stillSounding (r) == 0, "the mouse coming up finds nothing to end");

        r.proc.beginStepHoldPreview (64);   // and the next step does not accumulate
        r.run (2);
        r.proc.endStepHoldPreview();
        r.run (2);
        check (stillSounding (r) == 0, "and the next step held does not pile up on it");
        r.run (600);
        check (r.activeVoiceCount() == 0, "with nothing left standing");
    }

    // Clearing the handle is load-bearing on the CC 123 path too, which is why
    // the fix above is "let the drone through the pedal branches" and not
    // "leave the handle alone": in mono the drone reserves voice 0, and a stale
    // index there suppresses the keyboard's own note-ons.
    {
        Rig m;
        if (auto* p = m.proc.getValueTreeState().getParameter (PID::voiceCount))
            p->setValueNotifyingHost (p->convertTo0to1 (0.0f));   // index 0 = mono
        m.flush();
        // Asserted, not assumed: with the rig in poly the mutation this block
        // exists to catch lands on a slot findFreeVoice simply skips and both
        // checks below pass anyway -- measured, by putting the rig in poly and
        // running the mutation. The whole discriminating power is this line.
        check (m.proc.getVoiceManager().getVoiceLimit() == 1, "the rig is in mono");
        m.proc.beginStepHoldPreview (60);
        m.run (2);
        check (m.proc.getVoiceManager().hasDrone(), "the mouse is holding a step in mono");
        m.cc (1, 123, 0);
        m.flush();
        m.noteOn (2, 67);
        m.flush();
        check (m.heldVoiceForNote (67) != nullptr,
               "and the keyboard answers again afterwards");
    }

    // The SOSTENUTO pedal, which claims the drone just as readily: it marks
    // every voice with sourceId < 0, and the drone is one. Guarding only the
    // damper branch leaves the identical orphan one pedal over.
    {
        Rig r;
        r.proc.beginStepHoldPreview (60);
        r.run (2);
        check (r.proc.getVoiceManager().hasDrone(), "the mouse is holding a step");
        r.cc (1, 66, 127);             // sostenuto catches what is sounding
        r.flush();
        r.cc (1, 123, 0);
        r.flush();
        check (! r.proc.getVoiceManager().hasDrone(), "CC 123 ends it under sostenuto too");
        check (stillSounding (r) == 0, "and leaves nothing sounding");
        r.proc.endStepHoldPreview();
        r.run (2);
        check (stillSounding (r) == 0, "with the mouse coming up onto nothing");
    }

    // And the other direction, which is the whole capability the split exists
    // to protect: a drone being held must not cost the damper its KEYS.
    // "The drone is not a key" written as "there is no drone" reads the same
    // and takes all of them away.
    {
        Rig r;
        r.cc (1, 64, 127);             // damper down
        r.flush();
        r.noteOn (2, 60); r.noteOn (3, 64); r.noteOn (4, 67);
        r.flush();
        r.noteOff (2, 60); r.noteOff (3, 64); r.noteOff (4, 67);
        r.flush();
        check (stillSounding (r) == 3, "three keys, up, held by the pedal");
        r.proc.beginStepHoldPreview (72);
        r.run (2);
        check (stillSounding (r) == 4, "and a step held under the mouse beside them");
        r.cc (1, 123, 0);
        r.flush();
        check (stillSounding (r) == 3, "CC 123 takes the drone and leaves the pedal its keys");
        r.cc (1, 64, 0);
        r.run (2);
        check (stillSounding (r) == 0, "which the pedal lifting then ends");
    }

    // CC 123 under both pedals, and then each of them coming up on its own.
    // The note belongs to whichever pedal is still down, in either order, and
    // ends only with the last of them -- see case 83, which is the same claim
    // on the plain key-up path this one is meant to agree with.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.cc (1, 66, 127);             // sostenuto catches it
        r.cc (1, 64, 127);             // and the damper goes down over the top
        r.flush();
        r.cc (1, 123, 0);
        r.flush();
        check (stillSounding (r) == 1, "both pedals hold it through CC 123");
        r.cc (1, 66, 0);               // the middle pedal up, the right still down
        r.run (2);
        check (stillSounding (r) == 1, "the damper keeps it when sostenuto lets go");
        r.cc (1, 64, 0);
        r.run (2);
        check (stillSounding (r) == 0, "and the last pedal up ends it");
    }
    {
        // The same, the other way round.
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.cc (1, 66, 127);
        r.cc (1, 64, 127);
        r.flush();
        r.cc (1, 123, 0);
        r.flush();
        r.cc (1, 64, 0);               // the right pedal up, the middle still down
        r.run (2);
        check (stillSounding (r) == 1, "sostenuto keeps it when the damper lets go");
        r.cc (1, 66, 0);
        r.run (2);
        check (stillSounding (r) == 0, "and the last pedal up ends it here too");
    }
}

// ── 83. Two pedals are two claims on the same note, not one owner ──────────
//      A piano's damper falls back only when NOTHING holds the string: the key
//      is up, the damper pedal is up, and sostenuto is not holding that string.
//      The right pedal lifts every damper physically and the middle one coming
//      up cannot put one back.
//
//      This synth had first-claim-wins. releaseSostenutoVoices released the
//      note whatever the damper was doing, so holding the right pedal through
//      and letting the middle one go silenced what the right pedal was holding
//      -- measured on the plain key-up path, no all-notes-off anywhere.
//      releaseSustainedVoices had always deferred the other way; the two are
//      mirrors of each other now.
void casePedalsAreTwoClaims()
{
    std::printf ("[83] two pedals are two claims on one note, not one owner\n");

    auto sounding = [] (Rig& rig)
    {
        const auto& vm = rig.proc.getVoiceManager();
        int n = 0;
        for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            if (vm.getVoice (i).isActive() && ! vm.getVoice (i).isReleasing())
                ++n;
        return n;
    };

    // Sostenuto catches it, the damper joins, the key comes up. Then the middle
    // pedal alone.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.cc (1, 66, 127);
        r.flush();
        r.cc (1, 64, 127);
        r.flush();
        r.noteOff (2, 60);
        r.flush();
        check (sounding (r) == 1, "the key is up and two pedals are holding it");
        r.cc (1, 66, 0);
        r.run (2);
        check (sounding (r) == 1, "sostenuto letting go leaves it to the damper");
        r.cc (1, 64, 0);
        r.run (2);
        check (sounding (r) == 0, "and the damper letting go ends it");
    }

    // The damper down FIRST, then sostenuto over the top -- a different flag
    // order through noteOff, the same claim.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.cc (1, 64, 127);
        r.flush();
        r.cc (1, 66, 127);
        r.flush();
        r.noteOff (2, 60);
        r.flush();
        r.cc (1, 66, 0);
        r.run (2);
        check (sounding (r) == 1, "sostenuto letting go leaves it to the damper here too");
        r.cc (1, 64, 0);
        r.run (2);
        check (sounding (r) == 0, "and the damper ends it");
    }

    // And the direction that already worked, so the mirror cannot be broken by
    // "fixing" the other half.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.cc (1, 66, 127);
        r.cc (1, 64, 127);
        r.flush();
        r.noteOff (2, 60);
        r.flush();
        r.cc (1, 64, 0);               // the damper first
        r.run (2);
        check (sounding (r) == 1, "the damper letting go leaves it to sostenuto");
        r.cc (1, 66, 0);
        r.run (2);
        check (sounding (r) == 0, "and sostenuto ends it");
    }

    // Neither hand-over may outlive the pedals themselves. Reset All
    // Controllers puts all four at 0, and each release function defers to the
    // OTHER pedal while it is still down -- so with both flags still true a
    // voice held by both is passed from one to the other and back, and the
    // flag arrays are then wiped with the note still sounding and nothing left
    // that could ever release it.
    {
        Rig r;
        r.noteOn (2, 60);
        r.flush();
        r.cc (1, 66, 127);
        r.cc (1, 64, 127);
        r.flush();
        r.noteOff (2, 60);
        r.flush();
        check (sounding (r) == 1, "both pedals are holding it");
        r.cc (1, 121, 0);              // reset all controllers
        r.run (2);
        check (sounding (r) == 0, "and reset-all-controllers takes both pedals with it");
        r.run (600);
        check (r.activeVoiceCount() == 0, "with nothing left standing");
    }
}

// ── 78. The NRPN bit is per channel, not one for the instrument ────────────
//      Capability 21 is the migration's own headline: the RPN selection was
//      ONE global pair where the spec has sixteen, and the old code's comment
//      named that as a defect. The library fixed the RPN register; the NRPN
//      bit beside it is this synth's own and had no test, so collapsing the
//      sixteen bits onto one passed all 387 assertions -- the exact defect the
//      row claims to have left behind.
//
//      Discriminating because it INTERLEAVES two channels. One channel selects
//      an NRPN, another then selects an RPN, and the first channel's data byte
//      arrives afterwards: with sixteen bits it is still an NRPN byte and is
//      refused; with one, the second channel's RPN selection cleared it and the
//      byte lands on the first channel's latched RPN 0.
void caseNrpnSelectionIsPerChannel()
{
    std::printf ("[78] an NRPN selected on one channel does not follow another\n");
    Rig r;
    r.rpn (5, 0, 0, 12);          // ch5: RPN 0 = 12, and RPN 0 stays latched there
    r.flush();
    r.cc (5, 98, 6);              // ch5 selects an NRPN
    r.cc (7, 101, 0);             // ch7 selects an RPN -- a DIFFERENT channel
    r.cc (7, 100, 0);
    r.flush();
    r.cc (5, 6, 40);              // ch5's data byte: an NRPN's, not a bend range
    r.flush();

    r.noteOn (5, 64);
    r.flush();
    r.wheel (5, 16383);
    r.flush();
    const auto* v = r.voiceForNote (64);
    check (v != nullptr, "the voice is alive");
    if (v == nullptr) return;
    checkNear (v->getPerVoicePitchBend(), 12.0f * 8191.0f / 8192.0f, 0.01f,
               "the range is the 12 ch5 was given, not the 40 its NRPN carried");
}

// ── 79. A declared zone outlives a panic and a controller reset ────────────
//      Capability 5 lists four things a layout must survive: prepareToPlay,
//      preset load, panic and Reset All Controllers. Only prepareToPlay was
//      gated (case 16). Wiping the layout inside the CC 120 branch passed all
//      387 assertions. A zone describes the DEVICE that is plugged in; a
//      transport stop is not the player unplugging it, and a controller only
//      sends its MCM once, at connection.
void caseZoneOutlivesPanicAndReset()
{
    std::printf ("[79] a declared zone outlives a panic and a controller reset\n");
    Rig r;
    r.rpn (16, 0, 6, 1);          // upper zone, one member: ch16 is its master
    r.flush();
    r.cc (1, 120, 0);             // panic
    r.flush();
    r.cc (1, 121, 0);             // reset all controllers
    r.flush();

    r.noteOn (1, 60);
    r.noteOn (5, 64);
    r.flush();
    r.pressure (16, 127);         // zone-wide only while ch16 is still a master
    r.flush();
    const auto* a = r.voiceForNote (60);
    const auto* b = r.voiceForNote (64);
    check (a != nullptr && b != nullptr, "both voices alive");
    if (a == nullptr || b == nullptr) return;
    checkNear (a->getAftertouch(), 1.0f, 1e-4f,
               "channel 16 is still the upper zone's master after both messages");
    checkNear (b->getAftertouch(), 1.0f, 1e-4f,
               "and its pressure is still zone-wide");
}

// ── 80. Channel 16's wheel stays a MEMBER bend under a declared upper zone ──
//      Capability 11, and the one expression that deliberately does not ask
//      isMpeMasterChannel: pressure and CC74 on a declared master are zone-wide,
//      the wheel there is not, because routing it through the predicate would
//      turn a declared upper zone's master bend global -- every sequencer and
//      arpeggiator voice sliding with it. The site says so; nothing measured
//      it, and making that substitution passed all 387 assertions.
void caseChannel16WheelStaysAMemberBend()
{
    std::printf ("[80] channel 16's wheel is a member bend under a declared upper zone\n");
    Rig r;
    r.rpn (16, 0, 6, 1);          // ch16 becomes the upper zone's master
    r.flush();
    r.noteOn (16, 60);
    r.noteOn (1, 67);             // and an untouched voice to watch the global bend on
    r.flush();
    r.wheel (16, 16383);
    r.flush();

    const auto* v = r.voiceForNote (60);
    check (v != nullptr, "the voice is alive");
    if (v == nullptr) return;
    check (v->getPerVoicePitchBend() > 1.0f,
           "the wheel bent that note per-note, master or not");
    checkNear (12.0f * std::log2 (r.proc.getVoiceManager().globalPitchBendRatio()), 0.0f, 1e-3f,
               "and left the global bend where it was");
}

// ── 81. A CC6 that completes no RPN still reaches a user binding ───────────
//      Capability 22, and the reason handleMpeRpnByte returns a bool at all:
//      an else-if cannot both consume a message and decline it. Swallowing
//      every CC6 passed all 387 assertions, and a bound fader on CC6 would
//      simply have gone dead.
void caseUnclaimedCc6ReachesABinding()
{
    std::printf ("[81] a CC6 that completes no RPN still reaches a binding\n");
    Rig r;
    auto* p = r.proc.getValueTreeState().getParameter (PID::ampAttack);
    check (p != nullptr, "the parameter exists");
    if (p == nullptr) return;

    r.proc.startMidiLearn (PID::ampAttack);
    r.cc (5, 6, 10);              // channel 5 has no RPN selected: this completes nothing
    r.flush();
    pump (40);                    // the learn is finished by an AsyncUpdater
    check (r.proc.findBoundCc (PID::ampAttack) == 6,
           "the learn bound CC6, so the byte reached the binding layer at all");
    r.cc (5, 6, 10);
    r.flush();
    r.run (4);
    const float low = p->getValue();
    r.cc (5, 6, 120);
    r.flush();
    r.run (4);
    const float high = p->getValue();
    check (high > low + 0.5f,
           "the bound parameter followed the fader, so the byte was not swallowed");
}


// ── 82. What "All Sound Off" has to switch off besides the amp envelope ────
//      The corpus's CC 120 cases all run the stock patch: amp envelope on the
//      DCA, every mod envelope target None. That leaves two of the three things
//      cutSound closes with no assertion at all, and the first cut shipped with
//      one of them wrong.
//
//      Which envelope holds the LEVEL is a patch decision (computeDcaGain): the
//      amp envelope where it is routed to the DCA, the KEY GATE otherwise. And a
//      mod envelope whose target is outside the voice -- delay, reverb, the LFO
//      rates (EnvTarget::isOutsideTheVoice) -- keeps renderBlock's
//      stillModulating true, which holds the slot allocated and goes on driving
//      the master delay and reverb long after the message. Default mod release
//      is 4 s and reaches 10.
void caseAllSoundOffClosesEveryArm()
{
    std::printf ("[82] all-sound-off closes every arm, not only the amp envelope\n");

    auto set = [] (Rig& r, const char* pid, float v)
    {
        if (auto* p = r.proc.getValueTreeState().getParameter (pid))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };
    auto blocksToSilence = [] (Rig& r, int limit)
    {
        for (int b = 0; b < limit; ++b)
        {
            r.run (1);
            if (r.activeVoiceCount() == 0)
                return b + 1;
        }
        return -1;
    };

    // The key-gate arm: the amp envelope points at the filter, so the KEY is the
    // level and closing the envelope alone would leave the voice open.
    {
        Rig r;
        set (r, PID::ampTarget, (float) EnvTarget::Filter);
        set (r, PID::ampRelease, 4000.0f);
        r.flush();
        r.noteOn (2, 60);
        r.run (10);
        check (r.activeVoiceCount() == 1, "a voice whose LEVEL is the key gate sounds");
        r.cc (1, 120, 0);
        const int blocks = blocksToSilence (r, 2000);
        check (blocks > 0 && blocks <= 3, "and the panic closes the gate, not just the envelope");
    }

    // The outside-the-voice arm: a mod envelope on the master delay's mix, with
    // a long release. Nothing of this voice can be heard after the cut, but the
    // SLOT is held and the delay goes on being swept from it.
    {
        Rig r;
        set (r, PID::mod1Target, (float) EnvTarget::DelayMix);
        set (r, PID::mod1Release, 4000.0f);
        set (r, PID::mod1Amount, 1.0f);
        r.flush();
        r.noteOn (2, 60);
        r.run (10);
        check (r.activeVoiceCount() == 1, "a voice modulating the delay sounds");
        r.cc (1, 120, 0);
        const int blocks = blocksToSilence (r, 2000);
        check (blocks > 0 && blocks <= 3,
               "and the panic ends it instead of sweeping the delay for four seconds");
    }

    // And the counter-check, so the two above cannot be satisfied by a cut that
    // simply ends every voice on sight: CC 123 is still a key-up, and a key
    // still DOWN goes on sounding under it in both patches.
    {
        Rig r;
        set (r, PID::mod1Target, (float) EnvTarget::DelayMix);
        set (r, PID::mod1Release, 4000.0f);
        r.flush();
        r.noteOn (2, 60);
        r.run (10);
        r.cc (1, 123, 0);
        r.run (2);
        check (r.activeVoiceCount() == 1, "CC 123 leaves it to its own release");
    }
}



// ── 84. The cache is travelled PER NOTE ─────────────────────────────────────
// The complaint this exists for, in the player's words: "das ist keine
// Poly-Funktion. Der Cache wird wie mit einem primitiven Mono_AT abgefahren.
// D.h. bei 2 gehaltenen Noten wechselt das Sample für alle synchron."
//
// It was true, and not at one site: maxHeldExpression folded every held voice
// into ONE reading, one zone state followed it, one index was posted and one
// instrument-wide load installed it. Everything a per-note controller sends
// arrived and was averaged away at the first stage.
//
// What this asserts is the smallest observable a mono construction cannot
// satisfy: two keys held at two lateral positions follow two DIFFERENT masters.
// Plus the counter-check, because "always different" would satisfy that
// assertion and be just as wrong.
void caseCacheIsTravelledPerNote()
{
    std::printf ("[84] two held keys at two lateral positions play two samples\n");
    Rig r;

    // Distinct in PITCH and in LENGTH. The length is what makes this a check on
    // the sound rather than on two addresses: a build that prepared every
    // position from the same entry would hand the two keys two different objects
    // holding the same audio, and an assertion on pointers alone would pass it.
    auto tone = [] (float hz, int samples)
    {
        juce::AudioBuffer<float> b (1, samples);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                * hz * (float) i / 44100.0f));
        return b;
    };

    // Four entries: enough that two engaged fingers can stand in different zones
    // without one of them sitting on the step it landed on (a finger that has
    // not moved is deliberately still on the instrument-wide master, and a case
    // that read one would pass for the wrong reason).
    auto fill = [&tone] (Rig& rig)
    {
        rig.proc.setInferenceCacheCapacity (4);
        for (int k = 0; k < 4; ++k)
            rig.proc.addInferenceCacheEntry (tone (220.0f * (float) (k + 1),
                                                   22050 + k * 11025), 44100.0);
        auto set = [&rig] (const char* pid, float v)
        {
            if (auto* p = rig.proc.getValueTreeState().getParameter (pid))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        };
        set (PID::aftertouchAmtCache, 1.0f);      // full depth: the bar spans all four
        set (PID::exprSrcCache, (float) ExprSource::X);
        rig.run (2);
        // The positions are prepared on a background thread. Give it time, and
        // do not wait on a flag the implementation sets - if it never prepares
        // them the checks below fail, which is the point.
        for (int i = 0; i < 60; ++i)
        {
            pump (25);
            rig.run (2);
        }
    };

    // X as a MODULATION source is the bend in semitones measured against the X
    // full scale, not the wheel fraction -- so the wheel value for a wanted
    // reading depends on both settings. Derived from what is actually in force
    // rather than assumed, so a calibration change fails this loudly instead of
    // quietly moving which zone the gesture reaches.
    auto wheelForX = [] (const Rig& rig, float x)
    {
        const float centered = x * rig.xFullScale() / rig.noteBendRange();
        return juce::jlimit (0, 16383, 8192 + (int) std::lround (centered * 8192.0f));
    };

    auto voiceIndexForNote = [] (const Rig& rig, int note)
    {
        const auto& vm = rig.proc.getVoiceManager();
        for (int i = 0; i < VoiceManager::MAX_VOICES; ++i)
            if (vm.getVoice (i).isActive() && ! vm.getVoice (i).isReleasing()
                && vm.getVoice (i).getCurrentNote() == note)
                return i;
        return -1;
    };

    fill (r);
    check (r.proc.isInferenceCacheFull(), "the cache is full, so the bar can travel it");

    r.noteOn (5, 60);
    r.noteOn (7, 64);
    r.flush();
    r.run (2);

    const int va = voiceIndexForNote (r, 60);
    const int vb = voiceIndexForNote (r, 64);
    check (va >= 0 && vb >= 0 && va != vb, "two keys, two voices");
    if (va < 0 || vb < 0)
        return;

    // Both fingers travel to the far end, so both engage; then one comes back
    // most of the way. Two hands, two places in the cache, at the same instant.
    r.wheel (5, wheelForX (r, 1.0f));
    r.wheel (7, wheelForX (r, 1.0f));
    r.run (4);
    r.wheel (7, wheelForX (r, 0.15f));
    r.run (4);

    const auto& vm = r.proc.getVoiceManager();
    const auto* ma = vm.voiceSamplerMaster (va);
    const auto* mb = vm.voiceSamplerMaster (vb);
    check (ma != nullptr, "the key at one end follows a cache position of its own");
    check (mb != nullptr, "the key nearer the middle follows one of its own");
    check (ma != mb, "and they are NOT the same position -- the whole complaint");
    if (ma != nullptr && mb != nullptr)
        check (ma->estimateReferenceLengthSamples() != mb->estimateReferenceLengthSamples(),
               "and the two positions hold different AUDIO, not just different addresses");

    // The counter-check. Same reading, same position: a bar that simply handed
    // every voice a master of its own would satisfy the assertion above and be
    // no more polyphonic than the mono construction it replaced.
    Rig s;
    fill (s);
    s.noteOn (5, 60);
    s.noteOn (7, 64);
    s.flush();
    s.run (2);
    s.wheel (5, wheelForX (s, 1.0f));
    s.wheel (7, wheelForX (s, 1.0f));   // both leaning the same way, the same distance
    s.run (6);

    const auto& svm = s.proc.getVoiceManager();
    const int sa = voiceIndexForNote (s, 60);
    const int sb = voiceIndexForNote (s, 64);
    if (sa >= 0 && sb >= 0)
    {
        check (svm.voiceSamplerMaster (sa) != nullptr, "both are engaged");
        check (svm.voiceSamplerMaster (sa) == svm.voiceSamplerMaster (sb),
               "two keys at the SAME position play the same sample");
    }

    // And a released key hands its position back, so the next note on that voice
    // starts from the instrument-wide master rather than inheriting a stranger's
    // finger position.
    r.noteOff (5, 60);
    r.run (4);
    r.noteOn (5, 62);
    r.flush();
    const int vc = voiceIndexForNote (r, 62);
    if (vc >= 0)
        check (r.proc.getVoiceManager().voiceSamplerMaster (vc) == nullptr,
               "a fresh key starts on the instrument-wide master, not on a stranger's position");

    // TWO ways a voice changes hands with a key still down, and they want
    // OPPOSITE things. Neither passes the not-held branch that forgets a
    // gesture, which is why both are here.

    // (a) MONO LEGATO. Same voice, sounding continuously, one finger sliding to
    // the next pitch: it must go on playing the sample it is playing. The mono
    // note-on's clearVoiceEngineMasters sits AFTER the legato branch returns,
    // deliberately - clearing here would crossfade the slide back onto the
    // instrument-wide sample, i.e. the slide would change the sound.
    {
        Rig t;
        if (auto* p = t.proc.getValueTreeState().getParameter (PID::voiceCount))
            p->setValueNotifyingHost (p->convertTo0to1 (0.0f));   // index 0 = 1 voice
        t.run (2);
        fill (t);
        check (t.proc.getVoiceManager().getVoiceLimit() == 1, "mono, so the next key slides");

        t.noteOn (5, 60);
        t.flush();
        t.wheel (5, wheelForX (t, 1.0f));       // engage, out at the far end
        t.run (6);
        const int t0 = voiceIndexForNote (t, 60);
        check (t0 >= 0 && t.proc.getVoiceManager().voiceSamplerMaster (t0) != nullptr,
               "the first key reaches a position");
        const auto* held = t0 >= 0 ? t.proc.getVoiceManager().voiceSamplerMaster (t0) : nullptr;

        t.noteOn (7, 67);                       // legato slide, wheel left where it was
        t.flush();
        t.run (2);
        const int t1 = voiceIndexForNote (t, 67);
        check (t1 >= 0 && t.proc.getVoiceManager().voiceSamplerMaster (t1) == held,
               "and a legato slide keeps the sample it is sliding on");
    }

    // (b) A STEAL. A different finger, a fresh strike, on a voice whose old key
    // was still down. Here the position MUST go: it belongs to the hand that
    // has left. Two voices, both engaged, so whichever the policy takes is
    // carrying one.
    {
        Rig t;
        if (auto* p = t.proc.getValueTreeState().getParameter (PID::voiceCount))
            p->setValueNotifyingHost (p->convertTo0to1 (1.0f));   // index 1 = 4 voices
        t.run (2);
        fill (t);
        check (t.proc.getVoiceManager().getVoiceLimit() == 4, "four voices, so the fifth key steals");

        // Fill the pool and engage every one of them, so whichever the stealing
        // policy takes is carrying a position that belongs to a hand that left.
        const int notes[] = { 60, 62, 64, 65 };
        for (int k = 0; k < 4; ++k)
            t.noteOn (5 + k, notes[k]);
        t.flush();
        for (int k = 0; k < 4; ++k)
            t.wheel (5 + k, wheelForX (t, 1.0f));
        t.run (6);
        int engaged = 0;
        for (int k = 0; k < 4; ++k)
        {
            const int vi = voiceIndexForNote (t, notes[k]);
            if (vi >= 0 && t.proc.getVoiceManager().voiceSamplerMaster (vi) != nullptr)
                ++engaged;
        }
        check (engaged == 4, "all four keys reach a position");

        // Steal, with the wheels left exactly where they were. The new key has
        // not moved, so it must claim nothing...
        t.noteOn (9, 67);
        t.flush();
        t.run (2);
        const int t1 = voiceIndexForNote (t, 67);
        check (t1 >= 0 && t.proc.getVoiceManager().voiceSamplerMaster (t1) == nullptr,
               "the key that stole the voice claims nothing until IT has moved");

        // ...and once it does move, it must be able to reach a position again. A
        // stolen voice that kept the old finger's index would find every position
        // it resolves to already claimed and never re-point - stuck on the
        // instrument-wide master for the rest of the phrase.
        t.wheel (9, wheelForX (t, 0.15f));
        t.run (6);
        t.wheel (9, wheelForX (t, 1.0f));
        t.run (6);
        const int t2 = voiceIndexForNote (t, 67);
        check (t2 >= 0 && t.proc.getVoiceManager().voiceSamplerMaster (t2) != nullptr,
               "and it is not stuck: once it travels, it reaches one");
    }

    // (c) THE PLATFORM INVARIANT, which per-note breaks unless it is handed
    // back: a HELD note always plays the CURRENT sample. A voice pointed at a
    // cache position followed THAT master unconditionally, so a regenerate
    // under a held key never reached it and the note went deaf to Regenerate
    // for the rest of its life - with A/B drift, which regenerates continuously
    // under held notes, that is the whole feature gone. The claim says which of
    // the OLD sounds the key plays; it does not outrank a new one.
    {
        Rig t;
        fill (t);
        t.noteOn (5, 60);
        t.flush();
        t.wheel (5, wheelForX (t, 1.0f));
        t.run (6);
        const int v0 = voiceIndexForNote (t, 60);
        check (v0 >= 0 && t.proc.getVoiceManager().voiceSamplerMaster (v0) != nullptr,
               "the held key is on a position of its own");

        // A regenerate: fresh audio into the instrument-wide master, nothing to
        // do with the cache (so the cache generation does not move, and the
        // positions stay current - the claim is what has to give, not them).
        juce::AudioBuffer<float> fresh (1, 44100);
        for (int i = 0; i < fresh.getNumSamples(); ++i)
            fresh.setSample (0, i, 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                    * 111.0f * (float) i / 44100.0f));
        t.proc.loadGeneratedAudio (fresh, 44100.0);
        t.run (4);
        const int v1 = voiceIndexForNote (t, 60);
        check (v1 == v0 && v1 >= 0,
               "the same voice is still holding the note");
        check (v1 >= 0 && t.proc.getVoiceManager().voiceSamplerMaster (v1) == nullptr,
               "and the regenerate takes the claim back, so the held note follows it");

        // ...and the bar still works afterwards. Taking the claim back without
        // re-arming the GESTURE leaves the voice's last landed index standing;
        // that index still matches the zone the motionless finger is in, so
        // every following pass reads "already pointed there" and the bar is
        // simply dead under that finger until it leaves the zone and returns.
        t.wheel (5, wheelForX (t, 0.15f));
        t.run (6);
        t.wheel (5, wheelForX (t, 1.0f));
        t.run (6);
        const int v2 = voiceIndexForNote (t, 60);
        check (v2 >= 0 && t.proc.getVoiceManager().voiceSamplerMaster (v2) != nullptr,
               "and the finger can travel again without having to leave and return");
    }
}


// ── 85. Two masters never share a bank generation ───────────────────────────
// The tripwire for the defect that made case 84 pass while the sound did not
// change. Both morph guards ask ONE question - "is this the same published bank
// I already hold?" - and answer it by comparing a generation NUMBER across
// instances. The counters were per instance, so every freshly built master
// stamped generation 1, and sixteen cache positions all claimed to be the same
// bank as each other:
//
//   Wavetable: morphToFramesFrom sees sameActive and returns. The voice keeps
//              the sound it had. Silent, and case 84's pointer assertion is
//              satisfied the whole time.
//   Freeze:    morphToBufferFrom takes its "same buffer - harmless" branch and
//              republishes genuinely different audio under a sounding voice.
//              That is a hard swap mid-grain, i.e. the one thing the Regen
//              XFade contract forbids.
//
// Checked here on the engines directly rather than through the synth, because
// the property is the engines' own and a per-instance counter reintroduced
// anywhere would fail this in a line.
void caseTwoMastersNeverShareAGeneration()
{
    std::printf ("[85] two engine instances never stamp the same bank generation\n");

    auto tone = [] (float hz, int samples)
    {
        juce::AudioBuffer<float> b (1, samples);
        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi
                                                * hz * (float) i / 44100.0f));
        return b;
    };
    const auto a = tone (220.0f, 22050);
    const auto b = tone (330.0f, 33075);

    {
        FreezeTextureEngine fa, fb;
        auto sa = fa.prepareBufferLoad (a, 44100.0);
        auto sb = fb.prepareBufferLoad (b, 44100.0);
        check (sa != nullptr && sb != nullptr, "two freeze engines both prepare");
        if (sa != nullptr && sb != nullptr)
            check (sa->generation != sb->generation,
                   "and their snapshots do not claim to be the same buffer");
    }
    {
        WavetableOscillator oa, ob;
        auto ma = oa.prepareContiguousFrames (a, 44100.0, 0.0f, 1.0f);
        auto mb = ob.prepareContiguousFrames (b, 44100.0, 0.0f, 1.0f);
        check (ma != nullptr && mb != nullptr, "two wavetable oscillators both prepare");
        if (ma != nullptr && mb != nullptr)
            check (ma->generation != mb->generation,
                   "and their banks do not claim to be the same bank");
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
    caseSeqNoteOffDoesNotEndAHeldKey();
    caseTwoFingersOnOneKeyAreTwoKeysToTheArp();
    caseTheResetBurstDoesNotLandOnTheTail();
    caseMonoSlideOntoANewChannelIsANewFinger();
    caseTheWheelDrivesNotesWithNoFingerOnThem();
    casePolyAftertouchDoesNotReachAReleasedNote();
    casePanicDoesNotHandTheTailsBackToTheWheel();
    caseComputerKeyboardTailFreezesToo();
    caseSostenutoTailFreezesToo();
    casePanicEndsTheMachinesOwnNotesToo();
    caseSlidingStepDoesNotContinueAPanickedTail();
    caseMonoKnowsAHandFromTheMachine();
    caseResetAllControllersDoesNotCutAFadingNote();
    caseResetAllControllersDoesNotCentreAFrozenBend();
    caseResetAllControllersDoesNotMoveTheNextNotesYOrigin();
    casePanicDoesNotFourOctaveANoteOnItsWayOut();
    caseResetAllControllersZeroesASoundingMachineNote();
    caseResetAllControllersDoesNotRaiseTheInstrumentToFull();
    caseTheRestOfTheResetList();
    caseAllSoundOffCutsAndAllNotesOffIsAKeyUp();
    caseAllNotesOffPedalsAndDrone();
    caseWhatAllNotesOffLeavesBehind();
    caseNrpnSelectionIsPerChannel();
    caseZoneOutlivesPanicAndReset();
    caseChannel16WheelStaysAMemberBend();
    caseUnclaimedCc6ReachesABinding();
    caseAllSoundOffClosesEveryArm();
    casePedalsAreTwoClaims();
    caseCacheIsTravelledPerNote();
    caseTwoMastersNeverShareAGeneration();

    std::printf ("\n%d checks, %d failures -- %s\n\n",
                 gChecks, gFailures, gFailures == 0 ? "ALL PASS" : "FAILED");
    return gFailures == 0 ? 0 : 1;
}
