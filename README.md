# synth-ideas — additive resynthesis engine

Sample any sound into an **additive model** (per-partial envelope matrix +
noise bands), then play, transpose, morph, and mutate it. Two halves:

- **Python** (`additive/`) — the "sound designer": analysis, offline
  rendering, morphing, genetic mutation. Fast to iterate.
- **JUCE plugin** (`plugin/`) — the instrument: loads `.addm` model files
  and plays them polyphonically in a DAW (Ableton) as AU/VST3.

## The representation

A sound = `env[n_frames][n_partials]` (amplitude of harmonic k at ~172
frames/sec) + `f0_track` (vibrato/drift as ratio) + `noise_env` (24
log-spaced noise bands) + `f0_ref` + inharmonicity `B`
(partial k at `k·f0·√(1+B·k²)`). Tonal by construction at any pitch.

Everything is stored in `.addm` files — a trivial binary format (JSON
header + float32 blocks, see `additive/model.py`) that both Python and the
plugin read.

## Python usage

```bash
.venv/bin/python demo.py                     # round-trip demo -> out/, models/

# your own samples (monophonic, pitched material works best)
.venv/bin/python -m additive.cli analyze mysample.wav -o models/my.addm

# batch import: whole directory of (long) tracks -> 5 models per file.
# Random slices are scored on pyin voiced-fraction x pitch stability;
# the most tonal moments win. Preview wavs land in <outdir>/previews/.
.venv/bin/python -m additive.cli import "/path/to/tracks" -o models/imported -n 5
.venv/bin/python -m additive.cli play models/my.addm --midi 57 --dur 3
.venv/bin/python -m additive.cli morph models/a.addm models/b.addm 0.5
.venv/bin/python -m additive.cli mutate models/my.addm -n 8 --amount 0.4
```

If pitch detection fails (polyphonic/noisy source), pass `--f0 <Hz>` to
force the fundamental.

Library: `additive.analyze / render_note / render_sequence`, transforms in
`additive.mutate` (morph, tilt, odd_even, stretch_time, set_inharmonicity,
reverse, freeze, mutate). All transforms return new models — compose freely.

## Plugin

```bash
cd plugin
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j8
```

Builds AU + VST3 + Standalone and copies them to `~/Library/Audio/Plug-Ins/`
automatically. In Ableton: Settings → Plug-Ins → rescan, then find
**AddSynth** (SynthIdeas).

**Morph pad**: *Add models...* (multi-select .addm files, up to 16), drag
the green nodes to place timbres on the XY field, drag the white circle to
morph — blending is inverse-distance-weighted, log-domain, live while notes
play. Double-click a node to remove it. Cursor position = automatable
`Morph X` / `Morph Y` parameters, so you can automate timbre sweeps from
Ableton. Pad layout is saved with the session.

**Knobs** (all automatable):
- *Gain*, *Noise* (noise-layer level), *Attack*, *Release*
- *Speed* — envelope playback rate; 0 freezes the spectrum (drone), 8x chirps
- *Blur* — lags the envelopes (percussive slice -> swelling pad)
- *Tilt* — spectral tilt in dB/oct, pivoted at partial 8 (dark <-> bright)
- *Odd/Even* — harmonic balance (-1 evens only, +1 odds only)
- *Stretch* — extra inharmonicity (piano -> bell -> metallic)
- *Partials* — number of audible harmonics (organ/lo-fi at low counts)
- *Drift* — slow random per-partial shimmer (0.1-1.6 Hz LFOs)
- *Pitch Env* — scales the analyzed pitch track (0 = flat/pure, 2 = exaggerated)
- *Spread X/Y* + *Spread N* — polyphonic morph spread: each new voice gets an
  index (0..N-1, cycling, reset when all notes stop) and plays at
  cursor + index*spread, wrapped mod 1 around the pad. Chords fan out across
  the morph field; orange ghost cursors on the pad show where each voice sits.
- *Width* — stereo: each partial gets a fixed pan position (lows centered,
  highs spread, constant-power) plus ~±2 cents of opposite L/R micro-detune
  for slowly evolving decorrelation; noise bands alternate pan lean.
  0 = exact mono. Fully mono-compatible (sums back cleanly).
- *Loop Pos* / *Loop Len* — sustain loop: while a key is held, the envelope
  cycles the region [pos, pos+len]; release plays out past the loop
  naturally. Len 0 = one-shot (default). Click-free (phase is continuous).
- *Bend* + the one-octave keyboard under the pad — pitch-envelope scale
  quantizer. Toggle pitch classes on the keyboard; the sounding pitch
  (played note x analyzed pitch track) snaps to the nearest enabled note in
  any octave, gliding over *Bend* seconds. All keys off = bypass. Crank
  *Pitch Env* to exaggerate the track and the quantizer turns wobbly slices
  into melodies/arpeggios in key.

Voice engine: 8 voices × 64 phasor-rotation sine oscillators (drift-corrected
complex multiply — no per-sample `sin()` calls) + 24 constant-Q bandpass
noise filters. All pad models are pre-resampled onto one common control grid
(precomputed log envelopes) whenever the pad changes, so per-control-frame
blending is just weighted sums + exp. The envelope matrix plays through once
per note (one-shot); note-off applies a fade of `Release` seconds.

## Finder Quick Action

Right-click any audio file in Finder -> Quick Actions -> **Convert to
AddSynth Model**: runs the tonal-slice importer on it (up to 5 models for
long files) into `models/`, with previews in `models/previews/`, and posts
a notification when done. Log: `~/Library/Logs/addsynth-quickaction.log`.

Installed at `~/Library/Services/Convert to AddSynth Model.workflow`
(a copy lives in `tools/`; to reinstall: `cp -R "tools/Convert to AddSynth
Model.workflow" ~/Library/Services/`). Works on multi-selections too.

## Evolution tools

**GA against a reference** — evolve a model's timbre toward any audio file
(fitness = multi-scale log-mel distance, genome = tilt/odd-even/8 group
gains/inharmonicity/time-stretch/noise scale):

```bash
.venv/bin/python -m additive.cli evolve reference.wav models/base.addm --gens 20
```

**Interactive breeding** — you are the fitness function:

```bash
.venv/bin/python -m additive.cli breed start models/base.addm -o breeding/s1
# listen to breeding/s1/gen00/*.wav, pick two favourites (e.g. 2 and 5):
.venv/bin/python -m additive.cli breed next 2 5 -o breeding/s1
# repeat until happy, then export candidate N to models/:
.venv/bin/python -m additive.cli breed pick 3 -o breeding/s1
```

Each generation keeps the two parents as candidates 00/01 (lineages never
regress); children are morphs at random depths plus small mutations.

## Ideas / next steps

- wavetable export (any frame's partial amplitudes = one wavetable cycle)
- model browser in the plugin
