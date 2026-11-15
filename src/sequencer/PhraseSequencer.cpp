#include "PhraseSequencer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

using namespace PhraseModel;

namespace
{
constexpr double kEpsQ = 1.0e-9;
constexpr double kJumpQ = 0.01;   // host position mismatch that counts as a locate

std::uint32_t packSlot(const Slot& s)
{
    return static_cast<std::uint32_t>(s.fn)
         | (static_cast<std::uint32_t>(s.member & 3u) << 4)
         | (static_cast<std::uint32_t>(std::clamp(static_cast<int>(s.octave), -2, 5) + 2) << 6)
         | (s.accent ? 1u << 9 : 0u)
         | (s.slide  ? 1u << 10 : 0u);
}

Slot unpackSlot(std::uint32_t w)
{
    Slot s;
    const std::uint32_t fn = w & 15u;
    s.fn = fn < static_cast<std::uint32_t>(Fn::Count) ? static_cast<Fn>(fn) : Fn::Rest;
    s.member = static_cast<std::uint8_t>((w >> 4) & 3u);
    s.octave = static_cast<std::int8_t>(static_cast<int>((w >> 6) & 7u) - 2);
    s.accent = ((w >> 9) & 1u) != 0;
    s.slide  = ((w >> 10) & 1u) != 0;
    return s;
}

// A view step in one word: note + 1 (bits 0-7), fn (8-11), head, accent,
// slide (12-14), tone (15-17), octave + 4 (18-20), label (21-24).
static_assert(static_cast<int>(Fn::Count) <= 16 && static_cast<int>(Tone::Count) <= 8
                  && static_cast<int>(ToneLabel::Count) <= 16,
              "a view step's fields fit their bits");

std::uint32_t packView(const StepView& v)
{
    return static_cast<std::uint32_t>(std::clamp(v.note + 1, 0, 128))
         | (static_cast<std::uint32_t>(v.fn) << 8)
         | (v.head   ? 1u << 12 : 0u)
         | (v.accent ? 1u << 13 : 0u)
         | (v.slide  ? 1u << 14 : 0u)
         | (static_cast<std::uint32_t>(v.tone) << 15)
         | (static_cast<std::uint32_t>(std::clamp(v.octave, -4, 3) + 4) << 18)
         | (static_cast<std::uint32_t>(v.label) << 21);
}

std::uint32_t packChord(const Chord& c)
{
    auto iv = [&](int m) { return static_cast<std::uint32_t>(c.iv[static_cast<size_t>(m)] + 1) & 31u; };
    return (static_cast<std::uint32_t>(c.rootPc) & 15u) | (iv(1) << 4) | (iv(2) << 9) | (iv(3) << 14);
}

/** The time signature the sequencer runs in: numerator 1..32, a denominator
 *  that is a power of two up to 32, any other one 4. */
void normaliseTimeSignature(int& numerator, int& denominator)
{
    numerator = std::clamp(numerator, 1, 32);
    if (denominator != 1 && denominator != 2 && denominator != 4 && denominator != 8
        && denominator != 16 && denominator != 32)
        denominator = 4;
}
} // namespace

T5ynthPhraseSequencer::T5ynthPhraseSequencer()
{
    for (auto& w : viewWords) w.store(0, std::memory_order_relaxed);
    heldNote.fill(-1);
    publish(stored);
}

void T5ynthPhraseSequencer::prepare(double sr)
{
    sampleRate = sr > 0.0 ? sr : 44100.0;
}

// ─── Audio thread: configuration ───────────────────────────────────────────

void T5ynthPhraseSequencer::setConfig(const Config& c)
{
    if (!cfgValid)
    {
        cfg = c;
        cfgValid = true;
        latchedEvolve = c.evolve;
        motifDirty = renderDirty = true;
        return;
    }
    // Evolve is latched at phrase entry (§3.8): turning it changes the next
    // phrase, not the bar that is playing (processBlock re-renders the bar
    // for the line into the next phrase).
    Config a = cfg, b = c;
    a.evolve = b.evolve = 0.0f;
    if (a != b)
    {
        renderDirty = true;
        if (!sameMotifInputs(a, b)) motifDirty = true;
    }
    cfg = c;
}

void T5ynthPhraseSequencer::setTimeSignature(int numerator, int denominator)
{
    int num = numerator, den = denominator;
    normaliseTimeSignature(num, den);
    if (num == tsNum && den == tsDen) return;
    tsNum = num;
    tsDen = den;
    barLenQ = 4.0 * num / den;
    positioned = false;   // the bar grid moved: re-enter at the next block
    renderDirty = true;   // stopped: the bar the view shows has another length
}

Config T5ynthPhraseSequencer::latchedConfig() const
{
    Config c = cfg;
    c.evolve = latchedEvolve;
    return c;
}

// ─── Audio thread: keys ────────────────────────────────────────────────────

void T5ynthPhraseSequencer::keyOn(int note)
{
    if (note < 0 || note > 127 || keyIsDown[static_cast<size_t>(note)]) return;
    if (keysDown == 0)
    {
        // The first key after all keys were up starts a new chord.
        keysMask = 0;
        keysLowest = 128;
    }
    keyIsDown[static_cast<size_t>(note)] = 1;
    ++keysDown;
    keysMask = static_cast<std::uint16_t>(keysMask | (1u << (note % 12)));
    keysLowest = std::min(keysLowest, note);
    keysPending = chordFromKeys(keysLowest, keysMask);
    keysChanged = true;
}

void T5ynthPhraseSequencer::keyOff(int note)
{
    if (note < 0 || note > 127 || !keyIsDown[static_cast<size_t>(note)]) return;
    keyIsDown[static_cast<size_t>(note)] = 0;
    keysDown = std::max(0, keysDown - 1);   // the latch keeps the chord
}

void T5ynthPhraseSequencer::clearKeys()
{
    keyIsDown.fill(0);
    keysDown = 0;
    keysMask = 0;
    keysLowest = 128;
    keysChanged = false;
    keysEverSet = false;
}

Chord T5ynthPhraseSequencer::currentKeysChord() const
{
    // Nothing played yet: the tonic chord until the first chord arrives.
    return keysEverSet ? keysChord : chordForDegree(0, cfg.scaleType, cfg.scaleRoot, cfg.ext7);
}

// ─── Audio thread: transport ───────────────────────────────────────────────

void T5ynthPhraseSequencer::start()
{
    if (running) return;
    running = true;
    posQ = 0.0;
    positioned = false;
    evoValid = false;
}

void T5ynthPhraseSequencer::stop()
{
    running = false;
    positioned = false;
    renderDirty = true;   // the view goes to the first bar, where start() enters
    editOrigin.store(kOriginAtNextEntry, std::memory_order_relaxed);
    currentStepForGui.store(-1, std::memory_order_relaxed);
    currentPhraseForGui.store(0, std::memory_order_relaxed);
}

void T5ynthPhraseSequencer::emitOff(int note, int strand, std::vector<VoiceEvent>& out, int offset)
{
    VoiceEvent v;
    v.sampleOffset = offset;
    v.type = VoiceEvent::Type::NoteOff;
    v.note = note;
    v.velocity = 0.0f;
    v.strandId = strand;
    // Never dropped: a lost note-off hangs a voice. Pushed like the step and
    // generative sequencers' offs, into the capacity the processor reserves.
    out.push_back(v);
}

void T5ynthPhraseSequencer::allNotesOff(std::vector<VoiceEvent>& out, int sampleOffset)
{
    for (int i = 0; i < pendingCount; ++i)
        emitOff(pending[static_cast<size_t>(i)].note, pending[static_cast<size_t>(i)].strand, out, sampleOffset);
    pendingCount = 0;
    for (int s = 0; s < kStrandSlots; ++s)
    {
        if (heldNote[static_cast<size_t>(s)] >= 0)
            emitOff(heldNote[static_cast<size_t>(s)], s - 1, out, sampleOffset);
        heldNote[static_cast<size_t>(s)] = -1;
    }
}

void T5ynthPhraseSequencer::flushPendingFor(int note, int strand, std::vector<VoiceEvent>& out, int offset)
{
    // A pending off for the very note about to start would end the new note.
    for (int i = 0; i < pendingCount;)
    {
        auto& p = pending[static_cast<size_t>(i)];
        if (p.note == note && p.strand == strand)
        {
            emitOff(p.note, p.strand, out, offset);
            p = pending[static_cast<size_t>(--pendingCount)];
        }
        else ++i;
    }
}

void T5ynthPhraseSequencer::addPending(double q, double ownQ, int note, int strand, Line line,
                                       std::vector<VoiceEvent>& out, int offset)
{
    if (pendingCount >= kMaxPending)
    {
        // Full (cannot happen at the model's event counts): end the earliest
        // now rather than lose any off.
        int earliest = 0;
        for (int i = 1; i < pendingCount; ++i)
            if (pending[static_cast<size_t>(i)].q < pending[static_cast<size_t>(earliest)].q) earliest = i;
        emitOff(pending[static_cast<size_t>(earliest)].note, pending[static_cast<size_t>(earliest)].strand, out, offset);
        pending[static_cast<size_t>(earliest)] = pending[static_cast<size_t>(--pendingCount)];
    }
    pending[static_cast<size_t>(pendingCount++)] = { q, ownQ, note, strand, line };
}

bool T5ynthPhraseSequencer::sharesOnset(int index) const
{
    // The events are sorted by onset, so the voices of one onset are neighbours.
    const Event& e = render.events[static_cast<size_t>(index)];
    for (int i = index - 1; i >= 0 && render.events[static_cast<size_t>(i)].onQ == e.onQ; --i)
        if (render.events[static_cast<size_t>(i)].strand == e.strand) return true;
    for (int i = index + 1; i < render.count && render.events[static_cast<size_t>(i)].onQ == e.onQ; ++i)
        if (render.events[static_cast<size_t>(i)].strand == e.strand) return true;
    return false;
}

void T5ynthPhraseSequencer::emitOn(int index, std::vector<VoiceEvent>& out, int offset)
{
    const Event& e = render.events[static_cast<size_t>(index)];
    const int slot = slotOf(e.strand);
    if (slot < 0 || slot >= kStrandSlots) return;
    flushPendingFor(e.note, e.strand, out, offset);

    VoiceEvent v;
    v.sampleOffset = offset;
    v.type = VoiceEvent::Type::NoteOn;
    v.note = e.note;
    v.velocity = std::clamp(e.vel, 0.0f, 1.0f);
    v.strandId = e.strand;
    v.pan = 0.0f;   // centre: panning is the musician's mix decision (§3.10)
    // A held note is continued by its strand's next onset (TB-303 slide, as
    // the step sequencer): that onset glides the live voice and its later
    // note-off names the new note. An onset of several voices on the strand
    // has no one voice to continue: the held note ends there and the voices
    // start fresh. Only Stab's chord is such an onset (met after a Part switch
    // from a sliding line); a monophonic line never puts two notes on one
    // tick, and a layer plays on strands of its own. The
    // processor's sort keeps no order among note-ons of one tick, and the
    // voice manager glides the newest voice of the source, so a chord voice
    // started on the same tick could take the glide and leave the held voice
    // without a note-off.
    const int held = heldNote[static_cast<size_t>(slot)];
    heldNote[static_cast<size_t>(slot)] = -1;
    const bool glide = held >= 0 && !sharesOnset(index);
    if (held >= 0 && !glide) emitOff(held, e.strand, out, offset);
    if (out.size() >= out.capacity())
    {
        // No room: the onset is dropped rather than allocate on the audio
        // thread. A voice it would have continued ends here instead of hanging.
        if (glide) emitOff(held, e.strand, out, offset);
        return;
    }
    v.artic = glide ? VoiceEvent::Articulation::Glide : VoiceEvent::Articulation::Normal;
    out.push_back(v);

    if (e.held) heldNote[static_cast<size_t>(slot)] = e.note;
    else        addPending(barStartQ + e.offQ, barStartQ + e.gateOffQ, e.note, e.strand, e.line, out, offset);
}

void T5ynthPhraseSequencer::skipOn(int index, std::vector<VoiceEvent>& out, int offset)
{
    // The held note on the strand would have been continued by this note and
    // ended at its off, or ended at its onset (a chord's). Both lie in this
    // sample: the held note ends here.
    const Event& e = render.events[static_cast<size_t>(index)];
    const int slot = slotOf(e.strand);
    if (slot < 0 || slot >= kStrandSlots) return;
    const int held = heldNote[static_cast<size_t>(slot)];
    if (held < 0) return;
    heldNote[static_cast<size_t>(slot)] = -1;
    emitOff(held, e.strand, out, offset);
}

void T5ynthPhraseSequencer::releaseUnusedHeld(int fromIndex, std::vector<VoiceEvent>& out, int offset)
{
    // A held note waits for its strand's next onset: one the (re)rendered bar
    // has left on that strand, or the next bar's first when the bar as
    // rendered now still ends held on it (entering that bar releases the
    // note when it has none). When neither — the layer was switched off, the
    // motif changed — it is released here instead of hanging. Without the
    // second, a re-render between the bar's last onset and the next downbeat
    // cut the slide into that downbeat.
    for (int s = 0; s < kStrandSlots; ++s)
    {
        if (heldNote[static_cast<size_t>(s)] < 0) continue;
        bool used = false;
        for (int i = fromIndex; i < render.count && !used; ++i)
            used = slotOf(render.events[static_cast<size_t>(i)].strand) == s;
        for (int i = fromIndex - 1; i >= 0 && !used; --i)
            if (slotOf(render.events[static_cast<size_t>(i)].strand) == s)
            {
                used = render.events[static_cast<size_t>(i)].held;
                break;
            }
        if (!used)
        {
            emitOff(heldNote[static_cast<size_t>(s)], s - 1, out, offset);
            heldNote[static_cast<size_t>(s)] = -1;
        }
    }
}

void T5ynthPhraseSequencer::endPendingByNextOnset(int fromIndex)
{
    // A note ends at its own off (gate x its step) or at its line's next
    // onset, whichever comes first (finishLine). Its render bounded it by the
    // onsets THAT render had. A re-render in the bar can put its line's next
    // onset sooner — a denser grid, another motif — and the note would sound
    // on under the new render's notes: it ends at that onset instead. Its
    // line is the one the renderer bounds it by (Event::line): the Octave and
    // Chord layers end at the main line's next onset, the Answer at its own.
    //
    // The bound starts from the note's own off each time, not from the last
    // bound nor from the off its render gave it: a note still pending has not
    // reached any earlier bound, so the onset that set it has not played, and
    // the render now playing may no longer have it (a sparser grid, a second
    // change before that onset, a knob dragged across values). Only the render
    // now playing says where the note ends. With no onset of its line left in
    // this bar, the bar entry bounds it by the next bar's first.
    //
    // It also ends at its strand's next onset, whatever line that is: a layer
    // switch puts another line on the strand (the Answer and the Octave and
    // Chord layers all play on strand 5), and a note of the old line would
    // sound on under the new line's note on its own strand. Within one render
    // a strand carries one line, and the line's bound is the stricter one.
    constexpr double inf = std::numeric_limits<double>::infinity();
    double nextMain = inf, nextAnswer = inf;
    std::array<double, kStrandSlots> nextOnStrand;
    nextOnStrand.fill(inf);
    for (int i = fromIndex; i < render.count; ++i)
    {
        const Event& e = render.events[static_cast<size_t>(i)];
        const double on = barStartQ + e.onQ;   // sorted by onset: the first is the next
        double& next = e.line == Line::Answer ? nextAnswer : nextMain;
        if (next == inf) next = on;
        const int slot = slotOf(e.strand);
        if (slot >= 0 && slot < kStrandSlots && nextOnStrand[static_cast<size_t>(slot)] == inf)
            nextOnStrand[static_cast<size_t>(slot)] = on;
    }
    for (int i = 0; i < pendingCount; ++i)
    {
        auto& p = pending[static_cast<size_t>(i)];
        const int slot = slotOf(p.strand);
        const double strandNext = slot >= 0 && slot < kStrandSlots ? nextOnStrand[static_cast<size_t>(slot)] : inf;
        p.q = std::min({ p.ownQ, p.line == Line::Answer ? nextAnswer : nextMain, strandNext });
    }
}

// ─── Evolution cache ───────────────────────────────────────────────────────

void T5ynthPhraseSequencer::rebuildEvolution(int phrase)
{
    // The phrase before the phrase last entered played with the Evolve it
    // latched (prevEvolve). Any other phrase, or another phrase length that
    // numbers them anew, has no phrase before that played as such: as after
    // a locate it derives with the Evolve latched last. A length that comes
    // back within the phrase last entered numbers them as they played.
    const float before = phrase == entryPhrase && phraseBarsOf(cfg) == entryBars ? prevEvolve : latchedEvolve;
    const Config L = latchedConfig();
    evoPrev = evolvedMotif(fitted, L, phrase - 1);
    evoCur = evoPrev;
    if (phrase > fitted.origin) evolveOnce(evoCur, L, phrase);
    if (before != latchedEvolve)
    {
        // A phrase's motif is the origin's evolved with its own latch
        // (deriveNext): the line entered from that one, and Keep keeps it as
        // the lead-in.
        Config B = L;
        B.evolve = before;
        evoPrev = evolvedMotif(fitted, B, phrase - 1);
    }
    deriveNext(phrase);
    if (leadsInto(phrase)) evoPrev = fittedLeadIn;
    evoPhrase = phrase;
    evoValid = true;
    publishHeard();
}

void T5ynthPhraseSequencer::advanceEvolution(int phrase)
{
    if (evoValid && phrase == evoPhrase) return;
    if (!evoValid || phrase != evoPhrase + 1) { rebuildEvolution(phrase); return; }
    evoPrev = leadsInto(phrase) ? fittedLeadIn : evoCur;
    evoCur = evoNext;
    deriveNext(phrase);
    evoPhrase = phrase;
    publishHeard();
}

void T5ynthPhraseSequencer::deriveNext(int phrase)
{
    // The next phrase latches Evolve as it stands at its entry (§3.8), and
    // with another value than this phrase's it derives its motif from the
    // origin with that value (enterBar): the line leads into that motif.
    // processBlock re-derives it when Evolve turns, so a phrase entry always
    // finds it derived with the value it latches.
    if (cfg.evolve == latchedEvolve)
    {
        evoNext = evoCur;
        if (phrase + 1 > fitted.origin) evolveOnce(evoNext, cfg, phrase + 1);
    }
    else
    {
        evoNext = evolvedMotif(fitted, cfg, phrase + 1);
    }
    evoNextEvolve = cfg.evolve;
}

bool T5ynthPhraseSequencer::leadsInto(int phrase) const
{
    return fitted.leadInKept && fitted.origin == phrase && fitted.leadInPhrase == phrase;
}

void T5ynthPhraseSequencer::publishHeard()
{
    // Only what plays is heard. Stopped, the channel keeps the phrase last
    // played (Keep after a stop), and a motif stored since stays unheard (Keep
    // does nothing, its origin stays open) although the view renders it.
    if (!running) return;
    Motif h = evoCur;
    h.leadInKept = true;
    h.leadIn = evoPrev;
    h.leadInPhrase = evoPhrase;
    heard.write(h, toAudioSeen, static_cast<std::uint32_t>(evoPhrase));
}

// ─── Bars ──────────────────────────────────────────────────────────────────

void T5ynthPhraseSequencer::renderCurrent()
{
    const int N = phraseBarsOf(cfg);
    const int p = floorDiv(bar, N);
    const Motif& cur = evoCur;
    const Config L = latchedConfig();

    Motif answer;
    const bool wantAnswer = cfg.layer == Layer::Answer && cfg.part != Part::Stab;
    if (wantAnswer) answer = answerMotif(cur, L);

    RenderInput in;
    in.cfg = &L;
    in.motif = &cur;
    in.nextPhraseMotif = &evoNext;
    in.answer = wantAnswer ? &answer : nullptr;
    in.bar = bar;
    in.barLenQ = barLenQ;
    in.keys = cfg.harm == Harm::Keys;
    in.keysChord = currentKeysChord();
    // The line enters from bar − 1 rendered on its own, so bar b sounds the
    // same whether reached by playing or by a locate, and no drift carries
    // on from bar to bar (a line continued live climbs without end where no
    // head pulls it back). The last bar of the phrase before plays evoPrev
    // (or the kept lead-in) on into this phrase. Keys renders bar − 1 with
    // the chord it ended on; the voicing goes on live, voice-led from the
    // chord before (it stays near its anchor).
    const bool firstBar = floorDiv(bar - 1, N) != p;
    int ep = -1, epp = -1;
    entryNotes(L, firstBar ? evoPrev : cur, firstBar ? cur : evoNext, bar, barLenQ,
               in.keys ? &entryChord : nullptr, scratch, ep, epp);
    in.prevNote = ep;
    in.prevPrevNote = epp;
    if (in.keys)
    {
        in.prevVoicing = entryVoicingCount > 0 ? entryVoicing.data() : nullptr;
        in.prevVoicingCount = entryVoicingCount;
    }
    in.slideIn = heldNote[static_cast<size_t>(slotOf(-1))] >= 0;
    renderBar(in, render);
    publishView();
    chordForGui.store(packChord(render.chord), std::memory_order_relaxed);
    currentBarForGui.store(bar, std::memory_order_relaxed);
    currentPhraseForGui.store(p, std::memory_order_relaxed);
    // Stopped, an edit anchors where playback next starts (stop()), not at
    // the bar the view shows.
    if (running) editOrigin.store(p, std::memory_order_relaxed);
}

void T5ynthPhraseSequencer::enterBar(int newBar, bool continuous)
{
    if (continuous)
    {
        entryChord = render.chord;   // the chord bar − 1 ended on
        entryVoicingCount = render.lastVoicingCount;
        for (int v = 0; v < entryVoicingCount; ++v)
            entryVoicing[static_cast<size_t>(v)] = render.lastVoicing[static_cast<size_t>(v)];
    }
    else
    {
        entryVoicingCount = 0;
    }

    bar = newBar;
    barStartQ = static_cast<double>(bar) * barLenQ;

    const int N = phraseBarsOf(cfg);
    const int p = floorDiv(bar, N);
    if (!continuous || !evoValid)
    {
        latchedEvolve = prevEvolve = cfg.evolve;
        entryPhrase = p;
        entryBars = N;
        rebuildEvolution(p);
    }
    else
    {
        // A phrase entry latches Evolve (§3.8). The motif the phrase plays is
        // already derived with it (processBlock re-derives the next phrase's
        // when Evolve turns), and the phrase before stays the one that
        // played: the line enters from its last bar (§3.9), and Keep keeps
        // it as the lead-in. Rebuilt with the new value, the entry came from
        // a bar that never sounded; a rebuild inside the phrase therefore
        // derives the phrase before with the value it latched (prevEvolve).
        if (p != evoPhrase)
        {
            prevEvolve = latchedEvolve;
            entryPhrase = p;
            entryBars = N;
        }
        if (floorMod(bar, N) == 0) latchedEvolve = cfg.evolve;
        advanceEvolution(p);
    }

    // A bar start is a beat: a pending Keys chord applies here at the latest.
    if (keysChanged)
    {
        keysChord = keysPending;
        keysEverSet = true;
        keysChanged = false;
    }
    // A locate has no bar before playing into it: the chord held at it. A
    // chord taking effect later in the bar does not change where it entered.
    if (!continuous) entryChord = currentKeysChord();

    renderCurrent();
    nextEvent = 0;
    if (continuous) barStartFlag.store(true, std::memory_order_relaxed);
}

// ─── processBlock ──────────────────────────────────────────────────────────

void T5ynthPhraseSequencer::takeStoredMotif()
{
    if (toAudio.sequence() == toAudioSeen) return;
    Motif m;
    std::uint32_t unusedA = 0, unusedB = 0, seqRead = 0;
    if (toAudio.tryRead(m, unusedA, unusedB, seqRead))
    {
        liveMotif = m;
        toAudioSeen = seqRead;
        motifDirty = renderDirty = true;
    }
}

bool T5ynthPhraseSequencer::stoppedViewStale() const
{
    // Evolve is latched at a bar entry, not marked by setConfig.
    return motifDirty || renderDirty || keysChanged || cfg.evolve != latchedEvolve;
}

bool T5ynthPhraseSequencer::viewPendingWhileStopped(const Config& c, int numerator, int denominator) const
{
    if (running) return false;
    if (!cfgValid || c != cfg) return true;
    normaliseTimeSignature(numerator, denominator);
    return numerator != tsNum || denominator != tsDen
        || toAudio.sequence() != toAudioSeen || stoppedViewStale();
}

void T5ynthPhraseSequencer::updateStoppedView()
{
    // Stopped, the view shows the bar playback starts in on the sequencer's
    // own transport, the first one (start() counts from 0), as start() enters
    // it. A motif stored while stopped gets its origin where it is first
    // heard; the view shows it heard at the first bar's phrase, and liveMotif
    // keeps the open origin for the start to resolve. Nothing is heard here:
    // publishHeard and the edit origin stay as playback left them.
    if (running || !cfgValid) return;
    takeStoredMotif();
    if (!stoppedViewStale()) return;
    Motif m = liveMotif;
    if (m.origin == kOriginAtNextEntry) m.origin = floorDiv(0, phraseBarsOf(cfg));
    fitted = fitMotif(m, cfg);
    if (fitted.leadInKept) fittedLeadIn = fitMotif(leadInMotif(fitted), cfg);
    enterBar(0, false);
    motifDirty = renderDirty = false;
}

void T5ynthPhraseSequencer::processBlock(int numSamples, double bpm, bool hostSync, double hostPpq,
                                         std::vector<VoiceEvent>& out)
{
    if (numSamples <= 0 || !cfgValid) return;

    if (!running)
    {
        updateStoppedView();
        return;
    }
    takeStoredMotif();
    if (!(bpm > 0.0)) return;

    const double dq = static_cast<double>(numSamples) * bpm / (60.0 * sampleRate);
    const double q0 = hostSync ? hostPpq : posQ;
    const double q1 = q0 + dq;
    auto offsetAt = [&](double q) {
        const double f = (q - q0) / dq * static_cast<double>(numSamples);
        return std::clamp(static_cast<int>(f), 0, numSamples - 1);
    };
    // Where this block takes over playback: every onset before it lies
    // behind, played by the blocks before or before the point a start or
    // locate enters at. A locate counts an onset within rounding before its
    // point as at it, so a locate onto a step plays it. Not this block's
    // start: a host may report a block a little before where the last one
    // ended (under kJumpQ), and when that is more than a block, the next
    // block starts behind onsets already played too.
    double resumeQ = playedQ;
    // The first event not yet emitted once playback has reached q. The loop
    // below emits every onset before the time it reaches (t < q1), so the
    // same strict test resumes exactly behind the last onset played and never
    // repeats one that lies just before q.
    auto firstEventNotPlayed = [&](double q) {
        int i = 0;
        while (i < render.count && barStartQ + render.events[static_cast<size_t>(i)].onQ < q) ++i;
        return i;
    };

    if (hostSync != lastHostSync) positioned = false;
    if (positioned && hostSync && std::abs(q0 - expectedQ) > kJumpQ) positioned = false;
    lastHostSync = hostSync;

    if (liveMotif.origin == kOriginAtNextEntry)
    {
        // Stored while stopped: the motif starts evolving from the phrase
        // where it is first heard — the bar this block enters, or the bar
        // playing (§3.8).
        const int heardBar = positioned ? bar : static_cast<int>(std::floor(q0 / barLenQ));
        liveMotif.origin = floorDiv(heardBar, phraseBarsOf(cfg));
        motifDirty = renderDirty = true;
    }

    if (motifDirty)
    {
        fitted = fitMotif(liveMotif, cfg);
        if (fitted.leadInKept) fittedLeadIn = fitMotif(leadInMotif(fitted), cfg);
        evoValid = false;
    }

    if (!positioned)
    {
        // Start or locate: silence what sounds and enter the bar at q0.
        allNotesOff(out, 0);
        enterBar(static_cast<int>(std::floor(q0 / barLenQ)), false);
        resumeQ = q0 - kEpsQ;
        nextEvent = firstEventNotPlayed(resumeQ);
        positioned = true;
    }
    else if (renderDirty || (evoValid && evoNextEvolve != cfg.evolve))
    {
        // A change inside the bar: re-render it and play on from where
        // playback has reached (a host may jitter q0 around that by < kJumpQ).
        // Evolve turned: the next phrase plays another motif, which the line
        // leads into.
        if (motifDirty) rebuildEvolution(floorDiv(bar, phraseBarsOf(cfg)));
        else if (evoNextEvolve != cfg.evolve) deriveNext(evoPhrase);
        renderCurrent();
        nextEvent = firstEventNotPlayed(resumeQ);
        releaseUnusedHeld(nextEvent, out, 0);
        endPendingByNextOnset(nextEvent);
    }
    motifDirty = renderDirty = false;

    const double beatQ = 4.0 / tsDen;
    while (true)
    {
        constexpr double inf = std::numeric_limits<double>::infinity();
        double tOff = inf;
        for (int i = 0; i < pendingCount; ++i) tOff = std::min(tOff, pending[static_cast<size_t>(i)].q);
        const double tOn = nextEvent < render.count
                         ? barStartQ + render.events[static_cast<size_t>(nextEvent)].onQ : inf;
        const double tBar = barStartQ + barLenQ;
        double tBeat = inf;
        if (keysChanged)
        {
            if (cfg.harm == Harm::Keys)
            {
                // The first beat playback has not passed, from where it has
                // reached, not from where the host reports this block: a
                // report a little off (under kJumpQ) must not move the change
                // to another beat. An onset before resumeQ has played, so a
                // beat counts as passed exactly when it lies before resumeQ,
                // even by a hair (a host accumulating its position ends a
                // block a hair past a beat). Beats are multiples of beatQ, a
                // power of two: the division and the ceiling are exact.
                tBeat = std::ceil(std::max(resumeQ, barStartQ) / beatQ) * beatQ;
            }
            else
            {
                keysChord = keysPending;   // not heard in this source: adopt silently
                keysEverSet = true;
                keysChanged = false;
            }
        }

        const double t = std::min({ tOff, tOn, tBar, tBeat });
        if (!(t < q1)) break;
        const int offset = offsetAt(t);

        if (tOff <= t)
        {
            // Offs first, so a note-off and a note-on on one tick keep order.
            for (int i = 0; i < pendingCount;)
            {
                if (pending[static_cast<size_t>(i)].q <= t)
                {
                    emitOff(pending[static_cast<size_t>(i)].note, pending[static_cast<size_t>(i)].strand, out, offset);
                    pending[static_cast<size_t>(i)] = pending[static_cast<size_t>(--pendingCount)];
                }
                else ++i;
            }
            continue;
        }
        if (tBar <= t)
        {
            enterBar(bar + 1, true);
            releaseUnusedHeld(0, out, offset);
            // A note sounding on into this bar (a shuffled step's off past
            // the bar end) was bounded by its render's view of this bar's
            // first onset. A re-render after the note started (a change late
            // in the bar before) bounds it only by the onsets left in that
            // bar, by none when it came after the last: this bar as rendered
            // now bounds it.
            endPendingByNextOnset(0);
            continue;
        }
        if (tBeat <= t)
        {
            keysChord = keysPending;
            keysEverSet = true;
            keysChanged = false;
            renderCurrent();
            // The chord takes effect at the beat, which lies at or after
            // where playback reached: the onsets from it on have not played.
            nextEvent = firstEventNotPlayed(t);
            releaseUnusedHeld(nextEvent, out, offset);
            endPendingByNextOnset(nextEvent);
            continue;
        }
        // A note needs a sample to sound in. An onset in time the host skipped
        // (a block reported a little ahead of where playback reached) starts
        // late, on the block's first sample; a note whose off falls in that
        // same sample has nothing left to sound. Its on and off would share a
        // tick, where the processor puts offs before ons: the off would end
        // nothing and the on would hang. It is not started.
        const Event& e = render.events[static_cast<size_t>(nextEvent)];
        const double offQ = barStartQ + e.offQ;
        if (!e.held && offQ < q1 && offsetAt(offQ) <= offset) skipOn(nextEvent, out, offset);
        else emitOn(nextEvent, out, offset);
        ++nextEvent;
    }

    if (!hostSync) posQ = q1;
    expectedQ = q1;
    playedQ = std::max(resumeQ, q1);

    const int spb = std::clamp(cfg.stepsPerBar, 1, kMaxBarSteps);
    const double stepQ = 4.0 / spb;
    const int step = static_cast<int>(std::floor((q1 - barStartQ) / stepQ));
    currentStepForGui.store(step >= 0 && step < render.viewSteps ? step : -1, std::memory_order_relaxed);
}

float T5ynthPhraseSequencer::getGlideTime(double bpm) const
{
    if (!(bpm > 0.0)) return 10.0f;
    const int spb = std::clamp(cfg.stepsPerBar, 1, kMaxBarSteps);
    const double stepMs = (60.0 / bpm) * (4.0 / spb) * 1000.0;
    return std::clamp(static_cast<float>(stepMs * 0.25), 10.0f, 200.0f);
}

// ─── GUI view ──────────────────────────────────────────────────────────────

void T5ynthPhraseSequencer::publishView()
{
    for (int k = 0; k < kMaxBarSteps; ++k)
        viewWords[static_cast<size_t>(k)].store(packView(render.view[static_cast<size_t>(k)]),
                                                std::memory_order_relaxed);
    viewSteps.store(render.viewSteps, std::memory_order_relaxed);
    viewGeneration.fetch_add(1, std::memory_order_release);
}

int T5ynthPhraseSequencer::readView(std::array<StepView, kMaxBarSteps>& outView) const
{
    (void) viewGeneration.load(std::memory_order_acquire);
    const int n = std::clamp(viewSteps.load(std::memory_order_relaxed), 0, kMaxBarSteps);
    for (int k = 0; k < kMaxBarSteps; ++k)
    {
        const std::uint32_t w = viewWords[static_cast<size_t>(k)].load(std::memory_order_relaxed);
        StepView v;
        v.note = static_cast<int>(w & 255u) - 1;
        const std::uint32_t fn = (w >> 8) & 15u;
        v.fn = fn < static_cast<std::uint32_t>(Fn::Count) ? static_cast<Fn>(fn) : Fn::Rest;
        v.head   = ((w >> 12) & 1u) != 0;
        v.accent = ((w >> 13) & 1u) != 0;
        v.slide  = ((w >> 14) & 1u) != 0;
        const std::uint32_t tone = (w >> 15) & 7u;
        v.tone   = tone < static_cast<std::uint32_t>(Tone::Count) ? static_cast<Tone>(tone) : Tone::None;
        v.octave = static_cast<int>((w >> 18) & 7u) - 4;
        const std::uint32_t label = (w >> 21) & 15u;
        v.label  = label < static_cast<std::uint32_t>(ToneLabel::Count) ? static_cast<ToneLabel>(label) : ToneLabel::None;
        outView[static_cast<size_t>(k)] = v;
    }
    return n;
}

Chord T5ynthPhraseSequencer::readChord() const
{
    const std::uint32_t w = chordForGui.load(std::memory_order_relaxed);
    Chord c;
    c.rootPc = static_cast<int>(w & 15u);
    for (int m = 1; m < kMaxVoices; ++m)
        c.iv[static_cast<size_t>(m)] = static_cast<int>((w >> (4 + 5 * (m - 1))) & 31u) - 1;
    return c;
}

// ─── Motif hand-off ────────────────────────────────────────────────────────

T5ynthPhraseSequencer::MotifChannel::MotifChannel()
{
    for (auto& w : words) w.store(0, std::memory_order_relaxed);
}

void T5ynthPhraseSequencer::MotifChannel::write(const Motif& m, std::uint32_t tagA, std::uint32_t tagB)
{
    // Writers serialised by the caller. Odd sequence = write in progress.
    const std::uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    // Words: the motif's slots, the lead-in's slots, then the header —
    // 0..5 the motif's shape, 6..11 the lead-in's, 12 origin, 13 lead-in
    // kept, 14 lead-in phrase, 15..16 the tags.
    auto put = [&](int i, std::uint32_t v) { words[static_cast<size_t>(i)].store(v, std::memory_order_relaxed); };
    auto putBody = [&](const MotifBody& b, int slotsAt, int shapeAt) {
        for (int i = 0; i < kMaxMotifSteps; ++i) put(slotsAt + i, packSlot(b.slots[static_cast<size_t>(i)]));
        put(shapeAt + 0, static_cast<std::uint32_t>(b.len));
        put(shapeAt + 1, static_cast<std::uint32_t>(b.stepsPerBar));
        put(shapeAt + 2, static_cast<std::uint32_t>(b.pulses));
        put(shapeAt + 3, static_cast<std::uint32_t>(b.rotation));
        put(shapeAt + 4, static_cast<std::uint32_t>(b.part));
        put(shapeAt + 5, b.seed);
    };
    constexpr int H = 2 * kMaxMotifSteps;
    putBody(m, 0, H);
    putBody(m.leadIn, kMaxMotifSteps, H + 6);
    put(H + 12, static_cast<std::uint32_t>(m.origin));
    put(H + 13, m.leadInKept ? 1u : 0u);
    put(H + 14, static_cast<std::uint32_t>(m.leadInPhrase));
    put(H + 15, tagA);
    put(H + 16, tagB);
    static_assert(kHeader == 17, "header words as written above");
    seq.store(s + 2, std::memory_order_release);
}

bool T5ynthPhraseSequencer::MotifChannel::tryRead(Motif& m, std::uint32_t& tagA, std::uint32_t& tagB,
                                                  std::uint32_t& seqRead) const
{
    const std::uint32_t s1 = seq.load(std::memory_order_acquire);
    if (s1 & 1u) return false;
    std::array<std::uint32_t, kWords> w {};
    for (int i = 0; i < kWords; ++i)
        w[static_cast<size_t>(i)] = words[static_cast<size_t>(i)].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq.load(std::memory_order_relaxed) != s1) return false;

    auto get = [&](int i) { return w[static_cast<size_t>(i)]; };
    auto getBody = [&](MotifBody& b, int slotsAt, int shapeAt) {
        for (int i = 0; i < kMaxMotifSteps; ++i) b.slots[static_cast<size_t>(i)] = unpackSlot(get(slotsAt + i));
        b.len         = std::clamp(static_cast<int>(get(shapeAt + 0)), 0, 2);
        b.stepsPerBar = std::clamp(static_cast<int>(get(shapeAt + 1)), 1, kMaxBarSteps);
        b.pulses      = static_cast<int>(get(shapeAt + 2));
        b.rotation    = static_cast<int>(get(shapeAt + 3));
        b.part        = static_cast<Part>(std::clamp(static_cast<int>(get(shapeAt + 4)), 0, 3));
        b.seed        = get(shapeAt + 5);
    };
    constexpr int H = 2 * kMaxMotifSteps;
    getBody(m, 0, H);
    getBody(m.leadIn, kMaxMotifSteps, H + 6);
    m.origin       = static_cast<int>(get(H + 12));
    m.leadInKept   = get(H + 13) != 0;
    m.leadInPhrase = static_cast<int>(get(H + 14));
    tagA = get(H + 15);
    tagB = get(H + 16);
    seqRead = s1;
    return true;
}

void T5ynthPhraseSequencer::publish(const Motif& m)
{
    toAudio.write(m, 0, 0);
}

bool T5ynthPhraseSequencer::readHeard(Motif& m, int& phrase, std::uint32_t storedSeq) const
{
    std::uint32_t source = 0, ph = 0, unusedSeq = 0;
    // The audio thread writes this channel only around a refit or a bar
    // crossing, a few words at a time: a read that catches it mid-write
    // succeeds on a retry.
    while (!heard.tryRead(m, source, ph, unusedSeq))
        std::this_thread::yield();
    if (source != storedSeq) return false;   // from another stored motif
    phrase = static_cast<int>(ph);
    return true;
}

// ─── Message thread: the stored motif ──────────────────────────────────────

int T5ynthPhraseSequencer::resolvedStoredOrigin() const
{
    if (stored.origin != kOriginAtNextEntry) return stored.origin;
    Motif h;
    int phrase = 0;
    return readHeard(h, phrase, toAudio.sequence()) ? h.origin : kOriginAtNextEntry;
}

void T5ynthPhraseSequencer::adoptResolvedOrigin()
{
    stored.origin = resolvedStoredOrigin();
}

Motif T5ynthPhraseSequencer::getStoredMotif() const
{
    // Any thread: a host may save its state off the message thread, so this
    // reads the published copy rather than `stored`, and resolves the origin
    // against the very publication it read.
    Motif m;
    std::uint32_t unusedA = 0, unusedB = 0, seqRead = 0;
    while (!toAudio.tryRead(m, unusedA, unusedB, seqRead))
        std::this_thread::yield();
    if (m.origin == kOriginAtNextEntry)
    {
        Motif h;
        int phrase = 0;
        if (readHeard(h, phrase, seqRead)) m.origin = h.origin;
    }
    return m;
}

void T5ynthPhraseSequencer::setStoredMotif(const Motif& m)
{
    const std::lock_guard<std::mutex> lock(editMutex);
    stored = m;
    hasUndo.store(false, std::memory_order_relaxed);
    publish(stored);
}

void T5ynthPhraseSequencer::dice(const Config& c, std::uint32_t newSeed)
{
    const std::lock_guard<std::mutex> lock(editMutex);
    adoptResolvedOrigin();
    undoMotif = stored;
    hasUndo.store(true, std::memory_order_relaxed);
    stored = generateMotif(newSeed, editOrigin.load(std::memory_order_relaxed), c);
    publish(stored);
}

void T5ynthPhraseSequencer::keep()
{
    const std::lock_guard<std::mutex> lock(editMutex);
    Motif m;
    int phrase = 0;
    if (!readHeard(m, phrase, toAudio.sequence())) return;
    // m.origin is the stored motif's origin as the audio thread resolved it
    // (fitMotif carries it). A phrase before it played the motif unevolved:
    // the origin stays, running or stopped, or the phrases up to it would
    // start evolving. From the origin on, running: the heard motif anchored
    // at its own phrase plays on exactly as before, even when the next phrase
    // has begun meanwhile. Stopped: it starts from the phrase where playback
    // resumes.
    // The heard motif carries the motif its phrase was entered from as its
    // lead-in: kept with it, the origin phrase enters as without the Keep,
    // in this config and every later one, although the phrase before it now
    // plays the kept motif itself. The lead-in names that phrase, so after a
    // stop it applies only when playback resumes there; anywhere else the
    // phrase before leads in. Before the origin the motif stays as stored,
    // its own lead-in included.
    if (phrase >= m.origin)
    {
        m.origin = editOrigin.load(std::memory_order_relaxed) == kOriginAtNextEntry
                       ? kOriginAtNextEntry
                       : phrase;
    }
    else
    {
        m.leadInKept = stored.leadInKept;
        m.leadIn = stored.leadIn;
        m.leadInPhrase = stored.leadInPhrase;
    }
    adoptResolvedOrigin();
    undoMotif = stored;
    hasUndo.store(true, std::memory_order_relaxed);
    stored = m;
    publish(stored);
}

bool T5ynthPhraseSequencer::fromSteps(const StepInput* steps, int count, const Config& c)
{
    const std::lock_guard<std::mutex> lock(editMutex);
    const Motif m = motifFromSteps(steps, count, c, stored.seed, editOrigin.load(std::memory_order_relaxed));
    if (m.pulses == 0) return false;   // no step in the analysed bars
    adoptResolvedOrigin();
    undoMotif = stored;
    hasUndo.store(true, std::memory_order_relaxed);
    stored = m;
    publish(stored);
    return true;
}

bool T5ynthPhraseSequencer::undo()
{
    const std::lock_guard<std::mutex> lock(editMutex);
    if (!hasUndo.load(std::memory_order_relaxed)) return false;
    adoptResolvedOrigin();
    std::swap(stored, undoMotif);   // a second Undo redoes
    publish(stored);
    return true;
}
