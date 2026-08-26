// END-TO-END: does the engine-window scan cursor's ACTUAL data source
// (T5ynthProcessor::modulatedValues.scanPosition) sweep when a DCO table is
// loaded through the real plugin path and a note is held? This drives the REAL
// processor — loadDcoWavetable (engine switch + setExactFrames + setDcoMotion +
// distributeWavetableFrames), a MIDI note-on, then processBlock in a loop — and
// reads modulatedValues.scanPosition exactly as the SynthPanel timer does
// (SynthPanel.cpp:1347). If it stays NaN or pinned, the cursor cannot move and
// "nothing morphs" is a real feed bug. Also asserts the display-mode flags
// (hasNewWtDisplay / isWavetableMode / isDcoTableActive) that gate the fan.
//
// Build (flags.make response file + lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this], like the other
// tools/ harnesses). Run from repo root; no args, no Python backend needed.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/DcoBaker.h"
#include "dsp/DcoRecipeJson.h"
#include "dsp/WavetableOscillator.h"
#include <cstdio>
#include <cmath>

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Rich additive -> pure sine morph over 256 frames (same table as the
    // osc-level test, so the two harnesses corroborate each other).
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

    const double sr = 48000.0;
    const int    bs = 512;
    T5ynthProcessor proc;
    proc.prepareToPlay(sr, bs);

    // Real plugin path: 1 Hz sweep so several sweeps show in a few seconds.
    proc.loadDcoWavetable(strip, 1.0f);

    std::printf("after loadDcoWavetable:\n");
    std::printf("  hasNewWtDisplay = %d  (fan gets published to the display)\n", (int) proc.hasNewWtDisplay());
    std::printf("  isWavetableMode = %d  isDcoTableActive = %d  (both gate showWtTable)\n",
                (int) proc.isWavetableMode(), (int) proc.isDcoTableActive());
    const bool flagsOk = proc.hasNewWtDisplay() && proc.isWavetableMode() && proc.isDcoTableActive();

    // Hold A3 for the whole run (no note-off) so a voice stays active and
    // hasVoices is true → the wavetable branch at PluginProcessor.cpp:4379
    // feeds voiceOut.lastModulatedScan into modulatedValues.scanPosition.
    juce::MidiBuffer noteOn, empty;
    noteOn.addEvent(juce::MidiMessage::noteOn(1, 57, 1.0f), 0);   // A3

    juce::AudioBuffer<float> buf(2, bs);
    std::printf("\n  t(s)   mv.scanPosition (SynthPanel reads THIS)\n");
    float minScan = 1e9f, maxScan = -1e9f;
    int nanCount = 0, realCount = 0;
    const int blocks = 420;                       // ~4.5 s
    for (int b = 0; b < blocks; ++b)
    {
        buf.clear();
        proc.processBlock(buf, b == 0 ? noteOn : empty);
        const float scan = proc.modulatedValues.scanPosition.load(std::memory_order_relaxed);
        if (std::isnan(scan)) { ++nanCount; }
        else { ++realCount; minScan = std::min(minScan, scan); maxScan = std::max(maxScan, scan); }
        if (b % 40 == 0)
            std::printf("  %.2f   %s\n", b * bs / sr,
                        std::isnan(scan) ? "NaN (NO_GHOST)" : juce::String(scan, 3).toRawUTF8());
    }

    std::printf("\nreal scan samples: %d, NaN: %d\n", realCount, nanCount);
    std::printf("mv.scanPosition span: %.3f .. %.3f  (%s)\n", minScan, maxScan,
                (realCount > 0 && (maxScan - minScan) > 0.2f) ? "SWEEPS - cursor moves in GUI"
                                                              : "STUCK/absent - cursor frozen");
    const bool sweeps = realCount > 0 && (maxScan - minScan) > 0.2f;
    const bool ok = flagsOk && sweeps;
    std::printf("\n%s\n", ok ? "END-TO-END SCAN FEED CONFIRMED (fan shows + cursor sweeps)."
                             : "*** BROKEN: see flags/span above ***");
    return ok ? 0 : 1;
}
