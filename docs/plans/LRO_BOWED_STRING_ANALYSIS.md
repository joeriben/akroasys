# LRO: why the bowed strings are bad — the code, the measurements, and where the field is

Status: analysis 2026-11-07. No fix ordered. Work order (BJ, verbatim, German) is in
`memory/ACTIVE_WORKORDERS.md`: the LRO's strings/bowed algorithms are really bad; check whether
something is wrong; the genre has moved on considerably in recent years.

Everything below was read in source or measured today, except where a sentence names its
paper. Measurements are at 176400 Hz (the rate the plugin compiles Csound at, host 44100 × 4)
unless a table says otherwise. Renders and reference recordings are on disk:
`tools/lco_ab_string_bowed/*.wav` (the three bodies of §2–3, A2/A3/A4, levelled) and
`tools/lco_reference_out/string/*.wav` (§5). Scripts for the numbers: the session's
`measure_bowed.py` / `measure_any.py` (HNR, per-partial envelope flicker, pitch track, harmonic
slope); the opcode renders through `tools/lco_measure.py`.

## 1. What a "bowed" prompt reaches today

The library (`backend/lco_library.json`, lexicon_version 34) has two string entries.

**`string`** — "plucked or bowed string / guitar / harp / koto / pizzicato / violin / cello". One
body, three axes (`bow`, `pick`, `damp`). The whole body is:

```
aexd    dust 1.0, kden                 ; kden = 300 + 3600 * kbow  (300/s plucked … 3900/s bowed)
aexd    alpass aexd, …  (× 8)          ; smear each impulse in time
aexb    tone aexd, kcut                ; brightness envelope + a 1.9 Hz LFO
apik    vdelay aexb, (kpick / kstr) * 1000, 60
aexc    = aexb - apik                  ; pick-position comb
astr    streson aexc, kstr, 0.9884     ; the string = one comb resonator at the played pitch
abal    balance astr, aref             ; held against a fixed 400 Hz sine
```

`bow` changes nothing but the **density of random impulses** and four constants of the
brightness envelope. There is no bow in it: no friction, no stick–slip, no feedback from the
string's own motion into an excitation. It is a noise-excited comb filter, and the entry's own
`why` text describes that configuration as the thing to avoid ("a continuous wall of noise into the
same resonator is audibly an electric string driver: aggressive and restless").

**`strings`** — "bowed string SECTION — orchestral strings". Three detuned `vco2` sawtooths, each
through a `tone` whose cutoff an LFO called "bow pressure" moves, summed, plus one fixed `reson`
at 460 Hz. That is a 1970s string machine (Solina/Logan), which is legitimate material, but it is
what the author is handed for "orchestral strings", and its `heard.status` is `offen` — never
approved.

**The bank proves this is what ships.** Two UCDCAE presets carry the dust body verbatim:
`Bowed String.t5p` (prompt "bowed violin", 2026-08-02; reading line "Sustained bowed violin string
with breathing bow pressure") and `Rich Strings I.t5p` ("bowed violin + supersaw"). Both orchestras
contain `dust 1.0, kden` with `kden = 300.0 + 3600.0 * kbow` and `streson`. What a user hears as the
bank's bowed string is 3900 random impulses per second into a comb.

**BJ's own record on it** (`heard.bj_wortlaut` in `dco_lexicon.json`): 2026-07-30 "string
gestrichen ist immer noch nur eine notlösung"; 2026-08-01/02 "'bowed string' ist völlig daneben",
"bow ist auch furchtbar, ein einziges Gezitter", "bei bow nur sehr schwach"; accepted on
2026-08-02 as "verifiziert – noch beta" after `damp` and `pick` were rewired — and on that day only
those two axes were touched; the record says of `bow`: "UNVERÄNDERT gelassen (BJ 2026-08-02: nur
damp und pick)". So the bow axis BJ called "völlig daneben" is the one in the library today.

**The one real bow model that was tried was parked.** `tools/lco_ab_string_bowed/` (commit
6750430c, 2026-08-02) put three sides next to each other at 176400: the incumbent, the bare
waveguide bow `wgbow`, and `wgbow` with a "bow arm" (pressure/speed trajectories) and a "box"
(Woodhouse's four violin signature modes + bridge hill as a `mode` bank). BJ heard the bare bow —
"leider zu statisch … intensiv, aggressiv, aber nicht bowed string" — and the commit says the
arm+box body "is BJ's call and is parked until he makes it". No verdict on it is recorded anywhere.
Nothing of it is in the library.

**What the shipped author writes today** (`tools/lco_author_offline.py`, gemma-4-12b-it-qat-q4_0,
the author the plugin loads, run 2026-11-07, three prompts, 65 536-token context):

| prompt | result | time | what came out |
|---|---|---|---|
| `a bowed cello, sustained` | **no orchestra** after 3 attempts | 967 s | library-copy slips every round (`kdamp` used before defined; `=` before an opcode) |
| `a solo violin, bowed, singing` | **no inference at all** | — | `Requested tokens (65668) exceed context window of 65536` — head prompt 22.9 k chars + index 94.9 k chars + the consulted bodies no longer fit |
| `bowed string` | compiled at attempt 3 | 504 s | `string`'s **default** body verbatim (`dust 1.0, 736.2`, i.e. `bow` 0.35 — not even the bowed anchor); consulted `clarinet` + `string`; reading "A sustained bowed string with a shimmering high-end glint"; centroid 4910 Hz, comb contrast 32.9 dB |

One of three bowed prompts produced a sound, and that sound is the dust body at its plucked-leaning
default. The context overflow is a defect of the author path, not of the strings: a five-word
prompt fails before the model writes a line, and the `string` entry's nine full anchor bodies are
the likeliest reason the consultation no longer fits (the `strings`+`string` pair opens ~250 lines
of Csound). It is recorded here because it was found here; it needs its own work order.

## 2. Why the shipped body cannot be a bowed string — measured

A bowed string is a self-sustained oscillation: the bow hair sticks to the string, the string's
own travelling waves pull it loose, it slips, and the hair catches again one period later
(Helmholtz motion). The excitation is therefore **periodic by construction** and it is produced
**by the string**. The dust body has no path from the string back to its excitation; the
excitation is a Poisson process whose only coupling to the pitch is that it is filtered by a
comb afterwards. That difference is audible as three things, all measured on the A/B renders
(stationary window 1.0–3.5 s):

| body (176400) | pitch | HNR (dB) | per-partial envelope flicker h1…h6 (dB std) | pitch movement (cents, std) | harmonic slope (dB/oct) |
|---|---|---|---|---|---|
| `string` bow=1.0 (shipped) | A2 | 29.1 | 5.0 4.4 5.0 7.0 5.9 4.4 | 0.1 | −3.6 |
| | A3 | 28.5 | 5.3 5.4 4.6 6.9 4.3 4.7 | 0.1 | −4.9 |
| | A4 | 29.6 | 5.2 5.3 4.9 5.9 5.9 4.9 | 0.2 | −5.6 |
| `wgbow` bare | A2 | 37.0 | 4.2 3.1 2.3 2.0 2.3 1.8 | 0.1 | **+1.3** |
| | A3 | 42.2 | 3.9 2.7 2.0 1.2 2.8 1.8 | 0.0 | **0.0** |
| | A4 | 78.1 | 1.8 1.2 0.9 0.6 0.4 0.3 | 0.2 | −3.4 |
| `wgbow` + arm + box | A2 | 86.2 | 5.1 3.2 2.5 2.8 2.6 2.9 | 0.2 | −5.9 |
| | A3 | 62.9 | 3.0 2.1 1.4 1.8 1.8 1.6 | 0.0 | −0.2 |
| | A4 | 76.0 | 2.1 1.8 1.4 1.2 1.0 0.9 | 0.3 | −8.4 |

HNR = energy on the harmonics against energy halfway between them, 24 harmonics. Flicker = std of
each partial's own envelope (±6 % band, 5 ms frames) over the window.

- **The "Gezitter" is the dust.** Every partial of the shipped body flickers by 4–7 dB,
  independently of its neighbours, at the random rate of the impulses. In the waveguide bow the
  partials move 1–3 dB and together (the arm LFOs), which is what a bowed note's partials do.
- **28–30 dB HNR at every pitch** is the broadband drive showing through the comb. A periodic
  stick–slip drive has nothing between the harmonics but what the body's own noise puts there.
- **The shipped body stands dead still in pitch** (0.1–0.2 cents) and so does the bare `wgbow`
  (0.0–0.2). §5 has what a bowed note actually does. ("zu statisch" is the right word for both.)
- The harmonic slope of the dust body is in the right region (a Helmholtz sawtooth is −6 dB/oct)
  only because the `tone` lowpass is tuned to put it there; the bare `wgbow` at 176400 is **flat
  to rising** — see §3 for why that is not the model's fault but the plugin's rate.

## 3. The bow models this Csound actually ships — read in source

### 3.1 `wgbow` (Opcodes/physmod.c)

"Bowed String model ala Smith, after McIntyre, Schumacher, Woodhouse, by Perry Cook 1995–96,
recoded for Csound by John ffitch, November 1997." It is STK's `Bowed` of that year:

- **Friction:** a table-free curve `f(Δv) = (|Δv · slope| + 0.75)^-4`, clamped to ≤ 1
  (`BowTabl_lookup`). `kpres` is written straight into `slope`. This is the friction-curve model
  of McIntyre–Woodhouse 1979 / Smith 1986: friction is a memoryless function of relative velocity.
- **String losses:** ONE one-pole lowpass at the bridge, pole `0.6 − 0.1·(22050/sr)`, gain 0.95
  (`reflFilt`). No frequency-dependent loss you can set, no stiffness/dispersion, one
  polarisation, a point bow (no bow width, no hair compliance), linear-interpolated delay lines,
  `baseDelay = sr/f − 4` as the filter-delay correction.
- **Body:** ONE biquad at 500 Hz, r = 0.85, gain 0.2 (`bodyFilt`). STK itself replaced this in
  2011 with Esteban Maestre's six-section measured violin body ("Old single body filter" is
  commented out in today's `Bowed.cpp`); Csound's `wgbow` still has the 1995 one.
- **Bow velocity** rides on a hard-coded ADSR (attack 20 ms) started at init and released on
  note-off; the model decides the attack, not the body author.
- Recorded in this project before today: it catches a partial instead of the fundamental above
  ~880 Hz (10 of 27 settings), and it never renders twice the same (chaotic stick–slip).

**The oversampling finding — this is the "something wrong" with the one real bow.** The loop-loss
filter is specified in *normalised* frequency. At the plugin's 4× rate its cutoff in Hz is about
four times higher, so the string loses far less of its top per round trip than the model was
tuned for. Measured today, the bare `wgbow` body of the A/B, same settings:

| sr | A2 centroid | A3 centroid | A4 centroid | comb contrast A2/A3/A4 (dB) | tuning A2/A3/A4 (cents) |
|---|---|---|---|---|---|
| 44100 | 1656 Hz | 1521 Hz | 1822 Hz | 68.1 / 67.4 / 68.2 | +3.8 / +7.8 / +16.1 |
| 176400 (plugin) | **5052 Hz** | **5815 Hz** | 5524 Hz | **36.9 / 41.5** / 91.5 | +0.7 / +1.5 / +3.1 |

Spectral centroid 1.6–1.9 octaves higher, comb contrast down by 26–31 dB at A2/A3. What BJ heard on
2026-08-01 — "intensiv, aggressiv, aber nicht bowed string" — was this 4× rendering; the model as
designed (the 44100 column) has never been in front of him through the plugin. The project
recorded the same class of effect for `wgclar` ("centroid 340 Hz at 44100 and 1235 Hz at 176400",
`lco_write.py` head prompt) without drawing the conclusion for the bow. The tuning moves the other
way (the fixed "−4 samples" is a 44.1 kHz constant) and is better at 4×.

### 3.2 `STKBowed` (Opcodes/stk; `libstkops.dylib` links STK statically — Homebrew's STK is 5.0.1 here)

Same string and friction curve, two real differences: **Maestre's 2011 six-section body cascade**
(a measured violin bridge-to-radiation response) and bow pressure mapped as `slope = 5 − 4·p`
with bow velocity on its own controller (100). So the better body **is** in the build — behind two
measured limits already in the record (`project_lco_stk_opcodes_available`,
`docs/STK_OPCODES.html`): frequency is an **i-rate** argument (no `kfreq` follow, `reinit` per
note), and Csound's Windows release has no `stkops` module.

### 3.3 The rest

`STKBandedWG` — Essl & Cook's banded waveguides, the method Mutable Instruments' *Elements* uses
for its bow (per-mode bandpass + delay with a bow table on the sum; `elements/dsp/resonator.cc`,
MIT). i-rate frequency, like all STK opcodes. `wgbowedbar` — locks on a fixed mode (measured
2026-07-26). **Csound 7.0.0** (released 2026-10-21) adds no physical-model opcodes: its what's-new
page is language work (opcode references, arrays of opcodes, UDO syntax, MIDI file input), and
the only new filter named in the developers' forum is the analogue `otafilter`.

### 3.4 What the author is told

`lco_write.py`'s head prompt names `wgbow` among the "DRIVEN bodies" and says their movement
belongs in the driving parameter — correct — but gives it none of the measured facts it gives
`wgclar`, and `docs/LCO_CONCEPT.md` §6 files `wgbow` as "only 220–880 Hz, and it does not render
twice the same". The index the author reads describes `string`'s `bow=1.0` anchor as "drawn with a
bow: standing, breathing, the top rubbed off", with the dust body as its exemplar. Every bowed
prompt therefore lands on the dust body; the two bank presets are the proof.

## 4. Where the field is (what changed since 1995)

**Friction.** The memoryless friction curve `wgbow` uses was shown by Smith & Woodhouse (2000,
*J. Mech. Phys. Solids*) not to describe rosin: friction follows the **temperature** of the rosin
layer, and the thermal model is "more benign" in establishing Helmholtz motion and matches
measurements in both the Schelleng (force vs β) and Guettler (force vs acceleration) planes
better (Woodhouse 2003, *Acta Acustica*; Galluzzo, Woodhouse & Mansour, "Assessing friction laws
for simulating bowed-string motion"). The other current family is **elasto-plastic** friction
(bristle model with pre-sliding and hysteresis; Serafin, Avanzini & Rocchesso, SMAC 2003; Serafin
PhD 2004). In 2026 van Walstijn et al. (*Acta Acustica* 10, 47) combine the two and predict the
minimum and maximum bow force across β. A 2025 *Frontiers in Signal Processing* paper
(10.3389/frsip.2025.1525044) gives the elasto-plastic model the passivity it lacked.

**Numerics, and real time.** Willemsen, Bilbao & Serafin (DAFx 2019) run an elasto-plastic bowed
*stiff* string by finite differences in real time at under 6 % CPU for one string; Willemsen's
2021 thesis (*The Emulated Ensemble*, Aalborg) extends it. Desvages & Bilbao (2016, *Applied
Sciences* 6:135; Desvages PhD 2018) model **two polarisations**, finger and bow as contact forces,
and drive it from gestures. Russo, Ducceschi & Bilbao (DAFx 2022) simulate the bowed string **in
modal form, non-iteratively, faster than real time**; Russo's 2025 thesis and the group's 2024
work (IEQ/SAV schemes) give provably stable, non-iterative nonlinear strings in general.

**Body.** Maestre, Scavone & Smith (SMAC 2013) fit low-order modal filters to **measured bridge
admittances** of violin-family instruments for real-time synthesis — the body STK adopted. Inácio,
Antunes & Wright (2004) show that merely filtering the bridge force through a body response
misses the energy the body feeds back into the string; proper coupling is a modal body in the
loop. Woodhouse's signature modes (A0 272, CBR 407, B1− 462, B1+ 551 Hz, bridge hill ~2.3 kHz)
are already in this project's provenance record and in the parked arm+box body.

**Waveguides today.** Tablas de Paula, Smith, Välimäki & Reiss, "Four Decades of Digital
Waveguides" (*JAES* 74, 2026): the 1986 table-lookup reflection function is the starting point,
"later models introduced friction-loop hysteresis and thermodynamic models of rosin friction";
the review's bowed-string lineage then runs through neural inversion of waveguide bows (Casey;
Sinclair's adversarial autoencoder) to differentiable DSP.

**Commercial.** Audio Modeling's SWAM Solo Strings (violin/viola/cello/bass, since ~2013; string
sections 2023) are pure physical models derived from digital waveguides with bow speed, pressure
and position as the player's controls; articulations emerge from the model rather than being
switched (Sound on Sound confirms no samples are involved). AAS String Studio VS-3 has a bow
exciter on a modelled string (reviewers: hard to make behave, unstable at range extremes).
Mutable Instruments *Elements* (2014, MIT-licensed) does a synth bow: banded waveguides with a
friction table, into a modal resonator.

**Neural.** Luan & Scavone (DAFx 2025, arXiv 2505.18950) train physics-informed networks on
bow friction — on a one-mass toy, not a string yet. Lee et al. (NeurIPS 2024) do differentiable
modal synthesis for plucked strings. Nothing here replaces a physical model for a playable bow
today; it is where parameter fitting is heading.

## 5. The reference BJ asked for, on disk

`tools/lco_reference.py`'s own method, with the instrument named: SA3 (stable-audio-3-medium),
"solo cello / solo violin, one instrument alone, single sustained bowed note, close microphone, dry
studio recording, unaccompanied, no reverb", two seeds each → `tools/lco_reference_out/string/`
(PROMPT.txt there). These are for BJ's ear next to `tools/lco_ab_string_bowed/*.wav`.

What the meters can say about them, and what they cannot: every reference **moves in pitch** —
p10–p90 30–36 cents at 4.6–5.8 Hz (vibrato) — against 0.5 cents in the shipped body and 0–0.3 in
`wgbow`. Because of that vibrato, the HNR and per-partial flicker numbers of §2 are **not
comparable** across reference and renders (a fixed-pitch comb reads vibrato as noise), so no such
comparison is drawn here. A vibrato-robust meter (f0-tracked harmonic analysis) would be needed
before any number from a reference enters an acceptance test. The platform rule stands that a
body carries no built-in vibrato (the player's modulation does that); the point of the reference
is only that a bowed note is never static, and that whatever stands in for the bow has to carry
its movement in the bow's own parameters.

## 6. Verdict — what is wrong, in order of weight

1. **The shipped "bowed" string has no bow.** `bow` is the density of random impulses into a comb.
   The jitter (4–7 dB independent partial flicker), the noise floor (28–30 dB HNR) and the
   lifelessness (0.1 cents) all follow from that one fact. It was accepted as beta on a day when
   only `damp` and `pick` were fixed and the bow axis was explicitly left as BJ had rejected it.
2. **The one real bow in the build is 1995 technology, and the plugin runs it at a rate it was not
   designed for.** `wgbow`'s loop-loss filter is sample-rate-relative, so at the LRO's 4× it is
   1.6–1.9 octaves brighter and 26–31 dB noisier between harmonics than the same model at 44.1 kHz. The
   "aggressiv" verdict of 2026-08-01 was a verdict on that.
3. **The better STK body is in the build and unreachable for a melodic voice** (`STKBowed`: i-rate
   frequency, macOS only).
4. **`strings` is a string machine labelled as an orchestral section**, never approved, and the
   author's only exemplar for "orchestral strings".
5. **The author is steered onto the dust body**: the `string` anchor glosses promise a bow, the
   head prompt gives `wgbow` no usable facts, and §6 of the concept document files it as
   restricted. Measured end to end (§1): of three bowed prompts, one compiles, and it is the
   dust body at its default; one dies on library-copy slips; one never reaches the model because
   the prompt no longer fits the 64 k context.

Nothing in the bowed path is a bug in the sense of a wrong line; the construction is a 1983
sustained-Karplus–Strong stand-in where the field has had real-time stick–slip models for twenty
years and provably stable modern ones for five.

## 7. What a repair would need (routes, with a recommendation)

Named per CLAUDE.md's Instrument Authoring rules: method and source first, nearest existing body
rendered first, BJ's ear decides, one instrument at a time, no code taken from GPL sources
(`docs/LCO_CODE_PROVENANCE.md`: take physics, not code).

- **A. Run `wgbow` at its design rate.** Not available: the loss filter is inside the opcode, the
  LRO compiles one orchestra at the host's oversampled rate, and patching Csound's LGPL opcode
  means carrying a Csound fork. Rejected.
- **B. Write the bow at orchestra level, in Csound.** Two delay lines (`delayr`/`delayw` inside a
  UDO with `setksmps 1`, because the stick–slip loop needs one-sample feedback), a friction
  nonlinearity as a `tablei` lookup (McIntyre–Woodhouse curve, or the thermal model's state
  update written out), a **proper** loop-loss filter in Hz that does not move with `sr`, string
  stiffness as an allpass, and the body as Woodhouse/Maestre modal filters (`mode`). Everything
  in it is published method; everything in it follows `kfreq`, bow pressure, velocity and
  position at k-rate, so it fits "an LLM writes Csound" and the three-oscillator panel as it
  stands. The open number is CPU: `setksmps 1` at 176400 × 16 voices has to be measured before
  anything is promised. This is the route that lets BJ hear a real bow with a real loss filter at
  the plugin's rate within days, and it is the probe for whether the friction-curve model is
  already good enough once it is not running 1.6–1.9 octaves bright.
- **C. An own Csound plugin opcode** (`libt5bow` beside the shipped `stkops`), in C, implementing a
  current model: stiff string with frequency-dependent loss, elasto-plastic or thermal friction
  (Willemsen 2019 / Woodhouse 2003 as the published algorithms), finger and bow as k-rate
  controls, a measured-admittance modal body (Maestre 2013) — cross-platform, k-rate frequency,
  cheap per voice. This is what the genre did (SWAM, Willemsen, Russo). It is the target if B
  shows the friction curve is the limit and not the rate.
- **D. `STKBowed` with `reinit` per note** to hear Maestre's body at once. macOS only, no legato
  or glide. An A/B tool, not a library entry.

Recommendation: B first, as a measured probe with `nearest_existing.wav` = today's `string`
bow=1.0 and the SA3 references beside it; C as the target the probe decides for or against; D only
for hearing the body. Nothing here is started without BJ's order.

## 8. Sources read

Code: `backend/lco_library.json`, `backend/dco_lexicon.json`, `backend/lco_write.py` (head
prompt), `tools/lco_ab_string_bowed.py` and its bodies, `tools/lco_reference.py`,
csound/csound `Opcodes/physmod.c` (csound6), thestk/stk `src/Bowed.cpp` (master),
pichenettes/eurorack `elements/dsp/{exciter,voice,resonator}.cc`, the two bank presets.

Papers: Smith 1986 ICMC; McIntyre, Schumacher & Woodhouse 1983; Smith & Woodhouse 2000; Woodhouse
2003 Acta Acustica; Galluzzo, Woodhouse & Mansour, "Assessing friction laws…"; Serafin, Avanzini &
Rocchesso SMAC 2003; Serafin PhD 2004; Willemsen, Bilbao & Serafin DAFx 2019
(<https://www.dafx.de/paper-archive/2019/DAFx2019_paper_18.pdf>); Willemsen PhD 2021 (Aalborg,
doi 10.54337/aau451025772); Desvages & Bilbao 2016 Applied Sciences 6:135; Russo, Ducceschi &
Bilbao DAFx 2022 "Efficient Simulation of the Bowed String in Modal Form"; Russo PhD 2025 (Bologna);
Maestre, Scavone & Smith SMAC 2013; Inácio, Antunes & Wright 2004 (IoA); van Walstijn et al. Acta
Acustica 2026, 10:47 (doi 10.1051/aacus/2026042); Frontiers Signal Processing 2025
(10.3389/frsip.2025.1525044); Tablas de Paula, Smith, Välimäki & Reiss, JAES 74(7/8) 2026, arXiv
2604.12878; Luan & Scavone DAFx 2025, arXiv 2505.18950; Lee et al. NeurIPS 2024, arXiv 2407.05516.
Products: audiomodeling.com (SWAM-S), Sound on Sound SWAM Solo Strings review, AAS String Studio
VS-3 reviews (Sound on Sound, MusicRadar), Mutable Instruments Elements manual.

## 9. Route B built — 2026-11-08

BJ, 2026-11-08, on the SA3 references of §5: *"Violine und Cello sind schon sehr gut; es flattert
nur etwas. […] bau das so."* Route B of §7 is built; nothing of C or D.

**What exists now**

- `t5bow`, a user-defined opcode in the orchestra head (`backend/lco_write.py`, `_BOW_UDO`,
  available to every body like the ftgens): a digital waveguide bowed string in velocity waves —
  two delay lines meeting at the bow, nut −1, bridge −1 through a `tone` whose cutoff is in **Hz**
  at 0.95 per round trip, and the memoryless friction junction with Cook's hyperbolic curve
  (Smith 1986; MSW 1983; Jaffe & Smith 1983; STK `Bowed`/`BowTable` for the curve). It runs in a
  sub-block of round(sr/44100) samples taken down to a power of two, because a local ksmps must
  divide the global 64 (1 at 44100/48000, 2 at 88200, 4 at 176400/192000; a 3 at 132300 or at
  176224, the 44.1 k pull-down × 4, would refuse to initialise and silence all 16 voices — found
  by the verification pass, fixed before commit), so the loop closes every 21–23 µs at the common
  rates; both taps are shortened by that sub-block, so the loop length is exact. The loss filter's phase delay at the
  played fundamental, computed from `tone`'s own pole, is taken out of the nut side.
- `bowed_string`, a lexicon entry (`backend/dco_lexicon.json` → `backend/lco_library.json`): the
  string, a bow **arm** (bite-and-release at note start, slow drift of weight and speed — none of it
  on the pitch), and a **box** of five `mode` filters at Woodhouse's violin signature modes
  (A0 272, CBR 407, B1− 462, B1+ 551, hill 2300 Hz; Euphonics 5.3), scaled by `box` down to a
  cello at 0.37. Three axes: `place` 0.10..0.20, `gut` 0..1 (loss cutoff 1400..8400 Hz, floored at
  3× the played pitch), `box` 0.35..1.20. A `press` axis was built and dropped: over slope 1.0..2.5
  it moved the centroid by a tenth and nothing else a player would reach for, and past 1.25 it let
  the string fall into double-period motion away from the bridge; the weight stands at 2.0.
- The bowed words (`bowed string`, `violin`, `cello`, `fiddle`, `double bass`, …) moved from
  `string`'s surface forms to `bowed_string`; `string` keeps the plucked ones and its `why` now
  points at the bowed entry. `string`'s code, params and anchors are untouched, so BJ's approval
  of it stands (`tools/lco_approvals.py` hashes only those).

**Measured** (`tools/lco_ab_bowed_waveguide.py`, `tools/lco_axis_probe.py --gate`; 176400 and 44100)

| | 55 Hz | 110 | 220 | 440 | 880 | 1320 | 1760 | 2500 |
|---|---|---|---|---|---|---|---|---|
| bowed note, 1st partial, 176400 | +0.5 c | +1.0 | +2.0 | +4.0 | +7.2 | +13.2 | +14.0 | +13.2 |
| same at 44100 | +0.5 | +0.9 | +2.0 | +3.3 | +7.8 | +9.5 | +9.3 | +13.6 |
| free string (bow removed), both rates | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |

- The sharpness is the bow's: the hair catches the smeared Helmholtz corner as soon as it enters
  the stick window (0.25/slope wide), before the corner has fully arrived. It grows with pitch, with
  weight and with darkness (gut 0 at 440 Hz: +6 c; at 880 Hz: +14 c). Declared in the entry, not
  tuned out — a fitted correction would be a number laid over the model (Instrument Authoring 2).
- Gate PASS at both rates, 27 corners × 6 registers: loudness spread 0.01 dB, register tilt
  0.0 dB, worst peak 0.62 against the host's 2.746; every corner moves.
- The fundamental holds over place 0.10..0.20 at 55–2500 Hz, both rates; the sub-octave stays
  below −50 dB everywhere inside the ranges (at 0.24–0.25 it was 10–17 dB under the note). Twelve
  note jumps on the already-vibrating string (110→440, 1760→880, 55→1320, 2500→330, …) all land on
  the new fundamental within the first 50–400 ms.
- Same instrument at both rates: audible-band centroid 228/228 Hz (110 Hz note), 508/504 (440),
  1805/1788 (1760) at 176400/44100. The full-band centroid differs because 4× carries ultrasonic
  stick-slip content the plugin's decimator removes.
- Around 880–1100 Hz the hill lifts partials 2–3 some 4–7 dB above the fundamental (a violin at
  A5); declared in `box`'s note.
- Cost: 1.9 % of one core per voice at 176400 against 0.8 % for `string`; sixteen always-on voices
  ≈ a third of a core at 4×.

**For BJ's ear** — `tools/lco_ab_bowed_waveguide/`: `nearest_existing__A2/A3/A4` (today's `string`
bow=1.0), `new__…` (violin box), `new_cello__…` (box 0.37), all at 176400, levelled to the
incumbent per pitch, and the four SA3 references beside them (`reference__cello_sa3_seed11/22`,
`reference__violin_sa3_seed11/22`). The audio is ignored by git; the bodies and the tool are
tracked.

**Open**

- BJ's verdict. Route C (an own opcode with a thermal or elasto-plastic friction model, §4) only
  after it; it would also be where the capture-sharpness could be computed away rather than
  declared.
- The author path's context overflow on long consultations (§1) is a separate defect and is not
  touched here.
