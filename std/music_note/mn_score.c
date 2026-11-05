/* KingCobra std.music_note — multiline multi-track notation parser, score API, MIDI export.
 *
 * Notation overview (full reference: docs/STD_MUSIC_NOTE.md):
 *
 *   title "Ode to Joy"          # header directives; '#' or '//' starts a comment
 *   tempo 120                   # quarter-note BPM (tempo=90 inline changes it mid-piece)
 *   time 4/4
 *   key G                       # key signature: unmarked F plays as F#
 *
 *   track melody piano vol=0.9 pan=-0.2 reverb=0.25
 *   E5:q E5 F5 G5 | G5 F5 E5 D5 | C5 C5 D5 E5 | E5:q. D5:e D5:h
 *
 *   track bass bass
 *   |: {C}:w {G}:w :|x2         # chord symbols in braces, repeats with |: :|xN
 *
 *   track drums drums
 *   |: kick:q hat:e hat:e snare:q hat:e hat:e :|x4
 *
 * Author: Divyanshu Sinha <divyanshu.sinha631@gmail.com>
 */
#include "mn_internal.h"
#include <stdarg.h>

#define SETK(o, k, v) kc_object_set((o), (k), strlen(k), (v))
#define MAX_TOKENS 4000000
#define MAX_EVENTS 2000000

/* ============================================================ error context */
static KcPos g_pos;
static const char *g_who = "parse";

typedef struct { char *s; int line, col; } Tok;
typedef struct { Tok *t; int n, cap; } TL;

typedef struct { char name[200]; char dir[200]; } FInfo;     /* a source file taking part in a parse (main text, includes) */
static FInfo *g_fi; static int g_nfi, g_capfi;
static int fi_add(const char *name, const char *dir) {
    if (g_nfi == g_capfi) { g_capfi = g_capfi ? g_capfi * 2 : 8; g_fi = kc_realloc(g_fi, (size_t)g_capfi * sizeof *g_fi); }
    snprintf(g_fi[g_nfi].name, sizeof g_fi[0].name, "%s", name ? name : "");
    snprintf(g_fi[g_nfi].dir, sizeof g_fi[0].dir, "%s", dir ? dir : "");
    return g_nfi++;
}
static void perr_at(int lineenc, int col, const char *fmt, ...) {
    char msg[300];
    int file = lineenc >> 20, line = lineenc & 0xFFFFF;
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    if (file < g_nfi && g_fi[file].name[0])
        kc_error_at(g_pos, "music_note.%s: %s line %d, col %d: %s", g_who, g_fi[file].name, line, col, msg);
    kc_error_at(g_pos, "music_note.%s: line %d, col %d: %s", g_who, line, col, msg);
}
#define perr(tk, ...) perr_at((tk)->line, (tk)->col, __VA_ARGS__)

static void tl_push(TL *l, const char *s, size_t len, int line, int col) {
    if (l->n == MAX_TOKENS) perr_at(line, col, "score is too large (more than %d tokens)", MAX_TOKENS);
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->t = kc_realloc(l->t, (size_t)l->cap * sizeof(Tok)); }
    char *d = kc_malloc(len + 1); memcpy(d, s, len); d[len] = 0;
    l->t[l->n].s = d; l->t[l->n].line = line; l->t[l->n].col = col; l->n++;
}
static void tl_free(TL *l) { for (int i = 0; i < l->n; i++) free(l->t[i].s); free(l->t); l->t = NULL; l->n = l->cap = 0; }

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Split one line into tokens. Bar lines, repeat marks and bracketed groups are tokens of their own. */
static void lex_line(TL *out, const char *ln, int lineno) {
    const char *p = ln;
    while (*p) {
        while (is_ws(*p)) p++;
        if (!*p) break;
        int col = (int)(p - ln) + 1;
        if (*p == '#' || *p == ';' || (p[0] == '/' && p[1] == '/')) break;
        if (*p == '|') {
            if (p[1] == ':') { tl_push(out, "|:", 2, lineno, col); p += 2; }
            else { tl_push(out, "|", 1, lineno, col); p++; }
            continue;
        }
        if (p[0] == ':' && p[1] == '|') {
            const char *q = p + 2;
            if ((*q == 'x' || *q == 'X') && isdigit((unsigned char)q[1])) { q++; while (isdigit((unsigned char)*q)) q++; }
            tl_push(out, p, (size_t)(q - p), lineno, col); p = q;
            continue;
        }
        const char *q = p;
        while (*q && !is_ws(*q)) {
            if (*q == '|' || (*q == ':' && q[1] == '|')) break;
            if (*q == '[' || *q == '{') {
                char close = *q == '[' ? ']' : '}';
                const char *c = strchr(q, close);
                if (!c) perr_at(lineno, (int)(q - ln) + 1, "unmatched '%c'", *q);
                q = c + 1; continue;
            }
            q++;
        }
        tl_push(out, p, (size_t)(q - p), lineno, col);
        p = q;
    }
}

/* ============================================================ score container */

static MnPatch default_patch(void) {
    MnPatch p; memset(&p, 0, sizeof p); p.inst = mn_inst_find("piano"); p.atk = p.dec = p.sus = p.rel = -1; p.vib_rate = p.vib_depth = -1; p.detune = 0;
    return p;
}
static int patch_idx(MnScore *S, const MnPatch *p) {
    for (int i = S->np - 1; i >= 0; i--) {
        const MnPatch *q = &S->patch[i];
        if (q->inst == p->inst && q->atk == p->atk && q->dec == p->dec && q->sus == p->sus && q->rel == p->rel &&
            q->vib_rate == p->vib_rate && q->vib_depth == p->vib_depth && q->detune == p->detune && q->sample == p->sample) return i;
    }
    if (S->np == S->capp) { S->capp = S->capp ? S->capp * 2 : 8; S->patch = kc_realloc(S->patch, (size_t)S->capp * sizeof(MnPatch)); }
    S->patch[S->np] = *p;
    return S->np++;
}
static void tempo_push(MnScore *S, double beat, double bpm) {
    if (S->nt == S->capt) { S->capt = S->capt ? S->capt * 2 : 8; S->tempo = kc_realloc(S->tempo, (size_t)S->capt * sizeof(MnTempo)); }
    S->tempo[S->nt].beat = beat; S->tempo[S->nt].bpm = bpm; S->nt++;
}
static int tempo_cmp(const void *a, const void *b) {
    const MnTempo *x = a, *y = b;
    return x->beat < y->beat ? -1 : x->beat > y->beat ? 1 : 0;
}
void mn_score_free(MnScore *s) {
    if (!s) return;
    for (int i = 0; i < s->ntr; i++) free(s->tr[i].ev);
    for (int i = 0; i < s->nsmp; i++) mn_audio_free(s->smp[i].a);
    free(s->smp); free(s->mk);
    free(s->tr); free(s->patch); free(s->tempo); free(s);
}
double mn_beat_to_sec(const MnScore *s, double beat) {
    double sec = 0;
    for (int i = 0; i < s->nt; i++) {
        double b0 = s->tempo[i].beat, b1 = (i + 1 < s->nt) ? s->tempo[i + 1].beat : 1e300;
        if (beat <= b0) break;
        double hi = beat < b1 ? beat : b1;
        sec += (hi - b0) * 60.0 / s->tempo[i].bpm;
        if (beat <= b1) break;
    }
    return sec;
}
/* ---- tempo map helpers (seconds <-> beats) ---- */
static MnTempo *tmap_build(const MnScore *S, int *n) {      /* sorted, deduplicated, with a point at beat 0; malloc'd */
    int cnt = S->nt;
    MnTempo *a = kc_malloc(((size_t)cnt + 2) * sizeof *a);
    for (int i = 0; i < cnt; i++) a[i] = S->tempo[i];
    for (int x = 1; x < cnt; x++) { MnTempo t = a[x]; int y = x - 1; while (y >= 0 && a[y].beat > t.beat) { a[y + 1] = a[y]; y--; } a[y + 1] = t; }
    int m = 0;
    for (int x = 0; x < cnt; x++) { if (m > 0 && fabs(a[m - 1].beat - a[x].beat) < 1e-9) a[m - 1] = a[x]; else a[m++] = a[x]; }
    if (m == 0) { a[0].beat = 0; a[0].bpm = 120.0; m = 1; }
    else if (a[0].beat > 1e-9) { memmove(a + 1, a, (size_t)m * sizeof *a); a[0].beat = 0; a[0].bpm = a[1].bpm; m++; }
    *n = m; return a;
}
static double tm_beat_to_sec(const MnTempo *t, int n, double beat) {
    double sec = 0;
    for (int i = 0; i < n; i++) {
        double b0 = t[i].beat, b1 = (i + 1 < n) ? t[i + 1].beat : 1e300;
        if (beat <= b0) break;
        double hi = beat < b1 ? beat : b1;
        sec += (hi - b0) * 60.0 / t[i].bpm;
        if (beat <= b1) break;
    }
    return sec;
}
static double tm_sec_to_beat(const MnTempo *t, int n, double sec) {
    double acc = 0;
    for (int i = 0; i < n; i++) {
        double spb = 60.0 / t[i].bpm, b0 = t[i].beat;
        if (i + 1 >= n) return b0 + (sec - acc) / spb;
        double seg = (t[i + 1].beat - b0) * spb;
        if (sec <= acc + seg) return b0 + (sec - acc) / spb;
        acc += seg;
    }
    return 0;
}
double mn_sec_to_beat(const MnScore *s, double sec) { int n; MnTempo *t = tmap_build(s, &n); double b = tm_sec_to_beat(t, n, sec); free(t); return b; }
double mn_bpm_at(const MnScore *s, double beat) {
    double bpm = s->nt ? s->tempo[0].bpm : 120.0;
    for (int i = 0; i < s->nt; i++) if (s->tempo[i].beat <= beat + 1e-9) bpm = s->tempo[i].bpm;
    return bpm;
}

/* ---- timestamps: 12.5  12.5s  500ms  1:02.5  0:01:02.500  1m30s  2h ---- */
bool mn_parse_time(const char *s, double *out) {
    while (*s == ' ') s++;
    if (!*s) return false;
    double total = 0;
    if (strchr(s, ':')) {
        char b[64]; size_t L = strlen(s); while (L && s[L - 1] == ' ') L--;
        if (L >= sizeof b) return false;
        memcpy(b, s, L); b[L] = 0;
        double f[3]; int k = 0; char *sv = NULL;
        for (char *t = mn_tok(b, ":", &sv); t; t = mn_tok(NULL, ":", &sv)) {
            char *e; if (k >= 3) return false;
            f[k] = strtod(t, &e); if (e == t || *e || f[k] < 0) return false;
            k++;
        }
        if (k < 2) return false;
        total = k == 2 ? f[0] * 60.0 + f[1] : f[0] * 3600.0 + f[1] * 60.0 + f[2];
    } else {
        const char *p = s; bool any = false;
        while (*p && *p != ' ') {
            char *e; double v = strtod(p, &e); if (e == p || v < 0) return false;
            p = e;
            if (!strncmp(p, "ms", 2)) { total += v / 1000.0; p += 2; }
            else if (*p == 's') { total += v; p++; }
            else if (*p == 'm') { total += v * 60.0; p++; }
            else if (*p == 'h') { total += v * 3600.0; p++; }
            else if (*p == 0 || *p == ' ') total += v;
            else return false;
            any = true;
        }
        if (!any) return false;
    }
    if (!isfinite(total) || total < 0 || total > 1e7) return false;
    *out = total; return true;
}
char *mn_timecode(double sec, int fps, char *buf) {
    if (sec < 0) sec = 0;
    if (fps > 0) {
        long long tf = llround(sec * fps), ff = tf % fps, ts = tf / fps;
        sprintf(buf, "%02lld:%02lld:%02lld:%02lld", ts / 3600, ts / 60 % 60, ts % 60, ff);
    } else {
        long long ms = llround(sec * 1000.0), ts = ms / 1000;
        sprintf(buf, "%02lld:%02lld:%02lld.%03lld", ts / 3600, ts / 60 % 60, ts % 60, ms % 1000);
    }
    return buf;
}

KcValue mn_score_wrap(MnScore *s) {
    KcHandle *h = kc_malloc(sizeof *h);
    h->kind = H_SCORE; h->ptr = s; h->size = 0; h->extra = 0;
    KcValue v; v.type = VAL_HANDLE; v.as.handle = h;
    return v;
}
bool mn_is_score(KcValue v) { return v.type == VAL_HANDLE && v.as.handle->kind == H_SCORE; }
MnScore *mn_score_arg(KcValue v, KcPos pos, const char *who) {
    if (!mn_is_score(v)) kc_error_at(pos, "music_note.%s: expected a score (from music_note.parse) or a notation string, got %s", who, kc_type_name(v));
    if (!v.as.handle->ptr) kc_error_at(pos, "music_note.%s: the score was already freed", who);
    return (MnScore *)v.as.handle->ptr;
}

/* ============================================================ durations */

static bool num_of(const char *s, double *out) {
    char *e; if (!*s) return false;
    double v = strtod(s, &e);
    if (*e || e == s || !isfinite(v)) return false;
    *out = v; return true;
}
static bool tuplet_factor(int n, double *f) {
    switch (n) { case 2: *f = 1.5; return true; case 3: *f = 2.0 / 3; return true; case 4: *f = 0.75; return true;
                 case 5: *f = 0.8; return true; case 6: *f = 4.0 / 6; return true; case 7: *f = 4.0 / 7; return true;
                 case 9: *f = 8.0 / 9; return true; default: return false; }
}
/* one duration term: q  q.  q..  e3  8  8.  1/8  3/16.  1.5b  q*2/3 */
static bool dur_term(const char *t, double *beats) {
    char buf[64]; size_t L = strlen(t); if (!L || L >= sizeof buf) return false;
    memcpy(buf, t, L + 1);
    double factor = 1.0;
    char *star = strchr(buf, '*');
    if (star) {
        *star = 0; const char *f = star + 1; char *sl = strchr((char *)f, '/');
        if (sl) { *sl = 0; double p, q; if (!num_of(f, &p) || !num_of(sl + 1, &q) || q == 0) return false; factor = p / q; }
        else if (!num_of(f, &factor)) return false;
        if (factor <= 0) return false;
    }
    L = strlen(buf); if (!L) return false;
    double base; char *p = buf;
    int dots = 0;
    if (strchr(buf, '/')) {
        char *sl = strchr(buf, '/'); *sl = 0;
        char *e; long pn = strtol(buf, &e, 10); if (*e || e == buf) return false;
        long qn = strtol(sl + 1, &e, 10); if (e == sl + 1 || qn <= 0) return false;
        base = 4.0 * (double)pn / (double)qn;
        while (*e == '.') { dots++; e++; }
        if (*e) return false;
    } else if (buf[L - 1] == 'b' && (isdigit((unsigned char)buf[0]) || buf[0] == '.')) {
        buf[L - 1] = 0; if (!num_of(buf, &base)) return false;
    } else if (isdigit((unsigned char)buf[0])) {
        char *e; long den = strtol(buf, &e, 10);
        if (den != 1 && den != 2 && den != 4 && den != 8 && den != 16 && den != 32 && den != 64) return false;
        base = 4.0 / (double)den;
        while (*e == '.') { dots++; e++; }
        if (*e) return false;
    } else {
        switch (tolower((unsigned char)*p)) {
            case 'w': base = 4.0; break; case 'h': base = 2.0; break; case 'q': base = 1.0; break; case 'e': base = 0.5; break;
            case 's': base = 0.25; break; case 't': base = 0.125; break; case 'x': base = 0.0625; break; default: return false;
        }
        p++;
        while (*p == '.') { dots++; p++; }
        if (isdigit((unsigned char)*p)) { double tf; if (p[1] || !tuplet_factor(*p - '0', &tf)) return false; factor *= tf; p++; }
        if (*p) return false;
    }
    if (dots) base *= 2.0 - pow(2.0, -dots);
    base *= factor;
    if (!(base > 0) || base > 1000) return false;
    *beats = base; return true;
}
static bool parse_dur(const char *s, size_t len, double *beats) {
    char buf[96]; if (!len || len >= sizeof buf) return false;
    memcpy(buf, s, len); buf[len] = 0;
    double total = 0; char *p = buf;
    for (;;) {
        char *plus = strchr(p, '+'); if (plus) *plus = 0;
        double b; if (!dur_term(p, &b)) return false;
        total += b;
        if (!plus) break;
        p = plus + 1;
    }
    *beats = total; return true;
}

/* ============================================================ generation state */

typedef struct {
    int octave; double len; int vel; double gate, cur; int transpose; double strum, swing, swing16;
    int fifths; bool minor; MnPatch patch; int pidx;
    struct { int midi; int ev; double end; } ties[32]; int nties;
    double bar_acc, bar_len; int bar_no; bool bar_started;
} St;

typedef struct { char name[40]; TL toks; int init_fifths; bool init_minor; } PTr;

static int dyn_value(const char *s) {
    static const struct { const char *n; int v; } D[] = { { "ppp", 22 }, { "pp", 36 }, { "p", 50 }, { "mp", 64 }, { "mf", 80 }, { "f", 96 }, { "ff", 112 }, { "fff", 124 } };
    for (size_t i = 0; i < sizeof D / sizeof D[0]; i++) if (!strcmp(D[i].n, s)) return D[i].v;
    return -1;
}
static int parse_vel(const Tok *tk, const char *s) {
    int d = dyn_value(s); if (d >= 0) return d;
    double v = 0; if (!num_of(s, &v) || v < 1 || v > 127) perr(tk, "velocity must be 1..127 or one of ppp pp p mp mf f ff fff (got '%s')", s);
    return (int)lround(v);
}
static void touch_patch(MnScore *S, St *st) { st->pidx = patch_idx(S, &st->patch); }

static void ev_push(MnTrack *T, const Tok *tk, MnEv e) {
    if (T->n >= MAX_EVENTS) perr(tk, "too many notes in track '%s' (limit %d)", T->name, MAX_EVENTS);
    if (T->n == T->cap) { T->cap = T->cap ? T->cap * 2 : 128; T->ev = kc_realloc(T->ev, (size_t)T->cap * sizeof(MnEv)); }
    T->ev[T->n++] = e;
}

/* One pitched or drum note at the current position. */
static void emit_note(MnScore *S, MnTrack *T, St *st, const Tok *tk, int midi, int drum, double dur, int vel, double gate, double off, bool tie) {
    if (dur <= 0) return;
    double beat = st->cur;
    if (!drum) {
        for (int i = 0; i < st->nties; i++) {
            if (st->ties[i].midi == midi && fabs(st->ties[i].end - beat) < 1e-6) {
                MnEv *e = &T->ev[st->ties[i].ev];
                e->dur += dur; e->gate = (float)gate;
                if (tie) st->ties[i].end = beat + dur;
                else { st->ties[i] = st->ties[--st->nties]; }
                return;
            }
        }
    }
    double shift = 0;
    if (st->swing > 0 && dur <= 0.5 + 1e-9) {
        double fr = beat - floor(beat);
        if (fabs(fr - 0.5) < 1e-6) shift = st->swing * 0.5;
    }
    if (st->swing16 > 0 && dur <= 0.25 + 1e-9) {
        double q = beat * 4.0, qi = floor(q + 0.5);
        if (fabs(q - qi) < 1e-6 && ((long)qi & 1)) shift += st->swing16 * 0.25;
    }
    MnEv e; e.beat = beat + shift; e.dur = dur - shift > 0.02 ? dur - shift : dur; e.vel = (float)vel; e.gate = (float)gate; e.off = (float)off;
    e.midi = (int16_t)midi; e.patch = (int16_t)st->pidx; e.drum = (uint8_t)drum; e.smp = 0;
    if (T->n == 0) T->first_patch = st->pidx;
    ev_push(T, tk, e);
    if (tie && !drum && st->nties < 32) { st->ties[st->nties].midi = midi; st->ties[st->nties].ev = T->n - 1; st->ties[st->nties].end = beat + dur; st->nties++; }
}

static void emit_sample(MnScore *S, MnTrack *T, St *st, const Tok *tk, int smp_idx1, int semis, double dur, int vel) {
    (void)S;
    MnEv e; memset(&e, 0, sizeof e);
    e.beat = st->cur; e.dur = dur; e.vel = (float)vel; e.gate = 1.0f; e.midi = (int16_t)semis; e.patch = (int16_t)st->pidx; e.smp = (int16_t)smp_idx1;
    if (T->n == 0) T->first_patch = st->pidx;
    ev_push(T, tk, e);
}

typedef struct { double dur; bool has_dur; int vel; double gate; bool accent, tie; } Suf;

static void parse_suffix(const Tok *tk, const char *p, Suf *sf) {
    sf->has_dur = false; sf->vel = -1; sf->gate = -1; sf->accent = false; sf->tie = false; sf->dur = 0;
    while (*p) {
        if (*p == ':') {
            p++; const char *q = p;
            while (*q && !strchr("~'!^@", *q)) q++;
            if (q == p) perr(tk, "missing duration after ':'");
            if (!parse_dur(p, (size_t)(q - p), &sf->dur)) perr(tk, "bad duration '%.*s' (examples: q  e.  h  8  1/8  e3  1.5b  q+e)", (int)(q - p), p);
            sf->has_dur = true; p = q;
        } else if (*p == '~') { sf->tie = true; p++; }
        else if (*p == '\'') { sf->gate = 0.5; p++; }
        else if (*p == '!') { sf->accent = true; p++; }
        else if (*p == '^') { sf->gate = 1.0; p++; }
        else if (*p == '@') {
            p++; const char *q = p;
            while (*q && !strchr("~'!^:", *q)) q++;
            char b[16]; size_t L = (size_t)(q - p); if (!L || L >= sizeof b) perr(tk, "bad velocity after '@'");
            memcpy(b, p, L); b[L] = 0; sf->vel = parse_vel(tk, b); p = q;
        } else perr(tk, "unexpected '%c' in '%s' (did you mean a duration like ':q'?)", *p, tk->s);
    }
}

typedef enum { K_NOTE, K_DRUM, K_REST, K_SAMPLE, K_BAD } WordKind;

static int sample_find(const MnScore *S, const char *name) {
    for (int i = 0; i < S->nsmp; i++) if (!strcmp(S->smp[i].name, name)) return i;
    return -1;
}

/* classify the pitch word of a token; for notes returns the midi number (key signature + transpose applied) */
static WordKind classify(const MnScore *S, St *st, const char *w, int *midi, int *drum) {
    (void)S;
    if (!strcmp(w, "r") || !strcmp(w, "R") || !strcmp(w, "rest") || !strcmp(w, "-")) return K_REST;
    if (w[0] == '&') {                                   /* explicit sample trigger: &name */
        int si = sample_find(S, w + 1);
        if (si < 0) return K_BAD;
        *drum = si + 1; *midi = st->transpose; return K_SAMPLE;
    }
    bool drumtrack = mn_inst_is_drum(st->patch.inst);
    if (drumtrack) {
        int d = mn_drum_find(w); if (d) { *drum = d; *midi = mn_drum_midi(d); return K_DRUM; }
        int si = sample_find(S, w); if (si >= 0) { *drum = si + 1; *midi = st->transpose; return K_SAMPLE; }
        return K_BAD;
    }
    MnNote n; const char *e = mn_note_scan(w, &n);
    if (e && !*e) {
        int acc = n.acc;
        if (!n.has_acc) acc = mn_key_acc(st->fifths, n.letter);
        *midi = ((n.has_oct ? n.octave : st->octave) + 1) * 12 + n.pc + acc + st->transpose;
        return K_NOTE;
    }
    { int si = sample_find(S, w); if (si >= 0) { *drum = si + 1; *midi = st->transpose; return K_SAMPLE; } }
    return K_BAD;
}

static void check_midi(const Tok *tk, int midi) { if (midi < 0 || midi > 127) perr(tk, "note is outside the MIDI range 0..127 (after octave/transpose)"); }

static void bar_add(const Tok *tk, St *st, double d) { (void)tk; st->bar_acc += d; st->bar_started = true; }

/* ---------------------------------------------------------------- option tokens */
static void apply_opt(MnScore *S, MnTrack *T, St *st, const Tok *tk, bool *patch_dirty) {
    char key[40], *val; size_t kl;
    char *eq = strchr(tk->s, '=');
    kl = (size_t)(eq - tk->s); if (kl >= sizeof key) perr(tk, "unknown option '%s'", tk->s);
    memcpy(key, tk->s, kl); key[kl] = 0; val = eq + 1;
    for (char *c = key; *c; c++) *c = (char)tolower((unsigned char)*c);
    double v = 0;
#define NUMV(lo, hi, what) do { if (!num_of(val, &v) || v < (lo) || v > (hi)) perr(tk, "%s must be a number from %g to %g (got '%s')", what, (double)(lo), (double)(hi), val); } while (0)
    if (!strcmp(key, "at") || !strcmp(key, "wait") || !strcmp(key, "mark")) {
        if (!strcmp(key, "mark")) {
            if (!val[0] || strlen(val) >= sizeof S->mk[0].name || !isalpha((unsigned char)val[0])) perr(tk, "marker names start with a letter and are at most 39 characters (got '%s')", val);
            for (int i = 0; i < S->nmk; i++) if (!strcmp(S->mk[i].name, val)) perr(tk, "marker '%s' is defined twice", val);
            if (S->nmk == S->capmk) { S->capmk = S->capmk ? S->capmk * 2 : 8; S->mk = kc_realloc(S->mk, (size_t)S->capmk * sizeof(MnMarker)); }
            MnMarker *m = &S->mk[S->nmk++]; memset(m, 0, sizeof *m); snprintf(m->name, sizeof m->name, "%s", val); m->beat = st->cur; m->track = S->ntr - 1;
        } else if (!strcmp(key, "at")) {
            double sec;
            if (isalpha((unsigned char)val[0]) && !mn_parse_time(val, &sec)) {       /* at=chorus : jump to a marker */
                int mi = -1; for (int i = 0; i < S->nmk; i++) if (!strcmp(S->mk[i].name, val)) mi = i;
                if (mi < 0) perr(tk, "unknown marker '%s' (define it earlier with mark=%s or a `mark %s <time>` line)", val, val, val);
                if (S->mk[mi].timed) { int nt; MnTempo *tm = tmap_build(S, &nt); st->cur = tm_sec_to_beat(tm, nt, S->mk[mi].sec); free(tm); }
                else st->cur = S->mk[mi].beat;
            } else {
                if (!mn_parse_time(val, &sec)) perr(tk, "bad timestamp '%s' (examples: 12.5s  500ms  1:02.5  0:01:02.500  1m30s)", val);
                int nt; MnTempo *tm = tmap_build(S, &nt); st->cur = tm_sec_to_beat(tm, nt, sec); free(tm);
            }
            st->nties = 0; st->bar_acc = 0; st->bar_started = false;           /* an absolute jump starts a fresh bar count */
        } else {
            double sec; if (!mn_parse_time(val, &sec)) perr(tk, "bad duration '%s' for wait= (examples: 2s  500ms  1:30)", val);
            int nt; MnTempo *tm = tmap_build(S, &nt);
            double nb = tm_sec_to_beat(tm, nt, tm_beat_to_sec(tm, nt, st->cur) + sec); free(tm);
            bar_add(tk, st, nb - st->cur); st->cur = nb;
        }
    }
    else if (!strcmp(key, "sample")) {
        int si = sample_find(S, val);
        if (si < 0) perr(tk, "unknown sample '%s' (declare it with: sample %s \"file.wav\")", val, val);
        st->patch.sample = si + 1; st->patch.inst = mn_inst_find("sample"); *patch_dirty = true;
    }
    else if (!strcmp(key, "tempo") || !strcmp(key, "bpm")) { NUMV(10, 999, "tempo"); tempo_push(S, st->cur, v); }
    else if (!strcmp(key, "time")) {
        int a, b; char tail; if (sscanf(val, "%d/%d%c", &a, &b, &tail) != 2 || a < 1 || a > 64 || (b != 1 && b != 2 && b != 4 && b != 8 && b != 16 && b != 32)) perr(tk, "time signature must look like 4/4 or 6/8 (got '%s')", val);
        S->ts_num = a; S->ts_den = b; st->bar_len = 4.0 * a / b;
    }
    else if (!strcmp(key, "key")) {
        int f; bool mi; if (!mn_parse_key(val, &f, &mi)) perr(tk, "unknown key '%s' (examples: G  Bb  F#m  Am)", val);
        st->fifths = f; st->minor = mi;
        if (!S->has_key) { S->key_fifths = f; S->key_minor = mi; S->has_key = true; }
    }
    else if (!strcmp(key, "inst") || !strcmp(key, "instrument")) {
        int id = mn_inst_find(val);
        if (id < 0) perr(tk, "unknown instrument '%s' (see music_note.instruments())", val);
        st->patch.inst = id; if (id != mn_inst_find("sample")) st->patch.sample = 0;
        *patch_dirty = true;
    }
    else if (!strcmp(key, "vol") || !strcmp(key, "volume")) { NUMV(0, 2, "vol"); T->vol = v; }
    else if (!strcmp(key, "pan")) { NUMV(-1, 1, "pan"); T->pan = v; }
    else if (!strcmp(key, "reverb")) { NUMV(0, 1, "reverb"); T->reverb = v; }
    else if (!strcmp(key, "delay")) { NUMV(0, 1, "delay"); T->delay = v; }
    else if (!strcmp(key, "delay_time")) { NUMV(0.01, 16, "delay_time (beats)"); T->delay_time = v; }
    else if (!strcmp(key, "delay_fb")) { NUMV(0, 0.95, "delay_fb"); T->delay_fb = v; }
    else if (!strcmp(key, "chorus")) { NUMV(0, 1, "chorus"); T->chorus = v; }
    else if (!strcmp(key, "lowpass") || !strcmp(key, "cutoff")) { NUMV(20, 20000, "lowpass"); T->lowpass = v; }
    else if (!strcmp(key, "highpass")) { NUMV(5, 20000, "highpass"); T->highpass = v; }
    else if (!strcmp(key, "swing")) { NUMV(0, 0.75, "swing"); st->swing = v; }
    else if (!strcmp(key, "swing16")) { NUMV(0, 0.75, "swing16"); st->swing16 = v; }
    else if (!strcmp(key, "glide")) { NUMV(0, 1, "glide (seconds)"); T->glide = v; }
    else if (!strcmp(key, "transpose")) { NUMV(-48, 48, "transpose"); st->transpose = (int)lround(v); }
    else if (!strcmp(key, "strum")) { NUMV(0, 0.5, "strum (seconds)"); st->strum = v; }
    else if (!strcmp(key, "gate")) { NUMV(0.05, 1.5, "gate"); st->gate = v; }
    else if (!strcmp(key, "vel") || !strcmp(key, "velocity") || !strcmp(key, "dyn")) st->vel = parse_vel(tk, val);
    else if (!strcmp(key, "octave") || !strcmp(key, "o")) { NUMV(-1, 9, "octave"); st->octave = (int)lround(v); }
    else if (!strcmp(key, "len") || !strcmp(key, "l")) { if (!parse_dur(val, strlen(val), &st->len)) perr(tk, "bad duration '%s'", val); }
    else if (!strcmp(key, "attack")) { NUMV(0, 20, "attack"); st->patch.atk = v; *patch_dirty = true; }
    else if (!strcmp(key, "decay")) { NUMV(0, 20, "decay"); st->patch.dec = v; *patch_dirty = true; }
    else if (!strcmp(key, "sustain")) { NUMV(0, 1, "sustain"); st->patch.sus = v; *patch_dirty = true; }
    else if (!strcmp(key, "release")) { NUMV(0, 20, "release"); st->patch.rel = v; *patch_dirty = true; }
    else if (!strcmp(key, "adsr")) {
        double a[4]; char b[80]; snprintf(b, sizeof b, "%s", val); int k = 0; char *sv = NULL;
        for (char *t = mn_tok(b, ",", &sv); t && k < 4; t = mn_tok(NULL, ",", &sv)) { if (!num_of(t, &a[k]) || a[k] < 0) perr(tk, "adsr must be four numbers like adsr=0.01,0.1,0.7,0.3"); k++; }
        if (k != 4 || a[2] > 1) perr(tk, "adsr must be four numbers: attack,decay,sustain(0..1),release");
        st->patch.atk = a[0]; st->patch.dec = a[1]; st->patch.sus = a[2]; st->patch.rel = a[3]; *patch_dirty = true;
    }
    else if (!strcmp(key, "vibrato")) {
        if (!strcmp(val, "off") || !strcmp(val, "0")) { st->patch.vib_rate = 0; st->patch.vib_depth = 0; }
        else {
            char b[48]; snprintf(b, sizeof b, "%s", val); char *c = strchr(b, ','); double r, d;
            if (!c) perr(tk, "vibrato must be 'rate,depth' (Hz, semitones) like vibrato=5.5,0.2, or 'off'");
            *c = 0; if (!num_of(b, &r) || !num_of(c + 1, &d) || r < 0 || r > 30 || d < 0 || d > 2) perr(tk, "vibrato rate must be 0..30 Hz and depth 0..2 semitones");
            st->patch.vib_rate = r; st->patch.vib_depth = d;
        }
        *patch_dirty = true;
    }
    else if (!strcmp(key, "detune")) { NUMV(0, 100, "detune (cents)"); st->patch.detune = v; *patch_dirty = true; }
    else perr(tk, "unknown option '%s'", key);
#undef NUMV
}

/* ---------------------------------------------------------------- one track */
typedef struct { char name[48]; TL toks; } PPat;     /* a `pattern NAME ... end` block, played with $NAME or $NAME*3 */

static void expand_tok(TL *out, const Tok *tk, const PPat *pats, int np, int depth) {
    if (tk->s[0] == '$' && tk->s[1]) {
        char name[48]; const char *p = tk->s + 1; size_t L = 0;
        while (p[L] && p[L] != '*' && L < sizeof name - 1) L++;
        memcpy(name, p, L); name[L] = 0;
        int cnt = 1;
        if (p[L] == '*') { cnt = atoi(p + L + 1); if (cnt < 1 || cnt > 1000) perr(tk, "pattern repeat count must be 1..1000"); }
        int pi = -1; for (int i = 0; i < np; i++) if (!strcmp(pats[i].name, name)) pi = i;
        if (pi < 0) perr(tk, "unknown pattern '$%s' (define it with:  pattern %s ... end)", name, name);
        if (depth >= 16) perr(tk, "patterns are nested too deeply (does '$%s' call itself?)", name);
        for (int r = 0; r < cnt; r++) for (int k = 0; k < pats[pi].toks.n; k++) expand_tok(out, &pats[pi].toks.t[k], pats, np, depth + 1);
        return;
    }
    tl_push(out, tk->s, strlen(tk->s), tk->line, tk->col);
}

static void gen_track(MnScore *S, PTr *pt, const TL *hdr, const PPat *pats, int npats, bool strict) {
    if (S->ntr == S->captr) { S->captr = S->captr ? S->captr * 2 : 8; S->tr = kc_realloc(S->tr, (size_t)S->captr * sizeof(MnTrack)); }
    MnTrack *T = &S->tr[S->ntr++];
    memset(T, 0, sizeof *T);
    snprintf(T->name, sizeof T->name, "%s", pt->name);
    T->vol = 1.0; T->delay_time = 0.75; T->delay_fb = 0.35;
    St st; memset(&st, 0, sizeof st);
    st.octave = 4; st.len = 1.0; st.vel = 80; st.gate = 0.97; st.fifths = pt->init_fifths; st.minor = pt->init_minor;
    st.patch = default_patch(); st.bar_len = 4.0 * S->ts_num / S->ts_den; st.bar_no = 1;
    touch_patch(S, &st); T->first_patch = st.pidx;

    /* expand patterns ($name), then repeats */
    TL pre = { 0 };
    for (int i = 0; i < hdr->n; i++) expand_tok(&pre, &hdr->t[i], pats, npats, 0);
    for (int i = 0; i < pt->toks.n; i++) expand_tok(&pre, &pt->toks.t[i], pats, npats, 0);
    TL ex = { 0 };
    int starts[64], ns = 0;
    int total_in = pre.n;
    for (int i = 0; i < total_in; i++) {
        Tok *tk = &pre.t[i];
        if (!strcmp(tk->s, "|:")) { if (ns >= 64) perr(tk, "repeats nested too deeply"); starts[ns++] = ex.n; continue; }
        if (tk->s[0] == ':' && tk->s[1] == '|') {
            int cnt = 2;
            if (tk->s[2] == 'x' || tk->s[2] == 'X') cnt = atoi(tk->s + 3);
            if (cnt < 1 || cnt > 1000) perr(tk, "repeat count must be 1..1000");
            int from = ns > 0 ? starts[--ns] : 0, to = ex.n;
            for (int r = 1; r < cnt; r++) for (int k = from; k < to; k++) tl_push(&ex, ex.t[k].s, strlen(ex.t[k].s), ex.t[k].line, ex.t[k].col);
            continue;
        }
        tl_push(&ex, tk->s, strlen(tk->s), tk->line, tk->col);
    }

    for (int i = 0; i < ex.n; i++) {
        const Tok *tk = &ex.t[i]; const char *s = tk->s;
        for (int k = 0; k < st.nties; ) { if (st.ties[k].end < st.cur - 1e-6) st.ties[k] = st.ties[--st.nties]; else k++; }

        if (!strcmp(s, "|")) {
            if (strict && st.bar_started && st.bar_acc > 1e-6 && fabs(st.bar_acc - st.bar_len) > 1e-6 && !(st.bar_no == 1))
                perr(tk, "bar %d of track '%s' has %g beats but the time signature %d/%d needs %g", st.bar_no, T->name, st.bar_acc, S->ts_num, S->ts_den, st.bar_len);
            if (st.bar_acc > 1e-6) st.bar_no++;
            st.bar_acc = 0; st.bar_started = false;
            continue;
        }
        if (!strcmp(s, "<") || !strcmp(s, ">")) { st.octave += s[0] == '>' ? 1 : -1; continue; }
        if ((s[0] == '<' || s[0] == '>') && s[1] == s[0] && !s[2]) { st.octave += s[0] == '>' ? 2 : -2; continue; }
        if (s[0] == '@') { st.vel = parse_vel(tk, s + 1); continue; }
        if (dyn_value(s) >= 0 && strcmp(s, "f") != 0) { st.vel = dyn_value(s); continue; }
        if ((s[0] == 'o' || s[0] == 'O') && (isdigit((unsigned char)s[1]) || (s[1] == '-' && isdigit((unsigned char)s[2])))) {
            char *e; long o = strtol(s + 1, &e, 10); if (*e || o < -1 || o > 9) perr(tk, "bad octave '%s'", s);
            st.octave = (int)o; continue;
        }
        if (isalpha((unsigned char)s[0]) && strchr(s, '=') && (!strchr(s, ':') || strchr(s, ':') > strchr(s, '=')) && s[0] != '[' ) {
            const char *eq = strchr(s, '='); const char *br = strpbrk(s, "[{");
            if (!br || br > eq) {
                bool dirty = false; apply_opt(S, T, &st, tk, &dirty);
                if (dirty) { if (mn_inst_is_drum(st.patch.inst)) T->drums = true; touch_patch(S, &st); }
                continue;
            }
        }

        /* ---- a note-like token ---- */
        Suf sf;
        if (s[0] == '[') {
            const char *close = strchr(s, ']');
            parse_suffix(tk, close + 1, &sf);
            char inner[400]; size_t L = (size_t)(close - s - 1); if (L >= sizeof inner) perr(tk, "chord too long");
            memcpy(inner, s + 1, L); inner[L] = 0;
            double dur = sf.has_dur ? sf.dur : st.len;
            int vel = sf.vel >= 0 ? sf.vel : st.vel; if (sf.accent) vel = vel + 20 > 127 ? 127 : vel + 20;
            double gate = sf.gate > 0 ? sf.gate : st.gate;
            char *sv = NULL; int idx = 0;
            for (char *w = mn_tok(inner, " \t", &sv); w; w = mn_tok(NULL, " \t", &sv), idx++) {
                bool tie = sf.tie; size_t wl = strlen(w);
                if (wl > 1 && w[wl - 1] == '~') { tie = true; w[wl - 1] = 0; }
                int midi = 0, drum = 0; WordKind kd = classify(S, &st, w, &midi, &drum);
                if (kd == K_REST) perr(tk, "a rest cannot be part of a chord");
                if (kd == K_BAD) perr(tk, "'%s' is not a %s inside the chord", w, mn_inst_is_drum(st.patch.inst) ? "drum name" : "note name");
                if (kd == K_SAMPLE) { emit_sample(S, T, &st, tk, drum, midi, dur, vel); continue; }
                if (kd == K_NOTE) check_midi(tk, midi); else T->drums = true;
                emit_note(S, T, &st, tk, midi, drum, dur, vel, gate, st.strum * idx, tie);
            }
            if (idx == 0) perr(tk, "empty chord []");
            st.cur += dur; bar_add(tk, &st, dur);
            continue;
        }
        if (s[0] == '{') {
            const char *close = strchr(s, '}');
            parse_suffix(tk, close + 1, &sf);
            char inner[64]; size_t L = (size_t)(close - s - 1); if (!L || L >= sizeof inner) perr(tk, "bad chord symbol");
            memcpy(inner, s + 1, L); inner[L] = 0;
            int mids[16], cnt = 0; const char *e = NULL;
            if (!mn_chord_symbol(inner, st.octave, mids, &cnt, 16, &e) || (e && *e)) perr(tk, "cannot read chord symbol '%s' (examples: {C} {Am7} {F#m7b5} {G/B} {Dsus4})", inner);
            double dur = sf.has_dur ? sf.dur : st.len;
            int vel = sf.vel >= 0 ? sf.vel : st.vel; if (sf.accent) vel = vel + 20 > 127 ? 127 : vel + 20;
            double gate = sf.gate > 0 ? sf.gate : st.gate;
            if (mn_inst_is_drum(st.patch.inst)) perr(tk, "chord symbols cannot be used on a drum track");
            for (int k = 0; k < cnt; k++) { int m = mids[k] + st.transpose; check_midi(tk, m); emit_note(S, T, &st, tk, m, 0, dur, vel, gate, st.strum * k, sf.tie); }
            st.cur += dur; bar_add(tk, &st, dur);
            continue;
        }
        /* plain note / rest / drum word */
        const char *p = s; while (*p && !strchr(":~'!^@", *p)) p++;
        size_t wl = (size_t)(p - s); char word[48]; if (!wl || wl >= sizeof word) perr(tk, "unrecognized token '%s'", s);
        memcpy(word, s, wl); word[wl] = 0;
        int midi = 0, drum = 0; WordKind kd = classify(S, &st, word, &midi, &drum);
        if (kd == K_BAD) {
            if (!mn_inst_is_drum(st.patch.inst) && mn_drum_find(word)) perr(tk, "'%s' is a drum name but track '%s' is not a drum track (add `drums` to its track line)", word, T->name);
            if (mn_inst_is_drum(st.patch.inst)) perr(tk, "'%s' is not a drum name (see music_note.drums())", word);
            if (word[0] == '&') perr(tk, "unknown sample '%s' (declare it with: sample %s \"file.wav\")", word + 1, word + 1);
            perr(tk, "unrecognized token '%s' (notes look like C4, F#3, Bb5; rests are r or -; chords are [C4 E4 G4] or {Cmaj7}; samples are &name; patterns are $name)", s);
        }
        parse_suffix(tk, p, &sf);
        double dur = sf.has_dur ? sf.dur : st.len;
        if (kd != K_REST) {
            int vel = sf.vel >= 0 ? sf.vel : st.vel; if (sf.accent) vel = vel + 20 > 127 ? 127 : vel + 20;
            double gate = sf.gate > 0 ? sf.gate : st.gate;
            if (kd == K_SAMPLE) emit_sample(S, T, &st, tk, drum, midi, dur, vel);
            else {
                if (kd == K_NOTE) check_midi(tk, midi); else T->drums = true;
                if (kd == K_NOTE && st.patch.inst == mn_inst_find("sample") && !st.patch.sample) perr(tk, "the 'sample' instrument needs a sample: add sample=NAME to the track");
                emit_note(S, T, &st, tk, midi, drum, dur, vel, gate, 0, sf.tie);
            }
        }
        st.cur += dur; bar_add(tk, &st, dur);
    }
    tl_free(&ex); tl_free(&pre);
    if (strict && st.bar_started && st.bar_acc > 1e-6 && fabs(st.bar_acc - st.bar_len) > 1e-6 && st.bar_no > 1)
        perr(&pt->toks.t[pt->toks.n ? pt->toks.n - 1 : 0], "last bar of track '%s' has %g beats but the time signature needs %g", T->name, st.bar_acc, st.bar_len);
    if (T->n == 0 && st.patch.inst >= 0) T->first_patch = st.pidx;
    double end = st.cur;
    for (int i = 0; i < T->n; i++) { double e = T->ev[i].beat + T->ev[i].dur; if (e > end) end = e; }
    if (end > S->total_beats) S->total_beats = end;
}

/* ============================================================ parser */

static const char *OPT_KEYS[] = { "tempo", "bpm", "time", "key", "inst", "instrument", "vol", "volume", "pan", "reverb", "delay", "delay_time",
    "delay_fb", "chorus", "lowpass", "cutoff", "highpass", "swing", "transpose", "strum", "gate", "vel", "velocity", "dyn", "octave", "len",
    "swing16", "glide", "at", "wait", "mark", "sample", "adsr", "attack", "decay", "sustain", "release", "vibrato", "detune" };

static bool is_opt_key(const char *s) {
    for (size_t i = 0; i < sizeof OPT_KEYS / sizeof OPT_KEYS[0]; i++) if (!strcasecmp(OPT_KEYS[i], s)) return true;
    return false;
}

/* ============================================================ preprocessor: continuations, block comments, includes, inline audio */

typedef struct { char *s; int file, line; } Ln;
typedef struct { Ln *l; int n, cap; } LV;
static void lv_push(LV *v, char *s, int file, int line) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 64; v->l = kc_realloc(v->l, (size_t)v->cap * sizeof(Ln)); }
    v->l[v->n].s = s; v->l[v->n].file = file; v->l[v->n].line = line; v->n++;
}
static void lv_free(LV *v) { for (int i = 0; i < v->n; i++) free(v->l[i].s); free(v->l); v->l = NULL; v->n = v->cap = 0; }

typedef struct { const char *t; size_t len, pos; int line; } Rd;
static bool rd_line(Rd *r, char **out) {
    if (r->pos >= r->len) return false;
    size_t j = r->pos; while (j < r->len && r->t[j] != '\n') j++;
    size_t L = j - r->pos; if (L && r->t[r->pos + L - 1] == '\r') L--;
    char *b = kc_malloc(L + 1); memcpy(b, r->t + r->pos, L); b[L] = 0;
    r->pos = j + 1; r->line++; *out = b; return true;
}
/* blank out block comments (they may span lines); line comments are left alone */
static void strip_block(char *s, bool *in) {
    for (size_t i = 0; s[i]; ) {
        if (*in) {
            if (s[i] == '*' && s[i + 1] == '/') { s[i] = s[i + 1] = ' '; i += 2; *in = false; }
            else { s[i++] = ' '; }
        } else {
            if (s[i] == '/' && s[i + 1] == '*') { s[i] = s[i + 1] = ' '; i += 2; *in = true; }
            else if (s[i] == '/' && s[i + 1] == '/') break;
            else if ((s[i] == '#' || s[i] == ';') && (i == 0 || is_ws(s[i - 1]))) break;
            else i++;
        }
    }
}
static size_t code_len(const char *s) {
    for (size_t i = 0; s[i]; i++) {
        if ((s[i] == '#' || s[i] == ';') && (i == 0 || is_ws(s[i - 1]))) return i;
        if (s[i] == '/' && s[i + 1] == '/') return i;
    }
    return strlen(s);
}
static void path_dir(const char *p, char *dir, size_t cap) {
    const char *sl = NULL; for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') sl = q;
    size_t n = sl ? (size_t)(sl - p) : 0; if (n >= cap) n = cap - 1;
    memcpy(dir, p, n); dir[n] = 0;
}
static void path_join(const char *dir, const char *rel, char *out, size_t cap) {
    bool abs_ = rel[0] == '/' || rel[0] == '\\' || (isalpha((unsigned char)rel[0]) && rel[1] == ':');
    if (abs_ || !dir[0]) snprintf(out, cap, "%s", rel); else snprintf(out, cap, "%s/%s", dir, rel);
}
static char *trim_dup(const char *s) {
    while (is_ws(*s)) s++;
    size_t L = strlen(s); while (L && is_ws(s[L - 1])) L--;
    char *d = kc_malloc(L + 1); memcpy(d, s, L); d[L] = 0; return d;
}

static void check_readable(const char *full, int enc, const char *what) {
    FILE *f = fopen(full, "rb");
    if (!f) perr_at(enc, 1, "cannot open %s '%s': %s", what, full, strerror(errno));
    fclose(f);
}

static void preprocess(LV *v, const char *text, size_t len, int file, int depth, const MnParseOpts *po, const char **stack, int sdepth) {
    Rd rd = { text, len, 0, 0 }; bool in_bc = false; char *phys;
    while (rd_line(&rd, &phys)) {
        int start_line = rd.line;
        /* `sample NAME [options] data` ... `end`: inline audio, copied verbatim (base64 may contain // and similar) */
        if (!in_bc) {
            const char *p = phys; while (is_ws(*p)) p++;
            if (!strncasecmp(p, "sample", 6) && is_ws(p[6])) {
                size_t cl = code_len(p); while (cl && is_ws(p[cl - 1])) cl--;
                if (cl >= 6 && !strncasecmp(p + cl - 4, "data", 4) && is_ws(p[cl - 5])) {
                    size_t cap = cl + 256, n = 0; char *buf = kc_malloc(cap);
                    memcpy(buf, p, cl); n = cl; buf[n++] = ' ';
                    char *d; bool closed = false;
                    while (rd_line(&rd, &d)) {
                        char *t = trim_dup(d);
                        if (!strcasecmp(t, "end")) { closed = true; free(t); free(d); break; }
                        free(t);
                        size_t dl = strlen(d);
                        if (n + dl + 2 > cap) { cap = (n + dl + 2) * 2; buf = kc_realloc(buf, cap); }
                        for (size_t k = 0; k < dl; k++) if (!is_ws(d[k])) buf[n++] = d[k];
                        free(d);
                    }
                    buf[n] = 0;
                    if (!closed) { g_who = g_who; perr_at((file << 20) | start_line, 1, "the inline audio block is missing its closing 'end'"); }
                    lv_push(v, buf, file, start_line); free(phys); continue;
                }
            }
        }
        /* assemble one logical line: `\` continues, an open [ or { keeps reading */
        size_t cap = strlen(phys) + 16, an = 0; char *acc = kc_malloc(cap); acc[0] = 0;
        int bdepth = 0, joined = 0;
        for (;;) {
            strip_block(phys, &in_bc);
            size_t cl = code_len(phys), el = cl; while (el && is_ws(phys[el - 1])) el--;
            bool cont = el > 0 && phys[el - 1] == '\\';
            size_t take = cont ? el - 1 : cl;
            for (size_t k = 0; k < take; k++) { if (phys[k] == '[' || phys[k] == '{') bdepth++; else if (phys[k] == ']' || phys[k] == '}') bdepth--; }
            bool more = (cont || bdepth > 0) && joined < 255;
            size_t add = more ? take : strlen(phys);
            if (an + add + 2 > cap) { cap = (an + add + 2) * 2; acc = kc_realloc(acc, cap); }
            memcpy(acc + an, phys, add); an += add; if (more) acc[an++] = ' '; acc[an] = 0;
            free(phys); phys = NULL;
            if (!more || !rd_line(&rd, &phys)) break;
            joined++;
        }
        /* include "other.kcm" */
        {
            const char *p = acc; while (is_ws(*p)) p++;
            if (!strncasecmp(p, "include", 7) && is_ws(p[7])) {
                const char *rest = p + 8; while (is_ws(*rest)) rest++;
                char *path;
                if (*rest == '"' || *rest == '\'') {                    /* quoted: up to the closing quote, anything after is a comment */
                    const char *q = strchr(rest + 1, *rest);
                    size_t pl = q ? (size_t)(q - rest - 1) : strlen(rest + 1);
                    path = kc_malloc(pl + 1); memcpy(path, rest + 1, pl); path[pl] = 0;
                } else {
                    size_t pl = code_len(rest); path = kc_malloc(pl + 1); memcpy(path, rest, pl); path[pl] = 0;
                    size_t L2 = strlen(path); while (L2 && is_ws(path[L2 - 1])) path[--L2] = 0;
                }
                if (!path[0]) perr_at((file << 20) | start_line, 1, "include needs a file name, e.g.  include \"drums.kcm\"");
                if (depth >= 8) perr_at((file << 20) | start_line, 1, "includes are nested too deeply (limit 8)");
                char full[420]; path_join(g_fi[file].dir, path, full, sizeof full);
                for (int k = 0; k < sdepth; k++) if (!strcmp(stack[k], full)) perr_at((file << 20) | start_line, 1, "include cycle: '%s' includes itself", full);
                if (po->ip) kc_forbid_in_comptime(po->ip, g_pos, "music_note include");
                check_readable(full, (file << 20) | start_line, "include file");
                size_t flen; uint8_t *fb = mn_read_file(full, &flen, g_pos, g_who);
                char dir[200]; path_dir(full, dir, sizeof dir);
                int fid = fi_add(full, dir);
                const char *st2[10]; for (int k = 0; k < sdepth && k < 9; k++) st2[k] = stack[k];
                st2[sdepth < 9 ? sdepth : 8] = g_fi[fid].name;
                preprocess(v, (const char *)fb, flen, fid, depth + 1, po, st2, sdepth < 9 ? sdepth + 1 : 9);
                free(fb); free(path); free(acc); continue;
            }
        }
        lv_push(v, acc, file, start_line);
    }
    if (in_bc) perr_at((file << 20) | (rd.line ? rd.line : 1), 1, "a /* block comment */ is never closed");
}

/* ---- sample declarations:  sample NAME "file.wav" [opts]   |   sample NAME [opts] data <base64> ---- */
static bool sample_name_ok(const char *n) {
    if (!n[0] || strlen(n) >= 32 || !(isalpha((unsigned char)n[0]) || n[0] == '_')) return false;
    for (const char *p = n; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return false;
    return true;
}
static MnSample *sample_add(MnScore *S, const char *name, int lineenc) {
    if (!sample_name_ok(name)) perr_at(lineenc, 1, "bad sample name '%s' (letters, digits, _ and -, starting with a letter; at most 31 characters)", name);
    MnNote nn; const char *e = mn_note_scan(name, &nn);
    if ((e && !*e) || !strcasecmp(name, "r") || !strcasecmp(name, "rest") || mn_drum_find(name) || dyn_value(name) >= 0 || !strcasecmp(name, "data"))
        perr_at(lineenc, 1, "sample name '%s' clashes with a note, rest, drum or dynamic name; pick another", name);
    for (int i = 0; i < S->nsmp; i++) if (!strcmp(S->smp[i].name, name)) perr_at(lineenc, 1, "sample '%s' is declared twice", name);
    if (S->nsmp >= 512) perr_at(lineenc, 1, "too many samples (limit 512)");
    if (S->nsmp == S->capsmp) { S->capsmp = S->capsmp ? S->capsmp * 2 : 8; S->smp = kc_realloc(S->smp, (size_t)S->capsmp * sizeof(MnSample)); }
    MnSample *m = &S->smp[S->nsmp++]; memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name); m->root = 60;
    return m;
}
static void parse_sample_decl(MnScore *S, const char *line, int file, int lineno, const MnParseOpts *po) {
    int enc = (file << 20) | lineno;
    const char *p = line; while (is_ws(*p)) p++; p += 6; while (is_ws(*p)) p++;
    char name[48]; size_t L = 0; while (p[L] && !is_ws(p[L]) && L < sizeof name - 1) L++;
    memcpy(name, p, L); name[L] = 0; p += L;
    if (!name[0]) perr_at(enc, 1, "sample needs a name:  sample kick \"kick.wav\"");
    char path[420] = ""; const char *data = NULL; size_t dlen = 0;
    double gain = 0, st = 0, en = 0, fi = 0, fo = 0, bpm = 0; int root = 60; bool rev = false, one = false, cut = false;
    while (*p) {
        while (is_ws(*p)) p++;
        if (!*p) break;
        if (*p == '"' || *p == '\'') {
            char q = *p++; const char *e = strchr(p, q);
            if (!e) perr_at(enc, 1, "unterminated quoted file name");
            size_t n = (size_t)(e - p); if (n >= sizeof path) perr_at(enc, 1, "file name too long");
            memcpy(path, p, n); path[n] = 0; p = e + 1; continue;
        }
        const char *w = p; while (*p && !is_ws(*p)) p++;
        size_t wl = (size_t)(p - w); char word[400]; if (wl >= sizeof word) { if (!strncmp(w, "data", 4) && is_ws(w[4])) { wl = 4; p = w + 4; } else perr_at(enc, 1, "bad sample option"); }
        memcpy(word, w, wl); word[wl] = 0;
        if (word[0] == '#' || word[0] == ';' || (word[0] == '/' && word[1] == '/')) break;     /* trailing comment */
        if (!strcmp(word, "data")) { while (is_ws(*p)) p++; data = p; dlen = strlen(p); break; }
        char *eq = strchr(word, '=');
        if (!eq) {
            if (!strcmp(word, "reverse")) rev = true; else if (!strcmp(word, "oneshot")) one = true; else if (!strcmp(word, "cut")) cut = true;
            else if (!path[0]) snprintf(path, sizeof path, "%s", word);
            else perr_at(enc, 1, "unknown sample option '%s' (gain= start= end= root= bpm= fadein= fadeout= reverse oneshot cut)", word);
            continue;
        }
        *eq = 0; const char *k = word, *val = eq + 1; double v = 0;
        if (!strcmp(k, "gain")) { if (!num_of(val, &v)) perr_at(enc, 1, "gain must be a number of dB"); gain = v; }
        else if (!strcmp(k, "start") || !strcmp(k, "end")) { if (!mn_parse_time(val, &v)) perr_at(enc, 1, "bad time '%s' for %s=", val, k); if (k[0] == 's') st = v; else en = v; }
        else if (!strcmp(k, "fadein") || !strcmp(k, "fadeout")) { if (!mn_parse_time(val, &v)) perr_at(enc, 1, "bad time '%s' for %s=", val, k); if (k[4] == 'i') fi = v; else fo = v; }
        else if (!strcmp(k, "bpm")) { if (!num_of(val, &v) || v < 20 || v > 400) perr_at(enc, 1, "bpm must be 20..400"); bpm = v; }
        else if (!strcmp(k, "root")) { MnNote nn; const char *e = mn_note_scan(val, &nn); if (!e || *e) perr_at(enc, 1, "root must be a note name such as C4"); root = mn_note_midi(&nn, 4); }
        else perr_at(enc, 1, "unknown sample option '%s'", k);
    }
    if (data && path[0]) perr_at(enc, 1, "sample '%s' has both a file and inline data", name);
    if (!data && !path[0]) perr_at(enc, 1, "sample '%s' needs a file (sample %s \"file.wav\") or inline data (sample %s data ... end)", name, name, name);
    MnSample *m = sample_add(S, name, enc);
    if (data) {
        size_t n; uint8_t *b = mn_b64_decode(data, dlen, &n);
        if (!b) perr_at(enc, 1, "sample '%s': the inline data is not valid base64", name);
        char label[80]; snprintf(label, sizeof label, "<inline %s>", name);
        m->a = mn_wav_decode(b, n, label, g_pos, g_who); free(b);
        snprintf(m->src, sizeof m->src, "inline");
    } else {
        char full[420]; path_join(g_fi[file].dir, path, full, sizeof full);
        if (po->ip) kc_forbid_in_comptime(po->ip, g_pos, "music_note sample files");
        check_readable(full, enc, "sample file");
        m->a = mn_wav_read(full, g_pos, g_who);
        snprintf(m->src, sizeof m->src, "%s", path);
    }
    m->gain_db = gain; m->start = st; m->end = en; m->fade_in = fi; m->fade_out = fo; m->bpm = bpm; m->root = root; m->reverse = rev; m->oneshot = one; m->cut = cut;
}

/* push a line's tokens onto a list; `tempo 90` / `inst piano` / `at 1:30` become key=value tokens */
static void push_line_tokens(TL *dst, const TL *lt, int start) {
    if (start >= lt->n) return;
    if (start == 0 && lt->n >= 2) {
        char first[40]; snprintf(first, sizeof first, "%s", lt->t[0].s);
        for (char *c = first; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (is_opt_key(first) && !strchr(lt->t[0].s, '=') && lt->t[1].s[0] != ':') {
            char b[120]; snprintf(b, sizeof b, "%s=%s", first, lt->t[1].s);
            tl_push(dst, b, strlen(b), lt->t[0].line, lt->t[0].col); start = 2;
        }
    }
    for (int k = start; k < lt->n; k++) tl_push(dst, lt->t[k].s, strlen(lt->t[k].s), lt->t[k].line, lt->t[k].col);
}

MnScore *mn_score_parse_ex(const char *text, size_t len, const MnParseOpts *po, KcPos pos, const char *who) {
    static const MnParseOpts NOPO = { 0 };
    if (!po) po = &NOPO;
    g_pos = pos; g_who = who; g_nfi = 0;
    int mainfile = fi_add(po->file ? po->file : "", po->base ? po->base : "");
    bool strict = po->strict;
    MnScore *S = kc_malloc(sizeof *S); memset(S, 0, sizeof *S);
    S->ts_num = 4; S->ts_den = 4;
    uint32_t h = 2166136261u; for (size_t i = 0; i < len; i++) { h ^= (uint8_t)text[i]; h *= 16777619u; } S->seed = h;
    /* samples handed in by the host program */
    for (int i = 0; i < po->nnames; i++) {
        MnSample *m = sample_add(S, po->names[i], mainfile << 20);
        m->a = mn_audio_clone(po->audios[i], pos, who); snprintf(m->src, sizeof m->src, "host");
    }
    LV lv = { 0 };
    { const char *stk[10]; int sd = 0; if (po->file && po->file[0]) stk[sd++] = po->file; preprocess(&lv, text, len, mainfile, 0, po, stk, sd); }
    PTr *tr = NULL; int ntr = 0, captr = 0, cur = -1;
    PPat *pats = NULL; int npats = 0, cappats = 0, curpat = -1;
    TL hdr = { 0 };          /* key=value lines before the first track: defaults applied to every track */
    int dfifths = 0; bool dminor = false;
    bool tempo_set = false;
    for (int li = 0; li < lv.n; li++) {
        const char *ln = lv.l[li].s; int lineno = (lv.l[li].file << 20) | lv.l[li].line;
        { const char *q = ln; while (is_ws(*q)) q++;
          if (!strncasecmp(q, "sample", 6) && is_ws(q[6]) && curpat < 0) { parse_sample_decl(S, ln, lv.l[li].file, lv.l[li].line, po); continue; } }
        TL lt = { 0 }; lex_line(&lt, ln, lineno);
        if (lt.n == 0) { tl_free(&lt); continue; }
        char first[40]; snprintf(first, sizeof first, "%s", lt.t[0].s);
        for (char *c = first; *c; c++) *c = (char)tolower((unsigned char)*c);
        int start = 0;
        if (curpat >= 0) {                                   /* inside  pattern NAME ... end */
            if (!strcmp(first, "end")) curpat = -1;
            else push_line_tokens(&pats[curpat].toks, &lt, 0);
            tl_free(&lt); continue;
        }
        if (!strcmp(first, "pattern")) {
            if (lt.n < 2) perr(&lt.t[0], "pattern needs a name:  pattern riff ... end");
            const char *nm = lt.t[1].s;
            if (!isalpha((unsigned char)nm[0]) && nm[0] != '_') perr(&lt.t[1], "pattern names start with a letter");
            if (strlen(nm) >= sizeof pats[0].name) perr(&lt.t[1], "pattern name too long");
            for (int k = 0; k < npats; k++) if (!strcmp(pats[k].name, nm)) perr(&lt.t[1], "pattern '%s' is defined twice", nm);
            if (npats == cappats) { cappats = cappats ? cappats * 2 : 8; pats = kc_realloc(pats, (size_t)cappats * sizeof *pats); }
            memset(&pats[npats], 0, sizeof pats[npats]); snprintf(pats[npats].name, sizeof pats[npats].name, "%s", nm);
            curpat = npats++;
            push_line_tokens(&pats[curpat].toks, &lt, 2);
            tl_free(&lt); continue;
        }
        if (!strcmp(first, "length") && lt.n >= 2) {
            double v; if (!mn_parse_time(lt.t[1].s, &v) || v <= 0) perr(&lt.t[1], "length must be a duration such as 2:30 or 90s");
            S->length_sec = v; tl_free(&lt); continue;
        }
        if (!strcmp(first, "mark") && lt.n >= 3) {                       /* mark NAME TIME : a timestamped marker */
            double v; if (!mn_parse_time(lt.t[2].s, &v)) perr(&lt.t[2], "bad timestamp '%s' (examples: 12.5s  1:02.5  0:01:02.500)", lt.t[2].s);
            const char *nm = lt.t[1].s;
            if (!isalpha((unsigned char)nm[0]) || strlen(nm) >= sizeof S->mk[0].name) perr(&lt.t[1], "marker names start with a letter and are at most 39 characters");
            for (int k = 0; k < S->nmk; k++) if (!strcmp(S->mk[k].name, nm)) perr(&lt.t[1], "marker '%s' is defined twice", nm);
            if (S->nmk == S->capmk) { S->capmk = S->capmk ? S->capmk * 2 : 8; S->mk = kc_realloc(S->mk, (size_t)S->capmk * sizeof(MnMarker)); }
            MnMarker *m = &S->mk[S->nmk++]; memset(m, 0, sizeof *m); snprintf(m->name, sizeof m->name, "%s", nm); m->timed = true; m->sec = v; m->track = -1;
            tl_free(&lt); continue;
        }
        if (!strcmp(first, "mark") && lt.n == 2 && cur < 0) perr(&lt.t[0], "mark needs a time outside a track:  mark chorus 1:30");
        if (!strcmp(first, "title") || !strcmp(first, "composer")) {
            char *dst = first[0] == 't' ? S->title : S->composer; size_t cap = first[0] == 't' ? sizeof S->title : sizeof S->composer;
            const char *r = lt.n > 1 ? ln + lt.t[1].col - 1 : "";
            while (is_ws(*r)) r++;
            size_t L = strlen(r); while (L && is_ws(r[L - 1])) L--;
            if (L >= 2 && ((r[0] == '"' && r[L - 1] == '"') || (r[0] == '\'' && r[L - 1] == '\''))) { r++; L -= 2; }
            if (L >= cap) L = cap - 1;
            memcpy(dst, r, L); dst[L] = 0;
            tl_free(&lt); continue;
        }
        if (!strcmp(first, "track")) {
            if (lt.n < 2) perr(&lt.t[0], "track needs a name, e.g.  track melody piano");
            const char *nm = lt.t[1].s; int found = -1;
            for (int k = 0; k < ntr; k++) if (!strcmp(tr[k].name, nm)) found = k;
            if (strlen(nm) >= sizeof tr[0].name) perr(&lt.t[1], "track name too long (max 39 characters)");
            if (found < 0) {
                if (ntr == captr) { captr = captr ? captr * 2 : 8; tr = kc_realloc(tr, (size_t)captr * sizeof *tr); }
                memset(&tr[ntr], 0, sizeof tr[ntr]);
                snprintf(tr[ntr].name, sizeof tr[ntr].name, "%s", nm);
                tr[ntr].init_fifths = dfifths; tr[ntr].init_minor = dminor;
                found = ntr++;
            }
            cur = found; start = 2;
            for (int k = 2; k < lt.n; k++) {
                const char *s = lt.t[k].s;
                if (strchr(s, '=')) tl_push(&tr[cur].toks, s, strlen(s), lt.t[k].line, lt.t[k].col);
                else if (mn_inst_find(s) >= 0) { char b[80]; snprintf(b, sizeof b, "inst=%s", s); tl_push(&tr[cur].toks, b, strlen(b), lt.t[k].line, lt.t[k].col); }
                else perr(&lt.t[k], "unknown track option '%s' (an instrument name or key=value such as vol=0.8 pan=-0.3 reverb=0.2 sample=NAME)", s);
            }
            tl_free(&lt); continue;
        }
        if (cur < 0) {
            bool allopt = true;
            for (int k = 0; k < lt.n; k++) if (!isalpha((unsigned char)lt.t[k].s[0]) || !strchr(lt.t[k].s, '=') || strpbrk(lt.t[k].s, "[{")) allopt = false;
            if (allopt) { for (int k = 0; k < lt.n; k++) tl_push(&hdr, lt.t[k].s, strlen(lt.t[k].s), lt.t[k].line, lt.t[k].col); tl_free(&lt); continue; }
        }
        /* header directives (before any track) */
        if (cur < 0 && lt.n >= 2 && is_opt_key(first) && !strchr(lt.t[0].s, '=')) {
            if (!strcmp(first, "tempo") || !strcmp(first, "bpm")) {
                double v = 0; if (!num_of(lt.t[1].s, &v) || v < 10 || v > 999) perr(&lt.t[1], "tempo must be a number from 10 to 999");
                tempo_push(S, 0, v); tempo_set = true; start = 2;
            } else if (!strcmp(first, "time")) {
                int a, b; char t; if (sscanf(lt.t[1].s, "%d/%d%c", &a, &b, &t) != 2 || a < 1 || a > 64 || (b != 1 && b != 2 && b != 4 && b != 8 && b != 16 && b != 32)) perr(&lt.t[1], "time signature must look like 4/4 or 6/8");
                S->ts_num = a; S->ts_den = b; start = 2;
            } else if (!strcmp(first, "key")) {
                int f; bool mi; if (!mn_parse_key(lt.t[1].s, &f, &mi)) perr(&lt.t[1], "unknown key '%s' (examples: G  Bb  F#m  Am)", lt.t[1].s);
                dfifths = f; dminor = mi; S->key_fifths = f; S->key_minor = mi; S->has_key = true; start = 2;
            }
        }
        if (start < lt.n && cur < 0) {   /* implicit default track */
            if (ntr == captr) { captr = captr ? captr * 2 : 8; tr = kc_realloc(tr, (size_t)captr * sizeof *tr); }
            memset(&tr[ntr], 0, sizeof tr[ntr]); snprintf(tr[ntr].name, sizeof tr[ntr].name, "main");
            tr[ntr].init_fifths = dfifths; tr[ntr].init_minor = dminor; cur = ntr++;
        }
        if (start < lt.n) push_line_tokens(&tr[cur].toks, &lt, start);
        tl_free(&lt);
    }
    if (curpat >= 0) kc_error_at(pos, "music_note.%s: pattern '%s' is missing its closing 'end'", who, pats[curpat].name);
    lv_free(&lv);
    if (!tempo_set) tempo_push(S, 0, 120.0);
    for (int k = 0; k < ntr; k++) gen_track(S, &tr[k], &hdr, pats, npats, strict);
    tl_free(&hdr);
    for (int k = 0; k < ntr; k++) tl_free(&tr[k].toks);
    for (int k = 0; k < npats; k++) tl_free(&pats[k].toks);
    free(tr); free(pats);
    /* tempo map: sort by beat (stable), last value at a beat wins, ensure a point at beat 0 */
    {
        MnTempo *a = S->tempo; int n = S->nt;
        for (int x = 1; x < n; x++) { MnTempo t = a[x]; int y = x - 1; while (y >= 0 && a[y].beat > t.beat) { a[y + 1] = a[y]; y--; } a[y + 1] = t; }
        int m = 0;
        for (int x = 0; x < n; x++) { if (m > 0 && fabs(a[m - 1].beat - a[x].beat) < 1e-9) a[m - 1] = a[x]; else a[m++] = a[x]; }
        S->nt = m;
        if (S->tempo[0].beat > 1e-9) { tempo_push(S, 0, S->tempo[0].bpm); qsort(S->tempo, (size_t)S->nt, sizeof(MnTempo), tempo_cmp); }
    }
    if (S->ntr == 0) kc_error_at(pos, "music_note.%s: the score contains no notes", who);
    return S;
}
static KcInterp *g_ip;
void mn_set_ip(KcInterp *ip) { g_ip = ip; }
MnScore *mn_score_parse_opts(KcValue text, KcValue opts, KcPos pos, const char *who) {
    MnParseOpts po; memset(&po, 0, sizeof po);
    po.strict = mn_opt_bool(opts, "strict_bars", false, pos, who); po.base = mn_opt_str(opts, "base", NULL, pos, who);
    po.file = mn_opt_str(opts, "file", NULL, pos, who); po.ip = g_ip;
    return mn_score_parse_ex(text.as.s->data, (size_t)text.as.s->len, &po, pos, who);
}
MnScore *mn_score_parse(const char *text, size_t len, bool strict, KcPos pos, const char *who) {
    MnParseOpts po; memset(&po, 0, sizeof po); po.strict = strict; po.ip = g_ip;
    return mn_score_parse_ex(text, len, &po, pos, who);
}

/* ============================================================ MIDI export */

typedef struct { uint8_t *d; size_t n, cap; } Buf;
static void b_put(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 256; b->d = kc_realloc(b->d, b->cap); }
    memcpy(b->d + b->n, p, n); b->n += n;
}
static void b_u8(Buf *b, int v) { uint8_t c = (uint8_t)v; b_put(b, &c, 1); }
static void b_u16(Buf *b, int v) { b_u8(b, v >> 8); b_u8(b, v); }
static void b_u32(Buf *b, uint32_t v) { b_u8(b, (int)(v >> 24)); b_u8(b, (int)(v >> 16)); b_u8(b, (int)(v >> 8)); b_u8(b, (int)v); }
static void b_vlq(Buf *b, uint32_t v) {
    uint8_t t[5]; int n = 0; t[n++] = v & 0x7f; while (v >>= 7) t[n++] = (uint8_t)((v & 0x7f) | 0x80);
    while (n) b_u8(b, t[--n]);
}
typedef struct { uint32_t tick; int order; uint8_t st, d1, d2; } MEv;
static int mev_cmp(const void *a, const void *b) {
    const MEv *x = a, *y = b;
    if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
    return x->order - y->order;
}
static void write_track(Buf *out, Buf *trk) { b_put(out, "MTrk", 4); b_u32(out, (uint32_t)trk->n); b_put(out, trk->d, trk->n); free(trk->d); }

int mn_midi_write(const MnScore *S, const char *path, KcPos pos, const char *who) {
    const int PPQ = 480;
    Buf out = { 0 };
    b_put(&out, "MThd", 4); b_u32(&out, 6); b_u16(&out, 1); b_u16(&out, S->ntr + 1); b_u16(&out, PPQ);
    /* conductor track */
    {
        Buf t = { 0 };
        if (S->title[0]) { b_vlq(&t, 0); b_u8(&t, 0xFF); b_u8(&t, 0x03); b_vlq(&t, (uint32_t)strlen(S->title)); b_put(&t, S->title, strlen(S->title)); }
        b_vlq(&t, 0); b_u8(&t, 0xFF); b_u8(&t, 0x58); b_u8(&t, 4); b_u8(&t, S->ts_num);
        int dd = 0; while ((1 << dd) < S->ts_den) dd++;
        b_u8(&t, dd); b_u8(&t, 24); b_u8(&t, 8);
        if (S->has_key) { b_vlq(&t, 0); b_u8(&t, 0xFF); b_u8(&t, 0x59); b_u8(&t, 2); b_u8(&t, (int8_t)S->key_fifths); b_u8(&t, S->key_minor ? 1 : 0); }
        uint32_t last = 0;
        for (int i = 0; i < S->nt; i++) {
            uint32_t tick = (uint32_t)llround(S->tempo[i].beat * PPQ), us = (uint32_t)llround(60000000.0 / S->tempo[i].bpm);
            b_vlq(&t, tick - last); last = tick;
            b_u8(&t, 0xFF); b_u8(&t, 0x51); b_u8(&t, 3); b_u8(&t, (int)(us >> 16)); b_u8(&t, (int)(us >> 8)); b_u8(&t, (int)us);
        }
        b_vlq(&t, 0); b_u8(&t, 0xFF); b_u8(&t, 0x2F); b_u8(&t, 0);
        write_track(&out, &t);
    }
    int melodic = 0;
    for (int ti = 0; ti < S->ntr; ti++) {
        const MnTrack *T = &S->tr[ti];
        int ch = melodic % 15; if (ch >= 9) ch++;       /* skip channel 10 (index 9), reserved for drums */
        bool has_pitched = false; for (int e = 0; e < T->n; e++) if (!T->ev[e].drum) { has_pitched = true; break; }
        if (has_pitched) melodic++;
        MEv *ev = kc_malloc(((size_t)T->n * 2 + 8) * sizeof(MEv)); int ne = 0;
        int lastprog = -1;
        ev[ne++] = (MEv){ 0, -3, (uint8_t)(0xB0 | ch), 7, (uint8_t)(T->vol * 100 > 127 ? 127 : T->vol * 100 < 0 ? 0 : (int)(T->vol * 100)) };
        ev[ne++] = (MEv){ 0, -3, (uint8_t)(0xB0 | ch), 10, (uint8_t)((T->pan + 1.0) * 63.5 < 0 ? 0 : (int)((T->pan + 1.0) * 63.5)) };
        for (int e = 0; e < T->n; e++) {
            const MnEv *x = &T->ev[e];
            if (x->smp) continue;                         /* audio samples have no MIDI equivalent */
            int c = x->drum ? 9 : ch;
            if (!x->drum) {
                int prog = mn_inst_program(S->patch[x->patch].inst);
                if (prog != lastprog) { ev[ne++] = (MEv){ (uint32_t)llround(x->beat * PPQ), -2, (uint8_t)(0xC0 | ch), (uint8_t)prog, 0 }; lastprog = prog; }
            }
            uint32_t on = (uint32_t)llround(x->beat * PPQ);
            uint32_t off = (uint32_t)llround((x->beat + x->dur * x->gate) * PPQ); if (off <= on) off = on + 1;
            int vel = (int)lround(x->vel); if (vel < 1) vel = 1; if (vel > 127) vel = 127;
            ev[ne++] = (MEv){ on, 1, (uint8_t)(0x90 | c), (uint8_t)x->midi, (uint8_t)vel };
            ev[ne++] = (MEv){ off, 0, (uint8_t)(0x80 | c), (uint8_t)x->midi, 0 };
        }
        qsort(ev, (size_t)ne, sizeof(MEv), mev_cmp);
        Buf t = { 0 };
        b_vlq(&t, 0); b_u8(&t, 0xFF); b_u8(&t, 0x03); b_vlq(&t, (uint32_t)strlen(T->name)); b_put(&t, T->name, strlen(T->name));
        uint32_t last = 0;
        for (int k = 0; k < ne; k++) {
            b_vlq(&t, ev[k].tick - last); last = ev[k].tick;
            b_u8(&t, ev[k].st); b_u8(&t, ev[k].d1);
            if ((ev[k].st & 0xF0) != 0xC0) b_u8(&t, ev[k].d2);
        }
        b_vlq(&t, 0); b_u8(&t, 0xFF); b_u8(&t, 0x2F); b_u8(&t, 0);
        write_track(&out, &t);
        free(ev);
    }
    FILE *f = fopen(path, "wb");
    if (!f) { free(out.d); kc_error_at(pos, "music_note.%s: cannot open '%s' for writing: %s", who, path, strerror(errno)); }
    size_t w = fwrite(out.d, 1, out.n, f);
    fclose(f);
    int total = (int)out.n; free(out.d);
    if (w != (size_t)total) kc_error_at(pos, "music_note.%s: write to '%s' failed", who, path);
    return total;
}

/* ============================================================ API */

static MnScore *src_score(KcValue v, bool *owned, KcPos pos, const char *who) {
    if (v.type == VAL_STRING) { *owned = true; return mn_score_parse(v.as.s->data, (size_t)v.as.s->len, false, pos, who); }
    *owned = false; return mn_score_arg(v, pos, who);
}

/* parse(notation[, options[, sample_names[], sample_audios[]]]) -> score */
static KcValue sc_parse(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    g_ip = ip;
    mn_need(n, 1, 4, pos, "parse(notation[, \"strict_bars base=dir\"[, sample_names[], sample_audios[]]]) -> score");
    const char *t = mn_str(a[0], pos, "parse", "notation");
    KcValue o = n > 1 ? a[1] : kc_null();
    MnParseOpts po; memset(&po, 0, sizeof po);
    po.strict = mn_opt_bool(o, "strict_bars", false, pos, "parse"); po.base = mn_opt_str(o, "base", NULL, pos, "parse"); po.file = mn_opt_str(o, "file", NULL, pos, "parse"); po.ip = ip;
    if (n > 2) {
        if (n < 4 || a[2].type != VAL_ARRAY || a[3].type != VAL_ARRAY || a[2].as.arr->count != a[3].as.arr->count) kc_error_at(pos, "music_note.parse: pass sample_names[] and sample_audios[] together, with the same length");
        po.nnames = a[2].as.arr->count;
        po.names = kc_malloc(((size_t)po.nnames + 1) * sizeof *po.names); po.audios = kc_malloc(((size_t)po.nnames + 1) * sizeof *po.audios);
        for (int i = 0; i < po.nnames; i++) { po.names[i] = mn_str(a[2].as.arr->items[i], pos, "parse", "sample name"); po.audios[i] = mn_audio_arg(a[3].as.arr->items[i], pos, "parse"); }
    }
    MnScore *S = mn_score_parse_ex(t, (size_t)a[0].as.s->len, &po, pos, "parse");
    free(po.names); free(po.audios);
    return mn_score_wrap(S);
}
/* parse_file(path[, options]) -> score — relative sample / include paths resolve against the file's folder */
static KcValue sc_parse_file(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    g_ip = ip;
    kc_forbid_in_comptime(ip, pos, "music_note.parse_file");
    mn_need(n, 1, 2, pos, "parse_file(path[, \"strict_bars\"]) -> score");
    const char *path = mn_str(a[0], pos, "parse_file", "path");
    size_t len; uint8_t *b = mn_read_file(path, &len, pos, "parse_file");
    char dir[200]; path_dir(path, dir, sizeof dir);
    MnParseOpts po; memset(&po, 0, sizeof po);
    po.strict = n > 1 && mn_opt_bool(a[1], "strict_bars", false, pos, "parse_file"); po.base = dir; po.file = path; po.ip = ip;
    MnScore *S = mn_score_parse_ex((const char *)b, len, &po, pos, "parse_file");
    free(b);
    return mn_score_wrap(S);
}

static const char *key_text(const MnScore *S, char *b, size_t cap) {
    static const char *MAJ[15] = { "Cb", "Gb", "Db", "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#" };
    static const char *MIN[15] = { "Ab", "Eb", "Bb", "F", "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#" };
    int f = S->key_fifths + 7; if (f < 0) f = 0; if (f > 14) f = 14;
    snprintf(b, cap, "%s%s", S->key_minor ? MIN[f] : MAJ[f], S->key_minor ? "m" : "");
    return b;
}

/* score_info(score | notation) -> {title, composer, tempo, time, key, beats, seconds, notes, tracks:[{name, instrument, notes, beats, seconds, drums}]} */
static KcValue sc_info(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    g_ip = ip;
    mn_need(n, 1, 1, pos, "score_info(score | notation) -> object");
    bool owned; MnScore *S = src_score(a[0], &owned, pos, "score_info");
    KcValue o = kc_object_new(), tracks = kc_array_new(); int total = 0;
    for (int i = 0; i < S->ntr; i++) {
        const MnTrack *T = &S->tr[i]; KcValue t = kc_object_new(); double end = 0;
        for (int e = 0; e < T->n; e++) { double x = T->ev[e].beat + T->ev[e].dur; if (x > end) end = x; }
        SETK(t, "name", kc_string(T->name)); SETK(t, "instrument", kc_string(mn_inst_name(S->patch[T->first_patch].inst)));
        SETK(t, "notes", kc_int(T->n)); SETK(t, "beats", kc_float(end)); SETK(t, "seconds", kc_float(mn_beat_to_sec(S, end)));
        SETK(t, "volume", kc_float(T->vol)); SETK(t, "pan", kc_float(T->pan)); SETK(t, "drums", kc_bool(T->drums));
        kc_array_push(tracks, t); total += T->n;
    }
    char kb[16];
    SETK(o, "title", kc_string(S->title)); SETK(o, "composer", kc_string(S->composer));
    SETK(o, "tempo", kc_float(S->tempo[0].bpm)); SETK(o, "tempo_changes", kc_int(S->nt - 1));
    char tb[16]; snprintf(tb, sizeof tb, "%d/%d", S->ts_num, S->ts_den);
    SETK(o, "time", kc_string(tb)); SETK(o, "key", kc_string(S->has_key ? key_text(S, kb, sizeof kb) : ""));
    SETK(o, "beats", kc_float(S->total_beats)); SETK(o, "seconds", kc_float(mn_beat_to_sec(S, S->total_beats)));
    SETK(o, "notes", kc_int(total)); SETK(o, "tracks", tracks);
    SETK(o, "samples", kc_int(S->nsmp)); SETK(o, "markers", kc_int(S->nmk)); SETK(o, "length", kc_float(S->length_sec));
    if (owned) mn_score_free(S);
    return o;
}

/* events(score | notation[, track]) -> [{track, start, duration, beat, beats, midi, note, velocity, drum}] (seconds) */
static KcValue sc_events(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    g_ip = ip;
    mn_need(n, 1, 2, pos, "events(score | notation[, track_name]) -> object[] (start in seconds, timecode, sample name when sample-backed)");
    bool owned; MnScore *S = src_score(a[0], &owned, pos, "events");
    const char *only = n > 1 ? mn_str(a[1], pos, "events", "track_name") : NULL;
    KcValue out = kc_array_new();
    for (int i = 0; i < S->ntr; i++) {
        const MnTrack *T = &S->tr[i]; if (only && strcmp(only, T->name)) continue;
        for (int e = 0; e < T->n; e++) {
            const MnEv *x = &T->ev[e]; KcValue o = kc_object_new(); char nb[16];
            double s0 = mn_beat_to_sec(S, x->beat), s1 = mn_beat_to_sec(S, x->beat + x->dur);
            SETK(o, "track", kc_string(T->name)); SETK(o, "start", kc_float(s0 + x->off)); SETK(o, "duration", kc_float(s1 - s0));
            SETK(o, "beat", kc_float(x->beat)); SETK(o, "beats", kc_float(x->dur)); SETK(o, "midi", kc_int(x->midi));
            SETK(o, "note", kc_string(x->drum ? "" : mn_midi_name(x->midi, false, nb)));
            if (x->drum) SETK(o, "note", kc_string(mn_drum_name(x->drum)));
            SETK(o, "velocity", kc_int((int64_t)lround(x->vel))); SETK(o, "drum", kc_bool(x->drum != 0));
            { char tcb[32]; SETK(o, "timecode", kc_string(mn_timecode(s0 + x->off, 0, tcb))); }
            { int si = x->smp ? x->smp : S->patch[x->patch].sample; SETK(o, "sample", kc_string(si > 0 ? S->smp[si - 1].name : "")); }
            if (x->smp) SETK(o, "note", kc_string(""));
            kc_array_push(out, o);
        }
    }
    if (owned) mn_score_free(S);
    return out;
}

/* sample_list(score | notation) -> [{name, source, seconds, rate, channels, gain, root, bpm, oneshot, reverse}] */
static KcValue sc_samples(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    g_ip = ip;
    mn_need(n, 1, 1, pos, "sample_list(score | notation) -> object[]");
    bool owned; MnScore *S = src_score(a[0], &owned, pos, "sample_list");
    KcValue out = kc_array_new();
    for (int i = 0; i < S->nsmp; i++) {
        const MnSample *m = &S->smp[i]; KcValue o = kc_object_new(); char nb[16];
        SETK(o, "name", kc_string(m->name)); SETK(o, "source", kc_string(m->src));
        SETK(o, "seconds", kc_float((double)m->a->frames / m->a->rate)); SETK(o, "rate", kc_int(m->a->rate)); SETK(o, "channels", kc_int(m->a->ch));
        SETK(o, "gain", kc_float(m->gain_db)); SETK(o, "root", kc_string(mn_midi_name(m->root, false, nb))); SETK(o, "bpm", kc_float(m->bpm));
        SETK(o, "oneshot", kc_bool(m->oneshot)); SETK(o, "reverse", kc_bool(m->reverse));
        kc_array_push(out, o);
    }
    if (owned) mn_score_free(S);
    return out;
}
/* markers(score | notation) -> [{name, time, timecode, beat, track}] sorted by time */
static int mk_cmp(const void *a, const void *b) { double x = ((const double *)a)[0], y = ((const double *)b)[0]; return x < y ? -1 : x > y ? 1 : 0; }
static KcValue sc_markers(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    g_ip = ip;
    mn_need(n, 1, 1, pos, "markers(score | notation) -> object[]   (named timestamps: mark=name in a track, or `mark name 1:30`)");
    bool owned; MnScore *S = src_score(a[0], &owned, pos, "markers");
    double *key = kc_malloc(((size_t)S->nmk + 1) * 2 * sizeof(double));
    for (int i = 0; i < S->nmk; i++) { key[2 * i] = S->mk[i].timed ? S->mk[i].sec : mn_beat_to_sec(S, S->mk[i].beat); key[2 * i + 1] = i; }
    qsort(key, (size_t)S->nmk, 2 * sizeof(double), mk_cmp);
    KcValue out = kc_array_new();
    for (int i = 0; i < S->nmk; i++) {
        const MnMarker *m = &S->mk[(int)key[2 * i + 1]]; KcValue o = kc_object_new(); char tcb[32];
        SETK(o, "name", kc_string(m->name)); SETK(o, "time", kc_float(key[2 * i])); SETK(o, "timecode", kc_string(mn_timecode(key[2 * i], 0, tcb)));
        SETK(o, "beat", kc_float(m->timed ? mn_sec_to_beat(S, m->sec) : m->beat)); SETK(o, "track", kc_string(m->track >= 0 && m->track < S->ntr ? S->tr[m->track].name : ""));
        kc_array_push(out, o);
    }
    free(key);
    if (owned) mn_score_free(S);
    return out;
}
/* timecode(seconds[, "fps=30"]) -> "HH:MM:SS.mmm" (or HH:MM:SS:FF with fps) */
static KcValue sc_timecode(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 2, pos, "timecode(seconds[, \"fps=30\"]) -> string");
    int fps = n > 1 ? (int)mn_opt_int(a[1], "fps", 0, pos, "timecode") : 0;
    if (fps < 0 || fps > 240) kc_error_at(pos, "music_note.timecode: fps must be 0..240");
    char b[40]; return kc_string(mn_timecode(mn_num(a[0], pos, "timecode", "seconds"), fps, b));
}
/* time_seconds(text) -> seconds    12.5  500ms  1:02.5  0:01:02.500  1m30s */
static KcValue sc_time_seconds(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    mn_need(n, 1, 1, pos, "time_seconds(text) -> float   (12.5  500ms  1:02.5  0:01:02.500  1m30s)");
    if (mn_is_num(a[0])) return kc_float(mn_num(a[0], pos, "time_seconds", "text"));
    double v; const char *t = mn_str(a[0], pos, "time_seconds", "text");
    if (!mn_parse_time(t, &v)) kc_error_at(pos, "music_note.time_seconds: cannot read '%s' as a time (examples: 12.5s 500ms 1:02.5 0:01:02.500 1m30s)", t);
    return kc_float(v);
}
/* embed(audio | wav_path[, "name=vox bits=16 gain=-3 root=C4 bpm=95 oneshot reverse cut start end fadein fadeout"]) -> KCM text */
static KcValue sc_embed(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    g_ip = ip;
    mn_need(n, 1, 2, pos, "embed(audio | wav_path[, options]) -> text   (a `sample NAME ... data` / `end` block to paste into a .kcm score)");
    KcValue o = n > 1 ? a[1] : kc_null();
    const char *name = mn_opt_str(o, "name", "sample1", pos, "embed");
    if (!sample_name_ok(name)) kc_error_at(pos, "music_note.embed: bad sample name '%s' (letters, digits, _ and -)", name);
    uint8_t *img; size_t len; bool own = true;
    if (a[0].type == VAL_STRING) {
        kc_forbid_in_comptime(ip, pos, "music_note.embed");
        img = mn_read_file(a[0].as.s->data, &len, pos, "embed");
        MnAudio *chk = mn_wav_decode(img, len, a[0].as.s->data, pos, "embed"); mn_audio_free(chk);       /* validate it really is a WAV */
    } else {
        MnAudio *au = mn_audio_arg(a[0], pos, "embed"); int bits = (int)mn_opt_int(o, "bits", 16, pos, "embed");
        if (bits != 8 && bits != 16 && bits != 24) kc_error_at(pos, "music_note.embed: bits must be 8, 16 or 24");
        img = mn_wav_encode(au, bits, true, &len, pos, "embed");
    }
    (void)own;
    if (len > (size_t)64 << 20) { free(img); kc_error_at(pos, "music_note.embed: audio is larger than 64 MiB; reference the file with  sample name \"file.wav\"  instead"); }
    char *b64 = mn_b64_encode(img, len, (int)mn_opt_int(o, "wrap", 76, pos, "embed")); free(img);
    size_t cap = strlen(b64) + 256; char *out = kc_malloc(cap); size_t k = (size_t)snprintf(out, cap, "sample %s", name);
    static const char *NUMK[] = { "gain", "bpm", "start", "end", "fadein", "fadeout" };
    for (size_t i = 0; i < sizeof NUMK / sizeof NUMK[0]; i++) { KcValue *v = mn_opt_get(o, NUMK[i]); if (v && v->type != VAL_NULL) k += (size_t)snprintf(out + k, cap - k, " %s=%g", NUMK[i], mn_num(*v, pos, "embed", NUMK[i])); }
    { const char *rt = mn_opt_str(o, "root", NULL, pos, "embed"); if (rt) k += (size_t)snprintf(out + k, cap - k, " root=%s", rt); }
    static const char *FLAGK[] = { "reverse", "oneshot", "cut" };
    for (size_t i = 0; i < sizeof FLAGK / sizeof FLAGK[0]; i++) if (mn_opt_bool(o, FLAGK[i], false, pos, "embed")) k += (size_t)snprintf(out + k, cap - k, " %s", FLAGK[i]);
    k += (size_t)snprintf(out + k, cap - k, " data\n");
    size_t bl = strlen(b64); if (k + bl + 8 > cap) { cap = k + bl + 8; out = kc_realloc(out, cap); }
    memcpy(out + k, b64, bl); k += bl; memcpy(out + k, "\nend\n", 6); k += 6; out[k] = 0;
    free(b64);
    KcValue v = kc_string(out); free(out);
    return v;
}

/* to_midi(score | notation, path) -> bytes written */
static KcValue sc_to_midi(KcInterp *ip, KcValue *a, int n, KcPos pos) {
    g_ip = ip;
    kc_forbid_in_comptime(ip, pos, "music_note.to_midi");
    mn_need(n, 2, 2, pos, "to_midi(score | notation, path) -> int");
    bool owned; MnScore *S = src_score(a[0], &owned, pos, "to_midi");
    int w = mn_midi_write(S, mn_str(a[1], pos, "to_midi", "path"), pos, "to_midi");
    if (owned) mn_score_free(S);
    return kc_int(w);
}

KcModuleEntry mn_score_entries[] = {
    { "parse", sc_parse }, { "parse_file", sc_parse_file }, { "score_info", sc_info }, { "events", sc_events }, { "to_midi", sc_to_midi },
    { "sample_list", sc_samples }, { "markers", sc_markers }, { "timecode", sc_timecode }, { "time_seconds", sc_time_seconds }, { "embed", sc_embed },
};
const int mn_score_count = (int)(sizeof mn_score_entries / sizeof mn_score_entries[0]);
