#include "PromptPanel.h"
#include "DimensionExplorer.h"
#include "GuiHelpers.h"
#include "MidiLearnMenu.h"
#include "../PluginProcessor.h"
#include "../dsp/BlockParams.h"
#include "../inference/RepromptStances.h"
#include "../dsp/CsoundEngine.h"   // renderBareOscillator (the Re-Prompt ear's probe)
#include <thread>
#include <cmath>
#include <cstring>

// LCO self-check master switch — DEACTIVATED / DEPRECATED (BJ 2026-07-21,
// "self check ist eine katastrophe"). 0 = the self-listen / compare / correct
// loop in triggerDcoBake() is compiled out; a bake authors the orchestra once
// and plays it. The loop and every helper it drives are retained (each marked
// DEPRECATED), not deleted, so flipping this to 1 resurrects them unchanged.
// Full rationale at triggerDcoBake()'s #if T5YNTH_LCO_SELFCHECK.
#define T5YNTH_LCO_SELFCHECK 0

namespace
{
GenerationEventLogEntry buildEventLogGenerationEntry(const PipeInference::Request& req,
                                                     const PipeInference::Result& result)
{
    GenerationEventLogEntry e;
    e.success          = result.success;
    e.generationTimeMs = result.generationTimeMs;
    e.realizedSeed     = result.seed;

    e.promptA = req.promptA;
    e.promptB = req.promptB;
    e.alpha           = req.alpha;
    e.magnitude       = req.magnitude;
    e.noiseSigma      = req.noiseSigma;
    e.durationSeconds = req.durationSeconds;
    e.startPosition   = req.startPosition;
    e.steps           = req.steps;
    e.cfgScale        = req.cfgScale;
    e.device          = req.device;
    e.model           = req.model;
    e.trackType       = req.trackType;
    e.modalityEpoch   = req.modalityEpoch;

    e.dimensionOffsets = req.dimensionOffsets;
    e.semanticAxes     = req.semanticAxes;
    e.axesAmount       = req.axesAmount;

    e.injectionMode         = req.injectionMode;
    e.injectionTransitionAt = req.injectionTransitionAt;
    e.latePhaseAlpha        = req.latePhaseAlpha;
    e.splitStart            = req.splitStart;
    e.splitEnd              = req.splitEnd;

    e.hadInitAudio        = req.initAudio.getNumSamples() > 0;
    e.initAudioSampleRate = req.initAudioSampleRate;
    e.initNoiseLevel      = req.initNoiseLevel;
    return e;
}

constexpr float kPromptPadFactor   = 0.04f;
constexpr float kPromptMultiInput  = 3.7f;   // two-line prompt editor (roomy box)
constexpr float kPromptCompactRow  = 1.15f;
// Scale-mark strip under the Duration bar: hairline + the second number.
// Reserved on EVERY model, not only on SA3, so selecting a model never moves
// the two rows under it - the marks themselves come and go with the range.
constexpr float kPromptTicks       = 0.75f;
// Clearance under the numbers. The ordinary row gap is not enough here: the
// numbers sit at the BOTTOM of the strip, so they end where the next bar
// begins and the two read as one block.
constexpr float kPromptTicksGap    = 0.45f;
constexpr float kPromptCompactCtrl = 0.9f;
constexpr float kPromptSeedCtrl    = 1.75f;
constexpr float kPromptGap         = 0.28f;
// Delineation spacing (clear visual separation between function blocks):
constexpr float kPromptModeBar     = 1.3f;   // mode-bar height (a real control band, not a strip)
constexpr float kPromptInnerGap    = 0.6f;   // breathing room around the mode bar, inside the A↔B block
constexpr float kPromptModelGap    = 0.6f;   // below the model selector
constexpr float kPromptGroupGap    = 1.0f;   // around the divider between the A↔B block and the params
constexpr float kPromptReprompt    = 4.0f;   // Re-Prompt MODULE total height (card + accent header + content: stance glyph bar | 3-stacked coupling)
// The prompting area is an A↔B block: [A editor / mode-bar / B editor] in a left
// column with a full-height vertical blend slider on the right. The block is
// framed by breathing room and a recessed band behind the mode bar; a divider
// separates it from the generation params below.
//
// ONE shared height/font budget for BOTH modes (Easy and Advanced/DCO). This
// replaces a former two-constant pair that getPreferredHeightForWidth and
// resized() each branched on by easyMode_ — keeping two numbers in lockstep
// by hand was a standing bug risk, and toggling the mode used to resize the
// section and rescale the font. kPromptContentUnits MUST equal the unit sum
// in getPreferredHeightForWidth so that resized()'s
// f = (height-2)/kPromptContentUnits resolves back to the preferred font.
//
// The sum is the EASY layout's own (model row + A/B block + Re-Prompt row +
// divider + the 2x2 gen-param block); Advanced no longer gets a distinct,
// smaller budget of its own:
//   modelRow(compactRow) + modelGap                                    = 1.15 + 0.6  = 1.75
//   abBlock (2*multiInput + 2*innerGap + modeBar = 7.4+1.2+1.3=9.9) + innerGap = 9.9 + 0.6 -> 12.25
//   + repromptRow                                                      = 4.0          -> 16.25
//   + groupGap (divider)                                               = 1.0          -> 17.25
//   + 2x(compactRow + gap)  (Duration|Variation, Magnitude|Chaos)      = 2*1.43 = 2.86 -> 20.11
//   + durationTicks strip (under the Duration bar, both models)         = 0.75          -> 20.86
//   + clearance under the tick numbers                                  = 0.45          -> 21.31
// Advanced (the DCO panel: one prompt editor + a BAKE/status row) needs LESS
// minimum height than this sum, so it is a valid minimum for Advanced too —
// resized()'s Advanced branch lays the BAKE/status row out at a fixed
// compactRow height and gives the DCO prompt editor 100% of whatever height
// remains, so it simply absorbs the slack instead of needing its own budget.
constexpr float kPromptContentUnits = 21.31f;
constexpr int kBaseSeed = 123456789;

float preferredPromptFontForWidth(int width)
{
    const float innerW = juce::jmax(160.0f, static_cast<float>(width) * (1.0f - 2.0f * kPromptPadFactor));
    return juce::jlimit(12.5f, 17.0f, innerW * 0.05f);
}

}

// Colors from GuiHelpers.h (kAccent, kDim, kDim, kSurface)

// Linear crossfade between old and new audio buffers.
// Blends the first xfadeSamples of newBuf with corresponding samples from oldBuf.
// Linear (not equal-power) because this is applied iteratively during drift regens —
// equal-power has gain > 1 (peak √2) which compounds with normalize into degeneration.
static void applyDriftCrossfade(juce::AudioBuffer<float>& newBuf,
                                 const juce::AudioBuffer<float>& oldBuf,
                                 int xfadeSamples)
{
    int len = std::min(xfadeSamples, std::min(newBuf.getNumSamples(), oldBuf.getNumSamples()));
    if (len <= 0) return;
    int channels = std::min(newBuf.getNumChannels(), oldBuf.getNumChannels());
    for (int ch = 0; ch < channels; ++ch)
    {
        float* dst = newBuf.getWritePointer(ch);
        const float* src = oldBuf.getReadPointer(ch);
        for (int i = 0; i < len; ++i)
        {
            float t = static_cast<float>(i) / static_cast<float>(len);
            dst[i] = src[i] * (1.0f - t) + dst[i] * t;
        }
    }
}

constexpr float kResynthLoopFloor          = 0.05f;  // keep effective resynth > attach gate (0.01)

static void makeSlider(juce::Slider& s, juce::Component* p)
{
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    s.setColour(juce::Slider::trackColourId, kOscCol);
    s.setColour(juce::Slider::backgroundColourId, kBorder);   // visible rail (kSurface was too dark on kBg)
    p->addAndMakeVisible(s);
}

static void makeLabel(juce::Label& l, const juce::String& text, juce::Colour col,
                      juce::Justification just, juce::Component* p)
{
    l.setText(text, juce::dontSendNotification);
    labelAsCaption(l, col);
    l.setJustificationType(just);
    p->addAndMakeVisible(l);
}

PromptPanel::PromptPanel(T5ynthProcessor& processor)
    : processorRef(processor)
{
    // Impulse A — periwinkle identity. The old "Impulse A" label is gone; the
    // editor's text colour plus an empty-state placeholder now carry the role.
    // Two-line with word-wrap so longer impulses stay visible. With
    // setReturnKeyStartsNewLine(false) Return still triggers generation; the
    // wrap only kicks in when the text itself exceeds one line's width.
    promptAEditor.setMultiLine(true, true);
    promptAEditor.setReturnKeyStartsNewLine(false);
    promptAEditor.setColour(juce::TextEditor::backgroundColourId, kSurface.brighter(0.08f));
    promptAEditor.setColour(juce::TextEditor::textColourId, kImpulseAText);
    promptAEditor.setColour(juce::TextEditor::outlineColourId, kBorder);
    promptAEditor.setColour(juce::TextEditor::focusedOutlineColourId, kImpulseA);
    promptAEditor.setColour(juce::TextEditor::highlightColourId, kImpulseA.withAlpha(0.30f));
    promptAEditor.setTextToShowWhenEmpty("Insert Impulse A here", kImpulseAText.withAlpha(0.45f));
    promptAEditor.onReturnKey = [this] { triggerGeneration(); };
    promptAEditor.onTextChange = [this] {
        // Impulse edits should force the next drift regen to use a fresh snapshot.
        lastGenPromptA_.clear();
        pendingLoopPromptA_.clear();  // user override: discard any staged loop prompt
        // Record human authorship of pole A into the durable human-prompt store so
        // preset save persists THIS, not a later loop rewrite. Fires only on human
        // typing + translation (loop/restore/load writes use dontSendNotification),
        // so the loop can never poison it. Per-pole: an A edit must not recapture B.
        processorRef.setHumanPromptA(promptAEditor.getText().trim());
    };
    promptAEditor.setBufferedToImage(true);
    addAndMakeVisible(promptAEditor);

    // Impulse B — yellow identity (complementary contrast to A).
    promptBEditor.setMultiLine(true, true);
    promptBEditor.setReturnKeyStartsNewLine(false);
    promptBEditor.setColour(juce::TextEditor::backgroundColourId, kSurface.brighter(0.08f));
    promptBEditor.setColour(juce::TextEditor::textColourId, kImpulseB);
    promptBEditor.setColour(juce::TextEditor::outlineColourId, kBorder);
    promptBEditor.setColour(juce::TextEditor::focusedOutlineColourId, kImpulseB);
    promptBEditor.setColour(juce::TextEditor::highlightColourId, kImpulseB.withAlpha(0.30f));
    promptBEditor.setTextToShowWhenEmpty("Insert Impulse B here", kImpulseB.withAlpha(0.45f));
    promptBEditor.onTextChange = [this] {
        // Impulse edits should force the next drift regen to use a fresh snapshot.
        lastGenPromptB_.clear();
        pendingLoopPromptB_.clear();  // user override: discard any staged loop prompt
        // Record human authorship of pole B (see promptAEditor.onTextChange).
        processorRef.setHumanPromptB(promptBEditor.getText().trim());
    };
    promptBEditor.setBufferedToImage(true);
    addAndMakeVisible(promptBEditor);

    // A↔B blend — vertical slider with an A→B gradient track and a position-
    // coloured thumb (custom LnF). Replaces the old horizontal alpha slider; the
    // gradient is self-describing, so alphaLabel/alphaValue stay hidden but are
    // still updated by onValueChange (they feed no layout).
    alphaSlider.setSliderStyle(juce::Slider::LinearVertical);
    alphaSlider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    alphaSlider.setLookAndFeel(&alphaLnF);
    addAndMakeVisible(alphaSlider);
    makeLabel(alphaLabel, "A " + juce::String(juce::CharPointer_UTF8("\xe2\x86\x94")) + " B", kDim, juce::Justification::centredLeft, this);
    makeLabel(alphaValue, "0", kOscCol, juce::Justification::centredRight, this);
    alphaLabel.setVisible(false);
    alphaValue.setVisible(false);
    alphaSlider.onValueChange = [this] {
        if (injectionMode_ == "linear")
        {
            float v = static_cast<float>(alphaSlider.getValue());
            if (std::abs(v) < 0.001f)
                alphaValue.setText("0", juce::dontSendNotification);
            else if (v < 0.0f)
                alphaValue.setText("A " + juce::String(-v, 3), juce::dontSendNotification);
            else
                alphaValue.setText("B " + juce::String(v, 3), juce::dontSendNotification);
        }
        else if (injectionMode_ == "late_step"
              || injectionMode_ == "kombi1"
              || injectionMode_ == "kombi2"
              || injectionMode_ == "kombi3")
        {
            float v = static_cast<float>(alphaSlider.getValue());
            lateMixForMode(injectionMode_) = v;
            alphaValue.setText(juce::String(v, 2), juce::dontSendNotification);
        }
        else  // layer_split — TwoValueVertical: read both thumbs
        {
            splitLayerStart_ = static_cast<float>(alphaSlider.getMinValue());
            splitLayerEnd_   = static_cast<float>(alphaSlider.getMaxValue());
            int s = static_cast<int>(std::round(splitLayerStart_));
            int e = static_cast<int>(std::round(splitLayerEnd_));
            // Denominator is the active model's DiT depth — was hardcoded "/16"
            // (SAO Small assumption) and confused the readout on SA3 Small.
            alphaValue.setText(juce::String(s) + "-" + juce::String(e)
                                   + "/" + juce::String(ditBlocks_),
                               juce::dontSendNotification);
        }
    };

    // ── Injection-mode test row (TEMPORARY, research; not persisted) ──
    // Three radio-group buttons; the existing alphaSlider's range/label/state
    // shifts with the active mode (see applyModeToSlider()).
    auto styleModeBtn = [this](juce::TextButton& b)
    {
        styleSwitchButton(b, kOscCol);
        b.setClickingTogglesState(true);
        b.setRadioGroupId(2027);  // unique id, distinct from model switchbox (1004)
        addAndMakeVisible(b);
    };
    styleModeBtn(injModeLinear);
    styleModeBtn(injModeFine);
    styleModeBtn(injModeLayer);
    styleModeBtn(injModeKombi1);
    styleModeBtn(injModeKombi2);
    styleModeBtn(injModeKombi3);
    injModeLinear.setConnectedEdges(juce::Button::ConnectedOnRight);
    injModeFine  .setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    injModeLayer .setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    injModeKombi1.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    injModeKombi2.setConnectedEdges(juce::Button::ConnectedOnLeft | juce::Button::ConnectedOnRight);
    injModeKombi3.setConnectedEdges(juce::Button::ConnectedOnLeft);
    injModeLinear.setToggleState(true, juce::dontSendNotification);
    // Mode-button onClick handlers also trigger an immediate regeneration so
    // the user can A/B modes by clicking — same UX affordance as drift /
    // slider auto-regen, but for the discrete mode dimension.
    injModeLinear.onClick = [this] { if (injModeLinear.getToggleState()) selectInjectionMode("linear", true); };
    injModeFine  .onClick = [this] { if (injModeFine  .getToggleState()) selectInjectionMode("late_step", true); };
    injModeLayer .onClick = [this] { if (injModeLayer .getToggleState()) selectInjectionMode("layer_split", true); };
    injModeKombi1.onClick = [this] { if (injModeKombi1.getToggleState()) selectInjectionMode("kombi1", true); };
    injModeKombi2.onClick = [this] { if (injModeKombi2.getToggleState()) selectInjectionMode("kombi2", true); };
    injModeKombi3.onClick = [this] { if (injModeKombi3.getToggleState()) selectInjectionMode("kombi3", true); };

    // --- Compact params ---
    // Duration — Easy view only, house-standard inline-bar SliderRow (mirrors
    // MainPanel's RESYNTH row): accent-band label + fill bar in kOscCol, with
    // the "Ns" read-out as the inline value. Attached to PID::genDuration
    // alongside the other Attachments below.
    durationRow = std::make_unique<SliderRow>(
        "DURATION",
        [](double v) { return juce::String(v, 2) + "s"; },
        kOscCol);
    durationRow->setInlineLabel(true);
    addAndMakeVisible(*durationRow);

    // The four cache-depth thresholds. 16 slots fit up to 12s, 8 up to 24s,
    // 4 up to 48s, 2 up to 96s (192 slot-seconds, see
    // T5ynthProcessor::maxInferenceCacheCapacityForDuration) - so these marks
    // are where the cache ladder steps down, and each sits on a whole-second
    // detent snapGenerationDuration already provides. Above 11s they only
    // exist on SA3, and ScaleTicks drops them by itself on the short-sound
    // engines because the range ends below the first one.
    durationTicks = std::make_unique<ScaleTicks>(durationRow->getSlider());
    durationTicks->setValues({ 12.0, 24.0, 48.0, 96.0 });
    addAndMakeVisible(*durationTicks);

    // Magnitude — Easy view only, house-standard inline-bar SliderRow (mirrors
    // durationRow above). Moved out of Advanced entirely. Attached to
    // PID::genMagnitude alongside the other Attachments below.
    magRow = std::make_unique<SliderRow>(
        "MAGNITUDE",
        [](double v) { return juce::String(v, 3); },
        kOscCol);
    magRow->setInlineLabel(true);
    addAndMakeVisible(*magRow);

    // Chaos — Easy view only, house-standard inline-bar SliderRow (mirrors
    // durationRow above). Moved out of Advanced entirely. Attached to
    // PID::genNoise alongside the other Attachments below.
    noiseRow = std::make_unique<SliderRow>(
        "CHAOS",
        [](double v) { return juce::String(v, 3); },
        kOscCol);
    noiseRow->setInlineLabel(true);
    addAndMakeVisible(*noiseRow);

    // Union-Jack translate: a MOMENTARY action. Clicking it rewrites the A/B
    // prompts to English in place. Because the single IPC pipe is shared with
    // auto-regen, the click pauses auto-regen for the duration of the translation
    // (freeing the pipe); auto-regen resumes automatically, with its unchanged bar
    // setting, once the translation finishes. The flag pulses while it runs.
    translateToggle.setTooltip("Translate prompts to English in place "
                               "(auto-regen pauses during translation, then resumes)");
    translateToggle.onClick = [this] { translatePromptsInPlace(); };
    addAndMakeVisible(translateToggle);

    {
        static constexpr const char* labels[kNumSeedModeBtns] = { "none", "last", "auto" };
        // Icon + tooltip per seed mode: none → Ban (fixed base seed),
        // last → Lock (reuse previous seed), auto → Shuffle (new random seed).
        // The text is kept as the accessible name; the LnF draws the glyph.
        static constexpr Icon seedIcons[kNumSeedModeBtns] = { Icon::Ban, Icon::Lock, Icon::Shuffle };
        static constexpr const char* seedTips[kNumSeedModeBtns] = {
            "none: fixed base seed (no variation)",
            "last: reuse the previous seed (double-click to type a specific seed)",
            "auto: new random seed each generation"
        };
        for (int i = 0; i < kNumSeedModeBtns; ++i)
        {
            auto& b = seedModeBtns[i];
            b.setButtonText(labels[i]);
            b.setLookAndFeel(&seedBtnLnF);
            b.getProperties().set("iconId", static_cast<int>(seedIcons[i]));
            b.setTooltip(seedTips[i]);
            styleSwitchButton(b, kOscCol);
            b.setClickingTogglesState(true);
            b.setRadioGroupId(2038);
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumSeedModeBtns - 1) edges |= juce::Button::ConnectedOnRight;
            b.setConnectedEdges(edges);
            b.onClick = [this, i]
            {
                if (!seedModeBtns[i].getToggleState())
                    return;
                setSeedMode(static_cast<SeedMode>(i), true);
            };
            addAndMakeVisible(b);
        }
        syncSeedModeButtons();

        // Double-click on the Lock ("last"/steady) button opens a modal to type
        // an exact seed (see mouseDoubleClick/openSeedEntryDialog) — the Easy-mode
        // way to set a specific seed (the Advanced seed-entry field was removed
        // in DCO Slice 0). The co-firing single click just selects steady mode,
        // which is fine/desired.
        seedModeBtns[static_cast<int>(SeedMode::steady)].addMouseListener(this, false);

        // Easy-view Variation is a single switchbox row: a "VAR" caption + the
        // 3 seed-mode icons (framed by paintSwitchBoxBorder). No card — the
        // Duration inline row + this row are the two standard-height rows.
        makeLabel(varSwitchLabel, "VAR", kDim, juce::Justification::centredLeft, this);
    }

    // DCO surface — Advanced IS the DCO panel now (a completely different
    // paradigm from the neural Easy view, not a variant of it): its own
    // multiline prompt editor (panel-local text, NOT bound to Impulse A) +
    // a bake trigger + status/flags line. Authors a recipe from the DCO
    // prompt via the backend lexicon router, bakes off-thread, loads the
    // wavetable master (loadDcoWavetable).
    {
        dcoPromptEditor.setMultiLine(true, true);
        dcoPromptEditor.setReturnKeyStartsNewLine(false);
        // Styled EXACTLY like promptAEditor (the neural Impulse A editor) — the
        // LCO panel reuses the T5osc editor identity verbatim, not a variant.
        dcoPromptEditor.setColour(juce::TextEditor::backgroundColourId, kSurface.brighter(0.08f));
        dcoPromptEditor.setColour(juce::TextEditor::textColourId, kImpulseAText);
        dcoPromptEditor.setColour(juce::TextEditor::outlineColourId, kBorder);
        dcoPromptEditor.setColour(juce::TextEditor::focusedOutlineColourId, kImpulseA);
        dcoPromptEditor.setColour(juce::TextEditor::highlightColourId, kImpulseA.withAlpha(0.30f));
        dcoPromptEditor.setTextToShowWhenEmpty("Describe the oscillator to craft", kImpulseAText.withAlpha(0.45f));
        dcoPromptEditor.onReturnKey = [this] { triggerLcoGenerate(); };
        // No onTextChange mirror to the processor: the DCO prompt is
        // panel-local for now (preset persistence is a documented open seam).
        dcoPromptEditor.setBufferedToImage(true);
        addAndMakeVisible(dcoPromptEditor);

        // The LCO title now lives in the panel header (MainPanel::setOscEasyMode /
        // the header layout), so there is no separate subtitle line here any more.

        // LCO model strip — a single tab surfacing the LCO's author LLM, which is
        // also the app's only LLM, styled EXACTLY like a T5osc model button
        // (modelBtns[], below): same LnF, same styleSwitchButton accent. Display-
        // only for now (selection is future work); the tab lights when the model
        // is installed and dims when absent (updateLcoModelTabs, driven by
        // MainPanel). Replaces the old single "LRO: ready" status line and the
        // earlier single collapsed model button.
        {
            // The tab names the model, never a guess: MainPanel pushes the
            // resolver-mirror name at open and on live install/removal
            // (setLcoResolvedModel), and each authored orchestra brings the
            // backend's own claim (setLcoAuthorModel), which wins while it
            // stands. Until either speaks the placeholder states plainly that no
            // model is there — never a pseudo model name (a compiled-in
            // "Coder 7B" once stayed on screen while a different model wrote
            // every orchestra, and "LCO author" read like a model called that).
            for (int i = 0; i < kNumLcoModelSlots; ++i)
            {
                dcoModelBtns[i].setButtonText(kLcoNoModelLabel);
                dcoModelBtns[i].setLookAndFeel(&modelSwitchLnF);
                styleSwitchButton(dcoModelBtns[i], kOscCol);
                dcoModelBtns[i].setClickingTogglesState(false);
                dcoModelBtns[i].setConnectedEdges(0);   // single slot: no connected edges
                dcoModelBtns[i].setEnabled(false);      // display-only for now (selection is future work)
                // Hover stays live (clicks still do nothing on the disabled
                // button) so the tooltip — full directory name + whether it
                // wrote the current orchestra or will write the next — can show.
                dcoModelBtns[i].setInterceptsMouseClicks(true, false);
                addAndMakeVisible(dcoModelBtns[i]);
            }
            updateLcoModelTabs();
        }

        // No BAKE button: the reused GENERATE button (MainPanel) authors the bake
        // in LCO mode via triggerLcoGenerate(). dcoStatusLabel stays as the
        // logical status/error holder (many call sites write it) but is no
        // longer laid out as its own visible line — its text is routed into
        // dcoTraceView below (setLcoStatus).
        makeLabel(dcoStatusLabel, "LRO: ready", kDim, juce::Justification::centredLeft, this);

        // Flags list — the guardrail honesty channel made visible: one
        // "word: reason" line per approximated/unmappable prompt term
        // (previously tooltip-only on the status label). Empty when clean;
        // the layout collapses its row then.
        makeLabel(dcoFlagsLabel, "", kWarning, juce::Justification::topLeft, this);

        // The authoring trace: fills the middle where the baked wave used to sit
        // (the wave now draws in the engine window) — the disclosure the LCO is
        // FOR (docs/DCO_REPROMPT_CONCEPT.md): what the machine heard, made
        // visible and negotiable, not a picture of the table. It also doubles as
        // the LCO status/error surface (setLcoStatus) until a trace exists. The
        // dual A+B/harmonic-inharmonic split this used to carry (a second, twin
        // editor) is retired — BJ 2026-07-17: "this split is dead".
        dcoTraceView.setPlaceholder("Describe a sound and press Generate. What the machine "
                                    "makes of it appears here, step by step; hold the panel "
                                    "to see the Csound it wrote.");
        addAndMakeVisible(dcoTraceView);

        // DCO Re-Prompt (stance-driven self-reading loop, docs/DCO_REPROMPT_CONCEPT.md):
        // a SECOND stance bar bound to its OWN parameter (dcoRepromptStance) — never
        // shares repromptStance with the neural loop (paradigm isolation, BlockParams.h).
        // Reuses the SAME RepromptStance::kEntries as the neural bar (the glyphs are
        // index-hardwired to that order; a curated DCO-specific stance set is a
        // documented follow-up, not this slice). Uses `processor` (the ctor parameter)
        // directly, not the `apvts` alias below — this block runs before that alias
        // is declared.
        if (auto* dcoStanceParam = processor.getValueTreeState().getParameter(PID::dcoRepromptStance))
            dcoStanceBar.attachTo(*dcoStanceParam, RepromptStance::kCount);
        dcoStanceBar.setTooltip(
            "LRO Re-Prompt stance: the machine listens to the oscillator it just "
            "built and rewrites the LRO prompt from what it heard, before the next "
            "one is crafted. Hover a glyph for its movement type.");
        dcoStanceBar.setPositionTooltips({
            "Off - Re-Prompt loop disabled.",
            // "hears", like the neural list: since 2026-07-28 this stance works on a
            // CLAP description of the rendered oscillator, not on the author's own
            // account of its code (docs/DCO_REPROMPT_CONCEPT.md, Nachtrag 2026-07-28).
            "Transcribe (fixed point): the machine re-describes what it hears; the prompt stays put.",
            "Sober (inward spiral): re-states the sound plainly and factually - sentiment and scene stripped, the real source kept.",
            "Sweeten (damped settling): softens toward a gentler, cuter reading.",
            "Variation (bounded cluster): small variations around the current theme.",
            "Abduction (wandering): leaps to new scenes, drifting far from the source.",
            "Opposite (limit cycle): oscillates between opposing readings."
        });
        addAndMakeVisible(dcoStanceBar);

        // "RE-PROMPT" framed ModuleBox — the same card template as the neural
        // repromptModuleBox (accent top-header, no icon), holding just the DCO
        // stance bar (no coupling column: the DCO chain has no A/B poles). Sits
        // directly above the reused GENERATE button, which drives the loop:
        // stance Off -> bake, stance engaged -> one re-prompt step.
        dcoRepromptBox.configure("RE-PROMPT", kOscCol, Icon::numIcons);
        addAndMakeVisible(dcoRepromptBox);
        dcoRepromptBox.toBack();

        // BJ 2026-07-29: "das muss ein Schalter im Osc sein. User entscheidet
        // hier ob der Synth die Parameter ändern darf oder ob sie user-exklusiv
        // bleiben." Off by default — the knobs are the player's until the player
        // hands them over.
        styleSwitchButton(dcoSetsParamsBtn, kOscCol);
        dcoSetsParamsBtn.setClickingTogglesState(true);
        dcoSetsParamsBtn.setTooltip("Let an authored instrument set the synth's own controls too: "
                                    "filter, envelopes, LFOs, drift, aftertouch. It prefers targets "
                                    "nothing is using. Off: they stay yours - the instrument still "
                                    "writes its settings, and they go on the patch the moment you "
                                    "switch this on.");
        addAndMakeVisible(dcoSetsParamsBtn);
        dcoSetsParamsBtnA = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
            processorRef.getValueTreeState(), PID::lcoSetsParams, dcoSetsParamsBtn);
        // No onClick here: "Off: they stay yours" is kept on the PROCESSOR
        // (parameterChanged -> handleAsyncUpdate), so it also holds for host
        // automation, for a MIDI-learned controller, and while this window is
        // closed.
    }

    // Model selector — fixed 4 slots, always visible (disabled = gray until model found).
    // Order: SA3 first (newest, default), then SA1 family, then AudioLDM2.
    {
        // Compact one-token labels (no space between family and variant) to save
        // horizontal room. "SA3mus" is clipped to 6 chars (matching "SA3sfx") so the
        // tier s/m badge has clean right-margin room on both SA3 slots.
        const char* slotLabels[kNumModelSlots] = { "SA3mus", "SA3sfx", "SA1open", "SA1small", "AudioLDM2" };
        for (int i = 0; i < kNumModelSlots; ++i)
        {
            modelBtns[i].setButtonText(slotLabels[i]);
            modelBtns[i].setLookAndFeel(&modelSwitchLnF);  // draws the s/m tier cell (tierLetter)
            styleSwitchButton(modelBtns[i], kOscCol);
            modelBtns[i].setClickingTogglesState(true);
            modelBtns[i].setRadioGroupId(1004);
            int edges = 0;
            if (i > 0) edges |= juce::Button::ConnectedOnLeft;
            if (i < kNumModelSlots - 1) edges |= juce::Button::ConnectedOnRight;
            modelBtns[i].setConnectedEdges(edges);
            modelBtns[i].setEnabled(false);  // gray until Python reports availability
            modelBtns[i].setAlpha(0.3f);
            modelBtns[i].onClick = [this, i]()
            {
                if (!modelBtns[i].getToggleState()) return;
                auto model = modelSlotIds[i];
                if (model.isEmpty()) return;

                // Second click on the already-active SA3 slot toggles the per-machine
                // tier (small <-> medium) rather than re-selecting. The tier has no
                // dedicated widget — just the "s"/"m" cell ModelSwitchLnF draws on these
                // two slots (the "tierLetter" property). Only when both tiers are
                // installed (otherwise the tier is forced and there is nothing to toggle).
                if (i == activeModelSlot_ && (i == 0 || i == 1) && sa3TierChoiceAvailable_)
                {
                    setSa3Tier(sa3Tier_ == "medium" ? "small" : "medium", true);
                    return;
                }
                activeModelSlot_ = i;

                // Selecting a model has two distinct kinds of consequence, and
                // they must NOT share a guard. The JUCE radio toggle that picked
                // this button has already fired (it is ungated), so the cheap,
                // purely-local consequences — default steps/cfg and the SA3 gate
                // + injection-mode availability — must apply unconditionally too,
                // or the UI desyncs from the selection (e.g. the Semantic Axes /
                // Dimension Explorer cards stay dimmed after an SA3 -> non-SA3
                // switch while drift auto-regen keeps `generating` true).
                // Anything that reaches into the inference backend is deferred
                // while generating (the guard below).
                auto& apvts = processorRef.getValueTreeState();
                const auto defaults = defaultParamsFor(model);
                apvts.getParameter(PID::infSteps)->setValueNotifyingHost(
                    apvts.getParameter(PID::infSteps)->convertTo0to1(defaults.steps));
                apvts.getParameter(PID::genCfg)->setValueNotifyingHost(
                    apvts.getParameter(PID::genCfg)->convertTo0to1(defaults.cfg));
                syncInjectionModeAvailability();

                // The Duration ceiling (SA3 -> 120s, else 11s) is pure UI: it
                // reads the selected-model toggle and re-scopes the slider range
                // without touching the inference backend, so it must apply
                // unconditionally too. Otherwise switching to SA3 while drift
                // auto-regen holds `generating` true leaves the slider stuck at
                // the 11s ceiling. (The DiT-depth re-scope below DOES call
                // getModelMetadata() on the PipeInference mutex and stays
                // deferred under the guard; this duration call is a no-op repeat
                // when refreshDitBlocksForCurrentModel() runs below.)
                applyDurationRangeForCurrentModel();

                // Everything below reaches into the inference backend, so it is
                // deferred while a generation is in flight. refreshDitBlocks-
                // ForCurrentModel() calls getModelMetadata(), which contends on
                // the same PipeInference mutex that generate() holds for the
                // entire blocking IPC round-trip — calling it mid-render would
                // freeze the message thread. The preload likewise drives the IPC
                // pipe. The next generation loads the selected model on demand;
                // the DiT/duration ranges re-scope on the next idle model touch.
                if (generating) return;

                // The active model can have a different DiT depth (SA3 vs SAO
                // Small): clamp the layer slider range before the user has a
                // chance to drag past the new ceiling.
                refreshDitBlocksForCurrentModel();

                // Preload model in background so first generate is instant
                if (onStatusChanged) onStatusChanged("Loading " + model + "...", true);
                generateButton.setEnabled(false);

                auto pipePtr = processorRef.getPipeInferencePtr();
                juce::String device = defaultInferenceDevice_;
                juce::Component::SafePointer<PromptPanel> safeThis(this);
                std::thread([safeThis, pipePtr, model, device]()
                {
                    bool ok = pipePtr->preload(model, device);
                    juce::MessageManager::callAsync([safeThis, ok, model]()
                    {
                        if (auto* self = safeThis.getComponent())
                        {
                            if (!self->generating)
                                self->generateButton.setEnabled(true);
                            if (self->onStatusChanged)
                                self->onStatusChanged(ok ? model + " ready" : model + " load failed", false);
                        }
                    });
                }).detach();
            };
            addAndMakeVisible(modelBtns[i]);
        }
    }

    // (SA3 tier has no dedicated widget — it's the s/m badge on the SA3 slots,
    // toggled by a second click on the active SA3 slot. See modelBtns onClick.)

    // Generate button is now in MainPanel — keep internal for triggerGeneration()
    generateButton.setVisible(false);

    // APVTS
    auto& apvts = processor.getValueTreeState();
    alphaA  = std::make_unique<Attachment>(apvts, PID::genAlpha, alphaSlider);
    magA    = std::make_unique<Attachment>(apvts, PID::genMagnitude, magRow->getSlider());
    noiseA  = std::make_unique<Attachment>(apvts, PID::genNoise, noiseRow->getSlider());
    durA    = std::make_unique<Attachment>(apvts, PID::genDuration, durationRow->getSlider());
    // Keep the inline read-outs in sync (mirrors MainPanel's resynthRow — the
    // attachment owns onValueChange, so re-wire the display update after it).
    durationRow->getSlider().onValueChange = [this] { durationRow->updateValue(); };
    durationRow->updateValue();
    durationRow->onRightClick = [this](juce::Point<int> p) {
        showMidiLearnMenu(processorRef, PID::genDuration, p);
    };
    magRow->getSlider().onValueChange = [this] { magRow->updateValue(); };
    magRow->updateValue();
    magRow->onRightClick = [this](juce::Point<int> p) {
        showMidiLearnMenu(processorRef, PID::genMagnitude, p);
    };
    noiseRow->getSlider().onValueChange = [this] { noiseRow->updateValue(); };
    noiseRow->updateValue();
    noiseRow->onRightClick = [this](juce::Point<int> p) {
        showMidiLearnMenu(processorRef, PID::genNoise, p);
    };

    // Right-click MIDI Learn on raw sliders (not wrapped in SliderRow).
    alphaSlider.addMouseListener(this, false);
    if (auto* startParam = apvts.getParameter(PID::genStart))
        startParam->setValueNotifyingHost(0.0f);

    // The durA attachment just set the slider to the parameter's full 0.1–120s
    // range; scope it to the short-sound default until a model is selected
    // (populateModelSelector → refreshDitBlocksForCurrentModel re-applies it).
    applyDurationRangeForCurrentModel();

    // ── Re-Prompt controls (semantic self-listening loop) ──
    // Placed under the prompts (laid out in resized()), co-located with the loop
    // logic this panel owns. The stance bar paints the movement-type glyphs and
    // binds itself to repromptStance; the vertical switchbox mirrors the model
    // switchbox pattern (hidden combo + radio buttons) and binds to repromptCoupling.
    // Re-Prompt is engine-agnostic (NOT SA3-gated): the word loop runs on any model.
    // Re-Prompt is its own framed module (card + accent top-header) — the same
    // ModuleBox template as Duration/Variation/Resynth. It IS one control module,
    // so it gets a frame. The header strip carries the "RE-PROMPT" title; the
    // stance glyph bar + 3-way coupling stack live in the content area below it.
    repromptModuleBox.configure("RE-PROMPT", kOscCol, Icon::numIcons);
    addAndMakeVisible(repromptModuleBox);
    repromptModuleBox.toBack();

    if (auto* stanceParam = apvts.getParameter(PID::repromptStance))
        repromptStanceBar.attachTo(*stanceParam, RepromptStance::kCount);
    repromptStanceBar.setTooltip(
        "Re-Prompt stance: after each render the machine listens to its own output "
        "and rewrites the prompt(s) before the next one. Hover a glyph for its "
        "movement type.");
    repromptStanceBar.setPositionTooltips({
        "Off - Re-Prompt loop disabled.",
        "Transcribe (fixed point): the machine re-describes what it hears; the prompt stays put.",
        "Sober (inward spiral): re-states the sound plainly and factually - sentiment and scene stripped, the real source kept.",
        "Sweeten (damped settling): softens toward a gentler, cuter reading.",
        "Variation (bounded cluster): small variations around the current theme.",
        "Abduction (wandering): leaps to new scenes, drifting far from the source.",
        "Opposite (limit cycle): oscillates between opposing readings."
    });
    addAndMakeVisible(repromptStanceBar);

    // Coupling switchbox: visible radio buttons + a hidden ComboBox carrying the
    // ChoiceParameter attachment (so the buttons stay a pure view of the param).
    const char* couplingUi[kNumCouplingBtns] = { "B only", "AB add", "AB replace" };
    const char* couplingTip[kNumCouplingBtns] = {
        "B only: one interpret run rewrites prompt B; A stays the human anchor (the "
        "alpha slider sets the A/B mix).",
        "AB add: two runs (A & B); each pole = its own original prompt + its latest "
        "interpretation.",
        "AB replace: two runs (A & B); each pole fully replaced by its stance "
        "interpretation."
    };
    juce::StringArray couplingItems;
    for (const auto& e : RepromptCoupling::kEntries)
        couplingItems.add(juce::String(juce::CharPointer_UTF8(e.label)));
    repromptCouplingHidden.addItemList(couplingItems, 1);
    repromptCouplingHidden.onChange = [this] {
        const int id = repromptCouplingHidden.getSelectedId();
        for (int i = 0; i < kNumCouplingBtns; ++i)
            repromptCouplingBtns[i].setToggleState(i + 1 == id, juce::dontSendNotification);
    };
    addChildComponent(repromptCouplingHidden);   // hidden: only its value/attachment is used
    for (int i = 0; i < kNumCouplingBtns; ++i)
    {
        auto& bb = repromptCouplingBtns[i];
        bb.setButtonText(couplingUi[i]);
        // The block sits in the RE-PROMPT box's own colour: unselected = the same
        // periwinkle as the left-header chip (kOscCol @0.7), so the whole 3-way
        // reads as part of RE-PROMPT; ONLY the selected coupling pops in the drift
        // orange. White text on both. White-on-periwinkle = 6.1:1 (AA), white-on-
        // orange = 3.8:1; the selected differs in BOTH hue and luminance (1.85x),
        // and periwinkle<->orange is the canonical CVD-safe pairing.
        bb.setColour(juce::TextButton::buttonColourId, kOscCol.withAlpha(0.7f));
        bb.setColour(juce::TextButton::buttonOnColourId, kDriftCol);
        bb.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
        bb.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
        bb.setClickingTogglesState(true);
        bb.setRadioGroupId(1006);   // unique repo-wide (1004 = model switchbox, 1005 = old MainPanel coupling)
        bb.setTooltip(couplingTip[i]);
        bb.onClick = [this, i] { repromptCouplingHidden.setSelectedId(i + 1); };
        addAndMakeVisible(bb);
    }
    // The attachment's sendInitialUpdate() fires onChange (sendNotificationSync),
    // syncing the button toggles to the restored value — no manual sync needed.
    repromptCouplingA = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
        apvts, PID::repromptCoupling, repromptCouplingHidden);

    startTimerHz(10);  // poll for device availability + drift regen + ghost
}

int PromptPanel::getPreferredHeightForWidth(int width) const
{
    const float f = preferredPromptFontForWidth(width);
    const int multiInputH = juce::roundToInt(f * kPromptMultiInput);
    const int gap = juce::roundToInt(f * kPromptGap);
    const int modelGap = juce::roundToInt(f * kPromptModelGap);
    const int innerGap = juce::roundToInt(f * kPromptInnerGap);
    const int groupGap = juce::roundToInt(f * kPromptGroupGap);
    const int modeBarH = juce::roundToInt(f * kPromptModeBar);
    const int compactRowH = juce::roundToInt(f * kPromptCompactRow);
    const int compactCtrlH = juce::roundToInt(f * kPromptCompactCtrl);
    const int seedCtrlH = juce::roundToInt(f * kPromptSeedCtrl);
    const int repromptRowH = juce::roundToInt(f * kPromptReprompt);

    // A↔B block: A editor / mode-bar / B editor with breathing room around the
    // mode bar (the right-hand vertical slider overlays this same height, so it
    // adds nothing to the vertical budget).
    const int abBlockH = multiInputH + innerGap + modeBarH + innerGap + multiInputH;

    // ONE budget for both modes (kPromptContentUnits, unified above) — toggling
    // EASY/ADV must never resize the section. This is the Easy layout's own
    // sum; Advanced (a pure DCO panel: one prompt editor + a BAKE/status row)
    // needs LESS minimum height than this, so it fits under the same minimum
    // with room to spare — the DCO editor absorbs the slack in resized()
    // rather than being budgeted here.
    return (compactRowH + 2) + modelGap             // model selector row
         + abBlockH + innerGap + repromptRowH       // A↔B block + Re-Prompt row
         + groupGap                                 // divider
         + 2 * (compactRowH + gap)                  // 2x2 gen block: Duration|Variation, Magnitude|Chaos
         + juce::roundToInt(f * (kPromptTicks + kPromptTicksGap));  // scale marks under Duration
}

void PromptPanel::timerCallback()
{
    if (!devicesPopulated && processorRef.isPipeInferenceReady())
    {
        populateDeviceChoice();
        populateModelSelector();
        // Don't stop timer — continue for drift regen polling + ghost updates
    }

    // The KNOBS station follows the switch, which moves from anywhere — a click,
    // host automation, a MIDI CC — and is acted on one async hop later. Polled
    // on the PROCESSOR's revision rather than on the switch itself, so the card
    // is re-read after the knobs were actually taken or given back rather than
    // on the switch's word. A flip back within one tick can still be seen with
    // the give-back done and the re-take queued, which reads as "took nothing"
    // for that tick and corrects itself on the next. Two int reads a tick while
    // a trace is on screen, nothing at all otherwise.
    if (dcoKnobsKnown_
        && (dcoKnobsRev_ != processorRef.getAuthorSettingsRevision()
            || dcoKnobsGen_ != processorRef.getAuthorSettingsGeneration()))
        refreshLcoKnobStation();

    // Ghost indicators for drift-modulated sliders — update every tick.
    auto& mv0 = processorRef.modulatedValues;
    const float newAlpha = mv0.driftAlpha.load(std::memory_order_relaxed);
    const float newMag   = mv0.driftMagnitude.load(std::memory_order_relaxed);
    const float newNoise = mv0.driftNoise.load(std::memory_order_relaxed);
    // Mode-specific ghosts for Step-in/Layer: derive the alpha-LFO offset (in
    // alpha-units) and project it onto the active mode's parameter range
    // using the same scaling factors the regen path uses.
    float newLateMix    = std::numeric_limits<float>::quiet_NaN();
    float newSplitStart = std::numeric_limits<float>::quiet_NaN();
    float newSplitEnd   = std::numeric_limits<float>::quiet_NaN();
    {
        const float baseAlpha0 = processorRef.getValueTreeState()
                                     .getRawParameterValue(PID::genAlpha)->load();
        const float alphaOff = std::isnan(newAlpha) ? 0.0f
                                                    : (newAlpha - baseAlpha0);
        const bool fineLike = (injectionMode_ == "late_step"
                            || injectionMode_ == "kombi1"
                            || injectionMode_ == "kombi2"
                            || injectionMode_ == "kombi3");
        if (fineLike && std::abs(alphaOff) > 0.001f)
        {
            newLateMix = juce::jlimit(0.0f, 1.0f, lateMixForMode(injectionMode_) + alphaOff * 0.25f);
        }
        else if (injectionMode_ == "layer_split" && std::abs(alphaOff) > 0.001f)
        {
            const float blocksF = static_cast<float>(ditBlocks_);
            const float width = splitLayerEnd_ - splitLayerStart_;
            const float maxStart = std::max(0.0f, blocksF - width);
            // Drift travels half the block range per unit of alphaOff. SAO
            // Small (16 blocks) keeps the historical 8-block sweep; SA3
            // scales proportionally.
            const float sweepScale = blocksF * 0.5f;
            float gs = juce::jlimit(0.0f, maxStart, splitLayerStart_ + alphaOff * sweepScale);
            newSplitStart = gs;
            newSplitEnd   = gs + width;
        }
    }

    auto same = [](float a, float b) {
        return (std::isnan(a) && std::isnan(b)) || a == b;
    };
    const bool alphaGroupChanged = !same(alphaGhostValue_,    newAlpha)
                                || !same(lateMixGhostValue_,  newLateMix)
                                || !same(splitStartGhostValue_, newSplitStart)
                                || !same(splitEndGhostValue_, newSplitEnd);

    alphaGhostValue_      = newAlpha;
    lateMixGhostValue_    = newLateMix;
    splitStartGhostValue_ = newSplitStart;
    splitEndGhostValue_   = newSplitEnd;

    // Easy-only: the ghost draw itself is easyMode_-gated in paintOverChildren
    // (the alpha slider is hidden behind the DCO panel in Advanced), so
    // invalidating its stale rect there would blit for no visual effect.
    if (easyMode_ && alphaGroupChanged)
        repaint(alphaSlider.getBounds().expanded(4));

    // Magnitude/Chaos ghosts are now SliderRow-owned drift indicators (the row
    // paints + smooths natively via setGhostValue/tickGhost, like MainPanel's
    // resynthRow and the FX/Synth panel rows) — no manual bounds/repaint
    // bookkeeping needed here.
    magRow->setGhostValue(newMag);
    noiseRow->setGhostValue(newNoise);
    magRow->tickGhost();
    noiseRow->tickGhost();

    // Re-Prompt deactivation → restore the human originals. Edge-detected here (not in
    // pollDriftRegen, which returns early once the loop is Off) so it fires the instant
    // the stance is set back to Off, independent of the regen mode. dontSendNotification:
    // we resync the lastGen* trackers + setLastPrompts ourselves, exactly like the loop's
    // own apply tail, so no spurious promptChanged regen is provoked.
    {
        const int curStance = static_cast<int>(processorRef.getValueTreeState()
            .getRawParameterValue(PID::repromptStance)->load());
        if (curStance == RepromptStance::Off
            && prevStanceForRestore_ != RepromptStance::Off
            && loopOriginalsValid_)
        {
            promptAEditor.setText(loopOriginalA_, juce::dontSendNotification);
            promptBEditor.setText(loopOriginalB_, juce::dontSendNotification);
            lastGenPromptA_ = loopOriginalA_;
            lastGenPromptB_ = loopOriginalB_;
            processorRef.setLastPrompts(loopOriginalA_, loopOriginalB_);
            // NOTE: do NOT touch the durable human-prompt store here — it already holds
            // these originals (loopOriginalA_/B_ were sourced from it on engage) and must
            // survive so a later save/buffer-write persists the human text even if this
            // restore edge is ever missed. setText above used dontSendNotification, so
            // onTextChange won't overwrite it with the reverted text either.
            loopOriginalsValid_ = false;
            loopEngaged_ = false;   // re-arm the original-capture edge for the next run
            pendingLoopPromptA_.clear();
            pendingLoopPromptB_.clear();
            // Make the SOUND follow the reverted text: re-render the restored original
            // once, clean (init_audio detached) so it isn't anchored to the last B.
            // Deferred — fired by the block at the end of timerCallback once the pipe
            // is free (a loop step / generation is usually still in flight right here).
            forceCleanRenderOnce_   = true;
            pendingOriginalReRender_ = true;
        }
        prevStanceForRestore_ = curStance;
    }

    // Replay transport: fires the generation the playhead just crossed. Checked
    // before auto-regen — while a tape runs, its generations own the pipe.
    pollReplayRegen();

    // Auto-regen polling. Two paradigms, one cadence control: pollDriftRegen owns
    // it in Easy, pollLcoRepromptCadence in Advanced — each early-outs on the
    // other's mode, so exactly one runs per tick.
    pollDriftRegen();
    pollLcoRepromptCadence();

    // Phase 5 Csound compile-window poll (SPEC_phase4_5_csound_llm_preset.md):
    // a cheap no-op unless triggerDcoBake() opened a window.
    pollCsoundCompile();
    pollCsoundPerformanceEnded();

    // The trace view's "something is running" pulse, held to the truth. A status
    // that arms it is written at the START of an authoring or a re-prompt, and
    // not every way out of those writes another one — a re-prompt that succeeds
    // and hands over to a bake which then early-returns leaves nothing behind to
    // clear it. Two bool reads a tick, and the alternative is a 12 Hz repaint on
    // an idle panel, which is this project's oldest bug.
    if (! dcoBaking_ && ! dcoRepromptBusy_)
        dcoTraceView.stopBusy();

    // Reprompt-off deferred clean re-render: when the stance is switched to Off the
    // restore block reverts the prompts AND sets pendingOriginalReRender_, but a loop
    // step / generation is almost always still in flight right at that moment. Fire it
    // here once the pipe is free. forceCleanRenderOnce_ detaches init_audio for this one
    // render so the SOUND follows the reverted text instead of staying anchored to the
    // last mutated B; buildInferenceRequest consumes both flags.
    //
    // The guard MUST mirror triggerGeneration's own real-generation preconditions
    // (cache-not-full + backend-ready): those paths return BEFORE buildInferenceRequest,
    // which is the only place the flags are consumed — calling into them would leave
    // pendingOriginalReRender_ stuck true and re-fire every tick (cache replay / status
    // spam at 10 Hz, and the intended clean render never happening). When a real
    // generation cannot proceed the debt simply persists (a cheap bool check, no spin)
    // and discharges the moment it can — e.g. the backend finishes connecting.
    // easyMode_ too: this is a neural render of the (in Advanced: hidden)
    // prompts — deferred until the user is back on the Easy view, same
    // debt-persists semantics as the other preconditions.
    if (pendingOriginalReRender_ && easyMode_
        && !generating && !translatingPrompts_ && !loopStepInFlight_
        && processorRef.isInferenceReady()
        && !processorRef.isInferenceCacheFull())
        triggerGeneration();
}

void PromptPanel::paint(juce::Graphics& g)
{
    // Recessed band framing the mode bar (drawn before children, so the mode
    // buttons + flag paint on top): marks the blend-mode selector as a control.
    if (!injModeSwitchBounds.isEmpty())
        paintSwitchBoxBorder(g, injModeSwitchBounds);

    // (The A↔B / params divider is drawn in paintOverChildren() — Easy view
    // only; Advanced is the DCO panel and never sets paramsDividerY, so no
    // divider paints there. See the divider block in resized() for detail.)

    if (!modelSwitchBounds.isEmpty())
        paintSwitchBoxBorder(g, modelSwitchBounds);
    if (easyMode_ && !seedModeSwitchBounds.isEmpty())
        paintSwitchBoxBorder(g, seedModeSwitchBounds);
}

void PromptPanel::paintOverChildren(juce::Graphics& g)
{
    // Divider between the A↔B / Re-Prompt block and the generation params.
    // Easy view only — Advanced (the DCO panel) sets paramsDividerY = -1 in
    // resized(), so this never paints there.
    if (paramsDividerY >= 0)
    {
        const int pad = juce::roundToInt(static_cast<float>(getWidth()) * kPromptPadFactor);
        g.setColour(kBorder);
        g.drawHorizontalLine(paramsDividerY, static_cast<float>(pad),
                             static_cast<float>(getWidth() - pad));
    }

    auto drawGhost = [&](juce::Slider& slider, float ghostVal) {
        if (std::isnan(ghostVal)) return;
        auto sb = slider.getBounds();
        double norm = slider.valueToProportionOfLength(static_cast<double>(ghostVal));
        norm = juce::jlimit(0.0, 1.0, norm);
        const int thumbW = slider.getLookAndFeel().getSliderThumbRadius(slider) * 2;
        float gx, gy, r;
        if (slider.isVertical())
        {
            // alphaSlider: proportion→pixel matches JUCE's own thumb mapping
            // (top = high proportion; region inset by thumbW/2 each end — see
            // LookAndFeel_V2::getSliderLayout). norm already carries the A-top flip.
            const float trackY = static_cast<float>(sb.getY() + thumbW / 2);
            const float trackH = static_cast<float>(sb.getHeight() - thumbW);
            gx = static_cast<float>(sb.getCentreX());
            gy = trackY + trackH * static_cast<float>(1.0 - norm);
            // thumbW = getSliderThumbRadius*2 ≈ 2× the drawn thumb diameter, so
            // 0.22× lands the ghost just under that diameter.
            r  = static_cast<float>(thumbW) * 0.22f;
        }
        else
        {
            const int trackX = sb.getX() + thumbW / 2;
            const int trackW = sb.getWidth() - thumbW;
            gx = static_cast<float>(trackX) + static_cast<float>(trackW) * static_cast<float>(norm);
            gy = static_cast<float>(sb.getCentreY());
            r  = static_cast<float>(sb.getHeight()) * 0.28f;
        }
        g.setColour(kModCol.withAlpha(0.8f)); // ghost stays in the Mod-section colour
        g.fillEllipse(gx - r, gy - r, r * 2.0f, r * 2.0f);
    };

    // Alpha ghost only makes sense when the slider drives α (linear). In Step-in
    // and Layer the alpha-LFO offset is remapped onto the active parameter's
    // axis (lateMix or [splitStart, splitEnd]) — those ghosts paint on the
    // same physical slider but at the mode-specific value position. Easy-only:
    // alphaSlider belongs to the neural view — Advanced hides it and lays out
    // the DCO editor instead, so its bounds are stale there and must not be read.
    if (easyMode_)
    {
        if (injectionMode_ == "linear")
            drawGhost(alphaSlider, alphaGhostValue_);
        else if (injectionMode_ == "late_step"
              || injectionMode_ == "kombi1"
              || injectionMode_ == "kombi2"
              || injectionMode_ == "kombi3")
            drawGhost(alphaSlider, lateMixGhostValue_);
        else if (injectionMode_ == "layer_split")
        {
            // Two synchronous ghosts for the range slider — one per thumb.
            drawGhost(alphaSlider, splitStartGhostValue_);
            drawGhost(alphaSlider, splitEndGhostValue_);
        }
    }
    // (Magnitude/Chaos ghosts are painted by magRow/noiseRow themselves —
    // SliderRow's native setGhostValue/tickGhost — see timerCallback().)

    // Tiny A / 0 / B anchor scale at the slider's left edge, aligned to the snap
    // positions (−1 / 0 / +1). A minimal orientation aid replacing the removed
    // numeric readout; in the impulse identity colours (A periwinkle, centre
    // neutral, B gold). Linear mode only — other modes give the slider different
    // semantics. Same value→pixel mapping as the ghost so the marks sit exactly
    // where the thumb detents. Easy-only, same reason as the ghost above.
    if (easyMode_ && injectionMode_ == "linear")
    {
        const auto sb = alphaSlider.getBounds();
        const int   thumbW  = alphaSlider.getLookAndFeel().getSliderThumbRadius(alphaSlider) * 2;
        const float trackY  = static_cast<float>(sb.getY() + thumbW / 2);
        const float trackH  = static_cast<float>(sb.getHeight() - thumbW);
        const float thumbDia = static_cast<float>(thumbW) * 0.5f;  // actual drawn thumb Ø
        const int   labelW  = juce::jmax(7, juce::roundToInt(
                                  static_cast<float>(sb.getCentreX() - sb.getX()) - thumbDia * 0.5f - 1.0f));
        const float fontH   = juce::jlimit(8.0f, 11.0f, trackH * 0.07f);
        g.setFont(juce::FontOptions(fontH));

        struct Anchor { float v; const char* t; juce::Colour c; };
        const Anchor anchors[] = {
            { -1.0f, "A", kImpulseAText },
            {  0.0f, "0", kTextMuted    },
            {  1.0f, "B", kImpulseB     },
        };
        for (const auto& a : anchors)
        {
            const double norm = juce::jlimit(0.0, 1.0,
                alphaSlider.valueToProportionOfLength(static_cast<double>(a.v)));
            const int y = juce::roundToInt(trackY + trackH * static_cast<float>(1.0 - norm) - fontH * 0.5f);
            g.setColour(a.c);
            g.drawText(juce::String(a.t), sb.getX(), y, labelW, juce::roundToInt(fontH),
                       juce::Justification::centred, false);
        }
    }
    // (The SA3 s/m tier cell — divider + letter — is drawn by ModelSwitchLnF in the
    // button itself via the "tierLetter" property, not overpainted here.)
}

void PromptPanel::resized()
{
    auto b = getLocalBounds();
    float w = static_cast<float>(b.getWidth());
    int pad = juce::roundToInt(w * kPromptPadFactor);
    auto area = b.reduced(pad);

    float f = juce::jlimit(10.0f, 20.0f,
        (static_cast<float>(area.getHeight()) - 2.0f) / kPromptContentUnits);
    int gap = juce::roundToInt(f * kPromptGap);
    int modelGap = juce::roundToInt(f * kPromptModelGap);
    int innerGap = juce::roundToInt(f * kPromptInnerGap);
    int groupGap = juce::roundToInt(f * kPromptGroupGap);
    int modeBarH = juce::roundToInt(f * kPromptModeBar);
    int compactRowH = juce::roundToInt(f * kPromptCompactRow);
    int compactCtrlH = juce::roundToInt(f * kPromptCompactCtrl);
    int seedCtrlH = juce::roundToInt(f * kPromptSeedCtrl);
    int repromptRowH = juce::roundToInt(f * kPromptReprompt);

    const bool easy = easyMode_;

    // Visibility split: Advanced IS the DCO panel now — a completely different
    // paradigm from the neural Easy view, not a variant of it. Model selector,
    // A/B editors, injection-mode bar, translate flag, alpha slider, and the
    // Re-Prompt module are Easy-only; the DCO trio (prompt editor + BAKE +
    // status) is Advanced-only.
    for (int i = 0; i < kNumModelSlots; ++i)
        modelBtns[i].setVisible(easy);
    promptAEditor.setVisible(easy);
    promptBEditor.setVisible(easy);
    injModeLinear.setVisible(easy);
    injModeFine.setVisible(easy);
    injModeLayer.setVisible(easy);
    injModeKombi1.setVisible(easy);
    injModeKombi2.setVisible(easy);
    injModeKombi3.setVisible(easy);
    translateToggle.setVisible(easy);
    alphaSlider.setVisible(easy);
    repromptModuleBox.setVisible(easy);
    repromptStanceBar.setVisible(easy);
    for (auto& bCoupling : repromptCouplingBtns)
        bCoupling.setVisible(easy);

    for (auto& bSeed : seedModeBtns)
        bSeed.setVisible(easy);
    // Duration/Magnitude/Chaos have no advanced form any more — they're the
    // inline durationRow/magRow/noiseRow, shown in Easy only.
    durationRow->setVisible(easy);
    durationTicks->setVisible(easy);
    magRow->setVisible(easy);
    noiseRow->setVisible(easy);
    varSwitchLabel.setVisible(easy);
    // The DCO surface IS the Advanced canvas now (its own prompt editor + the
    // BAKE/status row), not one row sharing the canvas with neural controls.
    // dcoStatusLabel is the hidden logical status holder now (never laid out/
    // shown — see setLcoStatus); it is intentionally NOT toggled here.
    dcoPromptEditor.setVisible(!easy);
    for (auto& b : dcoModelBtns) b.setVisible(!easy);
    dcoTraceView.setVisible(!easy);
    dcoFlagsLabel.setVisible(!easy);
    dcoStanceBar.setVisible(!easy);
    dcoRepromptBox.setVisible(!easy);
    dcoSetsParamsBtn.setVisible(!easy);

    if (!easy)
    {
        // Chrome sentinels: these gate the custom paint()/paintOverChildren()
        // drawing (switchbox frames, params divider) for components hidden
        // above — clear them so nothing belonging to the neural view renders
        // behind the DCO panel.
        modelSwitchBounds = {};
        injModeSwitchBounds = {};
        seedModeSwitchBounds = {};
        paramsDividerY = -1;

        // The LCO panel REUSES the T5osc (neural/Easy) editor sizing verbatim
        // rather than the shared `f` above: `f` is height-driven
        // ((height-2)/kPromptContentUnits, kPromptContentUnits being the
        // NEURAL row-count budget), and MainPanel gives the LCO panel a much
        // TALLER height than that budget assumes, so `f` inflates toward its
        // 20px ceiling — the oversized-font bug. preferredPromptFontForWidth
        // is the SAME width-based font T5osc's own editors resolve to
        // (PromptPanel.cpp construction), so reusing it here makes the LCO
        // panel match the neural chrome exactly instead of scaling up.
        const float fLco = preferredPromptFontForWidth(getLocalBounds().getWidth());
        const int gapLco         = juce::roundToInt(fLco * kPromptGap);
        const int modelGapLco    = juce::roundToInt(fLco * kPromptModelGap);
        const int compactRowLco  = juce::roundToInt(fLco * kPromptCompactRow);
        const int multiInputHLco = juce::roundToInt(fLco * kPromptMultiInput);
        const float editorFontLco = fLco * 1.1f;

        // Top-down: the LCO model button row, then the FULL-WIDTH prompt
        // editor (A-styled) — no slider sharing this row any more, which was
        // squeezing it too narrow to use. Bottom-up (pinned so they sit
        // directly above the reused GENERATE button in the column below): the
        // RE-PROMPT framed card, then the flags list above it. The two HEARD
        // AS output editors (A/B-styled, one per engine) fill whatever
        // remains in the middle, with the E↔O oscillator-mix slider carved
        // off their combined right edge. There is no BAKE/STEP button —
        // GENERATE drives the bake/loop (triggerLcoGenerate).
        {
            auto modelRow = area.removeFromTop(compactRowLco + 2);
            const int slotW = modelRow.getWidth() / kNumLcoModelSlots;
            for (int i = 0; i < kNumLcoModelSlots; ++i)
                dcoModelBtns[i].setBounds(i == kNumLcoModelSlots - 1
                                              ? modelRow                        // last slot absorbs the rounding remainder
                                              : modelRow.removeFromLeft(slotW));
        }
        area.removeFromTop(modelGapLco);

        // Prompt block — full width now (see comment above).
        {
            auto promptBlock = area.removeFromTop(multiInputHLco);
            dcoPromptEditor.applyFontToAllText(juce::FontOptions(editorFontLco));
            dcoPromptEditor.setBounds(promptBlock);
        }
        area.removeFromTop(gapLco);

        // RE-PROMPT framed ModuleBox — DIMENSIONALLY IDENTICAL to the neural
        // repromptModuleBox (same kPromptReprompt height budget, same header,
        // same stance-glyph region width), so the phase-portrait glyphs render at
        // EXACTLY the T5osc size. The glyph radius is HEIGHT-BOUND
        // (RepromptStanceBar: R = min(barH*0.36, (barW/N)*0.46)), so a shorter
        // box shrinks the icons — an earlier compact version floored them to half
        // size. The LCO has no A/B coupling stack, so the column the neural box
        // reserves on the right is simply left empty and the SAME-WIDTH glyph bar
        // is CENTRED in the content (not sprawled full-width, which read as an
        // oversized empty card). Pinned near the panel bottom; the AIR above
        // GENERATE comes from the MainPanel GENERATE-block centering (the same
        // slack T5osc has), NOT from padding inside this panel.
        {
            // Size THIS box from the RECONSTRUCTED neural Easy-mode font, NOT fLco.
            // The neural RE-PROMPT box is laid out in Easy mode, where MainPanel
            // sits the panel at exactly getPreferredHeightForWidth
            // (MainPanel.cpp:3462/3533), so its resized() font resolves to
            //   f = (oscH - 2*pad - 2)/kPromptContentUnits  ==  fLco - 2*pad/units,
            // i.e. ~1.2 units SMALLER than the width-based fLco. The LCO panel is
            // deliberately TALLER (MainPanel.cpp:3527), so sizing this box with
            // fLco makes it ~10-15% too tall and — because the glyph radius is
            // HEIGHT-bound — renders the stance icons LARGER than T5osc's. Rebuild
            // the neural Easy font here so the box, and thus the glyph size,
            // matches T5osc EXACTLY. (At the narrow kMinOscH clamp both paths
            // converge, so not mirroring that clamp is harmless.)
            const float fRp = juce::jlimit(10.0f, 20.0f,
                (static_cast<float>(getPreferredHeightForWidth(b.getWidth())) - 2.0f * pad - 2.0f)
                    / kPromptContentUnits);
            const int rpBoxH   = juce::roundToInt(fRp * kPromptReprompt);
            const int rpPad    = juce::jmax(3, juce::roundToInt(fRp * 0.3f));
            const int rpHeader = juce::roundToInt(fRp * kPromptCompactRow);
            auto rpArea = area.removeFromBottom(rpBoxH);
            dcoRepromptBox.setBaseFont(fRp);
            dcoRepromptBox.setHeaderHeight(rpHeader);
            dcoRepromptBox.setContentPadding(rpPad);
            dcoRepromptBox.setBounds(rpArea);
            auto content = dcoRepromptBox.getContentBounds();
            // Match the neural glyph-region WIDTH exactly (there the right ~30% is
            // the coupling column + a separating gap): this fixes the glyph pitch
            // and size to T5osc's instead of stretching 7 glyphs across the full
            // width. Centre the resulting bar since there is no coupling column.
            const int couplingLikeW = juce::jlimit(58, 96, juce::roundToInt(content.getWidth() * 0.30f));
            const int couplingGap   = juce::jmax(juce::roundToInt(fRp * kPromptGap) * 2,
                                                 juce::roundToInt(fRp * 0.9f));
            const int barW = juce::jmax(1, content.getWidth() - couplingLikeW - couplingGap);
            dcoStanceBar.setBounds(content.withSizeKeepingCentre(barW, content.getHeight()));
        }
        // The knob-authority switch, right-aligned in the air above the card —
        // one compact row out of the tripled gap, which was pure breathing room.
        area.removeFromBottom(gapLco);
        {
            auto row = area.removeFromBottom(compactRowLco);
            const int w = juce::jmin(row.getWidth(), juce::roundToInt(fLco * 4.6f));
            dcoSetsParamsBtn.setBounds(row.removeFromRight(w));
        }
        // Tripled breathing room above the RE-PROMPT card (was a single gapLco,
        // too cramped against the HEARD AS box). The extra space is taken from
        // the HEARD AS editor below, which fills whatever `area` remains.
        area.removeFromBottom(gapLco * 2);

        // dcoFlagsLabel is no longer laid out. Under the write-path it only ever
        // carried the compile state ("compiling..." / the Csound error) — the
        // per-word guardrail flags it was built for belong to the retired keys
        // path and were always empty here. That state is now the trace's own
        // RUNNING station, so the line above the RE-PROMPT card is gone and the
        // trace reclaims its height. The label survives as the logical holder,
        // like dcoStatusLabel, because several call sites write it.
        dcoFlagsLabel.setBounds({});

        // The trace fills the remaining middle — the LCO's disclosure surface
        // (docs/DCO_REPROMPT_CONCEPT.md): what the machine heard, made visible
        // and negotiable. It takes the FULL former two-card area (the E↔O
        // oscillator-mix slider and the second, Inharmonic-engine card it used
        // to sit beside are both retired — BJ 2026-07-17: "this split is dead").
        // Flexes to absorb the tall LCO panel; status and empty-state text are
        // carried by the view itself (setPlaceholder / setLcoStatus).
        {
            // fLco, not editorFontLco. The editor's font is the panel base
            // ENLARGED for the prompt box; feeding it here and applying the
            // house Caption role on top rendered the trace larger than the
            // prompt it is about. This view is chrome, so it takes the same
            // base every other label in the panel takes.
            dcoTraceView.setBaseFont(fLco);
            dcoTraceView.setBounds(area);
        }
        return;
    }

    // ── Model selector switchbox at top (compact, fixed 5 slots) ──
    // Easy-only — Advanced returned above with its own DCO layout.
    {
        auto modelRow = area.removeFromTop(compactRowH + 2);
        // Distribute remaining row width across remaining slots so the row
        // always fills to the right edge regardless of slot count. Earlier
        // we used a fixed f*5.5f per cell, which clipped the rightmost
        // button on narrow panels once we went from 4 slots to 5. The same
        // pattern is used for seedModeBtns above.
        for (int i = 0; i < kNumModelSlots; ++i)
        {
            const int cellW = (i == kNumModelSlots - 1)
                ? modelRow.getWidth()
                : juce::jmax(1, modelRow.getWidth() / (kNumModelSlots - i));
            modelBtns[i].setBounds(modelRow.removeFromLeft(cellW));
        }
        modelSwitchBounds = modelBtns[0].getBounds()
            .getUnion(modelBtns[kNumModelSlots - 1].getBounds());
        area.removeFromTop(modelGap);
    }

    const int multiInputH = juce::roundToInt(f * kPromptMultiInput);

    // ── A↔B block ──────────────────────────────────────────────────────────
    // Left column stacks [A editor / mode band / B editor] with breathing room
    // around the mode band so it reads as a deliberate control — the operator
    // that turns A into B — not a strip squeezed between the two inputs. The
    // right column is a slim full-height vertical blend slider whose A(top)→
    // B(bottom) gradient makes the relationship self-evident.
    {
        const int blockH = multiInputH + innerGap + modeBarH + innerGap + multiInputH;
        auto block = area.removeFromTop(blockH);

        // Right column: vertical A↔B slider, spanning the full block height so
        // its top edge meets A and its bottom edge meets B. The track+thumb are
        // slim (see AlphaSliderLnF), so a narrow column suffices.
        const int sliderColW = juce::jmax(30, juce::roundToInt(f * 1.9f));
        alphaSlider.setBounds(block.removeFromRight(sliderColW));
        block.removeFromRight(gap);

        // Impulse text is the primary input, so the editors carry a slightly
        // larger font than the surrounding chrome. applyFontToAllText (NOT setFont):
        // setFont only sets the font for text typed AFTER the call, so any prompt
        // already in the editor — prefill / preset / loop-apply set before this
        // resized() ran — keeps the default ~14px while newly typed text jumps to
        // editorFont (the erratic two-size look). applyFontToAllText re-fonts the
        // existing text too and sets the current font for new text, so the size is
        // uniform regardless of setText/resized order.
        const float editorFont = f * 1.1f;

        // Left column, top: Impulse A editor (purple).
        promptAEditor.applyFontToAllText(juce::FontOptions(editorFont));
        promptAEditor.setBounds(block.removeFromTop(multiInputH));
        block.removeFromTop(innerGap);

        // Left column, middle: the injection-mode switchbox — six connected
        // radio buttons framed once by paintSwitchBoxBorder (matching the
        // model/seed switchboxes). The Union-Jack translate toggle sits
        // separately at the right end of the row.
        {
            auto modeRow = block.removeFromTop(modeBarH);

            // Union-Jack translate toggle: reserve the same right region (so the mode
            // switchbox keeps its width), then CENTRE the flag within it — midway
            // between the switchbox and the prompt-box right edge above — rather than
            // flush-right, so it has equal breathing room on both sides.
            const int flagRegionW = juce::jmax(1, modeRow.getWidth() / 7 + juce::jmax(2, gap));
            auto flagRegion = modeRow.removeFromRight(flagRegionW);
            const int flagH = juce::jmax(8, juce::roundToInt(modeRow.getHeight() * 0.80f));
            const int flagW = juce::jmin(flagRegionW, juce::roundToInt(flagH * 1.6f));
            translateToggle.setBounds(flagRegion.withSizeKeepingCentre(flagW, flagH));

            // Six connected radio buttons fill the remaining width; the last
            // claims the integer-division remainder so the row ends flush.
            int btnW = juce::jmax(1, modeRow.getWidth() / 6);
            injModeLinear.setBounds(modeRow.removeFromLeft(btnW));
            injModeFine  .setBounds(modeRow.removeFromLeft(btnW));
            injModeLayer .setBounds(modeRow.removeFromLeft(btnW));
            injModeKombi1.setBounds(modeRow.removeFromLeft(btnW));
            injModeKombi2.setBounds(modeRow.removeFromLeft(btnW));
            injModeKombi3.setBounds(modeRow);
            injModeSwitchBounds = injModeLinear.getBounds()
                .getUnion(injModeKombi3.getBounds());
        }
        block.removeFromTop(innerGap);

        // Left column, bottom: Impulse B editor (yellow). applyFontToAllText, see A above.
        promptBEditor.applyFontToAllText(juce::FontOptions(editorFont));
        promptBEditor.setBounds(block.removeFromTop(multiInputH));
    }

    // ── Re-Prompt module ──────────────────────────────────────────────────────
    // A framed ModuleBox (card + accent top-header), the same template as Duration/
    // Variation/Resynth. The header strip is the "RE-PROMPT" title; the content
    // holds the stance glyph bar (left) and the 3-way coupling stack (right). The
    // stance bar gets the FULL module-content height — its phase-portrait glyph
    // size is min(height, width)-bound, so keeping it tall AND wide (only the
    // narrow coupling column comes off the right) un-squeezes the glyphs.
    area.removeFromTop(innerGap);
    {
        auto rpArea = area.removeFromTop(repromptRowH);   // repromptRowH = module height
        const int rpPad = juce::jmax(3, juce::roundToInt(f * 0.3f));
        repromptModuleBox.setBaseFont(f);
        repromptModuleBox.setHeaderHeight(compactRowH);
        repromptModuleBox.setContentPadding(rpPad);
        repromptModuleBox.setBounds(rpArea);
        auto content = repromptModuleBox.getContentBounds();   // PARENT-relative

        // Coupling stack on the right (3 stacked radio buttons), unchanged.
        const int couplingW = juce::jlimit(58, 96, juce::roundToInt(content.getWidth() * 0.30f));
        auto couplingCol = content.removeFromRight(juce::jmin(couplingW, content.getWidth()));
        // Clearer separation between the stance bar ("…opposite") and the coupling.
        const int couplingGap = juce::jmax(gap * 2, juce::roundToInt(f * 0.9f));
        content.removeFromRight(juce::jmin(couplingGap, content.getWidth()));

        // Stance glyph bar fills the rest (full content height → large glyphs).
        repromptStanceBar.setBounds(content);

        const int segGap = 1;
        const int segH = juce::jmax(9,
            (couplingCol.getHeight() - segGap * (kNumCouplingBtns - 1)) / kNumCouplingBtns);
        for (int i = 0; i < kNumCouplingBtns; ++i)
        {
            repromptCouplingBtns[i].setBounds(couplingCol.removeFromTop(juce::jmin(segH, couplingCol.getHeight())));
            if (i < kNumCouplingBtns - 1)
                couplingCol.removeFromTop(juce::jmin(segGap, couplingCol.getHeight()));
        }
    }

    // Separator between the Re-Prompt row and the 2x2 gen-param block below: a
    // thin divider line centred in the groupGap (drawn in paintOverChildren).
    // Easy-only — Advanced returned above with its own DCO layout and set
    // paramsDividerY = -1, so no divider paints under the DCO panel.
    area.removeFromTop(groupGap / 2);
    paramsDividerY = area.getY();
    area.removeFromTop(groupGap - groupGap / 2);

    // --- Compact params: 2 columns ---
    int colGap = juce::roundToInt(w * 0.03f);

    // (layoutCompactPair/layoutSeedRow were removed in DCO Slice 0 — Advanced no
    // longer renders a Steps/CFG/Seed param grid.)

    // Easy view: the four generation params in a 2x2 block of side-by-side pair
    // rows, mirroring layoutCompactPair's column split. Row 1 = Duration (inline
    // SliderRow) | Variation (VAR switchbox: "VAR" caption + the 3 connected
    // seed-mode icons, framed by paintSwitchBoxBorder). Row 2 = Magnitude | Chaos
    // (inline SliderRows). Magnitude/Chaos moved out of Advanced entirely
    // (mirrors Duration's earlier move). Each cell is one standard inline band
    // at the template row height (rowH = compactRow), same as the resynth/cache
    // rows; nothing stacked full-width, nothing oversized. Height budget:
    // getPreferredHeightForWidth / kPromptContentUnits.
    auto layoutEasyGenParamsBlock = [&]
    {
        const int rowH = compactRowH;
        const int tickH = juce::roundToInt(f * kPromptTicks);
        const int tickGapH = juce::roundToInt(f * kPromptTicksGap);
        const int colW = (area.getWidth() - colGap) / 2;

        // The VAR switchbox occupies one pair-cell: "VAR" caption + the 3
        // connected seed-mode icons (union framed by paintSwitchBoxBorder).
        auto layoutVarSwitchbox = [&](juce::Rectangle<int> cell)
        {
            setUiFont(varSwitchLabel, TextRole::Caption, f);
            const int varLabelW = measureTextWidth("VAR", uiFontSize(TextRole::Caption, f))
                                + juce::roundToInt(f * 0.6f);
            varSwitchLabel.setBounds(cell.removeFromLeft(juce::jmin(varLabelW, cell.getWidth() / 2)));
            cell.removeFromLeft(juce::jmax(2, juce::roundToInt(f * 0.3f)));
            for (int i = 0; i < kNumSeedModeBtns; ++i)
            {
                const int cellWSeed = (i == kNumSeedModeBtns - 1)
                    ? cell.getWidth()
                    : juce::jmax(1, cell.getWidth() / (kNumSeedModeBtns - i));
                seedModeBtns[i].setBounds(cell.removeFromLeft(cellWSeed));
            }
            seedModeSwitchBounds = seedModeBtns[0].getBounds()
                .getUnion(seedModeBtns[kNumSeedModeBtns - 1].getBounds());
        };

        // Row 1: Duration | Variation, with the Duration bar's scale marks in a
        // strip directly beneath it (left column only, same x and width as the
        // bar so the hairlines meet the fill edge).
        {
            auto row = area.removeFromTop(rowH);
            const auto durBounds = row.removeFromLeft(colW);
            durationRow->setBounds(durBounds);
            row.removeFromLeft(colGap);
            layoutVarSwitchbox(row);
            durationTicks->setBounds(area.removeFromTop(tickH).withX(durBounds.getX())
                                                             .withWidth(durBounds.getWidth()));
            area.removeFromTop(tickGapH);
            area.removeFromTop(gap);
        }

        // Row 2: Magnitude | Chaos.
        {
            auto row = area.removeFromTop(rowH);
            magRow->setBounds(row.removeFromLeft(colW));
            row.removeFromLeft(colGap);
            noiseRow->setBounds(row);
            area.removeFromTop(gap);
        }
    };

    // Center the generation-param block in the free space below the divider.
    // At the panel's preferred (minimum) height the slack is zero, so the block
    // sits flush above SEMANTIC AXES; any extra height is split evenly above and
    // below it. (Easy-only code from here on — Advanced returned above.)
    area.removeFromTop(juce::jmax(0, area.getHeight()
                                    - (2 * (compactRowH + gap)
                                       + juce::roundToInt(f * (kPromptTicks + kPromptTicksGap)))) / 2);

    layoutEasyGenParamsBlock();
}

void PromptPanel::loadPresetData(const juce::String& promptA, const juce::String& promptB,
                                  int seed, bool randomSeed,
                                  const juce::String& device,
                                  const juce::String& model,
                                  const juce::String& injectionMode,
                                  float lateMixAmount,
                                  float splitStart,
                                  float splitEnd)
{
    juce::ignoreUnused(device);
    promptAEditor.setText(promptA, false);
    promptBEditor.setText(promptB, false);
    lastGenPromptA_.clear();
    lastGenPromptB_.clear();
    // Re-Prompt loop runtime is per-session, NOT preset state. Reset the engage
    // gate so (a) the deactivation-restore can't overwrite the just-loaded prompts
    // with the OLD session's captured originals, and (b) the loop re-captures the
    // NEW prompts when it next engages. Stance/coupling themselves are APVTS params
    // the processor restores separately (Off for presets predating Re-Prompt).
    loopEngaged_ = false;
    loopOriginalsValid_ = false;
    // The loaded prompts ARE the human authorship for this preset — seed the durable
    // store so a re-save reproduces them (and a subsequent loop engage restores to
    // them), regardless of any rewrite the loop later puts in the editors. setText
    // above used dontSendNotification, so onTextChange did NOT fire — set explicitly.
    processorRef.setHumanPrompts(promptA, promptB);
    // Write BOTH seed fields before syncing: syncSeedModeFromCurrentState()
    // derives the VAR switchbox mode from getLastRandomSeed(), so the random
    // flag must be current or the box shows the previous preset's mode.
    processorRef.setLastSeed(seed);
    processorRef.setLastRandomSeed(randomSeed);
    syncSeedModeFromCurrentState();
    if (auto* startParam = processorRef.getValueTreeState().getParameter(PID::genStart))
        startParam->setValueNotifyingHost(0.0f);

    // Apply research-mode injection state if the preset carries it. Old .t5p
    // files predating this feature pass empty/NaN sentinels here, in which case
    // we keep the panel's current values and skip applyModeToSlider() (the
    // panel's mode buttons / slider stay where the user left them).
    bool injectionDirty = false;
    if (injectionMode.isNotEmpty())
    {
        injectionMode_ = injectionMode;
        injectionDirty = true;
        injModeLinear.setToggleState(injectionMode == "linear",      juce::dontSendNotification);
        injModeFine  .setToggleState(injectionMode == "late_step",   juce::dontSendNotification);
        injModeLayer .setToggleState(injectionMode == "layer_split", juce::dontSendNotification);
        injModeKombi1.setToggleState(injectionMode == "kombi1",      juce::dontSendNotification);
        injModeKombi2.setToggleState(injectionMode == "kombi2",      juce::dontSendNotification);
        injModeKombi3.setToggleState(injectionMode == "kombi3",      juce::dontSendNotification);
    }
    if (!std::isnan(lateMixAmount))
    {
        // The preset stores a single lateMixAmount. Restore it to the slot
        // that matches the preset's injectionMode (when present); otherwise
        // restore to the currently active mode's slot.
        const juce::String slotMode = injectionMode.isNotEmpty() ? injectionMode : injectionMode_;
        lateMixForMode(slotMode) = juce::jlimit(0.0f, 1.0f, lateMixAmount);
        injectionDirty = true;
    }

    // Select the preset's model BEFORE clamping split values. Without this
    // step the clamp would use the previously selected model's ditBlocks_,
    // potentially truncating a preset whose SA3-sized splitEnd is valid
    // under SA3 but clipped to SAO Small's 16-block ceiling. modelBtns'
    // setToggleState is called with dontSendNotification so the onClick
    // lambda doesn't preload the model from a preset load — we just need
    // the UI state. refreshDitBlocksForCurrentModel then pulls the right
    // depth out of the backend's handshake metadata.
    bool deferredModel = false;
    if (model.isNotEmpty())
    {
        if (modelsPopulated)
        {
            // Exact installed id first, then family fallback so a legacy preset
            // id (pre-split "stable-audio-3-small") still selects its SA3 slot
            // instead of silently leaving the previously active model selected.
            const int s = slotForModel(model);
            if (s >= 0)
            {
                modelBtns[s].setToggleState(true, juce::dontSendNotification);
                activeModelSlot_ = s;   // keep the SA3 re-click baseline in sync, else
                                        // the next click on this slot would mis-fire as
                                        // a tier toggle instead of a fresh select
            }
            refreshDitBlocksForCurrentModel();
        }
        else
        {
            pendingModel_ = model;
            deferredModel = true;
        }
    }

    // Preset block-count may differ from the currently active model's
    // (e.g. saved under SA3 then recalled under SAO Small). Clamp to the
    // active model's ditBlocks_ ceiling so the slider stays in range.
    // When the model selection is deferred (backend not ready yet) we
    // stash the raw values so populateModelSelector can replay them once
    // ditBlocks_ reflects the preset's intended model.
    if (deferredModel)
    {
        if (!std::isnan(splitStart)) pendingSplitStart_ = splitStart;
        if (!std::isnan(splitEnd))   pendingSplitEnd_   = splitEnd;
    }
    else
    {
        const float blocksF = static_cast<float>(ditBlocks_);
        if (!std::isnan(splitStart))
        {
            splitLayerStart_ = juce::jlimit(0.0f, blocksF, splitStart);
            injectionDirty = true;
        }
        if (!std::isnan(splitEnd))
        {
            splitLayerEnd_ = juce::jlimit(0.0f, blocksF, splitEnd);
            injectionDirty = true;
        }
    }
    if (injectionDirty)
        applyModeToSlider();

    syncInjectionModeAvailability();
}

void PromptPanel::populateDeviceChoice()
{
    auto& pipeInf = processorRef.getPipeInference();
    defaultInferenceDevice_ = pipeInf.getDefaultDevice();
    if (defaultInferenceDevice_.isEmpty())
    {
        auto& devs = pipeInf.getAvailableDevices();
        if (!devs.isEmpty())
            defaultInferenceDevice_ = devs[0];
    }
    devicesPopulated = true;
}

// Map a model id to its fixed switchbox slot by family pattern, independent of
// whether that slot is installed. Slot 0 SA3 Music, 1 SA3 SFX, 2 SA1 Open,
// 3 SA1 Small, 4 AudioLDM2; -1 if no family matches.
//
// Order matters: SA3 names contain "small", so the SA3 check must fire before
// the SA1 Small fallback; within SA3 the "sfx" sub-check splits Music (0) from
// SFX (1) — both carry "stable-audio-3". This is the single source of truth for
// the mapping; populateModelSelector and slotForModel both defer to it.
static int patternSlotFor(const juce::String& m)
{
    if (m.containsIgnoreCase("stable-audio-3"))     return m.containsIgnoreCase("sfx") ? 1 : 0;
    if (m.containsIgnoreCase("small"))              return 3;  // SA1 Small
    if (m.containsIgnoreCase("stable-audio-open"))  return 2;  // SA1 Open 1.0
    if (m.containsIgnoreCase("audioldm")
        || m.containsIgnoreCase("audio-ldm"))       return 4;  // AudioLDM2
    return -1;
}

// Persisted-id domain tag. A single SA3 checkpoint that backs BOTH the Music (0)
// and SFX (1) slots (medium) can't encode the chosen domain in its bare dir id, so
// when SFX is the active domain we suffix "-sfx". On recall slotForModel/patternSlotFor
// re-derive slot 1 from that token and shortenModelName renders it as "SA3 SFX"; the
// real dir id is always recovered from modelSlotIds[] (this token is never used to load
// a model). Identity for ids that already carry the domain (small-sfx), the Music slot,
// and non-SA3 models — so every existing persisted id is byte-for-byte unchanged.
static juce::String taggedPersistId(const juce::String& id, bool sfxDomain)
{
    return (sfxDomain && id.containsIgnoreCase("stable-audio-3") && !id.containsIgnoreCase("sfx"))
               ? id + "-sfx" : id;
}

// Resolve a stored/preset model id to an INSTALLED slot: exact installed-id
// match first, then a family fallback (patternSlotFor) so a legacy preset id —
// e.g. the pre-split "stable-audio-3-small" — still selects its family's slot
// when that model is installed. -1 when nothing suitable is installed.
int PromptPanel::slotForModel(const juce::String& model) const
{
    if (model.isEmpty())
        return -1;
    for (int i = 0; i < kNumModelSlots; ++i)
        if (modelSlotIds[i] == model)
            return i;
    const int s = patternSlotFor(model);
    return (s >= 0 && s < kNumModelSlots && modelSlotIds[s].isNotEmpty()) ? s : -1;
}

void PromptPanel::populateModelSelector()
{
    auto& pipeInf = processorRef.getPipeInference();
    auto& models = pipeInf.getAvailableModels();

    // Match available models to fixed slots by pattern
    // Slot 0: SA3 Music, 1: SA3 SFX, 2: SA1 Open, 3: SA1 Small, 4: AudioLDM2
    for (int i = 0; i < kNumModelSlots; ++i)
        modelSlotIds[i] = {};

    for (auto& m : models)
    {
        // Medium is NOT slotted generically — it shares slot 0 with small-music
        // (patternSlotFor routes both there, having no "sfx" token), so letting it
        // through here would make slot 0 depend on backend iteration order. The tier
        // block below is the SOLE authority for placing medium; skipping it here lets
        // small deterministically own slots 0/1 whenever the tier resolves to small.
        if (m.containsIgnoreCase("stable-audio-3") && m.containsIgnoreCase("medium"))
            continue;

        const int slot = patternSlotFor(m);
        if (slot >= 0 && slot < kNumModelSlots)
        {
            modelSlotIds[slot] = m;
            modelBtns[slot].setEnabled(true);
            modelBtns[slot].setAlpha(1.0f);
        }
    }

    // SA3 tier resolution (per-machine, default "small" — installing medium no longer
    // silently takes over; the user opts in via the visible tier switch). Medium is
    // ONE checkpoint that renders BOTH domains (track_type, not the dir name, picks
    // Music vs SFX), so when it owns the SA3 tier it fills BOTH the Music (0) and SFX
    // (1) slots; small ships two checkpoints (placed in 0/1 by the generic loop above,
    // which deliberately skipped medium). Decide which tier owns the SA3 slots.
    juce::String sa3MediumId;
    bool sa3SmallInstalled = false;
    for (auto& m : models)
    {
        if (!m.containsIgnoreCase("stable-audio-3")) continue;
        if (m.containsIgnoreCase("medium")) sa3MediumId = m;
        else                                sa3SmallInstalled = true;   // small-music / small-sfx
    }
    // A real choice exists only when BOTH tiers are installed; otherwise the tier is
    // forced and the switch stays hidden. Use medium when the user picked it OR when
    // it is the only SA3 installed (so a medium-only machine still backs the SA3 slots).
    sa3TierChoiceAvailable_ = sa3MediumId.isNotEmpty() && sa3SmallInstalled;
    // Hand the active tier letter to the model-switch LnF, which draws it as a divider
    // + s/m cell on the two SA3 slots. Empty string on every other state means "plain
    // slot" — the LnF then falls back to a normal centred label.
    for (int s : { 0, 1 })
    {
        modelBtns[s].getProperties().set("tierLetter",
            sa3TierChoiceAvailable_ ? (sa3Tier_ == "medium" ? "m" : "s") : juce::String());
        modelBtns[s].repaint();
    }
    const bool useMedium = sa3MediumId.isNotEmpty()
                           && (sa3Tier_ == "medium" || !sa3SmallInstalled);
    if (useMedium)
        for (int s : { 0, 1 })
        {
            modelSlotIds[s] = sa3MediumId;
            modelBtns[s].setEnabled(true);
            modelBtns[s].setAlpha(1.0f);
        }
    // (The effective tier is surfaced by the s/m cell ModelSwitchLnF draws on the SA3
    // slots from the "tierLetter" property set just above. When only one tier is
    // installed the tier is forced, tierLetter is empty, the cell is hidden, and there
    // is nothing to toggle.)

    // Select model: pending preset model > leftmost installed slot in display
    // order. SA3 Music sits at slot 0, so "leftmost installed" naturally
    // prefers SA3 when present and falls through SA1 Open → SA1 Small →
    // AudioLDM2 in turn when earlier slots are empty.
    int selectIdx = -1;
    if (pendingModel_.isNotEmpty())
    {
        selectIdx = slotForModel(pendingModel_);   // exact id, then family fallback
        pendingModel_ = {};
    }
    if (selectIdx < 0)
    {
        for (int i = 0; i < kNumModelSlots; ++i)
            if (modelSlotIds[i].isNotEmpty()) { selectIdx = i; break; }
    }
    if (selectIdx >= 0)
    {
        modelBtns[selectIdx].setToggleState(true, juce::dontSendNotification);
        activeModelSlot_ = selectIdx;   // baseline for SA3 re-click tier toggle
    }

    modelsPopulated = true;
    syncInjectionModeAvailability();

    // refreshDitBlocksForCurrentModel() calls getModelMetadata(), which contends on
    // the PipeInference mutex that generate() holds for the entire blocking IPC round-
    // trip — calling it on the message thread mid-render freezes the GUI. Every other
    // caller of populateModelSelector runs at idle (generating == false), but a tier
    // flip (setSa3Tier) can land here while a generation / drift auto-regen is in
    // flight. Mirror the model-switch onClick's deferral: skip the DiT re-scope while
    // generating; it re-applies on the next idle model touch.
    if (!generating)
        refreshDitBlocksForCurrentModel();

    // Replay deferred preset splits now that ditBlocks_ reflects the real
    // model. loadPresetData stashed these when the backend wasn't ready yet
    // and the clamp ceiling was still the previous model's value.
    const bool hasPendingStart = !std::isnan(pendingSplitStart_);
    const bool hasPendingEnd   = !std::isnan(pendingSplitEnd_);
    if (hasPendingStart || hasPendingEnd)
    {
        const float blocksF = static_cast<float>(ditBlocks_);
        if (hasPendingStart)
            splitLayerStart_ = juce::jlimit(0.0f, blocksF, pendingSplitStart_);
        if (hasPendingEnd)
            splitLayerEnd_   = juce::jlimit(0.0f, blocksF, pendingSplitEnd_);
        pendingSplitStart_ = std::numeric_limits<float>::quiet_NaN();
        pendingSplitEnd_   = std::numeric_limits<float>::quiet_NaN();
        if (injectionMode_ == "layer_split")
            applyModeToSlider();
    }

    resized();
    repaint();   // refresh the SA3 s/m tier badge for the new install/tier state
}

// juce::Slider works in double; the APVTS parameter range is float. Mirror the
// exact float→double bridge SliderParameterAttachment uses (forwarding
// convert/snapToLegalValue through the float range, copying interval/skew) so
// the slider keeps the same skew + whole-second detents the parameter defines.
static juce::NormalisableRange<double> toDoubleRange(juce::NormalisableRange<float> r)
{
    auto from = [r] (double s, double e, double v) mutable
                { r.start = (float) s; r.end = (float) e; return (double) r.convertFrom0to1 ((float) v); };
    auto to   = [r] (double s, double e, double v) mutable
                { r.start = (float) s; r.end = (float) e; return (double) r.convertTo0to1 ((float) v); };
    auto snap = [r] (double s, double e, double v) mutable
                { r.start = (float) s; r.end = (float) e; return (double) r.snapToLegalValue ((float) v); };

    juce::NormalisableRange<double> d { (double) r.start, (double) r.end,
                                        std::move (from), std::move (to), std::move (snap) };
    d.interval      = r.interval;
    d.skew          = r.skew;
    d.symmetricSkew = r.symmetricSkew;
    return d;
}

void PromptPanel::applyDurationRangeForCurrentModel()
{
    auto& apvts = processorRef.getValueTreeState();

    // SA3 generates variable-length, music-scale audio (rotary DiT, ~120s
    // trained); every other engine stays at T5ynth's 11s short-sound ceiling.
    const float maxSec = selectedModelIsSA3() ? 120.0f : 11.0f;

    // Narrow/widen the slider's range. The APVTS parameter already spans the
    // global 120s max, so this is safe against the SliderParameterAttachment
    // range-ownership rule documented in applyModeToSlider — we only ever shrink
    // the slider inside the parameter's range, never beyond it. setNormalisableRange
    // re-clamps the thumb with dontSendNotification (juce_Slider.cpp updateRange),
    // so it never writes back to the parameter on its own.
    durationRow->getSlider().setNormalisableRange(toDoubleRange(T5ynthProcessor::makeDurationRange(maxSec)));

    // The marks read their x back out of that range, so every one of them has
    // just moved (or left the scale entirely, on the 11s engines). Scoped to
    // the strip - a model change must not repaint the panel.
    durationTicks->repaint();

    // Model selection is UI state from the backend handshake, not an APVTS
    // parameter — at editor-construction / DAW state-restore time no model is
    // known yet and maxSec defaults to 11. Clamping the parameter here would
    // permanently downgrade a state-restored SA3 duration (e.g. 80s → 11s)
    // before SA3 is ever selected. So only re-clamp once the model is known;
    // the value survives in the parameter and the thumb resyncs when
    // populateModelSelector selects SA3 and calls us again with maxSec = 120.
    if (getSelectedModel().isEmpty())
        return;

    // Re-clamp into the new ceiling. Read the *parameter* (source of truth), not
    // the slider: on preset restore the slider may still be clamped under the
    // previous, narrower range, and using its stale value would overwrite a
    // freshly-restored long duration. sendNotificationSync re-syncs the thumb,
    // refreshes the "Ns" readout, and writes the clamped value back via durA.
    if (auto* p = apvts.getParameter(PID::genDuration))
    {
        const float cur     = p->convertFrom0to1(p->getValue());
        const float clamped = juce::jlimit(0.1f, maxSec, cur);
        durationRow->getSlider().setValue(clamped, juce::sendNotificationSync);
    }
}

void PromptPanel::refreshDitBlocksForCurrentModel()
{
    // The active model also governs the Duration ceiling (SA3 → 120s, else
    // 11s). Kept in lock-step with the DiT/layer re-scope below so every model
    // change re-scopes both sliders from a single call site.
    applyDurationRangeForCurrentModel();

    const auto model = getSelectedModel();
    if (model.isEmpty())
    {
        ditBlocks_ = 16;
        return;
    }

    const auto meta = processorRef.getPipeInference().getModelMetadata(model);
    // Defensive clamp: a backend reporting absurd block counts (negative,
    // zero, hundreds) would either invert the slider range or stretch it
    // beyond anything the kombi geometry tolerates. 64 is well above the
    // largest SA3 variant and gives us headroom for future architectures.
    const int blocks = juce::jlimit(1, 64, meta.ditBlocks);
    ditBlocks_ = blocks;

    // Re-clamp the panel's own state so any saved value beyond the new
    // ceiling (e.g. preset switched from SA3 back to SAO) snaps inside.
    const float maxF = static_cast<float>(blocks);
    splitLayerStart_ = juce::jlimit(0.0f, maxF, splitLayerStart_);
    splitLayerEnd_   = juce::jlimit(0.0f, maxF, splitLayerEnd_);

    // The alphaSlider's range is set inside applyModeToSlider; only re-run
    // it when layer_split is the active mode, otherwise we'd overwrite the
    // current mode's slider settings.
    if (injectionMode_ == "layer_split")
        applyModeToSlider();
}

void PromptPanel::refreshInferenceChoices()
{
    auto selectedModel = getSelectedModel();

    if (selectedModel.isNotEmpty())
        // Preserve the Music/SFX slot across the backend restart for a both-slots SA3
        // checkpoint (medium): re-stash slot 1 as the SFX-tagged id so the post-restart
        // populateModelSelector re-selects SFX rather than defaulting to Music (slot 0).
        pendingModel_ = taggedPersistId(selectedModel, getSelectedSlot() == 1);

    devicesPopulated = false;
    modelsPopulated = false;
    defaultInferenceDevice_.clear();

    for (int i = 0; i < kNumModelSlots; ++i)
    {
        modelSlotIds[i].clear();
        modelBtns[i].setToggleState(false, juce::dontSendNotification);
        modelBtns[i].setEnabled(false);
        modelBtns[i].setAlpha(0.3f);
    }

    if (!processorRef.isPipeInferenceReady())
        return;

    populateDeviceChoice();
    populateModelSelector();
}

void PromptPanel::setEasyMode(bool easy)
{
    if (easyMode_ == easy)
        return;

    easyMode_ = easy;
    if (easyMode_)
        syncSeedModeFromCurrentState();

    resized();
    repaint();
}

// Out-of-line because PromptPanel.h only forward-declares T5ynthProcessor.
int PromptPanel::getSeed() const        { return processorRef.getLastSeed(); }
bool PromptPanel::isRandomSeed() const  { return processorRef.getLastRandomSeed(); }

bool PromptPanel::hasHiddenActiveState() const
{
    // Nothing qualifies anymore: Magnitude/Chaos live on the Easy view, and
    // Steps/CFG have no UI anywhere (DCO Slice 0) — buildInferenceRequest
    // force-overrides them per model, so a stale APVTS value can never affect
    // a render. Pulsing the toggle would point at an empty Advanced canvas.
    // Revisit once the DCO surface holds real state.
    return false;
}

int PromptPanel::getSelectedSlot() const
{
    for (int i = 0; i < kNumModelSlots; ++i)
        if (modelBtns[i].getToggleState() && modelSlotIds[i].isNotEmpty())
            return i;
    return -1;
}

juce::String PromptPanel::getSelectedModel() const
{
    const int s = getSelectedSlot();
    return s >= 0 ? modelSlotIds[s] : juce::String();
}

// Per-machine SA3 tier (small | medium). Persisted by MainPanel in ui_settings.json,
// NOT in presets/APVTS — it's a hardware-capability choice, not part of the sound.
// Default "small" so installing the heavy medium checkpoint never silently takes
// over; the user opts in by re-clicking the active SA3 slot (critical-aesthetic
// mission: expose and let the user choose, don't substitute behind their back).
void PromptPanel::setSa3Tier(const juce::String& tier, bool persist)
{
    const juce::String t = tier.equalsIgnoreCase("medium") ? "medium" : "small";
    const bool changed = (t != sa3Tier_);
    sa3Tier_ = t;

    if (persist && changed && onSa3TierChanged)
        onSa3TierChanged(sa3Tier_);

    // Nothing to re-slot until the backend has reported its models (the startup push
    // only records the tier so the first populateModelSelector honors it), and no
    // work to do if the tier did not actually change.
    if (!modelsPopulated || !changed)
        return;

    // Re-resolve which checkpoint backs the SA3 slots for the new tier. populateModel-
    // Selector rebuilds modelSlotIds[], re-selects the leftmost slot, refreshes the
    // s/m badge, and runs that slot's per-selection consequences.
    const int prevSlot = getSelectedSlot();
    populateModelSelector();

    // Restore the user's slot if populate moved off it. SA3 Music/SFX share one
    // checkpoint id under medium (only the toggle matters — track_type derives from
    // the slot at request time), so no consequence re-run is needed for them; a
    // non-SA3 slot has a different id and needs its per-selection UI re-asserted.
    if (prevSlot > 0 && prevSlot < kNumModelSlots
        && modelSlotIds[prevSlot].isNotEmpty()
        && getSelectedSlot() != prevSlot)
    {
        modelBtns[prevSlot].setToggleState(true, juce::dontSendNotification);
        activeModelSlot_ = prevSlot;             // keep re-click baseline on the user's slot
        syncInjectionModeAvailability();        // re-eval injection + fire onModelChanged
        applyDurationRangeForCurrentModel();     // duration ceiling (pure UI)
        if (!generating)
            refreshDitBlocksForCurrentModel();   // DiT depth (IPC mutex — defer while generating)
    }
    repaint();   // ensure the s/m badge reflects the new tier
}

bool PromptPanel::isAudioLDM2Model(const juce::String& model) const
{
    return model.containsIgnoreCase("audioldm")
        || model.containsIgnoreCase("audio-ldm");
}

// Per-model defaults for steps / CFG. Single source for the model-click
// handler (which writes these into APVTS on model switch) and for
// hasHiddenActiveState (which compares against them to decide whether
// easy mode's params have been user-modified).
//  - SA3 Small Music: 8 steps, CFG 1.0 — verified from the model's own
//    model_config.json (training.demo.demo_steps = 8, demo_cfg_scales = [1])
//    and the HF model card's sample code (generate_diffusion_cond(...,
//    steps=8, cfg_scale=1.0, sampler_type="pingpong")). May 2026 sources.
//    Matched by SA3 prefix so future variants ("medium", future fine-tunes)
//    land here automatically rather than falling through to the SAO 1.0
//    branch.
//  - SAO Small: 8 steps, CFG 1.0 — same numbers, separate cause
//    (SAO Small's own model card recommends these).
//  - AudioLDM2: 50 / 3.5.
//  - SAO 1.0 and anything else: 20 / 7.0 (historical Brownian-tree
//    path defaults).
PromptPanel::DefaultParams PromptPanel::defaultParamsFor(const juce::String& model) const
{
    const bool isSA3       = model.containsIgnoreCase("stable-audio-3");
    const bool isSAOSmall  = !isSA3 && model.containsIgnoreCase("small");
    const bool isAudioLDM2 = isAudioLDM2Model(model);
    return {
        (isSA3 || isSAOSmall) ? 8.0f : (isAudioLDM2 ? 50.0f : 20.0f),
        (isSA3 || isSAOSmall) ? 1.0f : (isAudioLDM2 ? 3.5f  : 7.0f),
    };
}

bool PromptPanel::selectedModelIsAudioLDM2() const
{
    return isAudioLDM2Model(getSelectedModel());
}

bool PromptPanel::isSA3Model(const juce::String& model) const
{
    // Matches the populateModelSelector slotting and defaultParamsFor logic so
    // present and future SA3 variants ("small-music", "medium", fine-tunes)
    // are all recognised by the shared "stable-audio-3" prefix.
    return model.containsIgnoreCase("stable-audio-3");
}

bool PromptPanel::selectedModelIsSA3() const
{
    return isSA3Model(getSelectedModel());
}

void PromptPanel::setSeedMode(SeedMode mode, bool applyState)
{
    seedMode_ = mode;

    if (applyState)
    {
        if (mode == SeedMode::base)
        {
            processorRef.setLastSeed(kBaseSeed);
        }
        else if (mode == SeedMode::steady)
        {
            if (processorRef.getLastSeed() <= 0)
                processorRef.setLastSeed(kBaseSeed);
        }
    }

    seedMode_ = mode;
    syncSeedModeButtons();
    // Mirror the Easy-mode choice on the processor so PresetFormat::save sees
    // the current auto/random intent even when the user hasn't generated yet.
    processorRef.setLastRandomSeed(mode == SeedMode::autoRandom);
}

void PromptPanel::syncSeedModeFromCurrentState()
{
    if (processorRef.getLastRandomSeed())
        seedMode_ = SeedMode::autoRandom;
    else if (processorRef.getLastSeed() == kBaseSeed)
        seedMode_ = SeedMode::base;
    else
        seedMode_ = SeedMode::steady;

    syncSeedModeButtons();
}

void PromptPanel::syncSeedModeButtons()
{
    for (int i = 0; i < kNumSeedModeBtns; ++i)
        seedModeBtns[i].setToggleState(static_cast<int>(seedMode_) == i,
                                       juce::dontSendNotification);
}

void PromptPanel::selectInjectionMode(const juce::String& mode, bool shouldTrigger)
{
    injectionMode_ = selectedModelIsAudioLDM2() ? "linear" : mode;
    injModeLinear.setToggleState(injectionMode_ == "linear",      juce::dontSendNotification);
    injModeFine  .setToggleState(injectionMode_ == "late_step",   juce::dontSendNotification);
    injModeLayer .setToggleState(injectionMode_ == "layer_split", juce::dontSendNotification);
    injModeKombi1.setToggleState(injectionMode_ == "kombi1",      juce::dontSendNotification);
    injModeKombi2.setToggleState(injectionMode_ == "kombi2",      juce::dontSendNotification);
    injModeKombi3.setToggleState(injectionMode_ == "kombi3",      juce::dontSendNotification);
    applyModeToSlider();
    syncInjectionModeAvailability();

    if (shouldTrigger)
        triggerGeneration();
}

void PromptPanel::syncInjectionModeAvailability()
{
    const bool audioLDM2 = selectedModelIsAudioLDM2();
    if (audioLDM2 && injectionMode_ != "linear")
    {
        injectionMode_ = "linear";
        injModeLinear.setToggleState(true, juce::dontSendNotification);
        injModeFine  .setToggleState(false, juce::dontSendNotification);
        injModeLayer .setToggleState(false, juce::dontSendNotification);
        injModeKombi1.setToggleState(false, juce::dontSendNotification);
        injModeKombi2.setToggleState(false, juce::dontSendNotification);
        injModeKombi3.setToggleState(false, juce::dontSendNotification);
        applyModeToSlider();
    }

    const auto nonLinearAlpha = audioLDM2 ? 0.32f : 1.0f;
    const auto nonLinearEnabled = !audioLDM2;
    for (auto* b : { &injModeFine, &injModeLayer, &injModeKombi1, &injModeKombi2, &injModeKombi3 })
    {
        b->setEnabled(nonLinearEnabled);
        b->setAlpha(nonLinearAlpha);
        b->setTooltip(audioLDM2 ? "AudioLDM2 supports Linear mode only" : "");
    }
    injModeLinear.setEnabled(true);
    injModeLinear.setAlpha(1.0f);
    injModeLinear.setTooltip(audioLDM2 ? "AudioLDM2 supports Linear mode only" : "");

    // Every model-change path (button click, preset load, backend availability)
    // funnels through here — the same chokepoint that already gates injection
    // mode by model. Notify MainPanel so it can swap the AxesPanel's per-model
    // axis table (SAO/AudioLDM2 <-> SA3) and refresh the Resynth gate. Resynth is
    // the only element still scoped to SA3 — the semantic axes and DimExplorer
    // run on every engine. Cheap, idempotent, and user-driven (never on the
    // audio/timer hot path).
    if (onModelChanged)
        onModelChanged();
}

void PromptPanel::syncSeedState(int seed)
{
    processorRef.setLastSeed(seed);
}

// Easy-mode entry point for an exact fixed seed: double-clicking the Lock
// (steady) button in the Variation switchbox (mouseDoubleClick) opens this
// instead of just selecting steady mode. Mirrors the "Rename Preset" async
// AlertWindow pattern (MainPanel.cpp, onRenameRequested) — heap-owned,
// deleted inside its own modal callback; never a blocking modal loop.
void PromptPanel::openSeedEntryDialog()
{
    auto* alert = new juce::AlertWindow("Set Seed", "Enter a seed number:",
                                        juce::MessageBoxIconType::NoIcon, this);
    alert->addTextEditor("seed", juce::String(getSeed()));
    alert->getTextEditor("seed")->setInputRestrictions(12, "0123456789");
    alert->addButton("Set", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    alert->enterModalState(true,
        juce::ModalCallbackFunction::create([this, alert](int result)
        {
            std::unique_ptr<juce::AlertWindow> deleter(alert);
            if (result != 1) return;

            const int seed = alert->getTextEditor("seed")->getText().getIntValue();
            if (seed <= 0) return;

            // Value-first: write the typed seed into the store BEFORE switching
            // to steady mode, so setSeedMode keeps it instead of falling back
            // to getLastSeed()/kBaseSeed.
            syncSeedState(seed);
            setSeedMode(SeedMode::steady, true);
        }), false);
}

void PromptPanel::triggerGenerationWithOffsets(std::vector<std::pair<int, float>> offsets)
{
    pendingOffsets_ = std::move(offsets);
    triggerGeneration();
}

void PromptPanel::triggerLcoGenerate()
{
    // The reused GENERATE button's LCO action (also Cmd/Return and XL CC, since all
    // route through MainPanel::triggerMainGeneration). Stance Off — or no orchestra
    // yet, so there is nothing to listen to — authors from the current prompt; an
    // engaged stance runs one re-prompt STEP (listen → rewrite → bake).
    // triggerDcoBake / triggerDcoReprompt own the busy-gates and model checks.
    //
    // Gated on the ORCHESTRA, not on dcoLastMachineReading_: since the step listens
    // instead of reading the author's account of its own code, the reading is no
    // longer what the step needs — and a SNAP recall or a preset restore can load a
    // perfectly audible orchestra whose stored reading is empty, which used to make
    // GENERATE silently bake instead of stepping.
    //
    // dcoEarFailed_ keeps that from dead-ending. hasCsoundOrchestra() reads the last
    // REQUESTED text, which is never rolled back on a failed compile, so an orchestra
    // that cannot be rendered would otherwise route every press into a step that
    // always fails, with no way back to authoring except turning the stance off.
    // After one press has SAID the ear failed, the next authors.
    //
    // Every MANUAL route lands here — the GENERATE button and XL CC 37 via
    // MainPanel::triggerMainGeneration, and the prompt editor's Return key directly
    // — which is why the drop-to-Manual escape hatch sits here and not in MainPanel
    // (the neural path's copy lives there because triggerMainGeneration IS its only
    // manual route). Same reason as the neural one: a deliberate press must not be
    // overwritten by the cadence seconds later. In Manual the loop still advances,
    // one step per press, via the routing below.
    //
    // Unconditional, ahead of the gates the two callees own: a press that lands while
    // a step is in flight reports "Still …" and does nothing else, and stopping the
    // loop is exactly what it should still do — otherwise a running cadence could only
    // be halted at the REGENERATE switchbox, never from the button that started it.
    if (auto* p = processorRef.getValueTreeState().getParameter(PID::driftRegen))
        p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(DriftRegen::Manual)));

    // A full cache is answered HERE, before the stance routing below. The bake
    // at the end of a re-prompt step would replay too, but only after the step
    // has taken the Csound lifecycle lock for a bare-oscillator render, run a
    // listen and two model turns, and rewritten the prompt editor - which the
    // replay then writes straight back over. The button says "cache hit"; the
    // press has to be one.
    if (processorRef.isCsoundCacheFull())
    {
        playNextCachedCsound();
        return;
    }

    const int stance = static_cast<int>(processorRef.getValueTreeState()
                          .getRawParameterValue(PID::dcoRepromptStance)->load());
    if (stance != RepromptStance::Off && processorRef.hasCsoundOrchestra() && ! dcoEarFailed_)
        triggerDcoReprompt();
    else
        triggerDcoBake();
}

// The KNOBS station, re-read from the processor. Called at the publish and then
// again on every change of the borrow — the switch is an A/B and the same
// settings are on the patch or waiting for it depending on where it stands, so
// a station written once would go on claiming "waiting" over knobs that are
// live. What is drawn is never composed here: the lines come from the processor
// (read back off the parameters while they stand, from the request while they
// wait) and only the sentence that says WHY they wait is decided here, because
// only this side knows whether it was the switch or the oscillator.
// The KNOBS station for a set of author settings that has JUST been installed on
// the processor. Written once and called from both places that install one: the
// authoring pass's publish, and a cache replay - which installs the very same var
// and must therefore report the very same station, rather than the "a different
// patch has been loaded" line a stale generation counter would produce.
void PromptPanel::adoptAuthorSettingsStation(const juce::var& settings)
{
    juce::StringArray refused;
    if (auto* arr = settings.getArray())
        for (const auto& e : *arr)
        {
            if (static_cast<bool>(e.getProperty("ok", juce::var(false))))
                continue;
            const auto name = e.getProperty("name", juce::var()).toString();
            const auto val  = e.getProperty("value", juce::var()).toString();
            const auto note = e.getProperty("note", juce::var()).toString();
            refused.add(name + "  " + val + " - "
                        + (note.isEmpty() ? juce::String("not set") : note));
        }
    dcoKnobsRefused_ = refused;
    dcoKnobsAsked_.clear();
    dcoKnobsKnown_   = true;
    // The generation this card's station belongs to: the request just handed
    // over. Anything that replaces it — a preset, a restored session — is a
    // different patch, and the station says so instead of reporting that
    // patch's answer as this one's.
    dcoKnobsGen_ = processorRef.getAuthorSettingsGeneration();
    refreshLcoKnobStation();
}

void PromptPanel::refreshLcoKnobStation()
{
    if (! dcoKnobsKnown_) return;
    dcoKnobsRev_ = processorRef.getAuthorSettingsRevision();

    // A preset or a restored session replaced the patch, so the processor no
    // longer holds THIS card's request and what it does hold is not this sound's
    // to report. The lines stay — they are the record of what was authored —
    // and the station says plainly that they are no longer on the synth. Read as
    // a live answer instead, the station reported "the author took nothing"
    // under a card whose author had taken three.
    if (processorRef.getAuthorSettingsGeneration() != dcoKnobsGen_)
    {
        // No disclaimer where there is nothing to disclaim: an authoring that
        // never had a landable setting reads as "took nothing", which stays true
        // across any number of patches.
        dcoTraceView.setKnobs(dcoKnobsAsked_, dcoKnobsRefused_,
                              dcoKnobsAsked_.isEmpty()
                                  ? juce::String()
                                  : juce::String("a different patch has been loaded - "
                                                 "this is no longer on the synth"));
        // Drawn ONCE and then final: the record cannot become live again, and a
        // poll that kept matching would relayout the card ten times a second for
        // the rest of the session.
        dcoKnobsKnown_ = false;
        return;
    }

    T5ynthProcessor::AuthorKnobStand stand{};
    auto lines = processorRef.describeAuthorSettings(stand);
    juce::String pending;
    switch (stand)
    {
        case T5ynthProcessor::AuthorKnobStand::onThePatch:
            break;   // live: the lines are what stands there
        case T5ynthProcessor::AuthorKnobStand::notSounding:
            pending = "another oscillator is playing - nothing of this instrument "
                      "stands on the patch";
            break;
        case T5ynthProcessor::AuthorKnobStand::switchOff:
            pending = "KNOBS is off - this is what the instrument asks of the synth, "
                      "and it goes on the patch the moment you switch it on";
            break;
    }
    // The record half: kept so the station can still show what was authored once
    // the live half stops being about this sound.
    if (! lines.isEmpty())
        dcoKnobsAsked_ = lines;
    dcoTraceView.setKnobs(lines, dcoKnobsRefused_, pending);
}

// Route status/error text into both the logical holder (dcoStatusLabel — kept
// for the many call sites that already write it) and the visible trace view,
// which doubles as the LCO status/error channel until an actual trace exists.
void PromptPanel::setLcoStatus(const juce::String& text, const juce::String& tooltip, bool busy)
{
    dcoStatusLabel.setText(text, juce::dontSendNotification);   // keep the logical status holder
    // A status REPLACES the trace (the view dims it as a status line, never as a
    // reading). That replacement is the point: the trace on screen describes an
    // orchestra that is being superseded, or an attempt that just failed, and
    // leaving it standing would let it read as the current one.
    dcoTraceView.setStatus(text, busy);
    dcoTraceView.setTooltip(tooltip);
    // A new state supersedes the previous orchestra's compile report too — the
    // RUNNING station must not keep claiming "compiled" over a failed attempt.
    // (setStatus clears it in the view; this keeps dcoFlagsLabel in step.)
    dcoFlagsLabel.setText({}, juce::dontSendNotification);
    dcoFlagsLabel.setTooltip({});
    // Any status write replaces the card, so a self-check still in flight no
    // longer describes what is on screen — including the case this exists for: a
    // REJECTED generate attempt ("prompt is empty") never reaches the bump inside
    // triggerDcoBake, and without this the late finding would silently overwrite
    // the error the user was just shown.
    ++dcoBakeSeq_;
    dcoSelfCheck_.clear();
    // The card is gone, so the station it carried has no trace to sit in and the
    // poll above must stop writing one. (The processor keeps the settings: the
    // sound that asked for them is still playing.)
    dcoKnobsKnown_ = false;
    dcoKnobsAsked_.clear();
    dcoKnobsRefused_.clear();
}

// The compile window's report — see the declaration comment. Writes the label
// holder and the trace's RUNNING station together, but it is NOT the only
// writer of either: LcoTraceView::setTrace()/setStatus() reset the station's
// state to Unknown from their own call sites, and setLcoStatus clears the
// label. That is why pollCsoundPerformanceEnded() keys its precedence on
// dcoTraceView.compileState() — the view's own truth — never on a record of
// what this function last wrote.
void PromptPanel::setLcoCompileState(LcoTraceView::CompileState state, const juce::String& detail)
{
    juce::String label;
    switch (state)
    {
        case LcoTraceView::CompileState::Compiling: label = "compiling..."; break;
        case LcoTraceView::CompileState::Error:     label = detail;         break;
        case LcoTraceView::CompileState::Ok:
        case LcoTraceView::CompileState::Unknown:
        case LcoTraceView::CompileState::Ended:     break;   // nothing to report on a line
    }
    dcoFlagsLabel.setText(label, juce::dontSendNotification);
    dcoFlagsLabel.setTooltip(state == LcoTraceView::CompileState::Error ? detail : juce::String());
    dcoTraceView.setCompileState(state, detail);
}

// Phase 5 compile-window poll (SPEC_phase4_5_csound_llm_preset.md) — see its
// declaration comment in PromptPanel.h. Called every tick from the panel's
// existing 10Hz timerCallback(); a cheap no-op unless triggerDcoBake() just
// opened a window. Combines csoundCompileInFlight()/csoundSwapPending()/
// csoundSwapFading() (busy signals covering BOTH the fade path and the
// "instant adopt, no ready active engine" path — the latter never touches
// swapPending/swapFading at all, only compileInFlight) with a short grace
// period so a tick landing in the gap between requestCsoundOrchestra()'s
// triggerAsyncUpdate() and handleAsyncUpdate() actually running never
// misreads "hasn't started yet" as "done, no error".
void PromptPanel::pollCsoundCompile()
{
    if (! csoundCompileWatching_)
        return;

    // Leaving Csound mode mid-compile/fade (engine button, DAW automation, the
    // XL) legitimately DEFERS processBlock's swap-consumption until Csound mode
    // is re-entered — that is correct behavior, but it means csoundSwapPending()/
    // csoundSwapFading() stay true indefinitely while parked outside Csound, so
    // the busy check below would otherwise report "compiling..." forever; read
    // the engine mode the same way the rest of this panel reads APVTS state and
    // resolve the watch immediately once we're no longer in Csound mode.
    const int engineModeNow = static_cast<int>(processorRef.getValueTreeState()
                                  .getRawParameterValue(PID::engineMode)->load());
    if (engineModeNow != EngineMode::Csound)
    {
        csoundCompileWatching_ = false;
        // UNKNOWN, not Ok. This window is abandoned, not resolved: the swap is
        // merely deferred, csoundCompileError() may well be non-empty, and
        // nothing re-opens a watch on re-entering Csound mode. Reporting
        // "compiled" here would be a success nobody observed.
        setLcoCompileState(LcoTraceView::CompileState::Unknown);
        return;
    }

    const bool busyNow = processorRef.csoundCompileInFlight()
                       || processorRef.csoundSwapPending()
                       || processorRef.csoundSwapFading();

    if (busyNow)
    {
        csoundCompileSeenBusy_ = true;
        setLcoCompileState(LcoTraceView::CompileState::Compiling);
        return;   // still going — check again next tick
    }

    // Not busy this tick. If we've already seen it busy at least once, this
    // IS the real completion edge — resolve now. Otherwise this could simply
    // be a tick that landed before the background compile even started
    // (real Csound compiles run ~100-400ms, comfortably inside a single 10Hz
    // tick — see handleAsyncUpdate's own "~100ms compile" comment — so most
    // windows DO catch a busy tick; this grace period only covers the rare
    // miss): keep reporting "compiling..." until a short wall-clock grace
    // (900ms — generously above the expected compile time) expires, then
    // resolve anyway rather than hang the status forever.
    constexpr double kGraceMs = 900.0;
    if (! csoundCompileSeenBusy_
        && (juce::Time::getMillisecondCounterHiRes() - csoundCompileWatchStartMs_) < kGraceMs)
    {
        setLcoCompileState(LcoTraceView::CompileState::Compiling);
        return;
    }

    csoundCompileWatching_ = false;

    const juce::String err = processorRef.csoundCompileError();
    if (err.isNotEmpty())
    {
        setLcoCompileState(LcoTraceView::CompileState::Error, err);
        // The swap FAILED: the engine still plays the previous orchestra. An
        // in-flight self-check rendered the NEW text, so its finding describes a
        // sound nobody can hear — drop it. This is the one invalidation that comes
        // from the engine rather than from the UI.
        ++dcoBakeSeq_;
        dcoSelfCheck_.clear();
    }
    else
    {
        setLcoCompileState(LcoTraceView::CompileState::Ok);
    }
}

void PromptPanel::pollCsoundPerformanceEnded()
{
    // The compile watch owns the RUNNING station while it is open, and a shown
    // Error keeps its diagnostic (the blunter "engine stopped" adds nothing a
    // regenerate would not fix either way). This poll only fills the gaps: it
    // raises Ended when nothing else claims the station, and clears it back to
    // Unknown -- "compile not observed", deliberately not Ok, nobody observed a
    // compile -- once a recompile (regenerate, or any re-prepare: the engine
    // refuses the same-text early-out while latched) has revived the engine.
    if (csoundCompileWatching_)
        return;
    const int engineModeNow = static_cast<int>(processorRef.getValueTreeState()
                                  .getRawParameterValue(PID::engineMode)->load());
    const bool inCsound = (engineModeNow == EngineMode::Csound);
    const bool ended = inCsound && processorRef.csoundPerformanceEnded();
    // Precedence keys on the VIEW's shown state, never a shadow copy of the
    // last write: setTrace()/setStatus() reset the station to Unknown without
    // passing through setLcoCompileState, and a shadow copy that misses those
    // resets would pin the "already shown" guard on a state nobody displays —
    // a latched engine could then never be reported again. Reading the view
    // also makes the raise self-healing: whatever wipes the station, the next
    // tick re-raises Ended for as long as the engine really is latched.
    const auto shown = dcoTraceView.compileState();
    if (ended
        && shown != LcoTraceView::CompileState::Ended
        && shown != LcoTraceView::CompileState::Error)
        setLcoCompileState(LcoTraceView::CompileState::Ended);
    else if (! ended && shown == LcoTraceView::CompileState::Ended)
        setLcoCompileState(LcoTraceView::CompileState::Unknown);
}

// ──────────────────────────────────────────────────────────────────────────────
// Csound orchestra authoring (SPEC_phase4_5_csound_llm_preset.md, Phase 4):
// the LCO GENERATE trigger's ACTIVE body. The prompt routes through the SAME
// coder/interpreter model the retired wavetable-bake path used (backend/
// csound_author.py — one constrained instruct call against the fixed
// TOOLS/CHARACTERS/MOTION/ENVELOPES lexicon, at most one retry on a
// parse/assembly failure, LLM-first — never a keyword-matched or default
// orchestra), returning either a
// ready-to-compile orchestra + a human "how it was heard" reading, or an
// honest failure. On success the engine is forced into Csound mode (reusing
// dcoPrevEngineMode_ exactly like the retired wavetable path did for Lco —
// see forceCsoundEngineMode's own comment) and the orchestra is handed to
// requestCsoundOrchestra(); a compile-window poll (pollCsoundCompile, driven
// by the panel's own existing 10Hz Timer — see its declaration comment)
// tracks compiling -> ok/error to completion.
// ──────────────────────────────────────────────────────────────────────────────
void PromptPanel::triggerDcoBake()
{
    // csound_author.py's backend handler resolves the SAME installed model this
    // flag tracks (_resolve_coder_model_dir), so "language model not installed"
    // is the exact failure mode blocking this trigger.
    if (! llmAvailable_)
    {
        setLcoStatus("Load the language model in Settings");
        // The cadence armed this step; it will not reach the cache, and an arm
        // left standing stamps whichever LATER pass happens to land.
        lroTakeStepArmed_ = false;
        return;
    }

    // Forcing the engine into Csound mode + swapping its live orchestra is the
    // same kind of engine-state change a running tape must not race — mirrors
    // the retired path's identical replay guard.
    if (processorRef.isReplayActive())
    {
        if (onStatusChanged) onStatusChanged(juce::String::fromUTF8(
            "replay running \xe2\x80\x94 stop it to craft"), false);
        lroTakeStepArmed_ = false;
        return;
    }

    if (dcoBaking_)
        return;
    // Mirrors triggerDcoReprompt's own gate on dcoBaking_ (line ~3243): the
    // Return-key and Launch Control XL paths both bypass the disabled
    // on-screen GENERATE button, so without this check a bake could launch
    // while a reprompt is still in flight — two detached threads racing the
    // one serialized pipe, and the reprompt's tail would re-enable the button
    // mid-bake and rewrite the prompt editor out from under it (adversarial
    // review finding).
    if (dcoRepromptBusy_)
    {
        setLcoStatus("Still rewriting the prompt");
        return;
    }
    // The pipe is one serialized channel (recursive stateMutex_): authoring
    // clicked mid-generation would just park behind it for minutes with a
    // misleading "authoring..." label. Same gate set as the retired bake /
    // triggerGeneration.
    if (generating || translatingPrompts_ || loopStepInFlight_)
    {
        setLcoStatus("Still generating");
        lroTakeStepArmed_ = false;
        return;
    }
    // A full cache is AUDITIONED, not authored again - the same rule the neural
    // Generate follows, and with no exception for a running Re-Prompt stance:
    // the depth the player set IS the budget (BJ, 22.08.2026).
    //
    // Asked of the LRO cache directly, NOT of pressWouldReplayCache(): that one
    // answers for the panel in front, which is right for a button label and
    // wrong here. This function is an LRO operation reached asynchronously - the
    // Re-Prompt tail calls it after a listen and two model turns - and the
    // oscillator toggle stays live throughout, so by the time it runs the panel
    // may be the neural one and the panel-shaped answer would be about the
    // wrong cache entirely.
    if (processorRef.isCsoundCacheFull())
    {
        lroTakeStepArmed_ = false;
        playNextCachedCsound();
        return;
    }

    auto pipePtr = processorRef.getPipeInferencePtr();
    if (pipePtr == nullptr)
    {
        setLcoStatus("The synthesis helper is not running");
        lroTakeStepArmed_ = false;
        return;
    }
    const auto text = dcoPromptEditor.getText().trim();
    if (text.isEmpty())
    {
        setLcoStatus("Type what the instrument should sound like");
        lroTakeStepArmed_ = false;
        return;
    }

    dcoBaking_ = true;
    if (onLcoBusyChanged) onLcoBusyChanged(true);   // disable the reused GENERATE button
    // The first phase, named for what it is: before the first token can
    // stream, the author has to evaluate its whole prompt — the library
    // selection, tens of seconds on this model — and during that there is
    // nothing to show BECAUSE nothing exists yet. The first streamed frame
    // flips the line to "Writing the instrument" (see onThinking/onBody), and
    // each repair round renames it again (onAttempt) — one line that always
    // says which phase the minutes belong to (BJ 2026-07-24).
    setLcoStatus("Reading the library", {}, /*busy=*/true);

    // One blocking IPC round-trip (may lazily load the instruct model) on a
    // detached background thread — house pattern (triggerGeneration,
    // triggerDcoReprompt, the retired bake above): only the UI-visible
    // completion marshals back via SafePointer + callAsync.
    juce::Component::SafePointer<PromptPanel> safeThis(this);
    const unsigned long long bakeSeq = ++dcoBakeSeq_;
    // Read off the APVTS HERE — this is the message thread; the worker below is
    // not, and juce::var is not something to build off it.
    //
    // The shelf goes out ALWAYS, and deliberately not only when the KNOBS switch
    // is on (BJ, 2026-08-01: "Ist der Button deaktiviert, kann dennoch eine
    // virtuelle Einstellung vorgenommen werden. Sie wird halt nur erst aktiv
    // wenn user knobs aktiviert"). The switch says what may be TOUCHED, not what
    // may be WRITTEN: withholding the shelf made the author write a different,
    // smaller instrument — one with no filter and no envelopes — so turning the
    // switch on afterwards had nothing to turn on. Now every authoring describes
    // its whole sound and the switch decides when that reaches the patch.
    const juce::var synthParams = processorRef.buildAuthorParamIndex();
    std::thread([safeThis, pipePtr, text, bakeSeq, synthParams]() mutable
    {
      // Publishing ONE attempt — engine swap, card, Re-Prompt bookkeeping. The
      // first authoring and every correction publish identically (a correction IS
      // the sound now, not a preview), so this is one lambda rather than two
      // copies that could drift apart.
      auto publish = [safeThis, text]
                     (const PipeInference::CsoundAuthorResult& authored,
                      int attempt, bool moreToCome)
      {
        juce::MessageManager::callAsync([safeThis, authored, text, attempt, moreToCome]()
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;   // panel gone — nothing to write

            // Stay BUSY while the self-check and any correction still hold the
            // serialized inference pipe. Clearing it here would re-enable GENERATE
            // over a pipe that is still occupied for a CLAP analyze plus an
            // uncapped LLM turn, and the next press would park behind it showing
            // "authoring..." while doing nothing — the exact misleading state
            // triggerDcoBake's own gates exist to prevent. The loop's single exit
            // clears it instead.
            if (! moreToCome)
            {
                self->dcoBaking_ = false;
                if (self->onLcoBusyChanged) self->onLcoBusyChanged(false);
            }

            if (! authored.success)
            {
                // Honest failure (LLM-first, no fallback: csound_author.py
                // exhausted its one retry without ever falling back to a
                // default/keyword-matched orchestra) — leave the engine and
                // any previously-authored orchestra completely untouched.
                self->setLcoStatus(authored.errorMessage.isNotEmpty()
                                       ? authored.errorMessage
                                       : juce::String("Could not write an instrument for this prompt"));
                // Nothing was authored, so nothing reached the cache and this
                // step's stamp belongs to no entry - see the ear tail in
                // triggerDcoReprompt for what a flag left armed would mark.
                self->lroTakeStepArmed_ = false;
                return;
            }

            // The trace: every station is a record the write-path kept on the way
            // to this orchestra (backend/lco_write.py). Nothing is composed here
            // and nothing was asked of the author — its system prompt still
            // forbids explanation (BJ 2026-07-24: "nur was ohnehin passiert
            // ist"). The Csound orchestra is one combined authored voice, not a
            // dual A/B oscillator split (retired — BJ 2026-07-17: "this split is
            // dead"), so there is one trace, not two.
            //
            // The authored BODY is deliberately NOT shown here any more. It is
            // stashed on the processor (setCsoundParamsText, below) and becomes
            // the back of this card.
            //
            // NOTE: the deprecated self-check loop's progress line ("listening to
            // what was built...", written while moreToCome held) has no station.
            // That loop is compiled out (T5YNTH_LCO_SELFCHECK 0) and cannot reach
            // here with moreToCome set; reviving it means giving it a slot rather
            // than borrowing one that means something else.
            juce::ignoreUnused(attempt, moreToCome);

            LcoTraceView::Trace tr;
            tr.prompt             = text;
            tr.model              = authored.authorModel;
            tr.reading            = authored.reading;
            tr.thinking           = authored.thinking;
            tr.namedInstruments   = authored.namedInstruments;
            tr.namedAdjectives    = authored.namedAdjectives;
            tr.namedMotions       = authored.namedMotions;
            tr.openedInstruments  = authored.openedInstruments;
            tr.openedAdjectives   = authored.openedAdjectives;
            tr.openedMotions      = authored.openedMotions;
            tr.libraryEntryCount  = authored.libraryEntryCount;
            // A consultation was RECORDED for this bake — which is what lets the
            // OPENED station distinguish "the author asked for nothing" (a
            // finding worth showing) from "nobody recorded a consultation" (a
            // recall, where the station is left out entirely). The library size
            // is the one field a consultation block always carries.
            tr.consultationKnown  = authored.libraryEntryCount > 0;
            // The KNOBS station is NOT filled here: what the author asked for and
            // what actually landed are two different lists, and only the second
            // one is worth showing. It is set below, from the return of the apply.
            tr.repairs            = authored.repairs;
            tr.attempts           = authored.attempts;
            self->dcoTraceView.setTrace(std::move(tr));
            self->dcoTraceView.setTooltip({});
            self->dcoStatusLabel.setText("LRO: csound authored", juce::dontSendNotification);

            // Re-Prompt bookkeeping (docs/DCO_REPROMPT_CONCEPT.md): the
            // chain reads its own last reading/flags to build the next
            // stance turn, exactly like the retired bake fed it — Csound has
            // no per-word flags concept, so the flags line is simply empty.
            // Name the model that actually wrote this orchestra.
            self->setLcoAuthorModel(authored.authorModel);

            self->dcoLastMachineReading_ = authored.reading;
            self->dcoLastFlagsLine_ = {};
            self->dcoEarFailed_ = false;   // a fresh orchestra earns a fresh listen
            if (text != self->dcoLoopLast_)
            {
                self->dcoLoopLast_ = text;
                self->dcoLoopRecent_.clearQuick();
                self->dcoLoopRecent_.add(text);
            }

            // Force Csound mode (stash/restore via dcoPrevEngineMode_, EXACTLY
            // the mechanism the retired path used for Lco — see
            // forceCsoundEngineMode's own comment) and hand off the compiled
            // orchestra text. requestCsoundOrchestra() always queues + returns
            // true; the actual compile runs on the processor's own background
            // thread (handleAsyncUpdate), tracked below.
            //
            // Only while the LCO is still the ACTIVE paradigm: authoring takes
            // seconds to minutes, and the toggle stays live throughout, so a
            // user who has moved on to T5osc in the meantime would otherwise get
            // the engine yanked into Csound under a neural panel by a bake they
            // already left behind. The orchestra is handed over either way — it
            // is what switching back to the LCO then sounds.
            if (! self->easyMode_)
                self->processorRef.forceCsoundEngineMode();
            self->processorRef.requestCsoundOrchestra(authored.orchestra);
            self->processorRef.setCsoundPrompt(text);   // what was asked for, saved with it
            self->processorRef.setCsoundReading(authored.reading);
            self->processorRef.setCsoundParamsText(authored.paramsText);
            // The knobs this instrument gives the player, and their starting
            // positions. NOT gated on the KNOBS switch below: that switch is
            // about the player's own patch (filter, envelopes, LFOs), while
            // these twelve belong to the body that was just written and to
            // nothing else — there is nothing of the player's to borrow here.
            self->processorRef.setCsoundControls(
                LroControls::fromVar(authored.controls), /*applyValues=*/true);
            // The knobs the author asked of the synth itself. Handed over
            // WHOLE, whichever way the KNOBS switch stands: the processor keeps
            // the request and puts it on the patch only while the player allows
            // it and this instrument is the sounding oscillator (see
            // setAuthorSettings / reconcileAuthorSettings). So a sound written
            // with the switch off is complete and waits, and turning the switch
            // on later is what makes it audible — instead of the request being
            // dropped and the previous sound's staying on record in its place.
            self->processorRef.setAuthorSettings(authored.settings);

            // ...and the same result into the LRO's cache. Every authoring pass
            // offers its result here; the cache takes it while one is recording
            // and refuses it otherwise, exactly as the neural side does with a
            // landed render. A Re-Prompt run is what makes this worth having:
            // it leaves its whole series behind, and the CACHE row then travels
            // it without asking the model for any of it a second time.
            {
                T5ynthProcessor::CsoundCacheEntry slot;
                slot.orchestra   = authored.orchestra;
                slot.prompt      = text;
                slot.reading     = authored.reading;
                slot.paramsText  = authored.paramsText;
                // Whoever wrote THIS one. A replay puts the name back on the
                // model tab with the orchestra it belongs to, instead of leaving
                // the last bake's name standing over somebody else's work.
                slot.authorModel = authored.authorModel;
                slot.controls    = authored.controls;
                slot.settings    = authored.settings;
                // The arm belongs to THIS pass and is spent here whatever the
                // cache did with the result. Left standing when the entry is
                // refused - the depth changed mid-pass, the cache filled or was
                // switched off - it would mark whichever later pass happened to
                // land, a hand-played one included.
                const bool armed = self->lroTakeStepArmed_;
                self->lroTakeStepArmed_ = false;
                if (self->processorRef.addCsoundCacheEntry(slot))
                {
                    // A recording is a recording only while EVERY entry in it is
                    // one. The first sets the flag; each further one can keep it
                    // where it stands but never raise it again - so a live entry
                    // (a manual press, Manual cadence, the switch off) makes the
                    // cache a mixture for good, instead of the last entry alone
                    // deciding what the whole run was.
                    //
                    // The next step follows from HERE - from the code being
                    // finished, not from a clock (see isLroAsyncTakeRecording):
                    // the cadence skips its interval entirely while a take
                    // records.
                    const bool first = self->processorRef.getCsoundCacheFillCount() <= 1;
                    self->processorRef.setCsoundCacheOfflineTake(
                        armed && (first || self->processorRef.isCsoundCacheOfflineTake()));
                }
            }

            // The KNOBS station. What LANDED is read back off the parameters by
            // the apply, so a line the backend passed and this side then refused
            // cannot be reported as set; what is only WAITING is shown as the
            // request. Everything that will never land is listed as refused,
            // with the reason.
            self->adoptAuthorSettingsStation(authored.settings);
            self->dcoTraceView.setBody(authored.paramsText);   // the back of the card

            // The engine now holds a new, unsaved sound — drop the loaded/
            // last-saved preset identity (same reason and same siting as the
            // neural render). Sited at the PUBLISH, not at the trigger where it
            // used to sit: authoring fails asynchronously (dead subprocess,
            // backend error, timeout) and that failure branch deliberately
            // leaves the previously authored orchestra playing — a preset that
            // is still sounding must keep its name.
            if (self->onNewGenerationStarted) self->onNewGenerationStarted();

            // Open the compile-window poll: pollCsoundCompile (called every
            // tick from the panel's existing 10Hz timerCallback) reports
            // compiling -> ok/error via dcoFlagsLabel until this request
            // resolves. Shared with MainPanel's SNAP recall so a recalled
            // orchestra reports its compile the same way (beginCsoundCompileWatch).
            self->beginCsoundCompileWatch();
        });
      };

        // ── SELF-CHECK DEACTIVATED — DEPRECATED (BJ 2026-07-21) ──────────────────
        // "self check ist eine katastrophe": the self-listen / describe / compare /
        // correct loop is switched OFF. A bake now authors the orchestra ONCE and
        // plays it — no bare-oscillator probe, no CLAP description, no comparer
        // verdict, no correction pass, and no "Self-check" card section. The loop is
        // retained verbatim under the (compiled-out) T5YNTH_LCO_SELFCHECK switch so
        // the machinery is preserved-and-marked, not deleted — see the #else path for
        // the active bake. Every helper it drives carries a matching DEPRECATED
        // banner: RepromptStances::{syspSelfCheck, composeHeardDescription,
        // buildSelfCheckUserTurn, selfCheckReportsMismatch}, PromptPanel::
        // formatSelfCheck, CsoundEngine::renderBareOscillator, and the backend
        // build_csound_response correction branch (_CS_CORRECTION_RULE).
#if T5YNTH_LCO_SELFCHECK
        // ── Listen, compare, correct ────────────────────────────────────────────
        // Each pass: author → publish (the sound is playable NOW) → render it bare
        // → let the listener describe it → let a second model compare description
        // and request. A mismatch feeds the FINDING back into the author as a
        // correction brief and the pass repeats, up to kMaxSelfCorrections.
        //
        // What is NOT here: choosing the best among the attempts. That needs a
        // proximity index, and the index needs a reference BJ has not handed over
        // yet. Until then the loop simply stops at the first attempt the comparer
        // does not accuse — so the accepted configuration is one that matched, not
        // one that scored highest.
        //
        // ONE exit on purpose. Every path out still has to hand the busy flag
        // back, so the steps write into locals and fall through to a single final
        // callAsync — a check that cannot run must not leave the panel wedged.
        juce::String description, finding, correction, previousReading;

        for (int attempt = 0; attempt <= kMaxSelfCorrections; ++attempt)
        {
            auto authored = pipePtr->authorCsoundOrchestra(text, correction, previousReading,
                                                           synthParams);

            const bool canContinue = authored.success && authored.orchestra.isNotEmpty();

            // A CORRECTION that fails is not a failed bake. The sound from the
            // previous pass is loaded and playing; publishing this result would
            // replace a good card with "authoring failed" and tell the user the
            // bake broke when only the improvement did. Leave with what plays, and
            // with the finding that describes it. (Measured: a correction pass can
            // come back "no synthesis idiom matched" where the first pass mapped
            // cleanly.) At attempt 0 there is nothing to fall back to, so the
            // failure is the result and is published as one.
            if (! canContinue && attempt > 0)
                break;

            publish(authored, attempt, canContinue);
            if (! canContinue)
                return;   // busy was already released by publish()

            description.clear();
            finding.clear();

            // NOTE: this thread reaches the probe before the message thread has run
            // publish()'s callAsync far enough to start the live orchestra swap, and
            // both take the same process-wide Csound lifecycle lock. The probe
            // therefore holds that lock for its own create/compile/start (~50-165 ms
            // measured) while the swap waits, so the new sound becomes audible that
            // much later. The render itself no longer holds it (see
            // renderBareOscillator). Ordering the two properly needs a message-thread
            // handshake this thread has no safe way to do — SafePointer and
            // processorRef are message-thread-only, and every access here obeys that.
            //
            // The BARE oscillator (BJ's choice): the Csound output alone, no ADSR and
            // no filter. What is under examination is the OSCILLATOR's reading of the
            // prompt, not what a patch later does to it.
            const auto samples = CsoundEngine::renderBareOscillator(authored.orchestra.toStdString());
            if (! samples.empty())
            {
                juce::AudioBuffer<float> probe (1, (int) samples.size());
                std::memcpy(probe.getWritePointer(0), samples.data(), samples.size() * sizeof(float));

                const auto heard = pipePtr->analyze(probe, 48000.0, 5, {});
                if (heard.success)
                {
                    // The listener has now DESCRIBED the sound. A second model
                    // COMPARES that description with the request: two texts, one
                    // language task — the audio is not part of the comparison. The
                    // SAME string goes to the card below, so the user reads exactly
                    // what the comparison had to work with.
                    //
                    description = RepromptStances::composeHeardDescription(heard.tags,
                                                                           heard.spectral);
                    auto verdict = pipePtr->interpret(
                        RepromptStances::stanceSystemPrompt("selfcheck"),
                        RepromptStances::buildSelfCheckUserTurn(text, description),
                        0, {});
                    if (verdict.success)
                        finding = verdict.text.trim();
                }
            }

            // No accusation — either it matched or the check could not run. Either
            // way there is nothing to repair, so stop and keep this sound.
            if (! RepromptStances::selfCheckReportsMismatch(finding))
                break;

            // The finding, not a rewritten prompt, is what goes back in: the author
            // re-reads the ORIGINAL request under a correction brief. Rewriting the
            // user's words would be an unauthorized sound-shaping act. The reading
            // travels with it so the brief has a patch to repair rather than a
            // complaint to guess against.
            correction      = finding;
            previousReading = authored.reading;
        }

        juce::MessageManager::callAsync([safeThis, bakeSeq, description, finding]()
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;

            self->dcoBaking_ = false;
            if (self->onLcoBusyChanged) self->onLcoBusyChanged(false);

            // Drop the finding if it no longer describes what is loaded. Any later
            // bake, preset load or status write bumps the counter — see
            // dcoBakeSeq_'s declaration. Note the busy flag above is released
            // regardless: it belongs to THIS thread and must not leak.
            if (bakeSeq != self->dcoBakeSeq_) return;
            self->dcoSelfCheck_ = formatSelfCheck(description, finding);
            if (self->dcoSelfCheck_.isEmpty()) return;

            // The finding is HELD but not drawn: the trace has no self-check
            // station, and the surface this used to write into (the HEARD AS text
            // box, rebuilt as reading + parametrisation + finding) no longer
            // exists. Reviving this loop therefore means giving the finding a
            // station of its own in LcoTraceView — it must NOT be smuggled into
            // WROTE, which reports what the AUTHOR said, while a finding is what a
            // SECOND model said about the author's sound. Kept as a hold rather
            // than deleted so re-enabling the switch still compiles and the gap
            // is visible at exactly the line that has to close it.
        });
#else
        // ── Deactivated bake: author once, publish, done ────────────────────────
        // publish() with moreToCome=false releases the busy flag on BOTH success
        // and honest failure, so this single call is the whole body — no probe, no
        // listen, no correction. bakeSeq is still bumped at the call site to
        // invalidate any stale in-flight card; nothing here consumes it now.
        juce::ignoreUnused(bakeSeq);
        // The authoring, while it is being written — the reasoning, the code,
        // and each repair round. All three arrive on THIS thread, so they
        // marshal like every other UI-visible result; the view's live setters
        // stop accepting them the moment the finished trace lands, which is
        // what keeps a late frame from overwriting the attempt that actually
        // compiled.
        //
        // The status line follows the phases: a first-attempt frame means the
        // prompt evaluation is over and the author is writing ("Reading the
        // library" -> "Writing the instrument"); a repair frame names its round
        // and what it was sent back for. Only attempt-1 frames flip the line —
        // during a repair the caption belongs to onAttempt, and a body frame
        // overwriting it would erase the one sight of the loop the user has.
        auto onThinking = [safeThis](int attempt, const juce::String& thinking)
        {
            juce::MessageManager::callAsync([safeThis, attempt, thinking]()
            {
                if (auto* self = safeThis.getComponent())
                {
                    if (attempt == 1)
                        self->dcoTraceView.setStatusLine("Writing the instrument");
                    self->dcoTraceView.setLiveThinking(attempt, thinking);
                }
            });
        };
        auto onBody = [safeThis](int attempt, const juce::String& code)
        {
            juce::MessageManager::callAsync([safeThis, attempt, code]()
            {
                if (auto* self = safeThis.getComponent())
                {
                    if (attempt == 1)
                        self->dcoTraceView.setStatusLine("Writing the instrument");
                    self->dcoTraceView.setLiveBody(attempt, code);
                }
            });
        };
        auto onAttempt = [safeThis](int attempt, int maxTries, const juce::String& errors)
        {
            juce::MessageManager::callAsync([safeThis, attempt, maxTries, errors]()
            {
                if (auto* self = safeThis.getComponent())
                {
                    juce::String line("Repairing (attempt ");
                    line << attempt;
                    if (maxTries > 0)
                        line << " of " << maxTries;
                    line << ")";
                    if (errors.isNotEmpty())
                        line << ": " << errors;
                    self->dcoTraceView.setStatusLine(line);
                }
            });
        };
        // The synth's own knobs go out with the request ONLY when the player has
        // allowed the author to set them (PID::lcoSetsParams). With the switch
        // off the shelf is empty and the author is never told these controls
        // exist — it writes exactly the orchestra it writes today.
        auto authored = pipePtr->authorCsoundOrchestra(text, {}, {}, synthParams,
                                                       onThinking, onBody, onAttempt);
        publish(authored, /*attempt=*/0, /*moreToCome=*/false);
#endif
    }).detach();
}

void PromptPanel::setLcoRecalledTrace(const juce::String& prompt, const juce::String& reading,
                                      const juce::String& authorModel,
                                      const juce::String& csoundBody)
{
    ++dcoBakeSeq_;
    dcoSelfCheck_.clear();
    // A recalled preset carries no answer about the synth's own controls, so the
    // station is left out entirely rather than filled from whatever the live
    // processor happens to hold from an earlier authoring.
    dcoKnobsKnown_ = false;
    dcoKnobsAsked_.clear();
    dcoKnobsRefused_.clear();
    LcoTraceView::Trace t;
    t.prompt  = prompt;
    t.reading = reading;
    t.model   = authorModel;
    dcoTraceView.setTrace(std::move(t));

    // The back of the card follows the recall — but a preset written before
    // params_text meant the BODY stores a plain-language account instead, and
    // that is not code to edit. When what came back is not Csound, it goes to
    // the front as a reading and the code is recovered from the orchestra the
    // engine is actually running, which every preset carries.
    if (LcoTraceView::looksLikeCsound(csoundBody))
        dcoTraceView.setBody(csoundBody);
    else
        dcoTraceView.setBody(bodyFromOrchestra(processorRef.getCsoundOrchestraText()),
                             csoundBody);
}

// The authored body, cut back out of the wrapped orchestra. The scaffold around
// it is fixed (backend/lco_write.py _HEAD/_TAIL) and its last and first own
// lines are the boundary: everything between the host's note counter and its
// output stage is what somebody wrote. Returns empty if either marker is
// missing, which is the honest answer — better an empty page than sixty lines
// of scaffold nobody authored and step 3 must not let anyone edit.
juce::String PromptPanel::bodyFromOrchestra(const juce::String& orchestra)
{
    static const juce::String kAfterHead ("knote    = knote + 1/kr");
    // Prefix common to BOTH tail generations: the current one ("asig * kgate *
    // kpresGain", kvel removed 2026-07-25 — velocity is the envelope's, §4 one
    // loudness) and the old one still stored inside pre-change presets
    // ("asig * kgate * kvel * ..."). Anchoring on the full old line made every
    // NEW orchestra unparseable here and blanked the trace card's code page.
    static const juce::String kBeforeTail("aout     = asig * kgate");
    const int a = orchestra.indexOf(kAfterHead);
    const int b = orchestra.indexOf(kBeforeTail);
    if (a < 0 || b <= a)
        return {};
    return orchestra.substring(a + kAfterHead.length(), b).trim();
}

// ──────────────────────────────────────────────────────────────────────────────
// DCO Re-Prompt step (docs/DCO_REPROMPT_CONCEPT.md)
// ──────────────────────────────────────────────────────────────────────────────
// One STEP = listen, then one interpret() call under the selected stance.
//
// THE EAR (BJ 2026-07-28, concept doc's Nachtrag of that date). This loop used to
// read the author's own READING line instead of hearing anything — "lesen ->
// deuten -> umformulieren". That rested on a self-description which no longer
// exists: it was written for the retired lexicon router, whose vocabulary was
// closed and pre-heard, so reading a recipe meant looking a sound up in a list of
// already-heard things. Authored Csound is open — nobody heard the code before it
// ran — so the LCO is now in exactly the neural panel's position, and it listens
// the same way the neural panel does (runSemanticLoopStep): CLAP top-5 timbre
// tags + spectral words of the sound that came out.
//
// CLAP IS ON LOAN. It ranks against a fixed candidate vocabulary and cannot
// caption freely, so a self-authored texture gets folded back onto a curated tag
// register — BJ: "if any, clap wäre ungeeignet"; the audio-LLM is the real
// candidate. Provisional by decision, not by oversight.
//
// NOT the self-check. Nothing here judges the sound against the request and
// nothing corrects the author (T5YNTH_LCO_SELFCHECK stays 0). The description is
// MATERIAL for a stance the user picked, never a verdict.
//
// A step that cannot listen does not step. There is no fallback to the author's
// READING line: substituting its account of its own code would hand the model a
// claim under a label that says the instrument was heard. The neural loop bails
// the same way on an empty render ("nothing to listen to").
// Manual STEP only — no auto-loop, no A/B poles (v1, see the concept doc's
// "was bewusst NICHT kopiert wird").
void PromptPanel::triggerDcoReprompt()
{
    // Message-thread gates, mirroring triggerDcoBake's gate order/shape. Every
    // early-out reports through dcoStatusLabel so the user always sees WHY a
    // click did nothing (house pattern: triggerDcoBake, triggerGeneration).
    if (dcoRepromptBusy_)
    {
        setLcoStatus("Still rewriting the prompt");
        return;
    }
    if (dcoBaking_)
    {
        // Words only, NOT setLcoStatus: a full status write clears the live
        // reasoning and code of the bake that is still running — the click
        // would blank the very stream the user is watching, and the caption
        // itself would be overwritten by the next arriving frame anyway.
        dcoTraceView.setStatusLine("Still writing the instrument");
        return;
    }
    // loopStepInFlight_ added to match triggerDcoBake's identical gate (line
    // ~2018): the neural loop's runSemanticLoopStep can be mid-flight with
    // `generating` already false (its own analyze+interpret round-trip runs
    // AFTER the render completes), and it is not gated by easyMode_ — so a user
    // can flip to Advanced and hit STEP while a background neural step is still
    // interpreting. Without this, STEP would proceed, serialize its interpret()
    // behind the neural one on the shared IPC pipe, then its own triggerDcoBake()
    // call would silently no-op on THIS same gate (editor text updated, table
    // not re-baked) — adversarial review finding.
    if (generating || translatingPrompts_ || loopStepInFlight_)
    {
        setLcoStatus("Still generating");
        return;
    }
    if (! llmAvailable_)
    {
        setLcoStatus("Rewriting the prompt needs the language model");
        return;
    }
    const int stanceIdx = static_cast<int>(processorRef.getValueTreeState()
                              .getRawParameterValue(PID::dcoRepromptStance)->load());
    if (stanceIdx == RepromptStance::Off)
    {
        setLcoStatus("Pick a stance first");
        return;
    }
    // The ear needs an orchestra to render, not a reading to quote (see
    // triggerLcoGenerate's matching gate).
    if (! processorRef.hasCsoundOrchestra())
    {
        setLcoStatus("Write an instrument first");
        return;
    }
    // A compile of the PREVIOUS orchestra can still be running on the processor's
    // own thread — publish() clears dcoBaking_ and opens the compile watch in the
    // same callAsync, so GENERATE becomes clickable exactly when that compile
    // begins. Both it and the probe take the process-wide Csound lifecycle lock, so
    // stepping into the window would serialise the probe's own create/compile/start
    // (~50-165 ms measured) against the engine's, delaying the sound the user is
    // waiting to hear — and the ear would then be listening to an orchestra the
    // engine has not finished swapping to. Wait for the window to close. (No GUI
    // stall either way: the probe runs detached, and the message-thread poll takes
    // a different mutex.)
    if (csoundCompileWatching_)
    {
        setLcoStatus("Still compiling the instrument");
        return;
    }

    const int sIdx = juce::jlimit(0, RepromptStance::kCount - 1, stanceIdx);
    const juce::String stanceKey = RepromptStance::kEntries[sIdx].key;

    // The chain's own last link — falls back to the live editor text on the
    // very first step. The DCO chain has no separate "human original" concept
    // to restore on deactivation (unlike the neural loop): a documented v1 seam,
    // docs/DCO_REPROMPT_CONCEPT.md "Ein Feld, keine Historie sichtbar".
    const juce::String prev = dcoLoopLast_.isNotEmpty() ? dcoLoopLast_ : dcoPromptEditor.getText().trim();
    const juce::StringArray recent = dcoLoopRecent_;
    const juce::String flags = dcoLastFlagsLine_;
    const juce::String vocab = dcoReferenceVocab_;   // scanner palette (may be empty)
    // What the ear listens to: the orchestra the panel last requested. Captured
    // HERE because processorRef is message-thread-only — the background thread
    // below never touches it (same rule the retired self-check probe obeyed).
    const juce::String orchestra = processorRef.getCsoundOrchestraText();
    // TWO RATES, and the whole point is that they differ — the live signal path has
    // them both too.
    //
    // RENDER at the rate the body is WRITTEN for. The engine compiles the authored
    // text at sampleRate * the LRO oversampling factor (CsoundEngine::prepare) — 4x
    // by default — and an authored body may derive its own partial count or FM index
    // from `sr`. Rendering the probe at the bare host rate would not merely alias it
    // (1x is worse than the 2x this project measured as audibly dirty); it can
    // produce a DIFFERENT sound. effectiveOversampleFactor is the same policy the
    // engine reconcile uses, so the factor here is the one actually compiled.
    //
    // LISTEN at the rate the user HEARS. Everything the engine renders oversampled
    // is decimated back to the host rate before it reaches a speaker, and the ear
    // must be given that same band: the backend resamples for CLAP, but it computes
    // the spectral words on the native-rate signal against ABSOLUTE Hz thresholds
    // (centroid 500/1500/3500, low band under 250). Hand it 192 kHz and 24-96 kHz of
    // content the decimator throws away is folded into the description — measured on
    // this build: a warm, bass-heavy tone reads "bright, full-bodied", a plain saw's
    // centroid reads 14158 Hz where the audible signal is 4446 Hz. So the probe is
    // decimated here, before analyze(), and analyze() is told the host rate.
    const double hostRate = processorRef.getSampleRate() > 0.0
                                ? processorRef.getSampleRate() : 48000.0;
    const int    probeOs   = CsoundEngine::effectiveOversampleFactor(
                                 hostRate, processorRef.getLroOsFactor());
    // Clamped HERE with the probe's own ceiling, so listenRate is derived from what
    // was really rendered rather than from what was asked for: a host above the
    // ceiling (factor 1, nothing left to halve) would otherwise render clamped and
    // be labelled with its own rate, and analyze would measure against thresholds
    // shifted by that ratio. No standard rate reaches this; the arithmetic should
    // still be true at the one that does.
    const double probeRate  = juce::jmin(hostRate * (double) probeOs,
                                         CsoundEngine::kMaxProbeSampleRate);
    const double listenRate = probeRate / (double) probeOs;

    auto pipePtr = processorRef.getPipeInferencePtr();
    if (pipePtr == nullptr)
    {
        setLcoStatus("The synthesis helper is not running");
        return;
    }
    const juce::String device = defaultInferenceDevice_;

    dcoRepromptBusy_ = true;
    if (onLcoBusyChanged) onLcoBusyChanged(true);   // disable the reused GENERATE button
    setLcoStatus("Listening, then rewriting", {}, /*busy=*/true);

    // IPC on a detached background thread ONLY — never the message thread (JUCE
    // rule; house pattern: triggerDcoBake / runSemanticLoopStep).
    juce::Component::SafePointer<PromptPanel> safeThis(this);
    std::thread([safeThis, pipePtr, stanceKey, flags, prev, recent, vocab,
                 orchestra, probeRate, probeOs, listenRate, device]() mutable
    {
        // ── The ear ──────────────────────────────────────────────────────────
        // The BARE oscillator: the Csound output alone, no ADSR and no filter.
        // What is under examination is the OSCILLATOR's reading of the prompt, not
        // what a patch later does to it (BJ's choice, kept from the retired
        // self-check probe, as are 220 Hz / 3 s / gate off at 2.5 s). The RATE is
        // not a default — see probeRate above. Then analyze() exactly as the neural
        // loop calls it: top-5, default device.
        //
        // renderBareOscillator takes the process-wide Csound lifecycle lock for its
        // own create/compile/start (~50-165 ms measured); the render itself does
        // not. That lock is shared with every engine prepare()/teardown in the
        // process, not only this panel's bakes, so the caller's gates thin the
        // collisions (dcoBaking_, csoundCompileWatching_) without excluding them —
        // a SNAP recall or a DAW state restore can still take it underneath us.
        // Waiting is the correct behaviour there; the order is one-way, so it
        // cannot deadlock.
        juce::String heardTags, heardSpectral, earError;
        // Distinguishes "this ORCHESTRA has nothing to hear" from "the EAR was not
        // reachable" — the two failures deserve opposite answers, see the bail below.
        bool orchestraUnheard = false;
        // Rendered at probeRate, handed back at listenRate == probeRate/probeOs:
        // the probe runs the body at the rate it is written for and then decimates
        // through the engine's own halfband stages, so what the ear gets is the band
        // a player hears. A plain interpolator would NOT do — it does not adapt its
        // kernel to the ratio, and everything above the host Nyquist would fold back
        // in, which is the very error this exists to prevent.
        const auto samples = CsoundEngine::renderBareOscillator(
            orchestra.toStdString(), probeRate, /*freqHz=*/220.0, /*seconds=*/3.0,
            /*gateOffSeconds=*/2.5, /*decimateBy=*/probeOs);
        if (samples.empty())
        {
            // renderBareOscillator returns empty for a compile error, a broken
            // contract, a non-finite sample or a render that never left the noise
            // floor — all of them "this orchestra has no sound to describe", and it
            // has already logged which. The one to REPLACE, not to wait for.
            earError = "Could not listen to the instrument";
            orchestraUnheard = true;
        }
        else
        {
            juce::AudioBuffer<float> probe (1, (int) samples.size());
            std::memcpy(probe.getWritePointer(0), samples.data(),
                        samples.size() * sizeof(float));
            const auto heard = pipePtr->analyze(probe, listenRate, 5, {});
            if (heard.success)
            {
                heardTags = heard.tags;
                heardSpectral = heard.spectral;
            }
            else
            {
                // Surface what actually went wrong — a first STEP on a machine
                // without CLAP downloads it and can take minutes, and "could not
                // listen" would read as a broken instrument rather than a wait.
                earError = heard.errorMessage.isNotEmpty()
                               ? heard.errorMessage
                               : juce::String("Could not listen to the instrument");
            }
        }

        // NO FALLBACK TO THE AUTHOR'S READING. A step that cannot listen does not
        // step — exactly as the neural loop bails on an empty render ("nothing to
        // listen to", runSemanticLoopStep). Substituting the author's account of
        // its own code would hand the model a claim under a label that says the
        // instrument was heard, which is the one thing this change exists to stop.
        if (heardTags.isEmpty() && heardSpectral.isEmpty())
        {
            juce::MessageManager::callAsync([safeThis, earError, orchestraUnheard]()
            {
                auto* self = safeThis.getComponent();
                if (self == nullptr) return;
                self->dcoRepromptBusy_ = false;
                if (self->onLcoBusyChanged) self->onLcoBusyChanged(false);
                // Arm the bake route ONLY when the ORCHESTRA is what cannot be
                // heard: that is a sound the user needs to REPLACE, and every
                // further press would otherwise re-attempt a step that cannot work.
                // The next press authors instead, after this message has said why.
                //
                // NOT when the EAR was unreachable — a backend that is not up, a
                // CLAP download still running, an analyze that came back empty.
                // None of those is a broken instrument, and rerouting GENERATE
                // would re-author a good orchestra out from under a user who is
                // only waiting. That case keeps stepping, and keeps reporting what
                // is missing.
                self->dcoEarFailed_ = orchestraUnheard;
                // Throttle the CADENCE only (manual presses stay immediate), mirroring
                // pollDriftRegen's lastRegenFailureMs_. The unreachable-ear case above
                // deliberately keeps stepping, so without this an armed cadence would
                // re-attempt on the very next tick — each attempt taking the
                // process-wide Csound lifecycle lock for the probe's create/compile/
                // start, against the live engine, several times a second.
                self->lastLcoStepFailureMs_ = juce::Time::getMillisecondCounterHiRes();
                // A step that authored nothing recorded nothing. Left armed, the
                // stamp would sit on whichever LATER pass happened to land - a
                // hand-played one included - and mark it a take.
                self->lroTakeStepArmed_ = false;
                self->setLcoStatus(earError.isNotEmpty()
                                       ? earError
                                       : juce::String("Could not listen to the instrument"));
            });
            return;
        }

        const juce::String sysp = RepromptStances::stanceSystemPrompt(stanceKey);
        juce::String userTurn = RepromptStances::buildDcoStanceUserTurn(
            stanceKey, heardTags, heardSpectral, flags, prev, recent);
        // Ground the rewrite in the acoustic palette (LCO-only; a no-op when the
        // palette is empty), but ONLY for the stances that DESCRIBE the sound.
        // abduction (name a real-world source) and verniedlicher (re-narrate as
        // magical realism) leave acoustic description entirely — constraining them
        // to the palette collapses them back onto the reading instead of leaping
        // (empirically: with the palette, abduction converged 100% on "a vintage
        // analogue synthesizer produces…"; without it, real leaps — tools/lco_diagnostic.py).
        // Skip on an empty turn (off/unknown stance) so a bare constraint is never
        // sent with nothing to reinterpret.
        const bool leavesAcoustic = (stanceKey == "abduction" || stanceKey == "verniedlicher");
        if (userTurn.isNotEmpty() && ! leavesAcoustic)
            userTurn += RepromptStances::dcoVocabularyConstraintBlock(vocab);
        // 0 == no cap (see PipeInference::interpret): the stance already asks for
        // 3-8 words and cleanPrompt enforces the budget AFTER the fact, on a whole
        // reply. The former hard 64 could only ever cut a longer reply mid-sentence
        // — silently, since a truncated generation is not an error — and cleanPrompt
        // would then trim the fragment into something that reads finished.
        auto r = pipePtr->interpret(sysp, userTurn, 0, device);
        const juce::String cleaned = r.success ? RepromptStances::cleanPrompt(r.text) : juce::String();

        juce::MessageManager::callAsync(
            [safeThis, success = r.success, cleaned, errorMessage = r.errorMessage]()
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;   // panel gone — nothing to write

            self->dcoRepromptBusy_ = false;
            if (self->onLcoBusyChanged) self->onLcoBusyChanged(false);

            if (! success || cleaned.isEmpty())
            {
                const juce::String failMsg = errorMessage.isNotEmpty()
                                                 ? errorMessage
                                                 : juce::String("The rewrite came back empty");
                self->lastLcoStepFailureMs_ = juce::Time::getMillisecondCounterHiRes();  // see the ear tail
                self->lroTakeStepArmed_ = false;                                         // ditto
                self->setLcoStatus(failMsg);
                return;   // do NOT touch the prompt editor on failure/empty
            }

            self->lastLcoStepFailureMs_ = 0.0;   // a step landed — disarm the throttle
            self->dcoLoopLast_ = cleaned;
            self->dcoLoopRecent_.add(cleaned);
            while (self->dcoLoopRecent_.size() > 3)
                self->dcoLoopRecent_.remove(0);
            self->dcoPromptEditor.setText(cleaned, juce::dontSendNotification);
            self->triggerDcoBake();   // re-checks its own gates
        });
    }).detach();
}

// ── Per-mode slider memory ───────────────────────────────────────────────────
float& PromptPanel::lateMixForMode(const juce::String& mode)
{
    if (mode == "kombi1") return lateMixKombi1_;
    if (mode == "kombi2") return lateMixKombi2_;
    if (mode == "kombi3") return lateMixKombi3_;
    return lateMixFine_;  // late_step + fallback for linear/layer_split
}

float PromptPanel::lateMixForMode(const juce::String& mode) const
{
    if (mode == "kombi1") return lateMixKombi1_;
    if (mode == "kombi2") return lateMixKombi2_;
    if (mode == "kombi3") return lateMixKombi3_;
    return lateMixFine_;
}

// ── Reconfigure alphaSlider for the active injection mode ────────────────────
// Linear: APVTS-attached, range −2..+2 (quadratic skew near zero), label "A ↔ B".
// Step-in: detached, range 0..1, label "Step-in / Combo: A → mix".
// Layer : detached, range 0..ditBlocks (snap=1), label "Layer Split".
// The alphaSlider's onValueChange dispatches on injectionMode_ to update both
// the value formatter and (for non-linear) the local state.
void PromptPanel::applyModeToSlider()
{
    auto& apvts = processorRef.getValueTreeState();
    if (injectionMode_ == "linear")
    {
        // Restore single-thumb style in case we're coming from layer mode.
        alphaSlider.setSliderStyle(juce::Slider::LinearVertical);
        // Range is owned by the SliderAttachment — JUCE 8's
        // SliderParameterAttachment ctor calls slider.setNormalisableRange()
        // with the APVTS parameter's NormalisableRange. For genAlpha that is
        // [-2, +2] with a quadratic skew for fine control near zero.
        //
        // This used to call setRange(-1, 1, 0.001) defensively. That
        // hardcoded the wrong endpoints AND dropped the parameter skew. On
        // initial enter-linear the `if (alphaA == nullptr)` guard below
        // skipped Attachment recreation, so the bogus range stuck. Mode-
        // toggling reset alphaA, which on re-enter recreated the
        // Attachment and silently restored the correct range — surfacing
        // the bug as "scale is -1..1 on first open, becomes -2..2 after
        // toggling modes". The defensive call is gone; the Attachment is
        // now the single source of truth for both range and skew.
        if (alphaA == nullptr)
            alphaA = std::make_unique<Attachment>(apvts, PID::genAlpha, alphaSlider);
        alphaLabel.setText("A " + juce::String(juce::CharPointer_UTF8("\xe2\x86\x94")) + " B",
                           juce::dontSendNotification);
        // Onchange handler will populate alphaValue from current slider value.
        alphaSlider.setValue(alphaSlider.getValue(), juce::sendNotificationSync);
    }
    else if (injectionMode_ == "late_step"
          || injectionMode_ == "kombi1"
          || injectionMode_ == "kombi2"
          || injectionMode_ == "kombi3")
    {
        alphaA.reset();  // detach from APVTS so the slider drives local state only
        alphaSlider.setSliderStyle(juce::Slider::LinearVertical);
        // Slider is the intensity 0..1: 0 = minimum perceptible effect, 1 =
        // maximum. Internal mapping (in buildInferenceRequest) shifts this
        // onto the audible region of injection_transition_at / late_phase_alpha.
        // setRange() may clamp the current value and fire onValueChange,
        // which would overwrite the saved state — capture and restore.
        const float saved = juce::jlimit(0.0f, 1.0f, lateMixForMode(injectionMode_));
        alphaSlider.setRange(0.0, 1.0, 0.01);
        lateMixForMode(injectionMode_) = saved;
        const juce::String prefix = (injectionMode_ == "kombi1") ? "Combo 1: A "
                                  : (injectionMode_ == "kombi2") ? "Combo 2: A "
                                  : (injectionMode_ == "kombi3") ? "Combo 3: A "
                                  :                                "Step-in: A ";
        alphaLabel.setText(prefix + juce::String(juce::CharPointer_UTF8("\xe2\x86\x92")) + " mix",
                           juce::dontSendNotification);
        alphaSlider.setValue(saved, juce::sendNotificationSync);
    }
    else  // layer_split — two-thumb range slider [b_start, b_end]
    {
        alphaA.reset();
        alphaSlider.setSliderStyle(juce::Slider::TwoValueVertical);
        const float blocksF = static_cast<float>(ditBlocks_);
        const float savedStart = juce::jlimit(0.0f, blocksF, splitLayerStart_);
        const float savedEnd   = juce::jlimit(0.0f, blocksF, splitLayerEnd_);
        alphaSlider.setRange(0.0, static_cast<double>(blocksF), 1.0);
        splitLayerStart_ = savedStart;
        splitLayerEnd_   = savedEnd;
        alphaLabel.setText("Layer B-zone (low-high)", juce::dontSendNotification);
        // Set both thumbs without firing notifications, then trigger onValueChange
        // once at the end to populate the value display from saved state.
        alphaSlider.setMinValue(savedStart, juce::dontSendNotification, true);
        alphaSlider.setMaxValue(savedEnd,   juce::sendNotificationSync, true);
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Shared request builder
// ──────────────────────────────────────────────────────────────────────────────
PipeInference::Request PromptPanel::buildInferenceRequest(
    float alphaOverride, std::map<juce::String, float> axesOverride,
    float noiseOverride, float magnitudeOverride,
    float lateMixOverride, float splitStartOverride, float splitEndOverride,
    float resynthOverride)
{
    auto& apvts = processorRef.getValueTreeState();
    float alpha = std::isnan(alphaOverride)
                      ? apvts.getRawParameterValue(PID::genAlpha)->load()
                      : alphaOverride;
    float magnitude = std::isnan(magnitudeOverride)
                          ? apvts.getRawParameterValue(PID::genMagnitude)->load()
                          : magnitudeOverride;
    float noiseSigma = std::isnan(noiseOverride)
                           ? apvts.getRawParameterValue(PID::genNoise)->load()
                           : noiseOverride;
    float duration = apvts.getRawParameterValue(PID::genDuration)->load();
    // Steps/CFG have no UI anymore (DCO Slice 0) — always force the per-model
    // values at the generation choke point, overriding whatever a preset or
    // host automation wrote into APVTS. The model-click handler keeps APVTS in
    // sync for display/automation lanes, but the request never trusts it.
    const auto forcedInf = defaultParamsFor(getSelectedModel());
    int steps = static_cast<int>(forcedInf.steps);
    float cfgScale = forcedInf.cfg;
    int seed = processorRef.getLastRandomSeed() ? -1 : processorRef.getLastSeed();

    // Mode-specific parameter resolution: drift-driven overrides win when
    // present, otherwise fall back to the panel's slider state.
    const float effLateMix    = std::isnan(lateMixOverride)    ? lateMixForMode(injectionMode_) : lateMixOverride;
    const float effSplitStart = std::isnan(splitStartOverride) ? splitLayerStart_   : splitStartOverride;
    const float effSplitEnd   = std::isnan(splitEndOverride)   ? splitLayerEnd_     : splitEndOverride;

    PipeInference::Request req;
    req.promptA = (pendingLoopPromptA_.isNotEmpty() ? pendingLoopPromptA_ : promptAEditor.getText()).trim();
    req.promptB = (pendingLoopPromptB_.isNotEmpty() ? pendingLoopPromptB_ : promptBEditor.getText()).trim();
    req.alpha = alpha;
    req.magnitude = magnitude;
    req.noiseSigma = noiseSigma;
    req.durationSeconds = duration;
    req.startPosition = 0.0f;
    req.steps = steps;
    req.cfgScale = cfgScale;
    req.seed = seed;
    req.device = defaultInferenceDevice_;
    req.model = getSelectedModel();
    // Modality-routing epoch for this preset/session (T5ynthProcessor::getModalityEpoch).
    // Sent on every request; the backend acts on it only for SA3. A legacy preset reports
    // kLegacyModalityEpoch, so the backend keeps the old Music/SFX-only prefixes.
    req.modalityEpoch = processorRef.getModalityEpoch();
    const auto requestInjectionMode = isAudioLDM2Model(req.model) ? juce::String("linear")
                                                                  : injectionMode_;
    req.dimensionOffsets = std::move(pendingOffsets_);
    req.semanticAxes = axesOverride.empty() ? std::move(pendingAxes_) : std::move(axesOverride);
    req.axesAmount = apvts.getRawParameterValue(PID::genAxesAmount)->load();
    // Semantic axes AND the dimension explorer now run for SA3 too — the backend
    // confines the embedding edit to the real (non-padded) tokens, so both are
    // sent as-is. Only the SA3 modality routing is model-specific below.
    if (isSA3Model(req.model))
    {
        // SA3 modality: the tonal slot (0) and SFX slot (1) can be backed by the
        // SAME medium checkpoint, so the selected slot index — not the model id —
        // selects the domain. The tonal slot sends "instrument" (isolated single
        // instrument): from epoch 1 the backend renders Instrument by default and
        // upgrades to Music only for music-signalling prompts (never SFX — that is
        // the SFX slot's job); under the legacy epoch "instrument" maps straight to
        // Music, so old presets don't change. Sent for every SA3 request; the backend
        // prefers it over the dir-name sniff.
        req.trackType = (getSelectedSlot() == 1) ? "sfx" : "instrument";
    }
    req.injectionMode = requestInjectionMode;
    // Single-prompt promptability guard (linear mode, slider-driven α only).
    // The linear blend (0.5−0.5α)·A + (0.5+0.5α)·B places a lone prompt at α=∓1
    // and the null / unconditional output at the α=0 slider centre — so typing
    // one prompt and leaving the slider untouched would render null, not the
    // prompt (on SA3 that null is a buzz). When exactly one field is filled, pin
    // α to that prompt's pure end. Symmetric: A-only and B-only are equal
    // partners (the A/B-equality design). Two-prompt blends stay untouched, and
    // delta / step / layer modes — which don't collapse at α=0 — are excluded.
    // Skipped when α is explicitly overridden (drift / explorer alpha sweep), so
    // intentional exploration through the null point still works.
    if (requestInjectionMode == "linear" && std::isnan(alphaOverride)
        && req.promptA.isEmpty() != req.promptB.isEmpty())
        req.alpha = req.promptB.isEmpty() ? -1.0f : 1.0f;  // B empty → pure A; A empty → pure B
    // Step-in slider drives BOTH transition_at AND late-phase α together so that
    // slider=0.5 → minimum effect (A-dominant), slider=1.0 → pure B.
    //   t = (slider - 0.5) / 0.5  ∈ [0, 1]
    //   transition_at: 0.5 (halfway swap) → 0.05 (almost immediate)
    //   late_α       : 0   (50/50 mix)    → 1   (pure B)
    {
        const float t = juce::jlimit(0.0f, 1.0f, effLateMix);
        req.injectionTransitionAt = juce::jlimit(0.05f, 0.95f, 0.5f - 0.45f * t);
        req.latePhaseAlpha        = t;  // 0 → 50/50, 1 → pure B
    }
    // Layer: two-thumb range slider directly defines the B-zone [start, end]
    // in DiT block index space. No inversion needed — the user's mental model
    // (low thumb = where B starts, high thumb = where B ends) maps 1:1 to the
    // backend's b_start / b_end fields.
    const float blocksF = static_cast<float>(ditBlocks_);
    req.splitStart = juce::jlimit(0.0f, blocksF, effSplitStart);
    req.splitEnd   = juce::jlimit(0.0f, blocksF, effSplitEnd);
    // Combo modes overwrite the layer range with their per-mode band so drift /
    // preset / slider state can never desync the geometry.
    // Combo 1 = "B as surface skin" (low DiT blocks);
    // Combo 2 = "B as gestalt filter" (broad mid).
    //
    // The bands are FRACTIONS of the active model's DiT depth (blocksF =
    // ditBlocks_), so they map to the same relative depths on any model
    // (SAO Small=16 → historical 0-4 / 4-12 / 6-10; SA3=20 → 0-5 / 5-15 /
    // 7.5-12.5). The backend (_generate_native) re-asserts the identical
    // fractions, so both sides stay in lock-step automatically.
    if (requestInjectionMode == "kombi1") { req.splitStart = 0.0f;            req.splitEnd = 0.25f  * blocksF; }
    if (requestInjectionMode == "kombi2") { req.splitStart = 0.25f  * blocksF; req.splitEnd = 0.75f  * blocksF; }
    if (requestInjectionMode == "kombi3") { req.splitStart = 0.375f * blocksF; req.splitEnd = 0.625f * blocksF; }

    // Resynth (init_audio / i2i): a single Off->Full amount. 0 (Off) = text-only;
    // turning up feeds the LAST raw generation back as the denoise seed so the next
    // render evolves from it instead of from pure noise. SA3-gated to match the UI
    // element (and so a stale non-zero recalled under SAO/AudioLDM2 — whose
    // diffusers path can't take init_audio — never leaks a buffer). Only attaches
    // when a prior buffer actually exists; the very first generation of a session
    // has none and stays text-only. Read on the message thread, same as
    // loadGeneratedAudio writes it.
    // Drift can modulate resynth: when it does, pollDriftRegen passes the effective
    // (base+offset) value as resynthOverride so the loop denoises from the drifted
    // amount rather than the static slider. NaN override → plain slider read.
    float resynthAmount = juce::jlimit(0.0f, 1.0f,
        std::isnan(resynthOverride) ? apvts.getRawParameterValue(PID::resynthAmount)->load()
                                    : resynthOverride);
    // One-shot clean render after a Re-Prompt deactivation: detach init_audio so the
    // restored ORIGINAL renders fresh from the prompt instead of staying anchored to
    // the last machine output (which would otherwise keep the last B audible despite
    // the reverted text). Consuming it here also clears the pending-trigger flag, so
    // whichever path renders first (manual / auto-regen / the deferred trigger) does it
    // exactly once.
    if (forceCleanRenderOnce_)
    {
        resynthAmount = 0.0f;
        forceCleanRenderOnce_ = false;
        pendingOriginalReRender_ = false;
    }
    if (resynthAmount > 0.01f && isSA3Model(req.model))
    {
        const int resynthSource = static_cast<int>(
            apvts.getRawParameterValue(PID::resynthSource)->load());
        bool haveSeed = false;
        if (resynthSource == ResynthSource::External)
        {
            // EXTERNAL: seed from LIVE external audio capture. Returns false (→ no
            // init_audio, text-only) when no input device / denied permission /
            // silence, so a missing source never seeds silence.
            double extCaptureSR = 0.0;
            if (processorRef.snapshotExternalCapture(req.initAudio, extCaptureSR, duration))
            {
                req.initAudioSampleRate = extCaptureSR;
                haveSeed = true;
            }
        }
        else
        {
            // INTERNAL (default): self-feedback — seed from the last raw generation,
            // so each render evolves from the previous. The original Resynth path.
            const auto& rawBuf = processorRef.getGeneratedAudioRaw();
            if (rawBuf.getNumSamples() > 0 && rawBuf.getNumChannels() > 0)
            {
                req.initAudio.makeCopyOf(rawBuf);
                req.initAudioSampleRate = processorRef.getGeneratedSampleRate();
                haveSeed = true;
            }
        }
        if (haveSeed)
        {
            // Amount (0->1) maps to backend init_noise_level (the SDEdit start
            // sigma). Full (1.0) -> 0.05 = max self-resynthesis (the input strongly
            // seeds the result); LOW end -> ~0.90 = PROMPT-DOMINANT (a typed prompt
            // wins over the carried input).
            //
            // WHY 0.90 and not the old 0.50 cap: at sigma <=0.5 the init_audio carry
            // DROWNS the prompt. The old band [0.05 .. 0.50] therefore had NO setting
            // where a static, user-typed prompt could win — feed a pad, type "samba",
            // and you heard "forever a slightly altered pad". The prompt only broke
            // through when Re-Prompt forced sigma 0.9 (the override below) — i.e.
            // prompt-dominance was gated behind a button it should never have needed.
            // Reaching 0.9 here hands the sound to the prompt directly: turn Resynth
            // DOWN for the prompt, UP to resynthesise/preserve the input.
            //
            // The self-resynthesis EVOLUTION response (measured: tools/test_resynth_loop.py,
            // RESYNTH_CALIBRATION_FINDINGS.md) lives in sigma [~0.12 .. ~0.40]
            // (saturates below 0.12, washes the carry out above ~0.40) and now
            // occupies the UPPER part of the travel (Full..mid); the lower part is
            // the prompt-dominant region the user asked to reach. Linear — the
            // full-scale numbers are the user's by-ear call. Applies to both int and ext.
            req.initNoiseLevel = 0.90f - 0.85f * resynthAmount;

            // ── Semantic-loop word-dominance override ──
            // When a loop stance is active the rewritten PROMPT must drive the next
            // render, not the carried-over wave. At HIGH Resynth amounts the band
            // above sits at low sigma (Full -> 0.05) where the init_audio carry
            // DROWNS the rewritten prompt and the loop re-renders the same carry
            // centroid. clap_llm_loop.py runs at init_noise 0.9 (validated: commit
            // 9feb21ef / tools/diag_promptbite.py) so the words win while the signal
            // still carries. Force that band whenever a stance is engaged, regardless
            // of the Resynth amount, so a rewrite is never drowned. Read on the
            // message thread, same as every other param here.
            const int repromptStance = static_cast<int>(
                apvts.getRawParameterValue(PID::repromptStance)->load());
            if (repromptStance != RepromptStance::Off)
                req.initNoiseLevel = 0.9f;
        }
    }
    return req;
}

// ──────────────────────────────────────────────────────────────────────────────
// Manual generation (Generate button / Enter)
// ──────────────────────────────────────────────────────────────────────────────
bool PromptPanel::playNextCachedInference()
{
    if (!processorRef.playNextInferenceCacheEntry())
        return false;

    pendingOffsets_.clear();
    if (onStatusChanged) onStatusChanged("From cache", false);
    return true;
}

bool PromptPanel::pressWouldReplayCache() const
{
    if (easyMode_)
        // The neural manual press replays whenever the cache is full, stance or
        // no stance (see triggerGenerationWithOffsets' own gate).
        return processorRef.isInferenceCacheFull();

    // The LRO's, and it is the same rule: a full cache is a full cache. The depth
    // the player set IS the budget, and once it is spent nothing more is written
    // - whether the entries were recorded as a take or played in by hand, and
    // whether or not a Re-Prompt stance is engaged.
    //
    // It used to carry an exception for a running stance, on the grounds that
    // the loop listens to its own output in order to rewrite and so needs a real
    // authoring pass. What that produced was an LRO that kept authoring past a
    // full cache forever - every step paying a bare render, a listen and two
    // model turns for a result addCsoundCacheEntry then refused (BJ, 22.08.2026:
    // "hoert nicht auf zu generieren auch wenn der Cache voll ist").
    return processorRef.isCsoundCacheFull();
}

bool PromptPanel::playNextCachedCsound()
{
    // Which slot is next - read BEFORE the play, which advances the index past
    // it. playCachedCsoundAt does the installing for both callers.
    if (! processorRef.isCsoundCacheFull())
        return false;
    return playCachedCsoundAt(processorRef.getCsoundCachePlaybackIndex());
}

bool PromptPanel::playCachedCsoundAt(int index)
{
    // Clamped HERE, with the same limit the processor half applies, so both
    // halves install the same slot. Left to the processor alone, an index past
    // the end would play a clamped entry while the bounds check below skipped
    // the panel half entirely - a half-recall, which is the one thing this
    // function exists to prevent.
    const int count = processorRef.getCsoundCacheFillCount();
    if (count <= 0)
        return false;
    index = juce::jlimit(0, count - 1, index);

    if (!processorRef.playCsoundCacheEntry(index))
        return false;

    // FIRST, because a status REPLACES the trace (see setLcoStatus) - written
    // after the card below it would blank the very thing this replay has to
    // show. The label keeps the word; the card keeps the instrument.
    setLcoStatus("From cache", {}, /*busy=*/false);

    // A replay IS a recall, so it does what MainPanel's SNAP recall does. The
    // processor half (engine mode, orchestra, prompt, reading, params, knobs,
    // author settings) has just run inside playCsoundCacheEntry; everything
    // below belongs to the panel and has no other way in.
    const auto& entries = processorRef.getCsoundCacheEntries();
    if (index < static_cast<int>(entries.size()))
    {
        const auto& e = entries[static_cast<size_t>(index)];

        // The trace card. NOT setLcoStatus alone: a status REPLACES the trace,
        // so a replay would show nothing at all about the instrument now
        // sounding, although the slot carries its prompt, its reading and its
        // body. A slot holds no consultation and no repairs - those happened at
        // bake time - and those stations stay absent rather than blank, exactly
        // as they do for a recalled snapshot.
        setLcoRecalledTrace(e.prompt, e.reading, e.authorModel, e.paramsText);
        // Whoever wrote THIS orchestra, including "not known" - which has to
        // clear the previous bake's name rather than let it stand over a
        // different sound.
        if (e.authorModel.isNotEmpty()) setLcoAuthorModel(e.authorModel);
        else                            resetLcoAuthorModel();
        // Re-Prompt reads the panel's own last reading and prompt to build its
        // next turn. Without this a stance step would rewrite the orchestra the
        // replay just replaced - and the ear-failure latch would still be armed
        // from an instrument that is no longer loaded.
        adoptRecalledOrchestra(e.prompt, e.reading);
        // The editor too, exactly as MainPanel's SNAP recall writes it:
        // triggerDcoBake authors from the editor's TEXT, not from the chain's
        // last prompt, so a GENERATE without a stance would otherwise write a
        // new instrument for whatever the last step happened to leave standing
        // there rather than for the one the player is listening to.
        setLcoPrompt(e.prompt);
        // The KNOBS station belongs to the request that was just installed - the
        // same var the authoring pass handed over, so the same station. Without
        // it the generation counter is left behind (setAuthorSettings bumps it)
        // and the card reports "a different patch has been loaded" over settings
        // that ARE on the synth. AFTER setLcoRecalledTrace, which clears the
        // station on the grounds that a recalled PRESET carries no answer - a
        // slot does carry one.
        adoptAuthorSettingsStation(e.settings);
    }

    // The compile window, same as a fresh bake and a recalled snapshot: a cached
    // orchestra can fail to compile like any other (a preset travels to other
    // sample rates and oversampling factors), and without this the flags line
    // reports nothing either way - and csoundCompileWatching_, which both
    // triggerDcoReprompt and the cadence gate on, would stay false while the
    // swap is still in flight. LAST: setLcoRecalledTrace resets the RUNNING
    // station to Unknown, exactly as it does for a recalled snapshot.
    beginCsoundCompileWatch();
    return true;
}

bool PromptPanel::playCachedInferenceAt(int index)
{
    if (!processorRef.playInferenceCacheEntry(index))
        return false;

    pendingOffsets_.clear();
    if (onStatusChanged) onStatusChanged("From cache", false);
    return true;
}

// ──────────────────────────────────────────────────────────────────────────────
// Language-model availability gate
// ──────────────────────────────────────────────────────────────────────────────
// One model does all three LLM jobs: it writes the LCO's Csound orchestra, it
// translates prompts, and it is the Re-Prompt interpreter. So there is one gate.
// When the model is absent those features fail silently — the LCO bake has nothing
// to author with, Translate keeps the original text, and the interpreter falls back
// to the previous prompt every step (an invisible no-op loop). Mirror the
// model-settings install state instead: disable the controls (they dim) and explain
// why via tooltip, and re-enable the instant it is installed. The bake gate lives at
// the top of triggerDcoBake and the loop guard in runSemanticLoopStep covers any
// preset/automation that engaged a stance while the model is gone.
void PromptPanel::setLlmAvailable(bool available)
{
    if (llmAvailableKnown_ && available == llmAvailable_)
        return;   // idempotent: onCoderModelChanged may re-send the same value
    llmAvailableKnown_ = true;
    llmAvailable_ = available;

    translateToggle.setEnabled(available);
    repromptStanceBar.setEnabled(available);
    for (auto& b : repromptCouplingBtns)
        b.setEnabled(available);
    dcoStanceBar.setEnabled(available);

    translateToggle.setTooltip(available
        ? "Translate prompts to English in place "
          "(auto-regen pauses during translation, then resumes)"
        : "Install the language model in the Models settings tab to translate "
          "prompts.");
    // Empty → the stance bar's per-glyph hover tooltips resume (set in mouseMove).
    repromptStanceBar.setTooltip(available
        ? juce::String()
        : juce::String("Re-Prompt needs the language model. Install it in the "
                       "Models settings tab."));
    dcoStanceBar.setTooltip(available
        ? juce::String()
        : juce::String("LRO Re-Prompt needs the language model. Install it in the "
                       "Models settings tab."));

    repromptStanceBar.repaint();   // a raw Component: reflect the dim immediately
    dcoStanceBar.repaint();        // same custom-paint dim logic as repromptStanceBar
    updateLcoModelTabs();          // the LCO model tab lights/dims with the same flag
}

void PromptPanel::beginCsoundCompileWatch()
{
    csoundCompileWatching_ = true;
    csoundCompileSeenBusy_ = false;
    csoundCompileWatchStartMs_ = juce::Time::getMillisecondCounterHiRes();
    // Through the ONE writer, so the RUNNING station enters "compiling" with the
    // window rather than keeping whatever the previous orchestra resolved to.
    // (No resized() any more: dcoFlagsLabel is never laid out, so the relayout
    // this used to trigger was a full panel pass for an invisible label.)
    setLcoCompileState(LcoTraceView::CompileState::Compiling);
}

// The shipped install slot gets its readable name; anything else (an env pin, a
// legacy or dev drop) shows its directory name verbatim — honest over pretty.
static juce::String lcoModelPrettyName(const juce::String& dirName)
{
    return dirName == "gemma-4-12b-it-qat-q4_0" ? juce::String("Gemma 4 12B")
                                                : dirName;
}

// The tab line names the model AND what it does here, with the synthesis
// backend spelled out: "Gemma 4 12B — writes code for Csound". The bake claim
// wins over the idle resolution while it stands; the tooltip carries the full
// directory name and which of the two is being shown.
void PromptPanel::refreshLcoModelTabText()
{
    const auto shown = lcoAuthorClaim_.isNotEmpty() ? lcoAuthorClaim_
                                                    : lcoResolvedModel_;
    if (shown.isEmpty())
    {
        dcoModelBtns[0].setButtonText(kLcoNoModelLabel);
        dcoModelBtns[0].setTooltip("Install the language model in the Models "
                                   "settings tab.");
    }
    else
    {
        dcoModelBtns[0].setButtonText(lcoModelPrettyName(shown)
            + juce::String::fromUTF8(" \xe2\x80\x94 writes code for Csound"));
        dcoModelBtns[0].setTooltip((lcoAuthorClaim_.isNotEmpty()
                                        ? "Wrote this orchestra: "
                                        : "Will write the next orchestra: ")
                                   + shown);
    }
    dcoModelBtns[0].repaint();
}

// Name the model that ACTUALLY wrote the orchestra. The backend puts its
// resolver's answer on the wire (`author_model`); while that claim stands the
// tab shows it and nothing else, so a resolver that walked past the intended
// slot is visible on the panel instead of hiding behind the idle resolution.
void PromptPanel::setLcoAuthorModel(const juce::String& modelDirName)
{
    const auto name = modelDirName.trim();
    if (name.isEmpty())
        return;                       // no claim beats a wrong claim
    lcoAuthorClaim_ = name;
    refreshLcoModelTabText();
}

void PromptPanel::setLcoResolvedModel(const juce::String& modelDirName)
{
    lcoResolvedModel_ = modelDirName.trim();
    refreshLcoModelTabText();
}

void PromptPanel::resetLcoAuthorModel()
{
    lcoAuthorClaim_ = {};
    refreshLcoModelTabText();
}

// Refresh the single-tab LCO model strip: the tab shows the model at full
// opacity when installed and dimmed (0.3, matching modelBtns[]) when absent.
void PromptPanel::updateLcoModelTabs()
{
    const bool installed[kNumLcoModelSlots] = { llmAvailable_ };
    for (int i = 0; i < kNumLcoModelSlots; ++i)
    {
        dcoModelBtns[i].setAlpha(installed[i] ? 1.0f : 0.3f);
        dcoModelBtns[i].repaint();
    }
}

// ──────────────────────────────────────────────────────────────────────────────
// Manual prompt translation (Union-Jack flag)
// ──────────────────────────────────────────────────────────────────────────────
// Clicking the flag rewrites the A/B prompts to English IN PLACE. The single IPC
// pipe is shared with auto-regen, so the click DISABLES auto-regen for the whole
// translation by raising translatingPrompts_ (which gates both pollDriftRegen and
// triggerGeneration). The translation runs on a background thread that serialises
// behind any in-flight generation via PipeInference's recursive mutex; when it
// completes, lowering the flag REACTIVATES auto-regen — automatically with its
// unchanged bar setting, and only if it was on. The flag pulses while the
// translation actually runs.
void PromptPanel::translatePromptsInPlace()
{
    if (translatingPrompts_) return;  // already translating

    // Mutually exclusive with an engaged Re-Prompt stance. The loop already yields to
    // translation (runSemanticLoopStep bails on translatingPrompts_); make the reverse
    // true too. Translate reads the LIVE editors and writes them back WITH notification
    // (onTextChange → the human-prompt store). Mid-loop the editors hold the machine's
    // rewrite, not the user's text, so translating then would poison the human store
    // with machine-derived text — the exact leak this store exists to prevent. Turn the
    // stance Off (restores the human originals) before translating.
    if (static_cast<int>(processorRef.getValueTreeState()
            .getRawParameterValue(PID::repromptStance)->load()) != RepromptStance::Off)
    {
        if (onStatusChanged) onStatusChanged("Turn Re-Prompt off to translate", false);
        return;
    }

    if (!processorRef.isPipeInferenceReady())
    {
        if (onStatusChanged) onStatusChanged("Backend not connected", false);
        return;
    }

    // Defensive: the flag is disabled when the model is absent, so onClick can't fire —
    // but never reach the backend for a translate that can only return an error.
    if (!llmAvailable_)
    {
        if (onStatusChanged) onStatusChanged("Language model not installed", false);
        return;
    }

    const auto srcA = promptAEditor.getText();
    const auto srcB = promptBEditor.getText();
    if (srcA.trim().isEmpty() && srcB.trim().isEmpty())
        return;  // nothing to translate

    // Suppress auto-regen for the whole translation via translatingPrompts_ (the
    // pollDriftRegen + triggerGeneration guards). This frees the shared IPC pipe
    // so the background translate can run, and auto-regen resumes automatically —
    // with its unchanged bar setting — the instant this flag clears. Deliberately
    // NOT done by writing PID::driftRegen: that param lives on the processor and
    // would be left stuck OFF (and could be saved into a preset) if the editor
    // were closed mid-translation, since the async restore would be skipped.
    translatingPrompts_ = true;   // blocks pollDriftRegen + manual gen until done
    translateToggle.setPulsing(true);
    if (onStatusChanged) onStatusChanged("translating...", true);

    auto pipePtr = processorRef.getPipeInferencePtr();
    const auto device = defaultInferenceDevice_;
    juce::Component::SafePointer<PromptPanel> safeThis(this);
    std::thread([safeThis, pipePtr, srcA, srcB, device]() mutable
    {
        juce::String errMsg;
        auto translateOne = [&](const juce::String& s) -> juce::String
        {
            if (s.trim().isEmpty() || pipePtr == nullptr) return s;
            // Keep trailing pitch/tempo tokens (c3, 120bpm) verbatim: translate only
            // the descriptive core, then re-append the suffix.
            const juce::String suffix = RepromptStances::trailingMusicSuffix(s);
            const juce::String core   = RepromptStances::stripMusicSuffix(s);
            if (core.isEmpty()) return s;  // nothing but tokens → leave verbatim
            auto tr = pipePtr->translate(core, device);  // blocks behind any in-flight generate()
            if (tr.success && tr.text.isNotEmpty())
                return suffix.isEmpty() ? tr.text
                                        : RepromptStances::reattachMusicSuffix(tr.text, suffix);
            if (!tr.success && errMsg.isEmpty()) errMsg = tr.errorMessage;
            return s;  // failure → keep the original text
        };
        const juce::String enA = translateOne(srcA);
        const juce::String enB = translateOne(srcB);

        juce::MessageManager::callAsync([safeThis, enA, enB, srcA, srcB, errMsg]
        {
            if (auto* self = safeThis.getComponent())
            {
                if (enA != srcA) self->promptAEditor.setText(enA, juce::sendNotification);
                if (enB != srcB) self->promptBEditor.setText(enB, juce::sendNotification);
                self->translateToggle.setPulsing(false);
                self->translatingPrompts_ = false;  // auto-regen resumes here (param untouched)
                if (self->onStatusChanged)
                    self->onStatusChanged(errMsg.isNotEmpty() ? errMsg : juce::String("translated"), false);
            }
        });
    }).detach();
}

void PromptPanel::triggerGeneration()
{
    // A running tape owns the generation pipe and the sample buffer: a manual render
    // would loadGeneratedAudio() over the sound the replay just faded in, dragging
    // every held voice to a timbre the tape never contained. Say so rather than
    // silently ignoring the press. (Same reasoning as pollDriftRegen's guard; this
    // covers the Generate button, the prompt editors' Return key, MIDI-mapped
    // generate and triggerGenerationWithOffsets.)
    if (processorRef.isReplayActive())
    {
        if (onStatusChanged) onStatusChanged(juce::String::fromUTF8(
            "replay running \xe2\x80\x94 stop it to generate"), false);
        return;
    }

    // loopStepInFlight_ too: a semantic-loop step's analyze+interpret holds the
    // single IPC pipe on a background thread (with `generating` already false), so a
    // manual Generate must wait or it would contend on the pipe.
    if (generating || translatingPrompts_ || loopStepInFlight_) return;

    if (processorRef.isInferenceCacheFull())
    {
        // No detach here: a preset saved WITH its inference cache restores the
        // whole set, so this press auditions that preset's OWN stored variants
        // — the sound still belongs to the loaded preset and keeps its name.
        playNextCachedInference();
        return;
    }

    if (!processorRef.isInferenceReady())
    {
        // isInferenceReady() is just "pipe connection up" — there is no separate
        // model-loaded state (models are selected per-request). The old "No model
        // loaded" string was misleading when the actual failure was a missing
        // backend install (plugin context with no companion Standalone).
        if (onStatusChanged) onStatusChanged("Backend not connected", false);
        return;
    }

    // Populate device/model info from Python if not yet done
    auto& pipeInf = processorRef.getPipeInference();
    if (!devicesPopulated && pipeInf.isReady())
    {
        populateDeviceChoice();
        populateModelSelector();
    }

    generating = true;
    generateButton.setEnabled(false);
    // A press while an offline take records consumes a cache point like any other
    // entry — without the step it would record the frozen parameters a second time
    // and put a duplicate in the middle of the trajectory.
    takeStepArmed_ = isAsyncTakeRecording();
    if (onStatusChanged) onStatusChanged("generating...", true);

    auto req = buildInferenceRequest();
    auto deviceForLabel = req.device.isEmpty() ? pipeInf.getDefaultDevice() : req.device;
    auto modelForLabel = req.model.isEmpty() ? pipeInf.getDefaultModel() : req.model;
    // Snapshot alongside req, not re-read later — the user can flip Resynth
    // Source while this generation is in flight, and the async result callback
    // must judge "was this call internally-seeded" against what req actually
    // was built with, not whatever the toggle reads by the time results land.
    const bool wasInternalResynth = req.initAudio.getNumSamples() > 0
        && static_cast<int>(processorRef.getValueTreeState()
               .getRawParameterValue(PID::resynthSource)->load()) == 0;
    auto pipePtr = processorRef.getPipeInferencePtr();
    juce::Component::SafePointer<PromptPanel> safeThis(this);
    std::thread([safeThis, pipePtr, req, deviceForLabel, modelForLabel, wasInternalResynth]() mutable
    {
        auto inferenceResult = pipePtr->generate(req);
        juce::MessageManager::callAsync([safeThis, result = std::move(inferenceResult), req, deviceForLabel, modelForLabel, wasInternalResynth]()
        {
            if (auto* self = safeThis.getComponent())
            {
                auto& processor = self->processorRef;
                self->generating = false;
                self->generateButton.setEnabled(true);
                if (result.success)
                {
                    self->stepOfflineTakeAfterCapture(
                        processor.addInferenceCacheEntry(result.audio, result.sampleRate));
                    processor.loadGeneratedAudio(result.audio, result.sampleRate);
                    // The sound in the engine is now a new, unsaved one — drop the
                    // loaded/last-saved preset identity so Save stops pre-filling
                    // that name and warning it will "Replace" a file this sound has
                    // nothing to do with (the LCO bake detaches for the same reason).
                    // Sited HERE, not at the request, because the neural path's
                    // dominant failure mode is asynchronous: a backend error returns
                    // through result.errorMessage with the loaded preset still
                    // playing, and that preset must keep its identity.
                    if (self->onNewGenerationStarted) self->onNewGenerationStarted();
                    processor.setLastDevice(deviceForLabel);
                    // Persist the SFX domain when the bare SA3 id can't encode it (medium backs
                    // both Music/SFX slots). req.trackType is the request-time domain (SA3-only;
                    // empty otherwise), so this is a no-op for every other model and the Music slot.
                    processor.setLastModel(taggedPersistId(modelForLabel, req.trackType == "sfx"));
                    processor.setLastSeed(result.seed);
                    processor.recordEventLogGeneration(buildEventLogGenerationEntry(req, result),
                                                       wasInternalResynth);
                    // Reveal the loop prompt that just became wirksam in this generation.
                    if (self->pendingLoopPromptA_.isNotEmpty())
                    {
                        self->promptAEditor.setText(self->pendingLoopPromptA_, juce::dontSendNotification);
                        self->pendingLoopPromptA_.clear();
                    }
                    if (self->pendingLoopPromptB_.isNotEmpty())
                    {
                        self->promptBEditor.setText(self->pendingLoopPromptB_, juce::dontSendNotification);
                        self->pendingLoopPromptB_.clear();
                    }
                    auto promptA = self->promptAEditor.getText().trim();
                    auto promptB = self->promptBEditor.getText().trim();
                    processor.setLastPrompts(promptA, promptB);
                    self->lastGenPromptA_ = promptA;
                    self->lastGenPromptB_ = promptB;
                    self->syncSeedState(result.seed);
                    processor.setLastGenerationTimeMs(result.generationTimeMs);
                    // A successful manual generate proves the backend is
                    // healthy — clear any stale failure stamp left by a
                    // previous drift auto-regen so the next auto-tick
                    // isn't throttled for up to 2s after recovery.
                    self->lastRegenFailureMs_ = 0.0;
                    auto info = juce::String(result.generationTimeMs / 1000.0f, 1) + "s | seed "
                                + juce::String(result.seed) + " | " + modelForLabel
                                + " | " + deviceForLabel;
                    if (self->onStatusChanged) self->onStatusChanged(info, false);

                    if (!result.embeddingA.empty())
                    {
                        processor.setLastEmbeddings(result.embeddingA, result.embeddingB);
                        auto baseline = result.embeddingBaseline;
                        if (baseline.size() != result.embeddingA.size())
                        {
                            baseline = DimensionExplorer::estimateBaselineValues(
                                result.embeddingA, result.embeddingB,
                                req.alpha, req.magnitude);
                        }
                        if (self->onEmbeddingsReady)
                            self->onEmbeddingsReady(result.embeddingA, result.embeddingB, baseline);
                    }

                    // Semantic loop: listen to this render and rewrite the prompt(s)
                    // for the next generation (no-op unless a stance is active + SA3).
                    self->runSemanticLoopStep(result);
                }
                else
                {
                    if (self->onStatusChanged) self->onStatusChanged(result.errorMessage, false);
                }
            }
        });
    }).detach();
}

// ──────────────────────────────────────────────────────────────────────────────
// Drift auto-regeneration
// ──────────────────────────────────────────────────────────────────────────────
void PromptPanel::triggerDriftRegeneration(float effectiveAlpha,
                                            std::map<juce::String, float> effectiveAxes,
                                            float effectiveNoise,
                                            float effectiveMagnitude,
                                            float effectiveLateMix,
                                            float effectiveSplitStart,
                                            float effectiveSplitEnd,
                                            float effectiveResynth,
                                            bool /*holdForBar*/)
{
    if (generating || translatingPrompts_ || loopStepInFlight_) return;

    lastGenAlpha_ = effectiveAlpha;
    lastGenNoise_ = effectiveNoise;
    lastGenMagnitude_ = effectiveMagnitude;
    lastGenAxes_ = effectiveAxes;
    lastGenLateMix_ = effectiveLateMix;
    lastGenSplitStart_ = effectiveSplitStart;
    lastGenSplitEnd_ = effectiveSplitEnd;

    // Cache replay short-circuits real generation — but the Re-Prompt loop must render
    // fresh audio each step (it listens to the output to rewrite the prompt). So when a
    // stance is active, the auto-regen step bypasses the cache and generates for real.
    // (Manual Generate keeps replaying the cache — only the self-running loop needs to
    // override it.)
    const bool stanceActiveForCache = static_cast<int>(processorRef.getValueTreeState()
        .getRawParameterValue(PID::repromptStance)->load()) != RepromptStance::Off;
    // ... unless the entries were recorded as an offline take: then they are a
    // finished recording of that very evolution, in order, and replaying it is the
    // point of having made it. Asked of the CACHE, not of the switch — with no
    // generation-side Drift to freeze there is no recording, and the switch then
    // leaves play exactly as it was (BJ, 19.08.2026: "async = sync, automatisch").
    if (processorRef.isInferenceCacheFull()
        && (!stanceActiveForCache || processorRef.isInferenceCacheOfflineTake()))
    {
        playNextCachedInference();
        return;
    }

    if (!processorRef.isPipeInferenceReady()) return;

    generating = true;
    generateButton.setEnabled(false);
    takeStepArmed_ = isAsyncTakeRecording();   // this render may consume a cache point
    if (onStatusChanged) onStatusChanged("auto regen...", true);

    auto req = buildInferenceRequest(effectiveAlpha, effectiveAxes, effectiveNoise, effectiveMagnitude,
                                     effectiveLateMix, effectiveSplitStart, effectiveSplitEnd,
                                     effectiveResynth);
    auto deviceForLabel = req.device.isEmpty()
        ? processorRef.getPipeInference().getDefaultDevice() : req.device;
    auto modelForLabel = req.model.isEmpty()
        ? processorRef.getPipeInference().getDefaultModel() : req.model;
    // See triggerGeneration()'s identical comment: snapshot now, don't re-read
    // PID::resynthSource inside the async result callback.
    const bool wasInternalResynth = req.initAudio.getNumSamples() > 0
        && static_cast<int>(processorRef.getValueTreeState()
               .getRawParameterValue(PID::resynthSource)->load()) == 0;
    auto pipePtr = processorRef.getPipeInferencePtr();
    juce::Component::SafePointer<PromptPanel> safeThis(this);
    std::thread([safeThis, pipePtr, req, deviceForLabel, modelForLabel, wasInternalResynth]() mutable
    {
        auto inferenceResult = pipePtr->generate(req);
        juce::MessageManager::callAsync([safeThis, result = std::move(inferenceResult), req, deviceForLabel, modelForLabel, wasInternalResynth]()
        {
            if (auto* self = safeThis.getComponent())
            {
                auto& processor = self->processorRef;
                self->generating = false;
                self->generateButton.setEnabled(true);
                if (result.success)
                {
                    auto newAudio = result.audio;

                    // The previous loop output (output N-1) lives in the processor
                    // until loadGeneratedAudio overwrites it below; capture it once
                    // for the boundary crossfade. (Valid only until that load —
                    // used before it.)
                    const auto& oldRaw = processor.getGeneratedAudioRaw();

                    float xfadeMs = processor.getValueTreeState()
                        .getRawParameterValue(PID::driftCrossfade)->load();
                    int xfadeSamples = juce::roundToInt(xfadeMs * 0.001f * static_cast<float>(result.sampleRate));
                    if (xfadeSamples > 0 && oldRaw.getNumSamples() > 0)
                        applyDriftCrossfade(newAudio, oldRaw, xfadeSamples);
                    // Offline take: this cache point is recorded, so the parameters
                    // move on to the next one — by one cadence interval, not by
                    // however long this render happened to take.
                    self->stepOfflineTakeAfterCapture(
                        processor.addInferenceCacheEntry(result.audio, result.sampleRate));
                    processor.loadGeneratedAudio(newAudio, result.sampleRate);
                    // Reveal the loop prompt that just became wirksam in this generation.
                    if (self->pendingLoopPromptA_.isNotEmpty())
                    {
                        self->promptAEditor.setText(self->pendingLoopPromptA_, juce::dontSendNotification);
                        self->pendingLoopPromptA_.clear();
                    }
                    if (self->pendingLoopPromptB_.isNotEmpty())
                    {
                        self->promptBEditor.setText(self->pendingLoopPromptB_, juce::dontSendNotification);
                        self->pendingLoopPromptB_.clear();
                    }
                    auto promptA = self->promptAEditor.getText().trim();
                    auto promptB = self->promptBEditor.getText().trim();
                    processor.setLastDevice(deviceForLabel);
                    processor.setLastModel(taggedPersistId(modelForLabel, req.trackType == "sfx"));
                    processor.setLastSeed(result.seed);
                    processor.recordEventLogGeneration(buildEventLogGenerationEntry(req, result),
                                                       wasInternalResynth);
                    processor.setLastPrompts(promptA, promptB);
                    self->lastGenPromptA_ = promptA;
                    self->lastGenPromptB_ = promptB;
                    self->syncSeedState(result.seed);
                    processor.setLastGenerationTimeMs(result.generationTimeMs);
                    // Healthy generation — clear the failure throttle so the
                    // next drift change can fire immediately again.
                    self->lastRegenFailureMs_ = 0.0;
                    auto info = juce::String(result.generationTimeMs / 1000.0f, 1) + "s | auto regen";
                    if (self->onStatusChanged) self->onStatusChanged(info, false);

                    if (!result.embeddingA.empty())
                    {
                        processor.setLastEmbeddings(result.embeddingA, result.embeddingB);
                        auto baseline = result.embeddingBaseline;
                        if (baseline.size() != result.embeddingA.size())
                        {
                            baseline = DimensionExplorer::estimateBaselineValues(
                                result.embeddingA, result.embeddingB,
                                req.alpha, req.magnitude);
                        }
                        if (self->onEmbeddingsReady)
                            self->onEmbeddingsReady(result.embeddingA, result.embeddingB, baseline);
                    }

                    // Semantic loop: listen to this Auto-Regen render and rewrite the
                    // prompt(s) for the next tick (no-op unless a stance is active +
                    // SA3). Uses the PRISTINE result.audio (what CLAP should hear),
                    // not the boundary-crossfaded newAudio that was loaded for playback.
                    self->runSemanticLoopStep(result);
                }
                else
                {
                    // Drift auto-regen failure — stamp the failure clock so
                    // pollDriftRegen throttles the next attempt. Without this
                    // the 10 Hz timer would re-trigger generation on every
                    // tick after `generating` flips back to false, oscillating
                    // the status label between "auto regen..." and the error.
                    self->lastRegenFailureMs_ = juce::Time::getMillisecondCounterHiRes();
                    if (self->onStatusChanged) self->onStatusChanged(result.errorMessage, false);
                }
            }
        });
    }).detach();
}

PromptPanel::~PromptPanel()
{
    stopTimer();
    // The offline take's freeze is PROCESSOR state driven from here, and the
    // processor outlives this panel: close the window mid-take and the generation-
    // side Drift LFOs would stay frozen for the life of the instance, with the
    // timer that could release them gone. Hand it back, like MainPanel does with
    // every other processor-side hook.
    processorRef.setDriftGenerationHold(false);
    if (processorRef.isReplayActive())
        processorRef.stopReplay();
}

// ──────────────────────────────────────────────────────────────────────────────
// R2c: Replay generation (called from timerCallback at 10 Hz)
// ──────────────────────────────────────────────────────────────────────────────
void PromptPanel::pollReplayRegen()
{
    if (! processorRef.isReplayActive())
    {
        pendingReplayGen_.reset();
        return;
    }

    const uint32_t epoch = processorRef.getReplayEpoch();

    // A new tape (Stop then Play, possibly inside one timer gap): any held entry
    // belongs to the old timeline. Drop it — the new tape's startReplay() already
    // reset the generation slot, so there is nothing to hand back — and forget the
    // old tape's resynth lineage.
    if (pendingReplayEpoch_ != epoch)
    {
        pendingReplayEpoch_     = epoch;
        pendingReplayGen_.reset();
        lastReplayGenSucceeded_ = false;
    }

    if (! pendingReplayGen_.has_value())
    {
        GenerationEventLogEntry ge;
        if (! processorRef.takeDueReplayGeneration(ge))
            return;
        pendingReplayGen_ = std::move(ge);
    }

    // A generation the tape recorded as failed is not worth re-firing — release the
    // slot so the playhead can arm the next one.
    if (! pendingReplayGen_->success)
    {
        pendingReplayGen_.reset();
        lastReplayGenSucceeded_ = false;
        processorRef.replayGenerationFinished(epoch);
        return;
    }

    // Pipe busy (manual generate, drift regen, a semantic-loop step): hold the entry
    // and retry on the next tick rather than dropping it.
    if (generating || translatingPrompts_ || loopStepInFlight_)
        return;
    if (! processorRef.isPipeInferenceReady())
        return;

    const auto logged = *pendingReplayGen_;
    pendingReplayGen_.reset();
    fireReplayGeneration(logged);
}

void PromptPanel::fireReplayGeneration(const GenerationEventLogEntry& logged)
{
    PipeInference::Request req;
    req.promptA         = logged.promptA;
    req.promptB         = logged.promptB;
    req.alpha           = logged.alpha;
    req.magnitude       = logged.magnitude;
    req.noiseSigma      = logged.noiseSigma;
    req.durationSeconds = logged.durationSeconds;
    req.startPosition   = logged.startPosition;
    req.steps           = logged.steps;
    req.cfgScale        = logged.cfgScale;
    req.seed            = logged.realizedSeed;   // the REALIZED seed — never the -1 sentinel
    req.device          = logged.device;
    req.model           = logged.model;
    req.trackType       = logged.trackType;
    req.modalityEpoch   = logged.modalityEpoch;
    req.dimensionOffsets = logged.dimensionOffsets;
    req.semanticAxes     = logged.semanticAxes;
    req.axesAmount       = logged.axesAmount;
    req.injectionMode         = logged.injectionMode;
    req.injectionTransitionAt = logged.injectionTransitionAt;
    req.latePhaseAlpha        = logged.latePhaseAlpha;
    req.splitStart            = logged.splitStart;
    req.splitEnd              = logged.splitEnd;

    // Resynth. An INTERNAL-resynth link (parent != 0) reconstructs for free: the
    // parent generation already replayed, so its raw output is exactly what sits in
    // the processor right now — the same buffer buildInferenceRequest would take.
    // Two cases degrade to text-only, both deliberately:
    //  * EXTERNAL capture (parent == 0 with init audio): live mic audio, genuinely
    //    gone. The disclosed fidelity gap.
    //  * The parent did not render this time round (backend error, or it was the
    //    first generation of the tape): the buffer sitting in the processor is NOT
    //    the parent's output, so seeding from it would resynthesize the wrong sound.
    if (logged.hadInitAudio && logged.parentGenerationId != 0 && lastReplayGenSucceeded_)
    {
        const auto& rawBuf = processorRef.getGeneratedAudioRaw();
        if (rawBuf.getNumSamples() > 0 && rawBuf.getNumChannels() > 0)
        {
            req.initAudio.makeCopyOf(rawBuf);
            req.initAudioSampleRate = processorRef.getGeneratedSampleRate();
            req.initNoiseLevel      = logged.initNoiseLevel;
        }
    }

    // The tape's prompts are NOT written into the editors: the replay overlay
    // covers the panel and displays them itself, so the user's typed prompts stay
    // untouched — no stash/restore needed. (Prompts are GUI-only, not in the APVTS
    // state, so they never travel through the start-patch either.)
    generating = true;
    generateButton.setEnabled(false);
    if (onStatusChanged) onStatusChanged("replay: generating...", true);

    const uint32_t epoch = processorRef.getReplayEpoch();

    auto pipePtr = processorRef.getPipeInferencePtr();
    juce::Component::SafePointer<PromptPanel> safeThis(this);
    std::thread([safeThis, pipePtr, req, epoch]() mutable
    {
        auto inferenceResult = pipePtr->generate(req);
        juce::MessageManager::callAsync([safeThis, result = std::move(inferenceResult), req, epoch]()
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr)
                return;   // editor closed mid-generation; ~PromptPanel already stopped the replay

            auto& processor = self->processorRef;
            self->generating = false;
            self->generateButton.setEnabled(true);
            self->lastReplayGenSucceeded_ = false;

            // Stopped — or a different tape started — while this was in flight. The
            // user is back in live control (or on another timeline), so do not
            // overwrite the sound they are now playing with this tape's audio. The
            // epoch check also stops us releasing a slot that is no longer ours.
            if (! processor.isReplayActive() || processor.getReplayEpoch() != epoch)
            {
                processor.replayGenerationFinished(epoch);
                return;
            }

            if (result.success)
            {
                auto newAudio = result.audio;

                // The identical crossfade-then-load block the drift loop uses, so held
                // voices follow the new sample over Regen XFade through the single
                // existing swap site — no new swap path, invariant preserved.
                const auto& oldRaw = processor.getGeneratedAudioRaw();
                const float xfadeMs = processor.getValueTreeState()
                    .getRawParameterValue(PID::driftCrossfade)->load();
                const int xfadeSamples = juce::roundToInt(xfadeMs * 0.001f * static_cast<float>(result.sampleRate));
                if (xfadeSamples > 0 && oldRaw.getNumSamples() > 0)
                    applyDriftCrossfade(newAudio, oldRaw, xfadeSamples);
                processor.loadGeneratedAudio(newAudio, result.sampleRate);

                // Deliberately NO fingerprints: the tape must not overwrite the
                // user's last-prompt/seed/model/device/embedding state — the overlay
                // displays the tape's identity, the live UI keeps the user's. (Also
                // keeps a quit-during-replay from persisting tape state into the
                // standalone buffer preset; only the SOUND is the tape's, disclosed.)
                self->lastReplayGenSucceeded_ = true;
                if (self->onStatusChanged)
                    self->onStatusChanged(juce::String(result.generationTimeMs / 1000.0f, 1) + "s | replay", false);
            }
            else if (self->onStatusChanged)
            {
                self->onStatusChanged("replay: " + result.errorMessage, false);
            }

            // Release the slot last, whatever happened: the audio thread arms the next
            // logged generation only once this one is done.
            processor.replayGenerationFinished(epoch);
        });
    }).detach();
}

// ──────────────────────────────────────────────────────────────────────────────
// Offline cache take (see the header for what it is for)
// ──────────────────────────────────────────────────────────────────────────────
bool PromptPanel::isAsyncCacheMode() const
{
    return processorRef.getValueTreeState()
               .getRawParameterValue(PID::cacheAsync)->load() > 0.5f;
}

void PromptPanel::stepOfflineTakeAfterCapture(bool captured)
{
    // One step per CAPTURED entry, and only if the freeze held for that whole
    // render. Anything that released it mid-flight (the LRO panel, a starting
    // replay, the capacity set to Off) let the LFOs free-run meanwhile, so adding
    // the full cadence step on top would advance them twice — machine-dependently,
    // in the one feature that exists to be machine-independent.
    if (! captured)
        return;                        // nothing was written: the cache is unchanged
    if (! takeStepArmed_)
    {
        // This entry was recorded live — the switch was off, the mode was Manual, or
        // the freeze broke while it rendered. The cache is then a mixture, and the
        // playback rules that belong to a recording must not apply to it.
        processorRef.setInferenceCacheOfflineTake(false);
        return;
    }
    takeStepArmed_ = false;
    processorRef.setInferenceCacheOfflineTake(true);
    processorRef.requestDriftGenerationStep(static_cast<float>(cadenceIntervalSeconds()));
}

bool PromptPanel::isLroAsyncTakeRecording() const
{
    // The LRO's counterpart, and it means something different here, because a
    // bar is not a unit this side can work in: "async" is the LLM's inference
    // time until the Csound code is finished, and no coding model available
    // today writes an orchestra inside one bar (BJ, 2026-08-22). So a bar
    // interval on this side is not a pace, it is a number that is always
    // already overdue.
    //
    // With the switch on, the interval is therefore not consulted at all: each
    // step WAITS until the code is there and the next re-prompt follows from
    // that. One step per pass, in order, however long any of them took - which
    // is the only sense in which a recorded run on this side can come out the
    // same twice.
    //
    // Manual is left as it is, exactly as on the neural side: no cadence, no
    // pace to hold. Cache Off likewise - the switch records takes, it does not
    // re-pace live play.
    return isAsyncCacheMode()
        && ! easyMode_
        && ! processorRef.isReplayActive()
        && processorRef.getCsoundCacheCapacity() > 0
        && ! processorRef.isCsoundCacheFull()
        && processorRef.driftRegenMode.load(std::memory_order_relaxed) != 0;
}

bool PromptPanel::isAsyncTakeRecording() const
{
    // Manual is left exactly as it was: there is no cadence to step, a take fills
    // one Generate press at a time and the drift keeps running between presses.
    // Cache Off likewise — the switch records takes, it does not re-pace live play.
    return isAsyncCacheMode()
        && easyMode_
        && ! processorRef.isReplayActive()
        && processorRef.isInferenceCacheActive()
        && ! processorRef.isInferenceCacheFull()
        && driftCanCarryATake()
        && processorRef.driftRegenMode.load(std::memory_order_relaxed) != 0;
}

bool PromptPanel::driftCanCarryATake() const
{
    // A take needs a Drift slot that actually MOVES a generation parameter: an armed
    // target with its Amount still at zero moves nothing, and a take engaged on it
    // would freeze nothing while overriding the change gate — i.e. render the SAME
    // request into every entry of the cache, unasked. Amount defaults to 0, so the
    // natural order (pick the target, then turn the knob up) passes straight through
    // that state, and parking a drift by turning Amount back down returns to it.
    //
    // Read the USER's Amount, not the modulated one: an LFO→Drift-Amt routing crosses
    // zero every cycle, and a precondition that follows it would make the freeze flap
    // in the middle of a recording. A drift whose depth comes ONLY from such a routing
    // therefore records live, exactly as it did before this switch existed.
    auto& apvts = processorRef.getValueTreeState();
    static constexpr struct { const char* target; const char* depth; } slots[] = {
        { PID::drift1Target, PID::drift1Depth },
        { PID::drift2Target, PID::drift2Depth },
        { PID::drift3Target, PID::drift3Depth },
    };
    for (const auto& slot : slots)
    {
        const int target = static_cast<int>(apvts.getRawParameterValue(slot.target)->load());
        if (DriftLFO::isGenerationTarget(target)
            && std::abs(apvts.getRawParameterValue(slot.depth)->load()) > 0.0f)
            return true;
    }
    return false;
}

double PromptPanel::cadenceIntervalSeconds() const
{
    // Same table as the cooldown below, with ASAP resolved to its 1-beat floor.
    static constexpr int beatCounts[] = { 0, 1, 4, 8, 16, 32, 64 }; // man,asap,1/2/4/8/16 bar
    const int mode = juce::jlimit(0, 6,
        processorRef.driftRegenMode.load(std::memory_order_relaxed));
    const float bpm = processorRef.driftRegenBpm.load(std::memory_order_relaxed);
    return (beatCounts[mode] * 60.0) / static_cast<double>(juce::jmax(1.0f, bpm));
}

// ──────────────────────────────────────────────────────────────────────────────
// Drift regen polling (called from timerCallback at 10 Hz)
// ──────────────────────────────────────────────────────────────────────────────
void PromptPanel::pollDriftRegen()
{
    // Offline take: freeze or release the generation-side drift BEFORE any early
    // return below. Most of them — a render in flight, a Re-Prompt step being
    // written, the cadence cooldown, the LCO panel — are precisely the states the
    // freeze exists to survive, so a hold applied further down would leak.
    const bool holdGenDrift = isAsyncTakeRecording();
    if (holdGenDrift != lastDriftGenHoldSent_)
    {
        processorRef.setDriftGenerationHold(holdGenDrift);
        lastDriftGenHoldSent_ = holdGenDrift;
        if (! holdGenDrift)
            takeStepArmed_ = false;   // freeze broke: a render in flight no longer earns its step
    }

    // A running tape owns the generation pipe: its own logged generations drive the
    // timbre, and an auto-regen firing alongside them would overwrite the sound the
    // replay just faded in.
    if (processorRef.isReplayActive()) return;

    // loopStepInFlight_ too: while a semantic-loop step is analyzing+interpreting on
    // a background thread (`generating` is already false), Auto-Regen must not fire a
    // new generate() — both share the single IPC pipe. The step clears the flag on
    // its callAsync tail; the next tick then proceeds with the rewritten prompts.
    if (generating || translatingPrompts_ || loopStepInFlight_) return;

    // Paused while the DCO panel is shown: auto-regen renders the HIDDEN neural
    // prompts and would silently clobber a baked DCO table every cycle. The
    // loop resumes on the next tick after switching back to Easy — cadence
    // state (bar counters, stance) is untouched.
    if (!easyMode_) return;

    // stanceActive: an active Re-Prompt stance. Read once here; reused below for the
    // repromptLoop standing trigger and the idle-cache bypass (a running stance keeps
    // rendering fresh audio to listen to). It does NOT override the regen mode.
    const bool stanceActive = static_cast<int>(processorRef.getValueTreeState()
        .getRawParameterValue(PID::repromptStance)->load()) != RepromptStance::Off;

    int regenMode = processorRef.driftRegenMode.load(std::memory_order_relaxed);
    // Manual (regenMode 0) is authoritative: NO auto-regen, even with a stance engaged.
    // The Re-Prompt loop still advances in Manual, one step per manual Generate press:
    // triggerGeneration's gen-complete callback runs runSemanticLoopStep, which writes
    // the next prompt; the next press renders it. ASAP / N-bars drive the loop
    // automatically via the repromptLoop standing trigger below. (This early-out was
    // previously gated on !stanceActive, so a stance force-ran the loop in Manual --
    // making Manual ignore its own setting, reported as "even in Manual it
    // autogenerates". The regen switchbox is the single cadence control for the stance
    // loop too.)
    if (regenMode == 0)
    {
        return;                         // Manual: no auto-regen; stance loop steps per Generate press
    }

    // Failure throttle: when the previous drift-driven regen failed (e.g.
    // the backend rejected the selected model), wait ~2s before retrying so
    // the 10 Hz timer can't spin and oscillate the status label.
    if (lastRegenFailureMs_ > 0.0)
    {
        constexpr double FAILURE_COOLDOWN_MS = 2000.0;
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if ((nowMs - lastRegenFailureMs_) < FAILURE_COOLDOWN_MS)
            return;
    }

    // Idle-CPU guard: cache replay runs the full loadGeneratedAudio path
    // (rumble/HF/normalize + WT frame extraction) per cycle, and at idle no
    // voice picks up the new buffer — pure wasted work. Real-inference
    // generation (cache-fill path) is allowed to continue so the cache can
    // still pre-fill while the user is not playing.
    const bool fullCachePlayback = processorRef.isInferenceCacheFull();
    // A finished offline take is played back like any other cache, stance or no
    // stance: the take IS the recorded evolution, so it replays instead of being
    // overridden by a live loop that would re-render it differently every cycle.
    // The Re-Prompt exception below therefore stops applying once such a take is full.
    const bool asyncTakePlayback = fullCachePlayback && processorRef.isInferenceCacheOfflineTake();
    if (fullCachePlayback
        && processorRef.audioIdle.load(std::memory_order_relaxed)
        && (!stanceActive || asyncTakePlayback))   // Re-Prompt must keep rendering fresh audio to listen to
        return;

    // Bar-based cooldown: modes 2-6 = iterate every 1/2/4/8/16 bars (1 bar = 4
    // beats, expressed in beats below so the BPM→ms math is unchanged). When the
    // inference cache is full, ASAP (mode 1) is throttled to a 1-beat floor so
    // cache playback cannot run at the GUI polling rate.
    // The same 1-beat floor covers a RECORDING offline take: its step advances the
    // drift by one cadence interval, and ASAP has none — every entry would sit on
    // the same drift value and the take would record one sound N times.
    const bool asapFloor = (regenMode == 1) && (fullCachePlayback || holdGenDrift);
    if (regenMode >= 2 || asapFloor)
    {
        static constexpr int beatCounts[] = { 0, 0, 4, 8, 16, 32, 64 }; // man,asap,1/2/4/8/16 bar
        int beats = asapFloor
            ? 1
            : beatCounts[juce::jlimit(0, 6, regenMode)];
        float bpm = processorRef.driftRegenBpm.load(std::memory_order_relaxed);
        double cooldownMs = (beats * 60000.0) / static_cast<double>(juce::jmax(1.0f, bpm));
        double now = juce::Time::getMillisecondCounterHiRes();
        if ((now - lastRegenTimeMs_) < cooldownMs)
            return; // cooldown not elapsed
    }

    // Read effective drift values from processor atomics
    auto& mv = processorRef.modulatedValues;
    float effAlpha = mv.driftAlpha.load(std::memory_order_relaxed);
    float dAxis1 = mv.driftAxis1.load(std::memory_order_relaxed);
    float dAxis2 = mv.driftAxis2.load(std::memory_order_relaxed);
    float dAxis3 = mv.driftAxis3.load(std::memory_order_relaxed);
    float effNoise = mv.driftNoise.load(std::memory_order_relaxed);
    float effMag   = mv.driftMagnitude.load(std::memory_order_relaxed);
    // Effective Resynth amount: the drifted (base+offset) value when a Drift slot
    // sweeps it, otherwise the live slider. Drives the standing feedback-loop trigger
    // below AND is passed through as the init_audio amount to buildInferenceRequest.
    float driftResynthVal = mv.driftResynth.load(std::memory_order_relaxed);
    float effResynth = std::isnan(driftResynthVal)
        ? processorRef.getValueTreeState().getRawParameterValue(PID::resynthAmount)->load()
        : driftResynthVal;

    // Build effective axes: AxesPanel base values + per-slot drift offsets
    std::map<juce::String, float> effAxes;
    if (getAxisValuesCallback)
    {
        float o1 = std::isnan(dAxis1) ? 0.0f : dAxis1;
        float o2 = std::isnan(dAxis2) ? 0.0f : dAxis2;
        float o3 = std::isnan(dAxis3) ? 0.0f : dAxis3;
        effAxes = getAxisValuesCallback(o1, o2, o3);
    }

    // Check if values changed enough from last generation
    constexpr float DRIFT_THRESHOLD = 0.005f;

    // Mode-specific drift mapping: the same alpha-LFO offset (in alpha-units)
    // drives a different parameter per mode. This keeps the Drift Panel UX
    // simple ("Alpha" target works in all modes) while ensuring drift actually
    // moves audible parameters in Step-in/Layer.
    auto& apvts = processorRef.getValueTreeState();
    const float baseAlphaForOff = apvts.getRawParameterValue(PID::genAlpha)->load();
    const float alphaOff = std::isnan(effAlpha) ? 0.0f : (effAlpha - baseAlphaForOff);

    float effectiveLateMix    = lateMixForMode(injectionMode_);
    float effectiveSplitStart = splitLayerStart_;
    float effectiveSplitEnd   = splitLayerEnd_;
    const bool fineLikeMode = (injectionMode_ == "late_step"
                            || injectionMode_ == "kombi1"
                            || injectionMode_ == "kombi2"
                            || injectionMode_ == "kombi3");
    if (fineLikeMode && std::abs(alphaOff) > 0.001f)
    {
        effectiveLateMix = juce::jlimit(0.0f, 1.0f, lateMixForMode(injectionMode_) + alphaOff * 0.25f);
    }
    else if (injectionMode_ == "layer_split" && std::abs(alphaOff) > 0.001f)
    {
        const float driftBlocksF = static_cast<float>(ditBlocks_);
        const float width = splitLayerEnd_ - splitLayerStart_;
        const float maxStart = std::max(0.0f, driftBlocksF - width);
        // Drift sweeps half the block range per unit of alphaOff (8 blocks
        // for SAO Small's 16-deep DiT, scaled for SA3-sized models).
        const float sweepScale = driftBlocksF * 0.5f;
        effectiveSplitStart = juce::jlimit(0.0f, maxStart, splitLayerStart_ + alphaOff * sweepScale);
        effectiveSplitEnd   = effectiveSplitStart + width;
    }

    // Alpha-driven auto-regen routes to the mode's parameter:
    // Linear → α, Step-in → lateMix, Layer → splitStart (split end follows).
    bool alphaChanged = false;
    if (injectionMode_ == "linear")
    {
        alphaChanged = !std::isnan(effAlpha)
            && (std::isnan(lastGenAlpha_) || std::abs(effAlpha - lastGenAlpha_) > DRIFT_THRESHOLD);
    }
    else if (fineLikeMode)
    {
        alphaChanged = std::isnan(lastGenLateMix_)
            || std::abs(effectiveLateMix - lastGenLateMix_) > DRIFT_THRESHOLD;
        // Suppress regen on initial frame when nothing has moved yet.
        if (std::isnan(lastGenLateMix_) && std::abs(alphaOff) < 0.001f)
            alphaChanged = false;
    }
    else if (injectionMode_ == "layer_split")
    {
        // Detect change on EITHER thumb — drift moves both synchronously, but
        // a manual drag of just the upper thumb changes splitEnd without
        // touching splitStart, so we need both comparisons.
        const bool startChanged = std::isnan(lastGenSplitStart_)
            || std::abs(effectiveSplitStart - lastGenSplitStart_) > DRIFT_THRESHOLD;
        const bool endChanged = std::isnan(lastGenSplitEnd_)
            || std::abs(effectiveSplitEnd - lastGenSplitEnd_) > DRIFT_THRESHOLD;
        alphaChanged = startChanged || endChanged;
        // Suppress regen on the initial frame when nothing has moved yet.
        if (std::isnan(lastGenSplitStart_) && std::isnan(lastGenSplitEnd_)
            && std::abs(alphaOff) < 0.001f)
            alphaChanged = false;
    }

    bool noiseChanged = !std::isnan(effNoise) &&
        (std::isnan(lastGenNoise_) || std::abs(effNoise - lastGenNoise_) > DRIFT_THRESHOLD);

    bool magChanged = !std::isnan(effMag) &&
        (std::isnan(lastGenMagnitude_) || std::abs(effMag - lastGenMagnitude_) > DRIFT_THRESHOLD);

    // Resynth feedback loop — a STANDING trigger, not change-detection: while Resynth
    // is active (amount > 0) and a non-Manual regen mode is engaged, keep regenerating
    // at the selected cadence (auto / 1-bar / 4-bar, throttled by the beat cooldown
    // above and the `generating` guard at the top). Each cycle re-feeds the last
    // output as init_audio (buildInferenceRequest reads getGeneratedAudioRaw), so the
    // sound evolves on its own with no slider movement — same shape as random-seed
    // regen. SA3-gated to match buildInferenceRequest's attach gate: init_audio only
    // attaches on SA3, so looping elsewhere would spin identical text-only renders.
    const bool resynthLoop = selectedModelIsSA3() && effResynth > 0.01f;
    if (!resynthLoop)
        prevLoopParamsChanged_ = false;   // re-arm the release edge-detector for next time

    // Re-Prompt word loop — a STANDING trigger too: when a stance is engaged but the
    // SA3 init_audio carry is NOT already clocking the loop (resynthLoop covers SA3 +
    // Resynth>0), provide the trigger ourselves so Auto-Regen keeps re-rendering the
    // rewritten prompts. Covers non-SA3 entirely AND SA3-with-Resynth-off. Needed
    // because runSemanticLoopStep deliberately suppresses promptChanged (to protect
    // the SA3 release edge), so without a standing trigger the loop would advance only
    // once. The SA3-only resynth control machinery below stays gated on resynthLoop —
    // a pure word loop renders clean from the prompt (no init_audio attaches off-SA3
    // or at amount 0, per buildInferenceRequest's gate), so loopResynth stays inert.
    const bool repromptLoop = stanceActive && ! resynthLoop;   // stanceActive: see top

    auto promptA = promptAEditor.getText().trim();
    auto promptB = promptBEditor.getText().trim();
    bool promptChanged = (promptA != lastGenPromptA_) || (promptB != lastGenPromptB_);

    bool axesChanged = false;
    for (auto& [key, val] : effAxes)
    {
        auto it = lastGenAxes_.find(key);
        if (it == lastGenAxes_.end() || std::abs(val - it->second) > DRIFT_THRESHOLD)
        {
            axesChanged = true;
            break;
        }
    }

    bool randomRegen = processorRef.getLastRandomSeed();
    // While an offline take records, the CADENCE is the trigger and this gate must
    // not apply: the parameters stand still BY DESIGN, so "did anything move?" is
    // the wrong question — and asking it deadlocks the take, because with the
    // freeze on nothing outside this loop can ever move them again. Two ways in:
    // a render that fails leaves the trackers holding the frozen values and no
    // step is taken, and a step whose delta lands under DRIFT_THRESHOLD (near a
    // waveform's turning point a half-cycle step is nearly self-cancelling) reads
    // as "unchanged". Either one froze alpha/noise/magnitude/axes for good until
    // the user toggled the switch. The cooldown above already paces the take.
    if (!holdGenDrift
        && !alphaChanged && !axesChanged && !noiseChanged && !magChanged && !promptChanged
        && !resynthLoop && !repromptLoop && !randomRegen)
        return;

    // genNoise/genMag are pre-resolved to their slider values; alpha is
    // intentionally NOT — pass the drift sentinel (NaN when alpha is not being
    // modulated) straight through. buildInferenceRequest resolves NaN→slider
    // identically (see its top), so the rendered alpha is unchanged; preserving
    // the sentinel is what lets its single-prompt guard tell the plain slider
    // default apart from a genuine alpha sweep. Pre-resolving here (the old bug)
    // handed the guard a concrete 0.0 on every non-alpha drift regen — including
    // the promptChanged tick fired the instant a lone prompt is typed — so the
    // guard silently skipped and the linear blend collapsed to null.
    float genNoise = std::isnan(effNoise)
        ? apvts.getRawParameterValue(PID::genNoise)->load() : effNoise;
    float genMag = std::isnan(effMag)
        ? apvts.getRawParameterValue(PID::genMagnitude)->load() : effMag;

    // Resynth-loop control, gated on whether a t5osc PARAMETER moved:
    //
    //   • a parameter change EDGE → RELEASE: detach init (loopResynth → 0, below the
    //     attach gate) so the new content renders clean FROM THE PROMPT instead of
    //     being anchored to the carried-over wave. The re-lock is automatic: the
    //     next tick falls through to the LOCK branch and init re-attaches at the
    //     set level — now AGREEING with the new content, so it holds.
    //   • otherwise → LOCK: init stays attached at the user's resynth amount
    //     (floored at kResynthLoopFloor); the loop follows that setting with no
    //     auto-correction.
    //
    // EDGE-triggered, not level: paramsChanged is "moved since the last loop regen",
    // which a CONTINUOUS drift (sine/triangle — the default waveforms) holds true
    // every tick. Releasing on the level would detach every tick and silently
    // disable the feedback loop (its headline "evolve with drift" use). Firing only
    // on the false→true edge makes a square/discrete change release once per flip,
    // and a continuous drift release once at onset and then lock-and-evolve.
    //
    // Empirically validated for the discrete switch (tools/test_ab_resynth.py): with
    // init attached at ANY sigma a switched-in prompt only reaches a half-bright mush
    // attractor (cen×B ≈ 0.06–0.44); detach-then-relock lands it clean (cen×B 1.0 on
    // the switch) and HOLDS it (≥1.0) at every Resynth level — tighter the higher the
    // level, since the carried wave then matches the prompt.
    const bool paramsChanged = alphaChanged || axesChanged || noiseChanged
                            || magChanged || promptChanged;
    const bool releaseEdge = resynthLoop && paramsChanged && !prevLoopParamsChanged_;
    float loopResynth = effResynth;
    if (releaseEdge)
    {
        loopResynth = 0.0f;            // RELEASE: detach this round → render clean
    }
    else if (resynthLoop)
    {
        loopResynth = juce::jlimit(kResynthLoopFloor, 1.0f, effResynth);  // LOCK: follow the user's setting
    }
    if (resynthLoop)
        prevLoopParamsChanged_ = paramsChanged;  // arm the edge test for the next regen

    lastRegenTimeMs_ = juce::Time::getMillisecondCounterHiRes();
    triggerDriftRegeneration(effAlpha, effAxes, genNoise, genMag,
                             effectiveLateMix, effectiveSplitStart, effectiveSplitEnd,
                             loopResynth, false);
}

// ──────────────────────────────────────────────────────────────────────────────
// LCO Re-Prompt cadence (called from timerCallback at 10 Hz)
// ──────────────────────────────────────────────────────────────────────────────
// The LCO twin of pollDriftRegen's stance loop. The SAME REGENERATE switchbox
// (drift_regen + driftRegenBpm) paces both paradigms: the rate control the user
// already knows works here too, and there is no second cadence control to keep in
// sync. What the paradigm boundary separates is the STANCE (dcoRepromptStance vs
// repromptStance) — the clock is shared, deliberately.
//
// Fires triggerDcoReprompt(), NOT triggerLcoGenerate(): a cadence step is always
// "listen → rewrite → bake" (the step's own tail bakes). Without a stance there is
// nothing to step, and falling through to a bake would re-author the SAME prompt
// every cycle — replacing a working instrument with another roll of the same dice
// that the user never asked for.
void PromptPanel::pollLcoRepromptCadence()
{
    // Is a take still recording? Asked BEFORE every early return below, because
    // most of those returns - the panel switched away, the stance turned off, a
    // manual press writing Manual over the cadence - are exactly the states this
    // has to notice. Without it the flag has no release path at all: it would
    // survive the pass it belonged to and stamp whichever later one landed.
    //
    // But NOT while a pass is in flight. Unlike the neural side, where the same
    // conditions ARE the parameter freeze and breaking one really does spoil the
    // render, nothing here reaches into a pass that is already running: the
    // authoring happens on a detached thread from the prompt the cadence handed
    // it. A step started by the recording cadence stays a step of that
    // recording, even if the player looks at the other panel for two seconds
    // while it runs - and that mattered most on the pass that FILLS the cache,
    // where a disarm turned the whole recorded run into a live one.
    {
        const bool recording = isLroAsyncTakeRecording();
        if (recording != lastLroTakeRecording_)
        {
            lastLroTakeRecording_ = recording;
            if (! recording && ! dcoRepromptBusy_ && ! dcoBaking_)
                lroTakeStepArmed_ = false;
        }
    }

    // Easy/neural panel: pollDriftRegen owns the cadence there, and its mirror of
    // this line (`if (!easyMode_) return;`) makes the two mutually exclusive.
    if (easyMode_) return;

    // A running tape owns the generation pipe — same reason as pollDriftRegen's
    // identical gate: its logged generations drive the timbre, and a step firing
    // alongside them would overwrite the sound the replay just faded in.
    if (processorRef.isReplayActive()) return;

    // The stance is what makes an automatic cadence meaningful at all (see above).
    const int stance = static_cast<int>(processorRef.getValueTreeState()
                          .getRawParameterValue(PID::dcoRepromptStance)->load());
    if (stance == RepromptStance::Off) return;

    // Manual is authoritative here exactly as it is on the neural side: NO auto
    // stepping, even with a stance engaged. The loop still advances one step per
    // GENERATE press, since triggerLcoGenerate routes an engaged stance into
    // triggerDcoReprompt.
    const int regenMode = processorRef.driftRegenMode.load(std::memory_order_relaxed);
    if (regenMode == 0) return;

    // A full cache is not stepped over. triggerDcoBake would REPLAY it (its own
    // gate says so) and throw the step's result away - but the step still costs a
    // bare-oscillator render, a listen and two model turns first, every cadence
    // tick, for a rewrite nobody will hear. What was recorded is travelled by the
    // CACHE row and the aftertouch bar from here on, not written over.
    // Same condition as the replay gate, deliberately.
    if (processorRef.isCsoundCacheFull())
        return;

    // Nothing to listen to yet, or the last step found the orchestra unlistenable.
    // Both are conditions under which triggerLcoGenerate AUTHORS instead of
    // stepping — and a cadence must not author (see above), so it waits for a
    // GENERATE press rather than stepping into the same failure every N bars.
    if (! processorRef.hasCsoundOrchestra() || dcoEarFailed_) return;

    // No language model: triggerDcoReprompt would refuse and write a status. Gate
    // it here so the cadence stays silent instead of restating that every N bars.
    if (! llmAvailable_) return;

    // Busy gates, mirroring triggerDcoReprompt's own. It re-checks all of them, but
    // returning BEFORE the timestamp advances is the point: a step that never ran
    // must not consume its cadence slot.
    if (dcoRepromptBusy_ || dcoBaking_ || csoundCompileWatching_) return;
    if (generating || translatingPrompts_ || loopStepInFlight_) return;

    // Failure throttle, pollDriftRegen's lastRegenFailureMs_ twin: a step whose ear
    // or rewrite failed leaves no busy flag standing, and the unreachable-ear case is
    // deliberately NOT latched into dcoEarFailed_ (a backend still starting is a wait,
    // not a broken instrument) — so in ASAP an armed cadence would otherwise retry at
    // the 10 Hz timer rate, each retry taking the process-wide Csound lifecycle lock
    // for the probe against the live engine. 2 s, same figure as the neural side.
    if (lastLcoStepFailureMs_ > 0.0)
    {
        constexpr double FAILURE_COOLDOWN_MS = 2000.0;
        if ((juce::Time::getMillisecondCounterHiRes() - lastLcoStepFailureMs_) < FAILURE_COOLDOWN_MS)
            return;
    }

    // Bar cooldown, identical math to pollDriftRegen's: modes 2-6 = every
    // 1/2/4/8/16 bars at the REGENERATE BPM, measured from the step's START so a
    // slow step eats into its own interval rather than adding to it. ASAP (mode 1)
    // has no cooldown — the busy gates above are its floor, and one LCO step is
    // seconds of real work (probe render + ear + two model calls + compile), never
    // the cached replay that ASAP is throttled against on the neural side.
    //
    // ...and while a take is recording, no cooldown either, whatever the bars
    // say: the switch means "wait until the code is there, then go on" (see
    // isLroAsyncTakeRecording), and the busy gates above are that wait. Asked
    // before the block rather than inside it so ASAP and the five bar settings
    // behave identically under the switch, instead of the interval quietly
    // applying to five of the six.
    if (regenMode >= 2 && ! lastLroTakeRecording_)
    {
        static constexpr int beatCounts[] = { 0, 0, 4, 8, 16, 32, 64 }; // man,asap,1/2/4/8/16 bar
        const int    beats = beatCounts[juce::jlimit(0, 6, regenMode)];
        const float  bpm   = processorRef.driftRegenBpm.load(std::memory_order_relaxed);
        const double cooldownMs = (beats * 60000.0) / static_cast<double>(juce::jmax(1.0f, bpm));
        if ((juce::Time::getMillisecondCounterHiRes() - lastLcoStepTimeMs_) < cooldownMs)
            return;
    }

    // Does this step earn a cache point recorded as a take? Asked before the
    // step, like the neural takeStepArmed_, because the answer must be the state
    // the step STARTED in - the cache fills during it. (Read from the value the
    // hold-break check at the top already took, so the one answer governs both
    // the pacing above and the stamp.)
    lroTakeStepArmed_ = lastLroTakeRecording_;
    lastLcoStepTimeMs_ = juce::Time::getMillisecondCounterHiRes();
    triggerDcoReprompt();
}

// ──────────────────────────────────────────────────────────────────────────────
// Semantic self-listening loop step (CLAP ear → LLM interpreter → next prompt)
// ──────────────────────────────────────────────────────────────────────────────
// One iteration body of clap_llm_loop.py run_llm_loop, minus its own generate():
// here generation is owned by triggerGeneration / triggerDriftRegeneration, so this
// step runs the analyze+interpret+apply for the JUST-rendered audio and writes the
// next prompt(s) into the editor(s). The next generation renders them — manual
// Generate, or the Auto-Regen standing trigger (resynthLoop on SA3 + Resynth,
// repromptLoop otherwise). On SA3 with Resynth the init_audio carry is forced to
// init_noise ~0.9 so the rewritten words dominate it; off-SA3 (or Resynth off) it is
// a pure word loop that re-renders fresh from the prompt. From BOTH gen-complete cbs.
void PromptPanel::runSemanticLoopStep(const PipeInference::Result& result)
{
    // (1) Off early-out FIRST (cheap), like pollDriftRegen's regenMode==0.
    auto& apvts = processorRef.getValueTreeState();
    const int stanceIdx = static_cast<int>(apvts.getRawParameterValue(PID::repromptStance)->load());
    if (stanceIdx == RepromptStance::Off)
    {
        loopEngaged_ = false;   // loop disabled → re-arm the original-capture edge
        // INVARIANT: do NOT touch loopOriginalsValid_ / prevStanceForRestore_ here —
        // they are timer-owned and must survive this clear so the deactivation-restore
        // still fires on the next tick even if a gen-complete callback observes Off
        // before timerCallback does. Writing them here would let machine text leak into
        // the next engage capture.
        return;
    }
    // NO model gate: Re-Prompt is engine-agnostic — it listens to the just-rendered
    // output (any model) and rewrites the prompt(s) for the next render. Only the
    // OPTIONAL init_audio signal carry (Resynth) is SA3-only, layered on top by
    // buildInferenceRequest when present; non-SA3 simply runs the pure word loop
    // (fresh render from the rewritten prompt each step).
    // Model gate: the interpreter REQUIRES the language model. Without it every
    // interpret() errors and the loop falls back to the previous prompt each step,
    // an invisible no-op. Bail instead. The UI also disables the stance bar; this
    // covers a preset/automation that engaged a stance while the model is absent.
    if (! llmAvailable_)
        return;
    // (2) Re-entrancy guard: a fast Auto-Regen cadence must not stack steps, and a
    //     manual Generate mid-step must not contend on the single IPC pipe.
    if (loopStepInFlight_ || generating || translatingPrompts_)
        return;
    if (result.audio.getNumSamples() <= 0 || result.audio.getNumChannels() <= 0)
        return;   // nothing to listen to

    const int couplingIdx = static_cast<int>(apvts.getRawParameterValue(PID::repromptCoupling)->load());
    const bool dual = (couplingIdx == RepromptCoupling::AbAdd || couplingIdx == RepromptCoupling::AbReplace);
    const bool concat = (couplingIdx == RepromptCoupling::AbAdd);

    // Resolve the stance KEY (BlockParams.h RepromptStance::kEntries[i].key == the
    // clap_llm_loop MODES id RepromptStances expects). Guard the index defensively.
    const int sIdx = juce::jlimit(0, RepromptStance::kCount - 1, stanceIdx);
    const juce::String stanceKey = RepromptStance::kEntries[sIdx].key;

    // transcribe collapses A==B under AB-replace (it reads only the shared machine tags),
    // which leaves alpha/alpha-drift nothing to interpolate. In that one case, write a
    // single pole per step (alternating) so A and B stay distinct consecutive snapshots.
    // Other stances read per-pole memory and stay distinct; AB-add keeps the originals.
    const bool altReplace = (stanceKey == "transcribe")
                         && (couplingIdx == RepromptCoupling::AbReplace);

    // (5 + run_llm_loop glieder init) Capture the human originals on the false→true
    // engage edge: a_glieder=[header_a], b_glieder=[header_b]. The "header" is the
    // text the user has in the editors when the loop first engages; subsequent links
    // are machine-written. Seed last-link + recent from the original (recent =
    // glieder[-3:], which on iter 1 is just [original]).
    if (!loopEngaged_)
    {
        loopEngaged_ = true;
        // Only (re)capture the originals on a GENUINE fresh engage. A stance blip
        // (the Off early-out re-arms loopEngaged_ without restoring the editors) must
        // not reset the loop's evolving memory mid-run.
        if (!loopOriginalsValid_)
        {
            // Source the "human original" from the DURABLE human-prompt store, NOT the
            // live editors: after a buffer reload or a missed deactivation-restore the
            // editors can already hold a machine rewrite, and reading them here is what
            // poisoned the saved seed (then re-poisoned it every buffer round-trip).
            // The store is written only by human authorship. Fall back to the editors
            // only if nothing has been authored yet this session.
            const bool haveHuman = processorRef.hasHumanPrompts();
            loopOriginalA_ = haveHuman ? processorRef.getHumanPromptA() : promptAEditor.getText().trim();  // FULL (with any musical suffix) — the restore puts this back verbatim
            loopOriginalB_ = haveHuman ? processorRef.getHumanPromptB() : promptBEditor.getText().trim();
            // Hold the user's trailing pitch/tempo tokens (c3, 120bpm) aside, run the
            // whole chain on the descriptive CORE, re-append on every editor write — so
            // no stance ever paraphrases or drops them. Empty when none were appended.
            loopSuffixA_ = RepromptStances::trailingMusicSuffix(loopOriginalA_);
            loopSuffixB_ = RepromptStances::trailingMusicSuffix(loopOriginalB_);
            const juce::String coreA0 = RepromptStances::stripMusicSuffix(loopOriginalA_);
            const juce::String coreB0 = RepromptStances::stripMusicSuffix(loopOriginalB_);
            loopLastA_ = coreA0;
            loopLastB_ = coreB0;
            loopRecentA_.clearQuick(); loopRecentA_.add(coreA0);
            loopRecentB_.clearQuick(); loopRecentB_.add(coreB0);
            loopOriginalsValid_ = true;   // arm the stance→Off restore (timerCallback)
            // Back-fill the store for a from-empty engage (no prior load/typing).
            if (!haveHuman)
                processorRef.setHumanPrompts(loopOriginalA_, loopOriginalB_);
        }
        loopAltWriteA_ = false;       // each new session starts by writing B (the primary pole)
    }

    // Which pole this step writes. Alternation is active in altReplace (transcribe) AND
    // in dual non-altReplace (voll/concat) — both now run ONE interpret per step and
    // flip the driven pole. Alpha (non-dual, non-altReplace) never alternates: always B.
    const bool altWriteA = (altReplace || dual) && loopAltWriteA_;

    loopStepInFlight_ = true;

    // Snapshot the per-pole interpret inputs for the background thread (the chain's
    // OWN last link + that pole's last-3 memory — NOT the applied/concat editor text).
    const juce::String prevA = loopLastA_, prevB = loopLastB_;
    juce::StringArray recentA = loopRecentA_, recentB = loopRecentB_;
    // CORE bases for concat2 (the musical suffix is re-appended at the editor write,
    // so it stays trailing instead of landing mid-string inside the concat).
    const juce::String origA = RepromptStances::stripMusicSuffix(loopOriginalA_);
    const juce::String origB = RepromptStances::stripMusicSuffix(loopOriginalB_);

    auto pipePtr = processorRef.getPipeInferencePtr();
    const juce::String device = defaultInferenceDevice_;
    // The just-rendered audio (CLAP listens to THIS, the source of the next prompt).
    juce::AudioBuffer<float> audioForClap;
    audioForClap.makeCopyOf(result.audio);
    const double sr = result.sampleRate;

    juce::Component::SafePointer<PromptPanel> safeThis(this);
    std::thread([safeThis, pipePtr, device, audioCopy = std::move(audioForClap), sr,
                 stanceKey, dual, concat, altReplace, altWriteA,
                 prevA, prevB, recentA, recentB, origA, origB]() mutable
    {
        if (pipePtr == nullptr) {
            juce::MessageManager::callAsync([safeThis] {
                if (auto* s = safeThis.getComponent()) s->loopStepInFlight_ = false; });
            return;
        }

        // CLAP ear: top-5 timbre tags + DSP spectral words (topk=5, default device).
        // Serialises behind any in-flight generate() via PipeInference's recursive
        // mutex (the loopStepInFlight_/generating guards cover the whole span).
        const auto an = pipePtr->analyze(audioCopy, sr, 5, {});
        const juce::String tags = an.success ? an.tags : juce::String();
        const juce::String spectral = an.success ? an.spectral : juce::String();

        // One interpret PER DRIVEN POLE — never copy one run into both poles.
        // next = cleanPrompt(interpret(stanceSysp, userTurn)) OR the chain's last
        // link on empty (run_llm_loop's `or *_glieder[-1]`).
        const juce::String sysp = RepromptStances::stanceSystemPrompt(stanceKey);
        auto interpretPole = [&](const juce::String& prev, const juce::StringArray& recent,
                                 const juce::String& fallback) -> juce::String
        {
            const juce::String userTurn =
                RepromptStances::buildStanceUserTurn(stanceKey, tags, prev, recent, spectral);
            // 0 == no cap; cleanPrompt bounds the result instead (see the DCO
            // twin's call site above for why a hard token limit is wrong here).
            auto r = pipePtr->interpret(sysp, userTurn, 0, device);
            const juce::String cleaned =
                r.success ? RepromptStances::cleanPrompt(r.text) : juce::String();
            return cleaned.isNotEmpty() ? cleaned : fallback;
        };

        // ONE interpret per loop step (never two): the reprompt LLM inferences are
        // linearised and the driven pole alternates A↔B, exactly the pattern the
        // altReplace/transcribe path already uses.
        //   • altReplace (transcribe): pole-independent CLAP tags (B inputs), routed
        //     to the alternating pole in the tail.
        //   • dual non-altReplace (voll/concat): was TWO interprets per step; now one,
        //     alternating via altWriteA — each pole evolves at half cadence.
        //   • alpha (non-dual): B only.
        const bool writeAthisStep = dual && !altReplace && altWriteA;
        const juce::String nextPole = writeAthisStep
            ? interpretPole(prevA, recentA, prevA)
            : interpretPole(prevB, recentB, prevB);

        juce::MessageManager::callAsync(
            [safeThis, dual, concat, altReplace, altWriteA, nextPole, origA, origB]
        {
            auto* self = safeThis.getComponent();
            if (self == nullptr) return;   // panel gone — nothing to write; guard auto-clears with the object

            // Bail (without applying) if the loop was deactivated while this step was
            // interpreting — do NOT clobber the human originals the deactivation-restore
            // (timerCallback) put back with this now-stale rewrite. Two signals:
            //   • stance reads Off — the plain deactivate, tail running before the restore.
            //   • !loopEngaged_ — a restore already ran (it clears loopEngaged_, and a
            //     re-activation does NOT set it back; only the next capture does). This
            //     covers the Off→on ABA flicker that a bare stance==Off check would miss,
            //     which would otherwise apply the stale rewrite AND poison the next
            //     capture's "originals" with machine text.
            if (! self->loopEngaged_
                || static_cast<int>(self->processorRef.getValueTreeState()
                       .getRawParameterValue(PID::repromptStance)->load()) == RepromptStance::Off)
            {
                self->loopStepInFlight_ = false;
                return;
            }

            // transcribe + AB-replace: write ONE pole this step (alternating), leaving
            // the other untouched, so A and B stay distinct snapshots of consecutive
            // renders — an A/B axis for alpha-drift instead of an instant A==B collapse.
            // nextPole carries the transcription (transcribe is pole-independent).
            if (altReplace)
            {
                // loopLast_/recent_ keep the CORE (nextPole); only the editor gets the
                // pole's preserved pitch/tempo suffix re-appended.
                if (altWriteA)
                {
                    self->loopLastA_ = nextPole;
                    self->pendingLoopPromptA_ = RepromptStances::reattachMusicSuffix(nextPole, self->loopSuffixA_);
                    self->loopRecentA_.add(nextPole);
                    while (self->loopRecentA_.size() > 3) self->loopRecentA_.remove(0);
                }
                else
                {
                    self->loopLastB_ = nextPole;
                    self->pendingLoopPromptB_ = RepromptStances::reattachMusicSuffix(nextPole, self->loopSuffixB_);
                    self->loopRecentB_.add(nextPole);
                    while (self->loopRecentB_.size() > 3) self->loopRecentB_.remove(0);
                }
                self->loopAltWriteA_ = ! altWriteA;   // alternate the target for next step
                self->loopStepInFlight_ = false;
                return;
            }

            // Stage the new prompt into pending — the editor is only updated when the
            // next generation completes (i.e., when the prompt becomes wirksam).
            // ONE pole this step (alternating in dual; always B in alpha), so a dual
            // step runs a single interpret instead of two. concat2 only when there's a
            // real core to prepend — a pole that was nothing but a musical token has an
            // EMPTY core (orig==""), and concat2("",x) would emit a leading ", "; fall
            // back to the bare new link so the suffix re-append yields "x, 120bpm".
            if (dual && altWriteA)
            {
                // A pole this step (dual only). loopLast_/recent_ keep the CORE.
                self->loopLastA_ = nextPole;                            // glieder[-1] (CORE)
                const juce::String appliedA = RepromptStances::reattachMusicSuffix(
                    (concat && origA.isNotEmpty()) ? RepromptStances::concat2(origA, nextPole) : nextPole,
                    self->loopSuffixA_);
                self->pendingLoopPromptA_ = appliedA;
                self->loopRecentA_.add(nextPole);                      // push into recent…
                while (self->loopRecentA_.size() > 3) self->loopRecentA_.remove(0);  // …keep last 3
            }
            else
            {
                // B pole this step (alpha always lands here; dual on B's turn).
                self->loopLastB_ = nextPole;                            // glieder[-1] (CORE)
                const juce::String appliedB = RepromptStances::reattachMusicSuffix(
                    (concat && origB.isNotEmpty()) ? RepromptStances::concat2(origB, nextPole) : nextPole,
                    self->loopSuffixB_);
                self->pendingLoopPromptB_ = appliedB;
                self->loopRecentB_.add(nextPole);                      // push into recent…
                while (self->loopRecentB_.size() > 3) self->loopRecentB_.remove(0);  // …keep last 3
            }

            // Alternate the driven pole for the next step (dual only; alpha stays on B,
            // its A pole the untouched human anchor).
            if (dual)
                self->loopAltWriteA_ = ! altWriteA;

            self->loopStepInFlight_ = false;
        });
    }).detach();
}

void PromptPanel::mouseDown(const juce::MouseEvent& e)
{
    if (! e.mods.isRightButtonDown())
        return;

    juce::String paramId;
    auto* src = e.eventComponent;
    if      (src == &alphaSlider)     paramId = PID::genAlpha;

    if (paramId.isNotEmpty())
        showMidiLearnMenu(processorRef, paramId, e.getScreenPosition());
}

void PromptPanel::mouseDoubleClick(const juce::MouseEvent& e)
{
    // The co-firing single click (Button's own handling) already selects
    // steady mode via seedModeBtns[steady].onClick — this only layers the
    // seed-entry dialog on top for the Easy-mode "type an exact seed" path.
    if (e.eventComponent == &seedModeBtns[static_cast<int>(SeedMode::steady)])
        openSeedEntryDialog();
}
