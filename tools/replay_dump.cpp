// replay_dump.cpp — Offline validation tool for .t5evt session logs.
//
// Parses a .t5evt file using EventLogReader and prints a human-readable
// timeline of all events. Used to validate the parser on real data before
// any engine wiring (R1 verification step).
//
// Build (ad-hoc, not in CMake):
//   cd /Users/joerissen/ai/t5ynth
//   clang++ -std=c++20 -I JUCE/modules -I src \
//       -DJUCE_STANDALONE_APPLICATION=1 \
//       tools/replay_dump.cpp \
//       src/eventlog/EventLog.cpp \
//       src/eventlog/EventLogReader.cpp \
//       -framework Foundation -framework CoreFoundation \
//       -o /tmp/replay_dump
//
// Usage:
//   /tmp/replay_dump ~/Library/T5ynth/eventlogs/session_20260707_*.t5evt

#include "JuceHeader.h"
#include "eventlog/EventLogReader.h"
#include <cstdio>

static const char* sourceStr(NoteEventLogEntry::Source s)
{
    switch (s)
    {
        case NoteEventLogEntry::Source::StepSequencer:        return "StepSeq";
        case NoteEventLogEntry::Source::GenerativeSequencer:  return "GenSeq";
        case NoteEventLogEntry::Source::PhraseSequencer:      return "PhraseSeq";
        case NoteEventLogEntry::Source::Arpeggiator:          return "Arp";
        case NoteEventLogEntry::Source::ExternalMidi:         return "MIDI";
    }
    return "?";
}

static const char* typeStr(VoiceEvent::Type t)
{
    return t == VoiceEvent::Type::NoteOn ? "ON " : "OFF";
}

static const char* originStr(ParamOrigin o)
{
    switch (o)
    {
        case ParamOrigin::HostAutomation: return "host";
        case ParamOrigin::UserEdit:       return "user";
        case ParamOrigin::MidiCCLearn:    return "cc";
    }
    return "?";
}

static void printTimestamp(uint64_t samples, double sampleRate)
{
    const double seconds = static_cast<double>(samples) / sampleRate;
    const int mins = static_cast<int>(seconds) / 60;
    const double secs = seconds - mins * 60.0;
    printf("%02d:%06.3f", mins, secs);
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <session.t5evt>\n", argv[0]);
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI juceInit;

    const juce::File file(argv[1]);
    EventLogReader reader;

    if (! reader.loadFile(file))
    {
        fprintf(stderr, "Failed to load: %s\n", reader.getErrorMessage().toRawUTF8());
        return 1;
    }

    const auto& header = reader.getHeader();
    printf("=== Session Log: %s ===\n", file.getFileName().toRawUTF8());
    printf("Format version: %d\n", header.formatVersion);
    printf("T5ynth version: %s\n", header.t5ynthVersion.toRawUTF8());
    printf("Sample rate:    %.0f Hz\n", header.sampleRate);
    printf("Start state:    %s\n", header.startStateBase64.isNotEmpty() ? "present" : "absent");
    printf("\n");

    // Merge all events into a single timeline for display
    struct TimelineEntry
    {
        uint64_t timestamp;
        int      kind;  // 0=note, 1=param, 2=generation, 3=preset
        size_t   index;
    };

    std::vector<TimelineEntry> timeline;
    timeline.reserve(reader.getNoteEvents().size() +
                     reader.getParamEvents().size() +
                     reader.getGenerationEvents().size() +
                     reader.getPresetLoadedEvents().size());

    for (size_t i = 0; i < reader.getNoteEvents().size(); ++i)
        timeline.push_back({ reader.getNoteEvents()[i].timestamp, 0, i });
    for (size_t i = 0; i < reader.getParamEvents().size(); ++i)
        timeline.push_back({ reader.getParamEvents()[i].timestamp, 1, i });
    for (size_t i = 0; i < reader.getGenerationEvents().size(); ++i)
        timeline.push_back({ reader.getGenerationEvents()[i].timestamp, 2, i });
    for (size_t i = 0; i < reader.getPresetLoadedEvents().size(); ++i)
        timeline.push_back({ reader.getPresetLoadedEvents()[i].timestamp, 3, i });

    std::sort(timeline.begin(), timeline.end(),
              [](const TimelineEntry& a, const TimelineEntry& b) { return a.timestamp < b.timestamp; });

    printf("Timeline (%zu events):\n", timeline.size());
    printf("─────────────────────────────────────────────────────────────────────\n");

    int noteCount = 0, paramCount = 0, genCount = 0, presetCount = 0;
    int externalCaptureCount = 0;

    for (const auto& entry : timeline)
    {
        printTimestamp(entry.timestamp, header.sampleRate);
        printf("  ");

        switch (entry.kind)
        {
            case 0: // Note
            {
                const auto& n = reader.getNoteEvents()[entry.index];
                printf("[%s] %s note=%d vel=%.2f src=%-7s strand=%d ch=%d\n",
                       typeStr(n.type), typeStr(n.type),
                       n.note, n.velocity,
                       sourceStr(n.source), n.strandId, n.midiChannel);
                ++noteCount;
                break;
            }
            case 1: // Param
            {
                const auto& p = reader.getParamEvents()[entry.index];
                printf("[PARAM] %-20s = %.4f (%s)\n",
                       p.paramId.toRawUTF8(), p.value, originStr(p.origin));
                ++paramCount;
                break;
            }
            case 2: // Generation
            {
                const auto& g = reader.getGenerationEvents()[entry.index];
                printf("[GEN] id=%llu parent=%llu %s seed=%d %.1fs model=%s\n",
                       (unsigned long long)g.generationId,
                       (unsigned long long)g.parentGenerationId,
                       g.success ? "OK" : "FAIL",
                       g.realizedSeed,
                       g.generationTimeMs / 1000.0f,
                       g.model.toRawUTF8());
                if (g.hadInitAudio && g.parentGenerationId == 0)
                {
                    printf("       ⚠ External capture (non-reproducible)\n");
                    ++externalCaptureCount;
                }
                ++genCount;
                break;
            }
            case 3: // Preset loaded
            {
                const auto& pl = reader.getPresetLoadedEvents()[entry.index];
                printf("[PRESET] loaded: %s\n",
                       pl.presetName.isNotEmpty() ? pl.presetName.toRawUTF8() : "(unnamed)");
                ++presetCount;
                break;
            }
        }
    }

    printf("─────────────────────────────────────────────────────────────────────\n");
    printf("\nSummary:\n");
    printf("  Notes:       %d\n", noteCount);
    printf("  Params:      %d\n", paramCount);
    printf("  Generations: %d\n", genCount);
    printf("  Presets:     %d\n", presetCount);
    printf("  Duration:    %.1f seconds (%llu samples)\n",
           static_cast<double>(reader.getTotalDurationSamples()) / header.sampleRate,
           (unsigned long long)reader.getTotalDurationSamples());
    if (reader.getSkippedEventCount() > 0)
        printf("  ⚠ Skipped:   %d events (malformed)\n", reader.getSkippedEventCount());
    if (externalCaptureCount > 0)
        printf("  ⚠ External captures: %d (non-reproducible in replay)\n", externalCaptureCount);

    // Generation parent-chain validation
    printf("\nGeneration chain:\n");
    std::map<uint64_t, const GenerationEventLogEntry*> genById;
    for (const auto& g : reader.getGenerationEvents())
        genById[g.generationId] = &g;

    int chainBreaks = 0;
    for (const auto& g : reader.getGenerationEvents())
    {
        if (g.parentGenerationId != 0)
        {
            if (genById.find(g.parentGenerationId) == genById.end())
            {
                printf("  ⚠ Gen %llu references missing parent %llu (chain break)\n",
                       (unsigned long long)g.generationId,
                       (unsigned long long)g.parentGenerationId);
                ++chainBreaks;
            }
        }
    }
    if (chainBreaks == 0)
        printf("  ✓ All parent references valid\n");

    return 0;
}