#!/usr/bin/env python3
"""Drive and direction of the `, c3` suffix, as CLAUDE.md asks of any change on the
prompt path. A C3 switch appends these tokens to both prompts.

    .venv/bin/python tools/pitch_c3_drive.py

Uses tools/measure_conditioning_drive.py's own Conditioning. Drive is the norm over the
real tokens, and a suffix adds tokens, so the per-token figure is printed beside it.
"""

import sys
from pathlib import Path

import torch
import torch.nn.functional as F

sys.path.insert(0, str(Path(__file__).parent))
from measure_conditioning_drive import Conditioning  # noqa: E402
from pitch_c3_generate import PITCHED  # noqa: E402

# backend/pipe_inference.py _native_modality_prefix: every SA3 prompt reaches the encoder
# behind this prefix at the current modality epoch. SAO takes none.
PREFIX = {"sao": "", "sa3": "TrackType: Instrument, "}

for engine in ("sao", "sa3"):
    c = Conditioning(engine)
    pre = PREFIX[engine]
    print(f"\n{engine}  {'prompt':38} tok  drive  tok  drive  ratio  per-token  cos")
    ratios, per_token, coss = [], [], []
    for p in PITCHED:
        A, ma = c.encode(pre + p)
        B, mb = c.encode(pre + p + ", c3")
        mask = (ma | mb)[0]
        pa, pb = c.proj(A)[0], c.proj(B)[0]
        da, db = pa[ma[0]].norm().item(), pb[mb[0]].norm().item()
        na, nb = int(ma.sum()), int(mb.sum())
        cos = F.cosine_similarity(pa[mask].flatten(), pb[mask].flatten(), dim=0).item()
        rt = (db / nb ** 0.5) / (da / na ** 0.5)
        ratios.append(db / da); per_token.append(rt); coss.append(cos)
        print(f"     {p:38} {na:3} {da:6.1f} {nb:3} {db:6.1f} {db / da:6.3f} {rt:9.3f} {cos:+.3f}")
    t = torch.tensor
    print(f"     median ratio {t(ratios).median():.3f}  per-token {t(per_token).median():.3f}  "
          f"cos {t(coss).median():+.3f}")
