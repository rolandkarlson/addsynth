"""Transformations on AdditiveModels — the 'interesting sounds' toolbox.

All functions return a NEW model (inputs are never modified), so they
compose freely and every result can be saved as .addm and played in the
plugin.
"""

from __future__ import annotations

import numpy as np

from .model import AdditiveModel


def _clone(m: AdditiveModel, **overrides) -> AdditiveModel:
    kw = dict(
        env=m.env.copy(),
        f0_track=m.f0_track.copy(),
        noise_env=m.noise_env.copy(),
        noise_band_freqs=m.noise_band_freqs.copy(),
        f0_ref=m.f0_ref,
        control_rate=m.control_rate,
        inharmonicity=m.inharmonicity,
        name=m.name,
        source_sr=m.source_sr,
        meta=dict(m.meta),
    )
    kw.update(overrides)
    return AdditiveModel(**kw)


def _resample_frames(x: np.ndarray, n_frames: int) -> np.ndarray:
    """Linear resample along the time axis to n_frames."""
    if x.shape[0] == n_frames:
        return x.copy()
    src = np.linspace(0, x.shape[0] - 1, n_frames)
    i0 = np.floor(src).astype(int)
    i1 = np.minimum(i0 + 1, x.shape[0] - 1)
    fr = (src - i0)[:, None] if x.ndim > 1 else (src - i0)
    return (x[i0] * (1 - fr) + x[i1] * fr).astype(x.dtype)


def morph(a: AdditiveModel, b: AdditiveModel, t: float,
          name: str | None = None) -> AdditiveModel:
    """Interpolate two models: t=0 -> a, t=1 -> b.

    Interpolation happens in log-amplitude space, which sounds far more
    natural than linear crossfading.
    """
    nf = max(a.n_frames, b.n_frames)
    npart = min(a.n_partials, b.n_partials)
    nb = min(a.noise_env.shape[1], b.noise_env.shape[1])

    def prep(m):
        return (_resample_frames(m.env[:, :npart], nf),
                _resample_frames(m.noise_env[:, :nb], nf),
                _resample_frames(m.f0_track, nf))

    ea, na, fa = prep(a)
    eb, nb_, fb = prep(b)
    eps = 1e-7
    env = np.exp((1 - t) * np.log(ea + eps) + t * np.log(eb + eps)) - eps
    noise = np.exp((1 - t) * np.log(na + eps) + t * np.log(nb_ + eps)) - eps
    return _clone(
        a,
        env=np.maximum(env, 0).astype(np.float32),
        noise_env=np.maximum(noise, 0).astype(np.float32),
        f0_track=((1 - t) * fa + t * fb).astype(np.float32),
        noise_band_freqs=a.noise_band_freqs[:nb].copy(),
        f0_ref=a.f0_ref * (b.f0_ref / a.f0_ref) ** t,
        inharmonicity=(1 - t) * a.inharmonicity + t * b.inharmonicity,
        name=name or f"{a.name}x{b.name}@{t:.2f}",
    )


def tilt(m: AdditiveModel, db_per_octave: float) -> AdditiveModel:
    """Spectral tilt: positive = brighter, negative = darker."""
    k = np.arange(1, m.n_partials + 1)
    g = 10 ** (db_per_octave * np.log2(k) / 20)
    return _clone(m, env=(m.env * g[None, :]).astype(np.float32),
                  name=f"{m.name}_tilt{db_per_octave:+.0f}")


def odd_even(m: AdditiveModel, odd_gain: float, even_gain: float) -> AdditiveModel:
    """Rebalance odd vs even harmonics (odd-only ~ clarinet/square)."""
    g = np.where(np.arange(1, m.n_partials + 1) % 2 == 1, odd_gain, even_gain)
    return _clone(m, env=(m.env * g[None, :]).astype(np.float32),
                  name=f"{m.name}_oddeven")


def stretch_time(m: AdditiveModel, factor: float) -> AdditiveModel:
    """Stretch the envelopes in time (2.0 = twice as long, pitch unchanged)."""
    nf = max(2, int(round(m.n_frames * factor)))
    return _clone(
        m,
        env=_resample_frames(m.env, nf),
        noise_env=_resample_frames(m.noise_env, nf),
        f0_track=_resample_frames(m.f0_track, nf),
        name=f"{m.name}_x{factor:.2g}",
    )


def set_inharmonicity(m: AdditiveModel, B: float) -> AdditiveModel:
    """0 = pure harmonic, ~1e-4 piano-ish, ~1e-2 bell-like."""
    return _clone(m, inharmonicity=B, name=f"{m.name}_B{B:g}")


def reverse(m: AdditiveModel) -> AdditiveModel:
    return _clone(m, env=m.env[::-1].copy(), noise_env=m.noise_env[::-1].copy(),
                  f0_track=m.f0_track[::-1].copy(), name=f"{m.name}_rev")


def freeze(m: AdditiveModel, at: float, hold_sec: float = 2.0) -> AdditiveModel:
    """Sustain the spectrum found at relative position `at` (0..1)."""
    idx = int(np.clip(at, 0, 1) * (m.n_frames - 1))
    hold = int(hold_sec * m.control_rate)
    env = np.concatenate([m.env[: idx + 1],
                          np.repeat(m.env[idx:idx + 1], hold, axis=0),
                          m.env[idx + 1:]])
    noise = np.concatenate([m.noise_env[: idx + 1],
                            np.repeat(m.noise_env[idx:idx + 1], hold, axis=0),
                            m.noise_env[idx + 1:]])
    f0t = np.concatenate([m.f0_track[: idx + 1],
                          np.repeat(m.f0_track[idx:idx + 1], hold),
                          m.f0_track[idx + 1:]])
    return _clone(m, env=env, noise_env=noise, f0_track=f0t,
                  name=f"{m.name}_frz{at:.2f}")


def mutate(m: AdditiveModel, amount: float = 0.3,
           rng: np.random.Generator | None = None) -> AdditiveModel:
    """Random mutation for genetic exploration.

    Applies a random subset of the transforms above with random strengths
    scaled by `amount` (0 = identity, 1 = wild).
    """
    rng = rng or np.random.default_rng()
    out = _clone(m)
    if rng.random() < 0.8:
        out = tilt(out, rng.normal(0, 6 * amount))
    if rng.random() < 0.5:
        out = odd_even(out, 1 + rng.normal(0, amount),
                       1 + rng.normal(0, amount))
    if rng.random() < 0.5:
        out = stretch_time(out, float(np.exp(rng.normal(0, 0.7 * amount))))
    if rng.random() < 0.4:
        out = set_inharmonicity(out, abs(rng.normal(0, 3e-4 * amount * 10)))
    if rng.random() < 0.3 * amount:
        out = reverse(out)
    # per-partial-group random gains (comb-like colorations)
    if rng.random() < 0.6:
        groups = rng.integers(0, 4, size=out.n_partials)
        gains = 10 ** (rng.normal(0, 8 * amount, size=4) / 20)
        out = _clone(out, env=(out.env * gains[groups][None, :]).astype(np.float32))
    out.name = f"{m.name}_mut"
    return out
