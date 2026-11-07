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
// BJ, 2026-11-07, on the same open output: "das Haltepedal etc. muessen natuerlich
// funktionieren". The XL's binding layer answers on every channel, and it held
// inf_steps on CC 64: the damper did not hold, and pressing it moved a parameter.
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
#include <thread>
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
    auto fake = juce::MidiInput::createNewDevice ("LCXL test DAW In", &port);
    if (fake == nullptr) { std::printf ("FAIL  no virtual MIDI device\n"); return 1; }
    fake->start();
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.2, false);

    juce::String id;
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
        if (d.name.contains ("LCXL test DAW In"))
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

    const auto value = [&] (const char* paramId)
    {
        auto* p = proc->getValueTreeState().getParameter (paramId);
        return p != nullptr ? p->getValue() : -1.0f;
    };
    const auto cc = [] (int channel, int number, int v)
    {
        return juce::MidiMessage::controllerEvent (channel, number, v);
    };
    const auto gain = [&] { return proc->getVoiceManager().performanceOutputGain(); };
    const auto allParams = [&]
    {
        std::vector<float> values;
        for (auto* p : proc->getParameters())
            values.push_back (p->getValue());
        return values;
    };
    const char* const movedOffThePedals[] = { "inf_steps", "gen_cfg", "gen_start", "gen_axes_amount", "seq_bpm" };

    const auto pedals = [&] (const char* portName)
    {
        std::printf ("[22] the pedals with %s selected\n", portName);
        for (const int channel : { 1, 2, 7, 9, 16 })
        {
            const int keyChannel = channel == 1 ? 2 : channel;
            char what[80];
            const auto before = allParams();

            rig.send (cc (channel, 64, 127));
            rig.send (juce::MidiMessage::noteOn (keyChannel, 60, (juce::uint8) 90));
            rig.send (juce::MidiMessage::noteOff (keyChannel, 60, (juce::uint8) 0));
            std::snprintf (what, sizeof (what), "the damper on channel %d holds a lifted key", channel);
            check (rig.voicesOn (60) == 1, what);
            rig.send (cc (channel, 64, 0));
            std::snprintf (what, sizeof (what), "and lets it go on channel %d", channel);
            check (rig.voicesOn (60) == 0, what);

            rig.send (juce::MidiMessage::noteOn (keyChannel, 60, (juce::uint8) 90));
            rig.send (cc (channel, 66, 127));
            rig.send (juce::MidiMessage::noteOff (keyChannel, 60, (juce::uint8) 0));
            std::snprintf (what, sizeof (what), "sostenuto on channel %d holds a lifted key", channel);
            check (rig.voicesOn (60) == 1, what);
            rig.send (cc (channel, 66, 0));
            check (rig.voicesOn (60) == 0, "and lets it go");

            const float full = gain();
            rig.send (cc (channel, 67, 127));
            std::snprintf (what, sizeof (what), "the soft pedal on channel %d softens", channel);
            check (gain() < full, what);
            rig.send (cc (channel, 67, 0));
            check (gain() == full, "and stops");

            for (const int pedal : { 65, 67, 68, 69 })
            {
                rig.send (cc (channel, pedal, 127));
                rig.send (cc (channel, pedal, 0));
            }
            std::snprintf (what, sizeof (what), "no pedal on channel %d moved a parameter", channel);
            check (allParams() == before, what);
        }
    };

    std::printf ("[23] what left the pedals is still reachable\n");
    {
        const std::pair<int, const char*> moved[] = {
            { 59, "inf_steps" }, { 60, "gen_cfg" }, { 76, "gen_start" },
            { 105, "gen_axes_amount" }, { 108, "seq_bpm" } };
        check (reselect (*proc), "the DAW port selected");
        rig.leaveEchoWindow();
        for (const auto& [number, paramId] : moved)
        {
            rig.send (cc (1, number, 20));
            const float low = value (paramId);
            rig.send (cc (1, number, 100));
            char what[80];
            std::snprintf (what, sizeof (what), "CC %d drives %s", number, paramId);
            check (value (paramId) > low, what);
        }

        const auto before = allParams();
        for (const int button : { 65, 66, 102, 103, 104, 106, 107 })
        {
            rig.send (cc (1, button, 127));
            rig.send (cc (1, button, 0));
        }
        check (allParams() == before, "a tap on the XL's own buttons writes no parameter");
    }
    pedals ("the DAW port");

    std::printf ("[24] CC 7 and CC 11 with the DAW port: the faders are on channel 16\n");
    for (const auto& [number, paramId] : { std::pair<int, const char*> { 7, "filter_cutoff" },
                                           std::pair<int, const char*> { 11, "reverb_mix" } })
    {
        char what[80];
        rig.send (cc (16, number, 20));
        const float low = value (paramId);
        rig.send (cc (16, number, 100));
        std::snprintf (what, sizeof (what), "CC %d on channel 16 is the fader", number);
        check (value (paramId) > low, what);
        check (gain() == 1.0f, "and no volume");
        const float faderPosition = value (paramId);
        for (const int channel : { 1, 9 })
        {
            rig.send (cc (channel, number, 20));
            std::snprintf (what, sizeof (what), "CC %d on channel %d leaves it alone", number, channel);
            check (value (paramId) == faderPosition, what);
            std::snprintf (what, sizeof (what), "CC %d on channel %d turns the instrument down", number, channel);
            check (gain() < 0.2f, what);
            rig.send (cc (channel, number, 127));
            check (gain() == 1.0f, "and back up");
        }
    }

    std::printf ("[25] on channel 7 the XL is talking about itself\n");
    {
        const auto before = allParams();
        rig.send (cc (7, 30, 1));
        rig.send (cc (7, 30, 2));
        for (const int report : { 63, 69, 70, 71, 72, 73 })
        {
            rig.send (cc (7, report, 127));
            rig.send (cc (7, report, 0));
        }
        rig.send (cc (7, 30, 6));
        rig.send (cc (7, 30, 2));
        check (allParams() == before, "no report writes a parameter");

        rig.send (cc (7, 74, 90));
        check (allParams() == before, "an MPE slide on channel 7 is no report and no knob");

        rig.send (cc (7, 30, 0));
        const float cutoff = value ("filter_cutoff");
        rig.send (cc (1, 7, 20));
        check (value ("filter_cutoff") == cutoff, "a value that is no mode changes nothing");
        rig.send (cc (1, 7, 127));

        const char* const row3Knob = LaunchControlXLLeds::paramIdForCc (30);
        rig.send (cc (16, 30, 20));
        const float low = value (row3Knob);
        rig.send (cc (16, 30, 100));
        check (value (row3Knob) > low, "CC 30 on channel 16 is still the knob");

        rig.send (cc (1, 63, 20));
        const float lowDuration = value ("gen_duration");
        rig.send (cc (1, 63, 100));
        check (value ("gen_duration") > lowDuration, "CC 63 from another device still drives gen_duration");
    }

    std::printf ("[26] a Custom Mode takes the faders off channel 16\n");
    for (const int customMode : { 6, 29 })
    {
        rig.send (cc (7, 30, customMode));
        for (const auto& [number, paramId] : { std::pair<int, const char*> { 7, "filter_cutoff" },
                                               std::pair<int, const char*> { 11, "reverb_mix" } })
        {
            char what[80];
            rig.send (cc (1, number, 20));
            const float low = value (paramId);
            rig.send (cc (1, number, 100));
            std::snprintf (what, sizeof (what), "mode %d: CC %d on channel 1 is the fader", customMode, number);
            check (value (paramId) > low, what);
            check (gain() == 1.0f, "and no volume");
            const float faderPosition = value (paramId);
            rig.send (cc (9, number, 20));
            check (value (paramId) == faderPosition && gain() < 0.2f, "channel 9 is still the player's");
            rig.send (cc (9, number, 127));
        }
        rig.send (cc (7, 30, 2));
        const float cutoff = value ("filter_cutoff");
        rig.send (cc (1, 7, 20));
        check (value ("filter_cutoff") == cutoff && gain() < 0.2f, "back on a DAW surface it is a volume again");
        rig.send (cc (1, 7, 127));
    }

    std::printf ("[27] selecting the output takes back what a stray fader left\n");
    rig.send (cc (1, 7, 20));
    rig.send (cc (1, 11, 20));
    check (gain() < 0.1f, "turned down");
    rig.send (cc (7, 30, 6));
    check (reselect (*proc), "selected again");
    rig.leaveEchoWindow();
    check (gain() == 1.0f, "full level");
    {
        const float cutoff = value ("filter_cutoff");
        rig.send (cc (1, 7, 20));
        check (value ("filter_cutoff") == cutoff, "and the Custom Mode is forgotten with the old selection");
        rig.send (cc (1, 7, 127));
    }

    std::printf ("[27a] a restored session leaves a player's volume where it is\n");
    {
        rig.send (cc (9, 7, 40));
        const float held = gain();
        juce::MemoryBlock state;
        proc->getStateInformation (state);
        const int seen = port.enables.load();
        proc->setStateInformation (state.getData(), (int) state.getSize());
        for (int i = 0; i < 200 && port.enables.load() == seen; ++i)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false);
        check (port.enables.load() > seen, "the restore selected the output again");
        rig.leaveEchoWindow();
        check (gain() == held, "still there");
        rig.send (cc (9, 7, 127));
    }

    std::printf ("[27b] a fader that loses the map to the message thread is no volume\n");
    {
        std::atomic<bool> stop { false };
        std::thread gui ([&] { while (! stop.load()) (void) proc->getCcMappingCopy (7); });
        for (int i = 0; i < 400; ++i)
        {
            rig.send (cc (16, 7, 20));
            rig.send (cc (16, 11, 20));
        }
        check (gain() == 1.0f, "full level");
        int lost = 0;
        for (int i = 0; i < 400; ++i)
        {
            const int volume = (i % 2 == 0) ? 40 : 127;
            rig.send (cc (9, 7, volume));
            if (gain() != static_cast<float> (volume) / 127.0f)
                ++lost;
        }
        stop.store (true);
        gui.join();
        check (lost == 0, "a player's volume gets through all the same");
    }

    std::printf ("[27c] an output closed before its bindings were applied stays closed\n");
    {
        proc->closeMidiOutputDevice();
        rig.leaveEchoWindow();
        const int seen = port.enables.load();
        proc->openMidiOutputDevice (id);
        proc->closeMidiOutputDevice();
        for (int i = 0; i < 30; ++i)
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false);
        check (port.enables.load() == seen, "no enable went out");
        const float cutoff = value ("filter_cutoff");
        rig.send (cc (1, 7, 40));
        check (value ("filter_cutoff") == cutoff, "and no fader binding came back");
        rig.send (cc (1, 7, 127));
    }

    rig.leaveEchoWindow();
    check (selectOn (*proc, silentId, silentPort), "the MIDI port selected");
    pedals ("the MIDI port");

    std::printf ("[28] CC 7 and CC 11 with the MIDI port: the faders are on channel 1\n");
    for (const auto& [number, paramId] : { std::pair<int, const char*> { 7, "filter_cutoff" },
                                           std::pair<int, const char*> { 11, "reverb_mix" } })
    {
        char what[80];
        rig.send (cc (1, number, 20));
        const float low = value (paramId);
        rig.send (cc (1, number, 100));
        std::snprintf (what, sizeof (what), "CC %d on channel 1 is the fader", number);
        check (value (paramId) > low, what);
        check (gain() == 1.0f, "and no volume");

        rig.send (cc (16, number, 20));
        const float low16 = value (paramId);
        rig.send (cc (16, number, 100));
        std::snprintf (what, sizeof (what), "CC %d on channel 16 is the fader too", number);
        check (value (paramId) > low16 && gain() == 1.0f, what);

        const float faderPosition = value (paramId);
        for (const int channel : { 2, 9, 15 })
        {
            rig.send (cc (channel, number, 20));
            std::snprintf (what, sizeof (what), "CC %d on channel %d is the player's", number, channel);
            check (value (paramId) == faderPosition && gain() < 0.2f, what);
            rig.send (cc (channel, number, 127));
        }
    }
    rig.send (cc (7, 63, 20));
    {
        const float low = value ("gen_duration");
        rig.send (cc (7, 63, 100));
        check (value ("gen_duration") > low, "channel 7 is an ordinary channel here");
    }

    std::printf ("[29] with no XL selected the pedals and controllers are the player's\n");
    proc->closeMidiOutputDevice();
    {
        const float cutoff = value ("filter_cutoff");
        rig.send (cc (1, 7, 30));
        check (value ("filter_cutoff") == cutoff, "CC 7 moves no fader binding");
        check (gain() < 0.3f, "CC 7 is the volume");
        rig.send (cc (1, 7, 127));
        rig.send (cc (1, 11, 30));
        check (gain() < 0.3f, "CC 11 is the expression");
        rig.send (cc (1, 11, 127));
    }
    pedals ("no XL output");

    proc->closeMidiOutputDevice();
    proc->releaseResources();
    proc.reset();
    silent->stop();
    fake->stop();

    std::printf (failures == 0 ? "\nALL GREEN\n" : "\n%d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
