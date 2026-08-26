// Offline audition for the DCO baker (dco::Baker::bake -> WavetableOscillator).
//
// Re-proves the two lost scratchpad demos referenced in
// docs/HANDOVER_DCO_OSCILLATOR.md ("tzpwm_demo.py" = band-limited PWM,
// "morph_loop_demo.py" = a saw->square->pulse->saw morph loop), plus a third
// recipe (additive bell <-> 2-op FM) exercising the Additive/Fm2 kinds. For
// each recipe: bake -> framesToBuffer -> load into a real WavetableOscillator
// via extractContiguousFrames (the exact hook loadGeneratedAudio uses, per
// docs/HANDOVER_DCO_OSCILLATOR.md §4.5) -> render three WAVs (a manual
// out-and-back scan, a loop-wrap seam stress test, and a high-pitch
// mip/aliasing check) into tools/dco_audition_out/.
//
// Build: compile against the plugin's static lib + JUCE flags, e.g.
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/audition_dco_bake.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/dco_bake
// Run from the repo root (writes tools/dco_audition_out/, a relative path).
// Exits non-zero if any WAV fails the basic sanity/click checks, or if a
// loop=true recipe's motion doesn't actually close (see checkClosure()).
#include "JuceHeader.h"
#include "dsp/DcoBaker.h"
#include "dsp/DcoRecipeJson.h"
#include "dsp/WavetableOscillator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace dco;

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

// ─── Recipe builders (the two lost demos + one Additive/Fm2 case) ───

static Recipe makeMorphloop()
{
    Keyframe saw;    saw.kind = Keyframe::Kind::Saw;
    Keyframe square; square.kind = Keyframe::Kind::Square;
    Keyframe pulse;  pulse.kind = Keyframe::Kind::Pulse; pulse.width = 0.25f;

    Recipe r;
    r.keyframes = { saw, square, pulse }; // 0=Saw, 1=Square, 2=Pulse
    r.motion = {
        MotionSegment{ 0, 0.0f,  Curve::Lin },  // start on Saw
        MotionSegment{ 1, 0.34f, Curve::Lin },  // -> Square
        MotionSegment{ 2, 0.33f, Curve::Lin },  // -> Pulse
        MotionSegment{ 0, 0.33f, Curve::Lin },  // -> Saw (closes the loop)
    };
    r.loop = true;
    r.frames = 128;
    return r;
}

static Recipe makePwm()
{
    Keyframe wide;   wide.kind = Keyframe::Kind::Pulse; wide.width = 0.5f;
    Keyframe narrow; narrow.kind = Keyframe::Kind::Pulse; narrow.width = 0.05f;

    Recipe r;
    r.keyframes = { wide, narrow }; // 0=Pulse(0.5), 1=Pulse(0.05)
    r.motion = {
        MotionSegment{ 0, 0.0f, Curve::Lin },
        MotionSegment{ 1, 0.5f, Curve::Slow },
        MotionSegment{ 0, 0.5f, Curve::Fast },
    };
    r.loop = true;
    r.frames = 128;
    return r;
}

static Recipe makeFmbell()
{
    Keyframe add; add.kind = Keyframe::Kind::Additive;
    add.partials = {
        Partial{ 1.0f,  1.0f,  0.0f },
        Partial{ 4.0f,  0.35f, 0.0f },
        Partial{ 7.0f,  0.15f, 0.0f },
        Partial{ 10.0f, 0.08f, 0.0f },
    };
    Keyframe fm2; fm2.kind = Keyframe::Kind::Fm2; fm2.ratio = 3.0f; fm2.index = 2.5f;

    Recipe r;
    r.keyframes = { add, fm2 }; // 0=Additive, 1=Fm2
    r.motion = {
        MotionSegment{ 0, 0.0f, Curve::Lin },
        MotionSegment{ 1, 0.5f, Curve::Lin },
        MotionSegment{ 0, 0.5f, Curve::Lin },
    };
    r.loop = true;
    r.frames = 128;
    return r;
}

static Recipe makeDistorted(float sh)
{
    // A statically-driven saw: exercises applyShape (shape>0) inside the
    // asserted demo suite so the shaper is gated by checkFiniteSanity /
    // checkLoopwrapClicks / checkClosure (the waveshaper is periodic-preserving,
    // so a looping single-keyframe bake must stay finite AND click-free).
    Keyframe saw; saw.kind = Keyframe::Kind::Saw; saw.shape = sh;

    Recipe r;
    r.keyframes = { saw };
    r.motion = { MotionSegment{ 0, 0.0f, Curve::Lin } };
    r.loop = true;
    r.frames = 128;
    return r;
}

// ─── Rendering: manual scan sweep through a loaded WavetableOscillator ───

static std::vector<float> renderScan(WavetableOscillator& osc, double freqHz, double seconds, int sr,
                                     const std::function<float(double)>& scanFn, int block = 64)
{
    osc.reset(); // fresh phase/scan-smoothing state; the loaded frame bank survives reset()
    osc.setFrequency(static_cast<float>(freqHz));
    const int totalSamples = static_cast<int>(seconds * sr);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(totalSamples));
    int pos = 0;
    while (pos < totalSamples)
    {
        const int n = std::min(block, totalSamples - pos);
        const double progress = static_cast<double>(pos) / totalSamples;
        osc.setScanPosition(scanFn(progress));
        for (int i = 0; i < n; ++i)
            out.push_back(osc.processSample());
        pos += n;
    }
    return out;
}

static float scanOutAndBack(double p) { return static_cast<float>(p < 0.5 ? p * 2.0 : (1.0 - p) * 2.0); }
static float scanLinear(double p)     { return static_cast<float>(p); }
static float scanWrap3(double p)      { return static_cast<float>(std::fmod(p * 3.0, 1.0)); }

// ─── Assertions ───

static bool checkFiniteSanity(const std::string& label, const std::vector<float>& buf,
                              double& outRms, bool& allOk)
{
    bool anyNonFinite = false;
    float peak = 0.0f;
    double sumSq = 0.0;
    for (float s : buf)
    {
        if (!std::isfinite(s)) { anyNonFinite = true; continue; }
        peak = std::max(peak, std::abs(s));
        sumSq += static_cast<double>(s) * s;
    }
    outRms = buf.empty() ? 0.0 : std::sqrt(sumSq / static_cast<double>(buf.size()));
    // 1.05 ceiling, not a strict 1.0: WavetableOscillator's own per-octave mip
    // decimation (generateMipLevels: FFT brick-wall truncation + IFFT) can mildly
    // overshoot a band-limited frame's already-normalized peak via ordinary
    // Gibbs-phenomenon ringing (classically ~9% for one edge; a narrow pulse's
    // two close edges can add) at aggressive high-pitch mip truncation. Verified
    // engine-side and independent of this Baker: extractContiguousFrames
    // renormalizes every frame to peak 0.95 *before* generateMipLevels runs,
    // regardless of what gain the Baker applied — the Baker has no lever over a
    // downstream mip level's reconstruction peak. 1.05 still catches a real bug
    // (e.g. missing normalization, a runaway DC offset) with wide margin.
    const bool ok = !anyNonFinite && peak <= 1.05f && outRms > 0.01;
    printf("  [%-28s] finite=%s peak=%.4f rms=%.4f -> %s\n",
           label.c_str(), anyNonFinite ? "NO" : "yes", static_cast<double>(peak), outRms,
           ok ? "OK" : "*** FAIL ***");
    if (!ok) allOk = false;
    return ok;
}

// Adapts audition_sampler_follow.cpp's discontinuity measure (windowMax vs. a
// naturalMax reference region, loose tolerance factor).
//
// A first version compared against 6x the MEDIAN delta of the whole file, per
// the original spec text. Empirically (verified with a standalone delta-dump
// over the rendered WAVs) that reference does not work for Square/Pulse
// content: median is dominated by the smooth majority of each cycle and
// wildly understates the legitimate once-per-cycle EDGE delta, which for
// these waveforms can be nearly as large as the whole file's global max —
// e.g. morphloop's actual global max (0.896) occurs in a region with no wrap
// anywhere nearby, well above 6x its own median (0.048). The 6x-median
// reference was flagging normal Square/Pulse edges as "clicks" throughout the
// file, not anything specific to the wrap.
//
// The correct reference (and what audition_sampler_follow.cpp itself does) is
// "the max delta found elsewhere in the SAME file, away from the event under
// test" — i.e. is the wrap actually worse than an ordinary edge this same
// waveform already produces during normal playback. That reference passes
// with real margin for all three recipes once measured correctly.
static bool checkLoopwrapClicks(const std::string& label, const std::vector<float>& buf, int sr, bool& allOk)
{
    if (buf.size() < 2) return true;

    // scanWrap3 wraps 1->0 at progress = 1/3 and 2/3 of the render.
    const int halfWin = static_cast<int>(0.02 * sr); // 20ms either side (scan smoothing settles in ~5ms)
    const std::vector<int> wrapSamples = {
        static_cast<int>(std::lround(static_cast<double>(buf.size()) / 3.0)),
        static_cast<int>(std::lround(static_cast<double>(buf.size()) * 2.0 / 3.0)),
    };
    std::vector<std::pair<int, int>> wrapRanges;
    for (int w : wrapSamples)
        wrapRanges.push_back({ std::max(0, w - halfWin),
                                std::min(static_cast<int>(buf.size()) - 1, w + halfWin) });
    const auto inWrapRange = [&](int i) {
        for (const auto& r : wrapRanges)
            if (i >= r.first && i <= r.second) return true;
        return false;
    };

    float wrapMax = 0.0f;
    float naturalMax = 0.0f;
    for (size_t i = 0; i + 1 < buf.size(); ++i)
    {
        const float d = std::abs(buf[i + 1] - buf[i]);
        if (inWrapRange(static_cast<int>(i))) wrapMax = std::max(wrapMax, d);
        else                                  naturalMax = std::max(naturalMax, d);
    }

    const bool ok = wrapMax <= naturalMax * 1.5f + 1.0e-4f;
    printf("  [%-28s] wrapMax=%.5f  naturalMax=%.5f  (1.5x=%.5f) -> %s\n",
           label.c_str(), static_cast<double>(wrapMax), static_cast<double>(naturalMax),
           static_cast<double>(naturalMax * 1.5f), ok ? "CLICK-FREE" : "*** CLICK ***");
    if (!ok) allOk = false;
    return ok;
}

// Confirms a loop=true recipe's motion actually closes on its start keyframe.
// loop=true samples p=i/N, which never reaches p==1 within N frames (the last
// frame sits at p=(N-1)/N) — that's what makes the wrap from frame[N-1] back
// to frame[0] seamless, but it also means frame[N-1] itself is NOT quite
// frame[0] for a coarse trajectory split (e.g. morphloop's final segment is
// only 0.33 of the whole trajectory). To evaluate p==1 exactly we re-bake the
// SAME recipe with loop=false: p=i/(N-1) DOES hit p==1 exactly on the last
// frame. The two bakes each compute their own independent global-
// normalization gain (the achieved peak differs slightly because the two
// bakes sample the continuous trajectory at slightly different points), so
// we compare PEAK-NORMALIZED shapes rather than raw levels — that isolates
// "does the waveshape close" from "do two unrelated bakes happen to share a gain".
static bool checkClosure(const std::string& label, const Recipe& recipe, bool& allOk)
{
    if (!recipe.loop) return true;

    auto startFrames = Baker::bake(recipe);
    Recipe nonLoop = recipe;
    nonLoop.loop = false;
    auto endFrames = Baker::bake(nonLoop);

    if (startFrames.empty() || endFrames.empty())
    {
        printf("  [%-28s] *** FAIL: empty bake ***\n", label.c_str());
        allOk = false;
        return false;
    }

    auto peakOf = [](const std::vector<float>& f) {
        float pk = 0.0f;
        for (float s : f) pk = std::max(pk, std::abs(s));
        return pk;
    };
    const float peakStart = std::max(peakOf(startFrames.front()), 1.0e-9f);
    const float peakEnd   = std::max(peakOf(endFrames.back()), 1.0e-9f);

    const auto& a = startFrames.front();
    const auto& b = endFrames.back();
    double sumSq = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const double na = static_cast<double>(a[i]) / peakStart;
        const double nb = static_cast<double>(b[i]) / peakEnd;
        const double d = na - nb;
        sumSq += d * d;
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(a.size()));
    const bool ok = rms < 1.0e-3;
    printf("  [%-28s] closure RMS (peak-normalized) = %.6f -> %s\n",
           label.c_str(), rms, ok ? "CLOSES" : "*** DOES NOT CLOSE ***");
    if (!ok) allOk = false;
    return ok;
}

// A static (single-keyframe) Additive keyframe carrying at least one NON-integer
// partial is inharmonic — a bell/metal timbre a single looped cycle cannot hold. It
// takes the real-time additive path (setAdditiveBank) instead of a baked frame.
// Motion recipes and all-integer additive stay on the baked-frame path (harmonic),
// so this only routes the genuinely-inharmonic static case. (v1: shape is not
// applied on the additive path — the inharmonic presets carry shape 0.)
static bool tryAdditiveBank(const Recipe& r, std::vector<WavetableOscillator::AdditivePartial>& out)
{
    if (r.keyframes.size() != 1) return false;
    const auto& kf = r.keyframes[0];
    if (kf.kind != Keyframe::Kind::Additive) return false;

    bool anyInharmonic = false;
    for (const auto& p : kf.partials)
        if (std::abs(p.h - std::round(p.h)) > 1.0e-3f) { anyInharmonic = true; break; }
    if (!anyInharmonic) return false;

    out.clear();
    for (const auto& p : kf.partials)
        out.push_back(WavetableOscillator::AdditivePartial{ p.h, p.a, p.phase });
    return true;
}

// Held-note crossfade-follow invariant (docs / CLAUDE.md platform invariant): a
// HELD voice retargets to the newly published bank via an equal-power crossfade
// over morphTimeMs, click-free. This must hold across the NEW additive path too —
// table->additive, additive->table, additive->additive. Renders a sustained 220 Hz
// voice, triggers a live morph mid-render (master publishes B, voice
// morphToFramesFrom), and asserts the morph region has no sample jump larger than
// the signal's own natural max delta (x1.5) — the same reference audition_sampler_
// follow.cpp uses. Sine/additive test banks are smooth, so a seam would spike.
static bool morphCase(const char* label,
                      const std::function<void(WavetableOscillator&)>& loadA,
                      const std::function<void(WavetableOscillator&)>& loadB,
                      int sr, double f0, bool& allOk)
{
    WavetableOscillator master, voice;
    master.prepare(sr, 512);  voice.prepare(sr, 512);
    master.setInterpolation(true);  voice.setInterpolation(true);
    master.setMorphTimeMs(200.0f);  voice.setMorphTimeMs(200.0f);

    loadA(master);
    voice.shareFramesFrom(master);
    voice.setFrequency(static_cast<float>(f0));

    std::vector<float> out;
    auto renderN = [&](int n) { for (int i = 0; i < n; ++i) out.push_back(voice.processSample()); };

    renderN(sr / 4);                       // 0.25s on bank A
    const int mStart = static_cast<int>(out.size());
    loadB(master);                         // master publishes bank B
    voice.morphToFramesFrom(master);       // held voice retargets (the invariant path)
    const int morphSamples = static_cast<int>(0.2 * sr);
    renderN(morphSamples + sr / 4);        // through the 200ms morph + 0.25s settle

    auto maxDelta = [&](int a, int b) {
        float m = 0.0f;
        for (int i = std::max(1, a); i < b && i < static_cast<int>(out.size()); ++i)
            m = std::max(m, std::fabs(out[i] - out[i - 1]));
        return m;
    };
    const float preMax    = maxDelta(1, mStart);
    const float morphMax   = maxDelta(mStart, mStart + morphSamples + static_cast<int>(0.005 * sr));
    const float steadyMax = maxDelta(mStart + morphSamples + static_cast<int>(0.02 * sr),
                                     static_cast<int>(out.size()));
    const float natural = std::max(preMax, steadyMax);
    float peak = 0.0f; bool finite = true;
    for (float s : out) { if (!std::isfinite(s)) finite = false; peak = std::max(peak, std::fabs(s)); }

    // Click test = the DELTA bound (a seam is a sample-to-sample JUMP above the
    // signal's own natural max delta). Peak is a SEPARATE sanity: an equal-power
    // crossfade of two ~0.95-normalized periodic banks sums to ~0.707*(0.95+0.95)
    // ~ 1.34 when their peaks momentarily align — inherent to the intended
    // constant-power morph (table<->table has the identical overshoot, see the ref
    // case), NOT a click. 1.40 only catches a runaway (e.g. an accidental doubling).
    const bool ok = finite && peak <= 1.40f && morphMax <= natural * 1.5f + 1.0e-4f;
    printf("  [%-26s] pre=%.4f steady=%.4f morph=%.4f  peak=%.3f finite=%s -> %s\n",
           label, static_cast<double>(preMax), static_cast<double>(steadyMax),
           static_cast<double>(morphMax), static_cast<double>(peak), finite ? "yes" : "NO",
           ok ? "CLICK-FREE" : "*** CLICK ***");
    if (!ok) allOk = false;
    return ok;
}

// Mid-morph retarget: a SECOND bank is published while the first morph is still in
// flight and its target is dominant (morphAlpha >= 0.5). This exercises the
// dominant-swap branch of beginMorphToMipData (the one the single-morph case above
// can't reach) — the promoted additive target must carry its running phase, or the
// held note clicks. Asserts the swap point has no jump above the signal's natural
// max delta.
// Returns the measured swap-point delta so the caller can baseline additive cases
// against the pure-table seam. swapCeil is the tolerated swap delta (a NEW
// discontinuity beyond it fails); pass a huge value for the baseline case itself.
static float morphCaseDouble(const char* label,
                             const std::function<void(WavetableOscillator&)>& loadA,
                             const std::function<void(WavetableOscillator&)>& loadB1,
                             const std::function<void(WavetableOscillator&)>& loadB2,
                             int sr, double f0, float swapCeil, bool& allOk)
{
    WavetableOscillator master, voice;
    master.prepare(sr, 512);  voice.prepare(sr, 512);
    master.setInterpolation(true);  voice.setInterpolation(true);
    master.setMorphTimeMs(200.0f);  voice.setMorphTimeMs(200.0f);

    loadA(master);
    voice.shareFramesFrom(master);
    voice.setFrequency(static_cast<float>(f0));

    std::vector<float> out;
    auto renderN = [&](int n) { for (int i = 0; i < n; ++i) out.push_back(voice.processSample()); };

    // Pre-roll deliberately NON-commensurate with the period: A advances alone for
    // this long before T1 starts, so at the promote instant A's (stale) phase and
    // T1's (correct) phase differ by f0*preroll cycles. sr/4 = exactly 55 cycles at
    // 220 Hz => they coincide mod 2*pi and the stale-phase bug hides; 0.263 s is a
    // generic regen offset (57.9 cycles) that exposes it, so this case discriminates
    // fix #1 (the additive phase-carry) as a real guard, not a decoration.
    const int preRoll = static_cast<int>(0.263 * sr);
    renderN(preRoll);
    const int morphSamples = static_cast<int>(0.2 * sr);
    loadB1(master); voice.morphToFramesFrom(master);   // A -> T1
    renderN(static_cast<int>(morphSamples * 0.6));      // ~120ms: morphAlpha ~0.6 (T1 dominant)
    const int swap = static_cast<int>(out.size());
    loadB2(master); voice.morphToFramesFrom(master);   // mid-morph retarget: T1 promoted, T1 -> T2
    renderN(morphSamples + sr / 4);

    auto maxDelta = [&](int a, int b) {
        float m = 0.0f;
        for (int i = std::max(1, a); i < b && i < static_cast<int>(out.size()); ++i)
            m = std::max(m, std::fabs(out[i] - out[i - 1]));
        return m;
    };
    const float preMax    = maxDelta(sr / 4, swap - 2);                      // the legit A->T1 morph
    const float steadyMax = maxDelta(static_cast<int>(out.size()) - sr / 8, static_cast<int>(out.size()));
    const float swapMax   = maxDelta(swap - 2, swap + static_cast<int>(0.005 * sr));  // around the retarget
    const float natural = std::max({ preMax, steadyMax, 0.02f });
    float peak = 0.0f; bool finite = true;
    for (float s : out) { if (!std::isfinite(s)) finite = false; peak = std::max(peak, std::fabs(s)); }

    // A mid-morph retarget of a 2-slot equal-power crossfade unavoidably drops the
    // outgoing active*cos(alpha) term when the dominant target is promoted (the
    // shipped table engine does exactly this). So the swap seam is bounded either by
    // the signal's own intra-morph slope OR by the pre-existing pure-table baseline
    // seam (swapCeil) — NOT by the tiny intra-morph delta of a smooth additive bank.
    // The click metric alone does NOT prove the promoted bank's phase is carried:
    // a wrong-phase additive signal is still smooth sample-to-sample. phaseCarryCase
    // below is the discriminator for that (a two-voice reference comparison).
    const bool ok = finite && peak <= 1.40f
                    && (swapMax <= natural * 1.5f + 1.0e-4f || swapMax <= swapCeil);
    printf("  [%-26s] pre=%.4f steady=%.4f swap=%.4f  peak=%.3f -> %s\n",
           label, static_cast<double>(preMax), static_cast<double>(steadyMax),
           static_cast<double>(swapMax), static_cast<double>(peak),
           ok ? "CLICK-FREE" : "*** CLICK ***");
    if (!ok) allOk = false;
    return swapMax;
}

// Discriminates fix #1 (the additive phase-carry on the dominant-swap) — the thing
// the click metric can't see. Two voices run in lockstep from the same note-on and
// the same A->T1 morph start:
//   REF  morphs A->T1 with a SHORT time so it COMPLETES before the compare window;
//        its T1 is carried into active by the morph-DONE branch (line ~1010) = the
//        correct running phase, and it then renders pure T1.
//   RET  morphs A->T1 with the normal time and RETARGETS to T2 while T1 is dominant,
//        so T1 is carried into active by the dominant-SWAP branch (line ~170) = the
//        path under test. Right after the retarget T2's weight is ~0, so RET is also
//        ~pure T1 -- at the SAME absolute phase as REF, because both advanced T1 from
//        the same start. If the swap carries the phase, RET ~= REF here; if it does
//        NOT (bug), RET plays T1 at a foreign phase and diverges by ~a full cycle.
static bool phaseCarryCase(const char* label,
                           const std::function<void(WavetableOscillator&)>& loadA,
                           const std::function<void(WavetableOscillator&)>& loadT1,
                           const std::function<void(WavetableOscillator&)>& loadT2,
                           int sr, double f0, bool& allOk)
{
    WavetableOscillator mRef, vRef, mRet, vRet;
    for (auto* o : { &mRef, &vRef, &mRet, &vRet }) { o->prepare(sr, 512); o->setInterpolation(true); }
    // morphTime must be set on the MASTERS: shareFramesFrom/morphToFramesFrom re-sync
    // it from the source, so a per-voice value would be clobbered. REF's short time
    // makes its A->T1 morph COMPLETE before the compare window (pure T1 reference);
    // RET's normal time leaves T1 dominant (alpha~0.6) at the retarget.
    mRef.setMorphTimeMs(100.0f);  mRet.setMorphTimeMs(200.0f);
    loadA(mRef); loadA(mRet);
    vRef.shareFramesFrom(mRef);   vRet.shareFramesFrom(mRet);
    vRef.setFrequency(static_cast<float>(f0));  vRet.setFrequency(static_cast<float>(f0));

    std::vector<float> ref, ret;
    auto step = [&]() { ref.push_back(vRef.processSample()); ret.push_back(vRet.processSample()); };

    const int preRoll   = static_cast<int>(0.263 * sr);        // generic (non-commensurate) offset
    const int morphRet  = static_cast<int>(0.2 * sr);
    for (int i = 0; i < preRoll; ++i) step();
    loadT1(mRef); vRef.morphToFramesFrom(mRef);                // both start A->T1 at the SAME sample
    loadT1(mRet); vRet.morphToFramesFrom(mRet);
    for (int i = 0; i < static_cast<int>(morphRet * 0.6); ++i) step();  // ~120ms: T1 dominant in RET, REF long done
    loadT2(mRet); vRet.morphToFramesFrom(mRet);                // RET retargets: dominant-swap promotes T1
    const int cmp0 = static_cast<int>(ret.size());
    const int win  = static_cast<int>(0.0006 * sr);            // ~0.6ms: T2 weight still ~0 in RET
    for (int i = 0; i < win; ++i) step();

    float maxDiff = 0.0f; bool finite = true;
    for (int i = cmp0; i < cmp0 + win && i < static_cast<int>(ret.size()); ++i) {
        if (!std::isfinite(ret[i]) || !std::isfinite(ref[i])) finite = false;
        maxDiff = std::max(maxDiff, std::fabs(ret[i] - ref[i]));
    }
    // WITH the carry the only gap is T2 just beginning to fade in over 0.6ms (<~0.05);
    // WITHOUT it the promoted bank is a full cycle out of phase (~1.0). 0.15 splits them.
    const bool ok = finite && maxDiff <= 0.15f;
    printf("  [%-26s] promoted-vs-reference maxDiff=%.4f -> %s\n",
           label, static_cast<double>(maxDiff), ok ? "PHASE-CARRIED" : "*** PHASE BROKEN ***");
    if (!ok) allOk = false;
    return ok;
}

// A malformed partial with h <= 0 must be DROPPED, not admitted as a DC generator:
// h == 0 never advances and never trips the Nyquist drop, so a naive clamp-to-0 would
// leave a constant a*gain*sin(phase0) term biasing every sample. Render a bank that
// pairs a bad partial with a good one and assert the mean stays ~0.
static bool dcOffsetCase(const char* label,
                         const std::vector<WavetableOscillator::AdditivePartial>& bank,
                         int sr, double f0, bool& allOk)
{
    WavetableOscillator m, v;
    m.prepare(sr, 512); v.prepare(sr, 512); m.setInterpolation(true); v.setInterpolation(true);
    m.setAdditiveBank(bank);
    v.shareFramesFrom(m);
    v.setFrequency(static_cast<float>(f0));
    double sum = 0.0; int n = sr / 2; bool finite = true;
    for (int i = 0; i < n; ++i) { float s = v.processSample(); if (!std::isfinite(s)) finite = false; sum += s; }
    const double dc = std::fabs(sum / n);
    const bool ok = finite && dc <= 0.01;
    printf("  [%-26s] |DC mean|=%.4f -> %s\n", label, dc, ok ? "NO-DC" : "*** DC OFFSET ***");
    if (!ok) allOk = false;
    return ok;
}

// argv[1] = path to a recipe JSON file, argv[2] = output WAV path: bakes the
// recipe and renders ~2s at 220Hz (A3) to argv[2]. No args: runs the demo suite below.
int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit; // safe init for any JUCE statics

    if (argc == 2 && std::string(argv[1]) == "--morphtest")
    {
        auto loadSine = [](WavetableOscillator& m) {
            Recipe r; Keyframe k; k.kind = Keyframe::Kind::Additive;
            k.partials = { Partial{ 1.0f, 1.0f, 0.0f } };
            r.keyframes = { k }; r.motion = { MotionSegment{ 0, 0.0f, Curve::Lin } };
            r.loop = true; r.frames = 64;
            m.setExactFrames(Baker::framesToBuffer(Baker::bake(r)));
        };
        auto loadBell = [](WavetableOscillator& m) {
            m.setAdditiveBank({ { 1.0f, 1.0f, 0.0f }, { 2.76f, 0.55f, 0.0f },
                                { 5.40f, 0.33f, 0.0f }, { 8.93f, 0.16f, 0.0f } });
        };
        auto loadTubular = [](WavetableOscillator& m) {
            m.setAdditiveBank({ { 1.0f, 1.0f, 0.0f }, { 2.76f, 0.60f, 0.0f }, { 5.40f, 0.45f, 0.0f },
                                { 8.93f, 0.35f, 0.0f }, { 13.34f, 0.22f, 0.0f } });
        };
        auto loadSaw = [](WavetableOscillator& m) {   // existing baked path — overshoot baseline
            Recipe r; Keyframe k; k.kind = Keyframe::Kind::Saw;
            r.keyframes = { k }; r.motion = { MotionSegment{ 0, 0.0f, Curve::Lin } };
            r.loop = true; r.frames = 64;
            m.setExactFrames(Baker::framesToBuffer(Baker::bake(r)));
        };
        auto loadMetal = [](WavetableOscillator& m) {
            m.setAdditiveBank({ { 1.0f, 1.0f, 0.0f }, { 1.414f, 0.70f, 0.0f }, { 2.828f, 0.55f, 0.0f },
                                { 4.243f, 0.40f, 0.0f }, { 5.657f, 0.30f, 0.0f } });
        };
        auto loadSquare = [](WavetableOscillator& m) {   // second baked path — pure-table retarget ref
            Recipe r; Keyframe k; k.kind = Keyframe::Kind::Square;
            r.keyframes = { k }; r.motion = { MotionSegment{ 0, 0.0f, Curve::Lin } };
            r.loop = true; r.frames = 64;
            m.setExactFrames(Baker::framesToBuffer(Baker::bake(r)));
        };
        printf("=== held-note morph click test (220 Hz, 200 ms crossfade) ===\n");
        bool allOk = true;
        morphCase("sine -> saw   (tbl->tbl ref)", loadSine, loadSaw,     44100, 220.0, allOk);
        morphCase("sine -> bell  (tbl->add)",     loadSine, loadBell,    44100, 220.0, allOk);
        morphCase("bell -> sine  (add->tbl)",     loadBell, loadSine,    44100, 220.0, allOk);
        morphCase("bell -> tubular (add->add)",   loadBell, loadTubular, 44100, 220.0, allOk);
        // Mid-morph retarget (dominant-swap branch) — the bug the single morphs miss.
        // Pure-table ref FIRST establishes the shipped 2-slot seam baseline; additive
        // retargets must be no worse than it (they are in fact smaller in absolute Δ).
        const float tblSwap = morphCaseDouble("sine>saw>square (tbl3 ref)", loadSine, loadSaw,
                                              loadSquare, 44100, 220.0, /*swapCeil*/1.0e9f, allOk);
        const float retargetCeil = std::max(0.30f, tblSwap);
        morphCaseDouble("bell>tub>metal (add3 swap)", loadBell, loadTubular, loadMetal,   44100, 220.0, retargetCeil, allOk);
        morphCaseDouble("sine>bell>tub (tbl>add2)",   loadSine, loadBell,    loadTubular, 44100, 220.0, retargetCeil, allOk);
        // Phase-carry discriminator: the click metric can't see a wrong-phase promote,
        // so assert the promoted additive bank matches a correctly-carried reference.
        phaseCarryCase("bell>tub>metal (phasecarry)", loadBell, loadTubular, loadMetal,   44100, 220.0, allOk);
        phaseCarryCase("sine>bell>tub (phasecarry)",  loadSine, loadBell,    loadTubular, 44100, 220.0, allOk);
        // Malformed-input guard: h<=0 must be dropped, not admitted as a DC term.
        dcOffsetCase("neg-h + valid (no DC)",  { { -1.0f, 1.0f, 1.5707963f }, { 1.0f, 1.0f, 0.0f } }, 44100, 220.0, allOk);
        dcOffsetCase("zero-h + valid (no DC)", { {  0.0f, 1.0f, 1.5707963f }, { 1.0f, 1.0f, 0.0f } }, 44100, 220.0, allOk);
        printf("%s\n", allOk ? "ALL CLICK-FREE" : "*** CLICK DETECTED ***");
        return allOk ? 0 : 1;
    }

    if (argc == 3)
    {
        const juce::File jsonFile(argv[1]);
        const juce::var parsed = juce::JSON::parse(jsonFile.loadFileAsString());
        const Recipe recipe = recipeFromVar(parsed);

        WavetableOscillator osc;
        osc.prepare(44100.0, 512);
        osc.setInterpolation(true);

        std::vector<WavetableOscillator::AdditivePartial> bank;
        if (tryAdditiveBank(recipe, bank))
        {
            osc.setAdditiveBank(bank);   // real-time inharmonic synthesis
            fprintf(stderr, "[bake] additive-bank path: %d inharmonic partials\n",
                    static_cast<int>(bank.size()));
        }
        else
        {
            auto frames = Baker::bake(recipe);
            auto buf = Baker::framesToBuffer(frames);
            osc.setExactFrames(buf);   // the DCO plugin load path (bit-exact, no per-frame renorm)
        }

        auto mono = renderScan(osc, 220.0, 2.0, 44100, scanLinear);
        writeWav(argv[2], mono, 44100);
        return 0;
    }

    std::filesystem::create_directories("tools/dco_audition_out");

    const int sr = 48000;
    bool allOk = true;
    std::vector<std::pair<std::string, double>> wavReport;

    struct Case { std::string name; Recipe recipe; };
    const std::vector<Case> cases = {
        { "morphloop", makeMorphloop() },
        { "pwm",       makePwm() },
        { "fmbell",    makeFmbell() },
        { "dist040",   makeDistorted(0.4f) },
        { "dist060",   makeDistorted(0.6f) },
        { "dist100",   makeDistorted(1.0f) },
    };

    for (const auto& c : cases)
    {
        printf("\n=== %s ===\n", c.name.c_str());

        checkClosure(c.name, c.recipe, allOk);

        auto frames = Baker::bake(c.recipe);
        bool shapeOk = (static_cast<int>(frames.size()) == c.recipe.frames);
        for (const auto& fr : frames)
            shapeOk = shapeOk && (static_cast<int>(fr.size()) == Baker::kFrameSize);
        printf("  [%-28s] bake shape: %d frames x %d samples -> %s\n",
               c.name.c_str(), static_cast<int>(frames.size()), Baker::kFrameSize,
               shapeOk ? "OK" : "*** FAIL ***");
        if (!shapeOk) allOk = false;

        auto buf = Baker::framesToBuffer(frames);

        WavetableOscillator osc;
        osc.prepare(static_cast<double>(sr), 512);
        osc.setExactFrames(buf);   // the DCO plugin load path (bit-exact, no per-frame renorm)
        osc.setInterpolation(true);

        // A3_scan: 8s @ 220Hz, scan swept 0->1->0 (out-and-back over the morph).
        {
            auto out = renderScan(osc, 220.0, 8.0, sr, scanOutAndBack);
            const std::string path = "tools/dco_audition_out/" + c.name + "_A3_scan.wav";
            writeWav(path, out, sr);
            double rms = 0.0;
            checkFiniteSanity(c.name + "_A3_scan", out, rms, allOk);
            wavReport.push_back({ path, rms });
        }

        // A3_loopwrap: 6s @ 220Hz, scan swept 0->1 three times (sawtooth wrap) —
        // exposes the loop seam (frame N-1 vs frame 0).
        {
            auto out = renderScan(osc, 220.0, 6.0, sr, scanWrap3);
            const std::string path = "tools/dco_audition_out/" + c.name + "_A3_loopwrap.wav";
            writeWav(path, out, sr);
            double rms = 0.0;
            checkFiniteSanity(c.name + "_A3_loopwrap", out, rms, allOk);
            checkLoopwrapClicks(c.name + "_A3_loopwrap", out, sr, allOk);
            wavReport.push_back({ path, rms });
        }

        // C6_scan: 4s @ 1046.5Hz, scan swept 0->1 (high-pitch mip/aliasing ear-check).
        {
            auto out = renderScan(osc, 1046.5, 4.0, sr, scanLinear);
            const std::string path = "tools/dco_audition_out/" + c.name + "_C6_scan.wav";
            writeWav(path, out, sr);
            double rms = 0.0;
            checkFiniteSanity(c.name + "_C6_scan", out, rms, allOk);
            wavReport.push_back({ path, rms });
        }
    }

    printf("\n=== WAV summary ===\n");
    for (const auto& entry : wavReport)
        printf("  %-50s rms=%.4f\n", entry.first.c_str(), entry.second);

    printf("\n%s\n", allOk ? "ALL PASS" : "*** FAIL ***");
    return allOk ? 0 : 1;
}
