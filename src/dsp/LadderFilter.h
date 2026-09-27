#pragma once
#include <JuceHeader.h>
#include <cmath>
#include "BlockParams.h"   // LadderResoLaw

/**
 * Zero-delay-feedback (ZDF / TPT) 4-pole transistor-ladder filter.
 *
 * History: this was previously the Huovilainen DAFx-04 cascade with a *delayed*
 * resonance feedback (`fbIn = halfIn − k·y4`, where y4 was last sample's value —
 * one full sample of delay in the loop) plus a half-sample input-averaging
 * compensation. That structure has a well-documented flaw: the unit delay makes
 * the per-stage attenuation at the resonance frequency drift away from 3 dB as
 * cutoff rises, so a fixed feedback k cannot hold the resonant peak — the peak
 * collapses toward Nyquist (Huovilainen 2004; Stilson & Smith 1996). Measured on
 * this exact code: a strong +24 dB peak at 100 Hz fell to ~0 dB at 3.2 kHz and
 * went negative (no peak at all) above 6 kHz at full resonance.
 *
 * The cure is to resolve the feedback *within the same sample* (no unit delay):
 * the TPT/ZDF ladder. Each one-pole is a topology-preserving-transform
 * integrator with a prewarped coefficient g = tan(π·fc/fs), so cutoff lands
 * exactly and the resonance (which sits at cutoff) is not detuned. The global
 * resonance loop is then closed algebraically each sample.
 *
 * Reference: Zavalishin, "The Art of VA Filter Design" §§5.1–5.3 (TPT ladder,
 * eq. 5.8); Pirkle Addendum A11 (identical α0 = 1/(1 + K·G⁴) delay-free loop);
 * D'Angelo & Välimäki 2014, IEEE/ACM TASLP — delay-free loop gives "perfect
 * decoupling between cut-off frequency and resonance controls."
 *
 * ZDF derivation (linear case). One TPT lowpass has output y = G1·x + z where
 * G1 = g/(1+g) is the per-stage instantaneous gain and z = (1−G1)·s is the
 * state feed-forward. Cascading four identical stages:
 *     y4 = G·u + S,   G = G1⁴,   S = G1³z1 + G1²z2 + G1z3 + z4.
 * With linear feedback u = x − k·y4 this solves (Zavalishin eq. 5.8) to
 *     u = (x − k·S) / (1 + k·G).
 * The denominator (1 + k·G) ≥ 1 for k ≥ 0, so the loop is unconditionally
 * stable (no instantaneous blow-up; Zavalishin: stable for k ≥ −1).
 *
 * Drive topology (unchanged): in a real ladder, gain is applied *before* the
 * ladder, each saturating stage shapes that hot signal — a single tanh *before*
 * the filter would flat-clip and leave nothing for the stages to shape. So this
 * filter takes `setInputDrive(gain)` separately; upstream code feeds the
 * `filter_drive` knob in here and skips the pre-filter tanh when the ladder is
 * active.
 *
 * Thermal-voltage-normalised per-stage saturation (Huovilainen / Surge):
 * `satStage(x) = 2·Vt · tanh(x / (2·Vt))` instead of plain tanh. This keeps the
 * slope at zero = 1 (so k = 4 is still the self-oscillation threshold) while
 * widening each stage's ceiling to ±2·Vt — enough headroom for the feedback to
 * keep ringing the first stage through its linear region at high drive (the
 * the classic overdriven-ladder roar; see docs/handover_session16_filter_drive.md).
 * With satStage as
 * identity the ZDF resolution above is exact; with it active, the linearised
 * feedback is the standard bounded nonlinear-ZDF approximation — the loop stays
 * stable (denominator ≥ 1, tanh bounded) and self-limits under heavy saturation,
 * which is the analog-correct behaviour.
 *
 * Slope switch picks which ladder tap is the output (y1 = 6, y2 = 12, y3 = 18,
 * y4 = 24 dB/oct). HP = input − LP tap, BP = y2 − y4.
 *
 * API mirrors T5ynthFilter so SynthVoice treats all filter models interchangeably.
 */
class LadderFilter
{
public:
    void prepare(double sampleRate, int /*samplesPerBlock*/)
    {
        sr = sampleRate;
        reset();
        updateCoeffs();
    }

    void reset()
    {
        s1 = s2 = s3 = s4 = 0.0f;
        y1 = y2 = y3 = y4 = 0.0f;
    }

    // `force` skips the dead-band test below -- for a caller that approached
    // this value in steps the dead-band may have swallowed, and must land it
    // exactly.
    void setCutoff(float hz, bool force = false)
    {
        if (! force && std::abs(hz - lastCutoff) < 0.5f) return;
        lastCutoff = hz;
        updateCoeffs();
    }

    // Resonance 0..1 → feedback gain. The knob travels evenly in DECIBELS of
    // peak height, not in feedback gain: the ladder self-oscillates at k = 4
    // (Zavalishin §5.2: 1/|G(jω_c)| = 1/(1/4) = 4), so a knob linear in k spends
    // its whole first half below +1 dB and its last eighth climbing 40. See
    // LadderResoLaw in BlockParams.h for the inversion and the measurement.
    // Unlike the old delayed-feedback structure, the threshold is cutoff-
    // independent — no per-frequency boost needed.
    void setResonance(float r)
    {
        if (std::abs(r - lastReso) < 0.001f) return;
        lastReso = juce::jlimit(0.0f, 1.0f, r);
        k = LadderResoLaw::feedback(lastReso, LadderResoLaw::kLadderPole);
    }

    // currentType: 0 = LP, 1 = HP, 2 = BP
    void setType(int type)    { currentType  = juce::jlimit(0, 2, type); }
    void setSlope(int slope)  { currentSlope = juce::jlimit(0, 3, slope); }
    void setMix(float mix)    { currentMix   = juce::jlimit(0.0f, 1.0f, mix); }

    // Pre-filter input gain. The ladder's four tanh stages see the hot signal
    // and saturate on it; that's the audible drive character. No output comp
    // is applied: on a real ladder, drive makes the filter louder + crunchier,
    // not level-matched. Master volume handles overall level.
    void setInputDrive(float gain)
    {
        inDrive = juce::jmax(0.0001f, gain);
    }

    float processSample(float input)
    {
        if (currentMix < 0.001f)
            return input;

        // Pre-filter gain (the ladder's own nonlinearities are the drive).
        const float hot = input * inDrive;

        // ── Zero-delay feedback resolution ──
        // Close the resonance loop within this sample using the current TPT
        // states. G1 is the per-stage instantaneous gain; z_i the per-stage
        // state feed-forward. G = G1⁴ and S = Σ G1^(4-i)·z_i are the linear
        // cascade's instantaneous gain and zero-input response, so the loop
        // input u = (hot − k·S)/(1 + k·G) (Zavalishin eq. 5.8). No unit delay
        // in the loop ⇒ the resonant peak no longer collapses with cutoff.
        const float G1 = gFrac;
        const float z1 = (1.0f - G1) * s1;
        const float z2 = (1.0f - G1) * s2;
        const float z3 = (1.0f - G1) * s3;
        const float z4 = (1.0f - G1) * s4;
        const float G  = G1 * G1 * G1 * G1;
        const float S  = (G1 * G1 * G1) * z1 + (G1 * G1) * z2 + G1 * z3 + z4;
        const float u  = (hot - k * S) / (1.0f + k * G);   // denom ≥ 1 ⇒ stable

        // Forward pass: four TPT one-poles with thermal-voltage saturation at
        // each stage input. satStage keeps the ladder ringing at high drive
        // instead of going silent (see class comment); with it ≈ identity the
        // feedback above is exact, with it active it self-limits cleanly.
        y1 = tptStep(s1, satStage(u));
        y2 = tptStep(s2, satStage(y1));
        y3 = tptStep(s3, satStage(y2));
        y4 = tptStep(s4, satStage(y3));

        // Denormal guard: add & subtract a tiny offset on the integrator state
        // so sub-normal numbers can't slow the CPU to a crawl in long decays.
        constexpr float antiDenormal = 1.0e-20f;
        s1 += antiDenormal;  s1 -= antiDenormal;
        s2 += antiDenormal;  s2 -= antiDenormal;
        s3 += antiDenormal;  s3 -= antiDenormal;
        s4 += antiDenormal;  s4 -= antiDenormal;

        // Flat output makeup for level parity with the SVF and the prior
        // (delayed-feedback) Ladder. The ZDF reformulation dropped the half-
        // sample averaging, so its old 1.20× tap-comp purpose (offsetting the
        // cos(ω/2) FIR loss) is gone — but the measured deep-passband level now
        // sits ~1.5 dB below the previous Ladder, which users A/B against. This
        // makeup is a *flat* gain: it scales peak and passband equally, so the
        // resonance prominence is unchanged — purely a loudness match.
        constexpr float kOutComp = 1.20f;
        const float wet = tapOutput(hot) * kOutComp;

        if (currentMix > 0.999f)
            return wet;
        return input * (1.0f - currentMix) + wet * currentMix;
    }

private:
    // Thermal-voltage scale for the per-stage saturation. Vt = 0.5 recovers
    // plain tanh exactly; raising Vt widens the per-stage ceiling to ±2·Vt and
    // therefore the feedback tap's authority at high drive. Surge XT's
    // VintageLadders uses Vt ≈ 1.22. Tune by ear against the acceptance matrix
    // in docs/handover_session16_filter_drive.md §8.
    //
    // NOTE: CutoffWarpFilter.h has its own copy of this constant for its Tanh
    // style. Keep the two in sync — if you change kVt here, change it there too.
    static constexpr float kVt = 1.22f;

    static inline float satStage(float x)
    {
        constexpr float k2Vt    = 2.0f * kVt;
        constexpr float kInv2Vt = 1.0f / (2.0f * kVt);
        return k2Vt * std::tanh(x * kInv2Vt);
    }

    void updateCoeffs()
    {
        // Prewarped TPT integrator gain g = tan(π·fc/fs). tan() blows up toward
        // Nyquist, so clamp ω to just below π/2. gFrac = g/(1+g) = the one-pole
        // instantaneous gain (G1) used by tptStep and the ZDF resolution.
        const double w = juce::MathConstants<double>::pi
                         * static_cast<double>(lastCutoff) / sr;
        const double wClamped = juce::jlimit(1.0e-4,
                                             juce::MathConstants<double>::halfPi - 1.0e-4,
                                             w);
        g     = static_cast<float>(std::tan(wClamped));
        gFrac = g / (1.0f + g);
    }

    // One topology-preserving-transform one-pole lowpass step. Updates state s,
    // returns the stage output y = G1·x + (1−G1)·s.
    float tptStep(float& s, float x)
    {
        const float v = (x - s) * gFrac;
        const float y = v + s;
        s = y + v;
        return y;
    }

    float tapOutput(float driven) const
    {
        const float lp = (currentSlope == 0) ? y1
                       : (currentSlope == 1) ? y2
                       : (currentSlope == 2) ? y3
                       :                       y4;
        switch (currentType)
        {
            case 0: return lp;
            case 1: return driven - lp;   // HP on the driven input so drive
                                          // stays in the HP path too
            case 2: return (y2 - y4);
            default: return lp;
        }
    }

    double sr = 44100.0;

    float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;  // TPT integrator states
    float y1 = 0.0f, y2 = 0.0f, y3 = 0.0f, y4 = 0.0f;  // stage outputs (for taps)

    float g     = 0.0f;
    float gFrac = 0.0f;
    float k     = 0.0f;
    float inDrive = 1.0f;
    float lastCutoff = 20000.0f;
    float lastReso   = 0.0f;
    float currentMix = 1.0f;
    int   currentType  = 0;
    int   currentSlope = 3;
};
