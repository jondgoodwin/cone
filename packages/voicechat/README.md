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
C:\src\llmchat.cmd voice small      the 1.7B (answers about 0.4 s sooner)
```

Speak; the reply is spoken as it is written. Speak over the reply and it stops at once and listens. Ctrl-C ends it.
Options after `voice`: `--voice-name af_heart` (any Kokoro voice, or a blend `af_heart:0.7+bf_emma:0.3`; the launcher wants `+`
where the program takes a comma, because a `.cmd` splits at commas), `--speed 1.1`, `--threshold-scale 1` (with a headset),
`--end-silence-ms 450` (the silence that ends your turn; default 600), `--ctx 4096` (voice mode asks for 2048), `--seconds n`,
`--max-tokens n`, `--no-timings`, `--no-gpu-warm`. The full list is the header of `examples/llmchat.cone`.

No sound unless asked: `--audio system` (the default, and the only one that makes sound) or `--audio disk in.raw out.raw`
(SDL's disk driver: the microphone is a raw f32 file at 44.1 kHz mono, the speaker is written to one at 44.1 kHz stereo) or
`--audio dummy` (silence in, nothing out). `packages/voiceaudio/README.md` says how the driver is named and checked.

## How a turn goes

1. **Listen.** `Microphone.pump` feeds `Vad`; an utterance ends after the end-silence (`VadConfig.hangoverMs`, 600 ms).
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

**The GPU is kept warm only while somebody is speaking.** After a second or two without work the laptop GPU drops its clock,
and Parakeet's 0.1 s on an utterance became 0.3 to 0.7 s (8B run without it: 0.35, 0.44, 0.68 s; with it 0.09 to 0.12). So
while the detector is inside an utterance and the newest 20 ms frame is 6 dB or more over the noise floor (the detector's own
test for a frame of speech; false in the hangover's silence and when nobody is there), Parakeet runs on 0.25 s of silence, at
most once in 400 ms. Nothing runs while the program is idle, while the person is quiet, or during the reply. The cost, printed
after every turn: 2 to 4 runs per utterance, 0.2 to 0.8 s of GPU work during 1.3 to 1.6 s of speech (17 to 52 %; the first run
after idle takes 0.1 to 0.4 s because it is at the low clock itself, the rest about 0.1 s). `--no-gpu-warm` turns it off.

## Measured

All on SDL's disk driver (no sound), RTX 4060 Laptop, release build. The GPU was shared with the other voice lanes' test runs
at times; `nvidia-smi` showed nothing else resident during the runs below (memory used was this program's alone). Times are
from the end of the speech (where the detector put it) to the first sample above 0.002 rms in the speaker's file, both on the
disk device's own clock, for three questions in one conversation (`latC.raw`: "What is the capital of France?" with a
one-sentence reply, "How tall is Mount Everest?", "Why is the sky blue?" with a three-sentence reply).

**End-silence 600 (default), 400 and 300 ms**, seconds, turns 1 / 2 / 3:

| end-silence | 8B Q4_K_M | 1.7B Q8_0 |
| --- | --- | --- |
| 600 ms | 1.78 / 2.04 / 2.06 | 1.41 / 1.86 / 1.59 |
| 400 ms | 1.64 / 1.79 / 1.90 | 1.23 / 1.58 / 1.37 |
| 300 ms | 1.53 / 1.74 / 1.78 | 1.11 / 1.53 / 1.29 |

The target of about 1 s is met by neither model at 600 ms; the 1.7B at 300 ms on a short reply gets to 1.1 s. Where the time
goes, turn 1 at 600 ms (later turns are dated by the disk device running about 0.8 % slow against the wall clock, which adds
0.15 s to "silence until detected" at 20 s and 0.3 s at 36 s; the file times above do not have that error):

| stage | 8B | 1.7B |
| --- | --- | --- |
| silence until the detector ends the utterance (the end-silence + one frame + the read) | 0.67 s | 0.67 s |
| Parakeet | 0.12 s | 0.13 s |
| the words and the turn's end into Qwen's cache | 0.46 s | 0.12 s |
| Qwen writes the first sentence | 0.21 s | 0.10 s |
| Kokoro on it (2.3 s of speech) | 0.32 s | 0.35 s |
| queue and the device's first buffer | 0.0-0.1 s | 0.0-0.1 s |

**What a shorter end-silence costs: an utterance cut in two** when the speaker pauses longer than it inside a sentence; the
first half is then answered as if it were the whole, and the second half interrupts the answer. `voiceaudio`'s tests run at
600 ms only; they pin that a 0.4 s pause does not split (true of any end-silence above 0.4 s, by construction) and that
1.2 s does. Measured on the 16 LibriSpeech clips (read speech, which pauses at punctuation, so a harsher test than talk),
each alone in a quiet room, utterances found and clips cut in more than one:

| end-silence | 600 | 500 | 450 | 400 | 350 | 300 | 250 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| utterances from 16 clips | 22 | 24 | 25 | 27 | 28 | 31 | 31 |
| clips cut | 4 | 4 | 4 | 5 | 6 | 7 | 7 |

So 450 cuts the same four clips the default does (a few more times); 400 cuts one more clip; 300 cuts three more. The
choice is Jon's: 400 is a good first try for conversation, and `--end-silence-ms 600` goes back.

Kokoro puts about 0.4 s of silence before a sentence and 0.5 to 0.7 s after it (20 sentences measured); `trimPads` took
0.2 s off the time to first sound and takes the second-long pauses out between sentences.

**GPU memory** (nvidia-smi, GPU otherwise idle: 0 MiB resident before; the display runs on the integrated GPU), all three
models loaded:

| | peak | free of 8188 MiB |
| --- | --- | --- |
| 8B, context 4096 | 7324 MiB (7.15 GiB) | 864 MiB (10.6 %) |
| 8B, context 2048 (the voice default) | 6720 MiB (6.56 GiB) | 1468 MiB (17.9 %) |
| 1.7B, context 4096 | 4407 MiB (4.30 GiB) | 3781 MiB |

Parakeet's weights are 1178 MiB, Kokoro about 500 MiB, the 8B's cache 288 KiB a position. **If another program holds the
memory**, nothing fails: Windows lets Vulkan over-commit device memory and moves what does not fit into shared system memory.
Measured: while the 8B conversation ran (7.3 GB), a second process loaded the 1.7B voice stack (4.4 GB more) without an error
(it took 6.0 s to load, 2.7 s alone), the GPU reported 7.9 GB in use, and the 8B's turns went from 0.42 s to write a first
sentence to 7.6 s and 4.8 s, its prompt from 0.41 s to 1.0 s: about 15 times slower, because the weights were being read across
the PCIe bus. So with 864 MiB spare, any other program holding more than that (a game, a video call, a browser tab on the
discrete GPU) makes the 8B very slow until it stops; at 2048 the margin is 1.4 GiB. A spoken exchange is about 75 tokens
(the system line about 100), so 2048 positions hold about 25 exchanges, after which a new
conversation begins (printed, and the model has forgotten the old one); `--ctx 4096` doubles that.

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
and 0.1 is not; at 3, 0.1 (-20 dB) is passed and 0.15 is not; at 4, 0.1 is passed. The other side, a second voice (Kokoro's,
rms about 0.05 at full level) laid in at 0.25 of that (-12 dB, rms 0.012) still cuts the reply at scale 3; at 0.12 (-18 dB,
rms 0.006) it does not. So at 3 the detector ignores a bleed up to about -20 dB of the speaker's level and hears a talker down
to about -12 dB of that same reference: a margin of 8 dB, where at scale 1 it would hear both. On a real device the bleed is
whatever the speakers and the room make it: a headset makes it nothing; start with 3 and lower it if interruptions are missed.

**Round trip.** The replies of the 8B run (three turns, 5 sentences, 44 words) transcribed by Parakeet from the speaker's
file come back with 0 word errors; so do the 1.7B's (55 words; "(8,848 meters)" came back as "8,848 meters," with commas).
`tests/pipeline.cone` does the whole circuit (Kokoro says a question, the loop answers it, Parakeet hears the answer).

## What was not run

Nothing here opened a real microphone or speaker. For the morning test with a device: the detector's levels against a real
microphone's noise and gain; the speaker's cut-off delay (`Driver.system(480)` asks for 20 ms; SDL may choose another); the
bleed of the laptop's speakers into its microphone and the right `--threshold-scale`; Kokoro's voices by ear.
