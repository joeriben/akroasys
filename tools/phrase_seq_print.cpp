// Drives the REAL T5ynthPhraseSequencer offline, prints what it plays bar by
// bar, and asserts the properties the design rests on
// (docs/PHRASE_SEQ_CONCEPT.md):
//
//   A. No hanging voice: every note-on is ended (note-off or glide hand-over)
//      and allNotesOff leaves nothing sounding; no note-off names a note that
//      is not sounding on its strand.
//   B. The main line (strand −1) is monophonic for Bass / Riff / Lead.
//   C. Determinism (§3.9), Pedal and Chords: a locate into bar k — at the bar
//      start and mid-bar — plays the same onsets (time, note, velocity,
//      strand) as continuous playback from bar 0.
//   D. Bass: an onset on a bar's downbeat is the root of that bar's chord.
//   E. In a diatonic key every out-of-scale note is a chromatic approach: the
//      next note on its strand lies a semitone above.
//   F. Evolve 0 repeats every phrase exactly; Evolve > 0 changes the motif
//      across phrases.
//   G. Glide: every Glide note-on continues a held note on its own strand.
//   H. Keep stores the motif heard (latched Evolve, current phrase), so
//      playback goes on unchanged; a motif diced while stopped is heard as
//      itself where playback starts.
//
// Build (JUCE-free, the sequencer has no dependencies), from the repo root:
//   g++ -std=c++17 -O1 -Isrc tools/phrase_seq_print.cpp
//       src/sequencer/PhraseModel.cpp src/sequencer/PhraseSequencer.cpp
//       -o /tmp/phrase_seq_print && /tmp/phrase_seq_print [--quiet]

#include "sequencer/PhraseSequencer.h"
#include "sequencer/ScaleQuantizer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace PhraseModel;

namespace
{
constexpr double kSr = 48000.0;
constexpr int kBlock = 512;
constexpr double kBpm = 120.0;

int failures = 0;
bool quiet = false;

// An onset exactly on a block boundary may land on the previous block's last
// sample (offsets truncate). One sample of tolerance keeps it in its bar.
constexpr double kOneSampleQ = kBpm / (60.0 * kSr);

int barOf(double q, double barLenQ)
{
    return static_cast<int>(std::floor((q + 1.5 * kOneSampleQ) / barLenQ));
}

void check(bool ok, const std::string& what)
{
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

std::string noteName(int n)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return std::string(names[n % 12]) + std::to_string(n / 12 - 1);
}

char fnChar(const StepView& v, int memberGuess)
{
    switch (v.fn)
    {
        case Fn::Rest:              return '.';
        case Fn::ChordTone:         return "R357"[std::clamp(memberGuess, 0, 3)];
        case Fn::Repeat:            return '=';
        case Fn::Octave:            return 'o';
        case Fn::Passing:           return 'p';
        case Fn::NeighbourUp:       return 'n';
        case Fn::NeighbourDown:     return 'u';
        case Fn::ApproachUp:        return '/';
        case Fn::ApproachDown:      return '\\';
        case Fn::ApproachChromatic: return 'c';
        default:                    return '?';
    }
}

/** A chord tone's member, recovered from its pitch against the bar's chord. */
int memberOf(int note, const Chord& c)
{
    const int iv = ((note - c.rootPc) % 12 + 12) % 12;
    for (int m = 0; m < kMaxVoices; ++m)
        if (c.iv[static_cast<size_t>(m)] >= 0 && c.iv[static_cast<size_t>(m)] % 12 == iv) return m;
    return 0;
}

struct Played
{
    double q;          // absolute quarters
    int sample;        // absolute sample
    VoiceEvent ev;
};

struct Run
{
    std::vector<Played> events;
    std::map<int, std::string> barViews;   // bar → view line
    int hangs = 0, orphanOffs = 0, polyMain = 0, badGlides = 0;
    int sounding = 0;
};

/** Simulates the voice allocator's view: (note, strand) instances. */
struct VoiceSim
{
    std::vector<std::pair<int, int>> live;   // (note, strand)
    int orphanOffs = 0, badGlides = 0, polyMain = 0;
    void on(const VoiceEvent& e)
    {
        if (e.artic == VoiceEvent::Articulation::Glide)
        {
            for (auto it = live.rbegin(); it != live.rend(); ++it)
                if (it->second == e.strandId) { it->first = e.note; return; }
            ++badGlides;
        }
        live.push_back({ e.note, e.strandId });
        if (e.strandId == -1)
        {
            int n = 0;
            for (auto& v : live) n += v.second == -1;
            if (n > 1) ++polyMain;
        }
    }
    void off(const VoiceEvent& e)
    {
        for (auto it = live.begin(); it != live.end(); ++it)
            if (it->first == e.note && it->second == e.strandId) { live.erase(it); return; }
        ++orphanOffs;
    }
};

/** Glide note-ons that share their tick and strand with another note-on. The
 *  processor's sort keeps no order among note-ons of one tick, so such a glide
 *  continues the held voice with whichever of them it dispatches first. */
int sharedGlides(const std::vector<VoiceEvent>& out)
{
    int n = 0;
    for (const auto& g : out)
    {
        if (g.type != VoiceEvent::Type::NoteOn || g.artic != VoiceEvent::Articulation::Glide) continue;
        for (const auto& o : out)
            if (&o != &g && o.type == VoiceEvent::Type::NoteOn && o.sampleOffset == g.sampleOffset
                && o.strandId == g.strandId) { ++n; break; }
    }
    return n;
}

/** Called before every block with the block's start position. */
using Hook = std::function<void(T5ynthPhraseSequencer&, double)>;

/** Runs `bars` bars. startQ ≥ 0 → host-synced from that position (a locate);
 *  the sequencer is fresh either way. */
Run play(const Config& cfg, double startQ, double untilQ, int tsNum = 4, int tsDen = 4,
         const std::vector<std::pair<double, std::vector<int>>>& keys = {},
         std::uint32_t diceSeed = 0, const Hook& hook = {})
{
    T5ynthPhraseSequencer seq;
    seq.prepare(kSr);
    seq.setTimeSignature(tsNum, tsDen);
    seq.setConfig(cfg);
    if (diceSeed) seq.dice(cfg, diceSeed);
    seq.start();

    Run r;
    VoiceSim sim;
    std::vector<VoiceEvent> out;
    out.reserve(2048);
    const double dq = kBlock * kBpm / (60.0 * kSr);
    double q = startQ;
    long long blockSample = static_cast<long long>(std::llround(startQ * 60.0 * kSr / kBpm));
    int lastBar = -1;
    size_t keyIdx = 0;
    while (q < untilQ)
    {
        while (keyIdx < keys.size() && keys[keyIdx].first <= q)
        {
            seq.clearKeys();
            for (int n : keys[keyIdx].second) seq.keyOn(n);
            for (int n : keys[keyIdx].second) seq.keyOff(n);   // the latch keeps it
            ++keyIdx;
        }
        if (hook) hook(seq, q);
        out.clear();
        seq.processBlock(kBlock, kBpm, true, q, out);
        // Dispatch order as the processor: by offset, note-off first on a tie.
        std::stable_sort(out.begin(), out.end(), [](const VoiceEvent& a, const VoiceEvent& b) {
            if (a.sampleOffset != b.sampleOffset) return a.sampleOffset < b.sampleOffset;
            return a.type == VoiceEvent::Type::NoteOff && b.type == VoiceEvent::Type::NoteOn;
        });
        for (const auto& e : out)
        {
            if (e.sampleOffset < 0 || e.sampleOffset >= kBlock) ++r.hangs;   // out of block: count as error
            const long long s = blockSample + e.sampleOffset;
            r.events.push_back({ static_cast<double>(s) * kBpm / (60.0 * kSr), static_cast<int>(s), e });
            if (e.type == VoiceEvent::Type::NoteOn) sim.on(e); else sim.off(e);
        }
        const int b = seq.currentBarForGui.load();
        if (b != lastBar)
        {
            lastBar = b;
            std::array<StepView, kMaxBarSteps> view {};
            const int n = seq.readView(view);
            const std::uint32_t cw = seq.chordForGui.load();
            Chord c;
            c.rootPc = static_cast<int>(cw & 15u);
            c.iv[1] = static_cast<int>((cw >> 4) & 31u) - 1;
            c.iv[2] = static_cast<int>((cw >> 9) & 31u) - 1;
            c.iv[3] = static_cast<int>((cw >> 14) & 31u) - 1;
            std::string line;
            for (int k = 0; k < n; ++k)
            {
                const auto& v = view[static_cast<size_t>(k)];
                line += v.head ? '[' : ' ';
                line += fnChar(v, v.note >= 0 ? memberOf(v.note, c) : 0);
                line += v.accent ? '>' : (v.slide ? '~' : (v.head ? ']' : ' '));
            }
            static const char* pcs[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            char hdr[64];
            std::snprintf(hdr, sizeof hdr, "%-2s%s%s ", pcs[c.rootPc],
                          c.iv[1] == 3 ? "m" : (c.iv[1] == 4 ? "" : (c.iv[1] < 0 ? "5" : "sus")),
                          c.iv[3] >= 0 ? "7" : "");
            r.barViews[b] = std::string(hdr) + line;
        }
        q += dq;
        blockSample += kBlock;
    }
    out.clear();
    seq.allNotesOff(out, 0);
    for (const auto& e : out) sim.off(e);
    r.sounding = static_cast<int>(sim.live.size());
    r.orphanOffs = sim.orphanOffs;
    r.badGlides = sim.badGlides;
    r.polyMain = sim.polyMain;
    return r;
}

std::vector<Played> onsInBars(const Run& r, double barLenQ, int fromBar, int toBar, double fromQ = -1e9)
{
    std::vector<Played> v;
    for (const auto& p : r.events)
    {
        if (p.ev.type != VoiceEvent::Type::NoteOn) continue;
        const int b = barOf(p.q, barLenQ);
        if (b >= fromBar && b < toBar && p.q >= fromQ - 1.5 * kOneSampleQ) v.push_back(p);
    }
    return v;
}

void printBars(const Run& r, double barLenQ, int fromBar, int toBar, int spb)
{
    if (quiet) return;
    for (int b = fromBar; b < toBar; ++b)
    {
        auto it = r.barViews.find(b);
        std::printf("    bar %3d %s\n", b, it != r.barViews.end() ? it->second.c_str() : "");
        std::string notes = "            ";
        for (const auto& p : r.events)
        {
            if (p.ev.type != VoiceEvent::Type::NoteOn || p.ev.strandId != -1) continue;
            const int pb = barOf(p.q, barLenQ);
            if (pb != b) continue;
            const double inBar = std::max(0.0, p.q - pb * barLenQ);
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.2f:%s%s%s ", inBar / (4.0 / spb), noteName(p.ev.note).c_str(),
                          p.ev.velocity > 0.75f ? ">" : "",
                          p.ev.artic == VoiceEvent::Articulation::Glide ? "~" : "");
            notes += buf;
        }
        std::printf("%s\n", notes.c_str());
    }
}

bool sameOns(const std::vector<Played>& a, const std::vector<Played>& b, std::string& why)
{
    if (a.empty()) { why = "(nothing to compare)"; return false; }
    if (a.size() != b.size()) { why = "count " + std::to_string(a.size()) + " vs " + std::to_string(b.size()); return false; }
    for (size_t i = 0; i < a.size(); ++i)
    {
        const auto& x = a[i].ev; const auto& y = b[i].ev;
        if (std::abs(a[i].sample - b[i].sample) > 1 || x.note != y.note || x.strandId != y.strandId
            || std::abs(x.velocity - y.velocity) > 1e-6f)
        {
            char buf[160];
            std::snprintf(buf, sizeof buf, "#%zu: q %.4f %s s%d v%.3f  vs  q %.4f %s s%d v%.3f", i,
                          a[i].q, noteName(x.note).c_str(), x.strandId, x.velocity,
                          b[i].q, noteName(y.note).c_str(), y.strandId, y.velocity);
            why = buf;
            return false;
        }
    }
    return true;
}

void commonChecks(const Run& r, const Config& cfg, double barLenQ, int bars, const std::string& tag)
{
    check(r.hangs == 0, tag + ": every event offset inside its block");
    check(r.sounding == 0 && r.orphanOffs == 0,
          tag + ": no hanging voice, no orphan note-off (left " + std::to_string(r.sounding)
              + ", orphans " + std::to_string(r.orphanOffs) + ")");
    check(r.badGlides == 0, tag + ": every Glide continues a held note on its strand");
    if (cfg.part != Part::Stab)
        check(r.polyMain == 0, tag + ": main line monophonic (" + std::to_string(r.polyMain) + " overlaps)");

    // E. out-of-scale notes are chromatic approaches
    if (cfg.scaleType != ScaleQuantizer::Off)
    {
        const std::uint16_t mask = scaleMask(cfg.scaleType, cfg.scaleRoot);
        std::map<int, std::vector<const Played*>> byStrand;
        for (const auto& p : r.events)
            if (p.ev.type == VoiceEvent::Type::NoteOn) byStrand[p.ev.strandId].push_back(&p);
        int bad = 0, chrom = 0;
        for (auto& [s, v] : byStrand)
            for (size_t i = 0; i < v.size(); ++i)
            {
                const int n = v[i]->ev.note;
                if (mask & (1u << (n % 12))) continue;
                const bool approach = i + 1 < v.size() && v[i + 1]->ev.note == n + 1;
                if (approach) ++chrom; else ++bad;
            }
        check(bad == 0, tag + ": out-of-scale notes are chromatic approaches (" + std::to_string(chrom)
                            + " approaches, " + std::to_string(bad) + " stray)");
    }

    // D. Bass downbeat = chord root
    if (cfg.part == Part::Bass && cfg.harm != Harm::Keys)
    {
        int checked = 0, bad = 0;
        for (const auto& p : r.events)
        {
            if (p.ev.type != VoiceEvent::Type::NoteOn || p.ev.strandId != -1) continue;
            const int b = barOf(p.q, barLenQ);
            if (std::abs(p.q - b * barLenQ) > 1e-6) continue;
            ++checked;
            const int rootPc = (chordForBar(cfg, b).rootPc + cfg.transpose % 12 + 12) % 12;
            if (p.ev.note % 12 != rootPc) ++bad;
        }
        check(checked > 0 && bad == 0, tag + ": Bass downbeats are chord roots (" + std::to_string(checked)
                                           + " downbeats, " + std::to_string(bad) + " wrong)");
    }
    (void) bars;
}

void determinism(const Config& cfg, int locateBar, const std::string& tag)
{
    const double barLenQ = 4.0;
    const int toBar = locateBar + 6;
    const Run cont = play(cfg, 0.0, toBar * barLenQ);
    const Run loc = play(cfg, locateBar * barLenQ, toBar * barLenQ);
    std::string why;
    bool ok = sameOns(onsInBars(cont, barLenQ, locateBar, toBar), onsInBars(loc, barLenQ, locateBar, toBar), why);
    check(ok, tag + ": locate to bar " + std::to_string(locateBar) + " == continuous " + why);
    const double midQ = locateBar * barLenQ + 2.5;
    const Run mid = play(cfg, midQ, toBar * barLenQ);
    why.clear();
    ok = sameOns(onsInBars(cont, barLenQ, locateBar, toBar, midQ), onsInBars(mid, barLenQ, locateBar, toBar, midQ), why);
    check(ok, tag + ": locate mid-bar " + std::to_string(locateBar) + "+2.5q == continuous " + why);
}

/** The main line's notes of each bar, as a key for repetition checks. */
std::vector<std::vector<int>> barNotes(const Run& r, double barLenQ, int bars)
{
    std::vector<std::vector<int>> v(static_cast<size_t>(bars));
    for (const auto& p : r.events)
    {
        if (p.ev.type != VoiceEvent::Type::NoteOn || p.ev.strandId != -1) continue;
        const int b = barOf(p.q, barLenQ);
        if (b >= 0 && b < bars) v[static_cast<size_t>(b)].push_back(p.ev.note * 1000 + static_cast<int>(std::lround((p.q - b * barLenQ) * 100)));
    }
    return v;
}

Config base()
{
    Config c;
    c.part = Part::Bass;
    c.harm = Harm::Pedal;
    c.scaleType = ScaleQuantizer::Minor;
    c.scaleRoot = 9;
    c.stepsPerBar = 16;
    c.pulses = 5;
    c.rotation = 0;
    c.phraseBars = 8;
    c.form = Form::AAAB;
    c.variation = 0.5f;
    c.evolve = 0.0f;
    c.slide = 0.4f;
    c.gate = 0.8f;
    return c;
}

/** A note's own off: its onset + gate x its step (finishLine), before its
 *  line's next onset bounds it. For a config in 4/4 without Variation, where
 *  no slot is cut and no ratchet or fill note plays. */
double ownOffQ(double onQ, const Config& c)
{
    return onQ + std::max(1.0e-3, std::clamp(static_cast<double>(c.gate), 0.1, 1.0) * 4.0 / c.stepsPerBar);
}
} // namespace

int main(int argc, char** argv)
{
    quiet = argc > 1 && std::strcmp(argv[1], "--quiet") == 0;
    const double B = 4.0;

    std::printf("\n== 1. Bass, Pedal, A minor, 16/5, AAAB, Var 0.5, Slide 0.4\n");
    {
        const Config c = base();
        const Run r = play(c, 0.0, 16 * B);
        printBars(r, B, 0, 8, c.stepsPerBar);
        commonChecks(r, c, B, 16, "bass/pedal");
        determinism(c, 11, "bass/pedal");
    }

    std::printf("\n== 2. Bass, Chords i-VI-III-VII, 16/7 rot 2, Evolve 0.6\n");
    {
        Config c = base();
        c.harm = Harm::Chords;
        c.chordDegrees = { 1, 6, 3, 7 };
        c.pulses = 7; c.rotation = 2;
        c.evolve = 0.6f;
        const Run r = play(c, 0.0, 32 * B);
        printBars(r, B, 0, 9, c.stepsPerBar);
        commonChecks(r, c, B, 32, "bass/chords");
        determinism(c, 13, "bass/chords");
        determinism(c, 17, "bass/chords");
        const auto bn = barNotes(r, B, 32);
        check(bn[0] != bn[8] || bn[1] != bn[9] || bn[2] != bn[10], "bass/chords: Evolve 0.6 changes phrase 1 against phrase 0");
    }

    std::printf("\n== 3. Riff, Chords i-iv, 16/6, Loop, Evolve 0 — phrases repeat\n");
    {
        Config c = base();
        c.part = Part::Riff;
        c.harm = Harm::Chords;
        c.chordDegrees = { 1, 4, 0, 0 };
        c.chordRateBars = 2;
        c.pulses = 6;
        c.form = Form::Loop;
        c.variation = 0.0f;
        const Run r = play(c, 0.0, 24 * B);
        printBars(r, B, 0, 4, c.stepsPerBar);
        commonChecks(r, c, B, 24, "riff/chords");
        const auto bn = barNotes(r, B, 24);
        bool rep = true;
        for (int b = 0; b < 8; ++b) rep = rep && bn[static_cast<size_t>(b)] == bn[static_cast<size_t>(b + 8)] && bn[static_cast<size_t>(b)] == bn[static_cast<size_t>(b + 16)];
        check(rep, "riff/chords: Evolve 0, Variation 0 → every phrase identical");
        determinism(c, 9, "riff/chords");
    }

    std::printf("\n== 4. Lead, Chords i-VII-VI-VII, 8/5, ABAB, Slide 0.8, Answer layer\n");
    {
        Config c = base();
        c.part = Part::Lead;
        c.harm = Harm::Chords;
        c.chordDegrees = { 1, 7, 6, 7 };
        c.stepsPerBar = 8; c.pulses = 5;
        c.form = Form::ABAB;
        c.slide = 0.8f;
        c.layer = Layer::Answer;
        c.len = 2;
        const Run r = play(c, 0.0, 16 * B);
        printBars(r, B, 0, 8, c.stepsPerBar);
        commonChecks(r, c, B, 16, "lead/answer");
        determinism(c, 5, "lead/answer");
        int glides = 0, answer = 0;
        for (const auto& p : r.events)
        {
            if (p.ev.type != VoiceEvent::Type::NoteOn) continue;
            glides += p.ev.artic == VoiceEvent::Articulation::Glide;
            answer += p.ev.strandId == 5;
        }
        check(glides > 0, "lead/answer: Slide 0.8 produces glides (" + std::to_string(glides) + ")");
        check(answer > 0, "lead/answer: the Answer layer plays in B units (" + std::to_string(answer) + " notes)");
    }

    std::printf("\n== 5. Stab, Chords i-VI-III-VII, 8/3, Octave layer\n");
    {
        Config c = base();
        c.part = Part::Stab;
        c.harm = Harm::Chords;
        c.chordDegrees = { 1, 6, 3, 7 };
        c.stepsPerBar = 8; c.pulses = 3;
        c.layer = Layer::Octave;
        c.ext7 = true;
        const Run r = play(c, 0.0, 8 * B);
        printBars(r, B, 0, 4, c.stepsPerBar);
        commonChecks(r, c, B, 8, "stab/octave");
        determinism(c, 6, "stab/octave");
        std::map<int, int> perOnset;
        for (const auto& p : r.events)
            if (p.ev.type == VoiceEvent::Type::NoteOn) ++perOnset[p.sample];
        int minV = 99, maxV = 0;
        for (auto& [s, n] : perOnset) { minV = std::min(minV, n); maxV = std::max(maxV, n); }
        check(minV == 8 && maxV == 8, "stab/octave: 4 voices + 4 octave-layer voices per hit (" + std::to_string(minV) + ".." + std::to_string(maxV) + ")");
        // The chord is the main line: all four voices on strand −1, so the
        // voice manager pans, folds and pedals them alike; the layer on 5..8.
        int mainOn = 0, layerOn = 0, other = 0;
        for (const auto& p : r.events)
        {
            if (p.ev.type != VoiceEvent::Type::NoteOn) continue;
            if (p.ev.strandId == -1) ++mainOn;
            else if (p.ev.strandId >= 5 && p.ev.strandId <= 8) ++layerOn;
            else ++other;
        }
        check(other == 0 && mainOn == layerOn && mainOn > 0,
              "stab/octave: the whole chord on strand -1, the layer on 5..8 (" + std::to_string(mainOn) + " / "
                  + std::to_string(layerOn) + " / " + std::to_string(other) + " elsewhere)");
    }

    std::printf("\n== 6. Riff, Keys: Am, then F at bar 2 beat 2\n");
    {
        Config c = base();
        c.part = Part::Riff;
        c.harm = Harm::Keys;
        c.layer = Layer::Chord;
        const std::vector<std::pair<double, std::vector<int>>> keys {
            { 0.0, { 57, 60, 64 } }, { 2 * B + 1.3, { 53, 57, 60 } } };
        const Run r = play(c, 0.0, 6 * B, 4, 4, keys);
        printBars(r, B, 0, 4, c.stepsPerBar);
        commonChecks(r, c, B, 6, "riff/keys");
        bool fFromBeat3 = true, sawF = false;
        for (const auto& p : r.events)
        {
            if (p.ev.type != VoiceEvent::Type::NoteOn || p.ev.strandId < 5) continue;
            if (p.q >= 2 * B + 2.0 - 1e-6 && p.q < 3 * B) { sawF = true; fFromBeat3 = fFromBeat3 && (p.ev.note % 12 == 5 || p.ev.note % 12 == 9 || p.ev.note % 12 == 0); }
        }
        check(sawF && fFromBeat3, "riff/keys: the F chord applies from the next beat (bar 2 beat 3)");
    }

    std::printf("\n== 7. Bass, Chords, 3/4 and 7/8\n");
    {
        Config c = base();
        c.harm = Harm::Chords;
        c.chordDegrees = { 1, 6, 0, 0 };
        const Run r34 = play(c, 0.0, 8 * 3.0, 3, 4);
        printBars(r34, 3.0, 0, 3, c.stepsPerBar);
        commonChecks(r34, c, 3.0, 8, "bass/3-4");
        const Run r78 = play(c, 0.0, 8 * 3.5, 7, 8);
        printBars(r78, 3.5, 0, 3, c.stepsPerBar);
        commonChecks(r78, c, 3.5, 8, "bass/7-8");
        double maxIn = 0.0;
        for (const auto& p : r34.events)
            if (p.ev.type == VoiceEvent::Type::NoteOn)
                maxIn = std::max(maxIn, p.q - barOf(p.q, 3.0) * 3.0);
        check(maxIn < 3.0, "bass/3-4: every onset inside its 3-quarter bar");
    }

    std::printf("\n== 8. Dice / Keep / Undo on the message-thread API\n");
    {
        Config c = base();
        c.evolve = 0.5f;
        T5ynthPhraseSequencer seq;
        const Motif m0 = seq.getStoredMotif();
        seq.dice(c, 1234);
        const Motif m1 = seq.getStoredMotif();
        check(m1.seed == 1234 && m1.len == 1, "dice: the stored motif carries the new seed and the cfg's shape");
        seq.undo();
        check(seq.getStoredMotif().seed == m0.seed, "undo: back to the motif before dice");
        seq.undo();
        check(seq.getStoredMotif().seed == 1234, "undo twice: redo");
        const Run a = play(c, 0.0, 8 * B, 4, 4, {}, 1234);
        const Run b2 = play(c, 0.0, 8 * B, 4, 4, {}, 99);
        check(barNotes(a, B, 8) != barNotes(b2, B, 8), "dice: two seeds play two different motifs");
    }

    std::printf("\n== 9. Live changes: config, motif, keys and locates while notes sound\n");
    {
        // Every few blocks something changes — part, layer, slide, grid,
        // harmony, a dice, a host jump — and the voice simulation must stay
        // clean throughout. Overlaps on strand −1 are allowed here (a Stab
        // chord still sounding when the part turns to Bass is not a hang).
        std::uint32_t rng = 0x5eed;
        auto rnd = [&](int n) { rng = rng * 1664525u + 1013904223u; return static_cast<int>((rng >> 8) % static_cast<std::uint32_t>(n)); };
        T5ynthPhraseSequencer seq;
        seq.prepare(kSr);
        Config c = base();
        c.harm = Harm::Chords;
        c.chordDegrees = { 1, 6, 3, 7 };
        seq.setConfig(c);
        seq.start();
        VoiceSim sim;
        std::vector<VoiceEvent> out;
        out.reserve(2048);
        const double dq = kBlock * kBpm / (60.0 * kSr);
        double q = 0.0;
        int changes = 0, jumps = 0, maxEvents = 0, shared = 0;
        for (int blk = 0; blk < 40000; ++blk)
        {
            if (rnd(6) == 0)
            {
                ++changes;
                switch (rnd(12))
                {
                    case 0: c.part = static_cast<Part>(rnd(4)); break;
                    case 1: c.layer = static_cast<Layer>(rnd(4)); break;
                    case 2: c.slide = rnd(2) ? 0.9f : 0.0f; break;
                    case 3: c.stepsPerBar = 1 << rnd(5); c.pulses = 1 + rnd(c.stepsPerBar * c.len); break;
                    case 4: c.harm = static_cast<Harm>(rnd(3)); break;
                    case 5: c.len = 1 + rnd(2); break;
                    case 6: c.rotation = rnd(16); c.pulses = 1 + rnd(16); break;
                    case 7: c.evolve = rnd(2) ? 0.8f : 0.0f; c.variation = rnd(2) ? 1.0f : 0.3f; break;
                    case 8: seq.dice(c, static_cast<std::uint32_t>(rnd(100000) + 1)); break;
                    case 9: { seq.clearKeys(); const int r = 48 + rnd(12); seq.keyOn(r); seq.keyOn(r + 4); seq.keyOn(r + 7); break; }
                    case 10: q = rnd(400) * 0.25; ++jumps; break;   // host locate
                    case 11: c.gate = rnd(2) ? 1.0f : 0.2f; c.shuffle = rnd(2) ? 0.6f : 0.0f; break;
                }
                seq.setConfig(c);
            }
            out.clear();
            seq.processBlock(kBlock, kBpm, true, q, out);
            maxEvents = std::max(maxEvents, static_cast<int>(out.size()));
            shared += sharedGlides(out);
            std::stable_sort(out.begin(), out.end(), [](const VoiceEvent& a, const VoiceEvent& b) {
                if (a.sampleOffset != b.sampleOffset) return a.sampleOffset < b.sampleOffset;
                return a.type == VoiceEvent::Type::NoteOff && b.type == VoiceEvent::Type::NoteOn;
            });
            for (const auto& e : out) { if (e.type == VoiceEvent::Type::NoteOn) sim.on(e); else sim.off(e); }
            q += dq;
        }
        const size_t before = sim.live.size();
        out.clear();
        seq.allNotesOff(out, 0);
        for (const auto& e : out) sim.off(e);
        std::printf("  %d changes, %d locates, at most %d events in one block, %zu sounding before allNotesOff\n",
                    changes, jumps, maxEvents, before);
        check(sim.live.empty() && sim.orphanOffs == 0,
              "live: no hanging voice, no orphan note-off (left " + std::to_string(sim.live.size())
                  + ", orphans " + std::to_string(sim.orphanOffs) + ")");
        check(sim.badGlides == 0, "live: every Glide continues a held note (" + std::to_string(sim.badGlides) + " bad)");
        check(shared == 0, "live: no Glide shares its tick with another note-on of its strand (" + std::to_string(shared) + ")");
        check(before <= 16, "live: never more than 16 notes sounding at the end (" + std::to_string(before) + ")");
    }

    std::printf("\n== 10. Shuffle moves onsets, never removes one (ratchets, bars cut short)\n");
    {
        // A shuffled onset or a ratchet repeat at or past the bar end used to
        // be lost: the scheduler enters the next bar first. Shuffle must leave
        // the number of onsets in every bar unchanged.
        auto countsPerBar = [&](const Config& cfg, int bars, int num, int den) {
            const double len = 4.0 * num / den;
            const Run r = play(cfg, 0.0, bars * len, num, den);
            std::vector<size_t> n;
            for (int b = 0; b < bars; ++b) n.push_back(onsInBars(r, len, b, b + 1).size());
            return n;
        };
        struct Case { const char* tag; Part part; int spb, pulses, rot, num, den; };
        const Case cases[] = {
            { "bass 1/4 2 rot1 4/4 (ratchet on the last step)", Part::Bass, 4, 2, 1, 4, 4 },
            { "riff 1/16 4 rot3 4/4 (ratchet on step 15)",      Part::Riff, 16, 4, 3, 4, 4 },
            { "bass 1/2 2 3/4 (last slot cut to one quarter)",  Part::Bass, 2, 2, 0, 3, 4 },
            { "riff 1/8 5 7/8",                                 Part::Riff, 8, 5, 0, 7, 8 },
        };
        for (const Case& k : cases)
        {
            Config c = base();
            c.part = k.part;
            c.stepsPerBar = k.spb; c.pulses = k.pulses; c.rotation = k.rot;
            c.variation = 0.8f;
            c.phraseBars = 4; c.form = Form::Loop;
            c.slide = 0.0f;
            const auto straight = countsPerBar(c, 16, k.num, k.den);
            bool same = true;
            for (float sh : { 0.5f, 0.75f })
            {
                c.shuffle = sh;
                same = same && countsPerBar(c, 16, k.num, k.den) == straight;
            }
            check(same, std::string("shuffle: ") + k.tag + ": onsets per bar unchanged at Shuffle 0.5 / 0.75");
        }
    }

    std::printf("\n== 11. Keep and Dice anchor to what plays (Bass, 4-bar phrases, Evolve 1)\n");
    {
        Config c = base();
        c.phraseBars = 4;
        c.form = Form::Loop;
        c.evolve = 1.0f;
        Config c0 = c;
        c0.evolve = 0.0f;
        const double end = 28 * B;
        // A run's actions, each taken once, at the first block at or after its
        // position. Built fresh for every run.
        using Act = std::function<void(T5ynthPhraseSequencer&)>;
        auto acts = [](std::vector<std::pair<double, Act>> list) {
            auto done = std::make_shared<std::vector<bool>>(list.size(), false);
            return Hook([list, done](T5ynthPhraseSequencer& s, double q) {
                for (size_t i = 0; i < list.size(); ++i)
                    if (!(*done)[i] && q >= list[i].first) { list[i].second(s); (*done)[i] = true; }
            });
        };
        const Act dice777  = [&](T5ynthPhraseSequencer& s) { s.dice(c, 777); };
        const Act stopKeep = [](T5ynthPhraseSequencer& s) { s.stop(); s.keep(); s.start(); };
        const Act evoOff   = [&](T5ynthPhraseSequencer& s) { s.setConfig(c0); };
        const Act diceStop = [&](T5ynthPhraseSequencer& s) { s.stop(); s.dice(c, 4242); s.start(); };
        const double keepQ = 21 * B + 1.0;   // phrase 5, mid-bar; the dice at bar 4 has origin 1
        auto same = [&](const Run& a, const Run& b, int from, int to, double fromQ, const std::string& what) {
            std::string why;
            const bool ok = sameOns(onsInBars(a, B, from, to, fromQ), onsInBars(b, B, from, to, fromQ), why);
            check(ok, what + " " + why);
        };

        // Equal as stored motifs, origin aside.
        auto sameMotif = [](const Motif& a, const Motif& b) {
            if (a.len != b.len || a.stepsPerBar != b.stepsPerBar || a.pulses != b.pulses
                || a.rotation != b.rotation || a.part != b.part || a.seed != b.seed) return false;
            for (size_t i = 0; i < a.slots.size(); ++i)
            {
                const Slot& x = a.slots[i];
                const Slot& y = b.slots[i];
                if (x.fn != y.fn || x.member != y.member || x.octave != y.octave
                    || x.accent != y.accent || x.slide != y.slide) return false;
            }
            return true;
        };
        // What phrase 5 plays after the dice at bar 4 (origin 1), under Evolve 1.
        const Motif heard5 = evolvedMotif(fitMotif(generateMotif(777, 1, c), c), c, 5);
        check(!sameMotif(heard5, generateMotif(777, 1, c)),
              "keep: phrases 2-5 evolved the diced motif (else the checks below prove nothing)");

        const Run cont = play(c, 0.0, end, 4, 4, {}, 0, acts({ { 4 * B, dice777 } }));
        Motif kept;
        const Act keepSeen = [&](T5ynthPhraseSequencer& s) { s.keep(); kept = s.getStoredMotif(); };
        same(cont, play(c, 0.0, end, 4, 4, {}, 0, acts({ { 4 * B, dice777 }, { keepQ, keepSeen } })),
             21, 28, keepQ, "keep while running: this phrase and the next play on unchanged");
        check(sameMotif(kept, heard5) && kept.origin == 5,
              "keep while running: stores the motif heard in phrase 5, origin 5 (origin "
                  + std::to_string(kept.origin) + ")");
        same(cont, play(c, 0.0, end, 4, 4, {}, 0, acts({ { 4 * B, dice777 }, { keepQ, stopKeep } })),
             21, 28, keepQ, "stop, keep, play on from the same bar: unchanged");

        // Evolve turned to 0 mid-phrase is latched at the next phrase entry:
        // Keep stores the motif evolved under 1, the one that plays.
        // From the next phrase on, Evolve 0 plays the stored motif as it is:
        // the kept one, as a run that stores it from the start plays it.
        const Run keptEvoOff = play(c, 0.0, end, 4, 4, {}, 0,
                                    acts({ { 4 * B, dice777 }, { 21 * B, evoOff }, { keepQ, keepSeen } }));
        same(play(c, 0.0, end, 4, 4, {}, 0, acts({ { 4 * B, dice777 }, { 21 * B, evoOff } })),
             keptEvoOff, 21, 24, keepQ,
             "keep after Evolve changed mid-phrase: the rest of the phrase plays on unchanged");
        check(sameMotif(kept, heard5) && kept.origin == 5,
              "keep after Evolve changed mid-phrase: stores the motif heard under the latched Evolve");
        same(play(c0, 0.0, end, 4, 4, {}, 0, acts({ { 0.0, [heard5](T5ynthPhraseSequencer& s) {
                 s.setStoredMotif(heard5); } } })),
             keptEvoOff, 24, 28, -1e9, "keep, then Evolve 0 from the next phrase: the kept motif plays on");

        // Keep in a phrase before the stored motif's origin (a Dice further on,
        // then a rewind): the motif plays unevolved there, and Keep must not
        // pull the origin back, or the phrases up to it would start evolving.
        const Motif at4 = generateMotif(777, 4, c);
        const Act load4 = [at4](T5ynthPhraseSequencer& s) { s.setStoredMotif(at4); };
        const double earlyQ = 9 * B + 1.0;   // phrase 2
        same(play(c, 0.0, end, 4, 4, {}, 0, acts({ { 0.0, load4 } })),
             play(c, 0.0, end, 4, 4, {}, 0, acts({ { 0.0, load4 }, { earlyQ, keepSeen } })),
             9, 28, earlyQ, "keep before the origin: every phrase plays on unchanged");
        check(kept.origin == 4, "keep before the origin: the origin stays 4, got " + std::to_string(kept.origin));
        const Act stopKeepSeen = [&](T5ynthPhraseSequencer& s) {
            s.stop();
            s.keep();
            kept = s.getStoredMotif();
            s.start();
        };
        const Act stopStart = [](T5ynthPhraseSequencer& s) { s.stop(); s.start(); };
        same(play(c, 0.0, end, 4, 4, {}, 0, acts({ { 0.0, load4 }, { earlyQ, stopStart } })),
             play(c, 0.0, end, 4, 4, {}, 0, acts({ { 0.0, load4 }, { earlyQ, stopKeepSeen } })),
             9, 28, earlyQ, "stop, keep before the origin, play on from the same bar: unchanged");
        check(kept.origin == 4, "stop, keep before the origin: the origin stays 4, got "
                                    + (kept.origin == kOriginAtNextEntry ? std::string("next entry")
                                                                         : std::to_string(kept.origin)));

        // Keep in the first bar of a phrase after the origin. The bar's entry
        // is led from the phrase before; under the kept motif (origin = this
        // phrase) that phrase plays the kept motif itself, not what was heard
        // there. The kept motif carries the motif heard there as its lead-in,
        // so every later rendering of the bar — the re-render Keep itself
        // causes, Undo + Redo, a stop and start on the spot, a locate back
        // into the phrase (a host cycle) — plays it as heard. And a later
        // change that leaves the motifs alone (Gate, Variation, Form, Harm,
        // the chord loop and rate, the time signature) plays as the same
        // change does without the Keep, played on or located into.
        {
            Config cc = base();
            cc.part = Part::Lead;
            cc.harm = Harm::Chords;
            cc.chordDegrees = { 1, 6, 3, 7 };
            cc.layer = Layer::Off;
            cc.len = 1;
            cc.stepsPerBar = 16;
            cc.pulses = 6;
            cc.rotation = 2;
            cc.phraseBars = 4;
            cc.form = Form::AAAB;
            cc.evolve = 1.0f;
            cc.variation = 0.0f;
            const Motif f = fitMotif(generateMotif(2293, 1, cc), cc);
            const Motif h1 = evolvedMotif(f, cc, 1), h2 = evolvedMotif(f, cc, 2);
            BarRender scratch;
            int p1 = -1, pp1 = -1, p2 = -1, pp2 = -1;
            entryNotes(cc, h1, h2, 8, B, scratch, p1, pp1);
            entryNotes(cc, h2, h2, 8, B, scratch, p2, pp2);
            check(p1 != p2 || pp1 != pp2,
                  "keep in a phrase's first bar: its entry differs when led from the kept motif "
                  "(else the checks below prove nothing)");

            struct Tally { int differ = 0; std::string why; };
            Tally plain, redo, spot, locate, before, changed, changedLocate, dropped, metre, metreDropped, mid;
            int runs = 0, changeRuns = 0, metreRuns = 0, midRuns = 0;
            auto tallyIn = [&](Tally& t, double barLen, const Run& a, const Run& b, int from, int to, double fromQ) {
                std::string why;
                if (!sameOns(onsInBars(a, barLen, from, to, fromQ), onsInBars(b, barLen, from, to, fromQ), why))
                    if (t.differ++ == 0) t.why = why;
            };
            auto tally = [&](Tally& t, const Run& a, const Run& b, int from, int to, double fromQ) {
                tallyIn(t, B, a, b, from, to, fromQ);
            };
            const Act undo = [](T5ynthPhraseSequencer& s) { s.undo(); };
            const Act stopStart = [](T5ynthPhraseSequencer& s) { s.stop(); s.start(); };
            for (Harm h : { Harm::Pedal, Harm::Chords })
                for (std::uint32_t seed : { 2293u, 777u, 4242u, 31337u, 182150u })
                {
                    Config hc = cc;
                    hc.harm = h;
                    // Changes the motifs do not read (sameMotifInputs).
                    std::vector<Config> changes;
                    {
                        Config gate = hc, varied = hc, form = hc, harm = hc, loop = hc, rate = hc;
                        gate.gate = 0.5f;
                        varied.variation = 0.8f;
                        form.form = Form::ABAB;
                        harm.harm = h == Harm::Pedal ? Harm::Chords : Harm::Pedal;
                        loop.chordDegrees = { 1, 4, 5, 1 };   // Chords: bar 7 plays I, not vii
                        rate.chordRateBars = 2;
                        changes = { gate, varied, form, harm, loop, rate };
                        if (h == Harm::Pedal)
                        {
                            // Chords on a one-chord loop of I: the same chords
                            // as the pedal, another Harm source.
                            Config tonic = hc;
                            tonic.harm = Harm::Chords;
                            tonic.chordDegrees = { 1, 0, 0, 0 };
                            changes.push_back(tonic);
                        }
                    }
                    const Motif at1 = generateMotif(seed, 1, hc);
                    const Act load1 = [at1](T5ynthPhraseSequencer& s) { s.setStoredMotif(at1); };
                    auto locate8 = [&](const Config& vc, const Motif& m) {
                        return play(vc, 8 * B, 12 * B, 4, 4, {}, 0,
                                    acts({ { 0.0, [m](T5ynthPhraseSequencer& s) { s.setStoredMotif(m); } } }));
                    };
                    const Run ref = play(hc, 0.0, 12 * B, 4, 4, {}, 0, acts({ { 0.0, load1 } }));
                    for (double off = 0.0; off < B; off += 0.25)
                    {
                        const double kq = 8 * B + off;   // bar 8 = first bar of phrase 2
                        ++runs;
                        tally(plain, ref, play(hc, 0.0, 12 * B, 4, 4, {}, 0, acts({ { 0.0, load1 }, { kq, keepSeen } })),
                              8, 12, kq);
                        const Motif keptHere = kept;
                        Motif noLeadIn = keptHere;
                        noLeadIn.leadInKept = false;
                        tally(redo, ref,
                              play(hc, 0.0, 12 * B, 4, 4, {}, 0,
                                   acts({ { 0.0, load1 }, { kq, keepSeen }, { kq + 0.25, undo }, { kq + 0.5, undo } })),
                              8, 12, kq);
                        tally(spot, play(hc, 0.0, 12 * B, 4, 4, {}, 0, acts({ { 0.0, load1 }, { kq, stopStart } })),
                              play(hc, 0.0, 12 * B, 4, 4, {}, 0, acts({ { 0.0, load1 }, { kq, stopKeep } })), 8, 12, kq);
                        tally(locate, ref, locate8(hc, keptHere), 8, 12, 8 * B);
                        // Located into the phrase before, which plays the
                        // kept motif itself, and played on into the origin
                        // phrase: it enters as heard there too.
                        tally(before, ref,
                              play(hc, 7 * B, 12 * B, 4, 4, {}, 0, acts({ { 0.0, [keptHere](T5ynthPhraseSequencer& s) {
                                       s.setStoredMotif(keptHere); } } })),
                              8, 12, 8 * B);

                        for (const Config& vc : changes)
                        {
                            ++changeRuns;
                            const Act change = [vc](T5ynthPhraseSequencer& s) { s.setConfig(vc); };
                            tally(changed,
                                  play(hc, 0.0, 12 * B, 4, 4, {}, 0, acts({ { 0.0, load1 }, { kq + 1.0, change } })),
                                  play(hc, 0.0, 12 * B, 4, 4, {}, 0,
                                       acts({ { 0.0, load1 }, { kq, keepSeen }, { kq + 1.0, change } })),
                                  8, 12, kq);
                            const Run without = locate8(vc, at1);
                            tally(changedLocate, without, locate8(vc, keptHere), 8, 12, 8 * B);
                            // Without its lead-in the kept motif would enter
                            // differently there: else the checks prove nothing.
                            tally(dropped, without, locate8(vc, noLeadIn), 8, 12, 8 * B);
                        }

                        // Kept in 4/4, located into in 3/4.
                        ++metreRuns;
                        auto locate8In3 = [&](const Motif& m) {
                            return play(hc, 8 * 3.0, 12 * 3.0, 3, 4, {}, 0,
                                        acts({ { 0.0, [m](T5ynthPhraseSequencer& s) { s.setStoredMotif(m); } } }));
                        };
                        const Run without3 = locate8In3(at1);
                        tallyIn(metre, 3.0, without3, locate8In3(keptHere), 8, 12, 8 * 3.0);
                        tallyIn(metreDropped, 3.0, without3, locate8In3(noLeadIn), 8, 12, 8 * 3.0);
                    }

                    // A change inside phrase 2 after its first bar, a Keep
                    // later in the phrase: a locate back to bar 8 in that
                    // config plays as it does without the Keep.
                    for (const Config& vc : changes)
                    {
                        const Act change = [vc](T5ynthPhraseSequencer& s) { s.setConfig(vc); };
                        const Run without = locate8(vc, at1);
                        for (double changeQ : { 8 * B + 1.0, 9 * B + 0.5 })
                            for (double keepAt : { 10 * B + 1.0, 11 * B + 1.5 })
                            {
                                ++midRuns;
                                play(hc, 0.0, 12 * B, 4, 4, {}, 0,
                                     acts({ { 0.0, load1 }, { changeQ, change }, { keepAt, keepSeen } }));
                                tally(mid, without, locate8(vc, kept), 8, 12, 8 * B);
                            }
                    }
                }
            auto report = [&](const Tally& t, int of, const std::string& what) {
                check(t.differ == 0, "keep in a phrase's first bar, " + what + " (" + std::to_string(t.differ) + " of "
                                         + std::to_string(of) + " differ) " + t.why);
            };
            auto matters = [&](const Tally& t, int of, const std::string& what) {
                check(t.differ > 0, "keep in a phrase's first bar: " + what + " (" + std::to_string(t.differ) + " of "
                                        + std::to_string(of) + " differ)");
            };
            // Stop and Keep in phrase 2, playback resumes in phrase 3. The
            // lead-in preceded phrase 2; phrase 3 enters from the phrase
            // before, as the same motif without the lead-in does.
            {
                Tally elsewhere, claimed;
                int resumeRuns = 0;
                const Act stopKeepStay = [&](T5ynthPhraseSequencer& s) { s.stop(); s.keep(); kept = s.getStoredMotif(); };
                for (Harm h : { Harm::Pedal, Harm::Chords })
                    for (std::uint32_t seed : { 2293u, 777u, 4242u, 31337u, 182150u, 15851u })
                    {
                        Config hc = cc;
                        hc.harm = h;
                        const Motif at1 = generateMotif(seed, 1, hc);
                        auto resumeIn3 = [&](const Motif& m) {
                            return play(hc, 12 * B, 16 * B, 4, 4, {}, 0,
                                        acts({ { 0.0, [m](T5ynthPhraseSequencer& s) { s.setStoredMotif(m); } } }));
                        };
                        for (double off = 0.0; off < 2 * B; off += 0.5)
                        {
                            ++resumeRuns;
                            play(hc, 0.0, 8 * B + off + 0.01, 4, 4, {}, 0,
                                 acts({ { 0.0, [at1](T5ynthPhraseSequencer& s) { s.setStoredMotif(at1); } },
                                        { 8 * B + off, stopKeepStay } }));
                            const Motif stopped = kept;
                            Motif noLeadIn = stopped, forced = stopped;
                            noLeadIn.leadInKept = false;
                            forced.leadInPhrase = 3;
                            const Run without = resumeIn3(noLeadIn);
                            tally(elsewhere, without, resumeIn3(stopped), 12, 16, 12 * B);
                            // The same lead-in claimed for phrase 3 does apply
                            // there: else the check above proves nothing.
                            tally(claimed, without, resumeIn3(forced), 12, 16, 12 * B);
                        }
                    }
                check(elsewhere.differ == 0,
                      "stop and keep in phrase 2, resume in phrase 3: it enters as without the lead-in ("
                          + std::to_string(elsewhere.differ) + " of " + std::to_string(resumeRuns) + " differ) "
                          + elsewhere.why);
                check(claimed.differ > 0,
                      "stop and keep in phrase 2: the lead-in claimed for phrase 3 would change it ("
                          + std::to_string(claimed.differ) + " of " + std::to_string(resumeRuns) + " differ)");
            }
            report(plain, runs, "the rest of the bar and the phrase play on unchanged");
            report(redo, runs, "then Undo, Redo: unchanged");
            report(spot, runs, "stop, keep, start on the spot: as a stop and start");
            report(locate, runs, "a locate back into the phrase plays it as heard");
            report(before, runs, "a locate into the phrase before, played on into it: as heard");
            report(changed, changeRuns,
                   "then Gate / Variation / Form / Harm / chord loop / chord rate later in the bar: as the change alone");
            report(changedLocate, changeRuns, "then such a change, a locate back into the phrase: as without the Keep");
            matters(dropped, changeRuns, "without its lead-in the kept motif would enter otherwise after such a change");
            report(metre, metreRuns, "kept in 4/4, located into in 3/4: as without the Keep");
            matters(metreDropped, metreRuns, "without its lead-in the kept motif would enter otherwise in 3/4");
            report(mid, midRuns, "a change inside the phrase, Keep later in it: a locate back to its first bar plays "
                                 "as without the Keep");
        }

        // Dice while stopped, playback resumes at bar 20 (phrase 5): the new
        // motif is heard as itself there, so Evolve cannot have touched it yet.
        Motif resolved;
        const Hook diced = acts({ { 20 * B, diceStop } });
        const Run dicedEvo = play(c, 0.0, end, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
            diced(s, q);
            resolved = s.getStoredMotif();
        });
        same(play(c0, 0.0, 24 * B, 4, 4, {}, 0, acts({ { 20 * B, diceStop } })), dicedEvo,
             20, 24, -1e9, "dice while stopped, start at bar 20: the new motif plays unevolved");
        check(resolved.origin == 5, "dice while stopped: origin resolves to the phrase first heard (5), got "
                                        + std::to_string(resolved.origin));

        // Dice again and Undo it: the first motif comes back with the origin
        // it was heard from (5), not anchored anew at the phrase of the Undo.
        // Compared from the next bar on: in the bar of the Undo the replaced
        // motif's held note still slides into the re-rendered line.
        const double undoQ = 25 * B + 1.0;
        same(dicedEvo,
             play(c, 0.0, end, 4, 4, {}, 0, acts({ { 20 * B, diceStop },
                                                   { 22 * B, [&](T5ynthPhraseSequencer& s) { s.dice(c, 31337); } },
                                                   { undoQ, [](T5ynthPhraseSequencer& s) { s.undo(); } } })),
             26, 28, -1e9, "undo after a later dice: the motif diced while stopped evolves on from its own origin");

        T5ynthPhraseSequencer seq;
        const Motif m0 = seq.getStoredMotif();
        seq.dice(c, 4242);
        check(seq.getStoredMotif().origin == kOriginAtNextEntry, "dice before any start: origin waits for the next entry");
        seq.keep();
        check(seq.getStoredMotif().seed == 4242, "keep before the motif was heard: the stored motif stays");
        seq.undo();
        check(seq.getStoredMotif().seed == m0.seed, "keep before the motif was heard: adds no undo level");

        // From Step reads at most two bars of the grid. Steps enabled only
        // beyond them give it nothing to analyse: it changes nothing, adds
        // no undo level, and says so.
        T5ynthPhraseSequencer fs;
        fs.dice(c, 555);
        fs.undo();                                   // back to the initial motif, Undo now redoes the dice
        fs.undo();                                   // the diced motif again
        const Motif before = fs.getStoredMotif();
        std::array<StepInput, 64> steps {};
        for (int i = 0; i < 64; ++i) steps[static_cast<size_t>(i)] = { 57, i >= 2 * c.stepsPerBar, false, 0.8f };
        check(!fs.fromSteps(steps.data(), 64, c), "from step, no step in the analysed bars: refused");
        check(fs.getStoredMotif().seed == before.seed && fs.getStoredMotif().len == before.len
                  && fs.getStoredMotif().pulses == before.pulses,
              "from step, no step in the analysed bars: the stored motif stays");
        fs.undo();
        check(fs.getStoredMotif().seed == m0.seed, "from step, no step in the analysed bars: adds no undo level");
        fs.undo();
        check(fs.getStoredMotif().seed == before.seed && before.seed == 555u,
              "from step, no step in the analysed bars: Undo still redoes the dice");
        steps[0].enabled = true;
        check(fs.fromSteps(steps.data(), 64, c) && fs.getStoredMotif().pulses == 1,
              "from step, one step in the analysed bars: stored with one pulse");
    }

    std::printf("\n== 12. A held slide note meets a Stab chord (Part switched mid-line)\n");
    {
        // Lead with slides, then Part turns to Stab inside a bar. A held note
        // must end at the chord's onset rather than glide into one of its
        // voices: dispatched here with the note-ons of one tick in reverse
        // emit order, which the processor's sort is free to do.
        int shared = 0, endedAtChord = 0, left = 0, orphans = 0, bad = 0;
        for (int seed = 1; seed <= 64; ++seed)
        {
            Config c = base();
            c.part = Part::Lead;
            c.harm = Harm::Chords;
            c.chordDegrees = { 1, 6, 3, 7 };
            c.stepsPerBar = 8; c.pulses = 6;
            c.slide = 1.0f;
            T5ynthPhraseSequencer seq;
            seq.prepare(kSr);
            seq.setConfig(c);
            seq.dice(c, static_cast<std::uint32_t>(seed));
            seq.start();
            VoiceSim sim;
            std::map<std::pair<int, int>, double> startedAt;   // (note, strand) → q of its onset
            std::vector<VoiceEvent> out;
            out.reserve(2048);
            const double dq = kBlock * kBpm / (60.0 * kSr);
            const double switchQ = 2 * B + (seed % 16) * 0.25 + 0.01;
            bool switched = false;
            for (double q = 0.0; q < 6 * B; q += dq)
            {
                if (!switched && q >= switchQ) { c.part = Part::Stab; seq.setConfig(c); switched = true; }
                out.clear();
                seq.processBlock(kBlock, kBpm, true, q, out);
                shared += sharedGlides(out);
                std::reverse(out.begin(), out.end());
                std::stable_sort(out.begin(), out.end(), [](const VoiceEvent& a, const VoiceEvent& b) {
                    if (a.sampleOffset != b.sampleOffset) return a.sampleOffset < b.sampleOffset;
                    return a.type == VoiceEvent::Type::NoteOff && b.type == VoiceEvent::Type::NoteOn;
                });
                for (const auto& e : out)
                {
                    const double eq = q + e.sampleOffset * kOneSampleQ;
                    if (e.type == VoiceEvent::Type::NoteOff)
                    {
                        // A lead note from before the switch, ended on the tick of a chord onset.
                        auto it = startedAt.find({ e.note, e.strandId });
                        int chordOns = 0;
                        for (const auto& o : out)
                            chordOns += o.type == VoiceEvent::Type::NoteOn && o.strandId == -1
                                        && o.sampleOffset == e.sampleOffset;
                        if (switched && e.strandId == -1 && it != startedAt.end() && it->second < switchQ
                            && chordOns > 1) ++endedAtChord;
                        sim.off(e);
                    }
                    else
                    {
                        // A glide continues a voice: it keeps that voice's onset.
                        double from = eq;
                        if (e.artic == VoiceEvent::Articulation::Glide)
                            for (auto it = sim.live.rbegin(); it != sim.live.rend(); ++it)
                                if (it->second == e.strandId) { from = startedAt[*it]; break; }
                        startedAt[{ e.note, e.strandId }] = from;
                        sim.on(e);
                    }
                }
            }
            out.clear();
            seq.allNotesOff(out, 0);
            for (const auto& e : out) sim.off(e);
            left += static_cast<int>(sim.live.size());
            orphans += sim.orphanOffs;
            bad += sim.badGlides;
        }
        std::printf("  64 seeds: %d lead notes ended at a chord onset\n", endedAtChord);
        check(endedAtChord > 0, "held into stab: the case occurs (" + std::to_string(endedAtChord) + " times)");
        check(shared == 0, "held into stab: no Glide shares its tick with another note-on of its strand ("
                               + std::to_string(shared) + ")");
        check(left == 0 && orphans == 0 && bad == 0,
              "held into stab: same-tick note-ons reversed, no hanging voice, no orphan off, no bad glide (left "
                  + std::to_string(left) + ", orphans " + std::to_string(orphans) + ", bad " + std::to_string(bad) + ")");
    }

    std::printf("\n== 13. A Fill note never starts on a step's onset (shuffle, odd meters)\n");
    {
        // Shuffle moves an odd step into a later 16th, which may be one the
        // Fill runs on: a Fill note there started on (or a hair beside) the
        // step's onset, two note-ons of a monophonic line on one tick.
        const int meters[][2] = { { 4, 4 }, { 3, 4 }, { 7, 8 }, { 9, 16 }, { 5, 8 } };
        const int spbs[] = { 4, 8, 16 };
        const float shuffles[] = { 0.2f, 0.25f, 0.5f, 0.75f };
        const Part parts[] = { Part::Bass, Part::Lead };
        int runs = 0, sameTick = 0, hangs = 0;
        for (const auto& mt : meters)
            for (int spb : spbs)
                for (float sh : shuffles)
                    for (Part part : parts)
                        for (int pulses : { std::max(1, spb / 2), spb * 3 / 4, spb })
                        for (int rot : { 0, 1 })
                        for (std::uint32_t seed = 1; seed <= 2; ++seed)
                        {
                            Config c = base();
                            c.part = part;
                            c.stepsPerBar = spb;
                            c.pulses = pulses;
                            c.rotation = rot;
                            c.shuffle = sh;
                            c.variation = 1.0f;
                            c.slide = 1.0f;
                            c.phraseBars = 4;
                            const double barLenQ = mt[0] * 4.0 / mt[1];
                            const Run r = play(c, 0.0, 16 * barLenQ, mt[0], mt[1], {}, seed);
                            ++runs;
                            hangs += r.sounding + r.orphanOffs + r.badGlides;
                            std::map<int, int> onsAt;   // sample → main-line note-ons
                            for (const auto& pl : r.events)
                                if (pl.ev.type == VoiceEvent::Type::NoteOn && pl.ev.strandId == -1)
                                    sameTick += onsAt[pl.sample]++ > 0;
                        }
        check(sameTick == 0, "fill vs shuffle: no two main-line note-ons on one tick over "
                                 + std::to_string(runs) + " runs (" + std::to_string(sameTick) + ")");
        check(hangs == 0, "fill vs shuffle: no hanging voice, orphan off or bad glide (" + std::to_string(hangs) + ")");
    }

    std::printf("\n== 14. A layer leaves the main line's glides alone\n");
    {
        // A layer doubles the line on its own strand, on the same onsets. An
        // onset shared with another strand still continues the held note.
        int glidesOff = 0, glidesLayer = 0;
        for (std::uint32_t seed = 1; seed <= 8; ++seed)
            for (Layer layer : { Layer::Off, Layer::Octave })
            {
                Config c = base();
                c.part = Part::Lead;
                c.harm = Harm::Chords;
                c.chordDegrees = { 1, 7, 6, 7 };
                c.stepsPerBar = 8; c.pulses = 5;
                c.slide = 0.8f;
                c.layer = layer;
                const Run r = play(c, 0.0, 16 * B, 4, 4, {}, seed);
                int g = 0;
                for (const auto& pl : r.events)
                    g += pl.ev.type == VoiceEvent::Type::NoteOn && pl.ev.strandId == -1
                         && pl.ev.artic == VoiceEvent::Articulation::Glide;
                (layer == Layer::Off ? glidesOff : glidesLayer) += g;
            }
        check(glidesOff > 0 && glidesLayer == glidesOff,
              "octave layer: main-line glides unchanged (" + std::to_string(glidesLayer) + " with the layer, "
                  + std::to_string(glidesOff) + " without)");
    }

    std::printf("\n== 15. A change in play never repeats an onset the last block played\n");
    {
        // A block that starts a hair past an onset: the block before played
        // it, and a re-render (here Gate, which moves no onset) must resume
        // behind it. Host positions are arbitrary doubles; so is an internal
        // clock that sums block lengths.
        const double dq = kBlock * kBpm / (60.0 * kSr);
        int dup = 0, runs = 0, errs = 0;
        for (Part part : { Part::Bass, Part::Lead, Part::Stab })
            for (double past : { 1.0e-12, 1.0e-10, 5.0e-10 })
                for (int bar : { 8, 9, 13 })
                {
                    Config c = base();
                    c.part = part;
                    c.rotation = 0;   // step 0 is a pulse: an onset on every bar start
                    Config g = c;
                    g.gate = 0.5f;
                    const double at = bar * B;
                    const double startQ = at - 40 * dq + past;
                    bool changed = false;
                    const Run ref = play(c, startQ, at + 2 * B);
                    const Run k = play(c, startQ, at + 2 * B, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                        if (!changed && q > at) { s.setConfig(g); changed = true; }
                    });
                    const auto a = onsInBars(ref, B, bar, bar + 2, at), b = onsInBars(k, B, bar, bar + 2, at);
                    bool downbeat = false;
                    for (const auto& p : a) downbeat = downbeat || (p.ev.strandId == -1 && std::abs(p.q - at) < 2 * kOneSampleQ);
                    std::string why;
                    ++runs;
                    if (!downbeat || !sameOns(a, b, why)) ++dup;
                    errs += (part == Part::Stab ? 0 : k.polyMain) + k.sounding + k.orphanOffs + k.badGlides;
                }
        check(dup == 0, "a re-render a hair past an onset plays the same onsets (" + std::to_string(dup) + " of "
                            + std::to_string(runs) + " differ or lack the downbeat)");
        check(errs == 0, "a re-render a hair past an onset: no doubled, hanging or orphan note ("
                             + std::to_string(errs) + ")");
    }

    std::printf("\n== 16. A slide holds only into a step the next bar plays\n");
    {
        // One step per bar, in the last beat, with a forced slide (From
        // Step). The B bar (AAAB, a bar per unit) varies it: its Fill runs on
        // the free 16ths of that beat, its Drop silences the beat. The slide
        // before it finds no step onset to glide into — a Fill note comes
        // first, or nothing — so its note ends at its gated length. Into a bar
        // that plays the step first it glides.
        const float fillVel = 72.0f / 127.0f;   // PhraseModel's kVelFill
        int fillGlides = 0, stepGlides = 0, fillNotes = 0, silentBars = 0, heldIntoNothing = 0, errs = 0, runs = 0;
        std::string whyFill, whyHeld;
        for (int k : { 12, 13, 14, 15 })
            for (float var : { 0.6f, 1.0f })        // Fill; Fill or Drop by the seed's coin
                for (float sh : { 0.0f, 0.6f })
                    for (std::uint32_t seed : { 11u, 2293u, 777u, 4242u })
                    {
                        Config c = base();
                        c.part = Part::Lead;
                        c.stepsPerBar = 16;
                        c.len = 1;
                        c.pulses = 1;
                        c.rotation = 0;
                        c.phraseBars = 4;
                        c.form = Form::AAAB;
                        c.variation = var;
                        c.shuffle = sh;
                        c.slide = 0.0f;
                        c.gate = 0.5f;
                        c.layer = Layer::Off;
                        std::array<StepInput, 16> steps {};
                        for (int i = 0; i < 16; ++i) steps[static_cast<size_t>(i)] = { 69, i == k, i == k, 0.8f };
                        const Motif m = motifFromSteps(steps.data(), 16, c, seed, 0);
                        bool loaded = false;
                        ++runs;
                        const Run r = play(c, 0.0, 8 * B, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double) {
                            if (!loaded) { s.setStoredMotif(m); loaded = true; }
                        });
                        errs += r.polyMain + r.sounding + r.orphanOffs + r.badGlides + r.hangs;
                        // A glide continues the sounding note; a note-off ends
                        // it. Ended more than a slot after its onset, it was
                        // held for a glide that never came.
                        int noteOn = -1;
                        double onQ = 0.0;
                        std::array<bool, 8> barPlays {};
                        for (const auto& p : r.events)
                        {
                            if (p.ev.strandId != -1) continue;
                            if (p.ev.type == VoiceEvent::Type::NoteOn)
                            {
                                const bool fill = std::abs(p.ev.velocity - fillVel) < 1.0e-4f;
                                fillNotes += fill;
                                const int b = static_cast<int>(std::floor(p.q / B + 1.0e-9));
                                if (b >= 0 && b < 8) barPlays[static_cast<size_t>(b)] = true;
                                if (p.ev.artic == VoiceEvent::Articulation::Glide)
                                {
                                    if (fill && fillGlides++ == 0)
                                        whyFill = "#1 k " + std::to_string(k) + " var " + std::to_string(var).substr(0, 3)
                                              + " seed " + std::to_string(seed) + ": glide into a fill note at q "
                                              + std::to_string(p.q);
                                    stepGlides += !fill;
                                }
                                noteOn = p.ev.note;
                                onQ = p.q;
                            }
                            else if (p.ev.note == noteOn)
                            {
                                if (p.q - onQ > 0.25 + 1.0e-6 && heldIntoNothing++ == 0)
                                    whyHeld = "#1 k " + std::to_string(k) + " var " + std::to_string(var).substr(0, 3)
                                          + " seed " + std::to_string(seed) + ": note from q " + std::to_string(onQ)
                                          + " ends at q " + std::to_string(p.q);
                                noteOn = -1;
                            }
                        }
                        silentBars += !barPlays[3] + !barPlays[7];
                    }
        check(fillNotes > 0 && silentBars > 0 && stepGlides > 0,
              "slide into the next bar: the runs meet Fill (" + std::to_string(fillNotes) + " notes), Drop ("
                  + std::to_string(silentBars) + " silent bars) and gliding steps (" + std::to_string(stepGlides) + ")");
        check(fillGlides == 0, "slide into the next bar: never into a Fill note (" + std::to_string(fillGlides)
                                   + " glides over " + std::to_string(runs) + " runs) " + whyFill);
        check(heldIntoNothing == 0, "slide into the next bar: no note held into a bar that does not take it ("
                                        + std::to_string(heldIntoNothing) + ") " + whyHeld);
        check(errs == 0, "slide into the next bar: no doubled, hanging or orphan note, no bad glide ("
                             + std::to_string(errs) + ")");
    }

    std::printf("\n== 17. A step longer than its bar ends with the bar, not under the bars after it\n");
    {
        // One or two steps per bar in a meter shorter than a step: the step
        // is cut at the bar end. The bar after it may be silent — Drop (the
        // B bar of AAAB at Variation 1; below 4 steps per bar there is no
        // Fill, so the variation is a Drop) or an empty bar of a Len 2 motif
        // — and the note before must still end before the bar beyond starts.
        int poly = 0, errs = 0, silent = 0, runs = 0, lenErr = 0;
        std::string why;
        auto tally = [&](const Run& r, double bl, int bars, const std::string& tag) {
            ++runs;
            poly += r.polyMain;
            errs += r.sounding + r.orphanOffs + r.badGlides + r.hangs;
            std::vector<bool> plays(static_cast<size_t>(bars), false);
            for (const auto& p : r.events)
                if (p.ev.strandId == -1 && p.ev.type == VoiceEvent::Type::NoteOn)
                {
                    const int b = static_cast<int>(std::floor(p.q / bl + 1.0e-9));
                    if (b >= 0 && b < bars) plays[static_cast<size_t>(b)] = true;
                }
            for (bool b : plays) silent += !b;
            if (r.polyMain && why.empty())
            {
                why = tag + ":";
                for (const auto& p : r.events)
                    if (p.ev.strandId == -1 && p.q < 6 * bl)
                        why += std::string(" ") + (p.ev.type == VoiceEvent::Type::NoteOn ? "on" : "off")
                             + std::to_string(p.ev.note) + "@" + std::to_string(p.q).substr(0, 5);
            }
        };
        const std::pair<int, int> metres[] = { { 1, 4 }, { 1, 8 }, { 2, 8 }, { 3, 16 }, { 5, 16 }, { 3, 8 } };
        for (Part part : { Part::Bass, Part::Lead })
            for (int spb : { 1, 2 })
                for (const auto& ts : metres)
                    for (float gate : { 0.8f, 1.0f })
                    {
                        const double bl = 4.0 * ts.first / ts.second;
                        const std::string tag = "part " + std::to_string(static_cast<int>(part)) + " spb "
                                              + std::to_string(spb) + " " + std::to_string(ts.first) + "/"
                                              + std::to_string(ts.second) + " gate " + std::to_string(gate).substr(0, 3);
                        Config c = base();
                        c.part = part;
                        c.stepsPerBar = spb;
                        c.rotation = 0;
                        c.slide = 0.0f;
                        c.gate = gate;
                        c.layer = Layer::Off;
                        c.harm = Harm::Chords;
                        c.chordDegrees = { 1, 4, 5, 6 };
                        c.phraseBars = 4;
                        // Drop in the B bar.
                        for (std::uint32_t seed : { 1u, 7u, 99u })
                        {
                            Config d = c;
                            d.len = 1; d.pulses = 1;
                            d.form = Form::AAAB;
                            d.variation = 1.0f;
                            tally(play(d, 0.0, 16 * bl, ts.first, ts.second, {}, seed), bl, 16,
                                  tag + " drop seed " + std::to_string(seed));
                        }
                        // A Len 2 motif whose second bar is empty.
                        Config e = c;
                        e.len = 2; e.pulses = 1;
                        e.form = Form::Loop;
                        e.variation = 0.0f;
                        std::array<StepInput, 4> steps {};
                        for (int i = 0; i < 2 * spb; ++i) steps[static_cast<size_t>(i)] = { 57, i == 0, false, 0.8f };
                        const Motif m = motifFromSteps(steps.data(), 2 * spb, e, 5, 0);
                        bool loaded = false;
                        const Run r = play(e, 0.0, 12 * bl, ts.first, ts.second, {}, 0, [&](T5ynthPhraseSequencer& s, double) {
                            if (!loaded) { s.setStoredMotif(m); loaded = true; }
                        });
                        tally(r, bl, 12, tag + " len 2");
                        // The first step: gate × what the bar leaves of it.
                        double onQ = -1.0;
                        for (const auto& p : r.events)
                        {
                            if (p.ev.strandId != -1) continue;
                            if (p.ev.type == VoiceEvent::Type::NoteOn) { onQ = p.q; continue; }
                            if (onQ < 0.0) continue;
                            const double want = gate * std::min(4.0 / spb, bl);
                            if (std::abs((p.q - onQ) - want) > 2.0e-3 && lenErr++ == 0)
                                why += " | " + tag + " len 2: note " + std::to_string(p.q - onQ).substr(0, 5)
                                     + " q, want " + std::to_string(want).substr(0, 5);
                            break;
                        }
                    }
        check(silent > 0, "step longer than its bar: the runs meet silent bars (" + std::to_string(silent) + ")");
        check(poly == 0, "step longer than its bar: main line monophonic (" + std::to_string(poly)
                             + " overlaps over " + std::to_string(runs) + " runs) " + why);
        check(lenErr == 0, "step longer than its bar: it sounds gate x the bar (" + std::to_string(lenErr)
                               + " off) " + why);
        check(errs == 0, "step longer than its bar: no hanging or orphan note, no bad glide ("
                             + std::to_string(errs) + ")");
    }

    std::printf("\n== 18. A change in play ends the replaced render's notes at the new render's next onset\n");
    {
        // One step per bar at Gate 1: the bar-start note lasts the bar. A
        // change in the bar to 16 steps re-renders it with onsets the old
        // render did not have; the note the old render started must end at
        // the first of them, on the main line and on the layer alike (a line
        // is one voice or one chord at a time; Stab's chord is the main line).
        int under = 0, errs = 0, runs = 0, newOns = 0, notLegato = 0;
        std::string why, whyEnd;
        auto soundsUnder = [&](const Run& r, double fromQ, const std::string& tag) {
            struct V { int note, strand; double onQ; };
            std::vector<V> live;
            for (const auto& p : r.events)
            {
                const VoiceEvent& e = p.ev;
                if (e.type == VoiceEvent::Type::NoteOff)
                {
                    for (auto it = live.begin(); it != live.end(); ++it)
                        if (it->note == e.note && it->strand == e.strandId) { live.erase(it); break; }
                    continue;
                }
                if (e.artic == VoiceEvent::Articulation::Glide)
                {
                    for (auto it = live.rbegin(); it != live.rend(); ++it)
                        if (it->strand == e.strandId) { it->note = e.note; it->onQ = p.q; break; }
                    continue;
                }
                if (p.q > fromQ) newOns += e.strandId == -1;
                for (const auto& v : live)
                    if ((v.strand < 0) == (e.strandId < 0) && v.onQ < p.q - 2 * kOneSampleQ)
                    {
                        if (under++ == 0)
                            why = tag + ": note " + std::to_string(v.note) + " strand " + std::to_string(v.strand)
                                + " from q " + std::to_string(v.onQ).substr(0, 6) + " under note "
                                + std::to_string(e.note) + " strand " + std::to_string(e.strandId) + " at q "
                                + std::to_string(p.q).substr(0, 6);
                        break;
                    }
                live.push_back({ e.note, e.strandId, p.q });
            }
        };
        // The change falls in bar 3, a B unit of ABAB, where the Answer layer
        // plays a rhythm of its own.
        for (Part part : { Part::Bass, Part::Riff, Part::Lead, Part::Stab })
            for (Layer layer : { Layer::Off, Layer::Octave, Layer::Chord, Layer::Answer })
                for (int pulses : { 16, 5 })
                    for (double beat : { 1.0, 2.5 })
                    {
                        Config c = base();
                        c.part = part;
                        c.layer = layer;
                        c.stepsPerBar = 1;
                        c.pulses = 1;
                        c.rotation = 0;
                        c.form = Form::ABAB;
                        c.variation = 0.0f;
                        c.slide = 0.0f;
                        c.gate = 1.0f;
                        Config d = c;
                        d.stepsPerBar = 16;
                        d.pulses = pulses;
                        const double barQ = 3 * B, at = barQ + beat;
                        const std::string tag = "part " + std::to_string(static_cast<int>(part)) + " layer "
                                              + std::to_string(static_cast<int>(layer)) + " pulses "
                                              + std::to_string(pulses) + " beat " + std::to_string(beat).substr(0, 3);
                        bool changed = false;
                        const Run r = play(c, 0.0, 5 * B, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                            if (!changed && q >= at) { s.setConfig(d); changed = true; }
                        });
                        ++runs;
                        soundsUnder(r, at, tag);
                        errs += r.sounding + r.orphanOffs + r.badGlides + r.hangs;
                        // Gate 1: the bar-start note (Stab: each voice of the
                        // chord) ends on the main line's next onset — not
                        // later, under it, and not sooner, at the change.
                        double nextOn = -1.0;
                        for (const auto& p : r.events)
                            if (p.ev.strandId == -1 && p.ev.type == VoiceEvent::Type::NoteOn
                                && p.q > barQ + 2 * kOneSampleQ) { nextOn = p.q; break; }
                        for (size_t i = 0; i < r.events.size(); ++i)
                        {
                            const auto& p = r.events[i];
                            if (p.ev.strandId != -1 || p.ev.type != VoiceEvent::Type::NoteOn
                                || std::abs(p.q - barQ) > 2 * kOneSampleQ) continue;
                            double offQ = -1.0;
                            for (size_t j = i + 1; j < r.events.size() && offQ < 0.0; ++j)
                                if (r.events[j].ev.type == VoiceEvent::Type::NoteOff && r.events[j].ev.strandId == -1
                                    && r.events[j].ev.note == p.ev.note) offQ = r.events[j].q;
                            if (nextOn < 0.0 || std::abs(offQ - nextOn) > 2 * kOneSampleQ)
                                if (notLegato++ == 0)
                                    whyEnd = tag + ": note " + std::to_string(p.ev.note) + " ends at q "
                                           + std::to_string(offQ).substr(0, 6) + ", next onset at q "
                                           + std::to_string(nextOn).substr(0, 6);
                        }
                    }
        check(newOns > 0, "change in play: the denser grid plays after the change (" + std::to_string(newOns)
                              + " main-line onsets)");
        check(under == 0, "change in play: no note of the replaced render sounds under the new one ("
                              + std::to_string(under) + " over " + std::to_string(runs) + " runs) " + why);
        check(notLegato == 0, "change in play: the replaced render's note ends on the new render's next onset ("
                                  + std::to_string(notLegato) + " do not) " + whyEnd);
        check(errs == 0, "change in play: no hanging or orphan note, no bad glide (" + std::to_string(errs) + ")");
    }

    std::printf("\n== 19. A note sounding at a change ends where the renderer ends it: at its line's next onset\n");
    {
        // The renderer ends a note at its line's next onset (finishLine): the
        // main line's for the main line, Stab's chord and the Octave and Chord
        // layers, which take its lengths; the Answer's own for the Answer. A
        // note the replaced render started must end at the earlier of its own
        // off (gate x its step) and its line's next onset as played after the
        // change. Two changes: a denser grid in the bar (the Chord layer's
        // hits end at the main line's next onset, not at its next hit), and a
        // change after the bar's last onset under a shuffled off that reaches
        // past the bar end (the next bar's first onset ends it).
        int checked = 0, cut = 0, wrong = 0, errs = 0, runs = 0, beyondOwn = 0;
        std::string why;
        struct Change { Config from, to; double at; std::string tag; };
        std::vector<Change> changes;
        for (Part part : { Part::Bass, Part::Riff, Part::Lead, Part::Stab })
            for (Layer layer : { Layer::Off, Layer::Octave, Layer::Chord, Layer::Answer })
            {
                Config c = base();
                c.part = part;
                c.layer = layer;
                c.form = Form::ABAB;   // bar 3 is a B unit: the Answer plays
                c.variation = 0.0f;
                c.slide = 0.0f;
                c.gate = 1.0f;
                const std::string pl = "part " + std::to_string(static_cast<int>(part)) + " layer "
                                     + std::to_string(static_cast<int>(layer));
                for (int pulses : { 16, 5 })
                    for (double beat : { 0.5, 1.0, 2.5 })
                    {
                        Config a = c;
                        a.stepsPerBar = 1; a.pulses = 1; a.rotation = 0;
                        Config d = a;
                        d.stepsPerBar = 16; d.pulses = pulses;
                        changes.push_back({ a, d, 3 * B + beat, pl + " denser, pulses " + std::to_string(pulses)
                                                                     + " beat " + std::to_string(beat).substr(0, 3) });
                    }
                for (int rotation : { 0, 1 })
                {
                    Config a = c;
                    a.harm = Harm::Chords;
                    a.chordDegrees = { 1, 4, 0, 0 };
                    a.stepsPerBar = 2; a.pulses = 1; a.rotation = rotation;
                    a.shuffle = 0.75f;
                    Config d = a;
                    d.stepsPerBar = 1; d.rotation = 0;
                    changes.push_back({ a, d, 3 * B + 3.75, pl + " shuffled, rotation " + std::to_string(rotation) });
                }
            }
        auto offAfter = [](const Run& r, size_t i) {
            for (size_t j = i + 1; j < r.events.size(); ++j)
                if (r.events[j].ev.type == VoiceEvent::Type::NoteOff && r.events[j].ev.strandId == r.events[i].ev.strandId
                    && r.events[j].ev.note == r.events[i].ev.note) return r.events[j].q;
            return std::numeric_limits<double>::infinity();
        };
        for (const auto& ch : changes)
        {
            // Its line's onsets: the main line plays on strand −1, the Answer
            // on strand 5; every other layer takes the main line's lengths.
            const bool answer = ch.from.layer == Layer::Answer;
            auto lineStrandOf = [&](int strand) { return strand >= 0 && answer ? 5 : -1; };
            bool changed = false;
            const Run r = play(ch.from, 0.0, 6 * B, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                if (!changed && q >= ch.at) { s.setConfig(ch.to); changed = true; }
            });
            const Run ref = play(ch.from, 0.0, 6 * B);
            ++runs;
            errs += r.sounding + r.orphanOffs + r.badGlides + r.hangs;
            for (size_t i = 0; i < r.events.size(); ++i)
            {
                const auto& p = r.events[i];
                if (p.ev.type != VoiceEvent::Type::NoteOn || p.q >= ch.at) continue;
                // The off the replaced render gave it: the same note unchanged.
                double oldOff = -1.0;
                for (size_t k = 0; k < ref.events.size() && oldOff < 0.0; ++k)
                    if (ref.events[k].sample == p.sample && ref.events[k].ev.type == VoiceEvent::Type::NoteOn
                        && ref.events[k].ev.note == p.ev.note && ref.events[k].ev.strandId == p.ev.strandId)
                        oldOff = offAfter(ref, k);
                if (!(oldOff > ch.at)) continue;   // ended before the change: nothing to bound
                const double own = ownOffQ(p.q, ch.from);
                beyondOwn += oldOff > own + 2 * kOneSampleQ;   // the formula does not describe it
                const int lineStrand = lineStrandOf(p.ev.strandId);
                double nextOn = std::numeric_limits<double>::infinity();
                for (size_t k = i + 1; k < r.events.size(); ++k)
                    if (r.events[k].ev.type == VoiceEvent::Type::NoteOn && r.events[k].ev.strandId == lineStrand
                        && r.events[k].q > p.q + 2 * kOneSampleQ)
                    { nextOn = r.events[k].q; break; }
                const double want = std::min(own, nextOn);
                const double got = offAfter(r, i);
                ++checked;
                cut += nextOn < oldOff - 2 * kOneSampleQ;
                if (std::abs(got - want) > 2 * kOneSampleQ && wrong++ == 0)
                    why = ch.tag + ": note " + std::to_string(p.ev.note) + " strand " + std::to_string(p.ev.strandId)
                        + " from q " + std::to_string(p.q).substr(0, 6) + " ends at q " + std::to_string(got).substr(0, 6)
                        + ", its line's next onset q " + std::to_string(nextOn).substr(0, 6) + ", its own off q "
                        + std::to_string(own).substr(0, 6);
            }
        }
        check(checked > 0 && cut > 0, "note at a change: notes sounding at the change, some ended sooner ("
                                          + std::to_string(checked) + " notes, " + std::to_string(cut) + " sooner)");
        check(beyondOwn == 0, "note at a change: no unchanged note sounds past its own off, gate x its step ("
                                  + std::to_string(beyondOwn) + ")");
        check(wrong == 0, "note at a change: it ends at min(its own off, its line's next onset) ("
                              + std::to_string(wrong) + " of " + std::to_string(checked) + " do not) " + why);
        check(errs == 0, "note at a change: no hanging or orphan note, no bad glide (" + std::to_string(errs)
                             + " over " + std::to_string(runs) + " runs)");
    }

    std::printf("\n== 20. A Keys chord taking effect at a beat a hair before the block replays no onset\n");
    {
        // A Keys chord takes effect at the next beat, found with a rounding
        // tolerance, so a block that starts a hair past a beat re-renders the
        // bar at that beat, before the block. The onset on the beat was played
        // by the block before; the re-render must not play it again. Same for
        // a host that reports a block a little before where the last one
        // ended (jitter under the locate threshold): the onsets in between
        // were played. Every run is compared with the same block positions
        // and no chord change: the onsets must be the same. Each run goes on
        // past the next beat after it (8.0 when the block starts beyond the
        // rounding), where the new chord must sound.
        const double dq = kBlock * kBpm / (60.0 * kSr);
        const double beatQ = 7.0;
        struct Case { std::vector<double> starts; size_t changeAt; std::string tag; };
        std::vector<Case> cases;
        for (double past : { 0.0, 5.0e-7, 2.0e-6, 0.1 })
        {
            Case k;
            for (int b = 40; b >= 1; --b) k.starts.push_back(beatQ + past - b * dq);
            k.changeAt = k.starts.size();
            for (int b = 0; b < 64; ++b) k.starts.push_back(beatQ + past + b * dq);
            char tag[64];
            std::snprintf(tag, sizeof tag, "block %g q past the beat", past);
            k.tag = tag;
            cases.push_back(k);
        }
        for (double back : { 0.005, 0.009 })
        {
            // The block before ends 0.004 q past the beat; the host reports the
            // next one `back` earlier, then runs on from there.
            Case k;
            const double end = beatQ + 0.004;
            for (int b = 40; b >= 1; --b) k.starts.push_back(end - b * dq);
            k.changeAt = k.starts.size();
            for (int b = 0; b < 64; ++b) k.starts.push_back(end - back + b * dq);
            char tag[64];
            std::snprintf(tag, sizeof tag, "host reports the block %g q back", back);
            k.tag = tag;
            cases.push_back(k);
        }
        // Main-line onsets (quarters) and their notes.
        auto run = [&](const Case& k, bool change, std::vector<std::pair<double, int>>& ons) {
            Config c = base();
            c.harm = Harm::Keys;
            c.form = Form::Loop;
            c.pulses = 16;
            c.variation = 0.0f;
            c.slide = 0.0f;
            c.layer = Layer::Off;
            T5ynthPhraseSequencer seq;
            seq.prepare(kSr);
            seq.setTimeSignature(4, 4);
            seq.setConfig(c);
            seq.start();
            auto latch = [&](std::initializer_list<int> notes) {
                seq.clearKeys();
                for (int n : notes) seq.keyOn(n);
                for (int n : notes) seq.keyOff(n);
            };
            latch({ 45, 48, 52 });   // A minor
            std::vector<VoiceEvent> out;
            out.reserve(1024);
            int errs = 0;
            VoiceSim sim;
            for (size_t b = 0; b < k.starts.size(); ++b)
            {
                if (change && b == k.changeAt) latch({ 50, 53, 57 });   // D minor
                out.clear();
                seq.processBlock(kBlock, kBpm, true, k.starts[b], out);
                std::stable_sort(out.begin(), out.end(), [](const VoiceEvent& x, const VoiceEvent& y) {
                    if (x.sampleOffset != y.sampleOffset) return x.sampleOffset < y.sampleOffset;
                    return x.type == VoiceEvent::Type::NoteOff && y.type == VoiceEvent::Type::NoteOn;
                });
                for (const auto& e : out)
                {
                    if (e.type == VoiceEvent::Type::NoteOn) sim.on(e); else sim.off(e);
                    if (e.type == VoiceEvent::Type::NoteOn && e.strandId == -1)
                        ons.push_back({ k.starts[b] + e.sampleOffset * kOneSampleQ, e.note });
                }
            }
            out.clear();
            seq.allNotesOff(out, 0);
            for (const auto& e : out) sim.off(e);
            errs += static_cast<int>(sim.live.size()) + sim.orphanOffs + sim.badGlides;
            return errs;
        };
        int differ = 0, unchanged = 0, errs = 0;
        std::string why;
        for (const auto& k : cases)
        {
            std::vector<std::pair<double, int>> got, ref;
            errs += run(k, true, got) + run(k, false, ref);
            bool same = got.size() == ref.size();
            for (size_t i = 0; same && i < got.size(); ++i)
                same = std::abs(got[i].first - ref[i].first) <= 2 * kOneSampleQ;
            if (!same)
                why += std::string(differ++ ? "; " : "") + k.tag + ": " + std::to_string(got.size()) + " onsets, "
                     + std::to_string(ref.size()) + " without the chord change";
            // The chord did take effect: a note after the beat differs.
            bool took = false;
            for (size_t i = 0; same && i < got.size(); ++i)
                took = took || (got[i].first > beatQ + 2 * kOneSampleQ && got[i].second != ref[i].second);
            unchanged += same && !took;
        }
        check(differ == 0, "Keys beat a hair before the block: the onsets of the run without the change ("
                               + std::to_string(differ) + " of " + std::to_string(cases.size()) + " differ) " + why);
        check(unchanged == 0, "Keys beat a hair before the block: the new chord sounds after the beat ("
                                  + std::to_string(unchanged) + " runs without it)");
        check(errs == 0, "Keys beat a hair before the block: no hanging or orphan note, no bad glide ("
                             + std::to_string(errs) + ")");
    }

    std::printf("\n== 21. Several changes before a note's bound onset: the render now playing bounds it\n");
    {
        // A change bounds a sounding note by its line's next onset (19). A
        // second change before that onset may take the onset away; the note
        // must then end where the render now playing ends it, not at the onset
        // that never played. Two changes in a bar (a denser grid and back, or
        // on to a third grid) and a knob dragged across grid sizes, one value
        // per block. Oracle, as 19 for every note: it ends at the earlier of
        // its own off (gate x its step in the config it started under, where
        // a run of that config plays it: same note on the same sample) and
        // its line's next onset as played.
        struct Schedule { Config from; std::vector<std::pair<double, Config>> changes; std::string tag; };
        std::vector<Schedule> schedules;
        static const int sweep[] = { 1, 2, 3, 4, 6, 8, 12, 16, 12, 8, 6, 4, 3, 2, 1 };
        for (Part part : { Part::Bass, Part::Riff, Part::Lead, Part::Stab })
            for (Layer layer : { Layer::Off, Layer::Octave, Layer::Chord, Layer::Answer })
                for (float gate : { 1.0f, 0.5f })
                {
                    Config a = base();
                    a.part = part;
                    a.layer = layer;
                    a.form = Form::ABAB;   // bar 3 is a B unit: the Answer plays
                    a.variation = 0.0f;
                    a.slide = 0.0f;
                    a.gate = gate;
                    a.stepsPerBar = 1; a.pulses = 1; a.rotation = 0;
                    const std::string pl = "part " + std::to_string(static_cast<int>(part)) + " layer "
                                         + std::to_string(static_cast<int>(layer)) + " gate "
                                         + std::to_string(gate).substr(0, 3);
                    for (int pulses : { 16, 5 })
                        for (int back : { 1, 2 })
                        {
                            // Denser (next onset 0.75), then back to one step
                            // or on to two (next onset 2.0) before 0.75.
                            Config d = a, e = a;
                            d.stepsPerBar = 16; d.pulses = pulses;
                            e.stepsPerBar = back; e.pulses = back;
                            schedules.push_back({ a, { { 3 * B + 0.552, d }, { 3 * B + 0.6507, e } },
                                                  pl + " 1 -> 16/" + std::to_string(pulses) + " -> "
                                                      + std::to_string(back) });
                        }
                    Schedule s { a, {}, pl + " knob drag 1..16..1" };
                    const double dq = kBlock * kBpm / (60.0 * kSr);
                    for (int k = 0; k < static_cast<int>(std::size(sweep)); ++k)
                    {
                        Config d = a;
                        d.stepsPerBar = d.pulses = sweep[k];
                        s.changes.push_back({ 3 * B + 0.30 + k * dq, d });
                    }
                    schedules.push_back(s);
                }
        auto offAfter = [](const Run& r, size_t i) {
            for (size_t j = i + 1; j < r.events.size(); ++j)
                if (r.events[j].ev.type == VoiceEvent::Type::NoteOff && r.events[j].ev.strandId == r.events[i].ev.strandId
                    && r.events[j].ev.note == r.events[i].ev.note) return r.events[j].q;
            return std::numeric_limits<double>::infinity();
        };
        int checked = 0, wrong = 0, unmatched = 0, errs = 0, longer = 0;
        std::string why;
        for (const auto& sc : schedules)
        {
            const bool answer = sc.from.layer == Layer::Answer;
            auto lineStrandOf = [&](int strand) { return strand >= 0 && answer ? 5 : -1; };
            // The configs in turn, each with the position it took effect at.
            std::vector<std::pair<double, Config>> applied { { -1.0, sc.from } };
            size_t next = 0;
            const Run r = play(sc.from, 0.0, 6 * B, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                while (next < sc.changes.size() && q >= sc.changes[next].first)
                {
                    s.setConfig(sc.changes[next].second);
                    applied.push_back({ q, sc.changes[next].second });
                    ++next;
                }
            });
            std::vector<Run> refs;
            for (const auto& ap : applied) refs.push_back(play(ap.second, 0.0, 6 * B));
            errs += r.sounding + r.orphanOffs + r.badGlides + r.hangs;
            const double from = sc.changes.front().first;
            for (size_t i = 0; i < r.events.size(); ++i)
            {
                const auto& p = r.events[i];
                if (p.ev.type != VoiceEvent::Type::NoteOn) continue;
                const double got = offAfter(r, i);
                if (!(got > from)) continue;   // ended before the first change
                size_t k = 0;
                while (k + 1 < applied.size() && applied[k + 1].first <= p.q + 0.5 * kOneSampleQ) ++k;
                const Run& ref = refs[k];
                double refOff = -1.0;
                for (size_t j = 0; j < ref.events.size() && refOff < 0.0; ++j)
                    if (ref.events[j].sample == p.sample && ref.events[j].ev.type == VoiceEvent::Type::NoteOn
                        && ref.events[j].ev.note == p.ev.note && ref.events[j].ev.strandId == p.ev.strandId)
                        refOff = offAfter(ref, j);
                const double ownOff = ownOffQ(p.q, applied[k].second);
                if (refOff < 0.0) { ++unmatched; continue; }                         // not in that run
                if (std::isinf(refOff)) continue;                                    // the run's end
                if (refOff > ownOff + 2 * kOneSampleQ) { ++unmatched; continue; }   // past its own off
                const int lineStrand = lineStrandOf(p.ev.strandId);
                double nextOn = std::numeric_limits<double>::infinity();
                for (size_t j = i + 1; j < r.events.size(); ++j)
                    if (r.events[j].ev.type == VoiceEvent::Type::NoteOn && r.events[j].ev.strandId == lineStrand
                        && r.events[j].q > p.q + 2 * kOneSampleQ)
                    { nextOn = r.events[j].q; break; }
                const double want = std::min(ownOff, nextOn);
                ++checked;
                // A note the second change lets sound past the first change's bound.
                longer += want > 3 * B + 0.75 + 2 * kOneSampleQ && p.q < from;
                if (std::abs(got - want) > 2 * kOneSampleQ && wrong++ == 0)
                    why = sc.tag + ": note " + std::to_string(p.ev.note) + " strand " + std::to_string(p.ev.strandId)
                        + " from q " + std::to_string(p.q).substr(0, 6) + " ends at q " + std::to_string(got).substr(0, 6)
                        + ", its line's next onset q " + std::to_string(nextOn).substr(0, 6) + ", its own off q "
                        + std::to_string(ownOff).substr(0, 6);
            }
        }
        // The plain case: Bass, one step, 16 steps at 0.552, one step again at
        // 0.6507. The bar's note ends at the next bar's onset, 4.0 into it.
        {
            Config a = base();
            a.form = Form::Loop;
            a.variation = 0.0f;
            a.slide = 0.0f;
            a.gate = 1.0f;
            a.layer = Layer::Off;
            a.stepsPerBar = 1; a.pulses = 1; a.rotation = 0;
            Config d = a;
            d.stepsPerBar = 16; d.pulses = 16;
            int step = 0;
            const Run r = play(a, 0.0, 2 * B, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                if (step == 0 && q >= 0.552) { s.setConfig(d); ++step; }
                else if (step == 1 && q >= 0.6507) { s.setConfig(a); ++step; }
            });
            double firstOff = -1.0;
            for (const auto& e : r.events)
                if (e.ev.type == VoiceEvent::Type::NoteOff) { firstOff = e.q; break; }
            check(std::abs(firstOff - B) <= 2 * kOneSampleQ,
                  "several changes: 1 -> 16 -> 1 step before the 16-grid's onset, the bar's note ends at the next bar (q "
                      + std::to_string(firstOff).substr(0, 6) + ")");
            errs += r.sounding + r.orphanOffs + r.badGlides + r.hangs;
        }
        check(checked > 0 && longer > 0, "several changes: notes sounding across the changes, some past the first bound ("
                                             + std::to_string(checked) + " notes, " + std::to_string(longer) + " past it)");
        check(unmatched == 0, "several changes: every note found in a run of the config it started under, within its own off ("
                                  + std::to_string(unmatched) + " not)");
        check(wrong == 0, "several changes: a note ends at min(its own off, its line's next onset as played) ("
                              + std::to_string(wrong) + " of " + std::to_string(checked) + " do not) " + why);
        check(errs == 0, "several changes: no hanging or orphan note, no bad glide (" + std::to_string(errs) + ")");
    }

    std::printf("\n== 22. A host reporting blocks behind where playback reached: a change after it replays no onset\n");
    {
        // A host may report a block a little before where the last one ended
        // (under the locate threshold). With small blocks that is more than a
        // block, so the blocks after it start behind onsets already played
        // too, and a change in one of them (a Keys chord at the next beat, a
        // config change) re-renders the bar there. It must play on from where
        // playback reached, not from that block's start. Each run is compared
        // with the same block positions and no change: the main line's onsets
        // must be the same, and the change must take effect.
        const double end = 0.004;   // the last block before the jitter ends this far past the beat
        int differ = 0, unchanged = 0, errs = 0, runs = 0;
        std::string why;
        for (int block : { 32, 64, 128 })
            for (double beatQ : { 7.0, 8.0 })   // a beat inside the bar, a bar start
                for (double back : { 0.005, 0.009 })
                    for (int delay : { 0, 1, 2, 3 })
                        for (bool keys : { true, false })
                        {
                            const double dq = block * kBpm / (60.0 * kSr);
                            std::vector<double> starts;
                            for (int b = static_cast<int>(std::ceil(1.0 / dq)); b >= 1; --b)
                                starts.push_back(beatQ + end - b * dq);
                            const size_t changeAt = starts.size() + static_cast<size_t>(delay);
                            for (int b = 0; b < static_cast<int>(std::ceil(1.5 / dq)); ++b)
                                starts.push_back(beatQ + end - back + b * dq);
                            // Main-line onsets and offs: (quarters, note).
                            struct Main { std::vector<std::pair<double, int>> ons, offs; };
                            auto run = [&](bool change, Main& m) {
                                Config c = base();
                                c.harm = Harm::Keys;
                                c.form = Form::Loop;
                                c.pulses = 16;
                                c.variation = 0.0f;
                                c.slide = 0.0f;
                                c.layer = Layer::Off;
                                T5ynthPhraseSequencer seq;
                                seq.prepare(kSr);
                                seq.setTimeSignature(4, 4);
                                seq.setConfig(c);
                                seq.start();
                                auto latch = [&](std::initializer_list<int> notes) {
                                    seq.clearKeys();
                                    for (int n : notes) seq.keyOn(n);
                                    for (int n : notes) seq.keyOff(n);
                                };
                                latch({ 45, 48, 52 });   // A minor
                                std::vector<VoiceEvent> out;
                                out.reserve(1024);
                                VoiceSim sim;
                                for (size_t b = 0; b < starts.size(); ++b)
                                {
                                    if (change && b == changeAt)
                                    {
                                        if (keys) latch({ 50, 53, 57 });   // D minor
                                        else { Config d = c; d.gate = 0.4f; seq.setConfig(d); }
                                    }
                                    out.clear();
                                    seq.processBlock(block, kBpm, true, starts[b], out);
                                    std::stable_sort(out.begin(), out.end(), [](const VoiceEvent& x, const VoiceEvent& y) {
                                        if (x.sampleOffset != y.sampleOffset) return x.sampleOffset < y.sampleOffset;
                                        return x.type == VoiceEvent::Type::NoteOff && y.type == VoiceEvent::Type::NoteOn;
                                    });
                                    for (const auto& e : out)
                                    {
                                        if (e.type == VoiceEvent::Type::NoteOn) sim.on(e); else sim.off(e);
                                        if (e.strandId != -1) continue;
                                        const double q = starts[b] + e.sampleOffset * kOneSampleQ;
                                        (e.type == VoiceEvent::Type::NoteOn ? m.ons : m.offs).push_back({ q, e.note });
                                    }
                                }
                                out.clear();
                                seq.allNotesOff(out, 0);
                                for (const auto& e : out) sim.off(e);
                                return static_cast<int>(sim.live.size()) + sim.orphanOffs + sim.badGlides;
                            };
                            Main got, ref;
                            errs += run(true, got) + run(false, ref);
                            ++runs;
                            bool same = got.ons.size() == ref.ons.size();
                            for (size_t i = 0; same && i < got.ons.size(); ++i)
                                same = std::abs(got.ons[i].first - ref.ons[i].first) <= 2 * kOneSampleQ;
                            char tag[128];
                            std::snprintf(tag, sizeof tag, "%d-sample blocks, beat %g, %g q back, change %d blocks on, %s",
                                          block, beatQ, back, delay, keys ? "Keys chord" : "gate");
                            if (!same && differ++ < 3)
                                why += std::string(differ > 1 ? "; " : "") + tag + ": " + std::to_string(got.ons.size())
                                     + " onsets, " + std::to_string(ref.ons.size()) + " without the change";
                            // The change took effect: a note (Keys) or an off (gate) after it differs.
                            const double after = starts[changeAt] + 2 * kOneSampleQ;
                            bool took = false;
                            if (keys)
                                for (size_t i = 0; same && i < got.ons.size(); ++i)
                                    took = took || (got.ons[i].first > after && got.ons[i].second != ref.ons[i].second);
                            else
                                for (size_t i = 0; same && i < got.offs.size() && i < ref.offs.size(); ++i)
                                    took = took || (got.offs[i].first > after
                                                    && std::abs(got.offs[i].first - ref.offs[i].first) > 2 * kOneSampleQ);
                            unchanged += same && !took;
                        }
        check(differ == 0, "host behind where playback reached: the onsets of the run without the change ("
                               + std::to_string(differ) + " of " + std::to_string(runs) + " differ) " + why);
        check(unchanged == 0, "host behind where playback reached: the change takes effect ("
                                  + std::to_string(unchanged) + " runs without it)");
        check(errs == 0, "host behind where playback reached: no hanging or orphan note, no bad glide ("
                             + std::to_string(errs) + ")");
    }

    std::printf("\n== 23. The Answer's last note before an A unit is not cut at an onset the Answer never plays\n");
    {
        // The Answer plays only in B units. Its note ends at its next onset
        // or at its own length (gate x its step); before an A unit it has no
        // next onset, so a note reaching past the bar end sounds its length.
        // Two steps a bar, the second shuffled to 3.5: it reaches past the
        // bar unless the next bar's onset at 4.0 cuts it. Oracle, every Answer
        // note: it ends at the earlier of its next Answer onset as played and
        // its onset + gate x 2 q.
        auto offAfter = [](const Run& r, size_t i) {
            for (size_t j = i + 1; j < r.events.size(); ++j)
                if (r.events[j].ev.type == VoiceEvent::Type::NoteOff && r.events[j].ev.strandId == r.events[i].ev.strandId
                    && r.events[j].ev.note == r.events[i].ev.note) return r.events[j].q;
            return std::numeric_limits<double>::infinity();
        };
        auto answerConfig = [&](Part part, Form form, int bars, float gate) {
            Config c = base();
            c.part = part;
            c.form = form;
            c.phraseBars = bars;
            c.layer = Layer::Answer;
            c.stepsPerBar = 2; c.pulses = 2; c.rotation = 0;
            c.gate = gate;
            c.shuffle = 0.75f;
            c.variation = 0.0f;
            c.slide = 0.0f;
            return c;
        };
        int checked = 0, wrong = 0, intoA = 0, errs = 0;
        std::string why;
        for (Part part : { Part::Bass, Part::Riff, Part::Lead })
            for (Form form : { Form::ABAB, Form::AABA, Form::AAAB })
                for (int bars : { 4, 8 })
                    for (float gate : { 1.0f, 0.6f })
                    {
                        const Config c = answerConfig(part, form, bars, gate);
                        const Run r = play(c, 0.0, 2 * bars * B);
                        errs += r.sounding + r.orphanOffs + r.badGlides + r.hangs;
                        for (size_t i = 0; i < r.events.size(); ++i)
                        {
                            const auto& p = r.events[i];
                            if (p.ev.type != VoiceEvent::Type::NoteOn || p.ev.strandId != 5) continue;
                            const double got = offAfter(r, i);
                            double nextOn = std::numeric_limits<double>::infinity();
                            for (size_t j = i + 1; j < r.events.size(); ++j)
                                if (r.events[j].ev.type == VoiceEvent::Type::NoteOn && r.events[j].ev.strandId == 5
                                    && r.events[j].q > p.q + 2 * kOneSampleQ)
                                { nextOn = r.events[j].q; break; }
                            const double own = p.q + gate * 2.0;
                            if (!std::isfinite(nextOn) && own > 2 * bars * B - B) continue;   // the run's end
                            const double want = std::min(own, nextOn);
                            ++checked;
                            const int bar = barOf(p.q, B);
                            intoA += want > (bar + 1) * B + 2 * kOneSampleQ && !isBUnit(c, bar + 1);
                            if (std::abs(got - want) > 2 * kOneSampleQ && wrong++ == 0)
                                why = "part " + std::to_string(static_cast<int>(part)) + " form "
                                    + std::to_string(static_cast<int>(form)) + " " + std::to_string(bars)
                                    + " bars gate " + std::to_string(gate).substr(0, 3) + ": note "
                                    + std::to_string(p.ev.note) + " from q " + std::to_string(p.q).substr(0, 6)
                                    + " ends at q " + std::to_string(got).substr(0, 6) + ", want q "
                                    + std::to_string(want).substr(0, 6);
                        }
                    }
        // The plain case: Bass, ABAB, 8 bars (B units bars 2-3, 6-7), gate 1.
        // The note at 11.5 meets bar 3's onset at 12.0; the one at 15.5 sounds
        // its two quarters into bar 4, an A unit.
        {
            const Config c = answerConfig(Part::Bass, Form::ABAB, 8, 1.0f);
            const Run r = play(c, 0.0, 6 * B);
            double off115 = -1.0, off155 = -1.0;
            for (size_t i = 0; i < r.events.size(); ++i)
            {
                const auto& p = r.events[i];
                if (p.ev.type != VoiceEvent::Type::NoteOn || p.ev.strandId != 5) continue;
                if (std::abs(p.q - 11.5) <= 2 * kOneSampleQ) off115 = offAfter(r, i);
                if (std::abs(p.q - 15.5) <= 2 * kOneSampleQ) off155 = offAfter(r, i);
            }
            check(std::abs(off115 - 12.0) <= 2 * kOneSampleQ && std::abs(off155 - 17.5) <= 2 * kOneSampleQ,
                  "answer: Bass ABAB, the note at 11.5 ends at the next bar's onset 12.0 (q "
                      + std::to_string(off115).substr(0, 6) + "), the one at 15.5 before an A unit at 17.5 (q "
                      + std::to_string(off155).substr(0, 6) + ")");
            errs += r.sounding + r.orphanOffs + r.badGlides + r.hangs;
        }
        check(checked > 0 && intoA > 0, "answer: notes checked, some reaching into an A unit ("
                                            + std::to_string(checked) + " notes, " + std::to_string(intoA) + " into A)");
        check(wrong == 0, "answer: a note ends at min(its next Answer onset, onset + gate x step) ("
                              + std::to_string(wrong) + " of " + std::to_string(checked) + " do not) " + why);
        check(errs == 0, "answer: no hanging or orphan note, no bad glide (" + std::to_string(errs) + ")");
    }

    std::printf("\n== 24. A change that takes a note's bounding onset away lets the note sound its own length\n");
    {
        // A shuffled step's note reaches past the next step's onset: its
        // render ends it there (finishLine). A change between the two onsets
        // to a sparser grid takes that onset away; the note then ends at its
        // own off (gate x its step) or the render now playing's next onset of
        // its line, not at the onset the old render had. Sixteen steps, the
        // odd ones shuffled 3/4: the note at 12.4375 would end at 12.5; the
        // change lands in the block from 12.4587. Oracle, every note: it ends
        // at min(its own off in the config it started under, its line's next
        // onset as played).
        auto offAfter = [](const Run& r, size_t i) {
            for (size_t j = i + 1; j < r.events.size(); ++j)
                if (r.events[j].ev.type == VoiceEvent::Type::NoteOff && r.events[j].ev.strandId == r.events[i].ev.strandId
                    && r.events[j].ev.note == r.events[i].ev.note) return r.events[j].q;
            return std::numeric_limits<double>::infinity();
        };
        auto shuffledConfig = [&](Part part, Layer layer, float gate) {
            Config c = base();
            c.part = part;
            c.layer = layer;
            c.form = Form::ABAB;   // bar 3 is a B unit: the Answer plays
            c.stepsPerBar = 16; c.pulses = 16; c.rotation = 0;
            c.gate = gate;
            c.shuffle = 0.75f;
            c.variation = 0.0f;
            c.slide = 0.0f;
            return c;
        };
        int checked = 0, wrong = 0, longer = 0, errs = 0;
        std::string why;
        for (Part part : { Part::Bass, Part::Riff, Part::Lead, Part::Stab })
            for (Layer layer : { Layer::Off, Layer::Octave, Layer::Chord, Layer::Answer })
                for (float gate : { 1.0f, 0.6f })
                    for (int target : { 0, 1, 2 })
                    {
                        const Config a = shuffledConfig(part, layer, gate);
                        Config d = a;
                        if (target == 0) d.pulses = 1;                          // 16 steps, one onset
                        else if (target == 1) d.stepsPerBar = d.pulses = 1;     // one step
                        else d.stepsPerBar = d.pulses = 2;                      // two, the second at 3.5
                        const double at = 3 * B + 0.45;
                        double appliedAt = -1.0;
                        const Run r = play(a, 0.0, 6 * B, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                            if (appliedAt < 0.0 && q >= at) { s.setConfig(d); appliedAt = q; }
                        });
                        errs += r.sounding + r.orphanOffs + r.badGlides + r.hangs;
                        const bool answer = layer == Layer::Answer;
                        for (size_t i = 0; i < r.events.size(); ++i)
                        {
                            const auto& p = r.events[i];
                            if (p.ev.type != VoiceEvent::Type::NoteOn) continue;
                            const double got = offAfter(r, i);
                            if (!(got > appliedAt) || std::isinf(got)) continue;   // before the change, or the run's end
                            const Config& under = p.q < appliedAt - 0.5 * kOneSampleQ ? a : d;
                            const double own = ownOffQ(p.q, under);
                            const int lineStrand = p.ev.strandId >= 0 && answer ? 5 : -1;
                            double nextOn = std::numeric_limits<double>::infinity();
                            for (size_t j = i + 1; j < r.events.size(); ++j)
                                if (r.events[j].ev.type == VoiceEvent::Type::NoteOn && r.events[j].ev.strandId == lineStrand
                                    && r.events[j].q > p.q + 2 * kOneSampleQ)
                                { nextOn = r.events[j].q; break; }
                            const double want = std::min(own, nextOn);
                            ++checked;
                            longer += p.q < appliedAt && want > 3 * B + 0.5 + 2 * kOneSampleQ;
                            if (std::abs(got - want) > 2 * kOneSampleQ && wrong++ == 0)
                                why = "part " + std::to_string(static_cast<int>(part)) + " layer "
                                    + std::to_string(static_cast<int>(layer)) + " gate " + std::to_string(gate).substr(0, 3)
                                    + " target " + std::to_string(target) + ": note " + std::to_string(p.ev.note)
                                    + " strand " + std::to_string(p.ev.strandId) + " from q " + std::to_string(p.q).substr(0, 7)
                                    + " ends at q " + std::to_string(got).substr(0, 7) + ", want q "
                                    + std::to_string(want).substr(0, 7);
                        }
                    }
        // The plain case: Bass, Loop, 16 steps all sounding, gate 1; one
        // onset from the block at 0.4693. The note at 0.4375 sounds its
        // quarter-step to 0.6875, not to 0.5 where step 2 no longer plays.
        {
            Config a = shuffledConfig(Part::Bass, Layer::Off, 1.0f);
            a.form = Form::Loop;
            Config d = a;
            d.pulses = 1;
            bool changed = false;
            const Run r = play(a, 0.0, 2 * B, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                if (!changed && q >= 0.46) { s.setConfig(d); changed = true; }
            });
            double off = -1.0;
            for (size_t i = 0; i < r.events.size(); ++i)
                if (r.events[i].ev.type == VoiceEvent::Type::NoteOn && std::abs(r.events[i].q - 0.4375) <= 2 * kOneSampleQ)
                    off = offAfter(r, i);
            check(std::abs(off - 0.6875) <= 2 * kOneSampleQ,
                  "own length: Bass 16/16 shuffled, one onset from 0.4693, the note at 0.4375 ends at 0.6875 (q "
                      + std::to_string(off).substr(0, 6) + ")");
            errs += r.sounding + r.orphanOffs + r.badGlides + r.hangs;
        }
        check(checked > 0 && longer > 0, "own length: notes sounding across the change, some past the old bound ("
                                             + std::to_string(checked) + " notes, " + std::to_string(longer) + " past it)");
        check(wrong == 0, "own length: a note ends at min(its own off, its line's next onset as played) ("
                              + std::to_string(wrong) + " of " + std::to_string(checked) + " do not) " + why);
        check(errs == 0, "own length: no hanging or orphan note, no bad glide (" + std::to_string(errs) + ")");
    }

    std::printf("\n== 25. A Keys chord takes effect at the beat after where playback reached, wherever the host reports\n");
    {
        // A Keys chord latched between two blocks takes effect at the next
        // beat after where playback has reached. A host may report the block
        // after the latch a little off that point (under the locate
        // threshold), ahead or behind; the beat must not move with it. Each
        // jittered run is compared with the same run reported exactly where
        // playback reached: the same notes in the same order, each onset
        // shifted by no more than the jitter. And against the jittered run
        // with no chord change: the first note the chord changes lies in the
        // beat after where playback reached.
        int differ = 0, early = 0, errs = 0, runs = 0;
        std::string why;
        struct Meter { int num, den; std::vector<double> beats; };   // a beat inside the bar, a bar start
        for (const Meter& m : { Meter { 4, 4, { 7.0, 8.0 } }, Meter { 6, 8, { 5.5, 6.0 } } })
            for (int block : { 32, 64, 128, 512 })
                for (double beat : m.beats)
                    for (double side : { -0.004, 0.004 })
                        for (double jit : { -0.009, -0.005, 0.005, 0.009 })
                        {
                            const double reached = beat + side;
                            const double beatQ = 4.0 / m.den;
                            const double barLenQ = 4.0 * m.num / m.den;
                            const double next = std::ceil(reached / beatQ) * beatQ;
                            const double dq = block * kBpm / (60.0 * kSr);
                            // The runs end apart by the jitter: compare up to a point between
                            // two steps (0.25 q), a bar and more after where playback reached.
                            const double until = std::floor(reached / 0.25) * 0.25 + barLenQ + 0.625;
                            // Main-line onsets from the block the latch precedes: (quarters, note).
                            auto run = [&](double j, bool change, std::vector<std::pair<double, int>>& ons) {
                                Config c = base();
                                c.harm = Harm::Keys;
                                c.form = Form::Loop;
                                c.pulses = 16;
                                c.variation = 0.0f;
                                c.slide = 0.0f;
                                c.layer = Layer::Off;
                                T5ynthPhraseSequencer seq;
                                seq.prepare(kSr);
                                seq.setTimeSignature(m.num, m.den);
                                seq.setConfig(c);
                                seq.start();
                                auto latch = [&](std::initializer_list<int> notes) {
                                    seq.clearKeys();
                                    for (int n : notes) seq.keyOn(n);
                                    for (int n : notes) seq.keyOff(n);
                                };
                                latch({ 45, 48, 52 });   // A minor
                                std::vector<double> starts;
                                for (int b = static_cast<int>(std::ceil(1.0 / dq)); b >= 1; --b)
                                    starts.push_back(reached - b * dq);
                                const size_t at = starts.size();
                                for (int b = 0; b < static_cast<int>(std::ceil((barLenQ + 1.0) / dq)); ++b)
                                    starts.push_back(reached + j + b * dq);
                                std::vector<VoiceEvent> out;
                                out.reserve(1024);
                                VoiceSim sim;
                                for (size_t b = 0; b < starts.size(); ++b)
                                {
                                    if (change && b == at) latch({ 50, 53, 57 });   // D minor
                                    out.clear();
                                    seq.processBlock(block, kBpm, true, starts[b], out);
                                    std::stable_sort(out.begin(), out.end(), [](const VoiceEvent& x, const VoiceEvent& y) {
                                        if (x.sampleOffset != y.sampleOffset) return x.sampleOffset < y.sampleOffset;
                                        return x.type == VoiceEvent::Type::NoteOff && y.type == VoiceEvent::Type::NoteOn;
                                    });
                                    for (const auto& e : out)
                                    {
                                        if (e.type == VoiceEvent::Type::NoteOn) sim.on(e); else sim.off(e);
                                        const double q = starts[b] + e.sampleOffset * kOneSampleQ;
                                        if (e.type == VoiceEvent::Type::NoteOn && e.strandId == -1 && b >= at && q < until)
                                            ons.push_back({ q, e.note });
                                    }
                                }
                                out.clear();
                                seq.allNotesOff(out, 0);
                                for (const auto& e : out) sim.off(e);
                                return static_cast<int>(sim.live.size()) + sim.orphanOffs + sim.badGlides;
                            };
                            std::vector<std::pair<double, int>> got, exact, still;
                            errs += run(jit, true, got) + run(0.0, true, exact) + run(jit, false, still);
                            ++runs;
                            const double tol = std::abs(jit) + 2 * kOneSampleQ;
                            bool same = got.size() == exact.size();
                            for (size_t i = 0; same && i < got.size(); ++i)
                                same = got[i].second == exact[i].second && std::abs(got[i].first - exact[i].first) <= tol;
                            char tag[128];
                            std::snprintf(tag, sizeof tag, "%d/%d, %d-sample blocks, reached %g, reported %+g q",
                                          m.num, m.den, block, reached, jit);
                            if (!same && differ++ < 3)
                                why += std::string(differ > 1 ? "; " : "") + tag;
                            // The first note the chord changes, against the run without the change.
                            double first = -1.0;
                            for (size_t i = 0; first < 0.0 && i < got.size() && i < still.size(); ++i)
                                if (got[i].second != still[i].second) first = got[i].first;
                            const bool inBeat = first >= next - tol && first < next + beatQ;
                            if (!inBeat && early++ < 3 && differ == 0)
                                why += std::string(why.empty() ? "" : "; ") + tag + ": first changed note at q "
                                     + std::to_string(first).substr(0, 6) + ", beat " + std::to_string(next).substr(0, 4);
                        }
        check(differ == 0, "Keys beat under host jitter: the notes of the run reported exactly ("
                               + std::to_string(differ) + " of " + std::to_string(runs) + " differ) " + why);
        check(early == 0, "Keys beat under host jitter: the chord changes in the beat after where playback reached ("
                              + std::to_string(early) + " of " + std::to_string(runs) + " elsewhere)");
        check(errs == 0, "Keys beat under host jitter: no hanging or orphan note, no bad glide (" + std::to_string(errs) + ")");
    }

    std::printf("\n== 26. A note with no sample left after a host reports a block ahead is not started\n");
    {
        // A host may report a block a little ahead of where playback reached
        // (under the locate threshold). Onsets in the time it skipped start
        // late, on the block's first sample. A note whose off falls in that
        // sample too (a short ratchet note, a low gate) has nothing left to
        // sound: started, its on and off would share a tick, the processor
        // would put the off first, and the note would hang. Random configs
        // with short notes (ratchets at a low gate). Each runs reported
        // exactly first; then with forward spikes on random blocks, and on
        // the block of every note short enough to fit in one, sized to skip
        // it whole. No on shares a tick with its own off, every note outside
        // the skipped time plays (within the spike), no note plays that the
        // exact run has not, and nothing hangs. One case is the fuzz find:
        // Riff, Keys, 32 steps, 6 pulses, AABA, Variation 1, shuffle 0.6,
        // Octave layer, gate 0.2, 5/16, 100-sample blocks.
        struct Note { double on, end; int note, strand; };
        struct Played { std::vector<Note> notes; int tickPairs = 0, errs = 0; };
        std::mt19937 rng(26);
        auto I = [&](int a, int b) { return std::uniform_int_distribution<int>(a, b)(rng); };
        auto pick = [&](std::initializer_list<double> v) { return *(v.begin() + I(0, static_cast<int>(v.size()) - 1)); };
        int runs = 0, missing = 0, extra = 0, pairs = 0, errs = 0, skippedNotes = 0;
        std::string why;
        for (int r = 0; r < 240; ++r)
        {
            Config c = base();
            int num = 5, den = 16, block = 100;
            double bpm = 120.0, spike = 0.009;
            c.part = Part::Riff; c.harm = Harm::Keys; c.stepsPerBar = 32; c.pulses = 6; c.form = Form::AABA;
            c.variation = 1.0f; c.slide = 0.0f; c.shuffle = 0.6f; c.layer = Layer::Octave; c.gate = 0.2f;
            if (r > 0)
            {
                c.part = static_cast<Part>(I(0, 3));
                c.harm = static_cast<Harm>(I(0, 2));
                c.stepsPerBar = I(0, 1) ? 32 : 16;
                c.pulses = I(1, c.stepsPerBar);
                c.form = static_cast<Form>(I(0, 3));
                c.variation = static_cast<float>(pick({ 0.5, 1.0 }));
                c.slide = static_cast<float>(pick({ 0.0, 0.5 }));
                c.shuffle = static_cast<float>(pick({ 0.0, 0.6 }));
                c.layer = static_cast<Layer>(I(0, 3));
                c.gate = static_cast<float>(pick({ 0.1, 0.2, 0.3 }));
                static const int meters[4][2] = { { 4, 4 }, { 3, 4 }, { 5, 16 }, { 7, 8 } };
                const int m = I(0, 3);
                num = meters[m][0]; den = meters[m][1];
                block = static_cast<int>(pick({ 32, 64, 100, 256 }));
                bpm = pick({ 90.0, 120.0, 174.0 });
                spike = pick({ 0.005, 0.009 });
            }
            const double qps = bpm / (60.0 * kSr);   // quarters per sample
            const double dq = block * qps;
            const int blocks = static_cast<int>(std::ceil(8.0 * (4.0 * num / den) / dq));
            std::vector<double> ahead(static_cast<size_t>(blocks), 0.0);
            auto run = [&](bool jitter) {
                T5ynthPhraseSequencer seq;
                seq.prepare(kSr);
                seq.setTimeSignature(num, den);
                seq.setConfig(c);
                seq.clearKeys();
                for (int n : { 45, 48, 52 }) seq.keyOn(n);
                for (int n : { 45, 48, 52 }) seq.keyOff(n);
                seq.start();
                Played pl;
                // Live voices: (note, strand, index into pl.notes); a glide hands the voice on.
                struct Live { int note, strand; size_t idx; };
                std::vector<Live> live;
                int orphan = 0, badGlide = 0;
                std::vector<VoiceEvent> out;
                out.reserve(1024);
                for (int b = 0; b < blocks; ++b)
                {
                    const double q0 = b * dq + (jitter ? ahead[static_cast<size_t>(b)] : 0.0);
                    out.clear();
                    seq.processBlock(block, bpm, true, q0, out);
                    // The sequencer's own order: no on is followed by its own off on one tick.
                    for (size_t i = 0; i < out.size(); ++i)
                        if (out[i].type == VoiceEvent::Type::NoteOn)
                            for (size_t j = i + 1; j < out.size(); ++j)
                                if (out[j].type == VoiceEvent::Type::NoteOff && out[j].note == out[i].note
                                    && out[j].strandId == out[i].strandId && out[j].sampleOffset == out[i].sampleOffset)
                                    ++pl.tickPairs;
                    std::stable_sort(out.begin(), out.end(), [](const VoiceEvent& x, const VoiceEvent& y) {
                        if (x.sampleOffset != y.sampleOffset) return x.sampleOffset < y.sampleOffset;
                        return x.type == VoiceEvent::Type::NoteOff && y.type == VoiceEvent::Type::NoteOn;
                    });
                    for (const auto& e : out)
                    {
                        const double q = q0 + e.sampleOffset * qps;
                        if (e.type == VoiceEvent::Type::NoteOn)
                        {
                            pl.notes.push_back({ q, std::numeric_limits<double>::infinity(), e.note, e.strandId });
                            if (e.artic == VoiceEvent::Articulation::Glide)
                            {
                                bool found = false;
                                for (auto it = live.rbegin(); it != live.rend() && !found; ++it)
                                    if (it->strand == e.strandId)
                                    {
                                        pl.notes[it->idx].end = q;
                                        it->note = e.note;
                                        it->idx = pl.notes.size() - 1;
                                        found = true;
                                    }
                                if (found) continue;
                                ++badGlide;
                            }
                            live.push_back({ e.note, e.strandId, pl.notes.size() - 1 });
                        }
                        else
                        {
                            bool found = false;
                            for (auto it = live.begin(); it != live.end() && !found; ++it)
                                if (it->note == e.note && it->strand == e.strandId)
                                {
                                    pl.notes[it->idx].end = q;
                                    live.erase(it);
                                    found = true;
                                }
                            if (!found) ++orphan;
                        }
                    }
                }
                out.clear();
                seq.allNotesOff(out, 0);
                for (const auto& e : out)
                    for (auto it = live.begin(); it != live.end(); ++it)
                        if (it->note == e.note && it->strand == e.strandId) { live.erase(it); break; }
                pl.errs = static_cast<int>(live.size()) + orphan + badGlide;
                return pl;
            };
            const Played exact = run(false);
            // Spikes: random blocks, and the block of each note short enough to
            // skip whole (reported half a sample past its end). None in the
            // last 0.01 q, so a spike reaches no further than the exact run.
            const int last = blocks - static_cast<int>(std::ceil(0.01 / dq)) - 1;
            for (int b = 1; b < last; ++b)
                if (I(0, 4) == 0) ahead[static_cast<size_t>(b)] = spike;
            for (const Note& n : exact.notes)
            {
                const int b = static_cast<int>(std::floor(n.on / dq + 1.0e-9));
                const double past = n.end + 0.5 * qps - b * dq;
                if (b >= 1 && b < last && past < 0.0095) ahead[static_cast<size_t>(b)] = past;
            }
            const Played got = run(true);
            ++runs;
            pairs += got.tickPairs + exact.tickPairs;
            errs += got.errs + exact.errs;
            // The time each spike skipped: from where playback reached to the block's start.
            std::vector<std::pair<double, double>> skipped;
            double reached = 0.0;
            for (int b = 0; b < blocks; ++b)
            {
                const double q0 = b * dq + ahead[static_cast<size_t>(b)];
                if (b > 0 && q0 > reached) skipped.push_back({ reached, q0 });
                reached = std::max(reached, q0 + dq);
            }
            auto inSkipped = [&](const Note& n) {
                for (const auto& sk : skipped)
                    if (n.on >= sk.first - qps && n.on < sk.second && n.end < sk.second + 2 * qps) return true;
                return false;
            };
            const double tol = 0.0095 + 2 * qps;
            std::vector<bool> used(got.notes.size(), false);
            int miss = 0, more = 0;
            for (const Note& n : exact.notes)
            {
                bool hit = false;
                for (size_t i = 0; i < got.notes.size() && !hit; ++i)
                    if (!used[i] && got.notes[i].note == n.note && got.notes[i].strand == n.strand
                        && std::abs(got.notes[i].on - n.on) <= tol)
                        used[i] = hit = true;
                if (inSkipped(n)) { ++skippedNotes; continue; }
                miss += !hit;
            }
            for (bool u : used) more += !u;
            if ((miss || more) && (missing + extra) == 0)
            {
                char tag[160];
                std::snprintf(tag, sizeof tag, " (run %d: part %d harm %d %d/%d, %d-sample blocks, %d missing, %d extra)",
                              r, static_cast<int>(c.part), static_cast<int>(c.harm), num, den, block, miss, more);
                why = tag;
            }
            missing += miss;
            extra += more;
        }
        check(skippedNotes > 0, "host ahead: notes lie wholly in skipped time (" + std::to_string(skippedNotes) + " in "
                                    + std::to_string(runs) + " runs)");
        check(pairs == 0, "host ahead: no note-on shares a tick with its own off (" + std::to_string(pairs) + ")");
        check(missing == 0 && extra == 0, "host ahead: every note outside the skipped time plays, none the exact run has not ("
                                              + std::to_string(missing) + " missing, " + std::to_string(extra) + " extra)" + why);
        check(errs == 0, "host ahead: no hanging or orphan note, no bad glide (" + std::to_string(errs) + ")");
    }

    std::printf("\n== 27. The Answer leads to a head only in a bar where it plays\n");
    {
        // The Answer plays only in B units. A note after its bar's last head
        // resolves toward the next head (§3.2: an approach note targets it);
        // a bar the Answer does not play in has none, so the target is the
        // head where it plays next, or none (renderSlot then resolves against
        // the previous note). The crafted Answer motif has, in each of its two
        // bars, a head on step 0 and an Approach-up on step 2, the bar's last
        // onset, so that note resolves toward a head of a later bar. Changing
        // a head the Answer never plays must leave it as it is.
        auto answerOf = [](Member head0, Member head1) {
            Motif a;
            a.len = 2; a.stepsPerBar = 16; a.pulses = 2; a.rotation = 0; a.part = Part::Riff;
            for (int b = 0; b < 2; ++b)
            {
                Slot& h = a.slots[static_cast<size_t>(b * 16)];
                h.fn = Fn::ChordTone;
                h.member = b == 0 ? head0 : head1;
                a.slots[static_cast<size_t>(b * 16 + 2)].fn = Fn::ApproachUp;
            }
            return a;
        };
        auto cfgOf = [](Form form, int bars, std::array<int, 4> chords) {
            Config c = base();
            c.part = Part::Riff;
            c.harm = Harm::Chords;
            c.chordDegrees = chords;
            c.chordRateBars = 1;
            c.form = form;
            c.phraseBars = bars;
            c.layer = Layer::Answer;
            c.variation = 0.0f; c.slide = 0.0f; c.shuffle = 0.0f;
            return c;
        };
        // The Answer's notes in `bar` as (onset, note).
        auto answerNotes = [](const Config& c, const Motif& answer, int bar) {
            const Motif main = generateMotif(7, 0, c);
            RenderInput in;
            in.cfg = &c; in.motif = &main; in.nextPhraseMotif = &main; in.answer = &answer;
            in.bar = bar; in.barLenQ = 4.0;
            BarRender r;
            renderBar(in, r);
            std::vector<std::pair<double, int>> v;
            for (int i = 0; i < r.count; ++i)
                if (r.events[static_cast<size_t>(i)].line == Line::Answer)
                    v.emplace_back(r.events[static_cast<size_t>(i)].onQ, r.events[static_cast<size_t>(i)].note);
            return v;
        };
        auto show = [](const std::vector<std::pair<double, int>>& v) {
            std::string s;
            for (const auto& [q, n] : v) s += " " + std::to_string(n) + "@" + std::to_string(q).substr(0, 4);
            return s.empty() ? std::string(" none") : s;
        };
        const std::array<int, 4> chords { 1, 6, 4, 5 };

        // AABA, 8 bars: B units are bars 4-5. Bar 4 leads into bar 5, where
        // the Answer plays (the control: its approach follows bar 5's head);
        // bar 5 into bars 6-7, both A.
        {
            const Config c = cfgOf(Form::AABA, 8, chords);
            const auto into5a = answerNotes(c, answerOf(Root, Root), 4);
            const auto into5b = answerNotes(c, answerOf(Root, Fifth), 4);
            check(into5a.size() == 2 && into5a != into5b,
                  "answer: AABA bar 4's approach follows bar 5's head (" + show(into5a) + " /" + show(into5b) + " )");
            const auto into6a = answerNotes(c, answerOf(Root, Root), 5);
            const auto into6b = answerNotes(c, answerOf(Fifth, Root), 5);
            check(into6a.size() == 2 && into6a == into6b,
                  "answer: AABA bar 5's approach follows no head of the A unit after it (" + show(into6a) + " /"
                      + show(into6b) + " )");
        }
        // AAAB, 4 bars: bar 3 is the B unit; the next two bars are A.
        {
            const Config c = cfgOf(Form::AAAB, 4, chords);
            const auto a = answerNotes(c, answerOf(Root, Root), 3);
            const auto b = answerNotes(c, answerOf(Fifth, Root), 3);
            check(a.size() == 2 && a == b,
                  "answer: AAAB bar 3's approach follows no head of the next phrase's A units (" + show(a) + " /"
                      + show(b) + " )");
        }
        // ABAB, 4 bars: B units are bars 1 and 3. Bar 1 leads across bar 2
        // (A) to bar 3's head: bar 2's head leaves it as it is, bar 3's chord
        // changes it.
        {
            const Config c = cfgOf(Form::ABAB, 4, chords);
            const auto a = answerNotes(c, answerOf(Root, Root), 1);
            const auto b = answerNotes(c, answerOf(Fifth, Root), 1);
            check(a.size() == 2 && a == b,
                  "answer: ABAB bar 1's approach follows no head of bar 2, an A unit (" + show(a) + " /" + show(b) + " )");
            const Config c2 = cfgOf(Form::ABAB, 4, { 1, 6, 4, 2 });
            const auto d = answerNotes(c2, answerOf(Root, Root), 1);
            check(a.size() == 2 && a != d,
                  "answer: ABAB bar 1's approach follows bar 3's head, chord V / ii (" + show(a) + " /" + show(d) + " )");
        }
    }

    std::printf("\n== 28. A Keys chord latched after a block that ended a hair past a beat sounds from the next beat\n");
    {
        // A host's block starts land a hair off the beats: it accumulates its
        // position block by block, or derives it from a sample count while the
        // sequencer adds the block length to it. The block before a latch can
        // end a hair past a beat, with that beat's onset played. The chord
        // takes effect at the first beat whose onset has not played: the first
        // note it changes is the first onset from that beat on (a bar longer
        // than 16 sixteenths, 3/2, leaves its last beat empty), and every note
        // before it is the run's without the change. The oracle takes what
        // played from the notes emitted before the latch, not from the
        // sequencer's arithmetic: every step of a bar's 16 plays (16 pulses),
        // so that beat is the first after the last onset emitted.
        Config c = base();
        c.harm = Harm::Keys;
        c.form = Form::Loop;
        c.pulses = 16;
        c.variation = 0.0f;
        c.slide = 0.0f;
        c.layer = Layer::Off;
        int wrong = 0, errs = 0, cases = 0, beatPlayed = 0, beatAhead = 0;
        std::string why;
        struct Meter { int num, den; };
        for (bool accumulated : { true, false })
            for (const Meter& m : { Meter { 4, 4 }, Meter { 6, 8 }, Meter { 3, 2 } })
                for (int block : { 32, 64, 128, 256, 512 })
                {
                    const double beatQ = 4.0 / m.den;
                    const int stepsPerBeat = static_cast<int>(beatQ / 0.25);
                    const double dq = block * kBpm / (60.0 * kSr);
                    const int afterBlocks = static_cast<int>(std::ceil((2.0 * beatQ + 0.5) / dq));
                    std::vector<double> starts;
                    for (long n = 0; starts.empty() || starts.back() < 24.0 + 2.0 * beatQ + 1.0; ++n)
                        starts.push_back(accumulated ? (n == 0 ? 0.0 : starts.back() + dq)
                                                     : static_cast<double>(n * block) * kBpm / (60.0 * kSr));
                    struct On { int block, step, note; };
                    // Main-line onsets of the blocks before `end`, the chord
                    // changing before block `latchAt` (none when negative).
                    auto run = [&](int latchAt, int end, std::vector<On>& ons) {
                        T5ynthPhraseSequencer seq;
                        seq.prepare(kSr);
                        seq.setTimeSignature(m.num, m.den);
                        seq.setConfig(c);
                        seq.start();
                        auto latch = [&](std::initializer_list<int> notes) {
                            seq.clearKeys();
                            for (int n : notes) seq.keyOn(n);
                            for (int n : notes) seq.keyOff(n);
                        };
                        latch({ 45, 48, 52 });   // A minor
                        std::vector<VoiceEvent> out;
                        out.reserve(1024);
                        VoiceSim sim;
                        for (int b = 0; b < end; ++b)
                        {
                            if (b == latchAt) latch({ 50, 53, 57 });   // D minor
                            out.clear();
                            seq.processBlock(block, kBpm, true, starts[static_cast<size_t>(b)], out);
                            std::stable_sort(out.begin(), out.end(), [](const VoiceEvent& x, const VoiceEvent& y) {
                                if (x.sampleOffset != y.sampleOffset) return x.sampleOffset < y.sampleOffset;
                                return x.type == VoiceEvent::Type::NoteOff && y.type == VoiceEvent::Type::NoteOn;
                            });
                            for (const auto& e : out)
                            {
                                if (e.type == VoiceEvent::Type::NoteOn) sim.on(e); else sim.off(e);
                                if (e.type == VoiceEvent::Type::NoteOn && e.strandId == -1)
                                {
                                    const double q = starts[static_cast<size_t>(b)] + e.sampleOffset * kOneSampleQ;
                                    ons.push_back({ b, static_cast<int>(std::lround(q / 0.25)), e.note });
                                }
                            }
                        }
                        out.clear();
                        seq.allNotesOff(out, 0);
                        for (const auto& e : out) sim.off(e);
                        return static_cast<int>(sim.live.size()) + sim.orphanOffs + sim.badGlides;
                    };
                    std::vector<On> still;
                    errs += run(-1, static_cast<int>(starts.size()), still);
                    int taken = 0;
                    for (size_t n = 1; n + static_cast<size_t>(afterBlocks) < starts.size() && taken < 6; ++n)
                    {
                        const double beat = std::round(starts[n] / beatQ) * beatQ;
                        if (starts[n] < 1.0 || std::abs(starts[n] - beat) > 1.0e-6) continue;
                        ++taken;
                        ++cases;
                        const int latchAt = static_cast<int>(n);
                        std::vector<On> got;
                        errs += run(latchAt, latchAt + afterBlocks, got);
                        int lastStep = -1;
                        for (const On& o : got)
                            if (o.block < latchAt) lastStep = std::max(lastStep, o.step);
                        const int beatStep = (lastStep / stepsPerBeat + 1) * stepsPerBeat;
                        int expectStep = -1;
                        for (size_t i = 0; expectStep < 0 && i < still.size(); ++i)
                            if (still[i].step >= beatStep) expectStep = still[i].step;
                        (lastStep >= static_cast<int>(std::lround(beat / 0.25)) ? beatPlayed : beatAhead)++;
                        int firstStep = -1;
                        for (size_t i = 0; firstStep < 0 && i < got.size() && i < still.size(); ++i)
                            if (got[i].step != still[i].step || got[i].note != still[i].note) firstStep = got[i].step;
                        if (firstStep != expectStep && wrong++ < 3)
                        {
                            char tag[200];
                            std::snprintf(tag, sizeof tag, "%s%s host, %d/%d, %d-sample blocks, latch before q %.17g:"
                                          " first changed note at q %g, beat q %g, its first onset q %g",
                                          why.empty() ? "" : "; ", accumulated ? "accumulating" : "sample-count",
                                          m.num, m.den, block, starts[n], firstStep * 0.25, beatStep * 0.25,
                                          expectStep * 0.25);
                            why += tag;
                        }
                    }
                }
        check(beatPlayed > 0 && beatAhead > 0,
              "Keys beat a hair off the block: both kinds occur, the block-start beat played before the latch ("
                  + std::to_string(beatPlayed) + ") and not (" + std::to_string(beatAhead) + ")");
        check(wrong == 0, "Keys beat a hair off the block: the chord's first note is the first unplayed beat's ("
                              + std::to_string(wrong) + " of " + std::to_string(cases) + " elsewhere) " + why);
        check(errs == 0, "Keys beat a hair off the block: no hanging or orphan note, no bad glide (" + std::to_string(errs) + ")");
    }

    std::printf("\n== 29. A layer switch: a note of the old line ends at its strand's next onset\n");
    {
        // The Answer and the Octave and Chord layers all play on strand 5. A
        // shuffled step's note ends past the next grid step, where the other
        // line can start a note on that strand. Switched while such a note
        // sounded, a note of the old line kept its own off (its line has no
        // onset left in the new render) and sounded on under the new line's
        // note on its strand. Oracle: no note-on (other than a glide, which
        // continues the strand's voice) while a note of its strand sounds and
        // did not start on the same sample (a chord's voices start together).
        // Non-vacuity: notes sounding at an Answer -> Octave / Chord switch end
        // earlier than in the same run switched to no layer at the same block,
        // where nothing else plays on their strand and an Answer note keeps its
        // own off. (Against the run without a switch the count is thin: there
        // the Answer's next onset mostly falls on the new line's.) The reverse
        // switches keep the main line, which bounds a note before any Answer
        // onset; they are checked for overlap only.
        std::mt19937 rng(29);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        auto F = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
        struct Pair { Layer from, to; const char* name; };
        const Pair pairs[] = { { Layer::Answer, Layer::Octave, "Answer -> Octave" },
                               { Layer::Answer, Layer::Chord, "Answer -> Chord" },
                               { Layer::Octave, Layer::Answer, "Octave -> Answer" },
                               { Layer::Chord, Layer::Answer, "Chord -> Answer" } };
        for (const Pair& pr : pairs)
        {
            int overlaps = 0, cut = 0, runs = 0;
            std::string why;
            for (int t = 0; t < 1000; ++t)
            {
                Config a = base();
                a.part = static_cast<Part>(U(0, 2));
                const int grids[] = { 4, 8, 16 };
                a.stepsPerBar = grids[U(0, 2)];
                a.pulses = U(1, 12);
                a.rotation = U(0, 7);
                a.form = static_cast<Form>(U(1, 3));
                a.variation = static_cast<float>(F(0.0, 1.0));
                a.slide = U(0, 1) ? static_cast<float>(F(0.0, 1.0)) : 0.0f;
                a.gate = static_cast<float>(F(0.3, 1.0));
                a.shuffle = static_cast<float>(F(0.2, 0.75));
                a.layer = pr.from;
                Config b = a;
                b.layer = pr.to;
                const double at = F(0.0, 32.0);
                Config off = a;
                off.layer = Layer::Off;
                double switchQ = -1.0, offQ = -1.0;
                const Run ref = play(a, 0.0, 40.0, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                    if (offQ < 0.0 && q >= at) { s.setConfig(off); offQ = q; }
                });
                const Run sw = play(a, 0.0, 40.0, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                    if (switchQ < 0.0 && q >= at) { s.setConfig(b); switchQ = q; }
                });
                ++runs;
                std::map<int, std::vector<std::pair<int, int>>> live;   // strand -> (note, onset sample)
                for (size_t i = 0; i < sw.events.size(); ++i)
                {
                    const Played& p = sw.events[i];
                    auto& v = live[p.ev.strandId];
                    if (p.ev.type == VoiceEvent::Type::NoteOff)
                    {
                        for (auto it = v.begin(); it != v.end(); ++it)
                            if (it->first == p.ev.note) { v.erase(it); break; }
                        continue;
                    }
                    if (p.ev.type != VoiceEvent::Type::NoteOn) continue;
                    if (p.ev.artic == VoiceEvent::Articulation::Glide) { v.clear(); v.push_back({ p.ev.note, p.sample }); continue; }
                    bool over = false;
                    for (const auto& n : v) over = over || n.second != p.sample;
                    if (over && overlaps++ < 2)
                    {
                        char tag[200];
                        std::snprintf(tag, sizeof tag, "%strial %d, switch at q %.4f: note %d on strand %d at q %.4f under note %d",
                                      why.empty() ? "" : "; ", t, switchQ, p.ev.note, p.ev.strandId, p.q, v.front().first);
                        why += tag;
                    }
                    v.push_back({ p.ev.note, p.sample });
                }
                // A layer note sounding at the switch: its off in both runs. Not
                // counted: a glide continues it, and the same note starting
                // again on its strand ends it whatever bounds it (a retrigger,
                // with or without the strand's onset bound).
                auto offAfter = [](const Run& r, size_t from, int note, int strand) {
                    for (size_t j = from + 1; j < r.events.size(); ++j)
                    {
                        const VoiceEvent& e = r.events[j].ev;
                        if (e.strandId != strand) continue;
                        if (e.type == VoiceEvent::Type::NoteOn
                            && (e.artic == VoiceEvent::Articulation::Glide || e.note == note)) return -1;
                        if (e.type != VoiceEvent::Type::NoteOff || e.note != note) continue;
                        for (size_t k = j + 1; k < r.events.size() && r.events[k].sample == r.events[j].sample; ++k)
                            if (r.events[k].ev.strandId == strand && r.events[k].ev.type == VoiceEvent::Type::NoteOn
                                && r.events[k].ev.note == note) return -1;
                        return r.events[j].sample;
                    }
                    return -1;
                };
                for (size_t i = 0; i < sw.events.size() && i < ref.events.size(); ++i)
                {
                    const Played& p = sw.events[i];
                    if (p.q >= switchQ) break;
                    if (p.ev.type != VoiceEvent::Type::NoteOn || p.ev.strandId < 5) continue;
                    const int offSw = offAfter(sw, i, p.ev.note, p.ev.strandId);
                    const int offRef = offAfter(ref, i, p.ev.note, p.ev.strandId);
                    if (offSw >= 0 && offRef >= 0 && offSw < offRef && offRef > static_cast<int>(switchQ / kOneSampleQ)) ++cut;
                }
            }
            check(overlaps == 0, std::string("layer switch ") + pr.name + ": no note starts on a strand under a note of it ("
                                     + std::to_string(overlaps) + " in " + std::to_string(runs) + " runs) " + why);
            if (pr.from == Layer::Answer)
                check(cut > 0, std::string("layer switch ") + pr.name + ": notes sounding at the switch end at the new line's onset ("
                                   + std::to_string(cut) + ")");
        }
    }

    std::printf("\n== 30. The lookahead reads a bar of the next phrase from the next phrase's motif\n");
    {
        // An approach note leads into the next head (renderSlot: a scale step
        // below or above it, or a semitone below), and a Fill runs into the
        // next bar's first head; both find it up to two bars on. The second
        // bar was read from the motif of the first one's phrase: from a
        // phrase's second-to-last bar (a 4-bar AABA's B bar for the Fill),
        // past a last bar without a head, the line led into the head the
        // current phrase's motif has there, and with Evolve the next phrase
        // plays another one. The Answer reads the next phrase from its own
        // answer, which Evolve leaves as it is (rhythm and seed); in a 4-bar
        // ABAB it plays bars 1 and 3 and looks from bar 3 into the next
        // phrase's bar 1, checked here as played.
        // Oracle, as played: every approach note whose next head sounds within
        // two bars lies where its function puts it from that head, and every
        // Fill note lies its distance from that head on the scale. Main line:
        // notes, functions and heads from the bar views (Variation 0 and no
        // ABAB cadence, so a head plays as its slot renders), Fill notes from
        // the played events (their velocity; Variation 3/5: Lift and Fill, the
        // head two bars on in an unvaried bar; Lift, but for Bass, lifts the
        // run with the last beat, and the line under it, which a bar after it
        // enters from, is the line without Lift). Answer: notes from the played
        // events, functions and heads from the answer of the phrase's heard
        // motif (Shuffle 0: each onset is its step).
        std::mt19937 rng(30);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        struct Note { int note = -1; Fn fn = Fn::Rest; bool head = false; };
        using Bar = std::array<Note, kMaxBarSteps>;
        constexpr int kBars = 24;
        auto isApproachFn = [](Fn f) { return f == Fn::ApproachUp || f == Fn::ApproachDown || f == Fn::ApproachChromatic; };
        struct Tally { int checked = 0, crossing = 0, bad = 0; std::string why; };
        auto leadIn = [&](const std::vector<Bar>& bars, const Config& c, Tally& t, const char* tag,
                          const std::function<bool(int, int)>& crossing) {
            const std::uint16_t mask = scaleMask(c.scaleType, c.scaleRoot);
            for (int b = 0; b + 2 < static_cast<int>(bars.size()); ++b)
                for (int k = 0; k < kMaxBarSteps; ++k)
                {
                    const Note& a = bars[static_cast<size_t>(b)][static_cast<size_t>(k)];
                    if (a.note < 0 || !isApproachFn(a.fn)) continue;
                    int T = -1, at = -1;
                    for (int ahead = 0; ahead <= 2 && T < 0; ++ahead)
                        for (int j = ahead == 0 ? k + 1 : 0; j < kMaxBarSteps && T < 0; ++j)
                        {
                            const Note& h = bars[static_cast<size_t>(b + ahead)][static_cast<size_t>(j)];
                            if (h.note >= 0 && h.head) { T = h.note; at = b + ahead; }
                        }
                    if (T < 0) continue;
                    const int want = a.fn == Fn::ApproachUp ? scaleStep(T, -1, mask)
                                   : a.fn == Fn::ApproachDown ? scaleStep(T, 1, mask) : T - 1;
                    ++t.checked;
                    t.crossing += crossing(b, at);
                    if (a.note != want && t.bad++ < 2)
                    {
                        char s[160];
                        std::snprintf(s, sizeof s, " | %s: bar %d step %d note %d, head %d in bar %d wants %d",
                                      tag, b, k, a.note, T, at, want);
                        t.why += s;
                    }
                }
        };
        auto randomMotif = [&](Config& c, std::uint32_t seed) {
            if (U(0, 1))
            {
                // From Step, a second bar without a step.
                std::array<StepInput, 2 * kMaxBarSteps> steps {};
                const int spb = c.stepsPerBar;
                for (int i = 0; i < spb; ++i)
                    steps[static_cast<size_t>(i)] = { U(45, 69), U(0, 2) == 0 || i == 0, false, 0.8f };
                const Motif m = motifFromSteps(steps.data(), 2 * spb, c, seed, 0);
                c.len = m.len; c.pulses = m.pulses; c.rotation = 0;
                return m;
            }
            c.len = 2; c.pulses = U(1, 6); c.rotation = U(0, 7);
            return generateMotif(seed, 0, c);
        };
        auto randomConfig = [&]() {
            Config c = base();
            c.part = static_cast<Part>(U(0, 2));
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.chordRateBars = 1;
            c.evolve = static_cast<float>(U(3, 10)) / 10.0f;
            c.variation = 0.0f; c.slide = 0.0f; c.shuffle = 0.0f;
            return c;
        };
        // Plays `m` from q 0 for kBars bars; `views` gets each bar's view.
        auto playMotif = [&](const Config& c, const Motif& m, std::vector<Bar>* views) {
            bool loaded = false;
            return play(c, 0.0, kBars * 4.0, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double) {
                if (!loaded) { s.setStoredMotif(m); loaded = true; return; }
                if (!views) return;
                const int b = s.currentBarForGui.load();
                if (b < 0 || b >= kBars) return;
                std::array<StepView, kMaxBarSteps> v {};
                const int n = s.readView(v);
                Bar& bar = (*views)[static_cast<size_t>(b)];
                bar = Bar {};
                for (int k = 0; k < n; ++k)
                    bar[static_cast<size_t>(k)] = { v[static_cast<size_t>(k)].note, v[static_cast<size_t>(k)].fn,
                                                    v[static_cast<size_t>(k)].head };
            });
        };

        Tally mainT, answerT, fillT;
        int runErrs = 0, unmapped = 0;
        for (int t = 0; t < 300; ++t)
        {
            Config c = randomConfig();
            c.phraseBars = U(0, 1) ? 8 : 4;
            c.form = static_cast<Form>(U(0, 2));   // Loop, AAAB, AABA: no cadence
            c.layer = Layer::Off;
            const Motif m = randomMotif(c, static_cast<std::uint32_t>(1000 + t));
            std::vector<Bar> views(kBars);
            const Run r = playMotif(c, m, &views);
            runErrs += r.hangs + r.orphanOffs + r.badGlides + r.polyMain;
            const int N = phraseBarsOf(c);
            leadIn(views, c, mainT, ("main " + std::to_string(t)).c_str(), [&](int b, int at) {
                return at == b + 2 && floorDiv(b + 1, N) == floorDiv(b, N) && floorDiv(at, N) != floorDiv(b, N);
            });
        }
        for (int t = 0; t < 300; ++t)
        {
            Config c = randomConfig();
            c.phraseBars = 4;
            c.form = Form::AABA;
            c.variation = 0.6f;   // 3 of 5: Re-pitch, Lift, Fill
            c.layer = Layer::Off;
            const Motif m = randomMotif(c, static_cast<std::uint32_t>(9000 + t));
            std::vector<Bar> views(kBars);
            const Run r = playMotif(c, m, &views);
            runErrs += r.hangs + r.orphanOffs + r.badGlides + r.polyMain;
            const int N = phraseBarsOf(c);
            const std::uint16_t mask = scaleMask(c.scaleType, c.scaleRoot);
            const float kFillVel = 72.0f / 127.0f;
            for (int b = 0; b + 2 < kBars; ++b)
            {
                // The Fill's head: the first of bar b + 1, else of bar b + 2.
                // Bar b + 1 can be varied: a head in its last beat may show
                // lifted, so such a case is skipped.
                int T = -1, at = -1, atK = -1;
                for (int ahead = 1; ahead <= 2 && T < 0; ++ahead)
                    for (int k = 0; k < kMaxBarSteps && T < 0; ++k)
                    {
                        const Note& h = views[static_cast<size_t>(b + ahead)][static_cast<size_t>(k)];
                        if (h.note >= 0 && h.head) { T = h.note; at = b + ahead; atK = k; }
                    }
                if (T < 0 || (at == b + 1 && atK * (4.0 / c.stepsPerBar) >= 3.0 - 1.0e-9)) continue;
                // The run starts from the bar's last note before its last
                // beat; without one, from the note the bar entered with: the
                // last of the bar before (Pedal / Chords: entryNotes renders
                // it, and it can itself lead into the next phrase), without
                // the octave Lift put on that bar's last beat. A bar entered
                // from an empty bar is skipped.
                const int lift = c.part != Part::Bass ? 12 : 0;
                auto varied = [&](int bar) {
                    const int j = floorMod(bar, N), unitBars = N / 4;
                    return (isBUnit(c, bar) && j % unitBars == unitBars - 1) || j == N - 1;
                };
                int from = -1, entered = -1;
                std::vector<std::pair<double, int>> fills;
                for (const Played& p : r.events)
                {
                    if (p.ev.strandId != -1 || p.ev.type != VoiceEvent::Type::NoteOn) continue;
                    const double inBar = p.q - 4.0 * b;
                    if (inBar >= -4.0 - 1.0e-3 && inBar < -1.0e-3)
                        entered = p.ev.note - c.transpose - (varied(b - 1) && inBar >= -1.0 - 1.0e-3 ? lift : 0);
                    if (inBar < -1.0e-3 || inBar > 4.0 - 1.0e-3) continue;
                    if (std::abs(p.ev.velocity - kFillVel) < 1.0e-4) fills.emplace_back(inBar, p.ev.note - c.transpose);
                    else if (inBar < 3.0 - 1.0e-3) from = p.ev.note - c.transpose;
                }
                if (from < 0) from = entered;
                if (fills.empty() || from < 0) continue;
                const int dir = T >= from ? -1 : 1;
                for (const auto& [inBar, note] : fills)
                {
                    const int j = static_cast<int>(std::lround((inBar - 3.0) / 0.25));
                    const int want = scaleSteps(T, dir * (4 - j), mask) + lift;
                    ++fillT.checked;
                    fillT.crossing += at == b + 2 && floorDiv(b + 1, N) == floorDiv(b, N) && floorDiv(at, N) != floorDiv(b, N);
                    if (note != want && fillT.bad++ < 2)
                    {
                        char w[160];
                        std::snprintf(w, sizeof w, " | fill %d: bar %d at %.2f note %d, head %d in bar %d wants %d",
                                      t, b, inBar, note, T, at, want);
                        fillT.why += w;
                    }
                }
            }
        }
        rng.seed(3030);
        for (int t = 0; t < 600; ++t)
        {
            Config c = randomConfig();
            c.phraseBars = 4;
            c.form = Form::ABAB;
            c.layer = Layer::Answer;
            const Motif m = randomMotif(c, static_cast<std::uint32_t>(5000 + t));
            const Run r = playMotif(c, m, nullptr);
            runErrs += r.hangs + r.orphanOffs + r.badGlides + r.polyMain;
            const int N = phraseBarsOf(c);
            const Motif fitted = fitMotif(m, c);
            const double stepQ = 4.0 / c.stepsPerBar;
            std::vector<Bar> bars(kBars);
            for (const Played& p : r.events)
            {
                if (p.ev.strandId != 5 || p.ev.type != VoiceEvent::Type::NoteOn) continue;
                // The nearest step: an onset can sound a sample early (q is
                // summed per block here).
                const int g = static_cast<int>(std::lround(p.q / stepQ));
                const int b = floorDiv(g, c.stepsPerBar), k = floorMod(g, c.stepsPerBar);
                if (b >= kBars) continue;
                if (std::abs(p.q - g * stepQ) > 1.0e-3) { ++unmapped; continue; }
                const Motif ans = answerMotif(evolvedMotif(fitted, c, floorDiv(b, N)), c);
                std::array<bool, kMaxMotifSteps> heads {};
                computeHeads(ans, heads);
                const int s = floorMod(b, ans.len) * ans.stepsPerBar + k;
                bars[static_cast<size_t>(b)][static_cast<size_t>(k)] = { p.ev.note - c.transpose, ans.slots[static_cast<size_t>(s)].fn,
                                                                         heads[static_cast<size_t>(s)] };
            }
            leadIn(bars, c, answerT, ("answer " + std::to_string(t)).c_str(), [&](int b, int at) {
                return floorDiv(at, N) != floorDiv(b, N);
            });
        }
        check(runErrs == 0 && unmapped == 0, "lookahead runs: no hanging or orphan note, no bad glide, every Answer onset on its step ("
                                                 + std::to_string(runErrs) + ", " + std::to_string(unmapped) + ")");
        check(mainT.bad == 0, "main line: an approach note leads into the head that plays (" + std::to_string(mainT.bad)
                                  + " of " + std::to_string(mainT.checked) + " off)" + mainT.why);
        check(mainT.crossing > 0, "main line: approach notes led past a headless last bar into the next phrase ("
                                      + std::to_string(mainT.crossing) + ")");
        check(fillT.bad == 0, "Fill: a run leads into the head that plays (" + std::to_string(fillT.bad)
                                  + " of " + std::to_string(fillT.checked) + " off)" + fillT.why);
        check(fillT.crossing > 0, "Fill: runs led past a headless last bar into the next phrase ("
                                      + std::to_string(fillT.crossing) + ")");
        check(answerT.bad == 0, "Answer: an approach note leads into the head that plays (" + std::to_string(answerT.bad)
                                    + " of " + std::to_string(answerT.checked) + " off)" + answerT.why);
        check(answerT.crossing > 0, "Answer: approach notes led into the next phrase (" + std::to_string(answerT.crossing) + ")");
    }

    std::printf("\n== 31. Lift transposes the last beat of the line as it plays without Lift\n");
    {
        // Register lift (§3.7): the last beat goes up an octave. The line was
        // lifted in itself: an embellishment after a lifted note took its P
        // from that note and was lifted again (Repeat, Neighbour, Passing two
        // or three octaves up), and the Fill run, in the same last beat, was
        // not lifted at all. Oracle: the same varied bar at Variation 2/5
        // (Re-pitch, Lift) and 1/5 (Re-pitch, the same draw): every event in
        // the last beat sounds 12 above, every event before it is the same,
        // and the line under it ends on the same notes, so a bar after it
        // enters as it would without Lift. Riff, Lead and Stab (Bass plays an
        // Octave instead, unchanged). The view shows the lifted notes.
        std::mt19937 rng(31);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        int bars = 0, liftedNotes = 0, offInLast = 0, offBefore = 0, offEnd = 0, offView = 0, shapeErrs = 0;
        std::string why;
        for (int t = 0; t < 2000; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(1, 3));   // Riff, Lead, Stab
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.len = U(1, 2); c.pulses = U(2, 12); c.rotation = U(0, 7);
            c.phraseBars = 8;
            c.form = static_cast<Form>(U(0, 3));
            c.shuffle = U(0, 1) ? 0.0f : 0.3f;
            c.layer = Layer::Off;
            const Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(31000 + t), 0, c), c);
            const int bar = 7;   // the last of phrase 0: varied in every form
            // Both renders enter with the same notes.
            const int pn = U(45, 69), ppn = U(45, 69);
            BarRender r[2];
            for (int i = 0; i < 2; ++i)
            {
                Config v = c;
                v.variation = i == 0 ? 0.2f : 0.4f;
                RenderInput in;
                in.cfg = &v; in.motif = &m; in.nextPhraseMotif = &m;
                in.bar = bar; in.barLenQ = 4.0;
                in.prevNote = pn; in.prevPrevNote = ppn;
                renderBar(in, r[i]);
            }
            ++bars;
            if (r[0].count != r[1].count) { ++shapeErrs; continue; }
            for (int e = 0; e < r[0].count; ++e)
            {
                const Event& a = r[0].events[static_cast<size_t>(e)];
                const Event& b = r[1].events[static_cast<size_t>(e)];
                if (std::abs(a.onQ - b.onQ) > 1.0e-9 || a.strand != b.strand) { ++shapeErrs; continue; }
                const bool inLast = a.onQ >= 3.0 - 1.0e-9;
                if (inLast) ++liftedNotes;
                const int want = a.note + (inLast ? 12 : 0);
                if (b.note != want)
                {
                    ++(inLast ? offInLast : offBefore);
                    if (why.size() < 300)
                        why += " | t " + std::to_string(t) + " part " + std::to_string(static_cast<int>(c.part)) + " at "
                             + std::to_string(a.onQ) + ": " + std::to_string(a.note) + " -> " + std::to_string(b.note);
                }
            }
            if (r[0].lastNote != r[1].lastNote || r[0].prevNote != r[1].prevNote) ++offEnd;
            for (int k = 0; k < r[0].viewSteps; ++k)
            {
                const StepView& a = r[0].view[static_cast<size_t>(k)];
                const StepView& b = r[1].view[static_cast<size_t>(k)];
                if (a.note < 0) continue;
                const bool inLast = k * (4.0 / m.stepsPerBar) >= 3.0 - 1.0e-9;
                offView += b.note != a.note + (inLast ? 12 : 0);
            }
        }
        check(shapeErrs == 0, "Lift changes no onset (" + std::to_string(shapeErrs) + " of " + std::to_string(bars) + " bars)");
        check(liftedNotes > 1000, "last-beat events compared (" + std::to_string(liftedNotes) + ")");
        check(offInLast == 0, "every event of the last beat sounds an octave above the bar without Lift ("
                                  + std::to_string(offInLast) + " off)" + why);
        check(offBefore == 0, "every event before the last beat is the bar without Lift (" + std::to_string(offBefore) + " off)");
        check(offEnd == 0, "the line ends on the notes it ends on without Lift (" + std::to_string(offEnd) + " bars off)");
        check(offView == 0, "the view shows the lifted notes (" + std::to_string(offView) + " off)");
    }

    std::printf("\n== 32. The lookahead leads into the head as its bar plays it\n");
    {
        // The lookahead read the next head from the motif slot: in a bar the
        // variation or the form changes, the line led into a note that never
        // sounds. An ABAB cadence puts the root or the fifth on its bar's last
        // head, Drop silences the last beat, and under Lift Bass plays an
        // Octave on the last pulse of the last beat in place of what is there.
        // Oracle, as played, in the line under the octave Lift (§3.7: notes
        // are led in the line as it plays without Lift): bar views give notes,
        // functions and heads; a lifted last beat (not Bass) is read an octave
        // down, and Bass's octave pulse is no head and leads into none. Every
        // approach note whose next sounding head lies within two bars lies
        // where its function puts it from that head; every Fill note (the
        // played events, by velocity) lies its distance from the next bar's
        // first sounding head on the scale. All forms, ABAB weighted;
        // phrases of 4, 8 and 16 bars; Variation 0, 2/5, 3/5 and 1.
        std::mt19937 rng(32);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        struct Note { int note = -1; Fn fn = Fn::Rest; bool head = false, octave = false; };
        using Bar = std::array<Note, kMaxBarSteps>;
        using Line = std::vector<Bar>;
        constexpr int kBars = 40;
        auto isApproachFn = [](Fn f) { return f == Fn::ApproachUp || f == Fn::ApproachDown || f == Fn::ApproachChromatic; };
        const float kFillVel = 72.0f / 127.0f;
        static const float kVariation[] = { 0.0f, 0.4f, 0.6f, 1.0f };
        static const int kPhraseBars[] = { 4, 8, 16 };
        auto draw = [&]() {
            Config c = base();
            c.part = static_cast<Part>(U(0, 2));   // Bass, Riff, Lead
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.chordRateBars = 1;
            c.evolve = static_cast<float>(U(0, 10)) / 10.0f;
            c.form = U(0, 2) == 0 ? static_cast<Form>(U(0, 2)) : Form::ABAB;
            c.phraseBars = kPhraseBars[U(0, 2)];
            c.variation = kVariation[U(0, 3)];
            c.slide = 0.0f; c.shuffle = 0.0f;
            c.layer = Layer::Off;
            c.len = U(1, 2); c.pulses = U(1, 8); c.rotation = U(0, 7);
            return c;
        };

        // Plays c with motif m from bar 0 and reads the bar views into line.
        // turnQ >= 0: Evolve turns to turnTo in the first block from turnQ on,
        // which starts at turnedAt.
        auto playLine = [&](const Config& c, const Motif& m, double turnQ, float turnTo, Line& line,
                            double& turnedAt) {
            line.assign(kBars, Bar {});
            turnedAt = -1.0;
            bool loaded = false;
            return play(c, 0.0, kBars * 4.0, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                if (!loaded) { s.setStoredMotif(m); loaded = true; return; }
                if (turnQ >= 0.0 && turnedAt < 0.0 && q >= turnQ)
                {
                    Config n = c;
                    n.evolve = turnTo;
                    s.setConfig(n);
                    turnedAt = q;
                }
                const int b = s.currentBarForGui.load();
                if (b < 0 || b >= kBars) return;
                std::array<StepView, kMaxBarSteps> v {};
                const int n = s.readView(v);
                Bar& bar = line[static_cast<size_t>(b)];
                bar = Bar {};
                for (int k = 0; k < n; ++k)
                    bar[static_cast<size_t>(k)] = { v[static_cast<size_t>(k)].note, v[static_cast<size_t>(k)].fn,
                                                    v[static_cast<size_t>(k)].head, false };
            });
        };

        struct Tally
        {
            int runErrs = 0, checked = 0, bad = 0, cadenceTargets = 0, dropSkips = 0, octaveSkips = 0;
            int fillChecked = 0, fillBad = 0, fillCadence = 0;
            int crossing = 0, moved = 0, fillCrossing = 0, fillMoved = 0;
            std::string why, fillWhy;
        };
        // Checks the approach notes and the Fill notes of run r that sound
        // from fromQ on. With `without` (the same trial without the turn), it
        // counts those that lead from phrase 1 into phrase 2, and among them
        // those whose head there is another without the turn.
        auto checkRun = [&](int t, const Config& c, Line& line, const Run& r, double fromQ, Line* without,
                            Tally& y) {
            y.runErrs += r.hangs + r.orphanOffs + r.badGlides + r.polyMain;

            const int N = phraseBarsOf(c), unitBars = N / 4;
            const int ops = static_cast<int>(std::lround(c.variation * 5.0f));
            const bool bass = c.part == Part::Bass;
            const double stepQ = 4.0 / c.stepsPerBar;
            const std::uint16_t mask = scaleMask(c.scaleType, c.scaleRoot);
            auto inLast = [&](int k) { return k * stepQ >= 3.0 - 1.0e-9; };
            auto lifted = [&](int bar) {
                const int j = floorMod(bar, N);
                return ops >= 2 && ((isBUnit(c, bar) && j % unitBars == unitBars - 1) || j == N - 1);
            };
            auto cadenceBar = [&](int bar) { return c.form == Form::ABAB && floorMod(bar, N) % unitBars == unitBars - 1; };

            // The line under the octave Lift.
            auto underLift = [&](Line& L) {
                for (int b = 0; b < kBars; ++b)
                {
                    if (!lifted(b)) continue;
                    Bar& bar = L[static_cast<size_t>(b)];
                    int lastK = -1;
                    for (int k = 0; k < kMaxBarSteps; ++k)
                        if (bar[static_cast<size_t>(k)].note >= 0 && inLast(k))
                        {
                            lastK = k;
                            if (!bass) bar[static_cast<size_t>(k)].note -= 12;
                        }
                    if (bass && lastK >= 0)
                    {
                        bar[static_cast<size_t>(lastK)].note = -1;
                        bar[static_cast<size_t>(lastK)].octave = true;
                    }
                }
            };
            underLift(line);
            if (without) underLift(*without);

            // The next sounding head from bar b, step k on (k = -1: from the
            // bar start); `skips` counts the dropped heads and octave pulses
            // passed on the way.
            struct Target { int note = -1, bar = -1, step = -1, drops = 0, octaves = 0; };
            auto nextHead = [&](const Line& L, int b, int k) {
                Target h;
                for (int ahead = 0; ahead <= 2; ++ahead)
                    for (int j = ahead == 0 ? k + 1 : 0; j < kMaxBarSteps; ++j)
                    {
                        const Note& n = L[static_cast<size_t>(b + ahead)][static_cast<size_t>(j)];
                        if (!n.head) continue;
                        if (n.octave) { ++h.octaves; continue; }
                        if (n.note < 0) { h.drops += n.fn != Fn::Rest; continue; }
                        h.note = n.note; h.bar = b + ahead; h.step = j;
                        return h;
                    }
                return h;
            };
            auto isCadence = [&](const Target& h) {
                if (!cadenceBar(h.bar)) return false;
                for (int j = h.step + 1; j < kMaxBarSteps; ++j)
                {
                    const Note& n = line[static_cast<size_t>(h.bar)][static_cast<size_t>(j)];
                    if (n.head && n.note >= 0) return false;
                }
                return true;
            };
            auto crosses = [&](int b, const Target& h) { return floorDiv(b, N) == 1 && floorDiv(h.bar, N) == 2; };

            for (int b = 0; b + 2 < kBars; ++b)
                for (int k = 0; k < kMaxBarSteps; ++k)
                {
                    const Note& a = line[static_cast<size_t>(b)][static_cast<size_t>(k)];
                    if (a.note < 0 || !isApproachFn(a.fn)) continue;
                    if (4.0 * b + k * stepQ < fromQ + 1.0e-6) continue;
                    const Target h = nextHead(line, b, k);
                    if (h.note < 0) continue;
                    const int want = a.fn == Fn::ApproachUp ? scaleStep(h.note, -1, mask)
                                   : a.fn == Fn::ApproachDown ? scaleStep(h.note, 1, mask) : h.note - 1;
                    ++y.checked;
                    y.cadenceTargets += isCadence(h);
                    y.dropSkips += h.drops;
                    y.octaveSkips += h.octaves;
                    if (without && crosses(b, h))
                    {
                        ++y.crossing;
                        y.moved += nextHead(*without, b, k).note != h.note;
                    }
                    if (a.note != want && y.bad++ < 3)
                    {
                        char s[200];
                        std::snprintf(s, sizeof s, " | t %d form %d part %d: bar %d step %d note %d, head %d at %d/%d wants %d",
                                      t, static_cast<int>(c.form), static_cast<int>(c.part), b, k, a.note, h.note, h.bar,
                                      h.step, want);
                        y.why += s;
                    }
                }

            // Fill: from the bar's last note before its last beat, else from
            // the note the bar entered with (the last of the bar before, in
            // the line under the Lift), into the next bar's first sounding
            // head; the run sounds with the Lift (not Bass).
            for (int b = 1; b + 2 < kBars; ++b)
            {
                const Target h = nextHead(line, b + 1, -1);
                if (h.note < 0) continue;
                int from = -1, entered = -1;
                std::vector<std::pair<double, int>> fills;
                for (const Played& p : r.events)
                {
                    if (p.ev.strandId != -1 || p.ev.type != VoiceEvent::Type::NoteOn) continue;
                    const double inBar = p.q - 4.0 * b;
                    const int note = p.ev.note - c.transpose;
                    if (inBar >= -4.0 - 1.0e-3 && inBar < -1.0e-3)
                        entered = note - (lifted(b - 1) && !bass && inBar >= -1.0 - 1.0e-3 ? 12 : 0);
                    if (inBar < -1.0e-3 || inBar > 4.0 - 1.0e-3) continue;
                    if (std::abs(p.ev.velocity - kFillVel) < 1.0e-4)
                    {
                        if (p.q >= fromQ + 1.0e-6) fills.emplace_back(inBar, note);
                    }
                    else if (inBar < 3.0 - 1.0e-3) from = note;
                }
                if (from < 0) from = entered;
                if (fills.empty() || from < 0) continue;
                const int dir = h.note >= from ? -1 : 1;
                const bool cross = without && crosses(b, h);
                const bool moved = cross && nextHead(*without, b + 1, -1).note != h.note;
                for (const auto& [inBar, note] : fills)
                {
                    const int j = static_cast<int>(std::lround((inBar - 3.0) / 0.25));
                    const int want = scaleSteps(h.note, dir * (4 - j), mask) + (bass ? 0 : 12);
                    ++y.fillChecked;
                    y.fillCadence += isCadence(h);
                    y.fillCrossing += cross;
                    y.fillMoved += moved;
                    if (note != want && y.fillBad++ < 3)
                    {
                        char s[200];
                        std::snprintf(s, sizeof s, " | t %d form %d part %d: bar %d at %.2f note %d, head %d at %d/%d wants %d",
                                      t, static_cast<int>(c.form), static_cast<int>(c.part), b, inBar, note, h.note, h.bar,
                                      h.step, want);
                        y.fillWhy += s;
                    }
                }
            }
        };

        Tally one;
        for (int t = 0; t < 2000; ++t)
        {
            const Config c = draw();
            const Motif m = generateMotif(static_cast<std::uint32_t>(32000 + t), 0, c);
            Line line;
            double unturned = -1.0;
            const Run r = playLine(c, m, -1.0, 0.0f, line, unturned);
            checkRun(t, c, line, r, -1.0e9, nullptr, one);
        }
        check(one.runErrs == 0, "lookahead as played: no hanging or orphan note, no bad glide (" + std::to_string(one.runErrs) + ")");
        check(one.cadenceTargets > 0 && one.dropSkips > 0 && one.octaveSkips > 0,
              "lookahead as played: approach notes into cadence heads (" + std::to_string(one.cadenceTargets)
                  + "), past dropped heads (" + std::to_string(one.dropSkips) + ") and past Bass's octave pulse ("
                  + std::to_string(one.octaveSkips) + ")");
        check(one.bad == 0, "lookahead as played: an approach note leads into the head that sounds (" + std::to_string(one.bad)
                                + " of " + std::to_string(one.checked) + " off)" + one.why);
        check(one.fillCadence > 0, "lookahead as played: Fill runs into cadence heads (" + std::to_string(one.fillCadence) + ")");
        check(one.fillBad == 0, "lookahead as played: a Fill runs into the head that sounds (" + std::to_string(one.fillBad)
                                    + " of " + std::to_string(one.fillChecked) + " off)" + one.fillWhy);

        std::printf("\n== 33. The line leads into the next phrase with the Evolve it latches\n");
        // Evolve is latched at phrase entry (§3.8), so turning it in a phrase
        // gives the next phrase another motif; the line led into the motif the
        // next phrase had before the turn. Evolve turns by 0.5 in phrase 1,
        // half the time in its last bar, always inside a bar's first three
        // beats (the bar re-renders, and the view read after the turn shows
        // it). Oracle of 32, for what sounds from the block of the turn on;
        // the same trial unturned counts the heads the turn moves.
        Tally two;
        for (int t = 0; t < 1000; ++t)
        {
            const Config c = draw();
            const Motif m = generateMotif(static_cast<std::uint32_t>(33000 + t), 0, c);
            const int N = phraseBarsOf(c);
            const int turnBar = U(0, 1) ? 2 * N - 1 : U(N, 2 * N - 1);
            const double turnQ = 4.0 * turnBar + 0.25 * U(0, 11);
            const float turnTo = c.evolve >= 0.5f ? 0.0f : 1.0f;
            Line line, without;
            double turnedAt = -1.0, unturned = -1.0;
            const Run r = playLine(c, m, turnQ, turnTo, line, turnedAt);
            const Run r0 = playLine(c, m, -1.0, 0.0f, without, unturned);
            two.runErrs += r0.hangs + r0.orphanOffs + r0.badGlides + r0.polyMain;
            checkRun(t, c, line, r, turnedAt, &without, two);
        }
        check(two.runErrs == 0, "Evolve turned: no hanging or orphan note, no bad glide (" + std::to_string(two.runErrs) + ")");
        check(two.crossing > 0 && two.moved > 0,
              "Evolve turned: approach notes from phrase 1 into phrase 2 (" + std::to_string(two.crossing) + "), "
                  + std::to_string(two.moved) + " of them into a head the turn moved");
        check(two.bad == 0, "Evolve turned: an approach note leads into the head that sounds (" + std::to_string(two.bad)
                                + " of " + std::to_string(two.checked) + " off)" + two.why);
        check(two.fillCrossing > 0 && two.fillMoved > 0,
              "Evolve turned: Fill notes from phrase 1 into phrase 2 (" + std::to_string(two.fillCrossing) + "), "
                  + std::to_string(two.fillMoved) + " of them into a head the turn moved");
        check(two.fillBad == 0, "Evolve turned: a Fill runs into the head that sounds (" + std::to_string(two.fillBad)
                                    + " of " + std::to_string(two.fillChecked) + " off)" + two.fillWhy);
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
