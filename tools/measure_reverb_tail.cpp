// How LOUD is each reverb's tail? — the meter the steady-tone one could not be.
//
// tools/measure_fx_mix.cpp measures wet-path gain in STEADY STATE, which is right
// for the delay and right for setting a mix law, but it is blind to the property a
// reverb is actually judged by: the level of the tail once the note has stopped.
//
// In steady state a convolution reverb's gain is |input| * sqrt(sum h^2), and JUCE
// normalises every loaded IR to unit energy (Convolution::Normalise::yes is the
// default, juce_Convolution.cpp), so ALL THREE EMT-140 IRs measure identically at
// +0.00 dB there — while by ear the Bright plate is nearly inaudible and the Dark
// one is obvious. Equal ENERGY is not equal LOUDNESS: an IR that spreads the same
// energy over a longer, denser decay is quieter at every instant.
//
// So this meter plays a NOTE and listens AFTER it, which is what a player does:
//   - 300 ms of 220 Hz saw with a short attack, then silence
//   - tail RMS over the 200 ms immediately after the note stops
//   - tail RMS over the following second, to catch decay-length differences
//   - and, for reference, the steady-state figure the other meter reports
//
// Freeverb at Room 0.7 is the ANCHOR: its level is the one that was judged right,
// so the other types are reported relative to it and trimmed onto it.
//
// Build (after a Release build of the plugin):
//   cd build_clean && grep -m2 -h '^CXX_\(DEFINES\|INCLUDES\|FLAGS\) =' \
//     CMakeFiles/T5ynth.dir/flags.make | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/measure_reverb_tail.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     build_clean/T5ynth_artefacts/JuceLibraryCode/../../libT5ynthData.a \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/measure_reverb_tail
#include "JuceHeader.h"
#include "BinaryData.h"
#include "dsp/BlockParams.h"
#include "dsp/AlgorithmicReverb.h"
#include "dsp/ConvolutionReverb.h"
#include <cstdio>
#include <cmath>
#include <functional>
#include <vector>
#include <array>

static constexpr double SR = 44100.0;
static constexpr int    BS = 512;
static constexpr int    kLen = 44100 * 4;              // 4 s
static constexpr int    kNoteLen = static_cast<int>(SR * 0.3);

static double rms(const std::vector<float>& v, int from, int to)
{
    from = juce::jmax(0, from);
    to   = juce::jmin(static_cast<int>(v.size()), to);
    if (to <= from) return 0.0;
    double s = 0.0;
    for (int i = from; i < to; ++i) s += static_cast<double>(v[i]) * v[i];
    return std::sqrt(s / static_cast<double>(to - from));
}
static double peakOf(const std::vector<float>& v)
{
    double p = 0.0;
    for (float s : v) p = juce::jmax(p, std::abs(static_cast<double>(s)));
    return p;
}
static double db(double x) { return 20.0 * std::log10(juce::jmax(1.0e-12, x)); }

// 220 Hz saw, 300 ms, 5 ms attack and 10 ms release so the stimulus itself has no
// click of its own to be mistaken for reverb.
static void makeNote(std::vector<float>& out)
{
    out.assign(kLen, 0.0f);
    double ph = 0.0;
    const double inc = 220.0 / SR;
    for (int i = 0; i < kNoteLen; ++i)
    {
        double env = 1.0;
        const double t = i / SR;
        if (t < 0.005)                       env = t / 0.005;
        if (i > kNoteLen - static_cast<int>(SR * 0.01))
            env = static_cast<double>(kNoteLen - i) / (SR * 0.01);
        out[i] = static_cast<float>((2.0 * ph - 1.0) * 0.5 * env);
        ph += inc;
        if (ph >= 1.0) ph -= 1.0;
    }
}

// Steady 220 Hz saw for the steady-state reference column.
static void makeSteady(std::vector<float>& out)
{
    out.assign(kLen, 0.0f);
    double ph = 0.0;
    const double inc = 220.0 / SR;
    for (int i = 0; i < kLen; ++i)
    {
        out[i] = static_cast<float>((2.0 * ph - 1.0) * 0.5);
        ph += inc;
        if (ph >= 1.0) ph -= 1.0;
    }
}

// Renders the source through `proc` (pure wet) and keeps BOTH channels.
// Mono-summing here would be a meter bug: a reverb's two channels are
// decorrelated, so a sum loses up to 3 dB of exactly the reverbs that are widest,
// while the correlated dry loses nothing. Levels are therefore compared the way
// BS.1770 does it — by summing channel POWERS.
using Stereo = std::array<std::vector<float>, 2>;

static Stereo render(const std::vector<float>& src,
                     const std::function<void(juce::AudioBuffer<float>&)>& proc)
{
    Stereo out { std::vector<float>(kLen, 0.0f), std::vector<float>(kLen, 0.0f) };
    juce::AudioBuffer<float> buf(2, BS);
    for (int off = 0; off + BS <= kLen; off += BS)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < BS; ++i)
                buf.setSample(ch, i, src[static_cast<size_t>(off + i)]);
        proc(buf);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < BS; ++i)
                out[static_cast<size_t>(ch)][static_cast<size_t>(off + i)] = buf.getSample(ch, i);
    }
    return out;
}

static double rmsS(const Stereo& s, int from, int to)
{
    const double l = rms(s[0], from, to), r = rms(s[1], from, to);
    return std::sqrt(0.5 * (l * l + r * r));
}
static double peakS(const Stereo& s)
{
    return juce::jmax(peakOf(s[0]), peakOf(s[1]));
}

// ── ITU-R BS.1770 K-weighting ────────────────────────────────────────────────
// The tail window answers "how loud is the reverb AFTER the note", the steady
// window answers "how much does it add DURING one", and for the plates those two
// disagree by up to 8 dB — a steady tone only samples the reverb where the saw's
// harmonics sit, and a bright IR has little of its energy down there. Loudness
// over the whole event is the meter that settles it, so this is the broadcast one
// (high-shelf + high-pass, RBJ biquads at the actual sample rate), integrated over
// note AND tail together.
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double process(double x)
    {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y;
        return y;
    }
};

static Biquad makeHighShelf(double fc, double gainDb, double q, double sr)
{
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w = 2.0 * juce::MathConstants<double>::pi * fc / sr;
    const double cw = std::cos(w), sw = std::sin(w);
    const double alpha = sw / (2.0 * q);
    const double sq = 2.0 * std::sqrt(A) * alpha;
    const double b0 =      A * ((A + 1) + (A - 1) * cw + sq);
    const double b1 = -2 * A * ((A - 1) + (A + 1) * cw);
    const double b2 =      A * ((A + 1) + (A - 1) * cw - sq);
    const double a0 =           (A + 1) - (A - 1) * cw + sq;
    const double a1 =      2 * ((A - 1) - (A + 1) * cw);
    const double a2 =           (A + 1) - (A - 1) * cw - sq;
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, 0, 0, 0, 0 };
}

static Biquad makeHighPass(double fc, double q, double sr)
{
    const double w = 2.0 * juce::MathConstants<double>::pi * fc / sr;
    const double cw = std::cos(w), sw = std::sin(w);
    const double alpha = sw / (2.0 * q);
    const double b0 =  (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2;
    const double a0 =   1 + alpha,   a1 = -2 * cw,   a2 = 1 - alpha;
    return { b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, 0, 0, 0, 0 };
}

// Mean-square K-weighted level of the whole render, as a linear RMS-like number.
static double kLoudness1(const std::vector<float>& v)
{
    Biquad shelf = makeHighShelf(1681.97, 3.999, 0.7071, SR);
    Biquad hp    = makeHighPass(38.13, 0.5003, SR);
    double sum = 0.0;
    for (float s : v)
    {
        const double y = hp.process(shelf.process(static_cast<double>(s)));
        sum += y * y;
    }
    return std::sqrt(sum / static_cast<double>(v.size()));
}

static double kLoudness(const Stereo& s)
{
    const double l = kLoudness1(s[0]), r = kLoudness1(s[1]);
    return std::sqrt(0.5 * (l * l + r * r));
}

struct Row { const char* name; double tail200; double tail1s; double steady; double loud; double peak; };

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::vector<float> note, steady;
    makeNote(note);
    makeSteady(steady);

    const int tailFrom = kNoteLen;
    const int tail200  = kNoteLen + static_cast<int>(SR * 0.2);
    const int tail1s   = kNoteLen + static_cast<int>(SR * 1.2);
    const int stFrom   = static_cast<int>(SR * 2.0);

    const double dryTail200 = rms(note, tailFrom, tail200);   // = 0, the note is over
    const double drySteady  = rms(steady, stFrom, kLen);
    juce::ignoreUnused(dryTail200);

    std::vector<Row> rows;

    auto measureAlgo = [&](const char* name, bool er)
    {
        AlgorithmicReverb rv;
        rv.prepare(SR, BS);
        rv.setMix(1.0f);
        rv.setEarlyReflections(er);
        rv.setRoomSize(0.7f);
        rv.setDamping(0.4f);
        rv.setWidth(1.0f);
        auto wet = render(note, [&](juce::AudioBuffer<float>& b) { rv.processBlock(b); });

        AlgorithmicReverb rv2;
        rv2.prepare(SR, BS);
        rv2.setMix(1.0f);
        rv2.setEarlyReflections(er);
        rv2.setRoomSize(0.7f);
        rv2.setDamping(0.4f);
        rv2.setWidth(1.0f);
        auto wetS = render(steady, [&](juce::AudioBuffer<float>& b) { rv2.processBlock(b); });

        rows.push_back({ name,
                         rmsS(wet, tailFrom, tail200),
                         rmsS(wet, tail200,  tail1s),
                         rmsS(wetS, stFrom, kLen) / drySteady,
                         kLoudness(wet),
                         peakS(wet) });
    };

    // juce::dsp::Convolution loads its IR on a BACKGROUND thread and passes audio
    // through UNCHANGED until the swap happens. Measuring straight after
    // loadImpulseResponse() therefore measures the dry signal and reports a
    // perfectly plausible "+0.00 dB, the IRs are already unity" — which is exactly
    // what the first version of this measurement did, and what put a wrong plate
    // trim into the shipped table. So: wait, warm up, and then REFUSE to report a
    // plate whose tail is still silent.
    auto warmUp = [&](ConvolutionReverb& rv)
    {
        juce::AudioBuffer<float> silence(2, BS);
        for (int attempt = 0; attempt < 40; ++attempt)
        {
            juce::Thread::sleep(25);
            silence.clear();
            rv.processBlock(silence);
        }
    };

    auto measurePlate = [&](const char* name, const char* data, int size)
    {
        ConvolutionReverb rv;
        rv.prepare(SR, BS);
        rv.setMix(1.0f);
        rv.loadImpulseResponse(data, static_cast<size_t>(size));
        warmUp(rv);
        auto wet = render(note, [&](juce::AudioBuffer<float>& b) { rv.processBlock(b); });

        ConvolutionReverb rv2;
        rv2.prepare(SR, BS);
        rv2.setMix(1.0f);
        rv2.loadImpulseResponse(data, static_cast<size_t>(size));
        warmUp(rv2);
        auto wetS = render(steady, [&](juce::AudioBuffer<float>& b) { rv2.processBlock(b); });

        if (rmsS(wet, tailFrom, tail200) < 1.0e-6)
        {
            std::printf("  !! %s: no tail at all - the IR never became active, "
                        "this measurement is INVALID\n", name);
            std::exit(2);
        }

        rows.push_back({ name,
                         rmsS(wet, tailFrom, tail200),
                         rmsS(wet, tail200,  tail1s),
                         rmsS(wetS, stFrom, kLen) / drySteady,
                         kLoudness(wet),
                         peakS(wet) });
    };

    measureAlgo("Freeverb  (anchor)", false);
    measureAlgo("Freeverb+         ", true);
    measurePlate("Plate Dark        ", BinaryData::emt_140_plate_dark_wav,   BinaryData::emt_140_plate_dark_wavSize);
    measurePlate("Plate Medium      ", BinaryData::emt_140_plate_medium_wav, BinaryData::emt_140_plate_medium_wavSize);
    measurePlate("Plate Bright      ", BinaryData::emt_140_plate_bright_wav, BinaryData::emt_140_plate_bright_wavSize);

    const double anchor200 = rows[0].tail200;
    const double anchor1s  = rows[0].tail1s;
    const double anchorLd  = rows[0].loud;

    std::printf("\n  UNTRIMMED wet paths, 220 Hz saw note of 300 ms, pure wet\n");
    std::printf("  %-20s %10s %10s   %10s %10s   %10s\n",
                "", "tail 0-200ms", "vs anchor", "tail 0.2-1.2s", "vs anchor", "steady");
    for (auto& r : rows)
        std::printf("  %-20s %10.5f %9.2f dB %10.5f %9.2f dB %9.2f dB\n",
                    r.name, r.tail200, db(r.tail200 / anchor200),
                    r.tail1s, db(r.tail1s / anchor1s), db(r.steady));

    std::printf("\n  K-WEIGHTED LOUDNESS of the whole event (note + tail)\n");
    for (auto& r : rows)
        std::printf("  %-20s %10.5f %9.2f dB   wet peak %.3f\n",
                    r.name, r.loud, db(r.loud / anchorLd), r.peak);

    std::printf("\n  TRIM candidates, and what each does to the wet peak at Mix 1.0\n");
    std::printf("  %-20s %8s %8s %8s   %10s\n", "", "by tail", "by loud", "chosen", "peak@chosen");
    for (auto& r : rows)
    {
        const double byTail = r.tail200 > 0.0 ? anchor200 / r.tail200 : 1.0;
        const double byLoud = r.loud > 0.0 ? anchorLd / r.loud : 1.0;
        const double chosen = byLoud;
        std::printf("  %-20s %8.3f %8.3f %8.3f   %10.3f\n",
                    r.name, byTail, byLoud, chosen, r.peak * chosen);
    }

    // The point of a trim table is that every type lands on the anchor. Say so
    // with numbers rather than trusting the arithmetic above.
    std::printf("\n  AFTER the SHIPPED trim - every type should sit on the anchor\n");
    {
        const int types[] = { ReverbType::Algo, ReverbType::AlgoPlus,
                              ReverbType::Dark, ReverbType::Medium, ReverbType::Bright };
        const double anchorTrimmed = rows[0].loud * FxMixLaw::reverbTrim(ReverbType::Algo);
        double worst = 0.0;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const double l = rows[i].loud * FxMixLaw::reverbTrim(types[i]);
            const double d = db(l / anchorTrimmed);
            worst = juce::jmax(worst, std::abs(d));
            std::printf("  %-20s %10.5f %9.2f dB   wet peak %.3f\n",
                        rows[i].name, l, d, rows[i].peak * FxMixLaw::reverbTrim(types[i]));
        }
        std::printf("  worst deviation from the anchor: %.2f dB   %s\n",
                    worst, worst < 1.0 ? "ok" : "OUT OF TOLERANCE");
    }

    std::printf("\n  SHIPPED trims for comparison\n");
    std::printf("  %-20s  %.3f\n", "Freeverb          ", FxMixLaw::kReverbTrimAlgo);
    std::printf("  %-20s  %.3f\n", "Freeverb+         ", FxMixLaw::kReverbTrimAlgoPlus);
    std::printf("  %-20s  %.3f\n", "Plate Dark        ", FxMixLaw::reverbTrim(ReverbType::Dark));
    std::printf("  %-20s  %.3f\n", "Plate Medium      ", FxMixLaw::reverbTrim(ReverbType::Medium));
    std::printf("  %-20s  %.3f\n", "Plate Bright      ", FxMixLaw::reverbTrim(ReverbType::Bright));
    std::printf("\n");
    return 0;
}
