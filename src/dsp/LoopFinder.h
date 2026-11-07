#pragma once
#include <JuceHeader.h>
#include <vector>

/**
 * LoopFinder — seam-quality search for the sampler's forward loops and
 * ping-pong turnarounds. Off-thread only (allocates, O(n·k) searches).
 *
 * The method is the classical cascade used by sample editors, written here
 * from the published descriptions (docs/plans/SAMPLER_LOOP_FINDING_ANALYSIS.md
 * §4 names them; none of their code is used):
 *
 *   1. Candidate seam points are positive-going zero crossings — a splice can
 *      only be clean where both sides cross zero the same way.
 *   2. A seam is scored over a WINDOW, not a sample: the mismatch of value,
 *      slope and curvature at log-spaced offsets up to ±256 samples around the
 *      splice (the short-window AMDF family: Element Green/Swami, Polyphone).
 *   3. The two regions a crossfade mixes are scored by their normalised
 *      correlation over the fade length. For a periodic sound this is the
 *      integer-period constraint (Sony US 5,430,241) measured instead of
 *      imposed; for a texture it says how coherent the fade will be.
 *   4. For auto placement over a whole generation the sample-level search is
 *      preceded by a feature-level pass: log-band spectra per frame, chunk
 *      distance between the two regions the fade will mix (LoopMaker,
 *      loopweave, BGM-loop-detector all gate on spectral similarity first),
 *      because a 1.5–6 s texture loop needs head and tail to be the same KIND
 *      of sound before any sample alignment can help.
 *   5. The crossfade's gain law follows the measured correlation r: constant
 *      voltage at r = 1 (coherent, no +3 dB swell), constant power at r = 0
 *      (uncorrelated, no dip) — Bristow-Johnson / Niemitalo, music-dsp 2010.
 *   6. A ping-pong turnaround is scored by MIRROR correlation — x[c+τ] against
 *      x[c−τ] over the fade length — because time reversal negates every odd
 *      derivative and only local time-symmetry makes it smooth.
 *
 * All analysis runs on one channel (what the sampler plays: channel 0).
 */
namespace LoopFinder
{
    /** A forward-loop seam: play region [start, end). */
    struct Seam
    {
        int   start = 0;
        int   end   = 0;
        float seamError   = 1.0f;   // windowed value/slope/curvature mismatch, 0 = identical
        float correlation = 0.0f;   // r of the two regions the fade mixes, [-1, 1]
        float featureDistance = 0.0f; // coarse log-spectral distance (auto placement only)
        int   fade = 0;             // fade length actually available at this seam
        int   fadeSide = -1;        // -1: material BEFORE start fades into the tail (classical);
                                    // +1: material AFTER end fades into the head (start too
                                    //     close to the buffer start for the classical form)
        float rank = 1.0e9f;        // lower is better
        bool  valid = false;
    };

    /** A ping-pong turnaround point. */
    struct Turnaround
    {
        int   position = 0;
        float mirrorError = 1.0f;   // windowed mismatch of x[c+k] vs x[c-k]
        float correlation = 0.0f;   // mirror correlation over the fade length
        float rank = 1.0e9f;
        bool  valid = false;
    };

    /** Normalised correlation of a[i] and b[i], i in [0, len). 0 if either is silent. */
    float correlation(const float* a, const float* b, int len);

    /** Crossfade gains at position t in [0,1] for two signals of correlation r
     *  (clamped to [0,1]): gainIn rises 0→1, gainOut falls 1→0, and
     *  gainIn² + gainOut² + 2·r·gainIn·gainOut = 1 — constant power for the
     *  measured coherence (linear at r = 1, equal-power at r = 0). */
    void crossfadeGains(float t, float r, float& gainIn, float& gainOut);

    /** Score ONE forward-loop seam at exactly [start, end) with fade length
     *  fadeSamples. The fade mixes x[start-fade .. start) into x[end-fade .. end)
     *  when that much material precedes the start; otherwise (a loop starting
     *  at or near the buffer start) it mixes x[end .. end+fade) into
     *  x[start .. start+fade). The result's fade / fadeSide say which.
     *  Used when the markers are to be taken literally (optimizer Off). */
    Seam scoreSeam(const float* x, int n, int start, int end, int fadeSamples);

    /** Fine search: best seam with start in [startLo, startHi], end in [endLo, endHi],
     *  end - start in [minLen, maxLen], among positive-going zero crossings.
     *  Falls back to the raw markers (rawStart, rawEnd; the window centres when
     *  not given) when no crossing pair qualifies and the raw pair itself is
     *  within [minLen, maxLen]. lengthTarget > 0 adds a weak preference for
     *  loops near that length (shorter loops must earn it with a better seam). */
    Seam findSeam(const float* x, int n, int fadeSamples,
                  int startLo, int startHi, int endLo, int endHi,
                  int minLen, int maxLen, int lengthTarget = 0,
                  int rawStart = -1, int rawEnd = -1);

    /** Full cascade for auto placement: feature-level pass over [regionLo, regionHi)
     *  for region pairs of length [minLen, maxLen] whose fade regions are
     *  spectrally alike, then findSeam around each of the best coarse pairs.
     *  lengthTarget: preferred loop length (samples). */
    Seam findLoopInRegion(const float* x, int n, double sampleRate, int fadeSamples,
                          int regionLo, int regionHi, int minLen, int maxLen, int lengthTarget);

    /** Score ONE turnaround at exactly `position` with fade length fadeSamples
     *  (the fade mixes x[position ± τ], τ in [0, fade)). */
    Turnaround scoreTurnaround(const float* x, int n, int position, int fadeSamples);

    /** Best turnaround in [lo, hi] — every sample is a candidate; falls back to
     *  the raw marker (`centre`, the window centre when not given). A position
     *  near the buffer's ends is scored with the fade it has room for, the same
     *  truncation the runtime applies. */
    Turnaround findTurnaround(const float* x, int n, int fadeSamples, int lo, int hi, int centre = -1);

    /** Positive-going zero crossings in [lo, hi]: indices i with x[i-1] < 0 <= x[i]. */
    std::vector<int> zeroCrossings(const float* x, int n, int lo, int hi);
}
