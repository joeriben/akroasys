// Repro: does the delay tail get cut erratically / depend on hold duration?
// Drives the REAL T5ynthDelayLine exactly like PluginProcessor does — clear the
// buffer each block, fill with a burst for `burstBlocks`, then feed SILENCE —
// and logs per-block output magnitude. A click-free physical delay must decay
// SMOOTHLY (monotone-ish exponential); an abrupt drop to 0 while still audible
// = the internal silence gate cutting the tail.
//
// Build (after a Release build of the plugin):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/repro_delay_tail.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/repro_delay_tail
#include "JuceHeader.h"
#include "dsp/DelayLine.h"
#include "dsp/BlockParams.h"
#include <cstdio>
#include <cmath>
#include <vector>

static const char* modeName(int m) {
    switch (m) { case 1: return "Digital"; case 2: return "PingPong";
                 case 3: return "Tape"; case 4: return "BBD"; default: return "?"; }
}

// Returns the LAST block at which the output was still audible (the tail length
// in blocks). The meaningful invariant: a short hit's tail must be ~as long as a
// held note's tail (it carries less energy, so a little shorter is fine, but it
// must NOT be killed before the first echo even returns).
static int run(int mode, int character, int burstBlocks, float feedback, bool log)
{
    const double SR = 44100.0; const int BS = 512;
    T5ynthDelayLine d;
    d.prepare(SR, BS);
    d.setMode(mode);
    d.setCharacter(character);
    d.setTime(250.0f);
    d.setFeedback(feedback);
    d.setMix(0.5f);
    d.setDamp(0.3f);

    juce::AudioBuffer<float> buf(2, BS);
    double phase = 0.0; const double dph = juce::MathConstants<double>::twoPi * 220.0 / SR;

    const int totalBlocks = 2000;          // ~23 s — far longer than any reasonable tail
    int lastAudibleBlock = -1;
    const float audThr = 1e-4f;            // ~ -80 dB, well above the 1e-6 gate

    for (int b = 0; b < totalBlocks; ++b)
    {
        buf.clear();                        // mirrors PluginProcessor.cpp:1964
        if (b < burstBlocks)                // voice "held" for burstBlocks
        {
            for (int i = 0; i < BS; ++i) {
                float s = 0.6f * (float) std::sin(phase);
                phase += dph;
                buf.setSample(0, i, s);
                buf.setSample(1, i, s);
            }
        }
        d.processBlock(buf);

        const float outMag = juce::jmax(buf.getMagnitude(0, BS), buf.getMagnitude(1, BS));
        if (outMag > audThr) lastAudibleBlock = b;

        if (log && (b < burstBlocks + 4 || b % 40 == 0))
            printf("  blk %4d  in=%s  out=%.6f\n", b,
                   (b < burstBlocks ? "tone " : "SIL  "), outMag);
    }
    return lastAudibleBlock;
}

int main()
{
    printf("=== Delay tail: short hit must NOT be killed before its echoes ===\n");
    const double SR = 44100.0; const int BS = 512;
    const int delayPeriodBlocks = (int) std::ceil((0.250 * SR) / BS); // 250ms delay
    int fails = 0;
    for (int mode = 1; mode <= 4; ++mode) {
        for (float fb : { 0.50f, 0.70f, 0.90f }) {
            const int shortTail = run(mode, 0, 1,  fb, false);
            const int holdTail  = run(mode, 0, 60, fb, false);
            // Invariant: a single short hit must produce a REAL multi-period tail
            // (echoes returned and decayed), not be killed before its first echo.
            // The bug gave shortTail ≈ 0 (< first echo at delayPeriodBlocks); the
            // fix gives 3–23 s, i.e. ≥70% of the held-note tail. Require both: the
            // tail clearly outlasts the first echo, and it's a real fraction of the
            // held tail (rules out a near-immediate cut).
            const bool ok = shortTail > 4 * delayPeriodBlocks
                            && shortTail >= holdTail / 2;
            if (!ok) ++fails;
            printf("[%-8s] fb=%.2f  short=%5.2fs  hold=%5.2fs  %s\n",
                   modeName(mode), fb,
                   shortTail * BS / SR, holdTail * BS / SR,
                   ok ? "ok" : "*** SHORT HIT KILLED ***");
        }
    }
    // Tape Old (character 3, the legacy additive wobble) must obey the same
    // short-hit invariant as the modern presets.
    printf("--- Tape character variants, short hit fb=0.70 ---\n");
    for (int ch = 0; ch <= 3; ++ch) {
        const int shortTail = run(3, ch, 1, 0.70f, false);
        const int holdTail  = run(3, ch, 60, 0.70f, false);
        const bool ok = shortTail > 4 * delayPeriodBlocks && shortTail >= holdTail / 2;
        if (!ok) ++fails;
        printf("[Tape    ] char=%d  short=%5.2fs  hold=%5.2fs  %s\n",
               ch, shortTail * BS / SR, holdTail * BS / SR, ok ? "ok" : "*** KILLED ***");
    }

    printf("\n--- verbose: Tape short hit fb=0.70 (echo must appear ~blk %d) ---\n",
           delayPeriodBlocks);
    run(3, 0, 1, 0.70f, true);

    printf("\nRESULT: %d failures\n", fails);
    return fails > 0 ? 1 : 0;
}
