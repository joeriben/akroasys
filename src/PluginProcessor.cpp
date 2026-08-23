#include "PluginProcessor.h"
#include <cstring>
#include "PluginEditor.h"
#include "BinaryData.h"
#include "UpdateChecker.h"
#include "dsp/Tuning.h"
#include "midi/LaunchControlXLLeds.h"
#include "presets/CalibrationMigration.h"
#include <algorithm>
#include <chrono>

#define SAMPLER_PROCESSOR_DEBUG_LOG 0

namespace
{
constexpr float kAlphaAnchorSnapThreshold = 0.04f;
// Centre/linear detent. Deliberately smaller than the ±1 anchor threshold: the
// alpha range's quadratic skew is flattest at 0, so a given value-window maps to
// a much wider pixel-window there. 0.02 gives a clearly findable "linear" detent
// without eating the fine-control band the skew exists to provide.
constexpr float kAlphaLinearSnapThreshold = 0.02f;
constexpr float kMagnitudeUnitySnapThreshold = 0.03f;
constexpr float kDurationSecondSnapThreshold = 0.05f;

// RAII around begin/endBulkParamLoad so an early return during a bulk parameter
// apply (preset import, DAW state restore) can't leave eventLogSuppressParamEvents_
// stuck on — commit() marks a real load happened; an uncommitted guard cancels
// silently (no marker), which is correct for a parse failure before anything
// was actually suppressed as well as for one after.
struct BulkParamLoadGuard
{
    explicit BulkParamLoadGuard(T5ynthProcessor& p) : proc(p) { proc.beginBulkParamLoad(); }
    ~BulkParamLoadGuard() { committed ? proc.endBulkParamLoad(name) : proc.cancelBulkParamLoad(); }
    void commit(juce::String presetName) { name = std::move(presetName); committed = true; }
    T5ynthProcessor& proc;
    juce::String name;
    bool committed = false;
};

float snapIfNear(float value, float target, float threshold)
{
    return std::abs(value - target) <= threshold ? target : value;
}

float snapToInterval(float rangeStart, float value, float interval)
{
    return rangeStart + interval * std::floor((value - rangeStart) / interval + 0.5f);
}

float snapGenerationAlpha(float rangeStart, float rangeEnd, float value)
{
    juce::ignoreUnused(rangeStart, rangeEnd);
    value = snapIfNear(value,  0.0f, kAlphaLinearSnapThreshold);  // linear (50/50) centre
    value = snapIfNear(value, -1.0f, kAlphaAnchorSnapThreshold);  // A1 anchor
    value = snapIfNear(value,  1.0f, kAlphaAnchorSnapThreshold);  // B1 anchor
    return juce::jlimit(rangeStart, rangeEnd, value);
}

// Per-target aftertouch amount param IDs in AftertouchTarget order (index by the
// enum; [0]=None unused). Shared by preset save/load + DAW migration so the old
// single-select aftertouch_target/_amount can be folded onto the right target.
const char* const kAftertouchAmtPid[AftertouchTarget::kCount] = {
    nullptr,                              // None
    PID::aftertouchAmtLfo1Depth,          // LFO1Depth
    PID::aftertouchAmtLfo2Depth,          // LFO2Depth
    PID::aftertouchAmtLfo3Depth,          // LFO3Depth
    PID::aftertouchAmtEnv1Sustain,        // Env1Sustain
    PID::aftertouchAmtEnv2Sustain,        // Env2Sustain
    PID::aftertouchAmtEnv3Sustain,        // Env3Sustain
    PID::aftertouchAmtCutoff,             // Cutoff
    PID::aftertouchAmtResonance,          // Resonance
    PID::aftertouchAmtScan,               // Scan
    PID::aftertouchAmtDca,                // DCA
    PID::aftertouchAmtPitch,              // Pitch
    PID::aftertouchAmtNoiseLevel,         // NoiseLevel
    PID::aftertouchAmtEnv4Sustain,        // Env4Sustain
    PID::aftertouchAmtEnv5Sustain,        // Env5Sustain
    PID::aftertouchAmtCache,              // Cache
    PID::aftertouchAmtSnap,               // Snap
};

// The matching expression-SOURCE param ids, same AftertouchTarget order and the
// same [0]=None hole. Kept beside the amounts because the two are read together
// everywhere: a routing is an amount AND a source.
const char* const kExprSrcPid[AftertouchTarget::kCount] = {
    nullptr,                              // None
    PID::exprSrcLfo1Depth,                // LFO1Depth
    PID::exprSrcLfo2Depth,                // LFO2Depth
    PID::exprSrcLfo3Depth,                // LFO3Depth
    PID::exprSrcEnv1Sustain,              // Env1Sustain
    PID::exprSrcEnv2Sustain,              // Env2Sustain
    PID::exprSrcEnv3Sustain,              // Env3Sustain
    PID::exprSrcCutoff,                   // Cutoff
    PID::exprSrcResonance,                // Resonance
    PID::exprSrcScan,                     // Scan
    PID::exprSrcDca,                      // DCA
    PID::exprSrcPitch,                    // Pitch
    PID::exprSrcNoiseLevel,               // NoiseLevel
    PID::exprSrcEnv4Sustain,              // Env4Sustain
    PID::exprSrcEnv5Sustain,              // Env5Sustain
    PID::exprSrcCache,                    // Cache
    PID::exprSrcSnap,                     // Snap
};

/** The authored instrument's twelve knob positions, and its three layer levels.
    Two lists, not one, because the two are owned by different people: a knob is
    the AUTHOR's — it means what the instrument says it means, and baking a new
    instrument resets it — while a level is the PLAYER's mix between the layers
    and no authoring may seize it. The preset payload writes both and the preset
    reader restores both; `setCsoundControls` walks only the knobs. Named here so
    the places that do walk both cannot fall out of step with each other. */
static constexpr const char* kLroKnobIds[] = {
    PID::lroP1a, PID::lroP1b, PID::lroP1c, PID::lroP1d,
    PID::lroP2a, PID::lroP2b, PID::lroP2c, PID::lroP2d,
    PID::lroP3a, PID::lroP3b, PID::lroP3c, PID::lroP3d };
static constexpr const char* kLroLevelIds[] = {
    PID::lroLvl1, PID::lroLvl2, PID::lroLvl3 };

/** Does any MOD envelope — ENV 2..5 — point at this target?
    Deliberately does NOT ask the amp envelope: some of the idle-gate predicates
    below include amp and some do not, and folding it in here would quietly
    change which of them fire. Ask `p.ampTarget` alongside where it belongs. */
static bool anyModEnvTargets (const BlockParams& p, int target)
{
    for (int i = 0; i < kNumModEnvs; ++i)
        if (p.modEnv[i].target == target) return true;
    return false;
}

float snapGenerationMagnitude(float rangeStart, float rangeEnd, float value)
{
    constexpr float interval = 0.001f;
    value = snapIfNear(value, 1.0f, kMagnitudeUnitySnapThreshold);
    value = snapToInterval(rangeStart, value, interval);
    return juce::jlimit(rangeStart, rangeEnd, value);
}

float snapGenerationDuration(float rangeStart, float rangeEnd, float value)
{
    constexpr float interval = 0.01f;
    // Whole-second detents across the whole range. For the default 11s slider
    // this is the historical 1..11; for the SA3 120s slider the detents simply
    // extend so round music lengths (30s, 60s, 90s) snap cleanly too. ceil()
    // keeps the 11.0 endpoint behaving exactly as before.
    const int maxSecond = static_cast<int>(std::ceil(rangeEnd));
    for (int seconds = 1; seconds <= maxSecond; ++seconds)
        value = snapIfNear(value, static_cast<float>(seconds), kDurationSecondSnapThreshold);

    value = snapToInterval(rangeStart, value, interval);
    return juce::jlimit(rangeStart, rangeEnd, value);
}

float convertSkew03From0To1(float rangeStart, float rangeEnd, float proportion)
{
    constexpr float skew = 0.3f;
    proportion = juce::jlimit(0.0f, 1.0f, proportion);
    if (proportion > 0.0f)
        proportion = std::exp(std::log(proportion) / skew);
    return rangeStart + (rangeEnd - rangeStart) * proportion;
}

float convertSkew03To0To1(float rangeStart, float rangeEnd, float value)
{
    constexpr float skew = 0.3f;
    auto proportion = juce::jlimit(0.0f, 1.0f, (value - rangeStart) / (rangeEnd - rangeStart));
    return std::pow(proportion, skew);
}

float applyNormalizedOffset(float baseValue, float modulationOffset)
{
    return juce::jlimit(0.0f, 1.0f, baseValue + modulationOffset);
}

#if SAMPLER_PROCESSOR_DEBUG_LOG
void samplerProcessorDebugLog(const juce::String& message)
{
    juce::Logger::writeToLog("[SamplerDebug] " + message);
    juce::FileOutputStream out(juce::File("/tmp/t5ynth_sampler_debug.log"));
    if (out.openedOk())
    {
        out << "[SamplerDebug] " << message << juce::newLine;
        out.flush();
    }
}
#else
// When debug logging is disabled, replace the call with a no-op so the
// expensive string-concat arguments are never evaluated (audio thread safety).
#define samplerProcessorDebugLog(...) ((void)0)
#endif

bool samePrepareConfig(const SamplePlayer::PrepareConfig& a,
                       const SamplePlayer::PrepareConfig& b)
{
    auto sameFloat = [] (float x, float y)
    {
        return std::abs(x - y) <= 1.0e-6f;
    };

    return a.loopMode == b.loopMode
        && sameFloat(a.crossfadeMs, b.crossfadeMs)
        && a.normalizeOn == b.normalizeOn
        && a.loopOptimizeLevel == b.loopOptimizeLevel
        && sameFloat(a.startPosFrac, b.startPosFrac)
        && sameFloat(a.loopStartFrac, b.loopStartFrac)
        && sameFloat(a.loopEndFrac, b.loopEndFrac);
}

juce::AudioBuffer<float> makeFreezeLoadBuffer(const juce::AudioBuffer<float>& source,
                                              double sourceRate,
                                              bool normalizeOn,
                                              float normalizeStartFrac,
                                              float normalizeEndFrac,
                                              const SamplePlayer& normalizer)
{
    juce::AudioBuffer<float> buffer;
    const int numSamples = source.getNumSamples();
    const int numChannels = source.getNumChannels();
    if (numSamples > 0 && numChannels > 0)
    {
        // Freeze renders from a mono snapshot, so normalize the exact folded
        // signal it will play instead of normalizing stereo and losing level
        // later through (L+R)/channels.
        buffer.setSize(1, numSamples, false, false, true);
        buffer.clear();

        const float invChannels = 1.0f / static_cast<float>(numChannels);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float* src = source.getReadPointer(ch);
            float* dst = buffer.getWritePointer(0);
            for (int i = 0; i < numSamples; ++i)
                dst[i] += src[i] * invChannels;
        }
    }

    if (!normalizeOn || buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        return buffer;

    const int freezeSamples = buffer.getNumSamples();
    float startFrac = juce::jlimit(0.0f, 1.0f, normalizeStartFrac);
    float endFrac = juce::jlimit(0.0f, 1.0f, normalizeEndFrac);
    if (endFrac < startFrac)
        std::swap(startFrac, endFrac);

    int normStart = juce::roundToInt(startFrac * static_cast<float>(freezeSamples));
    int normEnd = juce::roundToInt(endFrac * static_cast<float>(freezeSamples));
    normStart = juce::jlimit(0, freezeSamples, normStart);
    normEnd = juce::jlimit(normStart, freezeSamples, normEnd);
    if (normEnd <= normStart)
    {
        normStart = 0;
        normEnd = freezeSamples;
    }

    normalizer.normalizeBuffer(buffer, normStart, normEnd, sourceRate);
    return buffer;
}
}

namespace {
// Nonlinear-filter oversampling: UI quality index 0/1/2 ↔ DSP factor 1/2/4.
inline int osFactorFromQualityIndex(int idx) noexcept
{
    switch (idx) { case 2: return 4; case 1: return 2; default: return 1; }
}
inline int osQualityIndexFromFactor(int factor) noexcept
{
    switch (factor) { case 4: return 2; case 2: return 1; default: return 0; }
}

// The output gain the master stage applies, from the `limiterThresh` parameter.
//
// WHAT THIS REPLACES. Until the compressor came out of the master stage, this
// was `juce::dsp::Limiter::update()`'s own `outputVolume`:
// `10^(10*(1 - 1/4)/40) * decibelsToGain(-threshold)`, i.e. +6.75 dB of MAKEUP
// for a 4:1 compressor plus the threshold read back as gain. Carrying the
// makeup forward without the compression it was compensating for is not a
// calibration, it is a leftover -- and it was 6.75 dB of the instrument's
// loudness borrowed from a stage that was squashing every chord to pay for it.
//
// WHAT IT IS NOW: how loud the instrument is, as a function of the VOICE COUNT
// SWITCH.
//
// Setting it from one polyphony was wrong, and BJ said so: the voice count is a
// SWITCH -- seven positions on the panel, Mono to 64 -- so the circuit can read
// it. It is not a hidden dependence on how many notes happen to be sounding
// (that would be the paraphony this whole repair removed, and VoiceManager's
// 1/N^0.1 is the only thing in the instrument allowed to depend on THAT). It is
// a static function of a control the player sets deliberately, and it changes
// only when they move it.
//
// What it buys is the loudness back where it is actually missed. A mono lead no
// longer pays for headroom a sixteen-voice pad needs and it never uses.
//
// Measured (tools/measure_engine_levels), post-trim voice-chain peak with EVERY
// switch position held full. The wavetable engine is the steepest at every one
// of them -- its voices all read one spectrum, so their partials coincide far
// more often -- so it sets the table:
//
//   notes held      1      4      6      8     12     16     64
//   Wavetable   0.278  0.724  0.904  1.079  1.498  1.688  5.498   <- sets this
//   Sampler     0.278  0.606  0.703  0.828  1.051  1.151  2.707
//   Granular    0.278  0.433  0.603  0.717  0.857  1.000  2.444
//   LRO         0.278  0.499  0.613  0.697  0.936  0.958      --  (capped at 16)
//
// The gain is 0.9 / (that peak), so a chord that fills the selected polyphony
// lands exactly on the ceiling's knee: at every position the VOICE SUM is as
// loud as it can be while still passing the ceiling bit-identically.
//
// The voice sum, and not the output. Delay and reverb add up to ~2.7x on top of
// it (the gain-staging block below), and the sequencer's one-shots join after
// the voices too. A wet patch WILL reach the ceiling, and no calibration at a
// useful loudness can prevent that -- the FX gain alone is +8.6 dB. What this
// table fixes is the dry voice sum; the ceiling is what catches the rest, which
// is the job it exists for.
//
// ABOVE 16 THE LAW STOPS, and holds the 16-voice value. The switch stops
// meaning "a chord this big" there: 64 notes at once is not a hand, it is a
// sequencer or MPE texture where notes come and go, and calibrating for a
// 64-note cluster would cost 10 dB that essentially never sounds. A dense
// moment at that setting reaches the ceiling. That is the ceiling's job.
//
// The `limiterThresh` parameter still offsets the whole table, over the same
// -30..0 dB range and in the same direction (more negative is louder). Its
// DEFAULT is the reference: at -3.0 dB the table is exactly what is written
// above.
//
// NO CALIBRATION EPOCH, deliberately, and this is the place to say why because
// `voiceCount` IS stored in every preset and every DAW session, so every one of
// them changes absolute level on load -- against the pre-repair chain, Mono
// +3.4 dB, "4" -4.9, "8" -8.3, "16" -12.2. An epoch exists to keep a stored
// preset sounding as authored across a law change; here the law change IS the
// repair, and an epoch would have to undo it preset by preset. There is also no
// room to undo it in: `master_vol` is attenuate-only and `limiterThresh` reaches
// just -3 dB at its quiet end, against the 12.2 dB a 16-voice preset moved.
constexpr float kThresholdRef = -3.0f;   // == the parameter default

constexpr float kOutputGainForVoiceSwitch[] = {
    3.237f,   // Mono   0.9 / 0.278   single note at -0.9 dBFS
    1.243f,   // 4      0.9 / 0.724               -9.2 dBFS
    0.996f,   // 6      0.9 / 0.904              -11.2 dBFS
    0.834f,   // 8      0.9 / 1.079              -12.7 dBFS   (the shipped default)
    0.601f,   // 12     0.9 / 1.498              -15.5 dBFS
    0.533f,   // 16     0.9 / 1.688              -16.6 dBFS
    0.533f,   // 64     held, see above
    0.533f    // 128    hidden from the UI, same
};
static_assert(sizeof(kOutputGainForVoiceSwitch) / sizeof(kOutputGainForVoiceSwitch[0])
                  == VoiceCount::kCount,
              "one gain per voice-count switch position: adding a position to "
              "VoiceCount::kEntries without one here would zero-fill it and make "
              "that position silent, with no compile error.");

// The shipped default position. Anything that is NOT a voice -- the sequencer's
// one-shot samples -- is referred to this, so that moving a polyphony switch
// does not move the level of something the polyphony does not bound.
constexpr float kOneShotReferenceGain = 0.834f;   // == kOutputGainForVoiceSwitch[VoiceCount::V8]

inline float outputGainForThreshold(float thresholdDb, int voiceSwitchIndex) noexcept
{
    const int i = juce::jlimit(0, VoiceCount::kCount - 1, voiceSwitchIndex);
    return kOutputGainForVoiceSwitch[i]
         * juce::Decibels::decibelsToGain(kThresholdRef - thresholdDb, -100.0f);
}
} // namespace

T5ynthProcessor::T5ynthProcessor()
    : AudioProcessor(BusesProperties()
                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                     .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "T5ynth", createParameterLayout())
{
    paramCache.init(parameters);

    // Load the global (machine-wide) nonlinear-filter oversampling quality from
    // the settings store into the audio-thread atomic. Default index 1 = 2×.
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "T5ynth";
        opts.filenameSuffix      = "settings";
        opts.folderName          = "T5ynth";
        opts.osxLibrarySubFolder = "Application Support";
        appProperties_.setStorageParameters(opts);

        const int qualityIdx = appProperties_.getUserSettings()->getIntValue("filterOsQuality", 1);
        filterOsFactor_.store(osFactorFromQualityIndex(qualityIdx), std::memory_order_relaxed);

        // LRO (Csound) oversampling — its own setting, its own default (index 2 = 4×).
        const int lroIdx = appProperties_.getUserSettings()->getIntValue("lroOsQuality", 2);
        lroOsFactor_.store(osFactorFromQualityIndex(lroIdx), std::memory_order_relaxed);

        // LRO AUTHOR provider (external API alternative to the local GGUF) —
        // load once here so the very first translate/interpret/csound call
        // already knows about it, not just calls after the Settings UI is opened.
        pipeInference->setAuthorProviderConfig(getLroAuthorProviderConfig());
    }

    // Event Log (.t5evt): build the paramID<->index tables once (index is what
    // crosses the audio-thread-safe FIFO; the ID string only gets resolved back
    // on the writer thread) and register one listener for every parameter. The
    // writer thread itself always starts — recording is gated by
    // eventLogEnabled_ at every push site, not by the thread's lifecycle, so
    // toggling the Settings switch mid-session needs no thread restart.
    {
        std::vector<juce::String> paramIdByIndex;
        for (auto* p : getParameters())
        {
            auto* rap = dynamic_cast<juce::RangedAudioParameter*>(p);
            if (rap == nullptr)
                continue;   // every APVTS-created parameter in this codebase is one; defensive only
            const auto id = rap->getParameterID();
            eventLogParamIndexById_[id] = static_cast<int>(paramIdByIndex.size());
            paramIdByIndex.push_back(id);
            parameters.addParameterListener(id, this);
        }

       #if defined(T5YNTH_FULL_VERSION)
        const juce::String eventLogVersion { T5YNTH_FULL_VERSION };
       #else
        const juce::String eventLogVersion { ProjectInfo::versionString };
       #endif
        EventLogHeader header;
        header.t5ynthVersion = eventLogVersion;
        header.calibEpoch    = Calibration::kEpoch;   // what this tape's values mean
        const auto eventLogDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                      .getChildFile("Library/T5ynth/eventlogs");
        eventLogWriter_ = std::make_unique<EventLogWriterThread>(eventLogDir, header, std::move(paramIdByIndex));
        // The writer owns its ingress FIFOs and drains itself — it never reaches
        // back into this processor, so recording is editor-independent and cannot
        // touch processor memory at shutdown. The audio-thread taps only call its
        // lock-free pushNote()/pushParam().
        eventLogWriter_->startThread(juce::Thread::Priority::background);

        eventLogEnabled_.store(appProperties_.getUserSettings()->getBoolValue("eventLogEnabled", false),
                               std::memory_order_relaxed);
    }

    // Kick off the (opt-out, throttled) background update check. Its own thread;
    // does not touch appProperties_ again after this call returns, so it cannot
    // race the audio thread or delay Python backend / model loading below.
    startUpdateCheckIfDue();

    // Cache the transport params the XL DAW-mode buttons toggle (set from the audio
    // thread via setValueNotifyingHost — same mechanism as the CC binding apply).
    seqRunningParam_    = parameters.getParameter(PID::seqRunning);
    genSeqRunningParam_ = parameters.getParameter(PID::genSeqRunning);

    juce::File("/tmp/t5ynth_sampler_debug.log").deleteFile();
    samplerProcessorDebugLog("session start");
    stepSequencer.setOneShotTriggerCallback(
        [this](const T5ynthStepSequencer::OneShotTrigger& trigger)
        {
            queueSequencerOneShotTrigger(trigger);
        });
    samplerReprepareThread = std::thread([this] { samplerReprepareThreadMain(); });
}

void T5ynthProcessor::setFilterOsQuality(int qualityIndex)
{
    qualityIndex = juce::jlimit(0, 2, qualityIndex);
    filterOsFactor_.store(osFactorFromQualityIndex(qualityIndex), std::memory_order_relaxed);
    if (auto* s = appProperties_.getUserSettings())
    {
        s->setValue("filterOsQuality", qualityIndex);
        s->saveIfNeeded();
    }
}

int T5ynthProcessor::getFilterOsQuality() const
{
    return osQualityIndexFromFactor(filterOsFactor_.load(std::memory_order_relaxed));
}

void T5ynthProcessor::setLroOsQuality(int qualityIndex)
{
    qualityIndex = juce::jlimit(0, 2, qualityIndex);
    const int factor = osFactorFromQualityIndex(qualityIndex);
    if (factor == lroOsFactor_.load(std::memory_order_relaxed))
        return;                       // no-op change: never pay for a recompile

    lroOsFactor_.store(factor, std::memory_order_relaxed);
    if (auto* s = appProperties_.getUserSettings())
    {
        s->setValue("lroOsQuality", qualityIndex);
        s->saveIfNeeded();
    }

    // Unlike filterOsQuality, this cannot take effect on the next block: the
    // factor IS the compiled orchestra's sr, so it needs a recompile. Deliberately
    // NOT started here — handleAsyncUpdate's reconcile block owns that, because
    // deciding it here would mean testing isReady() at this instant and silently
    // dropping the change whenever a compile or prepareToPlay happens to be in
    // flight. Just poke the async pass; it compares compiled-vs-wanted itself and
    // keeps doing so until they agree.
    triggerAsyncUpdate();
}

int T5ynthProcessor::getLroOsQuality() const
{
    return osQualityIndexFromFactor(lroOsFactor_.load(std::memory_order_relaxed));
}

void T5ynthProcessor::setLroAuthorProviderConfig(const PipeInference::AuthorProviderConfig& config)
{
    if (auto* s = appProperties_.getUserSettings())
    {
        s->setValue("lroAuthorProvider", config.provider);
        s->setValue("lroAuthorApiBase", config.apiBase);
        s->setValue("lroAuthorApiModel", config.apiModel);
        s->setValue("lroAuthorApiKey", config.apiKey);
        s->saveIfNeeded();
    }
    // Forward immediately: PromptPanel's background thread reads whatever
    // PipeInference last got told, not this settings file, so a live edit in
    // Settings would otherwise not take effect until the next launch.
    pipeInference->setAuthorProviderConfig(config);
}

PipeInference::AuthorProviderConfig T5ynthProcessor::getLroAuthorProviderConfig()
{
    PipeInference::AuthorProviderConfig config;
    if (auto* s = appProperties_.getUserSettings())
    {
        config.provider = s->getValue("lroAuthorProvider", "");
        config.apiBase  = s->getValue("lroAuthorApiBase", "");
        config.apiModel = s->getValue("lroAuthorApiModel", "");
        config.apiKey   = s->getValue("lroAuthorApiKey", "");
    }
    return config;
}

void T5ynthProcessor::startUpdateCheckIfDue()
{
    auto* s = appProperties_.getUserSettings();
    if (s == nullptr || ! s->getBoolValue("checkForUpdatesEnabled", true))
        return;

    const juce::int64 lastCheckSec = s->getValue("lastUpdateCheckEpochSec", "0").getLargeIntValue();
    const juce::int64 nowSec = juce::Time::getCurrentTime().toMilliseconds() / 1000;
    constexpr juce::int64 kMinIntervalSec = 24 * 60 * 60;
    if (nowSec - lastCheckSec < kMinIntervalSec)
        return;

    s->setValue("lastUpdateCheckEpochSec", juce::String(nowSec));
    s->saveIfNeeded();

    // Use the FULL tag (incl. -beta.N), not the JUCE-stripped X.Y.Z — otherwise
    // every beta→beta bump (how this project actually releases) is invisible.
   #if defined(T5YNTH_FULL_VERSION)
    const juce::String selfVersion { T5YNTH_FULL_VERSION };
   #else
    const juce::String selfVersion { ProjectInfo::versionString };
   #endif
    updateChecker_ = std::make_unique<UpdateChecker>(selfVersion);
    // Captures updateState_ by value (shared_ptr), never `this` — see the
    // UpdateState comment in PluginProcessor.h for why that matters.
    updateChecker_->onUpdateAvailable = [state = updateState_](juce::String version, juce::String url)
    {
        const juce::ScopedLock sl(state->lock);
        state->version = std::move(version);
        state->url = std::move(url);
        state->consumed = false;
    };
    updateChecker_->startThread(juce::Thread::Priority::background);
}

void T5ynthProcessor::setCheckForUpdatesEnabled(bool enabled)
{
    if (auto* s = appProperties_.getUserSettings())
    {
        s->setValue("checkForUpdatesEnabled", enabled);
        s->saveIfNeeded();
    }
}

bool T5ynthProcessor::getCheckForUpdatesEnabled() const
{
    if (auto* s = const_cast<juce::ApplicationProperties&>(appProperties_).getUserSettings())
        return s->getBoolValue("checkForUpdatesEnabled", true);
    return true;
}

bool T5ynthProcessor::takeAvailableUpdate(juce::String& versionOut, juce::String& urlOut)
{
    const juce::ScopedLock sl(updateState_->lock);
    if (updateState_->consumed)
        return false;
    versionOut = updateState_->version;
    urlOut = updateState_->url;
    updateState_->consumed = true;
    return true;
}

T5ynthProcessor::~T5ynthProcessor()
{
    // Unregister first, before anything else in this destructor runs — a
    // parameter change landing on `this` between here and `parameters`'s own
    // destruction would otherwise call parameterChanged() on a half-torn-down object.
    for (const auto& kv : eventLogParamIndexById_)
        parameters.removeParameterListener(kv.first, this);

    // Cancel any pending deferred LED burst + AsyncUpdater callback before members
    // are destroyed — both must run while all members are still alive.
    xlLedTimer_.stopTimer();
    replayTimer_.stopTimer();   // its callback reaches back into `this`
    cancelPendingUpdate();

    // Join the update-check thread before it (or its members) go away. Its result
    // callback only holds a shared_ptr to updateState_, never `this`, so even a
    // callAsync that was already queued before stopThread() joins is harmless —
    // it just writes into a still-live, otherwise-unread UpdateState.
    if (updateChecker_)
        updateChecker_->stopThread(4000);

    // The writer thread is fully self-contained (owns its FIFOs + file), so a clean
    // join just deletes it. On the pathological path where stopThread(4000) times
    // out on stalled disk I/O and cannot join, we LEAK the object (release, not
    // delete) rather than free memory a still-live thread is using: it only ever
    // touches its own members, so a one-time small leak at process exit is strictly
    // safer than a use-after-free. (This is the only place that can happen; a
    // normal shutdown always joins well within 4 s for these tiny writes.)
    if (eventLogWriter_)
    {
        if (eventLogWriter_->stopThread(4000))
            eventLogWriter_.reset();
        else
            eventLogWriter_.release();   // deliberate leak — see above
    }

    closeMidiOutputDevice();

    samplerReprepareThreadShouldExit.store(true, std::memory_order_release);
    if (samplerReprepareThread.joinable())
        samplerReprepareThread.join();

    // D9 (extended Phase-2 S8): join any in-flight background Csound compile
    // — whichever kind, a D9 bootstrap or a Phase-2 orchestra-swap compile,
    // both share this one thread handle — before csoundEngines_ (members,
    // destroyed after this body returns) go away while that thread might
    // still be calling into one of them. The thread handle is moved out UNDER
    // the lifecycle mutex, but join() runs WITHOUT it: the compile thread
    // itself acquires csoundLifecycleMutex_ for its work, so joining while
    // holding it deadlocks if the thread was created but has not yet reached
    // its lock (adversarial-review finding).
    // cancelPendingUpdate() above already prevents a NEW compile from being
    // launched past this point.
    {
        std::thread toJoin;
        {
            std::lock_guard<std::mutex> csoundLock(csoundLifecycleMutex_);
            toJoin = std::move(csoundCompileThread_);
        }
        if (toJoin.joinable())
            toJoin.join();
    }
}

bool T5ynthProcessor::launchPipeInference(const juce::File& backendDir)
{
    return pipeInference->launch(backendDir);
}

bool T5ynthProcessor::canUseStepHoldPreview() const
{
    // Always available now: VoiceManager reserves the drone voice so the
    // mouse-held step is protected from seq voice-stealing (poly) and
    // suppresses seq noteOns on voice 0 (mono) for as long as the mouse is held.
    return true;
}

void T5ynthProcessor::beginStepHoldPreview(int midiNote, float velocity)
{
    const juce::ScopedLock sl(getCallbackLock());

    // Apply the same seq-wide octave shift the step sequencer adds to its
    // emitted MIDI (see PluginProcessor.cpp:1060 + StepSequencer.cpp:208).
    // The drone is the GUI mirror of the step's effective pitch, so it must
    // include this shift; the per-voice oscOctave is applied later inside
    // SynthVoice::noteOn via octaveShift_ from BlockParams.
    const int seqOctaveIdx = static_cast<int>(paramCache.seqOctave->load());
    const int seqOctaveSemi = (seqOctaveIdx - 2) * 12;
    const int note = juce::jlimit(0, 127, midiNote + seqOctaveSemi);
    const float vel = juce::jlimit(0.0f, 1.0f, velocity);
    const bool lfo1TrigMode = static_cast<int>(paramCache.lfo1Mode->load()) == 1;
    const bool lfo2TrigMode = static_cast<int>(paramCache.lfo2Mode->load()) == 1;
    const bool lfo3TrigMode = static_cast<int>(paramCache.lfo3Mode->load()) == 1;

    voiceManager.setDroneNote(note, vel, lfo1TrigMode, lfo2TrigMode, lfo3TrigMode);

    stepHoldPreviewActive = true;
    stepHoldPreviewNote = note;
    lastMidiNote.store(note, std::memory_order_relaxed);
    lastMidiVelocity.store(juce::roundToInt(vel * 127.0f), std::memory_order_relaxed);
    lastMidiNoteOn.store(true, std::memory_order_relaxed);
}

void T5ynthProcessor::updateStepHoldPreview(int midiNote, float velocity)
{
    beginStepHoldPreview(midiNote, velocity);
}

void T5ynthProcessor::endStepHoldPreview()
{
    const juce::ScopedLock sl(getCallbackLock());

    if (!stepHoldPreviewActive)
        return;

    voiceManager.clearDroneNote();
    stepHoldPreviewActive = false;
    stepHoldPreviewNote = -1;

    if (!voiceManager.hasActiveVoices())
        lastMidiNoteOn.store(false, std::memory_order_relaxed);
}

// Voice source id for notes played on the computer keyboard. Distinct from
// external MIDI (-1) so the two can be released independently.
static constexpr int kComputerKeyboardSourceId = VoiceManager::kComputerKeyboardSourceId;

void T5ynthProcessor::beginComputerKeyboardNote(int midiNote, float velocity, bool isKeystroke)
{
    // Computer-keyboard notes bypass the MIDI buffer (direct voiceManager call), so
    // the replay transport's midiMessages.clear() cannot neutralise them — gate here
    // or they'd play on top of the tape.
    if (isReplayActive())
        return;

    const juce::ScopedLock sl(getCallbackLock());

    const int note = juce::jlimit(0, 127, midiNote);
    const float vel = juce::jlimit(0.0f, 1.0f, velocity);

    // The arpeggiator tracks held keys unconditionally (see Arpeggiator.h): this
    // is what lets the computer keyboard drive the arp at all — it reads the MIDI
    // buffer, which these notes never enter — and what lets switching the arp on
    // mid-hold pick up keys that are already down.
    arpeggiator.noteOn(note, vel, kComputerKeyboardSourceId);

    // With the arp on, the arp's notes sound and the raw key does not, exactly as
    // for an external keyboard. The arp emits them from processBlock.
    if (static_cast<int>(paramCache.arpMode->load()) <= 0)
    {
        const bool lfo1TrigMode = static_cast<int>(paramCache.lfo1Mode->load()) == 1;
        const bool lfo2TrigMode = static_cast<int>(paramCache.lfo2Mode->load()) == 1;
        const bool lfo3TrigMode = static_cast<int>(paramCache.lfo3Mode->load()) == 1;

        voiceManager.noteOn(note, vel, false, 0.0f,
                            lfo1TrigMode, lfo2TrigMode, lfo3TrigMode,
                            kComputerKeyboardSourceId, 0.0f);
    }

    lastMidiNote.store(note, std::memory_order_relaxed);
    lastMidiVelocity.store(juce::roundToInt(vel * 127.0f), std::memory_order_relaxed);
    lastMidiNoteOn.store(true, std::memory_order_relaxed);

    // Step-record: capture this played note into the current step (self-gates
    // on stepRecordArmed; runs on the message thread under the callback lock).
    // Only a real keystroke advances the pattern — an octave key moving a note the
    // player is still holding is the same note, not a new one, and writing a step
    // for it would fill the pattern with notes nobody played.
    if (isKeystroke)
        recordStepNote(note, vel);
}

void T5ynthProcessor::endComputerKeyboardNote(int midiNote)
{
    const juce::ScopedLock sl(getCallbackLock());

    const int note = juce::jlimit(0, 127, midiNote);
    arpeggiator.noteOff(note);
    // Unconditional: a no-op unless this key really started a direct voice (arp
    // off when it went down, or switched off while it was held).
    voiceManager.noteOff(note, kComputerKeyboardSourceId);
    if (!voiceManager.hasActiveVoices())
        lastMidiNoteOn.store(false, std::memory_order_relaxed);
}

void T5ynthProcessor::allComputerKeyboardNotesOff()
{
    const juce::ScopedLock sl(getCallbackLock());

    arpeggiator.allKeysUp();
    for (int note = 0; note < 128; ++note)
        voiceManager.noteOff(note, kComputerKeyboardSourceId);
    if (!voiceManager.hasActiveVoices())
        lastMidiNoteOn.store(false, std::memory_order_relaxed);
}

void T5ynthProcessor::toggleStepRecord()
{
    const bool nowArmed = ! stepRecordArmed.load(std::memory_order_relaxed);
    if (nowArmed)
    {
        // Failsafe: every (re-)arm starts at step 1 and discards any stale
        // candidates an audio block may have queued during the last disarm.
        // Discard consumer-side (this runs on the message thread, the FIFO's
        // only consumer) — never reset() concurrently with the audio producer.
        stepRecordCursor.store(0, std::memory_order_relaxed);
        int s1, sz1, s2, sz2;
        stepRecFifo.prepareToRead(stepRecFifo.getNumReady(), s1, sz1, s2, sz2);
        stepRecFifo.finishedRead(sz1 + sz2);
    }
    stepRecordArmed.store(nowArmed, std::memory_order_relaxed);
}

void T5ynthProcessor::recordStepNote(int playedNote, float velocity)
{
    // Message thread only (computer-keyboard note path + drainStepRecordQueue).
    if (! stepRecordArmed.load(std::memory_order_relaxed))
        return;
    const int cursor = stepRecordCursor.load(std::memory_order_relaxed);
    if (cursor < 0 || cursor >= stepSequencer.getNumSteps())
        return;   // pattern full: ignore further notes, stay armed until toggled off

    // WYSIWYG: playback re-applies the seq-wide octave shift to step.note, so
    // store the played note MINUS that shift — the recorded pitch then sounds
    // identical on playback (mirrors beginStepHoldPreview's inverse).
    const int seqOctaveIdx  = static_cast<int>(paramCache.seqOctave->load());
    const int seqOctaveSemi = (seqOctaveIdx - 2) * 12;
    const int stored = juce::jlimit(0, 127, playedNote - seqOctaveSemi);

    stepSequencer.setStepNote(cursor, stored);
    stepSequencer.setStepVelocity(cursor, juce::jlimit(0.0f, 1.0f, velocity));
    stepSequencer.setStepEnabled(cursor, true);
    stepRecordCursor.store(cursor + 1, std::memory_order_relaxed);
}

void T5ynthProcessor::recordStepRest()
{
    // Message thread (Space key + sustain-pedal rest). Leave the current step
    // empty (disabled) and advance the cursor — a gap in the pattern.
    if (! stepRecordArmed.load(std::memory_order_relaxed))
        return;
    const int cursor = stepRecordCursor.load(std::memory_order_relaxed);
    if (cursor < 0 || cursor >= stepSequencer.getNumSteps())
        return;   // pattern full: ignore, stay armed until toggled off
    stepSequencer.setStepEnabled(cursor, false);
    stepRecordCursor.store(cursor + 1, std::memory_order_relaxed);
}

void T5ynthProcessor::pushStepRecordCandidate(int note, float velocity)
{
    // Audio thread (external MIDI note-on). Lock-free, no allocation.
    int s1, sz1, s2, sz2;
    stepRecFifo.prepareToWrite(1, s1, sz1, s2, sz2);
    if (sz1 > 0)
    {
        stepRecQueue[static_cast<size_t>(s1)] = { note, velocity };
        stepRecFifo.finishedWrite(1);
    }
}

void T5ynthProcessor::drainStepRecordQueue()
{
    // Message thread (SequencerPanel timer). Drains MIDI-played notes into the
    // step grid in arrival order.
    const int ready = stepRecFifo.getNumReady();
    if (ready <= 0)
        return;
    int s1, sz1, s2, sz2;
    stepRecFifo.prepareToRead(ready, s1, sz1, s2, sz2);
    auto apply = [this](const StepRecCandidate& c)
    {
        if (c.note < 0) recordStepRest();              // sentinel: empty step (rest)
        else            recordStepNote(c.note, c.velocity);
    };
    for (int i = 0; i < sz1; ++i) apply(stepRecQueue[static_cast<size_t>(s1 + i)]);
    for (int i = 0; i < sz2; ++i) apply(stepRecQueue[static_cast<size_t>(s2 + i)]);
    stepRecFifo.finishedRead(sz1 + sz2);
}

// ── Event Log (.t5evt) ───────────────────────────────────────────────────────

void T5ynthProcessor::logInternalNoteEventsFrom(size_t startIndex, NoteEventLogEntry::Source source,
                                                bool skipLeadForArp, bool genModeForSkip)
{
    // Audio thread. internalNoteEvents_[startIndex..) is exactly what the
    // just-returned producer call appended (GenSeq/StepSeq/Arp all append their
    // own notes with a real sampleOffset), so no other tap can have touched this
    // range yet — but the seq-drives-arp lead-extraction erase() (later in this
    // same processBlock) still WILL touch it if skipLeadForArp is set, so those
    // entries are excluded here rather than logged and then also logged again
    // as the arp notes derived from them.
    for (size_t i = startIndex; i < internalNoteEvents_.size(); ++i)
    {
        const auto& ev = internalNoteEvents_[i];
        if (skipLeadForArp && (genModeForSkip ? (ev.strandId == 0) : (ev.strandId < 0)))
            continue;
        NoteEventLogEntry e;
        e.timestamp   = eventLogBlockStart_ + static_cast<uint64_t>(juce::jmax(0, ev.sampleOffset));
        e.source      = source;
        e.type        = ev.type;
        e.note        = ev.note;
        e.velocity    = ev.velocity;
        e.artic       = ev.artic;
        e.strandId    = ev.strandId;
        e.pan         = ev.pan;
        e.midiChannel = 0;

        // eventLogWriter_ is created in the ctor and only reset in the dtor, both
        // while the audio thread is stopped, so it is always valid here.
        eventLogWriter_->pushNote(e);
        eventLogRealEventLogged_.store(true, std::memory_order_relaxed);   // freeze the start-state
    }
}

void T5ynthProcessor::logExternalNoteEvent(bool noteOn, int note, float velocity, int channel,
                                           int sampleOffsetInBlock)
{
    // Audio thread.
    NoteEventLogEntry e;
    e.timestamp   = eventLogBlockStart_ + static_cast<uint64_t>(juce::jmax(0, sampleOffsetInBlock));
    e.source      = NoteEventLogEntry::Source::ExternalMidi;
    e.type        = noteOn ? VoiceEvent::Type::NoteOn : VoiceEvent::Type::NoteOff;
    e.note        = note;
    e.velocity    = velocity;
    e.artic       = VoiceEvent::Articulation::Normal;
    e.strandId    = -1;
    e.pan         = 0.0f;
    e.midiChannel = channel;

    eventLogWriter_->pushNote(e);
    eventLogRealEventLogged_.store(true, std::memory_order_relaxed);   // freeze the start-state
}

void T5ynthProcessor::parameterChanged(const juce::String& parameterID, float newValue)
{
    // Csound engine (Phase-1 spec D9), live engine_mode switch: this callback
    // can run on the AUDIO thread (see the eventLog comment just below — the
    // same hazard applies to every parameter, engine_mode included, if it's
    // ever MIDI-CC-Learn-bound), so launching a std::thread here would be an
    // audio-thread-safety violation (allocation + kernel thread creation).
    // Just flag + triggerAsyncUpdate — both RT-safe — and defer the actual
    // decision (already ready? already compiling? still even Csound mode by
    // the time this runs?) to handleAsyncUpdate on the message thread. Placed
    // BEFORE the eventLog early-return below: engine switching must work
    // whether or not the event log is recording.
    if (parameterID == PID::engineMode
        && juce::roundToInt(newValue) == static_cast<int>(EngineMode::Csound))
    {
        csoundWantsPrepare_.store(true, std::memory_order_release);
        triggerAsyncUpdate();
    }

    // The player granting or withdrawing the author's authority, and the
    // oscillator the authored orchestra plays on. "Off: they stay yours" is a
    // promise of the INSTRUMENT, so it is kept here and not in the LRO panel:
    // sited on the button it would hold only while that window happens to be
    // open, and not at all for host automation or a MIDI-learned controller.
    // Both directions, because the switch is an A/B: off hands the knobs back,
    // on takes them again. Same shape as above — flag + triggerAsyncUpdate,
    // because this can arrive on the audio thread and reconciling writes
    // parameters. The flag carries no value: handleAsyncUpdate reads where the
    // switch and the engine actually stand, so a fast double-flip cannot land
    // on the wrong one of two queued edges.
    if (parameterID == PID::lcoSetsParams || parameterID == PID::engineMode)
    {
        authorReconcileWanted_.store(true, std::memory_order_release);
        triggerAsyncUpdate();
    }

    // Can run on the audio thread (confirmed: MIDI-CC-Learn-bound params call
    // setValueNotifyingHost from inside processBlock) or the message thread —
    // never assume which. No allocation, no lock beyond the lock-free FIFO.
    if (! eventLogRecordingActive())
        return;
    if (eventLogSuppressParamEvents_.load(std::memory_order_relaxed))
        return;

    const auto it = eventLogParamIndexById_.find(parameterID);
    if (it == eventLogParamIndexById_.end())
        return;

    // "Last writer" hint, mirrors midiTouchPacked_: set immediately before a
    // known-origin setValueNotifyingHost call, consumed here. No hint present
    // (-1) defaults to HostAutomation — the "not one of our own recognized paths"
    // bucket, which covers real host automation and (for now, see Phase 1
    // report) plain GUI edits alike.
    const int originHint = eventLogOriginHint_.exchange(-1, std::memory_order_relaxed);

    ParamEventLogEntry e;
    e.timestamp  = eventLogTotalSamples_.load(std::memory_order_relaxed);
    e.paramIndex = it->second;
    e.value      = newValue;
    e.origin     = (originHint == static_cast<int>(ParamOrigin::MidiCCLearn))
                 ? ParamOrigin::MidiCCLearn : ParamOrigin::HostAutomation;

    // pushParam is multi-producer-safe (try-lock inside) and audio-thread-safe.
    // Guarded because parameterChanged could in principle fire before the writer
    // is constructed; in practice eventLogEnabled_ (checked above) is only set
    // true after construction, so this is belt-and-suspenders.
    if (eventLogWriter_)
    {
        eventLogWriter_->pushParam(e);
        eventLogRealEventLogged_.store(true, std::memory_order_relaxed);   // freeze the start-state
    }
}

void T5ynthProcessor::beginBulkParamLoad()
{
    // Message thread (PresetFormat::loadFromFile, around parameters.replaceState).
    eventLogSuppressParamEvents_.store(true, std::memory_order_relaxed);
}

void T5ynthProcessor::endBulkParamLoad(const juce::String& presetName)
{
    eventLogSuppressParamEvents_.store(false, std::memory_order_relaxed);
    if (! eventLogRecordingActive() || eventLogWriter_ == nullptr)
        return;

    // A replay's own start/stop restore is not a session event — no start-state
    // capture, no preset marker into the live log. (See eventLogInReplayRestore_.)
    if (eventLogInReplayRestore_)
        return;

    // A preset/state load before the first musical event IS this session's start
    // patch — re-snapshot it so the .t5evt header carries the patch actually in
    // effect. This is the load that matters in the sticky-enabled case: the ctor
    // runs before standalone's _buffer.t5p (or a DAW's project state) is restored,
    // so capturing at construction would freeze the init patch instead.
    captureEventLogStartStateIfPending();

    PresetLoadedLogEntry e;
    e.timestamp  = eventLogTotalSamples_.load(std::memory_order_relaxed);
    e.presetName = presetName;
    eventLogWriter_->enqueue(e);
}

void T5ynthProcessor::captureEventLogStartStateIfPending()
{
    // Message thread only. Captures the current APVTS patch as the replay start
    // state, but only until the first real event is logged — after that, the
    // running patch no longer equals the tape's t=0 patch. getStateInformation
    // reads live APVTS, so every hook that calls this (enable toggle, preset/state
    // load, prepareToPlay) captures whatever is loaded at that moment; the last one
    // before the first event wins.
    if (eventLogWriter_ == nullptr
        || ! eventLogEnabled_.load(std::memory_order_relaxed)
        || replayModeActive_.load(std::memory_order_relaxed)
        || eventLogRealEventLogged_.load(std::memory_order_relaxed))
        return;

    juce::MemoryBlock stateBlock;
    getStateInformation(stateBlock);
    eventLogWriter_->setStartState(
        juce::Base64::toBase64(stateBlock.getData(), stateBlock.getSize()));
}

void T5ynthProcessor::cancelBulkParamLoad()
{
    eventLogSuppressParamEvents_.store(false, std::memory_order_relaxed);
}

void T5ynthProcessor::recordEventLogGeneration(GenerationEventLogEntry entry, bool wasInternalResynth)
{
    if (! eventLogRecordingActive() || eventLogWriter_ == nullptr)
        return;

    const uint64_t id = eventLogNextGenerationId_.fetch_add(1, std::memory_order_relaxed);
    entry.timestamp    = eventLogTotalSamples_.load(std::memory_order_relaxed);
    entry.generationId = id;
    entry.parentGenerationId = wasInternalResynth
                             ? eventLogLastGenerationId_.load(std::memory_order_relaxed) : 0;

    eventLogWriter_->enqueue(entry);
    eventLogLastGenerationId_.store(id, std::memory_order_relaxed);
    eventLogRealEventLogged_.store(true, std::memory_order_relaxed);   // freeze the start-state
}

void T5ynthProcessor::setEventLogEnabled(bool enabled)
{
    eventLogEnabled_.store(enabled, std::memory_order_relaxed);
    if (auto* s = appProperties_.getUserSettings())
    {
        s->setValue("eventLogEnabled", enabled);
        s->saveIfNeeded();
    }

    // R0: capture the current patch as the replay start state (self-contained
    // .t5evt). On a runtime toggle this grabs exactly what the user is looking at.
    if (enabled)
        captureEventLogStartStateIfPending();
}

bool T5ynthProcessor::getEventLogEnabled() const
{
    return eventLogEnabled_.load(std::memory_order_relaxed);
}

// ── R2: Replay Transport ──────────────────────────────────────────────────────

bool T5ynthProcessor::startReplay(const EventLogReader& reader)
{
    // Message thread only.
    //
    // Decode and VALIDATE the tape's start patch before touching anything. A tape
    // whose start-state is missing or corrupt would otherwise play its notes against
    // whatever patch happens to be loaded — right notes, wrong sound — and, worse,
    // feed a garbage blob into setStateInformation.
    juce::MemoryOutputStream startStateBytes;
    {
        const auto& startState = reader.getHeader().startStateBase64;
        if (startState.isEmpty() || ! juce::Base64::convertFromBase64(startStateBytes, startState))
            return false;

        std::unique_ptr<juce::XmlElement> probe(
            getXmlFromBinary(startStateBytes.getData(), static_cast<int>(startStateBytes.getDataSize())));
        if (probe == nullptr || ! probe->hasTagName(parameters.state.getType()))
            return false;
    }

    // Stopping first also restores the user's patch, so the snapshot taken below is
    // theirs and not the outgoing tape's.
    if (isReplayActive())
        stopReplay();

    // Play is non-destructive: remember the patch we are about to overwrite, plus the
    // three transport params setStateInformation insists on zeroing.
    getStateInformation(preReplayPatch_);
    preReplaySeqRunning_     = parameters.getRawParameterValue(PID::seqRunning)->load();
    preReplayGenSeqRunning_  = parameters.getRawParameterValue(PID::genSeqRunning)->load();
    preReplayRepromptStance_ = parameters.getRawParameterValue(PID::repromptStance)->load();
    preReplayDcoStance_      = parameters.getRawParameterValue(PID::dcoRepromptStance)->load();

    // Build the state off to the side (allocations, string decode) before it is
    // published — nothing here is visible to the audio thread yet.
    ReplayState state;
    state.noteEvents         = reader.getNoteEvents();
    state.paramEvents        = reader.getParamEvents();
    state.generationEvents   = reader.getGenerationEvents();
    state.sampleRate         = reader.getHeader().sampleRate;

    // ── Rebase and rescale the timeline ──────────────────────────────────────
    // Logged timestamps are absolute samples on the recorder's free-running clock,
    // which starts at plugin CONSTRUCTION, not at record-enable — a log whose first
    // event sits at t=80412160 would otherwise replay 28 minutes of silence before
    // the first note. Subtract the earliest event so the tape starts at zero.
    //
    // They are also counted in the RECORDING device's samples. Replaying a 48 kHz
    // log on a 44.1 kHz device without rescaling would play the whole performance
    // ~8.8 % slow, so convert into the current device's sample domain.
    uint64_t base = std::numeric_limits<uint64_t>::max();
    if (! state.noteEvents.empty())       base = std::min(base, state.noteEvents.front().timestamp);
    if (! state.paramEvents.empty())      base = std::min(base, state.paramEvents.front().timestamp);
    if (! state.generationEvents.empty()) base = std::min(base, state.generationEvents.front().timestamp);
    if (base == std::numeric_limits<uint64_t>::max())
        base = 0;   // empty tape

    const double logSR    = state.sampleRate > 0.0 ? state.sampleRate : 44100.0;
    const double deviceSR = getSampleRate() > 0.0 ? getSampleRate() : logSR;
    const double scale    = deviceSR / logSR;

    const auto rebase = [base, scale](uint64_t t) -> uint64_t
    {
        const uint64_t rel = t > base ? t - base : 0;
        return static_cast<uint64_t>(static_cast<double>(rel) * scale);
    };
    for (auto& n : state.noteEvents)       n.timestamp = rebase(n.timestamp);
    for (auto& p : state.paramEvents)      p.timestamp = rebase(p.timestamp);
    for (auto& g : state.generationEvents) g.timestamp = rebase(g.timestamp);

    // The tape's param values are DENORMALISED, so a number written under an
    // older calibration means something else today. Migrated ONCE here rather
    // than at each apply, so the tape feed on screen shows the same number the
    // parameter will take. The start-state went through migrateValueTree in
    // setStateInformation below; without this the tape would start correct and
    // jump wrong at the first curve event (a logged Lin index of 2 would land on
    // Exp). A tape with no epoch in its header is epoch 0 — see migrateLoggedValue
    // for what that does and does not cover.
    {
        const int tapeEpoch = reader.getHeader().calibEpoch;
        for (auto& p : state.paramEvents)
            p.value = Calibration::migrateLoggedValue(p.paramId, p.value, tapeEpoch);
    }
    state.totalDurationSamples = rebase(reader.getTotalDurationSamples());
    state.sampleRate = deviceSR;   // the tail check below now lives in the device domain

    // Restore the start-state (decoded and validated at the top) so the engine is in
    // the exact configuration the session was recorded with. setStateInformation
    // guards the param flood itself — do NOT wrap it in a second BulkParamLoadGuard;
    // the suppress flag is a plain bool, not a counter. eventLogInReplayRestore_
    // keeps this restore out of any live recording (start-state + marker).
    eventLogInReplayRestore_ = true;
    stateRestoreMarkerName_ = "replay_start";
    setStateInformation(startStateBytes.getData(), static_cast<int>(startStateBytes.getDataSize()));
    eventLogInReplayRestore_ = false;

    // Publish under the callback lock: processBlock reads replayState_ by
    // reference, so the vectors must not be reseated while a block is in flight.
    // The lock delivers that on Standalone/VST3/AU, where the JUCE wrapper holds
    // it across the whole processBlock call. NOT on CLAP: clap-juce-extensions
    // calls processBlock bare (zero getCallbackLock in its wrapper), so on that
    // format this publish is unsynchronised against a block in flight. Same hole
    // as every other lock-only publish here -- see
    // [[project_processblock_holds_callbacklock]]; it is not specific to this
    // site and is not fixed here.
    {
        const juce::ScopedLock sl(getCallbackLock());
        replayState_ = std::move(state);
        replayPlayhead_.store(0, std::memory_order_relaxed);
        replayDueGenerationId_.store(0, std::memory_order_relaxed);
        replayGenerationBusy_.store(false, std::memory_order_relaxed);
        replayRate_.store(1.0f, std::memory_order_relaxed);   // every tape starts at ×1
        replayRateFrac_ = 0.0;   // audio-thread member, but the callback is held out by this lock
        replayEpoch_.fetch_add(1, std::memory_order_acq_rel);   // invalidates in-flight generations from a prior tape
        replayModeActive_.store(true, std::memory_order_release);
    }

    // Kill anything the user was holding when they hit Play — the audio thread
    // consumes this on its next block (never call voiceManager from here).
    requestMidiPanic();

    replayTimer_.owner = this;
    replayTimer_.startTimerHz(30);   // param application + end-of-tape detection
    return true;
}

void T5ynthProcessor::stopReplay()
{
    replayTimer_.stopTimer();

    // Clear the flag first so the audio thread stops injecting on the next block;
    // replayState_ itself is left alone (an in-flight block may still be reading it).
    replayModeActive_.store(false, std::memory_order_release);
    replayDueGenerationId_.store(0, std::memory_order_relaxed);
    replayGenerationBusy_.store(false, std::memory_order_relaxed);

    // Release whatever the tape left sounding. Audio-thread-consumed, so this is
    // safe from the message thread — unlike calling voiceManager.allNotesOff() here.
    requestMidiPanic();

    // Hand the user their patch back (see preReplayPatch_). Done after the flag is
    // cleared so the restore is logged as one preset_loaded marker in a live
    // recording rather than swallowed as replay traffic.
    if (preReplayPatch_.getSize() > 0)
    {
        // Move it out first: setStateInformation must not read a member this call
        // could otherwise re-enter, and a failed restore must not retry forever.
        const juce::MemoryBlock patch = std::move(preReplayPatch_);
        preReplayPatch_.reset();
        stateRestoreMarkerName_ = "replay_end";   // setStateInformation's own guard names the marker
        eventLogInReplayRestore_ = true;          // keep this restore out of a live recording
        setStateInformation(patch.getData(), static_cast<int>(patch.getSize()));
        eventLogInReplayRestore_ = false;

        // setStateInformation zeroes these unconditionally ("no acoustic surprise on
        // session reopen"). Correct for a host reopening a project; wrong here, where
        // Stop Replay promises the user the state they left. Put them back.
        const auto restore = [this](const char* pid, float v)
        {
            if (auto* p = parameters.getParameter(pid))
                p->setValueNotifyingHost(p->convertTo0to1(v));
        };
        restore(PID::seqRunning,      preReplaySeqRunning_);
        restore(PID::genSeqRunning,   preReplayGenSeqRunning_);
        restore(PID::repromptStance,  preReplayRepromptStance_);
        restore(PID::dcoRepromptStance, preReplayDcoStance_);
    }
}

bool T5ynthProcessor::takeDueReplayGeneration(GenerationEventLogEntry& out)
{
    // Message thread. Claim the busy slot BEFORE consuming the due flag: taking the
    // flag first would leave a window in which the audio thread sees flag==0 and
    // busy==false and arms a second generation, breaking "one in flight".
    bool expected = false;
    if (! replayGenerationBusy_.compare_exchange_strong(expected, true,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire))
        return false;

    const uint64_t oneBased = replayDueGenerationId_.exchange(0, std::memory_order_acquire);
    const size_t   idx      = static_cast<size_t>(oneBased) - 1;
    if (oneBased == 0 || idx >= replayState_.generationEvents.size())
    {
        replayGenerationBusy_.store(false, std::memory_order_release);   // nothing due; hand the slot back
        return false;
    }

    out = replayState_.generationEvents[idx];
    return true;
}

void T5ynthProcessor::replayGenerationFinished(uint32_t epoch)
{
    // A generation dispatched for a previous tape must not release the current
    // tape's slot — its own slot was already reset by that tape's startReplay().
    if (epoch != replayEpoch_.load(std::memory_order_acquire))
        return;
    replayGenerationBusy_.store(false, std::memory_order_release);
}

void T5ynthProcessor::replayTimerTick()
{
    // Message thread, 30 Hz. Param application is deliberately NOT sample-accurate:
    // a parameter landing a few ms late is inaudible, and setValueNotifyingHost
    // locks — it can never run on the audio thread.
    if (! isReplayActive())
        return;

    const uint64_t playhead = replayPlayhead_.load(std::memory_order_relaxed);
    auto& params = replayState_.paramEvents;

    // The logged value is DENORMALISED (APVTS::Listener hands parameterChanged the
    // denormalised value), so it goes back in through convertTo0to1 — the same
    // idiom every other programmatic param write in this file uses.
    while (replayState_.nextParamIdx < params.size()
           && params[replayState_.nextParamIdx].timestamp <= playhead)
    {
        const auto& pe = params[replayState_.nextParamIdx++];
        if (auto* p = parameters.getParameter(pe.paramId))
            p->setValueNotifyingHost(p->convertTo0to1(pe.value));
    }

    // End of tape: stop once the playhead has passed the last event, plus a short
    // tail so final releases ring out rather than being cut by the panic.
    const uint64_t tail = static_cast<uint64_t>(replayState_.sampleRate * 2.0);
    if (playhead > replayState_.totalDurationSamples + tail)
    {
        stopReplay();
        if (onReplayFinished)
            onReplayFinished();
    }
}

juce::NormalisableRange<float> T5ynthProcessor::makeDurationRange(float maxSeconds)
{
    // Single source for the Duration range's skew + snapping. The APVTS
    // parameter is registered once at the global maximum (120s) so it can hold
    // any model's duration; PromptPanel narrows the *slider* per model (11s
    // default, 120s for SA3) via setNormalisableRange using this same factory,
    // so the slider feel and the snapping never drift from the parameter.
    return juce::NormalisableRange<float>(0.1f, maxSeconds,
        convertSkew03From0To1,
        convertSkew03To0To1,
        snapGenerationDuration);
}

juce::AudioProcessorValueTreeState::ParameterLayout T5ynthProcessor::createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Helper: build a juce::StringArray of display labels from any
    // BlockParams.h kEntries table. Keeps AudioParameterChoice construction
    // in sync with the single source of truth.
    auto toChoices = [](const auto& entries) {
        juce::StringArray arr;
        for (const auto& e : entries) arr.add(e.label);
        return arr;
    };

    // Oscillator
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::oscScan, 1}, "Scan Position",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));

    // Voice count: Mono, 4, 6, 8, 12, 16
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::voiceCount, 1}, "Voice Count",
        toChoices(VoiceCount::kEntries), 3)); // default 8

    // Tuning system
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::tuning, 1}, "Tuning",
        toChoices(TuningType::kEntries), 0)); // default 12-TET

    // Amplitude Envelope (A=0, D=200ms, S=10%, R=180ms)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::ampAttack, 1}, "Attack",
        juce::NormalisableRange<float>(0.0f, 5000.0f, 0.1f, 0.3f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::ampDecay, 1}, "Decay",
        juce::NormalisableRange<float>(0.0f, 5000.0f, 0.1f, 0.3f), 200.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::ampSustain, 1}, "Sustain",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.1f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::ampRelease, 1}, "Release",
        juce::NormalisableRange<float>(0.0f, 10000.0f, 0.1f, 0.3f), 180.0f));

    // Filter
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::filterCutoff, 1}, "Filter Cutoff",
        juce::NormalisableRange<float>(20.0f, 20000.0f, 0.1f, 0.25f), 20000.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::filterResonance, 1}, "Filter Resonance",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::filterType, 1}, "Filter Type",
        toChoices(FilterType::kEntries), 1));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::filterSlope, 1}, "Filter Slope",
        toChoices(FilterSlope::kEntries), 1));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::filterMix, 1}, "Filter Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::filterKbdTrack, 1}, "Filter Kbd Track",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::filterDrive, 1}, "Filter Drive",
        juce::NormalisableRange<float>(0.0f, 36.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::filterDriveOs, 1}, "Filter Drive OS",
        toChoices(FilterDriveOs::kEntries), FilterDriveOs::X2));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::filterAlgorithm, 1}, "Filter Algorithm",
        toChoices(FilterAlgorithm::kEntries), FilterAlgorithm::SVF));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::filterWarpStyle, 1}, "Filter Warp Style",
        toChoices(FilterWarpStyle::kEntries), FilterWarpStyle::Tanh));

    // Delay
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::delayTime, 1}, "Delay Time",
        juce::NormalisableRange<float>(1.0f, 2000.0f, 0.1f, 0.35f), 250.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::delayFeedback, 1}, "Delay Feedback",
        juce::NormalisableRange<float>(0.0f, 0.95f), 0.35f));
    // Mix defaults are knob positions, so they mean something different under the
    // epoch-5 FxMixLaw and CANNOT be migrated (a default is not a stored value).
    // Both effect TYPES default to Off, so there is no old default sound to
    // preserve — these are simply where the knob starts when an effect is switched
    // on, chosen for what they now produce. Under the old law delayMix 0.30 meant
    // -3.8 dB wet/dry (with the default feedback) and reverbMix 0.25 meant -3.5 dB
    // on the plate but -12.3 dB on Algo — the same number, two different sounds.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::delayMix, 1}, "Delay Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f));   // -12.0 dB W/D

    // The amplifier chain: distortion -> chorus -> phaser -> tremolo, ahead of
    // delay and reverb (src/dsp/AmpEffects.h). BJ, 2026-07-31, asked for these
    // "ggf noch ohne UI", so they are declared here and reachable through the
    // author shelf and presets, with no control surface yet.
    //
    // EVERY DEFAULT IS OFF. Adding parameters to a shipping synth may not change
    // one existing preset, and an APVTS default is what every preset written
    // before today will load. 0 dB of drive, 0 depth, 0 mix.
    // The four bypasses. ON by default, which is the state every patch written
    // before them was in: the chain's OFF has always been "mix or depth at 0",
    // and that still holds — this switch only adds a way to silence an effect
    // without moving its settings, which is what the panel's OFF cell needs.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::fxDistOn, 1}, "Dist On", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::fxChorusOn, 1}, "Chorus On", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::fxPhaserOn, 1}, "Phaser On", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::fxTremOn, 1}, "Trem On", true));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxDistDrive, 1}, "Dist Drive",
        juce::NormalisableRange<float>(0.0f, 36.0f, 0.1f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxDistMix, 1}, "Dist Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxTremRate, 1}, "Trem Rate",
        juce::NormalisableRange<float>(0.05f, 20.0f, 0.01f, 0.4f), 5.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxTremDepth, 1}, "Trem Amt",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxTremStereo, 1}, "Trem Stereo",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    // Sine first because that is what the tremolo was before it had a choice —
    // the default has to leave every existing patch sounding as it did.
    {
        juce::StringArray tremWaveLabels;
        for (const auto& e : TremWave::kEntries) tremWaveLabels.add(e.label);
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{PID::fxTremWave, 1}, "Trem Wave",
            tremWaveLabels, TremWave::Sine));
    }
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxChorusRate, 1}, "Chorus Rate",
        juce::NormalisableRange<float>(0.05f, 10.0f, 0.01f, 0.5f), 0.8f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxChorusDepth, 1}, "Chorus Amt",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.35f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxChorusMix, 1}, "Chorus Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxPhaserRate, 1}, "Phaser Rate",
        juce::NormalisableRange<float>(0.02f, 10.0f, 0.01f, 0.5f), 0.4f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxPhaserDepth, 1}, "Phaser Amt",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxPhaserFeedback, 1}, "Phaser Feedback",
        juce::NormalisableRange<float>(-0.95f, 0.95f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::fxPhaserMix, 1}, "Phaser Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));

    // Reverb
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::reverbMix, 1}, "Reverb Mix",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.3f));   // -15.6 dB W/D,
        // now identical for Algo and Plate. 0.25 would read -18.0 dB — audible but
        // shy for a starting point.

    // Algorithmic reverb parameters (only active when reverb_type == Algo)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::algoRoom, 1}, "Algo Room",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.7f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::algoDamping, 1}, "Algo Damping",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.4f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::algoWidth, 1}, "Algo Width",
        juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    // Generation
    // Alpha: quadratic curve around 0 for fine control near center (±0.15 sensitive zone)
    auto alphaRange = juce::NormalisableRange<float>(-2.0f, 2.0f,
        [](float s, float e, float n) {
            float c = n * 2.0f - 1.0f;
            float curved = (c >= 0.0f ? 1.0f : -1.0f) * c * c;
            return s + (e - s) * (curved * 0.5f + 0.5f);
        },
        [](float s, float e, float v) {
            float norm = (v - s) / (e - s);
            float c = norm * 2.0f - 1.0f;
            float uncurved = (c >= 0.0f ? 1.0f : -1.0f) * std::sqrt(std::abs(c));
            return uncurved * 0.5f + 0.5f;
        },
        snapGenerationAlpha);
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::genAlpha, 1}, "Alpha",
        alphaRange, 0.0f));

    auto magnitudeRange = juce::NormalisableRange<float>(0.001f, 5.0f,
        convertSkew03From0To1,
        convertSkew03To0To1,
        snapGenerationMagnitude);
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::genMagnitude, 1}, "Magnitude",
        magnitudeRange, 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::genNoise, 1}, "Noise",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f, 0.3f), 0.0f));
    // Resynth (init_audio / i2i): a single Off->Full amount, no separate toggle —
    // the slider's minimum IS off. 0 = ordinary text-only generation; turning up
    // feeds the last raw generation back as the denoise seed so each render evolves
    // from the previous one, and 1 = full effect (output follows the fed-back
    // source most strongly). buildInferenceRequest maps the amount onto SA3's
    // MEASURED useful init_noise band (0.48..0.05); 0 sends no init_audio at all.
    // Default off so normal SA3 generation is unchanged until you opt in.
    // 0.05 grid (21 steps): the six named anchors (0 / .05 / .25 / .5 / .75 / 1 →
    // Off / Min / Subtle / Medium / Strong / Full) all land exactly on the grid so
    // the word readout maps one-to-one to a click-stop and "Full" is unambiguously
    // the rightmost stop; "Min" is the smallest active step (one grid notch above
    // Off). The 0.05 steps in between let you dial a value the words don't name (the
    // readout shows a percentage off-anchor). Drift modulates resynth through an
    // override path, not this param, so the stepped base does not coarsen drift's sweep.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::resynthAmount, 1}, "Resynth",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.05f), 0.0f));

    // Semantic self-listening loop (CLAP ear → LLM interpreter → next prompt).
    // Read message-thread-only at generation time (PromptPanel::runSemanticLoopStep
    // + buildInferenceRequest's init_noise override) — deliberately NOT in
    // ParamCache / BlockParams / processBlock (no audio-thread consumer; an APVTS
    // lookup there would be a pure idle-CPU regression). Both default to index 0
    // (stance Off → loop disabled; coupling alpha → A anchor / B rewritten).
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::repromptStance, 1}, "Re-Prompt Stance",
        toChoices(RepromptStance::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::repromptCoupling, 1}, "Re-Prompt Coupling",
        toChoices(RepromptCoupling::kEntries), 0));
    // DCO panel's own Re-Prompt stance (docs/DCO_REPROMPT_CONCEPT.md) — a SEPARATE
    // parameter from repromptStance above (paradigm isolation), reusing the same
    // RepromptStance::kEntries table (the DCO stance bar's glyphs are index-
    // hardwired to that order; a curated DCO-specific stance set is a documented
    // follow-up, not this slice). Read message-thread-only by PromptPanel, same as
    // repromptStance — no audio-thread consumer.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::dcoRepromptStance, 1}, "DCO Re-Prompt Stance",
        toChoices(RepromptStance::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::resynthSource, 1}, "Resynth Source",
        toChoices(ResynthSource::kEntries), 0));   // default 0 = Internal (self-feedback)

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::genAxesAmount, 1}, "Axes Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));

    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::genDuration, 1}, "Duration",
        // The parameter spans the global maximum (120s). The *slider* ceiling is
        // model-dependent (PromptPanel::applyDurationRangeForCurrentModel): 11s
        // for SAO/AudioLDM2 — T5ynth's short-sound default — and 120s for SA3,
        // whose rotary-DiT generates variable-length, music-scale audio for
        // embedded/deconstructed samples. The slider can't exceed its model's
        // real ceiling; the parameter just has to be able to hold 120s.
        makeDurationRange(120.0f), 3.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::infSteps, 1}, "Steps", 1, 100, 8));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::genCfg, 1}, "CFG Scale",
        juce::NormalisableRange<float>(1.0f, 15.0f, 0.1f), 1.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::genStart, 1}, "Start Position",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::genSeed, 1}, "Seed", -1, 999999999, 123456789));

    // Engine mode
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::engineMode, 1}, "Engine Mode",
        toChoices(EngineMode::kEntries), 0));
    // Whether an authored LRO instrument may also set the synth's own knobs.
    // Default OFF: the knobs are the player's until the player says otherwise.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::lcoSetsParams, 1}, "LRO Sets Synth Params", false));
    // The twelve knobs an authored LRO instrument gives the player — the
    // library parameters its body kept (backend/lco_write.py, LroControls.h).
    // They exist ALWAYS and under these fixed ids, even when no orchestra has
    // one: a parameter that came and went with the sound could be neither
    // automated nor stored in a preset, and the host would drop its automation
    // lane every time a new instrument was written. What each one MEANS is not
    // decided here — the name over the slider is the library's, travelling with
    // the instrument. 0.5 is what a channel nothing reads sits at in the
    // orchestra head, so an unused knob reads the same on both sides.
    {
        static constexpr const char* kLroIds[] = {
            PID::lroP1a, PID::lroP1b, PID::lroP1c, PID::lroP1d,
            PID::lroP2a, PID::lroP2b, PID::lroP2c, PID::lroP2d,
            PID::lroP3a, PID::lroP3b, PID::lroP3c, PID::lroP3d };
        for (int i = 0; i < 12; ++i)
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{kLroIds[i], 1},
                // charToString, not String(char): juce::String has no char
                // constructor, so a char promotes to int and picks String(int)
                // — the host's automation list would read "LRO 197".
                "LRO " + juce::String(1 + i / 4)
                       + juce::String::charToString(
                             static_cast<juce::juce_wchar>("abcd"[i % 4])),
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 0.5f));
    }
    // One level per part, on the scaffold's `osc1vol`…`osc3vol`. Default 1.0 is
    // not a taste: it is the constant those channels held while nothing wrote
    // them, so every preset and every already-authored orchestra sounds exactly
    // as before until the player moves one.
    {
        static constexpr const char* kLvlIds[] = {
            PID::lroLvl1, PID::lroLvl2, PID::lroLvl3 };
        for (int i = 0; i < 3; ++i)
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{kLvlIds[i], 1},
                "LRO Level " + juce::String(1 + i),
                juce::NormalisableRange<float>(0.0f, 1.0f, 0.001f), 1.0f));
    }
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::freezeTexture, 1}, "Granular Texture",
        toChoices(FreezeTexture::kEntries), FreezeTexture::Silk));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::freezeStereo, 1}, "Granular Stereo",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.25f));

    // Mod envelopes ENV 2..5 (A=0, D=2500ms, S=10%, R=4000ms, Amt=0)
    for (int i = 0; i < kNumModEnvs; ++i)
    {
        const auto& id = PID::modEnv[i];
        const juce::String n = "Mod" + juce::String(i + 1) + " ";
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id.attack, 1}, n + "Attack",
            juce::NormalisableRange<float>(0.0f, 5000.0f, 0.1f, 0.3f), 0.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id.decay, 1}, n + "Decay",
            juce::NormalisableRange<float>(0.0f, 5000.0f, 0.1f, 0.3f), 2500.0f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id.sustain, 1}, n + "Sustain",
            juce::NormalisableRange<float>(0.0f, 1.0f), 0.1f));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id.release, 1}, n + "Release",
            juce::NormalisableRange<float>(0.0f, 10000.0f, 0.1f, 0.3f), 4000.0f));
    }

    // LFO 1 (reference defaults: rate=2.0, depth=0, sine)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::lfo1Rate, 1}, "LFO1 Rate",
        juce::NormalisableRange<float>(0.01f, 30.0f, 0.01f, 0.3f), 2.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::lfo1Depth, 1}, "LFO1 Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo1Wave, 1}, "LFO1 Wave",
        toChoices(LfoWave::kEntries), 0));

    // LFO 2 (reference defaults: rate=0.5, depth=0, triangle)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::lfo2Rate, 1}, "LFO2 Rate",
        juce::NormalisableRange<float>(0.01f, 30.0f, 0.01f, 0.3f), 0.5f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::lfo2Depth, 1}, "LFO2 Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo2Wave, 1}, "LFO2 Wave",
        toChoices(LfoWave::kEntries), 1));

    // LFO 3 (defaults: rate=0.2, depth=0, sine)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::lfo3Rate, 1}, "LFO3 Rate",
        juce::NormalisableRange<float>(0.01f, 30.0f, 0.01f, 0.3f), 0.2f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::lfo3Depth, 1}, "LFO3 Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo3Wave, 1}, "LFO3 Wave",
        toChoices(LfoWave::kEntries), 0));

    // MIDI aftertouch performance routing: one bipolar amount per target
    // (-1..+1, default 0 = off). Each AT target owns its signed depth; the UI
    // binds to these per-target floats.
    {
        struct AtTarget { const char* pid; const char* name; };
        const AtTarget atTargets[] = {
            { PID::aftertouchAmtLfo1Depth,   "AT LFO1 Amount"   },
            { PID::aftertouchAmtLfo2Depth,   "AT LFO2 Amount"   },
            { PID::aftertouchAmtLfo3Depth,   "AT LFO3 Amount"   },
            { PID::aftertouchAmtEnv1Sustain, "AT ENV1 Sustain" },
            { PID::aftertouchAmtEnv2Sustain, "AT ENV2 Sustain" },
            { PID::aftertouchAmtEnv3Sustain, "AT ENV3 Sustain" },
            { PID::aftertouchAmtCutoff,      "AT Cutoff"       },
            { PID::aftertouchAmtResonance,   "AT Resonance"    },
            { PID::aftertouchAmtScan,        "AT Scan"         },
            { PID::aftertouchAmtDca,         "AT DCA"          },
            { PID::aftertouchAmtPitch,       "AT Pitch"        },
            { PID::aftertouchAmtNoiseLevel,  "AT Noise"        },
            { PID::aftertouchAmtEnv4Sustain, "AT ENV4 Sustain" },
            { PID::aftertouchAmtEnv5Sustain, "AT ENV5 Sustain" },
            { PID::aftertouchAmtCache,       "AT Cache"        },
            { PID::aftertouchAmtSnap,        "AT Snap"         },
        };
        // Honest linear bipolar amount, 0.01 step (two decimals). The DSP
        // full-scales are musical, so the control needs no skew or 1/1000-scale
        // values — the whole travel maps to a usable range. AT→Cutoff feeds the
        // shared cutoff bus, whose own depth law (ModCalib::cutoffDepthCurve)
        // curves this position on the way in: ±4 oct at 0.67, ±10 at full.
        for (const auto& a : atTargets)
            params.push_back(std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID{ a.pid, 1 }, a.name,
                juce::NormalisableRange<float>(-1.0f, 1.0f, 0.01f), 0.0f));

        // One source per target, beside its amount. Three rows start wired
        // (ExprSource::defaultFor): DCA on Z, Cutoff and Scan on Y. The other
        // thirteen start on None -- a fresh patch says what it is instead of
        // arming fourteen depths at once, and the column reads at a glance.
        // A default per row rather than one for the whole column because the
        // column is sixteen different things, and "which axis should this be on"
        // has sixteen answers, thirteen of them "none yet".
        static_assert(sizeof(atTargets) / sizeof(atTargets[0])
                          == AftertouchTarget::kCount - 1,
                      "Every target but None needs an amount AND a source.");
        for (int t = 1; t < AftertouchTarget::kCount; ++t)
            params.push_back(std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID{ kExprSrcPid[t], 1 },
                juce::String("Expr Source ") + AftertouchTarget::kEntries[t].label,
                toChoices(ExprSource::kEntries), ExprSource::defaultFor(t)));
    }

    // Drift LFO
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::driftEnabled, 1}, "Drift Enabled", false));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::driftRegen, 1}, "Regenerate",
        toChoices(DriftRegen::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::driftCrossfade, 1}, "Drift Crossfade",
        juce::NormalisableRange<float>(0.0f, 2000.0f, 1.0f), 200.0f));
    // Offline cache take. Not automatable: it does not shape a sound, it decides
    // how the cache RECORDS one — a take that is paced by the machine it runs on
    // captures a different stretch of the drift on a fast box than on a slow one.
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::cacheAsync, 1}, "Cache Offline Take", false,
        juce::AudioParameterBoolAttributes().withAutomatable(false)));
    // Drift rate floor = 1/128 Hz = 128 s/cycle (≈ 64 bars @120 BPM): the slowest
    // genuinely useful drift on T5ynth's short sounds — slower than that the cycle
    // is effectively static within a note. Free mode displays the period (s/cyc);
    // the param stays Hz with a log skew. Pre-existing sub-floor drift clamps up
    // to the floor on load (raising a floor is inherently lossy — accepted).
    // Default 0.25 Hz (4 s/cyc) = the 2/1 sync division @120 BPM, so a fresh
    // drift idles at the same musical rate whether free-running or BPM-synced.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::drift1Rate, 1}, "Drift1 Rate",
        juce::NormalisableRange<float>(1.0f / 128.0f, 2.0f, 0.001f, 0.3f), 0.25f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::drift1Depth, 1}, "Drift1 Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::drift2Rate, 1}, "Drift2 Rate",
        juce::NormalisableRange<float>(1.0f / 128.0f, 2.0f, 0.001f, 0.3f), 0.25f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::drift2Depth, 1}, "Drift2 Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::drift3Rate, 1}, "Drift3 Rate",
        juce::NormalisableRange<float>(1.0f / 128.0f, 2.0f, 0.001f, 0.3f), 0.25f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::drift3Depth, 1}, "Drift3 Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));

    // Drift targets + waveform selection
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift1Target, 1}, "Drift1 Target",
        toChoices(DriftTarget::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift2Target, 1}, "Drift2 Target",
        toChoices(DriftTarget::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift1Wave, 1}, "Drift1 Wave",
        toChoices(DriftWave::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift2Wave, 1}, "Drift2 Wave",
        toChoices(DriftWave::kEntries), 0));

    // Drift 3 target + waveform (was missing — drift3 rate/depth existed but had no target/wave)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift3Target, 1}, "Drift3 Target",
        toChoices(DriftTarget::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift3Wave, 1}, "Drift3 Wave",
        toChoices(DriftWave::kEntries), 0));

    // ENV Amount (per envelope)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::ampAmount, 1}, "Amp Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));
    for (int i = 0; i < kNumModEnvs; ++i)
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{PID::modEnv[i].amount, 1},
            "Mod" + juce::String(i + 1) + " Amount",
            juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    // Global velocity amount: how strongly note velocity scales EVERY envelope's
    // peak — i.e. the env's depth on whatever it targets (DCA loudness, filter,
    // pitch, scan…). 1.0 = full (peak == velocity), 0.0 = velocity-independent.
    // Orthogonal to the per-env Amt (static depth); see SynthVoice::velPeakScale.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::velAmt, 1}, "Velocity Amount",
        juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f));

    // Per-stage velocity sensitivity, signed [-1..+1]: velocity→stage TIME only
    // (A/D/R), default 0 (no velocity effect). Velocity→peak is velAmt above; the
    // held level is expressed via Aftertouch, not velocity.
    auto addVelSens = [&params](const char* id, const juce::String& name, float def) {
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id, 1}, name,
            juce::NormalisableRange<float>(-1.0f, 1.0f), def));
    };
    addVelSens(PID::ampAttackVelSens,  "Amp Attack Vel Sens",  0.0f);
    addVelSens(PID::ampDecayVelSens,   "Amp Decay Vel Sens",   0.0f);
    addVelSens(PID::ampReleaseVelSens, "Amp Release Vel Sens", 0.0f);
    for (int i = 0; i < kNumModEnvs; ++i)
    {
        const auto& id = PID::modEnv[i];
        const juce::String n = "Mod" + juce::String(i + 1) + " ";
        addVelSens(id.attackVelSens,  n + "Attack Vel Sens",  0.0f);
        addVelSens(id.decayVelSens,   n + "Decay Vel Sens",   0.0f);
        addVelSens(id.releaseVelSens, n + "Release Vel Sens", 0.0f);
    }

    // ENV Loop (per envelope)
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::ampLoop, 1}, "Amp Loop", false));
    for (int i = 0; i < kNumModEnvs; ++i)
        params.push_back(std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID{PID::modEnv[i].loop, 1},
            "Mod" + juce::String(i + 1) + " Loop", false));

    // ENV Curve — a CONTINUOUS per-stage bend since 2026-08-05, not the five-step
    // choice it was: -1 Log … -0.5 SLog … 0 Lin … +0.5 SExp … +1 Exp. The five old
    // shapes are those five VALUES exactly (ADSREnvelope.h), so no stored patch
    // changes its curve: the .t5p JSON stores the shape by key and maps straight
    // onto the anchor, and the APVTS XML in a DAW session or a .t5p snapshot
    // stores the raw index and travels through Calibration epoch 9.
    //
    // The range is ASYMMETRIC and differs by stage, because the extra travel BJ
    // asked for is one-sided (EnvCurve::kBendSag): an attack may go past Log, a
    // decay and a release past Exp. Consequence, stated because it is the one
    // thing this cannot carry over: a stored NORMALISED value no longer means
    // what it did. Nothing of ours reads one — but a DAW automation lane does,
    // and a lane written against the old 5-way choice now lands one shape off
    // (0.5 was Lin and is SLog on an attack). Lanes are the host's, not in our
    // state, so there is nothing to migrate; it is worth one look at any session
    // that automated an envelope curve.
    static_assert(EnvCurve::kBendSag == kMaxEnvBend,
                  "The parameter range and the DSP's bend clamp must agree.");
    const auto attackBendRange = juce::NormalisableRange<float>(
        -EnvCurve::kBendSag, EnvCurve::kBendPole, EnvCurve::kBendStep);
    const auto fallBendRange = juce::NormalisableRange<float>(
        -EnvCurve::kBendPole, EnvCurve::kBendSag, EnvCurve::kBendStep);
    // A DAW's automation list showed the shape's NAME while this was a choice.
    // It still does: the bend reads as the pole it leans to and how far, and the
    // five old words are accepted back (a typed "SExp" is +0.5, "Exp" is +1).
    // Without the pair, a host that lets you type would take its own read-out
    // ("Exp 0.50") apart as the number 0.
    const auto curveBendAttrs = juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([] (float v, int) -> juce::String
        {
            // "Lin" is decided by the digits that get PRINTED. A separate
            // threshold would let a value just outside it still print "0.00",
            // and a host that types its own read-out back reads that magnitude
            // as 0, i.e. as the pole — Lin would jump to Exp on a round-trip.
            const auto mag = juce::String(std::abs(v), 2);
            if (mag == "0.00") return "Lin";
            return juce::String(v > 0.0f ? "Exp " : "Log ") + mag;
        })
        .withValueFromStringFunction([] (const juce::String& text) -> float
        {
            const auto s = text.trim();
            const float mag = std::abs(s.retainCharacters("0123456789.+-").getFloatValue());
            const bool hasMag = mag > 0.0f;
            if (s.startsWithIgnoreCase("slog") || s.startsWithIgnoreCase("softlog"))
                return hasMag ? -mag : -0.5f;
            if (s.startsWithIgnoreCase("sexp") || s.startsWithIgnoreCase("softexp"))
                return hasMag ? mag : 0.5f;
            if (s.startsWithIgnoreCase("log")) return hasMag ? -mag : -1.0f;
            if (s.startsWithIgnoreCase("exp")) return hasMag ? mag : 1.0f;
            if (s.startsWithIgnoreCase("lin")) return 0.0f;
            return s.getFloatValue();
        });
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::ampAttackCurve, 2},  "Amp Attack Curve",  attackBendRange,
        EnvCurve::bendFromIndex(EnvCurve::Lin), curveBendAttrs));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::ampDecayCurve, 2},   "Amp Decay Curve",   fallBendRange,
        EnvCurve::bendFromIndex(EnvCurve::Lin), curveBendAttrs));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::ampReleaseCurve, 2}, "Amp Release Curve", fallBendRange,
        EnvCurve::bendFromIndex(EnvCurve::Exp), curveBendAttrs));
    for (int i = 0; i < kNumModEnvs; ++i)
    {
        const auto& id = PID::modEnv[i];
        const juce::String n = "Mod" + juce::String(i + 1) + " ";
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id.attackCurve, 2},  n + "Attack Curve",  attackBendRange,
            EnvCurve::bendFromIndex(EnvCurve::Lin), curveBendAttrs));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id.decayCurve, 2},   n + "Decay Curve",   fallBendRange,
            EnvCurve::bendFromIndex(EnvCurve::Lin), curveBendAttrs));
        params.push_back(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{id.releaseCurve, 2}, n + "Release Curve", fallBendRange,
            EnvCurve::bendFromIndex(EnvCurve::Exp), curveBendAttrs));
    }

    // ENV / LFO target choice lists — the single source of truth lives in
    // src/dsp/BlockParams.h (EnvTarget::kEntries / LfoTarget::kEntries). The
    // enum, this APVTS StringArray and gui/SynthPanel.cpp all iterate the
    // same array, so the index↔label mapping cannot drift.
    juce::StringArray envTargetChoices;
    for (const auto& e : EnvTarget::kEntries) envTargetChoices.add(e.label);
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::ampTarget, 1}, "Amp Target", envTargetChoices, EnvTarget::DCA));
    for (int i = 0; i < kNumModEnvs; ++i)
        params.push_back(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{PID::modEnv[i].target, 1},
            "Mod" + juce::String(i + 1) + " Target", envTargetChoices, 0));

    juce::StringArray lfoTargetChoices;
    for (const auto& e : LfoTarget::kEntries) lfoTargetChoices.add(e.label);
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo1Target, 1}, "LFO1 Target", lfoTargetChoices, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo2Target, 1}, "LFO2 Target", lfoTargetChoices, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo3Target, 1}, "LFO3 Target", lfoTargetChoices, 0));

    // LFO Mode
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo1Mode, 1}, "LFO1 Mode",
        toChoices(LfoMode::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo2Mode, 1}, "LFO2 Mode",
        toChoices(LfoMode::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo3Mode, 1}, "LFO3 Mode",
        toChoices(LfoMode::kEntries), 0));

    // BPM-sync clock mode + division for LFO 1/2/3, Drift 1/2/3, Delay.
    // ClockMode default Off. Division default 1/4 for LFO/Delay
    // (ClockDivision::D1_4); Drift has its own slower list, default 2/1.
    // No DSP behaviour yet — wired up here so presets save/load and the UI
    // can attach. Sync rate computation lands in a later step.
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo1ClockMode, 1}, "LFO1 Clock Mode",
        toChoices(ClockMode::kEntries), ClockMode::Off));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo1ClockDivision, 1}, "LFO1 Clock Division",
        toChoices(ClockDivision::kEntries), ClockDivision::D1_4));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo2ClockMode, 1}, "LFO2 Clock Mode",
        toChoices(ClockMode::kEntries), ClockMode::Off));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo2ClockDivision, 1}, "LFO2 Clock Division",
        toChoices(ClockDivision::kEntries), ClockDivision::D1_4));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo3ClockMode, 1}, "LFO3 Clock Mode",
        toChoices(ClockMode::kEntries), ClockMode::Off));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::lfo3ClockDivision, 1}, "LFO3 Clock Division",
        toChoices(ClockDivision::kEntries), ClockDivision::D1_4));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift1ClockMode, 1}, "Drift1 Clock Mode",
        toChoices(ClockMode::kEntries), ClockMode::Off));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift1ClockDivision, 1}, "Drift1 Clock Division",
        toChoices(DriftDivision::kEntries), DriftDivision::D2_1));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift2ClockMode, 1}, "Drift2 Clock Mode",
        toChoices(ClockMode::kEntries), ClockMode::Off));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift2ClockDivision, 1}, "Drift2 Clock Division",
        toChoices(DriftDivision::kEntries), DriftDivision::D2_1));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift3ClockMode, 1}, "Drift3 Clock Mode",
        toChoices(ClockMode::kEntries), ClockMode::Off));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::drift3ClockDivision, 1}, "Drift3 Clock Division",
        toChoices(DriftDivision::kEntries), DriftDivision::D2_1));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::delayClockMode, 1}, "Delay Clock Mode",
        toChoices(ClockMode::kEntries), ClockMode::Off));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::delayClockDivision, 1}, "Delay Clock Division",
        toChoices(ClockDivision::kEntries), ClockDivision::D1_4));

    // Delay damp
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::delayDamp, 1}, "Delay Damp",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.5f));

    // Sampler controls
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::loopMode, 1}, "Loop Mode",
        toChoices(LoopMode::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::crossfadeMs, 1}, "Crossfade",
        juce::NormalisableRange<float>(0.0f, 500.0f, 10.0f), 150.0f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::normalize, 1}, "Normalize", true));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::loopOptimize, 2}, "Loop Optimize",
        toChoices(LoopOptimize::kEntries), 0));

    // Effect enables
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::filterEnabled, 1}, "Filter Enabled", true));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::delayType, 1}, "Delay Type",
        toChoices(DelayType::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::reverbType, 1}, "Reverb Type",
        toChoices(ReverbType::kEntries), 0));

    // The master output stage. Neither of these is on the panel (FxPanel.h:
    // "Limiter is internal only"), so both are reachable only through a preset
    // or host automation.
    //
    // `limiterThresh` now OFFSETS the static output gain, which is itself a
    // function of the voice-count switch (outputGainForThreshold). Same range,
    // same direction it always ran; its default is the reference the table is
    // written at. A stored value no longer means the same absolute level it did
    // when the master stage still carried a compressor's makeup -- that makeup
    // is gone, and how far below it the instrument now sits depends on the
    // polyphony the preset also stores. `limiterRelease` drives nothing any more -- a release time is
    // exactly what the master stage no longer has, and having one was the
    // paraphony (dsp/Limiter.h). It is KEPT rather than removed because the
    // APVTS stores a DAW session by parameter index: dropping it would re-point
    // every parameter after it in every saved session.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::limiterThresh, 1}, "Limiter Threshold",
        juce::NormalisableRange<float>(-30.0f, 0.0f, 0.1f), -3.0f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::limiterRelease, 1}, "Limiter Release",
        juce::NormalisableRange<float>(1.0f, 500.0f, 0.1f, 0.3f), 100.0f));

    // (reverb_ir merged into reverb_type switchbox)

    // Sequencer / Arpeggiator
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::seqMode, 1}, "Seq Mode",
        toChoices(SeqMode::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::seqRunning, 1}, "Seq Running", false));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::seqBpm, 1}, "Seq BPM",
        juce::NormalisableRange<float>(20.0f, 300.0f, 0.1f), 120.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::seqSteps, 1}, "Seq Steps", 1, 64, 16));
    // Sequencer note division (reference: 1/1, 1/2, 1/4, 1/8, 1/16)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::seqDivision, 1}, "Seq Division",
        toChoices(SeqDivision::kEntries), 4)); // default 1/16
    // Sequencer glide time (reference: 10-500ms, default 80)
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::seqGlideTime, 1}, "Glide Time",
        juce::NormalisableRange<float>(10.0f, 500.0f, 1.0f), 80.0f));
    // Arp rate: musical divisions (reference: 1/4, 1/8, 1/16, 1/32, 1/4T, 1/8T, 1/16T)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::arpRate, 1}, "Arp Rate",
        toChoices(ArpRate::kEntries), 2));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::arpOctaves, 1}, "Arp Octaves", 1, 4, 1));
    // Arp mode (Off = disabled, rest = enabled with that pattern)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::arpMode, 1}, "Arp Mode",
        toChoices(ArpMode::kEntries), 0));
    // Global seq gate + preset
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::seqGate, 1}, "Seq Gate",
        juce::NormalisableRange<float>(0.1f, 1.0f, 0.01f), 0.8f));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::seqShuffle, 1}, "Seq Shuffle",
        juce::NormalisableRange<float>(0.0f, 0.75f, 0.01f), 0.0f));
    // Seq octave shift: -2..+2 octaves (choice index 0..4, default 2 = no shift)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::seqOctave, 1}, "Seq Octave",
        toChoices(SeqOctave::kEntries), 2));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::seqPreset, 1}, "Seq Preset",
        toChoices(SeqPreset::kEntries), 0));

    // Generative sequencer
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::genSeqRunning, 1}, "Gen Seq Running", false));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::genSteps, 1}, "Gen Steps", 2, 32, 21));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::genPulses, 1}, "Gen Pulses", 1, 32, 16));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::genRotation, 1}, "Gen Rotation", 0, 31, 2));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::genMutation, 1}, "Gen Mutation",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.80f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::genRange, 1}, "Gen Range",
        toChoices(GenRange::kEntries), 2)); // default index 2 = "3" octaves
    // Fix toggles — lock parameters against Euclidean drift
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::genFixSteps, 1}, "Fix Steps", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::genFixPulses, 1}, "Fix Pulses", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::genFixRotation, 1}, "Fix Rotation", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::genFixMutation, 1}, "Fix Mutation", true));

    // ── Polyphonic generative sequencer — shared pitch field ──
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::genFieldMode, 1}, "Field Mode",
        toChoices(FieldMode::kEntries), FieldMode::Drift));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::genFieldRate, 1}, "Field Rate", 1, 32, 8));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::genFieldCenterPc, 1}, "Field Center PC", 0, 11, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::genFieldPivot, 1}, "Field Pivot",
        toChoices(FieldPivot::kEntries), FieldPivot::m3));

    // ── Inter-strand coordination ──
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::genCoordinationMode, 1}, "Coordination Mode",
        toChoices(CoordinationMode::kEntries), CoordinationMode::DensityBudget));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::genCoordinationCap, 1}, "Coordination Cap", 1, 5, 3));

    // ── Strand 0 — role/octave/divMult/dominance (Euclidean params share legacy gen_* IDs) ──
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::genRole, 1}, "S1 Role",
        toChoices(StrandRole::kEntries), StrandRole::Line));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::genOctave, 1}, "S1 Octave", -2, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::genDivMult, 1}, "S1 Div",
        toChoices(StrandDivMult::kEntries), StrandDivMult::X));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::genDominance, 1}, "S1 Gravity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));

    // ── Strand 2 ──
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen2Enable, 1}, "S2 Enable", false));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::gen2Role, 1}, "S2 Role",
        toChoices(StrandRole::kEntries), StrandRole::Line));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen2Octave, 1}, "S2 Octave", -2, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::gen2DivMult, 1}, "S2 Div",
        toChoices(StrandDivMult::kEntries), StrandDivMult::X));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::gen2Dominance, 1}, "S2 Gravity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen2Steps, 1}, "S2 Steps", 2, 32, 16));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen2Pulses, 1}, "S2 Pulses", 1, 32, 5));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen2Rotation, 1}, "S2 Rotation", 0, 31, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::gen2Mutation, 1}, "S2 Mutation",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.20f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen2FixSteps, 1}, "S2 Fix Steps", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen2FixPulses, 1}, "S2 Fix Pulses", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen2FixRotation, 1}, "S2 Fix Rotation", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen2FixMutation, 1}, "S2 Fix Mutation", true));

    // ── Strand 3 ──
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen3Enable, 1}, "S3 Enable", false));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::gen3Role, 1}, "S3 Role",
        toChoices(StrandRole::kEntries), StrandRole::Line));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen3Octave, 1}, "S3 Octave", -2, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::gen3DivMult, 1}, "S3 Div",
        toChoices(StrandDivMult::kEntries), StrandDivMult::X));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::gen3Dominance, 1}, "S3 Gravity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen3Steps, 1}, "S3 Steps", 2, 32, 16));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen3Pulses, 1}, "S3 Pulses", 1, 32, 5));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen3Rotation, 1}, "S3 Rotation", 0, 31, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::gen3Mutation, 1}, "S3 Mutation",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.20f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen3FixSteps, 1}, "S3 Fix Steps", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen3FixPulses, 1}, "S3 Fix Pulses", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen3FixRotation, 1}, "S3 Fix Rotation", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen3FixMutation, 1}, "S3 Fix Mutation", true));

    // ── Strand 4 ──
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen4Enable, 1}, "S4 Enable", false));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::gen4Role, 1}, "S4 Role",
        toChoices(StrandRole::kEntries), StrandRole::Line));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen4Octave, 1}, "S4 Octave", -2, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::gen4DivMult, 1}, "S4 Div",
        toChoices(StrandDivMult::kEntries), StrandDivMult::X));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::gen4Dominance, 1}, "S4 Gravity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen4Steps, 1}, "S4 Steps", 2, 32, 16));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen4Pulses, 1}, "S4 Pulses", 1, 32, 5));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen4Rotation, 1}, "S4 Rotation", 0, 31, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::gen4Mutation, 1}, "S4 Mutation",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.20f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen4FixSteps, 1}, "S4 Fix Steps", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen4FixPulses, 1}, "S4 Fix Pulses", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen4FixRotation, 1}, "S4 Fix Rotation", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen4FixMutation, 1}, "S4 Fix Mutation", true));

    // ── Strand 5 ──
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen5Enable, 1}, "S5 Enable", false));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::gen5Role, 1}, "S5 Role",
        toChoices(StrandRole::kEntries), StrandRole::Line));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen5Octave, 1}, "S5 Octave", -2, 2, 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::gen5DivMult, 1}, "S5 Div",
        toChoices(StrandDivMult::kEntries), StrandDivMult::X));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::gen5Dominance, 1}, "S5 Gravity",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen5Steps, 1}, "S5 Steps", 2, 32, 16));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen5Pulses, 1}, "S5 Pulses", 1, 32, 5));
    params.push_back(std::make_unique<juce::AudioParameterInt>(
        juce::ParameterID{PID::gen5Rotation, 1}, "S5 Rotation", 0, 31, 0));
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::gen5Mutation, 1}, "S5 Mutation",
        juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 0.20f));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen5FixSteps, 1}, "S5 Fix Steps", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen5FixPulses, 1}, "S5 Fix Pulses", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen5FixRotation, 1}, "S5 Fix Rotation", false));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::gen5FixMutation, 1}, "S5 Fix Mutation", true));

    // Scale (shared between gen seq and future features)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::scaleRoot, 1}, "Scale Root",
        toChoices(ScaleRoot::kEntries), 0));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::scaleType, 1}, "Scale Type",
        toChoices(ScaleType::kEntries), 0));

    // HF boost: compensate VAE decoder high-frequency rolloff
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::genHfBoost, 1}, "HF Boost", true));

    // Octave shift: -2 to +2 (index 0-4, default 2 = 0 octaves)
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::oscOctave, 1}, "Octave Shift",
        toChoices(OscOctave::kEntries), 2));

    // Noise oscillator: level + type
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::noiseLevel, 1}, "Noise Level",
        juce::NormalisableRange<float>(0.0f, 1.0f), 0.0f));
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::noiseType, 1}, "Noise Type",
        toChoices(NoiseKind::kEntries), 0));

    // Wavetable frame count: 0=32, 1=64, 2=128, 3=256
    params.push_back(std::make_unique<juce::AudioParameterChoice>(
        juce::ParameterID{PID::wtFrames, 1}, "WT Frames",
        toChoices(WtFrames::kEntries), 3));

    // Wavetable smooth (Catmull-Rom interpolation between frames)
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::wtSmooth, 1}, "WT Smooth", true));
    params.push_back(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID{PID::wtAutoScan, 1}, "WT Auto Scan", true));

    // Master volume: purely attenuative (0dB max). DAW fader handles boost.
    params.push_back(std::make_unique<juce::AudioParameterFloat>(
        juce::ParameterID{PID::masterVol, 1}, "Master Volume",
        juce::NormalisableRange<float>(-60.0f, 0.0f, 0.1f), 0.0f));

    return { params.begin(), params.end() };
}

void T5ynthProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    if (eventLogWriter_)
        eventLogWriter_->setSampleRate(sampleRate);

    // Fallback start-state capture: if recording is enabled but nothing has loaded a
    // patch (user just plays from the init patch), the header would otherwise carry
    // no start-state. Reads live APVTS, so it's also correct after a host restore.
    captureEventLogStartStateIfPending();

    masterOsc.prepare(sampleRate, samplesPerBlock);

    // Csound engine(s) (Phase-1 spec D9, extended Phase-2 spec S9): lazy
    // compile that never stalls a normal session load. csoundLifecycleMutex_
    // (see its declaration comment in PluginProcessor.h) serializes this
    // against handleAsyncUpdate's background-compile launches below —
    // prepareToPlay is guaranteed NOT concurrent with processBlock, but is NOT
    // guaranteed to run on the message thread (adversarial review: the
    // Standalone wrapper's AudioProcessorPlayer calls it from the audio-device
    // setup thread), so without this lock a live engine-mode switch or
    // orchestra-swap racing a host prepareToPlay could enter an engine's
    // prepare() from two threads at once. Held for at most ~100-400ms, only in
    // that rare interleaving.
    bool csoundDroppedUnconsumedSwap = false;
    {
        // Join any in-flight background compile FIRST (whichever kind — D9
        // bootstrap or a Phase-2 orchestra swap; both share this one thread
        // handle). This also IS the "generation/SR check after async
        // completion" (D9b): if a host SR/buffer-size change lands mid-compile,
        // this join waits it out, then the prepare() call below re-checks
        // isReady() and recompiles at the (possibly new)
        // sampleRate/samplesPerBlock.
        // The join MUST run without holding csoundLifecycleMutex_: the compile
        // thread acquires that mutex for its work, so joining under it
        // deadlocks if the thread was created but has not yet reached its
        // lock (adversarial-review finding). Move the handle out under the
        // lock, join outside, then re-lock for the prepare decision.
        std::thread toJoin;
        {
            std::lock_guard<std::mutex> csoundLock(csoundLifecycleMutex_);
            toJoin = std::move(csoundCompileThread_);
        }
        if (toJoin.joinable())
            toJoin.join();

        // (No csoundCompileInFlight_ store here: the compile thread clears it
        // itself as its last act, and handleAsyncUpdate may legitimately have
        // launched a NEW compile in the unlocked window above — clobbering the
        // flag would break its single-launch guard.)
        std::lock_guard<std::mutex> csoundLock(csoundLifecycleMutex_);

        // Phase-2 (S9): a swap not yet FINISHED being consumed — either the
        // just-joined compile just published csoundSwapPending_, or
        // processBlock had already started fading it (csoundSwapFading_) —
        // is RE-ARMED here, not dropped. Either way the inactive engine (or,
        // mid-fade, the fade itself) was prepared at the PRE-prepareToPlay
        // sample rate/block size (only the ACTIVE engine is re-prepared,
        // immediately below), so that stale-rate result is discarded. The
        // pending request itself is captured (below) and rewound so
        // handleAsyncUpdate recompiles the SAME orchestra text fresh, at the
        // NEW sample rate, once this lock releases — it is never left
        // stranded.
        csoundDroppedUnconsumedSwap = csoundSwapPending_.load(std::memory_order_acquire)
                                    || csoundSwapFading_.load(std::memory_order_acquire);
        csoundSwapPending_.store(false, std::memory_order_release);
        csoundSwapFading_.store(false, std::memory_order_release);
        csoundFadePos_ = 0;
        csoundFadeLen_ = 1;

        // (a): compile synchronously ONLY if a preset/session is loading
        // straight into Csound mode (~100ms once, off the audio thread) or the
        // instance is already prepared (an actual SR/buffer-size change here
        // is a real recompile). Every other case (starting in another engine
        // mode) leaves the active engine untouched — selecting Csound later is
        // what triggers the background compile path (b), via parameterChanged
        // + handleAsyncUpdate below. Re-prepares with its CURRENT orchestra
        // text (S2 keeps it via orchestraText(); empty = built-in) so an
        // active custom orchestra survives a plain SR/buffer-size change — the
        // INACTIVE engine is left exactly as it was (stays inert until the
        // next swap, S9).
        const int csoundActiveIdxAtLoad = csoundActiveIdx_.load(std::memory_order_relaxed);
        auto& csoundActiveEngineAtLoad = csoundEngines_[csoundActiveIdxAtLoad];
        const std::string activeOrchestraTextAtLoad = csoundActiveEngineAtLoad.orchestraText();
        const bool wantsCsoundAtLoad =
            static_cast<int>(paramCache.engineMode->load()) == static_cast<int>(EngineMode::Csound);
        if (wantsCsoundAtLoad || csoundActiveEngineAtLoad.isReady())
            csoundActiveEngineAtLoad.prepare(sampleRate, samplesPerBlock,
                activeOrchestraTextAtLoad.empty() ? nullptr : activeOrchestraTextAtLoad.c_str(),
                lroOsFactor_.load(std::memory_order_relaxed));

        // Rewind the started-generation marker (generation counters are only
        // ever touched under csoundLifecycleMutex_, held here) so it no longer
        // matches csoundSwapRequestGeneration_ — handleAsyncUpdate's
        // wantsNewSwapCompile sees the pending request as unconsumed again and
        // recompiles csoundPendingOrchestraText_ (untouched above) into the
        // proper swap path: priming + fade, or instant-adopt if the active
        // engine isn't ready.
        if (csoundDroppedUnconsumedSwap)
            csoundSwapStartedGeneration_ = csoundSwapRequestGeneration_ - 1;
    }
    // Outside the lock (matches handleAsyncUpdate's own compile-thread pattern):
    // wake the message thread to actually launch the re-armed swap compile.
    if (csoundDroppedUnconsumedSwap)
        triggerAsyncUpdate();

    // Phase-2 fade mix buffers (spec S5/S8): preallocated here (message/setup
    // thread) so the audio thread never allocates during a crossfade.
    for (auto& buf : csoundMixBufs_)
        buf.assign((size_t) samplesPerBlock, 0.0f);
    // Per-sample equal-power gain scratch (adversarial-review finding, see
    // this file's declaration comment in PluginProcessor.h) — same
    // preallocate-here-never-on-the-audio-thread discipline as csoundMixBufs_.
    csoundFadeGainNew_.assign((size_t) samplesPerBlock, 0.0f);
    csoundFadeGainOld_.assign((size_t) samplesPerBlock, 0.0f);

    masterSampler.prepare(sampleRate, samplesPerBlock);
    masterFreeze.prepare(sampleRate, samplesPerBlock);
    voiceManager.prepare(sampleRate, samplesPerBlock);
    lfo1.prepare(sampleRate);
    lfo2.prepare(sampleRate);
    lfo3.prepare(sampleRate);
    postFilter.prepare(sampleRate, samplesPerBlock);
    delay.prepare(sampleRate, samplesPerBlock);
    reverb.prepare(sampleRate, samplesPerBlock);
    algoReverb.prepare(sampleRate, samplesPerBlock);
    // Load default IR (medium plate)
    reverb.loadImpulseResponse(BinaryData::emt_140_plate_medium_wav,
                               static_cast<size_t>(BinaryData::emt_140_plate_medium_wavSize));
    lastReverbIr = 1; // 0=Bright, 1=Medium, 2=Dark
    ampDistortion.prepare(sampleRate, samplesPerBlock);
    ampChorus.prepare(sampleRate, samplesPerBlock);
    ampPhaser.prepare(sampleRate, samplesPerBlock);
    ampTremolo.prepare(sampleRate, samplesPerBlock);
    // OutputCeiling holds no state and needs no preparing. The output gain does:
    // seeded here so the first block after a rate/size change starts AT the
    // parameter's gain instead of ramping up to it from whatever the last
    // session left behind.
    outputGainPrev_ = outputGainForThreshold(paramCache.limiterThresh->load(),
                                             static_cast<int>(paramCache.voiceCount->load()));
    oneShotPreGainPrev_ = kOneShotReferenceGain / juce::jmax(1.0e-6f, outputGainPrev_);
    // Pre-size the internal note-event buffer so the audio thread never grows it
    // (a push_back reallocation would be a heap alloc on the audio thread). Worst
    // case is pathological — max BPM (300) + smallest division + all 5 strands +
    // arp, in a large offline-render block — which tops out around a few hundred
    // events even at an 8192-sample block. 2048 leaves a wide margin; the buffer
    // is allocated once here and only ever clear()ed (capacity retained) per block.
    internalNoteEvents_.reserve(2048);
    stepSequencer.prepare(sampleRate, samplesPerBlock);
    generativeSequencer.prepare(sampleRate, samplesPerBlock);
    arpeggiator.prepare(sampleRate, samplesPerBlock);
    lfo1Buffer.resize(static_cast<size_t>(samplesPerBlock));
    lfo2Buffer.resize(static_cast<size_t>(samplesPerBlock));
    lfo3Buffer.resize(static_cast<size_t>(samplesPerBlock));
    reverbSendBuffer.setSize(2, samplesPerBlock);
    oneShotBuffer.setSize(2, samplesPerBlock);
    pendingSequencerOneShotCount = 0;
    for (auto& voice : activeSequencerOneShots)
        voice.active = false;

    silentBlockCount = 0;
    // Allow ~10 seconds of reverb tail before deep idle
    tailBlocks = std::max(1, static_cast<int>(10.0 * sampleRate / samplesPerBlock));

    // External-capture ring: size for this host sample rate (usable history +
    // race margin). Allocation happens here on the prepare thread, never on the
    // audio thread. The lock serializes this (re)allocation against a concurrent
    // message-thread snapshot read — a host may re-call prepareToPlay (SR/buffer
    // change) while a regen snapshot is in flight; without it the reader would
    // copy from a buffer being freed. processBlock is already excluded by JUCE.
    {
        const std::lock_guard<std::mutex> lk(captureRingMutex);
        captureSampleRate    = sampleRate;
        captureUsableSamples = (int) std::ceil(kMaxCaptureSeconds * sampleRate);
        const int captureRingLen = captureUsableSamples
                                 + (int) std::ceil(kCaptureMarginSeconds * sampleRate);
        captureRing.setSize(2, captureRingLen, false, true, true);
        captureRing.clear();
        captureWritePos.store(0, std::memory_order_relaxed);
    }
}

bool T5ynthProcessor::requestCsoundOrchestra(const juce::String& orchestraText)
{
    // Tail migration (2026-07-25, kvel removal): presets, DAW sessions and SNAP
    // slots saved before that date carry the old host output line — with the
    // kvel factor that made the LRO scale as vel^2 — inside their stored
    // orchestra text. Every path an orchestra can enter by funnels through this
    // method (bake, preset JSON, DAW XML state, SNAP recall, LRO reconcile), so
    // rewriting the one scaffold line here re-bases ALL of them onto the
    // current tail while leaving the authored body byte-identical; and because
    // both save paths (getStateInformation, exportJsonPreset) read the pending
    // text stored below, a migrated preset heals on its next save.
    // Exact-match: a text without the old line passes through
    // unchanged, and the line is host scaffold outside the authored-body
    // markers, so no author-written code can be touched.
    const juce::String migratedText =
        orchestraText.replace("= asig * kgate * kvel * kpresGain",
                              "= asig * kgate * kpresGain");

    // Message thread or background (Phase-2 spec S4) — NEVER the audio thread.
    // getCallbackLock() below blocks until any in-progress processBlock call
    // returns ON Standalone/VST3/AU, where the JUCE wrapper holds that lock for
    // the whole call; on CLAP it does not, because clap-juce-extensions calls
    // processBlock bare and takes no callback lock at all, so there the read
    // below is unsynchronised. Same limit as every other lock-only reader here
    // (distributeSamplerBuffer et al.) -- see
    // [[project_processblock_holds_callbacklock]]. What the lock never is, on
    // any format, is a NEW lock introduced on the audio thread itself: taking it
    // here only ever blocks the CALLER (this method), never processBlock.
    float epochs[CsoundEngine::kMaxVoices];
    float freqs[CsoundEngine::kMaxVoices];
    {
        const juce::ScopedLock sl(getCallbackLock());
        // Mirrors what the real fade will write, which is the whole point of the
        // snapshot. This used to be a hardcoded 1.0f, justified in a comment as
        // matching processBlock's bp.performancePitchRatio — true at the time,
        // but only because THAT was itself a dead default and the pitch wheel
        // reached the orchestra nowhere. Now that the bridge publishes the real
        // wheel, passing 1.0f here would prime a swapped-in orchestra at the
        // unbent pitch while the fade immediately writes the bent one. Safe to
        // read: we hold getCallbackLock().
        voiceManager.snapshotCsoundState(epochs, freqs, voiceManager.globalPitchBendRatio());
    }

    {
        // UI mirror of the pending text, under its OWN short-lived lock (see
        // getCsoundOrchestraText): csoundLifecycleMutex_ below is held by the
        // compile thread across prepare() + primeForTakeover — over a second of
        // Csound warmup — so a GUI reader that took it would freeze the message
        // thread for the rest of every swap. Written here, in the same call that
        // writes csoundPendingOrchestraText_, so the two never disagree.
        std::lock_guard<std::mutex> textLock(csoundOrchestraTextMutex_);
        csoundOrchestraTextForUi_ = migratedText;
    }

    {
        std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
        csoundPendingOrchestraText_ = migratedText;
        std::memcpy(csoundPendingEpochs_, epochs, sizeof(csoundPendingEpochs_));
        std::memcpy(csoundPendingFreqs_, freqs, sizeof(csoundPendingFreqs_));
        ++csoundSwapRequestGeneration_;
        // A fresh request supersedes whatever the PREVIOUS request left behind —
        // including a stale failure message. Without this, a caller polling
        // csoundCompileError() right after issuing a brand-new request (Phase 5:
        // PromptPanel's compile-window Timer) could read an old error belonging
        // to a completely different, earlier orchestra text and report a false
        // failure before this request's own compile has even run.
        csoundCompileErrorText_.clear();
    }

    triggerAsyncUpdate();
    return true;
}

juce::String T5ynthProcessor::csoundCompileError() const
{
    std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
    return csoundCompileErrorText_;
}

bool T5ynthProcessor::csoundPerformanceEnded() const
{
    const int activeIdx = csoundActiveIdx_.load(std::memory_order_acquire);
    return csoundEngines_[activeIdx].performanceHasEnded();
}

void T5ynthProcessor::forceCsoundEngineMode()
{
    // Message thread. Mirrors loadDcoWavetable's engine-mode stash/force EXACTLY
    // (same dcoPrevEngineMode_ member, same restore site in loadGeneratedAudio —
    // see that function's isWavetableMode()-or-Csound check) but forces Csound
    // instead of Lco: the paradigm shift (SPEC_phase4_5_csound_llm_preset.md)
    // means PromptPanel::triggerDcoBake now authors a Csound orchestra, not a
    // wavetable bake. Re-triggering while ALREADY in Csound mode keeps the
    // ORIGINAL pre-Csound stash — the same "don't clobber an existing stash
    // with the forced mode itself" rule loadDcoWavetable applies for Lco.
    const int cur = static_cast<int>(paramCache.engineMode->load());
    if (cur != EngineMode::Csound)
        dcoPrevEngineMode_ = cur;
    if (auto* engineParam = parameters.getParameter(PID::engineMode))
        engineParam->setValueNotifyingHost(
            engineParam->convertTo0to1(static_cast<float>(EngineMode::Csound)));
}

void T5ynthProcessor::restoreNeuralEngineMode()
{
    // Message thread. The exact inverse of forceCsoundEngineMode above.
    const int cur = static_cast<int>(paramCache.engineMode->load());
    const bool onLanguageMode = (cur == EngineMode::Csound || cur == EngineMode::Lco);

    int target = dcoPrevEngineMode_;
    if (target < 0 || target == EngineMode::Csound || target == EngineMode::Lco)
        target = EngineMode::Sampler; // no stash to restore, or a language mode
    // Consume the stash unconditionally: it describes where to return FROM a
    // language mode, so once we are (or already were) on a neural engine it is
    // spent. Leaving it behind would let a stash written three steps ago
    // override an engine the user has since picked by hand.
    dcoPrevEngineMode_ = -1;
    // Leaving the LRO for a neural engine: the authored orchestra stops
    // sounding, so the knobs it borrowed go back. Discarding the record here
    // instead would strand them with nobody left able to return them.
    releaseAuthorSettings();

    if (!onLanguageMode)
        return;                       // already on a neural engine

    lcoEngineMode_ = cur;             // come back to THIS one, not a guess
    if (auto* engineParam = parameters.getParameter(PID::engineMode))
        engineParam->setValueNotifyingHost(
            engineParam->convertTo0to1(static_cast<float>(target)));
}

bool T5ynthProcessor::restoreLanguageEngineMode()
{
    // Message thread. Mirror image of restoreNeuralEngineMode: same stash, same
    // "don't clobber an existing stash with a language mode" rule as
    // forceCsoundEngineMode.
    if (lcoEngineMode_ < 0)
        return false;

    const int cur = static_cast<int>(paramCache.engineMode->load());
    if (cur != EngineMode::Csound && cur != EngineMode::Lco)
        dcoPrevEngineMode_ = cur;

    const int target = lcoEngineMode_;
    lcoEngineMode_ = -1;              // consumed; leaving again records it anew
    if (auto* engineParam = parameters.getParameter(PID::engineMode))
        engineParam->setValueNotifyingHost(
            engineParam->convertTo0to1(static_cast<float>(target)));
    return true;
}

bool T5ynthProcessor::hasCsoundOrchestra() const
{
    // Reads the UI mirror, not csoundPendingOrchestraText_: same value (both are
    // written by the same requestCsoundOrchestra call), but this one is asked on
    // the message thread by the T5osc/LCO toggle, and csoundLifecycleMutex_ is
    // held by the compile thread across a full Csound prepare + warmup — the
    // toggle would freeze for the rest of the swap.
    std::lock_guard<std::mutex> lock(csoundOrchestraTextMutex_);
    return csoundOrchestraTextForUi_.isNotEmpty();
}

juce::String T5ynthProcessor::getCsoundOrchestraText() const
{
    std::lock_guard<std::mutex> lock(csoundOrchestraTextMutex_);
    return csoundOrchestraTextForUi_;
}

void T5ynthProcessor::releaseResources()
{
    // The sampler-reprepare worker (samplerReprepareThreadMain) is NOT joined here
    // — it is started in the ctor and joined only in the dtor, so it keeps polling
    // across stop/start. Its publish critical section (serviceSamplerReprepare
    // block 4) mutates masterSampler/masterFreeze/voiceManager under getCallbackLock
    // (originalBuffer move, playBuffer.setSize, audioLoaded, snapshot atomic_store,
    // voice distribute). reset() rewrites those same members, so take the SAME lock
    // to serialize teardown against an in-flight publish; without it the two threads
    // could reallocate the same juce::AudioBuffer concurrently (heap corruption).
    // No deadlock: the worker never holds getCallbackLock and samplerReprepareSource-
    // Mutex at once, and neither do we (separate scopes); reset() takes no lock.
    {
        const juce::ScopedLock sl(getCallbackLock());
        masterOsc.reset();
        masterSampler.reset();
        masterFreeze.reset();
        voiceManager.reset();
    }
    {
        std::lock_guard<std::mutex> lock(samplerReprepareSourceMutex);
        samplerReprepareSourceBuffer.setSize(0, 0);
        samplerReprepareSourceValid = false;
        ++samplerReprepareSourceVersion;
    }
    captureWritePos.store(0, std::memory_order_relaxed);
}

bool T5ynthProcessor::assignSequencerOneShotFromCurrentRegion(int step, int slot)
{
    float regionStart = 0.0f;
    float regionEnd = 1.0f;
    {
        const juce::ScopedLock sl(getCallbackLock());
        regionStart = masterSampler.getStartPos();
        regionEnd = masterSampler.getLoopEnd();
    }

    return assignSequencerOneShotFromRegion(step, slot, regionStart, regionEnd);
}

bool T5ynthProcessor::assignSequencerOneShotFromRegion(int step, int slot, float regionStart, float regionEnd)
{
    if (step < 0 || step >= T5ynthStepSequencer::MAX_STEPS
        || slot < 0 || slot >= T5ynthStepSequencer::ONE_SHOT_SLOTS
        || !std::isfinite(regionStart) || !std::isfinite(regionEnd))
        return false;

    auto sample = std::make_shared<SequencerOneShotSample>();

    {
        const juce::ScopedLock sl(getCallbackLock());

        const auto& source = generatedAudioFull;
        const int sourceSamples = source.getNumSamples();
        const int sourceChannels = source.getNumChannels();
        if (sourceSamples <= 0 || sourceChannels <= 0)
            return false;

        const float start = juce::jlimit(0.0f, 1.0f, regionStart);
        const float end = juce::jlimit(0.0f, 1.0f, regionEnd);
        const float lo = std::min(start, end);
        const float hi = std::max(start, end);

        int startSample = juce::jlimit(0, sourceSamples - 1,
            static_cast<int>(std::floor(lo * static_cast<float>(sourceSamples))));
        int endSample = juce::jlimit(startSample + 1, sourceSamples,
            static_cast<int>(std::ceil(hi * static_cast<float>(sourceSamples))));
        if (endSample <= startSample)
            endSample = juce::jmin(sourceSamples, startSample + 1);

        const int length = endSample - startSample;
        sample->audio.setSize(sourceChannels, length, false, false, true);
        for (int ch = 0; ch < sourceChannels; ++ch)
            sample->audio.copyFrom(ch, 0, source, ch, startSample, length);

        sample->sampleRate = generatedSampleRate > 0.0 ? generatedSampleRate : getSampleRate();
        const double startSec = static_cast<double>(startSample) / sample->sampleRate;
        const double endSec = static_cast<double>(endSample) / sample->sampleRate;
        sample->label = "P1-P3 "
            + juce::String(startSec, 2) + "s-"
            + juce::String(endSec, 2) + "s";
    }

    stepSequencer.setStepOneShotMode(step, slot, T5ynthStepSequencer::OneShotMode::Normal);
    std::atomic_store_explicit(
        &sequencerOneShotSamples[static_cast<size_t>(step)][static_cast<size_t>(slot)],
        SequencerOneShotSamplePtr(std::move(sample)),
        std::memory_order_release);
    return true;
}

bool T5ynthProcessor::hasSequencerOneShotSample(int step, int slot) const
{
    if (step < 0 || step >= T5ynthStepSequencer::MAX_STEPS
        || slot < 0 || slot >= T5ynthStepSequencer::ONE_SHOT_SLOTS)
        return false;

    return std::atomic_load_explicit(
        &sequencerOneShotSamples[static_cast<size_t>(step)][static_cast<size_t>(slot)],
        std::memory_order_acquire) != nullptr;
}

void T5ynthProcessor::clearSequencerOneShotSample(int step, int slot)
{
    if (step < 0 || step >= T5ynthStepSequencer::MAX_STEPS
        || slot < 0 || slot >= T5ynthStepSequencer::ONE_SHOT_SLOTS)
        return;

    std::atomic_store_explicit(
        &sequencerOneShotSamples[static_cast<size_t>(step)][static_cast<size_t>(slot)],
        SequencerOneShotSamplePtr{},
        std::memory_order_release);
    stepSequencer.setStepOneShotMode(step, slot, T5ynthStepSequencer::OneShotMode::Normal);
}

void T5ynthProcessor::clearSequencerOneShotSamples()
{
    for (int step = 0; step < T5ynthStepSequencer::MAX_STEPS; ++step)
        for (int slot = 0; slot < T5ynthStepSequencer::ONE_SHOT_SLOTS; ++slot)
            clearSequencerOneShotSample(step, slot);
}

bool T5ynthProcessor::copySequencerOneShotSample(int srcStep, int srcSlot, int dstStep, int dstSlot)
{
    if (srcStep < 0 || srcStep >= T5ynthStepSequencer::MAX_STEPS
        || srcSlot < 0 || srcSlot >= T5ynthStepSequencer::ONE_SHOT_SLOTS
        || dstStep < 0 || dstStep >= T5ynthStepSequencer::MAX_STEPS
        || dstSlot < 0 || dstSlot >= T5ynthStepSequencer::ONE_SHOT_SLOTS)
        return false;

    if (srcStep == dstStep && srcSlot == dstSlot)
        return false;

    auto sample = std::atomic_load_explicit(
        &sequencerOneShotSamples[static_cast<size_t>(srcStep)][static_cast<size_t>(srcSlot)],
        std::memory_order_acquire);
    if (!sample || sample->audio.getNumSamples() <= 0)
        return false;

    // Duplicate the source slot's playback mode so the copy behaves identically.
    stepSequencer.setStepOneShotMode(dstStep, dstSlot,
        stepSequencer.getStepOneShotMode(srcStep, srcSlot));

    // Share the immutable sample by pointer — no deep audio copy. The audio
    // thread only atomic-loads these pointers and reads the const buffer, so
    // two slots aliasing one sample is the same lock-free sharing model used
    // for master→voice engine data. The previous dst pointer (if any) is
    // released here on the message thread by atomic_store.
    std::atomic_store_explicit(
        &sequencerOneShotSamples[static_cast<size_t>(dstStep)][static_cast<size_t>(dstSlot)],
        sample,
        std::memory_order_release);
    return true;
}

std::vector<T5ynthProcessor::SequencerOneShotExport>
T5ynthProcessor::exportSequencerOneShotSamples() const
{
    std::vector<SequencerOneShotExport> out;

    for (int step = 0; step < T5ynthStepSequencer::MAX_STEPS; ++step)
    {
        for (int slot = 0; slot < T5ynthStepSequencer::ONE_SHOT_SLOTS; ++slot)
        {
            auto sample = std::atomic_load_explicit(
                &sequencerOneShotSamples[static_cast<size_t>(step)][static_cast<size_t>(slot)],
                std::memory_order_acquire);
            if (!sample || sample->audio.getNumSamples() <= 0)
                continue;

            SequencerOneShotExport e;
            e.step = step;
            e.slot = slot;
            e.mode = stepSequencer.getStepOneShotMode(step, slot);
            e.label = sample->label;
            e.sampleRate = sample->sampleRate;
            e.audio.makeCopyOf(sample->audio);
            out.push_back(std::move(e));
        }
    }

    return out;
}

void T5ynthProcessor::importSequencerOneShotSamples(const std::vector<SequencerOneShotExport>& slots)
{
    clearSequencerOneShotSamples();

    for (const auto& slot : slots)
    {
        if (slot.step < 0 || slot.step >= T5ynthStepSequencer::MAX_STEPS
            || slot.slot < 0 || slot.slot >= T5ynthStepSequencer::ONE_SHOT_SLOTS
            || slot.audio.getNumSamples() <= 0 || slot.audio.getNumChannels() <= 0)
            continue;

        auto sample = std::make_shared<SequencerOneShotSample>();
        sample->audio.makeCopyOf(slot.audio);
        sample->sampleRate = slot.sampleRate > 0.0 ? slot.sampleRate : 44100.0;
        sample->label = slot.label;

        stepSequencer.setStepOneShotMode(slot.step, slot.slot, slot.mode);
        std::atomic_store_explicit(
            &sequencerOneShotSamples[static_cast<size_t>(slot.step)][static_cast<size_t>(slot.slot)],
            SequencerOneShotSamplePtr(std::move(sample)),
            std::memory_order_release);
    }
}

void T5ynthProcessor::queueSequencerOneShotTrigger(const T5ynthStepSequencer::OneShotTrigger& trigger)
{
    if (trigger.stepIndex < 0 || trigger.stepIndex >= T5ynthStepSequencer::MAX_STEPS
        || trigger.slotIndex < 0 || trigger.slotIndex >= T5ynthStepSequencer::ONE_SHOT_SLOTS
        || pendingSequencerOneShotCount >= kMaxSequencerOneShotVoices)
        return;

    auto& pending = pendingSequencerOneShots[static_cast<size_t>(pendingSequencerOneShotCount)];
    // `pending` is normally cleared at consume (startVoice), but a 0-sample block
    // (renderSequencerOneShots early-out) can leave a stale ref here. Retire it to the
    // bin before reusing the slot — and BEFORE loading `sample` below, so a ring-full
    // drop never strands a freshly-loaded shared_ptr local to release on the audio
    // thread. (A null pending is a no-op that returns true, so this falls through.)
    if (! retireOneShotSampleToBin(pending.sample))
        return;

    auto sample = std::atomic_load_explicit(
        &sequencerOneShotSamples[static_cast<size_t>(trigger.stepIndex)]
                                [static_cast<size_t>(trigger.slotIndex)],
        std::memory_order_acquire);
    // Test only for null here. A non-null one-shot is guaranteed non-empty by the
    // store side (assign/copy/import all reject 0-sample / 0-channel buffers), so a
    // getNumSamples()/getNumChannels() test would be dead code — and taking this
    // early return with a *non-null* `sample` would destruct the last reference on
    // the audio thread (CLAUDE.md #4). Returning only on null destructs a null and
    // never frees; renderSequencerOneShots still guards empties defensively.
    if (! sample)
        return;

    pending.sample = std::move(sample);  // assign over null
    pending.gain = trigger.gain;
    pending.sampleOffset = trigger.sampleOffset;
    ++pendingSequencerOneShotCount;
}

bool T5ynthProcessor::hasActiveSequencerOneShots() const
{
    for (const auto& voice : activeSequencerOneShots)
        if (voice.active)
            return true;
    return false;
}

bool T5ynthProcessor::retireOneShotSampleToBin(SequencerOneShotSamplePtr& ptr) noexcept
{
    // Audio thread. Hand a one-shot sample ref to the worker thread for release by
    // MOVING it into the SPSC ring — a move never changes the refcount, so it can
    // never trigger the held juce::AudioBuffer's free on the audio thread
    // (CLAUDE.md #4). A null ptr is a no-op. Returns false only when the ring is
    // full, leaving ptr untouched (still holding its ref) so the caller keeps it
    // in place rather than freeing it here.
    if (ptr == nullptr)
        return true;

    const int head = oneShotRetireHead.load(std::memory_order_relaxed);  // producer owns head
    const int next = (head + 1) & kOneShotRetireMask;
    // acquire pairs with the consumer's tail release-store: it guarantees the
    // consumer has already reset (nulled) slot `head` before we are allowed to
    // reuse it, so the move-assign below writes into a null slot and never
    // releases a ref on the audio thread.
    if (next == oneShotRetireTail.load(std::memory_order_acquire))
        return false;  // ring full — leave ptr in place (safe; retired on a later pass)

    oneShotRetireBin[static_cast<size_t>(head)] = std::move(ptr);
    oneShotRetireHead.store(next, std::memory_order_release);  // publishes the moved-in ptr
    return true;
}

void T5ynthProcessor::drainSequencerOneShotRetireBin() noexcept
{
    // Worker thread (samplerReprepareThreadMain). Releases the one-shot samples
    // the audio thread retired — the actual juce::AudioBuffer frees happen here,
    // off the audio thread. Single consumer: only this thread writes the tail.
    const int head = oneShotRetireHead.load(std::memory_order_acquire);  // see producer's moves
    int tail = oneShotRetireTail.load(std::memory_order_relaxed);        // consumer owns tail
    while (tail != head)
    {
        oneShotRetireBin[static_cast<size_t>(tail)].reset();
        tail = (tail + 1) & kOneShotRetireMask;
    }
    oneShotRetireTail.store(tail, std::memory_order_release);  // publishes the resets (slots now null)
}

void T5ynthProcessor::stopSequencerOneShots()
{
    pendingSequencerOneShotCount = 0;
    for (auto& voice : activeSequencerOneShots)
    {
        voice.active = false;
        retireOneShotSampleToBin(voice.sample);  // off-thread release (never reset() on the audio thread)
        voice.startOffset = 0;
        voice.position = 0.0;
    }
}

void T5ynthProcessor::renderSequencerOneShots(juce::AudioBuffer<float>& buffer)
{
    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    if (numSamples <= 0 || numChannels <= 0)
        return;

    auto startVoice = [this, numSamples](PendingSequencerOneShot& pending)
    {
        int target = -1;
        uint64_t oldest = std::numeric_limits<uint64_t>::max();

        for (int i = 0; i < kMaxSequencerOneShotVoices; ++i)
        {
            const auto& voice = activeSequencerOneShots[static_cast<size_t>(i)];
            if (!voice.active)
            {
                target = i;
                break;
            }
            if (voice.age < oldest)
            {
                oldest = voice.age;
                target = i;
            }
        }

        if (target < 0)
            return;

        auto& voice = activeSequencerOneShots[static_cast<size_t>(target)];

        // The chosen slot may still hold a ref: a stolen still-playing voice, or a
        // leftover from prepareToPlay / a previous bin-full retire. Releasing it by
        // overwriting voice.sample would free on the audio thread, so hand it to the
        // retire bin first. If the ring is full we must not overwrite a live ref —
        // drop this trigger instead (a missed one-shot is a glitch, never a crash).
        // A free voice holds null, so this is a no-op in the common case.
        if (! retireOneShotSampleToBin(voice.sample))
            return;

        voice.sample = pending.sample;  // assign over null (copy ⇒ refcount ≥ 2)
        voice.position = 0.0;
        const double hostRate = getSampleRate() > 0.0 ? getSampleRate() : 44100.0;
        voice.increment = juce::jmax(0.0001, pending.sample->sampleRate / hostRate);
        voice.gain = pending.gain;
        voice.startOffset = juce::jlimit(0, juce::jmax(0, numSamples - 1), pending.sampleOffset);
        voice.active = true;
        voice.age = ++sequencerOneShotAgeCounter;

        // The voice now owns a copy, so this drop can never be the last reference —
        // safe to release on the audio thread. Keeping `pending` null means the next
        // block's overwrite in queueSequencerOneShotTrigger also never frees here.
        pending.sample.reset();
    };

    for (int i = 0; i < pendingSequencerOneShotCount; ++i)
        startVoice(pendingSequencerOneShots[static_cast<size_t>(i)]);
    pendingSequencerOneShotCount = 0;

    for (auto& voice : activeSequencerOneShots)
    {
        if (!voice.active || !voice.sample)
            continue;

        const auto& source = voice.sample->audio;
        const int sourceSamples = source.getNumSamples();
        const int sourceChannels = source.getNumChannels();
        if (sourceSamples <= 0 || sourceChannels <= 0)
        {
            voice.active = false;
            continue;
        }

        const int start = juce::jlimit(0, numSamples, voice.startOffset);
        voice.startOffset = 0;

        for (int i = start; i < numSamples; ++i)
        {
            if (voice.position >= static_cast<double>(sourceSamples))
            {
                voice.active = false;
                retireOneShotSampleToBin(voice.sample);  // off-thread release (never reset() on the audio thread)
                break;
            }

            const int idx = juce::jlimit(0, sourceSamples - 1,
                static_cast<int>(voice.position));
            const int idx2 = juce::jmin(idx + 1, sourceSamples - 1);
            const float frac = static_cast<float>(voice.position - static_cast<double>(idx));

            for (int outCh = 0; outCh < numChannels; ++outCh)
            {
                const int srcCh = sourceChannels == 1 ? 0 : juce::jmin(outCh, sourceChannels - 1);
                const float s0 = source.getSample(srcCh, idx);
                const float s1 = source.getSample(srcCh, idx2);
                buffer.addSample(outCh, i, (s0 + (s1 - s0) * frac) * voice.gain);
            }

            voice.position += voice.increment;
        }
    }
}

void T5ynthProcessor::syncSamplerSettingsFromParametersLocked()
{
    int loopModeIdx = static_cast<int>(paramCache.loopMode->load());
    masterSampler.setLoopMode(static_cast<SamplePlayer::LoopMode>(juce::jlimit(0, 2, loopModeIdx)));
    masterSampler.setCrossfadeMs(paramCache.crossfadeMs->load());
    masterSampler.setNormalize(paramCache.normalize->load() > 0.5f);
    masterSampler.setLoopOptimizeLevel(static_cast<int>(paramCache.loopOptimize->load()));
}

void T5ynthProcessor::storeSamplerReprepareSource(const juce::AudioBuffer<float>& buffer,
                                                  double sampleRate,
                                                  float normalizeStartFrac,
                                                  float normalizeEndFrac)
{
    std::lock_guard<std::mutex> lock(samplerReprepareSourceMutex);
    samplerReprepareSourceBuffer.makeCopyOf(buffer);
    samplerReprepareSourceRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    samplerReprepareNormalizeStartFrac = juce::jlimit(0.0f, 1.0f, normalizeStartFrac);
    samplerReprepareNormalizeEndFrac = juce::jlimit(0.0f, 1.0f, normalizeEndFrac);
    samplerReprepareSourceValid = samplerReprepareSourceBuffer.getNumSamples() > 0
                               && samplerReprepareSourceBuffer.getNumChannels() > 0;
    ++samplerReprepareSourceVersion;
}

bool T5ynthProcessor::serviceSamplerReprepare()
{
    SamplePlayer::PrepareConfig config;
    {
        const juce::ScopedLock sl(getCallbackLock());
        syncSamplerSettingsFromParametersLocked();
        if (!masterSampler.hasAudio() || !masterSampler.needsReprepare())
            return false;
        config = masterSampler.capturePrepareConfig();
    }

    juce::AudioBuffer<float> source;
    double sourceRate = 44100.0;
    juce::uint64 sourceVersion = 0;
    float normalizeStartFrac = 0.0f;
    float normalizeEndFrac = 1.0f;
    {
        std::lock_guard<std::mutex> lock(samplerReprepareSourceMutex);
        if (!samplerReprepareSourceValid)
            return false;
        source.makeCopyOf(samplerReprepareSourceBuffer);
        sourceRate = samplerReprepareSourceRate;
        sourceVersion = samplerReprepareSourceVersion;
        normalizeStartFrac = samplerReprepareNormalizeStartFrac;
        normalizeEndFrac = samplerReprepareNormalizeEndFrac;
    }

    auto prepared = masterSampler.prepareBufferLoad(source, sourceRate, config);
    auto preparedFreezeBuffer = makeFreezeLoadBuffer(source,
                                                     sourceRate,
                                                     config.normalizeOn,
                                                     normalizeStartFrac,
                                                     normalizeEndFrac,
                                                     masterSampler);

    {
        std::lock_guard<std::mutex> lock(samplerReprepareSourceMutex);
        if (!samplerReprepareSourceValid || samplerReprepareSourceVersion != sourceVersion)
        {
            samplerReprepareWorkRequested.store(true, std::memory_order_release);
            return false;
        }
    }

    {
        const juce::ScopedLock sl(getCallbackLock());
        syncSamplerSettingsFromParametersLocked();
        const auto currentConfig = masterSampler.capturePrepareConfig();
        if (!samePrepareConfig(config, currentConfig))
        {
            samplerReprepareWorkRequested.store(true, std::memory_order_release);
            return false;
        }

        voiceManager.drainRetiredSamplerSnapshots();
        masterSampler.applyPreparedBufferLoad(std::move(prepared), config);
        masterFreeze.loadBuffer(preparedFreezeBuffer, sourceRate);
        // Held sampler voices crossfade onto the re-prepared snapshot on the next
        // audio-thread distribute pass — morphToBufferFrom's own contract confines
        // it to the audio thread (see its doc comment in SamplePlayer.h), so this
        // background-thread (samplerReprepareThread) call passes allowMorph=false
        // and leaves the crossfade to start on processBlock's own redistribute
        // pass instead. Off-thread → allowMorph=false (sync inactive voices only).
        voiceManager.distributeSamplerBuffer(masterSampler, 0.0f, /*allowMorph=*/false);
        // Sampler re-prepare (config change, not a new inference) → keep held
        // granular voices on their current buffer (no live morph).
        voiceManager.distributeFreezeBuffer(masterFreeze, 0.0f, false);
    }

    return true;
}

void T5ynthProcessor::samplerReprepareThreadMain()
{
    while (!samplerReprepareThreadShouldExit.load(std::memory_order_acquire))
    {
        samplerReprepareWorkRequested.store(false, std::memory_order_release);
        serviceSamplerReprepare();
        drainSequencerOneShotRetireBin();  // release one-shot samples the audio thread retired (off-thread)

        for (int i = 0; i < 5 && !samplerReprepareThreadShouldExit.load(std::memory_order_acquire); ++i)
        {
            if (samplerReprepareWorkRequested.exchange(false, std::memory_order_acq_rel))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

bool T5ynthProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Output must be mono or stereo.
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    // Input is OPTIONAL (live-capture seed for Resynth): allow it disabled, mono,
    // or stereo. A host that gives a synth no input simply disables the bus.
    const auto in = layouts.getMainInputChannelSet();
    if (! in.isDisabled()
        && in != juce::AudioChannelSet::mono()
        && in != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

bool T5ynthProcessor::snapshotExternalCapture (juce::AudioBuffer<float>& dest,
                                               double& sampleRateOut,
                                               double seconds) const
{
    // Hold the ring lock across the whole read: it serializes against prepareToPlay
    // reallocating captureRing under us (use-after-free). Not contended by the audio
    // thread — the writer is lock-free and processBlock can't run during prepareToPlay.
    const std::lock_guard<std::mutex> lk(captureRingMutex);

    const int    ringN = captureRing.getNumSamples();
    const double sr    = captureSampleRate;
    if (ringN <= 0 || sr <= 0.0 || seconds <= 0.0 || captureUsableSamples <= 0)
        return false;

    // Never request more than the usable history; the remaining margin keeps the
    // writer from lapping the read window during this copy.
    int want = juce::jmin(captureUsableSamples, (int) std::llround(seconds * sr));
    want = juce::jmin(want, ringN);
    if (want <= 0)
        return false;

    const int wp = captureWritePos.load(std::memory_order_acquire);
    const int ch = captureRing.getNumChannels();

    // Message thread: allocation is allowed here.
    dest.setSize(ch, want, false, false, true);

    int start = wp - want; if (start < 0) start += ringN;
    const int firstLen = juce::jmin(want, ringN - start);
    for (int c = 0; c < ch; ++c)
    {
        dest.copyFrom(c, 0, captureRing, c, start, firstLen);
        if (want > firstLen)
            dest.copyFrom(c, firstLen, captureRing, c, 0, want - firstLen);
    }

    // Silence guard: no device / denied permission / nothing playing → report no
    // capture so the caller falls back to text-only rather than seeding silence.
    float mag = 0.0f;
    for (int c = 0; c < ch; ++c)
        mag = juce::jmax(mag, dest.getMagnitude(c, 0, want));
    if (mag < 1.0e-4f)
    {
        // Empty dest so the wire serializer (PipeInference.cpp, init_audio is
        // emitted whenever initAudio.getNumSamples() > 0) omits init_audio
        // entirely — a false return must mean "no seed", not "silent seed".
        dest.setSize(0, 0);
        return false;
    }

    // Peak-normalise to ~unit amplitude. The VAE encoder expects the seed in the
    // conditioned range the OLD self-feedback seed had: the backend peak-normalises
    // model output to 1.0 (pipe_inference.py, "SA3 outputs hot"), so the old loop
    // always fed a peak-1.0 seed. Raw live input arrives at an arbitrary/hot level
    // — nothing downstream normalises init_audio (prepare_audio only resamples /
    // pad-crops) — so an un-normalised hot capture drives the encode out of
    // distribution → audible overdrive. Target 0.95 (not 1.0) leaves headroom
    // against inter-sample overshoot from the backend's 48k→model-SR resample
    // (the old seed was model-native, so it never resampled). mag is the captured
    // peak, already computed above and guaranteed >= 1e-4 here.
    dest.applyGain(0.95f / mag);

    sampleRateOut = sr;
    return true;
}

void T5ynthProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Hold the callback lock ourselves, for the whole block.
    //
    // Every off-thread publisher in this file takes getCallbackLock() before it
    // touches engine state, and the whole publish discipline -- masterSampler's
    // prepared load, masterFreeze's and masterOsc's snapshots, the traversal
    // brackets, replayState_ -- assumes that taking it excludes the audio
    // thread. On Standalone, VST3 and AU it does, because the JUCE wrapper holds
    // it across this call. On CLAP it does not: clap-juce-extensions calls
    // processBlock bare and its wrapper contains no getCallbackLock at all, so
    // there the lock excluded nothing and every plain member a publisher writes
    // -- originalBuffer, loop mode, the start/loop fractions, audioLoaded, the
    // wavetable extract brackets -- was read here unsynchronised.
    //
    // juce::CriticalSection is recursive, so on the three formats whose wrapper
    // already owns it this is a re-entry by the owning thread and costs an
    // increment. On CLAP it establishes the exclusion the rest of the file was
    // written for. This function takes no other lock anywhere in its 2600 lines,
    // so there is no ordering to get wrong; what it does inherit on CLAP is the
    // exposure the other three formats already have -- the audio thread can wait
    // on a publisher, which is why publishers keep the expensive work outside
    // the lock and only move already-prepared data inside it.
    const juce::ScopedLock callbackLock (getCallbackLock());

    juce::ScopedNoDenormals noDenormals;

    // ── External-audio capture (Resynth init_audio source) ──────────────────
    // Snapshot the live input bus into the pre-allocated ring BEFORE buffer.clear()
    // wipes the shared in/out buffer. RT-safe: block memcpy + one atomic store, no
    // alloc, no lock. numSamples is always << ring length, so the write wraps at
    // most once.
    if (getTotalNumInputChannels() > 0 && captureRing.getNumSamples() > 0)
    {
        auto inBus = getBusBuffer(buffer, true, 0);
        const int nIn   = inBus.getNumChannels();
        const int ringN = captureRing.getNumSamples();
        const int n     = buffer.getNumSamples();
        if (nIn > 0 && n > 0 && n <= ringN)
        {
            int wp = captureWritePos.load(std::memory_order_relaxed);
            const int first = juce::jmin(n, ringN - wp);
            for (int ch = 0; ch < captureRing.getNumChannels(); ++ch)
            {
                const int src = juce::jmin(ch, nIn - 1);   // mono input spreads to both ring channels
                const float* in = inBus.getReadPointer(src);
                captureRing.copyFrom(ch, wp, in, first);
                if (n > first)
                    captureRing.copyFrom(ch, 0, in + first, n - first);
            }
            wp += n; if (wp >= ringN) wp -= ringN;
            captureWritePos.store(wp, std::memory_order_release);
        }
    }

    buffer.clear();
    pendingSequencerOneShotCount = 0;


    const int numSamples = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();

    // Event Log sample clock: snap this block's absolute start before any tap
    // below uses it, then advance for the next block. Audio thread is the sole
    // writer; relaxed is enough since readers only need an approximate count.
    eventLogBlockStart_ = eventLogTotalSamples_.load(std::memory_order_relaxed);
    eventLogTotalSamples_.fetch_add(static_cast<uint64_t>(numSamples), std::memory_order_relaxed);

    // ── R2: Replay transport ────────────────────────────────────────────────
    // Acquire pairs with startReplay's release store: everything it wrote into
    // replayState_ is visible here. Its own playhead starts at 0 for each tape,
    // independent of the recorder's running sample clock above. The playhead is in
    // TAPE samples and advances by numSamples×rate (Speed control) with a
    // fractional carry so non-integer rates stay drift-free.
    const bool replayActive = replayModeActive_.load(std::memory_order_acquire);
    const float replayRateNow = replayRate_.load(std::memory_order_relaxed);
    uint64_t replayBlockStart = 0, replayAdvance = 0;
    if (replayActive)
    {
        replayRateFrac_ += static_cast<double>(numSamples) * static_cast<double>(replayRateNow);
        replayAdvance    = static_cast<uint64_t>(replayRateFrac_);
        replayRateFrac_ -= static_cast<double>(replayAdvance);
        replayBlockStart = replayPlayhead_.fetch_add(replayAdvance, std::memory_order_relaxed);
    }

    // Live input is neutralised for the duration of the tape: dropping the whole
    // MIDI buffer here takes out external notes, pitch-bend, CC, CC-Learn and both
    // pushStepRecordCandidate sites in one move. (The sequencers and arp are held
    // stopped further down; their notes are already in the log.)
    //
    // Disclosed consequence: incoming MIDI Clock is dropped too, so a patch whose
    // LFO/Drift is set to Clock Sync falls back to internal BPM for the duration of
    // the replay. The tape's own transport is sample-driven and does not need it.
    if (replayActive)
        midiMessages.clear();

    // ── Host-transport snapshot (feeds resolveSyncBpm()) ────────────────────
    {
        bool playing = false;
        if (auto* ph = getPlayHead())
        {
            if (auto pos = ph->getPosition())
            {
                if (pos->getIsPlaying())
                {
                    playing = true;
                    if (auto bpm = pos->getBpm())
                        hostBpmLastSeen.store(static_cast<float>(*bpm),
                                              std::memory_order_relaxed);
                }
            }
        }
        hostPlayingNow.store(playing, std::memory_order_relaxed);
    }

    // ── MIDI Clock: loss detection + tick pre-pass (feeds resolveSyncBpm()) ─
    if (midiClockEnabled_.load(std::memory_order_relaxed))
    {
        // Re-enable edge: reset counters so stale intervals from a previous
        // session don't produce a wrong first post-re-enable tick interval.
        if (!midiClockPrevEnabled_)
        {
            midiClockTickCount_ = 0;
            midiClockLastTick_  = 0;
            midiClockPrevEnabled_ = true;
        }

        // Loss detection: no tick for > 1 second → invalidate.
        if (midiClockValid_.load(std::memory_order_relaxed)
            && midiClockLastTick_ > 0
            && midiClockBlockStart_ > midiClockLastTick_
            && (midiClockBlockStart_ - midiClockLastTick_)
               > static_cast<uint64_t>(getSampleRate()))
        {
            midiClockValid_.store(false, std::memory_order_release);
            midiClockTickCount_ = 0;
        }

        for (const auto& meta : midiMessages)
        {
            const auto msg = meta.getMessage();
            if (msg.isMidiClock())
            {
                const uint64_t tickSample = midiClockBlockStart_
                                          + static_cast<uint64_t>(meta.samplePosition);
                if (midiClockLastTick_ > 0 && tickSample > midiClockLastTick_)
                {
                    const uint32_t interval =
                        static_cast<uint32_t>(tickSample - midiClockLastTick_);
                    midiClockIntervals_[midiClockTickIdx_] = interval;
                    midiClockTickIdx_ = (midiClockTickIdx_ + 1) % 24;
                    if (midiClockTickCount_ < 24) ++midiClockTickCount_;

                    if (midiClockTickCount_ >= 4)
                    {
                        uint64_t sum = 0;
                        for (int i = 0; i < midiClockTickCount_; ++i)
                            sum += midiClockIntervals_[i];
                        const float avg = static_cast<float>(sum)
                                        / static_cast<float>(midiClockTickCount_);
                        const float bpm = 60.0f * static_cast<float>(getSampleRate())
                                        / (avg * 24.0f);
                        midiClockBpm_.store(juce::jlimit(20.0f, 300.0f, bpm),
                                            std::memory_order_release);
                        midiClockValid_.store(true, std::memory_order_release);
                    }
                }
                midiClockLastTick_ = tickSample;
            }
            else if (msg.isMidiStop())
            {
                midiClockValid_.store(false, std::memory_order_release);
                midiClockTickCount_ = 0;
                midiClockLastTick_  = 0;
            }
            else if (msg.isMidiStart())
            {
                midiClockValid_.store(false, std::memory_order_release);
                midiClockTickCount_ = 0;
                midiClockLastTick_  = 0;
            }
            // isMidiContinue: keep accumulating ticks without resetting warm-up
        }
    }
    else
    {
        midiClockPrevEnabled_ = false;  // reset so re-enable is detected next time
    }

    const float syncBpm = resolveSyncBpm();

    // ── MIDI Panic (StatusBar button) ────────────────────────────────────
    // GUI sets the flag from any thread; we consume it once here on the
    // audio thread. Mirrors the CC120/123 path below — release all voices,
    // clear sustain/sostenuto/drone, reset performance controllers.
    if (midiPanicRequested.exchange(false, std::memory_order_acq_rel))
    {
        voiceManager.allNotesOff();
        // Panic must drop what the arp believes is held too, or it keeps
        // arpeggiating into the silence it was just asked to produce. Keys +
        // lead, NOT reset(): reset() would also clear lastPlayedNote, and the
        // note-off the arp still owes for it would never be emitted.
        arpeggiator.allKeysUp();
        arpeggiator.clearSeqLead();
        lastMidiNoteOn.store(false, std::memory_order_relaxed);
    }

    // ── Sync LFO/Drift phase alignment to the sequencer downbeat ────────────
    // A Drift/LFO switched to Sync should START its cycle on the beat (the
    // sequencer's "1"), not wherever the toggle happened to land.
    //   • Transport stop→start: the start instant IS step 0 (the downbeat) →
    //     align phase to 0 immediately, as before.
    //   • Switched Off→Sync WHILE the step sequencer already leads the clock:
    //     snapping now would lock the cycle between beats. Instead ARM the
    //     modulator — held silent at phase 0 (see LFO::setArmed /
    //     DriftLFO::setLfoArmed) — and release it on the next bar downbeat
    //     (where barStartFlag is consumed, below) so its first cycle starts on
    //     the "1". ("ggf. kurz warten bis der Beat kommt.")
    // Scope: only while the STEP seq is the lead (not GEN mode, host not
    // playing) — that is the only transport exposing a bar signal to align to;
    // GEN-engine and host-led playback keep the old immediate behaviour.
    // PID::genSeqRunning is a STEP↔GEN toggle, not transport.
    {
        const bool seqRunNow      = paramCache.seqRunning->load() > 0.5f;
        const bool transportStart = seqRunNow && !lastSeqRunning;
        const bool stepLeads      = seqRunNow && !genModeActiveInAudio
                                    && !hostPlayingNow.load(std::memory_order_relaxed);

        const int lc[3] = { static_cast<int>(paramCache.lfo1ClockMode->load()),
                            static_cast<int>(paramCache.lfo2ClockMode->load()),
                            static_cast<int>(paramCache.lfo3ClockMode->load()) };
        const int dc[3] = { static_cast<int>(paramCache.drift1ClockMode->load()),
                            static_cast<int>(paramCache.drift2ClockMode->load()),
                            static_cast<int>(paramCache.drift3ClockMode->load()) };
        // A generation-side slot frozen for an offline cache take is not running in
        // musical time at all — its phase is the RECORDING position and moves one
        // cadence step per captured entry. Aligning that to a downbeat would throw
        // the take back to the start of its trajectory (and redraw a sample-and-hold
        // value) on a PLAY press, which is a transport action, not a Drift setting:
        // the same take would then record differently depending on whether the
        // sequencer happened to be running. There is nothing to align, so skip it.
        const bool holdGen = driftGenHold_.load(std::memory_order_relaxed);
        const int dt[3] = { static_cast<int>(paramCache.drift1Target->load()),
                            static_cast<int>(paramCache.drift2Target->load()),
                            static_cast<int>(paramCache.drift3Target->load()) };
        LFO* const lfoPtr[3] = { &lfo1, &lfo2, &lfo3 };

        for (int i = 0; i < 3; ++i)
        {
            if (transportStart)
            {
                // Start coincides with the downbeat → align now, clear any arm.
                lfoSyncArmed[i]   = false;
                driftSyncArmed[i] = false;
                lfoPtr[i]->setArmed(false);
                driftLfo.setLfoArmed(i, false);
                if (lc[i] != ClockMode::Off) lfoPtr[i]->reset();
                if (dc[i] != ClockMode::Off && ! (holdGen && DriftLFO::isGenerationTarget(dt[i])))
                    driftLfo.resetLfoPhase(i);
            }
            else
            {
                // Arm on a mid-run Off→Sync edge (only while the step seq leads).
                if (stepLeads && lc[i] != ClockMode::Off && lastLfoClockMode[i] == ClockMode::Off)
                    lfoSyncArmed[i] = true;
                if (stepLeads && dc[i] != ClockMode::Off && lastDriftClockMode[i] == ClockMode::Off)
                    driftSyncArmed[i] = true;

                // Cancel a pending arm if Sync was switched off or the lead was lost.
                if (lc[i] == ClockMode::Off || !stepLeads) lfoSyncArmed[i]   = false;
                if (dc[i] == ClockMode::Off || !stepLeads) driftSyncArmed[i] = false;

                lfoPtr[i]->setArmed(lfoSyncArmed[i]);
                driftLfo.setLfoArmed(i, driftSyncArmed[i]);
            }

            lastLfoClockMode[i]   = lc[i];
            lastDriftClockMode[i] = dc[i];
        }
        lastSeqRunning = seqRunNow;
    }

    updateDriftState(numSamples, syncBpm);

    // ── Idle detection ──────────────────────────────────────────────────────
    // During replay the sequencers and arp are held stopped: the tape already
    // contains every note they emitted, so letting them run would double each one.
    // Overriding the local (rather than writing the APVTS params) keeps the user's
    // patch untouched — a Stop hands back exactly the transport state they had.
    bool seqRunning = paramCache.seqRunning->load() > 0.5f && ! replayActive;
    // A pending sequencer-preset change must keep the block awake for one cycle
    // so the apply further below runs even while stopped — otherwise picking a
    // preset in the dropdown does nothing until playback starts. Cheap (one
    // atomic load + compare) and self-clearing: once applied, lastSeqPreset
    // matches and the block idles again on the next cycle.
    bool seqPresetPending =
        static_cast<int>(paramCache.seqPreset->load()) != lastSeqPreset.load(std::memory_order_relaxed);
    // Read the arp's enable state HERE, above the idle gate, not down at the arp
    // stage: the deep-idle branch returns before the arp stage ever runs, and
    // arpWasEnabled has to be kept honest across that return (see below).
    const int arpModeRaw = static_cast<int>(paramCache.arpMode->load());
    const bool arpEnabled = arpModeRaw > 0 && ! replayActive;   // see seqRunning above
    // An arpeggiating held key is activity even when nothing is sounding YET:
    // computer-keyboard notes never enter midiMessages, and with the arp on they
    // start no voice of their own — so without this term a key pressed while the
    // synth sits in deep idle would return below and the arp would never start.
    // Gated on the arp being on: with it off, a held key sounds a voice, and
    // hasActiveVoices() already covers it (a zero-sustain patch would otherwise
    // keep the block awake for the whole time the key stays down).
    bool arpHoldingKeys = arpEnabled && arpeggiator.hasHeldKeys();
    // A Csound orchestra swap is serviced ONLY inside the synthesis path: the
    // pending→fade transition at the consume site (~:4109) and the fade advance
    // that flips csoundActiveIdx_ (~:4339) both sit inside `if (!skipSynthesis)`,
    // below the deep-idle early return. So a swap that lands while the instrument
    // is silent — a recall/bake with no key held, or a note released before the
    // Regen-XFade crossfade finishes — is never consumed: csoundSwapPending_/
    // csoundSwapFading_ stay set, freeToStartSwap (handleAsyncUpdate) stays false,
    // and every later requestCsoundOrchestra() bumps the generation without ever
    // compiling — the panel latches "compiling..." forever with no internal exit.
    // An outstanding swap therefore counts as activity and keeps the block awake
    // until the EXISTING machinery completes it (a note-free fade mixes silence
    // with silence and is inaudible; it just flips the active engine). Bounded by
    // the crossfade, then the instrument idles again. Gated on Csound mode: a swap
    // requested while another engine is live stays inert until the user switches
    // back (the S-spec's own "inert until switched back" rule) and must never keep
    // a non-Csound session awake — that would be an idle-CPU regression.
    const bool csoundSwapOutstanding =
        static_cast<int>(paramCache.engineMode->load()) == static_cast<int>(EngineMode::Csound)
        && (csoundSwapPending_.load(std::memory_order_acquire)
            || csoundSwapFading_.load(std::memory_order_acquire));
    bool hasActivity = voiceManager.hasActiveVoices()
                       || hasActiveSequencerOneShots()
                       || !midiMessages.isEmpty()
                       || seqRunning
                       || seqPresetPending
                       || arpHoldingKeys
                       || csoundSwapOutstanding
                       || replayActive;   // the tape must never idle out mid-playback

    if (hasActivity)
        silentBlockCount = 0;
    else
        ++silentBlockCount;

    // PHASE 2: Deep idle (tails fully decayed) → buffer already cleared, just return
    if (silentBlockCount > tailBlocks)
    {
        audioIdle.store(true, std::memory_order_relaxed);
        // The master stage below this return is never reached while idle, so the
        // two gain ramps have to be re-seeded HERE or they start the next block
        // from a value that can be ten seconds old. Both now follow the
        // voice-count SWITCH, which is a front-panel button: press "16" on a
        // silent instrument, play a chord, and a stale start value would ramp the
        // onset from the Mono gain -- 15.7 dB too hot for the length of one
        // block. Idle is exactly when a switch gets pressed, so this is the
        // normal case and not an edge one.
        {
            const float g = outputGainForThreshold(paramCache.limiterThresh->load(),
                                                   static_cast<int>(paramCache.voiceCount->load()));
            outputGainPrev_     = g;
            oneShotPreGainPrev_ = kOneShotReferenceGain / juce::jmax(1.0e-6f, g);
        }
        // The arp edges below this return are never evaluated while idle, so the
        // edge state has to track the parameter here — otherwise switching the arp
        // off during idle leaves arpWasEnabled true, and the first block after the
        // next key press fires the off-edge and sounds that key a SECOND time on
        // top of the voice the key press already started.
        arpWasEnabled = arpEnabled;
        // Same reason, for the aftertouch bars. updateAftertouchTraversal also
        // sits below this return, and it is the only writer of atAnyKeyHeld_ -
        // so a key released while the instrument is idle (a computer-keyboard
        // key makes no MIDI and wakes nothing) would never be seen as released.
        // The bars would still be engaged at the next key press, and the first
        // block of that note would land the position pressure 0 resolves to,
        // for a press with no aftertouch in it at all.
        //
        // Not quite "idle means hands off" - a chord with no sustain can decay
        // into idle with the keys still down. Ten seconds of silence ends a
        // gesture either way, and the bar re-arms from wherever the hand is.
        // Once on the way into idle, not on every idle block - the cancels below
        // would otherwise sit permanently raised (see cancelParkedCachePosition).
        if (atAnyKeyHeld_ || atCacheEngaged_ || atSnapEngaged_
            || atCacheZone_ >= 0 || atSnapZone_ >= 0
            || atCacheBaseZone_ >= 0 || atSnapBaseZone_ >= 0)
        {
            atAnyKeyHeld_    = false;
            atCacheZone_     = -1;
            atCacheBaseZone_ = -1;
            atCacheEngaged_  = false;
            atSnapZone_      = -1;
            atSnapBaseZone_  = -1;
            atSnapEngaged_   = false;
            cancelParkedCachePosition();
            cancelParkedSnapSlot();
        }
        // Keep free-running modulators phase-accurate. lastLfoXVal_ must be
        // refreshed here too, not just advanced — updateDriftState() (called
        // every block, including this deep-idle one, since it runs above this
        // return) reads it to modulate a Drift LFO's own Amt when an LFO
        // targets Drift1/2/3Depth. A bare advancePhase() would leave that
        // value frozen at whatever it was when the tail fully decayed, and
        // driftLfo.getOffsetForTarget() (which keeps modulating Alpha/Axis/
        // Noise/Magnitude/Resynth for the generation side "runs even during
        // tail" drift) would inherit that stale constant instead of a live
        // one — for a note-free session that can silently stop that drift
        // dead. bp doesn't exist yet at this point in processBlock, so pull
        // rate/waveform/Amount straight off paramCache (same Clock-Sync
        // resolution as the live path below) before sampling — otherwise the
        // Amount would react live while Rate/Wave/Clock-Sync edits go unheard
        // until a note breaks idle. Mirrors the free-running branch further
        // down (skipSynthesis-but-not-deep-idle) exactly.
        if (numSamples > 0)
        {
            const int c1 = static_cast<int>(paramCache.lfo1ClockMode->load());
            lfo1.setRate(c1 == ClockMode::Off ? paramCache.lfo1Rate->load()
                : ClockSync::computeRate(syncBpm, static_cast<int>(paramCache.lfo1ClockDivision->load())));
            lfo1.setWaveform(static_cast<int>(paramCache.lfo1Wave->load()));
            const int c2 = static_cast<int>(paramCache.lfo2ClockMode->load());
            lfo2.setRate(c2 == ClockMode::Off ? paramCache.lfo2Rate->load()
                : ClockSync::computeRate(syncBpm, static_cast<int>(paramCache.lfo2ClockDivision->load())));
            lfo2.setWaveform(static_cast<int>(paramCache.lfo2Wave->load()));
            const int c3 = static_cast<int>(paramCache.lfo3ClockMode->load());
            lfo3.setRate(c3 == ClockMode::Off ? paramCache.lfo3Rate->load()
                : ClockSync::computeRate(syncBpm, static_cast<int>(paramCache.lfo3ClockDivision->load())));
            lfo3.setWaveform(static_cast<int>(paramCache.lfo3Wave->load()));

            lastLfo1Val_ = lfo1.processSample() * paramCache.lfo1Depth->load();
            lastLfo2Val_ = lfo2.processSample() * paramCache.lfo2Depth->load();
            lastLfo3Val_ = lfo3.processSample() * paramCache.lfo3Depth->load();
            if (numSamples > 1)
            {
                lfo1.advancePhase(numSamples - 1);
                lfo2.advancePhase(numSamples - 1);
                lfo3.advancePhase(numSamples - 1);
            }
        }
        // MIDI Clock sample counter must advance unconditionally — a frozen base
        // would corrupt tick timestamps and break loss detection after idle gaps.
        midiClockBlockStart_ += static_cast<uint64_t>(numSamples);
        return;
    }
    audioIdle.store(false, std::memory_order_relaxed);

    // ── GAIN STAGING ────────────────────────────────────────────────────────
    // Per Voice: Osc +-1.0 → engine trim (EngineCalib) → VCA → Filter
    //            (gain-neutral, reso +12dB)
    // Sum:       N voices * 1/N^0.1 (VoiceManager::updateGainTarget)
    // Post-Sum:  Delay+Reverb up to ~2.7x → Master 0dB max → output gain
    //            (x3.24 Mono .. x0.53 at 16, per the voice-count SWITCH)
    //            → ceiling, STANDALONE only
    //
    // Three numbers here were stale and are corrected rather than carried:
    // the per-voice VCA is `ampEnvVal * prod(1 + Amt_m)` over the mod envelopes
    // pointed at the DCA (SynthVoice.cpp, computeDcaGain), so with all four at
    // Amt 1.0 it reaches x16, not the +-4.0 this line claimed from the two-mod-
    // envelope era; the sum scaling is 1/N^0.1 and not 1/sqrt(N), which is a
    // factor-of-five difference in the exponent; and the master stage is no
    // longer a limiter at all (dsp/Limiter.h).
    //
    // WHERE THE HEADROOM WENT, measured (tools/measure_engine_levels.cpp). The
    // engines were 13.7 dB apart at a single note and none of them was placed
    // against full scale; EngineCalib now matches them, and the output gain is a
    // function of the voice-count SWITCH, set so that a chord filling the
    // selected polyphony lands on the ceiling's knee. Everything a given switch
    // position can play stays below 1.0, and Mono is 15.7 dB louder than 16
    // rather than paying for headroom it never uses.
    //
    // What still does NOT fit is the mod matrix on top of it: with all four mod
    // envelopes pointed at the DCA at Amt 1.0 the VCA alone is x16, and on
    // tools/measure_poly_independence.cpp's deliberately hot patch ONE held
    // note used to leave this chain at 1.86. No master stage can carry that --
    // the old compressor absorbed it, a memoryless ceiling can only flat-top
    // it. Whether the DCA product should be bounded is a separate question
    // about the modulation law, it is NOT decided here, and it is BJ's: it
    // changes the loudness of every preset that stacks mod envelopes on the
    // DCA.
    // ────────────────────────────────────────────────────────────────────────

    // ── Voice count ──────────────────────────────────────────────────────────
    {
        static constexpr int voiceCounts[] = { 1, 4, 6, 8, 12, 16, 64, 128 };
        int vcIdx = static_cast<int>(paramCache.voiceCount->load());
        int effectiveLimit = voiceCounts[juce::jlimit(0, 7, vcIdx)];
        // D1: Csound mode caps effective polyphony at min(voiceLimit, 16) — the
        // Phase-1 orchestra is a fixed 16-instrument/channel affair
        // (CsoundEngine::kMaxVoices), never an always-on-128 variant (idle-CPU
        // regression risk). bp (with its parsed engineMode) doesn't exist yet
        // at this point in the block — read the raw engine-mode choice
        // straight from the paramCache atomic instead.
        if (static_cast<int>(paramCache.engineMode->load()) == static_cast<int>(EngineMode::Csound))
            effectiveLimit = juce::jmin(effectiveLimit, CsoundEngine::kMaxVoices);
        voiceManager.setVoiceLimit(effectiveLimit);
    }

    // ── Tuning table ──────────────────────────────────────────────────────────
    {
        int tuningIdx = static_cast<int>(paramCache.tuning->load());
        int scaleRoot = static_cast<int>(paramCache.scaleRoot->load());
        auto tt = static_cast<Tuning::Type>(juce::jlimit(0, (int)Tuning::COUNT - 1, tuningIdx));
        Tuning::buildTable(tuningTable, tt, scaleRoot);
        voiceManager.setTuningTable(tuningTable);
    }

    // ── Read all parameters into BlockParams ──────────────────────────────────
    BlockParams bp;
    bp.ampAttack  = paramCache.ampAttack->load();
    bp.ampDecay   = paramCache.ampDecay->load();
    bp.ampSustain = paramCache.ampSustain->load();
    bp.ampRelease = paramCache.ampRelease->load();
    bp.ampAmount  = paramCache.ampAmount->load();
    bp.velAmt     = paramCache.velAmt->load();
    bp.ampTarget  = static_cast<int>(paramCache.ampTarget->load());
    bp.ampLoop    = paramCache.ampLoop->load() > 0.5f;
    bp.ampAttackBend  = paramCache.ampAttackCurve->load();
    bp.ampDecayBend   = paramCache.ampDecayCurve->load();
    bp.ampReleaseBend = paramCache.ampReleaseCurve->load();
    bp.ampAttackVelSens  = paramCache.ampAttackVelSens->load();
    bp.ampDecayVelSens   = paramCache.ampDecayVelSens->load();
    bp.ampReleaseVelSens = paramCache.ampReleaseVelSens->load();

    for (int i = 0; i < kNumModEnvs; ++i)
    {
        const auto& src = paramCache.modEnv[i];
        auto& dst = bp.modEnv[i];
        dst.attack  = src.attack->load();
        dst.decay   = src.decay->load();
        dst.sustain = src.sustain->load();
        dst.release = src.release->load();
        dst.amount  = src.amount->load();
        dst.target  = static_cast<int>(src.target->load());
        dst.loop    = src.loop->load() > 0.5f;
        dst.attackBend  = src.attackCurve->load();
        dst.decayBend   = src.decayCurve->load();
        dst.releaseBend = src.releaseCurve->load();
        dst.attackVelSens  = src.attackVelSens->load();
        dst.decayVelSens   = src.decayVelSens->load();
        dst.releaseVelSens = src.releaseVelSens->load();
    }


    // LFOs (global) — Clock-Sync override: when ClockMode::Sync, the rate
    // displayed on the slider is replaced by the sync-derived rate. We store
    // the effective rate back into `bp.lfo*Rate` so downstream env→LFO-rate
    // modulation (lines further down) scales relative to the sync rate.
    {
        const int   c1 = static_cast<int>(paramCache.lfo1ClockMode->load());
        const float r1 = paramCache.lfo1Rate->load();
        bp.lfo1Rate = (c1 == ClockMode::Off) ? r1
            : ClockSync::computeRate(syncBpm,
                static_cast<int>(paramCache.lfo1ClockDivision->load()));
        bp.lfo1Depth = paramCache.lfo1Depth->load();
        bp.lfo1Wave = static_cast<int>(paramCache.lfo1Wave->load());
        bp.lfo1TrigMode = static_cast<int>(paramCache.lfo1Mode->load()) == LfoMode::Trigger;
        lfo1.setRate(bp.lfo1Rate);
        lfo1.setDepth(1.0f);
        lfo1.setWaveform(bp.lfo1Wave);
        bp.lfo1Target = static_cast<int>(paramCache.lfo1Target->load());
    }
    {
        const int   c2 = static_cast<int>(paramCache.lfo2ClockMode->load());
        const float r2 = paramCache.lfo2Rate->load();
        bp.lfo2Rate = (c2 == ClockMode::Off) ? r2
            : ClockSync::computeRate(syncBpm,
                static_cast<int>(paramCache.lfo2ClockDivision->load()));
        bp.lfo2Depth = paramCache.lfo2Depth->load();
        bp.lfo2Wave = static_cast<int>(paramCache.lfo2Wave->load());
        bp.lfo2TrigMode = static_cast<int>(paramCache.lfo2Mode->load()) == LfoMode::Trigger;
        lfo2.setRate(bp.lfo2Rate);
        lfo2.setDepth(1.0f);
        lfo2.setWaveform(bp.lfo2Wave);
        bp.lfo2Target = static_cast<int>(paramCache.lfo2Target->load());
    }
    {
        const int   c3 = static_cast<int>(paramCache.lfo3ClockMode->load());
        const float r3 = paramCache.lfo3Rate->load();
        bp.lfo3Rate = (c3 == ClockMode::Off) ? r3
            : ClockSync::computeRate(syncBpm,
                static_cast<int>(paramCache.lfo3ClockDivision->load()));
        bp.lfo3Depth = paramCache.lfo3Depth->load();
        bp.lfo3Wave = static_cast<int>(paramCache.lfo3Wave->load());
        bp.lfo3TrigMode = static_cast<int>(paramCache.lfo3Mode->load()) == LfoMode::Trigger;
        lfo3.setRate(bp.lfo3Rate);
        lfo3.setDepth(1.0f);
        lfo3.setWaveform(bp.lfo3Wave);
        bp.lfo3Target = static_cast<int>(paramCache.lfo3Target->load());
    }

    {
        auto setAmt = [&](const std::atomic<float>* p, int target) {
            bp.aftertouchTargetAmt[target] = p->load();
        };
        setAmt(paramCache.aftertouchAmtLfo1Depth,   AftertouchTarget::LFO1Depth);
        setAmt(paramCache.aftertouchAmtLfo2Depth,   AftertouchTarget::LFO2Depth);
        setAmt(paramCache.aftertouchAmtLfo3Depth,   AftertouchTarget::LFO3Depth);
        setAmt(paramCache.aftertouchAmtEnv1Sustain, AftertouchTarget::Env1Sustain);
        setAmt(paramCache.aftertouchAmtEnv2Sustain, AftertouchTarget::Env2Sustain);
        setAmt(paramCache.aftertouchAmtEnv3Sustain, AftertouchTarget::Env3Sustain);
        setAmt(paramCache.aftertouchAmtCutoff,      AftertouchTarget::Cutoff);
        setAmt(paramCache.aftertouchAmtResonance,   AftertouchTarget::Resonance);
        setAmt(paramCache.aftertouchAmtScan,        AftertouchTarget::Scan);
        setAmt(paramCache.aftertouchAmtDca,         AftertouchTarget::DCA);
        setAmt(paramCache.aftertouchAmtPitch,       AftertouchTarget::Pitch);
        setAmt(paramCache.aftertouchAmtNoiseLevel,  AftertouchTarget::NoiseLevel);
        setAmt(paramCache.aftertouchAmtEnv4Sustain, AftertouchTarget::Env4Sustain);
        setAmt(paramCache.aftertouchAmtEnv5Sustain, AftertouchTarget::Env5Sustain);
        setAmt(paramCache.aftertouchAmtCache,        AftertouchTarget::Cache);
        setAmt(paramCache.aftertouchAmtSnap,         AftertouchTarget::Snap);

        auto setSrc = [&](const std::atomic<float>* p, int target) {
            bp.aftertouchTargetSrc[static_cast<size_t>(target)] =
                juce::jlimit(0, ExprSource::kCount - 1,
                             static_cast<int>(p->load()));
        };
        setSrc(paramCache.exprSrcLfo1Depth,   AftertouchTarget::LFO1Depth);
        setSrc(paramCache.exprSrcLfo2Depth,   AftertouchTarget::LFO2Depth);
        setSrc(paramCache.exprSrcLfo3Depth,   AftertouchTarget::LFO3Depth);
        setSrc(paramCache.exprSrcEnv1Sustain, AftertouchTarget::Env1Sustain);
        setSrc(paramCache.exprSrcEnv2Sustain, AftertouchTarget::Env2Sustain);
        setSrc(paramCache.exprSrcEnv3Sustain, AftertouchTarget::Env3Sustain);
        setSrc(paramCache.exprSrcCutoff,      AftertouchTarget::Cutoff);
        setSrc(paramCache.exprSrcResonance,   AftertouchTarget::Resonance);
        setSrc(paramCache.exprSrcScan,        AftertouchTarget::Scan);
        setSrc(paramCache.exprSrcDca,         AftertouchTarget::DCA);
        setSrc(paramCache.exprSrcPitch,       AftertouchTarget::Pitch);
        setSrc(paramCache.exprSrcNoiseLevel,  AftertouchTarget::NoiseLevel);
        setSrc(paramCache.exprSrcEnv4Sustain, AftertouchTarget::Env4Sustain);
        setSrc(paramCache.exprSrcEnv5Sustain, AftertouchTarget::Env5Sustain);
        setSrc(paramCache.exprSrcCache,       AftertouchTarget::Cache);
        setSrc(paramCache.exprSrcSnap,        AftertouchTarget::Snap);
    }

    // The two targets that move the instrument rather than a voice. Resolved
    // here, once per block, from the same pressure the voices are reading.
    updateAftertouchTraversal(bp);

    // Filter
    // filter_type: 0=Off, 1=LP, 2=HP, 3=BP → filterEnabled from type, DSP type is 0-based
    {
        int ft = static_cast<int>(paramCache.filterType->load());
        bp.filterEnabled = (ft > 0);
        bp.filterType = ft > 0 ? ft - 1 : 0;  // 0=LP, 1=HP, 2=BP for DSP
    }
    bp.baseCutoff = paramCache.filterCutoff->load();
    bp.baseReso = paramCache.filterResonance->load();
    bp.filterSlope = static_cast<int>(paramCache.filterSlope->load());
    bp.filterMix = paramCache.filterMix->load();
    bp.kbdTrack = paramCache.filterKbdTrack->load();
    bp.filterDriveDb = paramCache.filterDrive->load();
    bp.filterDriveOs = static_cast<int>(paramCache.filterDriveOs->load());
    bp.filterAlgorithm = static_cast<int>(paramCache.filterAlgorithm->load());
    bp.filterWarpStyle = static_cast<int>(paramCache.filterWarpStyle->load());
    bp.filterOsFactor = filterOsFactor_.load(std::memory_order_relaxed);  // global, not per-preset
    bp.filterDriveGain = std::pow(10.0f, bp.filterDriveDb * (1.0f / 20.0f));

    // Scan
    bp.baseScan = paramCache.oscScan->load();

    // Octave shift (APVTS choice 0-4, maps to -2..+2)
    bp.octaveShift = static_cast<int>(paramCache.oscOctave->load()) - 2;

    // Noise oscillator
    bp.noiseLevel = paramCache.noiseLevel->load();
    bp.noiseType = static_cast<int>(paramCache.noiseType->load());

    bp.driftCrossfade = paramCache.driftCrossfade->load();

    // Wavetable smooth
    bp.wtSmooth = paramCache.wtSmooth->load() > 0.5f;
    bp.wtAutoScan = paramCache.wtAutoScan->load() > 0.5f;
    bp.freezeTexture = juce::jlimit(static_cast<int>(FreezeTexture::Hold),
                                    static_cast<int>(FreezeTexture::Cloud),
                                    static_cast<int>(paramCache.freezeTexture->load()));
    bp.freezeStereo = juce::jlimit(0.0f, 1.0f,
                                   paramCache.freezeStereo->load());

    // Engine mode — read directly from APVTS (0=Sampler, 1=Wavetable, 2=Granular, 3=LCO, 4=Csound)
    int engineModeRaw = static_cast<int>(paramCache.engineMode->load());
    bp.engineMode = juce::jlimit(static_cast<int>(EngineMode::Sampler),
                                 static_cast<int>(EngineMode::Csound),
                                 engineModeRaw);
    // LCO maps to the SAME Wavetable DSP path — it's a Wavetable bake with a
    // distinct preset identity, not a distinct SynthVoice engine. Csound is
    // NOT scan-driven (spec §2 item 8) — it stays out of both flags below,
    // same as Sampler.
    bp.engineIsWavetable = (bp.engineMode == EngineMode::Wavetable || bp.engineMode == EngineMode::Lco);
    bp.engineIsFreeze = (bp.engineMode == EngineMode::Freeze);
    switch (bp.engineMode)
    {
        case EngineMode::Wavetable:
        case EngineMode::Lco:
            voiceManager.setEngineMode(SynthVoice::EngineMode::Wavetable);
            break;
        case EngineMode::Freeze:
            voiceManager.setEngineMode(SynthVoice::EngineMode::Freeze);
            break;
        case EngineMode::Csound:
            voiceManager.setEngineMode(SynthVoice::EngineMode::Csound);
            break;
        case EngineMode::Sampler:
        default:
            voiceManager.setEngineMode(SynthVoice::EngineMode::Sampler);
            break;
    }

    // Master volume (dB → linear)
    float masterDb = paramCache.masterVol->load();
    float masterGain = juce::Decibels::decibelsToGain(masterDb);

    // Apply drift offsets to their respective targets
    bp.driftScanOffset   = driftLfo.getOffsetForTarget(DriftLFO::TgtWtScan);
    bp.driftFilterOffset = driftLfo.getOffsetForTarget(DriftLFO::TgtFilter);
    bp.driftPitchOffset  = driftLfo.getOffsetForTarget(DriftLFO::TgtPitch);
    // Phase-independent, for decisions a voice must make once at note start
    // rather than from wherever the drift waveform happens to sit.
    bp.driftPitchReach   = driftLfo.getReachForTarget(DriftLFO::TgtPitch);
    // Block-level drift targets (delay/reverb) applied after modDelayTime etc. are declared

    // Drift → envelope amounts (additive, clamped to 0–1)
    bp.ampAmount  = juce::jlimit(0.0f, 1.0f, bp.ampAmount  + driftLfo.getOffsetForTarget(DriftLFO::TgtEnv1Amt));
    // Drift's ENV n Amt targets are 1-based over ALL FIVE envelopes: ENV1 is the
    // amp envelope, so modEnv[i] is ENV (i+2) and its drift target is
    // TgtEnv2Amt + i.
    for (int i = 0; i < kNumModEnvs; ++i)
        bp.modEnv[i].amount = juce::jlimit(0.0f, 1.0f, bp.modEnv[i].amount
            + driftLfo.getOffsetForTarget(static_cast<DriftLFO::Target>(DriftLFO::TgtEnv2Amt + i)));

    // Give noteOn/noteOff access to the current envelope/modulation block state.
    voiceManager.setBlockParams(bp);

    // ── Sequencer / Arpeggiator (in series: Seq → Arp → synth) ─────────────
    // (seqRunning already read above for idle detection)
    float seqBpm = paramCache.seqBpm->load();
    int seqSteps = static_cast<int>(paramCache.seqSteps->load());
    int seqOctaveShift = static_cast<int>(paramCache.seqOctave->load()) - 2;
    float seqGate = paramCache.seqGate->load();
    float seqShuffle = paramCache.seqShuffle->load();
    int seqPreset = static_cast<int>(paramCache.seqPreset->load());
    // (arpModeRaw / arpEnabled already read above for idle detection)
    int arpMode = arpModeRaw > 0 ? arpModeRaw - 1 : 0; // 0=Up,1=Down,2=UpDown,3=Random
    int arpRate = static_cast<int>(paramCache.arpRate->load());
    int arpOctaves = static_cast<int>(paramCache.arpOctaves->load());

    // Internal note events for this block (sequencers + arp). Typed, never MIDI.
    // Cleared here (capacity retained — no allocation) before any source writes.
    internalNoteEvents_.clear();

    // ── R2: inject this block's replay notes ────────────────────────────────
    // They enter the same stream the sequencers write to, so the sort + merge-walk
    // below dispatch them to voiceManager exactly as if a sequencer had emitted
    // them. External-MIDI notes in the log come through here too: they lose their
    // MPE channel (VoiceEvent has none), which is the one disclosed fidelity gap.
    // Bounded by the reserved capacity — push_back must never allocate here.
    if (replayActive)
    {
        const uint64_t blockEnd = replayBlockStart + replayAdvance;
        const auto& notes = replayState_.noteEvents;
        while (replayState_.nextNoteIdx < notes.size()
               && internalNoteEvents_.size() < internalNoteEvents_.capacity())
        {
            const auto& n = notes[replayState_.nextNoteIdx];
            if (n.timestamp >= blockEnd)
                break;
            // Events already behind the playhead (a tape that starts mid-note, or a
            // block-size change) land at offset 0 rather than being dropped. The
            // tape-samples delta maps back into DEVICE samples via ÷rate (the block
            // covers `replayAdvance` tape samples across `numSamples` device samples).
            const uint64_t delta = n.timestamp > replayBlockStart ? n.timestamp - replayBlockStart : 0;

            VoiceEvent ev;
            ev.sampleOffset = juce::jlimit(0, numSamples - 1,
                static_cast<int>(static_cast<double>(delta) / static_cast<double>(replayRateNow)));
            ev.type         = n.type;
            ev.note         = n.note;
            ev.velocity     = n.velocity;
            ev.artic        = n.artic;
            ev.strandId     = n.strandId;
            ev.pan          = n.pan;
            internalNoteEvents_.push_back(ev);
            ++replayState_.nextNoteIdx;
        }

        // Arm the next logged generation once the playhead reaches it. At most one
        // is armed at a time (the message thread clears busy when it completes), so
        // a slow generation delays the following one instead of dropping it — the
        // same "one in flight" rule the live drift-regen loop follows.
        const auto& gens = replayState_.generationEvents;
        if (replayState_.nextGenIdx < gens.size()
            && ! replayGenerationBusy_.load(std::memory_order_acquire)
            && replayDueGenerationId_.load(std::memory_order_relaxed) == 0
            && gens[replayState_.nextGenIdx].timestamp < blockEnd)
        {
            replayDueGenerationId_.store(replayState_.nextGenIdx + 1, std::memory_order_release);
            ++replayState_.nextGenIdx;
        }
    }

    // Arp false→true edge: the active sequencer's currently-sounding note was
    // emitted direct-to-synth last block, and from this block on the arp will
    // swallow all seq note-offs — so flush that single note now before the
    // engines run. Manual keyboard notes stay untouched.
    if (arpEnabled && !arpWasEnabled)
    {
        if (genModeActiveInAudio)
            generativeSequencer.allNotesOff(internalNoteEvents_);
        else
            stepSequencer.allNotesOff(internalNoteEvents_);

        // Keys already down were sounding as ordinary voices; from this block on
        // the arp plays them instead. Release those voices or they drone under
        // the arpeggio — forced, because a plain note-off on an external key with
        // the sustain pedal down only MARKS the voice sustained and leaves it
        // ringing, which is exactly the drone this edge exists to prevent.
        for (const auto& heldKey : arpeggiator.getHeldKeys())
            voiceManager.noteOff(heldKey.note, heldKey.sourceId, /*forceRelease=*/true);
    }

    // Arp true→false edge: the mirror image. Keys still down were feeding the arp
    // and sounding nothing of their own — hand them to the voices now, with the
    // source id their eventual key-up will use, or they stay silent until released.
    // NOT on a replay start: replay also clears arpEnabled, and handing the keys
    // to the voices there would play a held chord on top of the tape for its whole
    // length — the very thing beginComputerKeyboardNote's replay gate prevents.
    //
    // Queued as internal events at offset 0, NOT called on voiceManager directly:
    // the arp still owes a note-off for its own sounding note, and the arp-off
    // branch below pushes it at offset 0 too. That note-off carries strandId -1,
    // which matches a voice of that pitch from ANY source — and with Octaves 1 the
    // arp's note IS the held key, so a direct hand-back would be killed by it
    // inside this same block. Going through the stream puts both under the sort's
    // NoteOff-before-NoteOn tiebreak, which is exactly the order needed. The key's
    // MPE channel rides along (VoiceEvent::mpeChannel) so an external key keeps its
    // per-note expression AND stays out of the internal channel-0 bucket that a
    // step-seq slide may hijack.
    if (!arpEnabled && arpWasEnabled && !replayActive)
    {
        for (const auto& heldKey : arpeggiator.getHeldKeys())
        {
            if (internalNoteEvents_.size() >= internalNoteEvents_.capacity())
                break;   // never allocate on the audio thread
            internalNoteEvents_.push_back({ 0, VoiceEvent::Type::NoteOn,
                                            heldKey.note, heldKey.velocity,
                                            VoiceEvent::Articulation::Normal,
                                            heldKey.sourceId, /*pan=*/0.0f,
                                            heldKey.mpeChannel });
        }
    }

    // Preset change detection
    const int prevSeqPreset = lastSeqPreset.load(std::memory_order_relaxed);
    if (seqPreset != prevSeqPreset)
    {
        stepSequencer.loadPreset(seqPreset);
        lastSeqPreset.store(seqPreset, std::memory_order_relaxed);

        // Adopt the preset's length into the seqSteps param on a genuine preset
        // application — fresh-startup default OR a runtime dropdown change — but
        // NOT on the first apply after a DAW state-restore. getStateInformation
        // persists seqPreset and seqSteps independently, so a restored session
        // can legitimately hold a hand-set step count (e.g. 24); forcing the
        // preset's natural length there would silently destroy the saved count on
        // every reload. setStateInformation sets seqStateRestored, which we
        // consume exactly once here: true → restore, skip the push (and clear the
        // flag so the *next* genuine change pushes again); false → fresh start or
        // runtime change, adopt the preset length. (The earlier prevSeqPreset>=0
        // test conflated fresh-startup with restore — both land at -1 — which was
        // harmless while every preset was 16 = the seqSteps default, but a 32-step
        // preset must push its length at startup or it'd be truncated back to 16.)
        // The push sticks because the non-GEN branch re-asserts setNumSteps(
        // seqSteps) every block and the GUI reads this param, not the sequencer;
        // we update the local copy so this block already uses it.
        if (! seqStateRestored.exchange(false, std::memory_order_relaxed))
        {
            seqSteps = stepSequencer.getNumSteps();
            if (auto* par = parameters.getParameter(PID::seqSteps))
                par->setValueNotifyingHost(par->convertTo0to1(static_cast<float>(seqSteps)));
        }
    }

    // GEN mode toggle — PLAY is master transport, GEN switches engine
    bool genModeWanted = paramCache.genSeqRunning->load() > 0.5f;
    int seqDivision = static_cast<int>(paramCache.seqDivision->load());

    // Detect mode switch: apply at step 0 boundary
    if (genModeWanted != genModeActiveInAudio)
    {
        // Check if either sequencer is at step 0 (or not running yet)
        bool atBarBoundary = !seqRunning
            || stepSequencer.getCurrentStep() == 0
            || generativeSequencer.currentStepForGui.load(std::memory_order_relaxed) <= 0;

        if (atBarBoundary)
        {
            if (genModeWanted)
            {
                // Step → Gen: seed from current step seq pattern, then switch
                int seqCount = stepSequencer.getNumSteps();
                int seedNotes[T5ynthStepSequencer::MAX_STEPS]{};
                bool seedEnabled[T5ynthStepSequencer::MAX_STEPS]{};
                int pulseCount = 0;
                for (int i = 0; i < seqCount; ++i)
                {
                    const auto& step = stepSequencer.getStep(i);
                    seedNotes[i] = step.note;
                    seedEnabled[i] = step.enabled;
                    if (step.enabled) pulseCount++;
                }

                // Flush step-seq's currently-sounding note before the gen-seq
                // takes over — avoids a hanging voice across the engine swap.
                stepSequencer.allNotesOff(internalNoteEvents_);
                stopSequencerOneShots();
                stepSequencer.stop();
                generativeSequencer.setBpm(static_cast<double>(seqBpm));
                generativeSequencer.setDivision(seqDivision);
                generativeSequencer.setGate(seqGate);
                generativeSequencer.setShuffle(seqShuffle);
                generativeSequencer.setScale(
                    static_cast<int>(paramCache.scaleType->load()),
                    static_cast<int>(paramCache.scaleRoot->load()));
                generativeSequencer.setPrimaryTransposeSemitones(seqOctaveShift * 12);
                generativeSequencer.seedFromSteps(seedNotes, seedEnabled, seqCount);

                // Update gen params to match seeded pattern
                if (auto* par = parameters.getParameter(PID::genSteps))
                    par->setValueNotifyingHost(par->convertTo0to1(static_cast<float>(seqCount)));
                if (auto* par = parameters.getParameter(PID::genPulses))
                    par->setValueNotifyingHost(par->convertTo0to1(static_cast<float>(pulseCount)));

                lastGenSteps = lastGenPulses = lastGenRotation = -1;
                lastGenMutation = -1.0f;
                genModeActiveInAudio = true;
            }
            else
            {
                // Gen → Step: copy generated pattern into step data, then switch
                int genSteps = generativeSequencer.numStepsForGui.load(std::memory_order_relaxed);
                if (genSteps > 0)
                {
                    stepSequencer.setNumSteps(genSteps);
                    if (auto* par = parameters.getParameter(PID::seqSteps))
                        par->setValueNotifyingHost(par->convertTo0to1(static_cast<float>(genSteps)));
                    for (int i = 0; i < genSteps; ++i)
                    {
                        int note = generativeSequencer.notePatternForGui[static_cast<size_t>(i)]
                            .load(std::memory_order_relaxed);
                        if (note > 0)
                        {
                            stepSequencer.setStepNote(i, note);
                            stepSequencer.setStepEnabled(i, true);
                        }
                        else
                        {
                            stepSequencer.setStepEnabled(i, false);
                        }
                    }
                }
                // Flush every gen-seq strand's sounding note before handing
                // back to the step-seq — avoids hanging voices across the swap.
                generativeSequencer.allNotesOff(internalNoteEvents_);
                generativeSequencer.stop();
                genModeActiveInAudio = false;
            }
        }
    }

    // Configure + run the active engine
    if (genModeActiveInAudio)
    {
        generativeSequencer.setBpm(static_cast<double>(seqBpm));
        generativeSequencer.setDivision(seqDivision);
        generativeSequencer.setGate(seqGate);
        generativeSequencer.setShuffle(seqShuffle);
        generativeSequencer.setPrimaryTransposeSemitones(seqOctaveShift * 12);

        // Fix flags
        bool fxS = paramCache.genFixSteps->load() > 0.5f;
        bool fxP = paramCache.genFixPulses->load() > 0.5f;
        bool fxR = paramCache.genFixRotation->load() > 0.5f;
        bool fxM = paramCache.genFixMutation->load() > 0.5f;
        generativeSequencer.setFixSteps(fxS);
        generativeSequencer.setFixPulses(fxP);
        generativeSequencer.setFixRotation(fxR);
        generativeSequencer.setFixMutation(fxM);

        // Steps/Pulses/Rotation: if FIXED, overwrite every block.
        // If UNFIXED, only overwrite when user changes the slider.
        {
            int gs = static_cast<int>(paramCache.genSteps->load());
            int gp = static_cast<int>(paramCache.genPulses->load());
            int gr = static_cast<int>(paramCache.genRotation->load());

            if (fxS || gs != lastGenSteps)    { generativeSequencer.setSteps(gs);    lastGenSteps = gs; }
            if (fxP || gp != lastGenPulses)   { generativeSequencer.setPulses(gp);   lastGenPulses = gp; }
            if (fxR || gr != lastGenRotation) { generativeSequencer.setRotation(gr); lastGenRotation = gr; }
        }

        // Mutation: always update base; only overwrite effective rate if fixed or user changed slider
        {
            float gm = paramCache.genMutation->load();
            generativeSequencer.setBaseMutation(gm);
            if (fxM || gm != lastGenMutation) { generativeSequencer.setMutation(gm); lastGenMutation = gm; }
        }

        generativeSequencer.setRange(static_cast<int>(paramCache.genRange->load()) + 1);
        generativeSequencer.setScale(
            static_cast<int>(paramCache.scaleType->load()),
            static_cast<int>(paramCache.scaleRoot->load()));

        // ── Shared pitch-field setters ──
        generativeSequencer.setFieldMode(static_cast<int>(
            paramCache.genFieldMode->load()));
        generativeSequencer.setFieldChangeRate(static_cast<int>(
            paramCache.genFieldRate->load()));
        // Field Center PC follows Scale Root — they are conceptually the
        // same anchor, and a separate "second tonic" dropdown was confusing.
        const int scaleRootForField = static_cast<int>(
            paramCache.scaleRoot->load());
        generativeSequencer.setFieldCenterPc(scaleRootForField);

        // Field Pivot interval is derived from Scale Type — its musical
        // character chooses one of three universal pivots: P5 for major
        // flavours, m3 for minor flavours, Tri for symmetric / sharp-fourth
        // scales. Pivot mode then transposes the pc-set by this interval.
        {
            const int scaleTypeForField = static_cast<int>(
                paramCache.scaleType->load());
            int pivotSemitones = 7;  // P5 default
            switch (scaleTypeForField)
            {
                case ScaleType::WhlT: case ScaleType::Locr:
                case ScaleType::HunM: case ScaleType::Lyd:
                case ScaleType::Hjz:
                    pivotSemitones = 6; break;   // Tri
                case ScaleType::Min:  case ScaleType::Dor:  case ScaleType::Phry:
                case ScaleType::Harm: case ScaleType::MelM: case ScaleType::MinP:
                case ScaleType::Blu:  case ScaleType::Hira: case ScaleType::InSn:
                case ScaleType::Iwat: case ScaleType::Kumo: case ScaleType::Ryuk:
                case ScaleType::DblH: case ScaleType::Todi: case ScaleType::Purv:
                case ScaleType::Pers: case ScaleType::NeaM:
                    pivotSemitones = 3; break;   // m3
                default:
                    pivotSemitones = 7; break;   // P5
            }
            generativeSequencer.setFieldPivotInterval(pivotSemitones);
        }

        // ── Inter-strand coordination ──
        generativeSequencer.setCoordinationMode(static_cast<int>(
            paramCache.genCoordinationMode->load()));
        generativeSequencer.setCoordinationCap(static_cast<int>(
            paramCache.genCoordinationCap->load()));

        // ── Per-strand setters (0..4) ──
        {
            struct StrandPIDs {
                const char* enable;
                const char* role;
                const char* octave;
                const char* divMult;
                const char* dominance;
                const char* steps;
                const char* pulses;
                const char* rotation;
                const char* mutation;
                const char* fixSteps;
                const char* fixPulses;
                const char* fixRotation;
                const char* fixMutation;
            };
            static const StrandPIDs kStrands[5] = {
                { nullptr,         PID::genRole,  PID::genOctave,  PID::genDivMult,  PID::genDominance,
                  PID::genSteps,   PID::genPulses, PID::genRotation, PID::genMutation,
                  PID::genFixSteps, PID::genFixPulses, PID::genFixRotation, PID::genFixMutation },
                { PID::gen2Enable, PID::gen2Role, PID::gen2Octave, PID::gen2DivMult, PID::gen2Dominance,
                  PID::gen2Steps,  PID::gen2Pulses, PID::gen2Rotation, PID::gen2Mutation,
                  PID::gen2FixSteps, PID::gen2FixPulses, PID::gen2FixRotation, PID::gen2FixMutation },
                { PID::gen3Enable, PID::gen3Role, PID::gen3Octave, PID::gen3DivMult, PID::gen3Dominance,
                  PID::gen3Steps,  PID::gen3Pulses, PID::gen3Rotation, PID::gen3Mutation,
                  PID::gen3FixSteps, PID::gen3FixPulses, PID::gen3FixRotation, PID::gen3FixMutation },
                { PID::gen4Enable, PID::gen4Role, PID::gen4Octave, PID::gen4DivMult, PID::gen4Dominance,
                  PID::gen4Steps,  PID::gen4Pulses, PID::gen4Rotation, PID::gen4Mutation,
                  PID::gen4FixSteps, PID::gen4FixPulses, PID::gen4FixRotation, PID::gen4FixMutation },
                { PID::gen5Enable, PID::gen5Role, PID::gen5Octave, PID::gen5DivMult, PID::gen5Dominance,
                  PID::gen5Steps,  PID::gen5Pulses, PID::gen5Rotation, PID::gen5Mutation,
                  PID::gen5FixSteps, PID::gen5FixPulses, PID::gen5FixRotation, PID::gen5FixMutation }
            };

            for (int i = 0; i < 5; ++i)
            {
                const auto& ids = kStrands[i];

                if (ids.enable != nullptr)
                {
                    const bool en = parameters.getRawParameterValue(ids.enable)->load() > 0.5f;
                    generativeSequencer.setStrandEnabled(i, en);
                }

                generativeSequencer.setStrandRole(i, static_cast<int>(
                    parameters.getRawParameterValue(ids.role)->load()));

                if (i == 0)
                {
                    // Strand 0 is the legacy mono-gen voice: preserve its
                    // original melody/rhythm and let only Seq Octave transpose it.
                    generativeSequencer.setStrandOctave(i, 0);
                    generativeSequencer.setStrandDivMult(i, 1.0f);
                    generativeSequencer.setStrandDominance(i, 0.0f);
                    continue;
                }

                generativeSequencer.setStrandOctave(i, static_cast<int>(
                    parameters.getRawParameterValue(ids.octave)->load()));
                {
                    int dIdx = juce::jlimit(0, StrandDivMult::kCount - 1, static_cast<int>(
                        parameters.getRawParameterValue(ids.divMult)->load()));
                    generativeSequencer.setStrandDivMult(i, StrandDivMult::kFactor[dIdx]);
                }
                generativeSequencer.setStrandDominance(i, static_cast<float>(
                    parameters.getRawParameterValue(ids.dominance)->load()));

                const bool sFix = parameters.getRawParameterValue(ids.fixSteps   )->load() > 0.5f;
                const bool pFix = parameters.getRawParameterValue(ids.fixPulses  )->load() > 0.5f;
                const bool rFix = parameters.getRawParameterValue(ids.fixRotation)->load() > 0.5f;
                const bool mFix = parameters.getRawParameterValue(ids.fixMutation)->load() > 0.5f;
                generativeSequencer.setStrandFixSteps   (i, sFix);
                generativeSequencer.setStrandFixPulses  (i, pFix);
                generativeSequencer.setStrandFixRotation(i, rFix);
                generativeSequencer.setStrandFixMutation(i, mFix);

                const int gs = static_cast<int>(parameters.getRawParameterValue(ids.steps   )->load());
                const int gp = static_cast<int>(parameters.getRawParameterValue(ids.pulses  )->load());
                const int gr = static_cast<int>(parameters.getRawParameterValue(ids.rotation)->load());
                generativeSequencer.setStrandSteps   (i, gs);
                generativeSequencer.setStrandPulses  (i, gp);
                generativeSequencer.setStrandRotation(i, gr);

                const float gm = parameters.getRawParameterValue(ids.mutation)->load();
                generativeSequencer.setStrandBaseMutation(i, gm);
                generativeSequencer.setStrandMutation    (i, gm);
            }
        }

        if (seqRunning)
            generativeSequencer.start();
        else
            generativeSequencer.stop();
        const size_t eventLogGenSeqBefore_ = internalNoteEvents_.size();
        generativeSequencer.processBlock(buffer, internalNoteEvents_);
        if (eventLogRecordingActive())
            logInternalNoteEventsFrom(eventLogGenSeqBefore_, NoteEventLogEntry::Source::GenerativeSequencer,
                                     seqRunning && arpEnabled, /*genModeForSkip=*/true);

        // Write effective (post-drift) values back to APVTS so sliders move
        {
            int effS = generativeSequencer.effectiveStepsForGui.load(std::memory_order_relaxed);
            int effP = generativeSequencer.effectivePulsesForGui.load(std::memory_order_relaxed);
            float effM = generativeSequencer.effectiveMutationForGui.load(std::memory_order_relaxed);

            if (!fxS && effS != lastGenSteps)
            {
                if (auto* par = parameters.getParameter(PID::genSteps))
                    par->setValueNotifyingHost(par->convertTo0to1(static_cast<float>(effS)));
                lastGenSteps = effS;
            }
            if (!fxP && effP != lastGenPulses)
            {
                if (auto* par = parameters.getParameter(PID::genPulses))
                    par->setValueNotifyingHost(par->convertTo0to1(static_cast<float>(effP)));
                lastGenPulses = effP;
            }
            if (!fxM && effM != lastGenMutation)
            {
                if (auto* par = parameters.getParameter(PID::genMutation))
                    par->setValueNotifyingHost(par->convertTo0to1(effM));
                lastGenMutation = effM;
            }
        }
    }
    else
    {
        stepSequencer.setBpm(static_cast<double>(seqBpm));
        stepSequencer.setNumSteps(seqSteps);
        stepSequencer.setDivision(seqDivision);
        stepSequencer.setShuffle(seqShuffle);
        stepSequencer.setAllGates(seqGate);
        int seqOctaveIdx = static_cast<int>(paramCache.seqOctave->load());
        stepSequencer.setOctaveShiftSemitones((seqOctaveIdx - 2) * 12);

        if (seqRunning)
            stepSequencer.start();
        else
            stepSequencer.stop();
        const size_t eventLogStepSeqBefore_ = internalNoteEvents_.size();
        stepSequencer.processBlock(buffer, internalNoteEvents_);
        if (eventLogRecordingActive())
            logInternalNoteEventsFrom(eventLogStepSeqBefore_, NoteEventLogEntry::Source::StepSequencer,
                                     seqRunning && arpEnabled, /*genModeForSkip=*/false);
    }

    // Consume bar-start flag (sequencer display + sync-arm release)
    if (stepSequencer.barStartFlag.exchange(false))
    {
        barBoundaryFlag.store(true, std::memory_order_relaxed);

        // Release any sync-armed Drift/LFO on the downbeat: they have been held
        // silent at phase 0 since the Off→Sync switch, so from here their cycle
        // runs locked to the "1". (Released here, right after the step seq sets
        // barStartFlag for the bar it just crossed.)
        LFO* const lfoPtr[3] = { &lfo1, &lfo2, &lfo3 };
        for (int i = 0; i < 3; ++i)
        {
            if (lfoSyncArmed[i])   { lfoSyncArmed[i]   = false; lfoPtr[i]->setArmed(false); }
            if (driftSyncArmed[i]) { driftSyncArmed[i] = false; driftLfo.setLfoArmed(i, false); }
        }
    }

    // (driftRegenBpm is now stored in updateDriftState() with the resolved
    // sync BPM — no duplicate write needed here.)

    // Stage 2: Arpeggiator. The source is what is HELD: computer-keyboard keys
    // (registered in beginComputerKeyboardNote — they never enter the MIDI buffer)
    // and external MIDI keys (consumed here). A running sequencer's lead note is
    // only a stand-in for "nothing is held"; the moment the transport stops that
    // stand-in is dropped, so a stopped sequencer leaves the arp without a source
    // and it falls silent instead of cycling the last step forever.
    if (arpEnabled)
    {
        arpeggiator.setBpm(static_cast<double>(seqBpm));
        arpeggiator.setRate(arpRate);
        arpeggiator.setOctaveRange(arpOctaves);
        arpeggiator.setShuffle(seqShuffle);
        arpeggiator.setGate(seqGate);
        arpeggiator.setMode(static_cast<T5ynthArpeggiator::Mode>(arpMode));

        // External keys feed the arp whether or not the sequencer runs: the arp's
        // notes sound, the raw key does not. Pitch-bend/CC stay in the buffer so
        // they still reach the voices. arpFilteredMidi_ is a MEMBER that keeps its
        // storage across blocks (clear() is clearQuick()), and the survivors are
        // copied back into midiMessages rather than swapped in — a swap would hand
        // our high-water storage to the host and leave us with whatever it had.
        arpFilteredMidi_.clear();
        for (const auto metadata : midiMessages)
        {
            auto msg = metadata.getMessage();
            const int ch = msg.getChannel();
            // XL DAW-mode ch16 encoder channel is never musical — on note-OFF
            // either, or a ch16 note-off evicts a real held key of that number.
            const bool ch16Encoder = dawModeActive_.load(std::memory_order_relaxed) && ch == 16;
            if (msg.isNoteOn())
            {
                if (ch16Encoder)
                    continue;
                arpeggiator.noteOn(msg.getNoteNumber(), msg.getFloatVelocity(),
                                   /*sourceId=*/-1, ch);
                if (stepRecordArmed.load(std::memory_order_relaxed))
                    pushStepRecordCandidate(msg.getNoteNumber(), msg.getFloatVelocity());
            }
            else if (msg.isNoteOff())
            {
                if (ch16Encoder)
                    continue;
                arpeggiator.noteOff(msg.getNoteNumber());
            }
            else
            {
                arpFilteredMidi_.addEvent(msg, metadata.samplePosition);
            }
        }
        midiMessages.clear();
        midiMessages.addEvents(arpFilteredMidi_, 0, -1, 0);

        if (seqRunning)
        {
            // The running sequencer offers its lead note. Lead = step-seq notes
            // (strandId < 0) or gen-seq strand 0; non-lead gen strands keep
            // sounding alongside the arp. Pull every lead event out of the
            // internal stream — held keys outrank the lead, but either way it
            // must not sound raw underneath the arp.
            int leadNote = -1;
            float leadVel = 0.0f;
            const bool genMode = genModeActiveInAudio;
            internalNoteEvents_.erase(
                std::remove_if(internalNoteEvents_.begin(), internalNoteEvents_.end(),
                    [&](const VoiceEvent& e)
                    {
                        const bool lead = genMode ? (e.strandId == 0) : (e.strandId < 0);
                        if (!lead) return false;
                        if (e.type == VoiceEvent::Type::NoteOn)
                            { leadNote = e.note; leadVel = e.velocity; }
                        return true;  // drop lead note-ons AND note-offs
                    }),
                internalNoteEvents_.end());
            if (leadNote >= 0)
                arpeggiator.setSeqLead(leadNote, leadVel);
        }
        else
        {
            // Transport stopped → the stand-in goes away. Without this the arp
            // kept cycling the sequencer's last note for as long as it was on.
            arpeggiator.clearSeqLead();
        }
        const size_t eventLogArpBefore_ = internalNoteEvents_.size();
        arpeggiator.processBlock(buffer, internalNoteEvents_);
        if (eventLogRecordingActive())
            logInternalNoteEventsFrom(eventLogArpBefore_, NoteEventLogEntry::Source::Arpeggiator);
    }
    else
    {
        // Arp off: flush the arp's own sounding note (if any) and drop the
        // sequencer lead + timing — but KEEP tracking which keys are down, so
        // switching the arp on mid-hold picks them straight up. External notes
        // pass through to the voices untouched; we only observe them.
        const size_t eventLogArpOffBefore_ = internalNoteEvents_.size();
        arpeggiator.allNotesOff(internalNoteEvents_);
        if (eventLogRecordingActive())
            logInternalNoteEventsFrom(eventLogArpOffBefore_, NoteEventLogEntry::Source::Arpeggiator);
        arpeggiator.suspend();

        // Replay clears midiMessages before this point, so no external note-OFF can
        // reach the arp for the length of the tape. A key released mid-replay would
        // stay "held" forever: the arp would arpeggiate a key that is not down, and
        // arpHoldingKeys would pin the block permanently awake. Hold nothing during
        // replay — the transport owns the notes, exactly as in beginComputerKeyboardNote.
        if (replayActive)
        {
            arpeggiator.allKeysUp();
        }
        else
        {
            for (const auto metadata : midiMessages)
            {
                const auto msg = metadata.getMessage();
                const int ch = msg.getChannel();
                // XL DAW-mode ch16 is the encoder channel, on note-OFF too: without
                // this guard a ch16 note-off evicts a real held key of that number.
                if (dawModeActive_.load(std::memory_order_relaxed) && ch == 16)
                    continue;
                if (msg.isNoteOn())
                    arpeggiator.noteOn(msg.getNoteNumber(), msg.getFloatVelocity(),
                                       /*sourceId=*/-1, ch);
                else if (msg.isNoteOff())
                    arpeggiator.noteOff(msg.getNoteNumber());
            }
        }
    }

    // The sequencers schedule per-strand and the arp appends after them, so the
    // internal stream is not globally time-ordered. Sort by sample offset so the
    // merge-walk below stays sample-accurate. std::sort is in-place (introsort) —
    // unlike std::stable_sort it never requests a heap temp buffer, so it is
    // audio-thread-safe. The secondary key (NoteOff before NoteOn at an equal
    // offset) replaces what stability was buying us: a note-off must free its
    // voice before a same-tick note-on can retrigger/steal it. All sequencer/arp
    // emissions are off-then-on at a shared tick, so this matches emit intent.
    std::sort(internalNoteEvents_.begin(), internalNoteEvents_.end(),
              [](const VoiceEvent& a, const VoiceEvent& b)
              {
                  if (a.sampleOffset != b.sampleOffset)
                      return a.sampleOffset < b.sampleOffset;
                  return a.type == VoiceEvent::Type::NoteOff
                      && b.type == VoiceEvent::Type::NoteOn;
              });

    arpWasEnabled = arpEnabled;

    // (barStartFlag consumed + forwarded to barBoundaryFlag above)

    // ── Sample-accurate MIDI + Voice rendering ──────────────────────────────
    const bool lfo1TrigMode = bp.lfo1TrigMode;
    const bool lfo2TrigMode = bp.lfo2TrigMode;
    const bool lfo3TrigMode = bp.lfo3TrigMode;

    // Re-check: seq/arp may have generated notes
    if (voiceManager.hasActiveVoices() || hasActiveSequencerOneShots() || !midiMessages.isEmpty())
        silentBlockCount = 0;

    bool skipSynthesis = (silentBlockCount > 0 && !voiceManager.hasActiveVoices()
                          && midiMessages.isEmpty());

    // Modulation values (zero when skipping synthesis)
    float modDelayTime = 0.0f, modDelayFb = 0.0f, modDelayMix = 0.0f, modReverbMix = 0.0f;

    // Drift → block-level FX targets (runs even during tail for smooth drift)
    modDelayTime += driftLfo.getOffsetForTarget(DriftLFO::TgtDelayTime);
    modDelayFb   += driftLfo.getOffsetForTarget(DriftLFO::TgtDelayFb);
    modDelayMix  += driftLfo.getOffsetForTarget(DriftLFO::TgtDelayMix);
    modReverbMix += driftLfo.getOffsetForTarget(DriftLFO::TgtReverbMix);
    VoiceManager::VoiceOutput voiceOut;

    if (!skipSynthesis)
    {
        // ── Audio generation via VoiceManager + global LFOs ─────────────────────
        float baseLfo1Rate = bp.lfo1Rate;
        float baseLfo2Rate = bp.lfo2Rate;
        float baseLfo3Rate = bp.lfo3Rate;
        float baseLfo1Depth = bp.lfo1Depth;
        float baseLfo2Depth = bp.lfo2Depth;
        float baseLfo3Depth = bp.lfo3Depth;

        // Re-prepare runs on samplerReprepareThread; the audio thread keeps
        // using the last published snapshot and only distributes it. This is the
        // ONE pass where held sampler voices crossfade onto the new snapshot —
        // on the audio thread, matching morphToBufferFrom's own audio-thread-only
        // contract (see its doc comment in SamplePlayer.h). The crossfade runs
        // over the Drift Crossfade time (Regen XFade); the generation guard makes
        // this a no-op once a held voice is current.
        if (masterSampler.hasAudio())
            voiceManager.distributeSamplerBuffer(masterSampler,
                                                 paramCache.driftCrossfade->load(),
                                                 /*allowMorph=*/true);
        if (masterFreeze.hasAudio())
            // Audio-thread per-block redistribution → allowMorph MUST be false
            // (morphToBufferFrom may free a retired snapshot off-thread). The
            // generation guard makes this a no-op when the buffer is unchanged.
            voiceManager.distributeFreezeBuffer(masterFreeze, 0.0f, false);
        if (masterOsc.hasFrames())
        {
            // With a DCO/LCO-baked table the traversal was set once by
            // loadDcoWavetable (full-range motion loop) — re-deriving it from
            // the last neural sample's regions every block would clobber it.
            if (!dcoTableActive_.load(std::memory_order_relaxed)
                && generatedAudioFull.getNumSamples() > 0)
                syncWavetableTraversal(generatedSampleRate, generatedAudioFull.getNumSamples());
            else if (dcoTableActive_.load(std::memory_order_relaxed))
            {
                // A DCO/LCO table scans through the SAME standard AutoScan transport
                // as neural mode now (the oscillator owns no private motion), but does
                // not run through the full syncWavetableTraversal — that re-derives
                // extract brackets and rate from the neural buffer, which do not apply
                // to a DCO table. Mirror only the two live-adjustable controls every
                // block: the One-shot/Loop/Ping-pong buttons and the AutoScan toggle.
                // LoopMode enum order matches BlockParams LoopMode (OneShot=0, Loop=1,
                // PingPong=2). Rate is the recipe's motion tempo, set once at load.
                const auto dcoLoopMode = static_cast<WavetableOscillator::LoopMode>(
                    juce::jlimit(0, 2, static_cast<int>(paramCache.loopMode->load())));
                const bool dcoAutoScanOn = paramCache.wtAutoScan->load() > 0.5f;
                masterOsc.setAutoScanLoopMode(dcoLoopMode);
                masterOsc.setAutoScan(dcoAutoScanOn);
            }
            masterOsc.setMorphTimeMs(paramCache.driftCrossfade->load());
            voiceManager.distributeWavetableFrames(masterOsc);
        }

        // Pre-compute global LFO values for the block (needed by VoiceManager)
        float* lfo1Buf = lfo1Buffer.data();
        float* lfo2Buf = lfo2Buffer.data();
        float* lfo3Buf = lfo3Buffer.data();
        for (int i = 0; i < numSamples; ++i)
        {
            float l1 = lfo1.processSample();
            float l2 = lfo2.processSample();
            float l3 = lfo3.processSample();

            lfo1Buf[i] = l1;
            lfo2Buf[i] = l2;
            lfo3Buf[i] = l3;
        }

        // Csound engine pump (Phase-1 spec §3/D2/D9, extended Phase-2 S1/S4/S5):
        // decided ONCE per block — reused by every per-segment write below and
        // the end-of-block carry update, so the isReady() atomic load happens
        // once, not per segment. False whenever the engine mode isn't Csound or
        // the (possibly still background-compiling) ACTIVE engine isn't ready
        // yet — the entire bridge is then completely inert, a single bool
        // short-circuit, satisfying the audioIdle/performance gate's "zero new
        // per-block cost in every other mode" requirement.
        //
        // csoundActiveIdxNow is a LOCAL, mutable copy of csoundActiveIdx_: it
        // only ever changes (to the other slot) at the exact sample where a
        // running fade completes, later in this same function — so the rest of
        // the block's rendering always indexes the CORRECT engine even across
        // a mid-block flip (a fade completing partway through a MIDI-event-
        // bounded sub-range never leaves later sub-ranges reading the OLD
        // engine).
        int csoundActiveIdxNow = csoundActiveIdx_.load(std::memory_order_acquire);
        const bool csoundActive = (bp.engineMode == EngineMode::Csound)
                                && csoundEngines_[csoundActiveIdxNow].isReady();

        // Consume a compiled+primed swap (S4/S5) at the block's startBlock
        // site: arm the fade (fadePos=0, fadeLen=driftCrossfade in samples,
        // read ONCE here — a knob move mid-crossfade never retimes an
        // in-progress fade, exactly like the other engines' own morph calls
        // capture morphMs once at crossfade-start). Only when Csound is
        // actually the live engine mode; a swap requested while some OTHER
        // mode is selected stays queued untouched (picked up whenever the
        // user switches back to Csound — the entire bridge is inert until then).
        if (csoundActive && csoundSwapPending_.exchange(false, std::memory_order_acq_rel))
        {
            csoundFadePos_ = 0;
            csoundFadeLen_ = juce::jmax(1, juce::roundToInt(bp.driftCrossfade * 0.001f * (float) getSampleRate()));
            csoundSwapFading_.store(true, std::memory_order_release);
        }

        bool csoundFadingNow = csoundActive && csoundSwapFading_.load(std::memory_order_acquire);
        const int csoundOtherIdx = 1 - csoundActiveIdxNow;   // only meaningful while csoundFadingNow

        // startBlock() resets an engine's write position for this host block and
        // replays any ksmps carry from the previous one (CsoundEngine's own
        // bookkeeping) — placed next to the LFO fill above since neither
        // depends on the sample-accurate MIDI split below. Both engines are
        // pumped only while actually fading; a session that never swaps never
        // touches the second engine at all (byte-identical to Phase 1).
        if (csoundActive)
        {
            // The authored instrument's own knobs — the library parameters its
            // body kept (LroControls.h). Once per block, not per voice: they
            // describe the INSTRUMENT, so every voice reads the same twelve
            // channels. Written to BOTH engines while a swap
            // fades — the outgoing one is still sounding, and a knob that
            // stopped answering for the length of a crossfade would read as the
            // control breaking.
            const float lroKnobs[CsoundEngine::kNumGlobalControls] = {
                paramCache.lroP1a->load(), paramCache.lroP1b->load(),
                paramCache.lroP1c->load(), paramCache.lroP1d->load(),
                paramCache.lroP2a->load(), paramCache.lroP2b->load(),
                paramCache.lroP2c->load(), paramCache.lroP2d->load(),
                paramCache.lroP3a->load(), paramCache.lroP3b->load(),
                paramCache.lroP3c->load(), paramCache.lroP3d->load(),
                paramCache.lroLvl1->load(), paramCache.lroLvl2->load(),
                paramCache.lroLvl3->load() };

            csoundEngines_[csoundActiveIdxNow].setGlobalControls(lroKnobs);
            csoundEngines_[csoundActiveIdxNow].startBlock(numSamples);
            if (csoundFadingNow)
            {
                csoundEngines_[csoundOtherIdx].setGlobalControls(lroKnobs);
                csoundEngines_[csoundOtherIdx].startBlock(numSamples);
            }
        }

        // LFO → normalized amount targets (additive, clamped to 0–1). Drift
        // Amt is NOT handled here — driftLfo's depth is already consumed by
        // the time this runs (see updateDriftState), so it's modulated there
        // instead, before its own tick()/getOffsetForTarget calls.
        {
            // lfo?Buf holds the raw unit-amplitude LFO (the oscillators run at
            // setDepth(1.0)); the per-voice path multiplies by the LFO Amount
            // (bp.lfo?Depth) before use, so this block-rate Env-Amt path must
            // apply the same Amount too — otherwise the Amt control is
            // bypassed here and these targets always see full depth (100%).
            float l1End = (numSamples > 0 ? lfo1Buf[numSamples - 1] : 0.0f) * baseLfo1Depth;
            float l2End = (numSamples > 0 ? lfo2Buf[numSamples - 1] : 0.0f) * baseLfo2Depth;
            float l3End = (numSamples > 0 ? lfo3Buf[numSamples - 1] : 0.0f) * baseLfo3Depth;
            const int   lfoTgt[3] = { bp.lfo1Target, bp.lfo2Target, bp.lfo3Target };
            const float lfoEnd[3] = { l1End, l2End, l3End };
            for (int l = 0; l < 3; ++l)
            {
                if (lfoTgt[l] == LfoTarget::Env1Amt)
                    bp.ampAmount = applyNormalizedOffset(bp.ampAmount, lfoEnd[l]);
                for (int i = 0; i < kNumModEnvs; ++i)
                    if (lfoTgt[l] == LfoTarget::modEnvAmt(i))
                        bp.modEnv[i].amount = applyNormalizedOffset(bp.modEnv[i].amount, lfoEnd[l]);
            }
        }

        // Scan → P1 modulation offset (Sampler mode only: retrigger uses it).
        // Granular reads Scan directly inside the voice as its held position.
        if (bp.engineMode == EngineMode::Sampler)
        {
            float p1Mod = bp.driftScanOffset;
            float l1 = numSamples > 0 ? lfo1Buf[numSamples - 1] : 0.0f;
            float l2 = numSamples > 0 ? lfo2Buf[numSamples - 1] : 0.0f;
            float l3 = numSamples > 0 ? lfo3Buf[numSamples - 1] : 0.0f;
            if (bp.lfo1Target == LfoTarget::Scan) p1Mod += l1;
            if (bp.lfo2Target == LfoTarget::Scan) p1Mod += l2;
            if (bp.lfo3Target == LfoTarget::Scan) p1Mod += l3;
            masterSampler.setStartPosOffset(p1Mod);
        }
        else
        {
            masterSampler.setStartPosOffset(0.0f);
        }

        // ── Sample-accurate rendering: split block at MIDI event boundaries ──
        {
            auto midiIter = midiMessages.cbegin();
            size_t intIdx = 0;
            int renderPos = 0;
            // Sentinel strictly above any real event position (positions are
            // 0..numSamples-1) so an exhausted stream never wins the comparisons.
            const int kNoEvent = numSamples + 1;

            while (renderPos < numSamples)
            {
                // Next sub-block ends at the earliest pending event across BOTH
                // streams: external controller MIDI (real MPE channels) and the
                // internal typed events (sequencer/arp).
                int subEnd = numSamples;
                if (midiIter != midiMessages.cend())
                    subEnd = juce::jmin((*midiIter).samplePosition, subEnd);
                if (intIdx < internalNoteEvents_.size())
                    subEnd = juce::jmin(internalNoteEvents_[intIdx].sampleOffset, subEnd);
                // Phase-2 (S5): never let a sub-range straddle the fade's
                // completion point — the flip (csoundActiveIdx_/csoundSwapFading_,
                // below) must land on an exact sample boundary, right after the
                // LAST fading sub-range, so the very next sub-range in this same
                // block (if any) already takes the plain non-fading path with
                // the newly-active engine.
                if (csoundFadingNow)
                    subEnd = juce::jmin(subEnd, renderPos + (csoundFadeLen_ - csoundFadePos_));

                // Render voices up to this point
                int subLen = subEnd - renderPos;
                if (subLen > 0)
                {
                    if (csoundActive)
                    {
                        // Csound pump (D2, extended S5/S6): on-demand, right
                        // before this segment renders — so gate/freq/vel/pres/
                        // timb/trig reflect every event dispatched at renderPos,
                        // sample-accurate with the MIDI split. samplesSinceLastWrite
                        // is the length of the PRECEDING segment (renderPos minus
                        // the position of the previous write), which is exactly how
                        // far the glide smoother needs to advance to catch up to
                        // now, since the orchestra held the last-written controls
                        // constant (via its own portk) for that whole span.
                        // csoundLastWritePos_ carries across host-block boundaries
                        // as a negative offset (see the end-of-loop update below).
                        //
                        // S6: writeCsoundControls advances the glide smoother ONCE
                        // per call regardless of how many engines are fed — BOTH
                        // engines during a fade get the identical, single-advance
                        // control set (the "glide-double-advance trap").
                        CsoundEngine* pumpEngines[2] = { &csoundEngines_[csoundActiveIdxNow], nullptr };
                        int numPumpEngines = 1;
                        if (csoundFadingNow)
                        {
                            pumpEngines[1] = &csoundEngines_[csoundOtherIdx];
                            numPumpEngines = 2;
                        }
                        // Two pitch factors that never reached the orchestra,
                        // fixed together because they are one defect wearing two
                        // hats -- the bridge could not see what the synth does to
                        // pitch (§4: "pitch belongs to the synth; everything must
                        // track it"):
                        //
                        //  - globalPitchBendRatio(): the pitch WHEEL. This used
                        //    to pass bp.performancePitchRatio, which is assigned
                        //    only on the copy applyPerformanceControllers()
                        //    returns and so is a hard 1.0f here -- the wheel bent
                        //    every other engine and left the LCO standing.
                        //  - &bp + the three RAW global LFO samples AT this write
                        //    point: the pitch-modulation bus (LFO/env/drift/
                        //    aftertouch routed to Pitch). Without them an LFO on
                        //    Pitch was inaudible on the Csound engine while
                        //    reaching all three others, which resolve the bus per
                        //    sample inside renderBlock.
                        //
                        // See writeCsoundControls' body for the two known limits
                        // of publishing the bus at block rate rather than per
                        // sample (fast-LFO aliasing; Trig-mode LFOs unreadable).
                        voiceManager.writeCsoundControls(pumpEngines, numPumpEngines,
                                                          voiceManager.globalPitchBendRatio(),
                                                          renderPos - csoundLastWritePos_,
                                                          &bp,
                                                          lfo1Buf[renderPos], lfo2Buf[renderPos],
                                                          lfo3Buf[renderPos]);
                        for (int e = 0; e < numPumpEngines; ++e)
                            pumpEngines[e]->renderUpTo(subEnd - 1);
                        csoundLastWritePos_ = renderPos;

                        const float* csoundVoiceBufs[CsoundEngine::kMaxVoices];
                        if (csoundFadingNow)
                        {
                            // Equal-power crossfade (S5): TRUE sin/cos of each
                            // sample's ABSOLUTE fade position, computed ONCE per
                            // sample (hoisted above the per-voice loop — every
                            // voice shares the identical gain at a given sample,
                            // so this runs subLen times total, never subLen x
                            // kMaxVoices times). Equal-power (not the per-source-
                            // SmoothedValue SUM form) is correct here: the two
                            // orchestras are UNCORRELATED sources, exactly the
                            // morphToBufferFrom situation (CLAUDE.md invariant).
                            //
                            // Adversarial-review finding, post-implementation: an
                            // earlier version of this code computed sin/cos only
                            // at the sub-range's start/end, then LINEARLY
                            // interpolated per sample — which does NOT preserve
                            // gNew^2+gOld^2==1 in the interior of a sub-range
                            // (degrades toward a plain linear crossfade, a real
                            // -3dB power dip at the segment midpoint, whenever a
                            // sub-range spans a non-trivial arc of the quarter-
                            // circle — e.g. the WHOLE 200ms fade in one sub-range
                            // at a short Regen XFade time, or during an offline
                            // bounce with a large host block). Computing sin/cos
                            // directly per sample (hoisted above the voice loop)
                            // is both genuinely equal-power AND cheaper than that
                            // per-voice lerp was.
                            for (int s = 0; s < subLen; ++s)
                            {
                                const float t = (float) (csoundFadePos_ + s) / (float) csoundFadeLen_;
                                csoundFadeGainNew_[static_cast<size_t>(renderPos + s)] =
                                    std::sin(t * juce::MathConstants<float>::halfPi);
                                csoundFadeGainOld_[static_cast<size_t>(renderPos + s)] =
                                    std::cos(t * juce::MathConstants<float>::halfPi);
                            }

                            CsoundEngine& oldEngine = csoundEngines_[csoundActiveIdxNow];
                            CsoundEngine& newEngine = csoundEngines_[csoundOtherIdx];
                            for (int vi = 0; vi < CsoundEngine::kMaxVoices; ++vi)
                            {
                                const float* oldBuf = oldEngine.voiceBuffer(vi);
                                const float* newBuf = newEngine.voiceBuffer(vi);
                                float* mix = csoundMixBufs_[static_cast<size_t>(vi)].data();
                                for (int s = 0; s < subLen; ++s)
                                {
                                    const float gNew = csoundFadeGainNew_[static_cast<size_t>(renderPos + s)];
                                    const float gOld = csoundFadeGainOld_[static_cast<size_t>(renderPos + s)];
                                    const float o = oldBuf != nullptr ? oldBuf[renderPos + s] : 0.0f;
                                    const float n = newBuf != nullptr ? newBuf[renderPos + s] : 0.0f;
                                    mix[renderPos + s] = gOld * o + gNew * n;
                                }
                                csoundVoiceBufs[vi] = mix;
                            }
                        }
                        else
                        {
                            for (int vi = 0; vi < CsoundEngine::kMaxVoices; ++vi)
                                csoundVoiceBufs[vi] = csoundEngines_[csoundActiveIdxNow].voiceBuffer(vi);
                        }

                        voiceOut = voiceManager.renderBlock(buffer, bp,
                            lfo1Buf + renderPos, lfo2Buf + renderPos, lfo3Buf + renderPos,
                            renderPos, subLen, csoundVoiceBufs);

                        if (csoundFadingNow)
                        {
                            csoundFadePos_ += subLen;
                            if (csoundFadePos_ >= csoundFadeLen_)
                            {
                                // Fade complete exactly here: flip which engine is
                                // active (S5), clear the fade state, and rebind the
                                // LOCAL active-index copy so any remaining
                                // sub-ranges later in this same block already use
                                // the newly-active engine via the plain (non-
                                // fading) path above. The old engine simply stops
                                // being pumped — its instruments keep whatever
                                // state they're in, irrelevant until it is
                                // recompiled on the next swap.
                                csoundActiveIdx_.store(csoundOtherIdx, std::memory_order_release);
                                csoundSwapFading_.store(false, std::memory_order_release);
                                csoundActiveIdxNow = csoundOtherIdx;
                                csoundFadingNow = false;
                                triggerAsyncUpdate();   // let a queued request (arrived during this fade) start
                            }
                        }
                    }
                    else
                    {
                        voiceOut = voiceManager.renderBlock(buffer, bp,
                            lfo1Buf + renderPos, lfo2Buf + renderPos, lfo3Buf + renderPos,
                            renderPos, subLen, static_cast<const float* const*>(nullptr));
                    }
                }

                // Dispatch every event at this position, internal and external
                // interleaved in time order (internal first on a tie so a seq
                // note-off precedes a same-tick external note-on).
                while (true)
                {
                    const int extPos = (midiIter != midiMessages.cend())
                                     ? (*midiIter).samplePosition : kNoEvent;
                    const int intPos = (intIdx < internalNoteEvents_.size())
                                     ? internalNoteEvents_[intIdx].sampleOffset : kNoEvent;
                    if (extPos > subEnd && intPos > subEnd)
                        break;

                    if (intPos <= extPos)
                    {
                        // ── Internal sequencer/arp event — typed, never MPE ──
                        const VoiceEvent& ev = internalNoteEvents_[intIdx++];
                        if (ev.type == VoiceEvent::Type::NoteOn)
                        {
                            const bool isBind = (ev.artic != VoiceEvent::Articulation::Normal);
                            // Bind = instant (glideMs 0); Glide + Normal = getGlideTime()
                            // (Normal's value only feeds mono legato).
                            const float glideMs = (ev.artic == VoiceEvent::Articulation::Bind)
                                                ? 0.0f : stepSequencer.getGlideTime();
                            lastMidiNote.store(ev.note, std::memory_order_relaxed);
                            lastMidiVelocity.store(juce::roundToInt(ev.velocity * 127.0f),
                                                   std::memory_order_relaxed);
                            lastMidiNoteOn.store(true, std::memory_order_relaxed);
                            voiceManager.noteOn(ev.note, ev.velocity, isBind, glideMs,
                                lfo1TrigMode, lfo2TrigMode, lfo3TrigMode,
                                ev.strandId, ev.pan, ev.mpeChannel);
                        }
                        else
                        {
                            voiceManager.noteOff(ev.note, ev.strandId);
                            if (!voiceManager.hasActiveVoices())
                                lastMidiNoteOn.store(false, std::memory_order_relaxed);
                        }
                        continue;
                    }

                    // ── External controller MIDI — real channels → full MPE ──
                    const auto msg = (*midiIter).getMessage();
                    ++midiIter;
                    const int channel = msg.getChannel();
                    if (msg.isNoteOn()
                        // XL DAW-mode ch16 is the encoder/fader channel and never sends
                        // musical Note Ons. Without this guard the DAW-mode-enable message
                        // we send (0x9F 0x0C 0x7F) can loop back via the OS MIDI stack and
                        // trigger a stuck voice on note 12 — the root cause of the
                        // intermittent startup pulsating sound.
                        && ! (dawModeActive_.load(std::memory_order_relaxed)
                              && channel == 16))
                    {
                        const int note = msg.getNoteNumber();
                        const float velocity = msg.getFloatVelocity();
                        lastMidiNote.store(note, std::memory_order_relaxed);
                        lastMidiVelocity.store(juce::roundToInt(velocity * 127.0f),
                                               std::memory_order_relaxed);
                        lastMidiNoteOn.store(true, std::memory_order_relaxed);
                        // External note: tagged with its real MIDI channel so per-note
                        // MPE bend / pressure / timbre on that channel route to it. No
                        // bind/glide/strand — those are internal-sequencer concepts.
                        voiceManager.noteOn(note, velocity, /*isBind=*/false,
                            stepSequencer.getGlideTime(),
                            lfo1TrigMode, lfo2TrigMode, lfo3TrigMode,
                            /*sourceId=*/-1, /*pan=*/0.0f, /*mpeChannel=*/channel);
                        if (stepRecordArmed.load(std::memory_order_relaxed))
                            pushStepRecordCandidate(note, velocity);
                        if (eventLogRecordingActive())
                            logExternalNoteEvent(true, note, velocity, channel, extPos);
                    }
                    else if (msg.isNoteOff())
                    {
                        voiceManager.noteOff(msg.getNoteNumber(), -1);
                        if (!voiceManager.hasActiveVoices())
                            lastMidiNoteOn.store(false, std::memory_order_relaxed);
                        if (eventLogRecordingActive())
                            logExternalNoteEvent(false, msg.getNoteNumber(), 0.0f, channel, extPos);
                    }
                    else if (msg.isAftertouch())
                    {
                        // Poly key pressure matches by note number across external voices.
                        const float pressure = static_cast<float>(msg.getAfterTouchValue()) / 127.0f;
                        voiceManager.setPolyPressure(msg.getNoteNumber(), pressure, -1);
                    }
                    else if (msg.isChannelPressure())
                    {
                        const float pressure = static_cast<float>(msg.getChannelPressureValue()) / 127.0f;
                        // MPE Loudness (Z). A zone MASTER channel = zone-wide pressure
                        // (all voices). A member channel = per-note pressure on the voice
                        // tagged with that channel only. Which channel 16 is depends on
                        // whether an upper zone was declared — see isMpeMasterChannel.
                        if (isMpeMasterChannel (channel))
                            voiceManager.setChannelPressure(pressure);
                        else
                            voiceManager.setChannelPressureForChannel(channel, pressure);
                    }
                    else if (msg.isPitchWheel())
                    {
                        const int pbChannel = msg.getChannel();
                        const float centered = (static_cast<float>(msg.getPitchWheelValue()) - 8192.0f) / 8192.0f;
                        // Ch1 = MPE master / standard MIDI global bend.
                        // Ch2-16 = MPE per-note bend; VoiceManager only forwards to voices
                        // that were tagged with that channel on noteOn (external notes only —
                        // internal sequencer notes are tagged channel 0 and never match).
                        //
                        // Deliberately NOT isMpeMasterChannel(): this is the one expression
                        // that already treated channel 16 as a member, so it needs no repair,
                        // and routing it through the predicate would turn a declared upper
                        // zone's master bend global. Correct per MPE, and untestable here —
                        // a change to what the pitch wheel does, arriving inside a change
                        // about which channel is a master.
                        if (pbChannel == 1)
                        {
                            voiceManager.setPitchBendSemitones(
                                juce::jlimit(-1.0f, 1.0f, centered)
                                * static_cast<float>(mpeMasterBendRangeInForce_));
                        }
                        else
                        {
                            // `centered` rides along untouched as MPE X's own
                            // value: it is the wheel travel, independent of the
                            // bend range, which is what a modulation target can
                            // use. The semitones are that travel times the range.
                            voiceManager.setPerVoicePitchBend(pbChannel,
                                centered * static_cast<float>(mpePerNoteBendRangeInForce_),
                                centered);
                        }
                    }
                    else if (msg.isAllNotesOff() || msg.isAllSoundOff())
                    {
                        voiceManager.allNotesOff();
                        // Keys + lead only. reset() here would clear lastPlayedNote
                        // while this block's already-queued arp NoteOn is still
                        // ahead of us in internalNoteEvents_ — that voice would
                        // start after the panic and never receive its note-off.
                        arpeggiator.allKeysUp();
                        arpeggiator.clearSeqLead();
                        lastMidiNoteOn.store(false, std::memory_order_relaxed);
                    }
                    else if (msg.isController())
                    {
                        const int cc = msg.getControllerNumber();
                        const int value7 = msg.getControllerValue();
                        // ── CC routing priority ──────────────────────────────────────
                        //  1. CC Learn: capture ANY incoming CC (incl. reserved) as target.
                        //  2. RPN state machine (pitch-bend range / MPE) + gen-seq strand pan:
                        //     internal/standard routing — kept ABOVE user bindings so a bound
                        //     CC6/CC10 can never shadow MPE bend-range or strand pan.
                        //  3. Explicit user binding (XL Map / CC Learn): wins over the built-in
                        //     GM performance CCs below — e.g. an XL fader on CC7/CC11 drives its
                        //     mapped param instead of channel-volume / expression.
                        //  4. Built-in GM / system CCs: fallback for unbound CCs.
                        //
                        // The RPN bytes go to juce::MPEZoneLayout, and the answer is
                        // needed BEFORE the chain rather than as a branch of it: an
                        // else-if cannot both consume a CC6 and decline it, and a CC6
                        // that completes no RPN we act on has to reach a user binding.
                        // The XL guard is the hand-written parser's, unchanged: in DAW
                        // mode channel 16 carries the Resynth fader on CC6 and the
                        // Drift3 Amt encoder on CC100, and reading either as RPN killed
                        // that control.
                        //
                        // Read ONCE and reused below: the message thread can flip
                        // this between two loads, and one CC6 would then both set
                        // the bend range and be captured as a learn target.
                        const bool learning = midiLearnActive.load(std::memory_order_relaxed);

                        const bool consumedAsMpeRpn =
                            ! learning
                            && (isMpeRpnSelectController (cc)
                                || isNrpnSelectController (cc)
                                || cc == 6)
                            && ! (dawModeActive_.load(std::memory_order_relaxed)
                                  && msg.getChannel() == 16)
                            && handleMpeRpnByte (msg.getChannel(), cc, value7);

                        if (learning)
                        {
                            // CC Learn intercept: signal the message thread with the CC number.
                            midiLearnTargetCc.store(cc, std::memory_order_release);
                            triggerAsyncUpdate();
                        }
                        else if (consumedAsMpeRpn)
                        {
                            // Already handled above by juce::MPEZoneLayout.
                        }
                        else if (cc == 74 && ! isMpeMasterChannel (channel))
                        {
                            // MPE Timbre (Y / the slide axis) → per-note brightness on the
                            // voice(s) tagged with this MEMBER channel. Gated off the zone
                            // MASTER channels on purpose: CC74 on ch1 is the generic
                            // control-surface knob default (kExtMap → osc_scan), and MPE
                            // timbre is always transmitted per-note on member channels, so
                            // the channel cleanly separates the two meanings — a ch1 knob
                            // keeps driving Scan, a LinnStrument slide drives brightness.
                            voiceManager.setTimbre(channel, static_cast<float>(value7) / 127.0f);
                        }
                        else if (dawModeActive_.load(std::memory_order_relaxed)
                                 && msg.getChannel() == 1
                                 && ((cc >= 37 && cc <= 52) || cc == 116 || cc == 118))
                        {
                            // XL DAW-mode buttons (ch1): the two bottom rows CC 37-52 plus
                            // the left transport buttons Play ▶ = 116 / Record ● = 118
                            // (programmer's ref p.9). Act on press (value>=64); the release
                            // (0) is ignored. The device sends exactly one 127 per press, and
                            // transport toggles are de-duplicated by the atomic-request
                            // exchange in handleAsyncUpdate, so one press = one action with no
                            // per-button latch needed. Gated by dawModeActive_ so these CCs
                            // only act as buttons while the XL is driving us.
                            if (value7 >= 64)
                                handleXLButtonPress(cc);
                        }
                        else if (dawModeActive_.load(std::memory_order_relaxed)
                                 && msg.getChannel() == 16
                                 && cc >= 77 && cc <= 100)
                        {
                            // XL DAW-mode encoders in RELATIVE mode (endless encoders): the
                            // ch16 guard matches the manual (p.9 "Encoders and faders output on
                            // channel 16") and is consistent with the cc100/cc6 guards above — it
                            // stops a second controller's CC 77-100 from nudging XL-mapped params.
                            // value is a signed delta around 64, and the relative CC is the
                            // absolute CC + 64. Nudge the bound param from its CURRENT value
                            // (no absolute position → no jump). Same setValueNotifyingHost /
                            // ScopedTryLock path as the absolute fader binding below.
                            const int absCc = cc - 64;          // 77-100 → 13-36
                            const int delta = value7 - 64;      // signed: >0 CW, <0 CCW
                            if (delta != 0)
                            {
                                const juce::SpinLock::ScopedTryLockType tryLock(ccMappingLock_);
                                if (tryLock.isLocked())
                                {
                                    const auto& mapping = resolveCcMapping(absCc);
                                    if (mapping.param != nullptr)
                                    {
                                        // span is signed so an inverted binding keeps its direction.
                                        const float span = mapping.maxNorm - mapping.minNorm;
                                        // One detent is delta≈±1 → ±1/127≈0.008 normalized. For a
                                        // param with a fine interval AND low-end skew (e.g. Attack:
                                        // 0-5000 ms, interval 0.1 ms, skew 0.3) that step converts to
                                        // ~0.0005 ms and snaps back to 0 — so getValue() never advances
                                        // and the encoder is dead at its 0 default. Carry the unrealized
                                        // remainder across detents so it eventually crosses the snap.
                                        float& acc = relEncoderAccum_[static_cast<size_t>(absCc)];
                                        acc += static_cast<float>(delta) / 127.0f * span;
                                        const float cur  = mapping.param->getValue();
                                        const float next = juce::jlimit(0.0f, 1.0f, cur + acc);
                                        eventLogOriginHint_.store(static_cast<int>(ParamOrigin::MidiCCLearn),
                                                                  std::memory_order_relaxed);
                                        mapping.param->setValueNotifyingHost(next);
                                        if (next <= 0.0f || next >= 1.0f)
                                            acc = 0.0f;  // at a rail: drop remainder (no reversal dead-zone)
                                        else
                                            acc -= (mapping.param->getValue() - cur);  // keep sub-snap remainder
                                        const uint64_t seq =
                                            (midiTouchPacked_.load(std::memory_order_relaxed) >> 32) + 1;
                                        midiTouchPacked_.store(
                                            (seq << 32) | static_cast<uint32_t>(absCc),
                                            std::memory_order_release);
                                    }
                                }
                            }
                        }
                        else
                        {
                            // Explicit user binding wins over the built-in GM CCs.
                            // ScopedTryLockType: if the message thread holds the lock (writing
                            // a new binding) we skip the apply — the binding becomes visible on
                            // the next CC event — and fall through to the GM handling below.
                            bool boundHandled = false;
                            {
                                const juce::SpinLock::ScopedTryLockType tryLock(ccMappingLock_);
                                if (tryLock.isLocked())
                                {
                                    // param* and minNorm/maxNorm only — no String access.
                                    const auto& mapping = resolveCcMapping(cc);
                                    if (mapping.param != nullptr)
                                    {
                                        const float norm = juce::jmap(
                                            static_cast<float>(value7), 0.f, 127.f,
                                            mapping.minNorm, mapping.maxNorm);
                                        eventLogOriginHint_.store(static_cast<int>(ParamOrigin::MidiCCLearn),
                                                                  std::memory_order_relaxed);
                                        mapping.param->setValueNotifyingHost(
                                            juce::jlimit(0.0f, 1.0f, norm));
                                        // Record the touch so the editor can make the easy-mode
                                        // ENV/LFO/Drift tab follow this controller (cosmetic).
                                        // Pack (seq+1, cc) into one word — single writer here.
                                        const uint64_t seq =
                                            (midiTouchPacked_.load(std::memory_order_relaxed) >> 32) + 1;
                                        midiTouchPacked_.store(
                                            (seq << 32) | static_cast<uint32_t>(cc),
                                            std::memory_order_release);
                                        boundHandled = true;
                                    }
                                }
                            }

                            if (! boundHandled)
                            {
                                if (cc == 64)
                                {
                                    const bool down = value7 >= 64;
                                    // While step-record is armed the sustain pedal
                                    // enters a REST (empty step) on each press —
                                    // edge-triggered so a held pedal doesn't spam,
                                    // and normal sustain is suppressed. Otherwise it
                                    // is the usual damper pedal.
                                    if (stepRecordArmed.load(std::memory_order_relaxed))
                                    {
                                        if (down && ! sustainPedalDown_)
                                            pushStepRecordCandidate(-1, 0.0f);  // sentinel: rest
                                        // Never latch the damper while armed: arming
                                        // with the pedal already held would otherwise
                                        // leave notes stuck (the release routes here).
                                        // setSustainPedal early-returns if unchanged.
                                        voiceManager.setSustainPedal(false);
                                    }
                                    else
                                    {
                                        voiceManager.setSustainPedal(down);
                                    }
                                    sustainPedalDown_ = down;
                                }
                                else if (cc == 1)
                                {
                                    voiceManager.setModWheel(static_cast<float>(value7) / 127.0f);
                                }
                                else if (cc == 2)
                                {
                                    voiceManager.setBreathController(static_cast<float>(value7) / 127.0f);
                                }
                                else if (cc == 7)
                                {
                                    voiceManager.setChannelVolume(static_cast<float>(value7) / 127.0f);
                                }
                                else if (cc == 11)
                                {
                                    voiceManager.setExpression(static_cast<float>(value7) / 127.0f);
                                }
                                else if (cc == 66)
                                {
                                    voiceManager.setSostenutoPedal(value7 >= 64);
                                }
                                else if (cc == 67)
                                {
                                    voiceManager.setSoftPedal(value7 >= 64);
                                }
                                else if (cc == 120 || cc == 123)
                                {
                                    // Unreachable: CC120/123 ARE isAllSoundOff/
                                    // isAllNotesOff and were consumed above.
                                    voiceManager.allNotesOff();
                                    lastMidiNoteOn.store(false, std::memory_order_relaxed);
                                }
                                else if (cc == 121)
                                {
                                    voiceManager.resetPerformanceControllers();
                                    // Reset All Controllers returns the RPN selection to
                                    // Null. Inert while RPN 0 was the only one we read;
                                    // now a stale selection can swallow a CC6 that was
                                    // meant for a binding. NOT the zone layout — that is
                                    // a property of the device, not a controller value.
                                    //
                                    // Same XL guard as the RPN feed: in DAW mode channel
                                    // 16 is not a MIDI control channel for us, and this
                                    // is the one place that could still reach into it.
                                    if (! (dawModeActive_.load(std::memory_order_relaxed)
                                           && msg.getChannel() == 16))
                                        deselectMpeRpn(msg.getChannel(), /*lsbOnly=*/false);
                                }
                            }
                        }
                    }
                    // (midiIter was advanced right after getMessage() above — the
                    // instant we committed to consuming this external message — so
                    // there is no second increment here.)
                }

                renderPos = subEnd;
            }
        }

        // Csound pump (D2): carry the write-position accounting across the
        // host-block boundary. The last write this block landed at some
        // renderPos < numSamples (there's rarely a MIDI event exactly at the
        // final sample); subtracting numSamples rebases it to a negative
        // offset so the FIRST write of the next block correctly measures the
        // gap all the way from that last write, through the unwritten tail of
        // THIS block, into the next one's renderPos (spec §3/D2 worked example).
        if (csoundActive)
            csoundLastWritePos_ -= numSamples;

        lastTriggeredNote = voiceOut.lastTriggeredNote;

        // Capture last LFO values for block-rate modulation + ghost display
        float lastAmpVal = voiceOut.lastAmpVal;
        // Every envelope as one list, amp first and then ENV 2..5, so each target
        // question below is asked once instead of once per envelope. An envelope
        // has exactly one target, so at most one arm of each group fires per
        // entry and the accumulation order across envelopes is unchanged.
        struct EnvSrc { int target; float value; };
        EnvSrc envSrc[1 + kNumModEnvs];
        envSrc[0] = { bp.ampTarget, lastAmpVal };
        for (int i = 0; i < kNumModEnvs; ++i)
            envSrc[1 + i] = { bp.modEnv[i].target, voiceOut.lastModVal[i] };

        float effectiveLfo1Depth = baseLfo1Depth;
        float effectiveLfo2Depth = baseLfo2Depth;
        float effectiveLfo3Depth = baseLfo3Depth;
        for (const auto& e : envSrc)
        {
            if (e.target == EnvTarget::LFO1Depth) effectiveLfo1Depth = applyNormalizedOffset(effectiveLfo1Depth, e.value);
            if (e.target == EnvTarget::LFO2Depth) effectiveLfo2Depth = applyNormalizedOffset(effectiveLfo2Depth, e.value);
            if (e.target == EnvTarget::LFO3Depth) effectiveLfo3Depth = applyNormalizedOffset(effectiveLfo3Depth, e.value);
        }
        float rawLastLfo1Val = numSamples > 0 ? lfo1Buf[numSamples - 1] : 0.0f;
        float rawLastLfo2Val = numSamples > 0 ? lfo2Buf[numSamples - 1] : 0.0f;
        float rawLastLfo3Val = numSamples > 0 ? lfo3Buf[numSamples - 1] : 0.0f;
        float lastLfo1Val = rawLastLfo1Val * effectiveLfo1Depth;
        float lastLfo2Val = rawLastLfo2Val * effectiveLfo2Depth;
        float lastLfo3Val = rawLastLfo3Val * effectiveLfo3Depth;
        lastLfo1Val_ = lastLfo1Val;
        lastLfo2Val_ = lastLfo2Val;
        lastLfo3Val_ = lastLfo3Val;

        // Filter is now per-voice (in SynthVoice::renderBlock)

        // ── Accumulate block-rate modulation for delay/reverb ─────────────────
        // (Pitch modulation is handled per-sample in SynthVoice::renderBlock)
        for (const auto& e : envSrc)
        {
            if (e.target == EnvTarget::DelayTime)  modDelayTime += e.value;
            if (e.target == EnvTarget::DelayFB)    modDelayFb   += e.value;
            if (e.target == EnvTarget::DelayMix)   modDelayMix  += e.value;
            if (e.target == EnvTarget::ReverbMix)  modReverbMix += e.value;
        }

        // Env → LFO modulation. The Depth arms repeat the ones above; that
        // double application is how this has always behaved and is left alone.
        for (const auto& e : envSrc)
        {
            if (e.target == EnvTarget::LFO1Rate)   lfo1.setRate(baseLfo1Rate * (1.0f + e.value));
            if (e.target == EnvTarget::LFO1Depth)  effectiveLfo1Depth = applyNormalizedOffset(effectiveLfo1Depth, e.value);
            if (e.target == EnvTarget::LFO2Rate)   lfo2.setRate(baseLfo2Rate * (1.0f + e.value));
            if (e.target == EnvTarget::LFO2Depth)  effectiveLfo2Depth = applyNormalizedOffset(effectiveLfo2Depth, e.value);
            if (e.target == EnvTarget::LFO3Rate)   lfo3.setRate(baseLfo3Rate * (1.0f + e.value));
            if (e.target == EnvTarget::LFO3Depth)  effectiveLfo3Depth = applyNormalizedOffset(effectiveLfo3Depth, e.value);
        }
        if (bp.lfo1Target == LfoTarget::DelayTime)  modDelayTime += lastLfo1Val;
        if (bp.lfo1Target == LfoTarget::DelayFB)    modDelayFb += lastLfo1Val;
        if (bp.lfo1Target == LfoTarget::DelayMix)   modDelayMix += lastLfo1Val;
        if (bp.lfo1Target == LfoTarget::ReverbMix)  modReverbMix += lastLfo1Val;
        if (bp.lfo2Target == LfoTarget::DelayTime)  modDelayTime += lastLfo2Val;
        if (bp.lfo2Target == LfoTarget::DelayFB)    modDelayFb += lastLfo2Val;
        if (bp.lfo2Target == LfoTarget::DelayMix)   modDelayMix += lastLfo2Val;
        if (bp.lfo2Target == LfoTarget::ReverbMix)  modReverbMix += lastLfo2Val;
        if (bp.lfo3Target == LfoTarget::DelayTime)  modDelayTime += lastLfo3Val;
        if (bp.lfo3Target == LfoTarget::DelayFB)    modDelayFb += lastLfo3Val;
        if (bp.lfo3Target == LfoTarget::DelayMix)   modDelayMix += lastLfo3Val;
        if (bp.lfo3Target == LfoTarget::ReverbMix)  modReverbMix += lastLfo3Val;
    }
    else
    {
        // Free-running LFOs: sample one value for ghost display, advance rest
        if (numSamples > 0)
        {
            lastLfo1Val_ = lfo1.processSample() * bp.lfo1Depth;
            lastLfo2Val_ = lfo2.processSample() * bp.lfo2Depth;
            lastLfo3Val_ = lfo3.processSample() * bp.lfo3Depth;
            if (numSamples > 1)
            {
                lfo1.advancePhase(numSamples - 1);
                lfo2.advancePhase(numSamples - 1);
                lfo3.advancePhase(numSamples - 1);
            }
        }
    }

    // GenSeq one-shots render into their own buffer. They are kept OUT of the
    // delay path (injected post-delay so the echo line never repeats them) but
    // folded into the signal before the reverb send so they still reverberate.
    // Match the current block length — renderSequencerOneShots advances voices by
    // the passed buffer's sample count. Allocated at samplesPerBlock in
    // prepareToPlay, so this is a no-op resize while the host honours its declared
    // max block (same RT-safety assumption as reverbSendBuffer).
    oneShotBuffer.setSize(2, numSamples, false, false, true);
    oneShotBuffer.clear();
    renderSequencerOneShots(oneShotBuffer);

    // The output gain at the end of this function is a function of the VOICE
    // COUNT switch, and the one-shots are not voices: they never pass through
    // VoiceManager, the switch does not bound how many of them can sound
    // (MAX_STEPS * ONE_SHOT_SLOTS do, regardless), and EngineCalib and the
    // 1/N^0.1 law never touch them. Left alone they would ride the switch
    // anyway, moving by 15.7 dB between Mono and 16 for a control that says
    // nothing about them. Pre-divided here, they come out of the master stage at
    // kOneShotReferenceGain whatever the switch says.
    const float oneShotPreGain = kOneShotReferenceGain
        / juce::jmax(1.0e-6f, outputGainForThreshold(paramCache.limiterThresh->load(),
                                                     static_cast<int>(paramCache.voiceCount->load())));

    // Advanced HERE and not inside the lambda: the lambda runs in at most one of
    // four mutually exclusive routing branches, so a block that takes a branch
    // with no one-shots would otherwise leave the ramp's start value a block or
    // more behind and step the next one-shot that does sound.
    const float oneShotPreGainPrev = oneShotPreGainPrev_;
    oneShotPreGainPrev_ = oneShotPreGain;

    auto addOneShots = [&](juce::AudioBuffer<float>& dest)
    {
        const int ch = juce::jmin(dest.getNumChannels(), oneShotBuffer.getNumChannels());
        for (int c = 0; c < ch; ++c)
            dest.addFromWithRamp(c, 0, oneShotBuffer.getReadPointer(c), numSamples,
                                 oneShotPreGainPrev, oneShotPreGain);
    };

    // ── The amplifier chain: distortion → chorus → phaser → tremolo ────────
    // Ahead of delay and reverb, which is the order an instrument goes through an
    // amp and its pedals: the dirt is on the note, the modulation is on the dirty
    // note, and the room is around all of it. It runs on the voice sum only —
    // sequencer one-shots join below, the same place they already skip the delay.
    //
    // The setters are NOT behind the gate — they run every block, and two of them
    // cost more than a comparison (`setDrive` is a std::pow, the widget setters
    // call juce::dsp::Chorus/Phaser::update()). Measured at APVTS defaults: 54.9
    // ns per block, which at 512/48k is 0.0005% of one core. The four
    // `processBlock` calls below are the ones that matter and they DO gate
    // themselves. Idle CPU is this project's number one historical class of bug
    // (docs/PERFORMANCE_GUIDE.md), and four always-on effects behind the voice sum
    // would be four new per-block costs on a synth playing nothing.
    {
        // The four bypasses go through the MIX (the tremolo through its depth),
        // not around the processBlock call. AmpEffects.h documents why in as
        // many words: "A GATE THAT OPENS AND SHUTS IS A CLICK", measured at
        // -1.29 on the phaser against a signal whose own largest step is 0.0144,
        // which is why each effect gates on its SMOOTHED value and keeps running
        // until its ramp has arrived. Routing the switch into the same ramp
        // inherits all of that; wrapping the call in an `if` would reintroduce
        // exactly the defect that mechanism exists for.
        const bool distOn   = paramCache.fxDistOn->load()   > 0.5f;
        const bool chorusOn = paramCache.fxChorusOn->load() > 0.5f;
        const bool phaserOn = paramCache.fxPhaserOn->load() > 0.5f;
        const bool tremOn   = paramCache.fxTremOn->load()   > 0.5f;

        ampDistortion.setDrive(paramCache.fxDistDrive->load());
        ampDistortion.setMix(distOn ? paramCache.fxDistMix->load() : 0.0f);
        ampChorus.setRate(paramCache.fxChorusRate->load());
        ampChorus.setDepth(paramCache.fxChorusDepth->load());
        ampChorus.setMix(chorusOn ? paramCache.fxChorusMix->load() : 0.0f);
        ampPhaser.setRate(paramCache.fxPhaserRate->load());
        ampPhaser.setDepth(paramCache.fxPhaserDepth->load());
        ampPhaser.setFeedback(paramCache.fxPhaserFeedback->load());
        ampPhaser.setMix(phaserOn ? paramCache.fxPhaserMix->load() : 0.0f);
        ampTremolo.setRate(paramCache.fxTremRate->load());
        ampTremolo.setDepth(tremOn ? paramCache.fxTremDepth->load() : 0.0f);
        ampTremolo.setStereo(paramCache.fxTremStereo->load());
        ampTremolo.setWave(static_cast<int>(paramCache.fxTremWave->load()));

        ampDistortion.processBlock(buffer);
        ampChorus.processBlock(buffer);
        ampPhaser.processBlock(buffer);
        ampTremolo.processBlock(buffer);
    }

    // ── Effects (parallel send-bus: dry + delay + reverb → limiter) ───────
    int delayType = static_cast<int>(paramCache.delayType->load());
    bool delayEnabled = delayType > 0;
    int reverbType = static_cast<int>(paramCache.reverbType->load());
    bool reverbEnabled = reverbType > 0;
    bool reverbIsAlgo = ReverbType::isAlgorithmic(reverbType);

    if (delayEnabled)
    {
        const int delayClock = static_cast<int>(paramCache.delayClockMode->load());
        const float baseDelayTime = (delayClock == ClockMode::Off)
            ? paramCache.delayTime->load()
            : ClockSync::computeDelayMs(syncBpm,
                static_cast<int>(paramCache.delayClockDivision->load()));
        float baseDelayFb = paramCache.delayFeedback->load();
        float baseDelayMix = paramCache.delayMix->load();
        // Apply modulation offsets to delay params
        delay.setTime(juce::jlimit(1.0f, 5000.0f, baseDelayTime * (1.0f + modDelayTime)));
        delay.setFeedback(juce::jlimit(0.0f, 0.95f, baseDelayFb + modDelayFb));
        delay.setMix(juce::jlimit(0.0f, 1.0f, baseDelayMix + modDelayMix));
        delay.setDamp(paramCache.delayDamp->load());
        delay.setMode(DelayType::baseMode(delayType));
        delay.setCharacter(DelayType::character(delayType));
    }

    if (reverbEnabled)
    {
        if (reverbIsAlgo)
        {
            algoReverb.setRoomSize(paramCache.algoRoom->load());
            algoReverb.setDamping(paramCache.algoDamping->load());
            algoReverb.setWidth(paramCache.algoWidth->load());
            // Freeverb+ only: the early-reflection front end. Freeverb stays
            // byte-for-byte JUCE's reverb, so no existing preset changes voicing.
            algoReverb.setEarlyReflections(reverbType == ReverbType::AlgoPlus);
        }
        else
        {
            // Convolution: map reverb_type 1=Dark→2, 2=Medium→1, 3=Bright→0
            int irIndex = reverbType == 1 ? 2 : reverbType == 2 ? 1 : 0;
            if (irIndex != lastReverbIr)
            {
                const void* irData = nullptr;
                size_t irSize = 0;
                switch (irIndex)
                {
                    case 0: irData = BinaryData::emt_140_plate_bright_wav;
                            irSize = static_cast<size_t>(BinaryData::emt_140_plate_bright_wavSize); break;
                    case 1: irData = BinaryData::emt_140_plate_medium_wav;
                            irSize = static_cast<size_t>(BinaryData::emt_140_plate_medium_wavSize); break;
                    case 2: irData = BinaryData::emt_140_plate_dark_wav;
                            irSize = static_cast<size_t>(BinaryData::emt_140_plate_dark_wavSize); break;
                }
                if (irData != nullptr)
                {
                    reverb.loadImpulseResponse(irData, irSize);
                    lastReverbIr = irIndex;
                }
            }
        }
        // Reverbs always render pure wet; the outer crossfade handles dry/wet.
        if (reverbIsAlgo) algoReverb.setMix(1.0f);
        else              reverb.setMix(1.0f);
    }

    // Wet-path normalisation. The retired kPlateWetGain = 2.0 did the opposite of
    // this: it pushed the (already unity) EMT-140 IRs UP to match the Freeverb,
    // which is itself +5.5 dB hot — matching to a broken reference. Measured with
    // tools/measure_fx_mix.cpp; constants and rationale in FxMixLaw. Folded into
    // the crossfade's wet gain rather than applied as a separate pass over the
    // send buffer: it is a per-block constant, so a whole extra multiply pass
    // (which JUCE would skip for the plate's 1.0 but not for Algo) buys nothing.
    const float reverbWetTrim = FxMixLaw::reverbTrim(reverbType);

    auto processReverb = [&](juce::AudioBuffer<float>& buf) {
        if (reverbIsAlgo)
            algoReverb.processBlock(buf);
        else
            reverb.processBlock(buf);
    };

    auto crossfadeReverbInto = [&](juce::AudioBuffer<float>& dest, float mix)
    {
        // Constant-power law on the normalised wet path — identical for Algo and
        // Plate now that neither is hot. The old `mix*mix` algo curve existed only
        // to restrain the untrimmed Freeverb and made its bottom end unusable
        // (-33.6 dB W/D at mix=0.10). mix=1.0 still reaches pure wet.
        //
        // RAMPED, not stepped. The dry slope is no longer the old law's constant
        // -1: it steepens to about -8.5 near mix=1.0, so a per-block step that was
        // inaudible under `dry = 1-mix` zippers audibly here when an LFO or
        // envelope rides Reverb Mix high in its travel.
        const float wetAmt = FxMixLaw::wetGain(mix) * reverbWetTrim;
        const float dryAmt = FxMixLaw::dryGain(mix);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            dest.applyGainRamp(ch, 0, numSamples, prevReverbDry_, dryAmt);
            dest.addFromWithRamp(ch, 0, reverbSendBuffer.getReadPointer(ch), numSamples,
                                 prevReverbWet_, wetAmt);
        }
        prevReverbDry_ = dryAmt;
        prevReverbWet_ = wetAmt;
    };

    if (delayEnabled && reverbEnabled)
    {
        // Serial chain: delay -> reverb. The reverb send is taken AFTER the delay
        // so the delay repeats are themselves reverberated (delay INTO reverb, the
        // classic lush routing); the crossfade then sums the reverb against the
        // post-delay dry. (Previously the send was pre-delay, leaving the delay
        // running parallel PAST the reverb — the repeats were never reverberated.)
        delay.processBlock(buffer);

        addOneShots(buffer);  // one-shots skip the delay, join before the reverb send

        for (int ch = 0; ch < numChannels; ++ch)
            reverbSendBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        processReverb(reverbSendBuffer);

        float revMix = juce::jlimit(0.0f, 1.0f,
            paramCache.reverbMix->load() + modReverbMix);
        crossfadeReverbInto(buffer, revMix);
    }
    else if (delayEnabled)
    {
        delay.processBlock(buffer);
        addOneShots(buffer);  // one-shots bypass the delay entirely
        prevReverbDry_ = 1.0f;   // reverb bypassed: dry passes at unity, no wet
        prevReverbWet_ = 0.0f;
    }
    else if (reverbEnabled)
    {
        addOneShots(buffer);  // one-shots reverberate with everything else

        for (int ch = 0; ch < numChannels; ++ch)
            reverbSendBuffer.copyFrom(ch, 0, buffer, ch, 0, numSamples);

        processReverb(reverbSendBuffer);

        float revMix = juce::jlimit(0.0f, 1.0f,
            paramCache.reverbMix->load() + modReverbMix);
        crossfadeReverbInto(buffer, revMix);
    }
    else
    {
        addOneShots(buffer);  // no FX: one-shots still need to reach the output
        prevReverbDry_ = 1.0f;   // reverb bypassed: dry passes at unity, no wet
        prevReverbWet_ = 0.0f;
    }

    // ── Update modulated values for GUI ghost indicators ────────────────────
    // LFO-driven ghosts run continuously (LFOs are free-running) so the user
    // sees modulation movement even between notes. Envelope-driven ghosts
    // still require active voices (envelopes only produce values when a
    // voice is playing).
    constexpr float NO_GHOST = std::numeric_limits<float>::quiet_NaN();

    {
        bool hasVoices = voiceOut.hasActiveVoices;

        // Filter cutoff ghost
        {
            bool lfoModFilter = bp.lfo1Target == LfoTarget::Filter || bp.lfo2Target == LfoTarget::Filter
                                || bp.lfo3Target == LfoTarget::Filter;
            bool envModFilter = (bp.ampTarget == EnvTarget::Filter
                                 || anyModEnvTargets(bp, EnvTarget::Filter)
                                 || bp.kbdTrack > 0.0f) && hasVoices;
            bool aftertouchModFilter = bp.filterEnabled
                                    && bp.aftertouchTargetAmt[AftertouchTarget::Cutoff] != 0.0f && hasVoices;

            if (hasVoices && (lfoModFilter || envModFilter || aftertouchModFilter))
            {
                modulatedValues.filterCutoff.store(voiceOut.lastModulatedCutoff, std::memory_order_relaxed);
            }
            else if (lfoModFilter)
            {
                // Hypothetical cutoff from base + LFO (no envelope, no kbd track).
                // Same per-destination law as the audio path: each filter-targeted
                // LFO's depth travels ModCalib::cutoffDepthCurve, the normalized
                // contributions sum, and the sum is scaled once by
                // ModCalib::kCutoffModOctaves. lastLfo*Val_ is wave·depth, and with
                // no voices sounding no envelope can be modulating that depth, so
                // the base depth from bp is the effective one.
                float hypoOct = 0.0f;
                if (bp.lfo1Target == LfoTarget::Filter) hypoOct += lastLfo1Val_ * ModCalib::cutoffDepthCurve(bp.lfo1Depth);
                if (bp.lfo2Target == LfoTarget::Filter) hypoOct += lastLfo2Val_ * ModCalib::cutoffDepthCurve(bp.lfo2Depth);
                if (bp.lfo3Target == LfoTarget::Filter) hypoOct += lastLfo3Val_ * ModCalib::cutoffDepthCurve(bp.lfo3Depth);
                float hypo = bp.baseCutoff * std::pow(2.0f, hypoOct * ModCalib::kCutoffModOctaves);
                modulatedValues.filterCutoff.store(juce::jlimit(20.0f, 20000.0f, hypo), std::memory_order_relaxed);
            }
            else
            {
                modulatedValues.filterCutoff.store(NO_GHOST, std::memory_order_relaxed);
            }
        }

        // Filter resonance ghost
        {
            const bool aftertouchModResonance = bp.filterEnabled
                                             && bp.aftertouchTargetAmt[AftertouchTarget::Resonance] != 0.0f && hasVoices;
            modulatedValues.filterResonance.store(aftertouchModResonance
                ? voiceOut.lastModulatedResonance
                : NO_GHOST, std::memory_order_relaxed);
        }

        // Scan ghost
        {
            bool lfoModScan = bp.lfo1Target == LfoTarget::Scan || bp.lfo2Target == LfoTarget::Scan
                              || bp.lfo3Target == LfoTarget::Scan;
            bool envModScan = bp.ampTarget == EnvTarget::Scan
                           || anyModEnvTargets(bp, EnvTarget::Scan);
            bool driftModScan = std::abs(bp.driftScanOffset) > 0.001f && hasVoices;
            bool scanDrivenEngineActive = (bp.engineIsWavetable || bp.engineIsFreeze) && hasVoices;

            if (scanDrivenEngineActive)
            {
                modulatedValues.scanPosition.store(voiceOut.lastModulatedScan, std::memory_order_relaxed);
            }
            else if (hasVoices && (lfoModScan || envModScan || driftModScan))
            {
                modulatedValues.scanPosition.store(voiceOut.lastModulatedScan, std::memory_order_relaxed);
            }
            else if (lfoModScan)
            {
                float hypo = bp.baseScan;
                if (bp.lfo1Target == LfoTarget::Scan) hypo += lastLfo1Val_;
                if (bp.lfo2Target == LfoTarget::Scan) hypo += lastLfo2Val_;
                if (bp.lfo3Target == LfoTarget::Scan) hypo += lastLfo3Val_;
                modulatedValues.scanPosition.store(juce::jlimit(0.0f, 1.0f, hypo), std::memory_order_relaxed);
            }
            else
            {
                modulatedValues.scanPosition.store(NO_GHOST, std::memory_order_relaxed);
            }
        }

        // Noise level ghost
        {
            bool lfoModNoise = bp.lfo1Target == LfoTarget::NoiseLevel || bp.lfo2Target == LfoTarget::NoiseLevel
                               || bp.lfo3Target == LfoTarget::NoiseLevel;
            bool envModNoise = bp.ampTarget == EnvTarget::NoiseLevel
                            || anyModEnvTargets(bp, EnvTarget::NoiseLevel);

            if (hasVoices && (lfoModNoise || envModNoise))
            {
                modulatedValues.noiseLevel.store(
                    voiceOut.lastModulatedNoiseLevel, std::memory_order_relaxed);
            }
            else if (lfoModNoise)
            {
                float hypo = bp.noiseLevel;
                if (bp.lfo1Target == LfoTarget::NoiseLevel) hypo += lastLfo1Val_;
                if (bp.lfo2Target == LfoTarget::NoiseLevel) hypo += lastLfo2Val_;
                if (bp.lfo3Target == LfoTarget::NoiseLevel) hypo += lastLfo3Val_;
                modulatedValues.noiseLevel.store(
                    juce::jlimit(0.0f, 1.0f, hypo), std::memory_order_relaxed);
            }
            else
            {
                modulatedValues.noiseLevel.store(NO_GHOST, std::memory_order_relaxed);
            }
        }

        // LFO1/2 Rate/Depth ghost (env → LFO modulation, requires active voices)
        if (!skipSynthesis)
        {
            bool lfo1RateMod  = bp.ampTarget == EnvTarget::LFO1Rate
                              || anyModEnvTargets(bp, EnvTarget::LFO1Rate);
            bool lfo1DepthMod = bp.ampTarget == EnvTarget::LFO1Depth
                              || anyModEnvTargets(bp, EnvTarget::LFO1Depth);
            modulatedValues.lfo1Rate.store(lfo1RateMod ? lfo1.getRate() : NO_GHOST, std::memory_order_relaxed);
            float ghostLfo1Depth = bp.lfo1Depth;
            if (bp.ampTarget == EnvTarget::LFO1Depth) ghostLfo1Depth = applyNormalizedOffset(ghostLfo1Depth, voiceOut.lastAmpVal);
            for (int i = 0; i < kNumModEnvs; ++i)
                if (bp.modEnv[i].target == EnvTarget::LFO1Depth) ghostLfo1Depth = applyNormalizedOffset(ghostLfo1Depth, voiceOut.lastModVal[i]);
            modulatedValues.lfo1Depth.store(lfo1DepthMod ? ghostLfo1Depth : NO_GHOST, std::memory_order_relaxed);

            bool lfo2RateMod  = bp.ampTarget == EnvTarget::LFO2Rate
                              || anyModEnvTargets(bp, EnvTarget::LFO2Rate);
            bool lfo2DepthMod = bp.ampTarget == EnvTarget::LFO2Depth
                              || anyModEnvTargets(bp, EnvTarget::LFO2Depth);
            modulatedValues.lfo2Rate.store(lfo2RateMod ? lfo2.getRate() : NO_GHOST, std::memory_order_relaxed);
            float ghostLfo2Depth = bp.lfo2Depth;
            if (bp.ampTarget == EnvTarget::LFO2Depth) ghostLfo2Depth = applyNormalizedOffset(ghostLfo2Depth, voiceOut.lastAmpVal);
            for (int i = 0; i < kNumModEnvs; ++i)
                if (bp.modEnv[i].target == EnvTarget::LFO2Depth) ghostLfo2Depth = applyNormalizedOffset(ghostLfo2Depth, voiceOut.lastModVal[i]);
            modulatedValues.lfo2Depth.store(lfo2DepthMod ? ghostLfo2Depth : NO_GHOST, std::memory_order_relaxed);

            bool lfo3RateMod  = bp.ampTarget == EnvTarget::LFO3Rate
                              || anyModEnvTargets(bp, EnvTarget::LFO3Rate);
            bool lfo3DepthMod = bp.ampTarget == EnvTarget::LFO3Depth
                              || anyModEnvTargets(bp, EnvTarget::LFO3Depth);
            modulatedValues.lfo3Rate.store(lfo3RateMod ? lfo3.getRate() : NO_GHOST, std::memory_order_relaxed);
            float ghostLfo3Depth = bp.lfo3Depth;
            if (bp.ampTarget == EnvTarget::LFO3Depth) ghostLfo3Depth = applyNormalizedOffset(ghostLfo3Depth, voiceOut.lastAmpVal);
            for (int i = 0; i < kNumModEnvs; ++i)
                if (bp.modEnv[i].target == EnvTarget::LFO3Depth) ghostLfo3Depth = applyNormalizedOffset(ghostLfo3Depth, voiceOut.lastModVal[i]);
            modulatedValues.lfo3Depth.store(lfo3DepthMod ? ghostLfo3Depth : NO_GHOST, std::memory_order_relaxed);
        }
        else
        {
            modulatedValues.lfo1Rate.store(NO_GHOST, std::memory_order_relaxed);
            modulatedValues.lfo1Depth.store(NO_GHOST, std::memory_order_relaxed);
            modulatedValues.lfo2Rate.store(NO_GHOST, std::memory_order_relaxed);
            modulatedValues.lfo2Depth.store(NO_GHOST, std::memory_order_relaxed);
            modulatedValues.lfo3Rate.store(NO_GHOST, std::memory_order_relaxed);
            modulatedValues.lfo3Depth.store(NO_GHOST, std::memory_order_relaxed);
        }

        // LFO → Drift depth ghosts
        {
            auto computeDriftDepthGhost = [&](const char* paramId,
                                              int target,
                                              int driftTarget) -> float
            {
                // While a take freezes a generation slot, its Amount modulation is
                // suspended (see updateDriftState) — a ghost still sweeping there
                // would show a movement the engine has stopped reading.
                if (driftGenHold_.load(std::memory_order_relaxed)
                    && DriftLFO::isGenerationTarget(driftTarget))
                    return NO_GHOST;
                bool lfo1Mod = bp.lfo1Target == target;
                bool lfo2Mod = bp.lfo2Target == target;
                bool lfo3Mod = bp.lfo3Target == target;
                if (!lfo1Mod && !lfo2Mod && !lfo3Mod)
                    return NO_GHOST;

                float depth = parameters.getRawParameterValue(paramId)->load();
                if (lfo1Mod)
                    depth = applyNormalizedOffset(depth, lastLfo1Val_);
                if (lfo2Mod)
                    depth = applyNormalizedOffset(depth, lastLfo2Val_);
                if (lfo3Mod)
                    depth = applyNormalizedOffset(depth, lastLfo3Val_);
                return depth;
            };

            modulatedValues.drift1Depth.store(
                computeDriftDepthGhost(PID::drift1Depth, LfoTarget::Drift1Depth,
                                       static_cast<int>(paramCache.drift1Target->load())),
                std::memory_order_relaxed);
            modulatedValues.drift2Depth.store(
                computeDriftDepthGhost(PID::drift2Depth, LfoTarget::Drift2Depth,
                                       static_cast<int>(paramCache.drift2Target->load())),
                std::memory_order_relaxed);
            modulatedValues.drift3Depth.store(
                computeDriftDepthGhost(PID::drift3Depth, LfoTarget::Drift3Depth,
                                       static_cast<int>(paramCache.drift3Target->load())),
                std::memory_order_relaxed);
        }

        // Delay/Reverb ghosts (modulated by env or LFO targeting them)
        {
            bool dlyTimeMod = modDelayTime != 0.0f;
            bool dlyFbMod   = modDelayFb != 0.0f;
            bool dlyMixMod  = modDelayMix != 0.0f;
            bool revMixMod  = modReverbMix != 0.0f;
            const int delayClockGhost = static_cast<int>(paramCache.delayClockMode->load());
            const float delayBaseGhost = (delayClockGhost == ClockMode::Off)
                ? paramCache.delayTime->load()
                : ClockSync::computeDelayMs(syncBpm,
                    static_cast<int>(paramCache.delayClockDivision->load()));
            modulatedValues.delayTime.store(dlyTimeMod && delayEnabled
                ? juce::jlimit(1.0f, 2000.0f, delayBaseGhost * (1.0f + modDelayTime))
                : NO_GHOST, std::memory_order_relaxed);
            modulatedValues.delayFeedback.store(dlyFbMod && delayEnabled
                ? juce::jlimit(0.0f, 0.95f, paramCache.delayFeedback->load() * (1.0f + modDelayFb))
                : NO_GHOST, std::memory_order_relaxed);
            modulatedValues.delayMix.store(dlyMixMod && delayEnabled
                ? juce::jlimit(0.0f, 1.0f, paramCache.delayMix->load() + modDelayMix)
                : NO_GHOST, std::memory_order_relaxed);
            modulatedValues.reverbMix.store(revMixMod && reverbEnabled
                ? juce::jlimit(0.0f, 1.0f, paramCache.reverbMix->load() + modReverbMix)
                : NO_GHOST, std::memory_order_relaxed);
        }
    }

    // ── Master volume ───────────────────────────────────────────────────────
    buffer.applyGain(masterGain);

    // ── Output gain ─────────────────────────────────────────────────────────
    // The static half of what juce::dsp::Limiter used to do here. Ramped across
    // the block, because a moved control would otherwise step the whole mix by
    // whatever the control moved -- the widget smoothed the same value over
    // 1 ms for the same reason.
    {
        // Both reads are of SWITCH positions, not of anything the sounding
        // voices do, so this gain cannot couple one held note to another. The
        // ramp below is what keeps moving either control click-free.
        const float outGain = outputGainForThreshold(paramCache.limiterThresh->load(),
                                                     static_cast<int>(paramCache.voiceCount->load()));
        buffer.applyGainRamp(0, numSamples, outputGainPrev_, outGain);
        outputGainPrev_ = outGain;
    }

    // ── Output ceiling: the STANDALONE only ─────────────────────────────────
    // A host carries float and lets a plugin exceed 0 dBFS; the standalone hands
    // its buffer to a converter that clips hard, so only it needs a bound. Both
    // used to get the same stage because both run this function, which is how
    // the plugin came to carry the standalone's converter protection -- and, in
    // the compressor that stage used to be, a coupling between every sounding
    // voice. Memoryless here, so meeting the ceiling costs the waveform its top
    // and not every held note its level. See dsp/Limiter.h for what it does and
    // does not promise.
    //
    // The non-finite scrub is NOT gated: the widget's own unconditional
    // clip(-1, 1) used to keep an Inf from reaching any host, and removing the
    // stage from plugin builds was meant to free the LEVEL, not that.
    OutputCeiling::scrubNonFinite(buffer);
    if (wrapperType == wrapperType_Standalone)
        outputCeiling.processBlock(buffer);

    midiClockBlockStart_ += static_cast<uint64_t>(numSamples);
}

T5ynthProcessor::WtTraversalMapping T5ynthProcessor::makeWtTraversalMapping(int totalSamples) const
{
    const float p1 = masterSampler.getStartPos();
    const float p2 = masterSampler.getLoopStart();
    const float p3 = masterSampler.getLoopEnd();

    return makeWtTraversalMapping(totalSamples, p1, p2, p3);
}

T5ynthProcessor::WtTraversalMapping T5ynthProcessor::makeWtTraversalMapping(int totalSamples,
                                                                            float p1,
                                                                            float p2,
                                                                            float p3) const
{
    WtTraversalMapping mapping;

    mapping.extractStart = juce::jlimit(0.0f, 1.0f, std::min(p1, p2));
    mapping.extractEnd = juce::jlimit(0.0f, 1.0f, std::max(p1, p3));

    const float minWidth = totalSamples > 0
        ? 4.0f / static_cast<float>(totalSamples)
        : 0.01f;
    if (mapping.extractEnd - mapping.extractStart < minWidth)
    {
        mapping.extractEnd = juce::jmin(1.0f, mapping.extractStart + minWidth);
        if (mapping.extractEnd - mapping.extractStart < minWidth)
            mapping.extractStart = juce::jmax(0.0f, mapping.extractEnd - minWidth);
    }

    const float extractWidth = juce::jmax(0.0001f, mapping.extractEnd - mapping.extractStart);
    const float invWidth = 1.0f / extractWidth;

    mapping.startInExtract = juce::jlimit(0.0f, 1.0f, (p1 - mapping.extractStart) * invWidth);
    mapping.loopStartInExtract = juce::jlimit(0.0f, 1.0f, (p2 - mapping.extractStart) * invWidth);
    mapping.loopEndInExtract = juce::jlimit(0.0f, 1.0f, (p3 - mapping.extractStart) * invWidth);
    mapping.regionSamples = juce::jmax(1,
        static_cast<int>(std::round(extractWidth * static_cast<float>(juce::jmax(1, totalSamples)))));

    return mapping;
}

void T5ynthProcessor::syncWavetableTraversal(double bufferSampleRate, int totalSamples)
{
    if (totalSamples <= 0)
        return;

    const auto mapping = makeWtTraversalMapping(totalSamples);
    masterSampler.setWtExtractStart(mapping.extractStart);
    masterSampler.setWtExtractEnd(mapping.extractEnd);

    auto loopMode = masterSampler.getLoopMode();
    WavetableOscillator::LoopMode oscLoopMode;
    switch (loopMode)
    {
        case SamplePlayer::LoopMode::OneShot:  oscLoopMode = WavetableOscillator::LoopMode::OneShot;  break;
        case SamplePlayer::LoopMode::PingPong: oscLoopMode = WavetableOscillator::LoopMode::PingPong; break;
        default:                               oscLoopMode = WavetableOscillator::LoopMode::Loop;     break;
    }

    masterOsc.setAutoScanStartPos(mapping.startInExtract);
    masterOsc.setAutoScanLoop(mapping.loopStartInExtract, mapping.loopEndInExtract, oscLoopMode);
    if (paramCache.wtAutoScan->load() > 0.5f)
    {
        masterOsc.setAutoScan(true);
        masterOsc.setAutoScanRate(bufferSampleRate, mapping.regionSamples);
    }
    else
    {
        masterOsc.setAutoScan(false);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// MPE zone layout — juce::MPEZoneLayout, plus this synth's three deviations
// from it. Everything here runs on the audio thread and allocates nothing;
// tools/measure_mpe_instrument_rt.cpp is the measurement.
// ═══════════════════════════════════════════════════════════════════════════

bool T5ynthProcessor::handleMpeRpnByte(int channel, int cc, int value7) noexcept
{
    // Both detectors see the same bytes in the same order, so they stay in step.
    // This one exists only to answer what the layout cannot be asked: WHICH
    // parameter a completed RPN carried, and with what value.
    const auto channelBit = static_cast<juce::uint16>(1u << (channel - 1));

    if (isNrpnSelectController(cc))
    {
        // Remembered, and passed to NOBODY -- not to either detector, and not
        // consumed. CC98/CC99 stay bindable, which in XL DAW mode they must:
        // there they are the Lfo3 Amt and Drift3 Rate encoders.
        mpeNrpnSelected_ = static_cast<juce::uint16>(mpeNrpnSelected_ | channelBit);
        return false;
    }

    if (cc == 6 && (mpeNrpnSelected_ & channelBit) != 0)
        return false;   // an NRPN's data byte. Not a bend range, not a zone.

    // An RPN selection replaces the NRPN one, exactly as the register does.
    mpeNrpnSelected_ = static_cast<juce::uint16>(mpeNrpnSelected_ & ~channelBit);

    const auto parsed = mpeRpnWatcher_.tryParse(channel, cc, value7);

    // The layout sees the same bytes, minus the data byte that COMPLETES RPN 0.
    // It would store a bend range nothing here reads -- the pair below is this
    // synth's -- and it range-checks the value already in the zone while
    // assigning the new one unchecked, so a controller transmitting a range
    // above 96 fires JUCE's own assertion from the audio thread on the next
    // change (juce_MPEZoneLayout.cpp:150-168).
    //
    // parsed->isNRPN is deliberately NOT tested here or below: ChannelState
    // sets it only from CC98/CC99, which the early return above keeps out of
    // this detector entirely, so it is permanently false. Testing it would
    // read as the protection -- and the protection is that the bytes never
    // arrive.
    const bool completesBendRangeRpn = parsed.has_value()
                                       && parsed->parameterNumber == 0;

    if (! completesBendRangeRpn)
        mpeZones_.processNextMidiEvent(juce::MidiMessage::controllerEvent(channel, cc, value7));

    const bool actedOn = parsed.has_value()
                         && (parsed->parameterNumber == 0
                             || parsed->parameterNumber == juce::MPEMessages::zoneLayoutMessagesRpnNumber);

    if (actedOn && parsed->parameterNumber == 0)
    {
        // Pitch Bend Sensitivity. MPE puts one range on the master channel and
        // another on the members, and MPEZoneLayout stores them that way; this
        // synth keeps ONE pair for the whole instrument, which is what every
        // call site assumes and what survives a later zone declaration.
        //
        // A master-channel RPN therefore reaches the MEMBERS too. That is not
        // an oversight: a LinnStrument transmits its Bend Range on the master
        // channel only, and taking MPE at its word there leaves its member
        // notes bending at the default instead of the range the player set.
        //
        // The floor of 1 is the hand-written parser's -- a transmitted 0 meant
        // one semitone, not none. Value 0 is what a controller sends to mean
        // "no bend", and honouring that would silently disable pitch bend.
        const int range = std::max(1, value7);
        mpePerNoteBendRangeInForce_ = range;
        if (isMpeMasterChannel(channel))
            mpeMasterBendRangeInForce_ = range;
    }
    else if (actedOn)
    {
        // The MPE Configuration Message is a one-shot declaration, but
        // MidiRPNDetector keeps the selected parameter latched per channel --
        // so the NEXT CC6 on this channel, a bound fader included, would be
        // read as another zone message and rewrite the layout from its
        // position. The layout itself is now juce::MPEZoneLayout's to keep.
        deselectMpeRpn(channel, /*lsbOnly=*/true);
    }

    // CC100/CC101 are RPN selection and nothing else, so they are always
    // consumed. A CC6 is only ours when it completed an RPN we act on;
    // otherwise it falls through to the user bindings below, exactly as it did
    // before.
    return cc != 6 || actedOn;
}

void T5ynthProcessor::deselectMpeRpn(int channel, bool lsbOnly) noexcept
{
    // Expressed as the MIDI a controller would send, so the library's own
    // detectors do the deselecting and no parser state is kept here -- bar the
    // one bit the library has no place for.
    mpeNrpnSelected_ = static_cast<juce::uint16>(mpeNrpnSelected_ & ~(1u << (channel - 1)));

    const auto feed = [this, channel](int cc, int value)
    {
        mpeRpnWatcher_.tryParse(channel, cc, value);
        mpeZones_.processNextMidiEvent(juce::MidiMessage::controllerEvent(channel, cc, value));
    };

    if (! lsbOnly)
        feed(101, 0x7F);

    // The LSB alone after a zone message: CC100=0 followed by CC6 is a legal way
    // for a controller to send a bend range next, and it must still find RPN 0's
    // MSB waiting.
    feed(100, 0x7F);
}

void T5ynthProcessor::updateDriftState(int numSamples, float syncBpm)
{
    driftLfo.setRegenMode(static_cast<int>(paramCache.driftRegen->load()));

    const int d1t = static_cast<int>(paramCache.drift1Target->load());
    const int d2t = static_cast<int>(paramCache.drift2Target->load());
    const int d3t = static_cast<int>(paramCache.drift3Target->load());

    bool driftHasTarget = (d1t != 0) || (d2t != 0) || (d3t != 0);
    bool hasOsc = false;
    for (int t : { d1t, d2t, d3t })
        if ((t >= DriftLFO::TgtAlpha && t <= DriftLFO::TgtAxis3)
            || t == DriftLFO::TgtNoise || t == DriftLFO::TgtMagnitude
            || t == DriftLFO::TgtResynth)
            hasOsc = true;

    driftHasOscTarget.store(hasOsc, std::memory_order_relaxed);
    driftRegenMode.store(static_cast<int>(paramCache.driftRegen->load()),
                         std::memory_order_relaxed);
    driftRegenBpm.store(syncBpm, std::memory_order_relaxed);

    bool driftManualEnable = paramCache.driftEnabled->load() > 0.5f;
    driftLfo.setEnabled(driftHasTarget || driftManualEnable);

    // Per-Drift sync-rate resolution — same pattern as LFO override.
    auto driftRate = [&](const char* clockPid, const char* divPid, const char* ratePid) {
        const int cm = static_cast<int>(parameters.getRawParameterValue(clockPid)->load());
        if (cm == ClockMode::Off)
            return parameters.getRawParameterValue(ratePid)->load();
        const int divIdx = juce::jlimit(0, DriftDivision::kCount - 1,
            static_cast<int>(parameters.getRawParameterValue(divPid)->load()));
        return ClockSync::computeRateFromFactor(syncBpm, DriftDivision::kFactor[divIdx]);
    };

    // LFO1/2/3 → Drift1/2/3 Amt must land HERE, before tick()/getOffsetForTarget
    // below consume driftLfo's depth — every real consumer of that depth (the
    // Alpha/Axis/Noise/Magnitude/Resynth offsets further down in this
    // function, plus WtScan/Filter/Pitch/EnvAmt and Delay/Reverb right after
    // this function returns) reads it within THIS block, so a later overwrite
    // is silently discarded on the very next call. bp isn't populated yet at
    // this point in processBlock, so read the LFO targets straight off
    // paramCache; lastLfoXVal_ is the previous block's LFO end-value (this
    // block's own LFOs haven't ticked yet either) — the ghost computation
    // further down uses the SAME member, just refreshed to this block's value
    // by the time it reads it, so ghost and sound are one block apart, same
    // lag as every other block-rate LFO ghost/effect pair in this file.
    const int lt1 = static_cast<int>(paramCache.lfo1Target->load());
    const int lt2 = static_cast<int>(paramCache.lfo2Target->load());
    const int lt3 = static_cast<int>(paramCache.lfo3Target->load());
    // Offline cache take, read here because the DEPTH has to freeze with the phase:
    // getOffsetForTarget multiplies the two, so a main LFO routed to Drift-N Amt
    // would keep a "held" generation value moving in real time and put machine
    // timing straight back into the recorded trajectory. The knob alone is also what
    // the take's precondition judges on (PromptPanel::driftCanCarryATake), so both
    // ends speak about the same quantity. Audible slots keep their modulation.
    const bool genHold = driftGenHold_.load(std::memory_order_relaxed);
    auto modulatedDriftDepth = [&](const std::atomic<float>* depthParam, int target,
                                   int driftTarget)
    {
        float depth = depthParam->load();
        if (genHold && DriftLFO::isGenerationTarget(driftTarget))
            return depth;
        if (lt1 == target) depth = applyNormalizedOffset(depth, lastLfo1Val_);
        if (lt2 == target) depth = applyNormalizedOffset(depth, lastLfo2Val_);
        if (lt3 == target) depth = applyNormalizedOffset(depth, lastLfo3Val_);
        return depth;
    };

    driftLfo.setLfoRate(0, driftRate(PID::drift1ClockMode, PID::drift1ClockDivision, PID::drift1Rate));
    driftLfo.setLfoDepth(0, modulatedDriftDepth(paramCache.drift1Depth, LfoTarget::Drift1Depth, d1t));
    driftLfo.setLfoTarget(0, d1t);
    driftLfo.setLfoWaveform(0, static_cast<int>(paramCache.drift1Wave->load()));
    driftLfo.setLfoRate(1, driftRate(PID::drift2ClockMode, PID::drift2ClockDivision, PID::drift2Rate));
    driftLfo.setLfoDepth(1, modulatedDriftDepth(paramCache.drift2Depth, LfoTarget::Drift2Depth, d2t));
    driftLfo.setLfoTarget(1, d2t);
    driftLfo.setLfoWaveform(1, static_cast<int>(paramCache.drift2Wave->load()));
    driftLfo.setLfoRate(2, driftRate(PID::drift3ClockMode, PID::drift3ClockDivision, PID::drift3Rate));
    driftLfo.setLfoDepth(2, modulatedDriftDepth(paramCache.drift3Depth, LfoTarget::Drift3Depth, d3t));
    driftLfo.setLfoTarget(2, d3t);
    driftLfo.setLfoWaveform(2, static_cast<int>(paramCache.drift3Wave->load()));

    // Offline cache take. The hold has to be applied before the step and the step
    // before the tick: the step is the take's move from one cache point to the
    // next and must travel at the rates just written above, and it must land
    // before tick() so a single block cannot both step and free-run. exchange()
    // rather than load+store so a step requested between the two never vanishes.
    driftLfo.setGenerationHold(genHold);
    const float pendingDriftStep = driftGenStepSec_.exchange(0.0f, std::memory_order_relaxed);
    if (pendingDriftStep > 0.0f)
        driftLfo.stepGenerationTargets(static_cast<double>(pendingDriftStep));

    driftLfo.tick(static_cast<double>(numSamples) / getSampleRate());

    static constexpr float NO_GHOST = std::numeric_limits<float>::quiet_NaN();
    const float alphaOff = driftLfo.getOffsetForTarget(DriftLFO::TgtAlpha);
    const float ax1Off   = driftLfo.getOffsetForTarget(DriftLFO::TgtAxis1);
    const float ax2Off   = driftLfo.getOffsetForTarget(DriftLFO::TgtAxis2);
    const float ax3Off   = driftLfo.getOffsetForTarget(DriftLFO::TgtAxis3);
    const float noiseOff = driftLfo.getOffsetForTarget(DriftLFO::TgtNoise);
    const float magOff   = driftLfo.getOffsetForTarget(DriftLFO::TgtMagnitude);
    const float resynthOff = driftLfo.getOffsetForTarget(DriftLFO::TgtResynth);
    const float baseAlpha = paramCache.genAlpha->load();
    const float baseNoise = paramCache.genNoise->load();
    const float baseMag = paramCache.genMagnitude->load();
    const float baseResynth = paramCache.resynthAmount->load();

    modulatedValues.driftAlpha.store(
        std::abs(alphaOff) > 0.001f ? baseAlpha + alphaOff : NO_GHOST,
        std::memory_order_relaxed);
    modulatedValues.driftAxis1.store(
        std::abs(ax1Off) > 0.001f ? ax1Off : NO_GHOST, std::memory_order_relaxed);
    modulatedValues.driftAxis2.store(
        std::abs(ax2Off) > 0.001f ? ax2Off : NO_GHOST, std::memory_order_relaxed);
    modulatedValues.driftAxis3.store(
        std::abs(ax3Off) > 0.001f ? ax3Off : NO_GHOST, std::memory_order_relaxed);
    modulatedValues.driftNoise.store(
        std::abs(noiseOff) > 0.001f ? baseNoise + noiseOff : NO_GHOST,
        std::memory_order_relaxed);
    modulatedValues.driftMagnitude.store(
        std::abs(magOff) > 0.001f ? baseMag + magOff : NO_GHOST,
        std::memory_order_relaxed);
    modulatedValues.driftResynth.store(
        std::abs(resynthOff) > 0.001f ? juce::jlimit(0.0f, 1.0f, baseResynth + resynthOff)
                                      : NO_GHOST,
        std::memory_order_relaxed);
}

bool T5ynthProcessor::seqRunningNow() const
{
    const bool stepOn = paramCache.seqRunning->load() > 0.5f;
    const bool genOn  = paramCache.genSeqRunning->load() > 0.5f;
    return stepOn || genOn;
}

float T5ynthProcessor::resolveSyncBpm() const
{
    if (midiClockEnabled_.load(std::memory_order_relaxed)
        && midiClockValid_.load(std::memory_order_relaxed))
        return midiClockBpm_.load(std::memory_order_relaxed);
    if (hostPlayingNow.load(std::memory_order_relaxed))
        return hostBpmLastSeen.load(std::memory_order_relaxed);
    if (seqRunningNow())
        return paramCache.seqBpm->load();
    const float h = hostBpmLastSeen.load(std::memory_order_relaxed);
    return (h > 0.0f) ? h : paramCache.seqBpm->load();
}

bool  T5ynthProcessor::isMidiClockActive()  const noexcept
{
    return midiClockEnabled_.load(std::memory_order_relaxed)
        && midiClockValid_.load(std::memory_order_relaxed);
}
float T5ynthProcessor::getMidiClockBpm()    const noexcept { return midiClockBpm_.load(std::memory_order_relaxed); }
bool  T5ynthProcessor::isMidiClockEnabled() const noexcept { return midiClockEnabled_.load(std::memory_order_relaxed); }

void T5ynthProcessor::setMidiClockEnabled(bool e)
{
    // Only flip the atomics here — called from message thread.
    // midiClockTickCount_ / midiClockLastTick_ are audio-thread-only;
    // the audio thread resets them when it first sees enabled=false.
    midiClockEnabled_.store(e, std::memory_order_release);
    if (!e)
        midiClockValid_.store(false, std::memory_order_release);
}

bool T5ynthProcessor::isWavetableMode() const
{
    // LCO is a Wavetable bake with a distinct preset identity — GUI callers
    // (WT display, engine window) treat it identically to plain Wavetable.
    const int m = static_cast<int>(paramCache.engineMode->load());
    return m == EngineMode::Wavetable || m == EngineMode::Lco;
}

bool T5ynthProcessor::isFreezeMode() const
{
    return static_cast<int>(paramCache.engineMode->load()) == EngineMode::Freeze;
}

bool T5ynthProcessor::isSamplerMode() const
{
    return static_cast<int>(paramCache.engineMode->load()) == EngineMode::Sampler;
}

// The region a freshly generated buffer offers: where its audible content sits,
// and the loop window a Sampler should prefer inside it. Buffer, rate and loop
// mode decide it and nothing else.
//
// Lifted out of loadGeneratedAudio unchanged so audio that is NOT the live
// generation can be prepared the same way. A stored position whose loop window
// came from some other analysis would not be the sound that playing the same
// position from the CACHE row produces, and those two must not drift apart.
namespace
{
struct GeneratedRegions
{
    float activeStartFrac = 0.0f;
    float activeEndFrac   = 1.0f;
    float loopStartFrac   = 0.0f;
    float loopEndFrac     = 1.0f;
};

GeneratedRegions analyzeGeneratedRegions (const juce::AudioBuffer<float>& feedBuffer,
                                          double sr,
                                          SamplePlayer::LoopMode samplerLoopMode)
{
    float loopStartFrac = 0.0f;
    float loopEndFrac   = 1.0f;
    float activeStartFrac = 0.0f;
    float activeEndFrac   = 1.0f;

    // getReadPointer(0) below needs a channel to exist; a sample count alone
    // never guaranteed one.
    if (feedBuffer.getNumChannels() > 0)
    {

        const int numSamples = feedBuffer.getNumSamples();

        if (numSamples > 0)
        {
            const float* data = feedBuffer.getReadPointer(0);

            // Sustained windowed-RMS detection. The previous peak-per-window
            // algorithm tripped on isolated noise samples — VAE/decoder
            // artefacts in the -45..-55 dB band would anchor the start point
            // long before the actual content began. We now require ≥3
            // consecutive ~10 ms windows of meaningful RMS energy.
            const int windowSize = juce::jmax(64, static_cast<int>(std::round(sr * 0.010)));
            const int numWindows = numSamples / windowSize;
            constexpr int minSustainedWindows = 3;

            int firstActive = 0;
            int lastActive  = numSamples;

            if (numWindows >= minSustainedWindows)
            {
                std::vector<float> windowRms(static_cast<size_t>(numWindows), 0.0f);
                float globalPeakRms = 0.0f;
                for (int w = 0; w < numWindows; ++w)
                {
                    const float* base = data + w * windowSize;
                    double sumSq = 0.0;
                    for (int i = 0; i < windowSize; ++i)
                    {
                        double s = static_cast<double>(base[i]);
                        sumSq += s * s;
                    }
                    float rms = std::sqrt(static_cast<float>(sumSq / windowSize));
                    windowRms[static_cast<size_t>(w)] = rms;
                    globalPeakRms = std::max(globalPeakRms, rms);
                }

                // -35 dB from peak RMS captures musically present content while
                // rejecting low-level decoder hiss; floor at -50 dB absolute so
                // genuinely quiet generations don't collapse to threshold 0.
                const float relThreshold = globalPeakRms * 0.01778f; // -35 dB
                constexpr float absFloor = 0.00316f;                 // -50 dB
                const float threshold = std::max(relThreshold, absFloor);

                int run = 0;
                int firstRunStart = -1;
                int lastRunEnd    = -1;
                for (int w = 0; w < numWindows; ++w)
                {
                    if (windowRms[static_cast<size_t>(w)] > threshold)
                    {
                        ++run;
                        if (run >= minSustainedWindows)
                        {
                            if (firstRunStart < 0)
                                firstRunStart = w - minSustainedWindows + 1;
                            lastRunEnd = w + 1;
                        }
                    }
                    else
                    {
                        run = 0;
                    }
                }

                if (firstRunStart >= 0)
                {
                    firstActive = firstRunStart * windowSize;
                    lastActive  = juce::jmin(lastRunEnd * windowSize, numSamples);
                }
            }

            // Small margin (one window each side) — preserves natural attack/release
            firstActive = juce::jmax(0, firstActive - windowSize);
            lastActive  = juce::jmin(numSamples, lastActive + windowSize);

            activeStartFrac = static_cast<float>(firstActive) / static_cast<float>(numSamples);
            activeEndFrac   = static_cast<float>(lastActive)  / static_cast<float>(numSamples);

            if (activeEndFrac - activeStartFrac < 0.05f)
            {
                activeStartFrac = 0.0f;
                activeEndFrac   = 1.0f;
                firstActive = 0;
                lastActive = numSamples;
            }

            loopStartFrac = activeStartFrac;
            loopEndFrac   = activeEndFrac;

            // Sampler loops should prefer a stable sustain excerpt instead of
            // the full generated evolution. Looping the entire active region of
            // AI material often creates slow macro-dynamics that read like a
            // "broken normalize" even when the gain is static.
            if (samplerLoopMode != SamplePlayer::LoopMode::OneShot)
            {
                const int activeLen = lastActive - firstActive;
                const int minLoopSamples = static_cast<int>(std::round(sr * 1.5));
                const int maxLoopSamples = static_cast<int>(std::round(sr * 6.0));
                int targetLoopSamples = juce::jlimit(minLoopSamples, maxLoopSamples, activeLen / 3);

                if (activeLen > targetLoopSamples + windowSize)
                {
                    std::vector<double> powerPrefix(static_cast<size_t>(numSamples + 1), 0.0);
                    for (int i = 0; i < numSamples; ++i)
                    {
                        double s = static_cast<double>(data[i]);
                        powerPrefix[static_cast<size_t>(i + 1)]
                            = powerPrefix[static_cast<size_t>(i)] + s * s;
                    }

                    int bestStart = firstActive;
                    double bestPower = -1.0;
                    const int latestStart = lastActive - targetLoopSamples;
                    for (int pos = firstActive; pos <= latestStart; pos += windowSize)
                    {
                        const int end = pos + targetLoopSamples;
                        const double sumSq = powerPrefix[static_cast<size_t>(end)]
                                           - powerPrefix[static_cast<size_t>(pos)];
                        const double avgPower = sumSq / static_cast<double>(targetLoopSamples);
                        if (avgPower > bestPower)
                        {
                            bestPower = avgPower;
                            bestStart = pos;
                        }
                    }

                    loopStartFrac = static_cast<float>(bestStart) / static_cast<float>(numSamples);
                    loopEndFrac   = static_cast<float>(bestStart + targetLoopSamples)
                                  / static_cast<float>(numSamples);
                }
            }
        }

    }

    return { activeStartFrac, activeEndFrac, loopStartFrac, loopEndFrac };
}
}  // namespace

juce::AudioBuffer<float> T5ynthProcessor::conditionGeneratedSource (const juce::AudioBuffer<float>& source,
                                                                    double sr,
                                                                    bool hfBoost) const
{
    // Everything a freshly generated buffer goes through before anything measures
    // or plays it. Lifted out of loadGeneratedAudio unchanged, for the same reason
    // analyzeGeneratedRegions was: audio that is NOT the live generation has to
    // arrive in exactly this state, or it is not the same sound.

    // Rumble filter — always on, removes DC/sub-bass from VAE output
    juce::AudioBuffer<float> conditioned;
    conditioned.makeCopyOf(source);
    applyRumbleFilter(conditioned, sr);

    // Conditionally apply HF boost to compensate VAE decoder rolloff
    if (hfBoost)
        applyHfBoost(conditioned, sr);

    // Pre-trim leading silence BEFORE computing activeStartFrac/activeEndFrac.
    // prepareBufferLoad() also trims internally; doing it here makes the trim
    // idempotent and ensures the fractions we compute below align with the
    // buffer the sampler ultimately plays. Without this the new sustained-RMS
    // trim would shift the buffer after we'd already measured an audible-start
    // fraction, landing P1 past the real attack.
    masterSampler.trimLeadingSilencePublic(conditioned);

    // Symmetric trailing trim: diffusion models emit the full requested duration
    // even when the sound is short, leaving a dead near-silent tail. The granular
    // engine (scan 0..1 across the whole buffer, no playhead) otherwise parks in
    // that pure-zero field, and the waveform/playhead show a flat tail. Drop it
    // here — before the active-region fractions below are computed — so sampler,
    // wavetable, freeze and the display all end at real content. No-op when the
    // content already runs to the end.
    masterSampler.trimTrailingSilencePublic(conditioned);
    return conditioned;
}

void T5ynthProcessor::loadGeneratedAudio(const juce::AudioBuffer<float>& audioBuffer, double sr)
{
    samplerProcessorDebugLog("loadGeneratedAudio begin samples=" + juce::String(audioBuffer.getNumSamples())
                             + " sr=" + juce::String(sr, 2)
                             + " masterBefore={" + masterSampler.debugStateString() + "}");

    // NOTE (BJ 2026-07-22): this used to be where a bake's engine stash was
    // spent — a fresh generation handed the user back the engine the bake had
    // forced away. That made loading audio a paradigm switch, which is not what
    // it is: it fired on every audio reload (HF-boost reprocess, snapshot
    // recall, preset audio, an auto-regen landing) and could pull the engine
    // out from under a panel still showing the LCO, while the case it was meant
    // for — leaving the LCO — went unhandled whenever no generation followed or
    // the session had started in a language mode with no stash to spend. The
    // oscillator-mode toggle owns that switch now (restoreNeuralEngineMode /
    // restoreLanguageEngineMode); loading audio only loads audio.

    // Store raw audio (unmodified) for preset embedding and re-apply on toggle
    if (&audioBuffer != &generatedAudioRaw)
        generatedAudioRaw.makeCopyOf(audioBuffer);

    const bool hfOn = paramCache.genHfBoost->load() > 0.5f;
    juce::AudioBuffer<float> cleanBuffer = conditionGeneratedSource (audioBuffer, sr, hfOn);

    const auto& feedBuffer = cleanBuffer;

    SamplePlayer::LoopMode samplerLoopMode = SamplePlayer::LoopMode::Loop;
    SamplePlayer::PrepareConfig samplerConfig;
    bool autoPositionPoints = false;
    float prevP1 = 0.0f;
    {
        const juce::ScopedLock sl (getCallbackLock());
        syncSamplerSettingsFromParametersLocked();
        samplerConfig = masterSampler.capturePrepareConfig();
        samplerLoopMode = samplerConfig.loopMode;
        autoPositionPoints = !masterSampler.getPointsLocked();
        prevP1 = masterSampler.getStartPos();
    }

    // ── Auto-position P1/P2/P3 BEFORE loadBuffer so that preparePlaybackBuffer
    //    (which runs normalization) already sees the correct region. ──────
    float loopStartFrac = 0.0f;
    float loopEndFrac   = 1.0f;
    float activeStartFrac = 0.0f;
    float activeEndFrac   = 1.0f;

    if (autoPositionPoints)
    {
        const auto regions = analyzeGeneratedRegions (feedBuffer, sr, samplerLoopMode);
        activeStartFrac = regions.activeStartFrac;
        activeEndFrac   = regions.activeEndFrac;
        loopStartFrac   = regions.loopStartFrac;
        loopEndFrac     = regions.loopEndFrac;

        samplerConfig.loopStartFrac = loopStartFrac;
        samplerConfig.loopEndFrac = loopEndFrac;

        // P1: preserve the user's choice where possible, but clamp against
        // the active audio region rather than the loop window.
        if (prevP1 < activeStartFrac || prevP1 > activeEndFrac)
            samplerConfig.startPosFrac = activeStartFrac;
    }

    const bool wavetableMode = isWavetableMode();
    const auto wtMapping = makeWtTraversalMapping(feedBuffer.getNumSamples(),
                                                  samplerConfig.startPosFrac,
                                                  samplerConfig.loopStartFrac,
                                                  samplerConfig.loopEndFrac);
    const float extractStart = wavetableMode ? wtMapping.extractStart
                                             : samplerConfig.loopStartFrac;
    const float extractEnd   = wavetableMode ? wtMapping.extractEnd
                                             : samplerConfig.loopEndFrac;

    constexpr int frameCounts[] = {32, 64, 128, 256};
    int fcIdx = static_cast<int>(paramCache.wtFrames->load());
    int maxFrames = frameCounts[juce::jlimit(0, 3, fcIdx)];

    auto preparedSamplerLoad = masterSampler.prepareBufferLoad(feedBuffer, sr, samplerConfig);
    storeSamplerReprepareSource(preparedSamplerLoad.originalBuffer, sr, activeStartFrac, activeEndFrac);
    auto preparedFreezeBuffer = makeFreezeLoadBuffer(feedBuffer,
                                                     sr,
                                                     samplerConfig.normalizeOn,
                                                     activeStartFrac,
                                                     activeEndFrac,
                                                     masterSampler);
    juce::AudioBuffer<float> preparedGeneratedAudio;
    preparedGeneratedAudio.makeCopyOf(feedBuffer);
    juce::AudioBuffer<float> preparedWaveformSnapshot;
    const bool normDisplay = wavetableMode
        || (paramCache.normalize->load() > 0.5f);
    if (feedBuffer.getNumChannels() > 0 && feedBuffer.getNumSamples() > 0)
    {
        preparedWaveformSnapshot.setSize(1, feedBuffer.getNumSamples(), false, false, true);
        preparedWaveformSnapshot.copyFrom(0, 0, feedBuffer, 0, 0, feedBuffer.getNumSamples());

        if (normDisplay)
        {
            float peak = 0.0f;
            const float* d = preparedWaveformSnapshot.getReadPointer(0);
            for (int i = 0; i < preparedWaveformSnapshot.getNumSamples(); ++i)
                peak = std::max(peak, std::abs(d[i]));
            if (peak > 0.001f)
                preparedWaveformSnapshot.applyGain(0.95f / peak);
        }
    }

    // Off-lock compute: extraction + FFT mip-levels only, no publish (see
    // WavetableOscillator::prepareFramesFromBuffer's doc comment). The actual
    // publish happens inside the lock below, alongside masterSampler's/
    // masterFreeze's. publishedMipData_ publishes through the real atomic
    // free-function API (that is what covers the CLAP build, which holds no
    // lock around processBlock at all); it is still computed off-lock here so
    // the expensive extraction/FFT work never delays the audio thread's next
    // processBlock on the formats where this lock IS in effect.
    auto preparedMipData = wavetableMode
        ? masterOsc.prepareFramesFromBuffer(feedBuffer, sr, extractStart, extractEnd, maxFrames)
        : masterOsc.prepareContiguousFrames(feedBuffer, sr, extractStart, extractEnd);
    // Off-lock compute: mixdown only, no publish (see FreezeTextureEngine::
    // prepareBufferLoad's doc comment). The actual publish happens inside the
    // lock below, alongside masterSampler's. masterFreeze.publishedSnapshot_
    // publishes through the real atomic free-function API (see its
    // declaration in FreezeTextureEngine.h) — computed off-lock here purely to
    // keep the expensive mixdown from delaying the audio thread's next
    // processBlock on the formats where this lock is in effect.
    auto preparedFreezeSnapshot = masterFreeze.prepareBufferLoad(preparedFreezeBuffer, sr);

    {
        // Guard engine-state mutation against the realtime callback. The Linux
        // standalone path takes this same lock around processBlock().
        const juce::ScopedLock sl (getCallbackLock());

        generatedSampleRate = sr;

        // Keep generatedAudioFull in sync (used for waveform display + presets)
        generatedAudioFull = std::move(preparedGeneratedAudio);

        // Release the reclaim slot the audio thread parked after the previous
        // regenerate (off-thread free), sequenced before publishing the new
        // snapshot so it cannot overlap a fresh audio-thread adoption.
        voiceManager.drainRetiredSamplerSnapshots();

        // Publish the already-prepared sampler, freeze, and wavetable state inside
        // the lock. All three snapshot pointers publish through the real atomic
        // free-function API (that is what actually excludes the audio thread on
        // CLAP, which holds no lock around processBlock); the lock here is real
        // and necessary on Standalone/VST3/AU and for the OTHER, non-atomic
        // engine state this same critical section also touches. masterOsc's
        // publish must land before distributeWavetableFrames below, which reads it.
        masterSampler.applyPreparedBufferLoad(std::move(preparedSamplerLoad), samplerConfig);
        masterFreeze.applyPreparedBufferLoad(std::move(preparedFreezeSnapshot));
        masterOsc.applyPreparedMipData(std::move(preparedMipData));

        dcoTableActive_.store(false, std::memory_order_relaxed);  // neural frames own masterOsc again
        clearLcoBakeSnapshot();  // masterOsc is neural again — an LCO save block would be stale
        // Neural regen gives masterOsc fresh, definitely-not-stale content.
        syncWavetableTraversal(sr, feedBuffer.getNumSamples());
        masterOsc.setMorphTimeMs(paramCache.driftCrossfade->load());

        // A HELD note plays the freshly generated sample: held sampler voices
        // crossfade onto the new snapshot on the next audio-thread distribute pass
        // (false here — morphToBufferFrom's contract confines it to the audio
        // thread, see SamplePlayer.h; this message-thread call leaves the
        // crossfade to start on processBlock's own redistribute pass instead).
        voiceManager.distributeSamplerBuffer(masterSampler, 0.0f, /*allowMorph=*/false);
        voiceManager.distributeWavetableFrames(masterOsc);
        // New inference → held granular voices crossfade-adopt it live (near
        // real-time), mirroring distributeWavetableFrames above. Off the audio
        // thread (under getCallbackLock), so morphToBufferFrom is RT-safe here.
        voiceManager.distributeFreezeBuffer(masterFreeze, paramCache.driftCrossfade->load(), true);

        samplerProcessorDebugLog("loadGeneratedAudio end masterAfter={" + masterSampler.debugStateString() + "}");

        // Snapshot channel 0 for waveform display
        if (preparedWaveformSnapshot.getNumSamples() > 0)
        {
            waveformSnapshot = std::move(preparedWaveformSnapshot);
            newWaveformReady.store(true, std::memory_order_release);
        }
    }

    // Neural wavetable frames feed the engine-window 2.5D fan too (source-agnostic
    // display). Outside the lock — the strip build allocates. Sampler/Freeze skip.
    if (wavetableMode)
        publishWtDisplayFromOscFrames();
}

void T5ynthProcessor::loadDcoWavetable(const juce::AudioBuffer<float>& frameStrip,
                                       float motionRateHz)
{
    // Message thread. The strip is N baked single cycles laid end-to-end
    // (mono, N*2048 samples) — extractContiguousFrames re-slices it on exact
    // frame boundaries (no pitch detection, no resampling). Publish discipline
    // mirrors loadGeneratedAudio: frame-slicing off the lock (prepareExactFrames,
    // no publish — publishedMipData_ publishes through the real atomic
    // free-function API, which is what covers CLAP; kept off-lock here purely
    // so the extraction work never delays the audio thread's next processBlock
    // on the formats where this lock is in effect), traversal/morph/distribute
    // under it (applyPreparedMipData first, so distributeWavetableFrames sees it).
    if (frameStrip.getNumChannels() < 1
        || frameStrip.getNumSamples() < WavetableOscillator::FRAME_SIZE)
        return;

    const double sr = getSampleRate() > 0.0 ? getSampleRate() : 44100.0;

    // The DCO is a wavetable source: make it audible. Host-visible param
    // change, message thread, before the engine data lands so the block-param
    // mapper picks both up together. Remember the engine the user came from
    // so the next neural generation can restore it — a re-bake while the DCO
    // is already active keeps the original stash (the pre-DCO engine), and a
    // user genuinely on Wavetable for neural audio has nothing to restore.
    {
        const int cur = static_cast<int>(paramCache.engineMode->load());
        if (cur != EngineMode::Lco)
            dcoPrevEngineMode_ = cur;
        else if (!dcoTableActive_.load(std::memory_order_relaxed))
            dcoPrevEngineMode_ = -1;
    }
    if (auto* engineParam = parameters.getParameter(PID::engineMode))
        engineParam->setValueNotifyingHost(
            engineParam->convertTo0to1(static_cast<float>(EngineMode::Lco)));
    // Movement by default: a fresh bake must move continuously, so force the
    // transport to Loop — the param's registered default (the sampler's
    // One-shot) would freeze the motion after one pass. Same host-visible
    // write pattern as engineMode above, so the Loop button lights up; the
    // loop-mode buttons keep full control afterwards.
    if (auto* loopParam = parameters.getParameter(PID::loopMode))
        loopParam->setValueNotifyingHost(
            loopParam->convertTo0to1(static_cast<float>(LoopMode::Loop)));
    // Movement by default, continued: the DCO/LCO table's motion now runs
    // through the SAME standard AutoScan transport used everywhere else (the
    // oscillator owns no private motion), so make the AutoScan toggle
    // host-visible too — same write pattern as engineMode/loopMode above, so
    // the AutoScan button lights up and the user can switch it off immediately
    // after (mirrored onto masterOsc every block — see the dcoTableActive_
    // branch in processBlock).
    if (auto* autoScanParam = parameters.getParameter(PID::wtAutoScan))
        autoScanParam->setValueNotifyingHost(1.0f);

    // Bit-exact adoption — NOT extractContiguousFrames: its seam ramp and
    // per-frame renorm are corrections for arbitrary neural slices and
    // audibly corrupt exact closed-form cycles (setExactFrames doc). Also
    // marks the table content-seamless so auto-scan Loop wraps don't slew
    // back through the whole table (the "dropout every table-pass" defect).
    // Off-lock compute only (prepareExactFrames) — see this function's top
    // comment; the publish happens inside the lock below via
    // applyPreparedMipData().
    auto preparedMipData = masterOsc.prepareExactFrames(frameStrip);

    {
        // Guard engine-state mutation against the realtime callback (same
        // rule as loadGeneratedAudio).
        const juce::ScopedLock sl (getCallbackLock());

        // Publish before anything below reads it (setAutoScanLoop/setAutoScan/
        // distributeWavetableFrames all act on the newly adopted bank).
        masterOsc.applyPreparedMipData(std::move(preparedMipData));

        // NOT syncWavetableTraversal(): it re-derives extract brackets and rate
        // from a neural buffer, which do not apply to a DCO table. The DCO
        // table is an authored gesture with its own tempo, but it now moves
        // through the SAME standard AutoScan transport as neural mode (the
        // oscillator owns no private motion) — full range [0,1] (a DCO gesture
        // has no source brackets), Loop by default (already made host-visible
        // above). The recipe's motion_rate_hz sets the tempo; older recipes
        // without it fall back to the legacy strip-length rate. Movement is
        // always on at load (matching the prior behaviour); the AutoScan
        // toggle write above lets the user switch it off immediately after
        // (mirrored onto masterOsc every block — see processBlock), and the
        // manual Scan control and modulation still ADD on top.
        masterOsc.setAutoScanLoop(0.0f, 1.0f, WavetableOscillator::LoopMode::Loop);
        masterOsc.setAutoScanStartPos(0.0f);
        masterOsc.setAutoScanRateHz(
            motionRateHz > 0.0f
                ? motionRateHz
                : static_cast<float>(sr / static_cast<double>(frameStrip.getNumSamples())));
        masterOsc.setAutoScan(true);

        masterOsc.setMorphTimeMs(paramCache.driftCrossfade->load());
        // Gate the per-block traversal re-sync BEFORE distributing, so no
        // audio block can re-derive neural scan brackets over the DCO table.
        dcoTableActive_.store(true, std::memory_order_relaxed);
        // A HELD note plays the freshly baked table: active wavetable voices
        // equal-power crossfade over the Regen XFade time, silent voices adopt.
        voiceManager.distributeWavetableFrames(masterOsc);
    }

    // Publish the baked strip for the engine-window WT display. Message thread
    // (makeCopyOf allocates — fine here, never on the audio thread), OUTSIDE the
    // callback lock: it is display data, not engine state. The SynthPanel timer
    // reads it via hasNewWtDisplay() and draws the table frame-decimated. NOT the
    // sample path (newWaveformReady) — a bake sets no sample snapshot, so the
    // extraction-region sample view never fires for the LCO.
    wtDisplaySnapshot.makeCopyOf(frameStrip);
    newWtDisplayReady.store(true, std::memory_order_release);
}

void T5ynthProcessor::reloadProcessedAudio(const juce::AudioBuffer<float>& processed)
{
    samplerProcessorDebugLog("reloadProcessedAudio begin samples=" + juce::String(processed.getNumSamples())
                             + " masterBefore={" + masterSampler.debugStateString() + "}");
    SamplePlayer::PrepareConfig samplerConfig;
    bool wavetableMode = false;
    {
        const juce::ScopedLock sl (getCallbackLock());
        syncSamplerSettingsFromParametersLocked();
        samplerConfig = masterSampler.capturePrepareConfig();
        wavetableMode = isWavetableMode();
    }
    auto preparedSamplerLoad = masterSampler.prepareBufferLoad(processed, generatedSampleRate, samplerConfig);
    storeSamplerReprepareSource(preparedSamplerLoad.originalBuffer, generatedSampleRate);
    juce::AudioBuffer<float> preparedGeneratedAudio;
    preparedGeneratedAudio.makeCopyOf(processed);
    juce::AudioBuffer<float> preparedWaveformSnapshot;
    if (processed.getNumChannels() > 0 && processed.getNumSamples() > 0)
    {
        preparedWaveformSnapshot.setSize(1, processed.getNumSamples(), false, false, true);
        preparedWaveformSnapshot.copyFrom(0, 0, processed, 0, 0, processed.getNumSamples());
    }

    // Defaults to nullptr (no extraction below) — applyPreparedMipData() no-ops
    // on nullptr, matching this function's original behaviour of leaving
    // masterOsc's bank untouched when the outer condition below is false.
    WavetableOscillator::MipDataPtr preparedMipData;
    if (preparedWaveformSnapshot.getNumSamples() > 0 && masterOsc.hasFrames())
    {
        const auto wtMapping = makeWtTraversalMapping(preparedWaveformSnapshot.getNumSamples(),
                                                      samplerConfig.startPosFrac,
                                                      samplerConfig.loopStartFrac,
                                                      samplerConfig.loopEndFrac);
        float start = wavetableMode ? wtMapping.extractStart : samplerConfig.loopStartFrac;
        float end   = wavetableMode ? wtMapping.extractEnd   : samplerConfig.loopEndFrac;

        constexpr int frameCounts[] = {32, 64, 128, 256};
        int fcIdx = static_cast<int>(paramCache.wtFrames->load());
        int maxFrames = frameCounts[juce::jlimit(0, 3, fcIdx)];

        // Off-lock compute only, no publish (see WavetableOscillator::
        // prepareFramesFromBuffer's doc comment) — publishedMipData_ publishes
        // through the real atomic free-function API, which is what covers
        // CLAP; kept off-lock here purely so the extraction/FFT work never
        // delays the audio thread's next processBlock on the formats where
        // this lock is in effect. The publish happens inside the lock below
        // via applyPreparedMipData(), before distributeWavetableFrames reads it.
        preparedMipData = wavetableMode
            ? masterOsc.prepareFramesFromBuffer(preparedWaveformSnapshot, generatedSampleRate, start, end, maxFrames)
            : masterOsc.prepareContiguousFrames(preparedWaveformSnapshot, generatedSampleRate, start, end);
    }
    auto preparedFreezeBuffer = makeFreezeLoadBuffer(processed,
                                                     generatedSampleRate,
                                                     samplerConfig.normalizeOn,
                                                     0.0f,
                                                     1.0f,
                                                     masterSampler);
    // Off-lock compute: mixdown only, no publish (see FreezeTextureEngine::
    // prepareBufferLoad's doc comment). The actual publish happens inside the
    // lock below, alongside masterSampler's/masterOsc's. masterFreeze.
    // publishedSnapshot_ publishes through the real atomic free-function API
    // (see its declaration in FreezeTextureEngine.h) — computed off-lock here
    // purely to keep the mixdown from delaying the audio thread's next
    // processBlock on the formats where this lock is in effect.
    auto preparedFreezeSnapshot = masterFreeze.prepareBufferLoad(preparedFreezeBuffer, generatedSampleRate);
    {
        const juce::ScopedLock sl (getCallbackLock());

        // Update stored audio and reload into sampler without Rumble/HF/Normalize
        generatedAudioFull = std::move(preparedGeneratedAudio);
        voiceManager.drainRetiredSamplerSnapshots();
        masterSampler.applyPreparedBufferLoad(std::move(preparedSamplerLoad), samplerConfig);
        masterFreeze.applyPreparedBufferLoad(std::move(preparedFreezeSnapshot));
        masterOsc.applyPreparedMipData(std::move(preparedMipData));
        if (preparedWaveformSnapshot.getNumSamples() > 0)
            waveformSnapshot = std::move(preparedWaveformSnapshot);
        // Held sampler notes crossfade onto the reprocessed sample on the next
        // audio-thread distribute pass (off-thread → allowMorph=false).
        voiceManager.distributeSamplerBuffer(masterSampler, 0.0f, /*allowMorph=*/false);
        if (masterOsc.hasFrames())
        {
            dcoTableActive_.store(false, std::memory_order_relaxed);  // re-extracted from processed audio above
            clearLcoBakeSnapshot();
            // Neural extraction reclaims masterOsc with fresh, definitely-not-
            // stale content.
            syncWavetableTraversal(generatedSampleRate, waveformSnapshot.getNumSamples());
            masterOsc.setMorphTimeMs(paramCache.driftCrossfade->load());
            voiceManager.distributeWavetableFrames(masterOsc);
        }
        // Reprocessed audio (e.g. Rumble/HF/Normalize changed) → held granular
        // voices crossfade-adopt it live, like Wavetable above. Off the audio
        // thread (under getCallbackLock), so morphToBufferFrom is RT-safe here.
        voiceManager.distributeFreezeBuffer(masterFreeze, paramCache.driftCrossfade->load(), true);

        samplerProcessorDebugLog("reloadProcessedAudio end masterAfter={" + masterSampler.debugStateString() + "}");

        if (waveformSnapshot.getNumSamples() > 0)
        {
            newWaveformReady.store(true, std::memory_order_release);
        }
    }

    // Reprocessed audio re-extracted the WT frames above → refresh the fan too.
    if (wavetableMode)
        publishWtDisplayFromOscFrames();
}

int T5ynthProcessor::maxInferenceCacheCapacityForDuration() const
{
    // 192 slot-seconds; the header carries why the budget is slot-seconds and
    // not bytes. Descending, so the first fit is the deepest one that fits.
    constexpr float kSlotSecondBudget = 192.0f;

    // Half a display detent. The Duration slider steps in 0.01s, so this is the
    // widest tolerance that still separates two values the user can tell apart -
    // and it has to be here, because the number in the parameter is NOT the
    // number snapGenerationDuration produced. AudioParameterFloat::setValue
    // applies convertFrom0to1 without snapToLegalValue, and the atomic behind
    // getRawParameterValue is written as convertFrom0to1(convertTo0to1(v)) - two
    // trips through a 0.3 skew, whose 1/0.3 exponent multiplies the float error.
    // A slider reading exactly "96.00s" arrives here as 96.0000076, and a bare
    // <= 192 then denies the 2-slot rung that this Duration is supposed to grant.
    // Measured: without this tolerance all ten detents that display 96.00s give
    // 0, and 12s/24s only pass because this platform's libm happens to round
    // down - correctly-rounded arithmetic drops them a rung too.
    constexpr float kDetentTolerance = 0.005f;

    const float duration = juce::jmax(0.001f, paramCache.genDuration->load());
    for (int depth : { 16, 8, 4, 2 })
        if (duration <= kSlotSecondBudget / static_cast<float>(depth) + kDetentTolerance)
            return depth;
    return 0;
}


int T5ynthProcessor::sanitizeCacheCapacity(int capacity)
{
    // The five depths both rows offer. Anything else is ROUNDED UP to the next
    // one, never down, and only a request for 0 switches a cache off.
    //
    // It used to fall to 0 - the row can only ask for one of the five, so an
    // in-between value only ever arrives from a FILE, where it means "this many
    // entries are in here". Reading that as "off" threw every one of them away
    // and left a preset's recorded run unloadable, which is the opposite of what
    // widening the stored depth to the entry count was for.
    static constexpr int kAllowed[] = { 0, 2, 4, 8, 16 };
    constexpr int maxAllowed = kAllowed[sizeof(kAllowed) / sizeof(kAllowed[0]) - 1];
    if (capacity >= maxAllowed)
        return maxAllowed;
    for (int allowed : kAllowed)
        if (capacity <= allowed)
            return allowed;
    return maxAllowed;
}

void T5ynthProcessor::setInferenceCacheCapacity(int capacity)
{
    // 32 and 64 were dropped: a preset carrying a take that deep runs to hundreds of
    // megabytes, and the two switch cells they held now carry the offline-take mode
    // (and, next, the MPE feature). A preset saved at either clamps to 16 below.
    const int sanitized = sanitizeCacheCapacity(capacity);

    if (sanitized == inferenceCacheCapacity)
        return;

    inferenceCacheCapacity = sanitized;
    clearInferenceCache();   // publishes the zone count for us
}

void T5ynthProcessor::clearInferenceCache()
{
    // Anything still on its way: a position posted into a cache that is being
    // emptied must not install into whatever fills it next.
    atCachePosReq_.store(0, std::memory_order_release);
    inferenceCacheEntries.clear();
    inferenceCachePlaybackIndex = 0;
    inferenceCacheIsOfflineTake = false;   // whatever it held, it is gone with it
    publishInferenceCacheTraversableZones();
}

void T5ynthProcessor::publishInferenceCacheTraversableZones()
{
    // Every mutation of the cache ends here. Only a FULL cache is traversable:
    // while it is filling, the reachable positions would move under the hand.
    inferenceCacheTraversableZones_.store(
        isInferenceCacheFull() ? static_cast<int>(inferenceCacheEntries.size()) : 0,
        std::memory_order_release);
    inferenceCacheGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

bool T5ynthProcessor::addInferenceCacheEntry(const juce::AudioBuffer<float>& buffer, double sampleRate)
{
    if (inferenceCacheCapacity <= 0
        || static_cast<int>(inferenceCacheEntries.size()) >= inferenceCacheCapacity
        || buffer.getNumSamples() <= 0
        || buffer.getNumChannels() <= 0)
        return false;

    InferenceCacheEntry entry;
    entry.audio.makeCopyOf(buffer);
    entry.sampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    inferenceCacheEntries.push_back(std::move(entry));
    if (isInferenceCacheFull())
        inferenceCachePlaybackIndex = 0;
    publishInferenceCacheTraversableZones();
    return true;
}

bool T5ynthProcessor::playInferenceCacheEntry(int index)
{
    if (!isInferenceCacheFull())
        return false;


    const int count = static_cast<int>(inferenceCacheEntries.size());
    index = juce::jlimit(0, count - 1, index);
    const auto& entry = inferenceCacheEntries[static_cast<size_t>(index)];
    // The sequential cursor lands one past whatever was played, by hand or in
    // sequence, so a Re-Prompt step after a hand-driven jump carries on from
    // where the hand left off instead of from where the sweep had got to.
    inferenceCachePlaybackIndex = (index + 1) % count;
    loadGeneratedAudio(entry.audio, entry.sampleRate);
    return true;
}

bool T5ynthProcessor::playNextInferenceCacheEntry()
{
    if (!isInferenceCacheFull())
        return false;
    return playInferenceCacheEntry(inferenceCachePlaybackIndex);
}

namespace
{
/** One aftertouch press, as it travels from the audio thread to the message
 *  thread: the position asked for and the serial of the press that asked, in a
 *  single word so the two cannot be read apart. 0 means the mailbox is empty. */
inline juce::uint64 makeTraversalReq(unsigned seq, int idx)
{
    return (static_cast<juce::uint64>(seq) << 32)
         | static_cast<juce::uint32>(idx + 1);
}
inline unsigned traversalReqSeq(juce::uint64 v) { return static_cast<unsigned>(v >> 32); }
inline int      traversalReqIdx(juce::uint64 v)
{
    return static_cast<int>(static_cast<juce::uint32>(v & 0xffffffffu)) - 1;
}
/** Serial `a` is at or before serial `b`, wrap included. */
inline bool traversalSeqReached(unsigned a, unsigned b)
{
    return static_cast<juce::int32>(a - b) <= 0;
}

/** Which of `zones` equal steps the pressure has reached, with a dead band so a
 *  finger resting on a boundary does not walk back and forth over it. Returns
 *  -1 when there is nothing to travel through.
 *
 *  The band is deliberately wide - three quarters of a step - because a step
 *  here is not a value change but a whole sample being crossfaded in. Getting
 *  one too late costs nothing; getting one twice is audible. */
int traversalZone(float pressure, float amount, int zones, int currentZone)
{
    if (zones <= 0)  return -1;
    if (zones == 1)  return 0;

    // The bar's amount is a DEPTH here as it is everywhere else in this module:
    // it says how far through the cache full pressure carries. Half a bar spans
    // half the entries, and its sign is read by the caller as the direction.
    // MAGNITUDE, because the reading is signed: maxHeldExpression returns the
    // held voice furthest from rest and keeps the sign, since X leans both ways.
    // Clamping instead of rectifying would make a down-bend read as rest — and
    // one voice bent down would cancel another bent up, leaving the whole
    // negative half of the axis inert for these two targets. Rest is still rest:
    // |0| is 0. The sign of the AMOUNT stays the caller's direction, as before.
    const float drive  = juce::jlimit(0.0f, 1.0f, std::abs(pressure)) * std::abs(amount);
    const float scaled = drive * static_cast<float>(zones);

    // EQUAL-WIDTH STEPS, and both ends always land. Rounding to the nearest
    // position instead would make the first and last steps half as wide as the
    // rest - measured on a 16-deep cache, the last entry then lived in the top
    // three aftertouch values alone, and a two-deep cache with the bar at half
    // could not be moved at all: armed, and inert over its whole travel. Each
    // step is now 1/N of the travel, and pressed fully in (or fully released)
    // the traveller is at an end of what this bar spans, where no hysteresis may
    // hold it back.
    //
    // The floor carries the SAME epsilon the test above does. The bar's amount
    // comes off a snapped slider grid, and the snap is a multiply-add that the
    // compiler contracts to an FMA - so the value drawn as 0.50 arrives as
    // 0.49999997. On a two-deep cache that is floor(0.99999994) = 0: the top of
    // the bar's travel resolves to the step it started in, and a bar that
    // engages on travel would then never engage at all. Inert over its whole
    // length, for every amount that lands exactly on 1/zones.
    if (drive >= std::abs(amount) - 1.0e-4f || drive <= 1.0e-4f)
        return juce::jlimit(0, zones - 1, static_cast<int>(std::floor(scaled + 1.0e-4f)));

    // A quarter of a step of overshoot before a boundary is crossed. Wide,
    // because a step here is not a value moving but a whole sample being
    // crossfaded in: taking one late costs nothing, taking one twice is audible.
    if (currentZone >= 0
        && scaled > static_cast<float>(currentZone) - 0.25f
        && scaled < static_cast<float>(currentZone) + 1.25f)
        return juce::jlimit(0, zones - 1, currentZone);   // a shrunken cache must not leak an old zone

    return juce::jlimit(0, zones - 1, static_cast<int>(std::floor(scaled + 1.0e-4f)));
}
} // namespace

void T5ynthProcessor::cancelParkedCachePosition()
{
    // What is PARKED, and only that. A position already in a mailbox was asked
    // for by a hand that had moved, and it is one turn of the message loop from
    // landing - a millisecond or two. Taking it back would make the same gesture
    // load or not load depending on whether that turn happened to come first,
    // and it would bite hardest at the bottom of the travel: on most keyboards
    // the pressure reaches zero a few milliseconds BEFORE the note-off, so the
    // last step of every phrase would be a coin toss.
    //
    // What waits behind a Csound swap is a different matter. That one arrives a
    // second or more later, long after the gesture, and it lives in the drain's
    // own state where dropping it is exact rather than a race.
    atCacheCancelSeq_.store(atPostSeq_, std::memory_order_release);
}

void T5ynthProcessor::cancelParkedSnapSlot()
{
    atSnapCancelSeq_.store(atPostSeq_, std::memory_order_release);   // parked only - see above
}

void T5ynthProcessor::updateAftertouchTraversal(const BlockParams& bp)
{
    const float cacheAmt = bp.aftertouchTargetAmt[AftertouchTarget::Cache];
    const float snapAmt  = bp.aftertouchTargetAmt[AftertouchTarget::Snap];

    // A press this bar made was dropped, or the slot it holds was written over.
    // Either way it no longer holds what it last asked for, and going on
    // believing it does would leave that position unreachable until the finger
    // had visited another one and come back.
    //
    // A full RE-ARM, not just the claim. Letting go of the claim alone would
    // leave the bar engaged, and an engaged bar with nothing claimed posts on
    // the very next block - firing a landing under a motionless finger, for a
    // snapshot the player was storing or a press that was just discarded. The
    // same rule as every other re-arm in this function: it permits the next
    // landing, it does not make one.
    if (atCacheForgetActed_.exchange(false, std::memory_order_acq_rel))
    {
        atCacheZone_ = -1; atCacheActedIdx_ = -1;
        atCacheEngaged_ = false; atCacheBaseZone_ = -1;
    }
    if (atSnapForgetActed_.exchange(false, std::memory_order_acq_rel))
    {
        atSnapZone_ = -1; atSnapActedSlot_ = -1;
        atSnapEngaged_ = false; atSnapBaseZone_ = -1;
    }

    // Each bar re-arms on ITS OWN. Zeroing one while the other stays live has to
    // forget that one's zone, or turning it back up with the finger already
    // resting where it left off would post nothing and the instrument would sit
    // on whatever is loaded instead of going where the hand is.
    // Once, when the bar goes down - not on every block it spends at zero. Most
    // patches leave both of these at zero forever, and a cancel raised on every
    // block would sit permanently true: harmless today, because nothing else
    // fills the two parking slots, and a trap for whoever adds something that
    // does.
    if (cacheAmt == 0.0f && (atCacheZone_ >= 0 || atCacheActedIdx_ >= 0
                             || atCacheEngaged_ || atCacheBaseZone_ >= 0))
    {
        atCacheZone_ = -1; atCacheActedIdx_ = -1;
        atCacheEngaged_ = false; atCacheBaseZone_ = -1;
        cancelParkedCachePosition();
    }
    if (snapAmt == 0.0f && (atSnapZone_ >= 0 || atSnapActedSlot_ >= 0
                            || atSnapEngaged_ || atSnapBaseZone_ >= 0))
    {
        atSnapZone_ = -1; atSnapActedSlot_ = -1;
        atSnapEngaged_ = false; atSnapBaseZone_ = -1;
        cancelParkedSnapSlot();
    }
    if (cacheAmt == 0.0f && snapAmt == 0.0f)
        return;

    // Only while a key is actually HELD. Not "a voice is active": a voice stays
    // active through its whole release tail, and its pressure is not cleared
    // until it goes silent - so letting go of everything would read as a slow
    // slide back to the first position, a move nobody asked for at the one
    // moment nobody is playing.
    //
    // Hands off ends the gesture, and everything it had outstanding with it: a
    // position swept past and parked behind a Csound compile must not arrive a
    // second later over an empty keyboard, and the next note must be pressed
    // into before a bar claims anything again.
    //
    // The ARPEGGIATOR counts as hands on the keyboard. It plays its chord as a
    // run of short internal notes, and between two of them no voice is held at
    // all - so asking the voices alone would read every gap in the pattern as
    // the player letting go, forget where the traveller was, and then take the
    // next step as a fresh gesture: one whole sound loaded per arpeggiator step,
    // which in the language oscillator is one Csound recompile per step.
    //
    // And a PEDAL is not a hand. A voice held by the damper, by sostenuto or as
    // the step sequencer's drone is active and not releasing, so counting those
    // would mean the gesture never ends: with every key lifted the bar stays
    // engaged and keeps travelling on whatever the mod wheel or the breath
    // controller is parked at - one landing per step, nobody at the keyboard,
    // and the drone latches that state until it is cleared.
    const bool anyHeld = voiceManager.getKeyHeldVoiceCount() > 0
                      || arpeggiator.hasHeldKeys();
    if (anyHeld != atAnyKeyHeld_)
    {
        atAnyKeyHeld_ = anyHeld;
        if (! anyHeld)
        {
            // The gesture is over: each bar must be pressed into again before it
            // claims anything, and nothing it had outstanding may still arrive.
            //
            // What does NOT go is where the traveller stood. Forgetting that
            // would make every repeated note re-ask for the position already
            // loaded, and the pressure does not have to fall between two notes
            // for that to fire: the mod wheel and the breath controller feed the
            // same reading and stay where they were parked, and under the
            // arpeggiator the finger holding the chord makes no voice at all. In
            // the language oscillator each of those re-asks is a recompile of
            // the orchestra already sounding, one per note, back to back.
            atCacheZone_ = -1; atCacheEngaged_ = false; atCacheBaseZone_ = -1;
            atSnapZone_  = -1; atSnapEngaged_  = false; atSnapBaseZone_  = -1;
            cancelParkedCachePosition();
            cancelParkedSnapSlot();
        }
    }
    if (! anyHeld)
        return;
    // Each bar reads its OWN axis: these two are targets in the expression
    // matrix like the rest, so which of V/X/Y/Z moves them is the player's
    // choice, not a constant.
    //
    // Same reason as above for the reading itself: in an arpeggiator gap there
    // is no voice to read the pressure off, and taking 0 there would walk the
    // traveller back to the first position on every gap. That repair is Z's
    // alone - an arpeggiated note is internal, carries no MIDI channel and so
    // has no MPE X or Y at all, and there is no per-held-key velocity to read
    // either. A bar on one of those axes is simply not armed during a gap,
    // which is the truth rather than a hole.
    auto axisReading = [this] (int src)
    {
        float value = voiceManager.maxHeldExpression(src);
        if (src == ExprSource::Z)
            for (const auto& k : arpeggiator.getHeldKeys())
                value = juce::jmax(value, voiceManager.pressureForHeldNote(k.note));
        return value;
    };
    const int   snapSrc       = bp.aftertouchTargetSrc[AftertouchTarget::Snap];
    const int   cacheSrc      = bp.aftertouchTargetSrc[AftertouchTarget::Cache];
    const float snapPressure  = axisReading(snapSrc);
    const float cachePressure = axisReading(cacheSrc);

    // Changing which axis a bar rides is not the hand moving, and without this
    // it looks exactly like it: the reading jumps from wherever the old axis was
    // to wherever the new one is, in one block, under a finger that has not
    // stirred. An engaged bar would post a landing on the spot - and on the
    // cache bar in the LRO a landing is a Csound recompile. Same re-arm as the
    // cache's own oscillator change further down, and for the same reason: a
    // re-arm PERMITS the next landing, it does not fire one. Travel one step and
    // the bar is back.
    if (snapSrc != atSnapSrc_)
    {
        atSnapSrc_       = snapSrc;
        atSnapZone_      = -1;
        atSnapActedSlot_ = -1;
        atSnapEngaged_   = false;
        atSnapBaseZone_  = -1;
    }
    if (cacheSrc != atCacheSrc_)
    {
        atCacheSrc_       = cacheSrc;
        atCacheZone_      = -1;
        atCacheActedIdx_  = -1;
        atCacheEngaged_   = false;
        atCacheBaseZone_  = -1;
    }

    // REST IS NOT A DESTINATION - see where each bar engages, below.

    if (snapAmt != 0.0f)
    {
        constexpr int kSnapSlots = 4;
        const int zone = traversalZone(snapPressure, snapAmt, kSnapSlots, atSnapZone_);
        if (zone >= 0)
        {
            atSnapZone_ = zone;
            if (! atSnapEngaged_)        // the hand has to move - see the cache bar below
            {
                if (atSnapBaseZone_ < 0)          atSnapBaseZone_ = zone;
                else if (zone != atSnapBaseZone_) atSnapEngaged_ = true;
            }
            if (atSnapEngaged_)
            {
                const int slot = (snapAmt > 0.0f ? zone
                                                 : (kSnapSlots - 1 - zone)) + 1;  // slots are 1-4
                // Same question as the cache bar below, for the same reason.
                if (slot != atSnapActedSlot_)
                {
                    atSnapActedSlot_ = slot;
                    atSnapReq_.store(makeTraversalReq(++atPostSeq_, slot),
                                     std::memory_order_release);
                    triggerAsyncUpdate();
                }
            }
        }
    }

    if (cacheAmt != 0.0f)
    {
        // Whichever oscillator is sounding owns the cache this bar travels.
        //
        // Asked of the PARAMETER, not of bp: this runs early in processBlock,
        // before bp.engineMode is filled in, so reading it from there would have
        // handed back the struct's default on every single block - the bar would
        // have sized itself against the neural cache forever, and in the LRO,
        // where that cache is usually empty, it would never have moved at all.
        // The atomic is lock-free and costs the same.
        const bool lro = isLanguageOscillatorSounding();
        // The claim is one number and the two caches take turns owning it, so
        // the oscillator changing under a held note has to release it: index 7
        // of the LRO's cache says nothing about index 7 of the neural one, and
        // leaving it standing would make the first travel to that position in
        // the new cache do nothing at all.
        if (lro != atCacheLro_)
        {
            atCacheLro_ = lro;
            atCacheZone_ = -1;
            atCacheActedIdx_ = -1;
            // Engagement goes too. A re-arm PERMITS the next landing, it does
            // not fire one: nothing here was the hand moving. Leaving the bar
            // engaged would post under a motionless finger the moment something
            // else changed the cache - and something else did, or we would not
            // be re-arming. The player travels one step and the bar is back.
            atCacheEngaged_ = false;
            atCacheBaseZone_ = -1;
            // The MAILBOXES go too, which the cancel deliberately leaves alone
            // everywhere else. Elsewhere a posted position is a millisecond from
            // landing and taking it back would be a race; here it is simply
            // wrong - it was resolved against the cache being left, and an LRO
            // landing forces the engine back to Csound on its way in. The flip
            // cannot post in this same block: it has just cleared engagement, so
            // the most this block does below is take a bearing.
            //
            // Emptying them is not enough on its own: a drain can already have
            // taken the press out of the mailbox before this block runs, and no
            // store order reaches backwards into a load that has happened. So
            // the serial is what actually discards it - anything posted up to
            // now is cancelled HARD, and the drain refuses it wherever it has
            // got to.
            atCachePosReq_.store(0, std::memory_order_release);
            atLroCachePosReq_.store(0, std::memory_order_release);
            atCacheHardCancelSeq_.store(atPostSeq_, std::memory_order_release);
            cancelParkedCachePosition();
        }
        // The size and the generation are two atomics, written in the opposite
        // order to the one they are read in. Release/acquire rules out the pair
        // that would be dangerous - a new generation over the old depth - and
        // leaves the harmless one, a fresh depth still carrying the generation
        // before the bump. Harmless only because of this sandwich: read the
        // generation on both sides and sit the block out if it moved, so the
        // bar can never record that generation as SEEN while it was steering by
        // a depth that did not belong to it. That mistake would not correct
        // itself; sitting out costs one block, 1-10 ms.
        // Do not "fix" this by swapping the two stores in the publishers - that
        // turns the self-correcting case into the permanent one.
        const unsigned genBefore = lro ? getCsoundCacheGeneration()
                                       : getInferenceCacheGeneration();
        const int zones = lro ? getCsoundCacheTraversableZones()
                              : getInferenceCacheTraversableZones();
        const unsigned gen = lro ? getCsoundCacheGeneration()
                                 : getInferenceCacheGeneration();
        if (gen != genBefore)
            return;   // the Snap bar above has already had its block
        // A cache that has been touched at all is a different cache, and the
        // position this bar last asked for says nothing about the new one - so
        // the bar re-arms and a finger already somewhere in it can travel there
        // again. A COUNTER, not the size: the size is only ever sampled on
        // blocks where this bar is armed and a key is down, so every value it
        // passes through in between is invisible. A preset load empties the
        // cache and refills it to the same depth inside one message-thread call;
        // watching the size, the bar would see 4 before and 4 after and go on
        // believing the position under the finger is loaded. A counter cannot be
        // stepped over.
        if (! atCacheGenValid_ || gen != atCacheGenSeen_)
        {
            atCacheGenValid_ = true;
            atCacheGenSeen_  = gen;
            atCacheZone_     = -1;
            atCacheActedIdx_ = -1;
            atCacheEngaged_  = false;   // permits, does not fire - see below
            atCacheBaseZone_ = -1;
        }
        const int zone = traversalZone(cachePressure, cacheAmt, zones, atCacheZone_);
        if (zone >= 0)
        {
            atCacheZone_ = zone;
            // THE HAND HAS TO MOVE. Where a bar stands when it is re-armed is
            // its starting point, not a destination it was steered to, and until
            // the hand carries it off that step it claims nothing.
            //
            // Without this the first note of the session crossfades away the
            // sound the preset opened with - a key taken and not pressed into
            // sits at the first step, and in the language oscillator claiming it
            // is a recompile over a second long. And a re-arm would FIRE a
            // landing rather than permit one: the cache changing under a
            // motionless finger would install the position that finger happens
            // to rest on, for a movement nobody made.
            //
            // A threshold on the pressure cannot do this. One step is 1/N of the
            // bar's travel, so a level that reads as "pressed in" on a two-deep
            // cache is a quarter of the way through a sixteen-deep one, and a
            // keyboard whose aftertouch idles a few counts above zero engages on
            // contact. The step boundary is the only measure that scales with
            // both the bar's depth and the cache's - and it is the one the zone
            // decision above already computes, hysteresis included.
            //
            // Once engaged the bar follows the finger for the rest of the
            // phrase, rest included: by then rest is somewhere it was steered
            // back to.
            if (! atCacheEngaged_)
            {
                if (atCacheBaseZone_ < 0)          atCacheBaseZone_ = zone;
                else if (zone != atCacheBaseZone_) atCacheEngaged_ = true;
            }
            if (atCacheEngaged_)
            {
                // The sign of the bar is the direction of travel through the
                // cache: right for the order the samples were generated in, left
                // for the way back. That is the one thing this feature adds to
                // the stepping the Re-Prompt path already does.
                const int idx = cacheAmt > 0.0f ? zone : (zones - 1 - zone);
                // Asked for once per position the HAND reaches. Not "once per
                // position that is not currently loaded": both bars hang off the
                // same pressure, and every landing is a load, so a bar that
                // chases what is loaded is re-armed by the other bar's landing
                // under a finger that never moved - and each re-post lands again.
                if (idx != atCacheActedIdx_)
                {
                    atCacheActedIdx_ = idx;
                    // Into the mailbox of the cache this index was RESOLVED
                    // against. One shared mailbox would leave the message thread
                    // to guess, and it would guess with whatever the mode is by
                    // the time it drains - which a snapshot recall in the same
                    // drain pass can already have moved. An index means nothing
                    // without its cache.
                    if (lro)
                        atLroCachePosReq_.store(makeTraversalReq(++atPostSeq_, idx),
                                                std::memory_order_release);
                    else
                        atCachePosReq_.store(makeTraversalReq(++atPostSeq_, idx),
                                             std::memory_order_release);
                    triggerAsyncUpdate();
                }
            }
        }
    }

}

// ── The LRO's cache ─────────────────────────────────────────────────────────
void T5ynthProcessor::setCsoundCacheCapacity(int capacity)
{
    // The same five depths the neural row offers, and the same sanitizer.
    const int sanitized = sanitizeCacheCapacity(capacity);

    if (sanitized == csoundCacheCapacity)
        return;

    csoundCacheCapacity = sanitized;
    clearCsoundCache();
}

void T5ynthProcessor::clearCsoundCache()
{
    // Anything still on its way: a position posted into a cache that is being
    // emptied must not install into whatever fills it next.
    atLroCachePosReq_.store(0, std::memory_order_release);
    pendingLroCachePos_ = -1;
    csoundCacheEntries.clear();
    csoundCachePlaybackIndex = 0;
    csoundCacheIsOfflineTake = false;
    publishCsoundCacheTraversableZones();
}

void T5ynthProcessor::publishCsoundCacheTraversableZones()
{
    csoundCacheTraversableZones_.store(
        isCsoundCacheFull() ? static_cast<int>(csoundCacheEntries.size()) : 0,
        std::memory_order_release);
    csoundCacheGeneration_.fetch_add(1, std::memory_order_acq_rel);
}

bool T5ynthProcessor::addCsoundCacheEntry(const CsoundCacheEntry& entry)
{
    if (csoundCacheCapacity <= 0
        || static_cast<int>(csoundCacheEntries.size()) >= csoundCacheCapacity
        || entry.orchestra.isEmpty())
        return false;

    csoundCacheEntries.push_back(entry);
    if (isCsoundCacheFull())
        csoundCachePlaybackIndex = 0;
    publishCsoundCacheTraversableZones();
    return true;
}

bool T5ynthProcessor::playCsoundCacheEntry(int index)
{
    if (!isCsoundCacheFull())
        return false;

    const int count = static_cast<int>(csoundCacheEntries.size());
    index = juce::jlimit(0, count - 1, index);
    const auto& e = csoundCacheEntries[static_cast<size_t>(index)];
    csoundCachePlaybackIndex = (index + 1) % count;

    // What an authoring pass installs on the processor, in its order. Nothing is
    // left out: a slot that installed less than the pass did would sound like the
    // authored instrument with somebody else's knobs on it.
    //
    // The engine mode FIRST. A replay is a recall, and MainPanel's SNAP recall
    // forces it for the same reason: an LRO cache can be full while the engine
    // still sounds the neural oscillator (a preset carries the cache whatever
    // mode it was saved in, and the oscillator toggle deliberately leaves the
    // engine alone until something has been authored). Without it the whole
    // instrument installs and NOTHING IS HEARD - and since the replay gate then
    // fires on every further GENERATE, the LRO could never author its way out.
    //
    // ...but only while the LRO is still the paradigm in front, which is the
    // authoring pass's own guard (`if (! easyMode_)`) and the SNAP recall's
    // (activateSnapshot returns into its neural branch first). An authoring pass
    // runs for minutes and its tail can land long after the player has moved on
    // to T5osc; forcing there would yank the engine into Csound under a neural
    // panel AND let setAuthorSettings below write the author's filter, envelopes
    // and FX over the patch the player is holding.
    if (isSurfaceParadigmLanguage())
        forceCsoundEngineMode();
    requestCsoundOrchestra(e.orchestra);
    setCsoundPrompt(e.prompt);
    setCsoundReading(e.reading);
    setCsoundParamsText(e.paramsText);
    setCsoundControls(LroControls::fromVar(e.controls), /*applyValues=*/true);
    setAuthorSettings(e.settings);
    return true;
}

bool T5ynthProcessor::playNextCsoundCacheEntry()
{
    if (!isCsoundCacheFull())
        return false;
    return playCsoundCacheEntry(csoundCachePlaybackIndex);
}

// ── Whichever cache is in force ─────────────────────────────────────────────
bool T5ynthProcessor::isLanguageOscillatorSounding() const
{
    // Csound and nothing else. EngineMode::Lco is the retired wavetable-bake
    // paradigm, not a Csound path (BlockParams.h) - a cached orchestra does not
    // sound there, so counting it as "the language oscillator" would let the
    // aftertouch bar install orchestras into an engine playing a wavetable.
    return static_cast<int>(paramCache.engineMode->load()) == EngineMode::Csound;
}

void T5ynthProcessor::setSurfaceParadigmIsLanguage(bool isLanguage)
{
    surfaceParadigmIsLanguage_.store(isLanguage, std::memory_order_release);
}

int  T5ynthProcessor::getActiveCacheCapacity() const
{ return isSurfaceParadigmLanguage() ? getCsoundCacheCapacity() : getInferenceCacheCapacity(); }

void T5ynthProcessor::setActiveCacheCapacity(int capacity)
{
    if (isSurfaceParadigmLanguage()) setCsoundCacheCapacity(capacity);
    else                             setInferenceCacheCapacity(capacity);
}

void T5ynthProcessor::selectActiveCacheCapacity(int capacity)
{
    // The ceiling is the neural side's alone; maxActiveCacheCapacityForDuration
    // returns the full depth in the LRO, so this one call covers both.
    const int ceiling = maxActiveCacheCapacityForDuration();

    // A positive request is never clamped down to OFF. Above 96 s the ceiling is
    // 0, and clamping there turned every press on a dimmed depth cell into
    // "switch the cache off" - which CLEARS it. A player holding a finished
    // 16-deep take recorded at 10 s, who then moves the Duration, loses the take
    // to a press that asked for a smaller depth. Nothing is available at that
    // Duration, so the press does nothing; only an explicit 0 switches off.
    if (capacity > 0 && ceiling <= 0)
        return;

    setActiveCacheCapacity(juce::jmin(capacity, ceiling));
}

int  T5ynthProcessor::getActiveCacheFillCount() const
{ return isSurfaceParadigmLanguage() ? getCsoundCacheFillCount() : getInferenceCacheFillCount(); }

bool T5ynthProcessor::isActiveCacheFull() const
{ return isSurfaceParadigmLanguage() ? isCsoundCacheFull() : isInferenceCacheFull(); }


int T5ynthProcessor::maxActiveCacheCapacityForDuration() const
{
    // 16, always, in the LRO: the budget the ceiling enforces is slot-SECONDS of
    // audio, and an LRO slot carries none.
    return isSurfaceParadigmLanguage() ? 16 : maxInferenceCacheCapacityForDuration();
}

void T5ynthProcessor::reextractWavetable()
{
    // A DCO/LCO table owns the oscillator with bit-exact frames (setExactFrames).
    // Frame-count buttons and bracket drags must NOT re-slice the last neural
    // snapshot over it — that would revert the authored table to neural material.
    // The 256-frame standard stays pinned for a baked table (SynthPanel also greys
    // the frame-count buttons while the DCO lock is active).
    if (dcoTableActive_.load(std::memory_order_relaxed))
        return;

    const bool wtMode = isWavetableMode();
    {
        const juce::ScopedLock sl (getCallbackLock());

        if (waveformSnapshot.getNumSamples() > 0)
        {
            const auto wtMapping = makeWtTraversalMapping(waveformSnapshot.getNumSamples());
            float start = isWavetableMode() ? wtMapping.extractStart
                                            : masterSampler.getLoopStart();
            float end   = isWavetableMode() ? wtMapping.extractEnd
                                            : masterSampler.getLoopEnd();

            constexpr int frameCounts[] = {32, 64, 128, 256};
            int fcIdx = static_cast<int>(paramCache.wtFrames->load());
            int maxFrames = frameCounts[juce::jlimit(0, 3, fcIdx)];

            if (isWavetableMode())
                masterOsc.extractFramesFromBuffer(waveformSnapshot, generatedSampleRate, start, end, maxFrames);
            else
                masterOsc.extractContiguousFrames(waveformSnapshot, generatedSampleRate, start, end);

            dcoTableActive_.store(false, std::memory_order_relaxed);  // re-extracted from the snapshot above
            clearLcoBakeSnapshot();
            // Same neural-reclaims-masterOsc transition as reloadProcessedAudio /
            // loadGeneratedAudio: fresh content on A.
            syncWavetableTraversal(generatedSampleRate, waveformSnapshot.getNumSamples());
            masterOsc.setMorphTimeMs(paramCache.driftCrossfade->load());
            voiceManager.distributeWavetableFrames(masterOsc);
        }
    }

    // Re-slicing changed the neural table → refresh the engine-window 2.5D fan.
    // The strip build allocates, so it stays OUTSIDE the callback lock (mirrors
    // the DCO publish in loadDcoWavetable). No-op in sampler mode / with no frames.
    if (wtMode)
        publishWtDisplayFromOscFrames();
}

void T5ynthProcessor::publishWtDisplayFromOscFrames()
{
    // Snapshot the current level-0 frames (numFrames * FRAME_SIZE, frame-major)
    // and hand them to the engine-window fan exactly like the DCO baked strip.
    // snapshotLevel0Frames reads the atomically-published bank (safe off the lock)
    // and returns false for an inharmonic additive bank (no frames) — a no-op.
    std::vector<float> flat;
    int frameSize = 0, numFrames = 0;
    if (! masterOsc.snapshotLevel0Frames(flat, frameSize, numFrames)
        || frameSize <= 0 || numFrames <= 0)
        return;

    wtDisplaySnapshot.setSize(1, numFrames * frameSize, false, false, true);
    std::copy(flat.begin(), flat.end(), wtDisplaySnapshot.getWritePointer(0));
    newWtDisplayReady.store(true, std::memory_order_release);
}

juce::AudioProcessorEditor* T5ynthProcessor::createEditor()
{
    return new T5ynthEditor(*this);
}

void T5ynthProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    // Mid-replay, the live APVTS holds the TAPE's patch, not the user's — a host
    // saving its project (or an auto-save) would silently persist the tape over
    // unsaved work. Hand back the patch we stashed at startReplay() instead. Safe
    // against recursion: startReplay() takes its snapshot before replayModeActive_
    // is set, so this branch cannot fire while filling preReplayPatch_ itself.
    if (isReplayActive() && preReplayPatch_.getSize() > 0)
    {
        destData = preReplayPatch_;
        return;
    }

    auto state = parameters.copyState();
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    xml->setAttribute("midiOutputDeviceId", midiOutputDeviceId_);
    xml->setAttribute("midiClockEnabled", midiClockEnabled_.load());
    xml->setAttribute("modalityEpoch", currentModalityEpoch_);
    xml->setAttribute("calibEpoch", Calibration::kEpoch);
    // Csound orchestra (mirrors exportJsonPreset's engine block): PID::engineMode
    // is an APVTS param, so a session saved in Csound mode would otherwise reload
    // with the wrong (built-in, or a reused live instance's previous-project)
    // orchestra text. Read pending, not the active engine's, for the same reason
    // exportJsonPreset does (see its fuller comment on the csound_orchestra
    // property) -- absence (non-Csound save) keeps the old-session XML shape.
    if (static_cast<int>(paramCache.engineMode->load()) == static_cast<int>(EngineMode::Csound))
    {
        juce::String pendingOrchestraText;
        {
            std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
            pendingOrchestraText = csoundPendingOrchestraText_;
        }
        xml->setAttribute("csoundOrchestra", pendingOrchestraText);
        xml->setAttribute("csoundPrompt", csoundPrompt_);
        xml->setAttribute("csoundReading", csoundReading_);
        xml->setAttribute("csoundParamsText", csoundParamsText_);
        // The knob NAMES. Their VALUES are already saved — they are twelve
        // ordinary parameters — but without the names the panel would come back
        // with twelve unlabelled sliders on a sound whose author had said
        // exactly what each of them does.
        xml->setAttribute("csoundControls", juce::JSON::toString(csoundControls_.toVar(), true));
    }
    copyXmlToBinary(*xml, destData);
}

void T5ynthProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    // A loaded state defines its own engine mode; a stale pre-bake stash must
    // not "restore" over it when the state's audio loads below. Same for the
    // language mode a toggle left BEFORE this state arrived: restoring it would
    // send the mode toggle back to a paradigm this state knows nothing about
    // (e.g. legacy Lco over a state that came back in Csound).
    dcoPrevEngineMode_ = -1;
    lcoEngineMode_ = -1;

    // Consume the caller's marker name HERE, not inside the guard block below: a
    // blob that fails the xml/tag check never reaches the guard, and a name left
    // behind would mislabel the next, unrelated DAW-session restore.
    const juce::String markerName = stateRestoreMarkerName_;
    stateRestoreMarkerName_.clear();

    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
    if (xml != nullptr && xml->hasTagName(parameters.state.getType()))
    {
        auto loadedTree = juce::ValueTree::fromXml(*xml);

        // BPM-sync clock params (v1.7.0-beta.1), patched into the tree so a
        // session saved before they existed restores at their defaults rather
        // than at whatever the host last touched.
        //
        // MEASURED against this build's JUCE (8.0.6), 2026-07-29, because the
        // comment that stood here said the opposite and was believed for two
        // years: `APVTS::replaceState` does NOT leave a parameter that has no
        // node in the tree untouched. `updateParameterConnectionsToChildTrees`
        // appends a node carrying only the id, and `setNewState` then reads
        // `value` off it with `getDenormalisedDefaultValue()` as the fallback —
        // so an absent parameter is RESET to its layout default. (That is the
        // opposite of the .t5p reader in importJsonPreset, which writes only
        // the fields the file carries and therefore really does let an absent
        // one inherit. The two loaders do not behave alike; do not reason from
        // one to the other.)
        //
        // What that leaves this block doing: the same values JUCE would apply
        // anyway, one step earlier, so the swap is atomic and no parameter
        // takes a visible detour through a pre-reset. Kept for that, and
        // because an explicit table is what the next migration will edit.
        struct ClockDefault { const char* pid; float defaultValue; };
        const ClockDefault clockDefaults[] = {
            { PID::lfo1ClockMode,      ClockMode::Off          },
            { PID::lfo1ClockDivision,  ClockDivision::D1_4     },
            { PID::lfo2ClockMode,      ClockMode::Off          },
            { PID::lfo2ClockDivision,  ClockDivision::D1_4     },
            { PID::lfo3ClockMode,      ClockMode::Off          },
            { PID::lfo3ClockDivision,  ClockDivision::D1_4     },
            { PID::drift1ClockMode,    ClockMode::Off          },
            { PID::drift1ClockDivision,DriftDivision::D2_1     },
            { PID::drift2ClockMode,    ClockMode::Off          },
            { PID::drift2ClockDivision,DriftDivision::D2_1     },
            { PID::drift3ClockMode,    ClockMode::Off          },
            { PID::drift3ClockDivision,DriftDivision::D2_1     },
            { PID::delayClockMode,     ClockMode::Off          },
            { PID::delayClockDivision, ClockDivision::D1_4     },
        };
        auto hasParam = [&](const juce::String& pid) {
            for (int i = 0; i < loadedTree.getNumChildren(); ++i)
                if (loadedTree.getChild(i).getProperty("id").toString() == pid)
                    return true;
            return false;
        };
        for (const auto& cd : clockDefaults)
        {
            if (hasParam(cd.pid)) continue;
            juce::ValueTree node("PARAM");
            node.setProperty("id", cd.pid, nullptr);
            node.setProperty("value", cd.defaultValue, nullptr);
            loadedTree.appendChild(node, nullptr);
        }

        // Same reasoning for ENV 4/5 and their aftertouch sustains (2026-07-29):
        // a session saved before they existed carries no node for them, so
        // without this they inherit whatever the last patch left. Defaults come
        // from the layout, not from a second copy of the numbers.
        auto patchToLayoutDefault = [&](const char* pid) {
            if (hasParam(pid)) return;
            auto* prm = parameters.getParameter(pid);
            if (prm == nullptr) return;
            juce::ValueTree node("PARAM");
            node.setProperty("id", pid, nullptr);
            node.setProperty("value", prm->convertFrom0to1(prm->getDefaultValue()), nullptr);
            loadedTree.appendChild(node, nullptr);
        };
        for (int e = 2; e < kNumModEnvs; ++e)          // mod3/mod4 = ENV 4/5
            for (const char* id : PID::modEnv[e].all())
                patchToLayoutDefault(id);
        // lcoSetsParams is here for completeness, not for effect: per the
        // measurement above, replaceState resets it to `false` whether this
        // line runs or not. A DAW project that predates the switch therefore
        // restores with KNOBS off, and there is no way to carry a permission
        // across a session restore short of writing it back AFTER replaceState.
        // Off is the right answer for a freshly instantiated plugin anyway —
        // nobody has granted anything in it yet. This is NOT the case that
        // broke: that one is the .t5p reader, where an absent field really did
        // fall through to a default and really did revoke (see importJsonPreset).
        patchToLayoutDefault(PID::lcoSetsParams);
        patchToLayoutDefault(PID::aftertouchAmtEnv4Sustain);
        patchToLayoutDefault(PID::aftertouchAmtEnv5Sustain);

        // Expression sources (2026-08-23). The layout default is NOT the right
        // answer here, and this is the one place in this block where that is
        // true. defaultFor leaves thirteen rows on None, so a session written
        // before sources existed would come back with its aftertouch amounts
        // intact and nothing driving them - the bars still orange, the pressure
        // dead - and with Cutoff and Scan quietly moved onto CC 74.
        //
        // The test is the GROUP, not the id: a session that carries even one of
        // these keys was written by a build that had them, and every value in it
        // is deliberate, including a None. A session that carries none of them
        // predates the whole idea, and every amount in it was written to mean
        // pressure. Same rule as importJsonPreset, which asks the same question
        // of the file's exprSource block.
        {
            bool sessionKnowsSources = false;
            for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
                sessionKnowsSources = sessionKnowsSources || hasParam(kExprSrcPid[t]);

            for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
            {
                if (sessionKnowsSources)
                {
                    patchToLayoutDefault(kExprSrcPid[t]);
                    continue;
                }
                juce::ValueTree node("PARAM");
                node.setProperty("id", kExprSrcPid[t], nullptr);
                node.setProperty("value", (float) ExprSource::kLegacy, nullptr);
                loadedTree.appendChild(node, nullptr);
            }
        }

        // Per-stage velocity sensitivity (continuous signed, A/D/R TIME only)
        // replaced the old global velSens + discrete A/D/R vel modes. Convert a
        // pre-redesign session's params straight into the loaded tree so the
        // velocity response survives the reload (same mapping as the .t5p preset
        // migration):  A/D/R velSens = sign(oldMode) * oldVelSens. The old
        // velocity→peak (loudness) is intentionally dropped — peak is Amt's job.
        // Legacy IDs are gone from PID:: by design — match them as raw strings.
        {
            auto paramVal = [&](const juce::String& pid, float fallback) -> float {
                for (int i = 0; i < loadedTree.getNumChildren(); ++i)
                    if (loadedTree.getChild(i).getProperty("id").toString() == pid)
                        return static_cast<float>(loadedTree.getChild(i).getProperty("value"));
                return fallback;
            };
            auto appendParam = [&](const char* pid, float value) {
                juce::ValueTree node("PARAM");
                node.setProperty("id", pid, nullptr);
                node.setProperty("value", value, nullptr);
                loadedTree.appendChild(node, nullptr);
            };
            auto modeSign = [](float idx) -> float {
                const int m = juce::roundToInt(idx);
                return m == EnvVelTimeMode::Positive ?  1.0f
                     : m == EnvVelTimeMode::Negative ? -1.0f : 0.0f;
            };
            struct VsMig {
                const char* oldVelSens;
                const char* oldAtkMode; const char* oldDecMode; const char* oldRelMode;
                const char* newAtk; const char* newDec; const char* newRel;
            };
            const VsMig vsMig[] = {
                { "amp_vel_sens",  "amp_attack_vel_mode",  "amp_decay_vel_mode",  "amp_release_vel_mode",
                  PID::ampAttackVelSens,  PID::ampDecayVelSens,  PID::ampReleaseVelSens },
                { "mod1_vel_sens", "mod1_attack_vel_mode", "mod1_decay_vel_mode", "mod1_release_vel_mode",
                  PID::mod1AttackVelSens, PID::mod1DecayVelSens, PID::mod1ReleaseVelSens },
                { "mod2_vel_sens", "mod2_attack_vel_mode", "mod2_decay_vel_mode", "mod2_release_vel_mode",
                  PID::mod2AttackVelSens, PID::mod2DecayVelSens, PID::mod2ReleaseVelSens },
            };
            for (const auto& m : vsMig)
            {
                // Only when genuinely pre-redesign: old global velSens present AND
                // the new per-stage params absent (don't clobber a new session).
                // (The old global velSens→loudness is intentionally dropped now —
                // peak is Amt's job; velocity only maps to the A/D/R times.)
                // newAtk is a valid "already-redesigned" discriminator because the
                // per-stage A/D/(S)/R velSens params have always been written as a
                // set since they were introduced together (f6e69410) — there is no
                // attack-less-but-sustain-bearing format to misclassify.
                if (!hasParam(m.oldVelSens) || hasParam(m.newAtk))
                    continue;
                const float vs = paramVal(m.oldVelSens, 1.0f);
                appendParam(m.newAtk, modeSign(paramVal(m.oldAtkMode, 0.0f)) * vs);
                appendParam(m.newDec, modeSign(paramVal(m.oldDecMode, 0.0f)) * vs);
                appendParam(m.newRel, modeSign(paramVal(m.oldRelMode, 0.0f)) * vs);
            }

            // Aftertouch: the single-select Choice (aftertouch_target) + global
            // aftertouch_amount were replaced by one bipolar amount per target.
            // Fold a pre-redesign session's routing onto the selected target's
            // per-target param. Legacy IDs are gone from PID:: — raw strings.
            if (hasParam("aftertouch_target"))
            {
                const int t = juce::roundToInt(paramVal("aftertouch_target", 0.0f));
                if (t >= AftertouchTarget::LFO1Depth && t <= AftertouchTarget::NoiseLevel
                    && ! hasParam(kAftertouchAmtPid[t]))
                    appendParam(kAftertouchAmtPid[t], paramVal("aftertouch_amount", 0.0f));
            }
        }

        // Calibration migration: rescale values authored under older DSP full-scales
        // so the session sounds identical under the new ones. Done on the loaded tree
        // BEFORE replaceState so only params actually present in the file are touched
        // (never a stale live value). ABSENT calibEpoch = epoch 0. The legacy AT fold
        // above already appended its cutoff node, so it gets rescaled too.
        Calibration::migrateValueTree(loadedTree, xml->getIntAttribute("calibEpoch", 0));

        // Suppresses the per-param ParamEvent flood replaceState() otherwise
        // generates (one setValueNotifyingHost per changed param) — a DAW
        // reopening a saved session is a bulk load exactly like a preset import.
        BulkParamLoadGuard eventLogGuard(*this);
        // A restored session is a different patch, so the last authoring's borrowed
    // knobs go back BEFORE the tree lands — free, since replaceState is about to
    // write every shelf parameter regardless: the ones the tree carries from the
    // tree, and the ones it does not from their layout defaults (measured above).
    // Sited here rather than at the top of the function on purpose: a state blob
    // that fails the tag check leaves the patch untouched, and forgetting the
    // record there would strand the authored knobs with nothing able to return
    // them. FORGET, not just release: the restored session is a different patch,
    // so flipping the switch on it must not fetch the previous sound's knobs.
    forgetAuthorSettings();
    parameters.replaceState(loadedTree);
        // Unnamed for a DAW-session restore; the replay transport names its own
        // (replay_start / replay_end) via stateRestoreMarkerName_.
        eventLogGuard.commit(markerName);

        // Modality epoch (v2.5.0+) — restored only on a matching-tag state load, so a
        // foreign/empty blob (guard false) leaves the live epoch untouched rather than
        // clobbering it to legacy. ABSENT attribute = pre-2.5.0 session -> legacy routing
        // (an old project keeps the Music/SFX-only behaviour it was built with).
        currentModalityEpoch_ = xml->getIntAttribute("modalityEpoch", kLegacyModalityEpoch);

        // Csound orchestra (mirrors importJsonPreset's engine-block handling —
        // see requestCsoundOrchestra()'s own comment there for why queuing here,
        // before handleAsyncUpdate has ever run for this load, is race-free).
        // Absence of the attribute means the session was saved in a different
        // engine mode (see getStateInformation); if the RESTORED mode is
        // nonetheless Csound, a reused live instance would otherwise keep
        // whichever orchestra the PREVIOUS project last compiled -- purge it
        // back to the built-in so the state defines its own sound.
        if (xml->hasAttribute("csoundOrchestra"))
        {
            requestCsoundOrchestra(xml->getStringAttribute("csoundOrchestra"));
            setCsoundPrompt(xml->getStringAttribute("csoundPrompt"));
            setCsoundReading(xml->getStringAttribute("csoundReading"));
            setCsoundParamsText(xml->getStringAttribute("csoundParamsText"));
            // Names only — applyValues stays FALSE. The twelve values are
            // parameters and the state has already restored them; writing the
            // author's starting positions over them here would throw away every
            // knob the player had moved before saving.
            setCsoundControls(LroControls::fromVar(
                juce::JSON::parse(xml->getStringAttribute("csoundControls"))),
                /*applyValues=*/false);
        }
        else if (static_cast<int>(paramCache.engineMode->load()) == static_cast<int>(EngineMode::Csound))
        {
            requestCsoundOrchestra(juce::String());
            setCsoundReading(juce::String()); // pairs with the built-in orchestra above, not a stale reading
            setCsoundParamsText(juce::String());
            setCsoundControls({}, /*applyValues=*/false);   // ...and no knobs from the sound before it
            setCsoundPrompt(juce::String());  // ...and so does the prompt: a purge that
                                              // leaves it standing lets the next save write
                                              // the previous project's prompt beside an
                                              // empty orchestra, and the reload then
                                              // captions the built-in one with it
        }
    }

    // Never auto-start sequencers on session restore — no acoustic surprises
    parameters.getParameter(PID::seqRunning)->setValueNotifyingHost(0.0f);
    parameters.getParameter(PID::genSeqRunning)->setValueNotifyingHost(0.0f);

    // Same anti-surprise rule for the Re-Prompt loop, but ONLY on this DAW host-state
    // path: a non-Off stance with a non-Manual cadence is a self-running generation
    // driver (Manual no longer auto-runs it — pollDriftRegen early-outs there), the
    // moral equivalent of seqRunning=1. A host silently re-opening a project must not
    // spontaneously render, so force the stance Off here. (A preset LOAD, by contrast,
    // is a deliberate user gesture and DOES restore the stance — see importJsonPreset.)
    // The coupling is a passive mode and is left untouched.
    parameters.getParameter(PID::repromptStance)->setValueNotifyingHost(0.0f);
    // The LCO stance is now the same kind of driver and gets the same treatment:
    // since pollLcoRepromptCadence exists, a restored dcoRepromptStance + a non-Manual
    // REGENERATE re-authors the orchestra unattended on project open, overwriting the
    // instrument the project was saved with. (It was safe to leave standing only while
    // the LCO stance was read exclusively by user-initiated presses.)
    parameters.getParameter(PID::dcoRepromptStance)->setValueNotifyingHost(0.0f);

    // Treat the restored seqPreset as not-yet-applied so the next processBlock
    // reloads its canned pattern (the step pattern isn't part of the saved
    // state). seqStateRestored tells that apply to SUPPRESS the step-count push
    // this one time — the restored seqSteps is its own saved param and must
    // survive, even though the reload makes seqPreset look freshly changed.
    // (Without the flag the apply can't tell a restore from a fresh launch, and
    // a 32-step preset would clobber a hand-set count on every project load.)
    // Resetting lastSeqPreset to -1 (rather than leaving a stale value) also
    // covers hosts that reuse a live instance across project loads.
    lastSeqPreset.store(-1, std::memory_order_relaxed);
    seqStateRestored.store(true, std::memory_order_relaxed);

    // Restore MIDI output device (per-installation setting, not per-preset).
    // Null-guarded: getXmlFromBinary returns null for any blob that is not a JUCE
    // binary XML, and "Play Session Log…" now feeds an arbitrary user-chosen file
    // into this path. Everything above already tolerates a null xml; this did not.
    if (xml != nullptr)
    {
        const auto deviceId = xml->getStringAttribute("midiOutputDeviceId");
        if (deviceId.isNotEmpty())
            openMidiOutputDevice(deviceId);

        if (xml->getBoolAttribute("midiClockEnabled", false))
            setMidiClockEnabled(true);
    }
}

// ═══════════════════════════════════════════════════════════════════
// JSON Preset Import/Export (compatible with Vue reference format)
// ═══════════════════════════════════════════════════════════════════

// Conversion helpers matching useFilter.ts:
//   normalizedToFreq(n) = 20 * pow(1000, n)     // 0→20Hz, 1→20kHz
//   freqToNormalized(f) = log(f/20) / log(1000)
static float cutoffNormToHz(float n) { return 20.0f * std::pow(1000.0f, juce::jlimit(0.0f, 1.0f, n)); }
static float cutoffHzToNorm(float hz) { return std::log(juce::jlimit(20.0f, 20000.0f, hz) / 20.0f) / std::log(1000.0f); }

// ── Choice-parameter string↔index helpers ──
//
// All *FromString/*ToString helpers route through the single source of
// truth in BlockParams.h: every choice parameter has a kEntries[] table
// with a stable snake_case `.key` column used for JSON serialization.
// These helpers are 3-line wrappers over `choiceFromKey` / `choiceToKey`
// below, which do a linear scan over the .key column (kCount is small).

template <std::size_t N>
static int choiceFromKey(const juce::String& s, const ChoiceEntry (&entries)[N]) {
    for (std::size_t i = 0; i < N; ++i)
        if (s == entries[i].key) return static_cast<int>(i);
    return 0; // first entry (typically "none"/"off"/"---")
}
template <std::size_t N>
static juce::String choiceToKey(int i, const ChoiceEntry (&entries)[N]) {
    return juce::String(i >= 0 && i < static_cast<int>(N) ? entries[i].key : entries[0].key);
}

static int filterTypeFromString(const juce::String& s)  { return choiceFromKey(s, FilterType::kEntries); }
static juce::String filterTypeToString(int i)           { return choiceToKey(i, FilterType::kEntries); }

static int filterSlopeFromString(const juce::String& s) { return choiceFromKey(s, FilterSlope::kEntries); }
static juce::String filterSlopeToString(int i)          { return choiceToKey(i, FilterSlope::kEntries); }

static int filterDriveOsFromString(const juce::String& s) { return choiceFromKey(s, FilterDriveOs::kEntries); }
static juce::String filterDriveOsToString(int i)          { return choiceToKey(i, FilterDriveOs::kEntries); }

static int filterAlgorithmFromString(const juce::String& s) { return choiceFromKey(s, FilterAlgorithm::kEntries); }
static juce::String filterAlgorithmToString(int i)          { return choiceToKey(i, FilterAlgorithm::kEntries); }

// The one renamed key in the whole parameter set (BlockParams.h says why).
// Without this, choiceFromKey falls through to index 0 and every preset that
// stored the old name would load Tanh — a different saturation, silently.
static int filterWarpStyleFromString(const juce::String& s) {
    if (s == "ojd") return FilterWarpStyle::Algebraic;
    return choiceFromKey(s, FilterWarpStyle::kEntries);
}
static juce::String filterWarpStyleToString(int i)          { return choiceToKey(i, FilterWarpStyle::kEntries); }

static int envTargetFromString(const juce::String& s)   { return choiceFromKey(s, EnvTarget::kEntries); }
static juce::String envTargetToString(int i)            { return choiceToKey(i, EnvTarget::kEntries); }

static int lfoTargetFromString(const juce::String& s)   { return choiceFromKey(s, LfoTarget::kEntries); }
static juce::String lfoTargetToString(int i)            { return choiceToKey(i, LfoTarget::kEntries); }

static int lfoWaveFromString(const juce::String& s)     { return choiceFromKey(s, LfoWave::kEntries); }
static juce::String lfoWaveToString(int i)              { return choiceToKey(i, LfoWave::kEntries); }

static int lfoModeFromString(const juce::String& s)     { return choiceFromKey(s, LfoMode::kEntries); }
static juce::String lfoModeToString(int i)              { return choiceToKey(i, LfoMode::kEntries); }

static int driftTargetFromString(const juce::String& s) { return choiceFromKey(s, DriftTarget::kEntries); }
static juce::String driftTargetToString(int i)          { return choiceToKey(i, DriftTarget::kEntries); }

static int driftWaveFromString(const juce::String& s)   { return choiceFromKey(s, DriftWave::kEntries); }
static juce::String driftWaveToString(int i)            { return choiceToKey(i, DriftWave::kEntries); }

static int clockModeFromString(const juce::String& s)     { return choiceFromKey(s, ClockMode::kEntries); }
static juce::String clockModeToString(int i)              { return choiceToKey(i, ClockMode::kEntries); }

static int clockDivisionFromString(const juce::String& s) { return choiceFromKey(s, ClockDivision::kEntries); }
static juce::String clockDivisionToString(int i)          { return choiceToKey(i, ClockDivision::kEntries); }

// Drift owns a SEPARATE, slower division list (DriftDivision, 64/1 … 1/4) and
// must serialise against THAT table, not the shared ClockDivision one. The keys
// overlap for the common divisions, so a pre-split preset reloads by key. An
// unknown/removed key (e.g. a now-too-fast "1_8" or a tuplet) maps to the
// fast-end 1/4 — the nearest surviving step for those removed faster divisions,
// NOT the 2/1 param default — instead of choiceFromKey()'s index-0 fallback
// (= 64/1, the slowest division).
static int driftDivisionFromString(const juce::String& s) {
    for (int i = 0; i < DriftDivision::kCount; ++i)
        if (s == DriftDivision::kEntries[i].key) return i;
    return DriftDivision::D1_4;
}
static juce::String driftDivisionToString(int i)          { return choiceToKey(i, DriftDivision::kEntries); }

// The env curve is a continuous bend now, but a preset still writes the nearest
// NAMED anchor as "<stage>Curve" alongside the exact "<stage>Bend": a build older
// than 2026-08-05 reads only the key and lands on the closest curve it owns
// instead of choiceFromKey's index-0 fallback (Log). New readers take the bend
// when it is there — see the reader in importJsonPreset.
static float curveBendFromString(const juce::String& s)
{
    return EnvCurve::bendFromIndex(choiceFromKey(s, EnvCurve::kEntries));
}
static juce::String curveBendToString(float bend)
{
    return choiceToKey(EnvCurve::nearestIndex(bend), EnvCurve::kEntries);
}
static int envVelTimeModeFromString(const juce::String& s) { return choiceFromKey(s, EnvVelTimeMode::kEntries); }
static juce::String envVelTimeModeToString(int i)          { return choiceToKey(i, EnvVelTimeMode::kEntries); }

// ── PID group tables for looped save/load of envelopes, LFOs, drift ──
// The save/load table is the amp envelope followed by PID::modEnv verbatim, so
// the mod envelopes' IDs are written down in exactly one place (BlockParams.h).
using EnvPIDs = PID::ModEnvIds;
static constexpr const EnvPIDs* kEnvPIDs   = PID::allEnvs;
static constexpr int            kNumEnvPIDs = PID::kNumEnvs;

struct LfoPIDs {
    const char* rate; const char* depth; const char* wave;
    const char* target; const char* mode;
    const char* clockMode; const char* clockDivision;
    constexpr std::array<const char*, 7> all() const
    { return { rate, depth, wave, target, mode, clockMode, clockDivision }; }
};
static_assert(sizeof(LfoPIDs) == 7 * sizeof(const char*),
              "LfoPIDs gained a field -- add it to all().");
static constexpr LfoPIDs kLfoPIDs[] = {
    { PID::lfo1Rate, PID::lfo1Depth, PID::lfo1Wave, PID::lfo1Target, PID::lfo1Mode,
      PID::lfo1ClockMode, PID::lfo1ClockDivision },
    { PID::lfo2Rate, PID::lfo2Depth, PID::lfo2Wave, PID::lfo2Target, PID::lfo2Mode,
      PID::lfo2ClockMode, PID::lfo2ClockDivision },
    { PID::lfo3Rate, PID::lfo3Depth, PID::lfo3Wave, PID::lfo3Target, PID::lfo3Mode,
      PID::lfo3ClockMode, PID::lfo3ClockDivision },
};
static constexpr int kNumLfoPIDs = sizeof(kLfoPIDs) / sizeof(kLfoPIDs[0]);

struct DriftPIDs {
    const char* rate; const char* depth; const char* target; const char* wave;
    const char* clockMode; const char* clockDivision;
    constexpr std::array<const char*, 6> all() const
    { return { rate, depth, target, wave, clockMode, clockDivision }; }
};
static_assert(sizeof(DriftPIDs) == 6 * sizeof(const char*),
              "DriftPIDs gained a field -- add it to all().");
static constexpr DriftPIDs kDriftPIDs[] = {
    { PID::drift1Rate, PID::drift1Depth, PID::drift1Target, PID::drift1Wave,
      PID::drift1ClockMode, PID::drift1ClockDivision },
    { PID::drift2Rate, PID::drift2Depth, PID::drift2Target, PID::drift2Wave,
      PID::drift2ClockMode, PID::drift2ClockDivision },
    { PID::drift3Rate, PID::drift3Depth, PID::drift3Target, PID::drift3Wave,
      PID::drift3ClockMode, PID::drift3ClockDivision },
};
static constexpr int kNumDriftPIDs = sizeof(kDriftPIDs) / sizeof(kDriftPIDs[0]);

// Helper to safely set a parameter value
static void setParam(juce::AudioProcessorValueTreeState& p, const juce::String& id, float val) {
    if (auto* param = p.getParameter(id))
        param->setValueNotifyingHost(param->convertTo0to1(val));
}

// Put a parameter back to the value createParameterLayout gave it. Reads the
// default OUT of the layout rather than restating it, so a load path can never
// disagree with the layout about what "unset" means.
static void setParamToLayoutDefault(juce::AudioProcessorValueTreeState& p, const juce::String& id) {
    if (auto* param = p.getParameter(id))
        param->setValueNotifyingHost(param->getDefaultValue());
}

// ── Reading a preset key the file may not carry ────────────────────────────
// getProperty() returns a VOID juce::var for a key that is not there, and a
// void var converts to 0.0f. So `static_cast<float>(obj->getProperty(key))` is
// not a tolerant read that falls back to anything — it writes a hard 0 into
// the parameter. For every parameter whose declared default is not 0 that is
// silently a different patch: a limiter threshold at 0 dB instead of -3 (which
// since the master-stage fix is 3 dB of output gain), a cutoff at 20 Hz
// instead of 20 kHz, a sequencer at 0 BPM, an amp envelope at Amt 0.
//
// The law is the one the Drift block below already states in prose: a key the
// file does not carry means the value createParameterLayout declared, never
// the void var's zero. The helpers below are how that law is applied, so no
// reader has to re-derive it per key. A key the file DOES carry is read
// exactly as before — this changes nothing about well-formed presets, only
// about hand-edited, foreign, truncated or older-format ones.
static void setParamFromJson(juce::AudioProcessorValueTreeState& p, const juce::String& id,
                             const juce::DynamicObject* obj, const char* key) {
    if (obj != nullptr && obj->hasProperty(key)) setParam(p, id, static_cast<float>(obj->getProperty(key)));
    else                                         setParamToLayoutDefault(p, id);
}

// Same, for a key the exporter writes as a bool.
static void setBoolParamFromJson(juce::AudioProcessorValueTreeState& p, const juce::String& id,
                                 const juce::DynamicObject* obj, const char* key) {
    if (obj != nullptr && obj->hasProperty(key))
        setParam(p, id, static_cast<bool>(obj->getProperty(key)) ? 1.0f : 0.0f);
    else
        setParamToLayoutDefault(p, id);
}

// Same, for a value that has to be converted or calibration-migrated on the
// way in: `read` receives the file's var and runs ONLY when the key is there,
// so a migration can never be handed a void var to rescale.
template <typename ReadFn>
static void setParamFromJson(juce::AudioProcessorValueTreeState& p, const juce::String& id,
                             const juce::DynamicObject* obj, const char* key, ReadFn&& read) {
    if (obj != nullptr && obj->hasProperty(key)) setParam(p, id, read(obj->getProperty(key)));
    else                                         setParamToLayoutDefault(p, id);
}

// Same, for a choice parameter stored as its stable snake_case key string.
// This one is the quiet half of the defect: choiceFromKey("") is 0, which is
// the right answer only where the layout's default is index 0 as well. Where
// it is not, an absent key picks the FIRST entry and reads as a deliberate
// setting — Octave -2, 32 frames, a 1/1 sequencer division, Anchor instead of
// Line. Declared after choiceFromKey, which it needs.
template <std::size_t N>
static void setChoiceParamFromJson(juce::AudioProcessorValueTreeState& p, const juce::String& id,
                                   const juce::DynamicObject* obj, const char* key,
                                   const ChoiceEntry (&entries)[N]) {
    if (obj != nullptr && obj->hasProperty(key))
        setParam(p, id, static_cast<float>(choiceFromKey(obj->getProperty(key).toString(), entries)));
    else
        setParamToLayoutDefault(p, id);
}

// Same, for the choices that have a NAMED key->index helper above. Those
// helpers are the single place a renamed or retired key can be repaired on the
// way in — filterWarpStyleFromString's "ojd" is the one that carries such a
// repair today, and it is hand-rolled below rather than routed through here —
// so a site that went straight to the table would be the wrong place to add
// the next one. A plain function pointer, not a template, so it cannot compete
// with the table overload above.
static void setChoiceParamFromJson(juce::AudioProcessorValueTreeState& p, const juce::String& id,
                                   const juce::DynamicObject* obj, const char* key,
                                   int (*fromString)(const juce::String&)) {
    if (obj != nullptr && obj->hasProperty(key))
        setParam(p, id, static_cast<float>(fromString(obj->getProperty(key).toString())));
    else
        setParamToLayoutDefault(p, id);
}

// A non-APVTS destination (the sampler's markers, a sequencer step, the LCO
// bake snapshot) has no layout to ask, so the fallback is stated at the call
// site and cited there. Returns the file's value, or `fallback` when the key
// is absent.
static float jsonFloatOr(const juce::DynamicObject* obj, const char* key, float fallback) {
    return (obj != nullptr && obj->hasProperty(key))
               ? static_cast<float>(static_cast<double>(obj->getProperty(key))) : fallback;
}

// The layout's default in the parameter's OWN units, for the cases that cannot
// just call setParamToLayoutDefault: a parameter two JSON keys share, and one
// whose value is also needed as a count before it is written. Rounds rather
// than truncates, because the round trip through the normalised domain does not
// always land back exactly on the integer (see exportJsonPreset's getInt).
static int layoutDefaultIntOf(juce::AudioProcessorValueTreeState& p, const juce::String& id) {
    auto* param = p.getParameter(id);
    return param != nullptr ? juce::roundToInt(param->convertFrom0to1(param->getDefaultValue())) : 0;
}

// ── The shelf of synth knobs the LRO author is shown ──
// BJ 2026-07-29: the author steers the filter (and the envelopes and LFOs) from
// OUTSIDE — through the synth's own parameters — not with a `tone` baked into
// the Csound body. This is the shelf that gets handed to it, built from the same
// id tables the preset loader walks so it cannot fall behind them.
//
// Sound-shaping only. Deliberately absent: everything about GENERATION (prompt,
// seed, duration, model), the engine's IDENTITY (mode, voice count, tuning) —
// neither is a knob on the instrument — and `amp_target`, which is the player's
// DCA routing and belongs to them (BJ: the switch decides whether the synth may
// touch the parameters at all; the DCA is not part of that bargain).
static const std::vector<const char*>& authorParamShelf()
{
    static const std::vector<const char*> shelf = [] {
        std::vector<const char*> v {
            PID::filterEnabled, PID::filterType, PID::filterSlope, PID::filterCutoff,
            PID::filterResonance, PID::filterMix, PID::filterKbdTrack, PID::filterDrive,
            PID::filterDriveOs, PID::filterAlgorithm, PID::filterWarpStyle,
            PID::noiseLevel, PID::noiseType,
            PID::delayType, PID::delayTime, PID::delayFeedback, PID::delayMix,
            PID::delayDamp, PID::delayClockMode, PID::delayClockDivision,
            PID::reverbType, PID::reverbMix,
            // The amplifier chain. On the shelf because BJ's answer to where the
            // wiring belongs was "die Woerter erreichbar machen": a prompt asking
            // for a Rhodes with tremolo, a phased stage piano or an overdriven
            // Wurlitzer has to be able to reach these, and this shelf is what the
            // author is allowed to set. The entry `ep_fm3` carries no effect of
            // its own -- a suitcase has ONE tremolo for the instrument, not one
            // per key -- so this is the only place those words can land.
            PID::fxDistOn, PID::fxChorusOn, PID::fxPhaserOn, PID::fxTremOn,
            PID::fxDistDrive, PID::fxDistMix,
            PID::fxTremRate, PID::fxTremDepth, PID::fxTremStereo, PID::fxTremWave,
            PID::fxChorusRate, PID::fxChorusDepth, PID::fxChorusMix,
            PID::fxPhaserRate, PID::fxPhaserDepth, PID::fxPhaserFeedback,
            PID::fxPhaserMix,
        };
        for (int i = 0; i < kNumEnvPIDs; ++i)
            for (const char* id : kEnvPIDs[i].all())
                if (std::strcmp(id, PID::ampTarget) != 0)   // the player's DCA routing
                    v.push_back(id);
        for (const auto& lp : kLfoPIDs)   for (const char* id : lp.all()) v.push_back(id);
        for (const auto& dp : kDriftPIDs) for (const char* id : dp.all()) v.push_back(id);
        v.push_back(PID::driftEnabled);
        for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
            // Not Cache, not Snap. This shelf is sound-shaping, and those two do
            // not shape a sound - they MOVE THE INSTRUMENT. Handing the Snap bar
            // to an authoring model would hand it, one pressure gesture later,
            // everything a snapshot recall restores: engine mode, voice count,
            // tuning, seed, duration, the prompts and the sample. That is the
            // generation state and the engine identity this shelf exists to
            // withhold. The loop is why this needs saying out loud - it takes
            // whatever the enum grows, and it grew two things it must not take.
            if (! AftertouchTarget::movesTheInstrument(t))
            {
                v.push_back(kAftertouchAmtPid[t]);
                // The source travels with its amount, for the same reason the
                // amount is here at all: a depth on an axis nobody is touching
                // is not the routing that was snapped.
                v.push_back(kExprSrcPid[t]);
            }
        return v;
    }();
    return shelf;
}

bool T5ynthProcessor::onAuthorParamShelf(const juce::String& id)
{
    for (const char* entry : authorParamShelf())
        if (id == entry) return true;
    return false;
}

// What the shelf CALLS a control — the panel's own name, not the APVTS one. The
// APVTS name is what a DAW's automation list shows and is not ours to rename,
// but every one of the five envelopes reads wrong through it: the amp
// envelope's stages are plain "Attack"/"Decay"/"Sustain"/"Release", with nothing
// saying which of five envelopes they belong to, and `modEnv[0]`'s are "Mod1 …"
// where the panel says ENV2. An author that takes "Mod1 Decay" and a player
// looking for it under ENV2 would then be talking about different envelopes.
static juce::String authorShelfName(const char* id, const juce::AudioProcessorParameter& prm)
{
    const juce::String plain = prm.getName(64);
    for (int e = 0; e < PID::kNumEnvs; ++e)
        for (const char* member : PID::allEnvs[e].all())
            if (std::strcmp(member, id) == 0)
            {
                // createParameterLayout writes "Mod<n> " on every mod-envelope
                // parameter and "Amp " on most of the amp envelope's — but its
                // four stage times carry no prefix at all ("Attack"), so both
                // branches have to be allowed to miss.
                juce::String stage = plain;
                if (stage.startsWith("Amp "))
                    stage = stage.substring(4);
                else if (stage.startsWith("Mod") && stage.indexOfChar(' ') > 0)
                    stage = stage.substring(stage.indexOfChar(' ') + 1);
                return "ENV" + juce::String(e + 1) + " " + stage;
            }
    return plain;
}

// The three per-stage curve parameters of any envelope, asked by id.
static bool isEnvCurveId(const char* id)
{
    for (int e = 0; e < PID::kNumEnvs; ++e)
    {
        const auto& ids = PID::allEnvs[e];
        if (std::strcmp(id, ids.attackCurve) == 0
            || std::strcmp(id, ids.decayCurve) == 0
            || std::strcmp(id, ids.releaseCurve) == 0)
            return true;
    }
    return false;
}

juce::var T5ynthProcessor::buildAuthorParamIndex() const
{
    juce::Array<juce::var> out;
    for (const char* id : authorParamShelf())
    {
        auto* prm = parameters.getParameter(id);
        if (prm == nullptr) continue;

        auto entry = juce::DynamicObject::Ptr(new juce::DynamicObject());
        entry->setProperty("id",   juce::String(id));
        entry->setProperty("name", authorShelfName(id, *prm));

        if (auto* bp = dynamic_cast<juce::AudioParameterBool*>(prm))
        {
            // A switch, shown as a switch: presented as 0 .. 1 the author has no
            // way to know it is not a continuous control.
            juce::Array<juce::var> words; words.add("off"); words.add("on");
            entry->setProperty("choices", words);
            entry->setProperty("value", bp->get() ? "on" : "off");
        }
        else if (auto* ch = dynamic_cast<juce::AudioParameterChoice*>(prm))
        {
            // The author names a choice by the LABEL the player reads on screen,
            // so what it writes and what the UI shows are the same word.
            juce::Array<juce::var> labels;
            for (const auto& c : ch->choices) labels.add(c);
            entry->setProperty("choices", labels);
            entry->setProperty("value", ch->getCurrentChoiceName());
        }
        else
        {
            const auto& r = prm->getNormalisableRange();
            entry->setProperty("min",   r.start);
            entry->setProperty("max",   r.end);
            entry->setProperty("value", prm->convertFrom0to1(prm->getValue()));
            // A bare number range hides what its sign means, and the env curve
            // used to hand the author the five words themselves. It is a
            // continuous bend now, so the words come along as the axis they still
            // label — as prose for the shelf line AND as a word→value table, so
            // an author that writes `SET amp_release_curve exp`, which is exactly
            // what it was shown until today, still lands on the shape it means
            // instead of being refused as "not a number".
            if (isEnvCurveId(id))
            {
                // The far end differs per stage, so it is read off THIS
                // parameter rather than written out once: an attack sags towards
                // Log and travels on that side, a decay and a release towards Exp.
                const bool sagsToLog = r.start < -EnvCurve::kBendPole;
                entry->setProperty("scale",
                    juce::String("-1 Log, -0.5 SLog, 0 Lin, +0.5 SExp, +1 Exp; and on to ")
                        + (sagsToLog ? "-2, a rise that stays down longer"
                                     : "+2, a faster fall with a longer quiet tail"));
                // BOTH spellings of each shape: the prose above names the labels
                // (SExp), the preset files name the keys (softexp), and an author
                // that has read either one must land on the same value.
                auto words = juce::DynamicObject::Ptr(new juce::DynamicObject());
                for (int c = 0; c < EnvCurve::kCount; ++c)
                {
                    words->setProperty(EnvCurve::kEntries[c].key,   EnvCurve::bendFromIndex(c));
                    words->setProperty(EnvCurve::kEntries[c].label, EnvCurve::bendFromIndex(c));
                }
                entry->setProperty("scale_words", juce::var(words.get()));
            }
        }
        out.add(juce::var(entry.get()));
    }
    return juce::var(out);
}

void T5ynthProcessor::releaseAuthorSettings()
{
    // Hand back what the last authoring borrowed. A knob whose NORMALISED value
    // is still exactly what was written there has not been touched since, so it
    // goes back; one that differs has been moved by the PLAYER and is left
    // where they left it. Compared normalised on purpose: the value that
    // survives the parameter's own 0..1 mapping is bit-stable, the denormalised
    // one it maps back to is not — over a skewed range like the filter cutoff's
    // two thirds of all values fail to compare equal to themselves, and every
    // one of those would strand the player's setting for good.
    for (const auto& prev : authorSetParams_)
        if (auto* prm = parameters.getParameter(prev.id))
            if (std::abs(prm->getValue() - prev.appliedNorm) < 1.0e-6f)
                setParam(parameters, prev.id, prev.before);
    authorSetParams_.clear();
    // Nothing of the author's stands on the patch now, so the station's live
    // half is empty — and anything showing it has to be told, since the give-back
    // can happen minutes after the authoring and from a switch on another thread.
    authorAppliedLines_.clear();
    ++authorSettingsRevision_;
}

T5ynthProcessor::AuthorKnobStand T5ynthProcessor::authorKnobStand() const
{
    // Both conditions read live, in ONE place: the take, the give-back and the
    // report all ask this, and a second copy of the test is how they would come
    // to describe different patches — a panel that re-read only the switch would
    // promise "turn KNOBS on and these land" while another oscillator is playing.
    if (static_cast<int>(paramCache.engineMode->load()) != EngineMode::Csound)
        return AuthorKnobStand::notSounding;
    if (! (parameters.getRawParameterValue(PID::lcoSetsParams)->load() > 0.5f))
        return AuthorKnobStand::switchOff;
    return AuthorKnobStand::onThePatch;
}

bool T5ynthProcessor::authorMayHoldSettings() const
{
    return authorKnobStand() == AuthorKnobStand::onThePatch;
}

void T5ynthProcessor::dropAuthorSettings()
{
    // No give-back: the caller is a patch replacement whose own values are about
    // to be written over every one of these knobs, and putting the previous
    // patch's values back first would only be a detour. The bookkeeping is the
    // point of the function — done by hand at the one call site, it was left out
    // and the KNOBS station went on showing the previous sound's answer.
    authorSetParams_.clear();
    authorSettings_ = juce::var();
    authorAppliedLines_.clear();
    ++authorSettingsRevision_;
    ++authorSettingsGeneration_;
}

void T5ynthProcessor::reconcileAuthorSettings()
{
    // The knobs are the author's while the player allows it AND its orchestra is
    // what is sounding. Anything else and they are the player's.
    //
    // Both conditions are read here, live, rather than passed in: the two edges
    // that call this (the switch, the oscillator toggle) can arrive in either
    // order and more than once before the async update runs, and the state the
    // controls SHOW is the only one that cannot be stale. Re-applying is
    // idempotent — applyAuthorSettings releases first — so a redundant call
    // costs nothing and a missed one cannot leave the patch half-borrowed.
    if (authorMayHoldSettings())
        applyAuthorSettings(authorSettings_);   // a no-op while nothing is authored
    else
        releaseAuthorSettings();
}

void T5ynthProcessor::setAuthorSettings(const juce::var& settings)
{
    // The PREVIOUS sound's borrow goes back first, and its request is replaced
    // here whether or not either of them may stand on the patch right now. That
    // is the half that used to be missing: with the switch off nothing was
    // stored at all, so the last sound's request stayed on record and flipping
    // the switch on put THAT instrument's filter and envelopes on the patch.
    releaseAuthorSettings();
    authorSettings_ = settings;
    ++authorSettingsGeneration_;   // a different instrument is asking now

    // Only the TAKING is gated. A sound authored while the switch is off is
    // written complete — its settings are made, they simply wait — and the
    // switch takes them when it arrives, through reconcileAuthorSettings.
    if (authorMayHoldSettings())
        applyAuthorSettings(authorSettings_);   // bumps the revision itself
    else
        ++authorSettingsRevision_;              // a new request is waiting: say so
}

bool T5ynthProcessor::resolveAuthorSetting(const juce::var& e,
                                           juce::RangedAudioParameter*& prm,
                                           float& target) const
{
    // The backend already checked this against the shelf it was handed and says
    // so. Checked AGAIN here, against the shelf itself, because the reply
    // crosses a process boundary: a REJECTED line still travels (it carries the
    // reason, for showing), and "the parameter exists" is not the same question
    // as "it was on the shelf" — amp_target exists.
    //
    // ONE resolver for the apply and for the report, so a line shown as waiting
    // is exactly a line that will land, and every refusal is refused in both.
    prm = nullptr;
    target = 0.0f;
    if (! static_cast<bool>(e.getProperty("ok", juce::var(false)))) return false;
    const auto id = e.getProperty("id", juce::var()).toString();
    if (! onAuthorParamShelf(id)) return false;
    prm = parameters.getParameter(id);
    if (prm == nullptr || ! e.hasProperty("value")) { prm = nullptr; return false; }

    const auto raw = e.getProperty("value", juce::var());
    if (dynamic_cast<juce::AudioParameterBool*>(prm) != nullptr)
    {
        const auto w = raw.toString().trim().toLowerCase();
        if (w != "on" && w != "off") { prm = nullptr; return false; }
        target = (w == "on") ? 1.0f : 0.0f;
    }
    else if (auto* ch = dynamic_cast<juce::AudioParameterChoice*>(prm))
    {
        const int idx = ch->choices.indexOf(raw.toString(), /*ignoreCase=*/true);
        if (idx < 0) { prm = nullptr; return false; }   // a label this parameter does not have
        target = static_cast<float>(idx);
    }
    else
    {
        // A number, and only a number: a juce::var holding a STRING converts
        // to a float silently, which is how a value the backend refused
        // ("0.5-0.7") would land as 0.5 anyway.
        if (! (raw.isDouble() || raw.isInt() || raw.isInt64())) { prm = nullptr; return false; }
        const auto& r = prm->getNormalisableRange();
        target = juce::jlimit(r.start, r.end, static_cast<float>(raw));
    }
    return true;
}

juce::StringArray T5ynthProcessor::describeAuthorSettings(AuthorKnobStand& stand) const
{
    stand = authorKnobStand();
    if (stand == AuthorKnobStand::onThePatch)
        return authorAppliedLines_;   // what actually stands, read back by the apply

    // Waiting: the same lines the apply would write, formatted from the request
    // itself. Only the resolver decides which they are, so this list cannot
    // promise a knob the switch would then refuse.
    juce::StringArray asked;
    if (auto* arr = authorSettings_.getArray())
        for (const auto& e : *arr)
        {
            juce::RangedAudioParameter* prm = nullptr;
            float target = 0.0f;
            if (! resolveAuthorSetting(e, prm, target)) continue;
            asked.add(authorShelfName(e.getProperty("id", juce::var()).toString().toRawUTF8(), *prm)
                      + "  " + prm->getText(prm->convertTo0to1(target), 0));
        }
    return asked;
}

void T5ynthProcessor::forgetAuthorSettings()
{
    // Order matters: give the knobs back to THIS patch first, then drop the
    // record. Dropping first would strand them at the authored values with
    // nothing left able to return them.
    releaseAuthorSettings();
    authorSettings_ = juce::var();
    ++authorSettingsGeneration_;   // a different patch: this request is nobody's now
}

juce::StringArray T5ynthProcessor::applyAuthorSettings(const juce::var& settings)
{
    juce::StringArray applied;
    // Deliberately NOT under a BulkParamLoadGuard: that guard suppresses the
    // EVENT LOG's parameter events, and an authoring is not recorded there by
    // anything else — suppressing them would make a .t5evt replay play the
    // authored sound with the pre-authoring knobs. It buys no atomicity anyway.
    //
    // Before the next sound takes anything, the last one gives its knobs back;
    // otherwise every regeneration would leave its cutoff and its ENV3 routing
    // standing on top of the one before it.
    releaseAuthorSettings();

    // Remembered past the release, so the switch can take these again after
    // handing them back — that is what makes KNOBS an A/B rather than a
    // one-way door. Stored BEFORE the early return below: an authoring that
    // wrote no SET line asked for nothing, and the sound before it must not
    // keep standing in for it. The guard is for the re-take, which passes this
    // very member back in.
    if (&settings != &authorSettings_)
    {
        authorSettings_ = settings;
        // A foreign request REPLACES the standing one, so it is a new generation
        // like any other replacement. Both call sites today pass authorSettings_
        // itself and never reach this, but the guard exists for the other case,
        // and a replacement that left the generation alone is exactly how a
        // panel comes to report one sound's answer under another's card.
        ++authorSettingsGeneration_;
    }

    auto* arr = settings.getArray();
    if (arr != nullptr)
      for (const auto& e : *arr)
      {
        juce::RangedAudioParameter* prm = nullptr;
        float target = 0.0f;
        if (! resolveAuthorSetting(e, prm, target)) continue;
        const auto id = e.getProperty("id", juce::var()).toString();

        AuthorSetParam rec;
        rec.id     = id;
        rec.before = prm->convertFrom0to1(prm->getValue());
        setParam(parameters, id, target);
        rec.appliedNorm = prm->getValue();   // read BACK: what actually stands there
        authorSetParams_.push_back(rec);

        // What the panel reports, taken from AFTER the write rather than from
        // the reply that asked for it. Every line the resolver refuses is one
        // the backend passed and this side still would not write, and each of
        // them would otherwise be shown as landed.
        applied.add(authorShelfName(id.toRawUTF8(), *prm)
                    + "  " + prm->getCurrentValueAsText());
      }

    // The station's live half, and the one signal that says it changed. Set on
    // every path, including the empty one: an authoring that asked for nothing
    // has to erase the previous sound's lines rather than leave them standing.
    authorAppliedLines_ = applied;
    ++authorSettingsRevision_;
    return applied;
}

void T5ynthProcessor::setCsoundControls(const LroControls& c, bool applyValues)
{
    csoundControls_ = c;
    ++csoundControlsRevision_;

    if (! applyValues)
        return;

    // Every one of the twelve, not only the declared ones. A knob left over
    // from the previous instrument would otherwise keep the position the player
    // had put it in and feed it to a body that means something entirely
    // different by that channel — the new sound would not be the sound the
    // author described, and nothing on screen would say why. Undeclared
    // channels go back to 0.5, which is what the orchestra head gives them.
    // The layer LEVELS are deliberately not in this reset: they are the
    // player's balance between the parts, a mixer and not a description of the
    // sound, so a new instrument does not seize them.
    for (const auto* id : kLroKnobIds)
    {
        float target = 0.5f;
        for (const auto& k : c.knobs)
            if (k.paramId == id)
            {
                target = k.value;
                break;
            }
        setParam(parameters, id, target);
    }
}

// ═══════════════════════════════════════════════════════════════════
// HF boost — two-band high shelf to compensate VAE decoder rolloff
// ═══════════════════════════════════════════════════════════════════

void T5ynthProcessor::applyHfBoost(juce::AudioBuffer<float>& buffer, double sampleRate)
{
    auto shelf1 = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
        sampleRate, 4000.0, 0.6, juce::Decibels::decibelsToGain(3.0f));
    auto shelf2 = juce::dsp::IIR::Coefficients<float>::makeHighShelf(
        sampleRate, 10000.0, 0.6, juce::Decibels::decibelsToGain(4.5f));

    juce::dsp::IIR::Filter<float> f1, f2;
    f1.coefficients = shelf1;
    f2.coefficients = shelf2;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        f1.reset();
        f2.reset();
        auto* data = buffer.getWritePointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            data[i] = f1.processSample(data[i]);
            data[i] = f2.processSample(data[i]);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Rumble filter — 2nd-order Butterworth HP at 25 Hz
// Removes DC offset and sub-bass rumble from VAE decoder output
// ═══════════════════════════════════════════════════════════════════

void T5ynthProcessor::applyRumbleFilter(juce::AudioBuffer<float>& buffer, double sampleRate)
{
    auto hp = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 25.0, 0.707);
    juce::dsp::IIR::Filter<float> f;
    f.coefficients = hp;

    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
    {
        f.reset();
        auto* data = buffer.getWritePointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            data[i] = f.processSample(data[i]);
    }
}

juce::String T5ynthProcessor::exportJsonPreset() const
{
    auto* p = const_cast<juce::AudioProcessorValueTreeState*>(&parameters);
    auto get = [&](const juce::String& id) -> float {
        auto* val = p->getRawParameterValue(id);
        jassert(val != nullptr); // fires in debug if PID is missing
        return val ? val->load() : 0.0f;
    };
    // Every integer-valued parameter (a step count, a choice index) arrives
    // here as a float, and a float carrying an integer is not always exactly
    // that integer: an AudioParameterInt/Choice that nothing has written yet
    // holds whatever its NORMALISED default converts back to, and inf_steps
    // sits at 7.9999995 in a freshly opened session. static_cast<int> truncated
    // that to 7, so a save recorded a value the parameter did not hold, and
    // reloading it moved the parameter. Round instead.
    //
    // Measured 2026-08-05 over all 120 int and choice parameters: inf_steps is
    // the only one that currently lands off-integer, and nothing downstream
    // reads it — the request forces the selected model's own step count at the
    // generation choke point (PromptPanel::…, "the request never trusts it").
    // So this is the export telling the truth about the parameter, not an
    // audible fix. It is done for all 67 casts because the conversion is wrong
    // for every one of them, not because inf_steps is special.
    auto getInt = [&](const juce::String& id) { return juce::roundToInt(get(id)); };

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    root->setProperty("version", 1);
    root->setProperty("name", "T5ynth Export");
    root->setProperty("timestamp", juce::Time::getCurrentTime().toISO8601(true));

    // Synth params
    juce::DynamicObject::Ptr synth = new juce::DynamicObject();
    synth->setProperty("promptA", ""); // prompts are GUI-only, not in APVTS
    synth->setProperty("promptB", "");
    synth->setProperty("alpha", get(PID::genAlpha));
    synth->setProperty("magnitude", get(PID::genMagnitude));
    synth->setProperty("noise", get(PID::genNoise));
    synth->setProperty("axesAmount", get(PID::genAxesAmount));
    synth->setProperty("resynth", get(PID::resynthAmount));
    synth->setProperty("velAmt", get(PID::velAmt));   // global velocity→peak amount
    // Re-Prompt (semantic loop): stance + coupling, saved as KEY strings so the
    // enum order can change without breaking presets. A preset saved before
    // Re-Prompt existed lacks both -> choiceFromKey("") -> 0 -> stance Off on load.
    synth->setProperty("repromptStance",
                       choiceToKey(getInt(PID::repromptStance), RepromptStance::kEntries));
    synth->setProperty("repromptCoupling",
                       choiceToKey(getInt(PID::repromptCoupling), RepromptCoupling::kEntries));
    synth->setProperty("resynthSource",
                       choiceToKey(getInt(PID::resynthSource), ResynthSource::kEntries));
    synth->setProperty("duration", get(PID::genDuration));
    synth->setProperty("startPosition", get(PID::genStart));
    synth->setProperty("steps", getInt(PID::infSteps));
    synth->setProperty("cfg", get(PID::genCfg));
    synth->setProperty("seed", getInt(PID::genSeed));
    synth->setProperty("model", lastModel);
    synth->setProperty("hfBoost", get(PID::genHfBoost) > 0.5f);
    // Modality epoch + authoring app version (v2.5.0+). A LEGACY preset (loaded
    // versionless, then re-saved) must stay versionless so it keeps the old routing,
    // so we OMIT both fields when legacy — absence is the switch (see importJsonPreset
    // and _native_modality_prefix in the backend).
    if (currentModalityEpoch_ != kLegacyModalityEpoch)
    {
        synth->setProperty("modalityEpoch", currentModalityEpoch_);
        synth->setProperty("appVersion", juce::String(ProjectInfo::versionString));
    }
    // Calibration epoch is unconditional (independent of the modality-legacy switch):
    // a fresh save is always at the current calibration, so a load→re-save never
    // re-migrates already-migrated values.
    synth->setProperty("calibEpoch", Calibration::kEpoch);
    root->setProperty("synth", synth.get());

    // Engine
    juce::DynamicObject::Ptr engine = new juce::DynamicObject();
    engine->setProperty("mode", choiceToKey(getInt(PID::engineMode), EngineMode::kEntries));
    // voiceCount and tuning are part of the engine config (polyphony + the
    // tuning table the VoiceManager applies). Both are in
    // MainPanel::kMainSnapshotParamIds so per-snapshot save round-trips
    // them, but they used to be omitted from the main preset JSON --
    // saving a preset with "12 voices / Maqam" would silently reset to
    // the APVTS defaults (8 voices / 12-TET) on reload.
    engine->setProperty("voiceCount", choiceToKey(getInt(PID::voiceCount), VoiceCount::kEntries));
    // NOT lcoSetsParams (BJ, 2026-07-30). It looked like part of the patch's
    // character and is not: the sound this file describes is fully in the values
    // it already carries — the cutoff and the envelope routings an authoring set
    // are saved like any others, and load back identically whether or not the
    // switch travels. What the switch decides is the NEXT authoring: whether it
    // may reach into the player's controls. That is authority over work not yet
    // done, and a file — least of all one from someone else — does not get to
    // grant it. It also made the switch unusable as the A/B it is, since every
    // preset load stamped it back down.
    engine->setProperty("tuning",     choiceToKey(getInt(PID::tuning),     TuningType::kEntries));
    engine->setProperty("loopMode", choiceToKey(getInt(PID::loopMode), LoopMode::kEntries));
    engine->setProperty("loopStartFrac", static_cast<double>(masterSampler.getLoopStart()));
    engine->setProperty("loopEndFrac", static_cast<double>(masterSampler.getLoopEnd()));
    engine->setProperty("startPosFrac", static_cast<double>(masterSampler.getStartPos()));
    engine->setProperty("wtExtractStart", static_cast<double>(masterSampler.getWtExtractStart()));
    engine->setProperty("wtExtractEnd", static_cast<double>(masterSampler.getWtExtractEnd()));
    engine->setProperty("pointsLocked", masterSampler.getPointsLocked());
    engine->setProperty("crossfadeMs", get(PID::crossfadeMs));
    engine->setProperty(PID::normalize, get(PID::normalize) > 0.5f);
    engine->setProperty("loopOptimize", choiceToKey(getInt(PID::loopOptimize), LoopOptimize::kEntries));
    // Csound orchestra (Phase 5, SPEC_phase4_5_csound_llm_preset.md): only
    // written when the engine is actually in Csound mode, mirroring the
    // modality-epoch "absence is the switch" convention above — a preset
    // saved in any other engine mode carries no csound_orchestra/csound_
    // reading at all, so an OLDER T5ynth build loading it sees exactly the
    // same JSON shape it always has. The orchestra TEXT is read from
    // csoundPendingOrchestraText_ (empty = built-in), NOT from the currently-
    // ACTIVE CsoundEngine's orchestraText() (adversarial-review finding,
    // post-implementation): csoundActiveIdx_ only flips at the END of a
    // post-bake crossfade, but requestCsoundOrchestra() writes
    // csoundPendingOrchestraText_ synchronously, in lockstep with
    // PromptPanel's setCsoundReading() call right after it (see
    // triggerDcoBake's completion lambda) — reading the active engine
    // instead would pair the OLD orchestra with the NEW reading for any Save
    // that lands inside that fade window (or before the very first bootstrap
    // compile has even started). Once the fade settles both sources agree
    // (the active engine's compiled text catches up to what was pending), so
    // this is a strict improvement with no behavior change in the settled
    // state. Locked because csoundPendingOrchestraText_ is written under
    // csoundLifecycleMutex_ from requestCsoundOrchestra(), which may run on
    // a different thread than this (const, but the mutex is `mutable`) call.
    if (getInt(PID::engineMode) == static_cast<int>(EngineMode::Csound))
    {
        juce::String pendingOrchestraText;
        {
            std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
            pendingOrchestraText = csoundPendingOrchestraText_;
        }
        engine->setProperty("csound_orchestra", pendingOrchestraText);
        engine->setProperty("csound_prompt", csoundPrompt_);
        engine->setProperty("csound_reading", csoundReading_);
        engine->setProperty("csound_params_text", csoundParamsText_);
        // What each of the twelve lro_p* parameters MEANS in this instrument —
        // the library parameters its body kept. Structured, not a text blob:
        // the panel reads it straight, and only backend/lco_write.py ever reads
        // a parameter line out of Csound.
        // Layers count as content here even with no knob at all: a body that
        // kept no library line still has its `kvolN` mix, and dropping the
        // block would take its levels off the panel on reload.
        if (! csoundControls_.isEmpty() || ! csoundControls_.layers.empty())
            engine->setProperty("csound_controls", csoundControls_.toVar());
        // …and WHERE each of them stands. `csound_controls` carries only what a
        // knob MEANS; the positions live in the twelve lro_p* parameters, and
        // this payload is a hand-written list, so until now a saved preset came
        // back with the right slider names under the player's hand and the
        // author's starting values behind them. The three layer levels are here
        // for the same reason. Written unconditionally: a value the player moved
        // on an orchestra that declared no knob for it is still their setting.
        {
            juce::DynamicObject::Ptr knobs = new juce::DynamicObject();
            for (const auto* id : kLroKnobIds)
                knobs->setProperty(id, get(id));
            for (const auto* id : kLroLevelIds)
                knobs->setProperty(id, get(id));
            engine->setProperty("csound_knob_values", knobs.get());
        }
    }
    root->setProperty("engine", engine.get());

    // Modulation: every envelope in kEnvPIDs (amp + ENV 2..5)
    juce::DynamicObject::Ptr modObj = new juce::DynamicObject();
    juce::Array<juce::var> envArr;
    for (int i = 0; i < kNumEnvPIDs; ++i)
    {
        const auto& ep = kEnvPIDs[i];
        juce::DynamicObject::Ptr env = new juce::DynamicObject();
        env->setProperty("attackMs", get(ep.attack));
        env->setProperty("decayMs", get(ep.decay));
        env->setProperty("sustain", get(ep.sustain));
        env->setProperty("releaseMs", get(ep.release));
        env->setProperty("amount", get(ep.amount));
        env->setProperty("target", envTargetToString(getInt(ep.target)));
        env->setProperty("loop", get(ep.loop) > 0.5f);
        // The three per-stage curves are no longer choice indices, so the
        // round-instead-of-truncate rule that governs every other integer
        // parameter in this export does not reach them: they are the bend
        // itself now, written as a float beside its readable name.
        env->setProperty("attackCurve", curveBendToString(get(ep.attackCurve)));
        env->setProperty("decayCurve", curveBendToString(get(ep.decayCurve)));
        env->setProperty("releaseCurve", curveBendToString(get(ep.releaseCurve)));
        env->setProperty("attackBend", get(ep.attackCurve));
        env->setProperty("decayBend", get(ep.decayCurve));
        env->setProperty("releaseBend", get(ep.releaseCurve));
        env->setProperty("attackVelSens", get(ep.attackVelSens));
        env->setProperty("decayVelSens", get(ep.decayVelSens));
        env->setProperty("releaseVelSens", get(ep.releaseVelSens));
        envArr.add(env.get());
    }
    modObj->setProperty("envs", envArr);

    // Modulation: 3 LFOs
    juce::Array<juce::var> lfoArr;
    for (int i = 0; i < 3; ++i)
    {
        const auto& lp = kLfoPIDs[i];
        juce::DynamicObject::Ptr lfo = new juce::DynamicObject();
        lfo->setProperty("rate", get(lp.rate));
        lfo->setProperty("depth", get(lp.depth));
        lfo->setProperty("waveform", lfoWaveToString(getInt(lp.wave)));
        lfo->setProperty("target", lfoTargetToString(getInt(lp.target)));
        lfo->setProperty("mode", lfoModeToString(getInt(lp.mode)));
        lfo->setProperty("clockMode", clockModeToString(getInt(lp.clockMode)));
        lfo->setProperty("clockDivision", clockDivisionToString(getInt(lp.clockDivision)));
        lfoArr.add(lfo.get());
    }
    modObj->setProperty("lfos", lfoArr);

    // MIDI aftertouch routing: one bipolar amount per target, keyed by target
    // key. (Superseded the old single-select target + global amount; pre-existing
    // presets are migrated on load.) Saved in the main preset JSON as well as
    // MainPanel::kMainSnapshotParamIds, so a routed preset reloads intact.
    juce::DynamicObject::Ptr aftertouch = new juce::DynamicObject();
    for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
        aftertouch->setProperty(AftertouchTarget::kEntries[t].key, get(kAftertouchAmtPid[t]));
    modObj->setProperty("aftertouch", aftertouch.get());

    // Which axis drives each of those amounts (ExprSource) — a sibling block
    // rather than a second field inside the one above, because that one is keyed
    // by target and a target cannot carry two values. Stored as the source's KEY
    // string, like every other choice in a .t5p, so the enum's order is free to
    // grow without re-pointing a saved routing.
    juce::DynamicObject::Ptr exprSrc = new juce::DynamicObject();
    for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
    {
        const int src = juce::jlimit(0, ExprSource::kCount - 1,
                                     juce::roundToInt(get(kExprSrcPid[t])));
        exprSrc->setProperty(AftertouchTarget::kEntries[t].key,
                             juce::String(ExprSource::kEntries[src].key));
    }
    modObj->setProperty("exprSource", exprSrc.get());

    root->setProperty("modulation", modObj.get());

    // Drift LFOs
    juce::Array<juce::var> driftArr;
    for (int i = 0; i < 3; ++i)
    {
        const auto& dp = kDriftPIDs[i];
        juce::DynamicObject::Ptr d = new juce::DynamicObject();
        d->setProperty("rate", get(dp.rate));
        d->setProperty("depth", get(dp.depth));
        d->setProperty("waveform", driftWaveToString(getInt(dp.wave)));
        d->setProperty("target", driftTargetToString(getInt(dp.target)));
        d->setProperty("clockMode", clockModeToString(getInt(dp.clockMode)));
        d->setProperty("clockDivision", driftDivisionToString(getInt(dp.clockDivision)));
        driftArr.add(d.get());
    }
    root->setProperty("driftLfos", driftArr);
    root->setProperty("driftEnabled", get(PID::driftEnabled) > 0.5f);
    root->setProperty("driftCrossfade", get(PID::driftCrossfade));
    root->setProperty("regenMode", choiceToKey(getInt(PID::driftRegen), DriftRegen::kEntries));
    root->setProperty("cacheAsync", get(PID::cacheAsync) > 0.5f);

    // Wavetable + Noise
    juce::DynamicObject::Ptr wt = new juce::DynamicObject();
    wt->setProperty("scan", get(PID::oscScan));
    wt->setProperty("octaveShift", choiceToKey(getInt(PID::oscOctave), OscOctave::kEntries));
    wt->setProperty("noiseLevel", get(PID::noiseLevel));
    wt->setProperty("noiseType", choiceToKey(getInt(PID::noiseType), NoiseKind::kEntries));
    wt->setProperty("frames", choiceToKey(getInt(PID::wtFrames), WtFrames::kEntries));
    wt->setProperty("smooth", get(PID::wtSmooth) > 0.5f);
    wt->setProperty("autoScan", get(PID::wtAutoScan) > 0.5f);
    root->setProperty("wavetable", wt.get());

    // LCO/DCO bake — prompt, both readings, A/B balance and the router's own
    // Re-Prompt stance. Written only when a bake snapshot exists (see
    // setLcoBakeSnapshot); its absence is the "not an LCO preset" fallback
    // for both legacy files and a session that never touched LCO. Frame data
    // (A's baked strip, B's additive stations) is patched in by
    // PresetFormat::saveToFile, which alone has access to a fresh
    // snapshotLevel0Frames()/snapshotAdditiveBank() read.
    if (hasLcoBakeSnapshot())
    {
        juce::DynamicObject::Ptr lco = new juce::DynamicObject();
        lco->setProperty("prompt", lcoPrompt_);
        lco->setProperty("readingA", lcoReadingA_);
        lco->setProperty("readingB", lcoReadingB_);
        lco->setProperty("motionRateHz", static_cast<double>(lcoMotionRateHz_));
        lco->setProperty("oscAHasContent", lcoOscAHasContent_);
        lco->setProperty("gainA", static_cast<double>(lcoGainA_));
        lco->setProperty("oscBHasContent", lcoOscBHasContent_);
        lco->setProperty("gainB", static_cast<double>(lcoGainB_));
        lco->setProperty("repromptStance",
                         choiceToKey(getInt(PID::dcoRepromptStance), RepromptStance::kEntries));
        root->setProperty("lco", lco.get());
    }

    // Granular
    juce::DynamicObject::Ptr freeze = new juce::DynamicObject();
    freeze->setProperty("texture", choiceToKey(getInt(PID::freezeTexture),
                                               FreezeTexture::kEntries));
    freeze->setProperty("stereo", get(PID::freezeStereo));
    root->setProperty("freeze", freeze.get());

    // Effects
    juce::DynamicObject::Ptr fx = new juce::DynamicObject();
    fx->setProperty("delayType", choiceToKey(getInt(PID::delayType), DelayType::kEntries));
    fx->setProperty("delayTimeMs", get(PID::delayTime));
    fx->setProperty("delayFeedback", get(PID::delayFeedback));
    fx->setProperty("delayMix", get(PID::delayMix));
    fx->setProperty("delayDamp", get(PID::delayDamp));
    fx->setProperty("delayClockMode",
                    clockModeToString(getInt(PID::delayClockMode)));
    fx->setProperty("delayClockDivision",
                    clockDivisionToString(getInt(PID::delayClockDivision)));
    fx->setProperty("distOn", get(PID::fxDistOn) > 0.5f);
    fx->setProperty("chorusOn", get(PID::fxChorusOn) > 0.5f);
    fx->setProperty("phaserOn", get(PID::fxPhaserOn) > 0.5f);
    fx->setProperty("tremOn", get(PID::fxTremOn) > 0.5f);
    fx->setProperty("distDrive", get(PID::fxDistDrive));
    fx->setProperty("distMix", get(PID::fxDistMix));
    fx->setProperty("tremRate", get(PID::fxTremRate));
    fx->setProperty("tremAmt", get(PID::fxTremDepth));
    fx->setProperty("tremStereo", get(PID::fxTremStereo));
    fx->setProperty("tremWave",
                    choiceToKey(getInt(PID::fxTremWave), TremWave::kEntries));
    fx->setProperty("chorusRate", get(PID::fxChorusRate));
    fx->setProperty("chorusAmt", get(PID::fxChorusDepth));
    fx->setProperty("chorusMix", get(PID::fxChorusMix));
    fx->setProperty("phaserRate", get(PID::fxPhaserRate));
    fx->setProperty("phaserAmt", get(PID::fxPhaserDepth));
    fx->setProperty("phaserFeedback", get(PID::fxPhaserFeedback));
    fx->setProperty("phaserMix", get(PID::fxPhaserMix));
    fx->setProperty("reverbType", choiceToKey(getInt(PID::reverbType), ReverbType::kEntries));
    fx->setProperty("reverbMix", get(PID::reverbMix));
    fx->setProperty("algoRoom", get(PID::algoRoom));
    fx->setProperty("algoDamping", get(PID::algoDamping));
    fx->setProperty("algoWidth", get(PID::algoWidth));
    // Limiter
    fx->setProperty("limiterThreshold", get(PID::limiterThresh));
    fx->setProperty("limiterRelease", get(PID::limiterRelease));
    root->setProperty("effects", fx.get());

    // Filter — store NORMALIZED cutoff (0-1), not Hz
    juce::DynamicObject::Ptr filt = new juce::DynamicObject();
    int ftRaw = getInt(PID::filterType);
    filt->setProperty("enabled", ftRaw > 0);
    filt->setProperty("type", filterTypeToString(ftRaw));
    filt->setProperty("slope", filterSlopeToString(getInt(PID::filterSlope)));
    filt->setProperty("cutoff", cutoffHzToNorm(get(PID::filterCutoff)));
    filt->setProperty("resonance", get(PID::filterResonance));
    filt->setProperty("mix", get(PID::filterMix));
    filt->setProperty("kbdTrack", get(PID::filterKbdTrack));
    filt->setProperty("drive", get(PID::filterDrive));
    filt->setProperty("driveOs", filterDriveOsToString(getInt(PID::filterDriveOs)));
    filt->setProperty("algorithm", filterAlgorithmToString(getInt(PID::filterAlgorithm)));
    filt->setProperty("warpStyle", filterWarpStyleToString(getInt(PID::filterWarpStyle)));
    root->setProperty("filter", filt.get());

    // Sequencer
    juce::DynamicObject::Ptr seq = new juce::DynamicObject();
    seq->setProperty("enabled", get(PID::seqRunning) > 0.5f);
    seq->setProperty("bpm", get(PID::seqBpm));
    int stepCount = getInt(PID::seqSteps);
    seq->setProperty("stepCount", stepCount);
    juce::Array<juce::var> stepArr;
    for (int i = 0; i < stepCount; ++i)
    {
        const auto& step = stepSequencer.getStep(i);
        juce::DynamicObject::Ptr s = new juce::DynamicObject();
        s->setProperty("active", step.enabled);
        s->setProperty("semitone", step.note - 60); // MIDI → semitone offset from C3
        s->setProperty("velocity", static_cast<double>(step.velocity));
        s->setProperty("gate", static_cast<double>(step.gate));
        // bindMode is authoritative (0=off,1=bind,2=glide); keep the legacy "bind"
        // bool so older builds still load these presets as instant binds.
        s->setProperty("bindMode", static_cast<int>(step.bindMode));
        s->setProperty("bind", step.bindMode != T5ynthStepSequencer::BindMode::Off);
        juce::Array<juce::var> oneShots;
        for (int slot = 0; slot < T5ynthStepSequencer::ONE_SHOT_SLOTS; ++slot)
        {
            juce::DynamicObject::Ptr shot = new juce::DynamicObject();
            shot->setProperty("mode", static_cast<int>(step.oneShotModes[static_cast<size_t>(slot)]));
            shot->setProperty("hasSample", hasSequencerOneShotSample(i, slot));
            oneShots.add(shot.get());
        }
        s->setProperty("oneShots", oneShots);
        stepArr.add(s.get());
    }
    seq->setProperty("steps", stepArr);
    seq->setProperty("octaveShift", choiceToKey(getInt(PID::seqOctave), SeqOctave::kEntries));
    seq->setProperty("division", choiceToKey(getInt(PID::seqDivision), SeqDivision::kEntries));
    seq->setProperty("glideTime", get(PID::seqGlideTime));
    seq->setProperty("gate", get(PID::seqGate));
    seq->setProperty("shuffle", get(PID::seqShuffle));
    seq->setProperty("scaleRoot", choiceToKey(getInt(PID::scaleRoot), ScaleRoot::kEntries));
    seq->setProperty("scaleType", choiceToKey(getInt(PID::scaleType), ScaleType::kEntries));
    root->setProperty("sequencer", seq.get());

    // Arpeggiator — new v3 format stores pattern as a single key
    // (ArpMode::Off is "off", replacing the old `enabled` bool + pattern).
    juce::DynamicObject::Ptr arp = new juce::DynamicObject();
    arp->setProperty("pattern", choiceToKey(getInt(PID::arpMode), ArpMode::kEntries));
    arp->setProperty("rate", choiceToKey(getInt(PID::arpRate), ArpRate::kEntries));
    arp->setProperty("octaveRange", getInt(PID::arpOctaves));
    root->setProperty("arpeggiator", arp.get());

    // Generative sequencer
    juce::DynamicObject::Ptr genSeq = new juce::DynamicObject();
    genSeq->setProperty("enabled", get(PID::genSeqRunning) > 0.5f);
    genSeq->setProperty("steps", getInt(PID::genSteps));
    genSeq->setProperty("pulses", getInt(PID::genPulses));
    genSeq->setProperty("rotation", getInt(PID::genRotation));
    genSeq->setProperty("mutation", get(PID::genMutation));
    genSeq->setProperty("range", choiceToKey(getInt(PID::genRange), GenRange::kEntries));
    genSeq->setProperty("fixSteps",    get(PID::genFixSteps) > 0.5f);
    genSeq->setProperty("fixPulses",   get(PID::genFixPulses) > 0.5f);
    genSeq->setProperty("fixRotation", get(PID::genFixRotation) > 0.5f);
    genSeq->setProperty("fixMutation", get(PID::genFixMutation) > 0.5f);

    // Inter-strand coordination (added 2026-07-16, was never persisted before)
    genSeq->setProperty("coordination",
                        choiceToKey(getInt(PID::genCoordinationMode),
                                    CoordinationMode::kEntries));
    genSeq->setProperty("coordinationCap", getInt(PID::genCoordinationCap));

    // Shared pitch field
    juce::DynamicObject::Ptr field = new juce::DynamicObject();
    field->setProperty("mode",     choiceToKey(getInt(PID::genFieldMode),  FieldMode::kEntries));
    field->setProperty("rate",     getInt(PID::genFieldRate));
    field->setProperty("centerPc", getInt(PID::genFieldCenterPc));
    field->setProperty("pivot",    choiceToKey(getInt(PID::genFieldPivot), FieldPivot::kEntries));
    genSeq->setProperty("pitchField", field.get());

    // Strand 0 extras (Euclidean params already serialised above under top-level keys)
    juce::DynamicObject::Ptr strand0 = new juce::DynamicObject();
    strand0->setProperty("role",      choiceToKey(getInt(PID::genRole),    StrandRole::kEntries));
    strand0->setProperty("octave",    getInt(PID::genOctave));
    strand0->setProperty("divMult",   choiceToKey(getInt(PID::genDivMult), StrandDivMult::kEntries));
    strand0->setProperty("dominance", get(PID::genDominance));
    genSeq->setProperty("strand0", strand0.get());

    // Strands 2..5 full state
    struct StrandIds {
        const char* enable;  const char* role;    const char* octave;  const char* divMult;
        const char* dominance; const char* steps; const char* pulses;  const char* rotation;
        const char* mutation; const char* fS;     const char* fP;      const char* fR;      const char* fM;
    };
    static const StrandIds kExtras[4] = {
        { PID::gen2Enable, PID::gen2Role, PID::gen2Octave, PID::gen2DivMult,
          PID::gen2Dominance, PID::gen2Steps, PID::gen2Pulses, PID::gen2Rotation,
          PID::gen2Mutation, PID::gen2FixSteps, PID::gen2FixPulses, PID::gen2FixRotation, PID::gen2FixMutation },
        { PID::gen3Enable, PID::gen3Role, PID::gen3Octave, PID::gen3DivMult,
          PID::gen3Dominance, PID::gen3Steps, PID::gen3Pulses, PID::gen3Rotation,
          PID::gen3Mutation, PID::gen3FixSteps, PID::gen3FixPulses, PID::gen3FixRotation, PID::gen3FixMutation },
        { PID::gen4Enable, PID::gen4Role, PID::gen4Octave, PID::gen4DivMult,
          PID::gen4Dominance, PID::gen4Steps, PID::gen4Pulses, PID::gen4Rotation,
          PID::gen4Mutation, PID::gen4FixSteps, PID::gen4FixPulses, PID::gen4FixRotation, PID::gen4FixMutation },
        { PID::gen5Enable, PID::gen5Role, PID::gen5Octave, PID::gen5DivMult,
          PID::gen5Dominance, PID::gen5Steps, PID::gen5Pulses, PID::gen5Rotation,
          PID::gen5Mutation, PID::gen5FixSteps, PID::gen5FixPulses, PID::gen5FixRotation, PID::gen5FixMutation }
    };
    static const char* kExtraKeys[4] = { "strand2", "strand3", "strand4", "strand5" };
    for (int i = 0; i < 4; ++i)
    {
        const auto& ids = kExtras[i];
        juce::DynamicObject::Ptr sn = new juce::DynamicObject();
        sn->setProperty("enabled",     get(ids.enable) > 0.5f);
        sn->setProperty("role",        choiceToKey(getInt(ids.role),    StrandRole::kEntries));
        sn->setProperty("octave",      getInt(ids.octave));
        sn->setProperty("divMult",     choiceToKey(getInt(ids.divMult), StrandDivMult::kEntries));
        sn->setProperty("dominance",   get(ids.dominance));
        sn->setProperty("steps",       getInt(ids.steps));
        sn->setProperty("pulses",      getInt(ids.pulses));
        sn->setProperty("rotation",    getInt(ids.rotation));
        sn->setProperty("mutation",    get(ids.mutation));
        sn->setProperty("fixSteps",    get(ids.fS) > 0.5f);
        sn->setProperty("fixPulses",   get(ids.fP) > 0.5f);
        sn->setProperty("fixRotation", get(ids.fR) > 0.5f);
        sn->setProperty("fixMutation", get(ids.fM) > 0.5f);
        genSeq->setProperty(kExtraKeys[i], sn.get());
    }

    root->setProperty("generativeSeq", genSeq.get());

    // CC bindings — sparse: only write entries that have a param assigned.
    // Old presets that lack this key default to no bindings on load.
    juce::Array<juce::var> ccArr;
    {
        const juce::SpinLock::ScopedLockType lock(ccMappingLock_);
        for (int cc = 0; cc < 128; ++cc)
        {
            const auto& m = ccMappings_[static_cast<size_t>(cc)];
            if (m.paramId.isEmpty()) continue;
            juce::DynamicObject::Ptr entry = new juce::DynamicObject();
            entry->setProperty("cc",    cc);
            entry->setProperty("param", m.paramId);
            entry->setProperty("min",   static_cast<double>(m.minNorm));
            entry->setProperty("max",   static_cast<double>(m.maxNorm));
            ccArr.add(entry.get());
        }
    }
    root->setProperty("ccMappings", ccArr);

    return juce::JSON::toString(root.get(), true);
}

bool T5ynthProcessor::importJsonPreset(const juce::String& json)
{
    // Suppresses the per-param ParamEvent flood every setParam() call below would
    // otherwise generate; any early return below (parse failure) cancels silently
    // via the guard's destructor, no marker logged for a load that never happened.
    BulkParamLoadGuard eventLogGuard(*this);

    auto parsed = juce::JSON::parse(json);
    if (!parsed.isObject()) return false;

    auto* root = parsed.getDynamicObject();
    if (!root) return false;
    const bool importingSequencePattern = root->getProperty("kind").toString() == "t5seq";

    // Calibration epoch the file was authored under (ABSENT = 0 = pre-calibration).
    // Stored values authored under older DSP full-scales are rescaled as they are
    // read (Calibration::migrateScalar) so the preset sounds identical.
    int fileCalibEpoch = 0;
    if (auto* synth = root->getProperty("synth").getDynamicObject())
        fileCalibEpoch = synth->hasProperty("calibEpoch")
                             ? static_cast<int>(synth->getProperty("calibEpoch")) : 0;

    // ── Synth params ──
    if (auto* synth = root->getProperty("synth").getDynamicObject())
    {
        setParamFromJson(parameters, PID::genAlpha, synth, "alpha");
        // Magnitude scales the conditioning the whole oscillator exists to
        // drive; its default is 1.0, and an absent key used to land on 0 —
        // i.e. no conditioning at all, the model's own prior.
        setParamFromJson(parameters, PID::genMagnitude, synth, "magnitude");
        setParamFromJson(parameters, PID::genNoise, synth, "noise");
        if (synth->hasProperty("axesAmount"))
            setParam(parameters, PID::genAxesAmount, static_cast<float>(synth->getProperty("axesAmount")));
        // resynth's layout default is 0 (off), so absence lands on off either
        // way: a preset saved before Resynth existed resets the slider to off
        // on load, as a preset's full state should.
        setParamFromJson(parameters, PID::resynthAmount, synth, "resynth");
        // velAmt default is 1.0 (full velocity→peak). A preset saved before VEL AMT
        // existed lacks the property; treat absence as 1.0 so old patches regain full
        // velocity response (the chosen "1.0 global"), not silence-on-soft-notes 0.
        setParam(parameters, PID::velAmt,
                 synth->hasProperty("velAmt") ? static_cast<float>(synth->getProperty("velAmt")) : 1.0f);
        // Re-Prompt: restore BOTH the stance and the coupling — a preset is a full
        // patch, and a self-listening "machine" preset (a stance + a non-Manual
        // cadence + Resynth) cannot reproduce without the stance that drives the
        // prompt-rewriting loop. Forcing the stance Off here was the "Re-Prompt mode
        // is not saved" bug. Safety is preserved by CADENCE, not by nuking the stance:
        // Manual cadence never auto-runs a restored stance (pollDriftRegen early-outs
        // in Manual — the old "fires even in Manual" claim was fixed); a non-Manual
        // cadence DOES resume the loop on load, which is the whole point of loading
        // such a patch (and mirrors the Resynth loop, which already resumes on load).
        // loadPresetData resets the per-session loop runtime (loopEngaged_/seed) so the
        // restored stance re-captures the just-loaded prompts on its first step. The
        // DAW host-state path (setStateInformation) STILL forces the stance Off — a
        // host re-opening a project is a passive load that must not render unbidden.
        // An older .t5p missing the keys -> the layout default, which for all
        // three is index 0 -> Off / B-only / Internal, restoring the original
        // behaviour so old presets load correctly.
        setChoiceParamFromJson(parameters, PID::repromptStance, synth, "repromptStance",
                               RepromptStance::kEntries);
        setChoiceParamFromJson(parameters, PID::repromptCoupling, synth, "repromptCoupling",
                               RepromptCoupling::kEntries);
        setChoiceParamFromJson(parameters, PID::resynthSource, synth, "resynthSource",
                               ResynthSource::kEntries);
        // duration (3 s), steps (8), cfg (1.0) and seed all have non-zero
        // defaults; absence used to mean a 0-second, 0-step generation.
        setParamFromJson(parameters, PID::genDuration, synth, "duration");
        setParamFromJson(parameters, PID::genStart, synth, "startPosition");
        setParamFromJson(parameters, PID::infSteps, synth, "steps",
                         [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); });
        setParamFromJson(parameters, PID::genCfg, synth, "cfg");
        setParamFromJson(parameters, PID::genSeed, synth, "seed",
                         [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); });
        if (synth->hasProperty("hfBoost"))
            setParam(parameters, PID::genHfBoost, static_cast<bool>(synth->getProperty("hfBoost")) ? 1.0f : 0.0f);
        // Modality epoch (v2.5.0+): which TrackType-routing behaviour this preset was
        // authored under. ABSENT = legacy (pre-2.5.0) -> the backend keeps the old
        // Music/SFX-only prefixes. (A partial .t5seq has no synth block, so this whole
        // reader is skipped and the live epoch is left untouched.)
        currentModalityEpoch_ = synth->hasProperty("modalityEpoch")
            ? static_cast<int>(synth->getProperty("modalityEpoch"))
            : kLegacyModalityEpoch;
    }

    // ── Engine ──
    if (auto* engine = root->getProperty("engine").getDynamicObject())
    {
        // A preset is a different patch: what the last authoring borrowed from
        // the PREVIOUS one is no longer anybody's to give back, and flipping the
        // switch on this patch must not fetch it either. Every engine, not only
        // a Csound one — a neural preset replaces the patch just as completely.
        // The record is DROPPED rather than released: the file's own values are
        // about to be written over every one of these knobs anyway, and putting
        // the previous patch's values back first would only be a detour.
        dropAuthorSettings();
        setChoiceParamFromJson(parameters, PID::engineMode, engine, "mode", EngineMode::kEntries);
        // Old .t5p files predate voiceCount / tuning being saved; guard
        // with hasProperty so they keep loading with their previous
        // (now-default) polyphony and tuning instead of being rejected.
        if (engine->hasProperty("voiceCount"))
            setParam(parameters, PID::voiceCount,
                     static_cast<float>(choiceFromKey(engine->getProperty("voiceCount").toString(), VoiceCount::kEntries)));
        // KNOBS (`lcoSetsParams`) is deliberately NOT read, and no longer
        // written — see the matching note in exportJsonPreset. It is the
        // player's standing permission for the NEXT authoring, not a property of
        // this sound, and it stays where they left it no matter what a file
        // says. Presets written in the few hours the field existed still carry
        // it; it is ignored. What this replaces, and why it matters: the field
        // used to be read like every sound parameter, absent-means-default, and
        // since not one shipped preset carried it (the switch was a day newer
        // than all of them) every single preset load silently revoked the
        // permission. Measured 2026-07-29 — BJ switched it on, loaded a preset,
        // and the next authoring went out with no shelf at all, which is why the
        // whole feature looked broken.
        if (engine->hasProperty("tuning"))
            setParam(parameters, PID::tuning,
                     static_cast<float>(choiceFromKey(engine->getProperty("tuning").toString(), TuningType::kEntries)));
        setChoiceParamFromJson(parameters, PID::loopMode, engine, "loopMode", LoopMode::kEntries);

        // Restore P1/P2/P3 directly — the explicit pointsLocked flag gates
        // auto-bracketing in loadGeneratedAudio so no pending-apply dance is
        // needed. Older v3 presets without the flag default to unlocked.
        // The three markers are not APVTS parameters, so their fallbacks are
        // SamplePlayer's own declared values — P2 = 0, P3 = 1, P1 = 0, i.e. the
        // whole buffer. Notably P3: an absent loopEndFrac used to ask for 0,
        // which the pair clamp opens to loopStart + 0.01, a sliver one
        // hundredth of the buffer wide.
        //
        // Measured 2026-08-05, before the pair went in as a pair: 10 of the 46
        // presets in the shipped bank and 31 of the 148 on this machine loaded
        // a different window depending on what came before them, most with
        // pointsLocked set, so loadGeneratedAudio never re-bracketed and
        // corrected it. Guard: tools/test_preset_loop_window_order.cpp.
        //
        // Every value is read BEFORE the lock is taken. Each hasProperty() and
        // getProperty() builds a juce::Identifier from a char literal, which
        // takes the global StringPool's CriticalSection and may allocate, and
        // this lock is the one the audio callback needs — same rule the CC
        // block at the end of this function states for getParameter().
        const float loopStartFrac  = jsonFloatOr(engine, "loopStartFrac", 0.0f);
        const float loopEndFrac    = jsonFloatOr(engine, "loopEndFrac", 1.0f);
        const float startPosFrac   = jsonFloatOr(engine, "startPosFrac", 0.0f);
        const bool  haveWtExtract  = engine->hasProperty("wtExtractStart");
        const float wtExtractStart = jsonFloatOr(engine, "wtExtractStart", 0.0f);
        const float wtExtractEnd   = jsonFloatOr(engine, "wtExtractEnd", 1.0f);
        const bool  pointsLocked   = static_cast<bool>(engine->getProperty("pointsLocked"));
        {
            const juce::ScopedLock sl (getCallbackLock());
            // As a PAIR: setLoopStart clamps against whatever loop END is live,
            // so a preset whose region sits entirely to the right of the one
            // currently loaded had its start silently pulled back to the old
            // end — the region a preset played then depended on the preset
            // before it.
            masterSampler.setLoopRegion(loopStartFrac, loopEndFrac);
            masterSampler.setStartPos(startPosFrac);
            // WT extraction region (fallback to P2/P3 for presets without it).
            // Read BACK from the sampler, not from the locals above: a pair
            // that is degenerate in the FILE (end below start + 1%) still gets
            // opened up, so the pair that landed is not always the pair named.
            if (haveWtExtract)
            {
                masterSampler.setWtExtractStart(wtExtractStart);
                masterSampler.setWtExtractEnd(wtExtractEnd);
            }
            else
            {
                masterSampler.setWtExtractStart(masterSampler.getLoopStart());
                masterSampler.setWtExtractEnd(masterSampler.getLoopEnd());
            }
            masterSampler.setPointsLocked(pointsLocked);
        }
        // crossfadeMs defaults to 150 and normalize to ON; absence used to mean
        // a 0 ms loop crossfade and normalisation silently switched off.
        setParamFromJson(parameters, PID::crossfadeMs, engine, "crossfadeMs");
        setBoolParamFromJson(parameters, PID::normalize, engine, PID::normalize);
        setChoiceParamFromJson(parameters, PID::loopOptimize, engine, "loopOptimize",
                               LoopOptimize::kEntries);

        // Csound orchestra (Phase 5, SPEC_phase4_5_csound_llm_preset.md).
        // Ordering: the engineMode setParam() just above already fired
        // parameterChanged() SYNCHRONOUSLY (setValueNotifyingHost is
        // synchronous) when the file was saved in Csound mode, which flags
        // csoundWantsPrepare_ + triggerAsyncUpdate() — but that only queues
        // handleAsyncUpdate, it does not run it inline. requestCsoundOrchestra()
        // below queues its OWN swap request (bumping
        // csoundSwapRequestGeneration_) regardless of engine mode — it does not
        // gate on PID::engineMode at all, only handleAsyncUpdate's bootstrap
        // branch does — so calling it here, before handleAsyncUpdate has ever
        // run for this load, is safe and race-free: by the time
        // handleAsyncUpdate finally runs (message thread, shortly after this
        // synchronous import returns), the bootstrap guard added in Phase 5
        // (csoundSwapRequestGeneration_ != csoundSwapStartedGeneration_) sees
        // this request already queued and skips the built-in bootstrap
        // entirely, going straight to the swap-compile path. Since the active
        // Csound engine slot is virtually never already isReady() at this
        // point (a fresh instance, or an instance that was in a different
        // engine mode before this load), that path's instant-adopt branch
        // compiles the restored orchestra directly into the live slot with no
        // audible fade — exactly the "no ready active engine -> instant adopt"
        // case the spec calls for. A preset saved in a NON-Csound mode omits
        // both properties (see exportJsonPreset), so hasProperty guards the
        // whole thing off for every other engine.
        if (engine->hasProperty("csound_orchestra"))
        {
            requestCsoundOrchestra(engine->getProperty("csound_orchestra").toString());
            setCsoundPrompt(engine->hasProperty("csound_prompt")
                                 ? engine->getProperty("csound_prompt").toString()
                                 : juce::String());
            setCsoundReading(engine->hasProperty("csound_reading")
                                 ? engine->getProperty("csound_reading").toString()
                                 : juce::String());
            setCsoundParamsText(engine->hasProperty("csound_params_text")
                                 ? engine->getProperty("csound_params_text").toString()
                                 : juce::String());
            // WHERE the file says its knobs stand decides which source is right,
            // and the two blocks are not one contract: `csound_controls` is what
            // each knob MEANS and has been written since the knobs existed;
            // `csound_knob_values` is where the player left them and is new. A
            // file with both is the player's patch and the values below are the
            // truth. A file with only the names — every LRO preset written
            // before this — has its positions nowhere else, so the AUTHOR's
            // starting values, which travel inside `csound_controls`, are the
            // truth, and `applyValues` fetches them. Reading neither would leave
            // the twelve wherever the previous patch left them and play the
            // saved instrument at a stranger's settings.
            const bool haveValues = engine->hasProperty("csound_knob_values");
            setCsoundControls(LroControls::fromVar(
                engine->getProperty("csound_controls")), /*applyValues=*/! haveValues);
            if (auto* kv = engine->getProperty("csound_knob_values").getDynamicObject())
            {
                auto restore = [&](const char* id)
                {
                    const auto v = kv->getProperty(id);
                    if (! v.isVoid())
                        setParam(parameters, id, static_cast<float>(static_cast<double>(v)));
                };
                for (const auto* id : kLroKnobIds)  restore(id);
                for (const auto* id : kLroLevelIds) restore(id);
            }
            else
            {
                // The layer levels are the one thing `setCsoundControls` cannot
                // supply, because no author ever declared them — they are the
                // player's mix. A file that does not carry them was saved when
                // nothing did, so unity is the state it was saved in. Without
                // this they keep the previous patch's positions and silently
                // scale a loaded preset — a Level left at 0.2 is 14 dB down on
                // an instrument whose panel, for the same old file, shows no
                // fader to find it with.
                for (const auto* id : kLroLevelIds)
                    setParamToLayoutDefault(parameters, id);
            }
        }
    }

    // ── Modulation ──
    // What the file carries, so the slots it does NOT carry can be defaulted
    // below. importJsonPreset does not reset the APVTS first, so anything left
    // unwritten keeps the PREVIOUSLY loaded patch's routing — a modulation slot
    // silently driving a preset that never asked for it. The gates below are
    // outside the "modulation" / "driftLfos" tests on purpose: a file missing
    // the whole block must clear those slots too, not inherit them.
    // Per slot, not a count: a file may carry a malformed entry in the middle of
    // an otherwise good array, and that slot must be defaulted like a missing
    // one rather than counted as carried.
    bool envWritten[kNumEnvPIDs] = {};
    bool lfoWritten[kNumLfoPIDs] = {};
    bool driftWritten[kNumDriftPIDs] = {};
    bool fileHasAftertouch = false;
    if (auto* mod = root->getProperty("modulation").getDynamicObject())
    {
        auto* envsArr = mod->getProperty("envs").getArray();
        if (envsArr)
        {
            for (int i = 0; i < std::min(kNumEnvPIDs, envsArr->size()); ++i)
            {
                auto* env = (*envsArr)[i].getDynamicObject();
                if (!env) continue;   // malformed entry: leave it to the fill below
                envWritten[i] = true;
                const auto& ep = kEnvPIDs[i];
                // Resolve the env's target up front: the cutoff-bus migration
                // rescales a filter-targeted env's amount (target-conditional), and
                // the same value sets the target param below.
                const int envTarget = env->hasProperty("target")
                    ? envTargetFromString(env->getProperty("target").toString())
                    : (i == 0 ? EnvTarget::DCA : EnvTarget::None);
                // An entry that carries SOME stage keys and not others is a
                // half-written envelope, not a zeroed one: D/S/R and Amt all
                // have non-zero defaults (amp 200/0.1/180, mod 2500/0.1/4000,
                // Amt 1.0 on every envelope), and Amt 0 on the amp envelope is
                // a patch that makes no sound.
                setParamFromJson(parameters, ep.attack, env, "attackMs");
                setParamFromJson(parameters, ep.decay, env, "decayMs");
                setParamFromJson(parameters, ep.sustain, env, "sustain");
                setParamFromJson(parameters, ep.release, env, "releaseMs");
                setParamFromJson(parameters, ep.amount, env, "amount",
                                 [&](const juce::var& v) {
                                     return Calibration::migrateScalarCond(ep.amount,
                                         static_cast<float>(v), fileCalibEpoch, envTarget);
                                 });
                setBoolParamFromJson(parameters, ep.loop, env, "loop");
                // Velocity sensitivity = signed per-stage A/D/R TIME only.
                // Any "sustainVelSens" from older presets (velocity→peak) is
                // intentionally ignored — peak is Amt's job now. Legacy format:
                // a single "velSens" (0..1) + per-stage A/D/R vel *modes*
                // (off/+/-) → A/D/R velSens = sign(mode) * velSens (sustain dropped).
                if (env->hasProperty("attackVelSens"))
                {
                    setParam(parameters, ep.attackVelSens,  static_cast<float>(env->getProperty("attackVelSens")));
                    setParam(parameters, ep.decayVelSens,   static_cast<float>(env->getProperty("decayVelSens")));
                    setParam(parameters, ep.releaseVelSens, static_cast<float>(env->getProperty("releaseVelSens")));
                }
                else if (env->hasProperty("velSens"))
                {
                    const float legacyVs = static_cast<float>(env->getProperty("velSens"));
                    auto signFromMode = [](int m) -> float {
                        return m == EnvVelTimeMode::Positive ?  1.0f
                             : m == EnvVelTimeMode::Negative ? -1.0f : 0.0f;
                    };
                    const int aMode = env->hasProperty("attackVelMode")
                        ? envVelTimeModeFromString(env->getProperty("attackVelMode").toString()) : EnvVelTimeMode::Off;
                    const int dMode = env->hasProperty("decayVelMode")
                        ? envVelTimeModeFromString(env->getProperty("decayVelMode").toString()) : EnvVelTimeMode::Off;
                    const int rMode = env->hasProperty("releaseVelMode")
                        ? envVelTimeModeFromString(env->getProperty("releaseVelMode").toString()) : EnvVelTimeMode::Off;
                    setParam(parameters, ep.attackVelSens,  signFromMode(aMode) * legacyVs);
                    setParam(parameters, ep.decayVelSens,   signFromMode(dMode) * legacyVs);
                    setParam(parameters, ep.releaseVelSens, signFromMode(rMode) * legacyVs);
                }
                // Exact bend if the file carries one, else the named anchor the
                // key stands for. A file written before the bend existed only
                // ever held one of the five anchors, so the key IS the exact
                // value there — no epoch needed on this surface.
                auto applyBend = [&](const char* pid, const char* bendKey, const char* curveKey)
                {
                    if (env->hasProperty(bendKey))
                        setParam(parameters, pid, static_cast<float>((double) env->getProperty(bendKey)));
                    else if (env->hasProperty(curveKey))
                        setParam(parameters, pid,
                                 curveBendFromString(env->getProperty(curveKey).toString()));
                };
                applyBend(ep.attackCurve,  "attackBend",  "attackCurve");
                applyBend(ep.decayCurve,   "decayBend",   "decayCurve");
                applyBend(ep.releaseCurve, "releaseBend", "releaseCurve");
                setParam(parameters, ep.target, static_cast<float>(envTarget));
            }
        }

        auto* lfosArr = mod->getProperty("lfos").getArray();
        if (lfosArr)
        {
            for (int i = 0; i < std::min(kNumLfoPIDs, lfosArr->size()); ++i)
            {
                auto* lfo = (*lfosArr)[i].getDynamicObject();
                if (!lfo) continue;   // malformed entry: leave it to the fill below
                lfoWritten[i] = true;
                const auto& lp = kLfoPIDs[i];
                // Resolved once: the depth migration is target-conditional, and
                // it has to see the SAME target the target parameter gets. When
                // the key is absent that is the layout's default, not index 0 —
                // the two agree for all three LFOs today, and would part company
                // the moment a target default moved.
                const int lfoTarget = lfo->hasProperty("target")
                    ? lfoTargetFromString(lfo->getProperty("target").toString())
                    : layoutDefaultIntOf(parameters, lp.target);
                // Rate defaults differ per LFO (2.0 / 0.5 / 0.2 Hz) and the
                // LFO 2 waveform default is triangle, not the first entry —
                // absence has to reach the layout, not index 0 / 0 Hz.
                setParamFromJson(parameters, lp.rate, lfo, "rate");
                setParamFromJson(parameters, lp.depth, lfo, "depth",
                                 [&](const juce::var& v) {
                                     return Calibration::migrateScalarCond(lp.depth,
                                         static_cast<float>(v), fileCalibEpoch, lfoTarget);
                                 });
                setChoiceParamFromJson(parameters, lp.wave, lfo, "waveform", lfoWaveFromString);
                setChoiceParamFromJson(parameters, lp.target, lfo, "target", lfoTargetFromString);
                setChoiceParamFromJson(parameters, lp.mode, lfo, "mode", lfoModeFromString);
                // Pre-v1.7 presets have no clock fields — default to Off / 1/4
                // explicitly so the previous session's clock state cannot stick.
                setParam(parameters, lp.clockMode, lfo->hasProperty("clockMode")
                    ? static_cast<float>(clockModeFromString(lfo->getProperty("clockMode").toString()))
                    : static_cast<float>(ClockMode::Off));
                setParam(parameters, lp.clockDivision, lfo->hasProperty("clockDivision")
                    ? static_cast<float>(clockDivisionFromString(lfo->getProperty("clockDivision").toString()))
                    : static_cast<float>(ClockDivision::D1_4));
            }
        }

        // MIDI aftertouch routing. New format: one bipolar amount per target
        // keyed by target key. Legacy format (single-select target + global
        // amount) is migrated onto the selected target's per-target amount.
        // hasProperty-gated so older .t5p files keep loading at their previous
        // routing instead of being rejected.
        if (auto* at = mod->getProperty("aftertouch").getDynamicObject())
        {
            fileHasAftertouch = true;
            // The file's aftertouch block fully determines aftertouch routing:
            // a target the file does NOT mention is set to 0, never left where
            // the previously loaded preset put it. Without that, a target added
            // after a preset was written (ENV4/5 Sustain) survives every load of
            // the entire shipped bank, silently modulating the next patch.
            bool perTarget = false;
            for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
                if (at->hasProperty(AftertouchTarget::kEntries[t].key)) { perTarget = true; break; }
            if (perTarget)
                for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
                    setParam(parameters, kAftertouchAmtPid[t],
                             at->hasProperty(AftertouchTarget::kEntries[t].key)
                                 ? Calibration::migrateScalar(kAftertouchAmtPid[t],
                                       static_cast<float>(at->getProperty(AftertouchTarget::kEntries[t].key)),
                                       fileCalibEpoch)
                                 : 0.0f);
            if (! perTarget)
            {
                // Legacy single-select: clear every target FIRST, then migrate
                // the one the file names. Without the clear, a legacy block
                // (20 of the shipped presets carry target "none") would set
                // fileHasAftertouch and then write nothing, so the previous
                // patch's pressure routing would survive the load — exactly
                // what the per-target branch above exists to prevent.
                for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
                    setParam(parameters, kAftertouchAmtPid[t], 0.0f);

                const int t = choiceFromKey(at->getProperty("target").toString(), AftertouchTarget::kEntries);
                if (at->hasProperty("target")
                    && t >= AftertouchTarget::LFO1Depth && t <= AftertouchTarget::NoiseLevel)
                    setParam(parameters, kAftertouchAmtPid[t],
                             Calibration::migrateScalar(kAftertouchAmtPid[t],
                                 at->hasProperty("amount") ? static_cast<float>(at->getProperty("amount")) : 0.0f,
                                 fileCalibEpoch));
            }

            // The axis each of those amounts is driven by. Same rule as the
            // amounts: the file's block fully determines the routing, and a
            // target it does not name falls back rather than keeping what the
            // last preset left there.
            //
            // WHERE it falls back to depends on whether the file knew about
            // sources at all. A file with NO block predates them: every amount
            // in it was written to mean pressure, so pressure is what it gets --
            // ExprSource::kLegacy, not the fresh-patch default, which for most
            // rows is None and would silence a routing the file plainly meant.
            // A file that HAS the block and merely omits a key is hand-edited or
            // newer; there the row's own default is the honest answer.
            auto* es = mod->getProperty("exprSource").getDynamicObject();
            for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
            {
                int src = (es != nullptr) ? ExprSource::defaultFor(t)
                                          : ExprSource::kLegacy;
                if (es != nullptr && es->hasProperty(AftertouchTarget::kEntries[t].key))
                {
                    // choiceFromKey returns 0 for "no match" as well as for a
                    // real hit on entry 0, which every other table it serves can
                    // live with because entry 0 is their none/off hole. This
                    // table's entry 0 is VELOCITY — the one source that does not
                    // move during a note — so an unknown key (a newer file, a
                    // hand edit, a non-string value) would silently freeze that
                    // target. Read the key back to tell the two apart.
                    const juce::String key = es->getProperty(AftertouchTarget::kEntries[t].key).toString();
                    const int k = choiceFromKey(key, ExprSource::kEntries);
                    if (k >= 0 && k < ExprSource::kCount && key == ExprSource::kEntries[k].key)
                        src = k;
                }
                setParam(parameters, kExprSrcPid[t], static_cast<float>(src));
            }
        }
    }

    // A .t5seq carries only sequencer/arp/generative and is loaded ON TOP of the
    // live patch — for it, "the file does not carry a modulation slot" means
    // "not mine to touch", not "clear it". Same reasoning as the two existing
    // importingSequencePattern gates further down.
    if (! importingSequencePattern)
    {
        // Envelope 0 is the amp envelope and has different defaults; a file
        // missing even that one is broken, so it is left alone rather than reset.
        for (int i = 1; i < kNumEnvPIDs; ++i)
            if (! envWritten[i])
                for (const char* id : kEnvPIDs[i].all())
                    setParamToLayoutDefault(parameters, id);

        for (int i = 0; i < kNumLfoPIDs; ++i)
            if (! lfoWritten[i])
                for (const char* id : kLfoPIDs[i].all())
                    setParamToLayoutDefault(parameters, id);

        if (! fileHasAftertouch)
            for (int t = AftertouchTarget::LFO1Depth; t < AftertouchTarget::kCount; ++t)
            {
                setParam(parameters, kAftertouchAmtPid[t], 0.0f);
                setParam(parameters, kExprSrcPid[t], static_cast<float>(ExprSource::defaultFor(t)));
            }
    }

    // ── Drift LFOs ──
    auto* driftArr = root->getProperty("driftLfos").getArray();
    if (driftArr)
    {
        for (int i = 0; i < std::min(kNumDriftPIDs, driftArr->size()); ++i)
        {
            auto* d = (*driftArr)[i].getDynamicObject();
            if (!d) continue;   // malformed entry: leave it to the fill below
            driftWritten[i] = true;
            const auto& dp = kDriftPIDs[i];
            // Same as the LFO block: the depth migration must see the target
            // the target parameter actually gets, layout default included.
            const int driftTarget = d->hasProperty("target")
                ? driftTargetFromString(d->getProperty("target").toString())
                : layoutDefaultIntOf(parameters, dp.target);
            setParamFromJson(parameters, dp.rate, d, "rate");   // default 0.25 Hz, not 0
            setParamFromJson(parameters, dp.depth, d, "depth",
                             [&](const juce::var& v) {
                                 return Calibration::migrateScalarCond(dp.depth,
                                     static_cast<float>(v), fileCalibEpoch, driftTarget);
                             });
            setChoiceParamFromJson(parameters, dp.wave, d, "waveform", driftWaveFromString);
            setChoiceParamFromJson(parameters, dp.target, d, "target", driftTargetFromString);
            setParam(parameters, dp.clockMode, d->hasProperty("clockMode")
                ? static_cast<float>(clockModeFromString(d->getProperty("clockMode").toString()))
                : static_cast<float>(ClockMode::Off));
            setParam(parameters, dp.clockDivision, d->hasProperty("clockDivision")
                ? static_cast<float>(driftDivisionFromString(d->getProperty("clockDivision").toString()))
                : static_cast<float>(DriftDivision::D2_1));
        }
    }
    if (! importingSequencePattern)
        for (int i = 0; i < kNumDriftPIDs; ++i)
            if (! driftWritten[i])
                for (const char* id : kDriftPIDs[i].all())
                    setParamToLayoutDefault(parameters, id);

    // Same law for the three loose drift params, and for the same reason: a key
    // the file does not carry means "layout default", never "the void var's
    // zero". Regen XFade is the one this actually bit — none of the presets
    // written before the exporter learned these keys carries driftCrossfade, so
    // every one of them was landing on 0 ms, and a 0 ms Regen XFade turns the
    // held-note follow into the hard swap the platform invariant forbids.
    if (! importingSequencePattern)
    {
        setBoolParamFromJson(parameters, PID::driftEnabled, root, "driftEnabled");
        setParamFromJson(parameters, PID::driftCrossfade, root, "driftCrossfade");
        setChoiceParamFromJson(parameters, PID::driftRegen, root, "regenMode", DriftRegen::kEntries);
        setBoolParamFromJson(parameters, PID::cacheAsync, root, "cacheAsync");
    }

    // ── Wavetable + Noise ──
    if (auto* wt = root->getProperty("wavetable").getDynamicObject())
    {
        setParamFromJson(parameters, PID::oscScan, wt, "scan");
        // Three of these have a default that is NOT the first entry: octave 0
        // is index 2 (index 0 is -2 octaves), 256 frames is index 3 (index 0
        // is 32), and smoothing is on. An absent key used to transpose the
        // oscillator two octaves down and drop it to the coarsest table.
        setChoiceParamFromJson(parameters, PID::oscOctave, wt, "octaveShift", OscOctave::kEntries);
        setParamFromJson(parameters, PID::noiseLevel, wt, "noiseLevel");
        setChoiceParamFromJson(parameters, PID::noiseType, wt, "noiseType", NoiseKind::kEntries);
        setChoiceParamFromJson(parameters, PID::wtFrames, wt, "frames", WtFrames::kEntries);
        setBoolParamFromJson(parameters, PID::wtSmooth, wt, "smooth");
        bool autoScan = wt->hasProperty("autoScan") ? static_cast<bool>(wt->getProperty("autoScan")) : true;
        setParam(parameters, PID::wtAutoScan, autoScan ? 1.0f : 0.0f);
    }
    // LCO/DCO bake metadata — the frame/station payload itself is restored by
    // PresetFormat::loadFromFile's caller (it alone has the FLAC blob cursor);
    // this only restores the text/balance/stance fields. Absence = not an LCO
    // preset, leaves the cached bake snapshot untouched (cleared already by
    // BulkParamLoadGuard's normal param flow, nothing else to do).
    if (auto* lco = root->getProperty("lco").getDynamicObject())
    {
        // The two gains are not APVTS parameters; their fallback is the
        // processor's own declared unity (PluginProcessor.h:676), because an
        // lco block without them would otherwise bake both oscillators silent.
        setLcoBakeSnapshot(lco->getProperty("prompt").toString(),
                           lco->getProperty("readingA").toString(),
                           lco->getProperty("readingB").toString(),
                           jsonFloatOr(lco, "motionRateHz", 0.0f),
                           static_cast<bool>(lco->getProperty("oscAHasContent")),
                           jsonFloatOr(lco, "gainA", 1.0f),
                           static_cast<bool>(lco->getProperty("oscBHasContent")),
                           jsonFloatOr(lco, "gainB", 1.0f));
        setChoiceParamFromJson(parameters, PID::dcoRepromptStance, lco, "repromptStance",
                               RepromptStance::kEntries);
    }
    if (auto* freeze = root->getProperty("freeze").getDynamicObject())
    {
        // Silk is index 1, so an absent texture used to select Hold.
        setChoiceParamFromJson(parameters, PID::freezeTexture, freeze, "texture",
                               FreezeTexture::kEntries);
        setParamFromJson(parameters, PID::freezeStereo, freeze, "stereo");
    }

    // ── Effects ──
    if (auto* fx = root->getProperty("effects").getDynamicObject())
    {
        // Back-compat: the retired 2-head "tape2" mode folds into the single
        // "Tape" (3-head, key "tape3") — remap so old presets don't fall back to
        // Off (choiceFromKey returns 0 for an unknown key).
        juce::String delayTypeKey = fx->getProperty("delayType").toString();
        if (delayTypeKey == "tape2") delayTypeKey = "tape3";
        // Absent still has to reach the layout rather than index 0 — the two
        // agree today, and the point of saying so here is that they need not.
        const int delayTypeIdx = fx->hasProperty("delayType")
            ? choiceFromKey(delayTypeKey, DelayType::kEntries)
            : layoutDefaultIntOf(parameters, PID::delayType);
        setParam(parameters, PID::delayType, static_cast<float>(delayTypeIdx));
        setParamFromJson(parameters, PID::delayTime, fx, "delayTimeMs");
        setParamFromJson(parameters, PID::delayFeedback, fx, "delayFeedback");
        // Epoch 5: the mix LAW changed, and how it changed depends on which delay
        // voicing the file selected — hence the type index rather than a factor.
        // Absent = the layout's 0.3, unmigrated: there is no stored value to
        // rescale, and a default is already expressed in today's law.
        setParamFromJson(parameters, PID::delayMix, fx, "delayMix",
                         [&](const juce::var& v) {
                             return Calibration::migrateMixScalar(PID::delayMix,
                                 static_cast<float>(v), fileCalibEpoch, delayTypeIdx);
                         });
        setParamFromJson(parameters, PID::delayDamp, fx, "delayDamp");
        setParam(parameters, PID::delayClockMode, fx->hasProperty("delayClockMode")
            ? static_cast<float>(clockModeFromString(fx->getProperty("delayClockMode").toString()))
            : static_cast<float>(ClockMode::Off));
        setParam(parameters, PID::delayClockDivision, fx->hasProperty("delayClockDivision")
            ? static_cast<float>(clockDivisionFromString(fx->getProperty("delayClockDivision").toString()))
            : static_cast<float>(ClockDivision::D1_4));
        const int reverbTypeIdx = fx->hasProperty("reverbType")
            ? choiceFromKey(fx->getProperty("reverbType").toString(), ReverbType::kEntries)
            : layoutDefaultIntOf(parameters, PID::reverbType);
        setParam(parameters, PID::reverbType, static_cast<float>(reverbTypeIdx));
        // Epoch 5: Algo and Plate had DIFFERENT old laws (squared vs linear) and
        // different old path gains, so the remap needs the reverb type.
        setParamFromJson(parameters, PID::reverbMix, fx, "reverbMix",
                         [&](const juce::var& v) {
                             return Calibration::migrateMixScalar(PID::reverbMix,
                                 static_cast<float>(v), fileCalibEpoch, reverbTypeIdx);
                         });
        setParamFromJson(parameters, PID::algoRoom, fx, "algoRoom");
        setParamFromJson(parameters, PID::algoDamping, fx, "algoDamping");
        setParamFromJson(parameters, PID::algoWidth, fx, "algoWidth");
        // The threshold's default is -3 dB and it now drives the static output
        // gain (outputGainForThreshold), so a file without the key used to load
        // 0 dB and play 3 dB quieter than the patch it describes. This is the
        // site the whole class was found through.
        setParamFromJson(parameters, PID::limiterThresh, fx, "limiterThreshold");
        setParamFromJson(parameters, PID::limiterRelease, fx, "limiterRelease");

        // The amplifier chain. `importJsonPreset` deliberately does not reset the
        // APVTS first, so a key that is not read here does not fall back to its
        // default — it KEEPS whatever the previous patch left, and the chain
        // becomes a sticky global that survives every load. Every one of these
        // twelve therefore falls back explicitly, and to the same value the
        // parameter layout declares, so a file written before today loads the
        // chain OFF rather than inheriting it.
        const auto fxOr = [&fx](const char* key, float fallback)
        {
            return fx->hasProperty(key) ? static_cast<float>(fx->getProperty(key))
                                        : fallback;
        };
        // The four bypasses fall back to ON, which is what a file without them
        // was: their OFF state has always been a mix or depth of 0, and those
        // are restored below from the file's own values.
        setParam(parameters, PID::fxDistOn,         fxOr("distOn", 1.0f));
        setParam(parameters, PID::fxChorusOn,       fxOr("chorusOn", 1.0f));
        setParam(parameters, PID::fxPhaserOn,       fxOr("phaserOn", 1.0f));
        setParam(parameters, PID::fxTremOn,         fxOr("tremOn", 1.0f));
        setParam(parameters, PID::fxDistDrive,      fxOr("distDrive", 0.0f));
        setParam(parameters, PID::fxDistMix,        fxOr("distMix", 0.0f));
        setParam(parameters, PID::fxTremRate,       fxOr("tremRate", 5.5f));
        setParam(parameters, PID::fxTremDepth,      fxOr("tremAmt", 0.0f));
        setParam(parameters, PID::fxTremStereo,     fxOr("tremStereo", 0.0f));
        // A file without it was a sine, which is index 0 — and choiceFromKey
        // returns 0 for an absent key, so the fallback is the same either way.
        setParam(parameters, PID::fxTremWave, static_cast<float>(
            choiceFromKey(fx->getProperty("tremWave").toString(), TremWave::kEntries)));
        setParam(parameters, PID::fxChorusRate,     fxOr("chorusRate", 0.8f));
        setParam(parameters, PID::fxChorusDepth,    fxOr("chorusAmt", 0.35f));
        setParam(parameters, PID::fxChorusMix,      fxOr("chorusMix", 0.0f));
        setParam(parameters, PID::fxPhaserRate,     fxOr("phaserRate", 0.4f));
        setParam(parameters, PID::fxPhaserDepth,    fxOr("phaserAmt", 0.5f));
        setParam(parameters, PID::fxPhaserFeedback, fxOr("phaserFeedback", 0.0f));
        setParam(parameters, PID::fxPhaserMix,      fxOr("phaserMix", 0.0f));
    }

    // ── Filter — CRITICAL: cutoff is normalized 0-1, convert to Hz ──
    if (auto* filt = root->getProperty("filter").getDynamicObject())
    {
        // Merge enabled + type: if enabled=false, force type to Off. The two
        // are views of ONE parameter, so they default together — a file
        // carrying neither lands on the layout's own type, and a file carrying
        // `type` alone is taken at its word instead of being forced Off by a
        // bool that simply is not there.
        int filtType = filt->hasProperty("type")
            ? filterTypeFromString(filt->getProperty("type").toString())
            : layoutDefaultIntOf(parameters, PID::filterType);
        if (filt->hasProperty("enabled") && ! static_cast<bool>(filt->getProperty("enabled")))
            filtType = FilterType::Off;
        setParam(parameters, PID::filterType, static_cast<float>(filtType));
        setChoiceParamFromJson(parameters, PID::filterSlope, filt, "slope", filterSlopeFromString);
        // Convert normalized cutoff to Hz: 20 * pow(1000, n). Absent is the
        // sharpest case in the whole importer: the conversion is exact at 0,
        // so a missing cutoff used to load a perfectly plausible 20 Hz — an
        // open filter closed all the way down, with nothing to look wrong.
        setParamFromJson(parameters, PID::filterCutoff, filt, "cutoff",
                         [](const juce::var& v) { return cutoffNormToHz(static_cast<float>(v)); });
        // Filter algorithm: absent in pre-Ladder/Warp presets -> SVF (bit-identical).
        // Read BEFORE the resonance, along with the warp style, because epoch 7
        // changed the resonance law for the two ladder algorithms — and the Warp's
        // pole is per saturation style.
        const int filtAlgIdx = filt->hasProperty("algorithm")
            ? filterAlgorithmFromString(filt->getProperty("algorithm").toString())
            : FilterAlgorithm::SVF;
        const int filtWarpStyleIdx = filt->hasProperty("warpStyle")
            ? filterWarpStyleFromString(filt->getProperty("warpStyle").toString())
            : FilterWarpStyle::Tanh;
        setParamFromJson(parameters, PID::filterResonance, filt, "resonance",
                         [&](const juce::var& v) {
                             return Calibration::migrateResoScalar(
                                 static_cast<float>(v), fileCalibEpoch,
                                 filtAlgIdx, filtWarpStyleIdx);
                         });
        setParamFromJson(parameters, PID::filterMix, filt, "mix");   // default 1.0, not 0
        setParamFromJson(parameters, PID::filterKbdTrack, filt, "kbdTrack");
        // Drive: absent in older presets -> treat as 0 dB.
        setParam(parameters, PID::filterDrive,
                 filt->hasProperty("drive") ? static_cast<float>(filt->getProperty("drive")) : 0.0f);
        // Drive OS: absent in older presets -> current default 2x.
        setParam(parameters, PID::filterDriveOs,
                 filt->hasProperty("driveOs")
                     ? static_cast<float>(filterDriveOsFromString(filt->getProperty("driveOs").toString()))
                     : static_cast<float>(FilterDriveOs::X2));
        setParam(parameters, PID::filterAlgorithm, static_cast<float>(filtAlgIdx));
        setParam(parameters, PID::filterWarpStyle, static_cast<float>(filtWarpStyleIdx));
    }

    // ── Sequencer ──
    if (auto* seq = root->getProperty("sequencer").getDynamicObject())
    {
        // We're about to write a custom step pattern straight into the
        // sequencer. Sync lastSeqPreset to the live dropdown value first, so the
        // audio-thread preset-apply (see processBlock) sees no pending change and
        // won't reload the canned preset over the pattern we just imported.
        lastSeqPreset.store(static_cast<int>(parameters.getRawParameterValue(PID::seqPreset)->load()),
                            std::memory_order_relaxed);

        // Preserve current seq_running state — don't stop playback on preset load
        // bool seqEnabled = seq->getProperty("enabled");
        // setParam(parameters, PID::seqRunning, seqEnabled ? 1.0f : 0.0f);
        // 0 BPM and a 0-step pattern are not states this sequencer has; the
        // layout says 120 and 16, and stepCount also drives setNumSteps and
        // the loop over the steps array below, so an absent key used to load a
        // sequencer with nothing in it.
        setParamFromJson(parameters, PID::seqBpm, seq, "bpm");
        const int stepCount = seq->hasProperty("stepCount")
            ? static_cast<int>(seq->getProperty("stepCount"))
            : layoutDefaultIntOf(parameters, PID::seqSteps);
        setParam(parameters, PID::seqSteps, static_cast<float>(stepCount));
        stepSequencer.setNumSteps(stepCount);
        if (!importingSequencePattern)
            clearSequencerOneShotSamples();

        auto* stepsArr = seq->getProperty("steps").getArray();
        if (stepsArr)
        {
            for (int i = 0; i < std::min(stepCount, stepsArr->size()); ++i)
            {
                auto* s = (*stepsArr)[i].getDynamicObject();
                if (!s) continue;
                // A step is not an APVTS parameter, so the fallbacks are
                // T5ynthStepSequencer::Step's own declared values
                // (StepSequencer.h:137-140 — note 60, velocity 0.8, enabled).
                // A step object that omits "velocity" or "active" describes a
                // step it did not think about, not a silent disabled one.
                int semitone = static_cast<int>(s->getProperty("semitone"));
                stepSequencer.setStepNote(i, 60 + semitone); // C3 + semitone offset
                stepSequencer.setStepVelocity(i, jsonFloatOr(s, "velocity", 0.8f));
                stepSequencer.setStepEnabled(i, s->hasProperty("active")
                                                    ? static_cast<bool>(s->getProperty("active")) : true);
                if (s->hasProperty("gate"))
                    stepSequencer.setStepGate(i, static_cast<float>(s->getProperty("gate")));
                if (s->hasProperty("bindMode"))
                    stepSequencer.setStepBindMode(i, static_cast<T5ynthStepSequencer::BindMode>(
                        juce::jlimit(0, 2, static_cast<int>(s->getProperty("bindMode")))));
                else if (s->hasProperty("bind"))
                    stepSequencer.setStepBindMode(i, static_cast<bool>(s->getProperty("bind"))
                        ? T5ynthStepSequencer::BindMode::Bind : T5ynthStepSequencer::BindMode::Off);
                else if (s->hasProperty("glide"))  // ancient pre-rename presets meant a ramped glide
                    stepSequencer.setStepBindMode(i, static_cast<bool>(s->getProperty("glide"))
                        ? T5ynthStepSequencer::BindMode::Glide : T5ynthStepSequencer::BindMode::Off);
                if (!importingSequencePattern)
                {
                    if (auto* oneShots = s->getProperty("oneShots").getArray())
                    {
                        for (int slot = 0; slot < std::min(T5ynthStepSequencer::ONE_SHOT_SLOTS, oneShots->size()); ++slot)
                        {
                            auto* shot = (*oneShots)[slot].getDynamicObject();
                            if (shot == nullptr || !shot->hasProperty("mode"))
                                continue;

                            const int mode = juce::jlimit(0, 2, static_cast<int>(shot->getProperty("mode")));
                            stepSequencer.setStepOneShotMode(i, slot,
                                static_cast<T5ynthStepSequencer::OneShotMode>(mode));
                        }
                    }
                }
            }
        }
        // Octave 0 is index 2 and 1/16 is index 4 — an absent key used to drop
        // the pattern two octaves and slow it to whole notes.
        setChoiceParamFromJson(parameters, PID::seqOctave, seq, "octaveShift", SeqOctave::kEntries);
        setChoiceParamFromJson(parameters, PID::seqDivision, seq, "division", SeqDivision::kEntries);
        setParamFromJson(parameters, PID::seqGlideTime, seq, "glideTime");
        setParamFromJson(parameters, PID::seqGate, seq, "gate");   // default 0.8; 0 is a gate that never opens
        setParamFromJson(parameters, PID::seqShuffle, seq, "shuffle");
        setChoiceParamFromJson(parameters, PID::scaleRoot, seq, "scaleRoot", ScaleRoot::kEntries);
        setChoiceParamFromJson(parameters, PID::scaleType, seq, "scaleType", ScaleType::kEntries);
    }

    // ── Arpeggiator ──
    if (auto* arp = root->getProperty("arpeggiator").getDynamicObject())
    {
        setChoiceParamFromJson(parameters, PID::arpMode, arp, "pattern", ArpMode::kEntries);
        // 1/16 is index 2 and the octave range defaults to 1 — absence used to
        // mean quarter notes over no octaves at all.
        setChoiceParamFromJson(parameters, PID::arpRate, arp, "rate", ArpRate::kEntries);
        setParamFromJson(parameters, PID::arpOctaves, arp, "octaveRange",
                         [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); });
    }

    // ── Generative sequencer ──
    if (auto* gs = root->getProperty("generativeSeq").getDynamicObject())
    {
        setBoolParamFromJson(parameters, PID::genSeqRunning, gs, "enabled");
        // Every one of these has a non-zero default (21 steps, 16 pulses,
        // rotation 2, mutation 0.80, range 3, and two of the four Fix switches
        // on). A Euclidean strand of 0 steps and 0 pulses is not a pattern.
        setParamFromJson(parameters, PID::genSteps, gs, "steps",
                         [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); });
        setParamFromJson(parameters, PID::genPulses, gs, "pulses",
                         [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); });
        setParamFromJson(parameters, PID::genRotation, gs, "rotation",
                         [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); });
        setParamFromJson(parameters, PID::genMutation, gs, "mutation");
        setChoiceParamFromJson(parameters, PID::genRange, gs, "range", GenRange::kEntries);
        setBoolParamFromJson(parameters, PID::genFixSteps,    gs, "fixSteps");
        setBoolParamFromJson(parameters, PID::genFixPulses,   gs, "fixPulses");
        setBoolParamFromJson(parameters, PID::genFixRotation, gs, "fixRotation");
        setBoolParamFromJson(parameters, PID::genFixMutation, gs, "fixMutation");

        // Inter-strand coordination (persisted since 2026-07-16). Absent in
        // older presets → restore the defaults (Density Budget, cap 3, both
        // matching the param declarations) rather than inheriting whatever
        // the session had — a preset is a full patch. NOTE choiceFromKey('')
        // would yield 0 = Independent, not the default, hence the explicit
        // hasProperty branch.
        setParam(parameters, PID::genCoordinationMode,
                 static_cast<float>(gs->hasProperty("coordination")
                     ? choiceFromKey(gs->getProperty("coordination").toString(),
                                     CoordinationMode::kEntries)
                     : CoordinationMode::DensityBudget));
        setParam(parameters, PID::genCoordinationCap,
                 static_cast<float>(gs->hasProperty("coordinationCap")
                     ? static_cast<int>(gs->getProperty("coordinationCap"))
                     : 3));

        // Shared pitch field (optional — absent in pre-polyphonic presets)
        if (auto* pf = gs->getProperty("pitchField").getDynamicObject())
        {
            // Drift is index 1, so an absent mode used to freeze the field.
            setChoiceParamFromJson(parameters, PID::genFieldMode, pf, "mode", FieldMode::kEntries);
            setParamFromJson(parameters, PID::genFieldRate, pf, "rate",
                             [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); });
            setParamFromJson(parameters, PID::genFieldCenterPc, pf, "centerPc",
                             [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); });
            setChoiceParamFromJson(parameters, PID::genFieldPivot, pf, "pivot", FieldPivot::kEntries);
        }

        // Strand 0 extras (optional)
        if (auto* s0 = gs->getProperty("strand0").getDynamicObject())
        {
            // Line is index 1 and x1 is index 6 — absence used to make the
            // strand an Anchor running at a sixteenth of its speed.
            setChoiceParamFromJson(parameters, PID::genRole, s0, "role", StrandRole::kEntries);
            setParamFromJson(parameters, PID::genOctave, s0, "octave",
                             [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); });
            setChoiceParamFromJson(parameters, PID::genDivMult, s0, "divMult", StrandDivMult::kEntries);
            setParamFromJson(parameters, PID::genDominance, s0, "dominance");
        }

        // Strands 2..5 (optional — pre-polyphonic presets and 4-strand
        // presets simply skip the missing entries; the affected strand stays
        // at its APVTS default).
        struct StrandImportIds {
            const char* enable;  const char* role;    const char* octave;  const char* divMult;
            const char* dominance; const char* steps; const char* pulses;  const char* rotation;
            const char* mutation; const char* fS;     const char* fP;      const char* fR;      const char* fM;
        };
        static const StrandImportIds kExtrasImport[4] = {
            { PID::gen2Enable, PID::gen2Role, PID::gen2Octave, PID::gen2DivMult,
              PID::gen2Dominance, PID::gen2Steps, PID::gen2Pulses, PID::gen2Rotation,
              PID::gen2Mutation, PID::gen2FixSteps, PID::gen2FixPulses, PID::gen2FixRotation, PID::gen2FixMutation },
            { PID::gen3Enable, PID::gen3Role, PID::gen3Octave, PID::gen3DivMult,
              PID::gen3Dominance, PID::gen3Steps, PID::gen3Pulses, PID::gen3Rotation,
              PID::gen3Mutation, PID::gen3FixSteps, PID::gen3FixPulses, PID::gen3FixRotation, PID::gen3FixMutation },
            { PID::gen4Enable, PID::gen4Role, PID::gen4Octave, PID::gen4DivMult,
              PID::gen4Dominance, PID::gen4Steps, PID::gen4Pulses, PID::gen4Rotation,
              PID::gen4Mutation, PID::gen4FixSteps, PID::gen4FixPulses, PID::gen4FixRotation, PID::gen4FixMutation },
            { PID::gen5Enable, PID::gen5Role, PID::gen5Octave, PID::gen5DivMult,
              PID::gen5Dominance, PID::gen5Steps, PID::gen5Pulses, PID::gen5Rotation,
              PID::gen5Mutation, PID::gen5FixSteps, PID::gen5FixPulses, PID::gen5FixRotation, PID::gen5FixMutation }
        };
        static const char* kExtraKeysImport[4] = { "strand2", "strand3", "strand4", "strand5" };
        for (int i = 0; i < 4; ++i)
        {
            auto* sn = gs->getProperty(kExtraKeysImport[i]).getDynamicObject();
            if (!sn) continue;
            const auto& ids = kExtrasImport[i];
            // Same defaults as strand 0's, plus 16 steps / 5 pulses / 0.20
            // mutation of their own — none of which is 0.
            const auto readInt = [](const juce::var& v) { return static_cast<float>(static_cast<int>(v)); };
            setBoolParamFromJson(parameters, ids.enable, sn, "enabled");
            setChoiceParamFromJson(parameters, ids.role, sn, "role", StrandRole::kEntries);
            setParamFromJson(parameters, ids.octave, sn, "octave", readInt);
            setChoiceParamFromJson(parameters, ids.divMult, sn, "divMult", StrandDivMult::kEntries);
            setParamFromJson(parameters, ids.dominance, sn, "dominance");
            setParamFromJson(parameters, ids.steps,    sn, "steps",    readInt);
            setParamFromJson(parameters, ids.pulses,   sn, "pulses",   readInt);
            setParamFromJson(parameters, ids.rotation, sn, "rotation", readInt);
            setParamFromJson(parameters, ids.mutation, sn, "mutation");
            setBoolParamFromJson(parameters, ids.fS, sn, "fixSteps");
            setBoolParamFromJson(parameters, ids.fP, sn, "fixPulses");
            setBoolParamFromJson(parameters, ids.fR, sn, "fixRotation");
            setBoolParamFromJson(parameters, ids.fM, sn, "fixMutation");
        }
    }

    // CC bindings — resolve entries outside the lock (getParameter is lock-free
    // but non-trivial), then hold the lock only for fill + field writes.
    // Matches the handleAsyncUpdate() pattern: never call getParameter under
    // the SpinLock; doing so lengthens the audio thread's contended window.
    {
        struct PendingEntry { int cc; CcMapping m; };
        std::vector<PendingEntry> pending;
        if (const auto* arr = root->getProperty("ccMappings").getArray())
        {
            for (const auto& entry : *arr)
            {
                auto* obj = entry.getDynamicObject();
                if (!obj) continue;
                const int cc = static_cast<int>(obj->getProperty("cc"));
                if (cc < 0 || cc >= 128) continue;
                juce::String pid = obj->getProperty("param").toString();
                if (pid.isEmpty()) continue;
                pid = LaunchControlXLLeds::migrateLegacyKnobParam(cc, pid);   // repair pre-fix swapped XL knob layout
                auto* param = parameters.getParameter(pid);
                if (!param) continue;
                CcMapping m;
                m.paramId = pid;
                m.param   = param;
                m.minNorm = obj->hasProperty("min") ? static_cast<float>(obj->getProperty("min")) : 0.0f;
                m.maxNorm = obj->hasProperty("max") ? static_cast<float>(obj->getProperty("max")) : 1.0f;
                pending.push_back({ cc, std::move(m) });
            }
        }
        const juce::SpinLock::ScopedLockType lock(ccMappingLock_);
        ccMappings_.fill({});
        for (auto& e : pending)
            ccMappings_[static_cast<size_t>(e.cc)] = std::move(e.m);
    }
    // NOTE: this fill replaces ONLY ccMappings_ (user/preset bindings). The XL controller
    // bindings live in the separate xlDefaults_ device layer (resolveCcMapping), which is
    // not touched here and not serialized — so a preset load can no longer wipe the XL
    // faders/encoders. No re-apply needed.

    // Pin engine-mode to the loaded value so the audio thread's Step↔Gen
    // transition (which copies pattern data between the two sequencers) does
    // not fire on the next block and overwrite the freshly imported step/gen
    // state. Without this, loading a preset while the in-memory mode differs
    // from the preset's mode silently corrupts the just-loaded sequencer
    // state.
    {
        const juce::ScopedLock sl (getCallbackLock());
        const bool wantGen = paramCache.genSeqRunning->load() > 0.5f;
        genModeActiveInAudio = wantGen;
        lastGenSteps = lastGenPulses = lastGenRotation = -1;
        lastGenMutation = -1.0f;
    }

    eventLogGuard.commit({});   // no filename here; the caller's own marker (applyLoadedPreset) carries the real name
    return true;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new T5ynthProcessor();
}

// ── MIDI CC Learn ──────────────────────────────────────────────────────────

void T5ynthProcessor::startMidiLearn(const juce::String& paramId)
{
    midiLearnParamId = paramId;
    midiLearnTargetCc.store(-1, std::memory_order_release);
    midiLearnActive.store(true, std::memory_order_release);
    sendLearnLed(true, -1);
    if (onMidiLearnStateChanged) onMidiLearnStateChanged(true, -1);
}

void T5ynthProcessor::cancelMidiLearn()
{
    midiLearnActive.store(false, std::memory_order_release);
    midiLearnParamId.clear();
    midiLearnTargetCc.store(-1, std::memory_order_release);
    // NOTE: do NOT cancelPendingUpdate() here — the AsyncUpdater is shared with the XL
    // button toggles, and cancelling would silently drop (then later replay) a pending
    // toggle. Cleanup is done inline below; if a learn-capture async is still queued it
    // runs harmlessly (handleAsyncUpdate returns at the !midiLearnActive guard).
    sendLearnLed(false, -1);
    if (onMidiLearnStateChanged) onMidiLearnStateChanged(false, -1);
}

void T5ynthProcessor::clearCcMapping(int cc)
{
    if (cc < 0 || cc >= 128) return;
    // Turn off the LED before clearing the binding.
    const int note = LaunchControlXLLeds::ccToLedNote(cc);
    if (note >= 0)
        sendMidiOutputMessage(LaunchControlXLLeds::ledOff(note));
    const juce::SpinLock::ScopedLockType lock(ccMappingLock_);
    ccMappings_[static_cast<size_t>(cc)] = {};
}

void T5ynthProcessor::clearAllCcMappings()
{
    const juce::SpinLock::ScopedLockType lock(ccMappingLock_);
    ccMappings_.fill({});
}

T5ynthProcessor::CcMapping T5ynthProcessor::getCcMappingCopy(int cc) const
{
    if (cc < 0 || cc >= 128) return {};
    // Hold the lock only for POD fields that the audio thread also reads.
    // paramId is message-thread-only, so it is safe to read after the lock.
    CcMapping result;
    {
        const juce::SpinLock::ScopedLockType lock(ccMappingLock_);
        // Resolve through the SAME precedence the audio thread uses (XL device layer wins,
        // else user/preset) so GUI consumers — e.g. the easy-mode "ENV/LFO/Drift tab follows
        // the controller" feature in SynthPanel — see the active XL binding for CC 5-36,
        // which now lives in xlDefaults_, not ccMappings_.
        const auto& m = resolveCcMapping(cc);
        result.param   = m.param;
        result.minNorm = m.minNorm;
        result.maxNorm = m.maxNorm;
        result.paramId = m.paramId;  // must be read under the lock (juce::String is not free-threaded)
    }
    return result;
}

void T5ynthProcessor::handleAsyncUpdate()
{
    // The KNOBS switch moved (click, automation or CC), or the oscillator did:
    // make the patch agree with what the two of them now say.
    if (authorReconcileWanted_.exchange(false, std::memory_order_acq_rel))
        reconcileAuthorSettings();

    // XL DAW-mode transport buttons: toggle on the message thread (setValueNotifyingHost
    // locks, so it must not run on the audio thread). Consumed before — and independently
    // of — CC-Learn. exchange() collapses duplicate requests to a single toggle.
    if (xlSeqToggleReq_.exchange(false, std::memory_order_acq_rel) && seqRunningParam_ != nullptr)
        seqRunningParam_->setValueNotifyingHost(
            paramCache.seqRunning->load() > 0.5f ? 0.0f : 1.0f);
    if (xlSeqModeToggleReq_.exchange(false, std::memory_order_acq_rel) && genSeqRunningParam_ != nullptr)
        genSeqRunningParam_->setValueNotifyingHost(
            paramCache.genSeqRunning->load() > 0.5f ? 0.0f : 1.0f);

    // XL "Generate" button (CC 37): trigger a generation on the message thread.
    // onGenerateRequested → MainPanel::triggerMainGeneration (which itself no-ops if a
    // generation is already in flight, and forces drift_regen = Manual). exchange()
    // collapses duplicate presses.
    if (xlGenerateReq_.exchange(false, std::memory_order_acq_rel) && onGenerateRequested)
        onGenerateRequested();

    // XL Re-Prompt stance buttons (CC 38-44): set reprompt_stance to the requested index
    // (0-6). The PromptPanel stance bar is attached to the param, so the UI follows. -1
    // (the reset sentinel) means no press is pending; last-press-wins on a duplicate.
    const int stanceReq = xlRepromptStanceReq_.exchange(-1, std::memory_order_acq_rel);
    if (stanceReq >= 0)
        if (auto* p = parameters.getParameter(PID::repromptStance))
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(stanceReq)));

    // XL snapshot buttons (CC 45-48): recall slot 1-4 via the editor (activateSnapshot
    // restores the params and refreshes the snapshot UI). -1 = no press pending.
    const int snapReq = xlSnapshotReq_.exchange(-1, std::memory_order_acq_rel);
    if (snapReq >= 0 && onSnapshotRequested)
        onSnapshotRequested(snapReq);

    // Aftertouch → Snap: recall the slot the pressure has reached. Its own
    // mailbox, not the controller's - a single slot would let whichever wrote
    // last silently eat the other, and the controller press is a one-shot with
    // no second chance while a bar can simply be moved again. The controller is
    // drained just above, so in a cycle carrying both, the bar has the last
    // word; the press is not lost, it is overruled by the finger.
    //
    // And it waits for the Csound swap for the same reason the cache landing
    // below does: in the language oscillator a slot carries an orchestra, and
    // installing it takes csoundLifecycleMutex_, which the compile thread holds
    // across a full prepare and warmup. Called straight from a held control that
    // is the whole window frozen for over a second, once per step.
    {
        const juce::uint64 req = atSnapReq_.exchange(0, std::memory_order_acq_rel);
        const bool arrivedNow = traversalReqIdx(req) >= 0;
        if (arrivedNow)
        {
            pendingAtSnapSlot_ = traversalReqIdx(req);
            pendingAtSnapSeq_  = traversalReqSeq(req);   // latest wins
        }

        const bool waitForSwap = isLanguageOscillatorSounding()
                              && (csoundCompileInFlight_.load(std::memory_order_acquire)
                               || csoundSwapPending_.load(std::memory_order_acquire)
                               || csoundSwapFading_.load(std::memory_order_acquire));
        // Belonging to a gesture that has ended.
        const bool stale = pendingAtSnapSlot_ >= 0
                        && traversalSeqReached(pendingAtSnapSeq_,
                               atSnapCancelSeq_.load(std::memory_order_acquire));

        // A press that can land NOW lands, cancelled or not: it is a millisecond
        // behind the finger, which is no distance at all, and the alternative is
        // a last step that arrives or does not depending on when the message
        // thread happened to run. Pressure reaches zero a few milliseconds
        // before the note-off on most keyboards, so that last step is the end of
        // very nearly every phrase.
        //
        // A press that would have to WAIT is another matter. It arrives when the
        // Csound swap frees up, a second or more later, and by then the gesture
        // it belonged to is over - so once overtaken by a cancel it goes, and it
        // goes on the same terms whenever this happens to run.
        if (pendingAtSnapSlot_ >= 0 && stale && (waitForSwap || ! arrivedNow))
        {
            pendingAtSnapSlot_ = -1;
            atSnapForgetActed_.store(true, std::memory_order_release);
        }

        if (pendingAtSnapSlot_ >= 0 && ! waitForSwap)
        {
            const int slot = pendingAtSnapSlot_;
            pendingAtSnapSlot_ = -1;
            if (onSnapshotRequested)
                onSnapshotRequested(slot);
        }
    }

    // Aftertouch → Cache: play the entry the pressure has reached. Same editor
    // path the Re-Prompt stepping uses, so the held note crossfades to it.
    // Nothing waits here - a neural entry installs at once - so the only cancel
    // that can reach this one is the hard one, and it applies wherever the press
    // has got to.
    {
        const juce::uint64 req = atCachePosReq_.exchange(0, std::memory_order_acq_rel);
        const int idx = traversalReqIdx(req);
        if (idx >= 0)
        {
            if (traversalSeqReached(traversalReqSeq(req),
                                    atCacheHardCancelSeq_.load(std::memory_order_acquire)))
                atCacheForgetActed_.store(true, std::memory_order_release);
            else if (onCachePositionRequested)
                onCachePositionRequested(idx);
        }
    }

    // Aftertouch → Cache, LRO side. Out through the editor like the neural one:
    // installing a slot is only half done in this processor.
    //
    // And it WAITS rather than blocking. requestCsoundOrchestra takes
    // csoundLifecycleMutex_, which the compile thread holds across a full Csound
    // prepare and warmup - over a second - so calling it from a performance
    // gesture would freeze the whole GUI for that long, on a control the player
    // is holding. So the newest position is kept and installed when the swap is
    // free; the swap's own completion comes back through here (it re-triggers to
    // let a request that arrived during the fade start), so nothing is stranded.
    {
        // No ordering between the mailbox and the cancel is needed, and none is
        // attempted: the press carries the serial it was made under, the cancel
        // carries the serial it cancels up to, and comparing the two gives the
        // same answer however the two threads interleave. See the Snap bar above
        // for which presses a cancel is allowed to take back.
        const juce::uint64 lroReq = atLroCachePosReq_.exchange(0, std::memory_order_acq_rel);
        const bool arrivedNow = traversalReqIdx(lroReq) >= 0;
        if (arrivedNow)
        {
            pendingLroCachePos_ = traversalReqIdx(lroReq);
            pendingLroCacheSeq_ = traversalReqSeq(lroReq);   // latest wins
        }

        const bool swapBusy = csoundCompileInFlight_.load(std::memory_order_acquire)
                           || csoundSwapPending_.load(std::memory_order_acquire)
                           || csoundSwapFading_.load(std::memory_order_acquire);
        const bool stale = pendingLroCachePos_ >= 0
                        && traversalSeqReached(pendingLroCacheSeq_,
                               atCacheCancelSeq_.load(std::memory_order_acquire));
        // The hard one takes no exemption: a press resolved against the cache
        // the player has switched away from is wrong however fresh it is.
        const bool hardStale = pendingLroCachePos_ >= 0
                        && traversalSeqReached(pendingLroCacheSeq_,
                               atCacheHardCancelSeq_.load(std::memory_order_acquire));

        if (pendingLroCachePos_ >= 0
            && (hardStale || (stale && (swapBusy || ! arrivedNow))))
        {
            pendingLroCachePos_ = -1;
            atCacheForgetActed_.store(true, std::memory_order_release);
        }

        if (pendingLroCachePos_ >= 0 && ! swapBusy)
        {
            const int idx = pendingLroCachePos_;
            pendingLroCachePos_ = -1;
            // Through the EDITOR, exactly as the neural landing goes through
            // onCachePositionRequested. Installing the slot here directly would
            // do the processor half only - and the panel half is not decoration:
            // it opens the compile window the Re-Prompt gates read, and it hands
            // the chain the prompt and reading of the instrument now sounding.
            // Without it a bar press leaves the loop rewriting the orchestra it
            // just travelled away from, and the KNOBS card announcing that a
            // different patch has been loaded over the one that just landed.
            if (onLroCachePositionRequested)
                onLroCachePositionRequested(idx);
        }
    }

    // XL cache button (CC 49): toggle the inference cache 4 ↔ Off via the editor (keeps the
    // on-screen radio buttons in sync).
    if (xlCacheToggleReq_.exchange(false, std::memory_order_acq_rel) && onCacheToggleRequested)
        onCacheToggleRequested();

    // XL generate-timing button (CC 50): toggle drift_regen between a.s.a.p. (Auto=1) and
    // 4 bars (Bar4=4). From any other mode (e.g. Manual after a Generate press) it engages
    // a.s.a.p. The REGENERATE switchbox is attached to drift_regen, so the UI follows.
    if (xlGenTimingToggleReq_.exchange(false, std::memory_order_acq_rel))
        if (auto* p = parameters.getParameter(PID::driftRegen))
        {
            const int cur  = static_cast<int>(p->convertFrom0to1(p->getValue()));
            const int next = (cur == DriftRegen::Auto) ? DriftRegen::Bar4 : DriftRegen::Auto;
            p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(next)));
        }

    // XL auto-(re)apply: a Launch Control XL output was selected or a session was restored.
    // (Re)enter DAW mode, repopulate the Page-1 bindings, and relight the LEDs — all on the
    // message thread — so selecting the port is self-sufficient and the faders/encoders are
    // never left unbound waiting for a manual "XL Map" click.
    if (xlAutoApplyReq_.exchange(false, std::memory_order_acq_rel))
        applyXLDefaultBindings();

    // Csound engine (Phase-1 spec D9b): a live engine_mode switch into Csound
    // flagged this from parameterChanged (which may have run on the audio
    // thread — see its comment). This is the message thread, so it's safe to
    // actually launch the background compile here. Re-check on this thread:
    // the mode may have changed again since the flag was set, the instance
    // may already be ready (e.g. a fast toggle back and forth), or a previous
    // compile may already be running.
    //
    // csoundLifecycleMutex_ (adversarial-review finding, post-implementation):
    // guards this entire join+launch-decision against prepareToPlay, which
    // is NOT guaranteed to run on this (message) thread — see the mutex's
    // declaration comment in PluginProcessor.h. The spawned thread below
    // re-acquires the same mutex around its own prepare() call so the two can
    // never run concurrently; that inner lock is only briefly contended
    // (waits out whichever prepare() call — this one's or prepareToPlay's —
    // got there first), never held on the message thread for the actual
    // ~100ms compile. Targets csoundEngines_[activeIdx] (Phase 2, S1) — a
    // session that never calls requestCsoundOrchestra() never moves
    // csoundActiveIdx_ off 0, so this is exactly Phase 1's single-engine
    // bootstrap, just indexed.
    if (csoundWantsPrepare_.exchange(false, std::memory_order_acq_rel))
    {
        std::lock_guard<std::mutex> csoundLock(csoundLifecycleMutex_);
        const bool stillWantsCsound =
            static_cast<int>(paramCache.engineMode->load()) == static_cast<int>(EngineMode::Csound);
        const int activeIdx = csoundActiveIdx_.load(std::memory_order_relaxed);
        // Skip the built-in bootstrap entirely when a custom-orchestra swap
        // request is already queued (Phase 5, SPEC_phase4_5_csound_llm_
        // preset.md: "no ready active engine -> instant adopt without fade").
        // Without this guard a prompt-authored requestCsoundOrchestra() call
        // that arrives before the FIRST bootstrap compile has run would still
        // get the built-in orchestra compiled into activeIdx first, then
        // immediately need the swap-compile block below to replace it via a
        // fade — audible built-in-then-custom churn on the very first note,
        // instead of the custom orchestra landing directly.
        const bool swapAlreadyQueued = csoundSwapRequestGeneration_ != csoundSwapStartedGeneration_;
        if (stillWantsCsound && ! csoundEngines_[activeIdx].isReady()
            && ! csoundCompileInFlight_.load(std::memory_order_acquire)
            && ! swapAlreadyQueued)
        {
            // A std::thread object must be joined or detached before a new one
            // is move-assigned onto it; a previous compile's thread, if any,
            // has already signalled done via csoundCompileInFlight_ above, so
            // this join is immediate (not a stall).
            if (csoundCompileThread_.joinable())
                csoundCompileThread_.join();

            const double sr = getSampleRate();
            const int blockSize = getBlockSize();
            // Captured by value alongside sr/blockSize so the compile thread uses
            // the factor that was current when the job was queued.
            const int lroOs = lroOsFactor_.load(std::memory_order_relaxed);
            if (sr > 0.0 && blockSize > 0)
            {
                csoundCompileInFlight_.store(true, std::memory_order_release);
                csoundCompileThread_ = std::thread([this, sr, blockSize, activeIdx, lroOs]
                {
                    {
                        std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
                        csoundEngines_[activeIdx].prepare(sr, blockSize, nullptr, lroOs);   // built-in orchestra
                    }
                    csoundCompileInFlight_.store(false, std::memory_order_release);
                    // Phase-2 extension: a requestCsoundOrchestra() call that
                    // arrived while this bootstrap compile ran would otherwise be
                    // stranded (both job kinds share csoundCompileInFlight_) until
                    // some UNRELATED async update happened to fire next. Re-check
                    // now so a queued swap starts promptly.
                    triggerAsyncUpdate();
                });
            }
        }
    }

    // LRO oversampling reconcile: does the ACTIVE engine's compiled factor still
    // match what the user asked for? Done here rather than in setLroOsQuality
    // because doing it there is a check-then-act that loses the change outright:
    // prepare() holds ready == false for its whole ~1.2 s (and nothing re-reads
    // the factor afterwards), so a setting made while a compile or a
    // prepareToPlay is in flight would be stored, shown in the combo, and never
    // reach the engine — with no way to re-apply it, since re-picking the same
    // combo item fires no onChange. Here it is idempotent and self-healing: any
    // async pass with a ready active engine and a mismatch asks for a swap, and
    // an in-flight compile simply re-triggers us when it finishes.
    //
    // Compared against effectiveOversampleFactor(), NOT the raw request: at a
    // 96 kHz host rate a request of 4 legitimately compiles as 2, and comparing
    // against the raw 4 would recompile on every async pass, forever.
    {
        juce::String lroReconcileText;
        bool lroNeedsSwap = false;
        {
            std::lock_guard<std::mutex> csoundLock(csoundLifecycleMutex_);
            auto& activeEngine = csoundEngines_[csoundActiveIdx_.load(std::memory_order_relaxed)];
            const int wantedFactor = CsoundEngine::effectiveOversampleFactor(
                getSampleRate(), lroOsFactor_.load(std::memory_order_relaxed));
            if (! csoundCompileInFlight_.load(std::memory_order_acquire)
                && csoundSwapRequestGeneration_ == csoundSwapStartedGeneration_
                && activeEngine.isReady()
                && activeEngine.oversampleFactor() != wantedFactor)
            {
                // The text the ACTIVE engine actually compiled — not the UI mirror
                // (csoundOrchestraTextForUi_), which holds the last REQUESTED text
                // and is never rolled back on a failed compile: recompiling that
                // would re-run a known-bad orchestra and republish its error out of
                // nowhere, on a swap the user only asked to change the rate of.
                lroReconcileText = juce::String(activeEngine.orchestraText());
                lroNeedsSwap = true;
            }
        }
        // Outside the lock: requestCsoundOrchestra takes csoundLifecycleMutex_
        // itself, and it is not recursive.
        if (lroNeedsSwap)
            requestCsoundOrchestra(lroReconcileText);
    }

    // Csound orchestra swap (Phase-2 spec S4): decide whether to launch a NEW
    // background compile for the latest requestCsoundOrchestra() call. Reuses
    // the SAME csoundCompileThread_/csoundCompileInFlight_ as the bootstrap
    // compile above — only one Csound compile (of either kind) runs at a time
    // — and the SAME csoundLifecycleMutex_ serializes this against
    // prepareToPlay exactly like the bootstrap path (S8: "extend it to cover
    // both engines + the pending text"). Runs on every handleAsyncUpdate call
    // (not gated behind midiLearnActive below), since triggerAsyncUpdate() is
    // also how a fade-completion or a superseded compile re-arms this check.
    {
        std::lock_guard<std::mutex> csoundLock(csoundLifecycleMutex_);

        const bool wantsNewSwapCompile = csoundSwapRequestGeneration_ != csoundSwapStartedGeneration_;
        const bool freeToStartSwap = ! csoundCompileInFlight_.load(std::memory_order_acquire)
                                   && ! csoundSwapPending_.load(std::memory_order_acquire)
                                   && ! csoundSwapFading_.load(std::memory_order_acquire);

        if (wantsNewSwapCompile && freeToStartSwap)
        {
            if (csoundCompileThread_.joinable())
                csoundCompileThread_.join();

            const double sr = getSampleRate();
            const int blockSize = getBlockSize();
            if (sr > 0.0 && blockSize > 0)
            {
                const uint64_t myGeneration = csoundSwapRequestGeneration_;
                csoundSwapStartedGeneration_ = myGeneration;
                // The engine that is INACTIVE right now — stable for the whole
                // compile: csoundActiveIdx_ only ever flips at a fade's END, and
                // freeToStartSwap above already guarantees no fade is running.
                const int inactiveIdx = 1 - csoundActiveIdx_.load(std::memory_order_relaxed);
                const int currentActiveIdx = csoundActiveIdx_.load(std::memory_order_relaxed);
                // Phase 5 fix (SPEC_phase4_5_csound_llm_preset.md: "no ready
                // active engine -> instant adopt without fade"): if the engine
                // that's SUPPOSED to be live right now was never successfully
                // prepared (fresh instance, prepareToPlay raced ahead of this
                // request, or a previous compile failed), there is nothing
                // audible to fade FROM. Compile the new orchestra straight into
                // the already-active slot instead of the inactive one, so it
                // becomes ready in place — processBlock's existing
                // csoundActive check (isReady() on csoundActiveIdx_) picks it
                // up on the very next block with no swap/fade machinery
                // involved at all.
                const bool activeReadyBefore = csoundEngines_[currentActiveIdx].isReady();
                const int targetIdx = activeReadyBefore ? inactiveIdx : currentActiveIdx;
                const juce::String textCopy = csoundPendingOrchestraText_;
                std::array<float, CsoundEngine::kMaxVoices> epochsCopy, freqsCopy;
                std::memcpy(epochsCopy.data(), csoundPendingEpochs_, sizeof(csoundPendingEpochs_));
                std::memcpy(freqsCopy.data(), csoundPendingFreqs_, sizeof(csoundPendingFreqs_));

                const int lroOs = lroOsFactor_.load(std::memory_order_relaxed);
                csoundCompileInFlight_.store(true, std::memory_order_release);
                csoundCompileThread_ = std::thread([this, sr, blockSize, targetIdx, activeReadyBefore,
                                                     textCopy, epochsCopy, freqsCopy, myGeneration, lroOs]
                {
                    bool ok = false;
                    {
                        std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
                        ok = csoundEngines_[targetIdx].prepare(sr, blockSize,
                                textCopy.isEmpty() ? nullptr : textCopy.toRawUTF8(), lroOs);
                        // primeForTakeover seeds the NEW engine's voice phase/
                        // freq state from the OLD (still-active) engine so a
                        // held note's crossfade doesn't re-strike — only
                        // meaningful when there IS an old active engine to
                        // hand off from (the fade case). The instant-adopt
                        // case has no prior audible engine, so there is
                        // nothing to prime from and no fade to prepare for.
                        if (ok && activeReadyBefore)
                            csoundEngines_[targetIdx].primeForTakeover(epochsCopy.data(), freqsCopy.data());
                    }

                    // Latest-wins (spec S4, guard case 6): if a NEWER request
                    // arrived while this compiled, discard this result
                    // unconditionally — even on success — and let the next
                    // handleAsyncUpdate (triggered below) pick up the newer
                    // text instead. Never publish a stale swap target.
                    bool stillCurrent = false;
                    {
                        std::lock_guard<std::mutex> lock(csoundLifecycleMutex_);
                        stillCurrent = (myGeneration == csoundSwapRequestGeneration_);
                        if (stillCurrent)
                            csoundCompileErrorText_ = ok ? juce::String()
                                : juce::String("Csound orchestra compile failed (see console log)");
                    }

                    // Only the genuine fade case (an already-ready engine gets
                    // replaced) arms the crossfade machinery. The instant-adopt
                    // case compiled directly into csoundActiveIdx_, so it is
                    // already live — arming csoundSwapPending_ here would tell
                    // processBlock to fade INTO the same index it's already
                    // playing, which is a no-op at best and a self-referential
                    // fade at worst.
                    if (ok && stillCurrent && activeReadyBefore)
                        csoundSwapPending_.store(true, std::memory_order_release);

                    csoundCompileInFlight_.store(false, std::memory_order_release);
                    triggerAsyncUpdate();   // re-check: a newer request may be queued
                });
            }
        }
    }

    // The rest is CC-Learn, which only triggers this async update while a learn is
    // armed; a button-only update (above) has nothing more to do.
    if (! midiLearnActive.load(std::memory_order_acquire))
        return;

    const int cc = midiLearnTargetCc.load(std::memory_order_acquire);
    if (cc < 0 || cc >= 128 || midiLearnParamId.isEmpty())
    {
        midiLearnActive.store(false, std::memory_order_release);
        midiLearnParamId.clear();
        sendLearnLed(false, -1);
        if (onMidiLearnStateChanged) onMidiLearnStateChanged(false, -1);
        return;
    }

    auto* param = parameters.getParameter(midiLearnParamId);
    if (param == nullptr)
    {
        midiLearnActive.store(false, std::memory_order_release);
        midiLearnParamId.clear();
        sendLearnLed(false, -1);
        if (onMidiLearnStateChanged) onMidiLearnStateChanged(false, -1);
        return;
    }

    {
        const juce::SpinLock::ScopedLockType lock(ccMappingLock_);
        auto& m = ccMappings_[static_cast<size_t>(cc)];
        m.paramId  = midiLearnParamId;
        m.param    = param;
        m.minNorm  = 0.0f;
        m.maxNorm  = 1.0f;
    }

    midiLearnActive.store(false, std::memory_order_release);
    midiLearnParamId.clear();
    midiLearnTargetCc.store(-1, std::memory_order_release);
    sendLearnLed(false, cc);
    if (onMidiLearnStateChanged) onMidiLearnStateChanged(false, cc);
}

int T5ynthProcessor::findBoundCc(const juce::String& paramId) const
{
    const juce::SpinLock::ScopedLockType lock(ccMappingLock_);
    for (int cc = 0; cc < 128; ++cc)
        if (ccMappings_[static_cast<size_t>(cc)].paramId == paramId)
            return cc;
    return -1;
}

// ── MIDI Output (LED feedback) ─────────────────────────────────────────────

void T5ynthProcessor::openMidiOutputDevice(const juce::String& deviceId)
{
    closeMidiOutputDevice();
    if (deviceId.isEmpty()) return;

    // openDevice can block — call before acquiring the lock.
    auto device = juce::MidiOutput::openDevice(deviceId);
    if (!device) return;

    bool isXL = false;
    {
        const juce::SpinLock::ScopedLockType lock(midiOutputLock_);
        midiOutputDevice_   = std::move(device);
        midiOutputDeviceId_ = deviceId;
        // The Mk3 enumerates its ports as "LCXL3 1 DAW In" — it does NOT contain the
        // string "Launch Control XL", so the original check silently never matched and
        // auto-apply never fired (the XL stayed in Custom mode → warm default LEDs =
        // the "orange cast"). Match both the abbreviation and the full product name.
        const auto outName = midiOutputDevice_->getName();
        isXL = outName.containsIgnoreCase("LCXL")
            || outName.containsIgnoreCase("Launch Control XL");
    }

    // Selecting (or restoring) a Launch Control XL output is self-sufficient: auto-apply the
    // DAW-mode mapping so the faders/encoders bind and the LEDs light without a manual "XL
    // Map" click ("Dropdown = verbindlich aktiv"). Deferred to the message thread via the
    // shared AsyncUpdater because applyXLDefaultBindings sends SysEx + starts a Timer +
    // takes setValueNotifyingHost-class locks; triggerAsyncUpdate is safe from any thread,
    // so this also covers the setStateInformation restore path that may run off-message.
    if (isXL)
    {
        xlAutoApplyReq_.store(true, std::memory_order_release);
        triggerAsyncUpdate();
    }
}

void T5ynthProcessor::closeMidiOutputDevice()
{
    {
        const juce::SpinLock::ScopedLockType lock(midiOutputLock_);
        if (midiOutputDevice_ != nullptr && dawModeActive_.load(std::memory_order_acquire))
        {
            // Restore the device to standalone/custom mode before we stop driving it.
            // Sent directly (not via sendMidiOutputMessage) because we already hold the
            // lock — the SpinLock is non-recursive.
            try { midiOutputDevice_->sendMessageNow(LaunchControlXLLeds::dawMode(false)); } catch (...) {}
        }
        dawModeActive_.store(false, std::memory_order_release);
        midiOutputDevice_.reset();
        midiOutputDeviceId_.clear();
    }
    // Tear down the XL device layer so a disconnected/!XL output stops shadowing user
    // CC-learns. Separate lock scope — ccMappingLock_ is NEVER nested with midiOutputLock_.
    {
        const juce::SpinLock::ScopedLockType lock(ccMappingLock_);
        xlDefaults_.fill({});
        relEncoderAccum_.fill(0.0f);   // drop any parked relative-encoder remainder with the bindings
    }
}

void T5ynthProcessor::sendMidiOutputMessage(const juce::MidiMessage& msg)
{
    // Message thread only — MidiOutput::sendMessageNow is not audio-thread-safe.
    const juce::SpinLock::ScopedLockType lock(midiOutputLock_);
    if (!midiOutputDevice_) return;
    try { midiOutputDevice_->sendMessageNow(msg); } catch (...) {}
}

void T5ynthProcessor::sendLearnLed(bool learning, int boundCc)
{
    if (learning)
    {
        // No LED change on learn-start — the XL has no knob-ring LEDs to blink.
        // Amber pulse could be added here in future (button-row Phase 2).
        return;
    }
    if (boundCc >= 0)
    {
        const int note = LaunchControlXLLeds::ccToLedNote(boundCc);
        if (note >= 0)
            sendMidiOutputMessage(LaunchControlXLLeds::ledOn(note, LaunchControlXLLeds::kColorBound));
    }
}

void T5ynthProcessor::applyXLDefaultBindings()
{
    // Host LED control requires the device in DAW mode (programmer's ref p.8-12);
    // a Custom Mode's LEDs cannot be recoloured by the host. Enable it first so the
    // deferred LED burst below is honoured. (User must select the XL's "DAW" USB
    // port; faders/knobs then transmit on ch16 — handled by the channel-agnostic
    // binding apply in processBlock.) Flag DAW mode active ONLY if a device actually
    // received the enable, and write the flag under the lock that guards it.
    {
        const juce::SpinLock::ScopedLockType lock(midiOutputLock_);
        if (midiOutputDevice_ != nullptr)
        {
            // Set the flag BEFORE sending so the ch16 Note-On guard in processBlock
            // is already true if the OS MIDI stack loops back 0x9F 0x0C 0x7F.
            // (Setting after the send leaves a window where the loopback arrives
            // while dawModeActive_ is still false, causing a stuck voice.)
            dawModeActive_.store(true, std::memory_order_release);
            try { midiOutputDevice_->sendMessageNow(LaunchControlXLLeds::dawMode(true)); } catch (...) {}
        }
    }

    populateXLDefaultBindings();

    // Defer the LED burst so the device has time to finish entering DAW mode before
    // the colours arrive — otherwise the first XL-Map click leaves the LEDs dark.
    // Owned one-shot timer (not callAfterDelay) so ~T5ynthProcessor can cancel a
    // pending burst deterministically; capturing `this` is safe because the timer's
    // lifetime is bounded by the processor's and it is stopped before teardown.
    xlLedTimer_.fn = [this] { lightXLLeds(); };
    xlLedTimer_.startTimer(100);
}

void T5ynthProcessor::populateXLDefaultBindings()
{
    // Build the XL DEVICE layer from the fixed Page-1 layout. This writes xlDefaults_ — a
    // table separate from the preset-controlled ccMappings_ — so it is immune to preset
    // loads and is never serialized. No SysEx / no LED / no DAW-mode handshake here, so it
    // is safe to call from any thread. Resolve param pointers off the lock first.
    struct PendingEntry { int cc; CcMapping m; };
    std::vector<PendingEntry> pending;
    pending.reserve(LaunchControlXLLeds::kPage1Count + LaunchControlXLLeds::kExtMapCount);

    // Helper: resolve one Binding into a PendingEntry (shared by kPage1 + kExtMap)
    auto addBinding = [&](const lcxl3_detail::Binding& b)
    {
        if (b.cc < 0 || b.cc >= 128) return;
        auto* param = parameters.getParameter(b.paramId);
        if (!param) return;
        CcMapping m;
        m.paramId = b.paramId;
        m.param   = param;
        m.minNorm = b.minNorm;
        m.maxNorm = b.maxNorm;
        pending.push_back({ b.cc, std::move(m) });
    };

    for (int i = 0; i < LaunchControlXLLeds::kPage1Count; ++i)
        addBinding(LaunchControlXLLeds::kPage1[i]);

    for (int i = 0; i < LaunchControlXLLeds::kExtMapCount; ++i)
        addBinding(LaunchControlXLLeds::kExtMap[i]);

    {
        const juce::SpinLock::ScopedLockType lock(ccMappingLock_);
        xlDefaults_.fill({});                                          // rebuild the whole device layer
        relEncoderAccum_.fill(0.0f);                                   // reset relative-encoder remainder on rebind
        for (auto& e : pending)
            xlDefaults_[static_cast<size_t>(e.cc)] = std::move(e.m);
    }
}

// XL DAW-mode action-button assignments. The SAME constants drive both the LED colour
// (lightXLLeds) and the action dispatch (handleXLButtonPress) so a button's light always
// matches what it does. The two bottom button rows (programmer's ref p.9) are laid out:
//   TOP row    CC 37-44: Generate, then the 7 Re-Prompt stances (reprompt_stance 0-6).
//   BOTTOM row CC 45-52: Snap 1-4, Cache 4/Off, a.s.a.p./4 bars, Step/Gen, Panic.
// Transport lives on the dedicated LEFT transport buttons (Play 116 / Record 118).
static constexpr int kXLBtnGenerate    = 37;   // top row 1 → trigger generation (+ force regen = Manual)
static constexpr int kXLReStanceFirst  = 38;   // top row 2-8 → reprompt_stance index 0-6
static constexpr int kXLReStanceLast   = 44;
static constexpr int kXLSnapFirst      = 45;   // bottom row 1-4 → snapshot slots 1-4
static constexpr int kXLSnapLast       = 48;
static constexpr int kXLBtnCache       = 49;   // bottom row 5 → toggle inference cache 4↔Off
static constexpr int kXLBtnGenTiming   = 50;   // bottom row 6 → toggle drift_regen a.s.a.p.↔4 bars
static constexpr int kXLBtnStepGen     = 51;   // bottom row 7 → toggle gen_seq_running (Step/Gen)
static constexpr int kXLBtnPanic       = 52;   // bottom row 8 → MIDI panic (all notes off)
static constexpr int kXLBtnPlay        = 116;  // ▶ left transport → toggle seq_running (transport)
static constexpr int kXLBtnRecord      = 118;  // ● left transport → toggle gen_seq_running (Step/Gen)

void T5ynthProcessor::lightXLLeds()
{
    // Deferred DAW-mode device setup (runs ~100 ms after the mode switch, once the XL
    // has entered DAW mode). First put the three encoder rows into RELATIVE mode so the
    // endless encoders nudge the bound param instead of jumping to an absolute position.
    for (int row = 0; row < 3; ++row)
        sendMidiOutputMessage(LaunchControlXLLeds::encoderRelativeMode(row, true));

    // NOTE: Fader Pickup (soft takeover, LaunchControlXLLeds::faderPickup(true)) is left
    // OFF on purpose. The user reported jumping ENDLESS ENCODERS — fixed by relative mode
    // above; physical faders are absolute and SHOULD move the param immediately on touch.
    // Enabling pickup would make a fader dead until swept past its current value, an
    // unrequested trade-off. Re-enable here only if jump-free faders are explicitly wanted.

    // Light all XL Page-1 LEDs in their module accent colour (honoured only in DAW
    // mode). ccToLedNote()==-1 skips controls with no LED; sendMidiOutputMessage is
    // a no-op when no output device is open.
    for (const auto& b : LaunchControlXLLeds::kPage1)
    {
        const int note = LaunchControlXLLeds::ccToLedNote(b.cc);
        if (note >= 0)
            sendMidiOutputMessage(LaunchControlXLLeds::ledOn(note, b.color));
    }

    // Bottom two button rows — FUNCTION legend (DAW mode only). The faders themselves have
    // NO LED (programmer's ref p.11), so their kPage1 module colours are invisible; these
    // buttons carry the action legend instead. Control-index == CC for buttons (p.9), so
    // send the CC directly. Colours are STATIC group accents drawn from the existing module
    // palette — LEDs that track the live active stance / cache / timing / Step-Gen state are
    // a deliberate follow-up.
    //
    // TOP row CC 37-44: Generate (white) + the 7 Re-Prompt stances (periwinkle = Gen family).
    sendMidiOutputMessage(LaunchControlXLLeds::ledOn(kXLBtnGenerate, lcxl3_detail::rgb(127, 127, 127))); // ◆ Generate
    for (int cc = kXLReStanceFirst; cc <= kXLReStanceLast; ++cc)
        sendMidiOutputMessage(LaunchControlXLLeds::ledOn(cc, LaunchControlXLLeds::kColorGen));            // Re-Prompt stance
    //
    // BOTTOM row CC 45-52: Snap 1-4 (gold = LFO) · Cache (amber = Env) · a.s.a.p./4 bars (red-orange = Drift) ·
    // Step/Gen (violet = Filter) · Panic (red).
    for (int cc = kXLSnapFirst; cc <= kXLSnapLast; ++cc)
        sendMidiOutputMessage(LaunchControlXLLeds::ledOn(cc, LaunchControlXLLeds::kColorLfo));            // Snap 1-4
    sendMidiOutputMessage(LaunchControlXLLeds::ledOn(kXLBtnCache,     LaunchControlXLLeds::kColorEnv));    // Cache 4/Off
    sendMidiOutputMessage(LaunchControlXLLeds::ledOn(kXLBtnGenTiming, LaunchControlXLLeds::kColorDrift));  // a.s.a.p./4 bars
    sendMidiOutputMessage(LaunchControlXLLeds::ledOn(kXLBtnStepGen,   LaunchControlXLLeds::kColorFilter)); // Step/Gen
    sendMidiOutputMessage(LaunchControlXLLeds::ledOn(kXLBtnPanic,     lcxl3_detail::rgb(127, 0, 0)));      // Panic
    //
    // LEFT transport buttons (unchanged): Play ▶ green, Record ● periwinkle (Step/Gen).
    sendMidiOutputMessage(LaunchControlXLLeds::ledOn(kXLBtnPlay,   LaunchControlXLLeds::kColorVol));       // ▶ transport
    sendMidiOutputMessage(LaunchControlXLLeds::ledOn(kXLBtnRecord, LaunchControlXLLeds::kColorGen));       // ● Step/Gen
}

void T5ynthProcessor::handleXLButtonPress(int cc)
{
    // Audio-thread dispatch for an XL DAW-mode button press edge. Every action that sets a
    // parameter or calls into the editor is DEFERRED to the message thread
    // (handleAsyncUpdate) via an atomic request + triggerAsyncUpdate, because
    // setValueNotifyingHost / editor callbacks lock and must never run on the audio thread.
    // Only Panic acts inline (it just raises an atomic flag consumed in processBlock).
    // The two range groups (stances, snapshots) are handled first; the rest are exact CCs.

    if (cc >= kXLReStanceFirst && cc <= kXLReStanceLast)   // top row 2-8 → reprompt_stance 0-6
    {
        xlRepromptStanceReq_.store(cc - kXLReStanceFirst, std::memory_order_release);
        triggerAsyncUpdate();
        return;
    }
    if (cc >= kXLSnapFirst && cc <= kXLSnapLast)           // bottom row 1-4 → snapshot slot 1-4
    {
        xlSnapshotReq_.store(cc - kXLSnapFirst + 1, std::memory_order_release);
        triggerAsyncUpdate();
        return;
    }

    switch (cc)
    {
        case kXLBtnGenerate:
            xlGenerateReq_.store(true, std::memory_order_release);
            triggerAsyncUpdate();
            break;
        case kXLBtnCache:
            xlCacheToggleReq_.store(true, std::memory_order_release);
            triggerAsyncUpdate();
            break;
        case kXLBtnGenTiming:
            xlGenTimingToggleReq_.store(true, std::memory_order_release);
            triggerAsyncUpdate();
            break;
        case kXLBtnStepGen:   // dedicated bottom-row Step/Gen, same toggle as Record
        case kXLBtnRecord:
            xlSeqModeToggleReq_.store(true, std::memory_order_release);
            triggerAsyncUpdate();
            break;
        case kXLBtnPlay:
            xlSeqToggleReq_.store(true, std::memory_order_release);
            triggerAsyncUpdate();
            break;
        case kXLBtnPanic:
            requestMidiPanic();
            break;
        default:
            break;
    }
}
