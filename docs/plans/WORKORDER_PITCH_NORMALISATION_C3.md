# Work order — pitch normalisation for "c3" prompts (investigation, not implementation)

Written 2026-11-07 for a fresh session. BJ's order, verbatim:

> „schreib mal einen Arbeitsauftrag-Prompt zur Überprüfung ob bei "c3"-angaben eine
> Tonhöhennormalisierung möglich ist (oder wir geben einen C3-Schalter zu den Prompt-Feldern)"

Context from the same day, verbatim: „dieses (Kern-) Feature [a held note regenerating onto a
new sample] ist musikalisch eher problematisch bzw. extrem experimentell, weil ohnehin hierbei
zB auch die tonhöhen oder die Oktaven wandern und wir keine Tonhöhen-Normalisierung haben, bisher".

**Scope of this order: answer the question with measurements and put a recommendation in front
of BJ. Build nothing in `src/` or `backend/` until BJ says so.** Measurement tools under `tools/`
are part of the investigation and are fine.

---

## 1. What is already true in the code (verified 2026-11-07; re-check before relying on it)

- **"c3" is an existing prompt convention, not a new idea.** The T5 Oscillator's dice appends
  `", c3"` to every entry of its pitched pools (`resources/t5_prompt_pairs.json`, the pools
  `waveform`, `event`, … — the `suffix` field); the Guide says it "puts the generated pitch on C"
  (`resources/akroasys_Guide.html`, the dice section). 48 of the 60 presets in the live bank
  (`~/Library/T5ynth/presets/*.t5p`) carry `c3` in a prompt string.
- **The sampler plays the generated buffer at unity on MIDI note 60.** `SynthVoice.cpp`, note-on:
  `ratio = tunedHz(shiftedNote) / tunedHz(60)` → `sampler.setTransposeRatio(ratio)`. Every other
  note goes through the Signalsmith stretcher (`SamplePlayer::renderPitchedBlock`), with a
  0.1-semitone dead zone around unity (`kNearUnitySemitones`, `SamplePlayer.cpp`) that keeps the
  direct-read path. Stretch latency is one full window (memory `project_stretcher_latency_measured`).
- **Naming trap.** MIDI 60 is C4 in scientific pitch notation and C3 in the Yamaha/Roland
  convention. Nobody has measured which octave the models mean by "c3" (130.8 Hz or 261.6 Hz, or
  neither reliably). Until that is measured, "normalise to C3" has no defined target.
- **There is no pitch estimation anywhere** in `src/`, `tools/` or `backend/` (grepped for pyin,
  crepe, yin, f0 estimation, root note). The backend has one machine-listening op, `"analyze"`
  (`backend/pipe_inference.py: analyze_audio`, CLAP tags + DSP spectral words, reached over IPC as
  described in `docs/IPC_PROTOCOL.md` §3.3). `torchaudio==2.7.1` is pinned in
  `backend/requirements.txt`.
- **Per-note cache positions** (`PluginProcessor.h`, "Per-note cache positions",
  `CachePositionEngines`) already generate and prepare one sampler buffer per position; any
  per-generation pitch correction would have to be per position too.
- **Held-note regenerate** (commit `0c3b32c5`, 2026-11-07): the outgoing side keeps its own
  playhead and loop geometry in `SamplePlayer::MorphTail`; the geometry travels inside
  `PlaybackSnapshot`. A per-buffer pitch correction would have to travel the same way and apply per
  side, or the crossfade mixes two buffers at one ratio and the wander this order is about stays.
- Memory rules in force: `feedback_no_selfmade_perceptual_meters` (an estimator needs a published
  method and a source), `feedback_one_note_once_proves_nothing`, `feedback_test_tool_ipc` (test
  the backend over IPC against the built binary), `feedback_realtime_short_sounds` (0.1–11 s,
  default 3 s), `feedback_never_fable_agents`, `feedback_no_unauthorized_sound_features`.

## 2. Questions to answer, each with a measurement

1. **Does "c3" land the pitch on C, and in which octave?** For each installed engine (Stable
   Audio Open Small, SA3 Small Music, SA3 Medium if present): take 10 pitched bank prompts that end
   in `, c3` and 10 of the same prompts with the suffix removed, generate N ≥ 8 seeds each at the
   default 3 s, estimate f0 over the stable part of the sound, and tabulate: share within ±50 cents
   of a C; octave distribution (C2/C3/C4/C5); share with no stable f0 (textures, noise). The same
   table for `, c4` and `, c2` tells whether the models read the number as an octave at all.
2. **How reliable is f0 estimation on this material?** Use two published estimators and report
   their agreement, never one alone and never a self-built one: pYIN (Mauch & Dixon 2014; in
   `librosa.pyin`, ISC licence) and CREPE (Kim, Salamon, Li, Bello 2018; `torchcrepe`, MIT) or
   `torchaudio.functional.detect_pitch_frequency` (already installed). Licence check per
   `docs/LCO_CODE_PROVENANCE.md` discipline before adding a dependency. Report octave errors
   separately from other errors.
3. **What does the wander actually look like on the regenerate path?** Take 5 presets, hold one
   note, regenerate 10 times (A/B drift as the player uses it), log f0 of each new buffer. This is
   the number that says whether normalisation is worth its cost.
4. **Where would a correction live, and what does each place cost?** Three candidates, each to be
   described with the file it touches and its cost, not built:
   - (a) **Offline resample at prepare time**: measure f0 after generation, resample the buffer by
     the ratio to the target in `SamplePlayer::preparePlaybackState` (or before it, in the
     backend) so MIDI 60 plays the target at unity. Keeps the direct-read path and the loop finder
     intact (it runs on the corrected buffer); changes duration and loop points; octave policy
     needed (nearest octave of the target, or exact?).
   - (b) **Per-buffer transpose ratio** carried in `PlaybackSnapshot`, applied per side in the
     morph. Keeps the material, puts every preset onto the stretch path (latency, quality).
   - (c) **Prompt-side only, the "C3 switch" next to the prompt fields** (BJ's own alternative):
     appends/strips `, c3` on both prompts, no measurement, no correction. Cheapest; it only asks
     the model and relies on what question 1 measures.
   - (d) **(c) plus (a)**: the switch names the target, the measurement corrects what the model
     missed.
5. **What should NOT be normalised?** Textures, noises, speech, nature scenes have no pitch; a
   correction there is damage. The decision needs the estimator's confidence/voicing output, and
   the thresholds need to be shown on the bank, not chosen.

## 3. Deliverables

- One report under `docs/plans/` with the tables from §2, the estimator agreement, the per-engine
  behaviour of `c3`, and the regenerate wander. Numbers in tables, not prose.
- A WAV folder on the Desktop for BJ's ear: for 5 presets, the 10 regenerated buffers as they
  are, and (if (a) is prototyped as a tool only) the same 10 corrected — BJ's ear decides whether
  normalised regenerates are musically better or merely flatter.
- A recommendation among (a)–(d), with its cost named, as a proposal to BJ. No UI element and no
  change to the sound path without BJ's yes. The C3 switch is BJ's own suggestion, so designing
  it is in scope; placing it in the panel is not until he says so.

## 4. Process

- Read `CLAUDE.md`, `ARCHITECTURE.md`, `docs/IPC_PROTOCOL.md` §3.3 and the memory index first.
- Generation for the measurement runs against the built backend over IPC (never by importing the
  backend into a side script), with the models the user actually has installed
  (`~/Library/T5ynth/models`).
- Verification agent (model opus) for any tool that computes a number that enters the report.
- Record BJ's answers verbatim in `memory/ACTIVE_WORKORDERS.md` as they arrive.
