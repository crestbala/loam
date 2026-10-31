# Mune: from "generated" to "composed"

A plan, not a description. `DESIGN.md` says how a piece is made today; this
says what changed and in what order. Phases 1–9 have landed: the
phrase plan, the motif, choosing among many seeded walks and lines,
cadence chords with a breath at the end of each phrase, a tension
curve across register, density, dissonance, dynamics, and harmonic rhythm,
a humanize pass on timing, velocity, and calm swing, and the
arrangement: a counter-melody, a bass that walks into its chords, and
layers that enter by phrase from one table, instruments rendered in
Loam instead of as ffmpeg expressions, and chords decoded as a sequence.

## Why

A piece now holds an idea, and the playing has a touch. Phrases have
labels and endings, one motif is developed through them, each bus keeps
the best of many walks, every hit sits a little off the grid under a
phrase arch, a second line answers the lead, the bass leads into each
chord, layers come in and leave a phrase at a time, and the instruments
are rendered in Loam. What is left is under **Next**, and each step is
judged by ear on the bench sessions.

What Mune keeps: sections and a target energy per bar, the label's unit
returning (`chain.theme_for`), the recording playing on (`ON_BONUS`), the
hook's chords as the progression, and a seeded, repeatable take.

## Two layers

Every change below touches one or both:

- **Recorded layer** — phrases of the listener's recordings, picked by the
  chain. Its notes cannot be changed, only chosen, cut, looped, shifted,
  and stretched. The *hook unit* is this layer's motif.
- **Synth layer** — drums, bass, pad, arp, bells, made note by note in
  `synth.loam`. Here Mune can compose for real.

Hard rule carried over from today: the synths must not fight the
recordings. Anything that changes harmony (cadence chords, harmonic
rhythm, added tensions) only happens where no tonal recording is playing,
or is checked against that unit's heard chroma.

## Order

Phases 1–6 moved a piece from generated to composed; phase 7 shaped the
arrangement.

| Phase | What | Layer | Impact |
|------:|------|-------|--------|
| 1 | Explicit form: phrases, labels, endings | both | landed |
| 2 | Motif and its development | both | landed |
| 3 | Generate many, then select | both | landed |
| 4 | Cadences and phrase shape | synth | landed |
| 5 | Tension curve on more than energy | both | landed |
| 6 | Humanization | synth | landed |
| 7 | Arrangement: counter-melody, moving bass, staggered layers | both | landed |
| 8 | Instruments rendered in Loam, not as ffmpeg expressions | synth | landed |
| 9 | Chords decoded as a sequence (HMM, Viterbi, bass chroma) | recorded | landed |

## Rendering

Drums, bass, pad, and arp are still loops repeated by `aloop`. Harmony
that cadences, takes a colour, or splits a bar keeps its own row; bass
and arp also key their density and dynamics, and pads that share chords
still share a generator. Every generator is a WAV `voices.loam` writes
before the job runs, read as an input. A bell line is one part per phrase, over
`ph_bars * bar_sec`, `reps = 1`, cached by label, variation, section
kind, length, chords, and density (`sy_line`, generator key `8000 +
line`); a counter-line is the same, one part per phrase on the next line
index. A hit loop bakes its humanize, so a calm straight drop keeps
its own generator where that swing would change the feel. Bass and arp
keys are a mixed radix above 200000 (row, swing, bars, lift, density,
dynamics, and for the bass the bar the lead peaks in, which turns its
approaches), so no two fields can collide. Held pads still share. Later
phases should keep that cache. A three-minute piece writes its voices in
about 3.5 s and mixes in about 20 s, where the expressions took about
45 s for one minute.

## Next

Phases 8 and 9 are in `DESIGN.md` (steps 4, 9, and 10). Chord reading
is scored by `tests/chords.loam`; a change to it should move that number.
After them, in order:

- **Chords beyond triads.** Sevenths and sus chords as states of their
  own, and a beat-synchronous chroma (frames averaged per beat) before
  decoding. Score both on `tests/chords.loam`, and on real recordings by
  ear.
- **Search, not samples.** A beam over the chain's slots and over a
  line's notes, rescored whole, in place of the best of 48 walks and 128
  lines. Still seeded.
- **The instruments, by ear.** A filter envelope per note on the bass and
  the arp (`dsp.svf` is there, unused), unison and drift on the pads.

## Seeds and determinism

Motif, candidates, and walks draw from `num.hash(s.seed, …)` with their
own salt. Humanize does too. The same take is still repeatable; New take
still walks elsewhere. Tests pin seeds.

## Bench

`tests/bench.loam` prints the motif (degrees, rhythm), the phrase plan
(label, var, end), the winning melody per phrase with its score terms, the
counter-line's score and terms where one plays, and the top three walks
per bus. Each later phase is judged by ear on the
same three real sessions before and after, plus the engine tests.

## Not in this plan

- Changing the notes of a recording (pitch-correcting a phrase to fit a
  cadence). Recordings are chosen, never rewritten.
- A learned model. Everything here is rules plus search over seeded
  candidates.
- UI. Maybe later: a "Melody" toggle in place of "Bells", and the motif
  drawn under the energy strip.

## Docs

Each phase updates `DESIGN.md` (*How a piece is made*, steps 7–9) when it
lands, and drops its section from this file.
