# LRO dialogue: the author edits its own orchestra on instruction

Status: plan, 2026-11-14. Design agreed with BJ in principle; nothing implemented.
Measurements in §3–4 were run today on the Fedora dev machine (RTX PRO 6000, llama-cpp-python
0.3.36 built with CUDA, the shipped `gemma-4-12b-it-qat-q4_0.gguf`). None were run on a Mac.

## 1. What it is

The LRO writes one Csound body per prompt. The dialogue lets the player keep that body and
have the author change it — a parameter, a movement, a part swapped or removed, or a structural
rebuild — instead of starting over from a new prompt.

**No new input, no new question in the UI.** BJ rejected a second text field and the suggestive
placeholder "Was soll sich ändern?". The design:

- **One input: the existing `dcoPromptEditor`**, placeholder unchanged ("Describe the oscillator
  to craft"). What the player types there is not prescribed: a rewritten description, a few
  added words, or an instruction all work. The field keeps the player's text.
- **One new position on the LRO Re-Prompt bar: "Edit"** (working title; name and glyph are BJ's).
  Appended at the END of the LRO bar so the stored indices of old presets keep their meaning.
  The LRO bar therefore gets its own entry count and glyph list instead of sharing
  `RepromptStance::kEntries`/`kCount` with the neural bar.
  - Off: fresh authoring, as today.
  - Machine stances (Transcribe … Opposite): unchanged.
  - Edit: GENERATE hands the author the running body and the field text, and the author edits
    the code.
- **Edit steps only on a manual press**, never on the REGENERATE cadence
  (`pollLcoRepromptCadence`). An instruction is a human act; the cadence belongs to the machine
  stances.
- **The trace card records the dialogue.** Per step it adds a station (e.g. ASKED — the text;
  CHANGED — lines kept, parts added/removed; RUNNING) and a version mark (v1, v2, …). A short
  click on a mark restores that version (body + knob values). Click-and-hold still flips the card
  (`kHoldMs` 400), so the short click is free.
- **Presets store the history as text** (the instructions and the version bodies), not as a new
  binary block in the orchestra path.

What it is NOT: not the self-check. The automatic compare-and-correct loop stays deprecated
(`T5YNTH_LCO_SELFCHECK=0`, `docs/plans/HANDOVER_lco_selfcorrect.md`). Here only the player
asks, and only the author answers. No instrument code-lock either: the author may rebuild
anything; the player sees on the card what was kept.

## 2. The backend turn

A new field `revise` on mode `csound` (the deprecated `correction`/`previous` pair, which only
quotes the old READING, is left untouched). The conversation:

1. system — the same system prompt as a fresh authoring (full library index + knob shelf).
2. user — the original prompt and the earlier instructions of this history.
3. assistant — the running body, with the CURRENT knob values written back into its library
   lines (`kbowl = 0.50 ; bowl[...]`). This needs a new inverse of `read_controls`; without it
   the author would edit the body it wrote, not the instrument the player is hearing.
4. user — the new field text.

Then `_consult` and `_writing_turn` as today. The pages opened are those the author names,
as in a fresh authoring. The body's own entries are not opened automatically. Their code is
already in turn 3, and the author can name them if it wants their pages. (The draft agreed in
chat opened them automatically; this is the change, and it keeps Python out of the choice.)
"Names nothing → whole library" stays the rule if the window allows it. At 65,536 it cannot
fit (§4), and at 131,072 it fits narrowly (35.2k + 86.1k + body + turns ≈ 126k). Step 1 of §5
decides which.

The author returns a full body, not a diff. It goes through the unchanged chain:
`sanitize → read_controls → wire_controls → wrap → syntax_check → perform_check →
frozen_controls → gate_knobs`, with the existing repair loop.

## 3. Unchanged text = a new seed (BJ, 2026-11-14)

> "Ist der Text unverändert, dann ist das das Äquivalent für einen neuen Seed, also läuft
> selbstverständlich ein neuer Coding-run."

**Today such a run would return the same code.** The author decodes greedily on every path:
`temperature=0.0` on llama.cpp (`backend/pipe_inference.py:1071`), `do_sample=False` on
transformers (`:1207`), `"temperature": 0.0` on both API authors (`backend/author_api.py:245`,
`:345`).

Measured: one prompt ("a breathy glass flute that slowly turns metallic") authored 3× in one
process, which is how the plugin's backend lives:

| run | consult | thinking | body |
|---|---|---|---|
| 1 (cold) | `37816c85` | `6afad54b` | `7ff63855` |
| 2 (warm) | `7659d8c1` | `8941d7c7` | `d3373a47` |
| 3 (warm) | `7659d8c1` | `8941d7c7` | `d3373a47` |

From the second press on, unchanged input gives byte-identical code. Run 1 differs because the
first call computes the 35k-token system prompt from scratch and later calls reuse its cache.
The different arithmetic tips a near-tie. That explanation is a hypothesis, not checked. Either way
it is an accident of the cache, not a seed. **This already applies to Off today**: a press with
unchanged prompt and settings re-authors the same body, and a Csound cache deeper than 1 fills
with copies. It contradicts `docs/LCO_CONCEPT.md:53` ("Thinking wird dann nicht-deterministisch
entscheiden …").

**Sampling.** The GGUF carries Google's sampling settings for this model:
`general.sampling.temp 1.0`, `top_k 64`, `top_p 0.95`. A re-roll uses those values, read from the
file (`Llama.metadata`), not numbers chosen here. Seed per authoring, stored with the version so
every version is reproducible. Measured on the flute prompt, 4 seeds, every call of the authoring
sampled:

| seed | result | attempts | consult opened |
|---|---|---|---|
| 11 | compiled | 1 | additive, blown_bottle, clarinet, flute, singing_bowl |
| 22 | compiled | 1 | additive, flute, metallic_fm, singing_bowl, struck_bar |
| 33 | compiled | 1 | additive, blown_bottle, driven_metal, flute, metallic_fm, singing_bowl |
| 44 | **no inference** | — | `Requested tokens (69856) exceed context window of 65536` |

The three bodies share 0.67–0.79 of their code lines (Jaccard, comments stripped). They are real
variants, not reshuffles. Three compiled runs are too few to say anything about the compile rate
under sampling; the probe in §6 measures it. Seed 44 failed on the window, not on sampling (§4).

**What a re-roll is based on.** It reruns the last step: same parent version, same text, new seed.
The result is an alternative to that step, not a step on top of it. Each version records
`(parent, text, seed)`. Version marks stay one row (v1, v2, v3 …). If v3 is a re-roll of v2, both
have parent v1, and the next edit's conversation follows the parent chain (v1 → v3), so a
discarded alternative never enters the dialogue. If the last step was a fresh authoring, the
re-roll is a fresh authoring with a new seed. The same rule fixes Off; that goes in its own
commit.

## 4. Precondition: the context window

Measured token counts (the model's own tokenizer):

| part | tokens |
|---|---|
| system prompt (head + full index + knob shelf) | 35,118 – 35,186 |
| writing turn, 6 instruments + 8 words opened (flute prompt) | 26,205 |
| writing turn, **whole library** (consult named nothing) | **86,082** |
| one authored body | 1,957 – 2,233 |

Fresh authoring of the flute prompt already sends 60,300–61,518 tokens to the writing turn, and
one of four sampled runs overflowed. `docs/plans/LRO_BOWED_STRING_ANALYSIS.md:71` records the
same failure on "a solo violin, bowed, singing". The "names nothing → whole library" fallback
cannot fit at all: 35k + 86k is beyond 65,536. An Edit turn adds the body (~2.1k), the history,
and a second consult. With a comparable page set it lands at ~67k, so **Edit cannot run reliably
in the current window.** This has to be solved first, as its own concern.

The model supports 262,144 tokens (`gemma4.context_length`); `_GGUF_N_CTX` 65,536 is our
setting. What a bigger window costs, as llama.cpp reports it at load:

| n_ctx | swa_full | flash_attn | global-layer KV | sliding-window KV | compute buffer |
|---|---|---|---|---|---|
| 65,536 (shipped) | on (default) | off (default) | 2,560 MiB | **20,480 MiB** | 2,367 MiB |
| 65,536 | on | on | 1,024 MiB | 20,480 MiB | 641 MiB |
| 131,072 | on | off | 5,120 MiB | 40,960 MiB | (no allocation reported) |
| 131,072 | on | on | 2,048 MiB | 40,960 MiB | 527 MiB |

**Side finding:** the shipped configuration reserves 22.5 GiB of KV cache on CUDA. 40 of the 48
layers use a 1,024-token sliding window, but `swa_full` (llama.cpp's C-API default, which the
binding inherits) caches them at full length. Without flash attention the V cache of the 8
global layers is also padded to 2,048. Whether the Metal build on a 16 GB Mac reserves the same
is unmeasured. `Llama(verbose=True)` on the Mac prints it.

`swa_full=False` shrinks the sliding-window cache to about the window size. **But with this
binding it corrupts silently** across authorings. Measured: authoring A (consult + writing turn,
61k tokens), then authoring B's consult. The binding finds the 35k-token system-prompt prefix,
llama.cpp's `seq_rm` returns success, and B's consult runs in 0.1 s. After it the cache holds
positions 35,128–35,185, but a correct window needs everything back to 34,162. The 40
sliding-window layers attend to 58 tokens instead of 1,024, and nothing reports an error. Inside
one authoring (consult → writing turn → repairs) the branch point is always the end of the
previous call, so reuse is correct there.

**The fix, measured (CUDA, 2026-11-14): `swa_full=False` + `flash_attn=True` + a
system-prompt checkpoint, at n_ctx 131,072.** After loading, evaluate the shared prefix once
(35,127 tokens: system prompt plus the opening of the user turn), save the state
(`Llama.save_state`), and restore it (`load_state`) at the start of every authoring instead of
relying on the binding's prefix match.

| | measured |
|---|---|
| build the checkpoint (once, at load) | 4.0 s |
| state held in RAM | 1,029 MiB llama state + 512 MiB logits buffer |
| restore per authoring | 0.6 – 0.8 s |
| sliding window after restore + next consult | pos_min 34,150 (correct: ≤ 34,162) |
| B's consult after a full authoring A, restored, vs B straight from the checkpoint | **byte-identical** |
| control: B after A without restore (binding's prefix match) | pos_min 35,128 (broken), output differs |
| A consult / A writing turn (60.3k tokens) | 2.8 s / 14.2 s (shipped config: 17.9 s / 69.9 s) |

So the restore is exact, the window is 2× the shipped one, and the KV cache drops from
22.5 GiB to roughly 2.5 GiB. Every authoring starts from the same state, so the first press
after a backend start no longer differs from the later ones (§3). Unmeasured: everything on the
Mac (memory, restore time, latency). Flash attention changes the arithmetic, so the greedy outputs
change for every prompt. That is a behaviour change of the shipped author and needs the corpus
run of `tools/lco_author_offline.py --measure` before and after.

**The API author** (`backend/author_api.py`) has windows well beyond this on Anthropic and
on most OpenAI-compatible models, so Edit works there without step 1. It sends no prompt-caching
markers today, so every Edit call re-sends the ~35k-token system prompt and its pages at full
price. Anthropic has no `seed` parameter. A re-roll there samples without a reproducible seed,
and the version keeps its body, which is what restore needs anyway.

## 5. Order of work

Each step is its own commit (or series), each with its verification.

1. **Window** (§4): loader to n_ctx 131,072, `swa_full=False`, `flash_attn=True`, checkpoint
   restored per authoring; corpus run before/after; Mac numbers from BJ.
2. **Seed**: sampling with the GGUF's own settings plus a per-authoring seed for a re-roll. This
   covers Off with unchanged prompt as well as Edit with unchanged text. The seed travels in the
   result, the trace and the preset.
3. **Backend `revise`** (§2), with the inverse of `read_controls`, then the probe (§6).
4. **UI**: the Edit position (LRO bar's own count and glyphs), routing in `triggerLcoGenerate`
   (Edit is a manual press, never cadence), card stations and version marks, restore on short
   click, preset persistence.

## 6. Probe (backend only, before any UI)

About 10 (prompt, instruction) pairs, each run greedy and with 3 seeds. The pairs cover:
moving a parameter, adding movement, swapping a part, removing a part, and a structural rebuild.
Prompts include the moving / morphing / dirty class, not only static timbres.

Measured objectively per run: compiled, attempts, code lines kept from the parent (Jaccard),
parts added/removed against the instruction, knob names kept. For the movement instructions, the
`lco_measure` signature: centroid travel before vs after. Whether the edit SOUNDS like what was
asked is BJ's ear, from an A/B folder (`parent.wav`, `edited.wav`, same pitch, length, loudness).

Scripts used for §3–4 (scratch, not committed): `same_input_twice.py`, `reroll_sampled.py`,
`consult_44.py`, `ctx_config_probe.py`, `ctx_reuse_probe.py`, `ctx_reuse_posmin.py`,
`ctx_checkpoint_probe.py`.
