#pragma once
#include <JuceHeader.h>
#include <atomic>

// ── Novation Launch Control XL 3 (Mk3) — LED protocol + Page 1 CC bindings ──
//
// The LCXL3 has full-RGB LEDs set via SysEx (NOT the original XL's Note-On
// velocity palette). One control's LED is addressed by its "control index":
//
//   F0 00 20 29 02 15 01 53 <control-index> <R> <G> <B> F7     (R/G/B 0..127)
//
//   00 20 29 = Novation manufacturer ID, 02 15 = LCXL3 device, 01 53 = LED cmd.
//   juce::MidiMessage::createSysExMessage() adds the F0/F7, so we pass the
//   inner bytes only.
//
// DAW-mode mapping (programmer's reference p.9): control-index == CC#. The same
// indices the hardware transmits double as the LED colour indices — faders 5-12,
// encoders 13-36 (ch16), buttons 37-52 (ch1). If a control lights the WRONG LED,
// only the index mapping in ccToLedNote() needs adjusting — colors + send path stay.
// (Spec-confirmed; physical per-LED position still worth a once-over on device.)
//
// Source: Novation LCXL3 programmer's reference (DAW mode: LED SysEx p.12, layout p.9).

namespace lcxl3_detail
{
    // Pack a 7-bit RGB triplet into one int: (R<<16)|(G<<8)|B, each 0..127.
    constexpr int rgb (int r, int g, int b) noexcept { return (r << 16) | (g << 8) | b; }

    // Convert a 0xAARRGGBB colour literal (from GuiHelpers.h) to a 7-bit RGB packed int.
    // Uses rounding (ch8*127+127)/255 so the result is pixel-identical to the original
    // rounded constants that were previously hand-coded here. When GuiHelpers.h changes
    // a colour, update the hex literal in kColor* below; the conversion stays.
    constexpr int fromGuiColor (unsigned int argb) noexcept
    {
        return rgb(
            static_cast<int>(( (argb >> 16) & 0xFFu) * 127u + 127u) / 255,
            static_cast<int>((( argb >>  8) & 0xFFu) * 127u + 127u) / 255,
            static_cast<int>((  argb        & 0xFFu) * 127u + 127u) / 255);
    }

    // One XL binding. minNorm/maxNorm map the 0..127 CC onto the param's
    // normalized range (default 0..1; swap to invert a control, e.g. Alpha).
    // Kept at namespace scope (not nested in LaunchControlXLLeds) so these default
    // member initializers stay usable by the constexpr arrays regardless of how
    // the enclosing class's static members get instantiated — a nested struct's
    // defaults can be rejected as "needed within an incomplete enclosing class".
    struct Binding { const char* paramId; int cc; int color; float minNorm = 0.0f; float maxNorm = 1.0f; };
}

struct LaunchControlXLLeds
{
    // ── Module accent colors (7-bit RGB) — derived from GuiHelpers.h via fromGuiColor().
    //    Hex literals match the 0xAARRGGBB values in GuiHelpers.h exactly; update both
    //    together when the on-screen palette changes. ──────────────────────────────────
    static constexpr int kColorOff    = lcxl3_detail::rgb(0, 0, 0);        // LED off
    static constexpr int kColorBound  = lcxl3_detail::rgb(0, 110, 0);      // CC-Learn bound — functional indicator, not a module colour

    static constexpr int kColorGen    = lcxl3_detail::fromGuiColor(0xff667eea); // kOscCol   — periwinkle (generation / osc)
    static constexpr int kColorEnv    = lcxl3_detail::fromGuiColor(0xffFF6F00); // kEnvCol   — amber      (envelopes A/D/S/R)
    // kLfoCol == kEnvCol == #FF6F00 in GuiHelpers.h — identical on screen; GUI palette has no
    // separate LFO or Drift colour. Three distinct hardware shades are hardcoded:
    //   Env   (127, 55, 0) ≈ #FF6E00 — amber-orange   (via fromGuiColor above)
    //   LFO   (127, 35, 0) ≈ #FF4700 — pure orange     ΔE2000 ≈ 9.3 from Env
    //   Drift (127, 10, 0) ≈ #FF1400 — deep red-orange ΔE2000 ≈ 5.8 from LFO, 15.2 from Env
    // All pairs exceed ΔE2000 = 5 (clearly distinguishable on hardware LEDs).
    static constexpr int kColorLfo    = lcxl3_detail::rgb(127,  35,   0); // pure orange    (LFO cols 5–6)
    static constexpr int kColorDrift  = lcxl3_detail::rgb(127,  10,   0); // deep red-orange (Drift cols 7–8)
    static constexpr int kColorFilter = lcxl3_detail::fromGuiColor(0xff7C4DFF); // kFilterCol— violet     (filter)
    static constexpr int kColorFx     = lcxl3_detail::fromGuiColor(0xff00BCD4); // kFxCol    — cyan       (delay / reverb)
    static constexpr int kColorSeq    = lcxl3_detail::fromGuiColor(0xff4CAF50); // kSeqCol   — green      (sequencer)
    static constexpr int kColorVol    = lcxl3_detail::fromGuiColor(0xff4CAF50); // kSeqCol   — green      (master vol)

    // ── CC ranges (LCXL3, verified) ─────────────────────────────────────────
    // In DAW mode faders/encoders transmit on ch16, buttons on ch1; the CC
    // numbers are the same as Custom Mode. Control-index == CC for LEDs.
    // Faders (left→right): CC  5-12
    // Row 1 top knobs:     CC 13-20
    // Row 2 mid knobs:     CC 21-28
    // Row 3 bottom knobs:  CC 29-36
    // Bottom buttons:      upper row CC 37-44, lower row CC 45-52  (CC 52 = panic)
    // Left transport (ch1, programmer's ref p.9 DAW-mode surface):
    //   Play ▶  = CC 116 (0x74)   Record ● = CC 118 (0x76)
    //   Page ^v = CC 106/107      Track ‹› = CC 103/102   (nav — unused by T5ynth)
    //   Shift   = CC 63 on ch7    (device-managed feature control — never host-driven)

    // Map a CC number → LED control index. Returns -1 if not an XL control.
    // (control-index == CC# assumed; see header note.)
    static int ccToLedNote(int cc) noexcept
    {
        if (cc >= 5  && cc <= 12) return cc;   // Faders  (CC 5-12)
        if (cc >= 13 && cc <= 36) return cc;   // Encoder rows 1-3 (CC 13-36)
        if (cc >= 37 && cc <= 52) return cc;   // Buttons (CC 37-52)
        return -1;
    }

    // Build a SysEx "set LED color" message for one control.
    //   controlIndex : the control's index (== its CC#, see header note)
    //   packedRgb    : (R<<16)|(G<<8)|B, each channel 0..127
    static juce::MidiMessage ledOn(int controlIndex, int packedRgb)
    {
        const juce::uint8 body[] = {
            0x00, 0x20, 0x29, 0x02, 0x15, 0x01, 0x53,
            static_cast<juce::uint8>(controlIndex      & 0x7F),
            static_cast<juce::uint8>((packedRgb >> 16) & 0x7F),
            static_cast<juce::uint8>((packedRgb >>  8) & 0x7F),
            static_cast<juce::uint8>( packedRgb        & 0x7F)
        };
        return juce::MidiMessage::createSysExMessage(body, static_cast<int>(sizeof(body)));
    }

    // Build a SysEx LED-off message (RGB 0,0,0).
    static juce::MidiMessage ledOff(int controlIndex)
    {
        return ledOn(controlIndex, kColorOff);
    }

    // DAW-mode enable/disable. Host LED control ("Colouring the surface") is ONLY
    // honored once the device is in DAW mode — programmer's reference p.8-12; a
    // Custom Mode's LEDs cannot be recoloured by the host. Sent as Note On ch16,
    // note 0x0C, velocity 127 (enable) / 0 (disable): bytes 9F 0C 7F / 9F 0C 00.
    static constexpr juce::uint8 kDawModeStatus     = 0x9F;
    static constexpr juce::uint8 kDawModeNote       = 0x0C;
    static constexpr juce::uint8 kDawModeOnVelocity = 0x7F;

    static juce::MidiMessage dawMode(bool enable)
    {
        return juce::MidiMessage(kDawModeStatus, kDawModeNote, enable ? kDawModeOnVelocity : 0x00);
    }

    // Counted by whoever sends a switch, BEFORE it goes out, and for the whole
    // process: the XL is one device, so its echo reaches every instance that
    // listens to it, whichever one sent the switch. See DawModeEchoFilter.
    inline static std::atomic<juce::uint32> dawModeEnablesSent  { 0 };
    inline static std::atomic<juce::uint32> dawModeDisablesSent { 0 };

    static void countDawModeSwitch(bool enable) noexcept
    {
        (enable ? dawModeEnablesSent : dawModeDisablesSent).fetch_add(1, std::memory_order_release);
    }

    // Measured on an LCXL3: its "DAW In" port answers a switch, its "MIDI In" port
    // does not. A port that stays silent owes no echo, so nothing is ever held back
    // for it. A port nobody measured counts as answering: an echo let through hangs
    // a voice.
    static bool portEchoesDawMode(const juce::String& portName)
    {
        return ! portName.containsIgnoreCase("MIDI In");
    }

    static bool isDawModeSwitch(const juce::uint8* bytes, int size, bool enable) noexcept
    {
        return size == 3
            && bytes[0] == kDawModeStatus
            && bytes[1] == kDawModeNote
            && bytes[2] == (enable ? kDawModeOnVelocity : 0x00);
    }

    // ── Feature controls (programmer's reference p.16-17) ───────────────────────
    // Every feature control is a CC on channel 7 (status byte B6h); On/Off = 127/0.
    // They configure the device's DAW-mode behaviour (and are reset when DAW mode is
    // disabled, except the (*)-marked persistent ones which we deliberately never set).
    static constexpr int kFcEncRow1Rel  = 0x45;  // 69 — encoder row 1 relative output
    static constexpr int kFcEncRow2Rel  = 0x48;  // 72 — encoder row 2 relative output
    static constexpr int kFcEncRow3Rel  = 0x49;  // 73 — encoder row 3 relative output
    static constexpr int kFcFaderPickup = 0x46;  // 70 — fader pickup (soft takeover)
    static constexpr int kFcTouchEvents = 0x47;  // 71 — continuous-control touch events

    // Build a feature-control message: CC on ch7 (B6h), value 0..127.
    static juce::MidiMessage featureControl(int cc, int value)
    {
        return juce::MidiMessage(0xB6, cc & 0x7F, value & 0x7F);
    }

    // Switch an encoder row to RELATIVE mode. The endless encoders default to ABSOLUTE
    // (they emit an internal 0-127 position, so the synth value JUMPS to it on the first
    // turn); relative mode emits a signed delta around 64 instead — no jump. Programmer's
    // ref p.10: B6h <RowID> 7Fh (ch7). RowID 0x45/0x48/0x49 = rows 1/2/3. In relative
    // mode a row's encoders transmit on CC (absoluteCC + 64): rows 1-3 = CC 77-84 /
    // 85-92 / 93-100, value = 64 + delta. rowIndex 0..2.
    static juce::MidiMessage encoderRelativeMode(int rowIndex, bool enable)
    {
        static constexpr int rowId[3] = { kFcEncRow1Rel, kFcEncRow2Rel, kFcEncRow3Rel };
        return featureControl(rowId[juce::jlimit(0, 2, rowIndex)], enable ? 0x7F : 0x00);
    }

    // Fader pickup (soft takeover): a physical fader does not "jump" the bound param to
    // its position on touch — it engages only once swept past the current value. Avoids
    // the analogous jump that relative mode fixes for encoders. Feature control CC 70/ch7.
    static juce::MidiMessage faderPickup(bool enable)
    {
        return featureControl(kFcFaderPickup, enable ? 0x7F : 0x00);
    }

    // ── Page 1 default bindings: { paramId, CC, color } ─────────────────────
    // Applied by PluginProcessor::applyXLDefaultBindings() on user request.
    //
    // Faders (CC 5-12): Alpha(inv) — Resynth — Cutoff — Res — Drive — DlyMix — RevMix — Vol
    // Alpha is inverted (minNorm=1,maxNorm=0): fader UP = A, matching the on-screen
    // FlippedVerticalSlider (A at top). Vol = master_vol (the on-screen "Vol" knob).
    // Row 1 (CC 13-20): Env1 (amp)  A/D/S/R — LFO1 Rate/Amt — Drift1 Rate/Amt
    // Row 2 (CC 21-28): Env2 (mod1) A/D/S/R — LFO2 Rate/Amt — Drift2 Rate/Amt
    // Row 3 (CC 29-36): Env3 (mod2) A/D/S/R — LFO3 Rate/Amt — Drift3 Rate/Amt
    //
    // Physical row N drives module group N — matching the easy-panel tab order
    // ENV1/2/3 = amp/mod1/mod2 (SynthPanel initEnv). Rows 2 and 3 were swapped.
    //
    // ENV4/5 (mod3/mod4, added 2026-07-29) have NO default binding, and cannot:
    // the controller has three knob rows and all three are spoken for. They stay
    // reachable through MIDI learn like every other unbound parameter.
    static constexpr lcxl3_detail::Binding kPage1[] = {
        // Faders — Generation | Filter | FX | Vol  (CC 5-12)
        { "gen_alpha",         5, kColorGen, 1.0f, 0.0f }, { "resynth_amount", 6, kColorGen },
        { "filter_cutoff",     7, kColorFilter }, { "filter_resonance",  8, kColorFilter },
        { "filter_drive",      9, kColorFilter },
        { "delay_mix",        10, kColorFx     }, { "reverb_mix",       11, kColorFx     },
        { "master_vol",       12, kColorVol    },

        // Row 1 — Env1 (amp) + LFO1 + Drift1  (CC 13-20)
        { "amp_attack",    13, kColorEnv   }, { "amp_decay",    14, kColorEnv   },
        { "amp_sustain",   15, kColorEnv   }, { "amp_release",  16, kColorEnv   },
        { "lfo1_rate",     17, kColorLfo   }, { "lfo1_depth",   18, kColorLfo   },
        { "drift1_rate",   19, kColorDrift }, { "drift1_depth", 20, kColorDrift },

        // Row 2 — Env2 (mod1) + LFO2 + Drift2  (CC 21-28)
        { "mod1_attack",   21, kColorEnv   }, { "mod1_decay",   22, kColorEnv   },
        { "mod1_sustain",  23, kColorEnv   }, { "mod1_release", 24, kColorEnv   },
        { "lfo2_rate",     25, kColorLfo   }, { "lfo2_depth",   26, kColorLfo   },
        { "drift2_rate",   27, kColorDrift }, { "drift2_depth", 28, kColorDrift },

        // Row 3 — Env3 (mod2) + LFO3 + Drift3  (CC 29-36)
        { "mod2_attack",   29, kColorEnv   }, { "mod2_decay",   30, kColorEnv   },
        { "mod2_sustain",  31, kColorEnv   }, { "mod2_release", 32, kColorEnv   },
        { "lfo3_rate",     33, kColorLfo   }, { "lfo3_depth",   34, kColorLfo   },
        { "drift3_rate",   35, kColorDrift }, { "drift3_depth", 36, kColorDrift },
    };

    static constexpr int kPage1Count = static_cast<int>(std::size(kPage1));

    // ── Extended map — secondary parameters (CC 53-76) ───────────────────────
    // CC 53-76 sits between the button row (CC 52) and the relative-encoder
    // overlay (CC 77-100). No XL DAW-mode control transmits these CCs, so
    // this range is safe for generic MIDI controllers without any XL conflict.
    // Applied by populateXLDefaultBindings() alongside kPage1, so the table is
    // active in xlDefaults_ and any controller whose knob CC falls here is
    // immediately usable without manual CC-learn.
    //
    // Layout: three groups of 8 (mirrors the XL's 3×8 encoder grid for orientation):
    //   Block A CC 53-60  FX secondary
    //   Block B CC 61-68  Generation secondary
    //   Block C CC 69-76  Sequencer + misc
    static constexpr lcxl3_detail::Binding kExtMap[] = {
        // Block A — FX secondary (CC 53-60)
        { "delay_time",       53, kColorFx  }, { "delay_feedback",   54, kColorFx  },
        { "delay_damp",       55, kColorFx  }, { "algo_room",        56, kColorFx  },
        { "algo_damping",     57, kColorFx  }, { "algo_width",       58, kColorFx  },
        // CC 59-60 spare (future: reverb pre-delay, IR trim)

        // Block B — Generation secondary (CC 61-68)
        { "gen_magnitude",    61, kColorGen }, { "gen_noise",        62, kColorGen },
        { "gen_duration",     63, kColorGen }, { "inf_steps",        64, kColorGen },
        { "gen_cfg",          65, kColorGen }, { "gen_start",        66, kColorGen },
        { "gen_axes_amount",  67, kColorGen },
        // CC 68 spare (future: gen_seed, gen_hf_boost)

        // Block C — Sequencer + misc (CC 69-76)
        { "seq_bpm",          69, kColorSeq }, { "seq_steps",        70, kColorSeq },
        { "seq_gate",         71, kColorSeq }, { "seq_shuffle",      72, kColorSeq },
        { "seq_glide_time",   73, kColorSeq }, { "osc_scan",         74, kColorGen },
        { "drift_crossfade",  75, kColorDrift},
        // CC 76 spare (was aftertouch_amount; AT is now 12 per-target bars in the easy panel)
    };
    static constexpr int kExtMapCount = static_cast<int>(std::size(kExtMap));

    // Canonical Page-1 paramId bound to a CC, or nullptr if the CC is not in the map.
    static const char* paramIdForCc(int cc) noexcept
    {
        for (const auto& b : kPage1)
            if (b.cc == cc) return b.paramId;
        return nullptr;
    }

    // One-time migration for presets saved before the Row-2/Row-3 fix, where the
    // two knob blocks (CC 21-28 ↔ 29-36) were swapped. Because the blocks were
    // swapped wholesale, the pre-fix paramId at a CC equals the *canonical*
    // paramId of its swap partner (CC±8); that signature uniquely identifies a
    // stale slot. Genuine user CC-learns (e.g. CC21→filter_cutoff) never match,
    // so they are returned unchanged.
    static juce::String migrateLegacyKnobParam(int cc, const juce::String& loadedParamId)
    {
        if (cc < 21 || cc > 36) return loadedParamId;        // only the swapped blocks moved
        const int partnerCc   = (cc <= 28) ? cc + 8 : cc - 8;
        const char* canonical = paramIdForCc(cc);
        const char* legacy    = paramIdForCc(partnerCc);     // == what the slot held pre-fix
        if (canonical != nullptr && legacy != nullptr && loadedParamId == legacy)
            return juce::String(canonical);
        return loadedParamId;
    }
};

// Takes the XL's echo of a DAW-mode switch out of a block's MIDI, before anything
// reads it.
//
// The device repeats dawMode() back on its DAW Out port: measured on an LCXL3,
// 9F 0C 7F about 10 ms after the enable and 9F 0C 00 after the disable, and
// nothing at all on its MIDI port. Taken for keys, the enable holds a voice on
// note 12 (the cause of the intermittent startup pulsating sound) and the disable
// lifts whatever is really held there. But the same bytes
// ARE a key: note 12 on channel 16, and an MPE controller plays on channel 16. An
// Osmose rotates its notes strictly over channels 2-16, so a filter on the channel
// lost every fifteenth note.
//
// A MidiBuffer does not name its source, so the one thing that tells the echo from
// the key is that a switch was just sent. Each switch on a port that echoes
// (portEchoesDawMode) therefore excuses ONE message of its own kind, within
// kWindowSeconds of audio; the first one to arrive is taken and the bytes are a key
// again. A real key that beats its echo there swaps roles with it, which comes to
// the same thing. The window is a hundred times the measured delay because an echo
// let through hangs a voice.
//
// A switch sent by another program is not seen here, and its echo plays as a key.
class DawModeEchoFilter
{
public:
    static constexpr double kWindowSeconds = 1.0;
    static constexpr int    kScratchBytes  = 65536;   // about 7000 messages in one block

    // `scratch` is the buffer process() will be handed.
    void prepare(double sampleRate, juce::MidiBuffer& scratch)
    {
        windowSamples_ = juce::roundToInt(sampleRate * kWindowSeconds);
        scratch.ensureSize(kScratchBytes);
    }

    // Audio thread, first thing in every block, bypassed ones too: the window is
    // counted in audio time, so an output restored before audio runs is still
    // covered when the first block arrives with the echo in it.
    void process(juce::MidiBuffer& midi, juce::MidiBuffer& scratch, int numSamples)
    {
        enables_.collect(LaunchControlXLLeds::dawModeEnablesSent.load(std::memory_order_acquire),
                         windowSamples_);
        disables_.collect(LaunchControlXLLeds::dawModeDisablesSent.load(std::memory_order_acquire),
                          windowSamples_);

        if (enables_.owed == 0 && disables_.owed == 0)
            return;

        scratch.clear();
        bool removed = false;
        for (const auto metadata : midi)
        {
            if (enables_.owed > 0
                && LaunchControlXLLeds::isDawModeSwitch(metadata.data, metadata.numBytes, true))
            {
                --enables_.owed;
                removed = true;
            }
            else if (disables_.owed > 0
                     && LaunchControlXLLeds::isDawModeSwitch(metadata.data, metadata.numBytes, false))
            {
                --disables_.owed;
                removed = true;
            }
            else
            {
                scratch.addEvent(metadata.data, metadata.numBytes, metadata.samplePosition);
            }
        }

        if (removed)
        {
            midi.clear();
            midi.addEvents(scratch, 0, -1, 0);
        }

        enables_.age(numSamples);
        disables_.age(numSamples);
    }

private:
    struct Echoes
    {
        juce::uint32 seen;      // a new instance owes nothing for switches before it
        int owed = 0;
        int samplesLeft = 0;

        void collect(juce::uint32 sent, int windowSamples) noexcept
        {
            if (sent == seen)
                return;
            owed += static_cast<int>(sent - seen);
            seen = sent;
            samplesLeft = windowSamples;
        }

        void age(int numSamples) noexcept
        {
            samplesLeft -= numSamples;
            if (samplesLeft <= 0)
                owed = 0;
        }
    };

    int    windowSamples_ = 0;
    Echoes enables_  { LaunchControlXLLeds::dawModeEnablesSent.load(std::memory_order_relaxed) };
    Echoes disables_ { LaunchControlXLLeds::dawModeDisablesSent.load(std::memory_order_relaxed) };
};
