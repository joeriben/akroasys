#include "PhraseModel.h"
#include "EuclideanRhythm.h"
#include "ScaleQuantizer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace PhraseModel
{

namespace
{
constexpr float kVelAccent = 100.0f / 127.0f;   // Gen's strand-0 accent / normal pair
constexpr float kVelNormal =  85.0f / 127.0f;
constexpr float kVelFill   =  72.0f / 127.0f;
constexpr float kLayerVel  = 0.85f;
constexpr double kEps      = 1.0e-9;

ScaleQuantizer::Scale scaleOf(int t)
{
    return static_cast<ScaleQuantizer::Scale>(
        std::clamp(t, 0, static_cast<int>(ScaleQuantizer::COUNT) - 1));
}

std::uint32_t mix32(std::uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

bool isApproach(Fn f)
{
    return f == Fn::ApproachUp || f == Fn::ApproachDown || f == Fn::ApproachChromatic;
}

int clampNote(int n) { return std::clamp(n, 0, 127); }

int t16Of(int step, int spb)
{
    return (step / spb) * 16 + (step % spb) * (16 / spb);
}

/** Stable insertion sort of a[0..n) by `less`. In place and allocation-free
 *  (std::stable_sort may take a heap buffer, and this runs on the audio
 *  thread); the lists are short and nearly sorted. */
template <typename T, typename Less>
void insertionSort(T* a, int n, Less less)
{
    for (int i = 1; i < n; ++i)
    {
        const T v = a[i];
        int j = i - 1;
        while (j >= 0 && less(v, a[j])) { a[j + 1] = a[j]; --j; }
        a[j + 1] = v;
    }
}
} // namespace

// ─── Config ────────────────────────────────────────────────────────────────

bool Config::operator==(const Config& o) const
{
    return part == o.part && harm == o.harm && chordDegrees == o.chordDegrees
        && chordRateBars == o.chordRateBars && ext7 == o.ext7 && len == o.len
        && stepsPerBar == o.stepsPerBar && pulses == o.pulses && rotation == o.rotation
        && phraseBars == o.phraseBars && form == o.form && variation == o.variation
        && evolve == o.evolve && slide == o.slide && layer == o.layer
        && scaleType == o.scaleType && scaleRoot == o.scaleRoot && gate == o.gate
        && shuffle == o.shuffle && transpose == o.transpose;
}

// ─── Helpers ───────────────────────────────────────────────────────────────

int floorDiv(int a, int b)
{
    const int q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

int floorMod(int a, int b) { return a - floorDiv(a, b) * b; }

std::uint32_t hashU32(std::uint32_t seed, int a, int b, int c)
{
    std::uint32_t h = mix32(seed ^ 0x9e3779b9U);
    h = mix32(h ^ (static_cast<std::uint32_t>(a) + 0x85ebca6bU));
    h = mix32(h ^ (static_cast<std::uint32_t>(b) + 0xc2b2ae35U));
    h = mix32(h ^ (static_cast<std::uint32_t>(c) + 0x27d4eb2fU));
    return h;
}

float hash01(std::uint32_t seed, int a, int b, int c)
{
    return static_cast<float>(hashU32(seed, a, b, c) >> 8) * (1.0f / 16777216.0f);
}

int metricWeight16(int p16)
{
    p16 = floorMod(p16, 16);
    if (p16 == 0) return 4;
    int w = 0;
    while ((p16 & 1) == 0) { p16 >>= 1; ++w; }
    return w;
}

int stepsPerBarForDivision(int division)
{
    static constexpr int kSpb[] = { 1, 2, 4, 8, 16 };
    return kSpb[std::clamp(division, 0, 4)];
}

std::uint16_t scaleMask(int scaleType, int scaleRoot)
{
    return ScaleQuantizer::pcSetFromScale(scaleOf(scaleType), floorMod(scaleRoot, 12));
}

int scaleStep(int note, int dir, std::uint16_t mask)
{
    const int d = dir >= 0 ? 1 : -1;
    if ((mask & 0x0FFFu) == 0) return note + d;
    for (int i = 1; i <= 12; ++i)
    {
        const int n = note + d * i;
        if (mask & (1u << floorMod(n, 12))) return n;
    }
    return note + d;
}

int scaleSteps(int note, int n, std::uint16_t mask)
{
    for (int i = 0; i < std::abs(n); ++i)
        note = scaleStep(note, n, mask);
    return note;
}

int scaleDistance(int a, int b, std::uint16_t mask)
{
    const int lo = std::min(a, b), hi = std::max(a, b);
    int count = 0;
    for (int n = lo + 1; n <= hi; ++n)
        if (mask & (1u << floorMod(n, 12))) ++count;
    return count;
}

// ─── Harmony ───────────────────────────────────────────────────────────────

int degreeCount(int scaleType)
{
    return ScaleQuantizer::degreesPerOctave(scaleOf(scaleType));
}

bool chordsAvailable(int scaleType)
{
    return scaleOf(scaleType) != ScaleQuantizer::Off;
}

Chord chordForDegree(int degree0, int scaleType, int scaleRoot, bool ext7)
{
    const auto s = scaleOf(scaleType);
    Chord c;
    if (s == ScaleQuantizer::Off)
    {
        // Chromatic: degrees mean nothing; root + fifth on the tonic (§3.3).
        c.rootPc = floorMod(scaleRoot, 12);
        c.iv = { 0, -1, 7, -1 };
        return c;
    }

    const int n = degreeCount(scaleType);
    const int d = floorMod(degree0, n);
    const auto& deg = ScaleQuantizer::kScaleDegrees[s];
    const int rootSemi = deg[d];
    c.rootPc = floorMod(scaleRoot + rootSemi, 12);

    if (n == 7)
    {
        // Heptatonic: stack alternate scale steps (diatonic triads / sevenths).
        auto above = [&](int k) { return floorMod(deg[(d + k) % 7] - rootSemi, 12); };
        c.iv = { 0, above(2), above(4), ext7 ? above(6) : -1 };
        return c;
    }

    // Other scales: the scale tone inside each tertian band above the root.
    std::uint16_t rel = 0;
    for (int k = 0; k < n; ++k)
        rel |= static_cast<std::uint16_t>(1u << floorMod(deg[k] - rootSemi, 12));
    auto has = [&](int iv) { return (rel >> iv) & 1u; };
    c.iv[1] = has(3) ? 3 : has(4) ? 4 : -1;
    c.iv[2] = has(7) ? 7 : has(6) ? 6 : has(8) ? 8 : -1;
    c.iv[3] = !ext7 ? -1 : has(10) ? 10 : has(11) ? 11 : -1;
    return c;
}

Chord chordFromKeys(int lowestNote, std::uint16_t pcMask)
{
    Chord c;
    c.rootPc = floorMod(lowestNote, 12);
    std::uint16_t rel = 0;
    for (int pc = 0; pc < 12; ++pc)
        if ((pcMask >> pc) & 1u)
            rel |= static_cast<std::uint16_t>(1u << floorMod(pc - c.rootPc, 12));
    rel |= 1u;   // the lowest key is the root by definition

    std::uint16_t used = 1u;
    auto take = [&](std::initializer_list<int> band) {
        for (int iv : band)
            if ((rel >> iv) & 1u) { used |= static_cast<std::uint16_t>(1u << iv); return iv; }
        return -1;
    };
    c.iv[1] = take({ 3, 4 });
    c.iv[2] = take({ 7, 6, 8 });
    c.iv[3] = take({ 10, 11 });

    // Leftover held pitch classes fill the members the bands left empty, in
    // ascending order (sus4 → the 4th sounds as the third, a sixth as the 7).
    for (int m = 1; m < kMaxVoices; ++m)
    {
        if (c.iv[static_cast<size_t>(m)] >= 0) continue;
        for (int iv = 1; iv < 12; ++iv)
            if (((rel >> iv) & 1u) && !((used >> iv) & 1u))
            {
                c.iv[static_cast<size_t>(m)] = iv;
                used |= static_cast<std::uint16_t>(1u << iv);
                break;
            }
    }
    return c;
}

int activeChordSlots(const Config& cfg, std::array<int, 4>& degrees0)
{
    if (!chordsAvailable(cfg.scaleType)) return 0;
    const int n = degreeCount(cfg.scaleType);
    int count = 0;
    for (int d : cfg.chordDegrees)
        if (d >= 1 && d <= n)
            degrees0[static_cast<size_t>(count++)] = d - 1;
    return count;
}

Chord chordForBar(const Config& cfg, int bar)
{
    if (cfg.harm == Harm::Chords)
    {
        std::array<int, 4> deg {};
        const int n = activeChordSlots(cfg, deg);
        if (n > 0)
        {
            const int idx = floorMod(floorDiv(bar, std::max(1, cfg.chordRateBars)), n);
            return chordForDegree(deg[static_cast<size_t>(idx)], cfg.scaleType,
                                  cfg.scaleRoot, cfg.ext7);
        }
    }
    return chordForDegree(0, cfg.scaleType, cfg.scaleRoot, cfg.ext7);
}

ChordLabel chordLabel(int degree0, int scaleType, bool ext7)
{
    ChordLabel l;
    const auto s = scaleOf(scaleType);
    if (s == ScaleQuantizer::Off) return l;
    const int n = degreeCount(scaleType);
    const int d = floorMod(degree0, n);
    const int rootSemi = ScaleQuantizer::kScaleDegrees[s][d];

    if (n == 7)
    {
        static constexpr int kMajor[] = { 0, 2, 4, 5, 7, 9, 11 };
        int diff = rootSemi - kMajor[d];
        if (diff > 6) diff -= 12;
        if (diff < -6) diff += 12;
        l.numeral = d + 1;
        l.accidental = std::clamp(diff, -1, 1);
    }
    else
    {
        static constexpr int kNum[] = { 1, 2, 2, 3, 3, 4, 4, 5, 6, 6, 7, 7 };
        static constexpr int kAcc[] = { 0, -1, 0, -1, 0, 0, 1, 0, -1, 0, -1, 0 };
        l.numeral = kNum[rootSemi];
        l.accidental = kAcc[rootSemi];
    }

    const Chord c = chordForDegree(d, scaleType, 0, ext7);
    l.noThird    = c.iv[1] < 0;
    l.minor      = c.iv[1] == 3;
    l.diminished = c.iv[1] == 3 && c.iv[2] == 6;
    l.augmented  = c.iv[1] == 4 && c.iv[2] == 8;
    return l;
}

// ─── Register ──────────────────────────────────────────────────────────────

int partAnchor(Part part)
{
    switch (part)
    {
        case Part::Bass: return 36;
        case Part::Riff: return 48;
        case Part::Lead: return 60;
        case Part::Stab: return 60;
    }
    return 48;
}

int tonicNote(int anchor, int scaleRoot)
{
    return anchor + floorMod(scaleRoot - anchor, 12);
}

int chordRootNote(const Chord& c, int tonic)
{
    int n = tonic + floorMod(c.rootPc - tonic, 12);
    if (n > tonic + 6) n -= 12;
    return n;
}

int chordToneNote(const Chord& c, int member, int octave, int tonic)
{
    member = std::clamp(member, 0, kMaxVoices - 1);
    int iv = c.iv[static_cast<size_t>(member)];
    if (iv < 0)
    {
        if (member == Seventh)    iv = 12;
        else if (member == Third) iv = c.iv[2] >= 0 ? c.iv[2] : 0;
        else                      iv = c.iv[1] >= 0 ? c.iv[1] : 0;
    }
    return clampNote(chordRootNote(c, tonic) + iv + 12 * octave);
}

int voiceChord(const Chord& c, int anchor, const int* prev, int prevCount, int* out)
{
    // Distinct pitch classes of the chord, ascending by interval above the root.
    int ivs[kMaxVoices];
    int n = 0;
    for (int m = 0; m < kMaxVoices; ++m)
    {
        const int iv = c.iv[static_cast<size_t>(m)];
        if (iv < 0) continue;
        bool dup = false;
        for (int k = 0; k < n; ++k) dup = dup || ivs[k] == iv;
        if (!dup) ivs[n++] = iv;
    }
    std::sort(ivs, ivs + n);

    int best[kMaxVoices] = {};
    long bestCost = std::numeric_limits<long>::max();
    for (int inv = 0; inv < n; ++inv)
    {
        const int lowPc = floorMod(c.rootPc + ivs[inv], 12);
        for (int low = anchor - 6; low <= anchor + 6; ++low)
        {
            if (floorMod(low, 12) != lowPc) continue;
            int v[kMaxVoices];
            v[0] = low;
            for (int k = 1; k < n; ++k)
            {
                const int pc = floorMod(c.rootPc + ivs[(inv + k) % n], 12);
                int note = v[k - 1] + 1;
                while (floorMod(note, 12) != pc) ++note;
                v[k] = note;
            }
            long cost = 0;
            if (prevCount > 0)
            {
                // Symmetric nearest-voice distance: common tones cost nothing,
                // the others pay their step to the nearest member.
                for (int k = 0; k < n; ++k)
                {
                    int d = 1000;
                    for (int j = 0; j < prevCount; ++j) d = std::min(d, std::abs(v[k] - prev[j]));
                    cost += d;
                }
                for (int j = 0; j < prevCount; ++j)
                {
                    int d = 1000;
                    for (int k = 0; k < n; ++k) d = std::min(d, std::abs(v[k] - prev[j]));
                    cost += d;
                }
                cost = cost * 64 + std::abs(low - anchor);
            }
            else
            {
                cost = std::abs(low - anchor) * 8 + inv;
            }
            if (cost < bestCost)
            {
                bestCost = cost;
                std::copy(v, v + n, best);
            }
        }
    }
    for (int k = 0; k < n; ++k) out[k] = clampNote(best[k]);
    return n;
}

// ─── Motif analysis ────────────────────────────────────────────────────────

void computeHeads(const Motif& m, std::array<bool, kMaxMotifSteps>& heads)
{
    heads.fill(false);
    const int L = std::min(m.numSteps(), kMaxMotifSteps);
    if (L <= 0) return;
    const int spb = m.stepsPerBar;

    if (m.part == Part::Stab)
    {
        for (int s = 0; s < L; ++s) heads[static_cast<size_t>(s)] = m.slots[static_cast<size_t>(s)].fn != Fn::Rest;
        return;
    }

    const int spanLen16 = m.part == Part::Lead ? 8 : 4;
    int bestStep[kMaxMotifSteps];
    std::fill(std::begin(bestStep), std::end(bestStep), -1);
    int bestW[kMaxMotifSteps] = {}, bestDur[kMaxMotifSteps] = {};

    for (int s = 0; s < L; ++s)
    {
        if (m.slots[static_cast<size_t>(s)].fn == Fn::Rest) continue;
        int dur = 1;
        while (dur < L && m.slots[static_cast<size_t>((s + dur) % L)].fn == Fn::Rest) ++dur;
        const int t16 = t16Of(s, spb);
        const int span = t16 / spanLen16;
        const int w = metricWeight16(t16);
        int& b = bestStep[span];
        // Earlier wins remaining ties: s ascends, so only a strict improvement replaces.
        if (b < 0 || w > bestW[span] || (w == bestW[span] && dur > bestDur[span]))
        {
            b = s; bestW[span] = w; bestDur[span] = dur;
        }
    }
    for (int sp = 0; sp < kMaxMotifSteps; ++sp)
        if (bestStep[sp] >= 0) heads[static_cast<size_t>(bestStep[sp])] = true;
}

namespace
{
struct HeadOption { std::uint8_t member; std::int8_t octave; float w; };

int headOptions(Part part, bool ext7, HeadOption* out)
{
    switch (part)
    {
        case Part::Bass:
            out[0] = { Root, 0, 0.65f }; out[1] = { Fifth, 0, 0.25f }; out[2] = { Root, 1, 0.10f };
            return 3;
        case Part::Riff:
            out[0] = { Root, 0, 1.0f }; out[1] = { Third, 0, 1.0f }; out[2] = { Fifth, 0, 1.0f };
            if (ext7) { out[3] = { Seventh, 0, 1.0f }; return 4; }
            return 3;
        case Part::Lead:
            out[0] = { Third, 0, 0.35f }; out[1] = { Fifth, 0, 0.35f }; out[2] = { Root, 0, 0.15f };
            if (ext7) { out[3] = { Seventh, 0, 0.15f }; return 4; }
            return 3;
        case Part::Stab:
            out[0] = { Root, 0, 1.0f };
            return 1;
    }
    return 0;
}

bool headMemberAllowed(Part part, std::uint8_t member)
{
    if (part == Part::Bass) return member == Root || member == Fifth;
    return true;
}

struct KindOption { Fn fn; float w; };

int kindOptions(Part part, KindOption* out)
{
    switch (part)
    {
        case Part::Bass:
            out[0] = { Fn::Repeat, 0.35f };       out[1] = { Fn::Octave, 0.25f };
            out[2] = { Fn::ApproachUp, 0.10f };   out[3] = { Fn::ApproachDown, 0.07f };
            out[4] = { Fn::ApproachChromatic, 0.08f }; out[5] = { Fn::Passing, 0.15f };
            return 6;
        case Part::Riff:
            out[0] = { Fn::Repeat, 0.35f };        out[1] = { Fn::NeighbourUp, 0.175f };
            out[2] = { Fn::NeighbourDown, 0.175f }; out[3] = { Fn::Passing, 0.30f };
            return 4;
        case Part::Lead:
            out[0] = { Fn::Passing, 0.40f };       out[1] = { Fn::NeighbourUp, 0.175f };
            out[2] = { Fn::NeighbourDown, 0.175f }; out[3] = { Fn::ApproachUp, 0.15f };
            out[4] = { Fn::ApproachDown, 0.10f };
            return 5;
        case Part::Stab:
            return 0;
    }
    return 0;
}

bool kindAllowed(Part part, Fn fn)
{
    KindOption opts[8];
    const int n = kindOptions(part, opts);
    for (int i = 0; i < n; ++i) if (opts[i].fn == fn) return true;
    return false;
}

int nextPulse(const Motif& m, int s)
{
    const int L = m.numSteps();
    for (int d = 1; d <= L; ++d)
    {
        const int k = (s + d) % L;
        if (m.slots[static_cast<size_t>(k)].fn != Fn::Rest) return k;
    }
    return -1;
}

int prevHead(const std::array<bool, kMaxMotifSteps>& heads, int L, int s)
{
    for (int d = 1; d <= L; ++d)
    {
        const int k = floorMod(s - d, L);
        if (heads[static_cast<size_t>(k)]) return k;
    }
    return -1;
}

int nextHead(const std::array<bool, kMaxMotifSteps>& heads, int L, int s)
{
    for (int d = 1; d <= L; ++d)
    {
        const int k = (s + d) % L;
        if (heads[static_cast<size_t>(k)]) return k;
    }
    return -1;
}

/** Context for drawing an embellishment at non-head step s: Approach only into
 *  a head, Passing only between heads at least two scale steps apart over the
 *  tonic chord (§3.6). */
bool kindValidAt(Fn fn, const Motif& m, const std::array<bool, kMaxMotifSteps>& heads,
                 int s, const Config& cfg)
{
    const int L = m.numSteps();
    if (isApproach(fn))
    {
        const int np = nextPulse(m, s);
        return np >= 0 && heads[static_cast<size_t>(np)];
    }
    if (fn == Fn::Passing)
    {
        const int ph = prevHead(heads, L, s), nh = nextHead(heads, L, s);
        if (ph < 0 || nh < 0) return false;
        const Chord tonicChord = chordForDegree(0, cfg.scaleType, cfg.scaleRoot, cfg.ext7);
        const int tonic = tonicNote(partAnchor(m.part), cfg.scaleRoot);
        const auto& a = m.slots[static_cast<size_t>(ph)];
        const auto& b = m.slots[static_cast<size_t>(nh)];
        const int na = chordToneNote(tonicChord, a.member, a.octave, tonic);
        const int nb = chordToneNote(tonicChord, b.member, b.octave, tonic);
        return scaleDistance(na, nb, scaleMask(cfg.scaleType, cfg.scaleRoot)) >= 2;
    }
    return true;
}

void drawHead(Slot& slot, Part part, bool ext7, std::uint32_t seed, int key, int salt,
              int excludeMember = -1, int excludeOctave = 0)
{
    HeadOption opts[4];
    const int n = headOptions(part, ext7, opts);
    float total = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        if (opts[i].member == excludeMember && opts[i].octave == excludeOctave) opts[i].w = 0.0f;
        total += opts[i].w;
    }
    slot.fn = Fn::ChordTone;
    if (total <= 0.0f) return;   // nothing else to choose: keep the slot's member
    float r = hash01(seed, key, salt, 11) * total;
    int pick = -1;
    for (int i = 0; i < n; ++i)
    {
        if (opts[i].w <= 0.0f) continue;
        pick = i;                       // the last valid option catches rounding
        if (r < opts[i].w) break;
        r -= opts[i].w;
    }
    slot.member = opts[pick].member;
    slot.octave = opts[pick].octave;
}

Fn drawKind(const Motif& m, const std::array<bool, kMaxMotifSteps>& heads, int s,
            const Config& cfg, std::uint32_t seed, int key, int salt, Fn exclude = Fn::Rest)
{
    KindOption opts[8];
    const int n = kindOptions(m.part, opts);
    float total = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        if (opts[i].fn == exclude || !kindValidAt(opts[i].fn, m, heads, s, cfg)) opts[i].w = 0.0f;
        total += opts[i].w;
    }
    if (total <= 0.0f)
    {
        if (exclude != Fn::Rest) return exclude;   // no alternative: keep it
        return m.part == Part::Lead ? Fn::NeighbourUp : Fn::Repeat;
    }
    float r = hash01(seed, key, salt, 23) * total;
    for (int i = 0; i < n; ++i)
    {
        if (opts[i].w <= 0.0f) continue;
        if (r < opts[i].w) return opts[i].fn;
        r -= opts[i].w;
    }
    for (int i = n - 1; i >= 0; --i) if (opts[i].w > 0.0f) return opts[i].fn;
    return Fn::Repeat;
}

int firstHead(const std::array<bool, kMaxMotifSteps>& heads, int L)
{
    for (int s = 0; s < L; ++s) if (heads[static_cast<size_t>(s)]) return s;
    return -1;
}

void computeAccents(Motif& m)
{
    // Gen's Euclidean rule: the first step and the pulse after the longest gap.
    const int L = m.numSteps();
    auto pulse = [&](int s) { return m.slots[static_cast<size_t>(floorMod(s, L))].fn != Fn::Rest; };
    auto gapBefore = [&](int s) {
        int gap = 0;
        for (int j = 1; j <= L; ++j) { if (pulse(s - j)) break; ++gap; }
        return gap;
    };
    int maxGap = 1;
    for (int s = 0; s < L; ++s) if (pulse(s)) maxGap = std::max(maxGap, gapBefore(s));
    for (int s = 0; s < L; ++s)
    {
        auto& sl = m.slots[static_cast<size_t>(s)];
        sl.accent = pulse(s) && (s == 0 || gapBefore(s) >= maxGap);
    }
}

/** Assigns functions to every pulse of `m` (whose rhythm is set). Slots of
 *  `keep` at the same time position, in the same role and valid for the Part,
 *  survive; the others are drawn from (seed, time position, role). */
void assignFunctions(Motif& m, const Config& cfg, const Motif* keep)
{
    const int L = m.numSteps();
    std::array<bool, kMaxMotifSteps> heads {};
    computeHeads(m, heads);
    std::array<bool, kMaxMotifSteps> keepHeads {};
    if (keep && keep->len > 0) computeHeads(*keep, keepHeads);

    auto keptSlot = [&](int s) -> const Slot* {
        if (!keep || keep->len <= 0) return nullptr;
        const int t16 = t16Of(s, m.stepsPerBar);
        const int ks = 16 / keep->stepsPerBar;
        if (t16 % ks != 0) return nullptr;
        const int k = (t16 / 16) * keep->stepsPerBar + (t16 % 16) / ks;
        if (k >= keep->numSteps()) return nullptr;
        const Slot& sl = keep->slots[static_cast<size_t>(k)];
        if (sl.fn == Fn::Rest) return nullptr;
        if (keepHeads[static_cast<size_t>(k)] != heads[static_cast<size_t>(s)]) return nullptr;
        return &sl;
    };

    // Heads first: embellishment validity reads the heads around it.
    for (int s = 0; s < L; ++s)
    {
        auto& sl = m.slots[static_cast<size_t>(s)];
        if (sl.fn == Fn::Rest || !heads[static_cast<size_t>(s)]) continue;
        const int t16 = t16Of(s, m.stepsPerBar);
        const Slot* k = keptSlot(s);
        if (m.part == Part::Stab)
        {
            sl.fn = Fn::ChordTone; sl.member = Root; sl.octave = 0;
            sl.slide = false;
        }
        else if (k && k->fn == Fn::ChordTone && headMemberAllowed(m.part, k->member))
        {
            sl.fn = k->fn; sl.member = k->member; sl.octave = k->octave; sl.slide = k->slide;
        }
        else
        {
            sl.slide = false;
            drawHead(sl, m.part, cfg.ext7, m.seed, t16, 1);
        }
    }
    if (m.part == Part::Bass)
    {
        // The downbeat root: the motif's first head is always R (§3.5).
        const int fh = firstHead(heads, L);
        if (fh >= 0)
        {
            auto& sl = m.slots[static_cast<size_t>(fh)];
            sl.fn = Fn::ChordTone; sl.member = Root; sl.octave = 0;
        }
    }

    for (int s = 0; s < L; ++s)
    {
        auto& sl = m.slots[static_cast<size_t>(s)];
        if (sl.fn == Fn::Rest || heads[static_cast<size_t>(s)]) continue;
        const int t16 = t16Of(s, m.stepsPerBar);
        const Slot* k = keptSlot(s);
        if (k && (k->fn == Fn::ChordTone || kindAllowed(m.part, k->fn))
              && (k->fn == Fn::ChordTone || kindValidAt(k->fn, m, heads, s, cfg)))
        {
            sl.fn = k->fn; sl.member = k->member; sl.octave = k->octave; sl.slide = k->slide;
        }
        else
        {
            sl.slide = false; sl.member = Root; sl.octave = 0;
            sl.fn = drawKind(m, heads, s, cfg, m.seed, t16, 2);
        }
    }
}

void setRhythm(Motif& m, int len, int spb, int pulses, int rotation)
{
    m.len = len;
    m.stepsPerBar = spb;
    const int L = len * spb;
    m.pulses = pulses;
    m.rotation = rotation;
    const auto pattern = EuclideanRhythm::generateOnsetFirst(L, pulses, rotation);
    for (int s = 0; s < kMaxMotifSteps; ++s)
    {
        auto& sl = m.slots[static_cast<size_t>(s)];
        sl = Slot {};
        if (s < L && pattern[static_cast<size_t>(s)]) sl.fn = Fn::ChordTone;
    }
}

struct Shape { int len, spb, pulses, rotation; };

Shape shapeOf(const Config& cfg)
{
    Shape sh;
    sh.len = std::clamp(cfg.len, 1, 2);
    sh.spb = std::clamp(cfg.stepsPerBar, 1, kMaxBarSteps);
    const int L = sh.len * sh.spb;
    sh.pulses = std::clamp(cfg.pulses, 1, L);
    sh.rotation = floorMod(cfg.rotation, L);
    return sh;
}
} // namespace

Motif fitMotif(const Motif& stored, const Config& cfg)
{
    const Shape sh = shapeOf(cfg);
    if (stored.len == sh.len && stored.stepsPerBar == sh.spb && stored.pulses == sh.pulses
        && stored.rotation == sh.rotation && stored.part == cfg.part)
        return stored;

    Motif m;
    m.part = cfg.part;
    m.seed = stored.seed;
    m.origin = stored.origin;
    setRhythm(m, sh.len, sh.spb, sh.pulses, sh.rotation);
    assignFunctions(m, cfg, &stored);
    computeAccents(m);
    return m;
}

Motif generateMotif(std::uint32_t seed, int origin, const Config& cfg)
{
    Motif empty;
    empty.seed = seed;
    empty.origin = origin;
    return fitMotif(empty, cfg);
}

Motif answerMotif(const Motif& main, const Config& cfg)
{
    Motif m = main;
    m.seed = hashU32(main.seed, 0xA45E);
    for (int s = 0; s < m.numSteps(); ++s)
    {
        auto& sl = m.slots[static_cast<size_t>(s)];
        if (sl.fn != Fn::Rest) { const bool acc = sl.accent; sl = Slot {}; sl.fn = Fn::ChordTone; sl.accent = acc; }
    }
    assignFunctions(m, cfg, nullptr);
    return m;
}

void evolveOnce(Motif& m, const Config& cfg, int phrase)
{
    if (m.part == Part::Stab || cfg.evolve <= 0.0f) return;
    const int L = m.numSteps();
    int pulses[kMaxMotifSteps];
    int np = 0;
    for (int s = 0; s < L; ++s)
        if (m.slots[static_cast<size_t>(s)].fn != Fn::Rest) pulses[np++] = s;
    if (np == 0) return;

    std::array<bool, kMaxMotifSteps> heads {};
    computeHeads(m, heads);
    const int locked = m.part == Part::Bass ? firstHead(heads, L) : -1;

    const int changes = std::max(1, static_cast<int>(std::lround(cfg.evolve * static_cast<float>(np) / 4.0f)));
    for (int c = 0; c < changes; ++c)
    {
        int idx = static_cast<int>(hashU32(m.seed, phrase, c, 0xE7) % static_cast<std::uint32_t>(np));
        if (pulses[idx] == locked)
        {
            if (np == 1) return;
            idx = (idx + 1) % np;
        }
        const int s = pulses[idx];
        auto& sl = m.slots[static_cast<size_t>(s)];
        if (heads[static_cast<size_t>(s)])
        {
            if (sl.fn != Fn::ChordTone) { sl.fn = Fn::ChordTone; sl.member = Root; sl.octave = 0; }
            if (hash01(m.seed, phrase, c, 0xE8) < 2.0f / 3.0f)
            {
                drawHead(sl, m.part, cfg.ext7, m.seed, phrase, 0xE900 + s, sl.member, sl.octave);
            }
            else if (m.part == Part::Bass)
            {
                if (sl.member == Root) sl.octave = sl.octave == 0 ? 1 : 0;
                else                   sl.octave = sl.octave == 0 ? -1 : 0;
            }
            else
            {
                sl.octave = sl.octave != 0 ? 0
                          : (hash01(m.seed, phrase, c, 0xEA) < 0.5f ? -1 : 1);
            }
        }
        else
        {
            sl.fn = drawKind(m, heads, s, cfg, m.seed, phrase, 0xEB00 + s, sl.fn);
        }
    }
}

Motif evolvedMotif(const Motif& fitted, const Config& cfg, int phrase)
{
    Motif m = fitted;
    if (fitted.origin == kOriginAtNextEntry) return m;   // not heard yet: nothing evolved
    for (int p = fitted.origin + 1; p <= phrase; ++p)
        evolveOnce(m, cfg, p);
    return m;
}

bool sameMotifInputs(const Config& a, const Config& b)
{
    // Every field fitMotif / generateMotif / evolveOnce read (directly or via
    // assignFunctions, drawHead, drawKind), plus the phrase length that
    // numbers the phrases. Evolve is the caller's: the scheduler latches it.
    return a.part == b.part && a.len == b.len && a.stepsPerBar == b.stepsPerBar
        && a.pulses == b.pulses && a.rotation == b.rotation && a.ext7 == b.ext7
        && a.scaleType == b.scaleType && a.scaleRoot == b.scaleRoot
        && phraseBarsOf(a) == phraseBarsOf(b);
}

// ─── Phrase grammar ────────────────────────────────────────────────────────

int phraseBarsOf(const Config& cfg)
{
    return cfg.phraseBars >= 16 ? 16 : cfg.phraseBars >= 8 ? 8 : 4;
}

bool isBUnit(const Config& cfg, int bar)
{
    const int N = phraseBarsOf(cfg);
    const int unit = floorMod(bar, N) / (N / 4);
    switch (cfg.form)
    {
        case Form::Loop: return false;
        case Form::AAAB: return unit == 3;
        case Form::AABA: return unit == 2;
        case Form::ABAB: return unit == 1 || unit == 3;
    }
    return false;
}

BarVariation variationForBar(const Motif& m, const std::array<bool, kMaxMotifSteps>& heads,
                             const Config& cfg, int bar)
{
    BarVariation v;
    const int N = phraseBarsOf(cfg);
    const int unitBars = N / 4;
    const int j = floorMod(bar, N);
    const int phrase = floorDiv(bar, N);
    const int unit = j / unitBars;
    const bool b = isBUnit(cfg, bar);
    const bool lastOfPhrase = j == N - 1;
    const bool lastOfUnit = (j % unitBars) == unitBars - 1;

    // Antecedent / consequent belong to the form, not to Variation (§3.7).
    if (cfg.form == Form::ABAB && lastOfUnit && m.part != Part::Stab)
        v.cadence = b ? 2 : 1;

    const int ops = static_cast<int>(std::lround(std::clamp(cfg.variation, 0.0f, 1.0f) * 5.0f));
    if (ops == 0 || !(b || lastOfPhrase)) return v;

    // B: one draw per unit (Re-pitch holds across its bars); a phrase-end bar
    // outside B draws for itself.
    const int key = b ? unit : 100 + j;
    const int L = m.numSteps();

    if (m.part != Part::Stab)
    {
        int cand[kMaxMotifSteps];
        int nc = 0;
        const int mb = floorMod(bar, m.len);
        for (int s = 0; s < L; ++s)
        {
            if (m.slots[static_cast<size_t>(s)].fn == Fn::Rest || heads[static_cast<size_t>(s)]) continue;
            if (!b && s / m.stepsPerBar != mb) continue;
            cand[nc++] = s;
        }
        if (nc > 0)
        {
            const int s = cand[hashU32(m.seed, phrase, key, 0x01) % static_cast<std::uint32_t>(nc)];
            const Fn cur = m.slots[static_cast<size_t>(s)].fn;
            const Fn alt = drawKind(m, heads, s, cfg, m.seed, phrase, 0x200 + key, cur);
            if (alt != cur) { v.repitchStep = s; v.repitchFn = alt; }
        }
    }

    if ((b && lastOfUnit) || lastOfPhrase)
    {
        const bool fillAvailable = m.stepsPerBar >= 4;
        v.lift = ops >= 2;
        v.fill = ops >= 3 && fillAvailable;
        v.ratchet = ops >= 4;
        v.ratchetHits = 2 + static_cast<int>(hashU32(m.seed, phrase, key, 0x03) % 2u);
        if (ops >= 5)
        {
            // Drop excludes Fill: a seeded coin picks one of them.
            if (!fillAvailable || hash01(m.seed, phrase, key, 0x04) < 0.5f)
            {
                v.drop = true;
                v.fill = false;
            }
        }
    }
    return v;
}

// ─── Rendering ─────────────────────────────────────────────────────────────

namespace
{
struct Onset
{
    double onQ = 0.0, durQ = 0.0;
    double gridQ = 0.0;        // unshuffled position (Fill looks for free 16ths)
    int    note = 60;
    int    octShift = 0;       // Lift: +12 (Stab applies it to the voicing)
    float  vel = kVelNormal;
    int    step = -1;          // motif step, −1 for fill notes
    int    barStep = -1;       // step index inside the bar, −1 for fill notes
    Fn     fn = Fn::Rest;
    bool   head = false, accent = false, slideWanted = false, forcedSlide = false;
    bool   ratchetTail = false;
    bool   glide = false, held = false;
    double offQ = 0.0;
};

struct OnsetList
{
    std::array<Onset, 48> a {};
    int n = 0;
    void push(const Onset& o) { if (n < static_cast<int>(a.size())) a[static_cast<size_t>(n++)] = o; }
};

struct LineCtx
{
    const Config* cfg = nullptr;
    const Motif*  motif = nullptr;
    const Motif*  nextMotif = nullptr;
    std::array<bool, kMaxMotifSteps> heads {}, nextHeads {};
    int    bar = 0;
    double barLenQ = 4.0;
    bool   keys = false;
    Chord  keysChord {};
    int    tonic = 48, center = 54;
    std::uint16_t mask = 0x0FFF;
    bool   lead = false, bass = false;
    const BarVariation* var = nullptr;
};

int barStepsFor(int spb, double barLenQ)
{
    const double stepQ = 4.0 / spb;
    int n = 0;
    while (n < spb && static_cast<double>(n) * stepQ < barLenQ - kEps) ++n;
    return n;
}

/** End of a grid slot: the next grid position or the bar end, whichever comes
 *  first. A bar that is not a whole number of steps cuts its last slot short. */
double slotEnd(double posQ, double lenQ, double barLenQ)
{
    return std::min(posQ + lenQ, barLenQ);
}

/** Onset of grid step k: an odd step is delayed by `shuffle` of its slot.
 *  Taken of the slot rather than of a full step, so a slot the bar cuts short
 *  keeps its onset inside the bar (the scheduler plays a bar's events before
 *  it enters the next one; an onset at or past the bar end would be lost). */
double shuffled(int k, double stepQ, float shuffle, double barLenQ)
{
    const double posQ = static_cast<double>(k) * stepQ;
    if ((k & 1) == 0) return posQ;
    return posQ + static_cast<double>(shuffle) * (slotEnd(posQ, stepQ, barLenQ) - posQ);
}

Chord chordAt(const LineCtx& x, int bar)
{
    return x.keys ? x.keysChord : chordForBar(*x.cfg, bar);
}

int headNote(const Slot& sl, const Chord& ch, int tonic)
{
    return chordToneNote(ch, sl.member, sl.octave, tonic);
}

/** The next head after bar-step k of the bar (k = −1: from the bar start),
 *  rendered against the chord valid at its own position (§3.4). */
int lookaheadHead(const LineCtx& x, int k, bool fromNextBar)
{
    const Motif& m = *x.motif;
    const int spb = m.stepsPerBar;
    if (!fromNextBar)
    {
        const int mb = floorMod(x.bar, m.len);
        const int steps = barStepsFor(spb, x.barLenQ);
        const Chord ch = chordAt(x, x.bar);
        for (int j = k + 1; j < steps; ++j)
        {
            const int s = mb * spb + j;
            if (m.slots[static_cast<size_t>(s)].fn != Fn::Rest && x.heads[static_cast<size_t>(s)])
                return headNote(m.slots[static_cast<size_t>(s)], ch, x.tonic);
        }
    }
    const Motif& nm = *x.nextMotif;
    for (int ahead = 1; ahead <= 2; ++ahead)
    {
        const int nb = x.bar + ahead;
        const int mb = floorMod(nb, nm.len);
        const int steps = barStepsFor(nm.stepsPerBar, x.barLenQ);
        const Chord ch = chordAt(x, nb);
        for (int j = 0; j < steps; ++j)
        {
            const int s = mb * nm.stepsPerBar + j;
            if (nm.slots[static_cast<size_t>(s)].fn != Fn::Rest && x.nextHeads[static_cast<size_t>(s)])
                return headNote(nm.slots[static_cast<size_t>(s)], ch, x.tonic);
        }
    }
    return -1;
}

int renderSlot(const Slot& sl, const Chord& ch, int P, int PP, int T, const LineCtx& x)
{
    if (sl.fn == Fn::ChordTone || sl.fn == Fn::Rest)
        return headNote(sl, ch, x.tonic);
    if (T < 0) T = P >= 0 ? P : chordRootNote(ch, x.tonic);
    if (P < 0) P = T;

    // Post-skip reversal for Lead lines (Huron 2006): after a leap larger than
    // 4 semitones the next embellishment steps back the other way.
    if (x.lead && PP >= 0 && std::abs(P - PP) > 4
        && (sl.fn == Fn::Repeat || sl.fn == Fn::Passing
            || sl.fn == Fn::NeighbourUp || sl.fn == Fn::NeighbourDown))
        return scaleStep(P, P > PP ? -1 : 1, x.mask);

    switch (sl.fn)
    {
        case Fn::Repeat:        return P;
        case Fn::Octave:        return P > x.center ? P - 12 : P + 12;
        case Fn::Passing:
        {
            if (T > P) { const int n = scaleStep(P, 1, x.mask);  return n < T ? n : scaleStep(P, -1, x.mask); }
            if (T < P) { const int n = scaleStep(P, -1, x.mask); return n > T ? n : scaleStep(P, 1, x.mask); }
            return scaleStep(P, 1, x.mask);
        }
        case Fn::NeighbourUp:   return scaleStep(P, 1, x.mask);
        case Fn::NeighbourDown: return scaleStep(P, -1, x.mask);
        case Fn::ApproachUp:    return scaleStep(T, -1, x.mask);
        case Fn::ApproachDown:  return scaleStep(T, 1, x.mask);
        case Fn::ApproachChromatic: return T - 1;
        default: break;
    }
    return P;
}

/** Main-line (or Answer) onsets of the bar, before durations and slides. */
void collectLine(const LineCtx& x, int& P, int& PP, OnsetList& out, BarRender* view)
{
    const Motif& m = *x.motif;
    const Config& cfg = *x.cfg;
    const int spb = m.stepsPerBar;
    const double stepQ = 4.0 / spb;
    const int steps = barStepsFor(spb, x.barLenQ);
    const int mb = floorMod(x.bar, m.len);
    const double lastBeatQ = std::max(0.0, x.barLenQ - 1.0);
    const BarVariation none;
    const BarVariation& var = x.var ? *x.var : none;
    const Chord ch = chordAt(x, x.bar);
    auto inLastBeat = [&](int k) { return static_cast<double>(k) * stepQ >= lastBeatQ - kEps; };

    // The cadence lands on the bar's last sounding head; Lift (Bass) on the
    // last sounding pulse of the last beat; Ratchet on the last beat's
    // strongest head (or strongest pulse when it has none).
    int lastHeadK = -1, lastPulseInLastBeat = -1, ratchetK = -1, ratchetW = -1;
    for (int k = 0; k < steps; ++k)
    {
        const int s = mb * spb + k;
        if (m.slots[static_cast<size_t>(s)].fn == Fn::Rest) continue;
        const bool inLast = inLastBeat(k);
        if (var.drop && inLast) continue;
        const bool head = x.heads[static_cast<size_t>(s)];
        if (head) lastHeadK = k;
        if (inLast)
        {
            lastPulseInLastBeat = k;
            const int w = (head ? 16 : 0) + metricWeight16(t16Of(k, spb));
            if (w > ratchetW) { ratchetW = w; ratchetK = k; }
        }
    }

    int pBeforeLastBeat = P;   // where a Fill run starts from
    for (int k = 0; k < steps; ++k)
    {
        const int s = mb * spb + k;
        const double posQ = static_cast<double>(k) * stepQ;
        const double onQ = shuffled(k, stepQ, cfg.shuffle, x.barLenQ);
        Slot sl = m.slots[static_cast<size_t>(s)];
        const bool head = x.heads[static_cast<size_t>(s)];
        const bool inLast = inLastBeat(k);
        StepView* sv = view ? &view->view[static_cast<size_t>(k)] : nullptr;
        if (sl.fn == Fn::Rest) { if (sv) *sv = StepView {}; continue; }
        if (s == var.repitchStep) sl.fn = var.repitchFn;
        if (var.drop && inLast)
        {
            if (sv) { *sv = StepView {}; sv->fn = sl.fn; sv->head = head; }
            continue;
        }

        const int T = lookaheadHead(x, k, false);
        int note = renderSlot(sl, ch, P, PP, T, x);
        int octShift = 0;

        if (var.cadence != 0 && head && k == lastHeadK)
        {
            if (var.cadence == 2)
                note = chordToneNote(ch, Root, sl.octave, x.tonic);
            else if (cfg.harm == Harm::Pedal && !x.keys)
                note = scaleStep(chordToneNote(ch, Root, sl.octave, x.tonic), 1, x.mask);   // degree 2
            else
                note = chordToneNote(ch, Fifth, sl.octave, x.tonic);
        }
        if (var.lift && inLast)
        {
            if (!x.bass)
            {
                note += 12;
                octShift = 12;
            }
            else if (k == lastPulseInLastBeat)
            {
                const int p = P >= 0 ? P : note;
                note = p > x.center ? p - 12 : p + 12;    // Bass: an Octave instead
            }
        }

        Onset o;
        o.onQ = onQ; o.gridQ = posQ; o.durQ = stepQ; o.note = note; o.octShift = octShift;
        o.vel = sl.accent ? kVelAccent : kVelNormal;
        o.step = s; o.barStep = k; o.fn = sl.fn; o.head = head; o.accent = sl.accent;
        o.forcedSlide = sl.slide;
        o.slideWanted = sl.slide
            || ((sl.fn == Fn::Passing || isApproach(sl.fn))
                && hash01(m.seed, s, 0x51) < cfg.slide);

        if (var.ratchet && k == ratchetK)
        {
            // The repeats divide what is left of the step's slot after its
            // (shuffled) onset: they stay inside the slot, so none lands on
            // the next step's onset or past the bar end.
            const int hits = std::clamp(var.ratchetHits, 2, 3);
            const double endQ = slotEnd(posQ, stepQ, x.barLenQ);
            const double hitQ = (endQ - onQ) / hits;
            const double gridHitQ = (endQ - posQ) / hits;
            o.durQ = hitQ;
            o.slideWanted = o.forcedSlide = false;
            out.push(o);
            for (int h = 1; h < hits; ++h)
            {
                Onset t = o;
                t.onQ = onQ + h * hitQ;
                t.gridQ = posQ + h * gridHitQ;
                t.vel = kVelNormal * 0.9f;
                t.ratchetTail = true;
                out.push(t);
            }
        }
        else
        {
            out.push(o);
        }
        if (sv) { sv->note = note; sv->fn = sl.fn; sv->head = head; sv->accent = sl.accent; sv->slide = false; }

        if (!inLast) pBeforeLastBeat = note;
        PP = P; P = note;
    }

    if (var.fill)
    {
        // A scale run on the 16ths of the last beat that carry no onset, into
        // the next bar's first head.
        const int T = lookaheadHead(x, steps, true);
        const int target = T >= 0 ? T : (P >= 0 ? P : chordRootNote(ch, x.tonic));
        const int from = pBeforeLastBeat >= 0 ? pBeforeLastBeat : target;
        const int dir = target >= from ? -1 : 1;   // counted back from the target
        for (int j = 0; j < 4; ++j)
        {
            const double q = lastBeatQ + 0.25 * j;
            if (q >= x.barLenQ - kEps) continue;
            // Taken on the straight grid, so Shuffle never removes a run note.
            bool taken = false;
            for (int i = 0; i < out.n; ++i)
                taken = taken || std::abs(out.a[static_cast<size_t>(i)].gridQ - q) < 0.125 - kEps;
            if (taken) continue;
            // The run swings with the line: inside the slot of the step it
            // falls in, it divides what follows that step's (shuffled) onset,
            // as the ratchet repeats do. Shuffling it on the 16th grid instead
            // put it on the onset of a coarser odd step that shuffle had moved
            // into this 16th: two notes of the line on one tick. At 1/16 the
            // two coincide.
            const int k = static_cast<int>(std::floor((q + kEps) / stepQ));
            const double slotQ = static_cast<double>(k) * stepQ;
            const double slotOnQ = shuffled(k, stepQ, cfg.shuffle, x.barLenQ);
            const double slotEndQ = slotEnd(slotQ, stepQ, x.barLenQ);
            const double onQ = slotOnQ + (q - slotQ) * (slotEndQ - slotOnQ) / (slotEndQ - slotQ);
            Onset f;
            f.onQ = onQ; f.gridQ = q;
            f.durQ = 0.25;
            f.note = scaleSteps(target, dir * (4 - j), x.mask);
            f.octShift = var.lift && !x.bass ? 12 : 0;
            f.vel = kVelFill;
            f.fn = Fn::Passing;
            out.push(f);
        }
    }

    insertionSort(out.a.data(), out.n, [](const Onset& a, const Onset& b) { return a.onQ < b.onQ; });
    if (out.n > 0)
    {
        P = out.a[static_cast<size_t>(out.n - 1)].note;
        PP = out.n > 1 ? out.a[static_cast<size_t>(out.n - 2)].note : PP;
    }
}

/** First onset of the next bar (quarters from this bar's start) and whether it
 *  is a head. +inf when the next bar is silent. */
void nextBarOnset(const LineCtx& x, double& onQ, bool& head)
{
    const Motif& nm = *x.nextMotif;
    const int spb = nm.stepsPerBar;
    const int mb = floorMod(x.bar + 1, nm.len);
    const int steps = barStepsFor(spb, x.barLenQ);
    for (int k = 0; k < steps; ++k)
    {
        const int s = mb * spb + k;
        if (nm.slots[static_cast<size_t>(s)].fn == Fn::Rest) continue;
        onQ = x.barLenQ + shuffled(k, 4.0 / spb, x.cfg->shuffle, x.barLenQ);
        head = x.nextHeads[static_cast<size_t>(s)];
        return;
    }
    onQ = std::numeric_limits<double>::infinity();
    head = false;
}

/** Durations, slides and glides of a monophonic line. A slide holds its note
 *  and the next onset glides it: a drawn slide (Passing / Approach) only into
 *  a head, a forced one (From Step) into any step onset; never into a fill
 *  note or a ratchet repeat. */
void finishLine(OnsetList& l, const LineCtx& x, bool slideIn, bool allowSlides, bool& endsHeld)
{
    double nbOn; bool nbHead;
    nextBarOnset(x, nbOn, nbHead);
    endsHeld = false;
    const double gate = std::clamp(static_cast<double>(x.cfg->gate), 0.1, 1.0);
    for (int i = 0; i < l.n; ++i)
    {
        Onset& o = l.a[static_cast<size_t>(i)];
        const bool last = i == l.n - 1;
        const Onset* next = last ? nullptr : &l.a[static_cast<size_t>(i + 1)];
        const double nextOn = last ? nbOn : next->onQ;
        const bool nextIsStep = last ? std::isfinite(nbOn) : (next->step >= 0 && !next->ratchetTail);
        const bool nextHead = last ? nbHead : next->head;
        if (i == 0 && slideIn) o.glide = true;
        if (allowSlides && o.slideWanted && nextIsStep && (o.forcedSlide || nextHead))
        {
            o.held = true;
            if (last) endsHeld = true;
            else      l.a[static_cast<size_t>(i + 1)].glide = true;
        }
        o.offQ = o.onQ + std::max(1.0e-3, std::min(gate * o.durQ, nextOn - o.onQ));
    }
}

void pushEvent(BarRender& out, const Event& e)
{
    if (out.count < kMaxEvents) out.events[static_cast<size_t>(out.count++)] = e;
}

void emitLine(BarRender& out, const OnsetList& l, int strand, int transpose, float velScale)
{
    for (int i = 0; i < l.n; ++i)
    {
        const Onset& o = l.a[static_cast<size_t>(i)];
        Event e;
        e.onQ = o.onQ; e.offQ = o.offQ;
        e.note = clampNote(o.note + transpose);
        e.vel = o.vel * velScale;
        e.strand = strand; e.glide = o.glide; e.held = o.held;
        pushEvent(out, e);
    }
}

/** One event per voice and onset. `oneStrand`: every voice on `strand` (Stab's
 *  chord is the main line, strand −1, so the voice manager treats the whole
 *  chord alike — pan, stereo, pedals); otherwise voice v on `strand` + v. */
void emitChords(BarRender& out, const OnsetList& l, const int* voicing, int nv,
                int strand, bool oneStrand, int shift, int transpose, float velScale)
{
    for (int i = 0; i < l.n; ++i)
    {
        const Onset& o = l.a[static_cast<size_t>(i)];
        for (int v = 0; v < nv; ++v)
        {
            Event e;
            e.onQ = o.onQ; e.offQ = o.offQ;
            e.note = clampNote(voicing[v] + shift + o.octShift + transpose);
            e.vel = o.vel * velScale;
            e.strand = oneStrand ? strand : strand + v;
            pushEvent(out, e);
        }
    }
}
} // namespace

int voicingForBar(const Config& cfg, int bar, int anchor, int* out)
{
    std::array<int, 4> deg {};
    const int n = cfg.harm == Harm::Chords ? activeChordSlots(cfg, deg) : 0;
    if (n == 0)
    {
        const Chord c = chordForDegree(0, cfg.scaleType, cfg.scaleRoot, cfg.ext7);
        return voiceChord(c, anchor, nullptr, 0, out);
    }
    const int idx = floorMod(floorDiv(bar, std::max(1, cfg.chordRateBars)), n);
    int prev[kMaxVoices];
    int nv = voiceChord(chordForDegree(deg[0], cfg.scaleType, cfg.scaleRoot, cfg.ext7),
                        anchor, nullptr, 0, prev);
    // One full lap, then on to idx in the second: the voicing at idx is then
    // independent of where playback started.
    for (int j = 1; j <= n + idx; ++j)
    {
        int cur[kMaxVoices];
        const Chord c = chordForDegree(deg[static_cast<size_t>(j % n)], cfg.scaleType, cfg.scaleRoot, cfg.ext7);
        const int nc = voiceChord(c, anchor, prev, nv, cur);
        std::copy(cur, cur + nc, prev);
        nv = nc;
    }
    std::copy(prev, prev + nv, out);
    return nv;
}

void renderBar(const RenderInput& in, BarRender& out)
{
    out.count = 0;
    const Config& cfg = *in.cfg;
    const Motif& m = *in.motif;
    out.view.fill(StepView {});
    out.viewSteps = barStepsFor(m.stepsPerBar, in.barLenQ);
    out.endsHeld = false;

    LineCtx x;
    x.cfg = &cfg; x.motif = &m; x.nextMotif = in.nextMotif ? in.nextMotif : in.motif;
    computeHeads(m, x.heads);
    computeHeads(*x.nextMotif, x.nextHeads);
    x.bar = in.bar; x.barLenQ = in.barLenQ;
    x.keys = in.keys; x.keysChord = in.keysChord;
    x.tonic = tonicNote(partAnchor(m.part), cfg.scaleRoot);
    x.center = x.tonic + 6;
    x.mask = scaleMask(cfg.scaleType, cfg.scaleRoot);
    x.lead = m.part == Part::Lead;
    x.bass = m.part == Part::Bass;
    const BarVariation var = variationForBar(m, x.heads, cfg, in.bar);
    x.var = &var;
    out.chord = chordAt(x, in.bar);

    const int octaveShift = m.part == Part::Bass ? 12 : -12;
    int P = in.prevNote, PP = in.prevPrevNote;

    OnsetList line;
    collectLine(x, P, PP, line, &out);
    bool endsHeld = false;
    finishLine(line, x, in.slideIn, m.part != Part::Stab, endsHeld);
    for (int i = 0; i < line.n; ++i)
    {
        const Onset& o = line.a[static_cast<size_t>(i)];
        if (o.barStep >= 0 && !o.ratchetTail && o.held)
            out.view[static_cast<size_t>(o.barStep)].slide = true;
    }

    int voicing[kMaxVoices];
    int nv = 0;
    if (m.part == Part::Stab)
    {
        nv = in.keys ? voiceChord(in.keysChord, partAnchor(Part::Stab), in.prevVoicing, in.prevVoicingCount, voicing)
                     : voicingForBar(cfg, in.bar, partAnchor(Part::Stab), voicing);
        for (int i = 0; i < line.n; ++i) line.a[static_cast<size_t>(i)].note = voicing[0];
        emitChords(out, line, voicing, nv, -1, true, 0, cfg.transpose, 1.0f);
        if (cfg.layer == Layer::Octave)
            emitChords(out, line, voicing, nv, 5, false, octaveShift, cfg.transpose, kLayerVel);
        for (int k = 0; k < out.viewSteps; ++k)
            if (out.view[static_cast<size_t>(k)].note >= 0) out.view[static_cast<size_t>(k)].note = voicing[0];
    }
    else
    {
        emitLine(out, line, -1, cfg.transpose, 1.0f);
        if (cfg.layer == Layer::Octave)
        {
            OnsetList dbl = line;
            for (int i = 0; i < dbl.n; ++i) dbl.a[static_cast<size_t>(i)].note += octaveShift;
            emitLine(out, dbl, 5, cfg.transpose, kLayerVel);
        }
        else if (cfg.layer == Layer::Chord)
        {
            const int anchor = m.part == Part::Lead ? 48 : 60;
            nv = in.keys ? voiceChord(in.keysChord, anchor, in.prevVoicing, in.prevVoicingCount, voicing)
                         : voicingForBar(cfg, in.bar, anchor, voicing);
            OnsetList hits;
            for (int i = 0; i < line.n; ++i)
            {
                Onset o = line.a[static_cast<size_t>(i)];
                if (!o.head || o.ratchetTail) continue;
                o.glide = o.held = false;   // offQ already holds the gated length
                hits.push(o);
            }
            emitChords(out, hits, voicing, nv, 5, false, 0, cfg.transpose, kLayerVel);
        }
        else if (cfg.layer == Layer::Answer && in.answer && isBUnit(cfg, in.bar))
        {
            LineCtx a = x;
            a.motif = in.answer; a.nextMotif = in.answer;
            computeHeads(*in.answer, a.heads);
            a.nextHeads = a.heads;
            a.tonic = tonicNote(partAnchor(m.part) + octaveShift, cfg.scaleRoot);
            a.center = a.tonic + 6;
            a.var = nullptr;
            int aP = chordRootNote(out.chord, a.tonic), aPP = aP;
            OnsetList al;
            collectLine(a, aP, aPP, al, nullptr);
            bool aHeld = false;
            finishLine(al, a, false, false, aHeld);
            emitLine(out, al, 5, cfg.transpose, kLayerVel);
        }
    }

    // The scheduler walks the events in time order: line and layer interleave.
    insertionSort(out.events.data(), out.count, [](const Event& a, const Event& b) { return a.onQ < b.onQ; });

    out.lastNote = P;
    out.prevNote = PP;
    out.endsHeld = endsHeld;
    for (int v = 0; v < nv; ++v) out.lastVoicing[static_cast<size_t>(v)] = voicing[v];
    out.lastVoicingCount = nv;
}

void entryNotes(const Config& cfg, const Motif& prevMotif, const Motif& motif,
                int bar, double barLenQ, BarRender& scratch, int& p, int& pp)
{
    RenderInput in;
    in.cfg = &cfg;
    in.motif = &prevMotif;
    in.nextMotif = &motif;
    in.bar = bar - 1;
    in.barLenQ = barLenQ;
    const Chord ch = chordForBar(cfg, bar - 1);
    in.prevNote = in.prevPrevNote = chordRootNote(ch, tonicNote(partAnchor(prevMotif.part), cfg.scaleRoot));
    Config c = cfg;
    c.layer = Layer::Off;   // only the main line's last notes matter
    in.cfg = &c;
    renderBar(in, scratch);
    p = scratch.lastNote;
    pp = scratch.prevNote;
}

// ─── Step → Phrase ─────────────────────────────────────────────────────────

Motif motifFromSteps(const StepInput* steps, int count, const Config& cfg,
                     std::uint32_t seed, int origin)
{
    Motif m;
    m.part = cfg.part;
    m.seed = seed;
    m.origin = origin;
    m.stepsPerBar = std::clamp(cfg.stepsPerBar, 1, kMaxBarSteps);
    m.len = count > m.stepsPerBar ? 2 : 1;
    m.rotation = 0;
    const int L = m.numSteps();
    int pulses = 0;
    for (int s = 0; s < L; ++s)
    {
        auto& sl = m.slots[static_cast<size_t>(s)];
        sl = Slot {};
        if (s < count && steps[s].enabled)
        {
            sl.fn = Fn::ChordTone;
            sl.accent = steps[s].velocity > 0.85f;
            sl.slide = steps[s].slide;
            ++pulses;
        }
    }
    m.pulses = pulses;
    if (pulses == 0) return m;

    std::array<bool, kMaxMotifSteps> heads {};
    computeHeads(m, heads);
    const Chord ch = chordForBar(cfg, 0);
    const int tonic = tonicNote(partAnchor(m.part), cfg.scaleRoot);
    const std::uint16_t mask = scaleMask(cfg.scaleType, cfg.scaleRoot);

    auto nearestMember = [&](int note, Slot& sl) {
        int best = 1 << 20;
        for (int mem = 0; mem < kMaxVoices; ++mem)
        {
            if (ch.iv[static_cast<size_t>(mem)] < 0) continue;
            if (m.part == Part::Bass && !headMemberAllowed(m.part, static_cast<std::uint8_t>(mem))) continue;
            for (int oct = -1; oct <= 1; ++oct)
            {
                const int cand = chordToneNote(ch, mem, oct, tonic);
                const int cost = (floorMod(cand - note, 12) != 0 ? 100 : 0) + std::abs(cand - note);
                if (cost < best)
                {
                    best = cost;
                    sl.member = static_cast<std::uint8_t>(mem);
                    sl.octave = static_cast<std::int8_t>(oct);
                }
            }
        }
        return best;
    };

    auto noteAt = [&](int s) { return steps[s].note; };
    for (int s = 0; s < L; ++s)
    {
        auto& sl = m.slots[static_cast<size_t>(s)];
        if (sl.fn == Fn::Rest) continue;
        const int n = noteAt(s);
        if (heads[static_cast<size_t>(s)] || m.part == Part::Stab) { nearestMember(n, sl); continue; }

        int ps = s;
        for (int d = 1; d <= L; ++d) { const int k = floorMod(s - d, L); if (m.slots[static_cast<size_t>(k)].fn != Fn::Rest) { ps = k; break; } }
        const int nh = nextHead(heads, L, s);
        const int P = noteAt(ps);
        const int T = nh >= 0 ? noteAt(nh) : P;
        const int np = nextPulse(m, s);
        const bool intoHead = np >= 0 && heads[static_cast<size_t>(np)];

        Fn fn = Fn::Rest;
        if (n == P)                                         fn = Fn::Repeat;
        else if (std::abs(n - P) == 12)                     fn = Fn::Octave;
        else if (T != P && ((T > P && n > P && n < T) || (T < P && n < P && n > T))
                 && n == scaleStep(P, T > P ? 1 : -1, mask)) fn = Fn::Passing;
        else if (intoHead && n == scaleStep(T, -1, mask))   fn = Fn::ApproachUp;
        else if (intoHead && n == scaleStep(T, 1, mask))    fn = Fn::ApproachDown;
        else if (intoHead && n == T - 1)                    fn = Fn::ApproachChromatic;
        else if (n == scaleStep(P, 1, mask))                fn = Fn::NeighbourUp;
        else if (n == scaleStep(P, -1, mask))               fn = Fn::NeighbourDown;

        if (fn == Fn::Rest)
        {
            // Nothing explains it as an embellishment: a chord tone if it is
            // one, otherwise the embellishment that lands nearest.
            Slot ct = sl;
            if (nearestMember(n, ct) < 100) { sl.fn = Fn::ChordTone; sl.member = ct.member; sl.octave = ct.octave; continue; }
            const Fn cands[] = { Fn::NeighbourUp, Fn::NeighbourDown, Fn::Passing, Fn::Repeat };
            int bestD = 1 << 20;
            for (Fn c : cands)
            {
                int r = c == Fn::NeighbourUp ? scaleStep(P, 1, mask)
                      : c == Fn::NeighbourDown ? scaleStep(P, -1, mask)
                      : c == Fn::Passing ? scaleStep(P, T >= P ? 1 : -1, mask) : P;
                if (std::abs(r - n) < bestD) { bestD = std::abs(r - n); fn = c; }
            }
        }
        sl.fn = fn;
    }
    return m;
}

} // namespace PhraseModel
