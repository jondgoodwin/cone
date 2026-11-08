# voiceaudio (a working name)

The microphone and speaker of a spoken conversation, over SDL3's audio streams, with voice activity detection.
It is the audio end of a voice chat with a local model: capture mono f32 at 16 kHz for speech recognition, play
mono f32 at 24 kHz for speech synthesis through a queue that can be cut off at once (barge-in), and decide when
the speaker has finished a sentence.

```
src/voiceaudio.cone  overview and imports
src/device.cone      Driver, Microphone, Speaker
src/vad.cone         VadConfig, Vad, Utterance
src/interrupt.cone   Interrupt: Ctrl-C for a loop of the program's own (SDL's audio swallows it; the header says why)
src/files.cone       raw f32 and WAV files, linear resampling, channel and span helpers
src/signals.cone     noise, a speech-like voice and a tone with known edges (for tests)
examples/echo.cone   listen, print each utterance, play it back, barge in (needs a named driver)
```

## Dependencies

`sdl` (the audio stream calls), `audiomixer` (its WAV decoder, `Rng` and `TAU`), `fs`, `path`, `thread`, `time`,
`collections`, `libc`. Windows alone for now, because `sdl` is.

## NO SOUND UNLESS ASKED: the driver is always named

Every `Microphone.open` and `Speaker.open` takes a `Driver` built by the caller; there is no default.

| constructor | what it is |
| --- | --- |
| `Driver.disk(inFile, outFile, timescale)` | SDL's `disk` driver: the microphone reads a raw f32 file, the speaker writes one. No device. |
| `Driver.dummy()` | SDL's `dummy` driver: audio goes nowhere, the microphone is silence. No device. |
| `Driver.system(deviceFrames)` | the machine's real devices. The only one that makes a sound or listens. |

`open` sets SDL's driver hint to what the Driver names, starts SDL's audio, and asks SDL which driver it chose
BEFORE it opens any device. A mismatch (SDL's audio was already running on another driver) returns an
`AudioError` with reason 2 and opens nothing; `tests/diskmic.cone` pins that. Every test and the example's `disk`
and `dummy` modes name their driver; `echo` with no driver named prints its usage and stops.

The disk driver, as measured on SDL 3.4.16: its device is **44.1 kHz** whatever the stream asks for (the
`SDL_AUDIO_FREQUENCY` hint did not change it), the microphone file is **mono** f32 and the speaker file is
**stereo** f32 (every sample twice), the microphone **records silence forever after the file's end**, and the
speaker **writes silence while nothing is queued**. `timescale` ("1" real time, "0.05" twenty times faster) paces
both. A stream opened at 44.1 kHz meets the file sample for sample; at another rate SDL's converter sits between,
which is how the 16 kHz microphone and 24 kHz speaker are tested.

## Model

Nothing here runs a thread of ours. SDL fills the microphone's stream and drains the speaker's from its own audio
thread; the stream's lock makes `queue` on one thread and `cutOff` on another safe.

* `Microphone.read(out, max)` appends what has arrived (nonblocking); `pump(vad, utterances)` reads it all and
  feeds the detector. SDL converts the device's format to the stream's, so there is no resampler for devices.
* `Speaker.queue(samples)` appends; `flush()` says a sentence is complete (SDL's converter holds back a few
  samples until told); `cutOff()` throws everything waiting away at once (what the device already took, its
  buffer, still plays: `Driver.system(480)` asks for 480 frames, 20 ms at 24 kHz, and SDL may choose otherwise);
  `waiting()`, `waitingMs()` and `drain(timeoutMs)` say what is queued.
* `Vad.push(samples, out)` takes any block size (one sample, or a whole file in one call: `tests/vadpush.cone` pins
  that the utterances are the same to the sample) and appends the utterances that ended; `flush(out)` ends one
  that is in progress; `speaking()` and `speechStartSample()` let a caller react to speech starting (barge-in)
  without waiting for it to end; `setThresholdScale(k)` multiplies both thresholds while the speaker plays and
  bleeds into the microphone.

## The detector

20 ms frames; the level is the frame's rms about its own mean (a DC offset does not count). The noise floor is the
quietest frame of the last 2 s, smoothed (it falls at once, rises 2 % of the gap a frame, and while somebody is
speaking rises 0.3 % unless the whole window is flat, which a room's hum is and speech is not, then 5 %). Speech
starts after 60 ms of frames 10 dB above the floor (and above 0.004 rms, -48 dBFS) and goes on while frames are
6 dB above it; an utterance ends after 600 ms of frames below that. The first 200 ms of the stream are taken as
the room. An `Utterance` carries the samples (300 ms before the speech, 200 ms after), where the speech began and
ended in the input's sample count, and `forced` when it was cut at 30 s or by `flush`. Defaults are
`VadConfig.standard(rate)`; every number is a field.

### What was measured

All runs on disk or dummy drivers; no sound.

* Synthetic edges known by construction (`tests/vadsynth.cone`; a speech-like voice laid at chosen samples over a
  noise bed): every cut point lands exactly on the laid edge (the voice starts and ends abruptly), a 0.4 s pause
  inside an utterance does not split it and a 1.2 s pause does, a 30 ms click is dropped, noise alone and digital
  silence find nothing, a quiet talker (rms 0.012) is heard, a DC offset moves nothing, the answer does not depend
  on how the samples are blocked (1, 320 or 4097 at a time). A room that turns 15.6 dB louder is mistaken for speech
  for 2.2 s, then the floor catches up and the utterance ends (the test pins under 2.5 s).
* Real speech (`tests/vadspeech.cone`, LibriSpeech test-clean clips from `C:\src\speech-models\librispeech-clips\`,
  read only, 16 of them): the truth is where a clip was laid, and a clip carries its own quiet margins, so the
  speech inside begins up to 0.7 s after the clip's first sample. All 16 clips alone give one or two utterances (two
  clips hold a pause over 600 ms, which the hangover correctly splits: one at 3.4 to 4.2 s of a 7.8 s clip, one at
  8.7 to 9.4 s of a 15.1 s clip) inside those margins. Four short clips joined with 1.5 s gaps give four utterances,
  each on its clip; with 0.3 s gaps the hangover joins them into two; in a room of rms 0.01 (about 15 dB under the
  speech) and at a fifth of the volume still four. Measured limits, not pinned: at a tenth of the volume one clip's start was
  found 0.5 s late, and in a room of rms 0.02 (about 5 dB under the quietest clip) one clip's start was found 1.3 s
  late, since its soft onset is under the threshold; the 300 ms pre-roll does not make that up.
* Cost: 600 s of audio through the detector in 21 ms (release build; a real-time factor of about 0.00004).

The disk microphone and speaker tests (`diskmic`, `diskspeaker`) check: every sample of a file arriving unchanged
at the device's rate; the cut points through the 44.1 to 16 kHz converter; the dummy microphone being silence; the
wrong driver being refused with nothing opened; the speaker's output file holding exactly the samples queued
(44099 non-zero samples, each equal); a 24 kHz tone coming out as a 440 Hz, 0.5 loud tone of one second at 44.1 kHz;
a cut-off after 0.3 s on a real-time device leaving under 1.2 s of sound in the file (0.3 s before, the 0.5 s queued
after), with `waiting()` zero at once.

### What a real device still has to show

The cut-off is instant at the stream (`waiting()` is 0 on return) but the device keeps its buffer, so the
audible delay is the device's buffer: untested without sound. The detector's thresholds were set against files, not
a real microphone's noise, room echo or the speaker's bleed (`setThresholdScale(3)` in `echo` is a guess). A
real microphone's levels and AGC are unmeasured.

## Run it

```
congo run examples/echo.cone -- disk 14 in.raw out.raw     # no sound: a raw 44.1 kHz mono f32 file in, a file out
congo run examples/echo.cone -- dummy 5                    # no sound
congo run examples/echo.cone -- system 30                  # real microphone and speaker: MAKES SOUND
```
