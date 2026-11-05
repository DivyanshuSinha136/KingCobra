# std.tts — text-to-speech

```kc
import std.tts;
tts.to_wav("Hello! KingCobra can read $5.50, 10/28/2026 and 3:30 pm.", "hello.wav", "voice=narrator");
```

A complete, dependency-free English speech synthesizer written in C: a text normaliser, a pronunciation
engine (dictionary + morphology + letter-to-sound rules + homograph handling, with ARPAbet **and** IPA in and out),
a prosody model, an **expressive vocal-style system** (`[happy]`, `[sad]`, `[whisper]`, `[dramatically]`, `[laugh]`,
`[sigh]`, … with automatic typo correction), and a Klatt-style formant synthesizer with ten voices. Everything runs offline, deterministically
(same text + options + `seed` → same samples), and at several hundred times real time.

Speech is returned as a regular `std.music_note` audio buffer, so `music_note.save_wav`, `music_note.play`,
`music_note.filter`, `reverb`, `mix`, … all work on it (see the last example).

The voice is a *formant* synthesizer: clear and fully controllable (pitch, speed, vocal-tract length, breathiness,
whisper, robot), but unmistakably synthetic — it is not a neural voice.

## Quick reference

| Area | Functions |
|---|---|
| Speak | `speak(text[, opts])` → audio · `speak_phonemes(phones[, opts])` → audio · `say(text[, opts])` (plays, blocks) |
| Files | `to_wav(text, path[, opts])` → `{path, seconds, frames, bytes}` · `save_wav(audio, path[, "bits=16"])` · `free(audio)` |
| Pronunciation | `phonemes(text[, opts])` · `ipa(text)` · `pronounce(word)` · `explain(word)` · `words(text)` · `from_ipa(ipa)` · `to_ipa(arpabet)` · `phoneme_table()` |
| Text | `normalize(text[, opts])` · `number_to_words(n[, mode])` |
| Timing | `duration(text[, opts])` · `align(text[, opts])` · `align_phonemes(phones[, opts])` |
| Lexicon | `add_word(w, pron)` · `add_words(text│array)` · `remove_word(w)` · `clear_lexicon()` · `lexicon_size()` · `load_lexicon(path)` · `save_lexicon(path)` · `lookup(w)` · `has_word(w)` |
| Substitution | `add_sub(word, replacement)` · `remove_sub(word)` · `clear_subs()` |
| Voices | `voices()` · `voice_info(name)` · `define_voice(name, base[, opts])` · `info()` |
| Styles | `styles([category])` · `style_info(name)` · `define_style(name, base[, opts])` · `resolve_tag(text)` · `events()` · `interjections()` |

Options are either an object, `{ voice: "female", rate: 1.2 }`, or a `"key=value key=value"` string. Wherever options are
accepted a bare voice name works too: `tts.speak("Hi", "female")`. Bad input raises a normal, catchable KingCobra error
(`try { tts.speak("x", "voice=nope"); } catch (e) { println(e.message); }`).

## Options

| Option | Default | Meaning |
|---|---|---|
| `voice` | `male` | one of `tts.voices()` |
| `rate` | `1.0` | speed multiplier, 0.25 – 4 (consonants compress less than vowels, as in real speech) |
| `wpm` | – | words per minute (`rate = wpm/170`) |
| `pitch` | `0` | shift in **semitones** (−24 … 24) |
| `f0` | voice | base pitch in Hz (overrides the voice) |
| `range` | `1.0` | intonation range: `0` monotone … `2` very expressive |
| `volume` | `1.0` | output level (the signal is RMS-normalised, then soft-limited) |
| `sr` | `22050` | sample rate 8000 – 48000 |
| `pause` | `1.0` | scales all pauses; `comma`, `period`, `paragraph` set them in ms (170 / 380 / 700) |
| `accent` | `us` | `us` (flapping: "water") or `uk` (non-rhotic: r-dropping) |
| `breath`, `tilt` | `0` | extra breathiness / spectral tilt (softer, darker voice) |
| `reverb` | `0` | room reverb amount 0 – 1 |
| `trim` | `false` | remove the lead-in/tail silence |
| `seed` | `1` | noise/jitter seed |
| `ssml` | `auto` | `auto` parses text that begins with `<speak`; `true` forces SSML; `false` disables |
| `normalize` | `true` | expand numbers, dates, abbreviations… (`false` = take the text literally) |
| `dateorder` | `mdy` | `dmy` reads `28/10/2026` day-first |
| `year` | `auto` | `cardinal` reads 4-digit numbers as cardinals ("one thousand nine hundred…") |
| `and` | `false` | British "one hundred **and** five" |
| `spell_unknown` | `false` | also spell unknown 4-letter ALL-CAPS words |
| `style` | – | style for the whole text, e.g. `style=sad` (see *Vocal styles*) |
| `intensity` | `1.0` | strength of `style` (0 – 2.5) |
| `tags` | `true` | parse `[style]` / `[event]` tags in the text; `false` speaks them literally |
| `strict_tags` | `false` | an unknown `[tag]` is an error (with a "did you mean" hint) instead of plain text |
| `slang` | `true` | chat spellings: `u`, `ur`, `r`, `pls`, `thx`, `idk`, `omg`, `btw`, … |
| `caps` | `true` | ALL-CAPS words (3+ letters, in mixed-case text) are emphasised |
| `bits` | `16` | `to_wav` only: 16, 24, 32 or `float` |

## Text normalisation

`tts.normalize(text)` shows exactly what is spoken:

| Input | Spoken |
|---|---|
| `1,234` · `-5` · `3.14` | one thousand two hundred thirty four · minus five · three point one four |
| `1984` · `2005` · `the 1990s` | nineteen eighty four · two thousand five · the nineteen nineties |
| `21st` · `3rd` | twenty first · third |
| `$5.50` · `£20` · `$2.5 billion` · `₹500` | five dollars and fifty cents · twenty pounds · two point five billion dollars · five hundred rupees |
| `12.5%` · `-5°C` · `4.5GB` · `100km/h` | twelve point five percent · minus five degrees Celsius · four point five gigabytes · one hundred kilometers per hour |
| `3:30 pm` · `7:05` · `17:00` | three thirty P M · seven oh five · seventeen hundred hours |
| `10/28/2026` · `2026-10-28` | October twenty eighth twenty twenty six |
| `1/2` · `3/4` · `2x` | one half · three quarters · two times |
| `pages 5-10` · `555-1234` | pages five to ten · five five five, one two three four |
| `Dr.` `Mrs.` `Prof.` `St.` `vs.` `e.g.` `i.e.` `etc.` | doctor, missus, professor, saint/street (by context), versus, for example, that is, et cetera |
| `NASA` · `FBI` · `USB` | spoken as a word · spelled "F B I" · spelled "U S B" |
| `camelCase` · `snake_case` · `HTTPServer2` | camel Case · snake case · H T T P Server two |
| `me@x.com` · `https://a.com/b` | me at x dot com · H T T P … a dot com slash b |
| `A & B` · `#1` · `@home` | A and B · number one · at home |
| `*very*` | emphasis (stronger accent, longer) |

A blank line is a paragraph break (longer pause). Accented Latin letters are folded (`café` → `cafe`), smart quotes
and dashes are understood, and unsupported scripts are skipped rather than mispronounced.

## Pronunciation

For each word the engine tries, in order:

1. **Homograph rules** — `read` (present/past), `record`, `present`, `object`, `project`, `use`, `close`, `live`, `lead`,
   `wind`, `tear`, `bass`, `bow`, `minute`, `used to`, … chosen from the neighbouring words.
2. **Your lexicon** (`add_word`, `load_lexicon`) → **built-in dictionary** (~1,200 function words, numbers, names and
   irregular spellings).
3. **The** before vowels (`DH IY0`), possessives (`tiger's`), contractions (`don't`, `they'll`).
4. **Morphology** — plurals/3rd person with correct voicing (`dogs` /z/, `cats` /s/, `boxes` /ɪz/), `-ed` (/t/ /d/ /ɪd/),
   `-ing`, `-er/-est`, `-ly`, `-ness`, `-less`, `-ment`, `-ful`, `-able`, prefixes `un- re- pre- dis- mis- over- under-…` —
   *on any stem that is in the lexicon or dictionary*, so one `add_word` covers all its inflections.
5. **Compound splitting** (`keyboard` = key + board).
6. **Acronyms** — short or vowel-less ALL-CAPS words are spelled letter by letter.
7. **Letter-to-sound rules** — ~330 context-sensitive rules (NRL style) for silent letters, `tion/sion/ture`, soft c/g,
   `ough`, `ph`, `gh`, magic-e, r-controlled vowels, … followed by **stress assignment** (suffix rules `-tion -ic -ity
   -ology -eer`, prefix rules, Latin penult weight, secondary stress) and **vowel reduction**.

`tts.explain("word")` tells you which path was used (`source`: `lexicon`, `dictionary`, `homograph`, `morphology`,
`compound`, `acronym`, `rules`) plus phonemes, IPA, syllable count and the stressed syllable.

### Fixing a pronunciation

```kc
tts.add_word("kobold", "K OW1 B OW0 L D");        // ARPAbet with stress digits (0 none, 1 primary, 2 secondary)
tts.add_word("tomato", "təˈmeɪtoʊ");              // or IPA
tts.add_words("alpha=AE1 L F AH0\nbeta  B EY1 T AH0");   // bulk: "word=PH PH" or CMUdict-style "word  PH PH"
tts.load_lexicon("cmudict.dict");                  // a full CMUdict gives dictionary-grade coverage
tts.add_sub("kc", "king cobra");                   // whole-word text substitution (before pronunciation)
```

Inline, without touching the lexicon: `"say {K IH1 NG K OW1 B R AH0} now"` (ARPAbet or IPA between braces).
`save_lexicon(path)` writes your entries back out; `clear_lexicon()` forgets them.

### Phoneme set

`tts.phoneme_table()` lists all 41 symbols. ARPAbet: vowels `AA AE AH AO AW AY EH ER EY IH IY OW OY UH UW`
(stress digit 0/1/2 — unstressed `AH0` is a schwa, `ER0` is /ɚ/); consonants `B CH D DH F G HH JH K L M N NG P R S SH T TH
V W Y Z ZH`, plus the flap `DX` and the glottal stop `Q`. `tts.to_ipa` / `tts.from_ipa` convert both ways, including stress
marks `ˈ ˌ`, `tʃ dʒ aɪ aʊ eɪ oʊ ɔɪ ɝ ɚ ə`.

`speak_phonemes("HH AH0 L OW1 | W ER1 L D .")` speaks phonemes directly: `|` separates words, `, . ? ! ; :` set phrasing.

## Vocal styles and expressive tags

Put a tag in square brackets anywhere in the text. A **style** tag changes how everything after it is spoken, until the
next style tag; a **sound event** tag produces a non-verbal sound right there.

```kc
tts.to_wav("[Happily] Hi it's great to meet u... [dramatically] but u didn't come...", "out.wav", "voice=female");
tts.to_wav("Oh no [sigh] I really thought we had it. [sad] It's over. [laugh] Just kidding! [excited] We won!", "out.wav");
tts.speak("The door creaked open.", { style: "mysterious" });          // style for the whole text
```

The tag text is forgiving: case does not matter, adverbs and typos are understood — `[Happily]`, `[happy]`, `[wisper]`,
`[dramitaclly]`, `[sarcasticaly]` all work. `tts.resolve_tag("wisper")` shows what a tag resolves to; `strict_tags=true`
turns unknown tags into errors that suggest the closest match. Brackets that are not tags (`[note]`, `[1]`) stay ordinary text.

### Tag grammar

| Tag | Meaning |
|---|---|
| `[sad]` | switch to a style (replaces the previous style) |
| `[very sad]` `[slightly sad]` `[a bit sad]` `[extremely sad]` | intensity words (0.5 … 1.8×) |
| `[sad:0.5]` `[sad 150%]` `[sad x2]` | numeric intensity |
| `[sad+whisper]` `[sad, tired]` `[sad and tired]` | combine styles |
| `[+whisper]` | add a style on top of the current one |
| `[/whisper]` | remove that style · `[/]` `[neutral]` `[normal]` `[reset]` remove all |
| `[pitch=+3]` `[rate=1.3]` `[volume=0.6]` `[breath=0.2]` … | set one parameter (several per tag: `[pitch=+3 rate=1.3]`) |
| `[voice=female]` `[/voice]` | switch voice for the following words |
| `[pause]` `[long pause]` `[short pause]` `[pause 1.5s]` `[pause=300ms]` | silence |
| `[laugh]` `[long laugh]` `[laugh:2]` | sound events; longer/stronger with intensity |
| `[ugh]` `[aww]` `[wow]` | interjections (spoken sounds with their own mood) |

### Styles

**Moods:** happy excited sad depressed angry scared nervous surprised disgusted calm tired bored serious dramatic
sarcastic playful mysterious evil confident romantic gentle warm cold shy pleading apologetic curious urgent crying
laughing storyteller announcer formal casual drunk

**Manner:** whisper soft loud shout slow fast high low deep squeaky robotic monotone breathy creaky trembling singsong
emphatic smiling tense

Each style also answers to synonyms (`cheerful`, `furious`, `terrified`, `exhausted`, `yell`, `quiet`, …) and their adverb
forms (`happily`, `angrily`, `nervously`, `dramatically`, …). `tts.styles()` lists the names, `tts.style_info("sad")`
shows what a style does.

### Sound events

`[laugh]` `[giggle]` `[chuckle]` `[cackle]` `[sigh]` `[gasp]` `[sniff]` `[sob]`/`[cry]` `[cough]` `[clear throat]`/`[ahem]`
`[hmm]` `[yawn]` `[scream]` `[groan]` `[inhale]` `[exhale]` `[breath]`. They are synthesized by the formant engine
(voiced pulses, breath noise, nasal murmur, pitch shapes), take the speaker's voice and current style (a laugh in a
`[whisper]` is whispered), and appear in `align()` with start/end times (`word: "[laugh]"`, `event: "laugh"`).

Interjections: `[ugh]` `[aww]` `[wow]` `[whoa]` `[oh]` `[ooh]` `[ah]` `[aha]` `[hey]` `[yay]` `[oops]` `[uh]` `[um]` `[huh]`
`[phew]` `[yikes]` `[ew]` `[shh]` `[psst]` `[ha]` `[ouch]` `[oof]` `[yum]` `[tada]`.

### What a style changes

A style is a bundle of 20 parameters, applied smoothly (no clicks at tag boundaries):

| Group | Parameters |
|---|---|
| Pitch | `pitch` (semitones) · `range` (intonation width) · `accent` (height of word accents) · `decl` (declination) · `fall` (final fall) · `lilt` (sing-song alternation) · `tremor` + `tremor_hz` (shaky pitch) |
| Timing | `rate` · `pauses` (length of pauses) · `artic` (consonant strength/length) |
| Voice quality | `breath` · `whisper` (0–1, voicing → noise) · `tilt` (darker/brighter) · `oq` (breathy ↔ pressed) · `jitter` · `shimmer` · `creak` (vocal fry) · `formant` (vocal-tract size: smile/frown) · `amod` (amplitude wobble when laughing) |
| Level | `volume` (a whole-text `[soft]`/`[loud]` is really quieter/louder — loudness is not re-normalised away) |

Intensity scales a style's *deviation from neutral*; combinations multiply or add per parameter.

### Your own styles

```kc
tts.define_style("grumpy", "angry", { pitch: -3, rate: 0.9, creak: 0.3, desc: "low and gruff", aliases: "cranky" });
tts.speak("[grumpy] Get off my lawn.");
```

`define_style(name, base, options)` copies `base` (or `""` for neutral) and overrides any of the parameters above; the new
name (and `aliases`, plus their adverb forms) then works in tags and in the `style` option. A style cannot reuse the name
of a sound event or an interjection.

### Text conventions that shape delivery

* `...` is a **trail-off**: longer pause, the voice fades and hangs. `?!`, `!!` are more emphatic.
* Chat spellings are expanded (`u` → you, `r` → are, `ur`, `pls`, `thx`, `idk`, `omg`, `btw`, `tbh`, `gr8`, …; `slang=false` to disable).
* Stretched words are slowed down: `sooo`, `noooo`, `pleeease`, `heyyy`.
* ALL-CAPS words get emphasis (`I said STOP`); acronyms (`NASA`, `FBI`) do not.
* Tags work inside SSML too, and `<style name="sad" amount="0.5">…</style>` is available as an element.

## Prosody

* Grammatical words (`the`, `of`, `can`, pronouns, …) are reduced and shortened; **content words carry the pitch accents**
  on their stressed syllable.
* Pitch follows a declining baseline with accent peaks; **statements fall**, **yes/no questions rise** at the end,
  **wh-questions fall**, commas give a **continuation rise**, `!` is more emphatic, `*word*` is emphasised.
* Klatt-style durations: stress, consonant clusters, shortening before voiceless consonants, **phrase-final lengthening**,
  aspirated vs. unaspirated stops (`pin` vs. `spin`), US flapping.
* Pauses come from punctuation (`comma`, `period`, `paragraph`, `pause` options) or `<break>`.

## Voices

`male` (default), `female`, `child`, `teen`, `deep`, `elder`, `narrator`, `whisper`, `robot`, `alien`. Each is a bundle of
`f0` (Hz), `range`, `tract` (vocal-tract scale: formants × tract), `oq` (open quotient), `tilt`, `breath`, `jitter`,
`shimmer`, `rate`, `bw` (formant bandwidth scale), and the `whisper` / `robot` flags.

```kc
tts.define_voice("hero", "male", { f0: 135, tract: 1.08, breath: 0.08, desc: "warm and slightly airy" });
tts.speak("Greetings.", { voice: "hero", pitch: -1 });
```

`define_voice(name, base_voice, options)` (object or string) creates (or replaces) a voice; up to 32 voices. Values are validated
(`f0` 40–800, `tract` 0.6–2.0, `oq` 0.3–0.9, `rate` 0.3–3).

## SSML subset

Text starting with `<speak` is parsed as SSML (or pass `ssml=true`):

```xml
<speak>
  Hello <break time="600ms"/> world.
  <prosody rate="slow" pitch="-2st" volume="soft">Quietly.</prosody>
  <emphasis level="strong">really</emphasis>
  <phoneme alphabet="ipa" ph="ˈkɪŋ">king</phoneme>  <sub alias="World Wide Web">WWW</sub>
  <say-as interpret-as="characters">USB</say-as>  <say-as interpret-as="digits">4072</say-as>
  <voice name="female">Hi.</voice>  <mark name="here"/>
  <p>Paragraph.</p> <s>Sentence.</s>
</speak>
```

Supported: `break` (`time`, `strength`), `prosody` (`rate`: words or `%`, `pitch`: `st`, `%`, words, `volume`: dB, words),
`emphasis`, `phoneme` (`ipa` or `arpabet`), `sub`, `say-as` (`characters`, `digits`, `telephone`, `ordinal`, `cardinal`, `year`),
`voice`, `mark`, `p`, `s`, plus the standard entities. Unknown tags are ignored.

## Timing, subtitles and lip-sync

```kc
let t = tts.align("Good morning, world.");
// t.duration, t.words[i] = {word, start, end, phonemes, phones: [{ph, start, end, viseme}]}, t.marks = [{name, time}]
```

Times are seconds from the start of the audio (including the lead-in). `viseme` is a 0–15 mouth-shape id
(0 silence, 1 open, 2 mid-front, 3 spread, 4 round-open, 5 round, 6 neutral, 7 lips closed, 8 lip-teeth, 9 tongue-teeth,
10 alveolar, 11 post-alveolar, 12 velar, 13 r, 14 w, 15 y). `align_phonemes` does the same for phoneme input and
`duration(text)` returns just the length.

## How it works

```
text ─► clean-up ─► normalise ─► tokenise (+SSML, {phonemes}, markers) ─► G2P (lexicon · dictionary · morphology ·
 rules · stress) ─► plan (units → durations → articulatory segments → F0 contour) ─► formant synthesizer ─► audio
```

The synthesizer is a cascade/parallel Klatt design: a Rosenberg glottal pulse (open quotient, tilt, jitter, shimmer,
breathiness) through five formant resonators plus a nasal pole/zero pair; aspiration through the same cascade;
frication and stop bursts through a parallel band-pass; voiced fricatives are modulated by the glottal cycle; whisper
replaces voicing with aspiration. Formant tracks coarticulate across phone boundaries and stops glide from their place
locus into the next vowel.

## Notes and limits

* Styles and events are generated by the same formant engine: they are convincing as *cues* (a laugh, a sigh, a cough)
  rather than as recordings.
* English only. Other scripts are skipped; foreign words go through English rules (add them with `add_word`).
* The built-in dictionary is deliberately compact; the rule engine is ~85–90% accurate on regular vocabulary. For a
  specific domain, load a pronouncing dictionary (`load_lexicon`) or list your terms with `add_words`.
* Roman numerals, chemical formulas and tables are not interpreted; write them the way you want them spoken.
* Text up to 400,000 characters per call. Calls that write files or play sound are refused in `comptime`.
* The lexicon, substitutions and custom voices are global to the running program.
