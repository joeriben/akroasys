#!/usr/bin/env bash
# Run tools/audition_loop_seams over a directory of WAVs with a settings table,
# print one RESULT line per case and a summary. The settings table is a text
# file with one case per line:  <wav-basename> <loop|pingpong> <opt> <xfMs> <ls> <le> [p1] [normalize]
# (p1 = the preset's start position, default = ls; normalize 0/1, default 0)
#
#   tools/measure_loop_seams.sh <tool-binary> <wav-dir> <cases.txt> [ratio] [out-dir]
set -euo pipefail
TOOL="${1:?tool binary}"; DIR="${2:?wav dir}"; CASES="${3:?cases file}"
RATIO="${4:-1.0}"; OUT="${5:-}"
while read -r name mode opt xf ls le p1 norm; do
    [ -z "$name" ] && continue
    case "$name" in \#*) continue;; esac
    f="$DIR/$name.wav"; [ -f "$f" ] || { echo "missing $f" >&2; continue; }
    o="-"; [ -n "$OUT" ] && { mkdir -p "$OUT"; o="$OUT/${name}_${mode}_opt${opt}_xf${xf}_r${RATIO}.wav"; }
    "$TOOL" "$f" "$mode" "$opt" "$xf" "$ls" "$le" "$RATIO" 0 "$o" "${p1:-$ls}" "${norm:-0}"
done < "$CASES" | awk '
/^RESULT/ { print; n++; for (i=1;i<=NF;i++) { split($i,kv,"="); v[kv[1]]=kv[2] }
            sm+=v["step_max"]; hm+=v["hf_max_dB"]; if (v["step_max"]>ws) ws=v["step_max"]; if (v["hf_max_dB"]>wh) wh=v["hf_max_dB"] }
END { if (n) printf "SUMMARY cases=%d mean(step_max)=%.2f worst(step_max)=%.2f mean(hf_max_dB)=%.1f worst(hf_max_dB)=%.1f\n", n, sm/n, ws, hm/n, wh }'
