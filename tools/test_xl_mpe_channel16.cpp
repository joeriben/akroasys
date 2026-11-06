// A note on MIDI channel 16 is a note, with a Launch Control XL open or without.
//
// BJ, 2026-11-06, on an Osmose: "verpasst STAENDIG eine angeschlagene Note ...
// das kommt mehrfach in einem song vor -> NoGo". An Osmose rotates its notes
// strictly over channels 2-16, and selecting an XL as the MIDI output made
// processBlock refuse every Note On on channel 16, so every fifteenth struck
// key made no sound. Replaying a real 90 s capture lost 6 of 85 notes, all six
// on channel 16. The guard only ever had to stop the XL's echo of a DAW-mode
// switch, and those bytes are also a key, note 12 on channel 16. So each switch
// excuses one message of its own kind, in the second after it; everything else
// has to play (DawModeEchoFilter in src/midi/LaunchControlXLLeds.h).
//
// tools/test_mpe_parity.cpp cannot reach this: it has no XL open. Here the
// output is REAL. A virtual CoreMIDI destination carries "LCXL" in its name, so
// openMidiOutputDevice takes the path a player's selection takes, and the
// enable arriving at the destination proves DAW mode is on before a note is
// sent. Nothing private is touched.
//
// Build: the recipe at the top of tools/test_mpe_parity.cpp, with this file.
// Run with DYLD_FALLBACK_LIBRARY_PATH=third_party/csound/macos-arm64/lib

#include <CoreFoundation/CoreFoundation.h>

#include "../src/PluginProcessor.h"
#include "../src/midi/LaunchControlXLLeds.h"
#include <atomic>
#include <cstdio>
#include <utility>
#include <vector>

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr int    kBlockSize  = 128;

    int failures = 0;

    void check (bool ok, const char* what)
    {
        std::printf ("  %s  %s\n", ok ? "ok  " : "FAIL", what);
        if (! ok) ++failures;
    }

    // Stands in for the XL's input port and counts the enables arriving at it.
    struct FakeXlPort : juce::MidiInputCallback
    {
        std::atomic<int> enables { 0 };

        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
        {
            if (LaunchControlXLLeds::isDawModeSwitch (m.getRawData(), m.getRawDataSize(), true))
                enables.fetch_add (1);
        }
    };

    struct Rig
    {
        T5ynthProcessor& proc;
        double rate = kSampleRate;
        int block = kBlockSize;
        juce::AudioBuffer<float> buf { 2, 2048 };
        juce::MidiBuffer midi;

        void run (int blocks = 1)
        {
            for (int b = 0; b < blocks; ++b)
            {
                juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, block);
                view.clear();
                proc.processBlock (view, midi);
                midi.clear();
            }
        }

        void send (const juce::MidiMessage& m) { midi.addEvent (m, 0); run(); }

        void runSeconds (double seconds) { run (juce::roundToInt (seconds * rate / block) + 1); }

        // 100 ms: ten times the measured delay of the real echo.
        void waitForEcho() { runSeconds (0.1); }

        void leaveEchoWindow() { runSeconds (DawModeEchoFilter::kWindowSeconds + 0.1); }

        int voicesOn (int note) const
        {
            int count = 0;
            const auto& vm = proc.getVoiceManager();
            for (int i = 0; i < 16; ++i)
                if (vm.getVoice (i).isActive() && ! vm.getVoice (i).isReleasing()
                    && vm.getVoice (i).getCurrentNote() == note)
                    ++count;
            return count;
        }

        // The note is taken away and played again, which is the only proof that the
        // arpeggiator owns it: a voice left droning also reads as on.
        bool arpReplays (int note)
        {
            bool wentSilent = false;
            for (int b = 0; b < 8000; ++b)
            {
                run();
                const bool on = voicesOn (note) > 0;
                wentSilent = wentSilent || ! on;
                if (wentSilent && on)
                    return true;
            }
            return false;
        }

        // Over several whole steps of the pattern, not at one instant of it.
        bool staysSilent (int note)
        {
            run (2000);
            for (int b = 0; b < 8000; ++b)
            {
                run();
                if (voicesOn (note) > 0)
                    return false;
            }
            return true;
        }
    };

    void setParam (T5ynthProcessor& proc, const char* id, float value)
    {
        if (auto* p = proc.getValueTreeState().getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
        else
            std::printf ("  FAIL  no parameter %s\n", id), ++failures;
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

    using Xl = LaunchControlXLLeds;
    const auto noteOn16  = [] (int note, int vel) { return juce::MidiMessage::noteOn (16, note, (juce::uint8) vel); };
    const auto noteOff16 = [] (int note) { return juce::MidiMessage::noteOff (16, note, (juce::uint8) 0); };
    const auto enable  = Xl::dawMode (true);
    const auto disable = Xl::dawMode (false);

    std::printf ("[1] what looks like a switch\n");
    const auto isSwitch = [] (const juce::MidiMessage& m, bool on)
    {
        return Xl::isDawModeSwitch (m.getRawData(), m.getRawDataSize(), on);
    };
    check (isSwitch (enable, true) && ! isSwitch (enable, false), "the enable");
    check (isSwitch (disable, false) && ! isSwitch (disable, true), "the disable");
    check (! isSwitch (noteOn16 (60, 127), true), "not another note at velocity 127");
    check (! isSwitch (noteOn16 (12, 100), true), "not note 12 at another velocity");
    check (! isSwitch (juce::MidiMessage::noteOn (15, 12, (juce::uint8) 127), true),
           "not the same note and velocity on channel 15");
    check (! isSwitch (noteOff16 (12), false), "not a note-off with status 8F");
    check (Xl::portEchoesDawMode ("LCXL3 1 DAW In"), "the DAW port echoes");
    check (! Xl::portEchoesDawMode ("LCXL3 1 MIDI In"), "the MIDI port does not");

    std::printf ("[2] what shares a block with an echo stays, in order and in place\n");
    {
        DawModeEchoFilter filter;
        juce::MidiBuffer scratch, block;
        filter.prepare (kSampleRate, scratch);
        const auto* storage = scratch.data.getRawDataPointer();
        for (int i = 0; i < 2000; ++i)
            scratch.addEvent (juce::MidiMessage::noteOn (5, 50, (juce::uint8) 64), 0);
        check (storage != nullptr && scratch.data.getRawDataPointer() == storage,
               "its copy of 2000 messages does not allocate");
        scratch.clear();
        Xl::countDawModeSwitch (true);
        const juce::uint8 payload[] = { 1, 2, 3, 4, 5 };
        block.addEvent (juce::MidiMessage::noteOn (5, 50, (juce::uint8) 64), 10);
        block.addEvent (enable, 20);
        block.addEvent (juce::MidiMessage::noteOn (6, 52, (juce::uint8) 64), 20);
        block.addEvent (juce::MidiMessage::createSysExMessage (payload, 5), 30);
        block.addEvent (juce::MidiMessage::noteOn (7, 54, (juce::uint8) 64), 127);
        filter.process (block, scratch, kBlockSize);

        std::vector<std::pair<int, int>> left;   // position, note or -1 for the SysEx
        for (const auto metadata : block)
        {
            const auto m = metadata.getMessage();
            left.emplace_back (metadata.samplePosition, m.isNoteOn() ? m.getNoteNumber() : -1);
        }
        const std::vector<std::pair<int, int>> wanted { { 10, 50 }, { 20, 52 }, { 30, -1 }, { 127, 54 } };
        check (left == wanted, "only the echo is gone");
    }

    FakeXlPort port;
    auto fake = juce::MidiInput::createNewDevice ("LCXL test port", &port);
    if (fake == nullptr) { std::printf ("FAIL  no virtual MIDI device\n"); return 1; }
    fake->start();
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.2, false);

    juce::String id;
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
        if (d.name.contains ("LCXL test port"))
            id = d.identifier;
    if (id.isEmpty()) { std::printf ("FAIL  virtual output not listed\n"); return 1; }

    auto proc = std::make_unique<T5ynthProcessor>();
    proc->prepareToPlay (kSampleRate, kBlockSize);
    Rig rig { *proc };
    rig.run (40);

    const auto selectOn = [] (T5ynthProcessor& p, const juce::String& device, FakeXlPort& to)
    {
        const int seen = to.enables.load();
        p.openMidiOutputDevice (device);
        for (int i = 0; i < 200 && to.enables.load() == seen; ++i)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false);
        return to.enables.load() > seen;
    };
    const auto select = [&] (T5ynthProcessor& p) { return selectOn (p, id, port); };
    const auto reselect = [&] (T5ynthProcessor& p)
    {
        p.closeMidiOutputDevice();
        return select (p);
    };

    std::printf ("[3] an XL output selected, DAW mode on\n");
    check (select (*proc), "the enable reached the output");
    if (port.enables.load() == 0) return 1;

    std::printf ("[4] the echo, ten times later than measured, is nothing\n");
    rig.waitForEcho();
    rig.send (enable);
    check (rig.voicesOn (12) == 0, "it started no voice");
    // Nothing may touch note 12 between the echo and here: a played note 12 would
    // erase the ghost key this looks for.
    setParam (*proc, PID::arpMode, 1.0f);
    check (rig.staysSilent (12), "and left the arpeggiator no key to play");
    setParam (*proc, PID::arpMode, 0.0f);
    rig.run (4);

    std::printf ("[5] an Osmose's rotation: a note on every member channel sounds\n");
    for (int ch = 2; ch <= 16; ++ch)
    {
        const int note = 40 + ch;
        rig.send (juce::MidiMessage::noteOn (ch, note, (juce::uint8) 64));
        char what[64];
        std::snprintf (what, sizeof (what), "channel %d note %d took a voice", ch, note);
        check (rig.voicesOn (note) == 1, what);
        rig.send (juce::MidiMessage::noteOff (ch, note, (juce::uint8) 0));
        std::snprintf (what, sizeof (what), "channel %d note %d released", ch, note);
        check (rig.voicesOn (note) == 0, what);
    }

    std::printf ("[6] note 12 on channel 16 as a played note\n");
    rig.send (noteOn16 (12, 90));
    check (rig.voicesOn (12) == 1, "it took a voice");
    rig.send (noteOff16 (12));
    check (rig.voicesOn (12) == 0, "and released");

    std::printf ("[7] keys sharing a block with the echoes play\n");
    check (reselect (*proc), "selected again");
    rig.waitForEcho();
    rig.midi.addEvent (juce::MidiMessage::noteOn (5, 50, (juce::uint8) 64), 5);
    rig.midi.addEvent (disable, 10);
    rig.midi.addEvent (enable, 11);
    rig.midi.addEvent (noteOn16 (56, 64), 20);
    rig.run();
    check (rig.voicesOn (50) == 1, "the key before them");
    check (rig.voicesOn (56) == 1, "the key after them");
    check (rig.voicesOn (12) == 0, "and the echoes started nothing");
    rig.send (juce::MidiMessage::noteOff (5, 50, (juce::uint8) 0));
    rig.send (noteOff16 (56));

    std::printf ("[8] the echo's own bytes, long after the switch, are a key\n");
    rig.leaveEchoWindow();
    rig.send (enable);
    check (rig.voicesOn (12) == 1, "9F 0C 7F took a voice");
    rig.send (disable);
    check (rig.voicesOn (12) == 0, "9F 0C 00 released it");
    proc->toggleStepRecord();
    rig.send (enable);
    proc->drainStepRecordQueue();
    check (proc->getStepRecordCursor() == 1, "9F 0C 7F reached step record");
    rig.send (disable);
    proc->toggleStepRecord();

    std::printf ("[9] the same bytes right after the one echo are a key\n");
    check (reselect (*proc), "selected again");
    rig.waitForEcho();
    rig.send (disable);
    rig.send (enable);
    check (rig.voicesOn (12) == 0, "both echoes taken, nothing sounds");
    rig.send (enable);
    check (rig.voicesOn (12) == 1, "the next 9F 0C 7F took a voice");
    rig.send (disable);
    check (rig.voicesOn (12) == 0, "the next 9F 0C 00 released it");

    std::printf ("[10] an enable excuses no disable: a key lifted as 9F 0C 00\n");
    proc->closeMidiOutputDevice();
    rig.leaveEchoWindow();
    rig.send (noteOn16 (12, 90));
    check (select (*proc), "first selection, a key held");
    rig.runSeconds (0.5);
    rig.send (disable);
    check (rig.voicesOn (12) == 0, "its key-up released it");
    rig.leaveEchoWindow();

    std::printf ("[11] selecting the XL again leaves a held key alone\n");
    rig.send (noteOn16 (12, 90));
    check (reselect (*proc), "selected again");
    rig.waitForEcho();
    rig.send (disable);
    check (rig.voicesOn (12) == 1, "the disable's echo did not lift it");
    rig.send (enable);
    check (rig.voicesOn (12) == 1, "the enable's echo did not double it");
    rig.send (noteOff16 (12));
    check (rig.voicesOn (12) == 0, "its own key-up did");
    rig.leaveEchoWindow();

    std::printf ("[12] closing the output leaves a held key alone\n");
    rig.send (noteOn16 (12, 90));
    proc->closeMidiOutputDevice();
    rig.waitForEcho();
    rig.send (disable);
    check (rig.voicesOn (12) == 1, "the disable's echo did not lift it");
    rig.send (noteOff16 (12));
    check (rig.voicesOn (12) == 0, "its own key-up did");
    rig.leaveEchoWindow();
    check (select (*proc), "selected again");
    rig.leaveEchoWindow();

    std::printf ("[13] two switches in one second owe two echoes\n");
    check (reselect (*proc), "selected");
    check (reselect (*proc), "and again");
    rig.waitForEcho();
    rig.send (disable);
    rig.send (enable);
    rig.send (disable);
    rig.send (enable);
    check (rig.voicesOn (12) == 0, "all four echoes taken");
    rig.leaveEchoWindow();

    std::printf ("[14] another instance's switch is expected here too\n");
    {
        auto other = std::make_unique<T5ynthProcessor>();
        other->prepareToPlay (kSampleRate, kBlockSize);
        Rig otherRig { *other };
        otherRig.run (40);
        rig.send (noteOn16 (12, 90));
        check (select (*other), "the other instance selected the XL");
        check (reselect (*other), "and recalled a state");
        rig.waitForEcho();
        rig.send (disable);
        check (rig.voicesOn (12) == 1, "its disable's echo did not lift a key held here");
        rig.send (enable);
        rig.send (enable);
        check (rig.voicesOn (12) == 1, "its enables' echoes did not double it");
        rig.send (noteOff16 (12));
        check (rig.voicesOn (12) == 0, "its own key-up did");
        other->closeMidiOutputDevice();
        other->releaseResources();
    }
    rig.leaveEchoWindow();
    {
        auto late = std::make_unique<T5ynthProcessor>();
        late->prepareToPlay (kSampleRate, kBlockSize);
        Rig lateRig { *late };
        lateRig.run (40);
        lateRig.send (enable);
        check (lateRig.voicesOn (12) == 1, "a new instance owes nothing for earlier switches");
        lateRig.send (disable);
        late->releaseResources();
    }

    std::printf ("[15] the arpeggiator takes a channel 16 key, and refuses the echo\n");
    setParam (*proc, PID::arpMode, 1.0f);
    rig.run (4);
    check (reselect (*proc), "selected again");
    rig.waitForEcho();
    rig.send (disable);
    rig.send (enable);
    check (rig.staysSilent (12), "the echoes armed no arp note");
    rig.send (noteOn16 (60, 90));
    check (rig.arpReplays (60), "a channel 16 key is arpeggiated");
    rig.send (noteOff16 (60));
    check (rig.staysSilent (60), "lifting it ends the pattern");
    rig.send (enable);
    check (rig.arpReplays (12), "9F 0C 7F past the window is arpeggiated");
    rig.send (disable);
    check (rig.staysSilent (12), "9F 0C 00 past the window ends it");

    std::printf ("[16] selecting the XL again with the arp on keeps a held key\n");
    rig.send (noteOn16 (12, 90));
    check (reselect (*proc), "selected again");
    rig.waitForEcho();
    rig.send (disable);
    rig.send (enable);
    check (rig.arpReplays (12), "it is still in the pattern");
    rig.send (noteOff16 (12));
    check (rig.staysSilent (12), "and its key-up ends it");

    std::printf ("[17] a channel 16 key held while the arp comes on\n");
    setParam (*proc, PID::arpMode, 0.0f);
    rig.run (4);
    rig.send (noteOn16 (60, 90));
    check (rig.voicesOn (60) == 1, "it sounds as a voice first");
    setParam (*proc, PID::arpMode, 1.0f);
    check (rig.arpReplays (60), "the arpeggiator takes it over");
    rig.send (noteOff16 (60));
    check (rig.staysSilent (60), "and lifting it ends the pattern");

    std::printf ("[18] a key held through a switch, then the arp comes on\n");
    setParam (*proc, PID::arpMode, 0.0f);
    rig.run (4);
    rig.send (noteOn16 (12, 90));
    check (reselect (*proc), "selected again");
    rig.waitForEcho();
    rig.send (disable);
    rig.send (enable);
    setParam (*proc, PID::arpMode, 1.0f);
    check (rig.arpReplays (12), "the arpeggiator takes it over");
    rig.send (noteOff16 (12));
    check (rig.staysSilent (12), "and lifting it ends the pattern");
    setParam (*proc, PID::arpMode, 0.0f);
    rig.run (4);

    std::printf ("[19] the window runs through deep idle and through bypass\n");
    rig.run (6000);
    check (reselect (*proc), "selected from deep idle");
    rig.leaveEchoWindow();
    rig.send (disable);
    rig.send (enable);
    check (rig.voicesOn (12) == 1, "the bytes play once it has passed");
    rig.send (disable);
    check (rig.voicesOn (12) == 0, "and release");
    rig.leaveEchoWindow();
    check (reselect (*proc), "selected while bypassed");
    for (int b = 0; b < 500; ++b)
    {
        juce::AudioBuffer<float> view (rig.buf.getArrayOfWritePointers(), 2, kBlockSize);
        proc->processBlockBypassed (view, rig.midi);
    }
    rig.send (enable);
    check (rig.voicesOn (12) == 1, "the bytes play once bypass has run it out");
    rig.send (disable);

    std::printf ("[20] another rate and block size\n");
    proc->prepareToPlay (44100.0, 1024);
    rig.rate = 44100.0;
    rig.block = 1024;
    rig.run (40);
    check (reselect (*proc), "selected again");
    rig.waitForEcho();
    rig.send (disable);
    rig.send (enable);
    check (rig.voicesOn (12) == 0, "the echo started no voice");
    rig.leaveEchoWindow();
    rig.send (enable);
    check (rig.voicesOn (12) == 1, "the bytes play past the window");
    rig.send (disable);
    check (rig.voicesOn (12) == 0, "and release");

    std::printf ("[21] a port that does not echo owes nothing\n");
    FakeXlPort silentPort;
    auto silent = juce::MidiInput::createNewDevice ("LCXL test MIDI In", &silentPort);
    if (silent == nullptr) { std::printf ("FAIL  no second virtual MIDI device\n"); return 1; }
    silent->start();
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.2, false);
    juce::String silentId;
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
        if (d.name.contains ("LCXL test MIDI In"))
            silentId = d.identifier;
    proc->closeMidiOutputDevice();
    rig.leaveEchoWindow();
    check (selectOn (*proc, silentId, silentPort), "selected");
    rig.send (enable);
    check (rig.voicesOn (12) == 1, "9F 0C 7F right after the enable is a key");
    proc->closeMidiOutputDevice();
    check (selectOn (*proc, silentId, silentPort), "selected again");
    rig.send (disable);
    check (rig.voicesOn (12) == 0, "9F 0C 00 right after the disable is its key-up");

    proc->closeMidiOutputDevice();
    proc->releaseResources();
    proc.reset();
    silent->stop();
    fake->stop();

    std::printf (failures == 0 ? "\nALL GREEN\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
