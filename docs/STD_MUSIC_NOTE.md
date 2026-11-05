# `std.music_note`

Music theory, a multiline multi-track notation language, synthesis, a full DSP toolbox,
analysis, and WAV / MIDI files — pure native code, no dependencies.

```
import std.music_note;
```

Source: `std/music_note/` (`music_note.c` theory + helpers, `mn_score.c` notation parser + MIDI,
`mn_synth.c` instruments/drums/renderer, `mn_dsp.c` effects/editing, `mn_analysis.c` FFT/pitch/tempo,
`mn_io.c` WAV/play). Example: `examples/std_music_note.kc`. Tests: `tests/music_note_test.kc`.

## Conventions

* **Options are `"key=value"` words**, exactly like `std.csv`: `"bits=24 dither=false"`.
  KingCobra has no object-literal syntax, so a JSON/struct object is accepted too.
  `true/false/yes/no/on/off` are booleans, `a,b,c` is a list.
* **Effect specs** put the effect name first: `"reverb mix=0.3"`, `"peak freq=1000 gain=3 q=1"`.
* **Audio buffers** are handles holding 32-bit float samples (interleaved), a sample rate and a channel
  count. Printing one shows `<audio 3.500s 2ch 44100Hz>`. Every effect returns a **new** buffer and
  leaves its input untouched; call `music_note.free(x)` when you are done with big buffers
  (limit: 2²⁸ samples per buffer).
* **Scores** are handles produced by `parse`. Anywhere a score is accepted you can pass the notation text
  directly instead.
* Notes are `A4`, `C#5`, `Bb3`, `Gx2` (double sharp), `Fn4` (natural), `C-1`; middle C = `C4` = MIDI 60;
  `A4` = 440 Hz.
* Fatal errors carry the position: `music_note.parse: line 3, col 7: unrecognized token 'X9' …`.

## Notation language

```
title "Groove"              # header directives; '#', ';' or '//' start a comment
composer Me
tempo 100                   # quarter-note BPM, 10..999
time 4/4                    # used by strict_bars and the MIDI file
key Dm                      # key signature: unmarked B plays as Bb
swing=0.3                   # key=value lines before the first track are defaults for every track

track drums drums           # track <name> [instrument] [key=value ...]
|: kick:q hat:e hat:e snare:q hat:e hat:e :|x4

track keys epiano vol=0.8 pan=0.3 reverb=0.3
{Dm7}:h {G7}:h {Cmaj7}:w
C4:q E4 [G4 B4]:h~ [G4 B4]:q r:q
```

Everything is whitespace separated and free-form across lines. `|` bar lines are ignored (but checked with
`parse(text, "strict_bars")`; the first bar of a track may be a pickup). A `track` header again with an existing
name **continues** that track, so a score can be written bar-by-bar across tracks. Lines before any `track`
form an implicit track called `main` (piano).

### Tokens

| Token | Meaning |
|---|---|
| `C4  F#3  Bb5  c` | a note. Without an octave it uses the track octave (default 4; `o5`, `octave=5`, `>` up, `<` down). An explicit octave always wins |
| `r  rest  -` | rest |
| `[C4 E4 G4]` | chord of notes (each may end in `~` to tie) |
| `{Cmaj7}  {Am/E}  {F#m7b5}` | chord symbol, rooted at the track octave. Qualities: `m dim aug sus2 sus4 5 6 m6 69 7 maj7 m7 mM7 dim7 m7b5 aug7 7b5 7sus4 9 maj9 m9 11 m11 13 maj13 m13 add9 madd9 7b9 7#9 7#11 9sus4` |
| `kick snare hat …` | drum names on a track whose instrument is `drums` (see `drums()`; aliases `bd sd hh oh cp cr rd cb tamb`) |

Any note/rest/chord can be followed by suffixes, in any order:

| Suffix | Meaning |
|---|---|
| `:q  :e.  :h  :8  :1/8  :e3  :1.5b  :q+e  :q*2/3` | duration. Letters `w h q e s t x` = whole … 64th; trailing dots; a trailing digit after a letter makes a tuplet (`e3` triplet, `e5`, …); `8`/`16` are note values; `1/8` fractions of a whole note; `1.5b` beats; `+` adds; `*p/q` scales. No duration = the current default (`l=e` / `len=e`, initially a quarter) |
| `~` | tie into the next note of the same pitch |
| `'` | staccato (half length) |
| `^` | tenuto (full length) |
| `!` | accent (+20 velocity) |
| `@90`  `@mf` | velocity for this note |

### Standalone tokens and options

| Token | Meaning |
|---|---|
| `pp ppp p mp mf ff fff`  `@f`  `dyn=f` | dynamics for what follows (`f` alone is the note F, so use `@f`/`dyn=f`) |
| `\|:  :\|  :\|x3` | repeat the enclosed section (2 times, or N); nests; a lone `:\|` repeats from the start of the track |
| `tempo=90` | tempo change at this point (applies to **all** tracks) |
| `time=3/4  key=G` | time / key signature from here |
| `inst=piano` (or a bare line `inst piano`) | change instrument |
| `vol pan reverb delay delay_time delay_fb chorus lowpass highpass` | track mixing (last value wins): `vol` 0..2, `pan` -1..1, `reverb/delay/chorus` 0..1 wet, `delay_time` in beats, `lowpass/highpass` in Hz |
| `swing=0.3 swing16=0.12 transpose=-2 strum=0.03 gate=0.9 vel=80` | swing for off-beat eighths / off-beat sixteenths (funk), semitone shift, seconds between chord notes, note length ratio, default velocity |
| `glide=0.08` | portamento (seconds) on a track: a tenuto (`^`) note slides into the next note's pitch — the 808 slide |
| `adsr=0.01,0.1,0.7,0.3` `attack decay sustain release` | envelope override |
| `vibrato=5.5,0.2` / `vibrato=off` `detune=8` | vibrato (Hz, semitones), unison detune (cents) |

A bare option line works too (`tempo 90`, `vol 0.7`). Header-level `key=value` lines are defaults for every track.

### Instruments

`instruments()` → `sine square saw triangle pulse organ piano epiano bell pluck bass strings pad brass flute lead
marimba xylophone harp choir noise 808 cowbell clav slap wah stab drums` (aliases: `guitar violin cello rhodes trumpet horn voice vox kit …`).
Synthesis: polyBLEP oscillators, additive piano with inharmonic partials and hammer noise, FM e-piano and bell,
Karplus-Strong plucks, filter-swept brass/bass, detuned-saw strings and pads, formant choir, breathy flute,
inharmonic mallets, organ drawbars. `drums()` → `kick snare rim clap hat ohat tom_lo tom_mid tom_hi crash ride
cowbell shaker tambourine conga clave kick808 snap`.
Production voices: `808` (sub bass with pitch click and saturation, portamento via `glide`), `cowbell` (the pitched
phonk lead), `clav`, `slap` (funk bass), `wah` (chicken-scratch guitar), `stab` (horn hits).

## KCM files: multiline blocks, patterns, includes, audio samples, timestamps

A `.kcm` file is just the notation language above, saved to disk. These features make larger scores practical.
`music_note.parse_file("song.kcm")` reads one (relative sample and include paths resolve against the file's folder);
`parse(text, "base=dir")` / `render(text, "base=dir")` do the same for text. Example: `examples/music/showcase.kcm`.

### Multiline syntax
| Syntax | Meaning |
|---|---|
| `... \` at the end of a line | the next line continues this one |
| `[C4` ⏎ `E4` ⏎ `G4]:h`  `{Am7` ⏎ `}` | a chord may span lines; an open `[` or `{` keeps reading |
| `/* ... */` | block comment, may span lines (`#`, `;` and `//` still comment to the end of a line) |
| `pattern NAME` ⏎ tokens ⏎ `end` | a reusable multi-line block. Play it with `$NAME` or `$NAME*4`. Patterns may call other patterns, contain repeats and option tokens, and are global (any track can use them) |
| `include "other.kcm"` | insert another file here (relative to the including file; nests up to 8 deep, cycles are rejected). Included files can define patterns, samples, markers |

Errors name the file and line, including lines inside included files: `drums.kcm line 4, col 2: ...`.

### Audio samples (several per score; files and/or inline)
```
sample kick  "samples/kick.wav" gain=-2          # a file, relative to this .kcm
sample vox   "vox.wav" root=C4 bpm=95 oneshot    # pitched by note name, tempo-synced
sample chime gain=-2 root=E5 data                # inline audio: base64 of a WAV file, then `end`
UklGRvDlAQBXQVZFZm10IBAAAAABAAEARKwAAIhYAQACABAAZGF0...
end
```
Declaration options: `gain=dB` `root=NOTE` (the pitch at which the sample plays unchanged) `bpm=N` (the sample's tempo; it is
time-stretched to follow the score tempo without changing pitch) `start=T` `end=T` (trim) `fadein=T` `fadeout=T` `reverse`
`oneshot` (play to the end, ignoring note length) `cut` (stop at the token's duration when triggered). `data` must be the last word
on the header line. Names are letters/digits/`_`/`-` and may not look like a note, rest, drum or dynamic (`f` is a note!).

Using them inside tracks:

| Use | Syntax |
|---|---|
| one-shot trigger | `&chime`, `&chime:q`, or just `chime` (like a drum name); `transpose=12` shifts triggers by semitones, `@100` sets loudness |
| sample as an instrument | `track lead sample=vox` then play notes: `C4:q E4:q` resample the sample to each pitch; held notes are cut with a short release unless the sample is `oneshot` |
| in a drum track | sample names work next to drum names: `kick clap2 snare` |

Samples at any sample rate or channel count are accepted (converted to the render rate; stereo samples stay stereo). Level is
unity at the default velocity. Programmatic access: `embed(audio | wav_path[, "name=vox gain=-3 root=C4 oneshot"])` returns
the `sample ... data ... end` text to paste into a score (a 0.3 s mono sample is about 13 KB); `sample_list(score)` lists them;
`parse(text, options, ["vox"], [audio])` supplies samples you built in code (names that you then use in the text).

### Timestamps
Anywhere a time is expected you can write `12.5` · `12.5s` · `500ms` · `1:02.5` · `0:01:02.500` · `1m30s` · `2h`.

| Syntax | Meaning |
|---|---|
| `at=1:30` | jump this track to an absolute time (backwards too, for overlays) — also `at 1:30` on its own line |
| `wait=2s` | rest for a duration in seconds, whatever the tempo |
| `mark=chorus` | name the current position of this track |
| `mark chorus 1:30` | (own line) a marker at an absolute time |
| `at=chorus` | jump to a marker (defined earlier) |
| `length 3:30` | exact output length: shorter music is padded with silence, longer is cut with a 20 ms fade |

Timestamps are converted with the tempo map **as defined so far**, so put `tempo` changes in the header or the first track
(or use `at=` only where the tempo no longer changes). An `at=` jump resets the strict-bar counter.
`events()` reports `start` (seconds) and a `timecode`; `markers(score)` → `[{name, time, timecode, beat, track}]` sorted by time;
`timecode(seconds[, "fps=30"])` → `01:02:05.500` / `00:01:01:15`; `time_seconds("1m30s")` → `90`.

| Function | |
|---|---|
| `parse_file(path[, "strict_bars"])` | read a `.kcm` file |
| `embed(audio \| wav_path[, options])` | inline-sample text |
| `sample_list(score)` | declared samples: `{name, source, seconds, rate, channels, gain, root, bpm, oneshot, reverse}` |
| `markers(score)` | named timestamps |
| `timecode(seconds[, "fps=30"])` · `time_seconds(text)` | |

## Function reference

### Theory
| Function | |
|---|---|
| `freq(note \| midi[, a4=440])` | frequency in Hz |
| `midi(note)` / `name(midi[, use_flats])` | note ⇄ MIDI number |
| `from_freq(hz[, a4])` | `{note, midi, cents, freq}` nearest note |
| `transpose(note \| midi \| notes[], semitones)` | |
| `interval(a, b)` | semitones |
| `scale(root[, kind="major"[, octaves=1]])` / `scales()` | 24 scales incl. modes, pentatonics, blues, whole-tone, diminished |
| `chord(symbol[, octave=4])` / `chords()` | chord symbol → notes |
| `identify(notes[])` | notes → chord symbol (`"Cmaj7"`, `"C/E"`, `""`) |
| `key_signature(key)` | `{fifths, minor, accidentals}` |

### Score and rendering
| Function | |
|---|---|
| `parse(text[, "strict_bars base=dir"[, names[], audios[]]])` | notation → score |
| `score_info(score\|text)` | `{title, composer, tempo, tempo_changes, time, key, beats, seconds, length, notes, samples, markers, tracks[…]}` |
| `events(score\|text[, track])` | list of `{track, start, timecode, duration, beat, beats, midi, note, velocity, drum, sample}` (seconds) |
| `render(score\|text[, options])` | stereo audio. Options: `rate` `tail` (s, default 1.5) `master` `speed` `transpose` `humanize` (ms) `seed` `normalize` `mono` `solo=name` `mute=a,b` `a4` |
| `to_midi(score\|text, path)` | Standard MIDI File (format 1, 480 PPQ, tempo map, key/time signature, GM programs, drums on channel 10, volume/pan) |
| `note(note\|midi, seconds[, options])` | one note (`instrument velocity rate attack decay sustain release vibrato_rate vibrato_depth detune a4`) |
| `drum(name[, "velocity=0.9"])`, `instruments()`, `drums()` | |
| `free(audio\|score)` | release memory |

### Generators
`tone(wave, freq|note, seconds[, "amp rate channels duty phase harmonics=1,0.5,0.25 fade_in fade_out"])`
(`sine square saw triangle pulse` or custom additive harmonics) · `noise(kind, seconds)`
(`white pink brown blue violet`) · `sweep(f0, f1, seconds[, "wave curve=exp|linear"])` ·
`silence(seconds[, rate[, channels]])` · `click_track(bpm, beats[, "accent_every=4"])` ·
`impulse_response(seconds[, "decay=0.6 rate channels predelay seed"])` (synthetic room).

### Effects and editing (each returns new audio; all also work inside `chain`)
| Function | Positional / options |
|---|---|
| `filter(a, type, freq[, "q gain slope"])` | `lowpass highpass bandpass notch allpass peak lowshelf highshelf`; `slope=12\|24\|48` for low/high-pass |
| `lowpass(a, freq)` `highpass(a, freq)` `bandpass(a, freq, q)` `notch(a, freq, q)` | shorthands |
| `eq(a, ["peak freq=1000 gain=3 q=1", "highshelf freq=8000 gain=-2"])` | parametric EQ |
| `delay(a, time[, "feedback=0.4 mix=0.35 pingpong=true tail"])` | echo, tail auto-extends |
| `reverb(a[, "room=0.7 damp=0.5 mix=0.3 width=1 predelay tail"])` | Freeverb, outputs stereo |
| `chorus(a[, "rate depth mix voices"])` `flanger(a[, "rate depth feedback mix"])` `phaser(a[, "rate depth stages feedback mix min_freq max_freq"])` | |
| `tremolo(a, rate, depth[, "shape=sine\|triangle\|square"])` `vibrato(a, rate, depth_cents)` `ring_mod(a, freq[, mix])` | |
| `distortion(a[, "drive=4 kind=soft\|hard\|fold\|tube mix level tone"])` `bitcrush(a, bits, downsample)` | |
| `compressor(a[, "threshold=-18 ratio=4 attack release knee makeup=auto"])` `limiter(a[, "ceiling=-1 lookahead release"])` `gate(a[, "threshold range attack release hold"])` | |
| `gain(a, db)` `normalize(a[, peak_db=-1])` `fade(a, in, out[, curve])` `dc_block(a)` | |
| `pan(a, pos)` `width(a, amount)` `to_mono(a)` `to_stereo(a)` `swap_channels(a)` `channel(a, i)` | |
| `slice(a[, "start end"])` `loop(a, n)` `reverse(a)` `trim_silence(a[, "threshold pad"])` `copy(a)` | |
| `resample(a, rate)` `speed(a, factor)` `time_stretch(a, factor)` (WSOLA) `pitch_shift(a, semitones)` | |
| `convolve(a, ir[, "mix normalize"])` | FFT convolution reverb with any impulse response (e.g. `load_wav`, `impulse_response`) |
| `mix(audios[][, gains_db[]])` `mix_at(base, other, at_seconds[, gain_db])` `concat(audios[][, crossfade_seconds])` | |
| `chain(a, ["lowpass freq=2000", "reverb mix=0.2", "gain db=-3"])` / `effects()` | apply a list in order (not `convolve`: a spec string cannot hold a buffer) |

### Analysis
`info(a)` · `stats(a)` (`peak peak_db rms rms_db dc crest_db clipped silent`) · `samples(a[, "channel start count step"])` ·
`from_samples(floats[], rate, channels)` · `waveform(a, points)` → `{min[], max[]}` · `window(kind, n)` ·
`fft(samples[])` → `{re, im, mag}` (`fft(obj, true)` inverts) ·
`spectrum(a[, "size=4096 start window"])` → `{freqs, mags, db, peak_freq, peak_mag}` ·
`pitch(a[, "min max start size threshold"])` (YIN) → `{freq, confidence, voiced, note, midi, cents}` ·
`onsets(a[, "sensitivity min_gap"])` → seconds[] · `bpm(a[, "min max"])` → `{bpm, confidence}`.

### Files
`save_wav(a, path[, "bits=16|8|24|32|float dither=true"])` · `load_wav(path)` · `wav_info(path)` ·
`to_wav(source, path[, options])` — source may be audio, notation, a score, or a note array:
`to_wav(["C4","E4"], path, 0.3)` keeps the original mono sine behaviour (returns the sample count);
`to_wav(notes, path, "seconds=0.5 instrument=pluck")` plays the array through an instrument ·
`play(source[, options])` plays through the system player (Windows: winmm; Linux/macOS: `aplay`, `paplay`,
`pw-play`, `afplay`, `ffplay` or `play`). Files and playback are refused inside `comptime`.


## Phonk and funk

Seeded generators write complete, editable arrangements as ordinary notation, then `produce` renders and
masters them like a small studio chain.

```
let text  = music_note.groove("phonk", "bars=16 bpm=140 key=F#m seed=7");   // notation you can read, edit, parse, to_midi
let audio = music_note.produce("phonk", "bars=16 bpm=140 key=F#m seed=7");  // mastered stereo audio
music_note.save_wav(audio, "phonk.wav", "bits=24");
```

| Style | Character |
|---|---|
| `phonk` (`memphis`) | 138 BPM, half-time trap drums, 808 following a kick pattern with slides, repeating cowbell motif transposed with the progression, eerie bells, dark pad, hi-hat rolls, fills every 8 bars, breaks every 16 |
| `drift_phonk` (`drift`) | 150 BPM, busier kicks and hat rolls, shorter 808 glides |
| `brazilian_phonk` (`funk_carioca`) | 130 BPM, tamborzão-inspired kick/clap syncopation under the cowbell lead and 808 |
| `funk` | 104 BPM, 16th-note groove with swing16, ghost-note snare, syncopated kicks, slap bass locked to the kick, clavinet stabs, wah chicken-scratch guitar, horn stabs on alternate bars, a pentatonic lead lick every 4 bars, i7/IV9 style vamps |

Options (all optional): `bars` (1..128, default 16) · `bpm` · `key` (`F#m`, `Dm`, `C` …; minor keys give minor-seventh funk
chords, major keys dominant-ninth) · `seed` (same seed = identical result) · `intensity` 0..1 (density of rolls, ghost notes,
syncopation) · `swing` (funk sixteenth swing) · `scale=phrygian` (phonk). `produce`/`stems` also take `master`
(`phonk funk clean loud lofi tape none`), `sidechain` (kick ducking depth, default 0.55 for phonk, 0 for funk), `rate`, `tail`,
`humanize` (ms).

Arrangements have an intro (first 2 bars), fills, and breaks, and each role is its own track (`kick snare hats cowbells bass
lead bells pad` / `kick snare hats openhat perc bass keys guitar horns lead`) so you can mute, solo or re-balance them:

```
let a = music_note.render(music_note.groove("funk", "seed=3"), "mute=horns,lead");
let stems = music_note.stems("phonk", "bars=8 seed=2");        // [{name, audio, ducked}, ...] for a DAW
music_note.to_midi(music_note.groove("drift_phonk", "seed=5"), "drift.mid");
```

| Function | |
|---|---|
| `groove(style[, options])` | notation text |
| `groove_styles()` | the style names |
| `produce(style[, options])` | stems rendered, bass/lead/bells/pad ducked by the kick, summed, mastered |
| `stems(style[, options])` | every track as its own stereo buffer |

### Production effects
| Function | |
|---|---|
| `sidechain(audio, key[, "depth=0.8 threshold=-12 attack=0.004 release=0.14"])` | duck `audio` whenever `key` is loud |
| `saturate(a[, "drive=2 mix=1 bias=0.1 tone"])` | asymmetric tape/tube saturation |
| `wah(a[, "rate=1.5 depth=1 q=4 min_freq max_freq mode=lfo\|env sensitivity mix"])` | auto-wah |
| `vinyl(a[, "level=0.015 crackle hiss rumble"])` | record noise |
| `master(a, preset)` | `phonk funk clean loud lofi tape`: EQ, saturation, compression and a brickwall limiter |

All of these (except `sidechain`) also work inside `chain`, as do the filter names (`"peak freq=350 gain=-2"`, `"lowshelf …"`).

## Notes

* The renderer is deterministic: the same score gives byte-identical audio (`seed=` and `humanize=` vary it).
* Mix headroom shrinks with the track count and a brickwall lookahead limiter (−0.3 dBFS) guarantees no clipping.
* The generated arrangements are original, rule-based patterns written for these genres; they are a starting point to edit, not a copy of any recording.
* `bpm` and `onsets` are estimates and work best on rhythmic material of several seconds.
* Not included: MIDI import, microtonal tunings beyond `a4=`, gradual tempo ramps and crescendo ramps.
