// Offline audition for the sampler's loop and ping-pong seams.
//
// Loads a WAV (a preset's generated audio, say), prepares it exactly as the
// plugin does (SamplePlayer::loadBuffer with the given loop mode, crossfade,
// optimizer level and P2/P3 fractions), renders a shared voice at a given
// transposition ratio through several loop passes, and measures every seam:
//
//   step   max per-sample delta within +-64 samples of the seam, divided by
//          the source material's own 99.9th-percentile per-sample delta inside
//          the loop region (1.0 = the material does this itself; >> 1 = click)
//   hfdB   energy of the second difference (a crude high-pass, what a click
//          puts into the top octaves) in the worst 5 ms window at the seam, in
//          dB over the LOUDER of the two neighbouring stretches of the output
//          (7.5-30 ms before and after): a click exceeds both neighbours, bright
//          content at the seam does not
//
// Seam instants are taken from the voice's own read position (wrap = backward
// jump, turnaround = direction flip), not guessed from the output.
//
// Usage:
//   audition_loop_seams <wav> <loop|pingpong> <opt 0..2> <xfadeMs> <lsFrac> <leFrac>
//                       [ratio=1.0] [seconds=auto] [out.wav] [startFrac=lsFrac] [normalize=0]
//
// startFrac is the preset's P1 (where a note starts; the bank's presets mostly
// start BEFORE the loop, so the first seam is the first pass's own); normalize
// 1 prepares the buffer with the Normalize gain the presets all carry.
// Prints one summary line "RESULT ..." per run; exit code 0 always (this is a
// meter, not a gate — tools/measure_loop_seams.sh aggregates it over material).
//
// Build: tools/build_offline_tool.sh audition_loop_seams [outdir]
// Run:   the binary dlopens Csound via @loader_path/../libs, so place it in a
//        bin/ directory beside a libs/ copy of third_party/csound/macos-arm64/lib.

#include "JuceHeader.h"
#include "dsp/SamplePlayer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static bool readWavMono(const std::string& path, std::vector<float>& mono, double& sr)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r(fm.createReaderFor(juce::File(path)));
    if (r == nullptr) return false;
    sr = r->sampleRate;
    const int n = static_cast<int>(r->lengthInSamples);
    juce::AudioBuffer<float> buf(static_cast<int>(r->numChannels), n);
    r->read(&buf, 0, n, 0, true, true);
    // The sampler reads channel 0 only (cubicSampleFrom); so does this meter.
    mono.assign(buf.getReadPointer(0), buf.getReadPointer(0) + n);
    return true;
}

static void writeWav(const std::string& path, const std::vector<float>& mono, int sr)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v){ f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](uint16_t v){ f.write(reinterpret_cast<char*>(&v), 2); };
    uint32_t dataBytes = static_cast<uint32_t>(mono.size()) * 2u;
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(static_cast<uint32_t>(sr));
    u32(static_cast<uint32_t>(sr * 2)); u16(2); u16(16);
    f.write("data", 4); u32(dataBytes);
    for (float s : mono) {
        int v = static_cast<int>(std::lround(std::max(-1.f, std::min(1.f, s)) * 32767.f));
        u16(static_cast<uint16_t>(static_cast<int16_t>(v)));
    }
}

static float percentile(std::vector<float> v, double p)
{
    if (v.empty()) return 0.0f;
    size_t k = static_cast<size_t>(std::min<double>(v.size() - 1, std::floor(p * (v.size() - 1))));
    std::nth_element(v.begin(), v.begin() + static_cast<long>(k), v.end());
    return v[k];
}

int main(int argc, char** argv)
{
    if (argc < 7) {
        std::fprintf(stderr, "usage: %s <wav> <loop|pingpong> <opt 0..2> <xfadeMs> <lsFrac> <leFrac> [ratio] [seconds] [out.wav] [startFrac] [normalize]\n", argv[0]);
        return 2;
    }
    const std::string wavPath = argv[1];
    const std::string modeStr = argv[2];
    const int opt = std::atoi(argv[3]);
    const float xfMs = static_cast<float>(std::atof(argv[4]));
    const float ls = static_cast<float>(std::atof(argv[5]));
    const float le = static_cast<float>(std::atof(argv[6]));
    const double ratio = argc > 7 ? std::atof(argv[7]) : 1.0;
    const double secondsArg = argc > 8 ? std::atof(argv[8]) : 0.0;
    const std::string outPath = argc > 9 && std::string(argv[9]) != "-" ? argv[9] : "";
    const float p1 = argc > 10 ? static_cast<float>(std::atof(argv[10])) : ls;
    const bool normalize = argc > 11 && std::atoi(argv[11]) != 0;

    std::vector<float> src; double sr = 44100.0;
    if (! readWavMono(wavPath, src, sr)) { std::fprintf(stderr, "cannot read %s\n", wavPath.c_str()); return 2; }

    const auto mode = modeStr == "pingpong" ? SamplePlayer::LoopMode::PingPong
                    : modeStr == "oneshot"  ? SamplePlayer::LoopMode::OneShot
                                            : SamplePlayer::LoopMode::Loop;
    juce::AudioBuffer<float> buf(1, static_cast<int>(src.size()));
    std::copy(src.begin(), src.end(), buf.getWritePointer(0));

    const int block = 512;
    SamplePlayer master;
    master.prepare(sr, block);
    master.setLoopMode(mode);
    master.setCrossfadeMs(xfMs);
    master.setLoopOptimizeLevel(opt);
    master.setNormalize(normalize);
    master.setLoopRegion(ls, le);
    master.setStartPos(p1);
    master.loadBuffer(buf, sr);
    const auto prepared = master.prepareBufferLoad(buf, sr, master.capturePrepareConfig());
    const int playStart = prepared.playbackState.playStart;
    const int playEnd   = prepared.playbackState.playEnd;
    const int L = playEnd - playStart;

    SamplePlayer voice;
    voice.prepare(sr, block);
    voice.setLoopMode(mode);
    voice.setNormalize(normalize);
    voice.shareBufferFrom(master);
    voice.setStartPos(p1);
    voice.setTransposeRatio(ratio);
    voice.retrigger();

    // Default length: at least four full passes (so >= 4 seams), never under 12 s.
    const double firstPass = std::max(0.0, static_cast<double>(ls - p1)) * static_cast<double>(src.size());
    const double seconds = secondsArg > 0.0 ? secondsArg : std::max(12.0, (4.5 * L + firstPass) / sr / ratio);
    const int total = static_cast<int>(seconds * sr);
    std::vector<float> out; out.reserve(static_cast<size_t>(total + 64));

    // Seams are located by watching the voice's read position: a wrap (Loop) is
    // a backward jump of the position, a turnaround (PingPong) a direction flip.
    // At unity the bypass path is rendered one sample at a time, so the seam
    // index is exact; on the stretch path (ratio != 1) blocks of 32 are used and
    // the seam is known to +-32 samples, which the +-10 ms search below absorbs.
    const int step = std::abs(ratio - 1.0) < 1e-9 ? 1 : 32;
    std::vector<float> blockBuf(static_cast<size_t>(step));
    std::vector<int> seams;
    double lastPos = voice.getReadPosition();
    int lastDir = voice.getPlayDirection();
    while (static_cast<int>(out.size()) < total) {
        voice.renderPitchedBlock(blockBuf.data(), step);
        const double pos = voice.getReadPosition();
        const int dir = voice.getPlayDirection();
        const bool wrapped = (dir > 0 && pos < lastPos - step * 2.0 * ratio - 1.0)
                          || (dir < 0 && pos > lastPos + step * 2.0 * ratio + 1.0);
        if (dir != lastDir || wrapped)
            seams.push_back(static_cast<int>(out.size()));
        lastPos = pos; lastDir = dir;
        out.insert(out.end(), blockBuf.begin(), blockBuf.end());
    }

    // Material's own per-sample delta inside the loop region (the reference).
    std::vector<float> d; d.reserve(static_cast<size_t>(std::max(0, L)));
    for (int i = std::max(1, playStart); i < playEnd && i < static_cast<int>(src.size()); ++i)
        d.push_back(std::abs(src[static_cast<size_t>(i)] - src[static_cast<size_t>(i - 1)]));
    const float refDelta = std::max(1e-6f, percentile(d, 0.999));

    // Second-difference energy in 5 ms windows over the output.
    const int win = std::max(16, static_cast<int>(0.005 * sr));
    std::vector<float> hf(out.size(), 0.0f);
    for (size_t i = 2; i < out.size(); ++i) {
        float v = out[i] - 2.0f * out[i - 1] + out[i - 2];
        hf[i] = v * v;
    }
    std::vector<double> prefix(out.size() + 1, 0.0);
    for (size_t i = 0; i < out.size(); ++i) prefix[i + 1] = prefix[i] + hf[i];
    auto winEnergy = [&](int c) {
        int a = std::max(0, c - win / 2), b = std::min(static_cast<int>(out.size()), c + win / 2);
        return b > a ? (prefix[static_cast<size_t>(b)] - prefix[static_cast<size_t>(a)]) / (b - a) : 0.0;
    };
    // A click is a burst that exceeds BOTH neighbouring stretches of the output.
    // Comparing with the whole output's median instead reads content as a
    // click: a bright loop start, or a decaying sound whose median is its tail.
    double worstStep = 0.0, meanStep = 0.0, worstHf = -200.0, meanHf = 0.0;
    int counted = 0;
    const int nearLo = static_cast<int>(0.0075 * sr), nearHi = static_cast<int>(0.030 * sr);
    auto meanEnergy = [&](int a, int b) {
        a = std::max(0, a); b = std::min(static_cast<int>(out.size()), b);
        return b > a ? (prefix[static_cast<size_t>(b)] - prefix[static_cast<size_t>(a)]) / (b - a) : 0.0;
    };
    for (int s : seams) {
        if (s < nearHi + 128 || s + nearHi + 128 >= static_cast<int>(out.size())) continue;
        float mx = 0.0f;
        for (int i = s - 64; i <= s + 64; ++i)
            mx = std::max(mx, std::abs(out[static_cast<size_t>(i)] - out[static_cast<size_t>(i - 1)]));
        // Worst 5 ms window within +-5 ms of the seam, over the louder neighbour.
        double e = 0.0;
        for (int c = s - win; c <= s + win; c += win / 4) e = std::max(e, winEnergy(c));
        const double ePre  = meanEnergy(s - nearHi, s - nearLo);
        const double ePost = meanEnergy(s + nearLo, s + nearHi);
        const double stepRatio = mx / refDelta;
        const double hfDb = 10.0 * std::log10((e + 1e-20) / (std::max(ePre, ePost) + 1e-20));
        worstStep = std::max(worstStep, stepRatio); meanStep += stepRatio;
        worstHf = std::max(worstHf, hfDb); meanHf += hfDb;
        ++counted;
    }
    if (counted > 0) { meanStep /= counted; meanHf /= counted; }

    if (! outPath.empty()) writeWav(outPath, out, static_cast<int>(sr));

    std::string seamList;
    for (size_t i = 0; i < seams.size(); ++i) seamList += (i ? "," : "") + std::to_string(seams[i]);
    std::printf("RESULT file=%s mode=%s opt=%d xf=%.0f ls=%.4f le=%.4f p1=%.4f norm=%d ratio=%.3f region=[%d,%d) L=%.3fs seams=%d "
                "step_max=%.2f step_mean=%.2f hf_max_dB=%.1f hf_mean_dB=%.1f seams_at=%s\n",
                juce::File(wavPath).getFileNameWithoutExtension().toRawUTF8(), modeStr.c_str(), opt, xfMs, ls, le, p1, normalize ? 1 : 0, ratio,
                playStart, playEnd, L / sr, counted, worstStep, meanStep, worstHf, meanHf, seamList.c_str());
    return 0;
}
