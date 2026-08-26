// End-to-end proof for the R0/R3 fix: does a recorded .t5evt actually carry the
// replay start-state, and does EventLogReader read it back so startReplay() would
// accept the tape? This is the exact chain that was SILENTLY BROKEN — every v2 log
// on disk had startState ABSENT because the processor's sticky-restore path never
// called setStartState().
//
// We drive the real EventLogWriterThread (self-contained: owns its FIFOs + file)
// exactly as the processor does — setStartState() then pushNote()/enqueue() — let
// it drain, then parse the resulting file with the real EventLogReader and assert:
//   1. the header round-trips a NON-EMPTY startState (the bug),
//   2. the base64 decodes back to the original bytes,
//   3. the note + generation events survive with correct fields.
//
// Build (same recipe as the other tools/*.cpp — flags.make response file +
// lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this]). Exits non-zero on any failed assertion.
#include "JuceHeader.h"
#include "eventlog/EventLog.h"
#include "eventlog/EventLogWriterThread.h"
#include "eventlog/EventLogReader.h"

#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++failures; } \
                              else { std::printf("  ok:   %s\n", msg); } } while(0)

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // MessageManager for juce::Thread

    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("t5ynth_replay_roundtrip");
    dir.deleteRecursively();
    dir.createDirectory();

    // A stand-in for getStateInformation()'s output: arbitrary binary, base64'd the
    // same way the processor does. The point is that whatever the patch is, it must
    // survive the header round-trip.
    const juce::String fakePatch = "T5YNTH-PATCH\x00\x01\x02 <the user's sound>";
    const auto startBase64 = juce::Base64::toBase64(fakePatch.toRawUTF8(),
                                                    (size_t) fakePatch.getNumBytesAsUTF8());

    juce::File writtenFile;
    {
        EventLogHeader header;
        header.t5ynthVersion = "test";
        std::vector<juce::String> paramIds { "genSeed", "cutoff" };
        EventLogWriterThread writer(dir, header, std::move(paramIds));
        writer.startThread(juce::Thread::Priority::background);

        writer.setSampleRate(48000.0);
        writer.setStartState(startBase64);   // <-- the call the processor was NOT making

        // One note, one generation — the header is stamped lazily on the first event.
        NoteEventLogEntry n;
        n.timestamp = 1234; n.source = NoteEventLogEntry::Source::StepSequencer;
        n.type = VoiceEvent::Type::NoteOn; n.note = 60; n.velocity = 0.8f;
        n.artic = VoiceEvent::Articulation::Normal; n.strandId = -1; n.pan = 0.0f; n.midiChannel = 0;
        writer.pushNote(n);

        GenerationEventLogEntry g;
        g.timestamp = 2000; g.generationId = 1; g.parentGenerationId = 0;
        g.success = true; g.realizedSeed = 424242; g.promptA = "calm waves"; g.promptB = "creek";
        g.durationSeconds = 3.0f;
        writer.enqueue(g);

        // Give the 20 ms drain loop time to open the file + write all three lines.
        juce::Thread::sleep(400);
        writtenFile = writer.getCurrentFile();
        writer.stopThread(2000);
    }

    std::printf("Written file: %s\n", writtenFile.getFullPathName().toRawUTF8());
    CHECK(writtenFile.existsAsFile(), "writer created a .t5evt file");

    // Parse it back with the real reader — this is what MainPanel::loadReplaySession does.
    EventLogReader reader;
    const bool loaded = reader.loadFile(writtenFile);
    CHECK(loaded, "EventLogReader parsed the file");

    const auto& h = reader.getHeader();
    CHECK(h.formatVersion == 2, "header formatVersion == 2");
    CHECK(h.sampleRate == 48000.0, "header sampleRate round-tripped");

    // THE bug: this was empty for every real session.
    CHECK(h.startStateBase64.isNotEmpty(), "header carries a NON-EMPTY startState");
    CHECK(h.startStateBase64 == startBase64, "startState base64 round-tripped exactly");

    // And it decodes back to the original patch bytes.
    juce::MemoryOutputStream decoded;
    const bool decodeOk = juce::Base64::convertFromBase64(decoded, h.startStateBase64);
    CHECK(decodeOk, "startState decodes from base64");
    const juce::String decodedStr(juce::CharPointer_UTF8((const char*) decoded.getData()),
                                  decoded.getDataSize());
    CHECK(decodedStr == fakePatch, "decoded bytes equal the original patch");

    CHECK(reader.getNoteEvents().size() == 1, "one note event parsed");
    if (! reader.getNoteEvents().empty())
    {
        const auto& rn = reader.getNoteEvents()[0];
        CHECK(rn.note == 60 && rn.timestamp == 1234, "note fields intact (note 60 @ 1234)");
    }
    CHECK(reader.getGenerationEvents().size() == 1, "one generation event parsed");
    if (! reader.getGenerationEvents().empty())
    {
        const auto& rg = reader.getGenerationEvents()[0];
        CHECK(rg.realizedSeed == 424242 && rg.promptA == "calm waves",
              "generation fields intact (seed + promptA)");
    }

    dir.deleteRecursively();
    std::printf("\n%s (%d failure(s))\n", failures == 0 ? "ALL PASS" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
