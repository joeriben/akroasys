# Pitch normalisation for `c3` prompts: report

Date: 2026-11-07. Order: [`WORKORDER_PITCH_NORMALISATION_C3.md`](WORKORDER_PITCH_NORMALISATION_C3.md).
Investigation only: nothing in `src/` or `backend/` changed, no UI element, no change to the sound path.
Listening page: `~/Desktop/akroasys_pitch_c3/index.html`.

## 1. Answers

| Question | Answer |
|---|---|
| Does `, c3` land on C? | Yes, on the five Stable Audio engines. 97 to 100 % of clips with an agreed pitch are on C; bare prompts 17 to 59 %. `, g3` lands on G 93 to 100 %, so the models read the letter. Clips on C sit a median 1.6 cents from exact C (95th percentile 15 c). AudioLDM2 gives no agreed pitch on 91 to 95 % of its clips with or without the word, so the word shows no measurable effect there. |
| In which octave? | Not reliably. The prompt's own register decides as much as the digit. Under `, c3` the share in C3 is 48 to 74 %. On SA3 `c3` means 130.8 Hz, scientific naming; on SAO it is split between C3 and C4. |
| Can f0 be estimated on this material? | Yes, where there is one pitch. pYIN and CREPE agree on 91 % of the clips both call stable, 6 % an octave apart, 3 % otherwise. Bells and chords are unreliable: on an inharmonic bell all three land one or two octaves below the prime, on a triad none finds the root reliably. |
| What does a regenerate do? | With the seed held, pitch stays: 0 to 40 cents across 50 buffers. With a new seed, the octave jumps in every preset. A drift sweep much wider than the presets' own jumps the octave in three of the five presets with an agreed pitch, seed held. The note name stays C, apart from two presets with a G: one's B prompt says `g3`, the other's G4 is the third partial of C3. |
| Where would a correction live? | §7. A pitch-class correction has almost nothing to do once `c3` is in the prompt. What remains is the octave, and moving a buffer an octave by resampling halves or doubles its length. |
| What must not be normalised? | Anything without an agreed pitch. The agreement of pYIN and CREPE marks 3 of 480 bare control clips as pitched. The shipped detector with two cut-offs marks 1 of 480. Without those cut-offs it marks 161 of 480. |

Recommendation in §10: build the C3 switch, off by default; build no resampling correction; let the ear decide on the octave.

## 2. Corrections to the work order's §1

| Order says | Measured or read |
|---|---|
| There is no pitch estimation anywhere | `WavetableOscillator::analyzePitchWindow` and `detectPitch` are a YIN, used for wavetable extraction in wavetable mode (`prepareFramesFromBuffer`; other modes take `prepareContiguousFrames`, which detects nothing). `tools/lco_measure.py` has an f0 as well. This study ported the shipped one and measured it. |
| 48 of 60 presets carry `c3` | 105 presets live: 46 published, 30 of them with `c3` in a prompt; 59 local, 37 with `c3`. |
| (b) puts every preset onto the stretch path | Every note except unity is on it already. Its real cost is in §7. |
| Nobody has measured which octave | Measured, §4. |

Not in the order and relevant: `Music::parse` and `Music::parseAB` in `src/inference/RepromptStances.h` already read a trailing note into pitch class, octave and MIDI number. Nothing in `src/` calls them yet; `tools/test_reprompt_stances.cpp` tests them. `trailingMusicSuffix` and `reattachMusicSuffix` are in use. The wavetable engine is pitch-synchronous, so with a detected pitch it plays the key's pitch whatever the buffer holds. Wander concerns the sampler and freeze.

## 3. Method

- **Generation.** Over IPC against `backend/dist/pipe_inference/pipe_inference`, with the request fields of `PromptPanel::buildInferenceRequest` and the forced steps and CFG per model. Default 3 s. Nothing was recorded from the built Standalone: the order specifies IPC, and the buffers are the ones the Standalone would receive for the same request, so this measures the model and not the playback path.
- **Matrix.** Ten pitched bank prompts (`steady saw wave`, `steady square wave`, `Hammond organ`, `a violin`, `cello`, `a trombone`, `sitar`, `tubular bells`, `a fog horn`, `male opera baritone voice singing`), each bare and with `, c3`, `, c2`, `, c4`, `, g3`. Same seeds in every cell. 16 seeds per cell, 8 for AudioLDM2, whose clips take far longer. `, g3` is an addition to the order: a share on C says nothing about the letter unless another letter moves it. Ten prompts without a pitch as controls, bare and with `, c3`, 8 seeds each. 5,360 clips. The design was fixed in the tool before anything was measured. Engines: all installed ones; `stable-audio-3-small-sfx` is not installed here. `sa3-medium-sfx` is the medium checkpoint with track type `sfx`.
- **Estimators.** pYIN (Mauch & Dixon 2014, `librosa.pyin`) and CREPE (Kim, Salamon, Li, Bello 2018, `torchcrepe`, model `full`, Viterbi, voicing threshold 0.21, the library's documented example). Third, a port of the shipped YIN (de Cheveigné & Kawahara 2002), checked against a verbatim float32 copy of the C++ on 60 real clips: same window and candidate counts, pitch within 0.0007 c.
- **Clip summary, declared as ours and without a source.** Plain descriptive statistics, no perceptual claim: active frames within 40 dB of the loudest; voiced share; dominant pitch as the median inside the best 100-cent window; concentration as the share of voiced frames in that window. Provisional cut-offs voiced ≥ 0.5, concentration ≥ 0.5, swept in §8.
- **Consensus.** A clip counts as pitched only when pYIN and CREPE both call it stable and agree within 50 cents. The value is pYIN's. Chance of landing within ±50 c of one pitch class is 9 % on pYIN's ten-cent grid.
- **Licences.** `librosa` ISC, `torchcrepe` MIT (weights in the wheel, 89 MB), `numba` BSD, and `soxr` LGPL-2.1-or-later, which does the resampling in these tools. They sit in a venv of their own under `~/.cache`; the project gained no dependency. Nothing in the recommendation needs them in the product.

## 4. Question 1: what `, c3` does

### Share on the named pitch class

Consensus clips only. Chance is 9 %: 11 of the 120 ten-cent bins pYIN reports fall within ±50 c of one pitch class. Range = lowest and highest prompt.

| Engine | Suffix | Clips | No consensus f0 | Consensus | On C | 95 % CI | Range over prompts | On G |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| sao-small | none | 160 | 40 % | 96 | 17 % | 11–25 % | 0–53 % | 4 % |
| sao-small | c3 | 160 | 8 % | 148 | 97 % | 92–99 % | 80–100 % | 1 % |
| sao-small | c2 | 160 | 8 % | 147 | 97 % | 92–99 % | 71–100 % | 1 % |
| sao-small | c4 | 160 | 7 % | 149 | 98 % | 94–99 % | 81–100 % | 0 % |
| sao-small | g3 | 160 | 11 % | 143 | 0 % | 0–3 % | 0–0 % | 93 % |
| sao-1.0 | none | 160 | 34 % | 106 | 18 % | 12–26 % | 0–100 % | 1 % |
| sao-1.0 | c3 | 160 | 19 % | 130 | 99 % | 96–100 % | 93–100 % | 0 % |
| sao-1.0 | c2 | 160 | 26 % | 118 | 99 % | 95–100 % | 91–100 % | 0 % |
| sao-1.0 | c4 | 160 | 14 % | 137 | 100 % | 97–100 % | 100–100 % | 0 % |
| sao-1.0 | g3 | 160 | 15 % | 136 | 0 % | 0–3 % | 0–0 % | 93 % |
| sa3-small-music | none | 160 | 50 % | 80 | 59 % | 48–69 % | 0–100 % | 6 % |
| sa3-small-music | c3 | 160 | 18 % | 132 | 100 % | 97–100 % | 100–100 % | 0 % |
| sa3-small-music | c2 | 160 | 7 % | 149 | 100 % | 97–100 % | 100–100 % | 0 % |
| sa3-small-music | c4 | 160 | 4 % | 153 | 100 % | 98–100 % | 100–100 % | 0 % |
| sa3-small-music | g3 | 160 | 3 % | 155 | 0 % | 0–2 % | 0–0 % | 96 % |
| sa3-medium | none | 160 | 33 % | 107 | 47 % | 38–56 % | 0–100 % | 5 % |
| sa3-medium | c3 | 160 | 4 % | 154 | 100 % | 98–100 % | 100–100 % | 0 % |
| sa3-medium | c2 | 160 | 7 % | 149 | 100 % | 97–100 % | 100–100 % | 0 % |
| sa3-medium | c4 | 160 | 7 % | 149 | 100 % | 97–100 % | 100–100 % | 0 % |
| sa3-medium | g3 | 160 | 9 % | 146 | 0 % | 0–3 % | 0–0 % | 100 % |
| sa3-medium-sfx | none | 160 | 49 % | 82 | 23 % | 15–33 % | 0–100 % | 10 % |
| sa3-medium-sfx | c3 | 160 | 8 % | 147 | 99 % | 96–100 % | 93–100 % | 0 % |
| sa3-medium-sfx | c2 | 160 | 16 % | 134 | 99 % | 96–100 % | 92–100 % | 0 % |
| sa3-medium-sfx | c4 | 160 | 10 % | 144 | 100 % | 97–100 % | 100–100 % | 0 % |
| sa3-medium-sfx | g3 | 160 | 8 % | 147 | 0 % | 0–3 % | 0–0 % | 99 % |
| audioldm2 | none | 80 | 92 % | 6 | 0 % | 0–39 % | 0–0 % | 33 % |
| audioldm2 | c3 | 80 | 95 % | 4 | 50 % | 15–85 % | 0–100 % | 0 % |
| audioldm2 | c2 | 80 | 91 % | 7 | 29 % | 8–64 % | 0–100 % | 0 % |
| audioldm2 | c4 | 80 | 94 % | 5 | 40 % | 12–77 % | 0–100 % | 20 % |
| audioldm2 | g3 | 80 | 92 % | 6 | 17 % | 3–56 % | 0–100 % | 0 % |

### Octave of the clips that landed on C (scientific pitch: C4 = MIDI 60 = 261.6 Hz)

| Engine | Suffix | On C | C1 | C2 | C3 | C4 | C5 | C6 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| sao-small | none | 16 | 0 | 0 | 7 | 4 | 2 | 3 |
| sao-small | c2 | 142 | 9 | 15 | 96 | 15 | 4 | 3 |
| sao-small | c3 | 143 | 0 | 0 | 68 | 63 | 8 | 4 |
| sao-small | c4 | 146 | 0 | 0 | 26 | 79 | 36 | 5 |
| sao-1.0 | none | 19 | 0 | 2 | 6 | 8 | 2 | 1 |
| sao-1.0 | c2 | 117 | 3 | 21 | 79 | 9 | 2 | 3 |
| sao-1.0 | c3 | 129 | 0 | 0 | 64 | 57 | 3 | 5 |
| sao-1.0 | c4 | 137 | 0 | 0 | 28 | 76 | 29 | 4 |
| sa3-small-music | none | 47 | 0 | 13 | 30 | 0 | 1 | 3 |
| sa3-small-music | c2 | 149 | 0 | 39 | 110 | 0 | 0 | 0 |
| sa3-small-music | c3 | 132 | 0 | 0 | 98 | 33 | 1 | 0 |
| sa3-small-music | c4 | 153 | 0 | 0 | 0 | 111 | 42 | 0 |
| sa3-medium | none | 50 | 3 | 10 | 26 | 7 | 4 | 0 |
| sa3-medium | c2 | 149 | 0 | 89 | 52 | 3 | 5 | 0 |
| sa3-medium | c3 | 154 | 0 | 19 | 90 | 35 | 10 | 0 |
| sa3-medium | c4 | 149 | 0 | 0 | 8 | 106 | 33 | 2 |
| sa3-medium-sfx | none | 19 | 1 | 2 | 11 | 2 | 3 | 0 |
| sa3-medium-sfx | c2 | 133 | 0 | 73 | 57 | 2 | 1 | 0 |
| sa3-medium-sfx | c3 | 146 | 0 | 27 | 73 | 42 | 4 | 0 |
| sa3-medium-sfx | c4 | 144 | 0 | 4 | 15 | 84 | 41 | 0 |
| audioldm2 | none | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| audioldm2 | c2 | 2 | 0 | 0 | 0 | 0 | 1 | 1 |
| audioldm2 | c3 | 2 | 0 | 0 | 0 | 0 | 1 | 1 |
| audioldm2 | c4 | 2 | 0 | 0 | 0 | 0 | 1 | 1 |

### Does the number move the octave? Same prompt, same seed, c2 → c4

The pairs hold everything but the digit. A model that reads the digit as an octave gives +24.

| Engine | Pairs, both consensus | Median shift, semitones | Exactly +24 (±0.5) | Unchanged (±0.5) | Other |
|---|---:|---:|---:|---:|---:|
| sao-small | 142 | +12.0 | 18 % | 23 % | 59 % |
| sao-1.0 | 109 | +12.0 | 20 % | 13 % | 67 % |
| sa3-small-music | 145 | +23.9 | 50 % | 0 % | 50 % |
| sa3-medium | 142 | +24.0 | 57 % | 1 % | 42 % |
| sa3-medium-sfx | 128 | +24.0 | 45 % | 4 % | 52 % |
| audioldm2 | 4 | +0.0 | 0 % | 100 % | 0 % |

### Median pitch by suffix, all consensus clips

| Engine | none | c2 | c3 | c4 | g3 |
|---|---:|---:|---:|---:|---:|
| sao-small | 60.0 (C4) | 48.0 (C3) | 59.8 (C4) | 60.0 (C4) | 67.0 (G4) |
| sao-1.0 | 62.5 (D#4) | 48.0 (C3) | 56.7 (A3) | 60.0 (C4) | 67.0 (G4) |
| sa3-small-music | 48.0 (C3) | 48.0 (C3) | 48.0 (C3) | 60.0 (C4) | 55.0 (G3) |
| sa3-medium | 48.1 (C3) | 36.1 (C2) | 48.0 (C3) | 60.0 (C4) | 55.0 (G3) |
| sa3-medium-sfx | 50.4 (D3) | 36.1 (C2) | 48.0 (C3) | 60.0 (C4) | 55.0 (G3) |
| audioldm2 | 58.1 (A#3) | 72.8 (C#5) | 62.2 (D4) | 55.2 (G3) | 74.0 (D5) |

### Per prompt, share on C, none → c3

| Prompt | sao-small | sao-1.0 | sa3-small-music | sa3-medium | sa3-medium-sfx | audioldm2 |
|---|---:|---:|---:|---:|---:|---:|
| Hammond organ | 3/7 → 15/15 | 1/1 → 8/8 | 11/11 → 3/3 | 12/12 → 16/16 | 2/3 → 16/16 | 0/0 → 0/0 |
| a fog horn | 0/14 → 16/16 | 0/15 → 15/15 | 2/10 → 16/16 | 0/13 → 16/16 | 0/13 → 14/15 | 0/2 → 0/0 |
| a trombone | 0/11 → 16/16 | 0/13 → 16/16 | 0/10 → 16/16 | 3/8 → 16/16 | 0/11 → 16/16 | 0/3 → 0/0 |
| a violin | 0/10 → 16/16 | 0/15 → 16/16 | 1/9 → 16/16 | 0/14 → 16/16 | 0/13 → 16/16 | 0/1 → 0/0 |
| cello | 0/14 → 15/15 | 0/15 → 8/8 | 1/3 → 16/16 | 4/13 → 16/16 | 0/8 → 16/16 | 0/0 → 0/2 |
| male opera baritone voice singing | 4/12 → 12/15 | 5/14 → 13/14 | 0/0 → 9/9 | 0/1 → 16/16 | 0/1 → 16/16 | 0/0 → 1/1 |
| sitar | 0/1 → 16/16 | 1/1 → 16/16 | 10/10 → 16/16 | 11/16 → 16/16 | 11/15 → 16/16 | 0/0 → 0/0 |
| steady saw wave | 8/15 → 16/16 | 11/15 → 16/16 | 14/15 → 16/16 | 15/16 → 16/16 | 2/2 → 16/16 | 0/0 → 0/0 |
| steady square wave | 1/10 → 14/16 | 1/13 → 16/16 | 5/6 → 16/16 | 4/11 → 16/16 | 0/12 → 16/16 | 0/0 → 0/0 |
| tubular bells | 0/2 → 7/7 | 0/4 → 5/5 | 3/6 → 8/8 | 1/3 → 10/10 | 4/4 → 4/4 | 0/0 → 1/1 |

### From one seed to the next: octave under `, c3`

Per prompt, over ALL clips of the prompt, so a clip off C or without an agreed pitch counts as a miss. Same octave twice: two different seeds drawn from the prompt's clips, the chance that both are on C in one octave, averaged over prompts. Of those known: the same, among pairs where both clips are on C. Tries: generations until one lands on C in the prompt's most frequent octave, median and worst prompt; strict counts a clip without an agreed pitch as a miss, lenient lets it pass because nothing can reject it. Never: no clip of that prompt ever qualified. Time: median generation of one clip on this Mac.

| Engine | Prompts | Octaves per prompt, median | Same octave twice | Of those known | Tries strict, median | Strict, worst prompt | Tries lenient, median | Lenient, worst prompt | Seconds per clip |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sao-small | 10 | 2.5 | 54 % | 64 % | 1.3 | 4.0 | 1.2 | 1.8 | 0.87 |
| sao-1.0 | 10 | 1.5 | 62 % | 83 % | 1.2 | 3.2 | 1.1 | 1.2 | 1.57 |
| sa3-small-music | 10 | 1.0 | 69 % | 87 % | 1.1 | 5.3 | 1.0 | 1.1 | 0.39 |
| sa3-medium | 10 | 2.0 | 75 % | 82 % | 1.1 | 1.8 | 1.1 | 1.8 | 0.80 |
| sa3-medium-sfx | 10 | 1.5 | 70 % | 81 % | 1.1 | 4.0 | 1.0 | 1.6 | 0.77 |
| audioldm2 | 10 | 1.0 | 0 % | – | never | never | 1.0 | 1.3 | 8.44 |
### The octave under `, c3`, prompt by prompt

Consensus clips on C, as counts per octave: C2/C3/C4/C5. Seeds per cell: sao-small 16, sao-1.0 16, sa3-small-music 16, sa3-medium 16, sa3-medium-sfx 16, audioldm2 8. Other octaves are left out; a dash is no clip on C.

| Prompt | sao-small | sao-1.0 | sa3-small-music | sa3-medium | sa3-medium-sfx | audioldm2 |
|---|---:|---:|---:|---:|---:|---:|
| Hammond organ | 0/2/12/1 | 0/0/6/2 | 0/3/0/0 | 0/15/1/0 | 0/15/1/0 | – |
| a fog horn | 0/10/5/1 | 0/13/2/0 | 0/16/0/0 | 3/13/0/0 | 5/9/0/0 | – |
| a trombone | 0/2/13/1 | 0/0/16/0 | 0/16/0/0 | 0/6/10/0 | 0/0/16/0 | – |
| a violin | 0/0/13/3 | 0/0/15/1 | 0/0/16/0 | 0/0/16/0 | 0/0/16/0 | – |
| cello | 0/4/10/1 | 0/3/5/0 | 0/14/2/0 | 0/9/7/0 | 0/10/6/0 | – |
| male opera baritone voice singing | 0/12/0/0 | 0/13/0/0 | 0/2/7/0 | 0/15/1/0 | 0/13/3/0 | 0/0/0/0 |
| sitar | 0/16/0/0 | 0/16/0/0 | 0/16/0/0 | 0/16/0/0 | 0/16/0/0 | – |
| steady saw wave | 0/9/7/0 | 0/3/13/0 | 0/14/2/0 | 1/15/0/0 | 6/10/0/0 | – |
| steady square wave | 0/12/2/0 | 0/16/0/0 | 0/16/0/0 | 15/1/0/0 | 16/0/0/0 | – |
| tubular bells | 0/1/1/1 | 0/0/0/0 | 0/1/6/1 | 0/0/0/10 | 0/0/0/4 | 0/0/0/1 |

AudioLDM2 is the exception throughout: 91 to 95 % of its clips have no agreed pitch in every cell, and its estimators agree on only 32 % of what both call stable, so its rows carry no finding about the word.

The per-prompt table explains the octave table. `, c3` does not pick an octave for the prompt; it picks the note, and the source keeps its register: `a violin` sits in C4 on all five Stable Audio engines, `sitar` in C3, `tubular bells` in C5 on SA3 medium, `steady square wave` in C2 on SA3 medium. A few prompts split between two octaves from seed to seed (`cello`, `a trombone`, `steady saw wave`).

### How far from an exact C the clips on C sit

Cents from the nearest C, signed for the median. pYIN value, so ±5 c is the grid of the estimator, not the model.

| Engine | Suffix | On C | Median | Median of the absolute | 95th percentile of the absolute |
|---|---|---:|---:|---:|---:|
| sao-small | c2 | 142 | +0 c | 0 c | 20 c |
| sao-small | c3 | 143 | +0 c | 0 c | 20 c |
| sao-small | c4 | 146 | +0 c | 0 c | 30 c |
| sao-1.0 | c2 | 117 | +0 c | 0 c | 20 c |
| sao-1.0 | c3 | 129 | +0 c | 0 c | 20 c |
| sao-1.0 | c4 | 137 | +0 c | 0 c | 20 c |
| sa3-small-music | c2 | 149 | +0 c | 0 c | 10 c |
| sa3-small-music | c3 | 132 | +0 c | 0 c | 20 c |
| sa3-small-music | c4 | 153 | +0 c | 0 c | 10 c |
| sa3-medium | c2 | 149 | +0 c | 0 c | 20 c |
| sa3-medium | c3 | 154 | +0 c | 0 c | 20 c |
| sa3-medium | c4 | 149 | +0 c | 0 c | 20 c |
| sa3-medium-sfx | c2 | 133 | +0 c | 0 c | 20 c |
| sa3-medium-sfx | c3 | 146 | +0 c | 0 c | 20 c |
| sa3-medium-sfx | c4 | 144 | +0 c | 0 c | 20 c |
| audioldm2 | c2 | 2 | -15 c | 25 c | 39 c |
| audioldm2 | c3 | 2 | -15 c | 25 c | 39 c |
| audioldm2 | c4 | 2 | -10 c | 20 c | 29 c |

Shipped YIN, which reports continuous values, on 1906 of those clips: median 1.6 c from exact C, 95th percentile 15.2 c.

**Naming.** The UI names MIDI 60 C4 (`getMidiNoteName(n, true, true, 4)`), and MIDI 60 plays a buffer at unity. A buffer that really holds C3 therefore sounds an octave below the key's label. `, c4` lands where the label says about as often as `, c3` lands on C3: 54 to 73 % in C4. The bank's word is `c3`, so this is stated and not proposed as a change.

**Spelling.** A follow-up decided after the matrix, because a switch can write any word.

### Other spellings of the switch word

Same prompts, seeds 1000 to 1007. Consensus clips; octaves of the clips on C.

| Engine | Spelling | Clips | Consensus | On C | C1 | C2 | C3 | C4 | C5 | C6 |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sao-small | `, c3` | 80 | 73 | 95 % | 0 | 0 | 30 | 31 | 7 | 1 |
| sao-small | `, C3` | 80 | 73 | 95 % | 0 | 1 | 31 | 32 | 4 | 1 |
| sao-small | `, note c3` | 80 | 76 | 95 % | 0 | 1 | 22 | 39 | 7 | 3 |
| sao-small | `, c` | 80 | 71 | 89 % | 2 | 1 | 17 | 26 | 14 | 3 |
| sao-small | `, 130.8 Hz` | 80 | 51 | 31 % | 1 | 1 | 6 | 3 | 3 | 2 |
| sa3-small-music | `, c3` | 80 | 63 | 100 % | 0 | 0 | 46 | 17 | 0 | 0 |
| sa3-small-music | `, C3` | 80 | 68 | 100 % | 0 | 0 | 51 | 13 | 4 | 0 |
| sa3-small-music | `, note c3` | 80 | 66 | 100 % | 0 | 0 | 39 | 25 | 2 | 0 |
| sa3-small-music | `, c` | 80 | 78 | 100 % | 0 | 12 | 33 | 25 | 0 | 8 |
| sa3-small-music | `, 130.8 Hz` | 80 | 33 | 33 % | 0 | 11 | 0 | 0 | 0 | 0 |
| sa3-medium | `, c3` | 80 | 77 | 100 % | 0 | 8 | 46 | 18 | 5 | 0 |
| sa3-medium | `, C3` | 80 | 75 | 100 % | 0 | 7 | 41 | 24 | 3 | 0 |
| sa3-medium | `, note c3` | 80 | 75 | 100 % | 0 | 5 | 45 | 23 | 2 | 0 |
| sa3-medium | `, c` | 80 | 72 | 100 % | 0 | 17 | 29 | 19 | 7 | 0 |
| sa3-medium | `, 130.8 Hz` | 80 | 49 | 22 % | 0 | 5 | 4 | 2 | 0 | 0 |

`, C3` and `, note c3` behave like `, c3`. The letter alone lands on C nearly as often and spreads the octave further, which is what the digit contributes. A frequency in words does not work. No spelling tested holds the octave better.

Why the models read note names at all is a hypothesis, unverified: sample-pack file names in the training data.

## 5. Question 2: reliability of the estimators

Calibration on signals of known pitch, 11 pitches from C1 to C6, off-grid ones included, 2 repeats. Counts are correct / octave error / other error / no stable f0, of 22.

| Signal | pYIN | CREPE | Shipped YIN |
|---|---|---|---|
| sine, saw, square, missing fundamental, vibrato ±30 c | 22/0/0/0 | 22/0/0/0 | 22/0/0/0 |
| plucked string (Karplus-Strong) | 22/0/0/0 | 22/0/0/0 | 21/0/0/1 |
| saw + pink noise at 20 and 10 dB | 22/0/0/0 | 22/0/0/0 | 22/0/0/0 |
| saw + pink noise at 0 dB | 22/0/0/0 | 21/0/0/1 | 0/0/0/22 |
| bell, published minor-third partials | 2/20/0/0 | 0/19/0/3 | 0/16/0/6 |
| major triad | 0/19/0/3 | 8/3/2/9 | 0/16/0/6 |
| white noise | 1/3/8/10 | 0/0/0/22 | 0/0/0/22 |
| pink noise, noise burst | 0/0/0/22 | 0/0/0/22 | 0/0/0/22 |

On the bell, counted against the prime, CREPE reports the hum one octave below it, a partial that is present. pYIN (16 of 20) and the shipped YIN (all 16) go two octaves below, under the lowest partial. White noise makes pYIN alone call a pitch in 12 of 22 cases, which is why one estimator never decides here.

| Estimator | Saw tones, random pitch | Median abs error | 95th percentile | Largest |
|---|---:|---:|---:|---:|
| pyin | 40 of 40 | 2.0 c | 6.7 c | 7.9 c |
| crepe | 40 of 40 | 4.7 c | 13.2 c | 14.2 c |
| yin | 40 of 40 | 0.3 c | 1.6 c | 1.7 c |

Moved by exactly +150 cents, real generated clips show whether an estimator follows on this material, where there is no ground truth.

300 clips, each moved by exactly +150 cents.

| Estimator | Stable before | Lost after | Followed within 25 c | Median error | Octave hop | Other |
|---|---:|---:|---:|---:|---:|---:|
| pyin | 284 | 0 | 99 % | 0.0 c | 1 % | 0 % |
| crepe | 270 | 1 | 97 % | 7.5 c | 2 % | 0 % |
| yin | 238 | 1 | 100 % | 0.1 c | 0 % | 0 % |

### Agreement between estimators on generated audio

Clips both estimators of a pair call stable. YIN is the detector the wavetable engine ships.

| Engine | Pair | Clips | Both stable | Within 50 cents | Octave apart | Other |
|---|---|---:|---:|---:|---:|---:|
| sao-small | pyin–crepe | 960 | 742 | 94 % | 3 % | 2 % |
| sao-small | pyin–yin | 960 | 705 | 99 % | 0 % | 0 % |
| sao-small | crepe–yin | 960 | 693 | 96 % | 3 % | 1 % |
| sao-1.0 | pyin–crepe | 960 | 697 | 92 % | 5 % | 3 % |
| sao-1.0 | pyin–yin | 960 | 519 | 98 % | 2 % | 1 % |
| sao-1.0 | crepe–yin | 960 | 511 | 94 % | 6 % | 1 % |
| sa3-small-music | pyin–crepe | 960 | 751 | 91 % | 8 % | 1 % |
| sa3-small-music | pyin–yin | 960 | 706 | 99 % | 1 % | 0 % |
| sa3-small-music | crepe–yin | 960 | 708 | 94 % | 6 % | 0 % |
| sa3-medium | pyin–crepe | 960 | 759 | 95 % | 4 % | 1 % |
| sa3-medium | pyin–yin | 960 | 674 | 99 % | 1 % | 0 % |
| sa3-medium | crepe–yin | 960 | 665 | 97 % | 3 % | 0 % |
| sa3-medium-sfx | pyin–crepe | 960 | 742 | 89 % | 9 % | 1 % |
| sa3-medium-sfx | pyin–yin | 960 | 631 | 99 % | 1 % | 0 % |
| sa3-medium-sfx | crepe–yin | 960 | 629 | 92 % | 8 % | 1 % |
| audioldm2 | pyin–crepe | 560 | 90 | 32 % | 26 % | 42 % |
| audioldm2 | pyin–yin | 560 | 46 | 65 % | 30 % | 4 % |
| audioldm2 | crepe–yin | 560 | 21 | 29 % | 62 % | 10 % |
| all | pyin–crepe | 5360 | 3781 | 91 % | 6 % | 3 % |
| all | pyin–yin | 5360 | 3281 | 98 % | 1 % | 0 % |
| all | crepe–yin | 5360 | 3227 | 94 % | 5 % | 1 % |
### Could the detector the product already ships decide alone?

Shipped YIN against the two published estimators, all clips of the matrix. Reference: a clip counts as pitched when pYIN and CREPE agree on it. Two gates. Shipped rule: what `prepareFramesFromBuffer` does, a pitch from two confident windows on. Cut-offs: the same pitch, kept only with at least half of ALL windows confident and half of those within 50 cents of it. Note that pYIN grew out of YIN, so their agreement is no independent proof; CREPE is the independent one.

| Engine | Gate | Clips | Agreed pitched | Gives a pitch on those | Same note | Octave off | Other | Gives a pitch where they do not agree |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| sao-small | shipped rule | 960 | 700 | 100 % | 99 % | 0 % | 1 % | 63 % |
| sao-small | cut-offs | 960 | 700 | 94 % | 100 % | 0 % | 0 % | 18 % |
| sao-1.0 | shipped rule | 960 | 640 | 99 % | 93 % | 6 % | 2 % | 60 % |
| sao-1.0 | cut-offs | 960 | 640 | 75 % | 99 % | 1 % | 0 % | 14 % |
| sa3-small-music | shipped rule | 960 | 686 | 100 % | 99 % | 0 % | 0 % | 49 % |
| sa3-small-music | cut-offs | 960 | 686 | 96 % | 100 % | 0 % | 0 % | 20 % |
| sa3-medium | shipped rule | 960 | 720 | 98 % | 99 % | 1 % | 0 % | 54 % |
| sa3-medium | cut-offs | 960 | 720 | 89 % | 100 % | 0 % | 0 % | 13 % |
| sa3-medium-sfx | shipped rule | 960 | 664 | 98 % | 99 % | 0 % | 1 % | 57 % |
| sa3-medium-sfx | cut-offs | 960 | 664 | 86 % | 100 % | 0 % | 0 % | 20 % |
| audioldm2 | shipped rule | 560 | 29 | 76 % | 55 % | 14 % | 32 % | 68 % |
| audioldm2 | cut-offs | 560 | 29 | 21 % | 100 % | 0 % | 0 % | 8 % |
| all | shipped rule | 5360 | 3439 | 99 % | 98 % | 1 % | 1 % | 60 % |
| all | cut-offs | 5360 | 3439 | 88 % | 100 % | 0 % | 0 % | 14 % |

On the 480 bare control clips, prompts without a pitch: shipped rule gives a pitch on 161, the cut-offs on 1, the agreement of pYIN and CREPE on 3.

The shipped code reads channel 0, this study the mono mix. Shipped YIN on channel 0 against the mono mix, 400 stereo clips with a pitch from the mix: same note 396, other pitch 3, no pitch 1.

## 6. Question 3: the wander on the regenerate path

Six published presets, each regenerated ten times over IPC with its own request. Regimes: `fixed_seed_alpha_drift` is VAR none or last with the preset's alpha LFOs summed as the plugin sums them, and a stand-in triangle of depth 0.2 for presets without one. `fixed_seed_wide_drift` is the same with a triangle of depth 0.5, an offset of ±1. The offset goes where the plugin sends it: on alpha in `linear` (Dirty Squarewave, Vintage Saw Gran, evolvement03 SA3, each around its saved position, so no series spans both poles), on the late mix at ×0.25 in the kombi modes (Funky Zing, Kalimba Gran, Horseback), where alpha stands still. `random_seed` is VAR auto, `random_seed_alpha_drift` VAR auto with drift. `resynth_25` and `resynth_75` chain each render into the next at those amounts, SA3 only. LFO phases step by the golden section, so that no two regenerates ask for the same offset. Drift on an axis is not modelled.

### Pitch of each regenerated buffer

Range = highest minus lowest, in cents. Step = between consecutive buffers. A note held across a regenerate moves by exactly that step.

| Preset | Regime | Consensus of 10 | Range | SD | Note names | Octaves | Median step | Steps ≥ 50 c |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Dirty Squarewave | fixed_seed_alpha_drift | 10 | 40 | 13 | 1 | 1 | 10 | 0/9 |
| Dirty Squarewave | fixed_seed_wide_drift | 8 | 40 | 14 | 1 | 1 | 10 | 0/5 |
| Dirty Squarewave | random_seed | 10 | 1900 | 718 | 2 | 2 | 1200 | 6/9 |
| Dirty Squarewave | random_seed_alpha_drift | 10 | 3100 | 745 | 2 | 3 | 30 | 4/9 |
| Funky Zing | fixed_seed_alpha_drift | 10 | 10 | 3 | 1 | 1 | 0 | 0/9 |
| Funky Zing | fixed_seed_wide_drift | 10 | 1200 | 377 | 1 | 2 | 0 | 2/9 |
| Funky Zing | random_seed | 7 | 1200 | 577 | 2 | 2 | 700 | 2/3 |
| Funky Zing | random_seed_alpha_drift | 8 | 1200 | 597 | 2 | 2 | 700 | 3/5 |
| Vintage Saw Gran | fixed_seed_alpha_drift | 10 | 0 | 0 | 1 | 1 | 0 | 0/9 |
| Vintage Saw Gran | fixed_seed_wide_drift | 10 | 0 | 0 | 1 | 1 | 0 | 0/9 |
| Vintage Saw Gran | random_seed | 9 | 1210 | 602 | 1 | 2 | 1200 | 4/7 |
| Vintage Saw Gran | random_seed_alpha_drift | 10 | 1210 | 506 | 1 | 2 | 10 | 4/9 |
| Vintage Saw Gran | resynth_25 | 10 | 10 | 3 | 1 | 1 | 0 | 0/9 |
| Vintage Saw Gran | resynth_75 | 10 | 10 | 3 | 1 | 1 | 0 | 0/9 |
| evolvement03 SA3 | fixed_seed_alpha_drift | 10 | 10 | 4 | 1 | 1 | 0 | 0/9 |
| evolvement03 SA3 | fixed_seed_wide_drift | 6 | 1190 | 615 | 1 | 2 | 1190 | 2/3 |
| evolvement03 SA3 | random_seed | 7 | 1200 | 640 | 1 | 2 | 0 | 0/5 |
| evolvement03 SA3 | random_seed_alpha_drift | 7 | 1210 | 639 | 1 | 2 | 605 | 2/4 |
| evolvement03 SA3 | resynth_25 | 5 | 0 | 0 | 1 | 1 | 0 | 0/2 |
| evolvement03 SA3 | resynth_75 | 10 | 10 | 5 | 1 | 1 | 0 | 0/9 |
| Echoes of a Laughing Kalimba Gran | fixed_seed_alpha_drift | 0 | – | – | – | – | – | – |
| Echoes of a Laughing Kalimba Gran | fixed_seed_wide_drift | 0 | – | – | – | – | – | – |
| Echoes of a Laughing Kalimba Gran | random_seed | 4 | 1230 | 608 | 1 | 2 | 20 | 0/1 |
| Echoes of a Laughing Kalimba Gran | random_seed_alpha_drift | 5 | 1240 | 545 | 1 | 2 | 35 | 0/2 |
| Saw a Horseback 140bpm | fixed_seed_alpha_drift | 10 | 0 | 0 | 1 | 1 | 0 | 0/9 |
| Saw a Horseback 140bpm | fixed_seed_wide_drift | 8 | 1200 | 424 | 1 | 2 | 0 | 2/5 |
| Saw a Horseback 140bpm | random_seed | 6 | 1200 | 617 | 1 | 2 | 10 | 1/3 |
| Saw a Horseback 140bpm | random_seed_alpha_drift | 6 | 1200 | 617 | 1 | 2 | 10 | 1/3 |

### The series, note by note

| Preset | Regime | Buffers (– = no consensus f0) |
|---|---|---|
| Dirty Squarewave | fixed_seed_alpha_drift | C4 C4+10 C4 C4+10 C4 C4 C4+40 C4 C4 C4 |
| Dirty Squarewave | fixed_seed_wide_drift | C4 C4+40 C4 – C4 C4 – C4 C4+10 C4+10 |
| Dirty Squarewave | random_seed | C3 C3 C4 C3 G4 C3 C3 C4 C3 C3 |
| Dirty Squarewave | random_seed_alpha_drift | C3 C2 C3 C3 G4 C3 C3+30 C3 C3 C3 |
| Funky Zing | fixed_seed_alpha_drift | C2+10 C2+10 C2+10 C2+10 C2+10 C2+10 C2+10 C2 C2+10 C2+10 |
| Funky Zing | fixed_seed_wide_drift | C2+10 C2+10 C3 C2+10 C2+10 C2+10 C2+10 C2 C2+10 C2+10 |
| Funky Zing | random_seed | C2+10 – C2+10 C3+10 – C3+10 – C2+10 C2+10 G2+10 |
| Funky Zing | random_seed_alpha_drift | C2+10 – C2+10 C3+10 – C3+10 C3+10 C2+10 C2+10 G2+10 |
| Vintage Saw Gran | fixed_seed_alpha_drift | C3 C3 C3 C3 C3 C3 C3 C3 C3 C3 |
| Vintage Saw Gran | fixed_seed_wide_drift | C3 C3 C3 C3 C3 C3 C3 C3 C3 C3 |
| Vintage Saw Gran | random_seed | C3 C3+10 C2 C3+10 C3 C3+10 – C2+10 C3+10 C2 |
| Vintage Saw Gran | random_seed_alpha_drift | C3 C3+10 C2 C3 C3 C3+10 C3+10 C2+10 C3+10 C3 |
| Vintage Saw Gran | resynth_25 | C3 C3+10 C3+10 C3+10 C3+10 C3+10 C3+10 C3+10 C3+10 C3+10 |
| Vintage Saw Gran | resynth_75 | C3 C3+10 C3 C3 C3 C3 C3 C3 C3 C3 |
| evolvement03 SA3 | fixed_seed_alpha_drift | C2 C2+10 C2+10 C2+10 C2+10 C2+10 C2+10 C2+10 C2 C2+10 |
| evolvement03 SA3 | fixed_seed_wide_drift | C2+10 – – – C2+10 C3 – C3 C2+10 C2+10 |
| evolvement03 SA3 | random_seed | C3 C3 C3 C3 – – – C2+10 C2 C2 |
| evolvement03 SA3 | random_seed_alpha_drift | C3 C3 – C3 – C2+10 C3+10 C2+10 C2 – |
| evolvement03 SA3 | resynth_25 | C3 C3 C3 – C3 – – – C3 – |
| evolvement03 SA3 | resynth_75 | C3 C3 C3 C3+10 C3+10 C3+10 C3+10 C3 C3 C3 |
| Echoes of a Laughing Kalimba Gran | fixed_seed_alpha_drift | – – – – – – – – – – |
| Echoes of a Laughing Kalimba Gran | fixed_seed_wide_drift | – – – – – – – – – – |
| Echoes of a Laughing Kalimba Gran | random_seed | C4+10 – – – C3 – – C4+10 C4+30 – |
| Echoes of a Laughing Kalimba Gran | random_seed_alpha_drift | C4+20 – – – C3 – – C4+10 C4+40 C4 |
| Saw a Horseback 140bpm | fixed_seed_alpha_drift | C2+10 C2+10 C2+10 C2+10 C2+10 C2+10 C2+10 C2+10 C2+10 C2+10 |
| Saw a Horseback 140bpm | fixed_seed_wide_drift | C2+10 C2+10 C3+10 C2+10 C2+10 – C2+10 – C2+10 C2+10 |
| Saw a Horseback 140bpm | random_seed | C2+10 C2 – C2 C3 C3 – C2+10 – – |
| Saw a Horseback 140bpm | random_seed_alpha_drift | C2+10 C2 – C2 C3 C3 – C2+10 – – |

The two presets with a G: Funky Zing's B prompt reads `FUNKY wah, g3`, so that G is asked for. Dirty Squarewave's G4 comes from `steady square wave, c3` and sits exactly on the third partial of C3; both estimators agree on it, and whether the buffer sounds on G4 or on C3 with a weak fundamental is for the ear (buffer 5 on the page).

**Is the harness measuring the preset?**

| Preset | Stored s | Harness s | Peak correlation | Lag ms | Stored pitch | Harness pitch |
|---|---:|---:|---:|---:|---|---|
| Dirty Squarewave | 6.73 | 6.73 | -0.003 | 2.5 | G4 | C4 |
| Funky Zing | 2.16 | 2.16 | -0.339 | 1.7 | C2 | C2 |
| Vintage Saw Gran | 10.00 | 11.00 | -0.262 | 1281.3 | C3 | C3 |
| evolvement03 SA3 | 11.00 | 11.00 | -0.238 | 2403.3 | C3 | C3 |
| Echoes of a Laughing Kalimba Gran | 3.06 | 3.06 | +0.937 | -0.0 | C4 | C4 |
| Saw a Horseback 140bpm | 4.68 | 4.68 | +0.797 | 0.0 | C2 | C2 |

The same request repeats exactly. Two published presets reproduce their stored audio; the others do not, so their series describe the saved request and not the stored buffer. 28 local presets with stored audio and an installed model: 9 reproduce their stored buffer at correlation 0.9 or above, 7 between 0.5 and 0.9, 12 below. The same request twice, 5 presets: lowest correlation +1.0000. Why the rest do not is not established; a preset saved while drift or Resynth ran, or from an earlier backend, would look like this.

## 7. Question 4: where a correction would live

Measured, the offline prototype of (a), applied to all buffers with an agreed pitch:

### After the offline correction

Each buffer with an agreed pitch moved tape-style onto the C nearest its series' median. Residual = measured again, minus the target. A residual near a whole octave means the estimators changed their mind about the moved buffer.

| Regime | Buffers moved | Moved by more than 50 c | Largest move | Length factor, range | Agreed again | Residual within 25 c | Median residual |
|---|---:|---:|---:|---:|---:|---:|---:|
| fixed_seed_alpha_drift | 50 | 0 | 40 c | 1.00–1.02 | 50 of 50 | 100 % | 0 c |
| fixed_seed_wide_drift | 42 | 4 | 1210 c | 1.00–2.01 | 42 of 42 | 100 % | 0 c |
| random_seed | 43 | 15 | 1900 c | 0.50–3.00 | 41 of 43 | 100 % | 0 c |
| random_seed_alpha_drift | 46 | 14 | 1900 c | 0.50–3.00 | 43 of 46 | 100 % | 0 c |
| resynth_25 | 15 | 0 | 10 c | 1.00–1.01 | 15 of 15 | 100 % | 0 c |
| resynth_75 | 20 | 0 | 10 c | 1.00–1.01 | 20 of 20 | 100 % | 0 c |

The correction does what it says: moved buffers measure on target. The cost is in the length factor. The 3.00 is the G4 of Dirty Squarewave moved to C3.

| Candidate | Touches | Cost | What it buys, measured |
|---|---|---|---|
| **(a)** Offline resample at prepare time | `T5ynthProcessor::loadGeneratedAudio`, `prepareCachePosition`, and `SamplePlayer::preparePlaybackState` or a step before it; a detector reachable from there | Length ×0.5 to ×3 here, attack and formants move with the pitch. The stored preset audio becomes the moved one unless the raw one is kept too. Loop finder then runs on the moved buffer, which is fine. Morph needs nothing, each side is already corrected. | Nearest-octave policy: moves at most 40 c on these series apart from the buffers on G, so little to gain with `c3` in the prompt. Exact policy: removes the octave wander. |
| **(b)** Per-buffer transpose in `PlaybackSnapshot` | `SamplePlayer.h` `PlaybackSnapshot`, `renderPitchedBlock`, `readRawSamples` | Keeps length. `readRawSamples` mixes both morph sides into one stretcher, so a ratio per side needs a second stretcher per voice during a morph, or the crossfade runs at one ratio. MIDI 60 leaves the direct read for any corrected buffer: one window of latency (`project_stretcher_latency_measured`). | Removes the octave wander with length intact, at the stretcher's sound at ×2. Not measured: the stretcher is C++ and no tool of this study links it. |
| **(c)** C3 switch | `src/gui/PromptPanel.*`, §9 | One control, a small text operation. No preset field, no backend change, no sound path change. Changes the sound of the prompt: §8, §9. | Note name right on 97 to 100 %, a median 1.6 c from exact C. Octave not held. |
| **(d)** (c) + (a) | Both | Both. Target from `Music::parseAB`. Open when A and B name different notes. | As (a) with an agreed pitch more often, because the word raises the share of clips with one: 50 to 67 % bare, 81 to 96 % with `, c3`. |
| Per-buffer read rate, not in the order | A field beside `bufferOriginalSR` in `PlaybackSnapshot` and `PreparedPlaybackState`; freeze `Snapshot::sampleRate`; the `prepareBufferLoad` call sites | Sounds like (a), tape-style, length changes alike. The buffer stays bit-exact, the loop finder is untouched, and the outgoing side of a morph already walks at its own rate (`SamplePlayer.cpp`, the `tailSR / curSR` line), so a per-side value exists on the stretch path too. A separate field is needed because `preparePlaybackState` also derives crossfade and search radius from the sample rate. A preset must store the value or measure again on load. | Same as (a), with no second copy of the audio. |
| Re-roll, not in the order | The generation path in `PromptPanel`; the detector, reachable outside `WavetableOscillator` | No sound path change, buffer bit-exact, held-note crossfade untouched. Costs generation time and needs a limit: after N misses the held sample stays. Only meaningful with a new seed. A clip without an agreed pitch cannot be rejected, so it passes. | Generations until the prompt's usual octave comes up, five Stable Audio engines: median 1.0 to 1.3; worst prompt 1.8 to 5.3 if a clip without an agreed pitch counts as a miss, 1.1 to 1.8 if it passes. One generation takes 0.4 to 1.6 s here. In the six series with drift, 4 to 8 of 10 buffers have an agreed pitch on target, 6 to 9 of 10 if unknown ones pass. |

Whichever is built, the held-note invariant in `CLAUDE.md` holds: the new buffer arrives through the same crossfade over Regen XFade. A correction needing a ratio per side must not collapse the two sides to one.

The detector needs no new dependency. With the two cut-offs, shipped YIN gives the same note as the agreement of pYIN and CREPE on 100 % of clips where it answers, answers on 88 % of them, and calls 1 of 480 bare control clips pitched. Without the cut-offs it calls 161 of 480 pitched, so they belong to it.

## 8. Question 5: what should not be normalised

### What a voicing cut-off does: pitched prompts against prompts without a pitch

Share of clips called pitched. A bare control prompt has no pitch by name, so a bare control clip called pitched is a false alarm. A control prompt with `, c3` is a different matter: see the next table.

Clips: 3520 pitched with a suffix, 880 pitched bare, 480 control bare, 480 control with `, c3`.

| Voiced ≥ | Concentration ≥ | Pitched, suffix | Pitched, bare | Control, bare | Control, `, c3` | Shipped YIN alone: pitched, suffix | Shipped YIN alone: control, bare |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.3 | 0.3 | 83 % | 61 % | 1 % | 15 % | 87 % | 1 % |
| 0.3 | 0.5 | 82 % | 55 % | 1 % | 15 % | 85 % | 1 % |
| 0.3 | 0.7 | 78 % | 47 % | 1 % | 14 % | 78 % | 1 % |
| 0.3 | 0.9 | 69 % | 37 % | 0 % | 12 % | 66 % | 0 % |
| 0.5 | 0.3 | 83 % | 60 % | 1 % | 15 % | 81 % | 0 % |
| 0.5 | 0.5 | 82 % | 54 % | 1 % | 15 % | 80 % | 0 % |
| 0.5 | 0.7 | 78 % | 47 % | 1 % | 14 % | 74 % | 0 % |
| 0.5 | 0.9 | 69 % | 37 % | 0 % | 12 % | 65 % | 0 % |
| 0.7 | 0.3 | 80 % | 55 % | 0 % | 14 % | 52 % | 0 % |
| 0.7 | 0.5 | 80 % | 50 % | 0 % | 14 % | 51 % | 0 % |
| 0.7 | 0.7 | 76 % | 43 % | 0 % | 14 % | 48 % | 0 % |
| 0.7 | 0.9 | 68 % | 35 % | 0 % | 12 % | 43 % | 0 % |
| 0.9 | 0.3 | 66 % | 35 % | 0 % | 10 % | 11 % | 0 % |
| 0.9 | 0.5 | 66 % | 34 % | 0 % | 10 % | 11 % | 0 % |
| 0.9 | 0.7 | 65 % | 32 % | 0 % | 10 % | 10 % | 0 % |
| 0.9 | 0.9 | 62 % | 29 % | 0 % | 10 % | 10 % | 0 % |

### Prompts without a pitch: does `, c3` give them one?

Consensus clips of all engines at the provisional cut-offs; on C in brackets.

| Prompt | Clips per suffix | bare | `, c3` |
|---|---:|---:|---:|
| a door slamming | 48 | 0 (0) | 0 (0) |
| a radio discussion | 48 | 0 (0) | 0 (0) |
| a samba group | 48 | 3 (0) | 29 (28) |
| calm waves | 48 | 0 (0) | 25 (25) |
| crackling fire | 48 | 0 (0) | 0 (0) |
| glass breaking | 48 | 0 (0) | 1 (0) |
| helicopter rotor | 48 | 0 (0) | 15 (13) |
| laughing hysterically | 48 | 0 (0) | 0 (0) |
| paper tearing | 48 | 0 (0) | 0 (0) |
| rain on a tin roof | 48 | 0 (0) | 0 (0) |

The word does more than ask for a pitch. `calm waves, c3` and `a samba group, c3` get an agreed pitch on more than half of their clips, nearly all on C; the same seeds bare give 0 and 3 of 48. That is the model changing the sound; no estimator's false alarm explains it, since the bare clips stay clean. Three prompts of this kind are on the listening page. The dice writes `, c3` on such sources on purpose: its `event`, `machine` and `nature` pools carry the suffix, and six of the ten controls come from them. Of those six, `helicopter rotor` gained an agreed pitch on 15 of 48 clips, `glass breaking` on 1, the other four on none. Voices, `sourceless`, `groove` and the field-recording pools go without it.

### Stored buffers in the preset library

| Pitch word in the prompts | Buffers | Consensus f0 | On C | On C of consensus |
|---|---:|---:|---:|---:|
| c3 | 61 | 33 | 30 | 91 % |
| none | 31 | 1 | 0 | 0 % |

### Pitch classes of stored buffers with c3 in a prompt, consensus only

| C | C# | D | D# | E | F | F# | G | G# | A | A# | B |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 30 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 2 |

### Inference caches: successive generations a player kept, published presets

| Preset | Entries | Consensus | Range | Note names | Octaves |
|---|---:|---:|---:|---:|---:|
| Five Percussive Voices 04 | 16 | 9 | 10 | 1 | 1 |
| Five Percussive Voices 05 | 16 | 0 | – | – | – |
| Samba Getdown Gran | 16 | 0 | – | – | – |
| Samba Getdown | 16 | 0 | – | – | – |
| Saw a Horseback 140bpm 2 | 16 | 13 | 20 | 1 | 1 |
| Saw a Horseback 140bpm | 8 | 8 | 0 | 1 | 1 |
| Silver House 140bpm fine | 4 | 0 | – | – | – |
| Silver House 140bpm layer | 4 | 0 | – | – | – |
| Talking about aliens 2 | 16 | 0 | – | – | – |
| Talking about aliens | 16 | 0 | – | – | – |
### The cut-offs on the bank's own stored buffers

63 primary buffers whose prompts carry a pitch word, 31 whose prompts carry none. Share called pitched by consensus. A buffer with a word may still be a chord or a texture; a buffer without one may still hold a tone. The table shows what each cut-off would touch.

| Voiced ≥ | Concentration ≥ | With a pitch word | Without one | Shipped YIN alone: with | Shipped YIN alone: without |
|---:|---:|---:|---:|---:|---:|
| 0.3 | 0.3 | 37 of 63 | 1 of 31 | 42 of 63 | 2 of 31 |
| 0.3 | 0.5 | 36 of 63 | 1 of 31 | 41 of 63 | 2 of 31 |
| 0.3 | 0.7 | 34 of 63 | 1 of 31 | 36 of 63 | 1 of 31 |
| 0.3 | 0.9 | 30 of 63 | 0 of 31 | 31 of 63 | 0 of 31 |
| 0.5 | 0.3 | 36 of 63 | 1 of 31 | 37 of 63 | 1 of 31 |
| 0.5 | 0.5 | 35 of 63 | 1 of 31 | 37 of 63 | 1 of 31 |
| 0.5 | 0.7 | 34 of 63 | 1 of 31 | 33 of 63 | 1 of 31 |
| 0.5 | 0.9 | 30 of 63 | 0 of 31 | 28 of 63 | 0 of 31 |
| 0.7 | 0.3 | 36 of 63 | 1 of 31 | 30 of 63 | 1 of 31 |
| 0.7 | 0.5 | 35 of 63 | 1 of 31 | 30 of 63 | 1 of 31 |
| 0.7 | 0.7 | 34 of 63 | 1 of 31 | 27 of 63 | 1 of 31 |
| 0.7 | 0.9 | 30 of 63 | 0 of 31 | 25 of 63 | 0 of 31 |
| 0.9 | 0.3 | 30 of 63 | 0 of 31 | 20 of 63 | 0 of 31 |
| 0.9 | 0.5 | 29 of 63 | 0 of 31 | 20 of 63 | 0 of 31 |
| 0.9 | 0.7 | 29 of 63 | 0 of 31 | 19 of 63 | 0 of 31 |
| 0.9 | 0.9 | 26 of 63 | 0 of 31 | 18 of 63 | 0 of 31 |

Between 0.3 and 0.7 on either axis consensus on prompts with a suffix moves between 76 and 83 %, on bare control clips between 0 and 1 %, and on the bank by a few buffers, so telling tone from texture does not hang on the cut-off. Bare pitched prompts react more, 61 down to 43 %. At 0.9 real tones start to go. The shipped YIN is steeper on the voiced axis, because its voiced share counts all windows: 80 % at 0.5, 51 % at 0.7, 11 % at 0.9.

A buffer without an agreed pitch stays as it is in every candidate above. That also covers what the T5 Oscillator is for: extreme positions produce material with no pitch to correct, and nothing here touches it.

## 9. The C3 switch, designed, not placed

**Text, not a hidden flag.** The switch edits the prompt fields; it writes `, c3` behind each non-empty prompt that has no trailing note, and takes off only a trailing `c3`. The prompt the player sees is what the model gets, a preset saves it as it saves any prompt, and neither the preset format nor the IPC request changes. A request-time append would need a preset field and would make the text and the sound disagree.

- **State.** Derived from the text with `Music::parse`: on when every non-empty prompt that the switch may touch ends in `c3`. No state of its own to persist.
- **A prompt that names another note** (`, g3`) is the player's own word: the switch neither adds to it nor takes it off. If every prompt is of that kind, the switch has nothing to act on and shows off.
- **Tempo.** `trailingMusicSuffix` treats `, c3, 140bpm` as one run; the note is inserted into that run. The bank writes it in that order. Word order was not measured.
- **Re-prompt and translation.** They already keep the trailing run through `stripMusicSuffix` and `reattachMusicSuffix`, so the word survives.
- **The word.** `, c3` on every engine: no spelling held the octave better (§4).
- **Default.** Off. On a prompt without a pitch it changes the sound (§8), and a prompt without the word is what the dice's unsuffixed pools ask for.
- **Effect on the next generation.** Editing the text counts as a prompt change: in Manual nothing happens until the next Generate, in any other Regen mode `pollDriftRegen` renders on its next tick. To be decided at build time whether that is wanted.

**Drive and direction,** as `CLAUDE.md` asks of a change on the prompt path, `tools/pitch_c3_drive.py` on the text the encoder really gets, SA3 with its `TrackType: Instrument, ` prefix.

| Engine | Drive, ratio with `, c3` to bare, median | Per token | Cosine to bare, median |
|---|---:|---:|---:|
| SAO | 1.369 | 0.961 | +0.645 |
| SA3 | 1.259 | 1.055 | +0.515 |

Drive rises, direction moves. It is not a drive loss. It is also not neutral: the word moves the conditioning, so the sound changes with it.

## 10. Recommendation (proposal)

1. **Build (c), the C3 switch, off by default, as text.** It delivers the note name at the lowest cost measured, and it touches neither the sound path nor the preset format. It does not hold the octave and it changes unpitched prompts; both are stated above.
2. **Do not build (a), (b) or (d) for the note name.** With `c3` in the prompt they have a median of 1.6 cents to correct, 15 at the 95th percentile. Without it they would move buffers by up to six semitones tape-style for a pitch the prompt never asked for.
3. **The octave needs the ear first.** Two routes are worth the comparison, both on the listening page. Moving a buffer changes its speed and its register; rolling again keeps the buffer as the model made it and costs time. My preference is the re-roll, because the sound path and the held-note crossfade stay as they are and each buffer stays what the model made. If the ear accepts moved buffers, the per-buffer read rate is the better build than (a)'s resample.
4. **Already available.** With VAR none or last and drift at the preset's own depth, pitch held within 40 cents on all five presets with an agreed pitch. A much wider sweep can still jump the octave, also through the late mix with alpha standing still. Resynth on SA3 held it within 10 cents.

## 11. Open and not verified

- Nothing was heard by anyone yet. Whether normalised regenerates are better or merely flatter is BJ's call. The held-note files are offline simulations with the preset's Regen XFade, not recordings from the synth.
- The re-roll version on the page is the series without the buffers that missed. A real re-roll would draw new buffers.
- (b) was not prototyped, so its sound at ×2 is unknown.
- Series were generated with a request harness; four of six published presets store audio it does not reproduce (§6). Axis drift is not modelled, and LFO phases are sampled, not clocked.
- Two-prompt behaviour is covered by the six presets and the bank only; the matrix uses single prompts.
- Consensus excludes clips the two estimators disagree on, so "no consensus" mixes unpitched clips with octave disagreements.
- AudioLDM2 ran at 8 seeds per cell, at its native 16 kHz. With so few agreed clips its effect is unknown, not zero.

## Appendix: tools

All under `tools/`; raw audio and results in `~/.cache/t5ynth-pitch-study/`, outside the repository, and the tools refuse a study directory inside it. Estimators run in `~/.cache/t5ynth-pitch-venv`, generation in `.venv`.

| Tool | Job |
|---|---|
| `pitch_c3_ipc.py` | IPC client, request fields |
| `pitch_c3_generate.py` | The matrix and the regenerate series |
| `pitch_c3_phrasing.py` | Other spellings of the word |
| `pitch_c3_f0.py`, `pitch_c3_analyse.py` | Estimators, per-clip results |
| `pitch_c3_calibrate.py`, `pitch_c3_equivariance.py` | Known-pitch signals, shift following, channel check |
| `pitch_c3_bank.py` | Stored buffers of all presets |
| `pitch_c3_drive.py`, `pitch_c3_wirecheck.py` | Drive and direction, harness against stored audio |
| `pitch_c3_tables.py` | Every table above |
| `pitch_c3_listen.py` | Offline correction, listening page |
