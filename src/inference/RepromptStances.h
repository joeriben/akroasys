#pragma once
// juce_core ONLY (juce::String / juce::StringArray) — this module is deliberately
// free of juce_graphics/juce_audio_* and of PipeInference, so it stays
// unit-testable in isolation (tools/test_reprompt_stances.cpp) and keeps the backend
// `interpret` op generic. Including the full <JuceHeader.h> aggregate here would
// drag in the graphics/audio modules this logic does not use.
#include <juce_core/juce_core.h>

/**
 * Stance / coupling logic for the CLAP→LLM semantic self-listening loop.
 *
 * A faithful C++ port of the curated subset of tools/clap_llm_loop.py (the
 * verified, working algorithm). This module is the interpreter HALF of the loop
 * on the plugin side: it provides the per-stance system prompts, the per-stance
 * user-turn builders, the leak/overrun cleaner, and the concat coupling helper.
 *
 * Self-contained on purpose — it depends only on juce_core (juce::String /
 * juce::StringArray), NOT on PipeInference and NOT on any audio/DSP code. That
 * keeps the backend `interpret` op generic (the stance lives here, not in the
 * subprocess) and makes this logic unit-testable in isolation.
 *
 * Stance keys mirror BlockParams.h RepromptStance::kEntries[i].key, which in turn
 * mirror the keys of clap_llm_loop.py's MODES dict. The SIX shipped stances are:
 *   transcribe, entkitscher, verniedlicher, variation, abduction, opposite.
 * (planetarizer/develop/critique from the tool are intentionally NOT ported —
 * too complex for the local Qwen2.5-1.5B interpreter; see clap_llm_loop notes.)
 *
 * ONE stance is NOT a port and has no Python mirror: "selfcheck" (below). It
 * JUDGES a sound instead of generating the next prompt, so it is absent from both
 * clap_llm_loop.py MODES and BlockParams kEntries, and the "VERBATIM from
 * clap_llm_loop.py" contract above does not cover it — its wording was derived
 * from measurements taken in this repo, and its tests assert against itself.
 */
namespace RepromptStances
{
    /** The system prompt (the "stance") for a given stance key, VERBATIM from
     *  clap_llm_loop.py MODES[key]. Empty for an unknown key or "off".
     *
     *  Also serves the "selfcheck" key — the one stance that JUDGES instead of
     *  generating. It is deliberately NOT in BlockParams.h kEntries: the six loop
     *  stances are user-selectable and each produce the next prompt, whereas
     *  selfcheck produces a finding about the current one and is never a link in
     *  the chain. Putting it in kEntries would offer it as a generator and break
     *  the loop. See buildSelfCheckUserTurn. */
    juce::String stanceSystemPrompt (const juce::String& stanceKey);

    /** Build the per-turn user text for a stance, mirroring the matching
     *  build(tags, prev_b, recent, spectral) closure in clap_llm_loop.py.
     *
     *  @param stanceKey   one of the six shipped stance ids
     *  @param tags        the CLAP top-k timbre words for the latest render
     *                     (comma-joined), i.e. the tool's `tags`
     *  @param prevPolePrompt  that pole's previous prompt — the chain's own last
     *                     link (`*_glieder[-1]`), NOT the applied concat prompt
     *  @param recentList  that pole's anti-stasis memory (its last ≤3 links,
     *                     `*_glieder[-3:]`); only the abduction/opposite stances
     *                     read it ("already tried, do not reuse")
     *  @param spectral    DSP spectral words; only the transcribe stance reads it
     *  @return the user-turn string; empty for an unknown key or "off". */
    juce::String buildStanceUserTurn (const juce::String& stanceKey,
                                      const juce::String& tags,
                                      const juce::String& prevPolePrompt,
                                      const juce::StringArray& recentList,
                                      const juce::String& spectral);

    /** The LCO twin of buildStanceUserTurn — same per-stance structure and tone,
     *  for the LCO/Advanced panel's Re-Prompt step (docs/DCO_REPROMPT_CONCEPT.md).
     *
     *  SAME KIND OF INPUT AS THE NEURAL TWIN since BJ's decision of 2026-07-28
     *  (that document's Nachtrag of the same date): CLAP tags + spectral words of
     *  a bare probe render of the authored orchestra. It used to be handed the
     *  retired router's own reading of its recipe instead — the loop READ where
     *  the neural one HEARD — which is why the labels this builder emits used to
     *  say "Machine reading". That rested on a closed, pre-heard lexicon; authored
     *  Csound is open, so there is a sound to hear and nothing to look up.
     *
     *  The two halves stay separate and separately labelled, exactly as
     *  buildStanceUserTurn does it: `heardTags` is an association ranked out of a
     *  fixed vocabulary, `heardSpectral` is computed from the signal, and the
     *  model must be able to tell them apart.
     *
     *  Guardrail note (the code is the author's, not this function's): whatever the
     *  model writes back is re-authored from scratch by the Csound author exactly
     *  like hand-typed text — this function only composes the prompt it sees.
     *
     *  The stance SYSTEM prompts are UNCHANGED and shared verbatim with the neural
     *  loop (stanceSystemPrompt above); their "neural ear" / "spectral descriptors"
     *  wording, a known v1 mismatch while this loop read instead of heard, now
     *  describes what actually arrives.
     *
     *  @param stanceKey     one of the six shipped stance ids
     *  @param heardTags     CLAP top-k timbre tags of the probe render, comma-joined
     *  @param heardSpectral the computed spectral words (e.g. "warm, full-bodied,
     *                       tonal"); may be empty, and is then left out with its label
     *  @param flagsLine     the flags[] honesty channel as one line; it has had no
     *                       producer since the Csound switch and is empty in the
     *                       shipped product (see the concept doc's status head)
     *  @param prevPrompt    the LCO loop's own last link (mirrors prevPolePrompt)
     *  @param recentList    the LCO loop's anti-stasis memory (its last <=3 links);
     *                       only abduction/opposite read it
     *  @return the user-turn string; empty for an unknown key or "off". */
    juce::String buildDcoStanceUserTurn (const juce::String& stanceKey,
                                         const juce::String& heardTags,
                                         const juce::String& heardSpectral,
                                         const juce::String& flagsLine,
                                         const juce::String& prevPrompt,
                                         const juce::StringArray& recentList);

    // ── DEPRECATED: LCO self-check (deactivated, BJ 2026-07-21) ──────────────
    // The three declarations below drive the self-listen / compare loop, which is
    // switched OFF in the product (PromptPanel.cpp T5YNTH_LCO_SELFCHECK = 0). They
    // are retained — and still unit-tested by tools/test_reprompt_stances.cpp — not
    // deleted, so the loop can be resurrected. Not called on any live path.

    /** Build the user turn for the "selfcheck" stance: does the oscillator's
     *  translation of the prompt actually arrive in the result?
     *
     *  Deliberately NOT part of buildStanceUserTurn's key dispatch — its inputs
     *  are different in kind (it judges one sound instead of generating the next
     *  prompt), exactly as buildDcoStanceUserTurn is its own function.
     *
     *  TWO AGENTS, TWO JOBS. The machine listener DESCRIBES the sound; this turn
     *  hands that description, as text, to a second model that COMPARES it with
     *  the request. Nothing here weighs audio against intent — the comparison is
     *  text against text, which is what a language model is for. So the
     *  description arrives whole: the learned timbre words AND the computed
     *  spectral words, composed by composeHeardDescription.
     *
     *  The guard against a description that is partly noisy is not to withhold
     *  half of it, but to require a CONTRADICTION: a quality the request asks for
     *  and the description merely fails to mention is never a mismatch (see
     *  syspSelfCheck). Absence is not evidence.
     *
     *  MODEL: run this on the AUTHOR model, not the small translator, and with the
     *  backend's anti-cycling transforms OFF. Both are measured requirements, not
     *  preferences — see syspSelfCheck's comment for what each one broke.
     *
     *  @param intention    what the prompt asked for, in the user's own words
     *  @param description  the listener's description (composeHeardDescription)
     *  @return the user-turn string. */
    juce::String buildSelfCheckUserTurn (const juce::String& intention,
                                         const juce::String& description);

    /** Does a selfcheck finding accuse the sound of missing the request?
     *
     *  Decodes syspSelfCheck's answer contract — exactly "matches", or one
     *  "asked for X, but the sound is described as Y" line — into the boolean the
     *  correction loop branches on. An EMPTY finding is not a mismatch: it means
     *  the check never ran (no render, analyze failed, the model errored), and
     *  correcting against a complaint nobody made would be worse than not
     *  correcting. Anything the stance did not contract to say is likewise not
     *  read as an accusation. */
    bool selfCheckReportsMismatch (const juce::String& finding);

    /** Compose the machine listener's DESCRIPTION of one sound from the two
     *  outputs of the shipped analyze op: the learned timbre words (CLAP top-k)
     *  and the computed spectral words. This is the describing agent's product —
     *  what buildSelfCheckUserTurn hands to the comparing one. Either half may be
     *  empty. NOT what the LCO Re-Prompt ear uses: that keeps the two halves
     *  apart and separately labelled (buildDcoStanceUserTurn). */
    juce::String composeHeardDescription (const juce::String& tags,
                                          const juce::String& spectral);

    /** Frames the DCO/LCO reference vocabulary (backend dco_recipe.reference_
     *  vocabulary — the exact palette the scanner resolves) as a constraint
     *  block to append to the LCO re-prompt turn, so the LLM stops emitting
     *  words the instrument silently drops. LCO-only: the neural loop's
     *  stanceSystemPrompt/buildStanceUserTurn never see this. Returns "" when
     *  the brief is empty (backend too old / not yet baked → current behaviour).
     *  @param referenceVocabulary  the backend brief verbatim (grouped word lists) */
    juce::String dcoVocabularyConstraintBlock (const juce::String& referenceVocabulary);

    /** Port of clap_llm_loop.py `_clean_prompt`: keep the first real line, strip
     *  label echoes (heard:/neural ear:/current prompt:/…) and wrapping quotes
     *  (straight or curly), cap to ≤maxWords words / ≤maxChars chars, and drop
     *  trailing function words so the prompt ends on a content word. Load-bearing:
     *  small instruct models leak labels/quotes and overrun t5gemma's token
     *  budget. */
    juce::String cleanPrompt (const juce::String& raw,
                              int maxChars = 120, int maxWords = 12);

    /** Port of clap_llm_loop.py `_concat2`: the concat (ab_add) coupling's
     *  generation prompt = the human ORIGINAL (first impulse) + ", " + ONLY the
     *  latest interpretation — never the accumulated chain (keeps the prompt
     *  short; the chain itself runs on its own last link elsewhere). */
    juce::String concat2 (const juce::String& original, const juce::String& last);

    /** Split a trailing RUN of musical CONTROL tokens off the end of a prompt so
     *  LLM rewrites (every loop stance + the in-place translation) leave the
     *  user's appended pitch/tempo anchors UNCHANGED. Two token kinds, case-
     *  insensitive:
     *    • scientific-pitch notes — A–G, optional ASCII accidental (#, ##, b, bb),
     *      octave -1 | 0–10  (c3, C#4, Db5, A0)
     *    • tempo — 1–3 digits (optional decimals), optional space, "bpm"
     *      (120bpm, 90 bpm, 128.5 BPM)
     *  Stable Audio conditions tempo on "…bpm" and a trailing note pins register,
     *  so these must survive verbatim. Trailing-only by design: a note/tempo word
     *  INSIDE the description is left for the rewrite. ASCII accidentals only (the
     *  unicode ♯/♭ are deliberately not matched, keeping the match pure-ASCII so
     *  the byte cut is UTF-8-safe).
     *  @return the matched trailing run VERBATIM (incl. its leading separator),
     *          or empty if the prompt has no trailing musical tokens. */
    juce::String trailingMusicSuffix (const juce::String& prompt);

    /** The descriptive CORE: the prompt with its trailingMusicSuffix() removed,
     *  trimmed. Feed this to the LLM (translate / stance interpret) so it never
     *  sees — and cannot mangle — the control tokens. */
    juce::String stripMusicSuffix (const juce::String& prompt);

    /** Re-attach a suffix from trailingMusicSuffix() to a rewritten core,
     *  normalising ONLY the core↔suffix junction to ", " (token spelling and
     *  inter-token separators are preserved). Empty suffix → core.trim(); empty
     *  core → the bare tokens. */
    juce::String reattachMusicSuffix (const juce::String& core, const juce::String& suffix);
}

// ── Structured musical parse ─────────────────────────────────────────────────
// Phase 1 of "T5ynth understands its prompts": turn the trailing pitch/tempo run
// (the same tokens RepromptStances::trailingMusicSuffix preserves) into STRUCTURED
// data — pitch classes, octaves, MIDI numbers, tempo — so later consumers (the
// sequencer's BPM, scaleRoot, note seeding) can use it. Purely additive: the
// preservation path is unchanged; this just exposes what the tokens MEAN.
//
// Source-of-truth note: parse the USER's appended tokens (intentional control),
// NOT the LLM's prose (hallucination-prone, and "the machine's musical reading"
// is a bias to SURFACE, not to obey). juce_core-only, like the rest of this file.
namespace Music
{
    struct Note
    {
        int pitchClass = 0;   // 0=C, 1=C#/Db … 11=B (accidentals resolved, mod 12)
        int octave     = 0;   // scientific-pitch octave as written (the digits after the note)
        int midiNote   = 0;   // MIDI number, SPN convention: A4→69, C4(middle C)→60, i.e. (octave+1)*12+pc
                              // with pc taken before the wrap, so the octave stays the letter's:
                              // Cb3 = B2 = 47, B#2 = C3 = 48. Can fall outside 0..127 (cb-1, b#9).
                              // (If T5ynth adopts C3=60 elsewhere, that's a fixed +12 the consumer applies.)
        juce::String text;    // the token VERBATIM as the user wrote it ("C#3", "db5")
    };

    struct Spec
    {
        juce::String core;        // the prompt minus its trailing musical run (== stripMusicSuffix), trimmed
        juce::String verbatim;    // the trailing run VERBATIM (== trailingMusicSuffix); empty when none
        juce::Array<Note> notes;  // parsed pitch tokens, in source order
        bool  hasBpm = false;
        float bpm     = 0.0f;     // parsed tempo (if several bpm tokens, the LAST wins)
        juce::String bpmText;     // the bpm token VERBATIM ("120bpm", "90 bpm")

        bool hasMusic() const { return verbatim.isNotEmpty(); }
    };

    /** Parse a prompt's trailing musical run into a Spec. Spec::core/verbatim mirror
     *  RepromptStances::stripMusicSuffix/trailingMusicSuffix exactly (so the existing
     *  preservation behaviour is unchanged); notes/bpm add the structured reading. */
    Spec parse (const juce::String& prompt);

    /** The A/B pair's musical understanding, kept SEPARATE per pole — A and B are
     *  equal partners and may carry their own pitch/tempo (A "c3", B "g2, 90bpm").
     *  The single handle a Phase-2 consumer passes around; reconciling two poles
     *  onto the single seqBpm/scaleRoot is a deliberate later decision, NOT baked
     *  in here (the data stays un-merged). */
    struct ABSpec { Spec a, b; };
    ABSpec parseAB (const juce::String& promptA, const juce::String& promptB);
}
