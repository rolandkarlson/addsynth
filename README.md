# AddSynth — additive resynthesis engine

by Roland Karlson · Algorytmid

## Sine lab experiment (`codex/sine-playground`)

This branch builds **AddSynth Sine Lab**, with its own AU/VST3 identity and
standalone app. It is a pure sine instrument: the analyzed noise layer and
the legacy Noise parameter are disabled here.

Load models, then press **glitch** or **ambient** in the new sine lab panel
and play some notes. These starting points replace the eight sequence
patterns and set the lab/timbre controls; the loaded models stay in place.
Use the top-right view menu to switch between **morph pad**, **sine lab**, and
**spectral planes**.

### Spectral Planes

Select **spectral planes**, load a model, and press **try planes**. This
starting point combines an orbit through two built-in spectral planes with
the ambient timelines and memory settings. Sequence playback is disabled
so the plane movement is easy to hear.

- **Orbit Hz** controls movement within the plane (0 holds its position,
  up to 4 cycles/second). **Ellipse / circle** changes the orbit shape.
- **Plane A / B** changes the plane itself. **Depth** mixes the resulting
  attenuation mask with the unmodified signal; 0 is transparent.
- **Capture A / B** each builds a whole plane from two moments of the
  currently blended models, including the base partial mask. **Capture
  position** chooses the first moment and **capture gap** their separation,
  as fractions of the common model timeline. If there is no room ahead,
  the second moment is taken earlier. No note needs to be sounding.
- Change the morph-pad position or capture controls before capturing the
  other plane. The captured basis vectors and labels save with the session.
  Static/proportional spectra receive a deterministic independent companion,
  explicitly reported as **companion added**. Silent captures leave the
  existing plane intact and display a message.

Each plane has two orthonormal vectors in 192-dimensional partial space.
Morphing aligns equivalent bases (including sign changes and reflections),
blends them, then reorthogonalises. This is a continuous subspace morph,
not an exact Grassmannian geodesic. Orbital coordinates are mapped through
absolute value and peak normalisation into an attenuation-only mask; this
mapping has half-turn symmetry. The mask is smoothed over 20 ms and multiplies
the base mask and sequence on both live notes and memory layers, before
the existing 4x render/anti-alias filter. The live graph displays that mask.
Earlier sessions default to planes bypassed. The captured data persists,
but the live orbit phase restarts when playback is prepared.

- **Sequence**: eight independent partial masks. Select **1–8** and draw in
  the bar editor below; **base** edits the global mask, which multiplies all
  steps. The dot under a step indicates playback, independently of which
  step you are editing. **Reset step** restores only the selected pattern.
  Note lengths run from 1/64 to one bar (four quarter-note beats), following
  the host's tempo and playing position, or 120 bpm when free-running.
  **Step glide** crossfades from the previous mask over a fraction of the
  step. Patterns and controls save with the session; the lab controls and
  base mask are automatable, while pattern drawing is saved as custom state.
- **Timelines**: each partial scans the model amplitude envelope separately.
  **Speed spread** separates rates by up to three octaves in each direction;
  **start scatter** offsets their positions; **reverse** sends a proportion
  backward. Playheads bounce at the model boundaries while a note is held.
  Pitch stays controlled by the existing pitch engine. Modulation destination
  5 is now **time spread** in this experimental instrument.
- **Memory**: releasing a note captures its current sine spectrum and pitch
  into one of four fading layers. **Memory** mixes them in, **fade seconds**
  sets the time to decay by 60 dB, and **clear memory** gently clears them.
  Replaced layers fade before their oscillator state changes. Memories follow
  the base mask and sequence; live tail contents are not stored in sessions.

The sine bank renders at 4× the host sample rate. Partial amplitudes fade
between 40% and 45% of the output sample rate (including stereo detuning),
followed by a 513-tap linear-phase low-pass filter before downsampling. The
plugin reports its 64-sample latency to the host. This deliberately trades
some very high frequency output and CPU for clean spectral movement; it is
not a mathematical claim that arbitrary modulation has zero alias energy.
Note attacks have a minimum 3 ms transition, note release edges are smooth,
and master-gain changes are smoothed before the output filter.

Validation: `ctest --test-dir plugin/build --output-on-failure` covers pattern
editing/recall, old-session defaults, timeline playback, memory release/clear,
high notes, filter latency, and >97 dB rejection of the tested out-of-band
tones at 32, 44.1, 48 and 96 kHz. The general instrument description below
also documents features from the main branch, including its noise layer.

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
**AddSynth Sine Lab** (Algorytmid) on this branch.

**Morph pad**: *Add models...* (multi-select .addm files, up to 16), drag
the green nodes to place timbres on the XY field, drag the white circle to
morph — blending is inverse-distance-weighted, log-domain, live while notes
play. Double-click a node to remove it. Cursor position = automatable
`Morph X` / `Morph Y` parameters, so you can automate timbre sweeps from
Ableton. Pad layout is saved with the session.

**Partial levels**: draw across the bars to attenuate individual partials
for every model on the pad. Full height = unchanged (100%), bottom = mute;
the curve multiplies the models' evolving amplitudes and also shapes held
notes and modal tails. Fast strokes fill in every crossed bar. Use the range
menu to zoom into groups of 64, double-click a bar to restore it, or use
*reset all* to restore all 192 levels. The curve is saved with the session,
each partial level is automatable, and the noise layer has its own *Noise*
control.

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
  naturally. Len 0 = one-shot (default). Click-free (phase is continuous),
  and a region running past the envelope end wraps around through the start.
- **mod matrix** (bottom strip) — 6 slots of source -> amount -> destination.
  Sources: lfo 1/2 (rate knobs beside the slots), env pos (where the note is
  in the model's timeline — the model modulates itself), voice idx
  (generalized spread), random-per-note, velocity, note (key tracking).
  Destinations: morph x/y, tilt, blur, speed, noise, width, stretch,
  odd/even, partials, pitch, loop pos. Everything is applied at control
  rate through the smoothed additive engine — partials above Nyquist stay
  muted, so modulation can't alias.
- *Attack* / *Decay* / *Sustain* / *Release* — an ADSR on the MODEL (the
  exciter). Decay 0 = no decay stage (default, model plays as analyzed);
  with Decay > 0 the level falls to Sustain over Decay seconds.
- **modal** checkbox + *Ring* / *Damp* — resonator mode using the same
  oscillator bank: the model x ADSR excites per-partial amplitudes that
  ring down on their own (Ring = base decay seconds), higher partials
  damped faster (Damp; 0 = metallic, 1 = woody). The ring is independent
  of the ADSR: a short strike (short decay/release, sustain 0) still
  rings out fully, and the voice ends only when the ring has died.
  Multi-hit models become struck instruments.
- *loop sync* checkbox — Loop Len picks musical lengths (1/4..16 beats) at
  the host tempo, compensated for Speed so the audible loop stays on grid.
- *speed sync* checkbox — Speed means "scan the whole envelope in N bars"
  (1/4..8, quantized). Both use the DAW's reported tempo (120 standalone).
- *Bend* + the one-octave keyboard under the pad — pitch-envelope scale
  quantizer. Toggle pitch classes on the keyboard; the sounding pitch
  (played note x analyzed pitch track) snaps to the nearest enabled note in
  any octave, gliding over *Bend* seconds. All keys off = bypass. Crank
  *Pitch Env* to exaggerate the track and the quantizer turns wobbly slices
  into melodies/arpeggios in key.

Voice engine: 8 voices × up to 192 phasor-rotation sine oscillators (drift-corrected
complex multiply — no per-sample `sin()` calls) + 24 constant-Q bandpass
noise filters. All pad models are pre-resampled onto one common control grid
(precomputed log envelopes) whenever the pad changes, so per-control-frame
blending is just weighted sums + exp. The envelope matrix plays through once
per note (one-shot); note-off applies a fade of `Release` seconds.

## Finder Quick Action

Right-click any audio file in Finder -> Quick Actions -> **Convert to
AddSynth Model**: runs the tonal-slice importer on it (up to 5 models for
long files) into `models/`, with previews in `models/previews/`, and posts
a notification when done, and auto-commits + pushes the new models to git
(with [skip ci]). Log: `~/Library/Logs/addsynth-quickaction.log`.

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
