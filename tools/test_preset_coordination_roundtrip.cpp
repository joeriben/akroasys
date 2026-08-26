// Preset persistence of the inter-strand coordination params (added
// 2026-07-16 — genCoordinationMode/genCoordinationCap were never persisted
// in .t5p before). Constructs the REAL processor and asserts, against
// exportJsonPreset/importJsonPreset (the .t5p JSON path):
//
//   1. export writes "coordination" (key string) + "coordinationCap"
//   2. import restores a saved non-default mode (dialog, cap 5)
//   3. a preset WITHOUT the keys (pre-2026-07-16) restores the DEFAULTS
//      (density_budget, cap 3) — NOT the session's current value, and NOT
//      choiceFromKey('')'s index 0 (= independent)
//   4. a legacy no-op key ("algebraic", removed 2026-07-16) maps to
//      index 0 = independent via choiceFromKey's unknown-key fallback
//
// Build: same recipe as the other tools/*.cpp (flags.make → h.rsp,
// link lib<product>_SharedCode.a  [tools/build_offline_tool.sh does this] + mac frameworks) PLUS
// build_clean/libT5ynthData.a — processor-level tools need BinaryData
// (plate IRs etc. referenced from prepareToPlay). Exits non-zero on failure.
#include "JuceHeader.h"
#include "PluginProcessor.h"
#include "dsp/BlockParams.h"
#include "presets/CalibrationMigration.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("  FAIL: %s\n", msg); ++failures; } \
                              else { std::printf("  ok:   %s\n", msg); } } while(0)

static int coordModeIndex (T5ynthProcessor& p)
{
    auto* par = p.getValueTreeState().getParameter (PID::genCoordinationMode);
    return (int) std::lround (par->convertFrom0to1 (par->getValue()));
}
static int coordCap (T5ynthProcessor& p)
{
    auto* par = p.getValueTreeState().getParameter (PID::genCoordinationCap);
    return (int) std::lround (par->convertFrom0to1 (par->getValue()));
}
static void setChoice (T5ynthProcessor& p, const char* pid, float plain)
{
    auto* par = p.getValueTreeState().getParameter (pid);
    par->setValueNotifyingHost (par->convertTo0to1 (plain));
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    T5ynthProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    // ── 1. export carries the keys (defaults) ──
    const juce::String defJson = proc.exportJsonPreset();
    CHECK (defJson.contains ("\"coordination\": \"density_budget\""),
           "default export writes coordination=density_budget");
    CHECK (defJson.contains ("\"coordinationCap\": 3"),
           "default export writes coordinationCap=3");

    // ── 2. non-default roundtrip ──
    setChoice (proc, PID::genCoordinationMode, (float) CoordinationMode::Dialog);
    setChoice (proc, PID::genCoordinationCap, 5.0f);
    const juce::String dlgJson = proc.exportJsonPreset();
    CHECK (dlgJson.contains ("\"coordination\": \"dialog\""), "export writes coordination=dialog");
    CHECK (dlgJson.contains ("\"coordinationCap\": 5"),       "export writes coordinationCap=5");

    setChoice (proc, PID::genCoordinationMode, (float) CoordinationMode::Independent);
    setChoice (proc, PID::genCoordinationCap, 1.0f);
    CHECK (proc.importJsonPreset (dlgJson), "dialog preset imports");
    CHECK (coordModeIndex (proc) == CoordinationMode::Dialog, "import restores Dialog");
    CHECK (coordCap (proc) == 5,                              "import restores cap 5");

    // ── 3. pre-2026-07-16 preset (keys absent) restores the DEFAULTS ──
    auto parsed = juce::JSON::parse (dlgJson);
    if (auto* gs = parsed.getDynamicObject()->getProperty ("generativeSeq").getDynamicObject())
    {
        gs->removeProperty ("coordination");
        gs->removeProperty ("coordinationCap");
    }
    // Session sits at Dialog/5 from step 2 — the old preset must OVERRIDE it.
    CHECK (proc.importJsonPreset (juce::JSON::toString (parsed, true)), "legacy preset imports");
    CHECK (coordModeIndex (proc) == CoordinationMode::DensityBudget,
           "absent key restores DensityBudget (not session value, not index 0)");
    CHECK (coordCap (proc) == 3, "absent cap restores 3");

    // ── 4. removed no-op key maps to Independent ──
    if (auto* gs = parsed.getDynamicObject()->getProperty ("generativeSeq").getDynamicObject())
        gs->setProperty ("coordination", "algebraic");
    CHECK (proc.importJsonPreset (juce::JSON::toString (parsed, true)), "algebraic preset imports");
    CHECK (coordModeIndex (proc) == CoordinationMode::Independent,
           "legacy 'algebraic' (was a silent no-op) maps to Independent");

    // ── 5. XML surfaces (DAW session chunk / .t5p snapshot trees): a raw
    //       index 2/3 stored under epoch <3 was a silent no-op ("Algebraic"/
    //       "Counterpoint") and must stay inert (Independent), while a
    //       modern (epoch 3) index 2 IS Dialog and must survive. ──
    {
        auto makeTree = [] (float storedIndex)
        {
            juce::ValueTree t ("PARAMS");
            juce::ValueTree p ("PARAM");
            p.setProperty ("id", PID::genCoordinationMode, nullptr);
            p.setProperty ("value", storedIndex, nullptr);
            t.appendChild (p, nullptr);
            return t;
        };
        auto storedIndex = [] (const juce::ValueTree& t)
        { return juce::roundToInt (static_cast<double> (t.getChild (0).getProperty ("value"))); };

        auto legacy2 = makeTree (2.0f);
        auto legacy3 = makeTree (3.0f);
        auto modern2 = makeTree (2.0f);
        Calibration::migrateValueTree (legacy2, 2);
        Calibration::migrateValueTree (legacy3, 0);
        Calibration::migrateValueTree (modern2, Calibration::kEpoch);
        CHECK (storedIndex (legacy2) == CoordinationMode::Independent,
               "epoch-2 stored index 2 (was no-op 'Algebraic') stays inert");
        CHECK (storedIndex (legacy3) == CoordinationMode::Independent,
               "epoch-0 stored index 3 (was no-op 'Counterpoint') stays inert");
        CHECK (storedIndex (modern2) == CoordinationMode::Dialog,
               "current-epoch stored index 2 stays Dialog");
    }

    std::printf ("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
                 failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
