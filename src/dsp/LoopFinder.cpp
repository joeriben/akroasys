#include "LoopFinder.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
    // Log-spaced offsets for the windowed seam measure: dense at the splice,
    // reaching ±256 samples (~6 ms at 44.1 kHz) so a slow partial's mismatch is
    // seen as well as a fast one's.
    constexpr int kOffsets[] = { 1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256 };
    constexpr int kNumOffsets = static_cast<int>(sizeof(kOffsets) / sizeof(kOffsets[0]));
    constexpr int kReach = 256 + 2;     // samples needed on each side of a scored point

    // Mismatch of the local waveform around positions p and q: value, first and
    // second difference, each normalised by `scale` (the peak of the material).
    inline float pointDifference(const float* x, int p, int q)
    {
        const float d0 = x[p] - x[q];
        const float d1 = (x[p] - x[p - 1]) - (x[q] - x[q - 1]);
        const float d2 = (x[p + 1] - 2.0f * x[p] + x[p - 1]) - (x[q + 1] - 2.0f * x[q] + x[q - 1]);
        return 0.5f * std::abs(d0) + 0.3f * std::abs(d1) + 0.2f * std::abs(d2);
    }

    // Windowed mismatch of the streams around a splice: x around `before` (the
    // side the playhead leaves) against x around `after` (the side it arrives
    // on), both directions of time, log-spaced. Requires kReach on each side.
    float windowedMismatch(const float* x, int before, int after)
    {
        float sum = pointDifference(x, before, after);
        for (int k = 0; k < kNumOffsets; ++k)
        {
            const int o = kOffsets[k];
            sum += pointDifference(x, before - o, after - o);
            sum += pointDifference(x, before + o, after + o);
        }
        return sum / static_cast<float>(1 + 2 * kNumOffsets);
    }

    // Windowed mismatch of x[c+k] against x[c-k]: local time-symmetry.
    float mirrorMismatch(const float* x, int c)
    {
        float sum = 0.0f;
        for (int k = 0; k < kNumOffsets; ++k)
        {
            const int o = kOffsets[k];
            // Compare the forward continuation with the reversed past at the same
            // distance: value, and slope with the sign flipped by reversal.
            const float d0 = x[c + o] - x[c - o];
            const float d1 = (x[c + o] - x[c + o - 1]) + (x[c - o] - x[c - o + 1]);
            sum += 0.6f * std::abs(d0) + 0.4f * std::abs(d1);
        }
        return sum / static_cast<float>(kNumOffsets);
    }

    float peakAbs(const float* x, int lo, int hi)
    {
        float p = 0.0f;
        for (int i = std::max(0, lo); i < hi; ++i) p = std::max(p, std::abs(x[i]));
        return p;
    }

    // Ranking weights. The seam measure and (1 - r) are both O(1); the length
    // term is a tiebreak only — a shorter loop must bring a clearly better seam.
    constexpr float kCorrelationWeight = 0.5f;
    constexpr float kLengthWeight      = 0.05f;

    float rankOf(float seamErrorNorm, float r, int len, int lengthTarget)
    {
        float rank = seamErrorNorm + kCorrelationWeight * (1.0f - juce::jlimit(-1.0f, 1.0f, r));
        if (lengthTarget > 0 && len > 0)
            rank += kLengthWeight * (static_cast<float>(lengthTarget) / static_cast<float>(len));
        return rank;
    }

    // ── Feature stage ────────────────────────────────────────────────────
    // Log-band spectra per frame. A frame of kFrameSize at hop kHop; kNumBands
    // bands spaced logarithmically between 60 Hz and 0.45·sr.
    constexpr int kFrameOrder = 10;              // 1024-point FFT
    constexpr int kFrameSize  = 1 << kFrameOrder;
    constexpr int kHop        = 512;
    constexpr int kNumBands   = 24;

    struct Features
    {
        int numFrames = 0;
        std::vector<float> data;    // numFrames × kNumBands, log energy
        const float* frame(int i) const { return data.data() + static_cast<size_t>(i) * kNumBands; }
    };

    Features computeFeatures(const float* x, int n, double sampleRate)
    {
        Features f;
        if (n < kFrameSize) return f;
        f.numFrames = 1 + (n - kFrameSize) / kHop;
        f.data.assign(static_cast<size_t>(f.numFrames) * kNumBands, 0.0f);

        juce::dsp::FFT fft(kFrameOrder);
        std::vector<float> buf(static_cast<size_t>(kFrameSize) * 2, 0.0f);
        std::vector<float> window(static_cast<size_t>(kFrameSize));
        for (int i = 0; i < kFrameSize; ++i)
            window[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * i / kFrameSize);

        // Band edges in bins, log-spaced.
        const double fLo = 60.0, fHi = 0.45 * sampleRate;
        std::vector<int> edges(static_cast<size_t>(kNumBands) + 1);
        for (int b = 0; b <= kNumBands; ++b)
        {
            const double hz = fLo * std::pow(fHi / fLo, static_cast<double>(b) / kNumBands);
            edges[static_cast<size_t>(b)] = juce::jlimit(1, kFrameSize / 2, static_cast<int>(hz / sampleRate * kFrameSize));
        }
        for (int b = 1; b <= kNumBands; ++b)
            edges[static_cast<size_t>(b)] = std::max(edges[static_cast<size_t>(b)], edges[static_cast<size_t>(b) - 1] + 1);

        for (int fr = 0; fr < f.numFrames; ++fr)
        {
            const float* src = x + fr * kHop;
            for (int i = 0; i < kFrameSize; ++i)
                buf[static_cast<size_t>(i)] = src[i] * window[static_cast<size_t>(i)];
            std::fill(buf.begin() + kFrameSize, buf.end(), 0.0f);
            fft.performFrequencyOnlyForwardTransform(buf.data());
            float* out = f.data.data() + static_cast<size_t>(fr) * kNumBands;
            for (int b = 0; b < kNumBands; ++b)
            {
                double e = 0.0;
                for (int k = edges[static_cast<size_t>(b)]; k < edges[static_cast<size_t>(b) + 1]; ++k)
                    e += static_cast<double>(buf[static_cast<size_t>(k)]) * buf[static_cast<size_t>(k)];
                out[b] = std::log10(static_cast<float>(e) + 1.0e-9f);
            }
        }
        return f;
    }

    // Mean squared log-spectral distance between the chunks of `frames` frames
    // ending at frame a and frame b.
    float chunkDistance(const Features& f, int aEnd, int bEnd, int frames)
    {
        float d = 0.0f;
        for (int i = 1; i <= frames; ++i)
        {
            const float* fa = f.frame(aEnd - i);
            const float* fb = f.frame(bEnd - i);
            for (int k = 0; k < kNumBands; ++k)
            {
                const float v = fa[k] - fb[k];
                d += v * v;
            }
        }
        return d / static_cast<float>(frames * kNumBands);
    }
}

namespace LoopFinder
{

float correlation(const float* a, const float* b, int len)
{
    double sab = 0.0, saa = 0.0, sbb = 0.0;
    for (int i = 0; i < len; ++i)
    {
        sab += static_cast<double>(a[i]) * b[i];
        saa += static_cast<double>(a[i]) * a[i];
        sbb += static_cast<double>(b[i]) * b[i];
    }
    const double denom = std::sqrt(saa * sbb);
    return denom > 1.0e-18 ? static_cast<float>(sab / denom) : 0.0f;
}

void crossfadeGains(float t, float r, float& gainIn, float& gainOut)
{
    t = juce::jlimit(0.0f, 1.0f, t);
    r = juce::jlimit(0.0f, 1.0f, r);
    const float a = t, b = 1.0f - t;
    const float power = a * a + b * b + 2.0f * r * a * b;   // > 0 for all t (a + b = 1)
    const float g = 1.0f / std::sqrt(power);
    gainIn  = a * g;
    gainOut = b * g;
}

std::vector<int> zeroCrossings(const float* x, int n, int lo, int hi)
{
    std::vector<int> out;
    lo = std::max(lo, 1);
    hi = std::min(hi, n - 1);
    for (int i = lo; i <= hi; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
            out.push_back(i);
    return out;
}

namespace
{
    // Broadband material has a crossing every few samples, so a wide window
    // can hold thousands of them and the start x end pair list grows with
    // their product (measured: 41 M pairs and gigabytes for a 120 s noise
    // take). A window with more crossings than kMaxCrossingsPerSide is
    // thinned to that many, and HOW matters, twice over:
    //  - never by an index stride: on a waveform with several crossings per
    //    period a stride keeps one phase class and can leave no in-phase
    //    start/end pair (measured on sin + 0.8 sin 2θ at 2 kHz: a
    //    half-period-off splice, err 0.5 where the full set has 0);
    //  - never as one run at the marker: a dense stretch beside the marker
    //    (a noisy attack) then eats the whole budget and clean crossings
    //    further out, still inside the window the user asked for, are never
    //    looked at (measured: err 0.19 where the full set has 0).
    // So the window is cut into equal TIME bins and each bin keeps a share
    // of the budget as one contiguous run at its centre: the runs cover the
    // whole window, and a run holds every phase class of anything with fewer
    // crossings per period than the run is long. Two more things the bins
    // must get right (both found by review, measured against the unbounded
    // search):
    //  - the START and END windows get DIFFERENT bin counts: with the same
    //    grid on both sides every surviving pair's length sits on a lattice
    //    (marker length + m · span/bins ± run width) and the loop lengths
    //    between the teeth are never scored, whatever their quality (a
    //    55 Hz buzz whose exact repeat fell between teeth: err 0.03 where
    //    the full set has 0). 16 against 17 bins breaks the lattice (central
    //    length coverage 58 % → 99 % at ±50 ms);
    //  - the budget is water-filled: a bin with fewer crossings than its
    //    share keeps all of them and hands the rest on, so the kept count
    //    does not fall off a cliff from the cap (everything) to a few dozen just above it when
    //    one bin is dense and the rest sparse.
    // A window with no more crossings than the cap is untouched. The cap is
    // set where the bounded search returns the unbounded search's seam on
    // every periodic probe (two-crossing tones, a tone under modulated
    // noise, 40–120 Hz buzzes with up to 330 partials; 320 did not — the
    // ±50 ms user window on a bright buzz holds ~1000 crossings) and the
    // worst case stays about a second on the message thread (120 s of
    // noise, 24 picks; the unbounded search took 30 s and 4 GB there). On
    // pure noise at the widest auto-placement window (±12800, a 120 s
    // take) the length axis is ~93 % covered and the seam error comes out
    // a little above the unbounded search's (0.17–0.19 against 0.16–0.17):
    // noise has no seam to find, only best-of-N luck, and that is the price.
    constexpr size_t kMaxCrossingsPerSide = 1024;
    constexpr int    kThinBinsStart = 16;
    constexpr int    kThinBinsEnd   = 17;

    // Keep up to maxCount elements of the sorted range [b, e) as one
    // contiguous run centred on `centre`.
    template <typename It>
    void appendRunNear(std::vector<int>& out, It b, It e, int centre, size_t maxCount)
    {
        const size_t count = static_cast<size_t>(e - b);
        if (count <= maxCount) { out.insert(out.end(), b, e); return; }
        const size_t i  = static_cast<size_t>(std::lower_bound(b, e, centre) - b);
        const size_t lo = std::min(count - maxCount, i >= maxCount / 2 ? i - maxCount / 2 : 0);
        out.insert(out.end(), b + static_cast<long>(lo), b + static_cast<long>(lo + maxCount));
    }

    void thinAcrossWindow(std::vector<int>& v, int windowLo, int windowHi, int numBins, size_t maxCount)
    {
        if (v.size() <= maxCount) return;
        const double span = std::max(1.0, static_cast<double>(windowHi - windowLo));

        // Bin edges (as iterators into the sorted vector) and counts.
        std::vector<std::vector<int>::iterator> edge(static_cast<size_t>(numBins) + 1);
        edge[0] = v.begin();
        for (int bin = 1; bin < numBins; ++bin)
            edge[static_cast<size_t>(bin)] = std::lower_bound(edge[static_cast<size_t>(bin) - 1], v.end(),
                                                              windowLo + static_cast<int>(span * bin / numBins));
        edge[static_cast<size_t>(numBins)] = v.end();

        // Water-fill the budget: sparse bins keep everything, the remainder is
        // shared equally among the denser ones (visited in ascending count).
        std::vector<int> order(static_cast<size_t>(numBins));
        for (int bin = 0; bin < numBins; ++bin) order[static_cast<size_t>(bin)] = bin;
        auto countOf = [&](int bin) { return static_cast<size_t>(edge[static_cast<size_t>(bin) + 1] - edge[static_cast<size_t>(bin)]); };
        std::sort(order.begin(), order.end(), [&](int a, int b) { return countOf(a) < countOf(b); });
        std::vector<size_t> take(static_cast<size_t>(numBins), 0);
        size_t remaining = maxCount;
        for (size_t k = 0; k < order.size(); ++k)
        {
            const int bin = order[k];
            const size_t share = remaining / (order.size() - k);
            take[static_cast<size_t>(bin)] = std::min(countOf(bin), share);
            remaining -= take[static_cast<size_t>(bin)];
        }

        std::vector<int> out;
        out.reserve(maxCount);
        for (int bin = 0; bin < numBins; ++bin)
        {
            if (take[static_cast<size_t>(bin)] == 0) continue;
            const int centre = windowLo + static_cast<int>(span * (bin + 0.5) / numBins);
            appendRunNear(out, edge[static_cast<size_t>(bin)], edge[static_cast<size_t>(bin) + 1], centre, take[static_cast<size_t>(bin)]);
        }
        v.swap(out);
    }

    // A non-finite score (NaN input) must not reach a sort comparator — it
    // breaks the strict weak ordering — so it ranks last instead.
    float finiteOrInf(float v) noexcept { return std::isfinite(v) ? v : std::numeric_limits<float>::infinity(); }
}

Seam scoreSeam(const float* x, int n, int start, int end, int fadeSamples)
{
    Seam s;
    s.start = start;
    s.end = end;
    if (n <= 0 || end - start < 4 || start < 0 || end > n) return s;

    // The fade mixes the pre-start material x[start-F .. start) into the tail
    // x[end-F .. end): same lag as the loop itself, so for a periodic sound an
    // integer-period loop makes the two coherent. A start too close to the
    // buffer's beginning has no such material; then the post-end material
    // x[end .. end+F) is mixed into the head instead (the other classical form).
    const int half = (end - start) / 2;
    const int fadePre  = juce::jlimit(0, std::min(start, half), fadeSamples);
    const int fadePost = juce::jlimit(0, std::min(n - end, half), fadeSamples);
    if (fadePost > fadePre) { s.fadeSide = +1; s.fade = fadePost; }
    else                    { s.fadeSide = -1; s.fade = fadePre; }
    const int fade = s.fade;
    if (fade >= 16)
        s.correlation = s.fadeSide < 0 ? correlation(x + end - fade, x + start - fade, fade)
                                       : correlation(x + start, x + end, fade);

    const float peak = std::max(1.0e-6f, peakAbs(x, std::max(0, start - kReach), std::min(n, end + kReach)));
    if (start >= kReach && end >= kReach && end + kReach <= n && start + kReach <= n)
        s.seamError = windowedMismatch(x, end, start) / peak;
    else
        s.seamError = std::abs(x[std::min(n - 1, end)] - x[start]) / peak;

    s.rank = rankOf(s.seamError, s.correlation, end - start, 0);
    s.valid = true;
    return s;
}

Seam findSeam(const float* x, int n, int fadeSamples,
              int startLo, int startHi, int endLo, int endHi,
              int minLen, int maxLen, int lengthTarget, int rawStart, int rawEnd)
{
    Seam best;
    if (n <= 0) return best;
    // The raw markers are the user's (or the caller's) own points, kept before
    // the search windows are clamped to the buffer.
    if (rawStart < 0) rawStart = (startLo + startHi) / 2;
    if (rawEnd   < 0) rawEnd   = (endLo + endHi) / 2;
    rawStart = juce::jlimit(0, n - 1, rawStart);
    rawEnd   = juce::jlimit(rawStart, n, rawEnd);
    startLo = juce::jlimit(0, n - 1, startLo); startHi = juce::jlimit(startLo, n - 1, startHi);
    endLo   = juce::jlimit(0, n, endLo);       endHi   = juce::jlimit(endLo, n, endHi);

    auto starts = zeroCrossings(x, n, std::max(kReach, startLo), startHi);
    auto ends   = zeroCrossings(x, n, endLo, std::min(n - kReach, endHi));
    thinAcrossWindow(starts, std::max(kReach, startLo), startHi,        kThinBinsStart, kMaxCrossingsPerSide);
    thinAcrossWindow(ends,   endLo, std::min(n - kReach, endHi),        kThinBinsEnd,   kMaxCrossingsPerSide);

    // Fallback: the raw markers, scored, so the caller always has a fade shape —
    // but only when they are a loop of the asked-for length at all.
    if (rawEnd - rawStart >= minLen && rawEnd - rawStart <= maxLen)
    {
        const Seam raw = scoreSeam(x, n, rawStart, rawEnd, fadeSamples);
        if (raw.valid)
        {
            best = raw;
            // a crossing pair wins ties; a NaN rank (NaN material under the
            // marker) must not be unbeatable
            best.rank = finiteOrInf(rankOf(best.seamError, best.correlation, rawEnd - rawStart, lengthTarget) + 1.0e-3f);
        }
    }

    if (starts.empty() || ends.empty()) return best;

    const float peak = std::max(1.0e-6f, peakAbs(x, std::max(0, startLo - kReach), std::min(n, endHi + kReach)));

    // Tier 1: windowed seam mismatch for every qualifying pair (cheap; at
    // most kMaxCrossingsPerSide^2 pairs after thinAcrossWindow above).
    struct Cand { int s, e; float err; };
    std::vector<Cand> cands;
    cands.reserve(starts.size() * std::min<size_t>(ends.size(), 64));
    for (int s : starts)
        for (int e : ends)
        {
            const int len = e - s;
            if (len < minLen || len > maxLen) continue;
            cands.push_back({ s, e, finiteOrInf(windowedMismatch(x, e, s) / peak) });
        }
    if (cands.empty()) return best;

    // Tier 2: correlation of the fade regions for the best tier-1 candidates.
    const size_t keep = std::min<size_t>(cands.size(), 48);
    std::partial_sort(cands.begin(), cands.begin() + static_cast<long>(keep), cands.end(),
                      [](const Cand& a, const Cand& b) { return a.err < b.err; });
    for (size_t i = 0; i < keep; ++i)
    {
        const Cand& c = cands[i];
        Seam s = scoreSeam(x, n, c.s, c.e, fadeSamples);
        s.rank = rankOf(s.seamError, s.correlation, c.e - c.s, lengthTarget);
        if (s.rank < best.rank) best = s;
    }
    return best;
}

Seam findLoopInRegion(const float* x, int n, double sampleRate, int fadeSamples,
                      int regionLo, int regionHi, int minLen, int maxLen, int lengthTarget)
{
    Seam best;
    regionLo = juce::jlimit(0, n, regionLo);
    regionHi = juce::jlimit(regionLo, n, regionHi);
    const int fade = std::max(16, fadeSamples);
    // The start needs `fade` of material before it (the fade mixes it in) and
    // the window measure needs kReach around every scored point.
    const int startMin = regionLo + std::max(fade, kReach);
    const int endMax   = regionHi - kReach;
    if (endMax - startMin < minLen) return best;
    maxLen = std::min(maxLen, endMax - startMin);
    if (lengthTarget <= 0) lengthTarget = juce::jlimit(minLen, maxLen, (minLen + maxLen) / 2);

    // Coarse pass: log-band features, chunk distance over the fade length.
    const Features feat = computeFeatures(x, n, sampleRate);
    // At most 8 frames (~93 ms) of each fade region are compared: enough to say
    // whether the two are the same kind of sound, and it keeps the pair count
    // times the frame count within a few hundred million operations.
    const int fadeFrames = juce::jlimit(2, 8, fade / kHop);
    struct Coarse { int sFrame, eFrame; float dist; };
    std::vector<Coarse> coarse;
    int step = 1;
    if (feat.numFrames > fadeFrames + 2)
    {
        const int sFrameMin = std::max(fadeFrames, (startMin + kFrameSize / 2) / kHop);
        const int eFrameMax = std::min(feat.numFrames, (endMax - kFrameSize / 2) / kHop);
        const int minFrames = std::max(1, minLen / kHop);
        const int maxFrames = std::max(minFrames, maxLen / kHop);
        // Frame step: the fine pass searches +-one hop around a pair, so a
        // coarse grid of a few hops loses nothing a longer fade would not
        // smear anyway; it bounds the pair count (~400 x 400) for long takes.
        step = std::max(1, (eFrameMax - sFrameMin) / 400);
        for (int sf = sFrameMin; sf < eFrameMax; sf += step)
            for (int ef = sf + minFrames; ef <= std::min(eFrameMax, sf + maxFrames); ef += step)
                coarse.push_back({ sf, ef, finiteOrInf(chunkDistance(feat, sf, ef, fadeFrames)) });
    }
    std::sort(coarse.begin(), coarse.end(), [](const Coarse& a, const Coarse& b) { return a.dist < b.dist; });

    // Keep the best coarse pairs, suppressing near-duplicates (within 2 frames).
    std::vector<Coarse> picked;
    for (const Coarse& c : coarse)
    {
        bool dup = false;
        for (const Coarse& p : picked)
            if (std::abs(p.sFrame - c.sFrame) <= 2 && std::abs(p.eFrame - c.eFrame) <= 2) { dup = true; break; }
        if (! dup) picked.push_back(c);
        if (picked.size() >= 24) break;
    }

    // Fine pass around each coarse pair: ±one coarse grid step on each
    // marker, so the grid leaves no gap, and never outside the region's own
    // admissible range (a start needs `fade` of material before it).
    for (const Coarse& c : picked)
    {
        const int s = c.sFrame * kHop, e = c.eFrame * kHop;
        Seam cand = findSeam(x, n, fade,
                             std::max(startMin, s - kHop * step), std::min(endMax, s + kHop * step),
                             std::max(startMin, e - kHop * step), std::min(endMax, e + kHop * step),
                             minLen, maxLen, lengthTarget, s, e);
        if (! cand.valid) continue;
        cand.featureDistance = c.dist;
        // The coarse distance joins the rank so two equally clean splices prefer
        // the pair whose fade regions are the same kind of sound.
        cand.rank += 0.1f * c.dist;
        if (cand.rank < best.rank) best = cand;
    }

    if (! best.valid)
    {
        // No coarse pass possible (very short material): one fine search over
        // the whole region.
        best = findSeam(x, n, fade, startMin, startMin + std::max(0, endMax - startMin - minLen),
                        startMin + minLen, endMax, minLen, maxLen, lengthTarget);
    }
    return best;
}

Turnaround scoreTurnaround(const float* x, int n, int position, int fadeSamples)
{
    Turnaround t;
    t.position = position;
    if (n <= 0 || position < 0 || position >= n) return t;
    const int fade = juce::jlimit(0, std::min(position, n - 1 - position), fadeSamples);
    // Forward continuation x[c+τ] against the reversed past x[c-τ]: their
    // correlation, and their LEVEL ratio. The runtime fade plays the forward
    // continuation at full level first, so a point whose continuation is much
    // louder than its past (an event just beyond the marker) lets that event
    // through; the level term steers the search away from such points
    // (amplitude matching at loop points: LoopAuditioneer's criterion).
    float levelMismatch = 0.0f;
    if (fade >= 16)
    {
        double sab = 0.0, saa = 0.0, sbb = 0.0;
        for (int i = 0; i < fade; ++i)
        {
            const double a = x[position + i], b = x[position - i];
            sab += a * b; saa += a * a; sbb += b * b;
        }
        const double denom = std::sqrt(saa * sbb);
        t.correlation = denom > 1.0e-18 ? static_cast<float>(sab / denom) : 0.0f;
        if (saa > 1.0e-18 && sbb > 1.0e-18)
            levelMismatch = juce::jlimit(0.0f, 2.0f,
                static_cast<float>(std::abs(10.0 * std::log10(saa / sbb)) / 6.0));   // 1.0 per 6 dB
    }
    const float peak = std::max(1.0e-6f, peakAbs(x, std::max(0, position - kReach), std::min(n, position + kReach)));
    if (position >= kReach && position + kReach <= n)
        t.mirrorError = mirrorMismatch(x, position) / peak;
    else
        t.mirrorError = std::abs(x[std::min(n - 1, position + 1)] - x[std::max(0, position - 1)]) / peak;
    t.rank = t.mirrorError + kCorrelationWeight * (1.0f - juce::jlimit(-1.0f, 1.0f, t.correlation))
           + kCorrelationWeight * levelMismatch;
    t.valid = true;
    return t;
}

Turnaround findTurnaround(const float* x, int n, int fadeSamples, int lo, int hi, int centre)
{
    Turnaround best;
    if (n <= 0) return best;
    if (centre < 0) centre = (lo + hi) / 2;
    centre = juce::jlimit(0, n - 1, centre);
    lo = juce::jlimit(0, n - 1, lo);
    hi = juce::jlimit(lo, n - 1, hi);
    best = scoreTurnaround(x, n, centre, fadeSamples);
    best.rank = finiteOrInf(best.rank + 1.0e-3f);   // a searched point wins ties; NaN never unbeatable

    // Every sample in range is a candidate (a turnaround wants an EXTREMUM of
    // the carrying waveform, which no zero-crossing set contains); the range is
    // at most a few thousand samples. Tier 1: the cheap windowed mirror
    // mismatch for all of them; tier 2: the mirror correlation over the fade
    // for the best few. A point near the buffer's ends is scored with the fade
    // it has room for (scoreTurnaround truncates it), as the runtime does.
    const int cLo = std::max(lo, kReach);
    const int cHi = std::min(hi, n - kReach);
    if (cHi < cLo) return best;

    const float peak = std::max(1.0e-6f, peakAbs(x, std::max(0, cLo - kReach), std::min(n, cHi + kReach)));
    struct Cand { int c; float err; };
    std::vector<Cand> cands;
    cands.reserve(static_cast<size_t>(cHi - cLo + 1));
    for (int c = cLo; c <= cHi; ++c)
        cands.push_back({ c, finiteOrInf(mirrorMismatch(x, c) / peak) });

    const size_t keep = std::min<size_t>(cands.size(), 64);
    std::partial_sort(cands.begin(), cands.begin() + static_cast<long>(keep), cands.end(),
                      [](const Cand& a, const Cand& b) { return a.err < b.err; });
    for (size_t i = 0; i < keep; ++i)
    {
        Turnaround t = scoreTurnaround(x, n, cands[i].c, fadeSamples);
        if (t.rank < best.rank) best = t;
    }
    return best;
}

} // namespace LoopFinder
