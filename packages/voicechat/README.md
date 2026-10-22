# voicechat

A spoken conversation with a local Qwen3 on this machine's GPU: the microphone's utterance goes to Parakeet (`speechtotext`),
the text to Qwen3 (`llmrun`) with thinking off and a system line that it is speaking aloud, each finished sentence of the
reply to Kokoro (`texttospeech`) while Qwen goes on writing, and the speech to the speaker's queue (`voiceaudio`), which
speech from the person cuts off. `llmchat --voice` (in `llmrun/examples`) is the program; `voicechat.run` is the library.

```
src/voicechat.cone   overview and imports
src/session.cone     VoiceOptions, run (the models, the loop, the timing lines), headTokens/tailTokens
src/sentences.cone   Sentences (a reply cut into sentences as it arrives), speechText, trimPads
tests/sentences.cone, tests/trim.cone   the text and sample helpers (CPU only)
tests/turn.cone      the user turn put in the cache in two parts is the template's ids (needs the 1.7B's vocabulary)
tests/pipeline.cone  the whole loop on SDL's disk audio driver, with a round trip through Parakeet (needs the models)
```

## Run it

```
C:\src\llmchat.cmd voice            the 8B (headset advised: the laptop's speakers feed its microphone)
C:\src\llmchat.cmd voice small      the 1.7B (answers about 0.5 s sooner)
```

Speak; the reply is spoken as it is written. Speak over the reply and it stops at once and listens. Ctrl-C ends it.
Options after `voice`: `--voice-name af_heart` (any Kokoro voice, or a blend `af_heart:0.7+bf_emma:0.3`; the launcher wants `+`
where the program takes a comma, because a `.cmd` splits at commas), `--speed 1.1`, `--threshold-scale 1` (with a headset),
`--hangover 450` (the silence that ends your turn, ms), `--seconds n`, `--max-tokens n`, `--no-timings`, `--no-warm`. The
full list is the header of `examples/llmchat.cone`.

No sound unless asked: `--audio system` (the default, and the only one that makes sound) or `--audio disk in.raw out.raw`
(SDL's disk driver: the microphone is a raw f32 file at 44.1 kHz mono, the speaker is written to one at 44.1 kHz stereo) or
`--audio dummy` (silence in, nothing out). `packages/voiceaudio/README.md` says how the driver is named and checked.

## How a turn goes

1. **Listen.** `Microphone.pump` feeds `Vad`; an utterance ends after `hangoverMs` (600) of silence. While the person is
   talking, a little work (Parakeet on 0.5 s of silence, every 150 ms) keeps the GPU's clock up.
2. **Hear.** `Parakeet.transcribe` on the utterance (pre-roll and trail included); a transcript with no letter or digit is
   ignored.
3. **Think.** The cache always ends with the opening of a user turn (the reply's closing, `<|im_start|>`, `user\n`), put there
   while nobody was waiting; the words and the turn's end (`<|im_end|>`, the assistant's turn with its empty thinking block)
   are run now. `tests/turn.cone` checks that the two parts are the ids `ModelTokens.userTurn` gives.
4. **Answer.** Each token is sampled and printed; `Sentences` hands back each sentence the moment it is certain to be over;
   `speechText` cleans it (markdown, emoji, curly quotes); Kokoro makes the samples; `trimPads` cuts the silence Kokoro puts at
   both ends to 80 ms before and 150 ms after; they go to the speaker's queue. Between tokens the microphone is read.
5. **Barge-in.** Speech while a reply is being written or played cuts the speaker's queue at once and stops the writing. The
   engine's cache is wound back to the end of the last sentence whose sound had begun to play, so the model remembers the
   sentences you began to hear and not the rest.

While the speaker has sound waiting (and for 300 ms after) the detector's thresholds are multiplied by `thresholdScale` (3).

## Measured

All on SDL's disk driver (no sound), RTX 4060 Laptop, release build; the GPU was shared with two other lanes' test runs for
part of the time (said where it matters). Times are from the end of the speech (where the detector put it) to the first
sample above 0.002 rms in the speaker's file, both on the disk device's own clock.

| | question | 8B Q4_K_M | 1.7B Q8_0 |
| --- | --- | --- | --- |
| turn 1 | "What is the capital of France?" (reply: one short sentence) | 2.02, 1.81 s | 1.37, 1.37 s |
| turn 2 | "How tall is Mount Everest?" | 1.97, 2.09 s | 1.79, 1.81 s |
| turn 3 | "Why is the sky blue?" (first sentence 12 words) | 2.10, 2.08 s | 1.50, 1.55 s |

(two runs each.) The target of about 1 s is not met; the 1.7B on a short reply is 1.4 s. Where it goes, in turn 1 (the
turns after it are dated by the disk device running about 0.8 % slow against the wall clock, which inflates "silence until
detected" by 0.15 s at 20 s and 0.3 s at 36 s; the file times above do not have that error):

| stage | 8B | 1.7B |
| --- | --- | --- |
| silence until the detector ends the utterance (600 ms hangover + one frame + the read) | 0.66-0.71 s | 0.66 s |
| Parakeet | 0.11-0.12 s | 0.12-0.13 s |
| the words and the turn's end into Qwen's cache | 0.43-0.44 s | 0.09-0.10 s |
| Qwen writes the first sentence | 0.21-0.22 s | 0.09-0.10 s |
| Kokoro on it (2.3 s of speech) | 0.37-0.51 s | 0.35 s |
| queue and the device's first buffer | 0.03 s | 0.03 s |

The hangover dominates. `--hangover 450` took the 8B's silence to 0.50 s (end to end 1.83 s, 1.83 s, 1.94 s over the three
turns, against 1.8 to 2.1 at 600): the detector's tests pin that a 0.4 s pause inside an utterance does not split it, so 450
is the lowest value they support; 350 was tried once and splits any pause longer than 0.35 s, which the tests do not cover.
Kokoro, and the 8B's prompt (a chunk of about 20 tokens costs 0.43 s on the matrix-matrix path, as much as single-token
steps would), are the next largest.

Kokoro puts about 0.4 s of silence before a sentence and 0.5 to 0.7 s after it (20 sentences measured); `trimPads` took
0.2 s off the time to first sound and takes the second-long pauses out between sentences.

GPU memory with all three models resident (nvidia-smi on an otherwise idle GPU, context 4096): 8B peak 7324 MiB of 8188
(7.15 GiB), 1.7B peak 4407 MiB (4.30 GiB). Parakeet's weights are 1178 MiB, Kokoro about 500 MiB.

**One thread.** A 90-token reply of the 8B took 5.5 s to write and speak into the queue, 2.8 s of it Kokoro's: about 16 tokens
a second with Kokoro interleaved against about 34 without, and about 5 are needed to keep ahead of speech, so the only cost is
before the first sound. The barge-in is noticed between tokens and between sentences, so Kokoro's 0.35 to 0.65 s per sentence is
the worst-case delay; measured over five runs, 0.16 to 0.54 s from the detector's first speech to the cut call, and the sound
in the speaker's file ends within 0.1 s of the call.

**Barge-in** (`barge3.raw`: a story request, then a second voice at 9.3 s, in the middle of the first sentence): the speaker's file holds
sound up to 10.04 s and then silence; the cut was called at 10.11 s, 0.39 s after the detector put the speech's start at
9.72 s; the model keeps the first sentence (18 of the 54 tokens written) and answers "The dragon's name was Drake." to
"Wait, what was the dragon name?"

**Threshold scale.** Replaying a reply's own sound into the microphone file at a fraction of its level (the 1.7B, reply 5.1 s
to 6.7 s): with the scale at 1 a bleed of 0.06 of the speaker's level (-24 dB) already cuts the reply; at 2, 0.06 is passed
and 0.1 is not; at 3, 0.1 (-20 dB) is passed and 0.15 is not; at 4, 0.1 is passed. The other side, a second voice (Kokoro's, rms about 0.05 at full level)
laid in at 0.25 of that (-12 dB, rms 0.012) still cuts the reply at scale 3; at 0.12 (-18 dB, rms 0.006) it does not. So at 3
the detector ignores a bleed up to about -20 dB of the speaker's level and hears a talker down to about -12 dB of that same
reference: a margin of 8 dB, where at scale 1 it would hear both. On a real device the bleed is whatever the speakers and the
room make it: a headset makes it nothing; start with 3 and lower it if interruptions are missed.

**Round trip.** The replies of the 8B run (three turns, 5 sentences, 44 words) transcribed by Parakeet from the speaker's
file come back with 0 word errors; so do the 1.7B's (55 words; "(8,848 meters)" came back as "8,848 meters," with commas).

## What was not run

Nothing here opened a real microphone or speaker. For the morning test with a device: the detector's levels against a real
microphone's noise and gain; the speaker's cut-off delay (`Driver.system(480)` asks for 20 ms; SDL may choose another); the
bleed of the laptop's speakers into its microphone and the right `--threshold-scale`; Kokoro's voices by ear.
