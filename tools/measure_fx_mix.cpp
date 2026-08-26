// What does the FX "Mix" knob actually DO across its travel?
//
// Drives the REAL T5ynthDelayLine / AlgorithmicReverb / ConvolutionReverb the way
// PluginProcessor does (reverbs pure-wet, outer crossfade in the processor) and
// reports, for each knob position, the WET-TO-DRY RATIO at the output in dB.
//
// Meter discipline: the wet-path gain is measured in STEADY STATE on a continuous
// tone (last 2 s of a 5 s render), never over a window where the dry has already
// decayed and only the tail still rings — that window flatters the wet by several
// dB and is exactly how a hot wet path hides from its own test.
//
// Both effects are linear in the mix control (the delay's feedback push uses the
// dry input, not the mixed output; the reverbs always render at internal mix 1.0),
// so ONE pure-wet render per effect gives the whole curve exactly.
//
// Build (after a Release build of the plugin):
//   cd build_clean && grep -m2 -h '^CXX_\(DEFINES\|INCLUDES\|FLAGS\) =' \
//     CMakeFiles/T5ynth.dir/flags.make | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/measure_fx_mix.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/measure_fx_mix
#include "JuceHeader.h"
#include "BinaryData.h"
#include "dsp/BlockParams.h"
#include "dsp/AlgorithmicReverb.h"
#include "dsp/ConvolutionReverb.h"
#include "dsp/DelayLine.h"
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <functional>
#include <vector>

static constexpr double SR = 44100.0;
static constexpr int    BS = 512;
static constexpr int    kBlocks = 430;                 // ~5 s
static constexpr int    kLen    = kBlocks * BS;
static constexpr int    kSettle = static_cast<int>(SR * 3.0);   // measure after 3 s

static double rms(const std::vector<float>& v, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i];
    const int n = to - from;
    return n > 0 ? std::sqrt(s / n) : 0.0;
}

static double db(double x) { return x > 1e-12 ? 20.0 * std::log10(x) : -240.0; }

// Continuous 220 Hz saw at a constant level: a steady excitation, so the wet path
// reaches its true steady-state gain and the dry reference never decays away.
static void makeSteady(std::vector<float>& L, std::vector<float>& R)
{
    L.assign(kLen, 0.0f);
    R.assign(kLen, 0.0f);
    double phase = 0.0;
    const double inc = 220.0 / SR;
    for (int i = 0; i < kLen; ++i)
    {
        const float s = static_cast<float>(0.25 * (2.0 * phase - 1.0));
        L[i] = s; R[i] = s;
        phase += inc;
        if (phase >= 1.0) phase -= 1.0;
    }
}

// Runs a pure-wet effect over the steady source and returns wetRMS / dryRMS.
static double wetGain(const std::function<void(juce::AudioBuffer<float>&)>& process,
                      const std::vector<float>& dryL, const std::vector<float>& dryR)
{
    std::vector<float> outL(kLen, 0.0f), outR(kLen, 0.0f);
    juce::AudioBuffer<float> buf(2, BS);
    for (int b = 0; b < kBlocks; ++b)
    {
        const int off = b * BS;
        for (int i = 0; i < BS; ++i)
        {
            buf.setSample(0, i, dryL[off + i]);
            buf.setSample(1, i, dryR[off + i]);
        }
        process(buf);
        for (int i = 0; i < BS; ++i)
        {
            outL[off + i] = buf.getSample(0, i);
            outR[off + i] = buf.getSample(1, i);
        }
    }
    const double d = rms(dryL, kSettle, kLen);
    const double w = rms(outL, kSettle, kLen);
    return d > 1e-12 ? w / d : 0.0;
}

// Prints the wet/dry ratio the user actually hears across the knob travel.
static void curve(const char* name, double wetRel,
                  const std::function<double(double)>& wetLaw,
                  const std::function<double(double)>& dryLaw)
{
    std::printf("\n%s   (wet path is %+.1f dB vs dry at knob=1.0)\n", name, db(wetRel));
    std::printf("   knob:");
    const double knobs[] = { 0.03, 0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.75, 0.90 };
    for (double k : knobs) std::printf(" %7.2f", k);
    std::printf("\n    W/D:");
    for (double k : knobs)
    {
        const double d = dryLaw(k), w = wetLaw(k);
        std::printf(" %+6.1f ", d > 1e-12 ? db((w * wetRel) / d) : 240.0);
    }
    std::printf(" dB\n  dry at:");
    for (double k : knobs) std::printf(" %+6.1f ", db(dryLaw(k)));
    std::printf(" dB\n");

    auto crossing = [&](double targetDb) {
        for (int i = 1; i <= 1000; ++i)
        {
            const double k = i / 1000.0;
            const double d = dryLaw(k), w = wetLaw(k);
            if (d <= 1e-12) return k;
            if (db((w * wetRel) / d) >= targetDb) return k;
        }
        return 1.0;
    };
    std::printf("  wet reaches -20 dB under dry at knob %.2f | equals dry at knob %.2f\n",
                crossing(-20.0), crossing(0.0));
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::ScopedJuceInitialiser_GUI juceInit;

    std::vector<float> dryL, dryR;
    makeSteady(dryL, dryR);

    std::printf("steady 220 Hz saw, wet gain measured over t=3..5 s\n");

    // ── Delay wet gain per mode (defaults: 250 ms, fb 0.35, damp 0.3) ────────
    // Every DelayType, not just one character per base mode — the trim is applied
    // per type, so every type has to be measured.
    struct { const char* name; int mode; int character; } modes[] = {
        { "Digital     ", 1, 0 }, { "Ping-Pong   ", 2, 0 },
        { "Tape        ", 3, 0 }, { "Tape Warm   ", 3, 1 },
        { "Tape Wild   ", 3, 2 }, { "Tape Old    ", 3, 3 },
        { "BBD         ", 4, 0 }, { "BBD Clean   ", 4, 1 },
        { "BBD Degraded", 4, 2 },
    };
    // fb=0 isolates the wet path's OWN gain (one pass, no regeneration) — that is
    // the honest normalisation target. The fb=0.35 column shows what the user's
    // own feedback setting legitimately adds on top.
    // With the trims in place the fb=0 column must read ~0 dB for EVERY type —
    // that column is the normalisation check, not just a datum.
    std::printf("\n--- DELAY wet-path gain (pure wet, 250 ms / damp 0.3) ---\n");
    std::printf("            fb=0 (path)   fb=0.35 (default)\n");
    double digitalWet = 1.0;
    for (auto& m : modes)
    {
        double g[2] = { 0.0, 0.0 };
        int gi = 0;
        for (float fb : { 0.0f, 0.35f })
        {
            T5ynthDelayLine dl;
            dl.prepare(SR, BS);
            dl.setMode(m.mode);
            dl.setCharacter(m.character);
            dl.setTime(250.0f);
            dl.setFeedback(fb);
            dl.setDamp(0.3f);
            dl.setMix(1.0f);
            g[gi++] = wetGain([&](juce::AudioBuffer<float>& b) { dl.processBlock(b); },
                              dryL, dryR);
        }
        std::printf("  %s : %+7.2f dB    %+7.2f dB\n", m.name, db(g[0]), db(g[1]));
        if (m.mode == 1) digitalWet = g[1];
    }

    // ── Algo reverb wet gain (room 0.7 / damp 0.4 / width 1.0 = the defaults) ─
    double algoWet = 1.0;
    std::printf("\n--- ALGO reverb wet gain vs Room (damp 0.4 / width 1.0) ---\n");
    for (float room : { 0.0f, 0.3f, 0.5f, 0.7f, 1.0f })
    {
        AlgorithmicReverb rv;
        rv.prepare(SR, BS);
        rv.setMix(1.0f);
        rv.setRoomSize(room);
        rv.setDamping(0.4f);
        rv.setWidth(1.0f);
        const double g = wetGain([&](juce::AudioBuffer<float>& b) { rv.processBlock(b); },
                                 dryL, dryR);
        std::printf("  room %.1f : %+6.2f dB%s\n", room, db(g),
                    room == 0.7f ? "   <- default" : "");
        if (room == 0.7f) algoWet = g;
    }

    // ── Plate reverb wet gain per IR, INCLUDING the processor's +6 dB comp ────
    struct { const char* name; const char* data; int size; } irs[] = {
        { "Bright", BinaryData::emt_140_plate_bright_wav, BinaryData::emt_140_plate_bright_wavSize },
        { "Medium", BinaryData::emt_140_plate_medium_wav, BinaryData::emt_140_plate_medium_wavSize },
        { "Dark  ", BinaryData::emt_140_plate_dark_wav,   BinaryData::emt_140_plate_dark_wavSize   },
    };
    // Freeverb+ : the same late field fed from the early-reflection bank.
    double algoPlusWet = 1.0;
    {
        AlgorithmicReverb rv;
        rv.prepare(SR, BS);
        rv.setMix(1.0f);
        rv.setEarlyReflections(true);
        rv.setRoomSize(0.7f);
        rv.setDamping(0.4f);
        rv.setWidth(1.0f);
        algoPlusWet = wetGain([&](juce::AudioBuffer<float>& b) { rv.processBlock(b); },
                              dryL, dryR);
    }

    std::printf("\n--- REVERB wet-path gain (pure wet) ---\n");
    std::printf("  Freeverb  (room 0.7) : %+6.2f dB   -> trim %.3f\n",
                db(algoWet), 1.0 / algoWet);
    std::printf("  Freeverb+ (room 0.7) : %+6.2f dB   -> trim %.3f\n",
                db(algoPlusWet), 1.0 / algoPlusWet);
    // NOTE: a steady tone is the wrong meter for a reverb TAIL, and this loop also
    // has to wait out juce::dsp::Convolution's background IR load — until the swap
    // lands the convolution passes audio through unchanged, which measures as a
    // flattering "the IRs are already unity". tools/measure_reverb_tail.cpp is the
    // meter the reverb trims actually come from; this column is only the
    // steady-state cross-check.
    double plateWet = 1.0;
    for (auto& ir : irs)
    {
        ConvolutionReverb rv;
        rv.prepare(SR, BS);
        rv.setMix(1.0f);
        rv.loadImpulseResponse(ir.data, static_cast<size_t>(ir.size));
        {
            juce::AudioBuffer<float> silence(2, BS);
            for (int i = 0; i < 40; ++i) { juce::Thread::sleep(25); silence.clear(); rv.processBlock(silence); }
        }
        const int type = ir.name[0] == 'B' ? ReverbType::Bright
                       : ir.name[0] == 'M' ? ReverbType::Medium : ReverbType::Dark;
        const double g = wetGain([&](juce::AudioBuffer<float>& b) {
            rv.processBlock(b);
            b.applyGain(FxMixLaw::reverbTrim(type));   // exactly what the processor does
        }, dryL, dryR);
        std::printf("  Plate %s (trimmed)   : %+6.2f dB\n", ir.name, db(g));
        if (ir.name[0] == 'M') plateWet = g;
    }

    // ── The knob curves the user actually turns ──────────────────────────────
    // wetRel here is the residual path gain AFTER trimming — the delay column
    // still carries the user's own fb=0.35 regeneration, which is legitimate.
    std::printf("\n================ SHIPPED LAW (FxMixLaw) ================\n");
    auto wetLaw = [](double m) { return static_cast<double>(FxMixLaw::wetGain(static_cast<float>(m))); };
    auto dryLaw = [](double m) { return static_cast<double>(FxMixLaw::dryGain(static_cast<float>(m))); };
    curve("DELAY Digital (fb 0.35)", digitalWet * FxMixLaw::kDelayTrimClean, wetLaw, dryLaw);
    curve("FREEVERB", algoWet * FxMixLaw::kReverbTrimAlgo, wetLaw, dryLaw);
    curve("FREEVERB+", algoPlusWet * FxMixLaw::kReverbTrimAlgoPlus, wetLaw, dryLaw);
    curve("PLATE REVERB", plateWet, wetLaw, dryLaw);

    // ── Migration check: does a stored value keep its audible balance? ───────
    // Old ratio R_old = wet_old(m)·gain_old/(1-m) must equal the new law's ratio
    // at the migrated position. Anything above ~0.1 dB of drift is a broken remap.
    std::printf("\n================ EPOCH-5 MIGRATION ================\n");
    struct Case { const char* name; int type; bool isDelay; double oldGain; int oldPow; };
    const Case cases[] = {
        { "Delay Digital ", DelayType::Digital, true,  1.0,                             1 },
        { "Delay Tape    ", DelayType::Tape,    true,  1.0 / FxMixLaw::kDelayTrimTape,  1 },
        { "Delay BBD     ", DelayType::Bbd,     true,  1.0 / FxMixLaw::kDelayTrimBbd,   1 },
        { "Reverb Plate  ", ReverbType::Medium, false, 2.0,                             1 },
        { "Reverb Algo   ", ReverbType::Algo,   false, 1.0 / FxMixLaw::kReverbTrimAlgo, 2 },
    };
    double worst = 0.0;
    for (const auto& c : cases)
    {
        std::printf("  %s :", c.name);
        for (double mOld : { 0.03, 0.10, 0.25, 0.30, 0.50, 0.75 })
        {
            const float mNew = c.isDelay ? FxMixLaw::migrateDelayMix(static_cast<float>(mOld), c.type)
                                         : FxMixLaw::migrateReverbMix(static_cast<float>(mOld), c.type);
            const double wetOld = (c.oldPow == 2) ? mOld * mOld : mOld;
            const double rOld   = (wetOld * c.oldGain) / (1.0 - mOld);
            const double rNew   = wetLaw(mNew) / dryLaw(mNew);
            const double drift  = std::fabs(db(rNew) - db(rOld));
            worst = std::max(worst, drift);
            std::printf("  %.2f->%.2f", mOld, mNew);
        }
        std::printf("\n");
    }
    std::printf("  worst wet/dry drift across all cases: %.4f dB %s\n", worst,
                worst < 0.1 ? "(OK)" : "<<< REMAP IS WRONG");

    // ── Modulated Mix must not step at block boundaries ──────────────────────
    // The new dry curve's slope reaches about -8.5 near mix=1.0 (the old law's was
    // a flat -1), so a per-block gain STEP that used to be inaudible would zipper.
    //
    // Meter discipline: the source here is a SINE, not the saw used above. A saw's
    // own wrap discontinuity (~0.73 between adjacent samples) is an order of
    // magnitude larger than any gain step, so a max-jump test on saw input reports
    // "smooth" no matter what the gains do — it would pass with the ramp deleted.
    // On a sine the only discontinuity possible IS the gain step. The test also
    // prints the step that a non-ramped implementation would produce, so a pass
    // means "far below the thing we are testing for", not merely "small".
    std::printf("\n================ MODULATED MIX (LFO on Delay Mix) ================\n");
    {
        std::vector<float> sinL(kLen, 0.0f), sinR(kLen, 0.0f);
        for (int i = 0; i < kLen; ++i)
        {
            const float v = static_cast<float>(0.25 * std::sin(2.0 * M_PI * 220.0 * i / SR));
            sinL[i] = v; sinR[i] = v;
        }

        T5ynthDelayLine dl;
        dl.prepare(SR, BS);
        dl.setMode(1);
        dl.setTime(250.0f);
        dl.setFeedback(0.35f);
        dl.setDamp(0.3f);

        std::vector<float> out(kLen, 0.0f);
        juce::AudioBuffer<float> buf(2, BS);
        double prevMix = 0.0, worstStep = 0.0;
        for (int b = 0; b < kBlocks; ++b)
        {
            // 2 Hz LFO, base 0.85 +/- 0.15 — parked where the dry curve is steepest.
            const double t = static_cast<double>(b * BS) / SR;
            const double m = 0.85 + 0.15 * std::sin(2.0 * M_PI * 2.0 * t);
            dl.setMix(static_cast<float>(m));
            if (b > 0)
            {
                // What an un-ramped implementation would jump by at this boundary:
                // the dry-gain change alone, times the source's own amplitude.
                const double dDry = std::fabs(FxMixLaw::dryGain(static_cast<float>(m))
                                            - FxMixLaw::dryGain(static_cast<float>(prevMix)));
                worstStep = std::max(worstStep, dDry * 0.25);
            }
            prevMix = m;
            const int off = b * BS;
            for (int i = 0; i < BS; ++i)
            {
                buf.setSample(0, i, sinL[off + i]);
                buf.setSample(1, i, sinR[off + i]);
            }
            dl.processBlock(buf);
            for (int i = 0; i < BS; ++i) out[off + i] = buf.getSample(0, i);
        }

        double innerMax = 0.0, boundaryMax = 0.0;
        for (int i = 1; i < kLen; ++i)
        {
            const double d = std::fabs(out[i] - out[i - 1]);
            if (i % BS == 0) boundaryMax = std::max(boundaryMax, d);
            else             innerMax    = std::max(innerMax, d);
        }
        // If the gains stepped, a boundary sample would carry the signal's own
        // slope PLUS the whole gain change: innerMax + worstStep. The test is
        // whether the measured boundary sits at innerMax or at that sum.
        const double ifStepped = innerMax + worstStep;
        std::printf("  largest jump inside blocks       : %.6f\n", innerMax);
        std::printf("  largest jump AT a block boundary : %.6f\n", boundaryMax);
        std::printf("  un-ramped build would reach      : %.6f  (%.1fx the natural slope)\n",
                    ifStepped, ifStepped / innerMax);
        std::printf("  %s\n", boundaryMax <= innerMax * 1.05
                    ? "ramped: boundaries are indistinguishable from the signal's own slope"
                    : "<<< ZIPPER: boundaries stand out");
    }
    return 0;
}
