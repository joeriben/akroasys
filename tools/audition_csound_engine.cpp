// Guard tool for the Phase-1 Csound engine voice bridge
// (docs: SPEC_phase1_csound_engine.md §3 voice bridge + §5 guard tool).
//
// Drives VoiceManager + CsoundEngine directly -- the same layer as the
// existing audition tools (audition_dco_presence_follow.cpp,
// measure_voiceleading.cpp et al) -- never through the plugin's processBlock.
// The block-splitting/pump loop below (CsoundRig::renderBlock) is a literal
// mirror of PluginProcessor.cpp's MIDI-split loop: startBlock() once per host
// block, then per event-bounded sub-segment: writeCsoundControls ->
// renderUpTo -> voiceManager.renderBlock(..., &cs), with the same
// csoundLastWritePos_ negative-carry accounting across block boundaries.
//
// Six cases (spec §5 plus §6 below), each a hard assert -- if any fails, the
// CODE under test is wrong, not the assertion (unless the assertion itself turns
// out to contradict the spec, which would be called out separately):
//   1. NOTE-ON LATENCY   -- post-VCA onset lands within ~1 ksmps + portk lag.
//   2. RELEASE INTEGRITY -- D3: gate = voice ACTIVE, not note-held; the T5ynth
//                           ampEnv release plays out in full after note-off.
//   3. STANDING TONE     -- the oscillator holds steady while gated (no strike,
//                           no decay); reusing a still-active mono slot injects
//                           no orchestra re-strike (amplitude SHAPE is the synth
//                           VCA/ADSR's job, not the oscillator's).
//   4. RT CLEANLINESS    -- D4: zero additional heap blocks across a 10 s
//                           steady-state pump with note churn, post-warmup.
//   5. TIMING            -- raw CsoundEngine: a gate/trig write followed by
//                           renderUpTo produces a non-zero sample within one
//                           ksmps block (64 samples).
//   6. REPEATED NOTES    -- N notes in a row all arrive in the voice buffer,
//                           sequentially and overlapping, and no sample in the
//                           run is non-finite. See the case's own header for
//                           the gap it closes.
//
// Renders WAVs (-12 dBFS) into tools/csound_engine_out/ for BJ's ear.
//
// Build (T5ynth's standard offline-tool recipe -- see
// audition_dco_presence_follow.cpp's header for the base pattern -- plus the
// Csound framework link/rpath from tools/csound_poc.cpp / CMakeLists.txt's
// T5YNTH_CSOUND_FOUND block). This tool only #includes dsp/CsoundEngine.h
// (never <csound/csound.h> directly -- that header is pimpl'd specifically so
// callers never need the framework's own headers), so no extra -I is needed
// for csound.h; the framework is only needed at LINK time for the symbols
// CsoundEngine.cpp (already compiled into lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this]) calls.
//
// IMPORTANT: verify the scraped CXX_DEFINES actually contains
// -DT5YNTH_HAS_CSOUND=1 before compiling -- if the framework weren't found at
// CMake configure time this would silently compile the INERT STUB inline in
// THIS translation unit while linking the REAL CsoundEngine.o from the
// static lib: an ODR violation (stub vs real Impl/class layout) that a
// linker will not necessarily flag as an error.
//
// The CMake target is still called T5ynth; the ARTEFACT it produces is named
// after the product and was renamed to `akroasys` on 2026-07-25, so the flags
// path and the library path do not share a name any more.
//
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   grep -o 'DT5YNTH_HAS_CSOUND=.' /tmp/h.rsp   # must print exactly "=1"
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSOUND_PREFIX=$(brew --prefix csound)
//   CSOUND_FW="$CSOUND_PREFIX/Frameworks"
//   mkdir -p tools/csound_engine_out
//   clang++ -std=c++17 -O2 @/tmp/h.rsp \
//     tools/audition_csound_engine.cpp \
//     build_clean/T5ynth_artefacts/Release/libakroasys_SharedCode.a \
//     -F"$CSOUND_FW" -framework CsoundLib64 -Wl,-rpath,"$CSOUND_FW" \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit \
//     -o tools/csound_engine_out/audition_csound_engine
//   tools/csound_engine_out/audition_csound_engine
//
// An optional argument runs case 6 a SECOND time against an authored orchestra
// instead of the built-in one, at the oversampling factor the plugin compiles
// at, so a body out of the library can be put through the same N-note test:
//
//   tools/csound_engine_out/audition_csound_engine <body.csd> [osFactor]

#include "JuceHeader.h"
#include "dsp/VoiceManager.h"
#include "dsp/CsoundEngine.h"
#include "dsp/BlockParams.h"

#include <malloc/malloc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

// ═══════════════════════════ shared helpers ═══════════════════════════

// Convention copied from tools/audition_dco_presence_follow.cpp / csound_poc.cpp
// (mono, 16-bit PCM, clamped to [-1,1]).
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

// Peak-normalize to targetDbfs and write (a silent buffer writes flat zero).
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

static malloc_statistics_t snapshotMalloc()
{
    malloc_statistics_t st;
    std::memset(&st, 0, sizeof(st));
    malloc_zone_statistics(malloc_default_zone(), &st);
    return st;
}

// ═══════════════════════════ test rig ═══════════════════════════
//
// Mirrors PluginProcessor.cpp's Csound pump wiring exactly (spec §3):
//   - startBlock(numSamples) once per host block (LFO-fill-site equivalent).
//   - per event-bounded sub-segment: writeCsoundControls -> renderUpTo ->
//     voiceManager.renderBlock(..., &cs) -- same order, same
//     samplesSinceLastWrite = renderPos - csoundLastWritePos_ accounting,
//     same negative carry across block boundaries (csoundLastWritePos_ -=
//     numSamples at the end of every block).
struct ScheduledEvent
{
    int sampleOffset = 0;   // relative to the rig's current timeline origin (see resetTimeline)
    std::function<void()> action;
};

struct CsoundRig
{
    static constexpr double SR = 48000.0;
    // Host block size. Deliberately NOT a multiple of kKsmps(64) -- 512 would
    // make every renderUpTo() land exactly on a ksmps boundary (leftover==0
    // in CsoundEngine::renderUpTo every time), leaving the carry FIFO
    // (CsoundEngine.cpp's carryBuf/carryCount, replayed by startBlock at the
    // next block) completely unexercised by every case below (adversarial-
    // review finding). 480 (a common real-world block size, 48000/100) forces
    // a 32-sample carry across every single block boundary in all five cases.
    static constexpr int BS = 480;   // host block size

    VoiceManager vm;
    CsoundEngine cs;
    BlockParams bp;
    juce::AudioBuffer<float> buf { 2, BS };
    std::vector<float> lfoZero;
    float tuning[128] {};

    int csoundLastWritePos_ = 0;
    int absoluteBlockStart_ = 0;   // this rig's running sample clock since the last resetTimeline()
    size_t evtCursor_ = 0;         // index into the events vector currently being scanned

    // See renderBlock. `ready` is false only when an AUTHORED orchestra failed to
    // compile — a built-in-orchestra failure is fatal there and never returns.
    bool  trackVoicePeaks = false;
    float voicePeak[CsoundEngine::kMaxVoices] {};
    bool  ready = false;

    void clearVoicePeaks() { for (float& p : voicePeak) p = 0.0f; }

    // `orchestra` == nullptr compiles the Phase-1 built-in, which is what cases
    // 1-5 have always used. A non-null text is compiled verbatim at `osFactor`,
    // exactly as PluginProcessor's compile thread does it (prepare's fourth
    // argument, lroOsFactor_), so an AUTHORED body can be driven through this
    // same rig at the rate the plugin actually runs Csound at.
    CsoundRig(float ampAttackMs, float ampDecayMs, float ampSustain, float ampReleaseMs, int voiceLimit,
              const char* orchestra = nullptr, int osFactor = 1)
        : lfoZero((size_t) BS, 0.0f)
    {
        for (int i = 0; i < 128; ++i)
            tuning[i] = (float) juce::MidiMessage::getMidiNoteInHertz(i);

        vm.prepare(SR, BS);
        vm.setTuningTable(tuning);
        vm.setEngineMode(SynthVoice::EngineMode::Csound);
        vm.setVoiceLimit(voiceLimit);

        bp.ampAttack = ampAttackMs;
        bp.ampDecay = ampDecayMs;
        bp.ampSustain = ampSustain;
        bp.ampRelease = ampReleaseMs;
        bp.velAmt = 1.0f;
        vm.setBlockParams(bp);

        ready = cs.prepare(SR, BS, orchestra, osFactor);
        if (! ready)
        {
            // An AUTHORED orchestra that does not compile is a result, not a
            // broken tool: say so and let the case report a verdict. Only the
            // built-in orchestra failing is fatal — cases 1-5 have nothing left
            // to measure, and the cause is the environment rather than any input.
            if (orchestra != nullptr)
            {
                std::fprintf(stderr,
                    "    the supplied orchestra did not compile (see the Csound log above).\n"
                    "    NOTE: this argument is a FULL 16-instrument CSD, not a library body --\n"
                    "    backend/lco_write.py's wrap() is what turns a body into one.\n");
                return;
            }
            std::fprintf(stderr,
                "FATAL: CsoundEngine::prepare() failed on the BUILT-IN orchestra -- framework "
                "missing at runtime, or this binary was built against the T5YNTH_HAS_CSOUND=0 "
                "stub (see this file's build-recipe warning). Aborting.\n");
            std::exit(1);
        }
    }

    void noteOn(int note, float velocity = 1.0f, float glideMs = 0.0f)
    {
        vm.noteOn(note, velocity, /*isBind=*/false, glideMs,
                  /*lfo1TrigMode=*/false, /*lfo2TrigMode=*/false, /*lfo3TrigMode=*/false,
                  /*sourceId=*/-1, /*pan=*/0.0f, /*mpeChannel=*/0);
    }

    void noteOff(int note) { vm.noteOff(note, -1); }

    // Resets this rig's local timeline bookkeeping to 0 -- call before
    // scheduling a fresh batch of events whose sampleOffsets are meant to be
    // relative to "now" rather than to the rig's construction-time clock.
    // Voice/engine STATE (active notes, CsoundEngine's internal ksmps carry)
    // is untouched -- this only resets how THIS HARNESS interprets event
    // offsets and the freq-glide "samples since last write" accounting.
    void resetTimeline()
    {
        absoluteBlockStart_ = 0;
        csoundLastWritePos_ = 0;
        evtCursor_ = 0;
    }

    // `events` must be sorted by sampleOffset and expressed relative to
    // whatever absoluteBlockStart_ was at the last resetTimeline(). evtCursor_
    // persists across calls so repeated calls with the SAME events vector
    // (the normal case: one resetTimeline() then many renderBlock() calls)
    // never re-scan or re-fire an already-consumed event. buf is fixed at BS
    // (set at construction) and never resized here, so repeated calls at
    // numSamples==BS cannot allocate.
    void renderBlock(int numSamples, const std::vector<ScheduledEvent>& events, std::vector<float>* outMono)
    {
        buf.clear();
        cs.startBlock(numSamples);

        int renderPos = 0;
        while (renderPos < numSamples)
        {
            int subEnd = numSamples;
            if (evtCursor_ < events.size())
            {
                const int rel = events[evtCursor_].sampleOffset - absoluteBlockStart_;
                subEnd = juce::jlimit(renderPos, numSamples, rel);
            }

            const int subLen = subEnd - renderPos;
            if (subLen > 0)
            {
                // Csound pump (spec §3/D2) -- identical order/accounting to
                // PluginProcessor.cpp's MIDI-split loop.
                vm.writeCsoundControls(cs, bp.performancePitchRatio, renderPos - csoundLastWritePos_);
                cs.renderUpTo(subEnd - 1);
                csoundLastWritePos_ = renderPos;
                vm.renderBlock(buf, bp, lfoZero.data() + renderPos, lfoZero.data() + renderPos,
                               lfoZero.data() + renderPos, renderPos, subLen, &cs);

                // Per-SLOT peak, read straight off the Csound engine rather than
                // inferred from the mono sum. Case 6's pool sweep needs to know
                // WHICH of the sixteen instruments produced audio, and one silent
                // slot among sixteen sounding ones is inaudible in the sum. Off by
                // default, so case 4's malloc measurement is one bool test poorer.
                if (trackVoicePeaks)
                    for (int vi = 0; vi < CsoundEngine::kMaxVoices; ++vi)
                        if (const float* vb = cs.voiceBuffer(vi))
                            for (int s = renderPos; s < subEnd; ++s)
                                voicePeak[vi] = std::max(voicePeak[vi], std::fabs(vb[s]));
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

    // Self-contained convenience wrapper for cases 1-3: resets the timeline,
    // renders totalSamples (rounded up to a whole number of BS blocks, then
    // truncated back to exactly totalSamples), dispatching `events` (offsets
    // relative to sample 0 of this call).
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

// ═══════════════════════════ Case 1: NOTE-ON LATENCY ═══════════════════════════
static bool caseNoteOnLatency()
{
    printf("[1] NOTE-ON LATENCY\n");
    // Near-instant VCA (attack ~0) isolates the Csound orchestra's own onset
    // (gate portk ~8ms + ksmps=64 quantization) as the dominant visible delay.
    CsoundRig rig(0.05f, 5.0f, 1.0f, 2000.0f, CsoundEngine::kMaxVoices);

    const int noteOnAt = 1000;   // mid-block: lands inside host block 1, not block-aligned, not
                                  // ksmps(64)-aligned either
    // Bug found on review: this used to render only `8 * CsoundRig::BS` samples
    // while measuring a steady-state window at [noteOnAt+3000, noteOnAt+4000)
    // = [4000, 5000) -- with the old BS=512 that's only 4096 samples (a
    // partially/fully out-of-bounds std::vector<float> read past `out`'s
    // resize()d size, silently "passing" only by accident via whatever
    // memory happened to sit past the vector's logical end). Render an
    // explicit, comfortably-sufficient length instead of tying it to BS.
    const int totalSamples = noteOnAt + 4500;   // covers the +4000 window end with margin
    auto out = rig.renderRange(totalSamples,
        { { noteOnAt, [&] { rig.noteOn(60); } } });

    // Pre-onset must be silent (sanity: the event actually gates the timeline).
    float preOnsetPeak = 0.0f;
    for (int i = 0; i < noteOnAt; ++i) preOnsetPeak = std::max(preOnsetPeak, std::fabs(out[(size_t) i]));

    // Reference steady-state peak, well after onset settles.
    float steadyPeak = 0.0f;
    for (int i = noteOnAt + 3000; i < noteOnAt + 4000; ++i)
        steadyPeak = std::max(steadyPeak, std::fabs(out[(size_t) i]));

    const float thresh = 0.05f * steadyPeak;
    int onsetIdx = -1;
    for (int i = noteOnAt; i < (int) out.size(); ++i)
        if (std::fabs(out[(size_t) i]) > thresh) { onsetIdx = i; break; }

    const int delaySamples = (onsetIdx >= 0) ? (onsetIdx - noteOnAt) : -1;
    // spec: "< 64 + ~400 samples" (one ksmps block + the ~8ms portk lag,
    // 0.008*48000=384) -- allow a bit of slack (600) without loosening the
    // bound into meaninglessness (a whole extra host block would be ~1112).
    const bool ok = preOnsetPeak < 1.0e-6f && onsetIdx >= 0 && delaySamples >= 0 && delaySamples < 600;

    printf("    preOnsetPeak=%.2e steadyPeak=%.4f onsetDelay=%d samples (%.2f ms) -> %s\n",
           preOnsetPeak, steadyPeak, delaySamples, delaySamples * 1000.0 / CsoundRig::SR,
           ok ? "PASS" : "*** FAIL ***");

    writeWavNormalized("tools/csound_engine_out/case1_noteon_latency.wav", out, (int) CsoundRig::SR, -12.0f);
    return ok;
}

// ═══════════════════════════ Case 2: RELEASE INTEGRITY ═══════════════════════════
static bool caseReleaseIntegrity()
{
    printf("[2] RELEASE INTEGRITY (D3: gate = active, not note-held)\n");
    const float releaseMs = 500.0f;
    CsoundRig rig(2.0f, 20.0f, 1.0f, releaseMs, CsoundEngine::kMaxVoices);

    const int noteOnAt = 0;
    const int noteOffAt = (int) (0.30 * CsoundRig::SR);          // hold 300ms, well into sustain
    const int totalSamples = (int) (0.80 * CsoundRig::SR);        // off + 500ms margin

    auto out = rig.renderRange(totalSamples,
        { { noteOnAt,  [&] { rig.noteOn(60); } },
          { noteOffAt, [&] { rig.noteOff(60); } } });

    const int preOffWindow = (int) (0.02 * CsoundRig::SR);   // 20ms window just before note-off
    double peakBeforeOff = 0.0;
    for (int i = noteOffAt - preOffWindow; i < noteOffAt; ++i)
        peakBeforeOff = std::max(peakBeforeOff, (double) std::fabs(out[(size_t) i]));

    const int measureAt = noteOffAt + (int) (0.25 * CsoundRig::SR);   // 250ms after note-off
    const double rmsAtMeasure = rmsWindow(out, measureAt - 200, 400);
    const double rmsBeforeOff  = rmsWindow(out, noteOffAt - 400, 400);

    const double relDb = 20.0 * std::log10(std::max(1.0e-12, rmsAtMeasure / std::max(1.0e-12, peakBeforeOff)));
    const bool ok = relDb > -40.0 && rmsBeforeOff > 1.0e-4;   // proves a real sustained tone existed pre-off

    printf("    peakBeforeOff=%.4f rmsBeforeOff=%.4f rmsAt+250ms=%.4f (%.1f dB rel.) -> %s\n",
           peakBeforeOff, rmsBeforeOff, rmsAtMeasure, relDb, ok ? "PASS" : "*** FAIL ***");

    writeWavNormalized("tools/csound_engine_out/case2_release_integrity.wav", out, (int) CsoundRig::SR, -12.0f);
    return ok;
}

// ═══════════════════════════ Case 3: STANDING TONE ═══════════════════════════
// STANDING-TONE contract (BJ 2026-07-17, "Hüllkurven gehören nicht in den
// Oszillator. Vollständig entfernen."): the oscillator holds a steady tone for
// as long as the gate is open -- no strike, no per-partial decay; amplitude
// SHAPE is the synth VCA/ADSR's job OUTSIDE the orchestra. A stolen/reused mono
// voice snaps to the new pitch (freq is un-smoothed) and keeps holding -- the
// orchestra injects no fresh transient of its own. Empirical, comparative
// (measure, don't theorize about exact levels):
//   (a) HELD-NOTE STEADINESS -- a single held note's late-window RMS matches
//       its early post-attack RMS. The OLD strike orchestra decayed here and
//       would fail; a standing tone holds. This is the decisive proof.
//   (b) NO RE-STRIKE ON REUSE -- run the SAME mono slot twice (once one note,
//       once note-off + second note-on 50ms later while the voice is still
//       active). Shortly after the second note-on both runs sit at the same
//       steady level (VCA back at sustain, orchestra steady): the retrig run
//       must NOT spike above the control the way a fresh strike would -- a
//       two-sided band, not the old one-sided "re-strike is louder".
static bool caseStandingTone()
{
    printf("[3] STANDING TONE (held note holds; reuse injects no re-strike)\n");
    const float releaseMs = 500.0f;   // long: voice 0 stays active across both notes
    const int totalSamples = (int) (0.10 * CsoundRig::SR);   // 100ms

    CsoundRig rigSingle(2.0f, 20.0f, 1.0f, releaseMs, /*voiceLimit=*/1);
    auto outSingle = rigSingle.renderRange(totalSamples, { { 0, [&] { rigSingle.noteOn(60); } } });

    CsoundRig rigRetrig(2.0f, 20.0f, 1.0f, releaseMs, /*voiceLimit=*/1);
    const int noteOffAt = (int) (0.005 * CsoundRig::SR);    // 5ms
    const int secondOnAt = (int) (0.050 * CsoundRig::SR);   // 50ms -- voice 0 still active
    auto outRetrig = rigRetrig.renderRange(totalSamples,
        { { 0,          [&] { rigRetrig.noteOn(60); } },
          { noteOffAt,  [&] { rigRetrig.noteOff(60); } },
          { secondOnAt, [&] { rigRetrig.noteOn(60); } } });   // same voice (mono), SAME pitch:
                                                              // identical spectrum + phase-continuous
                                                              // oscillators -> a clean RMS comparison
                                                              // (a different pitch would beat
                                                              // differently and confound the level).

    // (a) Steadiness of the single held note. The VCA (2ms attack / 20ms decay
    // / 1.0 sustain) has fully settled by ~30ms, so any drift between an early
    // (30-45ms) and a late (82-97ms) window is the ORCHESTRA's -- and a
    // standing tone must not drift. RMS is pitch-independent, so this isolates
    // amplitude behaviour.
    const int winLen  = (int) (0.015 * CsoundRig::SR);
    const double rmsEarly = rmsWindow(outSingle, (int) (0.030 * CsoundRig::SR), winLen);
    const double rmsLate  = rmsWindow(outSingle, (int) (0.082 * CsoundRig::SR), winLen);
    const bool steady = std::fabs(rmsLate - rmsEarly) < 0.20 * std::max(rmsEarly, rmsLate) + 1.0e-6;

    // Sanity: both rigs agree BEFORE the retrig run's note-off (identical setup
    // -- same note, freq, VCA -- before anything is deliberately made to
    // diverge). Not compared between noteOffAt and secondOnAt: the retrig run
    // is releasing there while the control still sustains, which is intended.
    const int preWindowEnd = noteOffAt - 20;
    const double rmsPreSingle = rmsWindow(outSingle, preWindowEnd - 100, 100);
    const double rmsPreRetrig = rmsWindow(outRetrig, preWindowEnd - 100, 100);
    const bool preMatch = std::fabs(rmsPreSingle - rmsPreRetrig)
                        < 0.25 * std::max(rmsPreSingle, rmsPreRetrig) + 1.0e-6;

    // (b) 15ms after the second note-on both runs sit at the SAME steady level
    // (VCA re-attacked back to sustain, orchestra steady, same pitch): a
    // two-sided band. A fresh orchestra strike would push the retrig run well
    // above the control; a silent drop-out would push it below.
    const int measureAt = secondOnAt + (int) (0.015 * CsoundRig::SR);
    const double rmsRetrigAfter = rmsWindow(outRetrig, measureAt - 150, 300);
    const double rmsSingleAfter = rmsWindow(outSingle, measureAt - 150, 300);
    const bool reuseSteady = std::fabs(rmsRetrigAfter - rmsSingleAfter)
                           < 0.20 * std::max(rmsRetrigAfter, rmsSingleAfter) + 1.0e-6;

    const bool ok = steady && preMatch && reuseSteady;

    printf("    steadiness: early=%.4f late=%.4f (steady=%s)\n",
           rmsEarly, rmsLate, steady ? "yes" : "NO");
    printf("    preDiverge: single=%.4f retrig=%.4f (match=%s)\n",
           rmsPreSingle, rmsPreRetrig, preMatch ? "yes" : "NO");
    printf("    +15ms post-2nd-note: control=%.4f retrig=%.4f (reuse steady, no re-strike=%s) -> %s\n",
           rmsSingleAfter, rmsRetrigAfter, reuseSteady ? "yes" : "NO", ok ? "PASS" : "*** FAIL ***");

    writeWavNormalized("tools/csound_engine_out/case3_standing_single.wav", outSingle, (int) CsoundRig::SR, -12.0f);
    writeWavNormalized("tools/csound_engine_out/case3_standing_retrig.wav", outRetrig, (int) CsoundRig::SR, -12.0f);
    return ok;
}

// ═══════════════════════════ Case 4: RT CLEANLINESS ═══════════════════════════
// D4: post-warmup, a steady-state pump with note churn must not move the heap
// (blocks_in_use) at all. Everything the HARNESS itself touches in the
// measured region is pre-built/reserved before the "before" snapshot, so a
// failure here can only be the DSP code under test (VoiceManager/SynthVoice/
// CsoundEngine), not this tool.
static bool caseRtCleanliness()
{
    printf("[4] RT CLEANLINESS (D4: zero malloc delta across a 10s churn pump)\n");
    CsoundRig rig(3.0f, 30.0f, 0.8f, 150.0f, CsoundEngine::kMaxVoices);

    // Warm-up phase 1: gate a handful of voices on, so any of THIS harness's
    // or VoiceManager/SynthVoice's own one-time lazy allocations (separate
    // from CsoundEngine's own D4 warmup, already done inside cs.prepare())
    // happen before the snapshot, not during the measured window.
    {
        std::vector<ScheduledEvent> warmupOn;
        warmupOn.reserve(4);
        for (int i = 0; i < 4; ++i)
            warmupOn.push_back({ i * 2000, [&rig, i] { rig.noteOn(60 + i); } });
        rig.resetTimeline();
        const int total = 3 * CsoundRig::BS + 8192;
        for (int done = 0; done < total; done += CsoundRig::BS)
            rig.renderBlock(CsoundRig::BS, warmupOn, nullptr);
    }
    // Warm-up phase 2: release them.
    {
        std::vector<ScheduledEvent> warmupOff;
        warmupOff.reserve(4);
        for (int i = 0; i < 4; ++i)
            warmupOff.push_back({ i * 100, [&rig, i] { rig.noteOff(60 + i); } });
        rig.resetTimeline();
        const int total = 2 * CsoundRig::BS + 8192;
        for (int done = 0; done < total; done += CsoundRig::BS)
            rig.renderBlock(CsoundRig::BS, warmupOff, nullptr);
    }

    // Steady-state churn schedule: a new note every ~100ms, rotating pitch
    // (VoiceManager's free-voice-first allocation rotates the voice slot
    // too), for 10s. Pre-built entirely before the "before" snapshot -- no
    // allocation happens inside the measured loop below.
    static int churnNote = 60;   // function-local static: no capture needed, single main() run
    const int churnPeriod = (int) (0.10 * CsoundRig::SR);   // 100ms
    const int totalSamples = 10 * (int) CsoundRig::SR;
    const int numChurnEvents = totalSamples / churnPeriod + 2;
    std::vector<ScheduledEvent> churnEvents;
    churnEvents.reserve((size_t) numChurnEvents);
    for (int i = 0; i < numChurnEvents; ++i)
    {
        churnEvents.push_back({ i * churnPeriod, [&rig]
        {
            rig.noteOff(churnNote);
            churnNote = 55 + (churnNote - 55 + 3) % 12;
            rig.noteOn(churnNote);
        } });
    }
    rig.resetTimeline();

    const malloc_statistics_t before = snapshotMalloc();

    for (int done = 0; done < totalSamples; done += CsoundRig::BS)
        rig.renderBlock(CsoundRig::BS, churnEvents, nullptr);

    const malloc_statistics_t after = snapshotMalloc();

    const long blocksDelta = (long) after.blocks_in_use - (long) before.blocks_in_use;
    const bool ok = blocksDelta == 0;

    printf("    blocks_in_use before=%u after=%u delta=%ld -> %s\n",
           before.blocks_in_use, after.blocks_in_use, blocksDelta, ok ? "PASS" : "*** FAIL ***");
    return ok;
}

// ═══════════════════════════ Case 5: TIMING (raw CsoundEngine) ═══════════════════════════
static bool caseTiming()
{
    printf("[5] TIMING (raw CsoundEngine: gate/trig write -> renderUpTo)\n");
    CsoundEngine cs;
    if (! cs.prepare(CsoundRig::SR, CsoundRig::BS))
    {
        printf("    cs.prepare() failed -> *** FAIL ***\n");
        return false;
    }

    CsoundEngine::VoiceControls c { /*gate=*/1.0f, /*freqHz=*/440.0f, /*velocity=*/1.0f,
                                    /*pressure=*/0.0f, /*timbre=*/0.5f, /*trigEpoch=*/1.0f };
    cs.setVoiceControls(0, c);
    cs.startBlock(CsoundEngine::kKsmps);
    cs.renderUpTo(CsoundEngine::kKsmps - 1);

    const float* v0 = cs.voiceBuffer(0);
    int firstNonZero = -1;
    const float eps = 1.0e-4f;
    if (v0 != nullptr)
        for (int i = 0; i < CsoundEngine::kKsmps; ++i)
            if (std::fabs(v0[i]) > eps) { firstNonZero = i; break; }

    const bool ok = firstNonZero >= 0 && firstNonZero < CsoundEngine::kKsmps;
    printf("    firstNonZero=%d (ksmps=%d) -> %s\n", firstNonZero, CsoundEngine::kKsmps,
           ok ? "PASS" : "*** FAIL ***");
    return ok;
}

// ═══════════════════════════ Case 6: REPEATED NOTES ═══════════════════════════
// THE GAP THIS CLOSES (2026-07-31). Every case above plays at most TWO notes,
// and the offline harness on the other side of the fence -- tools/lco_measure.py
// -- emits exactly ONE trigger edge per render (`ktrig = timeinsts() >= preroll
// ? 1 : 0`). So between them the two harnesses cannot see any defect that first
// appears at the SECOND note, or at the note that reuses a voice slot: a latched
// filter state, a trigger epoch that stops stepping, an allocator that hands out
// a slot nothing renders into. That class was invisible, and a symptom in it
// ("exactly one note, then silence") is what this case was written for.
//
// TWO RUNS, and each one measures the ONLY thing it is able to measure. Both
// shapes below were falsified against a deliberately-silenced note before being
// kept, because the first version of this case had a second run that could not
// see one at all.
//
//   (a) SEQUENTIAL -- release over before the next note-on, so each note's
//       window contains that note ALONE and an RMS floor means what it says.
//       Measured on a build with one note's note-on suppressed: that note reads
//       0.0000 and the run FAILS. The pre-note windows are asserted quiet, which
//       is what licenses the floor: if a window could contain a predecessor's
//       tail, a silent note would be masked by it.
//
//       WHAT AN RMS FLOOR STILL CANNOT SEPARATE, stated because it is a real
//       limit and not a hedge: the sixteen Csound instruments are ALWAYS ON, so
//       a resonant body keeps ringing inside its slot while the VCA gate is
//       shut. A note whose STRIKE fails but whose gate opens therefore lets the
//       previous strike's ring through. What catches it here is that the ring
//       DECAYS: the pre-fix `singing_bowl` (struck once, when the orchestra is
//       built) falls to 0.04 x note 1 by note 17 and fails. A body whose ring
//       outlasts the whole run would slip through, and only an onset measure
//       could tell those two apart.
//
//   (b) POOL SWEEP -- what run (a) cannot reach: it only ever occupies two
//       slots, because findFreeVoice returns the LOWEST inactive one. Holding
//       sixteen notes at once is the only way to make the allocator hand out
//       every slot, and the per-slot peaks are read straight off
//       CsoundEngine::voiceBuffer rather than from the mono sum, where one
//       silent instrument among sixteen is inaudible. Run twice, so the second
//       round is slot REUSE and not first use.
//
// A lower bound only, never an upper one: a self-decaying body's ring is still
// present when the next strike lands, so a run legitimately gets LOUDER note by
// note (measured on `singing_bowl`: 0.2575 -> 0.4708 -> 0.6108). The symptom
// this guards against is silence, and silence is a floor.
static int nonFiniteCount(const std::vector<float>& buf)
{
    int n = 0;
    for (float s : buf)
        if (! std::isfinite(s)) ++n;
    return n;
}

static bool sequentialNoteRun(const std::string& wavName, const char* orchestra, int osFactor)
{
    constexpr int    kNotes   = 20;
    constexpr double kHoldSec = 0.35;
    constexpr double kGapSec  = 0.30;
    constexpr float  kRelease = 200.0f;   // exp curve, idle well inside the gap

    CsoundRig rig(2.0f, 20.0f, 1.0f, kRelease, CsoundEngine::kMaxVoices, orchestra, osFactor);
    if (! rig.ready) { printf("    sequential   -> *** FAIL *** (orchestra did not compile)\n"); return false; }

    const int hold = (int) (kHoldSec * CsoundRig::SR);
    const int step = (int) ((kHoldSec + kGapSec) * CsoundRig::SR);

    std::vector<ScheduledEvent> events;
    events.reserve((size_t) kNotes * 2);
    for (int k = 0; k < kNotes; ++k)
    {
        // ONE pitch throughout: identical spectrum note to note, so the RMS
        // comparison below is a level comparison and nothing else (case 3 picks
        // its retrig pitch on the same reasoning).
        events.push_back({ k * step,        [&rig] { rig.noteOn(60); } });
        events.push_back({ k * step + hold, [&rig] { rig.noteOff(60); } });
    }
    auto out = rig.renderRange(kNotes * step, events);

    // Skip the first 5 ms of each note: the VCA attack and the orchestra's own
    // portk gate lag both live in there, and neither is what is being measured.
    const int skip   = (int) (0.005 * CsoundRig::SR);
    const int winLen = juce::jmax(1, juce::jmin(hold - skip, (int) (0.150 * CsoundRig::SR)));
    const int quietLen = (int) (0.050 * CsoundRig::SR);   // ends AT the note-on

    std::vector<double> rms((size_t) kNotes, 0.0);
    double loudestGap = 0.0;
    for (int k = 0; k < kNotes; ++k)
    {
        rms[(size_t) k] = rmsWindow(out, k * step + skip, winLen);
        if (k > 0)
            loudestGap = std::max(loudestGap, rmsWindow(out, k * step - quietLen, quietLen));
    }

    const int nonFinite = nonFiniteCount(out);
    const double first = rms[0];
    int worstNote = 0;
    double worst = first;
    for (int k = 1; k < kNotes; ++k)
        if (rms[(size_t) k] < worst) { worst = rms[(size_t) k]; worstNote = k; }

    // The gap must be quiet against the NOTE, not against an absolute level: a
    // loud body's numerical floor is higher than a quiet body's whole signal.
    const bool firstSounds = first > 1.0e-4;
    const bool gapsQuiet   = loudestGap < 0.10 * first;
    const bool allSound    = worst > 0.25 * first;
    const bool ok = firstSounds && gapsQuiet && allSound && nonFinite == 0;

    printf("    sequential   note1=%.4f  worst=note%d %.4f (%.2f x)  loudest gap %.4f (%.2f x)"
           "  nonFinite=%d -> %s\n",
           first, worstNote + 1, worst, first > 0.0 ? worst / first : 0.0,
           loudestGap, first > 0.0 ? loudestGap / first : 0.0,
           nonFinite, ok ? "PASS" : "*** FAIL ***");
    printf("      per-note rms:");
    for (int k = 0; k < kNotes; ++k)
        printf(" %.4f", rms[(size_t) k]);
    printf("\n");

    // A run that failed on non-finite samples still gets a WAV, because the
    // shape BEFORE the fault is the evidence -- but the non-finite samples are
    // zeroed for the file, since writeWav's lround() has nothing sane to do with
    // a NaN and peak-normalising a buffer that contains one silently keeps the
    // gain at 1. The count above is the finding; the file is only for the ear.
    if (nonFinite > 0)
        for (float& s : out)
            if (! std::isfinite(s)) s = 0.0f;
    writeWavNormalized(wavName, out, (int) CsoundRig::SR, -12.0f);
    return ok;
}

static bool poolSweepRun(const std::string& wavName, const char* orchestra, int osFactor)
{
    constexpr float kRelease = 200.0f;
    // A distinct pitch per slot: sixteen voices at ONE pitch interfere at fixed
    // relative phase, and the resulting per-slot spread would be read as a level
    // finding. Whole tones keep every slot's own partials apart.
    auto pitchFor = [](int slot) { return 48 + slot * 2; };

    CsoundRig rig(2.0f, 20.0f, 1.0f, kRelease, CsoundEngine::kMaxVoices, orchestra, osFactor);
    if (! rig.ready) { printf("    pool sweep   -> *** FAIL *** (orchestra did not compile)\n"); return false; }
    rig.trackVoicePeaks = true;

    const int onEvery  = (int) (0.060 * CsoundRig::SR);
    const int settle   = (int) (0.300 * CsoundRig::SR);   // after the 16th note-on
    const int roundLen = CsoundEngine::kMaxVoices * onEvery + settle;
    const int release  = (int) (1.500 * CsoundRig::SR);   // exp release + margin

    std::vector<float> out;
    bool ok = true;
    float peaks[2][CsoundEngine::kMaxVoices] {};

    for (int round = 0; round < 2; ++round)
    {
        std::vector<ScheduledEvent> on;
        on.reserve(CsoundEngine::kMaxVoices);
        for (int v = 0; v < CsoundEngine::kMaxVoices; ++v)
            on.push_back({ v * onEvery, [&rig, pitchFor, v] { rig.noteOn(pitchFor(v)); } });

        // Peaks are cleared AFTER the previous round's release has been rendered,
        // so a slot still ringing out from round 1 cannot be counted for round 2.
        rig.clearVoicePeaks();
        rig.renderRange(roundLen, on);   // renderRange resets the timeline itself
        std::memcpy(peaks[round], rig.voicePeak, sizeof(rig.voicePeak));

        std::vector<ScheduledEvent> off;
        off.reserve(CsoundEngine::kMaxVoices);
        for (int v = 0; v < CsoundEngine::kMaxVoices; ++v)
            off.push_back({ 0, [&rig, pitchFor, v] { rig.noteOff(pitchFor(v)); } });
        auto tail = rig.renderRange(release, off);
        (void) tail;
    }

    // Relative to the loudest slot, so a body whose level varies over two octaves
    // of pitch is not read as a dead slot.
    for (int round = 0; round < 2; ++round)
    {
        float loudest = 0.0f;
        for (float p : peaks[round]) loudest = std::max(loudest, p);
        int silent = 0;
        for (float p : peaks[round]) if (p < 0.05f * loudest || loudest <= 1.0e-5f) ++silent;
        const bool roundOk = loudest > 1.0e-4f && silent == 0;
        ok &= roundOk;
        printf("    pool sweep %s  loudest slot %.4f  silent slots %d/%d -> %s\n",
               round == 0 ? "(first use)" : "(reuse)   ", loudest, silent,
               CsoundEngine::kMaxVoices, roundOk ? "PASS" : "*** FAIL ***");
        printf("      per-slot peak:");
        for (float p : peaks[round]) printf(" %.3f", p);
        printf("\n");
    }

    // The sweep's own audio, for the ear: sixteen voices arriving one by one.
    {
        CsoundRig ear(2.0f, 20.0f, 1.0f, kRelease, CsoundEngine::kMaxVoices, orchestra, osFactor);
        if (ear.ready)
        {
            std::vector<ScheduledEvent> on;
            for (int v = 0; v < CsoundEngine::kMaxVoices; ++v)
                on.push_back({ v * onEvery, [&ear, pitchFor, v] { ear.noteOn(pitchFor(v)); } });
            out = ear.renderRange(roundLen, on);
            for (float& s : out) if (! std::isfinite(s)) s = 0.0f;
            writeWavNormalized(wavName, out, (int) CsoundRig::SR, -12.0f);
        }
    }
    return ok;
}

static bool caseRepeatedNotes(const char* orchestra, int osFactor, const char* what)
{
    // The EFFECTIVE factor, not the requested one: prepare() clamps anything
    // outside {1,2,4} and caps the absolute engine rate, so printing the request
    // would report a run that did not happen.
    const int effective = CsoundEngine::effectiveOversampleFactor(CsoundRig::SR, osFactor);
    printf("[6] REPEATED NOTES (%s, os=%dx", what, effective);
    if (effective != osFactor) printf(", requested %dx", osFactor);
    printf(")\n");

    const std::string prefix = std::string("tools/csound_engine_out/case6_")
                             + (orchestra == nullptr ? "builtin" : "authored");
    bool ok = sequentialNoteRun(prefix + "_sequential.wav", orchestra, osFactor);
    ok &= poolSweepRun(prefix + "_poolsweep.wav", orchestra, osFactor);
    return ok;
}

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // safe init for any JUCE statics

    // Optional: an authored orchestra to put through case 6 as well, and the
    // oversampling factor to compile it at (the plugin's own is lroOsFactor_).
    std::string authored;
    int authoredOs = 4;
    if (argc > 1)
    {
        std::ifstream f(argv[1], std::ios::binary);
        authored.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        // An unreadable OR empty file must not read as "no argument given": that
        // silently skips the authored run and the tool still prints ALL PASS.
        if (! f || authored.empty())
        {
            std::fprintf(stderr, "FATAL: orchestra file '%s' is missing or empty\n", argv[1]);
            return 1;
        }
        if (argc > 2)
        {
            authoredOs = std::atoi(argv[2]);
            if (authoredOs != 1 && authoredOs != 2 && authoredOs != 4)
            {
                std::fprintf(stderr, "FATAL: oversampling factor must be 1, 2 or 4 (got '%s')\n", argv[2]);
                return 1;
            }
        }
    }

    printf("Phase-1 Csound engine voice bridge -- guard tool (spec sections 3+5)\n");
    printf("SR=%.0f BS=%d kMaxVoices=%d kKsmps=%d\n\n",
           CsoundRig::SR, CsoundRig::BS, CsoundEngine::kMaxVoices, CsoundEngine::kKsmps);

    bool ok = true;
    ok &= caseNoteOnLatency();
    ok &= caseReleaseIntegrity();
    ok &= caseStandingTone();
    ok &= caseRtCleanliness();
    ok &= caseTiming();
    ok &= caseRepeatedNotes(nullptr, 1, "built-in orchestra");
    if (! authored.empty())
        ok &= caseRepeatedNotes(authored.c_str(), authoredOs, argv[1]);

    printf("\nWAVs: tools/csound_engine_out/case{1,2,3,6}_*.wav\n");
    printf("NOTE: case 6's authored run leaves BS(480) exactly divisible by "
           "kKsmps/os at os=2 and os=4,\n      so the carry FIFO the 480 was "
           "chosen for is exercised by the built-in (os=1) run only.\n");
    printf("%s\n", ok ? "ALL PASS" : "*** FAIL ***");
    return ok ? 0 : 1;
}
