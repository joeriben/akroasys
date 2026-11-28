#!/usr/bin/env python3
"""What the library costs the LCO author, counted in the author model's own tokens.

The author works inside one context window (`pipe_inference._GGUF_N_CTX`): the
system prompt with the whole library index, the consultation, the writing turn
with the pages it asked for, its answer and every repair. There is no overflow
handling. A request over the window is a hard error and the prompt produces no
oscillator; an answer that fills the window is cut silently. So a library entry
is paid for twice, and neither price is visible from the lexicon:

    index   what the entry adds to the system prompt of EVERY prompt
            (its description, its parameters, their anchors)
    page    what it costs when the author asks for it to be opened
            (the Csound, the knob lines, how each anchor changes the code)
    instr   the lines of its code that run: what the author writes out again

Only the tokenizer is loaded (`vocab_only`), no weights, no GPU.

    .venv/bin/python tools/lco_token_budget.py
        every entry, the sums, the system prompt against the window
    .venv/bin/python tools/lco_token_budget.py --request drum_head struck_drum
        the writing request that opens these entries, and what is left of the
        window for the consultation reply, the answer and a repair
    .venv/bin/python tools/lco_token_budget.py --entry my_entry.json
        an entry that is not in the lexicon yet (one technique, lexicon shape)

The numbers are of the request as `lco_write` builds it. The chat template adds
a few tokens per turn, and the parameter shelf (`render_params`, present when
the player lets the author set the synth) is not counted: it comes off the
same headroom.
"""
import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "backend"))
sys.path.insert(0, str(REPO / "tools"))

import pipe_inference as P          # noqa: E402  (the model resolver and the window)
import lco_write as W               # noqa: E402
import lco_build_library as B       # noqa: E402


def tokenizer():
    coder_dir = P._resolve_coder_model_dir({})
    gguf = P._coder_gguf_file(coder_dir) if coder_dir is not None else None
    if gguf is None:
        sys.exit("no GGUF author model installed: the counts are this model's, "
                 "and there is nothing honest to estimate them with")
    from llama_cpp import Llama
    llm = Llama(model_path=str(gguf), vocab_only=True, verbose=False)
    return lambda s: len(llm.tokenize(s.encode("utf-8"), add_bos=False, special=False))


def solo(item):
    return {"instruments": [item], "adjectives": [], "motions": [], "catalogue": {}}


def cost(item, n):
    """index / page / instr of one assembled library instrument."""
    empty = {"instruments": [], "adjectives": [], "motions": [], "catalogue": {}}
    index = n(W.render_index(solo(item))) - n(W.render_index(empty))
    page = n(W.render_library(solo(item))) - n(W.render_library(empty))
    runs = [l for l in W._code_lines(item.get("code") or "") if l.strip()]
    return {"index": index, "page": page, "instr": n("\n".join(runs)), "lines": len(runs)}


def table(items, n):
    rows = sorted(((cost(it, n), it["key"]) for it in items),
                  key=lambda r: r[0]["page"], reverse=True)
    print(f"{'entry':16} {'index':>6} {'page':>7} {'instr':>6} {'lines':>5}")
    for c, key in rows:
        print(f"{key:16} {c['index']:6d} {c['page']:7d} {c['instr']:6d} {c['lines']:5d}")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--request", nargs="+", metavar="KEY",
                    help="size of the writing request that opens these instruments")
    ap.add_argument("--entry", metavar="FILE",
                    help="cost of one lexicon-shaped entry that is not in the lexicon")
    args = ap.parse_args()
    n = tokenizer()
    window = P._GGUF_N_CTX

    if args.entry:
        lex = json.loads((REPO / "backend" / "dco_lexicon.json").read_text(encoding="utf-8"))
        lex["techniques"] = [json.loads(Path(args.entry).read_text(encoding="utf-8"))]
        table(B.assemble(lex)["instruments"], n)
        return 0

    lib = W._load_library()
    system = n(W.system_prompt())

    if args.request:
        known = {e["key"] for e in lib["instruments"]}
        missing = [k for k in args.request if k not in known]
        if missing:
            sys.exit(f"not a library instrument: {', '.join(missing)}")
        sel = W.open_entries({"instruments": args.request})
        turn = n(W._writing_turn(sel))
        table(sel["instruments"], n)
        print(f"\nsystem prompt {system:6d}\nwriting turn  {turn:6d}\n"
              f"together      {system + turn:6d} of {window}: {window - system - turn} left "
              "for the consultation reply, the answer and a repair")
        return 0 if system + turn <= window else 1

    rows = table(lib["instruments"], n)
    print(f"\n{len(rows)} instruments: index {sum(c['index'] for c, _ in rows)}, "
          f"pages {sum(c['page'] for c, _ in rows)}")
    print(f"system prompt {system} of a window of {window} "
          f"({100 * system / window:.0f} %), before the author has asked for a page")
    return 0


if __name__ == "__main__":
    sys.exit(main())
