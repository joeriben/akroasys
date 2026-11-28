#!/usr/bin/env python3
"""Drive the REAL LCO author over a corpus of prompts, offline, in one process.

The LCO's whole architecture is one prompt, one inference against the curated
library, one Csound orchestra, and it runs. Everything that can go wrong with it
is a property of what the model actually writes — whether a morph becomes a
crossfade or collapses into a layer, whether a repair continues the attempt or
starts the sound over, whether the emitted body MOVES — and none of that is
visible from the code. It has to be run.

Running it one prompt at a time through the plugin costs a model load per prompt
and leaves no record. This loads the author once (the shipped 4-bit GGUF, found
by the backend's own resolver) and puts a whole corpus through
`build_csound_response` — the exact call `pipe_inference`'s csound branch makes
one line after resolving the model — recording for every prompt how many attempts
it took, which Csound errors it had to repair past, what it wrote, and how long
each inference ran.

This is a measurement harness, not a wire test: it calls the author path
in-process. `tools/lco_trace_wire_check.py` is what proves the IPC wire carries
the result.

With `--measure`, each body that compiled is rendered through
`tools/lco_measure.py` and its movement signature recorded — which is the only
way a corpus can test the CLASS the LCO exists for (moving, evolving, morphing
sounds) rather than the static cases an implementation finds easy.

    .venv/bin/python tools/lco_author_offline.py "a bowed violin morphing into a bell"
    .venv/bin/python tools/lco_author_offline.py --corpus docs/… --out run.json --measure
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "backend"))
sys.path.insert(0, str(REPO / "tools"))

import pipe_inference as P          # noqa: E402  (needs the path above)
from lco_write import build_csound_response  # noqa: E402


def author(coder_dir, device="cpu"):
    """The author surface `build_csound_response` needs, plus a stopwatch.

    Mirrors pipe_inference's `csound_llm` closure exactly, including
    `accepts_messages` — declared, not inferred, because that flag is what lets a
    repair CONTINUE the failed attempt instead of asking for the sound again from
    nothing. Getting it wrong here would measure a different repair loop than the
    one that ships.
    """
    gguf = P._coder_gguf_file(coder_dir)
    stats = []

    def llm(text, system_prompt, max_new_tokens=None, on_delta=None):
        t0 = time.time()
        if gguf is not None:
            out = P.run_gguf_instruct(text, gguf, system_prompt,
                                      max_new_tokens=max_new_tokens,
                                      on_delta=on_delta)
        else:
            out = P.run_instruct(text, coder_dir, device, system_prompt,
                                 max_new_tokens=max_new_tokens)
        stats.append({"secs": round(time.time() - t0, 1),
                      "turns": len(text) if isinstance(text, (list, tuple)) else 1,
                      "chars": len(out or "")})
        return out

    llm.accepts_messages = True
    if gguf is not None:
        llm.count_tokens, llm.context_window = P.gguf_author_limits(gguf)
    return llm, stats


def run(prompt, llm, stats, measure=False, freq=220.0, dur=4.0):
    stats.clear()
    t0 = time.time()
    r = build_csound_response(prompt, llm)
    out = {"prompt": prompt,
           "ok": r.get("ok"),
           "attempts": r.get("attempts"),
           "repairs": r.get("repairs"),
           "reading": r.get("reading"),
           "thinking": r.get("thinking"),
           "body": r.get("params_text"),
           # The knobs the author gave the player for this body, and every
           # declaration that was refused — whether a model of this size writes
           # the two lines at all is the open question the panel work rests on,
           # and it is only answerable by running it.
           "controls": r.get("controls"),
           "error": r.get("error"),
           "consultation": r.get("consultation"),
           "secs_total": round(time.time() - t0, 1),
           "inferences": list(stats)}
    if measure and out["ok"] and out["body"]:
        import lco_measure as M
        # The body as authored, which is Csound throughout and needs nothing
        # taken out of it: every knob stands in it as its own library line, at
        # the number the author chose. So this measures the sound the author
        # described — the plugin's `kp` wiring belongs to the orchestra the host
        # wraps, and this scaffold is not that host.
        y, err = M.render(out["body"], dur=dur, freq=freq)
        out["measured"] = {"error": err} if y is None else dict(
            M.measure(y, freq), warning=err)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prompts", nargs="*", help="prompts to author")
    ap.add_argument("--corpus", help="file of prompts, one per line ('#' comments)")
    ap.add_argument("--out", help="write the full result JSON here (as it goes)")
    ap.add_argument("--measure", action="store_true",
                    help="render each compiled body and record what it does")
    ap.add_argument("--freq", type=float, default=220.0)
    ap.add_argument("--dur", type=float, default=4.0)
    ap.add_argument("--model", help="override the author model directory")
    args = ap.parse_args()

    prompts = list(args.prompts)
    if args.corpus:
        prompts += [l.strip() for l in Path(args.corpus).read_text().splitlines()
                    if l.strip() and not l.lstrip().startswith("#")]
    if not prompts:
        ap.error("give at least one prompt, or --corpus")

    request = {"coder_model_path": args.model} if args.model else {}
    coder_dir = P._resolve_coder_model_dir(request)
    if coder_dir is None:
        raise SystemExit("no LCO author model installed "
                         "(set $LCO_MODEL_DIR or pass --model)")
    print(f"author: {coder_dir}", flush=True)

    llm, stats = author(coder_dir)
    results = []
    for i, prompt in enumerate(prompts, 1):
        try:
            r = run(prompt, llm, stats, args.measure, args.freq, args.dur)
        except Exception as exc:          # a corpus run must not die on one prompt
            r = {"prompt": prompt, "ok": False, "error": repr(exc)}
        results.append(r)
        m = r.get("measured") or {}
        moved = ("" if not m else
                 f"  travel {m.get('centroid_travel_hz')}/{m.get('centroid_motion_hz')} Hz")
        print(f"[{i}/{len(prompts)}] {'ok ' if r.get('ok') else 'FAIL'} "
              f"attempts={r.get('attempts')} {r.get('secs_total')}s  {prompt!r}"
              f"{moved}" + ("" if r.get("ok") else f"\n    {r.get('error')}"),
              flush=True)
        c = r.get("controls") or {}
        if c.get("params") or c.get("refused"):
            for part in c.get("parts", []):
                print(f"    {part['name']}: " + ", ".join(
                    f"{p['name']!r}={p['value']:g}"
                    for p in c["params"] if p["part"] == part["n"]), flush=True)
            if not c.get("params"):
                print("    knobs: none", flush=True)
            for line in c.get("refused", []):
                print(f"    refused: {line}", flush=True)
        if args.out:
            Path(args.out).write_text(json.dumps(results, indent=1, ensure_ascii=False))

    n_ok = sum(1 for r in results if r.get("ok"))
    tries = [r.get("attempts") for r in results if r.get("attempts")]
    print(f"\n{n_ok}/{len(results)} authored"
          + (f", {sum(tries) / len(tries):.2f} attempts each" if tries else ""))

    # Did the author declare the player's knobs? A body with none is legal and
    # the panel handles it, but a corpus with none says the contract is not
    # reaching the model — which is a finding about the PROMPT, not about the
    # panel that shows the result.
    withc = [r for r in results if (r.get("controls") or {}).get("params")]
    n_knobs = sum(len(r["controls"]["params"]) for r in withc)
    n_ref = sum(len((r.get("controls") or {}).get("refused") or []) for r in results)
    print(f"{len(withc)}/{n_ok} authored instruments declared knobs "
          f"({n_knobs} in total, {n_ref} declarations refused)")

    # llama.cpp's Metal backend asserts while freeing the device at __cxa_finalize
    # (pipe_inference documents the same abort), so a clean run would exit 134.
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0 if n_ok == len(results) else 1)


if __name__ == "__main__":
    main()
