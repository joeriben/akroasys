#include "EventLogReader.h"
#include "../dsp/VoiceManager.h"

namespace
{
NoteEventLogEntry::Source sourceFromKey(const juce::String& key)
{
    if (key == "step_seq") return NoteEventLogEntry::Source::StepSequencer;
    if (key == "gen_seq")  return NoteEventLogEntry::Source::GenerativeSequencer;
    if (key == "arp")      return NoteEventLogEntry::Source::Arpeggiator;
    return NoteEventLogEntry::Source::ExternalMidi;
}

VoiceEvent::Type typeFromKey(const juce::String& key)
{
    return key == "off" ? VoiceEvent::Type::NoteOff : VoiceEvent::Type::NoteOn;
}

VoiceEvent::Articulation articFromKey(const juce::String& key)
{
    if (key == "bind") return VoiceEvent::Articulation::Bind;
    if (key == "glide") return VoiceEvent::Articulation::Glide;
    return VoiceEvent::Articulation::Normal;
}

ParamOrigin originFromKey(const juce::String& key)
{
    if (key == "user")    return ParamOrigin::UserEdit;
    if (key == "midi_cc") return ParamOrigin::MidiCCLearn;
    return ParamOrigin::HostAutomation;
}
} // namespace

bool EventLogReader::loadFile(const juce::File& file)
{
    if (! file.existsAsFile())
    {
        errorMessage_ = "File does not exist: " + file.getFullPathName();
        return false;
    }

    const auto content = file.loadFileAsString();
    return loadFromString(content);
}

bool EventLogReader::loadFromString(const juce::String& content)
{
    noteEvents_.clear();
    paramEvents_.clear();
    generationEvents_.clear();
    presetLoadedEvents_.clear();
    totalDurationSamples_ = 0;
    skippedCount_ = 0;
    errorMessage_.clear();

    auto lines = juce::StringArray::fromLines(content);
    // Remove empty trailing lines
    while (lines.size() > 0 && lines.strings.getReference(lines.size() - 1).trim().isEmpty())
        lines.strings.remove(lines.size() - 1);

    if (lines.isEmpty())
    {
        errorMessage_ = "Empty file";
        return false;
    }

    return parseLines(lines);
}

bool EventLogReader::parseLines(const juce::StringArray& lines)
{
    // First line must be the header
    if (lines.size() < 1)
    {
        errorMessage_ = "No header line";
        return false;
    }

    {
        juce::var headerObj;
        if (! juce::JSON::parse(lines[0], headerObj) || ! headerObj.isObject())
        {
            errorMessage_ = "Failed to parse header JSON";
            return false;
        }
        if (! parseHeader(headerObj))
            return false;
    }

    // Parse event lines
    for (int i = 1; i < lines.size(); ++i)
    {
        const auto& line = lines[i];
        if (line.trim().isEmpty())
            continue;

        juce::var obj;
        if (! juce::JSON::parse(line, obj) || ! obj.isObject())
        {
            ++skippedCount_;
            continue;
        }

        const auto kind = obj.getProperty("kind", "").toString();
        bool parsed = false;

        if (kind == "note")
            parsed = parseNoteEvent(obj);
        else if (kind == "param")
            parsed = parseParamEvent(obj);
        else if (kind == "generation")
            parsed = parseGenerationEvent(obj);
        else if (kind == "preset_loaded")
            parsed = parsePresetLoadedEvent(obj);

        if (! parsed)
            ++skippedCount_;
    }

    sortByTimestamp();
    return true;
}

bool EventLogReader::parseHeader(const juce::var& obj)
{
    header_.formatVersion = static_cast<int>(obj.getProperty("formatVersion", 1));
    header_.t5ynthVersion = obj.getProperty("t5ynthVersion", "").toString();
    header_.sampleRate    = static_cast<double>(obj.getProperty("sampleRate", 44100.0));
    header_.startStateBase64 = obj.getProperty("startState", "").toString();
    // Absent on every tape written before v3 — and absent means epoch 0, the
    // regime before any calibration, exactly as it does for a .t5p or a session.
    header_.calibEpoch    = static_cast<int>(obj.getProperty("calibEpoch", 0));
    return true;
}

bool EventLogReader::parseNoteEvent(const juce::var& obj)
{
    NoteEventLogEntry e;
    e.timestamp   = static_cast<uint64_t>(static_cast<juce::int64>(obj.getProperty("t", 0)));
    e.source      = sourceFromKey(obj.getProperty("source", "midi").toString());
    e.type        = typeFromKey(obj.getProperty("type", "on").toString());
    e.note        = static_cast<int>(obj.getProperty("note", 60));
    e.velocity    = static_cast<float>(static_cast<double>(obj.getProperty("velocity", 0.8)));
    e.artic       = articFromKey(obj.getProperty("artic", "normal").toString());
    // A tape is a file, and a file can say anything. The strand id becomes a
    // voice SOURCE id, and VoiceManager clamps those to 0..15 - so an id above
    // the range saturates onto the one the computer keyboard plays under, and a
    // replayed note would read as a key physically held. Anything out of range
    // is not a strand; -1 (external) is what it is.
    {
        const int sid = static_cast<int>(obj.getProperty("strandId", -1));
        e.strandId = (sid >= 0 && sid < VoiceManager::kComputerKeyboardSourceId) ? sid : -1;
    }
    e.pan         = static_cast<float>(static_cast<double>(obj.getProperty("pan", 0.0)));
    e.midiChannel = static_cast<int>(obj.getProperty("channel", 0));

    noteEvents_.push_back(e);
    totalDurationSamples_ = juce::jmax(totalDurationSamples_, e.timestamp);
    return true;
}

bool EventLogReader::parseParamEvent(const juce::var& obj)
{
    ParsedParamEvent e;
    e.timestamp = static_cast<uint64_t>(static_cast<juce::int64>(obj.getProperty("t", 0)));
    e.paramId   = obj.getProperty("param", "").toString();
    e.value     = static_cast<float>(static_cast<double>(obj.getProperty("value", 0.0)));
    e.origin    = originFromKey(obj.getProperty("origin", "host").toString());

    paramEvents_.push_back(e);
    totalDurationSamples_ = juce::jmax(totalDurationSamples_, e.timestamp);
    return true;
}

bool EventLogReader::parseGenerationEvent(const juce::var& obj)
{
    GenerationEventLogEntry e;
    e.timestamp          = static_cast<uint64_t>(static_cast<juce::int64>(obj.getProperty("t", 0)));
    e.generationId       = static_cast<uint64_t>(static_cast<juce::int64>(obj.getProperty("generationId", 0)));
    e.parentGenerationId = static_cast<uint64_t>(static_cast<juce::int64>(obj.getProperty("parentGenerationId", 0)));
    e.success            = static_cast<bool>(obj.getProperty("success", false));
    e.generationTimeMs   = static_cast<float>(static_cast<double>(obj.getProperty("generationTimeMs", 0.0)));
    e.realizedSeed       = static_cast<int>(obj.getProperty("seed", -1));

    e.promptA          = obj.getProperty("promptA", "").toString();
    e.promptB          = obj.getProperty("promptB", "").toString();
    e.alpha            = static_cast<float>(static_cast<double>(obj.getProperty("alpha", 0.0)));
    e.magnitude        = static_cast<float>(static_cast<double>(obj.getProperty("magnitude", 1.0)));
    e.noiseSigma       = static_cast<float>(static_cast<double>(obj.getProperty("noiseSigma", 0.0)));
    e.durationSeconds  = static_cast<float>(static_cast<double>(obj.getProperty("durationSeconds", 3.0)));
    e.startPosition    = static_cast<float>(static_cast<double>(obj.getProperty("startPosition", 0.0)));
    e.steps            = static_cast<int>(obj.getProperty("steps", 20));
    e.cfgScale         = static_cast<float>(static_cast<double>(obj.getProperty("cfgScale", 7.0)));
    e.device           = obj.getProperty("device", "").toString();
    e.model            = obj.getProperty("model", "").toString();
    e.trackType        = obj.getProperty("trackType", "").toString();
    e.modalityEpoch    = static_cast<int>(obj.getProperty("modalityEpoch", 1));

    // dimensionOffsets: array of {dim, value}
    if (auto* dimArr = obj.getProperty("dimensionOffsets", juce::var()).getArray())
    {
        for (const auto& item : *dimArr)
        {
            if (auto* entry = item.getDynamicObject())
            {
                const int dim = static_cast<int>(entry->getProperty("dim"));
                const float val = static_cast<float>(static_cast<double>(entry->getProperty("value")));
                e.dimensionOffsets.push_back({ dim, val });
            }
        }
    }

    // semanticAxes: object with string keys and float values
    if (auto* axesObj = obj.getProperty("semanticAxes", juce::var()).getDynamicObject())
    {
        for (const auto& prop : axesObj->getProperties())
            e.semanticAxes[prop.name.toString()] = static_cast<float>(static_cast<double>(prop.value));
    }
    e.axesAmount = static_cast<float>(static_cast<double>(obj.getProperty("axesAmount", 1.0)));

    e.injectionMode         = obj.getProperty("injectionMode", "linear").toString();
    e.injectionTransitionAt = static_cast<float>(static_cast<double>(obj.getProperty("injectionTransitionAt", 0.6)));
    e.latePhaseAlpha        = static_cast<float>(static_cast<double>(obj.getProperty("latePhaseAlpha", 0.0)));
    e.splitStart            = static_cast<float>(static_cast<double>(obj.getProperty("splitStart", 4.0)));
    e.splitEnd              = static_cast<float>(static_cast<double>(obj.getProperty("splitEnd", 16.0)));

    e.hadInitAudio        = static_cast<bool>(obj.getProperty("hadInitAudio", false));
    e.initAudioSampleRate = static_cast<double>(obj.getProperty("initAudioSampleRate", 0.0));
    e.initNoiseLevel      = static_cast<float>(static_cast<double>(obj.getProperty("initNoiseLevel", 1.0)));

    generationEvents_.push_back(e);
    totalDurationSamples_ = juce::jmax(totalDurationSamples_, e.timestamp);
    return true;
}

bool EventLogReader::parsePresetLoadedEvent(const juce::var& obj)
{
    PresetLoadedLogEntry e;
    e.timestamp  = static_cast<uint64_t>(static_cast<juce::int64>(obj.getProperty("t", 0)));
    e.presetName = obj.getProperty("presetName", "").toString();

    presetLoadedEvents_.push_back(e);
    totalDurationSamples_ = juce::jmax(totalDurationSamples_, e.timestamp);
    return true;
}

void EventLogReader::sortByTimestamp()
{
    std::sort(noteEvents_.begin(), noteEvents_.end(),
              [](const NoteEventLogEntry& a, const NoteEventLogEntry& b) { return a.timestamp < b.timestamp; });

    std::sort(paramEvents_.begin(), paramEvents_.end(),
              [](const ParsedParamEvent& a, const ParsedParamEvent& b) { return a.timestamp < b.timestamp; });

    std::sort(generationEvents_.begin(), generationEvents_.end(),
              [](const GenerationEventLogEntry& a, const GenerationEventLogEntry& b) { return a.timestamp < b.timestamp; });

    std::sort(presetLoadedEvents_.begin(), presetLoadedEvents_.end(),
              [](const PresetLoadedLogEntry& a, const PresetLoadedLogEntry& b) { return a.timestamp < b.timestamp; });
}