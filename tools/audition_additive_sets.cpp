// Offline audition for the K-station additive bank (spec §3, LCO wave
// interpolation, Slice 1). Loads K index-aligned partial "stations" from a JSON
// file into a REAL WavetableOscillator via setAdditiveBank(sets), drives the
// engine's own DCO motion transport (K>=2) so the per-index (a,h) blend is
// exercised exactly as the plugin drives it (loadDcoAdditive), renders N seconds
// at a fixed fundamental through processSample(), and writes a 16-bit mono WAV.
//
// This is the ear-gate + parity path of the spec's verification §9: the WAV is
// produced by the shipping synthesis code, not a numpy reimplementation.
//
// JSON shape:
//   {
//     "sets": [
//       [ {"h":1.0,"a":1.0,"phase":0.0}, {"h":2.76,"a":0.55,"phase":0.0}, ... ],
//       [ {"h":1.0,"a":0.4,"phase":0.0}, {"h":2.76,"a":0.6, "phase":0.0}, ... ]
//     ],
//     "motion_rate_hz": 0.5
//   }
// All stations SHOULD have the same length (index i = same partial in every set);
// setAdditiveBank caps to the shortest defensively. K==1 renders a static spectrum
// (motion off); K>=2 sweeps the stations at motion_rate_hz full loops/second.
//
// argv: <sets.json> <out.wav> [seconds=3] [f0=220]
//
// Build (standalone, same approach as tools/audition_dco_bake.cpp): compile
// against the plugin's static lib + JUCE flags, e.g.
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/audition_additive_sets.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/additive_sets
#include "JuceHeader.h"
#include "dsp/WavetableOscillator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

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

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit; // safe init for any JUCE statics

    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <sets.json> <out.wav> [seconds=3] [f0=220]\n", argv[0]);
        return 2;
    }

    const std::string outPath = argv[2];
    const double seconds = (argc >= 4) ? std::atof(argv[3]) : 3.0;
    const double f0      = (argc >= 5) ? std::atof(argv[4]) : 220.0;
    const int sr = 44100;

    const juce::File jsonFile { juce::String::fromUTF8(argv[1]) };
    if (!jsonFile.existsAsFile())
    {
        fprintf(stderr, "error: cannot read %s\n", argv[1]);
        return 2;
    }
    const juce::var parsed = juce::JSON::parse(jsonFile.loadFileAsString());

    // Parse the stations. Each set is an array of {h,a,phase} objects.
    std::vector<std::vector<WavetableOscillator::AdditivePartial>> sets;
    const juce::var setsVar = parsed.getProperty("sets", juce::var());
    if (const juce::Array<juce::var>* setArr = setsVar.getArray())
    {
        for (const auto& setV : *setArr)
        {
            std::vector<WavetableOscillator::AdditivePartial> station;
            if (const juce::Array<juce::var>* partialArr = setV.getArray())
            {
                for (const auto& pV : *partialArr)
                {
                    WavetableOscillator::AdditivePartial p;
                    p.h     = static_cast<float>(static_cast<double>(pV.getProperty("h", 1.0)));
                    p.a     = static_cast<float>(static_cast<double>(pV.getProperty("a", 0.0)));
                    p.phase = static_cast<float>(static_cast<double>(pV.getProperty("phase", 0.0)));
                    station.push_back(p);
                }
            }
            sets.push_back(std::move(station));
        }
    }

    if (sets.empty())
    {
        fprintf(stderr, "error: no \"sets\" array in %s\n", argv[1]);
        return 2;
    }

    const float motionRate = static_cast<float>(
        static_cast<double>(parsed.getProperty("motion_rate_hz", 0.5)));
    const int numSets = static_cast<int>(sets.size());

    // Drive the REAL engine exactly as the plugin glue does (loadDcoAdditive).
    WavetableOscillator osc;
    osc.prepare(static_cast<double>(sr), 512);
    osc.setInterpolation(true);
    osc.setFrequency(static_cast<float>(f0));
    osc.setAdditiveBank(sets);
    if (numSets >= 2)
        osc.setDcoMotion(true, motionRate > 0.0f ? motionRate : 0.5f);
    else
        osc.setDcoMotion(false, 0.0f);

    const int totalSamples = static_cast<int>(seconds * sr);
    std::vector<float> mono;
    mono.reserve(static_cast<size_t>(totalSamples));

    bool anyNonFinite = false;
    float peak = 0.0f;
    double sumSq = 0.0;
    for (int i = 0; i < totalSamples; ++i)
    {
        const float s = osc.processSample();
        if (!std::isfinite(s)) anyNonFinite = true;
        else { peak = std::max(peak, std::fabs(s)); sumSq += static_cast<double>(s) * s; }
        mono.push_back(s);
    }
    const double rms = mono.empty() ? 0.0 : std::sqrt(sumSq / static_cast<double>(mono.size()));

    writeWav(outPath, mono, sr);

    printf("[additive_sets] K=%d motion=%s rate=%.3f f0=%.1f secs=%.2f -> %s\n",
           numSets, (numSets >= 2 ? "on" : "off"),
           static_cast<double>(numSets >= 2 ? (motionRate > 0.0f ? motionRate : 0.5f) : 0.0f),
           f0, seconds, outPath.c_str());
    printf("[additive_sets] samples=%d finite=%s peak=%.4f rms=%.4f\n",
           totalSamples, anyNonFinite ? "NO" : "yes", static_cast<double>(peak), rms);

    // Non-silent and finite is the basic gate; the spectral checks are done in Python.
    const bool ok = !anyNonFinite && peak > 1.0e-4f && rms > 1.0e-4;
    printf("%s\n", ok ? "OK" : "*** FAIL (silent or non-finite) ***");
    return ok ? 0 : 1;
}
