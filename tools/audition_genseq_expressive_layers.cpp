// GUARD for the three restored GenSeq expressive layers (metric elasticity,
// stage panning, foreground velocity bias — restored 2026-07-16 after their
// unauthorized removal in b826a898) AND for strand 0's reference-voice
// contract. Drives the REAL T5ynthGenerativeSequencer for 120s and asserts:
//
//   A. Strand 0 stays a rigid reference clock: every inter-onset gap is an
//      integer multiple of the step duration (±3 samples), velocities stay
//      in the legacy 55/85/100 (±5 jitter) bands.
//   B. Secondary strands breathe: single-step inter-onset ratios vary
//      (elasticity alive) and stay inside the clamp band [0.84, 1.22].
//   C. Stage panning: every strand's pan MOVES over time, stays in ±0.95,
//      keeps its lane side (S1 left, S2 right, S3 left, S4 right), and the
//      centre actor S5 hovers around centre stage (|pan| small, both signs).
//   D. Foreground bias: secondary non-Gesture strands top out 5 below their
//      unbiased ceiling (mean+12+jitter), Gesture reaches its full ceiling.
//   E. Scale 0 (UI label "Chromatic") behaves chromatic — non-diatonic
//      pitch classes actually sound (it used to be coerced to Major).
//   F. Dialog coordination mode: stances are pendulum-driven and RECUR
//      (multiple transitions over time), strand 0 never has a stance,
//      Counter-stance strands fire measurably less while strand 0 sounds,
//      and outside Dialog mode all stances stay Independent.
//   G. Ensemble lattice: the group time balance keeps every secondary
//      strand's long-run offset vs the rigid S1 grid bounded (< 1 step
//      after 10 min; pre-fix it walked 3–5.5 steps) while per-step
//      timing still varies (the breathing survives).
//
// Build (see audition_sampler_follow.cpp header):
//   FLAGS=build_clean/CMakeFiles/T5ynth.dir/flags.make
//   { grep -m1 CXX_DEFINES "$FLAGS"; grep -m1 CXX_INCLUDES "$FLAGS"; } \
//     | sed 's/^CXX_[A-Z]* = //' > /tmp/h.rsp
//   echo -I$PWD/build_clean/_deps/signalsmith_stretch-src >> /tmp/h.rsp
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/audition_genseq_expressive_layers.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/genseq_layers && /tmp/genseq_layers
#include "JuceHeader.h"
#include "sequencer/GenerativeSequencer.h"
#include "dsp/VoiceEvent.h"
#include <cstdio>
#include <cmath>
#include <vector>
#include <set>
#include <algorithm>

static int g_failures = 0;
static void check(bool ok, const char* what)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

struct Ev { long pos; int strand; int vel; float pan; };

int main()
{
    constexpr double SR = 44100.0;
    constexpr int    BS = 512;
    constexpr double BPM = 120.0;
    constexpr double STEP = SR * 60.0 / BPM * 0.5; // division 3 => factor 0.5 => 11025

    T5ynthGenerativeSequencer seq;
    seq.prepare(SR, BS);
    seq.setBpm(BPM);
    seq.setDivision(3);
    seq.setGate(0.8f);
    seq.setShuffle(0.0f);
    seq.setScale(1, 0);   // C major
    seq.setRange(2);

    // Roles: S2=Anchor S3=Line S4=Density S5=Gesture (indices 1..4).
    for (int i = 1; i < T5ynthGenerativeSequencer::MAX_STRANDS; ++i)
    {
        seq.setStrandRole(i, i - 1);
        seq.setStrandEnabled(i, true);
    }
    seq.start();

    std::vector<Ev> ons;
    juce::AudioBuffer<float> buf(2, BS);
    std::vector<VoiceEvent> out;
    const long total = static_cast<long>(SR) * 120;
    for (long pos = 0; pos < total; pos += BS)
    {
        out.clear();
        seq.processBlock(buf, out);
        for (const auto& e : out)
            if (e.type == VoiceEvent::Type::NoteOn)
                ons.push_back({ pos + e.sampleOffset, e.strandId,
                                juce::roundToInt(e.velocity * 127.0f), e.pan });
    }

    // DensityBudget (default cap 3) drops/displaces low-priority notes by
    // design, so totals sit well below the theoretical 5-strand maximum.
    std::printf("collected %zu note-ons\n", ons.size());
    check(ons.size() > 1000, "enough events for statistics");

    for (int strand = 0; strand < T5ynthGenerativeSequencer::MAX_STRANDS; ++strand)
    {
        std::vector<Ev> mine;
        for (const auto& e : ons) if (e.strand == strand) mine.push_back(e);
        std::printf("strand %d: %zu note-ons\n", strand, mine.size());
        // Aliveness only, NOT a rate assertion: under DensityBudget cap 3 the
        // lowest-priority role is displaced by design and its count naturally
        // ranges ~110–160 in 120 s (measured pre- AND post-time-balance) —
        // the old threshold of 100 sat inside that spread and flaked.
        check(mine.size() > 60, "strand active");
        if (mine.size() < 2) { ++g_failures; continue; }

        // ── Timing ──
        double maxGridDev = 0.0;      // strand 0: deviation from step grid
        double minRatio = 9e9, maxRatio = -9e9; // secondaries: 1-step ratios
        int oneStepCount = 0;
        for (size_t i = 1; i < mine.size(); ++i)
        {
            const double delta = static_cast<double>(mine[i].pos - mine[i-1].pos);
            const int k = juce::jmax(1, juce::roundToInt(delta / STEP));
            if (strand == 0)
                maxGridDev = std::max(maxGridDev, std::abs(delta - k * STEP));
            else if (k == 1)
            {
                const double r = delta / STEP;
                minRatio = std::min(minRatio, r);
                maxRatio = std::max(maxRatio, r);
                ++oneStepCount;
            }
        }
        if (strand == 0)
        {
            std::printf("  grid deviation max %.2f samples\n", maxGridDev);
            check(maxGridDev <= 3.0, "S1 rigid on the step grid (no elasticity)");
        }
        else if (oneStepCount > 10)
        {
            std::printf("  1-step ratio %.4f..%.4f (n=%d)\n", minRatio, maxRatio, oneStepCount);
            check(minRatio >= 0.83 && maxRatio <= 1.23, "elastic ratios inside clamp band");
            const double spreadSamples = (maxRatio - minRatio) * STEP;
            check(spreadSamples >= (strand == 1 ? 5.0 : 10.0), "elasticity alive (timing varies)");
        }

        // ── Pan ──
        float panMin = 1.0f, panMax = -1.0f;
        std::set<int> distinctPan;
        for (const auto& e : mine)
        {
            panMin = std::min(panMin, e.pan);
            panMax = std::max(panMax, e.pan);
            distinctPan.insert(juce::roundToInt(e.pan * 10000.0f));
        }
        std::printf("  pan %.3f..%.3f (%zu distinct)\n", panMin, panMax, distinctPan.size());
        check(panMin >= -0.95f && panMax <= 0.95f, "pan bounded");
        check(distinctPan.size() > 10 && (panMax - panMin) >= 0.02f, "pan MOVES (stage alive)");
        switch (strand)
        {
            case 0: check(panMax <  0.01f, "S1 keeps left lane");            break;
            case 1: check(panMin > -0.01f, "S2 keeps right lane");           break;
            case 2: check(panMax <  0.01f, "S3 keeps left lane");            break;
            case 3: check(panMin > -0.01f, "S4 keeps right lane");           break;
            case 4: check(std::abs(panMin) <= 0.20f && std::abs(panMax) <= 0.20f
                          && panMin < 0.0f && panMax > 0.0f,
                          "S5 centre actor oscillates around centre");        break;
        }

        // ── Velocity ──
        int velMax = 0;
        bool velBandsOk = true;
        for (const auto& e : mine)
        {
            velMax = std::max(velMax, e.vel);
            if (strand == 0)
                velBandsOk = velBandsOk
                    && ((e.vel >= 50 && e.vel <= 60) || (e.vel >= 80 && e.vel <= 90)
                                                     || (e.vel >= 95 && e.vel <= 105));
        }
        std::printf("  vel max %d\n", velMax);
        if (strand == 0) check(velBandsOk, "S1 legacy velocity bands 55/85/100 +-5");
        if (strand == 1) check(velMax <= 92,  "Anchor ceiling biased (80+12-5+5)");
        if (strand == 2) check(velMax <= 100, "Line ceiling biased (88+12-5+5)");
        if (strand == 3) check(velMax <= 87,  "Density ceiling biased (75+12-5+5)");
        if (strand == 4) check(velMax >= 108 && velMax <= 112,
                               "Gesture UNbiased ceiling reached (95+12+5)");
    }

    // ── E. Chromatic scale honored (scale 0 = UI "Chromatic") ──
    {
        T5ynthGenerativeSequencer chrom;
        chrom.prepare(SR, BS);
        chrom.setBpm(BPM);
        chrom.setDivision(3);
        chrom.setGate(0.8f);
        chrom.setShuffle(0.0f);
        chrom.setScale(0, 0);   // Chromatic, root C
        chrom.setRange(2);
        chrom.setStrandRole(1, 1);   // one secondary Line strand (field path)
        chrom.setStrandEnabled(1, true);
        chrom.start();

        std::set<int> pcs0, pcs1;
        for (long pos = 0; pos < total; pos += BS)
        {
            out.clear();
            chrom.processBlock(buf, out);
            for (const auto& e : out)
                if (e.type == VoiceEvent::Type::NoteOn)
                    (e.strandId == 0 ? pcs0 : pcs1).insert(((e.note % 12) + 12) % 12);
        }
        auto nonDiatonic = [](const std::set<int>& pcs) {
            int n = 0;
            for (int pc : { 1, 3, 6, 8, 10 }) if (pcs.count(pc)) ++n;
            return n;
        };
        std::printf("chromatic: strand0 %zu pcs (%d non-diatonic), strand1 %zu pcs (%d non-diatonic)\n",
                    pcs0.size(), nonDiatonic(pcs0), pcs1.size(), nonDiatonic(pcs1));
        check(nonDiatonic(pcs0) >= 2, "Chromatic honored on strand 0 (non-diatonic pcs sound)");
        check(nonDiatonic(pcs1) >= 2, "Chromatic honored on secondary strand (field path)");
        check(pcs0.size() >= 9, "chromatic coverage broad on strand 0");
    }

    // Outside Dialog mode (the main run above used the DensityBudget
    // default), stances must never leave Independent.
    {
        bool allIndependent = true;
        for (int i = 0; i < T5ynthGenerativeSequencer::MAX_STRANDS; ++i)
            allIndependent = allIndependent
                && seq.stanceForGui[static_cast<size_t>(i)].load() == 0;
        check(allIndependent, "no stances outside Dialog mode");
    }

    // ── F. Dialog mode: pendulum stances ──
    {
        T5ynthGenerativeSequencer dlg;
        dlg.prepare(SR, BS);
        dlg.setBpm(BPM);
        dlg.setDivision(3);
        dlg.setGate(0.9f);
        dlg.setShuffle(0.0f);
        dlg.setScale(1, 0);
        dlg.setRange(2);
        dlg.setCoordinationMode(2);           // Dialog
        dlg.setStrandRole(1, 0);              // S2 = Anchor
        dlg.setStrandEnabled(1, true);
        dlg.setStrandRole(2, 1);              // S3 = Line
        dlg.setStrandEnabled(2, true);
        // x3 speed on the probes: since the group time balance (2026-07-17)
        // secondaries are phase-LOCKED to S1's grid, so at 1x their decision
        // moments sample only one fixed phase of S1's step — the "silent"
        // bucket then consists of the knife-edge gate tail and the measured
        // rates swing with the random seed (observed 0.14–1.38 fires/s run
        // to run). At x3 the decisions comb S1's phases {0, 1/3, 2/3} and
        // silence is sampled via S1's missed steps (whole-step windows) —
        // composition is structural, not seed-dependent.
        dlg.setStrandDivMult(1, 3.0f);
        dlg.setStrandDivMult(2, 3.0f);
        dlg.start();

        int  s1StanceSeen = 0;
        int  transitions[3] = { 0, 0, 0 };    // per strand 0..2
        int  lastStance[3]  = { 0, 0, 0 };
        std::set<int> distinctStances[3];
        // Per secondary strand: fires and dwell TIME (samples) bucketed by
        // (stance × strand-0-sounding). Counter's effect is CONDITIONAL —
        // ×0.70 while strand 0 sounds, +0.15 while it is silent — so an
        // aggregate rate over both conditions can legitimately be a wash;
        // the mechanism is the redistribution between them. Both counters
        // use the INSTANTANEOUS s1 state at the event's sampleOffset: the
        // strands are boundary-synchronized, so any block-granular proxy
        // (start- or end-of-block state) misattributes systematically, not
        // as noise — S1's short silent gap sits right before the shared
        // step boundary where the secondaries decide to fire.
        long fires[3][3][2] = {};             // [strand][stance][s1Sounding]
        long dwell[3][3][2] = {};             // samples spent in each bucket
        bool s1Sounding = false;              // tracked from strand-0 events

        const long dlgTotal = static_cast<long>(SR) * 600;
        for (long pos = 0; pos < dlgTotal; pos += BS)
        {
            out.clear();
            dlg.processBlock(buf, out);

            // Stances change at most once per phrase — block-granular stance
            // attribution is fine; only the s1 sounding state needs to be
            // sample-accurate.
            int curStance[3];
            for (int st = 0; st < 3; ++st)
            {
                curStance[st] = juce::jlimit(0, 2,
                    dlg.stanceForGui[static_cast<size_t>(st)].load());
                distinctStances[st].insert(curStance[st]);
                if (curStance[st] != lastStance[st]) { ++transitions[st]; lastStance[st] = curStance[st]; }
            }
            s1StanceSeen |= dlg.stanceForGui[0].load();

            // The engine emits events in temporal (sampleOffset) order,
            // interleaved across strands (ties resolve strand 0 first —
            // matching what the secondaries' fire decision actually saw in
            // strands[0].lastPlayedNote). Walk them once: accumulate dwell
            // spans between strand-0 toggles, and read the instantaneous
            // state at each secondary NoteOn.
            long cursor = 0;
            auto addDwell = [&](long upTo)
            {
                for (int st = 1; st < 3; ++st)
                    dwell[st][curStance[st]][s1Sounding ? 1 : 0] += upTo - cursor;
                cursor = upTo;
            };
            for (const auto& e : out)
            {
                if (e.strandId == 0)
                {
                    addDwell(e.sampleOffset);
                    if (e.type == VoiceEvent::Type::NoteOn)  s1Sounding = true;
                    if (e.type == VoiceEvent::Type::NoteOff) s1Sounding = false;
                }
                else if (e.type == VoiceEvent::Type::NoteOn && e.strandId <= 2)
                    ++fires[e.strandId][curStance[e.strandId]][s1Sounding ? 1 : 0];
            }
            addDwell(BS);
        }

        check(s1StanceSeen == 0, "strand 0 never takes a stance");
        for (int st = 1; st <= 2; ++st)
        {
            const long fI = fires[st][0][0] + fires[st][0][1];
            const long fF = fires[st][1][0] + fires[st][1][1];
            const long fC = fires[st][2][0] + fires[st][2][1];
            std::printf("dialog strand %d: stances=%zu transitions=%d  fires I/F/C = %ld/%ld/%ld\n",
                        st, distinctStances[st].size(), transitions[st], fI, fF, fC);
            check(distinctStances[st].size() >= 2, "stances actually change (pendulum alive)");
            check(transitions[st] >= 3, "stances RECUR over time");

            // Both halves of the Counter mechanism (fires per second of
            // dwell), plus the joint statement. NOTE the pulse-count
            // confound: a Counter stance means the pulse pendulum sits
            // ABOVE base, so Counter epochs have structurally more pulses
            // and a higher base fire rate — an absolute sounding-damping
            // threshold near the true ×0.70 is therefore knife-edge (0.90
            // observed 0.88–0.90). The confound cancels in the WITHIN-
            // stance ratio, so the primary assertion is redistribution:
            // Counter's sounding/silent rate ratio must sit well below
            // Independent's (observed ~0.66–0.68 vs threshold 0.85).
            auto rate = [&](int stance, int snd) {
                return static_cast<double>(fires[st][stance][snd]) * SR
                     / static_cast<double>(dwell[st][stance][snd]);
            };
            const long minDwell = static_cast<long>(SR) * 20;   // 20 s per bucket
            const bool haveAll = dwell[st][0][0] > minDwell && dwell[st][0][1] > minDwell
                              && dwell[st][2][0] > minDwell && dwell[st][2][1] > minDwell;
            if (haveAll)
            {
                std::printf("  while S1 sounds: fires/s independent=%.3f counter=%.3f\n",
                            rate(0, 1), rate(2, 1));
                std::printf("  while S1 silent: fires/s independent=%.3f counter=%.3f\n",
                            rate(0, 0), rate(2, 0));
                check(rate(2, 1) < 0.95 * rate(0, 1),
                      "Counter damping visible while strand 0 sounds");
                check(rate(2, 0) > 1.10 * rate(0, 0),
                      "Counter claims the floor while strand 0 is silent");
                const double shiftI = rate(0, 1) / rate(0, 0);
                const double shiftC = rate(2, 1) / rate(2, 0);
                std::printf("  sounding/silent ratio: independent=%.2f counter=%.2f\n",
                            shiftI, shiftC);
                check(shiftC < 0.85 * shiftI,
                      "Counter redistributes fire toward strand 0's silence");
            }
        }

        // ── Role change re-founds the stance (Rollenwechsel). A role switch
        //    rebuilds the pattern and zeroes the pulse-drift pendulum; a
        //    stance derived from the OLD identity must not outlive its
        //    decision basis. Wait for a non-Independent stance, flip the
        //    role, expect Independent within one step, then expect the
        //    pendulum to produce stances again. ──
        {
            auto runBlocks = [&](long maxSamples, auto stopWhen) -> bool
            {
                for (long p = 0; p < maxSamples; p += BS)
                {
                    out.clear();
                    dlg.processBlock(buf, out);
                    if (stopWhen()) return true;
                }
                return stopWhen();
            };
            auto s2Stance = [&] { return dlg.stanceForGui[1].load(); };

            const bool armed = runBlocks(static_cast<long>(SR) * 300,
                                         [&] { return s2Stance() != 0; });
            check(armed, "role-change probe reaches a non-Independent stance");
            if (armed)
            {
                dlg.setStrandRole(1, 3);   // Anchor -> Gesture mid-flight
                // Window MUST be far below one step (~11k samples here): the
                // rebuild fires at the top of the next block, so 4 blocks
                // suffice — while a 1 s window would also admit the ordinary
                // next-phrase-end decision (accum was zeroed → Independent)
                // and pass even WITHOUT the immediate reset. Verified by
                // ablation: with resetStance in rebuildPattern removed, 4
                // blocks fails 6/6 runs, 1 s still passed 5/6.
                check(runBlocks(static_cast<long>(BS) * 4, [&] { return s2Stance() == 0; }),
                      "role change resets the stance with its pendulum");
                check(runBlocks(static_cast<long>(SR) * 300, [&] { return s2Stance() != 0; }),
                      "stances re-emerge under the new role");
            }
        }
    }

    // ── G. Ensemble lattice: elasticity breathes but does NOT walk. The raw
    //    stretch moments sum positive, which (pre-2026-07-17) made every
    //    secondary strand run 0.1–0.25 % slow — ~5.5 steps adrift from the
    //    rigid S1 after 10 min (BJ: "im Gesamtergebnis chaotisch"). The group
    //    time balance must hold the offset bounded while per-step timing
    //    still varies. Setup: every step fires (16/16), all structural
    //    drifts fixed, Independent coordination — onsets sample the step
    //    clock directly. ──
    {
        T5ynthGenerativeSequencer ens;
        ens.prepare(SR, BS);
        ens.setBpm(BPM);
        ens.setDivision(3);
        ens.setGate(0.5f);
        ens.setShuffle(0.0f);
        ens.setScale(1, 0);
        ens.setRange(2);
        ens.setCoordinationMode(0);
        ens.setSteps(16); ens.setPulses(16);
        ens.setFixSteps(true); ens.setFixPulses(true);
        ens.setFixRotation(true); ens.setFixMutation(true);
        ens.setMutation(0.2f);
        for (int i = 1; i <= 4; ++i)
        {
            ens.setStrandRole(i, i - 1);          // Anchor, Line, Density, Gesture
            ens.setStrandSteps(i, 16);
            ens.setStrandPulses(i, 16);
            ens.setStrandFixSteps(i, true);
            ens.setStrandFixPulses(i, true);
            ens.setStrandFixRotation(i, true);
            ens.setStrandFixMutation(i, true);
            ens.setStrandMutation(i, 0.2f);
            ens.setStrandDivMult(i, 1.0f);
            ens.setStrandEnabled(i, true);
        }
        ens.start();

        std::vector<double> onsets[5];
        const long ensTotal = static_cast<long>(SR) * 600;
        for (long pos = 0; pos < ensTotal; pos += BS)
        {
            out.clear();
            ens.processBlock(buf, out);
            for (const auto& e : out)
                if (e.type == VoiceEvent::Type::NoteOn && e.strandId >= 0 && e.strandId < 5)
                    onsets[e.strandId].push_back((static_cast<double>(pos) + e.sampleOffset) / SR);
        }

        // Nominal grid from S1 (rigid by design). Provisional step = median
        // interval (robust against the ~2 % missed fires), then an exact
        // reconstruction: count steps between onsets by rounding.
        const auto& s1 = onsets[0];
        check(s1.size() > 2000, "lattice probe: S1 dense");
        std::vector<double> s1iv;
        for (size_t i = 1; i < s1.size(); ++i) s1iv.push_back(s1[i] - s1[i - 1]);
        std::nth_element(s1iv.begin(), s1iv.begin() + s1iv.size() / 2, s1iv.end());
        const double provisional = s1iv[s1iv.size() / 2];
        long s1Steps = 0;
        for (size_t i = 1; i < s1.size(); ++i)
        {
            const long k = std::lround((s1[i] - s1[i - 1]) / provisional);
            s1Steps += (k < 1 ? 1 : k);
        }
        const double nominal = (s1.back() - s1.front()) / static_cast<double>(s1Steps);

        for (int st = 1; st < 5; ++st)
        {
            const auto& on = onsets[st];
            if (on.size() < 1000) { check(false, "lattice probe: strand dense"); continue; }

            long stepCount = 0;
            double var = 0.0; long oneStepN = 0; double oneStepMean = 0.0;
            std::vector<double> oneStep;
            for (size_t i = 1; i < on.size(); ++i)
            {
                const double dt = on[i] - on[i - 1];
                const long k = std::lround(dt / nominal);
                stepCount += (k < 1 ? 1 : k);
                if (k == 1) oneStep.push_back(dt);
            }
            const double realizedMean = (on.back() - on.front()) / static_cast<double>(stepCount);
            // Bounded lattice: after 10 min the strand sits < 1 step from
            // where the rigid grid puts it (pre-fix: 3–5.5 steps).
            const double offsetSteps = (on.back() - on.front()) / nominal - static_cast<double>(stepCount);
            std::printf("lattice S%d: mean %.6f s (skew %+0.4f %%), offset@600s %+0.2f steps\n",
                        st + 1, realizedMean, (realizedMean / nominal - 1.0) * 100.0, -offsetSteps);
            check(std::abs(offsetSteps) < 1.0, "elasticity does not walk (offset bounded)");

            for (double v : oneStep) oneStepMean += v;
            oneStepMean /= static_cast<double>(oneStep.size());
            for (double v : oneStep) var += (v - oneStepMean) * (v - oneStepMean);
            const double sd = std::sqrt(var / static_cast<double>(oneStep.size()));
            oneStepN = static_cast<long>(oneStep.size());
            check(oneStepN > 500 && sd > nominal * 0.0005,
                  "elasticity still breathes (per-step timing varies)");
        }
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
