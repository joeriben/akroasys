// CsoundEngine.cpp — Phase-1 hard-wired Csound engine (real implementation).
//
// Compiled ONLY when CMake found CsoundLib64 (target_sources gated in
// CMakeLists.txt on T5YNTH_CSOUND_FOUND, which also drives the always-defined
// T5YNTH_HAS_CSOUND=1 in that configuration). The #if below is a defensive
// belt-and-braces guard: this TU should never be compiled with the macro at
// 0 (that would be an ODR/ABI mismatch against CsoundEngine.h's inline stub),
// but if it somehow is, this file compiles to an empty translation unit
// rather than trying to #include a header that isn't on the include path.
#include "CsoundEngine.h"

#if T5YNTH_HAS_CSOUND

#include <csound/csound.h>

// csound's sysdep.h (pulled in transitively by csound.h) leaks several short
// macro names into the rest of this translation unit (LIKELY/UNLIKELY/
// ATOMIC_*/DIRSEP/ENVSEP/CS_PURE) that risk colliding with identifiers used
// elsewhere in the codebase. This TU only calls Csound's documented public C
// API, never these internal macros, so clearing them here is safe and
// confined entirely to this file (copied verbatim from
// tools/csound_poc.cpp:94-102 — proven ground truth on this machine).
#undef LIKELY
#undef UNLIKELY
#undef DIRSEP
#undef ENVSEP
#undef ATOMIC_SET
#undef ATOMIC_GET
#undef ATOMIC_ADD
#undef ATOMIC_SUB
#undef CS_PURE

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>
#if defined(_WIN32)
 #include <windows.h>
#else
 #include <dlfcn.h>
#endif

namespace
{
    // ── Where Csound looks for its plugin opcodes ───────────────────────────
    //
    // Csound resolves them through a path baked in when the library was BUILT:
    // on this machine `strings CsoundLib64` shows
    // /opt/homebrew/Cellar/csound/<version>/…/Resources/Opcodes64. A copy of the
    // library inside our bundle therefore keeps reaching into a Homebrew tree
    // that exists here and nowhere else — measured 2026-07-26: without any
    // plugin modules only the 1917 built-in opcode entries register, and the
    // absent ones include scanu/scanu2/scans, fractalnoise, tvconv, MixerSend,
    // ftgenonce, limit1 and GEN padsynth. The author WRITES Csound and may reach
    // for any of them, so the modules are copied next to the library
    // (tools/bundle_csound_macos.sh — 2267 entries with them) and pointed at here.
    //
    // The rule is deliberately self-validating: use the plugin directory that we
    // SHIPPED, and only if it is really there. A plain system install has no such
    // directory, so nothing is overridden and Csound's own resolution — which is
    // correct there — is left alone.
    //
    // macOS anchors that on the library actually loaded (dladdr), because dyld keys
    // images by resolved path: each bundle's copy is its own image with its own copy
    // of this global, and one plugin cannot disturb another's.
    //
    // WINDOWS DOES NOT WORK THAT WAY, and this is the one place it matters. The
    // Windows loader keys modules by BASE NAME, so if another Csound-based plugin
    // (Cabbage, CsoundVST) is already loaded in the same DAW, a LoadLibrary of our
    // own csound64.dll hands back THEIR image — there is no second image to be had.
    // Anchoring the opcode directory on the loaded library would then point at their
    // installation, and ours would never be used: the 350-entry loss this whole
    // arrangement exists to prevent, in the case hardest to notice. So on Windows the
    // anchor is OUR OWN MODULE's directory instead, which is where we put plugins64.
    // Sharing one image also means sharing this global — whoever sets it last wins —
    // and we deliberately set it to our own stock 6.18.1 modules rather than lose them.
   #if ! defined(_WIN32)
    bool isDirectory (const std::string& path)
    {
        struct stat st {};
        return stat (path.c_str(), &st) == 0 && S_ISDIR (st.st_mode);
    }
   #else
    bool isDirectory (const std::wstring& path)
    {
        // MSVC's <sys/stat.h> has no S_ISDIR, so ask the API directly.
        const auto attr = GetFileAttributesW (path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    // Everything else here is wide, deliberately. The Standalone is unzipped wherever the
    // user likes, and GetModuleFileNameA on a path holding a character outside the
    // machine's ANSI code page — any Cyrillic, Greek or CJK user or folder name —
    // returns it with '?' substituted, which LoadLibraryA then cannot open. With no
    // bare-name fall-back left (and there must not be one, see below) that is a
    // permanently silent LRO for those users, with nothing said about it.
    std::wstring modulePath (HMODULE module)
    {
        // NOT optional: GetModuleFileNameW(NULL, …) is documented to return the path
        // of the running EXECUTABLE, not an error — a failed lookup passed through
        // here would silently become the host DAW's own directory.
        if (module == nullptr)
            return {};

        // MAX_PATH is not a limit on Windows paths, only on this API's default
        // buffer; it truncates and reports the buffer size rather than the need.
        for (DWORD size = MAX_PATH; size <= 32768u; size *= 2)
        {
            std::wstring buf (size, L'\0');
            const auto len = GetModuleFileNameW (module, buf.data(), size);
            if (len == 0)
                return {};
            if (len < size)
            {
                buf.resize (len);
                return buf;
            }
        }
        return {};
    }

    std::wstring directoryOf (HMODULE module)
    {
        const auto file = modulePath (module);
        const auto slash = file.find_last_of (L"\\/");
        return slash == std::wstring::npos ? std::wstring {} : file.substr (0, slash);
    }

    HMODULE thisModule()
    {
        HMODULE self {};
        return GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR> (&thisModule), &self)
             ? self : nullptr;
    }

    // csoundSetOpcodedir takes const char*, so a path Csound must open has to survive
    // the ANSI code page. Where it does not, the 8.3 short name does — that is what
    // it is for. Where even that is unavailable the plugin opcodes are out of reach
    // and we say so rather than silently shipping 1917 opcodes instead of 2267.
    std::string ansiPathForCsound (const std::wstring& wide)
    {
        auto narrow = [] (const std::wstring& w) -> std::string
        {
            if (w.empty())
                return {};
            BOOL lossy = FALSE;
            const int n = WideCharToMultiByte (CP_ACP, WC_NO_BEST_FIT_CHARS,
                                               w.c_str(), (int) w.size(),
                                               nullptr, 0, nullptr, &lossy);
            if (n <= 0)
                return {};
            std::string out ((size_t) n, '\0');
            if (WideCharToMultiByte (CP_ACP, WC_NO_BEST_FIT_CHARS,
                                     w.c_str(), (int) w.size(),
                                     out.data(), n, nullptr, &lossy) != n || lossy)
                return {};
            return out;
        };

        if (auto direct = narrow (wide); ! direct.empty())
            return direct;

        const auto needed = GetShortPathNameW (wide.c_str(), nullptr, 0);
        if (needed == 0)
            return {};
        std::wstring shortName (needed, L'\0');
        const auto got = GetShortPathNameW (wide.c_str(), shortName.data(), needed);
        if (got == 0 || got >= needed)
            return {};
        shortName.resize (got);
        return narrow (shortName);
    }

    // csound64.dll is DELAY-LOADED (see CMakeLists), for one reason: a VST3 is a
    // DLL the host loads by full path, and Windows does not add that path to the
    // import search. A statically imported csound64.dll would therefore be looked
    // for beside the HOST's executable, not beside ours, and the plugin would fail
    // to load in some hosts and not others. Delay-loading moves the decision to
    // first use, where we can load it by absolute path ourselves; the delay-load
    // helper then finds a module of that base name already loaded and uses it.
    bool loadCsoundDll (const std::wstring& ourDirectory)
    {
        if (GetModuleHandleW (L"csound64.dll") != nullptr)
            return true;    // already in the process — possibly not ours, see above

        // OUR directory, by absolute path, and nowhere else. There is deliberately
        // no fall-back to a bare LoadLibrary("csound64.dll"): the default search
        // order includes the CURRENT DIRECTORY, so a file of that name in whatever
        // folder the host happens to be pointed at would be loaded into the
        // process — a DLL-planting vector for a library we always ship ourselves.
        // If it is not beside us the installation is broken, and a silent LRO is
        // the right answer to that.
        return LoadLibraryW ((ourDirectory + L"\\csound64.dll").c_str()) != nullptr;
    }
   #endif

    // False means: there is no usable Csound in this process. Every caller then
    // does nothing and the LRO is silent — the same outcome as a build without
    // Csound, and never a crash on a machine where the library is missing.
    bool csoundLibraryReady()
    {
        static const bool ready = []
        {
           #if defined(_WIN32)
            const auto ourDir = directoryOf (thisModule());
            if (ourDir.empty() || ! loadCsoundDll (ourDir))
                return false;

            const auto plugins = ourDir + L"\\plugins64";
            if (isDirectory (plugins))
            {
                const auto usable = ansiPathForCsound (plugins);
                if (usable.empty())
                    std::fprintf (stderr, "CsoundEngine: the plugin opcode directory "
                                          "cannot be expressed in this machine's ANSI "
                                          "code page -- the LRO runs on core opcodes "
                                          "only\n");
                else
                    csoundSetOpcodedir (usable.c_str());
            }
           #else
            Dl_info info {};
            if (dladdr (reinterpret_cast<const void*> (&csoundCreate), &info) != 0
                && info.dli_fname != nullptr)
            {
                const std::string self { info.dli_fname };
                const auto slash = self.rfind ('/');
                if (slash != std::string::npos)
                {
                    const auto beside = self.substr (0, slash);

                    const auto opcodes = beside + "/Opcodes64";
                    if (isDirectory (opcodes))
                        csoundSetOpcodedir (opcodes.c_str());

                    // STK's data files, the same sibling rule — but through the
                    // ENVIRONMENT, because that is the only handle Csound's STK
                    // module offers: it reads getenv("RAWWAVE_PATH") as it loads
                    // and, finding nothing, refuses to register at all. Measured
                    // 2026-08-06: 2267 opcode entries without it, 2294 with —
                    // the difference is every STK opcode (STKBowed, STKBlowBotl,
                    // STKModalBar, STKSaxofony …), which the author WRITES and
                    // therefore may reach for.
                    //
                    // Only when the directory is there — but then it OVERWRITES
                    // whatever the environment already said, which is the one
                    // place this differs from ordinary politeness towards a
                    // user's setting. The reason is the failure mode: a value
                    // left over from an old STK install, pointing at a directory
                    // that has since moved or was never complete, does not make
                    // the module decline. It registers, and the first orchestra
                    // that reaches an STK opcode dies on a file it cannot open —
                    // an uncaught stk::StkError, i.e. abort(), i.e. the host DAW.
                    // We can vouch for the directory we shipped and for no other,
                    // which is the same reasoning that points csoundSetOpcodedir
                    // at our own modules rather than at whatever is installed.
                    // Nothing is taken from anyone: the variable is read by STK
                    // alone, and any other STK in the process wants these very
                    // files — the data set has not changed in twenty years.
                    const auto rawwaves = beside + "/rawwaves";
                    if (isDirectory (rawwaves))
                        ::setenv ("RAWWAVE_PATH", (rawwaves + "/").c_str(), 1);
                }
            }
           #endif
            return true;
        }();
        return ready;
    }

    // Hard-wired 12-partial bell/pad SPECTRUM (amp, ratio), adapted from the
    // BJ-approved tools/csound_poc_out/csound_strike_pad.csd. STANDING TONE
    // (BJ 2026-07-17, "Hüllkurven gehören nicht in den Oszillator. Vollständig
    // entfernen."): every partial holds at its authored amp for as long as the
    // gate is open — no strike, no per-partial decay. Amplitude SHAPE is the
    // downstream synth ADSR/VCA's job, never the oscillator's.
    //
    // NO VIBRATO (BJ 2026-07-17, "vibrato wird — selbstverständlich — aus dem
    // Vokabular genommen"). The per-partial vibHz/vibCents columns are gone.
    // The order named this file and this symbol; I kept them anyway on the
    // reasoning that "periodic motion stays", which was wrong twice over:
    // vibrato is PITCH modulation, i.e. expression, and expression belongs to
    // the synth (LFO -> Pitch), not to the oscillator — the same boundary that
    // took the envelopes out. And a fixed 0.07-0.5 Hz wobble welded into the
    // fallback tone is exactly the "always the same, slightly seasick" character
    // a player hears as unmusical, on every note the engine falls back to.
    struct Partial { double amp, ratio; };
    constexpr int kNumPartials = 12;
    constexpr Partial kPartials[kNumPartials] = {
        { 0.28, 0.56  },
        { 0.30, 1.00  },
        { 0.22, 1.19  },
        { 0.20, 1.71  },
        { 0.16, 2.00  },
        { 0.16, 2.007 },
        { 0.14, 2.74  },
        { 0.11, 3.76  },
        { 0.09, 4.07  },
        { 0.07, 5.43  },
        { 0.05, 6.98  },
        { 0.04, 8.21  },
    };

    // Builds the hard-wired orchestra: ONE numeric `instr 1`, 16 always-on
    // score instances (N=1..16=p4=voice index), 6 named channels per voice
    // (gate/freq/vel/pres/timb/trig). STANDING TONE: the trig channel is still
    // read (the 16x6 channel contract with the voice bridge is fixed) but
    // otherwise unused — every partial simply holds while the gate is open, so
    // there is no retrigger/reinit epoch. sr is baked in via snprintf at the
    // actual prepared sample rate.
    std::string buildOrchestra (double sampleRate)
    {
        std::string csd;
        char line[256];

        csd += "<CsoundSynthesizer>\n<CsOptions>\n-n -d\n</CsOptions>\n<CsInstruments>\n";
        std::snprintf(line, sizeof(line), "sr = %.0f\n", sampleRate);
        csd += line;
        std::snprintf(line, sizeof(line), "ksmps = %d\n", CsoundEngine::kKsmps);
        csd += line;
        std::snprintf(line, sizeof(line), "nchnls = %d\n", CsoundEngine::kMaxVoices);
        csd += line;
        csd += "0dbfs = 1\n\n";

        csd +=
            "; Hard-wired STANDING-TONE orchestra. gate = voice ACTIVE (incl. release),\n"
            "; not note-held (D3) -- the T5ynth ampEnv/VCA shapes attack/release\n"
            "; downstream; closing this gate on note-off would abort the release tail,\n"
            "; so it never does. Every partial holds at its authored amplitude while the\n"
            "; gate is open (no strike, no decay -- amplitude SHAPE is the synth ADSR's\n"
            "; job). The trig channel is read for the fixed 16x6 contract but is unused.\n"
            "; pres/timb are read end-to-end and mapped lightly here.\n"
            "instr 1\n"
            "  ivoice   = p4\n"
            "  Sgate    sprintf \"gate%d\", ivoice\n"
            "  Sfreq    sprintf \"freq%d\", ivoice\n"
            "  Svel     sprintf \"vel%d\", ivoice\n"
            "  Spres    sprintf \"pres%d\", ivoice\n"
            "  Stimb    sprintf \"timb%d\", ivoice\n"
            "  Strig    sprintf \"trig%d\", ivoice\n"
            "\n"
            "  kgateraw chnget Sgate\n"
            "  kfreqraw chnget Sfreq\n"
            "  kvel     chnget Svel\n"
            "  kpres    chnget Spres\n"
            "  ktimb    chnget Stimb\n"
            "  ktrigch  chnget Strig\n"
            "\n"
            // Gate: 1 ms HALF-time = pure declick for the raw engine (guard
            // tools drive it without the voice ADSR); in the plugin the DCA
            // envelope shapes amplitude outside, so this must never act as an
            // attack. 0.008 half-time audibly swallowed strike transients
            // (BJ 2026-07-17: "kein Anschlag, ein Einschwingen") — portk's
            // second arg is HALF-time, full settle ~7-10x that value.
            // Freq: NO smoothing — a new note snaps (phase-continuous in the
            // oscillators, so no click); 0.008 here was an unordered ~50 ms
            // exponential portamento (BJ: "deutliches Portamento — entfernen").
            // The voice-level SmoothedValue already smooths the actual glide
            // feature at block rate.
            "  kgate    portk kgateraw, 0.001      ; declick only, NOT an attack\n"
            "  kfreq    limit kfreqraw, 20, 12000\n"
            "\n"
            "  ; ktrigch is read above for the fixed 16x6 channel contract but is\n"
            "  ; otherwise unused: every partial below is a STANDING tone for as long\n"
            "  ; as the gate holds, so there is no retrigger/reinit epoch.\n\n";

        csd +=
            "\n"
            "  ; Phase-1 placeholder pres/timb mapping (channels exist end-to-end;\n"
            "  ; exact musical mapping is Phase 3): timbre cross-fades in the upper 6\n"
            "  ; partials' weight, pressure lifts overall presence a little.\n"
            "  ktimbHi   = 0.4 + 1.2*ktimb\n"
            "  kpresGain = 1.0 + 0.15*kpres\n\n";

        for (int i = 0; i < kNumPartials; ++i)
        {
            std::snprintf(line, sizeof(line),
                "  kfreq%-2d  = kfreq * %.4f\n", i + 1, kPartials[i].ratio);
            csd += line;

            // STANDING amplitude: the partial holds at its authored amp (no
            // strike term, no bed). Upper 6 partials still cross-fade under the
            // timbre control (ktimbHi), unchanged.
            if (i >= 6)
                std::snprintf(line, sizeof(line),
                    "  a%-2d      oscili %.4f*ktimbHi, kfreq%d\n",
                    i + 1, kPartials[i].amp, i + 1);
            else
                std::snprintf(line, sizeof(line),
                    "  a%-2d      oscili %.4f, kfreq%d\n",
                    i + 1, kPartials[i].amp, i + 1);
            csd += line;
        }

        csd +=
            "\n"
            "  asum     = a1+a2+a3+a4+a5+a6+a7+a8+a9+a10+a11+a12\n"
            "  ; headroom: the 0.2 scale bounds the worst-case simultaneous-phase\n"
            "  ; sum of all 12 standing partials (amp sum ~1.82) to <= ~0.5; the\n"
            "  ; voice's own VCA/DCA chain downstream handles the rest. kgate\n"
            "  ; (declick only) keeps an idle/inactive voice provably silent.\n"
            "  ; kvel deliberately absent (matches lco_write._TAIL): the voice\n"
            "  ; envelope's peak already tracks velocity, so a kvel factor here\n"
            "  ; made the engine scale as vel^2 where every other engine is linear.\n"
            "  aout     = asum * kgate * kpresGain * 0.2\n"
            "  outch    ivoice, aout\n"
            "endin\n"
            "</CsInstruments>\n<CsScore>\n";

        // ~63 years, deliberately < INT32_MAX — 360000 s (100 h) ended the
        // performance mid-session and the bridge then looped the frozen
        // spout; mirrors lco_write.py's _SCORE_LIFETIME. substituteScoreLifetime()
        // heals stored presets that still carry the old number.
        for (int v = 1; v <= CsoundEngine::kMaxVoices; ++v)
        {
            std::snprintf(line, sizeof(line), "i 1 0 2000000000 %d\n", v);
            csd += line;
        }
        csd += "e 2000000000\n</CsScore>\n</CsoundSynthesizer>\n";
        return csd;
    }

    std::string drainMessages (CSOUND* cs)
    {
        std::string log;
        const int n = csoundGetMessageCnt(cs);
        for (int i = 0; i < n; ++i)
        {
            if (const char* msg = csoundGetFirstMessage(cs))
                log += msg;
            csoundPopFirstMessage(cs);
        }
        return log;
    }

    // Phase-3 integration: backend/csound_assembler.py's generated orchestras
    // (and tools/csound_orch_check.cpp's own copy of this exact routine) carry
    // a literal "sr = %SR%" marker instead of a hard-coded sample rate, so a
    // preset-loaded orchestra can recompile correctly after a host
    // sample-rate change (D9's early-out above already forces a full
    // recompile whenever preparedSampleRate changes — this substitution just
    // has to run fresh on every such recompile, which it does since csdText
    // is always rebuilt from the ORIGINAL orchestraText, never cached in
    // substituted form). Plain substring replace (not printf-style
    // formatting) — mirrors csound_orch_check.cpp's substituteSr() verbatim.
    // A no-op on text without the marker (the built-in orchestra never
    // contains it), so this is safe to apply unconditionally.
    // Serialises the csoundCreate/CompileCsdText/Start/Destroy sequence across
    // ALL CsoundEngine instances in this process, including the instance-free
    // offline probe. The processor's csoundLifecycleMutex_ already serialises
    // every prepare() on its two engines and was added by adversarial review
    // because concurrent prepare() was judged a race; the probe cannot reach that
    // mutex (it is static and owns no engine), so without this the probe would be
    // the one path violating an invariant the rest of the code maintains.
    //
    // Lock ORDER is always csoundLifecycleMutex_ -> this one, never the reverse:
    // prepare() is only ever called under the processor's mutex, and this mutex is
    // released before any caller could take the processor's. No cycle, no deadlock.
    std::mutex& csoundLifecycleGlobal()
    {
        static std::mutex m;
        return m;
    }

    std::string substituteSr (std::string text, double sampleRate)
    {
        char srBuf[32];
        std::snprintf(srBuf, sizeof(srBuf), "%.0f", sampleRate);
        const std::string marker = "%SR%";
        size_t pos = 0;
        while ((pos = text.find(marker, pos)) != std::string::npos)
        {
            text.replace(pos, marker.size(), srBuf);
            pos += std::strlen(srBuf);
        }
        return text;
    }

    // Presets store the complete CSD including the score, so every preset
    // written before 2026-08-06 carries the 100 h voice lifetime whose end
    // freezes spout (see the score builder above). The two patterns are
    // anchored to the exact score-statement shapes the score writers ever
    // wrote (this file's built-in orchestra, lco_write.py, and the tools
    // mirroring them), so nothing inside an instrument body can match. A
    // no-op on text without the marker, exactly like substituteSr.
    // Deliberately NOT mirrored into tools/csound_orch_check.cpp — that
    // tool's copy of substituteSr stays verbatim, and its 0.25 s probe
    // never reaches a score end.
    std::string substituteScoreLifetime (std::string text)
    {
        const std::string oldVoice = "i 1 0 360000 ";
        const std::string newVoice = "i 1 0 2000000000 ";
        size_t pos = 0;
        while ((pos = text.find(oldVoice, pos)) != std::string::npos)
        {
            text.replace(pos, oldVoice.size(), newVoice);
            pos += newVoice.size();
        }

        const std::string oldEnd = "e 360000\n";
        const std::string newEnd = "e 2000000000\n";
        pos = 0;
        while ((pos = text.find(oldEnd, pos)) != std::string::npos)
        {
            text.replace(pos, oldEnd.size(), newEnd);
            pos += newEnd.size();
        }

        return text;
    }
}

namespace
{
    // ── Decimation filter for source-side oversampling ──────────────────────
    //
    // Linear-phase halfband FIR, 63 taps, Kaiser(beta=8). At a 96 kHz input rate
    // it is flat to ~21 kHz and >=80 dB down from ~28 kHz; every even tap except
    // the centre is exactly zero (halfband), which is what makes 63 taps this
    // cheap. Measured against a 768 kHz reference render it lands within 2 dB of
    // an ideal brickwall on every library patch, so the residual is the
    // oversampling factor's, not the filter's. Group delay is 31 input samples
    // (0.32 ms at 96 kHz) — constant, and the LRO is never layered against
    // another engine (the mode toggle owns engineMode), so it cannot comb.
    constexpr int kDecimTaps = 63;
    constexpr float kHalfbandFir[kDecimTaps] = {
        -2.401525244e-05f,  0.000000000e+00f,  1.090362306e-04f,  0.000000000e+00f, -2.935600810e-04f,  0.000000000e+00f,
         6.381063754e-04f,  0.000000000e+00f, -1.222076003e-03f,  0.000000000e+00f,  2.145908571e-03f,  0.000000000e+00f,
        -3.534414302e-03f,  0.000000000e+00f,  5.543647017e-03f,  0.000000000e+00f, -8.376210429e-03f,  0.000000000e+00f,
         1.231558303e-02f,  0.000000000e+00f, -1.780470021e-02f,  0.000000000e+00f,  2.563768528e-02f,  0.000000000e+00f,
        -3.748938157e-02f,  0.000000000e+00f,  5.772404439e-02f,  0.000000000e+00f, -1.024425088e-01f,  0.000000000e+00f,
         3.170728721e-01f,  4.999999672e-01f,  3.170728721e-01f,  0.000000000e+00f, -1.024425088e-01f,  0.000000000e+00f,
         5.772404439e-02f,  0.000000000e+00f, -3.748938157e-02f,  0.000000000e+00f,  2.563768528e-02f,  0.000000000e+00f,
        -1.780470021e-02f,  0.000000000e+00f,  1.231558303e-02f,  0.000000000e+00f, -8.376210429e-03f,  0.000000000e+00f,
         5.543647017e-03f,  0.000000000e+00f, -3.534414302e-03f,  0.000000000e+00f,  2.145908571e-03f,  0.000000000e+00f,
        -1.222076003e-03f,  0.000000000e+00f,  6.381063754e-04f,  0.000000000e+00f, -2.935600810e-04f,  0.000000000e+00f,
         1.090362306e-04f,  0.000000000e+00f, -2.401525244e-05f,
    };

    // 2:1 decimator — one per voice per stage (4x cascades two of them).
    // RT-safe by construction: fixed storage, no allocation, no branch on the
    // sample path. State is cleared in prepare() before the engine goes ready,
    // so a recompile can never leak the previous orchestra's tail into the new
    // one's first block.
    struct Decimator2x
    {
        static constexpr unsigned kMask = 63u;   // history is 64 == kDecimTaps+1
        float hist[kMask + 1] = {};
        unsigned pos = 0;

        void reset() noexcept
        {
            for (auto& s : hist) s = 0.0f;
            pos = 0;
        }

        // Consumes TWO input samples, returns ONE band-limited output sample.
        inline float process (float a, float b) noexcept
        {
            hist[pos & kMask] = a; ++pos;
            hist[pos & kMask] = b; ++pos;
            float acc = 0.0f;
            for (int k = 0; k < kDecimTaps; ++k)
                acc += kHalfbandFir[k] * hist[(pos - 1u - (unsigned) k) & kMask];
            return acc;
        }
    };

    // ---- prepare()'s level-reference render (see CsoundEngine::outputTrim) ----

    // 220 Hz, the reference pitch every offline measurement in this project uses
    // (tools/lco_measure.render's default), so a trim measured here and a number
    // read there describe the same note.
    constexpr double kLevelMeasureFreqHz = 220.0;
    // Settle covers the body's SPECTRAL attack — which the library's own guidance
    // makes a real thing (an upper band fading in over 40 ms, a filter opening, an
    // FM index reaching its value late) — plus the 63-tap decimators' priming. The
    // level belongs to the settled tone; the synth's envelope owns the onset.
    constexpr double kLevelSettleSeconds  = 0.30;
    // 0.60 s at 220 Hz is 132 cycles: enough that the rms is stable and that a
    // multi-partial body's peaks have had many chances to align. It is NOT enough
    // to average over a slow motion LFO (`tanpura`'s is 3.7 s), so such a body is
    // levelled at one phase of its own movement. Accepted deliberately: the error
    // is the depth of that body's own level motion, ~1 dB in the library, against
    // the 12.4 dB this whole pass exists to remove — and lengthening the window is
    // paid for in compile latency on every single authoring.
    constexpr double kLevelMeasureSeconds = 0.60;
    // Below this the reference render is numerical dust, not a signal.
    constexpr float kLevelSilenceFloor = 1.0e-6f;
    // AND below THIS rms the body did not really speak at the reference note, even
    // though it was not silent — so its level cannot be read off that note, and a
    // trim derived from it would be a guess of +40 dB or more.
    //
    // This is not caution in the abstract; the project documents the class it is
    // for. `wgbrass` puts 2 % of its energy on multiples of the played pitch and is
    // SILENT below about 440 Hz (docs/LCO_CONCEPT.md §6, measured); `wgbowedbar`
    // locks onto a fixed mode away from a narrow bow-position window. A body built
    // on one of those measures near-nothing at 220 Hz and roars an octave up, and
    // the peak ceiling cannot catch it — the ceiling is measured on the same silent
    // render. Leaving such a body untrimmed keeps it as loud as it was, which is
    // the known quantity; scaling it by what a near-silent window suggests is how a
    // 40 dB surprise reaches a player's ears one note higher.
    //
    // -40 dBFS is 30 dB under the quietest thing the library authors (`driven_metal`,
    // rms 0.0496 delivered) so nothing that works is caught by it.
    constexpr float kLevelReferenceFloorRms = 0.01f;   // -40 dBFS
    // Trim bounds. Note what the maximum does NOT do: it does not bound the
    // pitch-dependence risk above, which is proportional (a body needing 4x that
    // is 12 dB louder elsewhere overshoots by the same 12 dB as one needing 32x).
    // kLevelReferenceFloorRms is what addresses that. The maximum only has to reach
    // far enough to rescue the quiet tail of what authors actually write: the
    // library's quietest body needs ~5x, and 32 (+30 dB) covers everything down to
    // the reference floor with room to spare.
    constexpr float kLevelTrimMin = 0.05f;
    constexpr float kLevelTrimMax = 32.0f;

    // ---- The bound the trim would otherwise have removed ----
    //
    // The authored tail ends on `aout clip aout, 0, 0.95, 0.85` (backend/
    // lco_write.py), whose own comment promises it "bounds ANY op stack / crest /
    // host gain" — and until the trim existed it did: nothing could leave this
    // engine above 0.95, in any register, from any body. The trim multiplies AFTER
    // that clip, so on its own it turns a body needing 5x into a possible 4.8 peak
    // wherever the body happens to be louder than at the reference note. The
    // reference floor cannot catch that case (the body is not silent at 220 Hz,
    // merely quieter), and no trim cap can either — the overshoot is proportional
    // to the trim, so a smaller cap moves the number without removing the class.
    //
    // So the bound is re-applied here, after the trim, with the same SHAPE the
    // Csound clip has: transparent up to a knee, asymptotic to a ceiling. Sited so
    // that normal operation never reaches it — the trim targets a 0.891 peak and
    // the knee is above that — which keeps this a safety net and not a colour.
    // A body that IS far louder in another register gets soft-limited there, which
    // is exactly what the tail's clip did to it before.
    constexpr float kOutputKnee    = 0.95f;
    constexpr float kOutputCeiling = 1.50f;

    // One compare on the common path; the tanh only runs on a sample that is
    // already past the knee, which for a correctly trimmed body never happens.
    inline float boundOutput (float y) noexcept
    {
        const float a = std::fabs(y);
        if (a <= kOutputKnee)
            return y;
        constexpr float span = kOutputCeiling - kOutputKnee;
        const float shaped = kOutputKnee + span * std::tanh((a - kOutputKnee) / span);
        return y < 0.0f ? -shaped : shaped;
    }
}

struct CsoundEngine::Impl
{
    CSOUND* csound = nullptr;
    double preparedSampleRate = 0.0;   // HOST rate — what the caller asked for
    double engineSampleRate   = 0.0;   // preparedSampleRate * osFactor — what Csound runs at
    int    osFactor           = 1;     // 1, 2 or 4

    // Decimation chain, per voice. decim1 always runs when osFactor > 1;
    // decim2 only at 4x. Sized for kMaxVoices, never resized.
    Decimator2x decim1[CsoundEngine::kMaxVoices];
    Decimator2x decim2[CsoundEngine::kMaxVoices];
    // One ksmps' worth of HOST-rate samples per voice, produced by renderUpTo()
    // before the existing block/carry bookkeeping copies them out. At osFactor 1
    // that is kKsmps samples; higher factors use only the first kKsmps/osFactor.
    float decimScratch[CsoundEngine::kMaxVoices][CsoundEngine::kKsmps] {};

    int capacity  = 0;   // allocated voice-buffer length (>= every startBlock's numSamples)
    int blockSize = 0;   // current host block length
    int writePos  = 0;   // samples already rendered into the current block

    // Latched by renderUpTo() at the first non-zero csoundPerformKsmps return
    // (the ended state never reverts — measured, 500 consecutive post-end
    // calls all non-zero). While set, renderUpTo() emits silence WITHOUT
    // calling into Csound again: every post-end call queues one never-drained
    // message-buffer entry — a heap allocation plus a mutex on the audio
    // thread. Reset only by a successful recompile.
    // Read by the GUI through performanceHasEnded(), hence atomic — the plain
    // bool was a data race the moment a second thread reads it.
    std::atomic<bool> performanceEnded { false };

    // The static level trim measured for the compiled orchestra (see
    // CsoundEngine::outputTrim's header comment for why it is a measurement and
    // not a constant). Written by prepare() on the compile thread BEFORE `ready`
    // is stored with release semantics, read by renderUpTo() on the audio thread
    // only after it has loaded `ready` with acquire — the identical publish order
    // that makes channelPtr/globalPtr safe to read unsynchronised, and the reason
    // this needs no atomic of its own.
    float outputTrim = 1.0f;

    static constexpr int kChannelsPerVoice = 6; // gate, freq, vel, pres, timb, trig
    MYFLT* channelPtr[CsoundEngine::kMaxVoices][kChannelsPerVoice] = {};
    // The authored instrument's own knobs: not per voice, because they describe
    // the INSTRUMENT. Resolved with the per-voice pointers in prepare() and
    // written by the audio thread through setGlobalControls().
    MYFLT* globalPtr[CsoundEngine::kNumGlobalControls] = {};

    std::vector<float> voiceBuf[CsoundEngine::kMaxVoices];
    std::array<float, CsoundEngine::kKsmps> carryBuf[CsoundEngine::kMaxVoices] {};
    int carryCount = 0;

    // Phase-2 (spec S2): whichever orchestra text is currently compiled — empty
    // means the built-in Phase-1 orchestra. Set only on a SUCCESSFUL (re)compile,
    // right before `ready` is published; read by orchestraText() (message/
    // compile-thread only) and by prepare()'s own early-out check on the next call.
    std::string compiledOrchestraText;

    // Publish order (D9): pointers + buffers are fully set up in prepare()
    // BEFORE this is stored (release); the audio thread only ever loads it
    // (acquire) and never touches the instance before observing true — no
    // callback lock needed.
    std::atomic<bool> ready { false };

    ~Impl()
    {
        if (csound != nullptr)
        {
            // Destroy is part of the serialized lifecycle sequence, and this one
            // can run while the offline probe (renderBareOscillator, a detached
            // thread holding no reference to this processor) is inside its own
            // create/compile — so it takes the same lock every other lifecycle
            // call takes. See csoundLifecycleGlobal().
            const std::lock_guard<std::mutex> lifecycleLock (csoundLifecycleGlobal());
            csoundDestroy(csound);
        }
    }

    // Message/setup-thread-only helper (warmup): sets a channel by name.
    void setNamedChannel (const char* prefix, int voice1based, double value)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "%s%d", prefix, voice1based);
        csoundSetControlChannel(csound, name, (MYFLT) value);
    }

    // Message/setup-thread-only helper: resolves and caches one channel MYFLT*.
    bool resolveChannelPtr (const char* prefix, int voice1based, MYFLT*& out)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "%s%d", prefix, voice1based);
        return csoundGetChannelPtr(csound, &out, name,
            CSOUND_CONTROL_CHANNEL | CSOUND_INPUT_CHANNEL) == 0 && out != nullptr;
    }

    bool resolveGlobalPtr (const char* name, MYFLT*& out)
    {
        return csoundGetChannelPtr(csound, &out, name,
            CSOUND_CONTROL_CHANNEL | CSOUND_INPUT_CHANNEL) == 0 && out != nullptr;
    }
};

CsoundEngine::CsoundEngine() : impl (std::make_unique<Impl>()) {}
CsoundEngine::~CsoundEngine() = default;

int CsoundEngine::effectiveOversampleFactor (double sampleRate, int requested)
{
    // Only 1/2/4 are meaningful, and kKsmps must divide by the factor so a single
    // csoundPerformKsmps always yields a whole number of host samples. Anything
    // else falls back to 1 rather than producing a fractional-length block.
    if (requested != 1 && requested != 2 && requested != 4)
        requested = 1;
    if (kKsmps % requested != 0)
        requested = 1;

    // Cap the ABSOLUTE engine rate, not just the factor. At a 96 kHz host rate
    // 4x would mean 384 kHz — double the CPU again, to push away a Nyquist that
    // is already twice as far off as the 48 kHz case this was measured for. The
    // benefit saturates around 192 kHz, so a high-rate host gets the same
    // cleanliness at the factor that actually reaches it.
    while (requested > 1 && sampleRate * (double) requested > 200000.0)
        requested /= 2;

    return requested;
}

int CsoundEngine::oversampleFactor() const
{
    return impl->osFactor;
}

bool CsoundEngine::prepare (double sampleRate, int maxBlockSize, const char* orchestraText,
                            int oversampleFactor)
{
    // The first thing that happens, before any Csound symbol is touched: if the
    // library is not there, prepare() fails and the engine stays inert. Callers
    // already treat that as "no LRO" and go silent.
    if (! csoundLibraryReady())
        return false;

    // Phase-0 verified: MYFLT is double on this Homebrew build. A mismatch
    // here means spout would be read at the wrong width further down.
    assert(sizeof(MYFLT) == 8 &&
           "CsoundEngine assumes MYFLT==double (8 bytes), matching this machine's "
           "Homebrew Csound build (Phase-0 verified).");

    oversampleFactor = effectiveOversampleFactor (sampleRate, oversampleFactor);

    // What Csound itself is compiled and run at. Everything the CALLER sees —
    // startBlock/renderUpTo/voiceBuffer — stays in host samples at `sampleRate`.
    const double engineRate = sampleRate * (double) oversampleFactor;

    const int wantedCapacity = maxBlockSize > 0 ? maxBlockSize : 1;

    // Phase-2 (spec S2): nullptr means "the built-in orchestra"; compare against
    // whatever is CURRENTLY compiled (empty == built-in) so the D9 early-out just
    // below can never silently swallow a genuine orchestra-swap request — only a
    // re-prepare with the SAME text at the SAME sample rate may take that path.
    const bool requestedIsBuiltIn = (orchestraText == nullptr);
    const bool sameOrchestraAlreadyCompiled =
        requestedIsBuiltIn ? impl->compiledOrchestraText.empty()
                            : impl->compiledOrchestraText == orchestraText;

    // D9 early-out: already prepared at this sample rate with this same text.
    // Hosts call prepareToPlay repeatedly (e.g. on every play/stop); only the
    // FIRST call (or an actual sample-rate/orchestra change) may (re)compile/
    // rewarm the Csound instance. A host block-size-only change just grows the
    // (message-thread-owned) buffers -- no recompile needed.
    // The oversampling factor is part of what got COMPILED (it sets Csound's own
    // sr, and an authored orchestra may read `sr` to bound its own bandwidth), so
    // a factor change must take the full recompile path, never this early-out.
    // A LATCHED engine (performanceEnded — its Csound performance is over and
    // renderUpTo emits only silence) must fall through too: taking the early-out
    // would report success for an engine that can never sound again, and every
    // ordinary re-prepare (transport stop/start, block-size change, session
    // reload) matches "same text, same rate". Only the full recompile below
    // builds a fresh performance and clears the latch.
    if (impl->ready.load(std::memory_order_acquire) && impl->preparedSampleRate == sampleRate
        && impl->osFactor == oversampleFactor
        && sameOrchestraAlreadyCompiled
        && ! impl->performanceEnded.load(std::memory_order_acquire))
    {
        if (wantedCapacity > impl->capacity)
        {
            impl->ready.store(false, std::memory_order_release); // audio thread must not read mid-resize
            for (int v = 0; v < kMaxVoices; ++v)
                impl->voiceBuf[v].assign((size_t) wantedCapacity, 0.0f);
            impl->capacity  = wantedCapacity;
            impl->writePos  = 0;
            impl->blockSize = 0;
            impl->carryCount = 0;
            impl->ready.store(true, std::memory_order_release);
        }
        return true;
    }

    // (Re)compile path -- first prepare(), or a sample-rate change. Keep the
    // engine inert (never half-armed) until every step below has succeeded.
    impl->ready.store(false, std::memory_order_release);

    // Held across destroy/create/compile/start only — NOT across the warmup below,
    // which is per-instance csoundPerformKsmps and safe concurrently. Released
    // explicitly after the contract checks; every early return releases it too.
    std::unique_lock<std::mutex> lifecycleLock (csoundLifecycleGlobal());

    if (impl->csound != nullptr)
    {
        csoundDestroy(impl->csound);
        impl->csound = nullptr;
    }

    impl->csound = csoundCreate(nullptr); // NEVER in the constructor (host plugin-scan
                                           // constructs processors without prepareToPlay)
    if (impl->csound == nullptr)
    {
        std::fprintf(stderr, "CsoundEngine: csoundCreate() returned null\n");
        return false;
    }

    CSOUND* cs = impl->csound;
    csoundCreateMessageBuffer(cs, 0); // queue-only; never print straight to console
    csoundSetOption(cs, "-n");
    csoundSetOption(cs, "-d");

    // Both the built-in orchestra's baked-in header and an authored orchestra's
    // %SR% marker get the OVERSAMPLED rate: that is the rate Csound computes at,
    // and it is also the rate an authored patch must see when it derives its own
    // partial count or FM index from `sr` — telling it 48000 while running it at
    // 192000 would keep it bandwidth-limited to a Nyquist that no longer applies.
    const std::string rawText = requestedIsBuiltIn ? buildOrchestra(engineRate) : std::string(orchestraText);
    const std::string csdText = substituteSr(substituteScoreLifetime(rawText), engineRate);
    if (csoundCompileCsdText(cs, csdText.c_str()) != 0)
    {
        std::fprintf(stderr, "CsoundEngine: compile failed:\n%s\n", drainMessages(cs).c_str());
        csoundDestroyMessageBuffer(cs);
        csoundDestroy(cs);
        impl->csound = nullptr;
        return false;
    }
    if (csoundStart(cs) != 0)
    {
        std::fprintf(stderr, "CsoundEngine: csoundStart failed:\n%s\n", drainMessages(cs).c_str());
        csoundDestroyMessageBuffer(cs);
        csoundDestroy(cs);
        impl->csound = nullptr;
        return false;
    }

    if (csoundGetSizeOfMYFLT() != (int) sizeof(MYFLT))
    {
        std::fprintf(stderr, "CsoundEngine: FATAL MYFLT size mismatch (runtime=%d, compile-time=%d)\n",
                      csoundGetSizeOfMYFLT(), (int) sizeof(MYFLT));
        csoundDestroyMessageBuffer(cs);
        csoundDestroy(cs);
        impl->csound = nullptr;
        return false;
    }
    if ((int) csoundGetKsmps(cs) != kKsmps)
    {
        std::fprintf(stderr, "CsoundEngine: FATAL ksmps mismatch (runtime=%u, expected=%d)\n",
                      csoundGetKsmps(cs), (unsigned) kKsmps);
        csoundDestroyMessageBuffer(cs);
        csoundDestroy(cs);
        impl->csound = nullptr;
        return false;
    }
    // Untrusted preset-authored orchestra text (hand-edited .t5p): chnget
    // resolves the named control channels independent of the orchestra's
    // header nchnls, so a mismatched header would still reach ready==true.
    // renderUpTo() de-interleaves spout with a hardcoded kMaxVoices stride
    // (spout[s*kMaxVoices+v]) -- a smaller runtime nchnls reads past the
    // spout buffer on the audio thread.
    if ((int) csoundGetNchnls(cs) != kMaxVoices)
    {
        std::fprintf(stderr, "CsoundEngine: FATAL nchnls mismatch (runtime=%u, expected=%d)\n",
                      csoundGetNchnls(cs), (unsigned) kMaxVoices);
        csoundDestroyMessageBuffer(cs);
        csoundDestroy(cs);
        impl->csound = nullptr;
        return false;
    }

    // Compile phase over: the remaining work (channel resolution, buffer sizing,
    // the ~1.2 s warmup) touches only this instance.
    lifecycleLock.unlock();

    impl->capacity = wantedCapacity;
    for (int v = 0; v < kMaxVoices; ++v)
    {
        impl->voiceBuf[v].assign((size_t) impl->capacity, 0.0f);
        impl->carryBuf[v].fill(0.0f);
        // Cleared here, not just on construction: this same Impl is reused across
        // recompiles, and a decimator still holding the previous orchestra's tail
        // would bleed it into the new one's first 31 output samples.
        impl->decim1[v].reset();
        impl->decim2[v].reset();
        for (auto& s : impl->decimScratch[v]) s = 0.0f;
    }
    impl->carryCount = 0;
    impl->writePos   = 0;
    impl->blockSize  = 0;
    impl->osFactor   = oversampleFactor;
    impl->performanceEnded.store(false, std::memory_order_release); // fresh performance, fresh latch

    // ---- D4 warm-up: absorb the one-time lazy-init allocation residue
    // inside the first gated performKsmps passes, BEFORE the instance is
    // exposed to the audio thread. Uses name-based csoundSetControlChannel
    // (message-thread only, never on the RT path) -- cached pointers are
    // resolved AFTER this block, so warmup cannot use them yet. ----
    for (int v = 1; v <= kMaxVoices; ++v)
    {
        const double freq = 110.0 * std::pow(8.0, (double) (v - 1) / (double) (kMaxVoices - 1));
        impl->setNamedChannel("gate", v, 1.0);
        impl->setNamedChannel("freq", v, freq);
        impl->setNamedChannel("vel",  v, 0.8);
        impl->setNamedChannel("pres", v, 0.0);
        impl->setNamedChannel("timb", v, 0.0);
        impl->setNamedChannel("trig", v, 1.0);
    }
    // Warm-up durations are in SECONDS, so the block counts follow the rate
    // Csound actually runs at — at 4x, `sampleRate` here would warm up for a
    // quarter of the intended second.
    const long warmupOnBlocks = (long) std::llround(1.0 * engineRate / (double) kKsmps);
    for (long i = 0; i < warmupOnBlocks; ++i)
        csoundPerformKsmps(cs);

    // ---- Level normalisation: what does THIS orchestra actually deliver? ----
    // One voice, one reference note, measured through the same halfband stages
    // renderUpTo runs on the live signal — so the number is the level a player
    // hears, not the level at Csound's internal rate. See outputTrim()'s header
    // comment for the measurement that made this necessary and why no constant
    // can replace it.
    //
    // Placed AFTER the gated warmup and BEFORE the gate-off settle below, and both
    // halves of that matter:
    //
    // - after the warmup, because `balance`'s rms follower starts at zero and hands
    //   a body building from silence an enormous first-milliseconds gain
    //   (tools/lco_measure.scaffold documents the same trap offline, where two
    //   library entries were reported as clipping on that artefact alone). By here
    //   the follower has seen a second of signal, as it has in a plugin that has
    //   been open for a while.
    // - before the settle, because this pass OPENS voice 1's gate, and the settle
    //   is what closes every gate down again. Sited after it instead, voice 1 would
    //   publish with `kgate portk kgateraw, 0.001` only one k-cycle into its decay
    //   — 0.40 at 48 kHz/1x, 0.79 at 4x — i.e. the engine would go ready with one
    //   voice audibly a third to four-fifths open on a 220 Hz body it is about to
    //   be handed a different note for. On the swap path primeForTakeover's own
    //   0.25 s hides that; on the two paths that do not prime (the bootstrap
    //   compile and an instant adopt) it would not be hidden.
    //
    // The knob channels still hold what the orchestra's own head `chnset`s — the
    // AUTHOR's values, since the plugin does not write its parameters until the
    // first audio block. That is the right reference: it is the instrument as
    // authored, it is reproducible, and it is measured once per orchestra exactly
    // as SamplePlayer normalises once per buffer. A knob that moves the level
    // afterwards moves it, which is the body's business (§4 forbids an axis whose
    // only effect IS the level).
    {
        for (int v = 1; v <= kMaxVoices; ++v)
        {
            impl->setNamedChannel("gate", v, 0.0);
            impl->setNamedChannel("trig", v, 0.0);
        }
        // trig 0 -> 1 below is a real edge, so `changed2(ktrig)` fires and `knote`
        // starts at 0: the body sees a note-on, not an instance that has been up
        // for a second. Voice 1 only — this is one instrument's level, not a chord's.
        impl->setNamedChannel("gate", 1, 1.0);
        impl->setNamedChannel("freq", 1, kLevelMeasureFreqHz);
        impl->setNamedChannel("vel",  1, 1.0);
        impl->setNamedChannel("pres", 1, 0.0);
        // SynthVoice::kTimbreRest — what a note with no MPE timbre actually
        // carries, so the measured level is the level of an ordinary note. It
        // was 64/127 while CC 74 was read as a centre detent; the voice now
        // reports the TRAVEL from the value its note began on, which is 0 for an
        // untouched note on any controller.
        impl->setNamedChannel("timb", 1, 0.0);
        impl->setNamedChannel("trig", 1, 1.0);

        const int  osF        = oversampleFactor;
        const int  perPerform = kKsmps / osF;
        const long settleBlk  = (long) std::llround(kLevelSettleSeconds  * engineRate / (double) kKsmps);
        const long measureBlk = (long) std::llround(kLevelMeasureSeconds * engineRate / (double) kKsmps);

        double sumSq   = 0.0;
        long   nSamples = 0;
        float  peak     = 0.0f;
        bool   finite   = true;

        for (long i = 0; i < settleBlk + measureBlk && finite; ++i)
        {
            if (csoundPerformKsmps(cs) != 0)
                break;                      // score ended: keep whatever was measured
            const MYFLT* spout = csoundGetSpout(cs);

            // Voice 1 == spout channel 0. Mirrors renderUpTo's de-interleave and
            // decimation exactly; decim1[0]/decim2[0] are reset again below so no
            // state from this pass reaches the first audible block.
            for (int s = 0; s < perPerform; ++s)
            {
                float y;
                if (osF == 1)
                {
                    y = (float) spout[(size_t) s * (size_t) kMaxVoices];
                }
                else if (osF == 2)
                {
                    y = impl->decim1[0].process(
                            (float) spout[(size_t) (2 * s)     * (size_t) kMaxVoices],
                            (float) spout[(size_t) (2 * s + 1) * (size_t) kMaxVoices]);
                }
                else
                {
                    const float a = impl->decim1[0].process(
                            (float) spout[(size_t) (4 * s)     * (size_t) kMaxVoices],
                            (float) spout[(size_t) (4 * s + 1) * (size_t) kMaxVoices]);
                    const float b = impl->decim1[0].process(
                            (float) spout[(size_t) (4 * s + 2) * (size_t) kMaxVoices],
                            (float) spout[(size_t) (4 * s + 3) * (size_t) kMaxVoices]);
                    y = impl->decim2[0].process(a, b);
                }

                if (i < settleBlk)
                    continue;               // decimators still primed, body still arriving

                // NOT redundant with the silence guard below: std::max(a, NaN)
                // returns a, so a NaN would slip past `peak` untouched and then be
                // divided into the trim. The orchestra text is LLM-authored and an
                // unstable filter in it lands exactly here.
                if (! std::isfinite(y)) { finite = false; break; }
                peak = std::max(peak, std::fabs(y));
                sumSq += (double) y * (double) y;
                ++nSamples;
            }
        }

        impl->decim1[0].reset();
        impl->decim2[0].reset();

        const double rms = nSamples > 0 ? std::sqrt(sumSq / (double) nSamples) : 0.0;
        float trim = 1.0f;
        if (! finite)
        {
            std::fprintf(stderr, "CsoundEngine: non-finite sample in the level reference "
                                 "render - leaving the orchestra untrimmed\n");
        }
        else if (peak > kLevelSilenceFloor && rms >= (double) kLevelReferenceFloorRms)
        {
            // The sampler's own law, same two numbers: drive the rms onto the
            // target unless that would push the peak past the ceiling. A body
            // whose crest is higher than target/ceiling allows is peak-capped and
            // lands a little quieter — which is what SamplePlayer does to a spiky
            // sample too, so the two engines stay consistent with each other
            // rather than each being right on its own.
            const double byRms  = std::pow(10.0, (double) kLevelTargetRmsDb   / 20.0) / rms;
            const double byPeak = std::pow(10.0, (double) kLevelPeakCeilingDb / 20.0) / (double) peak;
            trim = (float) std::min(byRms, byPeak);
            trim = std::min(std::max(trim, kLevelTrimMin), kLevelTrimMax);
        }
        else
        {
            // Nothing usable at the reference pitch — silence, a body that speaks
            // only in another register, or one that has already decayed. Trim stays
            // 1.0: the body keeps the level it was written at, which is a known
            // quantity, instead of one extrapolated from a window that did not hear
            // it. renderBareOscillator refuses on the same ground.
            std::fprintf(stderr, "CsoundEngine: level reference render at %.0f Hz came back at "
                                 "%.1f dBFS rms (floor %.1f) - leaving the orchestra untrimmed\n",
                         kLevelMeasureFreqHz,
                         20.0 * std::log10(std::max(rms, 1.0e-12)),
                         20.0 * std::log10((double) kLevelReferenceFloorRms));
        }
        impl->outputTrim = trim;
    }

    for (int v = 1; v <= kMaxVoices; ++v)
        impl->setNamedChannel("gate", v, 0.0);
    const long warmupOffBlocks = (long) std::llround(0.2 * engineRate / (double) kKsmps);
    for (long i = 0; i < warmupOffBlocks; ++i)
        csoundPerformKsmps(cs);

    for (int v = 1; v <= kMaxVoices; ++v)
    {
        impl->setNamedChannel("gate", v, 0.0);
        impl->setNamedChannel("freq", v, 0.0);
        impl->setNamedChannel("vel",  v, 0.0);
        impl->setNamedChannel("pres", v, 0.0);
        impl->setNamedChannel("timb", v, 0.0);
        impl->setNamedChannel("trig", v, 0.0);
    }
    // Flush the zeroed channels through one perform pass so the last-performed
    // k-state reflects a clean zero baseline before the audio thread takes over.
    // (The trig channel is inert now that the orchestra is a standing tone, but
    // zeroing every channel here still leaves a defined starting state.)
    csoundPerformKsmps(cs);

    // ---- resolve all 16x6 channel pointers ONCE, post-warmup. The audio
    // thread never calls csoundGetChannelPtr/csoundSetControlChannel (name-
    // hash lookups) -- only these cached raw MYFLT* pointers. ----
    bool allResolved = true;
    static const char* const kPrefixes[Impl::kChannelsPerVoice] = { "gate", "freq", "vel", "pres", "timb", "trig" };
    for (int v = 0; v < kMaxVoices && allResolved; ++v)
        for (int c = 0; c < Impl::kChannelsPerVoice && allResolved; ++c)
            allResolved = impl->resolveChannelPtr(kPrefixes[c], v + 1, impl->channelPtr[v][c]);

    // The authored instrument's knobs. Deliberately NOT part of allResolved: an
    // orchestra written before this contract existed — a .t5p saved last month,
    // a hand-edited body — carries no lroP channel at all, and refusing to
    // prepare would turn "this preset has no knobs" into "this preset does not
    // play". A null stays null and setGlobalControls skips it.
    for (int i = 0; i < kNumGlobalControls; ++i)
    {
        char name[16];
        // The last three are the per-part LEVELS, and they carry the scaffold's
        // own names rather than a new set: every orchestra ever written by this
        // path already reads "osc1vol".."osc3vol" as `kvol1`..`kvol3`, because
        // the author is told to scale layer N by `kvolN`. So an instrument
        // authored long before these were driven gains its column levels by
        // being loaded — nothing about it has to change.
        if (i >= kNumKnobControls)
            std::snprintf(name, sizeof(name), "osc%dvol", 1 + i - kNumKnobControls);
        else
            std::snprintf(name, sizeof(name), "lroP%d%c", 1 + i / 4, "abcd"[i % 4]);
        if (! impl->resolveGlobalPtr(name, impl->globalPtr[i]))
            impl->globalPtr[i] = nullptr;
    }

    if (!allResolved)
    {
        std::fprintf(stderr, "CsoundEngine: failed to resolve one or more channel pointers\n");
        // Re-take the lifecycle lock dropped above: this is the one failure path
        // that destroys an instance AFTER the compile phase, and destroy belongs
        // inside the serialized sequence like every other lifecycle call.
        lifecycleLock.lock();
        csoundDestroyMessageBuffer(cs);
        csoundDestroy(cs);
        impl->csound = nullptr;
        return false;
    }

    impl->preparedSampleRate = sampleRate;
    impl->engineSampleRate   = engineRate;
    // Phase-2 (spec S2): record what got compiled — empty for the built-in
    // orchestra — so the NEXT prepare() call's early-out (above) and
    // orchestraText() (Phase 5 preset save) both see the truth.
    impl->compiledOrchestraText = requestedIsBuiltIn ? std::string() : std::string(orchestraText);

    // Publish order (D9): pointers + buffers fully set up BEFORE the ready
    // flag is stored with release semantics; see the Impl::ready comment.
    impl->ready.store(true, std::memory_order_release);
    return true;
}

const std::string& CsoundEngine::orchestraText() const
{
    return impl->compiledOrchestraText;
}

float CsoundEngine::outputTrim() const
{
    return impl->outputTrim;
}

void CsoundEngine::primeForTakeover (const float* epochs, const float* freqs)
{
    // Message/compile-thread only (spec S3) -- see this method's header-comment
    // for the full rationale. Must run AFTER prepare() has resolved the cached
    // channel pointers (guarded defensively below) and BEFORE this engine is
    // published as a swap target; the audio thread never calls this.
    if (! impl->ready.load(std::memory_order_acquire))
        return;

    CSOUND* cs = impl->csound;
    for (int v = 0; v < kMaxVoices; ++v)
    {
        auto& ptrs = impl->channelPtr[v];
        *ptrs[1] = (MYFLT) freqs[v];    // freq  (index matches kPrefixes order in prepare())
        *ptrs[5] = (MYFLT) epochs[v];   // trig
        // gate (ptrs[0]) is left untouched -- prepare()'s warmup already left it
        // at 0, and priming must never open the gate (that would be audible).
    }

    // 0.25 s at the rate Csound runs at, not the host rate — same reason as the
    // warm-up loops in prepare().
    const long primeBlocks = (long) std::llround(0.25 * impl->engineSampleRate / (double) kKsmps);
    for (long i = 0; i < primeBlocks; ++i)
        csoundPerformKsmps(cs);
}

bool CsoundEngine::isReady() const
{
    return impl->ready.load(std::memory_order_acquire);
}

bool CsoundEngine::performanceHasEnded() const
{
    return impl->performanceEnded.load(std::memory_order_acquire);
}

void CsoundEngine::setVoiceControls (int voiceIndex, const VoiceControls& c)
{
    if (voiceIndex < 0 || voiceIndex >= kMaxVoices)
        return;
    if (! impl->ready.load(std::memory_order_acquire))
        return;

    auto& ptrs = impl->channelPtr[voiceIndex];
    *ptrs[0] = (MYFLT) c.gate;
    *ptrs[1] = (MYFLT) c.freqHz;
    *ptrs[2] = (MYFLT) c.velocity;
    *ptrs[3] = (MYFLT) c.pressure;
    *ptrs[4] = (MYFLT) c.timbre;
    *ptrs[5] = (MYFLT) c.trigEpoch;
}

void CsoundEngine::setGlobalControls (const float* values)
{
    if (values == nullptr)
        return;
    if (! impl->ready.load(std::memory_order_acquire))
        return;

    for (int i = 0; i < kNumGlobalControls; ++i)
        if (impl->globalPtr[i] != nullptr)
            *impl->globalPtr[i] = (MYFLT) values[i];
}

void CsoundEngine::startBlock (int numSamples)
{
    if (! impl->ready.load(std::memory_order_acquire))
        return;

    int bs = numSamples;
    if (bs > impl->capacity) bs = impl->capacity; // defensive clamp; prepare()'s maxBlockSize contract should make this unreachable
    if (bs < 0) bs = 0;
    impl->blockSize = bs;
    impl->writePos  = 0;

    if (impl->carryCount > 0)
    {
        const int n = std::min(impl->carryCount, impl->blockSize);
        for (int v = 0; v < kMaxVoices; ++v)
            std::memcpy(impl->voiceBuf[v].data(), impl->carryBuf[v].data(), (size_t) n * sizeof(float));
        impl->writePos = n;

        const int remaining = impl->carryCount - n;
        if (remaining > 0)
            for (int v = 0; v < kMaxVoices; ++v)
                std::memmove(impl->carryBuf[v].data(), impl->carryBuf[v].data() + n, (size_t) remaining * sizeof(float));
        impl->carryCount = remaining;
    }
}

void CsoundEngine::renderUpTo (int upToSample)
{
    if (! impl->ready.load(std::memory_order_acquire))
        return;

    const int target = upToSample + 1;
    CSOUND* cs = impl->csound;
    const int osF = impl->osFactor;
    // The orchestra's measured level trim (prepare(); see outputTrim()). Applied
    // here rather than further downstream because decimScratch feeds BOTH the
    // block copy and the carry store, so one multiply per sample covers both and
    // a block boundary can never fall between a trimmed and an untrimmed sample.
    const float trim = impl->outputTrim;
    // Host-rate samples one csoundPerformKsmps yields: kKsmps at 1x, half that
    // at 2x, a quarter at 4x. prepare() guarantees kKsmps divides by osFactor,
    // so this is exact and the carry store (sized kKsmps) always has room.
    const int perPerform = kKsmps / osF;

    // Idempotent/monotonic: if writePos already covers `target` (this
    // sub-range was already rendered by an earlier call within the same
    // block), the loop body never runs.
    while (impl->writePos < target && impl->writePos < impl->blockSize)
    {
        // Non-zero return = the performance has ENDED (score exhausted, or a
        // fatal runtime error): Csound stops refilling spout, so de-interleaving
        // it again would replay the last k-cycle forever — mid-note a standing
        // buzz until the next recompile (measured 2026-08-06). Unreachable with
        // the ~63-year score above, but any future regression now fails to
        // silence instead of to a buzz. Also measured: every post-end
        // csoundPerformKsmps call queues one "Score finished in
        // csoundPerformKsmps() with 2." line into the engine's message buffer
        // (csoundCreateMessageBuffer in prepare()), which the live path never
        // drains — one heap allocation plus Csound's message-buffer mutex per
        // k-cycle on the audio thread, ~16.9 MB RSS per 100 s of audio at the
        // 176400/64 engine rate. So once ended, Csound is never called again
        // from here — impl->performanceEnded latches the first non-zero return
        // and every call after that skips straight to silence.
        bool performanceEnded = impl->performanceEnded.load(std::memory_order_relaxed);
        if (! performanceEnded && csoundPerformKsmps(cs) != 0)
        {
            impl->performanceEnded.store(true, std::memory_order_release);
            performanceEnded = true;
        }
        MYFLT* spout = csoundGetSpout(cs);

        // De-interleave spout and, when oversampling, band-limit + decimate it
        // to the host rate. Everything downstream of here counts HOST samples.
        for (int v = 0; v < kMaxVoices; ++v)
        {
            float* out = impl->decimScratch[v];
            if (performanceEnded)
            {
                for (int s = 0; s < perPerform; ++s)
                    out[s] = 0.0f;
            }
            else if (osF == 1)
            {
                for (int s = 0; s < kKsmps; ++s)
                    out[s] = boundOutput(trim * (float) spout[(size_t) s * (size_t) kMaxVoices + (size_t) v]);
            }
            else if (osF == 2)
            {
                for (int s = 0; s < perPerform; ++s)
                    out[s] = boundOutput(trim * impl->decim1[v].process(
                        (float) spout[(size_t) (2 * s)     * (size_t) kMaxVoices + (size_t) v],
                        (float) spout[(size_t) (2 * s + 1) * (size_t) kMaxVoices + (size_t) v]));
            }
            else // osF == 4: two cascaded halfband stages, 192k -> 96k -> 48k
            {
                for (int s = 0; s < perPerform; ++s)
                {
                    const float a = impl->decim1[v].process(
                        (float) spout[(size_t) (4 * s)     * (size_t) kMaxVoices + (size_t) v],
                        (float) spout[(size_t) (4 * s + 1) * (size_t) kMaxVoices + (size_t) v]);
                    const float b = impl->decim1[v].process(
                        (float) spout[(size_t) (4 * s + 2) * (size_t) kMaxVoices + (size_t) v],
                        (float) spout[(size_t) (4 * s + 3) * (size_t) kMaxVoices + (size_t) v]);
                    out[s] = boundOutput(trim * impl->decim2[v].process(a, b));
                }
            }
        }

        const int samplesToBlock = std::min(perPerform, impl->blockSize - impl->writePos);
        for (int v = 0; v < kMaxVoices; ++v)
        {
            float* dst = impl->voiceBuf[v].data() + impl->writePos;
            const float* src = impl->decimScratch[v];
            for (int s = 0; s < samplesToBlock; ++s)
                dst[s] = src[s];
        }

        const int leftover = perPerform - samplesToBlock;
        if (leftover > 0)
        {
            // Surplus beyond this block's end goes to the carry store,
            // replayed at the START of the next startBlock().
            for (int v = 0; v < kMaxVoices; ++v)
            {
                float* dst = impl->carryBuf[v].data();
                const float* src = impl->decimScratch[v] + samplesToBlock;
                for (int s = 0; s < leftover; ++s)
                    dst[s] = src[s];
            }
            impl->carryCount = leftover;
        }
        else
        {
            impl->carryCount = 0;
        }

        impl->writePos += samplesToBlock;
    }
}

const float* CsoundEngine::voiceBuffer (int voiceIndex) const
{
    if (voiceIndex < 0 || voiceIndex >= kMaxVoices)
        return nullptr;
    // Not-ready guard: before a successful prepare() the vectors may be empty;
    // data() would be a non-null-but-invalid base for caller pointer arithmetic.
    if (! impl->ready.load (std::memory_order_acquire))
        return nullptr;
    return impl->voiceBuf[voiceIndex].data();
}

std::vector<float> CsoundEngine::renderBareOscillator (const std::string& orchestraText,
                                                       double sampleRate,
                                                       double freqHz,
                                                       double seconds,
                                                       double gateOffSeconds,
                                                       int    decimateBy)
{
    // No library, no render — an empty buffer, which is what every caller here
    // already handles. Checked before the arguments, since nothing below can
    // matter without Csound.
    if (! csoundLibraryReady())
        return {};

    // `! (x > 0.0)` rather than `x <= 0.0`: it rejects NaN too, which every
    // comparison below would silently pass through into the render.
    if (orchestraText.empty() || ! (sampleRate > 0.0) || ! (seconds > 0.0)
        || ! (freqHz > 0.0))
        return {};

    // Upper bounds, not just lower ones: a caller passing a wild `seconds` would
    // overflow the block count and make reserve() throw out of a detached thread —
    // std::terminate, from a probe whose entire contract is that it cannot damage
    // anything. 60 s is far past any useful probe window.
    sampleRate = std::min(sampleRate, kMaxProbeSampleRate);
    seconds    = std::min(seconds, 60.0);
    freqHz     = std::min(freqHz, sampleRate * 0.5);

    // Clamp INTO the window, in BLOCK units, because the loop below compares block
    // indices: below one block the gate closes before the first perform pass
    // (guaranteed silence), and at `seconds` the index equals totalBlocks, which
    // the `b < totalBlocks` loop never reaches (the gate never closes, so no
    // release is captured). Both ends need a whole block of margin, not an epsilon.
    // A window too short to hold both is left gate-open rather than silent.
    {
        const double oneBlock = (double) kKsmps / sampleRate;
        const double minGate  = oneBlock;
        const double maxGate  = seconds - oneBlock;
        gateOffSeconds = (maxGate <= minGate)
                       ? minGate
                       : std::min(std::max(gateOffSeconds, minGate), maxGate);
    }

    // Own instance, own lifetime. The lock covers create/compile/start and the
    // destroy for the same reason prepare() takes it — see csoundLifecycleGlobal().
    // It is RELEASED before the render loop: that loop touches only this instance,
    // and holding it there would stall the live orchestra swap (prepare() takes the
    // same mutex) for the whole render, delaying the sound the user is waiting for.
    std::unique_lock<std::mutex> lifecycleLock (csoundLifecycleGlobal());

    CSOUND* cs = csoundCreate(nullptr);
    if (cs == nullptr)
        return {};

    // RAII: every early return below must destroy the instance, and there are
    // nine of them. A guard object is the only way that stays true when a later
    // contract check is inserted in the middle. It re-takes the lifecycle lock if
    // the render already dropped it, so destroy stays inside the serialized
    // sequence; declared AFTER the lock, it therefore runs BEFORE the lock's own
    // destructor releases it.
    struct Guard
    {
        CSOUND* cs;
        std::unique_lock<std::mutex>& lk;
        ~Guard()
        {
            if (cs == nullptr) return;
            if (! lk.owns_lock()) lk.lock();
            csoundDestroyMessageBuffer(cs);
            csoundDestroy(cs);
        }
    } guard { cs, lifecycleLock };

    csoundCreateMessageBuffer(cs, 0);
    csoundSetOption(cs, "-n");   // no audio device: this render is never heard
    csoundSetOption(cs, "-d");   // no displays

    if (csoundCompileCsdText(cs, substituteSr(orchestraText, sampleRate).c_str()) != 0)
    {
        std::fprintf(stderr, "CsoundEngine::renderBareOscillator: compile failed:\n%s\n",
                      drainMessages(cs).c_str());
        return {};
    }
    if (csoundStart(cs) != 0)
    {
        std::fprintf(stderr, "CsoundEngine::renderBareOscillator: start failed:\n%s\n",
                      drainMessages(cs).c_str());
        return {};
    }

    // The SAME three contract checks prepare() makes, for the same reasons: the
    // spout read below strides by kMaxVoices and reads MYFLT, so a mismatched
    // nchnls or MYFLT width would read out of bounds. An orchestra reaching this
    // path can come from a hand-edited .t5p, so it is not trusted here either.
    if (csoundGetSizeOfMYFLT() != (int) sizeof(MYFLT)
        || (int) csoundGetKsmps(cs) != kKsmps
        || (int) csoundGetNchnls(cs) != kMaxVoices)
    {
        std::fprintf(stderr, "CsoundEngine::renderBareOscillator: contract mismatch "
                             "(MYFLT=%d ksmps=%u nchnls=%u)\n",
                      csoundGetSizeOfMYFLT(), csoundGetKsmps(cs), csoundGetNchnls(cs));
        return {};
    }

    // Voice 1 only. Names must match prepare()'s kPrefixes order/spelling.
    MYFLT* gate = nullptr; MYFLT* freq = nullptr; MYFLT* vel  = nullptr;
    MYFLT* pres = nullptr; MYFLT* timb = nullptr; MYFLT* trig = nullptr;
    auto resolve = [cs] (const char* name, MYFLT*& out)
    {
        return csoundGetChannelPtr(cs, &out, name,
                   CSOUND_CONTROL_CHANNEL | CSOUND_INPUT_CHANNEL) == 0 && out != nullptr;
    };
    if (! (resolve("gate1", gate) && resolve("freq1", freq) && resolve("vel1", vel)
           && resolve("pres1", pres) && resolve("timb1", timb) && resolve("trig1", trig)))
    {
        std::fprintf(stderr, "CsoundEngine::renderBareOscillator: channel resolve failed\n");
        return {};
    }

    // Performance controls AT REST: mid velocity, no pressure, timbre at rest —
    // the probe asks what the ORCHESTRA sounds like, not what a performance does
    // to it. `timb` is 0.0 and not 0.5 because 0.0 is what an untouched note
    // publishes: MPE's Y axis (CC74) rests at zero, measured on the Osmose, and
    // the bridge sends the upward half of SynthVoice::getTimbre() -- the travel
    // from where the note began, which is 0 for an untouched note whatever the
    // controller. Half-scale here would probe a Y position no untouched note
    // produces.
    *gate = (MYFLT) 1.0;  *freq = (MYFLT) freqHz;  *vel  = (MYFLT) 0.85;
    *pres = (MYFLT) 0.0;  *timb = (MYFLT) 0.0;     *trig = (MYFLT) 1.0;

    const long totalBlocks = (long) std::llround(seconds * sampleRate / (double) kKsmps);
    const long gateOffBlk  = (long) std::llround(gateOffSeconds * sampleRate / (double) kKsmps);

    std::vector<float> mono;
    mono.reserve((size_t) std::max(0L, totalBlocks) * (size_t) kKsmps);

    // Compile phase over — everything from here to the Guard touches only this
    // instance. Mirrors prepare()'s own unlock at the same point in its sequence.
    lifecycleLock.unlock();

    for (long b = 0; b < totalBlocks; ++b)
    {
        if (b == gateOffBlk)
            *gate = (MYFLT) 0.0;
        if (csoundPerformKsmps(cs) != 0)
            break;                       // score ended early: keep what was rendered
        const MYFLT* spout = csoundGetSpout(cs);
        for (int s = 0; s < kKsmps; ++s)
            mono.push_back((float) spout[(size_t) s * (size_t) kMaxVoices]);   // voice 1
    }

    // Drain and discard: the message buffer grows for the whole render (Csound
    // queues a per-note and an end-of-performance summary), and nothing has read
    // it since the compile check. Failures above already printed what they needed.
    (void) drainMessages(cs);

    // Band-limit and decimate to the rate the CALLER hears, through the very same
    // halfband stages renderUpTo runs on the live signal (Decimator2x, cascaded at
    // 4x). A probe rendered at the engine rate is what the authored body must see —
    // it may derive its partial count or FM index from `sr` — but everything that
    // lives above Nyquist of the host rate is removed before it reaches a speaker,
    // and a caller measuring the probe has to be given the same band. Ratio 1 or a
    // sample count that is not a whole multiple leaves the render untouched.
    if (decimateBy > 1 && ! mono.empty() && (mono.size() % (size_t) decimateBy) == 0)
    {
        Decimator2x d1, d2;
        std::vector<float> down;
        down.reserve(mono.size() / (size_t) decimateBy);
        if (decimateBy == 2)
        {
            for (size_t i = 0; i + 1 < mono.size(); i += 2)
                down.push_back(d1.process(mono[i], mono[i + 1]));
        }
        else   // 4: 4x -> 2x -> 1x, exactly renderUpTo's cascade
        {
            for (size_t i = 0; i + 3 < mono.size(); i += 4)
            {
                const float a = d1.process(mono[i],     mono[i + 1]);
                const float b = d1.process(mono[i + 2], mono[i + 3]);
                down.push_back(d2.process(a, b));
            }
        }
        mono.swap(down);
    }

    // Peak-normalise to -12 dBFS. A render that never left the noise floor is
    // returned empty instead: normalising it would manufacture a loud signal out
    // of numerical dust and hand the ear something to hallucinate words about.
    //
    // The finiteness check is NOT redundant with the peak guard: std::max(a, NaN)
    // returns a, so a NaN would skip past `peak` untouched and be scaled, encoded
    // and posted to the analyser. The orchestra text is LLM-authored, and an
    // unstable filter or a divide by zero in it lands here — tools/
    // csound_orch_check.cpp already rejects orchestras on exactly this ground.
    float peak = 0.0f;
    for (float v : mono)
    {
        if (! std::isfinite(v))
        {
            std::fprintf(stderr, "CsoundEngine::renderBareOscillator: non-finite sample, "
                                 "discarding the render\n");
            return {};
        }
        peak = std::max(peak, std::fabs(v));
    }
    constexpr float kSilenceFloor = 1.0e-6f;
    if (mono.empty() || peak < kSilenceFloor)
        return {};

    const float scale = 0.251f / peak;   // 0.251 == -12 dBFS
    for (float& v : mono)
        v *= scale;
    return mono;
}

#endif // T5YNTH_HAS_CSOUND
