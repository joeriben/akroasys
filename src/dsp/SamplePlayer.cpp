#include "SamplePlayer.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#define SAMPLER_DEBUG_LOG 0

namespace
{
constexpr float kNormalizeCeilingDb = -1.0f;
constexpr float kSustainedTargetDb = -18.0f;
constexpr float kTransientPercentileTargetDb = -10.0f;
constexpr float kNearSilentPeakDb = -36.0f;
constexpr float kNearSilentActiveDb = -50.0f;
constexpr float kHotHeadroomDb = 2.0f;
constexpr float kShortTransientSeconds = 0.75f;
constexpr float kActiveBlockThresholdDb = -40.0f;
constexpr float kTransientActiveRatio = 0.35f;
constexpr float kTransientCrestDb = 18.0f;
constexpr float kTransientPeakGapDb = 8.0f;
// Inside this much of the sample's own pitch, the direct read IS the better
// answer as well as the cheaper one: it hands the recorded samples through
// untouched, where the stretcher would take them apart and rebuild them.
constexpr float kNearUnitySemitones = 0.1f;

#if SAMPLER_DEBUG_LOG
juce::String samplerPtrTag(const void* ptr)
{
    return "0x" + juce::String::toHexString(static_cast<juce::int64>(reinterpret_cast<std::uintptr_t>(ptr)));
}

void samplerDebugLog(const juce::String& message)
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
// expensive string-concat arguments (juce::String + samplerPtrTag +
// debugStateString) are never evaluated. The previous `if constexpr` lived
// inside the function body — too late: the caller had already built the
// final juce::String via heap allocations on the audio thread.
#define samplerDebugLog(...) ((void)0)
#endif

const char* loopModeName(SamplePlayer::LoopMode mode)
{
    switch (mode)
    {
        case SamplePlayer::LoopMode::OneShot:  return "OneShot";
        case SamplePlayer::LoopMode::Loop:     return "Loop";
        case SamplePlayer::LoopMode::PingPong: return "PingPong";
    }
    return "?";
}

float dbToGain(float db)
{
    return juce::Decibels::decibelsToGain(db);
}

float gainToDb(float gain)
{
    return juce::Decibels::gainToDecibels(std::max(gain, 1.0e-9f));
}
}

void SamplePlayer::prepare(double sampleRate, int samplesPerBlock)
{
    playbackSampleRate = sampleRate;
    maxBlockSize = samplesPerBlock;
    rawReadBuf.resize(static_cast<size_t>(samplesPerBlock));
    prepareStretcher();
}

void SamplePlayer::reset()
{
    originalBuffer.setSize(0, 0);
    playBuffer.setSize(0, 0);
    // playbackSnapshot_/retiredSnapshot_ are plain shared_ptr fields, published/
    // parked/read only under getCallbackLock() (see their declarations below and
    // applyPreparedPlaybackState/morphToBufferFrom/drainRetiredSnapshot). reset()
    // also rewrites the equally non-atomic originalBuffer/playBuffer/audioLoaded
    // that the worker mutates under that same lock — so reset() must run under
    // it too. Every path into it starts at T5ynthProcessor::releaseResources,
    // which holds that lock: the master instance directly, the per-voice ones
    // through VoiceManager::reset() -> SynthVoice::reset().
    // With the worker excluded and the audio thread stopped (releaseResources'
    // JUCE contract), these plain clears are safe.
    playbackSnapshot_.reset();
    retiredSnapshot_.reset();
    morphFromSnapshot_.reset();
    audioLoaded = false;
    playing = false;
    readPosition = 0.0;
    sourceGain_ = 1.0f;
    morphActive_ = false;
    morphAlpha_ = 1.0f;
    morphIncrement_ = 0.0f;
    sharedMode = false;
    stretcher.reset();
    pitchModReachSemis_ = 0.0f;
    pitchPathLatched_ = false;
    pitchPathUsesStretch_ = false;
}

void SamplePlayer::loadBuffer(const juce::AudioBuffer<float>& buffer, double bufferSampleRate)
{
    if (sharedMode)
    {
        samplerDebugLog("loadBuffer ignored sharedMode player=" + samplerPtrTag(this));
        return; // shared-mode players don't own audio
    }

    samplerDebugLog("loadBuffer player=" + samplerPtrTag(this)
                    + " samples=" + juce::String(buffer.getNumSamples())
                    + " sr=" + juce::String(bufferSampleRate, 2));
    const auto config = capturePrepareConfig();
    applyPreparedBufferLoad(prepareBufferLoad(buffer, bufferSampleRate, config), config);
    playing = true;
}

SamplePlayer::PrepareConfig SamplePlayer::capturePrepareConfig() const
{
    PrepareConfig config;
    config.loopMode = loopMode;
    config.crossfadeMs = crossfadeMsVal;
    config.normalizeOn = normalizeOn;
    config.loopOptimizeLevel = loopOptimizeLevel;
    config.startPosFrac = startPosFrac;
    config.loopStartFrac = loopStartFrac;
    config.loopEndFrac = loopEndFrac;
    return config;
}

SamplePlayer::PreparedBufferLoad SamplePlayer::prepareBufferLoad(const juce::AudioBuffer<float>& buffer,
                                                                double bufferSampleRate,
                                                                const PrepareConfig& config) const
{
    PreparedBufferLoad prepared;
    prepared.originalBuffer.makeCopyOf(buffer);
    trimLeadingSilence(prepared.originalBuffer);
    prepared.playbackState = preparePlaybackState(prepared.originalBuffer, bufferSampleRate, config);
    return prepared;
}

void SamplePlayer::applyPreparedBufferLoad(PreparedBufferLoad prepared, const PrepareConfig& config)
{
    originalBuffer = std::move(prepared.originalBuffer);
    loopMode = config.loopMode;
    crossfadeMsVal = config.crossfadeMs;
    normalizeOn = config.normalizeOn;
    loopOptimizeLevel = config.loopOptimizeLevel;
    startPosFrac = config.startPosFrac;
    loopStartFrac = config.loopStartFrac;
    loopEndFrac = config.loopEndFrac;
    applyPreparedPlaybackState(std::move(prepared.playbackState));
    sharedMode = false;
    needsReprepareFlag = false;
}

void SamplePlayer::shareBufferFrom(const SamplePlayer& master)
{
    bool wasShared = sharedMode;
    sharedMode = true;
    // The master republishes its snapshot under getCallbackLock() (see
    // applyPreparedPlaybackState). This function itself runs either on the
    // audio thread inside processBlock (locked for its whole duration by the
    // host wrapper on every shipped format) or off-thread under an explicit
    // ScopedLock(getCallbackLock()) — see VoiceManager::distributeSamplerBuffer's
    // callers. That shared lock is the ONLY guard on this read; without it,
    // reading master.playbackSnapshot_ here races its publish.
    playbackSnapshot_ = master.playbackSnapshot_;
    if (playbackSnapshot_ == nullptr)
        playBuffer.setSize(0, 0);
    bufferOriginalSR = master.bufferOriginalSR;
    playStart = master.playStart;
    playEnd = master.playEnd;
    coldStart = master.coldStart;
    loopMode = master.loopMode;
    startPosFrac = master.startPosFrac;
    loopStartFrac = master.loopStartFrac;
    loopEndFrac = master.loopEndFrac;
    wtExtractStartFrac_ = master.wtExtractStartFrac_;
    wtExtractEndFrac_ = master.wtExtractEndFrac_;
    startPosOffset_ = master.startPosOffset_;
    sourceGain_ = master.sourceGain_;
    audioLoaded = master.audioLoaded;
    // A plain (non-crossfade) buffer sync — cancel any in-flight crossfade so the
    // freshly shared buffer plays directly. morphFromSnapshot_ is retained (freed
    // off-thread at the next morph/reset), never released on the audio thread.
    morphActive_ = false;
    // Only reset read position on first share, not on subsequent syncs
    if (!wasShared)
        readPosition = static_cast<double>(coldStart);
    needsReprepareFlag = false;

    samplerDebugLog("shareBufferFrom dst=" + samplerPtrTag(this)
                    + " src=" + samplerPtrTag(&master)
                    + " wasShared=" + juce::String(wasShared ? 1 : 0)
                    + " readPos=" + juce::String(readPosition, 2)
                    + " state={" + debugStateString() + "}");
}

void SamplePlayer::morphToBufferFrom(const SamplePlayer& master, float morphMs)
{
    // Live-follow crossfade for a playing shared voice. Runs ONLY on the AUDIO
    // THREAD, inside processBlock — VoiceManager.h documents allowMorph=true as
    // valid only at the audio-thread call site — so processBlock's host-wrapper
    // lock (held for its whole duration on every shipped format) is in effect
    // for this entire function. That lock is what makes the plain read below
    // safe against the master's publish (applyPreparedPlaybackState, always
    // under an explicit ScopedLock(getCallbackLock())); without it, this would
    // race the master's store. The voice-side pointers below (playbackSnapshot_,
    // retiredSnapshot_, morphFromSnapshot_) are never written from any other
    // thread, so touching them here needs no lock of their own — only the
    // cross-instance read of master.playbackSnapshot_ depends on the callback
    // lock. Snapshot discipline mirrors FreezeTextureEngine.
    auto masterSnap = master.playbackSnapshot_;
    if (masterSnap == nullptr || masterSnap == playbackSnapshot_)
        return;                              // no master audio, or already current —
                                             // pointer identity is the generation guard
                                             // (never restarts an in-flight crossfade)

    if (retiredSnapshot_ != nullptr)
        return;                              // reclaim slot still full — defer one
                                             // block (current crossfade stays valid)

    // A CROSSFADE MAY ALREADY BE RUNNING, and what happens then decides whether
    // this is a crossfade or a hard swap.
    //
    // Restarting the ramp unconditionally — which is what this did — makes the
    // buffer we are PLAYING the new fade-from and stands it up at full gain. Mid
    // crossfade that buffer is the fade-TO, i.e. the QUIET side: 10 ms into a
    // 200 ms fade it carries sin(0.05*pi/2) = 0.08 of the sound. So the restart
    // dropped the 0.997 side and promoted the 0.08 side — a hard swap, the one
    // thing this function exists to avoid. Measured, and it is not hypothetical:
    // a SNAP recall used to re-assert the slot's loop points AFTER
    // loadGeneratedAudio had published, which marked the master for a re-prepare
    // and landed a second publication of the SAME audio ~10 ms later. Nothing
    // about the sound had changed and the held note jumped anyway
    // (tools/audition_sampler_follow.cpp's republish cases: a sample-to-sample
    // step 13x the material's own largest, +3.2 dB of level in 5 ms).
    //
    // Two slots is all there is — a fade-from and a fade-to — so a third buffer
    // cannot arrive without one of the two currently sounding sides going away.
    // The rule is to drop whichever side is QUIETER, and it is the rule the
    // other two engines already run (WavetableOscillator::beginMorphToMipData,
    // FreezeTextureEngine::morphToBufferFrom — "promote the dominant side"):
    //
    //   alpha >= 0.5  the in-flight target has taken over. It becomes the
    //                 fade-from; the side dropped is the old fade-from, at
    //                 cos(alpha*pi/2) <= 0.707.
    //   alpha <  0.5  the fade-from is still dominant and stays. The side
    //                 dropped is the target, at sin(alpha*pi/2) <= 0.707.
    //
    // The ramp is reset in BOTH cases, so the newcomer enters at gain 0 and
    // contributes nothing to the step. What the step IS: the dropped side
    // vanishing at its current gain, PLUS the kept side stepping from its
    // current gain up to 1.0 — worst case sin(a*pi/2)*|dropped| +
    // (1-cos(a*pi/2))*|kept| below halfway, mirrored above it. Both terms are
    // small early and jointly largest right at the 0.5 boundary; the second one
    // is why this is not a "bounded by the dropped side alone" rule.
    //
    // Worst per-sample step, measured on the guard's material (220 Hz at 0.50
    // fading to 660 Hz at 0.85, third buffer 330 Hz at 0.70, 200 ms ramp, worst
    // of 8 sub-block offsets), against the two alternatives:
    //
    //   alpha   rotate-always (what this replaces)   THIS   re-point, keep ramp
    //   0.05                              1.176     0.075                 0.107
    //   0.20                              0.884     0.247                 0.317
    //   0.40                              0.659     0.530                 0.858
    //   0.495                             0.562     0.721                 1.088
    //   0.55 and up                                identical by construction
    //
    // Two things that table settles. Re-pointing the target WITHOUT resetting
    // the ramp — the obvious "gentler" move — substitutes one buffer for another
    // at the target's live gain, costs |new - old| there, and is worse than
    // either of the others past about a third of the ramp. And this rule is NOT
    // uniformly better than the unconditional rotate it replaces: in a narrow
    // band from about alpha 0.42 to 0.5 it costs up to 0.16 more, because the
    // kept side's step to unity is largest exactly where the rule still keeps it.
    // The band is 8% of the ramp; the case that made this audible sits at alpha
    // 0.05, where the step drops 15-fold and lands below the material's own
    // largest sample-to-sample motion (0.073). The threshold is where it is
    // because it is exact for two equally loud sides (the step comparison
    // reduces to cos > sin); moving it per-amplitude would beat the house rule
    // in that band and would be a rule the other two engines do not run.
    //
    // Either way playbackSnapshot_ ends up as the master's newest, which is a
    // lifetime invariant and not just tidiness: shareBufferFrom() overwrites
    // playbackSnapshot_ with a plain assignment on the audio thread when the
    // voice goes inactive, so a voice left holding a snapshot nobody else holds
    // would free its buffers there. Deferring adoption while the fade runs opens
    // exactly that window, for a whole Regen XFade.
    //
    // The cost of the shared rule, stated once so no caller is surprised: while
    // publications keep arriving faster than morphMs, every one of them restarts
    // the ramp, so the kept fade-from never ages out and the held note does not
    // walk forward through the generations — it reaches the newest buffer one
    // full morphMs after the stream stops, not during it. Wavetable and Freeze
    // behave the same way; the audition guard's "publication stream" case pins
    // the landing.
    //
    // The displaced snapshot — exactly one per call, either branch — is parked
    // for the off-thread drain rather than released here: its last reference
    // must not drop on the audio thread. The slot was just observed empty and
    // only the audio thread parks, so the std::vector free always lands in
    // drainRetiredSnapshot() off the audio thread. The store into
    // retiredSnapshot_ below is a plain assignment: this whole function runs
    // only on the audio thread inside processBlock (see the function-top
    // comment), and drainRetiredSnapshot()'s clear always runs under an
    // explicit ScopedLock(getCallbackLock()) — that lock is what keeps the
    // park here and the drain there from ever overlapping.
    std::shared_ptr<const PlaybackSnapshot> outgoing;

    if (morphActive_ && morphFromSnapshot_ != nullptr && morphAlpha_ < 0.5f)
    {
        outgoing = playbackSnapshot_;            // the quiet side is the target: it goes,
                                                 // the dominant fade-from keeps its place
    }
    else
    {
        outgoing           = morphFromSnapshot_; // prior fade-from (may be null)
        morphFromSnapshot_ = playbackSnapshot_;  // current playback → fade-from
    }

    playbackSnapshot_ = masterSnap;              // adopt the new buffer
    retiredSnapshot_ = std::move(outgoing);

    // Equal-power ramp over morphMs (the global Drift Crossfade). morphMs<=0 →
    // 1 sample = effectively instant. Only crossfade when there is a fade-from
    // buffer and the voice is sounding; otherwise this is a plain adopt.
    const int morphSamples = juce::jmax(1, juce::roundToInt(
        static_cast<double>(morphMs) * 0.001 * playbackSampleRate));
    morphAlpha_     = 0.0f;
    morphIncrement_ = 1.0f / static_cast<float>(morphSamples);
    morphActive_    = playing && morphFromSnapshot_ != nullptr;

    // Follow the new sample's region / loop / SR state.
    bufferOriginalSR    = master.bufferOriginalSR;
    playStart           = master.playStart;
    playEnd             = master.playEnd;
    coldStart           = master.coldStart;
    loopMode            = master.loopMode;
    startPosFrac        = master.startPosFrac;
    loopStartFrac       = master.loopStartFrac;
    loopEndFrac         = master.loopEndFrac;
    wtExtractStartFrac_ = master.wtExtractStartFrac_;
    wtExtractEndFrac_   = master.wtExtractEndFrac_;
    startPosOffset_     = master.startPosOffset_;
    audioLoaded         = master.audioLoaded;
    sharedMode          = true;
    needsReprepareFlag  = false;
    // readPosition / playDirection_ / inFirstPass_ deliberately preserved
    // (playback continuity). sourceGain_ preserved (per-voice noteOn
    // normalization; configureForBlock re-asserts it each block).

    samplerDebugLog("morphToBufferFrom dst=" + samplerPtrTag(this)
                    + " src=" + samplerPtrTag(&master)
                    + " morphMs=" + juce::String(morphMs, 1)
                    + " active=" + juce::String(morphActive_ ? 1 : 0)
                    + " readPos=" + juce::String(readPosition, 2)
                    + " state={" + debugStateString() + "}");
}

void SamplePlayer::drainRetiredSnapshot()
{
    // MUST be called off the audio thread, under an explicit
    // ScopedLock(getCallbackLock()) — every caller (VoiceManager::
    // drainRetiredSamplerSnapshots, called from PluginProcessor.cpp's
    // serviceSamplerReprepare/loadGeneratedAudio/reloadProcessedAudio) already
    // holds it. That lock is what keeps this exchange from ever running
    // concurrently with morphToBufferFrom's plain park on the audio thread
    // (processBlock holds the same lock for its whole duration on every
    // shipped format). Take the parked snapshot and let this local drop the
    // last reference here — the buffer free lands off the audio thread.
    auto dead = std::exchange(retiredSnapshot_, nullptr);
    (void) dead;
}

void SamplePlayer::setMidiNote(int note)
{
    transposeRatio = std::pow(2.0, (note - 60) / 12.0);
    glideSamplesLeft = 0; // cancel any active glide
}

void SamplePlayer::setTransposeRatio(double ratio)
{
    transposeRatio = ratio;
    glideSamplesLeft = 0;
}

void SamplePlayer::glideToSemitones(int semitones, float durationMs)
{
    glideToRatio(std::pow(2.0, semitones / 12.0), durationMs);
}

void SamplePlayer::glideToRatio(double targetRatio, float durationMs)
{
    double durationSamples = (durationMs / 1000.0) * playbackSampleRate;
    int samples = std::max(1, static_cast<int>(durationSamples));

    glideTargetRatio = targetRatio;
    glideRatioIncr = (targetRatio - transposeRatio) / static_cast<double>(samples);
    glideSamplesLeft = samples;
}

void SamplePlayer::retrigger()
{
    // Fresh note: the render path is chosen again on this note's first block.
    // Above the empty-buffer early return — this is note state, not buffer
    // geometry, and must never leak from one note into the next.
    pitchPathLatched_ = false;
    pitchPathUsesStretch_ = false;

    const auto& buf = currentPlaybackBuffer();
    const int bufLen = buf.getNumSamples();
    if (bufLen == 0) return;

    // Effective P1 = base + modulation offset (Scan→P1 in Sampler mode)
    float effectiveP1 = juce::jlimit(0.0f, 1.0f, startPosFrac + startPosOffset_);

    // Determine direction based on P1 vs P3
    bool reversed = (effectiveP1 > loopEndFrac);
    playDirection_ = reversed ? -1 : 1;
    inFirstPass_ = true;

    // Convert P1 fraction to sample position
    int startSample = static_cast<int>(std::floor(effectiveP1 * bufLen));

    readPosition = static_cast<double>(juce::jlimit(0, bufLen - 1, startSample));
    playing = true;
    // Fresh hard restart — stop any in-flight buffer-adoption crossfade so the
    // fading-out old buffer can't bleed into the new note's first samples.
    // morphFromSnapshot_ is retained (audio thread); released off-thread on the
    // next morphToBufferFrom park / reset.
    morphActive_ = false;
    morphAlpha_ = 1.0f;

    if (stretcherPrepared)
    {
        stretcher.reset();
        stretcherNeedsPriming = true;
    }

    samplerDebugLog("retrigger player=" + samplerPtrTag(this)
                    + " startSample=" + juce::String(startSample)
                    + " effectiveP1=" + juce::String(effectiveP1, 4)
                    + " state={" + debugStateString() + "}");
}

void SamplePlayer::setLoopStart(float frac)
{
    float clamped = juce::jlimit(0.0f, loopEndFrac - 0.01f, frac);
    if (clamped != loopStartFrac)
    {
        loopStartFrac = clamped;
        needsReprepareFlag = true;
    }
}

void SamplePlayer::setLoopEnd(float frac)
{
    float clamped = juce::jlimit(loopStartFrac + 0.01f, 1.0f, frac);
    if (clamped != loopEndFrac)
    {
        loopEndFrac = clamped;
        needsReprepareFlag = true;
    }
}

void SamplePlayer::setLoopRegion(float startFrac, float endFrac)
{
    // Same clamps as the two single setters, applied to the pair in one step so
    // the region never passes through a wider intermediate one (see the header).
    const float s = juce::jlimit(0.0f, 0.99f, startFrac);
    const float e = juce::jlimit(s + 0.01f, 1.0f, endFrac);

    if (s != loopStartFrac || e != loopEndFrac)
    {
        loopStartFrac = s;
        loopEndFrac = e;
        needsReprepareFlag = true;
    }
}

void SamplePlayer::setStartPos(float frac)
{
    startPosFrac = juce::jlimit(0.0f, 1.0f, frac);
}

void SamplePlayer::setLoopMode(LoopMode mode)
{
    if (mode != loopMode) { loopMode = mode; needsReprepareFlag = true; }
}

void SamplePlayer::setCrossfadeMs(float ms)
{
    float clamped = juce::jlimit(0.0f, 500.0f, ms);
    if (clamped != crossfadeMsVal) { crossfadeMsVal = clamped; needsReprepareFlag = true; }
}

void SamplePlayer::setNormalize(bool on)
{
    if (on != normalizeOn) { normalizeOn = on; needsReprepareFlag = true; }
}

void SamplePlayer::setLoopOptimizeLevel(int level)
{
    int clamped = juce::jlimit(0, 2, level);
    if (clamped != loopOptimizeLevel) { loopOptimizeLevel = clamped; needsReprepareFlag = true; }
}

// ═══════════════════════════════════════════════════════════════════
// Buffer preparation (mirrors useSamplePlayer.ts prepareBuffer)
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::preparePlaybackBuffer()
{
    if (originalBuffer.getNumSamples() == 0)
        return;

    applyPreparedPlaybackState(
        preparePlaybackState(originalBuffer, bufferOriginalSR, capturePrepareConfig()));
    needsReprepareFlag = false;

    samplerDebugLog("preparePlaybackBuffer player=" + samplerPtrTag(this)
                    + " bufLen=" + juce::String(originalBuffer.getNumSamples())
                    + " norm=" + juce::String(normalizeOn ? 1 : 0)
                    + " state={" + debugStateString() + "}");
}

SamplePlayer::PreparedPlaybackState SamplePlayer::preparePlaybackState(
    const juce::AudioBuffer<float>& sourceBuffer, double sourceSampleRate, const PrepareConfig& config) const
{
    PreparedPlaybackState prepared;
    prepared.bufferOriginalSR = sourceSampleRate;
    prepared.audioLoaded = sourceBuffer.getNumSamples() > 0 && sourceBuffer.getNumChannels() > 0;

    if (!prepared.audioLoaded)
        return prepared;

    const int bufLen = sourceBuffer.getNumSamples();
    const int numCh  = sourceBuffer.getNumChannels();

    int ls = static_cast<int>(std::floor(config.loopStartFrac * bufLen));
    int le = std::min(bufLen, static_cast<int>(std::ceil(config.loopEndFrac * bufLen)));

    if (le - ls < 4)
        le = std::min(bufLen, ls + 4);

    prepared.playBuffer.makeCopyOf(sourceBuffer);

    int actualEnd = le;
    int firstPassNormEnd = le;

    if (config.loopMode == LoopMode::Loop)
    {
        if (config.loopOptimizeLevel > 0 && numCh > 0)
        {
            const float* data = sourceBuffer.getReadPointer(0);
            // Stage 1: phase-align loop end via cross-correlation (texture match).
            actualEnd = optimizeLoopEnd(data, ls, le, bufLen, config.loopOptimizeLevel);
            // Stage 2: refine loop start so the wrap splice matches in amplitude
            // AND slope. xcorr alone matches surrounding context but can leave a
            // small step at the splice itself — this stage closes that gap.
            ls = refineLoopStart(data, ls, actualEnd, bufLen, config.loopOptimizeLevel);
        }

        prepared.firstPassBuffer.makeCopyOf(prepared.playBuffer);

        int preEnd = actualEnd;
        applyLoopCrossfade(prepared.playBuffer, ls, actualEnd, config.crossfadeMs, sourceSampleRate);
        int fadeSamples = preEnd - actualEnd;
        firstPassNormEnd = preEnd;

        prepared.playStart = ls;
        prepared.playEnd   = actualEnd;
        prepared.coldStart = ls + fadeSamples;
    }
    else if (config.loopMode == LoopMode::PingPong)
    {
        // Snap both boundaries to the nearest local extremum so velocity-reversal
        // happens where the first derivative is naturally near zero (eliminates
        // the click that comes from sudden direction change at high-slope samples).
        if (config.loopOptimizeLevel > 0 && numCh > 0)
        {
            const float* data = sourceBuffer.getReadPointer(0);
            const int radius = (config.loopOptimizeLevel == 1) ? 512 : 2048;
            // End first, so start can use the snapped end as its upper bound.
            // boundHi is the *highest readable index* — the function reads i+1 internally,
            // so passing bufLen (one-past-end) would let i reach bufLen-1 and read data[bufLen].
            actualEnd = snapToLocalExtremum(data, le, radius, ls + 4, bufLen - 1);
            ls        = snapToLocalExtremum(data, ls, radius, 0,      actualEnd - 4);
        }
        prepared.playStart = ls;
        prepared.playEnd   = actualEnd;
        prepared.coldStart = ls;
    }
    else // OneShot
    {
        prepared.playStart = ls;
        prepared.playEnd   = actualEnd;
        prepared.coldStart = ls;
    }

    if (config.normalizeOn)
    {
        int p1 = static_cast<int>(std::floor(config.startPosFrac * bufLen));
        int normStart = std::min(p1, prepared.playStart);
        const int rs = juce::jlimit(0, prepared.playBuffer.getNumSamples(), normStart);
        const int re = juce::jlimit(rs, prepared.playBuffer.getNumSamples(), prepared.playEnd);
        if (re > rs && prepared.playBuffer.getNumChannels() > 0)
        {
            const NormalizeAnalysis analysis = analyzeNormalizeRegion(prepared.playBuffer, rs, re, sourceSampleRate);
            const NormalizeMode mode = chooseNormalizeMode(analysis);
            const float gain = chooseNormalizeGain(analysis, mode);
            if (std::isfinite(gain) && std::abs(gain - 1.0f) >= 1.0e-4f)
            {
                applyGainToRegion(prepared.playBuffer, rs, re, gain);

                if (prepared.firstPassBuffer.getNumSamples() == prepared.playBuffer.getNumSamples())
                    applyGainToRegion(prepared.firstPassBuffer, rs, firstPassNormEnd, gain);
            }

            samplerDebugLog("normalizeBuffer mode=" + juce::String(normalizeModeName(mode))
                            + " gainDb=" + juce::String(gainToDb(gain), 2)
                            + " peakDb=" + juce::String(gainToDb(analysis.peak), 2)
                            + " p999Db=" + juce::String(gainToDb(analysis.percentilePeak), 2)
                            + " rmsDb=" + juce::String(gainToDb(analysis.rms), 2)
                            + " activeDb=" + juce::String(gainToDb(analysis.activeRms), 2)
                            + " crestDb=" + juce::String(analysis.crestDb, 2)
                            + " peakGapDb=" + juce::String(analysis.peakToPercentileDb, 2)
                            + " activeRatio=" + juce::String(analysis.activeRatio, 3)
                            + " dur=" + juce::String(analysis.durationSeconds, 3));
        }
    }

    // Boundary guard samples MUST be written last so they capture the final state
    // of the playback region (post-crossfade, post-normalization). Without these,
    // cubic interpolation at the wrap/reverse moment reads unrelated audio past
    // the loop region — the primary cause of clicks/dropouts at boundaries.
    writeBoundaryGuards(prepared.playBuffer, prepared.playStart, prepared.playEnd, config.loopMode);

    return prepared;
}

void SamplePlayer::applyPreparedPlaybackState(PreparedPlaybackState preparedState)
{
    auto snapshot = std::make_shared<PlaybackSnapshot>();
    snapshot->playBuffer = std::move(preparedState.playBuffer);
    snapshot->firstPassBuffer = std::move(preparedState.firstPassBuffer);
    snapshot->bufferOriginalSR = preparedState.bufferOriginalSR;
    // Publish under getCallbackLock(): this is a PLAIN assignment, not an atomic
    // one, so the lock is the ONLY thing preventing a torn read / UAF against
    // the audio-thread readers in shareBufferFrom / morphToBufferFrom. Every
    // live caller of this function (applyPreparedBufferLoad, from
    // serviceSamplerReprepare / loadGeneratedAudio / reloadProcessedAudio in
    // PluginProcessor.cpp) already holds an explicit ScopedLock(getCallbackLock())
    // around the call, and processBlock holds the SAME lock for its whole
    // duration on every shipped format (Standalone/VST3/AU — the JUCE wrapper
    // locks it), so the audio thread is excluded here. preparePlaybackBuffer()
    // also reaches this function but currently has no caller anywhere in src/;
    // if it ever gains one, that caller MUST hold getCallbackLock() too, or
    // this store races the audio thread.
    playbackSnapshot_ = std::move(snapshot);

    playBuffer.setSize(0, 0);
    bufferOriginalSR = preparedState.bufferOriginalSR;
    playStart = preparedState.playStart;
    playEnd = preparedState.playEnd;
    coldStart = preparedState.coldStart;
    audioLoaded = preparedState.audioLoaded;

    readPosition = static_cast<double>(coldStart);
}

// ═══════════════════════════════════════════════════════════════════
// processBlock
// ═══════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════
// Catmull-Rom cubic interpolation (4-point, no trig — ~20× faster than Lanczos)
// ═══════════════════════════════════════════════════════════════════

const juce::AudioBuffer<float>& SamplePlayer::currentPlaybackBuffer() const
{
    if (playbackSnapshot_ != nullptr)
        return playbackSnapshot_->playBuffer;
    return playBuffer;
}

const juce::AudioBuffer<float>& SamplePlayer::currentFirstPassBuffer() const
{
    if (playbackSnapshot_ != nullptr
        && playbackSnapshot_->firstPassBuffer.getNumSamples() > 0
        && playbackSnapshot_->firstPassBuffer.getNumChannels() > 0)
    {
        return playbackSnapshot_->firstPassBuffer;
    }

    return currentPlaybackBuffer();
}

double SamplePlayer::currentBufferOriginalSR() const
{
    if (playbackSnapshot_ != nullptr)
        return playbackSnapshot_->bufferOriginalSR;
    return bufferOriginalSR;
}

float SamplePlayer::cubicSample(double pos) const
{
    return cubicSampleFrom(currentPlaybackBuffer(), pos);
}

float SamplePlayer::playbackSample(double pos, bool useFirstPassBuffer) const
{
    const float newS = cubicSampleFrom(
        useFirstPassBuffer ? currentFirstPassBuffer() : currentPlaybackBuffer(), pos);

    // Held-note Drift-Crossfade: equal-power blend the outgoing buffer in while
    // the crossfade ramps. All morph state is audio-thread-owned (morphToBufferFrom
    // runs on the audio thread), so these are plain reads. Both buffers are read at
    // the same read position; cubicSampleFrom bounds-clamps each independently, so a
    // length/region mismatch is at worst a brief, fading-out artefact, never an OOB.
    if (! morphActive_ || morphFromSnapshot_ == nullptr)
        return newS;

    const auto& oldBuf =
        (useFirstPassBuffer && morphFromSnapshot_->firstPassBuffer.getNumSamples() > 0)
            ? morphFromSnapshot_->firstPassBuffer
            : morphFromSnapshot_->playBuffer;
    const float oldS = cubicSampleFrom(oldBuf, pos);

    const float g       = juce::jlimit(0.0f, 1.0f, morphAlpha_);
    const float newGain = std::sin(g * juce::MathConstants<float>::halfPi);
    const float oldGain = std::cos(g * juce::MathConstants<float>::halfPi);
    return newS * newGain + oldS * oldGain;
}

float SamplePlayer::cubicSampleFrom(const juce::AudioBuffer<float>& buf, double pos) const
{
    if (buf.getNumSamples() <= 0 || buf.getNumChannels() <= 0)
        return 0.0f;

    const float* data = buf.getReadPointer(0);
    const int bufLen = buf.getNumSamples();

    int i1 = static_cast<int>(std::floor(pos));
    float t = static_cast<float>(pos - i1);

    // Clamp the base index into the buffer FIRST, then derive all four cubic taps
    // from the clamped value so every data[] read stays in bounds even when pos
    // runs past the buffer. A held voice live-following onto a freshly-adopted
    // SHORTER buffer keeps its old readPosition (morphToBufferFrom preserves it),
    // so pos can exceed bufLen for the sample or two before advancePosition wraps
    // it. i0 was previously computed from the PRE-clamp i1 and read out of bounds
    // (crash: i0 = floor(pos)-1 far past a shorter buffer). t is the fractional
    // part (pos - floor pos) and is unaffected by the clamp.
    if (i1 < 0) i1 = 0;
    else if (i1 >= bufLen) i1 = bufLen - 1;
    int i0 = (i1 > 0) ? i1 - 1 : 0;
    int i2 = (i1 + 1 < bufLen) ? i1 + 1 : bufLen - 1;
    int i3 = (i1 + 2 < bufLen) ? i1 + 2 : bufLen - 1;

    float p0 = data[i0], p1 = data[i1], p2 = data[i2], p3 = data[i3];

    // Catmull-Rom spline: 12 mul + 8 add, zero trig
    float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    float b =         p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    float c = -0.5f * p0              + 0.5f * p2;
    // d = p1
    return ((a * t + b) * t + c) * t + p1;
}

// ═══════════════════════════════════════════════════════════════════
// advancePosition — 3-point loop logic (shared by processSample/readRawSamples)
// ═══════════════════════════════════════════════════════════════════

bool SamplePlayer::advancePosition(double speedMagnitude)
{
    readPosition += speedMagnitude * playDirection_;

    const double pEnd   = static_cast<double>(playEnd);
    const double pStart = static_cast<double>(playStart);

    if (inFirstPass_)
    {
        // During first pass, only check the boundary the playhead is moving toward
        if (playDirection_ > 0 && readPosition >= pEnd)
        {
            inFirstPass_ = false;
            if (loopMode == LoopMode::OneShot) { playing = false; return false; }
            double overshoot = readPosition - pEnd;
            if (loopMode == LoopMode::PingPong)
            {
                readPosition = pEnd - overshoot;
                playDirection_ = -1;
            }
            else // Loop
            {
                readPosition = pStart + overshoot;
            }
        }
        else if (playDirection_ < 0 && readPosition < pStart)
        {
            inFirstPass_ = false;
            if (loopMode == LoopMode::OneShot) { playing = false; return false; }
            double overshoot = pStart - readPosition;
            if (loopMode == LoopMode::PingPong)
            {
                readPosition = pStart + overshoot;
                playDirection_ = 1;
            }
            else // Loop
            {
                readPosition = pEnd - overshoot;
            }
        }
    }
    else
    {
        // Standard looping between P2-P3
        if (readPosition >= pEnd)
        {
            double overshoot = readPosition - pEnd;
            if (loopMode == LoopMode::PingPong)
            {
                readPosition = pEnd - overshoot;
                playDirection_ = -1;
            }
            else
            {
                readPosition = pStart + overshoot;
            }
        }
        else if (readPosition < pStart)
        {
            double overshoot = pStart - readPosition;
            if (loopMode == LoopMode::PingPong)
            {
                readPosition = pStart + overshoot;
                playDirection_ = 1;
            }
            else
            {
                readPosition = pEnd - overshoot;
            }
        }
    }

    // Advance the held-note Drift-Crossfade ramp one source-sample step. Called
    // once per emitted sample in the bypass path and once per consumed input
    // sample (1:1 with output per block) in the stretch path, so the crossfade
    // lasts morphMs of output either way. At alpha=1 the old buffer is fully gone.
    if (morphActive_)
    {
        morphAlpha_ += morphIncrement_;
        if (morphAlpha_ >= 1.0f)
        {
            morphAlpha_ = 1.0f;
            morphActive_ = false;
            // morphFromSnapshot_ retained (audio thread); released off-thread on the
            // next morphToBufferFrom park / reset — never freed here.
        }
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════
// processSample (Bypass mode / legacy — speed-based transposition)
// ═══════════════════════════════════════════════════════════════════

float SamplePlayer::processSample()
{
    const int regionLen = playEnd - playStart;
    if (regionLen <= 0 || !playing) return 0.0f;

    // Apply pitch glide (per-sample linear ramp)
    if (glideSamplesLeft > 0)
    {
        transposeRatio += glideRatioIncr;
        glideSamplesLeft--;
        if (glideSamplesLeft == 0)
            transposeRatio = glideTargetRatio;
    }

    // pitchModFactor belongs in the read speed, not only in the stretcher's
    // transposition: reading faster or slower IS how this path bends a note, so
    // leaving it out silently deletes everything routed to the pitch bus —
    // envelopes, LFOs, Drift, pitch bend, MPE per-note bend. It was invisible
    // while this path was only ever entered inside a 0.1-semitone dead zone
    // (0.6 cents of error at worst); once the render path is latched at the note's
    // first block it becomes unbounded, and a note held on the direct read simply
    // would not bend. Block-rate steps in the factor step the read speed, never
    // the position, so there is no seam — the same block-rate quantisation the
    // stretch path has.
    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;
    double speedRatio = srRatio * transposeRatio * static_cast<double>(pitchModFactor);

    // Read with cubic interpolation. The first pass uses the linear source path
    // so loop crossfade/optimization is only heard after the first wrap.
    const bool useFirstPassBuffer = inFirstPass_ && loopMode == LoopMode::Loop;
    float result = playbackSample(readPosition, useFirstPassBuffer) * sourceGain_;

    // Advance with 3-point logic (direction, first-pass, boundaries)
    advancePosition(std::abs(speedRatio));

    return result;
}

// ═══════════════════════════════════════════════════════════════════
// readRawSamples — read at 1:1 speed (SR-corrected, no transposition)
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::readRawSamples(float* output, int numSamples)
{
    const int regionLen = playEnd - playStart;
    if (regionLen <= 0 || !playing)
    {
        std::memset(output, 0, sizeof(float) * static_cast<size_t>(numSamples));
        return;
    }

    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;

    for (int i = 0; i < numSamples; ++i)
    {
        const bool useFirstPassBuffer = inFirstPass_ && loopMode == LoopMode::Loop;
        output[i] = playbackSample(readPosition, useFirstPassBuffer) * sourceGain_;

        if (!advancePosition(std::abs(srRatio)))
        {
            // OneShot ended — zero-fill remainder
            std::memset(output + i + 1, 0,
                        sizeof(float) * static_cast<size_t>(numSamples - i - 1));
            return;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// renderPitchedBlock — pitch-preserving transposition via Signalsmith Stretch
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::renderPitchedBlock(float* output, int numSamples)
{
    if (!audioLoaded || !playing)
    {
        std::memset(output, 0, sizeof(float) * static_cast<size_t>(numSamples));
        return;
    }

    // Determine mode before glide advancement (use current ratio)
    double effectiveRatio = transposeRatio * static_cast<double>(pitchModFactor);
    float semitones = static_cast<float>(12.0 * std::log2(std::max(effectiveRatio, 1e-6)));

    // Which of the two render paths this note uses is decided ONCE, here, on its
    // first block, and then kept until the note retriggers.
    //
    // Deciding it per block — asking "is the CURRENT pitch inside the dead
    // zone?" — switches engines under a sounding note, and every such switch
    // costs an analysis window: the stretcher's input→output positional latency
    // is a FULL window, so entering mid-note restarted the heard stream ~50 ms
    // back into already-played material with a splice click, and leaving it
    // skipped ~100 ms forward, because readPosition leads the heard stream by
    // that window while stretching. A vibrato on a root-pitch note was enough
    // to trigger it.
    //
    // The reach the voice pushes (setPitchModulationReach) is how far pitch
    // bend and the pitch bus can carry this note at full scale, so a note that
    // can never leave the dead zone keeps the exact direct read for its whole
    // life, and one that can leave it is on the stretcher from its first
    // sample. Notes that DO stay on the direct read still follow their pitch
    // modulation — read faster or slower, so the colour rides along with the
    // pitch the way it does on a real instrument, which is what BJ chose over
    // formant-preserving vibrato (2026-07-26).
    //
    // Consequence, deliberate: a poly BIND glide (glideToNote → glideToRatio)
    // arrives mid-note and can take a latched direct-read note far outside the
    // dead zone. It stays on the direct read and the glide is heard tape-style.
    // Re-deciding there would reintroduce exactly the mid-note switch this
    // removes. The path it stays on is the shipped PitchShiftQuality::Bypass
    // path, not a new one.
    if (!pitchPathLatched_)
    {
        const float baseSemis = static_cast<float>(
            12.0 * std::log2(std::max(transposeRatio, 1e-6)));
        pitchPathUsesStretch_ =
            (std::abs(baseSemis) + pitchModReachSemis_) >= kNearUnitySemitones;
        pitchPathLatched_ = true;
    }

    // Bypass: processSample() handles glide per-sample — no block-level advancement
    if (pitchQuality == PitchShiftQuality::Bypass || !pitchPathUsesStretch_)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            output[i] = processSample();
            if (!playing)
            {
                std::memset(output + i + 1, 0,
                            sizeof(float) * static_cast<size_t>(numSamples - i - 1));
                break;
            }
        }
        return;
    }

    // Stretch path: advance glide at block level (processSample is not called)
    if (glideSamplesLeft > 0)
    {
        int steps = std::min(glideSamplesLeft, numSamples);
        transposeRatio += glideRatioIncr * steps;
        glideSamplesLeft -= steps;
        if (glideSamplesLeft <= 0)
        {
            transposeRatio = glideTargetRatio;
            glideSamplesLeft = 0;
        }
        // Recalculate after glide advancement
        effectiveRatio = transposeRatio * static_cast<double>(pitchModFactor);
        semitones = static_cast<float>(12.0 * std::log2(std::max(effectiveRatio, 1e-6)));
    }

    if (!stretcherPrepared)
        prepareStretcher();

    // Set transposition BEFORE priming — primeStretcher() runs process()
    // internally, which must use the new note's pitch, not the previous one's.
    stretcher.setTransposeSemitones(semitones);

    if (stretcherNeedsPriming)
    {
        primeStretcher();
        stretcherNeedsPriming = false;
    }

    // Read raw samples at original speed (SR-corrected only)
    readRawSamples(rawReadBuf.data(), numSamples);

    // Pitch-shift through Signalsmith Stretch (mono: 1 channel). When a held-note
    // crossfade is in flight, readRawSamples already blended the fade-from buffer
    // into rawReadBuf, so the stretcher pitch-shifts the crossfaded source.
    float* inPtr = rawReadBuf.data();
    stretcher.process(&inPtr, numSamples, &output, numSamples);
}

int SamplePlayer::estimateReferenceLengthSamples() const
{
    const auto& buf = currentPlaybackBuffer();
    const int bufLen = buf.getNumSamples();
    if (bufLen <= 0 || !audioLoaded)
        return 0;

    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;
    if (srRatio <= 0.0)
        return 0;

    const float effectiveP1 = juce::jlimit(0.0f, 1.0f, startPosFrac + startPosOffset_);
    int startSample = static_cast<int>(std::floor(effectiveP1 * static_cast<float>(bufLen)));
    startSample = juce::jlimit(0, bufLen - 1, startSample);

    const bool reversed = (effectiveP1 > loopEndFrac);
    const int loopLen = juce::jmax(1, playEnd - playStart);
    int sourceSamples = 0;

    if (!reversed)
        sourceSamples = juce::jmax(0, playEnd - startSample);
    else
        sourceSamples = juce::jmax(0, startSample - playStart);

    if (loopMode == LoopMode::Loop)
        sourceSamples += loopLen;
    else if (loopMode == LoopMode::PingPong)
        sourceSamples += loopLen * 2;

    return juce::jmax(1, static_cast<int>(std::ceil(static_cast<double>(sourceSamples) / srRatio)));
}

juce::String SamplePlayer::debugStateString() const
{
    const auto& buf = currentPlaybackBuffer();

    return "shared=" + juce::String(sharedMode ? 1 : 0)
        + " playing=" + juce::String(playing ? 1 : 0)
        + " loaded=" + juce::String(audioLoaded ? 1 : 0)
        + " read=" + juce::String(readPosition, 2)
        + " playStart=" + juce::String(playStart)
        + " coldStart=" + juce::String(coldStart)
        + " playEnd=" + juce::String(playEnd)
        + " len=" + juce::String(buf.getNumSamples())
        + " p1=" + juce::String(startPosFrac, 4)
        + " p2=" + juce::String(loopStartFrac, 4)
        + " p3=" + juce::String(loopEndFrac, 4)
        + " p1Off=" + juce::String(startPosOffset_, 4)
        + " dir=" + juce::String(playDirection_)
        + " firstPass=" + juce::String(inFirstPass_ ? 1 : 0)
        + " mode=" + juce::String(loopModeName(loopMode))
        + " sourceGain=" + juce::String(sourceGain_, 4)
        + " needsReprepare=" + juce::String(needsReprepareFlag ? 1 : 0);
}

float SamplePlayer::estimatePlaybackRms(const float* gains, int numSamples, float* outPeak) const
{
    const auto& buf = currentPlaybackBuffer();
    const int bufLen = buf.getNumSamples();
    if (gains == nullptr || numSamples <= 0 || bufLen <= 0 || !audioLoaded)
    {
        if (outPeak != nullptr) *outPeak = 0.0f;
        return 0.0f;
    }

    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;
    const float effectiveP1 = juce::jlimit(0.0f, 1.0f, startPosFrac + startPosOffset_);

    bool inFirstPass = true;
    int playDirection = (effectiveP1 > loopEndFrac) ? -1 : 1;
    double analysisReadPosition = static_cast<double>(
        std::floor(effectiveP1 * static_cast<float>(bufLen)));

    double sumSq = 0.0;
    int renderedSamples = 0;
    float peak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const bool useFirstPassBuffer = inFirstPass && loopMode == LoopMode::Loop;
        float sample = playbackSample(analysisReadPosition, useFirstPassBuffer);
        float weighted = sample * gains[i];
        sumSq += static_cast<double>(weighted) * static_cast<double>(weighted);
        peak = std::max(peak, std::abs(weighted));
        ++renderedSamples;

        analysisReadPosition += srRatio * static_cast<double>(playDirection);

        const double pEnd = static_cast<double>(playEnd);
        const double pStart = static_cast<double>(playStart);

        if (inFirstPass)
        {
            if (playDirection > 0 && analysisReadPosition >= pEnd)
            {
                inFirstPass = false;
                if (loopMode == LoopMode::OneShot)
                    break;

                const double overshoot = analysisReadPosition - pEnd;
                if (loopMode == LoopMode::PingPong)
                {
                    analysisReadPosition = pEnd - overshoot;
                    playDirection = -1;
                }
                else
                {
                    analysisReadPosition = pStart + overshoot;
                }
            }
            else if (playDirection < 0 && analysisReadPosition < pStart)
            {
                inFirstPass = false;
                if (loopMode == LoopMode::OneShot)
                    break;

                const double overshoot = pStart - analysisReadPosition;
                if (loopMode == LoopMode::PingPong)
                {
                    analysisReadPosition = pStart + overshoot;
                    playDirection = 1;
                }
                else
                {
                    analysisReadPosition = pEnd - overshoot;
                }
            }
        }
        else
        {
            if (analysisReadPosition >= pEnd)
            {
                const double overshoot = analysisReadPosition - pEnd;
                if (loopMode == LoopMode::PingPong)
                {
                    analysisReadPosition = pEnd - overshoot;
                    playDirection = -1;
                }
                else
                {
                    analysisReadPosition = pStart + overshoot;
                }
            }
            else if (analysisReadPosition < pStart)
            {
                const double overshoot = pStart - analysisReadPosition;
                if (loopMode == LoopMode::PingPong)
                {
                    analysisReadPosition = pStart + overshoot;
                    playDirection = 1;
                }
                else
                {
                    analysisReadPosition = pEnd - overshoot;
                }
            }
        }
    }

    if (outPeak != nullptr)
        *outPeak = peak;

    if (renderedSamples <= 0)
        return 0.0f;

    return static_cast<float>(std::sqrt(sumSq / static_cast<double>(renderedSamples)));
}

// ═══════════════════════════════════════════════════════════════════
// Pitch shifter configuration
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::prepareStretcher()
{
    float sr = static_cast<float>(playbackSampleRate);
    switch (pitchQuality)
    {
        case PitchShiftQuality::Bypass:
            break;
        case PitchShiftQuality::Efficient:
            stretcher.presetCheaper(1, sr);
            break;
        case PitchShiftQuality::Default:
            stretcher.presetCheaper(1, sr); // cheap enough for polyphonic real-time
            break;
        case PitchShiftQuality::HighQuality:
            stretcher.presetDefault(1, sr);
            break;
        default:
            stretcher.presetCheaper(1, sr);
            break;
    }
    stretcherPrepared = (pitchQuality != PitchShiftQuality::Bypass);

    // Size the prime-time scratch buffers off the audio thread (prepareStretcher
    // runs from prepare()/setPitchShiftQuality, never from renderPitchedBlock).
    // primeStretcher() now uses these in place of per-call std::vector allocs.
    if (stretcherPrepared)
    {
        const auto seekCap   = static_cast<size_t>(
            std::max(0, stretcher.blockSamples() + stretcher.intervalSamples()));
        const auto primeCap  = static_cast<size_t>(std::max(0, stretcher.inputLatency()));
        if (primeSeekBuf.size()    < seekCap)  primeSeekBuf.resize(seekCap);
        if (primeInputBuf.size()   < primeCap) primeInputBuf.resize(primeCap);
        if (primeDiscardBuf.size() < primeCap) primeDiscardBuf.resize(primeCap);
    }
}

void SamplePlayer::primeStretcher()
{
    const double srRatio = currentBufferOriginalSR() / playbackSampleRate;

    // ── Step 1: seek() — fill STFT analysis context with pre-roll audio ──
    // Reads audio BEFORE readPosition so the first STFT frames
    // have real left context instead of post-reset silence.
    //
    // When the start point sits at (or near) the very beginning of the buffer
    // there IS no audio before it. This used to clamp seekLen down to whatever
    // fit — to zero at P1 = 0 — so the STFT started from empty history and the
    // synthesis-window overlap faded the first ~windowSize of output in from
    // silence: measured 46 ms to 10 % of level on every transposed note (the
    // root note stays on the direct read and never enters the stretcher), and a
    // fresh generation lands in exactly this state because loadGeneratedAudio
    // trims leading silence and auto-positions P1 at the first active window.
    // Standard edge handling instead: REFLECT the buffer around the start
    // point (read -pos for pos < 0), so the analysis windows are always FULL
    // of real local material — time-reversed, which only matters to the prime
    // as spectral content — and the full pre-roll is always paid.
    // cubicSampleFrom bounds-clamps, so a buffer shorter than the mirrored
    // reach degrades to an edge-hold, never an OOB. Measured (96 kHz,
    // tools/measure_note_latency): transposed note at P1 = 0 was 46 ms to
    // 10 % of level before, ~1 ms after — same as with real pre-roll.
    const int seekLen = stretcher.blockSamples() + stretcher.intervalSamples();
    if (seekLen > 0)
    {
        // Capacity is sized in prepareStretcher() from the same stretcher
        // instance; mismatch means prepare() never ran. Assert in debug, bail
        // in release rather than alloc on the audio thread.
        jassert (static_cast<size_t>(seekLen) <= primeSeekBuf.size());
        if (static_cast<size_t>(seekLen) <= primeSeekBuf.size())
        {
            const bool useFP = inFirstPass_ && loopMode == LoopMode::Loop;
            double pos = readPosition - seekLen * srRatio;
            for (int i = 0; i < seekLen; ++i)
            {
                // sourceGain_ matches what processSample()/readRawSamples()
                // apply to the real audio that follows this pre-roll — without
                // it the analysis history sits at a different level than the
                // input, and with Normalize active the first ~windowSize/2 of
                // output rides that mismatch (adversarial-review finding).
                primeSeekBuf[static_cast<size_t>(i)] =
                    playbackSample(pos < 0.0 ? -pos : pos, useFP) * sourceGain_;
                pos += srRatio;
            }
            float* seekPtr = primeSeekBuf.data();
            stretcher.seek(&seekPtr, seekLen, 1.0);
        }
    }

    // ── Step 2: process()+discard — pay STFT output latency ──
    // After seek, the first inputLatency() output samples correspond to
    // pre-roll audio, not the retrigger point. Discard them so the next real output
    // starts at approximately readPosition. readPosition advances intentionally.
    int primeSamples = stretcher.inputLatency();
    if (primeSamples <= 0) return;
    jassert (static_cast<size_t>(primeSamples) <= primeInputBuf.size()
             && static_cast<size_t>(primeSamples) <= primeDiscardBuf.size());
    if (static_cast<size_t>(primeSamples) > primeInputBuf.size()
        || static_cast<size_t>(primeSamples) > primeDiscardBuf.size())
        return; // safety: bail rather than allocate on the audio thread

    readRawSamples(primeInputBuf.data(), primeSamples);

    float* inPtr = primeInputBuf.data();
    float* outPtr = primeDiscardBuf.data();
    stretcher.process(&inPtr, primeSamples, &outPtr, primeSamples);
}

void SamplePlayer::setPitchShiftQuality(PitchShiftQuality quality)
{
    if (quality != pitchQuality)
    {
        pitchQuality = quality;
        prepareStretcher();
    }
}

void SamplePlayer::processBlock(juce::AudioBuffer<float>& output)
{
    if (!audioLoaded || !playing || currentPlaybackBuffer().getNumSamples() == 0)
        return;

    const int numOutChannels = output.getNumChannels();
    const int numOutSamples  = output.getNumSamples();

    for (int i = 0; i < numOutSamples; ++i)
    {
        float sample = processSample();
        if (!playing) break; // one-shot ended

        for (int ch = 0; ch < numOutChannels; ++ch)
            output.addSample(ch, i, sample);
    }
}

// ═══════════════════════════════════════════════════════════════════
// Cross-correlation loop optimizer (from useSamplePlayer.ts)
// ═══════════════════════════════════════════════════════════════════

int SamplePlayer::optimizeLoopEnd(const float* data, int loopStart, int loopEnd, int bufLen, int level) const
{
    const int loopLen = loopEnd - loopStart;
    int win = std::min(XCORR_WINDOW[level], loopLen / 4);
    if (win < 16) return loopEnd;

    // Cap search range so we never extend by more than one loop length past the
    // user's region — keeps the optimizer's choices musically plausible even at
    // High level with very short user loops.
    const int searchRange = std::min(XCORR_SEARCH[level], std::max(loopLen, 256));
    int searchLo = std::max(loopStart + win * 2, loopEnd - searchRange);
    int searchHi = std::min(bufLen, loopEnd + searchRange);

    float bestCorr = -1e30f;
    int   bestEnd  = loopEnd;

    for (int cand = searchLo; cand < searchHi; ++cand)
    {
        int eStart = cand - win;
        if (eStart < loopStart) continue;

        float sum = 0.0f, normA = 0.0f, normB = 0.0f;
        for (int j = 0; j < win; ++j)
        {
            float a = data[loopStart + j];
            float b = data[eStart + j];
            sum   += a * b;
            normA += a * a;
            normB += b * b;
        }
        float denom = std::sqrt(normA * normB);
        float corr  = denom > 0.0f ? sum / denom : 0.0f;

        if (corr > bestCorr)
        {
            bestCorr = corr;
            bestEnd  = cand;
        }
    }
    return bestEnd;
}

int SamplePlayer::refineLoopStart(const float* data, int loopStart, int loopEnd, int bufLen, int level) const
{
    if (loopEnd - loopStart < 8 || loopEnd < 2 || loopStart >= bufLen)
        return loopStart;

    // Symmetric window around the user's loopStart. Smaller for Low (fast,
    // conservative) and larger for High (more chances to find a clean splice).
    const int delta = (level == 1) ? 64 : 256;
    const int lo = std::max(1, loopStart - delta);
    const int hi = std::min(loopEnd - 4, loopStart + delta);
    if (hi <= lo) return loopStart;

    // Splice quality is dominated by the sample-to-sample step at the wrap:
    //   prev_at_wrap = data[loopEnd - 1]
    //   next_at_wrap = data[startCand]
    // We minimize both amplitude jump (|next - prev|) and slope mismatch
    // (|outgoing_slope - incoming_slope|). Slope mismatch is more audible
    // because it produces a derivative discontinuity (click) — weight x2.
    const float endValue = data[loopEnd - 1];
    const float endSlope = endValue - data[loopEnd - 2];

    float bestErr = std::numeric_limits<float>::max();
    int bestStart = loopStart;
    for (int cand = lo; cand <= hi; ++cand)
    {
        const float candValue = data[cand];
        const float candSlope = data[cand + 1] - data[cand];
        const float ampErr   = std::abs(candValue - endValue);
        const float slopeErr = std::abs(candSlope - endSlope);
        // Tiny distance penalty keeps refinement near user intent when the
        // boundary error is already low and many candidates are equivalent.
        const float distPenalty = static_cast<float>(std::abs(cand - loopStart)) * 1.0e-6f;
        const float err = ampErr + 2.0f * slopeErr + distPenalty;
        if (err < bestErr)
        {
            bestErr = err;
            bestStart = cand;
        }
    }
    return bestStart;
}

int SamplePlayer::snapToLocalExtremum(const float* data, int centre, int searchRadius,
                                      int boundLo, int boundHi) const
{
    // Centered first-difference is a good proxy for instantaneous slope;
    // minima of its absolute value mark local extrema (peaks/troughs/zero
    // crossings of derivative). For ping-pong reversal these are the points
    // where direction-change introduces no velocity discontinuity.
    const int lo = std::max(boundLo + 1, centre - searchRadius);
    const int hi = std::min(boundHi - 1, centre + searchRadius);
    if (hi <= lo) return juce::jlimit(boundLo, boundHi, centre);

    float bestScore = std::numeric_limits<float>::max();
    int bestIdx = centre;
    for (int i = lo; i <= hi; ++i)
    {
        const float deriv = std::abs(data[i + 1] - data[i - 1]);
        const float distPenalty = static_cast<float>(std::abs(i - centre)) * 1.0e-6f;
        const float score = deriv + distPenalty;
        if (score < bestScore)
        {
            bestScore = score;
            bestIdx = i;
        }
    }
    return bestIdx;
}

// ═══════════════════════════════════════════════════════════════════
// Equal-power crossfade at loop boundary (from useSamplePlayer.ts)
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::applyLoopCrossfade(juce::AudioBuffer<float>& buf, int loopStart, int& loopEnd,
                                      float crossfadeMs, double bufferSampleRate)
{
    int loopLen = loopEnd - loopStart;
    int fadeSamples = std::min(
        static_cast<int>(crossfadeMs / 1000.0f * static_cast<float>(bufferSampleRate)),
        loopLen / 2
    );

    if (fadeSamples < 2) return;

    const float pi_half = juce::MathConstants<float>::halfPi;

    for (int ch = 0; ch < buf.getNumChannels(); ++ch)
    {
        float* d = buf.getWritePointer(ch);
        for (int i = 0; i < fadeSamples; ++i)
        {
            float t     = static_cast<float>(i) / static_cast<float>(fadeSamples);
            float gHead = std::sin(t * pi_half); // 0 → 1
            float gTail = std::cos(t * pi_half); // 1 → 0

            int headIdx = loopStart + i;
            int tailIdx = loopEnd - fadeSamples + i;
            // Blend fading-out tail into fading-in head
            d[headIdx] = d[headIdx] * gHead + d[tailIdx] * gTail;
        }
    }

    // Shorten loop: tail samples are baked into head, never played directly
    loopEnd -= fadeSamples;
}

// ═══════════════════════════════════════════════════════════════════
// Boundary guard samples — make cubic interpolation safe at loop edges
// ═══════════════════════════════════════════════════════════════════
//
// cubicSample(pos) reads data[i1-1, i1, i1+1, i1+2] using ABSOLUTE buffer
// indices. Without intervention, reads at pos ≈ playEnd or pos ≈ playStart
// touch samples outside [playStart, playEnd) which contain unrelated original
// audio. The cubic curve through those samples is discontinuous with what the
// loop logic will play next — audible as a click at every wrap/reverse.
//
// Fix: write guard samples that match the periodic (Loop) or palindromic
// (PingPong) continuation. After this, cubic interpolation produces smooth
// curves across the boundary without any awareness of the loop itself.

void SamplePlayer::writeBoundaryGuards(juce::AudioBuffer<float>& buf,
                                       int playStart, int playEnd, LoopMode mode)
{
    if (mode == LoopMode::OneShot)
        return;

    const int bufLen = buf.getNumSamples();
    const int numCh  = buf.getNumChannels();
    const int loopLen = playEnd - playStart;
    if (bufLen <= 0 || numCh <= 0 || loopLen < kBoundaryGuardSamples * 2)
        return;

    const int G = kBoundaryGuardSamples;

    for (int ch = 0; ch < numCh; ++ch)
    {
        float* d = buf.getWritePointer(ch);

        if (mode == LoopMode::Loop)
        {
            // Backward guards: wrap from the loop's tail end.
            // data[playStart - i] should equal data[playEnd - i] for i = 1..G,
            // so reading at pos = playStart + 0.x sees the natural prior cycle.
            for (int i = 1; i <= G; ++i)
            {
                const int dst = playStart - i;
                const int src = playEnd - i;
                if (dst >= 0 && src >= playStart && src < bufLen)
                    d[dst] = d[src];
            }
            // Forward guards: continue from the loop's head start.
            // The crossfade has been baked into [playStart, playStart + fadeSamples),
            // so copying from there gives the exact post-wrap continuation.
            for (int i = 0; i < G; ++i)
            {
                const int dst = playEnd + i;
                const int src = playStart + i;
                if (dst < bufLen && src < playEnd)
                    d[dst] = d[src];
            }
        }
        else // PingPong
        {
            // Backward guards: mirror forward (palindrome).
            // data[playStart - 1 - k] = data[playStart + k] for k = 0..G-1.
            for (int k = 0; k < G; ++k)
            {
                const int dst = playStart - 1 - k;
                const int src = playStart + k;
                if (dst >= 0 && src < playEnd)
                    d[dst] = d[src];
            }
            // Forward guards: mirror back (palindrome).
            // data[playEnd + k] = data[playEnd - 1 - k] for k = 0..G-1.
            for (int k = 0; k < G; ++k)
            {
                const int dst = playEnd + k;
                const int src = playEnd - 1 - k;
                if (dst < bufLen && src >= playStart)
                    d[dst] = d[src];
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Signal-aware linear normalization:
//   - near-silence: bypass
//   - already-hot material: ceiling cap only
//   - sparse/transient material: percentile-based upward normalize
//   - sustained material: active-RMS normalize with peak ceiling
// The gain is applied only to the measured playback region.
// ═══════════════════════════════════════════════════════════════════

const char* SamplePlayer::normalizeModeName(NormalizeMode mode)
{
    switch (mode)
    {
        case NormalizeMode::Bypass:    return "Bypass";
        case NormalizeMode::PeakCap:   return "PeakCap";
        case NormalizeMode::Transient: return "Transient";
        case NormalizeMode::Sustained: return "Sustained";
    }
    return "?";
}

SamplePlayer::NormalizeAnalysis SamplePlayer::analyzeNormalizeRegion(const juce::AudioBuffer<float>& buf,
                                                                    int regionStart,
                                                                    int regionEnd,
                                                                    double bufferSampleRate) const
{
    NormalizeAnalysis analysis;

    const int rs = juce::jlimit(0, buf.getNumSamples(), regionStart);
    const int re = juce::jlimit(rs, buf.getNumSamples(), regionEnd);
    const int numChannels = buf.getNumChannels();
    if (re <= rs || numChannels <= 0 || bufferSampleRate <= 0.0)
        return analysis;

    const int regionFrames = re - rs;
    analysis.durationSeconds = static_cast<float>(regionFrames / bufferSampleRate);

    std::vector<float> framePeaks;
    framePeaks.reserve(static_cast<size_t>(regionFrames));

    const int blockFrames = juce::jmax(1, static_cast<int>(std::round(bufferSampleRate * 0.050)));
    const float activeThreshold = dbToGain(kActiveBlockThresholdDb);

    double totalSq = 0.0;
    int totalSamples = 0;
    double activeSq = 0.0;
    int activeSamples = 0;
    int activeBlocks = 0;
    int totalBlocks = 0;
    double blockSq = 0.0;
    int framesInBlock = 0;

    for (int i = rs; i < re; ++i)
    {
        float framePeak = 0.0f;
        double frameSq = 0.0;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float sample = buf.getReadPointer(ch)[i];
            const float absSample = std::abs(sample);
            framePeak = std::max(framePeak, absSample);
            frameSq += static_cast<double>(sample) * static_cast<double>(sample);
        }

        framePeaks.push_back(framePeak);
        analysis.peak = std::max(analysis.peak, framePeak);
        totalSq += frameSq;
        totalSamples += numChannels;
        blockSq += frameSq;
        ++framesInBlock;

        if (framesInBlock == blockFrames || i == re - 1)
        {
            ++totalBlocks;
            const float blockRms = static_cast<float>(std::sqrt(
                blockSq / static_cast<double>(framesInBlock * numChannels)));
            if (blockRms > activeThreshold)
            {
                activeSq += blockSq;
                activeSamples += framesInBlock * numChannels;
                ++activeBlocks;
            }
            blockSq = 0.0;
            framesInBlock = 0;
        }
    }

    if (totalSamples <= 0 || framePeaks.empty())
        return analysis;

    analysis.rms = static_cast<float>(std::sqrt(totalSq / static_cast<double>(totalSamples)));
    analysis.activeRms = activeSamples > 0
        ? static_cast<float>(std::sqrt(activeSq / static_cast<double>(activeSamples)))
        : analysis.rms;
    analysis.activeRatio = totalBlocks > 0
        ? static_cast<float>(activeBlocks) / static_cast<float>(totalBlocks)
        : 0.0f;
    analysis.crestDb = gainToDb(analysis.peak / std::max(analysis.rms, 1.0e-9f));

    auto percentilePeaks = framePeaks;
    const size_t n = percentilePeaks.size();
    const size_t percentileIndex = juce::jlimit<size_t>(
        0u,
        n - 1,
        static_cast<size_t>(std::floor(0.999 * static_cast<double>(n - 1))));
    std::nth_element(percentilePeaks.begin(),
                     percentilePeaks.begin() + static_cast<std::ptrdiff_t>(percentileIndex),
                     percentilePeaks.end());
    analysis.percentilePeak = percentilePeaks[percentileIndex];
    analysis.peakToPercentileDb = gainToDb(
        analysis.peak / std::max(analysis.percentilePeak, 1.0e-9f));

    return analysis;
}

SamplePlayer::NormalizeMode SamplePlayer::chooseNormalizeMode(const NormalizeAnalysis& analysis) const
{
    if (analysis.peak <= 0.0f)
        return NormalizeMode::Bypass;

    const float peakDb = gainToDb(analysis.peak);
    const float activeDb = gainToDb(std::max(analysis.activeRms, analysis.rms));
    const float headroomDb = -peakDb;

    if (peakDb <= kNearSilentPeakDb && activeDb <= kNearSilentActiveDb)
        return NormalizeMode::Bypass;

    if (headroomDb < kHotHeadroomDb)
        return NormalizeMode::PeakCap;

    if (analysis.durationSeconds < kShortTransientSeconds
        || analysis.activeRatio < kTransientActiveRatio
        || analysis.crestDb > kTransientCrestDb
        || analysis.peakToPercentileDb > kTransientPeakGapDb)
    {
        return NormalizeMode::Transient;
    }

    return NormalizeMode::Sustained;
}

float SamplePlayer::chooseNormalizeGain(const NormalizeAnalysis& analysis, NormalizeMode mode) const
{
    if (analysis.peak <= 0.0f)
        return 1.0f;

    const float ceilingGain = dbToGain(kNormalizeCeilingDb) / analysis.peak;

    switch (mode)
    {
        case NormalizeMode::Bypass:
            return 1.0f;

        case NormalizeMode::PeakCap:
            return std::min(1.0f, ceilingGain);

        case NormalizeMode::Transient:
        {
            if (analysis.percentilePeak <= 0.0f)
                return std::min(1.0f, ceilingGain);

            const float transientGain = dbToGain(kTransientPercentileTargetDb) / analysis.percentilePeak;
            return std::min(ceilingGain, std::max(1.0f, transientGain));
        }

        case NormalizeMode::Sustained:
        {
            const float reference = std::max(analysis.activeRms, analysis.rms);
            if (reference <= 0.0f)
                return std::min(1.0f, ceilingGain);

            const float sustainedGain = dbToGain(kSustainedTargetDb) / reference;
            return std::min(ceilingGain, sustainedGain);
        }
    }

    return 1.0f;
}

void SamplePlayer::normalizeBuffer(juce::AudioBuffer<float>& buf,
                                   int regionStart,
                                   int regionEnd,
                                   double bufferSampleRate) const
{
    const int rs = juce::jlimit(0, buf.getNumSamples(), regionStart);
    const int re = juce::jlimit(rs, buf.getNumSamples(), regionEnd);
    const int numChannels = buf.getNumChannels();
    if (re <= rs || numChannels <= 0)
        return;

    const NormalizeAnalysis analysis = analyzeNormalizeRegion(buf, rs, re, bufferSampleRate);
    const NormalizeMode mode = chooseNormalizeMode(analysis);
    const float gain = chooseNormalizeGain(analysis, mode);
    if (!std::isfinite(gain) || std::abs(gain - 1.0f) < 1.0e-4f)
        return;

    applyGainToRegion(buf, rs, re, gain);

    samplerDebugLog("normalizeBuffer mode=" + juce::String(normalizeModeName(mode))
                    + " gainDb=" + juce::String(gainToDb(gain), 2)
                    + " peakDb=" + juce::String(gainToDb(analysis.peak), 2)
                    + " p999Db=" + juce::String(gainToDb(analysis.percentilePeak), 2)
                    + " rmsDb=" + juce::String(gainToDb(analysis.rms), 2)
                    + " activeDb=" + juce::String(gainToDb(analysis.activeRms), 2)
                    + " crestDb=" + juce::String(analysis.crestDb, 2)
                    + " peakGapDb=" + juce::String(analysis.peakToPercentileDb, 2)
                    + " activeRatio=" + juce::String(analysis.activeRatio, 3)
                    + " dur=" + juce::String(analysis.durationSeconds, 3));
}

void SamplePlayer::applyGainToRegion(juce::AudioBuffer<float>& buf,
                                     int regionStart,
                                     int regionEnd,
                                     float gain) const
{
    if (!std::isfinite(gain) || std::abs(gain - 1.0f) < 1.0e-4f)
        return;

    const int rs = juce::jlimit(0, buf.getNumSamples(), regionStart);
    const int re = juce::jlimit(rs, buf.getNumSamples(), regionEnd);
    const int numChannels = buf.getNumChannels();
    if (re <= rs || numChannels <= 0)
        return;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        float* data = buf.getWritePointer(ch);
        for (int i = rs; i < re; ++i)
            data[i] *= gain;
    }
}

// ═══════════════════════════════════════════════════════════════════
// Trim leading silence — removes near-zero head with no aesthetic value
// ═══════════════════════════════════════════════════════════════════

void SamplePlayer::trimLeadingSilence(juce::AudioBuffer<float>& buffer) const
{
    const int numSamples = buffer.getNumSamples();
    const int numCh      = buffer.getNumChannels();
    if (numSamples == 0 || numCh == 0) return;

    // Sustained windowed-RMS detection. A single-sample threshold ("first
    // non-zero") trips on isolated noise spikes and low-level DC drift —
    // common in VAE/decoder output — so the trim point would land in the
    // inaudible lead-in. We require ≥3 consecutive short RMS windows above
    // a conservative -50 dB absolute floor before we call it "audio".
    constexpr float threshold        = 0.00316f; // -50 dB absolute
    constexpr int   windowSamples    = 64;       // ~1.5 ms @ 44.1 kHz
    constexpr int   minSustainedWindows = 3;

    const int numWindows = numSamples / windowSamples;
    if (numWindows < minSustainedWindows) return;

    int firstActive = 0;
    int run = 0;
    bool found = false;
    for (int w = 0; w < numWindows; ++w)
    {
        const int base = w * windowSamples;
        double sumSq = 0.0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* p = buffer.getReadPointer(ch) + base;
            for (int i = 0; i < windowSamples; ++i)
            {
                double s = static_cast<double>(p[i]);
                sumSq += s * s;
            }
        }
        const float rms = std::sqrt(static_cast<float>(sumSq
                                    / static_cast<double>(windowSamples * numCh)));
        if (rms > threshold)
        {
            ++run;
            if (run >= minSustainedWindows)
            {
                firstActive = (w - minSustainedWindows + 1) * windowSamples;
                found = true;
                break;
            }
        }
        else
        {
            run = 0;
        }
    }

    if (!found || firstActive == 0) return; // nothing to trim

    // Truncate: copy from firstActive onward into a new buffer
    const int newLen = numSamples - firstActive;
    juce::AudioBuffer<float> trimmed(numCh, newLen);
    for (int ch = 0; ch < numCh; ++ch)
        trimmed.copyFrom(ch, 0, buffer, ch, firstActive, newLen);

    buffer = std::move(trimmed);
}

void SamplePlayer::trimTrailingSilence(juce::AudioBuffer<float>& buffer) const
{
    const int numSamples = buffer.getNumSamples();
    const int numCh      = buffer.getNumChannels();
    if (numSamples == 0 || numCh == 0) return;

    // Symmetric counterpart to trimLeadingSilence(): drop a sustained near-
    // silent TAIL so engines that traverse the whole buffer end at real content.
    // The granular/freeze engine maps scan 0..1 across the entire sample and has
    // no internal playhead, so a dead post-content field (the diffusion models
    // generate the requested duration even when the sound itself is short, e.g.
    // ~1 s of bell + ~10 s of near-silence at an 11 s request) parks the read
    // point in pure silence — and the waveform/playhead show a flat tail. Same
    // windowed-RMS / -50 dB floor / 3-window sustain test as the leading trim,
    // scanned for the LAST qualifying run. A ~46 ms margin past that run is kept
    // to preserve natural decay/release and keep the cut below -50 dB (no click).
    // Idempotent, and a no-op when content already runs to the end.
    constexpr float threshold           = 0.00316f; // -50 dB absolute (matches leading trim)
    constexpr int   windowSamples       = 64;       // ~1.5 ms @ 44.1 kHz
    constexpr int   minSustainedWindows = 3;
    constexpr int   marginWindows       = 32;        // ~46 ms tail kept past last active run

    const int numWindows = numSamples / windowSamples;
    if (numWindows < minSustainedWindows) return;

    int lastRunEnd = -1;   // exclusive window index of the end of the last sustained run
    int run = 0;
    for (int w = 0; w < numWindows; ++w)
    {
        const int base = w * windowSamples;
        double sumSq = 0.0;
        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* p = buffer.getReadPointer(ch) + base;
            for (int i = 0; i < windowSamples; ++i)
            {
                double s = static_cast<double>(p[i]);
                sumSq += s * s;
            }
        }
        const float rms = std::sqrt(static_cast<float>(sumSq
                                    / static_cast<double>(windowSamples * numCh)));
        if (rms > threshold)
        {
            ++run;
            if (run >= minSustainedWindows)
                lastRunEnd = w + 1;
        }
        else
        {
            run = 0;
        }
    }

    if (lastRunEnd < 0) return; // no sustained content — leave a genuinely quiet buffer alone

    int newLen = (lastRunEnd + marginWindows) * windowSamples;
    newLen = juce::jmin(numSamples, newLen);
    if (newLen >= numSamples) return; // content runs to (near) the end — nothing to trim

    // Truncate in place; not on the audio thread (load path), so realloc is fine.
    buffer.setSize(numCh, newLen, /*keepExistingContent*/ true,
                   /*clearExtraSpace*/ false, /*avoidReallocating*/ true);
}
