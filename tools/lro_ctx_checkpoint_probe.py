#!/usr/bin/env python3
"""The LCO author's context configuration, measured: the shipped one against the
checkpoint configuration of docs/plans/PLAN_lro_dialog.md §4.

    shipped     n_ctx 65,536, llama.cpp defaults (full-length sliding-window
                cache, no flash attention), the binding's own prefix reuse
    checkpoint  n_ctx 131,072, swa_full=False, flash_attn=True, the shared
                system-prompt prefix evaluated once, saved, and restored at the
                start of every authoring

Per configuration it reports what llama.cpp allocates, the process footprint,
how long a consult and a writing turn take, and — for the checkpoint — whether
the restore is EXACT (authoring B after a full authoring A, restored, must equal
B straight from the checkpoint byte for byte) and whether the sliding window is
intact afterwards. A control runs B after A without the restore, which is the
silent corruption the plan describes.

Needs only llama-cpp-python and the author GGUF: no plugin build, no torch, no
pipe_inference. On a Mac with the plugin installed the model is already in
~/Library/T5ynth/models.

    python3 -m venv /tmp/lro-probe
    /tmp/lro-probe/bin/pip install "llama-cpp-python>=0.3.36"
    /tmp/lro-probe/bin/python tools/lro_ctx_checkpoint_probe.py

Each configuration runs in its own child process, one after the other, so their
memory does not add up. Close the plugin first: it holds its own copy of the
author. Expect several minutes per configuration; the shipped one is the slower.
"""
import argparse
import ctypes
import ctypes.util
import inspect
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "backend"))

_SLOT = "gemma-4-12b-it-qat-q4_0"   # pipe_inference._CODER_SLOT
_WINDOW = 1024                       # gemma4.attention.sliding_window
_CONFIGS = {
    "shipped":    dict(n_ctx=65536,  swa_full=None,  flash_attn=None,  checkpoint=False),
    "checkpoint": dict(n_ctx=131072, swa_full=False, flash_attn=True,  checkpoint=True),
}
_PROMPT_A = "a breathy glass flute that slowly turns metallic"
_PROMPT_B = "a dark bowed metal plate with a slow swell"


def find_gguf(explicit):
    """The author GGUF: --model, else the backend's model roots (the subset of
    pipe_inference._model_search_base_dirs that the app writes to)."""
    if explicit:
        p = Path(explicit).expanduser()
        hits = [p] if p.is_file() else sorted(p.glob("*.gguf"))
    else:
        roots = ([Path(os.environ["T5YNTH_MODEL_DIR"])] if os.environ.get("T5YNTH_MODEL_DIR")
                 else [Path.home() / "Library" / "T5ynth" / "models",
                       Path.home() / ".local" / "share" / "T5ynth" / "models"])
        hits = [g for r in roots for g in sorted((r / "coder" / _SLOT).glob("*.gguf"))]
    hits = [h for h in hits if not h.name.startswith("mmproj")]
    if not hits:
        raise SystemExit("author GGUF not found (pass --model <dir or .gguf>)")
    return hits[0]


def footprint_mib():
    """The process's physical footprint. On macOS this is ri_phys_footprint, which
    counts Metal buffers in unified memory; elsewhere peak RSS, which does not
    count GPU memory (llama.cpp's own buffer report is the number to read there)."""
    try:
        if sys.platform == "darwin":
            class RUsageInfoV0(ctypes.Structure):
                _fields_ = [("uuid", ctypes.c_uint8 * 16)] + [
                    (n, ctypes.c_uint64) for n in (
                        "user_time", "system_time", "pkg_idle_wkups", "interrupt_wkups",
                        "pageins", "wired_size", "resident_size", "phys_footprint",
                        "proc_start_abstime", "proc_exit_abstime")]
            info = RUsageInfoV0()
            libc = ctypes.CDLL(ctypes.util.find_library("c"))
            if libc.proc_pid_rusage(os.getpid(), 0, ctypes.byref(info)) != 0:
                return None
            return round(info.phys_footprint / 2**20)
        import resource
        return round(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 1024)
    except Exception:
        return None


def load(gguf, cfg):
    """Load the model with llama.cpp's log captured at the file descriptor, and read
    the buffers it allocated from that log."""
    from llama_cpp import Llama
    from llama_cpp._logger import set_verbose
    kw = dict(model_path=str(gguf), n_gpu_layers=-1, n_ctx=cfg["n_ctx"], verbose=True)
    for k in ("swa_full", "flash_attn"):
        if cfg[k] is not None:
            if k not in inspect.signature(Llama.__init__).parameters:
                raise SystemExit(f"this llama-cpp-python has no '{k}' (need >= 0.3.36)")
            kw[k] = cfg[k]
    cap = tempfile.TemporaryFile()
    sys.stderr.flush()
    saved = os.dup(2)
    os.dup2(cap.fileno(), 2)
    t0 = time.time()
    try:
        m = Llama(**kw)
    finally:
        sys.stderr.flush()
        os.dup2(saved, 2)
        os.close(saved)
    secs = time.time() - t0
    set_verbose(False)
    m.verbose = False
    cap.seek(0)
    log = cap.read().decode("utf-8", "replace")
    kv = [dict(mib=float(a), cells=int(b), layers=int(c)) for a, b, c in
          re.findall(r"llama_kv_cache: size =\s*([\d.]+) MiB \(\s*(\d+) cells,\s*(\d+) layers", log)]
    compute = {d: float(v) for d, v in re.findall(r"(\S+) compute buffer size =\s*([\d.]+) MiB", log)}
    model = {d: float(v) for d, v in re.findall(r"(\S+) model buffer size =\s*([\d.]+) MiB", log)}
    swa_note = "using full-size SWA cache" in log
    return m, dict(load_s=round(secs, 1), kv=kv, compute_mib=compute, model_mib=model,
                   full_size_swa=swa_note)


def child(name, gguf, out_path):
    import llama_cpp
    import lco_write as W
    cfg = _CONFIGS[name]
    res = {"config": name, **{k: v for k, v in cfg.items()},
           "llama_cpp_python": llama_cpp.__version__, "platform": sys.platform}
    m, res["alloc"] = load(gguf, cfg)
    res["footprint_mib_after_load"] = footprint_mib()
    sysp = W.system_prompt(params=None)
    mem = m._ctx.memory

    def call(msgs, mx=-1):
        t0 = time.time()
        r = m.create_chat_completion(messages=[{"role": "system", "content": sysp}] + msgs,
                                     temperature=0.0, max_tokens=mx)
        u = r["usage"]
        return (r["choices"][0]["message"]["content"] or "",
                {"s": round(time.time() - t0, 1), "prompt_tokens": u["prompt_tokens"],
                 "completion_tokens": u["completion_tokens"]})

    def window(info):
        n = info["prompt_tokens"]
        lo = llama_cpp.llama_memory_seq_pos_min(mem, 0)
        return {"pos_min": lo, "needed_max": n - _WINDOW, "intact": lo <= n - _WINDOW}

    def save(key, val):
        res[key] = val
        Path(out_path).write_text(json.dumps(res, indent=1))

    uA = _PROMPT_A + W._CONSULT_TURN
    uB = _PROMPT_B + W._CONSULT_TURN
    # The first call evaluates the whole system prompt cold, as the first
    # authoring after a launch does; every later call in both configurations
    # starts warm. The shared prefix is what two different user turns have in
    # common.
    _, first = call([{"role": "user", "content": "x"}], mx=1)
    t1 = list(m.input_ids[:m.n_tokens])
    call([{"role": "user", "content": "y z"}], mx=1)
    t2 = list(m.input_ids[:m.n_tokens])
    k = next(i for i, (a, b) in enumerate(zip(t1, t2)) if a != b)
    save("system_prompt", {"prefix_tokens": k, "cold_first_call_s": first["s"]})
    restore = (lambda: None)
    if cfg["checkpoint"]:
        m.reset()
        t0 = time.time()
        m.eval(t1[:k])
        t_build = round(time.time() - t0, 1)
        t0 = time.time()
        state = m.save_state()
        save("checkpoint", {"build_s": t_build, "save_s": round(time.time() - t0, 2),
                            "state_mib": round(state.llama_state_size / 2**20),
                            "logits_mib": round(state.scores.nbytes / 2**20)})
        res["footprint_mib_after_checkpoint"] = footprint_mib()
        loads = []

        def restore():
            t0 = time.time()
            m.load_state(state)
            loads.append(round(time.time() - t0, 2))

        restore()
        ref, info = call([{"role": "user", "content": uB}], mx=300)
        save("B_from_checkpoint", {**info, "window": window(info)})

    try:
        restore()
        cA, info = call([{"role": "user", "content": uA}])
        save("A_consult", info)
        named = W.named_entries(cA)
        wt = W._writing_turn(W.open_entries(named))
        conv = [{"role": "user", "content": uA}, {"role": "assistant", "content": cA},
                {"role": "user", "content": wt}]
        _, info = call(conv, mx=200)
        save("A_writing_turn", {**info, "opened": named.get("instruments")})
    except ValueError as exc:            # the window overflowed: that is a result
        save("A_error", str(exc))
        conv = None

    restore()
    after, info = call([{"role": "user", "content": uB}], mx=300)
    save("B_after_A", {**info, "window": window(info)})
    if cfg["checkpoint"]:
        res["B_after_A"]["equals_B_from_checkpoint"] = (after == ref)
        res["restore_s"] = loads
        if conv is not None:
            call(conv, mx=50)
            bad, info = call([{"role": "user", "content": uB}], mx=300)
            save("control_no_restore", {"window": window(info),
                                        "equals_B_from_checkpoint": bad == ref})
    res["footprint_mib_end"] = footprint_mib()
    save("done", True)
    sys.stdout.flush()
    # llama.cpp's Metal backend asserts while freeing the device at exit
    # (tools/lco_author_offline.py documents the same), so leave without teardown.
    os._exit(0)


def report(results):
    def g(r, *path):
        for p in path:
            r = r.get(p) if isinstance(r, dict) else None
        return "—" if r is None else r

    def turn(r, key):
        return f"{g(r, key, 's')}  ({g(r, key, 'prompt_tokens')} / {g(r, key, 'completion_tokens')})"
    rows = []
    for r in results:
        a = r.get("alloc", {})
        kv = " + ".join(f"{x['mib']:.0f}" for x in a.get("kv", [])) or "—"
        rows += [
            (r["config"], ""),
            ("  llama-cpp-python / platform", f"{r.get('llama_cpp_python')} / {r.get('platform')}"),
            ("  n_ctx", r.get("n_ctx")),
            ("  KV cache MiB (global + sliding)", kv),
            ("  full-size sliding cache", a.get("full_size_swa")),
            ("  compute buffers MiB", a.get("compute_mib")),
            ("  load s", a.get("load_s")),
            ("  footprint MiB after load / end *", f"{g(r, 'footprint_mib_after_load')} / {g(r, 'footprint_mib_end')}"),
            ("  system prompt, shared prefix tokens", g(r, "system_prompt", "prefix_tokens")),
            ("  first call, system prompt cold, s", g(r, "system_prompt", "cold_first_call_s")),
        ]
        if r.get("checkpoint"):
            c = r["checkpoint"]
            rows += [
                ("  checkpoint build s / save s", f"{c['build_s']} / {c['save_s']}"),
                ("  checkpoint state MiB (+ logits)", f"{c['state_mib']} (+ {c['logits_mib']})"),
                ("  restore s", r.get("restore_s")),
                ("  footprint MiB after checkpoint *", g(r, "footprint_mib_after_checkpoint")),
            ]
        rows += [
            ("  A consult s  (prompt / generated tokens)", turn(r, "A_consult")),
            ("  A writing turn s  (prompt / generated)", turn(r, "A_writing_turn")),
        ]
        if r.get("A_error"):
            rows.append(("  A ERROR", r["A_error"]))
        rows += [
            ("  B after A s  (prompt / generated)", turn(r, "B_after_A")),
            ("  B after A: sliding window intact", g(r, "B_after_A", "window", "intact")),
        ]
        if r.get("checkpoint"):
            rows += [
                ("  B after A == B from checkpoint", g(r, "B_after_A", "equals_B_from_checkpoint")),
                ("  control, no restore: window intact (expect False)", g(r, "control_no_restore", "window", "intact")),
                ("  control, no restore: == B from checkpoint", g(r, "control_no_restore", "equals_B_from_checkpoint")),
            ]
        if not r.get("done"):
            rows.append(("  INCOMPLETE", "the child process ended early; see its output above"))
    w = max(len(k) for k, _ in rows)
    for k, v in rows:
        print(f"{k:<{w}}  {v}")
    print("\n* macOS: phys_footprint, current, Metal buffers included. Elsewhere: peak RSS,\n"
          "  GPU memory not included — read the KV and compute rows there.")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--model", help="author GGUF, or the directory holding it")
    ap.add_argument("--only", choices=sorted(_CONFIGS), help="run one configuration")
    ap.add_argument("--json", help="write all results here")
    ap.add_argument("--child", choices=sorted(_CONFIGS), help=argparse.SUPPRESS)
    ap.add_argument("--out", help=argparse.SUPPRESS)
    args = ap.parse_args()
    gguf = find_gguf(args.model)
    if args.child:
        child(args.child, gguf, args.out)
        return
    print(f"author: {gguf}", flush=True)
    results = []
    for name in ([args.only] if args.only else ["shipped", "checkpoint"]):
        print(f"\n== {name} ...", flush=True)
        with tempfile.NamedTemporaryFile(suffix=".json", delete=False) as f:
            out = f.name
        proc = subprocess.run([sys.executable, __file__, "--child", name,
                               "--model", str(gguf), "--out", out])
        try:
            r = json.loads(Path(out).read_text() or "{}")
        except (OSError, ValueError):
            r = {}
        r.setdefault("config", name)
        if proc.returncode != 0:
            r["exit_code"] = proc.returncode
        results.append(r)
        os.unlink(out)
    print()
    report(results)
    if args.json:
        Path(args.json).write_text(json.dumps(results, indent=1))


if __name__ == "__main__":
    main()
