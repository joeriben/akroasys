// Does Freeverb+ actually have early reflections, and does Room actually move them?
//
// A wet-level measurement certifies nothing here: a bare wash trimmed to the right
// gain passes it. These are the properties the feature exists for, so these are what
// get asserted, against Freeverb as the control:
//
//   1. ENERGY IN THE GAP. Freeverb's combs are 1116..1617 samples = 25.3..36.7 ms,
//      and nothing else generates output in between — except a direct leak at t=0,
//      because its allpass returns `bufferedValue - input` (juce_Reverb.h:307), so
//      four in series feed the input straight through at about -27 dB. Between that
//      leak and the first comb, Freeverb is empty. Freeverb+ must fill exactly that
//      window with its reflections, and resolve them as separated arrivals.
//   2. ROOM IS GEOMETRY. juce_Reverb.h derives no delay length from roomSize, so
//      Freeverb's arrival times CANNOT move with Room. Freeverb+'s must.
//   3. STEREO FROM DECORRELATION. Freeverb's whole image is stereoSpread = 23
//      samples = 0.52 ms, so its L/R impulse responses stay highly correlated.
//      Freeverb+ gives L and R disjoint tap times, so correlation must drop.
//
// Build (after a Release build of the plugin):
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/test_early_reflections.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/test_early_reflections
#include "JuceHeader.h"
#include "dsp/AlgorithmicReverb.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <chrono>

static constexpr double SR = 44100.0;
static constexpr int    BS = 512;
static constexpr int    kBlocks = 40;                  // ~465 ms
static constexpr int    kLen    = kBlocks * BS;

static int failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  %-64s %s\n", what, ok ? "ok" : "FAIL");
    if (! ok) ++failures;
}

struct Ir { std::vector<float> L, R; };

static Ir impulse(bool earlyReflections, float room)
{
    AlgorithmicReverb rv;
    rv.prepare(SR, BS);
    rv.setMix(1.0f);
    rv.setEarlyReflections(earlyReflections);
    rv.setRoomSize(room);
    rv.setDamping(0.0f);        // no output LP — do not smear the arrivals
    rv.setWidth(1.0f);
    rv.reset();                 // snaps the Room glide to its target

    Ir ir; ir.L.assign(kLen, 0.0f); ir.R.assign(kLen, 0.0f);
    juce::AudioBuffer<float> buf(2, BS);
    for (int b = 0; b < kBlocks; ++b)
    {
        buf.clear();
        if (b == 0) { buf.setSample(0, 0, 1.0f); buf.setSample(1, 0, 1.0f); }
        rv.processBlock(buf);
        for (int i = 0; i < BS; ++i)
        {
            ir.L[b * BS + i] = buf.getSample(0, i);
            ir.R[b * BS + i] = buf.getSample(1, i);
        }
    }
    return ir;
}

// Time of the first sample above `thresh`, in ms (-1 if the IR never gets there).
static double firstArrivalMs(const std::vector<float>& x, float thresh)
{
    for (size_t i = 0; i < x.size(); ++i)
        if (std::fabs(x[i]) > thresh)
            return 1000.0 * static_cast<double>(i) / SR;
    return -1.0;
}

// Counts SEPARATED arrivals in the first `windowMs`: peaks above `thresh` with at
// least `gapMs` of near-silence before them. A dense wash scores low by construction
// because it never goes quiet between peaks — which is exactly the distinction.
static int discreteArrivals(const std::vector<float>& x, double windowMs,
                            float thresh, double gapMs)
{
    const int n   = static_cast<int>(windowMs * 0.001 * SR);
    const int gap = static_cast<int>(gapMs * 0.001 * SR);
    int count = 0;
    int i = 0;
    while (i < n && i < static_cast<int>(x.size()))
    {
        if (std::fabs(x[i]) > thresh)
        {
            ++count;
            // Skip past this arrival: advance until the signal has stayed under the
            // threshold for a full gap.
            int quiet = 0;
            while (i < n && quiet < gap)
            {
                quiet = (std::fabs(x[i]) > thresh) ? 0 : quiet + 1;
                ++i;
            }
        }
        else ++i;
    }
    return count;
}

static double correlation(const std::vector<float>& a, const std::vector<float>& b, int n)
{
    double sa = 0, sb = 0, sab = 0;
    for (int i = 0; i < n; ++i) { sa += a[i]*a[i]; sb += b[i]*b[i]; sab += a[i]*b[i]; }
    const double d = std::sqrt(sa * sb);
    return d > 1e-20 ? sab / d : 0.0;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("impulse response, damping 0, width 1\n\n");

    const Ir fv   = impulse(false, 0.7f);
    const Ir fvp  = impulse(true,  0.7f);
    const float thresh = 0.002f;

    // ── 1. Discrete arrivals before the diffuse field ────────────────────────
    std::printf("discrete arrivals in the first 100 ms (gap 4 ms, |x| > %.3f):\n", thresh);
    const int nFv  = discreteArrivals(fv.L,  100.0, thresh, 4.0);
    const int nFvp = discreteArrivals(fvp.L, 100.0, thresh, 4.0);
    std::printf("    Freeverb  : %d      first arrival %.1f ms\n",
                nFv,  firstArrivalMs(fv.L, thresh));
    std::printf("    Freeverb+ : %d      first arrival %.1f ms\n",
                nFvp, firstArrivalMs(fvp.L, thresh));
    check(nFvp > nFv, "Freeverb+ resolves more separated arrivals than Freeverb");
    // The decisive window: after Freeverb's t=0 allpass leak, before its shortest
    // comb at 25.3 ms. Freeverb is empty here by construction; this is the hole the
    // reflections fill.
    auto energyMs = [](const std::vector<float>& x, double fromMs, double toMs) {
        const int a = static_cast<int>(fromMs * 0.001 * SR);
        const int b = static_cast<int>(toMs   * 0.001 * SR);
        double e = 0.0;
        for (int i = a; i < b && i < static_cast<int>(x.size()); ++i) e += x[i] * x[i];
        return e;
    };
    const double gapFv  = energyMs(fv.L,  5.0, 25.0);
    const double gapFvp = energyMs(fvp.L, 5.0, 25.0);
    std::printf("  energy in the 5-25 ms gap: Freeverb %.3e   Freeverb+ %.3e  (%.0fx)\n",
                gapFv, gapFvp, gapFvp / juce::jmax(gapFv, 1e-30));
    check(gapFvp > gapFv * 100.0,
          "Freeverb+ fills the gap Freeverb leaves between leak and first comb");

    // ── 2. Room must be geometry, not only decay ─────────────────────────────
    std::printf("\nfirst arrival vs Room:\n");
    double fvArr[3], fvpArr[3];
    const float rooms[3] = { 0.0f, 0.5f, 1.0f };
    for (int i = 0; i < 3; ++i)
    {
        fvArr[i]  = firstArrivalMs(impulse(false, rooms[i]).L, thresh);
        fvpArr[i] = firstArrivalMs(impulse(true,  rooms[i]).L, thresh);
        std::printf("    Room %.1f : Freeverb %.1f ms   Freeverb+ %.1f ms\n",
                    rooms[i], fvArr[i], fvpArr[i]);
    }
    check(std::fabs(fvArr[2] - fvArr[0]) < 0.5,
          "Freeverb's arrival time does NOT move with Room (the control)");
    check(fvpArr[2] > fvpArr[0] * 2.0,
          "Freeverb+ arrival time grows with Room (Room means size)");

    // ── 3. Stereo image from real decorrelation ──────────────────────────────
    const int n100 = static_cast<int>(0.100 * SR);
    const double cFv  = correlation(fv.L,  fv.R,  n100);
    const double cFvp = correlation(fvp.L, fvp.R, n100);
    std::printf("\nL/R correlation over the first 100 ms:\n");
    std::printf("    Freeverb  : %+.3f\n", cFv);
    std::printf("    Freeverb+ : %+.3f\n", cFvp);
    check(std::fabs(cFvp) < std::fabs(cFv) * 0.7,
          "Freeverb+ decorrelates the channels well beyond Freeverb's 0.52 ms");

    // ── 4. A Room DRAG must not click ────────────────────────────────────────
    // The bank's whole job is moving read positions when Room moves. Advancing the
    // scale once per block splices the signal at every boundary; the earlier
    // version of this suite could not see it, because it snapped the glide with
    // reset() and never swept. So: sweep, and compare the jump AT boundaries with
    // the jump inside them, against Freeverb (whose Room touches no delay length)
    // as the control.
    {
        auto sweep = [](bool er, int blockSize) {
            AlgorithmicReverb rv;
            rv.prepare(SR, blockSize);
            rv.setMix(1.0f);
            rv.setEarlyReflections(er);
            rv.setRoomSize(0.0f);
            rv.setDamping(0.4f);
            rv.setWidth(1.0f);
            rv.reset();

            const int blocks = static_cast<int>(1.0 * SR / blockSize);   // 1 s drag
            std::vector<float> out;
            out.reserve(static_cast<size_t>(blocks * blockSize));
            juce::AudioBuffer<float> buf(2, blockSize);
            int idx = 0;
            for (int b = 0; b < blocks; ++b)
            {
                rv.setRoomSize(static_cast<float>(b) / static_cast<float>(blocks - 1));
                for (int i = 0; i < blockSize; ++i, ++idx)
                {
                    const float v = static_cast<float>(0.25 * std::sin(2.0 * M_PI * 220.0 * idx / SR));
                    buf.setSample(0, i, v); buf.setSample(1, i, v);
                }
                rv.processBlock(buf);
                for (int i = 0; i < blockSize; ++i) out.push_back(buf.getSample(0, i));
            }
            double inner = 0.0, boundary = 0.0;
            for (size_t i = 1; i < out.size(); ++i)
            {
                const double d = std::fabs(out[i] - out[i - 1]);
                if (i % static_cast<size_t>(blockSize) == 0) boundary = std::max(boundary, d);
                else                                          inner    = std::max(inner, d);
            }
            return boundary / std::max(inner, 1e-12);
        };
        std::printf("\nRoom swept 0->1 over 1 s, boundary jump / inner jump:\n");
        bool ok = true;
        for (int bs : { 512, 2048 })
        {
            const double rFv  = sweep(false, bs);
            const double rFvp = sweep(true,  bs);
            std::printf("    block %4d : Freeverb %.2f   Freeverb+ %.2f\n", bs, rFv, rFvp);
            ok = ok && rFvp < 1.5;
        }
        check(ok, "a Room drag stays click-free at every block size");
    }

    // ── 5. An oversize host block must not silently drop the voicing ─────────
    {
        auto render = [](bool er, int prepSize, int hostBlock) {
            AlgorithmicReverb rv;
            rv.prepare(SR, prepSize);
            rv.setMix(1.0f);
            rv.setEarlyReflections(er);
            rv.setRoomSize(0.7f);
            rv.setDamping(0.4f);
            rv.setWidth(1.0f);
            rv.reset();
            juce::AudioBuffer<float> buf(2, hostBlock);
            std::vector<float> out;
            for (int b = 0; b < 8; ++b)
            {
                buf.clear();
                if (b == 0) { buf.setSample(0, 0, 1.0f); buf.setSample(1, 0, 1.0f); }
                rv.processBlock(buf);
                for (int i = 0; i < hostBlock; ++i) out.push_back(buf.getSample(0, i));
            }
            return out;
        };
        const auto fvBig  = render(false, 512, 1024);
        const auto fvpBig = render(true,  512, 1024);
        double diff = 0.0;
        for (size_t i = 0; i < fvBig.size(); ++i) diff += std::fabs(fvBig[i] - fvpBig[i]);
        std::printf("\nprepared at 512, host sends 1024:\n");
        std::printf("    sum|Freeverb+ - Freeverb| = %.6e\n", diff);
        check(diff > 1.0, "Freeverb+ stays Freeverb+ when the host block exceeds prepare()");
    }

    // ── 6. setMix must still mean dry/wet ────────────────────────────────────
    // The processor drives this at mix=1.0, so a broken mixer is invisible in the
    // plugin — but the class says "0=dry, 1=wet" and that has to be true.
    {
        auto dryAt = [](float mix) {
            AlgorithmicReverb rv;
            rv.prepare(SR, BS);
            rv.setEarlyReflections(true);
            rv.setMix(mix);
            rv.setRoomSize(0.7f);
            rv.setDamping(0.0f);
            rv.setWidth(1.0f);
            rv.reset();
            juce::AudioBuffer<float> buf(2, BS);
            buf.clear();
            buf.setSample(0, 0, 1.0f); buf.setSample(1, 0, 1.0f);
            rv.processBlock(buf);
            return std::fabs(buf.getSample(0, 0));
        };
        std::printf("\ndirect signal at sample 0 (Freeverb+, impulse in):\n");
        const double d25 = dryAt(0.25f), d50 = dryAt(0.5f);
        std::printf("    mix 0.25 : %.4f      mix 0.50 : %.4f\n", d25, d50);
        check(d25 > 0.5 && d50 > 0.3 && d25 > d50,
              "the dry path survives, and falls as Mix rises");
    }

    // ── What the front end costs ─────────────────────────────────────────────
    {
        auto bench = [](bool er) {
            AlgorithmicReverb rv;
            rv.prepare(SR, BS);
            rv.setMix(1.0f);
            rv.setEarlyReflections(er);
            rv.setRoomSize(0.7f);
            rv.setDamping(0.4f);
            rv.setWidth(1.0f);
            juce::AudioBuffer<float> buf(2, BS);
            juce::Random rng(1);
            const int n = 4000;                       // ~46 s of audio
            for (int i = 0; i < BS; ++i)              // keep it out of the silence skip
            { buf.setSample(0, i, rng.nextFloat() - 0.5f); buf.setSample(1, i, rng.nextFloat() - 0.5f); }
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < n; ++b)
            {
                for (int i = 0; i < BS; ++i)
                { buf.setSample(0, i, rng.nextFloat() - 0.5f); buf.setSample(1, i, rng.nextFloat() - 0.5f); }
                rv.processBlock(buf);
            }
            const auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(t1 - t0).count()
                 / (n * BS / SR);                     // fraction of one core, realtime
        };
        const double cFvB = bench(false), cFvpB = bench(true);
        std::printf("\nCPU (one stereo instance, fraction of realtime on one core):\n");
        std::printf("    Freeverb  : %.4f%%\n", cFvB * 100.0);
        std::printf("    Freeverb+ : %.4f%%   (%.2fx)\n", cFvpB * 100.0, cFvpB / cFvB);
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
