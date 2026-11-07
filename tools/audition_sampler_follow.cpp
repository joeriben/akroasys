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

    // At a publication the DROPPED side sits at a zero crossing and the KEPT
    // side at a peak, so the step there measures a stand-up of the kept side
    // in full and nothing else: every stream tone has an odd number of quarter
    // periods per publication interval, so it alternates zero / peak at the
    // publication instants, and is phased to cross zero where it is dropped
    // (the next publication before halfway; the one after that past halfway,
    // where it is first promoted at a peak); the held tone has an integer
    // number of half periods per interval and is phased to a peak at every
    // publication when it is the side that is kept (before halfway), to a zero
    // when it is dropped at the first one (past halfway). The buffer is an
    // integer number of intervals long with raw loop points at its ends, so
    // the wrap keeps every phase.
    const int intervalBlocks = std::max(1, (int) std::lround(intervalMs * 0.001 * sr / block));
    const int intervalSamples = intervalBlocks * block;
    const int afterBlocks    = std::min(intervalBlocks, (int) std::ceil(0.010 * sr / block));   // the 10 ms after a publication
    const int initialBlocks  = (int) std::lround(0.25 * sr / block);
    const double q = (double) sr / (2.0 * intervalSamples);            // frequency quantum (half period per interval)
    const int bufLen = 4 * intervalSamples * (int) std::ceil(2.0 * sr / (4.0 * intervalSamples));
    const double bufSeconds = (double) bufLen / sr;
    const double origFreq  = std::lround(220.0 / q) * q;
    const double pos0      = (double) ((initialBlocks + intervalBlocks - afterBlocks) * block);   // position at the first publication
    const double phaseA    = (intervalFrac > 0.5 ? 0.0 : M_PI / 2) - 2.0 * M_PI * origFreq * pos0 / sr;
    const int m0 = (int) std::lround(300.0 / q);
    const int stride = std::max(1, (int) std::ceil(50.0 / q));          // tones >= 50 Hz apart
    const int span  = 4 * intervalSamples * std::max(1, (int) std::ceil(0.100 * sr / (4.0 * intervalSamples)));

    // One run of the stream. The level is measured on two further runs with
    // one side silent each (the gains do not depend on the signal), because
    // the power of the sum depends on the tones' phase relations, the sum of
    // the two sides' powers does not: that sum is what the gain law adds up.
    struct Run { std::vector<float> out; std::vector<int> ats; int midStream = 0; double newestFreq = 0;
                 float worstStep = -1.0f, worstAfter = -1.0f, bound = 0.0f, afterBound = 0.0f; };
    auto run = [&](float heldAmp, float toneAmp) {
        Run r;
        auto bufA = makeTone(sr, bufSeconds, origFreq, heldAmp, phaseA);
        SamplePlayer master, voice;
        master.prepare(sr, block); master.setLoopMode(SamplePlayer::LoopMode::Loop);
        master.setNormalize(false); master.setCrossfadeMs(0.0f); master.setLoopOptimizeLevel(0);
        master.setLoopRegion(0.0f, 1.0f); master.setStartPos(0.0f); master.loadBuffer(bufA, sr);
        voice.prepare(sr, block);  voice.setLoopMode(SamplePlayer::LoopMode::Loop);
        voice.setNormalize(false); voice.setCrossfadeMs(0.0f); voice.shareBufferFrom(master);
        voice.retrigger();
        std::vector<float> blockBuf((size_t) block);
        auto render = [&](int blocks) {
            for (int b = 0; b < blocks; ++b) {
                voice.renderPitchedBlock(blockBuf.data(), block);
                r.out.insert(r.out.end(), blockBuf.begin(), blockBuf.end());
            } };
        render(initialBlocks);                           // 0.25 s held on A
        double newestFreq = origFreq, prevFreq = origFreq;
        // Bound each publication's step from the rule, not from the measurement:
        // the dropped side is at a zero crossing (above), so at the publication
        // sample the output may move only as the sounding materials move
        // themselves (kept side at its level, dropped side's last sample at its
        // slope, new side rising from zero); a side stood up to a higher level
        // there would step by the difference. In the 10 ms after it no side
        // steps either: the kept side moves as its material does plus the slow
        // recovery, the new one rises from zero.
        for (int p = 0; p < publications; ++p) {
            render(intervalBlocks - afterBlocks);
            const int at = (int) r.out.size();
            r.ats.push_back(at);
            prevFreq   = newestFreq;
            newestFreq = (m0 + stride * (1 + (p % 4)) + 0.5) * q;   // odd quarter periods per interval; four tones in rotation
            // zero where this tone is dropped: the next publication instant, or the one after
            const double posNext = std::fmod(voice.getReadPosition() + (intervalFrac > 0.5 ? 2 : 1) * intervalSamples, (double) bufLen);
            const double phase   = -2.0 * M_PI * newestFreq * posNext / sr;
            voice.drainRetiredSnapshot();
            master.loadBuffer(makeTone(sr, bufSeconds, newestFreq, toneAmp, phase), sr);
            voice.morphToBufferFrom(master, morphMs);
            render(afterBlocks);
            const float natural = (float) (2.0 * M_PI * std::max(prevFreq, newestFreq) / sr * streamAmp);
            r.bound      = 2.0f * natural + 0.005f;
            r.afterBound = 1.5f * natural;
            r.worstStep  = std::max(r.worstStep, std::abs(r.out[at] - r.out[at - 1]) - r.bound);
            for (int i = at + 1; i < at + (int) (0.010 * sr) && i < (int) r.out.size(); ++i)
                r.worstAfter = std::max(r.worstAfter, std::abs(r.out[i] - r.out[i - 1]) - r.afterBound);
        }
        r.midStream  = (int) r.out.size();
        r.newestFreq = newestFreq;
        render((int) std::lround((morphMs * 0.001 + 0.20) * sr / block));   // stream stops
        return r; };
    auto spanPower = [&](const Run& r) {
        double p = 0.0; for (int i = r.midStream - span; i < r.midStream; ++i) p += (double) r.out[i] * r.out[i];
        return p / span; };

    const Run both = run(streamAmp, streamAmp), heldOnly = run(streamAmp, 0.0f), streamOnly = run(0.0f, streamAmp);
    const double nominalRms = streamAmp / std::sqrt(2.0);

    // The recovery's DURATION, on the held-alone run (its output is the held
    // tone times the kept side's gain): where the kept side rises after a
    // publication (before halfway, 10 ms recovery), the rise at 2.5 ms must be
    // well short of complete (the law: ~0.14 of it; a near-instant recovery is
    // a step up at every publication the step bound cannot see, because it is
    // spread over a few samples).
    float worstRise = -1.0f; int riseChecks = 0;
    if (intervalFrac < 0.5 && morphMs >= 40.0f) {
        auto bufA = makeTone(sr, bufSeconds, origFreq, streamAmp, phaseA);
        const float* A = bufA.getReadPointer(0);
        auto gainAt = [&](int centre) {           // mean |out/A| over +-1 ms where A is large
            double g = 0.0; int n = 0;
            for (int i = centre - (int) (0.001 * sr); i < centre + (int) (0.001 * sr); ++i) {
                const float a = A[i % bufLen];
                if (std::abs(a) > 0.7f * streamAmp) { g += std::abs(heldOnly.out[i] / a); ++n; } }
            return n > 0 ? g / n : 0.0; };
        for (size_t k = 4; k < heldOnly.ats.size(); ++k) {
            const int at = heldOnly.ats[k];
            const double g0 = gainAt(at + (int) (0.0005 * sr)), gMid = gainAt(at + (int) (0.0025 * sr)), gEnd = gainAt(at + (int) (0.011 * sr));
            if (gEnd - g0 > 0.05) { worstRise = std::max(worstRise, (float) ((gMid - g0) / (gEnd - g0))); ++riseChecks; }
        }
    }
    const double midRms = std::sqrt(spanPower(heldOnly) + spanPower(streamOnly));   // incoherent level during the stream
    const int probe = (int) (0.100 * sr);
    const int endAt = (int) both.out.size() - probe;
    const double endNewest = goertzelAmp(both.out, endAt, probe, both.newestFreq, sr);
    const double endOrig   = goertzelAmp(both.out, endAt, probe, origFreq, sr);
    const double heldLevel = std::sqrt(spanPower(heldOnly)) / nominalRms, streamLevel = std::sqrt(spanPower(streamOnly)) / nominalRms;

    writeWav(wavOut, both.out, sr);

    const bool landed  = endNewest > 0.75 * streamAmp && endOrig < 0.05;
    const bool bounded = both.worstStep <= 0.0f && both.worstAfter <= 0.0f;
    const bool loud    = midRms >= 0.9 * nominalRms;   // publications must not drain the held note
    const bool paced   = worstRise < 0.5f;             // the recovery takes its 10 ms
    const bool pass = landed && bounded && loud && paced;
    printf("  [%-30s] %d publications every %.0f%% of a %.0f ms xfade  step over bound: at publication %+.3f (bound %.3f), 10 ms after %+.4f (bound %.4f)\n"
           "  %-32s mid-stream level %.3f of nominal (held side %.3f, stream side %.3f)  recovery at 2.5 ms: %s  after it stops: newest=%.3f original=%.3f -> %s\n",
           label.c_str(), publications, intervalFrac * 100.0, morphMs, both.worstStep, both.bound, both.worstAfter, both.afterBound, "",
           midRms / nominalRms, heldLevel, streamLevel,
           (riseChecks > 0 ? (juce::String(worstRise, 2) + " of the rise (" + juce::String(riseChecks) + " checks)") : juce::String("n/a")).toRawUTF8(),
           endNewest, endOrig,
           pass ? "LANDS ON THE NEWEST" : (! loud ? "*** DRAINED ***" : ! paced ? "*** RECOVERS TOO FAST ***" : landed ? "*** STEP ***" : "*** NEVER LANDS ***"));
    return pass;
}

// PING-PONG: a regenerate landing INSIDE a turnaround fade. The voice has just
// reflected at the loop end of buffer A and is blending A's forward continuation
// into its reversed playback (the sampler Crossfade) when buffer B arrives with
// a DIFFERENT loop end. The outgoing side must keep A's own turnaround geometry;
// only the Regen XFade ramp may move the voice. (Found by the guardian pass on
// the seam rewrite: with one shared geometry the step was 27x the natural
// per-sample delta.) endB > endA so the read position stays inside both loops.
// The material must NOT be time-symmetric anywhere, or a turnaround blend
// equals the plain reversed read and no geometry error can show: five partials
// at incommensurate ratios with unrelated phases (a pure sine's extrema are
// mirror points, and the search would sit on one).
static juce::AudioBuffer<float> makeAsymmetricTone(int sr, double seconds, double f0, float amp, double seed)
{
    const double ratios[] = { 1.0, 1.37, 2.11, 2.90, 3.71 };
    const double weights[] = { 1.0, 0.7, 0.5, 0.35, 0.25 };
    int n = static_cast<int>(seconds * sr);
    juce::AudioBuffer<float> b(1, n);
    float* d = b.getWritePointer(0);
    for (int i = 0; i < n; ++i) {
        double v = 0.0;
        for (int k = 0; k < 5; ++k)
            v += weights[k] * std::sin(2.0 * M_PI * f0 * ratios[k] * i / sr + seed * (k + 1) * 1.7);
        d[i] = amp * static_cast<float>(v / 2.8);
    }
    return b;
}

// delayMs < 0: the regenerate lands BEFORE the reflection (the voice still moving
// forward), with the new end below the old one — the ordering in which a fade
// anchored on the buffer's geometry instead of on the reflection steps.
// Loop mode, a regenerate whose NEW audio gets its own seam (optimizer on, so
// the loop points differ even with identical markers), landing shortly before
// the new seam. The outgoing buffer's fade is baked at ITS seam, so it must keep
// wrapping there, not at the new one: out - cos(ramp)*ref (ref = the same note
// never regenerated) is the incoming side alone, smooth and starting from zero.
// `morphBeforeNewWrap` is in samples before the new buffer's first wrap.
static bool runLoopRegenCase(const std::string& label, int optLevel, float xfMs, int morphBeforeNewWrap,
                             int sr, const std::string& wavOut)
{
    auto bufA = makeAsymmetricTone(sr, 2.0, 220.0, 0.50f, 0.0);
    auto bufB = makeAsymmetricTone(sr, 2.0, 277.0, 0.85f, 2.0);
    auto setup = [&](SamplePlayer& m, const juce::AudioBuffer<float>& b) {
        m.prepare(sr, 512); m.setLoopMode(SamplePlayer::LoopMode::Loop);
        m.setNormalize(false); m.setCrossfadeMs(xfMs); m.setLoopOptimizeLevel(optLevel);
        m.setLoopRegion(0.3f, 0.6f); m.setStartPos(0.3f); m.loadBuffer(b, sr); };
    auto mkVoice = [&](SamplePlayer& v, SamplePlayer& m) {
        v.prepare(sr, 512); v.setLoopMode(SamplePlayer::LoopMode::Loop);
        v.setNormalize(false); v.setCrossfadeMs(xfMs);
        v.shareBufferFrom(m); v.setTransposeRatio(1.0); v.retrigger(); };

    // Where B's first wrap falls, from a B-only voice.
    SamplePlayer masterB, voiceB; setup(masterB, bufB); mkVoice(voiceB, masterB);
    long wrapB = -1; double prevPos = voiceB.getReadPosition(); float s = 0.0f;
    for (long i = 0; i < (long) sr * 3 && wrapB < 0; ++i) {
        voiceB.renderPitchedBlock(&s, 1);
        if (voiceB.getReadPosition() < prevPos) wrapB = i;
        prevPos = voiceB.getReadPosition();
    }
    const long morphAt = wrapB - morphBeforeNewWrap;

    SamplePlayer master, voice, refMaster, ref;
    setup(master, bufA); mkVoice(voice, master);
    setup(refMaster, bufA); mkVoice(ref, refMaster);
    std::vector<float> out, refOut; float rs = 0.0f;
    const int morphSamples = (int) std::lround(0.200 * sr);
    for (long i = 0; i < (long) sr * 3; ++i) {
        if (i == morphAt) { master.loadBuffer(bufB, sr); voice.morphToBufferFrom(master, 200.0f); }
        voice.renderPitchedBlock(&s, 1); out.push_back(s);
        ref.renderPitchedBlock(&rs, 1); refOut.push_back(rs);
    }
    float natB = 0.0f;
    for (long i = morphAt + morphSamples + 4096; i + 1 < (long) out.size(); ++i)
        natB = std::max(natB, std::abs(out[i + 1] - out[i]));
    const double inc = 1.0 / morphSamples;
    auto resid = [&](long i) {
        const double a = std::min(1.0, (i - morphAt) * inc);
        return out[i] - (float) std::cos(a * M_PI / 2) * refOut[i]; };
    float residualMax = 0.0f, residualBoundAt = 0.0f; long residualAt = -1;
    for (long i = morphAt + 1; i < morphAt + morphSamples && i < (long) out.size(); ++i) {
        const double a = std::min(1.0, (i - morphAt) * inc);
        const float bound = 1.5f * (float) std::sin(a * M_PI / 2) * natB + 3.0e-4f;
        const float d = std::abs(resid(i) - resid(i - 1));
        if (d - bound > residualMax - residualBoundAt) { residualMax = d; residualBoundAt = bound; residualAt = i; }
    }
    auto rms = [&](long a, long b){ double q = 0; long n = 0;
        for (long i = std::max(0L, a); i < b && i < (long) out.size(); ++i) { q += out[i] * out[i]; ++n; }
        return n ? std::sqrt(q / n) : 0.0; };
    const double rmsA = rms(morphAt - 4000, morphAt);
    const double rmsB = rms(morphAt + morphSamples + 4096, morphAt + morphSamples + 8096);
    writeWav(wavOut, out, sr);
    const bool outgoingKept = residualMax <= residualBoundAt;
    const bool transitioned = rmsB > rmsA * 1.2;
    const bool pass = wrapB > 0 && morphAt > 0 && outgoingKept && transitioned;
    printf("  [%-34s] new seam wraps at %ld, morph %d samples before  rmsA=%.3f rmsB=%.3f  outgoing residual step=%.5f (bound %.5f @+%.1f ms) -> %s\n",
           label.c_str(), wrapB, morphBeforeNewWrap, rmsA, rmsB, residualMax, residualBoundAt,
           residualAt > 0 ? 1000.0 * (double) (residualAt - morphAt) / sr : 0.0,
           pass ? "CLICK-FREE XFADE" : (! outgoingKept ? "*** OUTGOING SIDE STEPPED ***" : "*** NO XFADE ***"));
    return pass;
}

// A held OneShot note regenerated shortly before its end: the outgoing side
// ends at its marker and must STAY silent, also when a second publication
// lands while the ramp is still below halfway (a dead fade-from is not the
// dominant side and must not come back at cos(0) = 1). Incoming buffers are
// silent so only the outgoing side is audible.
static bool runDeadTailCase(const std::string& label, double secondAtMs, int sr, const std::string& wavOut)
{
    auto bufA = makeAsymmetricTone(sr, 2.0, 220.0, 0.50f, 0.0);
    juce::AudioBuffer<float> silent(1, bufA.getNumSamples()); silent.clear();
    SamplePlayer master, voice;
    master.prepare(sr, 512); master.setLoopMode(SamplePlayer::LoopMode::OneShot);
    master.setNormalize(false); master.setCrossfadeMs(20.0f); master.setLoopOptimizeLevel(0);
    master.setLoopRegion(0.3f, 0.6f); master.setStartPos(0.3f); master.loadBuffer(bufA, sr);
    voice.prepare(sr, 512); voice.setLoopMode(SamplePlayer::LoopMode::OneShot);
    voice.setNormalize(false); voice.setCrossfadeMs(20.0f);
    voice.shareBufferFrom(master); voice.setTransposeRatio(1.0); voice.retrigger();
    const long endA   = (long) std::lround(0.3 * bufA.getNumSamples());   // samples from P1 to the end marker
    const long morph1 = endA - (long) std::lround(0.016 * sr);
    const long morph2 = morph1 + (long) std::lround(secondAtMs * 0.001 * sr);
    std::vector<float> out; float s = 0.0f;
    for (long i = 0; i < (long) sr * 1; ++i) {
        if (i == morph1) { master.setLoopMode(SamplePlayer::LoopMode::Loop); master.loadBuffer(silent, sr); voice.morphToBufferFrom(master, 200.0f); }
        if (i == morph2) { master.loadBuffer(silent, sr); voice.morphToBufferFrom(master, 200.0f); }
        voice.renderPitchedBlock(&s, 1); out.push_back(s);
    }
    float before = 0.0f, after = 0.0f;
    for (long i = morph1; i < endA; ++i) before = std::max(before, std::abs(out[i]));
    for (long i = endA + 2; i < (long) out.size(); ++i) after = std::max(after, std::abs(out[i]));
    writeWav(wavOut, out, sr);
    const bool pass = before > 0.05f && after < 1.0e-5f;
    printf("  [%-34s] outgoing side before its end: peak %.3f  after it: peak %.6f -> %s\n",
           label.c_str(), before, after, pass ? "STAYS SILENT" : "*** DEAD SIDE CAME BACK ***");
    return pass;
}

// A held Loop note regenerated to a OneShot whose end lies shortly AHEAD of the
// playhead: the incoming side ends there, the outgoing Loop side must play on
// under the cos ramp exactly as a never-regenerated note (reference-subtracted,
// incoming silent -> the output must be cos(ramp) * reference to the sample).
static bool runHeadEndsFirstCase(const std::string& label, int sr, const std::string& wavOut)
{
    auto bufA = makeAsymmetricTone(sr, 2.0, 220.0, 0.50f, 0.0);
    juce::AudioBuffer<float> silent(1, bufA.getNumSamples()); silent.clear();
    auto mk = [&](SamplePlayer& m, SamplePlayer& v) {
        m.prepare(sr, 512); m.setLoopMode(SamplePlayer::LoopMode::Loop);
        m.setNormalize(false); m.setCrossfadeMs(20.0f); m.setLoopOptimizeLevel(0);
        m.setLoopRegion(0.3f, 0.6f); m.setStartPos(0.3f); m.loadBuffer(bufA, sr);
        v.prepare(sr, 512); v.setLoopMode(SamplePlayer::LoopMode::Loop);
        v.setNormalize(false); v.setCrossfadeMs(20.0f);
        v.shareBufferFrom(m); v.setTransposeRatio(1.0); v.retrigger(); };
    SamplePlayer master, voice, refMaster, ref; mk(master, voice); mk(refMaster, ref);
    const int n = bufA.getNumSamples();
    // The OneShot end 10 ms ahead of where the playhead will be at the morph.
    const long morphAt = (long) std::lround(0.1 * sr);
    const float oneShotEnd = (0.3f * n + (float) morphAt + 0.010f * sr) / (float) n;
    std::vector<float> out, refOut; float s = 0.0f, rs = 0.0f;
    const int morphSamples = (int) std::lround(0.200 * sr);
    for (long i = 0; i < (long) sr * 1; ++i) {
        if (i == morphAt) { master.setLoopMode(SamplePlayer::LoopMode::OneShot); master.setLoopRegion(0.3f, oneShotEnd);
                            master.loadBuffer(silent, sr); voice.morphToBufferFrom(master, 200.0f); }
        voice.renderPitchedBlock(&s, 1); out.push_back(s);
        ref.renderPitchedBlock(&rs, 1); refOut.push_back(rs);
    }
    const double inc = 1.0 / morphSamples;
    float worst = 0.0f; long worstAt = -1;
    for (long i = morphAt; i < morphAt + morphSamples + 100 && i < (long) out.size(); ++i) {
        const double a = std::min(1.0, (i - morphAt) * inc);
        const float expect = (float) std::cos(a * M_PI / 2) * refOut[i];
        const float d = std::abs(out[i] - expect);
        if (d > worst) { worst = d; worstAt = i; }
    }
    writeWav(wavOut, out, sr);
    const bool pass = worst < 1.0e-4f;
    printf("  [%-34s] |out - cos(ramp)*ref| max %.6f @+%.1f ms -> %s\n", label.c_str(), worst,
           worstAt > 0 ? 1000.0 * (double) (worstAt - morphAt) / sr : 0.0,
           pass ? "OUTGOING SIDE PLAYS ON" : "*** OUTGOING SIDE CUT ***");
    return pass;
}

// Three sounding samples in a row: a held OneShot A is regenerated to a Loop B
// 10 ms before A's end marker, A's outgoing side ends, B is fading in alone at
// sin(ramp), and a third sample C is published 20 ms after B. B must carry on
// at the level it has (it used to be promoted to full level in one sample,
// +16 dB) and C must fade in from zero: around C's publication the output may
// step no more than the two materials step themselves.
static bool runThirdPublicationCase(const std::string& label, int sr, const std::string& wavOut)
{
    auto natStep = [](const juce::AudioBuffer<float>& b) {
        float m = 0.0f; const float* d = b.getReadPointer(0);
        for (int i = 1; i < b.getNumSamples(); ++i) m = std::max(m, std::abs(d[i] - d[i - 1]));
        return m; };
    const int n = (int) (2.0 * sr);
    const long endA   = (long) std::lround(0.3 * n);
    const long morphB = endA - (long) std::lround(0.010 * sr);
    const long morphC = morphB + (long) std::lround(0.020 * sr);
    const long morphD = morphC + (long) std::lround(0.100 * sr);     // halfway through C's ramp: B 0.11, C 0.71
    // One run with every material, and one per material with the others
    // silent (the gains do not depend on the signal): levels are then sums of
    // powers, free of the cross terms between phase-locked partials.
    auto run = [&](float aA, float aB, float aC, float aD) {
        auto bufA = makeAsymmetricTone(sr, 2.0, 220.0, aA, 0.0);
        auto bufB = makeAsymmetricTone(sr, 2.0, 330.0, aB, 1.0);
        auto bufC = makeAsymmetricTone(sr, 2.0, 440.0, aC, 2.0);
        auto bufD = makeAsymmetricTone(sr, 2.0, 550.0, aD, 3.0);
        SamplePlayer master, voice;
        master.prepare(sr, 512); master.setLoopMode(SamplePlayer::LoopMode::OneShot);
        master.setNormalize(false); master.setCrossfadeMs(20.0f); master.setLoopOptimizeLevel(0);
        master.setLoopRegion(0.3f, 0.6f); master.setStartPos(0.3f); master.loadBuffer(bufA, sr);
        voice.prepare(sr, 512); voice.setLoopMode(SamplePlayer::LoopMode::OneShot);
        voice.setNormalize(false); voice.setCrossfadeMs(20.0f);
        voice.shareBufferFrom(master); voice.setTransposeRatio(1.0); voice.retrigger();
        std::vector<float> out; float s = 0.0f;
        for (long i = 0; i < (long) (1.5 * sr); ++i) {
            // (the plugin drains the reclaim slot off-thread between publications and
            // retries a deferred one every block; here the drain is explicit)
            if (i == morphB) { master.setLoopMode(SamplePlayer::LoopMode::Loop); master.loadBuffer(bufB, sr); voice.morphToBufferFrom(master, 200.0f); }
            if (i == morphC) { voice.drainRetiredSnapshot(); master.loadBuffer(bufC, sr); voice.morphToBufferFrom(master, 200.0f); }
            if (i == morphD) { voice.drainRetiredSnapshot(); master.loadBuffer(bufD, sr); voice.morphToBufferFrom(master, 200.0f); }
            voice.renderPitchedBlock(&s, 1); out.push_back(s);
        }
        return out; };
    const auto out  = run(0.50f, 0.85f, 0.70f, 0.70f);
    const auto onlyB = run(0.0f, 0.85f, 0.0f, 0.0f), onlyC = run(0.0f, 0.0f, 0.70f, 0.0f), onlyD = run(0.0f, 0.0f, 0.0f, 0.70f);
    auto bufB = makeAsymmetricTone(sr, 2.0, 330.0, 0.85f, 1.0), bufC = makeAsymmetricTone(sr, 2.0, 440.0, 0.70f, 2.0),
         bufD = makeAsymmetricTone(sr, 2.0, 550.0, 0.70f, 3.0);

    const long w = (long) std::lround(0.001 * sr);
    float step = 0.0f, before = 0.0f, after = 0.0f, before10 = 0.0f, after10 = 0.0f;
    for (long i = morphC - w; i <= morphC + w; ++i) step = std::max(step, std::abs(out[i] - out[i - 1]));
    for (long i = morphC - 2 * w; i < morphC; ++i) before = std::max(before, std::abs(out[i]));
    for (long i = morphC; i < morphC + 2 * w; ++i) after = std::max(after, std::abs(out[i]));
    // ... and over the 10 ms the kept side recovers in: B was quiet because it
    // was fading in from a dead A, not because C's arrival hid anything, so it
    // must not swell (it used to come up 19 dB in 12 ms).
    for (long i = morphC - 10 * w; i < morphC; ++i) before10 = std::max(before10, std::abs(out[i]));
    for (long i = morphC; i < morphC + 10 * w; ++i) after10 = std::max(after10, std::abs(out[i]));
    writeWav(wavOut, out, sr);
    const float bound = natStep(bufB) + natStep(bufC);
    // 300 ms after D (ramp 200 ms): D alone must remain, B and C must be gone.
    const int probe = (int) (0.200 * sr);
    auto powOf = [](const float* d, int n) { double p = 0.0; for (int i = 0; i < n; ++i) p += (double) d[i] * d[i]; return p / n; };
    const double bLeft = goertzelAmp(out, (int) morphD + (int) (0.300 * sr), probe, 330.0, sr);
    const double cLeft = goertzelAmp(out, (int) morphD + (int) (0.300 * sr), probe, 440.0, sr);
    const double bFull = 0.85 / 2.8, cFull = 0.70 / 2.8;                   // fundamentals at full level
    const double rmsOut = std::sqrt(powOf(out.data() + morphD + (int) (0.300 * sr), probe));
    const double rmsD   = std::sqrt(powOf(bufD.getReadPointer(0) + (int) (0.300 * sr), probe));   // D at full level
    // At D both sides sound (B quiet, C loud): C is promoted and recovers to the
    // voice's level, the power sum sqrt(B^2 + C^2) = 0.716, not to the amplitude
    // sum B + C = 0.818 (+1.1 dB). Measured as the sum of the materials' powers
    // (each rendered alone), 10-30 ms after D against the 20 ms before it; the
    // law gives 1.089 here (most of it C still fading in before D), the
    // amplitude sum 1.237, no recovery 1.076.
    const int wD = (int) (0.020 * sr);
    auto sidesPow = [&](long from) { return powOf(onlyB.data() + from, wD) + powOf(onlyC.data() + from, wD) + powOf(onlyD.data() + from, wD); };
    const double ratioD = std::sqrt(sidesPow(morphD + (long) (0.010 * sr)) / sidesPow(morphD - wD));
    const bool steady = after <= 1.5f * before + 0.01f && after10 <= 1.5f * before10 + 0.01f && std::abs(ratioD - 1.089) < 0.06;
    const bool pass = step <= bound && steady
                      && rmsOut > 0.9 * rmsD && rmsOut < 1.1 * rmsD && bLeft < 0.05 * bFull && cLeft < 0.05 * cFull;
    printf("  [%-34s] level before C %.3f/%.3f (2/10 ms)  after %.3f/%.3f  max step %.4f (materials %.4f)  D: level x%.3f (law 1.089)  +300 ms: rms %.3f (D alone %.3f) B %.4f C %.4f -> %s\n",
           label.c_str(), before, before10, after, after10, step, bound, ratioD, rmsOut, rmsD, bLeft, cLeft,
           pass ? "CARRIES ON, THEN D ALONE" : (step > bound || ! steady) ? "*** JUMPED ***" : "*** D NEVER ARRIVES / B OR C STAYS ***");
    return pass;
}

// A plain OneShot at +7 semitones ending at full level: the stretcher's last
// window must be played out to its natural end, not cut where a counter runs
// out (the end used to be a 0.046 step on a 0.5 source).
static bool runOneShotEndStretchCase(const std::string& label, SamplePlayer::PitchShiftQuality quality,
                                     double semitones, int sr, const std::string& wavOut)
{
    auto bufA = makeAsymmetricTone(sr, 2.0, 220.0, 0.50f, 0.0);
    SamplePlayer master, voice;
    master.prepare(sr, 512); master.setLoopMode(SamplePlayer::LoopMode::OneShot);
    master.setNormalize(false); master.setCrossfadeMs(0.0f); master.setLoopOptimizeLevel(0);
    master.setLoopRegion(0.0f, 0.3f); master.setStartPos(0.0f); master.loadBuffer(bufA, sr);
    voice.prepare(sr, 512); voice.setPitchShiftQuality(quality); voice.setLoopMode(SamplePlayer::LoopMode::OneShot);
    voice.setNormalize(false); voice.setCrossfadeMs(0.0f);
    voice.shareBufferFrom(master); voice.setTransposeRatio(std::pow(2.0, semitones / 12.0)); voice.retrigger();
    std::vector<float> out(2 * sr, 0.0f);
    for (long i = 0; i < (long) out.size(); i += 512)
        voice.renderPitchedBlock(out.data() + i, (int) std::min<long>(512, (long) out.size() - i));
    long last = -1; float peak = 0.0f;
    for (long i = 0; i < (long) out.size(); ++i) { peak = std::max(peak, std::abs(out[i])); if (std::abs(out[i]) > 1.0e-6f) last = i; }
    float lastValue = last >= 0 ? std::abs(out[last]) : 0.0f, final1ms = 0.0f;
    for (long i = std::max(0L, last - (long) (0.001 * sr)); i <= last; ++i) final1ms = std::max(final1ms, std::abs(out[i]));
    writeWav(wavOut, out, sr);
    // -60 dB re peak at the very last sample: a window played out to its end,
    // not a counter running out (the cut used to sit at -41..-51 dB).
    const bool pass = last > 0 && lastValue < 0.001f * peak && final1ms < 0.02f * peak;
    printf("  [%-34s] peak %.3f  last sample %.5f (%.0f dB)  final 1 ms peak %.4f  ends at %.0f ms -> %s\n", label.c_str(), peak,
           lastValue, 20.0 * std::log10(lastValue / peak + 1e-12), final1ms, 1000.0 * (double) last / sr,
           pass ? "PLAYS OUT TO ITS END" : "*** CUT ***");
    return pass;
}

// A held Loop A regenerated to a silent OneShot that ends ahead: the incoming
// side is dead and A fades out under the ramp. A further publication C lands at
// 180 ms of that 200 ms fade. A is the only sound and is kept; it must stay on
// its fade (the note is ending), not come back up (it used to recover to full
// level within 10 ms, +16 dB, and a stream of publications held a dead note at
// full level).
static bool runLatePublicationOnFadingTailCase(const std::string& label, int sr, const std::string& wavOut)
{
    auto bufA = makeAsymmetricTone(sr, 2.0, 220.0, 0.50f, 0.0);
    auto bufC = makeAsymmetricTone(sr, 2.0, 440.0, 0.70f, 2.0);
    juce::AudioBuffer<float> silent(1, bufA.getNumSamples()); silent.clear();
    SamplePlayer master, voice;
    master.prepare(sr, 512); master.setLoopMode(SamplePlayer::LoopMode::Loop);
    master.setNormalize(false); master.setCrossfadeMs(20.0f); master.setLoopOptimizeLevel(0);
    master.setLoopRegion(0.3f, 0.6f); master.setStartPos(0.3f); master.loadBuffer(bufA, sr);
    voice.prepare(sr, 512); voice.setLoopMode(SamplePlayer::LoopMode::Loop);
    voice.setNormalize(false); voice.setCrossfadeMs(20.0f);
    voice.shareBufferFrom(master); voice.setTransposeRatio(1.0); voice.retrigger();
    const int n = bufA.getNumSamples();
    const long morphAt = (long) std::lround(0.1 * sr);
    const long morphC  = morphAt + (long) std::lround(0.180 * sr);
    const float oneShotEnd = (0.3f * n + (float) morphAt + 0.010f * sr) / (float) n;
    std::vector<float> out; float s = 0.0f;
    for (long i = 0; i < (long) sr; ++i) {
        if (i == morphAt) { master.setLoopMode(SamplePlayer::LoopMode::OneShot); master.setLoopRegion(0.3f, oneShotEnd);
                            master.loadBuffer(silent, sr); voice.morphToBufferFrom(master, 200.0f); }
        if (i == morphC)  { voice.drainRetiredSnapshot(); master.loadBuffer(bufC, sr); voice.morphToBufferFrom(master, 200.0f); }
        voice.renderPitchedBlock(&s, 1); out.push_back(s);
    }
    const long w = (long) std::lround(0.010 * sr);
    float before = 0.0f, after = 0.0f, later = 0.0f, tail = 0.0f;
    for (long i = morphC - w; i < morphC; ++i) before = std::max(before, std::abs(out[i]));
    for (long i = morphC; i < morphC + w; ++i) after = std::max(after, std::abs(out[i]));
    for (long i = morphC + w; i < morphC + 3 * w; ++i) later = std::max(later, std::abs(out[i]));
    for (long i = morphC + (long) (0.250 * sr); i < (long) out.size(); ++i) tail = std::max(tail, std::abs(out[i]));
    writeWav(wavOut, out, sr);
    // No swell within the recovery window or after it, and the note ends: the
    // head is dead, so C is installed under a dead head and the voice stops
    // when the kept side has faded.
    const bool pass = before > 0.02f && after <= 1.1f * before + 1.0e-3f && later <= 1.1f * before + 1.0e-3f && tail < 1.0e-5f;
    printf("  [%-34s] fading side 10 ms before C %.4f  10 ms after %.4f  10-30 ms after %.4f  250 ms after %.6f -> %s\n",
           label.c_str(), before, after, later, tail, pass ? "STAYS ON ITS FADE" : "*** CAME BACK UP ***");
    return pass;
}

// A held Loop A regenerated to a silent OneShot that ends ahead (the head is
// dead, A fades out over the 200 ms ramp), and then a STREAM of publications
// every 16 ms for a second. A must finish its fade on the original schedule
// and the voice must stop; no publication may restart or stretch the fade
// (it used to: A still at -2 dB after 2 s under a 4 ms stream).
static bool runDeadHeadStreamCase(const std::string& label, double intervalMs, int sr, const std::string& wavOut)
{
    auto bufA = makeAsymmetricTone(sr, 2.0, 220.0, 0.50f, 0.0);
    auto bufC = makeAsymmetricTone(sr, 2.0, 440.0, 0.70f, 2.0);
    juce::AudioBuffer<float> silent(1, bufA.getNumSamples()); silent.clear();
    auto mk = [&](SamplePlayer& m, SamplePlayer& v) {
        m.prepare(sr, 512); m.setLoopMode(SamplePlayer::LoopMode::Loop);
        m.setNormalize(false); m.setCrossfadeMs(20.0f); m.setLoopOptimizeLevel(0);
        m.setLoopRegion(0.3f, 0.6f); m.setStartPos(0.3f); m.loadBuffer(bufA, sr);
        v.prepare(sr, 512); v.setLoopMode(SamplePlayer::LoopMode::Loop);
        v.setNormalize(false); v.setCrossfadeMs(20.0f);
        v.shareBufferFrom(m); v.setTransposeRatio(1.0); v.retrigger(); };
    SamplePlayer master, voice, refMaster, ref; mk(master, voice); mk(refMaster, ref);
    const int n = bufA.getNumSamples();
    const long morphAt = (long) std::lround(0.1 * sr);
    const long interval = (long) std::lround(intervalMs * 0.001 * sr);
    const int morphSamples = (int) std::lround(0.200 * sr);
    const float oneShotEnd = (0.3f * n + (float) morphAt + 0.010f * sr) / (float) n;
    std::vector<float> out, refOut; float s = 0.0f, rs = 0.0f; long nextPub = morphAt + (long) std::lround(0.030 * sr);
    const long fadeEnd = morphAt + morphSamples;
    bool playingAfterSchedule = true;
    for (long i = 0; i < (long) (1.5 * sr); ++i) {
        if (i == morphAt) { master.setLoopMode(SamplePlayer::LoopMode::OneShot); master.setLoopRegion(0.3f, oneShotEnd);
                            master.loadBuffer(silent, sr); voice.morphToBufferFrom(master, 200.0f); }
        if (i == nextPub && i < morphAt + (long) (1.0 * sr)) { voice.drainRetiredSnapshot(); master.loadBuffer(bufC, sr);
                                                              voice.morphToBufferFrom(master, 200.0f); nextPub += interval; }
        voice.renderPitchedBlock(&s, 1); out.push_back(s);
        ref.renderPitchedBlock(&rs, 1); refOut.push_back(rs);
        if (i == fadeEnd + (long) (0.002 * sr)) playingAfterSchedule = voice.isPlaying();
    }
    // The fade must be exactly the unregenerated reference under the original
    // cos ramp, to the sample, through every publication: no cut, no step, no
    // restart (the incoming side is silent throughout; the stream's samples are
    // never heard because the head is dead).
    const double inc = 1.0 / morphSamples;
    float worst = 0.0f; long worstAt = -1; float during = 0.0f, after = 0.0f;
    for (long i = morphAt; i < fadeEnd + 100; ++i) {
        const double a = std::min(1.0, (i - morphAt) * inc);
        const float d = std::abs(out[i] - (float) std::cos(a * M_PI / 2) * refOut[i]);
        if (d > worst) { worst = d; worstAt = i; }
        if (i < fadeEnd) during = std::max(during, std::abs(out[i]));
    }
    for (long i = fadeEnd + (long) (0.002 * sr); i < (long) out.size(); ++i) after = std::max(after, std::abs(out[i]));
    writeWav(wavOut, out, sr);
    const bool pass = during > 0.05f && worst < 1.0e-4f && after < 1.0e-5f && ! playingAfterSchedule;
    printf("  [%-34s] |out - cos(ramp)*ref| max %.6f @+%.1f ms  after the schedule: peak %.6f, voice %s -> %s\n", label.c_str(), worst,
           worstAt > 0 ? 1000.0 * (double) (worstAt - morphAt) / sr : 0.0, after, playingAfterSchedule ? "still playing" : "stopped",
           pass ? "FADE ENDS ON SCHEDULE" : "*** STREAM CHANGED THE FADE ***");
    return pass;
}

// The stretch path at +7 semitones: a held Loop note regenerated to a silent
// OneShot ending 10 ms ahead. The outgoing side must fade out to silence over
// the ramp and then STOP CLEANLY: the stretcher's last window is played out,
// not cut (the voice used to stop emitting one window early, a step at the
// gain the tail had a window before).
static bool runHeadEndsFirstStretchCase(const std::string& label, int sr, const std::string& wavOut)
{
    auto bufA = makeAsymmetricTone(sr, 2.0, 220.0, 0.50f, 0.0);
    juce::AudioBuffer<float> silent(1, bufA.getNumSamples()); silent.clear();
    SamplePlayer master, voice;
    master.prepare(sr, 512); master.setLoopMode(SamplePlayer::LoopMode::Loop);
    master.setNormalize(false); master.setCrossfadeMs(20.0f); master.setLoopOptimizeLevel(0);
    master.setLoopRegion(0.3f, 0.6f); master.setStartPos(0.3f); master.loadBuffer(bufA, sr);
    voice.prepare(sr, 512); voice.setLoopMode(SamplePlayer::LoopMode::Loop);
    voice.setNormalize(false); voice.setCrossfadeMs(20.0f);
    voice.shareBufferFrom(master); voice.setTransposeRatio(std::pow(2.0, 7.0 / 12.0)); voice.retrigger();
    const int n = bufA.getNumSamples();
    const long morphAt = (long) std::lround(0.1 * sr);
    const float oneShotEnd = (0.3f * n + (float) morphAt * (float) std::pow(2.0, 7.0 / 12.0) + 0.010f * sr) / (float) n;
    std::vector<float> out(sr, 0.0f);
    for (long i = 0; i < (long) sr; i += 512) {
        if (i <= morphAt && morphAt < i + 512) { master.setLoopMode(SamplePlayer::LoopMode::OneShot); master.setLoopRegion(0.3f, oneShotEnd);
                                                  master.loadBuffer(silent, sr); voice.morphToBufferFrom(master, 200.0f); }
        voice.renderPitchedBlock(out.data() + i, (int) std::min<long>(512, (long) sr - i));
    }
    long last = -1; float peak = 0.0f;
    for (long i = 0; i < (long) out.size(); ++i) { peak = std::max(peak, std::abs(out[i])); if (std::abs(out[i]) > 1.0e-6f) last = i; }
    float finalPeak = 0.0f, lastStep = 0.0f;
    if (last > 0) {
        for (long i = std::max(0L, last - (long) (0.002 * sr)); i <= last; ++i) finalPeak = std::max(finalPeak, std::abs(out[i]));
        lastStep = std::abs(out[last] - (last + 1 < (long) out.size() ? out[last + 1] : 0.0f));
    }
    writeWav(wavOut, out, sr);
    const bool pass = last > morphAt && finalPeak < 0.05f * peak && lastStep < 0.01f * peak;
    printf("  [%-34s] peak %.3f  final 2 ms peak %.4f  last step %.4f  ends at +%.0f ms -> %s\n", label.c_str(), peak,
           finalPeak, lastStep, last > 0 ? 1000.0 * (double) (last - morphAt) / sr : 0.0,
           pass ? "FADES OUT AND ENDS CLEAN" : "*** CUT ***");
    return pass;
}

static bool runPingPongRegenCase(const std::string& label, float endA, float endB,
                                 double delayMs, float xfMs, int sr, const std::string& wavOut)
{
    auto bufA = makeAsymmetricTone(sr, 2.0, 220.0, 0.50f, 0.0);
    auto bufB = makeAsymmetricTone(sr, 2.0, 330.0, 0.85f, 2.0);

    SamplePlayer master, voice;
    master.prepare(sr, 512); master.setLoopMode(SamplePlayer::LoopMode::PingPong);
    master.setNormalize(false); master.setCrossfadeMs(xfMs); master.setLoopOptimizeLevel(0);
    master.setLoopRegion(0.2f, endA); master.setStartPos(0.2f); master.loadBuffer(bufA, sr);
    voice.prepare(sr, 512); voice.setLoopMode(SamplePlayer::LoopMode::PingPong);
    voice.setNormalize(false); voice.setCrossfadeMs(xfMs);
    voice.shareBufferFrom(master); voice.setTransposeRatio(1.0); voice.retrigger();
    // The same note on the same buffer, never regenerated: what the OUTGOING
    // side of the morph must keep playing (times its cos ramp).
    SamplePlayer refMaster, ref;
    refMaster.prepare(sr, 512); refMaster.setLoopMode(SamplePlayer::LoopMode::PingPong);
    refMaster.setNormalize(false); refMaster.setCrossfadeMs(xfMs); refMaster.setLoopOptimizeLevel(0);
    refMaster.setLoopRegion(0.2f, endA); refMaster.setStartPos(0.2f); refMaster.loadBuffer(bufA, sr);
    ref.prepare(sr, 512); ref.setLoopMode(SamplePlayer::LoopMode::PingPong);
    ref.setNormalize(false); ref.setCrossfadeMs(xfMs);
    ref.shareBufferFrom(refMaster); ref.setTransposeRatio(1.0); ref.retrigger();

    std::vector<float> out, refOut; float s = 0.0f, rs = 0.0f;
    int prevDir = voice.getPlayDirection();
    long reflectAt = -1, morphAt = -1;
    const int delay = (int) std::lround(delayMs * 0.001 * sr);
    const int morphSamples = (int) std::lround(0.200 * sr);
    // Where the first reflection at endA will be, counted from P1 = 0.2.
    const long expectedReflect = (long) std::lround((endA - 0.2f) * bufA.getNumSamples());
    for (long i = 0; i < (long) sr * 3; ++i) {
        if (reflectAt < 0 && prevDir > 0 && voice.getPlayDirection() < 0) reflectAt = i;
        prevDir = voice.getPlayDirection();
        const bool due = delay >= 0 ? (reflectAt >= 0 && i == reflectAt + delay)
                                    : (i == expectedReflect + delay);
        if (morphAt < 0 && due) {
            master.setLoopRegion(0.2f, endB); master.loadBuffer(bufB, sr);
            voice.morphToBufferFrom(master, 200.0f); morphAt = i;
        }
        voice.renderPitchedBlock(&s, 1); out.push_back(s);
        ref.renderPitchedBlock(&rs, 1); refOut.push_back(rs);
    }
    if (reflectAt < 0) reflectAt = expectedReflect;
    float naturalMax = 0.0f;
    for (long i = 1; i < morphAt - 10; ++i) naturalMax = std::max(naturalMax, std::abs(out[i] - out[i - 1]));
    for (long i = morphAt + morphSamples + 4096; i + 1 < (long) out.size(); ++i)
        naturalMax = std::max(naturalMax, std::abs(out[i + 1] - out[i]));
    float windowMax = 0.0f;
    for (long i = morphAt; i < morphAt + morphSamples + 4096 && i < (long) out.size(); ++i)
        windowMax = std::max(windowMax, std::abs(out[i] - out[i - 1]));
    auto rms = [&](long a, long b){ double q = 0; long n = 0;
        for (long i = std::max(0L, a); i < b && i < (long) out.size(); ++i) { q += out[i] * out[i]; ++n; }
        return n ? std::sqrt(q / n) : 0.0; };
    const double rmsA = rms(morphAt - 4000, morphAt);
    const double rmsB = rms(morphAt + morphSamples + 4096, morphAt + morphSamples + 8096);
    writeWav(wavOut, out, sr);
    // The sharp test, for a regenerate AFTER the reflection: out - cos(ramp)*ref
    // is the incoming side alone, sin(ramp)*B, which starts from zero and is
    // smooth. Anything the regenerate does to the OUTGOING side (a fade that
    // re-anchors on the new buffer's loop end, say) lands in this residual at
    // full level while the incoming side is still nearly silent. Bound per
    // sample: the incoming gain times B's own natural step, plus the ramp's own
    // slope. (A regenerate BEFORE the reflection makes the voice turn at the new
    // end while the reference turns at the old one, so the two legitimately
    // differ; those cases rely on the coarse step check above.)
    float residualMax = 0.0f, residualBoundAt = 0.0f; long residualAt = -1;
    if (delay >= 0 && morphAt > 0) {
        float natB = 0.0f;
        for (long i = morphAt + morphSamples + 4096; i + 1 < (long) out.size(); ++i)
            natB = std::max(natB, std::abs(out[i + 1] - out[i]));
        const double inc = 1.0 / morphSamples;
        auto resid = [&](long i) {
            const double a = std::min(1.0, (i - morphAt) * inc);
            return out[i] - (float) std::cos(a * M_PI / 2) * refOut[i]; };
        for (long i = morphAt + 1; i < morphAt + morphSamples && i < (long) out.size(); ++i) {
            const double a = std::min(1.0, (i - morphAt) * inc);
            const float bound = 1.5f * (float) std::sin(a * M_PI / 2) * natB + 3.0e-4f;
            const float d = std::abs(resid(i) - resid(i - 1));
            if (d - bound > residualMax - residualBoundAt) { residualMax = d; residualBoundAt = bound; residualAt = i; }
        }
    }
    const bool outgoingKept = residualMax <= residualBoundAt;
    const bool clickFree = windowMax <= naturalMax * 1.5f + 1.0e-4f;
    const bool transitioned = rmsB > rmsA * 1.2;
    const bool pass = morphAt > 0 && clickFree && transitioned && outgoingKept;
    printf("  [%-34s] morphAt=%+.1f ms after the reflection  windowMax=%.5f  naturalMax=%.5f  rmsA=%.3f rmsB=%.3f",
           label.c_str(), 1000.0 * (double) (morphAt - reflectAt) / sr, windowMax, naturalMax, rmsA, rmsB);
    if (delay >= 0)
        printf("  outgoing residual step=%.5f (bound %.5f @+%.1f ms)", residualMax, residualBoundAt,
               residualAt > 0 ? 1000.0 * (double) (residualAt - morphAt) / sr : 0.0);
    printf(" -> %s\n", pass ? "CLICK-FREE XFADE"
                        : (! outgoingKept ? "*** OUTGOING SIDE STEPPED ***" : (clickFree ? "*** NO XFADE ***" : "*** STEP ***")));
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
    ok &= runStreamCase("stream every 8% of xfade",  200.0f, 0.08, 150, sr,
                        "/tmp/sampler_follow_stream_08.wav");
    ok &= runStreamCase("stream every 60% of xfade", 200.0f, 0.60, 25, sr,
                        "/tmp/sampler_follow_stream_60.wav");
    ok &= runStreamCase("40% of a 20 ms xfade", 20.0f, 0.40, 60, sr,
                        "/tmp/sampler_follow_stream_40_20ms.wav");
    ok &= runStreamCase("40% of a 10 ms xfade", 10.0f, 0.40, 60, sr,
                        "/tmp/sampler_follow_stream_40_10ms.wav");
    printf("Loop: a regenerate whose new audio has its own seam (optimizer High, Crossfade 20 ms):\n");
    ok &= runLoopRegenCase("loop regen 300 before new seam", 2, 20.0f, 300,  sr, "/tmp/sampler_follow_loop_regen300.wav");
    ok &= runLoopRegenCase("loop regen 3000 before new seam", 2, 20.0f, 3000, sr, "/tmp/sampler_follow_loop_regen3000.wav");
    ok &= runLoopRegenCase("loop regen, optimizer Off", 0, 20.0f, 300, sr, "/tmp/sampler_follow_loop_regen_opt0.wav");
    printf("Ends during a morph: the outgoing side ends, or the incoming side ends first:\n");
    ok &= runDeadTailCase("dead fade-from, 2nd publication 40 ms", 40.0, sr, "/tmp/sampler_follow_deadtail40.wav");
    ok &= runDeadTailCase("dead fade-from, 2nd publication 150 ms", 150.0, sr, "/tmp/sampler_follow_deadtail150.wav");
    ok &= runHeadEndsFirstCase("incoming OneShot ends 10 ms ahead", sr, "/tmp/sampler_follow_headends.wav");
    ok &= runHeadEndsFirstStretchCase("same, +7 semitones (stretch path)", sr, "/tmp/sampler_follow_headends_stretch.wav");
    ok &= runThirdPublicationCase("3rd publication after a dead tail", sr, "/tmp/sampler_follow_thirdpub.wav");
    ok &= runLatePublicationOnFadingTailCase("publication at 180 ms of a fade-out", sr, "/tmp/sampler_follow_latepub.wav");
    ok &= runDeadHeadStreamCase("stream every 16 ms over a dead head", 16.0, sr, "/tmp/sampler_follow_deadhead_stream16.wav");
    ok &= runDeadHeadStreamCase("stream every 4 ms over a dead head", 4.0, sr, "/tmp/sampler_follow_deadhead_stream4.wav");
    ok &= runOneShotEndStretchCase("plain OneShot end, +7 st, Default", SamplePlayer::PitchShiftQuality::Default, 7.0, sr, "/tmp/sampler_follow_oneshot_stretch.wav");
    ok &= runOneShotEndStretchCase("plain OneShot end, -24 st, HighQ", SamplePlayer::PitchShiftQuality::HighQuality, -24.0, sr, "/tmp/sampler_follow_oneshot_stretch_hq.wav");
    printf("Ping-pong: a regenerate landing inside a turnaround fade (sampler Crossfade 30 ms):\n");
    ok &= runPingPongRegenCase("pp regen 2 ms after reflect",  0.6f, 0.6104f, 2.0,  30.0f, sr, "/tmp/sampler_follow_pp_2ms.wav");
    ok &= runPingPongRegenCase("pp regen 15 ms after reflect", 0.6f, 0.6104f, 15.0, 30.0f, sr, "/tmp/sampler_follow_pp_15ms.wav");
    ok &= runPingPongRegenCase("pp regen 50 ms after reflect", 0.6f, 0.601f,  50.0, 30.0f, sr, "/tmp/sampler_follow_pp_50ms.wav");
    // The regenerate lands while the voice is still moving forward and the new
    // end is a few ms away from the old one, in either order (the +-2 ms
    // turnaround search moves each end by up to 96 samples, so the marker
    // offsets of 4 and 8 ms keep the order with margin): the reflection then happens at the new
    // end, inside the old buffer's fade zone, and the outgoing side must not
    // step.
    ok &= runPingPongRegenCase("pp regen 10 ms before, marker +4 ms", 0.6f, 0.602f, -10.0, 30.0f, sr, "/tmp/sampler_follow_pp_before_up.wav");
    ok &= runPingPongRegenCase("pp regen 10 ms before, marker -8 ms", 0.6f, 0.596f, -10.0, 30.0f, sr, "/tmp/sampler_follow_pp_before_down.wav");
    printf("WAVs: /tmp/sampler_follow_*.wav\n");
    printf("%s\n", ok ? "ALL PASS" : "*** FAIL ***");
    return ok ? 0 : 1;
}
