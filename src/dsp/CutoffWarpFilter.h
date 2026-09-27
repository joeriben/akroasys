#pragma once
#include <JuceHeader.h>
#include <cmath>
#include "BlockParams.h"   // LadderResoLaw

/**
 * Cutoff-Warp ladder filter.
 *
 * Surge-XT-style ZDF (zero-delay-feedback) 4-pole ladder with a per-pole
 * nonlinearity applied at the feedback input. The style selector picks which
 * saturation curve is used, opening a continuous character space beyond the
 * single tanh response of a classical transistor ladder.
 *
 * Reference topology: Surge XT src/common/dsp/filters/CutoffWarp.cpp (GPL-3).
 * Written from scratch against the standard ZDF-ladder derivation and Surge's
 * documented saturation curves — no code copied.
 *
 * Feedback resolution: like LadderFilter, the resonance loop is solved
 * within the sample — u = (x − kEff·S)/(1 + kEff·G), kEff = k·kStyleScale — so
 * there is no unit-delay over-resonance at high cutoff. (Earlier revisions used
 * a delayed y4 + half-sample input averaging; both are gone.)
 *
 * Each one-pole stage is a TPT (topology-preserving transform) integrator:
 *   v = (x − s) × g / (1 + g)
 *   y = v + s
 *   s_new = y + v
 * where g = tan(π · cutoff / sr), computed once per sub-block.
 *
 * Drive topology: same as LadderFilter — input gain before the ladder,
 * the style-selected saturation inside the loop does the shaping, output
 * compensation brings level back down. Upstream code skips the pre-filter
 * tanh when the Warp is the active algorithm and feeds `filter_drive` in
 * through `setInputDrive()` instead.
 *
 * Per-style resonance scaling: each saturation curve has a different local
 * slope near zero (Sin = π/2, Tanh = 1, SoftClip = 1 but drops fast, …) which
 * determines the small-signal loop gain at the resonance frequency. A style-
 * dependent k multiplier evens out the perceived "resonance strength" across
 * styles so Sin doesn't ring at r = 0.25 while SoftClip stays silent at r = 1.
 *
 * Thermal-voltage-normalised Tanh style (Huovilainen / Surge): style 0 uses
 * `satStage(x) = 2·Vt · tanh(x / (2·Vt))` instead of plain `tanh(x)`, which
 * widens the per-stage saturation ceiling to ±2·Vt. The feedback tap `k·y4`
 * therefore gets more headroom to swing the first-stage summing node across
 * its linear region when the hot signal is pinned at saturation, restoring
 * resonance / self-oscillation at high drive. See LadderFilter.h for the
 * derivation. Other styles keep their original curves on this first pass —
 * revisit per-style if the acceptance matrix fails for one of them.
 *
 * Saturation styles must all be monotonic and centered at 0 (sat(0) == 0)
 * so the feedback loop has a well-defined equilibrium. The algebraic sigmoid was previously
 * biased and caused DC drift / glitches under modulation; it now subtracts
 * the at-zero offset to guarantee sat(0) = 0. Digital-clip has a hard corner
 * that's deliberate — that's its character.
 */
class CutoffWarpFilter
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

    // Same law as LadderFilter — the knob travels evenly in dB of peak
    // height, not in feedback gain — but against THIS style's own pole, because
    // each saturation curve reaches unity loop gain at a different nominal k
    // (bisected: Tanh 3.9934 … Sin 3.8536; see LadderResoLaw in BlockParams.h).
    // The pole is measured in nominal k, before kStyleScale, so the law composes
    // with that scale rather than fighting it: what gets evened out is where the
    // travel starts to bite, and each style keeps its own compression above the
    // knee — that difference IS the style.
    void setResonance(float r)
    {
        if (std::abs(r - lastReso) < 0.001f) return;
        lastReso = juce::jlimit(0.0f, 1.0f, r);
        updateFeedback();
    }

    void setType(int type)    { currentType  = juce::jlimit(0, 2, type); }
    void setSlope(int slope)  { currentSlope = juce::jlimit(0, 3, slope); }
    void setMix(float mix)    { currentMix   = juce::jlimit(0.0f, 1.0f, mix); }
    void setStyle(int style)
    {
        currentStyle = juce::jlimit(0, 5, style);
        kStyleScale  = resonanceScaleForStyle(currentStyle);
        kStyleLevel  = levelCompForStyle(currentStyle);
        // The knob's law is style-dependent, so k does not survive a style
        // change. setResonance() early-outs on an unchanged knob and would
        // leave the previous style's feedback standing.
        updateFeedback();
    }

    // Input gain feeds the ZDF loop's style-selected saturations. No output
    // compensation: drive is intended to raise loudness + harmonic content,
    // not self-cancel. Master volume handles overall level.
    void setInputDrive(float gain)
    {
        inDrive = juce::jmax(0.0001f, gain);
    }

    float processSample(float input)
    {
        if (currentMix < 0.001f)
            return input;

        // Pre-filter gain. The ZDF reformulation resolves the resonance loop
        // within the sample (below), so the old half-sample input averaging is
        // gone — there is no unit delay left to compensate.
        const float hot = input * inDrive;

        // ── Zero-delay feedback resolution ──
        // Close the resonance loop within this sample from the current TPT
        // states, exactly as in LadderFilter: G = G1⁴, S = Σ G1^(4-i)·z_i
        // with z_i = (1−G1)·s_i, and u = (hot − kEff·S)/(1 + kEff·G). The
        // feedback gain is kEff = k·kStyleScale so resonance "strength" stays
        // consistent across saturation curves. No delayed y4 ⇒ no cutoff-
        // dependent over-resonance: the previous delayed-feedback structure
        // spiked toward self-oscillation at high cutoff even for moderate r.
        // Character + bounding still come from the per-style sat() in the
        // forward pass.
        const float kEff = k * kStyleScale;
        const float G1 = gFrac;
        const float z1 = (1.0f - G1) * s1;
        const float z2 = (1.0f - G1) * s2;
        const float z3 = (1.0f - G1) * s3;
        const float z4 = (1.0f - G1) * s4;
        const float G  = G1 * G1 * G1 * G1;
        const float S  = (G1 * G1 * G1) * z1 + (G1 * G1) * z2 + G1 * z3 + z4;
        const float u  = (hot - kEff * S) / (1.0f + kEff * G);  // denom ≥ 1 ⇒ stable

        y1 = tptStep(s1, sat(u,  currentStyle));
        y2 = tptStep(s2, sat(y1, currentStyle));
        y3 = tptStep(s3, sat(y2, currentStyle));
        y4 = tptStep(s4, sat(y3, currentStyle));

        // Denormal guard: add & subtract a tiny offset on the integrator state
        // so sub-normal numbers can't slow the CPU to a crawl in long decays.
        constexpr float antiDenormal = 1.0e-20f;
        s1 += antiDenormal;  s1 -= antiDenormal;
        s2 += antiDenormal;  s2 -= antiDenormal;
        s3 += antiDenormal;  s3 -= antiDenormal;
        s4 += antiDenormal;  s4 -= antiDenormal;

        // Flat output makeup for level parity with the SVF and the Ladder
        // (matches LadderFilter's kOutComp). Flat ⇒ peak/passband ratio
        // unchanged; purely a loudness match.
        constexpr float kOutComp = 1.20f;
        const float wet = tapOutput(hot) * kOutComp * kStyleLevel;

        if (currentMix > 0.999f)
            return wet;
        return input * (1.0f - currentMix) + wet * currentMix;
    }

private:
    void updateFeedback()
    {
        k = LadderResoLaw::feedback(lastReso, LadderResoLaw::warpPole(currentStyle));
    }

    void updateCoeffs()
    {
        // tan() blows up as we approach Nyquist, so clamp w to just below π/2.
        const double w = juce::MathConstants<double>::pi
                         * static_cast<double>(lastCutoff) / sr;
        const double wClamped = juce::jlimit(1.0e-4,
                                             juce::MathConstants<double>::halfPi - 1.0e-4,
                                             w);
        const double t = std::tan(wClamped);
        g     = static_cast<float>(t);
        gFrac = g / (1.0f + g);
    }

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
            case 1: return driven - lp;
            case 2: return (y2 - y4);
            default: return lp;
        }
    }

    // Per-style resonance scaling: evens out perceived "resonance strength"
    // across saturation curves so every style reaches edge-of-self-oscillation
    // at r ≈ 0.95 (matching Tanh) and stays tame at low r. Styles whose
    // derivative near zero is higher (Sin = π/2) reach self-oscillation at a
    // lower nominal k; styles whose derivative rolls off fast saturate earlier.
    //
    // RECALIBRATED for the zero-delay-feedback loop. The old values were tuned
    // for the previous delayed-feedback structure, whose loop gain was lower;
    // the true-ZDF loop is more effective, so the old scales self-oscillated far
    // too early (measured onsets: SoftClip r=0.71, Algebraic 0.86, Sin 0.24, Asym
    // never). These values were set by bisecting each style's measured self-
    // oscillation onset to r ≈ 0.93 (slightly below 1 so it still rings at
    // nominal level, where saturation raises the effective threshold). Final
    // fine-tuning is by ear against the acceptance matrix.
    static float resonanceScaleForStyle(int style)
    {
        switch (style)
        {
            case 0: return 1.00f;  // Tanh — reference (onset r≈0.95)
            case 1: return 1.04f;  // SoftClip
            case 2: return 1.02f;  // Algebraic
            case 3: return 0.17f;  // Sin — π/2 slope at zero rings hardest, needs least k
            case 4: return 1.02f;  // Digital
            case 5: return 1.31f;  // Asym — bias lowers effective gain, needs most k
        }
        return 1.0f;
    }

    // Per-style output loudness makeup, applied as a FLAT gain on the wet tap
    // (on top of kOutComp). Without it, switching styles jumps the level hard:
    // each saturation curve has a different small-/medium-signal throughput, so
    // at the same input two styles differ by up to ~18 dB (Sin's π/2 slope rings
    // +12.7 dB above Tanh; SoftClip's soft knee sits −5.1 dB below it).
    //
    // Values are Tanh/style measured at the cleanest reference — resonance 0,
    // no drive, nominal input (0.25) — via the offline RMS harness. That comp is
    // exactly constant across cutoff (verified 1–8 kHz), so a flat scalar is the
    // right tool. It is NOT constant across drive or self-oscillation: a flat
    // gain cannot also equalise the *large-signal* output of nonlinear curves
    // (a hard clip is genuinely louder than a soft fold when slammed; a strong
    // limit cycle is louder than a weak one). Those residual differences are the
    // saturators' intrinsic drive/self-osc character and are left intact —
    // equalising them would need a drive-dependent gain that flattens the very
    // thing that distinguishes the styles when pushed. This makeup removes the
    // gross baseline jump; it does not (and should not) null the drive character.
    static float levelCompForStyle(int style)
    {
        switch (style)
        {
            case 0: return 1.000f; // Tanh — reference
            case 1: return 1.797f; // SoftClip — +5.1 dB (soft knee → quiet)
            case 2: return 1.076f; // Algebraic — +0.6 dB
            case 3: return 0.233f; // Sin — −12.7 dB (π/2 slope → loud)
            case 4: return 0.990f; // Digital — ≈0 dB
            case 5: return 1.270f; // Asym — +2.1 dB
        }
        return 1.0f;
    }

    // Thermal-voltage scale for the Tanh style's saturation. Vt = 0.5 reduces
    // `satStage` to plain tanh; raising Vt widens the per-stage ceiling to
    // ±2·Vt and therefore the allowed swing of the feedback tap, which is
    // what keeps resonance alive at high drive.
    //
    // NOTE: LadderFilter.h has its own copy of this constant. Keep the
    // two in sync — if you change kVt here, change it there too. Both were
    // left local to keep each filter self-contained.
    static constexpr float kVt = 1.22f;

    static inline float satStage(float x)
    {
        constexpr float k2Vt    = 2.0f * kVt;
        constexpr float kInv2Vt = 1.0f / (2.0f * kVt);
        return k2Vt * std::tanh(x * kInv2Vt);
    }

    // Saturation functions indexed by FilterWarpStyle::*. All must be
    // monotonic, bounded, and sat(0) == 0 so the ZDF loop has a clean
    // zero equilibrium.
    static float sat(float x, int style)
    {
        switch (style)
        {
            case 0: // Tanh — thermal-voltage-normalised (see satStage / kVt)
                return satStage(x);

            case 1: // SoftClip — x / (1 + |x|), gentler than tanh
                return x / (1.0f + std::abs(x));

            case 2: // Algebraic sigmoid — x / sqrt(1 + x²), slower approach to ±1 than
                    // tanh, more odd-harmonic content for the same saturation.
                    // Kept centered at 0 (no DC bias) so the feedback loop
                    // can't drift; asymmetry lives in "Asym" (style 5).
                return x / std::sqrt(1.0f + x * x);

            case 3: // Sin — wave-folding, FM-like timbre
            {
                const float arg = juce::jlimit(-juce::MathConstants<float>::halfPi,
                                                juce::MathConstants<float>::halfPi,
                                                x * juce::MathConstants<float>::halfPi);
                return std::sin(arg);
            }

            case 4: // Digital — hard clamp, buzzy when driven
                return juce::jlimit(-1.0f, 1.0f, x);

            case 5: // Asym — asymmetric tanh with bias-compensation so sat(0)=0,
                    // retaining 2nd-harmonic content.
            {
                constexpr float bias    = 0.25f;
                constexpr float biasOff = 0.24491866f; // std::tanh(0.25)
                return std::tanh(x + bias) - biasOff;
            }
        }
        return std::tanh(x);
    }

    double sr = 44100.0;

    float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;
    float y1 = 0.0f, y2 = 0.0f, y3 = 0.0f, y4 = 0.0f;

    float g     = 0.0f;
    float gFrac = 0.0f;
    float k     = 0.0f;
    float kStyleScale = 1.0f;
    float kStyleLevel = 1.0f;   // per-style flat output makeup (levelCompForStyle)
    float inDrive = 1.0f;
    float lastCutoff = 20000.0f;
    float lastReso   = 0.0f;
    float currentMix = 1.0f;
    int   currentType  = 0;
    int   currentSlope = 3;
    int   currentStyle = 0;
};
