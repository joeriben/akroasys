// Deterministic repro for the SamplePlayer cubic-interpolation out-of-bounds read
// that crashed T5ynth 2.5.3 on the audio thread (SamplePlayer::playbackSample ->
// cubicSampleFrom, EXC_BAD_ACCESS reading ~2x past a shorter buffer).
//
// Root cause: in cubicSampleFrom the tap index i0 was derived from the PRE-clamp
// i1 (= floor(pos)); only i1/i2/i3 were bounded to bufLen. When a HELD sampler
// voice live-follows (morphToBufferFrom) onto a freshly-adopted SHORTER buffer,
// its readPosition is deliberately preserved and can exceed the new bufLen for a
// sample or two before advancePosition wraps it -> data[i0] reads out of bounds.
//
// This harness reproduces exactly that: a voice plays a LONG buffer forward, then
// the master publishes a SHORT buffer and the voice morph-adopts it with the large
// readPosition preserved. The first post-morph render reads the short buffer at
// that large position. Compile against the REAL SamplePlayer.cpp under
// AddressSanitizer so the bad data[i0] read is caught precisely.
//
// Build (compiles SamplePlayer.cpp directly so ASan instruments the read):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   cp build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] /tmp/lib_noSP.a
//   ar d /tmp/lib_noSP.a SamplePlayer.cpp.o
//   clang++ -std=c++17 -g -O1 -fsanitize=address @/tmp/h.rsp \
//     tools/repro_cubic_oob.cpp src/dsp/SamplePlayer.cpp /tmp/lib_noSP.a \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/repro_cubic
//   /tmp/repro_cubic
// Exit 0 = read stayed in bounds (fixed). ASan abort / non-zero = the OOB (bug).
#include "JuceHeader.h"
#include "dsp/SamplePlayer.h"

#include <cstdio>
#include <vector>
#include <cmath>
#include <algorithm>

static juce::AudioBuffer<float> makeTone(int n, double freq, int sr, float amp)
{
    juce::AudioBuffer<float> b(1, n);
    float* d = b.getWritePointer(0);
    for (int i = 0; i < n; ++i)
        d[i] = amp * static_cast<float>(std::sin(2.0 * M_PI * freq * i / sr));
    return b;
}

int main()
{
    const int sr = 48000, block = 512;

    // Master holds a LONG buffer; a voice shares it and plays forward at unity.
    SamplePlayer master;
    master.prepare(sr, block);
    master.setLoopMode(SamplePlayer::LoopMode::Loop);
    master.setNormalize(false);
    master.setLoopOptimizeLevel(0);
    master.setCrossfadeMs(5.0f);
    auto longBuf = makeTone(200000, 220.0, sr, 0.40f);
    master.loadBuffer(longBuf, sr);

    SamplePlayer voice;
    voice.prepare(sr, block);
    voice.setLoopMode(SamplePlayer::LoopMode::Loop);
    voice.setNormalize(false);
    voice.shareBufferFrom(master);
    voice.setTransposeRatio(1.0);   // unity -> bypass path (playbackSample per sample)
    voice.play();

    std::vector<float> out(static_cast<size_t>(block));

    // Advance readPosition well past the SHORT buffer length we are about to swap
    // in (~40*512 = 20480 source samples, still inside the 200k long buffer).
    for (int blk = 0; blk < 40; ++blk)
        voice.renderPitchedBlock(out.data(), block);

    // A/B-drift regenerate: master publishes a SHORT buffer; the held voice
    // live-follows it (readPosition ~20480 preserved) onto a 5000-sample buffer.
    auto shortBuf = makeTone(5000, 660.0, sr, 0.60f);
    master.loadBuffer(shortBuf, sr);
    voice.morphToBufferFrom(master, 200.0f);

    // First reads on the short buffer at the preserved (large) readPosition.
    // Pre-fix: cubicSampleFrom reads data[i0 ~ 20479] on a 5000-sample buffer.
    bool allFinite = true;
    float maxAbs = 0.0f;
    for (int blk = 0; blk < 4; ++blk)
    {
        voice.renderPitchedBlock(out.data(), block);
        for (float s : out)
        {
            if (! std::isfinite(s)) allFinite = false;
            maxAbs = std::max(maxAbs, std::fabs(s));
        }
    }

    std::printf("post-morph render: allFinite=%d maxAbs=%.4f\n", allFinite ? 1 : 0, maxAbs);
    if (! allFinite) { std::printf("FAIL: non-finite output\n"); return 2; }
    if (maxAbs > 4.0f) { std::printf("FAIL: output out of range (garbage read)\n"); return 3; }
    std::printf("PASS: held-note live-follow onto a shorter buffer read in-bounds\n");
    return 0;
}
