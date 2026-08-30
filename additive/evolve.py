"""Evolve a model toward a reference sound.

Genome = a fixed stack of timbre transforms applied to a base model
(spectral tilt, odd/even balance, 8 partial-group gains, inharmonicity,
time stretch, noise scale). Fitness = multi-scale log-mel L1 distance
between the rendered note and the reference audio. Search = a simple
(mu + lambda) evolution strategy — no dependencies beyond numpy/librosa.
"""

from __future__ import annotations

from dataclasses import dataclass

import numpy as np
import librosa

from .model import AdditiveModel
from .synth import render_note
from . import mutate as T

SR = 44100

# genome layout: [tilt_db, odd, even, g0..g7, B, stretch_log, noise_log]
N_GROUPS = 8
GENOME_LEN = 3 + N_GROUPS + 3
LO = np.array([-18, -1.5, -1.5] + [-2.5] * N_GROUPS + [0.0, -0.7, -2.5])
HI = np.array([+18, +1.5, +1.5] + [+2.5] * N_GROUPS + [8e-3, +0.7, +1.5])


def apply_genome(base: AdditiveModel, v: np.ndarray) -> AdditiveModel:
    m = T.tilt(base, float(v[0]))
    m = T.odd_even(m, float(np.exp(v[1])), float(np.exp(v[2])))
    groups = np.linspace(0, N_GROUPS, m.n_partials, endpoint=False).astype(int)
    gains = np.exp(v[3:3 + N_GROUPS])
    m.env *= gains[groups][None, :].astype(np.float32)
    m = T.set_inharmonicity(m, float(v[3 + N_GROUPS]))
    m = T.stretch_time(m, float(np.exp(v[4 + N_GROUPS])))
    m.noise_env *= np.float32(np.exp(v[5 + N_GROUPS]))
    return m


def _logmels(y: np.ndarray, sr: int) -> list[np.ndarray]:
    out = []
    for n_fft in (512, 2048):
        s = librosa.feature.melspectrogram(
            y=y, sr=sr, n_fft=n_fft, hop_length=n_fft // 4, n_mels=64)
        out.append(librosa.power_to_db(s + 1e-9))
    return out


def _distance(a: list[np.ndarray], b: list[np.ndarray]) -> float:
    d = 0.0
    for x, y in zip(a, b):
        n = min(x.shape[1], y.shape[1])
        d += float(np.mean(np.abs(x[:, :n] - y[:, :n])))
    return d / len(a)


def _norm(y: np.ndarray) -> np.ndarray:
    r = np.sqrt(np.mean(y ** 2))
    return y / max(r, 1e-9) * 0.1


@dataclass
class EvolveResult:
    model: AdditiveModel
    genome: np.ndarray
    fitness: float
    history: list[float]


def evolve(
    reference_path: str,
    base: AdditiveModel,
    generations: int = 20,
    population: int = 16,
    elites: int = 4,
    sigma: float = 0.25,
    seed: int | None = None,
    f0_override: float | None = None,
    max_ref_sec: float = 4.0,
    log=print,
) -> EvolveResult:
    rng = np.random.default_rng(seed)

    ref, _ = librosa.load(reference_path, sr=SR, mono=True, duration=max_ref_sec)
    if f0_override is not None:
        f0_ref = float(f0_override)
    else:
        f0, voiced, _ = librosa.pyin(ref, fmin=40, fmax=2000, sr=SR)
        if not np.any(voiced):
            raise ValueError("reference has no pitched content; pass f0_override")
        f0_ref = float(np.median(f0[voiced]))
    duration = len(ref) / SR
    ref_feats = _logmels(_norm(ref), SR)
    log(f"reference: f0={f0_ref:.1f} Hz, {duration:.2f}s")

    span = HI - LO

    def clamp(v): return np.clip(v, LO, HI)

    def fitness(v: np.ndarray) -> float:
        m = apply_genome(base, v)
        y = render_note(m, f0=f0_ref, duration=duration, sr=SR, seed=0)
        return _distance(_logmels(_norm(y.astype(np.float64)), SR), ref_feats)

    # init: identity genome + small perturbations
    identity = np.zeros(GENOME_LEN)
    pop = [identity] + [clamp(identity + rng.normal(0, sigma, GENOME_LEN) * span * 0.5)
                        for _ in range(population - 1)]
    history = []
    scored = sorted(((fitness(v), v) for v in pop), key=lambda t: t[0])

    for gen in range(generations):
        parents = scored[:elites]
        children = []
        while len(children) < population - elites:
            fa, va = parents[rng.integers(len(parents))]
            if rng.random() < 0.3 and len(parents) > 1:      # crossover
                _, vb = parents[rng.integers(len(parents))]
                mask = rng.random(GENOME_LEN) < 0.5
                va = np.where(mask, va, vb)
            children.append(clamp(va + rng.normal(0, sigma, GENOME_LEN) * span
                                  * rng.uniform(0.2, 1.0)))
        scored = sorted(parents + [(fitness(v), v) for v in children],
                        key=lambda t: t[0])
        history.append(scored[0][0])
        log(f"gen {gen + 1:2d}/{generations}: best={scored[0][0]:.3f} dB "
            f"(median {scored[len(scored) // 2][0]:.3f})")

    best_fit, best_v = scored[0]
    model = apply_genome(base, best_v)
    model.name = f"{base.name}_evolved"
    model.meta.update(evolved_from=reference_path, fitness=round(best_fit, 4))
    return EvolveResult(model=model, genome=best_v, fitness=best_fit,
                        history=history)
