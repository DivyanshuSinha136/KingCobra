/* KingCobra std.tts — vocal styles.
 *
 * A *style* is a bundle of 20 voice-quality and prosody parameters (pitch, range, rate, volume, breathiness, whisper,
 * spectral tilt, open quotient, jitter/shimmer, tremor, creak, formant scale, pause length, accent height,
 * declination, articulation, final fall, lilt, amplitude modulation). Tags such as [happy], [sad], [whisper],
 * [dramatically] select one or more styles; [very sad], [sad:0.5], [sad+whisper], [+whisper], [/whisper] and
 * parameter tags like [pitch=+3] [rate=1.2] refine them. Sound events ([laugh] [sigh] [gasp] ...) and interjections
 * ([ugh] [aww] [wow] ...) are resolved here too. Misspelled tags ("wisper", "dramitaclly") are corrected by
 * fuzzy matching.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "tts_internal.h"
#include <ctype.h>
#include <math.h>

/* ================================================================== fields */
enum { F_PITCH, F_RANGE, F_RATE, F_VOL, F_BREATH, F_WHISPER, F_TILT, F_OQ, F_JITTER, F_SHIMMER, F_TREMOR, F_TREMOR_HZ, F_CREAK, F_FORMANT, F_PAUSE, F_ACCENT, F_DECL, F_ARTIC, F_FALL, F_LILT, F_AMOD, F_COUNT };
static const char *const FIELD_NAME[F_COUNT] = { "pitch", "range", "rate", "volume", "breath", "whisper", "tilt", "oq", "jitter", "shimmer", "tremor", "tremor_hz", "creak", "formant", "pauses", "accent", "decl", "artic", "fall", "lilt", "amod" };
static const bool FIELD_MUL[F_COUNT] = { 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0 };

static float *fptr(TtsStyle *s, int f) {
    switch (f) { case F_PITCH: return &s->pitch; case F_RANGE: return &s->range; case F_RATE: return &s->rate; case F_VOL: return &s->vol; case F_BREATH: return &s->breath; case F_WHISPER: return &s->whisper;
                 case F_TILT: return &s->tilt; case F_OQ: return &s->oq; case F_JITTER: return &s->jitter; case F_SHIMMER: return &s->shimmer; case F_TREMOR: return &s->tremor; case F_TREMOR_HZ: return &s->tremor_hz;
                 case F_CREAK: return &s->creak; case F_FORMANT: return &s->formant; case F_PAUSE: return &s->pause; case F_ACCENT: return &s->accent; case F_DECL: return &s->decl; case F_ARTIC: return &s->artic;
                 case F_FALL: return &s->fall; case F_LILT: return &s->lilt; case F_AMOD: return &s->amod; }
    return NULL;
}
void tts_style_neutral(TtsStyle *s) {
    memset(s, 0, sizeof *s);
    s->range = s->rate = s->vol = s->formant = s->pause = s->accent = s->decl = s->artic = s->fall = 1.0f; s->tremor_hz = 5.5f;
}
int tts_style_field(const char *n) {
    static const struct { const char *a; int f; } AL[] = { { "speed", F_RATE }, { "vol", F_VOL }, { "gain", F_VOL }, { "tract", F_FORMANT }, { "pausescale", F_PAUSE }, { "pitchrange", F_RANGE }, { "intonation", F_RANGE }, { "tremorhz", F_TREMOR_HZ } };
    for (int i = 0; i < F_COUNT; i++) if (!strcmp(n, FIELD_NAME[i])) return i;
    for (size_t i = 0; i < sizeof AL / sizeof AL[0]; i++) if (!strcmp(n, AL[i].a)) return AL[i].f;
    return -1;
}
void tts_style_set_field(TtsStyle *s, int f, float v) { float *p = fptr(s, f); if (p) *p = v; }
float tts_style_get_field(const TtsStyle *s, int f) { float *p = fptr((TtsStyle *)s, f); return p ? *p : 0; }

static void style_parse(TtsStyle *s, const char *spec) {
    char buf[512]; snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        char *eq = strchr(tok, '='); if (!eq) continue; *eq = 0;
        int f = tts_style_field(tok); if (f >= 0) tts_style_set_field(s, f, (float)atof(eq + 1));
    }
}

/* ================================================================== built-in styles */
typedef struct { char name[24], cat[8], desc[72]; TtsStyle p; } SDef;
#define MAX_STYLES 160
static SDef S[MAX_STYLES]; static int NS = 0;

typedef struct { char k[28]; int type, idx; } Key;
#define MAX_KEYS 3000
static Key K[MAX_KEYS]; static int NK = 0;

static const struct { const char *name, *cat, *desc, *spec, *aliases; } BUILTIN[] = {
    /* ---- moods ---- */
    { "happy", "mood", "upbeat, smiling, bright", "pitch=2 range=1.35 rate=1.08 volume=1.05 formant=1.03 tilt=-0.05 accent=1.2 decl=0.7 pauses=0.9 lilt=0.25", "cheerful joyful glad delighted pleased merry upbeat joyous content sunny bright smiling cheery" },
    { "excited", "mood", "high energy, fast, wide pitch", "pitch=3 range=1.6 rate=1.2 volume=1.15 formant=1.04 tilt=-0.1 accent=1.4 decl=0.5 artic=1.1 jitter=0.003 pauses=0.8", "thrilled enthusiastic eager energetic hyped animated lively pumped elated ecstatic" },
    { "sad", "mood", "low, slow, falling, a little breathy", "pitch=-2 range=0.6 rate=0.82 volume=0.85 breath=0.12 tilt=0.2 formant=0.98 decl=1.4 accent=0.7 pauses=1.3 fall=1.4 creak=0.25 tremor=0.12", "unhappy sorrowful down blue glum mournful sorry saddened dejected somber sombre" },
    { "depressed", "mood", "flat, heavy, hopeless", "pitch=-3.5 range=0.35 rate=0.72 volume=0.75 breath=0.15 tilt=0.3 formant=0.97 decl=1.6 accent=0.5 pauses=1.6 fall=1.5 creak=0.5 artic=0.8", "hopeless miserable despairing gloomy melancholy heartbroken devastated" },
    { "angry", "mood", "pressed, loud, hard consonants", "pitch=1 range=1.3 rate=1.1 volume=1.3 tilt=-0.15 oq=-0.12 shimmer=0.03 accent=1.5 artic=1.35 pauses=0.8", "mad furious irate annoyed irritated cross livid enraged fuming outraged hostile" },
    { "scared", "mood", "high, trembling, breathy", "pitch=3 range=1.2 rate=1.15 volume=0.95 breath=0.18 tremor=0.5 tremor_hz=7 jitter=0.01 formant=1.03 pauses=0.8", "afraid frightened terrified fearful panicked petrified horrified alarmed" },
    { "nervous", "mood", "shaky, hesitant", "pitch=1.5 range=1.1 rate=1.08 volume=0.92 breath=0.1 tremor=0.25 tremor_hz=6.5 jitter=0.006 formant=1.02", "anxious worried uneasy jittery timid tense apprehensive unsure hesitant" },
    { "surprised", "mood", "bright, high, wide", "pitch=3.5 range=1.7 rate=1.1 volume=1.1 formant=1.04 accent=1.4 decl=0.5", "shocked amazed astonished startled stunned astounded flabbergasted" },
    { "disgusted", "mood", "low, drawn out, tight", "pitch=-1 range=1.0 rate=0.88 formant=0.97 tilt=0.1 artic=1.1 accent=1.2 creak=0.15", "revolted grossed repulsed sickened appalled" },
    { "calm", "mood", "relaxed, slow, even", "pitch=-1 range=0.7 rate=0.88 volume=0.9 breath=0.05 tilt=0.1 pauses=1.2 accent=0.85", "relaxed peaceful serene soothing tranquil composed soothed placid mellow" },
    { "tired", "mood", "low energy, creaky, falling", "pitch=-2.5 range=0.45 rate=0.78 volume=0.8 breath=0.1 creak=0.35 decl=1.5 artic=0.8 pauses=1.3", "sleepy exhausted weary drowsy fatigued worn lethargic" },
    { "bored", "mood", "flat, drawn out", "pitch=-1.5 range=0.35 rate=0.88 volume=0.9 decl=1.3 artic=0.85 accent=0.6 pauses=1.1", "uninterested indifferent apathetic dull unimpressed" },
    { "serious", "mood", "low, firm, deliberate", "pitch=-2 range=0.7 rate=0.9 tilt=-0.05 artic=1.15 pauses=1.2 fall=1.2", "stern grave solemn firm earnest severe" },
    { "dramatic", "mood", "slow, wide, theatrical, long pauses", "pitch=-0.5 range=1.8 rate=0.85 volume=1.1 accent=1.7 pauses=1.5 artic=1.2 fall=1.3 decl=0.8", "theatrical theatrics expressive intense operatic grandiose" },
    { "sarcastic", "mood", "drawn out, exaggerated, dry", "pitch=-1 range=1.5 rate=0.9 formant=0.98 accent=1.4 decl=0.6 pauses=1.2 lilt=0.5 artic=1.1", "ironic mocking snarky sardonic snide wry deadpan" },
    { "playful", "mood", "bouncy, teasing", "pitch=1.5 range=1.5 rate=1.05 accent=1.3 lilt=0.8 pauses=0.9 formant=1.02", "teasing mischievous cheeky silly flirty fun naughty impish" },
    { "mysterious", "mood", "hushed, slow, ominous", "whisper=0.35 pitch=-2 range=0.8 rate=0.8 breath=0.2 volume=0.8 pauses=1.5 fall=1.2", "eerie spooky creepy ominous suspenseful enigmatic secretive" },
    { "evil", "mood", "dark, low, menacing", "pitch=-4 range=0.8 rate=0.8 formant=0.95 tilt=0.1 creak=0.25 pauses=1.4 artic=1.15 fall=1.3", "sinister menacing villainous wicked malevolent threatening" },
    { "confident", "mood", "firm, resolved, strong falls", "pitch=-1 range=0.9 volume=1.1 tilt=-0.05 artic=1.1 fall=1.3 accent=1.1", "assured authoritative commanding proud assertive decisive" },
    { "romantic", "mood", "close, breathy, slow", "pitch=-1.5 range=0.7 rate=0.8 breath=0.25 tilt=0.2 volume=0.8 pauses=1.3", "seductive sultry loving tender intimate affectionate passionate" },
    { "gentle", "mood", "soft, kind, warm", "pitch=-0.5 range=0.8 rate=0.92 volume=0.75 breath=0.1 tilt=0.12 oq=0.05 pauses=1.1", "kind caring sweet comforting tender soothing nurturing" },
    { "warm", "mood", "friendly, welcoming", "pitch=0.5 range=1.15 rate=0.97 tilt=0.05 formant=1.01 oq=0.03", "friendly welcoming kindly cordial affable" },
    { "cold", "mood", "flat, detached", "pitch=-1 range=0.4 rate=0.95 tilt=0.05 artic=1.1 decl=1.1", "icy detached unemotional distant aloof clinical flat" },
    { "shy", "mood", "small, breathy, hesitant", "pitch=1 range=0.7 rate=0.9 volume=0.7 breath=0.15 tremor=0.1 formant=1.02", "timid bashful embarrassed meek sheepish" },
    { "pleading", "mood", "begging, tremulous", "pitch=1.5 range=1.2 rate=0.95 breath=0.1 tremor=0.2 tremor_hz=6 volume=0.95", "begging desperate imploring urgentplea" },
    { "apologetic", "mood", "soft, low, slow", "pitch=-1 range=0.7 rate=0.88 volume=0.8 breath=0.08 pauses=1.2", "regretful remorseful contrite ashamed" },
    { "curious", "mood", "rising, light", "pitch=1.2 range=1.3 rate=1.0 lilt=0.3 accent=1.15", "inquisitive intrigued interested wondering" },
    { "urgent", "mood", "fast, tense, higher", "pitch=1.5 range=1.2 rate=1.25 volume=1.15 accent=1.3 pauses=0.7 artic=1.1", "hurried emergency rushed pressing desperate-haste panicky" },
    { "crying", "mood", "tearful, breaking voice", "pitch=0.5 range=0.9 rate=0.82 volume=0.85 breath=0.2 tremor=0.8 tremor_hz=6 jitter=0.02 creak=0.2 decl=1.3 pauses=1.2 formant=1.02", "sobbing weeping tearful teary wailing" },
    { "laughing", "mood", "speaking while laughing", "pitch=2 range=1.4 rate=1.05 breath=0.2 amod=0.35 tremor=0.5 tremor_hz=7.5 accent=1.2 formant=1.03", "giggly amused chuckling gigglingly cracking-up" },
    { "storyteller", "mood", "narrating, varied, warm pauses", "range=1.5 rate=0.9 accent=1.3 pauses=1.25 artic=1.05 fall=1.15", "narration narrative bedtime fairytale" },
    { "announcer", "mood", "clear, bright, projected", "pitch=0.5 range=1.3 rate=1.05 volume=1.15 artic=1.25 accent=1.3 tilt=-0.08", "broadcaster newscaster presenter reporter host" },
    { "formal", "mood", "careful, polite", "pitch=-0.5 range=0.9 rate=0.95 artic=1.1 pauses=1.05", "proper polite professional businesslike" },
    { "casual", "mood", "relaxed, informal", "rate=1.05 range=1.1 artic=0.85 pauses=0.9", "informal chill laidback easygoing" },
    { "drunk", "mood", "slurred, slow, wobbly", "pitch=-1 range=1.3 rate=0.75 artic=0.5 creak=0.1 jitter=0.01 tremor=0.1 volume=1.05", "tipsy slurred intoxicated wasted" },
    /* ---- manner ---- */
    { "whisper", "manner", "unvoiced, breathy", "whisper=1 volume=0.7 rate=0.95 range=0.6 pitch=0", "whispering whispered hushed murmur murmuring whispers" },
    { "soft", "manner", "quiet and light", "volume=0.65 breath=0.1 tilt=0.15 range=0.8 rate=0.93 oq=0.05", "quiet low-volume muted subdued faint" },
    { "loud", "manner", "projected, bright", "volume=1.45 pitch=1.5 tilt=-0.18 oq=-0.08 range=1.2 artic=1.2 accent=1.3", "louder booming strong projected" },
    { "shout", "manner", "shouted, pressed, bright", "volume=1.6 pitch=3 tilt=-0.25 oq=-0.12 range=1.3 rate=0.95 artic=1.3 accent=1.5 shimmer=0.03", "yell yelling shouting bellow roar holler scream-voice" },
    { "slow", "manner", "slower speech", "rate=0.7 pauses=1.25", "slower drawn-out leisurely unhurried" },
    { "fast", "manner", "faster speech", "rate=1.4 pauses=0.75", "quick rapid speedy faster brisk" },
    { "high", "manner", "higher pitch", "pitch=4 formant=1.03", "highpitched higher shrill" },
    { "low", "manner", "lower pitch", "pitch=-3.5", "lowpitched lower" },
    { "deep", "manner", "deep, large vocal tract", "pitch=-4 formant=0.92 tilt=0.05", "bass booming-deep baritone" },
    { "squeaky", "manner", "tiny, high vocal tract", "pitch=5 formant=1.18 range=1.2", "chipmunk helium tiny mousy" },
    { "robotic", "manner", "monotone mechanical", "range=0.02 tilt=0.1 jitter=0 shimmer=0 accent=1 fall=0.2 rate=0.97", "robot mechanical android synthetic machine monotone-robot" },
    { "monotone", "manner", "flat pitch", "range=0.1 accent=0.5", "flat droning deadpan-flat" },
    { "breathy", "manner", "airy voice", "breath=0.3 oq=0.1 tilt=0.1", "airy" },
    { "creaky", "manner", "vocal fry", "creak=0.7 pitch=-1", "fry vocalfry gravelly" },
    { "trembling", "manner", "shaky voice", "tremor=0.6 tremor_hz=6 jitter=0.012", "shaky wavering quivering shivering" },
    { "singsong", "manner", "bouncy, sing-song", "lilt=1 range=1.6 accent=1.2", "sing-song sing" },
    { "emphatic", "manner", "forceful, stressed", "accent=1.6 artic=1.2 rate=0.95 pauses=1.15 volume=1.1", "emphasized emphasised forceful stressed insistent" },
    { "smiling", "manner", "audible smile", "pitch=1 formant=1.04 tilt=-0.05 range=1.15", "smile smiley grinning" },
    { "tense", "manner", "tight, pressed", "oq=-0.1 tilt=-0.1 pitch=1 jitter=0.004", "tight strained pressed" },
};
#define NBUILTIN ((int)(sizeof BUILTIN / sizeof BUILTIN[0]))

/* ---- sound events and interjections ---- */
static const struct { const char *name, *desc, *aliases; } EVENTS[] = {
    { "laugh", "a laugh: ha-ha-ha", "laughs laughter haha hahaha lol lmao chortle guffaw laughing-out-loud" },
    { "giggle", "a quick, high giggle", "giggles giggling hehe teehee tehee titter" },
    { "chuckle", "a low chuckle: heh heh", "chuckles chuckling snicker snickers heh" },
    { "cackle", "a wicked cackle", "cackles cackling evillaugh evillaughter mwahaha muahaha bwahaha" },
    { "sigh", "a breathy sigh", "sighs sighing" },
    { "gasp", "a sharp inhale", "gasps gasping" },
    { "sniff", "a sniff", "sniffs sniffle sniffles sniffling" },
    { "sob", "a sob", "sobs cry cries weep weeps wail whimper whimpers" },
    { "cough", "a cough", "coughs coughing" },
    { "throat", "clearing the throat: ahem", "clearthroat clearsthroat clearingthroat ahem" },
    { "hmm", "a thinking hum", "hm hmmm mm mmm mmhm mhm uhhuh" },
    { "yawn", "a long yawn", "yawns yawning" },
    { "scream", "a scream", "screams screaming shriek shrieks screech" },
    { "groan", "a groan", "groans moan moans grunt grunts" },
    { "inhale", "a breath in", "inhales breathein inhalation intake" },
    { "exhale", "a breath out", "exhales breatheout exhalation puff" },
    { "breath", "a breath in and out", "breathe breathing breaths takeabreath deepbreath" },
};
#define NEVENTS ((int)(sizeof EVENTS / sizeof EVENTS[0]))
static const struct { const char *name, *phones; float pitch, stretch; const char *mood; const char *aliases; } INTERJ[] = {
    { "ugh", "AH1 G", -1, 1.4, "disgusted", "ughh blech bleh yuck" },
    { "aww", "AO1 W", 1, 1.9, "gentle", "awww aw awh" },
    { "wow", "W AW1", 2, 1.5, "excited", "whoa-wow" },
    { "whoa", "W OW1", 2, 1.5, "surprised", "woah" },
    { "oh", "OW1", 0, 1.2, "", "ohh" },
    { "ooh", "UW1", 1.5, 1.7, "playful", "oooh ohhh" },
    { "ah", "AA1", 0, 1.3, "", "ahh" },
    { "aha", "AH0 HH AA1", 2, 1.2, "excited", "ahah" },
    { "hey", "HH EY1", 1.5, 1.1, "", "heyy" },
    { "yay", "Y EY1", 3, 1.6, "happy", "yayy hooray hurray woohoo" },
    { "oops", "UW1 P S", 1, 1.2, "", "whoops oopsie" },
    { "uh", "AH0", 0, 0.9, "", "er erm" },
    { "um", "AH0 M", 0, 1.0, "", "umm uhm" },
    { "huh", "HH AH1", 2, 1.1, "curious", "" },
    { "phew", "F Y UW1", 0, 1.5, "tired", "whew" },
    { "yikes", "Y AY1 K S", 3, 1.2, "scared", "" },
    { "ew", "IY1 UW1", 0, 1.4, "disgusted", "eww eeew" },
    { "shh", "SH", 0, 3.0, "", "shush hush sh" },
    { "psst", "P S T", 0, 1.3, "", "pst" },
    { "ha", "HH AA1", 3, 0.9, "happy", "hah" },
    { "ouch", "AW1 CH", 3, 1.0, "", "ow" },
    { "oof", "UH1 F", -1, 1.0, "", "oomph" },
    { "yum", "Y AH1 M", 1, 1.3, "happy", "mmmyum yummy" },
    { "tada", "T AH0 D AA1", 2, 1.2, "excited", "ta-da" },
};
#define NINTERJ ((int)(sizeof INTERJ / sizeof INTERJ[0]))

static void key_add(const char *k, int type, int idx) {
    if (NK >= MAX_KEYS - 4) return;
    char t[28]; int n = 0;
    for (; *k && n < 26; k++) if (isalpha((unsigned char)*k)) t[n++] = (char)tolower((unsigned char)*k);
    t[n] = 0; if (!n) return;
    for (int i = 0; i < NK; i++) if (!strcmp(K[i].k, t)) { if (K[i].type == type) return; return; }   /* first definition wins */
    snprintf(K[NK].k, sizeof K[NK].k, "%s", t); K[NK].type = type; K[NK].idx = idx; NK++;
}
static void key_with_adverbs(const char *w, int type, int idx) {
    key_add(w, type, idx);
    size_t L = strlen(w); char t[40]; if (L < 3 || L > 18) return;
    if (L > 2 && w[L - 1] == 'y') { snprintf(t, sizeof t, "%.*sily", (int)L - 1, w); key_add(t, type, idx); }
    else if (L > 3 && !strcmp(w + L - 2, "ic")) { snprintf(t, sizeof t, "%sally", w); key_add(t, type, idx); }
    else if (L > 3 && !strcmp(w + L - 2, "le")) { snprintf(t, sizeof t, "%.*sy", (int)L - 1, w); key_add(t, type, idx); }
    else if (L > 3 && !strcmp(w + L - 3, "ful")) { snprintf(t, sizeof t, "%sly", w); key_add(t, type, idx); }
    snprintf(t, sizeof t, "%sly", w); key_add(t, type, idx);
    if (L > 3 && !strcmp(w + L - 2, "ed")) { snprintf(t, sizeof t, "%sly", w); key_add(t, type, idx); }
}
static void style_keys(int i, const char *aliases) {
    key_with_adverbs(S[i].name, 0, i);
    char buf[512]; snprintf(buf, sizeof buf, "%s", aliases ? aliases : "");
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) key_with_adverbs(tok, 0, i);
}
static bool g_init = false;
static void style_init(void) {
    if (g_init) return; g_init = true;
    for (int i = 0; i < NBUILTIN && NS < MAX_STYLES; i++) {
        SDef *d = &S[NS]; memset(d, 0, sizeof *d);
        snprintf(d->name, sizeof d->name, "%s", BUILTIN[i].name); snprintf(d->cat, sizeof d->cat, "%s", BUILTIN[i].cat); snprintf(d->desc, sizeof d->desc, "%s", BUILTIN[i].desc);
        tts_style_neutral(&d->p); style_parse(&d->p, BUILTIN[i].spec); NS++;
        style_keys(NS - 1, BUILTIN[i].aliases);
    }
    for (int i = 0; i < NEVENTS; i++) {
        key_add(EVENTS[i].name, 1, i);
        char buf[256]; snprintf(buf, sizeof buf, "%s", EVENTS[i].aliases);
        for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) key_add(tok, 1, i);
    }
    for (int i = 0; i < NINTERJ; i++) {
        key_add(INTERJ[i].name, 2, i);
        char buf[128]; snprintf(buf, sizeof buf, "%s", INTERJ[i].aliases);
        for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) key_add(tok, 2, i);
    }
    static const char *const PAUSES[] = { "pause", "beat", "silence", "shortpause", "longpause", "dramaticpause", "longerpause", "briefpause", "smallpause", "bigpause", "awkwardpause", "hesitation" };
    for (size_t i = 0; i < sizeof PAUSES / sizeof PAUSES[0]; i++) key_add(PAUSES[i], 3, (int)i);
    static const char *const RESETS[] = { "neutral", "normal", "reset", "default", "natural", "plain", "clear", "none", "off", "end", "regular", "standard" };
    for (size_t i = 0; i < sizeof RESETS / sizeof RESETS[0]; i++) key_add(RESETS[i], 4, 0);
}
int tts_style_count(void) { style_init(); return NS + 1; }       /* index 0 = neutral */
const char *tts_style_name(int i) { style_init(); return i == 0 ? "neutral" : (i >= 1 && i <= NS) ? S[i - 1].name : ""; }
const char *tts_style_cat(int i) { style_init(); return i == 0 ? "reset" : (i >= 1 && i <= NS) ? S[i - 1].cat : ""; }
const char *tts_style_desc(int i) { style_init(); return i == 0 ? "no styling" : (i >= 1 && i <= NS) ? S[i - 1].desc : ""; }
const TtsStyle *tts_style_at(int i) { static TtsStyle neutral; style_init(); if (i >= 1 && i <= NS) return &S[i - 1].p; tts_style_neutral(&neutral); return &neutral; }
int tts_event_count(void) { return NEVENTS; }
const char *tts_event_name(int i) { return i >= 0 && i < NEVENTS ? EVENTS[i].name : ""; }
int tts_interj_count(void) { return NINTERJ; }
const char *tts_interj_name(int i) { return i >= 0 && i < NINTERJ ? INTERJ[i].name : ""; }
const char *tts_interj_phones(int i, float *pitch, float *stretch, const char **mood) { if (pitch) *pitch = INTERJ[i].pitch; if (stretch) *stretch = INTERJ[i].stretch; if (mood) *mood = INTERJ[i].mood; return INTERJ[i].phones; }

/* ================================================================== matching */
static int dl_dist(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b); if (la > 30 || lb > 30) return 99;
    int d[32][32];
    for (int i = 0; i <= la; i++) d[i][0] = i; for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) for (int j = 1; j <= lb; j++) {
        int c = a[i - 1] == b[j - 1] ? 0 : 1, v = d[i - 1][j] + 1;
        if (d[i][j - 1] + 1 < v) v = d[i][j - 1] + 1; if (d[i - 1][j - 1] + c < v) v = d[i - 1][j - 1] + c;
        if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && d[i - 2][j - 2] + 1 < v) v = d[i - 2][j - 2] + 1;
        d[i][j] = v;
    }
    return d[la][lb];
}
static const Key *key_exact(const char *k) { for (int i = 0; i < NK; i++) if (!strcmp(K[i].k, k)) return &K[i]; return NULL; }
static const Key *key_lookup(const char *k, bool fuzzy, const Key **near) {
    style_init();
    const Key *e = key_exact(k); if (e) return e;
    static const struct { const char *suf, *rep; } SUF[] = { { "ically", "ic" }, { "ily", "y" }, { "ly", "" }, { "ing", "" }, { "ing", "e" }, { "ed", "" }, { "ed", "e" }, { "s", "" }, { "es", "" }, { "ness", "" }, { "ful", "" } };
    size_t L = strlen(k);
    for (size_t i = 0; i < sizeof SUF / sizeof SUF[0]; i++) {
        size_t sl = strlen(SUF[i].suf); if (L <= sl + 2 || strcmp(k + L - sl, SUF[i].suf)) continue;
        char t[40]; snprintf(t, sizeof t, "%.*s%s", (int)(L - sl), k, SUF[i].rep); if ((e = key_exact(t))) return e;
    }
    int maxd = L <= 4 ? 0 : L <= 6 ? 1 : L <= 9 ? 2 : 3;
    const Key *best = NULL; int bd = 99;
    for (int i = 0; i < NK; i++) {
        int d = dl_dist(k, K[i].k);
        if (d < bd || (d == bd && best && strlen(K[i].k) > strlen(best->k) && 0)) { bd = d; best = &K[i]; }
    }
    if (near) *near = (best && bd <= 3) ? best : NULL;
    if (fuzzy && best && bd <= maxd && (bd <= 1 || best->k[0] == k[0])) return best;
    return NULL;
}
static const char *key_name(const Key *k) {
    switch (k->type) { case 0: return S[k->idx].name; case 1: return EVENTS[k->idx].name; case 2: return INTERJ[k->idx].name; case 3: return "pause"; default: return "neutral"; }
}
const char *tts_style_canon(const char *text) {
    style_init(); char k[40]; int n = 0;
    for (; *text && n < 38; text++) if (isalpha((unsigned char)*text)) k[n++] = (char)tolower((unsigned char)*text);
    k[n] = 0; if (!n) return NULL;
    const Key *e = key_lookup(k, true, NULL); return e ? key_name(e) : NULL;
}
int tts_style_find(const char *name) {
    style_init(); char k[40]; int n = 0;
    for (; *name && n < 38; name++) if (isalpha((unsigned char)*name)) k[n++] = (char)tolower((unsigned char)*name);
    k[n] = 0; const Key *e = key_lookup(k, true, NULL);
    return (e && e->type == 0) ? e->idx + 1 : (e && e->type == 4) ? 0 : -1;
}

/* ================================================================== tag parser */
static float intensity_word(const char *w) {
    static const struct { const char *w; float v; } T[] = { { "slightly", 0.5f }, { "slight", 0.5f }, { "little", 0.5f }, { "bit", 0.5f }, { "mildly", 0.5f }, { "tiny", 0.4f }, { "somewhat", 0.7f }, { "kinda", 0.7f }, { "sorta", 0.7f }, { "fairly", 0.8f },
        { "quite", 1.2f }, { "very", 1.4f }, { "really", 1.4f }, { "so", 1.3f }, { "too", 1.4f }, { "extremely", 1.8f }, { "super", 1.8f }, { "totally", 1.6f }, { "deeply", 1.6f }, { "intensely", 1.7f }, { "utterly", 1.8f }, { "incredibly", 1.8f }, { "ultra", 1.8f }, { "more", 1.3f }, { "less", 0.6f }, { "long", 1.5f }, { "longer", 1.7f }, { "short", 0.6f }, { "brief", 0.6f }, { "big", 1.4f }, { "huge", 1.8f } };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (!strcmp(w, T[i].w)) return T[i].v;
    return 0;
}
static double parse_ms(const char *s) { char *e; double v = strtod(s, &e); if (e == s) return -1; if (!strncmp(e, "ms", 2)) return v; if (*e == 's' || *e == 0) return *e == 0 && v > 20 ? v : v * 1000.0; return v; }
static bool parse_part(const char *in, TagOp *op, char *suggest, size_t sn) {
    char s[96]; size_t L = 0; for (; *in && L < 90; in++) s[L++] = (char)tolower((unsigned char)*in); s[L] = 0;
    while (L && isspace((unsigned char)s[L - 1])) s[--L] = 0; char *p = s; while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return false;
    memset(op, 0, sizeof *op); op->amount = 1.0f;
    /* key=value / key:value */
    char *sep = strpbrk(p, "=:");
    if (sep) {
        char kname[40]; size_t kl = (size_t)(sep - p); if (kl >= sizeof kname) return false; memcpy(kname, p, kl); kname[kl] = 0;
        while (kl && isspace((unsigned char)kname[kl - 1])) kname[--kl] = 0;
        const char *val = sep + 1; while (*val == ' ') val++;
        if (!strcmp(kname, "voice") || !strcmp(kname, "speaker")) { op->type = TOP_VOICE; snprintf(op->name, sizeof op->name, "%s", val); return *val != 0; }
        if (!strcmp(kname, "pause") || !strcmp(kname, "wait") || !strcmp(kname, "break")) { double ms = parse_ms(val); if (ms < 0) return false; op->type = TOP_PAUSE; op->value = (float)ms; return true; }
        int f = tts_style_field(kname);
        if (f >= 0 && *sep == '=') { char *e; double v = strtod(val, &e); if (e == val) return false; if (*e == '%') v = v / 100.0 * (FIELD_MUL[f] ? 1.0 : 1.0); op->type = TOP_PARAM; op->field = f; op->value = (float)v; return true; }
        /* "sad:0.5" or "sad=0.5": intensity */
        char *e; double v = strtod(val, &e);
        if (e != val) { if (*e == '%') v /= 100.0; op->amount = (float)v; memmove(p + 0, kname, strlen(kname) + 1); }
        else return false;
    }
    /* words: leading intensity words, trailing number */
    char *w[10]; int nw = 0; for (char *tok = strtok(p, " \t-_"); tok && nw < 10; tok = strtok(NULL, " \t-_")) w[nw++] = tok;
    if (!nw) return false;
    int a = 0, b = nw;
    { char full[48]; int fn = 0; for (int i = 0; i < nw; i++) for (const char *c = w[i]; *c && fn < 44; c++) if (isalpha((unsigned char)*c)) full[fn++] = *c; full[fn] = 0;
      const Key *fe = fn ? key_exact(full) : NULL;
      if (fe && fe->type != 4) { snprintf(op->name, sizeof op->name, "%s", key_name(fe));
          switch (fe->type) { case 0: op->type = TOP_STYLE; op->idx = fe->idx + 1; return true; case 1: op->type = TOP_EVENT; op->idx = fe->idx; return true; case 2: op->type = TOP_INTERJ; op->idx = fe->idx; return true;
                              case 3: { static const float MS[] = { 450, 300, 600, 200, 1000, 1100, 1000, 220, 200, 1200, 1400, 500 }; op->type = TOP_PAUSE; op->value = MS[fe->idx % 12]; return true; } default: break; } }
      if (nw >= 2 && (!strcmp(w[0], "pause") || !strcmp(w[0], "wait") || !strcmp(w[0], "hold") || !strcmp(w[0], "silence") || !strcmp(w[0], "beat")) && (isdigit((unsigned char)w[1][0]) || w[1][0] == '.')) {
          double ms = parse_ms(w[1]); if (ms >= 0) { op->type = TOP_PAUSE; op->value = (float)ms; return true; } }
    }
    while (a < b - 1) { float k = intensity_word(w[a]); if (k > 0) { op->amount *= k; a++; } else if (!strcmp(w[a], "a") || !strcmp(w[a], "of")) a++; else break; }
    if (b - a >= 2) { const char *last = w[b - 1]; char *e; double v = strtod(last[0] == 'x' || last[0] == '*' ? last + 1 : last, &e); if (e != (last[0] == 'x' || last[0] == '*' ? last + 1 : last) && (!*e || *e == '%')) { op->amount *= (float)(*e == '%' ? v / 100.0 : v); b--; } }
    if (b - a >= 2 && (!strcmp(w[b - 1], "more") || !strcmp(w[b - 1], "less"))) { op->amount *= !strcmp(w[b - 1], "more") ? 1.3f : 0.6f; b--; }
    char key[48]; int kn = 0;
    for (int i = a; i < b; i++) for (const char *c = w[i]; *c && kn < 44; c++) if (isalpha((unsigned char)*c)) key[kn++] = *c;
    key[kn] = 0; if (!kn) return false;
    if (op->amount < 0.05f) op->amount = 0.05f; if (op->amount > 2.5f) op->amount = 2.5f;
    /* bare numbers like "1" / "2" are footnote markers, not tags */
    const Key *near = NULL; const Key *e = key_lookup(key, true, &near);
    if (!e) {
        int vi = tts_voice_find(key);
        if (vi >= 0) { op->type = TOP_VOICE; snprintf(op->name, sizeof op->name, "%s", tts_voice_at(vi)->name); return true; }
        if (suggest && sn && near) snprintf(suggest, sn, "%s", key_name(near));
        return false;
    }
    snprintf(op->name, sizeof op->name, "%s", key_name(e));
    switch (e->type) {
    case 0: op->type = TOP_STYLE; op->idx = e->idx + 1; return true;
    case 1: op->type = TOP_EVENT; op->idx = e->idx; return true;
    case 2: op->type = TOP_INTERJ; op->idx = e->idx; return true;
    case 3: { static const float MS[] = { 450, 300, 600, 200, 1000, 1100, 1000, 220, 200, 1200, 1400, 500 }; op->type = TOP_PAUSE; op->value = MS[e->idx % 12] * (op->amount > 0 ? op->amount : 1); return true; }
    default: op->type = TOP_RESET; return true;
    }
}
bool tts_tag_parse(const char *inner, TagParse *out, char *suggest, size_t sn) {
    style_init(); memset(out, 0, sizeof *out); if (suggest && sn) suggest[0] = 0;
    char buf[160]; size_t L = 0; while (*inner == ' ') inner++; for (; *inner && L < 150; inner++) buf[L++] = *inner; buf[L] = 0;
    while (L && buf[L - 1] == ' ') buf[--L] = 0;
    if (!L) return false;
    char *p = buf;
    if (*p == '/') { out->closing = true; p++; while (*p == ' ') p++; if (!*p) { out->n = 1; out->op[0].type = TOP_RESET; return true; } }
    if (*p == '+') { out->add = true; p++; }
    if (strchr(p, '=')) for (char *c = p; *c; c++) if (*c == ' ') { char *j = c + 1; while (*j == ' ') j++; char *k = j; while (*k && *k != ' ') k++; if (memchr(j, '=', (size_t)(k - j))) *c = ','; }
    bool any_alpha = false; for (const char *c = p; *c; c++) if (isalpha((unsigned char)*c)) any_alpha = true;
    if (!any_alpha) return false;
    /* split into parts at + , & ; and " and " */
    char *parts[8]; int np = 0; char *q = p; parts[np++] = q;
    for (; *q && np < 8; q++) {
        if (((*q == '+' && !(q > p && (q[-1] == '=' || q[-1] == ':'))) || *q == ',' || *q == '&' || *q == ';')) { *q = 0; parts[np++] = q + 1; }
        else if (!strncasecmp(q, " and ", 5)) { *q = 0; parts[np++] = q + 5; q += 4; }
    }
    for (int i = 0; i < np; i++) {
        if (out->n >= 8) break;
        if (!parse_part(parts[i], &out->op[out->n], suggest, sn)) return false;
        out->n++;
    }
    return out->n > 0;
}

/* ================================================================== composition */
void tts_style_compose(TtsStyle *out, const TtsStyle *const *st, const float *amount, int n) {
    tts_style_neutral(out);
    float tz = 0, tr = 0;
    for (int f = 0; f < F_COUNT; f++) {
        if (f == F_TREMOR_HZ) continue;
        double acc = FIELD_MUL[f] ? 1.0 : 0.0;
        for (int i = 0; i < n; i++) {
            double v = tts_style_get_field(st[i], f), k = amount[i];
            if (FIELD_MUL[f]) { if (v < 0.001) v = 0.001; acc *= pow(v, k); } else acc += v * k;
        }
        tts_style_set_field(out, f, (float)acc);
    }
    for (int i = 0; i < n; i++) if (st[i]->tremor * amount[i] > tr) { tr = st[i]->tremor * (float)amount[i]; tz = st[i]->tremor_hz; }
    out->tremor_hz = tz > 0 ? tz : 5.5f;
    if (out->whisper > 1) out->whisper = 1; if (out->whisper < 0) out->whisper = 0; if (out->creak > 1) out->creak = 1; if (out->creak < 0) out->creak = 0;
    if (out->breath > 0.6f) out->breath = 0.6f; if (out->breath < 0) out->breath = 0;
    if (out->range < 0.02f) out->range = 0.02f; if (out->range > 3) out->range = 3;
    if (out->rate < 0.3f) out->rate = 0.3f; if (out->rate > 3) out->rate = 3;
    if (out->vol > 3) out->vol = 3; if (out->pitch > 14) out->pitch = 14; if (out->pitch < -14) out->pitch = -14;
    if (out->formant < 0.7f) out->formant = 0.7f; if (out->formant > 1.4f) out->formant = 1.4f;
    if (out->oq < -0.25f) out->oq = -0.25f; if (out->oq > 0.3f) out->oq = 0.3f;
    if (out->tilt < -0.4f) out->tilt = -0.4f; if (out->tilt > 0.6f) out->tilt = 0.6f;
}

/* ================================================================== custom styles */
int tts_style_define(const char *name, const char *base, KcValue opts, KcPos pos) {
    style_init();
    int bi = 0; if (base && *base) { bi = tts_style_find(base); if (bi < 0) kc_error_at(pos, "tts.define_style: unknown base style '%s'", base); }
    TtsStyle p = *tts_style_at(bi);
    for (int f = 0; f < F_COUNT; f++) {
        char w[40]; snprintf(w, sizeof w, "tts.define_style");
        KcValue *v = mn_opt_get(opts, FIELD_NAME[f]);
        if (v && v->type != VAL_NULL) tts_style_set_field(&p, f, (float)mn_opt_num(opts, FIELD_NAME[f], 0, pos, w));
    }
    static const struct { const char *a; int f; } AL[] = { { "speed", F_RATE }, { "vol", F_VOL }, { "tract", F_FORMANT } };
    for (size_t i = 0; i < sizeof AL / sizeof AL[0]; i++) { KcValue *v = mn_opt_get(opts, AL[i].a); if (v && v->type != VAL_NULL) tts_style_set_field(&p, AL[i].f, (float)mn_opt_num(opts, AL[i].a, 0, pos, "tts.define_style")); }
    if (p.range < 0 || p.range > 3) kc_error_at(pos, "tts.define_style: range must be 0..3");
    if (p.rate < 0.3f || p.rate > 3) kc_error_at(pos, "tts.define_style: rate must be 0.3..3");
    if (p.vol < 0 || p.vol > 3) kc_error_at(pos, "tts.define_style: volume must be 0..3");
    if (p.pitch < -14 || p.pitch > 14) kc_error_at(pos, "tts.define_style: pitch must be -14..14 semitones");
    if (p.whisper < 0 || p.whisper > 1 || p.creak < 0 || p.creak > 1) kc_error_at(pos, "tts.define_style: whisper and creak must be 0..1");
    if (p.formant < 0.7f || p.formant > 1.4f) kc_error_at(pos, "tts.define_style: formant must be 0.7..1.4");
    char key[28]; int kn = 0; for (const char *c = name; *c && kn < 24; c++) if (isalpha((unsigned char)*c)) key[kn++] = (char)tolower((unsigned char)*c); key[kn] = 0;
    if (!kn) kc_error_at(pos, "tts.define_style: the name must contain letters");
    int idx = -1; for (int i = 0; i < NS; i++) { char k2[28]; int m = 0; for (const char *c = S[i].name; *c && m < 24; c++) if (isalpha((unsigned char)*c)) k2[m++] = (char)tolower((unsigned char)*c); k2[m] = 0; if (!strcmp(k2, key)) idx = i; }
    const Key *clash = key_exact(key);
    if (idx < 0 && clash && clash->type != 0) kc_error_at(pos, "tts.define_style: '%s' is already a sound event, interjection or reserved tag", name);
    if (idx < 0) { if (NS >= MAX_STYLES) kc_error_at(pos, "tts.define_style: too many styles (limit %d)", MAX_STYLES); idx = NS++; memset(&S[idx], 0, sizeof S[idx]); snprintf(S[idx].name, sizeof S[idx].name, "%s", key); style_keys(idx, NULL); }
    S[idx].p = p; snprintf(S[idx].cat, sizeof S[idx].cat, "custom");
    const char *d = mn_opt_str(opts, "desc", "custom style", pos, "tts.define_style"); snprintf(S[idx].desc, sizeof S[idx].desc, "%s", d);
    const char *al = mn_opt_str(opts, "aliases", NULL, pos, "tts.define_style");
    if (al) { char buf[256]; snprintf(buf, sizeof buf, "%s", al); for (char *tok = strtok(buf, ", "); tok; tok = strtok(NULL, ", ")) key_with_adverbs(tok, 0, idx); }
    return idx + 1;
}
