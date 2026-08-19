#pragma once
#include <cmath>
#include <array>
#include <cstdint>

/**
 * Drift LFO system — port of useDriftLfo.ts.
 *
 * 3 internal LFOs with selectable waveform (sine/tri/saw/sq/rnd/inv-saw).
 * Output = waveform(phase) * depth * halfRange (offset added to base).
 * Phase resets to 0 on target change.
 *
 * Target ranges (from reference):
 *   alpha:      [-2, 2]  halfRange = 2
 *   sem_axis_*: [-2, 2]  halfRange = 2
 *   wt_scan:    [0, 1]   halfRange = 0.5
 */
class DriftLFO
{
public:
    /** Target parameter indices. Matches APVTS choice order (0=None, 1=Alpha, etc.) */
    enum Target
    {
        TgtNone = 0,
        TgtAlpha,      // 1
        TgtNoise,      // 2
        TgtMagnitude,  // 3
        TgtAxis1,      // 4
        TgtAxis2,      // 5
        TgtAxis3,      // 6
        TgtResynth,    // 7  (generation-side: grouped with the axes, drives re-inference)
        TgtWtScan,     // 8
        TgtFilter,     // 9
        TgtPitch,      // 10
        TgtDelayTime,  // 11
        TgtDelayFb,    // 12
        TgtDelayMix,   // 13
        TgtReverbMix,  // 14
        TgtEnv1Amt,    // 15
        TgtEnv2Amt,    // 16
        TgtEnv3Amt,    // 17
        TgtEnv4Amt,    // 18
        TgtEnv5Amt,    // 19
        NumTargets
    };

    /** Waveform types. Matches APVTS drift_wave choice order. */
    enum Waveform { Sine = 0, Triangle, Sawtooth, Square, SampleHold, SawDown };

    static constexpr int NUM_LFOS = 3;

    DriftLFO() = default;

    /** Reset all phases and offsets. */
    void reset();

    /** Advance all LFOs by dt seconds. */
    void tick(double dt);

    /** True for the targets that do not shape the sound directly but the NEXT
     *  inference: what the request is built from. The offline cache take freezes
     *  exactly these and lets the audible ones (filter, pitch, scan, delay,
     *  reverb, env amounts) keep running — a held sweep would be audible for as
     *  long as a render takes, which is precisely the machine-dependent duration
     *  the take exists to get rid of. */
    static bool isGenerationTarget(int target)
    {
        return target >= TgtAlpha && target <= TgtResynth;
    }

    /** While held, the generation-side LFOs above stop advancing in tick(). They
     *  then move only by stepGenerationTargets() — one defined step per captured
     *  entry, so the recorded trajectory is the same on every machine. */
    void setGenerationHold(bool held) { genHold = held; }
    bool isGenerationHeld() const { return genHold; }

    /** Advance ONLY the generation-side LFOs by dt seconds, hold or no hold. This
     *  is the take's step from one cache point to the next. */
    void stepGenerationTargets(double dt);

    /** Get the combined offset for a given target parameter. */
    float getOffsetForTarget(int target) const;

    /** The largest magnitude getOffsetForTarget(target) can reach with the
     *  current arming/depths, independent of where the waveforms happen to sit
     *  right now. Every shipped waveform peaks at 1, so this is the sum of
     *  |depth| * halfRange over the contributing LFOs. Needed where a decision
     *  must be made once from what CAN happen rather than from the instantaneous
     *  value — e.g. the sampler's render-path latch at note start. */
    float getReachForTarget(int target) const;

    /** Configure a single internal LFO. */
    void setLfoRate(int lfoIndex, float hz);
    void setLfoDepth(int lfoIndex, float amount);
    void setLfoTarget(int lfoIndex, int target);
    void setLfoWaveform(int lfoIndex, int waveform);

    /** Reset phase of a single internal LFO. Used to phase-align a sync-mode
     *  Drift LFO to the sequencer's beat 1 on a stop→start transition. */
    void resetLfoPhase(int lfoIndex);

    /** Arm/disarm a single internal LFO for beat-sync start. While armed it
     *  holds at phase 0 and contributes zero offset; releasing lets its cycle
     *  begin from phase 0 on the sequencer's downbeat. */
    void setLfoArmed(int lfoIndex, bool armed);

    /** Enable/disable the entire drift system. */
    void setEnabled(bool enabled) { active = enabled; }
    bool isEnabled() const { return active; }

    /** Regen mode: 0=Manual, 1=Auto, 2=1st Bar (bar-boundary trigger). */
    void setRegenMode(int mode) { regenMode = mode; }
    int getRegenMode() const { return regenMode; }

private:
    struct InternalLFO
    {
        double phase = 0.0;
        float rate = 0.01f;     // Hz
        float depth = 0.0f;
        int target = 0;         // APVTS index: 0=None, 1=Alpha, etc.
        int waveform = Sine;
        float heldValue = 0.0f; // latched value for SampleHold waveform
        uint64_t rngState = 0xA1B2C3D4E5F60789ULL;
        bool armed = false;     // held silent at phase 0 until released on the downbeat
    };

    std::array<InternalLFO, NUM_LFOS> lfos {{
        { 0.0, 0.01f,        0.0f, 0, Sine },     // Drift 1: rate=0.01 Hz (100 s/cyc)
        { 0.0, 1.0f / 128.0f, 0.0f, 0, Triangle }, // Drift 2: rate=1/128 Hz — mirrors APVTS floor
        { 0.0, 1.0f / 128.0f, 0.0f, 0, Sine },     // Drift 3: rate=1/128 Hz — mirrors APVTS floor
    }};

    bool active = false;
    int regenMode = 0; // 0=Manual, 1=Auto, 2=1st Bar
    bool genHold = false;   // generation-side LFOs frozen (offline cache take)

    static constexpr double TWO_PI = 6.283185307179586;

    /** Move one LFO's phase on by dt seconds, wrapping and re-drawing a
     *  sample-and-hold value on the wrap. A step that spans several cycles draws
     *  ONE new value, like any other wrap — the take samples the waveform at its
     *  cache points, it does not replay what happened between them. */
    static void advancePhase(InternalLFO& lfo, double dt);

    /** Compute waveform value for phase 0-1, returns -1..+1. */
    static float waveformValue(const InternalLFO& lfo);
    static float nextRandom(InternalLFO& lfo);

    /** Get half-range for a target (APVTS index: 0=None, 1=Alpha, ..., 5=WtScan). */
    static float halfRangeForTarget(int target);

    /** The depth a target actually receives for a knob position. Almost every
     *  target takes the position as it stands; the cutoff bus owns a curve
     *  (ModCalib::cutoffDepthCurve) that every source feeding it travels, drift
     *  included — so the drift depth knob reaches the filter the same way an env
     *  amount or an LFO depth does. */
    static float depthForTarget(int target, float depth);
};
