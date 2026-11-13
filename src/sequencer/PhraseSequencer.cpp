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

std::uint32_t packView(const StepView& v)
{
    return static_cast<std::uint32_t>(std::clamp(v.note + 1, 0, 128))
         | (static_cast<std::uint32_t>(v.fn) << 8)
         | (v.head   ? 1u << 12 : 0u)
         | (v.accent ? 1u << 13 : 0u)
         | (v.slide  ? 1u << 14 : 0u);
}

std::uint32_t packChord(const Chord& c)
{
    auto iv = [&](int m) { return static_cast<std::uint32_t>(c.iv[static_cast<size_t>(m)] + 1) & 31u; };
    return (static_cast<std::uint32_t>(c.rootPc) & 15u) | (iv(1) << 4) | (iv(2) << 9) | (iv(3) << 14);
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
    // phrase, not the bar that is playing.
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
    const int num = std::clamp(numerator, 1, 32);
    int den = denominator;
    if (den != 1 && den != 2 && den != 4 && den != 8 && den != 16 && den != 32) den = 4;
    if (num == tsNum && den == tsDen) return;
    tsNum = num;
    tsDen = den;
    barLenQ = 4.0 * num / den;
    positioned = false;   // the bar grid moved: re-enter at the next block
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

void T5ynthPhraseSequencer::addPending(double q, int note, int strand, std::vector<VoiceEvent>& out, int offset)
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
    pending[static_cast<size_t>(pendingCount++)] = { q, note, strand };
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
    // (Stab's chord after a Part switch from a sliding line) has no one voice
    // to continue: the held note ends there and the chord starts fresh. The
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
    else        addPending(barStartQ + e.offQ, e.note, e.strand, out, offset);
}

void T5ynthPhraseSequencer::releaseUnusedHeld(int fromIndex, std::vector<VoiceEvent>& out, int offset)
{
    // A held note waits for its strand's next onset. When the (re)rendered bar
    // has none left on that strand — the layer was switched off, the motif
    // changed — it is released here instead of hanging.
    for (int s = 0; s < kStrandSlots; ++s)
    {
        if (heldNote[static_cast<size_t>(s)] < 0) continue;
        bool used = false;
        for (int i = fromIndex; i < render.count && !used; ++i)
            used = slotOf(render.events[static_cast<size_t>(i)].strand) == s;
        if (!used)
        {
            emitOff(heldNote[static_cast<size_t>(s)], s - 1, out, offset);
            heldNote[static_cast<size_t>(s)] = -1;
        }
    }
}

// ─── Evolution cache ───────────────────────────────────────────────────────

void T5ynthPhraseSequencer::rebuildEvolution(int phrase)
{
    const Config L = latchedConfig();
    evoPrev = evolvedMotif(fitted, L, phrase - 1);
    evoCur = evoPrev;
    if (phrase > fitted.origin) evolveOnce(evoCur, L, phrase);
    evoNext = evoCur;
    if (phrase + 1 > fitted.origin) evolveOnce(evoNext, L, phrase + 1);
    evoPhrase = phrase;
    evoValid = true;
    publishHeard();
}

void T5ynthPhraseSequencer::advanceEvolution(int phrase)
{
    if (evoValid && phrase == evoPhrase) return;
    if (!evoValid || phrase != evoPhrase + 1) { rebuildEvolution(phrase); return; }
    const Config L = latchedConfig();
    evoPrev = evoCur;
    evoCur = evoNext;
    evoNext = evoCur;
    if (phrase + 1 > fitted.origin) evolveOnce(evoNext, L, phrase + 1);
    evoPhrase = phrase;
    publishHeard();
}

void T5ynthPhraseSequencer::publishHeard()
{
    heard.write(evoCur, toAudioSeen, static_cast<std::uint32_t>(evoPhrase));
}

// ─── Bars ──────────────────────────────────────────────────────────────────

void T5ynthPhraseSequencer::renderCurrent()
{
    const int N = phraseBarsOf(cfg);
    const int p = floorDiv(bar, N);
    const Motif& cur = evoCur;
    const Motif& next = floorDiv(bar + 1, N) == p ? evoCur : evoNext;
    const Motif& prev = floorDiv(bar - 1, N) == p ? evoCur : evoPrev;
    const Config L = latchedConfig();

    Motif answer;
    const bool wantAnswer = cfg.layer == Layer::Answer && cfg.part != Part::Stab;
    if (wantAnswer) answer = answerMotif(cur, L);

    RenderInput in;
    in.cfg = &L;
    in.motif = &cur;
    in.nextMotif = &next;
    in.answer = wantAnswer ? &answer : nullptr;
    in.bar = bar;
    in.barLenQ = barLenQ;
    in.keys = cfg.harm == Harm::Keys;
    in.keysChord = currentKeysChord();
    if (in.keys)
    {
        // Keys has no chord history to recompute: the line continues live.
        in.prevNote = entryP;
        in.prevPrevNote = entryPP;
        in.prevVoicing = entryVoicingCount > 0 ? entryVoicing.data() : nullptr;
        in.prevVoicingCount = entryVoicingCount;
    }
    else
    {
        // Pedal / Chords: the entry state is bar − 1 rendered on its own, so
        // bar b sounds the same whether reached by playing or by a locate.
        int ep = -1, epp = -1;
        entryNotes(L, prev, cur, bar, barLenQ, scratch, ep, epp);
        in.prevNote = ep;
        in.prevPrevNote = epp;
    }
    in.slideIn = heldNote[static_cast<size_t>(slotOf(-1))] >= 0;
    renderBar(in, render);
    publishView();
    chordForGui.store(packChord(render.chord), std::memory_order_relaxed);
    currentBarForGui.store(bar, std::memory_order_relaxed);
    currentPhraseForGui.store(p, std::memory_order_relaxed);
    editOrigin.store(p, std::memory_order_relaxed);
}

void T5ynthPhraseSequencer::enterBar(int newBar, bool continuous)
{
    if (continuous)
    {
        entryP = render.lastNote;
        entryPP = render.prevNote;
        entryVoicingCount = render.lastVoicingCount;
        for (int v = 0; v < entryVoicingCount; ++v)
            entryVoicing[static_cast<size_t>(v)] = render.lastVoicing[static_cast<size_t>(v)];
    }
    else
    {
        entryP = entryPP = -1;
        entryVoicingCount = 0;
    }

    bar = newBar;
    barStartQ = static_cast<double>(bar) * barLenQ;

    const int N = phraseBarsOf(cfg);
    const int p = floorDiv(bar, N);
    if (!continuous || !evoValid)
    {
        latchedEvolve = cfg.evolve;
        rebuildEvolution(p);
    }
    else if (floorMod(bar, N) == 0 && cfg.evolve != latchedEvolve)
    {
        latchedEvolve = cfg.evolve;
        rebuildEvolution(p);
    }
    else
    {
        advanceEvolution(p);
    }

    // A bar start is a beat: a pending Keys chord applies here at the latest.
    if (keysChanged)
    {
        keysChord = keysPending;
        keysEverSet = true;
        keysChanged = false;
    }

    renderCurrent();
    nextEvent = 0;
    if (continuous) barStartFlag.store(true, std::memory_order_relaxed);
}

// ─── processBlock ──────────────────────────────────────────────────────────

void T5ynthPhraseSequencer::processBlock(int numSamples, double bpm, bool hostSync, double hostPpq,
                                         std::vector<VoiceEvent>& out)
{
    if (!running || numSamples <= 0 || !cfgValid || !(bpm > 0.0)) return;

    if (toAudio.sequence() != toAudioSeen)
    {
        Motif m;
        std::uint32_t barStands = 0, unusedB = 0, seqRead = 0;
        if (toAudio.tryRead(m, barStands, unusedB, seqRead))
        {
            liveMotif = m;
            toAudioSeen = seqRead;
            motifDirty = true;
            if (barStands == 0) renderDirty = true;
        }
    }

    const double dq = static_cast<double>(numSamples) * bpm / (60.0 * sampleRate);
    const double q0 = hostSync ? hostPpq : posQ;
    const double q1 = q0 + dq;
    auto offsetAt = [&](double q) {
        const double f = (q - q0) / dq * static_cast<double>(numSamples);
        return std::clamp(static_cast<int>(f), 0, numSamples - 1);
    };
    auto firstEventFrom = [&](double q) {
        int i = 0;
        while (i < render.count && barStartQ + render.events[static_cast<size_t>(i)].onQ < q - kEpsQ) ++i;
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
        evoValid = false;
    }

    if (!positioned)
    {
        // Start or locate: silence what sounds and enter the bar at q0.
        allNotesOff(out, 0);
        enterBar(static_cast<int>(std::floor(q0 / barLenQ)), false);
        nextEvent = firstEventFrom(q0);
        positioned = true;
    }
    else
    {
        if (motifDirty) rebuildEvolution(floorDiv(bar, phraseBarsOf(cfg)));
        if (renderDirty)
        {
            // A change inside the bar: re-render it and play on from here.
            renderCurrent();
            nextEvent = firstEventFrom(q0);
            releaseUnusedHeld(nextEvent, out, 0);
        }
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
                const double from = std::max(q0, barStartQ);
                const double k = std::ceil((from - barStartQ) / beatQ - 1.0e-6);
                tBeat = barStartQ + k * beatQ;
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
            continue;
        }
        if (tBeat <= t)
        {
            keysChord = keysPending;
            keysEverSet = true;
            keysChanged = false;
            renderCurrent();
            nextEvent = firstEventFrom(t);
            releaseUnusedHeld(nextEvent, out, offset);
            continue;
        }
        emitOn(nextEvent, out, offset);
        ++nextEvent;
    }

    if (!hostSync) posQ = q1;
    expectedQ = q1;

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
        outView[static_cast<size_t>(k)] = v;
    }
    return n;
}

// ─── Motif hand-off ────────────────────────────────────────────────────────

T5ynthPhraseSequencer::MotifChannel::MotifChannel()
{
    for (auto& w : words) w.store(0, std::memory_order_relaxed);
}

void T5ynthPhraseSequencer::MotifChannel::write(const Motif& m, std::uint32_t tagA, std::uint32_t tagB)
{
    // Single writer. Odd sequence = write in progress.
    const std::uint32_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (int i = 0; i < kMaxMotifSteps; ++i)
        words[static_cast<size_t>(i)].store(packSlot(m.slots[static_cast<size_t>(i)]), std::memory_order_relaxed);
    auto put = [&](int i, std::uint32_t v) { words[static_cast<size_t>(kMaxMotifSteps + i)].store(v, std::memory_order_relaxed); };
    put(0, static_cast<std::uint32_t>(m.len));
    put(1, static_cast<std::uint32_t>(m.stepsPerBar));
    put(2, static_cast<std::uint32_t>(m.pulses));
    put(3, static_cast<std::uint32_t>(m.rotation));
    put(4, static_cast<std::uint32_t>(m.part));
    put(5, m.seed);
    put(6, static_cast<std::uint32_t>(m.origin));
    put(7, tagA);
    put(8, tagB);
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

    for (int i = 0; i < kMaxMotifSteps; ++i) m.slots[static_cast<size_t>(i)] = unpackSlot(w[static_cast<size_t>(i)]);
    auto get = [&](int i) { return w[static_cast<size_t>(kMaxMotifSteps + i)]; };
    m.len         = std::clamp(static_cast<int>(get(0)), 0, 2);
    m.stepsPerBar = std::clamp(static_cast<int>(get(1)), 1, kMaxBarSteps);
    m.pulses      = static_cast<int>(get(2));
    m.rotation    = static_cast<int>(get(3));
    m.part        = static_cast<Part>(std::clamp(static_cast<int>(get(4)), 0, 3));
    m.seed        = get(5);
    m.origin      = static_cast<int>(get(6));
    tagA = get(7);
    tagB = get(8);
    seqRead = s1;
    return true;
}

void T5ynthPhraseSequencer::publish(const Motif& m, bool barStands)
{
    toAudio.write(m, barStands ? 1u : 0u, 0);
}

bool T5ynthPhraseSequencer::readHeard(Motif& m, int& phrase) const
{
    std::uint32_t source = 0, ph = 0, unusedSeq = 0;
    // The audio thread writes this channel only around a refit or a bar
    // crossing, a few words at a time: a read that catches it mid-write
    // succeeds on a retry.
    while (!heard.tryRead(m, source, ph, unusedSeq))
        std::this_thread::yield();
    if (source != toAudio.sequence()) return false;   // from an earlier stored motif
    phrase = static_cast<int>(ph);
    return true;
}

// ─── Message thread: the stored motif ──────────────────────────────────────

int T5ynthPhraseSequencer::resolvedStoredOrigin() const
{
    if (stored.origin != kOriginAtNextEntry) return stored.origin;
    Motif h;
    int phrase = 0;
    return readHeard(h, phrase) ? h.origin : kOriginAtNextEntry;
}

void T5ynthPhraseSequencer::adoptResolvedOrigin()
{
    stored.origin = resolvedStoredOrigin();
}

Motif T5ynthPhraseSequencer::getStoredMotif() const
{
    Motif m = stored;
    m.origin = resolvedStoredOrigin();
    return m;
}

void T5ynthPhraseSequencer::setStoredMotif(const Motif& m)
{
    stored = m;
    hasUndo = false;
    publish(stored);
}

void T5ynthPhraseSequencer::dice(const Config& c, std::uint32_t newSeed)
{
    adoptResolvedOrigin();
    undoMotif = stored;
    hasUndo = true;
    stored = generateMotif(newSeed, editOrigin.load(std::memory_order_relaxed), c);
    publish(stored);
}

void T5ynthPhraseSequencer::keep()
{
    Motif m;
    int phrase = 0;
    if (!readHeard(m, phrase)) return;
    // Running: the heard motif anchored at its own phrase plays on exactly as
    // before, even when the next phrase has begun meanwhile. m.origin is the
    // stored motif's origin as the audio thread resolved it (fitMotif carries
    // it); a phrase before it plays the motif unevolved, so there the origin
    // stays, or the phrases up to it would start evolving. Stopped: it starts
    // from the phrase where playback resumes.
    m.origin = editOrigin.load(std::memory_order_relaxed) == kOriginAtNextEntry
                   ? kOriginAtNextEntry
                   : std::max(phrase, m.origin);
    adoptResolvedOrigin();
    undoMotif = stored;
    hasUndo = true;
    stored = m;
    // The bar playing stands. In the first bar of a phrase a re-render would
    // lead its entry from the phrase before as the kept motif plays it — the
    // kept motif itself — instead of from what was heard there, and re-voice
    // the rest of the bar; from the next bar on the kept motif renders as
    // what was heard.
    publish(stored, true);
}

void T5ynthPhraseSequencer::fromSteps(const StepInput* steps, int count, const Config& c)
{
    adoptResolvedOrigin();
    undoMotif = stored;
    hasUndo = true;
    stored = motifFromSteps(steps, count, c, stored.seed, editOrigin.load(std::memory_order_relaxed));
    publish(stored);
}

bool T5ynthPhraseSequencer::undo()
{
    if (!hasUndo) return false;
    adoptResolvedOrigin();
    std::swap(stored, undoMotif);   // a second Undo redoes
    publish(stored);
    return true;
}
