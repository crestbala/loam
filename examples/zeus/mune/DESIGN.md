# Mune

A Zeus app that makes a new piece from recordings you already have. Mune
cuts phrases on each recording's own pulse, hears their chords, and uses
concatenative synthesis and a Markov chain to sequence them along an
energy arc — a quiet intro, a verse, a build, a full drop, a break, a
second drop that brings the first one back, an outro. Instruments play
along on the chords the recordings play, in the same key and tempo. One
short motif is drawn for the take and sung, changed, through the phrases
of that form.

`zeli serve examples/zeus/mune --target macos` opens it, and
`--target web` opens the wasm build in the browser (both talk to the same
engine — see **Native and web** below).

You need the rights to the recordings you import, and to the piece made
from them. Mune does not ship any songs.

## How a piece is made

1. **Decode.** ffmpeg writes a 48 kHz copy of each recording, a 100 Hz
   envelope in three bands (low < 180 Hz, mid 200 Hz – 3.5 kHz,
   high > 5 kHz), a mono 5512 Hz copy for key and chords, and the loudness. Loam
   never holds the recording itself.
2. **Voices.** When the host has a classifier, it maps where anyone sings
   or speaks. With voices removed, phrases come only from voice-free
   passages. A recording with almost none is separated by the host, and
   its least-voiced phrases are cleaned hard.
3. **Listen** (`core/listen.loam`). Onsets come from the band flux.
   Tempo is the autocorrelation peak (60–180 BPM), refined by folding.
   A live band that drifts reads as no pulse over the whole file, so a
   weak fold is checked again over 20 s windows, each at its own phase and
   tempo. The downbeat is the beat with the most kick. Bar lines are then
   tracked: each is predicted a bar on and pulled halfway to where the
   next two bars' beats land, so the grid follows the band. Every 1-, 2-,
   and 4-bar span is a candidate phrase, scored on its seam, level, and
   steadiness. Free-time recordings give 4 and 8 s windows instead.
4. **Hear the chords** (`core/key.loam`). Goertzel magnitudes of every
   semitone A2–C6 over 0.37 s frames fold into chroma, and C2–B2 into a
   bass chroma. A frame 40 dB under the recording's loud ones is silence.
   The key is the Krumhansl–Kessler profile that fits the whole file.
   Each phrase keeps four blocks of chroma, one per bar of a four-bar loop
   (a shorter phrase repeats its bars, the way it will loop), and how
   *tonal* each is — its three strongest pitch classes against its six
   weakest — says whether it has a chord at all (hiss and drums do not).
   The chords are decoded as a sequence over the whole recording: a hidden
   Markov model over the 24 triads and no chord, solved with Viterbi. A
   frame scores each triad by how well its chroma matches the triad's
   weights, damped where the frame is not tonal, plus how strong the
   triad's root is in the bass. Changing chord costs, more to one outside
   the key, so a frame smeared by a drum does not become a chord and a
   chord that holds does. A phrase's bar is the chord that holds 40% of
   its frames, or none. On Mune's own renders of every style, where each
   bar's chord is known, this names 85% of bars right; reading each bar
   alone named 80% (`tests/chords.loam`).
5. **Describe the units.** Each phrase becomes a unit with
   - **energy**: level (45%), onset density (35%), and brightness (20%),
     each ranked on one scale across every recording;
   - **clash**: how far its chords, moved into the piece key, stray from
     the progression — chord tones count fully, scale tones half — weighted
     by how tonal it is;
   - **kind** (groove or texture), brightness, level, and its loop score.
6. **Key, tempo, and chords** (`core/arrange.loam`). The piece takes the
   rhythm recording's key and pulse, folded into the style's tempo range.
   Other recordings shift by at most six semitones, and each phrase
   stretches (at most 26%) from its own tracked length onto its bars. The
   **hook** — the rhythm or motif phrase that loops best, is fullest, and
   is most tonal — is heard bar by bar, and its four chords become the
   progression every synth plays. Only when nothing is tonal enough does
   the style's own progression stand in.
7. **Form** (`core/plan.loam`). A piece is 1, 3, or 4 minutes. A minute
   is intro, verse, build, drop, outro; three and four are intro, verse,
   build, drop, break, verse, build, drop, outro. Sections scale to the
   length, then are trimmed or grown a phrase at a time (drops grow) so
   the piece lands within a phrase of it. Each section is a row of
   phrases, four bars long, or two when a slow minute has no room for
   four. A phrase has a label and an ending. The same label is the same
   idea: `0` is a, `1` is b, and the variation counts how many times that
   label has been heard.
   An intro is `a`, open. A verse is `a` open, then `a′` closed. A build
   is `b` open, then `b′` open, and does not resolve. A drop is `A` open,
   `A′` closed, `B` open, `A″` closed. A break is `b` open, then `a`
   closed. An outro is `a`, closed. A drop's `A` is the verse's `a`. A
   section longer than its plan repeats the last two phrases; a shorter
   one keeps the first and the last. The second drop copies the first
   drop's phrases, so the return is the same labels and the same
   variations.
   Every bar gets a target energy: the intro rises out of near-silence, a
   verse sits in the middle, a build climbs, a drop is full (the last
   fullest), a break falls away, the outro settles.
   Which layers play is one table (`arrange.entry`): for every layer,
   recorded buses included, the phrase of each section it enters on, and
   an entry tension that orders them. A verse opens on drums, bass, and
   pad (and the rhythm recording); the arp and the motif recording join
   on its second phrase. A drop starts full and adds the counter-melody
   on its second phrase. A break strips to the pad, the lead, and the
   recordings that carry them, with the counter on its second phrase. In
   the outro, after its first phrase, a layer leaves once the path falls
   below its entry tension, so the last in is the first out. The style
   only decides which cells are filled.
8. **Concatenative synthesis with a Markov chain** (`core/chain.loam`).
   Each bus (rhythm, motif, air) is a row of slots on the bar grid, four
   bars each. An intro, a build, or an outro is one slot, so its sweep or
   its fade spans the section, and the slot carries the phrase that
   starts it. Forty-eight walks are drawn. The one that is kept is the
   one that, taken whole, sits nearest the arc, brings a repeated label
   back to the unit its first phrase played, jumps least, varies its
   units without changing every slot, and does not share audio with a
   bus already placed. The same seed picks the same walk.
   Inside a walk the chance of the next unit is

       P(j | i, k) ∝ exp(−(energy(j, k) + clash(j) − quality(j) + move(i, j, k)) / T)

   - `energy`: distance from the unit's energy to slot k's target, mapped
     into the span that bus's units cover.
   - `clash`: so the recordings and the instruments agree.
   - `move`, what makes it music rather than a shuffle: inside a section
     the unit that followed `i` in its own recording is cheapest (the piece
     plays on as the record did); playing `i` once more is cheap (a hook is
     four bars, then the same four bars), a third time costs; a jump costs,
     more the further its sound. A label heard before brings back the unit
     the first phrase of that label played, so the second `a` is the first
     `a` again. A new label at a section boundary is still a change. In a
     build, a one- or two-bar cut from inside the hook is cheap, and the
     cost of having heard that audio lately is waived. Audio heard in the
     last three slots costs, unless it is the theme, the record playing
     on, or that fragment of the hook.
   - T = 45.
   A unit loops to fill its slot; audio played on one bus is kept off the
   others. A recording never plays against itself: motif draws only from
   motif recordings, and air that has to borrow the rhythm recording plays
   only where the rhythm bus is silent. Of two recordings the second is
   motif when it has a pulse, air when it has none.
9. **Instruments** (`core/synth.loam`, `core/motif.loam`,
   `core/voices.loam`). Drums, bass, pad, and arp are loops in the piece
   key and tempo, on the progression's chord codes, so a borrowed chord
   plays as it was heard. `synth.loam` decides the notes; `voices.loam`
   renders them to samples in Loam (`dsp.loam`: a sine table, PolyBLEP saws
   and squares, seeded noise, per-sample decays) and writes each loop as a
   24-bit WAV the mix reads. A note rings until it is 80 dB down; drums
   and bass choke the note before with a 3 ms fade, everything else rings
   over. A loop folds its tail back onto its start, so the repeat has no
   seam, and every legato pad oscillator, the detuned ones too, fills its
   loop in whole cycles. A line played once stops at its end.
   The bells are a line for each phrase, played once. One motif is drawn
   for the take: a few scale degrees, at most a sixth across, at most one
   leap, starting on a chord tone of the first bar. A phrase states that
   motif or a transform of it — inverted, reversed, stretched, or cut
   down and sequenced up — answers it, and ends as the phrase asks. An
   open phrase ends on 2, 5, or 7. A closed phrase ends on 1 or 3, on the
   downbeat of its last bar, and holds it. Strong beats sit on that bar's
   chord. The line kept is the best of many seeded candidates: steps,
   leaps, range, the chords, the recording underneath, the arch, the
   rhythm, and the distance from the motif. The same label and variation
   share one line, and so do the chords under it. A line is placed whole:
   one tonic note is chosen so its middle sits at the register asked
   for, and every degree keeps its octave from there, so a step is heard
   as a step. The drop's `A` is sung an octave higher where there is
   room. No sung note goes above A6 (1.76 kHz), and no arp note above E6:
   higher, the bell's partials pass half the sample rate and fold back
   as whistles. A line is played once, so its notes are nudged inside the
   phrase rather than wrapped around it, and it is silent before its
   first note. The final drop, recordings included, may move up a whole
   step, and only when every loop there can take it and stay within six
   semitones. The synths move with them: pads, arp, bass, the impact,
   and the sung lines all play in the new key there.
   Where the recording under a phrase is not tonal (the same threshold
   the hook uses), or nothing is playing, the chords cadence with the
   line. An open phrase's last bar becomes the dominant. A closed
   phrase's last two bars move dominant, then tonic. Under a tonal
   recording the chords stay as they were heard and only the melody
   cadences. The bass is a line, not only a figure: the root on the
   downbeat, then an approach into the next bar's root — a scale step
   above when it comes down, a semitone or a scale step below when it
   goes up. It comes down while the lead climbs to its peak and goes up
   after, so the two move against each other. A style with its own figure
   (house offbeats, the 808, the log drum, lo-fi, synthwave) keeps its
   rhythm: the note on the last eighth takes the approach, or one is
   added there and the note before it is cut. A held bass walks on beat
   four. Where the next bar keeps the root, the figure plays on.
   Where the table brings it in, a counter-melody sings under the lead,
   on the pad's timbre, an octave below. It is the best of many seeded
   lines: where the lead sounds on a beat the counter sits a third, a
   sixth, or an octave under it, and where the lead rests it takes a
   chord tone; it moves where the lead holds or rests, and is scored on
   contrary or oblique motion, consonance, filling the gaps, and steps.
   It lands on the downbeat of the phrase's last bar and holds. A lead
   line that returns brings its counter back.
   The last half-bar of every phrase takes a breath: the arp
   drops to eighths, the bells rest apart from the cadence note, and the
   drums play a fill (the kick rests, the snare answers).
   The same arc feeds five channels. Register lifts the pad, the arp,
   and the bells in whole tones, up to an octave, from the phrase.
   Density plays the arp in quarters when the bar is quiet and in
   sixteenths when it is full, and a motif at 700 or above is diminished
   (a build already is). Dissonance stays on triads while the path is
   low, adds sevenths as it rises, and in a build reaches a sus4 and
   then an add9, resolving to a triad on the drop's first bar. Dynamics
   sets the synth bed from −6 dB to 0 dB, and the velocities follow.
   Harmonic rhythm holds one chord across two bars when the path is low,
   one chord a bar when it is high, and two chords in the last bar of a
   build. That bar drops the kick and the bass; the drop's first
   downbeat, where it is free, is the tonic. A tonal recording keeps the
   chords, the style's voicing, and one chord per bar.
   Hits then leave the grid by a seeded amount. A loop repeats that feel;
   each sung phrase draws it again. The kick stays within 2 ms. The snare
   and the bass stay within 6 ms, the snare 3 ms late. Hats stay within
   8 ms, and within 4 ms over a recorded groove so they do not flam the
   record. The arp stays within 8 ms. The lead stays within 15 ms and sits
   5 ms late. A stabbed pad's onsets stay within 20 ms. A held pad stays
   on the bar, so a whole number of cycles still fills its loop. Velocity
   is the note, times an arch that peaks on the melody's highest note,
   times a stronger downbeat than an off-sixteenth, times a seeded 8%.
   It stays above silence and at most full. The last note of a closed
   phrase is softer. A straight style played calmly delays its odd
   sixteenths by another 2–4%. A drop stays on the style's own swing, and
   so does a style that already swings.
   Where a recording already
   carries a groove, the synth drums thin to the style's hats; where it
   carries a bass (over 55% low band), the synth bass stays out. The
   intro opens the groove out of a low-pass. A build's snare roll is
   short hits, so its sixteenths stay strokes rather than a wall of
   noise, and its riser's noise opens from 1.2 kHz to 9 kHz as it climbs.
10. **Mix** (`core/graph.loam`, `core/fx.loam`). The instrument voices
    are written first (`graph.render_voice`, dozens of voices at up to a
    second each, so the app and the engine write a slice of them per tick
    rather than holding the window), then one ffmpeg filter graph reads
    them with the recordings:
    each instrument loop is filtered, panned, repeated, and placed; each
    phrase is cut on the sample, cleaned if it has to be, shifted and
    stretched onto the grid, faded at the seam, looped, and delayed onto
    its bar. Buses get EQ; echo and reverb are sends; the master glues,
    limits, and fades.
11. **File** (`core/jobs.loam`). Float premaster, two-pass `loudnorm` to
    −14 LUFS / −1 dBTP, triangular high-pass dither into 24-bit 48 kHz
    WAV, and a 50 Hz peak overview for the waveform.

## App

```
app.loam              entry
theme.loam            the two appearances, as role tokens
routes/               one page
components/
  screen.loam         header (ffmpeg status, light/dark), the two columns
  kit.loam            panel, row, pills, slider, theme toggle, icons
  recordings.loam     drop zone, one row per recording and its role
  piece.loam          length, energy, tempo, pitch, voices
  sound.loam          style, instruments, effects
  player.loam         play, waveform, sections, energy arc, save
studio/
  store.loam          signals, the file list, the job stages
  controls.loam       one signal per sound control
  files.loam          work folders and saved pieces
  engine_client.loam  the wasm build's calls into the native engine
server/
  api.loam            the gRPC-Web contract (`Mune.*` methods)
  engine.loam         the pipeline the engine runs (decode → render)
  main.loam           the engine process `zeli serve` starts on :8080
web/
  app.loam            the wasm entry (Canvas2D), same store as the window
  vite.config.js      compiles the wasm, proxies `/Mune/*` to the engine
core/
  listen.loam         envelope, tempo, grid, phrases, timbre
  key.loam            key of a recording, chords of a phrase
  plan.loam           the form, the phrases, the energy arc
  motif.loam          the motif, and the line each phrase sings
  chain.loam          the Markov walk, and the walk that is kept
  arrange.loam        roles, chords, the score
  synth.loam          what the instruments play: hits, chords, grooves
  voices.loam         the instruments, rendered to samples
  dsp.loam            oscillators, noise, decays, the WAV writer
  graph.loam, fx.loam the filter graph
  jobs.loam           the shell jobs
tests/                engine, headless screen, the real session
```

`app_routes.loam` is generated by `zeli routes` and is not hand-edited.

### Native and web

A browser has no ffmpeg and no voice tools, so it cannot make a piece on its
own. The **macOS** window runs the pipeline in its own process. The **web**
build sends every step to the native engine (`server/main.loam`) over
gRPC-Web — the same pattern `examples/zeus/vidget` uses for its downloader.
`zeli serve` builds and starts one engine on :8080 before the UI on either
target, so both drive the same code. `studio/engine_client.loam` is the
switch: `remote()` is true on wasm, and the store forwards `add` / `generate`
/ `poll` / `take` / `save` to the engine instead of doing the work itself.

### Light and dark

Every colour in `theme.loam` is a role token (`@120`…), registered with a
light and a dark value. A widget stores the role and the paint pass
resolves it against `zeus.appearance()`, so the header toggle repaints the
whole window with no rebuild. A hex string is baked in at build time and
would not follow the switch. `install` also binds the Zeus roles the
stock widgets read, so text fields, switches, and buttons match the cards.

### Layout note

A box with `grow = 1` must not also set `width = px(0)`: the zero is taken
as a fixed width and the box never grows. Leave the width unset and let
`grow` share the row.

## Tests

- `tests/engine.loam` — chords heard in synthesised triads (and none in
  hiss), decoded as a sequence that holds through a smeared frame, takes
  a chord that holds, names none in noise, and lets the bass pick the
  root, chord codes voiced in key, energy ranks, that the walk follows
  the arc, avoids a clashing unit, plays the recording on, brings a
  repeated label back, honours slot lengths, and is seeded; that the
  best of many walks is no worse than the first and is repeatable; that
  phrases tile the form and the second drop repeats the first; that
  motif transforms undo and a chosen line cadences (closed on 1 or 3,
  open never on 1); that an arranged piece takes its progression from
  the recordings, follows the arc, thins the synth drums over a recorded
  groove, lifts its pads, and raises the last drop a whole step; that a
  cadence chord is written only where the recording is atonal or silent;
  that the last half-bar of a phrase thins the arp, rests the bells, and
  fills the drums; that density and register rise through a build, a
  drop's first bar has no dissonance, and a tonal recording keeps its
  harmony; that a humanized hit stays inside its part's window, velocities
  stay in range, the same seed repeats, and a calm drop keeps its own
  loop; that the style's progression stands in when nothing
  is tonal; that a recording never plays against itself; that a dream
  piece sings a line per phrase; that in a verse the layer count rises
  at the second phrase, the drop is the fullest section, the break keeps
  only the pad and the lead, and the outro lets layers go in reverse; that
  the counter-melody's beats are consonant with the lead, never cross it,
  and play only from a drop's or a break's second phrase; that the bass
  keeps a style's figure and approaches the next root from a step away,
  against the lead; that a line keeps its contour and stays under its
  ceiling, and a once-played line never wraps a note; that the lifted
  drop moves the synths with the recordings, drums aside; that the graph
  builds and reads every instrument from a WAV Loam rendered, each loop
  exactly its length, none clipping, a band-limited saw with no step, and a
  pad loop with no seam at its wrap. No window.
- `tests/routes.loam` — the screen paints headless, and the appearance
  toggle turns the room light and the ink dark.
- `tests/session.loam` — drops two files, ignores a duplicate paste,
  generates through ffmpeg, checks a 48 kHz 24-bit stereo WAV of the
  arranged length, and that the take placed phrases on a rising arc.
  Skips the render without ffmpeg.
- `tests/chords.loam` — not a test: renders every style, hears each
  render as a recording, and scores the chord readers against the bars'
  known chords.
- `tests/bench.loam` — not a test: the whole pipeline on any files from
  the command line, printing every phrase (chords, energy, low band), the
  progression, and the score, then rendering `take.wav`. For hearing what
  a change does to real music.

```
sh examples/zeus/mune/tests/run.sh
```

## Limits

- The first 8 minutes of each file are the material.
- Chords are triads heard from chroma, not a transcription: a sus or a
  slash chord reads as its nearest triad.
- A recording that changes tempo between sections is tracked within
  about 3% a bar; past that its phrases are cut off the beat and score
  low as loops.
- A recording with no pulse becomes air.
- Recordings in distant keys shift at most six semitones.
