# Sampler engine: why the loop/ping-pong seams click — analysis and the professional method

Status: analysis 2026-11-06; implemented 2026-11-07 (BJ: „Ok, dann programmiere das neu, freilich
nur mit legal verwendbaren Algos") — `src/dsp/LoopFinder.{h,cpp}` (the cascade of §4, own code
from the published methods, no code taken from Polyphone or LoopAuditioneer, both GPL),
`SamplePlayer::preparePlaybackState` / `readLoopedSample` (tail crossfade for Loop, runtime
turnaround crossfade for PingPong, gain law from the measured correlation), auto placement in
`PluginProcessor::analyzeGeneratedRegions`. Meters: `tools/audition_loop_seams.cpp`,
`tools/measure_loop_seams.sh`, `tools/seam_metrics.py`. Measurements in `docs/devlog.md`
(2026-11-07). Work order (BJ, verbatim, German) is in
`memory/ACTIVE_WORKORDERS.md`: the Sample engine's endpoint detection is bad, almost every
preset has an audible click, it can hardly be adjusted; research how it is done professionally,
compare with the code, identify why the algorithm is weak.

## 1. What the presets actually use

All 64 installed presets, field `engine.loopMode` (presets whose engine is not `sampler`
ignore these fields):

| engine   | oneshot | loop | pingpong |
|----------|--------:|-----:|---------:|
| sampler  | 2       | 3    | **18**   |
| freeze   | –       | 1    | 1        |
| csound   | 16      | 4    | 14       |

`loopOptimize`: 41 × high, 18 × off, 1 × low. `crossfadeMs`: mostly 150 or 130, a few 10–60.

So the complaint is, in ~80 % of the sampler presets, a complaint about **PingPong** — and
PingPong is the one mode for which the code performs no crossfade at all (§2.3).

## 2. What the code does today

Files: `src/dsp/SamplePlayer.cpp` (`preparePlaybackState` 565–676, `optimizeLoopEnd` 1401,
`refineLoopStart` 1443, `snapToLocalExtremum` 1485, `applyLoopCrossfade` 1516,
`writeBoundaryGuards` 1563, `advancePosition` 822) and `src/PluginProcessor.cpp`
(`analyzeGeneratedRegions` ≈7300–7450).

### 2.1 Where the loop window is placed (`analyzeGeneratedRegions`)

1. Active region = windows (10 ms) whose RMS exceeds max(−35 dB rel. peak RMS, −50 dBFS),
   ≥ 3 consecutive windows, ± one window margin.
2. Loop length is fixed first: `activeLen / 3`, clamped to **1.5 … 6 s**.
3. Loop position: the window of that length with the **highest average power**, stepped in
   10 ms increments.

Nothing about the seam enters this choice: no similarity between the head and the tail, no
pitch period, no zero crossing. The loop is simply put where the sound is loudest.

### 2.2 Loop mode

With `loopOptimize > 0`, two stages, then the crossfade:

- Stage 1 `optimizeLoopEnd`: normalised cross-correlation of the head window
  `[ls, ls+win)` against `[cand−win, cand)`, `win = min(512 | 2048, loopLen/4)`, candidates
  only within `±min(2000 | 8000, loopLen)` samples of the energy-chosen end (±45 ms / ±181 ms
  at 44.1 kHz). Channel 0 only. This is a sound phase-alignment step, but it is only a local
  tweak of an end point that was chosen for loudness.
- Stage 2 `refineLoopStart`: moves `ls` by up to ±64 / ±256 samples to minimise
  `|x[le−1] − x[ls]| + 2·|slope difference|` — a **single-sample** criterion at the raw splice.
- `applyLoopCrossfade`: equal-power (sin/cos) fade of the tail `[le−F, le)` into the head
  `[ls, ls+F)`, `F = crossfadeMs`, and the loop end moves to `le−F`. After this the sample
  played right after the wrap is 100 % tail, i.e. the raw splice is continuous by
  construction and the seam is spread over F.
- `writeBoundaryGuards`: periodic/palindromic guard samples for the 4-point cubic read.
  Correct and necessary (commit 94e6d9ad), not at issue.

### 2.3 PingPong mode

- `loopOptimize > 0`: each boundary is snapped to the sample within ±512 / ±2048 that
  minimises `|x[i+1] − x[i−1]|` (`snapToLocalExtremum`), channel 0.
- `loopOptimize = off`: nothing — the reversal happens at the energy-chosen 10 ms grid point.
- **No crossfade in this mode.** `crossfadeMs` is read, shown, stored in the preset, and
  ignored. The turnaround is a hard reflection of `readPosition` (`advancePosition`).

## 3. Why that is weak — ranked by audible impact on the shipped presets

### 3.1 PingPong: a hard reflection with a criterion that does not measure symmetry

Reversing time at sample `c` keeps `x` continuous but **negates every odd derivative**
(`x'`, `x'''`, …). A click-free reversal therefore needs the signal to be locally
time-symmetric around `c` over a whole window, not a zero first-difference at one sample:

    E_mirror(c) = Σ_k w_k · (x[c+k] − x[c−k])²      (k = 1 … W, W ≈ a few ms)

What the code minimises is `|x[c+1] − x[c−1]|` — one term of that sum, at one sample, on one
channel. On AI material with high-frequency content the first difference is dominated by the
HF/noise component, so the "extremum" found is wherever two neighbours happen to be equal, not
a turning point of the carrying waveform; every lower partial still has a slope discontinuity
there. With `loopOptimize = off` (5 of the 18 ping-pong sampler presets) there is no criterion
at all.

And whatever point is chosen, the standard remedy for a turnaround — crossfading the forward
continuation into the reversed playback over a few tens of ms — is absent, so the user's
Crossfade control changes nothing in the mode most presets use. That is the literal content of
"lässt sich kaum einstellen".

(The header still says "ping-pong (palindrome buffer)" and the devlog of the original port
says "Palindrome buffer for ping-pong (endpoints not doubled)". The current code reflects the
read position instead. A palindrome buffer is the better design: it turns a ping-pong into a
forward loop of `[seg, reverse(seg)]` with two ordinary seams, so every forward-loop tool —
alignment, seam measure, crossfade — applies to both.)

### 3.2 Loop: stage 2 undoes stage 1

Stage 1 aligns head and tail by phase over 512 / 2048 samples. Stage 2 then shifts the head by
up to 256 samples to fix a single-sample step at a splice that the crossfade will make
continuous anyway (§2.2). Net effect on pitched material: the 150 ms crossfade blends two
copies that are **no longer phase-aligned** — a flange/comb dip once per loop pass instead of a
seamless join. The stage exists to "close a small step at the splice" (commit 94e6d9ad); with a
crossfade ≥ 2 samples that step does not exist.

### 3.3 Crossfade shape is always equal-power

`sin/cos` is power-complementary: correct for **uncorrelated** segments (noise textures). For
**phase-aligned** segments — exactly what stage 1 produces — the two copies add coherently and
the sum peaks at `sin(45°)+cos(45°) = 1.414`, a **+3 dB swell** in the middle of every
crossfade. The music-dsp thread (Bristow-Johnson, Niemitalo) states the rule: constant-voltage
(linear) for correlation r → 1, constant-power for r → 0, or a shape derived from the measured
r. Polyphone uses linear. LSP's sampler offers both. The code offers neither choice nor
measurement.

### 3.4 The search is in the wrong place and far too small

Professional finders choose the **loop length and position by seam quality over the whole
sustain**; here the length is fixed by an energy heuristic to 1.5–6 s and the seam search may
then move the end by ±45/181 ms and the start by ±1.5/6 ms. The number of (start, end) pairs
evaluated for seam quality is a few thousand, all around a point chosen for loudness.
Polyphone evaluates every pair of same-polarity zero crossings in the steady part;
LoopAuditioneer every pair of low-derivative candidates ≥ 1 s apart.

### 3.5 No seam quality is ever measured or shown

Nothing computes a residual error after preparation. "Low/High" cannot be judged, the user
cannot see whether a loop is good, and the engine cannot derive a crossfade length from the
mismatch. Every reference implementation returns a number and acts on it (§4).

### 3.6 Minor

All criteria run on channel 0; a second channel gets no guarantee. The Signalsmith stretch
path smears a splice over its window but cannot remove a reversal; the direct-read path
(within the near-unity dead zone) reproduces every seam exactly.

## 4. How it is done professionally — the cascade

Sources read in full: Polyphone `sources/core/sample/sampleutils.cpp` (`findSteadyPart`,
`loopStep1`, `loopStep2`), LoopAuditioneer `src/AutoLooping.cpp` + user guide, music-dsp
thread "Algorithms for finding seamless loops in audio" (2010), dsprelated thread 15880,
Sony patent US 5,430,241, LoopMaker (Shi & Mysore 2018), loopweave and BGM-loop-detector
READMEs. Massie's chapter (Kahrs & Brandenburg 1998) and Roads were not reachable online and
are not cited here.

**Stage A — sustain detection.** Polyphone: RMS envelope in dB (5 ms hop), optimal piecewise-
linear segmentation with a penalty scaled to the envelope's own fluctuation, merge segments
whose common line is ≤ 6 dB/s and within the fluctuation → the steady part. LoopAuditioneer:
scan from both ends, offset the start to let the note stabilise. The present `−35 dB RMS`
gate finds *audible*, not *steady*.

**Stage B — candidate seam points.** Polyphone: positive-going zero crossings only
(same-polarity splices). LoopAuditioneer: samples whose |first difference| < 3 % of the
maximum in the sustain. Both reduce the search to points where a splice can be clean at all.

**Stage C — seam error over a window, not a sample.** Polyphone, per candidate pair, sums at
the splice and at 8–22 offsets before and after it:

    d = 0.45·|Δx| + 0.33·|Δ(slope)| + 0.22·|Δ(curvature)|

normalised by the peak and the term count. LoopAuditioneer: sum of |differences| over a
5-sample window per channel, worst channel counts. The thread's AMDF variant (Element Green,
Swami) weights a window peaked at the splice. Bristow-Johnson warns that correlation alone is
fooled by amplitude ramps — an error (difference) measure is not.

**Stage D — rank with a length preference.** Polyphone: `rank = 200·error + max(3, range/len)`,
longest loops tried first with a vectorised lower-bound filter so the whole search is
exhaustive and fast. LoopAuditioneer: ranked list of up to 8 loops ≥ 0.3 s apart for the user
to audition — "only your ears will be able to tell".

**Stage E — pitch constraint where there is a pitch.** Sony US 5,430,241: looping noise is
produced when the loop period is not an integer multiple of the source period; the looped
waveform contains only harmonics of the loop rate. Thread consensus (Vicanek, Stenzel,
Bencina): find the fundamental by ACF (smallest lag above threshold, to avoid octave errors),
quantise the loop length to whole cycles, then choose among those by correlation of the
overlapping ends. Pure phase alignment can be impossible with ≥ 3 inharmonic partials
(napierm, DSnooPy) — which is when the crossfade carries the load.

**Stage F — crossfade length from the residual, shape from the correlation.** Polyphone:
`crossfade = (error·sr/(5+0.75·checkNumber))²`, capped at half the loop — a worse seam gets a
longer fade, and the comment states the acoustic reason: it attenuates the partials whose
phases drifted over one loop. Linear fade. Thread: shape from r (constant-voltage at r = 1,
constant-power at r = 0, closed form `a(t)² + 2r·a(t)·a(1−t) + a(1−t)² = 1`).

**Stage G — textures (non-periodic AI material): similarity at feature level first.**
LoopMaker: per-beat chroma + MFCC + RMS, distance `a·Dc + b·Dm + q·Dr` (1, 0.6, 0.2), all
chunk pairs, top-N by adaptive threshold, then boundary refinement. loopweave: multi-
resolution spectral features, bounded recurrence search, joint endpoint refinement with
hard-cut vs crossfade selection, "correlation-aware, equal-power, and linear" overlap
rendering, ranked alternatives with a confidence verdict. BGM-loop-detector: spectral
similarity + onset-envelope similarity + boundary-discontinuity penalty, top-k refine,
40 ms crossfade. This is the layer for Stable-Audio output: a 1.5–6 s texture loop needs
head and tail to be *the same kind of sound* before any sample-level alignment can help.

**Stage H — ping-pong.** Reversal where the mirror error (§3.1) is minimal, plus a crossfade
across the turnaround; or build the palindrome and run the forward-loop cascade on both seams
(Audio Art of Looping; mooloop issue #31; FL Studio manual warns that a turnaround at a high
or low point clicks).

## 5. The gap in one sentence

The shipped algorithm chooses the loop for loudness, then adjusts one end by a few tens of
milliseconds with one sample-level filter at one scale, applies a fixed-shape fade in one
mode and none in the mode most presets use, and never measures the result — where the
professional method is a cascade (steady part → candidate points → windowed seam error →
ranked exhaustive search with a length preference → pitch quantisation if pitched → fade
length from residual and shape from correlation → feature-level similarity for textures →
symmetric turnaround for ping-pong), with the residual shown and the ear deciding.

## 6. Sources

- Polyphone (GPL-3.0): https://github.com/davy7125/polyphone — `sources/core/sample/sampleutils.cpp`
- LoopAuditioneer (GPL): https://github.com/GrandOrgue/LoopAuditioneer — `src/AutoLooping.cpp`;
  guide https://loopauditioneer.sourceforge.io/userguide.html
- music-dsp, "Algorithms for finding seamless loops in audio" (2010):
  https://music-dsp.music.columbia.narkive.com/C8IESiuZ/algorithms-for-finding-seamless-loops-in-audio
- dsprelated thread 15880: https://www.dsprelated.com/thread/15880/calculating-optimal-loop-point-in-audio-sample
- Sony, US 5,430,241 (1995): https://patents.google.com/patent/US5430241A/en
- Shi & Mysore, "LoopMaker" (CHI 2018): https://ccrma.stanford.edu/~kittyshi/files/ShiMysore_Chi2018LoopMaker.pdf
- loopweave: https://github.com/scpedicini/loopweave · BGM-loop-detector: https://github.com/Lucky-Ep/BGM-loop-detector
- LSP sampler manual (crossfade types): https://lsp-plug.in/?page=manuals&section=sampler_stereo
- FL Studio Loop Tuner (a smoother, not a finder; pitch-snapped crossfade):
  https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/editortool_looptuner.htm
- Ping-pong practice: https://the-audio-art-of.github.io/looping/pingpong/ ·
  https://github.com/lostsync/mooloop/issues/31
- Not reachable, not cited: Massie, "Wavetable Sampling Synthesis" (Kahrs & Brandenburg 1998);
  Kontakt / HALion / Zero-X / SampleRobot method documentation (marketing only or 404).
