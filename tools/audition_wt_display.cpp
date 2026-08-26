// Offline VISUAL audition for the engine-window wavetable display — renders the
// REAL WaveformDisplay in wavetable mode (2.5D X-fan cache + live scan cursor)
// to PNGs, so the shipping paint path (renderWtFan + cursor overlay) is eyeballed
// before commit, at a few scan positions.
//
// Build (same flags pattern as tools/audition_dco_bake.cpp):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/wt.rsp
//   clang++ -std=c++17 -O2 @/tmp/wt.rsp tools/audition_wt_display.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/wt_display
//   /tmp/wt_display    (run from repo root; writes tools/wt_display_out/)
#include "JuceHeader.h"
#include "dsp/DcoBaker.h"
#include "dsp/DcoRecipeJson.h"
#include "dsp/WavetableOscillator.h"
#include "gui/WaveformDisplay.h"

#include <cstdio>
#include <string>

// Render the real component at a given size + scan position.
static bool renderComponent(const juce::String& name, const juce::AudioBuffer<float>& strip,
                            int frameSize, int w, int h, float scanPos)
{
    WaveformDisplay wd;
    wd.setSize(w, h);
    wd.setRegionLabel("Wavetable");
    wd.setScanVisible(true);
    wd.setBottomReserve(18);
    wd.setWavetableFrames(strip, frameSize);
    if (! wd.isWavetableMode())
    {
        std::printf("  [%s] did NOT enter wtMode\n", name.toRawUTF8());
        return false;
    }
    if (scanPos >= 0.0f)
    {
        wd.setScanPosition(scanPos);
        wd.tickScan();   // NaN -> scanTarget: seeds scanPos exactly to scanPos
    }

    juce::Image img(juce::Image::ARGB, w, h, true);
    { juce::Graphics g(img); wd.paint(g); }

    juce::File out = juce::File::getCurrentWorkingDirectory()
                        .getChildFile("tools/wt_display_out").getChildFile(name + ".png");
    out.getParentDirectory().createDirectory();
    out.deleteFile();
    if (auto os = out.createOutputStream())
    {
        juce::PNGImageFormat png;
        const bool ok = png.writeImageToStream(img, *os);
        std::printf("  [%-20s] scan %.2f, %dx%d -> %s\n", name.toRawUTF8(),
                    scanPos, w, h, ok ? "ok" : "WRITE FAILED");
        return ok;
    }
    return false;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

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

    std::printf("WT display audition (REAL WaveformDisplay wtMode):\n");
    bool ok = true;
    const int FS = WavetableOscillator::FRAME_SIZE;
    ok &= renderComponent("real_scan50", strip, FS, 560, 230, 0.50f);
    ok &= renderComponent("real_scan15", strip, FS, 560, 230, 0.15f);
    ok &= renderComponent("real_scan85", strip, FS, 560, 230, 0.85f);
    ok &= renderComponent("real_noscan", strip, FS, 560, 230, -1.0f);   // idle: fan only, no cursor
    std::printf("%s\n", ok ? "all rendered." : "SOME FAILED.");
    return ok ? 0 : 1;
}
