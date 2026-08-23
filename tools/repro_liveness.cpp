// VOICE-LIVENESS test (independent of the StepSequencer): drive the real
// VoiceManager directly with balanced note-on/note-off pairs that mimic PLAYING
// (manual keys + a strand bind, poly, sustain>0). Invariant under test:
//   after every note that was turned on has been turned off, NO voice may remain
//   active. A voice still held = a permanent leak ("Voice fällt aus bis Restart").
//
// Build: same flags as repro_bind_stuck.cpp (see /tmp/h.rsp recipe).
#include "JuceHeader.h"
#include "dsp/VoiceManager.h"
#include "dsp/BlockParams.h"
#include <cstdio>
#include <vector>
#include <cmath>

static float g_tuning[128];

struct Ev { int on; int note; bool bind; int src; int ch; }; // on=1 noteOn,0 off; ch=mpeChannel(0=internal)

static int render(VoiceManager& vm, BlockParams& bp, int blocks){
    const int BS=512; juce::AudioBuffer<float> buf(2,BS); std::vector<float> z((size_t)BS,0.f);
    for (int b=0;b<blocks;++b){ buf.clear(); vm.renderBlock(buf,bp,z.data(),z.data(),z.data(),0,BS); }
    int held=0; for (int i=0;i<vm.getVoiceLimit();++i){ auto& v=vm.getVoice(i);
        if (v.isActive() && !v.isReleasing()) ++held; }
    return held;
}

static void scenario(const char* label, int voiceLimit, float sustain,
                     std::vector<Ev> evs)
{
    const double SR=44100.0; const int BS=512;
    VoiceManager vm; vm.prepare(SR,BS); vm.setTuningTable(g_tuning);
    vm.setEngineMode(SynthVoice::EngineMode::Freeze);
    vm.setVoiceLimit(voiceLimit);
    BlockParams bp; bp.ampAttack=2.f; bp.ampDecay=60.f; bp.ampSustain=sustain; bp.ampRelease=40.f;
    vm.setBlockParams(bp);
    const int BETW=6; // blocks between events (~70ms)
    for (auto& e: evs){
        if (e.on) vm.noteOn(e.note,0.8f,e.bind,0.f,false,false,false,e.src,0.f,e.ch);
        else      vm.noteOff(e.note,e.src,false,e.ch);   // origin, not a wildcard
        render(vm,bp,BETW);
    }
    // Everything that was turned on has now been turned off. Let releases finish.
    int leaked = render(vm,bp,200);
    printf("%-42s vL=%d sus=%.2f -> HELD after note-offs: %d", label, voiceLimit, sustain, leaked);
    if (leaked>0){ printf("  held notes: "); for(int i=0;i<vm.getVoiceLimit();++i){ auto&v=vm.getVoice(i);
        if(v.isActive()&&!v.isReleasing()) printf("%d ", v.getCurrentNote()); } }
    printf("  %s\n", leaked>0?"(see notes)":"ok");
}

int main(){
    for (int i=0;i<128;++i) g_tuning[i]=440.0f*std::pow(2.0f,(i-69)/12.0f);

    // Control: single-source external-MIDI legato (poly, ch1) — should release.
    scenario("ext-MIDI legato, single src (-1,ch1)", 4, 0.5f, {
        {1,60,false,-1,1},{1,64,false,-1,1},{1,67,false,-1,1},
        {0,67,false,-1,1},{0,64,false,-1,1},{0,60,false,-1,1} });

    // Control: same in mono.
    scenario("ext-MIDI legato mono (-1,ch1)", 1, 0.5f, {
        {1,60,false,-1,1},{1,64,false,-1,1},{0,64,false,-1,1},{0,60,false,-1,1} });

    // Computer keyboard (src 15, ch0) + a gen-strand BIND (src 0, ch0): cross-source.
    scenario("kbd(15) + strand-bind(0) glide", 4, 0.5f, {
        {1,60,false,15,0},{1,64,true,0,0},{0,60,false,15,0},{0,64,false,0,0} });
    scenario("kbd(15) + strand-bind(0) glide, sus=0", 4, 0.0f, {
        {1,60,false,15,0},{1,64,true,0,0},{0,60,false,15,0},{0,64,false,0,0} });

    // Two gen-seq strands binding across each other (src 0 and src 1, both ch0).
    scenario("strand0 note + strand1 bind glide", 4, 0.5f, {
        {1,60,false,0,0},{1,64,true,1,0},{0,60,false,0,0},{0,64,false,1,0} });

    // THE EXTERNAL-MIDI GAP the reviewer flagged: external key (src -1, ch5) held
    // while the STEP-SEQ slides (bind, src -1, ch0). Both are in the -1 bucket, so
    // only the origin-channel guard prevents the seq slide from hijacking the key.
    scenario("ext-key(ch5) + step-seq bind(ch0)", 4, 0.5f, {
        {1,60,false,-1,5},        // external key down -> voice (src-1, ch5)
        {1,64,true,-1,0},         // step-seq slide bind (src-1, ch0)
        {0,60,false,-1,5} });     // release the external key
    scenario("ext-key(ch5) + step-seq bind, sus=0", 4, 0.0f, {
        {1,60,false,-1,5},{1,64,true,-1,0},{0,60,false,-1,5} });

    return 0;
}
