// Does a baked DCO table ACTUALLY morph when played, and does the scan cursor's
// data source (getEffectiveScanPosition) sweep? Drives a real WavetableOscillator
// the way loadDcoWavetable + voice distribution do, plays it, and logs the
// effective scan + a morph proxy (mean |Δsample|, higher for harmonically rich
// frames) over time. If effScan is stuck or the proxy is flat, the "nothing
// morphs" report is a real bug — this is the LIVE test that the static PNG
// audition did not do.
//
// Build (same flags as tools/audition_dco_bake.cpp):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/wt.rsp
//   clang++ -std=c++17 -O2 @/tmp/wt.rsp tools/test_dco_morph_live.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/dco_morph
#include "JuceHeader.h"
#include "dsp/DcoBaker.h"
#include "dsp/DcoRecipeJson.h"
#include "dsp/WavetableOscillator.h"
#include <cstdio>
#include <cmath>

int main()
{
    // Rich additive -> pure sine morph over 256 frames.
    const juce::String morphJson = R"JSON({
      "frames": 256, "loop": false, "motion_rate_hz": 0.25,
      "keyframes": [
        { "kind": "additive", "partials": [
            {"h":1,"a":1.0,"phase":0.0},{"h":2,"a":0.5,"phase":0.0},
            {"h":3,"a":0.33,"phase":0.0},{"h":4,"a":0.25,"phase":0.0},
            {"h":5,"a":0.2,"phase":0.0},{"h":6,"a":0.16,"phase":0.0},
            {"h":7,"a":0.14,"phase":0.0},{"h":8,"a":0.12,"phase":0.0} ] },
        { "kind": "additive", "partials": [ {"h":1,"a":1.0,"phase":0.0} ] }
      ],
      "motion": [ {"to":0,"dur_frac":0.0,"curve":"lin"},
                  {"to":1,"dur_frac":1.0,"curve":"lin"} ]
    })JSON";

    const auto recipe = dco::recipeFromVar(juce::JSON::parse(morphJson));
    const auto strip  = dco::Baker::framesToBuffer(dco::Baker::bake(recipe));
    const int numFrames = strip.getNumSamples() / WavetableOscillator::FRAME_SIZE;
    std::printf("strip: %d frames, motionRateHz=%.3f\n", numFrames, recipe.motionRateHz);

    const double sr = 44100.0;
    const float  rate = 1.0f;   // 1 sweep/sec so several sweeps show in a short run

    // Master, set up exactly like loadDcoWavetable.
    WavetableOscillator master;
    master.prepare(sr, 512);
    master.setExactFrames(strip);
    master.setAutoScan(false);
    master.setDcoMotion(true, rate);
    std::printf("master dcoMotionActive=%d\n", (int) master.isDcoMotionActive());

    // Voice, adopted the way VoiceManager::distributeWavetableFrames does for an
    // (inactive) voice: shareFramesFrom(master).
    WavetableOscillator voice;
    voice.prepare(sr, 512);
    voice.shareFramesFrom(master);
    std::printf("voice  dcoMotionActive=%d (after shareFramesFrom)\n", (int) voice.isDcoMotionActive());
    voice.setFrequency(220.0f);
    voice.retriggerAutoScan();   // note-on: reset the gesture phase

    std::printf("\n  t(s)  effScan  morphProxy(mean|dS|)\n");
    const int chunk = static_cast<int>(sr) / 10;   // 0.1 s
    float prev = 0.0f;
    float minProxy = 1e9f, maxProxy = -1e9f, minScan = 1e9f, maxScan = -1e9f;
    for (int c = 0; c < 40; ++c)                    // 4 s
    {
        double dsum = 0.0;
        for (int i = 0; i < chunk; ++i)
        {
            const float o = voice.processSample();
            dsum += std::abs(o - prev);
            prev = o;
        }
        const float eff   = voice.getEffectiveScanPosition();
        const float proxy = static_cast<float>(dsum / chunk);
        minProxy = std::min(minProxy, proxy); maxProxy = std::max(maxProxy, proxy);
        minScan  = std::min(minScan, eff);    maxScan  = std::max(maxScan, eff);
        if (c % 2 == 0)
            std::printf("  %.1f   %.3f    %.5f\n", c * 0.1, eff, proxy);
    }

    std::printf("\neffScan span:    %.3f .. %.3f  (%s)\n", minScan, maxScan,
                (maxScan - minScan) > 0.2f ? "SWEEPS - cursor would move" : "STUCK - cursor frozen");
    std::printf("morphProxy span: %.5f .. %.5f  (%s)\n", minProxy, maxProxy,
                (maxProxy - minProxy) > 0.1f * maxProxy ? "MORPHS - timbre changes" : "FLAT - no morph");
    const bool ok = (maxScan - minScan) > 0.2f && (maxProxy - minProxy) > 0.1f * maxProxy;
    std::printf("\n%s\n", ok ? "LIVE MORPH + SCAN CONFIRMED." : "*** BROKEN: see spans above ***");
    return ok ? 0 : 1;
}
