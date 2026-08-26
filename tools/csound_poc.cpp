// Phase 0: Csound kill-gate PoC.
//
// Standalone offline probe. Reads nothing from src/ except the two headers
// pulled in below (JuceHeader.h, dsp/WavetableOscillator.h) purely to render
// the two "current B path" comparison WAVs (C5/C6) through the SAME engine
// code the plugin ships. Edits nothing outside this file + tools/csound_poc_out/.
//
// Three verdicts (LINK / RT-PATTERN / CPU) for the always-on, channel-gated
// Csound pattern, plus six renders (four Csound + two current-B) for BJ's ear.
// The tool renders; the verdict on the musical gate is his.
//
// ---------------------------------------------------------------------------
// Csound environment (this machine): `brew --prefix csound` = /opt/homebrew/opt/csound,
// Csound 6.18.1_14 (arm64, double samples). That prefix ships ONLY
// Frameworks/CsoundLib64.framework for the C API -- there is no lib/libcsound64.dylib
// in this install. lib/libcsnd6.dylib in the same lib/ dir is the C++/SWIG wrapper
// and must NOT be linked (it pulls in libsndfile + a *second*, separate
// CsoundLib64 reference and is not the plain C API this tool uses).
//
// Build (standalone clang++, C++17, -O2; T5ynth's own response-file recipe for
// C5/C6's static-lib link, exactly as tools/audition_dco_presence_follow.cpp's
// header documents, plus the Csound framework):
//
//   SCRATCH=<a scratch dir>
//   CSOUND_PREFIX=$(brew --prefix csound)
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > "$SCRATCH/h.rsp"
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> "$SCRATCH/h.rsp"
//   clang++ -std=c++17 -O2 @"$SCRATCH/h.rsp" -I"$CSOUND_PREFIX/include" \
//     tools/csound_poc.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -F"$CSOUND_PREFIX/Frameworks" -framework CsoundLib64 \
//     -Wl,-rpath,"$CSOUND_PREFIX/Frameworks" \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit \
//     -o tools/csound_poc_out/csound_poc
//   tools/csound_poc_out/csound_poc
//
// ---------------------------------------------------------------------------
// Csound syntax fixes applied to the spec's CSDs (found by test-compiling each
// CSD with the `csound` CLI before wiring up this harness; verbatim old->new,
// reasons inline -- see the CSD text constants below for exactly what ships):
//
//  1. RT probe score: `i 1 0 z 1` .. `i 1 0 z 8`  ->  `i 1 0 3600 1` .. `i 1 0 3600 8`
//     `z` is not a valid Csound p3 duration token. The score reader accepts it
//     without a compile error, but a minimal repro (`i 1 0 z` / `e 0.05`, one
//     trivial oscillator) driven via the `csound` CLI (whose internal perform
//     loop honors the score's own `e` and tries to reconcile the still-"active"
//     z-duration note at that boundary) hangs indefinitely -- confirmed twice,
//     independently, with an 8s bounded wait before a hard kill. Driving the
//     SAME csd via raw csoundPerformKsmps() calls (this tool's own access
//     pattern, which never asks Csound to reconcile/terminate anything -- it
//     just keeps pumping blocks) does NOT reproduce a hang in isolation: it
//     runs cleanly and simply never returns nonzero. Since this tool's RT probe
//     never relies on Csound's own end-of-score signal anyway (every loop bound
//     is the harness's own iteration count), `z` was not strictly required to
//     fail here -- but with a provably pathological code path sitting one
//     driver-choice away from this one, and a concrete duration costing
//     nothing, the fix is applied as a zero-cost precaution rather than a
//     narrowly-scoped one. 3600 mirrors the score's own `e 3600` line; any
//     sufficiently large finite p3 would do just as well.
//
//  2. C1 score: `i "Partial" 0 9 ...`  ->  `i 1 0 9 ...` (all 12 lines; same fix
//     shape used for C2/C3/C4's own instruments, composed with numeric p1 from
//     the start since their score lines were never given verbatim).
//     Quoted named-instrument score invocation fails on this Homebrew
//     6.18.1_14 arm64 build: `instr Partial` compiles fine and the compiler
//     itself confirms "instr Partial uses instrument number 1", but
//     `i "Partial" 0 1` in the score is rejected at perform time with
//     "note deleted. instr 0(200) undefined" (reproduced in isolation with a
//     3-line orchestra+score; the same failure persists regardless of which
//     numeric slot the named instrument lands on). Fixed by invoking the SAME
//     instrument by its compiler-assigned number instead; the orchestra's
//     `instr Partial` declaration (readable, as given) is untouched -- only the
//     score's p1 field changes.
//
// No other deviations from the spec's given CSD text. C2/C3/C4's score DATA
// (the actual per-partial numbers) were not given verbatim in the spec (only
// the instrument body + a prose description of the score) and were composed
// deterministically from that prose; see the comments at each CSD constant.
// ---------------------------------------------------------------------------

#include <csound/csound.h>

// csound's sysdep.h (pulled in transitively by csound.h) leaks several short
// macro names into the rest of this translation unit (LIKELY/UNLIKELY/
// ATOMIC_*/DIRSEP/ENVSEP/CS_PURE) that risk colliding with identifiers in JUCE
// or the T5ynth DSP headers included below. This tool only calls Csound's
// documented PUBLIC C API, never these internal macros, so clearing them here
// is safe and confined entirely to this file.
#undef LIKELY
#undef UNLIKELY
#undef DIRSEP
#undef ENVSEP
#undef ATOMIC_SET
#undef ATOMIC_GET
#undef ATOMIC_ADD
#undef ATOMIC_SUB
#undef CS_PURE

#include "JuceHeader.h"
#include "dsp/WavetableOscillator.h"

#include <malloc/malloc.h>
#include <time.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// ═══════════════════════════ shared helpers ═══════════════════════════

// writeWav convention copied verbatim from tools/audition_dco_presence_follow.cpp
// (mono, 16-bit PCM, clamped to [-1,1]).
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

static void writeTextFile(const std::string& path, const std::string& text)
{
    std::ofstream f(path, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

struct PeakRms { double peak = 0.0; double rms = 0.0; };

static PeakRms measurePeakRms(const std::vector<float>& buf)
{
    double peak = 0.0, sumSq = 0.0;
    for (float s : buf) { peak = std::max(peak, (double) std::fabs(s)); sumSq += (double) s * (double) s; }
    PeakRms r; r.peak = peak; r.rms = std::sqrt(sumSq / std::max<size_t>(1, buf.size()));
    return r;
}

// Peak-normalize to targetDbfs; returns the linear gain applied (1.0 on
// near-silence, left untouched rather than dividing by ~0).
static double normalizeToDbfs(std::vector<float>& buf, double targetDbfs)
{
    const PeakRms pre = measurePeakRms(buf);
    if (pre.peak < 1.0e-9) return 1.0;
    const double targetLin = std::pow(10.0, targetDbfs / 20.0);
    const double gain = targetLin / pre.peak;
    for (float& s : buf) s = (float) (s * gain);
    return gain;
}

// ONE global exponential decay (peak -> targetDb over toDbOverSec seconds,
// continuing the same exponential past that point) applied to the WHOLE
// buffer. This is B's limitation made explicit: no per-partial envelopes,
// just a single post-hoc gain curve over the entire mix.
static void applyGlobalExpDecay(std::vector<float>& buf, double sr, double toDbOverSec, double targetDb)
{
    for (size_t i = 0; i < buf.size(); ++i) {
        const double t = (double) i / sr;
        const double db = targetDb * (t / toDbOverSec);
        const double g = std::pow(10.0, db / 20.0);
        buf[i] = (float) (buf[i] * g);
    }
}

struct TimingStats { double medianUs = 0.0, p99Us = 0.0, maxUs = 0.0; size_t n = 0; };

static TimingStats computeStats(std::vector<double> v)
{
    TimingStats s; s.n = v.size();
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    s.medianUs = v[v.size() / 2];
    const size_t p99idx = (size_t) std::min<long long>((long long) v.size() - 1,
                               std::llround(0.99 * (double) (v.size() - 1)));
    s.p99Us = v[p99idx];
    s.maxUs = v.back();
    return s;
}

static inline double elapsedUs(const struct timespec& t0, const struct timespec& t1)
{
    return (double) (t1.tv_sec - t0.tv_sec) * 1.0e6 + (double) (t1.tv_nsec - t0.tv_nsec) / 1.0e3;
}

static malloc_statistics_t snapshotMalloc()
{
    malloc_statistics_t st;
    std::memset(&st, 0, sizeof(st));
    malloc_zone_statistics(malloc_default_zone(), &st);
    return st;
}

// Captures Csound's own diagnostic/compile/perform messages instead of
// letting them hit stdout, so this tool's stdout stays exactly the three
// verdicts + one line per render (spec). csoundCreateMessageBuffer(cs, 0)
// (toStdOut=0) is the robust suppression mechanism -- an earlier attempt using
// csoundSetMessageCallback() left a couple of teardown-time messages
// ("resetting Csound instance", a stale performance summary) leaking straight
// to the console via a path that bypasses the callback; the message-buffer
// API queues everything and prints nothing unless explicitly drained.
// Call csoundCreateMessageBuffer(cs, 0) once, right after csoundCreate();
// drain with drainCsoundMessages() on failure paths for diagnostics; call
// csoundDestroyMessageBuffer(cs) once, right before csoundDestroy(cs).
static std::string drainCsoundMessages(CSOUND* cs)
{
    std::string log;
    const int n = csoundGetMessageCnt(cs);
    for (int i = 0; i < n; ++i) {
        const char* msg = csoundGetFirstMessage(cs);
        if (msg != nullptr) log += msg;
        csoundPopFirstMessage(cs);
    }
    return log;
}

static bool checkMyfltSize()
{
    const int runtime = csoundGetSizeOfMYFLT();
    if (runtime != (int) sizeof(MYFLT)) {
        std::fprintf(stderr, "FATAL: csoundGetSizeOfMYFLT()=%d but sizeof(MYFLT)=%d at compile time "
                              "(header/library ABI mismatch -- spout would be read at the wrong width).\n",
                      runtime, (int) sizeof(MYFLT));
        return false;
    }
    return true;
}

// ═══════════════════════════ CSD text (Section A+B, RT probe) ═══════════════════════════
//
// Fix #1 applied (see header comment): `z` -> `3600` in all 8 score lines.
static const char* kRtProbeCsd = R"CSD(<CsoundSynthesizer>
<CsOptions>
-n -d
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 64
nchnls = 1
0dbfs = 1

; One always-running voice, gated + tuned via named channels ("gateN"/"freqN").
; Note-on/off = channel writes from the host. NO runtime i-events ever.
instr 1
  ivoice = p4
  Sg sprintf "gate%d", ivoice
  Sf sprintf "freq%d", ivoice
  kgate chnget Sg
  kfreq chnget Sf
  kfreq limit kfreq, 20, 12000
  kenv  portk kgate, 0.008           ; declick the gate
  a1 oscili 0.20*kenv, kfreq
  a2 oscili 0.15*kenv, kfreq*1.71
  a3 oscili 0.12*kenv, kfreq*2.74
  a4 oscili 0.10*kenv, kfreq*3.76
  a5 oscili 0.08*kenv, kfreq*5.43
  a6 oscili 0.06*kenv, kfreq*6.98
  a7 oscili 0.05*kenv, kfreq*8.21
  a8 oscili 0.04*kenv, kfreq*9.85
  out (a1+a2+a3+a4+a5+a6+a7+a8)*0.125
endin
</CsInstruments>
<CsScore>
i 1 0 3600 1
i 1 0 3600 2
i 1 0 3600 3
i 1 0 3600 4
i 1 0 3600 5
i 1 0 3600 6
i 1 0 3600 7
i 1 0 3600 8
e 3600
</CsScore>
</CsoundSynthesizer>
)CSD";

// ═══════════════════════════ CSD text (Section C, musical gate) ═══════════════════════════

// C1: per-partial decays + drift + beating pair. Given verbatim except Fix #2
// (score p1: "Partial" -> 1; orchestra's `instr Partial` name is untouched).
static const char* kC1Csd = R"CSD(<CsoundSynthesizer>
<CsOptions>
-n -d
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 64
nchnls = 1
0dbfs = 1
giBase = 220

; ONE inharmonic partial. The score line IS the partial list — readable sound.
;         p4=amp  p5=ratio  p6=decaySec  p7=driftCents(over p3)
instr Partial
  kcents line   0, p3, p7
  kfreq  =      giBase * p5 * cent(kcents)
  kenv   transeg 0, 0.002, 0, 1, p6, -6, 0.0001
  asig   oscili p4 * kenv, kfreq
         out    asig
endin
</CsInstruments>
<CsScore>
;                amp    ratio   dec   drift
i 1 0 9  0.28   0.56    8.0   -4
i 1 0 9  0.30   1.00    7.0    0
i 1 0 9  0.22   1.19    5.5    3
i 1 0 9  0.20   1.71    5.0   -5
i 1 0 9  0.16   2.00    4.0    2
i 1 0 9  0.16   2.007   4.0   -2
i 1 0 9  0.14   2.74    2.8    6
i 1 0 9  0.11   3.76    2.0   -7
i 1 0 9  0.09   4.07    1.5    5
i 1 0 9  0.07   5.43    1.0   -8
i 1 0 9  0.05   6.98    0.7    9
i 1 0 9  0.04   8.21    0.5  -10
e
</CsScore>
</CsoundSynthesizer>
)CSD";

// C2: spectrum continuously crossing (spectral crossfade IN TIME, not via
// scan) + independent slow vibrati. Instrument body given verbatim; score DATA
// composed from the spec's prose (10 partials, ratios as given; odd-indexed
// (1-based: 1,3,5,7,9) fade 0.20->0.02, even-indexed fade 0.02->0.20; vibHz
// linearly spread 0.07..0.5 and vibCents linearly spread 3..12 across the 10
// partials in ratio-list order — all distinct, as specified).
static const char* kC2Csd = R"CSD(<CsoundSynthesizer>
<CsOptions>
-n -d
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 64
nchnls = 1
0dbfs = 1
giBase = 220

; Spectrum continuously crossing (spectral crossfade IN TIME, not via scan) +
; independent slow vibrati per partial.
;         p4=ampStart p5=ampEnd p6=ratio p7=vibHz p8=vibCents
instr PadPartial
  kamp  line   p4, p3, p5
  kvib  oscili p8, p7
  kfreq =      giBase * p6 * cent(kvib)
  kwin  linseg 0, 0.8, 1, p3-1.6, 1, 0.8, 0
  asig  oscili kamp * kwin, kfreq
        out    asig
endin
</CsInstruments>
<CsScore>
;             ampStart ampEnd ratio  vibHz   vibCents
i 1 0 12      0.20     0.02   1.00   0.0700  3
i 1 0 12      0.02     0.20   1.19   0.1178  4
i 1 0 12      0.20     0.02   1.71   0.1656  5
i 1 0 12      0.02     0.20   2.007  0.2133  6
i 1 0 12      0.20     0.02   2.74   0.2611  7
i 1 0 12      0.02     0.20   3.76   0.3089  8
i 1 0 12      0.20     0.02   4.07   0.3567  9
i 1 0 12      0.02     0.20   5.43   0.4044  10
i 1 0 12      0.20     0.02   6.98   0.4522  11
i 1 0 12      0.02     0.20   8.21   0.5000  12
e
</CsScore>
</CsoundSynthesizer>
)CSD";

// C3: frequency-space morph harmonic -> bell -> harmonic. Instrument body
// given verbatim; score DATA composed 1:1 from the spec's harmonic/bell/amp
// lists (index i pairs harmonic[i] with bell[i] and amps[i]).
static const char* kC3Csd = R"CSD(<CsoundSynthesizer>
<CsOptions>
-n -d
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 64
nchnls = 1
0dbfs = 1
giBase = 220

; Frequency-space morph harmonic -> bell -> harmonic. Partials MOVE in
; frequency space, not just amplitude — impossible for a fixed-frame float-h
; engine.
;         p4=amp p5=ratioHarmonic p6=ratioBell
instr GlidePartial
  kratio linseg p5, p3*0.4, p6, p3*0.2, p6, p3*0.4, p5
  kfreq  =      giBase * kratio
  kwin   linseg 0, 0.05, 1, p3-0.1, 1, 0.05, 0
  asig   oscili p4 * kwin, kfreq
         out    asig
endin
</CsInstruments>
<CsScore>
;           amp   ratioHarm  ratioBell
i 1 0 8    0.30   1          1.00
i 1 0 8    0.22   2          1.71
i 1 0 8    0.16   3          2.74
i 1 0 8    0.12   4          3.76
i 1 0 8    0.09   5          5.43
i 1 0 8    0.07   6          6.98
i 1 0 8    0.05   7          8.21
i 1 0 8    0.04   8          9.85
e
</CsScore>
</CsoundSynthesizer>
)CSD";

// C4: C1's bell strike into C2's living sustain. NOT given verbatim by the
// spec (only a prose description) -- composed from C1/C2's own primitives:
//   - kstrike: C1's exact transeg strike-decay (fast decay to a -80dB floor).
//   - kswell:  a linseg that stays near-silent while the strike is still
//              audible (iswellStart = p6*0.8), then swells up to a quiet
//              "living bed" level (p9) over the remaining note, and tapers
//              back to 0 over the final 0.3s for a click-free note-off.
//   - kvib/kfreq: C2's per-partial vibrato, applied to the same ratio table.
// Score reuses C1's 12-partial amp/ratio/decay columns (same bell material),
// adding a vibHz/vibCents spread (as C2's) and a bedLevel = amp*0.35 per row.
static const char* kC4Csd = R"CSD(<CsoundSynthesizer>
<CsOptions>
-n -d
</CsOptions>
<CsInstruments>
sr = 48000
ksmps = 64
nchnls = 1
0dbfs = 1
giBase = 220

; C1's bell strike into C2's living sustain: fast strike-decay (transeg, as in
; C1's Partial) PLUS a slow re-swell to a quiet living bed (linseg) with
; independent per-partial vibrato (as in C2's PadPartial).
;         p4=amp p5=ratio p6=strikeDecaySec p7=vibHz p8=vibCents p9=bedLevel
instr StrikePad
  kstrike  transeg 0, 0.002, 0, 1, p6, -6, 0.0001
  iswellStart = p6*0.8
  iswellMid   = p3 - iswellStart - 0.3
  kswell   linseg 0, iswellStart, 0, iswellMid, p9, 0.3, 0
  kvib     oscili p8, p7
  kfreq    = giBase * p5 * cent(kvib)
  asig     oscili p4*kstrike + p4*kswell, kfreq
           out    asig
endin
</CsInstruments>
<CsScore>
;           amp   ratio    strikeDec  vibHz    vibCents  bedLevel
i 1 0 10   0.28   0.56     8.0        0.0700   3.00      0.098
i 1 0 10   0.30   1.00     7.0        0.1091   3.82      0.105
i 1 0 10   0.22   1.19     5.5        0.1482   4.64      0.077
i 1 0 10   0.20   1.71     5.0        0.1873   5.45      0.070
i 1 0 10   0.16   2.00     4.0        0.2264   6.27      0.056
i 1 0 10   0.16   2.007    4.0        0.2655   7.09      0.056
i 1 0 10   0.14   2.74     2.8        0.3045   7.91      0.049
i 1 0 10   0.11   3.76     2.0        0.3436   8.73      0.0385
i 1 0 10   0.09   4.07     1.5        0.3827   9.55      0.0315
i 1 0 10   0.07   5.43     1.0        0.4218  10.36      0.0245
i 1 0 10   0.05   6.98     0.7        0.4609  11.18      0.0175
i 1 0 10   0.04   8.21     0.5        0.5000  12.00      0.0140
e
</CsScore>
</CsoundSynthesizer>
)CSD";

// ═══════════════════════════ Section A+B: RT probe ═══════════════════════════

struct RtProbeResult {
    bool ok = true;
    std::string errorMsg;
    long long deltaBlocks = 0;
    long long deltaBytes = 0;
    TimingStats stats1v, stats8v;
    int sizeOfMyflt = 0;
    int csoundVersion = 0;
};

static RtProbeResult runRtProbe(const std::string& csdText)
{
    RtProbeResult r;
    r.csoundVersion = csoundGetVersion();

    CSOUND* cs = csoundCreate(nullptr);
    if (cs == nullptr) { r.ok = false; r.errorMsg = "csoundCreate returned null"; return r; }
    csoundCreateMessageBuffer(cs, 0); // toStdOut=0: queue only, print nothing
    csoundSetOption(cs, "-n");
    csoundSetOption(cs, "-d");

    int rc = csoundCompileCsdText(cs, csdText.c_str());
    if (rc != 0) {
        r.ok = false; r.errorMsg = "csoundCompileCsdText failed, rc=" + std::to_string(rc);
        std::fprintf(stderr, "Csound messages:\n%s\n", drainCsoundMessages(cs).c_str());
        csoundDestroyMessageBuffer(cs); csoundDestroy(cs); return r;
    }
    rc = csoundStart(cs);
    if (rc != 0) {
        r.ok = false; r.errorMsg = "csoundStart failed, rc=" + std::to_string(rc);
        std::fprintf(stderr, "Csound messages:\n%s\n", drainCsoundMessages(cs).c_str());
        csoundDestroyMessageBuffer(cs); csoundDestroy(cs); return r;
    }

    r.sizeOfMyflt = csoundGetSizeOfMYFLT();
    if (!checkMyfltSize()) {
        r.ok = false; r.errorMsg = "MYFLT size mismatch";
        csoundDestroyMessageBuffer(cs); csoundDestroy(cs); return r;
    }

    const double sr = (double) csoundGetSr(cs);
    const uint32_t ksmps = csoundGetKsmps(cs);
    const uint32_t nchnls = csoundGetNchnls(cs);

    // Precompute every channel name ONCE (before any measurement): building
    // std::strings inside the measured loop would be a host-side allocation
    // risk the whole probe exists to rule out. index 0 unused, voices 1..8.
    std::array<std::string, 9> gateNames, freqNames;
    for (int v = 1; v <= 8; ++v) {
        gateNames[(size_t) v] = "gate" + std::to_string(v);
        freqNames[(size_t) v] = "freq" + std::to_string(v);
    }

    // Step 1: preallocate ALL host buffers (20s reserve) before any measurement.
    const size_t capacity = (size_t) std::llround(20.0 * sr) + (size_t) ksmps * 8;
    std::vector<float> collectBuf(capacity, 0.0f);
    size_t collectPos = 0;

    auto failCleanup = [&]() {
        std::fprintf(stderr, "Csound messages:\n%s\n", drainCsoundMessages(cs).c_str());
        csoundDestroyMessageBuffer(cs);
        csoundDestroy(cs);
    };

    auto performOne = [&](std::vector<double>* bucket) -> bool {
        struct timespec t0{}, t1{};
        clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
        const int fin = csoundPerformKsmps(cs);
        MYFLT* spout = csoundGetSpout(cs);
        for (uint32_t s = 0; s < ksmps; ++s)
            collectBuf[collectPos + s] = (float) spout[(size_t) s * nchnls];
        collectPos += ksmps;
        clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
        if (bucket) bucket->push_back(elapsedUs(t0, t1));
        if (fin != 0) {
            r.ok = false;
            r.errorMsg = "csoundPerformKsmps returned nonzero unexpectedly (score end reached "
                         "prematurely or an internal error) during the RT probe";
            return false;
        }
        return true;
    };

    // Step 2: warm-up, 1s. Gate every voice on once (freqs 110..880, geometric
    // spread across 3 octaves), then off -- first gate-on may lazily allocate
    // inside opcodes, which is exactly why steady state is measured separately.
    const long warmupBlocks = (long) std::llround(1.0 * sr / ksmps);
    for (int v = 1; v <= 8; ++v) {
        const double freq = 110.0 * std::pow(8.0, (double) (v - 1) / 7.0);
        csoundSetControlChannel(cs, gateNames[(size_t) v].c_str(), 1.0);
        csoundSetControlChannel(cs, freqNames[(size_t) v].c_str(), freq);
    }
    for (long i = 0; i < warmupBlocks / 2; ++i)
        if (!performOne(nullptr)) { failCleanup(); return r; }
    for (int v = 1; v <= 8; ++v)
        csoundSetControlChannel(cs, gateNames[(size_t) v].c_str(), 0.0);
    for (long i = 0; i < warmupBlocks - warmupBlocks / 2; ++i)
        if (!performOne(nullptr)) { failCleanup(); return r; }

    // Step 3: snapshot malloc stats (whole-process metric -- hence step 1).
    const malloc_statistics_t before = snapshotMalloc();

    // Step 4a: 10s measured perform. Every 0.5s toggle one gate in a fixed
    // round-robin pattern (1,2,...,8,1,2,...) and sweep every active voice's
    // freq channel (deterministic ramp 110->880Hz over the pass). Time EVERY
    // csoundPerformKsmps+csoundGetSpout+copy. The round-robin ramps the active
    // count 0->8->0->... so it naturally passes through an all-8-active window
    // (exactly one 0.5s window, right after the 8th toggle) -- that window
    // supplies the @8-voices timing bucket.
    std::array<bool, 9> gateOn{};
    std::vector<double> raw8v;
    const long measuredBlocks = (long) std::llround(10.0 * sr / ksmps);
    const long blocksPerToggle = (long) std::llround(0.5 * sr / ksmps);
    int nextToggleVoice = 1;
    for (long i = 0; i < measuredBlocks; ++i) {
        if (i > 0 && (i % blocksPerToggle) == 0) {
            gateOn[(size_t) nextToggleVoice] = !gateOn[(size_t) nextToggleVoice];
            csoundSetControlChannel(cs, gateNames[(size_t) nextToggleVoice].c_str(),
                                     gateOn[(size_t) nextToggleVoice] ? 1.0 : 0.0);
            nextToggleVoice = (nextToggleVoice % 8) + 1;
        }
        const double tSec = (double) i * (double) ksmps / sr;
        const double freqNow = 110.0 * std::pow(8.0, std::min(1.0, tSec / 10.0));
        int activeCount = 0;
        for (int v = 1; v <= 8; ++v) {
            if (gateOn[(size_t) v]) {
                csoundSetControlChannel(cs, freqNames[(size_t) v].c_str(), freqNow);
                ++activeCount;
            }
        }
        std::vector<double>* bucket = (activeCount == 8) ? &raw8v : nullptr;
        if (!performOne(bucket)) { failCleanup(); return r; }
    }

    // Step 4b: dedicated clean 2s pass with only voice 1 gated, for the
    // 1-voice number (a longer, transient-free sample than any incidental
    // 1-voice window inside the round-robin pass above).
    for (int v = 1; v <= 8; ++v)
        csoundSetControlChannel(cs, gateNames[(size_t) v].c_str(), (v == 1) ? 1.0 : 0.0);
    std::vector<double> raw1v;
    const long oneVoiceBlocks = (long) std::llround(2.0 * sr / ksmps);
    for (long i = 0; i < oneVoiceBlocks; ++i) {
        const double tSec = (double) i * (double) ksmps / sr;
        const double freqNow = 110.0 * std::pow(8.0, std::min(1.0, tSec / 2.0));
        csoundSetControlChannel(cs, freqNames[1].c_str(), freqNow);
        if (!performOne(&raw1v)) { failCleanup(); return r; }
    }
    csoundSetControlChannel(cs, gateNames[1].c_str(), 0.0);

    // Step 5: snapshot again.
    const malloc_statistics_t after = snapshotMalloc();
    r.deltaBlocks = (long long) after.blocks_in_use - (long long) before.blocks_in_use;
    r.deltaBytes  = (long long) after.size_in_use   - (long long) before.size_in_use;
    r.stats1v = computeStats(raw1v);
    r.stats8v = computeStats(raw8v);

    csoundDestroyMessageBuffer(cs);
    csoundDestroy(cs);
    return r;
}

// ═══════════════════════════ Section C: musical gate (Csound renders) ═══════════════════════════

struct RenderResult {
    bool ok = true;
    std::string err;
    std::vector<float> mono;
    double prePeak = 0.0, preRms = 0.0;
};

// One fresh CSOUND* per render (create -> message buffer -> compile -> start
// -> perform -> destroy buffer -> destroy instance), mirroring EXACTLY the one
// lifecycle pattern proven fully clean by the RT probe (Section A+B): a single
// create/destroy pair with no csoundReset() in between. Deviation from the
// spec's literal suggestion to "csoundReset/csoundDestroy between sections":
// empirically, on this Csound build, csoundReset() interacts unpredictably
// with csoundCreateMessageBuffer's console-suppression (its own
// "resetting Csound instance" + prior-performance-summary text leaks to the
// console regardless of whether the buffer is (re)created before or after the
// reset call -- tried both orderings, each leaked a different subset). A
// fresh instance per render sidesteps the interaction entirely and costs
// nothing at this scale (four ~10s offline renders); the message-buffer
// suppression works perfectly on a create-once/destroy-once instance, exactly
// as it does for the RT probe.
static RenderResult renderCsdOffline(const std::string& csdText, double durationSec)
{
    RenderResult r;
    CSOUND* cs = csoundCreate(nullptr);
    if (cs == nullptr) { r.ok = false; r.err = "csoundCreate returned null"; return r; }
    csoundCreateMessageBuffer(cs, 0); // toStdOut=0: queue only, print nothing
    auto finish = [&]() {
        if (!r.ok) std::fprintf(stderr, "Csound messages:\n%s\n", drainCsoundMessages(cs).c_str());
        csoundDestroyMessageBuffer(cs);
        csoundDestroy(cs);
    };

    csoundSetOption(cs, "-n");
    csoundSetOption(cs, "-d");

    int rc = csoundCompileCsdText(cs, csdText.c_str());
    if (rc != 0) {
        r.ok = false; r.err = "csoundCompileCsdText failed, rc=" + std::to_string(rc);
        finish(); return r;
    }
    rc = csoundStart(cs);
    if (rc != 0) {
        r.ok = false; r.err = "csoundStart failed, rc=" + std::to_string(rc);
        finish(); return r;
    }
    if (!checkMyfltSize()) { r.ok = false; r.err = "MYFLT size mismatch"; finish(); return r; }

    const double sr = (double) csoundGetSr(cs);
    const uint32_t ksmps = csoundGetKsmps(cs);
    const uint32_t nchnls = csoundGetNchnls(cs);

    const long targetSamples = (long) std::llround(durationSec * sr);
    const long maxBlocks = (long) std::ceil((durationSec + 2.0) * sr / (double) ksmps); // 2s safety slack
    r.mono.reserve((size_t) targetSamples + (size_t) ksmps * 4);

    for (long i = 0; i < maxBlocks; ++i) {
        const int fin = csoundPerformKsmps(cs);
        MYFLT* spout = csoundGetSpout(cs);
        for (uint32_t s = 0; s < ksmps; ++s)
            r.mono.push_back((float) spout[(size_t) s * nchnls]);
        if (fin != 0) {
            const double gotSec = (double) r.mono.size() / sr;
            if (gotSec < durationSec * 0.9) {
                r.ok = false;
                r.err = "premature end at " + std::to_string(gotSec) + "s (expected ~"
                      + std::to_string(durationSec) + "s)";
            }
            break;
        }
    }
    if (!r.ok) { finish(); return r; }

    // Trim/pad to exactly the specified duration.
    if ((long) r.mono.size() > targetSamples) r.mono.resize((size_t) targetSamples);
    else if ((long) r.mono.size() < targetSamples) r.mono.resize((size_t) targetSamples, 0.0f);
    finish();

    const PeakRms pre = measurePeakRms(r.mono);
    r.prePeak = pre.peak; r.preRms = pre.rms;
    return r;
}

// ═══════════════════════════ Section C5/C6: current B path (WavetableOscillator) ═══════════════════════════

// C1's 12-partial bell set, reused verbatim (h, a, phase order per AdditivePartial).
static std::vector<WavetableOscillator::AdditivePartial> makeC1PartialBank()
{
    return {
        { 0.56f,  0.28f, 0.0f },
        { 1.00f,  0.30f, 0.0f },
        { 1.19f,  0.22f, 0.0f },
        { 1.71f,  0.20f, 0.0f },
        { 2.00f,  0.16f, 0.0f },
        { 2.007f, 0.16f, 0.0f },
        { 2.74f,  0.14f, 0.0f },
        { 3.76f,  0.11f, 0.0f },
        { 4.07f,  0.09f, 0.0f },
        { 5.43f,  0.07f, 0.0f },
        { 6.98f,  0.05f, 0.0f },
        { 8.21f,  0.04f, 0.0f },
    };
}

// b_current_bell.wav: ONE station (the full C1 partial set), static (no scan
// movement) -- B's baseline. The global exponential decay applied in main()
// (NOT here) is B's limitation made explicit: one envelope for the whole mix,
// no per-partial decay/drift like C1's transeg+line per partial.
static std::vector<float> renderBCurrentBell(double sr, int block, double durationSec)
{
    WavetableOscillator osc;
    osc.setAdditiveBank(makeC1PartialBank());
    osc.setFrequency(220.0f);
    osc.prepare(sr, block);
    const size_t n = (size_t) std::llround(durationSec * sr);
    std::vector<float> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = osc.processSample();
    return out;
}

// b_current_blend.wav: two stations -- A = full C1 set, B = only the 4 lowest
// partials (ratios 0.56/1.00/1.19/1.71, the "dark" end) with the other 8
// zeroed -- swept via setScanPosition 0->1 linearly over the render, updated
// once per block (block = 64, matching ksmps elsewhere in this tool). This is
// B's best available "Verlaufsform": an amplitude-blend between two fixed
// spectra, never a frequency-space movement (that's C3's point).
static std::vector<float> renderBCurrentBlend(double sr, int block, double durationSec)
{
    WavetableOscillator osc;
    auto stationA = makeC1PartialBank();
    auto stationB = stationA;
    for (size_t i = 4; i < stationB.size(); ++i) stationB[i].a = 0.0f; // keep only the 4 lowest
    osc.setAdditiveBank(std::vector<std::vector<WavetableOscillator::AdditivePartial>>{ stationA, stationB });
    osc.setFrequency(220.0f);
    osc.prepare(sr, block);

    const size_t n = (size_t) std::llround(durationSec * sr);
    std::vector<float> out(n);
    const int numBlocks = (int) (n / (size_t) block);
    size_t idx = 0;
    for (int b = 0; b < numBlocks; ++b) {
        const double frac = (numBlocks > 1) ? ((double) b / (double) (numBlocks - 1)) : 0.0;
        osc.setScanPosition((float) frac);
        for (int s = 0; s < block; ++s) out[idx++] = osc.processSample();
    }
    while (idx < n) out[idx++] = osc.processSample(); // remainder, if n isn't an exact block multiple
    return out;
}

// ═══════════════════════════ main ═══════════════════════════

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit; // safe init for any JUCE statics

    const std::string outDir = "/Users/joerissen/ai/t5ynth/tools/csound_poc_out/";
    std::filesystem::create_directories(outDir);

    bool overallOk = true;
    std::string metrics;
    auto M = [&](const std::string& line) { metrics += line; metrics += "\n"; };

    printf("Csound Kill-Gate PoC (Homebrew Csound 6.18, CsoundLib64.framework)\n");

    // ── Section A+B: RT probe (LINK / RT-PATTERN / CPU verdicts) ──
    writeTextFile(outDir + "rt_probe.csd", kRtProbeCsd);
    const RtProbeResult rt = runRtProbe(kRtProbeCsd);

    if (!rt.ok) {
        // runRtProbe's own failCleanup() already drained Csound's message
        // buffer to stderr before returning.
        printf("LINK: FAIL - %s\n", rt.errorMsg.c_str());
        return 1;
    }
    printf("LINK: OK - CsoundLib64 linked, compiled, started, and performed the RT-probe CSD. "
           "csoundGetVersion()=%d, sizeof(MYFLT)=%d (csoundGetSizeOfMYFLT()=%d, match)\n",
           rt.csoundVersion, (int) sizeof(MYFLT), rt.sizeOfMyflt);

    if (rt.deltaBlocks == 0) {
        printf("RT-PATTERN: CLEAN (0 block growth warmup->end; delta bytes=%lld)\n", rt.deltaBytes);
    } else {
        printf("RT-PATTERN: NOT CLEAN - delta blocks=%lld, delta bytes=%lld (warmup->end; reported honestly)\n",
               rt.deltaBlocks, rt.deltaBytes);
    }

    const double budgetUs = 1000000.0 * 64.0 / 48000.0; // ksmps=64 @ 48kHz = 1333.33us
    printf("CPU: @1 voice  (n=%4zu) median=%6.2fus (%5.1f%%)  p99=%6.2fus (%5.1f%%)  max=%6.2fus (%5.1f%%) of %.2fus budget\n",
           rt.stats1v.n, rt.stats1v.medianUs, 100.0 * rt.stats1v.medianUs / budgetUs,
           rt.stats1v.p99Us, 100.0 * rt.stats1v.p99Us / budgetUs,
           rt.stats1v.maxUs, 100.0 * rt.stats1v.maxUs / budgetUs, budgetUs);
    printf("CPU: @8 voices (n=%4zu) median=%6.2fus (%5.1f%%)  p99=%6.2fus (%5.1f%%)  max=%6.2fus (%5.1f%%) of %.2fus budget\n",
           rt.stats8v.n, rt.stats8v.medianUs, 100.0 * rt.stats8v.medianUs / budgetUs,
           rt.stats8v.p99Us, 100.0 * rt.stats8v.p99Us / budgetUs,
           rt.stats8v.maxUs, 100.0 * rt.stats8v.maxUs / budgetUs, budgetUs);

    M("=== Csound Kill-Gate PoC metrics ===");
    M("csoundGetVersion() = " + std::to_string(rt.csoundVersion));
    M("sizeof(MYFLT) compile-time = " + std::to_string((int) sizeof(MYFLT))
      + " ; csoundGetSizeOfMYFLT() runtime = " + std::to_string(rt.sizeOfMyflt));
    M("");
    M("--- RT probe (Section A+B): malloc deltas, warmup snapshot -> end snapshot ---");
    M("delta blocks_in_use = " + std::to_string(rt.deltaBlocks));
    M("delta size_in_use (bytes) = " + std::to_string(rt.deltaBytes));
    M("RT-PATTERN verdict: " + std::string(rt.deltaBlocks == 0 ? "CLEAN" : "NOT CLEAN (see deltas above)"));
    M("");
    M("--- RT probe timing (block budget @ ksmps=64/48kHz = 1333.33us) ---");
    char lineBuf[512];
    std::snprintf(lineBuf, sizeof(lineBuf),
        "@1 voice  n=%zu  median=%.3fus (%.2f%%)  p99=%.3fus (%.2f%%)  max=%.3fus (%.2f%%)",
        rt.stats1v.n, rt.stats1v.medianUs, 100.0*rt.stats1v.medianUs/budgetUs,
        rt.stats1v.p99Us, 100.0*rt.stats1v.p99Us/budgetUs, rt.stats1v.maxUs, 100.0*rt.stats1v.maxUs/budgetUs);
    M(lineBuf);
    std::snprintf(lineBuf, sizeof(lineBuf),
        "@8 voices n=%zu  median=%.3fus (%.2f%%)  p99=%.3fus (%.2f%%)  max=%.3fus (%.2f%%)",
        rt.stats8v.n, rt.stats8v.medianUs, 100.0*rt.stats8v.medianUs/budgetUs,
        rt.stats8v.p99Us, 100.0*rt.stats8v.p99Us/budgetUs, rt.stats8v.maxUs, 100.0*rt.stats8v.maxUs/budgetUs);
    M(lineBuf);
    M("");
    M("--- Section C renders (pre-normalization peak/RMS; all six normalized to -12 dBFS peak) ---");

    // ── Section C: musical gate renders ──
    struct RenderSpec { const char* wavName; const char* csdName; const char* csdText; double durationSec; };
    const RenderSpec specs[4] = {
        { "csound_bell_perpartial.wav", "csound_bell_perpartial.csd", kC1Csd, 9.0 },
        { "csound_pad_evolving.wav",    "csound_pad_evolving.csd",    kC2Csd, 12.0 },
        { "csound_glide_partials.wav",  "csound_glide_partials.csd",  kC3Csd, 8.0 },
        { "csound_strike_pad.wav",      "csound_strike_pad.csd",      kC4Csd, 10.0 },
    };

    // renderCsdOffline() owns its own CSOUND* create/destroy cycle per call
    // (see its header comment for why: one instance per render, matching the
    // RT probe's proven-clean create-once/destroy-once lifecycle).
    for (const auto& spec : specs) {
        writeTextFile(outDir + spec.csdName, spec.csdText);
        RenderResult rr = renderCsdOffline(spec.csdText, spec.durationSec);
        if (!rr.ok) {
            printf("%-28s FAIL - %s\n", spec.wavName, rr.err.c_str());
            overallOk = false;
            continue;
        }
        const double gain = normalizeToDbfs(rr.mono, -12.0);
        writeWav(outDir + spec.wavName, rr.mono, 48000);
        printf("%-28s dur=%5.2fs  prePeak=%.4f preRMS=%.4f  normGain=%+.2fdB -> OK\n",
               spec.wavName, spec.durationSec, rr.prePeak, rr.preRms, 20.0 * std::log10(gain));
        std::snprintf(lineBuf, sizeof(lineBuf), "%-28s dur=%5.2fs  prePeak=%.4f  preRMS=%.4f  normGain=%+.2fdB",
                      spec.wavName, spec.durationSec, rr.prePeak, rr.preRms, 20.0 * std::log10(gain));
        M(lineBuf);
    }

    // ── Section C5/C6: current B path (WavetableOscillator, real-time additive) ──
    const double sr = 48000.0;
    const int block = 64;

    {
        auto mono = renderBCurrentBell(sr, block, 9.0);
        applyGlobalExpDecay(mono, sr, 7.0, -60.0); // B's limitation: ONE global envelope, no per-partial decay
        const PeakRms pre = measurePeakRms(mono);
        const double gain = normalizeToDbfs(mono, -12.0);
        writeWav(outDir + "b_current_bell.wav", mono, 48000);
        printf("%-28s dur=%5.2fs  prePeak=%.4f preRMS=%.4f  normGain=%+.2fdB -> OK\n",
               "b_current_bell.wav", 9.0, pre.peak, pre.rms, 20.0 * std::log10(gain));
        std::snprintf(lineBuf, sizeof(lineBuf),
                      "%-28s dur=%5.2fs  prePeak=%.4f  preRMS=%.4f  normGain=%+.2fdB  "
                      "(post global -60dB/7s decay, pre-final-normalize)",
                      "b_current_bell.wav", 9.0, pre.peak, pre.rms, 20.0 * std::log10(gain));
        M(lineBuf);
    }
    {
        auto mono = renderBCurrentBlend(sr, block, 9.0);
        applyGlobalExpDecay(mono, sr, 7.0, -60.0);
        const PeakRms pre = measurePeakRms(mono);
        const double gain = normalizeToDbfs(mono, -12.0);
        writeWav(outDir + "b_current_blend.wav", mono, 48000);
        printf("%-28s dur=%5.2fs  prePeak=%.4f preRMS=%.4f  normGain=%+.2fdB -> OK\n",
               "b_current_blend.wav", 9.0, pre.peak, pre.rms, 20.0 * std::log10(gain));
        std::snprintf(lineBuf, sizeof(lineBuf),
                      "%-28s dur=%5.2fs  prePeak=%.4f  preRMS=%.4f  normGain=%+.2fdB  "
                      "(post global -60dB/7s decay, pre-final-normalize)",
                      "b_current_blend.wav", 9.0, pre.peak, pre.rms, 20.0 * std::log10(gain));
        M(lineBuf);
    }

    writeTextFile(outDir + "metrics.txt", metrics);

    if (!rt.ok) overallOk = false;
    printf("%s\n", overallOk ? "ALL OK" : "*** FAIL ***");
    return overallOk ? 0 : 1;
}
