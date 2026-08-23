// Renders MpeSettingsPage to PNG at the settings overlay's real sizes, so the
// layout can be judged without opening the synth and without driving anyone's
// screen. The overlay is jlimit(400,600) x jlimit(300,500) (MainPanel.cpp), so
// the two sizes rendered here are its FLOOR and its ceiling -- the floor is the
// one that decides whether the read-out clips.
//
// Build (T5ynth's standard offline-tool recipe):
//
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/render_mpe_tab.cpp \
//     build_clean/T5ynth_artefacts/Release/libakroasys_SharedCode.a \
//     build_clean/libT5ynthData.a \
//     $PWD/third_party/csound/macos-arm64/lib/CsoundLib64 \
//     -framework CoreAudioKit -framework DiscRecording -framework CoreAudio \
//     -framework CoreMIDI -framework AudioToolbox -framework Accelerate \
//     -framework WebKit -weak_framework Metal -weak_framework MetalKit \
//     -framework QuartzCore -framework Cocoa -framework Foundation \
//     -framework IOKit -framework Security -framework Carbon \
//     -framework AudioUnit -framework CoreServices -o /tmp/t5main/render_mpe_tab

#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "../src/gui/MpeSettingsPage.h"
#include "../src/gui/T5ynthLookAndFeel.h"

namespace
{
    void shoot (const juce::String& name, int w, int h,
                const MpeSettingsPage::Status& status,
                int perNote, int master, float xScale)
    {
        T5ynthLookAndFeel lnf;
        juce::LookAndFeel::setDefaultLookAndFeel (&lnf);

        {
            MpeSettingsPage page;
            page.statusSource = [status] { return status; };
            page.setRanges (perNote, master, xScale);
            page.setSize (w, h);
            page.refreshReadout();          // fill the painted block

            juce::Image img (juce::Image::ARGB, w, h, true);
            juce::Graphics g (img);
            page.paintEntireComponent (g, true);

            auto out = juce::File ("/tmp/t5main").getChildFile (name + ".png");
            juce::PNGImageFormat png;
            juce::FileOutputStream os (out);
            os.setPosition (0);
            os.truncate();
            png.writeImageToStream (img, os);
            std::printf ("%s  %dx%d\n", out.getFullPathName().toRawUTF8(), w, h);
        }

        // Never setLookAndFeel(nullptr) on a component (CLAUDE.md); resetting the
        // DEFAULT is a different call and is what keeps lnf from outliving its use.
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::File ("/tmp/t5main").createDirectory();

    // 1. Cold: nothing plugged in.
    shoot ("mpe_tab_empty", 400, 300, {}, 48, 2, 1.0f);

    // 2. The measured Osmose: notes on ch2-9, CC74 resting at 0 and rising,
    //    member pressure, a lean of +-171, and NO transmitted range or zone.
    MpeSettingsPage::Status osmose;
    osmose.messages = 4127;
    osmose.noteChannels = 0b0000000011111110;      // ch 2-8
    osmose.bendChannels = osmose.noteChannels;
    osmose.timbreChannels = osmose.noteChannels;
    osmose.memberPressureChannels = osmose.noteChannels;
    osmose.cc74Min = 0;   osmose.cc74Max = 83;
    osmose.bendMin = 8021; osmose.bendMax = 8363;
    osmose.perNoteBendRange = 48;
    osmose.xFullScale = 1.0f;
    osmose.memberChannels = 15;
    shoot ("mpe_tab_osmose", 400, 300, osmose, 48, 2, 1.0f);

    // 3. A device that DID announce itself: zone + a transmitted range that is
    //    not one of the listed values, plus a wide slide.
    MpeSettingsPage::Status linn = osmose;
    linn.mcmSeen = true;
    linn.rpnRange = 7;  linn.rpnChannel = 1;
    linn.perNoteBendRange = 7;
    linn.memberChannels = 7;
    linn.bendMin = 0;   linn.bendMax = 16383;
    linn.cc74Min = 12;  linn.cc74Max = 127;
    linn.masterPressure = true;
    linn.xFullScale = 7.0f;
    shoot ("mpe_tab_declared", 600, 500, linn, 7, 2, 7.0f);

    // 4. The commonest misconfiguration: MPE off, everything on channel 1.
    MpeSettingsPage::Status flat;
    flat.messages = 812;
    flat.noteChannels = 1u;
    flat.timbreChannels = 1u;
    flat.masterPressure = true;
    flat.polyPressure = true;
    flat.cc74Min = 0; flat.cc74Max = 127;
    flat.perNoteBendRange = 48;
    flat.xFullScale = 1.0f;
    flat.memberChannels = 15;
    shoot ("mpe_tab_notmpe", 400, 300, flat, 48, 2, 1.0f);

    return 0;
}
