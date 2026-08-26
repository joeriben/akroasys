// Processor-level round-trip guard for Csound orchestra preset persistence
// (SPEC_phase4_5_csound_llm_preset.md, Phase 5). Mirrors
// tools/test_preset_coordination_roundtrip.cpp exactly: constructs the REAL
// T5ynthProcessor, drives it through exportJsonPreset()/importJsonPreset()
// (the .t5p JSON path PresetFormat::saveToFile/loadFromFile wrap), and
// asserts:
//
//   1. A preset saved in a non-Csound engine mode carries NO
//      csound_orchestra/csound_reading keys at all (the "absence is the
//      switch" convention exportJsonPreset uses elsewhere).
//   2. Forcing Csound mode + requestCsoundOrchestra() + setCsoundReading(),
//      then exporting AFTER the compile settles, writes the EXACT orchestra
//      text and reading into the "engine" block.
//   3. Importing that JSON into a FRESH processor instance restores engine
//      mode == Csound, getCsoundReading() == the saved reading, AND the
//      orchestra actually becomes the LIVE, READY engine — exercising the
//      Phase-5 "no ready active engine -> instant adopt without fade" swap
//      path (handleAsyncUpdate's instant-adopt branch), with NO
//      processBlock call needed. This is the one case Phase 2's swap logic
//      did not originally cover — a fresh instance's active Csound engine is
//      never isReady() before its first request.
//   4. Re-exporting the freshly-loaded processor reproduces the same
//      orchestra/reading (a full round trip), proving the restore is a real
//      compile, not just a text pass-through — if handleAsyncUpdate never
//      actually ran, csoundEngines_[activeIdx].orchestraText() would still
//      be the untouched built-in text and this check would genuinely fail
//      (unlike the csoundCompileError()-empty check alone, which defaults to
//      empty even if nothing ever ran — this is why comparisons use exact
//      equality against the settled JSON's parsed field, not just "no
//      error").
//
// Pumping note: requestCsoundOrchestra()/importJsonPreset() only QUEUE the
// compile (triggerAsyncUpdate()); the actual compile runs in
// T5ynthProcessor::handleAsyncUpdate() on the JUCE message thread. In a
// plain console main() nothing pumps that automatically. Two mac-specific
// dead ends found along the way (kept here as a note so nobody re-treads
// them): (a) MessageManager::runDispatchLoopUntil() is compiled out —
// this project builds with JUCE_MODAL_LOOPS_PERMITTED=0 (a plugin must
// never let a NESTED modal loop block a host); (b)
// MessageManager::runDispatchLoop()/stopDispatchLoop() ARE compiled in, but
// on mac runDispatchLoop() calls straight into `[NSApp run]`
// (juce_MessageManager_mac.mm) — in a bare command-line binary with no
// NSApplication activation/event-loop context, that reliably never services
// the CFRunLoopSource AsyncUpdater's message posts wake (measured: 5 s,
// zero deliveries). What DOES work: JUCE's own mac MessageQueue
// (juce_MessageQueue_mac.h) registers its CFRunLoopSource on
// CFRunLoopGetMain() under kCFRunLoopCommonModes — pumping that run loop
// DIRECTLY via CFRunLoopRunInMode(kCFRunLoopDefaultMode, ...) (no NSApp
// involved at all) services it correctly, confirmed empirically below.
//
// Build (same response-file recipe as audition_csound_swap.cpp, PLUS
// build_clean/libT5ynthData.a — a real T5ynthProcessor's prepareToPlay
// touches BinaryData, e.g. the reverb plate IRs — see
// test_preset_coordination_roundtrip.cpp's own build comment):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   CSOUND_PREFIX=$(brew --prefix csound)
//   CSOUND_FW="$CSOUND_PREFIX/Frameworks"
//   mkdir -p tools/csound_preset_roundtrip_out
//   clang++ -std=c++17 -O2 @/tmp/h.rsp \
//     tools/test_csound_preset_roundtrip.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     build_clean/libT5ynthData.a \
//     -F"$CSOUND_FW" -framework CsoundLib64 -Wl,-rpath,"$CSOUND_FW" \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit \
//     -o tools/csound_preset_roundtrip_out/test_csound_preset_roundtrip
//   tools/csound_preset_roundtrip_out/test_csound_preset_roundtrip
//
// Exits non-zero on failure.
// CoreFoundation.h MUST come before JuceHeader.h: this project's generated
// JuceHeader.h does `using namespace juce;` at global scope, and
// CoreFoundation's MacTypes.h typedefs a global `struct Point` — parsed in
// the other order, that global Point collides with the now-unqualified-
// visible juce::Point ("reference to 'Point' is ambiguous"). No other tool
// in this codebase includes CoreFoundation directly (they only link the
// framework), so nobody had hit this ordering trap before.
#include <CoreFoundation/CoreFoundation.h>
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++failures; } \
                              else { std::printf("  ok:   %s\n", msg); } } while(0)

// A real assembled orchestra + reading (backend/csound_assembler.py's own
// output for a one-layer "sub" tool spec) — NOT hand-invented DSP text: the
// assembler emits the full <CsoundSynthesizer> CSD wrapper (csoundCompileCsdText
// requires it; a bare "instr 1 ... endin" snippet, tried first, compiled with
// no error reported yet never actually loaded — CsoundEngine::orchestraText()
// only latches the new text on FULL compile success, so a silently-rejected
// fake snippet would have made this guard tool assert nothing meaningful).
// Captured verbatim via:
//   .venv/bin/python -c "import backend.csound_assembler as a; \
//     o,r = a.assemble({'layers':[{'tool':'sub'}]}); print(o); print(r)"
//
// The fixture deliberately keeps the pre-2026-08 100 h score (`i 1 0 360000`
// / `e 360000`) — that is what every stored preset of that era contains, and
// prepare() must heal it via substituteScoreLifetime() at compile time while
// the STORED text roundtrips untouched, which is exactly what this tool
// asserts.
static const char* kTestOrchestra = R"CSDTEXT(<CsoundSynthesizer>
<CsOptions>
-n -d
</CsOptions>
<CsInstruments>
sr = %SR%
ksmps = 64
nchnls = 16
0dbfs = 1

; Phase-3 assembled orchestra (backend/csound_assembler.py).
; Voice-bridge prologue is a byte-for-byte mirror of the Phase-1
; built-in orchestra (src/dsp/CsoundEngine.cpp's buildOrchestra()):
; gate = voice ACTIVE (incl. release), never closed on note-off;
; retrigger is the changed2(trig)+reinit/rireturn epoch idiom, never
; a gate edge (a stolen/reused voice never sees gate fall).
instr 1
  ivoice   = p4
  Sgate    sprintf "gate%d", ivoice
  Sfreq    sprintf "freq%d", ivoice
  Svel     sprintf "vel%d", ivoice
  Spres    sprintf "pres%d", ivoice
  Stimb    sprintf "timb%d", ivoice
  Strig    sprintf "trig%d", ivoice

  kgateraw chnget Sgate
  kfreqraw chnget Sfreq
  kvel     chnget Svel
  kpres    chnget Spres
  ktimb    chnget Stimb
  ktrigch  chnget Strig

  kgate    portk kgateraw, 0.008
  kfreq0   portk kfreqraw, 0.008
  kfreq    limit kfreq0, 20, 12000

  ktrig    changed2 ktrigch
  if ktrig == 1 then
    reinit STRIKE
  endif
  STRIKE:
    kLL1_envRaw transeg 0, 0.0040, 0, 1.0000, 2.2000, -6, 0.3000
  rireturn

  ; ---- layer 1: sub 8' · strike ----
  kLL1_freq0 = kfreq * 1.000000
  kLL1_p0freq = kLL1_freq0 * 1.000000
  aLL1_p0 oscili 0.380000, kLL1_p0freq
  aLL1_sum = aLL1_p0
  aLL1_raw = aLL1_sum * kLL1_envRaw
  aLL1_out = aLL1_raw * 0.280000

  ; ---- MPE epilogue (contract, not per-block): kpres -> gentle level
  ; lift, ktimb (+ a touch of kpres) -> brightness lowpass cutoff. Applies
  ; identically regardless of what any individual layer/block does with
  ; pres/timb itself -- every composition gets a living MPE response.
  kmpeCut   = 700 + 6000*ktimb + 1200*kpres
  kmpeCut   limit kmpeCut, 100, 18000
  kmpeMix   = 0.30 + 0.70*ktimb
  kpresGain = 1.0 + 0.412*kpres

  asum      = aLL1_out
  aMpeLp    tone asum, kmpeCut
  aMpeOut   = aMpeLp*(1-kmpeMix) + asum*kmpeMix
  aout      = aMpeOut * kgate * kvel * kpresGain
  outch     ivoice, aout
endin
</CsInstruments>
<CsScore>
i 1 0 360000 1
i 1 0 360000 2
i 1 0 360000 3
i 1 0 360000 4
i 1 0 360000 5
i 1 0 360000 6
i 1 0 360000 7
i 1 0 360000 8
i 1 0 360000 9
i 1 0 360000 10
i 1 0 360000 11
i 1 0 360000 12
i 1 0 360000 13
i 1 0 360000 14
i 1 0 360000 15
i 1 0 360000 16
e 360000
</CsScore>
</CsoundSynthesizer>
)CSDTEXT";
static const char* kTestReading = "sub 8' \xc2\xb7 strike";   // UTF-8 "sub 8' · strike"

// A raw narrow-char literal implicitly converted to juce::String is NOT
// guaranteed UTF-8-correct on this codebase's own house rule (memory note
// feedback_juce_nonascii_strings: "raw literal mojibakes... grep fromUTF8") —
// wrap explicitly rather than rely on the implicit juce::String(const char*)
// conversion for anything past ASCII (the "·" in kTestReading/kTestOrchestra).
static juce::String utf8(const char* s) { return juce::String(juce::CharPointer_UTF8(s)); }

// exportJsonPreset()'s JSON is pretty-printed with escaped newlines inside
// string VALUES (juce::JSON::toString) — a raw substring search for
// kTestOrchestra (which contains real '\n' bytes) against that serialized
// text can never match. Parse the JSON and read the "engine" object's field
// back out (which correctly un-escapes) instead of pattern-matching the
// serialized form.
static juce::String csoundField(const juce::String& json, const char* key)
{
    auto parsed = juce::JSON::parse(json);
    if (auto* root = parsed.getDynamicObject())
        if (auto* engine = root->getProperty("engine").getDynamicObject())
            return engine->getProperty(key).toString();
    return {};
}

// Pumps JUCE's mac message queue directly via its own CFRunLoopSource (see
// the file header comment for why NSApp-based dispatch doesn't work here),
// polling the Csound busy signals between pumps until settled or a bounded
// timeout elapses. Mirrors PromptPanel::pollCsoundCompile's own
// seenBusy+timeout shape (see its declaration comment in
// src/gui/PromptPanel.h) for the same reason: a poll landing before
// handleAsyncUpdate has even started must not be misread as "already done,
// no error".
static void waitForCsoundSettle(T5ynthProcessor& p, double timeoutMs = 5000.0)
{
    const double start = juce::Time::getMillisecondCounterHiRes();
    bool seenBusy = false;
    while (juce::Time::getMillisecondCounterHiRes() - start < timeoutMs)
    {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);   // service one pending source, ~20ms cap
        const bool busy = p.csoundCompileInFlight() || p.csoundSwapPending() || p.csoundSwapFading();
        if (busy)
            seenBusy = true;
        else if (seenBusy)
            return;   // was busy, now isn't — settled
    }
    // Timed out (or the compile was faster than our first poll could ever
    // catch as "busy") — give one last short grace window regardless so a
    // sub-poll-interval-missed compile still has time to land before the
    // caller reads final state.
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.1, true);
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ── 1. A non-Csound preset carries no csound_orchestra/csound_reading ──
    T5ynthProcessor proc;
    // A real host calls BOTH of these; prepareToPlay() alone (what the other
    // tools/*.cpp processor-level tools call) leaves AudioProcessor's own
    // getSampleRate()/getBlockSize() at 0 — harmless for those tools, but the
    // Csound swap-compile launch in handleAsyncUpdate() reads them directly
    // (to survive a live SR/buffer-size change) and silently no-ops its
    // std::thread launch when either reads 0 (found by adding a temporary
    // debug print to the real swap-compile block — CHECK(...)s alone gave no
    // signal that "no compile ever launched" is a different failure from "a
    // launched compile errored").
    proc.setRateAndBufferSizeDetails(48000.0, 512);
    proc.prepareToPlay(48000.0, 512);

    const juce::String nonCsoundJson = proc.exportJsonPreset();
    CHECK(! nonCsoundJson.contains("csound_orchestra"),
          "non-Csound export omits csound_orchestra");
    CHECK(! nonCsoundJson.contains("csound_reading"),
          "non-Csound export omits csound_reading");

    // ── 2. Force Csound mode + author + let the compile settle + export ──
    proc.forceCsoundEngineMode();
    {
        auto* engineParam = proc.getValueTreeState().getParameter(PID::engineMode);
        const int mode = (int) std::lround(engineParam->convertFrom0to1(engineParam->getValue()));
        CHECK(mode == EngineMode::Csound, "forceCsoundEngineMode() sets engineMode == Csound");
    }
    CHECK(proc.requestCsoundOrchestra(utf8(kTestOrchestra)), "requestCsoundOrchestra() accepts the request");
    proc.setCsoundReading(utf8(kTestReading));

    const juce::String csoundJsonBeforeCompile = proc.exportJsonPreset();
    CHECK(csoundJsonBeforeCompile.contains("\"mode\": \"csound\""), "export writes engine mode = csound");
    // Regression (opus adversarial-review finding, post-implementation):
    // exportJsonPreset() used to read the ACTIVE CsoundEngine's compiled
    // orchestraText(), which only catches up to what was just requested
    // AFTER the background compile (and, for a re-bake, the crossfade) has
    // settled -- csoundActiveIdx_ flips only at the fade's end. A Save that
    // lands in that window (exactly like this export, taken BEFORE
    // waitForCsoundSettle below) would have persisted the OLD orchestra
    // paired with the NEW reading (which setCsoundReading() above already
    // updated synchronously). Fixed by reading csoundPendingOrchestraText_
    // instead, which requestCsoundOrchestra() writes synchronously in
    // lockstep with the reading.
    CHECK(csoundField(csoundJsonBeforeCompile, "csound_orchestra") == utf8(kTestOrchestra),
          "export BEFORE the compile/fade settles still carries the just-requested "
          "orchestra text, not a stale active-engine snapshot");
    CHECK(csoundField(csoundJsonBeforeCompile, "csound_reading") == utf8(kTestReading),
          "export BEFORE the compile/fade settles still carries the just-requested reading text");

    waitForCsoundSettle(proc);
    const juce::String csoundJsonSettled = proc.exportJsonPreset();
    CHECK(csoundField(csoundJsonSettled, "csound_orchestra") == utf8(kTestOrchestra),
          "export carries the exact orchestra text");
    CHECK(csoundField(csoundJsonSettled, "csound_reading") == utf8(kTestReading),
          "export carries the exact reading text");
    CHECK(proc.csoundCompileError().isEmpty(), "proc's own compile produced no error");

    // ── 3. Fresh processor, import: instant-adopt (no ready active engine) ──
    T5ynthProcessor proc2;
    proc2.setRateAndBufferSizeDetails(48000.0, 512);
    proc2.prepareToPlay(48000.0, 512);
    CHECK(proc2.importJsonPreset(csoundJsonSettled), "fresh processor imports the csound preset");
    {
        auto* engineParam = proc2.getValueTreeState().getParameter(PID::engineMode);
        const int mode = (int) std::lround(engineParam->convertFrom0to1(engineParam->getValue()));
        CHECK(mode == EngineMode::Csound, "import restores engineMode == Csound");
    }
    CHECK(proc2.getCsoundReading() == utf8(kTestReading),
          "import restores getCsoundReading() to the saved text");

    // Pump so handleAsyncUpdate's Phase-5 instant-adopt path (the swap-compile
    // block's activeReadyBefore == false branch) actually compiles the
    // restored orchestra into the live engine slot — no processBlock call
    // needed, matching a real preset-load-on-a-live-instance (the whole point
    // of the instant-adopt fix: nothing to fade FROM).
    waitForCsoundSettle(proc2);

    CHECK(proc2.csoundCompileError().isEmpty(),
          "restored orchestra compiled with no error");
    CHECK(! proc2.csoundCompileInFlight(), "compile settled (not still in flight)");
    CHECK(! proc2.csoundSwapPending() && ! proc2.csoundSwapFading(),
          "instant-adopt never arms the fade machinery (nothing to fade FROM)");

    // ── 4. Re-export reproduces the same orchestra/reading (real round trip,
    //       not just a text pass-through — see the file header comment) ──
    const juce::String reExported = proc2.exportJsonPreset();
    CHECK(csoundField(reExported, "csound_orchestra") == utf8(kTestOrchestra),
          "re-export after import carries the same orchestra text");
    CHECK(csoundField(reExported, "csound_reading") == utf8(kTestReading),
          "re-export after import carries the same reading text");

    // ── 5. The performance-ended latch reaches the processor accessor ──
    // This is the wire the Reading field's "engine stopped" state hangs on
    // (CsoundEngine latch -> processor accessor); the GUI poll reads exactly
    // this accessor.
    {
        const double sr = 48000.0;
        const int    bs = 512;
        juce::MidiBuffer noteOn, empty;
        noteOn.addEvent(juce::MidiMessage::noteOn(1, 57, 1.0f), 0);   // A3

        // Case A: the healthy fixture never reports ended.
        T5ynthProcessor procA;
        procA.setRateAndBufferSizeDetails(sr, bs);
        procA.prepareToPlay(sr, bs);
        procA.forceCsoundEngineMode();
        CHECK(procA.requestCsoundOrchestra(utf8(kTestOrchestra)),
              "Case A: requestCsoundOrchestra() accepts the healthy fixture");
        waitForCsoundSettle(procA);
        // The request lands via INSTANT ADOPT, not a fade — measured (the
        // whole binary settles in under a second): forceCsoundEngineMode()
        // and requestCsoundOrchestra() both run before anything pumps the
        // message loop, so handleAsyncUpdate's swapAlreadyQueued suppresses
        // the built-in bootstrap, the active engine is never isReady()
        // beforehand, and the compile lands in place. 64 blocks is simply a
        // generous bound (even the never-taken fade path would need ~19).
        {
            juce::AudioBuffer<float> buf(2, bs);
            for (int b = 0; b < 64; ++b)
            {
                buf.clear();
                procA.processBlock(buf, b == 0 ? noteOn : empty);
            }
        }
        CHECK(! procA.csoundPerformanceEnded(), "healthy orchestra does not report ended");

        // Case B: the same fixture with its score SHORTENED to 0.25 s — the
        // original 100 h failure shape in miniature, and deliberately NOT one
        // of the two historical statement shapes substituteScoreLifetime()
        // rewrites, so the heal leaves it alone and the performance genuinely
        // ends (during prepare()'s own ~1.2 s warmup already; renderUpTo()
        // then latches on its very first call).
        //
        // Why not end it from INSIDE the orchestra: with the healed eternal
        // instances plus a pending `e`, score events cannot end the
        // performance at all — measured 2026-08-06 in this harness,
        // `scoreline_i "e 0"` acts as a SECTION end that kills all 16 voice
        // instances while the performance runs on (rc stays 0: permanent
        // silence the latch cannot see — a distinct failure shape the
        // ended-state display does not cover). And `exitnow` CRASHED this
        // harness inside the swap-thread prepare() (measured, same day) — an
        // authored `exitnow` is a host-crash risk, not a diagnosable state.
        std::string endedOrchestra(kTestOrchestra);
        size_t shortened = 0;
        for (size_t pos = 0; (pos = endedOrchestra.find("i 1 0 360000 ", pos)) != std::string::npos; ++shortened)
        {
            endedOrchestra.replace(pos, std::string("i 1 0 360000 ").size(), "i 1 0 0.25 ");
            pos += std::string("i 1 0 0.25 ").size();
        }
        CHECK(shortened == 16, "Case B: all 16 score lines shortened to 0.25 s");
        const size_t ePos = endedOrchestra.find("e 360000\n");
        CHECK(ePos != std::string::npos, "Case B: end statement found");
        if (ePos != std::string::npos)
            endedOrchestra.replace(ePos, std::string("e 360000\n").size(), "e 0.25\n");

        T5ynthProcessor procB;
        procB.setRateAndBufferSizeDetails(sr, bs);
        procB.prepareToPlay(sr, bs);
        procB.forceCsoundEngineMode();
        CHECK(procB.requestCsoundOrchestra(utf8(endedOrchestra.c_str())),
              "Case B: requestCsoundOrchestra() accepts the self-ending fixture");
        waitForCsoundSettle(procB);
        // Same instant-adopt path and generous bound as Case A, with an early
        // exit once the latch shows: the adopted engine IS the active one, so
        // the very first renderUpTo k-cycle can already latch it.
        {
            juce::AudioBuffer<float> buf(2, bs);
            for (int b = 0; b < 64 && ! procB.csoundPerformanceEnded(); ++b)
            {
                buf.clear();
                procB.processBlock(buf, b == 0 ? noteOn : empty);
            }
        }
        CHECK(procB.csoundPerformanceEnded(), "ended orchestra latches and the processor reports it");
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
