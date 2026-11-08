#pragma once
#include <JuceHeader.h>
#include <cstdint>
#include "../dsp/VoiceEvent.h"

/**
 * Phase 1 of the session event log: an append-only `.t5evt` file (newline-
 * delimited JSON, one compact object per line) recording note events, param
 * changes, and generation calls during a live session — never rendered audio.
 * Structs here are the audio/message-thread-facing PODs; EventLog.cpp turns
 * each into one JSONL line. See docs/HANDOVER_* siblings for the MIDI-Out,
 * undo/redo, and SMF-export phases — none of that is built here.
 */

/** Origin of a ParamEvent value change. HostAutomation is also the default
 *  bucket for anything not explicitly hinted (real host automation AND any
 *  GUI path this phase didn't wire — see the Phase 1 report for which paths
 *  those are). */
enum class ParamOrigin : uint8_t { HostAutomation, UserEdit, MidiCCLearn };

/** Internal note event tap — one per VoiceEvent appended by the step
 *  sequencer, generative sequencer, or arpeggiator, tagged with which of the
 *  three produced it (VoiceEvent::strandId alone can't tell StepSeq and Arp
 *  apart — both always use -1). */
struct NoteEventLogEntry
{
    enum class Source : uint8_t { StepSequencer, GenerativeSequencer, Arpeggiator, ExternalMidi };

    uint64_t          timestamp    = 0;   // running sample counter at tap time (+ sampleOffset)
    Source            source       = Source::ExternalMidi;
    VoiceEvent::Type  type         = VoiceEvent::Type::NoteOn;
    int               note         = 60;
    float             velocity     = 0.8f;
    VoiceEvent::Articulation artic = VoiceEvent::Articulation::Normal;
    int               strandId     = -1;
    float             pan          = 0.0f;
    int               midiChannel  = 0;   // ExternalMidi only; 0 for internal sources
};

/** One CC-Learn/host-automation/user-edit change to an APVTS param. paramIndex
 *  resolves to the human-readable ID only at serialization time (message
 *  thread), never inside the hot parameterChanged callback. */
struct ParamEventLogEntry
{
    uint64_t    timestamp  = 0;
    int         paramIndex = -1;
    float       value      = 0.0f;
    ParamOrigin origin     = ParamOrigin::HostAutomation;
};

/** One completed PipeInference::generate() call. Mirrors PipeInference::Request
 *  minus the actual audio (initAudio is reduced to hadInitAudio) plus the
 *  realized outcome (result.seed, not req.seed — the request seed reads -1
 *  under "random"). Constructed on the message thread from PromptPanel. */
struct GenerationEventLogEntry
{
    uint64_t     timestamp   = 0;
    uint64_t     generationId = 0;
    uint64_t     parentGenerationId = 0;   // 0 = none; set iff this call's initAudio was the immediately-preceding generation's own output (internal resynth chain)
    bool         success      = false;
    float        generationTimeMs = 0.0f;
    int          realizedSeed = -1;

    juce::String promptA, promptB;
    float        alpha           = 0.0f;
    float        magnitude       = 1.0f;
    float        noiseSigma      = 0.0f;
    float        durationSeconds = 3.0f;
    float        startPosition   = 0.0f;
    int          steps           = 20;
    float        cfgScale        = 7.0f;
    juce::String device;
    juce::String model;
    juce::String trackType;
    int          modalityEpoch   = 1;

    std::vector<std::pair<int, float>>    dimensionOffsets;
    std::map<juce::String, float>         semanticAxes;
    float        axesAmount            = 1.0f;

    juce::String injectionMode         { "linear" };
    float        injectionTransitionAt = 0.6f;
    float        latePhaseAlpha        = 0.0f;
    float        splitStart            = 4.0f;
    float        splitEnd              = 16.0f;

    bool         hadInitAudio        = false;   // true iff req.initAudio was non-empty — NOT the audio itself
    double       initAudioSampleRate = 0.0;
    float        initNoiseLevel      = 1.0f;

    // Whole octaves the octave fit moved the result by before it played
    // (GeneratedOctave; positive = up). Absent on a tape older than the fit,
    // which reads as 0: those results played as the model made them.
    int          octaveMove          = 0;
};

/** Coarse marker for a bulk parameter load (preset import / DAW state restore)
 *  — replaces what would otherwise be one ParamEvent per restored param. */
struct PresetLoadedLogEntry
{
    uint64_t     timestamp = 0;
    juce::String presetName;   // may be empty (name not always available at the call site)
};

/** Header line — leads every .t5evt file, before any event line. */
struct EventLogHeader
{
    int          formatVersion = 3;   // v2 adds startStateBase64; v3 adds calibEpoch
    juce::String t5ynthVersion;
    double       sampleRate = 44100.0;
    juce::String startStateBase64;   // base64-encoded APVTS state at recording start (R0)
    // Which calibrations the LOGGED VALUES predate. A param event stores the
    // denormalised value, so a number written under an older law means something
    // else today — and one of them changed kind outright when the envelope curve
    // stopped being a 0..4 choice (Calibration epoch 9). Absent on a v1/v2 tape,
    // which reads as epoch 0, the same convention every other stored surface uses.
    int          calibEpoch = 0;
};

juce::DynamicObject::Ptr eventLogHeaderToDynamicObject(const EventLogHeader& h);
juce::DynamicObject::Ptr eventLogEntryToDynamicObject(const NoteEventLogEntry& e);
juce::DynamicObject::Ptr eventLogEntryToDynamicObject(const ParamEventLogEntry& e, const juce::String& paramId);
juce::DynamicObject::Ptr eventLogEntryToDynamicObject(const GenerationEventLogEntry& e);
juce::DynamicObject::Ptr eventLogEntryToDynamicObject(const PresetLoadedLogEntry& e);
