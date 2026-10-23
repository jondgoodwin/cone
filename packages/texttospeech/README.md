# texttospeech

Kokoro 82M text to speech (hexgrad's StyleTTS2 / iSTFTNet model, Apache 2.0) on the GPU, in Cone. English text goes in,
24 kHz mono samples come out, and `examples/texttospeech.cone` writes them to a WAV file. Nothing here plays sound.

```cone
imm speaker = Speaker.open(&mut device, "C:/src/speech-models/kokoro", false)?;      // true: British lexicon
imm voice = Voice.load("C:/src/speech-models/kokoro", "af_heart:0.7,bf_emma:0.3")?;
imm samples = speaker.speak(&mut device, "Hello world.", &voice, 1.0)?;               // List[f32], 24000 a second
```

What it is made of: `englishphonemes` (text to Kokoro's phonemes: misaki's rules, no espeak), `speechkernels` (the model's
layers as Slang kernels) with `llmkernels`' matrix product, `gpuwork` (jobs and buffers), `safetensors` (the weights).
The harmonic source and the short-time Fourier transforms are on the CPU (`source.cone`).

## Files it needs (outside the repository)

The converter (`C:\src\speech-models\convert\convert_kokoro.py`, not committed) reads hexgrad's `kokoro-v1_0.pth` and
`voices/*.pt` and writes, to `C:\src\speech-models\kokoro\`: `kokoro-v1_0.safetensors` (weight-norm pairs fused, each
LSTM's two biases added), `vocab.tsv`, `voices\<name>.safetensors` (the American and British voices) and misaki's four
lexicons as `.tsv`. Tests and the example skip or report when the files are absent.

## Voice controls

- **Voice**: any shipped voice by name: `af_heart`, `af_bella`, `af_nicole`, `af_sarah`, `af_sky` ... `am_michael`,
  `am_adam` ... `bf_emma`, `bf_isabella` ... `bm_george`, `bm_lewis` ... (a name starting with `b` is British: open the
  `Speaker` with `british = true` so the lexicon agrees).
- **Blends**: `Voice.load(dir, "af_heart:0.7,bf_emma:0.3")`: every row of the two voices' style tables mixed with those
  weights (divided by their sum, so `7` and `3` do the same); no weights is the plain mean, as Kokoro's own
  `af_bella,af_jessica`.
- **Speed**: `speak(..., speed)`: 1.0 normal, more is faster (the phonemes' durations are divided by it).
- **Seed**: `speaker.seed(n)`: the source's random numbers (a random starting phase of each harmonic and noise on every
  sample): the same text, voice, speed and seed give the same samples.

## Checks (tests/)

- `model`: PyTorch Kokoro's stage outputs for three cases (`dump_reference.py`, run outside the repository), compared
  stage by stage: ALBERT, the duration encoder and its LSTM, the text encoder, the durations, the pitch and energy
  predictors, the decoder, each generator stage, the last convolution, and the audio. Given PyTorch's pitch for the
  source the audio has a signal-to-noise ratio of 71 to 79 dB against PyTorch's. Free-running (its own durations, pitch and
  energy drive its own source) it is 34 to 48 dB, and the mean absolute difference of the log-mel spectrograms is 0.003 to
  0.019 nats. The source's phase is a running sum of the pitch times 2 pi times 300 (up to 8e4 radians), so a pitch error
  of one part in a million is a fraction of a radian there, and a sample-by-sample comparison cannot be better.
- `source`: the CPU source, STFT and inverse STFT each fed PyTorch's own input: source 3e-8, STFT magnitudes 1.2e-7,
  phases 5e-5 (the only difference in 3 cases is a phase of +pi against -pi in the first, symmetric frame, whose sign is
  rounding noise), inverse STFT 3.6e-7.
- `voice`: blends. `speaker`: the whole path (determinism, speed, blends, a long text in pieces, the WAV, a British voice).

## Speed and memory

RTX 4060 Laptop, release build: a sentence of 4 s of audio takes 0.45 s (about 0.35 s on the GPU, 0.06 s recording the
jobs, 0.05 s on the CPU for the source); 63 s of audio in 4.5 s (real-time factor 0.07). The model on the GPU takes about
500 MiB (327 MiB of weights, the rest scratch buffers that layers reuse).
