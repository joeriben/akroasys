// ANIMATED proof that the engine-window WT fan's scan cursor actually MOVES in
// the shipping paint path — not a static PNG at a hand-picked position. Drives
// the REAL WaveformDisplay exactly like the SynthPanel 30 Hz timer does
// (setScanPosition(target) + one tickScan() per frame, smoothing and all),
// sweeping the target 0->1, and for every rendered frame finds the brightest
// column (the white 2.2 px cursor cycle stands out of the periwinkle fan, which
// is drawn at <=0.6 alpha). The cursor column must march left->right; if it is
// pinned, the cursor is frozen. Also writes a horizontal filmstrip PNG to
// eyeball the sweeping bright wave + its morphing shape.
//
// Build: flags.make response file + lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] (as the other tools).
//   /tmp/wt_anim   (run from repo root; writes tools/wt_display_out/anim_strip.png)
#include "JuceHeader.h"
#include "dsp/DcoBaker.h"
#include "dsp/DcoRecipeJson.h"
#include "dsp/WavetableOscillator.h"
#include "gui/WaveformDisplay.h"
#include <cstdio>
#include <vector>

// Brightest near-white column in the waveform band (the cursor cycle). Returns
// -1 if no white pixel found (cursor absent).
static int brightestColumn(const juce::Image& img)
{
    const int w = img.getWidth(), h = img.getHeight();
    int bestX = -1; double best = 0.0;
    for (int x = 0; x < w; ++x)
    {
        double col = 0.0;
        for (int y = 18; y < h; ++y)            // skip the label band
        {
            const auto p = img.getPixelAt(x, y);
            if (p.getRed() > 200 && p.getGreen() > 200 && p.getBlue() > 200)
                col += (p.getRed() + p.getGreen() + p.getBlue()) * (double) p.getAlpha() / 255.0;
        }
        if (col > best) { best = col; bestX = x; }
    }
    return best > 0.0 ? bestX : -1;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;

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

    const int W = 560, H = 230, FS = WavetableOscillator::FRAME_SIZE;
    WaveformDisplay wd;
    wd.setSize(W, H);
    wd.setRegionLabel("Wavetable");
    wd.setScanVisible(true);
    wd.setBottomReserve(18);
    wd.setWavetableFrames(strip, FS);
    if (! wd.isWavetableMode()) { std::printf("did NOT enter wtMode\n"); return 1; }

    // 30 frames = one 1 Hz sweep at 30 fps: target advances ~0.033/frame, the
    // 0.3 one-pole smoother tracks it easily — exactly the plugin's cadence.
    const int frames = 30;
    std::vector<juce::Image> shots;
    std::vector<int> cursorX;
    for (int f = 0; f < frames; ++f)
    {
        const float target = (float) f / (float) (frames - 1);
        wd.setScanPosition(target);
        wd.tickScan();                          // one smoothing step per frame
        juce::Image img(juce::Image::ARGB, W, H, true);
        { juce::Graphics g(img); wd.paint(g); }
        shots.push_back(img);
        cursorX.push_back(brightestColumn(img));
    }

    // Report cursor column every ~4th frame + monotonicity.
    std::printf("frame  target  cursorX(px)\n");
    int found = 0, prevX = -1; bool monotonic = true;
    for (int f = 0; f < frames; ++f)
    {
        if (cursorX[f] >= 0) ++found;
        if (f % 4 == 0)
            std::printf("  %2d   %.3f   %d\n", f, (float) f / (frames - 1), cursorX[f]);
        if (cursorX[f] >= 0 && prevX >= 0 && cursorX[f] < prevX - 2) monotonic = false;
        if (cursorX[f] >= 0) prevX = cursorX[f];
    }
    int firstX = -1, lastX = -1;
    for (int f = 0; f < frames; ++f) if (cursorX[f] >= 0) { firstX = cursorX[f]; break; }
    for (int f = frames - 1; f >= 0; --f) if (cursorX[f] >= 0) { lastX = cursorX[f]; break; }

    // Filmstrip: 8 evenly-spaced frames stacked horizontally, thin separators.
    const int cols = 8, sep = 2;
    juce::Image strip2(juce::Image::ARGB, cols * W + (cols - 1) * sep, H, true);
    { juce::Graphics g(strip2);
      g.fillAll(juce::Colour(0xff222222));
      for (int c = 0; c < cols; ++c)
      { const int f = c * (frames - 1) / (cols - 1);
        g.drawImageAt(shots[(size_t) f], c * (W + sep), 0); } }
    juce::File out = juce::File::getCurrentWorkingDirectory()
        .getChildFile("tools/wt_display_out").getChildFile("anim_strip.png");
    out.getParentDirectory().createDirectory(); out.deleteFile();
    if (auto os = out.createOutputStream()) { juce::PNGImageFormat png; png.writeImageToStream(strip2, *os); }

    std::printf("\ncursor found in %d/%d frames; X sweep %d -> %d px; %s\n",
                found, frames, firstX, lastX, monotonic ? "MONOTONIC L->R" : "NON-MONOTONIC");
    std::printf("filmstrip: %s\n", out.getFullPathName().toRawUTF8());
    const bool ok = found >= frames - 2 && lastX - firstX > 100 && monotonic;
    std::printf("\n%s\n", ok ? "LIVE CURSOR SWEEP CONFIRMED (animated paint path)."
                             : "*** cursor did not sweep — see columns above ***");
    return ok ? 0 : 1;
}
