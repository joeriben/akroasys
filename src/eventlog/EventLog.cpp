#include "EventLog.h"

namespace
{
const char* noteEventSourceKey(NoteEventLogEntry::Source s)
{
    switch (s)
    {
        case NoteEventLogEntry::Source::StepSequencer:        return "step_seq";
        case NoteEventLogEntry::Source::GenerativeSequencer:  return "gen_seq";
        case NoteEventLogEntry::Source::Arpeggiator:          return "arp";
        case NoteEventLogEntry::Source::ExternalMidi:         return "midi";
    }
    return "midi";
}

const char* voiceEventTypeKey(VoiceEvent::Type t)
{
    return t == VoiceEvent::Type::NoteOn ? "on" : "off";
}

const char* articulationKey(VoiceEvent::Articulation a)
{
    switch (a)
    {
        case VoiceEvent::Articulation::Normal: return "normal";
        case VoiceEvent::Articulation::Bind:   return "bind";
        case VoiceEvent::Articulation::Glide:  return "glide";
    }
    return "normal";
}

const char* paramOriginKey(ParamOrigin o)
{
    switch (o)
    {
        case ParamOrigin::HostAutomation: return "host";
        case ParamOrigin::UserEdit:       return "user";
        case ParamOrigin::MidiCCLearn:    return "midi_cc";
    }
    return "host";
}

juce::var dimensionOffsetsToVar(const std::vector<std::pair<int, float>>& offsets)
{
    juce::Array<juce::var> arr;
    for (const auto& o : offsets)
    {
        juce::DynamicObject::Ptr entry = new juce::DynamicObject();
        entry->setProperty("dim",   o.first);
        entry->setProperty("value", static_cast<double>(o.second));
        arr.add(entry.get());
    }
    return arr;
}

juce::var semanticAxesToVar(const std::map<juce::String, float>& axes)
{
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    for (const auto& kv : axes)
        obj->setProperty(kv.first, static_cast<double>(kv.second));
    return juce::var(obj.get());
}
} // namespace

juce::DynamicObject::Ptr eventLogHeaderToDynamicObject(const EventLogHeader& h)
{
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("formatVersion", h.formatVersion);
    obj->setProperty("t5ynthVersion", h.t5ynthVersion);
    obj->setProperty("sampleRate",    h.sampleRate);
    obj->setProperty("calibEpoch",    h.calibEpoch);
    if (h.startStateBase64.isNotEmpty())
        obj->setProperty("startState", h.startStateBase64);
    return obj;
}

juce::DynamicObject::Ptr eventLogEntryToDynamicObject(const NoteEventLogEntry& e)
{
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("kind",      "note");
    obj->setProperty("t",         static_cast<juce::int64>(e.timestamp));
    obj->setProperty("source",    noteEventSourceKey(e.source));
    obj->setProperty("type",      voiceEventTypeKey(e.type));
    obj->setProperty("note",      e.note);
    obj->setProperty("velocity",  static_cast<double>(e.velocity));
    obj->setProperty("artic",     articulationKey(e.artic));
    obj->setProperty("strandId",  e.strandId);
    obj->setProperty("pan",       static_cast<double>(e.pan));
    obj->setProperty("channel",   e.midiChannel);
    return obj;
}

juce::DynamicObject::Ptr eventLogEntryToDynamicObject(const ParamEventLogEntry& e, const juce::String& paramId)
{
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("kind",   "param");
    obj->setProperty("t",      static_cast<juce::int64>(e.timestamp));
    obj->setProperty("param",  paramId);
    obj->setProperty("value",  static_cast<double>(e.value));
    obj->setProperty("origin", paramOriginKey(e.origin));
    return obj;
}

juce::DynamicObject::Ptr eventLogEntryToDynamicObject(const GenerationEventLogEntry& e)
{
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("kind",             "generation");
    obj->setProperty("t",                static_cast<juce::int64>(e.timestamp));
    obj->setProperty("generationId",       static_cast<juce::int64>(e.generationId));
    obj->setProperty("parentGenerationId", static_cast<juce::int64>(e.parentGenerationId));
    obj->setProperty("success",          e.success);
    obj->setProperty("generationTimeMs", static_cast<double>(e.generationTimeMs));
    obj->setProperty("seed",             e.realizedSeed);

    obj->setProperty("promptA", e.promptA);
    obj->setProperty("promptB", e.promptB);
    obj->setProperty("alpha",           static_cast<double>(e.alpha));
    obj->setProperty("magnitude",       static_cast<double>(e.magnitude));
    obj->setProperty("noiseSigma",      static_cast<double>(e.noiseSigma));
    obj->setProperty("durationSeconds", static_cast<double>(e.durationSeconds));
    obj->setProperty("startPosition",   static_cast<double>(e.startPosition));
    obj->setProperty("steps",           e.steps);
    obj->setProperty("cfgScale",        static_cast<double>(e.cfgScale));
    obj->setProperty("device",          e.device);
    obj->setProperty("model",           e.model);
    obj->setProperty("trackType",       e.trackType);
    obj->setProperty("modalityEpoch",   e.modalityEpoch);

    obj->setProperty("dimensionOffsets", dimensionOffsetsToVar(e.dimensionOffsets));
    obj->setProperty("semanticAxes",     semanticAxesToVar(e.semanticAxes));
    obj->setProperty("axesAmount",       static_cast<double>(e.axesAmount));

    obj->setProperty("injectionMode",         e.injectionMode);
    obj->setProperty("injectionTransitionAt", static_cast<double>(e.injectionTransitionAt));
    obj->setProperty("latePhaseAlpha",        static_cast<double>(e.latePhaseAlpha));
    obj->setProperty("splitStart",            static_cast<double>(e.splitStart));
    obj->setProperty("splitEnd",              static_cast<double>(e.splitEnd));

    obj->setProperty("hadInitAudio",        e.hadInitAudio);
    obj->setProperty("initAudioSampleRate", e.initAudioSampleRate);
    obj->setProperty("initNoiseLevel",      static_cast<double>(e.initNoiseLevel));
    obj->setProperty("octaveMove",          e.octaveMove);
    return obj;
}

juce::DynamicObject::Ptr eventLogEntryToDynamicObject(const PresetLoadedLogEntry& e)
{
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    obj->setProperty("kind",       "preset_loaded");
    obj->setProperty("t",          static_cast<juce::int64>(e.timestamp));
    obj->setProperty("presetName", e.presetName);
    return obj;
}
