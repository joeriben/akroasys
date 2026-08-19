#include "DriftLFO.h"
#include "BlockParams.h"   // ModCalib:: — the cutoff bus's depth law

float DriftLFO::nextRandom(InternalLFO& lfo)
{
    lfo.rngState ^= lfo.rngState << 13;
    lfo.rngState ^= lfo.rngState >> 7;
    lfo.rngState ^= lfo.rngState << 17;
    return static_cast<float>(static_cast<int64_t>(lfo.rngState))
         * (1.0f / 9223372036854775808.0f);
}

float DriftLFO::waveformValue(const InternalLFO& lfo)
{
    double p = lfo.phase - std::floor(lfo.phase); // normalize to 0-1
    switch (lfo.waveform)
    {
        case Sine:
            return static_cast<float>(std::sin(p * TWO_PI));
        case Triangle:
            if (p < 0.25) return static_cast<float>(p * 4.0);
            if (p < 0.75) return static_cast<float>(2.0 - p * 4.0);
            return static_cast<float>(p * 4.0 - 4.0);
        case Square:
            return p < 0.5 ? 1.0f : -1.0f;
        case Sawtooth:
            // True rising sawtooth: linear ramp -1..+1, hard reset down.
            return static_cast<float>(2.0 * p - 1.0);
        case SawDown:
            // Inverse (falling) sawtooth: linear ramp +1..-1, hard reset up.
            return static_cast<float>(1.0 - 2.0 * p);
        case SampleHold:
            return lfo.heldValue;
        default:
            return static_cast<float>(std::sin(p * TWO_PI));
    }
}

float DriftLFO::halfRangeForTarget(int target)
{
    switch (target)
    {
        case TgtAlpha:     return 2.0f;   // alpha: [-2, 2]
        case TgtAxis1:     return 2.0f;   // sem_axis_1
        case TgtAxis2:     return 2.0f;   // sem_axis_2
        case TgtAxis3:     return 2.0f;   // sem_axis_3
        case TgtWtScan:    return 0.5f;   // wt_scan: [0, 1]
        case TgtFilter:    return 1.0f;   // filter cutoff: normalized — the cutoff bus owns the octave scale (ModCalib::kCutoffModOctaves)
        case TgtPitch:     return 1.0f;   // pitch: normalized — the pitch bus owns the semitone scale (ModCalib::kPitchModSemitones)
        case TgtDelayTime: return 0.5f;   // delay time: ±50% of base
        case TgtDelayFb:   return 0.3f;   // delay feedback: ±30%
        case TgtDelayMix:  return 0.5f;   // delay mix: ±0.5
        case TgtReverbMix: return 0.5f;   // reverb mix: ±0.5
        case TgtEnv1Amt:   return 0.5f;   // ENV1 amount: ±50%
        case TgtEnv2Amt:   return 0.5f;   // ENV2 amount: ±50%
        case TgtEnv3Amt:   return 0.5f;   // ENV3 amount: ±50%
        case TgtEnv4Amt:   return 0.5f;   // ENV4 amount: ±50%
        case TgtEnv5Amt:   return 0.5f;   // ENV5 amount: ±50%
        case TgtNoise:     return 0.5f;   // noise sigma: ±0.5 (range 0–1)
        case TgtMagnitude: return 1.0f;   // magnitude: ±1.0 (range 0.001–5)
        case TgtResynth:   return 0.5f;   // resynth amount: ±0.5 (range 0–1)
        default:           return 0.0f;   // None
    }
}

float DriftLFO::depthForTarget(int target, float depth)
{
    // Only the cutoff destination curves its depth controls; every other target
    // takes the knob position as it stands.
    if (target == TgtFilter)
        return depth * ModCalib::cutoffDepthCurve(depth);
    return depth;
}

void DriftLFO::reset()
{
    for (auto& lfo : lfos)
    {
        lfo.phase = 0.0;
        lfo.heldValue = nextRandom(lfo);
    }
}

void DriftLFO::tick(double dt)
{
    if (!active)
        return;

    for (auto& lfo : lfos)
    {
        // depth is NOT part of this gate: with LFO->DriftAmt modulation, depth
        // is now dynamic and bipolar-crosses zero every cycle — gating the
        // phase clock on it would stall the Drift LFO for whatever fraction
        // of the cycle depth reads zero, silently halving its effective rate.
        // The main LFOs already free-run regardless of their own Amount knob
        // (see the deep-idle advancePhase calls in processBlock); this mirrors
        // that.
        if (lfo.target == 0) // target 0 = None
            continue;

        if (lfo.armed)   // beat-sync hold: stay at phase 0 until released
            continue;

        // Offline cache take: the generation-side LFOs stand still here and are
        // moved by stepGenerationTargets() instead, one defined step per captured
        // entry. Everything audible keeps running.
        if (genHold && isGenerationTarget(lfo.target))
            continue;

        advancePhase(lfo, dt);
    }
}

void DriftLFO::stepGenerationTargets(double dt)
{
    if (!active || dt <= 0.0)
        return;

    for (auto& lfo : lfos)
    {
        if (lfo.target == 0)   // target 0 = None
            continue;

        // An armed slot is parked at phase 0 until its downbeat and contributes
        // nothing, so there is nothing to carry forward — it starts its cycle at 0
        // when the sequencer releases it, which is what arming means. Arming can only
        // begin mid-take through a clock-mode change, and that changes the slot's
        // rate anyway: the take's exactness is over one setting, not across an edit.
        if (lfo.armed)
            continue;

        if (! isGenerationTarget(lfo.target))
            continue;

        advancePhase(lfo, dt);
    }
}

void DriftLFO::advancePhase(InternalLFO& lfo, double dt)
{
    lfo.phase += static_cast<double>(lfo.rate) * dt;

    // Wrap phase to prevent precision loss
    if (lfo.phase >= 1.0)
    {
        lfo.phase -= std::floor(lfo.phase);
        if (lfo.waveform == SampleHold)
            lfo.heldValue = nextRandom(lfo);
    }
}

float DriftLFO::getOffsetForTarget(int target) const
{
    if (!active || target <= 0 || target > NumTargets)
        return 0.0f;

    float totalOffset = 0.0f;
    const float hr = halfRangeForTarget(target);

    for (const auto& lfo : lfos)
    {
        if (lfo.armed)   // armed for beat-sync → contributes nothing yet
            continue;

        if (lfo.target == target && lfo.depth != 0.0f)
        {
            float value = waveformValue(lfo);
            totalOffset += value * depthForTarget(target, lfo.depth) * hr;
        }
    }

    return totalOffset;
}

float DriftLFO::getReachForTarget(int target) const
{
    // Same terms as getOffsetForTarget, with waveformValue at its bound instead
    // of its current value: every waveform above is confined to [-1, 1] (the
    // Sample&Hold branch included — nextRandom is int64/2^63).
    if (!active || target <= 0 || target > NumTargets)
        return 0.0f;

    float total = 0.0f;
    const float hr = halfRangeForTarget(target);

    for (const auto& lfo : lfos)
    {
        if (lfo.armed)   // armed for beat-sync → contributes nothing yet
            continue;

        if (lfo.target == target)
            total += std::abs(depthForTarget(target, lfo.depth)) * hr;
    }

    return total;
}

void DriftLFO::setLfoRate(int lfoIndex, float hz)
{
    if (lfoIndex >= 0 && lfoIndex < NUM_LFOS)
        lfos[static_cast<size_t>(lfoIndex)].rate = hz;
}

void DriftLFO::setLfoDepth(int lfoIndex, float amount)
{
    if (lfoIndex >= 0 && lfoIndex < NUM_LFOS)
        lfos[static_cast<size_t>(lfoIndex)].depth = amount;
}

void DriftLFO::setLfoTarget(int lfoIndex, int target)
{
    if (lfoIndex >= 0 && lfoIndex < NUM_LFOS)
    {
        auto& lfo = lfos[static_cast<size_t>(lfoIndex)];
        if (lfo.target != target)
        {
            lfo.phase = 0.0; // Phase reset on target change (reference behavior)
            if (lfo.waveform == SampleHold)
                lfo.heldValue = nextRandom(lfo);
            lfo.target = target;
        }
    }
}

void DriftLFO::resetLfoPhase(int lfoIndex)
{
    if (lfoIndex >= 0 && lfoIndex < NUM_LFOS)
    {
        auto& lfo = lfos[static_cast<size_t>(lfoIndex)];
        lfo.phase = 0.0;
        if (lfo.waveform == SampleHold)
            lfo.heldValue = nextRandom(lfo);
    }
}

void DriftLFO::setLfoArmed(int lfoIndex, bool armed)
{
    if (lfoIndex >= 0 && lfoIndex < NUM_LFOS)
    {
        auto& lfo = lfos[static_cast<size_t>(lfoIndex)];
        lfo.armed = armed;
        if (armed)
            lfo.phase = 0.0;
    }
}

void DriftLFO::setLfoWaveform(int lfoIndex, int waveform)
{
    if (lfoIndex >= 0 && lfoIndex < NUM_LFOS)
    {
        auto& lfo = lfos[static_cast<size_t>(lfoIndex)];
        if (lfo.waveform != waveform)
        {
            lfo.waveform = waveform;
            lfo.phase = 0.0;
            if (lfo.waveform == SampleHold)
                lfo.heldValue = nextRandom(lfo);
        }
    }
}
