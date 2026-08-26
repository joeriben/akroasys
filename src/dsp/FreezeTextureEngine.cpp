#include "FreezeTextureEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

// See the declaration in FreezeTextureEngine.h: process-wide so that snapshots
// from DIFFERENT engine instances never collide on a generation number.
std::atomic<juce::uint64> FreezeTextureEngine::nextGeneration_ { 1 };

namespace
{
double deterministicUnit(int index, std::uint32_t salt)
{
    std::uint32_t x = static_cast<std::uint32_t>(index) + salt;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return static_cast<double>(x) / static_cast<double>(std::numeric_limits<std::uint32_t>::max());
}

double deterministicBipolar(int index, std::uint32_t salt)
{
    return deterministicUnit(index, salt) * 2.0 - 1.0;
}
}

void FreezeTextureEngine::prepare(double sampleRate, int samplesPerBlock)
{
    playbackSampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlockSize_ = samplesPerBlock;
    positionSmoothCoeff_ = 1.0f - std::exp(-1.0f / static_cast<float>(playbackSampleRate_ * 0.020));
}

void FreezeTextureEngine::reset()
{
    // Every path into reset() starts at T5ynthProcessor::releaseResources(),
    // which takes an explicit ScopedLock(getCallbackLock()) around it: the
    // master instance directly, the per-voice ones through VoiceManager::reset()
    // -> SynthVoice::reset(). Per releaseResources' JUCE contract the audio
    // thread is stopped besides, on every format — so this publish/clear
    // (through publishSnapshot()/publishMorphFromSnapshot()'s real atomics)
    // can never race a reader regardless of lock or atomics, and releasing the
    // retained morph snapshot here is safe even if it is the last reference.
    publishSnapshot(nullptr);
    publishMorphFromSnapshot(nullptr);
    retiredPublished_.reset();
    retiredMorphFrom_.reset();
    resetCloud(cloud_);
    resetCloud(morphCloud_);
    morphActive_.store(false, std::memory_order_relaxed);
    morphAlpha_ = 1.0f;
    morphIncrement_ = 0.0f;
    transposeRatio_ = 1.0;
    glideTargetRatio_ = 1.0;
    glideRatioIncr_ = 0.0;
    glideSamplesLeft_ = 0;
    pitchModFactor_ = 1.0f;
    targetPosition_ = 0.5f;
    smoothedPosition_ = 0.5f;
    currentPosition_ = 0.5f;
}

void FreezeTextureEngine::loadBuffer(const juce::AudioBuffer<float>& buffer, double bufferSampleRate)
{
    // Combined compute+publish convenience — see this function's doc comment in
    // the header for who may call it and why. Kept as a single call so its only
    // production caller (serviceSamplerReprepare, already inside the lock) needs
    // no change; production sites that must NOT hold the lock across the mixdown
    // call prepareBufferLoad()/applyPreparedBufferLoad() separately instead.
    applyPreparedBufferLoad(prepareBufferLoad(buffer, bufferSampleRate));
}

FreezeTextureEngine::SnapshotPtr FreezeTextureEngine::prepareBufferLoad(const juce::AudioBuffer<float>& buffer,
                                                                        double bufferSampleRate)
{
    if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0)
        return nullptr;

    auto snapshot = std::make_shared<Snapshot>();
    snapshot->sampleRate = bufferSampleRate > 0.0 ? bufferSampleRate : 44100.0;
    snapshot->generation = nextGeneration_.fetch_add(1, std::memory_order_relaxed);
    snapshot->samples.resize(static_cast<size_t>(buffer.getNumSamples()), 0.0f);

    const int channels = buffer.getNumChannels();
    const float invChannels = 1.0f / static_cast<float>(channels);
    for (int ch = 0; ch < channels; ++ch)
    {
        const float* src = buffer.getReadPointer(ch);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            snapshot->samples[static_cast<size_t>(i)] += src[i] * invChannels;
    }

    return snapshot;
}

void FreezeTextureEngine::applyPreparedBufferLoad(SnapshotPtr snapshot)
{
    // See this function's doc comment in the header: publishSnapshot() below
    // is a real atomic_store_explicit (release) — the atomics are what cover
    // CLAP; call this under an explicit ScopedLock(getCallbackLock()) too,
    // which is real and necessary on Standalone/VST3/AU and for this class's
    // non-atomic state, but is not sufficient by itself.
    publishSnapshot(std::move(snapshot));
}

void FreezeTextureEngine::shareBufferFrom(const FreezeTextureEngine& master)
{
    publishSnapshot(master.loadPublishedSnapshot());
    textureLengthMs_ = master.textureLengthMs_;
    textureMode_ = master.textureMode_;
    stereoWidth_ = master.stereoWidth_;
    resetCloud(cloud_);
    resetCloud(morphCloud_);
    // Adopt the buffer cleanly — no crossfade. morphFromSnapshot_ is left
    // intact (shareBufferFrom may run on the audio thread via the per-block
    // redistribution; releasing a snapshot there could free on the audio
    // thread). It is released off-thread on the next morphToBufferFrom/reset.
    morphActive_.store(false, std::memory_order_relaxed);
}

void FreezeTextureEngine::morphToBufferFrom(const FreezeTextureEngine& master, float morphMs)
{
    // Deferred, OFF-AUDIO-THREAD release of the snapshots retired by the PREVIOUS
    // morph (see the retire-bin note in the header). Any audio-thread per-sample
    // local that briefly aliased these is long gone, so freeing here is safe and
    // keeps every snapshot free off the audio thread.
    retiredPublished_.reset();
    retiredMorphFrom_.reset();

    auto newSnap = master.loadPublishedSnapshot();
    if (newSnap == nullptr)
        return;  // master has no audio; leave this voice's current buffer untouched

    // Adopt master's texture configuration (matches shareBufferFrom).
    textureLengthMs_ = master.textureLengthMs_;
    textureMode_ = master.textureMode_;
    stereoWidth_ = master.stereoWidth_;

    auto curSnap = loadPublishedSnapshot();

    // Same buffer we already play (or are already morphing to) → no-op. Keeps
    // the per-block redistribution cheap and never restarts an in-flight morph
    // (mirrors WavetableOscillator's generation guards).
    if (curSnap != nullptr && curSnap->generation == newSnap->generation)
    {
        publishSnapshot(newSnap);  // same object; harmless
        return;
    }

    // Past this point we overwrite publishedSnapshot_ and/or morphFromSnapshot_ on
    // a voice the audio thread is actively rendering. Park BOTH current snapshots
    // in the message-thread-only retire bin so an audio-thread per-sample local
    // (processSampleStereo's `snapshot`/`oldSnap`) can never be the last reference
    // — i.e. the std::vector<float> is never freed on the audio thread. Released at
    // the top of the next morphToBufferFrom. (curSnap may be null on a first buffer;
    // parking a null is harmless.)
    retiredPublished_ = curSnap;
    retiredMorphFrom_ = loadMorphFromSnapshot();

    // Genuine new buffer. Instant adopt when there is nothing to fade from or
    // no crossfade was requested.
    if (curSnap == nullptr || morphMs <= 0.0f)
    {
        publishSnapshot(newSnap);
        resetCloud(cloud_);
        resetCloud(morphCloud_);
        publishMorphFromSnapshot(nullptr);
        morphActive_.store(false, std::memory_order_relaxed);
        return;
    }

    // Crossfade. Run two grain clouds and ramp an equal-power mix; at alpha=1 the
    // old gain is exactly 0, so the old cloud stops without a click. Choosing the
    // fade-FROM source ("promote the dominant side", mirrors WavetableOscillator):
    //   • First morph, or the primary cloud is the louder side (alpha >= 0.5):
    //     hand the in-flight primary grains to the morph cloud — they keep reading
    //     the old (now retained) snapshot — and start the primary fresh on the new.
    //   • Already morphing AND the old cloud is still louder (alpha < 0.5): keep
    //     the old cloud and its retained snapshot as the fade-from, discard the
    //     quieter in-flight primary, and start the new snapshot in the primary.
    // This bounds a re-morph discontinuity to the quieter side instead of dropping
    // the loud one. morphFromSnapshot_ is released only off the audio thread (this
    // method runs on the message/reprepare thread via the allowMorph gate).
    const bool alreadyMorphing = morphActive_.load(std::memory_order_relaxed);
    if (alreadyMorphing && morphAlpha_ < 0.5f)
    {
        // Old cloud dominant — keep morphCloud_ + morphFromSnapshot_ as-is.
        resetCloud(cloud_);
        publishSnapshot(newSnap);
    }
    else
    {
        morphCloud_ = cloud_;          // value copy of POD cloud — thread-safe
        resetCloud(cloud_);
        publishMorphFromSnapshot(curSnap);
        publishSnapshot(newSnap);
    }
    morphAlpha_ = 0.0f;
    const int morphSamples = std::max(1, static_cast<int>(std::round(
        static_cast<double>(morphMs) * 0.001 * playbackSampleRate_)));
    morphIncrement_ = 1.0f / static_cast<float>(morphSamples);
    // Release-store LAST: publishes the whole setup above to the audio thread's
    // acquire-load gate in processSampleStereo.
    morphActive_.store(true, std::memory_order_release);
}

bool FreezeTextureEngine::hasAudio() const
{
    auto snapshot = loadPublishedSnapshot();
    return snapshot != nullptr && snapshot->samples.size() >= 8;
}

void FreezeTextureEngine::retrigger()
{
    resetCloud(cloud_);
    resetCloud(morphCloud_);
    // retrigger() runs on the audio thread (noteOn), so it must not release a
    // snapshot. Just stop any in-flight morph; morphFromSnapshot_ is released
    // off-thread on the next morphToBufferFrom/reset.
    morphActive_.store(false, std::memory_order_relaxed);
    smoothedPosition_ = targetPosition_;
    currentPosition_ = targetPosition_;
}

void FreezeTextureEngine::setPosition(float position)
{
    targetPosition_ = juce::jlimit(0.0f, 1.0f, position);
}

void FreezeTextureEngine::setTextureMode(int mode)
{
    const int clamped = juce::jlimit(0, 3, mode);
    if (clamped == textureMode_)
        return;

    textureMode_ = clamped;
    textureLengthMs_ = getTextureConfig().defaultLengthMs;
}

void FreezeTextureEngine::setTextureLengthMs(float ms)
{
    textureLengthMs_ = juce::jlimit(120.0f, 700.0f, ms);
}

void FreezeTextureEngine::setStereoWidth(float width)
{
    stereoWidth_ = juce::jlimit(0.0f, 1.0f, width);
}

void FreezeTextureEngine::setTransposeRatio(double ratio)
{
    transposeRatio_ = juce::jlimit(0.0625, 16.0, ratio);
    glideSamplesLeft_ = 0;
}

void FreezeTextureEngine::glideToRatio(double targetRatio, float durationMs)
{
    const int samples = std::max(1, static_cast<int>(durationMs * 0.001f * static_cast<float>(playbackSampleRate_)));
    glideTargetRatio_ = juce::jlimit(0.0625, 16.0, targetRatio);
    glideRatioIncr_ = (glideTargetRatio_ - transposeRatio_) / static_cast<double>(samples);
    glideSamplesLeft_ = samples;
}

void FreezeTextureEngine::setPitchModulation(float factor)
{
    pitchModFactor_ = juce::jlimit(0.0625f, 16.0f, factor);
}

float FreezeTextureEngine::processSample()
{
    float left = 0.0f;
    float right = 0.0f;
    processSampleStereo(left, right);
    return 0.5f * (left + right);
}

void FreezeTextureEngine::processSampleStereo(float& left, float& right)
{
    auto snapshot = loadPublishedSnapshot();
    if (snapshot == nullptr || snapshot->samples.size() < 8)
    {
        left = 0.0f;
        right = 0.0f;
        return;
    }

    if (glideSamplesLeft_ > 0)
    {
        transposeRatio_ += glideRatioIncr_;
        --glideSamplesLeft_;
        if (glideSamplesLeft_ == 0)
            transposeRatio_ = glideTargetRatio_;
    }

    smoothedPosition_ += (targetPosition_ - smoothedPosition_) * positionSmoothCoeff_;
    currentPosition_ = juce::jlimit(0.0f, 1.0f, smoothedPosition_);

    const double effectiveRatio = juce::jlimit(0.0625, 16.0,
        transposeRatio_ * static_cast<double>(pitchModFactor_));

    const int grainDurationSamples = getGrainDurationSamples();

    // Primary cloud → the new/current buffer. Identical to the historical
    // single-cloud path when no morph is running.
    float newLeft = 0.0f;
    float newRight = 0.0f;
    renderCloud(cloud_, *snapshot, effectiveRatio, grainDurationSamples, newLeft, newRight);

    // Acquire-load the gate FIRST: pairs with the release-store in
    // morphToBufferFrom so the morph setup (morphCloud_, morphFromSnapshot_,
    // morphAlpha_, morphIncrement_) is visible here on the first morph.
    if (! morphActive_.load(std::memory_order_acquire))
    {
        left = newLeft;
        right = newRight;
        return;
    }

    // Crossfade window: also spray the retained old buffer and equal-power mix.
    // All of this is gated on morphActive_, so steady state costs nothing.
    auto oldSnap = loadMorphFromSnapshot();
    if (oldSnap != nullptr && oldSnap->samples.size() >= 8)
    {
        float oldLeft = 0.0f;
        float oldRight = 0.0f;
        renderCloud(morphCloud_, *oldSnap, effectiveRatio, grainDurationSamples, oldLeft, oldRight);

        const float g = juce::jlimit(0.0f, 1.0f, morphAlpha_);
        const float newGain = std::sin(g * juce::MathConstants<float>::halfPi);
        const float oldGain = std::cos(g * juce::MathConstants<float>::halfPi);
        left  = newLeft  * newGain + oldLeft  * oldGain;
        right = newRight * newGain + oldRight * oldGain;
    }
    else
    {
        left = newLeft;
        right = newRight;
    }

    morphAlpha_ += morphIncrement_;
    if (morphAlpha_ >= 1.0f)
    {
        morphAlpha_ = 1.0f;
        // Audio-thread store; no data is published through this transition (we
        // only stop reading morphCloud_), so relaxed suffices.
        morphActive_.store(false, std::memory_order_relaxed);
        // morphFromSnapshot_ intentionally retained (we are on the audio
        // thread); released off-thread on the next morphToBufferFrom/reset.
    }
}

// Real atomic free-function access (atomic_load/store_explicit, acquire/
// release) — see publishedSnapshot_/morphFromSnapshot_'s declarations in the
// header for why: getCallbackLock() does not cover the CLAP build (no lock
// around processBlock there), so the atomics, not the lock, are the guard
// that holds on every format. The caller's getCallbackLock() (explicit
// off-thread, ambient via processBlock on Standalone/VST3/AU) is a second,
// real guard on those three formats. Called unprotected ONLY by the
// single-threaded tools-dir *.cpp harnesses noted on loadBuffer()'s doc
// comment, where neither the atomics nor a lock are needed because nothing
// else touches that instance.
FreezeTextureEngine::SnapshotPtr FreezeTextureEngine::loadPublishedSnapshot() const
{
    return std::atomic_load_explicit(&publishedSnapshot_, std::memory_order_acquire);
}

void FreezeTextureEngine::publishSnapshot(SnapshotPtr snapshot)
{
    std::atomic_store_explicit(&publishedSnapshot_, snapshot, std::memory_order_release);
}

FreezeTextureEngine::SnapshotPtr FreezeTextureEngine::loadMorphFromSnapshot() const
{
    return std::atomic_load_explicit(&morphFromSnapshot_, std::memory_order_acquire);
}

void FreezeTextureEngine::publishMorphFromSnapshot(SnapshotPtr snapshot)
{
    std::atomic_store_explicit(&morphFromSnapshot_, snapshot, std::memory_order_release);
}

void FreezeTextureEngine::resetCloud(GrainCloud& cloud)
{
    for (auto& grain : cloud.grains)
        grain = {};
    cloud.spawnSamplesUntilNext = 0;
    cloud.nextGrainSlot = 0;
    cloud.spawnIndex = 0;
}

void FreezeTextureEngine::renderCloud(GrainCloud& cloud,
                                      const Snapshot& snapshot,
                                      double effectiveRatio,
                                      int grainDurationSamples,
                                      float& left,
                                      float& right)
{
    if (cloud.spawnSamplesUntilNext <= 0)
    {
        spawnGrain(cloud, snapshot, effectiveRatio);
        cloud.spawnSamplesUntilNext += getNextHopSamples(cloud, grainDurationSamples);
    }
    --cloud.spawnSamplesUntilNext;

    float sum = 0.0f;
    float sumRight = 0.0f;
    float weightSum = 0.0f;
    for (auto& grain : cloud.grains)
    {
        float weight = 0.0f;
        float grainLeft = 0.0f;
        float grainRight = 0.0f;
        processGrain(grain, snapshot, grainLeft, grainRight, weight);
        sum += grainLeft;
        sumRight += grainRight;
        weightSum += weight;
    }

    if (weightSum <= 1.0e-5f)
    {
        left = 0.0f;
        right = 0.0f;
        return;
    }

    left = sum / weightSum;
    right = sumRight / weightSum;
}

FreezeTextureEngine::TextureConfig FreezeTextureEngine::getTextureConfig() const
{
    switch (textureMode_)
    {
        case 0:  return { 380.0f, 10, 0.10f,  0.0f,  6.0f, 0.5f }; // Hold
        case 2:  return { 190.0f,  8, 0.55f, 22.0f, 28.0f, 3.0f }; // Air
        case 3:  return { 145.0f,  8, 0.75f, 42.0f, 42.0f, 4.5f }; // Cloud
        case 1:
        default: return { 260.0f,  9, 0.35f, 10.0f, 18.0f, 2.0f }; // Silk
    }
}

void FreezeTextureEngine::spawnGrain(GrainCloud& cloud, const Snapshot& snapshot, double effectiveRatio)
{
    auto& grain = cloud.grains[static_cast<size_t>(cloud.nextGrainSlot)];
    cloud.nextGrainSlot = (cloud.nextGrainSlot + 1) % kMaxGrains;

    const auto config = getTextureConfig();
    const int index = cloud.spawnIndex++;
    const int sampleCount = static_cast<int>(snapshot.samples.size());
    const double sourceRate = snapshot.sampleRate > 0.0 ? snapshot.sampleRate : 44100.0;
    const double durationJitter = 0.04 + 0.12 * static_cast<double>(config.blur);
    const double durationScale = 1.0 + deterministicBipolar(index, 0x3d20adeau) * durationJitter;
    const int durationSamples = std::max(8, static_cast<int>(std::round(
        static_cast<double>(getGrainDurationSamples()) * durationScale)));
    const double cents = deterministicBipolar(index, 0x79b4c3d1u) * static_cast<double>(config.pitchCents);
    const double readStep = (sourceRate / playbackSampleRate_)
        * effectiveRatio
        * std::pow(2.0, cents / 1200.0);
    const double readSpan = std::max(1.0, static_cast<double>(durationSamples - 1) * readStep);
    const double centreSample = 2.0 + currentPosition_ * static_cast<double>(std::max(1, sampleCount - 5));

    const double motionPhase = static_cast<double>(index) * 0.071;
    const double motionSamples = std::sin(motionPhase) * sourceRate * static_cast<double>(config.motionMs) * 0.001
        + std::sin(motionPhase * 0.37 + 1.91) * sourceRate * static_cast<double>(config.motionMs) * 0.00045;
    const double spreadSamples = std::min(sourceRate * static_cast<double>(config.spreadMs) * 0.001,
                                          readSpan * (0.08 + 0.34 * static_cast<double>(config.blur)));
    const double deterministicOffset = deterministicBipolar(index, 0x9e3779b9u) * spreadSamples;
    const double stereoOffset = static_cast<double>(stereoWidth_)
        * sourceRate
        * (0.0015 + 0.010 * static_cast<double>(config.blur))
        * deterministicBipolar(index, 0x45d9f3bu);

    const double proposedStart = centreSample + motionSamples + deterministicOffset - readSpan * 0.5;
    const double minStart = 1.0;
    const double maxStart = std::max(minStart, static_cast<double>(sampleCount) - 2.0 - readSpan);
    const double start = juce::jlimit(minStart, maxStart, proposedStart);

    const float pan = static_cast<float>(deterministicBipolar(index, 0xa511e9b3u))
        * stereoWidth_
        * (0.15f + 0.30f * config.blur);
    const float leftGain = juce::jlimit(0.65f, 1.15f, 1.0f - pan);
    const float rightGain = juce::jlimit(0.65f, 1.15f, 1.0f + pan);

    grain.active = true;
    grain.ageSamples = 0;
    grain.durationSamples = durationSamples;
    grain.readPosLeft = juce::jlimit(minStart, maxStart, start - stereoOffset);
    grain.readPosRight = juce::jlimit(minStart, maxStart, start + stereoOffset);
    grain.readStep = readStep;
    grain.ampLeft = leftGain;
    grain.ampRight = rightGain;
}

void FreezeTextureEngine::processGrain(Grain& grain,
                                       const Snapshot& snapshot,
                                       float& left,
                                       float& right,
                                       float& weight) const
{
    if (!grain.active)
    {
        left = 0.0f;
        right = 0.0f;
        weight = 0.0f;
        return;
    }

    const float phase = juce::jlimit(0.0f, 1.0f,
        static_cast<float>(grain.ageSamples) / static_cast<float>(std::max(1, grain.durationSamples - 1)));
    weight = 0.5f - 0.5f * std::cos(juce::MathConstants<float>::twoPi * phase);
    left = cubicSample(snapshot, grain.readPosLeft) * weight * grain.ampLeft;
    right = cubicSample(snapshot, grain.readPosRight) * weight * grain.ampRight;

    grain.readPosLeft += grain.readStep;
    grain.readPosRight += grain.readStep;
    ++grain.ageSamples;
    if (grain.ageSamples >= grain.durationSamples)
        grain.active = false;
}

float FreezeTextureEngine::cubicSample(const Snapshot& snapshot, double position) const
{
    const int size = static_cast<int>(snapshot.samples.size());
    if (size <= 0)
        return 0.0f;

    position = juce::jlimit(0.0, static_cast<double>(size - 1), position);
    int i1 = static_cast<int>(std::floor(position));
    const float t = static_cast<float>(position - static_cast<double>(i1));

    const int i0 = juce::jmax(0, i1 - 1);
    i1 = juce::jlimit(0, size - 1, i1);
    const int i2 = juce::jmin(size - 1, i1 + 1);
    const int i3 = juce::jmin(size - 1, i1 + 2);

    const float p0 = snapshot.samples[static_cast<size_t>(i0)];
    const float p1 = snapshot.samples[static_cast<size_t>(i1)];
    const float p2 = snapshot.samples[static_cast<size_t>(i2)];
    const float p3 = snapshot.samples[static_cast<size_t>(i3)];

    const float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
    const float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
    const float c = -0.5f * p0 + 0.5f * p2;
    return ((a * t + b) * t + c) * t + p1;
}

int FreezeTextureEngine::getGrainDurationSamples() const
{
    const auto requested = static_cast<int>(std::round(
        static_cast<double>(textureLengthMs_) * 0.001 * playbackSampleRate_));
    return std::max(8, requested);
}

int FreezeTextureEngine::getNextHopSamples(const GrainCloud& cloud, int grainDurationSamples) const
{
    const auto config = getTextureConfig();
    const int baseHop = std::max(1, grainDurationSamples / std::max(1, config.overlap));
    const double jitterDepth = 0.04 + 0.14 * static_cast<double>(config.blur);
    const double jitter = 1.0 + deterministicBipolar(cloud.spawnIndex, 0x632be59bu) * jitterDepth;
    return std::max(1, static_cast<int>(std::round(static_cast<double>(baseHop) * jitter)));
}
