/* KingCobra std.music_note — file I/O: WAV read/write (8/16/24/32-bit PCM, 32-bit float), to_wav, play.
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mn_internal.h"
#include "kc_platform.h"
#if !defined(_WIN32)
#include <unistd.h>
#endif

#define SETK(o, k, v) kc_object_set((o), (k), strlen(k), (v))

static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/* bits: 8, 16, 24, 32 (integer PCM) or -32 (IEEE float). Returns a malloc'd complete WAV file image. */
uint8_t *mn_wav_encode(const MnAudio *a, int bits, bool dither, size_t *len, KcPos pos, const char *who) {
    int bps = (bits < 0 ? -bits : bits) / 8;
    uint64_t data = (uint64_t)a->frames * (uint64_t)a->ch * (uint64_t)bps;
    if (data > 0xFFFFFFFFull - 64) kc_error_at(pos, "music_note.%s: audio is too long for a WAV file (4 GiB limit)", who);
    uint8_t *out = kc_malloc((size_t)data + 44 + 8);
    uint8_t *h = out;
    memcpy(h, "RIFF", 4); w32(h + 4, (uint32_t)(36 + data)); memcpy(h + 8, "WAVEfmt ", 8); w32(h + 16, 16);
    w16(h + 20, bits < 0 ? 3 : 1); w16(h + 22, (uint32_t)a->ch); w32(h + 24, (uint32_t)a->rate);
    w32(h + 28, (uint32_t)((uint64_t)a->rate * a->ch * bps)); w16(h + 32, (uint32_t)(a->ch * bps)); w16(h + 34, (uint32_t)(bps * 8));
    memcpy(h + 36, "data", 4); w32(h + 40, (uint32_t)data);
    size_t total = a->frames * (size_t)a->ch;
    MnRng rng = { 0x1234567ull };
    for (size_t i = 0; i < total; i++) {
        double v = a->d[i]; if (v > 1.0) v = 1.0; if (v < -1.0) v = -1.0;
        uint8_t *p = out + 44 + i * (size_t)bps;
        if (bits < 0) { float fv = (float)v; uint32_t u; memcpy(&u, &fv, 4); w32(p, u); continue; }
        double scale = bits == 8 ? 127.0 : bits == 16 ? 32767.0 : bits == 24 ? 8388607.0 : 2147483647.0;
        double sv = v * scale;
        if (dither && bits <= 24) sv += (mn_rng_f(&rng) + mn_rng_f(&rng)) * 0.5;
        long long q = llround(sv); long long lim = (long long)scale;
        if (q > lim) q = lim;
        if (q < -lim - 1) q = -lim - 1;
        if (bits == 8) p[0] = (uint8_t)(q + 128);
        else if (bits == 16) w16(p, (uint32_t)(int16_t)q);
        else if (bits == 24) { uint32_t u = (uint32_t)(int32_t)q; p[0] = (uint8_t)u; p[1] = (uint8_t)(u >> 8); p[2] = (uint8_t)(u >> 16); }
        else w32(p, (uint32_t)(int32_t)q);
    }
    *len = 44 + (size_t)data;
    return out;
}
int mn_wav_write(const MnAudio *a, const char *path, int bits, bool dither, KcPos pos, const char *who) {
    size_t n; uint8_t *img = mn_wav_encode(a, bits, dither, &n, pos, who);
    FILE *f = fopen(path, "wb");
    if (!f) { free(img); kc_error_at(pos, "music_note.%s: cannot open '%s' for writing: %s", who, path, strerror(errno)); }
    size_t w = fwrite(img, 1, n, f); int bad = ferror(f); fclose(f); free(img);
    if (bad || w != n) kc_error_at(pos, "music_note.%s: write to '%s' failed", who, path);
    return (int)n;
}

/* ---- base64 ---- */
char *mn_b64_encode(const uint8_t *d, size_t n, int wrap) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t cap = (n + 2) / 3 * 4 + (wrap > 0 ? ((n + 2) / 3 * 4) / (size_t)wrap + 2 : 0) + 4;
    char *o = kc_malloc(cap); size_t k = 0; int col = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o[k++] = T[v >> 18 & 63]; o[k++] = T[v >> 12 & 63]; o[k++] = i + 1 < n ? T[v >> 6 & 63] : '='; o[k++] = i + 2 < n ? T[v & 63] : '=';
        col += 4; if (wrap > 0 && col >= wrap && i + 3 < n) { o[k++] = '\n'; col = 0; }
    }
    o[k] = 0; return o;
}
uint8_t *mn_b64_decode(const char *s, size_t n, size_t *out) {
    static signed char T[256]; static bool init = false;
    if (!init) { memset(T, -1, sizeof T); const char *A = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for (int i = 0; i < 64; i++) T[(unsigned char)A[i]] = (signed char)i; T[(unsigned char)'-'] = 62; T[(unsigned char)'_'] = 63; init = true; }
    uint8_t *o = kc_malloc(n / 4 * 3 + 6); size_t k = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '=' ) break;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        if (T[c] < 0) { free(o); return NULL; }
        acc = acc << 6 | (uint32_t)T[c]; bits += 6;
        if (bits >= 8) { bits -= 8; o[k++] = (uint8_t)(acc >> bits); acc &= (1u << bits) - 1; }
    }
    *out = k; return o;
}

static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t r16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

typedef struct { int fmt, ch, rate, bits; size_t frames; const uint8_t *data; size_t dlen; } WavInfo;

uint8_t *mn_read_file(const char *path, size_t *len, KcPos pos, const char *who) {
    FILE *f = fopen(path, "rb");
    if (!f) kc_error_at(pos, "music_note.%s: cannot open '%s': %s", who, path, strerror(errno));
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || (uint64_t)sz > 0x7fffffffull) { fclose(f); kc_error_at(pos, "music_note.%s: '%s' is empty or too large to read (size %ld)", who, path, sz); }
    uint8_t *b = kc_malloc((size_t)sz + 1); size_t got = fread(b, 1, (size_t)sz, f); fclose(f);
    b[got] = 0; *len = got; return b;
}
static void wav_parse(const uint8_t *b, size_t len, WavInfo *w, const char *path, KcPos pos, const char *who) {
    if (len < 12 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) kc_error_at(pos, "music_note.%s: '%s' is not a RIFF/WAVE file", who, path);
    size_t p = 12; bool have_fmt = false, have_data = false; memset(w, 0, sizeof *w);
    while (p + 8 <= len) {
        uint32_t cl = r32(b + p + 4); const uint8_t *c = b + p + 8; size_t avail = len - (p + 8);
        if (!memcmp(b + p, "fmt ", 4) && cl >= 16 && avail >= 16) {
            w->fmt = (int)r16(c); w->ch = (int)r16(c + 2); w->rate = (int)r32(c + 4); w->bits = (int)r16(c + 14);
            if (w->fmt == 0xFFFE && cl >= 26 && avail >= 26) w->fmt = (int)r16(c + 24);
            have_fmt = true;
        } else if (!memcmp(b + p, "data", 4)) {
            w->data = c; w->dlen = cl > avail ? avail : cl; have_data = true; break;
        }
        p += 8 + (size_t)cl + (cl & 1);
    }
    if (!have_fmt || !have_data) kc_error_at(pos, "music_note.%s: '%s' has no fmt/data chunk", who, path);
    bool ok = (w->fmt == 1 && (w->bits == 8 || w->bits == 16 || w->bits == 24 || w->bits == 32)) || (w->fmt == 3 && (w->bits == 32 || w->bits == 64));
    if (!ok) kc_error_at(pos, "music_note.%s: unsupported WAV encoding (format %d, %d-bit); supported: 8/16/24/32-bit PCM and 32/64-bit float", who, w->fmt, w->bits);
    if (w->ch < 1 || w->ch > 8 || w->rate < 1000 || w->rate > 384000) kc_error_at(pos, "music_note.%s: '%s' has unsupported channels/sample rate (%d ch, %d Hz)", who, path, w->ch, w->rate);
    w->frames = w->dlen / ((size_t)(w->bits / 8) * (size_t)w->ch);
}
MnAudio *mn_wav_decode(const uint8_t *b, size_t len, const char *path, KcPos pos, const char *who) {
    WavInfo w;
    wav_parse(b, len, &w, path, pos, who);
    MnAudio *a = mn_audio_new(w.frames, w.ch, w.rate, pos, who);
    size_t total = w.frames * (size_t)w.ch; int bps = w.bits / 8;
    for (size_t i = 0; i < total; i++) {
        const uint8_t *p = w.data + i * (size_t)bps; float v;
        if (w.fmt == 3) { if (bps == 4) { uint32_t u = r32(p); memcpy(&v, &u, 4); } else { uint64_t u = (uint64_t)r32(p) | ((uint64_t)r32(p + 4) << 32); double dv; memcpy(&dv, &u, 8); v = (float)dv; } }
        else if (bps == 1) v = ((int)p[0] - 128) / 128.0f;
        else if (bps == 2) v = (int16_t)r16(p) / 32768.0f;
        else if (bps == 3) { int32_t s = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)); v = (float)(s / 2147483648.0); }
        else v = (float)((int32_t)r32(p) / 2147483648.0);
        a->d[i] = v;
    }
    return a;
}
MnAudio *mn_wav_read(const char *path, KcPos pos, const char *who) {
    size_t len; uint8_t *b = mn_read_file(path, &len, pos, who);
    MnAudio *a = mn_wav_decode(b, len, path, pos, who);
    free(b);
    return a;
}

static int bits_opt(KcValue o, KcPos pos, const char *who) {
    KcValue *v = mn_opt_get(o, "bits");
    if (!v || v->type == VAL_NULL) return 16;
    if (v->type == VAL_STRING) { if (!strcasecmp(v->as.s->data, "float") || !strcasecmp(v->as.s->data, "f32")) return -32; kc_error_at(pos, "music_note.%s: bits must be 8, 16, 24, 32 or \"float\"", who); }
    int64_t b = mn_int(*v, pos, who, "bits");
    if (b != 8 && b != 16 && b != 24 && b != 32) kc_error_at(pos, "music_note.%s: bits must be 8, 16, 24, 32 or \"float\"", who);
    return (int)b;
}

/* load_wav(path) -> audio */
static KcValue io_load_wav(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "music_note.load_wav");
    mn_need(n, 1, 1, pos, "load_wav(path) -> audio");
    return mn_audio_wrap(mn_wav_read(mn_str(a[0], pos, "load_wav", "path"), pos, "load_wav"));
}
/* wav_info(path) -> {rate, channels, bits, float, frames, seconds} — header only */
static KcValue io_wav_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "music_note.wav_info");
    mn_need(n, 1, 1, pos, "wav_info(path) -> {rate, channels, bits, float, frames, seconds}");
    const char *path = mn_str(a[0], pos, "wav_info", "path"); size_t len; uint8_t *b = mn_read_file(path, &len, pos, "wav_info"); WavInfo w;
    wav_parse(b, len, &w, path, pos, "wav_info");
    KcValue o = kc_object_new();
    SETK(o, "rate", kc_int(w.rate)); SETK(o, "channels", kc_int(w.ch)); SETK(o, "bits", kc_int(w.bits)); SETK(o, "float", kc_bool(w.fmt == 3));
    SETK(o, "frames", kc_int((int64_t)w.frames)); SETK(o, "seconds", kc_float((double)w.frames / w.rate));
    free(b); return o;
}
/* save_wav(audio, path[, {bits = 16, dither = true}]) -> bytes written */
static KcValue io_save_wav(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "music_note.save_wav");
    mn_need(n, 2, 3, pos, "save_wav(audio, path[, {bits = 16 | 8 | 24 | 32 | \"float\", dither = true}]) -> int");
    MnAudio *x = mn_audio_arg(a[0], pos, "save_wav"); KcValue o = n > 2 ? a[2] : kc_null();
    return kc_int(mn_wav_write(x, mn_str(a[1], pos, "save_wav", "path"), bits_opt(o, pos, "save_wav"), mn_opt_bool(o, "dither", true, pos, "save_wav"), pos, "save_wav"));
}

/* ---- legacy melody form: to_wav(notes[], path, seconds_per_note) -> samples; mono 16-bit sine ---- */
static int legacy_to_wav(KcValue notes, const char *path, double dur, KcPos pos) {
    if (dur <= 0) kc_error_at(pos, "music_note.to_wav: seconds_per_note must be > 0");
    const int SR = 44100; KcArray *arr = notes.as.arr;
    long per = (long)(SR * dur), total = per * arr->count;
    MnAudio *au = mn_audio_new((size_t)total, 1, SR, pos, "to_wav");
    for (int ni = 0; ni < arr->count; ni++) {
        if (arr->items[ni].type != VAL_STRING) { mn_audio_free(au); kc_error_at(pos, "music_note.to_wav: note %d is not a string", ni); }
        const char *name = arr->items[ni].as.s->data; double freq = 0;
        if (strcmp(name, "-") && strcmp(name, "rest")) {
            MnNote nn; const char *e = mn_note_scan(name, &nn);
            if (!e || *e) { mn_audio_free(au); kc_error_at(pos, "music_note.to_wav: '%s' is not a note name", name); }
            freq = mn_midi_freq(mn_note_midi(&nn, 4), 440.0);
        }
        for (long s = 0; s < per && freq > 0; s++) {
            double env = 1.0, att = 0.01 * SR, rel = 0.03 * SR;
            if (s < att) env = s / att; else if (s > per - rel) env = (double)(per - s) / rel;
            au->d[(size_t)ni * (size_t)per + (size_t)s] = (float)(sin(MN_TAU * freq * ((double)s / SR)) * env * 0.6 * (32000.0 / 32768.0));
        }
    }
    mn_wav_write(au, path, 16, false, pos, "to_wav");
    mn_audio_free(au);
    return (int)total;
}

/* Build notation from a plain array of note names: each note is a quarter note at 60/seconds BPM. */
static MnScore *score_from_array(KcValue arr, double sec_per_note, KcValue o, KcPos pos, const char *who) {
    size_t cap = 256; char *t = kc_malloc(cap); size_t n = 0;
    const char *inst = mn_opt_str(o, "instrument", "piano", pos, who);
    n += (size_t)snprintf(t, cap, "tempo %.6f\ntrack main %s\n", 60.0 / sec_per_note, inst);
    for (int i = 0; i < arr.as.arr->count; i++) {
        if (arr.as.arr->items[i].type != VAL_STRING) { free(t); kc_error_at(pos, "music_note.%s: note %d is not a string", who, i); }
        const char *s = arr.as.arr->items[i].as.s->data;
        if (n + strlen(s) + 2 >= cap) { cap = cap * 2 + strlen(s); t = kc_realloc(t, cap); }
        n += (size_t)sprintf(t + n, "%s ", s);
    }
    MnScore *S = mn_score_parse(t, n, false, pos, who); free(t);
    return S;
}

/* Turn any music source (notation string, score, note array, audio) into audio. *owned tells the caller to free it. */
static MnAudio *to_audio(KcValue src, KcValue opts, bool *owned, KcPos pos, const char *who) {
    if (mn_is_audio(src)) { *owned = false; return mn_audio_arg(src, pos, who); }
    *owned = true;
    if (src.type == VAL_STRING) { MnScore *s = mn_score_parse_opts(src, opts, pos, who); MnAudio *r = mn_score_render(s, opts, pos, who); mn_score_free(s); return r; }
    if (src.type == VAL_ARRAY) { MnScore *s = score_from_array(src, mn_opt_num(opts, "seconds", 0.4, pos, who), opts, pos, who); MnAudio *r = mn_score_render(s, opts, pos, who); mn_score_free(s); return r; }
    if (mn_is_score(src)) return mn_score_render(mn_score_arg(src, pos, who), opts, pos, who);
    kc_error_at(pos, "music_note.%s: expected notation text, a score, a note array or an audio buffer, got %s", who, kc_type_name(src));
    return NULL;
}

/* to_wav(source, path[, options | seconds_per_note]) -> frames written
 *   source: notes array (legacy sine form with a number/none), notation string, score, or audio. */
static KcValue io_to_wav(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "music_note.to_wav");
    mn_set_ip(ip);
    mn_need(n, 2, 3, pos, "to_wav(notes[] | notation | score | audio, path[, seconds_per_note | {options}]) -> int   (options: instrument, bits, dither, rate, tail, master, speed, seconds ...)");
    const char *path = mn_str(a[1], pos, "to_wav", "path");
    KcValue o = n > 2 && mn_is_opts(a[2]) ? a[2] : kc_null();
    if (a[0].type == VAL_ARRAY && !mn_is_opts(o)) {
        double dur = n > 2 ? mn_num(a[2], pos, "to_wav", "seconds_per_note") : 0.4;
        return kc_int(legacy_to_wav(a[0], path, dur, pos));
    }
    if (n > 2 && !mn_is_opts(a[2]) && a[0].type != VAL_ARRAY) kc_error_at(pos, "music_note.to_wav: the third argument must be an options object here");
    bool owned; MnAudio *au = to_audio(a[0], o, &owned, pos, "to_wav");
    mn_wav_write(au, path, bits_opt(o, pos, "to_wav"), mn_opt_bool(o, "dither", true, pos, "to_wav"), pos, "to_wav");
    int64_t frames = (int64_t)au->frames;
    if (owned) mn_audio_free(au);
    return kc_int(frames);
}

/* play(source[, options]) -> bool — blocks until playback ends */
static KcValue io_play(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    kc_forbid_in_comptime(ip, pos, "music_note.play");
    mn_set_ip(ip);
    mn_need(n, 1, 2, pos, "play(notes[] | notation | score | audio[, options]) -> bool   (plays through the system audio player)");
    KcValue o = n > 1 ? a[1] : kc_null();
    bool owned; MnAudio *au = to_audio(a[0], o, &owned, pos, "play");
    char path[512];
#if defined(_WIN32)
    const char *tmp = getenv("TEMP"); if (!tmp) tmp = ".";
    snprintf(path, sizeof path, "%s\\kc_music_%lu.wav", tmp, (unsigned long)GetCurrentProcessId());
    mn_wav_write(au, path, 16, true, pos, "play");
    kc_dl_handle h = kc_dlopen("winmm.dll"); bool ok = false;
    if (h) {
        typedef BOOL (WINAPI *PlayFn)(LPCSTR, HMODULE, DWORD);
        PlayFn pf = (PlayFn)kc_dlsym(h, "PlaySoundA");
        if (pf) ok = pf(path, NULL, 0x20000 /* SND_FILENAME */ | 0x0 /* SND_SYNC */) != 0;
    }
    remove(path);
    if (owned) mn_audio_free(au);
    if (!ok) kc_error_at(pos, "music_note.play: Windows audio playback failed; use save_wav and open the file with a player");
    return kc_bool(true);
#else
    const char *tmp = getenv("TMPDIR"); if (!tmp || !*tmp) tmp = "/tmp";
    snprintf(path, sizeof path, "%s/kc_music_XXXXXX.wav", tmp);
    int fd = mkstemps(path, 4);
    if (fd < 0) { if (owned) mn_audio_free(au); kc_error_at(pos, "music_note.play: cannot create a temporary file: %s", strerror(errno)); }
    close(fd);
    mn_wav_write(au, path, 16, true, pos, "play");
    if (owned) mn_audio_free(au);
    static const char *PLAYERS[] = { "aplay -q", "paplay", "pw-play", "afplay", "ffplay -nodisp -autoexit -loglevel quiet", "play -q" };
    bool ok = false;
    for (size_t i = 0; i < sizeof PLAYERS / sizeof PLAYERS[0] && !ok; i++) {
        char cmd[700]; snprintf(cmd, sizeof cmd, "%s '%s' >/dev/null 2>&1", PLAYERS[i], path);
        int rc = system(cmd);
        if (rc == 0) ok = true;
    }
    remove(path);
    if (!ok) kc_error_at(pos, "music_note.play: no working audio player found (tried aplay, paplay, pw-play, afplay, ffplay, play); use music_note.save_wav and open the file yourself");
    return kc_bool(true);
#endif
}

/* exported for std.tts: play an audio handle through the system player */
KcValue mn_play_audio(KcInterp *ip, KcValue audio, KcPos pos) { return io_play(ip, &audio, 1, pos); }

KcModuleEntry mn_io_entries[] = {
    { "load_wav", io_load_wav }, { "save_wav", io_save_wav }, { "wav_info", io_wav_info }, { "to_wav", io_to_wav }, { "play", io_play },
};
const int mn_io_count = (int)(sizeof mn_io_entries / sizeof mn_io_entries[0]);
