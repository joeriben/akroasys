#!/usr/bin/env bash
# Make a Linux artefact self-contained w.r.t. Csound.
#
#   CSOUND_LIBRARY=/usr/lib/x86_64-linux-gnu/libcsound64.so tools/bundle_csound_linux.sh <binary>
#
# Copies the Csound library, the plugin opcode modules and every non-system
# library they need into a `libs/` directory beside the binary, and points the
# binary at it with an $ORIGIN rpath. Nothing is modified: each file is the
# distribution's own build, with only its recorded search path rewritten so it
# resolves inside the artefact instead of in /usr/lib.
#
# The plugin modules land in libs/Opcodes64 — the same sibling-of-the-library
# layout macOS uses, which is what src/dsp/CsoundEngine.cpp looks for. A machine
# without a bundled copy (the .deb, which depends on libcsound64-6.0 instead) has
# no such sibling directory, so Csound's own resolution is left alone there.
#
# The linked binary keeps its SONAME reference either way, so ONE build serves
# both: an rpath that resolves to nothing falls through to the default search.
set -euo pipefail

BIN="${1:?usage: bundle_csound_linux.sh <binary>}"
[[ -f "$BIN" ]] || { echo "no such binary: $BIN" >&2; exit 1; }
BIN="$(readlink -f "$BIN")"
LIBS="$(dirname "$BIN")/libs"
OPCODES="$LIBS/Opcodes64"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LICENSE_SRC="${CSOUND_LICENSE_DIR:-$REPO_ROOT/resources/licenses/csound}"

CSOUND_LIBRARY="${CSOUND_LIBRARY:-}"
if [[ -z "$CSOUND_LIBRARY" ]]; then
    echo "CSOUND_LIBRARY is not set — nothing to bundle" >&2
    exit 1
fi
CSOUND_LIBRARY="$(readlink -f "$CSOUND_LIBRARY")"

command -v patchelf >/dev/null || { echo "patchelf is not installed" >&2; exit 1; }

# Never copied: the core runtime every ELF on the system shares. Bundling these is
# how a "self-contained" build becomes one that segfaults on a machine whose kernel,
# loader or C++ runtime is newer than the one it was built against. The list is
# every glibc library that can appear as a NEEDED entry plus the GCC runtime, not
# only the ones a given Csound happens to pull: measured on Fedora 42, a
# libcsound64 built with GCC 15 NEEDs
# libmvec.so.1 directly and libresolv.so.2 arrived through curl → krb5; both are
# glibc, and both had been copied into libs/.
is_system_lib() {
    case "$1" in
        libc.so.*|libm.so.*|libmvec.so.*|libdl.so.*|libpthread.so.*|librt.so.*|\
        libresolv.so.*|libutil.so.*|libanl.so.*|libnss_*.so.*|libBrokenLocale.so.*|\
        libthread_db.so.*|ld-linux*|\
        libgcc_s.so.*|libstdc++.so.*|libatomic.so.*|linux-vdso.so.*)  return 0 ;;
        *) return 1 ;;
    esac
}

# Everything a file needs, resolved the way the loader would. `ldd` rather than
# readelf -d: DT_NEEDED gives SONAMEs, and we need the paths they actually resolve
# to on this machine.
needed_paths() {
    ldd "$1" 2>/dev/null | awk '/=>/ && $3 ~ /^\// { print $3 }'
}

copy_with_deps() {
    local src="$1" dest_dir="$2" name
    name="$(basename "$src")"
    [[ -f "$dest_dir/$name" ]] && return 0
    install -m 0755 "$src" "$dest_dir/$name"
    local dep
    while IFS= read -r dep; do
        is_system_lib "$(basename "$dep")" && continue
        copy_with_deps "$dep" "$LIBS"
    done < <(needed_paths "$src")
}

rm -rf "$LIBS"
mkdir -p "$LIBS" "$OPCODES"

echo "bundling Csound (system library $CSOUND_LIBRARY)"
copy_with_deps "$CSOUND_LIBRARY" "$LIBS"

# The plugin opcode modules, from wherever this distribution keeps them. Csound
# without them drops from 2267 opcode entries to 1917 — no fractalnoise, no
# MixerSend, no GEN padsynth — and the author WRITES Csound, so an opcode that is
# not there is a capability it cannot reach.
PLUGIN_DIR=""
for candidate in "$(dirname "$CSOUND_LIBRARY")"/csound/plugins64-* \
                 /usr/lib/*/csound/plugins64-* /usr/lib/csound/plugins64-* \
                 /usr/local/lib/csound/plugins64-*; do
    [[ -d "$candidate" ]] && { PLUGIN_DIR="$candidate"; break; }
done

SKIPPED=""
if [[ -z "$PLUGIN_DIR" ]]; then
    echo "  WARNING: no plugin opcode directory found — the LRO will have core opcodes only" >&2
else
    for module in "$PLUGIN_DIR"/*.so; do
        [[ -f "$module" ]] || continue
        # Only modules that need nothing beyond what is already here or the system:
        # rtjack, rtpulse, osc and friends drag in a server or a network library
        # that has no business inside an instrument, exactly as on the other two
        # platforms. Missing deps also make a module fail to load at runtime, which
        # Csound reports only when an orchestra tries to compile its opcodes.
        wants_extra=""
        while IFS= read -r dep; do
            dep_name="$(basename "$dep")"
            is_system_lib "$dep_name" && continue
            [[ -f "$LIBS/$dep_name" ]] && continue
            wants_extra="$dep_name"
            break
        done < <(needed_paths "$module")
        if [[ -n "$wants_extra" ]] || ldd "$module" 2>/dev/null | grep -q "not found"; then
            SKIPPED="$SKIPPED$(basename "$module") (${wants_extra:-unresolved})"$'\n'
            continue
        fi
        install -m 0755 "$module" "$OPCODES/$(basename "$module")"
    done
fi

# $ORIGIN, not an absolute path: the artefact is unpacked wherever the user likes.
patchelf --set-rpath '$ORIGIN/libs' "$BIN"
for lib in "$LIBS"/*.so*; do
    [[ -f "$lib" ]] && patchelf --set-rpath '$ORIGIN' "$lib"
done
for module in "$OPCODES"/*.so; do
    [[ -f "$module" ]] && patchelf --set-rpath '$ORIGIN/..' "$module"
done

mkdir -p "$(dirname "$BIN")/licenses/csound"
cp -f "$LICENSE_SRC"/*.txt "$(dirname "$BIN")/licenses/csound/"

# The check that matters: the CSOUND payload must resolve inside the artefact. A
# single survivor means the LRO is silent on a machine without Csound, which is the
# whole case this exists for.
#
# The scope is per file, and the difference is not cosmetic. For the files this
# script copied, "outside the artefact" is always a failure: copy_with_deps pulled
# in every non-system dependency recursively, so anything still pointing at /usr/lib
# is a copy that did not happen or an rpath that did not take. The BINARY is not one
# of those files — it is the application, and it legitimately links the
# distribution's ALSA, fontconfig, freetype and curl, none of which this script
# bundles or should bundle. Holding it to the payload's rule failed the build on
# every one of those (measured on CI 2026-07-30: 36 flagged, not one of them
# Csound's). What it owes is narrower and is asserted positively below: the Csound
# library, and anything else the payload put in libs/, must come from libs/.
problems=""
csound_soname="$(basename "$CSOUND_LIBRARY")"
for f in "$BIN" "$LIBS"/*.so* "$OPCODES"/*.so; do
    [[ -f "$f" ]] || continue
    while IFS= read -r line; do
        case "$line" in
            *"not found"*) problems="$problems  $(basename "$f"): $line"$'\n' ;;
        esac
    done < <(ldd "$f" 2>/dev/null)
    while IFS= read -r dep; do
        dep_name="$(basename "$dep")"
        is_system_lib "$dep_name" && continue
        case "$dep" in
            "$LIBS"/*|"$OPCODES"/*) continue ;;
        esac
        # For the binary, only the payload's own libraries are this gate's business.
        if [[ "$f" == "$BIN" && ! -f "$LIBS/$dep_name" ]]; then continue; fi
        problems="$problems  $(basename "$f"): still resolves $dep"$'\n'
    done < <(needed_paths "$f")
done

# ...and the point of the whole exercise, stated as a requirement rather than left
# to fall out of the loop above: the binary reaches Csound through libs/.
# A here-string and not a pipe: `grep -q` exits on the first match, and with enough
# buffered output still to come the awk in needed_paths dies on SIGPIPE, which
# `pipefail` turns into a failed pipeline and `!` turns into a false alarm about a
# correct artefact. Measured on mawk 1.3.4 / Ubuntu 24.04: it starts firing above
# ~35 kB of output, i.e. around 950 resolved dependencies against today's 47.
if ! grep -qxF "$LIBS/$csound_soname" <<<"$(needed_paths "$BIN")"; then
    problems="$problems  $(basename "$BIN"): does not resolve $csound_soname inside $LIBS"$'\n'
fi
if [[ -n "$problems" ]]; then
    printf 'Csound bundling incomplete:\n%s' "$problems" >&2
    exit 1
fi

if [[ -n "$SKIPPED" ]]; then
    printf '  skipped (needs a library of its own): %s' "$SKIPPED" | tr '\n' ' '
    echo
fi
echo "  $(basename "$BIN"): $(find "$LIBS" -maxdepth 1 -name '*.so*' | wc -l | tr -d ' ') libs, \
$(find "$OPCODES" -name '*.so' | wc -l | tr -d ' ') plugin opcode modules"
