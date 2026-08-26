// MEASUREMENT (no fabricated signal): what actually happens to the DCA envelope
// and voice allocation for a step-seq C3-C3 loop vs a manual note, at several
// gates. Drives the REAL StepSequencer + VoiceManager + SynthVoice. Reports the
// real event stream, the per-time DCA env level (getAmpEnvLevel = the amplitude
// the waveform shows), output peak, active-voice count, and every note-on's
// target voice (fresh vs reuse-of-active). Carrier is a plain steady sine — no
// onset, no trickery; the env is read directly, not inferred.
//
// Build (see audition_sampler_follow.cpp header):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/measure_voiceleading.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/measure_vl
#include "JuceHeader.h"
#include "dsp/VoiceManager.h"
#include "dsp/SamplePlayer.h"
#include "dsp/BlockParams.h"
#include "sequencer/StepSequencer.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <string>

using Artic = VoiceEvent::Articulation;
static float g_tuning[128];

static const char* artStr(Artic a){
    switch(a){ case Artic::Normal: return "Normal"; case Artic::Glide: return "Glide";
               case Artic::Bind: return "Bind"; } return "?";
}

struct Rig {
    static constexpr double SR = 44100.0; static constexpr int BS = 512;
    VoiceManager vm; SamplePlayer master; BlockParams bp;
    std::vector<float> lfo; juce::AudioBuffer<float> buf{2, BS};
    Rig() : lfo((size_t)BS, 0.0f) {
        vm.prepare(SR, BS); vm.setTuningTable(g_tuning);
        vm.setEngineMode(SynthVoice::EngineMode::Sampler); vm.setVoiceLimit(16);
        bp.ampAttack = 5000.0f; bp.ampDecay = 200.0f; bp.ampSustain = 1.0f; bp.ampRelease = 10000.0f;
        bp.velAmt = 1.0f; vm.setBlockParams(bp);
        const int L = (int)(SR * 30.0);
        juce::AudioBuffer<float> c(1, L);
        for (int i = 0; i < L; ++i) c.setSample(0, i, 0.7f * std::sin(2.0 * M_PI * 130.81 * i / SR));
        master.prepare(SR, BS); master.loadBuffer(c, SR);
        vm.distributeSamplerBuffer(master, 0.0f, false);
    }
    // env of the newest active voice + total active count (read AFTER a render)
    void probe(float& env, int& active){
        env = 0.0f; active = 0;
        uint64_t ts = 0;
        for (int v = 0; v < vm.getVoiceLimit(); ++v){
            auto& vv = vm.getVoice(v);
            if (vv.isActive()){ ++active; if (vv.noteOnTimestamp >= ts){ ts = vv.noteOnTimestamp; env = vv.getAmpEnvLevel(); } }
        }
    }
    float renderBlockPeak(int n){
        buf.clear();
        vm.renderBlock(buf, bp, lfo.data(), lfo.data(), lfo.data(), 0, n);
        float pk = 0.0f; for (int i = 0; i < n; ++i) pk = std::max(pk, std::fabs(buf.getSample(0, i)));
        return pk;
    }
    // render one block, capture (env, activeCount, outputPeak)
    void tick(std::vector<float>& env, std::vector<float>& peak, std::vector<int>& act){
        float p = renderBlockPeak(BS); float e; int a; probe(e, a);
        env.push_back(e); peak.push_back(p); act.push_back(a);
    }
};

// Describe which voice a note-on landed on, seen from before/after snapshots.
static void snapshot(VoiceManager& vm, int* note, bool* act, bool* rel){
    for (int v = 0; v < 16; ++v){ auto& vv = vm.getVoice(v);
        act[v] = vv.isActive(); rel[v] = vv.isReleasing(); note[v] = vv.getCurrentNote(); }
}

static void printEnvTimeline(const std::vector<float>& env, const std::vector<float>& peak,
                             const std::vector<int>& act, double dtMs)
{
    // print every ~50 ms: t, env, peak, activeCount
    printf("      t(ms)  DCAenv  outPk  nV\n");
    int stride = std::max(1, (int)std::lround(50.0 / dtMs));
    for (size_t i = 0; i < env.size(); i += (size_t)stride)
        printf("      %5.0f  %5.3f  %5.3f  %d\n", i * dtMs, env[i], peak[i], act[i]);
}

// ── Manual note: on, hold, off, ring out. Baseline for "clean release". ──
static void manual()
{
    Rig r;
    std::vector<float> env, peak; std::vector<int> act;
    printf("\n=== MANUAL: noteOn C3, hold 1.5s, noteOff, ring 12s (attack5s rel10s sus1.0) ===\n");
    r.vm.noteOn(60,0.9f,false,0.0f,false,false,false,-1,0.0f,0);
    { int n=(int)(Rig::SR*1.5/Rig::BS); for(int b=0;b<n;++b) r.tick(env,peak,act); }
    printf("   -> noteOff at t=%.0fms\n", env.size()*(Rig::BS/Rig::SR*1000.0));
    r.vm.noteOff(60,-1);
    { int n=(int)(Rig::SR*12.0/Rig::BS); for(int b=0;b<n;++b) r.tick(env,peak,act); }
    printEnvTimeline(env,peak,act, Rig::BS/Rig::SR*1000.0);
}

// ── Sequencer C3-C3 at a given gate, real event stream. ──
static void seqCase(float gate)
{
    Rig r;
    T5ynthStepSequencer seq; seq.prepare(Rig::SR, Rig::BS);
    seq.setNumSteps(2); seq.setBpm(120.0); seq.setDivision(3); // 1/8 -> 0.25s/step (default)
    for (int i=0;i<2;++i){ seq.setStepNote(i,60); seq.setStepEnabled(i,true); seq.setStepVelocity(i,0.8f);
        seq.setStepBindMode(i, T5ynthStepSequencer::BindMode::Off); }
    seq.setAllGates(gate);  // real global-gate path
    seq.start();

    printf("\n=== SEQ C3-C3  gate=%.2f  (1/8 @120 = 0.25s/step) : events + env ===\n", gate);
    std::vector<float> env, peak; std::vector<int> act;
    std::vector<VoiceEvent> evs;
    int evShown=0;
    double tms=0; const double dt = Rig::BS/Rig::SR*1000.0;
    int loopBlocks=(int)(Rig::SR*3.0/Rig::BS);
    int reuseCount=0, freshCount=0, offCount=0;
    for (int b=0;b<loopBlocks;++b){
        evs.clear(); r.buf.clear();
        seq.processBlock(r.buf, evs);
        int pos=0;
        for (auto& ev: evs){
            int off=juce::jlimit(0,Rig::BS,ev.sampleOffset);
            if(off>pos){ r.vm.renderBlock(r.buf,r.bp,r.lfo.data(),r.lfo.data(),r.lfo.data(),pos,off-pos); pos=off; }
            if(ev.type==VoiceEvent::Type::NoteOn){
                // does a note-on land on an already-active voice of this note?
                bool reuse=false;
                for(int v=0;v<16;++v){ auto& vv=r.vm.getVoice(v);
                    if(vv.isActive() && vv.getCurrentNote()==ev.note){ reuse=true; break; } }
                bool isBind=ev.artic!=Artic::Normal;
                float gl=(ev.artic==Artic::Bind)?0.0f:seq.getGlideTime();
                r.vm.noteOn(ev.note,ev.velocity,isBind,gl,false,false,false,ev.strandId,ev.pan,0);
                if(reuse)++reuseCount; else++freshCount;
                if(evShown++<10) printf("   t=%6.0fms  ON  note=%d artic=%-6s -> %s\n",
                    tms+off/Rig::SR*1000.0, ev.note, artStr(ev.artic), reuse?"REUSE active same-note voice":"fresh voice");
            } else {
                ++offCount;
                if(evShown++<10) printf("   t=%6.0fms  OFF note=%d\n", tms+off/Rig::SR*1000.0, ev.note);
                r.vm.noteOff(ev.note,ev.strandId);
            }
        }
        if(pos<Rig::BS) r.vm.renderBlock(r.buf,r.bp,r.lfo.data(),r.lfo.data(),r.lfo.data(),pos,Rig::BS-pos);
        float e; int a; float pkOut=0; for(int i=0;i<Rig::BS;++i) pkOut=std::max(pkOut,std::fabs(r.buf.getSample(0,i)));
        r.probe(e,a);
        env.push_back(e); peak.push_back(pkOut); act.push_back(a);
        tms+=dt;
    }
    // stop -> release tail
    seq.stop();
    { r.buf.clear(); std::vector<VoiceEvent> t; seq.processBlock(r.buf,t);
      for(auto&ev:t){ if(ev.type==VoiceEvent::Type::NoteOff){ r.vm.noteOff(ev.note,ev.strandId); ++offCount;
          printf("   t=%6.0fms  OFF note=%d  (sequencer STOP)\n", tms, ev.note);} } }
    int tail=(int)(Rig::SR*8.0/Rig::BS);
    for(int b=0;b<tail;++b){ r.tick(env,peak,act); tms+=dt; }

    printf("   SUMMARY gate=%.2f : noteOns fresh=%d reuse=%d  noteOffs=%d\n", gate, freshCount, reuseCount, offCount);
    printEnvTimeline(env,peak,act, dt);
}

// ── Sustain pedal down: repeat the SAME note. Note-off is deferred, so the
//    previous voice is still HELD (not releasing). Does a repeat stack a fresh
//    duplicate (voice exhaustion + level jump) or reuse the held voice? ──
static void sustainCase()
{
    Rig r;
    r.vm.setSustainPedal(true);
    printf("\n=== SUSTAIN pedal DOWN: play C3 x10 (press+release each) — stacking? ===\n");
    printf("   press#  heldVoices(nV)  outPk\n");
    for (int k=0;k<10;++k){
        r.vm.noteOn(60,0.9f,false,0.0f,false,false,false,-1,0.0f,0);
        // hold ~150ms
        { int n=(int)(Rig::SR*0.15/Rig::BS); for(int b=0;b<n;++b){ r.buf.clear();
            r.vm.renderBlock(r.buf,r.bp,r.lfo.data(),r.lfo.data(),r.lfo.data(),0,Rig::BS);} }
        r.vm.noteOff(60,-1);   // deferred by sustain -> voice stays HELD
        int active=0; for(int v=0;v<16;++v) if(r.vm.getVoice(v).isActive()) ++active;
        r.buf.clear(); r.vm.renderBlock(r.buf,r.bp,r.lfo.data(),r.lfo.data(),r.lfo.data(),0,Rig::BS);
        float pk=0; for(int i=0;i<Rig::BS;++i) pk=std::max(pk,std::fabs(r.buf.getSample(0,i)));
        printf("   #%2d      %2d              %.3f\n", k+1, active, pk);
    }
}

int main()
{
    for (int i=0;i<128;++i) g_tuning[i]=440.0f*std::pow(2.0f,(i-69)/12.0f);
    manual();
    seqCase(0.50f);
    seqCase(0.80f);
    seqCase(1.00f);
    sustainCase();
    return 0;
}
