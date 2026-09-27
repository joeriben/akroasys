#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>

/**
 * State-variable filter — port of useFilter.ts.
 *
 * Supports lowpass, highpass, bandpass with 12dB or 24dB slope (cascade).
 * Equal-power dry/wet crossfade: dry = cos(mix * pi/2), wet = sin(mix * pi/2).
 * Resonance: UI 0-1 mapped to Q 0.5-18 via 0.5 * pow(36, r).
 */
class T5ynthFilter
{
public:
    T5ynthFilter() = default;

    void prepare(double sampleRate, int samplesPerBlock);
    void processBlock(juce::AudioBuffer<float>& buffer);
    void reset();

    /** Copy individual stages from src, each independently flagged. Used by
     *  the filter-transition pre-roll (SynthVoice::preRollFilterTransition) to
     *  take only the stages whose upstream input actually changed, keeping
     *  the others' already-settled state.
     *
     *  PRECONDITION: src is configured identically to this filter (same
     *  cutoff, resonance, type, slope) — the copy carries filter1/filter2's
     *  juce::dsp::StateVariableTPTFilter state AND coefficients, which is
     *  only correct when the two already agree. Copy-assigning the juce
     *  filter reuses its equal-size std::vectors (both instances prepared
     *  with numChannels 2) rather than reallocating, so this is
     *  audio-thread safe. */
    void takeStagesFrom (const T5ynthFilter& src, bool stage1, bool stage2, bool onePole);

    /** Set cutoff frequency in Hz. `force` skips the dead-band test below --
     *  for a caller that approached this value in steps the dead-band may
     *  have swallowed, and must land it exactly. */
    void setCutoff(float hz, bool force = false);

    /** Set resonance (0-1), mapped internally to Q 0.5-18. */
    void setResonance(float r);

    /** Set filter type: 0=Lowpass, 1=Highpass, 2=Bandpass. */
    void setType(int type);

    /** Set dry/wet mix (0=fully dry / bypassed, 1=fully wet / filtered).
     *  Uses equal-power crossfade: dry = cos(mix*pi/2), wet = sin(mix*pi/2). */
    void setMix(float mix);

    /** Set filter slope: 0=6dB, 1=12dB, 2=18dB, 3=24dB. */
    void setSlope(int slope);

    /** Process a single mono sample (channel 0). For per-voice use. */
    float processSample(float sample);

    /** Map UI resonance 0-1 to filter Q 0.5-18 (exponential). */
    static float resonanceToQ(float r)
    {
        return 0.5f * std::pow(36.0f, std::clamp(r, 0.0f, 1.0f));
    }

private:
    juce::dsp::StateVariableTPTFilter<float> filter1;
    juce::dsp::StateVariableTPTFilter<float> filter2; // second stage for 24dB cascade

    // One-pole filter for 6dB and 18dB slopes
    float onePoleState = 0.0f;
    float onePoleCoeff = 0.0f;  // computed from cutoff
    void updateOnePoleCoeff(float cutoffHz);

    double sr = 44100.0;
    int blockSize = 512;
    float currentMix = 1.0f;
    int currentType = -1;
    int currentSlope = 1; // 0=6dB, 1=12dB, 2=18dB, 3=24dB
    bool prepared = false;

    // Cached values to skip redundant coefficient recomputation
    float lastSetCutoff = 20000.0f;
    float lastSetReso = 0.0f;

    // Pre-computed dry/wet gains (avoid sin/cos per sample)
    float cachedWetGain = 1.0f;
    float cachedDryGain = 0.0f;
};
