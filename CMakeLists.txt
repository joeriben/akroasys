cmake_minimum_required(VERSION 3.22)

# Version is derived from T5YNTH_VERSION env (CI passes github.ref_name); dev
# builds fall back to `git describe` so the title bar shows a real version
# instead of 0.0.0.
if(DEFINED ENV{T5YNTH_VERSION})
    set(_T5YNTH_VERSION "$ENV{T5YNTH_VERSION}")
else()
    execute_process(
        COMMAND git describe --tags --abbrev=0
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _T5YNTH_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT _T5YNTH_VERSION)
        set(_T5YNTH_VERSION "0.0.0")
    endif()
endif()
# Keep the FULL tag (e.g. "v2.5.3-beta.1") for the update check — every T5ynth
# release is a GitHub pre-release, so the checker must compare full semver
# (incl. the -beta.N precedence), not just the X.Y.Z triplet JUCE requires.
set(_T5YNTH_FULL_VERSION "${_T5YNTH_VERSION}")
string(REGEX REPLACE "^v" "" _T5YNTH_VERSION "${_T5YNTH_VERSION}")
string(REGEX REPLACE "-.*$" "" _T5YNTH_VERSION "${_T5YNTH_VERSION}")
if(NOT _T5YNTH_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    set(_T5YNTH_VERSION "0.0.0")
    set(_T5YNTH_FULL_VERSION "0.0.0")
endif()

project(T5ynth VERSION ${_T5YNTH_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# libcurl: required on Linux (JUCE URL backend), optional elsewhere
if(UNIX AND NOT APPLE)
    find_package(CURL REQUIRED)
else()
    find_package(CURL QUIET)
endif()

# ── nlohmann/json (header-only, for config parsing) ──────
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(json)

# ── Signalsmith Stretch (header-only, for pitch-preserving transposition) ──────
FetchContent_Declare(
    signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        1.1.0
)
FetchContent_MakeAvailable(signalsmith_stretch)

add_subdirectory(JUCE)

# ── Csound engine ─────────────────────────────────────────────────────────
# The LRO IS Csound, so Csound travels with the app: third_party/csound/<platform>
# holds the exact payload a bundle ships — the library, its support libraries and
# the plugin opcode modules, with every load command already pointing inside the
# bundle. A build therefore needs no Homebrew, no dylibbundler and no system
# Csound anywhere, and dev and CI compile against the same bytes.
#
# STILL NEVER REQUIRED (CMakeLists' oldest rule): delete third_party/csound, or
# build on a platform that has no payload yet, and everything builds exactly as
# before — T5YNTH_CSOUND_FOUND goes false, CsoundEngine.cpp is not compiled, and
# the always-defined T5YNTH_HAS_CSOUND lets every call site branch at runtime.
#
# T5YNTH_CSOUND_PREFIX (CMake var or environment) overrides the vendored payload
# with an installation of one's own — on macOS its Frameworks/ dir, as Homebrew
# lays it out. Everything downstream follows from what was found, not from the
# prefix.
set(T5YNTH_CSOUND_FOUND FALSE)
set(T5YNTH_CSOUND_VENDORED "")
set(T5YNTH_CSOUND_INCLUDE "")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(_csound_vendor "${CMAKE_SOURCE_DIR}/third_party/csound/macos-arm64")
elseif(WIN32)
    set(_csound_vendor "${CMAKE_SOURCE_DIR}/third_party/csound/windows-x64")
else()
    set(_csound_vendor "")
endif()

# One header set for every platform. Measured 2026-07-26: the 6.18.1 Windows
# release's headers are byte-identical to the macOS ones apart from line endings
# (the only real difference is a typo fix in plugin.h, which is for WRITING
# plugin opcodes and which nothing here includes). Two copies would only be two
# things to keep in step.
set(_csound_include "${CMAKE_SOURCE_DIR}/third_party/csound/include")

if(NOT T5YNTH_CSOUND_PREFIX AND DEFINED ENV{T5YNTH_CSOUND_PREFIX})
    set(T5YNTH_CSOUND_PREFIX "$ENV{T5YNTH_CSOUND_PREFIX}")
endif()

if(_csound_vendor AND NOT T5YNTH_CSOUND_PREFIX
   AND EXISTS "${_csound_include}/csound/csound.h")
    if(APPLE AND EXISTS "${_csound_vendor}/lib/CsoundLib64")
        set(T5YNTH_CSOUND_VENDORED "${_csound_vendor}")
        set(T5YNTH_CSOUND_INCLUDE "${_csound_include}")
        set(T5YNTH_CSOUND_FOUND TRUE)
    elseif(WIN32 AND EXISTS "${_csound_vendor}/lib/csound64.lib")
        set(T5YNTH_CSOUND_VENDORED "${_csound_vendor}")
        set(T5YNTH_CSOUND_INCLUDE "${_csound_include}")
        set(T5YNTH_CSOUND_FOUND TRUE)
    endif()
endif()

if(APPLE AND NOT T5YNTH_CSOUND_FOUND)
    # No payload (or an explicit prefix): the framework, as Homebrew ships it.
    find_library(T5YNTH_CSOUND_FRAMEWORK NAMES CsoundLib64
                 PATHS "${T5YNTH_CSOUND_PREFIX}/Frameworks"
                       /opt/homebrew/opt/csound/Frameworks /Library/Frameworks
                 NO_DEFAULT_PATH)
    if(T5YNTH_CSOUND_FRAMEWORK)
        set(T5YNTH_CSOUND_FOUND TRUE)
    endif()
endif()

if(WIN32 AND NOT T5YNTH_CSOUND_FOUND AND T5YNTH_CSOUND_PREFIX)
    if(EXISTS "${T5YNTH_CSOUND_PREFIX}/lib/csound64.lib")
        set(T5YNTH_CSOUND_VENDORED "${T5YNTH_CSOUND_PREFIX}")
        set(T5YNTH_CSOUND_FOUND TRUE)
        # An installation of one's own brings its own headers if it has them;
        # otherwise the vendored set, which is the same 6.18.1 API.
        if(EXISTS "${T5YNTH_CSOUND_PREFIX}/include/csound/csound.h")
            set(T5YNTH_CSOUND_INCLUDE "${T5YNTH_CSOUND_PREFIX}/include")
        else()
            set(T5YNTH_CSOUND_INCLUDE "${_csound_include}")
        endif()
    endif()
endif()

# Linux does NOT vendor, and the reason is not squeamishness about binaries: the
# app itself is compiled on the machine that packages it, so a Csound built there
# carries exactly the glibc floor akróasys already carries, while a checked-in .so
# would be one binary in the tree nobody could regenerate. Ubuntu 24.04 ships
# 6.18.1 — the same version as both payloads — from the distribution's own archive,
# which is a first-party, licence-audited source and not the unknown-and-opaque
# situation `brew install` would have been on someone's Mac.
#
# It costs exactly one module: Debian and Ubuntu ship no scanned synthesis at all
# (no libscansyn in libcsound64-6.0, and none in the separate csound-plugins
# package either), so scanu/scanu2/scans are reachable on macOS and Windows and not
# here. Nothing in backend/lco_library.json uses them. Declared, not hidden:
# tools/verify_csound_bundle.py knows the difference per platform.
if(UNIX AND NOT APPLE AND NOT T5YNTH_CSOUND_FOUND)
    find_library(T5YNTH_CSOUND_LINUX_LIB NAMES csound64
                 HINTS "${T5YNTH_CSOUND_PREFIX}/lib")
    find_path(T5YNTH_CSOUND_LINUX_INCLUDE csound/csound.h
              HINTS "${T5YNTH_CSOUND_PREFIX}/include")
    if(T5YNTH_CSOUND_LINUX_LIB AND T5YNTH_CSOUND_LINUX_INCLUDE)
        set(T5YNTH_CSOUND_FOUND TRUE)
        set(T5YNTH_CSOUND_INCLUDE "${T5YNTH_CSOUND_LINUX_INCLUDE}")
    endif()
endif()

if(T5YNTH_CSOUND_VENDORED)
    message(STATUS "Csound: vendored payload at ${T5YNTH_CSOUND_VENDORED}")
elseif(T5YNTH_CSOUND_LINUX_LIB)
    message(STATUS "Csound: system library at ${T5YNTH_CSOUND_LINUX_LIB}")
elseif(T5YNTH_CSOUND_FOUND)
    message(STATUS "Csound: system framework at ${T5YNTH_CSOUND_FRAMEWORK}")
else()
    message(STATUS "Csound: none — the LRO will be silent in this build")
endif()

set(T5YNTH_PLUGIN_FORMATS Standalone VST3)

# CLAP is NOT a juce_add_plugin FORMAT — JUCE ships no CLAP client, and the JUCE
# team has never committed to one. The format comes from clap-juce-extensions
# (MIT), which builds a second module out of the SAME shared-code target, so
# there is no second copy of the plugin to keep in step: one processor, one
# editor, one APVTS, one preset format.
#
# It therefore cannot live in T5YNTH_PLUGIN_FORMATS (juce_add_plugin would reject
# the name) and is carried in its own variable instead, appended wherever a
# per-format loop runs — signing, Csound bundling, the charset gate. Empty when
# CLAP is not built, which makes every one of those loops a no-op without an
# `if` of its own.
#
# macOS only, deliberately. On macOS a .clap is a real bundle, so everything the
# VST3 and the AU already do — Contents/libs, @loader_path, the ad-hoc signature,
# the verifier — applies to it unchanged. On Windows and Linux a .clap is a bare
# DLL/.so with no bundle around it, and the Csound payload beside it would have to
# land in a directory shared with every other vendor's plugins; that needs a
# loader change in src/dsp/CsoundEngine.cpp and is a separate piece of work.
set(T5YNTH_CLAP_FORMAT "")
if(APPLE)
    option(T5YNTH_MACOS_STANDALONE_ONLY
        "Build only the macOS standalone app"
        OFF)
    option(T5YNTH_BUILD_CLAP
        "Build the CLAP plug-in (fetches clap-juce-extensions at configure time)"
        ON)

    if(T5YNTH_MACOS_STANDALONE_ONLY)
        set(T5YNTH_PLUGIN_FORMATS Standalone)
    else()
        list(APPEND T5YNTH_PLUGIN_FORMATS AU)
        if(T5YNTH_BUILD_CLAP)
            set(T5YNTH_CLAP_FORMAT CLAP)
        endif()
    endif()
endif()

if(T5YNTH_CLAP_FORMAT)
    # Pinned to a commit, not to a tag: this project's tags are CLAP *protocol*
    # versions from the pre-1.0 era (0.26.0 is from 2022, 111 commits behind), so
    # a tag here would be a downgrade, not a stable point.
    FetchContent_Declare(
        clap_juce_extensions
        GIT_REPOSITORY https://github.com/free-audio/clap-juce-extensions.git
        GIT_TAG        54b3c3268ab6721a7afeef813c9e1ce43a3d0fcd
    )
    FetchContent_MakeAvailable(clap_juce_extensions)
endif()

# The instrument is called akróasys (ASCII: akroasys). T5ynth was its name up to
# and including 2.5.3 — the last version without the language-resonant
# oscillator — and the identity below is a DELIBERATE clean break, taken while it
# is still cheap (BJ, 2026-07-25: "noch kennt kaum jemand diesen Synth").
#
# What each field does, because three of them look interchangeable and are not:
#   PLUGIN_NAME   what the host DISPLAYS. Carries the accent.
#   PRODUCT_NAME  the artefact FILENAME (akroasys.vst3 / .component / .app), so
#                 ASCII — a non-ASCII bundle name is a portability accident
#                 waiting on the first Windows or Linux path.
#   PLUGIN_CODE   the four-character IDENTITY a saved DAW project matches on.
#                 Changing it away from T5yn is what makes this a break: a
#                 project that had loaded T5ynth no longer finds the plugin.
# PLUGIN_MANUFACTURER_CODE stays A4Ae — it identifies the lab, not the product.
#
# The CMake TARGET is still T5ynth, so `build_clean/T5ynth_artefacts/…` and every
# script pointing at it keep working. Renaming the target, the source files and
# the symbols is a separate mechanical pass; doing it here would mean one commit
# that changes identity AND moves every path.
juce_add_plugin(T5ynth
    COMPANY_NAME "UCDCAE AI Lab"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    PLUGIN_MANUFACTURER_CODE A4Ae
    PLUGIN_CODE Akro
    FORMATS ${T5YNTH_PLUGIN_FORMATS}
    PLUGIN_NAME "akróasys"
    PRODUCT_NAME "akroasys"
    BUNDLE_ID "org.ai4artsed.akroasys"
    DESCRIPTION "Text-to-Sound Synthesizer — embedding-space navigation and a language-resonant oscillator"
    ICON_BIG "${CMAKE_SOURCE_DIR}/resources/logos/t5ynth_icon.png"
    ICON_SMALL "${CMAKE_SOURCE_DIR}/resources/logos/t5ynth_icon.png"
    COPY_PLUGIN_AFTER_BUILD FALSE
    BLUETOOTH_PERMISSION_ENABLED TRUE
    BLUETOOTH_PERMISSION_TEXT "akróasys uses Bluetooth to connect to MIDI controllers."
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "akróasys captures live audio input to seed Resynth (init_audio)."
    # JUCE would add the moduleinfo.json step the moment the VST3 target exists,
    # which is BEFORE the Csound bundling below. The helper LOADS the built module
    # to write that file, and on macOS the module names
    # @loader_path/../libs/CsoundLib64 — which the bundling has not put there yet, so
    # the load fails with "The bundle 'akroasys' couldn't be loaded" and the build
    # stops at 100%. Local builds hid it: the previous build's Contents/libs was
    # still lying there. The step is re-enabled by hand at the end of this file,
    # after every platform's Csound has been put beside the module.
    VST3_AUTO_MANIFEST FALSE
)

# ── the CLAP module, built from the same shared code ──────────────────────────
# Must come after juce_add_plugin: the helper reads PRODUCT_NAME, VERSION and
# COPY_PLUGIN_AFTER_BUILD off the target it is given. It creates T5ynth_CLAP,
# a MODULE bundle at T5ynth_artefacts/<config>/CLAP/akroasys.clap — the same
# layout every other format lands in.
#
# CLAP_ID is the identity a saved project matches on, the same job PLUGIN_CODE
# does for VST3/AU, and it is the bundle identifier as well. It matches BUNDLE_ID
# above, which all three existing formats already share.
#
# CLAP_USE_JUCE_PARAMETER_RANGES ALL: without it the wrapper hands the host
# normalised 0..1 values while JUCE's own formats expose the parameter's real
# range, so the same automation lane would read differently in a CLAP host than
# in the VST3. ALL keeps one meaning per parameter across all four formats.
if(T5YNTH_CLAP_FORMAT)
    clap_juce_extensions_plugin(TARGET T5ynth
        CLAP_ID "org.ai4artsed.akroasys"
        CLAP_FEATURES instrument synthesizer stereo
        CLAP_USE_JUCE_PARAMETER_RANGES ALL
        CLAP_MANUAL_URL "https://github.com/joeriben/akroasys"
        CLAP_SUPPORT_URL "https://github.com/joeriben/akroasys/issues")
endif()

# ── macOS: ad-hoc sign VST3 / AU bundles after JUCE's build steps ──
# Two separate JUCE 8.0.6 issues manifest as "plugin does not show up as
# instrument in Bitwig/Ableton":
#
#   1. VST3: JUCE signs the bundle *before* juce_vst3_helper generates
#      Contents/Resources/moduleinfo.json. That invalidates the sealed
#      signature ("a sealed resource is missing or invalid") and DAWs on
#      modern macOS refuse to register the plugin (it appears in scan/debug
#      lists but not in the instrument browser).
#
#   2. AU: JUCE 8.0.6 does not call _juce_adhoc_sign for the AU target at
#      all (only VST3 and LV2 get it). The linker embeds a minimal ad-hoc
#      Mach-O signature that expects a sealed _CodeSignature folder, which
#      never gets created — resulting in "code has no resources but signature
#      indicates they must be present".
#
# Fix for both: re-sign ad-hoc after all JUCE POST_BUILD steps have run.
# POST_BUILD commands run in the order they are added to a target, so this
# runs *after* JUCE's own manifest / sign steps. The VST3's re-sign is not here but
# at the end of the file, because its manifest step is now added there too.
if(APPLE AND TARGET T5ynth_AU)
    add_custom_command(TARGET T5ynth_AU POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "ad-hoc signing AU component bundle"
        COMMAND codesign --force --sign - --deep
            "$<TARGET_BUNDLE_DIR:T5ynth_AU>"
        VERBATIM)
endif()

# The CLAP has issue 2 for a simpler reason: clap-juce-extensions signs nothing at
# all, by design — it only builds the module. So the same linker-embedded ad-hoc
# signature is left expecting a sealed _CodeSignature that no one created, and the
# same "code has no resources but signature indicates they must be present"
# follows. This POST_BUILD runs after the helper's own steps (PkgInfo and the
# bundle icon), so there is nothing left to seal over.
if(APPLE AND TARGET T5ynth_CLAP)
    add_custom_command(TARGET T5ynth_CLAP POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "ad-hoc signing CLAP bundle"
        COMMAND codesign --force --sign - --deep
            "$<TARGET_BUNDLE_DIR:T5ynth_CLAP>"
        VERBATIM)
endif()

# Standalone has issue 2 as well, and it is the one a developer hits first:
# the .app that comes out of a local build fails `codesign --verify` with the
# same "code has no resources but signature indicates they must be present",
# and LaunchServices then refuses to open it. Running the Mach-O directly still
# works, which makes it read as "the app won't start" with no error anywhere —
# the CI already re-signs the .app for exactly this reason
# (.github/workflows/build.yml), so only the local build was left out.
if(APPLE AND TARGET T5ynth_Standalone)
    add_custom_command(TARGET T5ynth_Standalone POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "ad-hoc signing Standalone app bundle"
        COMMAND codesign --force --sign - --deep
            "$<TARGET_BUNDLE_DIR:T5ynth_Standalone>"
        VERBATIM)
endif()

# ── macOS: make each bundle self-contained w.r.t. Csound ──────────────────
# Without this the LRO is silent on every machine that does not have Csound
# installed — which is every machine but this one, and the reason the published
# build compiles green and makes no sound (docs/plans/HANDOVER_LCO.md §8.1).
# tools/bundle_csound_macos.sh copies CsoundLib64, its plugin opcodes and its
# dependency tree into Contents/libs, rewrites every load command to
# @loader_path, ships the LGPL texts, and re-signs — for the .app, the .vst3 AND
# the .component, because all three link the library and all three need the copy.
#
# Runs LAST on each target: POST_BUILD commands execute in the order they are
# added, so this follows JUCE's own signing steps and the re-signs above, and
# re-seals the bundle itself afterwards.
#
# Never required, exactly like the library itself: no Csound -> no bundling step,
# and a machine with Csound but without dylibbundler builds as it did before,
# linking the system framework directly. The bundling is what an INSTALLER needs;
# a dev build that only ever runs on this machine works without it.
option(T5YNTH_BUNDLE_CSOUND
    "Ship Csound with the built artefacts so they need no system Csound"
    ON)
if(APPLE AND T5YNTH_CSOUND_VENDORED AND T5YNTH_BUNDLE_CSOUND)
    # Vendored: the payload is already linked for this layout, so bundling is a
    # copy plus signatures. Not optional here and deliberately so — the binary
    # names @loader_path/../libs/CsoundLib64, and a bundle without it would not
    # launch at all. No dylibbundler, no Homebrew, nothing to install.
    foreach(_fmt Standalone VST3 AU ${T5YNTH_CLAP_FORMAT})
        if(TARGET T5ynth_${_fmt})
            add_custom_command(TARGET T5ynth_${_fmt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E env
                    "CSOUND_VENDORED_LIBS=${T5YNTH_CSOUND_VENDORED}/lib"
                    bash "${CMAKE_SOURCE_DIR}/tools/bundle_csound_macos.sh"
                    "$<TARGET_BUNDLE_DIR:T5ynth_${_fmt}>"
                VERBATIM)
        endif()
    endforeach()
elseif(APPLE AND T5YNTH_CSOUND_FOUND AND T5YNTH_BUNDLE_CSOUND)
    # A system framework instead of the payload: the old route, which has to
    # flatten the framework and pull its dependency tree in with dylibbundler.
    find_program(T5YNTH_DYLIBBUNDLER dylibbundler)
    if(NOT T5YNTH_DYLIBBUNDLER)
        message(WARNING
            "Csound found but dylibbundler is not installed — the bundles will "
            "keep linking ${T5YNTH_CSOUND_FRAMEWORK} and will only run on a "
            "machine that has it. `brew install dylibbundler` to bundle Csound.")
    else()
        foreach(_fmt Standalone VST3 AU ${T5YNTH_CLAP_FORMAT})
            if(TARGET T5ynth_${_fmt})
                add_custom_command(TARGET T5ynth_${_fmt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E env
                        "CSOUND_FRAMEWORK_BINARY=${T5YNTH_CSOUND_FRAMEWORK}/CsoundLib64"
                        bash "${CMAKE_SOURCE_DIR}/tools/bundle_csound_macos.sh"
                        "$<TARGET_BUNDLE_DIR:T5ynth_${_fmt}>"
                    VERBATIM)
            endif()
        endforeach()
    endif()
endif()

# Linux: the tarball artefacts get a libs/ dir beside the binary and an $ORIGIN
# rpath, so they run on a machine with no Csound installed. The .deb does not go
# through here — it ships the bare executable and depends on libcsound64-6.0, which
# is what a distribution package is for.
if(UNIX AND NOT APPLE AND T5YNTH_CSOUND_LINUX_LIB AND T5YNTH_BUNDLE_CSOUND)
    find_program(T5YNTH_PATCHELF patchelf)
    if(NOT T5YNTH_PATCHELF)
        message(WARNING
            "Csound found but patchelf is not installed — the artefacts will keep "
            "linking ${T5YNTH_CSOUND_LINUX_LIB} and will only run on a machine that "
            "has it. `apt install patchelf` to bundle Csound.")
    else()
        foreach(_fmt Standalone VST3)
            if(TARGET T5ynth_${_fmt})
                add_custom_command(TARGET T5ynth_${_fmt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E env
                        "CSOUND_LIBRARY=${T5YNTH_CSOUND_LINUX_LIB}"
                        bash "${CMAKE_SOURCE_DIR}/tools/bundle_csound_linux.sh"
                             "$<TARGET_FILE:T5ynth_${_fmt}>"
                    VERBATIM)
            endif()
        endforeach()
    endif()
endif()

# Windows: the DLL and its plugin opcodes go next to the module that loads them —
# beside the .exe for the standalone, inside Contents/x86_64-win for the VST3.
# src/dsp/CsoundEngine.cpp loads csound64.dll from exactly there, by absolute
# path, and then points Csound at the plugins64 dir beside it. The LGPL notice
# travels with them, as it does inside the macOS bundles.
if(WIN32 AND T5YNTH_CSOUND_VENDORED AND T5YNTH_BUNDLE_CSOUND)
    foreach(_fmt Standalone VST3)
        if(TARGET T5ynth_${_fmt})
            add_custom_command(TARGET T5ynth_${_fmt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${T5YNTH_CSOUND_VENDORED}/bin"
                    "$<TARGET_FILE_DIR:T5ynth_${_fmt}>"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/resources/licenses/csound"
                    "$<TARGET_FILE_DIR:T5ynth_${_fmt}>/licenses/csound"
                VERBATIM)
        endif()
    endforeach()
endif()

# ── the VST3 manifest step, now that Csound is beside the module ──────────────
# Turned off in juce_add_plugin above and re-enabled here, so that it is the LAST
# POST_BUILD step on the VST3 target: the helper LOADS the module, and until each
# platform's block above has put Csound next to it there is nothing to load. The
# order that results is JUCE's own sign, the Csound bundling (which re-signs), the
# manifest, and then the re-sign below — because writing moduleinfo.json into a
# signed bundle breaks the seal, which is issue 1 at the top of this section.
if(TARGET T5ynth_VST3)
    juce_enable_vst3_manifest_step(T5ynth)
    if(APPLE)
        add_custom_command(TARGET T5ynth_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo
                "re-signing VST3 bundle after moduleinfo.json generation"
            COMMAND codesign --force --sign - --deep
                "$<TARGET_BUNDLE_DIR:T5ynth_VST3>"
            VERBATIM)
    endif()
endif()

# The standalone brings its own JUCEApplication (src/StandaloneAppLauncher.cpp)
# so that launching does not open an audio INPUT device — JUCE's default app
# does, before any window exists, and a stored setup naming a different input
# than output device makes that an aggregate-device registration that can block
# on coreaudiod with no window, no crash and no log. Resynth ext opens the input
# on demand instead; the measurement and the mechanism are in
# src/StandaloneAudioInput.h. Standalone-only: the plugin formats are untouched,
# and so is the processor's stereo input bus.
if(TARGET T5ynth_Standalone)
    target_sources(T5ynth_Standalone PRIVATE src/StandaloneAppLauncher.cpp)
    target_compile_definitions(T5ynth_Standalone PRIVATE
        JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
endif()

juce_generate_juce_header(T5ynth)

# ── Charset gate ─────────────────────────────────────────────────────────────
# `juce::String` reads a plain `const char*` one byte per character -- Latin-1
# (juce_CharPointer_ASCII.h:108). So a source file saved as UTF-8, which is every
# source file, hands it three bytes for one em dash and gets three characters
# back: a stray "a-circumflex" plus two invisible controls where the dash was.
# JUCE guards that with a `jassert`, and this project builds Release, where
# `jassert` is nothing. The corruption is therefore invisible to the compiler,
# invisible to the tests, and visible only in the shipped menus -- which is how
# it reached the user repeatedly, most recently 2026-08-01.
#
# So it is checked here instead, before anything is compiled: every literal above
# ASCII must state how its bytes are to be read, at the literal. Not advisory --
# a violation fails the build, because every softer form of this rule has already
# been tried and walked past. It is not hand-work either: the failure prints
# `source_charset_gate.py --fix`, which does the repair (escape the bytes, wrap
# in fromUTF8) and leaves only the literals whose intent a script cannot know.
#
# Python is not optional for this project (the whole inference backend is Python),
# so a missing interpreter is a broken toolchain, not a reason to skip the check.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_target(t5ynth_charset_gate ALL
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/source_charset_gate.py"
            "${CMAKE_SOURCE_DIR}/src"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Charset gate: every non-ASCII literal in src/ states its encoding"
    VERBATIM)
# `T5ynth` is the target that actually compiles src/ -- juce_add_plugin renames
# only its OUTPUT to libakroasys_SharedCode.a, so a target called
# T5ynth_SharedCode does not exist. Naming one that does not exist is the way
# this kind of edge goes missing without a word, so an unresolvable name is a
# hard configure error here rather than a dropped dependency: a gate that is
# still in the file but no longer attached is worse than no gate, because it
# reads as protection.
foreach(_t T5ynth ${T5YNTH_PLUGIN_FORMATS} ${T5YNTH_CLAP_FORMAT})
    if(NOT TARGET ${_t})
        set(_t T5ynth_${_t})
    endif()
    if(NOT TARGET ${_t})
        message(FATAL_ERROR "charset gate: nothing named ${_t} to attach to")
    endif()
    add_dependencies(${_t} t5ynth_charset_gate)
endforeach()

target_sources(T5ynth PRIVATE
    # Core
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/UpdateChecker.cpp

    # DSP
    src/dsp/DcoBaker.cpp
    src/dsp/WavetableOscillator.cpp
    src/dsp/WavetableBank.cpp
    src/dsp/NoiseGenerator.cpp
    src/dsp/ADSREnvelope.cpp
    src/dsp/LFO.cpp
    src/dsp/DriftLFO.cpp
    src/dsp/StateVariableFilter.cpp
    src/dsp/DelayLine.cpp
    src/dsp/ConvolutionReverb.cpp
    src/dsp/AlgorithmicReverb.cpp
    src/dsp/EarlyReflections.cpp
    src/dsp/Limiter.cpp
    src/dsp/AmpEffects.cpp
    src/dsp/SamplePlayer.cpp
    src/dsp/FreezeTextureEngine.cpp
    src/dsp/SynthVoice.cpp
    src/dsp/VoiceManager.cpp
    src/dsp/ModulationMatrix.cpp

    # Sequencer
    src/sequencer/StepSequencer.cpp
    src/sequencer/Arpeggiator.cpp
    src/sequencer/GenerativeSequencer.cpp
    src/sequencer/EuclideanRhythm.h
    src/sequencer/ScaleQuantizer.h
    src/sequencer/GenerativeSequencer.h

    # Inference (Python pipe IPC)
    src/inference/PipeInference.cpp
    src/inference/RepromptStances.cpp

    # Event Log (.t5evt session log)
    src/eventlog/EventLog.cpp
    src/eventlog/EventLogWriterThread.cpp
    src/eventlog/EventLogReader.cpp

    # GUI
    src/gui/MainPanel.cpp
    src/gui/PhysicalKeyState.cpp
    src/gui/PromptPanel.cpp
    src/gui/SynthPanel.cpp
    src/gui/FxPanel.cpp
    src/gui/SequencerPanel.cpp
    src/gui/AxesPanel.cpp
    src/gui/DimensionExplorer.cpp
    src/gui/WaveformDisplay.cpp
    src/gui/SetupWizard.cpp
    src/gui/MpeSettingsPage.cpp
    src/gui/StatusBar.cpp
    src/gui/ReplayOverlay.cpp
    src/gui/T5ynthLookAndFeel.cpp

    # Presets
    src/presets/PresetFormat.cpp
    src/presets/PresetUpdater.cpp
)

# The typing keyboard's physical key map is fed by real AppKit key events on
# macOS, so its implementation is Objective-C++ (PhysicalKeyState.cpp compiles to
# nothing there and carries the other platforms' fallback).
if(APPLE)
    target_sources(T5ynth PRIVATE src/gui/PhysicalKeyStateMac.mm)
endif()

# target_sources/link additions must stay inside this if(): CsoundEngine.cpp
# #includes <csound/csound.h> at file scope and cannot even preprocess without
# the framework's headers on the include path.
if(T5YNTH_CSOUND_VENDORED)
    target_sources(T5ynth PRIVATE src/dsp/CsoundEngine.cpp)
    target_include_directories(T5ynth PRIVATE "${T5YNTH_CSOUND_INCLUDE}")
    if(APPLE)
        # The vendored library's own install name is @loader_path/../libs/CsoundLib64,
        # so linking it records exactly the load command the bundled layout needs —
        # no install_name_tool, no rpath, nothing machine-specific in the binary.
        target_link_libraries(T5ynth PRIVATE "${T5YNTH_CSOUND_VENDORED}/lib/CsoundLib64")
    else()
        target_link_libraries(T5ynth PRIVATE "${T5YNTH_CSOUND_VENDORED}/lib/csound64.lib")
        # Windows resolves imports relative to the HOST process, not to the module
        # that declares them, so a VST3 with a normal import of csound64.dll would
        # be looked for beside the DAW's exe and fail to load in most hosts.
        # Delay-loading postpones that to the first Csound call, by which time
        # CsoundEngine.cpp has loaded the DLL from its own directory by absolute
        # path. It is also what makes a MISSING DLL survivable: without it the
        # loader kills the process at startup; with it, the first call is where we
        # notice, and the LRO simply stays silent.
        # INTERFACE, not PRIVATE: T5ynth is the shared-code static library, and
        # these have to reach the real link of every format target.
        if(MSVC)
            target_link_options(T5ynth INTERFACE "/DELAYLOAD:csound64.dll")
            target_link_libraries(T5ynth INTERFACE delayimp)
        endif()
    endif()
elseif(T5YNTH_CSOUND_LINUX_LIB)
    # Linked by SONAME, so ONE binary serves both distributions: the tarball finds
    # the bundled copy through the $ORIGIN/libs rpath below, and the .deb — which
    # ships the bare executable and depends on libcsound64-6.0 — finds the
    # distribution's own, because an rpath that resolves to nothing simply falls
    # through to the default search. The opcode directory follows from the same
    # rule as everywhere: a sibling Opcodes64 next to the library actually loaded.
    # The bundled copy has one; /usr/lib does not, so there Csound's own baked-in
    # plugin path is left alone — which is the correct answer for a distro install.
    target_sources(T5ynth PRIVATE src/dsp/CsoundEngine.cpp)
    target_include_directories(T5ynth PRIVATE "${T5YNTH_CSOUND_INCLUDE}")
    target_link_libraries(T5ynth PRIVATE "${T5YNTH_CSOUND_LINUX_LIB}")
elseif(T5YNTH_CSOUND_FOUND)
    target_sources(T5ynth PRIVATE src/dsp/CsoundEngine.cpp)
    get_filename_component(_csound_fw_dir "${T5YNTH_CSOUND_FRAMEWORK}" DIRECTORY)
    # Homebrew's opt-prefix ships an "include/csound" symlink pointing into the
    # framework's own Headers/ dir (verified on this machine:
    # /opt/homebrew/opt/csound/include/csound -> ../Frameworks/CsoundLib64.framework/Headers).
    # That's the SAME include path tools/csound_poc.cpp's proven build recipe
    # uses ($CSOUND_PREFIX/include) and is what makes `#include <csound/csound.h>`
    # resolve. _csound_fw_dir is ".../Frameworks"; its parent is the Homebrew prefix.
    # A /Library/Frameworks install has no such prefix layout -- there the
    # framework's own Headers/ dir provides csound.h, so map <csound/...> onto it.
    get_filename_component(_csound_prefix "${_csound_fw_dir}" DIRECTORY)
    if(EXISTS "${_csound_prefix}/include/csound")
        target_include_directories(T5ynth PRIVATE "${_csound_prefix}/include")
    else()
        # Recreate the same shim in the build tree: <builddir>/csound_include/csound -> Headers
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/csound_include")
        file(CREATE_LINK "${T5YNTH_CSOUND_FRAMEWORK}/Headers"
             "${CMAKE_BINARY_DIR}/csound_include/csound" SYMBOLIC)
        target_include_directories(T5ynth PRIVATE "${CMAKE_BINARY_DIR}/csound_include")
    endif()
    target_link_libraries(T5ynth PRIVATE "${T5YNTH_CSOUND_FRAMEWORK}")
    target_link_options(T5ynth PRIVATE "-Wl,-rpath,${_csound_fw_dir}")
endif()

target_include_directories(T5ynth PRIVATE
    src
)
target_include_directories(T5ynth SYSTEM PRIVATE
    ${signalsmith_stretch_SOURCE_DIR}
)

juce_add_binary_data(T5ynthData SOURCES
    resources/ir/emt_140_plate_bright.wav
    resources/ir/emt_140_plate_medium.wav
    resources/ir/emt_140_plate_dark.wav
    resources/logos/t5ynth_icon.png
    resources/akroasys_Guide.html
    # SA3 metadata that the ungated Comfy-Org weight repos omit (~60 KB of JSON,
    # no model weights). Written into the model dir at install time by the
    # reassembly finalize step so SA3 can install login-free. model_config.json
    # is the Stability SA3 config for the SMALL checkpoints (music+sfx — modality
    # comes from the dir name, not the config); the four sa3_t5gemma_* files are
    # the Gemma tokenizer/encoder configs (shared by ALL SA3 tiers). The 34 MB
    # tokenizer.json is NOT bundled — the SentencePiece model is extracted from
    # the downloaded t5gemma weights.
    resources/sa3_configs/sa3_model_config.json
    # SA3 Medium DiT config (depth-24 differential-attn DiT + SAME-L autoencoder
    # with sliding_window attention). DISTINCT from the small config above; the
    # medium reassembly writes THIS one (finalizeSa3Reassembly branches on id).
    resources/sa3_configs/sa3_medium_model_config.json
    resources/sa3_configs/sa3_t5gemma_config.json
    resources/sa3_configs/sa3_t5gemma_generation_config.json
    resources/sa3_configs/sa3_t5gemma_tokenizer_config.json
    resources/sa3_configs/sa3_t5gemma_special_tokens_map.json
    # Stable Audio Open 1.0 metadata omitted by the ungated Comfy-Org repackaged
    # repo (which ships only the checkpoint). model_config.json is the canonical
    # Stability SAO 1.0 config (verified byte-identical across three independent
    # ungated mirrors); written into the model dir at install by the SAO reassembly
    # finalize step so SAO 1.0 can install login-free. No separate text encoder is
    # bundled here — SAO 1.0 conditions on t5-base, already available ungated.
    resources/sao_configs/sao_open_1_0_model_config.json
    # Verbatim third-party license texts written into each SA3 install dir. The
    # Stability AI Community License IV(a)(i) requires distributing a copy of the
    # Agreement; the Gemma Terms + Prohibited Use Policy are the encoder's terms.
    resources/sa3_configs/licenses/STABILITY_AI_COMMUNITY_LICENSE.txt
    resources/sa3_configs/licenses/GEMMA_TERMS_OF_USE.txt
    resources/sa3_configs/licenses/GEMMA_PROHIBITED_USE_POLICY.txt
)

target_link_libraries(T5ynth
    PRIVATE
        T5ynthData
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_dsp
        juce::juce_gui_extra
        $<$<BOOL:${CURL_FOUND}>:CURL::libcurl>
        nlohmann_json::nlohmann_json
        # JUCE 8's juce_gui_extra module header does not declare a
        # linuxPackages: line, so its Linux WebBrowserComponent deps
        # (webkit2gtk + gtk3) are not auto-linked. JUCEUtils.cmake creates
        # a JUCE_BROWSER_LINUX_DEPS pkgconfig target but leaves linkage
        # to the user. Without this explicit link, Linux builds fail with
        # 'gtk/gtk.h: No such file or directory' in juce_gui_extra.cpp.
        $<$<BOOL:${UNIX}>:$<$<NOT:$<BOOL:${APPLE}>>:juce::pkgconfig_JUCE_BROWSER_LINUX_DEPS>>
    PUBLIC
        juce::juce_recommended_config_flags
        # juce::juce_recommended_lto_flags  # disabled — too CPU-intensive for dev builds
        juce::juce_recommended_warning_flags
)

target_compile_definitions(T5ynth
    PUBLIC
        NOMINMAX              # prevent windows.h min/max macros
        JUCE_WEB_BROWSER=1
        JUCE_USE_CURL=$<BOOL:${CURL_FOUND}>
        T5YNTH_HAS_CSOUND=$<BOOL:${T5YNTH_CSOUND_FOUND}>
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
    PRIVATE
        T5YNTH_BACKEND_DIR="${CMAKE_SOURCE_DIR}/backend"
        T5YNTH_FULL_VERSION="${_T5YNTH_FULL_VERSION}"
)
