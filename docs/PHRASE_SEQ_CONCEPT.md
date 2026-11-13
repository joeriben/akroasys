# Phrase Sequencer — Concept

Status: concept, 2026-11-13. Nothing implemented. The Gen sequencer is not
touched by anything in here.

## 1. Purpose

A third sequencer mode for musicians who work in conventional electronic
idioms (techno, house, electro, trance, synth-pop, ambient with a pulse).
The mode switch becomes **Step | Phrase | Gen**, ordered by how much the
musician hands over:

| Mode   | Use case, in the musician's words |
|--------|-----------------------------------|
| Step   | "I write the pattern." |
| Phrase | "I want a bass line / riff / lead / stab that loops, fits my chords, varies at phrase ends, and sits in my DAW's bar structure." |
| Gen    | "I want an evolving, polymetric ensemble that I steer but do not control." |

Name: **Phrase**, because the organising principle of the mode is the phrase
(bar → 4/8/16-bar phrase), and the word means the same thing to a producer
and to a theory book.

## 2. Why Gen does not serve this use case

Read from `GenerativeSequencer.cpp`. Everything in Gen changes on the cycle
level, and there is no level above the cycle:

- Turing mutation moves notes ±1–4 scale degrees every cycle.
- Pulses, rotation and step count drift (pendulums over 8-cycle windows).
- The pitch field (pc-set) drifts / transforms / pivots every *Cyc* cycles.
- Strands run at different division multipliers (polymeter).
- Metric elasticity (rubato) and stage panning move timing and stereo.
- Pitch is a walk through a scale or pc-set. There is no chord and no
  distinction between a structural tone and an embellishing one.

All of that is Gen's point. Phrase replaces each of these with its
conventional counterpart: bar and phrase hierarchy, literal repetition,
change at phrase boundaries, and pitch organised by harmonic function.

## 3. Music-theoretical model

### 3.1 Meter

- 4/4 bar grid. The shared Division sets the step resolution:
  steps per bar = 1/16 → 16, 1/8 → 8, 1/4 → 4, 1/2 → 2, 1/1 → 1.
- **Metric weight** of a position = the number of metrical levels (bar,
  half bar, beat, 8th, 16th) on which it is a beat — the metrical grid of
  Lerdahl & Jackendoff (1983). On the 16th grid, position p (0-based):

  | p | 0 | 8 | 4, 12 | 2, 6, 10, 14 | odd |
  |---|---|---|-------|--------------|-----|
  | w | 4 | 3 | 2     | 1            | 0   |

  i.e. w(p) = number of trailing zero bits of p, w(0) = 4. Coarser divisions
  map their positions onto the 16th grid (p16 = p · 16 / stepsPerBar).
- No rubato, no step/pulse drift, no polymeter, no auto-panning. Shuffle
  (shared) applies, because swing is part of the groove.

### 3.2 Structural and embellishing pulses

Which pulses carry the harmony is decided by a time-span reduction
(Lerdahl & Jackendoff 1983, ch. 6–7):

- The bar is cut into **spans**. Span length depends on the Part (§3.5):
  beat for Bass and Riff, half bar for Lead; for Stab every pulse is
  structural.
- In each span, the **head** is the pulse with the highest metric weight;
  ties go to the longer duration until the next pulse (metrical preference
  for long events), then to the earlier one (preference for early strong
  beats).
- Heads are structural → chord tones. All other pulses are embellishments.
- A span without a pulse has no head.

The rule is relative to the pattern, not to the absolute grid. Worked
case: an offbeat bass (Euclid 16/4, rotation 2 → pulses on 2, 6, 10, 14)
has exactly one pulse per beat-span, so every offbeat is a head and plays
a chord tone — as an offbeat bass should. A 16th run in one beat gets its
chord tone on the beat and passing/neighbour tones in between.

### 3.3 Harmony

Three sources, selected per patch:

- **Pedal** (default): one chord on the tonic for the whole phrase. The
  normal case in techno and house.
- **Keys**: held keys define the chord. Held pitch classes are the chord
  members, the lowest held note is the root. The chord latches until the
  next chord is played; a change takes effect on the next beat. In this
  source the keys are control input and do not sound themselves.
- **Chords**: a chord loop of 1–4 slots, each slot a scale degree, one
  harmonic rhythm for all slots (1, 2 or 4 bars per chord). Changes happen
  only on bar boundaries.

No progression library ships with the mode — no stock pop progressions
(consistent with the devlog's "keine 1625-Klischees", 2026-04-24). The
musician sets the degrees or plays the chords.

**Chord construction** from the shared Scale (global `scaleRoot` /
`scaleType`, the same params Gen uses):

- Heptatonic scales: stack alternate scale steps (diatonic triads; with
  Ext = 7 also the seventh). Slots are displayed as roman numerals whose
  case and accidentals follow from the scale (aeolian: i, ii°, ♭III, iv,
  v, ♭VI, ♭VII).
- Other scales (pentatonic, hexatonic, …): for each member take the scale
  tone inside its tertian band above the root (third 3–4 semitones, fifth
  6–8, seventh 10–11). If two qualify, the fifth prefers 7, third and
  seventh prefer the lower. If none qualifies, the member is omitted
  (open voicing). Example: A minor pentatonic on A gives A–C–E (Ext 7:
  A–C–E–G); on D there is no third, so it becomes D–A (Ext 7: D–A–C).
  To be checked by listening (§6).
- Chromatic (`Off`): Chords source is disabled (degrees are meaningless);
  Pedal gives root + fifth; full chords come only from Keys.

Chord members: **R, 3, 5, 7** (7 only with Ext = 7).

### 3.4 Function representation

The motif stores what each step *is*, not which pitch it has. Per step:

- **Rest**
- **Chord tone**: member ∈ {R, 3, 5, 7}, octave ∈ {−1, 0, +1} relative to
  the Part's register anchor.
- **Embellishment**: kind ∈ {Repeat, Octave, Passing, Neighbour ↑/↓,
  Approach ↑/↓ (diatonic), Approach chromatic (Bass only)}.
- Flags: **accent**, **slide** (legato glide into the next note, the
  existing Bind/Glide voice path).

Rendering happens at playback, per step, against the chord and scale valid
at that moment. Embellishments are resolved against the previous sounding
note P and the next head T. T is computed with the chord valid at T's
position, so an approach note before a chord change targets the *new*
chord — which is what an approach note is for.

| Kind | Resolution |
|------|-----------|
| Repeat | P |
| Octave | P ± 12, direction alternating to stay in range |
| Passing | the scale step between P and T, moving toward T; if P and T are adjacent or equal, falls back to Neighbour |
| Neighbour ↑/↓ | scale step above/below P |
| Approach ↑/↓ | scale step above/below T |
| Approach chromatic | T − 1 semitone |

Categories after Aldwell, Schachter & Cadwallader (2011). Lead lines
follow post-skip reversal (Huron 2006): after a leap larger than 4
semitones, the next embellishment moves stepwise in the opposite direction.

Example — one bar, 8th grid, `R · 5 R' · 3 · ↗R`:

| Function | R | · | 5 | R' | · | 3 | · | ↗R |
|----------|---|---|---|----|---|---|---|----|
| over Am  | A | · | E | A' | · | C | · | G (→A) |
| over F   | F | · | C | F' | · | A | · | E (→F) |

When the chord changes, the motif follows by construction. This is the
conventional counterpart to Gen's degree walk.

### 3.5 Parts

The Part is the first choice in the UI. It sets register, span length,
member preferences and the embellishment set. The weights are starting
values for the listening phase, not derived from data.

| Part | Register anchor | Span | Head members (preference) | Embellishments |
|------|-----------------|------|---------------------------|----------------|
| Bass | MIDI 36 | beat | R ≫ 5 > R+12 | Repeat, Octave, Approach (incl. chromatic), Passing |
| Riff | MIDI 48 | beat | R, 3, 5, 7 about evenly | Repeat, Neighbour, Passing |
| Lead | MIDI 60 | half bar | 3, 5 > R, 7 | Passing, Neighbour, Approach (diatonic) |
| Stab | MIDI 60 | every pulse | whole chord | none (pulses and rests only) |

- Bass: the first head of the motif is always R (downbeat root).
- Stab plays the chord on every pulse, close position within an octave
  above the anchor, voice-led across chord changes: common tones stay,
  the other voices move to the nearest member. The whole chord is the
  main line (`VoiceEvent::strandId` −1), so the voice manager treats every
  voice alike: same stereo image, same sustain/sostenuto handling. Under
  the arp, the lowest note of the latest onset is the lead.
- The shared Seq Octave (`seqOctave`, already used by Step and Gen)
  transposes all Parts.

### 3.6 Motif generation

- Motif length: 1 or 2 bars (`Len`).
- Rhythm: Euclid (Toussaint 2005) over the motif's step count with Pulses
  and Rotation. The step count is fixed by Len × steps per bar and never
  drifts. Euclid already covers the core figures: 16/4 = four-on-the-floor,
  16/4 rotated by 2 = offbeat 8ths, 8/3 = tresillo.
- Accents: Gen's Euclidean rule (step 0 and the pulse after the longest
  gap).
- Pitch: heads draw a member from the Part's preferences; non-heads draw an
  embellishment kind from the Part's set, constrained by context (Passing
  only if P and T are at least two scale steps apart, and so on).
- Slides: only on Approach and Passing steps into a head (gliding into the
  target), with probability = Slide.
- Generation is seeded. Dice draws a new seed and generates a new motif;
  Undo returns to the previous one (one level).

### 3.7 Phrase grammar and variation

- Phrase length: 4, 8 or 16 bars. Form ∈ {Loop, AAAB, AABA, ABAB}; each
  form letter covers a quarter of the phrase; inside a letter the motif
  repeats.
- **A** is the motif as stored. **B** is A with variation operators
  applied.
- ABAB is antecedent/consequent (Caplin 1998): A ends open (last head on 5,
  or scale degree 2 over a tonic pedal), B ends closed (last head on R).
- Variation operators, ordered from least to most intrusive. The Variation
  control (0–1) sets how many of them apply, in this order, to B units and
  to the last bar of every phrase:
  1. **Re-pitch**: one embellishment changes kind (e.g. Neighbour → Passing).
  2. **Register lift**: the last beat goes up an octave (Bass: an Octave
     embellishment instead).
  3. **Fill**: extra 16th pulses in the last beat, as a passing/approach run
     into the next downbeat.
  4. **Ratchet**: one head in the last beat repeats as 2 or 3 sub-steps.
  5. **Drop**: the last beat is silent (the gap before the next phrase).
     Excludes Fill.
- Variation = 0 is a literal loop (ABAB keeps only its cadences, which
  belong to the form).
- Mechanically this is what Elektron's conditional trigs (A:B, FILL) do by
  hand; here the conditions follow from the phrase position.

**Phrase position** comes from the host when the host plays (bar index from
the PPQ position and the time-signature numerator, 4/4 if the host reports
none), from MIDI clock Song Position Pointer when that is the sync source,
otherwise from bars counted since Play. The chord loop and the phrase
grammar both run off this one bar index, so they are aligned at bar 1 and
stay aligned.

### 3.8 Evolution

- Evolve (0–1): at every phrase boundary, round(Evolve × pulses / 4)
  steps (at least 1 if Evolve > 0) change their function: head member
  (within the Part's preferences), embellishment kind, or head octave.
  The rhythm never evolves; Bass keeps R on its first head.
- Evolve = 0 is a locked loop (the locked end of Marbles' Déjà vu).
- **Keep** stores the currently heard (evolved) motif as the new stored
  motif.

### 3.9 Determinism

The output at bar b is a pure function of (stored motif, seed, parameters,
b). Variations are seeded from (seed, phrase index); evolution at phrase i
is the stored motif after i deterministic mutation steps, recomputed on
locate. Consequences:

- Playing the same DAW region twice gives the same notes; a bounce is what
  was heard.
- Locating to bar 97 plays what bar 97 plays, not "whatever the RNG is at".
- The stored motif is part of the plugin state and the preset; reloading
  plays the same line. (Gen's patterns are re-randomised; that difference
  is intentional.)

### 3.10 Layers

One extra layer per instance, using the strand mechanism, panned centre
(panning is the musician's mix decision):

- **Off**
- **Octave**: the line doubled (Bass +12, Riff/Lead −12).
- **Chord**: the chord, voice-led as in Stab, on the line's head positions.
  Not available for Stab.
- **Answer**: a second motif (own seed, register +12 or −12 relative to the
  Part) that sounds only in B units — call and response.

## 4. UI

The Phrase area replaces Gen's controls, not the shared header and not the
Arp row.

```
Row 1 (shared)  [▶] [Step|Phrase|Gen]  BPM[==]  [1/1 … 1/16]  Gate[==]  Shuffle[==]  MIDI
Row P1          Part [Bass|Riff|Lead|Stab]   Key [A▾][Minor▾]   Oct [-2 -1 0 +1 +2]   Layer [Off▾]   Slide[==]
Row P2          Harm [Pedal|Keys|Chords]   [ i ▾][♭VI▾][♭VII▾][ – ▾]   Rate [1|2|4]   Ext [3|7]
Row P3          Len [1|2]  Pulses[==]  Rot[==]   Bars [4|8|16]  Form [AAAB▾]  Var[==]  Evolve[==]  [⚄][Keep][↶]
Phrase view     ┌ A  Am ┬ A  Am ┬ A  F  ┬ B  G  ┐   ← phrase strip: form letter + chord per bar, current bar lit
                │ A3  ·  E3  A4  ·  C4  ·  G3   │   ← motif grid: note names
                │ R   ·  5   R'  ·  3   ·  ↗    │      function glyphs, heads drawn stronger, accent/slide marks
Arp (shared)    [ARP] [modes] [rate] [OCT 1 2 3 4]
```

The rows follow the use case: what (Part) → in which key → over which
harmony → which rhythm → how it unfolds over the phrase.

| Gen element | In Phrase |
|-------------|-----------|
| Steps + FIX | Len (bars); no drift, so no FIX |
| Pulses, Rotation | kept, own params; FIX dropped |
| Evolve + FIX | Evolve, per phrase instead of per cycle; Keep / Dice / Undo |
| Root, Scale | kept (same global params) |
| Rng | dropped; register comes from Part + head octave + Seq Octave |
| Drift (field mode), Cyc | replaced by Harm source + chord strip + Rate |
| Coord mode | dropped |
| Strand modules (Role, Div, Oct, Dom) | replaced by Part and Layer |
| CRT monitor | replaced by the phrase view |

- Mode switch: three connected segments, widths from `measureTextWidth`,
  so "Phrase" never drops below the 11 px minimum
  (`INTERFACE_DESIGN_POLICY.md`).
- Phrase view, v1: read-only. Hand editing happens in Step.
- Transfers between modes:
  - Phrase → Step: the rendered current bar(s) go into the step grid, as
    Gen → Step does today.
  - Step → Phrase: an explicit "From Step" action that analyses the step
    notes against the current chord (heads → nearest member, other steps →
    classified embellishment). Not automatic on mode switch, because
    Phrase's motif is a stored asset and a switch must not overwrite it.
  - Gen ↔ Phrase: no transfer.

## 5. Architecture

- New class `T5ynthPhraseSequencer` in `src/sequencer/PhraseSequencer.{h,cpp}`,
  beside Step and Gen. Reuses `EuclideanRhythm.h` and `ScaleQuantizer.h`;
  shares no code with Gen's strand pipeline, so Gen stays byte-identical.
- **Mode parameter.** The bool `gen_seq_running` (`PID::genSeqRunning`,
  a Step↔Gen toggle) becomes a Choice `seq_mode` {Step, Phrase, Gen}.
  Migration on state load and JSON preset import: if `seq_mode` is
  absent, `gen_seq_running` true → Gen, else Step. Host automation recorded
  on the old bool is not carried over. Current readers of the old param
  (grep, 2026-11-13):
  - `ParamCache.h` (`X(genSeqRunning)`)
  - `PluginProcessor.cpp`: mode-switch block with the Step↔Gen pattern
    transfer (~4692), `seqRunningNow()` (~7245), replay save/restore
    (~1448 / ~1582), ~10160, JSON export/import (~11352 / ~12322),
    preset-load mode sync (~12472), XL mode button (~12589, becomes a
    cycle Step → Phrase → Gen)
  - `SequencerPanel.cpp` (~635, ~1145, ~1546), `MainPanel.cpp` (~2118)
- **Params** `phr_*`: part, harmSource, chord slots ×4, chordRate, ext, len,
  pulses, rotation, bars, form, variation, evolve, layer, slide, seed.
  The motif (function slots) is not an APVTS param; it is persisted in
  the state XML and in a `phraseSeq` block of the JSON preset
  (`PRESET_FORMAT.md` to be extended).
- **Host bar position.** Phrase is the first sequencer to read the PPQ
  position; it is taken once per block in the existing host-transport
  snapshot in `processBlock`, where only the BPM is read today.
- Audio thread: fixed-size arrays, no allocation; rendering a step is
  O(1); recomputing evolution after a locate is O(phrases × pulses).
- GUI: the phrase view repaints only on step change
  (`PERFORMANCE_GUIDE.md`).

## 6. Open points

- **Arp + Keys source.** The arp also takes held keys as its source. With
  Keys as harmony source, the held keys are harmony input; whether the arp
  then arpeggiates the sequencer's output or is disabled has to be decided.
- **Non-4/4 host time signatures.** The bar index follows the numerator;
  the weight table is 4/4. 3/4, 6/8 and 7/8 need their own grids.
- **Non-heptatonic chord building** (§3.3) and the **Part weights** (§3.5)
  are decided by listening, not by measurement.
- **Step → Phrase analysis**: how well the classification recovers a
  hand-written riff can only be judged on real riffs.
- With two generative modes, the label "Gen" is less distinctive than it
  was; a rename is outside this concept.

## 7. Sources

- Lerdahl, F. & Jackendoff, R. (1983). *A Generative Theory of Tonal Music.*
  MIT Press. — metrical grid, metrical preference rules, time-span reduction.
- Butler, M. J. (2006). *Unlocking the Groove: Rhythm, Meter, and Musical
  Design in Electronic Dance Music.* Indiana University Press. — loop- and
  phrase-based form in EDM.
- Huron, D. (2006). *Sweet Anticipation: Music and the Psychology of
  Expectation.* MIT Press. — pitch proximity, post-skip reversal.
- Aldwell, E., Schachter, C. & Cadwallader, A. (2011). *Harmony and Voice
  Leading*, 4th ed. Schirmer. — embellishing-tone categories, voice leading.
- Caplin, W. E. (1998). *Classical Form.* Oxford University Press. —
  antecedent/consequent.
- Toussaint, G. T. (2005). "The Euclidean Algorithm Generates Traditional
  Musical Rhythms." *Proc. BRIDGES*, 47–56.
- Design references (instruments, not theory): Mutable Instruments Marbles
  (Déjà vu: lock vs. randomness), Elektron conditional trigs (A:B, FILL).
