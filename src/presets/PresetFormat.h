#pragma once
#include <JuceHeader.h>
#include <array>
#include <limits>
#include <vector>
#include "../dsp/WavetableOscillator.h"

class T5ynthProcessor;

/**
 * Preset serialization and deserialization.
 *
 * Format v5 (.t5p): Binary container with FLAC-compressed audio payloads.
 *   [4B]  Magic "T5YN"
 *   [4B]  Version (uint32 LE, currently 5)
 *   [4B]  JSON length (uint32 LE)
 *   [NB]  JSON (params + meta + embeddings + snapshots + lco)
 *   [VAR] Sequence of length-prefixed FLAC blobs, in JSON-declared order:
 *           primary audio · inferenceCache entries · sequencer one-shots ·
 *           snapshot audio (one blob per JSON snapshot entry) · LCO frames A
 *           (only present when the JSON "lco" block's frameCountA > 0).
 *           Each blob is [4B uint32 LE byteLen][N FLAC bytes]. 24-bit
 *           lossless FLAC; the FLAC stream's STREAMINFO carries sampleRate,
 *           channels and sampleCount — JSON metadata mirrors them so the
 *           library UI can describe a preset without decoding the audio.
 *
 * Format break v4 → v5: adds the "lco" JSON block (DCO/LCO bake prompt,
 * readings, A/B balance, Re-Prompt stance, B's additive station data as
 * plain JSON, and frame-count metadata for A) plus a trailing optional FLAC
 * blob carrying A's exact baked wavetable frames. This lets an LCO preset
 * round-trip its baked sound and text without re-running the LLM on load —
 * previously a DCO/LCO bake saved and reloaded as a generic, often-stale
 * Wavetable preset (engine.mode was forced to "wavetable" and nothing
 * LCO-specific was captured at all). Absence of the "lco" block (any v4
 * file, or a v5 file saved with no active LCO bake) is the "not an LCO
 * preset" fallback — same append-only append-safe policy as the v4
 * "snapshots" key. kMinLoadableVersion stays 3; v3/v4 files still load.
 *
 * Format break v3 → v4: audio is FLAC instead of raw float32 PCM. v3
 * presets remain loadable (the reader dispatches on the version field
 * and falls back to the raw-PCM path); writes always emit v4.
 *
 * Format break v2 → v3: all choice-parameter JSON fields are serialised
 * as stable snake_case keys from BlockParams.h kEntries (the `.key`
 * column). v2 and v1 presets are rejected outright — migrate via the
 * one-off Python tool used for the bundled DEMO preset.
 */
class PresetFormat
{
public:
    PresetFormat() = default;

    // Semantic axes: 3 slots (dropdown selection + slider value).
    struct AxisState { int dropdownId = 1; float value = 0.0f; };

    /** Per-snapshot state restored when the user activates a slot. Mirrors
     *  MainPanel::MainSnapshot but lives here so PresetFormat can persist it
     *  without depending on the GUI. Audio is serialised as a length-prefixed
     *  FLAC blob in the payload tail; all other fields ride in the JSON
     *  `snapshots` array. */
    struct SnapshotState
    {
        int slot = 0;          // 0-based slot index (UI shows 1..4)
        bool valid = false;

        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;

        juce::String promptA, promptB;
        juce::String device, model;
        juce::String injectionMode { "linear" };
        int seed = 0;
        bool randomSeed = false;
        float lateMixAmount = 0.75f;
        float splitStart    = 4.0f;
        float splitEnd      = 16.0f;

        std::array<AxisState, 3> axes;
        std::vector<float> embeddingA, embeddingB;
        std::vector<std::pair<int, float>> dimensionOffsets;

        // APVTS ValueTree serialised as XML — restored by MainPanel using
        // its kMainSnapshotParamIds whitelist.
        juce::String parametersXml;

        // Sampler markers — replicate MainSnapshot's layout point capture.
        float loopStart = 0.0f;
        float loopEnd = 1.0f;
        float startPos = 0.0f;
        float wtExtractStart = 0.0f;
        float wtExtractEnd = 1.0f;
        bool pointsLocked = false;
    };

    /** Result of loading a preset (audio + embeddings are optional). */
    struct LoadResult
    {
        bool success = false;
        juce::String presetName;
        juce::String promptA, promptB;
        int seed = 123456789;
        bool randomSeed = false;
        juce::String device;
        juce::String model;

        // Calibration epoch the file was authored under (0 = absent / pre-calibration).
        // Used to rescale stored values so old presets stay sonically identical.
        int calibEpoch = 0;

        // Embedded audio (empty if old-format preset)
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        bool hasAudio = false;

        struct InferenceCacheAudio
        {
            juce::AudioBuffer<float> audio;
            double sampleRate = 44100.0;
        };
        int inferenceCacheCapacity = 0;
        bool inferenceCacheIsOfflineTake = false;
        std::vector<InferenceCacheAudio> inferenceCache;

        // The LRO's cache. No audio blob and no FLAC: a slot is what an
        // authoring pass installs, which is text and two var trees, so it goes
        // into the JSON whole and costs kilobytes.
        struct CsoundCacheAuthored
        {
            juce::String orchestra, prompt, reading, paramsText, authorModel;
            juce::var controls, settings;
        };
        int csoundCacheCapacity = 0;
        bool csoundCacheIsOfflineTake = false;
        std::vector<CsoundCacheAuthored> csoundCache;

        std::array<AxisState, 3> axes;
        bool hasAxes = false;

        // Embeddings (empty if not available)
        std::vector<float> embeddingA, embeddingB;

        // User-assigned classification tags (empty for legacy presets)
        juce::StringArray tags;

        // Persisted per-slot snapshots (empty when the preset has no
        // snapshot section, e.g. all v3 and pre-snapshot v4 files).
        std::vector<SnapshotState> snapshots;

        // Research-mode injection state. Old .t5p files predating this feature
        // get the canonical pre-injection defaults — linear / 0.75 / 4 / 16 —
        // so loading an old preset reproduces its original sound regardless
        // of which mode the panel was on. New presets always overwrite these
        // with their saved values.
        juce::String injectionMode { "linear" };
        float lateMixAmount = 0.75f;
        float splitStart    = 4.0f;
        float splitEnd      = 16.0f;

        // LCO/DCO bake (format v5+; absent on any v4 file or a v5 file with
        // no active bake — hasLco stays false, caller skips the restore).
        bool hasLco = false;
        juce::String lcoPrompt, lcoReadingA, lcoReadingB;
        float lcoMotionRateHz = 0.0f;
        bool  lcoOscAHasContent = false, lcoOscBHasContent = false;
        float lcoGainA = 1.0f, lcoGainB = 1.0f;
        // A's exact baked wavetable frame strip (mono, N*FRAME_SIZE samples),
        // empty when lcoOscAHasContent is false.
        juce::AudioBuffer<float> lcoFramesA;
        // B's real-time additive stations, empty when lcoOscBHasContent is false.
        std::vector<std::vector<WavetableOscillator::AdditivePartial>> lcoStationsB;
    };

    /** Save current state to a .t5p file with embedded audio. */
    static bool saveToFile(const juce::File& file, T5ynthProcessor& processor,
                           bool includeInferenceCache = true,
                           const std::vector<SnapshotState>* snapshots = nullptr);

    /** Load a preset from file. Returns full result with audio + metadata. */
    static LoadResult loadFromFile(const juce::File& file, T5ynthProcessor& processor);

    /** Get the preset file extension. */
    static juce::String getFileExtension() { return ".t5p"; }

    /** Get default preset directory (creates if needed). Alias for getUserPresetsDirectory(). */
    static juce::File getPresetsDirectory();

    /** Per-user presets directory (writable, creates if needed). */
    static juce::File getUserPresetsDirectory();

    /** True when the user presets directory is itself a git checkout —
     *  i.e. this is the maintainer ("mother") machine that publishes the
     *  UCDCAE AI Lab bank. On that machine, saves/tag-edits of bank
     *  presets overwrite the originals directly instead of forking to
     *  "<name> (mine)" (per maintainer directive, 2026-06-12); git is the
     *  safety net there. All other installations keep the fork behavior.
     *  See docs/PRESET_LIBRARY_MAINTENANCE.md. */
    static bool userPresetsDirIsGitCheckout();

    /** Per-user sequencer-pattern (.t5seq) directory (writable, creates if needed). */
    static juce::File getUserSequencesDirectory();

    /** All .t5p files under the user presets directory (recursive). */
    static juce::Array<juce::File> getAllPresetFiles();

private:
    static constexpr char kMagic[4] = { 'T', '5', 'Y', 'N' };
    static constexpr uint32_t kVersion = 5;
    // v3 = raw float32 PCM payloads. v4 = length-prefixed FLAC blobs.
    // v5 = adds the "lco" JSON block + trailing LCO-frames-A FLAC blob.
    // All are accepted on read; writes always emit kVersion.
    static constexpr uint32_t kMinLoadableVersion = 3;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PresetFormat)
};
