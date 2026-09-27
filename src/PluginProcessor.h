#pragma once
#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <mutex>
#include <memory>
#include <limits>
#include <thread>
#include <unordered_map>
#include "dsp/VoiceManager.h"
#include "dsp/VoiceEvent.h"
#include "dsp/CsoundEngine.h"
#include "dsp/LroControls.h"
#include "dsp/ParamCache.h"
#include "dsp/LFO.h"
#include "dsp/DriftLFO.h"
#include "dsp/DelayLine.h"
#include "dsp/ConvolutionReverb.h"
#include "dsp/AlgorithmicReverb.h"
#include "dsp/Limiter.h"
#include "dsp/AmpEffects.h"
#include "sequencer/StepSequencer.h"
#include "sequencer/GenerativeSequencer.h"
#include "sequencer/Arpeggiator.h"
#include "inference/PipeInference.h"
#include "eventlog/EventLog.h"
#include "eventlog/EventLogReader.h"
#include "eventlog/EventLogWriterThread.h"

class T5ynthProcessor : public juce::AudioProcessor,
                        private juce::AsyncUpdater,
                        private juce::AudioProcessorValueTreeState::Listener
{
public:
    T5ynthProcessor();
    ~T5ynthProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    // The host calls this instead of processBlock while the plugin is
    // bypassed -- see its definition (directly after processBlock's) for why
    // it still needs to run something.
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // JucePlugin_Name comes from PLUGIN_NAME in CMakeLists.txt, so the name a host
    // shows can never drift from the one the plugin registers. It carries a
    // non-ASCII character ("akróasys") — fromUTF8, never the raw literal.
    const juce::String getName() const override { return juce::String::fromUTF8(JucePlugin_Name); }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }

    // The MPE zone is parsed from raw MIDI in processBlock (ch1/ch16 master,
    // member channels per-note bend/pressure/timbre/CC74, RPN 0 for the bend
    // range), so this is
    // a capability the plugin has always HAD and never declared. Two wrappers ask:
    // the CLAP one only offers the host CLAP_NOTE_DIALECT_MIDI_MPE when this is
    // true, and the AU one answers kAudioUnitProperty_SupportsMPE with it — so
    // without this an MPE controller in a CLAP host, and in Logic, reaches a synth
    // that would have understood it. The VST3 and Standalone paths never read it.
    bool supportsMPE() const override { return true; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState& getValueTreeState() { return parameters; }

    // Engine mode (read from APVTS "engine_mode": 0=Sampler, 1=Wavetable, 2=Granular)
    bool isWavetableMode() const;
    bool isFreezeMode() const;
    bool isSamplerMode() const;
    // True while masterOsc holds a DCO/LCO-baked table (loadDcoWavetable) rather
    // than neural-extracted frames — the engine-window display uses it to label
    // the wavetable "Wavetable" and suppress the source extraction brackets.
    bool isDcoTableActive() const { return dcoTableActive_.load(std::memory_order_relaxed); }

    /** GUI-thread entry point for the StatusBar "Panic" button. Sets a flag
     *  that the audio thread consumes at the start of the next processBlock,
     *  matching the CC120/123 (All Notes/Sound Off) behaviour. */
    void requestMidiPanic() { midiPanicRequested.store(true, std::memory_order_release); }

    /** Builds the Duration parameter's NormalisableRange (skew + whole-second
        snapping) for a given maximum in seconds. Shared by createParameterLayout
        (registers the parameter at the global 120s max) and PromptPanel, which
        re-scopes the slider per model: 11s for SAO/AudioLDM2, 120s for SA3. */
    static juce::NormalisableRange<float> makeDurationRange(float maxSeconds);

    // Load generated audio into the engine
    void loadGeneratedAudio(const juce::AudioBuffer<float>& buffer, double sampleRate);
    /** Reload already-processed audio into sampler (no Rumble/HF/Normalize). */
    void reloadProcessedAudio(const juce::AudioBuffer<float>& processed);
    /** Load DCO-baked single-cycle frames (mono strip, N*2048 samples laid
     *  end-to-end) into the wavetable master and switch the engine to
     *  Wavetable. Message thread only. Held voices crossfade over the Regen
     *  XFade time (distributeWavetableFrames), like any regeneration.
     *  motionRateHz = the recipe's authored motion tempo (full loops/sec,
     *  drives the engine's DCO motion transport); <= 0 falls back to the
     *  legacy strip-length rate (one loop per strip-duration).
     *  NOTE: reextractWavetable()/reloadProcessedAudio() re-extract from the
     *  last GENERATED audio, so touching WT frame-count/brackets after a DCO
     *  bake reverts the table to the neural material — known Slice-4 seam. */
    void loadDcoWavetable(const juce::AudioBuffer<float>& frameStrip,
                          float motionRateHz = 0.0f);

    /** Force the engine-mode parameter to Csound, stashing the engine the user
     *  was on beforehand into dcoPrevEngineMode_ — the EXACT SAME host-visible-
     *  parameter-force + previous-mode-stash mechanism loadDcoWavetable uses for
     *  Lco (see dcoPrevEngineMode_'s member comment and loadGeneratedAudio's
     *  restore site), extended rather than duplicated (SPEC_phase4_5_csound_llm_
     *  preset.md Phase 4b: the prompt-authored-orchestra path forces Csound, not
     *  Lco). Re-triggering while already in Csound mode keeps the ORIGINAL
     *  pre-Csound stash rather than overwriting it with "Csound" itself. Message
     *  thread only, called by PromptPanel::triggerDcoBake right after a
     *  successful authorCsoundOrchestra(). Does NOT touch dcoTableActive_ (the
     *  wavetable lock) — this path never bakes a table. */
    void forceCsoundEngineMode();

    /** Counterpart to forceCsoundEngineMode(): hand the engine back to the
     *  neural (T5osc) side. The oscillator-mode toggle is GUI state, so leaving
     *  the LCO left the authored orchestra rendering under a T5osc panel and
     *  only a following neural generation ever undid it — a session that
     *  STARTED in a language mode (preset load, DAW state) has no stash, so
     *  there it never came back at all (BJ 2026-07-22: "LCO klingt weiterhin").
     *  Restores the stashed pre-LCO engine, falling back to Sampler (the engine
     *  a fresh generation feeds) when there is no stash. Consumes the stash and
     *  remembers the language mode it left for restoreLanguageEngineMode().
     *  No-op (but still clears the stash) unless the engine currently IS on a
     *  language mode. Message thread only. */
    void restoreNeuralEngineMode();

    /** Re-enter the language mode restoreNeuralEngineMode() last left, stashing
     *  the neural engine on the way in exactly like forceCsoundEngineMode().
     *  Returns false — changing nothing — when the toggle has not left one, so
     *  the caller can decide what a first-ever switch into the LCO should do.
     *  Message thread only. */
    bool restoreLanguageEngineMode();

    /** True when an orchestra has been authored (bake) or restored (preset) —
     *  i.e. when a first-ever switch into the LCO has something to sound.
     *  Empty means the BUILT-IN orchestra, so this is deliberately not the test
     *  for "was in Csound mode": that is restoreLanguageEngineMode()'s job. */
    bool hasCsoundOrchestra() const;

    /** The orchestra text that is authoritative RIGHT NOW — what a fresh
     *  requestCsoundOrchestra() would have to be handed to make the instrument
     *  sound exactly as it sounds at this moment. Same value and same reason as
     *  exportJsonPreset's csound_orchestra field (what requestCsoundOrchestra
     *  last queued, NOT the active engine's orchestraText(), which lags a
     *  post-bake crossfade): a SNAP taken inside that fade window would
     *  otherwise store the orchestra the bake replaced. Reads a mirror kept
     *  under its own short-lived mutex rather than csoundLifecycleMutex_, which
     *  the compile thread holds across a full Csound prepare + warmup — a GUI
     *  caller (every SNAP press in LCO mode) must not stall behind that.
     *  Empty = the built-in orchestra, i.e. nothing was ever authored. */
    juce::String getCsoundOrchestraText() const;

    // Inference cache: raw inference audio only, no duplicate prompt/model metadata.
    struct InferenceCacheEntry
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
    };
    /** The five depths both cache rows offer: 0, 2, 4, 8, 16. Anything between
     *  two of them rounds UP to the next - a depth that holds everything - and
     *  only 0 means off. */
    static int sanitizeCacheCapacity(int capacity);
    void setInferenceCacheCapacity(int capacity);
    /** How many entries a traversal may travel through: the fill count when the
     *  cache is full, 0 otherwise. An ATOMIC, because the audio thread asks and
     *  inferenceCacheEntries is a plain vector the message thread reallocates -
     *  reading its size() across that is a race, and every other audio-side read
     *  of message-thread state in this instrument goes through an atomic
     *  snapshot for exactly this reason. */
    int getInferenceCacheTraversableZones() const
    { return inferenceCacheTraversableZones_.load(std::memory_order_acquire); }
    /** Bumped by every mutation of this cache. The zone COUNT cannot stand in
     *  for this: a preset that replaces a full 4-deep cache with another full
     *  4-deep one leaves the count where it was, and an aftertouch bar watching
     *  the count would go on believing the position under the finger is loaded
     *  when it holds something else entirely. */
    unsigned getInferenceCacheGeneration() const
    { return inferenceCacheGeneration_.load(std::memory_order_acquire); }
    void clearInferenceCache();
    bool addInferenceCacheEntry(const juce::AudioBuffer<float>& buffer, double sampleRate);
    bool playNextInferenceCacheEntry();
    /** Play cache entry `index` (clamped) and leave the sequential cursor one
     *  past it. This is the same call the Re-Prompt stepping makes - the
     *  oscillator's crossfade carries the change exactly as it carries a fresh
     *  inference - the only thing added is that the index is free, so movement
     *  through the cache runs in both time directions. */
    bool playInferenceCacheEntry(int index);
    bool isInferenceCacheActive() const { return inferenceCacheCapacity > 0; }
    bool isInferenceCacheFull() const { return inferenceCacheCapacity > 0
                                             && static_cast<int>(inferenceCacheEntries.size()) >= inferenceCacheCapacity; }
    int getInferenceCacheCapacity() const { return inferenceCacheCapacity; }
    /** Highest cache depth the current Duration allows; 0 when none fits.
     *
     *  The cache travels inside the preset, so depth x length IS a file size:
     *  16 slots of 120 s is half a gigabyte of 24-bit audio before FLAC starts.
     *  The budget is one number, 192 SLOT-SECONDS, which is exactly the ladder
     *  BJ set: 16 up to 12 s, 8 to 24 s, 4 to 48 s, 2 to 96 s, nothing beyond
     *  (16x12 = 8x24 = 4x48 = 2x96 = 192). All four thresholds sit on whole
     *  seconds, which is where snapGenerationDuration's detents are, so the
     *  player can land on each one deliberately. Landing there is not the same
     *  as the number ARRIVING here exact - see the tolerance in the .cpp.
     *
     *  Counted in slot-seconds and NOT in bytes on purpose: channel count and
     *  sample rate are unknown until audio exists, and the ceiling has to hold
     *  BEFORE the first render. Mono, or a lower rate, lands under the estimate,
     *  never over it. */
    int maxInferenceCacheCapacityForDuration() const;
    /** Set the capacity from a PLAYER SELECTION - clamped to the ceiling above.
     *
     *  Preset load must NOT come through here. What a file carries wins: its
     *  entries were recorded under whatever Duration was in force, and clamping
     *  on load would let addInferenceCacheEntry's own capacity check drop them
     *  silently - which is exactly what a 32- or 64-deep preset already suffers:
     *  it sanitises to 16 and loses the rest. One such truncation is enough. */
    /** True when the entries in the cache were recorded as an offline take, i.e.
     *  the parameters really were frozen between them. It is this, not the switch
     *  position, that lets a full cache replay over a running Re-Prompt stance: a
     *  cache filled with the drift sweeping freely is not a recording of anything,
     *  and with nothing to freeze the switch has to leave play exactly as it was. */
    bool isInferenceCacheOfflineTake() const { return inferenceCacheIsOfflineTake; }
    /** Set per captured entry: true while a take records them, false the moment one
     *  is recorded live. Half a take is not a recording of anything, so a single
     *  live entry disqualifies the whole cache — turning the switch off mid-fill,
     *  or finishing the cache by hand, must not leave it claiming to be one. */
    void setInferenceCacheOfflineTake(bool isTake) { inferenceCacheIsOfflineTake = isTake; }
    int getInferenceCacheFillCount() const { return static_cast<int>(inferenceCacheEntries.size()); }
    const std::vector<InferenceCacheEntry>& getInferenceCacheEntries() const { return inferenceCacheEntries; }

    // ── The LRO's own cache ──────────────────────────────────────────────────
    // Same cache, different substance. A neural slot holds raw audio; an LRO
    // slot holds what an authoring pass INSTALLS - the orchestra and the five
    // things that belong to it. Playing a slot is that installation run again,
    // and the compile crossfade the LRO already does after a bake carries it,
    // exactly as the sampler's crossfade carries a neural slot.
    //
    // No Duration ceiling here. That budget counts slot-seconds of audio; this
    // is text, and sixteen of them are kilobytes. All five depths stay open in
    // the LRO whatever the Duration says.
    struct CsoundCacheEntry
    {
        juce::String orchestra, prompt, reading, paramsText;
        // Who wrote this orchestra. Carried for the same reason a SNAP slot
        // carries it: the model tab names an author, and a name left standing
        // over an instrument somebody else wrote is a false claim. Empty means
        // "not known", which CLEARS the previous claim rather than keeping it.
        juce::String authorModel;
        juce::var controls;    // the twelve knobs the authored body offers
        juce::var settings;    // what the author asked of the synth itself
    };
    void setCsoundCacheCapacity(int capacity);
    int  getCsoundCacheCapacity() const { return csoundCacheCapacity; }
    void clearCsoundCache();
    /** Message thread. False when the cache is off or already full. */
    bool addCsoundCacheEntry(const CsoundCacheEntry& entry);
    bool isCsoundCacheFull() const
    { return csoundCacheCapacity > 0
          && static_cast<int>(csoundCacheEntries.size()) >= csoundCacheCapacity; }
    int  getCsoundCacheFillCount() const { return static_cast<int>(csoundCacheEntries.size()); }
    bool isCsoundCacheOfflineTake() const { return csoundCacheIsOfflineTake; }
    void setCsoundCacheOfflineTake(bool isTake) { csoundCacheIsOfflineTake = isTake; }
    const std::vector<CsoundCacheEntry>& getCsoundCacheEntries() const { return csoundCacheEntries; }
    /** Install entry `index` whole - engine mode, orchestra, prompt, reading,
     *  params, knobs, author settings. Everything an authoring pass installs on
     *  the PROCESSOR; the panel-side half of a recall (trace card, author name,
     *  the Re-Prompt chain's own memory, the compile watch) belongs to the
     *  editor and is done by PromptPanel::playNextCachedCsound, which is why
     *  the index that was played is readable below. */
    bool playCsoundCacheEntry(int index);
    bool playNextCsoundCacheEntry();
    /** The slot the next replay will install - and, right after a replay, one
     *  past the slot that was just installed (it advances on play). */
    int  getCsoundCachePlaybackIndex() const { return csoundCachePlaybackIndex; }
    int  getCsoundCacheTraversableZones() const
    { return csoundCacheTraversableZones_.load(std::memory_order_acquire); }
    /** See getInferenceCacheGeneration(). */
    unsigned getCsoundCacheGeneration() const
    { return csoundCacheGeneration_.load(std::memory_order_acquire); }

    // ── Whichever cache belongs to the oscillator in force ──────────────────
    // The CACHE row and the take switch speak to THIS, not to one of the two
    // above. One row, and the oscillator in force decides what it reaches -
    // rather than a second row beside the first.
    //
    // TWO different questions, deliberately not one predicate:
    //
    //  - which cache does the PANEL address? The panel the player is looking at.
    //    That is what the row is, and it is set from the oscillator toggle
    //    (setSurfaceParadigmIsLanguage). It must NOT be asked of the engine
    //    mode: applyOscModeToEngine leaves the engine alone until something has
    //    been authored, so the LRO panel can sit in front of a neural engine for
    //    a whole session - and a depth press there would then have cleared the
    //    NEURAL cache, from a panel that does not own it.
    //
    //  - which cache does the aftertouch bar TRAVEL? The one that is sounding,
    //    which is the engine mode (isLanguageOscillatorSounding) and only
    //    EngineMode::Csound: an LRO slot is a Csound orchestra and does not
    //    sound under any other engine, Lco included.
    bool isLanguageOscillatorSounding() const;
    void setSurfaceParadigmIsLanguage(bool isLanguage);
    bool isSurfaceParadigmLanguage() const
    { return surfaceParadigmIsLanguage_.load(std::memory_order_acquire); }
    int  getActiveCacheCapacity() const;
    void setActiveCacheCapacity(int capacity);
    void selectActiveCacheCapacity(int capacity);
    int  getActiveCacheFillCount() const;
    bool isActiveCacheFull() const;
    /** The deepest depth the active cache allows. The LRO has no ceiling. */
    int  maxActiveCacheCapacityForDuration() const;

    /** Offline cache take — message thread writes, audio thread reads.
     *
     *  While a take records, the generation-side Drift LFOs (alpha, noise,
     *  magnitude, the three axes, resynth) stand still: the parameters wait at the
     *  cache point until the render lands. They then move on by exactly one cadence
     *  interval, requested through requestDriftGenerationStep(). The recorded
     *  trajectory is therefore the same on any machine, however long the inference
     *  takes; without it a fast box captures the drift every bar and a slow one
     *  every few, and the Resynth/Re-Prompt chain skips steps with it.
     *  Everything audible the Drift can reach keeps running either way. */
    void setDriftGenerationHold(bool held) noexcept
    {
        driftGenHold_.store(held, std::memory_order_relaxed);
    }
    bool isDriftGenerationHeld() const noexcept
    {
        return driftGenHold_.load(std::memory_order_relaxed);
    }
    /** Accumulates: a step requested while the audio thread has not consumed the
     *  previous one must not be dropped, or the take would stall in place. */
    void requestDriftGenerationStep(float seconds) noexcept
    {
        if (seconds <= 0.0f) return;
        float expected = driftGenStepSec_.load(std::memory_order_relaxed);
        while (! driftGenStepSec_.compare_exchange_weak(expected, expected + seconds,
                                                        std::memory_order_relaxed))
        {}
    }

    // Inference (Python subprocess)
    bool isInferenceReady() const { return pipeInference->isReady(); }
    PipeInference& getPipeInference() { return *pipeInference; }
    std::shared_ptr<PipeInference> getPipeInferencePtr() { return pipeInference; }
    bool launchPipeInference(const juce::File& backendDir);
    bool isPipeInferenceReady() const { return pipeInference->isReady(); }

    // Last device/model used for generation (for preset tagging)
    void setLastDevice(const juce::String& dev) { lastDevice = dev; }
    const juce::String& getLastDevice() const { return lastDevice; }
    void setLastModel(const juce::String& m) { lastModel = m; }
    const juce::String& getLastModel() const { return lastModel; }

    // ── Modality epoch ──────────────────────────────────────────────────────────
    // Which SA3 TrackType-routing behaviour the current preset/session was authored
    // under. Travels in the preset (.t5p synth.modalityEpoch), the DAW session state
    // (XML attr), and every inference request (modality_epoch). ABSENCE of the field
    // is the legacy switch everywhere. Keep kModalityEpoch in sync with the backend
    // (backend/pipe_inference.py _native_modality_prefix): epoch>=1 == v2.5.0 routing
    // (Instrument default + music heuristic + hard selectors); epoch<=0 == pre-2.5.0
    // (Music/SFX only). Re-saving a legacy preset preserves its legacy mode.
    static constexpr int kLegacyModalityEpoch = 0;   // pre-2.5.0: Music/SFX only
    static constexpr int kModalityEpoch       = 1;   // v2.5.0: Instrument default + heuristic
    int  getModalityEpoch() const noexcept { return currentModalityEpoch_; }
    void setModalityEpoch(int e) noexcept  { currentModalityEpoch_ = e; }

    // Preset metadata (GUI-only state that must survive save/load)
    void setLastPrompts(const juce::String& a, const juce::String& b) { lastPromptA = a; lastPromptB = b; }
    const juce::String& getLastPromptA() const { return lastPromptA; }
    const juce::String& getLastPromptB() const { return lastPromptB; }

    // Durable HUMAN prompts — the single source of truth for "what the user
    // authored", independent of the live editors. The Re-Prompt loop rewrites the
    // editors in place (and so lastPromptA/B, which mirror them), and a missed
    // deactivation-restore or a buffer reload can leave a machine rewrite sitting
    // in an editor; deriving the saved prompt from the editors there is what kept
    // baking machine text into presets. This store is written ONLY by human
    // authorship — onTextChange (per pole) and preset load (both poles) — never by
    // the loop, so PresetFormat::saveToFile can always persist the human prompt
    // regardless of editor state. Per-pole setters: an A edit must not recapture a
    // rewritten B (and vice-versa).
    void setHumanPromptA(const juce::String& a) { humanPromptA = a; humanPromptsValid = true; }
    void setHumanPromptB(const juce::String& b) { humanPromptB = b; humanPromptsValid = true; }
    void setHumanPrompts(const juce::String& a, const juce::String& b)
        { humanPromptA = a; humanPromptB = b; humanPromptsValid = true; }
    void clearHumanPrompts() { humanPromptA.clear(); humanPromptB.clear(); humanPromptsValid = false; }
    bool hasHumanPrompts() const { return humanPromptsValid; }
    const juce::String& getHumanPromptA() const { return humanPromptA; }
    const juce::String& getHumanPromptB() const { return humanPromptB; }
    void setLastPresetName(const juce::String& name) { lastPresetName = name; }
    const juce::String& getLastPresetName() const { return lastPresetName; }
    void setLastTags(const juce::StringArray& tags) { lastTags = tags; }
    const juce::StringArray& getLastTags() const { return lastTags; }
    void setLastGenerationTimeMs(float ms) { lastGenerationTimeMs = ms; }
    float getLastGenerationTimeMs() const { return lastGenerationTimeMs; }

    void setLastSeed(int s) { lastSeed = s; }
    int getLastSeed() const { return lastSeed; }

    // Whether the user's seed UI is in "auto" / random mode. PromptPanel keeps
    // this in sync with its `randomSeedToggle` (Adv) and the "auto" button of
    // the Easy three-state switchbox. Preset save persists this bool so the
    // Easy-mode "auto" selection survives a round-trip — the APVTS PID::genSeed
    // param is not driven by the UI, so we can't infer the auto state from it.
    void setLastRandomSeed(bool r) { lastRandomSeed = r; }
    bool getLastRandomSeed() const { return lastRandomSeed; }

    // Research-mode injection state (GUI-only, persisted in .t5p with defaults
    // for old files: linear / 0.75 / 4.0 / 16.0 — matches the panel defaults).
    void setLastInjection(const juce::String& mode, float lateMixAmount,
                          float splitStart, float splitEnd)
    {
        lastInjectionMode = mode;
        lastLateMixAmount = lateMixAmount;
        lastSplitStart = splitStart;
        lastSplitEnd = splitEnd;
    }
    const juce::String& getLastInjectionMode() const { return lastInjectionMode; }
    float getLastLateMixAmount() const { return lastLateMixAmount; }
    float getLastSplitStart() const { return lastSplitStart; }
    float getLastSplitEnd() const { return lastSplitEnd; }

    // LCO/DCO bake snapshot — the exact inputs of the LAST successful bake
    // (prompt, both readings, motion tempo, A/B balance), cached at bake time
    // (PromptPanel::triggerDcoBake's completion lambda) so exportJsonPreset /
    // PresetFormat::saveToFile can persist a complete, re-baking-free LCO
    // preset. Mirrors the humanPromptA/B pattern above. Message-thread only.
    // Cleared on a fresh neural generation (loadGeneratedAudio) so a preset
    // saved after leaving LCO mode does not carry stale LCO metadata.
    void setLcoBakeSnapshot(const juce::String& prompt,
                            const juce::String& readingA, const juce::String& readingB,
                            float motionRateHz,
                            bool oscAHasContent, float gainA,
                            bool oscBHasContent, float gainB)
    {
        lcoPrompt_ = prompt; lcoReadingA_ = readingA; lcoReadingB_ = readingB;
        lcoMotionRateHz_ = motionRateHz;
        lcoOscAHasContent_ = oscAHasContent; lcoGainA_ = gainA;
        lcoOscBHasContent_ = oscBHasContent; lcoGainB_ = gainB;
        lcoSnapshotValid_ = true;
        // A WT Frames re-slice still waiting would clear this again: re-slicing
        // hands masterOsc back to the neural table and drops the snapshot with it.
        // A preset load or state restore writes the frame count before it installs
        // the snapshot, so the re-slice its own frame change asked for would erase
        // the snapshot it just restored, and the next save would lose the lco
        // block. The synchronous re-slice that ran there before read the old
        // count, so dropping the request leaves the table where that one did.
        wtReextractWanted_.exchange(false, std::memory_order_acq_rel);
    }
    void clearLcoBakeSnapshot() { lcoSnapshotValid_ = false; }
    bool hasLcoBakeSnapshot() const { return lcoSnapshotValid_; }
    const juce::String& getLcoPrompt() const { return lcoPrompt_; }
    const juce::String& getLcoReadingA() const { return lcoReadingA_; }
    const juce::String& getLcoReadingB() const { return lcoReadingB_; }
    float getLcoMotionRateHz() const { return lcoMotionRateHz_; }
    bool  getLcoOscAHasContent() const { return lcoOscAHasContent_; }
    float getLcoGainA() const { return lcoGainA_; }
    bool  getLcoOscBHasContent() const { return lcoOscBHasContent_; }
    float getLcoGainB() const { return lcoGainB_; }

    // Csound-authored orchestra reading (Phase 5, SPEC_phase4_5_csound_llm_preset.md):
    // the UI-only "how it was heard" text PromptPanel::triggerDcoBake's Csound
    // completion lambda stashes here so exportJsonPreset (a plain const method
    // with no GUI access) can persist "csound_reading" in the engine block —
    // mirrors the LCO bake snapshot above, but the orchestra TEXT itself needs
    // no separate stash: exportJsonPreset reads csoundPendingOrchestraText_,
    // which requestCsoundOrchestra() writes synchronously in lockstep with this
    // setter (NOT the active engine's orchestraText(), which lags until a fade
    // settles — see exportJsonPreset's own comment). Message-thread only.
    void setCsoundReading(const juce::String& reading) { csoundReading_ = reading; }
    const juce::String& getCsoundReading() const { return csoundReading_; }

    // The prompt that authored the orchestra. It was NOT being saved: a Csound
    // preset stored the reading and the orchestra but nothing of what was
    // asked for, so a reload could show the machine's interpretation with no
    // way to see what it was interpreting — and the prompt box came back with
    // whatever the user had last typed, which never authored anything. The
    // "lco" block does carry a prompt, but only for the retired wavetable bake
    // (exportJsonPreset gates it on hasLcoBakeSnapshot), so a Csound-only
    // preset never had one. Same stash/persistence shape as the reading below,
    // set by the same completion lambda. Message-thread only.
    void setCsoundPrompt(const juce::String& prompt) { csoundPrompt_ = prompt; }
    const juce::String& getCsoundPrompt() const { return csoundPrompt_; }

    // The parametrisation behind that reading (transparency surface for the
    // LCO panel: the reading alone is a terse "saw > fm_bell" gloss; this is
    // the catalogue entry — "<key>: <why>" plus, for a parametrised key, its
    // "params: name=anchor|anchor|..." line — that each technique actually
    // resolved to, straight from backend/lco_write.py's params_text field).
    // Same stash/persistence shape as csoundReading_ above, set by the same
    // completion lambda. Message-thread only.
    /** The shelf of synth parameters the LRO author is shown when it is allowed
     *  to set them: id, name, current value, and either the range or the choice
     *  labels. Sound-shaping knobs only — the filter, the five envelopes, the
     *  LFOs, the drift LFOs, the aftertouch amounts, noise and the two effects.
     *  Not the generation parameters, not the engine identity, and not
     *  amp_target, which is the player's DCA routing. Message thread. */
    juce::var buildAuthorParamIndex() const;

    /** Apply what the author asked to set: an array of {id, value}, value being
     *  a number for a continuous parameter and a choice LABEL for a choice one.
     *  Parameters the PREVIOUS authoring set, and that the user has not moved
     *  since, are put back first — otherwise every regeneration would stack its
     *  settings on top of the last one's. Message thread.
     *
     *  Returns one "Name  value" line per knob that ACTUALLY moved, read back
     *  off the parameter afterwards. This side refuses lines the backend passed
     *  (an id that was never on the shelf, a choice label this build does not
     *  have, a value that is not a number), so the reply is not a record of what
     *  landed and anything reporting to the player has to use this instead. */
    juce::StringArray applyAuthorSettings (const juce::var& settings);

    /** Take on what an authoring asked the synth to set. The request is KEPT
     *  whichever way the KNOBS switch stands, and only the TAKING is gated: it
     *  goes on the patch while the player allows it and this instrument is the
     *  sounding oscillator, and otherwise waits for reconcileAuthorSettings to
     *  take it when the switch arrives (BJ, 2026-08-01: with the switch off the
     *  setting is still made, it only becomes active when the player turns KNOBS
     *  on). What this replaces: the shelf was withheld from the author whenever
     *  the switch was off, so the sound was written without its filter and its
     *  envelopes at all and turning the switch on afterwards had nothing to take
     *  — and the previous sound's request stayed standing in its place.
     *  Message thread. */
    void setAuthorSettings (const juce::var& settings);

    /** Where the last authoring's settings stand. Not a bool, because the two
     *  ways of NOT standing want different sentences: the switch is one word
     *  from putting them on the patch, the oscillator is not. Sounding is asked
     *  FIRST — while another oscillator plays, "turn KNOBS on" would be a false
     *  promise whichever way the switch happens to sit. */
    enum class AuthorKnobStand { onThePatch, notSounding, switchOff };
    AuthorKnobStand authorKnobStand() const;

    /** The last authoring's settings, as the KNOBS station shows them: one
     *  "Name  value" line each. `stand` says which list this is — on the patch,
     *  what the apply read back off the parameters; otherwise the request, i.e.
     *  what will be put there once it may be. Message thread. */
    juce::StringArray describeAuthorSettings (AuthorKnobStand& stand) const;

    /** Bumped whenever the author's settings are taken, given back or replaced.
     *  A panel re-reads the station on a change of this rather than polling the
     *  switch itself: the switch moves on any thread and is acted on one async
     *  hop later, so its value is true before the knobs are actually taken. */
    int getAuthorSettingsRevision() const { return authorSettingsRevision_; }

    /** Bumped only when the REQUEST itself is replaced or dropped — a new
     *  authoring, a preset, a restored session. A take and a give-back leave it
     *  alone. It is what lets a panel tell "my sound's settings, wherever they
     *  currently stand" from "a different patch's", which the revision above
     *  cannot: both look like a change there. */
    int getAuthorSettingsGeneration() const { return authorSettingsGeneration_; }

    /** Give back every knob the last authoring borrowed and still holds — what
     *  pressing the switch OFF means, and what an authoring the player no longer
     *  allows leaves behind otherwise. Message thread. */
    void releaseAuthorSettings();

    /** Make the patch agree with the switch: the last authoring's knobs are on
     *  it while the switch is on AND its orchestra is the sounding oscillator,
     *  and off it otherwise. Called from the async update whenever either of
     *  those two changes, so the switch works as an A/B — off gives the knobs
     *  back, on takes them again — instead of being a one-way release that only
     *  the next authoring could undo. Reads the live state rather than trusting
     *  a value carried in a flag, so a fast double-flip lands where the switch
     *  actually stands. Message thread. */
    void reconcileAuthorSettings();

    /** Forget the last authoring's settings outright: a new patch has arrived
     *  and what the previous one borrowed is nobody's to give back or re-take.
     *  Message thread. */
    void forgetAuthorSettings();

    /** Was this parameter one the author was offered? "It exists" is a different
     *  question: amp_target exists and is deliberately not on the shelf. */
    static bool onAuthorParamShelf (const juce::String& id);

    void setCsoundParamsText(const juce::String& text) { csoundParamsText_ = text; }
    const juce::String& getCsoundParamsText() const { return csoundParamsText_; }

    /** The knobs the AUTHORED instrument gives the player — what the LRO panel
     *  draws, and what the twelve `lro_p*` parameters mean for THIS sound.
     *  Message-thread only, exactly like the reading and the params text beside
     *  it; the audio thread never reads this, it reads the parameters.
     *
     *  `applyValues` writes each knob's authored starting value into its APVTS
     *  parameter, which is what makes the sound the player first hears the sound
     *  the author described. Not gated on lcoSetsParams: that switch is about
     *  the PLAYER'S patch — filter, envelopes, LFOs — while these twelve belong
     *  to the instrument the author just wrote and exist for nothing else. */
    void setCsoundControls(const LroControls& c, bool applyValues);
    const LroControls& getCsoundControls() const { return csoundControls_; }
    /** Bumped on every setCsoundControls: the panel repaints its grid when this
     *  changes rather than rebuilding it on a timer. */
    int getCsoundControlsRevision() const { return csoundControlsRevision_; }

    // Semantic axes state (GUI-only, 3 slots: dropdownId + value)
    struct AxisSlotState { int dropdownId = 1; float value = 0.0f; };
    void setLastAxes(const std::array<AxisSlotState, 3>& a) { lastAxes = a; }
    const std::array<AxisSlotState, 3>& getLastAxes() const { return lastAxes; }

    void setLastEmbeddings(const std::vector<float>& a, const std::vector<float>& b) { lastEmbeddingA = a; lastEmbeddingB = b; }
    const std::vector<float>& getLastEmbeddingA() const { return lastEmbeddingA; }
    const std::vector<float>& getLastEmbeddingB() const { return lastEmbeddingB; }

    /** Get the processed audio buffer (with HF boost if enabled). */
    const juce::AudioBuffer<float>& getGeneratedAudio() const { return generatedAudioFull; }
    /** Get the raw VAE output (unmodified, for re-apply on HF toggle). */
    const juce::AudioBuffer<float>& getGeneratedAudioRaw() const { return generatedAudioRaw; }
    double getGeneratedSampleRate() const { return generatedSampleRate; }

    /** Copy the last `seconds` of captured live input (the window ending "now")
        into `dest` and set `sampleRateOut`. Returns false (leaving dest untouched)
        when there is no usable capture — no input bus, or the window is
        effectively silent (no device / denied mic permission / nothing playing).
        Message-thread only. */
    bool snapshotExternalCapture(juce::AudioBuffer<float>& dest,
                                 double& sampleRateOut, double seconds) const;

    /** Returns true when an external audio input is available and the capture
        ring has been primed with at least some data (input bus open + SR set).
        Message-thread only — captureRingMutex is mutable. Used to gate the
        "ext" resynth source button in MainPanel. */
    bool hasExternalInputAvailable() const
    {
        const std::lock_guard<std::mutex> lk(captureRingMutex);
        return getTotalNumInputChannels() > 0
            && captureRing.getNumSamples() > 0
            && captureSampleRate > 0.0;
    }

    // Voice pool. Read-only, and offered for one reason: the MPE parity guard
    // (tools/test_mpe_parity.cpp) has to see what a MIDI message did to a
    // voice, and the only honest place to look is the voice itself. Nothing in
    // the plugin calls this -- a caller that wants to CHANGE a voice belongs on
    // the audio thread, where the members are reachable directly.
    const VoiceManager& getVoiceManager() const { return voiceManager; }

    // Sequencer
    T5ynthStepSequencer& getStepSequencer() { return stepSequencer; }
    T5ynthGenerativeSequencer& getGenerativeSequencer() { return generativeSequencer; }
    T5ynthArpeggiator& getArpeggiator() { return arpeggiator; }
    bool assignSequencerOneShotFromCurrentRegion(int step, int slot);
    bool assignSequencerOneShotFromRegion(int step, int slot, float regionStart, float regionEnd);
    bool hasSequencerOneShotSample(int step, int slot) const;
    void clearSequencerOneShotSample(int step, int slot);
    void clearSequencerOneShotSamples();
    /** Copy a captured one-shot (audio + mode) from one slot to another.
     *  Shares the immutable sample buffer by pointer — no deep audio copy.
     *  No-op if the source is empty or src == dst. Message-thread only. */
    bool copySequencerOneShotSample(int srcStep, int srcSlot, int dstStep, int dstSlot);
    struct SequencerOneShotExport
    {
        int step = 0;
        int slot = 0;
        T5ynthStepSequencer::OneShotMode mode = T5ynthStepSequencer::OneShotMode::Normal;
        juce::String label;
        double sampleRate = 44100.0;
        juce::AudioBuffer<float> audio;
    };
    std::vector<SequencerOneShotExport> exportSequencerOneShotSamples() const;
    void importSequencerOneShotSamples(const std::vector<SequencerOneShotExport>& slots);
    bool canUseStepHoldPreview() const;
    void beginStepHoldPreview(int midiNote, float velocity = 0.8f);
    void updateStepHoldPreview(int midiNote, float velocity = 0.8f);
    void endStepHoldPreview();
    // isKeystroke=false means "the same note continuing at a new pitch" — the octave
    // keys moving a key that is still held. It sounds identically and is NOT offered
    // to step-record, which must only ever capture what the player actually struck.
    void beginComputerKeyboardNote(int midiNote, float velocity = 0.8f, bool isKeystroke = true);
    void endComputerKeyboardNote(int midiNote);
    void allComputerKeyboardNotesOff();

    // ── Step-record (double-click Step latch) ──────────────────────────────
    // GUI toggles this; while armed, every played note (computer keyboard or
    // external MIDI) is written into the current step, advancing a cursor that
    // resets to 0 on each (re-)arm. Monophonic: one note-on per step.
    void toggleStepRecord();
    bool isStepRecordArmed() const { return stepRecordArmed.load(std::memory_order_relaxed); }
    int  getStepRecordCursor() const { return stepRecordCursor.load(std::memory_order_relaxed); }
    void drainStepRecordQueue();   // message thread: pop queued MIDI notes → steps
    void recordStepRest();         // message thread: empty step (rest) + advance

    // Waveform display data
    bool hasNewWaveform() const { return newWaveformReady.load(std::memory_order_acquire); }
    void clearNewWaveformFlag() { newWaveformReady.store(false, std::memory_order_release); }
    const juce::AudioBuffer<float>& getWaveformSnapshot() const { return waveformSnapshot; }

    // Wavetable display data: the frame strip the engine-window WaveformDisplay
    // draws as a frame-decimated, cycle-readable 2.5D fan. Populated for BOTH a
    // DCO/LCO bake (loadDcoWavetable) and neural/engine frames
    // (publishWtDisplayFromOscFrames), so the fan shows for every wavetable.
    // Separate flag from newWaveformReady so the fan can co-exist with the sample
    // path (a bake sets no sample snapshot at all).
    bool hasNewWtDisplay() const { return newWtDisplayReady.load(std::memory_order_acquire); }
    void clearNewWtDisplayFlag() { newWtDisplayReady.store(false, std::memory_order_release); }
    const juce::AudioBuffer<float>& getWtDisplaySnapshot() const { return wtDisplaySnapshot; }

    /** Re-arm the WT-display publish so a freshly (re)opened editor re-adopts the
     *  current wavetable's 2.5D fan. The ready flag is one-shot (consumed by the
     *  previous editor) while the snapshot persists across editor lifetimes; the
     *  SynthPanel calls this on construction. No-op unless in Wavetable mode with
     *  a published table (DCO, LCO or neural). */
    void republishWtDisplayIfActive()
    {
        if (isWavetableMode() && wtDisplaySnapshot.getNumSamples() > 0)
            newWtDisplayReady.store(true, std::memory_order_release);
    }

    // JSON preset import/export (compatible with Vue reference format)
    juce::String exportJsonPreset() const;
    bool importJsonPreset(const juce::String& json);

    // Sampler/oscillator access for preset import and UI queries
    SamplePlayer& getSampler() { return masterSampler; }
    WavetableOscillator& getMasterOsc() { return masterOsc; }
    const WavetableOscillator& getMasterOscConst() const { return masterOsc; }

    /** Re-extract wavetable frames using current bracket region. deferredRequest:
     *  called for a waiting WT Frames change (handleAsyncUpdate), which runs only in
     *  a wavetable mode and only if the table is not already sliced at that count. */
    void reextractWavetable(bool deferredRequest = false);

    // ── BPM-sync resolution ──
    // `hostBpmLastSeen` freezes the last live host BPM so paused-DAW behavior
    // keeps the previously-known tempo (rather than collapsing to default).
    // `hostPlayingNow` distinguishes a live transport from a frozen value.
    // Resolution priority: live host > running in-app sequencer >
    // frozen host > seqBpm fallback.
    bool seqRunningNow() const;
    float resolveSyncBpm() const;

private:
    struct WtTraversalMapping
    {
        float extractStart = 0.0f;
        float extractEnd = 1.0f;
        float startInExtract = 0.0f;
        float loopStartInExtract = 0.0f;
        float loopEndInExtract = 1.0f;
        int regionSamples = 0;
    };

    juce::AudioProcessorValueTreeState parameters;

    // Global (machine-wide) settings store: ~/Library/Application Support/T5ynth/
    // T5ynth.settings. Holds the nonlinear-filter oversampling quality (read once at
    // construction into filterOsFactor_, the audio thread only ever touches that
    // atomic; written by the Settings UI via setFilterOsQuality()) and the
    // update-check opt-out / throttle below.
    juce::ApplicationProperties appProperties_;

public:
    // ── Update check (machine-wide opt-out, GitHub "latest release" poll) ──
    // Started once at construction on its own background thread (UpdateChecker) —
    // never blocks Python backend / model loading. A result (if any) lands in
    // updateState_, guarded by its own lock; MainPanel's timer polls
    // takeAvailableUpdate() once to surface a StatusBar notification. Throttled to
    // at most once per 24h via "lastUpdateCheckEpochSec" in appProperties_.
    void setCheckForUpdatesEnabled(bool enabled);
    bool getCheckForUpdatesEnabled() const;
    /** True and fills version/url iff a newer release was found since the last call
     *  (consumes the result — call once per notification). */
    bool takeAvailableUpdate(juce::String& versionOut, juce::String& urlOut);

private:
    void startUpdateCheckIfDue();
    // UpdateChecker's background thread posts its result via
    // juce::MessageManager::callAsync — that message can still be sitting in the
    // queue after ~T5ynthProcessor runs (stopThread() joins the thread but cannot
    // dequeue an already-posted callback). So the shared state it writes into is
    // heap-allocated and kept alive by a shared_ptr the checker's callback holds a
    // copy of — never by a raw `this` capture — so a late-firing callback touches
    // a still-live object instead of freed processor memory.
    struct UpdateState
    {
        juce::CriticalSection lock;
        juce::String version, url;
        bool consumed = true;
    };
    std::shared_ptr<UpdateState> updateState_ = std::make_shared<UpdateState>();
    std::unique_ptr<class UpdateChecker> updateChecker_;
    ParamCache paramCache;
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    WtTraversalMapping makeWtTraversalMapping(int totalSamples) const;
    WtTraversalMapping makeWtTraversalMapping(int totalSamples, float p1, float p2, float p3) const;
    void syncWavetableTraversal(double bufferSampleRate, int totalSamples);
    // Publish the oscillator's current level-0 frames as the WT-display strip so
    // the engine-window 2.5D fan reflects neural/engine wavetables too (the DCO
    // paths publish their baked strip directly). No-op when the bank has no
    // frames (e.g. an inharmonic additive bank). Message thread.
    void publishWtDisplayFromOscFrames();
    void updateDriftState(int numSamples, float syncBpm);
    void syncSamplerSettingsFromParametersLocked();
    bool serviceSamplerReprepare();
    void samplerReprepareThreadMain();
    void storeSamplerReprepareSource(const juce::AudioBuffer<float>& buffer,
                                     double sampleRate,
                                     float normalizeStartFrac = 0.0f,
                                     float normalizeEndFrac = 1.0f);

    // Engine (mode is stored in APVTS "engine_mode", no separate member)

    // DSP — polyphonic voice pool
    VoiceManager voiceManager;
    float tuningTable[128] {};  // MIDI note → Hz, rebuilt per-block

    // Master data holders (own the audio/frame data, voices share from these)
    WavetableOscillator masterOsc;
    // Csound engine(s) (spec docs/... SPEC_phase1_csound_engine.md,
    // SPEC_phase2_csound_swap.md). Phase 2 (S1): TWO processor-owned instances
    // — a new orchestra always compiles into the INACTIVE one and crossfades
    // in over the Regen XFade time (driftCrossfade), the platform's held-note
    // invariant (CLAUDE.md) applied to Csound. csoundActiveIdx_ says which is
    // live; voices/the pump only ever see engines through this index (or,
    // mid-fade, through csoundMixBufs_ below) — never a bare instance. Value
    // members — like masterOsc, they persist across prepareToPlay/
    // releaseResources cycles (prepare() is idempotent/cheap when already
    // prepared at the same sample rate + orchestra text, D9); the header is a
    // header-only inert stub when T5YNTH_HAS_CSOUND==0, so these members and
    // every call site below compile identically on every build/machine. A
    // session that never calls requestCsoundOrchestra() never moves
    // csoundActiveIdx_ off 0 — engine 0 alone then behaves exactly like
    // Phase 1's single `csoundEngine` member (byte-identical).
    static constexpr int kNumCsoundEngines = 2;
    CsoundEngine csoundEngines_[kNumCsoundEngines];
    std::atomic<int> csoundActiveIdx_ { 0 };
    // True while masterOsc holds a DCO-baked table (loadDcoWavetable) instead
    // of frames extracted from generated audio. Gates the per-block
    // syncWavetableTraversal in processBlock, which would otherwise re-derive
    // scan-loop brackets from the LAST NEURAL sample every block and clobber
    // the DCO's full-range motion loop. Message thread writes (all engine
    // loaders), audio thread reads relaxed. Any neural (re-)extraction into
    // masterOsc clears it.
    std::atomic<bool> dcoTableActive_ { false };
    // A WT Frames change is waiting for handleAsyncUpdate to re-slice, which it
    // does only in a wavetable mode (parameterChanged sets it, on any thread: a
    // MIDI-learned CC sets it on the audio thread). loadGeneratedAudio drops it
    // in a wavetable mode right before it reads the frame count (outside one it
    // installs contiguous chunks, and the request waits for the switch back),
    // reextractWavetable once it runs in one (its early returns, a baked table
    // or no snapshot, have nothing to re-slice), setLcoBakeSnapshot because a
    // re-slice would clear the snapshot.
    // An exchange and not a store: the read-modify-write sees the latest set,
    // so either its acquire makes the new count visible to the read that
    // follows, or the set lands after it and the request stands.
    std::atomic<bool> wtReextractWanted_ { false };
    // The WT Frames index the neural table in masterOsc was last sliced at, or
    // -1 (contiguous chunks, a baked table, nothing yet, or a load whose audio
    // yielded no table but replaced the snapshot a re-slice reads). A waiting
    // request whose count equals it has nothing to re-slice: a load settled the
    // table at that count, possibly while the request was still on its way (a
    // MIDI-learned CC stores the value on the audio thread before its listener
    // sets the flag, so a load can read the new count and still find no
    // request). Message thread only, like every load that writes it.
    int wtSlicedAtFramesIdx_ = -1;
    // Engine mode the user was on before a bake forced a language mode, or -1.
    // Leaving the language oscillator restores it (restoreNeuralEngineMode, the
    // sole consumer) so a bake never permanently hijacks the neural signal path.
    // It used to be consumed by the next fresh neural generation instead, which
    // both missed the case where no generation follows and let an unrelated
    // audio reload (HF-boost reprocess, snapshot recall, auto-regen) pull the
    // engine out from under an LCO panel. Message thread only (loadDcoWavetable
    // / forceCsoundEngineMode write, restoreNeuralEngineMode / setStateInformation
    // clear).
    int dcoPrevEngineMode_ = -1;
    // The language mode (Csound, or legacy Lco for a restored wavetable bake)
    // the oscillator-mode toggle last left, or -1. Its counterpart to the stash
    // above: switching back to the LCO panel returns to exactly the paradigm the
    // user left, rather than guessing one from whether an orchestra text exists
    // (which is empty for the built-in orchestra and never set at all by a
    // legacy Lco bake). Message thread only.
    int lcoEngineMode_ = -1;
    // Bake-time inputs cached for LCO preset save (see setLcoBakeSnapshot).
    juce::String lcoPrompt_, lcoReadingA_, lcoReadingB_;
    float lcoMotionRateHz_ = 0.0f;
    bool  lcoOscAHasContent_ = false, lcoOscBHasContent_ = false;
    float lcoGainA_ = 1.0f, lcoGainB_ = 1.0f;
    bool  lcoSnapshotValid_ = false;
    // The prompt that authored the orchestra, cached for preset save (see setCsoundPrompt).
    juce::String csoundPrompt_;
    // Csound-authored reading cached for preset save (see setCsoundReading).
    juce::String csoundReading_;
    // What the last authoring set, and what stood there before it — so the next
    // authoring can hand back the knobs it borrowed. `applied` is what the
    // author put there; a value that no longer matches it means the PLAYER has
    // moved that knob since, and then it is theirs and is left alone.
    // Set from parameterChanged (any thread) when the KNOBS switch or the engine
    // mode moves, consumed on the message thread in handleAsyncUpdate.
    std::atomic<bool> authorReconcileWanted_ { false };
    struct AuthorSetParam { juce::String id; float before = 0.0f; float appliedNorm = 0.0f; };
    std::vector<AuthorSetParam> authorSetParams_;   // message thread only
    // What the last authoring ASKED for, kept past a release so the switch can
    // take it again. `authorSetParams_` above cannot serve: it is the record of
    // what is currently borrowed and is emptied by the give-back, which is
    // precisely the moment this has to survive. Message thread only.
    juce::var authorSettings_;
    // What that request actually put on the patch, read back off the parameters
    // — the KNOBS station's live half. Kept here rather than handed to the panel
    // once, because the switch can take and give back long after the authoring
    // and the station has to be able to ask again. Message thread only.
    juce::StringArray authorAppliedLines_;
    int authorSettingsRevision_ = 0;
    int authorSettingsGeneration_ = 0;
    // May the author's settings stand on the patch right now: the player allows
    // it AND the authored orchestra is the sounding oscillator. Both read live,
    // in ONE place (authorKnobStand), so the take and the report cannot disagree
    // about it.
    bool authorMayHoldSettings() const;
    // Drop the request outright, without giving anything back: for a patch
    // replacement, whose own values are about to be written over these knobs.
    void dropAuthorSettings();
    // One entry of a SET list resolved against the shelf: the parameter it names
    // and the value it asks for, or false where THIS side refuses the line.
    bool resolveAuthorSetting (const juce::var& entry,
                               juce::RangedAudioParameter*& prm, float& target) const;

    // Csound-authored parametrisation text cached for preset save (see setCsoundParamsText).
    juce::String csoundParamsText_;
    // The authored instrument's own knobs — see setCsoundControls. Message
    // thread only; saved with the preset, because a reload that brought the
    // orchestra back but not the knobs would leave twelve live parameters with
    // no names on them.
    LroControls csoundControls_;
    int csoundControlsRevision_ = 0;
    SamplePlayer masterSampler;
    FreezeTextureEngine masterFreeze;

    // ── Per-note cache positions ──────────────────────────────────────────────
    // MPE means PER NOTE, and one cache position is a whole sample. The three
    // masters above can hold exactly one, so a bar riding them moves every note
    // at once - which is what a channel-wide aftertouch does, not what a
    // per-note controller does. Two fingers at two lateral positions have to be
    // able to play two different entries.
    //
    // So every cache entry gets a master of its own, prepared through the same
    // pipeline loadGeneratedAudio uses for the live one (prepareCachePosition
    // below shares its conditioning and its prepare calls, because a position
    // conditioned differently is a different sound). A voice is then pointed at
    // one of these instead of at the instrument-wide master
    // (VoiceManager::setVoiceEngineMasters) and crossfades onto it over the same
    // Regen XFade every other buffer change uses.
    //
    // Built on samplerReprepareThread, ONE position per pass: the work is the
    // expensive half of loadGeneratedAudio (region analysis, normalisation, the
    // mip FFT, the freeze mixdown) and doing sixteen of them in one go would
    // stall whichever thread ran it. Until a position is ready the voice keeps
    // the instrument-wide master, so the bar simply has less travel for a moment
    // rather than falling silent.
    struct CachePositionEngines
    {
        SamplePlayer        sampler;
        FreezeTextureEngine freeze;
        WavetableOscillator osc;
        double preparedRate = 0.0;   // build thread only: what prepare() was last given
        int    preparedBlock = 0;
        // The conditioned source this position was built from, for the transport
        // half of syncWavetableTraversal: extract brackets and the AutoScan rate
        // are derived from the sample's OWN length, and two takes are not the
        // same length.
        int    sourceSamples = 0;
        double sourceRate = 44100.0;
    };
    static constexpr int kMaxCachePositions = 16;   // sanitizeCacheCapacity's ceiling
    /** The stored snapshots had four positions here too, so a held note could
     *  travel their audio the way it travels the cache. Gone with Snap leaving
     *  the expression matrix - see AftertouchTarget::Snap. Nothing travels a
     *  snapshot, so nothing prepares one. */
    std::array<std::unique_ptr<CachePositionEngines>, kMaxCachePositions> cachePosEngines_;
    // Bit k: position k holds audio prepared from cache entry k. Under the
    // current SOURCE stamp always - that half acts at once. Not necessarily
    // under the current settings: those wait for the hand to come off the
    // control, and a position built while a run is open carries whatever was
    // live at that instant, under no claim that it is current. The run
    // invalidates the whole range when it ends. Written on the build thread,
    // read on the audio thread.
    std::atomic<juce::uint32> cachePosReadyMask_ { 0 };
    /** What the positions were last built from, split from the settings because
     *  the two halves must be treated in OPPOSITE ways. The SOURCE (which takes,
     *  the host rate) acts at once: audio that has been replaced or deleted must
     *  stop being reachable in the same instant, or a finger crossing a zone
     *  lands a take that no longer exists. The SETTINGS (HF Boost, WT Frames,
     *  Loop Mode, Normalize, Loop Optimize, Crossfade, AutoScan, the three
     *  locked points) WAIT for the hand to come off the control - acting on
     *  every detent of a drag discarded the range and rebuilt it one position
     *  per background pass, so for the whole gesture nothing was ever finished.
     *  Build thread only. */
    juce::uint64 cachePosSrcBuiltCache_ = 0;
    /** A stamp change WAITS for the hand to come off the control. Seven of the
     *  stamp's inputs are knobs (HF Boost, WT Frames, Loop Mode, Normalize, Loop
     *  Optimize, Crossfade, AutoScan) and the locked points are three more, and
     *  acting on every detent of a drag discarded the whole range and rebuilt it
     *  one position per background pass - so for the length of the gesture
     *  nothing was ever finished and the bar sat waiting. One rebuild per
     *  SETTLED drag instead. Build thread only. */
    struct StampSettle
    {
        juce::uint64 pending      = 0;
        juce::uint32 firstSeenMs  = 0;
        juce::uint32 lastChangeMs = 0;
        bool         active       = false;
    };
    StampSettle cachePosSettle_;
    /** Quiet for this long and the rebuild goes ahead. A detent of a knob drag
     *  is far quicker than this; a hand that has stopped is not. */
    static constexpr juce::uint32 kCachePosSettleMs = 250;
    /** ...but never defer longer than this, and the cap is consulted on the
     *  pass the value CHANGES as well as on a pass where it repeats: a stamp
     *  input that is MODULATED changes on every pass and reaches no other
     *  branch, so a cap it cannot reach cannot bound the one starvation it is
     *  for - the positions never rebuilt at all, which reads exactly like the
     *  feature working. */
    static constexpr juce::uint32 kCachePosMaxDeferMs = 2000;
    /** True on the single pass a RUN of settings changes ends - the hand came
     *  off, or the cap ran out. Not "the stamp differs from what is built": a
     *  control moved away and back inside the window ends where it started, and
     *  a stamp compare would then see nothing and never rebuild. */
    bool settingsRunEnded (juce::uint64 setNow, juce::uint32 nowMs);
    int          cachePosNextToBuild_ = 0;    // build thread only
    // The host's rate and block size, written by prepareToPlay and read by the
    // build thread. Part of the stamp, so a rate change invalidates what is
    // built the same way a parameter change does - and the engines themselves
    // are re-prepared on the BUILD thread, which is the only thread that ever
    // mutates them (prepareBufferLoad and prepareFramesFromBuffer are not const).
    std::atomic<double> cachePosHostRate_ { 0.0 };
    std::atomic<int>    cachePosHostBlock_ { 0 };


    /** Guards inferenceCacheEntries and inferenceCacheCapacity between the
     *  message thread that fills them and the position builder that reads them.
     *  NOT getCallbackLock(): the audio thread never touches this vector (it
     *  asks inferenceCacheTraversableZones_, an atomic, for exactly that
     *  reason), and using the callback lock parked it for a whole-take
     *  makeCopyOf - megabytes of memcpy with processBlock waiting, which on a
     *  deep cache of long takes is a dropout per position built. */
    mutable std::mutex cacheEntriesMutex_;
    /** What the positions must match to be current. Cheap enough to call per
     *  block; message/audio thread safe (atomics and parameter reads only). */
    /** WHAT the positions would be built FROM. Returns 0 when there is nothing
     *  to build. Acted on at once - see the built stamp. */
    juce::uint64 cachePositionSourceStamp() const;
    /** HOW a position would be built - the controls. Debounced. Never 0, so it
     *  cannot be confused with "nothing to build". */
    juce::uint64 cachePositionSettingsStamp() const;
    /** Is ANY position in [lo, hiExclusive) built and current? */
    bool positionsReadyInRange(int lo, int hiExclusive) const;
    /** Take back every voice claim pointing into [lo, hiExclusive). Caller holds
     *  getCallbackLock(); the engines in that range are about to be refilled. */
    void revokeVoiceClaimsInRange(int lo, int hiExclusive);
    /** Drop [lo, hiExclusive) from the ready mask, take the claims back and hand
     *  those voices to the instrument-wide masters by crossfade. */
    void invalidatePositionRange(int lo, int hiExclusive);
    /** Build at most one position. Returns true if it did work. Runs on
     *  samplerReprepareThread; publishes under getCallbackLock(). */
    bool serviceCachePositionEngines();
    /** One cache entry through loadGeneratedAudio's own conditioning and prepare
     *  calls, into `dest`. Off the audio thread. False when the entry is empty. */
    bool prepareCachePosition(CachePositionEngines& dest,
                              const juce::AudioBuffer<float>& raw,
                              double sr);
    /** Push the live Regen XFade onto every built position, in place. The one
     *  thing a position needs kept current that must NOT be in the stamp: it is
     *  a control the player turns while playing, and rebuilding on every step of
     *  that drag takes the per-note path off for the whole gesture. Safe to write
     *  under a sounding voice because morphTimeMs_ is an atomic scalar. */
    void refreshMorphTimeOnCachePositions();
    /** The prepared engines for a cache position, or nullptr when that position
     *  is not (yet) current. Audio-thread safe. */
    const CachePositionEngines* readyCachePosition(int index) const;
    /** Per-note traversal state - the instrument-wide atCacheZone_/BaseZone_/
     *  Engaged_ triple, once per voice. One of these per BAR; the expression
     *  matrix has one travelling bar today (Cache), and the struct stays a
     *  per-bar thing because that is what the state IS. Audio thread only. */
    struct VoiceBarState
    {
        std::array<int16_t, VoiceManager::MAX_VOICES> zone {};
        std::array<int16_t, VoiceManager::MAX_VOICES> base {};
        std::array<bool,    VoiceManager::MAX_VOICES> engaged {};
        // Whose gesture the state above belongs to: the voice's fresh-strike
        // epoch and the member channel it answers. A voice changes hands in two
        // ways that are otherwise invisible from here - a STEAL (a fresh strike
        // on a voice whose old key was still down) and the mono LEGATO hand-off
        // (a new finger on a new member channel, deliberately without a fresh
        // strike) - and in both the gesture standing here belongs to a finger
        // that is gone.
        bool active = false;            // this bar's per-note path was in charge
    };
    VoiceBarState atVoiceCache_;
    /** WHOSE HAND is on each voice, tracked once per block. A property of the
     *  voice, not of a bar - and it has to be recorded whether or not the bar is
     *  steering. Kept per bar it went stale in every gap: a
     *  bar at rest, or one whose amount was zero, stopped recording epochs, and
     *  its FIRST pass after coming back read that staleness as "this voice
     *  changed hands" and threw away the record of where the voice points and
     *  who owns it - under a finger that had not moved. */
    std::array<juce::uint64, VoiceManager::MAX_VOICES> atVoiceEpoch_ {};
    std::array<int,          VoiceManager::MAX_VOICES> atVoiceChan_ {};
    /** WHICH position each voice is pointed at, -1 for the instrument-wide
     *  master. One record for all bars, deliberately: a voice has one set of
     *  engine masters, so a bar remembering its own landed index could have it
     *  made stale by another one re-pointing the voice - and would then read
     *  "already pointed there" and refuse to act when the finger came back to
     *  it. One voice, one position, one record of it. */
    std::array<int16_t, VoiceManager::MAX_VOICES> atVoicePosIdx_ {};
    /** WHICH BAR last moved this voice, and therefore owns where it points:
     *  0 nobody, 1 the cache bar. ONE claimant since Snap left the expression
     *  matrix, and the arbiter stays because it is what makes the shared record
     *  above safe: bars run in the same block, over the same voices, into that
     *  one record, so without an owner the second to run wins every block
     *  regardless of which finger moved, and two of them re-point the same voice
     *  back and forth on every buffer with no hand moving at all. Claimed by
     *  MOVING: a bar takes a voice on the block its own zone changes under it,
     *  keeps it while it is the only one travelling, and hands it over the
     *  moment another bar's zone changes. Last gesture wins, which is the only
     *  reading that matches what the player did. */
    std::array<int8_t, VoiceManager::MAX_VOICES> atVoiceBarOwner_ {};
    // At least one voice was pointed at a new position and the message thread
    // has work to finish for it. TWO things, both of which only it may do:
    //
    //  - A held GRANULAR voice still has to crossfade onto its new buffer. The
    //    audio thread cannot make that one itself, because
    //    FreezeTextureEngine::morphToBufferFrom frees retired snapshots.
    //  - Every voice that DID morph on the audio thread (sampler) parked its
    //    old snapshot in a one-deep reclaim slot, and SamplePlayer::
    //    morphToBufferFrom REFUSES while that slot is full. Without a drain in
    //    the scrub's own loop, the four existing drain sites only ever run on a
    //    publish (regenerate / re-prepare / position build), so a held sampler
    //    note followed the bar exactly once and then stopped.
    std::atomic<bool> voiceCacheServicePending_ { false };
    /** The audio thread revoked the claims itself and a held GRANULAR voice
     *  still has to be crossfaded back to the instrument-wide master. It cannot
     *  do that one: processBlock's own freeze pass never morphs (allowMorph is
     *  false there by contract), and morphHeldFreezeVoicesToOwnMasters only ever
     *  reaches a voice's OWN master, which the revoke has just set to null - so
     *  without this the note would go on playing a cache take across the very
     *  regenerate the revoke exists to deliver. Sampler and wavetable need no
     *  hop: their audio-thread morph falls back to the instrument-wide master
     *  by itself. */
    std::atomic<bool> voiceCacheHandBackReq_ { false };
    /** A new instrument-wide sound has landed, so every per-note claim is void
     *  and every finger has to earn its position again. Raised off the audio
     *  thread, consumed by updateVoiceCachePositions ON it - which is the only
     *  way to close the window where the audio thread installs a claim between
     *  the message thread's revoke and its distribute pass, and the only place
     *  the per-voice gesture state may be touched. Without the gesture re-arm a
     *  voice keeps its old landed index, that index still matches the zone the
     *  motionless finger is standing in, and the bar simply stops working under
     *  that finger until it leaves the zone and comes back. */
    /** The audio thread must take its claims back: the positions have been
     *  rebuilt under them. Raised off the audio thread, consumed on it. */
    std::atomic<bool> voiceCacheRevokeReq_ { false };
    /** Every voice that changed hands since the last block loses the bar's
     *  gesture, the record of where it points and its ownership. Run
     *  unconditionally, above the bar and above every early return - see
     *  atVoiceEpoch_. Audio thread. */
    void updateVoiceHandChanges();
    void updateVoiceBarPositions(VoiceBarState& st, float amt, int src, int zones,
                                 int posBase, bool& repointed);
    /** Ask the message thread for the half of a re-point it alone may do - the
     *  granular crossfade and the sampler reclaim drain. */
    void requestVoiceCacheService();
    /** Whether the arpeggiator is on AND holding keys - the condition under
     *  which the cache bar must stay instrument-wide, because an arpeggiated
     *  chord makes no voice to read per note. */
    bool arpIsHoldingKeys() const;
    void updateVoiceCachePositions(float cacheAmt, int cacheSrc, int zones);
    /** Forget ONE bar's gesture and nothing else - not another bar's, not where
     *  any voice points. For the events that change what a bar MEANS (its axis)
     *  rather than what the positions CONTAIN. */
    void rearmVoiceBar(VoiceBarState& st);
    /** Drop the record of where voices point for positions in [lo, hiExclusive),
     *  and clear those voices' masters. Audio thread. */
    void forgetPositionIndices(int lo, int hiExclusive);
    /** A bar that is no longer steering gives its voices back - see the body. */
    void dropVoiceBarOwnership(const VoiceBarState& st);
    /** ONE bar going to rest: forget that bar's gesture and keep every sample. */
    void releaseVoiceBar(VoiceBarState& st);
    void releaseVoiceCachePositions();
    std::thread samplerReprepareThread;
    std::atomic<bool> samplerReprepareThreadShouldExit { false };
    std::atomic<bool> samplerReprepareWorkRequested { false };
    std::mutex samplerReprepareSourceMutex;
    juce::AudioBuffer<float> samplerReprepareSourceBuffer;
    double samplerReprepareSourceRate = 44100.0;
    juce::uint64 samplerReprepareSourceVersion = 0;
    float samplerReprepareNormalizeStartFrac = 0.0f;
    float samplerReprepareNormalizeEndFrac = 1.0f;
    bool samplerReprepareSourceValid = false;

    // Csound engine — D9 lazy/background compile. A live engine_mode switch
    // into Csound is caught by parameterChanged (which can run on the AUDIO
    // thread — see its own comment) and must never launch a thread there,
    // so it only flags csoundWantsPrepare_ + triggerAsyncUpdate(); the actual
    // std::thread launch happens in handleAsyncUpdate (message thread, same
    // pattern as the CC-Learn / XL-button requests below). Mirrors
    // samplerReprepareThread's shape but is a one-shot compile, not a
    // recurring poll worker — joined and relaunched per switch, never left
    // running idle.
    //
    // csoundLifecycleMutex_ (adversarial-review finding, post-implementation):
    // prepareToPlay is NOT guaranteed to run on the message thread — only
    // guaranteed not to be concurrent with processBlock (e.g. the Standalone
    // wrapper's AudioProcessorPlayer calls it from the audio-device setup
    // thread). handleAsyncUpdate's background-compile launch runs on the
    // message thread. Without a lock, those two could each end up calling
    // an engine's prepare() concurrently (one directly, one via the spawned
    // thread) — a data race on Impl's csound*/ready state — and could also
    // race on csoundCompileThread_'s own join()/joinable()/operator= (calling
    // std::thread member functions concurrently on the same object from two
    // threads is itself UB, independent of what CsoundEngine does). This
    // mutex serializes: (a) all csoundCompileThread_ join/joinable/reassign
    // sequences, (b) every CsoundEngine::prepare() call on EITHER engine, from
    // whichever thread makes it, and — Phase 2 (S8) — (c) every read/write of
    // the pending orchestra-swap text/snapshot/generation counters just below
    // (never touched by the audio thread at all: S8 "text/snapshot live
    // entirely on the compile side"). Held for ~100ms-400ms at most (compile +
    // the ~0.25s S3 prime, when a swap is involved), only in the rare
    // interleaving where a live engine-mode switch / orchestra-swap request
    // and a host prepareToPlay overlap — never on the audio thread itself.
    // `mutable` so the const csoundCompileError() status accessor can lock it.
    mutable std::mutex csoundLifecycleMutex_;
    std::thread csoundCompileThread_;
    std::atomic<bool> csoundCompileInFlight_ { false };
    std::atomic<bool> csoundWantsPrepare_ { false };
    // Audio-thread-only (processBlock): samples-since-last-Csound-channel-write,
    // carried across host block boundaries as a negative offset (spec §3/D2) so
    // the freq glide smoother's first advance of a new block correctly accounts
    // for the tail of the previous block that had no further MIDI event to
    // trigger a write. Untouched (irrelevant) whenever engine mode isn't Csound.
    int csoundLastWritePos_ = 0;

    // ── Csound orchestra swap (Phase-2 spec S4/S8) ──────────────────────────
    // Pending request state: compile-thread-only data, guarded by
    // csoundLifecycleMutex_ above, NEVER touched by the audio thread.
    juce::String csoundPendingOrchestraText_;
    // Mirror of the line above for GUI readers (getCsoundOrchestraText), with a
    // mutex of its own that is never held across anything slow. The lifecycle
    // mutex is held by the compile thread for the whole prepare + warmup, and a
    // SNAP press must not wait that out on the message thread. Written in
    // lockstep with csoundPendingOrchestraText_ inside requestCsoundOrchestra.
    juce::String csoundOrchestraTextForUi_;
    mutable std::mutex csoundOrchestraTextMutex_;
    float csoundPendingEpochs_[CsoundEngine::kMaxVoices] {};
    float csoundPendingFreqs_[CsoundEngine::kMaxVoices] {};
    // Bumped by requestCsoundOrchestra() on every call ("latest wins", S4);
    // csoundSwapStartedGeneration_ is the generation the most recently
    // LAUNCHED compile targeted. handleAsyncUpdate starts a new compile only
    // when these differ (unconsumed request) and nothing else is in flight —
    // a request arriving while an older one compiles or fades just re-writes
    // the pending slot + bumps this counter; the in-flight/just-finished
    // compile checks it again on completion and, if superseded, discards its
    // own result (even on success) instead of publishing a stale target.
    uint64_t csoundSwapRequestGeneration_ = 0;
    uint64_t csoundSwapStartedGeneration_ = 0;
    juce::String csoundCompileErrorText_;   // last failure (empty = none)

    // Published by the compile thread on success (release); consumed by
    // processBlock at the startBlock site (acquire) to arm the fade (S5).
    std::atomic<bool> csoundSwapPending_ { false };
    // Set/cleared by the audio thread ONLY (processBlock); read by the message
    // thread for status display (csoundSwapFading()) and by handleAsyncUpdate
    // to defer starting the next queued compile until the current fade ends
    // (S4: "a request arriving during an active fade ... starts after the
    // fade completes").
    std::atomic<bool> csoundSwapFading_ { false };
    // Audio-thread-owned fade position/length (processBlock only, never
    // touched off that thread — no atomics needed for these two).
    int csoundFadePos_ = 0;
    int csoundFadeLen_ = 1;
    // Fade mix buffers (S5/S8): preallocated in prepareToPlay (message
    // thread), sized samplesPerBlock. mix[vi][startSample+i] = the crossfaded
    // sample VoiceManager reads for voice vi during the ~200ms fade window —
    // the ONLY per-block cost beyond Phase-1's is during that window.
    std::array<std::vector<float>, CsoundEngine::kMaxVoices> csoundMixBufs_;
    // Per-sample equal-power gain scratch (adversarial-review finding,
    // post-implementation): TRUE sin/cos of each sample's ABSOLUTE fade
    // position, computed ONCE per sample and shared across all
    // kMaxVoices — never a per-(voice,sample) LINEAR interpolation of two
    // endpoint sin/cos values, which does NOT preserve gNew^2+gOld^2==1 in
    // the interior of a sub-range (degrades toward a plain linear crossfade,
    // a real -3dB power dip at the segment midpoint, whenever a sub-range
    // spans a non-trivial arc — e.g. the WHOLE 200ms fade in one sub-range at
    // a short Regen XFade time, or during an offline bounce with a large host
    // block). Preallocated here so the audio thread never allocates.
    std::vector<float> csoundFadeGainNew_;
    std::vector<float> csoundFadeGainOld_;

    // DSP — global (shared across voices, post-sum)
    LFO lfo1;
    LFO lfo2;
    LFO lfo3;
    DriftLFO driftLfo;
    T5ynthFilter postFilter;
    T5ynthDistortion ampDistortion;   // the amplifier chain, ahead of delay/reverb
    T5ynthChorus     ampChorus;
    T5ynthPhaser     ampPhaser;
    T5ynthTremolo    ampTremolo;
    T5ynthDelayLine delay;
    ConvolutionReverb reverb;
    AlgorithmicReverb algoReverb;
    OutputCeiling outputCeiling;      // standalone only -- see its own header

    // The static output gain, one block behind, so a moved control RAMPS across
    // the block instead of stepping. It replaces the makeup gain that
    // juce::dsp::Limiter used to apply (and smoothed over 1 ms for the same
    // reason); what went away with that widget is its compression AND its level,
    // which the instrument was borrowing. The gain follows `limiterThresh`,
    // which a preset load or host automation can move at any time.
    // Seeded by seedOutputStageGains() below -- prepareToPlay, the deep-idle
    // path, and processBlockBypassed all call it, so the first block after a
    // device change, after ten silent seconds, or after a host un-bypasses the
    // plugin ramps from the current settings. A host that bypasses without
    // calling processBlockBypassed, or stops calling the plugin for a while,
    // resumes ramping from the value before the gap.
    float outputGainPrev_ = 1.0f;

    // The same, for the sequencer's one-shot samples. They are not voices, so
    // they are referred to kOneShotReferenceGain and pre-divided by whatever
    // the master stage is about to multiply by (PluginProcessor.cpp, addOneShots).
    float oneShotPreGainPrev_ = 1.0f;

    // The Snap switch fade (snapFadeOut/snapFadeIn). The state is set by the
    // message thread and advanced by the audio thread. The position (1 = open,
    // 0 = silent; the gain is a raised cosine of it) belongs to the audio
    // thread, and to prepareToPlay while no block runs.
    enum : int { kSnapFadeOpen = 0, kSnapFadeClosing, kSnapFadeClosed, kSnapFadeOpening };
    std::atomic<int> snapFadeState_ { kSnapFadeOpen };
    float snapFadePos_ = 1.0f;
    // How far apart the host's blocks arrive, for snapFadeOut's wait: the longest
    // gap between two block starts, or the longest block, relaxing over seconds.
    // Not the last block's length -- a host may run a 2048-frame device period as
    // eight 256-frame blocks back to back.
    // Until the gaps between blocks have been measured. Long enough that the wait
    // (25 + 3 x 60 ms) covers an 8192-frame period at 44.1 kHz even when the host
    // runs it as short blocks and reports only those as its maximum.
    static constexpr float  kSnapHostPeriodSeedMs = 60.0f;
    static constexpr double kSnapHostPauseMs      = 500.0;    // a longer gap is a pause, not a period
    std::atomic<float> hostPeriodMs_ { kSnapHostPeriodSeedMs };
    juce::int64 prevBlockStartTicks_ = 0;   // audio thread
    void noteHostBlock (juce::int64 startTicks, int numSamples) noexcept;
    void applySnapFade (juce::AudioBuffer<float>& buffer, int numSamples) noexcept;
    void settleSnapFadeWhileSilent() noexcept;

    // Master volume, one block behind, same reasoning as outputGainPrev_ above:
    // a moved master_vol RAMPS instead of stepping at the boundary -- but over
    // kMasterRampMs (1 ms, the declick minimum), not the whole block; see
    // kMasterRampMs's comment in PluginProcessor.cpp. Seeded wherever
    // outputGainPrev_ is seeded -- see seedOutputStageGains() below.
    float masterGainPrev_ = 1.0f;

    // Sets outputGainPrev_, oneShotPreGainPrev_ and masterGainPrev_ to what a
    // block starting right now would settle at. Otherwise only processBlock
    // advances them, oneShotPreGainPrev_ where the one-shots are pre-divided
    // and the other two in the master stage at its end, so anywhere that code
    // does not run -- prepareToPlay (nothing has played yet), the deep-idle
    // early return (both sites are below it), and processBlockBypassed (the
    // host runs this instead of processBlock while bypassed) -- re-seeds them
    // here instead, or the next real block would ramp from a value left over
    // from before the gap. Defined in PluginProcessor.cpp right before
    // processBlock.
    void seedOutputStageGains() noexcept;

    // The reverb-crossfade counterpart of seedOutputStageGains() above: marks
    // a gap no voice sounds through (prepareToPlay with no voice active, the
    // deep-idle return), so that the first block with samples after it ramps
    // the dry gain (prevReverbDry_, declared with the FX members below) from
    // its own mix with every envelope at 0; the wet gain keeps ramping from
    // the last block's value (see the definition for why). Defined in
    // PluginProcessor.cpp right next to seedOutputStageGains().
    void seedReverbCrossfade() noexcept;

    // Sequencer
    T5ynthStepSequencer stepSequencer;
    T5ynthGenerativeSequencer generativeSequencer;
    T5ynthArpeggiator arpeggiator;
    bool genModeActiveInAudio = false;  // tracks which engine is currently running
    int lastGenSteps = -1, lastGenPulses = -1, lastGenRotation = -1;
    float lastGenMutation = -1.0f;

    // Internal note events from the sequencers/arp — typed, NOT MIDI. Reused
    // each block (cleared, capacity retained) so the audio thread never
    // allocates. Merged with the external MIDI stream in processBlock.
    std::vector<VoiceEvent> internalNoteEvents_;

    // Scratch buffer for the arp's external-MIDI filter (note-ons/offs consumed,
    // everything else kept). A member, not a local, so it keeps its storage across
    // blocks instead of allocating a fresh buffer on the audio thread every block.
    juce::MidiBuffer arpFilteredMidi_;

    struct SequencerOneShotSample
    {
        juce::AudioBuffer<float> audio;
        double sampleRate = 44100.0;
        juce::String label;
    };
    using SequencerOneShotSamplePtr = std::shared_ptr<const SequencerOneShotSample>;
    static constexpr int kMaxSequencerOneShotVoices =
        T5ynthStepSequencer::MAX_STEPS * T5ynthStepSequencer::ONE_SHOT_SLOTS;
    std::array<std::array<SequencerOneShotSamplePtr,
                          T5ynthStepSequencer::ONE_SHOT_SLOTS>,
               T5ynthStepSequencer::MAX_STEPS> sequencerOneShotSamples;
    struct PendingSequencerOneShot
    {
        SequencerOneShotSamplePtr sample;
        float gain = 1.0f;
        int sampleOffset = 0;
    };
    struct ActiveSequencerOneShot
    {
        SequencerOneShotSamplePtr sample;
        double position = 0.0;
        double increment = 1.0;
        float gain = 1.0f;
        int startOffset = 0;
        bool active = false;
        uint64_t age = 0;
    };
    std::array<PendingSequencerOneShot, kMaxSequencerOneShotVoices> pendingSequencerOneShots;
    std::array<ActiveSequencerOneShot, kMaxSequencerOneShotVoices> activeSequencerOneShots;
    int pendingSequencerOneShotCount = 0;
    uint64_t sequencerOneShotAgeCounter = 0;
    void queueSequencerOneShotTrigger(const T5ynthStepSequencer::OneShotTrigger& trigger);
    void renderSequencerOneShots(juce::AudioBuffer<float>& buffer);
    void stopSequencerOneShots();
    bool hasActiveSequencerOneShots() const;

    // ── One-shot sample retirement (deferred off-audio-thread release) ──────
    // CLAUDE.md JUCE-safety #4: a shared_ptr refcount reaching 0 frees the held
    // juce::AudioBuffer, and that free must never run on the audio thread. The
    // array store side (assign/clear/copy/import) already releases on the message
    // thread via std::atomic_store. But the audio thread also drops one-shot refs
    // whenever a voice ends, is stolen/reused, or is stopped; if the user cleared
    // or overwrote that slot meanwhile, the still-sounding voice can hold the LAST
    // ref, so a plain reset()/assignment there would free on the audio thread.
    //
    // Instead the audio thread MOVES the ref into this lock-free SPSC ring — a
    // move never touches the refcount, allocates, or frees — and the always-on
    // samplerReprepare worker thread drains it, performing the real release off
    // the audio thread. Same retire-bin idea as FreezeTextureEngine's
    // retiredPublished_/retiredMorphFrom_, generalised to a ring because a voice
    // ending is an audio→worker hand-off rather than a message-thread-local park.
    //
    // Sole producer: the audio thread (writes head). Sole consumer: the worker
    // thread (writes tail). Capacity comfortably exceeds the 192 maximum live
    // voices; on the (practically unreachable) full ring the producer leaves the
    // ref in place — still held, never freed — to be retired on a later pass.
    static constexpr int kOneShotRetireCapacity = 512;  // power of two, > kMaxSequencerOneShotVoices (192)
    static constexpr int kOneShotRetireMask = kOneShotRetireCapacity - 1;
    std::array<SequencerOneShotSamplePtr, kOneShotRetireCapacity> oneShotRetireBin;
    std::atomic<int> oneShotRetireHead { 0 };  // audio thread (producer) only
    std::atomic<int> oneShotRetireTail { 0 };  // worker thread (consumer) only
    bool retireOneShotSampleToBin(SequencerOneShotSamplePtr& ptr) noexcept;  // audio thread
    void drainSequencerOneShotRetireBin() noexcept;                          // worker thread

    // ── MPE zone layout and pitch-bend ranges ───────────────────────────────
    // juce::MPEZoneLayout owns the MPE Configuration Message, the bend-range
    // RPN, the sixteen per-channel RPN detectors and the master/member
    // classification. Rationale, and every place this synth deviates from it:
    // docs/MPE_MIGRATION_PARITY.md. Audio thread only, and allocation-free
    // there -- tools/measure_mpe_instrument_rt.cpp is the measurement, and is
    // also why juce::MPEInstrument is NOT here.
    //
    // The lower zone is declared rather than left empty because MPEInstrument's
    // rule holds for the layout too: a channel no zone covers is not MPE. 15
    // members is what every host defaults to, it makes channel 16 a MEMBER
    // structurally, and it keeps a plain keyboard on channel 1 playing.
    static constexpr int kMpeDefaultMemberChannels = 15;
    // kMpePerNoteBendRange / kMpeMasterBendRange are PUBLIC (see above): the
    // frozen corpus pins them at compile time.

    // kMpeXFullScaleSemitones is PUBLIC (see above), same reason.

    // Only the channel count is read back out of this: the two ranges are
    // seeded so the zone is not silently inconsistent with the pair below, but
    // the pair below is the authority and nothing ever reads the zone's own.
    juce::MPEZoneLayout mpeZones_ { juce::MPEZone { juce::MPEZone::Type::lower,
                                                    kMpeDefaultMemberChannels,
                                                    kMpePerNoteBendRange,
                                                    kMpeMasterBendRange } };

    // A second detector over the same bytes, in step with the one inside
    // mpeZones_ because it sees the same stream. It answers what the layout
    // cannot be asked: which parameter a completed RPN carried.
    juce::MidiRPNDetector mpeRpnWatcher_;

    // The bend ranges in force. Deliberately NOT read back out of mpeZones_,
    // and the split is the point: the library owns the LAYOUT and the parsing,
    // this instrument owns the range POLICY, exactly as it did before.
    //   - One pair for the whole synth, not one per zone. Every call site
    //     assumes that, and a zone declaration would otherwise reset the range
    //     a player had already dialled in: an MCM carries no range of its own,
    //     but MPEZoneLayout writes the spec's defaults whenever one arrives.
    //   - The per-note default is the spec's 48 (see kMpePerNoteBendRange for
    //     why it is not 24), and the MPE settings tab can move it for a device
    //     that transmits no RPN 0. SynthVoice still clamps to ±48, so a
    //     controller that transmits more is honoured up to that.
    //   - ATOMIC, because the MPE settings tab writes them from the message
    //     thread while the audio thread reads them. Relaxed: they are two
    //     independent scalars that carry no ordering for anything else, and a
    //     block rendered with the old range is simply the block before the
    //     change. Whoever wrote LAST wins -- the player's setting and the
    //     device's RPN 0 are the same authority over the same number, and the
    //     tab's diagnosis is what shows which one spoke.
    std::atomic<int> mpePerNoteBendRangeInForce_ { kMpePerNoteBendRange };
    std::atomic<int> mpeMasterBendRangeInForce_  { kMpeMasterBendRange };
    // What a full lateral lean is worth as a MODULATION source, in semitones.
    // No device ever transmits this -- it is not in the spec -- so unlike the
    // two above it has one writer, the settings tab.
    std::atomic<float> mpeXFullScaleInForce_ { kMpeXFullScaleSemitones };

    // ── What the connected controller is actually sending ────────────────────
    // Written by the audio thread as MIDI arrives (plain relaxed stores, no
    // allocation, no branch that can grow), read by the MPE tab's timer while
    // that tab is on screen. The reason it exists: every range and normalisation
    // above is negotiated ONCE, by an MCM and an RPN 0 the device sends at
    // power-on or mode-select. A plugin loaded after that moment never hears
    // them and cannot ask -- MIDI has no read-back. So the choice is between
    // guessing and looking, and this is looking.
    //
    // The audio thread is the only writer of NEW observations, so the min/max
    // pairs need no compare-exchange. clearMpeObservation() is a second writer
    // and its interleaving IS reachable: a reset landing between the audio
    // thread's load and its store leaves min at its empty sentinel with max
    // already set. The reader therefore tests min <= max rather than max >= 0,
    // and an inverted pair reads as "nothing observed" for one tick.
    struct MpeObservation
    {
        std::atomic<int>          messages { 0 };
        std::atomic<juce::uint32> noteChannels { 0 };
        std::atomic<juce::uint32> bendChannels { 0 };
        std::atomic<juce::uint32> timbreChannels { 0 };
        std::atomic<juce::uint32> memberPressureChannels { 0 };
        std::atomic<bool>         masterPressure { false };
        std::atomic<bool>         polyPressure { false };
        std::atomic<int>          rpnRange { 0 };        // 0 = never transmitted
        std::atomic<int>          rpnChannel { 0 };
        std::atomic<bool>         mcmSeen { false };
        // The zone, MIRRORED here rather than read out of mpeZones_ from the
        // message thread. juce::MPEZone is four plain ints and getLowerZone()
        // returns it by value, so reading it across threads is a formal data
        // race for no gain -- the audio thread already holds the answer at the
        // one instant it can change, so it publishes it.
        std::atomic<int>          memberChannels { kMpeDefaultMemberChannels };
        std::atomic<bool>         upperZone { false };
        std::atomic<int>          cc74Min { 128 };       // empty range: min > max
        std::atomic<int>          cc74Max { -1 };
        std::atomic<int>          bendMin { 16384 };
        std::atomic<int>          bendMax { -1 };
    };
    MpeObservation mpeObs_;

    /** One MIDI message, seen for the diagnosis only -- it decides nothing and
        consumes nothing. Audio thread. */
    void observeMpeMessage (const juce::MidiMessage& msg, int channel) noexcept;

    // The RPN parameter-select bytes, and ONLY those. CC6 is deliberately
    // absent: one that completes no RPN we act on must still reach a user
    // binding, so its fate is handleMpeRpnByte's return.
    static bool isMpeRpnSelectController (int cc) noexcept { return cc == 100 || cc == 101; }

    // The NRPN ones, tracked HERE and never handed to a MidiRPNDetector.
    // The detector keeps ONE parameter register per channel and lets RPN and
    // NRPN both write it (juce_MidiRPN.cpp:80-84), so feeding it these bytes
    // lets a CC99 and a CC100 assemble a parameter number that neither of them
    // selected -- and MPEZoneLayout::processRpnMessage never looks at isNRPN
    // (juce_MPEZoneLayout.cpp:131-137), so an ordinary NRPN 6 write would
    // install an MPE zone. Both were measured before this was written.
    static bool isNrpnSelectController (int cc) noexcept { return cc == 98 || cc == 99; }

    // One bit per channel: an NRPN is selected there, so the next CC6 is that
    // NRPN's data byte and not Pitch Bend Sensitivity. Without it a controller
    // writing NRPNs silently retunes the bend range, because the detector keeps
    // the last RPN latched -- the hand-written parser had the same hole. This
    // bit is the whole of the fix, and it stays out of the library's way.
    juce::uint16 mpeNrpnSelected_ = 0;

    /** Feeds one RPN byte to the zone layout; true if MPE consumed it. */
    bool handleMpeRpnByte (int channel, int cc, int value7) noexcept;
    void deselectMpeRpn (int channel, bool lsbOnly) noexcept;

    // Zone-wide expression, or one note's? Channel 1 is unconditional and that
    // is a deliberate stop: it is the lower zone's master AND plain MIDI's
    // global channel, so it carries zone-wide expression whether or not MPE is
    // in play. An upper zone of fifteen members would reach down and make it a
    // member; nothing here handles that layout, and the pitch-bend site reads
    // channel 1 as global outright.
    bool isMpeMasterChannel (int channel) const noexcept
    {
        if (channel == 1)
            return true;
        const auto upper = mpeZones_.getUpperZone();
        return upper.isActive() && channel == upper.getMasterChannel();
    }

    // Edge-detection for arp-toggle note-off cleanup. When arp transitions
    // false→true while a sequencer is running, the seq's currently-sounding
    // note must be flushed before arp's filter starts swallowing noteOffs.
    bool arpWasEnabled = false;

    // Inference (Python subprocess)
    std::shared_ptr<PipeInference> pipeInference = std::make_shared<PipeInference>();
    juce::String lastDevice;
    juce::String lastModel;
    // Modality-routing epoch (see getModalityEpoch / kModalityEpoch). Default = current
    // for a fresh instance; preset/DAW load overwrites it; Init resets it to current.
    int currentModalityEpoch_ = kModalityEpoch;

    // Preset metadata (stored here so preset save can access them)
    juce::String lastPresetName;
    juce::StringArray lastTags;
    juce::String lastPromptA, lastPromptB;
    juce::String humanPromptA, humanPromptB;   // durable human-authored prompts (never the loop's rewrites)
    bool humanPromptsValid = false;
    float lastGenerationTimeMs = 0.0f;
    int lastSeed = 123456789;
    bool lastRandomSeed = false;
    juce::String lastInjectionMode { "linear" };
    float lastLateMixAmount = 0.75f;
    float lastSplitStart = 4.0f;
    float lastSplitEnd = 16.0f;
    std::array<AxisSlotState, 3> lastAxes;
    std::vector<float> lastEmbeddingA, lastEmbeddingB;
    juce::AudioBuffer<float> generatedAudioFull;  // boosted buffer for engines + display
    juce::AudioBuffer<float> generatedAudioRaw;   // raw VAE output (for re-apply on toggle)

    // ── External-audio capture (live input → Resynth init_audio seed) ───────
    // The audio thread writes the main input bus into this pre-allocated ring
    // every block; the message thread snapshots the last N seconds at regen
    // (snapshotExternalCapture). RT-safe: one atomic write index, no locks. The
    // ring is sized kMaxCaptureSeconds of usable history PLUS a margin, so the
    // writer cannot lap the read window during a (sub-millisecond) snapshot copy.
    static constexpr double kMaxCaptureSeconds    = 12.0;
    static constexpr double kCaptureMarginSeconds = 0.5;
    juce::AudioBuffer<float> captureRing;                // [2, (max+margin)*sr]
    std::atomic<int>         captureWritePos { 0 };      // next write index (release-published)
    int                      captureUsableSamples { 0 }; // kMaxCaptureSeconds*sr — snapshot ceiling
    double                   captureSampleRate { 0.0 };  // host SR of the captured audio
    // Serializes the ring (re)allocation in prepareToPlay against the message-
    // thread snapshot read (snapshotExternalCapture). The audio-thread writer
    // NEVER takes it (stays lock-free; writer-vs-reader is covered by the margin),
    // and processBlock is already excluded from prepareToPlay by JUCE — so the
    // audio thread never contends this lock and RT-safety is preserved.
    mutable std::mutex       captureRingMutex;
    double generatedSampleRate = 44100.0;
    int inferenceCacheCapacity = 0;
    int inferenceCachePlaybackIndex = 0;
    std::vector<InferenceCacheEntry> inferenceCacheEntries;
    bool inferenceCacheIsOfflineTake = false;
    // The audio thread's only window onto the cache; see
    // getInferenceCacheTraversableZones(). Written by
    // publishInferenceCacheTraversableZones() from every mutation above.
    std::atomic<int> inferenceCacheTraversableZones_ { 0 };
    std::atomic<unsigned> inferenceCacheGeneration_ { 0 };

    std::vector<CsoundCacheEntry> csoundCacheEntries;
    int  csoundCacheCapacity = 0;
    int  csoundCachePlaybackIndex = 0;
    bool csoundCacheIsOfflineTake = false;
    std::atomic<int> csoundCacheTraversableZones_ { 0 };
    std::atomic<unsigned> csoundCacheGeneration_ { 0 };
    // Which panel the player is on - written by the editor's oscillator toggle,
    // read by the active-cache router above. Atomic only because a plugin can be
    // asked for its state off the message thread; it is never read from audio.
    std::atomic<bool> surfaceParadigmIsLanguage_ { false };
    std::atomic<bool>  driftGenHold_   { false };  // message->audio: freeze generation-side Drift
    std::atomic<float> driftGenStepSec_{ 0.0f };   // message->audio: pending take step, in seconds

    /** Everything a freshly generated buffer passes through before it is measured
     *  or played: the rumble filter, the optional HF boost, and the leading and
     *  trailing silence trims. Shared so that audio which is not the live
     *  generation can be brought into the same state -- a stored position
     *  conditioned differently is a different sound. */
    juce::AudioBuffer<float> conditionGeneratedSource (const juce::AudioBuffer<float>& source,
                                                       double sr,
                                                       bool hfBoost) const;

    /** Two-band high shelf to compensate VAE decoder HF rolloff. */
    static void applyHfBoost(juce::AudioBuffer<float>& buffer, double sampleRate);

    /** Rumble filter — 2nd-order Butterworth HP at 25 Hz, removes DC/sub-rumble from VAE output. */
    static void applyRumbleFilter(juce::AudioBuffer<float>& buffer, double sampleRate);

    // Last triggered note (for pitch modulation in block-rate section)
    int lastTriggeredNote = -1;

    // Pre-allocated LFO buffers (avoid heap alloc in processBlock)
    std::vector<float> lfo1Buffer, lfo2Buffer, lfo3Buffer;

    // Persisted LFO output for ghost display (updated every block, even during idle)
    float lastLfo1Val_ = 0.0f, lastLfo2Val_ = 0.0f, lastLfo3Val_ = 0.0f;

    // Waveform display
    juce::AudioBuffer<float> waveformSnapshot;
    std::atomic<bool> newWaveformReady { false };

    // Wavetable-display strip for the engine-window WaveformDisplay fan, published
    // by the DCO/LCO bakes (loadDcoWavetable) and the neural/engine
    // path (publishWtDisplayFromOscFrames). Message-thread write + message-thread
    // read (SynthPanel timer) — the atomic flag is the publish handshake, mirroring
    // waveformSnapshot/newWaveformReady.
    juce::AudioBuffer<float> wtDisplaySnapshot;
    std::atomic<bool> newWtDisplayReady { false };

    // Track loaded reverb IR / seq preset to avoid reloading every block.
    // lastSeqPreset is atomic: the audio thread applies preset changes, while
    // importJsonPreset (message thread) syncs it after restoring a custom step
    // pattern so a stale mismatch can't reload the canned preset over it.
    int lastReverbIr = -1;

    // Reverb dry/wet gains from the previous block, so the crossfade RAMPS instead
    // of stepping. The FxMixLaw dry curve steepens to about -8.5 near mix=1.0 —
    // ~8x the old `dry = 1-mix` slope — so a modulated Mix high in its travel would
    // otherwise zipper at block rate. Audio thread only; reset to bypass (dry 1,
    // wet 0) whenever the reverb is not in circuit, so re-enabling fades in.
    // After a gap no voice sounded through (seedReverbCrossfade():
    // prepareToPlay, the deep-idle path), reverbDrySeedPending_ has the first
    // block with samples ramp the dry gain from its own mix with every
    // envelope at 0, so the next note does not ramp from a stale bypass
    // value. The wet gain keeps ramping from the last block's. A zero-sample
    // call leaves them untouched outside deep idle -- it produced no audio, so
    // the gains last heard are still the ones to ramp from.
    float prevReverbDry_ = 1.0f;
    float prevReverbWet_ = 0.0f;
    bool reverbDrySeedPending_ = false;

    std::atomic<int> lastSeqPreset { -1 };
    // Set by setStateInformation, consumed once by the next processBlock preset
    // apply: distinguishes a DAW state-restore (preserve the saved seqSteps)
    // from a fresh launch or runtime dropdown change (adopt the preset's length).
    std::atomic<bool> seqStateRestored { false };

    // Temporary preview note from step-grid mouse-hold editing.
    bool stepHoldPreviewActive = false;
    int stepHoldPreviewNote = -1;

    // ── Step-record state ──────────────────────────────────────────────────
    // stepRecordArmed is read on the audio thread (MIDI note-on branches) to
    // decide whether to enqueue; the cursor + step writes happen ONLY on the
    // message thread (recordStepNote, called from the computer-keyboard note
    // path and from drainStepRecordQueue). MIDI candidates cross threads via a
    // single-producer (audio) / single-consumer (message) lock-free FIFO.
    std::atomic<bool> stepRecordArmed { false };
    std::atomic<int>  stepRecordCursor { 0 };
    void recordStepNote(int playedNote, float velocity);    // message thread
    void pushStepRecordCandidate(int note, float velocity); // audio thread
    struct StepRecCandidate { int note = 0; float velocity = 0.0f; };
    static constexpr int kStepRecQueueSize = 64;
    std::array<StepRecCandidate, kStepRecQueueSize> stepRecQueue;
    juce::AbstractFifo stepRecFifo { kStepRecQueueSize };
    bool sustainPedalDown_ = false;   // audio thread: CC64 edge tracking for pedal-rest

    // ── Event Log (.t5evt) — audio-thread taps ──────────────────────────────
    // The lock-free ingress rings + drain loop + output file all live inside
    // eventLogWriter_ (below) — see EventLogWriterThread. The taps here just build
    // an entry and hand it to eventLogWriter_->pushNote()/pushParam() (audio-safe,
    // lock-free). Nothing about the recorder reaches back into this processor, so
    // recording is editor-independent and shutdown-safe.
    std::atomic<bool> eventLogEnabled_ { false };
    // Set just before a known-origin setValueNotifyingHost call (CC-Learn today),
    // read-and-cleared inside parameterChanged() — mirrors midiTouchPacked_'s
    // "last writer" idiom. -1 = no hint recorded → defaults to HostAutomation.
    std::atomic<int>  eventLogOriginHint_ { -1 };
    // Suppresses per-param ParamEvents during a bulk preset/state load (one
    // PresetLoaded marker is emitted instead) — see beginBulkParamLoad().
    std::atomic<bool> eventLogSuppressParamEvents_ { false };
    // Running sample clock. eventLogTotalSamples_ is atomic because
    // parameterChanged() (below) can fire on a non-audio thread and reads it for
    // a best-effort ParamEvent timestamp. eventLogBlockStart_ is snapped from the
    // total at the top of every processBlock and is audio-thread-only (only the
    // note taps, themselves only called from processBlock, read it).
    std::atomic<uint64_t> eventLogTotalSamples_ { 0 };
    uint64_t              eventLogBlockStart_ = 0;
    std::unique_ptr<EventLogWriterThread>   eventLogWriter_;
    std::unordered_map<juce::String, int>   eventLogParamIndexById_;   // built once at construction, read-only after
    // Message-thread-only (both touched solely from recordEventLogGeneration,
    // called from PromptPanel's inference-result callback) — atomic only so a
    // future non-message-thread caller can't silently corrupt them.
    std::atomic<uint64_t> eventLogNextGenerationId_ { 1 };
    std::atomic<uint64_t> eventLogLastGenerationId_ { 0 };   // 0 = none yet
    // False until the first real (non-bulk-suppressed) event is logged this session.
    // While false, the start-state is (re)captured on each preset/state load so the
    // header carries the patch actually in effect at t=0; once true, capturing stops
    // (the running patch no longer equals the tape's start). Set at the audio-thread
    // note taps + parameterChanged + recordEventLogGeneration; read on the message
    // thread. See captureEventLogStartStateIfPending().
    std::atomic<bool> eventLogRealEventLogged_ { false };
    // True only across the setStateInformation calls inside startReplay()/stopReplay().
    // Those restores are internal transport machinery, not user/host state changes:
    // the log must not stamp the tape's patch as the live session's start-state, nor
    // emit a replay_start/replay_end preset marker into it. replayModeActive_ can't
    // gate this — it is flipped AFTER the start restore and cleared BEFORE the stop
    // restore, so it is provably false at the exact moment endBulkParamLoad runs here.
    bool eventLogInReplayRestore_ = false;
    // Message thread. Snapshots current APVTS as the replay start state, gated by the
    // flag above. Idempotent; safe to call from any start-of-session hook.
    void captureEventLogStartStateIfPending();

    // ── R2: Replay Transport — internals ──────────────────────────────────────
    // See the public startReplay()/stopReplay() block below for the contract.
    struct ReplayState
    {
        std::vector<NoteEventLogEntry>                noteEvents;
        std::vector<EventLogReader::ParsedParamEvent> paramEvents;
        std::vector<GenerationEventLogEntry>          generationEvents;

        size_t   nextNoteIdx  = 0;   // audio thread
        size_t   nextParamIdx = 0;   // message thread
        size_t   nextGenIdx   = 0;   // audio thread (advances only when the flag is free)

        uint64_t totalDurationSamples = 0;
        double   sampleRate = 44100.0;
    };
    //
    // replayState_ is written by the message thread ONLY while holding the
    // callback lock (startReplay), and read by the audio thread ONLY while
    // replayModeActive_ is true. nextNoteIdx / nextGenIdx are audio-thread-only;
    // nextParamIdx is message-thread-only (replayTimerTick).
    std::atomic<bool>     replayModeActive_ { false };
    ReplayState           replayState_;
    std::atomic<uint64_t> replayPlayhead_        { 0 };   // audio thread writes, both read (TAPE samples)
    std::atomic<float>    replayRate_            { 1.0f };// speed multiplier, see setReplayRate()
    double                replayRateFrac_ = 0.0;          // audio-thread-only fractional carry
    std::atomic<uint64_t> replayDueGenerationId_ { 0 };   // audio raises (1-based idx), message consumes
    std::atomic<bool>     replayGenerationBusy_  { false };// message: a replay generation is in flight

    // Message-thread half of the transport: applies logged param events at the
    // playhead and auto-stops at the end of the tape. Started in startReplay(),
    // stopped in stopReplay() and (defensively) first thing in ~T5ynthProcessor.
    struct ReplayTimer : public juce::Timer {
        T5ynthProcessor* owner = nullptr;
        ~ReplayTimer() override { stopTimer(); }
        void timerCallback() override { if (owner != nullptr) owner->replayTimerTick(); }
    };
    ReplayTimer replayTimer_;
    void        replayTimerTick();

    // Bumped by every startReplay(). A generation dispatched for tape N must not
    // load its audio into — or release the generation slot of — tape N+1 after a
    // stop/restart; the epoch it captured at dispatch is checked on completion.
    std::atomic<uint32_t> replayEpoch_ { 0 };

    // The user's patch, snapshotted before the tape's start-state overwrites it and
    // put back on stopReplay() — Play must not destroy unsaved work. Message thread
    // only; empty when no replay is running.
    //
    // Params only. getStateInformation carries neither the generated audio buffer nor
    // the last seed/embeddings, so after a Stop the SOUND is still the tape's last
    // generation and the seed/DimensionExplorer readouts still show the tape's.
    // Regenerate to get yours back.
    juce::MemoryBlock preReplayPatch_;

    // setStateInformation force-zeroes these three on every restore ("no acoustic
    // surprise on session reopen"). That rule is wrong for Stop Replay, which the
    // user reads as "give me back what I had" — so they are captured alongside the
    // patch and re-applied after it.
    float preReplaySeqRunning_      = 0.0f;
    float preReplayGenSeqRunning_   = 0.0f;
    float preReplayRepromptStance_  = 0.0f;
    float preReplayDcoStance_       = 0.0f;   // the LCO twin; setStateInformation zeroes it too

    // Names the preset_loaded marker that setStateInformation's OWN BulkParamLoadGuard
    // emits (it commits unnamed by default). Set immediately before the call, consumed
    // there. Exists because that guard is not reentrant: wrapping setStateInformation
    // in a second guard would end suppression early and log a doubled marker.
    juce::String stateRestoreMarkerName_;

    // True only when a live session should be written to the .t5evt log: recording
    // is on AND we are not replaying one (a replay's injected notes and re-applied
    // params must not feed back into a new recording).
    bool eventLogRecordingActive() const noexcept
    {
        return eventLogEnabled_.load(std::memory_order_relaxed)
            && ! replayModeActive_.load(std::memory_order_relaxed);
    }

    // skipLeadForArp: when the seq-drives-arp path is about to erase() the lead
    // strand's events out of internalNoteEvents_ (see the arp-lead-extraction
    // block in processBlock), skip logging those same events here instead of
    // logging them AND the arp notes derived from them — the original never
    // reaches voiceManager directly, so counting both would double the note in
    // the log. Mirrors that erase's own predicate exactly; genModeForSkip
    // selects strandId==0 (GenSeq) vs strandId<0 (StepSeq), matching genMode
    // there.
    void logInternalNoteEventsFrom(size_t startIndex, NoteEventLogEntry::Source source,
                                   bool skipLeadForArp = false, bool genModeForSkip = false);
    void logExternalNoteEvent(bool noteOn, int note, float velocity, int channel, int sampleOffsetInBlock);
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    // Idle detection (audio thread only — not atomic)
    int silentBlockCount = 0;
    int tailBlocks = 860;  // recalculated in prepareToPlay (~10s of reverb tail)

    // Tracks the previous-block transport state (PID::seqRunning) so the
    // processBlock can phase-align sync-mode LFO/Drift to beat 1 on a
    // stop→start transition. PID::genSeqRunning is a mode toggle (STEP vs
    // GEN engine), not transport — both engines' .start() is gated on
    // seqRunning alone. Audio thread only — not atomic.
    bool lastSeqRunning = false;

    // Per-modulator sync phase-alignment ("arm to the downbeat"). When a
    // Drift/LFO is switched Off→Sync while the STEP sequencer leads the clock,
    // snapping its phase immediately lands the cycle between beats; instead we
    // ARM it (hold it silent at phase 0) and release it on the next bar
    // downbeat so its first cycle starts on the "1". Index 0..2 = lfo1..3 /
    // drift 0..2. Audio thread only — not atomic.
    int  lastLfoClockMode[3]   { 0, 0, 0 };   // ClockMode::Off
    int  lastDriftClockMode[3] { 0, 0, 0 };
    bool lfoSyncArmed[3]       { false, false, false };
    bool driftSyncArmed[3]     { false, false, false };

    // Pre-allocated buffer for parallel reverb send (avoids heap alloc in processBlock)
    juce::AudioBuffer<float> reverbSendBuffer;

    // Pre-allocated buffer for GenSeq one-shots, kept out of the delay path (they
    // are injected post-delay so the echo line never repeats them) but folded back
    // in before the reverb send so they still reverberate.
    juce::AudioBuffer<float> oneShotBuffer;


public:
    // ── R2: Replay Transport — public API ─────────────────────────────────────
    // Replays a recorded .t5evt session: the start-state patch is restored, logged
    // notes inject at their sample-accurate times, logged params are re-applied at
    // the playhead, and logged generations are re-fired at their logged times and
    // crossfade in when they complete (the drift-regen loop's own timing model).
    //
    // While a replay runs, live input is neutralised: incoming MIDI is dropped at
    // the top of processBlock, and the sequencers/arp are held stopped — the log
    // already contains the notes they produced. Recording into the .t5evt log is
    // suspended too, so a replay never feeds back into a new tape.

    /** Start replaying a parsed session. Message thread only. Restores the
     *  start-state patch, publishes the event vectors under the callback lock,
     *  then flips replayModeActive_. A running replay is stopped first.
     *  Returns false (changing nothing) if the tape carries no decodable start
     *  patch — replaying a tape against a foreign patch would play the right
     *  notes with the wrong sound. */
    bool startReplay(const EventLogReader& reader);

    /** Stop replay and return to live control. Message thread only. Idempotent. */
    void stopReplay();

    bool     isReplayActive()        const noexcept { return replayModeActive_.load(std::memory_order_acquire); }
    uint64_t getReplayPlayhead()     const noexcept { return replayPlayhead_.load(std::memory_order_relaxed); }
    uint64_t getReplayDurationSamples() const noexcept { return replayState_.totalDurationSamples; }

    /** Replay speed multiplier (×0.25–×4). The tape's timeline is wall-clock samples
     *  (no bar grid), so "tempo" is honestly a time-scale: the playhead advances by
     *  numSamples×rate per block. Changeable live; generations still take real time,
     *  so at high rates they bunch (the one-in-flight rule delays, never drops). */
    void  setReplayRate(float rate) noexcept
    { replayRate_.store(juce::jlimit(0.25f, 4.0f, rate), std::memory_order_relaxed); }
    float getReplayRate() const noexcept { return replayRate_.load(std::memory_order_relaxed); }

    /** True while a replayed generation is on the pipe (overlay shows "generating…"). */
    bool isReplayGenerationInFlight() const noexcept
    { return replayGenerationBusy_.load(std::memory_order_relaxed); }

    // Read-only views into the ACTIVE tape for the replay overlay. Message thread
    // only, and only while isReplayActive() — the vectors are reseated (under the
    // callback lock, also from the message thread) each startReplay, so no
    // cross-thread hazard exists for a message-thread reader.
    const std::vector<NoteEventLogEntry>&                getReplayNoteEventsForUi()  const { return replayState_.noteEvents; }
    const std::vector<EventLogReader::ParsedParamEvent>& getReplayParamEventsForUi() const { return replayState_.paramEvents; }
    const std::vector<GenerationEventLogEntry>&          getReplayGenerationEventsForUi() const { return replayState_.generationEvents; }

    /** Message thread. Returns true and fills `out` when the playhead has crossed
     *  the next logged generation. At most one is pending at a time: the audio
     *  thread will not arm the next until this one has been taken, so a slow
     *  generation simply delays the following one rather than dropping it. */
    bool takeDueReplayGeneration(GenerationEventLogEntry& out);

    /** Message thread. Called by PromptPanel when a replay generation finished (or
     *  failed) so the audio thread may arm the next one. No-op if the replay it
     *  belonged to has since been stopped and another started — see replayEpoch_. */
    void replayGenerationFinished(uint32_t epoch);

    /** Identifies the current tape. Capture at generation dispatch, pass back to
     *  replayGenerationFinished(); a mismatch means the tape was swapped underneath. */
    uint32_t getReplayEpoch() const noexcept { return replayEpoch_.load(std::memory_order_acquire); }

    /** Called on the message thread when the tape runs out (replay already stopped).
     *  Set by the editor to reset its transport button; null when no editor is open. */
    std::function<void()> onReplayFinished;

    // Audio idle state (audio thread writes, GUI reads for timer gating)
    std::atomic<bool> audioIdle { false };

    // Host-transport state — written from processBlock, read from
    // resolveSyncBpm() (which itself runs on the audio thread).
    std::atomic<float> hostBpmLastSeen { 0.0f };
    std::atomic<bool>  hostPlayingNow  { false };

    // MIDI panic flag — set by requestMidiPanic() on the GUI thread,
    // consumed/cleared in processBlock on the audio thread.
    std::atomic<bool>  midiPanicRequested { false };

    // MIDI monitor (audio thread writes, GUI reads)
    std::atomic<int> lastMidiNote { -1 };
    std::atomic<int> lastMidiVelocity { 0 };
    std::atomic<bool> lastMidiNoteOn { false };

    // Global (machine-wide, not per-preset) nonlinear-filter oversampling factor:
    // 1 = Off, 2, 4. Read once per block into BlockParams.filterOsFactor; the
    // audio thread only ever touches this atomic, never a settings file. Default
    // 2× (see docs/HANDOVER_FILTER_OVERSAMPLING.md). Teil B adds file persistence.
    std::atomic<int> filterOsFactor_ { 2 };
    // Persist/restore that to the global store (Teil B). Called by the Settings
    // UI on the message thread, never the audio thread. Quality index 0=Off,1=2×,2=4×.
    void setFilterOsQuality(int qualityIndex);
    int  getFilterOsQuality() const;

    // Global (machine-wide, not per-preset) LRO oversampling factor: 1 = Off, 2, 4.
    // DELIBERATELY SEPARATE from filterOsFactor_ above, which wraps the downstream
    // Ladder/Warp nonlinearity: this one sets the rate CSOUND ITSELF computes at,
    // and it is the only place the oscillator's own aliasing can be prevented at
    // all (see CsoundEngine::prepare). The two cost very different amounts, so a
    // user who turns filter oversampling down to save CPU must not silently lose
    // the oscillator's. Default 4× — measured, 2× still leaves the metallic
    // patches audibly dirty in octave 5 (cymbal −13.9 dB vs −181 dB at 4×), and
    // the heaviest library patch still renders 6× faster than realtime at 4×.
    // Read on the message thread only: changing it forces a Csound RECOMPILE
    // (the factor is baked into the compiled orchestra's sr), never a per-block
    // read like filterOsFactor_.
    std::atomic<int> lroOsFactor_ { 4 };
    void setLroOsQuality(int qualityIndex);
    int  getLroOsQuality() const;
    /** The FACTOR itself (1/2/4), not the quality index. The Re-Prompt ear needs it
     *  to render its probe at the rate the live engine compiles the same authored
     *  text at — an authored body may derive its partial count or FM index from
     *  `sr`, so a probe at the bare host rate can render a different sound, not
     *  just a dirtier one. Message thread, like every other reader of this. */
    int  getLroOsFactor() const { return lroOsFactor_.load(std::memory_order_relaxed); }

    // ── LRO AUTHOR provider (external API as an alternative to the local GGUF,
    // NOT to be confused with lroOsFactor_ above, which is Csound's own sample
    // rate) — see docs/IPC_PROTOCOL.md's coder_provider and
    // PipeInference::AuthorProviderConfig. Plain juce::String storage: unlike
    // filterOsFactor_/lroOsFactor_ this is never read on the audio thread or
    // during Csound compilation, only when the Settings UI changes it or a
    // PromptPanel background thread builds the next translate/interpret/csound
    // request via PipeInference — which is why the setter also forwards the
    // config to pipeInference immediately, so a live edit takes effect on the
    // very next call. Default provider "" = local model, unchanged wire bytes.
    void setLroAuthorProviderConfig(const PipeInference::AuthorProviderConfig& config);
    // Not const: juce::ApplicationProperties::getUserSettings() isn't const
    // either (it can lazily create the properties file on first access).
    PipeInference::AuthorProviderConfig getLroAuthorProviderConfig();

    // ── Event Log (.t5evt) — global (machine-wide) on/off, mirrors filterOsQuality ──
    void setEventLogEnabled(bool enabled);
    bool getEventLogEnabled() const;

    // ── MPE device calibration (machine-wide, NOT part of a preset) ──────────
    // The split answers "can one preset drive different MPE controllers":
    // a preset carries DEPTHS, this carries what a full gesture is worth on
    // THIS device. A depth of 0.6 on X then means the same lean on an Osmose
    // and on a LinnStrument, and moving the instrument does not re-tune the
    // patch. Message thread; each stores to the settings file and publishes
    // the value the audio thread reads.
    void  setMpePerNoteBendRange (int semitones);
    int   getMpePerNoteBendRange() const;
    void  setMpeMasterBendRange (int semitones);
    int   getMpeMasterBendRange() const;
    void  setMpeXFullScaleSemitones (float semitones);
    float getMpeXFullScaleSemitones() const;

    // ── The fallbacks the instrument starts from ─────────────────────────
    // Public, and that is deliberate: what is actually in force is a
    // machine-wide user setting, so the frozen MPE corpus pins these
    // COMPILE-TIME defaults instead. A capability guard a preference can
    // turn red is no longer a guard (tools/test_mpe_parity.cpp).
    // The FALLBACK ranges: what applies until a controller transmits RPN 0.
    // 48 and 2 are the MPE spec's own defaults, and the MPE settings tab lets a
    // player override the per-note one for a device that transmits nothing.
    //
    // It was 24 here, with the comment "not the spec's 48: over-bends a
    // LinnStrument". That is backwards, and it under-ranged every device that
    // relied on the default. Roger Linn Design's panel-settings page: the four
    // values on the panel ("+/- 2, 3, 12 or 24 semitones") belong to ONE CHANNEL
    // mode, while ChPerNote -- the LinnStrument's MPE mode -- reads "Bend Range:
    // 48 (This uses the hidden setting 'Any Bend Range')", and that hidden
    // setting reaches 96. So 48 is not a range the reference controller cannot
    // do; 48 is the range it ships in.
    //
    // Under-ranging is the quieter fault of the two, which is why it survived: a
    // device that means 48 and is read at 24 plays every bend at half the
    // interval it intends, in tune with itself and wrong against everything else.
    static constexpr int kMpePerNoteBendRange = 48;
    static constexpr int kMpeMasterBendRange  = 2;

    // How much lateral lean fills the X axis as a MODULATION source, in
    // semitones. The bend itself is untouched by this -- it stays the wheel
    // travel times the range in force, as the spec says. This is only about
    // what X means when a target is routed to it.
    //
    // It has to be a musical interval and not the wheel travel, because the
    // wheel travel is not comparable between instruments. Measured on an Osmose
    // (tools/midi_monitor.cpp, 120 s of ordinary playing): pitch bend never left
    // 8021..8363, i.e. +/-171 of +/-8192 -- 2.1% of the wheel. Routed as a raw
    // wheel fraction, a target on X moved by two percent with the bar pulled all
    // the way over, while a LinnStrument sliding across pads would have covered
    // the same range many times.
    //
    // ONE SEMITONE, because that is the smallest interval that is unambiguously
    // a musical gesture rather than intonation.
    //
    // What that gives on the measured Osmose, stated rather than guessed: +/-171
    // of the wheel against the fallback range of 48 is +/-1.00 semitones, so a
    // full lean fills the axis exactly. Whether the device intends that is not
    // knowable from the capture -- it transmitted no RPN 0 in 120 s, so its own
    // assumed receiver range is unrecorded, and the MPE tab's diagnosis is what
    // will say. What is certain is the direction: 2.1% of the wheel becomes
    // 100% of the axis.
    //
    // This is the DEFAULT now, not the number: the MPE settings tab owns the
    // value in force (mpeXFullScaleSemitonesInForce_).
    static constexpr float kMpeXFullScaleSemitones = 1.0f;
    // Divided by, so it can never be zero -- the settings tab clamps its own
    // range for the same reason: jlimit passes a NaN straight through both
    // clamps and into the expression matrix.
    static_assert (kMpeXFullScaleSemitones > 0.0f,
                   "kMpeXFullScaleSemitones is a divisor -- zero would make X NaN.");

    /** MPE's own bounds: RPN 0 is a 7-bit value and MPEZoneLayout clamps to
        0..96 wherever it owns one (juce_MPEZoneLayout.cpp:74-76). 1 rather than
        0 for the floor because 0 means "no bend" on the wire and honouring it
        would silently disable pitch bend -- the same floor handleMpeRpnByte
        applies to a transmitted range. */
    static constexpr int kMpeBendRangeMin = 1;
    static constexpr int kMpeBendRangeMax = 96;

    /** A flat, copyable read-out of what the controller is sending plus what is
        currently in force. Message thread. */
    struct MpeStatus
    {
        int           messages = 0;
        juce::uint32  noteChannels = 0;            // bit n-1 set = channel n
        juce::uint32  bendChannels = 0;
        juce::uint32  timbreChannels = 0;
        juce::uint32  memberPressureChannels = 0;
        bool          masterPressure = false;
        bool          polyPressure = false;
        int           rpnRange = 0;                // 0 = the device transmitted none
        int           rpnChannel = 0;
        bool          mcmSeen = false;
        int           cc74Min = 128, cc74Max = -1; // empty range: min > max
        int           bendMin = 16384, bendMax = -1;
        int           perNoteBendRange = 0;        // in force, not the default
        int           masterBendRange = 0;
        float         xFullScale = 1.0f;
        int           memberChannels = 0;
        bool          upperZoneActive = false;
    };
    MpeStatus getMpeStatus() const;

    /** Forget what has been seen so far and watch again from now. */
    void clearMpeObservation();
    /** The .t5evt file being recorded this session, or empty if nothing has been
     *  recorded yet. Message thread — used to offer "Save Session Log". */
    juce::File getEventLogCurrentFile() const
    {
        return eventLogWriter_ ? eventLogWriter_->getCurrentFile() : juce::File();
    }
    /** Wrap a full parameter-state replace (preset/snapshot load) between these two
     *  calls: suppresses the per-param ParamEvent flood and logs one coarse marker
     *  instead. Message thread only. */
    void beginBulkParamLoad();
    void endBulkParamLoad(const juce::String& presetName);
    /** Same as endBulkParamLoad but logs no PresetLoaded marker — for a load
     *  attempt that started applying parameters and then failed/aborted. */
    void cancelBulkParamLoad();

    /** Fills in timestamp/generationId (and parentGenerationId, iff
     *  wasInternalResynth — the chain-tracking that makes the resynth
     *  self-feedback loop replayable without storing audio) and logs the entry.
     *  Message thread (called from PromptPanel's inference-result callback).
     *  No-op if the Event Log is disabled. */
    void recordEventLogGeneration(GenerationEventLogEntry entry, bool wasInternalResynth);

    // Modulated parameter values (audio thread writes, GUI reads for ghost indicators)
    struct ModulatedValues
    {
        // NaN = no modulation active → ghost hidden
        static constexpr float NONE = std::numeric_limits<float>::quiet_NaN();
        std::atomic<float> filterCutoff { NONE };
        std::atomic<float> filterResonance { NONE };
        std::atomic<float> scanPosition { NONE };
        std::atomic<float> noiseLevel { NONE };
        std::atomic<float> lfo1Rate { NONE };
        std::atomic<float> lfo1Depth { NONE };
        std::atomic<float> lfo2Rate { NONE };
        std::atomic<float> lfo2Depth { NONE };
        std::atomic<float> lfo3Rate { NONE };
        std::atomic<float> lfo3Depth { NONE };
        std::atomic<float> delayTime { NONE };
        std::atomic<float> delayFeedback { NONE };
        std::atomic<float> delayMix { NONE };
        std::atomic<float> reverbMix { NONE };
        // Drift → Osc targets (Alpha/Noise/Magnitude = effective base+offset; Axes = offset only)
        std::atomic<float> driftAlpha { NONE };
        std::atomic<float> driftAxis1 { NONE };
        std::atomic<float> driftAxis2 { NONE };
        std::atomic<float> driftAxis3 { NONE };
        std::atomic<float> drift1Depth { NONE };
        std::atomic<float> drift2Depth { NONE };
        std::atomic<float> drift3Depth { NONE };
        std::atomic<float> driftNoise { NONE };
        std::atomic<float> driftMagnitude { NONE };
        std::atomic<float> driftResynth { NONE };
    };
    ModulatedValues modulatedValues;

    // Drift regen coordination (audio thread writes, GUI reads)
    std::atomic<bool>  driftHasOscTarget { false };
    std::atomic<int>   driftRegenMode { 0 };       // 0=Manual, 1=Auto, 2-5=max N beats
    std::atomic<float> driftRegenBpm { 120.0f };   // current BPM for cooldown calc
    std::atomic<bool>  barBoundaryFlag { false };

    // ── MIDI CC Learn (public API — call from message thread only) ──────────
    struct CcMapping
    {
        juce::String                paramId;           // APVTS param ID — message thread only
        juce::RangedAudioParameter* param   = nullptr; // resolved pointer — audio thread reads this
        float                       minNorm = 0.0f;
        float                       maxNorm = 1.0f;
    };

    /** Play the cache entry the aftertouch traversal has moved to. Set by the
     *  editor beside onSnapshotRequested; called on the message thread.
     *
     *  One per cache, because installing a slot is only half done in the
     *  processor: the panel half writes the trace card, the author name, the
     *  Re-Prompt chain's own memory and the compile window the cadence gates
     *  read. Both go through the EDITOR for that reason, and both do nothing
     *  with no editor open - which is when there is nobody to tell either. */
    // Nothing comes back. A refused landing - an empty cache, a clamp that found
    // nothing, a panel that says no - simply does not sound, and the bar asks
    // again the next time the hand reaches this position. Reporting the refusal
    // so the bar could retry under a finger that never moved is what the retry
    // would cost: an install per audio block.
    std::function<void(int)> onCachePositionRequested;
    std::function<void(int)> onLroCachePositionRequested;

    void startMidiLearn(const juce::String& paramId);
    void cancelMidiLearn();
    void clearCcMapping(int cc);
    void clearAllCcMappings();
    /** Returns a copy of the mapping for the given CC (0–127). Thread-safe: acquires ccMappingLock_. */
    CcMapping getCcMappingCopy(int cc) const;
    /** Packed (seq << 32) | cc, bumped whenever an incoming mapped CC changes a param. The
     *  editor polls this so the easy-mode ENV/LFO/Drift tab follows the hardware controller.
     *  Lock-free; read seq from the high 32 bits, cc from the low 32. */
    uint64_t getMidiTouchPacked()   const noexcept { return midiTouchPacked_.load(std::memory_order_acquire); }
    bool isMidiLearnActive() const { return midiLearnActive.load(std::memory_order_relaxed); }
    /** The param waiting for a CC assignment. Message thread only. */
    const juce::String& getMidiLearnParamId() const { return midiLearnParamId; }
    /** Search all mappings for one bound to paramId. Returns CC (0–127) or -1. Message thread only. */
    int findBoundCc(const juce::String& paramId) const;

    /** Fired on the message thread whenever MIDI Learn mode changes.
     *  learning=true → learn started. learning=false, boundCc≥0 → CC was bound.
     *  learning=false, boundCc<0 → cancelled or aborted (no change to make). */
    std::function<void(bool learning, int boundCc)> onMidiLearnStateChanged;

    /** Fired on the message thread when the XL "Generate" button (CC 37) is pressed.
     *  The editor wires this to MainPanel::triggerMainGeneration. */
    std::function<void()> onGenerateRequested;

    /** Fired on the message thread when an XL snapshot button (CC 45-48) is pressed.
     *  Argument = slot 1-4. The editor wires this to MainPanel::activateSnapshot. */
    std::function<void(int slot)> onSnapshotRequested;

    /** The Snap switch fade, for MainPanel::activateSnapshot, on the message
     *  thread. snapFadeOut() fades the output to silence and returns once it is
     *  silent (or after a short wait when no audio block runs); the snap is
     *  written in that silence; snapFadeIn() fades the output back in. A
     *  snapFadeIn() with no fade pending does nothing. */
    void snapFadeOut();
    void snapFadeIn();

    /** Fired on the message thread when the XL cache button (CC 49) is pressed.
     *  The editor wires this to toggle the inference cache between 4 and Off. */
    std::function<void()> onCacheToggleRequested;

    // ── MIDI Output (LED feedback) — message thread only ────────────────────
    void openMidiOutputDevice(const juce::String& deviceId);
    void closeMidiOutputDevice();
    const juce::String& getMidiOutputDeviceId() const { return midiOutputDeviceId_; }
    /** Enable DAW mode, overwrite the XL Page-1 CC bindings with the canonical
     *  layout, and (after a short delay) light the LEDs. Authoritative for the
     *  Page-1 CC range; bindings on other CCs are left untouched. */
    void applyXLDefaultBindings();
    /** Overwrite ONLY the XL Page-1 CC bindings in ccMappings_ (no SysEx, no LED,
     *  no DAW-mode handshake) — safe to call from any thread. Used to keep the XL
     *  device bindings alive after a preset load wipes ccMappings_. */
    void populateXLDefaultBindings();

    // ── MIDI Clock Input ─────────────────────────────────────────────────────
    bool  isMidiClockActive()  const noexcept;
    float getMidiClockBpm()    const noexcept;
    bool  isMidiClockEnabled() const noexcept;
    void  setMidiClockEnabled(bool e);

    // ── Csound orchestra swap (Phase-2 spec S4) — message thread or background,
    // NEVER the audio thread ─────────────────────────────────────────────────
    // Compiles `orchestraText` into the currently-inactive Csound engine on the
    // existing D9 background-compile machinery, primes it (S3) so a held note's
    // takeover doesn't re-strike, then hands off to processBlock's crossfade
    // (S5) over the Regen XFade time (driftCrossfade). Always accepted: a
    // request arriving while another compiles or fades is queued/supersedes the
    // previous one (latest-wins, S4) — poll the status accessors below for the
    // outcome. Returns false only if the snapshot/queueing itself couldn't be
    // taken (should not happen in practice).
    bool requestCsoundOrchestra(const juce::String& orchestraText);
    /** Message thread: last compile failure text (empty = none, or the last
     *  request succeeded). */
    juce::String csoundCompileError() const;
    /** True when the ACTIVE Csound engine's performance has ended (it renders
     *  silence until something recompiles it). GUI polls this; any thread. */
    bool csoundPerformanceEnded() const;
    /** Message thread: a compiled+primed orchestra is waiting for processBlock
     *  to consume it and start the fade. */
    bool csoundSwapPending() const { return csoundSwapPending_.load(std::memory_order_acquire); }
    /** Message thread: the audio thread is actively crossfading orchestras right now. */
    bool csoundSwapFading() const { return csoundSwapFading_.load(std::memory_order_acquire); }
    /** Message thread: a background compile (bootstrap OR orchestra-swap) is
     *  running right now. Phase 5 UI polling (PromptPanel's compile-window
     *  Timer) combines this with csoundSwapPending()/csoundSwapFading()/
     *  csoundCompileError() to know when a requestCsoundOrchestra() call has
     *  fully resolved — including the "instant adopt" case (no ready active
     *  engine yet), which never sets csoundSwapPending()/csoundSwapFading() at
     *  all since there is nothing to fade from. */
    bool csoundCompileInFlight() const { return csoundCompileInFlight_.load(std::memory_order_acquire); }


private:

    void handleAsyncUpdate() override;

    // ── MIDI Output (LED feedback) ───────────────────────────────────────────
    std::unique_ptr<juce::MidiOutput> midiOutputDevice_;
    juce::String                      midiOutputDeviceId_;
    std::atomic<bool>                 dawModeActive_ { false };  // XL in DAW mode (LEDs + ch16 controls + buttons); audio thread reads it to gate button actions
    juce::SpinLock                    midiOutputLock_;

    // One-shot timer for the deferred XL LED burst (see applyXLDefaultBindings).
    // Owned + declared AFTER the midiOutput members so it is torn down (and its
    // ~Timer stops it) before them; ~T5ynthProcessor also stops it explicitly. This
    // makes the deferred burst safe even if a host destroys the plugin off the
    // message thread — unlike an un-cancellable Timer::callAfterDelay.
    struct OneShotTimer : public juce::Timer {
        std::function<void()> fn;
        void timerCallback() override { stopTimer(); if (fn) fn(); }
    };
    OneShotTimer                      xlLedTimer_;

    void sendMidiOutputMessage(const juce::MidiMessage& msg);
    void sendLearnLed(bool learning, int boundCc);
    void lightXLLeds();   // send all Page-1 LED colours (DAW mode; deferred from XL Map)

    // XL DAW-mode buttons (CC 37-52, ch1) → transport/actions. handleXLButtonPress
    // runs on the AUDIO thread (on a button press): it raises an atomic request and
    // triggerAsyncUpdate()s — the actual setValueNotifyingHost runs on the message
    // thread in handleAsyncUpdate() (it locks, so it must never run on the audio
    // thread). Panic uses the existing midiPanicRequested atomic.
    void handleXLButtonPress(int cc);
    juce::RangedAudioParameter* seqRunningParam_    = nullptr;  // resolved once (ctor)
    juce::RangedAudioParameter* genSeqRunningParam_ = nullptr;
    std::atomic<bool>           xlSeqToggleReq_     { false };  // audio→message: toggle seq_running
    std::atomic<bool>           xlSeqModeToggleReq_ { false };  // audio→message: toggle gen_seq_running (Step/Gen)
    std::atomic<bool>           xlGenerateReq_      { false };  // audio→message: trigger generation (CC 37)
    std::atomic<int>            xlRepromptStanceReq_ { -1 };    // audio→message: set reprompt_stance to index 0-6 (CC 38-44); -1 = none
    std::atomic<int>            xlSnapshotReq_      { -1 };     // audio→message: recall snapshot slot 1-4 (CC 45-48); -1 = none
    /** Resolve the Cache aftertouch target. Audio thread, once per block. Never
     *  touches the cache itself - it posts a position and lets the message
     *  thread do the loading, like every other audio→message request. */
    void updateAftertouchTraversal(const BlockParams& bp);
    /** Drop a Cache-bar position parked behind a Csound swap. NOT one already in
     *  a mailbox - see the definition. Audio thread; called where the bar
     *  re-arms. */
    void cancelParkedCachePosition();
    /** Recompute the atomic above. Message thread; called from every cache mutation. */
    void publishInferenceCacheTraversableZones();
    void publishCsoundCacheTraversableZones();

    std::atomic<juce::uint64>   atCachePosReq_      { 0 };      // audio→message: play NEURAL cache entry N (AT traversal); 0 = none
    // audio→message: install LRO cache entry N (AT traversal). Index AND the
    // serial of the press that asked for it, in one atomic so the pair cannot
    // tear. 0 = empty. See makeTraversalReq() for why the serial is needed.
    std::atomic<juce::uint64>   atLroCachePosReq_   { 0 };
    // Message thread. An LRO position that arrived while a compile or its fade
    // was still running, waiting for the swap to free up. -1 = none.
    int                         pendingLroCachePos_ { -1 };
    unsigned                    pendingLroCacheSeq_ { 0 };
    // audio→message: the serial of the last press each bar made BEFORE it was
    // disarmed (amount to zero, hands off the keyboard, idle, or the oscillator
    // changed under the held note). Everything up to and including that serial
    // belonged to a gesture that has ended.
    //
    // A serial rather than a flag, because the two threads cannot be ordered
    // usefully here: a bare flag says "something was cancelled" without saying
    // WHAT, so a press still in its mailbox when the flag went up is parked
    // afterwards and survives the cancel that was meant for it - and in the
    // language oscillator that lands a recompile a second later over an empty
    // keyboard. Comparing serials is order-free and gives the same answer
    // whenever the message thread happens to run.
    std::atomic<unsigned>       atCacheCancelSeq_   { 0 };
    // The harder kind. A gesture that merely ENDED still lets a press that can
    // land immediately land - it is a millisecond behind the finger. A press
    // resolved against the cache the player has just switched AWAY from must
    // never land at all: the index means something else now, and installing it
    // drags the engine back with it. Cancelled up to this serial means gone,
    // whatever the drain finds and whenever it runs.
    std::atomic<unsigned>       atCacheHardCancelSeq_ { 0 };
    // message→audio: a press was dropped, so the bar no longer holds what it
    // last asked for. Without this the dropped position stays claimed and the
    // finger cannot reach it again without visiting another one first.
    std::atomic<bool>           atCacheForgetActed_ { false };
    // Audio thread. Serial of the last press the bar made; only ever grows.
    unsigned                    atPostSeq_          { 0 };
    // Audio thread only. The zone the traversal target last resolved to, so a
    // pressure that has not left its zone requests nothing, and lifting the
    // amount to zero re-arms it from scratch.
    int                         atCacheZone_        { -1 };
    bool                        atCacheLro_         { false };  // which cache the claim belongs to
    // Which axis the travelling bar last rode. A change re-arms it, so swapping
    // the source cannot post a landing under a motionless finger.
    int                         atCacheSrc_         { ExprSource::None };
    unsigned                    atCacheGenSeen_     { 0 };      // its generation when this bar last resolved
    bool                        atCacheGenValid_    { false };
    // The position the bar last ASKED for - what the BAR sent, not what is
    // loaded. The zone alone is not enough to decide whether anything changed:
    // flipping a bar's sign turns the same zone into the opposite end of the
    // cache, and watching only the zone would let that reversal pass unnoticed
    // under a steady finger.
    //
    // Deliberately NOT "what is loaded". A memory of the loaded thing makes the
    // bar fight whatever else loads: a landing is a load, and a bar that
    // re-posted under a finger that has not moved would cost one sample
    // re-extract or one Csound recompile per audio block, for as long as the key
    // is down. A bar acts when the HAND moves it. Anything else that loads -
    // GENERATE, a snapshot, a preset - keeps what it loaded, and the finger
    // travels away and back to overrule it.
    //
    // Audio thread only. Reset to -1 wherever the zone is (amount to zero,
    // oscillator change), which is what re-arms the bar from scratch.
    int                         atCacheActedIdx_    { -1 };
    // Whether the pressure has been pushed past rest at all since the last time
    // every key came up. Rest is where a note STARTS, not somewhere the player
    // steered to, and a bar that acts there claims a position on the first note
    // of the session and again on every note-on after it.
    bool                        atCacheEngaged_     { false };
    // The step the bar stood on when it was last re-armed. -1 = not looked yet.
    // Engagement is "the resolved zone is no longer this one", which is the same
    // measure the zone decision already makes, hysteresis and all.
    int                         atCacheBaseZone_    { -1 };
    bool                        atAnyKeyHeld_       { false };
    std::atomic<bool>           xlCacheToggleReq_   { false };  // audio→message: toggle inference cache 4↔Off (CC 49)
    std::atomic<bool>           xlGenTimingToggleReq_ { false };// audio→message: toggle drift_regen a.s.a.p.↔4 bars (CC 50)
    std::atomic<bool>           xlAutoApplyReq_     { false };  // any→message: (re)apply XL bindings (port select / preset load)

    // ── MIDI CC Learn (internals) ────────────────────────────────────────────
    std::array<CcMapping, 128> ccMappings_;   // user CC-learns + preset mappings (serialized in presets)
    // XL DEVICE layer — a separate binding table populated from the fixed Page-1 layout ONLY
    // while a Launch Control XL output is connected, and cleared when it disconnects. It is
    // NEVER serialized, so loading a preset (which replaces ccMappings_) cannot wipe the XL
    // controls. This is the architectural separation: controller bindings are device state,
    // not preset content.
    std::array<CcMapping, 128> xlDefaults_;
    // Relative-encoder sub-step accumulator (audio-thread only; indexed by absolute CC 13-36).
    // Carries the unrealized fraction of an endless-encoder detent across messages so a param
    // with a fine interval + low-end skew (e.g. Attack at its 0 default) escapes the value snap
    // instead of sticking dead at the floor. Dropped when a value reaches a rail.
    std::array<float, 128> relEncoderAccum_ {};
    // Resolve a CC to its active binding: the XL device layer wins on the controls it owns
    // (populated only while an XL is connected), otherwise fall through to user/preset
    // bindings. Caller must hold ccMappingLock_ (the audio thread holds the try-lock).
    const CcMapping& resolveCcMapping (int cc) const noexcept
    {
        const auto& xl = xlDefaults_[static_cast<size_t>(cc)];
        return xl.param != nullptr ? xl : ccMappings_[static_cast<size_t>(cc)];
    }
    mutable juce::SpinLock     ccMappingLock_;
    std::atomic<bool>          midiLearnActive { false };
    std::atomic<int>           midiLearnTargetCc { -1 };  // audio thread writes, message thread reads

    // Easy-mode "tab follows controller": the audio thread records the last mapped
    // CC it applied; the editor's 30 Hz timer reads it (lock-free) and switches the
    // ENV/LFO/Drift tab to match. Packed as (seq << 32) | cc in ONE word so the
    // reader always gets a consistent seq/cc pair (single writer = the audio thread).
    std::atomic<uint64_t>      midiTouchPacked_ { 0 };
    juce::String               midiLearnParamId;           // message thread only

    // ── MIDI Clock Input (internals) ─────────────────────────────────────────
    // Atomics shared between audio thread (write BPM/valid) and message thread
    // (read for GUI display and resolveSyncBpm guard).
    std::atomic<bool>  midiClockEnabled_ { false };
    std::atomic<bool>  midiClockValid_   { false };
    std::atomic<float> midiClockBpm_     { 120.0f };
    // Audio-thread-only state — no concurrent access, no atomics needed:
    uint64_t           midiClockBlockStart_   = 0;     // abs sample at start of current block
    uint64_t           midiClockLastTick_     = 0;     // abs sample of last 0xF8 tick
    uint32_t           midiClockIntervals_[24] {};     // ring buffer of inter-tick intervals
    int                midiClockTickIdx_      = 0;
    int                midiClockTickCount_    = 0;     // ticks seen (caps at 24 = 1 beat)
    bool               midiClockPrevEnabled_  = false; // edge-detect re-enable on audio thread

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(T5ynthProcessor)
};
