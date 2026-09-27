#include "StateVariableFilter.h"

void T5ynthFilter::prepare(double sampleRate, int samplesPerBlock)
{
    sr = sampleRate;
    blockSize = samplesPerBlock;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    spec.numChannels = 2;

    filter1.prepare(spec);
    filter1.setCutoffFrequency(20000.0f);
    filter1.setResonance(resonanceToQ(0.0f)); // Q=0.5 at reso=0

    filter2.prepare(spec);
    filter2.setCutoffFrequency(20000.0f);
    filter2.setResonance(resonanceToQ(0.0f));

    prepared = true;
    lastSetCutoff = 20000.0f;
    lastSetReso = 0.0f;
    // The one-pole coefficient must match lastSetCutoff at THIS rate: setCutoff skips a cutoff
    // within 0.5 Hz of it, and 20000 Hz is where the cutoff parameter and every upward modulation
    // clamp. Without this a fresh instance kept coefficient 0 there (at 6 and 18 dB the LP silent,
    // at 6 dB the HP passing everything), and a re-prepared one the old rate's coefficient.
    updateOnePoleCoeff(lastSetCutoff);

    // Initialize cached mix gains
    const float halfPi = juce::MathConstants<float>::halfPi;
    cachedWetGain = std::sin(currentMix * halfPi);
    cachedDryGain = std::cos(currentMix * halfPi);
}

void T5ynthFilter::processBlock(juce::AudioBuffer<float>& buffer)
{
    if (!prepared)
        return;

    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();

    // Bypass: mix=0 means fully dry
    if (currentMix < 0.001f)
        return;

    // Equal-power crossfade gains
    const float halfPi = juce::MathConstants<float>::halfPi;
    const float wetGain = std::sin(currentMix * halfPi);
    const float dryGain = std::cos(currentMix * halfPi);

    // Fully wet: no need to keep dry copy
    if (dryGain < 0.001f)
    {
        juce::dsp::AudioBlock<float> block(buffer);
        juce::dsp::ProcessContextReplacing<float> context(block);
        filter1.process(context);

        if (currentSlope == 1)
        {
            juce::dsp::ProcessContextReplacing<float> context2(block);
            filter2.process(context2);
        }
        return;
    }

    // Mixed: process wet path into temp, blend with dry
    // Save dry copy
    juce::AudioBuffer<float> dryBuffer(numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        dryBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

    // Filter the buffer (wet path)
    juce::dsp::AudioBlock<float> block(buffer);
    juce::dsp::ProcessContextReplacing<float> context(block);
    filter1.process(context);

    if (currentSlope == 1)
    {
        juce::dsp::ProcessContextReplacing<float> context2(block);
        filter2.process(context2);
    }

    // Equal-power blend: output = dry * dryGain + wet * wetGain
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* wet = buffer.getWritePointer(ch);
        const auto* dry = dryBuffer.getReadPointer(ch);

        for (int i = 0; i < numSamples; ++i)
            wet[i] = dry[i] * dryGain + wet[i] * wetGain;
    }
}

void T5ynthFilter::reset()
{
    filter1.reset();
    filter2.reset();
    onePoleState = 0.0f;  // filter1/filter2 don't cover the 6dB/18dB one-pole stage
}

void T5ynthFilter::takeStagesFrom (const T5ynthFilter& src, bool stage1, bool stage2, bool onePole)
{
    if (stage1)  filter1 = src.filter1;
    if (stage2)  filter2 = src.filter2;
    if (onePole) onePoleState = src.onePoleState;
}

void T5ynthFilter::setMix(float mix)
{
    mix = juce::jlimit(0.0f, 1.0f, mix);
    if (mix == currentMix) return;
    currentMix = mix;

    // Pre-compute crossfade gains (avoids sin/cos per sample)
    const float halfPi = juce::MathConstants<float>::halfPi;
    cachedWetGain = std::sin(currentMix * halfPi);
    cachedDryGain = std::cos(currentMix * halfPi);
}

void T5ynthFilter::setCutoff(float hz)
{
    // Skip redundant coefficient update (std::tan is expensive)
    if (std::abs(hz - lastSetCutoff) < 0.5f) return;
    lastSetCutoff = hz;
    filter1.setCutoffFrequency(hz);
    filter2.setCutoffFrequency(hz);
    updateOnePoleCoeff(hz);
}

void T5ynthFilter::setResonance(float r)
{
    // Skip redundant coefficient update
    if (std::abs(r - lastSetReso) < 0.001f) return;
    lastSetReso = r;
    float q = resonanceToQ(r);
    filter1.setResonance(q);
    filter2.setResonance(q);
}

void T5ynthFilter::setType(int type)
{
    if (type == currentType) return;
    currentType = type;

    auto juceType = juce::dsp::StateVariableTPTFilterType::lowpass;
    switch (type)
    {
        case 0: juceType = juce::dsp::StateVariableTPTFilterType::lowpass; break;
        case 1: juceType = juce::dsp::StateVariableTPTFilterType::highpass; break;
        case 2: juceType = juce::dsp::StateVariableTPTFilterType::bandpass; break;
        default: break;
    }

    filter1.setType(juceType);
    filter2.setType(juceType);
}

void T5ynthFilter::setSlope(int slope)
{
    currentSlope = juce::jlimit(0, 3, slope);
}

void T5ynthFilter::updateOnePoleCoeff(float cutoffHz)
{
    // RC low-pass: coeff = 1 - e^(-2pi * fc / sr)
    double wc = juce::MathConstants<double>::twoPi * static_cast<double>(cutoffHz) / sr;
    onePoleCoeff = static_cast<float>(1.0 - std::exp(-wc));
}

float T5ynthFilter::processSample(float sample)
{
    if (!prepared || currentMix < 0.001f)
        return sample;

    float wet;
    switch (currentSlope)
    {
        case 0: // 6dB — one-pole only
            if (currentType == 0) // LP
            {
                onePoleState += onePoleCoeff * (sample - onePoleState);
                wet = onePoleState;
            }
            else if (currentType == 1) // HP
            {
                onePoleState += onePoleCoeff * (sample - onePoleState);
                wet = sample - onePoleState;
            }
            else // BP — use SVF for bandpass (no meaningful 1-pole BP)
            {
                wet = filter1.processSample(0, sample);
            }
            break;

        case 1: // 12dB — single SVF
            wet = filter1.processSample(0, sample);
            break;

        case 2: // 18dB — SVF + one-pole
            wet = filter1.processSample(0, sample);
            if (currentType == 0) {
                onePoleState += onePoleCoeff * (wet - onePoleState);
                wet = onePoleState;
            } else if (currentType == 1) {
                onePoleState += onePoleCoeff * (wet - onePoleState);
                wet = wet - onePoleState;
            }
            break;

        case 3: // 24dB — two SVFs cascaded
            wet = filter1.processSample(0, sample);
            wet = filter2.processSample(0, wet);
            break;

        default:
            wet = filter1.processSample(0, sample);
            break;
    }

    if (currentMix > 0.999f)
        return wet;

    return sample * cachedDryGain + wet * cachedWetGain;
}
