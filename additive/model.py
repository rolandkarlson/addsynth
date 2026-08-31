"""AdditiveModel: the analyzed representation of one sound.

The model is everything the synth needs to play a timbre at any pitch:
  - env:        (n_frames, n_partials) float32, linear amplitude of each
                harmonic partial over time, at `control_rate` frames/sec.
  - f0_track:   (n_frames,) float32, pitch of each frame as a RATIO to
                f0_ref (1.0 = nominal pitch). Carries vibrato/drift.
  - noise_env:  (n_frames, n_noise_bands) float32, linear amplitude of
                log-spaced noise bands over time.
  - detune:     (n_partials,) float32, per-partial frequency ratio (~1.0)
                measured against the ideal k*f0 harmonic grid — carries
                piano stretch, detuned-stack shimmer, bell inharmonicity.
  - f0_ref:     nominal fundamental (Hz) of the analyzed sound.
  - inharmonicity B: partial k sits at k*f0*sqrt(1 + B*k^2)*detune[k].

File format (.addm), version 2, deliberately trivial to parse from C++:

    bytes 0..3   magic "ADDM"
    bytes 4..7   uint32 LE version (2)
    bytes 8..11  uint32 LE header length H
    bytes 12..   H bytes of UTF-8 JSON header
    then, back to back, float32 LE arrays in this order:
        env        n_frames * n_partials
        f0_track   n_frames
        noise_env  n_frames * n_noise_bands
        noise_band_freqs  n_noise_bands   (center Hz of each noise band)
        detune     n_partials
"""

from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field

import numpy as np

MAGIC = b"ADDM"
VERSION = 2


@dataclass
class AdditiveModel:
    env: np.ndarray            # (n_frames, n_partials) float32
    f0_track: np.ndarray       # (n_frames,) float32, ratio to f0_ref
    noise_env: np.ndarray      # (n_frames, n_noise_bands) float32
    noise_band_freqs: np.ndarray  # (n_noise_bands,) float32, Hz
    f0_ref: float              # Hz
    control_rate: float        # frames per second
    detune: np.ndarray | None = None  # (n_partials,) ratio, default 1.0
    inharmonicity: float = 0.0
    name: str = "untitled"
    source_sr: int = 44100
    meta: dict = field(default_factory=dict)

    def __post_init__(self):
        if self.detune is None:
            self.detune = np.ones(self.env.shape[1], dtype=np.float32)

    @property
    def n_frames(self) -> int:
        return self.env.shape[0]

    @property
    def n_partials(self) -> int:
        return self.env.shape[1]

    @property
    def duration(self) -> float:
        return self.n_frames / self.control_rate

    def partial_freqs(self, f0: float) -> np.ndarray:
        """Frequencies (Hz) of all partials for a given fundamental."""
        k = np.arange(1, self.n_partials + 1, dtype=np.float64)
        return k * f0 * np.sqrt(1.0 + self.inharmonicity * k * k) * self.detune

    # ---------------------------------------------------------------- io

    def save(self, path: str) -> None:
        header = {
            "name": self.name,
            "n_frames": int(self.n_frames),
            "n_partials": int(self.n_partials),
            "n_noise_bands": int(self.noise_env.shape[1]),
            "f0_ref": float(self.f0_ref),
            "control_rate": float(self.control_rate),
            "inharmonicity": float(self.inharmonicity),
            "source_sr": int(self.source_sr),
            "meta": self.meta,
        }
        hdr = json.dumps(header).encode("utf-8")
        with open(path, "wb") as f:
            f.write(MAGIC)
            f.write(struct.pack("<II", VERSION, len(hdr)))
            f.write(hdr)
            f.write(self.env.astype("<f4").tobytes())
            f.write(self.f0_track.astype("<f4").tobytes())
            f.write(self.noise_env.astype("<f4").tobytes())
            f.write(self.noise_band_freqs.astype("<f4").tobytes())
            f.write(self.detune.astype("<f4").tobytes())

    @classmethod
    def load(cls, path: str) -> "AdditiveModel":
        with open(path, "rb") as f:
            magic = f.read(4)
            if magic != MAGIC:
                raise ValueError(f"{path}: not an ADDM file")
            version, hlen = struct.unpack("<II", f.read(8))
            if version != VERSION:
                raise ValueError(f"{path}: version {version} model — "
                                 f"re-analyze the source (format is v{VERSION})")
            header = json.loads(f.read(hlen).decode("utf-8"))
            nf = header["n_frames"]
            npart = header["n_partials"]
            nb = header["n_noise_bands"]

            def read_f32(count: int) -> np.ndarray:
                return np.frombuffer(f.read(count * 4), dtype="<f4").copy()

            env = read_f32(nf * npart).reshape(nf, npart)
            f0_track = read_f32(nf)
            noise_env = read_f32(nf * nb).reshape(nf, nb)
            band_freqs = read_f32(nb)
            detune = read_f32(npart)
        return cls(
            env=env,
            f0_track=f0_track,
            noise_env=noise_env,
            noise_band_freqs=band_freqs,
            detune=detune,
            f0_ref=header["f0_ref"],
            control_rate=header["control_rate"],
            inharmonicity=header.get("inharmonicity", 0.0),
            name=header.get("name", "untitled"),
            source_sr=header.get("source_sr", 44100),
            meta=header.get("meta", {}),
        )
