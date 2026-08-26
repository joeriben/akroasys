// Guard tool for Phase-2 Csound orchestra swap (docs: SPEC_phase2_csound_swap.md).
//
// Drives VoiceManager + TWO CsoundEngine instances directly -- the same layer as
// tools/audition_csound_engine.cpp (Phase 1) and the other audition_* tools --
// never through the plugin's actual PluginProcessor (which needs a full JUCE
// AudioProcessor/APVTS host lifecycle impractical for an offline tool). The
// SwapRig below is a literal, faithful mirror of PluginProcessor.h/.cpp's
// Phase-2 additions: two engines + csoundActiveIdx_, the pending-swap/
// latest-wins compile state machine (csoundLifecycleMutex_,
// csoundSwapRequestGeneration_/csoundSwapStartedGeneration_,
// csoundCompileInFlight_, csoundSwapPending_/csoundSwapFading_), and the
// audio-thread fade math (per-sub-range equal-power gain lerp into
// csoundMixBufs_) -- same variable names/shapes, so a reviewer can diff this
// rig's methods against PluginProcessor.cpp's pump section line-by-line.
//
// Seven cases (spec's guard-tool section), each a hard assert -- if any fails,
// the CODE under test is wrong, not the assertion:
//   1. HELD-NOTE TAKEOVER  -- click-free AND actually-transitioning (mirrors
//                             tools/audition_sampler_follow.cpp's standard).
//   2. NO RE-STRIKE        -- no attack bump in the energy envelope across the
//                             fade, asserted BOTH on the mixed output (click
//                             class) AND directly on the unmixed new-engine
//                             solo (the sensitive signal for a prime-failure
//                             re-strike -- see caseNoRestrike()'s comment).
//   3. FADE TIME           -- transition completes within driftCrossfade (exact,
//                             by construction of the S5 sub-range clamp), tested
//                             at 200ms AND 50ms.
//   4. NEW NOTE DURING FADE-- strikes audibly, ends up in the POST-fade orchestra.
//   5. RT CLEAN            -- malloc delta == 0 across a fade under churn
//                             (post-warmup, post-prime).
//   6. LATEST-WINS         -- swap B while C compiles / during B's fade converges
//                             to C, no crash, no stuck state.
//   7. INSTANT ADOPT       -- active engine never prepared -> held note is
//                             silent, no fade ever arms, compile lands IN
//                             PLACE with a correct (portk-smoothed) attack,
//                             THEN a second request (now that the active
//                             engine is ready) takes the normal fade path.
//
// Renders WAVs (-12 dBFS) into tools/csound_swap_out/ for BJ's ear.
//
// Build (same response-file recipe as audition_csound_engine.cpp):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   grep -o 'DT5YNTH_HAS_CSOUND=.' /tmp/h.rsp   # must print exactly "=1"
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSOUND_PREFIX=$(brew --prefix csound)
//   CSOUND_FW="$CSOUND_PREFIX/Frameworks"
//   mkdir -p tools/csound_swap_out
//   clang++ -std=c++17 -O2 @/tmp/h.rsp \
//     tools/audition_csound_swap.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -F"$CSOUND_FW" -framework CsoundLib64 -Wl,-rpath,"$CSOUND_FW" \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit \
//     -o tools/csound_swap_out/audition_csound_swap
//   tools/csound_swap_out/audition_csound_swap

#include "JuceHeader.h"
#include "dsp/VoiceManager.h"
#include "dsp/CsoundEngine.h"
#include "dsp/BlockParams.h"

#include <malloc/malloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// ═══════════════════════════ shared helpers ═══════════════════════════

static void writeWav(const std::string& path, const std::vector<float>& mono, int sr)
{
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
    uint32_t dataBytes = static_cast<uint32_t>(mono.size()) * 2u;
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(sr); u32(sr * 2); u16(2); u16(16);
    f.write("data", 4); u32(dataBytes);
    for (float s : mono)
    {
        int v = static_cast<int>(std::lround(std::max(-1.f, std::min(1.f, s)) * 32767.f));
        u16(static_cast<uint16_t>(static_cast<int16_t>(v)));
    }
}

static float peakOf(const std::vector<float>& buf)
{
    float pk = 0.0f;
    for (float s : buf) pk = std::max(pk, std::fabs(s));
    return pk;
}

static void writeWavNormalized(const std::string& path, std::vector<float> buf, int sr, float targetDbfs)
{
    const float pk = peakOf(buf);
    if (pk > 1.0e-9f)
    {
        const float targetLin = std::pow(10.0f, targetDbfs / 20.0f);
        const float gain = targetLin / pk;
        for (float& s : buf) s *= gain;
    }
    writeWav(path, buf, sr);
}

static double rmsWindow(const std::vector<float>& buf, int start, int len)
{
    start = std::max(0, start);
    const int end = std::min((int) buf.size(), start + len);
    if (end <= start) return 0.0;
    double sumSq = 0.0;
    for (int i = start; i < end; ++i) sumSq += (double) buf[(size_t) i] * (double) buf[(size_t) i];
    return std::sqrt(sumSq / (double) (end - start));
}

// Max RMS over a range, scanned in overlapping sub-windows -- used as a
// reference "how loud does this orchestra's own natural sound get, at its
// loudest" ceiling. A single short window is NOT a valid reference for a
// multi-partial additive orchestra (e.g. the built-in 12-partial bell):
// closely-spaced partials beat against each other, giving the sustained tone
// a natural amplitude ripple over a timescale of hundreds of ms that has
// nothing to do with any swap/crossfade. Scanning the SAME sub-window size
// used to scan the region under test, over an equally long reference span,
// makes the "no re-strike" ceiling honest regardless of either orchestra's
// own timbral fluctuation.
static double maxRmsOverRange(const std::vector<float>& buf, int start, int len, int winLen)
{
    double m = 0.0;
    for (int pos = start; pos < start + len; pos += std::max(1, winLen / 2))
        m = std::max(m, rmsWindow(buf, pos, winLen));
    return m;
}

// Crude spectral-centroid / "band-energy ratio" proxy (spec's own wording offers
// either): RMS of the first-difference (a differentiator, emphasising high
// frequencies) divided by RMS of the raw signal. Bigger == brighter/higher
// dominant frequency content -- discriminates the built-in bell (fundamental-
// heavy) from a single-partial test orchestra multiplied well above or below
// the fundamental, without needing an FFT in this offline tool.
static double brightnessProxy(const std::vector<float>& buf, int start, int len)
{
    start = std::max(1, start);
    const int end = std::min((int) buf.size(), start + len);
    if (end <= start) return 0.0;
    double rawSumSq = 0.0, diffSumSq = 0.0;
    for (int i = start; i < end; ++i)
    {
        const double x = buf[(size_t) i];
        const double d = x - (double) buf[(size_t) (i - 1)];
        rawSumSq += x * x;
        diffSumSq += d * d;
    }
    const double rawRms = std::sqrt(rawSumSq / (double) (end - start));
    const double diffRms = std::sqrt(diffSumSq / (double) (end - start));
    return rawRms > 1.0e-9 ? diffRms / rawRms : 0.0;
}

// Max adjacent-sample delta in [start, start+len).
static double maxAdjacentDelta(const std::vector<float>& buf, int start, int len)
{
    start = std::max(1, start);
    const int end = std::min((int) buf.size(), start + len);
    double m = 0.0;
    for (int i = start; i < end; ++i)
        m = std::max(m, std::fabs((double) buf[(size_t) i] - (double) buf[(size_t) (i - 1)]));
    return m;
}

// p999-th percentile of adjacent-sample deltas in [start, start+len) -- the
// "natural" per-sample delta ceiling a click-free crossfade must stay within
// (spec case 1: "bounded by the pre-fade p99.9 delta x margin").
static double p999AdjacentDelta(const std::vector<float>& buf, int start, int len)
{
    start = std::max(1, start);
    const int end = std::min((int) buf.size(), start + len);
    if (end <= start) return 0.0;
    std::vector<double> deltas;
    deltas.reserve((size_t) (end - start));
    for (int i = start; i < end; ++i)
        deltas.push_back(std::fabs((double) buf[(size_t) i] - (double) buf[(size_t) (i - 1)]));
    std::sort(deltas.begin(), deltas.end());
    size_t idx = (size_t) (0.999 * (double) (deltas.size() - 1));
    return deltas[idx];
}

static malloc_statistics_t snapshotMalloc()
{
    malloc_statistics_t st;
    std::memset(&st, 0, sizeof(st));
    malloc_zone_statistics(malloc_default_zone(), &st);
    return st;
}

static bool waitFor(const std::function<bool()>& cond, int timeoutMs = 5000)
{
    const auto start = std::chrono::steady_clock::now();
    while (! cond())
    {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count() > timeoutMs)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ═══════════════════════════ test orchestras ═══════════════════════════
//
// Two small, contract-conforming orchestras (instr 1, p4=voice index, the
// gate/freq/vel/pres/timb/trig channel contract, outch) with clearly
// different, simple spectra from the built-in 12-partial bell AND from each
// other -- ORCH_B multiplies the fundamental by 3 (brighter), ORCH_C divides
// it by 2 (darker), both with a short strike decay so the retrigger/no-
// restrike behaviour is still exercised without pulling in the full 12-
// partial bank.
static std::string buildTestOrchestra(double sampleRate, double freqMultiplier)
{
    std::string csd = "<CsoundSynthesizer>\n<CsOptions>\n-n -d\n</CsOptions>\n<CsInstruments>\n";
    char line[256];
    std::snprintf(line, sizeof(line), "sr = %.0f\n", sampleRate); csd += line;
    std::snprintf(line, sizeof(line), "ksmps = %d\n", CsoundEngine::kKsmps); csd += line;
    std::snprintf(line, sizeof(line), "nchnls = %d\n", CsoundEngine::kMaxVoices); csd += line;
    csd += "0dbfs = 1\n\ninstr 1\n"
           "  ivoice   = p4\n"
           "  Sgate    sprintf \"gate%d\", ivoice\n"
           "  Sfreq    sprintf \"freq%d\", ivoice\n"
           "  Svel     sprintf \"vel%d\", ivoice\n"
           "  Spres    sprintf \"pres%d\", ivoice\n"
           "  Stimb    sprintf \"timb%d\", ivoice\n"
           "  Strig    sprintf \"trig%d\", ivoice\n"
           "  kgateraw chnget Sgate\n"
           "  kfreqraw chnget Sfreq\n"
           "  kvel     chnget Svel\n"
           "  kpres    chnget Spres\n"
           "  ktimb    chnget Stimb\n"
           "  ktrigch  chnget Strig\n"
           "  kgate    portk kgateraw, 0.008\n"
           "  kfreq0   portk kfreqraw, 0.008\n"
           "  kfreq    limit kfreq0, 20, 12000\n"
           "  ktrig    changed2 ktrigch\n"
           "  if ktrig == 1 then\n"
           "    reinit STRIKE\n"
           "  endif\n"
           "  STRIKE:\n"
           "  kstrike  transeg 0, 0.002, 0, 1, 0.05, -6, 0.0001\n"
           "  rireturn\n";
    std::snprintf(line, sizeof(line),
        "  asig     oscili (0.6*kstrike + 0.05)*(1+0.3*ktimb), kfreq*%.4f\n", freqMultiplier);
    csd += line;
    csd += "  aout     = asig * kgate * kvel * (1+0.1*kpres)\n"
           "  outch    ivoice, aout\n"
           "endin\n</CsInstruments>\n<CsScore>\n";
    for (int v = 1; v <= CsoundEngine::kMaxVoices; ++v)
    {
        std::snprintf(line, sizeof(line), "i 1 0 2000000000 %d\n", v);
        csd += line;
    }
    csd += "e 2000000000\n</CsScore>\n</CsoundSynthesizer>\n";
    return csd;
}

// ═══════════════════════════ SwapRig ═══════════════════════════
//
// Faithful mirror of PluginProcessor's Phase-2 additions (see this file's
// header comment). Field names deliberately match PluginProcessor.h/.cpp so
// the two can be diffed side by side.
struct ScheduledEvent
{
    int sampleOffset = 0;
    std::function<void()> action;
};

struct SwapRig
{
    static constexpr double SR = 48000.0;
    static constexpr int BS = 480;   // matches audition_csound_engine.cpp: not a ksmps(64) multiple

    VoiceManager vm;
    CsoundEngine csoundEngines_[2];
    BlockParams bp;
    juce::AudioBuffer<float> buf { 2, BS };
    std::vector<float> lfoZero;
    float tuning[128] {};

    std::atomic<int> csoundActiveIdx_ { 0 };

    // Swap request/compile state -- mirrors PluginProcessor.h exactly.
    mutable std::mutex csoundLifecycleMutex_;
    std::thread csoundCompileThread_;
    std::atomic<bool> csoundCompileInFlight_ { false };

    juce::String csoundPendingOrchestraText_;
    float csoundPendingEpochs_[CsoundEngine::kMaxVoices] {};
    float csoundPendingFreqs_[CsoundEngine::kMaxVoices] {};
    uint64_t csoundSwapRequestGeneration_ = 0;
    uint64_t csoundSwapStartedGeneration_ = 0;
    juce::String csoundCompileErrorText_;

    std::atomic<bool> csoundSwapPending_ { false };
    std::atomic<bool> csoundSwapFading_ { false };
    int csoundFadePos_ = 0;
    int csoundFadeLen_ = 1;
    std::array<std::vector<float>, CsoundEngine::kMaxVoices> csoundMixBufs_;
    // Mirrors PluginProcessor's post-review fix exactly: TRUE per-sample
    // sin/cos of the absolute fade position, hoisted above the per-voice
    // loop -- NOT a per-(voice,sample) linear interpolation of two endpoint
    // sin/cos values (which does not preserve gNew^2+gOld^2==1 in the
    // interior of a sub-range; see PluginProcessor.cpp's declaration comment
    // for the full story on this bug, caught by adversarial review).
    std::vector<float> csoundFadeGainNew_;
    std::vector<float> csoundFadeGainOld_;

    int csoundLastWritePos_ = 0;
    int absoluteBlockStart_ = 0;
    size_t evtCursor_ = 0;

    // Diagnostics only (not in PluginProcessor -- guard-tool investigation
    // aid for case 2 (NO RE-STRIKE): captures each engine's own, UNMIXED
    // voice-0 contribution during fading sub-ranges, so a mix-level energy
    // bump can be attributed to a specific engine (or ruled out as neither,
    // i.e. genuine crossfade interference between two coherent signals) --
    // set non-null before renderBlock() to activate.
    std::vector<float>* debugOldSolo_ = nullptr;
    std::vector<float>* debugNewSolo_ = nullptr;

    // Diagnostics only (not in PluginProcessor -- guard-tool coverage for the
    // adversarial-review-confirmed crossfade power bug: accumulates the
    // per-sample gNew/gOld actually used for the mix across the WHOLE fade
    // (every renderBlock call appends its sub-range's worth), so a caller can
    // verify gNew^2+gOld^2==1 (equal power) at every sample -- not just at
    // sub-range endpoints, which is exactly where the original bug hid.
    std::vector<float>* debugGainNewAccum_ = nullptr;
    std::vector<float>* debugGainOldAccum_ = nullptr;

    // Diagnostics only (not in PluginProcessor -- guard-tool value-add for
    // case 3's fade-time measurement).
    int lastFadeDurationSamples_ = -1;
    int lastFadeLenTarget_ = -1;

    // skipInitialPrepare (case 7, BLIND SPOT 2): leaves csoundEngines_[0] --
    // the engine that starts out ACTIVE (csoundActiveIdx_ == 0) -- never
    // prepared, so isReady() reads false on it from construction. Mirrors
    // "the active engine was never successfully prepared" (fresh instance,
    // prepareToPlay raced ahead of the D9 bootstrap, or a previous compile
    // failed) -- exactly the condition handleAsyncUpdate's activeReadyBefore
    // branch (PluginProcessor.cpp ~7760-7774) exists for.
    SwapRig(float driftCrossfadeMs = 200.0f, bool skipInitialPrepare = false)
        : lfoZero((size_t) BS, 0.0f)
    {
        for (int i = 0; i < 128; ++i)
            tuning[i] = (float) juce::MidiMessage::getMidiNoteInHertz(i);

        vm.prepare(SR, BS);
        vm.setTuningTable(tuning);
        vm.setEngineMode(SynthVoice::EngineMode::Csound);
        vm.setVoiceLimit(CsoundEngine::kMaxVoices);

        bp.ampAttack = 2.0f;
        bp.ampDecay = 20.0f;
        bp.ampSustain = 1.0f;
        bp.ampRelease = 300.0f;
        bp.velAmt = 1.0f;
        bp.driftCrossfade = driftCrossfadeMs;
        vm.setBlockParams(bp);

        for (auto& m : csoundMixBufs_)
            m.assign((size_t) BS, 0.0f);
        csoundFadeGainNew_.assign((size_t) BS, 0.0f);
        csoundFadeGainOld_.assign((size_t) BS, 0.0f);

        if (! skipInitialPrepare && ! csoundEngines_[0].prepare(SR, BS))
        {
            std::fprintf(stderr, "FATAL: CsoundEngine::prepare() (built-in, engine 0) failed. Aborting.\n");
            std::exit(1);
        }
    }

    ~SwapRig()
    {
        std::thread toJoin;
        {
            std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
            toJoin = std::move(csoundCompileThread_);
        }
        if (toJoin.joinable())
            toJoin.join();
    }

    void noteOn(int note, float velocity = 1.0f)
    {
        vm.noteOn(note, velocity, false, 0.0f, false, false, false, -1, 0.0f, 0);
    }
    void noteOff(int note) { vm.noteOff(note, -1); }

    void resetTimeline()
    {
        absoluteBlockStart_ = 0;
        csoundLastWritePos_ = 0;
        evtCursor_ = 0;
    }

    bool isCompiling() const { return csoundCompileInFlight_.load(std::memory_order_acquire); }
    bool isPending() const { return csoundSwapPending_.load(std::memory_order_acquire); }
    bool isFading() const { return csoundSwapFading_.load(std::memory_order_acquire); }
    int  getActiveIdx() const { return csoundActiveIdx_.load(std::memory_order_acquire); }
    juce::String getError() const
    {
        std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
        return csoundCompileErrorText_;
    }

    // Mirrors PluginProcessor::requestCsoundOrchestra -- no getCallbackLock
    // equivalent needed here (single-threaded test driver calling this
    // between renderBlock()s, never concurrently with one), but the snapshot
    // + pending-slot + generation-bump shape is identical.
    bool requestCsoundOrchestra(const std::string& orchestraText)
    {
        float epochs[CsoundEngine::kMaxVoices];
        float freqs[CsoundEngine::kMaxVoices];
        vm.snapshotCsoundState(epochs, freqs, bp.performancePitchRatio);

        {
            std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
            csoundPendingOrchestraText_ = juce::String(orchestraText);
            std::memcpy(csoundPendingEpochs_, epochs, sizeof(csoundPendingEpochs_));
            std::memcpy(csoundPendingFreqs_, freqs, sizeof(csoundPendingFreqs_));
            ++csoundSwapRequestGeneration_;
        }
        maybeStartCompile();
        return true;
    }

    // Mirrors PluginProcessor::handleAsyncUpdate's swap-launch block exactly
    // (same lock, same generation/latest-wins logic, same target-index rule
    // -- INCLUDING the Phase-5 instant-adopt fix, BLIND SPOT 2: targetIdx/
    // activeReadyBefore, prime-only-when-ready-before, arm-pending-only-when-
    // ready-before. Brought up to the production shape (~PluginProcessor.cpp
    // 7760-7822) from an earlier draft that hardcoded inactiveIdx/always
    // primed/always armed pending -- that earlier shape never exercised the
    // instant-adopt path at all, see case 7).
    void maybeStartCompile()
    {
        std::lock_guard<std::mutex> csoundLock(csoundLifecycleMutex_);

        const bool wantsNewSwapCompile = csoundSwapRequestGeneration_ != csoundSwapStartedGeneration_;
        const bool freeToStartSwap = ! csoundCompileInFlight_.load(std::memory_order_acquire)
                                   && ! csoundSwapPending_.load(std::memory_order_acquire)
                                   && ! csoundSwapFading_.load(std::memory_order_acquire);
        if (! (wantsNewSwapCompile && freeToStartSwap))
            return;

        if (csoundCompileThread_.joinable())
            csoundCompileThread_.join();

        const uint64_t myGeneration = csoundSwapRequestGeneration_;
        csoundSwapStartedGeneration_ = myGeneration;
        // The engine that is INACTIVE right now -- stable for the whole
        // compile: csoundActiveIdx_ only ever flips at a fade's END, and
        // freeToStartSwap above already guarantees no fade is running.
        const int inactiveIdx = 1 - csoundActiveIdx_.load(std::memory_order_relaxed);
        const int currentActiveIdx = csoundActiveIdx_.load(std::memory_order_relaxed);
        // Phase 5 fix (SPEC_phase4_5_csound_llm_preset.md: "no ready active
        // engine -> instant adopt without fade"): if the engine that's
        // SUPPOSED to be live right now was never successfully prepared
        // (fresh instance, or a previous compile failed -- case 7's whole
        // point), there is nothing audible to fade FROM. Compile the new
        // orchestra straight into the already-active slot instead of the
        // inactive one, so it becomes ready in place -- the render loop's
        // existing isReady()-gated behaviour picks it up on the very next
        // block with no swap/fade machinery involved at all.
        const bool activeReadyBefore = csoundEngines_[currentActiveIdx].isReady();
        const int targetIdx = activeReadyBefore ? inactiveIdx : currentActiveIdx;
        const juce::String textCopy = csoundPendingOrchestraText_;
        std::array<float, CsoundEngine::kMaxVoices> epochsCopy, freqsCopy;
        std::memcpy(epochsCopy.data(), csoundPendingEpochs_, sizeof(csoundPendingEpochs_));
        std::memcpy(freqsCopy.data(), csoundPendingFreqs_, sizeof(csoundPendingFreqs_));

        csoundCompileInFlight_.store(true, std::memory_order_release);
        csoundCompileThread_ = std::thread([this, targetIdx, activeReadyBefore,
                                             textCopy, epochsCopy, freqsCopy, myGeneration]
        {
            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
                ok = csoundEngines_[targetIdx].prepare(SR, BS,
                        textCopy.isEmpty() ? nullptr : textCopy.toRawUTF8());
                // primeForTakeover seeds the NEW engine's voice phase/freq
                // state from the OLD (still-active) engine so a held note's
                // crossfade doesn't re-strike -- only meaningful when there
                // IS an old active engine to hand off from (the fade case).
                // The instant-adopt case has no prior audible engine, so
                // there is nothing to prime from and no fade to prepare for.
                if (ok && activeReadyBefore)
                    csoundEngines_[targetIdx].primeForTakeover(epochsCopy.data(), freqsCopy.data());
            }

            bool stillCurrent = false;
            {
                std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
                stillCurrent = (myGeneration == csoundSwapRequestGeneration_);
                if (stillCurrent)
                    csoundCompileErrorText_ = ok ? juce::String()
                        : juce::String("Csound orchestra compile failed");
            }

            // Only the genuine fade case (an already-ready engine gets
            // replaced) arms the crossfade machinery. The instant-adopt case
            // compiled directly into csoundActiveIdx_, so it is already
            // live -- arming csoundSwapPending_ here would tell the render
            // loop to fade INTO the same index it's already playing, which
            // is a no-op at best and a self-referential fade at worst.
            if (ok && stillCurrent && activeReadyBefore)
                csoundSwapPending_.store(true, std::memory_order_release);

            csoundCompileInFlight_.store(false, std::memory_order_release);
            // NOTE: deliberately does NOT call maybeStartCompile() here. In
            // production (PluginProcessor.cpp) this same point calls
            // triggerAsyncUpdate(), which hops to the JUCE message thread --
            // a DIFFERENT OS thread -- before any queued/superseding request
            // is serviced, so csoundCompileThread_.join() (inside a later
            // maybeStartCompile() call) never targets its own running
            // thread. This test rig has no message thread to hop to, so
            // calling maybeStartCompile() directly from THIS thread would
            // attempt to join itself (a real deadlock -- libc++ throws
            // "Resource deadlock avoided" -- caught while writing this
            // guard). Callers external to this thread (the render loop's
            // fade-completion branch, and the test driver's poll loops)
            // call maybeStartCompile() instead, mirroring the real
            // trigger sites (processBlock's post-fade triggerAsyncUpdate(),
            // and handleAsyncUpdate() itself).
        });
    }

    // Mirrors PluginProcessor.cpp's processBlock pump section exactly (S5/S6/S7).
    void renderBlock(int numSamples, const std::vector<ScheduledEvent>& events, std::vector<float>* outMono)
    {
        buf.clear();

        int csoundActiveIdxNow = csoundActiveIdx_.load(std::memory_order_acquire);

        if (csoundSwapPending_.exchange(false, std::memory_order_acq_rel))
        {
            csoundFadePos_ = 0;
            csoundFadeLen_ = juce::jmax(1, juce::roundToInt(bp.driftCrossfade * 0.001f * (float) SR));
            csoundSwapFading_.store(true, std::memory_order_release);
            lastFadeLenTarget_ = csoundFadeLen_;
        }

        bool csoundFadingNow = csoundSwapFading_.load(std::memory_order_acquire);
        const int csoundOtherIdx = 1 - csoundActiveIdxNow;

        csoundEngines_[csoundActiveIdxNow].startBlock(numSamples);
        if (csoundFadingNow)
            csoundEngines_[csoundOtherIdx].startBlock(numSamples);

        int renderPos = 0;
        while (renderPos < numSamples)
        {
            int subEnd = numSamples;
            if (evtCursor_ < events.size())
            {
                const int rel = events[evtCursor_].sampleOffset - absoluteBlockStart_;
                subEnd = juce::jlimit(renderPos, numSamples, rel);
            }
            if (csoundFadingNow)
                subEnd = juce::jmin(subEnd, renderPos + (csoundFadeLen_ - csoundFadePos_));

            const int subLen = subEnd - renderPos;
            if (subLen > 0)
            {
                CsoundEngine* pumpEngines[2] = { &csoundEngines_[csoundActiveIdxNow], nullptr };
                int numPumpEngines = 1;
                if (csoundFadingNow)
                {
                    pumpEngines[1] = &csoundEngines_[csoundOtherIdx];
                    numPumpEngines = 2;
                }
                vm.writeCsoundControls(pumpEngines, numPumpEngines, bp.performancePitchRatio,
                                       renderPos - csoundLastWritePos_);
                for (int e = 0; e < numPumpEngines; ++e)
                    pumpEngines[e]->renderUpTo(subEnd - 1);
                csoundLastWritePos_ = renderPos;

                const float* csoundVoiceBufs[CsoundEngine::kMaxVoices];
                if (csoundFadingNow)
                {
                    // TRUE per-sample sin/cos of the absolute fade position,
                    // hoisted above the per-voice loop -- mirrors
                    // PluginProcessor.cpp's post-review fix (see that file's
                    // declaration comment for csoundFadeGainNew_/Old_: a
                    // per-(voice,sample) LINEAR interpolation of two endpoint
                    // sin/cos values does NOT preserve gNew^2+gOld^2==1 in the
                    // interior of a sub-range).
                    for (int s = 0; s < subLen; ++s)
                    {
                        const float t = (float) (csoundFadePos_ + s) / (float) csoundFadeLen_;
                        csoundFadeGainNew_[static_cast<size_t>(renderPos + s)] =
                            std::sin(t * juce::MathConstants<float>::halfPi);
                        csoundFadeGainOld_[static_cast<size_t>(renderPos + s)] =
                            std::cos(t * juce::MathConstants<float>::halfPi);
                        if (debugGainNewAccum_ != nullptr)
                            debugGainNewAccum_->push_back(csoundFadeGainNew_[static_cast<size_t>(renderPos + s)]);
                        if (debugGainOldAccum_ != nullptr)
                            debugGainOldAccum_->push_back(csoundFadeGainOld_[static_cast<size_t>(renderPos + s)]);
                    }

                    CsoundEngine& oldEngine = csoundEngines_[csoundActiveIdxNow];
                    CsoundEngine& newEngine = csoundEngines_[csoundOtherIdx];
                    for (int vi = 0; vi < CsoundEngine::kMaxVoices; ++vi)
                    {
                        const float* oldBuf = oldEngine.voiceBuffer(vi);
                        const float* newBuf = newEngine.voiceBuffer(vi);
                        float* mix = csoundMixBufs_[static_cast<size_t>(vi)].data();
                        for (int s = 0; s < subLen; ++s)
                        {
                            const float gNew = csoundFadeGainNew_[static_cast<size_t>(renderPos + s)];
                            const float gOld = csoundFadeGainOld_[static_cast<size_t>(renderPos + s)];
                            const float o = oldBuf != nullptr ? oldBuf[renderPos + s] : 0.0f;
                            const float n = newBuf != nullptr ? newBuf[renderPos + s] : 0.0f;
                            mix[renderPos + s] = gOld * o + gNew * n;
                            if (vi == 0)
                            {
                                if (debugOldSolo_ != nullptr) debugOldSolo_->push_back(o);
                                if (debugNewSolo_ != nullptr) debugNewSolo_->push_back(n);
                            }
                        }
                        csoundVoiceBufs[vi] = mix;
                    }
                }
                else
                {
                    for (int vi = 0; vi < CsoundEngine::kMaxVoices; ++vi)
                        csoundVoiceBufs[vi] = csoundEngines_[csoundActiveIdxNow].voiceBuffer(vi);
                }

                vm.renderBlock(buf, bp, lfoZero.data() + renderPos, lfoZero.data() + renderPos,
                               lfoZero.data() + renderPos, renderPos, subLen, csoundVoiceBufs);

                if (csoundFadingNow)
                {
                    csoundFadePos_ += subLen;
                    if (csoundFadePos_ >= csoundFadeLen_)
                    {
                        csoundActiveIdx_.store(csoundOtherIdx, std::memory_order_release);
                        csoundSwapFading_.store(false, std::memory_order_release);
                        lastFadeDurationSamples_ = csoundFadePos_;
                        csoundActiveIdxNow = csoundOtherIdx;
                        csoundFadingNow = false;
                        maybeStartCompile();   // let a queued request start
                    }
                }
            }

            while (evtCursor_ < events.size()
                   && events[evtCursor_].sampleOffset - absoluteBlockStart_ <= subEnd)
            {
                events[evtCursor_].action();
                ++evtCursor_;
            }
            renderPos = subEnd;
        }
        csoundLastWritePos_ -= numSamples;
        absoluteBlockStart_ += numSamples;

        if (outMono != nullptr)
            for (int i = 0; i < numSamples; ++i)
                outMono->push_back(0.5f * (buf.getSample(0, i) + buf.getSample(1, i)));
    }

    std::vector<float> renderRange(int totalSamples, std::vector<ScheduledEvent> events)
    {
        std::sort(events.begin(), events.end(),
                  [](const ScheduledEvent& a, const ScheduledEvent& b) { return a.sampleOffset < b.sampleOffset; });
        resetTimeline();
        std::vector<float> out;
        out.reserve((size_t) totalSamples + (size_t) BS);
        int done = 0;
        while (done < totalSamples)
        {
            renderBlock(BS, events, &out);
            done += BS;
        }
        out.resize((size_t) totalSamples);
        return out;
    }
};

// ═══════════════════════════ shared controlled-swap render helper ═══════════════════════════
//
// A naive test would render N seconds "all at once" and assume the fade
// starts at the sample position where requestCsoundOrchestra() was issued.
// That assumption is WRONG: the compile that must complete before the fade
// can arm runs on a background thread and takes real wall-clock time, while
// this offline render loop produces audio far faster than real time -- so a
// batch-rendered "request at sample X" schedule can easily overshoot (or, in
// the pathological case, undershoot) the moment isPending() actually flips.
// (First draft of this file hit exactly this: case 1's fade never actually
// got measured in the right place, producing bogus near-zero brightness
// deltas.)
//
// A SECOND, subtler bug in the first draft: isPending() is consumed
// (exchange(false, ...)) at the very TOP of the very next renderBlock()
// call. Polling `{ renderBlock(); return isPending(); }` therefore checks
// the flag AFTER the very call that would have just consumed it -- it can
// only ever observe true in the tiny race window where the background
// compile finishes WHILE that renderBlock() call is executing. That made
// case 1 pass by luck and case 2 fail by bad luck on the same code, with no
// underlying difference between them (caught by running both and seeing
// case 2's pre/post windows land on top of each other, unresolved fadeLen
// -1). Fixed the same way case 5 (RT CLEAN) already did it correctly: poll
// isPending() on its own, WITHOUT advancing the render loop, so nothing
// consumes the flag until the wait succeeds -- then the FIRST renderBlock()
// call after that is the one that legitimately arms the fade, and
// fadeStartSample (captured as out.size() right before that call) is exact.
struct SwapMeasurement
{
    std::vector<float> out;
    int fadeStartSample = -1;
    int fadeLen = -1;
    bool becamePending = false;
};

static SwapMeasurement renderControlledSwap(SwapRig& rig, int preRollSamples,
                                             const std::string& orchestraText,
                                             int postFadeTailSamples,
                                             float midFadeActionFraction = -1.0f,
                                             std::function<void()> midFadeAction = nullptr,
                                             int* midFadeActionSamplePos = nullptr)
{
    SwapMeasurement m;
    rig.resetTimeline();

    for (int done = 0; done < preRollSamples; done += SwapRig::BS)
        rig.renderBlock(SwapRig::BS, {}, &m.out);

    rig.requestCsoundOrchestra(orchestraText);

    // Poll only -- do NOT render here (see comment above: rendering while
    // polling races the flag's own one-shot consumption).
    m.becamePending = waitFor([&] { return rig.isPending(); }, 5000);

    m.fadeStartSample = (int) m.out.size();

    bool midFadeActionDone = (midFadeAction == nullptr);
    int guard = 0;
    while (! (rig.getActiveIdx() == 1 && ! rig.isFading()) && guard < 4000)
    {
        rig.renderBlock(SwapRig::BS, {}, &m.out);
        ++guard;
        if (! midFadeActionDone && rig.isFading())
        {
            const int elapsed = (int) m.out.size() - m.fadeStartSample;
            if (rig.csoundFadeLen_ > 0
                && (float) elapsed >= midFadeActionFraction * (float) rig.csoundFadeLen_)
            {
                if (midFadeActionSamplePos != nullptr)
                    *midFadeActionSamplePos = (int) m.out.size();
                midFadeAction();
                midFadeActionDone = true;
            }
        }
    }
    m.fadeLen = rig.lastFadeLenTarget_;

    for (int done = 0; done < postFadeTailSamples; done += SwapRig::BS)
        rig.renderBlock(SwapRig::BS, {}, &m.out);

    return m;
}

// ═══════════════════════════ Case 1: HELD-NOTE TAKEOVER ═══════════════════════════
static bool caseHeldNoteTakeover()
{
    printf("[1] HELD-NOTE TAKEOVER (click-free AND actually-transitioning)\n");
    SwapRig rig(200.0f);
    const std::string orchB = buildTestOrchestra(SwapRig::SR, 3.0);   // brighter (3rd harmonic)

    rig.noteOn(60);
    auto meas = renderControlledSwap(rig, (int) (0.3 * SwapRig::SR), orchB, (int) (0.3 * SwapRig::SR));
    auto& out2 = meas.out;
    const int fadeStartSample = meas.fadeStartSample;
    const int fadeLen = meas.fadeLen;

    // Click-free: max adjacent delta across the whole fade window vs. the
    // pre-fade p99.9 delta.
    const double preFadeP999 = p999AdjacentDelta(out2, fadeStartSample - 4000, 4000);
    const double fadeWindowMax = maxAdjacentDelta(out2, fadeStartSample, fadeLen + 200);
    const double margin = 3.0;
    const bool clickFree = fadeWindowMax <= preFadeP999 * margin + 1.0e-6;

    // Actually-transitioning: brightness clearly differs pre vs post, moves
    // consistently through the fade.
    const double preBrightness  = brightnessProxy(out2, fadeStartSample - 4000, 4000);
    const double postBrightness = brightnessProxy(out2, fadeStartSample + fadeLen + 2000, 4000);
    const bool clearlyDifferent = (postBrightness > preBrightness * 1.3)
                                || (postBrightness < preBrightness * 0.7);
    const bool risingTrend = postBrightness > preBrightness;

    bool monotonicIsh = true;
    const int steps = 5;
    double prevDist = std::fabs(preBrightness - (risingTrend ? postBrightness : preBrightness));
    double lastVal = preBrightness;
    for (int i = 1; i <= steps; ++i)
    {
        const int pos = fadeStartSample + (fadeLen * i) / steps;
        const double v = brightnessProxy(out2, pos - 500, 1000);
        const double distToPost = std::fabs(v - postBrightness);
        // Allow generous slack (natural signal variance / windowing) -- only
        // fail if the trend clearly reverses (moves markedly AWAY from post).
        if (distToPost > prevDist + std::fabs(postBrightness - preBrightness) * 0.5 + 1.0e-9)
            monotonicIsh = false;
        prevDist = distToPost;
        lastVal = v;
    }
    (void) lastVal;

    const bool ok = clickFree && clearlyDifferent && monotonicIsh && meas.becamePending;
    printf("    becamePending=%s fadeStartSample=%d fadeLen=%d preP999=%.6f fadeWindowMax=%.6f (margin=%.1fx) -> clickFree=%s\n",
           meas.becamePending ? "yes" : "NO", fadeStartSample, fadeLen, preFadeP999, fadeWindowMax, margin,
           clickFree ? "yes" : "NO");
    printf("    preBrightness=%.4f postBrightness=%.4f clearlyDifferent=%s monotonicIsh=%s -> %s\n",
           preBrightness, postBrightness, clearlyDifferent ? "yes" : "NO", monotonicIsh ? "yes" : "NO",
           ok ? "PASS" : "*** FAIL ***");

    writeWavNormalized("tools/csound_swap_out/case1_held_note_takeover.wav", out2, (int) SwapRig::SR, -12.0f);
    return ok;
}

// ═══════════════════════════ Case 2: NO RE-STRIKE ═══════════════════════════
static bool caseNoRestrike()
{
    printf("[2] NO RE-STRIKE (energy envelope has no attack bump across the fade)\n");
    SwapRig rig(200.0f);
    const std::string orchB = buildTestOrchestra(SwapRig::SR, 3.0);

    rig.noteOn(60);
    std::vector<float> oldSolo, newSolo;
    rig.debugOldSolo_ = &oldSolo;
    rig.debugNewSolo_ = &newSolo;
    auto meas = renderControlledSwap(rig, (int) (0.3 * SwapRig::SR), orchB, (int) (0.3 * SwapRig::SR));
    rig.debugOldSolo_ = nullptr;
    rig.debugNewSolo_ = nullptr;
    auto& out = meas.out;
    const int fadeStart = meas.fadeStartSample;
    const int fadeLen = meas.fadeLen;

    // Attribute any mix-level bump to a specific engine (or rule both out,
    // pointing at crossfade interference between two coherent signals -- see
    // renderControlledSwap's header comment on this file's investigation
    // history). Computed unconditionally (not diagnostic-only, see the
    // BLIND SPOT 1 assertion below, which loads on newMax/newSettled).
    const int diagWin = (int) (0.005 * SwapRig::SR);
    double oldMax = 0.0, newMax = 0.0;
    double oldMaxFrac = -1.0, newMaxFrac = -1.0;
    for (int pos = 0; pos + diagWin < (int) oldSolo.size(); pos += diagWin / 2)
    {
        const double r = rmsWindow(oldSolo, pos, diagWin);
        if (r > oldMax) { oldMax = r; oldMaxFrac = (double) pos / (double) fadeLen; }
    }
    for (int pos = 0; pos + diagWin < (int) newSolo.size(); pos += diagWin / 2)
    {
        const double r = rmsWindow(newSolo, pos, diagWin);
        if (r > newMax) { newMax = r; newMaxFrac = (double) pos / (double) fadeLen; }
    }
    const double oldSettled = rmsWindow(oldSolo, 0, diagWin);
    const double newSettled = newSolo.size() > (size_t) diagWin
        ? rmsWindow(newSolo, (int) newSolo.size() - diagWin, diagWin) : 0.0;
    printf("    [diag] oldEngine solo: max=%.4f@%.0f%% settled(t=0)=%.4f | newEngine solo: max=%.4f@%.0f%% settled(t=end)=%.4f\n",
           oldMax, oldMaxFrac * 100.0, oldSettled, newMax, newMaxFrac * 100.0, newSettled);

    const int winLen = (int) (0.005 * SwapRig::SR);   // 5ms windows, scanned everywhere below

    // Reference ceiling: the LOUDEST this held note's own sound gets, at
    // steady state, over a span AS LONG AS THE FADE ITSELF -- not a single
    // short window (see maxRmsOverRange's comment: a multi-partial additive
    // orchestra beats/ripples in amplitude over a timescale of hundreds of
    // ms, purely from its own harmonic structure, independent of any swap).
    const double preRms = maxRmsOverRange(out, fadeStart - fadeLen, fadeLen, winLen);
    const double postRms = maxRmsOverRange(out, fadeStart + fadeLen, fadeLen, winLen);

    double maxFadeRms = 0.0;
    double maxFadeRmsFrac = -1.0;
    for (int pos = fadeStart; pos < fadeStart + fadeLen; pos += winLen / 2)
    {
        const double r = rmsWindow(out, pos, winLen);
        if (r > maxFadeRms) { maxFadeRms = r; maxFadeRmsFrac = (double) (pos - fadeStart) / (double) fadeLen; }
    }

    const double ceiling = std::max(preRms, postRms) * 1.25;

    // BLIND SPOT 1 fix (adversarial-review finding, confirmed by ablation --
    // see the ablation numbers below): the MIXED-output assertion above
    // (maxFadeRms <= ceiling) is VACUOUS for a prime-failure re-strike --
    // equal-power gNew = sin(t*pi/2) is ~0 at fade start, so a spurious
    // attack transient on the NEW engine gets multiplied down to near-
    // silence in the mix before it can ever bump maxFadeRms. The sensitive
    // signal is the new engine's own, UNMIXED solo (debugNewSolo_, filled at
    // renderBlock()'s vi==0 capture, ~line 586-587 of this file): a failed
    // primeForTakeover() leaves the new engine's trig channel at whatever
    // prepare()'s warmup left it (0) until the fade's first real control
    // write flips it to the snapshot epoch -- changed2() fires for real, a
    // genuine attack transient (the test orchestra's transeg 0->1 over 2ms)
    // well above the held note's settled, decaying level. A WORKING prime
    // has already fired that strike silently (gate closed) before this
    // engine was ever published as a swap target, so a correctly-primed new
    // engine's solo only DECAYS across the fade -- it never re-attacks.
    // newSettled (rmsWindow at the END of the captured fade window, where
    // the new engine is closest to its steady, fully-faded-in level) stands
    // in for "its settled post-fade windowed RMS" -- debugNewSolo_ is only
    // filled while csoundFadingNow (never after), so there is no literal
    // post-fade sample of the solo signal to measure directly.
    //
    // Ablation (primeForTakeover's body temporarily made an early-return
    // no-op in src/dsp/CsoundEngine.cpp, rebuilt, re-run): newMax=0.1422 vs
    // ceiling(newSettled*1.25)=0.0514 -> FAIL (2.8x over) -- meanwhile the
    // PRE-EXISTING mixed-output assertion alone, on the SAME broken run,
    // still measured maxFadeRms=0.1107 <= ceiling=0.1770 -> would have
    // PASSED, confirming it really is vacuous for this failure mode.
    // primeForTakeover restored verbatim, rebuilt, re-ran: newMax=0.0411 vs
    // ceiling=0.0505 -> PASS (matches the pre-ablation run exactly).
    const double newSoloCeiling = newSettled * 1.25;
    const bool newSoloOk = ! newSolo.empty() && newMax <= newSoloCeiling + 1.0e-6;

    const bool ok = meas.becamePending && preRms > 1.0e-6 && postRms > 1.0e-6
                  && maxFadeRms <= ceiling + 1.0e-6 && newSoloOk;

    printf("    [new-engine-solo assert] newMax=%.4f ceiling(newSettled*1.25)=%.4f -> %s\n",
           newMax, newSoloCeiling, newSoloOk ? "PASS" : "*** FAIL ***");
    printf("    becamePending=%s preRms=%.4f postRms=%.4f maxFadeRms=%.4f @%.0f%%-into-fade ceiling(1.25x max)=%.4f -> %s\n",
           meas.becamePending ? "yes" : "NO", preRms, postRms, maxFadeRms, maxFadeRmsFrac * 100.0, ceiling,
           ok ? "PASS" : "*** FAIL ***");

    writeWavNormalized("tools/csound_swap_out/case2_no_restrike.wav", out, (int) SwapRig::SR, -12.0f);
    return ok;
}

// ═══════════════════════════ Case 3: FADE TIME ═══════════════════════════
static bool caseFadeTime()
{
    printf("[3] FADE TIME (transition completes within driftCrossfade, tested at 200ms, 50ms AND 5ms)\n");
    const std::string orchB = buildTestOrchestra(SwapRig::SR, 3.0);
    bool ok = true;

    // 5ms is included specifically to reproduce the exact condition that
    // triggered the crossfade power bug caught by adversarial review: at
    // 5ms, fadeLen = 240 samples < BS = 480, so the ENTIRE fade fits in ONE
    // sub-range (no MIDI event splits it) -- exactly the "short Regen XFade
    // time" / "large offline-bounce block" scenario where a per-(voice,
    // sample) linear interpolation of two endpoint sin/cos values (the
    // original, buggy code) would have shown a real ~-3dB power dip at the
    // segment midpoint. 200ms/50ms are the spec-mandated durations.
    for (float crossfadeMs : { 200.0f, 50.0f, 5.0f })
    {
        SwapRig rig(crossfadeMs);
        rig.noteOn(60);
        std::vector<float> gainNewAccum, gainOldAccum;
        rig.debugGainNewAccum_ = &gainNewAccum;
        rig.debugGainOldAccum_ = &gainOldAccum;
        auto meas = renderControlledSwap(rig, (int) (0.05 * SwapRig::SR), orchB, 0);
        rig.debugGainNewAccum_ = nullptr;
        rig.debugGainOldAccum_ = nullptr;

        const int expectedSamples = juce::roundToInt(crossfadeMs * 0.001f * (float) SwapRig::SR);
        // S5's sub-range clamp makes this EXACT by construction (the fade never
        // overshoots its target length) -- assert exact equality as the primary
        // (strongest) check, well inside the spec's own "+/- one block" allowance.
        const bool exact = rig.lastFadeDurationSamples_ == expectedSamples;
        const bool withinBlock = std::abs(rig.lastFadeDurationSamples_ - expectedSamples) <= SwapRig::BS;

        // Equal-power check (adversarial-review finding coverage): at EVERY
        // accumulated sample across the whole fade, gNew^2+gOld^2 must be 1
        // to within float precision -- not just at sub-range endpoints,
        // which is exactly where the original linear-interpolation bug hid.
        double maxPowerErr = 0.0;
        for (size_t i = 0; i < gainNewAccum.size() && i < gainOldAccum.size(); ++i)
        {
            const double p = (double) gainNewAccum[i] * gainNewAccum[i]
                            + (double) gainOldAccum[i] * gainOldAccum[i];
            maxPowerErr = std::max(maxPowerErr, std::fabs(p - 1.0));
        }
        const bool equalPower = ! gainNewAccum.empty() && maxPowerErr < 1.0e-4;

        const bool caseOk = meas.becamePending && exact && withinBlock && equalPower;
        ok &= caseOk;

        printf("    crossfade=%.0fms expected=%d actual=%d (exact=%s, within+/-1block=%s) gainSamples=%zu maxPowerErr=%.2e -> %s\n",
               crossfadeMs, expectedSamples, rig.lastFadeDurationSamples_,
               exact ? "yes" : "NO", withinBlock ? "yes" : "NO", gainNewAccum.size(), maxPowerErr,
               caseOk ? "PASS" : "*** FAIL ***");
    }
    return ok;
}

// ═══════════════════════════ Case 4: NEW NOTE DURING FADE ═══════════════════════════
static bool caseNewNoteDuringFade()
{
    printf("[4] NEW NOTE DURING FADE (strikes audibly, sounds in POST-fade orchestra)\n");
    SwapRig rig(200.0f);
    const std::string orchB = buildTestOrchestra(SwapRig::SR, 3.0);

    rig.noteOn(60);
    int newNoteAt = -1;
    auto meas = renderControlledSwap(rig, (int) (0.2 * SwapRig::SR), orchB, (int) (0.3 * SwapRig::SR),
                                      0.25f, [&] { rig.noteOn(67); }, &newNoteAt);   // voice 1, fresh pitch, ~25% into the fade
    auto& out = meas.out;
    const int fadeStartSample = meas.fadeStartSample;
    const int fadeLen = meas.fadeLen;

    // Onset latency: pre-onset window (just before newNoteAt) must be at the
    // MIXED-fade level (not silent -- voice 0 is already sounding), so measure
    // the STEP in energy right after newNoteAt instead of absolute silence.
    const double preNewNoteRms = rmsWindow(out, newNoteAt - 300, 300);
    const int onsetWindow = 600;   // Phase-1 latency bound (audition_csound_engine.cpp case 1)
    double onsetIdx = -1;
    const double thresh = preNewNoteRms + 0.02;   // a clear step above whatever voice 0 alone was doing
    for (int i = newNoteAt; i < newNoteAt + onsetWindow + 200 && i < (int) out.size(); ++i)
    {
        if (rmsWindow(out, i, 64) > thresh) { onsetIdx = i; break; }
    }
    const bool strikesAudibly = newNoteAt >= 0 && onsetIdx >= 0 && (onsetIdx - newNoteAt) < onsetWindow;

    // Post-fade orchestra check: sample well after the fade completes, with
    // ONLY voice 67 sounding in isolation is hard (voice 60 also present) --
    // instead confirm the OVERALL post-fade brightness matches orchB's
    // signature (much brighter than the built-in bell, per case 1's measured
    // direction), proving BOTH voices ended up in the new orchestra.
    const double preRequestBrightness = brightnessProxy(out, fadeStartSample - 4000, 4000);
    const double postFadeBrightness = brightnessProxy(out, fadeStartSample + fadeLen + 2000, 4000);
    const bool inPostOrchestra = postFadeBrightness > preRequestBrightness * 1.3;

    const bool ok = meas.becamePending && strikesAudibly && inPostOrchestra;
    printf("    becamePending=%s newNoteAt=%d onsetIdx=%.0f (delay=%.0f samples, bound=%d) strikesAudibly=%s\n",
           meas.becamePending ? "yes" : "NO", newNoteAt, onsetIdx,
           onsetIdx >= 0 ? onsetIdx - newNoteAt : -1.0, onsetWindow, strikesAudibly ? "yes" : "NO");
    printf("    preRequestBrightness=%.4f postFadeBrightness=%.4f inPostOrchestra=%s -> %s\n",
           preRequestBrightness, postFadeBrightness, inPostOrchestra ? "yes" : "NO",
           ok ? "PASS" : "*** FAIL ***");

    writeWavNormalized("tools/csound_swap_out/case4_new_note_during_fade.wav", out, (int) SwapRig::SR, -12.0f);
    return ok;
}

// ═══════════════════════════ Case 5: RT CLEAN ═══════════════════════════
static bool caseRtClean()
{
    printf("[5] RT CLEAN (malloc delta == 0 across a fade under churn, post-warmup/post-prime)\n");
    SwapRig rig(200.0f);
    const std::string orchB = buildTestOrchestra(SwapRig::SR, 3.0);

    // Warm up voices/harness allocations (mirrors audition_csound_engine.cpp case 4).
    {
        std::vector<ScheduledEvent> warmupOn;
        for (int i = 0; i < 4; ++i)
            warmupOn.push_back({ i * 2000, [&rig, i] { rig.noteOn(60 + i); } });
        rig.resetTimeline();
        const int total = 3 * SwapRig::BS + 8192;
        for (int done = 0; done < total; done += SwapRig::BS)
            rig.renderBlock(SwapRig::BS, warmupOn, nullptr);
    }
    {
        std::vector<ScheduledEvent> warmupOff;
        for (int i = 0; i < 4; ++i)
            warmupOff.push_back({ i * 100, [&rig, i] { rig.noteOff(60 + i); } });
        rig.resetTimeline();
        const int total = 2 * SwapRig::BS + 8192;
        for (int done = 0; done < total; done += SwapRig::BS)
            rig.renderBlock(SwapRig::BS, warmupOff, nullptr);
    }

    // Compile + prime the swap BEFORE the malloc snapshot -- the fade itself
    // (audio-thread-equivalent path) must be allocation-free; the compile
    // (message/background-thread-equivalent path) is explicitly NOT part of
    // the RT-clean contract (S8: it runs csoundCompileCsdText/warmup, which
    // legitimately allocates off the audio thread).
    rig.noteOn(60);
    rig.resetTimeline();
    for (int done = 0; done < 4000; done += SwapRig::BS)
        rig.renderBlock(SwapRig::BS, {}, nullptr);
    rig.requestCsoundOrchestra(orchB);
    const bool primed = waitFor([&] { return rig.isPending(); }, 5000);

    static int churnNote = 61;
    const int churnPeriod = (int) (0.08 * SwapRig::SR);
    std::vector<ScheduledEvent> churnEvents;
    const int fadeLen = rig.lastFadeLenTarget_ > 0 ? rig.lastFadeLenTarget_
                                                    : juce::roundToInt(0.2f * (float) SwapRig::SR);
    const int churnTotal = fadeLen + (int) (0.3 * SwapRig::SR);   // covers the whole fade + margin
    const int numChurnEvents = churnTotal / churnPeriod + 2;
    for (int i = 0; i < numChurnEvents; ++i)
        churnEvents.push_back({ i * churnPeriod, [&]
        {
            rig.noteOff(churnNote);
            churnNote = 61 + (churnNote - 61 + 3) % 8;
            rig.noteOn(churnNote);
        } });
    rig.resetTimeline();

    const malloc_statistics_t before = snapshotMalloc();

    for (int done = 0; done < churnTotal; done += SwapRig::BS)
        rig.renderBlock(SwapRig::BS, churnEvents, nullptr);

    const malloc_statistics_t after = snapshotMalloc();
    const long blocksDelta = (long) after.blocks_in_use - (long) before.blocks_in_use;
    const bool ok = primed && blocksDelta == 0;

    printf("    primed=%s blocks_in_use before=%u after=%u delta=%ld -> %s\n",
           primed ? "yes" : "NO", before.blocks_in_use, after.blocks_in_use, blocksDelta,
           ok ? "PASS" : "*** FAIL ***");
    return ok;
}

// ═══════════════════════════ Case 6: LATEST-WINS ═══════════════════════════
static bool caseLatestWins()
{
    printf("[6] LATEST-WINS (swap B while C compiles / during B's fade converges to C)\n");
    const std::string orchB = buildTestOrchestra(SwapRig::SR, 3.0);   // brighter
    const std::string orchC = buildTestOrchestra(SwapRig::SR, 0.5);   // darker (sub-harmonic)
    bool ok = true;

    // ── 6a: issue swap B while swap C compiles ──
    {
        SwapRig rig(200.0f);
        rig.noteOn(60);
        rig.resetTimeline();
        for (int done = 0; done < 4000; done += SwapRig::BS)
            rig.renderBlock(SwapRig::BS, {}, nullptr);

        rig.requestCsoundOrchestra(orchB);
        // csoundCompileInFlight_ is stored true on THIS (calling) thread before
        // the compile thread is even constructed (see maybeStartCompile) -- so
        // this is a deterministic, not a lucky-timing, check.
        const bool wasCompiling = rig.isCompiling();
        rig.requestCsoundOrchestra(orchC);   // supersedes B while B's compile runs

        const bool converged = waitFor([&]
        {
            // Drive the render loop forward while polling so a fade (once
            // primed) actually progresses and completes. maybeStartCompile()
            // is called from THIS (main test) thread -- standing in for
            // production's triggerAsyncUpdate()-driven handleAsyncUpdate()
            // callback on the message thread -- so the SUPERSEDED-and-
            // relaunched C compile gets a chance to start once B's (discarded)
            // compile finishes.
            std::vector<float> discard;
            rig.renderBlock(SwapRig::BS, {}, &discard);
            rig.maybeStartCompile();
            return ! rig.isCompiling() && ! rig.isPending() && ! rig.isFading()
                && rig.csoundEngines_[rig.getActiveIdx()].orchestraText() == orchC;
        }, 8000);

        const std::string finalText = rig.csoundEngines_[rig.getActiveIdx()].orchestraText();
        const bool caseOk = wasCompiling && converged && finalText == orchC;
        ok &= caseOk;
        printf("    6a: wasCompiling=%s converged-to-C=%s finalIsC=%s -> %s\n",
               wasCompiling ? "yes" : "NO", converged ? "yes" : "NO",
               (finalText == orchC) ? "yes" : "NO", caseOk ? "PASS" : "*** FAIL ***");
    }

    // ── 6b: issue swap C during swap B's fade ──
    {
        SwapRig rig(200.0f);
        rig.noteOn(60);
        rig.resetTimeline();
        for (int done = 0; done < 4000; done += SwapRig::BS)
            rig.renderBlock(SwapRig::BS, {}, nullptr);

        rig.requestCsoundOrchestra(orchB);
        const bool bPrimed = waitFor([&] { return rig.isPending(); }, 5000);

        // Start the fade and let it run PART way, then interject C mid-fade.
        std::vector<float> discard;
        rig.renderBlock(SwapRig::BS, {}, &discard);   // consumes pending -> arms the fade
        const bool fadingAfterFirstBlock = rig.isFading();

        rig.requestCsoundOrchestra(orchC);   // issued WHILE B's fade is in progress
        const bool queuedNotStarted = ! rig.isCompiling();   // deferred: freeToStartSwap saw fading==true

        const bool converged = waitFor([&]
        {
            std::vector<float> d;
            rig.renderBlock(SwapRig::BS, {}, &d);
            rig.maybeStartCompile();
            return ! rig.isCompiling() && ! rig.isPending() && ! rig.isFading()
                && rig.csoundEngines_[rig.getActiveIdx()].orchestraText() == orchC;
        }, 8000);

        const std::string finalText = rig.csoundEngines_[rig.getActiveIdx()].orchestraText();
        const bool caseOk = bPrimed && fadingAfterFirstBlock && queuedNotStarted
                          && converged && finalText == orchC;
        ok &= caseOk;
        printf("    6b: bPrimed=%s fadingAfterFirstBlock=%s queuedNotStarted=%s converged-to-C=%s -> %s\n",
               bPrimed ? "yes" : "NO", fadingAfterFirstBlock ? "yes" : "NO",
               queuedNotStarted ? "yes" : "NO", converged ? "yes" : "NO", caseOk ? "PASS" : "*** FAIL ***");
    }

    return ok;
}

// ═══════════════════════════ Case 7: INSTANT ADOPT ═══════════════════════════
//
// BLIND SPOT 2: the guard never exercised the instant-adopt path (the branch
// that takes over when csoundEngines_[currentActiveIdx()].isReady() is FALSE
// at compile-launch time -- see maybeStartCompile()'s activeReadyBefore
// logic). This rig starts with the active engine (index 0) never prepared at
// all -- skipInitialPrepare -- to force that branch deterministically rather
// than racing prepareToPlay.
//
// Ablation (adversarial-review finding, confirmed): SwapRig::maybeStartCompile()
// temporarily reverted to its OLD pre-fix shape (hardcoded inactiveIdx, always
// primes, always arms pending -- i.e. the shape this case exists to catch),
// rebuilt, re-run: pendingArmedEver=YES(bad) (caught by the bounded post-
// isReady() poll added below), becameReady=NO (5s timeout -- the old shape
// compiles into the INACTIVE slot, so the ACTIVE engine this case holds a
// note against never becomes ready) -> case 7 FAILS, both part1Ok and
// part2Ok. Restored the targetIdx/activeReadyBefore shape verbatim, rebuilt,
// re-ran: ALL PASS again (see this function's own printed numbers).
static bool caseInstantAdopt()
{
    printf("[7] INSTANT ADOPT (active engine never prepared -> held note is silent; compile lands IN PLACE, no fade, correct attack)\n");
    SwapRig rig(200.0f, /*skipInitialPrepare=*/true);
    const std::string orchB = buildTestOrchestra(SwapRig::SR, 3.0);

    rig.noteOn(60);   // voice becomes active; engine inert -> must render silence
    rig.resetTimeline();

    std::vector<float> out;
    bool pendingArmedEver = false;

    // Pre-roll: confirm the held note is genuinely silent while the active
    // engine is not ready -- CsoundEngine's own not-ready guards on
    // startBlock()/renderUpTo()/voiceBuffer() and SynthVoice's null-
    // csoundBuf_ safety net are what make this a silence, not a crash.
    const int preRollSamples = (int) (0.1 * SwapRig::SR);
    for (int done = 0; done < preRollSamples; done += SwapRig::BS)
    {
        rig.renderBlock(SwapRig::BS, {}, &out);
        if (rig.isPending()) pendingArmedEver = true;
    }
    const double preRollPeak = peakOf(out);

    rig.requestCsoundOrchestra(orchB);

    // Poll ONLY -- do NOT render here (same reasoning as
    // renderControlledSwap's own header comment: the background compile
    // needs real WALL-CLOCK time, which a tight render-only busy loop never
    // yields, since this offline renderer produces audio far faster than
    // real time -- a first draft of this case spun through thousands of
    // renderBlock() calls, and therefore thousands of "sample-time" seconds,
    // in a handful of real milliseconds, exhausting its guard counter before
    // Csound's background compile thread ever got scheduled). Polling
    // isReady() -- NOT isPending()/isFading(), which the instant-adopt path
    // never touches at all (that's assertion (a) below) -- while nothing
    // consumes samples means readySample (captured right after) is exactly
    // where real, ready-engine rendering resumes; no race with a stale
    // non-ready state mid-block.
    const bool becameReady = waitFor([&] { return rig.csoundEngines_[rig.getActiveIdx()].isReady(); }, 5000);
    if (rig.isPending()) pendingArmedEver = true;
    // Extra bounded, poll-ONLY wait (still nothing rendering to consume the
    // latch): prepare()'s ready-store and any pending-store are two SEPARATE
    // atomics written sequentially by the compile thread (see
    // maybeStartCompile()'s lambda) -- isReady() flipping true is not proof
    // that a subsequent (hypothetical, regressed) pending-store has already
    // happened too. Without this, a regression that drops the
    // "&& activeReadyBefore" guard on the csoundSwapPending_ store could
    // still slip past the check above via pure scheduling luck (adversarial-
    // review finding). 300ms is generous relative to the compile itself
    // (already finished by the time we get here) and to any conceivable
    // follow-on atomic store on that same thread.
    if (waitFor([&] { return rig.isPending(); }, 300)) pendingArmedEver = true;
    const int readySample = (int) out.size();

    // A generous window past readiness for the FIRST real control write
    // (gate/trig, now that setVoiceControls() is no longer a no-op) to reach
    // the orchestra and the strike + portk gate-open (~8ms) to become
    // audible -- several host blocks' worth of slack, not a tight bound.
    const int onsetSearchWindow = SwapRig::BS * 10;
    if (becameReady)
    {
        for (int done = 0; done < onsetSearchWindow; done += SwapRig::BS)
        {
            rig.renderBlock(SwapRig::BS, {}, &out);
            if (rig.isPending()) pendingArmedEver = true;
        }
    }

    double onsetIdx = -1.0;
    const double onsetThresh = std::max(0.02, preRollPeak * 5.0 + 0.01);
    if (becameReady)
        for (int i = readySample; i < (int) out.size(); ++i)
            if (rmsWindow(out, i, 64) > onsetThresh) { onsetIdx = i; break; }

    const bool becomesAudible = becameReady && onsetIdx >= 0
                              && (onsetIdx - readySample) < onsetSearchWindow;

    // (c) NaN/inf, across everything rendered so far.
    bool finiteEverywhere = true;
    for (float s : out) if (! std::isfinite(s)) { finiteEverywhere = false; break; }

    // (c) no hard click BEYOND the portk-smoothed onset: skip a generous
    // settle window right after the strike (portk ~8ms + the test
    // orchestra's own 2ms/50ms strike envelope), then require adjacent-
    // sample deltas from there on to be bounded by that SAME settled
    // region's own p99.9 delta x margin -- exactly case 1's click-free
    // method, just anchored after an onset instead of before a fade.
    bool noClickPostOnset = false;
    double afterOnsetMax = -1.0, settledP999 = -1.0;
    if (becomesAudible)
    {
        const int settleSamples = (int) (0.05 * SwapRig::SR);   // 50ms past the strike
        const int settledStart = (int) onsetIdx + settleSamples;
        const int settledRefLen = std::min(4000, (int) out.size() - settledStart);
        if (settledRefLen > 200)
        {
            settledP999 = p999AdjacentDelta(out, settledStart, settledRefLen);
            afterOnsetMax = maxAdjacentDelta(out, settledStart, (int) out.size() - settledStart);
            noClickPostOnset = afterOnsetMax <= settledP999 * 3.0 + 1.0e-6;
        }
    }

    const bool part1Ok = ! pendingArmedEver && preRollPeak < 1.0e-9 && becameReady
                       && becomesAudible && finiteEverywhere && noClickPostOnset;

    printf("    pendingArmedEver=%s preRollPeak=%.6f becameReady=%s readySample=%d onsetIdx=%.0f (delay=%.0f, bound=%d) becomesAudible=%s\n",
           pendingArmedEver ? "YES(bad)" : "no", preRollPeak, becameReady ? "yes" : "NO", readySample,
           onsetIdx, onsetIdx >= 0 ? onsetIdx - readySample : -1.0, onsetSearchWindow, becomesAudible ? "yes" : "NO");
    printf("    finiteEverywhere=%s afterOnsetMax=%.6f settledP999=%.6f(x3=%.6f) noClickPostOnset=%s -> %s\n",
           finiteEverywhere ? "yes" : "NO", afterOnsetMax, settledP999, settledP999 * 3.0,
           noClickPostOnset ? "yes" : "NO", part1Ok ? "PASS" : "*** FAIL ***");

    // (d) a SECOND swap request, now that the active engine IS ready, must
    // take the NORMAL fade path -- reuses the exact same helper and a
    // case-1-style click-free/actually-transitioning assertion, on the SAME
    // rig/held voice, continuing from wherever the instant-adopt left off.
    const std::string orchC = buildTestOrchestra(SwapRig::SR, 0.5);   // darker, clearly different from orchB
    auto meas2 = renderControlledSwap(rig, (int) (0.1 * SwapRig::SR), orchC, (int) (0.2 * SwapRig::SR));
    auto& out2 = meas2.out;
    const int fadeStartSample = meas2.fadeStartSample;
    const int fadeLen = meas2.fadeLen;

    const double preFadeP999 = p999AdjacentDelta(out2, fadeStartSample - 4000, 4000);
    const double fadeWindowMax = maxAdjacentDelta(out2, fadeStartSample, fadeLen + 200);
    const bool clickFree2 = fadeWindowMax <= preFadeP999 * 3.0 + 1.0e-6;

    const double preBrightness2  = brightnessProxy(out2, fadeStartSample - 4000, 4000);
    const double postBrightness2 = brightnessProxy(out2, fadeStartSample + fadeLen + 2000, 4000);
    const bool clearlyDifferent2 = (postBrightness2 > preBrightness2 * 1.3)
                                 || (postBrightness2 < preBrightness2 * 0.7);
    // NOTE: case 1's own version of this check seeds prevDist as
    // |preBrightness - (risingTrend ? postBrightness : preBrightness)| --
    // correct (the full pre->post swing) for a RISING trend (post > pre,
    // the only direction any of cases 1/4/6 ever exercise: orchB is always
    // brighter than whatever it replaces), but it degenerates to 0 for a
    // FALLING trend, which this case is the first to exercise (orchB-
    // brighter -> orchC-darker). Fixed HERE (not in case 1, which stays
    // untouched) with the direction-agnostic form: the full swing either way.
    bool monotonicIsh2 = true;
    {
        const int steps = 5;
        double prevDist = std::fabs(postBrightness2 - preBrightness2);
        for (int i = 1; i <= steps; ++i)
        {
            const int pos = fadeStartSample + (fadeLen * i) / steps;
            const double v = brightnessProxy(out2, pos - 500, 1000);
            const double distToPost = std::fabs(v - postBrightness2);
            if (distToPost > prevDist + std::fabs(postBrightness2 - preBrightness2) * 0.5 + 1.0e-9)
                monotonicIsh2 = false;
            prevDist = distToPost;
        }
    }

    const bool part2Ok = meas2.becamePending && clickFree2 && clearlyDifferent2 && monotonicIsh2;

    printf("    [second swap, normal path] becamePending=%s fadeStartSample=%d fadeLen=%d clickFree=%s preBrightness=%.4f postBrightness=%.4f clearlyDifferent=%s monotonicIsh=%s -> %s\n",
           meas2.becamePending ? "yes" : "NO", fadeStartSample, fadeLen, clickFree2 ? "yes" : "NO",
           preBrightness2, postBrightness2, clearlyDifferent2 ? "yes" : "NO", monotonicIsh2 ? "yes" : "NO",
           part2Ok ? "PASS" : "*** FAIL ***");

    const bool ok = part1Ok && part2Ok;
    printf("    -> %s\n", ok ? "PASS" : "*** FAIL ***");

    writeWavNormalized("tools/csound_swap_out/case7_instant_adopt.wav", out, (int) SwapRig::SR, -12.0f);
    return ok;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    printf("Phase-2 Csound orchestra-swap guard tool\n");
    printf("SR=%.0f BS=%d kMaxVoices=%d kKsmps=%d\n\n",
           SwapRig::SR, SwapRig::BS, CsoundEngine::kMaxVoices, CsoundEngine::kKsmps);

    bool ok = true;
    ok &= caseHeldNoteTakeover();
    ok &= caseNoRestrike();
    ok &= caseFadeTime();
    ok &= caseNewNoteDuringFade();
    ok &= caseRtClean();
    ok &= caseLatestWins();
    ok &= caseInstantAdopt();

    printf("\nWAVs: tools/csound_swap_out/case{1,2,4,7}_*.wav\n");
    printf("%s\n", ok ? "ALL PASS" : "*** FAIL ***");
    return ok ? 0 : 1;
}
