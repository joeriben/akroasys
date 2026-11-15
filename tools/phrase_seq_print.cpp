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
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <tuple>
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

char fnChar(const StepView& v)
{
    switch (v.tone)
    {
        case Tone::Root:    return 'R';
        case Tone::Third:   return '3';
        case Tone::Fifth:   return '5';
        case Tone::Seventh: return '7';
        case Tone::Second:  return '2';
        case Tone::Chord:   return '#';
        default: break;
    }
    switch (v.fn)
    {
        case Fn::Rest:              return '.';
        case Fn::ChordTone:         return '?';
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

/** The bar's chord as chordForGui carries it. */
Chord chordFromGui(std::uint32_t cw)
{
    Chord c;
    c.rootPc = static_cast<int>(cw & 15u);
    c.iv[1] = static_cast<int>((cw >> 4) & 31u) - 1;
    c.iv[2] = static_cast<int>((cw >> 9) & 31u) - 1;
    c.iv[3] = static_cast<int>((cw >> 14) & 31u) - 1;
    return c;
}

/** Whether a view step's tone is the note it plays (section 38): R 3 5 7
 *  sound the chord's member at the step's octave, 2 the scale step above the
 *  root at it. Empty when it is; a step naming no tone, or the whole chord,
 *  has no note to check. */
std::string toneMismatch(const StepView& v, const Chord& ch, int tonic, std::uint16_t mask)
{
    if (v.tone == Tone::None || v.tone == Tone::Chord) return {};
    const int root = chordRootNote(ch, tonic) + 12 * v.octave;
    int want = -1;
    if (v.tone == Tone::Second)
        want = scaleStep(root, 1, mask);
    else
    {
        const int iv = ch.iv[static_cast<size_t>(static_cast<int>(v.tone) - static_cast<int>(Tone::Root))];
        if (iv < 0) return "names a member the chord lacks";
        want = root + iv;
    }
    if (v.note == want) return {};
    return "plays " + std::to_string(v.note) + ", its tone at its octave is " + std::to_string(want);
}

/** A chord symbol read back by a reader written here (sections 38, 39):
 *  root, third (2 or 4 for sus2 / sus4), fifth and seventh (9 for a 6 or
 *  °7) as intervals, −1 for absent, and the add… intervals. */
struct ChordRead { int root = -1, t = 4, f = 7, sv = -1; std::vector<int> adds; bool ok = true; };

ChordRead readChordName(const std::string& name)
{
    const std::string fl = "\xe2\x99\xad", sh = "\xe2\x99\xaf", dg = "\xc2\xb0", hd = "\xc3\xb8";
    ChordRead r;
    size_t i = 0;
    auto eat = [&](const std::string& tok) {
        if (name.compare(i, tok.size(), tok) != 0) return false;
        i += tok.size();
        return true;
    };
    static const char* kLetters = "C D EF G A B";
    const char* at = name.empty() ? nullptr : std::strchr(kLetters, name[0]);
    if (!at || name[0] == ' ') { r.ok = false; return r; }
    r.root = static_cast<int>(at - kLetters);
    i = 1;
    if (eat("#")) ++r.root;
    if (eat(hd)) { r.t = 3; r.f = 6; r.ok = eat("7"); r.sv = 10; }
    else if (eat(dg)) { r.t = 3; r.f = 6; if (eat("7")) r.sv = 9; }
    else if (eat("+")) { r.t = 4; r.f = 8; }
    else if (name.compare(i, 3, "maj") != 0 && eat("m")) r.t = 3;
    if (r.sv < 0)
    {
        if (eat("7")) r.sv = 10;
        else if (eat("maj7") || eat("(maj7)")) r.sv = 11;
        else if (!(r.t == 3 && r.f == 6) && eat("6")) r.sv = 9;   // a sixth over ° is °7
    }
    if (r.sv < 0 && i + 1 == name.size() && eat("5")) { r.t = -1; return r; }
    if (eat("sus2")) r.t = 2;
    else if (eat("sus4")) r.t = 5;
    if (eat("(" + fl + "5)")) r.f = 6;
    else if (eat("(" + sh + "5)")) r.f = 8;
    const std::vector<std::pair<std::string, int>> ext = {
        { fl + "13", 8 }, { sh + "11", 6 }, { fl + "9", 1 }, { sh + "9", 3 }, { "maj7", 11 },
        { "11", 5 }, { "9", 2 }, { "3", 4 }, { "5", 7 }, { "6", 9 }, { "7", 10 } };
    while (eat("add"))
    {
        bool found = false;
        for (const auto& [tok, iv] : ext)
            if (eat(tok)) { r.adds.push_back(iv); found = true; break; }
        if (!found) { r.ok = false; return r; }
    }
    if (eat("no3")) r.t = -1;
    if (eat("no5")) r.f = -1;
    r.ok = r.ok && i == name.size();
    return r;
}

/** Whether a view step's glyph names the pitch it plays as the bar's chord
 *  name does (section 38): the chord symbol, read back, gives each interval
 *  above its root a name (R; sus2 2, sus4 4, a third 3; a fifth 5; a sixth 6,
 *  a seventh 7, the °7's sixth 7; an add… its own). The glyph, octave marks
 *  dropped, is the name of the interval the step plays; the open cadence's
 *  2 that is no chord tone (Tone::Second) is 2; a step naming no member
 *  has no label. Empty when it is. */
std::string labelMismatch(const StepView& v, const Chord& ch)
{
    // A step that names no member (a rest, an embellishment, a Stab hit)
    // carries no label: the view word stays one word per step's meaning.
    if (v.tone == Tone::None || v.tone == Tone::Chord)
        return v.label == ToneLabel::None ? std::string()
                                          : "names no member, labelled " + std::to_string(static_cast<int>(v.label));
    if (v.note < 0) return {};
    std::string g = stepGlyphText(v);
    while (!g.empty() && (g.back() == '\'' || g.back() == ',')) g.pop_back();
    std::string want;
    const int d = ((v.note - ch.rootPc) % 12 + 12) % 12;
    if (v.tone == Tone::Second) want = "2";
    else
    {
        const std::string name = chordNameText(ch);
        const ChordRead r = readChordName(name);
        if (!r.ok) return "chord " + name + " unread";
        const std::string fl = "\xe2\x99\xad", sh = "\xe2\x99\xaf";
        static const char* kAddName[] = { "", "", "9", "", "3", "11", "", "5", "", "6", "7", "maj7" };
        std::vector<std::pair<int, std::string>> names = { { 0, "R" } };
        if (r.t >= 0) names.emplace_back(r.t, r.t == 2 ? "2" : r.t == 5 ? "4" : "3");
        if (r.f >= 0) names.emplace_back(r.f, "5");
        if (r.sv >= 0) names.emplace_back(r.sv, r.sv == 9 && !(r.t == 3 && r.f == 6) ? "6" : "7");
        for (int a : r.adds)
            names.emplace_back(a, a == 1 ? fl + "9" : a == 3 ? sh + "9" : a == 6 ? sh + "11" : a == 8 ? fl + "13"
                                                    : std::string(kAddName[a]));
        for (const auto& [iv, n] : names)
            if (iv == d) want = n;
        if (want.empty()) return "plays " + std::to_string(v.note) + ", no member of " + name;
    }
    if (g == want) return {};
    return "shows " + g + " for the " + want + " of " + chordNameText(ch);
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
            const Chord c = chordFromGui(seq.chordForGui.load());
            std::string line;
            for (int k = 0; k < n; ++k)
            {
                const auto& v = view[static_cast<size_t>(k)];
                line += v.head ? '[' : ' ';
                line += fnChar(v);
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
            entryNotes(cc, h1, h2, 8, B, nullptr, scratch, p1, pp1);
            entryNotes(cc, h2, h2, 8, B, nullptr, scratch, p2, pp2);
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
        // bar 5 into bars 6-7, both A: neither bar 6's head (head0) nor bar
        // 7's (its chord; head1 is bar 5's own) changes it.
        {
            const Config c = cfgOf(Form::AABA, 8, chords);
            const auto into5a = answerNotes(c, answerOf(Root, Root), 4);
            const auto into5b = answerNotes(c, answerOf(Root, Fifth), 4);
            check(into5a.size() == 2 && into5a != into5b,
                  "answer: AABA bar 4's approach follows bar 5's head (" + show(into5a) + " /" + show(into5b) + " )");
            const auto into6a = answerNotes(c, answerOf(Root, Root), 5);
            const auto into6b = answerNotes(c, answerOf(Fifth, Root), 5);
            const auto into6c = answerNotes(cfgOf(Form::AABA, 8, { 1, 6, 4, 2 }), answerOf(Root, Root), 5);
            check(into6a.size() == 2 && into6a == into6b && into6a == into6c,
                  "answer: AABA bar 5's approach follows no head of the A unit after it, bar 6's nor bar 7's ("
                      + show(into6a) + " /" + show(into6b) + " /" + show(into6c) + " )");
        }
        // AAAB, 4 bars: bar 3 is the B unit; the next two bars are A: neither
        // bar 4's head (head0) nor bar 5's (its chord, ii for vi) changes it.
        {
            const Config c = cfgOf(Form::AAAB, 4, chords);
            const auto a = answerNotes(c, answerOf(Root, Root), 3);
            const auto b = answerNotes(c, answerOf(Fifth, Root), 3);
            const auto d = answerNotes(cfgOf(Form::AAAB, 4, { 1, 2, 4, 5 }), answerOf(Root, Root), 3);
            check(a.size() == 2 && a == b && a == d,
                  "answer: AAAB bar 3's approach follows no head of the next phrase's A units, bar 4's nor bar 5's ("
                      + show(a) + " /" + show(b) + " /" + show(d) + " )");
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
        // end a hair past a beat, with that beat's onset played, while the
        // latch's own block starts exactly on that beat: a sample-count host
        // started two quarters before the first bar (a pre-roll) does so
        // inside the pre-roll (at q -0.5 and -1), one started at 0 nowhere in
        // these runs. There the beat is to be found from where playback
        // reached, not from the block start, so latches fall in the pre-roll
        // too. The chord
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
        int wrong = 0, errs = 0, cases = 0, beatPlayed = 0, beatAhead = 0, onBeatPlayed = 0;
        std::string why;
        struct Meter { int num, den; };
        struct Host { bool accumulated; double startQ; };
        for (const Host& h : { Host { true, 0.0 }, Host { false, 0.0 }, Host { true, -2.0 }, Host { false, -2.0 } })
            for (const Meter& m : { Meter { 4, 4 }, Meter { 6, 8 }, Meter { 3, 2 } })
                for (int block : { 32, 64, 128, 256, 512 })
                {
                    const double beatQ = 4.0 / m.den;
                    const int stepsPerBeat = static_cast<int>(beatQ / 0.25);
                    const double dq = block * kBpm / (60.0 * kSr);
                    const int afterBlocks = static_cast<int>(std::ceil((2.0 * beatQ + 0.5) / dq));
                    const long startSample = std::lround(h.startQ * 60.0 * kSr / kBpm);
                    std::vector<double> starts;
                    for (long n = 0; starts.empty() || starts.back() < 24.0 + 2.0 * beatQ + 1.0; ++n)
                        starts.push_back(h.accumulated ? (n == 0 ? h.startQ : starts.back() + dq)
                                                       : static_cast<double>(startSample + n * block) * kBpm / (60.0 * kSr));
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
                        if (starts[n] < h.startQ + 1.0 || std::abs(starts[n] - beat) > 1.0e-6) continue;
                        ++taken;
                        ++cases;
                        const int latchAt = static_cast<int>(n);
                        std::vector<On> got;
                        errs += run(latchAt, latchAt + afterBlocks, got);
                        // Before the first block nothing has played: the
                        // step before the host's start.
                        int lastStep = static_cast<int>(std::lround(h.startQ / 0.25)) - 1;
                        for (const On& o : got)
                            if (o.block < latchAt) lastStep = std::max(lastStep, o.step);
                        const int beatStep = (floorDiv(lastStep, stepsPerBeat) + 1) * stepsPerBeat;
                        // Steps are negative in the pre-roll: none is a step.
                        constexpr int kNone = std::numeric_limits<int>::min();
                        int expectStep = kNone;
                        for (size_t i = 0; expectStep == kNone && i < still.size(); ++i)
                            if (still[i].step >= beatStep) expectStep = still[i].step;
                        const bool played = lastStep >= static_cast<int>(std::lround(beat / 0.25));
                        (played ? beatPlayed : beatAhead)++;
                        if (played && starts[n] <= beat) ++onBeatPlayed;
                        int firstStep = kNone;
                        for (size_t i = 0; firstStep == kNone && i < got.size() && i < still.size(); ++i)
                            if (got[i].step != still[i].step || got[i].note != still[i].note) firstStep = got[i].step;
                        if (firstStep != expectStep && wrong++ < 3)
                        {
                            char tag[256];
                            std::snprintf(tag, sizeof tag, "%s%s host from q %g, %d/%d, %d-sample blocks, latch before q %.17g:"
                                          " first changed note at q %g, beat q %g, its first onset q %g",
                                          why.empty() ? "" : "; ", h.accumulated ? "accumulating" : "sample-count",
                                          h.startQ, m.num, m.den, block, starts[n], firstStep * 0.25, beatStep * 0.25,
                                          expectStep * 0.25);
                            why += tag;
                        }
                    }
                }
        check(beatPlayed > 0 && beatAhead > 0 && onBeatPlayed > 0,
              "Keys beat a hair off the block: both kinds occur, the block-start beat played before the latch ("
                  + std::to_string(beatPlayed) + ", " + std::to_string(onBeatPlayed)
                  + " of them with the block starting on the beat) and not (" + std::to_string(beatAhead) + ")");
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
                // last of the bar before (entryNotes renders it, and it can
                // itself lead into the next phrase), without
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

        // Stab's fill pulses repeat the chord and go up with their beat, as a
        // Riff or Lead Fill run does (below; scenarios 30 and 32 in 4/4).
        // Variation 3/5 (Re-pitch, Lift, Fill): every event of the last beat
        // is a voice of the bar's chord an octave up.
        int stabFills = 0, stabOff = 0;
        std::string stabWhy;
        for (int t = 0; t < 500; ++t)
        {
            Config c = base();
            c.part = Part::Stab;
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.len = U(1, 2); c.pulses = U(2, 12); c.rotation = U(0, 7);
            c.phraseBars = 8;
            c.form = static_cast<Form>(U(0, 3));
            c.shuffle = U(0, 1) ? 0.0f : 0.3f;
            c.layer = Layer::Off;
            c.variation = 0.6f;
            const Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(31500 + t), 0, c), c);
            RenderInput in;
            in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m;
            in.bar = 7; in.barLenQ = 4.0;
            BarRender r;
            renderBar(in, r);
            std::vector<int> chord;
            for (int e = 0; e < r.count; ++e)
                if (r.events[static_cast<size_t>(e)].onQ < 3.0 - 1.0e-9) chord.push_back(r.events[static_cast<size_t>(e)].note);
            if (chord.empty()) continue;
            for (int e = 0; e < r.count; ++e)
            {
                const Event& ev = r.events[static_cast<size_t>(e)];
                if (ev.onQ < 3.0 - 1.0e-9) continue;
                stabFills += std::abs(ev.vel - 72.0f / 127.0f) < 1.0e-4f;
                if (std::find(chord.begin(), chord.end(), ev.note - 12) == chord.end() && stabOff++ < 3)
                    stabWhy += " | t " + std::to_string(t) + " at " + std::to_string(ev.onQ) + ": " + std::to_string(ev.note);
            }
        }
        check(stabFills > 0 && stabOff == 0, "Stab: the fill pulses (" + std::to_string(stabFills)
                                                 + ") and every event of the lifted last beat sound the chord an octave up ("
                                                 + std::to_string(stabOff) + " off)" + stabWhy);

        // Riff and Lead: the Fill run goes up with its beat in every meter.
        // It is led into the next head as played without Lift (the first
        // head of the next bar, else of the one after), then lifted with the
        // beat: an octave above an unlifted head, onto a lifted one. A head
        // lies lifted where its bar is varied and the head in its last beat;
        // in a short bar (one beat or less, all of it last beat) that is
        // every head. The run is counted back from the head toward the note
        // it starts from: the bar's last note before its last beat, else the
        // note the bar entered with (in a short bar always that one), else
        // the head itself; it rises when the head lies at or above it. Every
        // bar ends on its last two notes as played without Lift, Fill notes
        // included, so the bar after it enters from that line. Bars rendered
        // in order (the entry notes chain), Shuffle 0, Evolve 0 (one motif
        // for every phrase), Variation 3/5 (Re-pitch, Lift, Fill: a Fill bar
        // is a lifted bar).
        static const double kBarLens[] = { 0.75, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0 };
        int runs = 0, runsIntoLifted = 0, runOff = 0, runsShort = 0, runsDown = 0, ends = 0, endsLifted = 0, endOff = 0;
        std::string runWhy, endWhy;
        for (int t = 0; t < 1500; ++t)
        {
            Config c = base();
            c.part = U(0, 1) ? Part::Riff : Part::Lead;
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.stepsPerBar = U(0, 1) ? 16 : (U(0, 1) ? 8 : 4);
            c.len = U(1, 2); c.pulses = U(1, 8); c.rotation = U(0, 7);
            c.phraseBars = U(0, 2) ? 4 : 8;
            c.form = U(0, 2) ? Form::AABA : static_cast<Form>(U(0, 3));
            c.shuffle = 0.0f;
            c.layer = Layer::Off;
            c.evolve = 0.0f;
            c.variation = 0.6f;
            const double barLenQ = kBarLens[U(0, 6)];
            const double lastBeatQ = std::max(0.0, barLenQ - 1.0);
            const Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(31600 + t), 0, c), c);
            const int N = phraseBarsOf(c), unitBars = N / 4, kBars = 2 * N + 2;
            const std::uint16_t mask = scaleMask(c.scaleType, c.scaleRoot);
            auto lifted = [&](int bar) {
                const int j = floorMod(bar, N);
                return (isBUnit(c, bar) && j % unitBars == unitBars - 1) || j == N - 1;
            };
            std::vector<BarRender> rs(static_cast<size_t>(kBars));
            int P = -1, PP = -1;
            bool slideIn = false;
            for (int bar = 0; bar < kBars; ++bar)
            {
                RenderInput in;
                in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m;
                in.bar = bar; in.barLenQ = barLenQ;
                in.prevNote = P; in.prevPrevNote = PP; in.slideIn = slideIn;
                BarRender& r = rs[static_cast<size_t>(bar)];
                renderBar(in, r);
                P = r.lastNote; PP = r.prevNote; slideIn = r.endsHeld;
            }
            for (int b = 0; b + 2 < kBars; ++b)
            {
                const BarRender& r = rs[static_cast<size_t>(b)];
                // The line's last two notes, as played without Lift.
                {
                    std::vector<int> line;
                    bool liftedEnd = false;
                    for (int e = 0; e < r.count; ++e)
                    {
                        const Event& ev = r.events[static_cast<size_t>(e)];
                        if (ev.strand != -1) continue;
                        const bool up = lifted(b) && ev.onQ >= lastBeatQ - 1.0e-9;
                        line.push_back(ev.note - c.transpose - (up ? 12 : 0));
                        liftedEnd = up;
                    }
                    if (line.size() >= 2)
                    {
                        ++ends;
                        endsLifted += liftedEnd;
                        const int want1 = line.back(), want2 = line[line.size() - 2];
                        if ((r.lastNote != want1 || r.prevNote != want2) && endOff++ < 3)
                            endWhy += " | t " + std::to_string(t) + " bar " + std::to_string(b) + ": "
                                    + std::to_string(r.prevNote) + " " + std::to_string(r.lastNote) + " want "
                                    + std::to_string(want2) + " " + std::to_string(want1);
                    }
                }
                std::vector<std::pair<int, int>> fills;   // (16th of the last beat, note)
                for (int e = 0; e < r.count; ++e)
                {
                    const Event& ev = r.events[static_cast<size_t>(e)];
                    if (ev.strand == -1 && std::abs(ev.vel - 72.0f / 127.0f) < 1.0e-4f)
                        fills.push_back({ static_cast<int>(std::lround((ev.onQ - lastBeatQ) / 0.25)), ev.note - c.transpose });
                }
                if (fills.empty()) continue;
                // The head the run leads into, as played without Lift.
                int L = -1;
                bool intoLifted = false;
                for (int ahead = 1; ahead <= 2 && L < 0; ++ahead)
                {
                    const BarRender& n = rs[static_cast<size_t>(b + ahead)];
                    for (int k = 0; k < n.viewSteps && L < 0; ++k)
                    {
                        const StepView& v = n.view[static_cast<size_t>(k)];
                        if (v.note < 0 || !v.head) continue;
                        intoLifted = lifted(b + ahead) && k * (4.0 / c.stepsPerBar) >= lastBeatQ - 1.0e-9;
                        L = v.note - (intoLifted ? 12 : 0);
                    }
                }
                if (L < 0) continue;
                int from = -1;
                for (int e = 0; e < r.count; ++e)
                {
                    const Event& ev = r.events[static_cast<size_t>(e)];
                    if (ev.strand == -1 && ev.onQ < lastBeatQ - 1.0e-9) from = ev.note - c.transpose;
                }
                runsShort += lastBeatQ < 1.0e-9;
                if (from < 0 && b > 0) from = rs[static_cast<size_t>(b - 1)].lastNote;
                if (from < 0) from = L;
                const int dir = L >= from ? -1 : 1;
                ++runs;
                runsIntoLifted += intoLifted;
                runsDown += dir > 0;
                auto runs4 = [&](int d) {
                    for (const auto& [j, note] : fills)
                        if (note - 12 != scaleSteps(L, d * (4 - j), mask)) return false;
                    return true;
                };
                const bool ok = lifted(b) && runs4(dir);
                if (!ok && runOff++ < 3)
                {
                    runWhy += " | t " + std::to_string(t) + " len " + std::to_string(barLenQ) + " bar " + std::to_string(b)
                            + " from " + std::to_string(from) + " into " + std::to_string(L)
                            + (intoLifted ? " (lifted)" : "") + ":";
                    for (const auto& [j, note] : fills) runWhy += " " + std::to_string(note);
                }
            }
        }
        check(runs > 500 && runsIntoLifted > 50 && runsShort > 50 && runsDown > 50 && runs - runsDown > 50,
              "Riff / Lead: Fill runs (" + std::to_string(runs) + ", " + std::to_string(runsIntoLifted) + " into a lifted head, "
                  + std::to_string(runsShort) + " in a bar of a beat or less, " + std::to_string(runsDown) + " falling)");
        check(runOff == 0, "Riff / Lead: a Fill run is led from the note before its beat into the head as played without "
                           "Lift and goes up with its beat (" + std::to_string(runOff) + " of " + std::to_string(runs) + " off)" + runWhy);
        check(ends > 5000 && endsLifted > 500 && endOff == 0,
              "Riff / Lead: a bar ends on its last two notes as played without Lift (" + std::to_string(endOff) + " of "
                  + std::to_string(ends) + " off, " + std::to_string(endsLifted) + " ending in a lifted beat)" + endWhy);

        // The layers and the ratchet go up with the beat (§3.7, §3.10). The
        // Octave layer doubles the line as it plays: a lifted note, and
        // Stab's lifted chord, are doubled lifted. The Chord layer's chord on
        // a head of the lifted beat goes up with it (not Bass's: its Lift is
        // an Octave pulse, which is no head). A ratchet repeat sounds what
        // its hit sounds, lifted once. Oracle: the varied bar at Variation
        // 2/5 to 5/5 (Lift, then Fill, Ratchet, Drop) rendered with the
        // layer and with Layer Off. The main line is the same in both; the
        // Octave layer is the main line 12 down (Bass: up), onset by onset;
        // a Chord-layer hit is the bar's voicing (voicingForBar), 12 up in
        // the lifted beat; a repeat (its velocity) sounds the notes of the
        // onset before it.
        const float tailVel = 85.0f / 127.0f * 0.9f;
        int mainOff = 0, octOff = 0, chordOff = 0, tailOff = 0, tails = 0;
        int octLifted[2] = { 0, 0 }, chordLifted = 0, tailsLifted[2] = { 0, 0 };   // [0] Riff / Lead, [1] Stab
        std::string layerWhy;
        for (int t = 0; t < 3000; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 3));   // Bass, Riff, Lead, Stab
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.len = U(1, 2); c.pulses = U(2, 12); c.rotation = U(0, 7);
            c.phraseBars = 8;
            c.form = static_cast<Form>(U(0, 3));
            c.shuffle = U(0, 1) ? 0.0f : 0.3f;
            c.variation = 0.2f * static_cast<float>(U(2, 5));
            const Layer layer = c.part == Part::Stab || U(0, 1) ? Layer::Octave : Layer::Chord;
            const bool stab = c.part == Part::Stab, bass = c.part == Part::Bass;
            const Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(31700 + t), 0, c), c);
            const int pn = U(45, 69), ppn = U(45, 69);
            BarRender r[2];
            Config v[2] = { c, c };
            for (int i = 0; i < 2; ++i)
            {
                v[i].layer = i == 0 ? Layer::Off : layer;
                RenderInput in;
                in.cfg = &v[i]; in.motif = &m; in.nextPhraseMotif = &m;
                in.bar = 7; in.barLenQ = 4.0;   // the last of phrase 0: varied in every form
                in.prevNote = pn; in.prevPrevNote = ppn;
                renderBar(in, r[i]);
            }
            // Notes per onset, sorted: the main line (strand −1) or the layer.
            using Groups = std::map<long long, std::vector<int>>;
            auto groups = [](const BarRender& b, bool layerStrands) {
                Groups g;
                for (int e = 0; e < b.count; ++e)
                {
                    const Event& ev = b.events[static_cast<size_t>(e)];
                    if ((ev.strand >= 5) == layerStrands) g[std::llround(ev.onQ * 1.0e6)].push_back(ev.note);
                }
                for (auto& [q, n] : g) std::sort(n.begin(), n.end());
                return g;
            };
            const Groups main = groups(r[1], false), lay = groups(r[1], true);
            auto inLast = [](long long q) { return q >= 3000000 - 1; };
            auto note = [&](const std::string& what, long long q, const std::vector<int>& want, const Groups& g) {
                if (layerWhy.size() < 400)
                {
                    const auto it = g.find(q);
                    layerWhy += " | t " + std::to_string(t) + " part " + std::to_string(static_cast<int>(c.part)) + " " + what
                              + " at " + std::to_string(static_cast<double>(q) * 1.0e-6) + ": want";
                    for (int n : want) layerWhy += " " + std::to_string(n);
                    layerWhy += ", got";
                    if (it != g.end())
                        for (int n : it->second) layerWhy += " " + std::to_string(n);
                }
            };
            if (main != groups(r[0], false)) ++mainOff;

            if (layer == Layer::Octave)
            {
                Groups want;
                for (const auto& [q, n] : main)
                    for (int x : n) want[q].push_back(std::clamp(x + (bass ? 12 : -12), 0, 127));
                for (auto& [q, n] : want) std::sort(n.begin(), n.end());
                if (want != lay)
                {
                    ++octOff;
                    for (const auto& [q, n] : want)
                        if (lay.count(q) == 0 || lay.at(q) != n) { note("octave", q, n, lay); break; }
                }
                for (const auto& [q, n] : lay)
                    if (inLast(q) && !bass) octLifted[stab] += static_cast<int>(n.size());
            }
            else
            {
                int vo[kMaxVoices];
                const int nv = voicingForBar(v[1], 7, c.part == Part::Lead ? 48 : 60, vo);
                for (const auto& [q, n] : lay)
                {
                    const int s = inLast(q) && !bass ? 12 : 0;
                    std::vector<int> want;
                    for (int i = 0; i < nv; ++i) want.push_back(std::clamp(vo[i] + s, 0, 127));
                    std::sort(want.begin(), want.end());
                    if (main.count(q) == 0 || n != want) { ++chordOff; note("chord", q, want, lay); }
                    if (s != 0) chordLifted += static_cast<int>(n.size());
                }
            }

            const std::vector<int>* before = nullptr;
            for (const auto& [q, n] : main)
            {
                bool tail = false;
                for (int e = 0; e < r[1].count; ++e)
                {
                    const Event& ev = r[1].events[static_cast<size_t>(e)];
                    if (ev.strand < 5 && std::llround(ev.onQ * 1.0e6) == q && std::abs(ev.vel - tailVel) < 1.0e-4f) tail = true;
                }
                if (tail)
                {
                    ++tails;
                    if (!bass) tailsLifted[stab] += inLast(q);
                    if (before == nullptr || *before != n)
                    {
                        ++tailOff;
                        note("repeat", q, before ? *before : std::vector<int> {}, main);
                    }
                }
                before = &n;
            }
        }
        check(mainOff == 0, "a layer leaves the main line as it is (" + std::to_string(mainOff) + " bars off)");
        check(octLifted[0] > 300 && octLifted[1] > 300 && octOff == 0,
              "Octave layer: the line doubled as it plays, lifted notes (" + std::to_string(octLifted[0])
                  + ") and Stab's lifted chords (" + std::to_string(octLifted[1]) + " voices) included ("
                  + std::to_string(octOff) + " bars off)" + layerWhy);
        check(chordLifted > 300 && chordOff == 0,
              "Chord layer: a hit is the bar's chord, an octave up in the lifted beat but for Bass (" + std::to_string(chordLifted)
                  + " lifted voices, " + std::to_string(chordOff) + " hits off)" + layerWhy);
        check(tailsLifted[0] > 300 && tailsLifted[1] > 300 && tailOff == 0,
              "a ratchet repeat sounds what its hit sounds, lifted once (" + std::to_string(tails) + " repeats, "
                  + std::to_string(tailsLifted[0]) + " Riff / Lead and " + std::to_string(tailsLifted[1]) + " Stab lifted; "
                  + std::to_string(tailOff) + " off)" + layerWhy);
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
        // down, and Bass's octave pulse is no head and leads into none (the
        // pulses passed are counted where the motif has a head). Every
        // approach step whose next sounding head lies within two bars lies
        // where its function puts it from that head; every Fill note (the
        // played events, by velocity) lies its distance from the next bar's
        // first sounding head on the scale. A Fill note on a step (one the
        // motif leaves a rest: nothing else sounds there) is no approach
        // step: the view shows it as the run it approaches the head with,
        // and the Fill check places it. All forms, ABAB weighted;
        // phrases of 4, 8 and 16 bars; Variation 0, 2/5, 3/5 and 1. In 4/4
        // and in bars shorter and longer: a bar of a beat or less is all
        // last beat, a bar of n/32 starts it on the 16th a 32nd after
        // barLen − 1 (§3.7), a bar longer than 4/4 ends its steps before
        // it. A quarter of the trials draws the bars that lead into a
        // cadence moved off Bass's octave pulse onto the head before it.
        // A bar enters with the last note of the bar before rendered on its
        // own (§3.9); in a phrase's first bar that bar is the last of the
        // phrase before, and its lookahead reads this phrase from this
        // phrase's motif, not from the one Evolve makes for the phrase
        // after. An embellishment that opens a bar before its last beat (a
        // Fill note can sound before a step there) lies where its function
        // puts it from that note, a Passing note a scale step towards the
        // next sounding head.
        std::mt19937 rng(32);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        struct Note { int note = -1; Fn fn = Fn::Rest; bool head = false, motifHead = false, octave = false, fill = false; };
        using Bar = std::array<Note, kMaxBarSteps>;
        using Line = std::vector<Bar>;
        constexpr int kBars = 40;
        auto isApproachFn = [](Fn f) { return f == Fn::ApproachUp || f == Fn::ApproachDown || f == Fn::ApproachChromatic; };
        const float kFillVel = 72.0f / 127.0f;
        static const float kVariation[] = { 0.0f, 0.4f, 0.6f, 1.0f };
        static const int kPhraseBars[] = { 4, 8, 16 };
        static const int kMeters[][2] = { { 4, 4 }, { 4, 4 }, { 3, 4 }, { 2, 4 }, { 1, 4 }, { 5, 8 }, { 7, 8 },
                                          { 7, 16 }, { 5, 16 }, { 3, 16 }, { 15, 32 }, { 7, 32 }, { 5, 4 } };
        constexpr int kNumMeters = static_cast<int>(sizeof kMeters / sizeof kMeters[0]);
        struct Meter { int num = 4, den = 4; double barLenQ = 4.0, lastBeatQ = 3.0; };
        // The last beat starts on the first 16th at or after barLen − 1.
        auto drawMeter = [&]() {
            const int i = U(0, kNumMeters - 1);
            Meter me;
            me.num = kMeters[i][0];
            me.den = kMeters[i][1];
            me.barLenQ = 4.0 * me.num / me.den;
            me.lastBeatQ = 0.0;
            while (me.lastBeatQ < me.barLenQ - 1.0 - 1.0e-9) me.lastBeatQ += 0.25;
            return me;
        };
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
            // Pulses over the whole motif, up to every step. An approach
            // leads into a cadence moved off Bass's octave pulse only in a
            // bar of two heads (the cadence moves to the first) after a bar
            // that ends on an approach: the two motif bars differ, and a
            // beat holds two pulses (more than half the steps sound).
            c.len = U(1, 2); c.pulses = U(1, c.len * c.stepsPerBar); c.rotation = U(0, 7);
            return c;
        };

        // Plays c with motif m from bar 0 and reads the bar views into line.
        // turnQ >= 0: Evolve turns to turnTo in the first block from turnQ on,
        // which starts at turnedAt.
        auto playLine = [&](const Config& c, const Meter& me, const Motif& m, double turnQ, float turnTo, Line& line,
                            double& turnedAt) {
            line.assign(kBars, Bar {});
            turnedAt = -1.0;
            // Evolve leaves the rhythm, and so the heads, as they are.
            std::array<bool, kMaxMotifSteps> heads {};
            computeHeads(m, heads);
            bool loaded = false;
            return play(c, 0.0, kBars * me.barLenQ, me.num, me.den, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
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
                {
                    const int s2 = floorMod(b, m.len) * m.stepsPerBar + k;
                    bar[static_cast<size_t>(k)] = { v[static_cast<size_t>(k)].note, v[static_cast<size_t>(k)].fn,
                                                    v[static_cast<size_t>(k)].head, heads[static_cast<size_t>(s2)], false,
                                                    v[static_cast<size_t>(k)].note >= 0
                                                        && m.slots[static_cast<size_t>(s2)].fn == Fn::Rest };
                }
            });
        };

        // The note bar b enters with (§3.9): the last of the bar before,
        // rendered on its own from its chord root, without the Lift. In a
        // bar whose run reads its entry (no step before its last beat) it
        // can differ from the note heard before the bar. `after` is the
        // Evolve phrase 2 on latches. `own`: the bar before reads past its
        // phrase from its own phrase's motif (for the count of entries the
        // next phrase's motif decides).
        using Entry = std::function<int(int, bool)>;
        auto entryOf = [&](const Config& c, const Config& after, const Motif& m, const Meter& me) -> Entry {
            const Motif f = fitMotif(m, c);
            return [c, after, f, me](int b, bool own) {
                const int N = phraseBarsOf(c);
                auto motifOf = [&](int p) { return evolvedMotif(f, p >= 2 ? after : c, p); };
                const int pb = floorDiv(b - 1, N);
                BarRender scratch;
                int p = -1, pp = -1;
                entryNotes(floorDiv(b, N) >= 2 ? after : c, motifOf(pb), motifOf(own ? pb : pb + 1), b, me.barLenQ,
                           nullptr, scratch, p, pp);
                return p;
            };
        };

        struct Tally
        {
            int runErrs = 0, checked = 0, bad = 0, cadenceTargets = 0, dropSkips = 0, octaveSkips = 0;
            int fillChecked = 0, fillBad = 0, fillCadence = 0;
            int crossing = 0, moved = 0, fillCrossing = 0, fillMoved = 0;
            int checkedOther = 0, fillOther = 0, movedCadence = 0;
            int entryChecked = 0, entryBad = 0, entryFirst = 0, entryDecided = 0;
            std::string why, fillWhy, entryWhy;
        };
        // Checks the approach notes and the Fill notes of run r that sound
        // from fromQ on. With `without` (the same trial without the turn), it
        // counts those that lead from phrase 1 into phrase 2, and among them
        // those whose head there is another without the turn.
        auto checkRun = [&](int t, const Config& c, const Meter& me, const Entry& entered, Line& line, const Run& r,
                            double fromQ, Line* without, Tally& y) {
            y.runErrs += r.hangs + r.orphanOffs + r.badGlides + r.polyMain;

            const int N = phraseBarsOf(c), unitBars = N / 4;
            const int ops = static_cast<int>(std::lround(c.variation * 5.0f));
            const bool bass = c.part == Part::Bass;
            const double stepQ = 4.0 / c.stepsPerBar;
            const std::uint16_t mask = scaleMask(c.scaleType, c.scaleRoot);
            const bool other = me.num != 4 || me.den != 4;
            auto inLast = [&](int k) { return k * stepQ >= me.lastBeatQ - 1.0e-9; };
            auto lifted = [&](int bar) {
                const int j = floorMod(bar, N);
                return ops >= 2 && ((isBUnit(c, bar) && j % unitBars == unitBars - 1) || j == N - 1);
            };
            auto cadenceBar = [&](int bar) { return c.form == Form::ABAB && floorMod(bar, N) % unitBars == unitBars - 1; };

            // The line under the octave Lift. Bass's octave pulse is the last
            // step of the last beat that sounds a motif step (a Fill note on
            // an empty step after it is none).
            auto underLift = [&](Line& L) {
                for (int b = 0; b < kBars; ++b)
                {
                    if (!lifted(b)) continue;
                    Bar& bar = L[static_cast<size_t>(b)];
                    int lastK = -1;
                    for (int k = 0; k < kMaxBarSteps; ++k)
                        if (bar[static_cast<size_t>(k)].note >= 0 && inLast(k))
                        {
                            if (!bar[static_cast<size_t>(k)].fill) lastK = k;
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
                        if (n.octave) { h.octaves += n.motifHead; continue; }
                        if (!n.head) continue;
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
            // A head in a later bar whose cadence Bass's octave pulse moved
            // off the last pulse onto it. Approach notes lead into one. A
            // Fill's run reaches one only past a bar that sounds no head
            // (lifted bars lie two apart or more), and Euclid spaces the
            // pulses too evenly for that bar to be empty while the next
            // holds two heads, but where a beat is cut short at the bar end;
            // the draw reaches none, so only approach notes are counted.
            auto movedCadenceIn = [&](int b, const Target& h) {
                if (!isCadence(h) || h.bar <= b) return false;
                for (int j = h.step + 1; j < kMaxBarSteps; ++j)
                {
                    const Note& n = line[static_cast<size_t>(h.bar)][static_cast<size_t>(j)];
                    if (n.octave && n.motifHead) return true;
                }
                return false;
            };

            for (int b = 0; b + 2 < kBars; ++b)
                for (int k = 0; k < kMaxBarSteps; ++k)
                {
                    const Note& a = line[static_cast<size_t>(b)][static_cast<size_t>(k)];
                    if (a.note < 0 || a.fill || !isApproachFn(a.fn)) continue;
                    if (me.barLenQ * b + k * stepQ < fromQ + 1.0e-6) continue;
                    const Target h = nextHead(line, b, k);
                    if (h.note < 0) continue;
                    const int want = a.fn == Fn::ApproachUp ? scaleStep(h.note, -1, mask)
                                   : a.fn == Fn::ApproachDown ? scaleStep(h.note, 1, mask) : h.note - 1;
                    ++y.checked;
                    y.checkedOther += other;
                    y.movedCadence += movedCadenceIn(b, h);
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
            // the note the bar enters with, into the next bar's first sounding
            // head; the run sounds with the Lift (not Bass). That head is
            // looked for from bar b + 1 up to two bars on, so the last bar the
            // loop reaches is b + 3.
            for (int b = 1; b + 3 < kBars; ++b)
            {
                const Target h = nextHead(line, b + 1, -1);
                if (h.note < 0) continue;
                int from = -1;
                std::vector<std::pair<double, int>> fills;
                for (const Played& p : r.events)
                {
                    if (p.ev.strandId != -1 || p.ev.type != VoiceEvent::Type::NoteOn) continue;
                    const double inBar = p.q - me.barLenQ * b;
                    const int note = p.ev.note - c.transpose;
                    if (inBar < -1.0e-3 || inBar > me.barLenQ - 1.0e-3) continue;
                    if (std::abs(p.ev.velocity - kFillVel) < 1.0e-4)
                    {
                        if (p.q >= fromQ + 1.0e-6) fills.emplace_back(inBar, note);
                    }
                    else if (inBar < me.lastBeatQ - 1.0e-3) from = note;
                }
                if (fills.empty()) continue;
                if (from < 0) from = entered(b, false);
                if (from < 0) continue;
                const int dir = h.note >= from ? -1 : 1;
                const bool cross = without && crosses(b, h);
                const bool moved = cross && nextHead(*without, b + 1, -1).note != h.note;
                for (const auto& [inBar, note] : fills)
                {
                    const int j = static_cast<int>(std::lround((inBar - me.lastBeatQ) / 0.25));
                    const int want = scaleSteps(h.note, dir * (4 - j), mask) + (bass ? 0 : 12);
                    ++y.fillChecked;
                    y.fillOther += other;
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

            // The embellishment that opens a bar, before its last beat, from
            // the note the bar enters with.
            const int center = tonicNote(partAnchor(c.part), c.scaleRoot) + 6;
            for (int b = 1; b + 2 < kBars; ++b)
            {
                const Bar& bar = line[static_cast<size_t>(b)];
                int k = 0;
                while (k < kMaxBarSteps && bar[static_cast<size_t>(k)].note < 0 && !bar[static_cast<size_t>(k)].octave) ++k;
                if (k == kMaxBarSteps || inLast(k) || me.barLenQ * b + k * stepQ < fromQ + 1.0e-6) continue;
                const Note& a = bar[static_cast<size_t>(k)];
                if (a.octave || a.fill || a.head) continue;
                const int from = entered(b, false);
                if (from < 0) continue;
                int want = -1;
                switch (a.fn)
                {
                    case Fn::Repeat:        want = from; break;
                    case Fn::NeighbourUp:   want = scaleStep(from, 1, mask); break;
                    case Fn::NeighbourDown: want = scaleStep(from, -1, mask); break;
                    case Fn::Octave:        want = from > center ? from - 12 : from + 12; break;
                    case Fn::Passing:
                    {
                        const Target h = nextHead(line, b, k);
                        if (h.note >= 0 && h.note != from) want = scaleStep(from, h.note > from ? 1 : -1, mask);
                        break;
                    }
                    default: break;
                }
                if (want < 0) continue;
                ++y.entryChecked;
                if (floorMod(b, N) == 0)
                {
                    ++y.entryFirst;
                    y.entryDecided += entered(b, true) != from;
                }
                if (a.note != want && y.entryBad++ < 3)
                {
                    char s[200];
                    std::snprintf(s, sizeof s, " | t %d form %d part %d: bar %d step %d fn %d note %d, entered from %d wants %d",
                                  t, static_cast<int>(c.form), static_cast<int>(c.part), b, k, static_cast<int>(a.fn),
                                  a.note, from, want);
                    y.entryWhy += s;
                }
            }
        };

        Tally one;
        for (int t = 0; t < 2000; ++t)
        {
            Config c = draw();
            // A quarter of the trials draws the bars a moved cadence follows
            // (see draw): Bass in ABAB under Lift without Drop, two motif
            // bars, more than half the steps sounding.
            if (t % 4 == 3)
            {
                c.part = Part::Bass;
                c.form = Form::ABAB;
                c.variation = U(0, 1) ? 0.4f : 0.6f;
                c.len = 2;
                c.pulses = U(c.stepsPerBar + 1, 2 * c.stepsPerBar);
            }
            const Meter me = drawMeter();
            const Motif m = generateMotif(static_cast<std::uint32_t>(32000 + t), 0, c);
            Line line;
            double unturned = -1.0;
            const Run r = playLine(c, me, m, -1.0, 0.0f, line, unturned);
            checkRun(t, c, me, entryOf(c, c, m, me), line, r, -1.0e9, nullptr, one);
        }
        check(one.runErrs == 0, "lookahead as played: no hanging or orphan note, no bad glide (" + std::to_string(one.runErrs) + ")");
        check(one.cadenceTargets > 0 && one.dropSkips > 0 && one.octaveSkips > 0,
              "lookahead as played: approach notes into cadence heads (" + std::to_string(one.cadenceTargets)
                  + "), past dropped heads (" + std::to_string(one.dropSkips) + ") and past Bass's octave pulse ("
                  + std::to_string(one.octaveSkips) + ")");
        check(one.checkedOther > 0 && one.fillOther > 0 && one.movedCadence > 0,
              "lookahead as played: approach notes (" + std::to_string(one.checkedOther) + ") and Fill notes ("
                  + std::to_string(one.fillOther) + ") in meters other than 4/4; approach notes into a later bar's "
                  "cadence moved off Bass's octave pulse (" + std::to_string(one.movedCadence) + ")");
        check(one.bad == 0, "lookahead as played: an approach note leads into the head that sounds (" + std::to_string(one.bad)
                                + " of " + std::to_string(one.checked) + " off)" + one.why);
        check(one.fillCadence > 0, "lookahead as played: Fill runs into cadence heads (" + std::to_string(one.fillCadence) + ")");
        check(one.fillBad == 0, "lookahead as played: a Fill runs into the head that sounds (" + std::to_string(one.fillBad)
                                    + " of " + std::to_string(one.fillChecked) + " off)" + one.fillWhy);
        check(one.entryFirst > 0 && one.entryDecided > 0,
              "lookahead as played: embellishments opening a phrase's first bar (" + std::to_string(one.entryFirst) + "), "
                  + std::to_string(one.entryDecided) + " of them entered from a note the next phrase's motif decides");
        check(one.entryBad == 0, "lookahead as played: an embellishment opening a bar lies where its function puts it from "
                                 "the note the bar enters with (" + std::to_string(one.entryBad) + " of "
                                 + std::to_string(one.entryChecked) + " off)" + one.entryWhy);

        std::printf("\n== 33. The line leads into the next phrase with the Evolve it latches\n");
        // Evolve is latched at phrase entry (§3.8), so turning it in a phrase
        // gives the next phrase another motif; the line led into the motif the
        // next phrase had before the turn. Evolve turns by 0.5 in phrase 1,
        // half the time in its last bar, always inside a bar's first three
        // beats, before its last beat in other meters, at its start in a bar
        // that is all last beat (the bar re-renders, and the view read after
        // the turn shows it). Oracle of 32, for what sounds from the block of
        // the turn on; the same trial unturned counts the heads the turn
        // moves.
        Tally two;
        for (int t = 0; t < 1000; ++t)
        {
            const Config c = draw();
            const Meter me = drawMeter();
            const Motif m = generateMotif(static_cast<std::uint32_t>(33000 + t), 0, c);
            const int N = phraseBarsOf(c);
            const int turnBar = U(0, 1) ? 2 * N - 1 : U(N, 2 * N - 1);
            const int before = std::max(0, static_cast<int>(std::lround(me.lastBeatQ / 0.25)) - 1);
            const double turnQ = me.barLenQ * turnBar + 0.25 * U(0, before);
            const float turnTo = c.evolve >= 0.5f ? 0.0f : 1.0f;
            Line line, without;
            double turnedAt = -1.0, unturned = -1.0;
            const Run r = playLine(c, me, m, turnQ, turnTo, line, turnedAt);
            const Run r0 = playLine(c, me, m, -1.0, 0.0f, without, unturned);
            two.runErrs += r0.hangs + r0.orphanOffs + r0.badGlides + r0.polyMain;
            Config turned = c;
            turned.evolve = turnTo;
            checkRun(t, c, me, entryOf(c, turned, m, me), line, r, turnedAt, &without, two);
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

    std::printf("\n== 34. An ABAB cadence under Bass's Lift lands on the last head that sounds\n");
    {
        // ABAB's B ends closed, on R (§3.7). Bass's Lift plays an Octave of
        // the note before it on the last pulse of the last beat, in place of
        // what is there; when that pulse was the bar's last head, the cadence
        // went with it and the bar ended on what the head before played.
        // Oracle from the motif: the last pulse of the last beat and the heads
        // (computeHeads). The cadence is on the last head that is not that
        // pulse (Bass), on the last head (Riff, Lead: their Lift transposes
        // the last beat). Its note is the root at the slot's octave (+12 in a
        // lifted last beat), in the view and as an onset; Bass's last pulse
        // plays an Octave of the note that sounds before it, a step's or a
        // Fill run note's. The last bar of a
        // B unit, Variation 2/5 to 4/5 (Lift, Fill, Ratchet), 4/4 and 3/4.
        std::mt19937 rng(34);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        int bars = 0, bassBars = 0, moved = 0, noCadence = 0, off = 0, offOnset = 0, octaves = 0, offOctave = 0;
        std::string why;
        for (int t = 0; t < 3000; ++t)
        {
            Config c = base();
            const int pick = U(0, 3);
            c.part = pick < 2 ? Part::Bass : pick == 2 ? Part::Riff : Part::Lead;
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.len = U(1, 2); c.pulses = U(2, 12); c.rotation = U(0, 7);
            c.phraseBars = 4 << U(0, 2);
            c.form = Form::ABAB;
            c.shuffle = 0.0f;
            c.layer = Layer::Off;
            c.variation = 0.2f * static_cast<float>(U(2, 4));
            const double barLenQ = U(0, 3) == 0 ? 3.0 : 4.0;
            const Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(34000 + t), 0, c), c);
            const int N = phraseBarsOf(c), unitBars = N / 4;
            const int bar = U(0, 1) ? 2 * unitBars - 1 : N - 1;   // the last bar of a B unit
            RenderInput in;
            in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m;
            in.bar = bar; in.barLenQ = barLenQ;
            in.prevNote = U(36, 60); in.prevPrevNote = U(36, 60);
            BarRender r;
            renderBar(in, r);

            std::array<bool, kMaxMotifSteps> heads {};
            computeHeads(m, heads);
            const int spb = m.stepsPerBar, mb = floorMod(bar, m.len);
            const double stepQ = 4.0 / spb;
            const bool bass = c.part == Part::Bass;
            auto slotOf = [&](int k) -> const Slot& { return m.slots[static_cast<size_t>(mb * spb + k)]; };
            auto isHead = [&](int k) { return heads[static_cast<size_t>(mb * spb + k)]; };
            auto inLast = [&](int k) { return k * stepQ >= barLenQ - 1.0 - 1.0e-9; };
            int lastPulse = -1, lastHead = -1, cadenceK = -1;
            for (int k = 0; k < r.viewSteps; ++k)
            {
                if (slotOf(k).fn == Fn::Rest) continue;
                if (inLast(k)) lastPulse = k;
                if (isHead(k)) lastHead = k;
            }
            for (int k = 0; k < r.viewSteps; ++k)
                if (slotOf(k).fn != Fn::Rest && isHead(k) && !(bass && k == lastPulse)) cadenceK = k;
            ++bars;
            bassBars += bass;
            if (bass && lastHead >= 0 && lastHead == lastPulse) ++moved;

            if (bass && lastPulse >= 0)
            {
                // The note before it is the one that sounds before it: a
                // step's, or a Fill run note on a 16th the motif leaves free.
                int p = in.prevNote;
                double pQ = -1.0;
                for (int e = 0; e < r.count; ++e)
                {
                    const Event& ev = r.events[static_cast<size_t>(e)];
                    if (ev.line == Line::Main && ev.onQ < lastPulse * stepQ - 1.0e-9 && ev.onQ > pQ) { pQ = ev.onQ; p = ev.note; }
                }
                ++octaves;
                offOctave += std::abs(r.view[static_cast<size_t>(lastPulse)].note - p) != 12;
            }
            if (cadenceK < 0) { ++noCadence; continue; }
            const int tonic = tonicNote(partAnchor(c.part), c.scaleRoot);
            const int want = chordToneNote(r.chord, Root, slotOf(cadenceK).octave, tonic)
                           + (!bass && inLast(cadenceK) ? 12 : 0);
            const int got = r.view[static_cast<size_t>(cadenceK)].note;
            if (got != want && off++ < 4)
                why += " | t " + std::to_string(t) + " part " + std::to_string(static_cast<int>(c.part)) + " step "
                     + std::to_string(cadenceK) + ": " + std::to_string(got) + " want " + std::to_string(want);
            bool onset = false;
            for (int e = 0; e < r.count; ++e)
            {
                const Event& ev = r.events[static_cast<size_t>(e)];
                onset |= ev.line == Line::Main && std::abs(ev.onQ - cadenceK * stepQ) < 1.0e-9 && ev.note == want;
            }
            offOnset += !onset;
        }
        check(bassBars > 1000 && bars - bassBars > 500, "bars compared (" + std::to_string(bassBars) + " Bass, "
                                                         + std::to_string(bars - bassBars) + " Riff/Lead)");
        check(moved > 100, "Bass bars whose last head is the Octave pulse (" + std::to_string(moved) + ", "
                               + std::to_string(noCadence) + " bars without another head)");
        check(off == 0, "the cadence plays R on the last head that sounds (" + std::to_string(off) + " off)" + why);
        check(offOnset == 0, "the cadence sounds as an onset at its step (" + std::to_string(offOnset) + " off)");
        check(octaves > 1000 && offOctave == 0, "Bass's last pulse plays an Octave of the note before it ("
                                                    + std::to_string(offOctave) + " of " + std::to_string(octaves) + " off)");
    }

    std::printf("\n== 35. A re-render after a bar's last onset keeps the slide into the next downbeat\n");
    {
        // A held note waits for its strand's next onset. A re-render in the
        // bar released it when the bar had no onset left on that strand,
        // though the bar still ended held: the slide was cut, the note went
        // silent before the downbeat, and the next bar's first onset on the
        // strand started fresh. Bars that end held (in the same trial
        // unchanged); the change falls after the strand's last onset in the
        // bar, before the downbeat.
        //  - Evolve turns in a bar whose lookahead does not reach the next
        //    phrase: every bar up to the two before the phrase end renders
        //    as it did, so every event before them is the unchanged trial's.
        //  - Keys: the chord changes on a beat after the bar's last onset.
        //    Every event before the downbeat is the unchanged trial's, and
        //    in the next bar up to the glide the same events start and end,
        //    gliding as they did (the notes follow the new chord).
        std::mt19937 rng(35);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        static const int kPhraseBars[] = { 4, 8, 16 };
        using Key = std::tuple<int, int, int, int, int>;   // sample, type, note, strand, artic
        auto keyOf = [](const Played& p, bool note) {
            return Key { p.sample, static_cast<int>(p.ev.type), note ? p.ev.note : 0, p.ev.strandId,
                         static_cast<int>(p.ev.artic) };
        };
        auto between = [&](const Run& r, double fromQ, double toQ, bool note) {
            std::vector<Key> v;
            for (const Played& p : r.events)
                if (p.q >= fromQ && p.q < toQ) v.push_back(keyOf(p, note));
            return v;
        };
        // The bars that end held: a strand's first onset in the next bar
        // glides. `strandOn`: that strand's last onset in the bar; `lastOn`:
        // the bar's last onset on any strand; `glideQ`: the glide.
        struct Held { int bar, strand, note; double strandOn, lastOn, glideQ; };
        auto heldBars = [&](const Run& r, int fromBar, int toBar, const std::function<bool(int)>& ok) {
            std::vector<Held> v;
            for (int b = fromBar; b < toBar; ++b)
            {
                if (!ok(b)) continue;
                const double barQ = 4.0 * b, nextQ = 4.0 * (b + 1);
                double lastOn = -1.0;
                std::map<int, double> strandOn, firstNext;
                std::map<int, int> strandNote;
                std::map<int, bool> glides;
                for (const Played& p : r.events)
                {
                    if (p.ev.type != VoiceEvent::Type::NoteOn) continue;
                    if (p.q >= barQ - 1.0e-3 && p.q < nextQ - 1.0e-3)
                    {
                        lastOn = p.q;
                        strandOn[p.ev.strandId] = p.q;
                        strandNote[p.ev.strandId] = p.ev.note;
                    }
                    else if (p.q >= nextQ - 1.0e-3 && !firstNext.count(p.ev.strandId))
                    {
                        firstNext[p.ev.strandId] = p.q;
                        glides[p.ev.strandId] = p.ev.artic == VoiceEvent::Articulation::Glide;
                    }
                }
                for (const auto& [strand, on] : strandOn)
                    if (glides[strand] && firstNext[strand] < nextQ + 4.0)
                    {
                        v.push_back({ b, strand, strandNote[strand], on, lastOn, firstNext[strand] });
                        break;
                    }
            }
            return v;
        };
        // Drawn slides (Passing / Approach into a head) end a bar held
        // rarely; half the trials add forced ones (From Step) on half the
        // motif's steps.
        auto withSlides = [&](Motif m) {
            if (U(0, 1))
                for (int s = 0; s < m.numSteps(); ++s)
                    if (m.slots[static_cast<size_t>(s)].fn != Fn::Rest && U(0, 1)) m.slots[static_cast<size_t>(s)].slide = true;
            return m;
        };
        const double dq = kBlock * kBpm / (60.0 * kSr);

        int evoTrials = 0, evoInGap = 0, evoChanged = 0, evoErrs = 0;
        std::string evoWhy;
        for (int t = 0; t < 600; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 2));
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.phraseBars = kPhraseBars[U(0, 2)];
            c.form = static_cast<Form>(U(0, 3));
            c.variation = static_cast<float>(U(0, 5)) / 5.0f;
            c.slide = U(0, 1) ? 1.0f : 0.5f;
            c.shuffle = U(0, 1) ? 0.0f : 0.3f;
            c.layer = U(0, 1) ? Layer::Off : Layer::Octave;
            c.evolve = static_cast<float>(U(0, 10)) / 10.0f;
            c.len = U(1, 2); c.pulses = U(1, 8); c.rotation = U(0, 7);
            const Motif m = withSlides(generateMotif(static_cast<std::uint32_t>(35000 + t), 0, c));
            const int N = phraseBarsOf(c);
            auto run = [&](double turnQ, double& turnedAt) {
                bool loaded = false;
                turnedAt = -1.0;
                return play(c, 0.0, 4.0 * (2 * N + 1), 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                    if (!loaded) { s.setStoredMotif(m); loaded = true; return; }
                    if (turnQ < 0.0 || turnedAt >= 0.0 || q < turnQ) return;
                    Config n = c;
                    n.evolve = c.evolve >= 0.5f ? 0.0f : 1.0f;
                    s.setConfig(n);
                    turnedAt = q;
                });
            };
            double none = -1.0;
            const Run r0 = run(-1.0, none);
            const auto bars = heldBars(r0, 0, 2 * N, [&](int b) { return floorMod(b, N) <= N - 4; });
            if (bars.empty()) continue;
            const Held h = bars[static_cast<size_t>(U(0, static_cast<int>(bars.size()) - 1))];
            const int b = h.bar;
            const double downQ = 4.0 * (b + 1);
            const double turnQ = h.strandOn + (downQ - h.strandOn) * U(5, 95) / 100.0;
            double turnedAt = -1.0;
            const Run r1 = run(turnQ, turnedAt);
            ++evoTrials;
            evoInGap += turnedAt > h.strandOn && turnedAt < downQ;
            evoErrs += r1.hangs + r1.orphanOffs + r1.badGlides + r1.polyMain;
            const double cutQ = 4.0 * (floorDiv(b, N) * N + N - 2);
            if (between(r0, 0.0, cutQ, true) != between(r1, 0.0, cutQ, true) && evoChanged++ < 3)
                evoWhy += " | t " + std::to_string(t) + " part " + std::to_string(static_cast<int>(c.part)) + " bar "
                        + std::to_string(b) + " turned at " + std::to_string(turnedAt);
        }
        check(evoErrs == 0, "Evolve turned: no hanging or orphan note, no bad glide (" + std::to_string(evoErrs) + ")");
        check(evoInGap > 100, "Evolve turned between a held strand's last onset in the bar and the downbeat ("
                                  + std::to_string(evoInGap) + " of " + std::to_string(evoTrials) + ")");
        check(evoChanged == 0, "Evolve turned: every event before the bars that lead into the next phrase is the "
                               "unturned trial's (" + std::to_string(evoChanged) + " trials off)" + evoWhy);

        static const std::vector<int> kChords[] = { { 57, 60, 64 }, { 55, 59, 62 }, { 53, 57, 60 }, { 52, 55, 59 } };
        int keysTrials = 0, keysInGap = 0, keysChanged = 0, keysErrs = 0;
        std::string keysWhy;
        for (int t = 0; t < 1500; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 2));
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.harm = Harm::Keys;
            c.phraseBars = kPhraseBars[U(0, 2)];
            c.form = static_cast<Form>(U(0, 3));
            c.variation = static_cast<float>(U(0, 5)) / 5.0f;
            c.slide = U(0, 1) ? 1.0f : 0.5f;
            c.shuffle = U(0, 1) ? 0.0f : 0.3f;
            c.layer = U(0, 1) ? Layer::Off : Layer::Octave;
            c.len = U(1, 2); c.pulses = U(1, 8); c.rotation = U(0, 7);
            const Motif m = withSlides(generateMotif(static_cast<std::uint32_t>(35600 + t), 0, c));
            const int from = U(0, 3);
            const int to = (from + U(1, 3)) % 4;
            const double untilQ = 4.0 * 10;
            auto run = [&](double pressQ) {
                std::vector<std::pair<double, std::vector<int>>> keys { { 0.0, kChords[from] } };
                if (pressQ >= 0.0) keys.push_back({ pressQ, kChords[to] });
                bool loaded = false;
                return play(c, 0.0, untilQ, 4, 4, keys, 0, [&](T5ynthPhraseSequencer& s, double) {
                    if (!loaded) { s.setStoredMotif(m); loaded = true; }
                });
            };
            const Run r0 = run(-1.0);
            // A beat after the bar's last onset (on any strand: the new chord
            // must not reach an onset in the bar), before the downbeat.
            const auto bars = heldBars(r0, 1, 9, [&](int) { return true; });
            std::vector<Held> fit;
            for (const Held& h : bars)
                if (std::floor(h.lastOn - 4.0 * h.bar) + 1.0 < 4.0) fit.push_back(h);
            if (fit.empty()) continue;
            const Held h = fit[static_cast<size_t>(U(0, static_cast<int>(fit.size()) - 1))];
            const int b = h.bar;
            const double beatQ = 4.0 * b + std::floor(h.lastOn - 4.0 * b) + 1.0;
            const double lo = std::max(h.lastOn, beatQ - 1.0) + 1.0e-3, hi = beatQ - dq - 1.0e-3;
            if (hi <= lo) continue;
            const double pressQ = lo + (hi - lo) * U(0, 100) / 100.0;
            const Run r1 = run(pressQ);
            ++keysTrials;
            const double downQ = 4.0 * (b + 1), endQ = h.glideQ + 1.0e-3;
            keysErrs += r1.hangs + r1.orphanOffs + r1.badGlides + r1.polyMain;
            const bool changedHere = between(r0, 0.0, downQ - 1.0e-3, true) != between(r1, 0.0, downQ - 1.0e-3, true)
                                  || between(r0, downQ - 1.0e-3, endQ, false) != between(r1, downQ - 1.0e-3, endQ, false);
            // The next bar plays the new chord: the change reached it.
            keysInGap += between(r0, downQ - 1.0e-3, endQ, true) != between(r1, downQ - 1.0e-3, endQ, true);
            if (changedHere && keysChanged++ < 3)
                keysWhy += " | t " + std::to_string(t) + " part " + std::to_string(static_cast<int>(c.part)) + " bar "
                         + std::to_string(b) + " beat " + std::to_string(beatQ);
        }
        check(keysErrs == 0, "Keys changed: no hanging or orphan note, no bad glide (" + std::to_string(keysErrs) + ")");
        check(keysInGap > 50, "Keys changed on a beat after a held bar's last onset, the next bar on the new chord ("
                                  + std::to_string(keysInGap) + " of " + std::to_string(keysTrials) + ")");
        check(keysChanged == 0, "Keys changed: what sounds before the downbeat is unchanged, the next bar glides as it did ("
                                    + std::to_string(keysChanged) + " trials off)" + keysWhy);

        // Slide turned to 0 after a held onset, before the downbeat: the
        // re-render keeps a forced slide (From Step), which the next bar's
        // first onset on the strand glides, and drops a drawn one. A dropped
        // slide's note no longer waits for that onset; it ends at the turn.
        // Whether the onset glides is what it does with Slide at 0 from the
        // start (bars render from the motif and the config alone, Evolve 0).
        int slTrials = 0, slKept = 0, slEnded = 0, slOff = 0, slErrs = 0, slMixed = 0;
        std::string slWhy;
        for (int t = 0; t < 1500; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 2));
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.phraseBars = kPhraseBars[U(0, 2)];
            c.form = static_cast<Form>(U(0, 3));
            c.variation = static_cast<float>(U(0, 5)) / 5.0f;
            c.slide = 1.0f;
            c.shuffle = U(0, 1) ? 0.0f : 0.3f;
            c.layer = U(0, 1) ? Layer::Off : Layer::Octave;
            c.len = U(1, 2); c.pulses = U(1, 8); c.rotation = U(0, 7);
            Motif m = generateMotif(static_cast<std::uint32_t>(37100 + t), 0, c);
            // Forced slides on a quarter of the steps: drawn and forced ones
            // meet in one bar.
            for (int k = 0; k < m.numSteps(); ++k)
                if (m.slots[static_cast<size_t>(k)].fn != Fn::Rest && U(0, 3) == 0) m.slots[static_cast<size_t>(k)].slide = true;
            auto run = [&](const Config& from, double turnQ, double& turnedAt) {
                bool loaded = false;
                turnedAt = -1.0;
                return play(from, 0.0, 4.0 * 10, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                    if (!loaded) { s.setStoredMotif(m); loaded = true; return; }
                    if (turnQ < 0.0 || turnedAt >= 0.0 || q < turnQ) return;
                    Config n = from;
                    n.slide = 0.0f;
                    s.setConfig(n);
                    turnedAt = q;
                });
            };
            double none = -1.0;
            const Run r0 = run(c, -1.0, none);
            Config c0 = c;
            c0.slide = 0.0f;
            const Run rz = run(c0, -1.0, none);
            const auto bars = heldBars(r0, 0, 9, [&](int) { return true; });
            if (bars.empty()) continue;
            const Held h = bars[static_cast<size_t>(U(0, static_cast<int>(bars.size()) - 1))];
            const double downQ = 4.0 * (h.bar + 1);
            const double turnQ = h.strandOn + (downQ - h.strandOn) * U(5, 95) / 100.0;
            double turnedAt = -1.0;
            const Run r1 = run(c, turnQ, turnedAt);
            if (!(turnedAt > h.strandOn && turnedAt < downQ)) continue;
            ++slTrials;
            slErrs += r1.hangs + r1.orphanOffs + r1.badGlides + r1.polyMain;
            // The strand's first onset at or after the downbeat, and the held
            // note's first off after its onset.
            double nextOn = -1.0, offQ = -1.0;
            bool glides = false, glidesAt0 = false, heldBefore = false;
            for (const Played& p : rz.events)
                if (p.ev.strandId == h.strand && p.ev.type == VoiceEvent::Type::NoteOn && p.q >= downQ - 1.0e-3)
                {
                    glidesAt0 = p.ev.artic == VoiceEvent::Articulation::Glide;
                    break;
                }
            // A slide earlier in the bar on the strand, kept at 0: the bar's
            // last onset glides from it.
            for (const Played& p : rz.events)
                heldBefore |= p.ev.strandId == h.strand && p.ev.type == VoiceEvent::Type::NoteOn
                           && p.ev.artic == VoiceEvent::Articulation::Glide
                           && p.q > 4.0 * h.bar + 1.0e-3 && p.q < downQ - 1.0e-3;
            slMixed += heldBefore && !glidesAt0;
            for (const Played& p : r1.events)
            {
                if (p.ev.strandId != h.strand) continue;
                if (p.ev.type == VoiceEvent::Type::NoteOn && p.q >= downQ - 1.0e-3 && nextOn < 0.0)
                {
                    nextOn = p.q;
                    glides = p.ev.artic == VoiceEvent::Articulation::Glide;
                }
                if (p.ev.type == VoiceEvent::Type::NoteOff && p.ev.note == h.note && p.q > h.strandOn + 1.0e-3 && offQ < 0.0)
                    offQ = p.q;
            }
            bool ok = glides == glidesAt0;
            if (glidesAt0) { ++slKept; ok = ok && (offQ < 0.0 || offQ > nextOn - 1.0e-3); }
            else           { ++slEnded; ok = ok && offQ >= 0.0 && std::abs(offQ - turnedAt) < 1.0e-3; }
            if (!ok && slOff++ < 3)
                slWhy += " | t " + std::to_string(t) + " part " + std::to_string(static_cast<int>(c.part)) + " bar "
                       + std::to_string(h.bar) + (glides ? " kept" : " dropped") + " turned at "
                       + std::to_string(turnedAt) + " off at " + std::to_string(offQ);
        }
        check(slErrs == 0, "Slide turned to 0: no hanging or orphan note, no bad glide (" + std::to_string(slErrs) + ")");
        check(slKept > 50 && slEnded > 50 && slMixed > 10,
              "Slide turned to 0 after a held onset: forced slides kept (" + std::to_string(slKept)
                  + "), drawn slides dropped (" + std::to_string(slEnded) + ", " + std::to_string(slMixed)
                  + " after a kept slide in the bar)");
        check(slOff == 0, "Slide turned to 0: a kept slide's note sounds into its glide, a dropped one's ends at the turn ("
                              + std::to_string(slOff) + " of " + std::to_string(slTrials) + " off)" + slWhy);

        static const Layer kLayers[] = { Layer::Off, Layer::Octave, Layer::Chord, Layer::Answer };
        auto turnRun = [&](const Motif& m, const Config& from, const Config& to, double turnQ, double& turnedAt) {
            bool loaded = false;
            turnedAt = -1.0;
            return play(from, 0.0, 4.0 * 10, 4, 4, {}, 0, [&](T5ynthPhraseSequencer& s, double q) {
                if (!loaded) { s.setStoredMotif(m); loaded = true; return; }
                if (turnQ < 0.0 || turnedAt >= 0.0 || q < turnQ) return;
                s.setConfig(to);
                turnedAt = q;
            });
        };
        auto strandKeys = [&](const Run& r, int strand) {
            std::vector<Key> v;
            for (const Played& p : r.events)
                if (p.ev.strandId == strand) v.push_back(keyOf(p, true));
            return v;
        };
        // An onset within a block of the turn: whether it plays before the
        // re-render or from it is the block grid's, not the rule's.
        auto nearTurn = [&](const Run& r, double turnedAt) {
            for (const Played& p : r.events)
                if (p.ev.type == VoiceEvent::Type::NoteOn && std::abs(p.q - turnedAt) < dq + 1.0e-3) return true;
            return false;
        };
        auto randomConfig = [&]() {
            Config c = base();
            c.part = static_cast<Part>(U(0, 2));
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.phraseBars = kPhraseBars[U(0, 2)];
            c.form = static_cast<Form>(U(0, 3));
            c.variation = static_cast<float>(U(0, 5)) / 5.0f;
            c.slide = U(0, 1) ? 1.0f : 0.5f;
            c.shuffle = U(0, 1) ? 0.0f : 0.3f;
            c.len = U(1, 2); c.pulses = U(1, 8); c.rotation = U(0, 7);
            return c;
        };

        // The layer switched after the bar's last onset, before the downbeat.
        // The line does not depend on the layer: every event of it is the
        // unswitched trial's, its slide into the downbeat kept whatever the
        // new layer puts beside it in the bar — an Answer onset after the
        // turn, a Chord hit on the held onset. The Octave layer's held copy
        // waits for its own strand's next onset; where the new layer leaves
        // none in the bar (Off, Chord, a bar without an Answer onset) it ends
        // at the turn, not at the downbeat nor in a glide.
        int lyTrials = 0, lyAfter = 0, lyBeside = 0, lyReleased = 0, lyOff = 0, lyErrs = 0;
        std::string lyWhy;
        for (int t = 0; t < 3000; ++t)
        {
            Config c = randomConfig();
            const int fromLayer = U(0, 3), toLayer = (fromLayer + U(1, 3)) % 4;
            c.layer = kLayers[fromLayer];
            // The Answer plays the motif's rhythm, without the line's
            // variation: only a Drop (Variation at full, a B unit's last bar)
            // leaves it onsets after the line's last.
            // Half the switches to the Answer aim at that bar: Variation at
            // full, a bar where the Answer's last onset follows the line's,
            // the turn between them.
            const bool aim = kLayers[toLayer] == Layer::Answer && U(0, 1);
            if (aim) c.variation = 1.0f;
            Config to = c;
            to.layer = kLayers[toLayer];
            const Motif m = withSlides(generateMotif(static_cast<std::uint32_t>(38600 + t), 0, c));
            double none = -1.0;
            const Run r0 = turnRun(m, c, c, -1.0, none);
            const Run rt = turnRun(m, to, to, -1.0, none);   // the new layer from the start
            auto lastOn = [&](const Run& r, int b, bool line) {
                double q = -1.0;
                for (const Played& p : r.events)
                    if (p.ev.type == VoiceEvent::Type::NoteOn && (p.ev.strandId == -1) == line
                        && p.q > 4.0 * b - 1.0e-3 && p.q < 4.0 * (b + 1) - 1.0e-3)
                        q = p.q;
                return q;
            };
            // The Answer sounds only in B units: a switch to it elsewhere
            // only switches the Octave copy off.
            const auto bars = heldBars(r0, 0, 9, [&](int b) {
                if (to.layer != Layer::Answer) return true;
                return isBUnit(to, b) && (!aim || lastOn(rt, b, false) > lastOn(r0, b, true) + 4.0 * dq);
            });
            if (bars.empty()) continue;
            const Held h = bars[static_cast<size_t>(U(0, static_cast<int>(bars.size()) - 1))];
            if (h.strand != -1) continue;
            const double downQ = 4.0 * (h.bar + 1);
            const double turnEnd = aim ? lastOn(rt, h.bar, false) : downQ;
            const double turnQ = h.strandOn + (turnEnd - h.strandOn) * U(5, 95) / 100.0;
            double turnedAt = -1.0;
            const Run r1 = turnRun(m, c, to, turnQ, turnedAt);
            if (!(turnedAt > h.strandOn && turnedAt < downQ) || nearTurn(rt, turnedAt)) continue;
            ++lyTrials;
            lyErrs += r1.hangs + r1.orphanOffs + r1.badGlides + r1.polyMain;
            bool after = false, after5 = false, beside = false;
            for (const Played& p : rt.events)
            {
                if (p.ev.type != VoiceEvent::Type::NoteOn || p.ev.strandId == -1) continue;
                if (p.q > turnedAt && p.q < downQ - 1.0e-3) { after = true; after5 |= p.ev.strandId == 5; }
                beside |= p.q > h.strandOn - 1.0e-3 && p.q < turnedAt;
            }
            lyAfter += after;
            lyBeside += beside && to.layer != Layer::Octave;
            bool ok = strandKeys(r0, -1) == strandKeys(r1, -1);
            std::string what = ok ? "" : " line changed";
            if (c.layer == Layer::Octave && !after5)
            {
                int copy = -1;
                for (const Played& p : r0.events)
                    if (p.ev.strandId == 5 && p.ev.type == VoiceEvent::Type::NoteOn && std::abs(p.q - h.strandOn) < 1.0e-3)
                        copy = p.ev.note;
                double offQ = -1.0;
                for (const Played& p : r1.events)
                    if (p.ev.strandId == 5 && p.ev.type == VoiceEvent::Type::NoteOff && p.ev.note == copy
                        && p.q > h.strandOn + 1.0e-3)
                    {
                        offQ = p.q;
                        break;
                    }
                ++lyReleased;
                if (!(copy >= 0 && std::abs(offQ - turnedAt) < 1.0e-3))
                {
                    ok = false;
                    what += " Octave copy off at " + std::to_string(offQ);
                }
            }
            if (!ok && lyOff++ < 3)
                lyWhy += " | t " + std::to_string(t) + " layer " + std::to_string(static_cast<int>(c.layer)) + ">"
                       + std::to_string(static_cast<int>(to.layer)) + " bar " + std::to_string(h.bar) + " turned at "
                       + std::to_string(turnedAt) + what;
        }
        check(lyErrs == 0, "Layer switched: no hanging or orphan note, no bad glide (" + std::to_string(lyErrs) + ")");
        check(lyAfter > 20 && lyBeside > 150 && lyReleased > 100,
              "Layer switched after a held bar's last onset: the new layer has an onset after the turn ("
                  + std::to_string(lyAfter) + "), one from the held onset to the turn (" + std::to_string(lyBeside)
                  + "), the Octave copy has none left on its strand (" + std::to_string(lyReleased) + ")");
        check(lyOff == 0, "Layer switched: the line plays as unswitched, its slide into the downbeat kept; the Octave copy "
                          "with no onset left on its strand ends at the turn ("
                              + std::to_string(lyOff) + " of " + std::to_string(lyTrials) + " off)" + lyWhy);

        // A Part or the rhythm switched after the bar's last onset, before
        // the downbeat: the bar re-renders as the new line, which plays as a
        // run with the new settings from the start plays it (§3.9). The held
        // note waits for that line's next onset when the line has one left in
        // the bar, or when its last onset before the turn is held into the
        // next bar; otherwise it ends at the turn. Stab's chord continues no
        // voice: the held note ends at its onset. Any layer beside the line.
        int swTrials = 0, swIn = 0, swAcross = 0, swEnded = 0, swStab = 0, swOff = 0, swErrs = 0;
        std::string swWhy;
        for (int t = 0; t < 1500; ++t)
        {
            Config c = randomConfig();
            c.layer = kLayers[U(0, 3)];
            Config to = c;
            const int change = U(0, 5);
            if (change == 0)
            {
                // A Part alone keeps the rhythm: Stab's chords fall on the
                // line's onsets, none after its last. With a rotation, some do.
                to.part = static_cast<Part>((static_cast<int>(c.part) + U(1, 3)) % 4);
                if (to.part == Part::Stab || U(0, 1)) to.rotation = (c.rotation + U(1, 7)) % 8;
            }
            else if (change == 1) to.pulses = (c.pulses - 1 + U(1, 7)) % 8 + 1;
            else if (change == 2) to.rotation = (c.rotation + U(1, 7)) % 8;
            else if (change == 3) to.shuffle = c.shuffle > 0.0f ? 0.0f : 0.3f;
            else if (change == 4) to.len = 3 - c.len;
            else                  to.stepsPerBar = c.stepsPerBar == 16 ? 8 : 16;
            const Motif m = withSlides(generateMotif(static_cast<std::uint32_t>(40100 + t), 0, c));
            double none = -1.0;
            const Run r0 = turnRun(m, c, c, -1.0, none);
            const auto bars = heldBars(r0, 0, 9, [&](int) { return true; });
            if (bars.empty()) continue;
            const Held h = bars[static_cast<size_t>(U(0, static_cast<int>(bars.size()) - 1))];
            if (h.strand != -1) continue;
            const double downQ = 4.0 * (h.bar + 1);
            const double turnQ = h.strandOn + (downQ - h.strandOn) * U(5, 95) / 100.0;
            double turnedAt = -1.0;
            const Run r1 = turnRun(m, c, to, turnQ, turnedAt);
            const Run rf = turnRun(m, to, to, -1.0, none);   // the new settings from the start
            if (!(turnedAt > h.strandOn && turnedAt < downQ) || nearTurn(rf, turnedAt)) continue;
            ++swTrials;
            // Stab's chord is the main strand's: its voices sound together.
            swErrs += r1.hangs + r1.orphanOffs + r1.badGlides + (to.part == Part::Stab ? 0 : r1.polyMain);
            // The new line: its last onset in the bar before the turn, its
            // next onset from the turn on and whether that glides.
            double fLast = -1.0, fNext = -1.0;
            bool fNextGlides = false;
            for (const Played& p : rf.events)
            {
                if (p.ev.strandId != -1 || p.ev.type != VoiceEvent::Type::NoteOn) continue;
                if (p.q < turnedAt) { if (p.q > 4.0 * h.bar - 1.0e-3) fLast = p.q; }
                else if (fNext < 0.0)
                {
                    fNext = p.q;
                    fNextGlides = p.ev.artic == VoiceEvent::Articulation::Glide;
                }
            }
            const bool inBar = fNext >= 0.0 && fNext < downQ - 1.0e-3;
            const bool keep = inBar || (fLast >= 0.0 && fNextGlides);
            double rNext = -1.0, offQ = -1.0;
            bool rNextGlides = false;
            for (const Played& p : r1.events)
            {
                if (p.ev.strandId != -1) continue;
                if (p.ev.type == VoiceEvent::Type::NoteOn && p.q > turnedAt && rNext < 0.0)
                {
                    rNext = p.q;
                    rNextGlides = p.ev.artic == VoiceEvent::Articulation::Glide;
                }
                if (p.ev.type == VoiceEvent::Type::NoteOff && p.ev.note == h.note && p.q > h.strandOn + 1.0e-3 && offQ < 0.0)
                    offQ = p.q;
            }
            bool ok = (rNext < 0.0 && fNext < 0.0) || std::abs(rNext - fNext) < 1.0e-3;
            const char* expect = "";
            if (keep && to.part != Part::Stab)
            {
                ++(inBar ? swIn : swAcross);
                expect = inBar ? " glides into the bar's next onset" : " glides into the next bar";
                ok = ok && rNextGlides && (offQ < 0.0 || offQ > rNext - 1.0e-3);
            }
            else if (keep)
            {
                ++swStab;
                expect = " ends at the chord";
                ok = ok && !rNextGlides && std::abs(offQ - rNext) < 1.0e-3;
            }
            else
            {
                ++swEnded;
                expect = " ends at the turn";
                ok = ok && !rNextGlides && std::abs(offQ - turnedAt) < 1.0e-3;
            }
            if (!ok && swOff++ < 3)
                swWhy += " | t " + std::to_string(t) + " change " + std::to_string(change) + " bar " + std::to_string(h.bar)
                       + " turned at " + std::to_string(turnedAt) + expect + "; next on " + std::to_string(rNext)
                       + (rNextGlides ? " glides" : "") + ", off " + std::to_string(offQ);
        }
        check(swErrs == 0, "Part or rhythm switched: no hanging or orphan note, no bad glide (" + std::to_string(swErrs) + ")");
        check(swIn > 30 && swAcross > 90 && swEnded > 120 && swStab > 7,
              "Part or rhythm switched after a held bar's last onset: the held note continues into the new line's "
              "onset in the bar (" + std::to_string(swIn) + "), into the next bar (" + std::to_string(swAcross)
                  + "), ends at the turn (" + std::to_string(swEnded) + "), at Stab's chord (" + std::to_string(swStab) + ")");
        check(swOff == 0, "Part or rhythm switched: the held note continues where the new line continues it, else ends at "
                          "the turn (" + std::to_string(swOff) + " of " + std::to_string(swTrials) + " off)" + swWhy);
    }

    std::printf("\n== 36. The pulse Bass's Lift turns into an Octave is no head\n");
    {
        // Under Lift Bass plays an Octave of the note before it on the last
        // pulse of the last beat, in place of what is there (§3.7): an
        // embellishment, no head. The pulse kept the motif step's head and
        // function: a drawn slide (a Passing or Approach step into a head)
        // glided into the octave, a Passing or Approach pulse drew one out of
        // it, the Chord layer hit it, the view showed a head, and Ratchet
        // weighed it as the last beat's head. Oracle from the motif (heads
        // from computeHeads, a forced slide on a random quarter of the steps,
        // Slide 1 so every Passing or Approach step into a head draws one):
        // the pulse glides only from a forced slide on the onset before it,
        // in its bar or from the bar before; it is held only by a forced
        // slide of its own; no Chord-layer voice starts on it; the view shows
        // an Octave and no head; the ratchet goes on the last beat's
        // strongest other head, else its strongest pulse. A forced slide
        // still glides: from the step onset before the pulse into it (in its
        // bar or from the bar before), and from the pulse into the next step
        // onset; and at Variation 4/5 every such bar ratchets. The last bar of a
        // phrase or of an ABAB B unit, Variation 2/5 to 4/5 (Lift, Fill,
        // Ratchet; 4/5 weighted), 1/16 and 1/8, bars of 1 to 4 beats; in a
        // quarter of the trials the bar rests until its last beat, so the
        // bar before leads into the pulse.
        std::mt19937 rng(36);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        const float kTailVel = 85.0f / 127.0f * 0.9f, kFillVel = 72.0f / 127.0f;
        auto isDrawable = [](Fn f) {
            return f == Fn::Passing || f == Fn::ApproachUp || f == Fn::ApproachDown || f == Fn::ApproachChromatic;
        };
        int pulses = 0, pulseHeads = 0, viewOff = 0, intoOff = 0, fromOff = 0, chordOff = 0, ratchetOff = 0;
        int drawnInto = 0, forcedInto = 0, drawnFrom = 0, chordOnHead = 0, ratchets = 0, ratchetMoved = 0;
        int crossDrawn = 0, crossOff = 0, forcedIn = 0, forcedCross = 0, forcedOut = 0, forcedOff = 0;
        int ratchetWanted = 0, ratchetMissing = 0;
        std::string why;
        auto fail = [&](int& count, int t, const std::string& what) {
            if (count++ < 3) why += " | t " + std::to_string(t) + " " + what;
        };
        for (int t = 0; t < 8000; ++t)
        {
            Config c = base();
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.stepsPerBar = U(0, 1) ? 16 : 8;
            const bool restsIn = U(0, 3) == 0;
            c.len = restsIn ? 2 : U(1, 2); c.pulses = U(2, 14); c.rotation = U(0, 7);
            c.phraseBars = 4 << U(0, 2);
            c.form = U(0, 1) ? Form::ABAB : static_cast<Form>(U(0, 2));
            c.variation = U(0, 1) ? 0.8f : 0.2f * static_cast<float>(U(2, 3));
            c.slide = 1.0f; c.shuffle = 0.0f;
            c.layer = U(0, 1) ? Layer::Chord : Layer::Off;
            static const double kBarLen[] = { 1.0, 2.0, 3.0, 4.0, 4.0, 4.0 };
            const double barLenQ = kBarLen[U(0, 5)];
            Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(36000 + t), 0, c), c);
            for (int s = 0; s < m.numSteps(); ++s)
                m.slots[static_cast<size_t>(s)].slide = U(0, 3) == 0;
            const int N = phraseBarsOf(c), unitBars = N / 4;
            const int bar = c.form == Form::ABAB && U(0, 1) ? 2 * unitBars - 1 : N - 1;
            if (bar < 1) continue;
            if (restsIn)   // the lifted bar is the motif's second: it rests until its last beat
                for (int k = 0; k * 4.0 / m.stepsPerBar < barLenQ - 1.0 - 1.0e-9; ++k)
                    m.slots[static_cast<size_t>(m.stepsPerBar + k)] = Slot {};
            auto render = [&](int b, BarRender& r) {
                RenderInput in;
                in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m;
                in.bar = b; in.barLenQ = barLenQ;
                in.prevNote = 33 + (t % 24); in.prevPrevNote = 33 + ((t * 7) % 24);
                renderBar(in, r);
            };
            BarRender r, before, after;
            render(bar, r);
            render(bar - 1, before);
            render(bar + 1, after);

            std::array<bool, kMaxMotifSteps> heads {};
            computeHeads(m, heads);
            const int spb = m.stepsPerBar;
            const double stepQ = 4.0 / spb, lastBeatQ = std::max(0.0, barLenQ - 1.0);
            auto slotAt = [&](int b, int k) -> const Slot& {
                return m.slots[static_cast<size_t>(floorMod(b, m.len) * spb + k)];
            };
            auto headAt = [&](int b, int k) { return heads[static_cast<size_t>(floorMod(b, m.len) * spb + k)]; };
            auto inLast = [&](int k) { return k * stepQ >= lastBeatQ - 1.0e-9; };
            int pulse = -1;
            for (int k = 0; k < r.viewSteps; ++k)
                if (slotAt(bar, k).fn != Fn::Rest && inLast(k)) pulse = k;
            if (pulse < 0) continue;
            ++pulses;
            const bool wasHead = headAt(bar, pulse);
            pulseHeads += wasHead;
            const std::string at = "bar " + std::to_string(bar) + " step " + std::to_string(pulse);

            const StepView& v = r.view[static_cast<size_t>(pulse)];
            if (v.head || v.fn != Fn::Octave) fail(viewOff, t, "view: " + at);

            // The main line's events in order, and the pulse's onset among them.
            auto mainOf = [](const BarRender& b) {
                std::vector<const Event*> l;
                for (int e = 0; e < b.count; ++e)
                    if (b.events[static_cast<size_t>(e)].strand == -1) l.push_back(&b.events[static_cast<size_t>(e)]);
                std::stable_sort(l.begin(), l.end(), [](const Event* x, const Event* y) { return x->onQ < y->onQ; });
                return l;
            };
            const std::vector<const Event*> main = mainOf(r);
            int i0 = -1;
            for (int i = 0; i < static_cast<int>(main.size()) && i0 < 0; ++i)
                if (std::abs(main[static_cast<size_t>(i)]->onQ - pulse * stepQ) < 1.0e-9) i0 = i;
            if (i0 < 0) { fail(intoOff, t, "no onset: " + at); continue; }
            const Event& e0 = *main[static_cast<size_t>(i0)];
            // The step an event is the onset of: on its grid, no Fill note,
            // no ratchet repeat (−1 otherwise).
            auto stepOf = [&](const Event& e) {
                const int k = static_cast<int>(std::lround(e.onQ / stepQ));
                const bool grid = std::abs(k * stepQ - e.onQ) < 1.0e-9 && std::abs(e.vel - kFillVel) > 1.0e-4
                               && std::abs(e.vel - kTailVel) > 1.0e-4;
                return grid ? k : -1;
            };

            if (i0 > 0)
            {
                const Event& ep = *main[static_cast<size_t>(i0 - 1)];
                const int kp = stepOf(ep);
                const bool forced = kp >= 0 && slotAt(bar, kp).fn != Fn::Rest && slotAt(bar, kp).slide;
                if (e0.glide && !forced) fail(intoOff, t, "glides into the pulse: " + at);
                if (forced && !e0.glide) fail(forcedOff, t, "a forced slide does not glide into the pulse: " + at);
                forcedIn += forced;
                forcedInto += e0.glide;
                if (kp >= 0 && !forced && wasHead && isDrawable(r.view[static_cast<size_t>(kp)].fn)) ++drawnInto;
            }
            else
            {
                // The pulse is the bar's first onset: the bar before leads into it.
                const std::vector<const Event*> prev = mainOf(before);
                if (!prev.empty())
                {
                    const Event& last = *prev.back();
                    const int kp = stepOf(last);
                    const bool forced = kp >= 0 && slotAt(bar - 1, kp).fn != Fn::Rest && slotAt(bar - 1, kp).slide;
                    if (last.held && !forced) fail(crossOff, t, "the bar before glides into the pulse: " + at);
                    if (forced && !last.held) fail(forcedOff, t, "a forced slide from the bar before is not held: " + at);
                    forcedCross += forced;
                    if (kp >= 0 && !forced && wasHead && isDrawable(before.view[static_cast<size_t>(kp)].fn)) ++crossDrawn;
                }
            }

            // Out of the pulse: only a forced slide of its own holds it, and
            // it does into the next step onset (the bar's next onset can only
            // be a Fill note or a ratchet repeat, the next bar's first is a
            // step's).
            if (e0.held && !slotAt(bar, pulse).slide) fail(fromOff, t, "held out of the pulse: " + at);
            if (slotAt(bar, pulse).slide)
            {
                bool nextIsStep = false;
                if (i0 + 1 < static_cast<int>(main.size()))
                    nextIsStep = stepOf(*main[static_cast<size_t>(i0 + 1)]) >= 0;
                else if (const std::vector<const Event*> nxt = mainOf(after); !nxt.empty())
                    nextIsStep = stepOf(*nxt.front()) >= 0;
                const bool ratcheted = i0 + 1 < static_cast<int>(main.size())
                                    && std::abs(main[static_cast<size_t>(i0 + 1)]->vel - kTailVel) < 1.0e-4;
                if (nextIsStep && !ratcheted)
                {
                    ++forcedOut;
                    if (!e0.held) fail(forcedOff, t, "a forced slide does not hold the pulse: " + at);
                }
            }
            if (isDrawable(slotAt(bar, pulse).fn) && !slotAt(bar, pulse).slide && i0 + 1 == static_cast<int>(main.size()))
            {
                // The pulse ends the bar: does the next bar start on a head?
                for (int k = 0; k < after.viewSteps; ++k)
                    if (after.view[static_cast<size_t>(k)].note >= 0)
                    {
                        drawnFrom += k == 0 && after.view[static_cast<size_t>(k)].head;
                        break;
                    }
            }

            if (c.layer == Layer::Chord)
            {
                chordOnHead += wasHead;
                for (int e = 0; e < r.count; ++e)
                {
                    const Event& ev = r.events[static_cast<size_t>(e)];
                    if (ev.strand >= 5 && std::abs(ev.onQ - pulse * stepQ) < 1.0e-9)
                    {
                        fail(chordOff, t, "Chord layer on the pulse: " + at);
                        break;
                    }
                }
            }

            // Ratchet: the step whose slot holds the repeats.
            int ratchetK = -1;
            for (const Event* e : main)
                if (std::abs(e->vel - kTailVel) < 1.0e-4) ratchetK = static_cast<int>(std::floor(e->onQ / stepQ + 1.0e-9));
            if (std::lround(c.variation * 5.0f) >= 4)
            {
                ++ratchetWanted;
                if (ratchetK < 0) fail(ratchetMissing, t, "no ratchet: " + at);
            }
            if (ratchetK >= 0)
            {
                ++ratchets;
                int want = -1, wantW = -1, old = -1, oldW = -1;
                for (int k = 0; k < r.viewSteps; ++k)
                {
                    if (slotAt(bar, k).fn == Fn::Rest || !inLast(k)) continue;
                    const int metric = metricWeight16(k * (16 / spb));
                    const int w = (headAt(bar, k) && k != pulse ? 16 : 0) + metric;
                    const int wOld = (headAt(bar, k) ? 16 : 0) + metric;
                    if (w > wantW) { wantW = w; want = k; }
                    if (wOld > oldW) { oldW = wOld; old = k; }
                }
                ratchetMoved += want != old;
                if (ratchetK != want)
                    fail(ratchetOff, t, "ratchet on step " + std::to_string(ratchetK) + ", want " + std::to_string(want)
                                            + ": " + at);
            }
        }
        check(pulses > 4000 && pulseHeads > 2000, "lifted Bass bars (" + std::to_string(pulses) + ", "
                                                    + std::to_string(pulseHeads) + " with a head on the pulse)");
        check(drawnInto > 40 && forcedInto > 200 && crossDrawn > 10 && drawnFrom > 80 && chordOnHead > 800
                  && ratchetMoved > 20,
              "cases: Passing / Approach into a head pulse (" + std::to_string(drawnInto) + ", "
                  + std::to_string(crossDrawn) + " from the bar before), forced slides into the pulse ("
                  + std::to_string(forcedInto) + "), Passing / Approach pulses before a head (" + std::to_string(drawnFrom)
                  + "), Chord layer on a head pulse (" + std::to_string(chordOnHead) + "), ratchets ("
                  + std::to_string(ratchets) + ", " + std::to_string(ratchetMoved) + " away from a head pulse)");
        check(forcedIn > 100 && forcedCross > 20 && forcedOut > 200 && ratchetWanted > 3000 && forcedOff + ratchetMissing == 0,
              "a forced slide still glides: into the pulse (" + std::to_string(forcedIn) + " in its bar, "
                  + std::to_string(forcedCross) + " from the bar before), out of it (" + std::to_string(forcedOut) + "); "
                  + std::to_string(forcedOff) + " off; Variation 4/5 ratchets (" + std::to_string(ratchetMissing) + " of "
                  + std::to_string(ratchetWanted) + " bars without)" + why);
        check(viewOff + intoOff + crossOff + fromOff + chordOff + ratchetOff == 0,
              "the pulse is an Octave and no head: view (" + std::to_string(viewOff) + "), no drawn slide into it ("
                  + std::to_string(intoOff) + ", " + std::to_string(crossOff) + " from the bar before) or out of it ("
                  + std::to_string(fromOff) + "), no Chord-layer hit (" + std::to_string(chordOff)
                  + "), ratchet on the strongest other head (" + std::to_string(ratchetOff) + ")" + why);
    }

    std::printf("\n== 37. Under Keys a bar enters from the bar before rendered on its own\n");
    {
        // Keys carried the line on live from bar to bar: a bar entered with
        // the last notes the bar before had played. Where no head sounds in a
        // bar (a bar of one beat, a sparse motif), nothing pulls the line
        // back: a Passing step goes on up from its P, a Neighbour drifts, and
        // the line climbed bar after bar until it stuck at the top of the
        // MIDI range. A locate entered with no notes at all and played other
        // notes than playing there did. Pedal and Chords enter from bar − 1
        // rendered on its own from the chord root (§3.9); Keys now does the
        // same with the chord bar − 1 ended on.
        //  - Oracle 1: keys held on one chord all along play what Chords with
        //    that chord as its only slot plays, note for note.
        //  - Oracle 2: a chord latched in a bar's last beat takes effect at the
        //    next downbeat; that bar plays as renderBar renders it with the
        //    new chord, entered from the bar before rendered on its own with
        //    the chord it ended on (entryNotes), not with the new one. A chord
        //    taking effect inside the bar before is the one it ended on, not
        //    the one it started on.
        //  - Oracle 3: a locate under Keys, with chords changing on the way,
        //    plays the line playing reaches there, when the bar before ended
        //    on the chord held at the locate. The Chord layer and Stab voice
        //    the chord led from the voicing before, live (it stays near its
        //    anchor): they are not compared.
        std::mt19937 rng(37);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        static const int kMeters[][2] = { { 1, 4 }, { 2, 4 }, { 1, 8 }, { 3, 8 }, { 3, 4 }, { 4, 4 }, { 5, 8 }, { 7, 8 }, { 5, 16 } };
        auto triadOf = [](const Chord& ch) {
            std::vector<int> v { 48 + ch.rootPc };
            for (int i = 1; i < 3; ++i) if (ch.iv[static_cast<size_t>(i)] >= 0) v.push_back(48 + ch.rootPc + ch.iv[static_cast<size_t>(i)]);
            return v;
        };
        auto mainTop = [](const Run& r) {
            int top = -1;
            for (const auto& p : r.events)
                if (p.ev.type == VoiceEvent::Type::NoteOn && p.ev.strandId == -1) top = std::max(top, p.ev.note);
            return top;
        };
        int trials = 0, sameOff = 0, higher = 0, short1 = 0, locates = 0, locOff = 0, changesIn = 0, errs = 0;
        int topKeys = 0, topChords = 0, entries = 0, entriesInside = 0, entryOff = 0, entryMatters = 0;
        std::string why, locWhy, entryWhy;
        auto chordOf = [](const std::vector<int>& v) {
            std::uint16_t mask = 0;
            for (int n : v) mask = static_cast<std::uint16_t>(mask | (1u << (n % 12)));
            return chordFromKeys(v[0], mask);
        };
        auto same = [](const std::vector<Played>& a, const std::vector<Played>& b, std::string& w) {
            return (a.empty() && b.empty()) || sameOns(a, b, w);
        };
        for (int t = 0; t < 240; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 3));
            c.scaleRoot = U(0, 11);
            c.stepsPerBar = 4 << U(0, 2);
            c.len = U(1, 2); c.pulses = U(1, c.stepsPerBar); c.rotation = U(0, c.stepsPerBar - 1);
            c.phraseBars = 4 << U(0, 2);
            c.form = static_cast<Form>(U(0, 3));
            c.variation = 0.25f * static_cast<float>(U(0, 4));
            c.evolve = U(0, 1) ? 0.0f : 0.5f;
            c.slide = 0.5f * static_cast<float>(U(0, 2));
            c.shuffle = U(0, 1) ? 0.0f : 0.4f;
            c.layer = static_cast<Layer>(U(0, 3));
            const int mi = U(0, 8);
            const int tsNum = kMeters[mi][0], tsDen = kMeters[mi][1];
            const double barLenQ = 4.0 * tsNum / tsDen, beatQ = 4.0 / tsDen;
            short1 += barLenQ <= 1.0;
            const std::uint32_t seed = static_cast<std::uint32_t>(37000 + t);
            ++trials;

            // Oracle 1
            const int deg = U(1, 7);
            Config k = c; k.harm = Harm::Keys;
            Config d = c; d.harm = Harm::Chords; d.chordDegrees = { deg, 0, 0, 0 };
            const std::vector<int> held = triadOf(chordForDegree(deg - 1, c.scaleType, c.scaleRoot, c.ext7));
            const int bars = 48;
            const Run rk = play(k, 0.0, bars * barLenQ, tsNum, tsDen, { { 0.0, held } }, seed);
            const Run rd = play(d, 0.0, bars * barLenQ, tsNum, tsDen, {}, seed);
            errs += rk.hangs + rk.orphanOffs + rk.badGlides + rk.sounding;
            std::string w;
            if (!same(onsInBars(rk, barLenQ, 0, bars), onsInBars(rd, barLenQ, 0, bars), w) && sameOff++ < 3)
                why += " | t " + std::to_string(t) + " " + w;
            const int tk = mainTop(rk), td = mainTop(rd);
            higher += tk > td;
            topKeys = std::max(topKeys, tk);
            topChords = std::max(topChords, td);

            if (c.part == Part::Stab) continue;

            // Oracle 2. Evolve 0 and no layer: every phrase plays the motif
            // as diced, and the bar's events are the main line's. The new
            // chord is another degree than the one held.
            for (int rep = 0; rep < 4; ++rep)
            {
                const int b = U(2, 12);
                const double bQ = b * barLenQ;
                const bool inside = rep == 3 && tsNum >= 2;   // effective on bar b − 1's second beat
                Config k2 = c; k2.harm = Harm::Keys; k2.evolve = 0.0f; k2.layer = Layer::Off;
                const std::vector<int> next = triadOf(chordForDegree((deg - 1 + U(1, 6)) % 7, c.scaleType, c.scaleRoot, c.ext7));
                const double tc = inside ? bQ - barLenQ + 0.5 * beatQ : bQ - 0.5 * beatQ;
                const Run r2 = play(k2, 0.0, (b + 1) * barLenQ, tsNum, tsDen, { { 0.0, held }, { tc, next } }, seed);
                errs += r2.hangs + r2.orphanOffs + r2.badGlides + r2.sounding;
                const Motif fm = fitMotif(generateMotif(seed, 0, k2), k2);
                auto expect = [&](const Chord& before) {
                    BarRender sc2, out2;
                    int p = -1, pp = -1;
                    entryNotes(k2, fm, fm, b, barLenQ, &before, sc2, p, pp);
                    RenderInput in;
                    in.cfg = &k2; in.motif = &fm; in.nextPhraseMotif = &fm; in.bar = b; in.barLenQ = barLenQ;
                    in.keys = true; in.keysChord = chordOf(next); in.prevNote = p; in.prevPrevNote = pp;
                    renderBar(in, out2);
                    std::vector<std::pair<double, int>> v;
                    for (int e = 0; e < out2.count; ++e)
                        v.push_back({ bQ + out2.events[static_cast<size_t>(e)].onQ, out2.events[static_cast<size_t>(e)].note });
                    return v;
                };
                const auto want = expect(chordOf(inside ? next : held)), fromNew = expect(chordOf(inside ? held : next));
                std::vector<std::pair<double, int>> got;
                for (const Played& p : onsInBars(r2, barLenQ, b, b + 1)) got.push_back({ p.q, p.ev.note });
                bool ok = got.size() == want.size();
                for (size_t e = 0; ok && e < got.size(); ++e)
                    ok = std::abs(got[e].first - want[e].first) < 1.5 * kOneSampleQ && got[e].second == want[e].second;
                ++entries;
                entriesInside += inside;
                bool matters = want.size() != fromNew.size();
                for (size_t e = 0; !matters && e < want.size(); ++e) matters = want[e].second != fromNew[e].second;
                entryMatters += matters;
                if (!ok && entryOff++ < 3)
                {
                    entryWhy += " | t " + std::to_string(t) + " bar " + std::to_string(b) + ":";
                    for (const auto& g : got) entryWhy += " " + std::to_string(g.second);
                    entryWhy += " want";
                    for (const auto& g : want) entryWhy += " " + std::to_string(g.second);
                }
            }

            // Oracle 3: chords change mid-beat, so both runs latch them in the
            // same beat whatever their block grids; one takes effect at the
            // beat after it.
            std::vector<std::pair<double, std::vector<int>>> keys { { 0.0, held } };
            const int locBar = U(2, 24);
            for (int n = U(1, 3); n > 0; --n)
            {
                const int beats = static_cast<int>(std::llround(locBar * barLenQ / beatQ)) + 4;
                keys.push_back({ (U(0, beats) + 0.5) * beatQ,
                                 triadOf(chordForDegree(U(0, 6), c.scaleType, c.scaleRoot, c.ext7)) });
            }
            // And one in the located bar's first beat: the bar entered keeps
            // the chord it was entered from when a re-render takes the new one.
            keys.push_back({ locBar * barLenQ + 0.5 * beatQ,
                             triadOf(chordForDegree(U(0, 6), c.scaleType, c.scaleRoot, c.ext7)) });
            std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            const double locQ = locBar * barLenQ;
            bool atEntry = false;   // a change taking effect at the locate's downbeat: bar − 1 ended on the old chord
            for (const auto& kc : keys)
                if (kc.first > 0.0)
                {
                    const double effective = std::ceil(kc.first / beatQ) * beatQ;
                    atEntry = atEntry || std::abs(effective - locQ) < 1.0e-9;
                    changesIn += kc.first < locQ;
                }
            if (atEntry) continue;
            const int toBar = locBar + 4;
            // Evolve 0: a motif diced while stopped evolves from the phrase it
            // is first heard in (§3.8), the located run's own.
            Config kl = c; kl.harm = Harm::Keys; kl.evolve = 0.0f;
            const Run cont = play(kl, 0.0, toBar * barLenQ, tsNum, tsDen, keys, seed);
            const Run loc = play(kl, locQ, toBar * barLenQ, tsNum, tsDen, keys, seed);
            errs += cont.hangs + cont.orphanOffs + cont.badGlides + cont.sounding
                  + loc.hangs + loc.orphanOffs + loc.badGlides + loc.sounding;
            auto lineOf = [&](const Run& r) {
                std::vector<Played> v;
                for (const Played& p : onsInBars(r, barLenQ, locBar, toBar))
                    if (c.layer != Layer::Chord || p.ev.strandId == -1) v.push_back(p);
                return v;
            };
            ++locates;
            w.clear();
            if (!same(lineOf(cont), lineOf(loc), w) && locOff++ < 3)
                locWhy += " | t " + std::to_string(t) + " locate to bar " + std::to_string(locBar) + ": " + w;
        }
        check(trials == 240 && short1 > 40 && locates > 120 && changesIn > 150,
              "cases: trials (" + std::to_string(trials) + ", " + std::to_string(short1)
                  + " in a bar of a beat or less), locates (" + std::to_string(locates) + ", "
                  + std::to_string(changesIn) + " chords changed before them)");
        check(sameOff == 0 && higher == 0,
              "Keys on one chord plays what Chords on it plays (" + std::to_string(sameOff) + " trials differ, "
                  + std::to_string(higher) + " higher; top note " + std::to_string(topKeys) + " vs "
                  + std::to_string(topChords) + ")" + why);
        check(entries > 600 && entriesInside > 100 && entryMatters > 30 && entryOff == 0,
              "Keys: a bar a chord change reaches at its downbeat enters from the chord the bar before ended on ("
                  + std::to_string(entryOff) + " of " + std::to_string(entries) + " off; " + std::to_string(entryMatters)
                  + " where the other chord's entry plays other notes; " + std::to_string(entriesInside)
                  + " with the chord taking effect inside the bar before)" + entryWhy);
        check(locOff == 0, "Keys: a locate plays the line playing reaches (" + std::to_string(locOff) + " of "
                               + std::to_string(locates) + " off)" + locWhy);
        check(errs == 0, "Keys entry: no hanging or orphan note, no bad glide (" + std::to_string(errs) + ")");

        // Oracle 4: Harm turned from Chords to Keys inside a bar. The bar
        // re-renders under Keys, entered from the chord bar − 1 ended on: the
        // one Chords rendered, not a Keys chord latched before (here the one
        // held at the start, under Chords not heard). Keys held on the triad
        // of the one Chords degree, the run plays on what Chords plays, note
        // for note. Evolve 0 and no Stab or Chord layer (their voicing is led
        // live), so the bar's render can be rebuilt here: `turnMatters`
        // counts the turns where entering from the stale chord plays other
        // notes after the turn.
        int turns = 0, turnOff = 0, turnMatters = 0, turnErrs = 0;
        std::string turnWhy;
        for (int t = 0; t < 800; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 2));
            c.scaleRoot = U(0, 11);
            c.stepsPerBar = 4 << U(0, 2);
            c.len = U(1, 2); c.pulses = U(1, c.stepsPerBar); c.rotation = U(0, c.stepsPerBar - 1);
            c.phraseBars = 4 << U(0, 2);
            c.form = static_cast<Form>(U(0, 3));
            c.variation = 0.25f * static_cast<float>(U(0, 4));
            c.slide = 0.5f * static_cast<float>(U(0, 2));
            c.shuffle = U(0, 1) ? 0.0f : 0.4f;
            c.layer = U(0, 2) == 0 ? Layer::Off : (U(0, 1) ? Layer::Octave : Layer::Answer);
            const int mi = U(0, 8);
            const int tsNum = kMeters[mi][0], tsDen = kMeters[mi][1];
            const double barLenQ = 4.0 * tsNum / tsDen, beatQ = 4.0 / tsDen;
            const std::uint32_t seed = static_cast<std::uint32_t>(37500 + t);

            const int deg = U(1, 7);
            Config d = c; d.harm = Harm::Chords; d.chordDegrees = { deg, 0, 0, 0 };
            Config k = d; k.harm = Harm::Keys;
            const Chord stale = chordForDegree((deg - 1 + U(1, 6)) % 7, c.scaleType, c.scaleRoot, c.ext7);
            const std::vector<int> held = triadOf(chordForDegree(deg - 1, c.scaleType, c.scaleRoot, c.ext7));
            const int b = U(2, 12);
            const double bQ = b * barLenQ;
            const double turnQ = bQ + barLenQ * U(1, 15) / 16.0;
            const int bars = b + 2;
            double turnedAt = -1.0;
            const Run ra = play(d, 0.0, bars * barLenQ, tsNum, tsDen, { { 0.0, triadOf(stale) }, { 0.5 * beatQ, held } }, seed,
                                [&](T5ynthPhraseSequencer& s, double q) {
                                    if (turnedAt < 0.0 && q >= turnQ) { s.setConfig(k); turnedAt = q; }
                                });
            const Run rb = play(d, 0.0, bars * barLenQ, tsNum, tsDen, {}, seed);
            turnErrs += ra.hangs + ra.orphanOffs + ra.badGlides + ra.sounding;
            ++turns;
            std::string w;
            if (!same(onsInBars(ra, barLenQ, 0, bars), onsInBars(rb, barLenQ, 0, bars), w) && turnOff++ < 3)
                turnWhy += " | t " + std::to_string(t) + " turn at " + std::to_string(turnedAt) + ": " + w;

            if (barOf(turnedAt, barLenQ) != b) continue;
            const Motif fm = fitMotif(generateMotif(seed, 0, k), k);
            const Motif am = c.layer == Layer::Answer ? answerMotif(fm, k) : Motif {};
            auto after = [&](const Chord& entry) {
                BarRender sc, out;
                int p = -1, pp = -1;
                entryNotes(k, fm, fm, b, barLenQ, &entry, sc, p, pp);
                RenderInput in;
                in.cfg = &k; in.motif = &fm; in.nextPhraseMotif = &fm; in.bar = b; in.barLenQ = barLenQ;
                in.answer = c.layer == Layer::Answer ? &am : nullptr;
                in.keys = true; in.keysChord = chordOf(held); in.prevNote = p; in.prevPrevNote = pp;
                renderBar(in, out);
                std::vector<std::pair<double, int>> v;
                for (int e = 0; e < out.count; ++e)
                    if (bQ + out.events[static_cast<size_t>(e)].onQ > turnedAt + 1.0e-9)
                        v.push_back({ out.events[static_cast<size_t>(e)].onQ, out.events[static_cast<size_t>(e)].note });
                return v;
            };
            turnMatters += after(chordOf(triadOf(stale))) != after(chordForDegree(deg - 1, c.scaleType, c.scaleRoot, c.ext7));
        }
        check(turns == 800 && turnMatters > 30 && turnOff == 0,
              "Chords turned to Keys inside a bar: the bar plays on entered from the chord Chords ended bar − 1 on ("
                  + std::to_string(turnOff) + " of " + std::to_string(turns) + " off; " + std::to_string(turnMatters)
                  + " where entering from the Keys chord latched before plays other notes after the turn)" + turnWhy);
        check(turnErrs == 0, "Chords turned to Keys: no hanging or orphan note, no bad glide (" + std::to_string(turnErrs) + ")");
    }

    std::printf("\n== 38. The view names the chord tone each step sounds\n");
    {
        // Each view step names the chord tone it sounds (StepView::tone and
        // octave): a chord-tone step the member the chord gives its slot (a 7
        // the chord lacks sounds as R an octave up, a missing 3 as the 5 or
        // R, a missing 5 as the 3 or R); the ABAB cadence on the bar's last
        // sounding head R (closed), 5 (open), or scale degree 2 (open over a
        // tonic pedal; the member it is where the scale steps from the root
        // onto a chord tone, as the minor pentatonic onto the 3); a Stab hit
        // the chord; a rest, a silenced step and an
        // embellishment nothing. A step a non-Bass Lift raises, a chord tone,
        // an embellishment or a Fill note on a step the motif leaves empty,
        // shows the octave it goes up (an embellishment showed none). Oracle:
        // the slot, the bar's variation and the fallback table written out
        // here; every tone is the note its step plays; and its glyph names
        // that note as the bar's chord name does (Csus4's F a 4, Cadd9's D a
        // 9: the glyph showed the member, 3 and 5).
        // Scales whose degrees lack members, Ext 3 and 7, Pedal, Chords and
        // Keys (one to four held keys: chords with no 3, no 5, neither).
        // B: the same through readView() and chordForGui while playing.
        std::mt19937 rng(38);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        static constexpr int kScales[] = { ScaleQuantizer::Minor, ScaleQuantizer::Major, ScaleQuantizer::HarmonicMinor,
                                           ScaleQuantizer::Pentatonic, ScaleQuantizer::MinorPentatonic,
                                           ScaleQuantizer::WholeTone, ScaleQuantizer::Iwato, ScaleQuantizer::Hirajoshi,
                                           ScaleQuantizer::Ryukyu, ScaleQuantizer::Pelog, ScaleQuantizer::Off };
        auto memberWant = [](const Chord& ch, int member, int& oct) {
            if (ch.iv[static_cast<size_t>(member)] >= 0) return static_cast<Tone>(static_cast<int>(Tone::Root) + member);
            if (member == Seventh) { ++oct; return Tone::Root; }
            const int other = member == Third ? Fifth : Third;
            return ch.iv[static_cast<size_t>(other)] >= 0 ? static_cast<Tone>(static_cast<int>(Tone::Root) + other)
                                                          : Tone::Root;
        };
        auto randomConfig = [&]() {
            Config c = base();
            c.part = static_cast<Part>(U(0, 3));
            c.harm = static_cast<Harm>(U(0, 2));
            c.scaleType = kScales[U(0, static_cast<int>(std::size(kScales)) - 1)];
            c.scaleRoot = U(0, 11);
            c.ext7 = U(0, 1) != 0;
            c.chordDegrees = { U(1, 7), U(0, 7), U(0, 7), U(0, 7) };
            c.stepsPerBar = 4 << U(0, 2);
            c.len = U(1, 2); c.pulses = U(1, c.stepsPerBar); c.rotation = U(0, c.stepsPerBar - 1);
            c.phraseBars = 4 << U(0, 2);
            c.form = U(0, 1) ? Form::ABAB : static_cast<Form>(U(0, 3));
            c.variation = 0.25f * static_cast<float>(U(0, 4));
            c.evolve = 0.25f * static_cast<float>(U(0, 2));
            c.shuffle = U(0, 1) ? 0.0f : 0.4f;
            return c;
        };
        auto keysChord = [&]() {
            std::uint16_t mask = 0;
            const int low = U(36, 72);
            mask |= static_cast<std::uint16_t>(1u << (low % 12));
            for (int i = U(0, 3); i > 0; --i) mask |= static_cast<std::uint16_t>(1u << U(0, 11));
            return chordFromKeys(low, mask);
        };

        enum { kSeventhAsRoot, kThirdAsFifth, kThirdAsRoot, kFifthAsThird, kFifthAsRoot,
               kCadRoot, kCadFifth, kCadSecond, kCadSecondAsMember, kLifted, kLiftedEmbellishment, kLiftedFill,
               kStab, kKeys, kSeen };
        static const char* kSeenName[] = { "7 as R'", "3 as 5", "3 as R", "5 as 3", "5 as R",
                                           "cadence R", "cadence 5", "cadence 2", "cadence 2 a member", "lifted",
                                           "lifted embellishment", "lifted Fill", "Stab hits", "Keys bars" };
        // The open cadence over a pedal: the scale's next pitch class above
        // the chord root, named the member it is when it is one.
        auto secondWant = [](const Chord& ch, std::uint16_t mask) {
            int d = 1;
            while (d < 12 && ((mask >> ((ch.rootPc + d) % 12)) & 1u) == 0) ++d;
            for (int member = Third; member <= Seventh; ++member)
                if (ch.iv[static_cast<size_t>(member)] == d) return static_cast<Tone>(static_cast<int>(Tone::Root) + member);
            return Tone::Second;
        };
        std::array<int, kSeen> seen {};
        std::map<std::string, int> labels;
        int steps = 0, off = 0, offNote = 0, offLabel = 0;
        std::string why;
        auto fail = [&](int& n, int t, int k, const std::string& w) {
            if (n++ < 4) why += " | t " + std::to_string(t) + " step " + std::to_string(k) + ": " + w;
        };
        for (int t = 0; t < 4000; ++t)
        {
            const Config c = randomConfig();
            const Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(38000 + t), 0, c), c);
            const double barLenQ = U(0, 3) == 0 ? 3.0 : 4.0;
            RenderInput in;
            in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m;
            in.bar = U(0, 31); in.barLenQ = barLenQ;
            in.prevNote = U(36, 72); in.prevPrevNote = U(36, 72);
            if (c.harm == Harm::Keys) { in.keys = true; in.keysChord = keysChord(); }
            BarRender r;
            renderBar(in, r);

            std::array<bool, kMaxMotifSteps> heads {};
            computeHeads(m, heads);
            const BarVariation var = variationForBar(m, heads, c, in.bar);
            const int spb = m.stepsPerBar, mb = floorMod(in.bar, m.len);
            const double stepQ = 4.0 / spb, lastBeatQ = std::max(0.0, barLenQ - 1.0);
            const Chord& ch = r.chord;
            const int tonic = tonicNote(partAnchor(c.part), c.scaleRoot);
            const std::uint16_t mask = scaleMask(c.scaleType, c.scaleRoot);
            int cadenceK = -1;
            for (int k = 0; k < r.viewSteps; ++k)
                if (r.view[static_cast<size_t>(k)].head && r.view[static_cast<size_t>(k)].note >= 0) cadenceK = k;
            if (var.cadence == 0) cadenceK = -1;
            seen[kKeys] += in.keys;
            for (int k = 0; k < r.viewSteps; ++k)
            {
                const StepView& v = r.view[static_cast<size_t>(k)];
                const Slot& sl = m.slots[static_cast<size_t>(mb * spb + k)];
                const int up = var.lift && c.part != Part::Bass && k * stepQ >= lastBeatQ - 1.0e-9 ? 1 : 0;
                Tone want = Tone::None;
                int oct = 0;
                if (v.note < 0)
                    want = Tone::None;
                else if (c.part == Part::Stab)
                {
                    want = Tone::Chord; oct = up; ++seen[kStab];
                }
                else if (k == cadenceK)
                {
                    oct = sl.octave;
                    want = var.cadence == 2 ? Tone::Root
                         : c.harm == Harm::Pedal ? secondWant(ch, mask)
                         : memberWant(ch, Fifth, oct);
                    oct += up;
                    ++seen[var.cadence == 2 ? kCadRoot
                           : c.harm != Harm::Pedal ? kCadFifth
                           : want == Tone::Second ? kCadSecond : kCadSecondAsMember];
                }
                else if (v.fn == Fn::ChordTone)
                {
                    oct = sl.octave;
                    want = memberWant(ch, sl.member, oct);
                    if (ch.iv[static_cast<size_t>(sl.member)] < 0)
                        ++seen[sl.member == Seventh ? kSeventhAsRoot
                               : sl.member == Third ? (want == Tone::Fifth ? kThirdAsFifth : kThirdAsRoot)
                                                    : (want == Tone::Third ? kFifthAsThird : kFifthAsRoot)];
                    oct += up;
                }
                if (want != Tone::None && up) ++seen[kLifted];
                if (want == Tone::None && v.note >= 0 && up)
                    ++seen[sl.fn == Fn::Rest ? kLiftedFill : kLiftedEmbellishment];
                if (want == Tone::None) oct = v.note >= 0 ? up : 0;
                ++steps;
                if (v.tone != want || v.octave != oct)
                    fail(off, t, k, "names tone " + std::to_string(static_cast<int>(v.tone)) + " octave "
                                        + std::to_string(v.octave) + ", want " + std::to_string(static_cast<int>(want))
                                        + " octave " + std::to_string(oct));
                const std::string w = toneMismatch(v, ch, tonic, mask);
                if (!w.empty()) fail(offNote, t, k, w);
                const std::string wl = labelMismatch(v, ch);
                if (!wl.empty()) fail(offLabel, t, k, wl);
                if (v.note >= 0 && v.tone != Tone::None && v.tone != Tone::Chord)
                {
                    std::string g = stepGlyphText(v);
                    while (!g.empty() && (g.back() == '\'' || g.back() == ',')) g.pop_back();
                    ++labels[v.tone == Tone::Second ? "2 (cadence)" : g];
                }
            }
        }
        std::string counts;
        bool covered = true;
        for (int i = 0; i < kSeen; ++i)
        {
            counts += std::string(i ? ", " : "") + kSeenName[i] + " " + std::to_string(seen[static_cast<size_t>(i)]);
            covered = covered && seen[static_cast<size_t>(i)] >= 20;
        }
        check(covered, "the cases met (" + counts + ")");
        check(off == 0, "A. each step names the tone its slot, the cadence or the Stab gives it ("
                            + std::to_string(off) + " of " + std::to_string(steps) + " off)" + why);
        check(offNote == 0, "A. each named tone is the note its step plays (" + std::to_string(offNote) + " off)");
        std::string labelCounts;
        int addLabels = 0;
        for (const auto& [g, n] : labels)
        {
            labelCounts += (labelCounts.empty() ? "" : ", ") + g + " " + std::to_string(n);
            if (g != "R" && g != "2" && g != "2 (cadence)" && g != "3" && g != "4" && g != "5" && g != "6" && g != "7") addLabels += n;
        }
        const bool labelsMet = labels["R"] >= 20 && labels["2"] >= 20 && labels["3"] >= 20 && labels["4"] >= 20
                            && labels["5"] >= 20 && labels["6"] >= 20 && labels["7"] >= 20 && addLabels >= 20;
        check(offLabel == 0 && labelsMet, "A. each tone's glyph names its note as the bar's chord name does ("
                                              + std::to_string(offLabel) + " off; " + labelCounts + ")");

        // B: what readView() and chordForGui hand the GUI. Keys holds, after
        // Am, F7 and G5, chords whose added member is named by an extension
        // (Cadd♭9, Cadd9, Cadd11, Cadd♯11, Cadd♭13, C7addmaj7no5), so the
        // labels the view word carries above 7 go through its packing too.
        // ♯9 is no such label: held keys make it the minor third (Cmadd3).
        int bars = 0, named = 0, octaves = 0, seconds = 0, offB = 0, unnamed = 0;
        std::array<int, static_cast<size_t>(ToneLabel::Count)> labelSeen {};
        std::string whyB;
        for (int t = 0; t < 120; ++t)
        {
            Config c = randomConfig();
            if (t % 3 == 0) { c.form = Form::ABAB; c.harm = Harm::Pedal; c.part = Part::Riff; }
            std::vector<std::pair<double, std::vector<int>>> keys;
            if (c.harm == Harm::Keys) keys = { { 0.0, { 45, 48, 52 } }, { 8.0, { 41, 45, 48, 51 } }, { 16.0, { 43, 50 } },
                                               { 24.0, { 36, 40, 43, 49 } }, { 32.0, { 36, 40, 43, 50 } },
                                               { 40.0, { 36, 40, 43, 53 } }, { 48.0, { 36, 40, 43, 54 } },
                                               { 56.0, { 36, 40, 43, 56 } }, { 64.0, { 36, 40, 46, 47 } } };
            const int tonic = tonicNote(partAnchor(c.part), c.scaleRoot);
            const std::uint16_t mask = scaleMask(c.scaleType, c.scaleRoot);
            int lastBar = -1;
            play(c, 0.0, 24 * 4.0, 4, 4, keys, static_cast<std::uint32_t>(38500 + t), [&](T5ynthPhraseSequencer& s, double) {
                const int b = s.currentBarForGui.load();
                if (b == lastBar) return;
                lastBar = b;
                ++bars;
                std::array<StepView, kMaxBarSteps> v {};
                const int n = s.readView(v);
                const Chord ch = chordFromGui(s.chordForGui.load());
                for (int k = 0; k < n; ++k)
                {
                    const StepView& x = v[static_cast<size_t>(k)];
                    if (x.note >= 0 && x.fn == Fn::ChordTone && x.tone == Tone::None) ++unnamed;
                    if (x.tone == Tone::None) continue;
                    ++named;
                    ++labelSeen[static_cast<size_t>(x.label)];
                    octaves += x.octave != 0;
                    seconds += x.tone == Tone::Second;
                    std::string w = toneMismatch(x, ch, tonic, mask);
                    if (w.empty()) w = labelMismatch(x, ch);
                    if (!w.empty() && offB++ < 4) whyB += " | t " + std::to_string(t) + " bar " + std::to_string(b) + ": " + w;
                }
            });
        }
        check(bars > 1000 && named > 5000 && octaves > 300 && seconds > 20,
              "B. bars read (" + std::to_string(bars) + "; " + std::to_string(named) + " tones, "
                  + std::to_string(octaves) + " off octave 0, " + std::to_string(seconds) + " cadence 2)");
        check(offB == 0 && unnamed == 0, "B. readView hands each sounding chord-tone step its tone, each the note it plays, named as the chord does ("
                                             + std::to_string(offB) + " off, " + std::to_string(unnamed) + " unnamed)" + whyB);
        std::string extSeen;
        bool extMet = true;
        for (ToneLabel l : { ToneLabel::Flat9, ToneLabel::Ninth, ToneLabel::Eleventh, ToneLabel::Sharp11,
                             ToneLabel::Flat13, ToneLabel::Major7 })
        {
            const int seen = labelSeen[static_cast<size_t>(l)];
            extMet = extMet && seen >= 3;
            StepView sv; sv.note = 60; sv.tone = Tone::Seventh; sv.label = l;
            extSeen += (extSeen.empty() ? "" : ", ") + stepGlyphText(sv) + " " + std::to_string(seen);
        }
        check(extMet, "B. each extension label crosses readView (" + extSeen + ")");
    }

    std::printf("\n== 39. The view's texts: chord names, step glyphs, note names\n");
    {
        const std::string fl = "\xe2\x99\xad", sh = "\xe2\x99\xaf", dg = "\xc2\xb0", hd = "\xc3\xb8";
        auto chord = [](int root, int t, int f, int sv) { Chord c; c.rootPc = root; c.iv = { 0, t, f, sv }; return c; };

        // A: names written out, one per shape the scales give (every scale,
        // every degree, Ext 3 and 7 — the probe that listed them is the list
        // below) and the shapes only held keys give.
        struct Named { int root, t, f, sv; std::string want; };
        const std::vector<Named> named = {
            { 0, 4, 7, -1, "C" },            { 9, 3, 7, -1, "Am" },           { 11, 3, 6, -1, "B" + dg },
            { 0, 4, 8, -1, "C+" },           { 9, -1, 7, -1, "A5" },          { 0, -1, 6, -1, "C(" + fl + "5)no3" },
            { 0, -1, 6, 10, "C7(" + fl + "5)no3" }, { 0, -1, 7, 10, "C7no3" }, { 0, -1, 7, 11, "Cmaj7no3" },
            { 0, 2, 6, 9, "C6sus2(" + fl + "5)" },  { 0, 2, 6, 10, "C7sus2(" + fl + "5)" }, { 0, 2, 7, 9, "C6sus2" },
            { 11, 3, 6, 9, "B" + dg + "7" }, { 11, 3, 6, 10, "B" + hd + "7" }, { 0, 3, 7, 9, "Cm6" },
            { 2, 3, 7, 10, "Dm7" },          { 0, 3, 7, 11, "Cm(maj7)" },     { 0, 3, 8, 10, "Cm7(" + sh + "5)" },
            { 0, 4, -1, 10, "C7no5" },       { 0, 4, -1, 11, "Cmaj7no5" },    { 0, 4, 6, 10, "C7(" + fl + "5)" },
            { 0, 4, 6, 11, "Cmaj7(" + fl + "5)" }, { 7, 4, 7, 10, "G7" },     { 5, 4, 7, 11, "Fmaj7" },
            { 0, 4, 8, 10, "C+7" },          { 0, 4, 8, 11, "C+(maj7)" },     { 0, 5, 7, 11, "Cmaj7sus4" },
            { 4, 5, 7, -1, "Esus4" },        { 0, 2, 7, -1, "Csus2" },        { 0, 4, -1, -1, "Cno5" },
            { 1, 3, -1, -1, "C#mno5" },      { 10, 3, 7, 10, "A#m7" },        { 6, 4, 7, -1, "F#" },
            // held keys only
            { 0, -1, -1, -1, "Cno3no5" },    { 0, 4, 7, 2, "Cadd9" },         { 0, 4, 9, -1, "Cadd6no5" },
            { 0, 9, 7, -1, "Cadd6no3" },     { 0, 1, 7, -1, "Cadd" + fl + "9no3" }, { 0, 3, 7, 4, "Cmadd3" },
            { 0, 4, 7, 6, "Cadd" + sh + "11" }, { 0, 4, 7, 8, "Cadd" + fl + "13" }, { 0, 4, 7, 5, "Cadd11" },
            { 0, 4, 11, 10, "C7addmaj7no5" }, { 0, 5, -1, 10, "C7sus4no5" }, { 0, 2, 6, -1, "Csus2(" + fl + "5)" },
            { 0, 3, 8, -1, "Cm(" + sh + "5)" }, { 0, 4, 6, -1, "C(" + fl + "5)" },
        };
        int offA = 0;
        std::string whyA;
        for (const auto& n : named)
        {
            const std::string got = chordNameText(chord(n.root, n.t, n.f, n.sv));
            if (got != n.want && offA++ < 6) whyA += " | " + std::to_string(n.t) + "," + std::to_string(n.f) + "," + std::to_string(n.sv) + ": " + got + " (want " + n.want + ")";
        }
        check(offA == 0, "A. chord names, " + std::to_string(named.size()) + " shapes written out (" + std::to_string(offA) + " off)" + whyA);

        // B: every chord the scales and held keys make, read back by the
        // chord-symbol reader written here (readChordName): the root, third,
        // fifth and seventh it reads are the chord's members, and its add…
        // are the members the vocabulary has no name for.
        int names = 0, offB = 0, scaleAdds = 0;
        std::string whyB;
        auto verify = [&](const Chord& c, bool fromScale) {
            ++names;
            const std::string name = chordNameText(c);
            const ChordRead r = readChordName(name);
            const int t = c.iv[1], f = c.iv[2], sv = c.iv[3];
            const bool tIn = t == 2 || t == 3 || t == 4 || t == 5, fIn = f == 6 || f == 7 || f == 8,
                       sIn = sv == 9 || sv == 10 || sv == 11;
            std::vector<int> adds;
            if (t > 0 && !tIn) adds.push_back(t);
            if (f > 0 && !fIn) adds.push_back(f);
            if (sv > 0 && !sIn) adds.push_back(sv);
            std::vector<int> got = r.adds;
            std::sort(adds.begin(), adds.end());
            std::sort(got.begin(), got.end());
            if (fromScale) scaleAdds += !adds.empty();
            const bool same = r.ok && r.root == c.rootPc && r.t == (tIn ? t : -1) && r.f == (fIn ? f : -1)
                              && r.sv == (sIn ? sv : -1) && got == adds;
            if (!same && offB++ < 6)
                whyB += " | " + std::to_string(c.rootPc) + ":" + std::to_string(t) + "," + std::to_string(f) + ","
                        + std::to_string(sv) + " named " + name + (r.ok ? "" : " (unreadable)");
        };
        for (int sc = 0; sc < ScaleQuantizer::COUNT; ++sc)
            for (int d = 0; d < 7; ++d)
                for (int root = 0; root < 12; ++root)
                    for (int e7 = 0; e7 < 2; ++e7)
                        verify(chordForDegree(d, sc, root, e7 != 0), true);
        for (int low = 36; low < 48; ++low)
            for (int mask = 0; mask < 4096; ++mask)
                verify(chordFromKeys(low, static_cast<std::uint16_t>(mask)), false);
        check(offB == 0 && scaleAdds == 0, "B. " + std::to_string(names) + " chords (every scale, degree, root and Ext; every held-key set) read back as their members ("
                                               + std::to_string(offB) + " off, " + std::to_string(scaleAdds) + " scale chords needing add)" + whyB);

        // C: step glyphs. A chord tone shows the label the model gave it,
        // the chord name's word for its pitch; the member it is does not
        // decide (a Third labelled 4 is Csus4's F).
        auto sv = [](int note, Fn fn, Tone tone, int oct, ToneLabel label = ToneLabel::None) {
            StepView v; v.note = note; v.fn = fn; v.tone = tone; v.octave = oct; v.label = label; return v;
        };
        const std::string dot = "\xc2\xb7", hit = "\xe2\x97\x8f", up = "\xe2\x86\x91", down = "\xe2\x86\x93",
                          rise = "\xe2\x86\x97", fall = "\xe2\x86\x98", half = "\xc2\xbd",
                          flat = "\xe2\x99\xad", sharp = "\xe2\x99\xaf";
        using L = ToneLabel;
        const std::vector<std::pair<StepView, std::string>> glyphs = {
            { sv(-1, Fn::Rest, Tone::None, 0), dot },          { sv(-1, Fn::ChordTone, Tone::None, 0), dot },
            { sv(-1, Fn::ChordTone, Tone::Root, 1, L::Root), dot },     { sv(45, Fn::ChordTone, Tone::Root, 0, L::Root), "R" },
            { sv(57, Fn::ChordTone, Tone::Root, 1, L::Root), "R'" },    { sv(69, Fn::ChordTone, Tone::Root, 2, L::Root), "R''" },
            { sv(33, Fn::ChordTone, Tone::Root, -1, L::Root), "R," },   { sv(48, Fn::ChordTone, Tone::Third, 0, L::Third), "3" },
            { sv(52, Fn::ChordTone, Tone::Fifth, 0, L::Fifth), "5" },   { sv(55, Fn::ChordTone, Tone::Seventh, 0, L::Seventh), "7" },
            { sv(59, Fn::ChordTone, Tone::Seventh, -1, L::Seventh), "7," }, { sv(47, Fn::ChordTone, Tone::Second, 0, L::Second), "2" },
            { sv(59, Fn::ChordTone, Tone::Second, 1, L::Second), "2'" }, { sv(60, Fn::ChordTone, Tone::Chord, 0), hit },
            { sv(72, Fn::ChordTone, Tone::Chord, 1, L::Root), hit + "'" }, { sv(52, Fn::Passing, Tone::Fifth, 0, L::Fifth), "5" },
            { sv(50, Fn::ChordTone, Tone::Third, 0, L::Second), "2" },  { sv(53, Fn::ChordTone, Tone::Third, 0, L::Fourth), "4" },
            { sv(57, Fn::ChordTone, Tone::Seventh, 0, L::Sixth), "6" }, { sv(57, Fn::ChordTone, Tone::Seventh, 1, L::Seventh), "7'" },
            { sv(49, Fn::ChordTone, Tone::Seventh, 0, L::Flat9), flat + "9" }, { sv(50, Fn::ChordTone, Tone::Fifth, 0, L::Ninth), "9" },
            { sv(51, Fn::ChordTone, Tone::Third, 0, L::Sharp9), sharp + "9" }, { sv(53, Fn::ChordTone, Tone::Seventh, 0, L::Eleventh), "11" },
            { sv(54, Fn::ChordTone, Tone::Fifth, 0, L::Sharp11), sharp + "11" }, { sv(56, Fn::ChordTone, Tone::Seventh, 0, L::Flat13), flat + "13" },
            { sv(59, Fn::ChordTone, Tone::Seventh, 0, L::Major7), "maj7" }, { sv(62, Fn::ChordTone, Tone::Fifth, 1, L::Ninth), "9'" },
            { sv(48, Fn::ChordTone, Tone::Third, 0), "?" },
            { sv(45, Fn::Repeat, Tone::None, 0), "=" },        { sv(57, Fn::Octave, Tone::None, 0), "8ve" },
            { sv(47, Fn::Passing, Tone::None, 0), "p" },       { sv(47, Fn::NeighbourUp, Tone::None, 0), "n" + up },
            { sv(43, Fn::NeighbourDown, Tone::None, 0), "n" + down }, { sv(43, Fn::ApproachUp, Tone::None, 0), rise },
            { sv(47, Fn::ApproachDown, Tone::None, 0), fall }, { sv(44, Fn::ApproachChromatic, Tone::None, 0), half + rise },
            { sv(59, Fn::Passing, Tone::None, 1), "p'" },      { sv(55, Fn::NeighbourUp, Tone::None, 1), "n" + up + "'" },
            { sv(57, Fn::Repeat, Tone::None, 1), "='" },       { sv(56, Fn::ApproachChromatic, Tone::None, 1), half + rise + "'" },
        };
        int offC = 0;
        std::string whyC;
        for (size_t i = 0; i < glyphs.size(); ++i)
        {
            const std::string got = stepGlyphText(glyphs[i].first);
            if (got != glyphs[i].second && offC++ < 6) whyC += " | #" + std::to_string(i) + ": " + got + " (want " + glyphs[i].second + ")";
        }
        check(offC == 0, "C. step glyphs, " + std::to_string(glyphs.size()) + " written out (" + std::to_string(offC) + " off)" + whyC);

        // D: every step the model renders has a glyph that says what it is
        // (none falls through to ?, as a chord tone without a label would),
        // and the rest dot stands exactly where nothing sounds.
        std::mt19937 rng(39);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        int seenSteps = 0, offD = 0;
        for (int t = 0; t < 2000; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 3));
            c.harm = static_cast<Harm>(U(0, 2));
            c.scaleType = U(0, ScaleQuantizer::COUNT - 1);
            c.ext7 = U(0, 1) != 0;
            c.chordDegrees = { U(1, 7), U(0, 7), U(0, 7), U(0, 7) };
            c.stepsPerBar = 4 << U(0, 2);
            c.pulses = U(1, c.stepsPerBar);
            c.form = static_cast<Form>(U(0, 3));
            c.variation = 0.25f * static_cast<float>(U(0, 4));
            const Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(39000 + t), 0, c), c);
            RenderInput in;
            in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m;
            in.bar = U(0, 15);
            in.prevNote = U(36, 72); in.prevPrevNote = U(36, 72);
            if (c.harm == Harm::Keys) { in.keys = true; in.keysChord = chordFromKeys(U(36, 60), static_cast<std::uint16_t>(U(1, 4095))); }
            BarRender r;
            renderBar(in, r);
            for (int k = 0; k < r.viewSteps; ++k)
            {
                const StepView& v = r.view[static_cast<size_t>(k)];
                const std::string g = stepGlyphText(v);
                ++seenSteps;
                if (g == "?" || (v.note >= 0) == (g == dot)) ++offD;
            }
        }
        check(offD == 0, "D. " + std::to_string(seenSteps) + " rendered steps: a glyph for each, a rest only where nothing sounds (" + std::to_string(offD) + " off)");

        // E: note names as the step grid writes them.
        const std::vector<std::pair<int, std::string>> notes = { { 60, "C4" }, { 57, "A3" }, { 61, "C#4" }, { 0, "C-1" },
                                                                 { 11, "B-1" }, { 127, "G9" }, { 46, "A#2" } };
        int offE = 0;
        for (const auto& [n, want] : notes) offE += noteNameText(n) != want;
        check(offE == 0, "E. note names C4 = 60, sharps (" + std::to_string(offE) + " off)");
    }

    // ── Chord labels: the Roman numerals the chord slots show (§3.3) ──
    {
        auto row = [](int scaleType, bool ext7) {
            std::string out;
            for (int d = 0; d < degreeCount(scaleType); ++d)
                out += (d ? " " : "") + chordLabelText(chordLabel(d, scaleType, ext7));
            return out;
        };
        auto expect = [&](int scaleType, const char* name, const std::string& want, bool ext7 = false) {
            const std::string got = row(scaleType, ext7);
            check(got == want, std::string("chord labels, ") + name + ": " + got + (got == want ? "" : " (want " + want + ")"));
        };
        const std::string fl = "\xe2\x99\xad", sh = "\xe2\x99\xaf", dg = "\xc2\xb0";
        expect(2,  "Minor",    "i ii" + dg + " " + fl + "III iv v " + fl + "VI " + fl + "VII");
        expect(1,  "Major",    "I ii iii IV V vi vii" + dg);
        expect(5,  "Harm.Min", "i ii" + dg + " " + fl + "III+ iv V " + fl + "VI vii" + dg);
        expect(11, "MinPent",  "i " + fl + "III IV5 v" + sh + "5 " + fl + "VII5");
        expect(23, "Persian",  "I" + fl + "5 " + fl + "II IIIsus2 iv " + fl + "Vsus4 " + fl + "VI+ VIIsus2" + fl + "5");
        // A degree with a third and no fifth in the scale plays root and third
        // only (chordToneNote's 5 → 3 fallback): no5, not the bare triad name.
        expect(16, "Iwato",    "I" + fl + "5no3 " + fl + "IIno5 IV5 " + sh + "IV " + fl + "vii");
        expect(14, "Hirajoshi", "i II" + fl + "5no3 " + fl + "IIIno5 V5 " + fl + "VI");
        expect(18, "Ryukyu",   "I iii IV5 Vno5 VII" + fl + "5no3");
        expect(26, "Pelog",    "i " + fl + "II5 " + fl + "IIIno5 V" + fl + "5no3 " + fl + "VI");

        // Ext = 7: the bare numeral reads as the seventh chord, so a degree
        // the scale has no seventh above (neither 10 nor 11) is marked no7.
        // A heptatonic degree stacks its scale's 7th step above, a major
        // sixth on some degrees of the exotic ones: 6, as the strip names
        // it (Em6, B6sus2). Over a diminished triad the 9 is the °7.
        expect(2,  "Minor, Ext 7", "i ii" + dg + " " + fl + "III iv v " + fl + "VI " + fl + "VII", true);
        expect(3,  "Penta, Ext 7", "Ino7 II5 iii" + sh + "5 V5no7 vi", true);
        expect(12, "Blues, Ext 7", "i " + fl + "iiino7 IV5 " + sh + "IV" + fl + "5 v" + sh + "5 " + fl + "VII5no7", true);
        expect(15, "In-sen, Ext 7", "I5 " + fl + "II" + fl + "5 IV5no7 v" + dg + " " + fl + "viino7", true);
        expect(18, "Ryukyu, Ext 7", "I iiino7 IV5 Vno5 VII" + fl + "5no3no7", true);
        expect(5,  "Harm.Min, Ext 7", "i ii" + dg + " " + fl + "III+ iv V " + fl + "VI vii" + dg, true);
        expect(20, "DblHarm, Ext 7", "I " + fl + "II iii6 iv V" + fl + "5 " + fl + "VI+ VII6sus2" + fl + "5", true);
        expect(23, "Persian, Ext 7", "I" + fl + "5 " + fl + "II III6sus2 iv " + fl + "Vsus4 " + fl + "VI+ VII6sus2" + fl + "5", true);
        expect(24, "HungMin, Ext 7", "i II" + fl + "5 " + fl + "III+ " + sh + "IV6sus2" + fl + "5 V " + fl + "VI vii6", true);

        // no7 after every triad name, the ones that end the name included:
        // no scale has a diminished or augmented degree without a seventh.
        ChordLabel dimL; dimL.numeral = 7; dimL.third = 3; dimL.fifth = 6; dimL.ext7 = true;
        ChordLabel augL; augL.numeral = 3; augL.third = 4; augL.fifth = 8; augL.ext7 = true;
        check(chordLabelText(dimL) == "vii" + dg + "no7" && chordLabelText(augL) == "III+no7",
              "chord labels, no7 after a closed name: " + chordLabelText(dimL) + " " + chordLabelText(augL));
        // A sixth in the 7's place in chordNameText's order, also over the
        // triads no scale stacks one on: III+6 (C+6), V6no3 (C6no3; 5 names
        // an open fifth with nothing above it); over ° it is the °7.
        ChordLabel aug6 = augL; aug6.seventh = 9;
        ChordLabel pow6; pow6.numeral = 5; pow6.fifth = 7; pow6.seventh = 9; pow6.ext7 = true;
        ChordLabel dim7 = dimL; dim7.seventh = 9;
        ChordLabel ext3; ext3.numeral = 3; ext3.third = 3; ext3.fifth = 7;
        check(chordLabelText(aug6) == "III+6" && chordLabelText(pow6) == "V6no3" && chordLabelText(dim7) == "vii" + dg
                  && chordLabelText(ext3) == "iii",
              "chord labels, 6 after the quality, none over " + dg + ", no mark under Ext 3: " + chordLabelText(aug6) + " "
                  + chordLabelText(pow6) + " " + chordLabelText(dim7) + " " + chordLabelText(ext3));

        // The label says what plays, read from what Stab sounds on the
        // degree under Ext 7 (every scale, degree and key): no7 where the
        // chord sounds nothing 9, 10 or 11 above its root, and there the
        // motif's 7 is the root's octave; 6 where it sounds a 9 over a triad
        // that is not diminished (a 3 and a 6 together), as the strip names
        // it; neither where it sounds a seventh.
        int seen7 = 0, marked7 = 0, marked6 = 0, off7 = 0, silent7 = 0;
        std::string why7;
        for (int st = 0; st < ScaleQuantizer::COUNT; ++st)
        {
            if (!chordsAvailable(st)) continue;
            for (int d = 0; d < degreeCount(st); ++d)
                for (int root = 0; root < 12; ++root)
                {
                    Config c = base();
                    c.part = Part::Stab;
                    c.harm = Harm::Chords;
                    c.ext7 = true;
                    c.scaleType = st;
                    c.scaleRoot = root;
                    c.chordDegrees = { d + 1, 0, 0, 0 };
                    c.variation = 0.0f;
                    const Motif m = generateMotif(7, 0, c);
                    RenderInput in;
                    in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m;
                    in.bar = 0; in.barLenQ = 4.0;
                    BarRender r;
                    renderBar(in, r);
                    const int rootPc = floorMod(root + ScaleQuantizer::kScaleDegrees[st][d], 12);
                    std::uint16_t rel = 0;
                    for (int i = 0; i < r.count; ++i)
                        rel |= static_cast<std::uint16_t>(1u << floorMod(r.events[static_cast<size_t>(i)].note - rootPc, 12));
                    auto plays = [&](int iv) { return ((rel >> iv) & 1u) != 0; };
                    if (r.count == 0) { ++silent7; continue; }
                    const bool seventh = plays(10) || plays(11), sixth = plays(9) && !(plays(3) && plays(6));
                    const bool none = !seventh && !plays(9);
                    const std::string t = chordLabelText(chordLabel(d, st, true));
                    const bool saysNo7 = t.size() >= 3 && t.compare(t.size() - 3, 3, "no7") == 0;
                    const bool says6 = t.find('6') != std::string::npos;
                    const Chord ch = chordForDegree(d, st, root, true);
                    ++seen7;
                    marked7 += saysNo7;
                    marked6 += says6;
                    if (saysNo7 != none || says6 != sixth
                        || (none && chordToneNote(ch, Seventh, 0, 60) != chordToneNote(ch, Root, 1, 60)))
                    {
                        if (++off7 <= 3) why7 += " | scale " + std::to_string(st) + " degree " + std::to_string(d + 1) + ": " + t;
                    }
                }
        }
        check(off7 == 0 && silent7 == 0 && marked7 > 0 && marked6 > 0,
              "chord labels, Ext 7: " + std::to_string(seen7) + " degrees in every key, read from what Stab plays: "
              + std::to_string(marked7) + " with no seventh read no7 and play R' for the 7, " + std::to_string(marked6)
              + " with a sixth in its place read 6 (" + std::to_string(off7) + " off, " + std::to_string(silent7) + " silent)" + why7);
    }

    std::printf("\n== 40. The view shows a note on every step a note starts on\n");
    {
        // Read from what plays, not from the view: for each step of the bar,
        // the main-line notes that start on its (shuffled) onset. The view
        // shows a note there exactly when one starts, and the note it shows
        // is the one that sounds (Stab: the chord's lowest voice) without Seq
        // Octave, as the step grid takes it. A Fill run note on a step the
        // motif leaves empty is such a note (the view showed a rest there).
        std::mt19937 rng(40);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        static const int spbs[] = { 1, 2, 4, 8, 16 };
        int seen = 0, sounding = 0, onEmptyLine = 0, onEmptyStab = 0, off = 0;
        std::string why;
        for (int t = 0; t < 20000; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 3));
            c.harm = static_cast<Harm>(U(0, 2));
            c.scaleType = U(0, ScaleQuantizer::COUNT - 1);
            c.scaleRoot = U(0, 11);
            c.ext7 = U(0, 1) != 0;
            c.chordDegrees = { U(1, 7), U(0, 7), U(0, 7), U(0, 7) };
            c.stepsPerBar = spbs[U(0, 4)];
            c.pulses = U(1, c.stepsPerBar);
            c.rotation = U(0, c.stepsPerBar - 1);
            c.form = static_cast<Form>(U(0, 3));
            c.variation = 0.25f * static_cast<float>(U(0, 4));
            c.slide = 0.25f * static_cast<float>(U(0, 4));
            c.layer = static_cast<Layer>(U(0, 3));
            c.shuffle = 0.25f * static_cast<float>(U(0, 3));
            c.transpose = 12 * U(-2, 2);
            const Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(40000 + t), 0, c), c);
            RenderInput in;
            in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m; in.answer = &m;
            in.bar = U(0, 15);
            in.barLenQ = 0.5 * U(1, 14);
            in.prevNote = U(36, 72); in.prevPrevNote = U(36, 72);
            in.slideIn = U(0, 1) != 0;
            if (c.harm == Harm::Keys) { in.keys = true; in.keysChord = chordFromKeys(U(36, 60), static_cast<std::uint16_t>(U(1, 4095))); }
            BarRender r;
            renderBar(in, r);
            const double stepQ = 4.0 / c.stepsPerBar;
            const int mb = ((in.bar % m.len) + m.len) % m.len;
            for (int k = 0; k < r.viewSteps; ++k)
            {
                const double posQ = k * stepQ;
                const double endQ = std::min(posQ + stepQ, in.barLenQ);
                const double onQ = (k & 1) ? posQ + static_cast<double>(c.shuffle) * (endQ - posQ) : posQ;
                int low = -1;
                for (int i = 0; i < r.count; ++i)
                {
                    const Event& e = r.events[static_cast<size_t>(i)];
                    if (e.strand != -1 || e.line != Line::Main || std::abs(e.onQ - onQ) > 1e-6) continue;
                    if (low < 0 || e.note < low) low = e.note;
                }
                const StepView& v = r.view[static_cast<size_t>(k)];
                ++seen;
                if (low >= 0)
                {
                    ++sounding;
                    if (m.slots[static_cast<size_t>(mb * c.stepsPerBar + k)].fn == Fn::Rest)
                        ++(c.part == Part::Stab ? onEmptyStab : onEmptyLine);
                }
                const bool bad = (low >= 0) != (v.note >= 0)
                              || (low >= 0 && low != std::clamp(v.note + c.transpose, 0, 127));
                if (bad && off++ < 4)
                    why += " | t" + std::to_string(t) + " k" + std::to_string(k) + ": plays " + std::to_string(low)
                         + ", view " + std::to_string(v.note);
            }
        }
        check(off == 0, std::to_string(seen) + " steps, " + std::to_string(sounding) + " sounding: the view shows a note exactly where one starts, and that note ("
                        + std::to_string(off) + " off)" + why);
        check(onEmptyLine >= 200 && onEmptyStab >= 50, "coverage: " + std::to_string(onEmptyLine) + " line and "
                        + std::to_string(onEmptyStab) + " Stab notes on steps the motif leaves empty (Fill)");
    }

    std::printf("\n== 41. The view names an embellishment for the note it plays\n");
    {
        // Read from what plays: for each step, the main-line note that starts
        // on its onset, the one the line played before it (on the bar's first
        // step the note the bar enters from), and the head it leads into: the
        // next of the bar, else the first of the next two bars, rendered here
        // (§3.4's lookahead), else none, which leads into the note before
        // (every bar here has a head, so none never occurs).
        // Without Variation (no Lift, Fill, Drop, Re-pitch, Ratchet) a step
        // the view shows as a Repeat plays the note before it, a Neighbour
        // the scale step above or below it. A stored Passing step plays the
        // scale step from the note before towards the head where that lies
        // strictly between them, and the view shows p; where it does not
        // (the head is the note before, or no further from it than that
        // step) the step plays the neighbour away from the head (above
        // without one), and the view showed p over it. And a Lead
        // embellishment after a leap of more than 4 semitones steps back
        // against the leap (Huron's post-skip reversal), whatever the motif
        // stored there: the view showed the stored = or n-arrow over the
        // step back. Half the motifs come from Step: a drawn Lead motif
        // stores no Repeat, an analysed one does wherever a step repeats its
        // note.
        std::mt19937 rng(41);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        static const int spbs[] = { 2, 4, 8, 16 };
        int named = 0, passing = 0, passingRoom = 0, passingFirst = 0, passingLater = 0, passingNoHead = 0;
        int reversals = 0, reversalsRelabelled = 0, repeatReversals = 0, off = 0;
        std::string why;
        for (int t = 0; t < 20000; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 2));
            c.harm = static_cast<Harm>(U(0, 2));
            c.scaleType = U(0, ScaleQuantizer::COUNT - 1);
            c.scaleRoot = U(0, 11);
            c.ext7 = U(0, 1) != 0;
            c.chordDegrees = { U(1, 7), U(0, 7), U(0, 7), U(0, 7) };
            c.stepsPerBar = spbs[U(0, 3)];
            c.pulses = U(1, c.stepsPerBar);
            c.rotation = U(0, c.stepsPerBar - 1);
            c.form = static_cast<Form>(U(0, 3));
            c.variation = 0.0f;
            c.layer = static_cast<Layer>(U(0, 3));
            c.shuffle = 0.25f * static_cast<float>(U(0, 3));
            c.transpose = 0;
            Motif drawn;
            if (U(0, 1))
            {
                std::array<StepInput, kMaxBarSteps> steps {};
                int last = U(55, 79);
                for (int i = 0; i < c.stepsPerBar; ++i)
                {
                    if (U(0, 2) != 0) last = U(55, 79);
                    steps[static_cast<size_t>(i)] = { last, U(0, 1) == 0 || i == 0, false, 0.8f };
                }
                drawn = motifFromSteps(steps.data(), c.stepsPerBar, c, static_cast<std::uint32_t>(41000 + t), 0);
                c.len = drawn.len; c.pulses = drawn.pulses; c.rotation = 0;
            }
            else
            {
                drawn = generateMotif(static_cast<std::uint32_t>(41000 + t), 0, c);
            }
            const Motif m = fitMotif(drawn, c);
            RenderInput in;
            in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m; in.answer = &m;
            in.bar = U(0, 15);
            in.barLenQ = 4.0;
            in.prevNote = U(36, 72); in.prevPrevNote = U(36, 72);
            if (c.harm == Harm::Keys) { in.keys = true; in.keysChord = chordFromKeys(U(36, 60), static_cast<std::uint16_t>(U(1, 4095))); }
            BarRender r;
            renderBar(in, r);
            const std::uint16_t mask = scaleMask(c.scaleType, c.scaleRoot);
            const double stepQ = 4.0 / c.stepsPerBar;
            const int mb = ((in.bar % m.len) + m.len) % m.len;
            auto noteAt = [&](const BarRender& b, int k) {
                const double posQ = k * stepQ;
                const double onQ = (k & 1) ? posQ + static_cast<double>(c.shuffle) * stepQ : posQ;
                int n = -1;
                for (int i = 0; i < b.count; ++i)
                {
                    const Event& e = b.events[static_cast<size_t>(i)];
                    if (e.strand == -1 && e.line == Line::Main && std::abs(e.onQ - onQ) < 1e-6) n = e.note;
                }
                return n;
            };
            // The first head of `b` from step `from`, as it plays; -1 none.
            auto firstHead = [&](const BarRender& b, int from) {
                for (int k2 = from; k2 < b.viewSteps; ++k2)
                    if (b.view[static_cast<size_t>(k2)].head) return noteAt(b, k2);
                return -1;
            };
            std::array<BarRender, 2> next;
            for (int i = 0; i < 2; ++i)
            {
                RenderInput a = in;
                a.bar = in.bar + 1 + i;
                renderBar(a, next[static_cast<size_t>(i)]);
            }
            int prev = in.prevNote, prevPrev = in.prevPrevNote;
            bool firstOnset = true;
            for (int k = 0; k < r.viewSteps; ++k)
            {
                const int note = noteAt(r, k);
                if (note < 0) continue;
                const StepView& v = r.view[static_cast<size_t>(k)];
                const Fn stored = m.slots[static_cast<size_t>(mb * c.stepsPerBar + k)].fn;
                const bool reversal = c.part == Part::Lead && std::abs(prev - prevPrev) > 4
                    && (stored == Fn::Repeat || stored == Fn::Passing
                        || stored == Fn::NeighbourUp || stored == Fn::NeighbourDown);
                std::string bad;
                if (v.tone == Tone::None)
                {
                    const int want = v.fn == Fn::Repeat ? prev
                                   : v.fn == Fn::NeighbourUp ? scaleStep(prev, 1, mask)
                                   : v.fn == Fn::NeighbourDown ? scaleStep(prev, -1, mask) : -1;
                    if (want >= 0)
                    {
                        ++named;
                        if (note != want) bad = "shown " + stepGlyphText(v) + " after " + std::to_string(prev);
                    }
                }
                if (stored == Fn::Passing && !reversal)
                {
                    int head = firstHead(r, k + 1);
                    const bool later = head < 0;
                    if (head < 0) head = firstHead(next[0], 0);
                    if (head < 0) head = firstHead(next[1], 0);
                    const int T = head >= 0 ? head : prev;
                    const int toward = scaleStep(prev, T < prev ? -1 : 1, mask);
                    const bool room = T > prev ? toward < T : T < prev && toward > T;
                    ++passing;
                    passingRoom += room;
                    passingFirst += firstOnset;
                    passingLater += later && head >= 0;
                    passingNoHead += head < 0;
                    const Fn wantFn = room ? Fn::Passing : T < prev ? Fn::NeighbourUp : T > prev ? Fn::NeighbourDown : Fn::NeighbourUp;
                    const int wantNote = room ? toward : scaleStep(prev, wantFn == Fn::NeighbourUp ? 1 : -1, mask);
                    if ((v.fn != wantFn || note != wantNote) && bad.empty())
                        bad = "stored p after " + std::to_string(prev) + " into " + std::to_string(T)
                            + ", shown " + stepGlyphText(v) + ", want " + std::to_string(wantNote);
                }
                if (reversal)
                {
                    ++reversals;
                    if (v.fn != stored) ++reversalsRelabelled;
                    if (stored == Fn::Repeat) ++repeatReversals;
                    if ((prev > prevPrev ? note >= prev : note <= prev) && bad.empty())
                        bad = "no step back after " + std::to_string(prevPrev) + "-" + std::to_string(prev);
                }
                if (!bad.empty() && off++ < 4)
                    why += " | t" + std::to_string(t) + " k" + std::to_string(k) + ": plays " + std::to_string(note) + ", " + bad;
                prevPrev = prev; prev = note;
                firstOnset = false;
            }
        }
        check(off == 0, std::to_string(named) + " Repeat/Neighbour steps, " + std::to_string(passing)
                        + " stored Passing steps, " + std::to_string(reversals)
                        + " Lead steps after a leap: each is named for the note it plays and steps back ("
                        + std::to_string(off) + " off)" + why);
        check(named >= 5000 && passing >= 2500 && passingRoom >= 1500 && passing - passingRoom >= 500
                  && passingFirst >= 30 && passingLater >= 500
                  && reversals >= 500 && reversalsRelabelled >= 200 && repeatReversals >= 50,
              "coverage: " + std::to_string(named) + " named, " + std::to_string(passing) + " stored Passing ("
              + std::to_string(passingRoom) + " with room, " + std::to_string(passingFirst) + " on the bar's first onset, "
              + std::to_string(passingLater) + " into a later bar's head, " + std::to_string(passingNoHead) + " into none), "
              + std::to_string(reversals) + " reversals, "
              + std::to_string(reversalsRelabelled) + " shown other than stored, "
              + std::to_string(repeatReversals) + " of a stored Repeat");
    }

    std::printf("\n== 42. The phrase strip names the form's units and the chords the phrase plays\n");
    {
        // A: the letters written out per form, not read back through
        // isBUnit: each unit's first bar (every N/4 bars) carries its unit's
        // letter, a bar inside a unit none, Loop none at all; the same
        // whichever bar of the phrase plays, a bar before 0 included.
        static const char* kUnits[] = { "", "AAAB", "AABA", "ABAB" };
        int offA = 0, stripsA = 0;
        std::string whyA;
        for (int f = 0; f < 4; ++f)
            for (int nb : { 4, 8, 16 })
                for (int bar : { 0, 1, nb - 1, nb, 3 * nb + 2, -1, -nb - 3 })
                {
                    Config c = base();
                    c.form = static_cast<Form>(f);
                    c.phraseBars = nb;
                    std::array<StripCell, kMaxPhraseBars> cells {};
                    const int n = phraseStrip(c, bar, nullptr, cells);
                    std::string got, want;
                    for (int j = 0; j < nb; ++j)
                    {
                        got += cells[static_cast<size_t>(j)].form != 0 ? cells[static_cast<size_t>(j)].form : '.';
                        want += (f != 0 && j % (nb / 4) == 0) ? kUnits[f][j / (nb / 4)] : '.';
                    }
                    ++stripsA;
                    if ((n != nb || got != want) && offA++ < 4)
                        whyA += " | form " + std::to_string(f) + " N " + std::to_string(nb) + " bar " + std::to_string(bar)
                              + ": " + std::to_string(n) + " cells " + got + ", want " + want;
                }
        check(offA == 0, "A. " + std::to_string(stripsA) + " strips: a letter on each unit's first bar, the form's ("
                         + std::to_string(offA) + " off)" + whyA);

        // B: against what plays. Pedal and Chords, loops of 1 to 4 slots at
        // 1, 2 and 4 bars a chord, so most loops do not divide the phrase and
        // a phrase starts elsewhere in the loop than the one before; from a
        // locate, so the bars are absolute. On each bar the strip of its
        // phrase carries forward to the chord readChord() hands the GUI; that
        // is the chord the bar renders, chordForBar of the absolute bar. A
        // cell names a chord exactly where the
        // phrase's first bar is, or where the bar before played another one.
        // Every bar of a phrase gets the same strip (the view recomputes it
        // only when the phrase changes).
        std::mt19937 rng(42);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        static const int spbs[] = { 4, 8, 16 };
        static const int rates[] = { 1, 2, 4 };
        int bars = 0, offB = 0, offD = 0, offSame = 0, offMark = 0, marks = 0, firstRepeats = 0, phraseShifts = 0;
        std::string whyB;
        auto note = [&](std::string& why, int& off, const std::string& w) { if (off++ < 4) why += " | " + w; };
        for (int t = 0; t < 160; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 3));
            c.harm = U(0, 3) == 0 ? Harm::Pedal : Harm::Chords;
            c.scaleType = U(0, ScaleQuantizer::COUNT - 1);
            c.scaleRoot = U(0, 11);
            c.ext7 = U(0, 1) != 0;
            c.chordDegrees = { U(1, 7), U(0, 7), U(0, 7), U(0, 7) };
            c.chordRateBars = rates[U(0, 2)];
            c.phraseBars = 4 << U(0, 2);
            c.form = static_cast<Form>(U(0, 3));
            c.stepsPerBar = spbs[U(0, 2)];
            c.pulses = U(1, c.stepsPerBar);
            const int tsNum = t % 4 == 0 ? 3 : 4;
            const int N = phraseBarsOf(c);
            const int from = U(0, 40);
            std::map<int, Chord> played;
            int lastBar = std::numeric_limits<int>::min();
            bool rendered = false;   // the hook's first call comes before the first block
            play(c, from * static_cast<double>(tsNum), (from + 2 * N + 3) * static_cast<double>(tsNum), tsNum, 4, {},
                 static_cast<std::uint32_t>(42000 + t), [&](T5ynthPhraseSequencer& s, double) {
                if (!rendered) { rendered = true; return; }
                const int b = s.currentBarForGui.load();
                if (b == lastBar || b < 0) return;
                lastBar = b;
                ++bars;
                const Chord ch = s.readChord();
                played[b] = ch;
                const std::string at = "t" + std::to_string(t) + " bar " + std::to_string(b);
                if (!(ch == chordForBar(c, b)))
                    note(whyB, offD, at + ": readChord " + chordNameText(ch) + ", the bar renders " + chordNameText(chordForBar(c, b)));
                std::array<StripCell, kMaxPhraseBars> cells {}, firstCells {};
                const int j = floorMod(b, N);
                phraseStrip(c, b, nullptr, cells);
                phraseStrip(c, b - j, nullptr, firstCells);
                for (int i = 0; i < N; ++i)
                    if (cells[static_cast<size_t>(i)].form != firstCells[static_cast<size_t>(i)].form
                        || cells[static_cast<size_t>(i)].chord != firstCells[static_cast<size_t>(i)].chord)
                    { note(whyB, offSame, at + ": the strip differs from its phrase's first bar's at cell " + std::to_string(i)); break; }
                std::string carried;
                for (int i = 0; i <= j; ++i)
                    if (!cells[static_cast<size_t>(i)].chord.empty()) carried = cells[static_cast<size_t>(i)].chord;
                if (carried != chordNameText(ch))
                    note(whyB, offB, at + ": the strip carries '" + carried + "' to cell " + std::to_string(j)
                                     + ", the bar plays " + chordNameText(ch));
            });
            for (const auto& [b, ch] : played)
            {
                const int j = floorMod(b, N);
                std::array<StripCell, kMaxPhraseBars> cells {};
                phraseStrip(c, b, nullptr, cells);
                const bool marked = !cells[static_cast<size_t>(j)].chord.empty();
                marks += marked;
                const auto before = played.find(b - 1);
                if (j == 0)
                {
                    if (!marked) note(whyB, offMark, "t" + std::to_string(t) + " bar " + std::to_string(b) + ": the phrase's first bar names no chord");
                    if (before != played.end() && before->second == ch) ++firstRepeats;
                    const auto lastPhrase = played.find(b - N);
                    if (lastPhrase != played.end() && !(lastPhrase->second == ch)) ++phraseShifts;
                }
                else if (before != played.end() && marked != !(before->second == ch))
                    note(whyB, offMark, "t" + std::to_string(t) + " bar " + std::to_string(b) + ": cell "
                                        + std::to_string(j) + (marked ? " names " : " names no ") + chordNameText(ch)
                                        + " after " + chordNameText(before->second));
            }
        }
        check(offD == 0, "B. " + std::to_string(bars) + " bars: readChord() is the chord the bar renders (" + std::to_string(offD) + " off)" + whyB);
        check(offB == 0 && offSame == 0 && offMark == 0,
              "B. the strip carries each bar's played chord, the same on every bar of its phrase, named where it changes ("
                  + std::to_string(offB) + " / " + std::to_string(offSame) + " / " + std::to_string(offMark) + " off)" + whyB);
        check(bars > 2500 && marks > 600 && firstRepeats > 20 && phraseShifts > 40,
              "B. coverage: " + std::to_string(bars) + " bars, " + std::to_string(marks) + " named, " + std::to_string(firstRepeats)
                  + " phrases opening on the chord before, " + std::to_string(phraseShifts) + " phrases opening on another chord than the phrase before");

        // C: Keys. A new key set every two bars, in the middle of its bar
        // (on beat 1.5), so it is delivered between beats: it takes effect
        // on the next beat (§3.2), changing the chord inside the bar, and the
        // bar after enters on it. Before every block readChord() is the chord
        // the key set in effect spells (chordFromKeys of the lowest key and
        // the pitch classes), the set in effect being the last one whose beat
        // lies before the block: a strip one chord change behind fails here.
        // Wherever the bar or the held chord changes (where the view
        // recomputes the strip) the strip names that chord on the playing bar
        // alone; the letters are Chords'.
        static const std::vector<std::vector<int>> keySets = {
            { 45, 48, 52 }, { 41, 45, 48, 51 }, { 43, 50 }, { 36, 40, 43, 49 }, { 38, 41, 45, 48 }, { 40, 44, 47, 50 } };
        auto spell = [](const std::vector<int>& ks) {
            std::uint16_t mask = 0;
            for (int k : ks) mask = static_cast<std::uint16_t>(mask | (1u << (k % 12)));
            return chordFromKeys(*std::min_element(ks.begin(), ks.end()), mask);
        };
        int keysBlocks = 0, keysBars = 0, offC = 0, offS = 0, keysChanges = 0, midBarChanges = 0;
        std::string whyC;
        for (int t = 0; t < 40; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 3));
            c.harm = Harm::Keys;
            c.scaleType = U(0, ScaleQuantizer::COUNT - 1);
            c.scaleRoot = U(0, 11);
            c.phraseBars = 4 << U(0, 2);
            c.form = static_cast<Form>(U(0, 3));
            c.stepsPerBar = spbs[U(0, 2)];
            c.pulses = U(1, c.stepsPerBar);
            const int N = phraseBarsOf(c);
            const int from = U(0, 40);
            std::vector<std::pair<double, std::vector<int>>> keys;
            for (int i = 0; i < 2 * N + 3; i += 2)
                keys.push_back({ (from + i) * 4.0 + (i == 0 ? 0.0 : 1.5),
                                 keySets[static_cast<size_t>(U(0, static_cast<int>(keySets.size()) - 1))] });
            // Each set's beat: the first beat at or after the block play()
            // delivers it before (beats are quarters here).
            std::vector<double> beatOf;
            int lastBar = std::numeric_limits<int>::min();
            Chord lastHeld;
            bool rendered = false;
            play(c, from * 4.0, (from + 2 * N + 3) * 4.0, 4, 4, keys, static_cast<std::uint32_t>(42500 + t),
                 [&](T5ynthPhraseSequencer& s, double q) {
                while (beatOf.size() < keys.size() && keys[beatOf.size()].first <= q) beatOf.push_back(std::ceil(q));
                if (!rendered) { rendered = true; return; }
                const int b = s.currentBarForGui.load();
                if (b < 0) return;
                ++keysBlocks;
                size_t inEffect = 0;
                while (inEffect + 1 < beatOf.size() && beatOf[inEffect + 1] < q) ++inEffect;
                const Chord want = spell(keys[inEffect].second);
                const Chord held = s.readChord();
                const std::string at = "t" + std::to_string(t) + " bar " + std::to_string(b) + " q " + std::to_string(q);
                if (!(held == want))
                    note(whyC, offC, at + ": readChord " + chordNameText(held) + ", the keys in effect spell " + chordNameText(want));
                if (b == lastBar && held == lastHeld) return;
                if (lastBar != std::numeric_limits<int>::min() && !(held == lastHeld))
                {
                    ++keysChanges;
                    midBarChanges += b == lastBar;
                }
                keysBars += b != lastBar;
                lastBar = b;
                lastHeld = held;
                std::array<StripCell, kMaxPhraseBars> cells {}, chords {};
                const int n = phraseStrip(c, b, &held, cells);
                phraseStrip(c, b, nullptr, chords);
                const int j = floorMod(b, N);
                for (int i = 0; i < n; ++i)
                {
                    const auto& x = cells[static_cast<size_t>(i)];
                    const std::string w = i == j ? chordNameText(held) : std::string();
                    if (x.chord != w || x.form != chords[static_cast<size_t>(i)].form)
                    { note(whyC, offS, at + ": cell " + std::to_string(i) + " reads '" + x.chord + "', want '" + w + "'"); break; }
                }
            });
        }
        check(offC == 0, "C. Keys: " + std::to_string(keysBlocks) + " blocks: readChord() is the chord of the keys in effect ("
                         + std::to_string(offC) + " off)" + whyC);
        check(offS == 0, "C. Keys: " + std::to_string(keysBars) + " bars: the strip names the held chord on the playing bar alone ("
                         + std::to_string(offS) + " off)" + whyC);
        check(keysBars > 500 && keysChanges > 100 && midBarChanges > 80,
              "C. coverage: " + std::to_string(keysChanges) + " held-chord changes, " + std::to_string(midBarChanges) + " inside a bar");
    }

    std::printf("\n== 43. A step after a Fill note is resolved against it\n");
    {
        // Variation bars with a Fill, read from what plays, shuffle on in half
        // of them (a step starts on its grid position, an odd one delayed by
        // shuffle of its slot; a run note on a step's position starts where
        // that step would), half in 3/4 or 4/4 and half in a meter from 1/32
        // to 9/2. The last beat is where a step's grid position lies, not its
        // onset: shuffle moves an odd step across a beat the step grid does
        // not meet. Its 16ths are the bar's own, the grid the steps lie on:
        // in n/32, off the 16th grid, the first one after barLenQ − 1. Stab plays the chord on
        // every onset and threads no line: C alone holds for it. A, B and D
        // read the line's notes in the order they sound, the Fill run's notes (72/127) among them, a ratchet's
        // repeats (85/127 x 0.9) not, each unlifted (Lift puts the last beat
        // an octave up, the run with it; Bass's Lift is its Octave pulse). An
        // embellishment step plays against the note that sounds before it,
        // a run note included (§3.4: P is the previous sounding note): a
        // Repeat that note, a Neighbour the scale step above or below it, a
        // Passing step the scale step from it towards the head (§41's rule
        // where none lies between), an Octave that note an octave away; a
        // Lead embellishment after a leap of more than 4 semitones the step
        // back. The head is the next of the bar as it plays, else the first
        // of the next two bars, rendered here. Rendered after the steps,
        // the run left a step after a run note resolved against the step
        // before it. Under shuffle a run 16th inside an odd step's slot lies
        // after the step's grid position but may lie before its delayed
        // onset: it sounds after the step, and the step plays against the
        // note before it, not against that run note.
        // B: a run note on a step's grid position is shown as what it is
        // there: p where it is the scale step from the note before it
        // towards the run's head (the next bars' first, else the note
        // before the last beat), strictly between the two; otherwise the
        // approach into that head, from below or above. Shown p throughout,
        // the run's first note claimed a step from a note it leapt from.
        // C: with shuffle off, the run plays on each 16th of the last beat
        // that no onset of the line takes (a step's or a ratchet repeat's
        // within half a 16th), and on no other (§3.7 op 3). Without Ratchet
        // nothing but the steps takes one: a bar that left out the 16ths a
        // ratchet would have repeated on dropped run notes.
        // D: the run note on the j-th 16th of the last beat lies 4 - j scale
        // steps from the run's head, counted back towards the note before
        // the last beat (the bar's entry note without one), shuffled as the
        // line (inside the slot of its step, it divides what follows the
        // step's onset). With no head ahead the run leads into the note
        // before the last beat (§3.7 op 3). Its slot is its 16th, cut at the
        // bar end as a step's: it sounds Gate of it from its onset.
        std::mt19937 rng(43);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        static const int spbs[] = { 4, 8, 16 };
        const float kTailVel = 85.0f / 127.0f * 0.9f, kFillVel = 72.0f / 127.0f;
        int bars = 0, checked = 0, afterRun = 0, off = 0;
        int afterRunRepeat = 0, afterRunNeighbour = 0, afterRunPassing = 0, afterRunOctave = 0, afterRunReversal = 0;
        int runInSlot = 0;
        int fillChecked = 0, offC = 0, fillWanted = 0, fillRatchet = 0, fillPlainStepInLast = 0;
        std::string whyC;
        int runPitches = 0, offD = 0, runPitchesNoHead = 0, runPitchesShuffled = 0;
        std::string whyD;
        int runLens = 0, offL = 0, runLensCut = 0;
        std::string whyL;
        int runShown = 0, offB = 0, runPasses = 0, runRises = 0, runFalls = 0, runAfterStep = 0, runNoHead = 0;
        std::string why, whyB;
        int oddMeterBars = 0, oddMeterChecked = 0, oddMeterRuns = 0, stabFillBars = 0;
        static const int dens[] = { 2, 4, 8, 16, 32 };
        int n32FillBars = 0, n32Runs = 0;
        for (int t = 0; t < 30000; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 3));
            c.harm = static_cast<Harm>(U(0, 2));
            c.scaleType = U(0, ScaleQuantizer::COUNT - 1);
            c.scaleRoot = U(0, 11);
            c.ext7 = U(0, 1) != 0;
            c.chordDegrees = { U(1, 7), U(0, 7), U(0, 7), U(0, 7) };
            c.stepsPerBar = spbs[U(0, 2)];
            c.pulses = U(1, c.stepsPerBar);
            c.rotation = U(0, c.stepsPerBar - 1);
            c.phraseBars = 4 << U(0, 2);
            c.form = static_cast<Form>(U(0, 3));
            c.variation = 0.2f * static_cast<float>(U(3, 5));
            c.layer = static_cast<Layer>(U(0, 3));
            c.shuffle = U(0, 1) ? 0.0f : 0.25f * static_cast<float>(U(1, 3));
            c.transpose = 0;
            Motif drawn;
            if (U(0, 1))
            {
                std::array<StepInput, kMaxBarSteps> steps {};
                int last = U(55, 79);
                for (int i = 0; i < c.stepsPerBar; ++i)
                {
                    if (U(0, 2) != 0) last = U(55, 79);
                    steps[static_cast<size_t>(i)] = { last, U(0, 2) != 0 || i == 0, false, 0.8f };
                }
                drawn = motifFromSteps(steps.data(), c.stepsPerBar, c, static_cast<std::uint32_t>(43000 + t), 0);
                c.len = drawn.len; c.pulses = drawn.pulses; c.rotation = 0;
            }
            else
            {
                drawn = generateMotif(static_cast<std::uint32_t>(43000 + t), 0, c);
            }
            const Motif m = fitMotif(drawn, c);
            std::array<bool, kMaxMotifSteps> heads {};
            computeHeads(m, heads);
            const int N = phraseBarsOf(c);
            std::vector<int> fillBars;
            for (int b = 0; b < N; ++b)
                if (variationForBar(m, heads, c, b).fill) fillBars.push_back(b);
            if (fillBars.empty()) continue;
            RenderInput in;
            in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m; in.answer = &m;
            in.bar = fillBars[static_cast<size_t>(U(0, static_cast<int>(fillBars.size()) - 1))];
            if (U(0, 1))
            {
                in.barLenQ = U(0, 3) == 0 ? 3.0 : 4.0;
            }
            else
            {
                const int num = U(1, 9);
                in.barLenQ = 4.0 * num / dens[U(0, 4)];
            }
            const bool oddMeter = in.barLenQ != 3.0 && in.barLenQ != 4.0;
            in.prevNote = U(36, 72); in.prevPrevNote = U(36, 72);
            if (c.harm == Harm::Keys) { in.keys = true; in.keysChord = chordFromKeys(U(36, 60), static_cast<std::uint16_t>(U(1, 4095))); }
            BarRender r;
            renderBar(in, r);
            ++bars;
            const std::uint16_t mask = scaleMask(c.scaleType, c.scaleRoot);
            const double stepQ = 4.0 / c.stepsPerBar;
            const double lastBeatQ = std::max(0.0, in.barLenQ - 1.0);
            const int mb = ((in.bar % m.len) + m.len) % m.len;
            oddMeterBars += oddMeter;
            const bool n32 = std::abs(in.barLenQ * 4.0 - std::round(in.barLenQ * 4.0)) > 1e-9;
            // The bar's 16ths in its last beat, in order.
            std::vector<double> last16;
            for (int i = 0; 0.25 * i < in.barLenQ - 1e-9; ++i)
                if (0.25 * i >= lastBeatQ - 1e-9) last16.push_back(0.25 * i);
            const bool bass = c.part == Part::Bass;
            // The step a note starts on: its grid position, an odd step's
            // delayed by shuffle of its slot; -1 none.
            auto slotEndOf = [&](int k2) { return std::min((k2 + 1) * stepQ, in.barLenQ); };
            auto stepAt = [&](double q) {
                for (int k2 = 0; k2 < r.viewSteps; ++k2)
                {
                    const double pos = k2 * stepQ;
                    const double on = (k2 & 1) == 0 ? pos : pos + c.shuffle * (slotEndOf(k2) - pos);
                    if (std::abs(on - q) < 1e-9) return k2;
                }
                return -1;
            };
            // Where a note of this bar lies on the straight grid: a step's
            // position, a run note's (all in the last beat) its onset.
            auto gridQOf = [&](const Event* e) {
                const int k2 = std::abs(e->vel - kFillVel) < 1e-4 ? -1 : stepAt(e->onQ);
                return k2 >= 0 ? k2 * stepQ : e->onQ;
            };
            // A note as played without Lift, by its grid position q.
            auto unlifted = [&](const BarRender& b, int bar, double q, int note) {
                const bool lift = variationForBar(m, heads, c, bar).lift;
                return lift && !bass && q >= lastBeatQ - 1e-9 ? note - 12 : note;
            };
            // The first head of `b` from step `from`, as it plays; -1 none.
            auto firstHead = [&](const BarRender& b, int bar, int from) {
                for (int k2 = from; k2 < b.viewSteps; ++k2)
                {
                    const StepView& v = b.view[static_cast<size_t>(k2)];
                    if (v.head && v.note >= 0) return unlifted(b, bar, k2 * stepQ, v.note);
                }
                return -1;
            };
            std::array<BarRender, 2> next;
            for (int i = 0; i < 2; ++i)
            {
                RenderInput a = in;
                a.bar = in.bar + 1 + i;
                renderBar(a, next[static_cast<size_t>(i)]);
            }
            const BarVariation var = variationForBar(m, heads, c, in.bar);
            if (c.shuffle == 0.0f)
            {
                auto isMain = [](const Event& e) { return e.strand == -1 && e.line == Line::Main; };
                std::vector<double> want, got;
                for (const double q : last16)
                {
                    bool taken = false;
                    for (int i = 0; i < r.count; ++i)
                    {
                        const Event& e = r.events[static_cast<size_t>(i)];
                        taken = taken || (isMain(e) && std::abs(e.vel - kFillVel) > 1e-4 && std::abs(e.onQ - q) < 0.125 - 1e-9);
                    }
                    if (!taken) want.push_back(q);
                }
                bool stepInLast = false;
                for (int i = 0; i < r.count; ++i)
                {
                    const Event& e = r.events[static_cast<size_t>(i)];
                    if (!isMain(e)) continue;
                    if (std::abs(e.vel - kFillVel) < 1e-4) got.push_back(e.onQ);
                    else stepInLast = stepInLast || e.onQ >= lastBeatQ - 1e-9;
                }
                std::sort(got.begin(), got.end());
                // Stab: one onset per chord.
                if (c.part == Part::Stab)
                    got.erase(std::unique(got.begin(), got.end(), [](double a, double b) { return std::abs(a - b) < 1e-9; }), got.end());
                stabFillBars += c.part == Part::Stab;
                n32FillBars += n32;
                ++fillChecked;
                fillWanted += static_cast<int>(want.size());
                fillRatchet += var.ratchet;
                fillPlainStepInLast += !var.ratchet && stepInLast;
                bool same = want.size() == got.size();
                for (size_t i = 0; same && i < want.size(); ++i) same = std::abs(want[i] - got[i]) < 1e-9;
                if (!same && offC++ < 4)
                {
                    auto list = [](const std::vector<double>& v) {
                        std::string o;
                        for (double q : v) o += (o.empty() ? "" : " ") + std::to_string(q).substr(0, 5);
                        return o.empty() ? std::string("none") : o;
                    };
                    whyC += " | t" + std::to_string(t) + " bar " + std::to_string(in.bar) + " len " + std::to_string(in.barLenQ).substr(0, 5)
                          + (c.part == Part::Stab ? " Stab" : "") + (var.ratchet ? " ratchet" : "")
                          + ": run at " + list(got) + ", free 16ths " + list(want);
                }
            }
            if (c.part == Part::Stab) continue;
            std::vector<const Event*> line;
            for (int i = 0; i < r.count; ++i)
            {
                const Event& e = r.events[static_cast<size_t>(i)];
                if (e.strand == -1 && e.line == Line::Main && std::abs(e.vel - kTailVel) > 1e-4) line.push_back(&e);
            }
            std::stable_sort(line.begin(), line.end(), [](const Event* a, const Event* b) { return a->onQ < b->onQ; });
            // The run's head: the next bars' first, else the note before the
            // last beat.
            int before = in.prevNote;
            for (const Event* e : line)
                if (gridQOf(e) < lastBeatQ - 1e-9) before = unlifted(r, in.bar, gridQOf(e), e->note);
            int runHead = firstHead(next[0], in.bar + 1, 0);
            if (runHead < 0) runHead = firstHead(next[1], in.bar + 2, 0);
            const bool noHead = runHead < 0;
            if (noHead)
            {
                ++runNoHead;
                runHead = before;
            }
            {
                const int dir = runHead >= before ? -1 : 1;
                for (const Event* e : line)
                {
                    if (std::abs(e->vel - kFillVel) > 1e-4) continue;
                    int j = -1;
                    for (int j2 = 0; j2 < static_cast<int>(last16.size()) && j < 0; ++j2)
                    {
                        const double q = last16[static_cast<size_t>(j2)];
                        const int k2 = static_cast<int>(std::floor((q + 1e-9) / stepQ));
                        const double slotQ = k2 * stepQ, slotEndQ = slotEndOf(k2);
                        const double slotOnQ = (k2 & 1) == 0 ? slotQ : slotQ + c.shuffle * (slotEndQ - slotQ);
                        if (std::abs(slotOnQ + (q - slotQ) * (slotEndQ - slotOnQ) / (slotEndQ - slotQ) - e->onQ) < 1e-9) j = j2;
                    }
                    if (j >= 0)
                    {
                        const double q = last16[static_cast<size_t>(j)];
                        const double gate = std::clamp(static_cast<double>(c.gate), 0.1, 1.0);
                        const double wantLen = std::max(1.0e-3, gate * std::min(0.25, in.barLenQ - q));
                        ++runLens;
                        runLensCut += in.barLenQ - q < 0.25 - 1e-9;
                        if (std::abs(e->gateOffQ - e->onQ - wantLen) > 1e-9 && offL++ < 4)
                            whyL += " | t" + std::to_string(t) + " bar " + std::to_string(in.bar) + " len "
                                  + std::to_string(in.barLenQ).substr(0, 5) + " 16th at " + std::to_string(q).substr(0, 5)
                                  + ": sounds " + std::to_string(e->gateOffQ - e->onQ).substr(0, 6) + ", want "
                                  + std::to_string(wantLen).substr(0, 6);
                    }
                    const int u = unlifted(r, in.bar, gridQOf(e), e->note);
                    oddMeterRuns += oddMeter;
                    n32Runs += n32;
                    const int want = j < 0 ? -1 : scaleSteps(runHead, dir * (4 - j), mask);
                    ++runPitches;
                    runPitchesNoHead += noHead;
                    runPitchesShuffled += c.shuffle > 0.0f;
                    if (u != want && offD++ < 4)
                        whyD += " | t" + std::to_string(t) + " bar " + std::to_string(in.bar) + " at " + std::to_string(e->onQ).substr(0, 6)
                              + (j < 0 ? std::string(": on no 16th of the last beat") : ": 16th " + std::to_string(j))
                              + ", from " + std::to_string(before) + " into " + std::to_string(runHead) + (noHead ? " (no head ahead)" : "")
                              + ": plays " + std::to_string(u) + ", want " + std::to_string(want);
                }
            }
            int P = in.prevNote, PP = in.prevPrevNote;
            bool pIsRun = false;
            for (const Event* e : line)
            {
                const int u = unlifted(r, in.bar, gridQOf(e), e->note);
                if (std::abs(e->vel - kFillVel) < 1e-4)
                {
                    const int k = stepAt(e->onQ);
                    if (k >= 0)
                    {
                        const StepView& v = r.view[static_cast<size_t>(k)];
                        const int d = runHead > P ? 1 : -1;
                        const bool passes = P != runHead && u == scaleStep(P, d, mask) && (d > 0 ? u < runHead : u > runHead);
                        const Fn wantFn = passes ? Fn::Passing : u < runHead ? Fn::ApproachUp : Fn::ApproachDown;
                        ++runShown;
                        runPasses += passes;
                        runRises += wantFn == Fn::ApproachUp;
                        runFalls += wantFn == Fn::ApproachDown;
                        runAfterStep += !pIsRun;
                        if ((v.note != e->note || v.fn != wantFn) && offB++ < 4)
                        {
                            StepView want = v;
                            want.fn = wantFn;
                            whyB += " | t" + std::to_string(t) + " bar " + std::to_string(in.bar) + " k" + std::to_string(k)
                                  + ": run note " + std::to_string(u) + " after " + std::to_string(P) + (pIsRun ? " (a run note)" : "")
                                  + " into " + std::to_string(runHead) + ": shown " + std::to_string(v.note) + " " + stepGlyphText(v)
                                  + ", want " + std::to_string(e->note) + " " + stepGlyphText(want);
                        }
                    }
                    PP = P; P = u; pIsRun = true;
                    continue;
                }
                const int k = stepAt(e->onQ);
                const StepView& v = r.view[static_cast<size_t>(std::max(k, 0))];
                std::string bad;
                if (k < 0)
                    bad = "an onset on no step's onset at " + std::to_string(e->onQ);
                else if (v.tone == Tone::None)
                {
                    const int s = mb * c.stepsPerBar + k;
                    const Fn stored = s == var.repitchStep ? var.repitchFn : m.slots[static_cast<size_t>(s)].fn;
                    int T = firstHead(r, in.bar, k + 1);
                    if (T < 0) T = firstHead(next[0], in.bar + 1, 0);
                    if (T < 0) T = firstHead(next[1], in.bar + 2, 0);
                    if (T < 0) T = P;
                    const bool reversal = c.part == Part::Lead && std::abs(P - PP) > 4
                        && (stored == Fn::Repeat || stored == Fn::Passing
                            || stored == Fn::NeighbourUp || stored == Fn::NeighbourDown);
                    Fn wantFn = Fn::Rest;
                    int want = -1;
                    if (v.fn == Fn::Octave)
                    {
                        wantFn = Fn::Octave;
                        want = std::abs(u - P) == 12 ? u : P + 12;
                        afterRunOctave += pIsRun;
                    }
                    else if (reversal)
                    {
                        wantFn = P > PP ? Fn::NeighbourDown : Fn::NeighbourUp;
                        want = scaleStep(P, P > PP ? -1 : 1, mask);
                        afterRunReversal += pIsRun;
                    }
                    else if (stored == Fn::Repeat) { wantFn = Fn::Repeat; want = P; afterRunRepeat += pIsRun; }
                    else if (stored == Fn::NeighbourUp || stored == Fn::NeighbourDown)
                    {
                        wantFn = stored;
                        want = scaleStep(P, stored == Fn::NeighbourUp ? 1 : -1, mask);
                        afterRunNeighbour += pIsRun;
                    }
                    else if (stored == Fn::Passing)
                    {
                        const int toward = scaleStep(P, T < P ? -1 : 1, mask);
                        const bool room = T > P ? toward < T : T < P && toward > T;
                        wantFn = room ? Fn::Passing : T < P ? Fn::NeighbourUp : T > P ? Fn::NeighbourDown : Fn::NeighbourUp;
                        want = room ? toward : scaleStep(P, wantFn == Fn::NeighbourUp ? 1 : -1, mask);
                        afterRunPassing += pIsRun;
                    }
                    StepView storedView;
                    storedView.note = 0;
                    storedView.fn = stored;
                    if (want >= 0)
                    {
                        ++checked;
                        oddMeterChecked += oddMeter;
                        afterRun += pIsRun;
                        // A run note sounds after this step inside its slot.
                        runInSlot += c.shuffle > 0.0f && std::any_of(line.begin(), line.end(), [&](const Event* f) {
                            return std::abs(f->vel - kFillVel) < 1e-4 && f->onQ > e->onQ + 1e-9 && f->onQ < slotEndOf(k) - 1e-9;
                        });
                        if (u != want || v.fn != wantFn)
                            bad = "stored " + stepGlyphText(storedView) + " after " + std::to_string(P)
                                + (pIsRun ? " (a run note)" : "") + " into " + std::to_string(T) + ": plays "
                                + std::to_string(u) + " shown " + stepGlyphText(v) + ", want " + std::to_string(want);
                    }
                }
                if (!bad.empty() && off++ < 4)
                    why += " | t" + std::to_string(t) + " bar " + std::to_string(in.bar) + " k" + std::to_string(k) + ": " + bad;
                PP = P; P = u; pIsRun = false;
            }
        }
        check(off == 0, std::to_string(checked) + " embellishment steps in " + std::to_string(bars)
                        + " Fill bars play against the note before them (" + std::to_string(off) + " off)" + why);
        check(afterRun > 2000 && afterRunRepeat > 100 && afterRunNeighbour > 200 && afterRunPassing > 200
                  && afterRunOctave > 50 && afterRunReversal > 100 && runInSlot > 1000,
              "coverage: " + std::to_string(afterRun) + " after a run note: " + std::to_string(afterRunRepeat) + " Repeat, "
                  + std::to_string(afterRunNeighbour) + " Neighbour, " + std::to_string(afterRunPassing) + " Passing, "
                  + std::to_string(afterRunOctave) + " Octave, " + std::to_string(afterRunReversal) + " reversals; "
                  + std::to_string(runInSlot) + " shuffled steps with a run note after them in their slot");
        check(offB == 0, "B. " + std::to_string(runShown) + " run notes on a step are shown p where they pass into the head, "
                         "an approach otherwise (" + std::to_string(offB) + " off)" + whyB);
        check(runPasses > 0 && runRises > 0 && runFalls > 0 && runAfterStep > 0 && runNoHead > 0,
              "B. coverage: " + std::to_string(runPasses) + " p, " + std::to_string(runRises) + " rising, "
                  + std::to_string(runFalls) + " falling, " + std::to_string(runAfterStep) + " after a step, "
                  + std::to_string(runNoHead) + " bars without a head ahead");
        check(offC == 0, "C. " + std::to_string(fillChecked) + " Fill bars without shuffle: the run plays on every free 16th of the "
                         "last beat and on no other (" + std::to_string(offC) + " off)" + whyC);
        check(fillWanted > 10000 && fillRatchet > 500 && fillPlainStepInLast > 2000,
              "C. coverage: " + std::to_string(fillWanted) + " free 16ths, " + std::to_string(fillRatchet) + " bars with Ratchet, "
                  + std::to_string(fillPlainStepInLast) + " without it and a step in the last beat");
        check(oddMeterChecked > 10000 && oddMeterRuns > 10000 && stabFillBars > 1000,
              "coverage: " + std::to_string(oddMeterBars) + " bars in a meter other than 3/4 and 4/4: "
                  + std::to_string(oddMeterChecked) + " embellishment steps, " + std::to_string(oddMeterRuns) + " run notes; "
                  + std::to_string(stabFillBars) + " Stab bars without shuffle");
        check(n32FillBars > 500 && n32Runs > 1000,
              "coverage: bars in n/32, off the 16th grid: " + std::to_string(n32FillBars) + " Fill bars without shuffle, "
                  + std::to_string(n32Runs) + " run notes");
        check(offD == 0, "D. " + std::to_string(runPitches) + " run notes lie 4 - j scale steps from the head, counted back towards "
                         "the note before the last beat (" + std::to_string(offD) + " off)" + whyD);
        check(offL == 0, "D. " + std::to_string(runLens) + " run notes sound Gate of their 16th, cut at the bar end ("
                             + std::to_string(offL) + " off)" + whyL);
        check(runLensCut > 300, "D. coverage: " + std::to_string(runLensCut) + " run notes on a 16th the bar end cuts");
        check(runPitchesNoHead > 200 && runPitchesShuffled > 10000,
              "D. coverage: " + std::to_string(runPitchesNoHead) + " into the note before the last beat (no head ahead), "
                  + std::to_string(runPitchesShuffled) + " shuffled");
    }

    std::printf("\n== 44. A ratchet's hits divide what is left of its slot, each sounding Gate of its own\n");
    {
        // Ratchet (§3.7 op 4): the step sounds 2 or 3 times, its hits
        // dividing what is left of its slot after its (shuffled) onset, the
        // slot cut at the bar end as a step's is; each hit sounds by the
        // step-length rule (§3.6), Gate of its own sub-step. The render
        // carries that as the note's own off (gateOffQ), the length a change
        // lets a note sound when it takes the note's bounding onset away
        // (scenario 24, which plays no variation). A repeat's own length was
        // never checked: doubled, it passed every scenario. Oracle from what
        // plays: the main line's repeats (their velocity) and the step onset
        // before them, the hit; its slot from the step the hit lies in.
        // Variation 4/5 (Re-pitch, Lift, Fill, Ratchet), every part and
        // layer, 1/2 to 1/16, Gate 0.1 to 1, shuffle on in half, in half the
        // trials a bar from 1/32 to 9/2.
        std::mt19937 rng(44);
        auto U = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        const float kTailVel = 85.0f / 127.0f * 0.9f, kFillVel = 72.0f / 127.0f;
        static const int spbs[] = { 2, 4, 8, 16 };
        static const int dens[] = { 2, 4, 8, 16, 32 };
        int ratchets = 0, threes = 0, shuffled = 0, cut = 0, oddMeter = 0, offOn = 0, offLen = 0;
        std::string why;
        auto fail = [&](int& n, int t, const std::string& what) {
            if (n++ < 4) why += " | t " + std::to_string(t) + ": " + what;
        };
        for (int t = 0; t < 20000; ++t)
        {
            Config c = base();
            c.part = static_cast<Part>(U(0, 3));
            c.harm = U(0, 1) ? Harm::Chords : Harm::Pedal;
            c.chordDegrees = { U(1, 7), U(1, 7), U(1, 7), U(1, 7) };
            c.stepsPerBar = spbs[U(0, 3)];
            c.pulses = U(1, c.stepsPerBar);
            c.rotation = U(0, c.stepsPerBar - 1);
            c.len = U(1, 2);
            c.phraseBars = 8;
            c.form = static_cast<Form>(U(0, 3));
            c.variation = 0.8f;
            c.layer = static_cast<Layer>(U(0, 3));
            c.shuffle = U(0, 1) ? 0.0f : 0.25f * static_cast<float>(U(1, 3));
            c.gate = 0.1f * static_cast<float>(U(1, 10));
            const Motif m = fitMotif(generateMotif(static_cast<std::uint32_t>(44000 + t), 0, c), c);
            RenderInput in;
            in.cfg = &c; in.motif = &m; in.nextPhraseMotif = &m; in.answer = &m;
            in.bar = 7;   // the last of phrase 0: varied in every form
            in.barLenQ = U(0, 1) ? (U(0, 3) == 0 ? 3.0 : 4.0) : 4.0 * U(1, 9) / dens[U(0, 4)];
            in.prevNote = U(36, 72); in.prevPrevNote = U(36, 72);
            BarRender r;
            renderBar(in, r);

            // The main line's onsets in order, each with its events (Stab: a voice each).
            std::map<long long, std::vector<const Event*>> byOn;
            for (int e = 0; e < r.count; ++e)
                if (r.events[static_cast<size_t>(e)].strand == -1)
                    byOn[std::llround(r.events[static_cast<size_t>(e)].onQ * 1.0e9)].push_back(&r.events[static_cast<size_t>(e)]);
            // Fill notes (their velocity) left out: a run plays on the free
            // 16ths, inside a ratchet's slot too (scenario 43 C).
            std::vector<std::vector<const Event*>> on;
            for (auto& [q, evs] : byOn)
                if (std::abs(evs.front()->vel - kFillVel) >= 1.0e-4f) on.push_back(evs);
            auto isRepeat = [&](const std::vector<const Event*>& evs) {
                return std::abs(evs.front()->vel - kTailVel) < 1.0e-4f;
            };
            int first = -1, repeats = 0;
            for (int i = 0; i < static_cast<int>(on.size()); ++i)
                if (isRepeat(on[static_cast<size_t>(i)])) { if (first < 0) first = i; ++repeats; }
            if (first < 0) continue;
            const std::string at = "bar " + std::to_string(in.barLenQ) + "q spb " + std::to_string(c.stepsPerBar) + " gate "
                                 + std::to_string(c.gate) + " shuffle " + std::to_string(c.shuffle);
            if (first == 0) { fail(offOn, t, "a repeat with no hit before it: " + at); continue; }
            ++ratchets;
            const int hits = repeats + 1;
            threes += hits == 3;
            oddMeter += in.barLenQ != 3.0 && in.barLenQ != 4.0;
            const double hitOn = on[static_cast<size_t>(first - 1)].front()->onQ;
            const double stepQ = 4.0 / c.stepsPerBar;
            const double posQ = std::floor(hitOn / stepQ + 1.0e-9) * stepQ;
            const double slotEnd = std::min(posQ + stepQ, in.barLenQ);
            shuffled += hitOn > posQ + 1.0e-9;
            cut += slotEnd < posQ + stepQ - 1.0e-9;
            const double hitQ = (slotEnd - hitOn) / hits;
            const double own = std::max(1.0e-3, std::clamp(static_cast<double>(c.gate), 0.1, 1.0) * hitQ);
            if (hits > 3) fail(offOn, t, std::to_string(hits) + " hits: " + at);
            for (int h = 0; h < hits; ++h)
            {
                const int i = first - 1 + h;
                if (i >= static_cast<int>(on.size()) || (h > 0 && !isRepeat(on[static_cast<size_t>(i)])))
                {
                    fail(offOn, t, "the repeats are not the onsets after the hit: " + at);
                    break;
                }
                for (const Event* e : on[static_cast<size_t>(i)])
                {
                    const double want = hitOn + h * hitQ;
                    if (std::abs(e->onQ - want) > 1.0e-9)
                        fail(offOn, t, "hit " + std::to_string(h) + " at " + std::to_string(e->onQ) + ", want "
                                           + std::to_string(want) + ": " + at);
                    if (std::abs(e->gateOffQ - e->onQ - own) > 1.0e-9 || e->offQ - e->onQ > own + 1.0e-9)
                        fail(offLen, t, "hit " + std::to_string(h) + " at " + std::to_string(e->onQ) + " sounds "
                                            + std::to_string(e->offQ - e->onQ) + ", own " + std::to_string(e->gateOffQ - e->onQ)
                                            + ", want " + std::to_string(own) + ": " + at);
                }
            }
            const int after = first - 1 + hits;
            if (after < static_cast<int>(on.size()) && on[static_cast<size_t>(after)].front()->onQ < slotEnd - 1.0e-9)
                fail(offOn, t, "an onset inside the ratchet's slot after its last hit: " + at);
        }
        check(ratchets > 5000 && threes > 1000 && shuffled > 500 && cut > 200 && oddMeter > 2000,
              "coverage: " + std::to_string(ratchets) + " ratchets, " + std::to_string(threes) + " of 3 hits, "
                  + std::to_string(shuffled) + " on a shuffled onset, " + std::to_string(cut) + " in a slot the bar end cuts, "
                  + std::to_string(oddMeter) + " in a meter other than 3/4 and 4/4");
        check(offOn == 0, "the hits divide what is left of the slot after the (shuffled) onset (" + std::to_string(offOn)
                              + " off)" + why);
        check(offLen == 0, "each hit sounds Gate of its own sub-step, and that is its own length (" + std::to_string(offLen)
                               + " off)" + why);
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASS" : "FAILURES", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
