// Plugin-path-exact audition for a DCO bake: recipe JSON (backend output)
// -> dco::recipeFromVar -> dco::Baker -> WavetableOscillator configured
// EXACTLY like T5ynthProcessor::loadDcoWavetable — including the auto-scan
// transport (setAutoScanRate over the strip length, Loop over [0,1], the 5 ms
// scan smoother from prepare(), retriggerAutoScan as note-on).
//
// This exists because tools/audition_dco_bake.cpp drives scan MANUALLY via
// setScanPosition and therefore never exercised the plugin's auto-scan wrap,
// which is where the user heard periodic dropouts. It renders BOTH table
// load paths:
//   OLD  = extractContiguousFrames (neural seam ramp + per-frame renorm,
//          Loop wrap slews smoothedScan back through the whole table)
//   NEW  = setExactFrames (bit-exact frames, content-seamless wrap jump)
// and prints: frames-count fidelity, per-frame corruption vs the raw strip,
// and a wrap-glitch metric (first-difference energy in windows centered on
// the expected wrap times vs the median window). Exits non-zero if the NEW
// path is not bit-exact or still spikes at wraps.
//
// Build: same recipe as audition_dco_bake.cpp (flags.make response file +
// lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this]). Run from the repo root:
//   /tmp/dco_pp <recipe.json> [outDir=tools/dco_audition_out]
#include "JuceHeader.h"
#include "dsp/DcoBaker.h"
#include "dsp/DcoRecipeJson.h"
#include "dsp/WavetableOscillator.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

static void writeWav(const std::string& path, const std::vector<float>& mono, int sr)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v){ f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](uint16_t v){ f.write(reinterpret_cast<char*>(&v), 2); };
    uint32_t dataBytes = static_cast<uint32_t>(mono.size()) * 2u;
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(static_cast<uint32_t>(sr));
    u32(static_cast<uint32_t>(sr) * 2); u16(2); u16(16);
    f.write("data", 4); u32(dataBytes);
    for (float s : mono) {
        int v = static_cast<int>(std::lround(std::max(-1.f, std::min(1.f, s)) * 32767.f));
        u16(static_cast<uint16_t>(static_cast<int16_t>(v)));
    }
}

// The OLD shipped traversal (pre-fix loadDcoWavetable): sampler-style
// auto-scan piggyback — rate = strip duration, Loop wrap through the smoother.
static void configureOldTraversal(WavetableOscillator& osc, int sr, int stripSamples)
{
    osc.setAutoScanStartPos(0.0f);
    osc.setAutoScanLoop(0.0f, 1.0f, WavetableOscillator::LoopMode::Loop);
    osc.setAutoScan(true);
    osc.setAutoScanRate(static_cast<double>(sr), stripSamples);
    osc.setMorphTimeMs(200.0f);
}

// The NEW traversal (current loadDcoWavetable): dedicated DCO motion
// transport — recipe rate (fallback: legacy strip-length rate), exact wrap.
static void configureNewTraversal(WavetableOscillator& osc, int sr, int stripSamples,
                                  float recipeRateHz)
{
    osc.setAutoScan(false);
    osc.setDcoMotion(true, recipeRateHz > 0.0f
        ? recipeRateHz
        : static_cast<float>(static_cast<double>(sr) / stripSamples));
    osc.setMorphTimeMs(200.0f);
}

static std::vector<float> renderAutoScan(WavetableOscillator& osc, double freqHz,
                                         double seconds, int sr)
{
    osc.reset();
    osc.retriggerAutoScan();     // note-on
    osc.setFrequency(static_cast<float>(freqHz));
    const int total = static_cast<int>(seconds * sr);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(total));
    for (int i = 0; i < total; ++i)
        out.push_back(osc.processSample());
    return out;
}

// First-difference energy in a window — a fast reverse scan sweep or a click
// shows up as a spike relative to the steady-state median.
static double diffEnergy(const std::vector<float>& x, int from, int to)
{
    from = std::max(1, from);
    to = std::min<int>(static_cast<int>(x.size()), to);
    double e = 0.0;
    for (int i = from; i < to; ++i)
    {
        const double d = static_cast<double>(x[i]) - static_cast<double>(x[i - 1]);
        e += d * d;
    }
    return e / std::max(1, to - from);
}

struct WrapMetric { double ratio1 = 0, ratio2 = 0, ratio3 = 0; };

static WrapMetric wrapGlitchMetric(const std::vector<float>& x, int sr, double sweepSeconds)
{
    // Median diff-energy over uniform 30 ms windows (glitch windows included —
    // 3 outliers among ~400 windows do not move the median).
    const int win = sr * 30 / 1000;
    std::vector<double> all;
    for (int start = sr / 2; start + win < static_cast<int>(x.size()); start += win)
        all.push_back(diffEnergy(x, start, start + win));
    std::vector<double> sorted = all;
    std::sort(sorted.begin(), sorted.end());
    const double median = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];

    auto around = [&](int k) {
        const int c = static_cast<int>(k * sweepSeconds * sr);
        // The wrap lands within one smoother τ (~5 ms); scan ±45 ms and take
        // the hottest 30 ms window so timing jitter can't hide the spike.
        double worst = 0.0;
        for (int off = -sr * 45 / 1000; off <= sr * 15 / 1000; off += win / 4)
            worst = std::max(worst, diffEnergy(x, c + off, c + off + win));
        return median > 0.0 ? worst / median : 0.0;
    };
    return { around(1), around(2), around(3) };
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s <recipe.json> [outDir]\n", argv[0]); return 2; }
    const std::string outDir = argc > 2 ? argv[2] : "tools/dco_audition_out";
    std::filesystem::create_directories(outDir);

    juce::File recipeFile(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]));
    const auto parsed = juce::JSON::parse(recipeFile.loadFileAsString());
    const dco::Recipe recipe = dco::recipeFromVar(parsed);
    std::printf("recipe.frames (through dco::recipeFromVar) = %d\n", recipe.frames);
    std::printf("recipe.keyframes = %d, loop = %s\n",
                static_cast<int>(recipe.keyframes.size()), recipe.loop ? "true" : "false");

    const auto baked = dco::Baker::bake(recipe);
    juce::AudioBuffer<float> strip = dco::Baker::framesToBuffer(baked);
    const int stripSamples = strip.getNumSamples();
    std::printf("baked frames = %d, strip samples = %d (%.3f s sweep at 48k)\n",
                static_cast<int>(baked.size()), stripSamples, stripSamples / 48000.0);

    // Raw-strip cycle boundary gap: what extractContiguousFrames' seam ramp
    // will smear across each full cycle as an additive linear ramp.
    const float* raw = strip.getReadPointer(0);
    const int F = WavetableOscillator::FRAME_SIZE;
    double maxGap = 0.0, meanGap = 0.0;
    const int nRawFrames = stripSamples / F;
    for (int k = 0; k < nRawFrames; ++k)
    {
        const double gap = std::abs(static_cast<double>(raw[k * F]) - raw[k * F + F - 1]);
        maxGap = std::max(maxGap, gap);
        meanGap += gap;
    }
    meanGap /= std::max(1, nRawFrames);
    std::printf("raw cycle boundary |first-last|: max %.4f mean %.4f (ramp adds this full-scale across the cycle)\n",
                maxGap, meanGap);

    const int sr = 48000;
    bool ok = true;

    auto measureLoadedDelta = [&](WavetableOscillator& osc, const char* label)
    {
        std::vector<float> flat; int fs = 0, nf = 0;
        if (!osc.snapshotLevel0Frames(flat, fs, nf)) { std::printf("%s: snapshot FAILED\n", label); return 1.0e9; }
        double maxDelta = 0.0;
        const int cmpFrames = std::min(nf, nRawFrames);
        for (int k = 0; k < cmpFrames; ++k)
            for (int i = 0; i < fs; ++i)
                maxDelta = std::max(maxDelta,
                    std::abs(static_cast<double>(flat[static_cast<size_t>(k) * fs + i]) - raw[k * F + i]));
        std::printf("%s: table frames = %d, max |loaded - baked| = %.6f\n", label, nf, maxDelta);
        return maxDelta;
    };

    // OLD transport sweeps at the strip-length rate; NEW at the recipe's
    // authored tempo (fallback = the same legacy rate when unspecified).
    const double sweepSecOld = static_cast<double>(stripSamples) / sr;
    const double sweepSecNew = recipe.motionRateHz > 0.0f
        ? 1.0 / static_cast<double>(recipe.motionRateHz)
        : sweepSecOld;

    // OLD path: extraction (seam ramp + renorm), wrap slews through the table.
    {
        WavetableOscillator osc;
        osc.prepare(sr, 512);
        osc.extractContiguousFrames(strip, sr, 0.0f, 1.0f);
        configureOldTraversal(osc, sr, stripSamples);
        measureLoadedDelta(osc, "OLD extractContiguousFrames");
        auto audio = renderAutoScan(osc, 220.0, 12.0, sr);
        const auto m = wrapGlitchMetric(audio, sr, sweepSecOld);
        std::printf("OLD wrap diff-energy ratio vs median: wrap1 %.1fx wrap2 %.1fx wrap3 %.1fx\n",
                    m.ratio1, m.ratio2, m.ratio3);
        writeWav(outDir + "/dcoPP_OLD_A3.wav", audio, sr);
    }

    // NEW path: bit-exact adoption + dedicated DCO motion transport.
    {
        WavetableOscillator osc;
        osc.prepare(sr, 512);
        osc.setExactFrames(strip);
        configureNewTraversal(osc, sr, stripSamples, recipe.motionRateHz);
        const double delta = measureLoadedDelta(osc, "NEW setExactFrames");
        if (delta > 1.0e-6) { std::printf("FAIL: NEW path not bit-exact\n"); ok = false; }
        auto audio = renderAutoScan(osc, 220.0, 12.0, sr);
        const auto m = wrapGlitchMetric(audio, sr, sweepSecNew);
        std::printf("NEW wrap diff-energy ratio vs median (rate %.3f Hz): wrap1 %.1fx wrap2 %.1fx wrap3 %.1fx\n",
                    recipe.motionRateHz > 0.0f ? recipe.motionRateHz : 1.0 / sweepSecOld,
                    m.ratio1, m.ratio2, m.ratio3);
        if (m.ratio1 > 3.0 || m.ratio2 > 3.0 || m.ratio3 > 3.0)
        {
            std::printf("FAIL: NEW path still spikes at loop wraps\n");
            ok = false;
        }
        writeWav(outDir + "/dcoPP_NEW_A3.wav", audio, sr);

        auto hi = renderAutoScan(osc, 1046.5, 4.0, sr);   // C6 mip check
        writeWav(outDir + "/dcoPP_NEW_C6.wav", hi, sr);
    }

    // MORPH EDGE — the held-note transport flip (platform invariant: a held
    // note crossfades click-free to the new table). A voice shares a DCO
    // master's table+motion, holds a note for 2 s (dcoMotionPos_ = 0.70),
    // then morphToFramesFrom() re-targets it to a neural-style master
    // (extractContiguousFrames + auto-scan), and 2 s later back to the DCO
    // master with a stale dcoMotionPos_. syncSharedConfigFrom flips the
    // transport flag mid-render on both edges; without the smoothedScan fold
    // scanNow steps by the whole motion offset in ONE sample at full
    // outgoing morph gain (0.70 -> 0.00 here = a ~44-frame table jump).
    //
    // PWM content hides such a step behind its own pulse edges (|diff| ~2 at
    // every edge), so this scenario bakes a synthetic SMOOTH table instead:
    // frame k blends sin(2*pi*x) -> sin(6*pi*x) and back (palindrome — real
    // bakes are content-closed the same way: the backend validator forces
    // loop recipes to end on their start keyframe, so the motion wrap is
    // seamless; an open table would poison the steady-state baseline with
    // its own wrap step). At 220 Hz the content bounds the per-sample first
    // difference to ~0.09; an un-folded flip jumps to |diff| ~1.0 in one
    // sample — an order of magnitude above the bound.
    {
        const int nf = 64;
        juce::AudioBuffer<float> smooth(1, nf * F);
        float* w = smooth.getWritePointer(0);
        for (int k = 0; k < nf; ++k)
        {
            const float t = 1.0f - std::abs(2.0f * static_cast<float>(k) / (nf - 1) - 1.0f);
            for (int i = 0; i < F; ++i)
            {
                const float x = static_cast<float>(i) / F;
                w[k * F + i] = (1.0f - t) * std::sin(2.0f * juce::MathConstants<float>::pi * x)
                             + t * std::sin(6.0f * juce::MathConstants<float>::pi * x);
            }
        }

        WavetableOscillator masterDco;
        masterDco.prepare(sr, 512);
        masterDco.setExactFrames(smooth);
        configureNewTraversal(masterDco, sr, smooth.getNumSamples(), 0.35f);

        WavetableOscillator masterNeural;
        masterNeural.prepare(sr, 512);
        // Published twice: generation counters are per-instance, so a single
        // publish would collide with masterDco's generation 1 and
        // morphToFramesFrom would dedupe the morph away. The plugin has one
        // master per engine (single counter) — harness-only artifact.
        masterNeural.extractContiguousFrames(smooth, sr, 0.0f, 1.0f);
        masterNeural.extractContiguousFrames(smooth, sr, 0.0f, 1.0f);
        configureOldTraversal(masterNeural, sr, smooth.getNumSamples());

        WavetableOscillator voice;
        voice.prepare(sr, 512);
        voice.shareFramesFrom(masterDco);       // note-on lands on the DCO table
        voice.reset();
        voice.retriggerAutoScan();
        voice.setFrequency(220.0f);

        // Flip timing must NOT land on an integer cycle count: at 220 Hz a
        // 2.000 s hold is exactly 440.0 cycles, so the flip hits phase x = 0
        // — a zero crossing SHARED by every frame (all partials sin(2*pi*k*0)
        // = 0), where even a hard 44-frame scan step is invisible. Falsified
        // empirically: with the fold disabled the un-tuned test still passed.
        // These holds land each flip at phase ~0.75, where the two partials
        // disagree maximally (|frame delta| ~1.2 full-scale).
        const int hold1 = 2 * sr + 164;         // flip A: phase 0.752, dcoMotionPos_ = 0.701
        const int hold2 = 2 * sr + 219;         // flip B: phase 0.755, stale pos 0.701
        const int hold3 = 2 * sr;
        std::vector<float> audio;
        audio.reserve(static_cast<size_t>(hold1 + hold2 + hold3));
        for (int i = 0; i < hold1; ++i) audio.push_back(voice.processSample());
        voice.morphToFramesFrom(masterNeural);  // flip A: DCO -> neural, held
        for (int i = 0; i < hold2; ++i) audio.push_back(voice.processSample());
        voice.morphToFramesFrom(masterDco);     // flip B: re-bake, stale motion pos
        for (int i = 0; i < hold3; ++i) audio.push_back(voice.processSample());

        auto maxDiffIn = [&](int from, int to) {
            from = std::max(1, from);
            to = std::min<int>(static_cast<int>(audio.size()), to);
            double m = 0.0;
            for (int i = from; i < to; ++i)
                m = std::max(m, std::abs(static_cast<double>(audio[i]) - audio[i - 1]));
            return m;
        };
        const int guard = sr / 20;              // ±50 ms around each flip
        const int fA = hold1, fB = hold1 + hold2, end = hold1 + hold2 + hold3;
        const double flipA = maxDiffIn(fA - guard, fA + guard);
        const double flipB = maxDiffIn(fB - guard, fB + guard);
        const double steady = std::max({ maxDiffIn(1, fA - guard),
                                         maxDiffIn(fA + guard, fB - guard),
                                         maxDiffIn(fB + guard, end) });
        std::printf("MORPH flip max|diff|: DCO->neural %.4f, neural->DCO %.4f, steady %.4f\n",
                    flipA, flipB, steady);
        if (flipA > 4.0 * steady || flipB > 4.0 * steady)
        {
            std::printf("FAIL: transport flip steps scanNow on a held note (invariant violation)\n");
            ok = false;
        }
        writeWav(outDir + "/dcoPP_MORPH_flip_A3.wav", audio, sr);
    }

    std::printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}
