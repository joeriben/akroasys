// What does the controller actually send, and which branch of processBlock catches it?
//
// Written for one question: on an Expressive E Osmose, PolyAT mode drives this
// synth's aftertouch correctly and MPE mode does not. Four candidate causes are
// provable in the code without hardware, but three further ones are not -- they
// depend on bytes only the device can answer for:
//
//   * does MPE mode send pressure as Channel Pressure on MEMBER channels (the
//     branch at PluginProcessor.cpp:5067), and does it ALSO send Poly Key
//     Pressure (:5061)? Both feed the same jmax in VoiceManager::pressureForVoice,
//     so a device that sends both has two pressures competing per note.
//   * does anything arrive on channel 1? Channel-1 pressure is zone-wide here
//     and becomes a floor under every voice.
//   * where does CC74 REST? This synth centres MPE timbre on 64/127 and maps the
//     deviation to +-4 octaves of cutoff (SynthVoice.cpp:993). A device whose Y
//     axis rests at 0 would sit four octaves dark on every note.
//
// Deliberately NOT built on juce::MidiInput or on T5ynthProcessor: the point is
// the raw wire. CoreMIDI hands over the bytes the device sent, running status
// and all, before any library has normalised them. It also keeps the build to a
// single translation unit and two frameworks, so this runs in seconds without
// the plugin's static library.
//
// The classification printed next to each message is this file's own reading of
// PluginProcessor.cpp's MIDI walk, with the line numbers named at each site. It
// is a transcription and can go stale -- when it disagrees with the processor,
// the processor is right.
//
// Build:
//   clang++ -std=c++17 -O2 -Wno-deprecated-declarations tools/midi_monitor.cpp \
//     -framework CoreMIDI -framework CoreFoundation -o /tmp/t5main/midi_monitor
//
// Use:
//   midi_monitor                 -- list the sources and exit
//   midi_monitor Osmose          -- connect to every source matching "Osmose"
//   midi_monitor Osmose --quiet  -- summary only, no per-message lines
//   midi_monitor Osmose --seconds 60
//
// Ctrl-C prints the summary and exits.

#include <CoreMIDI/CoreMIDI.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <string>
#include <vector>

namespace
{

// ── What the synth would do with this message ────────────────────────────────
// The lower zone is declared at construction with 15 members, master ch1,
// members ch2-16 (PluginProcessor.h:1057), and nothing in a plain session
// changes it. isMpeMasterChannel then answers "channel 1 only"
// (PluginProcessor.h:1112) -- an upper zone would add channel 16, but declaring
// one takes an MPE Configuration Message this monitor would report separately.
bool isMasterChannel (int channel) { return channel == 1; }

std::string classify (int status, int channel, int d1, int d2)
{
    const int type = status & 0xF0;
    const bool master = isMasterChannel (channel);

    switch (type)
    {
        case 0x90:
            if (d2 == 0)
                return "note off (velocity 0)  -> VoiceManager::noteOff, channel IGNORED (VoiceManager.cpp:350)";
            return std::string ("note on   -> voice tagged with channel ")
                 + std::to_string (channel) + "  (PluginProcessor.cpp:5024)";
        case 0x80:
            return "note off  -> VoiceManager::noteOff, channel IGNORED (VoiceManager.cpp:350)";

        case 0xA0:
            return "POLY KEY PRESSURE -> setPolyPressure, matched by NOTE NUMBER, "
                   "writes the global polyPressureByNote latch (PluginProcessor.cpp:5061)";

        case 0xD0:
            return master
                 ? "CHANNEL PRESSURE (master) -> ZONE-WIDE, a floor under every voice "
                   "(PluginProcessor.cpp:5075)"
                 : std::string ("channel pressure (member) -> MPE Z, per-note, voices tagged ch ")
                   + std::to_string (channel) + " (PluginProcessor.cpp:5077)";

        case 0xE0:
            return master
                 ? "pitch wheel (ch1) -> GLOBAL bend, master range (PluginProcessor.cpp:5095)"
                 : "pitch wheel (member) -> MPE X, per-note bend (PluginProcessor.cpp:5101)";

        case 0xB0:
            switch (d1)
            {
                case 1:  return "CC1  modwheel -> GLOBAL pressure source on ANY channel, "
                                "folded into the same jmax as MPE Z (PluginProcessor.cpp:5301)";
                case 2:  return "CC2  breath   -> GLOBAL pressure source on ANY channel, "
                                "folded into the same jmax as MPE Z (PluginProcessor.cpp:5305)";
                case 7:  return "CC7  channel volume -> GLOBAL output gain, any channel (PluginProcessor.cpp:5309)";
                case 11: return "CC11 expression     -> GLOBAL output gain, any channel (PluginProcessor.cpp:5313)";
                case 64: return "CC64 sustain pedal";
                case 66: return "CC66 sostenuto";
                case 74: return master
                              ? "CC74 on MASTER -> NOT timbre; stays the control-surface default (osc_scan)"
                              : "CC74 (member)  -> MPE Y / timbre, neutral is 64 (PluginProcessor.cpp:5169)";
                case 6: case 38: case 98: case 99: case 100: case 101:
                    return "RPN/NRPN byte -> juce::MPEZoneLayout (PluginProcessor.cpp:5140)";
                case 120: case 123: return "all notes off / all sound off -> panic";
                case 121: return "reset all controllers";
                default: return "CC -> user binding if bound, else unhandled";
            }

        case 0xC0: return "program change";
        default:   return "";
    }
}

const char* typeName (int status)
{
    switch (status & 0xF0)
    {
        case 0x80: return "NoteOff";
        case 0x90: return "NoteOn";
        case 0xA0: return "PolyAT";
        case 0xB0: return "CC";
        case 0xC0: return "PgmChange";
        case 0xD0: return "ChanPress";
        case 0xE0: return "PitchBend";
        default:   return "?";
    }
}

// ── Tally ────────────────────────────────────────────────────────────────────
struct CcStats
{
    long count = 0;
    int  first = -1, last = -1, lo = 128, hi = -1;
};

struct Tally
{
    long perChannelPerType[17][8] {};   // [channel 1-16][type index 0-7]
    long ccPerChannel[17][128] {};
    CcStats cc[128];
    long total = 0;
    int  pitchBendLo = 16384, pitchBendHi = -1;
    int  chanPressLo = 128,   chanPressHi = -1;
    int  polyAtLo    = 128,   polyAtHi    = -1;

    // RPN 0 (pitch bend sensitivity), reassembled the way PluginProcessor does:
    // CC101/CC100 select the parameter, CC6 writes it. Without this the capture
    // cannot say what range the device THINKS the receiver is using, and every
    // statement in semitones about its bend or its lateral travel is a guess.
    int  rpnMsb[17], rpnLsb[17];        // parameter select, per channel
    long bendRangeWrites = 0;
    int  bendRangeFirst = -1, bendRangeLast = -1;

    Tally()
    {
        for (int c = 0; c < 17; ++c) { rpnMsb[c] = -1; rpnLsb[c] = -1; }
    }

    static int typeIndex (int status) { return ((status & 0xF0) >> 4) - 8; }  // 0x80 -> 0
};

Tally gTally;
std::atomic<bool> gStop { false };
bool gQuiet = false;
CFAbsoluteTime gStart = 0.0;

void account (int status, int channel, int d1, int d2)
{
    ++gTally.total;
    const int ti = Tally::typeIndex (status);
    if (channel >= 1 && channel <= 16 && ti >= 0 && ti < 8)
        ++gTally.perChannelPerType[channel][ti];

    switch (status & 0xF0)
    {
        case 0xB0:
            if (channel >= 1 && channel <= 16) ++gTally.ccPerChannel[channel][d1];
            if (channel >= 1 && channel <= 16)
            {
                if      (d1 == 101) gTally.rpnMsb[channel] = d2;
                else if (d1 == 100) gTally.rpnLsb[channel] = d2;
                else if (d1 == 6 && gTally.rpnMsb[channel] == 0
                                 && gTally.rpnLsb[channel] == 0)
                {
                    ++gTally.bendRangeWrites;
                    if (gTally.bendRangeFirst < 0) gTally.bendRangeFirst = d2;
                    gTally.bendRangeLast = d2;
                }
            }
            {
                auto& s = gTally.cc[d1];
                ++s.count;
                if (s.first < 0) s.first = d2;
                s.last = d2;
                if (d2 < s.lo) s.lo = d2;
                if (d2 > s.hi) s.hi = d2;
            }
            break;
        case 0xD0:
            if (d1 < gTally.chanPressLo) gTally.chanPressLo = d1;
            if (d1 > gTally.chanPressHi) gTally.chanPressHi = d1;
            break;
        case 0xA0:
            if (d2 < gTally.polyAtLo) gTally.polyAtLo = d2;
            if (d2 > gTally.polyAtHi) gTally.polyAtHi = d2;
            break;
        case 0xE0:
        {
            const int v14 = (d2 << 7) | d1;
            if (v14 < gTally.pitchBendLo) gTally.pitchBendLo = v14;
            if (v14 > gTally.pitchBendHi) gTally.pitchBendHi = v14;
            break;
        }
        default: break;
    }
}

void report (const char* source, int status, int channel, int d1, int d2)
{
    account (status, channel, d1, d2);
    if (gQuiet)
        return;

    const double t = CFAbsoluteTimeGetCurrent() - gStart;
    const int type = status & 0xF0;

    char data[64];
    if (type == 0xD0 || type == 0xC0)
        std::snprintf (data, sizeof data, "%3d", d1);
    else if (type == 0xE0)
        std::snprintf (data, sizeof data, "%5d", (d2 << 7) | d1);
    else
        std::snprintf (data, sizeof data, "%3d %3d", d1, d2);

    // Device names run long and contain spaces ("Osmose 61 osmose play"), which
    // would break every column to the right of them -- and this log is read with
    // grep and awk, not by eye. Clipped to a fixed width.
    char src[13];
    std::snprintf (src, sizeof src, "%-12.12s", source);

    std::printf ("%8.3f  %s  ch%-3d %-9s %-9s  %s\n",
                 t, src, channel, typeName (status), data,
                 classify (status, channel, d1, d2).c_str());
    std::fflush (stdout);
}

// ── CoreMIDI ─────────────────────────────────────────────────────────────────
// One parser state per connected source: MIDI running status means a packet can
// carry data bytes with no status byte of their own, and the Osmose's continuous
// streams (pressure, bend, CC74) are exactly where a device uses it.
struct SourceState
{
    std::string name;
    int  runningStatus = 0;
    int  pending = 0;          // data bytes still expected
    int  d1 = 0;
    bool haveD1 = false;
};

std::vector<SourceState*> gSources;

int dataBytesFor (int status)
{
    switch (status & 0xF0)
    {
        case 0xC0: case 0xD0: return 1;
        case 0xF0: return 0;               // system messages: not this tool's business
        default:   return 2;
    }
}

void feed (SourceState& st, const Byte* bytes, int length)
{
    for (int i = 0; i < length; ++i)
    {
        const int b = bytes[i];

        if (b >= 0xF8)                     // real-time, interleaves anywhere
            continue;

        if (b >= 0x80)                     // status byte
        {
            if (b >= 0xF0)                 // system common clears running status
            {
                st.runningStatus = 0;
                st.pending = 0;
                st.haveD1 = false;
                continue;
            }
            st.runningStatus = b;
            st.pending = dataBytesFor (b);
            st.haveD1 = false;
            continue;
        }

        if (st.runningStatus == 0)         // data byte with no status yet
            continue;

        const int channel = (st.runningStatus & 0x0F) + 1;

        if (st.pending == 1)
        {
            report (st.name.c_str(), st.runningStatus, channel, b, 0);
            continue;                      // running status stays armed
        }

        if (! st.haveD1)
        {
            st.d1 = b;
            st.haveD1 = true;
        }
        else
        {
            report (st.name.c_str(), st.runningStatus, channel, st.d1, b);
            st.haveD1 = false;
        }
    }
}

std::string cfToStd (CFStringRef s)
{
    if (s == nullptr)
        return {};
    char buf[256] = {};
    CFStringGetCString (s, buf, sizeof buf, kCFStringEncodingUTF8);
    return buf;
}

std::string endpointName (MIDIEndpointRef ep)
{
    CFStringRef name = nullptr;
    if (MIDIObjectGetStringProperty (ep, kMIDIPropertyDisplayName, &name) != noErr || name == nullptr)
        MIDIObjectGetStringProperty (ep, kMIDIPropertyName, &name);
    std::string out = cfToStd (name);
    if (name != nullptr) CFRelease (name);
    return out;
}

void onSignal (int) { gStop.store (true); CFRunLoopStop (CFRunLoopGetMain()); }

// ── Summary ──────────────────────────────────────────────────────────────────
void printSummary()
{
    const double secs = CFAbsoluteTimeGetCurrent() - gStart;
    std::printf ("\n──────────────────────────────────────────────────────────────────────\n");
    std::printf ("%ld messages in %.1f s\n\n", gTally.total, secs);

    std::printf ("Per channel (blank = nothing):\n");
    std::printf ("  ch   NoteOn  NoteOff   PolyAT       CC  ChanPress  PitchBend\n");
    for (int ch = 1; ch <= 16; ++ch)
    {
        const long* r = gTally.perChannelPerType[ch];
        const long on = r[1], off = r[0], pat = r[2], cc = r[3], cp = r[5], pb = r[6];
        if ((on | off | pat | cc | cp | pb) == 0)
            continue;
        std::printf ("  %2d %s %8ld %8ld %8ld %8ld %10ld %10ld\n",
                     ch, isMasterChannel (ch) ? "M" : " ", on, off, pat, cc, cp, pb);
    }

    std::printf ("\nCC numbers seen (first / last / min / max):\n");
    for (int c = 0; c < 128; ++c)
    {
        const auto& s = gTally.cc[c];
        if (s.count == 0)
            continue;
        std::printf ("  CC%-4d %8ld   first %3d  last %3d  min %3d  max %3d",
                     c, s.count, s.first, s.last, s.lo, s.hi);
        if (c == 74)
            std::printf ("   <- MPE Y. This synth's neutral is 64.");
        if (c == 1 || c == 2)
            std::printf ("   <- GLOBAL pressure source in this synth.");
        std::printf ("\n");
    }

    std::printf ("\nRanges: channel pressure %d..%d   poly AT %d..%d   pitch bend %d..%d (8192 = centre)\n",
                 gTally.chanPressHi < 0 ? 0 : gTally.chanPressLo, gTally.chanPressHi < 0 ? 0 : gTally.chanPressHi,
                 gTally.polyAtHi    < 0 ? 0 : gTally.polyAtLo,    gTally.polyAtHi    < 0 ? 0 : gTally.polyAtHi,
                 gTally.pitchBendHi < 0 ? 0 : gTally.pitchBendLo, gTally.pitchBendHi < 0 ? 0 : gTally.pitchBendHi);

    // The four questions this run was started to answer.
    long masterPress = gTally.perChannelPerType[1][5];
    long memberPress = 0;
    for (int ch = 2; ch <= 16; ++ch) memberPress += gTally.perChannelPerType[ch][5];
    long polyAt = 0;
    for (int ch = 1; ch <= 16; ++ch) polyAt += gTally.perChannelPerType[ch][2];
    long memberCc1 = 0, memberCc2 = 0;
    for (int ch = 2; ch <= 16; ++ch) { memberCc1 += gTally.ccPerChannel[ch][1]; memberCc2 += gTally.ccPerChannel[ch][2]; }

    std::printf ("\nWhat this answers:\n");
    std::printf ("  pressure as MEMBER channel pressure (MPE Z) : %s (%ld)\n",
                 memberPress > 0 ? "YES" : "no", memberPress);
    std::printf ("  pressure ALSO zone-wide on channel 1        : %s (%ld)%s\n",
                 masterPress > 0 ? "YES" : "no", masterPress,
                 masterPress > 0 ? "   <- becomes a floor under every voice" : "");
    std::printf ("  poly key pressure present                   : %s (%ld)%s\n",
                 polyAt > 0 ? "YES" : "no", polyAt,
                 (polyAt > 0 && memberPress > 0) ? "   <- two pressures per note, composed by jmax" : "");
    std::printf ("  CC1 / CC2 on member channels                : %s (%ld / %ld)%s\n",
                 (memberCc1 + memberCc2) > 0 ? "YES" : "no", memberCc1, memberCc2,
                 (memberCc1 + memberCc2) > 0 ? "   <- global pressure, pins every voice" : "");
    const auto& y = gTally.cc[74];
    if (y.count > 0)
        std::printf ("  CC74 rest position                          : first %d, last %d, min %d%s\n",
                     y.first, y.last, y.lo,
                     (y.first < 32) ? "   <- rests near 0, not 64: every note sits dark here" : "");
    else
        std::printf ("  CC74                                        : never sent\n");
    if (gTally.bendRangeWrites > 0)
        std::printf ("  bend range the device TRANSMITS (RPN 0)     : %d semitones"
                     " (%ld writes, last %d)\n",
                     gTally.bendRangeFirst, gTally.bendRangeWrites, gTally.bendRangeLast);
    else
        std::printf ("  bend range the device TRANSMITS (RPN 0)     : NONE"
                     "   <- the synth's own default is in force, so every\n"
                     "                                                 statement in"
                     " semitones about this capture rests on it\n");
    std::printf ("──────────────────────────────────────────────────────────────────────\n");
}

} // namespace

int main (int argc, char** argv)
{
    std::string match;
    double seconds = 0.0;   // 0 = until Ctrl-C

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--quiet")        gQuiet = true;
        else if (a == "--seconds" && i + 1 < argc) seconds = std::atof (argv[++i]);
        else if (a.rfind ("--", 0) != 0) match = a;
    }

    const ItemCount n = MIDIGetNumberOfSources();

    if (match.empty())
    {
        std::printf ("MIDI sources:\n");
        for (ItemCount i = 0; i < n; ++i)
            std::printf ("  %s\n", endpointName (MIDIGetSource (i)).c_str());
        std::printf ("\nPass a name (or part of one) to connect, e.g.:  %s Osmose\n", argv[0]);
        return 0;
    }

    MIDIClientRef client = 0;
    if (MIDIClientCreate (CFSTR ("t5ynth midi_monitor"), nullptr, nullptr, &client) != noErr)
    {
        std::fprintf (stderr, "MIDIClientCreate failed\n");
        return 1;
    }

    MIDIPortRef port = 0;
    const OSStatus portErr = MIDIInputPortCreateWithBlock (
        client, CFSTR ("in"), &port,
        ^(const MIDIPacketList* list, void* srcConnRefCon)
        {
            auto* st = static_cast<SourceState*> (srcConnRefCon);
            const MIDIPacket* p = &list->packet[0];
            for (UInt32 i = 0; i < list->numPackets; ++i)
            {
                feed (*st, p->data, (int) p->length);
                p = MIDIPacketNext (p);
            }
        });

    if (portErr != noErr)
    {
        std::fprintf (stderr, "MIDIInputPortCreateWithBlock failed (%d)\n", (int) portErr);
        return 1;
    }

    for (ItemCount i = 0; i < n; ++i)
    {
        MIDIEndpointRef ep = MIDIGetSource (i);
        const std::string name = endpointName (ep);
        if (name.find (match) == std::string::npos)
            continue;
        auto* st = new SourceState { name, 0, 0, 0, false };
        gSources.push_back (st);
        if (MIDIPortConnectSource (port, ep, st) == noErr)
            std::printf ("connected: %s\n", name.c_str());
    }

    if (gSources.empty())
    {
        std::fprintf (stderr, "no MIDI source matching \"%s\" -- run with no arguments to list them\n",
                      match.c_str());
        return 1;
    }

    std::signal (SIGINT, onSignal);
    gStart = CFAbsoluteTimeGetCurrent();

    std::printf ("\nlistening%s -- Ctrl-C for the summary\n\n",
                 seconds > 0.0 ? (" for " + std::to_string ((int) seconds) + " s").c_str() : "");
    if (! gQuiet)
        std::printf ("    time  source        ch    type      data       what this synth does with it\n");

    if (seconds > 0.0)
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, seconds, false);
    else
        CFRunLoopRun();

    printSummary();
    return 0;
}
