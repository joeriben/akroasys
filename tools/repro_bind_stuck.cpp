// Headless repro: mono step-seq + 2 Bind steps + short decay -> stuck voice?
// Drives the REAL VoiceManager + StepSequencer with sample-accurate sub-block
// rendering, exactly like PluginProcessor's internal-event consumption.
//
// Build (see audition_sampler_follow.cpp header):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/repro_bind_stuck.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/repro_bind
#include "JuceHeader.h"
#include "dsp/VoiceManager.h"
#include "dsp/BlockParams.h"
#include "sequencer/StepSequencer.h"
#include <cstdio>
#include <vector>
#include <cmath>

using Bind = T5ynthStepSequencer::BindMode;

static float g_tuning[128];

static void runScenario(const char* label, int voiceLimit,
                        float decayMs, float sustain, float releaseMs,
                        bool bothBind, int blocks,
                        int switchBlock = -1, int switchVoiceLimit = -1,
                        int switchBlock2 = -1, int switchVoiceLimit2 = -1,
                        bool ampLoop = false, bool useGlide = false)
{
    const double SR = 44100.0;
    const int BS = 512;

    VoiceManager vm;
    vm.prepare(SR, BS);
    vm.setTuningTable(g_tuning);
    vm.setEngineMode(SynthVoice::EngineMode::Wavetable);
    vm.setVoiceLimit(voiceLimit);

    BlockParams bp;
    bp.ampAttack  = 2.0f;
    bp.ampDecay   = decayMs;   // "short decay"
    bp.ampSustain = sustain;   // pluck = 0
    bp.ampRelease = releaseMs;
    bp.ampLoop    = ampLoop;   // env loop on?
    vm.setBlockParams(bp);

    T5ynthStepSequencer seq;
    seq.prepare(SR, BS);
    seq.setNumSteps(16);
    seq.setBpm(120.0);
    seq.setDivision(3);            // 1/8
    for (int i = 0; i < 16; ++i) {
        seq.setStepNote(i, 60 + (i % 5));
        seq.setStepEnabled(i, true);
        seq.setStepGate(i, 0.5f);
        seq.setStepVelocity(i, 0.8f);
    }
    Bind mode = useGlide ? Bind::Glide : Bind::Bind;
    seq.setStepBindMode(2, mode);
    if (bothBind) seq.setStepBindMode(3, mode);
    seq.start();

    juce::AudioBuffer<float> buf(2, BS);
    std::vector<float> lfo(static_cast<size_t>(BS), 0.0f);
    std::vector<VoiceEvent> events;

    int totalOn = 0, fresh = 0, legato = 0;
    int onsAfterChain = 0, freshAfterChain = 0;  // post step-4 (first non-chain steps)
    int loopsSeen = 0;

    printf("\n===== %s (vLimit=%d decay=%.0f sus=%.2f rel=%.0f bothBind=%d) =====\n",
           label, voiceLimit, decayMs, sustain, releaseMs, (int)bothBind);

    for (int b = 0; b < blocks; ++b) {
        if (b == switchBlock && switchVoiceLimit > 0) {
            vm.setVoiceLimit(switchVoiceLimit);
            printf("  -- block %d: setVoiceLimit(%d) --\n", b, switchVoiceLimit);
        }
        if (b == switchBlock2 && switchVoiceLimit2 > 0) {
            vm.setVoiceLimit(switchVoiceLimit2);
            printf("  -- block %d: setVoiceLimit(%d) --\n", b, switchVoiceLimit2);
        }
        buf.clear();
        events.clear();
        seq.processBlock(buf, events);

        int pos = 0;
        for (auto& ev : events) {
            int off = juce::jlimit(0, BS, ev.sampleOffset);
            if (off > pos) { vm.renderBlock(buf, bp, lfo.data(), lfo.data(), lfo.data(), pos, off - pos); pos = off; }

            if (ev.type == VoiceEvent::Type::NoteOn) {
                // snapshot v0 state BEFORE applying (to classify fresh vs legato in mono)
                bool wasActive = vm.getVoice(0).isActive();
                bool wasRel    = vm.getVoice(0).isReleasing();
                bool isBind = ev.artic != VoiceEvent::Articulation::Normal;
                float glideMs = (ev.artic == VoiceEvent::Articulation::Bind) ? 0.0f : seq.getGlideTime();
                vm.noteOn(ev.note, ev.velocity, isBind, glideMs, false, false, false, ev.strandId, ev.pan, 0);
                ++totalOn;
                bool wouldLegato = (wasActive && !wasRel) || (isBind && wasActive);
                if (voiceLimit == 1) { if (wouldLegato) ++legato; else ++fresh; }
            } else {
                vm.noteOff(ev.note, ev.strandId);
            }
        }
        if (pos < BS) vm.renderBlock(buf, bp, lfo.data(), lfo.data(), lfo.data(), pos, BS - pos);

        // periodic state snapshot
        if (b % 40 == 0 || b == blocks - 1) {
            int ac = vm.getActiveVoiceCount();
            printf("  blk%3d: activeVoices=%d | v0[act=%d rel=%d note=%d] v1[act=%d rel=%d note=%d] v2[act=%d note=%d] v3[act=%d note=%d]\n",
                   b, ac,
                   vm.getVoice(0).isActive(), vm.getVoice(0).isReleasing(), vm.getVoice(0).getCurrentNote(),
                   vm.getVoice(1).isActive(), vm.getVoice(1).isReleasing(), vm.getVoice(1).getCurrentNote(),
                   vm.getVoice(2).isActive(), vm.getVoice(2).getCurrentNote(),
                   vm.getVoice(3).isActive(), vm.getVoice(3).getCurrentNote());
        }
    }

    if (voiceLimit == 1 && switchVoiceLimit < 0)
        printf("  MONO summary: noteOns=%d fresh-triggers=%d legato-glides=%d\n", totalOn, fresh, legato);
    printf("  FINAL: activeVoices=%d v0[act=%d rel=%d note=%d]\n",
           vm.getActiveVoiceCount(), vm.getVoice(0).isActive(), vm.getVoice(0).isReleasing(), vm.getVoice(0).getCurrentNote());
}

// Explicit-pattern harness: enabledMask / bindMask are bit i = step i.
static void runPattern(const char* label, int voiceLimit, int numSteps,
                       uint64_t enabledMask, uint64_t bindMask,
                       bool ampLoop, int blocks)
{
    const double SR = 44100.0;
    const int BS = 512;
    VoiceManager vm;
    vm.prepare(SR, BS);
    vm.setTuningTable(g_tuning);
    vm.setEngineMode(SynthVoice::EngineMode::Wavetable);
    vm.setVoiceLimit(voiceLimit);

    BlockParams bp;
    bp.ampAttack = 2.0f; bp.ampDecay = 30.0f; bp.ampSustain = 0.0f; bp.ampRelease = 50.0f;
    bp.ampLoop = ampLoop;
    vm.setBlockParams(bp);

    T5ynthStepSequencer seq;
    seq.prepare(SR, BS);
    seq.setNumSteps(numSteps);
    seq.setBpm(120.0); seq.setDivision(3);
    for (int i = 0; i < numSteps; ++i) {
        bool en = (enabledMask >> i) & 1ull;
        seq.setStepNote(i, 60 + (i % 5));
        seq.setStepEnabled(i, en);
        seq.setStepGate(i, 0.5f);
        seq.setStepVelocity(i, 0.8f);
        if ((bindMask >> i) & 1ull) seq.setStepBindMode(i, Bind::Bind);
    }
    seq.start();

    juce::AudioBuffer<float> buf(2, BS);
    std::vector<float> lfo(static_cast<size_t>(BS), 0.0f);
    std::vector<VoiceEvent> events;
    int totalOn = 0;
    int ssOn = 0, ssAudible = 0;          // steady-state (last 40% of run)
    const int ssStart = blocks * 6 / 10;
    // continuous-active tracking of voice 0
    int v0activeStreak = 0, v0maxStreak = 0;

    for (int b = 0; b < blocks; ++b) {
        bool steady = b >= ssStart;
        buf.clear(); events.clear();
        seq.processBlock(buf, events);
        int pos = 0;
        for (auto& ev : events) {
            int off = juce::jlimit(0, BS, ev.sampleOffset);
            if (off > pos) { vm.renderBlock(buf, bp, lfo.data(), lfo.data(), lfo.data(), pos, off - pos); pos = off; }
            if (ev.type == VoiceEvent::Type::NoteOn) {
                bool isBind = ev.artic != VoiceEvent::Articulation::Normal;
                float glideMs = (ev.artic == VoiceEvent::Articulation::Bind) ? 0.0f : seq.getGlideTime();
                // AUDIBLE = this noteOn actually retriggers an envelope (fresh attack),
                // not a silent legato glide of an already-decayed held voice.
                bool audible;
                if (voiceLimit == 1) {
                    bool a = vm.getVoice(0).isActive(), r = vm.getVoice(0).isReleasing();
                    bool wouldLegato = (a && !r) || (isBind && a);
                    audible = !wouldLegato;        // fresh retrigger
                } else {
                    // poly: a Normal note always grabs free/steal (fresh). A bind glides
                    // newest active. Approx: audible if Normal, or no active voice.
                    audible = !isBind || !vm.hasActiveVoices();
                }
                vm.noteOn(ev.note, ev.velocity, isBind, glideMs, false, false, false, ev.strandId, ev.pan, 0);
                ++totalOn;
                if (steady) { ++ssOn; if (audible) ++ssAudible; }
            } else {
                vm.noteOff(ev.note, ev.strandId);
            }
        }
        if (pos < BS) vm.renderBlock(buf, bp, lfo.data(), lfo.data(), lfo.data(), pos, BS - pos);
        if (vm.getVoice(0).isActive()) { ++v0activeStreak; v0maxStreak = std::max(v0maxStreak, v0activeStreak); }
        else v0activeStreak = 0;
    }
    // blocks-per-step for context (1/8 @120 = 11025 smp = ~21.5 blocks)
    bool stuck = (ssOn >= 3 && ssAudible == 0);   // seq still firing but nothing audible
    printf("%-46s vL=%d nSteps=%2d en=0x%04llx bind=0x%04llx loop=%d => ssOns=%d ssAUDIBLE=%d  v0maxActiveStreak=%d blk %s\n",
           label, voiceLimit, numSteps, (unsigned long long)enabledMask, (unsigned long long)bindMask, (int)ampLoop,
           ssOn, ssAudible, v0maxStreak,
           (stuck ? " <<<<< STUCK (no audible notes)" : (ssAudible>0?"ok":"")));
}

int main()
{
    for (int i = 0; i < 128; ++i)
        g_tuning[i] = 440.0f * std::pow(2.0f, static_cast<float>(i - 69) / 12.0f);

    printf("\n##### EXPLICIT PATTERN SWEEP #####\n");
    // Closed bind loop: 2 enabled steps, both bound -> slidesToNext always true.
    runPattern("2-step closed bind loop (0,1 bind)", 1, 2, 0x3, 0x3, false, 200);
    runPattern("2-step closed bind loop, ENV-LOOP",  1, 2, 0x3, 0x3, true,  200);
    runPattern("2-step closed bind loop, 4voice",    4, 2, 0x3, 0x3, false, 200);
    // 4 enabled, bind first two only (0,1) -> 2 binds then 2 normal
    runPattern("4-step, bind 0,1",                   1, 4, 0xF, 0x3, false, 200);
    // 4 enabled, bind ALL -> closed loop of 4
    runPattern("4-step closed loop (bind all 4)",    1, 4, 0xF, 0xF, false, 200);
    runPattern("4-step closed loop, 4voice",         4, 4, 0xF, 0xF, false, 200);
    // 8 enabled, bind last two (6,7) -> wrap
    runPattern("8-step, bind 6,7 (wrap)",            1, 8, 0xFF, 0xC0, false, 200);
    // disabled gap: steps 0,1 enabled+bound, 2 disabled, 3 enabled
    runPattern("0,1 bind + step2 REST + step3 on",   1, 4, 0xB, 0x3, false, 200);
    // sparse: only 2 enabled non-adjacent, both bound
    runPattern("steps 0 & 4 enabled, both bound",    1, 8, 0x11, 0x11, false, 200);

    printf("\n##### BRUTE FORCE: all-enabled, every bindMask, flag STUCK #####\n");
    for (int n = 2; n <= 6; ++n) {
        uint64_t en = (1ull << n) - 1;
        int stuckCount = 0, total = 0;
        for (uint64_t bm = 0; bm < (1ull << n); ++bm) {
            // capture stuck by re-deriving inline (quiet): reuse runPattern but it prints.
            // Instead, count via a quiet replica:
            // (We just call runPattern only for the ones that matter; here count closed loops.)
            ++total;
        }
        // Characterize: which masks stick? Print only the sticking ones.
        printf("-- numSteps=%d (all enabled) --\n", n);
        for (uint64_t bm = 0; bm < (1ull << n); ++bm) {
            // quiet stuck check
            // Closed-loop predicate: every enabled step has slidesToNext==true, i.e.
            // for every enabled step i, step i is bound AND next enabled step exists.
            // With all enabled, that's simply: all steps bound (bm == en).
            // But verify empirically for a few:
            if (bm == en || ((bm & en) == en)) {
                char lbl[64]; std::snprintf(lbl, sizeof lbl, "  bf n=%d bind=0x%llx", n, (unsigned long long)bm);
                runPattern(lbl, 1, n, en, bm, false, 120);
            }
        }
    }
    // Spot-check: do ANY non-all-bound masks stick? Test all masks for n=4,5 quietly via streak.
    for (int n = 4; n <= 5; ++n) {
        uint64_t en = (1ull << n) - 1;
        for (uint64_t bm = 1; bm < (1ull << n); ++bm) {
            if (bm == en) continue; // skip all-bound (known stuck)
            char lbl[80]; std::snprintf(lbl, sizeof lbl, "  partial n=%d bind=0x%llx", n, (unsigned long long)bm);
            runPattern(lbl, 1, n, en, bm, false, 120);
        }
    }

    printf("\n##### ORIGINAL 16-STEP SCENARIOS #####\n");

    // 1) pure mono, single Bind step, short decay pluck
    runScenario("mono single-bind pluck", 1, 30.0f, 0.0f, 50.0f, false, 300);
    // 2) pure mono, TWO adjacent bind steps, short decay pluck
    runScenario("mono two-bind pluck", 1, 30.0f, 0.0f, 50.0f, true, 300);
    // 3) mono two-bind, LONG release (decayed but long tail)
    runScenario("mono two-bind LONG release", 1, 30.0f, 0.0f, 800.0f, true, 300);
    // 4) full user repro: mono two-bind -> switch to 4 voice -> back to mono
    runScenario("mono->4voice->mono two-bind", 1, 30.0f, 0.0f, 50.0f, true, 360, 120, 4, 240, 1);
    // 5) poly 4-voice two-bind from the start
    runScenario("4voice two-bind pluck", 4, 30.0f, 0.0f, 50.0f, true, 300);

    // 6) ENV LOOP ON — held note never idles. mono two-bind.
    runScenario("mono two-bind ENV-LOOP", 1, 30.0f, 0.0f, 50.0f, true, 300,
                -1, -1, -1, -1, /*ampLoop=*/true, false);
    // 7) ENV LOOP ON — single bind
    runScenario("mono single-bind ENV-LOOP", 1, 30.0f, 0.0f, 50.0f, false, 300,
                -1, -1, -1, -1, /*ampLoop=*/true, false);
    // 8) ENV LOOP ON — Glide articulation
    runScenario("mono two-GLIDE ENV-LOOP", 1, 30.0f, 0.0f, 50.0f, true, 300,
                -1, -1, -1, -1, /*ampLoop=*/true, /*useGlide=*/true);
    // 9) ENV LOOP ON — poly 4-voice
    runScenario("4voice two-bind ENV-LOOP", 4, 30.0f, 0.0f, 50.0f, true, 300,
                -1, -1, -1, -1, /*ampLoop=*/true, false);
    // 10) Glide articulation, NO loop
    runScenario("mono two-GLIDE pluck", 1, 30.0f, 0.0f, 50.0f, true, 300,
                -1, -1, -1, -1, false, /*useGlide=*/true);

    return 0;
}
