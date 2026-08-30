"""Interactive breeding: you are the fitness function.

A session lives in a directory of generations:

    breeding/mysession/gen00/00.addm ... 07.addm  (+ .wav previews)
    breeding/mysession/gen01/...

Workflow (via cli):
    additive breed start base.addm -o breeding/mysession
        -> renders 8 mutants; listen to the wavs, pick your 2 favourites
    additive breed next 2 5 -o breeding/mysession
        -> children of 02 x 05: morph at random depths + a small mutation
    additive breed pick 3 -o breeding/mysession [--dest models]
        -> exports the chosen one from the latest generation
"""

from __future__ import annotations

import os
import re

import numpy as np
import soundfile as sf

from .model import AdditiveModel
from .synth import render_note
from .mutate import morph, mutate

SR = 44100


def _gen_dirs(session: str) -> list[str]:
    if not os.path.isdir(session):
        return []
    ds = [d for d in os.listdir(session) if re.fullmatch(r"gen\d\d", d)]
    return [os.path.join(session, d) for d in sorted(ds)]


def _write_generation(session: str, models: list[AdditiveModel],
                      log=print) -> str:
    idx = len(_gen_dirs(session))
    gdir = os.path.join(session, f"gen{idx:02d}")
    os.makedirs(gdir, exist_ok=True)
    for i, m in enumerate(models):
        m.name = f"g{idx:02d}_{i:02d}"
        m.save(os.path.join(gdir, f"{i:02d}.addm"))
        wav = render_note(m, sr=SR, seed=0)
        sf.write(os.path.join(gdir, f"{i:02d}.wav"), wav, SR)
    log(f"{gdir}: {len(models)} candidates rendered — listen to the .wavs")
    return gdir


def start(base_path: str, session: str, n: int = 8, amount: float = 0.4,
          seed: int | None = None, log=print) -> str:
    base = AdditiveModel.load(base_path)
    rng = np.random.default_rng(seed)
    if _gen_dirs(session):
        raise SystemExit(f"{session} already exists — use 'next' or a new -o dir")
    kids = [base] + [mutate(base, amount=amount, rng=rng) for _ in range(n - 1)]
    log(f"gen00: candidate 00 is the unmodified base, 01..{n-1:02d} are mutants")
    return _write_generation(session, kids, log)


def next_gen(session: str, pick_a: int, pick_b: int, n: int = 8,
             amount: float = 0.25, seed: int | None = None, log=print) -> str:
    gens = _gen_dirs(session)
    if not gens:
        raise SystemExit(f"no generations in {session} — run 'start' first")
    last = gens[-1]
    a = AdditiveModel.load(os.path.join(last, f"{pick_a:02d}.addm"))
    b = AdditiveModel.load(os.path.join(last, f"{pick_b:02d}.addm"))
    rng = np.random.default_rng(seed)

    kids = [a, b]  # parents survive so lineages never regress
    while len(kids) < n:
        child = morph(a, b, float(rng.uniform(0.2, 0.8)))
        if rng.random() < 0.8:
            child = mutate(child, amount=amount * float(rng.uniform(0.4, 1.5)),
                           rng=rng)
        kids.append(child)
    log(f"breeding {pick_a:02d} x {pick_b:02d} (parents kept as 00 and 01)")
    return _write_generation(session, kids, log)


def pick(session: str, index: int, dest: str = "models", log=print) -> str:
    gens = _gen_dirs(session)
    if not gens:
        raise SystemExit(f"no generations in {session}")
    src = os.path.join(gens[-1], f"{index:02d}.addm")
    m = AdditiveModel.load(src)
    m.name = os.path.basename(session.rstrip("/")) + f"_{index:02d}"
    os.makedirs(dest, exist_ok=True)
    out = os.path.join(dest, f"{m.name}.addm")
    m.save(out)
    log(f"exported {out}")
    return out
