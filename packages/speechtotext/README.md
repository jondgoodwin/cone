# speechtotext

NVIDIA's [Parakeet TDT 0.6B v2](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v2) (English, CC-BY-4.0) speech recognition on
the GPU, through `gpuwork` and `llmkernels`: 16 kHz mono samples in, text with capitals and punctuation out. Needs a Vulkan 1.3
device made with `ComputeRequest(largeBuffers: true)`; Windows only for now (it stands on `gpu`). The source header
(`src/speechtotext.cone`) is the guide: the model, what it is checked against and the numbers, the speed and where the time
goes, each method chosen and what was left out.

```
congo run --release examples/speechtotext.cone -- clip.wav [more.wav ...] [--model <file.safetensors>] [--vocab <file.txt>] [--timings]
```

reads WAV files (any rate, channels and sample format `audiomixer` reads; converted to 16 kHz mono) and prints each text. It
plays and records nothing. The defaults are `C:\src\speech-models\parakeet-tdt-0.6b-v2.f16.safetensors` and `...vocab.txt`.

## The model files

The package reads the weights from a safetensors file (package `safetensors`) and the 1024 SentencePiece pieces from a text file,
both written from NVIDIA's `.nemo` checkpoint by a converter that is **not in this repository**. It folds the convolution
module's batch norm into the depthwise convolution, stores the 1x1 convolutions as matrices and adds each LSTM layer's two
biases; the `f16` file also rounds every large matrix to a half (1.2 GB against 2.5 GB). Tensor names are the ones
`src/model.cone` asks for. Tests that need the files (`tests/model.cone`, `tests/frontend.cone`) are skipped where
`C:\src\speech-models` does not have them.

## API

```
Parakeet.open(&mut device, &modelPath, &vocabPath)  -> Result[Parakeet, SpeechToTextError]
p.transcribe(&mut device, samples)                  -> Result[Transcript, SpeechToTextError]   // text, token ids, frames, timings
p.run(&mut device, samples, trace)                  -> Result[Run, SpeechToTextError]          // with the features, the encoder's
                                                                                    // intermediate outputs and the decode's steps
resampleTo16k(samples, rate), soundTo16k(&sound)    -> List[f32]
```

## Tests

`congo test` in this folder: `safetensors` is its own package; here `frontend` (log-mel against NeMo's), `kernels` and `matmul`
(each GPU kernel against its CPU twin), `model` (the whole recogniser against NeMo's text, tokens, decode steps and encoder
outputs on 16 LibriSpeech clips, with F32 and F16 weights) and `resample`. `examples/bench.cone` times the matrix product.
