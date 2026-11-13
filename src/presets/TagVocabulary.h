#pragma once
#include <JuceHeader.h>

/** Curated tag taxonomy surfaced in the Preset-Manager's "Known tags" cloud
 *  (Detail card + Save drawer) and used as the autocomplete vocabulary in
 *  the tag-input field.
 *
 *  Design notes:
 *   - Replaces the old `PresetTagSuggester` heuristic which silently injected
 *     useless tags ("hot", "dense", "sparse", …) derived from peak-level /
 *     RMS analysis. That heuristic was removed entirely in Phase 1.
 *   - Tags are SUGGESTIONS, not classifications. The user adds them by hand;
 *     nothing else writes tags.
 *   - The cloud renders `user-seen-tags ∪ canonical`, USER TAGS FIRST: the
 *     clouds clip after a few rows, so the tags actually in use in the
 *     library (= the sidebar's TAGS list) must render before the canonical
 *     starter pack or the two lists visibly disagree. The merge lives in
 *     PresetManagerPanel::applyTagVocabulary().
 *   - "generative" only appears when a generative sequencer engine (Phrase
 *     or Gen) is selected, since that's the only context where the tag is
 *     meaningful (sequenced motion that varies as it plays).
 *
 *  Vocabulary lives in this header (not a translation unit) because it is
 *  pure data — header-only avoids another compile target without
 *  introducing globals.
 */
namespace TagVocabulary
{
    /** Curated tag set, in display order. `genSeqActive` appends the
     *  "generative" tag — keep that suffix conditional so its absence
     *  from the cloud signals that the relevant engine state is off. */
    inline juce::StringArray getCanonical(bool genSeqActive)
    {
        juce::StringArray v {
            // Character / timbre
            "analog", "digital", "warm", "bright", "dark", "clean", "dirty",
            "metallic", "glassy", "wooden", "organic", "synthetic",
            // Function / role
            "pad", "lead", "bass", "keys", "perc", "drum", "fx", "texture",
            "drone", "arp", "pluck", "vocal",
            // Genre / style
            "ambient", "cinematic", "vintage", "lo-fi", "experimental",
            "techno", "house", "dnb", "idm",
            // Motion
            "static", "evolving", "rhythmic", "pulsing", "glitchy", "wobble",
            // Mood
            "dreamy", "aggressive", "melancholic", "ethereal", "uplifting",
            "quirky", "emotion"
        };
        if (genSeqActive) v.add("generative");
        return v;
    }
}
