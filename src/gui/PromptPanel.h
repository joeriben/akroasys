#pragma once
#include <JuceHeader.h>
#include <vector>
#include <utility>
#include <functional>
#include <map>
#include <limits>
#include <optional>
#include "../inference/PipeInference.h"
#include "../eventlog/EventLog.h"   // GenerationEventLogEntry (replay transport)
#include "../dsp/BlockParams.h"   // RepromptCoupling (Re-Prompt coupling enum)
#include "GuiHelpers.h"  // FlippedVerticalSlider, AlphaSliderLnF, impulse colours
// (the baked table is now drawn in the engine window — SynthPanel/WaveformDisplay
//  wtMode — so the LCO shows the machine's READING here, not the wave)
#include "RepromptStanceBar.h"    // Re-Prompt stance "symbol slider"
#include "LcoTraceView.h"         // the LCO authoring trace (replaces the HEARD AS box)
#include "T5ynthLookAndFeel.h"    // base LnF for the model-switch text override

class T5ynthProcessor;

/**
 * LookAndFeel for the model-switch buttons: identical to T5ynthLookAndFeel for plain
 * slots, but a slot carrying a non-empty "tierLetter" property (the two SA3 slots when
 * both tiers are installed) is drawn as a centred group [label │ letter]: the label,
 * a thin vertical divider hugging the label's right edge (no gap to its left), then the
 * s/m tier letter. Owning the whole layout here — rather than overpainting a badge —
 * lets the divider sit exactly against the measured label width while the group stays
 * balanced in the button.
 */
class ModelSwitchLnF : public T5ynthLookAndFeel
{
public:
    void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        auto font = getTextButtonFont(b, b.getHeight());
        g.setFont(font);

        const auto ink = b.findColour(b.getToggleState() ? juce::TextButton::textColourOnId
                                                         : juce::TextButton::textColourOffId)
                             .withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.5f);

        const int yIndent = juce::jmin(4, b.proportionOfHeight(0.3f));
        const int corner  = juce::jmin(b.getHeight(), b.getWidth()) / 2;
        const int fontH   = juce::roundToInt(font.getHeight() * 0.6f);
        const int left    = juce::jmin(fontH, 2 + corner / (b.isConnectedOnLeft()  ? 4 : 2));
        const int right   = juce::jmin(fontH, 2 + corner / (b.isConnectedOnRight() ? 4 : 2));
        const int textH   = b.getHeight() - yIndent * 2;

        const juce::String tier = b.getProperties().getWithDefault("tierLetter", juce::String()).toString();

        // Plain slot: stock centred label.
        if (tier.isEmpty())
        {
            g.setColour(ink);
            const int textW = b.getWidth() - left - right;
            if (textW > 0)
                g.drawFittedText(b.getButtonText(), left, yIndent, textW, textH,
                                 juce::Justification::centred, 1);
            return;
        }

        // SA3 tier slot: centre [label │ letter] as one group. The divider hugs the
        // label's measured right edge (no space to its left); the letter follows it.
        const int labelW  = font.getStringWidth(b.getButtonText());
        const int letterW = font.getStringWidth(tier);
        const int gapR    = juce::jmax(3, b.getHeight() / 5);     // divider -> letter
        const int groupW  = labelW + 1 + gapR + letterW;          // +1 = divider stroke
        const int gx      = juce::jmax(left, (b.getWidth() - groupW) / 2);
        const int dx      = gx + labelW;                          // divider, hard against the label
        const int vInset  = juce::roundToInt(static_cast<float>(b.getHeight()) * 0.26f);

        g.setColour(ink);
        g.drawText(b.getButtonText(), gx, yIndent, labelW, textH, juce::Justification::centredLeft, false);

        g.setColour(ink.withMultipliedAlpha(0.5f));
        g.drawVerticalLine(dx, static_cast<float>(vInset), static_cast<float>(b.getHeight() - vInset));

        g.setColour(ink);
        g.drawText(tier, dx + 1 + gapR, yIndent, letterW, textH, juce::Justification::centredLeft, false);
    }
};

/**
 * GENERATION column: prompts, embedding controls, compact params, generate.
 */
class PromptPanel : public juce::Component, private juce::Timer
{
public:
    explicit PromptPanel(T5ynthProcessor& processor);
    // stopTimer() first (JUCE rule). Then stop any replay: the transport's
    // generation half lives here — with the panel gone, the tape's notes would keep
    // firing while its timbre changes never arrive. Recording is unaffected (it is
    // processor-owned); only playback, which the user started from this window,
    // ends with the window.
    ~PromptPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;
    int getPreferredHeightForWidth(int width) const;
    void setEasyMode(bool easy);

    // Enable/disable every control that REQUIRES the language model: the LCO bake
    // (triggerDcoBake) and its model tab, the Re-Prompt stance bar + coupling, and
    // the Translate flag. ONE model serves all three, so this is ONE gate — a
    // second flag could only ever say the same thing, or lie. Driven by the
    // model-settings install state (MainPanel). Disabled controls dim and explain
    // themselves via tooltip; the loop also bails (runSemanticLoopStep) so a preset
    // with an engaged stance can't silently no-op when the model is absent.
    // Message thread.
    void setLlmAvailable(bool available);

    /** Name the model that authored the orchestra just received, as reported by
     *  the backend resolver (`author_model` on the csound response). Empty leaves
     *  the current display standing. Call from the message thread. */
    void setLcoAuthorModel(const juce::String& modelDirName);
    /** Name the model the backend's resolver WILL author with next (MainPanel
     *  feeds it SettingsPage::resolvedCoderDirName() at open and on live
     *  install/removal). Shown on the model tab while no authored orchestra
     *  claims otherwise; empty = none loadable (the tab states that plainly and
     *  dims via setLlmAvailable). Message thread. */
    void setLcoResolvedModel(const juce::String& modelDirName);
    /** The model that wrote the CURRENT orchestra, as claimed by the last bake
     *  or SNAP recall -- empty when none has. Deliberately NOT the tab's visible
     *  text: idle, the tab names the model that would author NEXT, and a SNAP
     *  must store who wrote the orchestra it holds, not who would write another
     *  one. */
    juce::String getLcoAuthorModel() const { return lcoAuthorClaim_; }
    bool isEasyMode() const { return easyMode_; }
    bool hasHiddenActiveState() const;

    /** Load preset data that isn't in APVTS (prompts, seed, random toggle, device, model,
     *  research-mode injection state). The injection fields are optional — when omitted,
     *  the panel keeps its current values, which lets callers decide per-feature whether
     *  to push or preserve. */
    void loadPresetData(const juce::String& promptA, const juce::String& promptB,
                        int seed, bool randomSeed,
                        const juce::String& device = {},
                        const juce::String& model = {},
                        const juce::String& injectionMode = {},
                        float lateMixAmount = std::numeric_limits<float>::quiet_NaN(),
                        float splitStart = std::numeric_limits<float>::quiet_NaN(),
                        float splitEnd = std::numeric_limits<float>::quiet_NaN());

    /** Read current prompt/seed state (for preset save). Seed state lives on the
     *  processor (setLastSeed/getLastSeed, setLastRandomSeed/getLastRandomSeed),
     *  driven exclusively by the Easy-mode Variation switchbox — the Advanced
     *  seedEditor/randomSeedToggle widgets were removed (DCO Slice 0). */
    juce::String getPromptA() const { return promptAEditor.getText().trim(); }
    juce::String getPromptB() const { return promptBEditor.getText().trim(); }

    /** LCO (Advanced) prompt text — a SNAP slot carries it alongside the orchestra
     *  it stored, so a recall shows the words the sound came from. get returns the
     *  raw editor text; set replaces it without notifying (never auto-bakes: the
     *  recall brings the stored orchestra back directly, and a slot taken before any
     *  bake parks the prompt for the user to bake via GENERATE when ready). */
    juce::String getLcoPrompt() const { return dcoPromptEditor.getText(); }
    void setLcoPrompt(const juce::String& t) { dcoPromptEditor.setText(t, juce::dontSendNotification); }

    /** Restore the trace for an orchestra that was RECALLED rather than authored
     *  (preset load, SNAP recall), so a reload shows what is actually sounding
     *  instead of an empty placeholder or a re-derived reading.
     *
     *  A recall genuinely knows LESS than a bake: a preset stores the prompt, the
     *  reading and the author's name, never which library entries the words
     *  reached or which compiler errors the body was repaired past. Those
     *  stations are therefore left OUT rather than drawn empty — an absent
     *  station says "not known here", a blank one would claim "nothing
     *  happened". */
    // Preset restore writes the card directly. Bumping the bake generation here
    // is what stops an in-flight self-check — started on the PREVIOUS orchestra,
    // still running because CLAP plus an LLM turn take many seconds — from landing
    // on the freshly loaded preset and describing a sound that is no longer loaded.
    //  `authorModel` is passed in rather than read off the model tab: a SNAP
    //  recall sets the tab AFTER restoring the card, and a preset does not store
    //  an author at all — reading the tab here would caption a recalled orchestra
    //  with the name of whoever wrote the previous one.
    //  `csoundBody` is the back of the card, and it is passed in rather than
    //  read off the processor for the same reason `authorModel` is: the
    //  processor's copy outlives the sound it belongs to. importJsonPreset
    //  writes it only when the preset carries a Csound orchestra and never
    //  clears it otherwise, so a wavetable-LCO preset loaded after a Csound bake
    //  would put the PREVIOUS sound's code on the back of the new one's trace —
    //  exactly the mismatch this card must not show. Only the caller knows
    //  whether the sound it is restoring has one.
    void setLcoRecalledTrace(const juce::String& prompt, const juce::String& reading,
                             const juce::String& authorModel,
                             const juce::String& csoundBody);

    /** The authored body cut back out of a wrapped orchestra, or empty if the
     *  scaffold's boundaries are not both there. */
    static juce::String bodyFromOrchestra(const juce::String& orchestra);

    /** Point the Re-Prompt chain at a prompt that was RECALLED into the editor
     *  rather than typed or baked. With a stance engaged, GENERATE re-reads
     *  `dcoLoopLast_` — not the editor — so without this a recalled prompt is
     *  never the one that gets rewritten and baked; the previous prompt is,
     *  and the recalled text is then overwritten by the rewrite. Recency
     *  handling mirrors the bake (an unchanged prompt keeps the avoid-list).
     *  Message thread. */
    void adoptRecalledPrompt(const juce::String& prompt)
    {
        if (prompt == dcoLoopLast_)
            return;
        dcoLoopLast_ = prompt;
        dcoLoopRecent_.clearQuick();
        if (prompt.isNotEmpty())
            dcoLoopRecent_.add(prompt);
    }

    /** Take over the Re-Prompt chain's bookkeeping for an orchestra that was
     *  RECALLED rather than authored (MainPanel's SNAP recall): the stance turns
     *  carry the panel's last prompt, so leaving it on the bake the recall just
     *  replaced would make GENERATE rewrite a sound that is no longer loaded.
     *  Mirrors what triggerDcoBake's completion lambda sets. The reading travels
     *  with it for the trace and the preset; since the Re-Prompt step listens
     *  instead of quoting it, no stance turn reads it any more.
     *  Message thread. */
    void adoptRecalledOrchestra(const juce::String& prompt, const juce::String& reading)
    {
        dcoLastMachineReading_ = reading;
        dcoLastFlagsLine_ = {};
        // A recall publishes an orchestra without being a bake, so it has to clear
        // the ear-failure latch itself: an orchestra that could not be listened to
        // has just been REPLACED by this one, and leaving the latch armed would send
        // the next GENERATE press into a bake that authors over the sound the user
        // deliberately recalled — silently, since the message that armed it belonged
        // to the previous orchestra.
        dcoEarFailed_ = false;
        adoptRecalledPrompt(prompt);
    }

    /** Drop the author claim — the orchestra now loaded has no known author (a
     *  preset, or a bake whose backend reported none), and a stale name claiming
     *  otherwise is worse than no claim. The tab falls back to the resolved
     *  idle name (who would author next), or its no-model placeholder. */
    void resetLcoAuthorModel();

    /** Open the compile-window poll (pollCsoundCompile) for an orchestra that was
     *  just handed to requestCsoundOrchestra(): the flags line reports
     *  compiling -> ok/error until the swap resolves. Called by the bake's own
     *  completion lambda and by MainPanel's SNAP recall, so a recalled orchestra
     *  reports its compile exactly like a freshly authored one. Message thread. */
    void beginCsoundCompileWatch();

    // formatLcoDisclosure (reading + "── Parametrisation ──" + the raw authored
    // body, as one blob of text) is gone: the disclosure is the LcoTraceView's
    // stations now, and the Csound body becomes the BACK of that card rather
    // than a slab printed under a rule (BJ 2026-07-24). The body itself is still
    // kept — processor-side, in csoundParamsText — because the back side and the
    // preset round-trip both need it.

    /** DEPRECATED (LCO self-check deactivated, BJ 2026-07-21): no longer called
     *  on any live path — see PromptPanel.cpp's T5YNTH_LCO_SELFCHECK switch.
     *  Retained, not deleted.
     *
     *  The self-check section: how a machine listener DESCRIBED the bare
     *  oscillator, and the one sentence a second model wrote comparing that
     *  description with the prompt.
     *
     *  The description is printed in full, above the finding, and is the same
     *  string the comparison was made on (RepromptStances::composeHeardDescription
     *  builds both). That is deliberate: the finding is one model's reading of
     *  another model's words, so showing only the conclusion would present it as
     *  a fact about the sound. With the description on screen the user can see
     *  what the comparison actually had to work with — and disagree with it.
     *  No score, no percentage, no verdict on the sound. */
    static juce::String formatSelfCheck(const juce::String& description,
                                        const juce::String& finding)
    {
        juce::String out;
        if (description.isNotEmpty()) out += "heard as: " + description + "\n";
        if (finding.isNotEmpty())     out += finding;
        return out.trimEnd();
    }
    int getSeed() const;        // defined in the .cpp: T5ynthProcessor is only
    bool isRandomSeed() const;  // forward-declared here

    /** Read current injection-mode state (for preset save). */
    juce::String getInjectionMode()  const { return injectionMode_; }
    float        getLateMixAmount()  const { return lateMixForMode(injectionMode_); }
    float        getSplitStart()     const { return splitLayerStart_; }
    float        getSplitEnd()       const { return splitLayerEnd_; }

    /** Trigger generation with optional dimension offsets from DimensionExplorer. */
    void triggerGenerationWithOffsets(std::vector<std::pair<int, float>> offsets);

    /** LCO (Advanced) counterpart of triggerMainGeneration: the reused GENERATE
     *  button routes here in LCO mode. Stance Off (or no prior bake yet) → author/
     *  bake from the prompt; stance engaged → one re-prompt STEP (read recipe →
     *  rewrite → bake). One entry point so button + Cmd/Return + XL CC all agree. */
    void triggerLcoGenerate();

    /** Set semantic axis values to include in the next generation request. */
    void setSemanticAxes(std::map<juce::String, float> axes) { pendingAxes_ = std::move(axes); }

    /** Re-read available devices/models after the backend was restarted. */
    void refreshInferenceChoices();

    /** Called after generation with prompt refs + explorer baseline. */
    std::function<void(const std::vector<float>&, const std::vector<float>&, const std::vector<float>&)> onEmbeddingsReady;

    /** Status callback — called with status text (e.g. "generating...", "12.3s | seed 42 | mps") */
    std::function<void(const juce::String&, bool generating)> onStatusChanged;

    /** LCO busy callback — fired true when a bake/re-prompt starts, false when it
     *  ends. MainPanel uses it to disable the reused GENERATE button during an LCO
     *  authoring pass (the neural glow/cache path never runs in LCO). */
    std::function<void(bool busy)> onLcoBusyChanged;

    /** Fired once per new sound, on BOTH oscillators, at the AUDIO — the LCO
     *  bake where it publishes the authored orchestra, the neural render in its
     *  successful result. Never at the button: both paths fail asynchronously
     *  (dead backend, timeout, authoring error) and leave the loaded preset
     *  playing, and a preset that is still sounding must keep its name. The new
     *  sound is not the loaded/last-saved preset any more, so MainPanel drops
     *  the stale currentPresetFile/lastPresetName identity here — otherwise the
     *  Save dialog keeps defaulting to that old name and shows a "Replace"
     *  warning for a file the new sound has nothing to do with. Four paths
     *  deliberately do NOT fire it: LCO re-prompt corrections (they end in
     *  triggerDcoBake, which publishes and fires), the cache-replay
     *  short-circuit (a preset saved with its cache is auditioning its OWN
     *  stored variants), the drift auto-regen (an unattended background refresh
     *  of the preset the user is still sitting on), and replay playback
     *  (fireReplayGeneration — the tape restores the patch it snapshotted). */
    std::function<void()> onNewGenerationStarted;

    /** Callback to read AxesPanel values with per-slot drift offsets (wired by MainPanel). */
    std::function<std::map<juce::String, float>(float, float, float)> getAxisValuesCallback;

    /** Fired whenever the selected model may have changed (model click, preset
     *  load, backend availability). MainPanel uses it to flip the Resynth gate
     *  (SA3-only). Semantic Axes / Dimension Explorer now run on every engine. */
    std::function<void()> onModelChanged;

    /** Fired when the user toggles the SA3 tier (small/medium) by re-clicking the
     *  active SA3 slot. MainPanel persists it to ui_settings.json (machine-local). */
    std::function<void(juce::String)> onSa3TierChanged;

    /** Paint ghost overlay for alpha slider (drift modulation indicator). */
    void paintOverChildren(juce::Graphics& g) override;

    bool isGenerating() const { return generating; }

    /** Backend-selected inference device for this machine ("mps"/"cuda"/"cpu").
     *  Empty until the subprocess reports its device list. Used by the preset
     *  manager to flag presets rendered on a different device type. */
    const juce::String& getInferenceDevice() const { return defaultInferenceDevice_; }

    /** Set the per-machine SA3 tier ("small"/"medium"). persist=true writes it back
     *  through onSa3TierChanged. Re-resolves which checkpoint backs the SA3 slots. */
    void setSa3Tier(const juce::String& tier, bool persist);
    juce::String getSa3Tier() const { return sa3Tier_; }

private:
    void timerCallback() override;
    void triggerGeneration();
    // Manual Union-Jack action: translate the A/B prompts to English IN PLACE.
    // Pauses auto-regen for the duration (frees the shared IPC pipe); it resumes
    // automatically, with its unchanged bar setting, when the translation finishes.
    void translatePromptsInPlace();
    bool playNextCachedInference();
    /** Push the realized seed from a generation result onto the processor's
     *  seed store (replaces the old seedEditor display sync — no widget to
     *  update anymore, the Easy Variation switchbox reads processor state). */
    void syncSeedState(int seed);
    /** Easy-mode entry point for an exact fixed seed: opened by double-clicking
     *  the Lock (steady) button in the Variation switchbox. Async AlertWindow —
     *  mirrors the "Rename Preset" pattern in MainPanel.cpp. On OK, writes the
     *  typed value into the processor's seed store and switches to steady mode. */
    void openSeedEntryDialog();

    /** Build a PipeInference::Request from current UI state, with optional overrides. */
    PipeInference::Request buildInferenceRequest(float alphaOverride = std::numeric_limits<float>::quiet_NaN(),
                                                  std::map<juce::String, float> axesOverride = {},
                                                  float noiseOverride = std::numeric_limits<float>::quiet_NaN(),
                                                  float magnitudeOverride = std::numeric_limits<float>::quiet_NaN(),
                                                  float lateMixOverride = std::numeric_limits<float>::quiet_NaN(),
                                                  float splitStartOverride = std::numeric_limits<float>::quiet_NaN(),
                                                  float splitEndOverride = std::numeric_limits<float>::quiet_NaN(),
                                                  float resynthOverride = std::numeric_limits<float>::quiet_NaN());

    /** Trigger generation from drift auto-regen. */
    void triggerDriftRegeneration(float effectiveAlpha,
                                  std::map<juce::String, float> effectiveAxes,
                                  float effectiveNoise,
                                  float effectiveMagnitude,
                                  float effectiveLateMix,
                                  float effectiveSplitStart,
                                  float effectiveSplitEnd,
                                  float effectiveResynth = std::numeric_limits<float>::quiet_NaN(),
                                  bool holdForBar = false);

    /** Check if drift requires auto-regeneration (called from timerCallback). */
    void pollDriftRegen();

    /** The offline cache take (CACHE row's rightmost switch).
     *
     *  A cache take records the drift as it sweeps past the auto-regen cadence
     *  points. Which points it CATCHES depends on the machine: while a render is
     *  in flight the poll returns early, the cadence point passes unused, and the
     *  drift keeps sweeping meanwhile — so a fast box records the trajectory every
     *  bar and a slow one every few, and the Resynth carry / Re-Prompt rewrite
     *  chain skips the same steps with it.
     *
     *  The offline take closes that: the generation-side drift is frozen for the
     *  whole take and moves only by one cadence interval per CAPTURED entry (see
     *  T5ynthProcessor::setDriftGenerationHold). The parameters wait at Tc1 until
     *  the render lands, then step to Tc2. Recording only — with the cache Off
     *  nothing changes, and once the take is full it plays back like any other. */
    bool isAsyncCacheMode() const;
    bool isAsyncTakeRecording() const;

    /** True when a Drift slot aims at a generation parameter AND its Amount is
     *  non-zero — i.e. there is a trajectory for a take to record at all. */
    bool driftCanCarryATake() const;

    /** Advance the take by one cadence interval — called with the result of the
     *  cache write, so an uncaptured point (cache Off, cache full, failed render)
     *  never consumes a step. */
    void stepOfflineTakeAfterCapture(bool captured);

    /** The auto-regen cadence in seconds, from the REGENERATE mode + its BPM: what
     *  one step of an offline take advances the generation-side drift by. ASAP
     *  resolves to the same 1-beat floor cache playback already uses — without a
     *  defined interval every entry of a take would sit on the same drift value. */
    double cadenceIntervalSeconds() const;

    /** LCO twin of pollDriftRegen's stance loop: paces the LCO Re-Prompt off the
     *  SAME REGENERATE switchbox (drift_regen + its BPM). Called from
     *  timerCallback; exactly one of the two polls runs per tick (easyMode_). */
    void pollLcoRepromptCadence();

    /** R2c: replay transport — fire the generation the playhead just crossed.
     *  Called from timerCallback; a no-op unless a .t5evt replay is running.
     *  Mirrors pollDriftRegen's shape: the tape never stalls, the regenerated
     *  timbre crossfades in whenever the backend finishes. */
    void pollReplayRegen();
    void fireReplayGeneration(const GenerationEventLogEntry& logged);
    // Held across ticks when the pipe is busy: the processor has already handed us
    // this generation (and will not arm the next until it completes), so dropping it
    // here would silently truncate the tape's generation chain. Tagged with the tape
    // epoch it was taken from, so a stop/restart inside one timer gap discards it
    // instead of firing an old tape's generation against the new one.
    std::optional<GenerationEventLogEntry> pendingReplayGen_;
    uint32_t pendingReplayEpoch_ = 0;
    // Did the previous replayed generation actually render? An internal-resynth link
    // seeds from the parent's output sitting in the processor — if the parent failed
    // at replay time, that buffer is the GRANDparent's and the seed would be wrong.
    bool lastReplayGenSucceeded_ = false;

    /** One step of the CLAP→LLM semantic self-listening loop, mirroring one
     *  iteration body of clap_llm_loop.py run_llm_loop (minus its own generate()).
     *  Called from BOTH generation-complete callbacks (manual + Auto-Regen) on the
     *  message thread with the just-rendered result. It spawns a background thread
     *  that analyzes the audio (CLAP ear) and interprets the tags into the next
     *  prompt(s) per the active stance + coupling, then writes them back into the
     *  editor(s) on the message thread WITHOUT re-triggering onTextChange. The NEXT
     *  generation (manual Generate or the Auto-Regen standing trigger) renders the
     *  rewritten prompts. No-op (cheap early-out) unless a stance is active AND the
     *  model is SA3. */
    void runSemanticLoopStep(const PipeInference::Result& result);

    T5ynthProcessor& processorRef;

    // Prompts — colour-coded editors (purple A / yellow B) replace the old text labels
    juce::TextEditor promptAEditor, promptBEditor;
    UnionJackButton translateToggle;  // session-only: translate prompts → English before conditioning

    // Embedding controls.  alphaLnF is declared BEFORE alphaSlider so the slider
    // (a later member) is destroyed first — the LnF must outlive its only user.
    AlphaSliderLnF alphaLnF;             // A→B gradient track + position-coloured thumb
    FlippedVerticalSlider alphaSlider;   // vertical A↔B blend, A at top
    juce::Label alphaLabel, alphaValue;  // retained for callbacks but hidden (gradient is self-describing)

    // Duration — house-standard inline-bar SliderRow (mirrors MainPanel's
    // RESYNTH row): accent-band label + fill bar in kOscCol, with the "Ns"
    // read-out as the inline value. Easy view only (moved out of Advanced
    // entirely). Declared BEFORE durA (below) so the attachment tears down
    // first (reverse destruction order).
    std::unique_ptr<SliderRow> durationRow;
    // Easy-view "VAR" caption for the Variation switchbox row (the 3 seed-mode
    // icons framed by paintSwitchBoxBorder — no card, standard row height).
    juce::Label varSwitchLabel;
    // Magnitude / Chaos — house-standard inline-bar SliderRows (mirror
    // durationRow above). Easy view only (moved out of Advanced entirely).
    // Declared BEFORE magA/noiseA (below) so the attachments tear down first
    // (reverse destruction order).
    std::unique_ptr<SliderRow> magRow, noiseRow;
    // DCO surface — Advanced IS the DCO panel now (a completely different
    // paradigm from the neural Easy view, not a variant of it): a 3-line
    // prompt editor (panel-local text, NOT bound to Impulse A), and the
    // AUTHORING TRACE (dcoTraceView — what was heard, what it reached, who wrote
    // it, what had to be repaired, what is running; where the baked wave used to
    // sit, the wave itself now drawing in the engine window). GENERATE authors a
    // Csound orchestra from the DCO prompt (triggerDcoBake), which forces the
    // engine into Csound mode and hands the compiled orchestra off to
    // requestCsoundOrchestra().
    juce::TextEditor dcoPromptEditor;
    // Status/error channel — kept as the logical holder (many call sites write
    // it) but no longer laid out as its own visible line; its text is routed
    // into dcoTraceView below (see setLcoStatus).
    juce::Label dcoStatusLabel;
    // Compile-state holder, likewise no longer laid out: the write-path only ever
    // put "compiling..." / the Csound error here (the per-word guardrail flags it
    // was built for belong to the retired keys path), and that state is now the
    // trace's RUNNING station. Written by setLcoCompileState and cleared by
    // setLcoStatus.
    juce::Label dcoFlagsLabel;
    // The AUTHORING TRACE, filling the middle of the LCO panel: what was heard,
    // what the words reached in the library, who wrote it and what they said
    // they wrote, what the compiler sent back, and what is running now (see
    // LcoTraceView.h). It replaced the single "HEARD AS" text box, which showed
    // the reading with the raw Csound body printed under a rule — BJ 2026-07-24:
    // the authoring is to be transparent, and a slab of code is not that.
    // The dual A+B/harmonic-inharmonic split this surface used to carry is
    // retired (BJ 2026-07-17: "this split is dead") — one combined authored
    // voice has no per-engine reading to show. Also carries status/error text
    // when there is no trace yet (setLcoStatus).
    LcoTraceView dcoTraceView;
    bool dcoBaking_ = false;
    /** The LCO GENERATE trigger (SPEC_phase4_5_csound_llm_preset.md, Phase 4):
     *  authors a Csound orchestra from dcoPromptEditor's text via the coder/
     *  interpreter model (backend/csound_author.py — one constrained instruct
     *  call + at most one retry, LLM-first, no fallback), forces the engine
     *  into Csound mode (T5ynthProcessor::forceCsoundEngineMode, reusing
     *  dcoPrevEngineMode_), and hands the compiled orchestra text off to
     *  requestCsoundOrchestra(). */
    void triggerDcoBake();
    /** Write text into both the logical status holder and the visible HEARD AS
     *  box (which doubles as the LCO status/error channel until a reading
     *  exists). tooltip is optional context shown on the box. */
    //  `busy` = something is running and the user is waiting for it; the line
    //  then gets a pulsing dot. Everything else is a state that will not change
    //  on its own and stays still.
    void setLcoStatus(const juce::String& text, const juce::String& tooltip = {},
                      bool busy = false);

    /** The compile window's report, routed into the trace's RUNNING station (and
     *  into dcoFlagsLabel, which stays as the logical holder). Kept apart from
     *  setLcoStatus for one reason: a compile result must NOT wipe the trace of
     *  the very orchestra it is reporting on. `Unknown` is a real state — an
     *  abandoned compile window must not report the success it never saw. */
    void setLcoCompileState(LcoTraceView::CompileState state, const juce::String& detail = {});

    /** Phase 5 compile-window poll (SPEC_phase4_5_csound_llm_preset.md):
     *  called from the EXISTING 10Hz timerCallback() (PromptPanel is already
     *  a juce::Timer with a correct stopTimer()-first destructor — see
     *  ~PromptPanel — so this reuses that established, already-safe Timer
     *  rather than standing up a second Timer subclass with its own
     *  lifetime to get right). A cheap no-op every tick except during the
     *  window triggerDcoBake() opens right after a successful author +
     *  requestCsoundOrchestra() call: polls csoundCompileInFlight() /
     *  csoundSwapPending() / csoundSwapFading() / csoundCompileError() until
     *  the request has resolved (covers BOTH the fade path and the "no ready
     *  active engine -> instant adopt" path, which never sets
     *  swapPending/swapFading at all), then reports compiling -> ok/error
     *  via dcoFlagsLabel. */
    void pollCsoundCompile();
    bool   csoundCompileWatching_ = false;   // a compile-window poll is active
    bool   csoundCompileSeenBusy_ = false;   // observed at least one busy tick this window
    double csoundCompileWatchStartMs_ = 0.0; // juce::Time::getMillisecondCounterHiRes() at window-open

    /** Poll the latched performance-ended state (called every tick). Keys its
     *  precedence on dcoTraceView.compileState() — the view's own truth —
     *  never on a shadow copy of the last write, because setTrace()/setStatus()
     *  reset the station without passing through setLcoCompileState. */
    void pollCsoundPerformanceEnded();

    // DCO Re-Prompt (stance-driven self-listening loop, docs/DCO_REPROMPT_CONCEPT.md
    // and its Nachtrag of 2026-07-28): a SECOND stance bar bound to its OWN parameter
    // (dcoRepromptStance — never repromptStance; paradigm isolation, see
    // BlockParams.h). One step = a bare probe render of the authored orchestra, CLAP
    // describes it, the language model rewrites the LCO prompt under the selected
    // stance, then triggerDcoBake() re-bakes it. A step that cannot listen does not
    // step — there is no fallback onto the author's own READING line. The
    // reused GENERATE button drives it (there is no separate STEP button): stance
    // Off → bake, stance engaged → one step. All state below is panel-local and
    // deliberately NOT persisted (the same seam as dcoPromptEditor itself, above):
    // the editor IS the visible chain, and v1 has no auto-restore of the human
    // original when the stance returns to Off (concept doc "Ein Feld, keine
    // Historie sichtbar").
    RepromptStanceBar dcoStanceBar;
    // "RE-PROMPT" framed card (house ModuleBox — the same template as the neural
    // repromptModuleBox), holding just the DCO stance bar (no coupling column:
    // the DCO chain has no A/B poles to couple). Sits directly above the reused
    // GENERATE button, which drives it: stance Off -> bake, stance engaged ->
    // one re-prompt step.
    ModuleBox dcoRepromptBox;
    // Whether an authored instrument may also set the synth's own controls
    // (PID::lcoSetsParams). Sits with the RE-PROMPT card because it governs the
    // same button: it says what pressing GENERATE is allowed to do. Attachment
    // declared after the button (reverse destruction order).
    juce::TextButton dcoSetsParamsBtn { "KNOBS" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> dcoSetsParamsBtnA;
    // The card's KNOBS station, re-read whenever the borrow changes.
    //   known    the trace on screen has an answer at all (a recalled preset has
    //            none, and it also goes false once the record is final)
    //   rev      the processor revision the station was last drawn from
    //   gen      the processor GENERATION it belongs to: once a preset or a
    //            restored session replaces the request, this no longer matches
    //            and the live half of the station stops being about this sound
    //   asked    the lines as last drawn — the record, shown after that point
    //   refused  the part of the station that belongs to the authoring rather
    //            than to the live patch, so it is kept here
    // Message thread only.
    void refreshLcoKnobStation();
    bool dcoKnobsKnown_ = false;
    int  dcoKnobsRev_ = -1;
    int  dcoKnobsGen_ = -1;
    juce::StringArray dcoKnobsAsked_;
    juce::StringArray dcoKnobsRefused_;
    juce::String dcoLastMachineReading_, dcoLastFlagsLine_, dcoLoopLast_;
    // DEPRECATED (self-check deactivated 2026-07-21): the disabled
    // T5YNTH_LCO_SELFCHECK loop was its only writer, so it now stays empty and no
    // "Self-check" card section is produced; the remaining .clear() calls are
    // harmless. Retained, not deleted.
    // Self-check section for the current bake. Message-thread only. Deliberately
    // NOT stashed on the processor and NOT persisted: it is a finding about the
    // sound THIS bake made, and a restored preset that showed one would be
    // asserting a check that never ran on this session's render.
    juce::String dcoSelfCheck_;
    // Bake generation, bumped by triggerDcoBake. The self-check runs long after
    // its bake (render + CLAP + interpret) and must prove it is still describing
    // the sound on screen. A busy FLAG cannot do that: by the time a slow check
    // returns, a second bake may have both started AND finished, clearing the
    // flag — and the stale finding would then be appended to the new bake's card,
    // asserting a measurement of a sound that is no longer loaded.
    unsigned long long dcoBakeSeq_ = 0;
    // DEPRECATED (self-check deactivated 2026-07-21): read only by the disabled
    // T5YNTH_LCO_SELFCHECK loop. Retained, not deleted.
    // How many times a bake may re-author itself after the comparer accuses the
    // sound of missing the request. BJ, 2026-07-20: "es gibt max 5 Korrekturen".
    // Each correction costs a full author + render + analyze + compare, and the
    // panel stays busy for all of them, so this is a hard stop and not a target —
    // the loop leaves as soon as a pass is not accused.
    static constexpr int kMaxSelfCorrections = 5;
    juce::StringArray dcoLoopRecent_;
    // The Re-Prompt LLM's allowed palette — the scanner's own vocabulary, sent
    // back as a sibling field on every author response (backend dco_recipe.
    // reference_vocabulary). Static per lexicon; cached from the first non-empty
    // bake and appended to every re-prompt turn so the LLM stops emitting words
    // the instrument silently drops. Message-thread only (written in the bake
    // callAsync, read in triggerDcoReprompt) — no cross-thread access.
    juce::String dcoReferenceVocab_;
    bool dcoRepromptBusy_ = false;
    // Set when a STEP found the ORCHESTRA unlistenable — it failed to compile, went
    // non-finite, or never left the noise floor: the NEXT GENERATE press authors
    // instead of stepping, so a sound the ear cannot reach is never a dead end
    // (hasCsoundOrchestra() reads the last REQUESTED text and is never rolled back
    // on a failed compile, so without this every press would retry a step that
    // cannot work). NOT set when the EAR was merely unreachable — that is a wait,
    // not a broken instrument, and must never reroute GENERATE into re-authoring.
    // Cleared by anything that publishes a different orchestra: a bake's completion
    // lambda and adoptRecalledOrchestra (preset restore / SNAP recall).
    // Message thread only.
    bool dcoEarFailed_ = false;
    void triggerDcoReprompt();

    static constexpr int kNumSeedModeBtns = 3;
    // Declared BEFORE seedModeBtns so it outlives them (LnF destruction order).
    IconButtonLnF seedBtnLnF;
    juce::TextButton seedModeBtns[kNumSeedModeBtns];
    juce::Rectangle<int> seedModeSwitchBounds;
    enum class SeedMode { base = 0, steady = 1, autoRandom = 2 };
    SeedMode seedMode_ = SeedMode::base;
    void setSeedMode(SeedMode mode, bool applyState);
    void syncSeedModeFromCurrentState();
    void syncSeedModeButtons();

    // Refresh the LCO single-tab model strip from the language-model install
    // flag: the tab shows the model at full opacity when installed and dimmed
    // when absent.
    void updateLcoModelTabs();
    // Recompute the model tab's text + tooltip from the two name sources: the
    // bake claim wins while it stands, else the resolved idle name, else the
    // no-model placeholder.
    void refreshLcoModelTabText();

    // Model selector (fixed 4-slot switchbox: SA3 Music | SA1 Open | SA1 Small | AudioLDM2).
    // Easy-only: the model choice belongs to the neural view — Advanced is
    // the DCO panel, where no neural engine is involved.
    static constexpr int kNumModelSlots = 5;
    // Declared BEFORE modelBtns so it outlives them (LnF destruction order).
    ModelSwitchLnF modelSwitchLnF;
    juce::TextButton modelBtns[kNumModelSlots];
    // LCO (Advanced) model selector — a single-tab strip surfacing the LCO's
    // author LLM (which is also the app's only LLM), styled EXACTLY like modelBtns[] (same
    // modelSwitchLnF, connected edges). Display-only for now (selection is future
    // work); the tab lights when the model is installed and dims when absent
    // (updateLcoModelTabs). Must stay declared AFTER modelSwitchLnF above (LnF
    // destruction-order rule: the LnF must outlive every component whose
    // setLookAndFeel points at it).
    static constexpr int kNumLcoModelSlots = 1;   // the LCO author
    // Idle text while NO model is resolvable (nothing installed): a plain state
    // statement, never a pseudo model name ("LCO author" read like a model
    // called that, and a compiled-in "Coder 7B" once stayed on screen while a
    // different model wrote every orchestra). With a model present the tab
    // names it: "<model> — writes code for Csound" (refreshLcoModelTabText).
    static constexpr const char* kLcoNoModelLabel = "no language model";
    juce::TextButton dcoModelBtns[kNumLcoModelSlots];
    // The tab's two name sources: who WOULD author next (the resolver mirror,
    // pushed by MainPanel) and who DID author the current orchestra (the
    // backend's per-bake claim). The claim wins while it stands.
    juce::String lcoResolvedModel_;   // dir name; empty = none loadable
    juce::String lcoAuthorClaim_;     // dir name; empty = current orchestra unclaimed
    juce::String modelSlotIds[kNumModelSlots];  // resolved model directory name per slot
    juce::Rectangle<int> modelSwitchBounds;

    // SA3 tier (small | medium): a per-machine meta-choice with NO dedicated widget.
    // It is surfaced as a small "s"/"m" cell (divider + letter) that ModelSwitchLnF
    // draws on the two SA3 model slots from their "tierLetter" property, and toggled by
    // a SECOND click on the already-selected SA3 slot (see modelBtns onClick). Shown/
    // togglable only when BOTH tiers are installed (otherwise forced, cell hidden). NO
    // APVTS — machine-local, persisted to ui_settings.json by MainPanel via
    // onSa3TierChanged, never preset or automation. Default "small": installing medium
    // no longer silently overrides the lighter one.
    juce::String sa3Tier_ = "small";
    bool sa3TierChoiceAvailable_ = false;         // both tiers installed → tier cell + re-click toggle
    int  activeModelSlot_ = -1;                   // last-selected slot, for SA3 re-click detection

    // Unified switchbox frame for the injection-mode buttons (matches
    // modelSwitchBounds / seedModeSwitchBounds), and a divider line separating
    // the impulse/blend group from the params below.
    juce::Rectangle<int> injModeSwitchBounds;
    int paramsDividerY = -1;
    bool modelsPopulated = false;
    juce::String pendingModel_;  // deferred model selection until models are populated
    // Deferred split values: when a preset arrives before the backend has
    // reported its model list, we cache the preset's splitStart/splitEnd
    // here and clamp them against ditBlocks_ once populateModelSelector
    // has run refreshDitBlocksForCurrentModel for the resolved model. NaN
    // means "no pending value" so the panel falls back to current state.
    float pendingSplitStart_ = std::numeric_limits<float>::quiet_NaN();
    float pendingSplitEnd_   = std::numeric_limits<float>::quiet_NaN();
    void populateModelSelector();
    juce::String getSelectedModel() const;
    /** Index of the currently-toggled, occupied model slot, or -1 if none. The
     *  Music (0) and SFX (1) slots can hold the SAME SA3 checkpoint, so the slot
     *  index — not the model id — is what selects the SA3 track type at request time. */
    int getSelectedSlot() const;
    /** Pull the active model's DiT block count from the backend's handshake
     *  metadata into ditBlocks_, then clamp/expand the layer-split slider
     *  accordingly. Called after model selection changes and after the
     *  backend's ready frame arrives. */
    void refreshDitBlocksForCurrentModel();

    /** Re-scope the Duration slider for the active model: 120s for SA3 (music-
     *  scale / deconstructed samples), 11s otherwise. Clamps the parameter into
     *  the new ceiling. Called wherever refreshDitBlocksForCurrentModel runs. */
    void applyDurationRangeForCurrentModel();

    // Device selection is backend-controlled: GPU/Metal when available, else CPU.
    juce::String defaultInferenceDevice_;
    bool devicesPopulated = false;
    void populateDeviceChoice();

    juce::TextButton generateButton { "Generate" };

    // ── Temporary injection-mode test UI (research; not in APVTS). ──
    // Six toggle buttons + the existing alphaSlider whose label/range/state
    // shift with the active mode (linear=A↔B, step-in=step-transition, layer=split,
    // combo1/2/3=Step-in-style slider with hardcoded layer band).
    juce::TextButton injModeLinear { "Linear" };
    juce::TextButton injModeFine   { "Step-in" }; // = "late_step" — operates on sampler refinement steps
    juce::TextButton injModeLayer  { "Layer" };   // = "layer_split"
    juce::TextButton injModeKombi1 { "Combo 1" }; // = late × low-layer band [0..4]
    juce::TextButton injModeKombi2 { "Combo 2" }; // = late × broad-mid band [4..12]
    juce::TextButton injModeKombi3 { "Combo 3" }; // = late × narrow-center band [6..10]
    juce::String     injectionMode_         = "linear";  // "linear" | "late_step" | "layer_split" | "kombi1"/"kombi2"/"kombi3"
    // Step-in and the three Combo modes each remember their own slider position
    // (0–1, intensity), so a user A/B-ing the modes by clicking buttons
    // doesn't lose the last-used value of any individual mode. Layer mode
    // uses splitLayerStart_/splitLayerEnd_ instead. The backend's
    // `injection_transition_at` is the early-phase fraction; we send
    // 0.5 - 0.45·t (so t=0 → transition halfway, t=1 → almost immediate),
    // and `late_phase_alpha` = t directly (0 → 50/50 late blend, 1 → pure B).
    float            lateMixFine_           = 0.5f;
    float            lateMixKombi1_         = 0.5f;
    float            lateMixKombi2_         = 0.5f;
    float            lateMixKombi3_         = 0.5f;
    /** Returns a reference to the slot that owns the slider value for the
     *  active mode. Falls back to the Step-in slot for "linear" and "layer_split"
     *  (those modes use other state, but a non-null reference simplifies
     *  the call sites). */
    float&       lateMixForMode(const juce::String& mode);
    float        lateMixForMode(const juce::String& mode) const;
    // Layer mode: two-thumb range slider defining the B-zone [start, end]
    // along the DiT block indices. Both thumbs at extremes → full B;
    // start == end → no B (pure A); narrow range → B injected only into
    // a sub-band of layers (mid / early / late depending on position).
    // Range is 0..ditBlocks_; ditBlocks_ is refreshed from backend metadata
    // (SAO Small = 16, SA3 Small may differ) — refreshDitBlocksForCurrentModel
    // updates the slider on model change.
    float            splitLayerStart_       = 4.0f;       // 0–ditBlocks_, low thumb
    float            splitLayerEnd_         = 16.0f;      // 0–ditBlocks_, high thumb (default = top: B from layer 4 onwards)
    int              ditBlocks_             = 16;         // per-model count; updated on model change

    /** Reconfigure alphaSlider (range, label, value, attachment) for the active mode. */
    void applyModeToSlider();
    bool isAudioLDM2Model(const juce::String& model) const;
    bool selectedModelIsAudioLDM2() const;
    bool isSA3Model(const juce::String& model) const;
    /** Resolve a stored/preset model id to an installed switchbox slot: exact
     *  id match, then family fallback (patternSlotFor); -1 if none installed. */
    int  slotForModel(const juce::String& model) const;

public:
    /** True when the active model is SA3 (Stable Audio 3 Small Music). Public
     *  so MainPanel can gate the SA3-only Resynth control and the 120 s duration
     *  ceiling. (Semantic Axes / Dimension Explorer now run on SA3 too.) */
    bool selectedModelIsSA3() const;

private:
    /** Per-model defaults for the steps/CFG params. The model-click handler
     *  applies these on model switch, and hasHiddenActiveState() compares
     *  against them to decide whether easy mode's params reflect a
     *  user-modified state. Keeping the mapping in one place ensures the
     *  two stay in lockstep — drift would mean the dirty indicator
     *  contradicts the values the backend actually receives. */
    struct DefaultParams { float steps; float cfg; };
    DefaultParams defaultParamsFor(const juce::String& model) const;
    void selectInjectionMode(const juce::String& mode, bool trigger);
    void syncInjectionModeAvailability();

    bool generating = false;
    bool translatingPrompts_ = false;  // Union-Jack translate in flight (blocks IPC overlap)
    std::vector<std::pair<int, float>> pendingOffsets_;  // for DimensionExplorer
    std::map<juce::String, float> pendingAxes_;          // for SemanticAxes

    using Attachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    std::unique_ptr<Attachment> alphaA, magA, noiseA, durA;
    bool easyMode_ = false;

    // Auto-regen state
    float lastGenAlpha_ = std::numeric_limits<float>::quiet_NaN();
    float lastGenNoise_ = std::numeric_limits<float>::quiet_NaN();
    float lastGenMagnitude_ = std::numeric_limits<float>::quiet_NaN();
    float lastGenLateMix_ = std::numeric_limits<float>::quiet_NaN();
    float lastGenSplitStart_ = std::numeric_limits<float>::quiet_NaN();
    float lastGenSplitEnd_ = std::numeric_limits<float>::quiet_NaN();
    std::map<juce::String, float> lastGenAxes_;
    juce::String lastGenPromptA_;
    juce::String lastGenPromptB_;
    double lastRegenTimeMs_ = 0.0; // for beat-based cooldown
    // Same beat-based cooldown for the LCO Re-Prompt cadence. A SEPARATE clock
    // from lastRegenTimeMs_ on purpose: the two paradigms never run at once
    // (easyMode_ picks one), and sharing the timestamp would make switching
    // panels either skip a slot or fire an unearned step on the first tick.
    double lastLcoStepTimeMs_ = 0.0;
    // Cadence-only failure throttle, the twin of lastRegenFailureMs_. Stamped by
    // triggerDcoReprompt's two failure tails; a manual GENERATE press never reads it.
    double lastLcoStepFailureMs_ = 0.0;

    // ── Semantic self-listening loop state (message-thread only) ──
    // Mirrors clap_llm_loop.py run_llm_loop's per-pole *_glieder chain + anti-stasis
    // `recent`. The chain interprets its OWN last link (glieder[-1]), which under the
    // ab_add (concat) coupling is NOT what the editor holds (editor = original + ","
    // + last), so the last link and the human original are tracked here separately
    // from the editor text. All touched only on the message thread (pollDriftRegen,
    // the generation-complete callbacks, and runSemanticLoopStep's callAsync tail).
    bool loopStepInFlight_ = false;   // re-entrancy guard (like `generating`)
    bool loopEngaged_ = false;        // false→true edge = capture the human originals
    bool llmAvailable_ = true;        // language model installed? gates LCO bake + Re-Prompt + Translate
    bool llmAvailableKnown_ = false;  // has setLlmAvailable run once? (first call must not early-out)
    juce::String loopOriginalA_, loopOriginalB_;  // glieder[0] (the human impulse, kept by concat) — FULL, incl. musical suffix (for restore)
    juce::String loopLastA_, loopLastB_;          // glieder[-1] (the chain's own last link) — CORE only (no musical suffix)
    juce::StringArray loopRecentA_, loopRecentB_; // glieder[-3:] anti-stasis memory (last 3 links) — CORE only
    // User-appended pitch/tempo control tokens (c3, 120bpm), split off the originals at
    // engage and re-appended on every editor write so the loop's LLM rewrites never
    // touch them. Empty when the originals carried none. (RepromptStances::*MusicSuffix.)
    juce::String loopSuffixA_, loopSuffixB_;
    // Pending loop prompts: held off the editor until the next generation fires,
    // so the displayed prompt only updates when it becomes wirksam (effective).
    juce::String pendingLoopPromptA_, pendingLoopPromptB_;
    // Deactivation restore: when a stance→Off transition is seen (timerCallback edge
    // detect), the human originals captured at engage are put back into the editors.
    // loopOriginalsValid_ gates it: true only once a step has captured originals AND
    // machine-modified the editors; cleared after the restore (and never set if the
    // loop never ran, so toggling a stance on/off with no render in between is a no-op).
    bool loopOriginalsValid_ = false;
    int  prevStanceForRestore_ = 0;               // RepromptStance::Off — last seen stance index
    // transcribe + AB-replace anti-collapse: transcribe reads only the shared machine
    // tags, so writing BOTH poles each step makes A==B (collapses the blend → alpha/
    // alpha-drift has nothing to interpolate). Instead write ONE pole per step,
    // alternating, so A and B stay distinct transcriptions of consecutive renders.
    // false = this step writes B, true = writes A; flips after each applied step.
    bool loopAltWriteA_ = false;
    // Resynth-loop release edge-detector: the previous loop regen's "a parameter
    // moved" state. The release (detach init so a changed prompt renders clean) is
    // edge-triggered on the false→true transition — so a continuous drift, which
    // holds the change flag true every tick, releases ONCE at onset and then locks
    // and evolves, instead of detaching every tick and disabling the loop. Reset
    // when the loop is not running. Message-thread only (pollDriftRegen).
    bool prevLoopParamsChanged_ = false;
    // Re-Prompt deactivation re-render: turning a stance Off restores the human
    // ORIGINAL prompt text, but the last render's audio (and, with Resynth on, the
    // init_audio carry) would otherwise keep the last machine B audible. On the
    // restore edge we (a) force the next render to detach init_audio so it renders
    // CLEAN from the restored prompt (forceCleanRenderOnce_, consumed in
    // buildInferenceRequest), and (b) actually fire that render once the pipe is
    // free (pendingOriginalReRender_, retried each tick in timerCallback). Both are
    // cleared together when the render's request is built. Message-thread only.
    bool forceCleanRenderOnce_ = false;
    bool pendingOriginalReRender_ = false;
    // Failure throttle: when a drift-driven auto-regen fails, gate further
    // attempts for a couple of seconds so a persistently broken backend
    // (e.g. a model the bundled pipeline can't load) doesn't get hammered
    // by the 10 Hz timer and oscillate the status label between
    // "auto regen..." and the error message.
    double lastRegenFailureMs_ = 0.0;
    // Last hold state handed to the processor. The 10 Hz poll would otherwise
    // re-store the same atomic forever; at idle that is pure wasted work.
    bool lastDriftGenHoldSent_ = false;
    // Armed when a render fires while a take records; cleared the moment the
    // freeze breaks, so only a render that was frozen end to end earns its step.
    bool takeStepArmed_ = false;
    float alphaGhostValue_ = std::numeric_limits<float>::quiet_NaN();
    // Mode-specific ghosts: set when alpha-LFO offset is non-zero AND the
    // active mode targets the corresponding parameter. Painted via the same
    // drawGhost lambda in paintOverChildren.
    float lateMixGhostValue_    = std::numeric_limits<float>::quiet_NaN();
    float splitStartGhostValue_ = std::numeric_limits<float>::quiet_NaN();
    float splitEndGhostValue_   = std::numeric_limits<float>::quiet_NaN();

    // ── Re-Prompt controls (semantic self-listening loop) ──
    // Sit directly under the prompts (above the params divider), co-located with
    // the loop logic this panel already owns (runSemanticLoopStep / pollDriftRegen).
    // A custom glyph "symbol slider" picks the interpreter STANCE (off + 6 movement
    // stances) and binds itself to repromptStance internally; a compact vertical
    // 3-way switchbox picks the COUPLING (B only / AB add / AB replace). Both are
    // message-thread-only meta-controls (read in pollDriftRegen, not in processBlock).
    // repromptCouplingA (the ComboBoxAttachment) is declared AFTER its hidden combo
    // so it tears down first (reverse-destruction order).
    // repromptModuleBox is the framed card (accent "RE-PROMPT" top-header) the
    // stance bar + coupling sit inside — it IS one control module, so it gets a
    // frame, like Duration/Variation. Decorative; sits behind them (toBack in ctor).
    ModuleBox repromptModuleBox;
    RepromptStanceBar repromptStanceBar;
    static constexpr int kNumCouplingBtns = RepromptCoupling::kCount;
    juce::TextButton repromptCouplingBtns[kNumCouplingBtns];
    juce::ComboBox repromptCouplingHidden;   // hidden; drives the visible radio buttons
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> repromptCouplingA;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PromptPanel)
};
