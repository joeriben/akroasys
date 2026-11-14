#pragma once
#include <JuceHeader.h>
#include "../dsp/VoiceEvent.h"
#include <array>
#include <atomic>
#include <functional>
#include <vector>

/**
 * Step sequencer — port of useStepSequencer.ts.
 *
 * Features:
 * - Per-step: note (MIDI), velocity (0-1), gate (0.1-1), bind-mode (3-state), enabled
 * - 5 note divisions: 1/1, 1/2, 1/4, 1/8, 1/16
 * - Gate controls note duration as fraction of step duration
 * - Bind-mode: Off (normal retrigger) → Bind (instant pitch change, no
 *   retrigger) → Glide (ramped pitch change over getGlideTime(), no retrigger).
 *   A Bind/Glide flag is owned by its SOURCE step and ties it into the FOLLOWING
 *   note (TB-303 "slide"): the flagged step is held (no gate-off) and the next
 *   note continues the same voice — instant (Bind) or ramped (Glide) — instead
 *   of retriggering.
 * - 10 preset patterns from reference
 * - Bar-start callback (atomic flag) for drift regen sync
 */
class T5ynthStepSequencer
{
public:
    static constexpr int MAX_STEPS = 64;
    static constexpr int ONE_SHOT_SLOTS = 3;

    /** Note division relative to quarter note */
    enum Division { Div_1_1 = 0, Div_1_2, Div_1_4, Div_1_8, Div_1_16 };
    static constexpr float DIVISION_FACTORS[] = { 4.0f, 2.0f, 1.0f, 0.5f, 0.25f };

    static constexpr int NUM_PRESETS = 10;

    enum class OneShotMode
    {
        Normal = 0,
        Accent,
        Mute
    };

    /** Per-step bind behaviour. Cycle order in the UI: Off → Bind → Glide. */
    enum class BindMode
    {
        Off = 0,   ///< Normal step: retrigger, no pitch carry-over.
        Bind,      ///< No retrigger, instant pitch change (glideMs = 0).
        Glide      ///< No retrigger, pitch ramps over getGlideTime().
    };

    struct OneShotTrigger
    {
        int stepIndex = 0;
        int slotIndex = 0;
        float gain = 1.0f;
        int sampleOffset = 0;
    };

    using OneShotTriggerCallback = std::function<void(const OneShotTrigger&)>;

    T5ynthStepSequencer() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    /** Append this block's note events (typed, not MIDI) to `out`. */
    void processBlock(juce::AudioBuffer<float>& buffer, std::vector<VoiceEvent>& out);
    void reset();

    /** Set the number of active steps (1-64). */
    void setNumSteps(int steps);
    int getNumSteps() const { return numSteps; }

    /** Set step data. */
    void setStepNote(int step, int midiNote);
    void setStepVelocity(int step, float velocity);
    void setStepEnabled(int step, bool enabled);
    void setStepGate(int step, float gate);
    void setStepBindMode(int step, BindMode mode);
    void cycleStepBindMode(int step);
    BindMode getStepBindMode(int step) const { return getStep(step).bindMode; }
    void setStepOneShotMode(int step, int slot, OneShotMode mode);
    void cycleStepOneShotMode(int step, int slot);
    OneShotMode getStepOneShotMode(int step, int slot) const;
    void setOneShotTriggerCallback(OneShotTriggerCallback callback);

    /** Bulk set gate for all steps. */
    void setAllGates(float gate);

    /** Set playback rate in BPM. */
    void setBpm(double newBpm) { bpm = newBpm; }

    /** Set note division. */
    void setDivision(int div) { division = juce::jlimit(0, 4, div); }
    void setShuffle(float amount) { shuffle = juce::jlimit(0.0f, 0.75f, amount); }
    void setOctaveShiftSemitones(int semitones) { octaveShiftSemitones = semitones; }

    /** Auto glide time: 25% of step duration (ms), clamped to 10-200. A short
        leading portamento that reaches the target pitch well before the step
        ends — gliding across half the step (the old 50%) read as sluggish. */
    float getGlideTime() const
    {
        double stepMs = (60.0 / bpm) * static_cast<double>(DIVISION_FACTORS[division]) * 1000.0;
        return juce::jlimit(10.0f, 200.0f, static_cast<float>(stepMs * 0.25));
    }

    /** Load a preset pattern by index (0-9). */
    void loadPreset(int index);

    /** Reset grid to uniform defaults. */
    void resetGrid();

    /** Start/stop sequencer. */
    void start() { if (!running) { running = true; samplesUntilNextStep = 0.0; currentStep = 0; scheduledStep = 0; } }
    void stop();

    /** Append a note-off for the currently-sounding step note to `out` at
        `sampleOffset`, then clear lastPlayedNote. Does not alter running/
        position — safe to call at any transition. */
    void allNotesOff(std::vector<VoiceEvent>& out, int sampleOffset = 0);

    bool isRunning() const { return running; }

    int getCurrentStep() const { return currentStep; }

    /** Atomic step position for GUI polling. */
    std::atomic<int> currentStepForGui { -1 };

    /** Bumped whenever the pattern is replaced from outside the grid: a step
     *  preset (loadPreset) or an engine switch that copies Gen's or Phrase's
     *  pattern into the grid. The GUI polls it, also while the audio path is
     *  idle, and refreshes the step grid. Read it with acquire: the step
     *  count parameter the replacement wrote is visible after it. */
    std::atomic<int> patternReplacedGen { 0 };

    /** Publishes a pattern replacement to the GUI (patternReplacedGen).
     *  Call it after the steps and the step count are written. */
    void markPatternReplaced() noexcept
    {
        patternReplacedGen.fetch_add(1, std::memory_order_release);
    }

    /** Atomic flag: set to true when bar boundary crossed (step wraps to 0). */
    std::atomic<bool> barStartFlag { false };

    struct Step
    {
        int note = 60;
        float velocity = 0.8f;
        float gate = 0.8f;
        bool enabled = true;
        BindMode bindMode = BindMode::Off;
        std::array<OneShotMode, ONE_SHOT_SLOTS> oneShotModes {
            OneShotMode::Normal, OneShotMode::Normal, OneShotMode::Normal
        };
    };

    const Step& getStep(int idx) const { return steps[static_cast<size_t>(juce::jlimit(0, MAX_STEPS - 1, idx))]; }

private:
    std::array<Step, MAX_STEPS> steps;
    int numSteps = 16;
    int currentStep = 0;
    int scheduledStep = 0;
    double bpm = 120.0;
    double sampleRate = 44100.0;
    double samplesUntilNextStep = 0.0;
    double samplesUntilGateOff = -1.0; // <0 means no pending gate-off
    bool running = false;
    int lastPlayedNote = -1;
    int division = 3; // default 1/8 (index 3)
    float shuffle = 0.0f;
    int octaveShiftSemitones = 0;
    OneShotTriggerCallback oneShotTriggerCallback;
    // glideTimeMs removed — now computed automatically from BPM/division

    double stepDurationSamples() const;
    double shuffledStepDurationSamples(int stepIdx) const;
    void emitOneShotTriggers(int stepIdx, const Step& step, int sampleOffset);

    // Preset data
    struct PresetData { const char* name; const Step* steps; int count; };
    static const PresetData presetTable[NUM_PRESETS];
};
