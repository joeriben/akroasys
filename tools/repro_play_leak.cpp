// Faithful "during play" leak hunt: REAL StepSequencer closed bind loop (poly),
// with manual keyboard chords (src 15) injected between blocks to force voice
// stealing — then release every manual key AND stop the seq, and report any voice
// still held. That is the only honest test of "a voice drops out until restart".
//
// Build: same flags as repro_bind_stuck.cpp (see /tmp/h.rsp recipe).
#include "JuceHeader.h"
#include "dsp/VoiceManager.h"
#include "dsp/BlockParams.h"
#include "sequencer/StepSequencer.h"
#include <cstdio>
#include <vector>
#include <cmath>

using Bind = T5ynthStepSequencer::BindMode;
static float g_tuning[128];
static const int KBD = 15;

static int heldCount(VoiceManager& vm){ int h=0; for(int i=0;i<vm.getVoiceLimit();++i){
    auto&v=vm.getVoice(i); if(v.isActive()&&!v.isReleasing())++h;} return h; }
static int activeCount(VoiceManager& vm){ int a=0; for(int i=0;i<vm.getVoiceLimit();++i)
    if(vm.getVoice(i).isActive())++a; return a; }

static void dump(const char* tag, VoiceManager& vm){
    printf("   %-22s voices: ", tag);
    for(int i=0;i<vm.getVoiceLimit();++i){ auto&v=vm.getVoice(i);
        printf("v%d{a=%d r=%d n=%d} ",i,v.isActive(),v.isReleasing(),v.getCurrentNote()); }
    printf("\n");
}

// chordSrc/chordCh model the manual player: computer-kbd = (15, ch0);
// external MIDI = (-1, ch 1..4 per note — the reviewer's flagged gap).
static void playTest(const char* who, int chordSrc, bool extChannels)
{
    for (int i=0;i<128;++i) g_tuning[i]=440.0f*std::pow(2.0f,(i-69)/12.0f);
    const double SR=44100.0; const int BS=512;
    VoiceManager vm; vm.prepare(SR,BS); vm.setTuningTable(g_tuning);
    vm.setEngineMode(SynthVoice::EngineMode::Freeze);
    vm.setVoiceLimit(4);
    BlockParams bp; bp.ampAttack=2.f; bp.ampDecay=60.f; bp.ampSustain=0.5f; bp.ampRelease=40.f;
    vm.setBlockParams(bp);
    printf("\n#### manual source = %s ####\n", who);

    // Closed 2-step bind loop on the step seq (the proven stuck pattern).
    T5ynthStepSequencer seq; seq.prepare(SR,BS);
    seq.setNumSteps(2); seq.setBpm(120.0); seq.setDivision(3);
    for(int i=0;i<2;++i){ seq.setStepNote(i,60+i); seq.setStepEnabled(i,true);
        seq.setStepGate(i,0.5f); seq.setStepVelocity(i,0.8f); seq.setStepBindMode(i,Bind::Bind); }
    seq.start();

    juce::AudioBuffer<float> buf(2,BS); std::vector<float> z((size_t)BS,0.f);
    std::vector<VoiceEvent> evs;
    auto runBlocks=[&](int n){ for(int b=0;b<n;++b){ buf.clear(); evs.clear();
        seq.processBlock(buf,evs); int pos=0;
        for(auto&e:evs){ int off=juce::jlimit(0,BS,e.sampleOffset);
            if(off>pos){ vm.renderBlock(buf,bp,z.data(),z.data(),z.data(),pos,off-pos); pos=off; }
            if(e.type==VoiceEvent::Type::NoteOn){ bool ib=e.artic!=VoiceEvent::Articulation::Normal;
                float g=(e.artic==VoiceEvent::Articulation::Bind)?0.f:seq.getGlideTime();
                vm.noteOn(e.note,e.velocity,ib,g,false,false,false,e.strandId,e.pan,0);}
            else vm.noteOff(e.note,e.strandId);
        }
        if(pos<BS) vm.renderBlock(buf,bp,z.data(),z.data(),z.data(),pos,BS-pos);
    }};

    printf("=== POLY closed-loop + manual chord stealing, then release all + STOP ===\n");
    runBlocks(60);                       // let the closed loop establish + go silent (pass 2+)
    dump("after loop runs", vm);

    // Play a 4-note manual chord on TOP of the running closed loop.
    int chord[4]={72,75,79,82};
    auto chCh=[&](int k){ return extChannels ? (k+1) : 0; }; // external: distinct MPE channels
    for(int k=0;k<4;++k){ vm.noteOn(chord[k],0.8f,false,0.f,false,false,false,chordSrc,0.f,chCh(k)); runBlocks(2); }
    dump("chord down (steal)", vm);
    runBlocks(20);

    // Release every manual key the user pressed.
    for(int k=0;k<4;++k){ vm.noteOff(chord[k],chordSrc,false,chCh(k)); runBlocks(2); }
    dump("chord released", vm);
    runBlocks(40);
    printf("   -> held after releasing all manual keys (seq still running): %d\n", heldCount(vm));

    // Now STOP the sequencer (it should flush a note-off for its held note).
    seq.stop();
    runBlocks(8);
    dump("after seq STOP", vm);
    runBlocks(200);
    int leaked = activeCount(vm);
    printf("   -> ACTIVE voices after release-all + STOP + long render: %d  %s\n",
        leaked, leaked>0 ? "<<< LEAK (needs panic/restart)" : "ok (recovered)");

    // For contrast: does an explicit panic recover whatever is left?
    vm.allNotesOff(); runBlocks(200);
    printf("   -> ACTIVE after allNotesOff(panic): %d\n", activeCount(vm));
}

int main(){
    playTest("computer keyboard (src 15, ch0)", KBD,  false);
    playTest("external MIDI (src -1, ch 1..4)", -1,   true);
    return 0;
}
