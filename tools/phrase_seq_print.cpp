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
        // there, so a re-render would re-voice the rest of the bar.
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

            int differ = 0, runs = 0;
            std::string firstWhy;
            for (Harm h : { Harm::Pedal, Harm::Chords })
                for (std::uint32_t seed : { 2293u, 777u, 4242u, 31337u })
                {
                    Config hc = cc;
                    hc.harm = h;
                    const Motif at1 = generateMotif(seed, 1, hc);
                    const Act load1 = [at1](T5ynthPhraseSequencer& s) { s.setStoredMotif(at1); };
                    const Run ref = play(hc, 0.0, 12 * B, 4, 4, {}, 0, acts({ { 0.0, load1 } }));
                    for (double off = 0.0; off < B; off += 0.25)
                    {
                        const double kq = 8 * B + off;   // bar 8 = first bar of phrase 2
                        const Run k = play(hc, 0.0, 12 * B, 4, 4, {}, 0,
                                           acts({ { 0.0, load1 }, { kq, keepSeen } }));
                        std::string why;
                        ++runs;
                        if (!sameOns(onsInBars(ref, B, 8, 12, kq), onsInBars(k, B, 8, 12, kq), why))
                            if (differ++ == 0) firstWhy = why;
                    }
                }
            check(differ == 0, "keep in a phrase's first bar: the rest of the bar and the phrase play on unchanged ("
                                   + std::to_string(differ) + " of " + std::to_string(runs) + " differ) " + firstWhy);
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

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
