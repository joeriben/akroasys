// Wire-cap probe for the LCO recipe parser (spec sec.10, Slice 2b). Exercises
// dco::recipeFromVar directly: builds a keyframes array of N entries, parses it,
// and reports how many survive. The parser truncates (keeps the first cap, drops
// the tail) rather than rejecting -- this tool pins that semantics and the cap's
// numeric value, so the 8->32 raise is verified before/after by diffing output.
//
// Build (same recipe as tools/audition_dco_bake.cpp):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/test_lco_keyframe_cap.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/kfcap
// Exit 0 iff every invariant holds. Prints the discovered saturation cap.
#include "JuceHeader.h"
#include "dsp/DcoRecipeJson.h"
#include <cstdio>
#include <vector>

// Build a recipe var carrying N additive keyframes; each keyframe's width is set
// to its wire index (a marker) so truncation order can be verified after parse.
static dco::Recipe parseN(int n)
{
    juce::Array<juce::var> kfs;
    for (int i = 0; i < n; ++i)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("kind", "additive");
        obj->setProperty("width", (double) i);   // order marker, read back into kf.width
        // one aligned partial so each keyframe is a well-formed additive station
        juce::Array<juce::var> parts;
        auto* p = new juce::DynamicObject();
        p->setProperty("h", 1.0);
        p->setProperty("a", 1.0);
        p->setProperty("phase", 0.0);
        parts.add(juce::var(p));
        obj->setProperty("partials", parts);
        kfs.add(juce::var(obj));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("keyframes", kfs);
    return dco::recipeFromVar(juce::var(root));
}

int main()
{
    const int Ns[] = { 0, 1, 3, 7, 8, 9, 16, 32, 33, 64, 1000 };
    bool ok = true;

    // Discover the saturation cap empirically: the kept count for a huge input.
    const int cap = (int) parseN(100000).keyframes.size();
    std::printf("discovered wire keyframe cap = %d\n", cap);

    std::printf("  N -> kept   (status)\n");
    int prevKept = -1;
    for (int n : Ns)
    {
        const dco::Recipe r = parseN(n);
        const int kept = (int) r.keyframes.size();
        const int expect = (n < cap) ? n : cap;         // min(N, cap)
        const bool sizeOk = (kept == expect);

        // Order preservation: the survivors are the FIRST `kept` wire keyframes,
        // so width marker j must equal j for every surviving index.
        bool orderOk = true;
        for (int j = 0; j < kept; ++j)
            if ((int) r.keyframes[(size_t) j].width != j) { orderOk = false; break; }

        // Monotonic, saturating: kept never shrinks as N grows.
        const bool monoOk = (kept >= prevKept);
        prevKept = kept;

        const bool trunc = (kept < n);
        std::printf("  %5d -> %4d   (%s%s%s%s)\n", n, kept,
                    trunc ? "truncated" : "kept-all",
                    sizeOk ? "" : " SIZE-FAIL",
                    orderOk ? "" : " ORDER-FAIL",
                    monoOk ? "" : " MONO-FAIL");
        ok = ok && sizeOk && orderOk && monoOk;
    }

    // Explicit tail-drop semantics check: >cap must yield exactly cap (never a
    // reject-to-zero, never an error), and the survivors are the head of the wire.
    const dco::Recipe over = parseN(cap + 1);
    if ((int) over.keyframes.size() != cap) { ok = false; std::printf("OVER-CAP-FAIL\n"); }

    std::printf("%s\n", ok ? "ALL INVARIANTS PASS" : "*** INVARIANT FAILURE ***");
    return ok ? 0 : 1;
}
