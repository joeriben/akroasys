# How an LRO instrument is reworked or added

The order of work that produced `bowed_string` (2026-11-07/08), written down so the
next instrument can be done the same way by someone who was not there. The RULES it
obeys live elsewhere and are not restated here: CLAUDE.md "Instrument Authoring"
(method and source before code, the substrate's model as written, BJ's ear decides,
comparison first, one instrument at a time), "Migration & Substrate Discipline",
`docs/LCO_CSOUND_SOURCES_AND_LICENCES.md` (take physics, not code) and
`docs/LCO_TEST_POLICY.md` (the thing on trial is a WORD). This document is the
sequence, the tools, and the traps, each with the place where the bowed string shows
it. The worked example itself: `docs/plans/LRO_BOWED_STRING_ANALYSIS.md` (§1–8 the
analysis, §9 what was built and measured), the four commits 8405cd33, d71c1c48,
a5adfb98, eca4c5fd, and the listening folder `tools/lco_ab_bowed_waveguide/`.

The unit of work is ONE word (or one family of words that name one thing: violin,
cello, viola, bowed). "Rework the strings" is not an order this procedure can take;
"the bowed string sounds wrong" is.

## 0. Begin with the verdict, verbatim

BJ's words are the work order and the acceptance criterion, and they go into the
record at the moment they are said: `ACTIVE_WORKORDERS.md` for the session, the
analysis document's §1 for the repository. Not paraphrased. Two kinds of sentence
matter and are kept apart:

- the complaint ("Die Strings/bowed-Algos sind wirklich schlimm"), which names the
  WORD on trial and nothing about the cause;
- the reading of a reference ("Violine und Cello sind schon sehr gut; es flattert nur
  etwas"), which fixes what the result is compared against.

Everything the model or the author later says about the sound is quoted against these
and never reinterpreted (memory: `never_reinterpret_user_perception`).

## 1. Measure what ships before touching anything

Find out what the word's current body actually does, in source and in numbers, before
forming a hypothesis about why it sounds wrong. For the bowed string this step
overturned the premise: the lexicon's `string` entry had no bow at all, its `bow`
axis was the DENSITY of `dust` impulses into `streson`, and the one real bow model in
this Csound (`wgbow`, STK 1995) ran 1.6–1.9 octaves too bright at the plugin's 4×
rate because its loss filter is specified relative to the sample rate. Neither is
visible from the panel; both are visible in a render.

Tools:

- `tools/lco_measure.py` — `render(body, dur, freq, preroll)` through the REAL
  orchestra head (`lco_write.wrap`), plus `f0`, `cents`, `centroid_audible`,
  `partials`, `comb_contrast`, `loudness_travel`, `HOST_CLIP`.
- `tools/lco_axis_probe.py --body <key> --gate` — the shipped gate: every axis at its
  anchors across six registers (55–1760 Hz), loudness spread, movement, onset peak
  against the host clip. Run it on the INCUMBENT first; its numbers are the baseline
  the challenger is read against.
- `tools/lco_pitch_probe.py --body <key>` — tuning of the first partial in cents,
  capture failures (a partial at 2f or 3f stronger than f) and period doubling (energy
  at f/2), per register and per engine rate. These are the three ways a physical
  model plays the wrong note while compiling cleanly; see §5.

Render the incumbent's audio to disk NOW, as `nearest_existing__*.wav` in the A/B
folder that §8 will fill. This is CLAUDE.md's rule 4 in its operational form: the
thing to beat is on disk before the first line of the new orchestra exists.

Measure at BOTH engine rates, 44100 and 176400. The plugin runs Csound at the host
rate times the oversampling factor (default 4×, capped at 200 kHz by
`effectiveOversampleFactor`); every opcode whose coefficients are sr-relative
(`wgbow`'s loss pole, `wgclar`, the STK family) is a different instrument at 176400.
A body judged at 44100 alone has not been judged.

## 2. Put the external reference on disk, and let BJ read it

An entry that takes a real instrument's name is compared against that instrument
(CLAUDE.md rule 3, "the comparison target follows the claim"). The reference is
rendered BEFORE any design decision, into `tools/lco_reference_out/<word>/`, with a
`PROMPT.txt` beside it recording the exact prompt, model, steps, cfg, seconds and
date — so a later session knows what it is listening to. For the strings: four SA3
renders ("solo cello|violin, one instrument alone, single sustained bowed note, close
microphone, dry studio recording, unaccompanied, no reverb", SA3 medium, 50 steps,
cfg 6, two seeds each). Measure the reference too (every one of them carried vibrato,
30–36 cents at 4.6–5.8 Hz); the numbers say what the reference IS, and only BJ says
whether it is the reference ("schon sehr gut").

The `.gitignore` must be TESTED for the new files, both directions
(`git check-ignore -v` on the real paths; `git ls-files | git check-ignore --stdin`
empty). The string references were named `cello_sa3_*.wav`, which the existing rule
`sa3_*.wav` did not match; a rule was added and tested. A reference WAV that lands in
the repository is a licence problem, not a tidiness problem.

## 3. Read the substrate in source and the field in the literature; write the analysis

Before proposing anything: read the opcodes this Csound actually ships for the word
(`csound -z1` lists them; the method and its parameters are in the C source, not the
manual — `wgbow`'s single fixed loss pole and single body biquad were read in
`Opcodes/physmod.c`), and read what the field has done since the model was written.
For bowed strings that was: friction-curve (MSW 1983, Smith 1986) → thermal (Smith &
Woodhouse 2000) → elasto-plastic (Serafin 2003; Willemsen 2019, real-time) →
combined (van Walstijn 2026); measured bridge-admittance bodies (Maestre 2013).

Write `docs/plans/<WORD>_ANALYSIS.md` with: the verdict in order of weight (§6 of the
example), the sources read (§8), and the ROUTES with one recommendation (§7). A route
is costed in what it changes: orchestra level (a UDO or body), opcode level (a Csound
plugin), or author level (what the LLM is told). Then STOP. The analysis is answered
by BJ ("bau das so" chose Route B and said Route C waits for his verdict). Nothing is
built on an unproven premise and nothing is built that he has not asked for
(memories: `no_structure_on_unproven_premise`, `no_unauthorized_sound_features`).

## 4. Method and source, written before the first orchestra line

The deliverable of this step is a paragraph, not code: WHICH synthesis method, from
WHICH source, and what differs from the opcode the substrate already has. It is
written into the docstring of the A/B tool (`tools/lco_ab_<word>.py`, see §8) and,
for a head opcode, into the comment block above the UDO. For the bowed string:
digital waveguide in velocity waves (Smith, ICMC 1986; PASP "Digital Waveguide
Bowed-String", "The Bow-String Scattering Junction"), friction curve
`rho = (|Δv|·slope + 0.75)^-4` (McIntyre/Schumacher/Woodhouse 1983; Cook's form in
STK `BowTable`), loop filter after Jaffe & Smith 1983, body modes from Woodhouse
(Euphonics 5.3: A0 272, CBR 407, B1− 462, B1+ 551 Hz, bridge hill ~2.3 kHz). The one
line "what is different from `wgbow`" (loss filter in Hz; a measured body) is what
justifies building at all.

Cannot name the method AND the source → nothing is built (CLAUDE.md rule 1).

## 5. Prototype at orchestra level, against the real head, and measure the model's own behaviour

The prototype is a UDO plus a body, rendered through `lco_write.wrap` so it meets the
same `sr`, `ksmps`, `0dbfs`, control channels and 16 always-on instances the plugin
gives it. Scratch scripts are fine for the grid runs (they were, for the bowed string:
`proto.py`, `grid.py`, `peaks.py`); what they MEASURE is not optional, and
`tools/lco_pitch_probe.py` now does the three pitch measurements on any body so they
are not reinvented per instrument:

1. **Free-string tuning.** Disable the excitation's nonlinearity (or drive the loop
   with one impulse) and measure the first partial: a resonator must land on the note
   to the cent. If it does not, the correction is DERIVED from the model's own
   mathematics, never fitted: the bowed string's loss filter adds a phase delay at the
   fundamental, and that delay is computed from `tone`'s own pole formula
   (`kph = atan2(p·sin ω, 1 − p·cos ω) / ω`) and subtracted from the nut side. Result
   0.00 cents. A measured constant in its place would have been CLAUDE.md rule 2's
   "fitted curve laid over the output".
2. **Driven tuning.** With the nonlinearity on, the note may sit off by the model's
   nature (the bowed string: +0.5 c at 55 Hz, +4 c at 440, +13 c at 1320, because the
   hair catches the smeared Helmholtz corner early). That is DECLARED in the entry
   (`; FOLLOWS kfreq: yes, …`), with the numbers, and not tuned out — tuning it out
   would again be a fit over the model's output, and it would change with force,
   place and darkness anyway.
3. **Wrong-note modes.** Capture failure (the model locks onto 2f or 3f: a partial
   there stronger than at f, and no partial left near f to measure) and period
   doubling (energy at f/2) are where the parameter ranges come from. A body
   resonance can lift the second or third partial above the fundamental while the
   string still plays f (the bowed string at 880 Hz, under its 2.3 kHz bridge hill):
   the probe's `cents` column tells the two apart. The grid is registers × the axis values; the range of
   each axis is bounded where the mode begins, with margin (bow place stops at 0.20
   because 0.22 and up period-doubles away from the bridge; minimum force is kept
   above Schelleng's capture limit by the arm's speed ramp).

Beyond pitch, the gate's own three: one loudness (spread across anchors and
registers ≤ 1 dB), movement (the sound must move: CLAUDE.md and
`project_lco_movement_by_default`), onset peak against `HOST_CLIP` (2.746). And the
cost: CPU per voice at 4× for 16 voices (`sample(1)` methodology in
`docs/PERFORMANCE_GUIDE.md`; the bowed string is 1.9 % of a core per voice against
0.8 % for the incumbent, which is written down so the next body is read against it).

Audible-band equality across rates is the test that the Hz-specified design worked:
`centroid_audible` at 44100 and 176400 must agree even where the full-band centroid
does not (the 4× render carries ultrasonic stick-slip content that is simply not
there at 44100).

### Csound traps met on the way (each cost a build; none is in the manual's words)

- `setksmps` inside a UDO accepts an i-time expression, but the value MUST divide the
  global `ksmps` (64 here). `int(sr/44100)` is 3 at 132300 and at 176224 (44.1 k
  pull-down × 4): INIT ERROR, the note is deleted, all 16 voices are silent, and the
  compile still reports success. The sub-block is the largest power of two ≤
  round(sr/44100).
- A delay-line loop closed through `delayr`/`delayw` is written one k-period LATE:
  both taps are shortened by one sub-block or the loop is longer than the period at
  every rate. `deltapi` cannot read closer than one k-period plus one sample to the
  write.
- `limit` with low > high returns their average, silently. A floor computed from
  `kfreq` (`3 * kf`) crosses the ceiling at high pitch; write `min(3 * kf, 20000)`.
- `balance` pumps a body that is nearly silent (the string below its own second
  partial at a dark cutoff): the gate's 134× onset spike at one corner came from
  this. Bound the source so it never goes quiet, rather than taming the balance.
- i-time events fire once at instrument build; k-time is where a body lives
  (`project_lco_body_traps_oversampling_and_irate`).
- `dcblock2` at 4× behaves differently from 1× (same memory).

## 6. Axes: each one earns its place by measurement, and ranges are where the model behaves

An axis exists if it moves something a player would reach for, monotonically, over a
range where the model stays on the note. For each axis the entry records the
measured effect at its anchors (the `params.<axis>.note`), named anchors with a
gloss (`ponticello / normale / tasto`; `gut / wound / steel`; `cello / viola /
violin`), and the range. An axis that fails this is DROPPED, not shipped at a
reduced range: `press` (bow force) moved the centroid by a tenth, non-monotonically,
and below slope 1.25 period-doubled; the weight is fixed at 2.0 and the axis is gone
(memory: `quality_bar_omit_mediocre`). The axis comment line is the player's control
surface (`project_lco_params_are_the_user_surface`), so its range brackets are what
the panel offers; `lco_axis_probe.with_axis` rewrites that line to make the
`anchor_code` bodies.

Two declarations every body carries (CLAUDE.md rule 3): whether it follows `kfreq`
and to what pitch, and whether it stands or decays (`; DECAY: SELF` where it does).
One loudness is the yardstick `balance` against a fixed 400 Hz sine, as `string` has
it, so the axes move colour and never level. Movement by default comes from the
PHYSICAL layer that owns it — the bow arm (bite-and-release at note start, slow
drift of weight and speed), none of it on the pitch (`feedback_no_builtin_vibrato`;
the SA3 references' vibrato is the player's, not the string's, and is NOT copied).

## 7. The entry, the routing, the library, the author's notice

- `backend/dco_lexicon.json`: the new entry with `key`, `display_name`,
  `surface_forms` (every word that must open it, both languages), `priority`, `why`
  (what it is, what it is NOT, which neighbour owns the other reading), `code` (the
  body, comments included — the author reads them), `params` with notes,
  `anchor_code` (generated with `with_axis`, keyed `"{axis}={value}  ({anchor}:
  {gloss})"`), `heard` with `status: "offen"` and the date. Bump `lexicon_version`.
- Move surface forms away from the incumbent where the word now means the new body
  (violin, cello, bowed, arco … left `string`; plucked words stayed). Edit the
  incumbent's `why` to point across. Do NOT touch its `code`, `params` or
  `anchor_code`: those are the approval hash (`tools/lco_approvals.py`), and the
  incumbent's `heard` must survive the change (`string` did; `singing_bowl` had been
  broken by a comment-only edit three months earlier and lost its `curated` line in
  this rebuild — the restamp needs BJ's words, nobody else's).
- `tools/lco_build_library.py` then `--check` ("library is in step"). The library is
  GENERATED; never edit `backend/lco_library.json` by hand.
- If a head opcode was added: `_SYSTEM_HEAD`'s facility list in `backend/lco_write.py`
  gets its signature and ranges, in the same numbers as the entry (a mismatch here
  was one of the verification findings).
- Verify the routing in code, not by assumption: `lco_write.named_entries(prompt)`
  and `open_entries(prompt)` for the words moved, and for the words that must stay.
  Check the consulted page's size against the incumbent's (54 k characters either
  way for the strings): the author's context is 64 k tokens and already overflowed
  once on a bowed prompt — that overflow is its own open defect, not this entry's.

## 8. The A/B folder: what BJ hears, in his terms

`tools/lco_ab_<word>.py`, after the pattern of `tools/lco_ab_bowed_waveguide.py`:

- sides `nearest_existing` (the incumbent at the anchor that made the claim — `string`
  at `bow=1.0`, the body the bank's "Bowed String" preset carries), `new`, and the
  anchors worth hearing on their own (`new_cello` = `box=0.37`);
- the 4× engine rate, three registers (A2 / A3 / A4), four seconds, 0.5 s preroll;
- RMS-matched per pitch to the incumbent, then ONE common gain over all files so
  nothing is louder because it is new;
- the bodies written beside the audio as `.orc` (tracked) so what was heard is on
  record; WAVs, npz and `levels.json` fall under the `tools/lco_ab_*/` ignore rules;
- the external references copied in as `reference__*.wav`.

Then `open` the folder and give the paths. No WAV in the chat
(`feedback_audio_delivery`). The entry's `heard` stays `offen` until BJ has spoken;
own measurements accompany, never decide (CLAUDE.md rule 3; repeating a
self-assessment N times is one assessment). A routing change is user-visible the
moment the library is rebuilt, so it is reported as such even while the sound waits
for his ear.

## 9. Verification, commits, and reaching the synth

- Behaviour changed → one verification agent (model opus, never fable), brief: "This
  code has a bug. Find it." Its findings are applied before the commit, and comments
  it proves wrong are fixed with the code (the bowed string: the `setksmps` divisor,
  a wrong direction word, an unreachable bound). Re-measure after the fix: the rate
  sweep (44100, 48000, 88200, 96000, 132300, 160000, 176224, 176400, 192000, 200000:
  same RMS, same pitch) is the proof the divisor fix needed.
- Commits by concern, staged by named path, never `-A`: the head opcode; the lexicon
  and the library together (they must not drift); the A/B tool with its `.orc` bodies,
  the reference `PROMPT.txt` and any ignore rule; the analysis document. Guardian-path
  bodies say what changed in behaviour (the lexicon commit said that `singing_bowl`
  would leave the panel, and why). A `CHANGELOG.md` entry under Unreleased for the
  user-visible part.
- **The synth does not see any of it until the frozen backend is rebuilt.** The plugin
  launches `backend/dist/pipe_inference/pipe_inference` whenever it exists
  (`PipeInference::findBundledBinary`), and that bundle carries its OWN copy of
  `dco_lexicon.json` and `lco_library.json` from build time. The panel's instrument
  list, by contrast, reads `backend/lco_library.json` straight from the tree
  (`findLibraryFile`), so after a lexicon change the panel shows the new entry while
  the author still writes the old one — exactly what BJ saw with a `string` body for
  "violin". Rebuild with the CI's own command, `cd backend && pyinstaller
  pipe_inference.spec --noconfirm` (two minutes), with the synth QUIT: the running
  backend has the old directory mapped and the library cached, and swapping the
  files under it is not safe. Build into `--distpath` elsewhere if the synth cannot be
  quit yet, and move the result into place afterwards; keep the old dist renamed
  beside it rather than deleting it.
- Prove the wire, not the feature (`feedback_verify_the_wire_not_the_feature`): one
  `mode=csound` request over the plugin's IPC against the NEW binary
  (`tools/lco_trace_wire_check.py` shows the framing), and read `consultation.opened`
  and the body. For "violin" the author opened `bowed_string` and `string`, wrote in
  one attempt with no repair, and copied the entry's body with the violin anchor; for
  "a bowed cello, sustained" the same body with `box=0.37`. That is the architecture
  working (`project_lco_author_consults_library`): the author copies the library's
  body and sets the anchor the words name.

## 10. What the worked example left open (so the next one knows the limits)

- The bowed note sits sharp by the model's nature (declared, §5.2); a bow with a
  thermal or elasto-plastic friction model (Route C, a Csound plugin opcode) is the
  route past it, and waits for BJ's verdict on Route B.
- The author copies the default body verbatim for a one-word prompt. Whether it
  varies the axes for richer prompts ("a dark cello, bow near the bridge") has not
  been measured.
- The author's 64 k context overflowed on "a solo violin, bowed, singing" before this
  entry existed; the consulted page is the same size now. Own defect, own order.
- One authoring took 7–17 minutes with two or three llama instances sharing the
  machine; the author's speed alone, and whether the GGUF runs on Metal in the
  plugin, were not measured.

## Checklist

1. Verdict verbatim in the record; the WORD named.
2. Incumbent measured at 44100 and 176400; `nearest_existing__*.wav` on disk.
3. External reference on disk with `PROMPT.txt`; ignore rules tested both ways.
4. Substrate read in source; literature read; analysis with routes; BJ chose.
5. Method + source paragraph written (A/B docstring, UDO comment).
6. Free tuning exact by derivation; driven tuning declared; capture and period
   doubling mapped; ranges set with margin; CPU per voice known.
7. Axes earn their place or are dropped; declarations; one loudness; movement from
   the physical layer, none on pitch.
8. Entry, moved words, incumbent's sound fields untouched, library rebuilt and
   checked, `_SYSTEM_HEAD` in step, routing verified in code.
9. A/B folder RMS-matched with references; opened for BJ; `heard: offen`.
10. Verification agent; rate sweep; commits by concern; changelog.
11. Frozen backend rebuilt with the synth quit; author proven over the IPC wire.
