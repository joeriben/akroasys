// Offline audition for held-note Regen XFade (morphToBufferFrom crossfade).
//
// Reproduces the A/B-drift regenerate seam: a held sampler voice shares buffer A,
// then the master publishes a very different buffer B and the voice equal-power
// crossfades A->B over the Drift Crossfade time (morphToBufferFrom). We render
// straight through the crossfade and measure the sample-to-sample discontinuity
// vs. the signal's natural per-sample delta, AND confirm the blend actually
// transitions, for both render paths (unity → bypass/processSample, pitched →
// Signalsmith stretch).
//
// Build: compile against the plugin's static lib + JUCE flags, e.g.
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/audition_sampler_follow.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/sampler_follow
// Exits non-zero if any case is not a click-free, actually-transitioning crossfade.
#include "JuceHeader.h"
#include "dsp/SamplePlayer.h"

#include <cstdio>
#include <vector>
#include <cmath>
#include <string>
#include <fstream>
#include <algorithm>

static void writeWav(const std::string& path, const std::vector<float>& mono, int sr)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v){ f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](uint16_t v){ f.write(reinterpret_cast<char*>(&v), 2); };
    uint32_t dataBytes = static_cast<uint32_t>(mono.size()) * 2u;
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(sr); u32(sr * 2); u16(2); u16(16);
    f.write("data", 4); u32(dataBytes);
    for (float s : mono) {
        int v = static_cast<int>(std::lround(std::max(-1.f, std::min(1.f, s)) * 32767.f));
        u16(static_cast<uint16_t>(static_cast<int16_t>(v)));
    }
}

// A buffer of a steady tone — distinct freq/amp so a hard swap would click hard.
static juce::AudioBuffer<float> makeTone(int sr, double seconds, double freq, float amp, double phase)
{
    int n = static_cast<int>(seconds * sr);
    juce::AudioBuffer<float> b(1, n);
    float* d = b.getWritePointer(0);
    for (int i = 0; i < n; ++i)
        d[i] = amp * static_cast<float>(std::sin(2.0 * M_PI * freq * i / sr + phase));
    return b;
}

// secondPublishMs >= 0: the master publishes a SECOND snapshot that many ms
// after the first, while the crossfade from the first is still running. That is
// not a hypothetical — MainPanel::restoreMainSnapshot re-asserts the slot's loop
// points AFTER loadGeneratedAudio has already published, which marks the master
// for a re-prepare and lands a second publication a few ms later. The voice must
// still arrive at the newest buffer without a step.
// thirdBuffer: the second publication carries DIFFERENT audio (a real second
// regenerate landing mid-crossfade) rather than a rebuild of the same.
// stepBound >= 0: this case is NOT expected to be click-free — only two buffers
// can sound at once, so a third one arriving forces one of the two sounding
// sides out, and the rule is only that it is the QUIETER side. The bound is the
// step that rule predicts; the case asserts the prediction.
static bool runCase(const std::string& label, double ratio, float morphMs,
                    const std::string& wavOut, int sr, double secondPublishMs = -1.0,
                    bool thirdBuffer = false, float stepBound = -1.0f)
{
    const int block = 512;
    const int preBlocks = 40;    // ~0.43 s held on buffer A
    const int postBlocks = 80;   // ~0.85 s on buffer B after the live crossfade

    auto bufA = makeTone(sr, 2.0, 220.0, 0.50f, 0.0);
    auto bufB = makeTone(sr, 2.0, 660.0, 0.85f, M_PI * 0.5); // very different content

    SamplePlayer master;
    master.prepare(sr, block);
    master.setLoopMode(SamplePlayer::LoopMode::Loop);
    master.setNormalize(false);
    master.loadBuffer(bufA, sr);

    SamplePlayer voice;
    voice.prepare(sr, block);
    voice.setLoopMode(SamplePlayer::LoopMode::Loop);
    voice.setNormalize(false);
    voice.shareBufferFrom(master);
    voice.setTransposeRatio(ratio);
    voice.retrigger();

    std::vector<float> out;
    std::vector<float> blockBuf(static_cast<size_t>(block));

    for (int b = 0; b < preBlocks; ++b) {
        voice.renderPitchedBlock(blockBuf.data(), block);
        out.insert(out.end(), blockBuf.begin(), blockBuf.end());
    }

    const int seamIndex = static_cast<int>(out.size()); // first crossfade sample

    // The A/B-drift regenerate handoff: master republishes, held voice crossfades
    // from A to B over morphMs (the Regen XFade). morphToBufferFrom runs on the
    // audio thread, exactly as VoiceManager::distributeSamplerBuffer drives it.
    master.loadBuffer(bufB, sr);
    voice.morphToBufferFrom(master, morphMs);

    const int secondPublishBlock = secondPublishMs >= 0.0
        ? (int) std::lround(secondPublishMs * 0.001 * sr / block) : -1;

    for (int b = 0; b < postBlocks; ++b) {
        if (b == secondPublishBlock) {
            // A re-prepare of the SAME source: identical audio, new snapshot.
            // Nothing about the sound changed, so nothing about the sound may.
            voice.drainRetiredSnapshot();   // as the off-thread drain does, before publishing
            master.loadBuffer(thirdBuffer ? makeTone(sr, 2.0, 330.0, 0.70f, M_PI) : bufB, sr);
            voice.morphToBufferFrom(master, morphMs);
        }
        voice.renderPitchedBlock(blockBuf.data(), block);
        out.insert(out.end(), blockBuf.begin(), blockBuf.end());
    }

    const int morphSamples = (int) std::lround((double) morphMs * 0.001 * sr);
    // Crossfade window (seam → end of the equal-power ramp, + a small guard). The
    // stretch path delays the blend by the STFT latency, so cover generously.
    const int winEnd = seamIndex + morphSamples + 4096;

    // Continuity at the seam itself: alpha starts at 0 (all old buffer), so the
    // first crossfade sample continues A — there must be no step here.
    float seamDelta = std::abs(out[seamIndex] - out[seamIndex - 1]);

    // Max per-sample delta THROUGH the whole crossfade window — the click test.
    float windowMax = 0.0f;
    for (int i = seamIndex - 1; i < winEnd && i < (int) out.size() - 1; ++i)
        windowMax = std::max(windowMax, std::abs(out[i + 1] - out[i]));

    // Natural per-sample delta in steady state (pure B), the reference ceiling.
    float naturalMax = 0.0f;
    for (int i = winEnd; i < (int) out.size() - 1; ++i)
        naturalMax = std::max(naturalMax, std::abs(out[i + 1] - out[i]));

    // The crossfade must actually transition: pre-seam is A (220 Hz/0.50),
    // post-window is B (660 Hz/0.85). Confirm RMS moved toward B's level.
    auto rms = [&](int a, int b){ double s = 0; int n = 0;
        for (int i = a; i < b && i < (int) out.size(); ++i) { s += out[i]*out[i]; ++n; }
        return n ? std::sqrt(s / n) : 0.0; };
    const double rmsA = rms(seamIndex - 4000, seamIndex);
    const double rmsB = rms(winEnd, winEnd + 4000);

    // The per-sample step above catches a seam; it does NOT catch a hard swap
    // between two buffers that happen to meet at a similar instantaneous value.
    // What a hard swap always does is move the LEVEL in one block, so measure
    // that too: the steepest 5 ms change of a 10 ms sliding RMS through the
    // window. An equal-power crossfade over 200 ms cannot exceed ~1 dB there.
    const int envWin = (int) (0.010 * sr), envHop = (int) (0.001 * sr);
    float envJumpDb = 0.0f; int envJumpAt = seamIndex;
    for (int i = seamIndex - envWin; i + 5 * envHop + envWin < winEnd; i += envHop) {
        const double a = rms(i, i + envWin), b = rms(i + 5 * envHop, i + 5 * envHop + envWin);
        if (a > 1.0e-5 && b > 1.0e-5) {
            const float db = (float) (20.0 * std::log10(b / a));
            if (std::abs(db) > std::abs(envJumpDb)) { envJumpDb = db; envJumpAt = i + 5 * envHop; }
        }
    }

    writeWav(wavOut, out, sr);

    const bool clickFree = stepBound >= 0.0f
        ? windowMax <= stepBound
        : (windowMax <= naturalMax * 1.5f + 1.0e-4f && std::abs(envJumpDb) <= 3.0f);
    const bool transitioned = rmsB > rmsA * 1.2;   // 0.85 vs 0.50 → clearly higher
    const bool pass = clickFree && transitioned;
    printf("  [%-30s] seamDelta=%.5f  windowMax=%.5f  naturalMax=%.5f  envJump=%+5.1f dB @%+6.1f ms"
           "  rmsA=%.3f rmsB=%.3f -> %s\n",
           label.c_str(), seamDelta, windowMax, naturalMax, envJumpDb,
           1000.0 * (double) (envJumpAt - seamIndex) / sr, rmsA, rmsB,
           pass ? (stepBound >= 0.0f ? "WITHIN THE TWO-SLOT BOUND" : "CLICK-FREE XFADE")
                : (clickFree ? "*** NO XFADE ***" : "*** STEP ***"));
    return pass;
}

// Amplitude of one frequency over a window — tells which BUFFER is sounding
// when several tones are in play (the sliding RMS above cannot).
static double goertzelAmp(const std::vector<float>& x, int from, int n, double freq, int sr)
{
    const double w = 2.0 * M_PI * freq / sr;
    const double c = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = from; i < from + n && i < (int) x.size(); ++i) {
        const double s = (double) x[i] + c * s1 - s2;
        s2 = s1; s1 = s;
    }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, p)) / n;
}

// PUBLICATIONS ARRIVING FASTER THAN THE CROSSFADE.
//
// Every publication restarts the ramp, and below halfway the rule keeps the
// current fade-from — so under a stream the fade-from never ages out and the
// held note does NOT walk forward through the generations while the stream
// runs. That is a real cost of the rule the three engines share, and this case
// exists so it is measured rather than assumed: it prints what is sounding mid
// stream, and it ASSERTS the thing the platform invariant actually requires —
// once publications stop, the note arrives at the NEWEST buffer within one
// Regen XFade, with every publication's step inside the two-slot bound.
static bool runStreamCase(const std::string& label, float morphMs, double intervalFrac,
                          int publications, int sr, const std::string& wavOut)
{
    const int block = 64;                       // 1.33 ms grid — honours short intervals
    const double intervalMs = intervalFrac * morphMs;
    const float  streamAmp = 0.60f;
    const double origFreq  = 220.0;

    auto bufA = makeTone(sr, 2.0, origFreq, 0.50f, 0.0);

    SamplePlayer master, voice;
    master.prepare(sr, block); master.setLoopMode(SamplePlayer::LoopMode::Loop);
    master.setNormalize(false); master.loadBuffer(bufA, sr);
    voice.prepare(sr, block);  voice.setLoopMode(SamplePlayer::LoopMode::Loop);
    voice.setNormalize(false); voice.shareBufferFrom(master);
    voice.retrigger();

    std::vector<float> out, blockBuf((size_t) block);
    auto render = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            voice.renderPitchedBlock(blockBuf.data(), block);
            out.insert(out.end(), blockBuf.begin(), blockBuf.end());
        }
    };
    render((int) std::lround(0.25 * sr / block));      // 0.25 s held on A

    const int intervalBlocks = std::max(1, (int) std::lround(intervalMs * 0.001 * sr / block));
    double newestFreq = origFreq;
    float  worstStep = 0.0f;

    for (int p = 0; p < publications; ++p) {
        render(intervalBlocks);
        const int at = (int) out.size();
        newestFreq = 300.0 + 20.0 * p;                 // all distinct, 20 Hz apart
        voice.drainRetiredSnapshot();
        master.loadBuffer(makeTone(sr, 2.0, newestFreq, streamAmp, 0.0), sr);
        voice.morphToBufferFrom(master, morphMs);
        render(1);
        for (int i = at - 2; i + 1 < (int) out.size(); ++i)
            worstStep = std::max(worstStep, std::abs(out[i + 1] - out[i]));
    }

    const int midStream = (int) out.size();
    render((int) std::lround((morphMs * 0.001 + 0.20) * sr / block));   // stream stops

    const int probe = (int) (0.100 * sr);
    const double midNewest = goertzelAmp(out, midStream - probe, probe, newestFreq, sr);
    const double midOrig   = goertzelAmp(out, midStream - probe, probe, origFreq,   sr);
    const int endAt = (int) out.size() - probe;
    const double endNewest = goertzelAmp(out, endAt, probe, newestFreq, sr);
    const double endOrig    = goertzelAmp(out, endAt, probe, origFreq,   sr);

    // Bound each publication's step from the rule, not from the measurement:
    // the dropped side vanishes at sin(a*pi/2), the kept side steps up to 1.0
    // from cos(a*pi/2), plus the material's own per-sample motion.
    const double a = std::min(0.5, intervalFrac) * M_PI * 0.5;
    const double natural = 2.0 * M_PI * (300.0 + 20.0 * publications) / sr * streamAmp;
    const float bound = (float) (std::sin(a) * streamAmp
                                 + (1.0 - std::cos(a)) * streamAmp + natural);

    writeWav(wavOut, out, sr);

    const bool landed  = endNewest > 0.75 * streamAmp && endOrig < 0.05;
    const bool bounded = worstStep <= bound;
    const bool pass = landed && bounded;
    printf("  [%-30s] %d publications every %.0f%% of the xfade  worstStep=%.3f (bound %.3f)\n"
           "  %-32s mid-stream: newest=%.3f original=%.3f   after it stops: newest=%.3f original=%.3f -> %s\n",
           label.c_str(), publications, intervalFrac * 100.0, worstStep, bound, "",
           midNewest, midOrig, endNewest, endOrig,
           pass ? "LANDS ON THE NEWEST" : (landed ? "*** STEP ***" : "*** NEVER LANDS ***"));
    return pass;
}

int main()
{
    const int sr = 48000;
    juce::ScopedJuceInitialiser_GUI juceInit; // safe init for any JUCE statics

    printf("Held-note Regen-XFade audition (A=220Hz/0.50 -> B=660Hz/0.85, default 200 ms):\n");
    bool ok = true;
    ok &= runCase("unity (bypass path)",  1.0,                       200.0f, "/tmp/sampler_follow_unity.wav",   sr);
    ok &= runCase("pitched +7 (stretch)", std::pow(2.0, 7.0 / 12.0), 200.0f, "/tmp/sampler_follow_pitched.wav", sr);
    // A second publication of the SAME audio, 10 ms into the crossfade — the
    // SNAP-recall shape. The sound did not change, so the sound must not.
    ok &= runCase("unity, republished at 10 ms",  1.0,                       200.0f,
                  "/tmp/sampler_follow_unity_republish.wav",   sr, 10.0);
    ok &= runCase("pitched, republished at 10 ms", std::pow(2.0, 7.0 / 12.0), 200.0f,
                  "/tmp/sampler_follow_pitched_republish.wav", sr, 10.0);
    // A genuinely DIFFERENT third buffer landing mid-crossfade — two regenerates
    // inside one Regen XFade. Only two buffers can sound at once, so one of the
    // two currently sounding sides has to go; the rule is that it is the quieter
    // one. 10 ms in (target at 0.08) and at the halfway point, which is where
    // that rule is at its weakest and both sides carry 0.707.
    ok &= runCase("unity, 3rd buffer at 10 ms",  1.0, 200.0f,
                  "/tmp/sampler_follow_third_early.wav", sr, 10.0,  true, 0.10f);
    ok &= runCase("unity, 3rd buffer at 80 ms",  1.0, 200.0f,
                  "/tmp/sampler_follow_third_late.wav",  sr, 80.0,  true, 0.55f);
    // At the halfway point the rule is at its weakest: both sides carry 0.707,
    // so the one that goes is 0.707 of a sounding buffer. Output steps from
    // 0.707*A + 0.707*B to B, i.e. by |0.707*A - 0.293*B| <= 0.707*0.50 +
    // 0.293*0.85 = 0.60 for this material. That is what is asserted here --
    // click-freeness is not available to two slots, and claiming it would be
    // the same overstatement the naive restart made. (Unguarded, the same case
    // stepped by 0.97: the LOUD side went instead.)
    ok &= runCase("unity, 3rd buffer at 100 ms", 1.0, 200.0f,
                  "/tmp/sampler_follow_third_mid.wav",   sr, 100.0, true, 0.60f);
    // Held note under a publication STREAM (drag a loop bracket, fast auto-
    // regenerate). Below halfway the fade-from is kept, so the note does not walk
    // forward WHILE the stream runs — printed, not asserted. What is asserted is
    // the invariant: once it stops, the newest buffer wins within one Regen XFade.
    printf("Held note under a publication stream (each restarts the ramp):\n");
    ok &= runStreamCase("stream every 40% of xfade", 200.0f, 0.40, 25, sr,
                        "/tmp/sampler_follow_stream_40.wav");
    ok &= runStreamCase("stream every 8% of xfade",  200.0f, 0.08, 40, sr,
                        "/tmp/sampler_follow_stream_08.wav");
    printf("WAVs: /tmp/sampler_follow_*.wav\n");
    printf("%s\n", ok ? "ALL PASS" : "*** FAIL ***");
    return ok ? 0 : 1;
}
