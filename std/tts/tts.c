/* KingCobra std.tts — text-to-speech.
 *
 * Pure native, zero external dependencies:
 *   text front end  numbers, years, dates, times, currency, units, abbreviations, acronyms, URLs, camelCase, SSML subset
 *   pronunciation   user lexicon + built-in dictionary + morphology + compounds + homograph disambiguation +
 *                   NRL-style letter-to-sound rules + stress assignment; ARPAbet and IPA in/out; CMUdict loader
 *   prosody         phrasing, pitch accents, declination, question/continuation intonation, emphasis, Klatt durations
 *   synthesis       formant synthesizer with ten voices (and custom ones), whisper/robot modes, optional room reverb
 *   output          audio buffers shared with std.music_note (save_wav, play, DSP chain), word/phoneme/viseme timeline
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#endif
#include "tts_internal.h"
#include <ctype.h>
#include <math.h>

#define SETK(o, k, v) kc_object_set((o), (k), strlen(k), (v))
#define MAX_TEXT 400000

KcValue tts_audio_value(MnAudio *a) { return mn_audio_wrap(a); }

/* ============================================================ options */
void tts_opts_default(TtsOpts *o) {
    memset(o, 0, sizeof *o);
    o->voice = 0; o->rate = 1.0; o->pitch_st = 0; o->f0 = 0; o->range = 1.0; o->volume = 1.0; o->pause_scale = 1.0;
    o->comma_ms = 170; o->period_ms = 380; o->para_ms = 700; o->reverb = 0; o->breath = 0; o->tilt = 0; o->emph_gain = 1.0;
    o->sr = 22050; o->flap = true; o->uk = false; o->normalize = true; o->ssml = false; o->ssml_auto = true; o->caps_emph = false;
    o->trim = false; o->seed = 1; o->tags = true; o->strict_tags = false; o->slang = true; o->caps_emph = true; o->style[0] = 0; o->style_amount = 1.0;
}
void tts_opts_parse(TtsOpts *o, KcValue v, KcPos pos, const char *who) {
    tts_opts_default(o);
    if (v.type == VAL_NULL) return;
    char w[40]; snprintf(w, sizeof w, "tts.%s", who);
    const char *vs = mn_opt_str(v, "voice", NULL, pos, w);
    if (vs) { int vi = tts_voice_find(vs); if (vi < 0) { kc_error_at(pos, "tts.%s: unknown voice '%s' (see tts.voices())", who, vs); } o->voice = vi; }
    const TtsVoice *V = tts_voice_at(o->voice);
    o->rate = mn_opt_num(v, "rate", 1.0, pos, w);
    double wpm = mn_opt_num(v, "wpm", 0, pos, w); if (wpm > 0) o->rate = wpm / 170.0;
    o->pitch_st = mn_opt_num(v, "pitch", 0, pos, w);
    o->f0 = mn_opt_num(v, "f0", 0, pos, w);
    o->range = mn_opt_num(v, "range", mn_opt_num(v, "intonation", 1.0, pos, w), pos, w);
    o->volume = mn_opt_num(v, "volume", 1.0, pos, w);
    o->pause_scale = mn_opt_num(v, "pause", 1.0, pos, w);
    o->comma_ms = mn_opt_num(v, "comma", 170, pos, w); o->period_ms = mn_opt_num(v, "period", 380, pos, w); o->para_ms = mn_opt_num(v, "paragraph", 700, pos, w);
    o->reverb = mn_opt_num(v, "reverb", 0, pos, w); o->breath = mn_opt_num(v, "breath", 0, pos, w); o->tilt = mn_opt_num(v, "tilt", 0, pos, w);
    o->sr = (int)mn_opt_int(v, "sr", mn_opt_int(v, "samplerate", 22050, pos, w), pos, w);
    o->flap = mn_opt_bool(v, "flap", true, pos, w); o->normalize = mn_opt_bool(v, "normalize", true, pos, w);
    o->trim = mn_opt_bool(v, "trim", false, pos, w); o->and_word = mn_opt_bool(v, "and", false, pos, w);
    o->spell_unknown = mn_opt_bool(v, "spell_unknown", false, pos, w);
    o->tags = mn_opt_bool(v, "tags", true, pos, w); o->strict_tags = mn_opt_bool(v, "strict_tags", false, pos, w); o->slang = mn_opt_bool(v, "slang", true, pos, w); o->caps_emph = mn_opt_bool(v, "caps", true, pos, w);
    { const char *sty = mn_opt_str(v, "style", NULL, pos, w); if (sty) snprintf(o->style, sizeof o->style, "%s", sty); o->style_amount = mn_opt_num(v, "intensity", mn_opt_num(v, "amount", 1.0, pos, w), pos, w); if (o->style_amount < 0 || o->style_amount > 2.5) kc_error_at(pos, "tts.%s: intensity must be 0..2.5", who); }
    o->seed = (uint32_t)mn_opt_int(v, "seed", 1, pos, w);
    const char *acc = mn_opt_str(v, "accent", "us", pos, w);
    if (!strcasecmp(acc, "uk") || !strcasecmp(acc, "gb") || !strcasecmp(acc, "british")) o->uk = true;
    else if (strcasecmp(acc, "us") && strcasecmp(acc, "american")) kc_error_at(pos, "tts.%s: accent must be \"us\" or \"uk\"", who);
    const char *dord = mn_opt_str(v, "dateorder", "mdy", pos, w); o->dateorder_dmy = !strcasecmp(dord, "dmy");
    const char *yr = mn_opt_str(v, "year", "auto", pos, w); o->year_cardinal = !strcasecmp(yr, "cardinal");
    KcValue *sv = mn_opt_get(v, "ssml");
    if (sv && sv->type != VAL_NULL) { if (sv->type == VAL_STRING && !strcasecmp(sv->as.s->data, "auto")) { o->ssml = false; o->ssml_auto = true; } else { o->ssml = mn_opt_bool(v, "ssml", false, pos, w); o->ssml_auto = false; } }
    if (o->rate < 0.25 || o->rate > 4.0) kc_error_at(pos, "tts.%s: rate must be 0.25..4 (or use wpm=80..600)", who);
    if (o->sr < 8000 || o->sr > 48000) kc_error_at(pos, "tts.%s: sr must be 8000..48000", who);
    if (o->range < 0 || o->range > 3) kc_error_at(pos, "tts.%s: range must be 0..3", who);
    if (o->volume < 0 || o->volume > 4) kc_error_at(pos, "tts.%s: volume must be 0..4", who);
    if (o->pitch_st < -24 || o->pitch_st > 24) kc_error_at(pos, "tts.%s: pitch is in semitones, -24..24", who);
    (void)V;
}
/* a bare voice name ("female") is accepted wherever options are */
static KcValue opts_arg(KcValue *a, int n, int idx) {
    if (n <= idx) return kc_null();
    if (a[idx].type == VAL_STRING && !strchr(a[idx].as.s->data, '=') && tts_voice_find(a[idx].as.s->data) >= 0) {
        char t[64]; snprintf(t, sizeof t, "voice=%s", a[idx].as.s->data); return kc_string(t);
    }
    return a[idx];
}

/* ============================================================ rendering */
static const char *text_arg(KcValue v, KcPos pos, const char *who) {
    if (v.type != VAL_STRING) kc_error_at(pos, "tts.%s: text must be a string, got %s", who, kc_type_name(v));
    if (v.as.s->len > MAX_TEXT) kc_error_at(pos, "tts.%s: text too long (limit %d characters)", who, MAX_TEXT);
    return v.as.s->data;
}
static void prepare(const char *text, const TtsOpts *o, TtsWords *ws, TtsPlan *pl, KcPos pos) {
    memset(ws, 0, sizeof *ws);
    tts_tokenize(text, o, ws, pos);
    tts_plan_build(ws, o, pl);
}
static MnAudio *render_words(const TtsWords *ws, const TtsOpts *o, TtsPlan *pl, KcPos pos, const char *who) {
    tts_plan_build(ws, o, pl);
    return tts_synth(pl, o, pos, who);
}

/* speak(text[, options]) -> audio */
static KcValue f_speak(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "speak(text[, options]) -> audio   (options: voice rate wpm pitch f0 range volume sr pause comma period paragraph accent reverb ssml ...)");
    mn_set_ip(ip);
    TtsOpts o; tts_opts_parse(&o, opts_arg(a, n, 1), pos, "speak");
    TtsWords ws; TtsPlan pl; prepare(text_arg(a[0], pos, "speak"), &o, &ws, &pl, pos);
    MnAudio *au = tts_synth(&pl, &o, pos, "speak");
    tts_plan_free(&pl); tts_words_free(&ws);
    return tts_audio_value(au);
}
/* to_wav(text, path[, options]) -> {path, seconds, frames, bytes} */
static KcValue f_to_wav(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "tts.to_wav");
    mn_need(n, 2, 3, pos, "to_wav(text, path[, options]) -> {path, seconds, frames, bytes}   (extra option: bits=16|24|32|float)");
    mn_set_ip(ip);
    TtsOpts o; tts_opts_parse(&o, opts_arg(a, n, 2), pos, "to_wav");
    KcValue ov = n > 2 ? opts_arg(a, n, 2) : kc_null();
    TtsWords ws; TtsPlan pl; prepare(text_arg(a[0], pos, "to_wav"), &o, &ws, &pl, pos);
    MnAudio *au = tts_synth(&pl, &o, pos, "to_wav");
    const char *path = mn_str(a[1], pos, "to_wav", "path");
    int bits = 16; KcValue *bv = mn_opt_get(ov, "bits"); if (bv && bv->type == VAL_INT) bits = (int)bv->as.i; else if (bv && bv->type == VAL_STRING && !strcasecmp(bv->as.s->data, "float")) bits = 32;
    int bytes = mn_wav_write(au, path, bits, mn_opt_bool(ov, "dither", true, pos, "to_wav"), pos, "to_wav");
    KcValue r = kc_object_new();
    SETK(r, "path", kc_string(path)); SETK(r, "seconds", kc_float((double)au->frames / au->rate)); SETK(r, "frames", kc_int((int64_t)au->frames)); SETK(r, "bytes", kc_int(bytes));
    mn_audio_free(au); tts_plan_free(&pl); tts_words_free(&ws);
    return r;
}
/* say(text[, options]) -> bool : speak through the system audio player */
static KcValue f_say(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "tts.say");
    mn_need(n, 1, 2, pos, "say(text[, options]) -> bool   (blocks until playback ends)");
    mn_set_ip(ip);
    TtsOpts o; tts_opts_parse(&o, opts_arg(a, n, 1), pos, "say");
    TtsWords ws; TtsPlan pl; prepare(text_arg(a[0], pos, "say"), &o, &ws, &pl, pos);
    MnAudio *au = tts_synth(&pl, &o, pos, "say");
    tts_plan_free(&pl); tts_words_free(&ws);
    KcValue h = tts_audio_value(au);
    KcValue r = mn_play_audio(ip, h, pos);
    if (au) { mn_audio_free(au); h.as.handle->ptr = NULL; }
    return r;
}
/* save_wav(audio, path[, options]) -> bytes */
static KcValue f_save_wav(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "tts.save_wav");
    mn_need(n, 2, 3, pos, "save_wav(audio, path[, bits=16|24|32|float]) -> bytes");
    MnAudio *au = mn_audio_arg(a[0], pos, "save_wav"); KcValue ov = n > 2 ? a[2] : kc_null();
    int bits = 16; KcValue *bv = mn_opt_get(ov, "bits"); if (bv && bv->type == VAL_INT) bits = (int)bv->as.i; else if (bv && bv->type == VAL_STRING && !strcasecmp(bv->as.s->data, "float")) bits = 32;
    return kc_int(mn_wav_write(au, mn_str(a[1], pos, "save_wav", "path"), bits, mn_opt_bool(ov, "dither", true, pos, "save_wav"), pos, "save_wav"));
}
static KcValue f_free(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "free(audio)");
    if (!mn_is_audio(a[0])) kc_error_at(pos, "tts.free: expected an audio buffer");
    if (a[0].as.handle->ptr) { mn_audio_free((MnAudio *)a[0].as.handle->ptr); a[0].as.handle->ptr = NULL; }
    return kc_null();
}
/* duration(text[, options]) -> seconds (no audio is synthesised) */
static KcValue f_duration(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "duration(text[, options]) -> seconds");
    TtsOpts o; tts_opts_parse(&o, opts_arg(a, n, 1), pos, "duration");
    TtsWords ws; TtsPlan pl; prepare(text_arg(a[0], pos, "duration"), &o, &ws, &pl, pos);
    double s = pl.total_ms / 1000.0; tts_plan_free(&pl); tts_words_free(&ws);
    return kc_float(s);
}

/* ============================================================ phonetics */
static char *format_word(const TtsWord *w, bool ipa, bool digits) { return tts_phseq_format(w->ph, w->nph, ipa, digits); }
static const char *src_name(int s) {
    switch (s) { case SRC_LEXICON: return "lexicon"; case SRC_DICT: return "dictionary"; case SRC_MORPH: return "morphology"; case SRC_COMPOUND: return "compound"; case SRC_RULES: return "rules";
                 case SRC_SPELL: return "spelled"; case SRC_PHONES: return "phonemes"; case SRC_HOMOGRAPH: return "homograph"; case SRC_ACRONYM: return "acronym"; default: return "punctuation"; }
}
static KcValue fmt_string(const TtsWords *ws, const TtsOpts *o, bool ipa, bool digits, bool bars) {
    TBuf b; tb_init(&b); bool first = true; (void)o;
    for (int i = 0; i < ws->n; i++) {
        const TtsWord *w = &ws->w[i];
        if (w->kind == WK_PUNCT) { if (bars) { tb_puts(&b, first ? "" : " "); tb_putc(&b, w->punc); first = false; } continue; }
        if (w->kind != WK_WORD && w->kind != WK_PHONES) continue;
        char *s = format_word(w, ipa, digits);
        if (!first) tb_puts(&b, bars ? (ipa ? " " : " | ") : " ");
        if (bars && !first && ws->w[i - 1].kind == WK_PUNCT) { /* punctuation already separated */ }
        tb_puts(&b, s); free(s); first = false;
    }
    char *r = tb_take(&b); KcValue v = kc_string(r); free(r); return v;
}
static bool want_ipa(KcValue ov, KcPos pos, const char *who) {
    const char *f = mn_opt_str(ov, "format", "arpabet", pos, who);
    if (!strcasecmp(f, "ipa")) return true;
    if (strcasecmp(f, "arpabet") && strcasecmp(f, "arpa")) kc_error_at(pos, "tts.%s: format must be \"arpabet\" or \"ipa\"", who);
    return false;
}
/* phonemes(text[, "format=ipa stress=false words=false"]) -> string */
static KcValue f_phonemes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "phonemes(text[, options]) -> string   (options: format=arpabet|ipa  stress=true  words=true  accent=us|uk  + normalisation options)");
    KcValue ov = n > 1 ? opts_arg(a, n, 1) : kc_null();
    TtsOpts o; tts_opts_parse(&o, ov, pos, "phonemes");
    TtsWords ws; memset(&ws, 0, sizeof ws); tts_tokenize(text_arg(a[0], pos, "phonemes"), &o, &ws, pos);
    KcValue r = fmt_string(&ws, &o, want_ipa(ov, pos, "phonemes"), mn_opt_bool(ov, "stress", true, pos, "phonemes"), mn_opt_bool(ov, "words", true, pos, "phonemes"));
    tts_words_free(&ws); return r;
}
static KcValue f_ipa(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "ipa(text[, options]) -> string");
    TtsOpts o; tts_opts_parse(&o, n > 1 ? opts_arg(a, n, 1) : kc_null(), pos, "ipa");
    TtsWords ws; memset(&ws, 0, sizeof ws); tts_tokenize(text_arg(a[0], pos, "ipa"), &o, &ws, pos);
    KcValue r = fmt_string(&ws, &o, true, true, true); tts_words_free(&ws); return r;
}
/* from_ipa("həˈloʊ") -> "HH AH0 L OW1" */
static KcValue f_from_ipa(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "from_ipa(ipa) -> arpabet string");
    TtsPh ph[128]; int k = tts_ipa_parse(mn_str(a[0], pos, "from_ipa", "ipa"), ph, 128);
    if (k < 0) kc_error_at(pos, "tts.from_ipa: unsupported IPA symbol in '%s'", a[0].as.s->data);
    char *s = tts_phseq_format(ph, k, false, true); KcValue r = kc_string(s); free(s); return r;
}
static KcValue f_to_ipa(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "to_ipa(arpabet) -> ipa string");
    TtsPh ph[128]; char bad[32] = ""; int k = tts_phseq_parse(mn_str(a[0], pos, "to_ipa", "arpabet"), ph, 128, bad, sizeof bad);
    if (k < 0) kc_error_at(pos, "tts.to_ipa: unknown phoneme '%s'", bad);
    char *s = tts_phseq_format(ph, k, true, true); KcValue r = kc_string(s); free(s); return r;
}
static KcValue style_obj(const TtsStyle *p);
static void word_obj(KcValue o, const TtsWord *w, const TtsWords *ws) {
    SETK(o, "text", kc_string(w->text));
    switch (w->kind) {
    case WK_WORD: case WK_PHONES: {
        char *ar = format_word(w, false, true), *ip = format_word(w, true, true);
        SETK(o, "kind", kc_string(w->kind == WK_PHONES ? "phones" : "word")); SETK(o, "phonemes", kc_string(ar)); SETK(o, "ipa", kc_string(ip));
        SETK(o, "source", kc_string(src_name(w->src))); SETK(o, "syllables", kc_int(tts_syllables(w->ph, w->nph))); SETK(o, "content", kc_bool(w->content));
        free(ar); free(ip); break; }
    case WK_PUNCT: SETK(o, "kind", kc_string("punct")); break;
    case WK_BREAK: SETK(o, "kind", kc_string("break")); SETK(o, "ms", kc_float(w->brk_ms)); break;
    case WK_MARK: SETK(o, "kind", kc_string("mark")); break;
    case WK_STYLE: SETK(o, "kind", kc_string("style")); SETK(o, "tag", kc_string(w->text)); if (ws && w->style >= 0 && w->style < ws->nsty) SETK(o, "style", style_obj(&ws->sty[w->style])); break;
    case WK_EVENT: SETK(o, "kind", kc_string("event")); SETK(o, "event", kc_string(w->text)); SETK(o, "amount", kc_float(w->amount)); break;
    }
}
static KcValue style_obj(const TtsStyle *p) {
    KcValue o = kc_object_new();
    SETK(o, "pitch", kc_float(p->pitch)); SETK(o, "range", kc_float(p->range)); SETK(o, "rate", kc_float(p->rate)); SETK(o, "volume", kc_float(p->vol)); SETK(o, "breath", kc_float(p->breath));
    SETK(o, "whisper", kc_float(p->whisper)); SETK(o, "tilt", kc_float(p->tilt)); SETK(o, "oq", kc_float(p->oq)); SETK(o, "jitter", kc_float(p->jitter)); SETK(o, "shimmer", kc_float(p->shimmer));
    SETK(o, "tremor", kc_float(p->tremor)); SETK(o, "tremor_hz", kc_float(p->tremor_hz)); SETK(o, "creak", kc_float(p->creak)); SETK(o, "formant", kc_float(p->formant)); SETK(o, "pauses", kc_float(p->pause));
    SETK(o, "accent", kc_float(p->accent)); SETK(o, "decl", kc_float(p->decl)); SETK(o, "artic", kc_float(p->artic)); SETK(o, "fall", kc_float(p->fall)); SETK(o, "lilt", kc_float(p->lilt)); SETK(o, "amod", kc_float(p->amod));
    return o;
}
/* words(text[, options]) -> [{text, kind, phonemes, ipa, source, syllables, content}] */
static KcValue f_words(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "words(text[, options]) -> array of {text, kind, phonemes, ipa, source, syllables, content}");
    TtsOpts o; tts_opts_parse(&o, n > 1 ? opts_arg(a, n, 1) : kc_null(), pos, "words");
    TtsWords ws; memset(&ws, 0, sizeof ws); tts_tokenize(text_arg(a[0], pos, "words"), &o, &ws, pos);
    KcValue arr = kc_array_new();
    for (int i = 0; i < ws.n; i++) { KcValue o2 = kc_object_new(); word_obj(o2, &ws.w[i], &ws); kc_array_push(arr, o2); }
    tts_words_free(&ws); return arr;
}
/* pronounce(word[, options]) -> "HH AH0 L OW1" */
static KcValue f_pronounce(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "pronounce(word[, format=arpabet|ipa]) -> string");
    KcValue ov = n > 1 ? opts_arg(a, n, 1) : kc_null();
    TtsOpts o; tts_opts_parse(&o, ov, pos, "pronounce");
    TtsWords ws; memset(&ws, 0, sizeof ws); tts_tokenize(text_arg(a[0], pos, "pronounce"), &o, &ws, pos);
    KcValue r = fmt_string(&ws, &o, want_ipa(ov, pos, "pronounce"), true, false); tts_words_free(&ws); return r;
}
/* explain(word) -> {word, source, phonemes, ipa, syllables, stress} */
static KcValue f_explain(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "explain(word) -> {word, source, phonemes, ipa, syllables, stressed}");
    const char *s = mn_str(a[0], pos, "explain", "word");
    TtsOpts o; tts_opts_default(&o); tts_g2p_init();
    char low[128]; size_t L = strlen(s); if (L > 100) L = 100; bool caps = L >= 2;
    for (size_t i = 0; i < L; i++) { if (islower((unsigned char)s[i])) caps = false; low[i] = (char)tolower((unsigned char)s[i]); } low[L] = 0;
    TtsWord w; memset(&w, 0, sizeof w); w.text = low; w.caps = caps; w.kind = WK_WORD;
    tts_g2p_word(&w, NULL, NULL, &o);
    KcValue r = kc_object_new(); char *ar = format_word(&w, false, true), *ip2 = format_word(&w, true, true);
    SETK(r, "word", kc_string(s)); SETK(r, "source", kc_string(src_name(w.src))); SETK(r, "phonemes", kc_string(ar)); SETK(r, "ipa", kc_string(ip2));
    SETK(r, "syllables", kc_int(tts_syllables(w.ph, w.nph)));
    int st = 0, k = 0; for (int i = 0; i < w.nph; i++) if (tts_is_vowel(w.ph[i].ph)) { if (w.ph[i].st == 1) st = k + 1; k++; }
    SETK(r, "stressed", kc_int(st));
    free(ar); free(ip2); free(w.ph); return r;
}
static void pretty_norm(const char *s, TBuf *b) {
    for (const char *p = s; *p; p++) {
        if (*p == '\x01') { const char *e = strchr(p, '\x02'); if (!e) e = p + strlen(p); for (const char *q = p + 1; q < e; q++) { if (b->n && b->d[b->n - 1] != ' ') tb_putc(b, ' '); tb_putc(b, (char)toupper((unsigned char)*q)); } p = *e ? e : e - 1; continue; }
        if (*p == '\x03') { tb_puts(b, "\n\n"); continue; } if (*p == '\x05') { tb_putc(b, '*'); if (p[1] == ' ') p++; continue; } if (*p == '\x06') { tb_putc(b, '*'); continue; }
        tb_putc(b, *p);
    }
}
/* normalize(text[, options]) -> string */
static KcValue f_normalize(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "normalize(text[, options]) -> string   (expands numbers, dates, times, money, units, abbreviations, acronyms)");
    TtsOpts o; tts_opts_parse(&o, n > 1 ? opts_arg(a, n, 1) : kc_null(), pos, "normalize");
    char *s = tts_normalize(text_arg(a[0], pos, "normalize"), &o); TBuf b; tb_init(&b); pretty_norm(s, &b); free(s);
    char *r = tb_take(&b); KcValue v = kc_string(r); free(r); return v;
}
/* number_to_words(n[, mode]) mode: "cardinal" | "ordinal" | "year" | "digits" */
static KcValue f_number_to_words(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "number_to_words(number[, \"cardinal\" | \"ordinal\" | \"year\" | \"digits\"]) -> string");
    const char *mode = n > 1 ? mn_str(a[1], pos, "number_to_words", "mode") : "cardinal";
    TBuf b; tb_init(&b);
    if (!strcasecmp(mode, "digits") && a[0].type == VAL_STRING) { for (const char *p = a[0].as.s->data; *p; p++) if (isdigit((unsigned char)*p)) { static const char *D[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine" }; if (b.n) tb_putc(&b, ' '); tb_puts(&b, D[*p - '0']); } }
    else {
        long long v = (long long)mn_num(a[0], pos, "number_to_words", "number");
        if (!strcasecmp(mode, "cardinal")) tts_cardinal_ll(&b, v, false); else if (!strcasecmp(mode, "ordinal")) tts_ordinal_ll(&b, v, false);
        else if (!strcasecmp(mode, "year")) { if (v < 1000 || v > 9999) tts_cardinal_ll(&b, v, false); else tts_year_words(&b, (int)v); }
        else if (!strcasecmp(mode, "digits")) { char t[32]; snprintf(t, sizeof t, "%lld", v); static const char *D[] = { "zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine" }; for (const char *p = t; *p; p++) if (isdigit((unsigned char)*p)) { if (b.n) tb_putc(&b, ' '); tb_puts(&b, D[*p - '0']); } }
        else kc_error_at(pos, "tts.number_to_words: mode must be cardinal, ordinal, year or digits");
    }
    char *r = tb_take(&b); KcValue v = kc_string(r); free(r); return v;
}

/* speak_phonemes("HH AH0 L OW1 | W ER1 L D .") — ARPAbet (or IPA words), '|' between words, , . ? ! ; : as phrase marks */
static void push_phone_word(TtsWords *ws, TtsPh *ph, int *n, TBuf *label) {
    if (*n <= 0) return;
    TtsWord *w = tts_words_push(ws, WK_PHONES, label->d ? label->d : ""); w->src = SRC_PHONES; w->content = 1;
    w->ph = malloc((size_t)*n * sizeof(TtsPh)); memcpy(w->ph, ph, (size_t)*n * sizeof(TtsPh)); w->nph = *n; *n = 0; label->n = 0; if (label->d) label->d[0] = 0;
}
static void phones_to_words(const char *s, TtsWords *ws, KcPos pos, const char *who) {
    TtsPh cur[128]; int nc = 0; TBuf label; tb_init(&label);
    const char *p = s;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++; if (!*p) break;
        const char *e = p; while (*e && !isspace((unsigned char)*e)) e++;
        char tok[64]; size_t L = (size_t)(e - p); if (L >= sizeof tok) L = sizeof tok - 1; memcpy(tok, p, L); tok[L] = 0; p = e;
        if (!strcmp(tok, "|")) { push_phone_word(ws, cur, &nc, &label); continue; }
        if (L == 1 && strchr(",.?!;:", tok[0])) { push_phone_word(ws, cur, &nc, &label); TtsWord *w = tts_words_push(ws, WK_PUNCT, tok); w->punc = tok[0]; w->src = SRC_PUNCT; continue; }
        bool ascii = true; for (size_t i = 0; i < L; i++) if ((unsigned char)tok[i] >= 0x80) ascii = false;
        if (!ascii) { push_phone_word(ws, cur, &nc, &label); TtsPh t2[128]; int k = tts_ipa_parse(tok, t2, 128); if (k < 0) kc_error_at(pos, "tts.%s: unsupported IPA symbol in '%s'", who, tok); TtsWord *w = tts_words_push(ws, WK_PHONES, tok); w->src = SRC_PHONES; w->content = 1; w->ph = malloc((size_t)(k ? k : 1) * sizeof(TtsPh)); memcpy(w->ph, t2, (size_t)k * sizeof(TtsPh)); w->nph = k; continue; }
        TtsPh t2[8]; char bad[32] = ""; int k = tts_phseq_parse(tok, t2, 8, bad, sizeof bad);
        if (k < 0) kc_error_at(pos, "tts.%s: unknown phoneme '%s' (ARPAbet: AA AE AH AO AW AY B CH D DH EH ER EY F G HH IH IY JH K L M N NG OW OY P R S SH T TH UH UW V W Y Z ZH, stress digits 0/1/2)", who, bad);
        for (int i = 0; i < k && nc < 128; i++) cur[nc++] = t2[i];
        if (label.n) tb_putc(&label, ' '); tb_puts(&label, tok);
    }
    push_phone_word(ws, cur, &nc, &label); tb_free(&label);
}
static KcValue f_speak_phonemes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "speak_phonemes(phonemes[, options]) -> audio   (ARPAbet like \"HH AH0 L OW1 | W ER1 L D .\" or IPA words)");
    mn_set_ip(ip);
    TtsOpts o; tts_opts_parse(&o, opts_arg(a, n, 1), pos, "speak_phonemes");
    TtsWords ws; memset(&ws, 0, sizeof ws); phones_to_words(mn_str(a[0], pos, "speak_phonemes", "phonemes"), &ws, pos, "speak_phonemes");
    TtsPlan pl; MnAudio *au = render_words(&ws, &o, &pl, pos, "speak_phonemes"); tts_plan_free(&pl); tts_words_free(&ws);
    return tts_audio_value(au);
}

/* align(text[, options]) -> {duration, words: [{word, start, end, phonemes, phones: [{ph, start, end, viseme}]}], marks: [{name, time}]} */
static KcValue align_words(TtsWords *wsp, const TtsOpts *op, KcPos pos) {
    TtsWords ws = *wsp; TtsPlan pl; tts_plan_build(&ws, op, &pl);
    KcValue r = kc_object_new(), words = kc_array_new(), marks = kc_array_new();
    for (int wi = 0; wi < ws.n; wi++) {
        const TtsWord *w = &ws.w[wi]; if (w->kind != WK_WORD && w->kind != WK_PHONES && w->kind != WK_EVENT) continue;
        float t0 = -1, t1 = 0; KcValue phs = kc_array_new(); int lastpk = -1; float pt0 = 0, pt1 = 0; const TtsSeg *first = NULL;
        #define FLUSHPH() do { if (first) { KcValue po = kc_object_new(); char nm[8]; snprintf(nm, sizeof nm, "%s%s", TTS_PH[first->ph].arpa, tts_is_vowel(first->ph) ? (first->st == 1 ? "1" : first->st == 2 ? "2" : "0") : ""); SETK(po, "ph", kc_string(nm)); SETK(po, "start", kc_float(pt0 / 1000.0)); SETK(po, "end", kc_float(pt1 / 1000.0)); SETK(po, "viseme", kc_int(TTS_PH[first->ph].viseme)); kc_array_push(phs, po); } } while (0)
        for (int i = 0; i < pl.n; i++) {
            const TtsSeg *s = &pl.s[i]; if (s->word != wi) continue;
            float e = s->t0 + s->dur; if (t0 < 0) t0 = s->t0; t1 = e;
            if (s->pk != lastpk) { FLUSHPH(); first = s; pt0 = s->t0; lastpk = s->pk; }
            pt1 = e;
        }
        FLUSHPH();
        #undef FLUSHPH
        KcValue wo = kc_object_new(); char *ar = format_word(w, false, true);
        if (w->kind == WK_EVENT) { char nm[48]; snprintf(nm, sizeof nm, "[%s]", w->text); SETK(wo, "word", kc_string(nm)); SETK(wo, "event", kc_string(w->text)); } else SETK(wo, "word", kc_string(w->text));
        SETK(wo, "start", kc_float(t0 < 0 ? 0 : t0 / 1000.0)); SETK(wo, "end", kc_float(t1 / 1000.0)); SETK(wo, "phonemes", kc_string(ar)); SETK(wo, "phones", phs);
        free(ar); kc_array_push(words, wo);
    }
    for (int m = 0; m < pl.nmark; m++) { KcValue mo = kc_object_new(); int si = pl.mark_seg[m]; SETK(mo, "name", kc_string(pl.mark_name[m])); SETK(mo, "time", kc_float(si >= 0 && si < pl.n ? pl.s[si].t0 / 1000.0 : pl.total_ms / 1000.0)); kc_array_push(marks, mo); }
    SETK(r, "duration", kc_float(pl.total_ms / 1000.0)); SETK(r, "words", words); SETK(r, "marks", marks);
    tts_plan_free(&pl); return r;
}
static KcValue f_align(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "align(text[, options]) -> {duration, words[], marks[]}   (timing for subtitles and lip-sync)");
    TtsOpts o; tts_opts_parse(&o, opts_arg(a, n, 1), pos, "align");
    TtsWords ws; memset(&ws, 0, sizeof ws); tts_tokenize(text_arg(a[0], pos, "align"), &o, &ws, pos);
    KcValue r = align_words(&ws, &o, pos); tts_words_free(&ws); return r;
}
static KcValue f_align_phonemes(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "align_phonemes(phonemes[, options]) -> {duration, words[], marks[]}");
    TtsOpts o; tts_opts_parse(&o, opts_arg(a, n, 1), pos, "align_phonemes");
    TtsWords ws; memset(&ws, 0, sizeof ws); phones_to_words(mn_str(a[0], pos, "align_phonemes", "phonemes"), &ws, pos, "align_phonemes");
    KcValue r = align_words(&ws, &o, pos); tts_words_free(&ws); return r;
}

/* ============================================================ lexicon */
static int parse_pron(KcValue v, TtsPh *out, int cap, KcPos pos, const char *who) {
    const char *s = mn_str(v, pos, who, "pronunciation"); char bad[32] = "";
    bool ascii = true; for (const char *q = s; *q; q++) if ((unsigned char)*q >= 0x80) ascii = false;
    int k = ascii ? tts_phseq_parse(s, out, cap, bad, sizeof bad) : tts_ipa_parse(s, out, cap);
    if (k < 0) kc_error_at(pos, "tts.%s: bad pronunciation '%s'%s%s", who, s, ascii ? " — unknown phoneme " : " (unsupported IPA symbol)", ascii ? bad : "");
    if (k == 0) kc_error_at(pos, "tts.%s: empty pronunciation", who);
    return k;
}
/* add_word("kingcobra", "K IH1 NG K OW1 B R AH0")  — ARPAbet or IPA */
static KcValue f_add_word(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 2, pos, "add_word(word, pronunciation)   (ARPAbet \"K IH1 NG\" or IPA \"kɪŋ\")");
    const char *w = mn_str(a[0], pos, "add_word", "word"); TtsPh ph[128]; int k = parse_pron(a[1], ph, 128, pos, "add_word");
    if (!*w || strlen(w) > 100) kc_error_at(pos, "tts.add_word: word must be 1..100 characters");
    tts_lex_add(w, ph, k); return kc_int(tts_lex_size());
}
/* add_words("word  PH PH\nword=PH PH") or add_words(["word=PH PH", ...]) -> number added */
static KcValue f_add_words(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "add_words(text | array) -> int   (lines: \"word  PH PH\" or \"word=PH PH\")");
    TBuf b; tb_init(&b);
    if (a[0].type == VAL_STRING) tb_puts(&b, a[0].as.s->data);
    else if (a[0].type == VAL_ARRAY) { for (int i = 0; i < a[0].as.arr->count; i++) { KcValue e = a[0].as.arr->items[i]; if (e.type != VAL_STRING) kc_error_at(pos, "tts.add_words: array items must be strings"); tb_puts(&b, e.as.s->data); tb_putc(&b, '\n'); } }
    else kc_error_at(pos, "tts.add_words: expected a string or an array of strings");
    int errs = 0; int k = tts_lex_load_text(b.d ? b.d : "", pos, "add_words", &errs); tb_free(&b);
    if (errs) kc_error_at(pos, "tts.add_words: %d line%s could not be parsed (added %d)", errs, errs == 1 ? "" : "s", k);
    return kc_int(k);
}
static KcValue f_remove_word(KcInterp *ip, KcValue *a, int n, KcPos pos) { mn_need(n, 1, 1, pos, "remove_word(word) -> bool"); return kc_bool(tts_lex_remove(mn_str(a[0], pos, "remove_word", "word"))); }
static KcValue f_clear_lexicon(KcInterp *ip, KcValue *a, int n, KcPos pos) { mn_need(n, 0, 0, pos, "clear_lexicon()"); tts_lex_clear(); return kc_null(); }
static KcValue f_lexicon_size(KcInterp *ip, KcValue *a, int n, KcPos pos) { mn_need(n, 0, 0, pos, "lexicon_size() -> user words"); return kc_int(tts_lex_size()); }
/* load_lexicon(path) -> words loaded (CMUdict format or word=pron lines; ';;;' comments) */
static KcValue f_load_lexicon(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "tts.load_lexicon");
    mn_need(n, 1, 1, pos, "load_lexicon(path) -> int   (CMUdict, or lines like \"word=K IH1 NG\")");
    size_t len; uint8_t *buf = mn_read_file(mn_str(a[0], pos, "load_lexicon", "path"), &len, pos, "load_lexicon");
    char *t = malloc(len + 1); memcpy(t, buf, len); t[len] = 0; free(buf);
    int errs = 0; int k = tts_lex_load_text(t, pos, "load_lexicon", &errs); free(t);
    return kc_int(k);
}
static KcValue f_save_lexicon(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "tts.save_lexicon");
    mn_need(n, 1, 1, pos, "save_lexicon(path) -> bytes   (your add_word entries, CMUdict format)");
    const char *path = mn_str(a[0], pos, "save_lexicon", "path"); char *d = tts_lex_dump();
    FILE *f = fopen(path, "wb"); if (!f) { free(d); kc_error_at(pos, "tts.save_lexicon: cannot write '%s': %s", path, strerror(errno)); }
    size_t L = strlen(d); fwrite(d, 1, L, f); fclose(f); free(d); return kc_int((int64_t)L);
}
/* lookup(word) -> "PH PH" from the lexicon/dictionary, "" if not listed (rules would be used) */
static KcValue f_lookup(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "lookup(word[, \"ipa\"]) -> string, \"\" when the word is not in the lexicon or dictionary");
    TtsPh ph[96]; int k, src;
    if (!tts_lex_lookup(mn_str(a[0], pos, "lookup", "word"), ph, &k, 96, &src)) return kc_string("");
    bool ipa = n > 1 && a[1].type == VAL_STRING && !strcasecmp(a[1].as.s->data, "ipa");
    char *s = tts_phseq_format(ph, k, ipa, true); KcValue r = kc_string(s); free(s); return r;
}
static KcValue f_has_word(KcInterp *ip, KcValue *a, int n, KcPos pos) { mn_need(n, 1, 1, pos, "has_word(word) -> bool"); TtsPh ph[96]; int k, src; return kc_bool(tts_lex_lookup(mn_str(a[0], pos, "has_word", "word"), ph, &k, 96, &src)); }
/* add_sub("tiger", "tie grr") — whole-word text substitution before pronunciation */
static KcValue f_add_sub(KcInterp *ip, KcValue *a, int n, KcPos pos) { mn_need(n, 2, 2, pos, "add_sub(word, replacement_text)"); tts_add_sub(mn_str(a[0], pos, "add_sub", "word"), mn_str(a[1], pos, "add_sub", "replacement")); return kc_null(); }
static KcValue f_remove_sub(KcInterp *ip, KcValue *a, int n, KcPos pos) { mn_need(n, 1, 1, pos, "remove_sub(word) -> bool"); return kc_bool(tts_sub_remove(mn_str(a[0], pos, "remove_sub", "word"))); }
static KcValue f_clear_subs(KcInterp *ip, KcValue *a, int n, KcPos pos) { mn_need(n, 0, 0, pos, "clear_subs()"); tts_sub_clear(); return kc_null(); }

/* ============================================================ voices + info */
static KcValue voice_obj(int i) {
    const TtsVoice *v = tts_voice_at(i); KcValue o = kc_object_new();
    SETK(o, "name", kc_string(v->name)); SETK(o, "description", kc_string(v->desc)); SETK(o, "f0", kc_float(v->f0)); SETK(o, "range", kc_float(v->range)); SETK(o, "tract", kc_float(v->tract));
    SETK(o, "oq", kc_float(v->oq)); SETK(o, "tilt", kc_float(v->tilt)); SETK(o, "breath", kc_float(v->breath)); SETK(o, "jitter", kc_float(v->jitter)); SETK(o, "shimmer", kc_float(v->shimmer));
    SETK(o, "rate", kc_float(v->rate)); SETK(o, "bw", kc_float(v->bw)); SETK(o, "whisper", kc_bool(v->whisper)); SETK(o, "robot", kc_bool(v->robot));
    return o;
}
static KcValue f_voices(KcInterp *ip, KcValue *a, int n, KcPos pos) { mn_need(n, 0, 0, pos, "voices() -> names"); KcValue arr = kc_array_new(); for (int i = 0; i < tts_voice_count(); i++) kc_array_push(arr, kc_string(tts_voice_at(i)->name)); return arr; }
static KcValue f_voice_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "voice_info(name) -> {name, description, f0, range, tract, ...}");
    const char *nm = mn_str(a[0], pos, "voice_info", "name"); int i = tts_voice_find(nm); if (i < 0) kc_error_at(pos, "tts.voice_info: unknown voice '%s'", nm);
    return voice_obj(i);
}
/* define_voice("hero", "male", "f0=130 tract=1.05 breath=0.1") */
static KcValue f_define_voice(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 3, pos, "define_voice(name, base_voice[, \"f0=.. range=.. tract=.. oq=.. tilt=.. breath=.. jitter=.. shimmer=.. rate=.. bw=.. whisper robot desc=..\"])");
    const char *nm = mn_str(a[0], pos, "define_voice", "name");
    if (!*nm || strlen(nm) > 22) kc_error_at(pos, "tts.define_voice: name must be 1..22 characters");
    int bi = tts_voice_find("male"); (void)bi;
    int i = tts_voice_define(nm, mn_str(a[1], pos, "define_voice", "base"), n > 2 ? a[2] : kc_null(), pos);
    return voice_obj(i);
}
static KcValue f_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 0, 0, pos, "info() -> {voices, dictionary_words, user_words, phonemes}");
    tts_g2p_init(); KcValue o = kc_object_new();
    SETK(o, "voices", kc_int(tts_voice_count())); SETK(o, "dictionary_words", kc_int(tts_dict_size())); SETK(o, "user_words", kc_int(tts_lex_size())); SETK(o, "phonemes", kc_int(P_PAUSE - 1)); SETK(o, "version", kc_string("1.0"));
    return o;
}
static KcValue f_phoneme_table(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 0, 0, pos, "phoneme_table() -> [{arpabet, ipa, type, voiced}]");
    static const char *CN[] = { "silence", "vowel", "diphthong", "stop", "affricate", "fricative", "nasal", "liquid", "glide", "aspirate", "flap" };
    KcValue arr = kc_array_new();
    for (int i = 1; i < P_PAUSE; i++) { KcValue o = kc_object_new(); SETK(o, "arpabet", kc_string(TTS_PH[i].arpa)); SETK(o, "ipa", kc_string(TTS_PH[i].ipa)); SETK(o, "type", kc_string(CN[TTS_PH[i].cls])); SETK(o, "voiced", kc_bool(TTS_PH[i].voiced)); kc_array_push(arr, o); }
    return arr;
}


/* ============================================================ vocal styles */
static KcValue style_info_obj(int i) {
    KcValue o = kc_object_new();
    SETK(o, "name", kc_string(tts_style_name(i))); SETK(o, "category", kc_string(tts_style_cat(i))); SETK(o, "description", kc_string(tts_style_desc(i)));
    SETK(o, "params", style_obj(tts_style_at(i)));
    return o;
}
static KcValue f_styles(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 0, 1, pos, "styles([category]) -> names   (category: \"mood\", \"manner\", \"custom\")");
    const char *cat = n > 0 ? mn_str(a[0], pos, "styles", "category") : NULL; KcValue arr = kc_array_new();
    for (int i = 1; i < tts_style_count(); i++) if (!cat || !strcmp(cat, tts_style_cat(i))) kc_array_push(arr, kc_string(tts_style_name(i)));
    return arr;
}
static KcValue f_style_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "style_info(name) -> {name, category, description, params}");
    const char *nm = mn_str(a[0], pos, "style_info", "name"); int i = tts_style_find(nm);
    if (i < 0) { const char *c = tts_style_canon(nm); kc_error_at(pos, "tts.style_info: unknown style '%s'%s%s", nm, c ? " — did you mean " : "", c ? c : ""); }
    return style_info_obj(i);
}
static KcValue f_define_style(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 2, 3, pos, "define_style(name, base_style[, options]) -> style_info   (options: pitch range rate volume breath whisper tilt oq jitter shimmer tremor tremor_hz creak formant pauses accent decl artic fall lilt amod desc aliases)");
    const char *nm = mn_str(a[0], pos, "define_style", "name"); const char *base = a[1].type == VAL_NULL ? "" : mn_str(a[1], pos, "define_style", "base");
    int i = tts_style_define(nm, base, n > 2 ? a[2] : kc_null(), pos);
    return style_info_obj(i);
}
/* resolve_tag("dramitaclly") -> "dramatic" ; "" when the text is not a tag */
static KcValue f_resolve_tag(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "resolve_tag(text) -> canonical tag name (typos are corrected), \"\" if it is not a tag");
    const char *c = tts_style_canon(mn_str(a[0], pos, "resolve_tag", "text")); return kc_string(c ? c : "");
}
static KcValue f_events(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 0, 0, pos, "events() -> names of the sound events ([laugh] [sigh] ...)");
    KcValue arr = kc_array_new(); for (int i = 0; i < tts_event_count(); i++) kc_array_push(arr, kc_string(tts_event_name(i))); return arr;
}
static KcValue f_interjections(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 0, 0, pos, "interjections() -> names ([ugh] [aww] [wow] ...)");
    KcValue arr = kc_array_new(); for (int i = 0; i < tts_interj_count(); i++) kc_array_push(arr, kc_string(tts_interj_name(i))); return arr;
}

KcModuleEntry tts_entries[] = {
    { "styles", f_styles }, { "style_info", f_style_info }, { "define_style", f_define_style }, { "resolve_tag", f_resolve_tag }, { "events", f_events }, { "interjections", f_interjections },
    { "speak", f_speak }, { "align_phonemes", f_align_phonemes }, { "to_wav", f_to_wav }, { "say", f_say }, { "save_wav", f_save_wav }, { "free", f_free }, { "duration", f_duration }, { "speak_phonemes", f_speak_phonemes }, { "align", f_align },
    { "phonemes", f_phonemes }, { "ipa", f_ipa }, { "from_ipa", f_from_ipa }, { "to_ipa", f_to_ipa }, { "words", f_words }, { "pronounce", f_pronounce }, { "explain", f_explain },
    { "normalize", f_normalize }, { "number_to_words", f_number_to_words },
    { "add_word", f_add_word }, { "add_words", f_add_words }, { "remove_word", f_remove_word }, { "clear_lexicon", f_clear_lexicon }, { "lexicon_size", f_lexicon_size },
    { "load_lexicon", f_load_lexicon }, { "save_lexicon", f_save_lexicon }, { "lookup", f_lookup }, { "has_word", f_has_word },
    { "add_sub", f_add_sub }, { "remove_sub", f_remove_sub }, { "clear_subs", f_clear_subs },
    { "voices", f_voices }, { "voice_info", f_voice_info }, { "define_voice", f_define_voice }, { "info", f_info }, { "phoneme_table", f_phoneme_table },
};

KcModule *kc_mod_tts(void) {
    static KcModule m; static bool init = false;
    if (!init) { m.name = "tts"; m.entries = tts_entries; m.count = (int)(sizeof tts_entries / sizeof tts_entries[0]); init = true; }
    return &m;
}
