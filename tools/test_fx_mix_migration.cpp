// Epoch-5 FX Mix migration, exercised through the REAL ValueTree code path.
//
// Checks the three things the closed-form maths alone cannot:
//   1. migrateValueTree finds the sibling type param and rewrites the mix value,
//   2. a file already stamped at the current epoch is left ALONE (no double
//      migration on a load -> save -> load cycle),
//   3. a tree carrying the mix but NOT its type is left untouched rather than
//      migrated against a guessed type.
//
// Build (after a Release build of the plugin):
//   clang++ -std=c++17 -O2 @/tmp/h.rsp tools/test_fx_mix_migration.cpp \
//     build_clean/T5ynth_artefacts/Release/lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] \
//     -framework Accelerate -framework AudioToolbox -framework Cocoa -framework CoreAudio \
//     -framework CoreAudioKit -framework CoreMIDI -framework DiscRecording -framework Foundation \
//     -framework IOKit -framework QuartzCore -framework Security -framework WebKit \
//     -weak_framework Metal -weak_framework MetalKit -o /tmp/test_fx_mix_migration
#include "JuceHeader.h"
#include "presets/CalibrationMigration.h"
#include <cstdio>
#include <cmath>

static int failures = 0;

static void check(bool ok, const char* what)
{
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (! ok) ++failures;
}

static juce::ValueTree makeTree(const char* mixId, float mix,
                                const char* typeId, int type, bool includeType)
{
    juce::ValueTree t("PARAMETERS");
    juce::ValueTree m("PARAM");
    m.setProperty("id", mixId, nullptr);
    m.setProperty("value", mix, nullptr);
    t.appendChild(m, nullptr);
    if (includeType)
    {
        juce::ValueTree ty("PARAM");
        ty.setProperty("id", typeId, nullptr);
        ty.setProperty("value", static_cast<float>(type), nullptr);
        t.appendChild(ty, nullptr);
    }
    return t;
}

static float mixOf(const juce::ValueTree& t, const char* id)
{
    for (int i = 0; i < t.getNumChildren(); ++i)
        if (t.getChild(i).getProperty("id").toString() == id)
            return static_cast<float>(t.getChild(i).getProperty("value"));
    return -1.0f;
}

// The wet/dry ratio a stored value produced under the PRE-epoch-5 regime.
static double oldRatio(double m, int wetPow, double pathGain)
{
    const double wet = (wetPow == 2) ? m * m : m;
    return (wet * pathGain) / (1.0 - m);
}
static double newRatio(double m)
{
    return FxMixLaw::wetGain(static_cast<float>(m)) / FxMixLaw::dryGain(static_cast<float>(m));
}
static double db(double x) { return 20.0 * std::log10(x); }

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    std::printf("epoch-5 FX Mix migration, through migrateValueTree\n\n");

    struct Case { const char* label; const char* mixId; const char* typeId;
                  int type; int oldPow; double oldGain; };
    const Case cases[] = {
        { "delay Digital", PID::delayMix,  PID::delayType,  DelayType::Digital, 1, 1.0 },
        { "delay Tape",    PID::delayMix,  PID::delayType,  DelayType::Tape,    1, 1.0 / FxMixLaw::kDelayTrimTape },
        { "delay BBD",     PID::delayMix,  PID::delayType,  DelayType::Bbd,     1, 1.0 / FxMixLaw::kDelayTrimBbd  },
        // The plate's OLD path gain is the retired kPlateWetGain = 2.0 comp times
        // the IR's own gain (= 1 / its trim) — NOT 2.0 alone. An energy-normalised
        // EMT-140 IR is 14-18 dB below unity, which is exactly the fact the first
        // measurement missed.
        { "reverb Plate Dk", PID::reverbMix, PID::reverbType, ReverbType::Dark,   1, 2.0 / FxMixLaw::kReverbTrimPlateDark },
        { "reverb Plate Md", PID::reverbMix, PID::reverbType, ReverbType::Medium, 1, 2.0 / FxMixLaw::kReverbTrimPlateMedium },
        { "reverb Plate Br", PID::reverbMix, PID::reverbType, ReverbType::Bright, 1, 2.0 / FxMixLaw::kReverbTrimPlateBright },
        { "reverb Freeverb", PID::reverbMix, PID::reverbType, ReverbType::Algo,   2, 1.0 / FxMixLaw::kReverbTrimAlgo },
        { "reverb Freeverb+",PID::reverbMix, PID::reverbType, ReverbType::AlgoPlus, 2, 1.0 / FxMixLaw::kReverbTrimAlgoPlus },
    };

    // 1. The balance a preset had must survive the migration.
    std::printf("wet/dry balance preserved (epoch 4 -> 5):\n");
    for (const auto& c : cases)
    {
        double worst = 0.0;
        for (double stored : { 0.03, 0.10, 0.25, 0.30, 0.50, 0.70 })
        {
            auto t = makeTree(c.mixId, static_cast<float>(stored), c.typeId, c.type, true);
            Calibration::migrateValueTree(t, 4);
            const double got = mixOf(t, c.mixId);
            worst = std::max(worst, std::fabs(db(newRatio(got))
                                              - db(oldRatio(stored, c.oldPow, c.oldGain))));
        }
        char msg[128];
        std::snprintf(msg, sizeof msg, "%-16s worst drift %.4f dB", c.label, worst);
        check(worst < 0.05, msg);
    }

    // 2. Idempotence: a file already at the current epoch must not move again.
    std::printf("\nno double migration (a migrated file is re-saved at kEpoch):\n");
    for (const auto& c : cases)
    {
        auto t = makeTree(c.mixId, 0.30f, c.typeId, c.type, true);
        Calibration::migrateValueTree(t, 4);
        const float once = mixOf(t, c.mixId);
        Calibration::migrateValueTree(t, Calibration::kEpoch);   // re-loaded after a save
        char msg[128];
        std::snprintf(msg, sizeof msg, "%-16s stays at %.4f", c.label, once);
        check(std::fabs(mixOf(t, c.mixId) - once) < 1e-6f, msg);
    }

    // 3. Mix present, type absent: leave it alone rather than guess a type.
    std::printf("\nmix without its type param is left untouched:\n");
    for (const auto& c : cases)
    {
        auto t = makeTree(c.mixId, 0.30f, c.typeId, c.type, false);
        Calibration::migrateValueTree(t, 4);
        char msg[128];
        std::snprintf(msg, sizeof msg, "%-16s still %.4f", c.label, mixOf(t, c.mixId));
        check(std::fabs(mixOf(t, c.mixId) - 0.30f) < 1e-6f, msg);
    }

    // 4. Endpoints must stay endpoints: 0 = bypass, 1 = pure wet.
    std::printf("\nendpoints:\n");
    for (const auto& c : cases)
    {
        auto t0 = makeTree(c.mixId, 0.0f, c.typeId, c.type, true);
        auto t1 = makeTree(c.mixId, 1.0f, c.typeId, c.type, true);
        Calibration::migrateValueTree(t0, 4);
        Calibration::migrateValueTree(t1, 4);
        char msg[128];
        std::snprintf(msg, sizeof msg, "%-16s 0 -> %.3f, 1 -> %.3f", c.label,
                      mixOf(t0, c.mixId), mixOf(t1, c.mixId));
        check(mixOf(t0, c.mixId) == 0.0f && mixOf(t1, c.mixId) == 1.0f, msg);
    }

    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL",
                failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
