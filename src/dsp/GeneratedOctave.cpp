#include "GeneratedOctave.h"
#include "WavetableOscillator.h"

#include <algorithm>
#include <cmath>

namespace
{
    // The clip-level settings of WavetableOscillator::prepareFramesFromBuffer.
    constexpr int   kWindow     = 4096;
    constexpr int   kHop        = kWindow / 2;
    constexpr float kConfidence = 0.9f;
    constexpr int   kMaxWindows = 256;
    // The two cut-offs the report measured on the bank (§5, §8): with them the
    // detector calls 1 of 480 bare control clips pitched, without them 161.
    constexpr float kVoicedShare   = 0.5f;
    constexpr float kConcentration = 0.5f;
    constexpr float kSameNote      = 0.5f;   // semitones

    float hzToMidi(float hz) { return 69.0f + 12.0f * std::log2(hz / 440.0f); }

    /** JUCE's halfband equiripple lowpass, as its 2x oversampler designs the
     *  first stage at maximum quality (juce_Oversampling.cpp: transition 0.10
     *  and 0.12 halved for stage 0, -90 dB going up, -75 dB going down). */
    std::vector<float> halfband(float transitionWidth, float stopbandDb)
    {
        auto coeffs = juce::dsp::FilterDesign<float>::designFIRLowpassHalfBandEquirippleMethod(
            transitionWidth, stopbandDb);
        const float* c = coeffs->getRawCoefficients();
        return std::vector<float>(c, c + coeffs->getFilterOrder() + 1);
    }

    // The tap tables are never destroyed: a detached generation thread can
    // still be reading one while the app quits and runs static destructors.
    const std::vector<float>& interpolationTaps()
    {
        static const auto* taps = new std::vector<float>(halfband(0.05f, -90.0f));
        return *taps;
    }

    struct Tap { int offset; float gain; };

    /** The decimation filter's nonzero taps only: a halfband is zero at every
     *  second tap but the centre, so this halves the work of octaveUp. */
    const std::vector<Tap>& decimationTaps()
    {
        static const auto* taps = []
        {
            const auto h = halfband(0.06f, -75.0f);
            auto* nonzero = new std::vector<Tap>();
            for (size_t j = 0; j < h.size(); ++j)
                if (h[j] != 0.0f)
                    nonzero->push_back({ static_cast<int>(j), h[j] });
            return nonzero;
        }();
        return *taps;
    }

    int decimationDelay()
    {
        static const int delay = static_cast<int>(halfband(0.06f, -75.0f).size() - 1) / 2;
        return delay;
    }

    /** Twice as many samples, of which the first outLen are made: zero-stuff,
     *  lowpass, gain 2. The filter's delay is taken out, so sample 2n of the
     *  result is sample n of the input. */
    juce::AudioBuffer<float> octaveDown(const juce::AudioBuffer<float>& in, int outLen)
    {
        const auto& h = interpolationTaps();
        const int taps  = static_cast<int>(h.size());
        const int delay = (taps - 1) / 2;
        const int len   = in.getNumSamples();
        outLen = juce::jlimit(0, len * 2, outLen);
        juce::AudioBuffer<float> out(in.getNumChannels(), outLen);
        for (int ch = 0; ch < in.getNumChannels(); ++ch)
        {
            const float* x = in.getReadPointer(ch);
            float* y = out.getWritePointer(ch);
            for (int m = 0; m < outLen; ++m)
            {
                // y[m] = sum_j h[j] * u[m + delay - j], u[2n] = 2 x[n], u[odd] = 0
                const int t0 = m + delay;
                double acc = 0.0;
                for (int j = t0 & 1; j < taps; j += 2)
                {
                    const int t = t0 - j;
                    if (t < 0) break;
                    const int n = t >> 1;
                    if (n < len)
                        acc += static_cast<double>(h[static_cast<size_t>(j)]) * x[n];
                }
                y[m] = static_cast<float>(2.0 * acc);
            }
        }
        return out;
    }

    /** Half as many samples: lowpass, keep every other. Sample n of the result
     *  is sample 2n of the input. */
    juce::AudioBuffer<float> octaveUp(const juce::AudioBuffer<float>& in)
    {
        const auto& h = decimationTaps();
        const int delay = decimationDelay();
        const int len   = in.getNumSamples();
        const int outLen = (len + 1) / 2;
        juce::AudioBuffer<float> out(in.getNumChannels(), outLen);
        for (int ch = 0; ch < in.getNumChannels(); ++ch)
        {
            const float* x = in.getReadPointer(ch);
            float* y = out.getWritePointer(ch);
            for (int n = 0; n < outLen; ++n)
            {
                const int t0 = 2 * n + delay;
                double acc = 0.0;
                for (const auto& tap : h)
                {
                    const int t = t0 - tap.offset;
                    if (t >= 0 && t < len)
                        acc += static_cast<double>(tap.gain) * x[t];
                }
                y[n] = static_cast<float>(acc);
            }
        }
        return out;
    }
}

GeneratedOctave::Reading GeneratedOctave::read(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    const int len = audio.getNumSamples();
    const int channels = audio.getNumChannels();
    if (len < kWindow || channels <= 0 || sampleRate <= 0.0)
        return {};

    // The mono mix, as the report measured (channel 0 alone gave the same
    // note on 396 of 400 stereo clips).
    std::vector<float> mono(static_cast<size_t>(len), 0.0f);
    const float invChannels = 1.0f / static_cast<float>(channels);
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* src = audio.getReadPointer(ch);
        for (int i = 0; i < len; ++i)
            mono[static_cast<size_t>(i)] += src[i] * invChannels;
    }

    // Every window up to kMaxWindows (an 11 s sample at 44.1 kHz has 235, so
    // everything the report measured is read whole); a longer sample is read
    // in evenly spaced windows, so a 120 s SA3 render costs what an 11 s one does.
    const int allWindows = (len - kWindow) / kHop + 1;
    const int stride = (allWindows + kMaxWindows - 1) / kMaxWindows;
    std::vector<float> candidates;
    int windows = 0;
    for (int start = 0; start + kWindow <= len; start += kHop * stride)
    {
        ++windows;
        const auto estimate = WavetableOscillator::analyzePitchWindow(mono.data() + start, kWindow, sampleRate);
        if (estimate.confidence >= kConfidence && estimate.hz > 20.0f && estimate.hz < 5000.0f)
            candidates.push_back(estimate.hz);
    }

    const int voiced = static_cast<int>(candidates.size());
    if (voiced < 2 || static_cast<float>(voiced) < kVoicedShare * static_cast<float>(windows))
        return {};

    std::sort(candidates.begin(), candidates.end());
    const float dominant = hzToMidi(candidates[candidates.size() / 2]);
    int near = 0;
    for (float hz : candidates)
        if (std::abs(hzToMidi(hz) - dominant) <= kSameNote)
            ++near;
    if (static_cast<float>(near) < kConcentration * static_cast<float>(voiced))
        return {};

    Reading reading;
    reading.pitched = true;
    reading.midi = dominant;
    return reading;
}

int GeneratedOctave::fit(const Reading& fresh)
{
    if (! fresh.pitched)
        return 0;

    int best = -1;
    int bestOctaves = 0;
    float bestResidual = kSameNote;
    for (size_t i = 0; i < notes_.size(); ++i)
    {
        const float distance = notes_[i] - fresh.midi;
        const float octaves  = std::round(distance / 12.0f);
        const float residual = std::abs(distance - 12.0f * octaves);
        // Out of reach does not count as a match, so it cannot hide one that is
        // in reach (two entries of one note name can sit more than 50 cents apart).
        if (residual <= bestResidual && std::abs(octaves) <= static_cast<float>(kMaxOctaves))
        {
            best = static_cast<int>(i);
            bestOctaves = static_cast<int>(octaves);
            bestResidual = residual;
        }
    }

    if (best < 0)
    {
        notes_.push_back(fresh.midi);   // no note in reach: as the model made it, and as it sounds
        return 0;
    }

    notes_[static_cast<size_t>(best)] = fresh.midi + 12.0f * static_cast<float>(bestOctaves);
    return bestOctaves;
}

void GeneratedOctave::anchor(const Reading& current)
{
    notes_.clear();
    if (current.pitched)
        notes_.push_back(current.midi);
}

juce::AudioBuffer<float> GeneratedOctave::moveOctaves(const juce::AudioBuffer<float>& audio, int octaves)
{
    juce::AudioBuffer<float> moved;
    moved.makeCopyOf(audio);
    for (int i = 0; i < octaves; ++i)
        moved = octaveUp(moved);

    // Down: the result keeps the input's length. Output m of a pass reads its
    // input no further than (m + delay) / 2, so a pass that feeds another needs
    // no more than len + taps of its own output; only those are made.
    const int len  = audio.getNumSamples();
    const int taps = static_cast<int>(interpolationTaps().size());
    for (int i = 0; i > octaves; --i)
    {
        const bool last = (i - 1 == octaves);
        const int outLen = last ? len : juce::jmin(moved.getNumSamples() * 2, len + taps);
        moved = octaveDown(moved, outLen);
    }
    return moved;
}
