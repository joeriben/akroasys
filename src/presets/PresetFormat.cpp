#include "PresetFormat.h"
#include "../PluginProcessor.h"
#include "../dsp/BlockParams.h"
#include <cstring>
#include <memory>

namespace
{
juce::String getStoredPresetName(const juce::var& parsed, const juce::File& file)
{
    if (auto* root = parsed.getDynamicObject())
    {
        auto stored = root->getProperty("name").toString().trim();
        if (stored.isNotEmpty() && stored != "T5ynth Export")
            return stored;
    }

    return file.getFileNameWithoutExtension();
}

juce::StringArray readTagsFromJson(const juce::var& parsed)
{
    juce::StringArray tags;
    if (auto* root = parsed.getDynamicObject())
    {
        if (auto* arr = root->getProperty("tags").getArray())
        {
            for (auto& v : *arr)
            {
                auto t = v.toString().trim();
                if (t.isNotEmpty())
                    tags.addIfNotAlreadyThere(t);
            }
        }
    }
    return tags;
}

// ── FLAC compression helpers (v4 audio payloads) ─────────────────────
// Encode an AudioBuffer<float> to a self-contained 24-bit FLAC blob in
// memory. FLAC's STREAMINFO carries sampleRate / channels / sampleCount,
// so the blob is self-describing on decode; JSON metadata still mirrors
// them so the library UI can describe a preset without decoding.

bool encodeAudioToFlac(juce::MemoryBlock& outBlob,
                       const juce::AudioBuffer<float>& buffer,
                       double sampleRate)
{
    outBlob.reset();
    const int numChannels = buffer.getNumChannels();
    const int numSamples = buffer.getNumSamples();
    if (numChannels <= 0 || numSamples <= 0) return false;

    auto stream = std::make_unique<juce::MemoryOutputStream>(outBlob, false);

    juce::FlacAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        fmt.createWriterFor(stream.get(),
                            sampleRate > 0.0 ? sampleRate : 44100.0,
                            static_cast<unsigned int>(numChannels),
                            24,
                            {},
                            5));
    if (writer == nullptr) return false;
    // createWriterFor took ownership of the raw pointer on success.
    stream.release();

    if (!writer->writeFromAudioSampleBuffer(buffer, 0, numSamples))
        return false;

    // Destruct the writer to flush FLAC trailer and delete the stream.
    writer.reset();
    return outBlob.getSize() > 0;
}

bool decodeAudioFromFlac(juce::AudioBuffer<float>& outBuffer,
                         const void* flacBytes,
                         size_t flacByteLen)
{
    if (flacBytes == nullptr || flacByteLen == 0) return false;

    auto stream = std::make_unique<juce::MemoryInputStream>(flacBytes, flacByteLen, false);

    juce::FlacAudioFormat fmt;
    std::unique_ptr<juce::AudioFormatReader> reader(
        fmt.createReaderFor(stream.get(), false));
    if (reader == nullptr) return false;
    stream.release();  // reader owns the stream now

    const int numChannels = static_cast<int>(reader->numChannels);
    const int numSamples  = static_cast<int>(reader->lengthInSamples);
    if (numChannels <= 0 || numSamples <= 0) return false;

    outBuffer.setSize(numChannels, numSamples);
    return reader->read(&outBuffer, 0, numSamples, 0, true, true);
}

// Reads the next audio payload from a binary container at `offset`. For
// v3 the byte length is implicit (rawSamples * channels * sizeof(float));
// for v4 a uint32 LE length prefix precedes a self-contained FLAC blob.
// Advances `offset` past the payload on success. Returns false if the
// payload is malformed or runs past `size`.
bool readAudioPayload(juce::AudioBuffer<float>& outBuffer,
                      const uint8_t* data, size_t size, size_t& offset,
                      uint32_t version,
                      int expectedChannels, int expectedSamples)
{
    if (version == 3)
    {
        if (expectedChannels <= 0 || expectedSamples <= 0) return false;
        const size_t audioBytes = static_cast<size_t>(expectedSamples * expectedChannels)
                                  * sizeof(float);
        if (offset + audioBytes > size) return false;

        const auto* pcm = reinterpret_cast<const float*>(data + offset);
        outBuffer.setSize(expectedChannels, expectedSamples);
        for (int s = 0; s < expectedSamples; ++s)
            for (int c = 0; c < expectedChannels; ++c)
                outBuffer.setSample(c, s, pcm[s * expectedChannels + c]);
        offset += audioBytes;
        return true;
    }

    // v4: length-prefixed FLAC blob
    if (offset + 4 > size) return false;
    uint32_t byteLen = 0;
    std::memcpy(&byteLen, data + offset, 4);
    offset += 4;
    if (offset + byteLen > size) return false;

    const bool ok = decodeAudioFromFlac(outBuffer, data + offset, byteLen);
    offset += byteLen;
    return ok;
}
}

// ═══════════════════════════════════════════════════════════════════
// Save: T5YN header + JSON + length-prefixed FLAC audio blobs (v4)
// ═══════════════════════════════════════════════════════════════════

bool PresetFormat::saveToFile(const juce::File& file, T5ynthProcessor& processor,
                              bool includeInferenceCache,
                              const std::vector<SnapshotState>* snapshots)
{
    // Build JSON (reuse existing export + add meta/embeddings)
    juce::String jsonBase = processor.exportJsonPreset();
    auto parsed = juce::JSON::parse(jsonBase);
    if (!parsed.isObject()) return false;

    auto* root = parsed.getDynamicObject();
    root->setProperty("name",
                      processor.getLastPresetName().trim().isNotEmpty()
                          ? processor.getLastPresetName().trim()
                          : file.getFileNameWithoutExtension());

    // Tags are always written, even when empty, so clearing all tags is an
    // explicit saved state rather than falling back to legacy inference.
    {
        const auto& tags = processor.getLastTags();
        juce::Array<juce::var> tagArr;
        for (auto& t : tags) tagArr.add(t);
        root->setProperty("tags", tagArr);
    }

    // Patch in prompts (exportJsonPreset leaves them empty). Persist the durable
    // HUMAN prompts, not the live editors: while a Re-Prompt loop is/was running the
    // editors (and lastPromptA/B, which mirror them) can hold a machine rewrite, and
    // a missed deactivation-restore or buffer reload can leave that rewrite stranded
    // in an editor even with the stance Off. The human store is written only by
    // human authorship (onTextChange / preset load), never by the loop, so it always
    // reproduces the user's starting point. Fall back to lastPromptA/B only when
    // nothing has been authored yet (legacy / fresh empty state). (The stance +
    // coupling are saved by exportJsonPreset and both restored on load — see
    // importJsonPreset; the DAW host-state path still forces the stance Off.)
    if (auto* synth = root->getProperty("synth").getDynamicObject())
    {
        const bool human = processor.hasHumanPrompts();
        synth->setProperty("promptA", human ? processor.getHumanPromptA() : processor.getLastPromptA());
        synth->setProperty("promptB", human ? processor.getHumanPromptB() : processor.getLastPromptB());
        synth->setProperty("seed", processor.getLastSeed());
        synth->setProperty("model", processor.getLastModel());
        // The Easy/Adv seed-mode UI does not write back to PID::genSeed, so we
        // can't infer "auto" from the APVTS value. The processor caches the
        // user's auto/random choice in lastRandomSeed (kept in sync by
        // PromptPanel on every toggle / Easy-mode button click / preset load).
        synth->setProperty("randomSeed", processor.getLastRandomSeed());
        // Persist the most recent inference duration (ms). Older presets
        // miss this field; readers default to 0 → display "—".
        if (processor.getLastGenerationTimeMs() > 0.0f)
            synth->setProperty("inferenceMs",
                               static_cast<double>(processor.getLastGenerationTimeMs()));

        // Research-mode injection state. Always written; readers use
        // hasProperty() defaults so old .t5p files predating these fields
        // load gracefully into the panel's current values.
        synth->setProperty("injectionMode",  processor.getLastInjectionMode());
        synth->setProperty("lateMixAmount",  static_cast<double>(processor.getLastLateMixAmount()));
        synth->setProperty("splitStart",     static_cast<double>(processor.getLastSplitStart()));
        synth->setProperty("splitEnd",       static_cast<double>(processor.getLastSplitEnd()));
    }

    // Semantic axes (GUI-only state, 3 slots)
    {
        juce::Array<juce::var> axesArr;
        const auto& axes = processor.getLastAxes();
        for (int i = 0; i < 3; ++i)
        {
            juce::DynamicObject::Ptr ax = new juce::DynamicObject();
            ax->setProperty("dropdownId", axes[static_cast<size_t>(i)].dropdownId);
            ax->setProperty("value", static_cast<double>(axes[static_cast<size_t>(i)].value));
            axesArr.add(ax.get());
        }
        root->setProperty("semanticAxes", axesArr);
    }

    // Audio metadata
    const auto& audioBuf = processor.getGeneratedAudio();
    int numSamples = audioBuf.getNumSamples();
    int numChannels = audioBuf.getNumChannels();

    juce::DynamicObject::Ptr meta = new juce::DynamicObject();
    meta->setProperty("sampleRate", processor.getGeneratedSampleRate());
    meta->setProperty("channels", numChannels);
    meta->setProperty("numSamples", numSamples);
    root->setProperty("audio_meta", meta.get());

    // Embeddings
    const auto& embA = processor.getLastEmbeddingA();
    const auto& embB = processor.getLastEmbeddingB();
    if (!embA.empty())
    {
        juce::Array<juce::var> arrA, arrB;
        for (float v : embA) arrA.add(static_cast<double>(v));
        for (float v : embB) arrB.add(static_cast<double>(v));
        root->setProperty("embeddingA", arrA);
        root->setProperty("embeddingB", arrB);
    }

    // FILL, not depth. An armed-but-empty cache has nothing to write, and a file
    // carrying a bare depth arms a cache on a machine that asked for nothing -
    // and, with the take flag beside it, brings that machine up locked in
    // replay. The two caches are asked separately although one switch governs
    // both: the switch says "include what there is", not "include the other
    // one's depth as well".
    const bool writeInferenceCache = includeInferenceCache && processor.getInferenceCacheFillCount() > 0;
    const auto& inferenceCacheEntries = processor.getInferenceCacheEntries();
    if (writeInferenceCache)
    {
        juce::DynamicObject::Ptr cacheMeta = new juce::DynamicObject();
        cacheMeta->setProperty("capacity", processor.getInferenceCacheCapacity());
        // Whether these entries were RECORDED as an offline take travels with them:
        // it is what decides that they replay over a running Re-Prompt stance.
        cacheMeta->setProperty("offlineTake", processor.isInferenceCacheOfflineTake());
        juce::Array<juce::var> entries;
        for (const auto& entry : inferenceCacheEntries)
        {
            // Skip empty buffers in lockstep with writeCompressedAudio below —
            // otherwise the JSON entry would have no corresponding payload
            // and the read cursor would desync.
            if (entry.audio.getNumSamples() <= 0 || entry.audio.getNumChannels() <= 0)
                continue;
            juce::DynamicObject::Ptr e = new juce::DynamicObject();
            e->setProperty("sampleRate", entry.sampleRate);
            e->setProperty("channels", entry.audio.getNumChannels());
            e->setProperty("numSamples", entry.audio.getNumSamples());
            entries.add(e.get());
        }
        cacheMeta->setProperty("entries", entries);
        root->setProperty("inferenceCache", cacheMeta.get());
    }

    // The LRO's cache, under the same switch as the neural one: "include the
    // cache" is one decision about the file, not one per oscillator.
    if (includeInferenceCache && processor.getCsoundCacheFillCount() > 0)
    {
        juce::DynamicObject::Ptr lroMeta = new juce::DynamicObject();
        lroMeta->setProperty("capacity", processor.getCsoundCacheCapacity());
        lroMeta->setProperty("offlineTake", processor.isCsoundCacheOfflineTake());
        juce::Array<juce::var> entries;
        for (const auto& e : processor.getCsoundCacheEntries())
        {
            if (e.orchestra.isEmpty())
                continue;   // an entry with no orchestra has nothing to install
            juce::DynamicObject::Ptr o = new juce::DynamicObject();
            o->setProperty("orchestra",  e.orchestra);
            o->setProperty("prompt",     e.prompt);
            o->setProperty("reading",    e.reading);
            o->setProperty("paramsText", e.paramsText);
            o->setProperty("authorModel", e.authorModel);
            o->setProperty("controls",   e.controls);
            o->setProperty("settings",   e.settings);
            entries.add(o.get());
        }
        lroMeta->setProperty("entries", entries);
        root->setProperty("csoundCache", lroMeta.get());
    }

    const auto sequencerOneShots = processor.exportSequencerOneShotSamples();
    if (!sequencerOneShots.empty())
    {
        juce::Array<juce::var> entries;
        for (const auto& slot : sequencerOneShots)
        {
            // Skip empty buffers in lockstep with writeCompressedAudio below.
            if (slot.audio.getNumSamples() <= 0 || slot.audio.getNumChannels() <= 0)
                continue;
            juce::DynamicObject::Ptr e = new juce::DynamicObject();
            e->setProperty("step", slot.step);
            e->setProperty("slot", slot.slot);
            e->setProperty("mode", static_cast<int>(slot.mode));
            e->setProperty("label", slot.label);
            e->setProperty("sampleRate", slot.sampleRate);
            e->setProperty("channels", slot.audio.getNumChannels());
            e->setProperty("numSamples", slot.audio.getNumSamples());
            entries.add(e.get());
        }

        if (auto* seq = root->getProperty("sequencer").getDynamicObject())
            seq->setProperty("oneShotSamples", entries);
    }

    // Per-slot snapshots. Only valid slots with non-empty audio are persisted
    // — empty slots are simply omitted, mirroring the cache/one-shots policy
    // so the JSON ↔ payload positional mapping stays in lockstep.
    if (snapshots != nullptr && !snapshots->empty())
    {
        juce::Array<juce::var> snapsArr;
        for (const auto& snap : *snapshots)
        {
            if (!snap.valid) continue;
            if (snap.audio.getNumSamples() <= 0 || snap.audio.getNumChannels() <= 0) continue;

            juce::DynamicObject::Ptr s = new juce::DynamicObject();
            s->setProperty("slot", snap.slot);
            s->setProperty("promptA", snap.promptA);
            s->setProperty("promptB", snap.promptB);
            s->setProperty("device", snap.device);
            s->setProperty("model", snap.model);
            s->setProperty("injectionMode", snap.injectionMode);
            s->setProperty("seed", snap.seed);
            s->setProperty("randomSeed", snap.randomSeed);
            s->setProperty("lateMixAmount", static_cast<double>(snap.lateMixAmount));
            s->setProperty("splitStart",    static_cast<double>(snap.splitStart));
            s->setProperty("splitEnd",      static_cast<double>(snap.splitEnd));

            juce::Array<juce::var> axesArr;
            for (const auto& ax : snap.axes)
            {
                juce::DynamicObject::Ptr a = new juce::DynamicObject();
                a->setProperty("dropdownId", ax.dropdownId);
                a->setProperty("value", static_cast<double>(ax.value));
                axesArr.add(a.get());
            }
            s->setProperty("axes", axesArr);

            if (!snap.embeddingA.empty())
            {
                juce::Array<juce::var> snapEmbA, snapEmbB;
                for (float v : snap.embeddingA) snapEmbA.add(static_cast<double>(v));
                for (float v : snap.embeddingB) snapEmbB.add(static_cast<double>(v));
                s->setProperty("embeddingA", snapEmbA);
                s->setProperty("embeddingB", snapEmbB);
            }

            if (!snap.dimensionOffsets.empty())
            {
                juce::Array<juce::var> dimArr;
                for (const auto& d : snap.dimensionOffsets)
                {
                    juce::DynamicObject::Ptr e = new juce::DynamicObject();
                    e->setProperty("dim", d.first);
                    e->setProperty("offset", static_cast<double>(d.second));
                    dimArr.add(e.get());
                }
                s->setProperty("dimensionOffsets", dimArr);
            }

            if (snap.parametersXml.isNotEmpty())
                s->setProperty("parametersXml", snap.parametersXml);

            s->setProperty("loopStart",      static_cast<double>(snap.loopStart));
            s->setProperty("loopEnd",        static_cast<double>(snap.loopEnd));
            s->setProperty("startPos",       static_cast<double>(snap.startPos));
            s->setProperty("wtExtractStart", static_cast<double>(snap.wtExtractStart));
            s->setProperty("wtExtractEnd",   static_cast<double>(snap.wtExtractEnd));
            s->setProperty("pointsLocked",   snap.pointsLocked);

            s->setProperty("sampleRate", snap.sampleRate);
            s->setProperty("channels",   snap.audio.getNumChannels());
            s->setProperty("numSamples", snap.audio.getNumSamples());

            snapsArr.add(s.get());
        }
        if (!snapsArr.isEmpty())
            root->setProperty("snapshots", snapsArr);
    }

    // LCO/DCO bake — frame/station data. Patched in here (not
    // exportJsonPreset) because it requires a FRESH DSP read
    // (snapshotLevel0Frames/snapshotAdditiveBank), mirroring how audio_meta
    // above is computed from live engine state rather than cached earlier.
    // exportJsonPreset already wrote the "lco" object (prompt/readings/
    // balance/stance) when a bake snapshot exists; this only adds the
    // frame-count metadata for A and B's station data. A's actual frame
    // samples are appended as a trailing FLAC blob below.
    juce::AudioBuffer<float> lcoFramesABuffer;
    if (auto* lco = root->getProperty("lco").getDynamicObject())
    {
        if (processor.getLcoOscAHasContent())
        {
            std::vector<float> flat;
            int frameSize = 0, numFrames = 0;
            if (processor.getMasterOscConst().snapshotLevel0Frames(flat, frameSize, numFrames)
                && numFrames > 0)
            {
                lco->setProperty("frameSize", frameSize);
                lco->setProperty("frameCountA", numFrames);
                lcoFramesABuffer.setSize(1, static_cast<int>(flat.size()));
                lcoFramesABuffer.copyFrom(0, 0, flat.data(), static_cast<int>(flat.size()));
            }
        }

        // The dual A+B oscillator's B half is dead (BJ 2026-07-17): B additive
        // stations are no longer authored or exported. Old presets that still
        // carry a stationsB block parse it (below) but it is never restored.
    }

    juce::String json = juce::JSON::toString(parsed, true);
    auto jsonData = json.toRawUTF8();
    uint32_t jsonLen = static_cast<uint32_t>(json.getNumBytesAsUTF8());

    // Write file
    auto targetFile = file.withFileExtension("t5p");
    if (!targetFile.getParentDirectory().exists())
        targetFile.getParentDirectory().createDirectory();

    juce::TemporaryFile tempFile(targetFile);
    juce::FileOutputStream out(tempFile.getFile());
    if (out.failedToOpen()) return false;

    // Magic + version
    out.write(kMagic, 4);
    out.writeInt(static_cast<int>(kVersion));

    // JSON section
    out.writeInt(static_cast<int>(jsonLen));
    out.write(jsonData, static_cast<size_t>(jsonLen));

    // Each audio payload is encoded as 24-bit FLAC into a memory blob and
    // written as [uint32 LE byteLen][FLAC bytes]. FLAC carries channels /
    // sampleRate / sampleCount itself; JSON metadata mirrors them for UI.
    bool audioWriteOk = true;
    auto writeCompressedAudio = [&out, &audioWriteOk](const juce::AudioBuffer<float>& buffer,
                                                      double sampleRate)
    {
        if (buffer.getNumSamples() <= 0 || buffer.getNumChannels() <= 0) return;

        juce::MemoryBlock flacBlob;
        if (!encodeAudioToFlac(flacBlob, buffer, sampleRate))
        {
            audioWriteOk = false;
            return;
        }
        out.writeInt(static_cast<int>(flacBlob.getSize()));
        out.write(flacBlob.getData(), flacBlob.getSize());
    };

    if (numSamples > 0 && numChannels > 0)
        writeCompressedAudio(audioBuf, processor.getGeneratedSampleRate());

    if (writeInferenceCache)
        for (const auto& entry : inferenceCacheEntries)
            writeCompressedAudio(entry.audio, entry.sampleRate);

    for (const auto& slot : sequencerOneShots)
        writeCompressedAudio(slot.audio, slot.sampleRate);

    if (snapshots != nullptr)
        for (const auto& snap : *snapshots)
        {
            if (!snap.valid) continue;
            writeCompressedAudio(snap.audio, snap.sampleRate);
        }

    // LCO frames A — trailing, purely additive blob (v4 readers never see
    // it; their cursor logic stops after the snapshot blobs above). Only
    // written when the JSON "lco.frameCountA" was actually set above.
    if (lcoFramesABuffer.getNumSamples() > 0)
        writeCompressedAudio(lcoFramesABuffer,
                             processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 44100.0);

    if (!audioWriteOk) return false;

    out.flush();
    if (!out.getStatus().wasOk())
        return false;

    return tempFile.overwriteTargetFileWithTemporary();
}

// ═══════════════════════════════════════════════════════════════════
// Load: auto-detect format (T5YN binary v3/v4 / legacy JSON / legacy XML)
// ═══════════════════════════════════════════════════════════════════

PresetFormat::LoadResult PresetFormat::loadFromFile(const juce::File& file, T5ynthProcessor& processor)
{
    LoadResult result;
    if (!file.existsAsFile()) return result;

    juce::MemoryBlock fileData;
    if (!file.loadFileAsData(fileData)) return result;

    auto* data = static_cast<const char*>(fileData.getData());
    auto size = fileData.getSize();

    // ── Detect format ──
    bool isBinary = (size >= 12 && std::memcmp(data, kMagic, 4) == 0);

    if (isBinary)
    {
        // ── Binary format (v3 raw-PCM or v4 FLAC) ──
        auto* bytes = reinterpret_cast<const uint8_t*>(data);
        uint32_t version = *reinterpret_cast<const uint32_t*>(bytes + 4);
        if (version < kMinLoadableVersion || version > kVersion)
        {
            // Unknown / future version: refuse to parse rather than silently
            // mis-interpret the binary tail under the wrong schema.
            DBG("PresetFormat: unsupported .t5p version " << (int) version
                << " (loader accepts " << (int) kMinLoadableVersion
                << ".." << (int) kVersion << ")");
            return result;
        }
        uint32_t jsonLen = *reinterpret_cast<const uint32_t*>(bytes + 8);

        if (12 + jsonLen > size) return result;

        // Decode the JSON blob as UTF-8 explicitly. juce::String(const char*, size_t)
        // interprets bytes via CharPointer_ASCII (each byte → a codepoint, i.e. Latin-1),
        // which mangles multi-byte UTF-8: "ü" (C3 BC) became "Ã¼" on every reload.
        // The write side already emits UTF-8 (json.toRawUTF8 / getNumBytesAsUTF8).
        juce::String json = juce::String::fromUTF8(data + 12, static_cast<int>(jsonLen));
        if (!processor.importJsonPreset(json)) return result;

        // Parse JSON for metadata
        auto parsed = juce::JSON::parse(json);
        auto* root = parsed.getDynamicObject();
        if (!root) return result;

        // Extract prompts/seed
        if (auto* synth = root->getProperty("synth").getDynamicObject())
        {
            result.promptA = synth->getProperty("promptA").toString();
            result.promptB = synth->getProperty("promptB").toString();
            int s = static_cast<int>(synth->getProperty("seed"));
            if (s > 0) result.seed = s;
            result.device = synth->getProperty("device").toString();
            result.model = synth->getProperty("model").toString();
            result.calibEpoch = synth->hasProperty("calibEpoch")
                                    ? static_cast<int>(synth->getProperty("calibEpoch")) : 0;
            if (synth->hasProperty("randomSeed"))
                result.randomSeed = static_cast<bool>(synth->getProperty("randomSeed"));

            // Research-mode injection state. All four fields are independent —
            // a half-populated preset (e.g. only injectionMode set) is honoured
            // for the present field and leaves the others at NaN/empty so the
            // panel keeps its existing values. Old .t5p without any of these
            // fields → all four stay default → panel state is preserved.
            if (synth->hasProperty("injectionMode"))
                result.injectionMode = synth->getProperty("injectionMode").toString();
            if (synth->hasProperty("lateMixAmount"))
                result.lateMixAmount = static_cast<float>(
                    static_cast<double>(synth->getProperty("lateMixAmount")));
            if (synth->hasProperty("splitStart"))
                result.splitStart = static_cast<float>(
                    static_cast<double>(synth->getProperty("splitStart")));
            if (synth->hasProperty("splitEnd"))
                result.splitEnd = static_cast<float>(
                    static_cast<double>(synth->getProperty("splitEnd")));
        }

        // Extract semantic axes
        if (auto* axesArr = root->getProperty("semanticAxes").getArray())
        {
            for (int i = 0; i < std::min(axesArr->size(), 3); ++i)
            {
                if (auto* ax = (*axesArr)[i].getDynamicObject())
                {
                    result.axes[static_cast<size_t>(i)].dropdownId =
                        static_cast<int>(ax->getProperty("dropdownId"));
                    result.axes[static_cast<size_t>(i)].value =
                        static_cast<float>(ax->getProperty("value"));
                }
            }
            result.hasAxes = true;
        }

        // Extract embeddings
        if (auto* embAArr = root->getProperty("embeddingA").getArray())
        {
            result.embeddingA.reserve(static_cast<size_t>(embAArr->size()));
            for (auto& v : *embAArr) result.embeddingA.push_back(static_cast<float>(v));
        }
        if (auto* embBArr = root->getProperty("embeddingB").getArray())
        {
            result.embeddingB.reserve(static_cast<size_t>(embBArr->size()));
            for (auto& v : *embBArr) result.embeddingB.push_back(static_cast<float>(v));
        }

        // Extract audio payloads. The same offset cursor walks through
        // primary audio, then cache entries, then one-shots — in that fixed
        // order. readAudioPayload picks the right decoder based on version
        // (v3 = raw float32 with implicit length from JSON metadata,
        //  v4 = uint32-LE byteLen prefix + self-describing FLAC blob).
        size_t cursor = 12 + static_cast<size_t>(jsonLen);
        if (auto* am = root->getProperty("audio_meta").getDynamicObject())
        {
            int numChannels = static_cast<int>(am->getProperty("channels"));
            int numSamples = static_cast<int>(am->getProperty("numSamples"));
            result.sampleRate = static_cast<double>(am->getProperty("sampleRate"));

            if (numSamples > 0 && numChannels > 0)
            {
                if (readAudioPayload(result.audio, bytes, size, cursor, version,
                                     numChannels, numSamples))
                    result.hasAudio = true;
            }
        }

        // Optional inference-cache tail. Each entry is one length-prefixed
        // FLAC blob in v4 (or one raw-PCM run in v3).
        if (auto* lroMeta = root->getProperty("csoundCache").getDynamicObject())
        {
            result.csoundCacheCapacity = juce::jmax(0, static_cast<int>(lroMeta->getProperty("capacity")));
            result.csoundCacheIsOfflineTake = static_cast<bool>(lroMeta->getProperty("offlineTake"));
            if (const auto* entries = lroMeta->getProperty("entries").getArray())
                for (const auto& v : *entries)
                    if (auto* o = v.getDynamicObject())
                    {
                        LoadResult::CsoundCacheAuthored e;
                        e.orchestra  = o->getProperty("orchestra").toString();
                        if (e.orchestra.isEmpty())
                            continue;
                        e.prompt     = o->getProperty("prompt").toString();
                        e.reading    = o->getProperty("reading").toString();
                        e.paramsText = o->getProperty("paramsText").toString();
                        // Absent in files written before the slot carried it -
                        // which reads as "author not known", the honest answer.
                        e.authorModel = o->getProperty("authorModel").toString();
                        e.controls   = o->getProperty("controls");
                        e.settings   = o->getProperty("settings");
                        result.csoundCache.push_back(std::move(e));
                    }
            // What the file CARRIES wins, exactly as on the neural side: the
            // depth is widened to hold every entry rather than the entries being
            // dropped to fit a stored depth.
            result.csoundCacheCapacity = juce::jmax(result.csoundCacheCapacity,
                                                    static_cast<int>(result.csoundCache.size()));
        }

        if (auto* cacheMeta = root->getProperty("inferenceCache").getDynamicObject())
        {
            result.inferenceCacheCapacity = juce::jmax(0, static_cast<int>(cacheMeta->getProperty("capacity")));
            // Absent in presets written before the offline take existed → false,
            // which is the behaviour those files were saved under.
            result.inferenceCacheIsOfflineTake = static_cast<bool>(cacheMeta->getProperty("offlineTake"));
            if (auto* entries = cacheMeta->getProperty("entries").getArray())
            {
                for (auto& ev : *entries)
                {
                    auto* em = ev.getDynamicObject();
                    if (em == nullptr) { result.inferenceCache.clear(); result.inferenceCacheCapacity = 0;
                                     result.inferenceCacheIsOfflineTake = false; break; }

                    const int numChannels = static_cast<int>(em->getProperty("channels"));
                    const int numSamples = static_cast<int>(em->getProperty("numSamples"));
                    const double sr = static_cast<double>(em->getProperty("sampleRate"));
                    if (numSamples <= 0 || numChannels <= 0)
                    {
                        result.inferenceCache.clear();
                        result.inferenceCacheCapacity = 0;
                        result.inferenceCacheIsOfflineTake = false;
                        break;
                    }

                    LoadResult::InferenceCacheAudio cacheAudio;
                    cacheAudio.sampleRate = sr > 0.0 ? sr : 44100.0;
                    if (!readAudioPayload(cacheAudio.audio, bytes, size, cursor, version,
                                          numChannels, numSamples))
                    {
                        result.inferenceCache.clear();
                        result.inferenceCacheCapacity = 0;
                        result.inferenceCacheIsOfflineTake = false;
                        break;
                    }
                    result.inferenceCache.push_back(std::move(cacheAudio));
                }
            }
            result.inferenceCacheCapacity = juce::jmax(result.inferenceCacheCapacity,
                                                       static_cast<int>(result.inferenceCache.size()));
        }

        if (auto* seq = root->getProperty("sequencer").getDynamicObject())
        {
            if (auto* entries = seq->getProperty("oneShotSamples").getArray())
            {
                std::vector<T5ynthProcessor::SequencerOneShotExport> slots;
                for (auto& ev : *entries)
                {
                    auto* em = ev.getDynamicObject();
                    if (em == nullptr) { slots.clear(); break; }

                    const int step = static_cast<int>(em->getProperty("step"));
                    const int slot = static_cast<int>(em->getProperty("slot"));
                    const int mode = juce::jlimit(0, 2, static_cast<int>(em->getProperty("mode")));
                    const int numChannels = static_cast<int>(em->getProperty("channels"));
                    const int numSamples = static_cast<int>(em->getProperty("numSamples"));
                    const double sr = static_cast<double>(em->getProperty("sampleRate"));
                    if (numSamples <= 0 || numChannels <= 0)
                    {
                        slots.clear();
                        break;
                    }

                    T5ynthProcessor::SequencerOneShotExport slotAudio;
                    slotAudio.step = step;
                    slotAudio.slot = slot;
                    slotAudio.mode = static_cast<T5ynthStepSequencer::OneShotMode>(mode);
                    slotAudio.label = em->getProperty("label").toString();
                    slotAudio.sampleRate = sr > 0.0 ? sr : 44100.0;
                    if (!readAudioPayload(slotAudio.audio, bytes, size, cursor, version,
                                          numChannels, numSamples))
                    {
                        slots.clear();
                        break;
                    }

                    slots.push_back(std::move(slotAudio));
                }

                if (!slots.empty())
                    processor.importSequencerOneShotSamples(slots);
            }
        }

        // Per-slot snapshots. One length-prefixed FLAC blob per JSON entry,
        // in the same fixed cursor order. Snapshots are append-safe — old
        // v4 files without a "snapshots" key skip this block entirely.
        if (auto* snapsArr = root->getProperty("snapshots").getArray())
        {
            for (auto& sv : *snapsArr)
            {
                auto* sm = sv.getDynamicObject();
                if (sm == nullptr) { result.snapshots.clear(); break; }

                const int numChannels = static_cast<int>(sm->getProperty("channels"));
                const int numSamples  = static_cast<int>(sm->getProperty("numSamples"));
                const double sr       = static_cast<double>(sm->getProperty("sampleRate"));
                if (numSamples <= 0 || numChannels <= 0)
                {
                    result.snapshots.clear();
                    break;
                }

                SnapshotState snap;
                snap.slot           = static_cast<int>(sm->getProperty("slot"));
                snap.promptA        = sm->getProperty("promptA").toString();
                snap.promptB        = sm->getProperty("promptB").toString();
                snap.device         = sm->getProperty("device").toString();
                snap.model          = sm->getProperty("model").toString();
                snap.injectionMode  = sm->hasProperty("injectionMode")
                                          ? sm->getProperty("injectionMode").toString()
                                          : juce::String("linear");
                snap.seed           = static_cast<int>(sm->getProperty("seed"));
                snap.randomSeed     = static_cast<bool>(sm->getProperty("randomSeed"));
                if (sm->hasProperty("lateMixAmount"))
                    snap.lateMixAmount = static_cast<float>(
                        static_cast<double>(sm->getProperty("lateMixAmount")));
                if (sm->hasProperty("splitStart"))
                    snap.splitStart = static_cast<float>(
                        static_cast<double>(sm->getProperty("splitStart")));
                if (sm->hasProperty("splitEnd"))
                    snap.splitEnd = static_cast<float>(
                        static_cast<double>(sm->getProperty("splitEnd")));

                if (auto* axesArr = sm->getProperty("axes").getArray())
                {
                    for (int i = 0; i < std::min(axesArr->size(), 3); ++i)
                    {
                        if (auto* ax = (*axesArr)[i].getDynamicObject())
                        {
                            snap.axes[static_cast<size_t>(i)].dropdownId =
                                static_cast<int>(ax->getProperty("dropdownId"));
                            snap.axes[static_cast<size_t>(i)].value =
                                static_cast<float>(ax->getProperty("value"));
                        }
                    }
                }

                if (auto* embA = sm->getProperty("embeddingA").getArray())
                {
                    snap.embeddingA.reserve(static_cast<size_t>(embA->size()));
                    for (auto& v : *embA)
                        snap.embeddingA.push_back(static_cast<float>(v));
                }
                if (auto* embB = sm->getProperty("embeddingB").getArray())
                {
                    snap.embeddingB.reserve(static_cast<size_t>(embB->size()));
                    for (auto& v : *embB)
                        snap.embeddingB.push_back(static_cast<float>(v));
                }

                if (auto* dimArr = sm->getProperty("dimensionOffsets").getArray())
                {
                    for (auto& dv : *dimArr)
                    {
                        if (auto* dm = dv.getDynamicObject())
                        {
                            const int dim = static_cast<int>(dm->getProperty("dim"));
                            const float off = static_cast<float>(
                                static_cast<double>(dm->getProperty("offset")));
                            snap.dimensionOffsets.emplace_back(dim, off);
                        }
                    }
                }

                snap.parametersXml = sm->getProperty("parametersXml").toString();

                snap.loopStart      = static_cast<float>(static_cast<double>(sm->getProperty("loopStart")));
                snap.loopEnd        = sm->hasProperty("loopEnd")
                                          ? static_cast<float>(static_cast<double>(sm->getProperty("loopEnd")))
                                          : 1.0f;
                snap.startPos       = static_cast<float>(static_cast<double>(sm->getProperty("startPos")));
                snap.wtExtractStart = static_cast<float>(static_cast<double>(sm->getProperty("wtExtractStart")));
                snap.wtExtractEnd   = sm->hasProperty("wtExtractEnd")
                                          ? static_cast<float>(static_cast<double>(sm->getProperty("wtExtractEnd")))
                                          : 1.0f;
                snap.pointsLocked   = static_cast<bool>(sm->getProperty("pointsLocked"));

                snap.sampleRate = sr > 0.0 ? sr : 44100.0;
                if (!readAudioPayload(snap.audio, bytes, size, cursor, version,
                                      numChannels, numSamples))
                {
                    result.snapshots.clear();
                    break;
                }
                snap.valid = true;
                result.snapshots.push_back(std::move(snap));
            }
        }

        // LCO/DCO bake (v5+). Absence = not an LCO preset (v4 fallback, or a
        // v5 file saved with no active bake) — hasLco stays false and the
        // caller skips the restore entirely. A trailing FLAC blob carries A's
        // exact baked frames when frameCountA > 0; a corrupt/short tail
        // degrades to hasLco=false rather than failing the whole load, since
        // the JSON-declared blob order after this point is otherwise empty.
        if (auto* lco = root->getProperty("lco").getDynamicObject())
        {
            result.hasLco          = true;
            result.lcoPrompt       = lco->getProperty("prompt").toString();
            result.lcoReadingA     = lco->getProperty("readingA").toString();
            result.lcoReadingB     = lco->getProperty("readingB").toString();
            result.lcoMotionRateHz = static_cast<float>(static_cast<double>(lco->getProperty("motionRateHz")));
            result.lcoOscAHasContent = static_cast<bool>(lco->getProperty("oscAHasContent"));
            result.lcoGainA        = static_cast<float>(static_cast<double>(lco->getProperty("gainA")));
            result.lcoOscBHasContent = static_cast<bool>(lco->getProperty("oscBHasContent"));
            result.lcoGainB        = static_cast<float>(static_cast<double>(lco->getProperty("gainB")));

            const int frameCountA = lco->hasProperty("frameCountA")
                                        ? static_cast<int>(lco->getProperty("frameCountA")) : 0;
            if (frameCountA > 0)
            {
                const int frameSize = lco->hasProperty("frameSize")
                    ? static_cast<int>(lco->getProperty("frameSize")) : WavetableOscillator::FRAME_SIZE;
                if (!readAudioPayload(result.lcoFramesA, bytes, size, cursor, version,
                                      /*numChannels=*/1, frameCountA * frameSize))
                {
                    result.hasLco = false;  // corrupt tail — degrade rather than fail the whole load
                    result.lcoFramesA.setSize(0, 0);
                }
            }

            if (auto* stationsArr = lco->getProperty("stationsB").getArray())
            {
                for (auto& stationVar : *stationsArr)
                {
                    std::vector<WavetableOscillator::AdditivePartial> station;
                    if (auto* partArr = stationVar.getArray())
                    {
                        for (auto& pv : *partArr)
                        {
                            if (auto* pm = pv.getDynamicObject())
                                station.push_back({
                                    static_cast<float>(static_cast<double>(pm->getProperty("h"))),
                                    static_cast<float>(static_cast<double>(pm->getProperty("a"))),
                                    static_cast<float>(static_cast<double>(pm->getProperty("phase"))) });
                        }
                    }
                    if (!station.empty())
                        result.lcoStationsB.push_back(std::move(station));
                }
            }
        }

        result.presetName = getStoredPresetName(parsed, file);
        result.tags = readTagsFromJson(parsed);
        result.success = true;
    }
    else
    {
        // ── Legacy format: try JSON first, then XML ──
        juce::String fileText = file.loadFileAsString();

        // Try JSON
        if (fileText.trimStart().startsWith("{"))
        {
            if (processor.importJsonPreset(fileText))
            {
                auto parsed = juce::JSON::parse(fileText);
                if (auto* root = parsed.getDynamicObject())
                {
                    if (auto* synth = root->getProperty("synth").getDynamicObject())
                    {
                        result.promptA = synth->getProperty("promptA").toString();
                        result.promptB = synth->getProperty("promptB").toString();
                        int s = static_cast<int>(synth->getProperty("seed"));
                        if (s > 0) result.seed = s;
                        result.device = synth->getProperty("device").toString();
                        result.model = synth->getProperty("model").toString();
                        if (synth->hasProperty("randomSeed"))
                            result.randomSeed = static_cast<bool>(synth->getProperty("randomSeed"));
                        if (synth->hasProperty("injectionMode"))
                            result.injectionMode = synth->getProperty("injectionMode").toString();
                        if (synth->hasProperty("lateMixAmount"))
                            result.lateMixAmount = static_cast<float>(
                                static_cast<double>(synth->getProperty("lateMixAmount")));
                        if (synth->hasProperty("splitStart"))
                            result.splitStart = static_cast<float>(
                                static_cast<double>(synth->getProperty("splitStart")));
                        if (synth->hasProperty("splitEnd"))
                            result.splitEnd = static_cast<float>(
                                static_cast<double>(synth->getProperty("splitEnd")));
                    }
                }
                result.presetName = getStoredPresetName(parsed, file);
                result.tags = readTagsFromJson(parsed);
                result.success = true;
            }
        }
        // Try XML (old APVTS dump)
        else if (fileText.trimStart().startsWith("<"))
        {
            auto xml = juce::XmlDocument::parse(fileText);
            if (xml != nullptr)
            {
                auto state = juce::ValueTree::fromXml(*xml);
                if (state.isValid())
                {
                    // Deliberately NOT calibration-migrated. The XML dump format
                    // existed for 8 days (db43dda0 → 3eea0375, 2026-03/04), and its
                    // env/LFO target tables were renumbered afterwards: mod*_target
                    // was DCA=0/Filter=1/Scan=2 (now Filter=2/Scan=3), lfo*_target
                    // was Filter=0/Scan=1 (now Filter=1/Scan=2). Every epoch-2
                    // CondRescale keys off those indices, so on a file of this
                    // vintage they fire on SCAN-routed envelopes/LFOs — ×2.5 into a
                    // 0..1 param, clamped, unrecoverable — while the filter-routed
                    // patches they exist for are skipped. No other entry can reach
                    // such a file: aftertouch_amt_*, gen_coordination_mode,
                    // delay/reverb_type and amp_target did not exist yet, and the
                    // epoch-4 remaps need indices above these tables' maxima.
                    // Migrating here would only do damage; fixing it properly means
                    // remapping the old target indices first, which is unwritten
                    // work for a format nothing has produced since April 2026.
                    processor.getValueTreeState().replaceState(state);
                    result.presetName = file.getFileNameWithoutExtension();
                    result.success = true;
                }
            }
        }
    }

    return result;
}

juce::File PresetFormat::getPresetsDirectory()
{
    return getUserPresetsDirectory();
}

juce::File PresetFormat::getUserPresetsDirectory()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("T5ynth")
        .getChildFile("presets");
    dir.createDirectory();
    return dir;
}

bool PresetFormat::userPresetsDirIsGitCheckout()
{
    // .git is a directory in a normal clone and a file in a linked
    // worktree — exists() covers both. Only called from explicit user
    // actions on the message thread, so the stat-per-call is fine and
    // stays correct if the checkout appears while the app is running.
    return getUserPresetsDirectory().getChildFile(".git").exists();
}

juce::File PresetFormat::getUserSequencesDirectory()
{
    auto dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("T5ynth")
        .getChildFile("sequences");
    dir.createDirectory();
    return dir;
}

juce::Array<juce::File> PresetFormat::getAllPresetFiles()
{
    juce::Array<juce::File> files;

    // Recursive scan so user-created subdirectories ("banks") show up in
    // the library.
    auto userDir = getUserPresetsDirectory();
    if (userDir.isDirectory())
        for (auto& f : userDir.findChildFiles(juce::File::findFiles, true, "*.t5p"))
            files.add(f);

    return files;
}
