# audiomixer (a working name)

Sound for a 3-D world: a real-time mixer, positional audio and procedural
synthesis, written in Cone with no C. Its output device is a separate package,
`audiodevice` (also a working name), over SDL3, so nothing here links SDL and
every test runs headless.

```
src/audiomixer.cone  overview, scalar helpers (dB, semitones, clamp)
src/dsp.cone         PCG random numbers, pink and brown noise, PolyBLEP oscillators,
                     biquad filters (RBJ cookbook), one-pole, ADSR, ramps
src/wav.cone         WAV decode (PCM 8/16/24/32, float 32/64, extensible) and encode
src/spatial.cone     listener, attenuation models, pan law, head shadow, Doppler, occlusion
src/synth.cone       the recipes: wind, brook, fire, footsteps (grass, wood, stone), bird
src/mixer.cone       voices, buses, fades, priority stealing, block renderer, Cmd
src/engine.cone      Control (game thread) and Engine (audio thread) over a lock-free queue
src/analysis.cone    rms, zero-crossing frequency, FFT, spectrum, centroid (for tests)
```

## Model

* **Game thread** holds a `Control`. `play` answers a voice id at once and
  sends a `Cmd` (80 bytes, plain data) through sync's single-producer ring
  (`Channel.spsc`). A full ring refuses and counts (`Control.refused`); the game
  never waits for audio.
* **Audio thread** holds an `Engine`: at the start of each block it takes up to
  512 waiting commands, applies them, then renders. `Mixer.render(out, frames)`
  fills interleaved stereo f32.
* **Voices** (fixed count, made up front) play a sound from the table or a
  procedural `Synth`, through gain (a straight-line ramp, so fades and gain
  changes do not click), pitch, pan or 3-D placement, into a bus (music, effects,
  ambience); the buses sum through their own gains into the master. Output is
  hard-limited to -1..1.
* **Voice limiting**: when all voices are busy a newcomer takes the voice with the
  lowest priority, the quietest of those, the oldest of those; if every voice has
  a higher priority than the newcomer it is dropped. `Mixer.stolen` and
  `Mixer.dropped` count them.
* **3-D**: the listener (`Cmd.listener`) rides the camera. A voice made spatial
  (`setPosition`, with a velocity for Doppler) gets distance attenuation (OpenAL
  1.1 inverse, linear and exponent models, or a custom 8-point curve), the
  constant-power pan law on its bearing, a head-shadow low pass on the far ear
  (20 kHz down to 1.5 kHz on a log scale across the far half, no interaural time
  difference), OpenAL's Doppler formula, and the game's occlusion hook
  (`setOcclusion` 0..1: up to -12 dB and a low pass sliding from 20 kHz to
  400 Hz). The pan and spatial gains are interpolated across each block.

## Real-time safety, measured

* `tests/noalloc.cone` replaces the heap region's allocate and resize with
  counting wrappers and renders 600 blocks of 32 voices (samples, 3-D sources,
  every recipe) while commands arrive, voices are stolen and stopped: **zero
  allocations**. (The heap region is what every collection allocates from;
  the spsc ring allocates only when made.) There are no locks: the only shared
  state is the ring.
* `examples/bench.cone`, release build on the author's i7-13700HX: one 10 ms block
  (480 frames at 48 kHz) of 32 voices costs about **0.19 ms for plain sample
  voices** (1.9 % of real time), **0.25 ms for fully spatial ones** (2.5 %) and
  **0.87 ms for 32 procedural voices** (8.7 %); the worst block of 2000 was 3 ms,
  well under the 10 ms deadline.

## Procedural recipes

`examples/recipes.cone` renders every recipe and writes WAV files into
`build/recipes/` (the package's ignored build folder), printing a spectral
description. What they sound like, from the spectrum (44.1 kHz):

| recipe | rms | strongest | centroid | what it is |
| --- | --- | --- | --- | --- |
| wind, breeze | 0.024 | 266 Hz | 502 Hz | a low, soft whoosh; 76 % of the power between 250 Hz and 2 kHz, 22 % below 250 Hz |
| wind, gale | 0.087 | 896 Hz | 914 Hz | louder and higher, a howl: 91 % in 250 Hz to 2 kHz (the narrow resonance moves with the gust) |
| brook, trickle | 0.054 | 511 Hz | 873 Hz | a quiet bed of mid noise with sparse rising bubbles |
| brook, stream | 0.094 | 476 Hz | 1612 Hz | the same, busier: 6 % of power above 5 kHz (the hiss and the bubbles) |
| campfire | 0.052 | 54 Hz | 2170 Hz | a deep rumble (68 % under 250 Hz) with sharp crackles: 28 % of power above 2 kHz, crest factor over 15 |
| burner | 0.116 | 942 Hz | 3791 Hz | a steady roar: 60 % in 250 Hz to 2 kHz, 21 % above 5 kHz hiss |
| step, grass | 0.039 | 3.1 kHz | 4115 Hz | a soft hiss with crunches: 78 % above 2 kHz, 0.35 s |
| step, wood | 0.031 | 196 Hz | 238 Hz | a hollow knock, 82 % under 250 Hz, ringing 0.4 s |
| step, stone | 0.044 | 1225 Hz | 1009 Hz | a short tick with a ringing at 1.2 kHz, 75 % in the middle, 0.3 s |
| bird | 0.23 | 4021 Hz | 4289 Hz | a short whistle of a few swept syllables, 95 % in 2 to 5 kHz, 0.39 s |

Each recipe is the structure Andy Farnell describes in *Designing Sound*,
with this package's own constants, tuned by spectrum rather than by ear. They
are a starting point for a game's sound designer, who will want to listen and
adjust the constants in `synth.cone`; the tests pin the properties (where the
power sits, crest factor, ringing frequency, ending) so a retune that breaks a
recipe is caught.

## Method choices (all published algorithms, named in the code)

Biquads: Bristow-Johnson's Audio EQ Cookbook, transposed direct form II.
Oscillators: PolyBLEP (Valimaki and Huovilainen 2007). Pink noise: Paul Kellet's
refined filter. Random numbers: PCG (O'Neill). Attenuation and Doppler: OpenAL 1.1
specification. Pan: constant-power sin/cos law (as the Web Audio StereoPannerNode).
Head shadow: a one-pole low pass whose corner follows the bearing, a cheap relative
of Brown and Duda's spherical-head model. Resampling: linear interpolation. Voice
stealing: lowest priority, then quietest, then oldest (FMOD/Wwise style).
Packaging: SDL3 audio push stream fed by a thread (no C callback).

## What is not here

* **Ogg Vorbis and Opus decoding.** Opus would take an Ogg page parser and an
  RFC 6716 decoder (a range decoder, SILK and CELT with an MDCT and a postfilter),
  roughly 10,000 lines of C in the reference; Vorbis perhaps 2,500 lines (codebooks,
  floor 1, residue, MDCT). A port to Cone is a library of its own. In the browser
  the platform decodes them (below).
* A full HRTF (measured head-related transfer functions): the head shadow here is
  a cheap stand-in. Reverb zones and simulated propagation (the design note's
  later items), a streaming music player (sounds are whole in memory; a long
  track wants a streaming source), pitch and Doppler for procedural voices
  (a `Synth` runs at its own fixed pitch), cubic or sinc resampling.

## Browser integration needs

Not tried: nothing here has been compiled for WebAssembly.

1. **The mixer core is portable as written**: `Mixer`, `Synth`, `Cmd`, the DSP and
   spatial code are plain arithmetic over `libc` math, no threads, no OS. A browser
   build runs a `Mixer` inside an `AudioWorkletProcessor`, whose `process()` is
   asked for 128 frames at a time (`Mixer.render` accepts any frame count).
   It needs the Cone wasm target to export `Mixer.make`, `apply` and `render` as
   plain functions (and a way to give JavaScript the address of the output buffer
   and the 80-byte `Cmd`).
2. **Commands cross a thread boundary as data.** Either the worklet's wasm instance
   shares memory (`SharedArrayBuffer`, which needs cross-origin isolation headers
   and atomics support in the Cone wasm target) and `Engine` runs unchanged on the
   ring `sync` provides, or, simpler, the main thread `postMessage`s each `Cmd`
   (16 numbers and two addresses) to the worklet, which calls `Mixer.apply`. The
   `Control` API is the same either way; only `commandQueue` changes.
3. **Starting**: an `AudioContext` starts only after a user gesture; the page must
   call `audioContext.resume()` from a click or key handler, and until then the
   game's `Control` should queue (or drop) commands.
4. **Sounds**: `decodeAudioData` yields float PCM for any format the browser
   plays, including Ogg and Opus; copy its channels into wasm memory as a
   `Sound` (interleaved f32) and `addSound`. WAV can be decoded by `decodeWav`
   as on desktop. Procedural recipes need no assets, which suits a web delivery
   ("ship the program, not the data"). The `Sound` buffer must stay put while
   in the table (the same rule as native: `Control` owns it).
5. **Output rate**: the context's sample rate (44.1 or 48 kHz) is given to
   `Mixer.make`; synth recipes and filters are rate independent.
6. The alternative for the browser is Web Audio's own graph (`PannerNode` with
   HRTF); that would replace the spatial half of this package with calls out to
   JavaScript, gaining a real HRTF and losing determinism and the shared
   procedural recipes.
