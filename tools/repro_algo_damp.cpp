// Does the Algo reverb's Damp control actually darken the tail?
// Drives the REAL AlgorithmicReverb exactly like PluginProcessor does
// (setMix(1.0) pure wet, room 0.7, width 1.0), feeds one noise burst then
// silence, and measures the high-frequency energy of the decaying tail at
// damping 0.0 (brightest) vs 1.0 (darkest). If Damp works, HF-ratio must
// drop clearly as damping rises.
//
// Build (after a Release build of the plugin):
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/repro_algo_damp.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/repro_algo_damp
#include "JuceHeader.h"
#include "dsp/AlgorithmicReverb.h"
#include <cstdio>
#include <cmath>

// Returns {totalEnergy, hfEnergy} of the reverb tail for a given damping.
static void run(float damp, double& totalE, double& hfE)
{
    const double SR = 44100.0; const int BS = 512;
    AlgorithmicReverb rv;
    rv.prepare(SR, BS);
    rv.setMix(1.0f);        // pure wet, like the processor
    rv.setRoomSize(0.7f);
    rv.setWidth(1.0f);
    rv.setDamping(damp);

    juce::AudioBuffer<float> buf(2, BS);
    juce::Random rng(12345);
    totalE = 0.0; hfE = 0.0;
    float prevL = 0.0f;

    const int totalBlocks = 200;   // ~2.3 s of tail
    for (int b = 0; b < totalBlocks; ++b)
    {
        buf.clear();
        if (b == 0)                // one short broadband burst, then silence
            for (int i = 0; i < BS; ++i) {
                float s = rng.nextFloat() * 2.0f - 1.0f;
                buf.setSample(0, i, s);
                buf.setSample(1, i, s);
            }
        rv.processBlock(buf);

        if (b >= 2)                // measure the TAIL only (after the burst)
        {
            const auto* L = buf.getReadPointer(0);
            for (int i = 0; i < BS; ++i) {
                const float x = L[i];
                totalE += (double) x * x;
                const float d = x - prevL;          // crude HF (first difference)
                hfE += (double) d * d;
                prevL = x;
            }
        }
        else
        {
            prevL = buf.getSample(0, BS - 1);
        }
    }
}

int main()
{
    printf("=== Algo reverb Damp: HF energy of the tail must drop as damping rises ===\n");
    printf("%-8s %14s %14s %12s\n", "damp", "totalE", "hfE", "hf/total");
    double prevRatio = -1.0;
    bool monotoneDown = true;
    for (float d : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        double tot, hf; run(d, tot, hf);
        const double ratio = (tot > 0.0) ? hf / tot : 0.0;
        printf("%-8.2f %14.2f %14.2f %12.6f\n", d, tot, hf, ratio);
        if (prevRatio >= 0.0 && ratio > prevRatio + 1e-9) monotoneDown = false;
        prevRatio = ratio;
    }
    printf("\nHF-ratio monotone non-increasing with damping: %s\n",
           monotoneDown ? "YES (Damp works)" : "NO (Damp broken or inverted)");
    return 0;
}
