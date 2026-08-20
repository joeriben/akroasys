#include "WavetableOscillator.h"
#include <algorithm>
#include <limits>

void WavetableOscillator::prepare(double sr, int /*samplesPerBlock*/)
{
    sampleRate = sr;
    phase = 0.0;
    smoothedScan = 0.0f;
    // ~5ms time constant for scan smoothing
    scanSmoothCoeff = 1.0f - std::exp(-1.0f / static_cast<float>(sr * 0.005));
    // Invalidate mip-level cache: it depends on sampleRate via invBaseFreq.
    lastMipFreq_ = std::numeric_limits<float>::quiet_NaN();
}

void WavetableOscillator::reset()
{
    phase = 0.0;
    smoothedScan = 0.0f;
    if (!morphActive_)
        morphAlpha_ = 1.0f;
}

// Real atomic free-function access (atomic_load/store_explicit, acquire/
// release). The full contract lives on publishedMipData_'s declaration in the
// header: getCallbackLock() does not cover the CLAP build (no lock around
// processBlock there), so the atomics, not the lock, are the guard that holds
// on every format. Readers: hasFrames(), getNumFrames(), snapshotLevel0Frames(),
// snapshotAdditiveBank(), processSample() (audio thread, only while adopting a
// first bank — see its own comment), shareFramesFrom()/morphToFramesFrom()
// (cross-instance, reading a MASTER's published data). Writers:
// applyPreparedMipData() (the prepareXXXFrames()/prepareMipLevels() family and
// setAdditiveBank()), adoptMipData(), beginMorphToMipData(), shareFramesFrom(),
// morphToFramesFrom(). Called unprotected ONLY by the single-threaded
// tools-dir *.cpp harnesses noted on extractFramesFromBuffer()'s doc comment,
// where neither the atomics nor a lock are needed because nothing else
// touches that instance.
WavetableOscillator::MipDataPtr WavetableOscillator::loadPublishedMipData() const
{
    return std::atomic_load_explicit(&publishedMipData_, std::memory_order_acquire);
}

bool WavetableOscillator::hasFrames() const
{
    if (activeMorphMipData_ != nullptr)
        return activeMorphMipData_->numFrames > 0;

    auto published = loadPublishedMipData();
    return published != nullptr && published->numFrames > 0;
}

int WavetableOscillator::getNumFrames() const
{
    if (activeMorphMipData_ != nullptr)
        return activeMorphMipData_->numFrames;

    auto published = loadPublishedMipData();
    return published != nullptr ? published->numFrames : 0;
}

bool WavetableOscillator::snapshotLevel0Frames(std::vector<float>& outFlat,
                                               int& outFrameSize,
                                               int& outNumFrames) const
{
    auto published = loadPublishedMipData();
    if (published == nullptr || published->numFrames == 0
        || published->frames.empty() || published->frames[0].empty())
        return false;

    const auto& level0 = published->frames[0];
    outFrameSize = FRAME_SIZE;
    outNumFrames = (int) level0.size();

    outFlat.clear();
    outFlat.reserve(static_cast<size_t>(outFrameSize) * static_cast<size_t>(outNumFrames));
    for (const auto& frame : level0)
    {
        if ((int) frame.size() != outFrameSize) return false;  // bank shape mismatch
        outFlat.insert(outFlat.end(), frame.begin(), frame.end());
    }
    return true;
}

bool WavetableOscillator::snapshotAdditiveBank(std::vector<std::vector<AdditivePartial>>& outSets,
                                               float& outGain) const
{
    auto published = loadPublishedMipData();
    if (published == nullptr || !published->isAdditive || published->partialSets.empty())
        return false;

    outSets = published->partialSets;
    outGain = published->additiveGain;
    return true;
}

void WavetableOscillator::syncSharedConfigFrom(const WavetableOscillator& source)
{
    // Copy traversal/morph configuration, but keep per-voice runtime state.
    // scanNow reads only smoothedScan (see processSample) — already per-voice
    // continuous state untouched by this sync — so a transport change (e.g.
    // AutoScan toggled, or a DCO<->neural regeneration) needs no compensation
    // here: the smoother just slews to the new target over its normal 5 ms,
    // exactly like any other scan-target change.
    autoScan_ = source.autoScan_;
    autoScanIncr_ = source.autoScanIncr_;
    autoScanStartPos_ = source.autoScanStartPos_;
    autoScanLoopStart_ = source.autoScanLoopStart_;
    autoScanLoopEnd_ = source.autoScanLoopEnd_;
    autoScanLoopMode_ = source.autoScanLoopMode_;
    morphTimeMs_ = source.morphTimeMs_;
}

void WavetableOscillator::adoptMipData(MipDataPtr mipData, bool seedAdditivePhase)
{
    // BY VALUE, not const&: the morph-complete caller passes targetMorphMipData_,
    // and the targetMorphMipData_.reset() below would null a reference bound to it
    // BEFORE the seed block dereferences mipData. A copy (cheap atomic refcount,
    // audio-thread safe) keeps mipData alive independent of that reset.
    if (mipData == nullptr)
        return;

    // Atomic publish (release) — see loadPublishedMipData()'s comment for the
    // full contract; the atomics, not getCallbackLock() alone, cover CLAP.
    std::atomic_store_explicit(&publishedMipData_, mipData, std::memory_order_release);
    activeMorphMipData_ = mipData;
    targetMorphMipData_.reset();
    morphAlpha_ = 1.0f;
    morphIncrement_ = 0.0f;
    morphActive_ = false;
    // Mip-level cache key only includes frequency; numLevels is currently always 8 across banks,
    // but invalidate defensively in case a future bank emits a different level count.
    lastMipFreq_ = std::numeric_limits<float>::quiet_NaN();
    // Fresh adopt of an additive bank starts every partial at its authored phase.
    // The morph-complete path passes seedAdditivePhase=false: it has already carried
    // the running target phases into activeAddPhase_, so re-seeding would click.
    if (seedAdditivePhase && mipData->isAdditive)
        seedAdditivePhases(activeAddPhase_, *mipData);
}

void WavetableOscillator::beginMorphToMipData(const MipDataPtr& mipData)
{
    if (mipData == nullptr)
        return;

    // Atomic publish (release) — see loadPublishedMipData()'s comment for the
    // full contract; the atomics, not getCallbackLock() alone, cover CLAP.
    std::atomic_store_explicit(&publishedMipData_, mipData, std::memory_order_release);

    if (activeMorphMipData_ == nullptr || activeMorphMipData_->numFrames == 0)
    {
        adoptMipData(mipData);
        return;
    }

    if (activeMorphMipData_->generation == mipData->generation
        || (targetMorphMipData_ != nullptr && targetMorphMipData_->generation == mipData->generation))
    {
        if (!morphActive_ && activeMorphMipData_->generation == mipData->generation)
            adoptMipData(mipData);
        return;
    }

    if (morphTimeMs_ <= 0.0f)
    {
        adoptMipData(mipData);
        return;
    }

    if (morphActive_ && targetMorphMipData_ != nullptr)
    {
        // A second bank arrives mid-morph. If the in-flight target is dominant it
        // becomes the new active — and for an ADDITIVE target its RUNNING phase is
        // in targetAddPhase_, which the seed at the tail of this function is about
        // to overwrite with the NEW target. Carry it into activeAddPhase_ first
        // (the same phase-continuity the completion branch in processSample does),
        // or the promoted bank restarts at a foreign phase = a click on the held
        // note — the very invariant this path exists to preserve. (Table target:
        // the array copy is harmless, activeAddPhase_ is unused for a wavetable.)
        if (morphAlpha_ >= 0.5f)
        {
            activeMorphMipData_ = targetMorphMipData_;
            activeAddPhase_ = targetAddPhase_;
        }
    }

    if (activeMorphMipData_ == nullptr
        || activeMorphMipData_->generation == mipData->generation)
    {
        adoptMipData(mipData);
        return;
    }

    targetMorphMipData_ = mipData;
    morphAlpha_ = 0.0f;
    const int morphSamples = std::max(1, static_cast<int>(std::round(
        static_cast<double>(morphTimeMs_) * 0.001 * sampleRate)));
    morphIncrement_ = 1.0f / static_cast<float>(morphSamples);
    morphActive_ = true;
    // A new inharmonic bank fades IN from its authored phase offsets (wetGain
    // starts at 0, so the seed is inaudible); the equal-power crossfade then
    // follows the held note table<->additive exactly as it does table<->table.
    if (mipData->isAdditive)
        seedAdditivePhases(targetAddPhase_, *mipData);
}

void WavetableOscillator::shareFramesFrom(const WavetableOscillator& source)
{
    sharedSource_ = &source;
    syncSharedConfigFrom(source);

    auto mipData = source.loadPublishedMipData();
    if (mipData == nullptr)
        return;

    const bool sameActive = activeMorphMipData_ != nullptr
        && activeMorphMipData_->generation == mipData->generation;
    if (sameActive && !morphActive_)
    {
        // Atomic publish (release) — see loadPublishedMipData()'s comment for
        // the full contract; the atomics, not getCallbackLock() alone, cover CLAP.
        std::atomic_store_explicit(&publishedMipData_, mipData, std::memory_order_release);
        return;
    }

    adoptMipData(mipData);
}

void WavetableOscillator::morphToFramesFrom(const WavetableOscillator& source)
{
    sharedSource_ = &source;
    syncSharedConfigFrom(source);

    auto mipData = source.loadPublishedMipData();
    if (mipData == nullptr)
        return;

    // Atomic publish (release) — see loadPublishedMipData()'s comment for the
    // full contract; the atomics, not getCallbackLock() alone, cover CLAP.
    std::atomic_store_explicit(&publishedMipData_, mipData, std::memory_order_release);

    const bool sameActive = activeMorphMipData_ != nullptr
        && activeMorphMipData_->generation == mipData->generation;
    const bool sameTarget = targetMorphMipData_ != nullptr
        && targetMorphMipData_->generation == mipData->generation;

    if ((sameActive && !morphActive_) || sameTarget)
        return;

    beginMorphToMipData(mipData);
}

// ─── FFT (Radix-2 Cooley-Tukey, in-place) ───

void WavetableOscillator::fft(std::vector<double>& re, std::vector<double>& im)
{
    const int n = static_cast<int>(re.size());
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; i++)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    // Butterfly stages
    for (int len = 2; len <= n; len *= 2)
    {
        const int half = len >> 1;
        const double angle = -2.0 * juce::MathConstants<double>::pi / len;
        const double wRe = std::cos(angle), wIm = std::sin(angle);
        for (int i = 0; i < n; i += len)
        {
            double curRe = 1.0, curIm = 0.0;
            for (int j = 0; j < half; j++)
            {
                const int a = i + j, b = a + half;
                const double tRe = re[b] * curRe - im[b] * curIm;
                const double tIm = re[b] * curIm + im[b] * curRe;
                re[b] = re[a] - tRe; im[b] = im[a] - tIm;
                re[a] = re[a] + tRe; im[a] = im[a] + tIm;
                const double tmp = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = tmp;
            }
        }
    }
}

void WavetableOscillator::ifft(std::vector<double>& re, std::vector<double>& im)
{
    const int n = static_cast<int>(re.size());
    for (int i = 0; i < n; i++) im[i] = -im[i];
    fft(re, im);
    const double invN = 1.0 / n;
    for (int i = 0; i < n; i++)
    {
        re[i] *= invN;
        im[i] = -im[i] * invN;
    }
}

// ─── Mip-level generation ───

WavetableOscillator::MipDataPtr WavetableOscillator::prepareMipLevels(const std::vector<std::vector<float>>& srcFrames)
{
    // Off-lock compute phase shared by prepareFramesFromBuffer/prepareContiguousFrames/
    // prepareExactFrames: the FFT mip-level work (NUM_MIP_LEVELS passes over every
    // frame) is the expensive part of all three, kept off getCallbackLock() so it
    // never delays the audio thread's next processBlock. Returns the new snapshot
    // WITHOUT publishing — pass it to applyPreparedMipData() to publish. Mutates
    // nextPublishedGeneration_ exactly as unguarded as this always mutated it
    // (pre-existing, not a hazard introduced by this split — mirrors
    // FreezeTextureEngine::prepareBufferLoad's nextGeneration_).
    const int nFrames = static_cast<int>(srcFrames.size());
    auto dest = std::make_shared<MipData>();
    dest->frames.resize(NUM_MIP_LEVELS);

    // Level 0 = original frames
    dest->frames[0].resize(nFrames);
    for (int f = 0; f < nFrames; f++)
        dest->frames[0][f] = srcFrames[f];

    // FFT buffers (reused)
    std::vector<double> re(FRAME_SIZE), im(FRAME_SIZE);

    for (int level = 1; level < NUM_MIP_LEVELS; level++)
    {
        const int maxHarmonic = HALF_FRAME >> level;
        dest->frames[level].resize(nFrames);

        for (int f = 0; f < nFrames; f++)
        {
            const auto& src = srcFrames[f];
            for (int i = 0; i < FRAME_SIZE; i++) { re[i] = src[i]; im[i] = 0.0; }

            fft(re, im);

            // Zero harmonics above maxHarmonic
            for (int k = maxHarmonic + 1; k <= FRAME_SIZE - maxHarmonic - 1; k++)
            {
                re[k] = 0.0;
                im[k] = 0.0;
            }

            ifft(re, im);

            dest->frames[level][f].resize(FRAME_SIZE);
            for (int i = 0; i < FRAME_SIZE; i++)
                dest->frames[level][f][i] = static_cast<float>(re[i]);
        }
    }

    dest->numFrames = nFrames;
    dest->numLevels = NUM_MIP_LEVELS;
    dest->generation = ++nextPublishedGeneration_;
    return dest;
}

void WavetableOscillator::applyPreparedMipData(MipDataPtr mipData)
{
    // Publish phase for prepareMipLevels() and its three prepareXXXFrames()
    // callers (and setAdditiveBank()): an atomic_store_explicit (release) into
    // publishedMipData_, paired with the atomic_load_explicit (acquire) in
    // loadPublishedMipData() — that pairing, not a lock, is what prevents a
    // torn read / UAF against the audio-thread readers (hasFrames(),
    // getNumFrames(), processSample(), and shareFramesFrom()/
    // morphToFramesFrom() reading a MASTER's published data cross-instance) on
    // every shipped format. getCallbackLock() does NOT cover this on its own:
    // processBlock holds it for its whole duration on Standalone/VST3/AU, but
    // the CLAP build calls processBlock with no lock at all (verified against
    // clap-juce-wrapper.cpp — zero getCallbackLock references, processBlock
    // called bare). Call this under an explicit ScopedLock(getCallbackLock())
    // anyway — real and necessary on the other three formats, and for this
    // class's non-atomic state — but do not treat the lock as sufficient by
    // itself. A nullptr snapshot is a no-op: every prepare*() early-out
    // returns nullptr to mean "leave the previous bank untouched", not
    // "clear it" (unlike FreezeTextureEngine::applyPreparedBufferLoad, which DOES
    // publish nullptr — the two engines' pre-existing empty-input behaviour differs
    // and this preserves each one exactly).
    if (mipData == nullptr)
        return;
    std::atomic_store_explicit(&publishedMipData_, mipData, std::memory_order_release);
}

// ─── Pitch detection (simplified YIN autocorrelation) ───

WavetableOscillator::PitchEstimate WavetableOscillator::analyzePitchWindow(const float* data,
                                                                           int length, double sr)
{
    if (length < 256) return {};

    const int halfLen = length / 2;
    std::vector<float> diff(halfLen, 0.0f);

    // Difference function
    for (int tau = 1; tau < halfLen; tau++)
    {
        float sum = 0.0f;
        for (int i = 0; i < halfLen; i++)
        {
            float d = data[i] - data[i + tau];
            sum += d * d;
        }
        diff[tau] = sum;
    }

    // Cumulative mean normalized difference
    diff[0] = 1.0f;
    float running = 0.0f;
    for (int tau = 1; tau < halfLen; tau++)
    {
        running += diff[tau];
        diff[tau] = diff[tau] * tau / running;
    }

    // Absolute threshold (0.1)
    constexpr float threshold = 0.1f;
    int tauEstimate = -1;
    for (int tau = 2; tau < halfLen; tau++)
    {
        if (diff[tau] < threshold)
        {
            while (tau + 1 < halfLen && diff[tau + 1] < diff[tau])
                tau++;
            tauEstimate = tau;
            break;
        }
    }

    if (tauEstimate < 0) return {};

    // Parabolic interpolation
    float s0 = diff[std::max(0, tauEstimate - 1)];
    float s1 = diff[tauEstimate];
    float s2 = diff[std::min(halfLen - 1, tauEstimate + 1)];
    float refinedTau = tauEstimate + 0.5f * (s0 - s2) / (s0 - 2.0f * s1 + s2 + 1e-10f);

    PitchEstimate result;
    result.hz = static_cast<float>(sr / refinedTau);
    result.confidence = juce::jlimit(0.0f, 1.0f, 1.0f - s1);
    return result;
}

float WavetableOscillator::detectPitch(const float* data, int length, double sr)
{
    return analyzePitchWindow(data, length, sr).hz;
}

// ─── Lanczos sinc interpolation ───

float WavetableOscillator::lanczosSample(const float* src, int srcLen, double pos)
{
    const int center = static_cast<int>(std::floor(pos));
    const double frac = pos - center;
    double sum = 0.0, weightSum = 0.0;

    for (int i = -SINC_KERNEL_A + 1; i <= SINC_KERNEL_A; i++)
    {
        int idx = center + i;
        if (idx < 0 || idx >= srcLen) continue;

        double x = frac - i;
        double w;
        if (std::abs(x) < 1e-6)
            w = 1.0;
        else if (std::abs(x) >= SINC_KERNEL_A)
            w = 0.0;
        else
        {
            double piX = juce::MathConstants<double>::pi * x;
            double piXA = piX / SINC_KERNEL_A;
            w = (std::sin(piX) / piX) * (std::sin(piXA) / piXA);
        }

        sum += src[idx] * w;
        weightSum += w;
    }

    return weightSum > 0.0 ? static_cast<float>(sum / weightSum) : 0.0f;
}

int WavetableOscillator::nearestZeroCrossing(const float* data, int length, int pos, int maxSearch)
{
    if (length < 2) return juce::jlimit(0, juce::jmax(0, length - 1), pos);

    pos = juce::jlimit(0, length - 2, pos);
    maxSearch = juce::jlimit(0, length - 2, maxSearch);

    for (int d = 0; d <= maxSearch; ++d)
    {
        const int fwd = pos + d;
        if (fwd < length - 1 && data[fwd] * data[fwd + 1] <= 0.0f)
            return fwd;

        const int bwd = pos - d;
        if (bwd >= 0 && bwd < length - 1 && data[bwd] * data[bwd + 1] <= 0.0f)
            return bwd;
    }

    return pos;
}

std::vector<float> WavetableOscillator::extractResampledPeriod(const float* data, int totalSamples,
                                                               double start, double periodSamples)
{
    std::vector<float> frame(FRAME_SIZE);
    for (int i = 0; i < FRAME_SIZE; ++i)
    {
        const double srcPos = start + (static_cast<double>(i) / FRAME_SIZE) * periodSamples;
        frame[i] = lanczosSample(data, totalSamples, srcPos);
    }
    return frame;
}

double WavetableOscillator::computeLoopBoundaryError(const std::vector<float>& frame)
{
    if (frame.size() < 4)
        return 0.0;

    const float boundary = std::abs(frame.front() - frame.back());
    const float startSlope = frame[1] - frame[0];
    const float endSlope = frame[static_cast<int>(frame.size()) - 1]
                         - frame[static_cast<int>(frame.size()) - 2];
    const float slopeMismatch = std::abs(startSlope - endSlope);
    return static_cast<double>(boundary) + 0.5 * static_cast<double>(slopeMismatch);
}

// ─── Frame extraction from audio buffer ───

void WavetableOscillator::extractFramesFromBuffer(const juce::AudioBuffer<float>& buffer, double bufferSr,
                                                   float startFrac, float endFrac, int maxFrames)
{
    // Combined compute+publish convenience — see this function's doc comment in the
    // header for who may call it and why. The already-locked production caller
    // (reextractWavetable) and the single-threaded tools-dir *.cpp harnesses use this
    // directly; a caller that must NOT hold the lock across the extraction/FFT work
    // below uses prepareFramesFromBuffer()/applyPreparedMipData() instead.
    applyPreparedMipData(prepareFramesFromBuffer(buffer, bufferSr, startFrac, endFrac, maxFrames));
}

WavetableOscillator::MipDataPtr WavetableOscillator::prepareFramesFromBuffer(
    const juce::AudioBuffer<float>& buffer, double bufferSr,
    float startFrac, float endFrac, int maxFrames)
{
    // Off-lock compute phase of extractFramesFromBuffer(): identical extraction +
    // pitch-analysis work, returned WITHOUT publishing (see prepareMipLevels()'s
    // doc comment for the shared publish contract). A nullptr return (every early
    // return in this function) means "leave the previous bank untouched" — matches
    // extractFramesFromBuffer's original behaviour of silently no-op'ing on bad
    // input rather than clearing the bank.
    maxFrames = juce::jlimit(8, 256, maxFrames);
    if (sharedSource_ != nullptr) return nullptr; // shared-mode oscillators don't own frame data
    const int bufferLen = buffer.getNumSamples();

    // Apply extraction region (brackets)
    startFrac = juce::jlimit(0.0f, 1.0f, startFrac);
    endFrac   = juce::jlimit(0.0f, 1.0f, endFrac);
    if (endFrac <= startFrac) endFrac = 1.0f;

    const int regionStart = static_cast<int>(startFrac * bufferLen);
    const int regionEnd   = static_cast<int>(endFrac * bufferLen);
    const float* data = buffer.getReadPointer(0) + regionStart;
    const int totalSamples = regionEnd - regionStart;

    if (totalSamples < FRAME_SIZE) return nullptr;

    std::vector<std::vector<float>> frames;
    constexpr int analysisWindow = 4096;
    constexpr int analysisHop = analysisWindow / 2;
    constexpr float pitchConfidenceThreshold = 0.9f;
    std::vector<float> pitchCandidates;

    if (totalSamples >= 256)
    {
        if (totalSamples >= analysisWindow)
        {
            for (int windowStart = 0; windowStart + analysisWindow <= totalSamples; windowStart += analysisHop)
            {
                PitchEstimate estimate = analyzePitchWindow(data + windowStart, analysisWindow, bufferSr);
                if (estimate.confidence >= pitchConfidenceThreshold
                    && estimate.hz > 20.0f && estimate.hz < 5000.0f)
                {
                    pitchCandidates.push_back(estimate.hz);
                }
            }
        }
        else
        {
            PitchEstimate estimate = analyzePitchWindow(data, totalSamples, bufferSr);
            if (estimate.confidence >= pitchConfidenceThreshold
                && estimate.hz > 20.0f && estimate.hz < 5000.0f)
            {
                pitchCandidates.push_back(estimate.hz);
            }
        }
    }

    const bool allowPitchSync = !pitchCandidates.empty()
        && (pitchCandidates.size() >= 2 || totalSamples < analysisWindow * 2);

    if (allowPitchSync)
    {
        std::sort(pitchCandidates.begin(), pitchCandidates.end());
        const float detectedPitch = pitchCandidates[pitchCandidates.size() / 2];

        // Pitch-synchronous extraction with adaptive pitch tracking
        double periodSamples = bufferSr / detectedPitch;
        double pos = 0.0;
        int framesSinceDetect = 0;
        constexpr int REDETECT_INTERVAL = 8;

        while (static_cast<int>(pos + periodSamples * 1.5) < totalSamples
               && static_cast<int>(frames.size()) < maxFrames)
        {
            const double baseStart = pos;
            std::vector<float> frame = extractResampledPeriod(data, totalSamples, baseStart, periodSamples);
            double bestError = computeLoopBoundaryError(frame);
            double selectedStart = baseStart;

            const int searchRadius = juce::jlimit(0, totalSamples - 1,
                static_cast<int>(std::floor(periodSamples * 0.125)));
            if (searchRadius > 0)
            {
                const int baseIndex = juce::jlimit(0, totalSamples - 1,
                    static_cast<int>(std::round(baseStart)));
                const int candidateStart = nearestZeroCrossing(data, totalSamples, baseIndex, searchRadius);

                if (candidateStart != baseIndex
                    && static_cast<double>(candidateStart) + periodSamples <= totalSamples)
                {
                    std::vector<float> snapped = extractResampledPeriod(
                        data, totalSamples, static_cast<double>(candidateStart), periodSamples);
                    const double snappedError = computeLoopBoundaryError(snapped);

                    if (snappedError + 1.0e-6 < bestError)
                    {
                        frame = std::move(snapped);
                        bestError = snappedError;
                        selectedStart = static_cast<double>(candidateStart);
                    }
                }
            }

            // Seamless loop: linear ramp correction at boundary
            float diff = frame[0] - frame[FRAME_SIZE - 1];
            for (int i = 0; i < FRAME_SIZE; i++)
                frame[i] += diff * static_cast<float>(i) / FRAME_SIZE;

            frames.push_back(std::move(frame));
            pos = selectedStart + periodSamples;

            // Re-detect pitch periodically to track evolving content
            if (++framesSinceDetect >= REDETECT_INTERVAL)
            {
                int intPos = static_cast<int>(pos);
                int remaining = totalSamples - intPos;
                int localAnalysisLen = std::min(analysisWindow, remaining);
                if (localAnalysisLen >= 256)
                {
                    PitchEstimate estimate = analyzePitchWindow(data + intPos, localAnalysisLen, bufferSr);
                    if (estimate.confidence >= pitchConfidenceThreshold
                        && estimate.hz > 20.0f && estimate.hz < 5000.0f)
                    {
                        periodSamples = bufferSr / estimate.hz;
                    }
                }
                framesSinceDetect = 0;
            }
        }
    }
    else
    {
        // Unpitched: overlapping windowed extraction
        const int hop = FRAME_SIZE / 2;
        int pos = 0;

        while (pos + FRAME_SIZE <= totalSamples && static_cast<int>(frames.size()) < maxFrames)
        {
            std::vector<float> frame(FRAME_SIZE);
            constexpr float tukeyAlpha = 0.1f;
            const int taperLen = static_cast<int>(tukeyAlpha * FRAME_SIZE * 0.5f);
            for (int i = 0; i < FRAME_SIZE; i++)
            {
                // Tukey window: flat center (90%), cosine taper at edges (5% each side)
                float window = 1.0f;
                if (i < taperLen)
                    window = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * static_cast<float>(i) / taperLen));
                else if (i >= FRAME_SIZE - taperLen)
                    window = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * static_cast<float>(FRAME_SIZE - 1 - i) / taperLen));
                frame[i] = data[pos + i] * window;
            }

            // Seamless loop correction
            float diff = frame[0] - frame[FRAME_SIZE - 1];
            for (int i = 0; i < FRAME_SIZE; i++)
                frame[i] += diff * static_cast<float>(i) / FRAME_SIZE;

            frames.push_back(std::move(frame));
            pos += hop;
        }
    }

    // Remove near-silent frames (peak below -40dB ≈ 0.01)
    frames.erase(std::remove_if(frames.begin(), frames.end(), [](const std::vector<float>& f) {
        float peak = 0.0f;
        for (float s : f) peak = std::max(peak, std::abs(s));
        return peak < 0.01f;
    }), frames.end());

    // Normalize each frame to peak 0.95 (essential for wavetable playback)
    for (auto& frame : frames)
    {
        float peak = 0.0f;
        for (float s : frame) peak = std::max(peak, std::abs(s));
        if (peak > 0.001f)
        {
            float gain = 0.95f / peak;
            for (float& s : frame) s *= gain;
        }
    }

    // Ensure minimum frame count
    while (static_cast<int>(frames.size()) < MIN_FRAMES && !frames.empty())
    {
        frames.push_back(frames.back());
    }

    return frames.empty() ? nullptr : prepareMipLevels(frames);
}

// ─── Auto-scan (sampler-style temporal progression) ───

void WavetableOscillator::setAutoScanRate(double bufferSR, int bufferLen)
{
    // Scan goes from 0 to 1 in (bufferLen / bufferSR) seconds
    double durationSeconds = static_cast<double>(bufferLen) / bufferSR;
    if (durationSeconds > 0.0)
        autoScanIncr_ = 1.0 / (durationSeconds * sampleRate);
    else
        autoScanIncr_ = 0.0;
}

void WavetableOscillator::setAutoScanRateHz(float rateHz)
{
    // Full 0->1 sweeps per second, directly — for a caller with a tempo
    // rather than a source buffer's duration (the DCO/LCO recipe's
    // motion_rate_hz). Same clamp range the removed private DCO transport
    // used to apply, so a wild recipe rate still can't produce aliasing/silent
    // extremes. Writes the SAME autoScanIncr_ setAutoScanRate does above —
    // one scan-advance mechanism, two ways to express its rate.
    rateHz = juce::jlimit(0.01f, 10.0f, rateHz);
    autoScanIncr_ = static_cast<double>(rateHz) / sampleRate;
}

void WavetableOscillator::setAutoScanLoop(float startFrac, float endFrac, LoopMode mode)
{
    autoScanLoopStart_ = juce::jlimit(0.0f, 1.0f, startFrac);
    autoScanLoopEnd_   = juce::jlimit(0.0f, 1.0f, endFrac);
    if (autoScanLoopEnd_ <= autoScanLoopStart_)
        autoScanLoopEnd_ = 1.0f;
    autoScanLoopMode_ = mode;
}

void WavetableOscillator::setAutoScanStartPos(float frac)
{
    autoScanStartPos_ = juce::jlimit(0.0f, 1.0f, frac);
}

void WavetableOscillator::retriggerAutoScan()
{
    // Determine direction from P1 vs P3
    bool reversed = (autoScanStartPos_ > autoScanLoopEnd_);
    autoScanDirection_ = reversed ? -1 : 1;
    autoScanInFirstPass_ = true;

    autoScanPos_ = static_cast<double>(autoScanStartPos_);
    // Jump scan immediately (no smoothing lag on retrigger)
    const float scanTarget = autoScan_
        ? juce::jlimit(0.0f, 1.0f, static_cast<float>(autoScanPos_) + scanControl_)
        : static_cast<float>(autoScanPos_);
    smoothedScan = scanTarget;
}

// ─── Contiguous frame extraction (sampler-style) ───

void WavetableOscillator::extractContiguousFrames(const juce::AudioBuffer<float>& buffer, double bufferSR,
                                                    float startFrac, float endFrac)
{
    // Combined compute+publish convenience — see extractFramesFromBuffer's doc
    // comment in the header (same contract). A caller that must NOT hold the lock
    // across the extraction work below uses prepareContiguousFrames()/
    // applyPreparedMipData() instead.
    applyPreparedMipData(prepareContiguousFrames(buffer, bufferSR, startFrac, endFrac));
}

WavetableOscillator::MipDataPtr WavetableOscillator::prepareContiguousFrames(
    const juce::AudioBuffer<float>& buffer, double bufferSR,
    float startFrac, float endFrac)
{
    // Off-lock compute phase of extractContiguousFrames() — see
    // prepareFramesFromBuffer()'s doc comment for the shared nullptr/publish
    // contract.
    if (sharedSource_ != nullptr) return nullptr;

    const int bufferLen = buffer.getNumSamples();
    startFrac = juce::jlimit(0.0f, 1.0f, startFrac);
    endFrac   = juce::jlimit(0.0f, 1.0f, endFrac);
    if (endFrac <= startFrac) endFrac = 1.0f;

    const int regionStart = static_cast<int>(startFrac * bufferLen);
    const int regionEnd   = static_cast<int>(endFrac * bufferLen);
    const float* data = buffer.getReadPointer(0) + regionStart;
    const int totalSamples = regionEnd - regionStart;

    if (totalSamples < FRAME_SIZE) return nullptr;

    std::vector<std::vector<float>> frames;

    // Extract contiguous windows — no pitch detection, no resampling
    int pos = 0;
    while (pos + FRAME_SIZE <= totalSamples)
    {
        std::vector<float> frame(FRAME_SIZE);
        for (int i = 0; i < FRAME_SIZE; i++)
            frame[i] = data[pos + i];

        // Seamless loop correction (linear ramp to close boundary gap)
        float diff = frame[0] - frame[FRAME_SIZE - 1];
        for (int i = 0; i < FRAME_SIZE; i++)
            frame[i] += diff * static_cast<float>(i) / FRAME_SIZE;

        frames.push_back(std::move(frame));
        pos += FRAME_SIZE;  // non-overlapping
    }

    // Normalize each frame
    for (auto& frame : frames)
    {
        float peak = 0.0f;
        for (float s : frame) peak = std::max(peak, std::abs(s));
        if (peak > 0.001f)
        {
            float gain = 0.95f / peak;
            for (float& s : frame) s *= gain;
        }
    }

    if (static_cast<int>(frames.size()) < MIN_FRAMES && !frames.empty())
        while (static_cast<int>(frames.size()) < MIN_FRAMES)
            frames.push_back(frames.back());

    return frames.empty() ? nullptr : prepareMipLevels(frames);
}

void WavetableOscillator::setExactFrames(const juce::AudioBuffer<float>& strip)
{
    // Combined compute+publish convenience — see extractFramesFromBuffer's doc
    // comment in the header (same contract). A caller that must NOT hold the lock
    // across the frame-slicing/FFT work below uses prepareExactFrames()/
    // applyPreparedMipData() instead.
    applyPreparedMipData(prepareExactFrames(strip));
}

WavetableOscillator::MipDataPtr WavetableOscillator::prepareExactFrames(const juce::AudioBuffer<float>& strip)
{
    // Off-lock compute phase of setExactFrames() — see prepareFramesFromBuffer()'s
    // doc comment for the shared nullptr/publish contract.
    if (sharedSource_ != nullptr) return nullptr;

    const int totalSamples = strip.getNumSamples();
    if (strip.getNumChannels() < 1 || totalSamples < FRAME_SIZE)
        return nullptr;

    const float* data = strip.getReadPointer(0);
    const int numFrames = totalSamples / FRAME_SIZE;   // exact boundaries, tail ignored

    std::vector<std::vector<float>> frames;
    frames.reserve(static_cast<size_t>(numFrames));
    for (int f = 0; f < numFrames; ++f)
        frames.emplace_back(data + f * FRAME_SIZE, data + (f + 1) * FRAME_SIZE);

    if (static_cast<int>(frames.size()) < MIN_FRAMES && !frames.empty())
        while (static_cast<int>(frames.size()) < MIN_FRAMES)
            frames.push_back(frames.back());

    return frames.empty() ? nullptr : prepareMipLevels(frames);
}

void WavetableOscillator::setAdditiveBank(const std::vector<AdditivePartial>& partials)
{
    // Thin overload: one station is the K==1 case of the aligned-sets bank. Delegates
    // so there is exactly one sanitize/publish path.
    setAdditiveBank(std::vector<std::vector<AdditivePartial>>{ partials });
}

void WavetableOscillator::setAdditiveBank(const std::vector<std::vector<AdditivePartial>>& sets)
{
    if (sharedSource_ != nullptr) return;   // shared-mode voices adopt from the master

    // Build the additive-bank payload. No frames, no mips: the partials are
    // synthesized per sample, so this bypasses prepareMipLevels entirely. Publishes
    // via the same applyPreparedMipData() atomic-publish path prepareMipLevels'
    // callers use — voices pick it up via shareFramesFrom / morphToFramesFrom
    // exactly like a wavetable generation. NOTE: unlike those, this function
    // has no production caller today (only tools-dir *.cpp harnesses,
    // single-threaded, no lock or atomics needed there) — see
    // loadPublishedMipData()'s comment. A future production caller MUST hold
    // getCallbackLock() around it too, same as extractFramesFromBuffer()'s
    // convenience form — the atomics cover the publish on every format, but
    // the lock is still needed on Standalone/VST3/AU and for this class's
    // other, non-atomic state.
    auto dest = std::make_shared<MipData>();
    dest->isAdditive = true;
    dest->numFrames = 1;   // sentinel: hasFrames()/processSample treat the bank as "has data"
                           // (the STATION count is partialSets.size(), NOT numFrames)
    dest->numLevels = 1;   // frames stay empty; the additive path never indexes them

    // Cap the station count K (spec §3: user recipes 1–5, sub-stations up to 64).
    const int numSets = std::min(static_cast<int>(sets.size()), MAX_ADDITIVE_SETS);

    // Aligned INPUT range = the SHORTEST station's length. Index i can only carry a
    // partial if EVERY set has an entry there (index alignment), so it is the min over
    // sets, not the max — unequal lengths cap to the shortest defensively. Deliberately
    // NOT capped at MAX_ADDITIVE_PARTIALS: that cap is on the ACCEPTED count in the
    // loop below, so the scan may pass dropped (invalid-h) indices and still fill all
    // 64 slots with valid partials — exactly the old single-set whole-list scan.
    int nIn = 0;
    if (numSets > 0)
    {
        nIn = static_cast<int>(sets[0].size());
        for (int s = 1; s < numSets; ++s)
            nIn = std::min(nIn, static_cast<int>(sets[static_cast<size_t>(s)].size()));
    }

    // Sanitize per index across ALL sets in lockstep. h is a frequency ratio and MUST
    // be > 0: a non-finite/non-positive h never advances (w*h == 0) and never trips the
    // Nyquist drop, so clamping it to 0 would leave a constant a*gain*sin(phase0) DC
    // term forever (and steal headroom). It is dropped — but the drop is applied to the
    // SAME index in EVERY set, because dropping per-set would misalign the stations.
    std::vector<std::vector<AdditivePartial>> kept(static_cast<size_t>(numSets));
    for (int s = 0; s < numSets; ++s)
        kept[static_cast<size_t>(s)].reserve(
            static_cast<size_t>(std::min(nIn, MAX_ADDITIVE_PARTIALS)));
    for (int i = 0; i < nIn; ++i)
    {
        // Cap the KEPT count: stop only once MAX_ADDITIVE_PARTIALS partials have been
        // ACCEPTED — the same break-before-validity order as the old single-set loop,
        // so K==1 stays byte-equivalent to it. The kept sets grow in lockstep, so
        // kept[0] is THE accepted count (safe: nIn > 0 implies numSets > 0).
        if (static_cast<int>(kept[0].size()) >= MAX_ADDITIVE_PARTIALS) break;
        bool good = true;
        for (int s = 0; s < numSets && good; ++s)
        {
            const float h = sets[static_cast<size_t>(s)][static_cast<size_t>(i)].h;
            if (!std::isfinite(h) || h <= 0.0f) good = false;
        }
        if (!good) continue;
        for (int s = 0; s < numSets; ++s)
        {
            const auto& src = sets[static_cast<size_t>(s)][static_cast<size_t>(i)];
            AdditivePartial q;
            // NaN-guard a/phase (a recipe may come from an LLM's output) and clamp h
            // high-side to the wavetable ceiling — exactly as the single-set path did.
            q.h     = std::min(src.h, static_cast<float>(HALF_FRAME));
            q.a     = std::isfinite(src.a)     ? src.a     : 0.0f;
            q.phase = std::isfinite(src.phase) ? src.phase : 0.0f;
            kept[static_cast<size_t>(s)].push_back(q);
        }
    }

    // Gain = 0.95 / max_over_stations(sum|a_i|): the BANK-WIDE worst case, NOT a
    // per-position renorm. |lerp(a_s, a_s+1, t)| <= max of the two station sums, so no
    // scan position can clip; and the loudness between a dark and a bright station
    // breathes naturally (per-position renorm was the loudness-inversion defect the ear
    // rejected). For K==1 this is 0.95 / sum|a_i| of the one station — identical to the
    // former single-set gain.
    double maxSumAbs = 0.0;
    for (int s = 0; s < numSets; ++s)
    {
        double sumAbs = 0.0;
        for (const auto& p : kept[static_cast<size_t>(s)])
            sumAbs += std::abs(static_cast<double>(p.a));
        maxSumAbs = std::max(maxSumAbs, sumAbs);
    }

    // Empty outcome (no sets / N==0 / all-silent) keeps today's silence semantics: the
    // partialSets carry the sanitized (possibly empty) stations and the gain is 0, so
    // synthAdditiveSample returns 0 with no div-by-0.
    dest->partialSets = std::move(kept);
    dest->additiveGain = (maxSumAbs > 1.0e-9) ? static_cast<float>(0.95 / maxSumAbs) : 0.0f;
    dest->generation = ++nextPublishedGeneration_;

    applyPreparedMipData(std::move(dest));
}

// ─── Per-sample processing ───

void WavetableOscillator::glideToFrequency(float hz, float durationMs)
{
    hz = juce::jlimit(20.0f, 20000.0f, hz);
    float durationSamples = (durationMs / 1000.0f) * static_cast<float>(sampleRate);
    int samples = std::max(1, static_cast<int>(durationSamples));

    glideFreqTarget = hz;
    glideFreqIncr = (hz - targetFrequency) / static_cast<float>(samples);
    glideFreqSamplesLeft = samples;
}

float WavetableOscillator::processSample()
{
    if (activeMorphMipData_ == nullptr)
    {
        // First sample after a bare publish (the DCO tool / master path adopts
        // lazily here). Seed additive phases so an additive bank starts in phase.
        auto pub = loadPublishedMipData();
        if (pub != nullptr && pub->isAdditive)
            seedAdditivePhases(activeAddPhase_, *pub);
        activeMorphMipData_ = pub;
    }
    if (activeMorphMipData_ == nullptr || activeMorphMipData_->numFrames == 0)
        return 0.0f;

    // Apply frequency glide (per-sample linear ramp)
    if (glideFreqSamplesLeft > 0)
    {
        targetFrequency += glideFreqIncr;
        glideFreqSamplesLeft--;
        if (glideFreqSamplesLeft == 0)
            targetFrequency = glideFreqTarget;
        // Glide ramp bypasses setFrequency; clamp here to keep targetFrequency
        // non-negative and NaN-free. !(x >= 0) is true for negatives AND NaN.
        if (!(targetFrequency >= 0.0f))
            targetFrequency = 0.0f;
    }

    float scanTarget = scanControl_;

    // Auto-scan: advance scan position per sample (3-point logic). This is the
    // ONLY scan-advance mechanism the oscillator runs. A DCO/LCO table's
    // authored motion moves through it too (see setAutoScanRateHz) — enabled
    // once at load time via the same AutoScan the user's toggle/rate/loop
    // controls drive; the oscillator itself owns no private transport.
    if (autoScan_)
    {
        const double pEnd   = static_cast<double>(autoScanLoopEnd_);
        const double pStart = static_cast<double>(autoScanLoopStart_);

        autoScanPos_ += autoScanIncr_ * autoScanDirection_;

        if (autoScanInFirstPass_)
        {
            // During first pass, only check the boundary we're moving toward
            if (autoScanDirection_ > 0 && autoScanPos_ >= pEnd)
            {
                autoScanInFirstPass_ = false;
                if (autoScanLoopMode_ == LoopMode::OneShot)
                    autoScanPos_ = pEnd - 0.0001;
                else if (autoScanLoopMode_ == LoopMode::PingPong)
                {
                    autoScanPos_ = pEnd - (autoScanPos_ - pEnd);
                    autoScanDirection_ = -1;
                }
                else // Loop
                    autoScanPos_ = pStart + (autoScanPos_ - pEnd);
            }
            else if (autoScanDirection_ < 0 && autoScanPos_ < pStart)
            {
                autoScanInFirstPass_ = false;
                if (autoScanLoopMode_ == LoopMode::OneShot)
                    autoScanPos_ = pStart;
                else if (autoScanLoopMode_ == LoopMode::PingPong)
                {
                    autoScanPos_ = pStart + (pStart - autoScanPos_);
                    autoScanDirection_ = 1;
                }
                else // Loop
                    autoScanPos_ = pEnd - (pStart - autoScanPos_);
            }
        }
        else
        {
            // Standard looping between P2-P3
            if (autoScanPos_ >= pEnd)
            {
                if (autoScanLoopMode_ == LoopMode::PingPong)
                {
                    autoScanPos_ = pEnd - (autoScanPos_ - pEnd);
                    autoScanDirection_ = -1;
                }
                else
                    autoScanPos_ = pStart + (autoScanPos_ - pEnd);
            }
            else if (autoScanPos_ < pStart)
            {
                if (autoScanLoopMode_ == LoopMode::PingPong)
                {
                    autoScanPos_ = pStart + (pStart - autoScanPos_);
                    autoScanDirection_ = 1;
                }
                else
                    autoScanPos_ = pEnd - (pStart - autoScanPos_);
            }
        }

        scanTarget = juce::jlimit(0.0f, 1.0f,
            static_cast<float>(autoScanPos_) + scanControl_);
    }

    // Smooth scan position (5 ms one-pole)
    smoothedScan += (scanTarget - smoothedScan) * scanSmoothCoeff;

    const float scanNow = juce::jlimit(0.0f, 1.0f, smoothedScan);
    // Publish the effective read position for the WT display's scan cursor.
    // One float write, no allocation — audio-thread safe.
    lastScanNow_ = scanNow;

    // Recompute mip-level selector only when frequency changes. Outside glide and modulation
    // bursts this is constant for very long runs, so the log2/ceil cost is amortised to ~0.
    if (targetFrequency != lastMipFreq_)
    {
        const float invBaseFreq = FRAME_SIZE / static_cast<float>(sampleRate);
        const float rawLevel = std::log2(juce::jmax(1.0e-6f, targetFrequency * invBaseFreq));
        cachedMipRawCeil_ = static_cast<int>(std::ceil(rawLevel));
        lastMipFreq_ = targetFrequency;
    }

    // Active payload -> one sample. An additive bank synthesizes its partials in
    // real time (inharmonic-capable, no mip); a wavetable bank reads its mip frames.
    float output;
    if (activeMorphMipData_->isAdditive)
        output = synthAdditiveSample(*activeMorphMipData_, activeAddPhase_, scanNow);
    else
    {
        const int activeMip = juce::jlimit(0, activeMorphMipData_->numLevels - 1, cachedMipRawCeil_);
        output = readMipSample(*activeMorphMipData_, activeMip, phase, scanNow, doInterpolate);
    }

    if (morphActive_ && targetMorphMipData_ != nullptr && targetMorphMipData_->numFrames > 0)
    {
        const float alpha = juce::jlimit(0.0f, 1.0f, morphAlpha_);
        float targetSample;
        if (targetMorphMipData_->isAdditive)
            targetSample = synthAdditiveSample(*targetMorphMipData_, targetAddPhase_, scanNow);
        else
        {
            const int targetMip = juce::jlimit(0, targetMorphMipData_->numLevels - 1, cachedMipRawCeil_);
            targetSample = readMipSample(*targetMorphMipData_, targetMip, phase, scanNow, doInterpolate);
        }
        const float dryGain = std::cos(alpha * juce::MathConstants<float>::halfPi);
        const float wetGain = std::sin(alpha * juce::MathConstants<float>::halfPi);
        output = output * dryGain + targetSample * wetGain;

        morphAlpha_ += morphIncrement_;
        if (morphAlpha_ >= 1.0f)
        {
            // Morph done: carry the target's RUNNING partial phases into active so
            // the additive tail stays phase-continuous (re-seeding would click),
            // then adopt without re-seeding.
            activeAddPhase_ = targetAddPhase_;
            adoptMipData(targetMorphMipData_, /*seedAdditivePhase=*/false);
        }
    }

    // Advance additive phase accumulators once per sample (the reads above used the
    // current phase). Both banks advance while a morph is live so the fading-in
    // target stays phase-correct.
    if (activeMorphMipData_->isAdditive)
        advanceAdditivePhases(activeAddPhase_, *activeMorphMipData_, scanNow);
    if (morphActive_ && targetMorphMipData_ != nullptr && targetMorphMipData_->isAdditive)
        advanceAdditivePhases(targetAddPhase_, *targetMorphMipData_, scanNow);

    // Advance phase. Symmetric wrap via floor handles both positive overshoot
    // and (in case of any unforeseen negative drift) negative phase without an
    // audible click. NaN guard: any non-finite phase reseeds to 0.
    phase += targetFrequency * FRAME_SIZE / sampleRate;
    if (!std::isfinite(phase))
        phase = 0.0;
    else
        phase -= FRAME_SIZE * std::floor(phase / FRAME_SIZE);

    return output;
}

float WavetableOscillator::readMipSample(const MipData& mipData, int mipLevel, double phase,
                                         float scanPosition, bool interpolate)
{
    if (mipData.numFrames <= 0 || mipData.numLevels <= 0)
        return 0.0f;

    const auto& frames = mipData.frames[static_cast<size_t>(mipLevel)];
    const int nf = mipData.numFrames;

    const float framePos = juce::jlimit(0.0f, 1.0f, scanPosition) * static_cast<float>(nf - 1);
    const int frameA = static_cast<int>(std::floor(framePos));
    // Defensive: signed-safe modulo so a pathological negative or NaN phase from any
    // upstream path can never index frame[] with a negative-cast-to-size_t value.
    int idxRaw = static_cast<int>(std::floor(phase)) % FRAME_SIZE;
    if (idxRaw < 0) idxRaw += FRAME_SIZE;
    const int idx0 = idxRaw;
    const int idx1 = (idx0 + 1) % FRAME_SIZE;
    const float phaseFrac = static_cast<float>(phase - std::floor(phase));

    if (!interpolate)
    {
        const auto& frame = frames[static_cast<size_t>(juce::jlimit(0, nf - 1, frameA))];
        return frame[static_cast<size_t>(idx0)]
             + (frame[static_cast<size_t>(idx1)] - frame[static_cast<size_t>(idx0)]) * phaseFrac;
    }

    const float frameMix = framePos - static_cast<float>(frameA);
    const int i0 = juce::jlimit(0, nf - 1, frameA - 1);
    const int i1 = juce::jlimit(0, nf - 1, frameA);
    const int i2 = juce::jlimit(0, nf - 1, frameA + 1);
    const int i3 = juce::jlimit(0, nf - 1, frameA + 2);

    const auto& f0 = frames[static_cast<size_t>(i0)];
    const auto& f1 = frames[static_cast<size_t>(i1)];
    const auto& f2 = frames[static_cast<size_t>(i2)];
    const auto& f3 = frames[static_cast<size_t>(i3)];

    const float s0 = f0[static_cast<size_t>(idx0)]
                   + (f0[static_cast<size_t>(idx1)] - f0[static_cast<size_t>(idx0)]) * phaseFrac;
    const float s1 = f1[static_cast<size_t>(idx0)]
                   + (f1[static_cast<size_t>(idx1)] - f1[static_cast<size_t>(idx0)]) * phaseFrac;
    const float s2 = f2[static_cast<size_t>(idx0)]
                   + (f2[static_cast<size_t>(idx1)] - f2[static_cast<size_t>(idx0)]) * phaseFrac;
    const float s3 = f3[static_cast<size_t>(idx0)]
                   + (f3[static_cast<size_t>(idx1)] - f3[static_cast<size_t>(idx0)]) * phaseFrac;

    const float t = frameMix;
    const float t2 = t * t;
    const float t3 = t2 * t;
    return 0.5f * ((2.0f * s1)
        + (-s0 + s2) * t
        + (2.0f * s0 - 5.0f * s1 + 4.0f * s2 - s3) * t2
        + (-s0 + 3.0f * s1 - 3.0f * s2 + s3) * t3);
}

// ─── Real-time additive synthesis (inharmonic banks) ───

void WavetableOscillator::seedAdditivePhases(std::array<double, MAX_ADDITIVE_PARTIALS>& dst,
                                             const MipData& mip) const
{
    // phase is a per-index property, identical across stations by backend contract;
    // set 0 wins defensively. An empty bank (no sets) seeds all-zero.
    const int n = mip.partialSets.empty()
        ? 0
        : std::min(static_cast<int>(mip.partialSets[0].size()), MAX_ADDITIVE_PARTIALS);
    for (int i = 0; i < n; ++i)
        dst[static_cast<size_t>(i)] = static_cast<double>(mip.partialSets[0][static_cast<size_t>(i)].phase);
    for (int i = n; i < MAX_ADDITIVE_PARTIALS; ++i)
        dst[static_cast<size_t>(i)] = 0.0;
}

float WavetableOscillator::synthAdditiveSample(const MipData& mip,
                                               const std::array<double, MAX_ADDITIVE_PARTIALS>& phaseAcc,
                                               float scanNow) const
{
    // sum_i a_i * sin(phaseAcc_i), scaled by the precomputed gain. A partial whose
    // frequency f0*h_i has reached Nyquist is dropped (per-sample anti-aliasing):
    // this is why the additive path needs no mip band-limiting.
    const double nyquist = 0.5 * sampleRate;
    const double f0 = static_cast<double>(targetFrequency);
    const int numSets = static_cast<int>(mip.partialSets.size());
    if (numSets <= 0) return 0.0f;

    // K == 1: byte-identical to the former single-set loop (same operation order).
    if (numSets == 1)
    {
        const auto& set0 = mip.partialSets[0];
        const int n = std::min(static_cast<int>(set0.size()), MAX_ADDITIVE_PARTIALS);
        double sum = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const auto& p = set0[static_cast<size_t>(i)];
            if (f0 * static_cast<double>(p.h) >= nyquist) continue;
            sum += static_cast<double>(p.a) * std::sin(phaseAcc[static_cast<size_t>(i)]);
        }
        return static_cast<float>(sum * static_cast<double>(mip.additiveGain));
    }

    // K >= 2: linearly blend (a,h) per index between the two stations bracketing the
    // scan position, then sum. Stations are index-aligned (same N, one phase per index).
    // scanNow is clamped [0,1] to match readMipSample's own defensive frame clamp.
    const float pos = juce::jlimit(0.0f, 1.0f, scanNow) * static_cast<float>(numSets - 1);
    const int s = juce::jlimit(0, numSets - 2, static_cast<int>(std::floor(pos)));
    const float t = pos - static_cast<float>(s);
    const auto& setA = mip.partialSets[static_cast<size_t>(s)];
    const auto& setB = mip.partialSets[static_cast<size_t>(s + 1)];
    const int n = std::min(static_cast<int>(setA.size()), MAX_ADDITIVE_PARTIALS);
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const auto& pa = setA[static_cast<size_t>(i)];
        const auto& pb = setB[static_cast<size_t>(i)];
        const float h = pa.h + (pb.h - pa.h) * t;
        const float a = pa.a + (pb.a - pa.a) * t;
        if (f0 * static_cast<double>(h) >= nyquist) continue;   // Nyquist drop on the INTERPOLATED h
        sum += static_cast<double>(a) * std::sin(phaseAcc[static_cast<size_t>(i)]);
    }
    return static_cast<float>(sum * static_cast<double>(mip.additiveGain));
}

void WavetableOscillator::advanceAdditivePhases(std::array<double, MAX_ADDITIVE_PARTIALS>& phaseAcc,
                                                const MipData& mip, float scanNow) const
{
    const double twoPi = juce::MathConstants<double>::twoPi;
    const double w = twoPi * static_cast<double>(targetFrequency) / sampleRate;  // fundamental radians/sample
    const int numSets = static_cast<int>(mip.partialSets.size());
    if (numSets <= 0) return;

    // K == 1: byte-identical to the former single-set advance (same operation order).
    if (numSets == 1)
    {
        const auto& set0 = mip.partialSets[0];
        const int n = std::min(static_cast<int>(set0.size()), MAX_ADDITIVE_PARTIALS);
        for (int i = 0; i < n; ++i)
        {
            double ph = phaseAcc[static_cast<size_t>(i)]
                      + w * static_cast<double>(set0[static_cast<size_t>(i)].h);
            // Wrap mod 2*pi. sin() is 2*pi-periodic, so this is EXACT and click-free for
            // ANY h (integer or inharmonic) — unlike a wavetable's cycle wrap. floor
            // covers multi-wrap and any negative drift; NaN can't arise (guarded inputs).
            ph -= twoPi * std::floor(ph / twoPi);
            phaseAcc[static_cast<size_t>(i)] = ph;
        }
        return;
    }

    // K >= 2: advance each accumulator by w * h_i(scanNow), using the SAME station
    // interpolation as the sample sum so phase and amplitude track one blend. The mod
    // 2*pi wrap stays exact and click-free for any (interpolated) h.
    const float pos = juce::jlimit(0.0f, 1.0f, scanNow) * static_cast<float>(numSets - 1);
    const int s = juce::jlimit(0, numSets - 2, static_cast<int>(std::floor(pos)));
    const float t = pos - static_cast<float>(s);
    const auto& setA = mip.partialSets[static_cast<size_t>(s)];
    const auto& setB = mip.partialSets[static_cast<size_t>(s + 1)];
    const int n = std::min(static_cast<int>(setA.size()), MAX_ADDITIVE_PARTIALS);
    for (int i = 0; i < n; ++i)
    {
        const float hA = setA[static_cast<size_t>(i)].h;
        const float hB = setB[static_cast<size_t>(i)].h;
        const float h = hA + (hB - hA) * t;
        double ph = phaseAcc[static_cast<size_t>(i)] + w * static_cast<double>(h);
        ph -= twoPi * std::floor(ph / twoPi);
        phaseAcc[static_cast<size_t>(i)] = ph;
    }
}
