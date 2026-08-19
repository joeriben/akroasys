# T5ynth Preset Format (`.t5p`) — Specification

This document is a reverse-engineered specification of the T5ynth `.t5p`
preset container format, sufficient for a developer to implement a
third-party reader, converter or browser without reading the T5ynth
source.

All byte offsets, field widths and encoder choices cited here are drawn
directly from the implementation in `src/presets/PresetFormat.cpp` and
`src/PluginProcessor.cpp`. Citations point at specific lines in the
codebase at the time of writing. Where a field's purpose could not be
determined from reading the code, this is called out explicitly.

---

## 1. Overview

A `.t5p` file is a binary container that bundles five things into a
single self-contained preset:

1. **Parameter state** — everything needed to restore the synth DSP
   (envelopes, LFOs, drift, filter, FX, sequencer, arpeggiator,
   generative sequencer, wavetable/freeze/noise, engine mode, loop points).
2. **Generated audio** — the 44.1 kHz stereo buffer that feeds the
   sampler, wavetable oscillator, or freeze engine. From format v4
   onwards the audio is stored as 24-bit lossless FLAC; v3 stored it
   as raw float32. Saving the audio means loading the preset does
   **not** require re-running text-to-audio inference, which is
   expensive and non-deterministic across devices (CPU vs. MPS vs. CUDA).
3. **T5 text embeddings** — two 768-float vectors (prompt A and prompt
   B, averaged over sequence length). These populate the Dimension
   Explorer UI on load so the user can continue editing in embedding
   space without re-tokenising.
4. **Inference cache** (optional) — up to 16 previously-generated audio
   buffers per preset so the user can compare variations without
   re-running inference. Each entry is one FLAC blob.
5. **Session snapshots** (optional) — up to 4 fully-captured GUI states
   bound to keys 1/2/3/4. Each snapshot includes its own audio buffer
   (FLAC), APVTS state (XML), prompts, axes, embeddings, dimension
   offsets and sampler markers — enough to restore the synth to that
   exact moment.

In addition, the container stores three pieces of **GUI-only state**
that are not part of the JUCE `AudioProcessorValueTreeState` (APVTS)
and would otherwise be lost: the prompt texts, the semantic axes slot
state (3 slots), and the "last seed / last device / last model"
metadata.

### 1.1 Why a custom format

JUCE's built-in state storage (`getStateInformation` /
`setStateInformation` at `src/PluginProcessor.cpp:1516` and `:1523`)
serialises the APVTS as an XML blob. That is used for DAW session
persistence only. It is unsuitable as a user-facing preset because:

- Prompts, axes, seed, device and model are not APVTS parameters, so
  they would not be preserved.
- It cannot embed the generated audio buffer, so loading a preset in a
  DAW session would require regeneration and stall the audio thread.
- T5 embeddings (~6 kB of floats) do not belong in APVTS either.

The `.t5p` container wraps the JSON export produced by
`T5ynthProcessor::exportJsonPreset()`
(`src/PluginProcessor.cpp:1749`), patches in the GUI-only fields, and
appends the audio as length-prefixed FLAC blobs (v4) or, in older
files still on disk, as raw interleaved float32 PCM (v3).

### 1.2 Non-goals

- The format is **not** designed for interchange with other plugins or
  for long-term archival. It is tightly coupled to T5ynth's parameter
  set and model list.
- It does **not** carry a schema, XSD, or self-describing chunk
  directory. Readers must know the layout ahead of time.
- It does **not** store the per-token T5 embedding sequence, only the
  sequence-averaged vectors (see section 5).

---

## 2. Container Structure

The binary layout is fixed and sequential — there is no tagged-chunk
system, no TOC, and no checksums. See `src/presets/PresetFormat.cpp`
for the writer (in `saveToFile`) and reader (in `loadFromFile`).

```
offset    width  field
------    -----  -----------------------------------------------------
  0       4      Magic bytes: ASCII "T5YN" (0x54 0x35 0x59 0x4E)
  4       4      Version, uint32 little-endian (currently 4)
  8       4      JSON payload length in bytes, uint32 little-endian
 12       N      JSON payload (UTF-8, not NUL-terminated)
 12+N     ...    Sequence of length-prefixed FLAC blobs, in fixed order:
                   1. Primary audio (one blob, if numSamples > 0)
                   2. Inference-cache entries (one blob per entries[i])
                   3. Sequencer one-shots (one blob per oneShotSamples[i])
                   4. Snapshot audio (one blob per snapshots[i])
                 Each blob is [uint32 LE byteLen][N FLAC bytes].
```

A FLAC blob carries its own `STREAMINFO` (sampleRate, channels,
sampleCount); the JSON metadata mirrors those values so a library
browser can describe a preset without decoding the audio.

**v3 fallback.** In format v3 the audio payloads are raw float32
interleaved PCM, not FLAC. There is no length prefix — the byte count
of each payload is derived from the JSON metadata
(`numSamples * channels * sizeof(float)`). The loader dispatches on
the version field; writes always emit v4.

### 2.1 Magic and version

The magic bytes are defined in `src/presets/PresetFormat.h`:

```cpp
static constexpr char kMagic[4] = { 'T', '5', 'Y', 'N' };
static constexpr uint32_t kVersion = 4;
static constexpr uint32_t kMinLoadableVersion = 3;
```

The version is currently `4`, written via `out.writeInt(...)`. In JUCE,
`FileOutputStream::writeInt` writes a **little-endian int32**. Despite
the field being typed `uint32_t` in the header, the writer casts it to
`int` before writing. Readers should treat the field as little-endian
32-bit. The reader accepts any version in
`[kMinLoadableVersion, kVersion]` and rejects anything outside that
range — see section 9.

### 2.2 JSON length

Written and read as a little-endian uint32 at offset 8:

```cpp
uint32_t jsonLen = *reinterpret_cast<const uint32_t*>(bytes + 8);
```

The reader bails out if `12 + jsonLen > size`, so a truncated file
will fail cleanly.

### 2.3 Endianness

All multi-byte integers are **little-endian**. The writer uses JUCE's
`FileOutputStream::writeInt` (LE) and the reader dereferences a
`uint32_t*` directly into the file buffer. On a big-endian host the
reader would be wrong, but T5ynth only ships for little-endian targets
(macOS x86_64/arm64, Windows x64, Linux x64), so this is not a
practical concern.

### 2.4 Chunking

In v4 there is one explicit length prefix per audio payload (4 bytes,
little-endian uint32), but no overall TOC. Section boundaries
otherwise follow the JSON: the reader walks a single cursor through
the payload tail and consumes one length-prefixed FLAC blob for each
JSON entry it expects to find, in the fixed order

```
primary audio → inferenceCache.entries[] →
sequencer.oneShotSamples[] → snapshots[]
```

If an expected JSON entry has `numSamples == 0` it is treated as
corrupt: the loader clears the corresponding section and stops parsing
further entries in it. The writer never emits such entries (they are
filtered out alongside their payload write).

In v3 there are no length prefixes; payload byte counts are derived
from JSON metadata only, and snapshots are absent.

---

## 3. Parameter Payload (JSON)

The payload at offset 12 is the UTF-8 encoding of a single JSON object
produced by `juce::JSON::toString(parsed, /*pretty=*/true)`
(`src/presets/PresetFormat.cpp:68`). It is pretty-printed with
indentation; third-party readers must not assume a minified form.

The object is built in two stages. First, `exportJsonPreset()` emits
the APVTS-derived parameter tree. Second, `saveToFile` parses that
string back into a `DynamicObject` and patches in GUI-only fields
before re-serialising.

### 3.1 Top-level keys

Produced by `exportJsonPreset` at `src/PluginProcessor.cpp:1749-1962`,
then patched at `src/presets/PresetFormat.cpp:18-66`:

| Key             | Source        | Description                                    |
| --------------- | ------------- | ---------------------------------------------- |
| `version`       | export        | Integer, currently `1`. This is the **JSON schema** version, not the container version. |
| `name`          | save/export   | Preset display name when saved through `.t5p`; legacy raw exports may contain `"T5ynth Export"` and should fall back to the filename for UI display. |
| `timestamp`     | export        | ISO-8601 UTC timestamp of the save. |
| `synth`         | export+patch  | Core synth params + prompts + seed + device + model + randomSeed (see 3.2). |
| `engine`        | export        | Engine mode, loop mode, crossfade, normalise, loop optimise, loop/start fractions. In Csound (LRO) mode also `csound_orchestra`, `csound_prompt`, `csound_reading`, `csound_params_text` and `csound_controls` (see 3.2.1). |
| `modulation`    | export        | `envs` (3 envelopes) + `lfos` (2 LFOs). |
| `driftLfos`     | export        | Array of 3 drift LFO objects. |
| `driftEnabled`  | export        | Bool. |
| `driftCrossfade`| export        | Float. |
| `regenMode`     | export        | `"manual"` / `"auto"` / `"bar_1"` / `"bar_2"` / `"bar_4"` / `"bar_8"` / `"bar_16"` (iterate-every cadence in bars). Older presets' beat keys (`"max_1beat"` …) are unknown now → fall back to `"manual"`. |
| `cacheAsync`    | export        | Bool. Offline cache take: while a cache records, the generation-side Drift is held at each cache point until the render lands and then stepped on by one `regenMode` interval. Absent in presets written before it existed → `false`. |
| `wavetable`     | export        | `scan`, `octaveShift`, `noiseLevel`, `noiseType`, `frames`, `smooth`, `autoScan`. `scan` also acts as Granular position when `engine.mode` is `"freeze"` for preset compatibility. |
| `freeze`        | export        | Granular texture state: `texture` (`"hold"`, `"silk"`, `"air"`, `"cloud"`) and `stereo` (0.0–1.0 width). Key name remains `freeze` for compatibility. |
| `effects`       | export        | Delay, reverb, limiter. |
| `filter`        | export        | `enabled`, `type`, `slope`, `cutoff` (normalised 0..1), `resonance`, `mix`, `kbdTrack`. |
| `sequencer`     | export        | Step sequencer state including scale, steps array, division, glide, gate. |
| `arpeggiator`   | export        | `enabled`, `pattern`, `rate`, `octaveRange`. |
| `generativeSeq` | export        | Euclidean generator params + fix-flags + inter-strand coordination (`coordination` key string / `coordinationCap`, both since 2026-07-16; absent → Density Budget, cap 3). |
| `semanticAxes`  | patch (t5p)   | Array of exactly 3 objects, GUI-only (see 6). |
| `audio_meta`    | patch (t5p)   | `sampleRate`, `channels`, `numSamples` for the PCM tail (see 4). |
| `embeddingA`    | patch (t5p)   | Array of floats, typically 768 entries (see 5). Omitted if not yet generated. |
| `embeddingB`    | patch (t5p)   | As above. |
| `inferenceCache`| patch (t5p, optional) | Metadata for an optional inference-cache tail; one FLAC blob per entry appears after the primary audio (see 4.4). |
| `snapshots`     | patch (t5p, optional) | Array of per-slot snapshot state; each entry corresponds to one FLAC blob in the payload tail (see 7). |

### 3.1.1 What an ABSENT key means

A key a file does not carry restores the parameter's **default**, as
declared in `createParameterLayout()` — not `0`, and not whatever the
previously loaded preset left the parameter at. A third-party writer may
therefore omit any key it has nothing to say about, and the result is the
same as a T5ynth save of a patch sitting at that default.

This matters because the defaults are frequently not zero and frequently
not the first entry of a choice list: the limiter threshold is −3 dB, the
filter cutoff 20 kHz, the amplitude envelope's Amt 1.0, the sequencer 120
BPM over 16 steps, the loop end fraction 1.0, the oscillator octave shift
index 2 (= 0 octaves, where index 0 is −2), the wavetable frame count
index 3 (= 256, where index 0 is 32). Until 2026-08-05 the reader took a
missing key as a literal 0 for about half the parameter set, so files
written against an older reading of this document loaded quieter, darker
and slower than they described. Readers written to this section and files
written by T5ynth itself — which always emits every key — are unaffected.

The exceptions, where an absent key instead **leaves the parameter where
the previously loaded preset put it**:

- `engine.voiceCount`, `engine.tuning` — polyphony and the tuning table
  are engine configuration, not a property of the sound.
- `synth.axesAmount`, `synth.hfBoost`.
- `modulation.envs[].attackCurve` / `decayCurve` / `releaseCurve`, and
  the `attackVelSens` / `decayVelSens` / `releaseVelSens` trio when the
  legacy `velSens` field is absent too.
- `sequencer.steps[].gate` and the bind fields (`bindMode`, else `bind`,
  else the ancient `glide`), per step.
- `engine.wtExtractStart` / `wtExtractEnd` fall back to the loop markers
  P2/P3 as they ended up after §3.1.3's resolution, not to their own
  declared 0.0/1.0. Presence of `wtExtractStart` alone decides for the
  pair.

Two keys are written but never restored, so their value in a file has no
effect at all:

- `engine.lcoSetsParams` (no longer written either — see §3.2.1).
- `sequencer.enabled`: loading a preset deliberately does not start or
  stop the sequencer, so the transport keeps running or staying stopped
  across the load.

One key is not a default question at all: `filter.enabled: false` forces
the filter type to Off whatever `filter.type` says. Absent, it does not —
see §3.1.2.

A whole top-level block that is absent (`freeze`, `filter`, `effects`, …)
likewise leaves everything in it untouched. Three deliberate exceptions,
because a routing left over from the previous patch is audible in a way a
missing scalar is not:

- an LFO or drift **slot** the file does not carry is reset to its
  defaults, as is a malformed entry in the middle of an otherwise good
  array;
- the same for envelopes **2..5**. Envelope 1 is the amplitude envelope
  and a file missing it is broken, so it is left alone rather than reset;
- an absent `modulation.aftertouch` clears every pressure routing.

A `.t5seq` sequence pattern is loaded on top of the live patch, so none
of these resets apply to it.

### 3.1.2 `filter.enabled` and `filter.type`

Two keys, one parameter (`filter_type`, whose Off is index 0). They are
resolved together:

| `enabled` | `type`  | result                                      |
| --------- | ------- | ------------------------------------------- |
| `false`   | any     | Off — the bool wins, which is its whole job |
| `true`    | present | the named type                              |
| absent    | present | the named type — an absent bool does not switch a filter the file named off |
| `true`    | absent  | the layout default type (Lowpass)           |
| absent    | absent  | the layout default type (Lowpass)           |

T5ynth always writes both, and derives `enabled` from the type, so only
a foreign or hand-edited file reaches the last three rows.

### 3.1.3 `engine.loopStartFrac` / `loopEndFrac` (P2/P3)

The two loop markers are read as a **pair** and resolved against each
other, never against the window the sampler still holds from whatever was
loaded before. A file therefore always loads its own loop window,
independently of load order:

```
P2 = clamp(loopStartFrac, 0.0, 0.99)
P3 = clamp(loopEndFrac,   P2 + 0.01, 1.0)
```

The minimum width is 1% of the buffer (`SamplePlayer::setLoopRegion`). A
file naming an end at or below its start (`0.5`/`0.5`, or `0.5`/`0.2`)
gets `P2 + 0.01`, which is the same answer whenever the file is loaded —
it is a property of the file, not of the session.

Until 2026-08-05 the markers went in one at a time through setters that
each clamp against the other's *current* value, so a file whose loop
began after the previous patch's loop ended was dragged back to
(previous end − 1%). 10 of the 46 presets in the shipped bank loaded a
different window depending on which preset preceded them, most of them
with `pointsLocked: true`, so the auto-bracketing in `loadGeneratedAudio`
never corrected it. Nothing about that was visible afterwards: the file
was intact and the sampler had simply not used it.

`engine.startPosFrac` (P1) is independent of both and is only clamped to
0.0–1.0.

### 3.2 The `synth` object

Emitted at `src/PluginProcessor.cpp:1760-1774` with the following
fields:

| Field           | Type    | Source                                          |
| --------------- | ------- | ----------------------------------------------- |
| `promptA`       | string  | Initially `""`, patched to `getLastPromptA()`   |
| `promptB`       | string  | Initially `""`, patched to `getLastPromptB()`   |
| `alpha`         | float   | APVTS `gen_alpha`                               |
| `magnitude`     | float   | APVTS `gen_magnitude`                           |
| `noise`         | float   | APVTS `gen_noise`                               |
| `duration`      | float   | APVTS `gen_duration`                            |
| `startPosition` | float   | APVTS `gen_start`                               |
| `steps`         | int     | APVTS `inf_steps`                               |
| `cfg`           | float   | APVTS `gen_cfg`                                 |
| `seed`          | int     | Initially APVTS `gen_seed`, patched to `getLastSeed()` |
| `device`        | string  | Initially `lastDevice` member, patched to `getLastDevice()` |
| `model`         | string  | Initially `lastModel` member, patched to `getLastModel()` |
| `hfBoost`       | bool    | APVTS `gen_hf_boost > 0.5`                      |
| `randomSeed`    | bool    | Patched at `PresetFormat.cpp:28`, `gen_seed == -1` |

The `promptA`/`promptB` fields are intentionally blank in the raw
`exportJsonPreset()` output because prompts are GUI-only state and not
APVTS parameters. The `.t5p` save path overwrites them in
`PresetFormat::saveToFile`. A plain `.json` dump of
`exportJsonPreset()` (used by the sequencer's parameters-only export
in `src/gui/SequencerPanel.cpp`) does not run that patch, so such
files lose the prompt text — this is by design for the JSON export
path (see section 8.2).

### 3.2.1 `engine.csound_controls` — what the twelve LRO knobs mean

An authored LRO instrument carries the library parameters that survived into its
body (`docs/IPC_PROTOCOL.md` §3.3, `controls`), and their VALUES are twelve
ordinary APVTS parameters (`lro_p1a` … `lro_p3d`) that the preset already carries
like any other. What those parameters MEAN is not derivable from a number, so the
reading travels with them:

```json
"csound_controls": {
  "parts":  [{"n": 1, "name": "singing bowl"}],
  "params": [{"ch": "lroP1a", "part": 1, "slot": "a", "name": "Bowl",
              "value": 0.5, "gloss": "which measured bowl the mode series is"}],
  "refused": []
}
```

Written only in Csound mode and only when the body carried such a line.
On load the NAMES are restored and the values are NOT: the parameters have
already been restored from the preset, and re-applying the author's starting
positions would throw away every knob the player had moved before saving. A
preset written before this contract simply has no such key, and its twelve
parameters then sit unnamed and unshown — which is the truth about it.

### 3.3 What is not saved

The following APVTS parameters are defined but not written to the
preset:

- `master_vol` — master output volume. Treated as a runtime / session
  level parameter. It is stored in the DAW session via
  `getStateInformation` at `src/PluginProcessor.cpp:1516` but has no
  entry in `exportJsonPreset`.
- `seq_running` and `gen_seq_running` on the load path — to avoid
  acoustic surprises, the importer deliberately does **not** restart
  sequencers (see `src/PluginProcessor.cpp:2208-2209` for step seq and
  `:2319` for the generative sequencer, which is re-enabled).
  Note: the exporter does write `sequencer.enabled` on save, but the
  step sequencer import path ignores it. This is an asymmetric design
  choice, not a bug.

---

## 4. Audio Payload

In format v4 each audio payload (primary audio, cache entries,
one-shots, snapshots) is a length-prefixed FLAC blob:

```
[uint32 LE byteLen][N FLAC bytes]
```

FLAC carries `sampleRate`, `channels` and `sampleCount` itself in the
`STREAMINFO` block, so each blob is self-describing on decode; the
JSON metadata mirrors those values for fast library-UI inspection.
Compression is 24-bit lossless (the JUCE `FlacAudioFormat` writer is
called with `bitsPerSample = 24` and quality level 5).

### 4.1 v3 fallback

In format v3 the audio payloads are raw IEEE-754 float32, interleaved
(L, R, L, R, ...) with no length prefix. Each payload's byte count is
derived from JSON metadata: `numSamples * numChannels * sizeof(float)`.
The current loader dispatches on the version field and selects the
appropriate decoder per payload; v3 files therefore continue to load
on a v4-aware build.

### 4.2 Audio metadata block

The `audio_meta` JSON object describes the **primary** audio:

```json
"audio_meta": {
    "sampleRate": 44100.0,
    "channels": 2,
    "numSamples": 132300
}
```

`sampleRate` is a JSON number (double), `channels` and `numSamples`
are integers. For v4 these fields duplicate what is already in the
FLAC blob's `STREAMINFO`; they are kept for library-UI use and so a
reader can refuse to decode a preset whose channel/sample shape it
does not expect.

### 4.3 Absent audio

If the user saves a preset before any generation has happened,
`numSamples` is `0` and no audio blob is appended. The reader gates
audio extraction on `numSamples > 0 && numChannels > 0`, so missing or
truncated audio leaves `result.hasAudio == false` without erroring the
load.

A consumer must therefore always check `LoadResult::hasAudio` before
using `LoadResult::audio`.

### 4.4 Optional inference-cache tail

When the Inference Cache is active and the user chose to **include
Inference-Cache** in the Save drawer, the writer adds an
`inferenceCache` JSON object and appends one length-prefixed FLAC blob
per entry, in the same order as `entries`:

```json
"inferenceCache": {
    "capacity": 16,
    "entries": [
        { "sampleRate": 44100.0, "channels": 2, "numSamples": 132300 }
    ]
}
```

The cache stores raw inference audio only. It does not duplicate
prompt, seed, model, device, embedding or drift state per cache
entry. Each entry's blob format is identical to the primary audio
blob.

---

## 5. Embedding Payload

The T5 text embeddings are **not** appended as binary — they are
embedded in the JSON payload as arrays of floats.

### 5.1 Format

Written at `src/presets/PresetFormat.cpp:57-66`:

```cpp
const auto& embA = processor.getLastEmbeddingA();
const auto& embB = processor.getLastEmbeddingB();
if (!embA.empty())
{
    juce::Array<juce::var> arrA, arrB;
    for (float v : embA) arrA.add(static_cast<double>(v));
    for (float v : embB) arrB.add(static_cast<double>(v));
    root->setProperty("embeddingA", arrA);
    root->setProperty("embeddingB", arrB);
}
```

Values are widened to `double` during serialisation (JSON has no
float32) and narrowed back to `float` on load at
`:168-177`. Some precision is lost in the double->string->double round
trip, but JSON round-tripping of IEEE-754 through `juce::JSON` is
exact to within 17 significant digits, so for practical purposes the
values are preserved.

### 5.2 Vector length

Both vectors nominally contain 768 floats (the hidden dimension of
the FLAN-T5-Large encoder used by Stable Audio Open). The format does
**not** enforce this; the reader simply copies whatever length the
JSON array contains. Third-party tools should assume 768 but validate.

### 5.3 What they represent

Both vectors are **sequence-averaged** — the T5 encoder output for
the prompt tokens is mean-pooled over the time dimension before being
stored. The per-token sequence is not preserved because the generation
pipeline only consumes the averaged vector anyway. A third-party tool
cannot reconstruct per-token attention from a `.t5p`.

### 5.4 Use on load

On load, the reader populates `LoadResult::embeddingA` and
`embeddingB`. `MainPanel::loadPreset` (`src/gui/MainPanel.cpp:751-755`)
then calls `processor.setLastEmbeddings(...)` and
`dimensionExplorer.setEmbeddings(...)`, which lets the Dimension
Explorer UI render the 768-bar A-B diff plot without rerunning
inference.

### 5.5 Absent embeddings

If the preset was saved before the first generation, `embA.empty()`
is true and the `embeddingA` / `embeddingB` keys are omitted from the
JSON entirely. The reader handles this by simply not populating
`result.embeddingA`/`B`, leaving them empty.

---

## 6. Semantic Axes State

T5ynth exposes three "semantic axes" slots in the GUI. Each slot is
two things: a dropdown selection (integer ID identifying which
precomputed axis to use) and a continuous value (usually in some
bounded range — range not enforced by the format).

This state is **GUI-only** (stored on `T5ynthProcessor` as
`lastAxes`, see `src/PluginProcessor.h:76-78`, `:151`) and would be
lost without explicit preservation.

### 6.1 Serialisation

Written at `src/presets/PresetFormat.cpp:31-43`:

```json
"semanticAxes": [
    { "dropdownId": 4, "value": 0.25 },
    { "dropdownId": 7, "value": -0.15 },
    { "dropdownId": 1, "value": 0.0 }
]
```

Exactly 3 entries, in slot order. `dropdownId` is an `int`, `value` a
`double` in JSON (float32 in memory).

### 6.2 Deserialisation

Read at `src/presets/PresetFormat.cpp:152-165`. The reader iterates
up to 3 elements and sets `result.hasAxes = true` whenever the
`semanticAxes` key is present and parses as an array — even if the
array is empty or contains garbage dropdownIds. Third-party writers
should include all 3 slots.

### 6.3 Meaning of `dropdownId`

The dropdown ID is an internal index into the semantic axes catalogue
in the T5ynth UI. The mapping is not part of the preset format and may
change between T5ynth versions. Treat it as opaque: a third-party
viewer should display the raw integer and not attempt to resolve a
human-readable axis name unless it has an out-of-band mapping.

---

## 7. Per-slot Snapshots

T5ynth's GUI maintains up to four **session snapshots** bound to keys
1/2/3/4 on the user's keyboard. Each captures a full restore-state for
the synth — audio buffer, full APVTS, prompts, axes, embeddings, and
the sampler's loop / start / wavetable-extract markers.

Snapshots were session-only until format v4; the `snapshots` JSON key
plus the trailing FLAC blobs preserve them across saves.

### 7.1 Schema

Written as a JSON array under the top-level `snapshots` key. Only
slots whose audio is non-empty are emitted; empty slots are dropped
from both the JSON array and the payload tail so the JSON ↔ blob
mapping stays in lockstep.

```json
"snapshots": [
    {
        "slot": 0,
        "promptA": "...",
        "promptB": "...",
        "device": "MPS",
        "model": "stable_audio_open_small",
        "injectionMode": "linear",
        "seed": 123456,
        "randomSeed": false,
        "lateMixAmount": 0.75,
        "splitStart": 4.0,
        "splitEnd": 16.0,
        "axes": [
            { "dropdownId": 1, "value": 0.0 },
            { "dropdownId": 2, "value": 0.0 },
            { "dropdownId": 3, "value": 0.0 }
        ],
        "embeddingA": [ /* optional, 768 floats */ ],
        "embeddingB": [ /* optional, 768 floats */ ],
        "dimensionOffsets": [
            { "dim": 12, "offset": 0.41 }
        ],
        "parametersXml": "<...APVTS XML...>",
        "loopStart": 0.0,
        "loopEnd": 1.0,
        "startPos": 0.0,
        "wtExtractStart": 0.0,
        "wtExtractEnd": 1.0,
        "pointsLocked": false,
        "sampleRate": 44100.0,
        "channels": 2,
        "numSamples": 132300
    }
]
```

### 7.2 Slot numbering

`slot` is the 0-based slot index — the GUI displays it as 1..4. The
reader ignores any entry with `slot` outside `[0, 4)`. Slots are
addressable; the reader does not infer slot from array position.

### 7.3 APVTS state (`parametersXml`)

Each snapshot carries an XML serialisation of the relevant subset of
APVTS state. `MainPanel::buildSnapshotsForSave` produces the XML via
`juce::ValueTree::toXmlString()` on a `copyState()` taken at capture
time. On load, `MainPanel::applySnapshotsFromLoad` parses it with
`juce::parseXML(...)` + `juce::ValueTree::fromXml(...)` and restores
only the whitelisted parameter IDs in `kMainSnapshotParamIds` in
`src/gui/MainPanel.cpp` (envelopes, LFOs, drift, filter, engine mode,
modulation, wavetable, generation parameters — but **not** preset
metadata or sequencer running state).

Third-party tools that wish to inspect the snapshot params should
treat `parametersXml` as opaque JUCE XML and feed it back into a JUCE
runtime; the schema is the same as APVTS' own serialisation.

### 7.4 Audio blob

The audio for each snapshot is one length-prefixed FLAC blob in the
payload tail, **after** the sequencer one-shots, in the same order as
the JSON `snapshots` array. Decoding is identical to the primary audio
blob (see section 4).

### 7.5 Backwards compatibility

The `snapshots` key is optional. v3 files and v4 files written before
this feature simply omit it; the loader populates
`LoadResult::snapshots` as an empty vector and the GUI's four snapshot
slots stay clear.

---

## 8. Legacy Format Detection

The loader in `PresetFormat::loadFromFile`
(`src/presets/PresetFormat.cpp:107`) can handle three formats:

1. **Binary `T5YN` container** (current; versions 3 and 4 are both loadable, v4 is the writer default)
2. **Legacy plain JSON** (`.t5p` or `.json` containing a JSON object)
3. **Legacy XML** (`.t5p` containing a raw APVTS dump)

### 8.1 Detection logic

```cpp
bool isBinary = (size >= 12 && std::memcmp(data, kMagic, 4) == 0);
```
(`src/presets/PresetFormat.cpp:119`)

If the first four bytes are not `T5YN`, the loader reads the file as
text and dispatches on the first non-whitespace character:

```cpp
if (fileText.trimStart().startsWith("{"))          // JSON branch
else if (fileText.trimStart().startsWith("<"))     // XML branch
```
(`src/presets/PresetFormat.cpp:209`, `:233`)

There is no other fallback. A corrupt file that begins with neither
`T5YN`, `{`, nor `<` will produce a `LoadResult` with `success =
false` and all other fields default-initialised.

### 8.2 Legacy JSON

The JSON branch calls `processor.importJsonPreset(fileText)` directly
and then extracts prompts/seed/device/model from the
`synth` object for the UI.

Legacy `.json` files (raw output of `exportJsonPreset()`, no `T5YN`
magic) round-trip the APVTS parameters but not the GUI-only state:
prompts, axes, embeddings, audio and snapshots are all absent. The
`.t5p` loader treats them as a lossy subset of the current format and
silently degrades the UI fields the JSON cannot describe (e.g. no
audio buffer is restored). The sequencer also has its own
parameters-only `.json` export/import path that re-uses
`exportJsonPreset` / `importJsonPreset` under the hood — see
`src/gui/SequencerPanel.cpp` — but that flow is independent of the
`.t5p` container and is not handled by `PresetFormat::loadFromFile`.

### 8.3 Legacy XML

The XML branch at `src/presets/PresetFormat.cpp:233-245` parses the
file as an XML document and reconstructs a `ValueTree` via
`juce::ValueTree::fromXml`, then replaces the APVTS state wholesale.
This is the raw APVTS dump format that JUCE's `createXml()` produces.
Historically T5ynth used this as its preset format. It has no prompts,
no audio, no embeddings. Only the APVTS-resident parameters survive.

No version detection is performed on XML: any `ValueTree` that
`fromXml` accepts is loaded. A malformed or unrelated XML document
will produce `success = false` without further diagnostics.

---

## 9. Versioning and Migration

The header contains a version field. The current writer constants are
in `src/presets/PresetFormat.h`:

```cpp
static constexpr uint32_t kVersion = 4;          // emitted by writers
static constexpr uint32_t kMinLoadableVersion = 3; // accepted by readers
```

The reader accepts any version in the closed range
`[kMinLoadableVersion, kVersion]` and rejects anything outside it:

```cpp
if (version < kMinLoadableVersion || version > kVersion) { /* reject */ }
```

For each accepted version the audio-payload decoder dispatches on the
version field: v3 reads raw float32 interleaved PCM with byte counts
derived from JSON metadata; v4 reads length-prefixed FLAC blobs. JSON
schema differences are tolerated by treating unknown keys as
"missing" — the snapshot block, for example, is simply absent in v3
files and is parsed only when present.

**No format migration step is performed on load.** The writer always
emits the latest version; no v3 file is ever rewritten as v4
automatically. If a user opens an old v3 preset and re-saves, the
re-saved file will be v4 with FLAC payloads, but until that explicit
re-save happens the v3 file is read in place each time.

Consequences:

- A preset written with a version number outside the accepted range
  (including 0, 99, 0xFFFFFFFF or a hypothetical v5) is rejected up
  front — the loader returns `LoadResult{success = false}` and, in
  debug builds, prints the offending version via `DBG`.
- Any breaking change to the JSON schema or the payload layout that
  cannot be represented as a backwards-compatible extension should
  bump `kVersion`. If older builds must continue to load the new
  files, `kMinLoadableVersion` stays where it is and the loader gains
  another dispatch arm; otherwise `kMinLoadableVersion` is also
  bumped to cut the older readers off cleanly.
- Third-party writers should write `version = 4` to produce files that
  the current loader will accept and that other v4-aware tools will
  read with full fidelity (including FLAC audio and snapshots).
  Writing `version = 3` is permitted by the loader but loses FLAC
  compression and the snapshot block.

---

## 10. Entry Points

| API                                     | File                                |
| --------------------------------------- | ----------------------------------- |
| `PresetFormat::saveToFile`              | `src/presets/PresetFormat.cpp`      |
| `PresetFormat::loadFromFile`            | `src/presets/PresetFormat.cpp`      |
| `PresetFormat::LoadResult` (struct)     | `src/presets/PresetFormat.h`        |
| `PresetFormat::SnapshotState` (struct)  | `src/presets/PresetFormat.h`        |
| `PresetFormat::getPresetsDirectory()`   | `src/presets/PresetFormat.cpp`      |
| `T5ynthProcessor::exportJsonPreset()`   | `src/PluginProcessor.cpp`           |
| `T5ynthProcessor::importJsonPreset()`   | `src/PluginProcessor.cpp`           |
| `MainPanel::savePreset()`               | `src/gui/MainPanel.cpp`             |
| `MainPanel::loadPreset()`               | `src/gui/MainPanel.cpp`             |
| `MainPanel::importPresetFile()`         | `src/gui/MainPanel.cpp`             |
| `MainPanel::loadDefaultPreset()`        | `src/gui/MainPanel.cpp`             |
| `MainPanel::buildSnapshotsForSave()`    | `src/gui/MainPanel.cpp`             |
| `MainPanel::applySnapshotsFromLoad()`   | `src/gui/MainPanel.cpp`             |

### 10.1 `LoadResult` contract

```cpp
struct LoadResult {
    bool success = false;
    juce::String presetName;
    juce::String promptA, promptB;
    int seed = 123456789;
    bool randomSeed = false;
    juce::String device;
    juce::String model;

    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    bool hasAudio = false;

    int inferenceCacheCapacity = 0;
    std::vector<InferenceCacheAudio> inferenceCache;

    std::array<AxisState, 3> axes;
    bool hasAxes = false;

    std::vector<float> embeddingA, embeddingB;
    juce::StringArray tags;
    std::vector<SnapshotState> snapshots;

    juce::String injectionMode { "linear" };
    float lateMixAmount = 0.75f;
    float splitStart    = 4.0f;
    float splitEnd      = 16.0f;
};
```
(`src/presets/PresetFormat.h` — see the `LoadResult` declaration)

The consumer is expected to check `success`, then `hasAudio` and
`hasAxes`, test `embeddingA.empty()` before using the embeddings, and
inspect `snapshots` / `inferenceCache` only when those vectors are
non-empty.

### 10.2 `saveToFile` ordering

`saveToFile` in `src/presets/PresetFormat.cpp` performs these steps in
order:

1. Call `exportJsonPreset()` to get the base parameter JSON.
2. Parse it back to a `DynamicObject`.
3. Patch prompts, seed, device, model, randomSeed into `synth`.
4. Add the `semanticAxes` array.
5. Add the `audio_meta` object for the primary audio buffer.
6. Optionally add `embeddingA` / `embeddingB` arrays.
7. Optionally add the `inferenceCache` metadata object (capacity +
   per-entry sampleRate/channels/numSamples) — empty entries are
   skipped on both the JSON and payload side to keep the two in
   lockstep.
8. Optionally add the `snapshots` array — one entry per non-empty
   slot, with prompts, axes, embeddings, dimension offsets, APVTS
   `parametersXml`, sampler markers and audio metadata.
9. Re-serialise to a string, compute byte length.
10. Write header: magic, version (`kVersion = 4`), JSON length.
11. Write JSON bytes.
12. For each audio payload in fixed order — primary, then inference
    cache, then sequencer one-shots, then snapshots — encode the
    buffer with `juce::FlacAudioFormat` (24-bit, quality 5), write a
    little-endian uint32 byte length, then write the FLAC bytes.
13. Flush via `TemporaryFile::overwriteTargetFileWithTemporary`, return
    success.

### 10.3 Bundled presets

There are none anymore. Since `4e970b77` ("stop bundling factory presets
into the binary") no `.t5p` is baked into the binary — `juce_add_binary_data`
carries only the IRs, the icon, the manual, and SA3 metadata JSON. The
"UCDCAE AI Lab" bank is distributed exclusively through the public preset
repo and fetched on demand via the Preset Manager's **Update Library**
button (`PresetUpdater`: manifest diff + raw.githubusercontent.com
download) into the `UCDCAE AI Lab` subdirectory of the user preset
directory. A first launch with an empty library shows a "No Presets
Found" dialog pointing at that button (`MainPanel.cpp`, first-launch
hint). Downloaded files are fully user-editable — there is no read-only
tier. Startup itself does not load a demo preset; if no standalone
session buffer is restored, it uses the same clean Init state as the
status-bar `Init` action.

How the bank itself is published: `docs/PRESET_LIBRARY_MAINTENANCE.md`.

---

## 11. Worked Example: Generic `.t5p` Header

A `.t5p` container begins with a fixed 12-byte header:

```
offset  bytes                                    decoded
------  ---------------------------------------  ------------------
0x00    54 35 59 4E                              magic = "T5YN"
0x04    04 00 00 00                              version = 4 (LE) — writer default
0x08    NN NN NN NN                              JSON length (LE)
0x0C    7B 22 76 65 72 73 69 6F 6E 22 3A ...     JSON begins: {"version":...
```

The JSON length is little-endian and determines where the audio
payload tail begins. In v4 the tail is a sequence of length-prefixed
FLAC blobs in fixed order (primary audio → inference-cache entries →
sequencer one-shots → snapshots); each blob is
`[uint32 LE byteLen][N FLAC bytes]` and is self-describing once
decoded. In v3 (also accepted on read) the version byte reads
`03 00 00 00` and the tail is raw interleaved float32 PCM whose byte
counts are derived from JSON metadata only.

---

## 12. Known Limitations

### 12.1 Embedding averaging is lossy

`embeddingA` and `embeddingB` store the sequence-averaged T5 encoder
output. The per-token sequence is not preserved. A third-party tool
cannot, for example, recompute cross-attention weights or re-tokenise.
For T5ynth's own generation pipeline this is not a problem because
only the averaged vector is consumed downstream.

### 12.2 Model identifier is opaque and non-portable

The `synth.model` string is the model directory / identifier as seen
by the running T5ynth installation (e.g. a Stable Audio Open variant
name). The loader matches it against installed models by exact string
comparison at `src/gui/PromptPanel.cpp:485-492`:

```cpp
for (int i = 0; i < kNumModelSlots; ++i)
{
    if (modelSlotIds[i] == model)
    {
        modelBtns[i].setToggleState(true, juce::dontSendNotification);
        break;
    }
}
```

If the model is not installed, no button is toggled. There is **no**
error raised, no status message, and no fallback — the preset is
otherwise fully loaded (audio, params, embeddings) but the user's
model selector will show whatever was previously active. This is
graceful to the point of being silent; a third-party tool that wants
to warn on missing models must do its own check.

### 12.3 No checksum or integrity check

The format has no CRC, no SHA, and no magic trailer. In v4 a corrupted
FLAC blob will most likely fail to decode and the corresponding audio
slot will load empty (the per-payload length prefix lets the loader
skip past the damage and continue with later payloads). In v3 a
corrupted raw-float32 PCM tail simply produces distorted audio on
load, as long as the file size still matches
`audio_meta.numSamples * channels * 4`. A corrupted JSON in either
version is caught by `juce::JSON::parse` returning a non-object, and
the loader returns an empty `LoadResult`.

### 12.4 Dependency on JUCE's JSON pretty-printer

The writer uses `juce::JSON::toString(parsed, /*pretty=*/true)`. A
third-party reader must not assume a specific whitespace layout —
only that the bytes between offset 12 and 12+jsonLen are valid UTF-8
JSON describing a single root object.

### 12.5 `version` field is validated by a closed range

The reader accepts any version in `[kMinLoadableVersion, kVersion]`
(currently `[3, 4]`). Anything outside that range — 0, 99,
0xFFFFFFFF, or a hypothetical v5 — is rejected with
`LoadResult{success = false}`. This is intentional: payload decoders
only exist for the in-range versions, so accepting an unknown version
would silently mis-interpret the tail under the wrong schema.

Version 1 and version 2 fall outside the range and are rejected. v1
was never used with the `T5YN` magic — it referred to the pre-binary
JSON/XML fallback branches handled in the non-`isBinary` code path
(see section 8). v2 binary writers no longer exist in practice; the
one-off Python migration tool referenced in `PresetFormat.h` was used
to lift the bundled DEMO preset to v3.

### 12.6 Asymmetric handling of `sequencer.enabled`

The exporter writes `sequencer.enabled` into the JSON, but the
importer at `src/PluginProcessor.cpp:2207-2209` deliberately does not
restore it (the lines are commented with "Preserve current seq_running
state — don't stop playback on preset load"). The generative
sequencer's `enabled` field **is** restored at `:2319`. This is an
intentional UX choice for the step sequencer but may be surprising to
a third-party tool that expects symmetry.
