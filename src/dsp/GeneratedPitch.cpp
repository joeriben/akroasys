#include "GeneratedPitch.h"
#include "Tuning.h"
#include "WavetableOscillator.h"
#include "../inference/RepromptStances.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

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

    // ── Band-limited interpolation ──────────────────────────────────────────
    // Smith & Gossett, "A flexible sampling-rate conversion method", ICASSP
    // 1984; J. O. Smith, "Digital Audio Resampling Home Page", CCRMA. Output
    // sample m is the input reconstructed at t = m * speed through a windowed
    // sinc whose cutoff is min(1, 1/speed): above unity the cutoff falls with
    // the speed, so what the faster reading would fold back is removed first.
    //
    // The window is Kaiser's (Kaiser 1974; Oppenheim & Schafer, Discrete-Time
    // Signal Processing, eqs. 7.62 and 7.63): beta from the stopband, and the
    // transition width from beta and the length. The cutoff is pulled in by
    // that half-width, so the stopband starts at the new Nyquist and the
    // images of the top octave land in it.
    constexpr int    kZeroCrossings = 64;     // per side of the prototype sinc
    constexpr int    kPerCrossing   = 512;    // table points per zero crossing, linear in between
    constexpr double kStopbandDb    = 90.0;

    double kaiserBeta() { return 0.1102 * (kStopbandDb - 8.7); }

    /** Transition width of the prototype, in cycles per sample of the prototype's
     *  own rate (its zeros at the integers, cutoff 0.5): order M = 2 Z, and
     *  M = (A - 8) / (2.285 dw) with dw = 2 pi df. */
    double transitionWidth()
    {
        return (kStopbandDb - 8.0) / (2.285 * 2.0 * juce::MathConstants<double>::pi
                                      * 2.0 * kZeroCrossings);
    }

    /** The cutoff that puts the stopband edge exactly at the Nyquist. */
    double rolloff() { return 0.5 / (0.5 + 0.5 * transitionWidth()); }

    double besselI0(double x)
    {
        // Power series; terms fall off fast for the beta used here.
        double sum = 1.0, term = 1.0;
        const double q = 0.25 * x * x;
        for (int k = 1; k < 64; ++k)
        {
            term *= q / (static_cast<double>(k) * static_cast<double>(k));
            sum += term;
            if (term < 1.0e-12 * sum)
                break;
        }
        return sum;
    }

    // Never destroyed: a detached generation thread can still be reading the
    // table while the app quits and runs static destructors.
    const std::vector<float>& prototype()
    {
        static const auto* table = []
        {
            const int n = kZeroCrossings * kPerCrossing;
            auto* t = new std::vector<float>(static_cast<size_t>(n + 2), 0.0f);
            const double beta = kaiserBeta();
            const double norm = besselI0(beta);
            for (int j = 0; j <= n; ++j)
            {
                const double u = static_cast<double>(j) / kPerCrossing;
                const double r = u / kZeroCrossings;
                const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm;
                const double px = juce::MathConstants<double>::pi * u;
                const double sinc = (j == 0) ? 1.0 : std::sin(px) / px;
                (*t)[static_cast<size_t>(j)] = static_cast<float>(sinc * w);
            }
            return t;
        }();
        return *table;
    }

    /** The prototype at |u| (in zero crossings), 0 beyond its end. */
    inline float prototypeAt(const std::vector<float>& h, double u)
    {
        const double pos = std::abs(u) * kPerCrossing;
        const int j = static_cast<int>(pos);
        if (j >= kZeroCrossings * kPerCrossing)
            return 0.0f;
        const float frac = static_cast<float>(pos - j);
        return h[static_cast<size_t>(j)] + frac * (h[static_cast<size_t>(j) + 1] - h[static_cast<size_t>(j)]);
    }
}

GeneratedPitch::Named GeneratedPitch::namedIn(const juce::String& promptA, const juce::String& promptB,
                                              int tuningType, int scaleRoot)
{
    Named named;
    const auto ab = Music::parseAB(promptA, promptB);
    for (const auto* spec : { &ab.a, &ab.b })
        for (const auto& note : spec->notes)
        {
            named.classes |= 1 << (((note.pitchClass % 12) + 12) % 12);
            if (note.midiNote >= 0 && note.midiNote < 128)   // "b10" is 143: a class, not a note
                named.notes.set(static_cast<size_t>(note.midiNote));
        }

    const auto type = static_cast<Tuning::Type>(juce::jlimit(0, static_cast<int>(Tuning::COUNT) - 1, tuningType));
    const int root = ((scaleRoot % 12) + 12) % 12;
    for (int pc = 0; pc < 12; ++pc)
    {
        // The pitch class as the synth plays it: the sampler sounds a buffer at
        // unity on MIDI 60 and every other key at tunedHz(key) / tunedHz(60),
        // so a buffer on the tuning's own pitch puts the keyboard in tune.
        const float tuned = Tuning::noteToHz(60 + pc, type, root);
        const float equal = Tuning::noteToHz(60 + pc, Tuning::Equal, 0);
        named.cents[static_cast<size_t>(pc)] = 1200.0f * std::log2(tuned / equal);
    }
    return named;
}

GeneratedPitch::Reading GeneratedPitch::read(const juce::AudioBuffer<float>& audio, double sampleRate)
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

int GeneratedPitch::nearestNamed(const Named& named, float midi, float& distance)
{
    int best = 0;
    distance = std::numeric_limits<float>::max();
    for (int pc = 0; pc < 12; ++pc)
    {
        if ((named.classes & (1 << pc)) == 0)
            continue;
        const float offset = named.cents[static_cast<size_t>(pc)] / 100.0f;
        const int note = pc + 12 * static_cast<int>(std::round((midi - static_cast<float>(pc) - offset) / 12.0f));
        const float d = std::abs(static_cast<float>(note) + offset - midi);
        if (d < distance)
        {
            distance = d;
            best = note;
        }
    }
    return best;
}

int GeneratedPitch::nearestWritten(const Named& named, float midi, float& distance)
{
    int best = -1;
    distance = std::numeric_limits<float>::max();
    for (int note = 0; note < 128; ++note)
    {
        if (! named.notes.test(static_cast<size_t>(note)))
            continue;
        const float d = std::abs(static_cast<float>(note)
                                 + named.cents[static_cast<size_t>(note % 12)] / 100.0f - midi);
        if (d < distance)
        {
            distance = d;
            best = note;
        }
    }
    return best;
}

bool GeneratedPitch::choose(const Named& named, const Reading& reading)
{
    // On a named note: the series takes the octave the sample plays in.
    float distance = 0.0f;
    int note = nearestNamed(named, reading.midi, distance);
    if (distance > kOnNamed)
    {
        // Off every named note, so the sample shows no octave of it: the note
        // as the prompt writes it, if the sample can reach it.
        note = nearestWritten(named, reading.midi, distance);
        if (note < 0 || distance > kReach)
            return false;
    }
    target_    = note;
    classes_   = named.classes;
    notes_     = named.notes;
    hasTarget_ = true;
    return true;
}

float GeneratedPitch::fit(const Named& named, const Reading& fresh)
{
    if (! named.any() || ! fresh.pitched)
        return 0.0f;

    // Other notes named than the target was chosen from: the series starts anew.
    if ((! hasTarget_ || classes_ != named.classes || notes_ != named.notes)
        && ! choose(named, fresh))
        return 0.0f;

    const int pc = ((target_ % 12) + 12) % 12;
    const float exact = static_cast<float>(target_) + named.cents[static_cast<size_t>(pc)] / 100.0f;
    const float semitones = exact - fresh.midi;
    if (std::abs(semitones) > kReach || std::abs(semitones) < kMinMove)
        return 0.0f;
    return semitones;
}

void GeneratedPitch::anchor(const Named& named, const Reading& current)
{
    // A sound that can take no target (no pitch, or out of reach of the named
    // note) says nothing about where the series sits, so the series keeps the
    // target it had: a cache take left as the model made it is still part of it.
    if (named.any() && current.pitched)
        choose(named, current);
}


juce::AudioBuffer<float> GeneratedPitch::move(const juce::AudioBuffer<float>& audio, float semitones)
{
    const int len = audio.getNumSamples();
    const int channels = audio.getNumChannels();
    // Input samples read per output sample: above 1 the sample sounds higher.
    const double speed = std::pow(2.0, static_cast<double>(semitones) / 12.0);
    const int outLen = speed > 1.0 ? static_cast<int>(std::ceil(len / speed)) : len;
    juce::AudioBuffer<float> out(channels, outLen);
    if (len == 0 || outLen == 0)
        return out;

    const auto& h = prototype();
    const double cutoff = std::min(1.0, 1.0 / speed) * rolloff();
    const double reach = kZeroCrossings / cutoff;   // input samples either side of t

    for (int ch = 0; ch < channels; ++ch)
    {
        const float* x = audio.getReadPointer(ch);
        float* y = out.getWritePointer(ch);
        for (int m = 0; m < outLen; ++m)
        {
            const double t = static_cast<double>(m) * speed;
            const int first = std::max(0, static_cast<int>(std::ceil(t - reach)));
            const int last  = std::min(len - 1, static_cast<int>(std::floor(t + reach)));
            double acc = 0.0;
            for (int i = first; i <= last; ++i)
                acc += static_cast<double>(x[i]) * prototypeAt(h, cutoff * (t - i));
            y[m] = static_cast<float>(cutoff * acc);
        }
    }
    return out;
}
